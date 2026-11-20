#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

#include "faircross/continuous/event.hpp"

namespace faircross::continuous {

struct RestingOrder {
    Side side;
    PriceTicks price;
    Qty remaining;
    TimestampNs timestamp;

    bool operator==(const RestingOrder&) const = default;
};

struct DepthLevel {
    PriceTicks price;
    Qty quantity;
    std::size_t order_count;

    bool operator==(const DepthLevel&) const = default;
};

struct QueuePosition {
    RestingOrder order;
    Qty lots_ahead;
    std::size_t orders_ahead;

    bool operator==(const QueuePosition&) const = default;
};

/// Native Level-3 state for one instrument and one ID session.
class OrderBook {
public:
    /// Applies lifecycle events without crossing. A typed rejection leaves state unchanged.
    [[nodiscard]] Status apply(const Event& event);

    [[nodiscard]] std::optional<RestingOrder> find(OrderId id) const;
    [[nodiscard]] Qty level_qty(Side side, PriceTicks price) const;
    [[nodiscard]] std::size_t active_orders() const noexcept { return orders_.size(); }
    /// Best to worst, with at most max_levels entries.
    [[nodiscard]] std::vector<DepthLevel> depth(Side side, std::size_t max_levels) const;
    /// Orders at one price in FIFO order.
    [[nodiscard]] std::vector<OrderId> queue(Side side, PriceTicks price) const;
    [[nodiscard]] std::optional<QueuePosition> queue_position(OrderId id) const;

    bool operator==(const OrderBook&) const = default;

private:
    friend class MatchingEngine;

    struct Level {
        Qty quantity{};
        std::size_t count{};
        std::optional<OrderId> first;
        std::optional<OrderId> last;

        bool operator==(const Level&) const = default;
    };

    struct Entry {
        RestingOrder order;
        std::optional<OrderId> previous;
        std::optional<OrderId> next;

        bool operator==(const Entry&) const = default;
    };

    using Levels = std::map<PriceTicks, Level>;
    using Orders = std::unordered_map<std::uint64_t, Entry>;

    [[nodiscard]] Levels& levels(Side side) noexcept;
    [[nodiscard]] const Levels& levels(Side side) const noexcept;
    [[nodiscard]] Entry& entry(OrderId id) { return orders_.at(id.as_raw()); }
    [[nodiscard]] const Entry& entry(OrderId id) const { return orders_.at(id.as_raw()); }
    [[nodiscard]] Status missing_status(OrderId id) const;
    [[nodiscard]] bool has_id(OrderId id) const;
    void prepare_retirement(OrderId id);
    void discard_unused_retirement(OrderId id);
    void retire(OrderId id);

    [[nodiscard]] Status add(const Event& event);
    [[nodiscard]] Status cancel(Orders::iterator order, Qty qty);
    [[nodiscard]] Status modify(Orders::iterator order, const Event& event);
    [[nodiscard]] Orders::iterator prepare_insert(OrderId id, RestingOrder order);
    void append(Orders::iterator order, Level& level);
    void unlink(Entry& entry, Level& level);
    void detach(Orders::iterator order, bool keep_level = false);
    void consume(Orders::iterator order, Qty qty);

    Orders orders_;
    // Exact retirement: each nonempty block stores one bit per consecutive ID.
    std::unordered_map<std::uint64_t, std::uint64_t> retired_blocks_;
    Levels bids_;
    Levels asks_;
};

} // namespace faircross::continuous
