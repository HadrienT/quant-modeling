#include "quantModeling/engines/xva/hull_white_future_value.hpp"

#include "quantModeling/engines/analytic/hull_white_swaption.hpp"
#include "quantModeling/engines/analytic/swap.hpp"
#include "quantModeling/engines/xva/exposure_program.hpp"

#include <algorithm>
#include <cmath>
#include <string>

namespace quantModeling
{
    namespace
    {
        constexpr Real kTimeEps = 1e-10;
        /// A coupon fixed today has no fixing date on the grid.
        constexpr std::size_t kFixedToday = static_cast<std::size_t>(-1);

        std::size_t grid_index(const std::vector<Time> &grid, Time t)
        {
            const auto it = std::lower_bound(grid.begin(), grid.end(), t - kTimeEps);
            if (it == grid.end() || std::abs(*it - t) > kTimeEps)
                throw InvalidInput("exposure grid is missing the event date t=" + std::to_string(t));
            return static_cast<std::size_t>(it - grid.begin());
        }

        class HullWhiteSwapValue final : public FutureValue
        {
          public:
            HullWhiteSwapValue(InterestRateSwap swap, const HullWhiteCurveModel &model)
                : swap_(std::move(swap)), model_(model)
            {
                if (swap_.fixed_leg.empty() || swap_.floating_leg.empty())
                    throw InvalidInput("swap future value: both legs need at least one coupon");
                for (const CouponPeriod &c : swap_.floating_leg)
                {
                    if (std::abs(c.payment - c.end) > kTimeEps)
                        throw InvalidInput(
                            "swap future value: floating coupons must pay on their period end");
                    if (c.start < -kTimeEps)
                        throw InvalidInput(
                            "swap future value: a coupon that started in the past needs its "
                            "historical fixing, which is not supported");
                }
            }

            std::vector<Time> event_times() const override
            {
                std::vector<Time> times;
                for (const CouponPeriod &c : swap_.fixed_leg)
                    times.push_back(c.payment);
                for (const CouponPeriod &c : swap_.floating_leg)
                {
                    times.push_back(c.start);
                    times.push_back(c.end);
                }
                return times;
            }

            Time maturity() const override { return swap_.maturity(); }

            Real value_today() const override
            {
                return value_swap(swap_, MultiCurve{model_.discount(), model_.projection()}).npv;
            }

            void bind(const std::vector<Time> &grid) override
            {
                const Real side = swap_.payer ? 1.0 : -1.0;
                const Real N = swap_.notional;
                const DiscountCurve &d = model_.discount();
                const DiscountCurve &p = model_.projection();
                dates_.assign(grid.size(), Date{});
                floating_.clear();

                // The model's deterministic basis: β = (1 + τ F(0)) P_d(0, e) / P_d(0, s).
                const auto basis = [&](const CouponPeriod &c)
                {
                    return (p.discount(c.start) / p.discount(c.end)) /
                           (d.discount(c.start) / d.discount(c.end));
                };

                for (const CouponPeriod &c : swap_.fixed_leg)
                    dates_[grid_index(grid, c.payment)].fixed_cashflow -=
                        side * N * swap_.fixed_rate * c.accrual;

                for (const CouponPeriod &c : swap_.floating_leg)
                {
                    // Amount paid at the end, known from x at the start s:
                    //   N (β / P(s, e | x_s) - 1 + spread τ)
                    //   = N (B exp(g x_s) + constant),  g = G(s, e).
                    FloatingCoupon coupon;
                    coupon.fixing = c.start <= kTimeEps ? kFixedToday : grid_index(grid, c.start);
                    coupon.g = model_.G(c.start, c.end);
                    coupon.B = basis(c) * d.discount(c.start) / d.discount(c.end) *
                               std::exp(0.5 * coupon.g * coupon.g * model_.y(c.start));
                    coupon.constant = -1.0 + swap_.spread * c.accrual;
                    coupon.scale = side * N;
                    coupon.beta = basis(c);
                    coupon.start = c.start;
                    coupon.end = c.end;
                    dates_[grid_index(grid, c.end)].floating_paid.push_back(floating_.size());
                    floating_.push_back(coupon);
                }

                for (std::size_t i = 0; i < grid.size(); ++i)
                {
                    const Time t = grid[i];
                    Date &date = dates_[i];
                    for (const CouponPeriod &c : swap_.fixed_leg)
                        if (c.payment > t + kTimeEps)
                        {
                            date.bonds.push_back(
                                bond(t, c.payment, -side * N * swap_.fixed_rate * c.accrual));
                            date.annuity.push_back(bond(t, c.payment, N * c.accrual));
                        }
                    for (std::size_t k = 0; k < swap_.floating_leg.size(); ++k)
                    {
                        const CouponPeriod &c = swap_.floating_leg[k];
                        if (c.start >= t - kTimeEps)
                        {
                            // Not fixed yet: β P(t, s) - (1 - spread τ) P(t, e).
                            date.bonds.push_back(bond(t, c.start, side * N * basis(c)));
                            date.bonds.push_back(
                                bond(t, c.end, -side * N * (1.0 - swap_.spread * c.accrual)));
                        }
                        else if (c.end > t + kTimeEps)
                        {
                            date.in_progress.push_back({k, bond(t, c.end, 1.0)});
                        }
                    }
                }
            }

