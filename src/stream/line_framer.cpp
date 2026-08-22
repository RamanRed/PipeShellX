#include "psx/stream/line_framer.hpp"

#include <algorithm>

namespace psx::stream {

LineFramer::LineFramer(std::size_t maxLineLength)
    : maxLineLength_(maxLineLength == 0 ? kDefaultMaxLineLength : maxLineLength) {}

void LineFramer::emit(std::string_view line, bool truncated, const LineSink& sink) {
    sink(line, truncated);
}

void LineFramer::push(std::span<const char> data, const LineSink& sink) {
    // Appends one non-terminator byte; force-cuts first if the pending line is
    // already at the limit (so this byte would make it exceed). A line of
    // exactly maxLineLength that then ends in a newline stays one whole line.
    const auto pushData = [&](char byte) {
        if (pending_.size() == maxLineLength_) {
            emit(pending_, true, sink);
            pending_.clear();
        }
        pending_.push_back(byte);
    };

    for (const char c : data) {
        // A CR held from before is only a line terminator if this byte is LF;
        // otherwise it was a bare CR and belongs in the line.
        if (pendingCr_) {
            pendingCr_ = false;
            if (c == '\n') {
                emit(pending_, false, sink);
                pending_.clear();
                continue;
            }
            pushData('\r');
            // fall through to process c normally
        }

        if (c == '\n') {
            emit(pending_, false, sink);
            pending_.clear();
        } else if (c == '\r') {
            pendingCr_ = true; // decide when the next byte arrives
        } else {
            pushData(c);
        }
    }
}

bool LineFramer::flush(const LineSink& sink) {
    if (pendingCr_) {
        pending_.push_back('\r'); // a bare trailing CR is ordinary data at EOF
        pendingCr_ = false;
    }
    if (pending_.empty()) {
        return false;
    }
    emit(pending_, false, sink);
    pending_.clear();
    return true;
}

} // namespace psx::stream
