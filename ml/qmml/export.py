"""Weight files shared with the C++ core (`quantModeling::ml::Mlp::read`).

A text file, one token per number, written with `repr` so that a double is
read back to the last bit:

    qm-mlp 1
    activation softplus
    inputs <n>
    outputs <m>
    input_mean <n numbers>
    input_scale <n numbers>
    output_mean <m numbers>
    output_scale <m numbers>
    layers <L>
    layer <outputs> <inputs>
    <weights, row by row: one row per output>
    <biases>
    ...
"""

from __future__ import annotations

from pathlib import Path

import torch

from .twin import DTYPE, TwinNetwork

MAGIC = "qm-mlp"
VERSION = 1


def _numbers(values: torch.Tensor) -> str:
    return " ".join(repr(float(v)) for v in values.reshape(-1).tolist())


def dumps(net: TwinNetwork) -> str:
    lines = [
        f"{MAGIC} {VERSION}",
        "activation softplus",
        f"inputs {net.inputs}",
        f"outputs {net.outputs}",
        "input_mean " + _numbers(net.input_mean),
        "input_scale " + _numbers(net.input_scale),
        "output_mean " + _numbers(net.output_mean),
        "output_scale " + _numbers(net.output_scale),
        f"layers {len(net.linears)}",
    ]
    for linear in net.linears:
        lines.append(f"layer {linear.out_features} {linear.in_features}")
        lines.append(_numbers(linear.weight.detach()))
        lines.append(_numbers(linear.bias.detach()))
    return "\n".join(lines) + "\n"


def save(net: TwinNetwork, path: str | Path) -> None:
    Path(path).write_text(dumps(net))


class _Tokens:
    def __init__(self, text: str):
        self._tokens = text.split()
        self._at = 0

    def word(self) -> str:
        if self._at >= len(self._tokens):
            raise ValueError("mlp file: unexpected end")
        self._at += 1
        return self._tokens[self._at - 1]

    def expect(self, word: str) -> None:
        read = self.word()
        if read != word:
            raise ValueError(f"mlp file: expected '{word}', read '{read}'")

    def integer(self) -> int:
        return int(self.word())

    def numbers(self, count: int) -> torch.Tensor:
        return torch.tensor([float(self.word()) for _ in range(count)], dtype=DTYPE)


def loads(text: str) -> TwinNetwork:
    tokens = _Tokens(text)
    tokens.expect(MAGIC)
    if tokens.integer() != VERSION:
        raise ValueError("mlp file: unknown version")
    tokens.expect("activation")
    tokens.expect("softplus")
    tokens.expect("inputs")
    inputs = tokens.integer()
    tokens.expect("outputs")
    outputs = tokens.integer()
    scalings = {}
    for name, count in (
        ("input_mean", inputs),
        ("input_scale", inputs),
        ("output_mean", outputs),
        ("output_scale", outputs),
    ):
        tokens.expect(name)
        scalings[name] = tokens.numbers(count)
    tokens.expect("layers")
    layers = []
    for _ in range(tokens.integer()):
        tokens.expect("layer")
        out_features = tokens.integer()
        in_features = tokens.integer()
        weight = tokens.numbers(out_features * in_features).reshape(
            out_features, in_features
        )
        layers.append((weight, tokens.numbers(out_features)))

    hidden = [weight.shape[0] for weight, _ in layers[:-1]]
    if len(set(hidden)) > 1:
        raise ValueError(
            "mlp file: hidden layers of different widths are not supported here"
        )
    net = TwinNetwork(inputs, outputs, hidden[0] if hidden else 1, len(hidden))
    with torch.no_grad():
        for linear, (weight, bias) in zip(net.linears, layers, strict=True):
            linear.weight.copy_(weight)
            linear.bias.copy_(bias)
        for name, values in scalings.items():
            getattr(net, name).copy_(values)
    return net


def load(path: str | Path) -> TwinNetwork:
    return loads(Path(path).read_text())
