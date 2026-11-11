"""Reference values of the standard normal quantile at 40+ digits (mpmath).

Source of tests/data/inverse_normal_reference.txt (WP 26 §5, P1i): run it
with mpmath installed (outside the API's venv), then keep every 20th point
and every point with |x| > 3.

Points: random doubles in (0, 1), a log grid of tails down to 1e-300 on
both sides, the 53-bit grid near 0 and 1 (what Philox and Sobol produce),
and the algorithms' region boundaries. Output: p as a hex float, x as a
25-digit decimal.
"""

import math, random, sys
import mpmath as mp

random.seed(20261111)
ps = set()
for _ in range(40000):
    ps.add(random.random())
k = 1.0
while k <= 300:
    ps.add(10.0**-k)
    k += 0.25
for j in range(1, 54):
    ps.add(1.0 - 2.0**-j)
    ps.add(2.0**-j)
for i in range(1, 2000):
    ps.add(i * 2.0**-53)
    ps.add(1.0 - i * 2.0**-53)
for b in (0.02425, 0.075, 0.5 - 0.425, 0.5 + 0.425, math.exp(-25), 1 - math.exp(-25)):
    for d in (-3, -1, 0, 1, 3):
        ps.add(math.nextafter(b, 2) if d > 0 else b)
        ps.add(b + d * 1e-17 * b)
ps = sorted(p for p in ps if 0.0 < p < 1.0)

out = open(sys.argv[1], "w")
for p in ps:
    tail = min(p, 1.0 - p)
    mp.mp.dps = 45 + max(0, int(-math.log10(tail)))
    pm = mp.mpf(p)
    x = mp.sqrt(2) * mp.erfinv(2 * pm - 1)
    mp.mp.dps = 30
    out.write("%s %s\n" % (float.hex(p), mp.nstr(x, 25)))
print(len(ps))
