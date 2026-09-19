#ifndef QM_UTILS_RNG_INTERFACE_HPP
#define QM_UTILS_RNG_INTERFACE_HPP

#include "quantModeling/utils/inverse_normal.hpp"
#include "quantModeling/utils/rng.hpp"
#include "quantModeling/utils/sobol.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>

namespace quantModeling
{

    /**
     * @brief The book's RNG interface (blueprint/wp/17-aad.md §8.2), the
     *        seam parallel AAD needs: `skip_to(path)` must land a thread on
     *        exactly the gaussians path `path` would draw if generated
     *        sequentially, regardless of which thread computes it or in
     *        what order threads pick up work -- that is the entire
     *        mechanism behind simulate_parallel_aad's bit-for-bit
     *        reproducibility (§8.4).
     *
     * Gaussians only ever come from next_g(), never Box-Muller: Box-Muller's
     * one-draw-in-reserve state depends on call history, so a jump-ahead
     * would desynchronise it from what a sequential run would have produced
     * (§8.2's own constraint) -- next_g() always goes through the inverse
     * normal CDF, stateless by construction.
     */
    class RNG
    {
      public:
        virtual ~RNG() = default;

        /// Sets the number of coordinates one path draws. Called once,
        /// before any next_u()/next_g()/skip_to() call.
        virtual void init(std::size_t sim_dim) = 0;

        virtual void next_u(std::span<double> out) = 0;
        virtual void next_g(std::span<double> out) = 0;

        /// An independent copy in its own, freshly-seeded state -- used to
        /// give each thread of simulate_parallel_aad its own generator
        /// (ADR-A8; ThreadPool workers must never share one RNG instance).
        virtual std::unique_ptr<RNG> clone() const = 0;

        /// Positions the generator so the *next* next_u()/next_g() call
        /// returns path `path`'s own first coordinate -- in O(log path) for
        /// PCG32, O(sim_dim) for Sobol, never by drawing and discarding the
        /// coordinates in between.
        virtual void skip_to(std::size_t path) = 0;

        virtual std::size_t sim_dim() const = 0;
    };

    /**
     * @brief PCG32-backed RNG: skip_to(path) jumps path*sim_dim() draws
     *        ahead via Pcg32::skip()'s O(log n) binary exponentiation.
     */
    class Pcg32RNG final : public RNG
    {
      public:
        Pcg32RNG(uint64_t seed, uint64_t stream_id)
            : seed_(seed), stream_id_(stream_id), gen_(seed, stream_id)
        {
        }

        void init(std::size_t sim_dim) override { dim_ = sim_dim; }

        void next_u(std::span<double> out) override
        {
            for (double &x : out)
                x = uniform01(gen_);
        }

        void next_g(std::span<double> out) override
        {
            next_u(out);
            for (double &x : out)
                x = inverse_normal_cdf(x);
        }

        std::unique_ptr<RNG> clone() const override
        {
            return std::make_unique<Pcg32RNG>(*this);
        }

        void skip_to(std::size_t path) override
        {
            gen_ = Pcg32(seed_, stream_id_);
            gen_.skip(static_cast<uint64_t>(path) * static_cast<uint64_t>(dim_));
        }

        std::size_t sim_dim() const override { return dim_; }

      private:
        uint64_t seed_, stream_id_;
        Pcg32 gen_;
        std::size_t dim_ = 0;
    };

    /**
     * @brief Sobol-backed RNG: skip_to(path) computes point `path` directly
     *        via SobolSequence::skip_to()'s Gray-code formula. No *sim_dim()
     *        scaling, unlike Pcg32RNG: a Sobol point already carries all
     *        sim_dim() coordinates a path needs in one draw, so path index
     *        and point index are the same number.
     *
     * SobolSequence needs its dimension at construction, but RNG::init()
     * only learns sim_dim() after the RNG itself is constructed -- so the
     * underlying sequence is built lazily, on the first init() call.
     */
    class SobolRNG final : public RNG
    {
      public:
        explicit SobolRNG(uint64_t scramble_seed)
            : scramble_seed_(scramble_seed) {}

        void init(std::size_t sim_dim) override
        {
            dim_ = sim_dim;
            seq_.emplace(static_cast<int>(sim_dim), scramble_seed_);
        }

        void next_u(std::span<double> out) override { seq_->next_uniform(out); }
        void next_g(std::span<double> out) override { seq_->next_gaussian(out); }

        std::unique_ptr<RNG> clone() const override
        {
            auto copy = std::make_unique<SobolRNG>(scramble_seed_);
            if (seq_)
                copy->init(dim_);
            return copy;
        }

        // Unlike Pcg32RNG, no *dim_ scaling here: one Sobol *point* already
        // carries all dim_ coordinates a path needs (that is exactly what
        // "dimension" means for a Sobol sequence), so path index == point
        // index directly.
        void skip_to(std::size_t path) override
        {
            seq_->skip_to(static_cast<uint32_t>(path));
        }

        std::size_t sim_dim() const override { return dim_; }

      private:
        uint64_t scramble_seed_;
        std::optional<SobolSequence> seq_;
        std::size_t dim_ = 0;
    };

} // namespace quantModeling

#endif // QM_UTILS_RNG_INTERFACE_HPP
