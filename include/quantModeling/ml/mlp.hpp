#ifndef QM_ML_MLP_HPP
#define QM_ML_MLP_HPP

#include "quantModeling/core/types.hpp"

#include <cmath>
#include <cstddef>
#include <iosfwd>
#include <string>
#include <vector>

namespace quantModeling::ml
{

    /**
     * @file mlp.hpp
     * @brief Evaluation of a small dense network trained elsewhere
     *        (blueprint/wp/25-ml.md, ADR-M1, lot M0).
     *
     * Networks are trained in Python with PyTorch (`ml/qmml/`) and written to
     * a text file; this class reads the file and evaluates the network, with
     * its derivatives with respect to its inputs. Nothing here trains.
     *
     * The network, with the scalings stored in the file:
     *
     *     h_0 = (x - input_mean) / input_scale
     *     h_l = g(W_l h_{l-1} + b_l)            for the hidden layers
     *     y   = output_mean + output_scale * (W_L h_{L-1} + b_L)
     *
     * W_l has one row per output of the layer, as in `torch.nn.Linear`.
     */

    enum class Activation
    {
        Softplus,
    };

    /// log(1 + e^x), without overflow for large |x|.
    inline double softplus(double x)
    {
        return x > 0.0 ? x + std::log1p(std::exp(-x)) : std::log1p(std::exp(x));
    }

    /// The same function on any number type with exp and log: what lets the
    /// tape differentiate the network (the oracle of value_and_jacobian).
    template <class T>
    T softplus(const T &x)
    {
        using std::exp;
        using std::log;
        const T e = exp(x);
        const T one_plus = e + 1.0;
        const T result = log(one_plus);
        return result;
    }

    /// The derivative of softplus: the logistic function.
    inline double softplus_derivative(double x)
    {
        return x > 0.0 ? 1.0 / (1.0 + std::exp(-x)) : std::exp(x) / (1.0 + std::exp(x));
    }

    struct Layer
    {
        std::size_t inputs = 0;
        std::size_t outputs = 0;
        /// Row by row: the weight from input j to output i is at `i * inputs + j`.
        std::vector<Real> weights;
        std::vector<Real> bias;
    };

    class Mlp
    {
      public:
        /// Buffers of one evaluation. One per thread: with it, value() and
        /// value_and_jacobian() do not allocate.
        struct Workspace
        {
            std::vector<Real> a, b;
            std::vector<Real> jacobian_a, jacobian_b;
        };

        Mlp() = default;

        /// @throws InvalidInput when the layers do not chain or a scaling has
        ///         the wrong size or a zero scale.
        Mlp(std::vector<Layer> layers, Activation activation, std::vector<Real> input_mean,
            std::vector<Real> input_scale, std::vector<Real> output_mean, std::vector<Real> output_scale);

        /// Reads the text format written by `qmml.export.save`.
        /// @throws InvalidInput on a malformed file.
        static Mlp read(std::istream &in);
        static Mlp load(const std::string &path);

        bool empty() const { return layers_.empty(); }
        std::size_t inputs() const { return layers_.empty() ? 0 : layers_.front().inputs; }
        std::size_t outputs() const { return layers_.empty() ? 0 : layers_.back().outputs; }
        Activation activation() const { return activation_; }
        const std::vector<Layer> &layers() const { return layers_; }

        Workspace workspace() const;

        /// `x` holds inputs() numbers, `y` receives outputs() numbers.
        void value(const Real *x, Real *y, Workspace &ws) const;

        /// As value(), and the derivatives: the derivative of output i with
        /// respect to input j goes to `jacobian[i * inputs() + j]`.
        void value_and_jacobian(const Real *x, Real *y, Real *jacobian, Workspace &ws) const;

        /// The network on any number type, allocating: for tests against the
        /// tape, not for the hot path.
        template <class T>
        std::vector<T> evaluate(const std::vector<T> &x) const
        {
            std::vector<T> h;
            h.reserve(x.size());
            for (std::size_t j = 0; j < x.size(); ++j)
            {
                const T centred = x[j] - input_mean_[j];
                const T scaled = centred / input_scale_[j];
                h.push_back(scaled);
            }
            for (std::size_t l = 0; l < layers_.size(); ++l)
            {
                const Layer &layer = layers_[l];
                const bool hidden = l + 1 < layers_.size();
                std::vector<T> next;
                next.reserve(layer.outputs);
                for (std::size_t i = 0; i < layer.outputs; ++i)
                {
                    T sum(layer.bias[i]);
                    for (std::size_t j = 0; j < layer.inputs; ++j)
                    {
                        const T term = h[j] * layer.weights[i * layer.inputs + j];
                        const T updated = sum + term;
                        sum = updated;
                    }
                    if (hidden)
                    {
                        const T activated = softplus(sum);
                        next.push_back(activated);
                    }
                    else
                    {
                        const T scaled = sum * output_scale_[i];
                        const T shifted = scaled + output_mean_[i];
                        next.push_back(shifted);
                    }
                }
                h = std::move(next);
            }
            return h;
        }

      private:
        std::vector<Layer> layers_;
        Activation activation_ = Activation::Softplus;
        std::vector<Real> input_mean_, input_scale_, output_mean_, output_scale_;
        std::size_t width_ = 0;

        void forward(const Real *x, Real *y, Real *jacobian, Workspace &ws) const;
    };

} // namespace quantModeling::ml

#endif
