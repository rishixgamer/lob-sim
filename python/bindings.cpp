#include <nanobind/nanobind.h>
#include <nanobind/ndarray.h>
#include <nanobind/operators.h>
#include <nanobind/stl/optional.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/variant.h>
#include <nanobind/stl/vector.h>

#include "faircross/continuous/replay.hpp"

#include <cmath>
#include <fstream>
#include <limits>
#include <memory>
#include <type_traits>
#include <utility>

namespace nb = nanobind;
namespace lob = faircross::continuous;
using namespace nb::literals;

namespace {

using Result = std::variant<lob::Event, lob::Status>;

template <typename Int>
std::optional<Int> integer(nb::handle input) {
    if (PyBool_Check(input.ptr())) return std::nullopt;
    auto index = nb::steal<nb::object>(PyNumber_Index(input.ptr()));
    if (!index.is_valid()) {
        if (!PyErr_ExceptionMatches(PyExc_TypeError) &&
            !PyErr_ExceptionMatches(PyExc_OverflowError)) nb::raise_python_error();
        PyErr_Clear();
        return std::nullopt;
    }
    Int value;
    if constexpr (std::is_signed_v<Int>) {
        value = static_cast<Int>(PyLong_AsLongLong(index.ptr()));
    } else {
        value = static_cast<Int>(PyLong_AsUnsignedLongLong(index.ptr()));
    }
    if (PyErr_Occurred()) {
        if (!PyErr_ExceptionMatches(PyExc_OverflowError)) nb::raise_python_error();
        PyErr_Clear();
        return std::nullopt;
    }
    return value;
}

Result create_event(lob::MsgType type, nb::handle id, nb::handle timestamp, nb::handle qty,
                    nb::handle price, std::optional<lob::Side> side) {
    const auto order_id = integer<std::uint64_t>(id);
    if (!order_id) return lob::Status::InvalidOrderId;
    const auto timestamp_ns = integer<lob::TimestampNs>(timestamp);
    if (!timestamp_ns) return lob::Status::InvalidTimestamp;
    const auto quantity = integer<std::uint64_t>(qty);
    if (!quantity || *quantity == 0) return lob::Status::InvalidQuantity;
    std::optional<lob::PriceTicks> ticks;
    if (!price.is_none()) {
        ticks = integer<lob::PriceTicks>(price);
        if (!ticks) return lob::Status::InvalidPrice;
    }
    return lob::Event::create(type, *order_id, *timestamp_ns, *quantity, ticks, side);
}

class Observation {
public:
    Observation(const lob::OrderBook& book, std::size_t depth, double inventory, double pnl) {
        if (depth == 0 || depth > 65536) throw nb::value_error("depth must be in [1, 65536]");
        constexpr auto max_float = std::numeric_limits<float>::max();
        if (!std::isfinite(inventory) || std::abs(inventory) > max_float ||
            !std::isfinite(pnl) || std::abs(pnl) > max_float) {
            throw nb::value_error("inventory and unrealized_pnl must fit in float32");
        }

        const auto bids = book.depth(lob::Side::Buy, depth);
        const auto asks = book.depth(lob::Side::Sell, depth);
        auto values = std::make_shared<std::vector<float>>(4 * depth + 4, 0.0f);
        const auto write_side = [&](const auto& levels, std::size_t offset) {
            double total = 0;
            for (std::size_t i = 0; i < levels.size(); ++i) {
                const auto qty = levels[i].quantity.as_raw();
                (*values)[offset + 2 * i] = static_cast<float>(levels[i].price);
                (*values)[offset + 2 * i + 1] = static_cast<float>(qty);
                total += static_cast<double>(qty);
            }
            return total;
        };
        const auto bid_qty = write_side(bids, 0);
        const auto ask_qty = write_side(asks, 2 * depth);
        if (!bids.empty() && !asks.empty()) {
            (*values)[4 * depth] = static_cast<float>(asks.front().price - bids.front().price);
        }
        if (bid_qty + ask_qty > 0) {
            (*values)[4 * depth + 1] = static_cast<float>((bid_qty - ask_qty) / (bid_qty + ask_qty));
        }
        (*values)[4 * depth + 2] = static_cast<float>(inventory);
        (*values)[4 * depth + 3] = static_cast<float>(pnl);
        values_ = std::move(values);
    }

