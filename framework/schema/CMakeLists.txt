cmake_minimum_required(VERSION 3.20)
project(phyriad_schema LANGUAGES CXX)

# ── phyriad_schema — pillar schema (INTERFACE, header-only) ─────────────────────
# Expone las API de mensajes y fingerprinting de esquemas al resto del sistema.
#
# Headers:
#   xxh3_inline.hpp     — consteval XXH3-128 (seed=0), sin dependencias
#   SchemaHash.hpp      — Hash128, XXH3State, schema_hash<T>(), kPhyriadHalVersion
#   PodMessage.hpp      — PodMessage concept, PHYRIAD_ASSERT_POD, kMaxPodSize
#   CapnpMessage.hpp    — CapnpMessage concept, PHYRIAD_REGISTER_CAPNP (Tier 1)
#   OpaqueMessage.hpp   — OpaqueMessage concept, OpaqueSpan (Tier 2)
#   SchemaDescriptor.hpp— GraphSchemaDescriptor (64B, alignas(64)), WireDescriptor,
#                         make_schema_descriptor(), validate_schema_descriptor()
#   MessageTier.hpp     — Tier0/1/2 concepts, AnyMessage, tier_of<T>
#   Schema.hpp          — umbrella include
#
# Dependencias directas:
#   - phyriad_hal (INTERFACE) — expone hal::kDestructivePad para SchemaHash.hpp

# ── Resolución de phyriad_hal en build standalone ────────────────────────────────
if(NOT TARGET phyriad_hal)
    add_subdirectory(
        ${CMAKE_CURRENT_SOURCE_DIR}/../hal
        ${CMAKE_CURRENT_BINARY_DIR}/phyriad_hal
    )
endif()

# ── Target INTERFACE (header-only, no .a producida) ──────────────────────────
add_library(phyriad_schema INTERFACE)

target_include_directories(phyriad_schema INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(phyriad_schema INTERFACE cxx_std_23)

# phyriad_hal transitivamente expone <phyriad/hal/...> a todos los dependientes.
target_link_libraries(phyriad_schema INTERFACE phyriad_hal)

# ── Tests ─────────────────────────────────────────────────────────────────────
if(BUILD_TESTING OR PHYRIAD_BUILD_TESTS)
    include(CTest)

    add_executable(schema_types_test
        tests/schema_types_test.cpp
    )
    target_link_libraries(schema_types_test PRIVATE phyriad_schema)
    target_compile_features(schema_types_test PRIVATE cxx_std_23)

    add_test(
        NAME    schema_types_test
        COMMAND schema_types_test
        LABELS  schema
    )

    # ── blind_proxy_test — CapabilityRequest + PassthroughResponse POD smoke ──
    add_executable(blind_proxy_test
        tests/blind_proxy_test.cpp
    )
    target_link_libraries(blind_proxy_test PRIVATE phyriad_schema)
    target_compile_features(blind_proxy_test PRIVATE cxx_std_23)

    add_test(
        NAME    blind_proxy_test
        COMMAND blind_proxy_test
        LABELS  schema
    )

    # ── schema_hash_conformance_test — the ABI-STABILITY gate (§W.1) ──────────
    # Pins schema_hash / xxh3_128 against silent drift: raw-engine goldens are
    # compile-time static_asserts (a drift fails the BUILD on every platform);
    # schema_hash<T> goldens are runtime + ABI-guarded. A deliberate ABI change
    # (kPhyriadHalVersion bump) is meant to fail this until the goldens are re-captured.
    add_executable(schema_hash_conformance_test
        tests/schema_hash_conformance_test.cpp
    )
    target_link_libraries(schema_hash_conformance_test PRIVATE phyriad_schema)
    target_compile_features(schema_hash_conformance_test PRIVATE cxx_std_23)

    add_test(
        NAME    schema_hash_conformance_test
        COMMAND schema_hash_conformance_test
        LABELS  schema
    )
endif()
# Made with my soul - Swately <3
