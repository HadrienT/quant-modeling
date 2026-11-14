#include "quantModeling/scripting/ir_ad.hpp"

#include <algorithm>
#include <map>

#include "quantModeling/engines/mc/script_path.hpp"

namespace quantModeling::scripting::ir
{

    namespace
    {
        [[noreturn]] void script_level()
        {
            throw InvalidInput("script IR: a derivative is taken of a path-level block (bind the script's block to "
                               "a path first)");
        }

        /// Whether a sensitivity can flow into what instruction `in` defines.
        bool differentiable(const Inst &in)
        {
            switch (in.op)
            {
                case Op::Const:
                case Op::Param:
                case Op::Draw:
                case Op::Data:
                case Op::Eq:
                case Op::Ne:
                case Op::Gt:
                case Op::Ge:
                case Op::Lt:
                case Op::Le:
                case Op::And:
                case Op::Or:
                case Op::Not:
                case Op::GridSlope:
                case Op::GridLevSlope:
                    return false;
                default:
                    return true;
            }
        }
    } // namespace

    // ── tangent ────────────────────────────────────────────────────────────

    Block tangent(const Block &src, const std::vector<std::int32_t> &dirs)
    {
        const int K = static_cast<int>(dirs.size());
        const auto n = static_cast<std::int32_t>(src.code.size());
        Rebuilder r(src);
        Builder &b = r.b;
        std::vector<std::int32_t> tan(static_cast<std::size_t>(n) * static_cast<std::size_t>(std::max(K, 1)), -1);
        const auto T = [&](std::int32_t k, int d) -> std::int32_t &
        { return tan[static_cast<std::size_t>(k) * static_cast<std::size_t>(K) + static_cast<std::size_t>(d)]; };
        const auto zero = [&]()
        { return b.constant(0.0); };
        const auto zero_or = [&](std::int32_t t)
        { return t < 0 ? zero() : t; };
        // df * dx, 0 when dx is: a derivative that is not finite must not
        // turn a zero sensitivity into NaN (dual_detail::times).
        const auto times = [&](std::int32_t df, std::int32_t dx)
        { return b.select(b.value(Op::Eq, dx, zero()), zero(), b.mul(df, dx)); };
        const auto interior = [&](std::int32_t v)
        { return b.value(Op::And, b.value(Op::Gt, v, zero()), b.value(Op::Lt, v, b.constant(1.0))); };
        const auto sum = [&](std::int32_t x, std::int32_t y)
        { return x < 0 ? y : (y < 0 ? x : b.add(x, y)); };

        for (std::int32_t k = 0; k < n; ++k)
        {
            const Inst &in = src.code[static_cast<std::size_t>(k)];
            const std::int32_t id = r.copy(k);
            if (in.op == Op::GridAccum || in.op == Op::GridLevAccum)
                throw InvalidInput("script IR: the second order through the local-vol grid is not built yet");
            if (!defines_value(in.op))
                continue;
            const unsigned operands = value_operands(in.op);
            const std::int32_t A = (operands & 1u) ? r[in.a] : -1, B = (operands & 2u) ? r[in.b] : -1;
            for (int d = 0; d < K; ++d)
            {
                const std::int32_t ta = (operands & 1u) && in.a >= 0 ? T(in.a, d) : -1;
                const std::int32_t tb = (operands & 2u) && in.b >= 0 ? T(in.b, d) : -1;
                std::int32_t t = -1;
                switch (in.op)
                {
                    case Op::VarIn:
                    case Op::PayIn:
                    case Op::Spot:
                    case Op::Df:
                    case Op::Surv:
                    case Op::Num:
                        script_level();
                    case Op::State: // the same slot's tangent (of the primal, or of the adjoint)
                        t = b.input(Op::State, in.a, in.c + 1 + d);
                        break;
                    case Op::Theta:
                        if (dirs[static_cast<std::size_t>(d)] == in.a)
                            t = b.constant(1.0);
                        break;
                    case Op::Add:
                        t = sum(ta, tb);
                        break;
                    case Op::Sub:
                        t = tb < 0 ? ta : (ta < 0 ? b.neg(tb) : b.sub(ta, tb));
                        break;
                    case Op::Mul:
                        t = sum(ta < 0 ? -1 : b.mul(ta, B), tb < 0 ? -1 : b.mul(A, tb));
                        break;
                    case Op::Div: // q = a / b: da / b - (q / b) db
                    {
                        const std::int32_t left = ta < 0 ? -1 : b.div(ta, B);
                        const std::int32_t right = tb < 0 ? -1 : b.mul(b.div(id, B), tb);
                        t = right < 0 ? left : (left < 0 ? b.neg(right) : b.sub(left, right));
                        break;
                    }
                    case Op::Neg:
                        t = ta < 0 ? -1 : b.neg(ta);
                        break;
                    case Op::Min:
                        if (ta >= 0 || tb >= 0)
                            t = b.select(b.value(Op::Lt, A, B), zero_or(ta), zero_or(tb));
                        break;
                    case Op::Max:
                        if (ta >= 0 || tb >= 0)
                            t = b.select(b.value(Op::Gt, A, B), zero_or(ta), zero_or(tb));
                        break;
                    case Op::Log:
                        if (ta >= 0)
                            t = times(b.div(b.constant(1.0), A), ta);
                        break;
                    case Op::Exp:
                        if (ta >= 0)
                            t = b.mul(id, ta);
                        break;
                    case Op::Sqrt:
                        if (ta >= 0)
                            t = times(b.div(b.constant(0.5), id), ta);
                        break;
                    case Op::Abs:
                        if (ta >= 0)
                            t = b.select(b.value(Op::Ge, A, zero()), ta, b.neg(ta));
                        break;
                    case Op::Pow:
                        t = sum(ta < 0 ? -1 : times(b.div(b.mul(B, id), A), ta),
                                tb < 0 ? -1 : times(b.mul(id, b.value(Op::Log, A)), tb));
                        break;
                    case Op::Smooth: // clamp01((x + h) / (h + h)), flat outside (0, 1) and for h <= 0
                        if (ta >= 0 || tb >= 0)
                        {
                            const std::int32_t den = b.add(B, B), q = b.div(b.add(A, B), den);
                            const std::int32_t t_num = sum(ta, tb);
                            const std::int32_t left = b.div(t_num, den);
                            const std::int32_t tq = tb < 0 ? left : b.sub(left, b.mul(b.div(q, den), b.add(tb, tb)));
                            t = b.select(b.value(Op::And, b.value(Op::Gt, B, zero()), interior(q)), tq, zero());
                        }
                        break;
                    case Op::Select:
                    {
                        const std::int32_t tc = in.c >= 0 ? T(in.c, d) : -1;
                        if (tb >= 0 || tc >= 0)
                            t = b.select(A, zero_or(tb), zero_or(tc));
                        break;
                    }
                    case Op::Up: // clamp01((a + eps) / (2 eps))
                        if (ta >= 0)
                            t = b.select(interior(id), b.div(ta, b.constant(2.0 * in.x)), zero());
                        break;
                    case Op::Tent: // clamp01(1 - |a| / eps)
                        if (ta >= 0)
                            t = b.select(interior(id),
                                         b.neg(b.div(b.select(b.value(Op::Ge, A, zero()), ta, b.neg(ta)),
                                                     b.constant(in.x))),
                                         zero());
                        break;
                    case Op::GridVol:
                    case Op::GridLev:
                        if (ta >= 0)
                            t = b.mul(b.value(in.op == Op::GridVol ? Op::GridSlope : Op::GridLevSlope, A, B), ta);
                        break;
                    case Op::Phi:
                        if (ta >= 0 || tb >= 0)
                            t = b.raw(Op::Phi, zero_or(ta), zero_or(tb), r[in.c]);
                        break;
                    case Op::FuzzyPhi:
                    case Op::FuzzyPhiPay:
                    {
                        // d t' + (1 - d) e' where blended, plus d' (t - e):
                        // the payoff before cancels out of the payoff's blend.
                        const std::int32_t degree = src.code[static_cast<std::size_t>(in.c)].a;
                        const std::int32_t td = T(degree, d);
                        if (ta >= 0 || tb >= 0)
                            t = b.raw(Op::FuzzyPhi, zero_or(ta), zero_or(tb), r[in.c]);
                        if (td >= 0)
                            t = sum(t, b.select(interior(r[degree]), b.mul(td, b.sub(A, B)), zero()));
                        break;
                    }
                    default: // constants, terms, draws, data, logic, GridSlope: no derivative
                        break;
                }
                T(k, d) = t;
            }
        }
        Block out = b.take();
        for (const Out &o : src.outs)
        {
            out.outs.push_back({o.cls, o.index, o.tag, r[o.value], o.add});
            for (int d = 0; d < K; ++d)
            {
                const std::int32_t t = T(o.value, d);
                if (t >= 0)
                    out.outs.push_back({o.cls, o.index, o.tag + 1 + d, t, o.add});
                else if (!o.add)
                {
                    // An overwritten slot's tangent is overwritten too.
                    out.code.push_back(Inst{Op::Const, -1, -1, -1, 0.0});
                    out.outs.push_back(
                        {o.cls, o.index, o.tag + 1 + d, static_cast<std::int32_t>(out.code.size()) - 1, false});
                }
            }
        }
        remove_dead_code(out);
        return out;
    }