            Real value(std::size_t i, const Real *state) const override
            {
                const Date &date = dates_[i];
                const Real x = state[i];
                Real v = 0.0;
                for (const Bond &b : date.bonds)
                    v += b.amount * std::exp(-b.G * x);
                for (const InProgress &c : date.in_progress)
                    v += coupon_amount(floating_[c.coupon], state) * c.discount.amount *
                         std::exp(-c.discount.G * x);
                return v;
            }

            Real cashflow(std::size_t i, const Real *state) const override
            {
                const Date &date = dates_[i];
                Real flow = date.fixed_cashflow;
                for (const std::size_t k : date.floating_paid)
                    flow += coupon_amount(floating_[k], state);
                return flow;
            }

            /// Notional × annuity of the fixed coupons after grid[i]: what one
            /// unit of fixed rate is worth.
            Real annuity(std::size_t i, const Real *state) const
            {
                Real a = 0.0;
                for (const Bond &b : dates_[i].annuity)
                    a += b.amount * std::exp(-b.G * state[i]);
                return a;
            }

            Real fixed_rate() const { return swap_.fixed_rate; }
            bool payer() const { return swap_.payer; }
            Real annuity_today() const
            {
                return swap_.notional *
                       value_swap(swap_, MultiCurve{model_.discount(), model_.projection()}).annuity;
            }

            /// The terms of value() and cashflow(), date by date, in their
            /// order.
            bool compile(xva::ExposureProgram &program) const override
            {
                if (dates_.empty())
                    throw InvalidInput("swap future value: compile() needs the grid; call bind() first");
                if (program.trades.empty())
                    program.dates = dates_.size();
                else if (program.dates != dates_.size())
                    throw InvalidInput("exposure program: the trades are not bound to one grid");
                xva::ProgramTrade trade;
                trade.date_begin = static_cast<int>(program.date_records.size());
                const int first_coupon = static_cast<int>(program.coupons.size());
                for (const FloatingCoupon &c : floating_)
                {
                    program.coupons.push_back({c.B, c.g, c.constant, c.scale,
                                               c.fixing == kFixedToday ? -1 : static_cast<int>(c.fixing)});
                    program.coupon_sources.push_back({c.beta, c.start, c.end});
                }
                for (const Date &date : dates_)
                {
                    xva::ProgramDate record;
                    record.fixed_cashflow = date.fixed_cashflow;
                    record.exp_begin = static_cast<int>(program.exps.size());
                    for (const Bond &b : date.bonds)
                    {
                        program.exps.push_back({b.amount, b.G});
                        program.exp_sources.push_back({b.raw, b.from, b.maturity});
                    }
                    record.exp_end = static_cast<int>(program.exps.size());
                    record.accrued_begin = static_cast<int>(program.accrued.size());
                    for (const InProgress &c : date.in_progress)
                    {
                        program.accrued.push_back({c.discount.amount, c.discount.G,
                                                   first_coupon + static_cast<int>(c.coupon)});
                        program.accrued_sources.push_back(
                            {c.discount.raw, c.discount.from, c.discount.maturity});
                    }
                    record.accrued_end = static_cast<int>(program.accrued.size());
                    record.paid_begin = static_cast<int>(program.paid.size());
                    for (const std::size_t k : date.floating_paid)
                        program.paid.push_back(first_coupon + static_cast<int>(k));
                    record.paid_end = static_cast<int>(program.paid.size());
                    program.date_records.push_back(record);
                }
                program.trades.push_back(trade);

                // Today: every cash flow still to come, as bonds seen from
                // today (the terms of sensitivities_today()).
                xva::TodaySource today;
                today.term_begin = static_cast<int>(program.today_terms.size());
                const Real side = swap_.payer ? 1.0 : -1.0;
                const Real N = swap_.notional;
                for (const CouponPeriod &c : swap_.fixed_leg)
                    program.today_terms.push_back({-side * N * swap_.fixed_rate * c.accrual, c.payment});
                for (const FloatingCoupon &c : floating_)
                {
                    const Real accrual_spread = c.constant + 1.0; // spread × accrual
                    program.today_terms.push_back({side * N * c.beta, c.start});
                    program.today_terms.push_back({-side * N * (1.0 - accrual_spread), c.end});
                }
                today.term_end = static_cast<int>(program.today_terms.size());
                program.today.push_back(today);
                return true;
            }

