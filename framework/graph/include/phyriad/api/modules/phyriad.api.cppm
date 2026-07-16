// pillars/graph/include/phyriad/api/modules/phyriad.api.cppm
// C++23 module: phyriad.api
//
// Aggregates the full Phyriad user API: DSL + schema descriptor types +
// placement types + error types. Consumers write:
//
//   import phyriad.api;
//
// Everything in phyriad.dsl is available, plus the lower-level schema and
// scheduler types needed to inspect BuiltGraph, write SHM, or feed the
// Scheduler directly.
//
// §1.5 of PHASE_H_IMPLEMENTATION_PATTERNS.md

module;

// ── Global module fragment ────────────────────────────────────────────────────
#include <phyriad/api/GraphDSL.hpp>
#include <phyriad/schema/SchemaDescriptor.hpp>
#include <phyriad/scheduler/Placement.hpp>
#include <phyriad/schema/Error.hpp>

export module phyriad.api;

// Re-export the complete DSL layer.
export import phyriad.dsl;

// ── Schema descriptor types ───────────────────────────────────────────────────
export using gma::schema::GraphSchemaDescriptor;
export using gma::schema::NodeDescriptor;
export using gma::schema::WireDescriptor;
export using gma::schema::Hash128;
export using gma::schema::make_schema_descriptor;
export using gma::schema::validate_schema_descriptor;

// ── Placement types ───────────────────────────────────────────────────────────
export using gma::scheduler::PlacementHint;
export using gma::scheduler::PlacementPlan;
export using gma::scheduler::NodeAssignment;
export using gma::scheduler::ResourceBudget;

// ── Core error types ──────────────────────────────────────────────────────────
export using gma::Error;
export using gma::ErrorCode;
export using gma::NodeId;
// Made with my soul - Swately <3