    // ── adjoint ────────────────────────────────────────────────────────────

    namespace
    {
        class Reverse
        {
          public:
            Reverse(const Block &src, std::int32_t n_risks)
                : src_(src), r_(src), b_(r_.b), n_(static_cast<std::int32_t>(src.code.size())), n_risks_(n_risks)
            {
            }

            Block run()
            {
                // What the block reads and writes.
                std::vector<std::int32_t> read, written;
                for (const Inst &in : src_.code)
                {
                    if (in.op == Op::State)
                    {
                        if (in.c != kPrimal)
                            throw InvalidInput("script IR: the adjoint is taken of a primal block");
                        if (!slot_.count(in.a))
                            slot_[in.a] = next_++, read.push_back(in.a);
                    }
                    else if (in.op == Op::Theta && in.a < n_risks_ && !theta_.count(in.a))
                        theta_[in.a] = next_++;
                    else if (in.op == Op::VarIn || in.op == Op::PayIn || in.op == Op::Spot || in.op == Op::Df ||
                             in.op == Op::Num || in.op == Op::Surv)
                        script_level();
                }
                for (const Out &o : src_.outs)
                {
                    if (o.cls != Op::State || o.tag != kPrimal)
                        throw InvalidInput("script IR: the adjoint is taken of a primal block");
                    written.push_back(o.index);
                    if (!slot_.count(o.index))
                        slot_[o.index] = next_++;
                }
                adj_.assign(static_cast<std::size_t>(n_ + next_), -1);

                for (std::int32_t k = 0; k < n_; ++k) // the values, recomputed
                    r_.copy(k);

                // Seeds: the adjoint of what the block wrote; a slot it only
                // read keeps its adjoint and adds to it.
                for (const Out &o : src_.outs)
                    accumulate(o.value, b_.input(Op::State, o.index, kAdjoint));
                for (std::int32_t slot : read)
                    if (std::find(written.begin(), written.end(), slot) == written.end())
                        at(n_ + slot_[slot]) = b_.input(Op::State, slot, kAdjoint);

                reverse(0, n_);

                Block out = b_.take();
                for (const auto &[slot, index] : slot_)
                {
                    std::int32_t v = at(n_ + index);
                    if (v < 0)
                    {
                        out.code.push_back(Inst{Op::Const, -1, -1, -1, 0.0});
                        v = static_cast<std::int32_t>(out.code.size()) - 1;
                    }
                    out.outs.push_back({Op::State, slot, kAdjoint, v, false});
                }
                for (const auto &[p, index] : theta_)
                    if (at(n_ + index) >= 0)
                        out.outs.push_back({Op::Theta, p, kAdjoint, at(n_ + index), true});
                remove_dead_code(out);
                return out;
            }

