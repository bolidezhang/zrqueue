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
  * `SpscMirrorQueue`: Latency-optimized for shallow-flow (near-empty) ping-pong. For `sizeof(T) <= 4`, the producer packs each element into an atomic `{seq | payload}` entry inside its **own index cache line**, so the consumer's polling load of `write_index_` fetches index *and* payload in **one** cross-core migration — halving the theoretical line migrations per round trip (4 → 2). Measured on a bare-metal Core Ultra 5 125H (cores 2/4 pinned): at 4 B it is the lowest-RTT model on all four percentiles (165 ns p50 / 431 ns p99.9 vs 204 / 456 for `rigtorp`) and the fastest at 466 K ops/ms. The advantage is strictly tied to the 4-byte mirror path — from 8 B upward the mirror is compiled out and all models converge to ~205 ns p50. On a quiet dedicated host the simpler Heap/Ring models can match it, since the saved migration is partly offset by pack/unpack work, and it remains the most placement-sensitive of the four models. **Usage caveat:** hold the pointer returned by `front()` — a second `front()` call takes the classic slot path and re-reads the slot line, silently voiding the piggyback (neutral for the other models).
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

**Conditions:** Intel Core Ultra 5 125H (14 cores / 18 threads, P+E), Windows 10.0.26300, MSVC 19.51 `/O2 /Ob2 /Oy- /GT /LTCG`, consumer pinned to core 2 and producer to core 4, `__rdtsc()` calibrated at 0.333868 ns/cycle (2.995 GHz). Throughput in K ops/ms, latency in ns, all medians over interleaved reps. **These are host-specific numbers — do not compare them with results from other machines, VMs, or compilers** (a 4-vCPU Ubuntu VM produced non-monotonic length sweeps and 2.2–2.4 µs p99.9 with the same code).

**Throughput by payload length, capacity 4096 (K ops/ms):**

| bytes | `rigtorp` | `SpscHeapQueue` | `SpscInlineQueue` | `SpscMirrorQueue` | `SpscRingBuffer` |
| :--- | ---: | ---: | ---: | ---: | ---: |
| 4 | 170 | 376 | 359 | **466** | 488 |
| 8 | 307 | 484 | **539** | 521 | 341 |
| 16 | 140 | 142 | **180** | 156 | 129 |
| 32 | **74** | 72 | 84 | 75 | 75 |
| 64 | 37 | 40 | **47** | 42 | 42 |
| 128 | 18 | 22 | **25** | 24 | 19 |
| 256 | 5.5 | 5.6 | 5.4 | 5.5 | **5.9** |
| 512 | 2.9 | 2.7 | 2.9 | **3.0** | 3.1 |
| 1 KB | 1.4 | **1.5** | 1.5 | 1.5 | 1.4 |
| non-POD | 30 | 29 | 31 | 31 | **435** |

**Throughput, `int` payload (K ops/ms):**

| Variant | @4096 | @16M |
| :--- | ---: | ---: |
| `rigtorp` emplace/front/pop | 246 | 284 |
| `SpscHeapQueue` emplace/front/pop | 523 | 539 |
| `SpscInlineQueue` emplace/front/pop | 396 | 479 |
| `SpscMirrorQueue` emplace/front/pop | **528** | **540** |
| `SpscRingBuffer` alloc/write/push | 435 | 437 |
| `SpscRingBuffer` alloc_bulk/commit ×64 | **1057** | **949** |
| `SpscHeapQueue` push_bulk/consume_bulk ×64 | 723 | 663 |

**Round-trip latency (ns):**

| Variant | @4096 | @16M |
| :--- | ---: | ---: |
| `rigtorp` | 207 | 367 |
| `SpscHeapQueue` | 210 | 205 |
| `SpscInlineQueue` | 205 | 204 |
| `SpscMirrorQueue` | **161** | 233 |
| `SpscRingBuffer` | 188 | 262 |

**RTT percentiles by payload length (ns, capacity 256):**

| bytes | model | p50 | p90 | p99 | p99.9 |
| :--- | :--- | ---: | ---: | ---: | ---: |
| 4 | `rigtorp` | 204 | 223 | 252 | 456 |
| 4 | `SpscHeapQueue` | 201 | 225 | 249 | 466 |
| 4 | `SpscInlineQueue` | 202 | 222 | 293 | 479 |
| 4 | **`SpscMirrorQueue`** | **165** | **184** | 271 | **431** |
| 4 | `SpscRingBuffer` | 204 | 243 | 274 | 456 |
| 256 | `SpscRingBuffer` | **361** | **398** | **408** | **518** |
| 1 KB | `SpscRingBuffer` | **904** | **958** | 1127 | 5127 |

**Findings:**

* **Small elements (4–8 B) are where the design pays off.** `SpscMirrorQueue` reaches 466 K ops/ms at 4 B (2.7x `rigtorp`) and the lowest RTT of any model at 4 B on all four percentiles (165 ns p50, 31 % below `rigtorp`). From 8 B upward the mirror path is compiled out (`sizeof(T) <= 4`), and all five implementations collapse into a 204–212 ns p50 band.
* **`SpscRingBuffer` `alloc_bulk`/`commit` is the throughput ceiling** — ~1 MB ops/ms at both capacities, roughly 2x the best single-element model, because one release barrier covers 64 elements.
* **Non-POD elements reverse the ranking.** Every placement-new model (and `rigtorp`) drops to 29–31 K ops/ms because each element pays a constructor and destructor, while the pre-constructed `SpscRingBuffer` keeps 435 K — about **15x** the others.
* **Beyond 32 B the queue is no longer the bottleneck.** All models sit within ±25 % of each other, and `throughput × length` flattens to ~1.2–1.3 GB/s from 256 B to 1 KB: the limit is cross-core cache-line transfer and memory bandwidth, not the queue protocol.
* **Controlled-backlog sweep (4 B, 1 M iters):** at depth 0 (uncontrolled) zrqueue leads `rigtorp` by 2.3x; once the producer is throttled to depth ≥ 32 the gap closes to 1.15–1.3x. The large single-element advantage therefore comes from the unthrottled fast path, not from full-queue behaviour.
* **No model wins every dimension.** Mirror wins 4 B latency and 4 B/8 B throughput; Ring wins bulk throughput, non-POD throughput, and ≥256 B latency; Heap is the most stable across placements and supports runtime capacity plus HugePages. Pick by element size, backlog shape, and whether the path is bulk or single-element.
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

* **C++ Standard:** Requires **C++17** or later (uses `if constexpr`, `std::byte`, etc.).
* **Compilers:** GCC, Clang, or MSVC.
* **OS:** Linux, macOS, or Windows.
* **Architecture:** x86_64, aarch64.

To compile the benchmark tool, use the provided `CMakeLists.txt`:
```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . --config Release

# Run benchmark with Thread Affinity (Producer on Core 2, Consumer on Core 4)
./benchmark 2 4
```
*Note: The CMake configuration automatically applies `-O3`, `-march=native`, `-flto`, and `-fomit-frame-pointer` for maximum performance.*

## License
MIT License. Copyright (c) 2024 Bolide Zhang.
