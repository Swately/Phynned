// apps/ayama/core/include/ayama/version.hpp
// Ayama — compile-time version constants.
//
// Mirrors the layout of phyriad/version.hpp. Ayama and Phyriad version
// independently: a fix in Ayama bumps AYAMA_VERSION_PATCH without touching
// PHYRIAD_VERSION_*, and vice versa.
//
// release-please maintains the numeric MAJOR/MINOR/PATCH and the string
// AYAMA_VERSION_STRING in sync automatically. The marker comments
// immediately after each value are how release-please locates the line
// to bump; do not remove them.
//
// Consumers (ayama-ui's version-check feature) compare AYAMA_VERSION_STRING
// against the GitHub Releases API to detect when a newer ayama-vX.Y.Z tag
// is available.
//
#pragma once

#define AYAMA_VERSION_MAJOR 0   // x-release-please-major
#define AYAMA_VERSION_MINOR 1   // x-release-please-minor
#define AYAMA_VERSION_PATCH 0   // x-release-please-patch

// Optional pre-release tag. "experimental" while Ayama tracks the
// student-exploration phase of Phyriad; empty for a final release.
#define AYAMA_VERSION_PRERELEASE "experimental"

#define AYAMA_MAKE_VERSION(maj, min, patch) \
    (((maj) << 16) | ((min) << 8) | (patch))

#define AYAMA_VERSION \
    AYAMA_MAKE_VERSION(AYAMA_VERSION_MAJOR, AYAMA_VERSION_MINOR, AYAMA_VERSION_PATCH)

#define AYAMA_VERSION_STRING "0.1.0-experimental"  // x-release-please-version
// Made with my soul - Swately <3