          private:
            std::int32_t &at(std::int32_t i) { return adj_[static_cast<std::size_t>(i)]; }
            const Inst &code(std::int32_t k) const { return src_.code[static_cast<std::size_t>(k)]; }
            std::int32_t zero() { return b_.constant(0.0); }
            std::int32_t zero_or(std::int32_t v) { return v < 0 ? zero() : v; }

            bool wants(std::int32_t i) const
            {
                if (i >= n_)
                    return true;
                const Inst &in = code(i);
                return differentiable(in) && !(in.op == Op::Theta && in.a >= n_risks_);
            }
            void accumulate(std::int32_t i, std::int32_t v)
            {
                if (i < 0 || !wants(i))
                    return;
                at(i) = at(i) < 0 ? v : b_.add(at(i), v);
            }
            /// x, or 0 when the sensitivity is: the tape does not propagate
            /// a zero adjoint through a derivative that is not finite.
            std::int32_t guarded(std::int32_t adjoint, std::int32_t x)
            {
                return b_.select(b_.value(Op::Eq, adjoint, zero()), zero(), x);
            }
            std::int32_t interior(std::int32_t v)
            {
                return b_.value(Op::And, b_.value(Op::Gt, v, zero()), b_.value(Op::Lt, v, b_.constant(1.0)));
            }

