#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <unordered_map>
#include <unordered_set>

#include "faircross/continuous/event.hpp"

namespace faircross::continuous {

struct RestingOrder {
    Side side;
    PriceTicks price;
    Qty remaining;
    TimestampNs timestamp;

    bool operator==(const RestingOrder&) const = default;
};

/// Native Level-3 state for one instrument and one ID session.
class OrderBook {
public:
    /// Applies validated lifecycle events. A rejected event leaves state unchanged.
    [[nodiscard]] Status apply(const Event& event);

    [[nodiscard]] std::optional<RestingOrder> find(OrderId id) const;
    [[nodiscard]] Qty level_qty(Side side, PriceTicks price) const;

    bool operator==(const OrderBook&) const = default;

private:
    using Levels = std::map<PriceTicks, Qty>;
    using Orders = std::unordered_map<std::uint64_t, RestingOrder>;

    [[nodiscard]] Levels& levels(Side side) noexcept;
    [[nodiscard]] const Levels& levels(Side side) const noexcept;
    [[nodiscard]] Status add(const Event& event);
    [[nodiscard]] Status cancel(Orders::iterator order, Qty qty);
    [[nodiscard]] Status modify(Orders::iterator order, const Event& event);

    Orders orders_;
    std::unordered_set<std::uint64_t> closed_ids_;
    Levels bids_;
    Levels asks_;
};

} // namespace faircross::continuous
