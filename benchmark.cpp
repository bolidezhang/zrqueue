// benchmark.cpp - Benchmark zrqueue vs rigtorp::SPSCQueue
// Supports Windows (MSVC/MinGW) and Linux (GCC/Clang)
//
// Methodology (single samples on a noisy/shared machine are meaningless):
//  - Interleaved execution: every rep runs ALL variants, canceling thermal drift
//    and "rigtorp always first" order bias.
//  - Multiple reps, reporting min / median / max. min = least-noise sample.
//  - Two capacities: 4096 exercises the full/empty slow paths (index-cache reloads,
//    spin loops) that a huge queue never touches; 16M is the classic setup.
//  - Consumers hold the pointer returned by front() (single call per poll). Calling
//    front() twice per element re-reads the slot line after the mirror path already
//    delivered the payload - neutral for classic queues (second call is a cheap
//    fast path), but it silently voids SpscMirrorQueue's index-line piggyback.
//
// Usage: benchmark [consumer-core producer-core] [options]
//   --help / -h       print full usage
//   --rt              raise priority to reduce OS scheduling jitter
//   --rtt-only        RTT percentile tables only
//   --depth-only      controlled-backlog + RingBuffer diagnostics only
//   --depth=N         single backlog depth (0 = uncontrolled, 0<N<32 clamped)
// Run with --help for the complete description and examples.

#include "zrqueue.h"
#include "rigtorp/SPSCQueue.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <thread>
#include <vector>

#include <cstring>

#include "tests/payloads.h"

// ========== High-resolution clock for percentile sampling ==========
// steady_clock/QPC is ~100ns granular on Windows, which flattens p50/p90/p99
// onto the same 100ns steps. TSC gives cycle-level resolution.
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
    #include <intrin.h>
    #define ZRQ_TSC_FENCE() _mm_lfence()
    #define ZRQ_TSC()       static_cast<uint64_t>(__rdtsc())
#elif (defined(__x86_64__) || defined(__i386__)) && (defined(__GNUC__) || defined(__clang__))
    #include <x86intrin.h>
    #define ZRQ_TSC_FENCE() _mm_lfence()
    #define ZRQ_TSC()       static_cast<uint64_t>(__rdtsc())
#else
    #define ZRQ_TSC_FENCE() ((void)0)
    #define ZRQ_TSC()       static_cast<uint64_t>(nowNs())
    #define ZRQ_TSC_IS_NS   1
#endif

static double g_ns_per_tsc = 1.0;
static std::atomic<bool> g_realtime{ false };

// ========== Cross-Platform Thread Pinning (CPU Affinity) ==========
#if defined(_WIN32)
    #include <windows.h>
    static void pinThread(int cpu) {
        if (cpu < 0) {
            return;
        }
        // Pin thread to the specified logical processor (supports up to the first 64 cores)
        DWORD_PTR mask = (cpu < 64) ? (DWORD_PTR)1 << cpu : (DWORD_PTR)-1;
        SetThreadAffinityMask(GetCurrentThread(), mask);
        if (g_realtime.load(std::memory_order_relaxed)) {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
        }
    }
#elif defined(__linux__)
    #include <pthread.h>
    #include <sched.h>
    static void pinThread(int cpu) {
        if (cpu < 0) {
            return;
        }
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(cpu, &cpuset);
        pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
        if (g_realtime.load(std::memory_order_relaxed)) {
            struct sched_param sp{};
            sp.sched_priority = sched_get_priority_max(SCHED_FIFO);
            pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
        }
    }
#else
    static void pinThread(int) {
        // Other platforms: No pinning
    }
#endif

static int64_t nowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

static void calibrateTsc() {
#ifdef ZRQ_TSC_IS_NS
    g_ns_per_tsc = 1.0;
    std::printf("TSC: unavailable, falling back to steady_clock (low resolution)\n");
#else
    const int64_t t0 = nowNs();
    ZRQ_TSC_FENCE();
    const uint64_t c0 = ZRQ_TSC();
    while (nowNs() - t0 < 20000000) {
        ZRQUEUE_CPU_PAUSE();
    }
    ZRQ_TSC_FENCE();
    const uint64_t c1 = ZRQ_TSC();
    const int64_t t1 = nowNs();
    g_ns_per_tsc = static_cast<double>(t1 - t0) / static_cast<double>(c1 - c0);
    std::printf("TSC calibration: %.6f ns/cycle (%.3f GHz)\n",
                g_ns_per_tsc, 1.0 / g_ns_per_tsc);
#endif
}

[[noreturn]] static void orderViolation(const char* who) {
    std::fprintf(stderr, "ORDER VIOLATION in %s\n", who);
    std::abort();
}

// Runs produce() on the calling thread (pinned to cpuProd) while consume() runs on a
// worker pinned to cpuCons. Returns ops/ms for `iters` produced elements.
template <typename Produce, typename Consume>
static double runThroughput(int64_t iters, int cpuCons, int cpuProd, Produce&& produce, Consume&& consume) {
    std::thread t([&] {
        pinThread(cpuCons);
        consume();
    });
    pinThread(cpuProd);
    const int64_t start = nowNs();
    produce();
    t.join();
    return static_cast<double>(iters) * 1e6 / static_cast<double>(nowNs() - start);
}

// Ping-pong: produce() does one full round trip per iteration. Returns ns per RTT.
template <typename Produce, typename Consume>
static int64_t runRtt(int64_t iters, int cpuCons, int cpuProd, Produce&& produce, Consume&& consume) {
    std::thread t([&] {
        pinThread(cpuCons);
        consume();
    });
    pinThread(cpuProd);
    const int64_t start = nowNs();
    produce();
    const int64_t stop = nowNs();
    t.join();
    return (stop - start) / iters;
}

struct Stats {
    std::vector<double> samples;
    void add(double v) { samples.push_back(v); }
    void report(const char* name, const char* unit) {
        std::sort(samples.begin(), samples.end());
        std::printf("  %-38s min=%9.0f  median=%9.0f  max=%9.0f %s\n",
                    name, samples.front(), samples[samples.size() / 2], samples.back(), unit);
        samples.clear();
    }
};

static constexpr int64_t TPUT_ITERS = 10000000;
static constexpr int     TPUT_REPS  = 7;
static constexpr int64_t RTT_ITERS  = 2000000;
static constexpr int     RTT_REPS   = 5;
static constexpr int     BULK       = 64;

