// NDEBUG is deliberately undefined here so SpscRingBuffer's claim/commit
// misuse assertions stay live; expectAbort() verifies they actually fire.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>

#include "../zrqueue.h"
#include "payloads.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#if defined(_MSC_VER)
    #include <crtdbg.h>
    #include <windows.h>
#else
    #include <sys/wait.h>
    #include <unistd.h>
#endif

// 4 B type that opts the mirror off: must fall back to the classic path.
struct MirrorOptOut4 {
    uint32_t v;
    MirrorOptOut4(uint64_t s = 0) noexcept : v(static_cast<uint32_t>(s)) {}
};

namespace zrqueue {
template <>
struct mirror_traits<MirrorOptOut4> {
    static constexpr bool enable_4b = false;
    static constexpr bool enable_8b = false;
};
} // namespace zrqueue

// Custom 8 B type without an opt-in: the default policy keeps it classic.
struct MirrorDefault8 {
    uint32_t lo, hi;
    MirrorDefault8(uint64_t s = 0) noexcept
        : lo(static_cast<uint32_t>(s)), hi(static_cast<uint32_t>(s >> 32)) {}
};
static_assert(sizeof(MirrorDefault8) == 8, "MirrorDefault8 must be 8 bytes");

namespace {

int g_checks = 0;
int g_failures = 0;

#define CHECK(expr)                                                                 \
    do {                                                                            \
        ++g_checks;                                                                 \
        if (!(expr)) {                                                              \
            ++g_failures;                                                           \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr);   \
        }                                                                           \
    } while (0)

template <typename T>
uint64_t seqOf(const T& v) { return static_cast<uint64_t>(v); }
uint64_t seqOf(const PayloadBig& v) { return v.seq; }
uint64_t seqOf(const PayloadNonPod& v) { return v.seq; }
uint64_t seqOf(const PayloadScalar8& v) { return v.seq; }
uint64_t seqOf(const PayloadTwo32& v) { return v.bits(); }
uint64_t seqOf(const PayloadNarrow4& v) { return v.bits(); }

template <typename T>
void setSeq(T& v, uint64_t s) { v = static_cast<T>(s); }
void setSeq(PayloadBig& v, uint64_t s) { v.fill(s); }
void setSeq(PayloadNonPod& v, uint64_t s) { v.seq = s; }

template <typename T>
bool checkVal(const T& v, uint64_t s) { return seqOf(v) == s; }
bool checkVal(const PayloadBig& v, uint64_t s) { return v.check(s); }

template <typename T, typename Q>
bool tryPut(Q& q, uint64_t i) { return q.try_emplace(T(i)); }

template <typename T, uint32_t N>
bool tryPut(zrqueue::SpscRingBuffer<T, N>& q, uint64_t i) {
    return q.try_push([&](T* p) { setSeq(*p, i); });
}

struct RunResult {
    uint64_t produced;
    uint64_t consumed;
    int violations;
    bool timedOut;
};

// Two-thread ordering check, extracted from the benchmark's inline assertions.
// A spin ceiling prevents a broken queue from hanging ctest forever.
template <typename T, typename Q>
RunResult runOrdered(Q& q, uint64_t n) {
    std::atomic<bool> stop{ false };
    std::atomic<uint64_t> produced{ 0 }, consumed{ 0 };
    std::atomic<int> violations{ 0 };
    std::atomic<bool> timedOut{ false };

    std::thread producer([&] {
        for (uint64_t i = 0; i < n; ++i) {
            while (!tryPut<T>(q, i)) {
                if (stop.load(std::memory_order_relaxed)) {
                    return;
                }
                ZRQUEUE_CPU_PAUSE();
            }
            produced.fetch_add(1, std::memory_order_relaxed);
        }
    });

    std::thread consumer([&] {
        uint64_t spins = 0;
        for (uint64_t i = 0; i < n; ++i) {
            T* f = nullptr;
            while ((f = q.front()) == nullptr) {
                if (stop.load(std::memory_order_relaxed)) {
                    return;
                }
                if (++spins > 2000000000ULL) {
                    timedOut.store(true, std::memory_order_relaxed);
                    stop.store(true, std::memory_order_relaxed);
                    return;
                }
                ZRQUEUE_CPU_PAUSE();
            }
            if (!checkVal(*f, i)) {
                violations.fetch_add(1, std::memory_order_relaxed);
            }
            q.pop();
            consumed.fetch_add(1, std::memory_order_relaxed);
        }
    });

    producer.join();
    consumer.join();
    return RunResult{ produced.load(), consumed.load(), violations.load(), timedOut.load() };
}

void checkRun(const char* label, const char* model, const RunResult& r, uint64_t n) {
    CHECK(!r.timedOut);
    CHECK(r.violations == 0);
    CHECK(r.produced == n);
    CHECK(r.consumed == n);
    if (r.timedOut || r.violations != 0 || r.produced != n || r.consumed != n) {
        std::fprintf(stderr, "  [%s/%s] produced=%llu consumed=%llu violations=%d timedOut=%d\n",
                     label, model, (unsigned long long)r.produced,
                     (unsigned long long)r.consumed, r.violations, r.timedOut ? 1 : 0);
    }
}

template <typename T, uint32_t Cap>
void testAllModels(const char* label) {
    constexpr uint64_t kN = 100000;
    {
        zrqueue::SpscHeapQueue<T> q(Cap);
        checkRun(label, "HeapQueue", runOrdered<T>(q, kN), kN);
    }
    {
        auto q = std::make_unique<zrqueue::SpscInlineQueue<T, Cap>>();
        checkRun(label, "InlineQueue", runOrdered<T>(*q, kN), kN);
    }
    {
        auto q = std::make_unique<zrqueue::SpscMirrorQueue<T, Cap>>();
        checkRun(label, "MirrorQueue", runOrdered<T>(*q, kN), kN);
    }
    {
        auto q = std::make_unique<zrqueue::SpscRingBuffer<T, Cap>>();
        checkRun(label, "RingBuffer", runOrdered<T>(*q, kN), kN);
    }
}

