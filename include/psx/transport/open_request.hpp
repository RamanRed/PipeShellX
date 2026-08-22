#pragma once

#include "psx/result.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace psx::transport {

// The payload of an OPEN frame: what the controller asks a node to start.
// Kept deliberately small and versioned; env/limits/timeout can extend the wire
// format under a new version byte without breaking older readers.
struct OpenRequest {
    std::vector<std::string> argv; // argv[0] is the program to exec
    std::string cwd;               // empty = inherit the agent's working directory

    bool operator==(const OpenRequest& other) const { return argv == other.argv && cwd == other.cwd; }
};

// Wire (big-endian):
//   u8  version (= 1)
//   u32 argc
//   argc × [ u32 len, len bytes ]   (each argument)
//   u32 cwdLen, cwdLen bytes
std::string encodeOpen(const OpenRequest& request);

// Decodes an OPEN payload. Returns an error (protocol violation) on an unknown
// version, a truncated field, or a length that overruns the payload — never
// reads out of bounds, so it is safe on adversarial input.
psx::Result<OpenRequest> decodeOpen(std::string_view payload);

} // namespace psx::transport