            /// After a region [head, end]: the adjoints its two sides left
            /// different become phis; those of the region's own values end.
            void merge(const std::vector<std::int32_t> &then_state, const std::vector<std::int32_t> &else_state,
                       std::int32_t if_id, std::int32_t head, std::int32_t end)
            {
                for (std::int32_t i = 0; i < static_cast<std::int32_t>(adj_.size()); ++i)
                {
                    const std::int32_t t = then_state[static_cast<std::size_t>(i)],
                                       e = else_state[static_cast<std::size_t>(i)];
                    if (i > head && i <= end)
                        at(i) = -1;
                    else if (t == e)
                        at(i) = t;
                    else
                        at(i) = b_.raw(Op::Phi, zero_or(t), zero_or(e), if_id);
                }
            }

            /// `if cond { body } else { }` around a reversed range; `seed`
            /// runs first inside it.
            template <class Seed>
            void guarded_region(std::int32_t cond, std::int32_t lo, std::int32_t hi, std::int32_t head,
                                std::int32_t end, const Seed &seed)
            {
                const std::vector<std::int32_t> saved = adj_;
                const std::int32_t if_id = b_.raw(Op::If, cond);
                b_.open_scope();
                seed();
                reverse(lo, hi);
                const std::vector<std::int32_t> ran = adj_;
                b_.close_scope();
                const std::int32_t else_id = b_.raw(Op::Else, if_id);
                const std::int32_t end_id = b_.raw(Op::EndIf, if_id);
                b_.at(if_id).b = else_id;
                b_.at(if_id).c = end_id;
                b_.at(else_id).c = end_id;
                merge(ran, saved, if_id, head, end);
            }

