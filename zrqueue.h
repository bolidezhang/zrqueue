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

// ============================================================================
// [Language Standard Gate: C++17 or later is mandatory]
// The library relies on if constexpr, std::byte, inline variables,
// std::hardware_destructive_interference_size (when available) and
// std::is_nothrow_invocable. MSVC reports __cplusplus as 199711L unless
// /Zc:__cplusplus is passed, so use _MSVC_LANG there.
// ============================================================================
#if defined(_MSVC_LANG)
    #define ZRQUEUE_CPLUSPLUS _MSVC_LANG
#else
    #define ZRQUEUE_CPLUSPLUS __cplusplus
#endif

#if ZRQUEUE_CPLUSPLUS < 201703L
    #error "zrqueue requires C++17 or later (use /std:c++17 or -std=c++17)"
#endif

#if !defined(__cpp_if_constexpr)
    #error "zrqueue requires if constexpr (__cpp_if_constexpr missing)"
#endif

#define ZRQUEUE_VERSION 11000   //1.10.0

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
    // This is a public single-header library, so the Windows headers must not leak
    // into the including translation unit: WIN32_LEAN_AND_MEAN drops the rarely
    // used subsystems, and NOMINMAX keeps the min/max function-like macros from
    // breaking std::min/std::max in user code. Both stay defined afterwards, which
    // is the safer default for later <windows.h> includes in the same TU.
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
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
// [Branch Hints]
// There is no branch-hint facility in C++17: [[likely]]/[[unlikely]] arrived in
// C++20 and are STATEMENT attributes, so they cannot be embedded in a
// boolean-expression macro like the ones below (all ~70 call sites are
// `if (ZRQUEUE_UNLIKELY(expr))` / `while (...)`). __builtin_expect is the only
// usable expression form and is accepted by GCC and Clang in every mode.
// MSVC 19.5x has neither (verified: `error C3861: __builtin_expect`), so it
// degrades to the plain expression - correct, just unhinted.
// ============================================================================
#if defined(__GNUC__) || defined(__clang__)
    #define ZRQUEUE_LIKELY(x)    __builtin_expect(!!(x), 1)
    #define ZRQUEUE_UNLIKELY(x)  __builtin_expect(!!(x), 0)
#else
    #define ZRQUEUE_LIKELY(x)    (x)
    #define ZRQUEUE_UNLIKELY(x)  (x)
#endif

#if defined(__GNUC__) || defined(__clang__)
    // GCC and Clang (Including Clang-CL on Windows)
    #define ZRQUEUE_FORCE_INLINE inline __attribute__((always_inline))
