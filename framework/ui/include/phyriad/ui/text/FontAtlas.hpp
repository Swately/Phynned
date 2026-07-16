// framework/ui/include/phyriad/ui/text/FontAtlas.hpp
// Glyph-range configuration for the ImGui font atlas used by phyriad::ui.
//
// The default ImGui atlas (built by AddFontDefault() without arguments)
// covers only Basic Latin (codepoints 0x20-0x7E). Anything beyond — Spanish
// accented letters (á é í ó ú ñ ü), Latin-1 punctuation (¿ ¡ © ®), and the
// general-punctuation block (em dash —, ellipsis …, smart quotes " " ' ')
// — renders as missing glyphs ("tofu") even when the UTF-8 bytes are valid.
//
// install_default_font() builds the atlas with a phyriad-wide range that
// covers everything we actually use in UI strings, plus the same dpi-scale
// behavior the OpenGL3 backend's resize() path expects. Call it once at
// startup and again on every DPI change.
//
// This header is header-only and has no dependencies beyond <imgui.h>.
//
#pragma once
#include <imgui.h>

namespace phyriad::ui::text {

// Codepoint ranges covered by the phyriad default atlas:
//   0x0020..0x00FF — Basic Latin + Latin-1 Supplement
//     (covers all Spanish letters, ¿ ¡ © ® ° ±, the Western European set)
//   0x2010..0x205F — General Punctuation
//     (em dash —, en dash –, ellipsis …, smart quotes " " ' ', …)
//
// The trailing 0 is the ImGui-required sentinel. Static-storage so the
// pointer remains valid for the lifetime of the atlas.
inline const ImWchar* default_glyph_ranges() noexcept {
    static const ImWchar kRanges[] = {
        0x0020, 0x00FF,
        0x2010, 0x205F,
        0,
    };
    return kRanges;
}

// (Re)build the ImGui font atlas with the phyriad default range.
//
// font_global_scale lets the OpenGL3 backend pass the current DPI scale on
// resize without callers needing to know about io.FontGlobalScale. Pass 1.0f
// for the initial setup.
inline void install_default_font(ImGuiIO& io,
                                 float font_global_scale = 1.0f) noexcept
{
    io.Fonts->Clear();
    ImFontConfig cfg;
    cfg.OversampleH    = 2;
    cfg.OversampleV    = 1;
    cfg.PixelSnapH     = true;
    cfg.GlyphRanges    = default_glyph_ranges();
    io.Fonts->AddFontDefault(&cfg);
    io.FontGlobalScale = font_global_scale;
}

} // namespace phyriad::ui::text
// Made with my soul - Swately <3
