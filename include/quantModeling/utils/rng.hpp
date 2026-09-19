#ifndef UTILS_RNG_HPP
#define UTILS_RNG_HPP

#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>

struct Pcg32
{
    using result_type = uint32_t;

    uint64_t state = 0;
    uint64_t inc = 0; // stream selector (odd)

    Pcg32(uint64_t seed, uint64_t stream_id)
    {
        // stream_id defines an independant stream (inc must be odd)
        inc = (stream_id << 1u) | 1u;
        seed_rng(seed);
    }

    void seed_rng(uint64_t seed)
    {
        state = 0;
        (*this)();
        state += seed;
        (*this)();
    }

    uint32_t operator()()
    {
        uint64_t oldstate = state;
        state = oldstate * 6364136223846793005ULL + inc;
        uint32_t xorshifted =
            static_cast<uint32_t>(((oldstate >> 18u) ^ oldstate) >> 27u);
        uint32_t rot = static_cast<uint32_t>(oldstate >> 59u);
        return (xorshifted >> rot) | (xorshifted << ((-rot) & 31));
    }

    /**
     * @brief Jump `n` draws ahead (or behind, via wraparound) in O(log n),
     *        without generating them (blueprint/wp/17-aad.md §8.2).
     *
     * The state transition state' = state*mult + inc is an affine map;
     * applying it n times is itself affine, state' = acc_mult*state +
     * acc_plus, and acc_mult/acc_plus telescope under repeated squaring
     * exactly the way a fast power does -- this is the standard PCG-library
     * "advance" trick (O'Neill, 2014), not something specific to this repo.
     * All arithmetic is mod 2^64 via natural uint64_t overflow.
     */
    void skip(uint64_t n)
    {
        uint64_t cur_mult = 6364136223846793005ULL;
        uint64_t cur_plus = inc;
        uint64_t acc_mult = 1u, acc_plus = 0u;
        while (n > 0)
        {
            if (n & 1u)
            {
                acc_mult *= cur_mult;
                acc_plus = acc_plus * cur_mult + cur_plus;
            }
            cur_plus = (cur_mult + 1u) * cur_plus;
            cur_mult *= cur_mult;
            n >>= 1u;
        }
        state = acc_mult * state + acc_plus;
    }

    static constexpr uint32_t min() { return 0u; }
    static constexpr uint32_t max() { return 0xFFFFFFFFu; }
};

inline double uniform01(Pcg32 &rng)
{
    constexpr double inv =
        1.0 / (double(std::numeric_limits<uint32_t>::max()) + 1.0);
    double u = (double(rng()) + 0.5) * inv; // (0,1)
    return u;
}

struct NormalBoxMuller
{
    bool hasSpare = false;
    double spare = 0.0;

    double operator()(Pcg32 &rng)
    {
        if (hasSpare)
        {
            hasSpare = false;
            return spare;
        }
        double u1 = uniform01(rng);
        double u2 = uniform01(rng);

        double r = std::sqrt(-2.0 * std::log(u1));
        double theta = 2.0 * M_PI * u2;

        double z0 = r * std::cos(theta);
        double z1 = r * std::sin(theta);

        spare = z1;
        hasSpare = true;
        return z0;
    }
};

struct RngFactory
{
    uint64_t master_seed;
    explicit RngFactory(uint64_t seed)
        : master_seed(seed) {}

    Pcg32 make(uint64_t stream_id) const { return Pcg32(master_seed, stream_id); }
};

/**
 * Wrapper for Gaussian generator that supports antithetic variance reduction.
 * When antithetic mode is enabled, successive calls return negated pairs:
 * call 1 -> z, call 2 -> -z, call 3 -> z', call 4 -> -z', etc.
 * This allows the caller to transparently get pairs without managing negation logic.
 */
struct AntitheticGaussianGenerator
{
    NormalBoxMuller inner;
    bool antithetic_enabled = false;
    int call_count = 0; // counts calls since stream_start=true for toggling negation

    /**
   * Get next Gaussian sample.
   * If antithetic_enabled: returns z on even calls, -z on odd calls
   * Otherwise: always returns z
   */
    double operator()(Pcg32 &rng)
    {
        double z = inner(rng);

        if (antithetic_enabled)
        {
            // Alternate: even calls return z, odd calls return -z
            if ((call_count & 1) == 1)
                z = -z;
            ++call_count;
        }

        return z;
    }

    void enable_antithetic() { antithetic_enabled = true; }
    void disable_antithetic() { antithetic_enabled = false; }
    void reset_call_count() { call_count = 0; }
};

#endif // UTILS_RNG_HPP