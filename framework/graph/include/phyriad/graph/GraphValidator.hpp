// framework/graph/include/phyriad/graph/GraphValidator.hpp
// Runtime validation of a wired graph's wire topology.
//
// Three orthogonal checks:
//
//   validate_type_compat(wires):
//     Each wire's message_hash must be non-zero. Type compatibility between
//     outlet and inlet is guaranteed at compile time by connect()'s type
//     constraints, so this check catches only degenerate/unset descriptors.
//
//   validate_acyclicity(wires, node_count):
//     DFS with three-color marking (white=unvisited, gray=in-stack, black=done).
//     Returns CycleDetected with the cycle-entry node_id if a back-edge is found.
//     O(V + E).
//
//   validate_reachability(wires, node_count):
//     BFS from all source nodes (nodes with no incoming wires).
//     Any node not reached → UnreachableNode.
//     A graph with no source nodes at all → every node reported unreachable.
//     O(V + E).
//
//   validate_all(wires, node_count):
//     Runs all three checks; returns first failure encountered.
//
// All functions are noexcept and do not allocate on heap beyond what the
// internal vectors require. Called at graph-build time (cold path) — heap
// allocation is acceptable.
//
// §3.E of PHASE_H_IMPLEMENTATION_PATTERNS.md

#pragma once
#include <phyriad/schema/SchemaDescriptor.hpp>
#include <cstdint>
#include <span>
#include <vector>

namespace phyriad::graph {

// ── ValidationResult ─────────────────────────────────────────────────────
struct ValidationResult {
    enum class Code : uint8_t {
        OK               = 0,
        TypeMismatch     = 1,   // wire has zero message_hash
        CycleDetected    = 2,   // graph contains a directed cycle
        UnreachableNode  = 3,   // a node has no path from any source
        EmptyGraph       = 4,   // no nodes or no wires in a non-trivial graph
    };

    Code     code   {Code::OK};
    uint32_t node_id{0};   // offending node (CycleDetected / UnreachableNode)
    uint32_t wire_id{0};   // offending wire (TypeMismatch)

    [[nodiscard]] bool ok() const noexcept { return code == Code::OK; }

    // Convenience factory for the OK case.
    [[nodiscard]] static constexpr ValidationResult ok_result() noexcept {
        return {};
    }
};

// ── GraphValidator ────────────────────────────────────────────────────────
class GraphValidator {
public:
    // ── validate_type_compat ──────────────────────────────────────────────
    // Each wire must have a non-zero message_hash.
    // (Type compatibility itself is enforced at compile time by connect().)
    [[nodiscard]] static ValidationResult validate_type_compat(
        std::span<schema::WireDescriptor const> wires) noexcept
    {
        for (auto const& w : wires) {
            if (w.message_hash.is_zero()) [[unlikely]] {
                return ValidationResult{
                    .code    = ValidationResult::Code::TypeMismatch,
                    .node_id = w.src_node_id,
                    .wire_id = w.wire_id};
            }
        }
        return ValidationResult::ok_result();
    }

