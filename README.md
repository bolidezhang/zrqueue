这里是完整的、纯 Markdown 格式的 `README.md` 内容。你可以直接点击右上角的“复制”按钮，保存为 `README.md` 文件。

```markdown
# zrqueue

## Overview
`zrqueue` is a wait-free, lock-free, single-producer single-consumer (SPSC) queue library provided as a **single-header** C++17 library. 
It is proven to be significantly faster and more stable than the industry-standard `rigtorp::SPSCQueue`. 

Designed strictly for High-Frequency Trading (HFT) and ultra-low latency (ULL) systems where every nanosecond counts, 
`zrqueue` applies extreme hardware-level micro-architectural optimizations. By offering three distinct memory models (Heap-allocated, In-place embedded, and Pre-constructed Ring Buffer), it covers every possible hot-path scenario. Because it is entirely self-contained in one file, it offers the ultimate ease of integration—just drop it in and compile.

## Key Features

* **Three Distinct Queue Models:**
  * `SpscQueue`: Dynamic capacity, heap-allocated, supports OS HugePages.
  * `SpscInlineQueue`: Zero-allocation, data is embedded directly within the object footprint via `std::byte` arrays, eliminating the final pointer-chasing penalty.
  * `SpscRingBuffer`: Disruptor-style claim/commit paradigm. Objects are pre-constructed and memory-pre-faulted, enabling absolute zero-copy modifications.
* **Zero Branch Misprediction:** Uses monotonically increasing `uint64_t` indices and bitwise AND (`& mask_`) instead of modulo or `if`-based wrap-around.
* **Perfect Cache Line Isolation:** Every heavily-contended atomic variable strictly occupies its own dedicated 64-byte L1 Cache Line to utterly eradicate Cache Coherence Ping-Pong and Asymmetric False Sharing.
* **Memory Pre-Warming & Page-Fault Defense:** Aggressive use of value-initialization (`data_{}`) to trigger OS page faults during the application startup phase, ensuring zero latency spikes during live market hours.
* **Trivial Destructor Elision:** Leverages C++17 `if constexpr (!std::is_trivially_destructible_v<T>)` to completely bypass destruction overhead for POD types (Zero-Cost Abstraction).
* **Lookahead Support:** Native `peek(offset)` API allows consumers to inspect future elements without committing a `pop()`.
* **Micro-burst Defense:** Native `push_bulk` and `consume_bulk` APIs that execute only **one** atomic memory barrier for `N` elements.
* **Hardware Thermal Throttling Defense:** Built-in `spin_front()` extracts invariant loads out of the loop and utilizes `_mm_pause()` / `yield` to prevent CPU ALU burnout and thermal downclocking during 100% busy-polling.

## Architectural Superiority

When compared to `rigtorp::SPSCQueue`, `zrqueue` applies "surgical" rewrites specifically aimed at the HFT Hot Path.

| Feature | `rigtorp::SPSCQueue` | `zrqueue::SpscQueue` / Models | HFT Advantage |
| :--- | :--- | :--- | :--- |
| **Addressing (Hot Path)** | `if (next == cap) next = 0;` | Bitwise `& mask_` | **Zero Branching.** Saves ~10ns periodic branch misprediction penalty (Jitter). |
| **Memory Isolation** | Padding Array Elements | Strict `alignas(64)` Structs | Complete physical isolation of Producer/Consumer atomic updates. |
| **Destruction Overhead** | Always calls `~T()` | C++17 `if constexpr` bypass | Zero CPU cycles wasted on destructing Plain Old Data (POD). |
| **Memory Page Faults** | Lazy / Demand Paging | Forced Pre-Warming (`data_{}`) | Eliminates ~5μs First-Touch Page Faults at market open. |
| **TLB Miss Defense** | Standard `malloc` (4KB) | `HugePageAllocator` (2MB/1GB) | Sustains max throughput on multi-million element queues (e.g., Order Books). |
| **Zero-Copy Write** | Push-by-value/move | `SpscRingBuffer::alloc()` | Disruptor pattern: directly write into the queue's memory slot. |

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
```

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
zrqueue::SpscQueue<Tick> queue(1024); // Dynamically rounds up to 1024
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
zrqueue::SpscQueue<Tick, zrqueue::HugePageAllocator<Tick>> history_queue(1048576);
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
```
