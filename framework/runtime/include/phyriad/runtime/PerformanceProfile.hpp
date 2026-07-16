// framework/runtime/include/phyriad/runtime/PerformanceProfile.hpp
// Forwarding shim. The canonical home of `PerformanceProfile` is the
// `phyriad_profile` pillar — see
// `framework/profile/include/phyriad/profile/PerformanceProfile.hpp`.
//
// Problem 3 of PILLAR_COMPOSABILITY_AUDIT extracted this header so the pool
// pillar can take a profile dependency without dragging runtime/graph/
// scheduler/node. Existing code that includes
// `<phyriad/runtime/PerformanceProfile.hpp>` and uses
// `phyriad::runtime::PerformanceProfile` keeps working through the
// using-declarations below.
//
// New code should prefer `<phyriad/profile/PerformanceProfile.hpp>` and the
// `phyriad::profile::…` namespace.
#pragma once
#include <phyriad/profile/PerformanceProfile.hpp>

namespace phyriad::runtime {

using ::phyriad::profile::ProfileKind;
using ::phyriad::profile::RingWaitMode;
using ::phyriad::profile::SlotCopyMode;
using ::phyriad::profile::NodeTickProfile;
using ::phyriad::profile::RingProfile;
using ::phyriad::profile::PerformanceProfile;
using ::phyriad::profile::profile_name;
using ::phyriad::profile::make_auto_profile;
using ::phyriad::profile::apply_profile_hints;

} // namespace phyriad::runtime
// Made with my soul - Swately <3