            /// Every term of value() is a cash flow times a zero-coupon bond.
            bool sensitivities(std::size_t i, const Real *state, std::vector<BondExposure> &bonds,
                               std::vector<VolExposure> &) const override
            {
                const Date &date = dates_[i];
                const Real x = state[i];
                for (const Bond &b : date.bonds)
                    bonds.push_back({b.maturity, b.amount * std::exp(-b.G * x)});
                for (const InProgress &c : date.in_progress)
                    bonds.push_back({c.discount.maturity, coupon_amount(floating_[c.coupon], state) *
                                                              c.discount.amount *
                                                              std::exp(-c.discount.G * x)});
                return true;
            }

            bool sensitivities_today(std::vector<BondExposure> &bonds,
                                     std::vector<VolExposure> &) const override
            {
                const Real side = swap_.payer ? 1.0 : -1.0;
                const Real N = swap_.notional;
                const DiscountCurve &d = model_.discount();
                const DiscountCurve &p = model_.projection();
                for (const CouponPeriod &c : swap_.fixed_leg)
                    bonds.push_back(
                        {c.payment, -side * N * swap_.fixed_rate * c.accrual * d.discount(c.payment)});
                for (const CouponPeriod &c : swap_.floating_leg)
                {
                    const Real beta = (p.discount(c.start) / p.discount(c.end)) /
                                      (d.discount(c.start) / d.discount(c.end));
                    bonds.push_back({c.start, side * N * beta * d.discount(c.start)});
                    bonds.push_back(
                        {c.end, -side * N * (1.0 - swap_.spread * c.accrual) * d.discount(c.end)});
                }
                return true;
            }

            /// The value depends on today's state and on the floating coupons
            /// already fixed and not yet paid.
            std::size_t regressors(std::size_t i, const Real *state, Real *out) const override
            {
                std::size_t count = 0;
                out[count++] = state[i];
                for (const InProgress &c : dates_[i].in_progress)
                    if (count < kMaxRegressors)
                        out[count++] = coupon_amount(floating_[c.coupon], state);
                return count;
            }

          private:
            /// coefficient × P(t, T | x) = amount × exp(-G x).
            struct Bond
            {
                Real amount;
                Real G;
                Time maturity;
                /// The cash flow before discounting, and the date it is seen
                /// from: what the amount was computed from.
                Real raw = 0.0;
                Time from = 0.0;
            };
            struct FloatingCoupon
            {
                std::size_t fixing = kFixedToday;
                Real B = 0.0;
                Real g = 0.0;
                Real constant = 0.0;
                Real scale = 0.0;
                /// What B was computed from.
                Real beta = 1.0;
                Time start = 0.0, end = 0.0;
            };
            struct InProgress
            {
                std::size_t coupon;
                Bond discount;
            };
            struct Date
            {
                std::vector<Bond> bonds;
                /// The fixed coupons still to pay, per unit of fixed rate.
                std::vector<Bond> annuity;
                std::vector<InProgress> in_progress;
                Real fixed_cashflow = 0.0;
                std::vector<std::size_t> floating_paid;
            };

            Bond bond(Time t, Time T, Real coefficient) const
            {
                const Real G = model_.G(t, T);
                return {coefficient * model_.discount().discount(T) / model_.discount().discount(t) *
                            std::exp(-0.5 * G * G * model_.y(t)),
                        G, T, coefficient, t};
            }

            static Real coupon_amount(const FloatingCoupon &c, const Real *state)
            {
                const Real x = c.fixing == kFixedToday ? 0.0 : state[c.fixing];
                return c.scale * (c.B * std::exp(c.g * x) + c.constant);
            }

            InterestRateSwap swap_;
            const HullWhiteCurveModel &model_;
            std::vector<FloatingCoupon> floating_;
            std::vector<Date> dates_;
        };

