# zrqueue

## Overview
`zrqueue` is a wait-free, lock-free, single-producer single-consumer (SPSC) queue library provided as a **single-header** C++17 library. 
It is proven to be significantly faster and more stable than the industry-standard `rigtorp::SPSCQueue`. 

Designed strictly for High-Frequency Trading (HFT) and ultra-low latency (ULL) systems where every nanosecond counts, 
`zrqueue` applies extreme hardware-level micro-architectural optimizations. By offering four distinct memory models (Heap-allocated, In-place embedded, Pre-constructed Ring Buffer, and Mirrored), it covers every possible hot-path scenario. Because it is entirely self-contained in one file, it offers the ultimate ease of integration—just drop it in and compile.

## Key Features

* **Four Distinct Queue Models:**
  * `SpscHeapQueue`: Dynamic capacity, heap-allocated, supports OS HugePages.
  * `SpscInlineQueue`: Zero-allocation, data is embedded directly within the object footprint via `std::byte` arrays, eliminating the final pointer-chasing penalty.
  * `SpscRingBuffer`: Disruptor-style claim/commit paradigm. Objects are pre-constructed and memory-pre-faulted, enabling absolute zero-copy modifications. Supports single-slot (`alloc`/`push`) and burst (`alloc_bulk`/`commit`) claims with exactly one release barrier per commit.
  * `SpscMirrorQueue`: Latency-optimized for shallow-flow (near-empty) ping-pong. The producer packs each element into an atomic entry inside its **own index cache line**, so the consumer's polling load of `write_index_` fetches index *and* payload in **one** cross-core migration — halving the theoretical line migrations per round trip (4 → 2). Two widths are mirrored: `sizeof(T) <= 4` uses 8 B `{seq32 | payload32}` entries with a 4-element window; `sizeof(T) == 8` uses 16 B `{seq64, payload64}` seqlock entries with a 2-element window. 8 B is enabled automatically for arithmetic scalars and can be opted into for custom types via `mirror_traits<T>`. Measured on a bare-metal Core Ultra 5 125H (cores 2/4 pinned): 4 B is the lowest-RTT model on all four percentiles (174 ns p50 / 436 ns p99.9 vs 215 / 483 for `rigtorp`); the 8 B scalar path reaches 176 ns p50 versus 221 ns for the same-type classic path (1.9.3 A/B); its saturated throughput is **+27 % at capacity 4096 but −9…−13 % at 65536**, because the fast path is gated on a 2-element window and a deep backlog misses it while still paying the extra stores. **Usage caveats:** (1) hold the pointer returned by `front()` — a second `front()` call takes the classic slot path and re-reads the slot line, silently voiding the piggyback (neutral for the other models). (2) Types whose constructor emits narrow (byte/field) stores must not rely on the default slot read-back: use `emplace_packed` / `push_packed`, or specialize `mirror_codec<T>` so `emplace()` packs from the constructor arguments. Otherwise the read-back defeats store-to-load forwarding and can cost 3–7x throughput; `mirror_traits<T>` can also opt the type back to the classic path.
* **Zero Branch Misprediction:** Uses monotonically increasing `uint64_t` indices and bitwise AND (`& mask_`) instead of modulo or `if`-based wrap-around.
* **Perfect Cache Line Isolation:** Every heavily-contended atomic variable strictly occupies its own dedicated 64-byte L1 Cache Line to utterly eradicate Cache Coherence Ping-Pong and Asymmetric False Sharing.
* **Over-Aligned Payloads:** The default allocator aligns to `max(alignof(T), cache line)`, so `alignas(128)` message types get correctly aligned storage; a `static_assert` rejects any weaker alignment at compile time.
* **Memory Pre-Warming & Page-Fault Defense:** Aggressive use of value-initialization (`data_{}`) to trigger OS page faults during the application startup phase, ensuring zero latency spikes during live market hours.
* **Trivial Destructor Elision:** Leverages C++17 `if constexpr (!std::is_trivially_destructible_v<T>)` to completely bypass destruction overhead for POD types (Zero-Cost Abstraction).
* **Lookahead Support:** Native `peek(offset)` API allows consumers to inspect future elements without committing a `pop()`.
* **Micro-burst Defense:** Native `push_bulk` and `consume_bulk` APIs (plus `alloc_bulk`/`commit` on `SpscRingBuffer`) that execute only **one** atomic memory barrier for `N` elements.
* **Contiguous Batch Read:** `front_bulk(want, available)` / `pop_bulk(n)` hand the consumer a pointer to a run of up to `want` elements without copying, and retire them with a single release store. The run never straddles the wrap point, mirroring `alloc_bulk` on the producer side.
* **Compile-Time Safety Contract:** C++17 is enforced by `#error` (via `_MSVC_LANG` on MSVC, whose `__cplusplus` is otherwise stuck at `199711L`). Bulk APIs are `noexcept` and `static_assert` that `T` and the callback are nothrow-constructible / nothrow-invocable, so a throwing handler fails to compile instead of calling `std::terminate`.
* **Destructor Cost:** Destruction walks the live `[read_index, write_index)` range directly instead of driving `front()`/`pop()`, so tearing down a queue full of POD elements costs no atomic stores at all.
* **Hardware Thermal Throttling Defense:** Built-in `spin_front()` extracts invariant loads out of the loop and utilizes `_mm_pause()` / `yield` to prevent CPU ALU burnout and thermal downclocking during 100% busy-polling.

