#ifndef QM_SCRIPTING_DOMAIN_HPP
#define QM_SCRIPTING_DOMAIN_HPP

#include <vector>

namespace quantModeling::scripting
{

    /**
     * @brief The set of values an expression can take — a union of closed
     *        intervals and isolated points (Andreasen & Savine's Domain,
     *        blueprint/wp/16-scripting.md §4.2).
     *
     * The DomainProcessor propagates one of these through the AST. From the
     * domain of `lhs - rhs` for a condition `lhs ⋈ rhs` it decides:
     *  - the condition is a **discrete** test (finite isolated domain, e.g. a
     *    `ki = 1` flag) → never smoothed;
     *  - the condition is **always true / always false** (domain wholly on one
     *    side of zero) → folded away;
     *  - otherwise, a smoothing half-width from `default_eps`.
     *
     * Bounds use ±infinity() directly, so ordinary IEEE arithmetic carries the
     * infinities; the only guarded case is 0·∞ (taken as 0). Unhandled shapes
     * fall back to the whole real line — conservative: it smooths rather than
     * risks smoothing a genuinely discrete test wrongly, or the reverse.
     */
    class Domain
    {
      public:
        struct Interval
        {
            double lo = 0.0;
            double hi = 0.0;
            bool is_singleton() const;
            bool contains(double v) const;
        };

        Domain() = default; ///< empty

        static Domain singleton(double v);
        static Domain closed(double lo, double hi);
        static Domain greater_than(double lo); ///< (lo, +inf)
        static Domain at_least(double lo);     ///< [lo, +inf)
        static Domain real_line();

        bool empty() const { return parts_.empty(); }
        const std::vector<Interval> &parts() const { return parts_; }

        bool is_discrete() const; ///< non-empty, every part a singleton
        bool contains(double v) const;
        bool all_positive() const;     ///< non-empty, every element > 0
        bool all_negative() const;     ///< non-empty, every element < 0
        bool all_non_negative() const; ///< non-empty, every element >= 0
        bool all_non_positive() const; ///< non-empty, every element <= 0

        /// Smallest |x| over the domain (0 if it contains or straddles 0).
        double distance_to_zero() const;

        Domain operator-() const;
        Domain operator+(const Domain &other) const;
        Domain operator-(const Domain &other) const;
        Domain operator*(const Domain &other) const;
        Domain operator/(const Domain &other) const;

        friend Domain domain_union(const Domain &a, const Domain &b);
        friend Domain domain_min(const Domain &a, const Domain &b);
        friend Domain domain_max(const Domain &a, const Domain &b);
        friend Domain domain_abs(const Domain &a);
        friend Domain domain_exp(const Domain &a);
        friend Domain domain_log(const Domain &a);
        friend Domain domain_sqrt(const Domain &a);

      private:
        void add_interval(Interval iv); ///< insert then re-normalise
        void normalise();               ///< sort by lo, merge touching/overlapping

        std::vector<Interval> parts_;
    };

} // namespace quantModeling::scripting

#endif // QM_SCRIPTING_DOMAIN_HPP
