"""The installed quantbacktest wheel is the commit pinned in api/quantbacktest.pin.

The audit event of a backtest names the engine by this commit
(blueprint/wp/24-backtest.md): an environment running another build must fail
here, not record a wrong SHA.
"""

import os
import re
from pathlib import Path

import pytest

PIN_FILE = Path(__file__).resolve().parents[1] / "quantbacktest.pin"


def _pin() -> dict[str, str]:
    pairs = re.findall(r"^(QUANTBACKTEST_\w+)=(.+)$", PIN_FILE.read_text(), re.M)
    return dict(pairs)


def _wheel():
    # A developer without the wheel skips; CI must have it.
    if os.environ.get("CI"):
        import quantbacktest

        return quantbacktest
    return pytest.importorskip(
        "quantbacktest", reason="run scripts/install_quantbacktest.sh"
    )


def test_pin_is_a_full_commit_and_an_exact_version():
    pin = _pin()
    assert re.fullmatch(r"[0-9a-f]{40}", pin["QUANTBACKTEST_SHA"])
    assert re.fullmatch(r"\d+\.\d+\.\d+", pin["QUANTBACKTEST_VERSION"])


def test_installed_wheel_is_the_pinned_commit():
    info = _wheel().build_info()
    pin = _pin()
    assert info["git_sha"] == pin["QUANTBACKTEST_SHA"]
    assert info["version"] == pin["QUANTBACKTEST_VERSION"]
    # The pricing the hedging backtest runs on is named too (contract §4).
    assert re.fullmatch(r"[0-9a-f]{40}", info["quantmodeling_sha"])


def test_wheel_loads_next_to_quantmodeling():
    """Both extensions link quantModeling statically, at different commits, and
    live in the same process."""
    pytest.importorskip("quantmodeling")
    qb = _wheel()
    import numpy as np

    assert len(qb.fingerprint(np.arange(3.0))) == 64
