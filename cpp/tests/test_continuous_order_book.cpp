#include "faircross/continuous/order_book.hpp"
#include "faircross/marketdata/book.hpp"
#include "test_framework.hpp"

#include <limits>

namespace lob = faircross::continuous;

namespace {

using lob::Event;
using lob::MsgType;
using lob::OrderId;
using lob::PriceTicks;
using lob::Qty;
using lob::Side;
using lob::Status;
using lob::TimestampNs;

constexpr auto max_uint = std::numeric_limits<std::uint64_t>::max();
constexpr auto max_price = std::numeric_limits<PriceTicks>::max();

template <typename Price>
concept AcceptsPrice = requires(Price value) {
    Event::create(MsgType::New, 1, 0, 1, value, Side::Buy);
};

static_assert(AcceptsPrice<PriceTicks>);
static_assert(!AcceptsPrice<double>);
static_assert(!AcceptsPrice<std::optional<double>>);

template <typename... Fields>
auto submit(lob::OrderBook& book, Fields... fields) {
    const auto result = Event::create(fields...);
    if (const auto* event = std::get_if<Event>(&result)) return book.apply(*event);
    return std::get<Status>(result);
}

template <typename... Fields>
void reject(lob::OrderBook& book, Status expected, Fields... fields) {
    const auto before = book;
    REQUIRE_EQ(submit(book, fields...), expected);
    REQUIRE_EQ(book, before);
}

} // namespace

TEST_CASE(continuous_contract_rejects_invalid_fields) {
    lob::OrderBook book;
    REQUIRE_EQ(submit(book, MsgType::New, 1, 5, 8, 20, Side::Buy), Status::Ok);

    reject(book, Status::InvalidOrderId, MsgType::New, -1, 0, 1, 20, Side::Sell);
    reject(book, Status::InvalidTimestamp, MsgType::Cancel, 1, -1, 1);
    reject(book, Status::InvalidQuantity, MsgType::New, 2, 0, -1, 20, Side::Sell);
    reject(book, Status::InvalidQuantity, MsgType::Cancel, 1, 0, 0);
    reject(book, Status::InvalidQuantity, MsgType::Modify, 1, 0, 0, 20);
    reject(book, Status::InvalidQuantity, MsgType::Market, 2, 0, 0, std::nullopt, Side::Buy);
    reject(book, Status::InvalidQuantity, MsgType::Cancel, 1, 0, 9);
    reject(book, Status::InvalidPrice, MsgType::New, 2, 0, 1, -1, Side::Sell);
    reject(book, Status::InvalidPrice, MsgType::New, 2, 0, 1, 0, Side::Sell);
    reject(book, Status::InvalidPrice, MsgType::New, 2, 0, 1, max_uint, Side::Sell);
    reject(book, Status::InvalidPrice, MsgType::Modify, 1, 0, 1);
    reject(book, Status::InvalidPrice, MsgType::Cancel, 1, 0, 1, 20);
    reject(book, Status::InvalidPrice, MsgType::Market, 2, 0, 1, 20, Side::Buy);
    reject(book, Status::InvalidSide, MsgType::New, 2, 0, 1, 20);
    reject(book, Status::InvalidSide, MsgType::Market, 2, 0, 1);
    reject(book, Status::InvalidSide, MsgType::Modify, 1, 0, 1, 20, Side::Sell);
    reject(book, Status::InvalidSide, MsgType::New, 2, 0, 1, 20, static_cast<Side>(0));
    reject(book, Status::InvalidMessageType, static_cast<MsgType>(255), 2, 0, 1);
    reject(book, Status::UnsupportedEvent, MsgType::Market, 2, 0, 1, std::nullopt, Side::Buy);
    REQUIRE_EQ(submit(book, MsgType::New, 2, 0, 1, 20, Side::Sell), Status::Ok);
}

