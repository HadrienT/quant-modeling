"""Audit trail wired to quant-platform — blueprint WP 18b, 18d, 18e.

- 18b: `KafkaSink` against a fake producer (no broker needed): each event type
  reaches its contract topic with its contract key, a failed or impossible
  delivery lands in the spool, the spool is republished once the broker
  answers, nothing queued is lost at shutdown.
- Contract: every event the API emits validates against the envelope JSON
  Schema of quant-platform (`contract/envelope.schema.json`, a copy of
  `~/quant-platform/common/qp_common/envelope.schema.json`, canonical there).
- 18e: every pricing emits a `pricing.valuation` record, and the WP's property
  test — replay(valuation(req)) is `reproduced` with a zero gap at a fixed seed
  — for a sample of Monte-Carlo products; revised market data gives
  `drifted_inputs` naming the datum, a new build gives `drifted_code`.
- 18d: the business metrics carry the closed-set labels the platform's
  dashboards read, and an audit event carries the active trace id.
Run from the repo root:  pytest api/tests/test_observability.py
"""

from __future__ import annotations

import json
import os
from pathlib import Path

os.environ.setdefault("JWT_SECRET", "test-only-secret-not-for-deployment")

import jsonschema
import pytest
from fastapi.testclient import TestClient
from opentelemetry import metrics, trace
from opentelemetry.sdk.metrics import MeterProvider
from opentelemetry.sdk.metrics.export import InMemoryMetricReader
from opentelemetry.sdk.trace import TracerProvider

from api.app import auth, replay, schemas, valuation
from api.app.audit import emit
from api.app.audit.envelope import Event
from api.app.audit.metrics import audit_dropped_total
from api.app.audit.payloads import (
    AssistantChatPayload,
    AuthEventPayload,
    AuthOutcome,
    FallbackKind,
    FallbackPayload,
    HttpAccessPayload,
    MarketInputStatus,
    ValuationPayload,
)
from api.app.audit.sinks import (
    InMemorySink,
    KafkaSink,
    SpoolSink,
    UnroutableEvent,
    reset_sink_for_testing,
    route,
    set_sink_for_testing,
)
from api.app.audit.store import set_store_for_testing
from api.app.main import app

ENVELOPE_SCHEMA = json.loads(
    (Path(__file__).parent / "contract" / "envelope.schema.json").read_text()
)

# One provider for the whole module: OpenTelemetry allows a single global one,
# and the instruments created at import time bind to it.
_METRICS = InMemoryMetricReader()
metrics.set_meter_provider(MeterProvider(metric_readers=[_METRICS]))
trace.set_tracer_provider(TracerProvider())


# ── A fake confluent-kafka producer ──────────────────────────────────────────


class FakeProducer:
    """Records what is produced; delivers (runs the callbacks) on `poll`/`flush`
    with the outcome the test chose, like librdkafka does."""

    def __init__(self, config):
        self.config = config
        self.sent: list[tuple[str, bytes, bytes]] = []
        self.pending: list = []
        self.fail_delivery = False
        self.queue_full = False
        self.reachable = True

    def produce(self, topic, value, key, on_delivery):
        if self.queue_full:
            raise BufferError("Local: Queue full")
        self.pending.append((topic, value, key, on_delivery))

    def _deliver(self):
        pending, self.pending = self.pending, []
        for topic, value, key, cb in pending:
            if self.fail_delivery:
                cb("Local: Message timed out", None)
            else:
                self.sent.append((topic, key, value))
                cb(None, object())

    def poll(self, _timeout):
        self._deliver()
        return 0

    def flush(self, _timeout=None):
        if self.fail_delivery:
            return len(self.pending)
        self._deliver()
        return 0

    def purge(self):
        pending, self.pending = self.pending, []
        for *_rest, cb in pending:
            cb("Local: Purged in queue", None)

    def list_topics(self, timeout=None):
        if not self.reachable:
            raise RuntimeError("Local: Broker transport failure")
        return object()


@pytest.fixture
def kafka(tmp_path):
    """A KafkaSink on a fake producer, whose background thread never
    republishes on its own (tests call `republish_spool` explicitly)."""
    holder = {}

    def factory(config):
        holder["p"] = FakeProducer(config)
        return holder["p"]

    spool = SpoolSink(tmp_path / "spool")
    sink = KafkaSink(
        "kafka:9092", spool, producer_factory=factory, republish_interval_s=3600
    )
    sink._stop.set()  # stop the poll thread: the tests drive delivery
    sink._thread.join()
    yield sink, holder["p"], spool
    reset_sink_for_testing()


