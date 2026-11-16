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
  * `SpscMirrorQueue`: Latency-optimized for shallow-flow (near-empty) ping-pong. For `sizeof(T) <= 4`, the producer packs each element into an atomic `{seq | payload}` entry inside its **own index cache line**, so the consumer's polling load of `write_index_` fetches index *and* payload in **one** cross-core migration — halving the theoretical line migrations per round trip (4 → 2). Measured on a bare-metal Core Ultra 5 125H (cores 2/4 pinned): at 4 B it is the lowest-RTT model on all four percentiles (165 ns p50 / 433 ns p99.9 vs 205 / 513 for `rigtorp`) and the fastest single-element model at 439 K ops/ms. From 8 B upward the mirror is compiled out and all models converge to ~205 ns p50 — a 64-bit mirror variant was built and measured **10x slower** than the classic path (see Findings), because the producer has to read the element back out of a slot line the consumer currently owns. **Usage caveat:** hold the pointer returned by `front()` — a second `front()` call takes the classic slot path and re-reads the slot line, silently voiding the piggyback (neutral for the other models).
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

The bundled benchmark rotates the variant order on every repetition (cancel execution-order and cold-cache bias), synchronizes both threads on a start barrier, samples RTT with `__rdtsc()` plus a startup calibration (steady_clock is ~100 ns-granular on Windows), and reports medians. It sweeps two capacities (4096 exercises the full/empty slow paths, 16M is the classic setup), ten payload lengths (4 B – 1 KB plus a non-POD row), controlled backlog depths, and per-iteration RTT percentiles.

### Measured results (bare-metal Windows host)

**Conditions:** Intel Core Ultra 5 125H (14 cores / 18 threads, P+E), Windows 10.0.26300, MSVC 19.51 `/O2 /Ob2 /Oy- /GT /LTCG /DNDEBUG`, consumer pinned to core 2 and producer to core 4, `__rdtsc()` calibrated at 0.333868 ns/cycle (2.995 GHz). Throughput in K ops/ms, latency in ns, all medians over interleaved reps, two independent runs reported side by side. **These are host-specific numbers — do not compare them with results from other machines, VMs, or compilers** (a 4-vCPU Ubuntu VM produced non-monotonic length sweeps and 2.2–2.4 µs p99.9 with the same code).

> **`/DNDEBUG` is not optional.** It must be defined in the measured build. `rigtorp::SPSCQueue::pop()` asserts with a cross-core acquire load; with assertions live that single load costs a full cache-line transfer per element and understated `rigtorp` by **7x** in an earlier run of this very benchmark (47 K vs 332 K ops/ms at `int`, 4096). MSVC's own Release configuration defines it, but assigning to `CMAKE_CXX_FLAGS_RELEASE` discards CMake's default — hence it is listed explicitly in `CMakeLists.txt`.

**Throughput by payload length, capacity 4096 (K ops/ms, run 1 / run 2):**

| bytes | `rigtorp` | `SpscHeapQueue` | `SpscInlineQueue` | `SpscMirrorQueue` | `SpscRingBuffer` |
| :--- | ---: | ---: | ---: | ---: | ---: |
| 4 | 264 / 314 | 366 / 337 | 324 / 288 | **439 / 388** | 292 / 231 |
| 8 | 284 / 277 | 457 / 459 | 484 / 437 | 483 / 429 | 343 / 330 |
| 16 | 167 / 160 | 168 / 171 | 153 / 147 | 132 / 134 | 148 / 159 |
| 32 | **82 / 78** | 75 / 79 | 77 / 77 | 67 / 70 | 74 / 75 |
| 64 | 40 / 43 | 38 / 44 | 40 / 43 | 34 / 39 | **40 / 45** |
| 128 | 20 / 21 | 22 / 22 | 22 / 24 | 22 / 21 | 17 / 18 |
| 256 | 5.8 / 5.6 | 5.6 / 5.7 | 5.7 / 5.8 | 5.8 / 5.9 | **5.9 / 6.1** |
| 512 | 2.9 / 3.0 | 3.1 / 3.2 | 2.9 / 3.0 | 3.0 / 2.8 | 3.0 / 3.0 |
| 1 KB | 1.5 / 1.5 | 1.5 / 1.6 | 1.5 / 1.5 | 1.5 / 1.5 | 1.5 / 1.6 |
| non-POD | 25 / 22 | 26 / 26 | 25 / 21 | 24 / 21 | **344 / 318** |

**Throughput, `int` payload (K ops/ms, run 1 / run 2):**

| Variant | @4096 | @16M |
| :--- | ---: | ---: |
| `rigtorp` emplace/front/pop | 185 / 92 | 492 / 513 |
| `SpscHeapQueue` emplace/front/pop | 402 / 370 | 548 / 601 |
| `SpscInlineQueue` emplace/front/pop | 384 / 357 | 557 / 599 |
| `SpscMirrorQueue` emplace/front/pop | **454 / 436** | 557 / 572 |
| `SpscRingBuffer` alloc/write/push | 456 / 455 | 365 / 405 |
| `SpscRingBuffer` alloc_bulk/commit ×64 | **1028 / 971** | **935 / 1061** |
| `SpscHeapQueue` push_bulk/consume_bulk ×64 | 616 / 615 | 629 / 636 |

