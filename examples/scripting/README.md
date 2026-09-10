# Scripting playground

A tiny command-line tool to price a payoff script against a flat Black-Scholes
model. See [`blueprint/wp/16-scripting.md`](../../blueprint/wp/16-scripting.md)
for the language.

## Build

```sh
cmake --preset default
cmake --build build --target qm_price_script
```

## Run

```sh
./build/qm_price_script examples/scripting/european_call.qms
./build/qm_price_script examples/scripting/autocall.qms --vol 0.22 --sobol
./build/qm_price_script my_script.qms --spot 95 --rate 0.04 --paths 500000
```

Options: `--spot --rate --div --vol --valuation YYYY-MM-DD --paths --seed --sobol`
(defaults: spot 100, rate 3%, div 0, vol 20%, valuation = today, 200k pseudo-random
paths). Run with `-h` for the full list.

## The language in one minute

A script is a list of dated **events**. Under each date line, indented, come the
statements that run when a simulated path reaches that date:

```
2027-09-10
    pays max(spot() - 100, 0)
```

- `spot()` — the underlying's level at the current event date.
- `name = expr` — assign a variable. Variables persist from one event to the
  next, which is how path-dependency is expressed.
- `pays expr` — record a cash flow at the current date (it is discounted by that
  date's numeraire automatically).
- `if cond then ... [else ...] endIf` — conditions use `= != < <= > >=` and
  `and or not`.
- functions: `min max log exp sqrt abs smooth`; operators `+ - * / ^`
  (`^` binds right).
- `#` starts a comment to end of line.
- A date line may list several dates — the same block then applies to each.
- Every event must fall strictly after the valuation date (historical fixings
  are not supported yet).

## Examples here

| File | Payoff |
|---|---|
| `european_call.qms` | vanilla call, one payment at maturity |
| `arithmetic_asian.qms` | arithmetic-average Asian call over four fixings |
| `autocall.qms` | autocallable note, 5% memory coupon, terminal knock-in |
| `cliquet.qms` | ratchet: capped/floored quarterly returns |

## Errors

A malformed script prints the offending line and column with a caret:

```
script error:
line 2, col 26: expected ')' to close the argument list, found end of line
        pays max(spot() - 100
                             ^
```