def _event(event_type="data.fallback", payload=None, username=None) -> Event:
    return Event.create(
        event_type,
        payload or FallbackPayload(kind=FallbackKind.DEFAULT_RATE),
        username=username,
    )


def _spooled(spool: SpoolSink) -> list[dict]:
    lines = []
    for path in sorted(spool.directory.glob("*.jsonl*")):
        lines += [json.loads(l) for l in path.read_text().splitlines() if l.strip()]
    return lines


# ── 18b: the producer ────────────────────────────────────────────────────────


def test_producer_config_is_the_contract_one(kafka):
    _sink, producer, _spool = kafka
    c = producer.config
    assert c["acks"] == "all" and c["enable.idempotence"] is True
    assert c["linger.ms"] == 20 and c["compression.type"] == "zstd"


@pytest.mark.parametrize(
    "event_type, payload, username, topic, key",
    [
        (
            "data.fallback",
            FallbackPayload(kind=FallbackKind.LIVE_YFINANCE_SPOT),
            None,
            "qm.dataquality.fallback.v1",
            "live_yfinance_spot",
        ),
        (
            "auth.login_failed",
            AuthEventPayload(outcome=AuthOutcome.LOGIN_FAILED, ip_hash="ab12"),
            None,
            "qm.audit.auth.v1",
            "anon:ab12",
        ),
        (
            "auth.login_ok",
            AuthEventPayload(outcome=AuthOutcome.LOGIN_OK),
            "alice",
            "qm.audit.auth.v1",
            "alice",
        ),
        (
            "http.access",
            HttpAccessPayload(
                route="/api/pricing/{product}",
                method="POST",
                status_code=200,
                duration_ms=3.0,
                cache_hit=False,
            ),
            "alice",
            "qm.http.access.v1",
            "/api/pricing/{product}",
        ),
    ],
)
def test_each_event_type_goes_to_its_contract_topic_and_key(
    kafka, event_type, payload, username, topic, key
):
    sink, producer, _spool = kafka
    sink.publish(_event(event_type, payload, username))
    producer.poll(0)
    [(sent_topic, sent_key, value)] = producer.sent
    assert (sent_topic, sent_key.decode()) == (topic, key)
    jsonschema.validate(json.loads(value), ENVELOPE_SCHEMA)


def test_an_unknown_event_type_is_kept_in_the_spool_not_guessed(kafka):
    sink, producer, spool = kafka
    with pytest.raises(UnroutableEvent):
        route(_event("pricing.unheard_of"))
    sink.publish(_event("pricing.unheard_of"))
    assert producer.pending == [] and len(_spooled(spool)) == 1


def test_a_failed_delivery_lands_in_the_spool(kafka):
    sink, producer, spool = kafka
    producer.fail_delivery = True
    sink.publish(_event())
    producer.poll(0)
    assert producer.sent == [] and len(_spooled(spool)) == 1


def test_a_full_queue_spools_instead_of_blocking(kafka):
    sink, producer, spool = kafka
    producer.queue_full = True
    sink.publish(_event())
    assert len(_spooled(spool)) == 1


def test_resilience_broker_down_then_back(kafka):
    """WP 18b's acceptance criterion, with the broker simulated: 100 events
    while it is down all land in the spool; once it answers, the 100 reach
    Kafka and the spool empties."""
    sink, producer, spool = kafka
    producer.fail_delivery = True
    producer.reachable = False
    events = [_event() for _ in range(100)]
    for e in events:
        sink.publish(e)
    producer.poll(0)
    assert len(_spooled(spool)) == 100
    assert sink.republish_spool() == 0  # broker still down: the spool is untouched
    assert len(_spooled(spool)) == 100

    producer.fail_delivery = False
    producer.reachable = True
    assert sink.republish_spool() == 100
    producer.poll(0)
    delivered = {json.loads(v)["event_id"] for _t, _k, v in producer.sent}
    assert delivered == {e.event_id for e in events}
    assert _spooled(spool) == [] and list(spool.directory.iterdir()) == []


def test_a_republished_event_that_fails_again_goes_back_to_the_spool(kafka):
    sink, producer, spool = kafka
    spool.publish(_event())
    producer.fail_delivery = True
    assert sink.republish_spool() == 1
    producer.poll(0)
    # the claimed copy is gone, the event is in a fresh spool file
    assert len(_spooled(spool)) == 1
    assert not list(spool.directory.glob("*.replay-*"))


