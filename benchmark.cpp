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

#include "zrqueue.h"
#include "rigtorp/SPSCQueue.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <thread>
#include <vector>

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
                          size_t n = q->consume_bulk([&](int& v) {
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
                          size_t n = q.consume_bulk([&](int& v) {
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

int main(int argc, char* argv[]) {
    int cpu1 = -1; // consumer
    int cpu2 = -1; // producer

    if (argc >= 3) {
        cpu1 = std::atoi(argv[1]);
        cpu2 = std::atoi(argv[2]);
    }

    // 4096: tiny queue exercises the full/empty slow paths (cache reloads, spins)
    runThroughputSuite<4096>(cpu1, cpu2, "slow paths exercised");
    runRttSuite<4096>(cpu1, cpu2, "slow paths exercised");

    // 16M: classic setup (what 10M normalizes to), queue never fills
    runThroughputSuite<16777216>(cpu1, cpu2, "classic methodology");
    runRttSuite<16777216>(cpu1, cpu2, "classic methodology");

    return 0;
}
