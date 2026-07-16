// framework/process/include/phyriad/process/CurrentProcess.hpp
// Portable cached accessors for the current process's own identity.
//
// Provides:
//   phyriad::proc::self_pid()   — own PID, cached after first call.
//   phyriad::proc::self_ppid()  — own parent PID, cached after first call.
//   phyriad::proc::self_name()  — own exe basename (null-terminated), cached.
//
// Implementation notes:
//   All values computed once via function-local static (C++11 thread-safe
//   initialization guarantee) and returned as trivial integers / const char*.
//   Subsequent calls cost ~2 ns (static load).
//
// Threading:
//   All functions are safe to call from multiple threads simultaneously.
//   The underlying value is immutable after the first call returns.
//

#pragma once

#include <cstdint>

namespace phyriad::proc {

/// Returns the PID of the current process.
/// Cached after first call (~1 syscall on first invocation, ~2 ns after).
[[nodiscard]] uint32_t self_pid() noexcept;

/// Returns the parent PID of the current process.
/// Returns 0 if unavailable (some OS environments / sandboxed processes).
[[nodiscard]] uint32_t self_ppid() noexcept;

/// Returns the base name of the current executable (e.g. "my_app.exe").
/// The returned pointer is valid for the lifetime of the process.
/// Never returns nullptr — returns "" if the name cannot be determined.
[[nodiscard]] const char* self_name() noexcept;

} // namespace phyriad::proc
// Made with my soul - Swately <3
