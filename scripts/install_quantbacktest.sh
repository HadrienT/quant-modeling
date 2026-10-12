#!/usr/bin/env bash
#
# Builds the `quantbacktest` wheel at the commit pinned in api/quantbacktest.pin
# and installs it into the active Python environment.
#
#   scripts/install_quantbacktest.sh               build + install (no-op when
#                                                  the pinned commit is installed)
#   scripts/install_quantbacktest.sh --wheel-only  build into dist/quantbacktest/
#
# The engine lives in its own repository (blueprint/wp/24-backtest.md); this is
# the only way it enters this one. Needs git, CMake >= 3.25, Ninja and Eigen:
# through the vcpkg toolchain when VCPKG_ROOT is set, as its own
# scripts/build_wheel.sh does, otherwise the system package (libeigen3-dev).
# Its CMake fetches quantModeling at the commit *it* pins, so the build needs
# the network. NPROC=n bounds the compile jobs (default 16).
#
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

WHEEL_ONLY=0
[[ "${1:-}" == "--wheel-only" ]] && WHEEL_ONLY=1

# shellcheck disable=SC1091
source api/quantbacktest.pin

installed_sha() {
  python -c 'import quantbacktest; print(quantbacktest.build_info()["git_sha"])' 2>/dev/null || true
}

if [[ "$WHEEL_ONLY" == 0 && "$(installed_sha)" == "$QUANTBACKTEST_SHA" ]]; then
  echo "quantbacktest ${QUANTBACKTEST_SHA:0:7} already installed."
  exit 0
fi

# ── Sources at the pinned commit ─────────────────────────────────────────────
SRC_DIR="$ROOT_DIR/build/quantbacktest/$QUANTBACKTEST_SHA"
OUT_DIR="$ROOT_DIR/dist/quantbacktest"
if [[ "$(git -C "$SRC_DIR" rev-parse HEAD 2>/dev/null || true)" != "$QUANTBACKTEST_SHA" ]]; then
  rm -rf "$SRC_DIR"
  mkdir -p "$SRC_DIR"
  git -C "$SRC_DIR" init --quiet
  git -C "$SRC_DIR" fetch --quiet --depth 1 "$QUANTBACKTEST_REPO" "$QUANTBACKTEST_SHA"
  git -C "$SRC_DIR" -c advice.detachedHead=false checkout --quiet FETCH_HEAD
fi

# ── Wheel ────────────────────────────────────────────────────────────────────
# QB_GIT_SHA is passed rather than read from the clone: the commit recorded in
# the wheel is what the audit event names, and it must be the pin.
export CMAKE_ARGS="${CMAKE_ARGS:-} -DQB_GIT_SHA=${QUANTBACKTEST_SHA}"
if [[ -n "${VCPKG_ROOT:-}" ]]; then
  CMAKE_ARGS+=" -DCMAKE_TOOLCHAIN_FILE=${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
fi

# Bounded compile jobs: Ninja would otherwise start one per core, and this
# build (quantModeling included) runs next to the other wheel's in the image.
# NPROC=n to change, as in scripts/make.sh.
JOBS="${NPROC:-16}"
CORES="$(nproc)"
export CMAKE_BUILD_PARALLEL_LEVEL="$((JOBS < CORES ? JOBS : CORES))"

# uv if present (a uv-created environment has no pip), otherwise pip + build.
rm -rf "$OUT_DIR"
if command -v uv >/dev/null 2>&1; then
  PIP=(uv pip)
  (cd "$SRC_DIR" && uv build --wheel --python "$(command -v python)" --out-dir "$OUT_DIR")
else
  PIP=(python -m pip)
  python -m pip install --quiet --root-user-action=ignore build
  python -m build --wheel --outdir "$OUT_DIR" "$SRC_DIR"
fi

WHEEL=("$OUT_DIR"/quantbacktest-"$QUANTBACKTEST_VERSION"-*.whl)
if [[ ! -f "${WHEEL[0]}" ]]; then
  echo "✗ commit ${QUANTBACKTEST_SHA:0:7} did not build quantbacktest ${QUANTBACKTEST_VERSION}:" >&2
  echo "  api/quantbacktest.pin names a version that commit does not declare." >&2
  ls "$OUT_DIR" >&2
  exit 1
fi

if [[ "$WHEEL_ONLY" == 1 ]]; then
  echo "Wheel built: ${WHEEL[0]}"
  exit 0
fi

"${PIP[@]}" install --force-reinstall --no-deps "${WHEEL[0]}"

if [[ "$(installed_sha)" != "$QUANTBACKTEST_SHA" ]]; then
  echo "✗ quantbacktest is installed but does not report commit ${QUANTBACKTEST_SHA:0:7}." >&2
  exit 1
fi
echo "quantbacktest ${QUANTBACKTEST_VERSION} (${QUANTBACKTEST_SHA:0:7}) installed."
