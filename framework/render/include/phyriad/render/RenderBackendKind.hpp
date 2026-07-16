// framework/render/include/phyriad/render/RenderBackendKind.hpp
// Enum for selecting the render backend at application startup.
//
// Used in ApplicationConfig::render_backend. The Application selects the
// concrete IRenderBackend implementation based on this value.
// Vulkan is gated on PHYRIAD_BUILD_VULKAN (Phase 3); Composite on Phase 4.
//

#pragma once
#include <cstdint>

namespace phyriad::render {

enum class RenderBackendKind : uint8_t {
    OpenGL3   = 0,  // OpenGL 3.3 Core + ImGui — always available
    Vulkan    = 1,  // Vulkan 1.2+ (Phase 3, requires PHYRIAD_BUILD_VULKAN)
    Composite = 2,  // Multi-backend compositor (Phase 4)
    Auto      = 3,  // Auto-select: Vulkan if available, else OpenGL3
};

} // namespace phyriad::render
// Made with my soul - Swately <3
