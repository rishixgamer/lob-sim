#pragma once

#include <vector>

#include "faircross/continuous/order_book.hpp"

namespace faircross::continuous {

struct Trade {
    OrderId maker_id;
    OrderId taker_id;
    Side taker_side;
    PriceTicks price;
    Qty quantity;
    TimestampNs timestamp;

    bool operator==(const Trade&) const = default;
};

struct EventResult {
    Status status = Status::Ok;
    std::vector<Trade> fills;
    Qty remaining{};
    Qty canceled{};

    bool operator==(const EventResult&) const = default;
};

/// Single-instrument continuous price-time matcher. Submission order breaks ties.
class MatchingEngine {
public:
    [[nodiscard]] EventResult submit(const Event& event);
    [[nodiscard]] const OrderBook& book() const noexcept { return book_; }

private:
    [[nodiscard]] EventResult execute(const Event& event, Side side,
                                      OrderBook::Orders::iterator previous);
    void plan_fills(const Event& event, Side side, EventResult& result) const;

    OrderBook book_;
};

} // namespace faircross::continuous
