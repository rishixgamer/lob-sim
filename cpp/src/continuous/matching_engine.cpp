#include "faircross/continuous/matching_engine.hpp"

#include <algorithm>

namespace faircross::continuous {
namespace {

Qty subtract(Qty total, Qty qty) noexcept { return Qty(total.as_raw() - qty.as_raw()); }

EventResult rejected(Status status) { return {status, {}, {}, {}}; }

} // namespace

EventResult MatchingEngine::submit(const Event& event) {
    if (event.type() == MsgType::New || event.type() == MsgType::Market) {
        if (book_.has_id(event.id())) return rejected(Status::DuplicateOrderId);
        return execute(event, *event.side(), book_.orders_.end());
    }

    const auto previous = book_.orders_.find(event.id().as_raw());
    if (previous == book_.orders_.end()) return rejected(book_.missing_status(event.id()));

    const auto order = previous->second.order;
    if (event.type() == MsgType::Cancel) {
        const auto status = book_.cancel(previous, event.qty());
        if (status != Status::Ok) return rejected(status);
        return {Status::Ok, {}, subtract(order.remaining, event.qty()), event.qty()};
    }
    if (*event.price() == order.price && event.qty() <= order.remaining) {
        book_.consume(previous, subtract(order.remaining, event.qty()));
        return {Status::Ok, {}, event.qty(), {}};
    }
    return execute(event, order.side, previous);
}

void MatchingEngine::plan_fills(const Event& event, Side side, EventResult& result) const {
    const auto& opposite = book_.levels(is_buy(side) ? Side::Sell : Side::Buy);
    const auto limit = event.price();
    const auto sweep = [&](auto pos, auto end) {
        for (; pos != end && !result.remaining.is_zero(); ++pos) {
            const auto price = pos->first;
            if (limit && (is_buy(side) ? price > *limit : price < *limit)) break;
            for (auto id = pos->second.first; id && !result.remaining.is_zero();) {
                const auto& maker = book_.entry(*id);
                const auto traded = std::min(result.remaining, maker.order.remaining);
                result.fills.push_back({*id, event.id(), side, price, traded, event.timestamp()});
                result.remaining = subtract(result.remaining, traded);
                id = maker.next;
            }
        }
    };

    if (is_buy(side)) {
        sweep(opposite.begin(), opposite.end());
    } else {
        sweep(opposite.rbegin(), opposite.rend());
    }
}

EventResult MatchingEngine::execute(const Event& event, Side side,
                                     OrderBook::Orders::iterator previous) {
    EventResult result{Status::Ok, {}, event.qty(), {}};
    plan_fills(event, side, result);
    if (event.type() == MsgType::Market) {
        result.canceled = result.remaining;
        result.remaining = Qty::zero();
    }

    const auto price = event.price();
    const bool replacement = previous != book_.orders_.end();
    const bool same_price = replacement && previous->second.order.price == price;
    const bool rests = !result.remaining.is_zero();
    if (rests) {
        auto base = book_.level_qty(side, *price);
        if (same_price) base = subtract(base, previous->second.order.remaining);
        if (base.checked_add(result.remaining).is_err()) return rejected(Status::Overflow);
    }

    // Reserve all allocating state before changing queues or marking IDs closed.
    auto incoming = previous;
    OrderBook::Level* target = nullptr;
    try {
        for (const auto& fill : result.fills) {
            if (book_.entry(fill.maker_id).order.remaining == fill.quantity) {
                book_.prepare_retirement(fill.maker_id);
            }
        }
        if (!rests) {
            book_.prepare_retirement(event.id());
        } else {
            if (!replacement) {
                incoming = book_.prepare_insert(event.id(),
                                                {side, *price, result.remaining, event.timestamp()});
                target = &book_.levels(side).at(*price);
            } else {
                target = &book_.levels(side).try_emplace(*price).first->second;
            }
        }
    } catch (...) {
        for (const auto& fill : result.fills) book_.discard_unused_retirement(fill.maker_id);
        book_.discard_unused_retirement(event.id());
        throw;
    }

    if (replacement) book_.detach(incoming, same_price && rests);
    for (const auto& fill : result.fills) {
        book_.consume(book_.orders_.find(fill.maker_id.as_raw()), fill.quantity);
    }
    if (rests) {
        incoming->second.order = {side, *price, result.remaining, event.timestamp()};
        book_.append(incoming, *target);
    } else {
        book_.retire(event.id());
        if (replacement) book_.orders_.erase(incoming);
    }
    return result;
}

} // namespace faircross::continuous