        class HullWhiteSwaptionValue final : public FutureValue
        {
          public:
            HullWhiteSwaptionValue(const Swaption &swaption, const HullWhiteCurveModel &model)
                : region_(hull_white_exercise_region(swaption, model)),
                  underlying_(swaption.swap, model),
                  expiry_(swaption.expiry),
                  model_(model)
            {
                if (!(expiry_ > kTimeEps))
                    throw InvalidInput("swaption future value: the expiry must be in the future");
            }

            std::vector<Time> event_times() const override
            {
                std::vector<Time> times = underlying_.event_times();
                times.push_back(expiry_);
                return times;
            }

            Time maturity() const override { return underlying_.maturity(); }

            Real value_today() const override
            {
                return hull_white_european_swaption(region_, model_, 0.0, 0.0);
            }

            void bind(const std::vector<Time> &grid) override
            {
                underlying_.bind(grid);
                expiry_index_ = grid_index(grid, expiry_);
                times_.assign(grid.begin(),
                              grid.begin() + static_cast<std::ptrdiff_t>(expiry_index_));
            }

            Real value(std::size_t i, const Real *state) const override
            {
                if (i < expiry_index_)
                    return hull_white_european_swaption(region_, model_, times_[i], state[i]);
                const Real at_expiry = underlying_.value(expiry_index_, state);
                if (!(at_expiry > 0.0))
                    return 0.0;
                return i == expiry_index_ ? at_expiry : underlying_.value(i, state);
            }

            Real cashflow(std::size_t i, const Real *state) const override
            {
                if (i <= expiry_index_ || !(underlying_.value(expiry_index_, state) > 0.0))
                    return 0.0;
                return underlying_.cashflow(i, state);
            }

            /// The underlying's terms, gated at the expiry, and before it the
            /// integral of hull_white_european_swaption() with everything
            /// that does not depend on the state computed here.
            bool compile(xva::ExposureProgram &program) const override
            {
                underlying_.compile(program);
                xva::ProgramTrade &trade = program.trades.back();
                trade.expiry = static_cast<int>(expiry_index_);
                trade.option_date_begin = static_cast<int>(program.option_dates.size());
                const Real y_expiry = model_.y(expiry_);
                const DiscountCurve &d = model_.discount();
                for (const Time t : times_)
                {
                    const auto tr = model_.transition(t, expiry_, expiry_);
                    const Real G = model_.G(t, expiry_);
                    program.option_dates.push_back(
                        {tr.decay, tr.drift, tr.variance, std::sqrt(tr.variance),
                         0.5 * (tr.variance - y_expiry),
                         d.discount(expiry_) / d.discount(t) * std::exp(-0.5 * G * G * model_.y(t)), G});
                    program.option_date_sources.push_back({t, expiry_});
                }
                trade.bond_begin = static_cast<int>(program.option_bonds.size());
                for (std::size_t k = 0; k < region_.bonds.size(); ++k)
                {
                    program.option_bonds.push_back(
                        {region_.bonds[k].amount * region_.forward_bond[k], region_.G[k]});
                    program.option_bond_sources.push_back(
                        {region_.bonds[k].amount, expiry_, region_.bonds[k].time});
                }
                trade.bond_end = static_cast<int>(program.option_bonds.size());
                trade.interval_begin = static_cast<int>(program.intervals.size());
                for (const auto &[lower, upper] : region_.intervals)
                    program.intervals.push_back({lower, upper});
                trade.interval_end = static_cast<int>(program.intervals.size());
                // Today the trade is the option, not the swap the underlying
                // has just recorded.
                program.today.back().expiry = expiry_;
                return true;
            }

            /// Before the expiry: the swaption bond by bond, and its vega to
            /// the normal volatility its own price implies. After: the swap's.
            bool sensitivities(std::size_t i, const Real *state, std::vector<BondExposure> &bonds,
                               std::vector<VolExposure> &vols) const override
            {
                if (i >= expiry_index_)
                {
                    if (underlying_.value(expiry_index_, state) > 0.0)
                        underlying_.sensitivities(i, state, bonds, vols);
                    return true;
                }
                std::vector<Real> terms;
                hull_white_european_swaption_terms(region_, model_, times_[i], state[i], terms);
                Real price = 0.0;
                for (std::size_t k = 0; k < terms.size(); ++k)
                {
                    bonds.push_back({region_.bonds[k].time, terms[k]});
                    price += terms[k];
                }
                add_vega(price, underlying_.annuity(i, state), underlying_.value(i, state),
                         expiry_ - times_[i], vols);
                return true;
            }