// Empty, full, peek lookahead, drain and index-wraparound across several laps.
template <typename T, typename Q>
void testBoundaryOne(Q& q, uint32_t cap) {
    CHECK(q.empty());
    CHECK(q.size() == 0);
    CHECK(q.front() == nullptr);
    CHECK(q.peek(0) == nullptr);

    for (uint32_t i = 0; i < cap; ++i) {
        CHECK(tryPut<T>(q, i));
    }
    CHECK(!tryPut<T>(q, cap));
    CHECK(q.size() == cap);
    CHECK(!q.empty());

    if (cap >= 3) {
        T* p2 = q.peek(2);
        CHECK(p2 != nullptr);
        if (p2) {
            CHECK(checkVal(*p2, 2));
        }
        CHECK(q.peek(1) != nullptr);
    }
    CHECK(q.peek(cap) == nullptr);

    for (uint32_t i = 0; i < cap; ++i) {
        T* f = q.front();
        CHECK(f != nullptr);
        if (f) {
            CHECK(checkVal(*f, i));
        }
        q.pop();
    }
    CHECK(q.empty());
    CHECK(q.size() == 0);

    for (uint32_t lap = 1; lap <= 4; ++lap) {
        const uint64_t base = static_cast<uint64_t>(lap) * cap;
        for (uint32_t i = 0; i < cap; ++i) {
            CHECK(tryPut<T>(q, base + i));
        }
        for (uint32_t i = 0; i < cap; ++i) {
            T* f = q.front();
            CHECK(f != nullptr);
            if (f) {
                CHECK(checkVal(*f, base + i));
            }
            q.pop();
        }
    }
}

template <typename T, typename Q>
void testPeekOverflowOne(Q& q, uint32_t cap) {
    // Advance read_index far enough that a large offset would wrap it back into
    // the live window instead of landing past write_index.
    const uint64_t laps = 125;
    for (uint64_t lap = 0; lap < laps; ++lap) {
        for (uint32_t i = 0; i < cap; ++i) {
            CHECK(tryPut<T>(q, lap * cap + i));
        }
        for (uint32_t i = 0; i < cap; ++i) {
            CHECK(q.front() != nullptr);
            q.pop();
        }
    }

    const uint64_t base = laps * cap; // == read_index after the laps
    CHECK(tryPut<T>(q, base));

    T* p0 = q.peek(0);
    CHECK(p0 != nullptr);
    if (p0) {
        CHECK(checkVal(*p0, base));
    }
    CHECK(q.peek(1) == nullptr);

    // Offsets that wrap the index arithmetic must be rejected. Before the fix
    // these aliased slot 0 (already consumed) instead of returning nullptr.
    CHECK(q.peek(SIZE_MAX) == nullptr);
    CHECK(q.peek(static_cast<size_t>(0) - 1) == nullptr);
    CHECK(q.peek(static_cast<size_t>(0) - static_cast<size_t>(base)) == nullptr);
    CHECK(q.peek(static_cast<size_t>(0) - static_cast<size_t>(base - 1)) == nullptr);

    T* f = q.front();
    CHECK(f != nullptr);
    if (f) {
        CHECK(checkVal(*f, base));
    }
    q.pop();
    CHECK(q.empty());
}

template <typename T, uint32_t Cap>
void testPeekOverflow() {
    {
        zrqueue::SpscHeapQueue<T> q(Cap);
        testPeekOverflowOne<T>(q, Cap);
    }
    {
        auto q = std::make_unique<zrqueue::SpscInlineQueue<T, Cap>>();
        testPeekOverflowOne<T>(*q, Cap);
    }
    {
        auto q = std::make_unique<zrqueue::SpscMirrorQueue<T, Cap>>();
        testPeekOverflowOne<T>(*q, Cap);
    }
    {
        auto q = std::make_unique<zrqueue::SpscRingBuffer<T, Cap>>();
        testPeekOverflowOne<T>(*q, Cap);
    }
}

template <typename T, typename Q>
void testFrontBulkOne(Q& q, uint32_t cap) {
    size_t available = 99;
    CHECK(q.front_bulk(0, available) == nullptr);
    CHECK(available == 0);
    CHECK(q.front_bulk(4, available) == nullptr); // empty queue
    CHECK(available == 0);

    // Partial claim: want smaller than backlog.
    for (uint32_t i = 0; i < cap; ++i) {
        CHECK(tryPut<T>(q, i));
    }
    const size_t half = cap / 2;
    T* p = q.front_bulk(half, available);
    CHECK(p != nullptr);
    CHECK(available == half);
    for (size_t i = 0; i < available; ++i) {
        CHECK(checkVal(p[i], i));
    }
    q.pop_bulk(available);
    CHECK(q.size() == cap - half);

    // Drain in contiguous chunks until empty; order must be preserved.
    uint64_t expect = half;
    while (true) {
        size_t got = 0;
        T* batch = q.front_bulk(cap, got);
        if (batch == nullptr) {
            break;
        }
        CHECK(got > 0);
        for (size_t i = 0; i < got; ++i) {
            CHECK(checkVal(batch[i], expect + i));
        }
        q.pop_bulk(got);
        expect += got;
    }
    CHECK(expect == cap);
    CHECK(q.empty());

    // Wrap clamp: leave the cursor one slot before the physical end.
    for (uint32_t i = 0; i < cap; ++i) {
        CHECK(tryPut<T>(q, cap + i));
    }
    for (uint32_t i = 0; i + 1 < cap; ++i) {
        q.pop();
    }
    size_t clamped = 0;
    T* last = q.front_bulk(cap, clamped);
    CHECK(last != nullptr);
    CHECK(clamped == 1); // never straddles the wrap point
    CHECK(checkVal(last[0], cap + cap - 1));
    q.pop_bulk(clamped);
    CHECK(q.empty());
}

