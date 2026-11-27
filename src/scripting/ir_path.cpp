#include "quantModeling/scripting/ir_path.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

#include "quantModeling/engines/mc/script_path.hpp"

namespace quantModeling::scripting::ir
{

    std::string path_program_unsupported(DeviceModel::Kind kind, int n_assets)
    {
        switch (kind)
        {
            case DeviceModel::Kind::BlackScholes:
                return "";
            case DeviceModel::Kind::LocalVol:
            case DeviceModel::Kind::Heston:
            case DeviceModel::Kind::SLV:
                return n_assets == 1 ? "" : "local vol, Heston and SLV have one underlying";
        }
        return "unknown model";
    }

    std::string path_program_unsupported(const Script &script, DeviceModel::Kind kind, int n_assets)
    {
        std::string why = path_program_unsupported(kind, n_assets);
        if (!why.empty())
            return why;
        if (script.has_knock_out && n_assets != 1)
            return "knockOutAbove() / knockOutBelow() cover one underlying";
        if (!script.has_bridge)
            return why;
        if (n_assets != 1)
            return "survivalAbove() / survivalBelow() cover one underlying";
        if (kind != DeviceModel::Kind::BlackScholes && kind != DeviceModel::Kind::LocalVol)
            return "survivalAbove() / survivalBelow() run under Black-Scholes and local vol";
        return "";
    }

    namespace
    {
        /// The survival slots of a block: their positions among its
        /// quantities, in order.
        std::vector<std::size_t> survival_slots(const Block &b)
        {
            std::vector<std::size_t> out;
            for (std::size_t k = 0; k < b.quantities.size(); ++k)
                if (b.quantities[k].kind == StepQuantity::Survival)
                    out.push_back(k);
            return out;
        }
    } // namespace

    PathLayout path_layout(DeviceModel::Kind kind, int n_assets, const Script &script, bool risk, bool last_step,
                           bool half, bool milstein, bool curve)
    {
        PathLayout l;
        l.kind = kind;
        l.n_assets = n_assets;
        l.n_vars = script.n_vars;
        l.risk = risk;
        l.last_step = script.has_last_step || last_step;
        const int n = n_assets;
        switch (kind)
        {
            case DeviceModel::Kind::BlackScholes:
                l.n_model = n;
                l.factors = n;
                if (n == 1)
                {
                    l.th_spot = 0, l.th_rate = 1, l.th_div = 2, l.th_vol = 3;
                    l.n_theta = l.n_risks = 4;
                }
                else
                {
                    l.th_rate = 0, l.th_spot = 1, l.th_div = 1 + n, l.th_vol = 1 + 2 * n;
                    l.th_chol = 1 + 3 * n;
                    l.n_risks = 1 + 3 * n;
                    l.n_theta = l.n_risks + n * n;
                }
                l.step_cols = risk ? 2 : 2 * n;
                break;
            case DeviceModel::Kind::LocalVol:
                l.n_model = 1;
                l.factors = 1;
                l.th_spot = 0, l.th_rate = 1, l.th_div = 2;
                l.n_theta = l.n_risks = 3;
                l.th_grid = 3;
                l.step_cols = 3;
                break;
            case DeviceModel::Kind::Heston:
            case DeviceModel::Kind::SLV:
                l.n_model = 2;
                l.factors = 2;
                l.th_spot = 0, l.th_rate = 1, l.th_div = 2;
                l.th_heston = 3;
                l.n_theta = l.n_risks = 8;
                l.step_cols = kind == DeviceModel::Kind::SLV ? 3 : 2;
                if (kind == DeviceModel::Kind::SLV)
                    l.th_grid = 8; // the leverage
                break;
        }
        int max_df = -1, max_fwd = -1;
        for (const Block &b : script.blocks)
        {
            max_df = std::max(max_df, static_cast<int>(b.max_df));
            max_fwd = std::max(max_fwd, static_cast<int>(b.max_fwd));
            l.n_surv = std::max(l.n_surv, static_cast<int>(survival_slots(b).size()));
        }
        l.fwd_col0 = 2 + max_df;
        l.event_cols = 2 + max_df + (max_fwd + 1);
        l.surv_col0 = l.step_cols;
        l.step_cols += 2 * l.n_surv;
        if (script.has_knock_out)
        {
            l.knock_out = true;
            l.ko_col0 = l.step_cols;
            l.step_cols += 5;
        }
        if (half)
        {
            if (kind != DeviceModel::Kind::LocalVol)
                throw InvalidInput("script IR: the coarse path's conditioning on the fine one runs under local vol");
            l.half = true;
            l.half_col = l.step_cols++;
        }
        if (milstein)
        {
            if (kind != DeviceModel::Kind::LocalVol)
                throw InvalidInput("script IR: Milstein before the dates is local vol's");
            if (risk)
                throw InvalidInput("script IR: Milstein's risks run on the CPU's tape (the IR does not differentiate "
                                   "the vol's slope yet)");
            l.milstein = true;
            l.mil_col = l.step_cols++;
        }
        if (curve)
        {
            l.curve = true;
            if (kind != DeviceModel::Kind::BlackScholes || risk)
                l.carry_col = l.step_cols++;
            if (risk)
            {
                l.factor_col0 = l.event_cols;
                l.event_cols *= 2;
            }
        }
        return l;
    }

    namespace
    {
        /// lastStepCall / Put / Digital from the forward F and log-variance
        /// v the step left (models/equity/last_step.hpp, Black's formula):
        /// sd = sqrt(max(v, 1e-200)) so that an event with no step before it
        /// (v = 0) gets the payoff of F as the limit, with no branch and no
        /// infinity for the derivatives to multiply by zero.
        std::int32_t last_step_expectation(Builder &b, const PathLayout &l, const StepMonitor &q)
        {
            if (!l.last_step)
                throw InvalidInput("script IR: a last-step expectation in a layout without the step's forward");
            const int a = static_cast<int>(q.asset);
            if (a >= l.n_assets)
                throw InvalidInput("script IR: a last-step expectation on asset " + std::to_string(a) +
                                   ", but the model simulates " + std::to_string(l.n_assets));
            const std::int32_t F = b.input(Op::State, l.last0() + 2 * a);
            const std::int32_t v = b.input(Op::State, l.last0() + 2 * a + 1);
            const std::int32_t K = b.constant(q.level);
            const std::int32_t sd = b.value(Op::Sqrt, b.value(Op::Max, v, b.constant(1e-200)));
            const std::int32_t d1 = b.add(b.div(b.value(Op::Log, b.div(F, K)), sd), b.mul(b.constant(0.5), sd));
            const std::int32_t d2 = b.sub(d1, sd);
            const auto N = [&](std::int32_t x)
            { return b.value(Op::Ncdf, x); };
            switch (q.kind)
            {
                case StepQuantity::LastStepCall:
                    return b.sub(b.mul(F, N(d1)), b.mul(K, N(d2)));
                case StepQuantity::LastStepPut:
                    return b.sub(b.mul(K, N(b.neg(d2))), b.mul(F, N(b.neg(d1))));
                default:
                    return N(d2);
            }
        }
    } // namespace

