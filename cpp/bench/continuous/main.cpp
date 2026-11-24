#include "faircross/continuous/matching_engine.hpp"
#include "faircross/simulator/rng.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <sys/resource.h>
#include <unordered_map>
#include <vector>

namespace lob = faircross::continuous;

namespace {

using Clock = std::chrono::steady_clock;
using lob::Event;
using lob::MatchingEngine;
using lob::MsgType;
using lob::OrderId;
using lob::PriceTicks;
using lob::Side;
using lob::Status;
using lob::TimestampNs;

constexpr std::size_t default_levels = 64;
constexpr std::size_t max_levels = 512;
constexpr std::size_t slots_per_level = 4;

Event event(MsgType type, std::uint64_t id, TimestampNs timestamp, std::uint64_t qty,
            std::optional<PriceTicks> price = std::nullopt, std::optional<Side> side = std::nullopt) {
    return std::get<Event>(Event::create(type, id, timestamp, qty, price, side));
}

struct Slot {
    Side side;
    PriceTicks base_price;
    std::uint64_t id = 0;
    std::uint64_t remaining = 0;
};

struct DepthStats {
    std::uint64_t sum = 0;
    std::size_t maximum = 0;

    void sample(std::size_t size) {
        sum += size;
        maximum = std::max(maximum, size);
    }
};

class Workload {
public:
    explicit Workload(std::uint64_t seed, std::size_t levels)
        : rng_(seed), slots_(2 * levels * slots_per_level) {
        const auto slots_per_side = slots_.size() / 2;
        for (std::size_t i = 0; i < slots_.size(); ++i) {
            const bool buy = i < slots_per_side;
            const auto distance = static_cast<PriceTicks>(i % slots_per_side / slots_per_level);
            slots_[i] = {buy ? Side::Buy : Side::Sell, buy ? 990 - distance : 1010 + distance};
        }
    }

    Event next(TimestampNs timestamp) {
        const auto roll = rng_.next_u64() % 100;
        if (roll < 8) {
            const auto side = rng_.next_u64() % 2 ? Side::Buy : Side::Sell;
            const auto size = rng_.gen_range_u64(1, 12);
            return event(MsgType::Market, next_id_++, timestamp, size, std::nullopt, side);
        }
        if (roll < 13 && retired_) {
            return event(MsgType::Cancel, retired_, timestamp, 1);
        }

        auto& slot = slots_[rng_.next_u64() % slots_.size()];
        if (!slot.remaining) {
            slot.id = next_id_++;
            active_.emplace(slot.id, &slot);
            return event(MsgType::New, slot.id, timestamp, rng_.gen_range_u64(1, 8),
                         slot.base_price, slot.side);
        }
        if (roll < 45) {
            return event(MsgType::Cancel, slot.id, timestamp, rng_.gen_range_u64(1, slot.remaining));
        }
        const auto shift = static_cast<PriceTicks>(rng_.next_u64() % 2);
        return event(MsgType::Modify, slot.id, timestamp, rng_.gen_range_u64(1, 8),
                     slot.base_price + shift);
    }

    void consume(const Event& input, const lob::EventResult& result) {
        ++event_counts[static_cast<std::size_t>(input.type())];
        if (result.status == Status::AlreadyClosed && input.id() == OrderId(retired_)) {
            ++stale_cancels;
            return;
        }
        if (result.status != Status::Ok) throw std::runtime_error("unexpected workload rejection");
        for (const auto& fill : result.fills) {
            const auto maker = active_.find(fill.maker_id.as_raw());
            if (maker == active_.end() || maker->second->remaining < fill.quantity.as_raw()) {
                throw std::runtime_error("unexpected workload fill");
            }
            update(maker, maker->second->remaining - fill.quantity.as_raw());
            ++fills;
            lots_traded += fill.quantity.as_raw();
        }
        if (input.type() == MsgType::Market) {
            retired_ = input.id().as_raw();
            ++retired_ids;
        } else {
            const auto slot = active_.find(input.id().as_raw());
            if (slot == active_.end()) throw std::runtime_error("unknown live slot");
            update(slot, result.remaining.as_raw());
        }
    }

    std::array<std::uint64_t, 4> event_counts{};
    std::uint64_t fills = 0;
    std::uint64_t lots_traded = 0;
    std::uint64_t retired_ids = 0;
    std::uint64_t stale_cancels = 0;

private:
    using Active = std::unordered_map<std::uint64_t, Slot*>;

    void update(Active::iterator slot, std::uint64_t remaining) {
        slot->second->remaining = remaining;
        if (!remaining) {
            retired_ = slot->first;
            active_.erase(slot);
            ++retired_ids;
        }
    }

