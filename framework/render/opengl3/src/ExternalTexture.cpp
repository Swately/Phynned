// framework/render/opengl3/src/ExternalTexture.cpp
// Implementation — see header for the contract.
//
// Imports a Vulkan-allocated memory handle into an OpenGL texture via
// the GL_EXT_external_objects extension family. Function pointers are
// resolved through glfwGetProcAddress at init time; no GLAD/GLEW.
//
#include <phyriad/render/opengl3/ExternalTexture.hpp>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#ifdef _WIN32
#   ifndef WIN32_LEAN_AND_MEAN
#       define WIN32_LEAN_AND_MEAN
#   endif
#   ifndef NOMINMAX
#       define NOMINMAX
#   endif
#   include <windows.h>      // HANDLE, CloseHandle
#else
#   include <unistd.h>       // close(fd)
#endif

#include <cstdio>
#include <cstring>

// ─────────────────────────────────────────────────────────────────────────────
// Minimal GL types + entry points needed by this TU.
// Same convention as FSR1Pass.cpp — we don't pull in a GL loader library,
// we just declare the subset of glcorearb.h + GL_EXT_external_objects we use.
// ─────────────────────────────────────────────────────────────────────────────
#ifndef APIENTRY
#   ifdef _WIN32
#       define APIENTRY __stdcall
#   else
#       define APIENTRY
#   endif
#endif
#define APIENTRYP APIENTRY *

typedef unsigned int   GLenum;
typedef unsigned int   GLuint;
typedef int            GLint;
typedef int            GLsizei;
typedef unsigned char  GLubyte;
typedef unsigned long long GLuint64;

// Selected constants we use.
#define GL_NO_ERROR                              0
#define GL_INVALID_ENUM                          0x0500
#define GL_INVALID_VALUE                         0x0501
#define GL_INVALID_OPERATION                     0x0502
#define GL_TEXTURE_2D                            0x0DE1
#define GL_NUM_EXTENSIONS                        0x821D
#define GL_EXTENSIONS                            0x1F03
// Filter / wrap parameters (we set sane defaults so the imported
// texture is sampleable without further configuration).
#define GL_NEAREST                               0x2600
#define GL_LINEAR                                0x2601
#define GL_TEXTURE_MAG_FILTER                    0x2800
#define GL_TEXTURE_MIN_FILTER                    0x2801
#define GL_TEXTURE_WRAP_S                        0x2802
#define GL_TEXTURE_WRAP_T                        0x2803
#define GL_CLAMP_TO_EDGE                         0x812F
// GL_EXT_external_objects:
#define GL_HANDLE_TYPE_OPAQUE_WIN32_EXT          0x9587
#define GL_HANDLE_TYPE_OPAQUE_FD_EXT             0x9586
// Tiling: we asked Vulkan for OPTIMAL tiling. The GL side needs the
// matching parameter so its driver lays out the texture correctly.
#define GL_TEXTURE_TILING_EXT                    0x9580
#define GL_OPTIMAL_TILING_EXT                    0x9584
#define GL_LINEAR_TILING_EXT                     0x9585
// GL_EXT_memory_object texture parameter:
#define GL_DEDICATED_MEMORY_OBJECT_EXT           0x9581

// Function-pointer types.
typedef GLenum     (APIENTRYP PFNGLGETERRORPROC)(void);
typedef const GLubyte* (APIENTRYP PFNGLGETSTRINGIPROC)(GLenum name, GLuint index);
typedef void       (APIENTRYP PFNGLGETINTEGERVPROC)(GLenum pname, GLint* data);
typedef void       (APIENTRYP PFNGLCREATETEXTURESPROC)(GLenum target, GLsizei n, GLuint* textures);
typedef void       (APIENTRYP PFNGLDELETETEXTURESPROC)(GLsizei n, const GLuint* textures);
typedef void       (APIENTRYP PFNGLTEXTUREPARAMETERIPROC)(GLuint tex, GLenum pname, GLint param);
// GL_EXT_memory_object:
typedef void       (APIENTRYP PFNGLCREATEMEMORYOBJECTSEXTPROC)(GLsizei n, GLuint* memoryObjects);
typedef void       (APIENTRYP PFNGLDELETEMEMORYOBJECTSEXTPROC)(GLsizei n, const GLuint* memoryObjects);
typedef void       (APIENTRYP PFNGLMEMORYOBJECTPARAMETERIVEXTPROC)(GLuint memoryObject, GLenum pname, const GLint* params);
typedef void       (APIENTRYP PFNGLTEXTURESTORAGEMEM2DEXTPROC)(
    GLuint texture, GLsizei levels, GLenum internalFormat,
    GLsizei width, GLsizei height, GLuint memory, GLuint64 offset);