> **Observability vs. control:** `size()` and `empty()` issue two independent `relaxed` loads, so their result is a stale, non-atomic snapshot and can be negative-difference-clamped but never authoritative. Never gate flow control on them — use `try_push` / `try_emplace` / `try_pop`, which decide on the owning side.

## Architectural Superiority

When compared to `rigtorp::SPSCQueue`, `zrqueue` applies "surgical" rewrites specifically aimed at the HFT Hot Path.

| Feature | `rigtorp::SPSCQueue` | `zrqueue::SpscHeapQueue` / Models | HFT Advantage |
| :--- | :--- | :--- | :--- |
| **Addressing (Hot Path)** | `if (next == cap) next = 0;` | Bitwise `& mask_` | **Zero Branching.** Saves ~10ns periodic branch misprediction penalty (Jitter). |
| **Memory Isolation** | Padding Array Elements | Strict `alignas(64)` Structs | Complete physical isolation of Producer/Consumer atomic updates. |
| **Destruction Overhead** | Always calls `~T()` | C++17 `if constexpr` bypass | Zero CPU cycles wasted on destructing Plain Old Data (POD). |
| **Memory Page Faults** | Lazy / Demand Paging | Forced Pre-Warming (`data_{}`) | Eliminates ~5μs First-Touch Page Faults at market open. |
| **TLB Miss Defense** | Standard `malloc` (4KB) | `HugePageAllocator` (2MB/1GB) | Sustains max throughput on multi-million element queues (e.g., Order Books). |
| **Zero-Copy Write** | Push-by-value/move | `SpscRingBuffer::alloc()` | Disruptor pattern: directly write into the queue's memory slot. |

## Performance Comparison

The bundled benchmark rotates the variant order on every repetition (cancel execution-order and cold-cache bias), synchronizes both threads on a start barrier, samples RTT with `__rdtsc()` plus a startup calibration (steady_clock is ~100 ns-granular on Windows), and reports medians. It sweeps two capacities (4096 exercises the full/empty slow paths, 16M is the classic setup), ten payload lengths (4 B – 1 KB), 8 B scalar mirror/classic A/B rows, a non-POD row, a narrow-store payload-source diagnosis, controlled backlog depths, and per-iteration RTT percentiles.

### Measured results (bare-metal Windows host)

**Conditions:** Intel Core Ultra 5 125H (14 cores / 18 threads, P+E), Windows 10.0.26300, MSVC 19.51 `/O2 /Ob2 /Oy- /GT /LTCG /DNDEBUG`, consumer pinned to core 2 and producer to core 4, `__rdtsc()` calibrated at 0.333868 ns/cycle (2.995 GHz). Throughput in K ops/ms, latency in ns, all medians over interleaved reps, two independent runs reported side by side. **These are host-specific numbers — do not compare them with results from other machines, VMs, or compilers** (a 4-vCPU Ubuntu VM produced non-monotonic length sweeps and 2.2–2.4 µs p99.9 with the same code).

> **`/DNDEBUG` is not optional.** It must be defined in the measured build. `rigtorp::SPSCQueue::pop()` asserts with a cross-core acquire load; with assertions live that single load costs a full cache-line transfer per element and understated `rigtorp` by **7x** in an earlier run of this very benchmark (47 K vs 332 K ops/ms at `int`, 4096). MSVC's own Release configuration defines it, but assigning to `CMAKE_CXX_FLAGS_RELEASE` discards CMake's default — hence it is listed explicitly in `CMakeLists.txt`.

**Throughput by payload length, capacity 4096 (K ops/ms, run 1 / run 2):**

