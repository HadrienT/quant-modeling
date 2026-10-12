"""Writes the fixture that ties the Python and C++ evaluations together.

    python -m qmml.make_fixture tests/data

`mlp_reference.mlp` is a network with three inputs, two outputs and scalings
that are not trivial; `mlp_reference.cases` holds inputs with the values and
derivatives PyTorch computes for them. `tests/testMlp.cpp` reads both and
must find the same numbers; `ml/tests/test_export.py` checks that the
committed files still agree with PyTorch.
"""

from __future__ import annotations

import sys
from pathlib import Path

import torch

from .export import _numbers, save
from .twin import DTYPE, TwinNetwork

WEIGHTS = "mlp_reference.mlp"
CASES = "mlp_reference.cases"


def reference_network() -> TwinNetwork:
    generator = torch.Generator().manual_seed(20261012)
    net = TwinNetwork(inputs=3, outputs=2, hidden_units=5, hidden_layers=2)
    net.initialise(generator)
    with torch.no_grad():
        for linear in net.linears:
            linear.bias.copy_(
                torch.randn(linear.bias.shape, generator=generator, dtype=DTYPE) * 0.3
            )
        net.input_mean.copy_(torch.tensor([0.5, -1.0, 100.0], dtype=DTYPE))
        net.input_scale.copy_(torch.tensor([2.0, 0.25, 40.0], dtype=DTYPE))
        net.output_mean.copy_(torch.tensor([3.0, -0.5], dtype=DTYPE))
        net.output_scale.copy_(torch.tensor([10.0, 0.1], dtype=DTYPE))
    return net


def reference_inputs() -> torch.Tensor:
    generator = torch.Generator().manual_seed(7)
    centre = torch.tensor([0.5, -1.0, 100.0], dtype=DTYPE)
    spread = torch.tensor([4.0, 0.5, 80.0], dtype=DTYPE)
    return centre + spread * torch.randn(8, 3, generator=generator, dtype=DTYPE)


def cases_text(net: TwinNetwork, x: torch.Tensor) -> str:
    y, jacobian = net.value_and_jacobian(x)
    lines = [f"cases {x.shape[0]} inputs {net.inputs} outputs {net.outputs}"]
    for row in range(x.shape[0]):
        lines.append(_numbers(x[row]))
        lines.append(_numbers(y[row]))
        lines.append(_numbers(jacobian[row]))
    return "\n".join(lines) + "\n"


def main(directory: str) -> None:
    target = Path(directory)
    net = reference_network()
    save(net, target / WEIGHTS)
    (target / CASES).write_text(cases_text(net, reference_inputs()))


if __name__ == "__main__":
    main(sys.argv[1])
