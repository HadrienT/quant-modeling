#include <gtest/gtest.h>

#include "quantModeling/aad/number.hpp"
#include "quantModeling/ml/mlp.hpp"

#include <cmath>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

// Lot M0 of blueprint/wp/25-ml.md: the network is trained in Python and
// evaluated here. The fixture in tests/data is written by
// `python -m qmml.make_fixture tests/data`.

namespace quantModeling::ml
{
    namespace
    {
        const std::string kData = QM_TEST_DATA_DIR;

        struct Case
        {
            std::vector<Real> x, y, jacobian;
        };

        std::vector<Case> read_cases(const Mlp &net)
        {
            std::ifstream in(kData + "/mlp_reference.cases");
            std::string word;
            std::size_t count = 0, n = 0, m = 0;
            in >> word >> count >> word >> n >> word >> m;
            EXPECT_EQ(n, net.inputs());
            EXPECT_EQ(m, net.outputs());
            std::vector<Case> cases(count);
            for (Case &c : cases)
            {
                c.x.resize(n);
                c.y.resize(m);
                c.jacobian.resize(m * n);
                for (Real &v : c.x)
                    in >> v;
                for (Real &v : c.y)
                    in >> v;
                for (Real &v : c.jacobian)
                    in >> v;
            }
            EXPECT_TRUE(static_cast<bool>(in));
            return cases;
        }

        /// |a - b| relative to the larger of the two, or to 1.
        Real relative_gap(Real a, Real b)
        {
            return std::abs(a - b) / std::max({1.0, std::abs(a), std::abs(b)});
        }

        const char *kSmall = "qm-mlp 1\nactivation softplus\ninputs 2\noutputs 1\n"
                             "input_mean 0 0\ninput_scale 1 1\noutput_mean 0\noutput_scale 1\n"
                             "layers 2\nlayer 2 2\n1 0 0 1\n0 0\nlayer 1 2\n1 1\n0\n";
    } // namespace

    TEST(Mlp, ValuesAndJacobianEqualPyTorchOnTheFixture)
    {
        const Mlp net = Mlp::load(kData + "/mlp_reference.mlp");
        Mlp::Workspace ws = net.workspace();
        const std::vector<Case> cases = read_cases(net);
        ASSERT_FALSE(cases.empty());
        for (const Case &c : cases)
        {
            std::vector<Real> y(net.outputs()), jacobian(net.outputs() * net.inputs());
            net.value_and_jacobian(c.x.data(), y.data(), jacobian.data(), ws);
            for (std::size_t i = 0; i < y.size(); ++i)
                EXPECT_LT(relative_gap(y[i], c.y[i]), 1e-13);
            for (std::size_t k = 0; k < jacobian.size(); ++k)
                EXPECT_LT(relative_gap(jacobian[k], c.jacobian[k]), 1e-13);

            // value() alone gives the same bits as value_and_jacobian().
            std::vector<Real> alone(net.outputs());
            net.value(c.x.data(), alone.data(), ws);
            EXPECT_EQ(alone, y);
        }
    }

    TEST(Mlp, JacobianEqualsTheTape)
    {
        const Mlp net = Mlp::load(kData + "/mlp_reference.mlp");
        Mlp::Workspace ws = net.workspace();
        for (const Case &c : read_cases(net))
        {
            std::vector<Real> y(net.outputs()), jacobian(net.outputs() * net.inputs());
            net.value_and_jacobian(c.x.data(), y.data(), jacobian.data(), ws);
            for (std::size_t i = 0; i < net.outputs(); ++i)
            {
                aad::Number::tape->clear();
                std::vector<aad::Number> x;
                for (Real v : c.x)
                    x.emplace_back(v);
                std::vector<aad::Number> out = net.evaluate(x);
                EXPECT_LT(relative_gap(out[i].value(), y[i]), 1e-13);
                out[i].propagate_to_start();
                for (std::size_t j = 0; j < net.inputs(); ++j)
                    EXPECT_LT(relative_gap(x[j].adjoint(), jacobian[i * net.inputs() + j]), 1e-13);
            }
        }
    }

