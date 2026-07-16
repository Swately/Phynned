// framework/render/opengl3/src/Texture2D.cpp
// Implementation — see header for the contract.
//
// Creates a classic GL_TEXTURE_2D and uploads RGBA8 pixels via the
// bind-based path (glGenTextures / glTexImage2D), which is available on
// the 3.3 Core context the opengl3 backend uses. Function pointers are
// resolved through glfwGetProcAddress at first use; no GLAD/GLEW — same
// convention as ExternalTexture.cpp / FSR1Pass.cpp.
//
#include <phyriad/render/opengl3/Texture2D.hpp>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <cstdio>
#include <utility>

// ─────────────────────────────────────────────────────────────────────────────
// Minimal GL types + entry points needed by this TU. We don't pull in a GL
// loader library; we declare the subset of glcorearb.h we actually use.
// ─────────────────────────────────────────────────────────────────────────────
#ifndef APIENTRY
#   ifdef _WIN32
#       define APIENTRY __stdcall
#   else
#       define APIENTRY
#   endif
#endif
#define APIENTRYP APIENTRY *

typedef unsigned int  GLenum;
typedef unsigned int  GLuint;
typedef int           GLint;
typedef int           GLsizei;

// Constants we use.
#define GL_NO_ERROR             0
#define GL_TEXTURE_2D           0x0DE1
#define GL_TEXTURE_MIN_FILTER   0x2801
#define GL_TEXTURE_MAG_FILTER   0x2800
#define GL_LINEAR               0x2601
#define GL_TEXTURE_WRAP_S       0x2802
#define GL_TEXTURE_WRAP_T       0x2803
#define GL_CLAMP_TO_EDGE        0x812F
#define GL_UNPACK_ALIGNMENT     0x0CF5
#define GL_RGBA8                0x8058
#define GL_RGBA                 0x1908
#define GL_UNSIGNED_BYTE        0x1401

// Function-pointer types.
typedef GLenum (APIENTRYP PFNGLGETERRORPROC)(void);
typedef void   (APIENTRYP PFNGLGENTEXTURESPROC)(GLsizei n, GLuint* textures);
typedef void   (APIENTRYP PFNGLDELETETEXTURESPROC)(GLsizei n, const GLuint* textures);
typedef void   (APIENTRYP PFNGLBINDTEXTUREPROC)(GLenum target, GLuint texture);
typedef void   (APIENTRYP PFNGLTEXPARAMETERIPROC)(GLenum target, GLenum pname, GLint param);
typedef void   (APIENTRYP PFNGLPIXELSTOREIPROC)(GLenum pname, GLint param);
typedef void   (APIENTRYP PFNGLTEXIMAGE2DPROC)(
    GLenum target, GLint level, GLint internalformat,
    GLsizei width, GLsizei height, GLint border,
    GLenum format, GLenum type, const void* pixels);

namespace {

PFNGLGETERRORPROC       p_glGetError      = nullptr;
PFNGLGENTEXTURESPROC    p_glGenTextures   = nullptr;
PFNGLDELETETEXTURESPROC p_glDeleteTextures= nullptr;
PFNGLBINDTEXTUREPROC    p_glBindTexture   = nullptr;
PFNGLTEXPARAMETERIPROC  p_glTexParameteri = nullptr;
PFNGLPIXELSTOREIPROC    p_glPixelStorei   = nullptr;
PFNGLTEXIMAGE2DPROC     p_glTexImage2D    = nullptr;
bool g_loaded = false;

template <typename Fn>
[[nodiscard]] bool load_proc(Fn& out, const char* name) noexcept {
    out = reinterpret_cast<Fn>(glfwGetProcAddress(name));
    if (!out) {
        std::fprintf(stderr, "[Texture2D] missing GL entry point: %s\n", name);
        return false;
    }
    return true;
}

[[nodiscard]] bool load_all_procs() noexcept {
    if (g_loaded) return true;
    bool ok = true;
    ok &= load_proc(p_glGetError,       "glGetError");
    ok &= load_proc(p_glGenTextures,    "glGenTextures");
    ok &= load_proc(p_glDeleteTextures, "glDeleteTextures");
    ok &= load_proc(p_glBindTexture,    "glBindTexture");
    ok &= load_proc(p_glTexParameteri,  "glTexParameteri");
    ok &= load_proc(p_glPixelStorei,    "glPixelStorei");
    ok &= load_proc(p_glTexImage2D,     "glTexImage2D");
    g_loaded = ok;
    return ok;
}

[[nodiscard]] bool gl_check(const char* op) noexcept {
    if (!p_glGetError) return true;
    const GLenum err = p_glGetError();
    if (err != GL_NO_ERROR) {
        std::fprintf(stderr, "[Texture2D] %s: glGetError=0x%X\n", op, err);
        return false;
    }
    return true;
}

} // anonymous

namespace phyriad::render::opengl3 {

Texture2D::Texture2D(Texture2D&& other) noexcept
    : id_(other.id_), w_(other.w_), h_(other.h_)
{
    other.id_ = 0u;
    other.w_  = 0u;
    other.h_  = 0u;
}

Texture2D& Texture2D::operator=(Texture2D&& other) noexcept {
    if (this != &other) {
        destroy();
        id_ = std::exchange(other.id_, 0u);
        w_  = std::exchange(other.w_,  0u);
        h_  = std::exchange(other.h_,  0u);
    }
    return *this;
}

bool Texture2D::upload_rgba8(uint32_t    width,
                             uint32_t    height,
                             void const* pixels) noexcept
{
    if (!load_all_procs()) return false;
    if (width == 0u || height == 0u) {
        std::fprintf(stderr, "[Texture2D] upload_rgba8: zero extent\n");
        return false;
    }

    GLuint tex = id_;
    if (tex == 0u) {
        p_glGenTextures(1, &tex);
        if (!gl_check("glGenTextures") || tex == 0u) return false;
    }

    p_glBindTexture(GL_TEXTURE_2D, tex);
    p_glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,     GL_CLAMP_TO_EDGE);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,     GL_CLAMP_TO_EDGE);
    p_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8,
                   static_cast<GLsizei>(width),
                   static_cast<GLsizei>(height),
                   0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);

    if (!gl_check("glTexImage2D")) {
        // If we just created the name, don't leak it on failure.
        if (id_ == 0u && tex != 0u) p_glDeleteTextures(1, &tex);
        return false;
    }

    id_ = tex;
    w_  = width;
    h_  = height;
    return true;
}

void Texture2D::destroy() noexcept {
    if (id_ != 0u && p_glDeleteTextures) {
        p_glDeleteTextures(1, &id_);
    }
    id_ = 0u;
    w_  = 0u;
    h_  = 0u;
}

} // namespace phyriad::render::opengl3
// Made with my soul - Swately <3