    Block bind_event(const Block &event, const PathLayout &l)
    {
        Rebuilder r(event);
        Builder &b = r.b;
        const auto n = static_cast<std::int32_t>(event.code.size());
        for (std::int32_t k = 0; k < n; ++k)
        {
            const Inst &in = event.code[static_cast<std::size_t>(k)];
            std::int32_t id = -1;
            switch (in.op)
            {
                case Op::VarIn:
                    id = b.input(Op::State, l.var0() + in.a);
                    break;
                case Op::PayIn:
                    id = b.input(Op::State, l.pay());
                    break;
                case Op::Spot:
                    id = b.input(Op::State, in.a);
                    break;
                case Op::Num: // exp(r t), as script_path computes it
                    id = l.risk ? b.value(Op::Exp, b.mul(b.input(Op::Theta, l.th_rate), b.input(Op::Data, 0)))
                                : b.input(Op::Data, 0);
                    if (l.risk && l.factor_col0 >= 0) // 1/DF(t) e^{r t}, ModelCurve::write_rates
                        id = b.mul(b.input(Op::Data, l.factor_col0), id);
                    break;
                case Op::Df: // exp(-r (T - t))
                    id = l.risk ? b.value(Op::Exp,
                                          b.mul(b.neg(b.input(Op::Theta, l.th_rate)), b.input(Op::Data, 1 + in.a)))
                                : b.input(Op::Data, 1 + in.a);
                    if (l.risk && l.factor_col0 >= 0) // DF(T)/DF(t) e^{-r (T - t)}
                        id = b.mul(b.input(Op::Data, l.factor_col0 + 1 + in.a), id);
                    break;
                case Op::Fwd: // spot(0) exp((r - q) (T - t)), as the CPU models fill it
                {
                    const std::int32_t S = b.input(Op::State, 0);
                    const std::int32_t col = b.input(Op::Data, l.fwd_col0 + in.a);
                    // on a curve, S F(T)/F(t) e^{(r - q)(T - t)} (ModelCurve::write_rates)
                    const std::int32_t S_g =
                        l.risk && l.factor_col0 >= 0 ? b.mul(S, b.input(Op::Data, l.factor_col0 + l.fwd_col0 + in.a))
                                                     : S;
                    id = l.risk ? b.mul(S_g, b.value(Op::Exp, b.mul(b.sub(b.input(Op::Theta, l.th_rate),
                                                                          b.input(Op::Theta, l.th_div)),
                                                                    col)))
                                : b.mul(S, col);
                    break;
                }
                case Op::Surv:
                {
                    const auto slot = static_cast<std::size_t>(in.a);
                    if (slot >= event.quantities.size())
                        throw InvalidInput("script IR: a monitored quantity out of the date's slots");
                    const StepMonitor &q = event.quantities[slot];
                    if (q.kind == StepQuantity::LastStepForward || q.kind == StepQuantity::LastStepVariance)
                    {
                        // A strike the script computes (issue #363): the
                        // step's forward or variance itself, the date's
                        // block applies Black's formula.
                        if (!l.last_step)
                            throw InvalidInput("script IR: a last-step forward in a layout without the step's forward");
                        const int a = static_cast<int>(q.asset);
                        if (a >= l.n_assets)
                            throw InvalidInput("script IR: a last-step forward on asset " + std::to_string(a) +
                                               ", but the model simulates " + std::to_string(l.n_assets));
                        id = b.input(Op::State, l.last0() + 2 * a + (q.kind == StepQuantity::LastStepVariance ? 1 : 0));
                        break;
                    }
                    if (q.kind == StepQuantity::KnockOut)
                    {
                        // The probability the step into the date left: in
                        // the corridor's first slot, 1 in the others (they
                        // multiply the same weight), as write_knock_out.
                        if (!l.knock_out)
                            throw InvalidInput("script IR: a knock-out in a layout without its state");
                        std::size_t first = 0;
                        while (event.quantities[first].kind != StepQuantity::KnockOut)
                            ++first;
                        id = slot == first ? b.input(Op::State, l.ko()) : b.constant(1.0);
                        break;
                    }
                    if (q.kind != StepQuantity::Survival)
                    {
                        id = last_step_expectation(b, l, q);
                        break;
                    }
                    // The running product of the steps' bridge factors,
                    // times the spot of the date on the safe side: what the
                    // CPU model gives a date with no step before it (its
                    // bridge has no variance), and 1 -- the same bits --
                    // after a step, which already required it.
                    const std::vector<std::size_t> slots = survival_slots(event);
                    const auto j = static_cast<int>(std::find(slots.begin(), slots.end(), slot) - slots.begin());
                    const std::int32_t S = b.input(Op::State, static_cast<int>(q.asset));
                    const std::int32_t L = b.constant(q.level);
                    const std::int32_t unsafe = b.value(q.above ? Op::Le : Op::Ge, S, L);
                    id = b.select(unsafe, b.constant(0.0), b.input(Op::State, l.surv0() + j));
                    break;
                }
                default:
                    r.copy(k);
                    continue;
            }
            r.map[static_cast<std::size_t>(k)] = id;
        }
        // The next date's survivals start from 1.
        std::vector<std::int32_t> reset;
        for (int j = 0; j < l.n_surv; ++j)
            reset.push_back(b.constant(1.0));
        Block out = b.take();
        for (const Out &o : event.outs)
            out.outs.push_back({Op::State, o.cls == Op::VarIn ? l.var0() + o.index : l.pay(), kPrimal, r[o.value], false});
        for (int j = 0; j < l.n_surv; ++j)
            out.outs.push_back({Op::State, l.surv0() + j, kPrimal, reset[static_cast<std::size_t>(j)], false});
        out.max_spot = event.max_spot;
        out.max_df = event.max_df;
        remove_dead_code(out);
        return out;
    }