template <typename T, uint32_t Cap>
void testFrontBulk() {
    {
        zrqueue::SpscHeapQueue<T> q(Cap);
        testFrontBulkOne<T>(q, Cap);
    }
    {
        auto q = std::make_unique<zrqueue::SpscInlineQueue<T, Cap>>();
        testFrontBulkOne<T>(*q, Cap);
    }
    {
        auto q = std::make_unique<zrqueue::SpscRingBuffer<T, Cap>>();
        testFrontBulkOne<T>(*q, Cap);
    }
}

// front_bulk()/pop_bulk() on non-POD elements must destroy exactly n objects.
void testFrontBulkLifetime() {
    const int base = PayloadNonPod::alive();
    {
        zrqueue::SpscHeapQueue<PayloadNonPod> q(8);
        for (uint64_t i = 0; i < 4; ++i) {
            q.emplace(i);
        }
        CHECK(PayloadNonPod::alive() == base + 4);
        size_t available = 0;
        PayloadNonPod* p = q.front_bulk(4, available);
        CHECK(p != nullptr);
        CHECK(available == 4);
        q.pop_bulk(available);
        CHECK(PayloadNonPod::alive() == base);
    }
    CHECK(PayloadNonPod::alive() == base);
}

// A type requiring 128-byte alignment must come back 128-aligned from every
// model: the default allocator used to cap at one cache line (64).
struct alignas(128) AlignedMsg {
    uint64_t seq;
    uint64_t pad[15];
    AlignedMsg(uint64_t s = 0) noexcept : seq(s), pad{} {}
};
static_assert(alignof(AlignedMsg) == 128, "probe payload must require 128-byte alignment");
static_assert(sizeof(AlignedMsg) == 128, "probe payload must be exactly 128 bytes");

template <typename Q, typename Put>
void checkAlignedSlot(Q& q, Put put, const char* model) {
    put(q, 7);
    const void* p = q.front();
    CHECK(p != nullptr);
    const uintptr_t addr = reinterpret_cast<uintptr_t>(p);
    CHECK(addr % alignof(AlignedMsg) == 0);
    if (addr % alignof(AlignedMsg) != 0) {
        std::fprintf(stderr, "  [%s] misaligned slot: 0x%llx\n", model,
                     static_cast<unsigned long long>(addr));
    }
}

void testOverAlignedType() {
    // Repeat allocations: a single 64-aligned block lands 128-aligned by chance
    // about half the time, so one sample cannot prove the fix.
    for (int round = 0; round < 8; ++round) {
        {
            zrqueue::SpscHeapQueue<AlignedMsg> q(4);
            checkAlignedSlot(q, [](auto& qq, int v) { qq.push(AlignedMsg(static_cast<uint64_t>(v))); },
                             "HeapQueue");
        }
        {
            auto q = std::make_unique<zrqueue::SpscInlineQueue<AlignedMsg, 4>>();
            checkAlignedSlot(*q, [](auto& qq, int v) { qq.push(AlignedMsg(static_cast<uint64_t>(v))); },
                             "InlineQueue");
        }
        {
            auto q = std::make_unique<zrqueue::SpscMirrorQueue<AlignedMsg, 4>>();
            checkAlignedSlot(*q, [](auto& qq, int v) { qq.push(AlignedMsg(static_cast<uint64_t>(v))); },
                             "MirrorQueue");
        }
        {
            auto q = std::make_unique<zrqueue::SpscRingBuffer<AlignedMsg, 4>>();
            checkAlignedSlot(*q,
                             [](auto& qq, int v) { qq.push([v](AlignedMsg* p) { p->seq = static_cast<uint64_t>(v); }); },
                             "RingBuffer");
        }
    }

    // HeapQueue slots are contiguous: element i sits at base + i*sizeof(T),
    // so an aligned base keeps every element aligned.
    {
        zrqueue::SpscHeapQueue<AlignedMsg> q(4);
        for (int i = 0; i < 4; ++i) {
            q.push(AlignedMsg(static_cast<uint64_t>(i)));
        }
        size_t available = 0;
        AlignedMsg* batch = q.front_bulk(4, available);
        CHECK(batch != nullptr);
        CHECK(available == 4);
        for (size_t i = 0; i < available; ++i) {
            const uintptr_t addr = reinterpret_cast<uintptr_t>(batch + i);
            CHECK(addr % alignof(AlignedMsg) == 0);
        }
        q.pop_bulk(available);
    }
}

template <typename T, uint32_t Cap>
void testBoundaries() {
    {
        zrqueue::SpscHeapQueue<T> q(Cap);
        testBoundaryOne<T>(q, Cap);
    }
    {
        auto q = std::make_unique<zrqueue::SpscInlineQueue<T, Cap>>();
        testBoundaryOne<T>(*q, Cap);
    }
    {
        auto q = std::make_unique<zrqueue::SpscMirrorQueue<T, Cap>>();
        testBoundaryOne<T>(*q, Cap);
    }
    {
        auto q = std::make_unique<zrqueue::SpscRingBuffer<T, Cap>>();
        testBoundaryOne<T>(*q, Cap);
    }
}

