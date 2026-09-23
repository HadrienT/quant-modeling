"""IP hashing (contract §1, WP §7 / quant-platform ADR-009).

The audit trail never carries a clear-text IP address — it is personal data on
a public deployment. The producer hashes it before the event ever leaves the
process: the same IP always hashes to the same value, so a burst of failed
logins from one source is still detectable, but the address itself cannot be
recovered from the audit trail.

`QM_AUDIT_IP_HMAC_SECRET` is deliberately optional: if it isn't set, the
already-required `JWT_SECRET` (auth.py fails fast without one) is reused, so a
deployment doesn't need a second secret provisioned before this lot is usable.
"""

from __future__ import annotations

import hashlib
import hmac
import os


def hash_ip(ip: str | None) -> str | None:
    if not ip:
        return None
    secret = os.getenv("QM_AUDIT_IP_HMAC_SECRET") or os.getenv("JWT_SECRET", "")
    if not secret:
        return None
    return hmac.new(secret.encode(), ip.encode(), hashlib.sha256).hexdigest()
