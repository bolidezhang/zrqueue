/*
Copyright (c) 2024 Bolide Zhang <bolidezhang@gmail.com>

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
 */

 /**
  * @file zrqueue.h
  * @brief An ultra-low latency, wait-free, single-producer single-consumer (SPSC) queue.
  *        Designed strictly for High-Frequency Trading (HFT) Hot Paths.
  */

#pragma once

#define ZRQUEUE_VERSION 10501   //1.5.1

#include <atomic>
#include <array>
#include <cassert>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

  // ============================================================================
  // [Platform and OS Low-Level API Detection]
  // ============================================================================
#if defined(_MSC_VER) || defined(__MINGW32__)
    #define ZRQUEUE_OS_WINDOWS 1
    #include <windows.h>
    #include <memoryapi.h>
    #include <malloc.h>
#elif defined(__linux__) || defined(__APPLE__)
    #define ZRQUEUE_OS_POSIX 1
    #include <sys/mman.h>
    #include <unistd.h>
    #include <fcntl.h>
#endif

#ifdef __has_cpp_attribute
    #if __has_cpp_attribute(nodiscard)
        #define ZRQUEUE_NODISCARD [[nodiscard]]
    #endif
#endif
#ifndef ZRQUEUE_NODISCARD
    #define ZRQUEUE_NODISCARD
#endif

#ifndef MAP_HUGETLB
    #define MAP_HUGETLB 0x40000
#endif
#ifndef MAP_ANONYMOUS
    #define MAP_ANONYMOUS 0x20
#endif

// ============================================================================
// [Compiler Macros]
// ============================================================================
// Get CPU L1 cache line size (Core parameter to prevent false sharing)
#ifdef __cpp_lib_hardware_interference_size
    inline constexpr size_t ZRQUEUE_CACHE_LINE_SIZE = std::hardware_destructive_interference_size;
#else
    inline constexpr size_t ZRQUEUE_CACHE_LINE_SIZE = 64;
#endif

// ============================================================================
// [Compiler Macros and Microarchitecture Attributes]
// ============================================================================
#if defined(__GNUC__) || defined(__clang__)
    // GCC and Clang (Including Clang-CL on Windows)
    #define ZRQUEUE_LIKELY(x)    __builtin_expect(!!(x), 1)
    #define ZRQUEUE_UNLIKELY(x)  __builtin_expect(!!(x), 0)
    #define ZRQUEUE_FORCE_INLINE inline __attribute__((always_inline))
#else
    #define ZRQUEUE_LIKELY(x)    (x)
    #define ZRQUEUE_UNLIKELY(x)  (x)
    #if defined(_MSC_VER)
        #define ZRQUEUE_FORCE_INLINE __forceinline
    #else
        // Fallback for unknown compilers
        #define ZRQUEUE_FORCE_INLINE inline
    #endif
#endif

// ============================================================================
// [Microarchitecture Hardware Instruction Macros (Cross-CPU & Compiler Compatible)]
// ============================================================================
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
    #if defined(_MSC_VER)
        #include <intrin.h>
    #else
        // GCC / Clang (Linux, macOS, MinGW)
        #include <immintrin.h>
    #endif
    #define ZRQUEUE_CPU_PAUSE() _mm_pause()
#elif defined(__aarch64__) || defined(_M_ARM64)
    // ARM64 Architecture (Apple Silicon, AWS Graviton, Kunpeng)
    #if defined(_MSC_VER)
        #include <intrin.h>
        #define ZRQUEUE_CPU_PAUSE() __yield()
    #else
        #define ZRQUEUE_CPU_PAUSE() asm volatile("yield" ::: "memory")
    #endif
#else
    // Fallback for unknown architectures (Degrades to no-op)
    #define ZRQUEUE_CPU_PAUSE()
#endif

namespace zrqueue {

    // ============================================================================
    // [Low-Level Memory / Math Helper Functions]
    // ============================================================================

    // Round up to the next power of 2 (Ensures branchless mask addressing)
    inline constexpr uint32_t normalize_size(uint32_t n) noexcept {
        constexpr uint32_t MIN_SIZE = 2;
        constexpr uint32_t MAX_POW2 = 1u << 30;
        if (n < MIN_SIZE) {
            n = MIN_SIZE;
        }
        n--;
        n |= n >> 1;
        n |= n >> 2;
        n |= n >> 4;
        n |= n >> 8;
        n |= n >> 16;
        n++;
        if (n == 0) {
            return MIN_SIZE;
        }
        if (n > MAX_POW2) {
            return MAX_POW2;
        }
        return n;
    }

    // Round up to the specified boundary (Boundary must be a power of 2)
    inline constexpr size_t align_up(size_t size, size_t alignment) noexcept {
        return (size + alignment - 1) & ~(alignment - 1);
    }