| bytes | `rigtorp` | `SpscHeapQueue` | `SpscInlineQueue` | `SpscMirrorQueue` | `SpscRingBuffer` |
| :--- | ---: | ---: | ---: | ---: | ---: |
| 4 | 335 / 84 | 356 / 313 | 330 / 347 | **420 / 428** | 290 / 293 |
| 8 | 280 / 205 | 452 / 441 | 485 / 486 | 471 / 483 | 339 / 338 |
| 16 | 152 / 149 | 144 / 149 | 156 / 160 | 134 / 138 | **154 / 159** |
| 32 | **77 / 74** | 71 / 72 | 74 / 77 | 68 / 70 | 76 / 76 |
| 64 | 36 / 35 | 39 / 41 | 43 / 43 | 39 / 39 | **42 / 42** |
| 128 | 20 / 19 | 22 / 23 | 23 / 23 | 20 / 20 | 18 / 18 |
| 256 | 5.5 / 5.5 | 5.5 / 5.5 | 5.4 / 5.4 | 5.4 / 5.4 | **5.7 / 5.7** |
| 512 | 2.8 / 2.8 | 3.0 / 3.0 | 2.8 / 2.8 | 2.8 / 2.8 | 2.8 / 2.8 |
| 1 KB | 1.4 / 1.4 | 1.4 / 1.4 | 1.4 / 1.4 | 1.4 / 1.4 | 1.4 / 1.4 |
| non-POD | 29 / 27 | 29 / 26 | 28 / 28 | 29 / 27 | **294 / 300** |

**Throughput, `int` payload (K ops/ms, run 1 / run 2):**

| Variant | @4096 | @16M |
| :--- | ---: | ---: |
| `rigtorp` emplace/front/pop | 169 / 243 | 488 / 508 |
| `SpscHeapQueue` emplace/front/pop | 515 / 533 | 569 / 602 |
| `SpscInlineQueue` emplace/front/pop | 397 / 412 | 560 / 598 |
| `SpscMirrorQueue` emplace/front/pop | **567 / 579** | 535 / 527 |
| `SpscRingBuffer` alloc/write/push | 461 / 515 | 506 / 501 |
| `SpscRingBuffer` push_value | 500 / 562 | 498 / 500 |
| `SpscRingBuffer` alloc_bulk/commit ×64 | **1052 / 1091** | **991 / 1042** |
| `SpscHeapQueue` push_bulk/consume_bulk ×64 | 704 / 705 | 588 / 627 |

**Round-trip latency (ns, run 1 / run 2):**

| Variant | @4096 | @16M |
| :--- | ---: | ---: |
| `rigtorp` | 210 / 212 | 207 / 211 |
| `SpscHeapQueue` | 205 / 208 | 206 / 208 |
| `SpscInlineQueue` | 208 / 204 | 205 / 204 |
| `SpscMirrorQueue` | **166 / 164** | **165 / 165** |
| `SpscRingBuffer` | 202 / 202 | 191 / 191 |
| `SpscRingBuffer` push_value | 202 / 202 | 192 / 195 |

**RTT percentiles by payload length (ns, capacity 256, run 2):**

| bytes | model | p50 | p90 | p99 | p99.9 |
| :--- | :--- | ---: | ---: | ---: | ---: |
| 4 | `rigtorp` | 215 | 229 | 271 | 483 |
| 4 | `SpscHeapQueue` | 215 | 225 | 291 | 474 |
| 4 | `SpscInlineQueue` | 216 | 228 | 265 | 484 |
| 4 | **`SpscMirrorQueue`** | **174** | **186** | **216** | **436** |
| 4 | `SpscRingBuffer` | 213 | 233 | 264 | 478 |
| 8 | `rigtorp` | 219 | 258 | 267 | 483 |
| 8 | `SpscMirrorQueue` | 223 | 262 | 271 | 485 |
| 8 | `SpscMirrorQueue` (8 B scalar, mirror) | **176** | **204** | **246** | **447** |
| 8 | `SpscMirrorQueue` (8 B scalar, classic) | 221 | 262 | 269 | 476 |
| 128 | `rigtorp` | 253 | 295 | 321 | 556 |
| 256 | `SpscRingBuffer` | **402** | **430** | **598** | n/a |
| 1 KB | `SpscInlineQueue` | 1000 | 1045 | 1249 | n/a |

p99.9 is omitted from the ≥256 B rows because it is dominated by scheduler jitter on a general-purpose OS and swung 0.7–5.1 µs between the two runs (use `--rt` and core isolation to make it meaningful).

**Mirror 1.9.3 A/B (same payload type, cores 2/4, `--rt`, medians):**

| 8 B scalar | RTT p50 | RTT p99 | throughput @4096 |
| :--- | ---: | ---: | ---: |
| classic (`mirror_traits` disabled) | 221 ns | 269 ns | 411 / 457 K ops/ms |
| 8 B seqlock mirror (default for arithmetic) | **176 ns** | **246 ns** | 368 / 391 K ops/ms |

