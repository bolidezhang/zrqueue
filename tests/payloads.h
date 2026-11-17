#pragma once

#include "../zrqueue.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

// 256-byte, trivially destructible payload. Larger than a cache line, so it
// exercises the non-MIRROR_OK path and cache/TLB pressure.
struct PayloadBig {
    static constexpr std::size_t kBytes = 256;
    uint64_t seq;
    std::array<unsigned char, kBytes - sizeof(uint64_t)> pad;

    PayloadBig(uint64_t s = 0) noexcept : seq(s), pad{} { fill(s); }

    void fill(uint64_t s) noexcept {
        seq = s;
        for (std::size_t i = 0; i < pad.size(); ++i) {
            pad[i] = static_cast<unsigned char>((s + i * 31u) & 0xFFu);
        }
    }

    bool check(uint64_t s) const noexcept {
        if (seq != s) {
            return false;
        }
        for (std::size_t i = 0; i < pad.size(); ++i) {
            if (pad[i] != static_cast<unsigned char>((s + i * 31u) & 0xFFu)) {
                return false;
            }
        }
        return true;
    }
};
static_assert(sizeof(PayloadBig) == PayloadBig::kBytes, "PayloadBig must be exactly 256 bytes");
static_assert(std::is_trivially_destructible_v<PayloadBig>, "PayloadBig must stay trivially destructible");

// Non-trivially-destructible payload with no heap allocation. `live` lets tests
// prove that pop()/consume_bulk()/queue destruction call ~T() exactly once.
struct PayloadNonPod {
    uint64_t seq;
    static inline std::atomic<int> live{ 0 };

    PayloadNonPod(uint64_t s = 0) noexcept : seq(s) { live.fetch_add(1, std::memory_order_relaxed); }
    PayloadNonPod(const PayloadNonPod& o) noexcept : seq(o.seq) { live.fetch_add(1, std::memory_order_relaxed); }
    PayloadNonPod(PayloadNonPod&& o) noexcept : seq(o.seq) { live.fetch_add(1, std::memory_order_relaxed); }
    PayloadNonPod& operator=(const PayloadNonPod&) noexcept = default;
    PayloadNonPod& operator=(PayloadNonPod&&) noexcept = default;
    ~PayloadNonPod() { live.fetch_sub(1, std::memory_order_relaxed); }

    void fill(uint64_t s) noexcept { seq = s; }

    static int alive() noexcept { return live.load(std::memory_order_relaxed); }
};
static_assert(!std::is_trivially_destructible_v<PayloadNonPod>, "PayloadNonPod must exercise destructor paths");

// Fixed-size trivially-copyable payload for element-length sweeps.
// uint32_t seq + byte padding yields sizeof == N for any N divisible by 4.
template <std::size_t N>
struct PayloadSized {
    static_assert(N >= 4 && N % 4 == 0, "N must be a multiple of 4");
    uint32_t seq;
    std::array<unsigned char, N - sizeof(uint32_t)> pad;

    PayloadSized(uint64_t s = 0) noexcept : seq(static_cast<uint32_t>(s)), pad{} { fill(s); }

    void fill(uint64_t s) noexcept {
        seq = static_cast<uint32_t>(s);
        for (std::size_t i = 0; i < pad.size(); ++i) {
            pad[i] = static_cast<unsigned char>((s + i * 31u) & 0xFFu);
        }
    }
};

// std::array<T,0> is not zero-sized in MSVC, so the 4-byte case needs a
// padding-free specialization to keep sizeof == 4.
template <>
struct PayloadSized<4> {
    uint32_t seq;
    PayloadSized(uint64_t s = 0) noexcept : seq(static_cast<uint32_t>(s)) {}
    void fill(uint64_t s) noexcept { seq = static_cast<uint32_t>(s); }
};

// ============================================================================
// Mirror-path payloads
// ============================================================================

// 8 B scalar: one 8 B store, so the default mirror read-back forwards cheaply.
struct PayloadScalar8 {
    uint64_t seq;
    PayloadScalar8(uint64_t s = 0) noexcept : seq(s) {}
    void fill(uint64_t s) noexcept { seq = s; }
};
static_assert(sizeof(PayloadScalar8) == 8, "PayloadScalar8 must be 8 bytes");
static_assert(std::is_trivially_copyable_v<PayloadScalar8>,
              "PayloadScalar8 must be trivially copyable");

// Same layout and constructor as PayloadScalar8, but the mirror is disabled at
// compile time. Benchmarking the pair isolates the 8 B mirror's own cost/benefit.
struct PayloadScalar8Classic {
    uint64_t seq;
    PayloadScalar8Classic(uint64_t s = 0) noexcept : seq(s) {}
    void fill(uint64_t s) noexcept { seq = s; }
};
static_assert(sizeof(PayloadScalar8Classic) == 8, "PayloadScalar8Classic must be 8 bytes");
static_assert(std::is_trivially_copyable_v<PayloadScalar8Classic>,
              "PayloadScalar8Classic must be trivially copyable");

// 8 B with a two-store constructor: the default mirror read-back defeats
// store-to-load forwarding. Exercises emplace_packed()/push_packed().
struct PayloadTwo32 {
    uint32_t lo, hi;
    PayloadTwo32(uint64_t s = 0) noexcept
        : lo(static_cast<uint32_t>(s)), hi(static_cast<uint32_t>(s >> 32)) {}
    void fill(uint64_t s) noexcept {
        lo = static_cast<uint32_t>(s);
        hi = static_cast<uint32_t>(s >> 32);
    }
    uint64_t bits() const noexcept { return (static_cast<uint64_t>(hi) << 32) | lo; }
};
static_assert(sizeof(PayloadTwo32) == 8, "PayloadTwo32 must be 8 bytes");
static_assert(std::is_trivially_copyable_v<PayloadTwo32>,
              "PayloadTwo32 must be trivially copyable");

// 4 B with four byte stores: the same forwarding cliff on the 4 B path.
// mirror_codec supplies the payload from the constructor argument, so
// emplace(uint64_t) avoids the slot read-back.
struct PayloadNarrow4 {
    unsigned char a, b, c, d;
    PayloadNarrow4(uint64_t s = 0) noexcept { fill(s); }
    void fill(uint64_t s) noexcept {
        a = static_cast<unsigned char>(s);
        b = static_cast<unsigned char>(s >> 8);
        c = static_cast<unsigned char>(s >> 16);
        d = static_cast<unsigned char>(s >> 24);
    }
    uint32_t bits() const noexcept {
        return static_cast<uint32_t>(a) | (static_cast<uint32_t>(b) << 8) |
               (static_cast<uint32_t>(c) << 16) | (static_cast<uint32_t>(d) << 24);
    }
};
static_assert(sizeof(PayloadNarrow4) == 4, "PayloadNarrow4 must be 4 bytes");
static_assert(std::is_trivially_copyable_v<PayloadNarrow4>,
              "PayloadNarrow4 must be trivially copyable");

namespace zrqueue {
template <>
struct mirror_traits<PayloadScalar8> {
    static constexpr bool enable_4b = false;
    static constexpr bool enable_8b = true;
};

template <>
struct mirror_traits<PayloadScalar8Classic> {
    static constexpr bool enable_4b = false;
    static constexpr bool enable_8b = false;
};

template <>
struct mirror_traits<PayloadTwo32> {
    static constexpr bool enable_4b = false;
    static constexpr bool enable_8b = true;
};

template <>
struct mirror_codec<PayloadNarrow4> {
    static uint32_t pack(uint64_t s) noexcept { return static_cast<uint32_t>(s); }
};
} // namespace zrqueue