    namespace
    {
        Block init_block(const PathLayout &l)
        {
            Builder b;
            Block &out = b.block();
            for (int k = 0; k < l.n_assets; ++k)
                out.outs.push_back({Op::State, k, kPrimal, b.input(Op::Theta, l.th_spot + k), false});
            if (l.th_heston >= 0)
                out.outs.push_back({Op::State, l.n_assets, kPrimal, b.input(Op::Theta, l.th_heston), false});
            for (int i = 0; i < l.n_vars; ++i)
                out.outs.push_back({Op::State, l.var0() + i, kPrimal, b.input(Op::Data, i), false});
            out.outs.push_back({Op::State, l.pay(), kPrimal, b.constant(0.0), false});
            if (l.last_step) // before any step: F = S0, v = 0 (an event at t = 0 pays its payoff of S0)
                for (int k = 0; k < l.n_assets; ++k)
                {
                    out.outs.push_back({Op::State, l.last0() + 2 * k, kPrimal, b.input(Op::Theta, l.th_spot + k), false});
                    out.outs.push_back({Op::State, l.last0() + 2 * k + 1, kPrimal, b.constant(0.0), false});
                }
            for (int j = 0; j < l.n_surv; ++j)
                out.outs.push_back({Op::State, l.surv0() + j, kPrimal, b.constant(1.0), false});
            if (l.knock_out)
                out.outs.push_back({Op::State, l.ko(), kPrimal, b.constant(1.0), false});
            return b.take();
        }

