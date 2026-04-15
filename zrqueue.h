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

#define ZRQUEUE_VERSION 10404 // 1.4.4

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
 // [平台与系统底层 API 探测]
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
// [编译器宏]
// ============================================================================
// 获取 CPU L1 缓存行大小 (防伪共享核心参数)
#ifdef __cpp_lib_hardware_interference_size
    inline constexpr size_t ZRQUEUE_CACHE_LINE_SIZE = std::hardware_destructive_interference_size;
#else
    inline constexpr size_t ZRQUEUE_CACHE_LINE_SIZE = 64;
#endif

// ============================================================================
// [编译器宏与微架构级属性标记]
// ============================================================================
#if defined(__GNUC__) || defined(__clang__)
    // GCC 与 Clang (包含 Windows 下的 Clang-CL)
    #define ZRQUEUE_LIKELY(x)    __builtin_expect(!!(x), 1)
    #define ZRQUEUE_UNLIKELY(x)  __builtin_expect(!!(x), 0)
    #define ZRQUEUE_FORCE_INLINE inline __attribute__((always_inline))
#else
    #define ZRQUEUE_LIKELY(x)    (x)
    #define ZRQUEUE_UNLIKELY(x)  (x)
    #if defined(_MSC_VER)
        // 纯血原生 MSVC 编译器
        #define ZRQUEUE_FORCE_INLINE __forceinline
    #else
        // 未知编译器兜底 fallback
        #define ZRQUEUE_FORCE_INLINE inline
    #endif
#endif

// ============================================================================
// [微架构硬件级指令宏 (跨 CPU 架构与编译器完美兼容版)]
// ============================================================================
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
    // x86 / x64 架构
    #if defined(_MSC_VER)
        // Windows MSVC 编译器
        #include <intrin.h>
    #else
        // GCC / Clang (Linux, macOS, MinGW)
        #include <immintrin.h>
    #endif
    #define ZRQUEUE_CPU_PAUSE() _mm_pause()
#elif defined(__aarch64__) || defined(_M_ARM64)
    // ARM64 架构 (Apple Silicon, AWS Graviton, 鲲鹏)
    #if defined(_MSC_VER)
        #include <intrin.h>
        #define ZRQUEUE_CPU_PAUSE() __yield()
    #else
        #define ZRQUEUE_CPU_PAUSE() asm volatile("yield" ::: "memory")
    #endif
#else
    // 未知架构兜底 (退化为空操作)
    #define ZRQUEUE_CPU_PAUSE()
#endif

namespace zrqueue {

    // ============================================================================
    // [底层内存/数学辅助函数]
    // ============================================================================

    // 向上取整到 2 的次幂 (用于确保掩码寻址无分支)
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

    // 向上取整到指定的边界 (边界必须是 2 的次幂)
    inline constexpr size_t align_up(size_t size, size_t alignment) noexcept {
        return (size + alignment - 1) & ~(alignment - 1);
    }