    TEST(Mlp, JacobianEqualsCentredFiniteDifferences)
    {
        const Mlp net = Mlp::load(kData + "/mlp_reference.mlp");
        Mlp::Workspace ws = net.workspace();
        const std::size_t n = net.inputs(), m = net.outputs();
        for (const Case &c : read_cases(net))
        {
            std::vector<Real> y(m), jacobian(m * n), up(m), down(m);
            net.value_and_jacobian(c.x.data(), y.data(), jacobian.data(), ws);
            for (std::size_t j = 0; j < n; ++j)
            {
                const Real h = 1e-5 * std::max(1.0, std::abs(c.x[j]));
                std::vector<Real> x = c.x;
                x[j] = c.x[j] + h;
                net.value(x.data(), up.data(), ws);
                x[j] = c.x[j] - h;
                net.value(x.data(), down.data(), ws);
                for (std::size_t i = 0; i < m; ++i)
                    EXPECT_NEAR((up[i] - down[i]) / (2 * h), jacobian[i * n + j],
                                1e-7 * std::max(1.0, std::abs(jacobian[i * n + j])));
            }
        }
    }

    TEST(Mlp, SmallNetworkByHand)
    {
        // y = softplus(x0) + softplus(x1), so dy/dxj = logistic(xj).
        std::istringstream in(kSmall);
        const Mlp net = Mlp::read(in);
        Mlp::Workspace ws = net.workspace();
        const Real x[2] = {0.3, -1.2};
        Real y = 0.0, d[2] = {0.0, 0.0};
        net.value_and_jacobian(x, &y, d, ws);
        EXPECT_NEAR(y, std::log1p(std::exp(0.3)) + std::log1p(std::exp(-1.2)), 1e-15);
        EXPECT_NEAR(d[0], 1.0 / (1.0 + std::exp(-0.3)), 1e-15);
        EXPECT_NEAR(d[1], 1.0 / (1.0 + std::exp(1.2)), 1e-15);
    }

    TEST(Mlp, SoftplusHasNoOverflowAndItsLimits)
    {
        EXPECT_DOUBLE_EQ(softplus(800.0), 800.0);
        EXPECT_NEAR(softplus(-800.0), 0.0, 1e-300);
        EXPECT_DOUBLE_EQ(softplus(0.0), std::log(2.0));
        EXPECT_DOUBLE_EQ(softplus_derivative(800.0), 1.0);
        EXPECT_DOUBLE_EQ(softplus_derivative(0.0), 0.5);
        EXPECT_NEAR(softplus_derivative(-800.0), 0.0, 1e-300);
        // The stable form and the plain one agree where the plain one is safe.
        for (double x : {-20.0, -1.0, 0.5, 20.0})
            EXPECT_NEAR(softplus(x), std::log(1.0 + std::exp(x)), 1e-15 * std::max(1.0, x));
    }

    TEST(Mlp, MalformedFilesAreRefused)
    {
        const std::string good = kSmall;
        const auto read = [](const std::string &text)
        {
            std::istringstream in(text);
            return Mlp::read(in);
        };
        EXPECT_NO_THROW(read(good));
        EXPECT_THROW(read("not-a-network 1\n"), InvalidInput);
        EXPECT_THROW(read("qm-mlp 2\n"), InvalidInput);
        // Truncated: the last bias is missing.
        EXPECT_THROW(read(good.substr(0, good.size() - 2)), InvalidInput);

        std::string wrong_activation = good;
        wrong_activation.replace(wrong_activation.find("softplus"), 8, "relu");
        EXPECT_THROW(read(wrong_activation), InvalidInput);

        std::string zero_scale = good;
        zero_scale.replace(zero_scale.find("input_scale 1 1"), 15, "input_scale 1 0");
        EXPECT_THROW(read(zero_scale), InvalidInput);

        // A second layer that does not take what the first one gives.
        std::string broken_chain = good;
        broken_chain.replace(broken_chain.find("layer 1 2\n1 1"), 13, "layer 1 3\n1 1 1");
        EXPECT_THROW(read(broken_chain), InvalidInput);

        EXPECT_THROW(Mlp::load(kData + "/no_such_file.mlp"), InvalidInput);
    }

    TEST(Mlp, AWorkspaceFromAnotherNetworkIsRefused)
    {
        std::istringstream in(kSmall);
        const Mlp small = Mlp::read(in);
        const Mlp large = Mlp::load(kData + "/mlp_reference.mlp");
        Mlp::Workspace ws = small.workspace();
        std::vector<Real> x(large.inputs(), 0.0), y(large.outputs());
        EXPECT_THROW(large.value(x.data(), y.data(), ws), InvalidInput);
    }

} // namespace quantModeling::ml