def test_close_spools_what_could_not_be_delivered(kafka):
    sink, producer, spool = kafka
    producer.fail_delivery = True
    for _ in range(3):
        sink.publish(_event())
    sink.close()
    assert len(_spooled(spool)) == 3


def test_a_corrupt_spool_line_is_dropped_loudly(kafka):
    sink, producer, spool = kafka
    audit_dropped_total.reset_for_testing()
    (spool.directory / "2026-09-01.jsonl").write_text(
        _event().model_dump_json() + "\n{not json\n"
    )
    assert sink.republish_spool() == 1
    producer.poll(0)
    assert len(producer.sent) == 1 and audit_dropped_total.value == 1


# ── Contract: the envelope ───────────────────────────────────────────────────


@pytest.fixture
def inmemory_sink():
    sink = InMemorySink()
    set_sink_for_testing(sink)
    yield sink
    reset_sink_for_testing()


@pytest.fixture
def client(inmemory_sink, monkeypatch, tmp_path):
    monkeypatch.setenv("QM_STORAGE", "local")
    monkeypatch.setenv("QM_DATA_DIR", str(tmp_path / "data"))
    return TestClient(app)


def _valuations(sink: InMemorySink) -> list[Event]:
    return [e for e in sink.events if e.type == "pricing.valuation"]


def test_every_event_of_a_pricing_request_matches_the_envelope_schema(
    client, inmemory_sink
):
    r = client.post("/price/option/vanilla", json=VANILLA_MC)
    assert r.status_code == 200
    assert {e.type for e in inmemory_sink.events} == {
        "pricing.valuation",
        "http.access",
    }
    for event in inmemory_sink.events:
        jsonschema.validate(json.loads(event.model_dump_json()), ENVELOPE_SCHEMA)


@pytest.mark.parametrize("model", [ValuationPayload, AssistantChatPayload])
def test_new_payload_models_never_declare_a_secret_field(model):
    forbidden = {"password", "token", "authorization"}
    assert not (set(model.model_fields) & forbidden)


def test_no_pricing_request_model_carries_a_credential():
    """`pricing.valuation` records the request verbatim: this is what keeps a
    credential out of it — checked on the models, not on examples."""
    forbidden = {"password", "token", "authorization", "api_key", "secret"}
    for product in valuation.PRODUCTS.values():
        assert not (set(product.request.model_fields) & forbidden), product.request


# ── 18e: the valuation record ────────────────────────────────────────────────

VANILLA_MC = dict(
    spot=100.0,
    strike=100.0,
    maturity=1.0,
    rate=0.03,
    dividend=0.01,
    vol=0.2,
    is_call=True,
    engine="mc",
    n_paths=20_000,
    seed=7,
)
MC_SAMPLE = {
    "vanilla": VANILLA_MC,
    "asian": {**VANILLA_MC, "engine": "mc"},
    "barrier": dict(
        spot=100.0,
        strike=100.0,
        maturity=1.0,
        rate=0.03,
        dividend=0.0,
        vol=0.2,
        is_call=True,
        barrier_level=130.0,
        barrier_kind="up-and-out",
        n_paths=5_000,
        seed=3,
    ),
    "lookback": dict(
        spot=100.0,
        strike=100.0,
        maturity=0.5,
        rate=0.03,
        dividend=0.0,
        vol=0.2,
        is_call=True,
        n_paths=5_000,
        seed=11,
    ),
    "basket": dict(
        spots=[100.0, 90.0],
        vols=[0.2, 0.3],
        pairwise_correlation=0.4,
        strike=95.0,
        maturity=1.0,
        rate=0.03,
        is_call=True,
        n_paths=10_000,
        seed=5,
    ),
    "script": dict(
        script="2027-09-10\n    pays max(spot() - 100, 0)",
        model="black_scholes",
        spot=100.0,
        vol=0.2,
        rate=0.03,
        valuation_date="2026-09-10",
        n_paths=10_000,
        seed=9,
    ),
}


def _record(product_id: str, body: dict) -> Event:
    req = valuation.PRODUCTS[product_id].request.model_validate(body)
    priced = valuation.price(product_id, req)
    return Event.create(
        "pricing.valuation", valuation.payload(product_id, req, priced, ip_hash=None)
    )


