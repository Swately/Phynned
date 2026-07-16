// framework/schema/include/phyriad/schema/Schema.hpp
// Umbrella include for the phyriad_schema pillar.
//
// Includes:
//   - Error.hpp            — phyriad::Error, phyriad::ErrorCode, phyriad::NodeId (16B POD, used by all pillars)
//   - xxh3_inline.hpp      — consteval XXH3-128 engine
//   - SchemaHash.hpp       — Hash128, XXH3State, schema_hash<T>(), kPhyriadHalVersion
//   - PodMessage.hpp       — PodMessage<T> concept, PHYRIAD_ASSERT_POD, SampleTick, kMaxPodSize
//   - CapnpMessage.hpp     — CapnpMessage<T> concept, PHYRIAD_REGISTER_CAPNP
//   - OpaqueMessage.hpp    — OpaqueMessage<T> concept, OpaqueSpan
//   - SchemaDescriptor.hpp — GraphSchemaDescriptor, WireDescriptor, NodeDescriptor,
//                            make_schema_descriptor(), validate_schema_descriptor()
//   - MessageTier.hpp      — Tier0/1/2Message concepts, AnyMessage, tier_of<T>
//
// Usage (within another pillar):
//   #include <phyriad/schema/Schema.hpp>
//   static_assert(phyriad::schema::PodMessage<MyTick>);
#pragma once
#include "Error.hpp"
#include "xxh3_inline.hpp"
#include "SchemaHash.hpp"
#include "PodMessage.hpp"
#include "CapnpMessage.hpp"
#include "OpaqueMessage.hpp"
#include "SchemaDescriptor.hpp"
#include "MessageTier.hpp"
// Made with my soul - Swately <3
