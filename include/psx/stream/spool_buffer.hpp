#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace psx::stream {

// Disk-backed FIFO byte spill for the Spool overflow policy: overflow bytes are
// appended to an anonymous, auto-deleting temp file (std::tmpfile) so RAM stays
// bounded during a long stream, then read back in append order at the end. Not
// thread-safe — used only from the single reactor thread. On any I/O failure it
// reports false and the caller falls back (drops the bytes), so spooling can
// never corrupt the run.
class SpoolBuffer {
public:
    SpoolBuffer() = default;
    SpoolBuffer(const SpoolBuffer&) = delete;
    SpoolBuffer& operator=(const SpoolBuffer&) = delete;
    SpoolBuffer(SpoolBuffer&& other) noexcept { moveFrom(other); }
    SpoolBuffer& operator=(SpoolBuffer&& other) noexcept {
        if (this != &other) {
            close();
            moveFrom(other);
        }
        return *this;
    }
    ~SpoolBuffer() { close(); }

    // Appends bytes to the spill file (opened lazily on first use). Returns false
    // if the file could not be opened or fully written; the caller drops them.
    bool append(std::string_view bytes) {
        if (bytes.empty()) {
            return true;
        }
        if (failed_) {
            return false; // a prior spill failed: drop the rest so the spool stays consistent
        }
        if (file_ == nullptr) {
            file_ = std::tmpfile();
            if (file_ == nullptr) {
                failed_ = true;
                return false;
            }
        }
        if (std::fseek(file_, 0, SEEK_END) != 0) {
            failed_ = true;
            return false;
        }
        if (std::fwrite(bytes.data(), 1, bytes.size(), file_) != bytes.size()) {
            // A short write may have left partial bytes past the size_ boundary.
            // Do NOT advance size_ — readAll reads only the first size_ bytes, so
            // the orphan is ignored — and poison the buffer so no later append
            // writes past the orphan and corrupts the reconstruction. The caller
            // drops the rest of the stream (counted).
            failed_ = true;
            return false;
        }
        size_ += bytes.size();
        return true;
    }

    std::uint64_t size() const noexcept { return size_; }
    std::uint64_t remaining() const noexcept { return size_ - readOffset_; }
    bool empty() const noexcept { return remaining() == 0; }

    // Reads and consumes up to out.size() bytes from the spill.
    std::size_t read(std::span<char> out) {
        if (file_ == nullptr || out.empty() || empty() || failed_) {
            return 0;
        }
        if (std::fflush(file_) != 0 || std::fseek(file_, static_cast<long>(readOffset_), SEEK_SET) != 0) {
            return 0;
        }
        const std::size_t n = std::min<std::uint64_t>(out.size(), remaining());
        const std::size_t got = std::fread(out.data(), 1, n, file_);
        readOffset_ += got;
        // Keep later appends well-defined on the update stream.
        (void)std::fseek(file_, 0, SEEK_END);
        return got;
    }

    // Reads the entire spilled content back, in append order. Returns "" if
    // nothing was spooled or on a read/seek failure.
    std::string readAll() {
        if (file_ == nullptr || size_ == 0) {
            return {};
        }
        if (std::fflush(file_) != 0 || std::fseek(file_, 0, SEEK_SET) != 0) {
            return {};
        }
        std::string out;
        out.resize(static_cast<std::size_t>(size_));
        const std::size_t got = std::fread(out.data(), 1, out.size(), file_);
        out.resize(got);
        // Leave the stream positioned at the end: C11 forbids a write directly
        // after a read on an update stream without an intervening seek, so this
        // keeps a later append() well-defined.
        (void)std::fseek(file_, 0, SEEK_END);
        return out;
    }

    // Discards the spill (e.g. for a retry): closes the temp file, which deletes
    // it, and drops the byte count.
    void reset() noexcept { close(); }

private:
    void moveFrom(SpoolBuffer& other) noexcept {
        file_ = other.file_;
        size_ = other.size_;
        readOffset_ = other.readOffset_;
        failed_ = other.failed_;
        other.file_ = nullptr;
        other.size_ = 0;
        other.readOffset_ = 0;
        other.failed_ = false;
    }
    void close() noexcept {
        if (file_ != nullptr) {
            std::fclose(file_);
            file_ = nullptr;
        }
        size_ = 0;
        readOffset_ = 0;
        failed_ = false;
    }

    std::FILE* file_ = nullptr;
    std::uint64_t size_ = 0;
    std::uint64_t readOffset_ = 0;
    bool failed_ = false; // a spill I/O failure poisons the buffer: drop the rest, keep size_ consistent
};

} // namespace psx::stream