// ---------------------------------------------------------------------------
// Throughput suite: 7 variants, interleaved per rep
// ---------------------------------------------------------------------------
template <uint32_t Cap>
static void runThroughputSuite(int cpu1, int cpu2, const char* note) {
    std::printf("\n=== Throughput: capacity %u (%s), %lld iters x %d reps interleaved ===\n",
                Cap, note, (long long)TPUT_ITERS, TPUT_REPS);
    Stats sRig, sHeap, sInline, sMirror, sRing, sRingBulk, sHeapBulk;

    for (int rep = 0; rep < TPUT_REPS; ++rep) {
        {
            rigtorp::SPSCQueue<int> q(Cap);
            sRig.add(runThroughput(TPUT_ITERS, cpu1, cpu2,
                [&] { for (int64_t i = 0; i < TPUT_ITERS; ++i) q.emplace(static_cast<int>(i)); },
                [&] { for (int64_t i = 0; i < TPUT_ITERS; ++i) {
                          int* f;
                          while ((f = q.front()) == nullptr) {}
                          if (*f != static_cast<int>(i)) orderViolation("rigtorp");
                          q.pop(); } }));
        }
        {
            zrqueue::SpscHeapQueue<int> q(Cap);
            sHeap.add(runThroughput(TPUT_ITERS, cpu1, cpu2,
                [&] { for (int64_t i = 0; i < TPUT_ITERS; ++i) q.emplace(static_cast<int>(i)); },
                [&] { for (int64_t i = 0; i < TPUT_ITERS; ++i) {
                          int* f;
                          while ((f = q.front()) == nullptr) {}
                          if (*f != static_cast<int>(i)) orderViolation("SpscHeapQueue");
                          q.pop(); } }));
        }
        {
            auto* q = new zrqueue::SpscInlineQueue<int, Cap>();
            sInline.add(runThroughput(TPUT_ITERS, cpu1, cpu2,
                [&] { for (int64_t i = 0; i < TPUT_ITERS; ++i) q->emplace(static_cast<int>(i)); },
                [&] { for (int64_t i = 0; i < TPUT_ITERS; ++i) {
                          int* f;
                          while ((f = q->front()) == nullptr) {}
                          if (*f != static_cast<int>(i)) orderViolation("SpscInlineQueue");
                          q->pop(); } }));
            delete q;
        }
        {   // MirrorQueue: latest-K mirroring (payload rides the index line)
            auto* q = new zrqueue::SpscMirrorQueue<int, Cap>();
            sMirror.add(runThroughput(TPUT_ITERS, cpu1, cpu2,
                [&] { for (int64_t i = 0; i < TPUT_ITERS; ++i) q->emplace(static_cast<int>(i)); },
                [&] { for (int64_t i = 0; i < TPUT_ITERS; ++i) {
                          int* f;
                          while ((f = q->front()) == nullptr) {}
                          if (*f != static_cast<int>(i)) orderViolation("SpscMirrorQueue");
                          q->pop(); } }));
            delete q;
        }
        {   // RingBuffer single-slot: claim / write in place / commit
            auto* q = new zrqueue::SpscRingBuffer<int, Cap>();
            sRing.add(runThroughput(TPUT_ITERS, cpu1, cpu2,
                [&] { for (int64_t i = 0; i < TPUT_ITERS; ++i)
                          q->push([&](int* p) { *p = static_cast<int>(i); }); },
                [&] { for (int64_t i = 0; i < TPUT_ITERS; ++i) {
                          int* f;
                          while ((f = q->front()) == nullptr) {}
                          if (*f != static_cast<int>(i)) orderViolation("SpscRingBuffer");
                          q->pop(); } }));
            delete q;
        }
        {   // RingBuffer bulk: zero-copy alloc_bulk + commit, one barrier per burst
            auto* q = new zrqueue::SpscRingBuffer<int, Cap>();
            sRingBulk.add(runThroughput(TPUT_ITERS, cpu1, cpu2,
                [&] { int64_t i = 0;
                      while (i < TPUT_ITERS) {
                          size_t granted = 0;
                          int* p = q->alloc_bulk(BULK, granted);
                          if (!p) { ZRQUEUE_CPU_PAUSE(); continue; }
                          size_t written = 0;
                          for (size_t k = 0; k < granted && i < TPUT_ITERS; ++k) {
                              p[k] = static_cast<int>(i++);
                              ++written;
                          }
                          q->commit(written);
                      } },
                [&] { int64_t expect = 0;
                      while (expect < TPUT_ITERS) {
                          size_t n = q->consume_bulk([&](int& v) noexcept {
                              if (v != static_cast<int>(expect)) orderViolation("RingBuffer bulk");
                              ++expect; });
                          if (n == 0) ZRQUEUE_CPU_PAUSE();
                      } }));
            delete q;
        }
        {   // HeapQueue bulk: copy-based push_bulk (data already in a contiguous buffer)
            zrqueue::SpscHeapQueue<int> q(Cap);
            sHeapBulk.add(runThroughput(TPUT_ITERS, cpu1, cpu2,
                [&] { int64_t i = 0;
                      while (i < TPUT_ITERS) {
                          int buf[BULK];
                          int n = 0;
                          for (; n < BULK && i < TPUT_ITERS; ++n) buf[n] = static_cast<int>(i++);
                          size_t off = 0;
                          while (off < static_cast<size_t>(n)) {
                              size_t pushed = q.push_bulk(buf + off, n - off);
                              if (pushed == 0) ZRQUEUE_CPU_PAUSE();
                              off += pushed;
                          }
                      } },
                [&] { int64_t expect = 0;
                      while (expect < TPUT_ITERS) {
                          size_t n = q.consume_bulk([&](int& v) noexcept {
                              if (v != static_cast<int>(expect)) orderViolation("HeapQueue bulk");
                              ++expect; });
                          if (n == 0) ZRQUEUE_CPU_PAUSE();
                      } }));
        }
    }

    sRig.report("rigtorp emplace/front/pop", "ops/ms");
    sHeap.report("HeapQueue emplace/front/pop", "ops/ms");
    sInline.report("InlineQueue emplace/front/pop", "ops/ms");
    sMirror.report("MirrorQueue emplace/front/pop", "ops/ms");
    sRing.report("RingBuffer alloc/write/push", "ops/ms");
    sRingBulk.report("RingBuffer alloc_bulk/commit x64", "ops/ms");
    sHeapBulk.report("HeapQueue push_bulk/consume_bulk x64", "ops/ms");
}

