#pragma once

#include <concepts>
#include <cstdint>
#include <limits>
#include <optional>
#include <variant>

#include "faircross/domain/order.hpp"

namespace faircross::continuous {

using faircross::OrderId;
using faircross::Qty;
using faircross::Side;

using PriceTicks = std::int64_t;
using TimestampNs = std::uint64_t;

enum class MsgType : std::uint8_t { New = 0, Cancel = 1, Modify = 2, Market = 3 };

enum class Status : std::uint8_t {
    Ok,
    InvalidMessageType,
    InvalidOrderId,
    InvalidTimestamp,
    InvalidQuantity,
    InvalidPrice,
    InvalidSide,
    DuplicateOrderId,
    NotFound,
    AlreadyClosed,
    Overflow,
    UnsupportedEvent,
};

namespace detail {

template <typename T>
concept EventInteger = std::integral<T> && sizeof(T) <= sizeof(std::uint64_t);

template <typename T, typename Domain>
concept EventField = EventInteger<T> || std::same_as<T, Domain>;

template <typename T>
concept EventPrice = EventInteger<T> || std::same_as<T, std::optional<PriceTicks>> ||
                     std::same_as<T, std::nullopt_t>;

template <typename T>
constexpr std::uint64_t as_unsigned(T field) noexcept {
    if constexpr (EventInteger<T>) {
        return static_cast<std::uint64_t>(field);
    } else {
        return field.as_raw();
    }
}

} // namespace detail

/// A validated single-instrument event. See ADR-019 for fields and units.
class Event {
public:
    template <detail::EventField<OrderId> Id, detail::EventInteger Time,
              detail::EventField<Qty> Lots, detail::EventPrice Ticks = std::nullopt_t>
    [[nodiscard]] static std::variant<Event, Status>
    create(MsgType type, Id id, Time timestamp, Lots qty, Ticks price = std::nullopt,
           std::optional<Side> side = std::nullopt);

    [[nodiscard]] MsgType type() const noexcept { return type_; }
    [[nodiscard]] OrderId id() const noexcept { return id_; }
    [[nodiscard]] TimestampNs timestamp() const noexcept { return timestamp_; }
    [[nodiscard]] Qty qty() const noexcept { return qty_; }
    [[nodiscard]] std::optional<PriceTicks> price() const noexcept { return price_; }
    [[nodiscard]] std::optional<Side> side() const noexcept { return side_; }

private:
    Event(MsgType type, OrderId id, TimestampNs timestamp, Qty qty,
          std::optional<PriceTicks> price, std::optional<Side> side)
        : type_(type), id_(id), timestamp_(timestamp), qty_(qty), price_(price), side_(side) {}

    [[nodiscard]] static std::variant<Event, Status>
    create_normalized(MsgType type, OrderId id, TimestampNs timestamp, Qty qty,
                      std::optional<PriceTicks> price, std::optional<Side> side);

    MsgType type_;
    OrderId id_;
    TimestampNs timestamp_;
    Qty qty_;
    std::optional<PriceTicks> price_;
    std::optional<Side> side_;
};

template <detail::EventField<OrderId> Id, detail::EventInteger Time,
          detail::EventField<Qty> Lots, detail::EventPrice Ticks>
std::variant<Event, Status> Event::create(MsgType type, Id id, Time timestamp, Lots qty,
                                         Ticks price, std::optional<Side> side) {
    if constexpr (std::signed_integral<Id>) {
        if (id < 0) return Status::InvalidOrderId;
    }
    if constexpr (std::signed_integral<Time>) {
        if (timestamp < 0) return Status::InvalidTimestamp;
    }
    if constexpr (std::signed_integral<Lots>) {
        if (qty < 0) return Status::InvalidQuantity;
    }
    const Qty quantity(detail::as_unsigned(qty));
    if (quantity.is_zero()) return Status::InvalidQuantity;

    std::optional<PriceTicks> ticks;
    if constexpr (detail::EventInteger<Ticks>) {
        if constexpr (std::unsigned_integral<Ticks>) {
            constexpr auto max_ticks = static_cast<std::uint64_t>(
                std::numeric_limits<PriceTicks>::max());
            if (price > max_ticks) return Status::InvalidPrice;
        }
        ticks = static_cast<PriceTicks>(price);
    } else {
        ticks = price;
    }

    return create_normalized(type, OrderId(detail::as_unsigned(id)),
                             static_cast<TimestampNs>(timestamp), quantity, ticks, side);
}

} // namespace faircross::continuous
