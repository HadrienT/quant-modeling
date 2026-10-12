"""The weight file read by the C++ core (`quantModeling::ml::Mlp`)."""

from pathlib import Path

import pytest
import torch

from qmml import export, make_fixture

DATA = Path(__file__).resolve().parents[2] / "tests" / "data"


def test_round_trip_keeps_every_bit(tmp_path):
    net = make_fixture.reference_network()
    path = tmp_path / "net.mlp"
    export.save(net, path)
    back = export.load(path)
    x = make_fixture.reference_inputs()
    y, jacobian = net.value_and_jacobian(x)
    y_back, jacobian_back = back.value_and_jacobian(x)
    assert torch.equal(y, y_back)
    assert torch.equal(jacobian, jacobian_back)


def test_committed_fixture_agrees_with_pytorch():
    """tests/testMlp.cpp compares the C++ evaluation with these same files."""
    net = export.load(DATA / make_fixture.WEIGHTS)
    tokens = (DATA / make_fixture.CASES).read_text().split()
    count, n, m = int(tokens[1]), int(tokens[3]), int(tokens[5])
    numbers = torch.tensor([float(t) for t in tokens[6:]], dtype=torch.float64)
    rows = numbers.reshape(count, n + m + m * n)
    x = rows[:, :n]
    y, jacobian = net.value_and_jacobian(x)
    assert torch.allclose(y, rows[:, n : n + m], rtol=1e-13, atol=0)
    assert torch.allclose(
        jacobian.reshape(count, -1), rows[:, n + m :], rtol=1e-12, atol=1e-15
    )


def test_malformed_file_is_refused():
    with pytest.raises(ValueError):
        export.loads("qm-mlp 2\n")
    with pytest.raises(ValueError):
        export.loads("not-a-network 1\n")