// ---------------------------------------------------------------------------
// Round-trip latency suite: 5 variants, interleaved per rep
// ---------------------------------------------------------------------------
template <uint32_t Cap>
static void runRttSuite(int cpu1, int cpu2, const char* note) {
    std::printf("\n=== Round-trip latency: capacity %u (%s), %lld RTTs x %d reps interleaved ===\n",
                Cap, note, (long long)RTT_ITERS, RTT_REPS);
    Stats sRig, sHeap, sInline, sMirror, sRing;

    for (int rep = 0; rep < RTT_REPS; ++rep) {
        {
            rigtorp::SPSCQueue<int> a(Cap), b(Cap);
            sRig.add(static_cast<double>(runRtt(RTT_ITERS, cpu1, cpu2,
                [&] { for (int64_t i = 0; i < RTT_ITERS; ++i) {
                          a.emplace(static_cast<int>(i));
                          while (!b.front()) {}
                          b.pop(); } },
                [&] { for (int64_t i = 0; i < RTT_ITERS; ++i) {
                          int* f;
                          while ((f = a.front()) == nullptr) {}
                          b.emplace(*f);
                          a.pop(); } })));
        }
        {
            zrqueue::SpscHeapQueue<int> a(Cap), b(Cap);
            sHeap.add(static_cast<double>(runRtt(RTT_ITERS, cpu1, cpu2,
                [&] { for (int64_t i = 0; i < RTT_ITERS; ++i) {
                          a.emplace(static_cast<int>(i));
                          while (!b.front()) {}
                          b.pop(); } },
                [&] { for (int64_t i = 0; i < RTT_ITERS; ++i) {
                          int* f;
                          while ((f = a.front()) == nullptr) {}
                          b.emplace(*f);
                          a.pop(); } })));
        }
        {
            auto* a = new zrqueue::SpscInlineQueue<int, Cap>();
            auto* b = new zrqueue::SpscInlineQueue<int, Cap>();
            sInline.add(static_cast<double>(runRtt(RTT_ITERS, cpu1, cpu2,
                [&] { for (int64_t i = 0; i < RTT_ITERS; ++i) {
                          a->emplace(static_cast<int>(i));
                          while (!b->front()) {}
                          b->pop(); } },
                [&] { for (int64_t i = 0; i < RTT_ITERS; ++i) {
                          int* f;
                          while ((f = a->front()) == nullptr) {}
                          b->emplace(*f);
                          a->pop(); } })));
            delete a;
            delete b;
        }
        {   // MirrorQueue: the design target - payload rides the index line (2 migrations)
            auto* a = new zrqueue::SpscMirrorQueue<int, Cap>();
            auto* b = new zrqueue::SpscMirrorQueue<int, Cap>();
            sMirror.add(static_cast<double>(runRtt(RTT_ITERS, cpu1, cpu2,
                [&] { for (int64_t i = 0; i < RTT_ITERS; ++i) {
                          a->emplace(static_cast<int>(i));
                          while (!b->front()) {}
                          b->pop(); } },
                [&] { for (int64_t i = 0; i < RTT_ITERS; ++i) {
                          int* f;
                          while ((f = a->front()) == nullptr) {}
                          b->emplace(*f);
                          a->pop(); } })));
            delete a;
            delete b;
        }
        {
            auto* a = new zrqueue::SpscRingBuffer<int, Cap>();
            auto* b = new zrqueue::SpscRingBuffer<int, Cap>();
            sRing.add(static_cast<double>(runRtt(RTT_ITERS, cpu1, cpu2,
                [&] { for (int64_t i = 0; i < RTT_ITERS; ++i) {
                          a->push([&](int* p) { *p = static_cast<int>(i); });
                          while (!b->front()) {}
                          b->pop(); } },
                [&] { for (int64_t i = 0; i < RTT_ITERS; ++i) {
                          int* f;
                          while ((f = a->front()) == nullptr) {}
                          b->push([&](int* p) { *p = *f; });
                          a->pop(); } })));
            delete a;
            delete b;
        }
    }

    sRig.report("rigtorp", "ns RTT");
    sHeap.report("HeapQueue", "ns RTT");
    sInline.report("InlineQueue", "ns RTT");
    sMirror.report("MirrorQueue", "ns RTT");
    sRing.report("RingBuffer", "ns RTT");
}

// ---------------------------------------------------------------------------
// Payload length sweep: identical measurement at every element size, one row
// per size so rigtorp-vs-zrqueue ratios are comparable across lengths.
// ---------------------------------------------------------------------------
static constexpr int      PAYLOAD_REPS = 5;
static constexpr uint32_t PAYLOAD_CAP  = 4096;

// Both threads must be pinned and ready before the clock starts; otherwise a
// few-millisecond sample is dominated by thread startup.
template <typename Produce, typename Consume>
static double runThroughputSynced(int64_t iters, int cpuCons, int cpuProd,
                                  Produce&& produce, Consume&& consume) {
    std::atomic<int> ready{ 0 };
    std::atomic<bool> go{ false };
    std::thread t([&] {
        pinThread(cpuCons);
        ready.fetch_add(1, std::memory_order_release);
        while (!go.load(std::memory_order_acquire)) {
            ZRQUEUE_CPU_PAUSE();
        }
        consume();
    });
    pinThread(cpuProd);
    ready.fetch_add(1, std::memory_order_release);
    while (ready.load(std::memory_order_acquire) != 2) {
        ZRQUEUE_CPU_PAUSE();
    }
    const int64_t start = nowNs();
    go.store(true, std::memory_order_release);
    produce();
    t.join();
    return static_cast<double>(iters) * 1e6 / static_cast<double>(nowNs() - start);
}

template <typename T>
static void writePayload(T& v, uint64_t s) { v.fill(s); }