    // Cross-platform detection of system huge page size
    inline size_t get_hugepage_size() noexcept {
        static size_t hp_size = 0;
        if (hp_size != 0) {
            return hp_size;
        }
#if defined(ZRQUEUE_OS_WINDOWS)
        hp_size = GetLargePageMinimum();
        if (hp_size == 0) {
            hp_size = 1; // Fallback to prevent division by zero
        }
#elif defined(ZRQUEUE_OS_POSIX)
        hp_size = 2 * 1024 * 1024; // Linux default fallback to 2MB
        int fd = open("/proc/meminfo", O_RDONLY);
        if (fd != -1) {
            char buffer[1024];
            ssize_t bytes = read(fd, buffer, sizeof(buffer) - 1);
            if (bytes > 0) {
                buffer[bytes] = '\0';
                const char* match = strstr(buffer, "Hugepagesize:");
                if (match) {
                    size_t kb_size = 0;
                    if (sscanf(match, "Hugepagesize: %zu kB", &kb_size) == 1) {
                        hp_size = kb_size * 1024;
                    }
                }
            }
            close(fd);
        }
#endif
        return hp_size;
    }

    // ============================================================================
    // [Custom Memory Allocators]
    // ============================================================================

    /**
     * @brief Strictly aligned allocator: Completely eliminates false sharing at head and tail
     */
    template <typename T, size_t Alignment = ZRQUEUE_CACHE_LINE_SIZE>
    struct AlignedAllocator {
        using value_type = T;
        template <class U> struct rebind {
            using other = AlignedAllocator<U, Alignment>;
        };

        AlignedAllocator() noexcept = default;
        template <class U> constexpr AlignedAllocator(const AlignedAllocator<U, Alignment>&) noexcept {
        }

        T* allocate(std::size_t n) {
            if (n == 0) {
                return nullptr;
            }
            size_t raw_size = n * sizeof(T);
            // [Core Defense]: Round up allocation size to a multiple of cache line size,
            // preventing OS from placing unrelated variables at the tail
            size_t aligned_size = align_up(raw_size, Alignment);
            void *ptr = nullptr;

#if defined(ZRQUEUE_OS_WINDOWS)
            ptr = _aligned_malloc(aligned_size, Alignment);
#elif __cplusplus >= 201703L && !defined(__APPLE__)
            ptr = std::aligned_alloc(Alignment, aligned_size);
#elif defined(ZRQUEUE_OS_POSIX)
            if (posix_memalign(&ptr, Alignment, aligned_size) != 0) {
                ptr = nullptr;
            }
#endif
            if (!ptr) {
                throw std::bad_alloc();
            }
            return static_cast<T*>(ptr);
        }

        void deallocate(T *p, std::size_t) noexcept {
            if (!p) {
                return;
            }
#if defined(ZRQUEUE_OS_WINDOWS)
            _aligned_free(p);
#else
            std::free(p);
#endif
        }
    };

    /**
     * @brief Huge page allocator: Completely eliminates TLB Misses in massive queue scenarios
     */
    template <typename T>
    struct HugePageAllocator {
        using value_type = T;
        template <class U> struct rebind {
            using other = HugePageAllocator<U>;
        };

        HugePageAllocator() noexcept = default;
        template <class U> constexpr HugePageAllocator(const HugePageAllocator<U>&) noexcept {
        }

