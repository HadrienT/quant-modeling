#include "quantModeling/scripting/ir_path.hpp"

#include <algorithm>
#include <cmath>

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

    PathLayout path_layout(DeviceModel::Kind kind, int n_assets, const Script &script, bool risk)
    {
        PathLayout l;
        l.kind = kind;
        l.n_assets = n_assets;
        l.n_vars = script.n_vars;
        l.risk = risk;
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
        int max_df = -1;
        for (const Block &b : script.blocks)
            max_df = std::max(max_df, static_cast<int>(b.max_df));
        l.event_cols = 2 + max_df;
        return l;
    }

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
                    break;
                case Op::Df: // exp(-r (T - t))
                    id = l.risk ? b.value(Op::Exp,
                                          b.mul(b.neg(b.input(Op::Theta, l.th_rate)), b.input(Op::Data, 1 + in.a)))
                                : b.input(Op::Data, 1 + in.a);
                    break;
                case Op::Surv:
                    throw InvalidInput("script IR: survivalAbove() / survivalBelow() are not bound to a path yet");
                default:
                    r.copy(k);
                    continue;
            }
            r.map[static_cast<std::size_t>(k)] = id;
        }
        Block out = b.take();
        for (const Out &o : event.outs)
            out.outs.push_back({Op::State, o.cls == Op::VarIn ? l.var0() + o.index : l.pay(), kPrimal, r[o.value], false});
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
            const int n = l.n_assets;
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
                        std::int32_t x = -1;
                        if (!l.risk) // drift[s] + vol_sqrt_dt[s] * z, tabulated by the CPU model
                            x = b.add(data(a), b.mul(data(n + a), z));
                        else
                        {
                            const std::int32_t vol = theta(l.th_vol + a);
                            const std::int32_t mu = b.sub(b.sub(theta(l.th_rate), theta(l.th_div + a)),
                                                          b.mul(b.mul(b.constant(0.5), vol), vol));
                            x = b.add(b.mul(mu, data(0)), b.mul(b.mul(vol, data(1)), z));
                        }
                        out(a, b.mul(state(a), exp(x)));
                    }
                    break;
                case DeviceModel::Kind::LocalVol:
                {
                    const std::int32_t S = state(0);
                    const std::int32_t sig = b.value(Op::GridVol, S, data(0));
                    const std::int32_t drift =
                        b.mul(b.sub(b.sub(theta(l.th_rate), theta(l.th_div)), b.mul(b.mul(b.constant(0.5), sig), sig)),
                              data(1));
                    const std::int32_t vol_sqrt_dt = b.mul(sig, data(2));
                    out(0, b.mul(S, exp(b.add(drift, b.mul(vol_sqrt_dt, draw(0))))));
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
                    const std::int32_t v_plus = b.value(Op::Max, V, b.constant(mc::variance_floor()));
                    const std::int32_t sqrt_v = b.value(Op::Sqrt, v_plus);
                    std::int32_t log_return = -1;
                    if (slv) // (r - q - 0.5 L L v+) dt + L sqrt(v+) (sqrt(dt) z)
                        log_return = b.add(
                            b.mul(b.sub(b.sub(theta(l.th_rate), theta(l.th_div)),
                                        b.mul(b.mul(b.mul(b.constant(0.5), lev), lev), v_plus)),
                                  dt),
                            b.mul(b.mul(lev, sqrt_v), b.mul(sqdt, draw(0))));
                    else
                    {
                        // heston_log_return with lambda = k = 0, written out
                        const std::int32_t zero = b.constant(0.0);
                        const std::int32_t mu =
                            b.sub(b.sub(b.sub(theta(l.th_rate), theta(l.th_div)), b.mul(zero, zero)),
                                  b.mul(b.constant(0.5), v_plus));
                        log_return = b.add(b.mul(mu, dt), b.mul(sqrt_v, b.mul(sqdt, draw(0))));
                    }
                    out(0, b.mul(S, exp(log_return)));
                    // heston_variance_step
                    const int h = l.th_heston;
                    const std::int32_t rho = theta(h + 4);
                    const std::int32_t dW =
                        b.add(b.mul(rho, draw(0)),
                              b.mul(b.value(Op::Sqrt, b.sub(b.constant(1.0), b.mul(rho, rho))), draw(1)));
                    out(1, b.add(b.add(V, b.mul(b.mul(theta(h + 1), b.sub(theta(h + 2), v_plus)), dt)),
                                 b.mul(b.mul(theta(h + 3), sqrt_v), b.mul(sqdt, dW))));
                    break;
                }
            }
            return b.take();
        }
    } // namespace

    PathProgram path_program(const Script &script, DeviceModel::Kind kind, int n_assets, bool risk)
    {
        const std::string why = path_program_unsupported(kind, n_assets);
        if (!why.empty())
            throw InvalidInput("script IR: " + why);
        PathProgram p;
        p.layout = path_layout(kind, n_assets, script, risk);
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
            t_prev = ts;
            if (m.event[s] >= 0)
                event_time[static_cast<std::size_t>(m.event[s])] = ts;
        }

        const std::size_t ecols = static_cast<std::size_t>(l.event_cols);
        d.event_rows.assign(script.n_events() * ecols, 0.0);
        for (std::size_t e = 0; e < script.n_events(); ++e)
        {
            double *row = d.event_rows.data() + e * ecols;
            const Time ts = event_time[e];
            row[0] = l.risk ? ts : std::exp(m.r * ts);
            const std::vector<Time> &mats = discount_mats[e];
            for (std::size_t i = 0; i < mats.size() && 1 + i < ecols; ++i)
                row[1 + i] = l.risk ? mats[i] - ts : std::exp(-m.r * (mats[i] - ts));
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
