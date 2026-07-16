// framework/transport/include/phyriad/transport/TransportAll.hpp
// Umbrella include for the phyriad_transport pillar.
//
// Includes:
//   Transport.hpp       — Transport<Impl,Msg> concept, LatencyClass enum
//   Ring.hpp            — Ring<T,Cap> SWMR lock-free ring, RingHandle
//   RingChannel.hpp     — RingChannel<T,Cap,MaxReaders> MPMC ring (restored)
//   Latest.hpp          — Latest<T> seqlock SWMR single-slot channel
//   SlotCopy.hpp        — SlotCopyFn, SlotCopyMode, pick_slot_copy()
//   RingWaitPolicy.hpp  — RingWaitBusySpin/Pause/Yield/Backoff/Sleeping
//
// Usage:
//   #include <phyriad/transport/TransportAll.hpp>
//   Ring<MyTick, 1024> ring;
//   auto h = ring.subscribe();
//   ring.send(tick);
//   auto r = ring.receive(h);
#pragma once
#include "Transport.hpp"
#include "Ring.hpp"
#include "RingChannel.hpp"
#include "Latest.hpp"
#include "SlotCopy.hpp"
#include "RingWaitPolicy.hpp"
// Made with my soul - Swately <3