            void reverse_if(std::int32_t head, std::int32_t end)
            {
                const std::int32_t else_idx = code(head).b;
                const std::vector<std::int32_t> phis = pending_[head];
                const std::vector<std::int32_t> saved = adj_;
                const std::int32_t if_id = b_.raw(Op::If, r_[code(head).a]);
                b_.open_scope();
                for (std::int32_t phi : phis)
                    accumulate(code(phi).a, at(phi));
                reverse(head + 1, else_idx);
                const std::vector<std::int32_t> then_state = adj_;
                b_.close_scope();
                const std::int32_t else_id = b_.raw(Op::Else, if_id);
                adj_ = saved;
                b_.open_scope();
                for (std::int32_t phi : phis)
                    accumulate(code(phi).b, at(phi));
                reverse(else_idx + 1, end);
                const std::vector<std::int32_t> else_state = adj_;
                b_.close_scope();
                const std::int32_t end_id = b_.raw(Op::EndIf, if_id);
                b_.at(if_id).b = else_id;
                b_.at(if_id).c = end_id;
                b_.at(else_id).c = end_id;
                merge(then_state, else_state, if_id, head, end);
            }

            void reverse_fuzzy(std::int32_t head, std::int32_t end)
            {
                const std::int32_t else_idx = code(head).c, degree_src = code(head).a;
                const std::int32_t degree = r_[degree_src], one = b_.constant(1.0);
                // The then-block ran unless the degree was <= 0, the else-
                // block unless it was >= 1; in between, both, blended.
                const std::int32_t ran_then = b_.value(Op::Gt, degree, zero());
                const std::int32_t ran_else = b_.value(Op::Lt, degree, one);
                const std::int32_t blend = b_.value(Op::And, ran_then, ran_else);
                const std::int32_t w_then = b_.select(blend, degree, one);
                const std::int32_t w_else = b_.select(blend, b_.sub(one, degree), one);
                const std::vector<std::int32_t> phis = pending_[head];
                for (std::int32_t phi : phis) // d (t - e) / d degree, where blended
                    accumulate(degree_src,
                               b_.select(blend, b_.mul(at(phi), b_.sub(r_[code(phi).a], r_[code(phi).b])), zero()));
                guarded_region(ran_then, head + 1, else_idx, head, end, [&]
                               {
                                   for (std::int32_t phi : phis)
                                       accumulate(code(phi).a, b_.mul(at(phi), w_then)); });
                guarded_region(ran_else, else_idx + 1, end, head, end, [&]
                               {
                                   for (std::int32_t phi : phis)
                                       accumulate(code(phi).b, b_.mul(at(phi), w_else)); });
            }

