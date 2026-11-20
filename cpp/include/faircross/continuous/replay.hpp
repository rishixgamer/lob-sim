#pragma once

#include <cstdint>
#include <functional>
#include <istream>
#include <optional>
#include <string>
#include <variant>

#include "faircross/continuous/matching_engine.hpp"

namespace faircross::continuous {

enum class ReplayFormat { Csv, Binary };

enum class ReplayError {
    InvalidHeader,
    UnsupportedVersion,
    InvalidRecord,
    InvalidEvent,
    OutOfOrderTimestamp,
    Truncated,
    IoError,
};

struct ReplayDiagnostic {
    ReplayError error;
    std::uint64_t location; // CSV line (header = 1) or binary record (header = 0).
    std::string message;
    std::optional<Status> status;

    bool operator==(const ReplayDiagnostic&) const = default;
};

/// Reads at most one event at a time. After a diagnostic, the reader is exhausted.
class EventReader {
public:
    using Item = std::variant<std::monostate, Event, ReplayDiagnostic>;

    EventReader(std::istream& input, ReplayFormat format) : input_(input), format_(format) {}
    [[nodiscard]] Item next();

private:
    [[nodiscard]] std::optional<ReplayDiagnostic> read_header();
    [[nodiscard]] Item read_csv();
    [[nodiscard]] Item read_binary();

    std::istream& input_;
    ReplayFormat format_;
    std::uint64_t location_ = 0;
    std::optional<TimestampNs> previous_;
    bool header_read_ = false;
    bool done_ = false;
};

struct ReplayReport {
    std::uint64_t events = 0;
    std::optional<ReplayDiagnostic> diagnostic;
};

/// Applies records in file order. Business-rule rejections reach the callback
/// and replay continues. A reader diagnostic stops at the committed prefix.
[[nodiscard]] ReplayReport replay(
    MatchingEngine& engine, EventReader& reader,
    const std::function<void(const Event&, const EventResult&)>& on_event = {});

} // namespace faircross::continuous
