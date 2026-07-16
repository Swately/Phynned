// framework/node/include/phyriad/node/Mixins.hpp
// deducing-this mixins: Sender<T> and Receiver<T>.
//
// Sender<T>:   inherit to add publish(msg) → self.outlet().publish(msg).
// Receiver<T>: inherit to add poll()       → self.inlet().receive().
//
// Both use C++23 "deducing this" (P0847R7) — no CRTP boilerplate, fully inline.
// The deduced Self type gives the compiler the concrete node type at the call site,
// enabling total inlining of the chain: publish() → outlet().publish() → send_fn_().
//
// Concrete node requirements:
//   Sender<T>:   node must expose `Outlet<T>& outlet() noexcept`
//   Receiver<T>: node must expose `Inlet<T>&  inlet()  noexcept`
//
// Compiler support check: PHYRIAD_HAS_DEDUCING_THIS
//   GCC 14+, Clang 18+, MSVC 19.37+ all implement P0847R7 fully.
//   If PHYRIAD_HAS_DEDUCING_THIS == 0 an alternative SenderCrtp<T,Derived> /
//   ReceiverCrtp<T,Derived> is provided as fallback at the cost of one extra
//   template parameter at inheritance site.
//

#pragma once
#include "Port.hpp"
#include <phyriad/schema/Error.hpp>
#include <phyriad/schema/PodMessage.hpp>
#include <expected>

// ── Compiler capability detection ────────────────────────────────────────────
#if defined(__GNUC__) && !defined(__clang__) && __GNUC__ >= 14
    #define PHYRIAD_HAS_DEDUCING_THIS 1
#elif defined(__clang__) && __clang_major__ >= 18
    #define PHYRIAD_HAS_DEDUCING_THIS 1
#elif defined(_MSC_VER) && _MSC_VER >= 1937
    #define PHYRIAD_HAS_DEDUCING_THIS 1
#else
    #define PHYRIAD_HAS_DEDUCING_THIS 0
#endif

namespace phyriad::node {

// ── Sender<T> ─────────────────────────────────────────────────────────────
// Mixin: inherit to add publish(). Delegates to self.outlet().publish(msg).
// The deduced Self avoids the vtable indirection that virtual dispatch would add.
template <schema::PodMessage T>
struct Sender {
#if PHYRIAD_HAS_DEDUCING_THIS
    template <typename Self>
    [[nodiscard]] auto publish(this Self&& self, T const& msg) noexcept
        -> std::expected<void, phyriad::Error>
    {
        return self.outlet().publish(msg);
    }
#else
    // CRTP version in SenderCrtp<T,Derived> below — use that instead.
    static_assert(sizeof(T) == 0,
        "Sender<T> requires deducing this (GCC 14+ / Clang 18+ / MSVC 19.37+). "
        "Use SenderCrtp<T,Derived> on older compilers.");
#endif
};

// ── Receiver<T> ───────────────────────────────────────────────────────────
// Mixin: inherit to add poll(). Delegates to self.inlet().receive().
template <schema::PodMessage T>
struct Receiver {
#if PHYRIAD_HAS_DEDUCING_THIS
    template <typename Self>
    [[nodiscard]] auto poll(this Self&& self) noexcept
        -> std::expected<T, phyriad::Error>
    {
        return self.inlet().receive();
    }
#else
    static_assert(sizeof(T) == 0,
        "Receiver<T> requires deducing this (GCC 14+ / Clang 18+ / MSVC 19.37+). "
        "Use ReceiverCrtp<T,Derived> on older compilers.");
#endif
};

// ── CRTP fallbacks (older compilers only) ────────────────────────────────
// Usage: struct MyNode : SenderCrtp<T, MyNode> { Outlet<T>& outlet() { ... } };
#if !PHYRIAD_HAS_DEDUCING_THIS

template <schema::PodMessage T, typename Derived>
struct SenderCrtp {
    [[nodiscard]] auto publish(T const& msg) noexcept
        -> std::expected<void, phyriad::Error>
    {
        return static_cast<Derived&>(*this).outlet().publish(msg);
    }
};

template <schema::PodMessage T, typename Derived>
struct ReceiverCrtp {
    [[nodiscard]] auto poll() noexcept
        -> std::expected<T, phyriad::Error>
    {
        return static_cast<Derived&>(*this).inlet().receive();
    }
};

#endif  // !PHYRIAD_HAS_DEDUCING_THIS

} // namespace phyriad::node
// Made with my soul - Swately <3
