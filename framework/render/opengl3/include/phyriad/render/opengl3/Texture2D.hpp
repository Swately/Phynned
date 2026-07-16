// framework/render/opengl3/include/phyriad/render/opengl3/Texture2D.hpp
// Texture2D — a plain RGBA8 OpenGL texture the consumer owns and uploads
// from CPU memory, for display via ImGui::Image.
//
// Why this exists:
//   The opengl3 backend encapsulates its GL loader inside its .cpp on
//   purpose — OpenGL3Backend.hpp leaks no GL symbols. That is correct
//   design, but it leaves a gap: a consumer who wants to show their own
//   texture with ImGui::Image((ImTextureID)tex, ...) has no GL symbols to
//   create that texture with, and gets a cascade of "'GLuint' does not
//   name a type" errors. Texture2D closes that gap: all GL lives in the
//   .cpp (same idiom as ExternalTexture), so the consumer never includes
//   a GL header and never fights <windows.h>/APIENTRY.
//
// Use:
//   Texture2D tex;
//   tex.upload_rgba8(w, h, pixels);                 // GL context must be current
//   ImGui::Image((ImTextureID)(intptr_t)tex.id(), ImVec2(w, h));
//
// Scope is deliberately narrow — RGBA8, linear filtering, clamp-to-edge.
// Consumers who need other formats, mipmaps, immutable storage, or PBO
// streaming should drive GL directly with their own loader (see
// docs/reference/render.md). This helper is for the common case.
//
// Threading: a Texture2D must only be touched by the thread holding the
// current GL context.
//
#pragma once
#include <cstdint>

namespace phyriad::render::opengl3 {

class Texture2D {
public:
    Texture2D()  noexcept = default;
    ~Texture2D() noexcept { destroy(); }

    Texture2D(Texture2D const&)            = delete;
    Texture2D& operator=(Texture2D const&) = delete;
    Texture2D(Texture2D&&) noexcept;
    Texture2D& operator=(Texture2D&&) noexcept;

    // Create-or-replace the texture's storage and upload RGBA8 pixels.
    //   pixels — tightly packed, width*height*4 bytes, top-left origin.
    //            Pass nullptr to allocate storage without uploading.
    // Requires the GL context to be current. Returns false on failure
    // (logged to stderr); on failure any previous contents are preserved.
    [[nodiscard]] bool upload_rgba8(uint32_t    width,
                                    uint32_t    height,
                                    void const* pixels) noexcept;

    // Destroys the GL texture. Idempotent.
    void destroy() noexcept;

    [[nodiscard]] bool     valid()  const noexcept { return id_ != 0u; }
    [[nodiscard]] uint32_t id()     const noexcept { return id_; } // GL texture name
    [[nodiscard]] uint32_t width()  const noexcept { return w_; }
    [[nodiscard]] uint32_t height() const noexcept { return h_; }

private:
    uint32_t id_{0u};
    uint32_t w_ {0u};
    uint32_t h_ {0u};
};

} // namespace phyriad::render::opengl3
// Made with my soul - Swately <3