void testBulk() {
    {
        zrqueue::SpscHeapQueue<int> q(8);
        int buf[10] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9 };
        CHECK(q.push_bulk(buf, 10) == 8);
        CHECK(q.size() == 8);
        CHECK(q.push_bulk(buf, 1) == 0);
        int expect = 0;
        CHECK(q.consume_bulk([&](int& v) noexcept { CHECK(v == expect); ++expect; }) == 8);
        CHECK(expect == 8);
        CHECK(q.empty());
    }
    {
        zrqueue::SpscInlineQueue<int, 8> q;
        int buf[3] = { 11, 12, 13 };
        CHECK(q.push_bulk(buf, 3) == 3);
        int expect = 11;
        CHECK(q.consume_bulk([&](int& v) noexcept { CHECK(v == expect); ++expect; }, 2) == 2);
        CHECK(q.size() == 1);
        CHECK(q.consume_bulk([](int& v) noexcept { CHECK(v == 13); }) == 1);
        CHECK(q.empty());
    }
    {
        const int base = PayloadNonPod::alive();
        {
            zrqueue::SpscHeapQueue<PayloadNonPod> q(8);
            PayloadNonPod buf[4] = { PayloadNonPod(0), PayloadNonPod(1),
                                    PayloadNonPod(2), PayloadNonPod(3) };
            CHECK(q.push_bulk(buf, 4) == 4);
            CHECK(q.consume_bulk([](PayloadNonPod&) noexcept {}) == 4);
            CHECK(PayloadNonPod::alive() == base + 4);
        }
        CHECK(PayloadNonPod::alive() == base);
    }
}

void testNonPodLifetime() {
    const int base = PayloadNonPod::alive();
    {
        zrqueue::SpscHeapQueue<PayloadNonPod> q(8);
        for (uint64_t i = 0; i < 5; ++i) {
            q.emplace(i);
        }
        CHECK(PayloadNonPod::alive() == base + 5);
        CHECK(q.consume_bulk([](PayloadNonPod&) noexcept {}) == 5);
        CHECK(PayloadNonPod::alive() == base);
    }
    CHECK(PayloadNonPod::alive() == base);
    {
        zrqueue::SpscHeapQueue<PayloadNonPod> q(8);
        for (uint64_t i = 0; i < 4; ++i) {
            q.emplace(i);
        }
    }
    CHECK(PayloadNonPod::alive() == base);
    {
        auto q = std::make_unique<zrqueue::SpscInlineQueue<PayloadNonPod, 8>>();
        for (uint64_t i = 0; i < 3; ++i) {
            q->emplace(i);
        }
    }
    CHECK(PayloadNonPod::alive() == base);
    {
        auto q = std::make_unique<zrqueue::SpscMirrorQueue<PayloadNonPod, 8>>();
        for (uint64_t i = 0; i < 3; ++i) {
            q->emplace(i);
        }
    }
    CHECK(PayloadNonPod::alive() == base);
    {
        auto q = std::make_unique<zrqueue::SpscRingBuffer<PayloadNonPod, 8>>();
        for (uint64_t i = 0; i < 3; ++i) {
            q->push([&](PayloadNonPod* p) { p->seq = i; });
        }
        CHECK(q->consume_bulk([](PayloadNonPod&) noexcept {}) == 3);
    }
    CHECK(PayloadNonPod::alive() == base);
}

void testClaimCommit() {
    {
        zrqueue::SpscRingBuffer<int, 8> q;
        int* p = q.alloc();
        CHECK(p != nullptr);
        if (p) {
            *p = 42;
        }
        q.push();
        CHECK(q.size() == 1);
        int* f = q.front();
        CHECK(f != nullptr);
        if (f) {
            CHECK(*f == 42);
        }
        q.pop();
        CHECK(q.empty());
    }
    {
        zrqueue::SpscRingBuffer<int, 8> q;
        size_t granted = 0;
        int* batch = q.alloc_bulk(6, granted);
        CHECK(batch != nullptr);
        CHECK(granted == 6);
        for (size_t i = 0; i < granted; ++i) {
            batch[i] = static_cast<int>(i);
        }
        q.commit(3);
        CHECK(q.size() == 3);
        for (int i = 0; i < 3; ++i) {
            int* f = q.front();
            CHECK(f != nullptr);
            if (f) {
                CHECK(*f == i);
            }
            q.pop();
        }
        CHECK(q.empty());
        int* p = q.alloc();
        CHECK(p != nullptr);
        if (p) {
            *p = 999;
        }
        q.push();
        CHECK(q.size() == 1);
        CHECK(*q.front() == 999);
        q.pop();
    }
    {
        zrqueue::SpscRingBuffer<int, 8> q;
        for (int i = 0; i < 6; ++i) {
            int* p = q.alloc();
            CHECK(p != nullptr);
            if (p) {
                *p = i;
            }
            q.push();
        }
        for (int i = 0; i < 6; ++i) {
            q.pop();
        }
        size_t granted = 0;
        int* batch = q.alloc_bulk(8, granted);
        CHECK(batch != nullptr);
        CHECK(granted == 2);
        batch[0] = 60;
        batch[1] = 61;
        q.commit(granted);
        CHECK(q.size() == 2);
        CHECK(*q.front() == 60);
        q.pop();
        CHECK(*q.front() == 61);
        q.pop();
        CHECK(q.empty());
    }
    {
        zrqueue::SpscRingBuffer<int, 8> q;
        for (int i = 0; i < 5; ++i) {
            int* p = q.alloc();
            CHECK(p != nullptr);
            if (p) {
                *p = i;
            }
            q.push();
        }
        int expect = 0;
        CHECK(q.consume_bulk([&](int& v) noexcept { CHECK(v == expect); ++expect; }) == 5);
        CHECK(expect == 5);
        CHECK(q.empty());
    }
    {
        // commit(0) is a no-op, matching pop_bulk(0): nothing is published and the
        // pending claim stays intact for a later commit(n).
        zrqueue::SpscRingBuffer<int, 8> q;
        size_t granted = 0;
        int* batch = q.alloc_bulk(4, granted);
        CHECK(batch != nullptr);
        CHECK(granted == 4);
        q.commit(0);
        CHECK(q.empty());
        CHECK(q.size() == 0);
        batch[0] = 70;
        q.commit(1);
        CHECK(q.size() == 1);
        CHECK(*q.front() == 70);
        q.pop();
        CHECK(q.empty());
    }
}

