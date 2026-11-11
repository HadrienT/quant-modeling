#!/usr/bin/env python3
"""Run the tests a change can affect, not all of them.

Three levels (CLAUDE.md, "Tests: three levels"):

  level 1, while developing (default): the C++ tests whose sources depend on a
      changed file (Ninja's recorded dependencies), the GPU tests only when the
      change reaches them (all of them when it reaches the CUDA code), the API
      tests that import a changed module (all of them when none does), and the
      front end's typecheck, lint and vitest when web/ changed;
  level 2, before opening a PR (--pr): the whole CPU suite, the whole GPU suite
      when the change reaches the CUDA code (the CI has no GPU), the whole
      pytest when the API, the bindings or the library changed;
  level 3, the CI: everything but the GPU, as before.

    scripts/test_affected.py [--pr] [--base origin/main] [--build-dir build-cuda]
                             [--range A..B] [--dry-run]

--range lists the files changed by a commit range instead of the working tree
against --base (to check what a past change would have run); --dry-run prints
the selection without building or running. The C++ selection reads the
dependencies of an up-to-date build: the script builds the test targets first.
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TEST_RE = re.compile(r"^\s*TEST(?:_F|_P)?\(\s*(\w+)\s*,", re.MULTILINE)

# A change to these files can affect any C++ test: run them all.
CPP_GLOBAL = ("CMakeLists.txt", "CMakePresets.json", "vcpkg.json", "cmake/")


def run(cmd: list[str], **kw) -> subprocess.CompletedProcess:
    return subprocess.run(cmd, cwd=ROOT, text=True, capture_output=True, **kw)


def changed_files(base: str, rng: str | None) -> list[str]:
    if rng:
        out = run(["git", "diff", "--name-only", rng]).stdout.split()
        return sorted(set(out))
    mb = run(["git", "merge-base", "HEAD", base]).stdout.strip() or base
    tracked = run(["git", "diff", "--name-only", mb]).stdout.split()
    untracked = run(
        ["git", "ls-files", "--others", "--exclude-standard"]
    ).stdout.split()
    return sorted(set(tracked) | set(untracked))


def ninja_deps(build: Path, target: str) -> set[str]:
    """Absolute paths a target's object was built from (`ninja -t deps`)."""
    out = run(["ninja", "-C", str(build), "-t", "deps", target]).stdout
    return {line.strip() for line in out.splitlines()[1:] if line.startswith("    ")}


def test_objects(build: Path, target_dir: str) -> dict[str, str]:
    """{test source (repo-relative): its object} for one test executable."""
    objs = {}
    d = build / "CMakeFiles" / target_dir
    for obj in d.rglob("*.cpp.o"):
        rel = obj.relative_to(build).as_posix()
        src = obj.relative_to(d).as_posix()[: -len(".o")]
        objs[src] = rel
    return objs


def suites_of(sources: list[str]) -> list[str]:
    names = set()
    for s in sources:
        p = ROOT / s
        if p.exists():
            names.update(TEST_RE.findall(p.read_text(errors="replace")))
    return sorted(names)


def paired_headers(src_cpp: str) -> set[str]:
    """src/x/y.cpp -> the library headers named y.hpp (its declarations)."""
    stem = Path(src_cpp).stem
    return {p.as_posix() for p in (ROOT / "include").rglob(f"{stem}.hpp")}


def affected_tests(
    build: Path, target_dir: str, changed: list[str]
) -> tuple[list[str], bool]:
    """(test sources to run, run-all) for one test executable."""
    if any(c == g or c.startswith(g) for c in changed for g in CPP_GLOBAL):
        return [], True
    absolute = {str(ROOT / c) for c in changed}
    for c in changed:
        if c.startswith("src/") and c.endswith((".cpp", ".cu")):
            absolute |= paired_headers(c)
    picked = []
    for src, obj in sorted(test_objects(build, target_dir).items()):
        if src in changed or ninja_deps(build, obj) & absolute:
            picked.append(src)
    return picked, False


def reaches_kernels(build: Path, changed: list[str]) -> bool:
    """Whether the change reaches the CUDA code itself (src/gpu, the
    compiled-script kernel): then every GPU test may move."""
    if any(c.startswith("src/gpu/") for c in changed):
        return True
    if not (build / "CMakeFiles" / "quantModeling.dir" / "src" / "gpu").exists():
        return False
    absolute = {str(ROOT / c) for c in changed}
    for c in changed:
        if c.startswith("src/") and c.endswith((".cpp", ".cu")):
            absolute |= paired_headers(c)
    gpu_dir = build / "CMakeFiles" / "quantModeling.dir" / "src" / "gpu"
    targets = [o.relative_to(build).as_posix() for o in gpu_dir.glob("*.o")]
    targets.append("qm_script_jit_kernel.fatbin")
    return any(ninja_deps(build, t) & absolute for t in targets)