The throughput column is a dedicated same-process A/B (identical 8 B payload types, one with `mirror_traits` disabled, 10 M iters × 9 reps, order rotated per rep, three runs): mirror-on was 368–391 K against 411–457 K classic at capacity 65536, i.e. **10–15 % lower saturated throughput for 21 % lower p50 RTT**. At capacity 4096 the same A/B reverses to **+27–28 %**: the mirror only pays off while the backlog stays inside its 2-element window, so read that row as "deep-backlog throughput cost", not an unconditional one. The RTT columns come from the percentile sweep above.

**Mirror payload-source diagnosis (narrow-store constructors, 2 M iters × 3 reps, ops/ms):**

| type | default read-back | `emplace_packed` | classic heap |
| :--- | ---: | ---: | ---: |
| 4 B, four byte stores | 40 K | **336 K** | 269 K |
| 8 B, two `uint32_t` stores | 41 K | **406 K** | 320 K |

The default read-back is a store-to-load-forwarding trap for narrow-store constructors; the explicit payload API avoids it while keeping the mirror's RTT benefit. For single-store types (`int`, `uint64_t`), the read-back forwards cheaply and the default path is fine.

**Why the first 8 B attempt was rejected, and why that verdict was wrong.** The first 8 B implementation was measured at 54 K ops/ms against 513 K for the classic path and was reverted as structurally unworkable. The four-variant decomposition below shows the data was right and the *attribution* was wrong: the read-back was treated as an inherent part of the design rather than a by-product of the `emplace(args...)` API shape.

| variant (8 B, cap 4096, 4 M iters, same run) | ops/ms | what it removes |
| :--- | ---: | :--- |
| full 8 B mirror | 54 K | — |
| payload = index, no read-back | 170 K | the `memcpy` out of the just-constructed slot |
| no payload store, seq only | 320 K | the second store into the index line |
| mirror disabled (= classic) | 513 K | the mirror entirely |

The 4→2 migration saving was always real; the extraction step cancelled it, and for narrow-store constructors cancelled it many times over. Supplying the payload from the constructor arguments (`emplace_packed`, or `mirror_codec<T>` so call sites stay unchanged) severs the dependency chain `store to slot → load from slot → store to mirror`, so the payload never waits for ownership of the slot line.

What did **not** go away is the structural cost, and it is why the 8 B mirror is a latency feature rather than a throughput one:

| | producer writes to its index line | consumer loads | net |
| :--- | :--- | :--- | :--- |
| 4 B | 1 packed 8 B atomic (`{seq32 \| payload32}`) | 1 | wins on both axes |
| 8 B | 2 atomics (`seq`, then `payload`) | 3 (`seq` → `payload` → `seq`) | RTT wins; throughput depends on backlog depth (see below) |

A 16 B entry cannot be a single atomic without `cmpxchg16b`, so 8 B needs a seqlock and pays one extra store plus two extra loads per element. Whether that costs throughput depends on **how deep the queue runs**, because the mirror fast path is gated on `write_index - read_index <= K` (K = 2 for 8 B). Interleaved A/B of the same 8 B payload with `mirror_traits` toggled, three rounds, order rotated per rep:

| capacity | mirror vs classic | rounds |
| :--- | ---: | :--- |
| 4096 (shallow backlog) | **+27.5 … +28.3 %** | +27.7 / +28.3 / +27.5 |
| 65536 (deep backlog) | **−8.6 … −12.5 %** | −11.1 / −8.6 / −12.5 |

At capacity 4096 the producer and consumer stay within the 2-element window most of the time, so the mirror hits and the data-line migration is genuinely saved. At 65536 the backlog exceeds the window, `try_mirror` misses, the consumer falls back to the slots — and the producer still pays its two extra stores into the index line. So the 8 B mirror pays off in shallow flow and costs ~10 % in deep flow; size the queue to the expected backlog, or disable it with `mirror_traits` for throughput-bound paths that routinely queue deeply.

**Findings:**