void suppressAssertDialog() {
#if defined(_MSC_VER)
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
#endif
}

int runDeathChild(const char* name) {
    suppressAssertDialog();
    if (!std::strcmp(name, "double_alloc")) {
        zrqueue::SpscRingBuffer<int, 8> q;
        (void)q.alloc();
        (void)q.alloc();
    } else if (!std::strcmp(name, "double_spin_alloc")) {
        zrqueue::SpscRingBuffer<int, 8> q;
        (void)q.spin_alloc();
        (void)q.spin_alloc();
    } else if (!std::strcmp(name, "bulk_then_push")) {
        zrqueue::SpscRingBuffer<int, 8> q;
        size_t granted = 0;
        (void)q.alloc_bulk(4, granted);
        q.push();
    } else if (!std::strcmp(name, "over_commit")) {
        zrqueue::SpscRingBuffer<int, 8> q;
        size_t granted = 0;
        (void)q.alloc_bulk(4, granted);
        q.commit(granted + 1);
    } else if (!std::strcmp(name, "commit_without_claim")) {
        zrqueue::SpscRingBuffer<int, 8> q;
        q.commit(1);
    } else {
        return 2;
    }
    return 0;
}

bool expectAbort(const char* self, const char* name) {
#if defined(_WIN32)
    std::string cmd = "\"" + std::string(self) + "\" --death=" + name;
    std::vector<char> buf(cmd.begin(), cmd.end());
    buf.push_back('\0');
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessA(nullptr, buf.data(), nullptr, nullptr, FALSE, 0,
                        nullptr, nullptr, &si, &pi)) {
        std::fprintf(stderr, "  CreateProcess failed for %s (%lu)\n", name, GetLastError());
        return false;
    }
    if (WaitForSingleObject(pi.hProcess, 10000) == WAIT_TIMEOUT) {
        TerminateProcess(pi.hProcess, 0xDEAD);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        std::fprintf(stderr, "  death case '%s' timed out (assert dialog?)\n", name);
        return false;
    }
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return code != 0;
#else
    const pid_t pid = fork();
    if (pid < 0) {
        return false;
    }
    if (pid == 0) {
        alarm(10);
        const std::string arg = std::string("--death=") + name;
        execl(self, self, arg.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFSIGNALED(status) || (WIFEXITED(status) && WEXITSTATUS(status) != 0);
#endif
}

void testRingPushLargePayload() {
    // sizeof(T) > cache line takes the blocking spin_alloc branch of push(Writer).
    zrqueue::SpscRingBuffer<PayloadBig, 8> q;
    for (uint64_t i = 0; i < 6; ++i) {
        q.push([&](PayloadBig* p) { p->fill(i); });
    }
    for (uint64_t i = 0; i < 6; ++i) {
        PayloadBig* f = q.front();
        CHECK(f != nullptr);
        if (f) {
            CHECK(f->check(i));
        }
        q.pop();
    }
    CHECK(q.empty());
}

void testMirrorPackedApi() {
    // 4 B explicit payload API.
    {
        zrqueue::SpscMirrorQueue<int, 8> q;
        static_assert(decltype(q)::mirror_enabled, "int must use the 4 B mirror");
        static_assert(decltype(q)::mirror_window == 4, "4 B mirror window must be 4");
        for (uint32_t i = 0; i < 8; ++i) {
            CHECK(q.try_emplace_packed(i, static_cast<int>(i)));
        }
        CHECK(!q.try_emplace_packed(8u, 8));
        for (int i = 0; i < 8; ++i) {
            int* f = q.front();
            CHECK(f != nullptr);
            if (f) {
                CHECK(*f == i);
            }
            q.pop();
        }
        CHECK(q.empty());
        q.push_packed(42u, 42);
        CHECK(*q.front() == 42);
        q.pop();
        CHECK(q.empty());
        CHECK(q.try_push_packed(43u, 43));
        CHECK(*q.front() == 43);
        q.pop();
        CHECK(q.empty());
    }
    // Blocking emplace_packed must refresh the cached read index when full.
    {
        zrqueue::SpscMirrorQueue<int, 2> q;
        q.emplace_packed(1u, 1);
        q.emplace_packed(2u, 2);
        CHECK(*q.front() == 1);
        q.pop();
        q.emplace_packed(3u, 3);
        CHECK(*q.front() == 2);
        q.pop();
        CHECK(*q.front() == 3);
        q.pop();
        CHECK(q.empty());
    }
    // 8 B arithmetic scalar uses the seqlock mirror by default.
    {
        zrqueue::SpscMirrorQueue<uint64_t, 8> q;
        static_assert(decltype(q)::mirror_enabled, "uint64_t must use the 8 B mirror");
        static_assert(decltype(q)::mirror_window == 2, "8 B mirror window must be 2");
        for (uint64_t i = 0; i < 8; ++i) {
            CHECK(q.try_emplace_packed(i, i));
        }
        CHECK(!q.try_emplace_packed(8u, 8u));
        for (uint64_t i = 0; i < 8; ++i) {
            uint64_t* f = q.front();
            CHECK(f != nullptr);
            if (f) {
                CHECK(*f == i);
            }
            q.pop();
        }
        CHECK(q.empty());
        q.push_packed(0x123456789ABCDEF0ULL, 0x123456789ABCDEF0ULL);
        CHECK(*q.front() == 0x123456789ABCDEF0ULL);
        q.pop();
    }
    // Custom 8 B type opted in via mirror_traits; packed payload avoids the
    // two-store read-back stall (correctness is what this test checks).
    {
        zrqueue::SpscMirrorQueue<PayloadTwo32, 8> q;
        static_assert(decltype(q)::mirror_enabled, "PayloadTwo32 must be opted in");
        for (uint64_t i = 0; i < 8; ++i) {
            CHECK(q.try_emplace_packed(i, i));
        }
        CHECK(!q.try_emplace_packed(8u, 8u));
        for (uint64_t i = 0; i < 8; ++i) {
            PayloadTwo32* f = q.front();
            CHECK(f != nullptr);
            if (f) {
                CHECK(f->bits() == i);
            }
            q.pop();
        }
        CHECK(q.empty());
    }
    // 4 B narrow-store type: mirror_codec derives the payload from the
    // constructor argument, so the normal emplace() path stays fast.
    {
        zrqueue::SpscMirrorQueue<PayloadNarrow4, 8> q;
        static_assert(decltype(q)::mirror_enabled, "PayloadNarrow4 must use the 4 B mirror");
        for (uint64_t i = 0; i < 8; ++i) {
            q.emplace(i); // codec path: pack from the argument
        }
        for (uint64_t i = 0; i < 8; ++i) {
            PayloadNarrow4* f = q.front();
            CHECK(f != nullptr);
            if (f) {
                CHECK(f->bits() == i);
            }
            q.pop();
        }
        CHECK(q.empty());
        for (uint64_t i = 8; i < 16; ++i) {
            q.emplace_packed(static_cast<uint32_t>(i), i);
        }
        for (uint64_t i = 8; i < 16; ++i) {
            PayloadNarrow4* f = q.front();
            CHECK(f != nullptr);
            if (f) {
                CHECK(f->bits() == i);
            }
            q.pop();
        }
        CHECK(q.empty());
    }
}

template <typename T>
void testMirrorHitPath() {
    using Q = zrqueue::SpscMirrorQueue<T, 8>;
    static_assert(Q::mirror_enabled, "testMirrorHitPath requires a mirrored type");
    Q q;
    q.emplace(static_cast<uint64_t>(7));
    T* front_ptr = q.front();
    T* slot_ptr = q.peek(0); // peek() always returns the authoritative slot
    CHECK(front_ptr != nullptr);
    CHECK(slot_ptr != nullptr);
    CHECK(front_ptr != slot_ptr); // front() served from the consumer-local stash
    q.pop();
    CHECK(q.empty());
}

template <typename T>
void testMirrorDisabledPath() {
    using Q = zrqueue::SpscMirrorQueue<T, 8>;
    static_assert(!Q::mirror_enabled, "testMirrorDisabledPath requires a classic type");
    Q q;
    q.emplace(static_cast<uint64_t>(7));
    T* front_ptr = q.front();
    T* slot_ptr = q.peek(0);
    CHECK(front_ptr != nullptr);
    CHECK(slot_ptr != nullptr);
    CHECK(front_ptr == slot_ptr); // no mirror: front() returns the slot itself
    q.pop();
    CHECK(q.empty());
}

void testMirrorPolicyFallback() {
    // Explicit opt-out on a 4 B type: no mirror entries are consulted.
    {
        using Q = zrqueue::SpscMirrorQueue<MirrorOptOut4, 8>;
        static_assert(!Q::mirror_enabled, "MirrorOptOut4 must not use the mirror");
        static_assert(Q::mirror_window == 0, "disabled mirror window must be 0");
        Q q;
        for (uint64_t i = 0; i < 8; ++i) {
            CHECK(q.try_emplace(i));
        }
        CHECK(!q.try_emplace(8u));
        for (uint64_t i = 0; i < 8; ++i) {
            MirrorOptOut4* f = q.front();
            CHECK(f != nullptr);
            if (f) {
                CHECK(f->v == i);
            }
            q.pop();
        }
        CHECK(q.empty());
    }
    // Custom 8 B type without an opt-in stays on the classic path.
    {
        using Q = zrqueue::SpscMirrorQueue<MirrorDefault8, 8>;
        static_assert(!Q::mirror_enabled, "custom 8 B types must default to classic");
        Q q;
        for (uint64_t i = 0; i < 8; ++i) {
            CHECK(q.try_emplace(i));
        }
        CHECK(!q.try_emplace(8u));
        for (uint64_t i = 0; i < 8; ++i) {
            MirrorDefault8* f = q.front();
            CHECK(f != nullptr);
            if (f) {
                CHECK(f->lo == static_cast<uint32_t>(i));
                CHECK(f->hi == static_cast<uint32_t>(i >> 32));
            }
            q.pop();
        }
        CHECK(q.empty());
    }
}

void testMirrorConsumeBulk() {
    // Empty queue: nothing consumed, indices untouched.
    {
        zrqueue::SpscMirrorQueue<int, 8> q;
        size_t seen = 0;
        CHECK(q.consume_bulk([&](int&) noexcept { ++seen; }) == 0);
        CHECK(seen == 0);
        CHECK(q.empty());
    }
    // 4 B mirror: a shallow run is served from the mirror entries (seq matches),
    // so the handler must still see the right values in order.
    {
        zrqueue::SpscMirrorQueue<int, 8> q;
        for (int i = 0; i < 5; ++i) {
            q.emplace(i);
        }
        int expect = 0;
        const size_t n = q.consume_bulk([&](int& v) noexcept {
            CHECK(v == expect);
            ++expect;
        });
        CHECK(n == 5);
        CHECK(expect == 5);
        CHECK(q.empty());
        // One release store covered the whole run: the consumer index moved by 5.
        CHECK(q.size() == 0);
    }
    // max_count truncates; the remainder stays for the next call.
    {
        zrqueue::SpscMirrorQueue<int, 8> q;
        for (int i = 0; i < 6; ++i) {
            q.emplace(i);
        }
        int expect = 0;
        CHECK(q.consume_bulk([&](int& v) noexcept {
                  CHECK(v == expect);
                  ++expect;
              }, 4) == 4);
        CHECK(expect == 4);
        CHECK(q.consume_bulk([&](int& v) noexcept {
                  CHECK(v == expect);
                  ++expect;
              }) == 2);
        CHECK(expect == 6);
        CHECK(q.empty());
    }
    // Deep backlog: beyond the mirror window the slots are authoritative, and a
    // wrapped ring (index > N) still yields values in order.
    {
        zrqueue::SpscMirrorQueue<int, 4> q;
        int produced = 0;
        for (int round = 0; round < 5; ++round) {
            for (int i = 0; i < 4; ++i) {
                q.emplace(produced + i);
            }
            produced += 4;
            int expect = round * 4;
            const size_t n = q.consume_bulk([&](int& v) noexcept {
                CHECK(v == expect);
                ++expect;
            });
            CHECK(n == 4);
            CHECK(expect == round * 4 + 4);
        }
        CHECK(q.empty());
    }
    // Stale cached producer index: after a partial drain, cached_write_index_
    // lags the real write_index_, so the mirror depth pre-filter is an
    // UNDER-estimate for the remaining elements. Those must fall back to the
    // authoritative slots and still yield the right values in order.
    {
        zrqueue::SpscMirrorQueue<int, 8> q;
        for (int i = 0; i < 8; ++i) {
            q.emplace(i);
        }
        int expect = 0;
        CHECK(q.consume_bulk([&](int& v) noexcept {
                  CHECK(v == expect);
                  ++expect;
              }, 2) == 2); // read=2, cached_write_index_ = 8
        CHECK(expect == 2);
        q.emplace(8);
        q.emplace(9); // write=10; slots 0 and 1 were recycled by 8 and 9
        // cached_write_index_ is still 8, so only indices 2..7 are known.
        CHECK(q.consume_bulk([&](int& v) noexcept {
                  CHECK(v == expect);
                  ++expect;
              }) == 6);
        CHECK(expect == 8);
        // The next call refreshes the cached index and sees 8 and 9.
        CHECK(q.consume_bulk([&](int& v) noexcept {
                  CHECK(v == expect);
                  ++expect;
              }) == 2);
        CHECK(expect == 10);
        CHECK(q.empty());
    }

    // 8 B scalar mirror (seqlock path) drains correctly.
    {
        zrqueue::SpscMirrorQueue<uint64_t, 8> q;
        for (uint64_t i = 0; i < 6; ++i) {
            q.emplace(i * 0x0101010101010101ULL);
        }
        uint64_t expect = 0;
        CHECK(q.consume_bulk([&](uint64_t& v) noexcept {
                  CHECK(v == expect);
                  expect += 0x0101010101010101ULL;
              }) == 6);
        CHECK(q.empty());
    }
    // Non-trivial T: every element is destructed exactly once, including the ones
    // the handler observed through the mirror stash.
    {
        zrqueue::SpscMirrorQueue<PayloadBig, 4> q;
        PayloadBig v;
        v.fill(3);
        for (int i = 0; i < 3; ++i) {
            q.emplace(v);
        }
        size_t seen = 0;
        CHECK(q.consume_bulk([&](PayloadBig& p) noexcept {
                  CHECK(p.check(3));
                  ++seen;
              }) == 3);
        CHECK(seen == 3);
        CHECK(q.empty());
    }
}

void testMirrorPushBulk() {
    // Basic: 5 elements in, 5 out, in order.
    {
        zrqueue::SpscMirrorQueue<int, 8> q;
        const int src[] = { 10, 11, 12, 13, 14 };
        CHECK(q.push_bulk(src, 5) == 5);
        for (int i = 0; i < 5; ++i) {
            int* f = q.front();
            CHECK(f != nullptr);
            if (f) {
                CHECK(*f == 10 + i);
            }
            q.pop();
        }
        CHECK(q.empty());
    }
    // count == 0 is a no-op.
    {
        zrqueue::SpscMirrorQueue<int, 8> q;
        const int src[] = { 1 };
        CHECK(q.push_bulk(src, 0) == 0);
        CHECK(q.empty());
    }
    // Partial push: only the free slots are written, the rest is reported.
    {
        zrqueue::SpscMirrorQueue<int, 8> q;
        const int src[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9 };
        CHECK(q.push_bulk(src, 8) == 8);
        CHECK(q.push_bulk(src, 4) == 0); // full
        CHECK(q.consume_bulk([](int&) noexcept {}) == 8);
        CHECK(q.push_bulk(src, 10) == 8); // capacity is 8
        int expect = 0;
        CHECK(q.consume_bulk([&](int& v) noexcept {
                  CHECK(v == expect);
                  ++expect;
              }) == 8);
        CHECK(expect == 8);
    }
    // A run longer than the mirror window still drains correctly: elements past
    // the window are served from their slots.
    {
        zrqueue::SpscMirrorQueue<int, 16> q;
        const int src[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 };
        CHECK(q.push_bulk(src, 12) == 12);
        int expect = 0;
        CHECK(q.consume_bulk([&](int& v) noexcept {
                  CHECK(v == expect);
                  ++expect;
              }) == 12);
        CHECK(expect == 12);
        CHECK(q.empty());
    }
    // The trailing K elements are readable through the mirror: after a bulk push
    // of exactly K, front() must serve from the consumer-local stash.
    {
        zrqueue::SpscMirrorQueue<int, 8> q;
        const int src[] = { 7, 8, 9, 10 };
        CHECK(q.push_bulk(src, 4) == 4); // 4 B mirror window is K = 4
        CHECK(q.front() != q.peek(0));   // mirror hit: stash, not the slot
        CHECK(*q.front() == 7);
        q.pop();
        CHECK(q.empty() == false);
    }
    // 8 B seqlock mirror path.
    {
        zrqueue::SpscMirrorQueue<uint64_t, 8> q;
        const uint64_t src[] = { 0x1111, 0x2222, 0x3333 };
        CHECK(q.push_bulk(src, 3) == 3);
        for (int i = 0; i < 3; ++i) {
            uint64_t* f = q.front();
            CHECK(f != nullptr);
            if (f) {
                CHECK(*f == src[i]);
            }
            q.pop();
        }
        CHECK(q.empty());
    }
    // Non-mirrored type: bulk push still works, no entries are consulted.
    {
        zrqueue::SpscMirrorQueue<MirrorOptOut4, 8> q;
        MirrorOptOut4 src[3];
        src[0].v = 1; src[1].v = 2; src[2].v = 3;
        CHECK(q.push_bulk(src, 3) == 3);
        for (uint32_t i = 0; i < 3; ++i) {
            MirrorOptOut4* f = q.front();
            CHECK(f != nullptr);
            if (f) {
                CHECK(f->v == i + 1);
            }
            q.pop();
        }
        CHECK(q.empty());
    }
    // Wrapped ring: a second bulk push lands across the wrap point and still
    // reads back in order.
    {
        zrqueue::SpscMirrorQueue<int, 4> q;
        const int a[] = { 0, 1, 2 };
        CHECK(q.push_bulk(a, 3) == 3);
        CHECK(q.consume_bulk([](int&) noexcept {}) == 3);
        const int b[] = { 3, 4, 5, 6 };
        CHECK(q.push_bulk(b, 4) == 4);
        int expect = 3;
        CHECK(q.consume_bulk([&](int& v) noexcept {
                  CHECK(v == expect);
                  ++expect;
              }) == 4);
        CHECK(expect == 7);
        CHECK(q.empty());
    }
}

void testRingValuePush() {
    {
        zrqueue::SpscRingBuffer<int, 8> q;
        for (int i = 0; i < 6; ++i) {
            q.push_value(i);
        }
        for (int i = 0; i < 6; ++i) {
            int* f = q.front();
            CHECK(f != nullptr);
            if (f) {
                CHECK(*f == i);
            }
            q.pop();
        }
        CHECK(q.empty());
        for (int i = 0; i < 8; ++i) {
            CHECK(q.try_push_value(i));
        }
        CHECK(!q.try_push_value(99));
        for (int i = 0; i < 8; ++i) {
            int* f = q.front();
            CHECK(f != nullptr);
            if (f) {
                CHECK(*f == i);
            }
            q.pop();
        }
        CHECK(q.empty());
        q.push_value(42);
        CHECK(*q.front() == 42);
        q.pop();
        CHECK(q.empty());
    }
    {
        // Multi-cache-line value path exercises spin_alloc().
        zrqueue::SpscRingBuffer<PayloadBig, 4> q;
        PayloadBig v;
        v.fill(7);
        q.push_value(v);
        PayloadBig* f = q.front();
        CHECK(f != nullptr);
        if (f) {
            CHECK(f->check(7));
        }
        q.pop();
        CHECK(q.empty());
    }
}

void testDeathCases(const char* self) {
    static const char* cases[] = {
        "double_alloc", "double_spin_alloc", "bulk_then_push",
        "over_commit", "commit_without_claim"
    };
    for (const char* c : cases) {
        ++g_checks;
        if (!expectAbort(self, c)) {
            ++g_failures;
            std::fprintf(stderr, "FAIL death case '%s' did not abort (assertions disabled?)\n", c);
        }
    }
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc >= 2 && std::strncmp(argv[1], "--death=", 8) == 0) {
        return runDeathChild(argv[1] + 8);
    }

    // Death cases fork/exec a child; skip them under TSAN, whose runtime does
    // not tolerate fork in an instrumented process.
    bool runDeath = true;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--no-death")) {
            runDeath = false;
        }
    }

    std::printf("zrqueue unit tests: assertions %s\n",
#ifdef NDEBUG
                "DISABLED (claim/commit tests will fail)"
#else
                "ENABLED"
#endif
    );

    testAllModels<int, 8>("int");
    testAllModels<PayloadBig, 8>("256B");
    testAllModels<PayloadNonPod, 8>("non-POD");
    testAllModels<PayloadScalar8, 8>("8B scalar");
    testAllModels<PayloadTwo32, 8>("8B two32");
    testAllModels<PayloadNarrow4, 8>("4B narrow");

    testBoundaries<int, 8>();
    testBoundaries<int, 2>();
    testBoundaries<PayloadBig, 8>();
    testBoundaries<PayloadNonPod, 8>();
    testBoundaries<PayloadScalar8, 8>();
    testBoundaries<PayloadScalar8, 2>();
    testBoundaries<PayloadTwo32, 8>();
    testBoundaries<PayloadTwo32, 2>();
    testBoundaries<PayloadNarrow4, 8>();
    testBoundaries<PayloadNarrow4, 2>();

    testPeekOverflow<int, 8>();
    testPeekOverflow<int, 2>();
    testPeekOverflow<PayloadNonPod, 8>();
    testPeekOverflow<PayloadScalar8, 8>();
    testPeekOverflow<PayloadTwo32, 8>();

    testFrontBulk<int, 8>();
    testFrontBulk<int, 2>();
    testFrontBulk<PayloadNonPod, 8>();
    testFrontBulkLifetime();
    testOverAlignedType();

    testBulk();
    testNonPodLifetime();
    testClaimCommit();
    testMirrorPackedApi();
    testMirrorHitPath<int>();
    testMirrorHitPath<uint64_t>();
    testMirrorHitPath<PayloadScalar8>();
    testMirrorHitPath<PayloadTwo32>();
    testMirrorHitPath<PayloadNarrow4>();
    testMirrorDisabledPath<MirrorOptOut4>();
    testMirrorDisabledPath<MirrorDefault8>();
    testMirrorPolicyFallback();
    testRingValuePush();
    testMirrorConsumeBulk();
    testMirrorPushBulk();
    testRingPushLargePayload();
    if (runDeath) {
        testDeathCases(argv[0]);
    }

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
