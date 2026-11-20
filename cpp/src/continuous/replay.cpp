#include "faircross/continuous/replay.hpp"

#include <array>
#include <bit>
#include <charconv>
#include <string_view>
#include <utility>

namespace faircross::continuous {
namespace {

constexpr std::string_view csv_header = "timestamp_ns,type,order_id,side,price_ticks,qty";
constexpr std::size_t csv_limit = 256;

ReplayDiagnostic diagnostic(ReplayError error, std::uint64_t location, std::string message,
                            std::optional<Status> status = std::nullopt) {
    return {error, location, std::move(message), status};
}

struct CsvLine {
    std::array<char, csv_limit + 3> buffer;
    std::size_t size = 0;
    bool too_long = false;

    bool read(std::istream& input) {
        // Leave room for CR, LF, and the null terminator.
        input.getline(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        if (!input.gcount()) return false;
        if (input.bad()) return false;
        auto length = static_cast<std::size_t>(input.gcount());
        if (!input.eof() && !input.fail()) --length; // getline counts the delimiter.
        if (length && buffer[length - 1] == '\r') --length;
        too_long = input.fail() || length > csv_limit;
        if (!too_long) size = length;
        return true;
    }

    [[nodiscard]] std::string_view view() const { return {buffer.data(), size}; }
};

template <typename Integer>
bool parse_integer(std::string_view input, Integer& result) {
    if (input.empty()) return false;
    const auto* const end = input.data() + input.size();
    const auto parsed = std::from_chars(input.data(), end, result);
    return parsed.ec == std::errc{} && parsed.ptr == end;
}

std::uint64_t read_u64(const unsigned char* bytes) {
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < 8; ++i) value |= std::uint64_t{bytes[i]} << (8 * i);
    return value;
}

EventReader::Item validate(MsgType type, OrderId id, TimestampNs timestamp, Qty qty,
                           std::optional<PriceTicks> price, std::optional<Side> side,
                           std::uint64_t location) {
    auto parsed = Event::create(type, id, timestamp, qty, price, side);
    if (const auto* event = std::get_if<Event>(&parsed)) return *event;
    return diagnostic(ReplayError::InvalidEvent, location, "invalid event field or field combination",
                      std::get<Status>(parsed));
}

} // namespace

std::optional<ReplayDiagnostic> EventReader::read_header() {
    if (format_ == ReplayFormat::Csv) {
        ++location_;
        CsvLine line;
        if (line.read(input_) && !line.too_long && line.view() == csv_header) return std::nullopt;
        if (input_.bad()) return diagnostic(ReplayError::IoError, location_, "could not read CSV header");
        return diagnostic(ReplayError::InvalidHeader, location_, "unexpected or missing CSV header");
    }

    std::array<unsigned char, 8> header{};
    if (!input_.read(reinterpret_cast<char*>(header.data()),
                     static_cast<std::streamsize>(header.size()))) {
        return diagnostic(input_.bad() ? ReplayError::IoError : ReplayError::Truncated,
                          location_, "could not read binary header");
    }
    if (header[0] != 'F' || header[1] != 'C' || header[2] != 'L' || header[3] != 'B') {
        return diagnostic(ReplayError::InvalidHeader, location_, "invalid binary magic");
    }
    if (header[4] != 1 || header[5] != 0) {
        return diagnostic(ReplayError::UnsupportedVersion, location_, "unsupported binary version");
    }
    if (header[6] != 0 || header[7] != 0) {
        return diagnostic(ReplayError::InvalidHeader, location_, "nonzero binary header reserved bytes");
    }
    return std::nullopt;
}

EventReader::Item EventReader::read_csv() {
    CsvLine line;
    if (!line.read(input_)) {
        if (input_.eof() && !input_.bad()) return std::monostate{};
        return diagnostic(ReplayError::IoError, location_, "could not read CSV record");
    }
    if (line.too_long) {
        return diagnostic(ReplayError::InvalidRecord, location_, "CSV record exceeds 256 bytes");
    }
    auto text = line.view();
    std::array<std::string_view, 6> fields;
    for (std::size_t i = 0; i < fields.size() - 1; ++i) {
        const auto comma = text.find(',');
        if (comma == std::string_view::npos) {
            return diagnostic(ReplayError::InvalidRecord, location_, "expected six CSV fields");
        }
        fields[i] = text.substr(0, comma);
        text.remove_prefix(comma + 1);
    }
    fields.back() = text;
    if (text.find(',') != std::string_view::npos) {
        return diagnostic(ReplayError::InvalidRecord, location_, "expected six CSV fields");
    }

    TimestampNs timestamp;
    if (!parse_integer(fields[0], timestamp)) {
        return diagnostic(ReplayError::InvalidRecord, location_, "invalid timestamp_ns",
                          Status::InvalidTimestamp);
    }
    MsgType type;
    if (fields[1] == "New") type = MsgType::New;
    else if (fields[1] == "Cancel") type = MsgType::Cancel;
    else if (fields[1] == "Modify") type = MsgType::Modify;
    else if (fields[1] == "Market") type = MsgType::Market;
    else return diagnostic(ReplayError::InvalidRecord, location_, "unknown message type",
                           Status::InvalidMessageType);

    std::uint64_t id;
    if (!parse_integer(fields[2], id)) {
        return diagnostic(ReplayError::InvalidRecord, location_, "invalid order_id", Status::InvalidOrderId);
    }
    std::optional<Side> side;
    if (fields[3] == "Buy") side = Side::Buy;
    else if (fields[3] == "Sell") side = Side::Sell;
    else if (!fields[3].empty()) {
        return diagnostic(ReplayError::InvalidRecord, location_, "unknown side", Status::InvalidSide);
    }
    std::optional<PriceTicks> price;
    if (!fields[4].empty() && !parse_integer(fields[4], price.emplace())) {
        return diagnostic(ReplayError::InvalidRecord, location_, "invalid price_ticks", Status::InvalidPrice);
    }
    std::uint64_t qty;
    if (!parse_integer(fields[5], qty)) {
        return diagnostic(ReplayError::InvalidRecord, location_, "invalid qty", Status::InvalidQuantity);
    }
    return validate(type, OrderId(id), timestamp, Qty(qty), price, side, location_);
}

EventReader::Item EventReader::read_binary() {
    std::array<unsigned char, 36> record{};
    if (!input_.read(reinterpret_cast<char*>(record.data()),
                     static_cast<std::streamsize>(record.size()))) {
        if (!input_.bad() && input_.eof() && input_.gcount() == 0) return std::monostate{};
        return diagnostic(input_.bad() ? ReplayError::IoError : ReplayError::Truncated,
                          location_, "could not read complete binary record");
    }
    if (record[2] != 0 || record[3] != 0) {
        return diagnostic(ReplayError::InvalidRecord, location_, "nonzero binary record reserved bytes");
    }
    const auto type = static_cast<MsgType>(record[0]);
    std::optional<Side> side;
    if (record[1] == 1) side = Side::Buy;
    else if (record[1] == 2) side = Side::Sell;
    else if (record[1] != 0) {
        return diagnostic(ReplayError::InvalidRecord, location_, "unknown side", Status::InvalidSide);
    }
    const auto ticks = std::bit_cast<PriceTicks>(read_u64(record.data() + 20));
    const auto price = ticks == 0 ? std::nullopt : std::optional<PriceTicks>(ticks);
    return validate(type, OrderId(read_u64(record.data() + 12)), read_u64(record.data() + 4),
                    Qty(read_u64(record.data() + 28)), price, side, location_);
}

EventReader::Item EventReader::next() {
    if (done_) return std::monostate{};
    if (!header_read_) {
        header_read_ = true;
        if (auto failure = read_header()) {
            done_ = true;
            return std::move(*failure);
        }
    }
    ++location_;
    auto item = format_ == ReplayFormat::Csv ? read_csv() : read_binary();
    if (const auto* event = std::get_if<Event>(&item)) {
        const auto timestamp = event->timestamp();
        if (!previous_ || timestamp >= *previous_) {
            previous_ = timestamp;
            return item;
        }
        item = diagnostic(ReplayError::OutOfOrderTimestamp, location_,
                          "timestamp precedes the previous record", Status::InvalidTimestamp);
    }
    done_ = true;
    return item;
}

ReplayReport replay(MatchingEngine& engine, EventReader& reader,
                    const std::function<void(const Event&, const EventResult&)>& on_event) {
    ReplayReport report;
    while (true) {
        auto item = reader.next();
        if (const auto* event = std::get_if<Event>(&item)) {
            const auto result = engine.submit(*event);
            ++report.events;
            if (on_event) on_event(*event, result);
            continue;
        }
        if (auto* failure = std::get_if<ReplayDiagnostic>(&item)) {
            report.diagnostic = std::move(*failure);
        }
        return report;
    }
}

} // namespace faircross::continuous