    faircross::DeterministicRng rng_;
    std::vector<Slot> slots_;
    Active active_;
    std::uint64_t next_id_ = 1;
    std::uint64_t retired_ = 0;
};

std::uint64_t read_integer(const char* arg) {
    const std::string_view text(arg);
    std::uint64_t value = 0;
    const auto* end = text.data() + text.size();
    const auto [pos, error] = std::from_chars(text.data(), end, value);
    if (error != std::errc{} || pos != end || value == 0) {
        throw std::invalid_argument("events and seed must be positive integers");
    }
    return value;
}

void write_latencies(std::vector<std::int64_t>& samples) {
    std::cout << "{\"samples\":" << samples.size();
    if (!samples.empty()) {
        std::sort(samples.begin(), samples.end());
        const auto percentile = [&](std::size_t thousandths) {
            return samples[(samples.size() * thousandths + 999) / 1000 - 1];
        };
        std::cout << ",\"p50\":" << percentile(500) << ",\"p95\":" << percentile(950)
                  << ",\"p99\":" << percentile(990) << ",\"p999\":" << percentile(999)
                  << ",\"max\":" << samples.back();
    }
    std::cout << '}';
}

void run(std::uint64_t events, std::uint64_t seed, std::size_t levels) {
    Workload workload(seed, levels);
    const auto total_slots = 2 * levels * slots_per_level;
    MatchingEngine engine;
    faircross::DeterministicRng sampler(seed ^ 0x1c4d8c109a67ddf1ULL);
    std::vector<std::int64_t> latency;
    std::array<std::vector<std::int64_t>, 4> by_type;
    latency.reserve(events / 100);
    DepthStats active_orders, bid_levels, ask_levels;
    std::size_t level_samples = 0;

    const auto start = Clock::now();
    for (std::uint64_t i = 0; i < events; ++i) {
        const auto input = workload.next(i);
        const bool sample = (sampler.next_u64() & 127u) == 0 || i == 0;
        const auto before = sample ? Clock::now() : Clock::time_point{};
        const auto result = engine.submit(input);
        if (sample) {
            const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                Clock::now() - before).count();
            latency.push_back(ns);
            by_type[static_cast<std::size_t>(input.type())].push_back(ns);
        }
        workload.consume(input, result);
        active_orders.sample(engine.book().active_orders());
        if (i % 1024 == 0) {
            bid_levels.sample(engine.book().depth(Side::Buy, total_slots).size());
            ask_levels.sample(engine.book().depth(Side::Sell, total_slots).size());
            ++level_samples;
        }
    }

    const auto elapsed = std::chrono::duration<double>(Clock::now() - start).count();

    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0) throw std::runtime_error("getrusage failed");
#ifdef __APPLE__
    const auto peak_bytes = static_cast<std::uint64_t>(usage.ru_maxrss);
#else
    const auto peak_bytes = static_cast<std::uint64_t>(usage.ru_maxrss) * 1024;
#endif

    std::cout << std::setprecision(12) << "{\"events\":" << events << ",\"seed\":" << seed
              << ",\"elapsed_seconds\":" << elapsed
              << ",\"events_per_second\":" << static_cast<double>(events) / elapsed
              << ",\"peak_rss_bytes\":" << peak_bytes
              << ",\"mix\":{\"new\":" << workload.event_counts[0]
              << ",\"cancel\":" << workload.event_counts[1]
              << ",\"modify\":" << workload.event_counts[2]
              << ",\"market\":" << workload.event_counts[3]
              << ",\"already_closed_cancels\":" << workload.stale_cancels
              << ",\"retired_ids\":" << workload.retired_ids
              << ",\"fills\":" << workload.fills << ",\"lots_traded\":" << workload.lots_traded
              << "},\"depth\":{\"slots\":" << total_slots
              << ",\"level_samples\":" << level_samples
              << ",\"mean_orders\":" << static_cast<double>(active_orders.sum) / static_cast<double>(events)
              << ",\"max_orders\":" << active_orders.maximum
              << ",\"mean_bid_levels\":" << static_cast<double>(bid_levels.sum) / static_cast<double>(level_samples)
              << ",\"mean_ask_levels\":" << static_cast<double>(ask_levels.sum) / static_cast<double>(level_samples)
              << ",\"max_bid_levels\":" << bid_levels.maximum
              << ",\"max_ask_levels\":" << ask_levels.maximum
              << "},\"matching_latency_ns\":";
    write_latencies(latency);
    std::cout << ",\"matching_latency_by_type_ns\":{";
    const std::array names = {"new", "cancel", "modify", "market"};
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (i > 0) std::cout << ',';
        std::cout << '"' << names[i] << "\":";
        write_latencies(by_type[i]);
    }
    std::cout << "}}\n";
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 3 && argc != 4) {
            throw std::invalid_argument("usage: faircross_lob_bench EVENTS SEED [LEVELS]");
        }
        const auto levels = argc == 4 ? read_integer(argv[3]) : default_levels;
        if (levels > max_levels) throw std::invalid_argument("LEVELS must be in [1, 512]");
        run(read_integer(argv[1]), read_integer(argv[2]), static_cast<std::size_t>(levels));
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