        /// The models' steps, operation for operation as script_path and
        /// path_steps.hpp compute them.
        Block step_block(const PathLayout &l)
        {
            Builder b;
            const auto theta = [&](int i)
            { return b.input(Op::Theta, i); };
            const auto data = [&](int i)
            { return b.input(Op::Data, i); };
            const auto draw = [&](int i)
            { return b.input(Op::Draw, i); };
            const auto state = [&](int i)
            { return b.input(Op::State, i); };
            const auto exp = [&](std::int32_t x)
            { return b.value(Op::Exp, x); };
            const auto out = [&](int slot, std::int32_t v)
            { b.block().outs.push_back({Op::State, slot, kPrimal, v, false}); };
            // A step S' = S exp(m + s z), z ~ N(0, 1) given the path so far:
            // with last-step expectations, also F = S exp(m + s^2 / 2) and
            // v = s^2 (extra values only; the primal's operations are not
            // touched, so the prices keep their bits).
            // Conditioned on the fine path (PathLayout::half): a, the part
            // of the draw its first sub-step carries, and w, its share.
            const std::int32_t half_a = l.half ? draw(l.factors) : -1;
            const std::int32_t half_w = l.half ? data(l.half_col) : -1;
            const auto last = [&](int a, std::int32_t S, std::int32_t m, std::int32_t sz)
            {
                if (!l.last_step)
                    return;
                if (l.half) // given a: S' lognormal around S e^{m + sz a}, log-variance sz^2 (1 - w)
                {
                    const std::int32_t v = b.mul(b.mul(sz, sz), b.sub(b.constant(1.0), half_w));
                    out(l.last0() + 2 * a,
                        b.mul(S, exp(b.add(b.add(m, b.mul(sz, half_a)), b.mul(b.constant(0.5), v)))));
                    out(l.last0() + 2 * a + 1, v);
                    return;
                }
                const std::int32_t v = b.mul(sz, sz);
                out(l.last0() + 2 * a, b.mul(S, exp(b.add(m, b.mul(b.constant(0.5), v)))));
                out(l.last0() + 2 * a + 1, v);
            };
            // The bridge factor of every survival slot over this step from
            // a to b with log-variance var: mc::bridge_survival, operation
            // for operation (the level and the side are the step row's).
            const auto bridge = [&](int j, std::int32_t a, std::int32_t b2, std::int32_t var)
            {
                const std::int32_t L = data(l.surv_col0 + 2 * j);
                const std::int32_t above = data(l.surv_col0 + 2 * j + 1);
                const std::int32_t unsafe =
                    b.select(above, b.value(Op::Or, b.value(Op::Le, a, L), b.value(Op::Le, b2, L)),
                             b.value(Op::Or, b.value(Op::Ge, a, L), b.value(Op::Ge, b2, L)));
                const std::int32_t exponent =
                    b.div(b.mul(b.mul(b.constant(-2.0), b.value(Op::Log, b.div(L, a))), b.value(Op::Log, b.div(L, b2))),
                          var);
                return b.select(unsafe, b.constant(0.0),
                                b.select(b.value(Op::Lt, exponent, b.constant(0.0)),
                                         b.sub(b.constant(1.0), b.value(Op::Exp, exponent)), b.constant(0.0)));
            };
            const auto survive = [&](std::int32_t a, std::int32_t b2, std::int32_t var)
            {
                for (int j = 0; j < l.n_surv; ++j)
                    out(l.surv0() + j, b.mul(state(l.surv0() + j), bridge(j, a, b2, var)));
            };
            // Conditioned (PathLayout::half): through the midpoint the fine
            // path fixes, S e^{m w + sz a}, two bridges of log-variances
            // var w and var (1 - w).
            const auto survive_through_midpoint = [&](std::int32_t S, std::int32_t S_next, std::int32_t m,
                                                      std::int32_t sz)
            {
                const std::int32_t var = b.mul(sz, sz);
                const std::int32_t S_mid = b.mul(S, exp(b.add(b.mul(m, half_w), b.mul(sz, half_a))));
                for (int j = 0; j < l.n_surv; ++j)
                    out(l.surv0() + j,
                        b.mul(state(l.surv0() + j),
                              b.mul(bridge(j, S, S_mid, b.mul(var, half_w)),
                                    bridge(j, S_mid, S_next, b.mul(var, b.sub(b.constant(1.0), half_w))))));
            };
            // A knock-out (#369): the draw z of a step S' = e^{ls + mean + sd
            // z} conditioned on S' in the corridor the step row holds, as
            // mc::condition_on_survival computes it operation for operation,
            // and the survival probability. Inside a region on the step
            // row's flag (#380): a step that reaches no knock-out date skips
            // it -- the flag is the same for every path of a step, so the
            // GPU's threads do not diverge -- keeps z itself, its bits, and
            // leaves 1. Inside, every branch is computed on finite inputs and
            // the path's selected.
            const std::int32_t ko_one = l.knock_out ? b.constant(1.0) : -1;
            // Opens the region; `then` computes the values the region gives
            // a knock-out step, `otherwise` holds those of the other steps
            // (defined before it); returns their phis.
            const auto on_knock_out_step = [&](const auto &then, const std::vector<std::int32_t> &otherwise)
            {
                const std::int32_t head = b.raw(Op::If, data(l.ko_col0));
                b.open_scope();
                const std::vector<std::int32_t> values = then();
                b.close_scope();
                const std::int32_t mid = b.raw(Op::Else, head);
                const std::int32_t end = b.raw(Op::EndIf, head);
                b.at(mid).c = end;
                b.at(head).b = mid;
                b.at(head).c = end;
                std::vector<std::int32_t> out_values;
                for (std::size_t k = 0; k < values.size(); ++k)
                    out_values.push_back(values[k] == otherwise[k] ? values[k]
                                                                   : b.raw(Op::Phi, values[k], otherwise[k], head));
                return out_values;
            };
            // The conditioned draw and the probability, inside the region.
            const auto conditioned_draw = [&](std::int32_t z, std::int32_t ls, std::int32_t mean, std::int32_t sd)
            {
                const std::int32_t zero = b.constant(0.0), one = b.constant(1.0), half = b.constant(0.5);
                const std::int32_t has_lo = data(l.ko_col0 + 1), has_hi = data(l.ko_col0 + 3);
                const std::int32_t dl = b.div(b.sub(b.sub(data(l.ko_col0 + 2), ls), mean), sd);
                const std::int32_t du = b.div(b.sub(b.sub(data(l.ko_col0 + 4), ls), mean), sd);
                const auto N = [&](std::int32_t x)
                { return b.value(Op::Ncdf, x); };
                const std::int32_t A = b.select(has_lo, N(dl), zero), Ac = b.select(has_lo, N(b.neg(dl)), one);
                const std::int32_t B = b.select(has_hi, N(du), one), Bc = b.select(has_hi, N(b.neg(du)), zero);
                const std::int32_t p = b.select(b.value(Op::Lt, A, half), b.sub(B, A), b.sub(Ac, Bc));
                const std::int32_t alive = b.value(Op::Gt, p, b.constant(1e-14));
                const std::int32_t u = N(z), uc = N(b.neg(z));
                const std::int32_t lower = b.value(Op::Le, u, half);
                // the branch not taken reads 0.5: N^-1 stays finite there
                const std::int32_t cdf = b.select(b.value(Op::And, alive, lower), b.add(A, b.mul(u, p)), half);
                const std::int32_t sf =
                    b.select(b.value(Op::And, alive, b.value(Op::Not, lower)), b.add(Bc, b.mul(uc, p)), half);
                const std::int32_t zc =
                    b.select(lower, b.value(Op::InvNcdf, cdf), b.neg(b.value(Op::InvNcdf, sf)));
                return std::vector<std::int32_t>{b.select(alive, zc, z), b.select(alive, p, zero)};
            };
            // The step's draw at a knock-out date, conditioned; its weight
            // into the state. `ls` gives ln S, computed inside the region.
            const auto knock_out = [&](std::int32_t z, const auto &ls, std::int32_t mean, std::int32_t sd)
            {
                const std::vector<std::int32_t> v =
                    on_knock_out_step([&]
                                      { return conditioned_draw(z, ls(), mean, sd); }, {z, ko_one});
                out(l.ko(), v[1]);
                return v[0];
            };
            const int n = l.n_assets;
            // The step's rate: r, or on a forward curve the curve's carry
            // over the step plus r (WP 32 F4), as the CPU model's T(c_k) + r.
            const auto rate = [&]
            { return l.carry_col >= 0 ? b.add(data(l.carry_col), theta(l.th_rate)) : theta(l.th_rate); };
            switch (l.kind)
            {
                case DeviceModel::Kind::BlackScholes:
                    for (int a = 0; a < n; ++a)
                    {
                        std::int32_t z = -1;
                        if (n == 1)
                            z = draw(0);
                        else
                        {
                            z = b.constant(0.0); // acc = 0.0; acc += chol * w
                            for (int c = 0; c < n; ++c)
                                z = b.add(z, b.mul(theta(l.th_chol + a * n + c), draw(c)));
                        }
                        std::int32_t m = -1, sz = -1;
                        if (!l.risk) // drift[s] + vol_sqrt_dt[s] * z, tabulated by the CPU model
                            m = data(a), sz = data(n + a);
                        else
                        {
                            const std::int32_t vol = theta(l.th_vol + a);
                            const std::int32_t mu = b.sub(b.sub(rate(), theta(l.th_div + a)),
                                                          b.mul(b.mul(b.constant(0.5), vol), vol));
                            m = b.mul(mu, data(0)), sz = b.mul(vol, data(1));
                        }
                        if (l.knock_out) // one asset (path_program_unsupported)
                            z = knock_out(z, [&]
                                          { return b.value(Op::Log, state(a)); }, m, sz);
                        const std::int32_t x = b.add(m, b.mul(sz, z));
                        last(a, state(a), m, sz);
                        const std::int32_t S_next = b.mul(state(a), exp(x));
                        out(a, S_next);
                        if (a == 0 && l.n_surv > 0)
                            survive(state(0), S_next, b.mul(sz, sz));
                    }
                    break;
                case DeviceModel::Kind::LocalVol:
                {
                    const std::int32_t S = state(0);
                    const std::int32_t sig = b.value(Op::GridVol, S, data(0));
                    const std::int32_t drift =
                        b.mul(b.sub(b.sub(rate(), theta(l.th_div)), b.mul(b.mul(b.constant(0.5), sig), sig)),
                              data(1));
                    const std::int32_t vol_sqrt_dt = b.mul(sig, data(2));
                    // drift + sd z, and with Milstein before the dates (#371),
                    // on a step that reaches no date, Milstein's term
                    // 1/2 sigma (S dsigma/dS) dt (z^2 - 1), as
                    // mc::local_vol_milstein_step: in a region on the step
                    // row's flag, the same for every path of a step.
                    const auto log_return = [&](std::int32_t z)
                    {
                        const std::int32_t x = b.add(drift, b.mul(vol_sqrt_dt, z));
                        if (!l.milstein)
                            return x;
                        const std::int32_t head = b.raw(Op::If, data(l.mil_col));
                        b.open_scope();
                        const std::int32_t slope = b.value(Op::GridSlope, S, data(0));
                        const std::int32_t correction =
                            b.mul(b.mul(b.mul(b.mul(b.constant(0.5), sig), b.mul(S, slope)), data(1)),
                                  b.sub(b.mul(z, z), b.constant(1.0)));
                        const std::int32_t xm = b.add(x, correction);
                        b.close_scope();
                        const std::int32_t mid = b.raw(Op::Else, head);
                        const std::int32_t end = b.raw(Op::EndIf, head);
                        b.at(mid).c = end;
                        b.at(head).b = mid;
                        b.at(head).c = end;
                        return b.raw(Op::Phi, xm, x, head);
                    };
                    std::int32_t S_next = -1;
                    if (!l.knock_out)
                        S_next = b.mul(S, exp(log_return(draw(0))));
                    else if (!l.half) // LocalVolSimModel: local_vol_step on the conditioned draw
                        S_next = b.mul(S, exp(log_return(knock_out(draw(0), [&]
                                                                   { return b.value(Op::Log, S); }, drift, vol_sqrt_dt))));
                    else
                    {
                        // A coarse path conditioned on the fine one: only the
                        // part of the step after the fine sub-step a is
                        // conditioned, from S e^{drift w + sd a}, with the
                        // fine path's own last draw (z - a) / sqrt(1 - w).
                        // The CPU model conditions a step whose share w < 1
                        // and takes w = a = 0 otherwise; so does this one.
                        const std::int32_t S_plain = b.mul(S, exp(log_return(draw(0))));
                        const std::vector<std::int32_t> v = on_knock_out_step(
                            [&]
                            {
                                const std::int32_t split = b.value(Op::Lt, half_w, b.constant(1.0));
                                const std::int32_t w = b.select(split, half_w, b.constant(0.0));
                                const std::int32_t a = b.select(split, half_a, b.constant(0.0));
                                const std::int32_t rest = b.sub(b.constant(1.0), w);
                                const std::int32_t S_start =
                                    b.mul(S, exp(b.add(b.mul(drift, w), b.mul(vol_sqrt_dt, a))));
                                const std::int32_t mean = b.mul(drift, rest);
                                const std::int32_t sd = b.mul(vol_sqrt_dt, b.value(Op::Sqrt, rest));
                                const std::int32_t z2 = b.div(b.sub(draw(0), a), b.value(Op::Sqrt, rest));
                                const std::vector<std::int32_t> c =
                                    conditioned_draw(z2, b.value(Op::Log, S_start), mean, sd);
                                return std::vector<std::int32_t>{b.mul(S_start, exp(b.add(mean, b.mul(sd, c[0])))),
                                                                 c[1]};
                            },
                            {S_plain, ko_one});
                        S_next = v[0];
                        out(l.ko(), v[1]);
                    }
                    out(0, S_next);
                    last(0, S, drift, vol_sqrt_dt);
                    if (l.n_surv > 0)
                    {
                        if (l.half)
                            survive_through_midpoint(S, S_next, drift, vol_sqrt_dt);
                        else
                            survive(S, S_next, b.mul(vol_sqrt_dt, vol_sqrt_dt));
                    }
                    break;
                }
                case DeviceModel::Kind::Heston:
                case DeviceModel::Kind::SLV:
                {
                    const bool slv = l.kind == DeviceModel::Kind::SLV;
                    const std::int32_t S = state(0), V = state(1);
                    const std::int32_t dt = data(slv ? 1 : 0), sqdt = data(slv ? 2 : 1);
                    // slv_step reads its leverage before anything else
                    const std::int32_t lev = slv ? b.value(Op::GridLev, S, data(0)) : -1;
                    if (l.qe)
                    {
                        // mc::qe_step (slv_qe_step), operation for operation:
                        // draw 0 sets the variance, draw 1 the spot's
                        // independent part. Both branches are computed and
                        // the step's selected, as the price kernels have no
                        // derivative to keep (risk layouts never take QE).
                        const int h = l.th_heston;
                        const std::int32_t L = slv ? lev : b.constant(1.0);
                        const std::int32_t k = theta(h + 1), th = theta(h + 2), xi = theta(h + 3), rho = theta(h + 4);
                        const std::int32_t one = b.constant(1.0), two = b.constant(2.0), half = b.constant(0.5);
                        const std::int32_t zero = b.constant(0.0);
                        const std::int32_t ekt = exp(b.mul(b.neg(k), dt));
                        const std::int32_t one_m = b.sub(one, ekt);
                        const std::int32_t m = b.add(th, b.mul(b.sub(V, th), ekt));
                        const std::int32_t s2 =
                            b.add(b.div(b.mul(b.mul(b.mul(b.mul(V, xi), xi), ekt), one_m), k),
                                  b.div(b.mul(b.mul(b.mul(b.mul(th, xi), xi), one_m), one_m), b.mul(two, k)));
                        const std::int32_t psi = b.div(s2, b.mul(m, m));
                        const std::int32_t Lr = b.mul(L, rho);
                        const std::int32_t K0 = b.div(b.mul(b.mul(b.mul(b.neg(Lr), k), th), dt), xi);
                        const std::int32_t c = b.mul(b.mul(half, dt), b.sub(b.div(b.mul(k, Lr), xi), b.mul(b.mul(half, L), L)));
                        const std::int32_t K1 = b.sub(c, b.div(Lr, xi));
                        const std::int32_t K2 = b.add(c, b.div(Lr, xi));
                        const std::int32_t K3 = b.mul(b.mul(b.mul(b.mul(half, dt), L), L), b.sub(one, b.mul(rho, rho)));
                        const std::int32_t K4 = K3;
                        const std::int32_t A = b.add(K2, b.mul(half, K4));
                        const std::int32_t zv = draw(0), zs = draw(1);
                        // quadratic branch
                        const std::int32_t inv = b.div(one, psi);
                        const std::int32_t two_inv = b.mul(two, inv);
                        const std::int32_t b2 = b.add(b.sub(two_inv, one), b.mul(b.value(Op::Sqrt, two_inv),
                                                                                 b.value(Op::Sqrt, b.sub(two_inv, one))));
                        const std::int32_t a = b.div(m, b.add(one, b2));
                        const std::int32_t bb = b.value(Op::Sqrt, b2);
                        const std::int32_t vq = b.mul(b.mul(a, b.add(bb, zv)), b.add(bb, zv));
                        const std::int32_t cq = b.value(Op::Lt, A, b.div(one, b.mul(two, a)));
                        const std::int32_t om = b.sub(one, b.mul(b.mul(two, A), a));
                        const std::int32_t lq = b.sub(b.div(b.mul(b.mul(A, b2), a), om), b.mul(half, b.value(Op::Log, om)));
                        // exponential branch
                        const std::int32_t p = b.div(b.sub(psi, one), b.add(psi, one));
                        const std::int32_t beta = b.div(b.sub(one, p), m);
                        const std::int32_t tail = b.value(Op::Ncdf, b.neg(zv)); // 0.5 erfc(zv / sqrt 2)
                        const std::int32_t ve = b.select(b.value(Op::Le, b.sub(one, tail), p), zero,
                                                         b.div(b.value(Op::Log, b.div(b.sub(one, p), tail)), beta));
                        const std::int32_t ce = b.value(Op::Lt, A, beta);
                        const std::int32_t le =
                            b.value(Op::Log, b.add(p, b.div(b.mul(beta, b.sub(one, p)), b.sub(beta, A))));
                        const std::int32_t quad = b.value(Op::Le, psi, b.constant(mc::qe_psi_c()));
                        const std::int32_t v_next = b.select(quad, vq, ve);
                        const std::int32_t corrected = b.select(quad, cq, ce);
                        const std::int32_t log_mgf = b.select(quad, lq, le);
                        const std::int32_t K0s =
                            b.select(corrected, b.sub(b.neg(log_mgf), b.mul(b.add(K1, b.mul(half, K3)), V)), K0);
                        const std::int32_t var = b.add(b.mul(K3, V), b.mul(K4, v_next));
                        const std::int32_t qe_mean =
                            b.add(b.add(b.add(b.mul(b.sub(rate(), theta(l.th_div)), dt), K0s), b.mul(K1, V)),
                                  b.mul(K2, v_next));
                        const std::int32_t qe_sd = b.value(Op::Sqrt, b.value(Op::Max, var, zero));
                        // a knock-out conditions the spot's draw given V' (mc::qe_variance_step)
                        const std::int32_t zs_eff = l.knock_out ? knock_out(zs, [&]
                                                                            { return b.value(Op::Log, S); }, qe_mean, qe_sd)
                                                                : zs;
                        const std::int32_t x = b.add(qe_mean, b.mul(qe_sd, zs_eff));
                        out(0, b.mul(S, exp(x)));
                        if (l.last_step) // as the Euler step leaves them: forward S e^{(r-q)dt}, L^2 v+ dt
                        {
                            const std::int32_t v_plus = b.value(Op::Max, V, b.constant(mc::variance_floor()));
                            const std::int32_t sqrt_v = b.value(Op::Sqrt, v_plus);
                            const std::int32_t Lv = b.mul(b.mul(L, L), v_plus);
                            const std::int32_t mm =
                                b.mul(b.sub(b.sub(rate(), theta(l.th_div)), b.mul(half, Lv)), dt);
                            last(0, S, mm, b.mul(b.mul(L, sqrt_v), sqdt));
                        }
                        out(1, v_next);
                        break;
                    }
                    const std::int32_t v_plus = b.value(Op::Max, V, b.constant(mc::variance_floor()));
                    const std::int32_t sqrt_v = b.value(Op::Sqrt, v_plus);
                    std::int32_t log_return = -1, m = -1, sz = -1;
                    // the spot's draw, conditioned at a knock-out date; it
                    // drives the variance below too (heston_variance_step)
                    std::int32_t z_spot = draw(0);
                    if (slv) // (r - q - 0.5 L L v+) dt + L sqrt(v+) (sqrt(dt) z)
                    {
                        m = b.mul(b.sub(b.sub(rate(), theta(l.th_div)),
                                        b.mul(b.mul(b.mul(b.constant(0.5), lev), lev), v_plus)),
                                  dt);
                        if (l.knock_out)
                            z_spot = knock_out(z_spot, [&]
                                               { return b.value(Op::Log, S); }, m, b.mul(b.mul(lev, sqrt_v), sqdt));
                        log_return = b.add(m, b.mul(b.mul(lev, sqrt_v), b.mul(sqdt, z_spot)));
                        if (l.last_step)
                            sz = b.mul(b.mul(lev, sqrt_v), sqdt);
                    }
                    else
                    {
                        // heston_log_return with lambda = k = 0, written out
                        const std::int32_t zero = b.constant(0.0);
                        const std::int32_t mu =
                            b.sub(b.sub(b.sub(rate(), theta(l.th_div)), b.mul(zero, zero)),
                                  b.mul(b.constant(0.5), v_plus));
                        m = b.mul(mu, dt);
                        if (l.knock_out)
                            z_spot = knock_out(z_spot, [&]
                                               { return b.value(Op::Log, S); }, m, b.mul(sqrt_v, sqdt));
                        log_return = b.add(m, b.mul(sqrt_v, b.mul(sqdt, z_spot)));
                        if (l.last_step)
                            sz = b.mul(sqrt_v, sqdt);
                    }
                    out(0, b.mul(S, exp(log_return)));
                    last(0, S, m, sz);
                    // heston_variance_step
                    const int h = l.th_heston;
                    const std::int32_t rho = theta(h + 4);
                    const std::int32_t dW =
                        b.add(b.mul(rho, z_spot),
                              b.mul(b.value(Op::Sqrt, b.sub(b.constant(1.0), b.mul(rho, rho))), draw(1)));
                    out(1, b.add(b.add(V, b.mul(b.mul(theta(h + 1), b.sub(theta(h + 2), v_plus)), dt)),
                                 b.mul(b.mul(theta(h + 3), sqrt_v), b.mul(sqdt, dW))));
                    break;
                }
            }
            return b.take();
        }
    } // namespace