    // ── validate_acyclicity ───────────────────────────────────────────────
    // DFS three-color cycle detection. Returns CycleDetected{node_id=cycle_entry}
    // if any back-edge is found.
    [[nodiscard]] static ValidationResult validate_acyclicity(
        std::span<schema::WireDescriptor const> wires,
        uint32_t node_count) noexcept
    {
        if (node_count == 0) return ValidationResult::ok_result();

        // Build adjacency list (src → [dst]).
        std::vector<std::vector<uint32_t>> adj(node_count);
        for (auto const& w : wires) {
            if (w.src_node_id < node_count && w.dst_node_id < node_count) {
                adj[w.src_node_id].push_back(w.dst_node_id);
            }
        }

        enum class Color : uint8_t { White = 0, Gray = 1, Black = 2 };
        std::vector<Color> color(node_count, Color::White);

        // Iterative DFS to avoid stack overflow on large graphs.
        std::vector<std::pair<uint32_t, std::size_t>> stack;  // (node, adj_index)
        stack.reserve(node_count);

        for (uint32_t start = 0; start < node_count; ++start) {
            if (color[start] != Color::White) continue;

            stack.push_back({start, 0});
            color[start] = Color::Gray;

            while (!stack.empty()) {
                auto& [node, idx] = stack.back();
                if (idx < adj[node].size()) {
                    const uint32_t next = adj[node][idx++];
                    if (color[next] == Color::Gray) [[unlikely]] {
                        return ValidationResult{
                            .code    = ValidationResult::Code::CycleDetected,
                            .node_id = next,
                            .wire_id = 0};
                    }
                    if (color[next] == Color::White) {
                        color[next] = Color::Gray;
                        stack.push_back({next, 0});
                    }
                } else {
                    color[node] = Color::Black;
                    stack.pop_back();
                }
            }
        }
        return ValidationResult::ok_result();
    }

    // ── validate_reachability ─────────────────────────────────────────────
    // BFS from all source nodes (nodes with in_degree == 0).
    // Any node not reachable → UnreachableNode.
    [[nodiscard]] static ValidationResult validate_reachability(
        std::span<schema::WireDescriptor const> wires,
        uint32_t node_count) noexcept
    {
        if (node_count == 0) return ValidationResult::ok_result();
        // A single-node graph with no wires is trivially reachable from itself.
        if (wires.empty()) {
            return (node_count == 1)
                ? ValidationResult::ok_result()
                : ValidationResult{
                    .code    = ValidationResult::Code::UnreachableNode,
                    .node_id = 1,
                    .wire_id = 0};
        }

        // Compute in-degree per node.
        std::vector<uint32_t> indegree(node_count, 0u);
        std::vector<std::vector<uint32_t>> adj(node_count);
        for (auto const& w : wires) {
            if (w.src_node_id < node_count && w.dst_node_id < node_count) {
                ++indegree[w.dst_node_id];
                adj[w.src_node_id].push_back(w.dst_node_id);
            }
        }

        // BFS queue seeded with true source nodes:
        //   in_degree == 0  AND  at least one outgoing edge.
        // Nodes with neither incoming nor outgoing edges are isolated and
        // will correctly be reported as UnreachableNode.
        std::vector<bool>     visited(node_count, false);
        std::vector<uint32_t> queue;
        queue.reserve(node_count);

        for (uint32_t i = 0; i < node_count; ++i) {
            if (indegree[i] == 0 && !adj[i].empty()) {
                queue.push_back(i);
                visited[i] = true;
            }
        }

        for (std::size_t qi = 0; qi < queue.size(); ++qi) {
            const uint32_t node = queue[qi];
            for (uint32_t dst : adj[node]) {
                if (!visited[dst]) {
                    visited[dst] = true;
                    queue.push_back(dst);
                }
            }
        }

        for (uint32_t i = 0; i < node_count; ++i) {
            if (!visited[i]) [[unlikely]] {
                return ValidationResult{
                    .code    = ValidationResult::Code::UnreachableNode,
                    .node_id = i,
                    .wire_id = 0};
            }
        }
        return ValidationResult::ok_result();
    }

    // ── validate_all ──────────────────────────────────────────────────────
    // Run all checks in order: type_compat → acyclicity → reachability.
    [[nodiscard]] static ValidationResult validate_all(
        std::span<schema::WireDescriptor const> wires,
        uint32_t node_count) noexcept
    {
        if (auto r = validate_type_compat(wires);    !r.ok()) return r;
        if (auto r = validate_acyclicity(wires, node_count); !r.ok()) return r;
        if (auto r = validate_reachability(wires, node_count); !r.ok()) return r;
        return ValidationResult::ok_result();
    }
};

} // namespace phyriad::graph
// Made with my soul - Swately <3
