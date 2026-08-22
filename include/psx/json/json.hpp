#pragma once

#include <cstdio>
#include <string>
#include <string_view>

namespace psx::json {

// Minimal RFC 8259 escaping: returns `value` as a JSON double-quoted string.
inline std::string quote(std::string_view value) {
    std::string out = "\"";
    out.reserve(value.size() + 2);
    for (const char c : value) {
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            case '\b':
                out += "\\b";
                break;
            case '\f':
                out += "\\f";
                break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buffer[8];
                    std::snprintf(buffer, sizeof(buffer), "\\u%04x", static_cast<unsigned char>(c));
                    out += buffer;
                } else {
                    out += c;
                }
        }
    }
    out += '"';
    return out;
}

// A JSON boolean literal.
inline const char* boolean(bool value) noexcept {
    return value ? "true" : "false";
}

} // namespace psx::json