static double medianOf(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

template <typename T, std::size_t Bytes>
static void runPayloadSuite(int cpu1, int cpu2, const char* label, int64_t iters) {
    static_assert(sizeof(T) == Bytes, "payload size mismatch");

    auto measureRig = [&]() -> double {
        rigtorp::SPSCQueue<T> q(PAYLOAD_CAP);
        return runThroughputSynced(iters, cpu1, cpu2,
            [&] { for (int64_t i = 0; i < iters; ++i)
                      q.emplace(static_cast<uint64_t>(i)); },
            [&] { for (int64_t i = 0; i < iters; ++i) {
                      T* f;
                      while ((f = q.front()) == nullptr) {}
                      if (f->seq != static_cast<uint64_t>(i)) orderViolation("rigtorp payload");
                      q.pop(); } });
    };
    auto measureHeap = [&]() -> double {
        zrqueue::SpscHeapQueue<T> q(PAYLOAD_CAP);
        return runThroughputSynced(iters, cpu1, cpu2,
            [&] { for (int64_t i = 0; i < iters; ++i)
                      q.emplace(static_cast<uint64_t>(i)); },
            [&] { for (int64_t i = 0; i < iters; ++i) {
                      T* f;
                      while ((f = q.front()) == nullptr) {}
                      if (f->seq != static_cast<uint64_t>(i)) orderViolation("HeapQueue payload");
                      q.pop(); } });
    };
    auto measureInline = [&]() -> double {
        auto* q = new zrqueue::SpscInlineQueue<T, PAYLOAD_CAP>();
        const double r = runThroughputSynced(iters, cpu1, cpu2,
            [&] { for (int64_t i = 0; i < iters; ++i)
                      q->emplace(static_cast<uint64_t>(i)); },
            [&] { for (int64_t i = 0; i < iters; ++i) {
                      T* f;
                      while ((f = q->front()) == nullptr) {}
                      if (f->seq != static_cast<uint64_t>(i)) orderViolation("InlineQueue payload");
                      q->pop(); } });
        delete q;
        return r;
    };
    auto measureMirror = [&]() -> double {
        auto* q = new zrqueue::SpscMirrorQueue<T, PAYLOAD_CAP>();
        const double r = runThroughputSynced(iters, cpu1, cpu2,
            [&] { for (int64_t i = 0; i < iters; ++i)
                      q->emplace(static_cast<uint64_t>(i)); },
            [&] { for (int64_t i = 0; i < iters; ++i) {
                      T* f;
                      while ((f = q->front()) == nullptr) {}
                      if (f->seq != static_cast<uint64_t>(i)) orderViolation("MirrorQueue payload");
                      q->pop(); } });
        delete q;
        return r;
    };
    auto measureRing = [&]() -> double {
        auto* q = new zrqueue::SpscRingBuffer<T, PAYLOAD_CAP>();
        const double r = runThroughputSynced(iters, cpu1, cpu2,
            [&] { for (int64_t i = 0; i < iters; ++i)
                      q->push([&](T* p) { writePayload(*p, static_cast<uint64_t>(i)); }); },
            [&] { for (int64_t i = 0; i < iters; ++i) {
                      T* f;
                      while ((f = q->front()) == nullptr) {}
                      if (f->seq != static_cast<uint64_t>(i)) orderViolation("RingBuffer payload");
                      q->pop(); } });
        delete q;
        return r;
    };

    std::array<std::function<double()>, 5> variants = {
        measureRig, measureHeap, measureInline, measureMirror, measureRing
    };
    std::array<Stats, 5> stats;

    // Rotate the order every rep. A fixed "rigtorp first" order charges it the
    // cold-cache slot and systematically understates it (measured 47K vs 300K+).
    for (int rep = 0; rep < PAYLOAD_REPS; ++rep) {
        for (int pos = 0; pos < 5; ++pos) {
            const int v = (pos + rep) % 5;
            stats[v].add(variants[v]());
        }
    }

    const double mRig = medianOf(stats[0].samples);
    const double mHeap = medianOf(stats[1].samples);
    const double mInline = medianOf(stats[2].samples);
    const double mMirror = medianOf(stats[3].samples);
    const double mRing = medianOf(stats[4].samples);
    std::printf("  %6zu  %8lld  %8.0f %8.0f %8.0f %8.0f %8.0f   %6.2fx  %s\n",
                Bytes, (long long)iters, mRig, mHeap, mInline, mMirror, mRing,
                mRing / mRig, label);
}

// Ping-pong RTT percentiles. Only one element is ever in flight, so a small
// capacity avoids pre-faulting megabytes per rep without changing the protocol.
static constexpr uint32_t RTT_PAYLOAD_CAP  = 256;
static constexpr int      PAYLOAD_RTT_REPS = 5;

struct RttQuart {
    double p50, p90, p99, p999;
};

// One clock read per iteration: samples[i] is the interval from the previous
// iteration's end to this one's, i.e. one full producer-side round trip. The
// single QPC read per RTT is charged to every variant equally, so ratios stay
// comparable; absolute values sit ~20-30ns above an unsampled measurement.
// Sorting happens after join(), outside the timed region.
template <typename Produce, typename Consume>
static RttQuart runRttSampled(int64_t iters, int cpuCons, int cpuProd,
                              Produce&& produce, Consume&& consume) {
    std::vector<uint32_t> samples(static_cast<size_t>(iters));
    std::atomic<int> ready{ 0 };
    std::atomic<bool> go{ false };
    std::thread t([&] {
        pinThread(cpuCons);
        ready.fetch_add(1, std::memory_order_release);
        while (!go.load(std::memory_order_acquire)) {
            ZRQUEUE_CPU_PAUSE();
        }
        consume();
    });
    pinThread(cpuProd);
    ready.fetch_add(1, std::memory_order_release);
    while (ready.load(std::memory_order_acquire) != 2) {
        ZRQUEUE_CPU_PAUSE();
    }
    ZRQ_TSC_FENCE();
    uint64_t prev = ZRQ_TSC();
    go.store(true, std::memory_order_release);
    produce(samples, prev);
    t.join();

    std::sort(samples.begin(), samples.end());
    const auto at = [&](double q) {
        const size_t idx = static_cast<size_t>(static_cast<double>(samples.size()) * q);
        return static_cast<double>(samples[idx < samples.size() ? idx : samples.size() - 1]);
    };
    return RttQuart{ at(0.50), at(0.90), at(0.99), at(0.999) };
}

template <typename T, std::size_t Bytes>
static void runPayloadRttPercentiles(int cpu1, int cpu2, const char* label, int64_t iters) {
    static_assert(sizeof(T) == Bytes, "payload size mismatch");

    auto measureRig = [&]() -> RttQuart {
        rigtorp::SPSCQueue<T> a(RTT_PAYLOAD_CAP), b(RTT_PAYLOAD_CAP);
        return runRttSampled(iters, cpu1, cpu2,
            [&](std::vector<uint32_t>& s, uint64_t& prev) {
                for (int64_t i = 0; i < iters; ++i) {
                    a.emplace(static_cast<uint64_t>(i));
                    while (!b.front()) {}
                    b.pop();
                    ZRQ_TSC_FENCE();
                    const uint64_t now = ZRQ_TSC();
                    s[static_cast<size_t>(i)] =
                        static_cast<uint32_t>(static_cast<double>(now - prev) * g_ns_per_tsc);
                    prev = now;
                }
            },
            [&] { for (int64_t i = 0; i < iters; ++i) {
                      T* f;
                      while ((f = a.front()) == nullptr) {}
                      if (f->seq != static_cast<uint64_t>(i)) orderViolation("rigtorp payload RTT");
                      b.emplace(*f);
                      a.pop(); } });
    };
    auto measureHeap = [&]() -> RttQuart {
        zrqueue::SpscHeapQueue<T> a(RTT_PAYLOAD_CAP), b(RTT_PAYLOAD_CAP);
        return runRttSampled(iters, cpu1, cpu2,
            [&](std::vector<uint32_t>& s, uint64_t& prev) {
                for (int64_t i = 0; i < iters; ++i) {
                    a.emplace(static_cast<uint64_t>(i));
                    while (!b.front()) {}
                    b.pop();
                    ZRQ_TSC_FENCE();
                    const uint64_t now = ZRQ_TSC();
                    s[static_cast<size_t>(i)] =
                        static_cast<uint32_t>(static_cast<double>(now - prev) * g_ns_per_tsc);
                    prev = now;
                }
            },
            [&] { for (int64_t i = 0; i < iters; ++i) {
                      T* f;
                      while ((f = a.front()) == nullptr) {}
                      if (f->seq != static_cast<uint64_t>(i)) orderViolation("HeapQueue payload RTT");
                      b.emplace(*f);
                      a.pop(); } });
    };
    auto measureInline = [&]() -> RttQuart {
        auto* a = new zrqueue::SpscInlineQueue<T, RTT_PAYLOAD_CAP>();
        auto* b = new zrqueue::SpscInlineQueue<T, RTT_PAYLOAD_CAP>();
        const RttQuart r = runRttSampled(iters, cpu1, cpu2,
            [&](std::vector<uint32_t>& s, uint64_t& prev) {
                for (int64_t i = 0; i < iters; ++i) {
                    a->emplace(static_cast<uint64_t>(i));
                    while (!b->front()) {}
                    b->pop();
                    ZRQ_TSC_FENCE();
                    const uint64_t now = ZRQ_TSC();
                    s[static_cast<size_t>(i)] =
                        static_cast<uint32_t>(static_cast<double>(now - prev) * g_ns_per_tsc);
                    prev = now;
                }
            },
            [&] { for (int64_t i = 0; i < iters; ++i) {
                      T* f;
                      while ((f = a->front()) == nullptr) {}
                      if (f->seq != static_cast<uint64_t>(i)) orderViolation("InlineQueue payload RTT");
                      b->emplace(*f);
                      a->pop(); } });
        delete a;
        delete b;
        return r;
    };
    auto measureMirror = [&]() -> RttQuart {
        auto* a = new zrqueue::SpscMirrorQueue<T, RTT_PAYLOAD_CAP>();
        auto* b = new zrqueue::SpscMirrorQueue<T, RTT_PAYLOAD_CAP>();
        const RttQuart r = runRttSampled(iters, cpu1, cpu2,
            [&](std::vector<uint32_t>& s, uint64_t& prev) {
                for (int64_t i = 0; i < iters; ++i) {
                    a->emplace(static_cast<uint64_t>(i));
                    while (!b->front()) {}
                    b->pop();
                    ZRQ_TSC_FENCE();
                    const uint64_t now = ZRQ_TSC();
                    s[static_cast<size_t>(i)] =
                        static_cast<uint32_t>(static_cast<double>(now - prev) * g_ns_per_tsc);
                    prev = now;
                }
            },
            [&] { for (int64_t i = 0; i < iters; ++i) {
                      T* f;
                      while ((f = a->front()) == nullptr) {}
                      if (f->seq != static_cast<uint64_t>(i)) orderViolation("MirrorQueue payload RTT");
                      b->emplace(*f);
                      a->pop(); } });
        delete a;
        delete b;
        return r;
    };
    auto measureRing = [&]() -> RttQuart {
        auto* a = new zrqueue::SpscRingBuffer<T, RTT_PAYLOAD_CAP>();
        auto* b = new zrqueue::SpscRingBuffer<T, RTT_PAYLOAD_CAP>();
        const RttQuart r = runRttSampled(iters, cpu1, cpu2,
            [&](std::vector<uint32_t>& s, uint64_t& prev) {
                for (int64_t i = 0; i < iters; ++i) {
                    a->push([&](T* p) { p->fill(static_cast<uint64_t>(i)); });
                    while (!b->front()) {}
                    b->pop();
                    ZRQ_TSC_FENCE();
                    const uint64_t now = ZRQ_TSC();
                    s[static_cast<size_t>(i)] =
                        static_cast<uint32_t>(static_cast<double>(now - prev) * g_ns_per_tsc);
                    prev = now;
                }
            },
            [&] { for (int64_t i = 0; i < iters; ++i) {
                      T* f;
                      while ((f = a->front()) == nullptr) {}
                      if (f->seq != static_cast<uint64_t>(i)) orderViolation("RingBuffer payload RTT");
                      b->push([&](T* p) { *p = *f; });
                      a->pop(); } });
        delete a;
        delete b;
        return r;
    };

    std::array<std::function<RttQuart()>, 5> variants = {
        measureRig, measureHeap, measureInline, measureMirror, measureRing
    };
    std::array<std::vector<RttQuart>, 5> results;
    for (int rep = 0; rep < PAYLOAD_RTT_REPS; ++rep) {
        for (int pos = 0; pos < 5; ++pos) {
            const int v = (pos + rep) % 5;
            results[v].push_back(variants[v]());
        }
    }

    static const char* names[5] = { "rigtorp", "Heap", "Inline", "Mirror", "Ring" };
    for (int v = 0; v < 5; ++v) {
        std::vector<double> v50, v90, v99, v999;
        for (const auto& r : results[v]) {
            v50.push_back(r.p50);
            v90.push_back(r.p90);
            v99.push_back(r.p99);
            v999.push_back(r.p999);
        }
        const double p50 = medianOf(v50);
        const double p90 = medianOf(v90);
        const double p99 = medianOf(v99);
        const double p999 = medianOf(v999);
        std::printf("  %6zu  %8lld  %-9s %7.0f %7.0f %7.0f %8.0f   %5.2fx  %s\n",
                    Bytes, (long long)iters, names[v], p50, p90, p99, p999,
                    p99 / p50, label);
    }
}

// ---------------------------------------------------------------------------
// Controlled-backlog microbenchmark. The producer may lead the consumer by at
// most `depth` elements; the credit counter is touched only every 16 items on
// each side so its cost is amortized and identical for every variant.
// depth == 0 means "uncontrolled" (the classic full-speed producer).
// ---------------------------------------------------------------------------
static constexpr int      DEPTH_REPS  = 3;
static constexpr int64_t  DEPTH_ITERS = 1000000;
static constexpr uint32_t DEPTH_CAP   = 4096;

template <typename Produce, typename Consume>
static double runDepthThroughput(int64_t iters, int64_t depth, int cpuCons, int cpuProd,
                                 Produce&& produce, Consume&& consume) {
    std::atomic<int64_t> consumed{ 0 };
    std::atomic<int> ready{ 0 };
    std::atomic<bool> go{ false };
    std::thread t([&] {
        pinThread(cpuCons);
        ready.fetch_add(1, std::memory_order_release);
        while (!go.load(std::memory_order_acquire)) {
            ZRQUEUE_CPU_PAUSE();
        }
        consume(consumed);
    });
    pinThread(cpuProd);
    ready.fetch_add(1, std::memory_order_release);
    while (ready.load(std::memory_order_acquire) != 2) {
        ZRQUEUE_CPU_PAUSE();
    }
    const int64_t start = nowNs();
    go.store(true, std::memory_order_release);
    produce(consumed);
    t.join();
    return static_cast<double>(iters) * 1e6 / static_cast<double>(nowNs() - start);
}

template <typename T, typename Q, typename Put>
static double measureDepthVariant(Q& q, Put&& put, int64_t depth, int cpu1, int cpu2) {
    return runDepthThroughput(DEPTH_ITERS, depth, cpu1, cpu2,
        [&](std::atomic<int64_t>& c) {
            for (int64_t i = 0; i < DEPTH_ITERS; ++i) {
                if (depth > 0 && (i & 15) == 0) {
                    uint64_t spins = 0;
                    while (i - c.load(std::memory_order_acquire) >= depth) {
                        if (++spins > 100000000ULL) {
                            break;
                        }
                        ZRQUEUE_CPU_PAUSE();
                    }
                }
                put(q, i);
            }
        },
        [&](std::atomic<int64_t>& c) {
            for (int64_t i = 0; i < DEPTH_ITERS; ++i) {
                T* f;
                while ((f = q.front()) == nullptr) {
                    ZRQUEUE_CPU_PAUSE();
                }
                if (f->seq != static_cast<uint64_t>(i)) orderViolation("depth");
                q.pop();
                if (depth > 0 && (i & 15) == 0) {
                    c.store(i + 1, std::memory_order_release);
                }
            }
        });
}

template <typename T, std::size_t Bytes>
static void runDepthSuite(int cpu1, int cpu2, const char* label,
                          const std::vector<int64_t>& depths) {
    static_assert(sizeof(T) == Bytes, "payload size mismatch");
    std::printf("\n=== Controlled backlog: %s, capacity %u, %lld iters x %d reps (ops/ms) ===\n",
                label, DEPTH_CAP, (long long)DEPTH_ITERS, DEPTH_REPS);
    std::printf("  depth    rigtorp      Heap    Inline    Mirror      Ring   Ring/rig\n");

    for (int64_t depth : depths) {
        std::array<Stats, 5> st;
        for (int rep = 0; rep < DEPTH_REPS; ++rep) {
            std::array<std::function<double()>, 5> variants;
            variants[0] = [&]() -> double {
                rigtorp::SPSCQueue<T> q(DEPTH_CAP);
                return measureDepthVariant<T>(q,
                    [](auto& qq, int64_t i) { qq.emplace(static_cast<uint64_t>(i)); },
                    depth, cpu1, cpu2);
            };
            variants[1] = [&]() -> double {
                zrqueue::SpscHeapQueue<T> q(DEPTH_CAP);
                return measureDepthVariant<T>(q,
                    [](auto& qq, int64_t i) { qq.emplace(static_cast<uint64_t>(i)); },
                    depth, cpu1, cpu2);
            };
            variants[2] = [&]() -> double {
                auto* q = new zrqueue::SpscInlineQueue<T, DEPTH_CAP>();
                const double r = measureDepthVariant<T>(*q,
                    [](auto& qq, int64_t i) { qq.emplace(static_cast<uint64_t>(i)); },
                    depth, cpu1, cpu2);
                delete q;
                return r;
            };
            variants[3] = [&]() -> double {
                auto* q = new zrqueue::SpscMirrorQueue<T, DEPTH_CAP>();
                const double r = measureDepthVariant<T>(*q,
                    [](auto& qq, int64_t i) { qq.emplace(static_cast<uint64_t>(i)); },
                    depth, cpu1, cpu2);
                delete q;
                return r;
            };
            variants[4] = [&]() -> double {
                auto* q = new zrqueue::SpscRingBuffer<T, DEPTH_CAP>();
                const double r = measureDepthVariant<T>(*q,
                    [](auto& qq, int64_t i) {
                        qq.push([&](auto* p) { p->fill(static_cast<uint64_t>(i)); });
                    },
                    depth, cpu1, cpu2);
                delete q;
                return r;
            };

            for (int pos = 0; pos < 5; ++pos) {
                const int v = (pos + rep) % 5;
                st[v].add(variants[v]());
            }
        }
        const double mRig = medianOf(st[0].samples);
        const double mHeap = medianOf(st[1].samples);
        const double mInline = medianOf(st[2].samples);
        const double mMirror = medianOf(st[3].samples);
        const double mRing = medianOf(st[4].samples);
        std::printf("  %6lld  %8.0f %8.0f %8.0f %8.0f %8.0f   %6.2fx\n",
                    (long long)depth, mRig, mHeap, mInline, mMirror, mRing,
                    mRing / mRig);
    }
}

// Isolate which part of RingBuffer's single-element producer path costs the
// extra ~20-30% at 128B: push(Writer)'s retry loop, or the slot write itself.
template <typename T, std::size_t Bytes>
static void runRingPathDiag(int cpu1, int cpu2) {
    static_assert(sizeof(T) == Bytes, "payload size mismatch");
    constexpr int64_t kIters = 1000000;
    constexpr int kReps = 3;
    std::printf("\n=== RingBuffer producer-path diagnosis: %zuB, capacity %u, %lld iters x %d reps (ops/ms) ===\n",
                Bytes, DEPTH_CAP, (long long)kIters, kReps);

    std::array<Stats, 3> st;
    for (int rep = 0; rep < kReps; ++rep) {
        std::array<std::function<double()>, 3> variants;
        variants[0] = [&]() -> double {
            auto* q = new zrqueue::SpscRingBuffer<T, DEPTH_CAP>();
            const double r = runDepthThroughput(kIters, 0, cpu1, cpu2,
                [&](std::atomic<int64_t>&) {
                    for (int64_t i = 0; i < kIters; ++i) {
                        q->push([&](T* p) { p->fill(static_cast<uint64_t>(i)); });
                    }
                },
                [&](std::atomic<int64_t>&) {
                    for (int64_t i = 0; i < kIters; ++i) {
                        T* f;
                        while ((f = q->front()) == nullptr) { ZRQUEUE_CPU_PAUSE(); }
                        if (f->seq != static_cast<uint64_t>(i)) orderViolation("ring diag push");
                        q->pop();
                    }
                });
            delete q;
            return r;
        };
        variants[1] = [&]() -> double {
            auto* q = new zrqueue::SpscRingBuffer<T, DEPTH_CAP>();
            const double r = runDepthThroughput(kIters, 0, cpu1, cpu2,
                [&](std::atomic<int64_t>&) {
                    for (int64_t i = 0; i < kIters; ++i) {
                        T* p = q->spin_alloc();
                        p->fill(static_cast<uint64_t>(i));
                        q->push();
                    }
                },
                [&](std::atomic<int64_t>&) {
                    for (int64_t i = 0; i < kIters; ++i) {
                        T* f;
                        while ((f = q->front()) == nullptr) { ZRQUEUE_CPU_PAUSE(); }
                        if (f->seq != static_cast<uint64_t>(i)) orderViolation("ring diag spin");
                        q->pop();
                    }
                });
            delete q;
            return r;
        };
        variants[2] = [&]() -> double {
            auto* q = new zrqueue::SpscRingBuffer<T, DEPTH_CAP>();
            const double r = runDepthThroughput(kIters, 0, cpu1, cpu2,
                [&](std::atomic<int64_t>&) {
                    for (int64_t i = 0; i < kIters; ++i) {
                        size_t granted = 0;
                        T* p = q->alloc_bulk(1, granted);
                        if (!p) { --i; ZRQUEUE_CPU_PAUSE(); continue; }
                        p->fill(static_cast<uint64_t>(i));
                        q->commit(1);
                    }
                },
                [&](std::atomic<int64_t>&) {
                    for (int64_t i = 0; i < kIters; ++i) {
                        T* f;
                        while ((f = q->front()) == nullptr) { ZRQUEUE_CPU_PAUSE(); }
                        if (f->seq != static_cast<uint64_t>(i)) orderViolation("ring diag bulk");
                        q->pop();
                    }
                });
            delete q;
            return r;
        };

        for (int pos = 0; pos < 3; ++pos) {
            const int v = (pos + rep) % 3;
            st[v].add(variants[v]());
        }
    }
    static const char* names[3] = {
        "push(Writer) [current]", "spin_alloc+push", "alloc_bulk(1)+commit"
    };
    for (int v = 0; v < 3; ++v) {
        std::printf("  %-24s %9.0f\n", names[v], medianOf(st[v].samples));
    }
}

static void printUsage(const char* exe) {
    std::printf(
        "Usage: %s [consumer-core producer-core] [options]\n"
        "\n"
        "Positional (omit both to leave the threads unpinned):\n"
        "  consumer-core    logical CPU for the consumer thread\n"
        "  producer-core    logical CPU for the producer thread\n"
        "  A negative value disables pinning for that side.\n"
        "\n"
        "Options (may appear anywhere):\n"
        "  --help, -h           print this help and exit\n"
        "  --rt                 raise process/thread priority to cut OS jitter:\n"
        "                       HIGH_PRIORITY_CLASS + THREAD_PRIORITY_TIME_CRITICAL\n"
        "                       on Windows, SCHED_FIFO on Linux (needs root/CAP_SYS_NICE)\n"
        "  --rtt-only           run only the TSC-sampled RTT percentile tables\n"
        "  --depth-only         run only controlled-backlog + RingBuffer path diagnostics\n"
        "  --depth=N            run one backlog depth instead of the sweep.\n"
        "                       N=0 means uncontrolled; 0<N<32 is clamped to 32\n"
        "                       because the credit counter is amortized over 16 items\n"
        "\n"
        "Suites (default: all of them):\n"
        "  throughput + RTT at capacities 4096 and 16M\n"
        "  payload length sweep 4B..1KB plus a non-POD row\n"
        "  controlled-backlog sweep and RingBuffer producer-path diagnosis\n"
        "  RTT percentiles p50/p90/p99/p99.9 (one TSC read per RTT)\n"
        "\n"
        "Output: throughput in ops/ms, latency in ns. Every suite rotates the\n"
        "variant order per rep to cancel execution-order bias.\n"
        "\n"
        "Examples:\n"
        "  benchmark 2 4                        full run on cores 2/4\n"
        "  benchmark 2 4 --rt                   same, with reduced scheduling jitter\n"
        "  benchmark 2 4 --rtt-only --rt        ns-level percentiles only\n"
        "  benchmark 1 3 --depth-only           backlog diagnosis on a 4-vCPU host\n"
        "  benchmark 2 4 --depth-only --depth=128\n"
        "  benchmark --help                     this text\n",
        exe);
}

int main(int argc, char* argv[]) {
    int cpu1 = -1; // consumer
    int cpu2 = -1; // producer
    bool realtime = false;
    bool rttOnly = false;
    bool depthOnly = false;
    int64_t depthArg = -1;

    int positional = 0;
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (!std::strcmp(a, "--help") || !std::strcmp(a, "-h")) {
            printUsage(argv[0]);
            return 0;
        }
        if (!std::strcmp(a, "--rt")) {
            realtime = true;
            continue;
        }
        if (!std::strcmp(a, "--rtt-only")) {
            rttOnly = true;
            continue;
        }
        if (!std::strcmp(a, "--depth-only")) {
            depthOnly = true;
            continue;
        }
        if (!std::strncmp(a, "--depth=", 8)) {
            depthArg = std::atoll(a + 8);
            continue;
        }
        if (a[0] == '-' && !(a[1] >= '0' && a[1] <= '9')) {
            std::fprintf(stderr, "unknown option: %s\n\n", a);
            printUsage(argv[0]);
            return 1;
        }
        if (positional == 0) {
            cpu1 = std::atoi(a);
            ++positional;
        } else if (positional == 1) {
            cpu2 = std::atoi(a);
            ++positional;
        } else {
            std::fprintf(stderr, "unexpected argument: %s\n\n", a);
            printUsage(argv[0]);
            return 1;
        }
    }

    g_realtime.store(realtime, std::memory_order_relaxed);
    if (realtime) {
#if defined(_WIN32)
        SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
#elif defined(__linux__)
        struct sched_param sp{};
        sp.sched_priority = sched_get_priority_max(SCHED_FIFO);
        if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) != 0) {
            std::fprintf(stderr, "warning: SCHED_FIFO needs CAP_SYS_NICE/root; priority not raised\n");
        }
