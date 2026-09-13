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
  * `SpscMirrorQueue`: Latency-optimized for shallow-flow (near-empty) ping-pong. For `sizeof(T) <= 4`, the producer packs each element into an atomic `{seq | payload}` entry inside its **own index cache line**, so the consumer's polling load of `write_index_` fetches index *and* payload in **one** cross-core migration — halving the theoretical line migrations per round trip (4 → 2). In practice, a four-placement measurement sweep on a shared host measured it as the **lowest-median-RTT model in 4 of 4 placements** at 16M capacity (205 – 261 ns) and 3 of 4 at 4096; on a fully-quiet dedicated host the simpler Heap/Ring models can still match or beat it — the saved migration is partly offset by the extra pack/unpack work. Deep-backlog throughput at large capacities matches the classic path; at small capacities it sits below `SpscInlineQueue` and is the most placement-sensitive of the four models (mirror bookkeeping on the hot line). **Usage caveat:** hold the pointer returned by `front()` — a second `front()` call takes the classic slot path and re-reads the slot line, silently voiding the piggyback (neutral for the other models).
* **Zero Branch Misprediction:** Uses monotonically increasing `uint64_t` indices and bitwise AND (`& mask_`) instead of modulo or `if`-based wrap-around.
* **Perfect Cache Line Isolation:** Every heavily-contended atomic variable strictly occupies its own dedicated 64-byte L1 Cache Line to utterly eradicate Cache Coherence Ping-Pong and Asymmetric False Sharing.
* **Memory Pre-Warming & Page-Fault Defense:** Aggressive use of value-initialization (`data_{}`) to trigger OS page faults during the application startup phase, ensuring zero latency spikes during live market hours.
* **Trivial Destructor Elision:** Leverages C++17 `if constexpr (!std::is_trivially_destructible_v<T>)` to completely bypass destruction overhead for POD types (Zero-Cost Abstraction).
* **Lookahead Support:** Native `peek(offset)` API allows consumers to inspect future elements without committing a `pop()`.
* **Micro-burst Defense:** Native `push_bulk` and `consume_bulk` APIs (plus `alloc_bulk`/`commit` on `SpscRingBuffer`) that execute only **one** atomic memory barrier for `N` elements.
* **Hardware Thermal Throttling Defense:** Built-in `spin_front()` extracts invariant loads out of the loop and utilizes `_mm_pause()` / `yield` to prevent CPU ALU burnout and thermal downclocking during 100% busy-polling.

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

The bundled benchmark runs **interleaved repetitions** (7 throughput reps, 5 RTT reps — canceling thermal drift and execution-order bias), reports **min / median / max**, and sweeps **two capacities**: 4096 (exercises full/empty slow paths) and 16M (classic setup).

### Measured results (shared-VM host, 4 placement sweeps)

All numbers below are **medians**, each shown as the range observed across **four independent placement conditions** (unpinned + three CPU-pinning pairs) on a shared virtualized host (Meteor Lake class, Linux 6.8), `-O3 -march=native`, 10M iterations per throughput rep, 2M RTTs per latency rep. Range width is dominated by host-side vCPU placement noise, not by the queues.

**Throughput, capacity 4096 (slow paths exercised), K ops/ms — median range:**

| Model | median range |
| :--- | :--- |
| `rigtorp::SPSCQueue` emplace/front/pop | 130 – 183 |
| `SpscHeapQueue` emplace/front/pop | 324 – 386 |
| `SpscInlineQueue` emplace/front/pop | 328 – 373 |
| `SpscMirrorQueue` emplace/front/pop | 209 – 341 |
| `SpscRingBuffer` alloc/write/push | 246 – 301 |
| `SpscRingBuffer` alloc_bulk/commit x64 | **679 – 917** |
| `SpscHeapQueue` push_bulk/consume_bulk x64 | 486 – 648 |

**Throughput, capacity 16M (classic setup), K ops/ms — median range:**

| Model | median range |
| :--- | :--- |
| `rigtorp::SPSCQueue` emplace/front/pop | 166 – 203 |
| `SpscHeapQueue` emplace/front/pop | 304 – 319 |
| `SpscInlineQueue` emplace/front/pop | 341 – 493 |
| `SpscMirrorQueue` emplace/front/pop | 311 – 391 |
| `SpscRingBuffer` alloc/write/push | 361 – 404 |
| `SpscRingBuffer` alloc_bulk/commit x64 | **747 – 846** |
| `SpscHeapQueue` push_bulk/consume_bulk x64 | 298 – 431 |

**Round-trip latency, ns RTT — median range:**

| Model | @4096 | @16M |
| :--- | :--- | :--- |
| `rigtorp::SPSCQueue` | 265 – 318 | 275 – 311 |
| `SpscHeapQueue` | 267 – 335 | 266 – 312 |
| `SpscInlineQueue` | 264 – 330 | 254 – 310 |
| `SpscMirrorQueue` | **223 – 282** | **205 – 261** |
| `SpscRingBuffer` | 278 – 331 | 269 – 293 |

**Findings that held in 4 of 4 placements:**

* **Every zrqueue model beat `rigtorp::SPSCQueue` in every scenario** — throughput medians 1.8x – 2.9x, and rigtorp was never the lowest-RTT model.
* **`SpscMirrorQueue` is the RTT champion**: lowest median RTT at 16M in 4 of 4 placements (205 – 261 ns), and at 4096 in 3 of 4. Its throughput — especially at 4096 — is the most placement-sensitive of the four models and sits below `SpscInlineQueue`.
* **`SpscRingBuffer` `alloc_bulk`/`commit` is the throughput king**: ~2x the best single-element model at both capacities, thanks to one release barrier per 64-element burst and single-touch in-place writes.
* **`SpscHeapQueue` single-element throughput is the most stable** across placements (±2% at 4096) — its cache-line isolation holds up under host noise.
* **For large-capacity bursts, prefer `SpscRingBuffer` over `push_bulk`**: `SpscHeapQueue::push_bulk` throughput drops at 16M vs 4096 in all placements (producer sprints ahead, slots go cold, memory-bandwidth bound), while `alloc_bulk`/`commit` retains its advantage.

**Methodology note (why min values are not quoted):** on an oversubscribed shared host, the hypervisor can co-locate the two vCPUs onto SMT siblings of one physical core. An entire 2M-RTT rep then averages 51 – 106 ns — physically impossible for a true cross-core round trip (~200 ns floor), which we observed repeatedly on pinning pairs involving one particular vCPU. All min values from such runs were discarded; only medians and maxima are meaningful on this class of host. On a quiet dedicated machine, expect the same *ordering* but tighter ranges.

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

// Consumer bulk drain (zero-copy, single barrier, elements never destructed):
ring_buffer.consume_bulk([](Tick& t) { process(t); });
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

// Consumer eats all available elements zero-copy
queue.consume_bulk([](Tick& tick) {
    process(tick);
}); // Executes std::memory_order_release ONLY ONCE at the end!
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
