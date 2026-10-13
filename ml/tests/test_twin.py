"""Lot M0 of blueprint/wp/25-ml.md: the twin network, on cases with a known answer."""

import torch

from qmml import black_scholes
from qmml.twin import DTYPE, TrainingSettings, TwinNetwork, fit


def _random_network() -> TwinNetwork:
    generator = torch.Generator().manual_seed(3)
    net = TwinNetwork(inputs=3, outputs=2, hidden_units=6, hidden_layers=3)
    net.initialise(generator)
    with torch.no_grad():
        net.input_mean.copy_(torch.tensor([1.0, -2.0, 0.3], dtype=DTYPE))
        net.input_scale.copy_(torch.tensor([0.5, 3.0, 1.5], dtype=DTYPE))
        net.output_scale.copy_(torch.tensor([4.0, 0.2], dtype=DTYPE))
    return net


def test_jacobian_matches_centred_finite_differences():
    net = _random_network()
    x = torch.randn(5, 3, generator=torch.Generator().manual_seed(1), dtype=DTYPE)
    _, jacobian = net.value_and_jacobian(x)
    h = 1e-6
    for j in range(3):
        bump = torch.zeros(3, dtype=DTYPE)
        bump[j] = h
        with torch.no_grad():
            fd = (net.value(x + bump) - net.value(x - bump)) / (2 * h)
        assert torch.allclose(jacobian[:, :, j], fd, rtol=0, atol=1e-8)


def test_same_seed_gives_the_same_weights():
    x, y, z = black_scholes.simulate(512, seed=1)
    settings = TrainingSettings(epochs=5, seed=11)
    a, b = fit(x, y, z, settings), fit(x, y, z, settings)
    for wa, wb in zip(a.parameters(), b.parameters()):
        assert torch.equal(wa, wb)
    other = fit(x, y, z, TrainingSettings(epochs=5, seed=12))
    assert not torch.equal(next(a.parameters()), next(other.parameters()))


def test_affine_function_is_learned_with_its_slope():
    generator = torch.Generator().manual_seed(5)
    x = (
        torch.randn(2048, 2, generator=generator, dtype=DTYPE)
        * torch.tensor([2.0, 0.1])
        + 3.0
    )
    slope = torch.tensor([[1.5, -40.0]], dtype=DTYPE)
    y = x @ slope.T + 7.0
    net = fit(x, y, slope.expand_as(x), TrainingSettings(epochs=60))
    value, jacobian = net.value_and_jacobian(x)
    assert (value - y).abs().max() < 5e-2 * y.std()
    assert (
        (jacobian[:, 0, :] - slope).abs().max(dim=0).values < 5e-2 * slope.abs()[0]
    ).all()


def _errors(net: TwinNetwork) -> tuple[float, float]:
    spots = torch.linspace(0.35, 1.65, 200, dtype=DTYPE).reshape(-1, 1)
    price, delta = black_scholes.price_and_delta(spots)
    value, jacobian = net.value_and_jacobian(spots)
    return (
        (value - price).pow(2).mean().sqrt().item(),
        (jacobian[:, 0, :] - delta).pow(2).mean().sqrt().item(),
    )


def test_differentials_beat_values_alone_on_black_scholes():
    """The central figure of the paper: one payoff per path, 1024 paths.

    Errors are measured against the Black-Scholes formula on spots the
    training never saw, and averaged over training sets so that the verdict
    does not hang on one draw.
    """
    plain, twin = [], []
    for seed in range(4):
        x, y, z = black_scholes.simulate(1024, seed=100 + seed)
        settings = TrainingSettings(seed=seed)
        plain.append(_errors(fit(x, y, None, settings)))
        twin.append(_errors(fit(x, y, z, settings)))

    def mean(rows, k):
        return sum(r[k] for r in rows) / len(rows)

    # Measured (blueprint/wp/25-ml.md, lot M0): price error 0.0077 against
    # 0.0187, delta error 0.040 against 0.101. The thresholds leave room for
    # another PyTorch version, not for a broken differential term.
    assert mean(twin, 0) < 0.7 * mean(plain, 0)
    assert mean(twin, 1) < 0.6 * mean(plain, 1)
    assert mean(twin, 0) < 0.012
