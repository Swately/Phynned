// framework/_meta/include/phyriad/version.hpp
// Phyriad Framework — compile-time version constants.
//
// Consumers pin to a minimum version:
//   static_assert(PHYRIAD_VERSION >= PHYRIAD_MAKE_VERSION(0, 1, 0),
//                 "Phyriad 0.1.0 or newer required");
//
// The numeric MAJOR/MINOR/PATCH and the string PHYRIAD_VERSION_STRING are
// kept in sync automatically by release-please. The marker comments
// immediately after each value are how release-please locates the line
// to bump; do not remove them.
//
#pragma once

#define PHYRIAD_VERSION_MAJOR 0   // x-release-please-major
#define PHYRIAD_VERSION_MINOR 1   // x-release-please-minor
#define PHYRIAD_VERSION_PATCH 0   // x-release-please-patch

// Optional pre-release tag. Empty for a final release.
//   "experimental" — pre-1.0 exploratory state; APIs and numbers may move
//                    without notice. The previous "v1.x" maturity claim has
//                    been retracted (see CHANGELOG.md retraction notice).
//   "rc1", "rc2"   — release candidates: feature-complete but pending
//                    final validation (unused while pre-1.0).
//   "beta1"+       — beta releases: feature set may still change.
//   ""             — final release (1.0.0, 1.0.1, etc.).
#define PHYRIAD_VERSION_PRERELEASE "experimental"

#define PHYRIAD_MAKE_VERSION(maj, min, patch) \
    (((maj) << 16) | ((min) << 8) | (patch))

#define PHYRIAD_VERSION \
    PHYRIAD_MAKE_VERSION(PHYRIAD_VERSION_MAJOR, PHYRIAD_VERSION_MINOR, PHYRIAD_VERSION_PATCH)

#define PHYRIAD_VERSION_STRING "0.1.0-experimental"  // x-release-please-version
// Made with my soul - Swately <3
