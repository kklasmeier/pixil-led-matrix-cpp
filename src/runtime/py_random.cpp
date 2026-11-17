#include "runtime/py_random.h"

#include <random>
#include <vector>

namespace pixil {

namespace {
constexpr int M = 397;
constexpr uint32_t MATRIX_A = 0x9908b0dfU;
constexpr uint32_t UPPER_MASK = 0x80000000U;
constexpr uint32_t LOWER_MASK = 0x7fffffffU;
}  // namespace

PyRandom::PyRandom() { seed_from_entropy(); }

void PyRandom::init_genrand(uint32_t s) {
    mt_[0] = s;
    for (int i = 1; i < N; ++i) {
        mt_[i] = 1812433253U * (mt_[i - 1] ^ (mt_[i - 1] >> 30)) + static_cast<uint32_t>(i);
    }
    mti_ = N;
}

void PyRandom::init_by_array(const uint32_t* key, size_t key_length) {
    init_genrand(19650218U);
    size_t i = 1, j = 0;
    size_t k = (static_cast<size_t>(N) > key_length ? static_cast<size_t>(N) : key_length);
    for (; k; --k) {
        mt_[i] = (mt_[i] ^ ((mt_[i - 1] ^ (mt_[i - 1] >> 30)) * 1664525U)) + key[j] +
                 static_cast<uint32_t>(j);
        ++i;
        ++j;
        if (i >= static_cast<size_t>(N)) {
            mt_[0] = mt_[N - 1];
            i = 1;
        }
        if (j >= key_length) j = 0;
    }
    for (k = N - 1; k; --k) {
        mt_[i] = (mt_[i] ^ ((mt_[i - 1] ^ (mt_[i - 1] >> 30)) * 1566083941U)) -
                 static_cast<uint32_t>(i);
        ++i;
        if (i >= static_cast<size_t>(N)) {
            mt_[0] = mt_[N - 1];
            i = 1;
        }
    }
    mt_[0] = 0x80000000U;
    mti_ = N;
}

void PyRandom::seed(int64_t n) {
    // CPython: key = 32-bit little-endian words of abs(n); at least one word.
    uint64_t a = n < 0 ? static_cast<uint64_t>(-(n + 1)) + 1U : static_cast<uint64_t>(n);
    uint32_t key[2] = {static_cast<uint32_t>(a & 0xffffffffU), static_cast<uint32_t>(a >> 32)};
    init_by_array(key, key[1] ? 2 : 1);
}

void PyRandom::seed_from_entropy() {
    std::random_device rd;
    std::vector<uint32_t> key(N);
    for (auto& w : key) w = rd();
    init_by_array(key.data(), key.size());
}

uint32_t PyRandom::genrand_uint32() {
    static constexpr uint32_t mag01[2] = {0x0U, MATRIX_A};
    uint32_t y;
    if (mti_ >= N) {
        int kk = 0;
        for (; kk < N - M; ++kk) {
            y = (mt_[kk] & UPPER_MASK) | (mt_[kk + 1] & LOWER_MASK);
            mt_[kk] = mt_[kk + M] ^ (y >> 1) ^ mag01[y & 0x1U];
        }
        for (; kk < N - 1; ++kk) {
            y = (mt_[kk] & UPPER_MASK) | (mt_[kk + 1] & LOWER_MASK);
            mt_[kk] = mt_[kk + (M - N)] ^ (y >> 1) ^ mag01[y & 0x1U];
        }
        y = (mt_[N - 1] & UPPER_MASK) | (mt_[0] & LOWER_MASK);
        mt_[N - 1] = mt_[M - 1] ^ (y >> 1) ^ mag01[y & 0x1U];
        mti_ = 0;
    }
    y = mt_[mti_++];
    y ^= (y >> 11);
    y ^= (y << 7) & 0x9d2c5680U;
    y ^= (y << 15) & 0xefc60000U;
    y ^= (y >> 18);
    return y;
}

double PyRandom::random() {
    uint32_t a = genrand_uint32() >> 5;
    uint32_t b = genrand_uint32() >> 6;
    return (a * 67108864.0 + b) * (1.0 / 9007199254740992.0);
}

}  // namespace pixil