TEST_CASE(continuous_order_lifecycle_retains_ids_and_tracks_levels) {
    lob::OrderBook book;
    REQUIRE_EQ(submit(book, MsgType::New, OrderId(0), TimestampNs(0), Qty(10),
                      std::optional<PriceTicks>(20), Side::Buy), Status::Ok);
    reject(book, Status::InvalidQuantity, MsgType::Cancel, OrderId(0), 0, Qty::zero());
    REQUIRE_EQ(submit(book, MsgType::New, 1, 1, 5, 20, Side::Buy), Status::Ok);
    REQUIRE_EQ(submit(book, MsgType::New, 2, 1, 1, 20, Side::Sell), Status::Ok);
    REQUIRE_EQ(book.level_qty(Side::Buy, 20), Qty(15));
    REQUIRE_EQ(book.level_qty(Side::Sell, 20), Qty(1));
    reject(book, Status::DuplicateOrderId, MsgType::New, 0, 1, 1, 21, Side::Sell);
    reject(book, Status::NotFound, MsgType::Cancel, 3, 1, 1);
    reject(book, Status::NotFound, MsgType::Modify, 3, 1, 1, 20);

    REQUIRE_EQ(submit(book, MsgType::Cancel, 0, 2, 3), Status::Ok);
    REQUIRE_EQ(book.find(OrderId(0)).value(), (lob::RestingOrder{Side::Buy, 20, Qty(7), 0}));
    REQUIRE_EQ(book.level_qty(Side::Buy, 20), Qty(12));

    REQUIRE_EQ(submit(book, MsgType::Modify, 0, 3, 9, 21), Status::Ok);
    REQUIRE_EQ(book.find(OrderId(0)).value(), (lob::RestingOrder{Side::Buy, 21, Qty(9), 3}));
    REQUIRE_EQ(book.level_qty(Side::Buy, 20), Qty(5));
    REQUIRE_EQ(book.level_qty(Side::Buy, 21), Qty(9));

    REQUIRE_EQ(submit(book, MsgType::Cancel, 0, 4, 9), Status::Ok);
    REQUIRE(!book.find(OrderId(0)));
    REQUIRE_EQ(book.level_qty(Side::Buy, 21), Qty::zero());
    REQUIRE_EQ(book.level_qty(Side::Buy, 20), Qty(5));
    reject(book, Status::AlreadyClosed, MsgType::Cancel, 0, 5, 1);
    reject(book, Status::AlreadyClosed, MsgType::Modify, 0, 5, 1, 21);
    reject(book, Status::DuplicateOrderId, MsgType::New, 0, 5, 1, 21, Side::Buy);

    lob::OrderBook new_session;
    REQUIRE_EQ(submit(new_session, MsgType::New, 0, 0, 1, 20, Side::Buy), Status::Ok);
}

TEST_CASE(continuous_level_overflow_rejects_adds_and_modifications_atomically) {
    lob::OrderBook book;
    REQUIRE_EQ(submit(book, MsgType::New, max_uint, max_uint, max_uint - 1, max_price,
                      Side::Sell), Status::Ok);
    REQUIRE_EQ(submit(book, MsgType::New, 1, 0, 1, max_price, Side::Sell), Status::Ok);
    REQUIRE_EQ(book.level_qty(Side::Sell, max_price), Qty(max_uint));

    reject(book, Status::Overflow, MsgType::New, 2, 0, 1, max_price, Side::Sell);
    REQUIRE_EQ(submit(book, MsgType::New, 2, 0, 1, max_price - 1, Side::Sell), Status::Ok);
    reject(book, Status::Overflow, MsgType::Modify, 2, 1, 1, max_price);
    reject(book, Status::Overflow, MsgType::Modify, 1, 1, 2, max_price);

    REQUIRE_EQ(submit(book, MsgType::Modify, max_uint, max_uint, max_uint - 1, max_price),
               Status::Ok);
    REQUIRE_EQ(book.find(OrderId(max_uint)).value().timestamp, max_uint);
    REQUIRE_EQ(submit(book, MsgType::Cancel, 1, 1, 1), Status::Ok);
    REQUIRE_EQ(submit(book, MsgType::Modify, 2, 1, 1, max_price), Status::Ok);
    REQUIRE_EQ(book.level_qty(Side::Sell, max_price - 1), Qty::zero());
    REQUIRE_EQ(book.level_qty(Side::Sell, max_price), Qty(max_uint));
}
