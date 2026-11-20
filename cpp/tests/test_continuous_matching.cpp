#include "faircross/continuous/matching_engine.hpp"
#include "property.hpp"
#include "test_framework.hpp"

#include <algorithm>
#include <limits>
#include <optional>
#include <unordered_map>
#include <vector>

namespace lob = faircross::continuous;

namespace {

using lob::Event;
using lob::MatchingEngine;
using lob::MsgType;
using lob::OrderId;
using lob::PriceTicks;
using lob::Qty;
using lob::Side;
using lob::Status;
using lob::Trade;

template <typename... Fields>
Event make_event(Fields... fields) {
    return std::get<Event>(Event::create(fields...));
}

Trade trade(std::uint64_t maker, std::uint64_t taker, Side side, PriceTicks price,
            std::uint64_t qty, lob::TimestampNs timestamp) {
    return {OrderId(maker), OrderId(taker), side, price, Qty(qty), timestamp};
}

} // namespace

TEST_CASE(continuous_matches_resting_prices_in_price_time_order) {
    MatchingEngine engine;
    for (const auto& event : {make_event(MsgType::New, 10, 0, 7, 102, Side::Sell),
                              make_event(MsgType::New, 5, 1, 4, 101, Side::Sell),
                              make_event(MsgType::New, 1, 1, 2, 101, Side::Sell),
                              make_event(MsgType::New, 2, 2, 3, 100, Side::Buy)}) {
        REQUIRE_EQ(engine.submit(event).status, Status::Ok);
    }

    const auto result = engine.submit(make_event(MsgType::New, 3, 3, 9, 102, Side::Buy));
    REQUIRE_EQ(result.fills, (std::vector{trade(5, 3, Side::Buy, 101, 4, 3),
                                         trade(1, 3, Side::Buy, 101, 2, 3),
                                         trade(10, 3, Side::Buy, 102, 3, 3)}));
    REQUIRE_EQ(result.remaining, Qty::zero());
    REQUIRE_EQ(engine.book().depth(Side::Sell, 1), (std::vector{lob::DepthLevel{102, Qty(4), 1}}));

    const auto sell = engine.submit(make_event(MsgType::Market, 4, 4, 8, std::nullopt, Side::Sell));
    REQUIRE_EQ(sell.fills, (std::vector{trade(2, 4, Side::Sell, 100, 3, 4)}));
    REQUIRE_EQ(sell.remaining, Qty::zero());
    REQUIRE_EQ(sell.canceled, Qty(5));
    const auto empty = engine.submit(make_event(MsgType::Market, 6, 5, 4, std::nullopt, Side::Sell));
    REQUIRE(empty.fills.empty());
    REQUIRE_EQ(empty.canceled, Qty(4));
    REQUIRE_EQ(engine.submit(make_event(MsgType::Cancel, 6, 6, 1)).status, Status::AlreadyClosed);
    REQUIRE_EQ(engine.submit(make_event(MsgType::Modify, 5, 6, 1, 101)).status, Status::AlreadyClosed);
    REQUIRE_EQ(engine.submit(make_event(MsgType::Cancel, 20, 6, 1)).status, Status::NotFound);
    REQUIRE_EQ(engine.submit(make_event(MsgType::New, 4, 6, 1, 101, Side::Sell)).status,
               Status::DuplicateOrderId);
    REQUIRE_EQ(engine.submit(make_event(MsgType::Market, 10, 6, 1, std::nullopt, Side::Buy)).status,
               Status::DuplicateOrderId);
}

