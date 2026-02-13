FROM python:3.12-slim AS builder

WORKDIR /app

RUN apt-get update && apt-get install -y --no-install-recommends \
	build-essential \
	cmake \
	libeigen3-dev \
	pybind11-dev \
	ninja-build \
	&& rm -rf /var/lib/apt/lists/*

COPY pyproject.toml CMakeLists.txt README.md ./
COPY bindings ./bindings
COPY include ./include
COPY src ./src

RUN python -m pip install --upgrade pip \
	&& python -m pip install "scikit-build-core>=0.10.2" "pybind11>=3.0.1" build \
	&& python -m build --wheel

FROM python:3.12-slim

WORKDIR /app

COPY api/requirements.txt ./api/requirements.txt
RUN python -m pip install --upgrade pip \
	&& python -m pip install -r api/requirements.txt

COPY --from=builder /app/dist/*.whl /tmp/
RUN python -m pip install --force-reinstall /tmp/*.whl \
	&& rm -rf /tmp/*.whl

COPY api ./api

EXPOSE 8000

CMD ["uvicorn", "api.app.main:app", "--host", "0.0.0.0", "--port", "8000"]
