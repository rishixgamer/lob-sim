#include "faircross/continuous/order_book.hpp"

#include <algorithm>

namespace faircross::continuous {
namespace {

constexpr auto block_index(OrderId id) noexcept { return id.as_raw() / 64; }
constexpr auto block_bit(OrderId id) noexcept { return std::uint64_t{1} << (id.as_raw() % 64); }

Qty subtract(Qty total, Qty qty) noexcept { return Qty(total.as_raw() - qty.as_raw()); }

} // namespace

Status OrderBook::apply(const Event& event) {
    if (event.type() == MsgType::New) return add(event);
    if (event.type() == MsgType::Market) return Status::UnsupportedEvent;

    const auto order = orders_.find(event.id().as_raw());
    if (order == orders_.end()) return missing_status(event.id());
    return event.type() == MsgType::Cancel ? cancel(order, event.qty()) : modify(order, event);
}

std::optional<RestingOrder> OrderBook::find(OrderId id) const {
    const auto order = orders_.find(id.as_raw());
    if (order == orders_.end()) return std::nullopt;
    return order->second.order;
}

Qty OrderBook::level_qty(Side side, PriceTicks price) const {
    if (!is_buy(side) && !is_sell(side)) return Qty::zero();
    const auto& side_levels = levels(side);
    const auto level = side_levels.find(price);
    return level == side_levels.end() ? Qty::zero() : level->second.quantity;
}

std::vector<DepthLevel> OrderBook::depth(Side side, std::size_t max_levels) const {
    if (!is_buy(side) && !is_sell(side)) return {};
    const auto& side_levels = levels(side);
    std::vector<DepthLevel> result;
    result.reserve(std::min(max_levels, side_levels.size()));
    const auto collect = [&](auto pos, auto end) {
        for (; pos != end && result.size() < max_levels; ++pos) {
            result.push_back({pos->first, pos->second.quantity, pos->second.count});
        }
    };
    if (is_buy(side)) {
        collect(side_levels.rbegin(), side_levels.rend());
    } else {
        collect(side_levels.begin(), side_levels.end());
    }
    return result;
}

std::vector<OrderId> OrderBook::queue(Side side, PriceTicks price) const {
    if (!is_buy(side) && !is_sell(side)) return {};
    const auto& side_levels = levels(side);
    const auto level = side_levels.find(price);
    if (level == side_levels.end()) return {};
    std::vector<OrderId> result;
    result.reserve(level->second.count);
    for (auto id = level->second.first; id; id = entry(*id).next) result.push_back(*id);
    return result;
}

std::optional<QueuePosition> OrderBook::queue_position(OrderId id) const {
    const auto order = orders_.find(id.as_raw());
    if (order == orders_.end()) return std::nullopt;
    const auto& resting = order->second;
    std::uint64_t lots = 0;
    std::size_t count = 0;
    for (auto prev = resting.previous; prev; prev = entry(*prev).previous) {
        lots += entry(*prev).order.remaining.as_raw();
        ++count;
    }
    return QueuePosition{resting.order, Qty(lots), count};
}

OrderBook::Levels& OrderBook::levels(Side side) noexcept {
    return is_buy(side) ? bids_ : asks_;
}

const OrderBook::Levels& OrderBook::levels(Side side) const noexcept {
    return is_buy(side) ? bids_ : asks_;
}

Status OrderBook::missing_status(OrderId id) const {
    const auto block = retired_blocks_.find(block_index(id));
    return block != retired_blocks_.end() && (block->second & block_bit(id))
        ? Status::AlreadyClosed : Status::NotFound;
}

bool OrderBook::has_id(OrderId id) const {
    return orders_.contains(id.as_raw()) || missing_status(id) == Status::AlreadyClosed;
}

void OrderBook::prepare_retirement(OrderId id) {
    retired_blocks_.try_emplace(block_index(id), 0);
}

void OrderBook::discard_unused_retirement(OrderId id) {
    const auto block = retired_blocks_.find(block_index(id));
    if (block != retired_blocks_.end() && block->second == 0) retired_blocks_.erase(block);
}

void OrderBook::retire(OrderId id) {
    retired_blocks_.at(block_index(id)) |= block_bit(id);
}

OrderBook::Orders::iterator OrderBook::prepare_insert(OrderId id, RestingOrder order) {
    auto& side_levels = levels(order.side);
    const auto [level, inserted] = side_levels.try_emplace(order.price);
    try {
        return orders_.emplace(id.as_raw(), Entry{order, {}, {}}).first;
    } catch (...) {
        if (inserted) side_levels.erase(level);
        throw;
    }
}

void OrderBook::append(Orders::iterator order, Level& level) {
    const OrderId id(order->first);
    auto& resting = order->second;
    resting.previous = level.last;
    resting.next.reset();
    if (level.last) {
        entry(*level.last).next = id;
    } else {
        level.first = id;
    }
    level.last = id;
    ++level.count;
    level.quantity = Qty(level.quantity.as_raw() + resting.order.remaining.as_raw());
}

void OrderBook::unlink(Entry& resting, Level& level) {
    if (resting.previous) {
        entry(*resting.previous).next = resting.next;
    } else {
        level.first = resting.next;
    }
    if (resting.next) {
        entry(*resting.next).previous = resting.previous;
    } else {
        level.last = resting.previous;
    }
    --level.count;
    level.quantity = subtract(level.quantity, resting.order.remaining);
}

void OrderBook::detach(Orders::iterator order, bool keep_level) {
    auto& resting = order->second;
    auto& side_levels = levels(resting.order.side);
    const auto level = side_levels.find(resting.order.price);
    unlink(resting, level->second);
    if (level->second.count == 0 && !keep_level) side_levels.erase(level);
}

void OrderBook::consume(Orders::iterator order, Qty qty) {
    auto& resting = order->second.order;
    if (qty == resting.remaining) {
        retire(OrderId(order->first));
        detach(order);
        orders_.erase(order);
        return;
    }

    auto& level = levels(resting.side).at(resting.price);
    resting.remaining = subtract(resting.remaining, qty);
    level.quantity = subtract(level.quantity, qty);
}

Status OrderBook::add(const Event& event) {
    if (has_id(event.id())) return Status::DuplicateOrderId;

    const auto side = *event.side();
    const auto price = *event.price();
    if (level_qty(side, price).checked_add(event.qty()).is_err()) return Status::Overflow;

    const auto order = prepare_insert(event.id(), {side, price, event.qty(), event.timestamp()});
    append(order, levels(side).at(price));
    return Status::Ok;
}

Status OrderBook::cancel(Orders::iterator order, Qty qty) {
    if (qty > order->second.order.remaining) return Status::InvalidQuantity;
    if (qty == order->second.order.remaining) prepare_retirement(OrderId(order->first));
    consume(order, qty);
    return Status::Ok;
}

Status OrderBook::modify(Orders::iterator order, const Event& event) {
    auto& resting = order->second.order;
    auto& side_levels = levels(resting.side);
    const auto price = *event.price();
    const bool same_price = price == resting.price;
    const auto level_quantity = level_qty(resting.side, price);
    const auto base = same_price ? subtract(level_quantity, resting.remaining) : level_quantity;
    if (base.checked_add(event.qty()).is_err()) return Status::Overflow;

    const auto target = side_levels.try_emplace(price).first;
    detach(order, same_price);
    resting = {resting.side, price, event.qty(), event.timestamp()};
    append(order, target->second);
    return Status::Ok;
}

} // namespace faircross::continuous
