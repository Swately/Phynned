// framework/render/opengl3/include/phyriad/render/opengl3/ExternalTexture.hpp
// ExternalTexture — OpenGL texture imported from a Vulkan-allocated
// memory handle via the GL_EXT_external_objects extension family.
//
// Companion to phyriad::render::vulkan::ExternalImage: the VK side
// exports a memory HANDLE (Windows) or fd (POSIX), this class imports
// that handle into a regular GL_TEXTURE_2D object. The two now share
// the same physical GPU memory; writes from Vulkan are visible to
// OpenGL once synchronization (Stage 4) completes.
//
// Required GL extensions (queried at init):
//   GL_EXT_memory_object              — core import API
//   GL_EXT_memory_object_win32        — Windows handle import
//   GL_EXT_memory_object_fd           — POSIX fd import
//   GL_EXT_semaphore                  — VK↔GL sync (Stage 4)
//   GL_EXT_semaphore_win32 / _fd      — platform semaphore import
//
// Required GL version: 4.5 (for direct-state-access glCreateTextures,
// glTextureStorageMem2DEXT). The opengl3 backend already requires 4.5.
//
// Lifetime:
//   ext_tex.init(ctx, vk_image_size, w, h, format, vk_handle)
//   → GL_TEXTURE_2D bound to the VK image's memory
//   ext_tex.texture() → GLuint name for use in your shaders
//   ext_tex.shutdown()
//
// The caller of init() transfers ownership of vk_handle to this object.
// shutdown() closes the handle (CloseHandle / close) and destroys the
// GL texture + memory object.
//
// Threading: a single ExternalTexture must only be touched by the
// thread holding the current GL context.
//
#pragma once
#include <cstdint>

namespace phyriad::render::opengl3 {

class ExternalTexture {
public:
    ExternalTexture()  noexcept = default;
    ~ExternalTexture() noexcept = default;

    ExternalTexture(ExternalTexture const&)            = delete;
    ExternalTexture& operator=(ExternalTexture const&) = delete;
    ExternalTexture(ExternalTexture&&)                 = delete;
    ExternalTexture& operator=(ExternalTexture&&)      = delete;

    // ── Init ──────────────────────────────────────────────────────────
    // Imports the platform-specific memory handle into a GL texture.
    //
    // Arguments:
    //   vk_memory_size — exact bytes from VkMemoryRequirements::size on
    //                    the Vulkan side. MUST match what Vulkan
    //                    allocated; OpenGL imports the whole allocation.
    //   width / height — texture extent in texels (match the VkImage).
    //   gl_internal_format — e.g. GL_RGBA8. Must match the VkFormat
    //                        on the Vulkan side (RGBA8 ↔ RGBA8_UNORM).
    //
    // Platform handle types — pick the right overload:
    //   Windows: import_handle_win32(HANDLE) → caller still owns handle
    //                                          (GL_EXT spec: import is by ref;
    //                                          GL bumps the kernel object's
    //                                          refcount, you must CloseHandle
    //                                          when done OR pass HANDLE_TYPE_OPAQUE_WIN32
    //                                          which Phyriad uses, where GL takes ownership)
    //   POSIX:   import_handle_fd(int)        → GL takes ownership of fd
    //                                          (per the GL_EXT_memory_object_fd spec).
    //
    // The Phyriad convention chosen here is:
    //   Windows: GL takes ownership of HANDLE (so shutdown closes it).
    //   POSIX:   GL takes ownership of fd     (so shutdown closes it).
    // Either way, after a successful init(), the caller MUST NOT also
    // close the handle/fd itself.
    //
    // Returns true on success. On failure: GL error is logged via
    // stderr, no resources retained, return false.
    // Tiling MUST match the VK_IMAGE_TILING_* used by the VK side.
    // LINEAR is row-major RGBA8; OPTIMAL is GPU-private swizzled.
    enum class Tiling : uint32_t { Optimal = 0u, Linear = 1u };

#ifdef _WIN32
    [[nodiscard]] bool init(void*       win32_handle,
                            uint64_t    vk_memory_size,
                            uint32_t    width,
                            uint32_t    height,
                            uint32_t    gl_internal_format,
                            Tiling      tiling = Tiling::Optimal) noexcept;
#else
    [[nodiscard]] bool init(int         fd,
                            uint64_t    vk_memory_size,
                            uint32_t    width,
                            uint32_t    height,
                            uint32_t    gl_internal_format,
                            Tiling      tiling = Tiling::Optimal) noexcept;
#endif

    // ── Shutdown ──────────────────────────────────────────────────────
    // Destroys the GL texture + memory object. Idempotent.
    void shutdown() noexcept;

    [[nodiscard]] bool     valid()    const noexcept { return texture_id_ != 0u; }
    [[nodiscard]] uint32_t texture()  const noexcept { return texture_id_; }
    [[nodiscard]] uint32_t width()    const noexcept { return w_; }
    [[nodiscard]] uint32_t height()   const noexcept { return h_; }

    // ── Static feature query ──────────────────────────────────────────
    // True when the current GL context exposes the extensions needed
    // for VK↔GL memory interop. Call after the GL context is current.
    // Returns true on success; on false, init() will fail.
    [[nodiscard]] static bool extensions_available() noexcept;

private:
    uint32_t memory_object_id_{0u};
    uint32_t texture_id_      {0u};
    uint32_t w_               {0u};
    uint32_t h_               {0u};
};

} // namespace phyriad::render::opengl3
// Made with my soul - Swately <3