    // 跨平台探测系统大页大小
    inline size_t get_hugepage_size() noexcept {
        static size_t hp_size = 0;
        if (hp_size != 0) {
            return hp_size;
        }
#if defined(ZRQUEUE_OS_WINDOWS)
        hp_size = GetLargePageMinimum();
        if (hp_size == 0) {
            hp_size = 1; // 兜底防除 0
        }
#elif defined(ZRQUEUE_OS_POSIX)
        hp_size = 2 * 1024 * 1024; // Linux 默认回退到 2MB
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
    // [定制化内存分配器 Allocators]
    // ============================================================================

    /**
     * @brief 强对齐分配器：彻底消灭首部与尾部伪共享
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
            // 【核心防御】：将分配大小向上取整到缓存行的倍数，操作系统绝不会在尾巴上塞无关变量
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

        void deallocate(T* p, std::size_t) noexcept {
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
     * @brief 大页分配器：彻底消灭海量队列情况下的 TLB Miss
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
            void *ptr = nullptr;
            size_t hp_size = get_hugepage_size();

#if defined(ZRQUEUE_OS_POSIX)
            // 【核心防御】：强制向大页大小对齐，防止内核 mmap 越界或分配失败
            size_t aligned_size = align_up(raw_size, hp_size);
            ptr = mmap(nullptr, aligned_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
            if (ptr == MAP_FAILED) {
                // 大页失败，降级为普通 mmap，但仍使用相同的 alignment 对齐
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
                // 优雅降级:退回普通 4KB 内存页
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

        void deallocate(T* p, std::size_t n) noexcept {
            if (!p) {
                return;
            }

#if defined(ZRQUEUE_OS_POSIX)
            size_t raw_size = n * sizeof(T);
            // 释放时同样必须严格按分配时的对齐大小释放，防止内存泄漏
            size_t aligned_size = align_up(raw_size, get_hugepage_size());
            munmap(p, aligned_size);
#elif defined(ZRQUEUE_OS_WINDOWS)
            VirtualFree(p, 0, MEM_RELEASE);
#endif
        }
    };

    // 分配器比较
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
    // [极速无锁队列 SpscQueue]
    // ============================================================================
    template <typename T, typename Allocator = AlignedAllocator<T, ZRQUEUE_CACHE_LINE_SIZE>>
    class SpscQueue {
    public:
        explicit SpscQueue(const uint32_t capacity, const Allocator& allocator = Allocator())
            : capacity_(normalize_size(capacity)), allocator_(allocator) {
            mask_ = capacity_ - 1;

            // 分配物理连续内存。因为使用了定制 Allocator，首尾伪共享已经被完美解决，不需要再手工+Padding
            slots_ = std::allocator_traits<Allocator>::allocate(allocator_, capacity_);
        }

        ~SpscQueue() {
            while (front()) {
                pop();
            }
            std::allocator_traits<Allocator>::deallocate(allocator_, slots_, capacity_);
        }

        SpscQueue(const SpscQueue&) = delete;
        SpscQueue& operator=(const SpscQueue&) = delete;

        template <typename... Args>
        ZRQUEUE_FORCE_INLINE void emplace(Args&&...args) noexcept(std::is_nothrow_constructible<T, Args&&...>::value) {
            static_assert(std::is_constructible<T, Args&&...>::value, "T must be constructible with Args&&...");

            const uint64_t write_index = write_index_.load(std::memory_order_relaxed);
            const uint64_t next_write_index = write_index + 1;

            // 【机制】：仅在本地缓存下标认为满了时，才跨核拉取最新消费者下标 (Cache Ping-Pong 防御)
            while (ZRQUEUE_UNLIKELY(next_write_index - cached_read_index_ > capacity_)) {
                cached_read_index_ = read_index_.load(std::memory_order_acquire);
            }

            // 【机制】：纯位运算寻址 + 定位构造 (Zero-Branching & Zero-Initialization Penalty)
            new (&slots_[write_index & mask_]) T(std::forward<Args>(args)...);
            write_index_.store(next_write_index, std::memory_order_release);
        }

        template <typename... Args>
        ZRQUEUE_NODISCARD ZRQUEUE_FORCE_INLINE bool try_emplace(Args&&...args) noexcept(std::is_nothrow_constructible<T, Args&&...>::value) {
            static_assert(std::is_constructible<T, Args&&...>::value, "T must be constructible with Args&&...");
            const uint64_t write_index = write_index_.load(std::memory_order_relaxed);

            // 快速检查：仅比较本地缓存下标
            if (ZRQUEUE_LIKELY(write_index - cached_read_index_ < capacity_)) {
                new (&slots_[write_index & mask_]) T(std::forward<Args>(args)...);
                write_index_.store(write_index + 1, std::memory_order_release);
                return true;
            }

            // 慢速路径：更新缓存读下标
            cached_read_index_ = read_index_.load(std::memory_order_acquire);
            if (write_index - cached_read_index_ < capacity_) {
                new (&slots_[write_index & mask_]) T(std::forward<Args>(args)...);
                write_index_.store(write_index + 1, std::memory_order_release);
                return true;
            }

            return false;
        }

        ZRQUEUE_FORCE_INLINE void push(const T& v) noexcept(std::is_nothrow_copy_constructible<T>::value) {
            emplace(v);
        }

        template <typename P, typename = std::enable_if_t<std::is_constructible_v<T, P&&>>>
        ZRQUEUE_FORCE_INLINE void push(P&& v) noexcept(std::is_nothrow_constructible_v<T, P&&>) {
            emplace(std::forward<P>(v));
        }

        ZRQUEUE_NODISCARD ZRQUEUE_FORCE_INLINE bool try_push(const T& v) noexcept(std::is_nothrow_copy_constructible<T>::value) {
            return try_emplace(v);
        }

        template <typename P, typename = std::enable_if_t<std::is_constructible_v<T, P&&>>>
        ZRQUEUE_NODISCARD ZRQUEUE_FORCE_INLINE bool try_push(P&& v) noexcept(std::is_nothrow_constructible_v<T, P&&>) {
            return try_emplace(std::forward<P>(v));
        }

        // 获取数据指针，用于 In-place 原地处理，实现绝对零拷贝
        ZRQUEUE_NODISCARD ZRQUEUE_FORCE_INLINE T* front() noexcept {
            const uint64_t read_index = read_index_.load(std::memory_order_relaxed);
            if (ZRQUEUE_LIKELY(read_index < cached_write_index_)) {
                return &slots_[read_index & mask_];
            }
            cached_write_index_ = write_index_.load(std::memory_order_acquire);
            if (ZRQUEUE_LIKELY(read_index < cached_write_index_)) {
                return &slots_[read_index & mask_];
            }

            return nullptr;
        }

        // ------------------------------------------------------------------------
        // 极速防热自旋读取 (Spin Front)
        // 适用场景：策略核心主循环，死等行情数据。
        // 核心优势：在 100% 空转死循环中加入 _mm_pause，保护 CPU 不掉频。
        // ------------------------------------------------------------------------
        ZRQUEUE_NODISCARD T* spin_front() noexcept {
            const uint64_t read_index = read_index_.load(std::memory_order_relaxed);
            if (ZRQUEUE_LIKELY(read_index < cached_write_index_)) {
                return &slots_[read_index & mask_];
            }

            // 第二步：没查到，进入跨核拉取循环等模式
            while (true) {
                cached_write_index_ = write_index_.load(std::memory_order_acquire);
                if (ZRQUEUE_LIKELY(read_index < cached_write_index_)) {
                    return &slots_[read_index & mask_];
                }

                ZRQUEUE_CPU_PAUSE();
            }
        }

        // 数据处理完后弹出，手工析构生命周期
        ZRQUEUE_FORCE_INLINE void pop() noexcept {
            static_assert(std::is_nothrow_destructible<T>::value, "T must be nothrow destructible");
            const uint64_t read_index = read_index_.load(std::memory_order_relaxed);
            if constexpr (!std::is_trivially_destructible_v<T>) {
                slots_[read_index & mask_].~T();
            }
            read_index_.store(read_index + 1, std::memory_order_release);
        }

        // 允许查看当前读取游标往后 offset 位置的元素，但不移动游标
        ZRQUEUE_NODISCARD ZRQUEUE_FORCE_INLINE T* peek(size_t offset = 0) noexcept {
            const uint64_t read_index = read_index_.load(std::memory_order_relaxed);
            uint64_t new_read_index = read_index + offset;

            // 确保加上 offset 后，依然没有越过生产者写入的位置
            if (ZRQUEUE_LIKELY(new_read_index < cached_write_index_)) {
                return &slots_[new_read_index & mask_];
            }
            cached_write_index_ = write_index_.load(std::memory_order_acquire);
            if (new_read_index < cached_write_index_) {
                return &slots_[new_read_index & mask_];
            }

            return nullptr;
        }

        // ------------------------------------------------------------------------
        // 批量写入 (Bulk Push)
        // 适用场景：网关通过 UDP recvmmsg 一次性收到 10 个行情报文，瞬间全塞入队列。
        // 核心优势：无论写入多少个元素，只执行【1次】原子的 Release 屏障！
        // ------------------------------------------------------------------------
        template <typename Iterator>
        ZRQUEUE_NODISCARD size_t push_bulk(Iterator first, size_t count) noexcept {
            if (ZRQUEUE_UNLIKELY(count == 0)) {
                return 0;
            }
            const uint64_t write_index = write_index_.load(std::memory_order_relaxed);
            uint64_t new_write_index = write_index + count;

            // 1. 检查是否有足够空间容纳 count 个元素
            if (ZRQUEUE_UNLIKELY(new_write_index - cached_read_index_ > capacity_)) {
                cached_read_index_ = read_index_.load(std::memory_order_acquire);
                // 真实空间依然不够，计算最大可写入数量 (Partial Push)
                if (new_write_index - cached_read_index_ > capacity_) {
                    count = capacity_ - (write_index - cached_read_index_);
                    if (count == 0) {
                        return 0; // 队列全满
                    }
                }
            }

            // 2. 连续定位构造 (没有任何原子锁的纯内存操作)
            for (size_t i = 0; i < count; ++i) {
                new (&slots_[(write_index + i) & mask_]) T(*first++);
            }

            // 3. 批量写入完毕，仅需 1 次原子提交！
            write_index_.store(write_index + count, std::memory_order_release);
            return count; // 返回实际写入的个数
        }

        // ------------------------------------------------------------------------
        // 批量零拷贝消费 (Bulk Consume)
        // 适用场景：策略线程醒来发现队列积压了多个数据，一次性全量处理。
        // 核心优势：传递 Lambda 表达式进行原地 Zero-Copy 处理，最后只执行【1次】Release 屏障！
        // max_count 默认为 0，表示“有多少吃多少”。
        // ------------------------------------------------------------------------
        template <typename THandler>
        ZRQUEUE_NODISCARD size_t consume_bulk(THandler&& handler, size_t max_count = 0) noexcept {
            const uint64_t read_index = read_index_.load(std::memory_order_relaxed);

            // 1. 获取当前共有多少积压数据
            if (ZRQUEUE_UNLIKELY(read_index == cached_write_index_)) {
                cached_write_index_ = write_index_.load(std::memory_order_acquire);
                if (read_index == cached_write_index_) {
                    return 0; // 队列空
                }
            }

            size_t available = cached_write_index_ - read_index;
            size_t count_to_consume = (max_count == 0 || max_count > available) ? available : max_count;

            // 2. 连续处理数据并就地析构
            for (size_t i = 0; i < count_to_consume; ++i) {
                size_t current_offset = (read_index + i) & mask_;
                // 将数据的引用直接扔给用户的 lambda 函数处理 (绝无数据复制)
                std::forward<THandler>(handler)(slots_[current_offset]);

                if constexpr (!std::is_trivially_destructible_v<T>) {
                    // 处理完后手工销毁
                    slots_[current_offset].~T();
                }
            }

            // 3. 批量处理完毕，仅需 1 次原子释放！
            read_index_.store(read_index + count_to_consume, std::memory_order_release);
            return count_to_consume; // 返回实际处理的个数
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
        // 【物理内存布局】: 教科书级的伪共享隔离方案 (Cache Coherence Ping-Pong Defense)
        // ========================================================================

        // 【缓存行】：生产者专区。写线程疯狂操作，读线程偶尔读取
        alignas(ZRQUEUE_CACHE_LINE_SIZE) std::atomic<uint64_t> write_index_ { 0 };
        alignas(ZRQUEUE_CACHE_LINE_SIZE) uint64_t cached_read_index_ { 0 };

        // 【缓存行】：消费者专区。读线程疯狂操作，写线程偶尔读取
        alignas(ZRQUEUE_CACHE_LINE_SIZE) std::atomic<uint64_t> read_index_ { 0 };
        alignas(ZRQUEUE_CACHE_LINE_SIZE) uint64_t cached_write_index_ { 0 };

        // 【缓存行】：冷数据区。盘中绝对不变的元数据，远离热点指针区
        alignas(ZRQUEUE_CACHE_LINE_SIZE) uint32_t capacity_ { 0 };
        uint32_t mask_ { 0 };
        T *slots_ { nullptr };

        // 【缓存行】：分配器占据独立空间
        alignas(ZRQUEUE_CACHE_LINE_SIZE) Allocator allocator_ { };
    };  // end SpscQueue

    /**
     * @brief 极致内嵌的 SPSC 队列 (In-place)
     * 警告：由于数据内嵌在对象内部，如果 N 非常大，对象体积将极其庞大。
     * 绝对不要将其分配在线程栈（Stack）上！必须声明为全局变量(static) 或通过 new 分配在堆上。
     * @tparam T 元素类型
     * @tparam N 队列容量 (【必须】是 2 的次幂)
     */
    template <typename T, uint32_t N>
    class SpscInlineQueue {
        // 【编译期防御】：强行要求 N 是 2 的次幂，从根源消灭运行期位运算的风险
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

        template <typename... Args>
        ZRQUEUE_FORCE_INLINE void emplace(Args&&... args) noexcept(std::is_nothrow_constructible<T, Args&&...>::value) {
            static_assert(std::is_constructible<T, Args&&...>::value, "T must be constructible with Args&&...");
            const uint64_t write_index = write_index_.load(std::memory_order_relaxed);
            uint64_t next_write_index  = write_index + 1;
            while (ZRQUEUE_UNLIKELY(next_write_index - cached_read_index_ > N)) {
                cached_read_index_ = read_index_.load(std::memory_order_acquire);
            }

            // 极致基址寻址】：不再解引用 slots_ 指针，直接基于对象的 this 指针加上偏移量计算目标物理地址！
            new (&reinterpret_cast<T*>(slots_memory_)[write_index & MASK]) T(std::forward<Args>(args)...);
            write_index_.store(next_write_index, std::memory_order_release);
        }

        template <typename... Args>
        ZRQUEUE_FORCE_INLINE bool try_emplace(Args&&... args) noexcept(std::is_nothrow_constructible<T, Args&&...>::value) {
            const uint64_t write_index = write_index_.load(std::memory_order_relaxed);
            if (ZRQUEUE_LIKELY(write_index - cached_read_index_ < N)) {
                new (&reinterpret_cast<T*>(slots_memory_)[write_index & MASK]) T(std::forward<Args>(args)...);
                write_index_.store(write_index + 1, std::memory_order_release);
                return true;
            }
            cached_read_index_ = read_index_.load(std::memory_order_acquire);
            if (write_index_ - cached_read_index_ < N) {
                new (&reinterpret_cast<T*>(slots_memory_)[write_index & MASK]) T(std::forward<Args>(args)...);
                write_index_.store(write_index + 1, std::memory_order_release);
                return true;
            }

            return false;
        }

        ZRQUEUE_FORCE_INLINE void push(const T& v) noexcept(std::is_nothrow_copy_constructible<T>::value) {
            emplace(v);
        }

        template <typename P, typename = std::enable_if_t<std::is_constructible_v<T, P&&>>>
        ZRQUEUE_FORCE_INLINE void push(P&& v) noexcept(std::is_nothrow_constructible_v<T, P&&>) {
            emplace(std::forward<P>(v));
        }

        ZRQUEUE_NODISCARD ZRQUEUE_FORCE_INLINE bool try_push(const T& v) noexcept(std::is_nothrow_copy_constructible<T>::value) {
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
            uint64_t new_write_index = write_index + count;
            if (ZRQUEUE_UNLIKELY(new_write_index - cached_read_index_ > N)) {
                cached_read_index_ = read_index_.load(std::memory_order_acquire);
                if (new_write_index - cached_read_index_ > N) {
                    count = N - (write_index - cached_read_index_);
                    if (count == 0) {
                        return 0;
                    }
                }
            }

            T *raw_array = reinterpret_cast<T*>(slots_memory_);
            for (size_t i = 0; i < count; ++i) {
                new (&raw_array[(write_index + i) & MASK]) T(*first++);
            }

            write_index_.store(write_index + count, std::memory_order_release);
            return count;
        }

        ZRQUEUE_NODISCARD ZRQUEUE_FORCE_INLINE T* front() noexcept {
            const uint64_t read_index = read_index_.load(std::memory_order_relaxed);
            if (ZRQUEUE_LIKELY(read_index < cached_write_index_)) {
                return &reinterpret_cast<T*>(slots_memory_)[read_index & MASK];
            }
            cached_write_index_ = write_index_.load(std::memory_order_acquire);
            if (read_index < cached_write_index_) {
                return &reinterpret_cast<T*>(slots_memory_)[read_index & MASK];
            }

            return nullptr;
        }

        ZRQUEUE_NODISCARD T* spin_front() noexcept {
            const uint64_t read_index = read_index_.load(std::memory_order_relaxed);
            if (ZRQUEUE_LIKELY(read_index < cached_write_index_)) {
                return &reinterpret_cast<T*>(slots_memory_)[read_index & MASK];
            }

            while (true) {
                cached_write_index_ = write_index_.load(std::memory_order_acquire);
                if (ZRQUEUE_LIKELY(read_index < cached_write_index_)) {
                    return &reinterpret_cast<T*>(slots_memory_)[read_index & MASK];
                }

                ZRQUEUE_CPU_PAUSE();
            }
        }

        ZRQUEUE_FORCE_INLINE void pop() noexcept {
            static_assert(std::is_nothrow_destructible<T>::value, "T must be nothrow destructible");
            const uint64_t read_index = read_index_.load(std::memory_order_relaxed);
            if constexpr (!std::is_trivially_destructible_v<T>) {
                reinterpret_cast<T*>(slots_memory_)[read_index & MASK].~T();
            }
            read_index_.store(read_index + 1, std::memory_order_release);
        }

        ZRQUEUE_NODISCARD ZRQUEUE_FORCE_INLINE T* peek(size_t offset = 0) noexcept {
            const uint64_t read_index = read_index_.load(std::memory_order_relaxed);
            uint64_t new_read_index = read_index + offset;
            if (ZRQUEUE_LIKELY(new_read_index < cached_write_index_)) {
                return &reinterpret_cast<T*>(slots_memory_)[new_read_index & MASK];
            }
            cached_write_index_ = write_index_.load(std::memory_order_acquire);
            if (new_read_index < cached_write_index_) {
                return &reinterpret_cast<T*>(slots_memory_)[new_read_index & MASK];
            }

            return nullptr;
        }

        template <typename THandler>
        ZRQUEUE_NODISCARD size_t consume_bulk(THandler&& handler, size_t max_count = 0) noexcept {
            const uint64_t read_index = read_index_.load(std::memory_order_relaxed);
            if (ZRQUEUE_UNLIKELY(read_index == cached_write_index_)) {
                cached_write_index_ = write_index_.load(std::memory_order_acquire);
                if (read_index == cached_write_index_) {
                    return 0;
                }
            }

            size_t available = cached_write_index_ - read_index;
            size_t count_to_consume = (max_count == 0 || max_count > available) ? available : max_count;

            T *raw_array = reinterpret_cast<T*>(slots_memory_);
            for (size_t i = 0; i < count_to_consume; ++i) {
                size_t current_offset = (read_index + i) & MASK;
                std::forward<THandler>(handler)(raw_array[current_offset]);

                if constexpr (!std::is_trivially_destructible_v<T>) {
                    raw_array[current_offset].~T(); // 原地析构
                }
            }

            read_index_.store(read_index + count_to_consume, std::memory_order_release);
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

        // 【缓存行】：生产者专区
        alignas(ZRQUEUE_CACHE_LINE_SIZE) std::atomic<uint64_t> write_index_ { 0 };
        alignas(ZRQUEUE_CACHE_LINE_SIZE) uint64_t cached_read_index_ { 0 };

        // 【缓存行】：消费者专区
        alignas(ZRQUEUE_CACHE_LINE_SIZE) std::atomic<uint64_t> read_index_ { 0 };
        alignas(ZRQUEUE_CACHE_LINE_SIZE) uint64_t cached_write_index_ { 0 };

        // 【缓存行】：纯内嵌数据区 (In-place Memory Array)
        // 1. alignas(ZRQUEUE_CACHE_LINE_SIZE) 强制数据区的首地址与上方的索引区彻底物理切断。
        // 2. alignas(T) 确保强制类型转换 reinterpret_cast<T*> 是对齐安全的。
        // 3. std::byte 确保这是一块不调用任何构造函数的“纯粹垃圾内存”，仅在 push 时现场构造。
        alignas(ZRQUEUE_CACHE_LINE_SIZE) alignas(T) std::byte slots_memory_[N * sizeof(T)];
    }; // end SpscInlineQueue

    //编译期定长，预先构造对象，零拷贝读写，适合 Disruptor 模式
    template<class T, uint32_t N>
    class SpscRingBuffer {
        static_assert(N >= 2, "Capacity must be at least 2");
        static_assert((N & (N - 1)) == 0, "N must be a power of 2");
        //static_assert(std::is_trivially_destructible<T>::value, 
        //    "SpscRingBuffer is designed for POD types to prevent delayed resource deallocation.");
    public:
        SpscRingBuffer() noexcept = default;
        ~SpscRingBuffer() noexcept = default;

        // non-copyable and non-movable
        SpscRingBuffer(const SpscRingBuffer&) = delete;
        SpscRingBuffer& operator=(const SpscRingBuffer&) = delete;

        ZRQUEUE_NODISCARD ZRQUEUE_FORCE_INLINE T* alloc() {
            const uint64_t write_index = write_index_.load(std::memory_order_relaxed);
            if (ZRQUEUE_LIKELY(write_index - cached_read_index_ < N)) {
                return &data_[write_index & MASK];
            }
            cached_read_index_ = read_index_.load(std::memory_order_acquire);
            if (ZRQUEUE_LIKELY(write_index - cached_read_index_ < N)) {
                return &data_[write_index & MASK];
            }

            return nullptr;
        }

        ZRQUEUE_FORCE_INLINE void push() {
            const uint64_t write_index = write_index_.load(std::memory_order_relaxed);
            write_index_.store(write_index + 1, std::memory_order_release);
        }

        template<typename Writer>
        ZRQUEUE_NODISCARD bool try_push(Writer writer) {
            T *p = alloc();
            if (!p) {
                return false;
            }

            writer(p);
            push();
            return true;
        }

        template<typename Writer>
        void push(Writer writer) {
            while (!try_push(writer)) {
            }
        }

        ZRQUEUE_NODISCARD ZRQUEUE_FORCE_INLINE T* front() {
            const uint64_t read_index = read_index_.load(std::memory_order_relaxed);
            if (ZRQUEUE_LIKELY(read_index < cached_write_index_)) {
                return &data_[read_index & MASK];
            }
            cached_write_index_ = write_index_.load(std::memory_order_acquire);
            if (ZRQUEUE_LIKELY(read_index < cached_write_index_)) {
                return &data_[read_index & MASK];
            }

            return nullptr;
        }

        ZRQUEUE_NODISCARD T* spin_front() noexcept {
            const uint64_t read_index = read_index_.load(std::memory_order_relaxed);
            if (ZRQUEUE_LIKELY(read_index < cached_write_index_)) {
                return &data_[read_index & MASK];
            }

            while (true) {
                cached_write_index_ = write_index_.load(std::memory_order_acquire);
                if (ZRQUEUE_LIKELY(read_index < cached_write_index_)) {
                    return &data_[read_index & MASK];
                }

                ZRQUEUE_CPU_PAUSE();
            }
        }

        ZRQUEUE_FORCE_INLINE void pop() {
            const uint64_t read_index = read_index_.load(std::memory_order_relaxed);
            read_index_.store(read_index + 1, std::memory_order_release);
        }

        ZRQUEUE_NODISCARD T* peek(size_t offset = 0) noexcept {
            const uint64_t read_index = read_index_.load(std::memory_order_relaxed);
            uint64_t new_read_index = read_index + offset;
            if (ZRQUEUE_LIKELY(new_read_index < cached_write_index_)) {
                return &data_[new_read_index & MASK];
            }
            cached_write_index_ = write_index_.load(std::memory_order_acquire);
            if (ZRQUEUE_LIKELY(new_read_index < cached_write_index_)) {
                return &data_[new_read_index & MASK];
            }

            return nullptr;
        }

        template<typename Reader>
        ZRQUEUE_NODISCARD bool try_pop(Reader reader) {
            T *v = front();
            if (!v) {
                return false;
            }
            reader(v);
            pop();
            return true;
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

        // 【缓存行】：生产者专区
        alignas(ZRQUEUE_CACHE_LINE_SIZE) std::atomic<uint64_t> write_index_{ 0 };
        alignas(ZRQUEUE_CACHE_LINE_SIZE) uint64_t cached_read_index_ { 0 };

        // 【缓存行】：消费者专区
        alignas(ZRQUEUE_CACHE_LINE_SIZE) std::atomic<uint64_t> read_index_{ 0 };
        alignas(ZRQUEUE_CACHE_LINE_SIZE) uint64_t cached_write_index_ { 0 };

        // 数据区
        alignas(ZRQUEUE_CACHE_LINE_SIZE) std::array<T, N> data_ { };

    };  // end SpscRingBuffer

}  // end namespace zrqueue