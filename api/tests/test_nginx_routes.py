"""Every API route is reachable through the production nginx.

nginx (web/nginx.conf.template) forwards only a few path prefixes to the API;
a route outside them works in tests (which call the API directly) and 404s on
the live site. The forwarded prefixes are read from the template itself, so
this test follows it."""

import os
import re
from pathlib import Path

os.environ.setdefault("JWT_SECRET", "test-only-secret-not-for-deployment")

from fastapi.routing import APIRoute  # noqa: E402

from app.main import app  # noqa: E402

NGINX = Path(__file__).resolve().parents[2] / "web/nginx.conf.template"
#: FastAPI's own documentation routes, not served in production on purpose.
NOT_PUBLIC = {"/openapi.json", "/docs", "/docs/oauth2-redirect", "/redoc"}


def forwarded() -> list:
    """One regex per nginx location that proxies to the API."""
    text = NGINX.read_text()
    out = []
    # A block ends at a line holding only "}" (the body has "${API_URL}").
    for m in re.finditer(r"location\s+(=|~\*?|)\s*(\S+)\s*\{(.*?)\n\s*\}", text, re.S):
        kind, path, body = m.groups()
        if "${API_URL}" not in body:
            continue
        if kind == "=":
            out.append(re.compile(re.escape(path) + "$"))
        elif kind.startswith("~"):
            out.append(re.compile(path))
        else:
            out.append(re.compile(re.escape(path)))
    return out


def test_nginx_forwards_something():
    assert len(forwarded()) >= 2


def test_every_api_route_is_forwarded_by_nginx():
    rules = forwarded()
    missing = [
        r.path
        for r in app.routes
        if isinstance(r, APIRoute)
        and r.path not in NOT_PUBLIC
        and not any(rule.match(r.path) for rule in rules)
    ]
    assert not missing, (
        f"not forwarded by web/nginx.conf.template: {missing} -- "
        "put them under /api/, /market/ or /price/"
    )