            /// The instructions of [lo, hi), last first.
            void reverse(std::int32_t lo, std::int32_t hi)
            {
                for (std::int32_t k = hi - 1; k >= lo; --k)
                {
                    const Inst &in = code(k);
                    switch (in.op)
                    {
                        case Op::Phi:
                        case Op::FuzzyPhi:
                        case Op::FuzzyPhiPay:
                            if (at(k) >= 0)
                                pending_[in.c].push_back(k);
                            continue;
                        case Op::EndIf:
                            reverse_if(in.a, k);
                            k = in.a;
                            continue;
                        case Op::FuzzyEnd:
                            reverse_fuzzy(in.a, k);
                            k = in.a;
                            continue;
                        case Op::GridAccum:
                        case Op::GridLevAccum:
                            throw InvalidInput("script IR: the adjoint is taken of a primal block");
                        default:
                            break;
                    }
                    const std::int32_t a = at(k);
                    if (a < 0)
                        continue;
                    const unsigned operands = value_operands(in.op);
                    const std::int32_t A = (operands & 1u) ? r_[in.a] : -1, B = (operands & 2u) ? r_[in.b] : -1,
                                       V = r_[k];
                    switch (in.op)
                    {
                        case Op::State:
                            accumulate(n_ + slot_[in.a], a);
                            break;
                        case Op::Theta:
                            if (theta_.count(in.a))
                                accumulate(n_ + theta_[in.a], a);
                            break;
                        case Op::Add:
                            accumulate(in.a, a);
                            accumulate(in.b, a);
                            break;
                        case Op::Sub:
                            accumulate(in.a, a);
                            if (wants(in.b))
                                accumulate(in.b, b_.neg(a));
                            break;
                        case Op::Mul:
                            if (wants(in.a))
                                accumulate(in.a, b_.mul(a, B));
                            if (wants(in.b))
                                accumulate(in.b, b_.mul(a, A));
                            break;
                        case Op::Div:
                            if (wants(in.a))
                                accumulate(in.a, guarded(a, b_.div(a, B)));
                            if (wants(in.b))
                                accumulate(in.b, guarded(a, b_.neg(b_.mul(a, b_.div(V, B)))));
                            break;
                        case Op::Neg:
                            accumulate(in.a, b_.neg(a));
                            break;
                        case Op::Min:
                        case Op::Max:
                        {
                            const std::int32_t left = b_.value(in.op == Op::Min ? Op::Lt : Op::Gt, A, B);
                            if (wants(in.a))
                                accumulate(in.a, b_.select(left, a, zero()));
                            if (wants(in.b))
                                accumulate(in.b, b_.select(left, zero(), a));
                            break;
                        }
                        case Op::Log:
                            accumulate(in.a, guarded(a, b_.div(a, A)));
                            break;
                        case Op::Exp:
                            accumulate(in.a, b_.mul(a, V));
                            break;
                        case Op::Sqrt:
                            accumulate(in.a, guarded(a, b_.mul(a, b_.div(b_.constant(0.5), V))));
                            break;
                        case Op::Abs:
                            accumulate(in.a, b_.select(b_.value(Op::Ge, A, zero()), a, b_.neg(a)));
                            break;
                        case Op::Pow:
                            if (wants(in.a))
                                accumulate(in.a, guarded(a, b_.mul(a, b_.div(b_.mul(B, V), A))));
                            if (wants(in.b))
                                accumulate(in.b, guarded(a, b_.mul(a, b_.mul(V, b_.value(Op::Log, A)))));
                            break;
                        case Op::Smooth:
                        {
                            const std::int32_t den = b_.add(B, B), q = b_.div(b_.add(A, B), den);
                            const std::int32_t in_ramp = b_.value(Op::And, b_.value(Op::Gt, B, zero()), interior(q));
                            const std::int32_t a_num = b_.div(a, den);
                            if (wants(in.a))
                                accumulate(in.a, b_.select(in_ramp, a_num, zero()));
                            if (wants(in.b))
                            {
                                const std::int32_t a_den = b_.neg(b_.mul(a, b_.div(q, den)));
                                accumulate(in.b, b_.select(in_ramp, b_.add(a_num, b_.add(a_den, a_den)), zero()));
                            }
                            break;
                        }
                        case Op::Select:
                            if (wants(in.b))
                                accumulate(in.b, b_.select(A, a, zero()));
                            if (wants(in.c))
                                accumulate(in.c, b_.select(A, zero(), a));
                            break;
                        case Op::Up:
                            accumulate(in.a, b_.select(interior(V), b_.div(a, b_.constant(2.0 * in.x)), zero()));
                            break;
                        case Op::Tent:
                        {
                            const std::int32_t s = b_.neg(b_.div(a, b_.constant(in.x)));
                            accumulate(in.a, b_.select(interior(V),
                                                       b_.select(b_.value(Op::Ge, A, zero()), s, b_.neg(s)), zero()));
                            break;
                        }
                        case Op::GridVol:
                            accumulate(in.a, b_.mul(a, b_.value(Op::GridSlope, A, B)));
                            b_.raw(Op::GridAccum, a, A, B);
                            break;
                        case Op::GridLev:
                            accumulate(in.a, b_.mul(a, b_.value(Op::GridLevSlope, A, B)));
                            b_.raw(Op::GridLevAccum, a, A, B);
                            break;
                        default: // nothing flows into constants, data, logic
                            break;
                    }
                }
            }