    [[nodiscard]] auto array() const {
        // The capsule owns a share of the immutable storage independently of
        // the Observation, engine, and any other returned ndarray.
        auto owner = std::make_unique<Storage>(values_);
        nb::capsule capsule(owner.get(), [](void* address) noexcept {
            delete static_cast<Storage*>(address);
        });
        owner.release();
        return nb::ndarray<nb::numpy, const float, nb::ndim<1>, nb::c_contig>(
            values_->data(), {values_->size()}, capsule);
    }

private:
    using Storage = std::shared_ptr<const std::vector<float>>;
    Storage values_;
};

lob::ReplayReport replay_file(lob::MatchingEngine& engine, const std::string& path,
                              lob::ReplayFormat format, nb::object callback) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        const auto location = format == lob::ReplayFormat::Csv ? 1U : 0U;
        return {0, lob::ReplayDiagnostic{lob::ReplayError::IoError, location,
                                         "could not open replay input", std::nullopt}};
    }
    lob::EventReader reader(input, format);
    if (callback.is_none()) return lob::replay(engine, reader);
    return lob::replay(engine, reader, [&](const lob::Event& event, const lob::EventResult& result) {
        // Callback arguments can outlive this invocation.
        callback(nb::cast(event, nb::rv_policy::copy), nb::cast(result, nb::rv_policy::copy));
    });
}

} // namespace