def python_selection(changed: list[str], level2: bool) -> tuple[list[str], bool]:
    """(API test files, run-all)."""
    api = [c for c in changed if c.startswith("api/")]
    library = [c for c in changed if c.startswith(("src/", "include/", "bindings/"))]
    if level2 and (api or library):
        return [], True
    if any(c.startswith("bindings/") for c in changed):
        return [], True
    tests = ROOT / "api" / "tests"
    picked = set()
    for c in api:
        p = Path(c)
        if (
            c.startswith("api/tests/")
            and p.name.startswith("test_")
            and p.suffix == ".py"
        ):
            picked.add(c)
            continue
        if c.startswith("api/tests/"):  # conftest, fixtures: everything
            return [], True
        if p.suffix != ".py":
            continue
        module = ".".join(p.with_suffix("").parts[1:])  # app.routers.pricing
        short = p.stem
        pat = re.compile(
            rf"(from\s+{re.escape(module)}\s+import|import\s+{re.escape(module)}\b|"
            rf"from\s+{re.escape('.'.join(module.split('.')[:-1]))}\s+import\s+[^\n]*\b{short}\b)"
        )
        hits = {
            t.relative_to(ROOT).as_posix()
            for t in tests.rglob("test_*.py")
            if t.name.startswith(f"test_{short}")
            or pat.search(t.read_text(errors="replace"))
        }
        if not hits:
            # No test imports it (a router reached through the app's client):
            # every API test may.
            return [], True
        picked |= hits
    return sorted(picked), False


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument(
        "--pr", action="store_true", help="level 2: before opening a pull request"
    )
    ap.add_argument("--base", default="origin/main")
    ap.add_argument(
        "--build-dir",
        default="build-cuda" if (ROOT / "build-cuda").exists() else "build",
    )
    ap.add_argument(
        "--range",
        dest="rng",
        help="files changed by a commit range (A..B) instead of the tree",
    )
    ap.add_argument(
        "--files", nargs="+", help="these files as the change (to check a selection)"
    )
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--jobs", type=int, default=int(os.environ.get("NPROC", "16")))
    args = ap.parse_args()

    build = ROOT / args.build_dir
    changed = sorted(args.files) if args.files else changed_files(args.base, args.rng)
    cpp_changed = [
        c
        for c in changed
        if c.startswith(("include/", "src/", "tests/", "cmake/"))
        or c in ("CMakeLists.txt", "CMakePresets.json", "vcpkg.json")
    ]
    print(
        f"{len(changed)} changed file(s); build dir {args.build_dir}; level {'2 (--pr)' if args.pr else '1'}"
    )

    has_gpu_build = (build / "CMakeFiles" / "quantModeling_gpu_tests.dir").exists()
    steps: list[tuple[str, list[str]]] = []
    if cpp_changed:
        targets = ["quantModeling_tests"] + (
            ["quantModeling_gpu_tests"] if has_gpu_build else []
        )
        if not args.dry_run:
            print("building", " ".join(targets), "...")
            b = run(
                ["cmake", "--build", str(build), "--target", *targets, f"-j{args.jobs}"]
            )
            if b.returncode != 0:
                print(b.stdout[-4000:], b.stderr[-4000:])
                return b.returncode
        cpu_sources, cpu_all = affected_tests(build, "quantModeling_tests.dir", changed)
        if args.pr or cpu_all:
            print(
                "CPU: the whole suite ("
                + ("level 2" if args.pr else "a build file changed")
                + ")"
            )
            steps.append(
                (
                    "CPU suite (all)",
                    [
                        "ctest",
                        "--test-dir",
                        args.build_dir,
                        "-LE",
                        "gpu",
                        f"-j{args.jobs}",
                        "--output-on-failure",
                    ],
                )
            )
        elif cpu_sources:
            suites = suites_of(cpu_sources)
            print(
                f"CPU: {len(cpu_sources)} test file(s), {len(suites)} suite(s): {' '.join(cpu_sources)}"
            )
            steps.append(
                (
                    "CPU suites",
                    [
                        "ctest",
                        "--test-dir",
                        args.build_dir,
                        "-LE",
                        "gpu",
                        "-R",
                        "^(" + "|".join(suites) + r")\.",
                        f"-j{args.jobs}",
                        "--output-on-failure",
                    ],
                )
            )
        else:
            print("CPU: no test depends on the change")
        if has_gpu_build:
            # In series: the cards are shared (and with the assistant's LLM).
            # The CI has no GPU: what is selected here is never run elsewhere.
            gpu_cmd = [
                "ctest",
                "--test-dir",
                args.build_dir,
                "-L",
                "gpu",
                "-j1",
                "--output-on-failure",
            ]
            gpu_sources, gpu_all = affected_tests(
                build, "quantModeling_gpu_tests.dir", changed
            )
            if gpu_all or reaches_kernels(build, changed):
                print("GPU: the change reaches the CUDA code -- the whole GPU suite")
                steps.append(("GPU suite (all)", gpu_cmd))
            elif gpu_sources:
                suites = suites_of(gpu_sources)
                print(f"GPU: {len(gpu_sources)} test file(s): {' '.join(gpu_sources)}")
                steps.append(
                    ("GPU suites", gpu_cmd + ["-R", "^(" + "|".join(suites) + r")\."])
                )
            else:
                print("GPU: no GPU test depends on the change")
    py_tests, py_all = python_selection(changed, args.pr)
    python = str(ROOT / ".venv" / "bin" / "python")
    if py_all:
        steps.append(("pytest (all)", [python, "-m", "pytest", "-q"]))
    elif py_tests:
        print(f"pytest: {' '.join(py_tests)}")
        steps.append(("pytest", [python, "-m", "pytest", "-q", *py_tests]))

    web = [c for c in changed if c.startswith("web/")]
    if web:
        npm = ["npm", "--prefix", "web", "run", "-s"]
        print("web: typecheck, lint and vitest")
        steps += [
            ("web typecheck", npm + ["typecheck"]),
            ("web lint", npm + ["lint"]),
            ("web tests", npm + ["test"]),
        ]

    if not steps:
        print("nothing to run")
        return 0
    for name, cmd in steps:
        print(f"== {name}: {' '.join(cmd)}")
    if args.dry_run:
        return 0
    status = 0
    for name, cmd in steps:
        print(f"\n== {name}", flush=True)
        r = subprocess.run(cmd, cwd=ROOT)
        status = status or r.returncode
    return status


if __name__ == "__main__":
    sys.exit(main())