            const Block &src_;
            Rebuilder r_;
            Builder &b_;
            std::int32_t n_;
            std::int32_t n_risks_;
            std::int32_t next_ = 0;
            std::map<std::int32_t, std::int32_t> slot_, theta_;         ///< state slot / parameter -> its entry after the values
            std::vector<std::int32_t> adj_;                             ///< per value, then per slot and parameter: its adjoint so far
            std::map<std::int32_t, std::vector<std::int32_t>> pending_; ///< per region: its phis with an adjoint
        };
    } // namespace

    Block adjoint(const Block &b, std::int32_t n_risks)
    {
        return Reverse(b, n_risks).run();
    }

    PathProgram tangent(const PathProgram &p, const std::vector<std::int32_t> &dirs)
    {
        for (std::int32_t d : dirs)
            if (d < 0 || d >= p.layout.n_risks)
                throw InvalidInput("script IR: a tangent direction is one of the model's scalar parameters (the "
                                   "grid's points take the adjoint)");
        PathProgram out;
        out.layout = p.layout;
        out.init = tangent(p.init, dirs);
        out.step = tangent(p.step, dirs);
        for (const Block &e : p.events)
            out.events.push_back(tangent(e, dirs));
        return out;
    }

    PathProgram adjoint(const PathProgram &p)
    {
        PathProgram out;
        out.layout = p.layout;
        out.init = adjoint(p.init, p.layout.n_risks);
        out.step = adjoint(p.step, p.layout.n_risks);
        for (const Block &e : p.events)
            out.events.push_back(adjoint(e, p.layout.n_risks));
        return out;
    }

    // ── paths on the host ──────────────────────────────────────────────────

    namespace
    {
        std::size_t longest_block(const PathProgram &p)
        {
            std::size_t n = std::max(p.init.code.size(), p.step.code.size());
            for (const Block &b : p.events)
                n = std::max(n, b.code.size());
            return std::max<std::size_t>(n, 1);
        }

        Frame frame_of(const PathLayout &l, const PathData &data, const DeviceModel &model)
        {
            Frame f;
            f.theta = data.theta.data();
            f.grid_K = model.K.data();
            f.grid_nK = static_cast<int>(model.K.size());
            f.grid_T = model.T_grid.data();
            f.grid_nT = static_cast<int>(model.T_grid.size());
            f.grid0 = l.th_grid;
            return f;
        }
    } // namespace

    double run_path_tangent(const PathProgram &p, int K, const PathData &data, const DeviceModel &model,
                            const Script &script, mc::PathDraws &draws, double sign, DerivativeScratch &w,
                            double *derivatives)
    {
        const PathLayout &l = p.layout;
        const auto ns = static_cast<std::size_t>(l.n_state());
        w.state.assign(ns, 0.0);
        w.tangent.assign(ns * static_cast<std::size_t>(K), 0.0);
        w.val.resize(longest_block(p));
        double z[mc::ScriptLimits::kAssets] = {};
        Frame f = frame_of(l, data, model);
        f.state = w.state.data();
        f.state_tangent = w.tangent.data();
        f.directions = K;
        f.draws = z;
        f.data = data.baseline.data();
        run_block(p.init, f, w.val.data());
        int d = 0;
        for (std::size_t s = 0; s < model.t.size(); ++s)
        {
            if (model.draws[s])
            {
                for (int k = 0; k < l.factors; ++k)
                    z[k] = draws.draw(d, k, sign);
                f.data = data.step_rows.data() + s * static_cast<std::size_t>(l.step_cols);
                run_block(p.step, f, w.val.data());
                ++d;
            }
            const int e = model.event[s];
            if (e < 0)
                continue;
            f.data = data.event_rows.data() + static_cast<std::size_t>(e) * static_cast<std::size_t>(l.event_cols);
            f.terms = data.terms.data() + data.term_begin[static_cast<std::size_t>(e)];
            run_block(p.events[static_cast<std::size_t>(script.event_block[static_cast<std::size_t>(e)])], f,
                      w.val.data());
        }
        for (int k = 0; k < K; ++k)
            derivatives[k] = w.tangent[static_cast<std::size_t>(l.pay()) * static_cast<std::size_t>(K) +
                                       static_cast<std::size_t>(k)];
        return w.state[static_cast<std::size_t>(l.pay())];
    }

