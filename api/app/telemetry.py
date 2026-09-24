"""OpenTelemetry for the API — blueprint WP 18d (`blueprint/wp/18-observability.md` §5.4).

Two parallel paths (quant-platform principle 1): audit events go to Kafka
(`audit/`); metrics, traces and logs go to the OTel Collector of
`quant-platform`, over OTLP/HTTP. Telemetry may lose a sample, the audit trail
may not — which is why this module never blocks nor fails a request: without
`OTEL_EXPORTER_OTLP_ENDPOINT` nothing is exported and every instrument below is
a no-op, and an exporter that cannot reach the Collector only drops samples.

What it provides:
- HTTP metrics and spans from the FastAPI instrumentation, under the stable
  semantic conventions (`http_server_request_duration_seconds` with
  `http_route`, the path template — never the raw path — and
  `http_response_status_code`), which the platform's API dashboard reads;
- a span per psycopg query (market data reads);
- the business metrics `qm_*` of WP §5.4;
- the active trace, which `request_context.current_trace_id()` copies into
  every audit event, so a row of `audit.events` opens its trace in Grafana.

Cardinality rule (platform principle 8): labels take values from small closed
sets (product, engine, model, kind, outcome, error code). Never a ticker, a
user, a request id or an IP — those live in audit events and spans.
"""

from __future__ import annotations

import os
import sys
from typing import TYPE_CHECKING

from opentelemetry import metrics, trace

from .audit.metrics import audit_dropped_total, audit_spool_depth

if TYPE_CHECKING:
    from fastapi import FastAPI

SERVICE_NAME_DEFAULT = "quant-modeling-api"

_meter = metrics.get_meter("quant-modeling-api")
tracer = trace.get_tracer("quant-modeling-api")

#: Seconds, with buckets from 5 ms (an analytic price) to 20 s (the pricing
#: timeout of routers/pricing.py) — the SDK's default buckets are sized for
#: milliseconds and would put every pricing in the first two.
_PRICING_BUCKETS_S = [0.005, 0.01, 0.025, 0.05, 0.1, 0.25, 0.5, 1, 2.5, 5, 10, 20]

pricing_duration = _meter.create_histogram(
    "qm_pricing_duration",
    unit="s",
    description="Duration of a pricing, by product, engine and model.",
    explicit_bucket_boundaries_advisory=_PRICING_BUCKETS_S,
)
pricing_errors = _meter.create_counter(
    "qm_pricing_errors",
    unit="{error}",
    description="Pricings that failed, by product and error code.",
)
data_fallback = _meter.create_counter(
    "qm_data_fallback",
    unit="{event}",
    description="Fallbacks to a live source or a default value, by kind.",
)
auth_events = _meter.create_counter(
    "qm_auth_events",
    unit="{event}",
    description="Authentication events, by outcome.",
)


def _observe_dropped(_options):
    yield metrics.Observation(audit_dropped_total.value)


def _observe_spool_depth(_options):
    yield metrics.Observation(audit_spool_depth.value)


_meter.create_observable_counter(
    "qm_audit_dropped",
    callbacks=[_observe_dropped],
    unit="{event}",
    description="Audit events the API had to abandon.",
)
_meter.create_observable_gauge(
    "qm_audit_spool_depth",
    callbacks=[_observe_spool_depth],
    unit="{event}",
    description="Audit events waiting in the local spool for Kafka.",
)


def _zero_series() -> None:
    """Creates every closed-set series at 0: a counter that is born at 1 has
    no earlier sample, so `increase()` would miss its first event
    (quant-platform ADR-013 §2)."""
    from .audit.payloads import AuthOutcome, FallbackKind

    for kind in FallbackKind:
        data_fallback.add(0, {"kind": kind.value})
    for outcome in AuthOutcome:
        auth_events.add(0, {"outcome": outcome.value})


def setup_telemetry(app: "FastAPI") -> bool:
    """Installs the SDK and the instrumentations if an OTLP endpoint is
    configured; returns whether it did. Called once, from main.py."""
    if not os.getenv("OTEL_EXPORTER_OTLP_ENDPOINT"):
        return False
    try:
        # Stable HTTP semantic conventions: the metric and attribute names the
        # platform's dashboards and alerts are written against (ADR-012 §5).
        os.environ.setdefault("OTEL_SEMCONV_STABILITY_OPT_IN", "http")

        from opentelemetry.exporter.otlp.proto.http.metric_exporter import (
            OTLPMetricExporter,
        )
        from opentelemetry.exporter.otlp.proto.http.trace_exporter import (
            OTLPSpanExporter,
        )
        from opentelemetry.instrumentation.fastapi import FastAPIInstrumentor
        from opentelemetry.instrumentation.psycopg import PsycopgInstrumentor
        from opentelemetry.sdk.metrics import MeterProvider
        from opentelemetry.sdk.metrics.export import PeriodicExportingMetricReader
        from opentelemetry.sdk.resources import Resource
        from opentelemetry.sdk.trace import TracerProvider
        from opentelemetry.sdk.trace.export import BatchSpanProcessor

        resource = Resource.create(
            {
                "service.name": os.getenv("OTEL_SERVICE_NAME", SERVICE_NAME_DEFAULT),
                "service.version": os.getenv("COMMIT_SHA", "dev"),
            }
        )
        tracer_provider = TracerProvider(resource=resource)
        tracer_provider.add_span_processor(BatchSpanProcessor(OTLPSpanExporter()))
        trace.set_tracer_provider(tracer_provider)

        # 15 s matches Prometheus' scrape interval on the platform side.
        reader = PeriodicExportingMetricReader(
            OTLPMetricExporter(), export_interval_millis=15_000
        )
        metrics.set_meter_provider(
            MeterProvider(resource=resource, metric_readers=[reader])
        )

        FastAPIInstrumentor.instrument_app(app, excluded_urls="health")
        PsycopgInstrumentor().instrument()
        _zero_series()
        return True
    except Exception as exc:  # noqa: BLE001 — telemetry must never stop the API
        print(f"telemetry: disabled ({exc})", file=sys.stderr)
        return False


def shutdown_telemetry() -> None:
    """Flushes the last spans and metric points (FastAPI shutdown)."""
    for provider in (trace.get_tracer_provider(), metrics.get_meter_provider()):
        shutdown = getattr(provider, "shutdown", None)
        if shutdown is not None:
            try:
                shutdown()
            except Exception:  # noqa: BLE001
                pass
