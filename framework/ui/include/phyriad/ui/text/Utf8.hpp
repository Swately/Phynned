// framework/ui/include/phyriad/ui/text/Utf8.hpp
// UTF-8 decoding helpers for phyriad::ui consumers.
//
// Motivation:
//   ImGui expects all text input as UTF-8. Several common sources on Windows
//   do not produce UTF-8 by default:
//     - PresentMon 2.x writes stdout / stderr as UTF-16 LE with BOM (FF FE)
//       when its handles are redirected via STARTF_USESTDHANDLES. The BOM
//       passed straight to ImGui renders as two U+FFFD ("��") and embedded
//       NUL bytes truncate the rest — the infamous "��e" mojibake.
//     - PowerShell logs and some MSVC tools emit UTF-16 BE with BOM (FE FF)
//       or UTF-8 with BOM (EF BB BF).
//
//   decode_to_utf8() handles all three BOM kinds + plain UTF-8 (no BOM),
//   so any byte buffer captured from an external process or file can be
//   safely handed to ImGui::InputTextMultiline / Text / TextWrapped.
//
//   read_file_utf8() is the file-I/O wrapper.
//
// This header is header-only and self-contained. Apps that need it should
// link phyriad_ui (header-only) — no extra dependencies.
//
#pragma once
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
#include <cstddef>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

namespace phyriad::ui::text {

// Decode a byte buffer (file content, captured pipe output, etc.) into a
// UTF-8 std::string suitable for ImGui. Handles UTF-16 LE/BE/UTF-8 BOMs;
// no-BOM input is assumed to already be UTF-8 and returned as-is.
//
// Returns an empty string for empty input. Never throws.
inline std::string decode_to_utf8(const char* bytes, std::size_t n) noexcept {
    if (bytes == nullptr || n == 0u) return {};
    const auto* b = reinterpret_cast<const unsigned char*>(bytes);

    // ── UTF-16 LE BOM: FF FE ──────────────────────────────────────────────────
    if (n >= 2u && b[0] == 0xFFu && b[1] == 0xFEu) {
#ifdef _WIN32
        const wchar_t* w = reinterpret_cast<const wchar_t*>(bytes + 2);
        const int wlen = static_cast<int>((n - 2u) / sizeof(wchar_t));
        if (wlen <= 0) return {};
        const int u8len = WideCharToMultiByte(CP_UTF8, 0, w, wlen,
                                              nullptr, 0, nullptr, nullptr);
        if (u8len <= 0) return {};
        std::string out(static_cast<std::size_t>(u8len), '\0');
        WideCharToMultiByte(CP_UTF8, 0, w, wlen,
                            out.data(), u8len, nullptr, nullptr);
        return out;
#else
        // Non-Windows fallback: strip NULs so callers see something readable.
        // Loses anything beyond U+00FF, but the practical case where this
        // matters (PresentMon output) is ASCII-only after the BOM.
        std::string out;
        out.reserve((n - 2u) / 2u);
        for (std::size_t i = 2u; i + 1u < n; i += 2u) {
            if (b[i] != 0u) out.push_back(static_cast<char>(b[i]));
        }
        return out;
#endif
    }

    // ── UTF-16 BE BOM: FE FF ─────────────────────────────────────────────────
    if (n >= 2u && b[0] == 0xFEu && b[1] == 0xFFu) {
        // Byte-swap into a LE buffer then convert.
        std::vector<unsigned char> le(n - 2u);
        for (std::size_t i = 0u; i + 1u < n - 2u; i += 2u) {
            le[i]     = b[2 + i + 1];
            le[i + 1] = b[2 + i];
        }
#ifdef _WIN32
        const wchar_t* w = reinterpret_cast<const wchar_t*>(le.data());
        const int wlen = static_cast<int>(le.size() / sizeof(wchar_t));
        if (wlen <= 0) return {};
        const int u8len = WideCharToMultiByte(CP_UTF8, 0, w, wlen,
                                              nullptr, 0, nullptr, nullptr);
        if (u8len <= 0) return {};
        std::string out(static_cast<std::size_t>(u8len), '\0');
        WideCharToMultiByte(CP_UTF8, 0, w, wlen,
                            out.data(), u8len, nullptr, nullptr);
        return out;
#else
        std::string out;
        out.reserve(le.size() / 2u);
        for (std::size_t i = 0u; i + 1u < le.size(); i += 2u) {
            if (le[i] != 0u) out.push_back(static_cast<char>(le[i]));
        }
        return out;
#endif
    }

    // ── UTF-8 BOM: EF BB BF ──────────────────────────────────────────────────
    if (n >= 3u && b[0] == 0xEFu && b[1] == 0xBBu && b[2] == 0xBFu) {
        return std::string(bytes + 3, n - 3u);
    }

    // ── No BOM — assume UTF-8 already ────────────────────────────────────────
    return std::string(bytes, n);
}

// Read a file and decode_to_utf8 its contents.
// Returns empty string on read failure (caller should treat as "no content").
inline std::string read_file_utf8(const char* path) noexcept {
    if (path == nullptr || path[0] == '\0') return {};
    std::ifstream f(path, std::ios::binary);
    if (!f.good()) return {};
    std::vector<char> raw((std::istreambuf_iterator<char>(f)),
                          std::istreambuf_iterator<char>());
    if (raw.empty()) return {};
    return decode_to_utf8(raw.data(), raw.size());
}

} // namespace phyriad::ui::text
// Made with my soul - Swately <3
