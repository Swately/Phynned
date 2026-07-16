# Pillar: `phyriad::stigmergy`

**Audience:** contributors maintaining or extending the vendored stigmergy
pillar (`framework/stigmergy/`). (The substrate's 5-minute quickstart,
`QUICKSTART_STIGMERGY.md`, was not imported at the separation — it lives in
the parent container's `catalog/cpp/docs/guides/`.)

This doc covers: the design contract per primitive, the invariants the
implementations enforce, performance budgets, and the rules for adding
new primitives or modifying existing ones.

---

## Pillar shape

```
framework/stigmergy/
├── include/phyriad/stigmergy/
│   ├── Field.hpp         — single-cell, 1 writer / N readers (wraps transport::Latest)
│   ├── Pheromone.hpp     — N-slot, 1 writer per slot / N readers (atomic array)
│   ├── Classifier.hpp    — abstract base for the decide() policy
│   ├── Worker.hpp        — std::jthread wrapper (optional convenience)
│   └── Stigmergy.hpp     — umbrella include
├── tests/                — 4 test files, 10,104 EXPECTs
└── CMakeLists.txt        — INTERFACE library, depends on transport + hal + topology
```

**Dependency rule:** stigmergy depends on `transport` (for `Latest<T>`),
`schema` (for `PodMessage` concept), `hal` (for memory ordering wrappers
+ cache-line padding), `topology` (Worker's optional pin-to-core).
**Nothing depends on stigmergy except pool, examples, and downstream
user code.** Cycle-free.

The pillar is **INTERFACE-only**: all primitives are header-only
templates. Zero `.cpp` files. This means LTO can fully inline through
every call site.

---

## Per-primitive design contracts

### `Field<T>` — single-cell observable

**Contract:**
- Exactly **one writer thread** calls `publish()`. Multiple writers ⇒ UB.
- Any number of readers call `read()` concurrently. Wait-free in steady
  state; readers may briefly spin during an active publish (bounded by
  publisher's write duration).
- `T` must satisfy `phyriad::schema::PodMessage<T>`: trivially copyable,
  standard-layout, sizeof ≤ 4 KiB, alignof ≤ 64 B.
- Field<T> is **address-stable**: not copyable, not movable (contains
  atomics).

**Implementation:** thin forwarder to `phyriad::transport::Latest<T>`.
`Field<T>` adds zero overhead — `publish()` / `read()` are
`[[gnu::always_inline]]` and call through to `Latest<T>::write/read`.
The wrapper exists for naming + future extension points (watcher
callbacks, derived fields).

**Performance contract** (verified via `bench_stigmergy_primitives`):
- `publish()` single-thread: ≤ 5 ns
- `read()` no concurrent writer: ≤ 5 ns
- `read()` cross-thread p99: ≤ 150 ns

**Memory ordering** (inherited from `Latest<T>`):
- `publish`: seq_write fetch_add release → STLR on ARM
- `read`: seq_load acquire → LDAR on ARM; spin if odd; re-check seq

**When to modify:** if you add a watcher / observer hook, do it in a
separate non-inline path so the hot `read()` stays bit-identical to
`Latest::read`. Verify via `objdump --disassemble` diff.

---

### `Pheromone<T, N>` — N-slot per-agent field

**Contract:**
- Slot `i` has **exactly one writer thread** (agent `i`). Writing slot
  `i` from two threads concurrently ⇒ UB.
- Any number of readers call `read(i)`, `read_all()` concurrently.
- `T` must satisfy `detail::AtomicStorable`: trivially copyable,
  copy-constructible, sizeof ≤ 16 (DWCAS max on x86_64).
- `N` is a compile-time constant; range [1, 65536].
- Each slot is `alignas(hal::kDestructivePad)` (128 B on x86) →
  cross-CCD writers do NOT false-share.

**Implementation:** `std::array<Slot, N>` where each `Slot` is
`alignas(128) std::atomic<T>`.

**Performance contract:**
- `deposit(i, v)`: ≤ 10 ns (relaxed atomic store)
- `read(i)`: ≤ 5 ns (relaxed atomic load)
- `read_all()`: N × 5 ns linear, fits in L1 for N ≤ 32

**Memory ordering:**
- `deposit` uses `hal::stat_store_relaxed`
- `read`    uses `hal::stat_load_relaxed`
- `clear_all` uses `hal::stat_store_relaxed` (caller serialises)

**Why relaxed:** the slot value is a scalar (uint8/uint16/uint64/etc.)
with no implicit happens-before to other memory. The reader's decision
is based on the slot value alone. Stigmergic coordination tolerates
eventual consistency: a reader seeing a stale value still makes a valid
decision; next iteration converges.

**Cross-CCD scaling** (measured P-0.6.6 on 7950X3D):
- Same-CCD reader+writers: linear scaling
- Cross-CCD reader+writers: ~2× p99 latency penalty (Infinity Fabric)
- The cache-line padding prevents writer-writer interference; the
  reader-vs-writer cost is fundamental to the pattern

**When to modify:** if you add a struct slot type, **promote the
ordering to release/acquire** — the current `relaxed` is correct
ONLY for scalar slots; struct slots need acquire/release semantics
so the consumer observes a complete write rather than a torn one.

---

### `Classifier<Signal, Action>` — emergent decision policy

**Contract:**
- Pure virtual base. Concrete subclasses override `decide()`.
- `decide(Signal const&) noexcept` MUST be noexcept — exceptions on
  the dispatch path are unacceptable.
- Concrete subclasses **own no field state**; they hold references /
  pointers to whatever `Field<T>` / `Pheromone<T,N>` they consult.
  This avoids forcing a fixed shape (some classifiers need 1 field,
  others need 3 pheromones, etc.).
- Concrete subclasses should be marked `final`. LTO devirtualises
  through `final` overrides at use sites.

**Implementation:** the base class is 6 lines — just the virtual
`decide()` declaration + deleted copy/move. All cost is in subclass
impls.

**Performance contract:**
- `decide()` p99 typical (N ≤ 16 slots): ≤ 200 ns
- The 200 ns budget breaks down as: 1 field read (50 ns) + N pheromone
  reads (5 ns × N) + decision logic (50 ns). N=16 fits comfortably.
- Under heavy concurrent deposit pressure (Workflow B), p99 grows
  linearly with N up to ~1.3 µs at N=128 — expected and acceptable.

**Why a base class** (not a concept):
1. Allows runtime polymorphism (A/B test two routing policies under
   one dispatcher).
2. Stable extension point for future hooks (telemetry, replay capture)
   without breaking subclasses.
3. Even with virtual call cost (~3 ns on Zen), the cost is dwarfed by
   the cache misses in the field reads.

For compile-time classifiers, just write a regular functor and pass
it where the calling code accepts `Classifier&`. The base class is
for cases that want runtime polymorphism / type erasure.

**When to modify:** if you need to add observer hooks (e.g., "log every
decision for replay debugging"), add a non-virtual `notify_decision()`
that the base implementation can call after `decide()` returns. Do NOT
make `decide()` itself observable — observers shouldn't be on the hot
path.

---

### `Worker<Logic>` — autonomous-agent thread wrapper

**Contract:**
- `Logic` is `void(std::stop_token) noexcept` — explicit `std::is_invocable_r_v`
  requirement.
- `Worker` owns one `std::jthread`. Default ctor is idle; explicit ctor
  with `Logic` launches immediately.
- Destructor calls `request_stop()` + `join()`. Idempotent.
- Optional `pin_to_core` parameter calls `phyriad::hw::pin_current_thread()`
  at thread startup. UINT32_MAX = no pinning.

**Implementation:** ~50 lines around `std::jthread`. Move-constructible.

**Performance contract:** zero overhead vs raw `std::jthread`. The thread
body invokes `Logic` exactly once; Logic owns the loop.

**When to modify:** this is **an optional convenience** — not load-
bearing for the stigmergy pattern. `Field<T>` and `Pheromone<T,N>` work
with any thread implementation. Don't add knobs here; if users want
custom cadence (sleep-based / event-driven / cooperative scheduler),
they write their own thread loop.

---

## Testing strategy

`framework/stigmergy/tests/` has 4 files mirroring the four primitives:

| File | Sections | EXPECTs | Run-as-test target |
|---|---|---|---|
| `stigmergy_field_test.cpp` | 6 | ~2,500 | `stigmergy_field_test` |
| `stigmergy_pheromone_test.cpp` | 8 | ~3,000 | `stigmergy_pheromone_test` |
| `stigmergy_classifier_test.cpp` | 5 | ~1,000 | `stigmergy_classifier_test` |
| `stigmergy_integration_test.cpp` | 6 | ~3,600 | `stigmergy_integration_test` |

Plus `bench_stigmergy_primitives` (single-thread + scaling sweeps) and
`bench_stigmergy_workflow` (3 end-to-end workflows × N sweep).

**Rules for adding tests:**

1. New tests use the existing `EXPECT(cond)` macro pattern — no GTest.
   The micro-test framework is in
   [`stigmergy_field_test.cpp`](../framework/stigmergy/tests/stigmergy_field_test.cpp)
   lines 1-30.
2. **Section a test** with `SECTION("Test N: <what>")` for grep-ability
   in CI logs.
3. **Concurrent tests** must declare expected thread counts and use
   `std::jthread` (not raw `std::thread`) for clean shutdown.
4. **Performance assertions** belong in benches, not unit tests.
   Unit tests assert correctness only.

---

## Performance budget enforcement

Three layers of enforcement:

1. **`PERF_BASELINE.json`** (`docs/framework/PERF_BASELINE.json`)
   tracks 8 stigmergy metrics. Tolerance per-metric.
2. **`scripts/check_perf_regression.ps1`** runs the bench, parses
   output, compares vs baseline. Direction-aware (latency: lower is
   better; throughput: higher).
3. **`.github/workflows/bench-regression.yml`** runs the detector on
   manual trigger (public runners can't reproduce tight baselines;
   self-hosted is the right answer).

**If you change a primitive's hot path:**

1. Run `bench_stigmergy_primitives` BEFORE the change.
2. Apply the change.
3. Re-run; compare to baseline tolerance.
4. If perf regressed > tolerance:
   - Either: justify the regression (e.g., correctness fix that costs
     N ns) and update `PERF_BASELINE.json`.
   - Or: redesign to avoid the regression.

**Don't update PERF_BASELINE.json silently.** Each entry should map to
a PR with the justification in the commit message.

---

## Common contributor questions

### "Can I add `std::function` support to Worker<Logic>?"

Already works — `std::function<void(std::stop_token)>` satisfies the
`is_invocable_r_v<void, Logic, std::stop_token>` constraint. The
`examples/quickstart_stigmergy/main.cpp` uses exactly this. Just be
aware that `std::function` has a heap allocation + indirect call cost
that a captureless lambda doesn't.

### "Can I make Field<T> support multiple writers?"

No. By design. Use `Ring<T>` or `RingChannel<T>` for MPMC; Field is
SWMR by contract. Changing this would invalidate the seqlock invariant
and turn `Latest<T>` into a different primitive.

If you need "many writers logically, one writer mechanically", have
the writers send through a Ring and let a single consumer thread
publish to the Field. That's the standard pattern.

### "Can I add a `Pheromone::deposit_if(i, predicate, v)` variant?"

Yes, but ONLY if it inlines to the same atomic store + branch — no
additional fences. The point of Pheromone is sub-10ns deposit; anything
that adds fences breaks the contract. Run `bench_stigmergy_primitives`
before merging to verify.

### "Can the Classifier observe deposits in real-time (callbacks on deposit)?"

No callbacks from inside `deposit`. That would add an indirect call to
the hot path (3-5 ns at minimum, more if the callback does anything).
The intended model is: classifiers poll via `read_all()` at their own
cadence. If you want push semantics, the right primitive is a Ring;
that's a different pillar (transport).

### "Why isn't there a `Pheromone::lock_free()` query?"

Because it's `static_assert`'d at compile time via `detail::AtomicStorable`.
If `sizeof(T) > 16`, the type fails the concept check at instantiation,
not runtime.

---

## Future work tracked

| Item | Status |
|---|---|
| Pheromone slot promotion to release/acquire for struct slots | ⏳ Open |
| Topology probe CCD detection on AMD 7000-series | ⏳ Open |
| Cross-CCD Pheromone sharding helper (one Pheromone per CCD + aggregation classifier) | ⏳ Open |
| `Field<T>::publish_if_newer(T, version_t)` for safe re-entry | 💭 Idea |

When you spawn work on any of these, link it back here so the
contributor doc stays current.
