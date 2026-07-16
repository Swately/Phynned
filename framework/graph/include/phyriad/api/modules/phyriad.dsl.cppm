// pillars/graph/include/phyriad/api/modules/phyriad.dsl.cppm
// C++23 module: phyriad.dsl
//
// Wraps the fluent graph-construction DSL into a named module.
// Consumers write:
//
//   import phyriad.dsl;
//
//   auto g = gma::api::build_graph()
//       .node<MySource>("src", gma::placement::vcache_pinned())
//       .wire("src").to("sink")
//       .validate()
//       .build();
//
// CMake setup (requires CMake 3.28+ and a compiler with module support):
//   target_sources(gamma_graph PRIVATE
//       FILE_SET CXX_MODULES FILES
//           include/phyriad/api/modules/phyriad.dsl.cppm
//           include/phyriad/api/modules/phyriad.api.cppm)
//
// §1.5 of PHASE_H_IMPLEMENTATION_PATTERNS.md

module;

// ── Global module fragment ────────────────────────────────────────────────────
// Included here (NOT exported). Entities become available to the module
// purview; we selectively re-export via the export using declarations below.
#include <phyriad/api/GraphDSL.hpp>
#include <phyriad/api/NodeBuilder.hpp>
#include <phyriad/api/WireBuilder.hpp>
#include <phyriad/api/Validation.hpp>

export module phyriad.dsl;

// ── DSL core exports ──────────────────────────────────────────────────────────
export using gma::api::DslGraphBuilder;
export using gma::api::BuiltGraph;
export using gma::api::WireBuilder;
export using gma::api::WirePolicy;
export using gma::api::ConfigError;
export using gma::api::ConfigErrorCode;
export using gma::api::build_graph;

// ── Placement factory exports ─────────────────────────────────────────────────
// Users access these as gma::placement::vcache_pinned() etc.
export using gma::placement::vcache_pinned;
export using gma::placement::numa;
export using gma::placement::realtime;
export using gma::placement::gpu_affine;
export using gma::placement::isolated;
export using gma::placement::co_locate_with;
// Made with my soul - Swately <3
