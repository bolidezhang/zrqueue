#pragma once

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

    explicit PayloadBig(uint64_t s = 0) noexcept : seq(s), pad{} { fill(s); }

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

    explicit PayloadNonPod(uint64_t s = 0) noexcept : seq(s) { live.fetch_add(1, std::memory_order_relaxed); }
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

    explicit PayloadSized(uint64_t s = 0) noexcept : seq(static_cast<uint32_t>(s)), pad{} { fill(s); }

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
    explicit PayloadSized(uint64_t s = 0) noexcept : seq(static_cast<uint32_t>(s)) {}
    void fill(uint64_t s) noexcept { seq = static_cast<uint32_t>(s); }
};