#ifdef _WIN32
typedef void       (APIENTRYP PFNGLIMPORTMEMORYWIN32HANDLEEXTPROC)(
    GLuint memory, GLuint64 size, GLenum handleType, void* handle);
#else
typedef void       (APIENTRYP PFNGLIMPORTMEMORYFDEXTPROC)(
    GLuint memory, GLuint64 size, GLenum handleType, GLint fd);
#endif

// File-local function pointers (resolved once at first init() call).
namespace {

PFNGLGETERRORPROC                       p_glGetError                    = nullptr;
PFNGLGETSTRINGIPROC                     p_glGetStringi                  = nullptr;
PFNGLGETINTEGERVPROC                    p_glGetIntegerv                 = nullptr;
PFNGLCREATETEXTURESPROC                 p_glCreateTextures              = nullptr;
PFNGLDELETETEXTURESPROC                 p_glDeleteTextures              = nullptr;
PFNGLTEXTUREPARAMETERIPROC              p_glTextureParameteri           = nullptr;
PFNGLCREATEMEMORYOBJECTSEXTPROC         p_glCreateMemoryObjectsEXT      = nullptr;
PFNGLDELETEMEMORYOBJECTSEXTPROC         p_glDeleteMemoryObjectsEXT      = nullptr;
PFNGLMEMORYOBJECTPARAMETERIVEXTPROC     p_glMemoryObjectParameterivEXT  = nullptr;
PFNGLTEXTURESTORAGEMEM2DEXTPROC         p_glTextureStorageMem2DEXT      = nullptr;
#ifdef _WIN32
PFNGLIMPORTMEMORYWIN32HANDLEEXTPROC     p_glImportMemoryWin32HandleEXT  = nullptr;
#else
PFNGLIMPORTMEMORYFDEXTPROC              p_glImportMemoryFdEXT           = nullptr;
#endif
bool g_loaded = false;

template <typename Fn>
[[nodiscard]] bool load_proc(Fn& out, const char* name) noexcept {
    out = reinterpret_cast<Fn>(glfwGetProcAddress(name));
    if (!out) {
        std::fprintf(stderr,
            "[ExternalTexture] missing GL entry point: %s\n", name);
        return false;
    }
    return true;
}

[[nodiscard]] bool load_all_procs() noexcept {
    if (g_loaded) return true;
    bool ok = true;
    ok &= load_proc(p_glGetError,                   "glGetError");
    ok &= load_proc(p_glGetStringi,                 "glGetStringi");
    ok &= load_proc(p_glGetIntegerv,                "glGetIntegerv");
    ok &= load_proc(p_glCreateTextures,             "glCreateTextures");
    ok &= load_proc(p_glDeleteTextures,             "glDeleteTextures");
    ok &= load_proc(p_glTextureParameteri,          "glTextureParameteri");
    ok &= load_proc(p_glCreateMemoryObjectsEXT,     "glCreateMemoryObjectsEXT");
    ok &= load_proc(p_glDeleteMemoryObjectsEXT,     "glDeleteMemoryObjectsEXT");
    ok &= load_proc(p_glMemoryObjectParameterivEXT, "glMemoryObjectParameterivEXT");
    ok &= load_proc(p_glTextureStorageMem2DEXT,     "glTextureStorageMem2DEXT");
#ifdef _WIN32
    ok &= load_proc(p_glImportMemoryWin32HandleEXT, "glImportMemoryWin32HandleEXT");
#else
    ok &= load_proc(p_glImportMemoryFdEXT,          "glImportMemoryFdEXT");
#endif
    g_loaded = ok;
    return ok;
}

[[nodiscard]] bool gl_check(const char* op) noexcept {
    if (!p_glGetError) return true;   // we'll surface the load failure separately
    const GLenum err = p_glGetError();
    if (err != GL_NO_ERROR) {
        std::fprintf(stderr, "[ExternalTexture] %s: glGetError=0x%X\n", op, err);
        return false;
    }
    return true;
}

[[nodiscard]] bool has_extension(const char* name) noexcept {
    if (!p_glGetIntegerv || !p_glGetStringi) return false;
    GLint n = 0;
    p_glGetIntegerv(GL_NUM_EXTENSIONS, &n);
    for (GLint i = 0; i < n; ++i) {
        const GLubyte* s = p_glGetStringi(GL_EXTENSIONS, static_cast<GLuint>(i));
        if (s && std::strcmp(reinterpret_cast<const char*>(s), name) == 0)
            return true;
    }
    return false;
}

} // anonymous