#endif
    }
    char depthLabel[32];
    if (depthArg < 0) {
        std::snprintf(depthLabel, sizeof(depthLabel), "sweep");
    } else {
        std::snprintf(depthLabel, sizeof(depthLabel), "%lld", static_cast<long long>(depthArg));
    }
    std::printf("mode: cores=%d/%d  rt=%s  rtt-only=%s  depth-only=%s  depth=%s\n",
                cpu1, cpu2, realtime ? "on" : "off", rttOnly ? "on" : "off",
                depthOnly ? "on" : "off", depthLabel);
    calibrateTsc();

    // Full suite; --rtt-only / --depth-only skip straight to their tables.
    if (!rttOnly && !depthOnly) {
    // 4096: tiny queue exercises the full/empty slow paths (cache reloads, spins)
    runThroughputSuite<4096>(cpu1, cpu2, "slow paths exercised");
    runRttSuite<4096>(cpu1, cpu2, "slow paths exercised");

    // 16M: classic setup (what 10M normalizes to), queue never fills
    runThroughputSuite<16777216>(cpu1, cpu2, "classic methodology");
    runRttSuite<16777216>(cpu1, cpu2, "classic methodology");

    // Element-length sweep: 4B .. 1KB at a fixed capacity, plus a non-POD row.
    // Iterations shrink as the payload grows so every row gets a usable sample.
    std::printf("\n=== Payload length sweep: capacity %u, %d reps interleaved, synced start ===\n",
                PAYLOAD_CAP, PAYLOAD_REPS);
    std::printf("  bytes     iters   rigtorp      Heap    Inline    Mirror      Ring   Ring/rig  note\n");
    runPayloadSuite<PayloadSized<4>, 4>(cpu1, cpu2, "4B", 5000000);
    runPayloadSuite<PayloadSized<8>, 8>(cpu1, cpu2, "8B", 5000000);
    runPayloadSuite<PayloadSized<16>, 16>(cpu1, cpu2, "16B", 5000000);
    runPayloadSuite<PayloadSized<32>, 32>(cpu1, cpu2, "32B", 5000000);
    runPayloadSuite<PayloadSized<64>, 64>(cpu1, cpu2, "64B", 2000000);
    runPayloadSuite<PayloadSized<128>, 128>(cpu1, cpu2, "128B", 2000000);
    runPayloadSuite<PayloadSized<256>, 256>(cpu1, cpu2, "256B", 1000000);
    runPayloadSuite<PayloadSized<512>, 512>(cpu1, cpu2, "512B", 1000000);
    runPayloadSuite<PayloadSized<1024>, 1024>(cpu1, cpu2, "1KB", 500000);
    runPayloadSuite<PayloadNonPod, sizeof(PayloadNonPod)>(cpu1, cpu2, "non-POD", 2000000);
    }

    // Credit is amortized over 16 items, so depths below 32 would deadlock.
    if (!rttOnly) {
    std::vector<int64_t> depths = { 0, 32, 128, 1024, 4095 };
    if (depthArg == 0) {
        depths.assign(1, 0);
    } else if (depthArg > 0) {
        depths.assign(1, std::max<int64_t>(depthArg, 32));
    }
    runDepthSuite<PayloadSized<4>, 4>(cpu1, cpu2, "4B", depths);
    runDepthSuite<PayloadSized<128>, 128>(cpu1, cpu2, "128B", depths);
    runRingPathDiag<PayloadSized<128>, 128>(cpu1, cpu2);
    }

    // Per-iteration RTT percentiles. One clock read per RTT, sorted after join.
    if (!depthOnly) {
    std::printf("\n=== Payload RTT percentiles: capacity %u, %d reps, synced, 1 clock read/iter (ns) ===\n",
                RTT_PAYLOAD_CAP, PAYLOAD_RTT_REPS);
    std::printf("  bytes     iters  model        p50     p90     p99   p99.9   p99/p50  note\n");
    runPayloadRttPercentiles<PayloadSized<4>, 4>(cpu1, cpu2, "4B", 500000);
    runPayloadRttPercentiles<PayloadSized<8>, 8>(cpu1, cpu2, "8B", 500000);
    runPayloadRttPercentiles<PayloadSized<16>, 16>(cpu1, cpu2, "16B", 400000);
    runPayloadRttPercentiles<PayloadSized<32>, 32>(cpu1, cpu2, "32B", 300000);
    runPayloadRttPercentiles<PayloadSized<64>, 64>(cpu1, cpu2, "64B", 200000);
    runPayloadRttPercentiles<PayloadSized<128>, 128>(cpu1, cpu2, "128B", 150000);
    runPayloadRttPercentiles<PayloadSized<256>, 256>(cpu1, cpu2, "256B", 100000);
    runPayloadRttPercentiles<PayloadSized<512>, 512>(cpu1, cpu2, "512B", 60000);
    runPayloadRttPercentiles<PayloadSized<1024>, 1024>(cpu1, cpu2, "1KB", 40000);
    }

    return 0;
}
