#include "faircross/continuous/order_book.hpp"

namespace faircross::continuous {

Status OrderBook::apply(const Event& event) {
    if (event.type() == MsgType::New) return add(event);
    if (event.type() == MsgType::Market) return Status::UnsupportedEvent;

    const auto id = event.id().as_raw();
    const auto order = orders_.find(id);
    if (order == orders_.end()) {
        return closed_ids_.contains(id) ? Status::AlreadyClosed : Status::NotFound;
    }

    return event.type() == MsgType::Cancel ? cancel(order, event.qty()) : modify(order, event);
}

std::optional<RestingOrder> OrderBook::find(OrderId id) const {
    const auto order = orders_.find(id.as_raw());
    if (order == orders_.end()) return std::nullopt;
    return order->second;
}

Qty OrderBook::level_qty(Side side, PriceTicks price) const {
    if (!is_buy(side) && !is_sell(side)) return Qty::zero();
    const auto& side_levels = levels(side);
    const auto level = side_levels.find(price);
    return level == side_levels.end() ? Qty::zero() : level->second;
}

OrderBook::Levels& OrderBook::levels(Side side) noexcept {
    return is_buy(side) ? bids_ : asks_;
}

const OrderBook::Levels& OrderBook::levels(Side side) const noexcept {
    return is_buy(side) ? bids_ : asks_;
}

Status OrderBook::add(const Event& event) {
    const auto id = event.id().as_raw();
    if (orders_.contains(id) || closed_ids_.contains(id)) return Status::DuplicateOrderId;

    const auto side = *event.side();
    const auto price = *event.price();
    const auto total = level_qty(side, price).checked_add(event.qty());
    if (total.is_err()) return Status::Overflow;

    auto& side_levels = levels(side);
    const auto [level, inserted] = side_levels.try_emplace(price, Qty::zero());
    try {
        orders_.emplace(id, RestingOrder{side, price, event.qty(), event.timestamp()});
    } catch (...) {
        if (inserted) side_levels.erase(level);
        throw;
    }
    level->second = total.value();
    return Status::Ok;
}

Status OrderBook::cancel(Orders::iterator order, Qty qty) {
    auto& resting = order->second;
    if (qty > resting.remaining) return Status::InvalidQuantity;

    auto& side_levels = levels(resting.side);
    const auto level = side_levels.find(resting.price);
    const auto level_remaining = Qty(level->second.as_raw() - qty.as_raw());

    if (qty == resting.remaining) {
        closed_ids_.insert(order->first);
        orders_.erase(order);
    } else {
        resting.remaining = Qty(resting.remaining.as_raw() - qty.as_raw());
    }

    if (level_remaining.is_zero()) {
        side_levels.erase(level);
    } else {
        level->second = level_remaining;
    }
    return Status::Ok;
}

Status OrderBook::modify(Orders::iterator order, const Event& event) {
    auto& resting = order->second;
    auto& side_levels = levels(resting.side);
    const auto source = side_levels.find(resting.price);
    const auto source_remaining = Qty(source->second.as_raw() - resting.remaining.as_raw());
    const auto price = *event.price();
    const auto base = price == resting.price ? source_remaining : level_qty(resting.side, price);
    const auto total = base.checked_add(event.qty());
    if (total.is_err()) return Status::Overflow;

    const auto [target, inserted] = side_levels.try_emplace(price, total.value());
    if (!inserted) target->second = total.value();
    if (source != target) {
        if (source_remaining.is_zero()) {
            side_levels.erase(source);
        } else {
            source->second = source_remaining;
        }
    }
    resting = {resting.side, price, event.qty(), event.timestamp()};
    return Status::Ok;
}

} // namespace faircross::continuous