namespace phyriad::render::opengl3 {

// ─────────────────────────────────────────────────────────────────────────────
// extensions_available
// ─────────────────────────────────────────────────────────────────────────────
bool ExternalTexture::extensions_available() noexcept {
    if (!load_all_procs()) return false;

    bool ok = true;
    ok &= has_extension("GL_EXT_memory_object");
#ifdef _WIN32
    ok &= has_extension("GL_EXT_memory_object_win32");
#else
    ok &= has_extension("GL_EXT_memory_object_fd");
#endif
    return ok;
}

// ─────────────────────────────────────────────────────────────────────────────
// init
// ─────────────────────────────────────────────────────────────────────────────
#ifdef _WIN32
bool ExternalTexture::init(void*    win32_handle,
                           uint64_t vk_memory_size,
                           uint32_t width,
                           uint32_t height,
                           uint32_t gl_internal_format,
                           Tiling   tiling) noexcept
{
    if (valid()) {
        std::fprintf(stderr, "[ExternalTexture] already initialized\n");
        return true;
    }
    if (!load_all_procs()) return false;
    if (!extensions_available()) {
        std::fprintf(stderr,
            "[ExternalTexture] GL_EXT_memory_object{,_win32} unavailable\n");
        return false;
    }
    if (!win32_handle || vk_memory_size == 0u ||
        width == 0u || height == 0u)
    {
        std::fprintf(stderr, "[ExternalTexture] init: invalid arguments\n");
        return false;
    }

    // ── 1. Create a memory object on the GL side.
    GLuint mem_obj = 0u;
    p_glCreateMemoryObjectsEXT(1, &mem_obj);
    if (!gl_check("glCreateMemoryObjectsEXT") || mem_obj == 0u) return false;

    // Mark the memory object as "dedicated" — Vulkan allocations of an
    // image's memory are always dedicated to that image. Without this
    // parameter the GL driver might try to alias the import with other
    // resources and fail.
    {
        const GLint dedicated = 1;
        p_glMemoryObjectParameterivEXT(mem_obj,
            GL_DEDICATED_MEMORY_OBJECT_EXT, &dedicated);
        if (!gl_check("glMemoryObjectParameterivEXT(dedicated)")) {
            p_glDeleteMemoryObjectsEXT(1, &mem_obj);
            return false;
        }
    }

    // ── 2. Import the Vulkan-exported HANDLE into the memory object.
    // GL takes ownership of the handle (per the OPAQUE_WIN32 spec).
    p_glImportMemoryWin32HandleEXT(
        mem_obj, vk_memory_size,
        GL_HANDLE_TYPE_OPAQUE_WIN32_EXT, win32_handle);
    if (!gl_check("glImportMemoryWin32HandleEXT")) {
        p_glDeleteMemoryObjectsEXT(1, &mem_obj);
        return false;
    }

    // ── 3. Create the texture using DSA + bind to the imported memory.
    GLuint tex = 0u;
    p_glCreateTextures(GL_TEXTURE_2D, 1, &tex);
    if (!gl_check("glCreateTextures") || tex == 0u) {
        p_glDeleteMemoryObjectsEXT(1, &mem_obj);
        return false;
    }

    // Match the Vulkan-side OPTIMAL tiling so the driver doesn't reject
    // the import. (glTextureStorageMem2DEXT does NOT take a tiling param
    // directly — it's a texture parameter you set first.)
    // GL_TEXTURE_TILING_EXT must match the VK side. NVIDIA's GL driver
    // rejects LINEAR for imported images that have COLOR_ATTACHMENT in
    // their VK usage flags (it allocates render-target-compressed
    // layouts in that case), but accepts LINEAR for SAMPLED+TRANSFER_DST
    // images. OPTIMAL works for color attachments but yields
    // tile-swizzle artefacts on direct sampling on some NVIDIA driver
    // versions; LINEAR is the safer choice for sampling-only interop.
    const GLint tiling_param = (tiling == Tiling::Linear)
        ? static_cast<GLint>(GL_LINEAR_TILING_EXT)
        : static_cast<GLint>(GL_OPTIMAL_TILING_EXT);
    p_glTextureParameteri(tex, GL_TEXTURE_TILING_EXT, tiling_param);
    if (!gl_check("glTextureParameteri(TILING_EXT)")) {
        p_glDeleteTextures(1, &tex);
        p_glDeleteMemoryObjectsEXT(1, &mem_obj);
        return false;
    }

    // Set sample defaults BEFORE allocating storage so the texture is
    // immediately complete after import. Without these, the default
    // MIN_FILTER is GL_NEAREST_MIPMAP_LINEAR which makes a single-mip
    // texture "mipmap incomplete" — sampling returns undefined data
    // (typically garbled noise or the GPU's previous memory contents).
    p_glTextureParameteri(tex, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    p_glTextureParameteri(tex, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    p_glTextureParameteri(tex, GL_TEXTURE_WRAP_S,     GL_CLAMP_TO_EDGE);
    p_glTextureParameteri(tex, GL_TEXTURE_WRAP_T,     GL_CLAMP_TO_EDGE);
    if (!gl_check("glTextureParameteri(filter/wrap)")) {
        p_glDeleteTextures(1, &tex);
        p_glDeleteMemoryObjectsEXT(1, &mem_obj);
        return false;
    }

    p_glTextureStorageMem2DEXT(tex, 1,
        static_cast<GLenum>(gl_internal_format),
        static_cast<GLsizei>(width),
        static_cast<GLsizei>(height),
        mem_obj, /*offset*/ 0ull);
    if (!gl_check("glTextureStorageMem2DEXT")) {
        p_glDeleteTextures(1, &tex);
        p_glDeleteMemoryObjectsEXT(1, &mem_obj);
        return false;
    }

    memory_object_id_ = mem_obj;
    texture_id_       = tex;
    w_                = width;
    h_                = height;
    return true;
}
#else
bool ExternalTexture::init(int      fd,
                           uint64_t vk_memory_size,
                           uint32_t width,
                           uint32_t height,
                           uint32_t gl_internal_format,
                           Tiling   tiling) noexcept
{
    if (valid()) return true;
    if (!load_all_procs())                       return false;
    if (!extensions_available())                 return false;
    if (fd < 0 || vk_memory_size == 0u ||
        width == 0u || height == 0u)             return false;

    GLuint mem_obj = 0u;
    p_glCreateMemoryObjectsEXT(1, &mem_obj);
    if (!gl_check("glCreateMemoryObjectsEXT") || mem_obj == 0u) return false;

    const GLint dedicated = 1;
    p_glMemoryObjectParameterivEXT(mem_obj,
        GL_DEDICATED_MEMORY_OBJECT_EXT, &dedicated);
    if (!gl_check("glMemoryObjectParameterivEXT(dedicated)")) {
        p_glDeleteMemoryObjectsEXT(1, &mem_obj); return false;
    }

    p_glImportMemoryFdEXT(mem_obj, vk_memory_size,
        GL_HANDLE_TYPE_OPAQUE_FD_EXT, fd);
    if (!gl_check("glImportMemoryFdEXT")) {
        p_glDeleteMemoryObjectsEXT(1, &mem_obj); return false;
    }

    GLuint tex = 0u;
    p_glCreateTextures(GL_TEXTURE_2D, 1, &tex);
    if (!gl_check("glCreateTextures") || tex == 0u) {
        p_glDeleteMemoryObjectsEXT(1, &mem_obj); return false;
    }
    // GL_TEXTURE_TILING_EXT must match the VK side. NVIDIA's GL driver
    // rejects LINEAR for imported images that have COLOR_ATTACHMENT in
    // their VK usage flags (it allocates render-target-compressed
    // layouts in that case), but accepts LINEAR for SAMPLED+TRANSFER_DST
    // images. OPTIMAL works for color attachments but yields
    // tile-swizzle artefacts on direct sampling on some NVIDIA driver
    // versions; LINEAR is the safer choice for sampling-only interop.
    const GLint tiling_param = (tiling == Tiling::Linear)
        ? static_cast<GLint>(GL_LINEAR_TILING_EXT)
        : static_cast<GLint>(GL_OPTIMAL_TILING_EXT);
    p_glTextureParameteri(tex, GL_TEXTURE_TILING_EXT, tiling_param);
    if (!gl_check("glTextureParameteri(TILING_EXT)")) {
        p_glDeleteTextures(1, &tex);
        p_glDeleteMemoryObjectsEXT(1, &mem_obj); return false;
    }

    p_glTextureStorageMem2DEXT(tex, 1,
        static_cast<GLenum>(gl_internal_format),
        static_cast<GLsizei>(width),
        static_cast<GLsizei>(height),
        mem_obj, /*offset*/ 0ull);
    if (!gl_check("glTextureStorageMem2DEXT")) {
        p_glDeleteTextures(1, &tex);
        p_glDeleteMemoryObjectsEXT(1, &mem_obj); return false;
    }

    memory_object_id_ = mem_obj;
    texture_id_       = tex;
    w_                = width;
    h_                = height;
    return true;
}
#endif

// ─────────────────────────────────────────────────────────────────────────────
// shutdown
// ─────────────────────────────────────────────────────────────────────────────
void ExternalTexture::shutdown() noexcept {
    if (texture_id_ != 0u && p_glDeleteTextures) {
        p_glDeleteTextures(1, &texture_id_);
        texture_id_ = 0u;
    }
    if (memory_object_id_ != 0u && p_glDeleteMemoryObjectsEXT) {
        p_glDeleteMemoryObjectsEXT(1, &memory_object_id_);
        memory_object_id_ = 0u;
    }
    w_ = 0u;
    h_ = 0u;
}

} // namespace phyriad::render::opengl3
// Made with my soul - Swately <3
