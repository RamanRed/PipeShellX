#pragma once

#include <array>
#include <random>
#include <string>

namespace psx::runtime {

// A short random hex token that correlates every log line and audit record of
// one run. Not security-sensitive: a std::random_device-seeded PRNG is enough
// to make collisions between concurrent runs on one host vanishingly unlikely.
inline std::string newRunId() {
    std::mt19937_64 rng(std::random_device{}());
    const std::uint64_t value = rng();
    static constexpr std::array<char, 16> kHex{'0', '1', '2', '3', '4', '5', '6', '7',
                                               '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
    std::string out;
    out.reserve(16);
    for (int shift = 60; shift >= 0; shift -= 4) {
        out.push_back(kHex[(value >> shift) & 0xF]);
    }
    return out;
}

} // namespace psx::runtime
