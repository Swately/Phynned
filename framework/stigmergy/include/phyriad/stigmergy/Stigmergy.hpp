// framework/stigmergy/include/phyriad/stigmergy/Stigmergy.hpp
// Umbrella include for the entire stigmergy pillar.
//
// Users who don't care about minimizing include cost can simply:
//   #include <phyriad/stigmergy/Stigmergy.hpp>
// and get all the primitives at once.
//
// Users in hot-path translation units should include only the specific
// header(s) they use to minimize compile-time and ODR surface.
//
// Stigmergy as first-class pillar.
#pragma once

#include <phyriad/stigmergy/Field.hpp>
#include <phyriad/stigmergy/Pheromone.hpp>
#include <phyriad/stigmergy/Classifier.hpp>
#include <phyriad/stigmergy/Worker.hpp>

// All under namespace phyriad::stigmergy.
// Made with my soul - Swately <3
