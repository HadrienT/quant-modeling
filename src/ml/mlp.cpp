#include "quantModeling/ml/mlp.hpp"

#include <algorithm>
#include <fstream>
#include <istream>
#include <utility>

namespace quantModeling::ml
{
    namespace
    {
        constexpr const char *kMagic = "qm-mlp";
        constexpr int kVersion = 1;
        /// A malformed file must not make us allocate gigabytes.
        constexpr std::size_t kMaxWidth = 4096;
        constexpr std::size_t kMaxLayers = 64;

        void expect(std::istream &in, const std::string &word)
        {
            std::string read;
            if (!(in >> read) || read != word)
                throw InvalidInput("mlp file: expected '" + word + "', read '" + read + "'");
        }

        std::size_t read_size(std::istream &in, const std::string &what, std::size_t max)
        {
            long long n = 0;
            if (!(in >> n) || n <= 0 || static_cast<unsigned long long>(n) > max)
                throw InvalidInput("mlp file: " + what + " must be between 1 and " + std::to_string(max));
            return static_cast<std::size_t>(n);
        }

        std::vector<Real> read_numbers(std::istream &in, std::size_t count, const std::string &what)
        {
            std::vector<Real> out(count);
            for (Real &v : out)
                if (!(in >> v) || !std::isfinite(v))
                    throw InvalidInput("mlp file: " + what + " holds a missing or non-finite number");
            return out;
        }
    } // namespace

    Mlp::Mlp(std::vector<Layer> layers, Activation activation, std::vector<Real> input_mean,
             std::vector<Real> input_scale, std::vector<Real> output_mean, std::vector<Real> output_scale)
        : layers_(std::move(layers)), activation_(activation), input_mean_(std::move(input_mean)), input_scale_(std::move(input_scale)), output_mean_(std::move(output_mean)), output_scale_(std::move(output_scale))
    {
        if (layers_.empty())
            throw InvalidInput("mlp: a network needs at least one layer");
        for (std::size_t l = 0; l < layers_.size(); ++l)
        {
            const Layer &layer = layers_[l];
            if (layer.inputs == 0 || layer.outputs == 0)
                throw InvalidInput("mlp: a layer has no input or no output");
            if (layer.weights.size() != layer.inputs * layer.outputs || layer.bias.size() != layer.outputs)
                throw InvalidInput("mlp: a layer's weights or bias do not match its size");
            if (l > 0 && layer.inputs != layers_[l - 1].outputs)
                throw InvalidInput("mlp: a layer does not take as many inputs as the previous one gives");
            width_ = std::max({width_, layer.inputs, layer.outputs});
        }
        if (input_mean_.size() != inputs() || input_scale_.size() != inputs())
            throw InvalidInput("mlp: the input scaling does not have one entry per input");
        if (output_mean_.size() != outputs() || output_scale_.size() != outputs())
            throw InvalidInput("mlp: the output scaling does not have one entry per output");
        for (Real s : input_scale_)
            if (!(s > 0.0))
                throw InvalidInput("mlp: an input scale must be positive");
        for (Real s : output_scale_)
            if (!(s > 0.0))
                throw InvalidInput("mlp: an output scale must be positive");
    }

    Mlp Mlp::read(std::istream &in)
    {
        expect(in, kMagic);
        int version = 0;
        if (!(in >> version) || version != kVersion)
            throw InvalidInput("mlp file: unknown version");

        expect(in, "activation");
        std::string activation;
        in >> activation;
        if (activation != "softplus")
            throw InvalidInput("mlp file: unknown activation '" + activation + "'");

        expect(in, "inputs");
        const std::size_t n_in = read_size(in, "the number of inputs", kMaxWidth);
        expect(in, "outputs");
        const std::size_t n_out = read_size(in, "the number of outputs", kMaxWidth);

        expect(in, "input_mean");
        std::vector<Real> input_mean = read_numbers(in, n_in, "input_mean");
        expect(in, "input_scale");
        std::vector<Real> input_scale = read_numbers(in, n_in, "input_scale");
        expect(in, "output_mean");
        std::vector<Real> output_mean = read_numbers(in, n_out, "output_mean");
        expect(in, "output_scale");
        std::vector<Real> output_scale = read_numbers(in, n_out, "output_scale");

        expect(in, "layers");
        const std::size_t count = read_size(in, "the number of layers", kMaxLayers);
        std::vector<Layer> layers(count);
        for (Layer &layer : layers)
        {
            expect(in, "layer");
            layer.outputs = read_size(in, "a layer's number of outputs", kMaxWidth);
            layer.inputs = read_size(in, "a layer's number of inputs", kMaxWidth);
            layer.weights = read_numbers(in, layer.inputs * layer.outputs, "a layer's weights");
            layer.bias = read_numbers(in, layer.outputs, "a layer's bias");
        }
        return Mlp(std::move(layers), Activation::Softplus, std::move(input_mean), std::move(input_scale),
                   std::move(output_mean), std::move(output_scale));
    }