TEST_CASE(continuous_modify_priority_and_partial_cancel) {
    MatchingEngine engine;
    const auto submit = [&](const Event& event) { return engine.submit(event); };
    REQUIRE_EQ(submit(make_event(MsgType::New, 1, 1, 7, 99, Side::Buy)).status, Status::Ok);
    REQUIRE_EQ(submit(make_event(MsgType::New, 2, 2, 5, 99, Side::Buy)).status, Status::Ok);
    REQUIRE_EQ(submit(make_event(MsgType::Modify, 1, 3, 6, 99)).remaining, Qty(6));
    REQUIRE_EQ(submit(make_event(MsgType::Modify, 1, 4, 6, 99)).remaining, Qty(6));
    REQUIRE_EQ(submit(make_event(MsgType::Cancel, 1, 5, 1)).canceled, Qty(1));
    REQUIRE_EQ(engine.book().find(OrderId(1))->timestamp, 1U);
    REQUIRE_EQ(engine.book().queue_position(OrderId(2))->lots_ahead, Qty(5));
    REQUIRE_EQ(engine.book().queue_position(OrderId(2))->orders_ahead, 1U);
    REQUIRE_EQ(submit(make_event(MsgType::Modify, 1, 6, 6, 99)).status, Status::Ok);
    REQUIRE_EQ(engine.book().queue(Side::Buy, 99), (std::vector{OrderId(2), OrderId(1)}));

    REQUIRE_EQ(submit(make_event(MsgType::New, 3, 7, 5, 102, Side::Sell)).status, Status::Ok);
    const auto replaced = submit(make_event(MsgType::Modify, 1, 8, 7, 102));
    REQUIRE_EQ(replaced.fills, (std::vector{trade(3, 1, Side::Buy, 102, 5, 8)}));
    REQUIRE_EQ(replaced.remaining, Qty(2));
    REQUIRE_EQ(engine.book().queue(Side::Buy, 99), (std::vector{OrderId(2)}));
    REQUIRE_EQ(engine.book().queue(Side::Buy, 102), (std::vector{OrderId(1)}));
    REQUIRE_EQ(engine.book().find(OrderId(1))->timestamp, 8U);
    REQUIRE_EQ(submit(make_event(MsgType::Cancel, 1, 9, 2)).status, Status::Ok);
    REQUIRE(!engine.book().queue_position(OrderId(1)));
}

TEST_CASE(continuous_match_rejections_preserve_book_and_sparse_ids) {
    MatchingEngine engine;
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    REQUIRE_EQ(engine.submit(make_event(MsgType::New, 0, 1, maximum, 100, Side::Sell)).status,
               Status::Ok);
    REQUIRE_EQ(engine.submit(make_event(MsgType::New, 62, 2, 3, 101, Side::Sell)).status, Status::Ok);
    REQUIRE_EQ(engine.submit(make_event(MsgType::New, maximum, 3, 3, 99, Side::Buy)).status, Status::Ok);
    const auto before = engine.book();
    REQUIRE_EQ(engine.submit(make_event(MsgType::New, 63, 4, 1, 100, Side::Sell)).status,
               Status::Overflow);
    REQUIRE_EQ(engine.submit(make_event(MsgType::Modify, 62, 4, 1, 100)).status, Status::Overflow);
    REQUIRE_EQ(engine.submit(make_event(MsgType::Cancel, maximum, 4, 4)).status, Status::InvalidQuantity);
    REQUIRE_EQ(engine.book(), before);

    const auto cross = engine.submit(make_event(MsgType::Market, 63, 5, maximum, std::nullopt, Side::Buy));
    REQUIRE_EQ(cross.fills, (std::vector{trade(0, 63, Side::Buy, 100, maximum, 5)}));
    REQUIRE_EQ(engine.submit(make_event(MsgType::Cancel, 0, 6, 1)).status, Status::AlreadyClosed);
    REQUIRE_EQ(engine.submit(make_event(MsgType::Cancel, 61, 6, 1)).status, Status::NotFound);
    REQUIRE_EQ(engine.submit(make_event(MsgType::Cancel, 63, 6, 1)).status, Status::AlreadyClosed);
    REQUIRE_EQ(engine.submit(make_event(MsgType::Cancel, maximum, 6, 3)).status, Status::Ok);
    REQUIRE_EQ(engine.submit(make_event(MsgType::Cancel, maximum - 1, 6, 1)).status, Status::NotFound);
    REQUIRE_EQ(engine.submit(make_event(MsgType::Cancel, maximum, 6, 1)).status, Status::AlreadyClosed);
}

