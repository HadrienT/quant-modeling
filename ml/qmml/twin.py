"""Twin network of Huge & Savine, "Differential Machine Learning" (2020).

A dense network is trained on values and, when they are given, on the
derivatives of the values with respect to the inputs (the pathwise
differentials an AAD pass produces). The derivatives of the network come from
`torch.autograd.grad(..., create_graph=True)`: the "twin" pass of the paper.

Design: blueprint/wp/25-ml.md §3. The trained network is written by
`qmml.export.save` and evaluated in C++ by `quantModeling::ml::Mlp`.
"""

from __future__ import annotations

from dataclasses import dataclass

import torch

DTYPE = torch.float64

# Learning rate against training progress, piecewise linear: the one-cycle
# schedule of the authors' notebook
# (github.com/differential-machine-learning/notebooks).
ONE_CYCLE = ((0.0, 1e-8), (0.2, 0.1), (0.6, 0.01), (0.9, 1e-6), (1.0, 1e-8))


@dataclass(frozen=True)
class TrainingSettings:
    hidden_units: int = 20
    hidden_layers: int = 4
    epochs: int = 100
    batch_size: int = 256
    # Weight of the derivatives against the values (the paper's lambda).
    differential_weight: float = 1.0
    lr_schedule: tuple[tuple[float, float], ...] = ONE_CYCLE
    seed: int = 0


class TwinNetwork(torch.nn.Module):
    """Softplus network with the scalings of its inputs and outputs.

    `forward` works on scaled inputs and gives scaled outputs; `value` and
    `value_and_jacobian` work in the units of the data.
    """

    def __init__(
        self,
        inputs: int,
        outputs: int = 1,
        hidden_units: int = 20,
        hidden_layers: int = 4,
    ):
        super().__init__()
        sizes = [inputs] + [hidden_units] * hidden_layers + [outputs]
        self.linears = torch.nn.ModuleList(
            torch.nn.Linear(a, b, dtype=DTYPE) for a, b in zip(sizes[:-1], sizes[1:])
        )
        self.register_buffer("input_mean", torch.zeros(inputs, dtype=DTYPE))
        self.register_buffer("input_scale", torch.ones(inputs, dtype=DTYPE))
        self.register_buffer("output_mean", torch.zeros(outputs, dtype=DTYPE))
        self.register_buffer("output_scale", torch.ones(outputs, dtype=DTYPE))

    @property
    def inputs(self) -> int:
        return self.linears[0].in_features

    @property
    def outputs(self) -> int:
        return self.linears[-1].out_features

    def initialise(self, generator: torch.Generator) -> None:
        """Glorot weights and zero biases, drawn from `generator`."""
        with torch.no_grad():
            for linear in self.linears:
                fan_out, fan_in = linear.weight.shape
                std = (2.0 / (fan_in + fan_out)) ** 0.5
                linear.weight.copy_(
                    torch.randn(fan_out, fan_in, generator=generator, dtype=DTYPE) * std
                )
                linear.bias.zero_()

    def forward(self, scaled: torch.Tensor) -> torch.Tensor:
        h = scaled
        for linear in self.linears[:-1]:
            h = torch.nn.functional.softplus(linear(h))
        return self.linears[-1](h)

    def value(self, x: torch.Tensor) -> torch.Tensor:
        scaled = (x - self.input_mean) / self.input_scale
        return self.output_mean + self.output_scale * self.forward(scaled)

    def value_and_jacobian(self, x: torch.Tensor) -> tuple[torch.Tensor, torch.Tensor]:
        """Values, shape (N, outputs), and derivatives, shape (N, outputs, inputs)."""
        x = x.detach().clone().requires_grad_(True)
        y = self.value(x)
        rows = [
            torch.autograd.grad(y[:, i].sum(), x, retain_graph=True)[0]
            for i in range(self.outputs)
        ]
        return y.detach(), torch.stack(rows, dim=1)


def _learning_rate(schedule: tuple[tuple[float, float], ...], progress: float) -> float:
    for (p0, lr0), (p1, lr1) in zip(schedule[:-1], schedule[1:]):
        if progress <= p1:
            return lr0 + (lr1 - lr0) * (progress - p0) / (p1 - p0)
    return schedule[-1][1]


def fit(
    x: torch.Tensor,
    y: torch.Tensor,
    dydx: torch.Tensor | None = None,
    settings: TrainingSettings = TrainingSettings(),
) -> TwinNetwork:
    """Train a network on `y` (N, 1) against `x` (N, n).

    With `dydx` (N, n), the cost also carries the derivatives, each scaled by
    its own size, with the paper's balance: 1 / (1 + lambda n) on the values
    and lambda n / (1 + lambda n) on the derivatives. Without, it is a plain
    regression: the comparison every result must carry.
    """
    x = torch.as_tensor(x, dtype=DTYPE)
    y = torch.as_tensor(y, dtype=DTYPE)
    if x.ndim != 2 or y.ndim != 2 or y.shape != (x.shape[0], 1):
        raise ValueError("fit: x must have shape (N, n) and y shape (N, 1)")
    count, n = x.shape
    differential = dydx is not None
    if differential:
        dydx = torch.as_tensor(dydx, dtype=DTYPE)
        if dydx.shape != x.shape:
            raise ValueError("fit: dydx must have the shape of x")

    generator = torch.Generator().manual_seed(settings.seed)
    net = TwinNetwork(n, 1, settings.hidden_units, settings.hidden_layers)
    net.initialise(generator)

    # A constant input or target has no scale of its own: leave it as it is.
    def scale_of(values: torch.Tensor) -> torch.Tensor:
        std = values.std(dim=0)
        return torch.where(std > 0, std, torch.ones_like(std))

    net.input_mean.copy_(x.mean(dim=0))
    net.input_scale.copy_(scale_of(x))
    net.output_mean.copy_(y.mean(dim=0))
    net.output_scale.copy_(scale_of(y))

    x_scaled = (x - net.input_mean) / net.input_scale
    y_scaled = (y - net.output_mean) / net.output_scale
    if differential:
        z_scaled = dydx * net.input_scale / net.output_scale
        size = z_scaled.pow(2).mean(dim=0).sqrt()
        z_weight = torch.where(size > 0, 1.0 / size, torch.ones_like(size))
        lam = settings.differential_weight
        alpha = 1.0 / (1.0 + lam * n)
        beta = 1.0 - alpha

    optimiser = torch.optim.Adam(net.parameters())
    batch = min(settings.batch_size, count)
    steps_per_epoch = (count + batch - 1) // batch
    total_steps = settings.epochs * steps_per_epoch
    step = 0
    for _ in range(settings.epochs):
        order = torch.randperm(count, generator=generator)
        for start in range(0, count, batch):
            rows = order[start : start + batch]
            lr = _learning_rate(settings.lr_schedule, step / total_steps)
            for group in optimiser.param_groups:
                group["lr"] = lr

            if differential:
                inputs = x_scaled[rows].clone().requires_grad_(True)
                predicted = net(inputs)
                (gradient,) = torch.autograd.grad(
                    predicted.sum(), inputs, create_graph=True
                )
                loss = (
                    alpha * (predicted - y_scaled[rows]).pow(2).mean()
                    + beta * ((gradient - z_scaled[rows]) * z_weight).pow(2).mean()
                )
            else:
                loss = (net(x_scaled[rows]) - y_scaled[rows]).pow(2).mean()

            optimiser.zero_grad()
            loss.backward()
            optimiser.step()
            step += 1
    return net
