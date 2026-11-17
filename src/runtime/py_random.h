// MT19937 seeded and sampled exactly like CPython's `random` module, so a
// seeded native run draws the same random() sequence as a seeded legacy run.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace pixil {

class PyRandom {
public:
    PyRandom();  // nondeterministic seed (like CPython's os.urandom seeding)

    // random.seed(n) for a Python int n.
    void seed(int64_t n);
    void seed_from_entropy();
    // random.random(): 53-bit float in [0, 1).
    double random();
    uint32_t genrand_uint32();

private:
    void init_genrand(uint32_t s);
    void init_by_array(const uint32_t* key, size_t key_length);

    static constexpr int N = 624;
    std::array<uint32_t, N> mt_{};
    int mti_ = N + 1;
};

}  // namespace pixil