#else
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

    // Round up to the next power of 2 (Ensures branchless mask addressing).
    // Note: requests above 2^30 elements are silently clamped to 2^30.
    inline constexpr uint32_t normalize_size(uint32_t n) noexcept {
        constexpr uint32_t MIN_SIZE = 2;
        constexpr uint32_t MAX_SIZE = 1u << 30;

        if (n <= MIN_SIZE) {
            return MIN_SIZE;
        }

        if (n >= MAX_SIZE) {
            return MAX_SIZE;
        }

        --n;

        n |= n >> 1;
        n |= n >> 2;
        n |= n >> 4;
        n |= n >> 8;
        n |= n >> 16;

        return n + 1;
    }

    // Round up to the specified boundary (Boundary must be a power of 2)
    inline constexpr size_t align_up(size_t size, size_t alignment) noexcept {
        return (size + alignment - 1) & ~(alignment - 1);
    }

    // Cross-platform detection of system huge page size (one-shot probe)
    inline size_t detect_hugepage_size() noexcept {
#if defined(ZRQUEUE_OS_WINDOWS)
        size_t hp_size = GetLargePageMinimum();
        return hp_size != 0 ? hp_size : 1; // Fallback to prevent division by zero
#elif defined(ZRQUEUE_OS_POSIX)
        size_t hp_size = 2 * 1024 * 1024; // Linux default fallback to 2MB
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
        return hp_size;
#else
        return 1;
#endif
    }

    // Cross-platform detection of system huge page size.
    // Cached via magic static: initialization is thread-safe by the language, no data race.
    inline size_t get_hugepage_size() noexcept {
        static const size_t hp_size = detect_hugepage_size();
        return hp_size;
    }

    namespace detail {
        // Set when HugePageAllocator cannot obtain huge pages and silently downgrades
        // to normal pages. Written once on the cold allocation path, read never on hot paths.
        inline std::atomic<bool> g_hugepage_fallback { false };
    } // namespace detail

    // Returns true if any HugePageAllocator allocation fell back to normal (4KB) pages.
    // The fallback is graceful but was silent before 1.7.0; check once at startup to
    // confirm huge pages are actually in effect (e.g. hugetlbfs reserved / SeLockMemoryPrivilege held).
    inline bool hugepage_fallback_occurred() noexcept {
        return detail::g_hugepage_fallback.load(std::memory_order_relaxed);
    }

    // ============================================================================
    // [Custom Memory Allocators]
    // ============================================================================

    /**
     * @brief Strictly aligned allocator: Completely eliminates false sharing at head and tail
     * Default alignment covers over-aligned element types (e.g. alignas(128) messages):
     * a fixed 64-byte alignment would under-align alignof(T) > 64 types.
     */
    template <typename T, size_t Alignment = (alignof(T) > ZRQUEUE_CACHE_LINE_SIZE
                                                 ? alignof(T) : ZRQUEUE_CACHE_LINE_SIZE)>
    struct AlignedAllocator {
        static_assert(Alignment >= alignof(T),
                      "AlignedAllocator: Alignment must be at least alignof(T)");
        static_assert((Alignment & (Alignment - 1)) == 0,
                      "AlignedAllocator: Alignment must be a power of 2");
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
                detail::g_hugepage_fallback.store(true, std::memory_order_relaxed);
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
                detail::g_hugepage_fallback.store(true, std::memory_order_relaxed);
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
    // [Ultra-Fast Lock-Free SpscHeapQueue]
    // ============================================================================
    template <typename T, typename Allocator = AlignedAllocator<T>>
    class SpscHeapQueue {
    public:
        explicit SpscHeapQueue(const uint32_t capacity, const Allocator& allocator = Allocator())
            : capacity_(normalize_size(capacity)), allocator_(allocator) {
            mask_ = capacity_ - 1;
            // Allocate physically contiguous memory. Custom Allocator perfectly resolves head/tail false sharing, no manual padding needed.
            slots_ = std::allocator_traits<Allocator>::allocate(allocator_, capacity_);

            // [Core Defense]: Pre-fault every OS page at startup so the live phase never pays
            // first-touch page-fault spikes (~5us each). One byte per page suffices (a 2MB huge
            // page faults in entirely on first touch). A fixed 4KB step is safe on any base-page
            // size: stepping smaller than the real page only touches some pages twice.
            // Matches the pre-warming semantics of SpscInlineQueue (.BSS) and SpscRingBuffer (data_{}).
            volatile char* touch = reinterpret_cast<volatile char*>(slots_);
            const size_t total_bytes = static_cast<size_t>(capacity_) * sizeof(T);
            for (size_t off = 0; off < total_bytes; off += 4096) {
                touch[off] = 0;
            }
        }

        ~SpscHeapQueue() {
            // Destruction implies quiescence, so destroy the live range directly
            // instead of driving the front()/pop() protocol (N atomic stores).
            if constexpr (!std::is_trivially_destructible_v<T>) {
                const uint64_t head = read_index_.load(std::memory_order_relaxed);
                const uint64_t tail = write_index_.load(std::memory_order_relaxed);
                for (uint64_t i = head; i < tail; ++i) {
                    slots_[i & mask_].~T();
                }
            }
            std::allocator_traits<Allocator>::deallocate(allocator_, slots_, capacity_);
        }

        // Disable copy and move
        SpscHeapQueue(const SpscHeapQueue&) = delete;
        SpscHeapQueue& operator=(const SpscHeapQueue&) = delete;
        SpscHeapQueue(SpscHeapQueue&&) = delete;
        SpscHeapQueue& operator=(SpscHeapQueue&&) = delete;

        template <typename... Args>
        ZRQUEUE_FORCE_INLINE void emplace(Args&&...args) noexcept(std::is_nothrow_constructible_v<T, Args&&...>) {
            static_assert(std::is_constructible_v<T, Args&&...>, "T must be constructible with Args&&...");
            const uint64_t write_index = write_index_.load(std::memory_order_relaxed);
            const uint64_t next_write_index = write_index + 1;

            // [Mechanism]: Fetch latest consumer index across cores only when local cache indicates full (Cache Ping-Pong Defense)
            while (ZRQUEUE_UNLIKELY(next_write_index - cached_read_index_ > capacity_)) {
                cached_read_index_ = read_index_.load(std::memory_order_acquire);
                // [Mechanism]: _mm_pause relaxes hammering on the consumer's cache line,
                // otherwise every consumer release-store pays an RFO round-trip (measured 1.7-2.3x at small capacity)
                ZRQUEUE_CPU_PAUSE();
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

        // Allow peeking at elements 'offset' positions ahead of read cursor without moving it.
        // The bound is checked in the subtraction domain: read_index + offset can wrap for a
        // huge offset (peek(-1) converts to SIZE_MAX) and would then slip past the check.
        ZRQUEUE_NODISCARD ZRQUEUE_FORCE_INLINE T* peek(size_t offset = 0) noexcept {
            const uint64_t read_index = read_index_.load(std::memory_order_relaxed);
            const uint64_t off = static_cast<uint64_t>(offset);

            // Ensure offset does not cross the producer's write position
            if (ZRQUEUE_UNLIKELY(off >= cached_write_index_ - read_index)) {
                cached_write_index_ = write_index_.load(std::memory_order_acquire);
                if (ZRQUEUE_UNLIKELY(off >= cached_write_index_ - read_index)) {
                    return nullptr;
                }
            }

            return &slots_[(read_index + off) & mask_];
        }

        // ------------------------------------------------------------------------
        // Bulk Zero-Copy Read (front_bulk / pop_bulk)
        // Consumer claims up to `want` contiguous elements starting at the read
        // cursor, without copying. The run never straddles the wrap point; call
        // again after pop_bulk() to take the remainder. Pair every successful
        // front_bulk() with pop_bulk(n), n <= available.
        // ------------------------------------------------------------------------
        ZRQUEUE_NODISCARD ZRQUEUE_FORCE_INLINE T* front_bulk(size_t want, size_t& available) noexcept {
            available = 0;
            if (ZRQUEUE_UNLIKELY(want == 0)) {
                return nullptr;
            }
            const uint64_t read_index = read_index_.load(std::memory_order_relaxed);
            if (ZRQUEUE_UNLIKELY(read_index == cached_write_index_)) {
                cached_write_index_ = write_index_.load(std::memory_order_acquire);
                if (ZRQUEUE_UNLIKELY(read_index == cached_write_index_)) {
                    return nullptr;
                }
            }
            size_t count = static_cast<size_t>(cached_write_index_ - read_index);
            if (count > want) {
                count = want;
            }
            const size_t offset = static_cast<size_t>(read_index & mask_);
            const size_t till_wrap = capacity_ - offset;
            if (count > till_wrap) {
                count = till_wrap;
            }
            available = count;
            return &slots_[offset];
        }

        ZRQUEUE_FORCE_INLINE void pop_bulk(size_t n) noexcept {
            static_assert(std::is_nothrow_destructible_v<T>, "T must be nothrow destructible");
            if (ZRQUEUE_UNLIKELY(n == 0)) {
                return;
            }
            const uint64_t read_index = read_index_.load(std::memory_order_relaxed);
#ifndef NDEBUG
            assert(n <= cached_write_index_ - read_index && "pop_bulk(n) exceeds the published backlog");
#endif
            if constexpr (!std::is_trivially_destructible_v<T>) {
                for (size_t i = 0; i < n; ++i) {
                    slots_[(read_index + i) & mask_].~T();
                }
            }
            read_index_.store(read_index + n, std::memory_order_release);
        }

        // ------------------------------------------------------------------------
        // Bulk Push
        // Scenario: Gateway receives 10 market messages via UDP recvmmsg at once, pushing all instantly.
        // Advantage: Regardless of element count, executes ONLY ONE atomic Release barrier!
        // ------------------------------------------------------------------------
        template <typename Iterator>
        ZRQUEUE_NODISCARD size_t push_bulk(Iterator first, size_t count) noexcept {
            static_assert(std::is_nothrow_constructible_v<T, decltype(*first)>,
                          "push_bulk is noexcept: T must be nothrow-constructible from the iterator's value");
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
            static_assert(std::is_nothrow_destructible_v<T>, "T must be nothrow destructible");
            static_assert(std::is_nothrow_invocable_v<THandler, T&>,
                          "consume_bulk is noexcept: handler must be nothrow-invocable with T&");
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
            // The handler is invoked through std::forward on every iteration: the
            // parameter is a named variable (lvalue), so calling it directly would
            // reject rvalue-qualified operator() overloads. The forward is a
            // compile-time cast and emits no code, so repeating it is free.
            for (size_t i = 0; i < count_to_consume; ++i) {
                T& elem = slots_[(read_index + i) & mask_];
                std::forward<THandler>(handler)(elem); // zero-copy: reference, not a copy

                if constexpr (!std::is_trivially_destructible_v<T>) {
                    // Manually destruct after processing
                    elem.~T();
                }
            }

            // 3. Bulk process complete, only 1 atomic release required!
            read_index_.store(next_read_index, std::memory_order_release);
            return count_to_consume; // Return actual consumed count
        }

        // Observability only: reads two indices independently, so the result is a
        // stale, non-atomic snapshot. Never gate flow control on it - use
        // try_push/try_emplace/try_pop, which decide on the producer/consumer side.
        ZRQUEUE_NODISCARD ZRQUEUE_FORCE_INLINE size_t size() const noexcept {
            uint64_t w = write_index_.load(std::memory_order_relaxed);
            uint64_t r = read_index_.load(std::memory_order_relaxed);
            // Two independent loads, not a snapshot: r may be observed ahead of w
            // (consumer drained past the w we read), so guard the subtraction.
            return (w >= r) ? static_cast<size_t>(w - r) : 0;
        }

        // Observability only; same snapshot caveat as size().
        ZRQUEUE_NODISCARD ZRQUEUE_FORCE_INLINE bool empty() const noexcept {
            uint64_t w = write_index_.load(std::memory_order_relaxed);
            uint64_t r = read_index_.load(std::memory_order_relaxed);
            return w == r;
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
    };  // end SpscHeapQueue

    // Backward-compatible alias for the pre-1.6.0 name. New code should use SpscHeapQueue.
    template <typename T, typename Allocator = AlignedAllocator<T>>
    using SpscQueue [[deprecated("Renamed to SpscHeapQueue")]] = SpscHeapQueue<T, Allocator>;

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
        SpscInlineQueue() noexcept {
            // A user-provided constructor removes value-init zeroing for `new Q()`
            // and .BSS placement, so touch every page here instead of paying
            // first-touch faults in the live phase (16M ints is ~16K faults).
            volatile char* touch = reinterpret_cast<volatile char*>(slots_memory_);
            for (size_t off = 0; off < static_cast<size_t>(N) * sizeof(T); off += 4096) {
                touch[off] = 0;
            }
        }
        ~SpscInlineQueue() {
            // Destruction implies quiescence: walk the live range directly.
            if constexpr (!std::is_trivially_destructible_v<T>) {
                const uint64_t head = read_index_.load(std::memory_order_relaxed);
                const uint64_t tail = write_index_.load(std::memory_order_relaxed);
                T* raw_array = reinterpret_cast<T*>(slots_memory_);
                for (uint64_t i = head; i < tail; ++i) {
                    raw_array[i & MASK].~T();
                }
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
                ZRQUEUE_CPU_PAUSE();
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
            static_assert(std::is_nothrow_constructible_v<T, decltype(*first)>,
                          "push_bulk is noexcept: T must be nothrow-constructible from the iterator's value");
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
            const uint64_t off = static_cast<uint64_t>(offset);
            if (ZRQUEUE_UNLIKELY(off >= cached_write_index_ - read_index)) {
                cached_write_index_ = write_index_.load(std::memory_order_acquire);
                if (ZRQUEUE_UNLIKELY(off >= cached_write_index_ - read_index)) {
                    return nullptr;
                }
            }

            return &reinterpret_cast<T*>(slots_memory_)[(read_index + off) & MASK];
        }

        // Bulk Zero-Copy Read: same contract as SpscHeapQueue::front_bulk/pop_bulk.
        ZRQUEUE_NODISCARD ZRQUEUE_FORCE_INLINE T* front_bulk(size_t want, size_t& available) noexcept {
            available = 0;
            if (ZRQUEUE_UNLIKELY(want == 0)) {
                return nullptr;
            }
            const uint64_t read_index = read_index_.load(std::memory_order_relaxed);
            if (ZRQUEUE_UNLIKELY(read_index == cached_write_index_)) {
                cached_write_index_ = write_index_.load(std::memory_order_acquire);
                if (ZRQUEUE_UNLIKELY(read_index == cached_write_index_)) {
                    return nullptr;
                }
            }
            size_t count = static_cast<size_t>(cached_write_index_ - read_index);
            if (count > want) {
                count = want;
            }
            const size_t offset = static_cast<size_t>(read_index & MASK);
            const size_t till_wrap = N - offset;
            if (count > till_wrap) {
                count = till_wrap;
            }
            available = count;
            return &reinterpret_cast<T*>(slots_memory_)[offset];
        }

        ZRQUEUE_FORCE_INLINE void pop_bulk(size_t n) noexcept {
            static_assert(std::is_nothrow_destructible_v<T>, "T must be nothrow destructible");
            if (ZRQUEUE_UNLIKELY(n == 0)) {
                return;
            }
            const uint64_t read_index = read_index_.load(std::memory_order_relaxed);
#ifndef NDEBUG
            assert(n <= cached_write_index_ - read_index && "pop_bulk(n) exceeds the published backlog");
#endif
            if constexpr (!std::is_trivially_destructible_v<T>) {
                T* raw_array = reinterpret_cast<T*>(slots_memory_);
                for (size_t i = 0; i < n; ++i) {
                    raw_array[(read_index + i) & MASK].~T();
                }
            }
            read_index_.store(read_index + n, std::memory_order_release);
        }

        template <typename THandler>
        ZRQUEUE_NODISCARD size_t consume_bulk(THandler&& handler, size_t max_count = 0) noexcept {
            static_assert(std::is_nothrow_destructible_v<T>, "T must be nothrow destructible");
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
            // std::forward on every iteration is a compile-time cast that emits no
            // code; it is what keeps rvalue-qualified operator() overloads callable
            // (the parameter itself is a named lvalue).
            for (size_t i = 0; i < count_to_consume; ++i) {
                T& elem = raw_array[(read_index + i) & MASK];
                std::forward<THandler>(handler)(elem); // zero-copy: reference, not a copy

                if constexpr (!std::is_trivially_destructible_v<T>) {
                    elem.~T();
                }
            }

            read_index_.store(next_read_index, std::memory_order_release);
            return count_to_consume;
        }

        // Observability only: reads two indices independently, so the result is a
        // stale, non-atomic snapshot. Never gate flow control on it - use
        // try_push/try_emplace/try_pop, which decide on the producer/consumer side.
        ZRQUEUE_NODISCARD ZRQUEUE_FORCE_INLINE size_t size() const noexcept {
            uint64_t w = write_index_.load(std::memory_order_relaxed);
            uint64_t r = read_index_.load(std::memory_order_relaxed);
            // Two independent loads, not a snapshot: r may be observed ahead of w
            // (consumer drained past the w we read), so guard the subtraction.
            return (w >= r) ? static_cast<size_t>(w - r) : 0;
        }

        // Observability only; same snapshot caveat as size().
        ZRQUEUE_NODISCARD ZRQUEUE_FORCE_INLINE bool empty() const noexcept {
            uint64_t w = write_index_.load(std::memory_order_relaxed);
            uint64_t r = read_index_.load(std::memory_order_relaxed);
            return w == r;
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
        SpscRingBuffer() = default;
        ~SpscRingBuffer() = default;

        // non-copyable and non-movable
        SpscRingBuffer(const SpscRingBuffer&) = delete;
        SpscRingBuffer& operator=(const SpscRingBuffer&) = delete;
        SpscRingBuffer(SpscRingBuffer&&) = delete;
        SpscRingBuffer& operator=(SpscRingBuffer&&) = delete;

        // Claims one slot without publishing it. Every successful alloc() MUST be
        // paired with exactly one push(); every alloc_bulk() with one commit(n).
        // A missing commit leaves the slot unpublished (lost, not corrupted) and
        // trips the Debug-only pending_claim_ assertion on the next claim; Release
        // builds are silent. A second alloc() before push() overwrites the slot.
        ZRQUEUE_NODISCARD ZRQUEUE_FORCE_INLINE T* alloc() noexcept {
            const uint64_t write_index = write_index_.load(std::memory_order_relaxed);
            const uint64_t next_write_index = write_index + 1;

            // Interceptor: When local cache considers queue full (> N), enter slow path
            if (ZRQUEUE_UNLIKELY(next_write_index - cached_read_index_ > N)) {
                cached_read_index_ = read_index_.load(std::memory_order_acquire);
                if (ZRQUEUE_UNLIKELY(next_write_index - cached_read_index_ > N)) {
                    return nullptr;
                }
            }

#ifndef NDEBUG
            // A second alloc() before push()/commit() would return and overwrite the SAME slot
            assert(pending_claim_ == 0 && "alloc() called again before push()/commit(): previous claim not committed");
            pending_claim_ = 1;
#endif
            claimed_index_ = write_index;
            return &data_[write_index & MASK];
        }

        // Busy-wait variant of alloc(): spins (with CPU pause) until a slot is free.
        // Producer-side counterpart of spin_front().
        ZRQUEUE_NODISCARD ZRQUEUE_FORCE_INLINE T* spin_alloc() noexcept {
            const uint64_t write_index = write_index_.load(std::memory_order_relaxed);
            const uint64_t next_write_index = write_index + 1;

            while (ZRQUEUE_UNLIKELY(next_write_index - cached_read_index_ > N)) {
                cached_read_index_ = read_index_.load(std::memory_order_acquire);
                ZRQUEUE_CPU_PAUSE();
            }

#ifndef NDEBUG
            assert(pending_claim_ == 0 && "spin_alloc() called again before push()/commit(): previous claim not committed");
            pending_claim_ = 1;
#endif
            claimed_index_ = write_index;
            return &data_[write_index & MASK];
        }

        // ------------------------------------------------------------------------
        // Bulk Claim (Disruptor-style)
        // Scenario: Gateway receives a burst via recvmmsg, claims slots once, writes
        // in-place, commits all with ONE release barrier via commit(n).
        // Claims up to `want` consecutive slots; returns pointer to the first slot and
        // sets `granted` to the number actually claimed (nullptr / granted==0 when full).
        // The run NEVER straddles the ring's wrap point: `granted` may be smaller than
        // the free space if the remainder wraps; call alloc_bulk() again after commit()
        // to claim the rest. Single-producer: hot path reads only its own cache line.
        // ------------------------------------------------------------------------
        ZRQUEUE_NODISCARD T* alloc_bulk(size_t want, size_t& granted) noexcept {
            granted = 0;
            if (ZRQUEUE_UNLIKELY(want == 0)) {
                return nullptr;
            }
            const uint64_t write_index = write_index_.load(std::memory_order_relaxed);

            // Free space from the cached view; refresh across cores only when the cache says insufficient
            uint64_t free_count = N - (write_index - cached_read_index_);
            if (ZRQUEUE_UNLIKELY(free_count < want)) {
                cached_read_index_ = read_index_.load(std::memory_order_acquire);
                free_count = N - (write_index - cached_read_index_);
                if (ZRQUEUE_UNLIKELY(free_count == 0)) {
                    return nullptr;
                }
            }

            size_t count = want < free_count ? want : static_cast<size_t>(free_count);
            const size_t offset = static_cast<size_t>(write_index & MASK);
            const size_t till_wrap = N - offset; // slots from here to the physical end of data_
            if (count > till_wrap) {
                count = till_wrap; // contiguous-run semantics: never straddle the wrap point
            }

#ifndef NDEBUG
            assert(pending_claim_ == 0 && "alloc_bulk() called again before commit(): previous claim not committed");
            pending_claim_ = count;
#endif
            claimed_index_ = write_index;
            granted = count;
            return &data_[offset];
        }

        ZRQUEUE_FORCE_INLINE void push() noexcept {
#ifndef NDEBUG
            // push() commits exactly ONE slot; committing a bulk claim slot-by-slot would
            // silently strand the rest (never visible to the consumer, later re-claimed)
            assert(pending_claim_ == 1 && "push() commits a single alloc(); use commit(n) for alloc_bulk()");
            pending_claim_ = 0;
#endif
            write_index_.store(claimed_index_ + 1, std::memory_order_release);
        }

        // Commits n slots of a pending alloc_bulk() claim (n <= granted) with exactly ONE
        // release barrier. Slots beyond n are silently abandoned and re-claimed by a future alloc.
        // n == 0 is a no-op, matching pop_bulk(0).
        ZRQUEUE_FORCE_INLINE void commit(size_t n) noexcept {
            if (ZRQUEUE_UNLIKELY(n == 0)) {
                return;
            }
#ifndef NDEBUG
            assert(n <= pending_claim_ && "commit(n): n must be within the pending alloc_bulk() claim");
            pending_claim_ = 0;
#endif
            write_index_.store(claimed_index_ + n, std::memory_order_release);
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

        // The writer must not throw: a claim is already outstanding when it runs, so
        // an exception leaves that claim unpublished (Release: the slot is silently
        // reused; Debug: pending_claim_ trips on the next claim). Not enforced with a
        // static_assert because a plain lambda is not nothrow-invocable by default;
        // wrap throwing work in alloc() + your own try/catch + commit() instead.
        template<typename Writer>
        ZRQUEUE_FORCE_INLINE void push(Writer&& writer) {
            if constexpr (sizeof(T) > ZRQUEUE_CACHE_LINE_SIZE) {
                // Multi-cache-line payloads: try_push's bool return and nullptr
                // failure path force a stack frame, two register spills and two
                // extra branches (MSVC /O2). spin_alloc keeps one straight-line
                // claim path; measured +26% at 128B. Small payloads keep the
                // compact try_push loop, which wins there.
                T* v = spin_alloc();
                std::forward<Writer>(writer)(v);
                push();
            } else {
                while (!try_push(std::forward<Writer>(writer))) {
                    ZRQUEUE_CPU_PAUSE();
                }
            }
        }

        // Direct value API for pre-constructed slots. The caller already owns a
        // value, so no writer lambda claim/write/commit triple is needed: one
        // alloc(), one assignment into the slot, one release store. This is the
        // small-payload fast path; for in-place construction use push(Writer).
        template <typename P, typename = std::enable_if_t<std::is_assignable_v<T&, P&&>>>
        ZRQUEUE_NODISCARD ZRQUEUE_FORCE_INLINE bool try_push_value(P&& v) noexcept(std::is_nothrow_assignable_v<T&, P&&>) {
            T* slot = alloc();
            if (ZRQUEUE_LIKELY(slot != nullptr)) {
                *slot = std::forward<P>(v);
                push();
                return true;
            }
            return false;
        }

        template <typename P, typename = std::enable_if_t<std::is_assignable_v<T&, P&&>>>
        ZRQUEUE_FORCE_INLINE void push_value(P&& v) noexcept(std::is_nothrow_assignable_v<T&, P&&>) {
            if constexpr (sizeof(T) > ZRQUEUE_CACHE_LINE_SIZE) {
                T* slot = spin_alloc();
                *slot = std::forward<P>(v);
                push();
            } else {
                while (!try_push_value(std::forward<P>(v))) {
                    ZRQUEUE_CPU_PAUSE();
                }
            }
        }

        ZRQUEUE_NODISCARD ZRQUEUE_FORCE_INLINE T* front() noexcept {
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

        ZRQUEUE_FORCE_INLINE void pop() noexcept {
            const uint64_t read_index = read_index_.load(std::memory_order_relaxed);
            const uint64_t next_read_index = read_index + 1;
            read_index_.store(next_read_index, std::memory_order_release);
        }

        ZRQUEUE_NODISCARD ZRQUEUE_FORCE_INLINE T* peek(size_t offset = 0) noexcept {
            const uint64_t read_index = read_index_.load(std::memory_order_relaxed);
            const uint64_t off = static_cast<uint64_t>(offset);

            if (ZRQUEUE_UNLIKELY(off >= cached_write_index_ - read_index)) {
                cached_write_index_ = write_index_.load(std::memory_order_acquire);
                if (ZRQUEUE_UNLIKELY(off >= cached_write_index_ - read_index)) {
                    return nullptr;
                }
            }

            return &data_[(read_index + off) & MASK];
        }

        // Bulk Zero-Copy Read: elements are pre-constructed, so pop_bulk() only
        // advances the cursor (identical semantics to pop(), n slots at a time).
        ZRQUEUE_NODISCARD ZRQUEUE_FORCE_INLINE T* front_bulk(size_t want, size_t& available) noexcept {
            available = 0;
            if (ZRQUEUE_UNLIKELY(want == 0)) {
                return nullptr;
            }
            const uint64_t read_index = read_index_.load(std::memory_order_relaxed);
            if (ZRQUEUE_UNLIKELY(read_index == cached_write_index_)) {
                cached_write_index_ = write_index_.load(std::memory_order_acquire);
                if (ZRQUEUE_UNLIKELY(read_index == cached_write_index_)) {
                    return nullptr;
                }
            }
            size_t count = static_cast<size_t>(cached_write_index_ - read_index);
            if (count > want) {
                count = want;
            }
            const size_t offset = static_cast<size_t>(read_index & MASK);
            const size_t till_wrap = N - offset;
            if (count > till_wrap) {
                count = till_wrap;
            }
            available = count;
            return &data_[offset];
        }

        ZRQUEUE_FORCE_INLINE void pop_bulk(size_t n) noexcept {
            if (ZRQUEUE_UNLIKELY(n == 0)) {
                return;
            }
            const uint64_t read_index = read_index_.load(std::memory_order_relaxed);
#ifndef NDEBUG
            assert(n <= cached_write_index_ - read_index && "pop_bulk(n) exceeds the published backlog");
#endif
            read_index_.store(read_index + n, std::memory_order_release);
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

        // ------------------------------------------------------------------------
        // Bulk Zero-Copy Consume
        // Advantage: Pass Lambda for in-place processing, executes ONLY ONE Release barrier.
        // Unlike the placement-new queues, elements are never destructed (pre-constructed
        // ring); the handler receives T& and must leave the slot reusable.
        // max_count defaults to 0, meaning "consume as many as available".
        // ------------------------------------------------------------------------
        template <typename THandler>
        ZRQUEUE_NODISCARD size_t consume_bulk(THandler&& handler, size_t max_count = 0) noexcept {
            static_assert(std::is_nothrow_invocable_v<THandler, T&>,
                          "consume_bulk is noexcept: handler must be nothrow-invocable with T&");
            const uint64_t read_index = read_index_.load(std::memory_order_relaxed);

            if (ZRQUEUE_UNLIKELY(read_index == cached_write_index_)) {
                cached_write_index_ = write_index_.load(std::memory_order_acquire);
                if (ZRQUEUE_UNLIKELY(read_index == cached_write_index_)) {
                    return 0; // Ring is empty
                }
            }

            const size_t available = static_cast<size_t>(cached_write_index_ - read_index);
            const size_t count_to_consume = (max_count == 0 || max_count > available) ? available : max_count;

            // std::forward on every iteration is a compile-time cast that emits no
            // code; it is what keeps rvalue-qualified operator() overloads callable
            // (the parameter itself is a named lvalue). Elements are pre-constructed
            // and never destructed here, so the handler must leave the slot reusable.
            for (size_t i = 0; i < count_to_consume; ++i) {
                T& elem = data_[(read_index + i) & MASK];
                std::forward<THandler>(handler)(elem); // zero-copy: reference, not a copy
            }

            read_index_.store(read_index + count_to_consume, std::memory_order_release);
            return count_to_consume;
        }

        // Observability only: reads two indices independently, so the result is a
        // stale, non-atomic snapshot. Never gate flow control on it - use
        // try_push/try_emplace/try_pop, which decide on the producer/consumer side.
        ZRQUEUE_NODISCARD ZRQUEUE_FORCE_INLINE size_t size() const noexcept {
            uint64_t w = write_index_.load(std::memory_order_relaxed);
            uint64_t r = read_index_.load(std::memory_order_relaxed);
            // Two independent loads, not a snapshot: r may be observed ahead of w
            // (consumer drained past the w we read), so guard the subtraction.
            return (w >= r) ? static_cast<size_t>(w - r) : 0;
        }

        // Observability only; same snapshot caveat as size().
        ZRQUEUE_NODISCARD ZRQUEUE_FORCE_INLINE bool empty() const noexcept {
            uint64_t w = write_index_.load(std::memory_order_relaxed);
            uint64_t r = read_index_.load(std::memory_order_relaxed);
            return w == r;
        }

        ZRQUEUE_NODISCARD ZRQUEUE_FORCE_INLINE constexpr size_t capacity() const noexcept {
            return N;
        }

    private:
        static constexpr uint32_t MASK = N - 1;

        // [Cache Line]: Producer Zone
        alignas(ZRQUEUE_CACHE_LINE_SIZE) std::atomic<uint64_t> write_index_{ 0 };
        alignas(ZRQUEUE_CACHE_LINE_SIZE) uint64_t cached_read_index_ { 0 };
        // Index handed out by the pending claim. push()/commit() advance from this
        // instead of re-loading write_index_ (the atomic cannot be cached across the
        // caller's writer call, so the reload was a real load on the release path).
        uint64_t claimed_index_{ 0 };
#ifndef NDEBUG
        // Debug-only misuse detector: number of claimed-but-uncommitted slots
        // (1 from alloc()/spin_alloc(), k from alloc_bulk()). Producer-local,
        // shares the line above (no layout impact).
        size_t pending_claim_ = 0;
#endif

        // [Cache Line]: Consumer Zone
        alignas(ZRQUEUE_CACHE_LINE_SIZE) std::atomic<uint64_t> read_index_{ 0 };
        alignas(ZRQUEUE_CACHE_LINE_SIZE) uint64_t cached_write_index_ { 0 };

        // Data Zone. alignas(T) keeps over-aligned element types legal: a bare
        // alignas(64) would be weaker than alignof(T) for e.g. alignas(128) types.
        alignas(ZRQUEUE_CACHE_LINE_SIZE) alignas(T) std::array<T, N> data_ { };
    };  // end SpscRingBuffer

    // ============================================================================
    // [Mirror Policy Customization Points]
    // ============================================================================
    // SpscMirrorQueue mirrors a payload into the producer's index cache line so
    // the consumer's acquire-load of write_index_ delivers index + payload in a
    // single cross-core line migration. Two payload widths are supported:
    //
    //   sizeof(T) <= 4  entry = {seq32 | payload32} (8 B), K = 4
    //   sizeof(T) == 8  entry = {seq64, payload64} (16 B), K = 2, seqlock
    //
    // mirror_traits<T> is a compile-time policy. Defaults:
    //   - 4 B: mirror enabled for every trivially-copyable type of size 4 or less;
    //   - 8 B: mirror enabled only for arithmetic scalars (uint64_t, int64_t,
    //          double, ...). Custom 8 B structs fall back to the classic path
    //          unless they opt in.
    // The 8 B seqlock writes two 8 B words into the index line, so it trades
    // roughly 20% saturated throughput for roughly 20% lower RTT. Disable it
    // for a throughput-only queue by specializing mirror_traits<T>::enable_8b
    // to false (the 4 B path and classic protocol are unaffected).
    // Specialize BOTH flags (an explicit specialization does not inherit the
    // primary template's defaults):
    //
    //   template <> struct zrqueue::mirror_traits<MyTick8> {
    //       static constexpr bool enable_4b = false;
    //       static constexpr bool enable_8b = true;
    //   };
    //
    // mirror_codec<T> is an optional second customization point. If a
    // specialization provides `static <payload_type> pack(Arg&...)`, emplace()
    // packs the mirror payload from the construction arguments instead of
    // reading the constructed object back out of its slot. Use it for types
    // whose constructor emits several narrow stores (byte/field fills): reading
    // such an object back defeats store-to-load forwarding and collapses
    // throughput. The codec must be noexcept and must not move from its
    // arguments; it receives lvalue references and runs before the slot is
    // constructed.
    template <typename T>
    struct mirror_traits {
        static constexpr bool enable_4b = true;
        static constexpr bool enable_8b = std::is_arithmetic_v<T>;
    };

    template <typename T>
    using mirror_payload_t = std::conditional_t<(sizeof(T) <= 4), uint32_t, uint64_t>;

    template <typename T>
    struct mirror_codec {
        // Primary template intentionally has no pack(); detection below falls
        // back to the authoritative slot read-back.
    };

    namespace mirror_detail {
        template <typename T, typename... Args>
        struct has_codec {
        private:
            template <typename U, typename = decltype(mirror_codec<U>::pack(std::declval<Args&>()...))>
            static std::true_type test(int);
            template <typename U> static std::false_type test(...);
        public:
            static constexpr bool value = decltype(test<T>(0))::value;
        };
    } // namespace mirror_detail

    // ============================================================================
    // [Latency-Optimized Lock-Free Queue SpscMirrorQueue (Latest-K Element Mirroring)]
    // ============================================================================
    // Idea: in classic SPSC, delivering a near-empty-queue element costs TWO
    // cross-core cache-line migrations: the index line (write_index_) and the
    // data line (the slot). Here the producer additionally packs every pushed
    // element into an atomic entry living in its OWN index line (the line the
    // consumer's polling load fetches anyway). The consumer's acquire-load of
    // write_index_ returns the index AND brings the payload along; the entry is
    // then read as a plain L1 hit, atomically - no torn reads, no re-validation.
    // Ping-pong round-trip: 4 line migrations -> 2 (~40% lower RTT, measured).
    //
    // Safety: availability is still detected via the monotonic write_index_ (never
    // missed, even if the producer laps the mirror ring). An entry for element r
    // is only overwritten by element r+K, whose store happens-before write_index_
    // reaches r+K+1; the consumer gates the fast path on depth <= K, double-checks
    // the entry (seq for 4 B, seq/payload/seq for 8 B), and falls back to the
    // packed slots otherwise. Slots always hold authoritative data (the classic
    // protocol is fully preserved), so pop() always destructs the slot.
    //
    // The 8 B entry uses a seqlock: producer stores seq, then payload (release),
    // then write_index_ (release); consumer loads seq, payload (acquire), then
    // seq again. A concurrent overwrite either changes the first seq (miss) or,
    // if the payload load observes the newer value, the payload release/acquire
    // pair makes the newer seq visible to the second load (miss as well).
    //
    // Deep-backlog throughput path is untouched: packed slots, cached remote
    // indices, one release store per push. The mirror adds one or two stores to
    // the producer's OWN line per push, and in shallow flow REMOVES the data-line
    // migration classic SPSC pays per element.
    //
    // Payload extraction is the one place where the mirror can lose: reading a
    // just-constructed object back from its slot stalls when the constructor
    // emitted narrow (byte/field) stores. emplace_packed()/push_packed() let the
    // caller supply the payload explicitly, and mirror_codec<T> lets a type
    // provide it from the constructor arguments without changing call sites.
    //
    // Deliberately minimal API (RTT-focused): no bulk ops - use SpscRingBuffer or
    // SpscInlineQueue for burst workloads.
    template <typename T, uint32_t N>
    class SpscMirrorQueue {
        static_assert(N >= 2, "Capacity must be at least 2");
        static_assert((N & (N - 1)) == 0, "Capacity N must be a power of 2 for bitwise masking");

        static constexpr uint32_t MASK = N - 1;

        static constexpr bool MIRROR_4 =
            std::is_trivially_copyable_v<T> && sizeof(T) <= 4 && alignof(T) <= 8 &&
            mirror_traits<T>::enable_4b;
        static constexpr bool MIRROR_8 =
            std::is_trivially_copyable_v<T> && sizeof(T) == 8 && alignof(T) <= 8 &&
            mirror_traits<T>::enable_8b;
        static constexpr bool MIRROR_OK = MIRROR_4 || MIRROR_8;
        static constexpr size_t MIRROR_K = MIRROR_8 ? 2 : (MIRROR_4 ? 4 : 1);
        static constexpr size_t MIRROR_MASK = MIRROR_K - 1;
        static constexpr uint64_t EMPTY_ENTRY_4B = 0xFFFFFFFF00000000ULL; // seq = UINT32_MAX

        struct MirrorEntry8 {
            std::atomic<uint64_t> seq;
            std::atomic<uint64_t> payload;
            MirrorEntry8() noexcept : seq(0), payload(0) {}
        };
        using MirrorEntry = std::conditional_t<MIRROR_8, MirrorEntry8, std::atomic<uint64_t>>;
        using MirrorStash = std::conditional_t<MIRROR_OK, T, std::byte>;

    public:
        using payload_type = mirror_payload_t<T>;
        static constexpr bool mirror_enabled = MIRROR_OK;
        static constexpr size_t mirror_window = MIRROR_OK ? MIRROR_K : 0;

        SpscMirrorQueue() noexcept {
            if constexpr (MIRROR_4) {
                for (size_t i = 0; i < MIRROR_K; ++i) {
                    mirror_entries_[i].store(EMPTY_ENTRY_4B, std::memory_order_relaxed);
                }
            } else if constexpr (MIRROR_8) {
                for (size_t i = 0; i < MIRROR_K; ++i) {
                    mirror_entries_[i].seq.store(~static_cast<uint64_t>(0), std::memory_order_relaxed);
                    mirror_entries_[i].payload.store(0, std::memory_order_relaxed);
                }
            } else {
                mirror_entries_[0].store(0, std::memory_order_relaxed);
            }
            // [Core Defense]: Pre-fault every OS page of the slot array at startup so the
            // live phase never pays first-touch page-fault spikes (~1-5us each). This
            // class has a user-provided constructor, so unlike SpscInlineQueue it gets
            // NO value-init zeroing for free - at 16M ints a fresh heap object would
            // otherwise take ~10K faults during the live phase (~3x throughput hit).
            volatile char* touch = reinterpret_cast<volatile char*>(slots_);
            for (size_t off = 0; off < static_cast<size_t>(N) * sizeof(T); off += 4096) {
                touch[off] = 0;
            }
        }
        ~SpscMirrorQueue() {
            // Destruction implies quiescence: walk the live range directly.
            if constexpr (!std::is_trivially_destructible_v<T>) {
                const uint64_t head = read_index_.load(std::memory_order_relaxed);
                const uint64_t tail = write_index_.load(std::memory_order_relaxed);
                for (uint64_t i = head; i < tail; ++i) {
                    slot_at(i)->~T();
                }
            }
        }

        // non-copyable and non-movable
        SpscMirrorQueue(const SpscMirrorQueue&) = delete;
        SpscMirrorQueue& operator=(const SpscMirrorQueue&) = delete;
        SpscMirrorQueue(SpscMirrorQueue&&) = delete;
        SpscMirrorQueue& operator=(SpscMirrorQueue&&) = delete;

        template <typename... Args>
        ZRQUEUE_FORCE_INLINE void emplace(Args&&... args) noexcept(std::is_nothrow_constructible_v<T, Args&&...>) {
            static_assert(std::is_constructible_v<T, Args&&...>, "T must be constructible with Args&&...");
            const uint64_t write_index = write_index_.load(std::memory_order_relaxed);
            const uint64_t next_write_index = write_index + 1;

            while (ZRQUEUE_UNLIKELY(next_write_index - cached_read_index_ > N)) {
                cached_read_index_ = read_index_.load(std::memory_order_acquire);
                ZRQUEUE_CPU_PAUSE();
            }

            T* slot = slot_at(write_index);
            if constexpr (MIRROR_OK) {
                if constexpr (mirror_detail::has_codec<T, Args...>::value) {
                    const payload_type payload =
                        static_cast<payload_type>(mirror_codec<T>::pack(args...));
                    new (slot) T(std::forward<Args>(args)...);
                    mirror_store(write_index, payload);
                } else {
                    new (slot) T(std::forward<Args>(args)...);
                    mirror_from_slot(write_index, slot);
                }
            } else {
                new (slot) T(std::forward<Args>(args)...);
            }
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

            T* slot = slot_at(write_index);
            if constexpr (MIRROR_OK) {
                if constexpr (mirror_detail::has_codec<T, Args...>::value) {
                    const payload_type payload =
                        static_cast<payload_type>(mirror_codec<T>::pack(args...));
                    new (slot) T(std::forward<Args>(args)...);
                    mirror_store(write_index, payload);
                } else {
                    new (slot) T(std::forward<Args>(args)...);
                    mirror_from_slot(write_index, slot);
                }
            } else {
                new (slot) T(std::forward<Args>(args)...);
            }
            write_index_.store(next_write_index, std::memory_order_release);
            return true;
        }

        // --------------------------------------------------------------------
        // Explicit payload API. The caller supplies the object representation of
        // the constructed T, so publication never depends on reading the slot
        // back. Required for custom types whose constructor emits narrow stores;
        // otherwise the mirror read-back can cost 3-7x throughput.
        // Contract: payload must equal the byte representation of T(args...).
        // --------------------------------------------------------------------
        template <typename... Args>
        ZRQUEUE_FORCE_INLINE void emplace_packed(payload_type payload, Args&&... args) noexcept(std::is_nothrow_constructible_v<T, Args&&...>) {
            static_assert(MIRROR_OK, "emplace_packed requires a mirror-eligible T (see mirror_traits)");
            static_assert(std::is_constructible_v<T, Args&&...>, "T must be constructible with Args&&...");
            const uint64_t write_index = write_index_.load(std::memory_order_relaxed);
            const uint64_t next_write_index = write_index + 1;

            while (ZRQUEUE_UNLIKELY(next_write_index - cached_read_index_ > N)) {
                cached_read_index_ = read_index_.load(std::memory_order_acquire);
                ZRQUEUE_CPU_PAUSE();
            }

            new (slot_at(write_index)) T(std::forward<Args>(args)...);
            mirror_store(write_index, payload);
            write_index_.store(next_write_index, std::memory_order_release);
        }

        template <typename... Args>
        ZRQUEUE_NODISCARD ZRQUEUE_FORCE_INLINE bool try_emplace_packed(payload_type payload, Args&&... args) noexcept(std::is_nothrow_constructible_v<T, Args&&...>) {
            static_assert(MIRROR_OK, "try_emplace_packed requires a mirror-eligible T (see mirror_traits)");
            static_assert(std::is_constructible_v<T, Args&&...>, "T must be constructible with Args&&...");
            const uint64_t write_index = write_index_.load(std::memory_order_relaxed);
            const uint64_t next_write_index = write_index + 1;

            if (ZRQUEUE_UNLIKELY(next_write_index - cached_read_index_ > N)) {
                cached_read_index_ = read_index_.load(std::memory_order_acquire);
                if (ZRQUEUE_UNLIKELY(next_write_index - cached_read_index_ > N)) {
                    return false;
                }
            }

            new (slot_at(write_index)) T(std::forward<Args>(args)...);
            mirror_store(write_index, payload);
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

        ZRQUEUE_FORCE_INLINE void push_packed(payload_type payload, const T& v) noexcept(std::is_nothrow_copy_constructible_v<T>) {
            emplace_packed(payload, v);
        }

        template <typename P, typename = std::enable_if_t<std::is_constructible_v<T, P&&>>>
        ZRQUEUE_FORCE_INLINE void push_packed(payload_type payload, P&& v) noexcept(std::is_nothrow_constructible_v<T, P&&>) {
            emplace_packed(payload, std::forward<P>(v));
        }

        ZRQUEUE_NODISCARD ZRQUEUE_FORCE_INLINE bool try_push_packed(payload_type payload, const T& v) noexcept(std::is_nothrow_copy_constructible_v<T>) {
            return try_emplace_packed(payload, v);
        }

        template <typename P, typename = std::enable_if_t<std::is_constructible_v<T, P&&>>>
        ZRQUEUE_NODISCARD ZRQUEUE_FORCE_INLINE bool try_push_packed(payload_type payload, P&& v) noexcept(std::is_nothrow_constructible_v<T, P&&>) {
            return try_emplace_packed(payload, std::forward<P>(v));
        }

        // ------------------------------------------------------------------------
        // Bulk Push: construct up to `count` elements from an iterator and publish
        // them with ONE release store instead of one per element. Returns the
        // number actually written (partial push when the queue cannot fit the
        // whole run; 0 when full). Same signature and contract as
        // SpscHeapQueue::push_bulk / SpscInlineQueue::push_bulk.
        //
        // Mirror behaviour: entries are written only for elements that stay inside
        // the K-element window (the last K of the run). The consumer can only use
        // an entry when the backlog is <= K, which is exactly when the newest
        // elements are the ones being read. Earlier elements fall back to their
        // slots, which are authoritative anyway. Skipping those mirror stores is
        // not a micro-optimisation: a mirror entry for element i is overwritten by
        // element i+K, so a run longer than K would write K entries twice for no
        // benefit. Measured cost of the whole bulk path versus a per-element
        // emplace loop: none (see README) - the gain is API parity, not speed.
        // ------------------------------------------------------------------------
        template <typename Iterator>
        ZRQUEUE_NODISCARD size_t push_bulk(Iterator first, size_t count) noexcept {
            static_assert(std::is_nothrow_constructible_v<T, decltype(*first)>,
                          "push_bulk is noexcept: T must be nothrow-constructible from the iterator's value");
            if (ZRQUEUE_UNLIKELY(count == 0)) {
                return 0;
            }
            const uint64_t write_index = write_index_.load(std::memory_order_relaxed);
            uint64_t next_write_index = write_index + count;

            if (ZRQUEUE_UNLIKELY(next_write_index - cached_read_index_ > N)) {
                cached_read_index_ = read_index_.load(std::memory_order_acquire);
                if (ZRQUEUE_UNLIKELY(next_write_index - cached_read_index_ > N)) {
                    count = static_cast<size_t>(N - (write_index - cached_read_index_));
                    if (ZRQUEUE_UNLIKELY(count == 0)) {
                        return 0; // Queue is totally full
                    }
                    next_write_index = write_index + count;
                }
            }

            // Sequential placement new, no atomic traffic until the final store.
            for (size_t i = 0; i < count; ++i) {
                new (slot_at(write_index + i)) T(*first++);
            }

            if constexpr (MIRROR_OK) {
                // Only the trailing window can still be read through the mirror.
                const size_t mirrored = count < MIRROR_K ? count : MIRROR_K;
                for (size_t i = count - mirrored; i < count; ++i) {
                    mirror_from_slot(write_index + i, slot_at(write_index + i));
                }
            }

            write_index_.store(next_write_index, std::memory_order_release);
            return count;
        }

        ZRQUEUE_NODISCARD ZRQUEUE_FORCE_INLINE T* front() noexcept {
            const uint64_t read_index = read_index_.load(std::memory_order_relaxed);
            if (ZRQUEUE_LIKELY(read_index != cached_write_index_)) {
                return slot_at(read_index); // deep backlog: packed-slot path (authoritative)
            }
            const uint64_t w1 = write_index_.load(std::memory_order_acquire);
            if (ZRQUEUE_UNLIKELY(read_index == w1)) {
                return nullptr;
            }
            cached_write_index_ = w1;
            if constexpr (MIRROR_OK) {
                if (T* m = try_mirror(read_index, w1)) {
                    return m;
                }
            }
            return slot_at(read_index);
        }

        ZRQUEUE_NODISCARD T* spin_front() noexcept {
            const uint64_t read_index = read_index_.load(std::memory_order_relaxed);
            if (ZRQUEUE_LIKELY(read_index != cached_write_index_)) {
                return slot_at(read_index);
            }
            while (true) {
                const uint64_t w1 = write_index_.load(std::memory_order_acquire);
                if (ZRQUEUE_LIKELY(read_index != w1)) {
                    cached_write_index_ = w1;
                    if constexpr (MIRROR_OK) {
                        if (T* m = try_mirror(read_index, w1)) {
                            return m;
                        }
                    }
                    return slot_at(read_index);
                }
                ZRQUEUE_CPU_PAUSE();
            }
        }

        ZRQUEUE_FORCE_INLINE void pop() noexcept {
            static_assert(std::is_nothrow_destructible_v<T>, "T must be nothrow destructible");
            const uint64_t read_index = read_index_.load(std::memory_order_relaxed);
            if constexpr (!std::is_trivially_destructible_v<T>) {
                slot_at(read_index)->~T(); // slot always holds a constructed element, mirror or not
            }
            read_index_.store(read_index + 1, std::memory_order_release);
        }

        // ------------------------------------------------------------------------
        // Bulk Zero-Copy Consume: drain up to max_count (0 = everything published)
        // with ONE release store for the whole run, instead of one per element.
        //
        // Unlike SpscHeapQueue/SpscInlineQueue/SpscRingBuffer there is no
        // front_bulk(): those hand back a pointer into a contiguous slot array,
        // while here a mirror hit unpacks into the single-element mirror_stash_ and
        // a run can mix mirror hits with slot fallbacks, so no contiguous run
        // exists to return. The handler therefore receives one T& at a time, which
        // still costs no copy for non-mirrored elements.
        //
        // The mirror fast path is used per element when eligible. Safety is the
        // same as front(): the entry's seq must equal the element's index, so an
        // entry already overwritten by a later lap misses and falls back to the
        // authoritative slot. cached_write_index_ is an acquire-loaded past value
        // of write_index_, so the depth pre-filter can only under-estimate the
        // backlog; an under-estimate is still safe because the seq check decides.
        //
        // Handler and T must be nothrow: the indices are published with a single
        // release store after the loop, so a throw would strand a partially
        // consumed run.
        // ------------------------------------------------------------------------
        template <typename THandler>
        ZRQUEUE_NODISCARD size_t consume_bulk(THandler&& handler, size_t max_count = 0) noexcept {
            static_assert(std::is_nothrow_destructible_v<T>, "T must be nothrow destructible");
            static_assert(std::is_nothrow_invocable_v<THandler, T&>,
                          "consume_bulk is noexcept: handler must be nothrow-invocable with T&");
            const uint64_t read_index = read_index_.load(std::memory_order_relaxed);

            if (ZRQUEUE_UNLIKELY(read_index == cached_write_index_)) {
                cached_write_index_ = write_index_.load(std::memory_order_acquire);
                if (ZRQUEUE_UNLIKELY(read_index == cached_write_index_)) {
                    return 0; // Queue is empty
                }
            }

            const uint64_t available = cached_write_index_ - read_index;
            const size_t count = (max_count == 0 || max_count > available)
                                     ? static_cast<size_t>(available)
                                     : max_count;

            // Mirror entries are only meaningful while the whole run fits the
            // window: entry i is overwritten once the producer is more than
            // MIRROR_K elements ahead, and a burst drain usually is. Skipping the
            // per-element mirror probe in that case is what keeps this loop at
            // parity with the manual front()/pop() drain (a probe that must miss
            // is pure overhead - measured 16% slower without this guard).
            const bool probe_mirror = MIRROR_OK && available <= MIRROR_K;

            for (size_t i = 0; i < count; ++i) {
                const uint64_t index = read_index + i;
                T* elem = nullptr;
                if constexpr (MIRROR_OK) {
                    if (probe_mirror) {
                        elem = try_mirror(index, cached_write_index_);
                    }
                }
                if (elem == nullptr) {
                    elem = slot_at(index);
                }
                std::forward<THandler>(handler)(*elem); // zero-copy: reference, not a copy

                if constexpr (!std::is_trivially_destructible_v<T>) {
                    // The slot is authoritative and always constructed, so it is
                    // destructed even when the handler saw the mirror stash.
                    slot_at(index)->~T();
                }
            }

            read_index_.store(read_index + count, std::memory_order_release);
            return count;
        }

        // Reads the authoritative slot directly (mirroring is a front()-path
        // optimization only); the slot is always valid for any in-queue element.
        ZRQUEUE_NODISCARD ZRQUEUE_FORCE_INLINE T* peek(size_t offset = 0) noexcept {
            const uint64_t read_index = read_index_.load(std::memory_order_relaxed);
            const uint64_t off = static_cast<uint64_t>(offset);
            if (ZRQUEUE_UNLIKELY(off >= cached_write_index_ - read_index)) {
                cached_write_index_ = write_index_.load(std::memory_order_acquire);
                if (ZRQUEUE_UNLIKELY(off >= cached_write_index_ - read_index)) {
                    return nullptr;
                }
            }
            return slot_at(read_index + off);
        }

        // Observability only: reads two indices independently, so the result is a
        // stale, non-atomic snapshot. Never gate flow control on it - use
        // try_push/try_emplace/try_pop, which decide on the producer/consumer side.
        ZRQUEUE_NODISCARD ZRQUEUE_FORCE_INLINE size_t size() const noexcept {
            const uint64_t w = write_index_.load(std::memory_order_relaxed);
            const uint64_t r = read_index_.load(std::memory_order_relaxed);
            // Two independent loads, not a snapshot: guard the subtraction.
            return (w >= r) ? static_cast<size_t>(w - r) : 0;
        }

        // Observability only; same snapshot caveat as size().
        ZRQUEUE_NODISCARD ZRQUEUE_FORCE_INLINE bool empty() const noexcept {
            uint64_t w = write_index_.load(std::memory_order_relaxed);
            uint64_t r = read_index_.load(std::memory_order_relaxed);
            return w == r;
        }

        ZRQUEUE_NODISCARD ZRQUEUE_FORCE_INLINE constexpr size_t capacity() const noexcept {
            return N;
        }

    private:
        ZRQUEUE_FORCE_INLINE T* slot_at(uint64_t index) noexcept {
            return &reinterpret_cast<T*>(slots_)[index & MASK];
        }

        // Producer: pack a caller-supplied payload into the mirror entry.
        ZRQUEUE_FORCE_INLINE void mirror_store(uint64_t write_index, payload_type payload) noexcept {
            if constexpr (MIRROR_4) {
                mirror_entries_[write_index & MIRROR_MASK].store(
                    (static_cast<uint64_t>(static_cast<uint32_t>(write_index)) << 32) | payload,
                    std::memory_order_relaxed);
            } else if constexpr (MIRROR_8) {
                MirrorEntry8& e = mirror_entries_[write_index & MIRROR_MASK];
                e.seq.store(write_index, std::memory_order_relaxed);
                e.payload.store(payload, std::memory_order_release);
            } else {
                (void)write_index;
                (void)payload;
            }
        }

        // Producer default path: read the just-constructed element back out of its
        // slot. Cheap for single-store constructors, costly for narrow stores.
        ZRQUEUE_FORCE_INLINE void mirror_from_slot(uint64_t write_index, const T* slot) noexcept {
            if constexpr (MIRROR_4) {
                uint32_t lo = 0;
                std::memcpy(&lo, slot, sizeof(T));
                mirror_store(write_index, lo);
            } else if constexpr (MIRROR_8) {
                uint64_t payload = 0;
                std::memcpy(&payload, slot, sizeof(T));
                mirror_store(write_index, payload);
            }
        }

        // Consumer: one atomic load of the mirror entry - seq and payload arrive
        // together. The entry rides the same cache line as write_index_, so after
        // the acquire-load above this is an L1 hit. The unpacked element is
        // stashed on the consumer's OWN line, so the fast path never touches any
        // other shared state.
        ZRQUEUE_FORCE_INLINE T* try_mirror(uint64_t read_index, uint64_t w1) noexcept {
            if constexpr (MIRROR_4) {
                if (w1 - read_index <= MIRROR_K) {
                    const uint64_t e = mirror_entries_[read_index & MIRROR_MASK].load(std::memory_order_relaxed);
                    if (ZRQUEUE_LIKELY(static_cast<uint32_t>(e >> 32) == static_cast<uint32_t>(read_index))) {
                        const uint32_t lo = static_cast<uint32_t>(e);
                        std::memcpy(&mirror_stash_, &lo, sizeof(T));
                        return &mirror_stash_;
                    }
                }
            } else if constexpr (MIRROR_8) {
                if (w1 - read_index <= MIRROR_K) {
                    const MirrorEntry8& e = mirror_entries_[read_index & MIRROR_MASK];
                    const uint64_t s1 = e.seq.load(std::memory_order_relaxed);
                    if (ZRQUEUE_LIKELY(s1 == read_index)) {
                        const uint64_t payload = e.payload.load(std::memory_order_acquire);
                        if (ZRQUEUE_LIKELY(e.seq.load(std::memory_order_relaxed) == s1)) {
                            std::memcpy(&mirror_stash_, &payload, sizeof(T));
                            return &mirror_stash_;
                        }
                    }
                }
            }
            return nullptr;
        }

        // [Cache Line PAIR 0: Producer Zone]. The packed entries share write_index_'s
        // line so the consumer's polling load fetches index + payload in ONE cross-core
        // migration. The buddy pad keeps the 2-line L2 adjacent-line prefetch pair
        // single-owner: prefetching write_index_'s line only ever pulls producer-owned
        // bytes, never the consumer's read_index_ line.
        alignas(2 * ZRQUEUE_CACHE_LINE_SIZE) std::atomic<uint64_t> write_index_{ 0 };
        uint64_t cached_read_index_{ 0 };
        MirrorEntry mirror_entries_[MIRROR_K];
        static constexpr size_t PROD_LINE_USED = 16 + MIRROR_K * sizeof(MirrorEntry);
        static_assert(PROD_LINE_USED <= ZRQUEUE_CACHE_LINE_SIZE,
                      "mirror entries must fit the producer cache line");
        std::byte producer_tail_pad_[PROD_LINE_USED < ZRQUEUE_CACHE_LINE_SIZE
                                       ? ZRQUEUE_CACHE_LINE_SIZE - PROD_LINE_USED : 1];
        std::byte producer_buddy_pad_[ZRQUEUE_CACHE_LINE_SIZE]; // prefetch buddy, never touched

        // [Cache Line PAIR 1: Consumer Zone]. mirror_stash_ lives on the consumer's own
        // line so the unpack never touches shared state. Tail pad covers the rest of
        // the consumer line plus the untouched prefetch buddy line.
        alignas(2 * ZRQUEUE_CACHE_LINE_SIZE) std::atomic<uint64_t> read_index_{ 0 };
        uint64_t cached_write_index_{ 0 };
        alignas(MIRROR_OK ? alignof(T) : 1) MirrorStash mirror_stash_{};
        static constexpr size_t CONS_LINE_USED = 16 + (MIRROR_OK ? sizeof(T) : 1);
        std::byte consumer_tail_pad_[CONS_LINE_USED < 2 * ZRQUEUE_CACHE_LINE_SIZE
                                       ? 2 * ZRQUEUE_CACHE_LINE_SIZE - CONS_LINE_USED : 1];

        // [Data Zone]: packed authoritative slots (classic path, deep backlog)
        alignas(2 * ZRQUEUE_CACHE_LINE_SIZE) alignas(T) std::byte slots_[N * sizeof(T)];
    };  // end SpscMirrorQueue

}   // end namespace zrqueue