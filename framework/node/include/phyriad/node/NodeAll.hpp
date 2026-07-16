// framework/node/include/phyriad/node/NodeAll.hpp
// Umbrella include for the phyriad_node pillar.
//
// Includes:
//   Lifecycle.hpp           — lifecycle hook concepts + try_* dispatch
//   Port.hpp                — Outlet<T>, Inlet<T> type-erased ports
//   Categories.hpp          — Source, Sink, Transform, Splitter, Merger, Actor
//   Node.hpp                — Node<N> aggregator concept + helpers
//   Runnable.hpp            — Runnable<N>, WrappableNode<N>
//   Mixins.hpp              — Sender<T>, Receiver<T> (deducing-this)
//   CoroutineAdapter.hpp    — CoroFrameAllocator, AwaitableTask<T>
//
// Usage:
//   #include <phyriad/node/NodeAll.hpp>
//   struct MySource : phyriad::node::Sender<MyTick> { ... };
//

#pragma once
#include "Lifecycle.hpp"
#include "Port.hpp"
#include "Categories.hpp"
#include "Node.hpp"
#include "Runnable.hpp"
#include "Mixins.hpp"
#include "CoroutineAdapter.hpp"
// Made with my soul - Swately <3