TEST_CASE(continuous_seeded_events_conserve_both_sides_and_reproduce) {
    using namespace faircross::property;
    const auto seed = base_seed();
    const auto cases = case_count(120);
    for (std::size_t run = 0; run < cases; ++run) {
        Rng rng(seed + run);
        MatchingEngine engine;
        MatchingEngine duplicate;
        std::unordered_map<std::uint64_t, std::pair<Side, std::uint64_t>> live;
        for (std::uint64_t i = 1; i <= 160; ++i) {
            try {
                const auto type = static_cast<MsgType>(rng.in_range(0, 3));
                const bool fresh = type == MsgType::New || type == MsgType::Market;
                const auto id = fresh ? i : rng.in_range(i / 2, i);
                const auto old = live.find(id);
                const auto side = fresh || old == live.end()
                    ? (rng.coin() ? Side::Buy : Side::Sell) : old->second.first;
                const auto qty = rng.in_range(1, 12);
                const std::optional<PriceTicks> price = type == MsgType::New || type == MsgType::Modify
                    ? std::optional(static_cast<PriceTicks>(rng.in_range(98, 102))) : std::nullopt;
                const auto event = make_event(type, id, i / 2, qty, price,
                                              fresh ? std::optional(side) : std::nullopt);

                const auto result = engine.submit(event);
                REQUIRE_EQ(result, duplicate.submit(event));
                REQUIRE_EQ(engine.book(), duplicate.book());
                if (result.status == Status::Ok) {
                    std::uint64_t traded = 0;
                    for (const auto& fill : result.fills) {
                        REQUIRE_EQ(fill.taker_id, OrderId(id));
                        REQUIRE_EQ(fill.taker_side, side);
                        auto maker = live.find(fill.maker_id.as_raw());
                        REQUIRE(maker != live.end());
                        REQUIRE_NE(maker->second.first, side);
                        REQUIRE(fill.quantity.as_raw() <= maker->second.second);
                        maker->second.second -= fill.quantity.as_raw();
                        traded += fill.quantity.as_raw();
                        if (!maker->second.second) live.erase(maker);
                    }
                    if (type == MsgType::Cancel) {
                        REQUIRE(result.fills.empty());
                        REQUIRE(old != live.end());
                        REQUIRE_EQ(old->second.second, result.remaining.as_raw() + qty);
                        REQUIRE_EQ(result.canceled, Qty(qty));
                    } else {
                        REQUIRE_EQ(qty, result.remaining.as_raw() + result.canceled.as_raw() + traded);
                    }
                    if (result.remaining.is_zero()) {
                        live.erase(id);
                    } else {
                        live[id] = {side, result.remaining.as_raw()};
                    }
                } else {
                    REQUIRE(result.fills.empty());
                    REQUIRE(result.remaining.is_zero());
                    REQUIRE(result.canceled.is_zero());
                }

                REQUIRE_EQ(engine.book().active_orders(), live.size());
                for (const auto& [active_id, value] : live) {
                    const auto found = engine.book().find(OrderId(active_id));
                    REQUIRE(found);
                    REQUIRE_EQ(found->side, value.first);
                    REQUIRE_EQ(found->remaining, Qty(value.second));
                }
                const auto bids = engine.book().depth(Side::Buy, 20);
                const auto asks = engine.book().depth(Side::Sell, 20);
                REQUIRE(bids.empty() || asks.empty() || bids.front().price < asks.front().price);
                for (const auto& [levels, side_of_level] :
                     {std::pair{&bids, Side::Buy}, std::pair{&asks, Side::Sell}}) {
                    for (const auto& level : *levels) {
                        auto queued = engine.book().queue(side_of_level, level.price);
                        REQUIRE_EQ(queued.size(), level.order_count);
                        std::uint64_t sum = 0;
                        for (auto queued_id : queued) {
                            const auto order = engine.book().find(queued_id);
                            REQUIRE(order);
                            REQUIRE_EQ(order->price, level.price);
                            REQUIRE_EQ(order->side, side_of_level);
                            sum += order->remaining.as_raw();
                        }
                        REQUIRE_EQ(level.quantity, Qty(sum));
                    }
                }
            } catch (const faircross::test::TestFailure& error) {
                throw faircross::test::TestFailure("seed " + std::to_string(seed) +
                    ", case " + std::to_string(run) + ", event " + std::to_string(i) +
                    ": " + error.what());
            }
        }
    }
}