* **The mirror now covers 4 B and 8 B payloads (8 B scalars are automatic; custom types opt in).** 4 B `SpscMirrorQueue` wins on every axis: 420–428 K ops/ms in the payload sweep (1.3–5x `rigtorp` depending on how badly `rigtorp`'s 4 K ring degrades that run) and the lowest RTT at all four percentiles (174 ns p50, 19 % below `rigtorp`, p99 216 ns vs 271 ns). For 8 B arithmetic scalars the 16 B seqlock mirror lowers same-type RTT p50 from 221 ns to 176 ns (p99 269 → 246 ns). Its throughput effect depends on backlog depth, not on the type: an interleaved same-process A/B (three rounds, order rotated) measured **+27.5…+28.3 % at capacity 4096** but **−8.6…−12.5 % at capacity 65536**, because the fast path is gated on `write_index - read_index <= K` (K = 2 for 8 B) — a deeper backlog misses the mirror, falls back to the slots, and still pays the two extra stores into the index line. `SpscMirrorQueue` is a latency queue; size the ring to the expected backlog, or disable the 8 B mirror via `mirror_traits<T>` on throughput-bound paths that queue deeply.
* **The earlier 8 B mirror rejection was a payload-extraction problem, not a mirror problem.** The old variant read the element back out of its slot after construction; constructors that emit several narrow stores defeat store-to-load forwarding. With the explicit-payload API (`emplace_packed`/`push_packed`), narrow-store types recover and beat classic: **4 B read-back 40 K → packed 336 K ops/ms (classic 269 K)**; **8 B read-back 41 K → packed 406 K ops/ms (classic 320 K)** in the 1.9.3 diagnostic. `mirror_codec<T>` applies the same fix to normal `emplace()` call sites; custom 8 B structs stay on the classic path unless `mirror_traits<T>::enable_8b` opts them in. See *Why the first 8 B attempt was rejected* above for the variant-by-variant decomposition.
* **`SpscRingBuffer` `alloc_bulk`/`commit` is the throughput ceiling** — ~1 M ops/ms at both capacities, roughly 2x the best single-element model, because one release barrier covers 64 elements.
* **Non-POD elements reverse the ranking — it is a lifetime-model difference, not a tuning win.** Every placement-new model (and `rigtorp`) drops to 26–31 K ops/ms because each element pays a constructor and a destructor, while the pre-constructed `SpscRingBuffer` keeps 294–307 K — about **10x**. `SpscRingBuffer` constructs all `N` elements once in `data_` (`std::array<T, N> data_{}`), `push`/`push_value` assign into an existing object and `pop` only advances `read_index_` — no construction, no destruction on the hot path. The benchmark payload is only 8 bytes but carries a user-defined destructor and a shared `static inline std::atomic<int> live`, so each construct/destruct pair is two contended cross-core RMWs on one line; that is what collapses the other four. **The 10x applies only to types that genuinely need construction/destruction** — for POD payloads the gap disappears and Ring is actually slower than Inline (345 K vs 457 K at 8 B in the same run) because it pays the claim/commit protocol. The trade-off is real: Ring requires `T` to be assignable and default-constructible, and a reference returned by `front()` is only valid until `pop()`.
* **Beyond 32 B the queue is no longer the bottleneck.** All models sit within ±25 % of each other, and `throughput × length` flattens to ~1.4 GB/s from 256 B to 1 KB (5.5 K ops/ms × 256 B, 2.8 × 512 B, 1.4 × 1 KB): the limit is cross-core cache-line transfer and memory bandwidth, not the queue protocol.
* **Controlled-backlog sweep (4 B, 1 M iters):** at depth 0 (uncontrolled) zrqueue leads `rigtorp` by 2.5–3.0x; once the producer is throttled to depth ≥ 32 the gap closes to 1.06–1.38x. The large single-element advantage therefore comes from the unthrottled fast path, not from full-queue behaviour.
* **Heap "loses to rigtorp at depth 32/128" was noise, and the codegen fix for it regresses (1.9.3).** The proposal was to hoist `slots_`/`mask_` into locals and add `__restrict` so MSVC would stop reloading them from the cold-data line. Two findings killed it. First, the premise: a 15-rep interleaved A/B (cap 4096, 1 M iters, order rotated) put Heap/rigtorp at **1.045 / 1.056 / 1.064 / 1.004 at depth 32** across four runs — Heap *ahead* — and 0.942 / 0.975 / 1.002 / 0.977 at depth 128, i.e. within ±6 % with heavily overlapping min–max ranges (rigtorp 62–154 K vs Heap 81–151 K in the same cell). The single-run table rows that suggested a consistent deficit do not reproduce. Second, the fix itself: a prototype with hoisted locals measured **0.73x at depth 0** and 0.95–0.98x at depths 32–1024, while `__restrict` on the same code changed nothing. MSVC was evidently already keeping the base pointer and mask in registers; forcing the issue only constrained the register allocator. No library change made.
* **No model wins every dimension.** Mirror wins 4 B latency and 4 B throughput; Ring wins bulk throughput, non-POD throughput, and ≥256 B latency; Inline is competitive on 4 B controlled-backlog depth ≥ 128; Heap is the most stable across placements and supports runtime capacity plus HugePages. Pick by element size, backlog shape, and whether the path is bulk or single-element.
* **`rigtorp@4096` is the noisiest row** (82–335 K across runs while its own @16M row stays at ~490–570 K; four consecutive `--rt`-style runs gave 82–235, 209–259, 164–256 and 186–264 K). It spins without `pause` and holds no cached index, so a 4 K ring puts it in the full-queue slow path most of the time; treat that cell as "slow path stress", not a stable throughput figure. Because it is the denominator of every ratio in that table, the `int @4096` multipliers move with it — the zrqueue columns themselves reproduced within ±3 % across those four runs.
* **`size()`/`empty()` are observability only.** They issue two `relaxed` loads and are not a snapshot; the subtraction is clamped, but they must never drive flow control.
* **Adaptive mirror gating was measured and rejected (1.9.3).** Skipping the mirror store once the producer estimates a deep backlog sounds free — the consumer falls back to the slots anyway — but the estimate requires periodically re-reading the consumer's index line, i.e. exactly the cross-core load the mirror exists to avoid. Two interleaved A/B builds (`ZRQUEUE_MIRROR_ADAPTIVE_DEPTH` 0 vs 1, 10 M iters × 7 reps, cores 2/4, `--rt`): gating **off** 483–576 K ops/ms @4096 and 546–563 K @16M; gating **on** 331–342 K and 396–438 K, with RTT p50 also worse (146–150 ns vs 152–165 ns). Both axes regress, so the switch was removed rather than kept as dead configuration.
* **`push_value` is an ergonomics win, not a throughput win.** A focused same-process A/B (`SpscRingBuffer<int, 16M>`, 10 M iters × 9 reps, order rotated per rep) put `push_value` within −2 % … +8.6 % of `push(Writer)` across four runs, with medians differing by under 1 % in three of them: with an inlined trivial writer both forms compile to the same claim/store sequence. Use it because it is shorter and returns a bool, not because it is faster. The `sizeof(T) > cache line` branch does route through `spin_alloc()`, which is the path that measurably helps large payloads.
* **A mirror bulk *producer* fast path has no premise (measured; `push_bulk` added for parity only).** The original proposal was that shallow bursts larger than the mirror window (K = 4 at 4 B, 2 at 8 B) fall off the mirror path, so a `try_push_bulk` writing K entries behind one release would pay off. A dedicated burst ping-pong (cap 256, 400 K bursts × 7 reps, order rotated, ns per element) says otherwise: from burst 2 upward every model is within noise of the others — burst 2 ≈ 110 ns, 4 ≈ 58, 8 ≈ 30, 16 ≈ 18, 32 ≈ 11 for all of Mirror/Heap/Inline/RingBulk. Only burst 1 separates, and there the mirror already wins (157–163 ns vs 213–231 for Heap/Inline). The predicted "burst > K cliff" does not exist: past the first element the payload line is already resident and the extra mirror store costs nothing measurable. `push_bulk` was nevertheless added in 1.9.3 for signature parity, and measured neutral against the per-element loop (0.92–1.19x, no consistent direction); an isolation run against a mirror-disabled type confirmed the trailing mirror loop itself is free. The *consumer* side is different: `consume_bulk` collapses only the retire side and does pay off at batch 2–32 (see the usage section).

**Methodology note:** on a shared or virtualized host the hypervisor can co-locate both vCPUs on SMT siblings of one physical core, producing 40–106 ns "cross-core" round trips that are physically impossible. Such runs were discarded; the table above is from a bare-metal host with distinct cores pinned. Tail percentiles on a general-purpose OS measure scheduler jitter — run `benchmark 2 4 --rtt-only --rt` (high priority) and, on Linux, `isolcpus`/`nohz_full` for publishable p99.9.

## Usage & Examples

### 1. Disruptor-Style Ring Buffer (Absolute Zero-Copy)
Best for complex structs. You don't create an object and push it; instead, you claim a pre-allocated slot, write to it in-place, and commit.
```cpp
#include "zrqueue.h"

struct Tick { int symbol; double price; };

// Capacity must be a power of 2. Pre-allocates and pre-warms memory.
zrqueue::SpscRingBuffer<Tick, 1024> ring_buffer;

// Producer Thread: Claim, Write, Commit
Tick* tick = ring_buffer.alloc();
if (tick) {
    tick->symbol = 1001;
    tick->price = 3500.50;
    ring_buffer.push(); // Commit (Executes Release Barrier)
}

// Alternatively, using the lambda API:
ring_buffer.try_push([](Tick* t) {
    t->symbol = 1001;
    t->price = 3500.50;
});

// Already hold a value? Skip the claim/write/commit triple: one alloc(),
// one assignment, one release store. try_push_value returns false when full.
ring_buffer.push_value(Tick{ 1001, 3500.50 });

// Burst path: claim a contiguous run, write in-place, commit with ONE barrier.
// The run never straddles the ring's wrap point; loop to claim the remainder.
size_t granted = 0;
if (Tick* batch = ring_buffer.alloc_bulk(16, granted)) {
    for (size_t i = 0; i < granted; ++i) {
        batch[i].symbol = 1001;
        batch[i].price  = 3500.50;
    }
    ring_buffer.commit(granted); // Partial commit (n <= granted) is allowed
}

// Busy-wait producer variant (counterpart of spin_front):
Tick* t2 = ring_buffer.spin_alloc();
t2->symbol = 1002;
ring_buffer.push();

// Consumer bulk drain (zero-copy, single barrier, elements never destructed).
// Handlers must be noexcept: consume_bulk is noexcept and asserts it at compile time.
ring_buffer.consume_bulk([](Tick& t) noexcept { process(t); });

// Contiguous batch read: claim a run, process it in place, retire it with one
// release store. The run never straddles the wrap point - call again for the rest.
size_t count = 0;
Tick* batch = ring_buffer.front_bulk(64, count);
if (batch) {
    process_batch(batch, count);
    ring_buffer.pop_bulk(count);
}
```

Debug builds (`assert` enabled) guard the claim/commit discipline: claiming twice
before committing, committing more than granted, or `push()`-ing a bulk claim all abort.

### 2. Static Inline Queue (Zero Heap Allocation)
Data is embedded directly in the class footprint via `std::byte` to avoid Strict-Aliasing violations. Best declared in the `.BSS` segment (global/static) to completely avoid stack overflow and pointer dereferencing.
```cpp
// 4096 elements embedded in-place
static zrqueue::SpscInlineQueue<Tick, 4096> g_hot_queue;

void strategy_thread() {
    // spin_front() safely busy-polls while protecting CPU frequencies
    Tick* tick = g_hot_queue.spin_front();
    
    // Lookahead (peek at the next element without popping)
    Tick* next_tick = g_hot_queue.peek(1); 

    if (tick) {
        process(tick);
        g_hot_queue.pop(); // Manually destruct (if non-POD) and advance index
    }
}
```

### 3. Dynamic Heap Queue with Bulk Operations
Best for UDP network gateways. When receiving 10 packets via `recvmmsg`, push them with a single atomic barrier.
```cpp
zrqueue::SpscHeapQueue<Tick> queue(1024); // Dynamically rounds up to 1024
Tick buffer[10] = { ... };

// Pushes up to 10 elements. Executes std::memory_order_release ONLY ONCE!
size_t pushed = queue.push_bulk(buffer, 10);

// Consumer eats all available elements zero-copy (handler must be noexcept)
queue.consume_bulk([](Tick& tick) noexcept {
    process(tick);
}); // Executes std::memory_order_release ONLY ONCE at the end!

// Or take a contiguous run and process it as an array
size_t count = 0;
if (Tick* batch = queue.front_bulk(64, count)) {
    process_batch(batch, count);
    queue.pop_bulk(count);   // one release store for the whole run
}
```

### 4. HugePage Allocator (Massive Queues)
Ideal for preserving historical ticks without thrashing the CPU TLB.
```cpp
// Requires OS permissions (e.g., `vm.nr_hugepages` in Linux or "Lock Pages" in Windows)
zrqueue::SpscHeapQueue<Tick, zrqueue::HugePageAllocator<Tick>> history_queue(1048576);

// If huge pages are unavailable the allocator silently falls back to normal 4KB pages.
// Check once at startup to confirm you actually got huge pages:
if (zrqueue::hugepage_fallback_occurred()) {
    log_warn("HugePages unavailable — running on 4KB pages, expect TLB pressure");
}
```

### 5. Mirrored Queue (Shallow-Flow RTT)
Best for latency-critical ping-pong and near-empty flows. The payload rides the producer's index cache line, so the consumer's poll fetches
index and data in one cross-core migration. The mirror is deliberately single-element (no bulk API); use `SpscRingBuffer` for bursts.

```cpp
// 4 B and arithmetic 8 B scalars are mirrored automatically.
zrqueue::SpscMirrorQueue<int, 256> q4;
q4.emplace(42);

zrqueue::SpscMirrorQueue<uint64_t, 256> q8;
q8.emplace(0x123456789ABCDEF0ULL);

// Custom 8 B type: opt in explicitly (both flags must be defined).
struct MyTick8 { uint64_t value; /* ... */ };
template <> struct zrqueue::mirror_traits<MyTick8> {
    static constexpr bool enable_4b = false;
    static constexpr bool enable_8b = true;
};
zrqueue::SpscMirrorQueue<MyTick8, 256> q8c;

// A constructor that emits byte/field stores defeats the default slot
// read-back (store-to-load forwarding). Supply the payload explicitly...
struct Tick4 { unsigned char a, b, c, d; /* ... */ };
zrqueue::SpscMirrorQueue<Tick4, 256> qt;
qt.emplace_packed(static_cast<uint32_t>(x), x); // payload == object bytes

// ...or teach emplace() to pack from the constructor arguments once.
template <> struct zrqueue::mirror_codec<Tick4> {
    static uint32_t pack(uint64_t x) noexcept { return static_cast<uint32_t>(x); }
};
qt.emplace(x); // now uses the codec instead of reading the slot back

// Consumer: hold the pointer returned by front(); a second front() call
// falls back to the slot path and voids the piggyback.
Tick4* f = qt.front();
if (f) {
    process(*f);
    qt.pop();
}
```

`mirror_traits<T>` can also disable the 4 B mirror for a type that regresses. `emplace_packed` / `try_emplace_packed` /
`push_packed` / `try_push_packed` are available on every mirror-eligible queue. The payload contract is strict: it must equal the
object representation of `T`; for 4 B types that is a `uint32_t`, for 8 B types a `uint64_t`.

**Bulk push.** `push_bulk(Iterator first, size_t count)` matches the signature and contract of `SpscHeapQueue`/`SpscInlineQueue`:
it constructs up to `count` elements and publishes them with **one** release store, returning how many it actually wrote (partial
push when the queue cannot fit the run, 0 when full). Mirror entries are written only for the trailing K elements of the run — an
entry for element `i` is overwritten by element `i+K`, so mirroring earlier elements would be wasted stores. Elements outside that
window are served from their slots, which are authoritative.

```cpp
Tick4 batch[16] = { ... };
size_t n = qt.push_bulk(batch, 16); // one release store for the whole run
```

Performance is **neutral, not a speedup**: a 7-rep interleaved A/B (cap 65536, 200 K rounds, order rotated) put `push_bulk` at
0.92–1.19x of a per-element `emplace` loop with no consistent direction across batch sizes 1–32. An isolation run calling the same
`push_bulk` on a mirrored type and on a compile-time mirror-disabled type produced identical times at every batch (e.g. batch 8:
24.4 vs 24.5 ns, batch 16: 13.3 vs 13.2 ns), so the trailing mirror loop costs nothing. The API exists for parity with the other
models; the win is one barrier instead of N, which only matters when N is large and the producer is not already waiting on a full
queue — and in that regime the per-element path is equally fast.

**Bulk drain.** `consume_bulk(handler, max_count)` is available too, but with a different shape from the other three models: there is
no `front_bulk`, because a mirror hit unpacks into a single-element stash and a run can mix mirror hits with slot fallbacks, so no
contiguous run exists to hand back. The handler therefore receives one `T&` at a time — still no copy — and the whole run retires
with **one** release store instead of one per element:

```cpp
// Drain whatever is published, or cap the run.
size_t n = qt.consume_bulk([](Tick4& t) noexcept { process(t); });
```

Measured against an equivalent per-element `front()`/`pop()` loop (cap 65536, 200 K batches × 7 reps, order rotated, both drains
consuming the same fixed batch so only the release-store granularity differs): **+18–41 % at batch 2–32**, neutral at batch 1 and at
batch ≥ 64 (per-element transfer dominates). The mirror probe inside the loop is skipped when the backlog exceeds the window
(`available > MIRROR_K`), since a probe that must miss is pure overhead — without that guard the loop measured 16 % slower than the
manual drain.

## Build Instructions
As a single-header library, `zrqueue` requires no complex build systems or external dependencies. Simply drop `zrqueue.h` into your source tree and include it.

* **C++ Standard:** Requires **C++17** or later (uses `if constexpr`, `std::byte`, etc.). Enforced at compile time: the header `#error`s on an older standard, checking `_MSVC_LANG` because MSVC reports `__cplusplus` as `199711L`.
* **Compilers:** GCC, Clang, or MSVC.
* **OS:** Linux, macOS, or Windows.
* **Architecture:** x86_64, aarch64.

On Windows the header defines `WIN32_LEAN_AND_MEAN` and `NOMINMAX` before including `<windows.h>`, so it never leaks `min`/`max` macros into the including translation unit.

To compile the benchmark tool, use the provided `CMakeLists.txt`:
```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . --config Release

# Run benchmark with Thread Affinity (Producer on Core 2, Consumer on Core 4)
./benchmark 2 4
```
*Note: optimisation flags are per-configuration (`CMAKE_CXX_FLAGS_RELEASE` and friends), not global, so a Debug build keeps its assertions and stays debuggable. GCC/Clang Release gets `-O3 -DNDEBUG -march=native -flto=auto -fomit-frame-pointer`; MSVC Release gets `/O2 /Ob2 /GL /DNDEBUG` plus `/LTCG`. **`NDEBUG` is required for meaningful numbers** — without it `rigtorp`'s own `pop()` assertion costs a cross-core load per element and makes the comparison meaningless.*

## License
MIT License. Copyright (c) 2024 Bolide Zhang.
