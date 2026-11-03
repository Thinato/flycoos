#pragma once
#include <cstddef>
#include <cstdint>
#include <string_view>

// UTF-8 without control characters, bidi overrides or zero-width characters (which could hide or
// reorder a name's text); overlong forms and surrogates are invalid too.
inline bool validName(std::string_view s) {
    for (size_t i = 0; i < s.size();) {
        auto c = uint8_t(s[i]);
        int n = c < 0x80 ? 0 : c >= 0xC2 && c <= 0xDF ? 1 : (c & 0xF0) == 0xE0 ? 2 : c >= 0xF0 && c <= 0xF4 ? 3 : -1;
        if (n < 0 || c < 0x20 || c == 0x7F || s.size() - i <= size_t(n)) return false;
        uint32_t cp = n ? c & (0x3F >> n) : c;
        for (int k = 1; k <= n; ++k) {
            auto t = uint8_t(s[i + size_t(k)]);
            if ((t & 0xC0) != 0x80) return false;
            cp = cp << 6 | (t & 0x3F);
        }
        if ((cp >= 0x80 && cp < 0xA0) || (cp >= 0x200B && cp <= 0x200F) || (cp >= 0x202A && cp <= 0x202E) ||
            (cp >= 0x2066 && cp <= 0x2069) || cp == 0xFEFF || (n == 2 && (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF))) ||
            (n == 3 && (cp < 0x10000 || cp > 0x10FFFF)))
            return false;
        i += size_t(n) + 1;
    }
    return true;
}