        T* allocate(std::size_t n) {
            if (n == 0) {
                return nullptr;
            }
            size_t raw_size = n * sizeof(T);
            void  *ptr = nullptr;
            size_t hp_size = get_hugepage_size();

#if defined(ZRQUEUE_OS_POSIX)
            // [Core Defense]: Force alignment to huge page size to prevent kernel mmap out-of-bounds or allocation failure
            size_t aligned_size = align_up(raw_size, hp_size);
            ptr = mmap(nullptr, aligned_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
            if (ptr == MAP_FAILED) {
                // Huge page failed, downgrade to normal mmap, but still use the same alignment
                ptr = mmap(nullptr, aligned_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
                if (ptr == MAP_FAILED) {
                    throw std::bad_alloc();
                }
            }
#elif defined(ZRQUEUE_OS_WINDOWS)
            if (hp_size > 1) {
                size_t aligned_size = align_up(raw_size, hp_size);
                ptr = VirtualAlloc(NULL, aligned_size, MEM_COMMIT | MEM_RESERVE | MEM_LARGE_PAGES, PAGE_READWRITE);
            }
            if (!ptr) {
                // Graceful degradation: Fallback to normal 4KB memory pages
                SYSTEM_INFO sys_info;
                GetSystemInfo(&sys_info);
                ptr = VirtualAlloc(NULL, align_up(raw_size, sys_info.dwPageSize), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
                if (!ptr) {
                    throw std::bad_alloc();
                }
            }
#endif
            return static_cast<T*>(ptr);
        }

        void deallocate(T *p, std::size_t n) noexcept {
            if (!p) {
                return;
            }

#if defined(ZRQUEUE_OS_POSIX)
            size_t raw_size = n * sizeof(T);
            // Deallocation must strictly use the same aligned size as allocation to prevent memory leaks
            size_t aligned_size = align_up(raw_size, get_hugepage_size());
            munmap(p, aligned_size);
#elif defined(ZRQUEUE_OS_WINDOWS)
            VirtualFree(p, 0, MEM_RELEASE);
#endif
        }
    };

    // Allocator comparisons
    template <class T, class U, size_t A> bool operator==(const AlignedAllocator<T, A>&, const AlignedAllocator<U, A>&) {
        return true;
    }
    template <class T, class U, size_t A> bool operator!=(const AlignedAllocator<T, A>&, const AlignedAllocator<U, A>&) {
        return false;
    }
    template <class T, class U> bool operator==(const HugePageAllocator<T>&, const HugePageAllocator<U>&) {
        return true;
    }
    template <class T, class U> bool operator!=(const HugePageAllocator<T>&, const HugePageAllocator<U>&) {
        return false;
    }

    // ============================================================================
    // [Ultra-Fast Lock-Free SpscQueue]
    // ============================================================================
    template <typename T, typename Allocator = AlignedAllocator<T, ZRQUEUE_CACHE_LINE_SIZE>>
    class SpscQueue {
    public:
        explicit SpscQueue(const uint32_t capacity, const Allocator& allocator = Allocator())
            : capacity_(normalize_size(capacity)), allocator_(allocator) {
            mask_ = capacity_ - 1;
            // Allocate physically contiguous memory. Custom Allocator perfectly resolves head/tail false sharing, no manual padding needed.
            slots_ = std::allocator_traits<Allocator>::allocate(allocator_, capacity_);
        }

        ~SpscQueue() {
            while (front()) {
                pop();
            }
            std::allocator_traits<Allocator>::deallocate(allocator_, slots_, capacity_);
        }

        // Disable copy and move
        SpscQueue(const SpscQueue&) = delete;
        SpscQueue& operator=(const SpscQueue&) = delete;
        SpscQueue(SpscQueue&&) = delete;
        SpscQueue& operator=(SpscQueue&&) = delete;

        template <typename... Args>
        ZRQUEUE_FORCE_INLINE void emplace(Args&&...args) noexcept(std::is_nothrow_constructible_v<T, Args&&...>) {
            static_assert(std::is_constructible_v<T, Args&&...>, "T must be constructible with Args&&...");
            const uint64_t write_index = write_index_.load(std::memory_order_relaxed);
            const uint64_t next_write_index = write_index + 1;

            // [Mechanism]: Fetch latest consumer index across cores only when local cache indicates full (Cache Ping-Pong Defense)
            while (ZRQUEUE_UNLIKELY(next_write_index - cached_read_index_ > capacity_)) {
                cached_read_index_ = read_index_.load(std::memory_order_acquire);
            }

            // [Mechanism]: Pure bitwise addressing + Placement new (Zero-Branching & Zero-Initialization Penalty)
            new (&slots_[write_index & mask_]) T(std::forward<Args>(args)...);
            write_index_.store(next_write_index, std::memory_order_release);
        }

        template <typename... Args>
        ZRQUEUE_NODISCARD ZRQUEUE_FORCE_INLINE bool try_emplace(Args&&...args) noexcept(std::is_nothrow_constructible_v<T, Args&&...>) {
            static_assert(std::is_constructible_v<T, Args&&...>, "T must be constructible with Args&&...");
            const uint64_t write_index = write_index_.load(std::memory_order_relaxed);
            const uint64_t next_write_index = write_index + 1;

            if (ZRQUEUE_UNLIKELY(next_write_index - cached_read_index_ > capacity_)) {
                cached_read_index_ = read_index_.load(std::memory_order_acquire);
                if (ZRQUEUE_UNLIKELY(next_write_index - cached_read_index_ > capacity_)) {
                    return false;
                }
            }

            new (&slots_[write_index & mask_]) T(std::forward<Args>(args)...);
            write_index_.store(next_write_index, std::memory_order_release);
            return true;
        }

        ZRQUEUE_FORCE_INLINE void push(const T& v) noexcept(std::is_nothrow_copy_constructible_v<T>) {
            emplace(v);
        }

        template <typename P, typename = std::enable_if_t<std::is_constructible_v<T, P&&>>>
        ZRQUEUE_FORCE_INLINE void push(P&& v) noexcept(std::is_nothrow_constructible_v<T, P&&>) {
            emplace(std::forward<P>(v));
        }

        ZRQUEUE_NODISCARD ZRQUEUE_FORCE_INLINE bool try_push(const T& v) noexcept(std::is_nothrow_copy_constructible_v<T>) {
            return try_emplace(v);
        }

        template <typename P, typename = std::enable_if_t<std::is_constructible_v<T, P&&>>>
        ZRQUEUE_NODISCARD ZRQUEUE_FORCE_INLINE bool try_push(P&& v) noexcept(std::is_nothrow_constructible_v<T, P&&>) {
            return try_emplace(std::forward<P>(v));
        }

        // Get data pointer for in-place processing, achieving absolute zero-copy
        ZRQUEUE_NODISCARD ZRQUEUE_FORCE_INLINE T* front() noexcept {
            const uint64_t read_index = read_index_.load(std::memory_order_relaxed);
            if (ZRQUEUE_UNLIKELY(read_index == cached_write_index_)) {
                cached_write_index_ = write_index_.load(std::memory_order_acquire);
                if (ZRQUEUE_UNLIKELY(read_index == cached_write_index_)) {
                    return nullptr;
                }
            }

            return &slots_[read_index & mask_];
        }

        // ------------------------------------------------------------------------
        // Ultra-Fast Hot Spin Read (Spin Front)
        // Scenario: Strategy core main loop, busy-waiting for market data.
        // Advantage: Inserts _mm_pause in 100% idle busy loops to protect CPU from downclocking.
        // ------------------------------------------------------------------------
        ZRQUEUE_NODISCARD T* spin_front() noexcept {
            const uint64_t read_index = read_index_.load(std::memory_order_relaxed);
            if (ZRQUEUE_LIKELY(read_index != cached_write_index_)) {
                return &slots_[read_index & mask_];
            }

            // Step 2: Not found, enter cross-core fetch loop mode
            while (true) {
                cached_write_index_ = write_index_.load(std::memory_order_acquire);
                if (ZRQUEUE_LIKELY(read_index != cached_write_index_)) {
                    return &slots_[read_index & mask_];
                }

                ZRQUEUE_CPU_PAUSE();
            }
        }

        // Pop after data processing, manually destruct lifecycle
        ZRQUEUE_FORCE_INLINE void pop() noexcept {
            static_assert(std::is_nothrow_destructible_v<T>, "T must be nothrow destructible");
            const uint64_t read_index = read_index_.load(std::memory_order_relaxed);
            const uint64_t next_read_index = read_index + 1;

            if constexpr (!std::is_trivially_destructible_v<T>) {
                slots_[read_index & mask_].~T();
            }
            read_index_.store(next_read_index, std::memory_order_release);
        }

        // Allow peeking at elements 'offset' positions ahead of read cursor without moving it
        ZRQUEUE_NODISCARD ZRQUEUE_FORCE_INLINE T* peek(size_t offset = 0) noexcept {
            const uint64_t read_index = read_index_.load(std::memory_order_relaxed);
            const uint64_t new_read_index = read_index + offset;

            // Ensure adding offset does not cross the producer's write position
            if (ZRQUEUE_UNLIKELY(new_read_index >= cached_write_index_)) {
                cached_write_index_ = write_index_.load(std::memory_order_acquire);
                if (ZRQUEUE_UNLIKELY(new_read_index >= cached_write_index_)) {
                    return nullptr;
                }
            }

            return &slots_[new_read_index & mask_];
        }

        // ------------------------------------------------------------------------
        // Bulk Push
        // Scenario: Gateway receives 10 market messages via UDP recvmmsg at once, pushing all instantly.
        // Advantage: Regardless of element count, executes ONLY ONE atomic Release barrier!
        // ------------------------------------------------------------------------
        template <typename Iterator>
        ZRQUEUE_NODISCARD size_t push_bulk(Iterator first, size_t count) noexcept {
            if (ZRQUEUE_UNLIKELY(count == 0)) {
                return 0;
            }
            const uint64_t write_index = write_index_.load(std::memory_order_relaxed);
            uint64_t next_write_index = write_index + count;

            // 1. Check if there is enough space for 'count' elements
            if (ZRQUEUE_UNLIKELY(next_write_index - cached_read_index_ > capacity_)) {
                cached_read_index_ = read_index_.load(std::memory_order_acquire);
                // Real space still insufficient, calculate max writable count (Partial Push)
                if (ZRQUEUE_UNLIKELY(next_write_index - cached_read_index_ > capacity_)) {
                    count = capacity_ - (write_index - cached_read_index_);
                    if (ZRQUEUE_UNLIKELY(count == 0)) {
                        return 0; // Queue is totally full
                    }
                    next_write_index = write_index + count;
                }
            }

            // 2. Sequential placement new (Pure memory operations without atomic locks)
            for (size_t i = 0; i < count; ++i) {
                new (&slots_[(write_index + i) & mask_]) T(*first++);
            }

            // 3. Bulk push complete, only 1 atomic commit required!
            write_index_.store(next_write_index, std::memory_order_release);
            return count; // Return actual written count
        }

        // ------------------------------------------------------------------------
        // Bulk Zero-Copy Consume (Bulk Consume)
        // Scenario: Strategy thread wakes up, finds backlog, processes all at once.
        // Advantage: Pass Lambda for in-place Zero-Copy processing, executes ONLY ONE Release barrier at the end!
        // max_count defaults to 0, meaning "consume as many as available".
        // ------------------------------------------------------------------------
        template <typename THandler>
        ZRQUEUE_NODISCARD size_t consume_bulk(THandler&& handler, size_t max_count = 0) noexcept {
            const uint64_t read_index = read_index_.load(std::memory_order_relaxed);

            // 1. Get current number of backlogged elements
            if (ZRQUEUE_UNLIKELY(read_index == cached_write_index_)) {
                cached_write_index_ = write_index_.load(std::memory_order_acquire);
                if (ZRQUEUE_UNLIKELY(read_index == cached_write_index_)) {
                    return 0; // Queue is empty
                }
            }

            size_t available = cached_write_index_ - read_index;
            size_t count_to_consume = (max_count == 0 || max_count > available) ? available : max_count;
            const uint64_t next_read_index = read_index + count_to_consume;

            // 2. Sequentially process data and destruct in-place
            for (size_t i = 0; i < count_to_consume; ++i) {
                size_t current_offset = (read_index + i) & mask_;
                // Pass data reference directly to user's lambda for processing (Zero data copy)
                std::forward<THandler>(handler)(slots_[current_offset]);

                if constexpr (!std::is_trivially_destructible_v<T>) {
                    // Manually destruct after processing
                    slots_[current_offset].~T();
                }
            }

            // 3. Bulk process complete, only 1 atomic release required!
            read_index_.store(next_read_index, std::memory_order_release);
            return count_to_consume; // Return actual consumed count
        }

        ZRQUEUE_NODISCARD ZRQUEUE_FORCE_INLINE size_t size() const noexcept {
            uint64_t w = write_index_.load(std::memory_order_acquire);
            uint64_t r = read_index_.load(std::memory_order_acquire);
            return (w >= r) ? static_cast<size_t>(w - r) : 0;
        }

        ZRQUEUE_NODISCARD ZRQUEUE_FORCE_INLINE bool empty() const noexcept {
            return write_index_.load(std::memory_order_acquire) == read_index_.load(std::memory_order_acquire);
        }

        ZRQUEUE_NODISCARD ZRQUEUE_FORCE_INLINE size_t capacity() const noexcept {
            return capacity_;
        }

    private:
        // ========================================================================
        // [Physical Memory Layout]: Textbook False Sharing Isolation (Cache Coherence Ping-Pong Defense)
        // ========================================================================

        // [Cache Line]: Producer Zone. Written heavily by writer, read occasionally by reader.
        alignas(ZRQUEUE_CACHE_LINE_SIZE) std::atomic<uint64_t> write_index_{ 0 };
        alignas(ZRQUEUE_CACHE_LINE_SIZE) uint64_t cached_read_index_ { 0 };

        // [Cache Line]: Consumer Zone. Written heavily by reader, read occasionally by writer.
        alignas(ZRQUEUE_CACHE_LINE_SIZE) std::atomic<uint64_t> read_index_{ 0 };
        alignas(ZRQUEUE_CACHE_LINE_SIZE) uint64_t cached_write_index_ { 0 };

        // [Cache Line]: Cold Data Zone. Strictly immutable metadata during runtime, far from hot pointers.
        alignas(ZRQUEUE_CACHE_LINE_SIZE) uint32_t capacity_ { 0 };
        uint32_t mask_{ 0 };
        T *slots_{ nullptr };

        // [Cache Line]: Allocator occupies independent space
        alignas(ZRQUEUE_CACHE_LINE_SIZE) Allocator allocator_ { };
    };  // end SpscQueue

    /**
     * @brief Extreme in-place SPSC Queue
     * Warning: Since data is embedded within the object, if N is very large, the object size will be massive.
     * NEVER allocate this on the thread stack! Must be declared as a global variable (static) or allocated on the heap via new.
     * @tparam T Element type
     * @tparam N Queue capacity ([MUST] be a power of 2)
     */
    template <typename T, uint32_t N>
    class SpscInlineQueue {
        // [Compile-Time Defense]: Forcibly require N to be a power of 2, eliminating runtime bitwise risks at the root
        static_assert(N >= 2, "Capacity must be at least 2");
        static_assert((N & (N - 1)) == 0, "Capacity N must be a power of 2 for bitwise masking");
    public:
        SpscInlineQueue() noexcept = default;
        ~SpscInlineQueue() {
            while (front()) {
                pop();
            }
        }

        // non-copyable and non-movable
        SpscInlineQueue(const SpscInlineQueue&) = delete;
        SpscInlineQueue& operator=(const SpscInlineQueue&) = delete;
        SpscInlineQueue(SpscInlineQueue&&) = delete;
        SpscInlineQueue& operator=(SpscInlineQueue&&) = delete;

        template <typename... Args>
        ZRQUEUE_FORCE_INLINE void emplace(Args&&... args) noexcept(std::is_nothrow_constructible_v<T, Args&&...>) {
            static_assert(std::is_constructible_v<T, Args&&...>, "T must be constructible with Args&&...");
            const uint64_t write_index = write_index_.load(std::memory_order_relaxed);
            const uint64_t next_write_index = write_index + 1;

            while (ZRQUEUE_UNLIKELY(next_write_index - cached_read_index_ > N)) {
                cached_read_index_ = read_index_.load(std::memory_order_acquire);
            }

            // [Extreme Base Addressing]: No longer dereferencing slots_ pointer,
            // directly calculate target physical address based on object's 'this' pointer + offset!
            new (&reinterpret_cast<T*>(slots_memory_)[write_index & MASK]) T(std::forward<Args>(args)...);
            write_index_.store(next_write_index, std::memory_order_release);
        }

        template <typename... Args>
        ZRQUEUE_NODISCARD ZRQUEUE_FORCE_INLINE bool try_emplace(Args&&... args) noexcept(std::is_nothrow_constructible_v<T, Args&&...>) {
            static_assert(std::is_constructible_v<T, Args&&...>, "T must be constructible with Args&&...");
            const uint64_t write_index = write_index_.load(std::memory_order_relaxed);
            const uint64_t next_write_index = write_index + 1;

            if (ZRQUEUE_UNLIKELY(next_write_index - cached_read_index_ > N)) {
                cached_read_index_ = read_index_.load(std::memory_order_acquire);
                if (ZRQUEUE_UNLIKELY(next_write_index - cached_read_index_ > N)) {
                    return false;
                }
            }

            new (&reinterpret_cast<T*>(slots_memory_)[write_index & MASK]) T(std::forward<Args>(args)...);
            write_index_.store(next_write_index, std::memory_order_release);
            return true;
        }

        ZRQUEUE_FORCE_INLINE void push(const T& v) noexcept(std::is_nothrow_copy_constructible_v<T>) {
            emplace(v);
        }

        template <typename P, typename = std::enable_if_t<std::is_constructible_v<T, P&&>>>
        ZRQUEUE_FORCE_INLINE void push(P&& v) noexcept(std::is_nothrow_constructible_v<T, P&&>) {
            emplace(std::forward<P>(v));
        }

        ZRQUEUE_NODISCARD ZRQUEUE_FORCE_INLINE bool try_push(const T& v) noexcept(std::is_nothrow_copy_constructible_v<T>) {
            return try_emplace(v);
        }

        template <typename P, typename = std::enable_if_t<std::is_constructible_v<T, P&&>>>
        ZRQUEUE_NODISCARD ZRQUEUE_FORCE_INLINE bool try_push(P&& v) noexcept(std::is_nothrow_constructible_v<T, P&&>) {
            return try_emplace(std::forward<P>(v));
        }

        template <typename Iterator>
        ZRQUEUE_NODISCARD size_t push_bulk(Iterator first, size_t count) noexcept {
            if (ZRQUEUE_UNLIKELY(count == 0)) {
                return 0;
            }

            const uint64_t write_index = write_index_.load(std::memory_order_relaxed);
            uint64_t next_write_index = write_index + count;

            if (ZRQUEUE_UNLIKELY(next_write_index - cached_read_index_ > N)) {
                cached_read_index_ = read_index_.load(std::memory_order_acquire);
                if (ZRQUEUE_UNLIKELY(next_write_index - cached_read_index_ > N)) {
                    count = N - (write_index - cached_read_index_);
                    if (ZRQUEUE_UNLIKELY(count == 0)) {
                        return 0;
                    }
                    next_write_index = write_index + count;
                }
            }

            T *raw_array = reinterpret_cast<T*>(slots_memory_);
            for (size_t i = 0; i < count; ++i) {
                new (&raw_array[(write_index + i) & MASK]) T(*first++);
            }
            write_index_.store(next_write_index, std::memory_order_release);
            return count;
        }

        ZRQUEUE_NODISCARD ZRQUEUE_FORCE_INLINE T* front() noexcept {
            const uint64_t read_index = read_index_.load(std::memory_order_relaxed);
            if (ZRQUEUE_UNLIKELY(read_index == cached_write_index_)) {
                cached_write_index_ = write_index_.load(std::memory_order_acquire);
                if (ZRQUEUE_UNLIKELY(read_index == cached_write_index_)) {
                    return nullptr;
                }
            }

            return &reinterpret_cast<T*>(slots_memory_)[read_index & MASK];
        }

        ZRQUEUE_NODISCARD T* spin_front() noexcept {
            const uint64_t read_index = read_index_.load(std::memory_order_relaxed);
            if (ZRQUEUE_LIKELY(read_index != cached_write_index_)) {
                return &reinterpret_cast<T*>(slots_memory_)[read_index & MASK];
            }

            while (true) {
                cached_write_index_ = write_index_.load(std::memory_order_acquire);
                if (ZRQUEUE_LIKELY(read_index != cached_write_index_)) {
                    return &reinterpret_cast<T*>(slots_memory_)[read_index & MASK];
                }
                ZRQUEUE_CPU_PAUSE();
            }
        }

        ZRQUEUE_FORCE_INLINE void pop() noexcept {
            static_assert(std::is_nothrow_destructible_v<T>, "T must be nothrow destructible");
            const uint64_t read_index = read_index_.load(std::memory_order_relaxed);
            const uint64_t next_read_index = read_index + 1;
            if constexpr (!std::is_trivially_destructible_v<T>) {
                reinterpret_cast<T*>(slots_memory_)[read_index & MASK].~T();
            }
            read_index_.store(next_read_index, std::memory_order_release);
        }

        ZRQUEUE_NODISCARD ZRQUEUE_FORCE_INLINE T* peek(size_t offset = 0) noexcept {
            const uint64_t read_index = read_index_.load(std::memory_order_relaxed);
            const uint64_t new_read_index = read_index + offset;
            if (ZRQUEUE_UNLIKELY(new_read_index >= cached_write_index_)) {
                cached_write_index_ = write_index_.load(std::memory_order_acquire);
                if (ZRQUEUE_UNLIKELY(new_read_index >= cached_write_index_)) {
                    return nullptr;
                }
            }

            return &reinterpret_cast<T*>(slots_memory_)[new_read_index & MASK];
        }

        template <typename THandler>
        ZRQUEUE_NODISCARD size_t consume_bulk(THandler&& handler, size_t max_count = 0) noexcept {
            const uint64_t read_index = read_index_.load(std::memory_order_relaxed);

            if (ZRQUEUE_UNLIKELY(read_index == cached_write_index_)) {
                cached_write_index_ = write_index_.load(std::memory_order_acquire);
                if (ZRQUEUE_UNLIKELY(read_index == cached_write_index_)) {
                    return 0;
                }
            }

            size_t available = cached_write_index_ - read_index;
            size_t count_to_consume = (max_count == 0 || max_count > available) ? available : max_count;
            const uint64_t next_read_index = read_index + count_to_consume;

            T* raw_array = reinterpret_cast<T*>(slots_memory_);
            for (size_t i = 0; i < count_to_consume; ++i) {
                size_t current_offset = (read_index + i) & MASK;
                std::forward<THandler>(handler)(raw_array[current_offset]);

                if constexpr (!std::is_trivially_destructible_v<T>) {
                    raw_array[current_offset].~T();
                }
            }

            read_index_.store(next_read_index, std::memory_order_release);
            return count_to_consume;
        }

        ZRQUEUE_NODISCARD ZRQUEUE_FORCE_INLINE size_t size() const noexcept {
            uint64_t w = write_index_.load(std::memory_order_acquire);
            uint64_t r = read_index_.load(std::memory_order_acquire);
            return (w >= r) ? static_cast<size_t>(w - r) : 0;
        }

        ZRQUEUE_NODISCARD ZRQUEUE_FORCE_INLINE bool empty() const noexcept {
            return write_index_.load(std::memory_order_acquire) == read_index_.load(std::memory_order_acquire);
        }

        ZRQUEUE_NODISCARD ZRQUEUE_FORCE_INLINE constexpr size_t capacity() const noexcept {
            return N;
        }

    private:
        static constexpr uint32_t MASK = N - 1;

        // [Cache Line]: Producer Zone
        alignas(ZRQUEUE_CACHE_LINE_SIZE) std::atomic<uint64_t> write_index_{ 0 };
        alignas(ZRQUEUE_CACHE_LINE_SIZE) uint64_t cached_read_index_ { 0 };

        // [Cache Line]: Consumer Zone
        alignas(ZRQUEUE_CACHE_LINE_SIZE) std::atomic<uint64_t> read_index_{ 0 };
        alignas(ZRQUEUE_CACHE_LINE_SIZE) uint64_t cached_write_index_ { 0 };

        // [Cache Line]: Pure Embedded Data Zone (In-place Memory Array)
        // 1. alignas(ZRQUEUE_CACHE_LINE_SIZE) forces a complete physical cut between data zone's start address and the index zone above.
        // 2. alignas(T) ensures reinterpret_cast<T*> is alignment-safe.
        // 3. std::byte ensures this is "pure raw memory" invoking no constructors, constructed only upon push.
        alignas(ZRQUEUE_CACHE_LINE_SIZE) alignas(T) std::byte slots_memory_[N * sizeof(T)];
    }; // end SpscInlineQueue

    // ============================================================================
    // [Zero-Copy Pre-allocated Memory Lock-Free Queue SpscRingBuffer (Disruptor Style)]
    // ============================================================================
    template<class T, uint32_t N>
    class SpscRingBuffer {
        static_assert(N >= 2, "Capacity must be at least 2");
        static_assert((N & (N - 1)) == 0, "N must be a power of 2");
    public:
        SpscRingBuffer() noexcept = default;
        ~SpscRingBuffer() noexcept = default;

        // non-copyable and non-movable
        SpscRingBuffer(const SpscRingBuffer&) = delete;
        SpscRingBuffer& operator=(const SpscRingBuffer&) = delete;
        SpscRingBuffer(SpscRingBuffer&&) = delete;
        SpscRingBuffer& operator=(SpscRingBuffer&&) = delete;

        ZRQUEUE_NODISCARD ZRQUEUE_FORCE_INLINE T* alloc() {
            const uint64_t write_index = write_index_.load(std::memory_order_relaxed);
            const uint64_t next_write_index = write_index + 1;

            // Interceptor: When local cache considers queue full (> N), enter slow path
            if (ZRQUEUE_UNLIKELY(next_write_index - cached_read_index_ > N)) {
                cached_read_index_ = read_index_.load(std::memory_order_acquire);
                if (ZRQUEUE_UNLIKELY(next_write_index - cached_read_index_ > N)) {
                    return nullptr;
                }
            }

            return &data_[write_index & MASK];
        }

        ZRQUEUE_FORCE_INLINE void push() {
            const uint64_t write_index = write_index_.load(std::memory_order_relaxed);
            const uint64_t next_write_index = write_index + 1;
            write_index_.store(next_write_index, std::memory_order_release);
        }

        template<typename Writer>
        ZRQUEUE_NODISCARD ZRQUEUE_FORCE_INLINE bool try_push(Writer&& writer) {
            T* v = alloc();
            if (ZRQUEUE_LIKELY(v != nullptr)) {
                std::forward<Writer>(writer)(v);
                push();
                return true;
            }
            return false;
        }

        template<typename Writer>
        void push(Writer&& writer) {
            while (!try_push(std::forward<Writer>(writer))) {
                ZRQUEUE_CPU_PAUSE();
            }
        }

        ZRQUEUE_NODISCARD ZRQUEUE_FORCE_INLINE T* front() {
            const uint64_t read_index = read_index_.load(std::memory_order_relaxed);
            if (ZRQUEUE_UNLIKELY(read_index == cached_write_index_)) {
                cached_write_index_ = write_index_.load(std::memory_order_acquire);
                if (ZRQUEUE_UNLIKELY(read_index == cached_write_index_)) {
                    return nullptr;
                }
            }

            return &data_[read_index & MASK];
        }

        ZRQUEUE_NODISCARD T* spin_front() noexcept {
            const uint64_t read_index = read_index_.load(std::memory_order_relaxed);
            if (ZRQUEUE_LIKELY(read_index != cached_write_index_)) {
                return &data_[read_index & MASK];
            }

            while (true) {
                cached_write_index_ = write_index_.load(std::memory_order_acquire);
                if (ZRQUEUE_LIKELY(read_index != cached_write_index_)) {
                    return &data_[read_index & MASK];
                }
                ZRQUEUE_CPU_PAUSE();
            }
        }

        ZRQUEUE_FORCE_INLINE void pop() {
            const uint64_t read_index = read_index_.load(std::memory_order_relaxed);
            const uint64_t next_read_index = read_index + 1;
            read_index_.store(next_read_index, std::memory_order_release);
        }

        ZRQUEUE_NODISCARD ZRQUEUE_FORCE_INLINE T* peek(size_t offset = 0) noexcept {
            const uint64_t read_index = read_index_.load(std::memory_order_relaxed);
            const uint64_t new_read_index = read_index + offset;

            if (ZRQUEUE_UNLIKELY(new_read_index >= cached_write_index_)) {
                cached_write_index_ = write_index_.load(std::memory_order_acquire);
                if (ZRQUEUE_UNLIKELY(new_read_index >= cached_write_index_)) {
                    return nullptr;
                }
            }

            return &data_[new_read_index & MASK];
        }

        template<typename Reader>
        ZRQUEUE_NODISCARD ZRQUEUE_FORCE_INLINE bool try_pop(Reader&& reader) {
            T* v = front();
            if (ZRQUEUE_LIKELY(v)) {
                std::forward<Reader>(reader)(v);
                pop();
                return true;
            }
            return false;
        }

        ZRQUEUE_NODISCARD ZRQUEUE_FORCE_INLINE size_t size() const noexcept {
            uint64_t w = write_index_.load(std::memory_order_acquire);
            uint64_t r = read_index_.load(std::memory_order_acquire);
            return (w >= r) ? static_cast<size_t>(w - r) : 0;
        }

        ZRQUEUE_NODISCARD ZRQUEUE_FORCE_INLINE bool empty() const noexcept {
            return write_index_.load(std::memory_order_acquire) == read_index_.load(std::memory_order_acquire);
        }

        ZRQUEUE_NODISCARD ZRQUEUE_FORCE_INLINE constexpr size_t capacity() const noexcept {
            return N;
        }

    private:
        static constexpr uint32_t MASK = N - 1;

        // [Cache Line]: Producer Zone
        alignas(ZRQUEUE_CACHE_LINE_SIZE) std::atomic<uint64_t> write_index_{ 0 };
        alignas(ZRQUEUE_CACHE_LINE_SIZE) uint64_t cached_read_index_ { 0 };

        // [Cache Line]: Consumer Zone
        alignas(ZRQUEUE_CACHE_LINE_SIZE) std::atomic<uint64_t> read_index_{ 0 };
        alignas(ZRQUEUE_CACHE_LINE_SIZE) uint64_t cached_write_index_ { 0 };

        // Data Zone
        alignas(ZRQUEUE_CACHE_LINE_SIZE) std::array<T, N> data_{ };
    };  // end SpscRingBuffer

}   // end namespace zrqueue