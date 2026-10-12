FROM python:3.12-slim AS build-base

WORKDIR /app

RUN apt-get update && apt-get install -y --no-install-recommends \
	build-essential \
	cmake \
	libeigen3-dev \
	pybind11-dev \
	ninja-build \
	git \
	ca-certificates \
	&& rm -rf /var/lib/apt/lists/*

# The backtest engine, at the commit pinned in api/quantbacktest.pin
# (blueprint/wp/24-backtest.md). Rebuilt only when the pin moves.
FROM build-base AS backtest-builder

COPY api/quantbacktest.pin ./api/quantbacktest.pin
COPY scripts/install_quantbacktest.sh ./scripts/install_quantbacktest.sh
RUN scripts/install_quantbacktest.sh --wheel-only

FROM build-base AS builder

COPY pyproject.toml CMakeLists.txt README.md ./
COPY bindings ./bindings
COPY include ./include
COPY src ./src

RUN python -m pip install --upgrade pip --root-user-action=ignore \
	&& python -m pip install --root-user-action=ignore "scikit-build-core>=0.10.2" "pybind11>=3.0.1" build \
	&& python -m build --wheel

FROM python:3.12-slim

WORKDIR /app

RUN apt-get update && apt-get install -y --no-install-recommends \
	bash \
	bash-completion \
	curl \
	ca-certificates \
	&& rm -rf /var/lib/apt/lists/* \
	&& mkdir -p /app/logs

ENV SHELL=/bin/bash

COPY api/requirements.txt ./api/requirements.txt
RUN python -m pip install --upgrade pip --root-user-action=ignore \
	&& python -m pip install --root-user-action=ignore -r api/requirements.txt

COPY --from=builder /app/dist/*.whl /tmp/
COPY --from=backtest-builder /app/dist/quantbacktest/*.whl /tmp/
RUN python -m pip install --root-user-action=ignore --force-reinstall --no-deps /tmp/*.whl \
	&& rm -rf /tmp/*.whl


COPY api ./api

EXPOSE 8000

CMD ["uvicorn", "api.app.main:app", "--host", "0.0.0.0", "--port", "8000"]