    PathProgram path_program(const Script &script, DeviceModel::Kind kind, int n_assets, bool risk, bool last_step,
                             bool qe, bool half, bool milstein, bool curve)
    {
        const std::string why = path_program_unsupported(script, kind, n_assets);
        if (!why.empty())
            throw InvalidInput("script IR: " + why);
        PathProgram p;
        p.layout = path_layout(kind, n_assets, script, risk, last_step, half, milstein, curve);
        p.layout.qe = qe && !risk && (kind == DeviceModel::Kind::Heston || kind == DeviceModel::Kind::SLV);
        p.init = init_block(p.layout);
        p.step = step_block(p.layout);
        for (const Block &b : script.blocks)
            p.events.push_back(bind_event(b, p.layout));
        return p;
    }

    PathData path_data(const PathLayout &l, const DeviceModel &m, const Script &script,
                       const std::vector<std::vector<Time>> &discount_mats, const std::vector<Real> &baseline)
    {
        PathData d;
        const int n = l.n_assets;
        d.theta.assign(static_cast<std::size_t>(l.n_theta), 0.0);
        d.theta[static_cast<std::size_t>(l.th_rate)] = m.r;
        for (int k = 0; k < n; ++k)
            d.theta[static_cast<std::size_t>(l.th_spot + k)] = m.s0[static_cast<std::size_t>(k)];
        if (l.kind == DeviceModel::Kind::BlackScholes)
        {
            for (int k = 0; k < n; ++k)
            {
                d.theta[static_cast<std::size_t>(l.th_div + k)] = m.divs.empty() ? 0.0 : m.divs[static_cast<std::size_t>(k)];
                d.theta[static_cast<std::size_t>(l.th_vol + k)] = m.vols.empty() ? 0.0 : m.vols[static_cast<std::size_t>(k)];
            }
            if (l.th_chol >= 0)
                std::copy(m.chol.begin(), m.chol.end(), d.theta.begin() + l.th_chol);
        }
        else
            d.theta[static_cast<std::size_t>(l.th_div)] = m.q;
        if (l.th_heston >= 0)
        {
            const double h[5] = {m.heston.v0, m.heston.kappa, m.heston.theta, m.heston.xi, m.heston.rho};
            std::copy(h, h + 5, d.theta.begin() + l.th_heston);
        }
        if (l.th_grid >= 0)
            d.theta.insert(d.theta.end(), m.grid.begin(), m.grid.end());

        const std::size_t n_steps = m.t.size(), cols = static_cast<std::size_t>(l.step_cols);
        d.step_rows.assign(n_steps * cols, 0.0);
        std::vector<Time> event_time(script.n_events(), 0.0);
        Time t_prev = 0.0;
        for (std::size_t s = 0; s < n_steps; ++s)
        {
            const Time ts = m.t[s], dt = ts - t_prev;
            double *row = d.step_rows.data() + s * cols;
            if (l.kind == DeviceModel::Kind::BlackScholes && !l.risk)
            {
                for (int a = 0; a < n; ++a)
                {
                    const std::size_t i = s * static_cast<std::size_t>(n) + static_cast<std::size_t>(a);
                    row[a] = i < m.drift.size() ? m.drift[i] : 0.0;
                    row[n + a] = i < m.vol_sqrt_dt.size() ? m.vol_sqrt_dt[i] : 0.0;
                }
            }
            else if (l.kind == DeviceModel::Kind::LocalVol || l.kind == DeviceModel::Kind::SLV)
                row[0] = ts, row[1] = dt, row[2] = std::sqrt(dt);
            else
                row[0] = dt, row[1] = std::sqrt(dt);
            if (l.half)
            {
                if (m.half_share.size() != n_steps)
                    throw InvalidInput("script IR: a conditioned coarse path needs each step's fine share");
                row[l.half_col] = m.half_share[s];
            }
            if (l.milstein) // Milstein on a step that reaches no date
                row[l.mil_col] = m.event[s] < 0 ? 1.0 : 0.0;
            if (l.carry_col >= 0) // the curve's carry rate over the step (WP 32 F4)
            {
                if (m.step_carry.size() != n_steps)
                    throw InvalidInput("script IR: a model on a forward curve needs each step's carry");
                row[l.carry_col] = m.step_carry[s];
            }
            t_prev = ts;
            if (m.event[s] >= 0)
                event_time[static_cast<std::size_t>(m.event[s])] = ts;
        }
        if (l.n_surv > 0)
        {
            // Each step's levels are those of the date it leads to: the
            // first date at or after it. A slot that date does not use
            // gets a harmless level of 1, above.
            int next = -1;
            for (std::size_t s = n_steps; s-- > 0;)
            {
                if (m.event[s] >= 0)
                    next = m.event[s];
                double *row = d.step_rows.data() + s * cols + l.surv_col0;
                std::vector<std::size_t> slots;
                const Block *block = nullptr;
                if (next >= 0)
                {
                    block = &script.blocks[static_cast<std::size_t>(script.event_block[static_cast<std::size_t>(next)])];
                    slots = survival_slots(*block);
                }
                for (int j = 0; j < l.n_surv; ++j)
                {
                    const bool used = static_cast<std::size_t>(j) < slots.size();
                    const StepMonitor *q = used ? &block->quantities[slots[static_cast<std::size_t>(j)]] : nullptr;
                    row[2 * j] = used ? q->level : 1.0;
                    row[2 * j + 1] = used ? (q->above ? 1.0 : 0.0) : 1.0;
                }
            }
        }

        if (l.knock_out)
        {
            // The step that reaches a knock-out date: its corridor (the
            // tightest level on each side, mc::knock_out_corridor), the logs
            // of its levels; the other steps' flag is 0, their columns 0.
            for (std::size_t s = 0; s < n_steps; ++s)
            {
                if (m.event[s] < 0)
                    continue;
                const Block &block =
                    script.blocks[static_cast<std::size_t>(script.event_block[static_cast<std::size_t>(m.event[s])])];
                double lo = 0.0, hi = std::numeric_limits<double>::infinity();
                bool any = false;
                for (const StepMonitor &q : block.quantities)
                    if (q.kind == StepQuantity::KnockOut)
                    {
                        any = true;
                        if (q.above)
                            hi = std::min(hi, q.level);
                        else
                            lo = std::max(lo, q.level);
                    }
                if (!any)
                    continue;
                double *row = d.step_rows.data() + s * cols + l.ko_col0;
                row[0] = 1.0;
                row[1] = lo > 0.0 ? 1.0 : 0.0;
                row[2] = lo > 0.0 ? std::log(lo) : 0.0;
                row[3] = std::isfinite(hi) ? 1.0 : 0.0;
                row[4] = std::isfinite(hi) ? std::log(hi) : 0.0;
            }
        }

        const std::size_t ecols = static_cast<std::size_t>(l.event_cols);
        d.event_rows.assign(script.n_events() * ecols, 0.0);
        for (std::size_t e = 0; e < script.n_events(); ++e)
        {
            double *row = d.event_rows.data() + e * ecols;
            const Time ts = event_time[e];
            // On a forward curve (WP 32 F4), its factors times the shifts',
            // as the CPU model's ModelCurve::write_rates; a risk program's
            // rows keep the times, and the factors from factor_col0.
            const ForwardCurve *c = l.curve && m.curve ? &*m.curve : nullptr;
            const Real df_t = c ? c->discount(ts) : 1.0;
            if (c && l.risk)
            {
                double *f = row + l.factor_col0;
                f[0] = 1.0 / df_t;
                const std::vector<Time> &dm = discount_mats[e];
                for (std::size_t i = 0; i < dm.size() && 1 + i < static_cast<std::size_t>(l.factor_col0); ++i)
                    f[1 + i] = c->discount(dm[i]) / df_t;
                if (e < script.event_fwd_mats.size())
                    for (std::size_t i = 0; i < script.event_fwd_mats[e].size(); ++i)
                        if (static_cast<std::size_t>(l.fwd_col0) + i < static_cast<std::size_t>(l.factor_col0))
                            f[static_cast<std::size_t>(l.fwd_col0) + i] =
                                std::exp(c->carry(ts, script.event_fwd_mats[e][i]));
                c = nullptr; // the times below, as a flat risk program's
            }
            row[0] = l.risk ? ts : c ? (1.0 / df_t) * std::exp(m.r * ts)
                                     : std::exp(m.r * ts);
            const std::vector<Time> &mats = discount_mats[e];
            for (std::size_t i = 0; i < mats.size() && 1 + i < ecols; ++i)
                row[1 + i] = l.risk ? mats[i] - ts
                             : c    ? (c->discount(mats[i]) / df_t) * std::exp(-m.r * (mats[i] - ts))
                                    : std::exp(-m.r * (mats[i] - ts));
            if (e < script.event_fwd_mats.size())
            {
                const double carry = m.r - (m.n_assets > 1 ? m.divs[0] : m.q);
                const std::vector<Time> &fwds = script.event_fwd_mats[e];
                for (std::size_t i = 0; i < fwds.size(); ++i)
                {
                    const std::size_t col = static_cast<std::size_t>(l.fwd_col0) + i;
                    if (col < ecols)
                        row[col] = l.risk ? fwds[i] - ts
                                   : c    ? std::exp(c->carry(ts, fwds[i])) * std::exp(carry * (fwds[i] - ts))
                                          : std::exp(carry * (fwds[i] - ts));
                }
            }
        }

        d.baseline.assign(static_cast<std::size_t>(l.n_vars), 0.0);
        for (std::size_t i = 0; i < baseline.size() && i < d.baseline.size(); ++i)
            d.baseline[i] = baseline[i];
        d.terms.push_back(0.0); // never empty: a device pointer to give
        for (std::size_t e = 0; e < script.n_events(); ++e)
        {
            d.term_begin.push_back(static_cast<std::int32_t>(d.terms.size()));
            if (e < script.event_terms.size())
                d.terms.insert(d.terms.end(), script.event_terms[e].begin(), script.event_terms[e].end());
        }
        return d;
    }

