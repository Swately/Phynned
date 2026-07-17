// core/include/phynned/version.hpp
// Phynned — compile-time version constants.
//
// Mirrors the layout of phyriad/version.hpp. Phynned and Phyriad version
// independently: a fix in Phynned bumps PHYNNED_VERSION_PATCH without touching
// PHYRIAD_VERSION_*, and vice versa.
//
// release-please maintains the numeric MAJOR/MINOR/PATCH and the string
// PHYNNED_VERSION_STRING in sync automatically. The marker comments
// immediately after each value are how release-please locates the line
// to bump; do not remove them.
//
// Consumers (phynned-ui's version-check feature) compare PHYNNED_VERSION_STRING
// against the GitHub Releases API to detect when a newer phynned-vX.Y.Z tag
// is available.
//
#pragma once

#define PHYNNED_VERSION_MAJOR 0   // x-release-please-major
#define PHYNNED_VERSION_MINOR 1   // x-release-please-minor
#define PHYNNED_VERSION_PATCH 0   // x-release-please-patch

// Optional pre-release tag. "experimental" while Phynned tracks the
// student-exploration phase of Phyriad; empty for a final release.
#define PHYNNED_VERSION_PRERELEASE "experimental"

#define PHYNNED_MAKE_VERSION(maj, min, patch) \
    (((maj) << 16) | ((min) << 8) | (patch))

#define PHYNNED_VERSION \
    PHYNNED_MAKE_VERSION(PHYNNED_VERSION_MAJOR, PHYNNED_VERSION_MINOR, PHYNNED_VERSION_PATCH)

#define PHYNNED_VERSION_STRING "0.1.0-experimental"  // x-release-please-version
// Made with my soul - Swately <3