NB_MODULE(_native, module) {
    nb::enum_<lob::Side>(module, "Side")
        .value("Buy", lob::Side::Buy).value("Sell", lob::Side::Sell);
    nb::enum_<lob::MsgType>(module, "MsgType")
        .value("New", lob::MsgType::New).value("Cancel", lob::MsgType::Cancel)
        .value("Modify", lob::MsgType::Modify).value("Market", lob::MsgType::Market);
    nb::enum_<lob::Status>(module, "Status")
        .value("Ok", lob::Status::Ok)
        .value("InvalidMessageType", lob::Status::InvalidMessageType)
        .value("InvalidOrderId", lob::Status::InvalidOrderId)
        .value("InvalidTimestamp", lob::Status::InvalidTimestamp)
        .value("InvalidQuantity", lob::Status::InvalidQuantity)
        .value("InvalidPrice", lob::Status::InvalidPrice)
        .value("InvalidSide", lob::Status::InvalidSide)
        .value("DuplicateOrderId", lob::Status::DuplicateOrderId)
        .value("NotFound", lob::Status::NotFound)
        .value("AlreadyClosed", lob::Status::AlreadyClosed)
        .value("Overflow", lob::Status::Overflow)
        .value("UnsupportedEvent", lob::Status::UnsupportedEvent);

    nb::class_<lob::Event>(module, "Event")
        .def_static("create", create_event, "type"_a, "order_id"_a.none(),
                    "timestamp_ns"_a.none(), "qty"_a.none(),
                    "price_ticks"_a.none() = nb::none(), "side"_a = nb::none())
        .def_prop_ro("type", &lob::Event::type)
        .def_prop_ro("order_id", [](const lob::Event& event) { return event.id().as_raw(); })
        .def_prop_ro("timestamp_ns", &lob::Event::timestamp)
        .def_prop_ro("qty", [](const lob::Event& event) { return event.qty().as_raw(); })
        .def_prop_ro("price_ticks", &lob::Event::price)
        .def_prop_ro("side", &lob::Event::side);
    nb::class_<lob::Trade>(module, "Trade")
        .def_prop_ro("maker_id", [](const lob::Trade& trade) { return trade.maker_id.as_raw(); })
        .def_prop_ro("taker_id", [](const lob::Trade& trade) { return trade.taker_id.as_raw(); })
        .def_ro("taker_side", &lob::Trade::taker_side)
        .def_ro("price_ticks", &lob::Trade::price)
        .def_prop_ro("qty", [](const lob::Trade& trade) { return trade.quantity.as_raw(); })
        .def_ro("timestamp_ns", &lob::Trade::timestamp)
        .def(nb::self == nb::self);
    nb::class_<lob::EventResult>(module, "EventResult")
        .def_ro("status", &lob::EventResult::status)
        .def_ro("fills", &lob::EventResult::fills)
        .def_prop_ro("remaining", [](const lob::EventResult& result) { return result.remaining.as_raw(); })
        .def_prop_ro("canceled", [](const lob::EventResult& result) { return result.canceled.as_raw(); })
        .def(nb::self == nb::self);
    nb::class_<lob::RestingOrder>(module, "RestingOrder")
        .def_ro("side", &lob::RestingOrder::side)
        .def_ro("price_ticks", &lob::RestingOrder::price)
        .def_prop_ro("remaining", [](const lob::RestingOrder& order) { return order.remaining.as_raw(); })
        .def_ro("timestamp_ns", &lob::RestingOrder::timestamp)
        .def(nb::self == nb::self);
    nb::class_<lob::DepthLevel>(module, "DepthLevel")
        .def_ro("price_ticks", &lob::DepthLevel::price)
        .def_prop_ro("qty", [](const lob::DepthLevel& level) { return level.quantity.as_raw(); })
        .def_ro("order_count", &lob::DepthLevel::order_count)
        .def(nb::self == nb::self);
    nb::class_<lob::QueuePosition>(module, "QueuePosition")
        .def_ro("order", &lob::QueuePosition::order)
        .def_prop_ro("lots_ahead", [](const lob::QueuePosition& pos) { return pos.lots_ahead.as_raw(); })
        .def_ro("orders_ahead", &lob::QueuePosition::orders_ahead)
        .def(nb::self == nb::self);
    nb::class_<Observation>(module, "Observation").def("array", &Observation::array);

    nb::enum_<lob::ReplayFormat>(module, "ReplayFormat")
        .value("Csv", lob::ReplayFormat::Csv).value("Binary", lob::ReplayFormat::Binary);
    nb::enum_<lob::ReplayError>(module, "ReplayError")
        .value("InvalidHeader", lob::ReplayError::InvalidHeader)
        .value("UnsupportedVersion", lob::ReplayError::UnsupportedVersion)
        .value("InvalidRecord", lob::ReplayError::InvalidRecord)
        .value("InvalidEvent", lob::ReplayError::InvalidEvent)
        .value("OutOfOrderTimestamp", lob::ReplayError::OutOfOrderTimestamp)
        .value("Truncated", lob::ReplayError::Truncated)
        .value("IoError", lob::ReplayError::IoError);
    nb::class_<lob::ReplayDiagnostic>(module, "ReplayDiagnostic")
        .def_ro("error", &lob::ReplayDiagnostic::error)
        .def_ro("location", &lob::ReplayDiagnostic::location)
        .def_ro("message", &lob::ReplayDiagnostic::message)
        .def_ro("status", &lob::ReplayDiagnostic::status)
        .def(nb::self == nb::self);
    nb::class_<lob::ReplayReport>(module, "ReplayReport")
        .def_ro("events", &lob::ReplayReport::events)
        .def_ro("diagnostic", &lob::ReplayReport::diagnostic);

    nb::class_<lob::MatchingEngine>(module, "MatchingEngine")
        .def(nb::init<>())
        .def("submit", &lob::MatchingEngine::submit, "event"_a)
        .def("replay", replay_file, "path"_a, "format"_a, "on_event"_a.none() = nb::none())
        .def("depth", [](const lob::MatchingEngine& engine, lob::Side side, std::size_t n) {
            return engine.book().depth(side, n);
        }, "side"_a, "levels"_a)
        .def("queue", [](const lob::MatchingEngine& engine, lob::Side side, lob::PriceTicks price) {
            std::vector<std::uint64_t> ids;
            for (auto id : engine.book().queue(side, price)) ids.push_back(id.as_raw());
            return ids;
        }, "side"_a, "price_ticks"_a)
        .def("queue_position", [](const lob::MatchingEngine& engine, nb::handle raw) {
            const auto id = integer<std::uint64_t>(raw);
            if (!id) throw nb::value_error("order_id must fit in uint64");
            return engine.book().queue_position(lob::OrderId(*id));
        }, "order_id"_a)
        .def_prop_ro("active_orders", [](const lob::MatchingEngine& engine) {
            return engine.book().active_orders();
        })
        .def("observation", [](const lob::MatchingEngine& engine, std::size_t depth,
                               double inventory, double pnl) {
            return Observation(engine.book(), depth, inventory, pnl);
        }, "depth"_a, "inventory"_a = 0.0, "unrealized_pnl"_a = 0.0);
}