    double run_path_adjoint(const PathProgram &primal, const PathProgram &adj, const PathData &data,
                            const DeviceModel &model, const Script &script, mc::PathDraws &draws, double sign,
                            double weight, DerivativeScratch &w, double *gradient, long stride)
    {
        const PathLayout &l = primal.layout;
        const auto ns = static_cast<std::size_t>(l.n_state());
        const auto nf = static_cast<std::size_t>(l.factors);
        w.state.assign(ns, 0.0);
        w.val.resize(std::max(longest_block(primal), longest_block(adj)));
        w.checkpoints.clear();
        w.draws.clear();
        w.record.clear();
        double z[mc::ScriptLimits::kAssets] = {};
        Frame f = frame_of(l, data, model);
        f.state = w.state.data();
        f.draws = z;

        // Forward: the state before each block, the block, its draws.
        const auto checkpoint = [&]
        { w.checkpoints.insert(w.checkpoints.end(), w.state.begin(), w.state.end()); };
        f.data = data.baseline.data();
        run_block(primal.init, f, w.val.data());
        int d = 0;
        for (std::size_t s = 0; s < model.t.size(); ++s)
        {
            if (model.draws[s])
            {
                for (std::size_t k = 0; k < nf; ++k)
                    z[k] = draws.draw(d, static_cast<int>(k), sign);
                w.draws.insert(w.draws.end(), z, z + nf);
                checkpoint();
                w.record.push_back(-1 - static_cast<std::int32_t>(s)); // a step
                f.data = data.step_rows.data() + s * static_cast<std::size_t>(l.step_cols);
                run_block(primal.step, f, w.val.data());
                ++d;
            }
            const int e = model.event[s];
            if (e < 0)
                continue;
            checkpoint();
            w.record.push_back(e);
            f.data = data.event_rows.data() + static_cast<std::size_t>(e) * static_cast<std::size_t>(l.event_cols);
            f.terms = data.terms.data() + data.term_begin[static_cast<std::size_t>(e)];
            run_block(primal.events[static_cast<std::size_t>(script.event_block[static_cast<std::size_t>(e)])], f,
                      w.val.data());
        }
        const double payoff = w.state[static_cast<std::size_t>(l.pay())];

        // Backward: each block's adjoint on the state it started from.
        w.adjoint.assign(ns, 0.0);
        w.adjoint[static_cast<std::size_t>(l.pay())] = weight;
        f.state_adjoint = w.adjoint.data();
        f.theta_adjoint = gradient;
        f.theta_stride = stride;
        for (std::size_t i = w.record.size(); i-- > 0;)
        {
            f.state = w.checkpoints.data() + i * ns;
            const std::int32_t rec = w.record[i];
            if (rec < 0)
            {
                --d;
                const auto s = static_cast<std::size_t>(-1 - rec);
                f.draws = w.draws.data() + static_cast<std::size_t>(d) * nf;
                f.data = data.step_rows.data() + s * static_cast<std::size_t>(l.step_cols);
                run_block(adj.step, f, w.val.data());
            }
            else
            {
                const auto e = static_cast<std::size_t>(rec);
                f.data = data.event_rows.data() + e * static_cast<std::size_t>(l.event_cols);
                f.terms = data.terms.data() + data.term_begin[e];
                run_block(adj.events[static_cast<std::size_t>(script.event_block[e])], f, w.val.data());
            }
        }
        f.state = w.state.data(); // the init block reads no state
        f.data = data.baseline.data();
        run_block(adj.init, f, w.val.data());
        return payoff;
    }

} // namespace quantModeling::scripting::ir
