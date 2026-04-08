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

#define ZRQUEUE_VERSION 10202 // 1.2.2

#include <atomic>
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

#ifndef MAP_HUGETLB
#define MAP_HUGETLB 0x40000 
#endif
#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS 0x20
#endif

// ============================================================================
// [编译器宏]
// ============================================================================
// 1. 获取 CPU L1 缓存行大小 (防伪共享核心参数)
#ifdef __cpp_lib_hardware_interference_size
inline constexpr size_t ZRQUEUE_CACHE_LINE_SIZE = std::hardware_destructive_interference_size;
#else
inline constexpr size_t ZRQUEUE_CACHE_LINE_SIZE = 64;
#endif

// 2. 静态分支预测 (将极小概率事件踢出 CPU 指令预取流水线)
#if defined(__GNUC__) || defined(__clang__)
#define ZRQUEUE_LIKELY(x)   __builtin_expect(!!(x), 1)
#define ZRQUEUE_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define ZRQUEUE_LIKELY(x)   (x)
#define ZRQUEUE_UNLIKELY(x) (x)
#endif

// ============================================================================
// [微架构硬件级指令宏 (跨 CPU 架构与编译器完美兼容版)]
// ============================================================================
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
    // x86 / x64 架构
    #if defined(_MSC_VER)
        // Windows MSVC 编译器
        #include <intrin.h>
        #define ZRQUEUE_CPU_PAUSE() _mm_pause()
    #else
        // GCC / Clang (Linux, macOS, MinGW)
        #include <immintrin.h>
        #define ZRQUEUE_CPU_PAUSE() _mm_pause()
    #endif
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
            void* ptr = nullptr;
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
    // [极速无锁队列 SPSCQueue]
    // ============================================================================
    template <typename T, typename Allocator = AlignedAllocator<T, ZRQUEUE_CACHE_LINE_SIZE>>
    class SPSCQueue {
    public:
        explicit SPSCQueue(const uint32_t capacity, const Allocator& allocator = Allocator())
            : capacity_(normalize_size(capacity)), allocator_(allocator) {
            mask_ = capacity_ - 1;
            // 分配物理连续内存。因为使用了定制 Allocator，首尾伪共享已经被完美解决，不需要再手工 +Padding
            slots_ = std::allocator_traits<Allocator>::allocate(allocator_, capacity_);
        }

        ~SPSCQueue() {
            while (front()) {
                pop();
            }
            std::allocator_traits<Allocator>::deallocate(allocator_, slots_, capacity_);
        }

        SPSCQueue(const SPSCQueue&) = delete;
        SPSCQueue& operator=(const SPSCQueue&) = delete;

        template <typename... Args>
        void emplace(Args&&...args) noexcept(std::is_nothrow_constructible<T, Args&&...>::value) {
            static_assert(std::is_constructible<T, Args&&...>::value, "T must be constructible with Args&&...");

            auto const write_index = write_index_.load(std::memory_order_relaxed);
            auto next_write_index  = write_index + 1;

            // 【机制】：仅在本地缓存认为满了时，才跨核拉取最新消费者索引 (Cache Ping-Pong 防御)
            while (ZRQUEUE_UNLIKELY(next_write_index - cached_read_index_ > capacity_)) {
                cached_read_index_ = read_index_.load(std::memory_order_acquire);
            }

            // 【机制】：纯位运算寻址 + 定位构造 (Zero-Branching & Zero-Initialization Penalty)
            new (&slots_[write_index & mask_]) T(std::forward<Args>(args)...);
            write_index_.store(next_write_index, std::memory_order_release);
        }

        template <typename... Args>
        bool try_emplace(Args&&...args) noexcept(std::is_nothrow_constructible<T, Args&&...>::value) {
            static_assert(std::is_constructible<T, Args&&...>::value, "T must be constructible with Args&&...");

            auto const write_index = write_index_.load(std::memory_order_relaxed);
            auto next_write_index  = write_index + 1;

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

        void push(const T& v) noexcept(std::is_nothrow_copy_constructible<T>::value) {
            emplace(v);
        }

        template <typename P, typename = typename std::enable_if<std::is_constructible<T, P&&>::value>::type>
        void push(P&& v) noexcept(std::is_nothrow_constructible<T, P&&>::value) {
            emplace(std::forward<P>(v));
        }

        bool try_push(const T& v) noexcept(std::is_nothrow_copy_constructible<T>::value) {
            return try_emplace(v);
        }

        template <typename P, typename = typename std::enable_if<std::is_constructible<T, P&&>::value>::type>
        bool try_push(P&& v) noexcept(std::is_nothrow_constructible<T, P&&>::value) {
            return try_emplace(std::forward<P>(v));
        }

        // ------------------------------------------------------------------------
        // 极速防热自旋读取 (Spin Front)
        // 适用场景：策略核心主循环，死等行情数据。
        // 核心优势：在 100% 空转死循环中加入 _mm_pause，保护 CPU 不掉频。
        // ------------------------------------------------------------------------
        T* spin_front() noexcept {
            while (true) {
                // 1. 先用最低开销检查本地缓存
                auto const read_index = read_index_.load(std::memory_order_relaxed);
                if (ZRQUEUE_LIKELY(read_index != cached_write_index_)) {
                    return &slots_[read_index & mask_];
                }

                // 2. 本地没数据，跨核拉取最新状态
                cached_write_index_ = write_index_.load(std::memory_order_acquire);
                if (ZRQUEUE_LIKELY(read_index != cached_write_index_)) {
                    return &slots_[read_index & mask_];
                }

                // 3. 真的没数据！执行底层硬件级暂停，防止 CPU 算术逻辑单元(ALU)烧毁降频
                ZRQUEUE_CPU_PAUSE();
            }
        }

        // 获取数据指针，用于 In-place 原地处理，实现绝对零拷贝
        T* front() noexcept {
            auto const read_index = read_index_.load(std::memory_order_relaxed);
            if (read_index == cached_write_index_) {
                cached_write_index_ = write_index_.load(std::memory_order_acquire);
                if (cached_write_index_ == read_index) {
                    return nullptr;
                }
            }
            return &slots_[read_index & mask_];
        }

        // 允许查看当前读取游标往后 offset 位置的元素，但不移动游标
        T* peek(size_t offset = 0) noexcept {
            auto const read_index = read_index_.load(std::memory_order_relaxed);

            // 确保加上 offset 后，依然没有越过生产者写入的位置
            if (read_index + offset >= cached_write_index_) {
                cached_write_index_ = write_index_.load(std::memory_order_acquire);
                if (read_index + offset >= cached_write_index_) {
                    return nullptr; // 没有那么多数据
                }
            }
            return &slots_[(read_index + offset) & mask_];
        }

        // 数据处理完后弹出，手工析构生命周期
        void pop() noexcept {
            static_assert(std::is_nothrow_destructible<T>::value, "T must be nothrow destructible");
            auto const read_index = read_index_.load(std::memory_order_relaxed);

            assert(write_index_.load(std::memory_order_acquire) != read_index &&
                "Can only call pop() after front() has returned a non-nullptr");

            slots_[read_index & mask_].~T();
            read_index_.store(read_index + 1, std::memory_order_release);
        }

        // ------------------------------------------------------------------------
        // 批量写入 (Bulk Push)
        // 适用场景：网关通过 UDP recvmmsg 一次性收到 10 个行情报文，瞬间全塞入队列。
        // 核心优势：无论写入多少个元素，只执行【1次】原子的 Release 屏障！
        // ------------------------------------------------------------------------
        template <typename Iterator>
        size_t push_bulk(Iterator first, size_t count) noexcept {
            if (count == 0) {
                return 0;
            }

            auto const write_index = write_index_.load(std::memory_order_relaxed);

            // 1. 检查是否有足够空间容纳 count 个元素
            if (ZRQUEUE_UNLIKELY(write_index + count - cached_read_index_ > capacity_)) {
                cached_read_index_ = read_index_.load(std::memory_order_acquire);
                // 真实空间依然不够，计算最大可写入数量 (Partial Push)
                if (write_index + count - cached_read_index_ > capacity_) {
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
        template <typename TfnHandler>
        size_t consume_bulk(TfnHandler&& handler, size_t max_count = 0) noexcept {
            auto const read_index = read_index_.load(std::memory_order_relaxed);

            // 1. 获取当前共有多少积压数据
            if (read_index == cached_write_index_) {
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
                std::forward<TfnHandler>(handler)(slots_[current_offset]);
                // 处理完后手工销毁
                slots_[current_offset].~T();
            }

            // 3. 批量处理完毕，仅需 1 次原子释放！
            read_index_.store(read_index + count_to_consume, std::memory_order_release);
            return count_to_consume; // 返回实际处理的个数
        }

        size_t size() const noexcept {
            uint64_t w = write_index_.load(std::memory_order_acquire);
            uint64_t r = read_index_.load(std::memory_order_acquire);
            return (w >= r) ? static_cast<size_t>(w - r) : 0;
        }

        bool empty() const noexcept {
            return write_index_.load(std::memory_order_acquire) == read_index_.load(std::memory_order_acquire);
        }

        size_t capacity() const noexcept { 
            return capacity_; 
        }

    private:
        // ========================================================================
        // 【物理内存布局】: 教科书级的伪共享隔离方案 (Cache Coherence Ping-Pong Defense)
        // ========================================================================

        // 【缓存行】：生产者专区。写线程疯狂操作，读线程偶尔读取
        alignas(ZRQUEUE_CACHE_LINE_SIZE) std::atomic<uint64_t> write_index_{ 0 };
        alignas(ZRQUEUE_CACHE_LINE_SIZE) uint64_t cached_read_index_{ 0 };

        // 【缓存行】：消费者专区。读线程疯狂操作，写线程偶尔读取
        alignas(ZRQUEUE_CACHE_LINE_SIZE) std::atomic<uint64_t> read_index_{ 0 };
        alignas(ZRQUEUE_CACHE_LINE_SIZE) uint64_t cached_write_index_{ 0 };

        // 【缓存行】：冷数据区。盘中绝对不变的元数据，远离热点指针区
        alignas(ZRQUEUE_CACHE_LINE_SIZE) uint32_t capacity_ { 0 };
        uint32_t mask_{ 0 };
        T* slots_{ nullptr };

        // 【缓存行】：分配器占据独立空间
        alignas(ZRQUEUE_CACHE_LINE_SIZE) Allocator allocator_ {};
    };

    /**
     * @brief 极致内嵌的 SPSC 队列 (Static / In-place)
     *
     * 警告：由于数据内嵌在对象内部，如果 N 非常大，对象体积将极其庞大。
     * 绝对不要将其分配在线程栈（Stack）上！必须声明为全局变量(static) 或通过 new 分配在堆上。
     *
     * @tparam T 元素类型
     * @tparam N 队列容量 (【必须】是 2 的次幂)
     */
    template <typename T, uint32_t N>
    class SPSCStaticQueue {
        // 【编译期防御】：强行要求 N 是 2 的次幂，从根源消灭运行期位运算的风险
        static_assert(N >= 2, "Capacity must be at least 2");
        static_assert((N & (N - 1)) == 0, "Capacity N must be a power of 2 for bitwise masking");

    public:
        SPSCStaticQueue() noexcept = default;

        ~SPSCStaticQueue() {
            while (front()) {
                pop();
            }
        }

        // non-copyable and non-movable
        SPSCStaticQueue(const SPSCStaticQueue&) = delete;
        SPSCStaticQueue& operator=(const SPSCStaticQueue&) = delete;

        template <typename... Args>
        void emplace(Args&&... args) noexcept(std::is_nothrow_constructible<T, Args&&...>::value) {
            static_assert(std::is_constructible<T, Args&&...>::value, "T must be constructible with Args&&...");

            auto const write_index = write_index_.load(std::memory_order_relaxed);
            auto next_write_index = write_index + 1;

            while (ZRQUEUE_UNLIKELY(next_write_index - cached_read_index_ > N)) {
                cached_read_index_ = read_index_.load(std::memory_order_acquire);
            }

            // 【极致基址寻址】：不再解引用 slots_ 指针，直接基于对象的 this 指针加上偏移量计算目标物理地址！
            new (&reinterpret_cast<T*>(slots_memory_)[write_index & MASK]) T(std::forward<Args>(args)...);

            write_index_.store(next_write_index, std::memory_order_release);
        }

        template <typename... Args>
        bool try_emplace(Args&&... args) noexcept(std::is_nothrow_constructible<T, Args&&...>::value) {
            auto const write_index = write_index_.load(std::memory_order_relaxed);
            auto next_write_index = write_index + 1;

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

        void push(const T& v) noexcept(std::is_nothrow_copy_constructible<T>::value) {
            emplace(v);
        }

        bool try_push(const T& v) noexcept(std::is_nothrow_copy_constructible<T>::value) {
            return try_emplace(v);
        }

        template <typename Iterator>
        size_t push_bulk(Iterator first, size_t count) noexcept {
            if (count == 0) {
                return 0;
            }

            auto const write_index = write_index_.load(std::memory_order_relaxed);
            if (ZRQUEUE_UNLIKELY(write_index + count - cached_read_index_ > N)) {
                cached_read_index_ = read_index_.load(std::memory_order_acquire);
                if (write_index + count - cached_read_index_ > N) {
                    count = N - (write_index - cached_read_index_);
                    if (count == 0) {
                        return 0;
                    }
                }
            }

            T* raw_array = reinterpret_cast<T*>(slots_memory_);
            for (size_t i = 0; i < count; ++i) {
                new (&raw_array[(write_index + i) & MASK]) T(*first++);
            }

            write_index_.store(write_index + count, std::memory_order_release);
            return count;
        }

        T* front() noexcept {
            auto const read_index = read_index_.load(std::memory_order_relaxed);
            if (read_index == cached_write_index_) {
                cached_write_index_ = write_index_.load(std::memory_order_acquire);
                if (cached_write_index_ == read_index) {
                    return nullptr;
                }
            }
            return &reinterpret_cast<T*>(slots_memory_)[read_index & MASK];
        }

        T* peek(size_t offset = 0) noexcept {
            auto const read_index = read_index_.load(std::memory_order_relaxed);

            if (read_index + offset >= cached_write_index_) {
                cached_write_index_ = write_index_.load(std::memory_order_acquire);
                if (read_index + offset >= cached_write_index_) {
                    return nullptr;
                }
            }
            return &reinterpret_cast<T*>(slots_memory_)[(read_index + offset) & MASK];
        }

        T* spin_front() noexcept {
            while (true) {
                auto const read_index = read_index_.load(std::memory_order_relaxed);
                if (ZRQUEUE_LIKELY(read_index != cached_write_index_)) {
                    return &reinterpret_cast<T*>(slots_memory_)[read_index & MASK];
                }

                cached_write_index_ = write_index_.load(std::memory_order_acquire);
                if (ZRQUEUE_LIKELY(read_index != cached_write_index_)) {
                    return &reinterpret_cast<T*>(slots_memory_)[read_index & MASK];
                }

                ZRQUEUE_CPU_PAUSE();
            }
        }

        void pop() noexcept {
            static_assert(std::is_nothrow_destructible<T>::value, "T must be nothrow destructible");
            auto const read_index = read_index_.load(std::memory_order_relaxed);

            assert(write_index_.load(std::memory_order_acquire) != read_index &&
                "Can only call pop() after front() has returned a non-nullptr");

            reinterpret_cast<T*>(slots_memory_)[read_index & MASK].~T();
            read_index_.store(read_index + 1, std::memory_order_release);
        }

        template <typename TfnHandler>
        size_t consume_bulk(TfnHandler&& handler, size_t max_count = 0) noexcept {
            auto const read_index = read_index_.load(std::memory_order_relaxed);

            if (read_index == cached_write_index_) {
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
                std::forward<TfnHandler>(handler)(raw_array[current_offset]);
                raw_array[current_offset].~T(); // 原地析构
            }

            read_index_.store(read_index + count_to_consume, std::memory_order_release);
            return count_to_consume;
        }

        size_t size() const noexcept {
            uint64_t w = write_index_.load(std::memory_order_acquire);
            uint64_t r = read_index_.load(std::memory_order_acquire);
            return (w >= r) ? static_cast<size_t>(w - r) : 0;
        }

        bool empty() const noexcept {
            return write_index_.load(std::memory_order_acquire) == read_index_.load(std::memory_order_acquire);
        }

        constexpr size_t capacity() const noexcept { 
            return N; 
        }

    private:
        static constexpr uint32_t MASK = N - 1;

        // 【缓存行】：生产者专区。
        alignas(ZRQUEUE_CACHE_LINE_SIZE) std::atomic<uint64_t> write_index_{ 0 };
        alignas(ZRQUEUE_CACHE_LINE_SIZE) uint64_t cached_read_index_ { 0 };

        // 【缓存行】：消费者专区。
        alignas(ZRQUEUE_CACHE_LINE_SIZE) std::atomic<uint64_t> read_index_{ 0 };
        alignas(ZRQUEUE_CACHE_LINE_SIZE) uint64_t cached_write_index_ { 0 };

        // 【缓存行】：纯内嵌数据区 (In-place Memory Array)
        // 1. alignas(ZRQUEUE_CACHE_LINE_SIZE) 强制数据区的首地址与上方的索引区彻底物理切断。
        // 2. alignas(T) 确保强制类型转换 reinterpret_cast<T*> 是对齐安全的。
        // 3. unsigned char 确保这是一块不调用任何构造函数的“纯粹垃圾内存”，仅在 push 时现场构造。
        alignas(ZRQUEUE_CACHE_LINE_SIZE) alignas(T) unsigned char slots_memory_[N * sizeof(T)];
    };

}  // namespace zrqueue