def test_a_pricing_emits_its_valuation_record(client, inmemory_sink):
    r = client.post("/price/option/vanilla", json=VANILLA_MC)
    [event] = _valuations(inmemory_sink)
    p = event.payload
    assert p["product"] == "vanilla"
    assert p["engine"] == {
        "name": "mc",
        "n_paths": 20_000,
        "seed": 7,
        "scheme": None,
        "device": "cpu",
    }
    assert p["model"]["name"] == "black_scholes"
    assert p["result"]["npv"] == r.json()["npv"]
    assert p["request"]["seed"] == 7 and p["request_hash"].startswith("sha256:")
    assert p["market_inputs"] == []  # every input was typed in by the caller
    assert event.request_id == r.headers["X-Request-ID"]


def test_the_response_carries_the_compute_time_the_record_holds(
    client, inmemory_sink
):
    r = client.post("/price/option/vanilla", json=VANILLA_MC)
    [event] = _valuations(inmemory_sink)
    compute_ms = r.json()["compute_ms"]
    assert compute_ms > 0
    assert compute_ms == pytest.approx(event.payload["timing"]["duration_ms"])


def test_the_record_names_the_engine_that_actually_ran(client, inmemory_sink):
    """An American vanilla asked for with engine="mc" is priced on a binomial
    tree by pricing_service: the record says binomial, and keeps no seed."""
    client.post(
        "/price/option/vanilla", json={**VANILLA_MC, "is_american": True}
    ).raise_for_status()
    [event] = _valuations(inmemory_sink)
    assert event.payload["engine"] == {
        "name": "binomial",
        "n_paths": None,
        "seed": None,
        "scheme": None,
        "device": None,
    }


def test_a_failed_pricing_records_no_valuation(client, inmemory_sink):
    r = client.post("/price/scripted", json={**MC_SAMPLE["script"], "script": "x"})
    assert r.status_code == 422
    assert _valuations(inmemory_sink) == []


@pytest.mark.parametrize("product_id", sorted(MC_SAMPLE))
def test_replay_of_a_valuation_is_reproduced_at_a_fixed_seed(product_id):
    """WP 18e's property: replay(valuation(req)) is `reproduced` and the gap
    is zero — at a fixed seed the Monte-Carlo is deterministic."""
    event = _record(product_id, MC_SAMPLE[product_id])
    result = replay.replay(event.model_dump(mode="json"))
    assert result.outcome == replay.ReplayOutcome.REPRODUCED
    assert result.npv_difference == 0.0


def test_replay_names_a_revised_market_input(monkeypatch):
    """A datum revised since the valuation (data-ingest upserts) gives
    `drifted_inputs` and names it."""
    market = {"spot": 100.0}

    def price_on_market(req):
        valuation.record_market_input(
            "spot:SPY",
            "db:prices.sp500_daily",
            "2026-09-10",
            MarketInputStatus.OBSERVED,
            market["spot"],
        )
        return valuation.PRODUCTS["vanilla"].price(
            req.model_copy(update={"spot": market["spot"]})
        )

    fake = valuation.Product(
        schemas.VanillaRequest,
        price_on_market,
        lambda _r: "black_scholes",
        lambda _r: "analytic",
    )
    monkeypatch.setitem(valuation.PRODUCTS, "vanilla_on_market", fake)
    event = _record("vanilla_on_market", {**VANILLA_MC, "engine": "analytic"})
    assert event.payload["market_inputs"][0]["name"] == "spot:SPY"

    market["spot"] = 101.0
    result = replay.replay(event.model_dump(mode="json"))
    assert result.outcome == replay.ReplayOutcome.DRIFTED_INPUTS
    assert [d.name for d in result.drifted_inputs] == ["spot:SPY"]
    assert result.npv_difference != 0.0


def test_replay_reports_a_code_change_with_both_builds():
    event = _record("vanilla", VANILLA_MC).model_dump(mode="json")
    event["payload"]["code"]["lib_build"] = "0ld5ha1"
    result = replay.replay(event)
    assert result.outcome == replay.ReplayOutcome.DRIFTED_CODE
    assert result.recorded_code.lib_build == "0ld5ha1"
    assert result.current_code.lib_build != "0ld5ha1"


def test_replay_never_passes_a_different_price_silently():
    event = _record("vanilla", VANILLA_MC).model_dump(mode="json")
    event["payload"]["result"]["npv"] += 1e-6
    result = replay.replay(event)
    assert result.outcome == replay.ReplayOutcome.NOT_REPRODUCED


