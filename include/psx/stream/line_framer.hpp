#pragma once

// psx::stream::LineFramer — turns an arbitrarily-chunked byte stream into
// whole lines (L2). It emits a line only when its terminator has arrived, so
// two producers multiplexed into one --stream sink can never interleave a
// partial line; a partial line at EOF is released by flush(). CRLF is
// normalised to LF (a CR may be split across chunks). A line longer than
// maxLineLength is emitted in maxLineLength-sized pieces, each flagged
// truncated, so a runaway producer cannot grow the pending buffer without
// bound.

#include <cstddef>
#include <functional>
#include <span>
#include <string>
#include <string_view>

namespace psx::stream {

class LineFramer {
public:
    // The callback receives one line (without its terminator) and whether it
    // was force-cut at maxLineLength rather than ending at a real newline.
    using LineSink = std::function<void(std::string_view line, bool truncated)>;

    static constexpr std::size_t kDefaultMaxLineLength = 1024 * 1024;

    explicit LineFramer(std::size_t maxLineLength = kDefaultMaxLineLength);

    // Feeds bytes, emitting every complete (or length-capped) line via `sink`.
    void push(std::span<const char> data, const LineSink& sink);

    // Emits any buffered partial line (truncated=false — it is complete as far
    // as the input went). Returns true if a line was emitted.
    bool flush(const LineSink& sink);

    bool hasPending() const noexcept { return !pending_.empty(); }

private:
    void emit(std::string_view line, bool truncated, const LineSink& sink);

    std::string pending_; // bytes of the line not yet terminated
    std::size_t maxLineLength_;
    bool pendingCr_ = false; // a trailing CR from the previous chunk, held back
};

} // namespace psx::stream
