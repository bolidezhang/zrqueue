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

    static int alive() noexcept { return live.load(std::memory_order_relaxed); }
};
static_assert(!std::is_trivially_destructible_v<PayloadNonPod>, "PayloadNonPod must exercise destructor paths");
