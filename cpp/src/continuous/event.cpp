#include "faircross/continuous/event.hpp"

namespace faircross::continuous {

std::variant<Event, Status> Event::create_normalized(
    MsgType type, OrderId id, TimestampNs timestamp, Qty qty,
    std::optional<PriceTicks> price, std::optional<Side> side) {
    switch (type) {
        case MsgType::New:
        case MsgType::Cancel:
        case MsgType::Modify:
        case MsgType::Market:
            break;
        default:
            return Status::InvalidMessageType;
    }

    const bool requires_price = type == MsgType::New || type == MsgType::Modify;
    if (requires_price != price.has_value() || (price && *price <= 0)) {
        return Status::InvalidPrice;
    }

    const bool requires_side = type == MsgType::New || type == MsgType::Market;
    if (requires_side != side.has_value() || (side && !is_buy(*side) && !is_sell(*side))) {
        return Status::InvalidSide;
    }

    return Event(type, id, timestamp, qty, price, side);
}

} // namespace faircross::continuous
