#include "psx/transport/control_payloads.hpp"

#include "psx/transport/wire.hpp"

namespace psx::transport {

namespace {
psx::Error malformed(const char* what) {
    return psx::Error{psx::ErrorClass::Other, 0, what};
}
} // namespace

std::string encodeExit(const psx::os::ExitStatus& status) {
    std::string out;
    out.push_back(static_cast<char>(static_cast<std::uint8_t>(status.kind)));
    writeU32BE(out, static_cast<std::uint32_t>(status.code)); // i32 two's-complement bits
    return out;
}

psx::Result<psx::os::ExitStatus> decodeExit(std::string_view payload) {
    if (payload.size() != 5) {
        return malformed("EXIT payload: expected 5 bytes");
    }
    const auto kind = static_cast<std::uint8_t>(payload[0]);
    if (kind > static_cast<std::uint8_t>(psx::os::ExitStatus::Kind::Terminated)) {
        return malformed("EXIT payload: unknown status kind");
    }
    psx::os::ExitStatus status;
    status.kind = static_cast<psx::os::ExitStatus::Kind>(kind);
    status.code = static_cast<std::int32_t>(readU32BE(payload.data() + 1));
    return status;
}

std::string encodeWindowUpdate(std::uint32_t delta) {
    std::string out;
    writeU32BE(out, delta);
    return out;
}

psx::Result<std::uint32_t> decodeWindowUpdate(std::string_view payload) {
    if (payload.size() != 4) {
        return malformed("WINDOW_UPDATE payload: expected 4 bytes");
    }
    const std::uint32_t delta = readU32BE(payload.data());
    if (delta == 0) {
        return malformed("WINDOW_UPDATE payload: zero delta");
    }
    return delta;
}

} // namespace psx::transport
