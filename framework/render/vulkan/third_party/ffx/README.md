# AMD FidelityFX SDK — third_party/ffx

This directory is the expected location for the AMD FidelityFX SDK (v1.1+),
required to build FSR2Pass and FSR3Pass with `PHYRIAD_BUILD_FSR2=ON`.

## How to obtain

```bash
# Clone the SDK into this directory:
git clone --depth 1 \
    https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK.git \
    pillars/render/vulkan/third_party/ffx
```

Or use the standalone FSR2 repo:
```bash
git clone --depth 1 --branch v2.3.2 \
    https://github.com/GPUOpen-Effects/FidelityFX-FSR2.git \
    pillars/render/vulkan/third_party/ffx
```

## Expected layout after checkout

```
third_party/ffx/
  include/
    FfxFsr2.h
    vk/
      ffx_fsr2_vk.h
  lib/
    x64/
      *.lib    (Windows)  or
      *.a      (Linux)
  src/
    ...
```

## CMake detection

`pillars/render/vulkan/CMakeLists.txt` detects the SDK by checking for
`third_party/ffx/include/FfxFsr2.h`.  When found, FSR2Pass and FSR3Pass
compile against the real SDK with `PHYRIAD_HAS_FFX_SDK=1`.  When absent,
stub implementations are compiled with `PHYRIAD_HAS_FFX_SDK=0`; all API
calls return `ErrorCode::ResourceInitFailed`.

## License

The FidelityFX SDK is MIT licensed.
See `third_party/ffx/LICENSE` after checkout.
