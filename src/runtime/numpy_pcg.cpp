// numpy 1.24 Generator used by chladni. Each unseeded default_rng() in a
// capture is seeded [script_seed, n]. The bit generator is PCG64 (XSL-RR).
// integers() on a range that fits in 32 bits uses the buffered uint32
// stream and Lemire's method, which is what Generator.integers does.
#include "runtime/grid.h"

namespace pixil {
namespace {

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
using u128 = unsigned __int128;
#pragma GCC diagnostic pop

constexpr u128 kMult = (u128(2549297995355413924ull) << 64) | 4865540595714422341ull;

constexpr uint32_t kInitA = 0x43b0d7e5u;
constexpr uint32_t kMultA = 0x931e8875u;
constexpr uint32_t kInitB = 0x8b51f9ddu;
constexpr uint32_t kMultB = 0x58f38dedu;
constexpr uint32_t kMixL = 0xca01f9ddu;
constexpr uint32_t kMixR = 0x4973f715u;

u128 g_state = 0;
u128 g_inc = 0;
bool g_ready = false;
bool g_has32 = false;
uint32_t g_buf32 = 0;
uint32_t g_script_seed = 1;
uint32_t g_counter = 0;

uint32_t hash_mix(uint32_t value, uint32_t& hash_const) {
    value ^= hash_const;
    hash_const *= kMultA;
    value *= hash_const;
    value ^= value >> 16;
    return value;
}

uint32_t mix(uint32_t x, uint32_t y) {
    uint32_t result = kMixL * x - kMixR * y;
    result ^= result >> 16;
    return result;
}

uint64_t rot_r(uint64_t value, unsigned shift) {
    shift &= 63u;
    if (shift == 0) return value;
    return (value >> shift) | (value << (64u - shift));
}

void step() { g_state = g_state * kMult + g_inc; }

uint64_t raw64() {
    step();
    uint64_t hi = static_cast<uint64_t>(g_state >> 64);
    uint64_t lo = static_cast<uint64_t>(g_state);
    return rot_r(hi ^ lo, static_cast<unsigned>(hi >> 58));
}

uint32_t raw32() {
    if (g_has32) {
        g_has32 = false;
        return g_buf32;
    }
    uint64_t next = raw64();
    g_has32 = true;
    g_buf32 = static_cast<uint32_t>(next >> 32);
    return static_cast<uint32_t>(next);
}

// Closed interval [0, rng]. rng must not be 0xFFFFFFFF.
uint32_t lemire32(uint32_t rng) {
    const uint32_t span = rng + 1u;
    auto draw = [&]() {
        uint64_t product = static_cast<uint64_t>(raw32()) * span;
        return product;
    };
    uint64_t product = draw();
    uint32_t leftover = static_cast<uint32_t>(product);
    if (leftover < span) {
        const uint32_t threshold = (0xffffffffu - rng) % span;
        while (leftover < threshold) {
            product = draw();
            leftover = static_cast<uint32_t>(product);
        }
    }
    return static_cast<uint32_t>(product >> 32);
}

void seed_pcg(uint32_t a, uint32_t b) {
    uint32_t pool[4] = {};
    uint32_t entropy[2] = {a, b};
    uint32_t hash_const = kInitA;
    for (int i = 0; i < 4; ++i) {
        uint32_t src = i < 2 ? entropy[i] : 0u;
        pool[i] = hash_mix(src, hash_const);
    }
    for (int src = 0; src < 4; ++src) {
        for (int dst = 0; dst < 4; ++dst) {
            if (src != dst) pool[dst] = mix(pool[dst], hash_mix(pool[src], hash_const));
        }
    }
    uint32_t words[8];
    hash_const = kInitB;
    for (int i = 0; i < 8; ++i) {
        uint32_t data = pool[i % 4];
        data ^= hash_const;
        hash_const *= kMultB;
        data *= hash_const;
        data ^= data >> 16;
        words[i] = data;
    }
    auto pair = [&](int i) -> uint64_t {
        return static_cast<uint64_t>(words[i]) | (static_cast<uint64_t>(words[i + 1]) << 32);
    };
    uint64_t w0 = pair(0), w1 = pair(2), w2 = pair(4), w3 = pair(6);
    u128 initstate = static_cast<u128>(w1) | (static_cast<u128>(w0) << 64);
    u128 initseq = static_cast<u128>(w3) | (static_cast<u128>(w2) << 64);
    g_inc = (initseq << 1) | 1;
    g_state = 0;
    step();
    g_state += initstate;
    step();
    g_has32 = false;
    g_ready = true;
}

}  // namespace

void note_numpy_script_seed(uint32_t seed) {
    g_script_seed = seed;
    g_counter = 0;
    g_ready = false;
    g_has32 = false;
}

void numpy_reseed() {
    if (g_counter == 0xffffffffu) {
        throw PixilError(ErrorKind::Value, "numpy generator counter overflow");
    }
    ++g_counter;
    seed_pcg(g_script_seed, g_counter);
}

int64_t numpy_integers(int64_t lo, int64_t hi) {
    if (!g_ready) throw PixilError(ErrorKind::Value, "numpy generator is not seeded");
    if (hi <= lo) throw PixilError(ErrorKind::Value, "numpy integers high must be above low");
    const int64_t span = hi - lo;
    if (span <= 0 || span > 0x100000000LL) {
        throw PixilError(ErrorKind::Value, "numpy integers range is out of the 32-bit path");
    }
    const uint32_t rng = static_cast<uint32_t>(span - 1);
    const uint64_t off = static_cast<uint64_t>(lo);
    return static_cast<int64_t>(off + lemire32(rng));
}

}  // namespace pixil