            bool sensitivities_today(std::vector<BondExposure> &bonds,
                                     std::vector<VolExposure> &vols) const override
            {
                std::vector<Real> terms;
                hull_white_european_swaption_terms(region_, model_, 0.0, 0.0, terms);
                Real price = 0.0;
                for (std::size_t k = 0; k < terms.size(); ++k)
                {
                    bonds.push_back({region_.bonds[k].time, terms[k]});
                    price += terms[k];
                }
                add_vega(price, underlying_.annuity_today(), underlying_.value_today(), expiry_, vols);
                return true;
            }

            /// Before the expiry an option; after it the swap, or nothing.
            std::size_t regime(std::size_t i, const Real *state) const override
            {
                if (i < expiry_index_)
                    return 0;
                return underlying_.value(expiry_index_, state) > 0.0 ? 1 : 2;
            }

            std::size_t regressors(std::size_t i, const Real *state, Real *out) const override
            {
                if (i >= expiry_index_)
                    return underlying_.regressors(i, state, out);
                out[0] = state[i];
                return 1;
            }

          private:
            /**
             * σ ∂V/∂σ at the normal (Bachelier) volatility the price implies:
             * SIMM's vega is a sensitivity to an implied at-the-money
             * volatility, which a short-rate model does not have as a
             * parameter. Nothing when the price is at its intrinsic value:
             * no volatility is implied, and the vega is zero.
             */
            void add_vega(Real price, Real annuity, Real swap_value, Time to_expiry,
                          std::vector<VolExposure> &vols) const
            {
                if (!(annuity > 0.0) || !(to_expiry > kTimeEps) || !(price > 0.0))
                    return;
                const Real strike = underlying_.fixed_rate();
                // A payer swap is worth annuity × (forward - strike).
                const Real forward =
                    strike + (underlying_.payer() ? 1.0 : -1.0) * swap_value / annuity;
                const Real vol = bachelier_implied_vol(underlying_.payer(), price, forward, strike,
                                                       to_expiry, annuity);
                if (!std::isfinite(vol) || !(vol > 0.0))
                    return;
                vols.push_back(
                    {expiry_, vol * bachelier_vega(forward, strike, to_expiry, vol, annuity)});
            }

            HullWhiteExerciseRegion region_;
            HullWhiteSwapValue underlying_;
            Time expiry_;
            const HullWhiteCurveModel &model_;
            std::size_t expiry_index_ = 0;
            /// The grid dates before the expiry.
            std::vector<Time> times_;
        };

        /// One visit per instrument the Hull-White model reprices in closed
        /// form; everything else is unsupported.
        class Factory final : public IInstrumentVisitor
        {
          public:
            explicit Factory(const HullWhiteCurveModel &model)
                : model_(model) {}

            void visit(const InterestRateSwap &swap) override
            {
                result = make_future_value(swap, model_);
            }
            void visit(const Swaption &swaption) override
            {
                result = make_future_value(swaption, model_);
            }
            void visit(const BermudanSwaption &bermudan) override
            {
                result = make_future_value(bermudan, model_);
            }

            void visit(const VanillaOption &) override { unsupported("vanilla option"); }
            void visit(const AsianOption &) override { unsupported("Asian option"); }
            void visit(const BarrierOption &) override { unsupported("barrier option"); }
            void visit(const DigitalOption &) override { unsupported("digital option"); }
            void visit(const EquityFuture &) override { unsupported("equity future"); }
            void visit(const ZeroCouponBond &) override { unsupported("zero-coupon bond"); }
            void visit(const FixedRateBond &) override { unsupported("fixed-rate bond"); }

            std::unique_ptr<FutureValue> result;

          private:
            [[noreturn]] static void unsupported(const char *name)
            {
                throw UnsupportedInstrument(std::string("No future value under Hull-White for: ") +
                                            name);
            }

            const HullWhiteCurveModel &model_;
        };
    } // namespace

    std::unique_ptr<FutureValue> make_future_value(const InterestRateSwap &swap,
                                                   const HullWhiteCurveModel &model)
    {
        return std::make_unique<HullWhiteSwapValue>(swap, model);
    }

    std::unique_ptr<FutureValue> make_future_value(const Swaption &swaption,
                                                   const HullWhiteCurveModel &model)
    {
        return std::make_unique<HullWhiteSwaptionValue>(swaption, model);
    }

    std::unique_ptr<FutureValue> make_hull_white_future_value(const Instrument &instrument,
                                                              const HullWhiteCurveModel &model)
    {
        Factory factory(model);
        instrument.accept(factory);
        return std::move(factory.result);
    }

} // namespace quantModeling
