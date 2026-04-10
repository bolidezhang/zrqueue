# zrqueue

## Overview
`zrqueue::SpscQueue` is a wait-free, lock-free, fixed-size single-producer single-consumer queue provided as a **single-header** C++ library. 
It is proven to be significantly faster and more stable than `rigtorp::SPSCQueue`. 

Designed strictly for High-Frequency Trading (HFT) and ultra-low latency (ULL) systems where every nanosecond counts, 
`zrqueue` applies hardware-level micro-architectural optimizations including zero-branching, strict cache-line isolation, OS HugePages support, and hardware pausing. 
Because it is entirely self-contained in one file, it offers the ultimate ease of integration—just drop it in and compile.

## Key Features

* **Single-Header & Drop-in Ready:** No building, no linking, no external dependencies. Just `#include "zrqueue.h"` and you are ready to go.
* **Zero Branch Misprediction:** Uses monotonically increasing `uint64_t` indices and bitwise AND (`& mask_`) instead of modulo or `if`-based wrap-around.
* **Perfect Cache Line Isolation:** Every heavily-contended atomic variable and cached index strictly occupies its own dedicated 64-byte L1 Cache Line to utterly eradicate Cache Coherence Ping-Pong and False Sharing.
* **Static In-Place Array (`SpscInlineQueue`):** An alternative zero-allocation queue where data is embedded directly within the object footprint, eliminating the final pointer-chasing (dereference) penalty.
* **Custom HFT Allocators:** 
  * `AlignedAllocator`: Rounds up capacity to strictly align with cache lines, preventing tail false-sharing.
  * `HugePageAllocator`: Cross-platform support for 2MB/1GB OS Huge Pages to eliminate TLB Misses on massive queues.
* **Micro-burst Defense:** Native `push_bulk` and `consume_bulk` APIs that execute only **one** memory barrier for `N` elements.
* **Hardware Thermal Throttling Defense:** Built-in `spin_front()` utilizing `_mm_pause()` / `yield` to prevent CPU ALU burnout and thermal downclocking during 100% busy-polling.

## Architectural Superiority

When compared to `rigtorp::SPSCQueue` (the industry standard for lock-free queues), `zrqueue` applies "surgical" rewrites specifically aimed at the HFT Hot Path.

| Feature | `rigtorp::SPSCQueue` | `zrqueue::SpscQueue` | HFT Advantage |
| :--- | :--- | :--- | :--- |
| **Indexing Model** | `0` to `N-1` Wrap-around | Infinite Monotonic (`uint64_t`) | Math simplicity, no boundary edge cases. |
| **Addressing (Hot Path)** | `if (next == cap) next = 0;` | Bitwise `& mask_` | **Zero Branching.** Saves ~10ns periodic branch misprediction penalty (Jitter). |
| **Queue Capacity** | Any size (wastes 1 slack slot) | Strictly Power of 2 | 100% memory utilization, enables bitwise masking. |
| **False Sharing Defense** | Internal Padding Elements | Strict `AlignedAllocator` / `alignas` | No wasted array slots, perfectly aligns to physical OS pages. |
| **TLB Miss Defense** | Standard `malloc` (4KB pages) | `HugePageAllocator` (2MB/1GB) | Sustains max throughput on multi-million element queues (e.g., Order Book ticks). |
| **Pointer Dereference** | Always `slots_[idx]` (Heap) | `SpscInlineQueue` (In-place) | Faster base-offset addressing, zero dynamic allocation. |

## Performance Comparison

### Real-World Benchmark (Intel Core i5-8400, 6 Cores)
Under strict CPU core pinning and `-O3 -march=native` optimizations, `zrqueue` demonstrates a massive throughput advantage and lower latency due to the elimination of conditional jump instructions and perfect physical memory alignment.

**`rigtorp::SPSCQueue`:**
* Throughput: `182,938 ops/ms`
* Latency: `177 ns RTT`

**`zrqueue::SpscQueue`:**
* Throughput: `401,993 ops/ms` (🚀 **2.19x Faster**)
* Latency: `158 ns RTT` (🚀 **10.7% Lower Latency**)

## Usage & Examples

### Dynamic Allocation Queue
Best for general ultra-low latency message passing.
```cpp
#include "zrqueue.h"

struct Tick { int symbol; double price; };

// Capacity will be automatically rounded up to the nearest power of 2
zrqueue::SpscQueue<Tick> queue(1024);

// Producer Thread
queue.emplace(1001, 3500.50);

// Consumer Thread (Zero-Copy In-Place Processing)
Tick* tick = queue.front();
if (tick) {
    process(tick->price);
    queue.pop(); // Manually destruct and release slot
}
```

### Static Queue (Absolute Zero Allocation)
Data is embedded directly in the class. Best declared in the `.BSS` segment (global/static) to completely avoid stack overflow and pointer dereferencing.

```cpp
// Strictly requires a power-of-2 template argument.
static zrqueue::SpscInlineQueue<Tick, 4096> g_hot_queue;

void strategy_thread() {
    // spin_front() safely busy-polls while protecting CPU frequencies
    Tick* tick = g_hot_queue.spin_front();
    if (tick) {
        process(tick);
        g_hot_queue.pop();
    }
}
```

### Bulk Operations (Micro-burst Defense)
When the network gateway receives 10 UDP packets at once, push them with a single atomic barrier.

```cpp
Tick buffer[10] = { ... };

// Pushes up to 10 elements. Executes std::memory_order_release ONLY ONCE!
size_t pushed = queue.push_bulk(buffer, 10);

// Consumer eats all available elements zero-copy
queue.consume_bulk([](Tick& tick) {
    process(tick);
}); // Executes std::memory_order_release ONLY ONCE at the end!
```

### HugePage Allocator (For Massive Queues)
Ideal for preserving historical ticks without thrashing the CPU TLB.

```cpp
// Requires OS permissions (e.g., `vm.nr_hugepages` in Linux or "Lock Pages" in Windows)
zrqueue::SpscQueue<Tick, zrqueue::HugePageAllocator<Tick>> history_queue(10000000);
```

### Build Instructions
As a single-header library, zrqueue requires no complex build systems or external dependencies. Simply drop zrqueue.h into your source tree and include it.
Requirements:
C++ Standard: Requires C++17 or later. 
Compilers: GCC, Clang, or MSVC.
OS: Linux, macOS, or Windows.
Architecture: x86_64, aarch64.
To compile the benchmark tool, use the provided CMakeLists.txt. It is highly recommended to use the modern CMake build command:
```cpp
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . --config Release

# Run benchmark with Thread Affinity (Producer on Core 2, Consumer on Core 4)
./benchmark 2 4
```
Note: The CMake configuration automatically applies -O3, -march=native, -flto=auto, and -fomit-frame-pointer for maximum performance.

### License
MIT License. Copyright (c) 2024 Bolide Zhang.
