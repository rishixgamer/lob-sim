#include "faircross/continuous/replay.hpp"
#include "test_framework.hpp"

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace lob = faircross::continuous;

namespace {

constexpr auto header = "timestamp_ns,type,order_id,side,price_ticks,qty\n";

std::ifstream open_fixture(const char* name) {
    std::ifstream source(std::string("fixtures/continuous/") + name, std::ios::binary);
    if (!source) {
        source.clear();
        source.open(std::string("../fixtures/continuous/") + name, std::ios::binary);
    }
    return source;
}

} // namespace

TEST_CASE(continuous_csv_binary_and_direct_submission_agree) {
    using enum lob::MsgType;
    using enum lob::ReplayFormat;
    using enum lob::Status;
    using lob::Side;

    std::vector<lob::EventResult> csv_results;
    std::vector<lob::EventResult> binary_results;
    lob::MatchingEngine csv_engine;
    lob::MatchingEngine binary_engine;
    auto csv = open_fixture("session.csv");
    auto binary = open_fixture("session.bin");
    REQUIRE(csv);
    REQUIRE(binary);
    lob::EventReader csv_reader(csv, Csv);
    lob::EventReader binary_reader(binary, Binary);
    const auto capture = [](auto& output) {
        return [&output](const lob::Event&, const lob::EventResult& result) {
            output.push_back(result);
        };
    };
    const auto csv_report = lob::replay(csv_engine, csv_reader, capture(csv_results));
    const auto binary_report = lob::replay(binary_engine, binary_reader, capture(binary_results));
    REQUIRE(!csv_report.diagnostic);
    REQUIRE(!binary_report.diagnostic);
    REQUIRE_EQ(csv_report.events, 12U);
    REQUIRE_EQ(binary_report.events, csv_report.events);
    REQUIRE_EQ(csv_results, binary_results);
    REQUIRE_EQ(csv_engine.book(), binary_engine.book());

    using EventOrStatus = std::variant<lob::Event, lob::Status>;
    const std::vector<EventOrStatus> direct_events{
        lob::Event::create(New, 0, 0, 5, 101, Side::Sell),
        lob::Event::create(New, 1, 0, 3, 101, Side::Sell),
        lob::Event::create(New, 2, 1, 4, 102, Side::Sell),
        lob::Event::create(New, 3, 2, 10, 102, Side::Buy),
        lob::Event::create(New, 4, 3, 4, 99, Side::Buy),
        lob::Event::create(Modify, 4, 4, 4, 102),
        lob::Event::create(Market, 5, 5, 1, std::nullopt, Side::Sell),
        lob::Event::create(Cancel, 4, 6, 3),
        lob::Event::create(Cancel, 5, 7, 1),
        lob::Event::create(Market, 6, 8, 2, std::nullopt, Side::Buy),
        lob::Event::create(New, 7, 9, 6, 98, Side::Buy),
        lob::Event::create(Modify, 7, 9, 4, 98),
    };
    lob::MatchingEngine direct;
    for (std::size_t i = 0; i < direct_events.size(); ++i) {
        REQUIRE_EQ(direct.submit(std::get<lob::Event>(direct_events[i])), csv_results[i]);
    }
    REQUIRE_EQ(csv_results[7].status, InvalidQuantity);
    REQUIRE_EQ(csv_results[8].status, AlreadyClosed);
    REQUIRE_EQ(csv_results[9].canceled, lob::Qty(2));
    REQUIRE_EQ(direct.book(), csv_engine.book());
}

TEST_CASE(continuous_replay_stops_at_invalid_record_and_preserves_prefix) {
    using enum lob::ReplayError;
    using enum lob::ReplayFormat;
    const auto csv = [](std::string suffix, lob::ReplayError error, lob::Status status) {
        std::istringstream stream(std::string(header) + "8,New,1,Buy,100,4\r\n" +
                                  suffix + "\n9,New,3,Buy,100,4\n");
        lob::EventReader reader(stream, Csv);
        lob::MatchingEngine engine;
        const auto report = lob::replay(engine, reader);
        REQUIRE_EQ(report.events, 1U);
        REQUIRE(report.diagnostic);
        REQUIRE_EQ(report.diagnostic->location, 3U);
        REQUIRE_EQ(report.diagnostic->error, error);
        REQUIRE_EQ(report.diagnostic->status, status);
        REQUIRE_EQ(engine.book().active_orders(), 1U);
        REQUIRE_EQ(engine.book().level_qty(lob::Side::Buy, 100), lob::Qty(4));
        REQUIRE(std::holds_alternative<std::monostate>(reader.next()));
    };
    csv("9,New,-2,Sell,100,1", InvalidRecord, lob::Status::InvalidOrderId);
    csv("7,New,2,Sell,100,1", OutOfOrderTimestamp, lob::Status::InvalidTimestamp);
    csv("9,New,2,Sell,0,1", InvalidEvent, lob::Status::InvalidPrice);
    csv("9,Market,2,Buy,100,1", InvalidEvent, lob::Status::InvalidPrice);
    csv("9,New,2,Sell,100,18446744073709551616", InvalidRecord, lob::Status::InvalidQuantity);

    std::istringstream overlong(std::string(header) + std::string(100000, '1'));
    lob::EventReader overlong_reader(overlong, Csv);
    const auto oversized = std::get<lob::ReplayDiagnostic>(overlong_reader.next());
    REQUIRE_EQ(oversized.error, InvalidRecord);
    REQUIRE_EQ(oversized.location, 2U);
    REQUIRE(overlong.gcount() < 300);

    std::istringstream no_lf(std::string(header) + "0,New,0,Buy,1,1");
    lob::EventReader no_lf_reader(no_lf, Csv);
    REQUIRE(std::holds_alternative<lob::Event>(no_lf_reader.next()));
    REQUIRE(std::holds_alternative<std::monostate>(no_lf_reader.next()));

    const std::string binary_header("FCLB\1\0\0\0", 8);
    std::istringstream unsupported(std::string("FCLB\2\0\0\0", 8));
    lob::EventReader unsupported_reader(unsupported, Binary);
    const auto version = std::get<lob::ReplayDiagnostic>(unsupported_reader.next());
    REQUIRE_EQ(version.error, UnsupportedVersion);
    REQUIRE_EQ(version.location, 0U);

    auto fixture = open_fixture("session.bin");
    REQUIRE(fixture);
    std::string data(8 + 36 + 3, '\0');
    fixture.read(data.data(), static_cast<std::streamsize>(data.size()));
    REQUIRE(fixture);
    std::istringstream truncated(data);
    lob::EventReader truncated_reader(truncated, Binary);
    lob::MatchingEngine engine;
    const auto report = lob::replay(engine, truncated_reader);
    REQUIRE_EQ(report.events, 1U);
    REQUIRE(report.diagnostic);
    REQUIRE_EQ(report.diagnostic->error, Truncated);
    REQUIRE_EQ(report.diagnostic->location, 2U);
    REQUIRE_EQ(engine.book().level_qty(lob::Side::Sell, 101), lob::Qty(5));

    std::istringstream invalid(binary_header + std::string("\0\0\1", 3) + std::string(33, '\0'));
    lob::EventReader invalid_reader(invalid, Binary);
    const auto reserved = std::get<lob::ReplayDiagnostic>(invalid_reader.next());
    REQUIRE_EQ(reserved.error, InvalidRecord);
    REQUIRE_EQ(reserved.location, 1U);
}