def test_replay_refuses_an_event_that_is_not_a_valuation():
    event = Event.create(
        "data.fallback", FallbackPayload(kind=FallbackKind.DEFAULT_RATE)
    ).model_dump(mode="json")
    with pytest.raises(replay.NotAValuation):
        replay.replay(event)


# ── 18e: the admin endpoint ──────────────────────────────────────────────────


class FakeStore:
    def __init__(self, events):
        self.events = {e["event_id"]: e for e in events}

    def get(self, event_id):
        return self.events.get(event_id)


@pytest.fixture
def admin_client(client, monkeypatch):
    monkeypatch.setenv("QM_ADMIN_USERS", "boss")
    yield client
    set_store_for_testing(None)


def _bearer(username: str) -> dict:
    return {"Authorization": f"Bearer {auth._create_token(username)}"}


def test_replay_endpoint_is_for_administrators_only(admin_client):
    r = admin_client.post("/api/admin/replay/x", headers=_bearer("someone"))
    assert r.status_code == 403
    assert admin_client.post("/api/admin/replay/x").status_code == 401


def test_replay_endpoint_without_an_audit_database_says_so(admin_client, monkeypatch):
    monkeypatch.delenv("QM_AUDIT_DB_PASSWORD", raising=False)
    monkeypatch.delenv("QM_AUDIT_DB_DSN", raising=False)
    set_store_for_testing(None)
    r = admin_client.post("/api/admin/replay/x", headers=_bearer("boss"))
    assert r.status_code == 503 and "QM_AUDIT_DB_PASSWORD" in r.json()["message"]


def test_replay_endpoint_replays_a_stored_valuation(admin_client):
    event = _record("vanilla", VANILLA_MC).model_dump(mode="json")
    set_store_for_testing(FakeStore([event]))
    r = admin_client.post(
        f"/api/admin/replay/{event['event_id']}", headers=_bearer("boss")
    )
    assert r.status_code == 200, r.text
    assert r.json()["outcome"] == "reproduced"
    missing = admin_client.post("/api/admin/replay/nope", headers=_bearer("boss"))
    assert missing.status_code == 404


# ── 18d: metrics and traces ──────────────────────────────────────────────────


def _points(name: str) -> list:
    data = _METRICS.get_metrics_data()
    points = []
    for rm in data.resource_metrics if data else []:
        for sm in rm.scope_metrics:
            for m in sm.metrics:
                if m.name == name:
                    points += list(m.data.data_points)
    return points


def test_a_pricing_is_measured_with_closed_set_labels(client):
    client.post("/price/option/vanilla", json=VANILLA_MC).raise_for_status()
    labels = [dict(p.attributes) for p in _points("qm_pricing_duration")]
    assert {
        "product": "vanilla",
        "engine": "mc",
        "model": "black_scholes",
        "device": "cpu",
    } in labels
    for attrs in labels:
        assert set(attrs) == {"product", "engine", "model", "device"}
        assert attrs["device"] in {"cpu", "gpu"}


def test_a_failed_pricing_is_counted_by_error_code(client):
    client.post("/price/scripted", json={**MC_SAMPLE["script"], "script": "x"})
    labels = [dict(p.attributes) for p in _points("qm_pricing_errors")]
    assert {"product": "script", "code": "invalid_input"} in labels


def test_auth_and_fallback_events_are_counted(inmemory_sink):
    emit(
        "auth.login_failed",
        AuthEventPayload(outcome=AuthOutcome.LOGIN_FAILED),
    )
    emit("data.fallback", FallbackPayload(kind=FallbackKind.STALE_DATA))
    assert {"outcome": "login_failed"} in [
        dict(p.attributes) for p in _points("qm_auth_events")
    ]
    assert {"kind": "stale_data"} in [
        dict(p.attributes) for p in _points("qm_data_fallback")
    ]


def test_the_drop_counter_is_exported_even_at_zero():
    audit_dropped_total.reset_for_testing()
    [point] = _points("qm_audit_dropped")
    assert point.value == 0


def test_an_audit_event_carries_the_active_trace_id(inmemory_sink):
    with trace.get_tracer("test").start_as_current_span("request") as span:
        emit("data.fallback", FallbackPayload(kind=FallbackKind.DEFAULT_RATE))
    expected = format(span.get_span_context().trace_id, "032x")
    assert inmemory_sink.events[-1].trace_id == expected