    PathScratch::PathScratch(const PathProgram &p)
    {
        std::size_t longest = std::max(p.init.code.size(), p.step.code.size());
        for (const Block &b : p.events)
            longest = std::max(longest, b.code.size());
        state.assign(static_cast<std::size_t>(p.layout.n_state()), 0.0);
        val.assign(std::max<std::size_t>(longest, 1), 0.0);
    }

    double run_path(const PathProgram &p, const PathData &data, const DeviceModel &model, const Script &script,
                    mc::PathDraws &draws, double sign, PathScratch &scratch)
    {
        const PathLayout &l = p.layout;
        double z[mc::ScriptLimits::kAssets] = {};
        Frame f;
        f.state = scratch.state.data();
        f.theta = data.theta.data();
        f.draws = z;
        f.grid_K = model.K.data();
        f.grid_nK = static_cast<int>(model.K.size());
        f.grid_T = model.T_grid.data();
        f.grid_nT = static_cast<int>(model.T_grid.size());
        f.grid0 = l.th_grid;
        f.data = data.baseline.data();
        run_block(p.init, f, scratch.val.data());
        int d = 0;
        for (std::size_t s = 0; s < model.t.size(); ++s)
        {
            if (model.draws[s])
            {
                for (int k = 0; k < l.factors; ++k)
                    z[k] = draws.draw(d, k, sign);
                f.data = data.step_rows.data() + s * static_cast<std::size_t>(l.step_cols);
                run_block(p.step, f, scratch.val.data());
                ++d;
            }
            const int e = model.event[s];
            if (e < 0)
                continue;
            f.data = data.event_rows.data() + static_cast<std::size_t>(e) * static_cast<std::size_t>(l.event_cols);
            f.terms = data.terms.data() + data.term_begin[static_cast<std::size_t>(e)];
            run_block(p.events[static_cast<std::size_t>(script.event_block[static_cast<std::size_t>(e)])], f,
                      scratch.val.data());
        }
        return scratch.state[static_cast<std::size_t>(l.pay())];
    }

} // namespace quantModeling::scripting::ir
