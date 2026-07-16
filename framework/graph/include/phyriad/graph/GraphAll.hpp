// framework/graph/include/phyriad/graph/GraphAll.hpp
// Convenience aggregator — includes all phyriad::graph and phyriad::api headers.
//
// For tight includes in production code, prefer the individual headers.
// Use this in tests, prototypes, and translation units that need everything.
//

#pragma once

// ── phyriad::graph ────────────────────────────────────────────────────────────────
#include "Graph.hpp"
#include "GraphBuilder.hpp"
#include "GraphValidator.hpp"
#include "StaticGraph.hpp"
#include "DynamicGraph.hpp"

// ── phyriad::api ──────────────────────────────────────────────────────────────────
#include <phyriad/api/Validation.hpp>
#include <phyriad/api/NodeBuilder.hpp>
#include <phyriad/api/WireBuilder.hpp>
#include <phyriad/api/NodeHandle.hpp>
#include <phyriad/api/WireHandle.hpp>
#include <phyriad/api/WireRegistry.hpp>
#include <phyriad/api/NodeRegistry.hpp>
#include <phyriad/api/GraphDSL.hpp>
#include <phyriad/api/placement.hpp>
// Made with my soul - Swately <3