**Round-trip latency (ns, run 1 / run 2):**

| Variant | @4096 | @16M |
| :--- | ---: | ---: |
| `rigtorp` | 222 / 218 | 221 / 218 |
| `SpscHeapQueue` | 216 / 221 | 217 / 222 |
| `SpscInlineQueue` | 203 / 216 | 193 / 208 |
| `SpscMirrorQueue` | **177 / 176** | **164 / 182** |
| `SpscRingBuffer` | 196 / 223 | 217 / 232 |

**RTT percentiles by payload length (ns, capacity 256, run 2):**

| bytes | model | p50 | p90 | p99 | p99.9 |
| :--- | :--- | ---: | ---: | ---: | ---: |
| 4 | `rigtorp` | 205 | 209 | 272 | 513 |
| 4 | `SpscHeapQueue` | 208 | 211 | 267 | 483 |
| 4 | `SpscInlineQueue` | 204 | 208 | 243 | 448 |
| 4 | **`SpscMirrorQueue`** | **165** | **166** | **187** | **433** |
| 4 | `SpscRingBuffer` | 204 | 208 | 285 | 488 |
| 8 | `rigtorp` | 203 | 206 | 269 | 496 |
| 8 | `SpscMirrorQueue` | 211 | 213 | 277 | 498 |
| 128 | `rigtorp` | 234 | 237 | 287 | 496 |
| 256 | `SpscRingBuffer` | **401** | **407** | **466** | n/a |
| 1 KB | `SpscInlineQueue` | **916** | **938** | 1213 | n/a |

p99.9 is omitted from the ≥256 B rows because it is dominated by scheduler jitter on a general-purpose OS and swung 0.7–5.1 µs between the two runs (use `--rt` and core isolation to make it meaningful).

**Findings:**

* **Only `sizeof(T) <= 4` gets the mirror fast path.** `SpscMirrorQueue` wins 4 B on every axis: 439 K ops/ms (1.7x `rigtorp`) and the lowest RTT at all four percentiles (165 ns p50, 20 % below `rigtorp`, p99 187 ns vs 272 ns). From 8 B upward the mirror is compiled out and all five models converge into a 203–223 ns p50 band.
* **An 8 B mirror variant was implemented and rejected.** Piggybacking a 64-bit payload needs a 16 B entry (`{seq64 | payload64}` with a seqlock double-load, K = 2). It measured **54 K ops/ms at 8 B vs 513 K for the classic path — a 10x loss** on the same host. Cause: the producer must read the element back out of the slot it just constructed, and that line is currently owned by the consumer, so the read costs an extra cache-line transfer — exactly the migration the mirror was meant to save. Types whose constructor emits narrow stores (e.g. a byte-loop padding fill) also fail store-to-load forwarding: one 8 B store gave 197 K, two 4 B stores 48 K. The fast path is therefore deliberately limited to 4 B.
* **`SpscRingBuffer` `alloc_bulk`/`commit` is the throughput ceiling** — ~1 M ops/ms at both capacities, roughly 2x the best single-element model, because one release barrier covers 64 elements.
* **Non-POD elements reverse the ranking.** Every placement-new model (and `rigtorp`) drops to 21–26 K ops/ms because each element pays a constructor and destructor, while the pre-constructed `SpscRingBuffer` keeps 318–344 K — about **14x** the others.
* **Beyond 32 B the queue is no longer the bottleneck.** All models sit within ±25 % of each other, and `throughput × length` flattens to ~1.5 GB/s from 256 B to 1 KB (5.8 K ops/ms × 256 B, 3.0 × 512 B, 1.5 × 1 KB): the limit is cross-core cache-line transfer and memory bandwidth, not the queue protocol.
* **Controlled-backlog sweep (4 B, 1 M iters):** at depth 0 (uncontrolled) zrqueue leads `rigtorp` by 3.4–8.4x; once the producer is throttled to depth ≥ 32 the gap closes to 1.07–1.29x. The large single-element advantage therefore comes from the unthrottled fast path, not from full-queue behaviour.
* **No model wins every dimension.** Mirror wins 4 B latency and 4 B throughput; Ring wins bulk throughput, non-POD throughput, and ≥256 B latency; Inline leads the 4 B controlled-backlog sweep at every depth ≥ 32; Heap is the most stable across placements and supports runtime capacity plus HugePages. Pick by element size, backlog shape, and whether the path is bulk or single-element.
* **`rigtorp@4096` is the noisiest row** (92–185 K across runs while its own @16M row stays at ~500 K). It spins without `pause` and holds no cached index, so a 4 K ring puts it in the full-queue slow path most of the time; treat that cell as "slow path stress", not a stable throughput figure.
* **`size()`/`empty()` are observability only.** They issue two `relaxed` loads and are not a snapshot; the subtraction is clamped, but they must never drive flow control.

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