    Mlp Mlp::load(const std::string &path)
    {
        std::ifstream in(path);
        if (!in)
            throw InvalidInput("mlp file: cannot open '" + path + "'");
        return read(in);
    }

    Mlp::Workspace Mlp::workspace() const
    {
        Workspace ws;
        ws.a.resize(width_);
        ws.b.resize(width_);
        ws.jacobian_a.resize(width_ * inputs());
        ws.jacobian_b.resize(width_ * inputs());
        return ws;
    }

    void Mlp::value(const Real *x, Real *y, Workspace &ws) const
    {
        forward(x, y, nullptr, ws);
    }

    void Mlp::value_and_jacobian(const Real *x, Real *y, Real *jacobian, Workspace &ws) const
    {
        forward(x, y, jacobian, ws);
    }

    void Mlp::forward(const Real *x, Real *y, Real *jacobian, Workspace &ws) const
    {
        const std::size_t n = inputs();
        if (ws.a.size() < width_ || ws.b.size() < width_ || ws.jacobian_a.size() < width_ * n ||
            ws.jacobian_b.size() < width_ * n)
            throw InvalidInput("mlp: the workspace was not made by this network");

        // h holds the current layer's outputs, dh their derivatives with
        // respect to the inputs, one row of n numbers per output.
        Real *h = ws.a.data();
        Real *next = ws.b.data();
        Real *dh = ws.jacobian_a.data();
        Real *dnext = ws.jacobian_b.data();

        for (std::size_t j = 0; j < n; ++j)
            h[j] = (x[j] - input_mean_[j]) / input_scale_[j];
        if (jacobian)
        {
            std::fill(dh, dh + n * n, 0.0);
            for (std::size_t j = 0; j < n; ++j)
                dh[j * n + j] = 1.0 / input_scale_[j];
        }

        for (std::size_t l = 0; l < layers_.size(); ++l)
        {
            const Layer &layer = layers_[l];
            const bool hidden = l + 1 < layers_.size();
            for (std::size_t i = 0; i < layer.outputs; ++i)
            {
                const Real *w = layer.weights.data() + i * layer.inputs;
                Real z = layer.bias[i];
                for (std::size_t j = 0; j < layer.inputs; ++j)
                    z += w[j] * h[j];

                // Derivative of this output with respect to z: the slope of
                // the activation, or the output scale on the last layer.
                const Real slope = hidden ? softplus_derivative(z) : output_scale_[i];
                next[i] = hidden ? softplus(z) : output_mean_[i] + output_scale_[i] * z;

                if (jacobian)
                {
                    Real *row = dnext + i * n;
                    std::fill(row, row + n, 0.0);
                    for (std::size_t j = 0; j < layer.inputs; ++j)
                    {
                        const Real wj = w[j];
                        const Real *source = dh + j * n;
                        for (std::size_t k = 0; k < n; ++k)
                            row[k] += wj * source[k];
                    }
                    for (std::size_t k = 0; k < n; ++k)
                        row[k] *= slope;
                }
            }
            std::swap(h, next);
            std::swap(dh, dnext);
        }

        const std::size_t m = outputs();
        std::copy(h, h + m, y);
        if (jacobian)
            std::copy(dh, dh + m * n, jacobian);
    }

} // namespace quantModeling::ml
