# CLAUDE.md

## Project

Limit order book matching engine in C++20 for a single instrument, built with Spec-Driven Development (SDD).
Goals: demonstrate high-performance C++ on a real market problem, and demonstrate SDD in practice
(formal spec, tests derived from the spec, AI-assisted implementation). Final results go on a résumé
with concrete metrics: throughput and per-order latency (p50, p99, p99.9).

## Source of truth

`specs/` is the source of truth. Code follows the spec, never the other way around.

- `specs/spec.md`: what the system does. Requirements with IDs (`R-<AREA>-<NN>`), invariants, edge cases, decision log (`D-NN`).
- `specs/plan.md`: how it is built. Data structures, architecture, event format, performance targets.
- `specs/tasks.md`: the spec broken into small tasks, each with the requirements it covers and an acceptance criterion.

Read the relevant parts of `specs/` before any change.

## Rules

- Do not implement anything that is not in the spec. If something is ambiguous or missing, stop, point it out and propose a change to `spec.md` first.
- Any new behavior starts by changing the spec, then plan, then tasks, then tests, then code, in that order.
- Work one task from `tasks.md` at a time.
- Write tests from the spec before the code. Every test references the requirement IDs it verifies (in the test name or a comment).
- When reviewing code, check that it satisfies the spec, not only that it compiles and passes tests.
- Do not change the spec silently. Spec changes are explicit and go in their own commit.
- Respond to the user in Portuguese. Never use the em dash character.

## Conventions

- C++20, CMake, Ninja. Namespace `me`. Public headers in `include/me/`, sources in `src/`.
- Prices and quantities are integers (ticks). Never floating point.
- No dynamic allocation on the critical path (order processing). Preallocate.
- The engine library depends only on the standard library. GoogleTest and Google Benchmark are used only in `tests/` and `bench/`.
- The library builds with `-Wall -Wextra -Wpedantic -Werror`. Code is formatted with `clang-format` (config in repo root).
- Never run `git commit` or `git push`. The user reviews each task and commits himself. When a task is done, suggest a commit message instead.
- Suggested commit messages follow Conventional Commits (`feat:`, `fix:`, `test:`, `docs(spec):`, `chore:`, `perf:`) and reference task IDs when applicable.
- Nothing may mention Claude in any form: no `Co-Authored-By: Claude` trailer, no "Generated with Claude Code" line, no AI attribution in commit messages, PR descriptions or any file. The author is the repository owner only.

## Layout

```
specs/        spec.md, plan.md, tasks.md
include/me/   public headers
src/          engine implementation (library target `me`)
tests/        GoogleTest tests (target `tests`)
bench/        Google Benchmark + synthetic order generator (target `bench`)
```

## Commands

```bash
# Debug build with ASan + UBSan: use for correctness
cmake -S . -B build/debug -G Ninja -DCMAKE_BUILD_TYPE=Debug -DME_SANITIZE=ON
cmake --build build/debug && ctest --test-dir build/debug --output-on-failure

# Release build: use only for benchmarks
cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/release && ./build/release/bench
```

Never report performance numbers from the debug build.

## Status

Done:

- [x] Repository, CMake, GoogleTest and Google Benchmark set up; placeholder test and benchmark pass in both builds.
- [x] `specs/spec.md` v1 written, with all ambiguities resolved (D-01 to D-22).
- [x] `specs/plan.md` written.

Next:

- [ ] `specs/tasks.md`
- [ ] Implementation of v1, task by task
- [ ] Randomized invariant tests (R-INV-*), optionally against a naive reference engine
- [ ] Benchmark v1: throughput and p50/p99/p99.9 latency
- [ ] README with results and requirement → test table
- [ ] CI (GitHub Actions) running the debug build and tests

Update this section at the end of every completed step.

## Future versions

Each starts by changing the spec.

- v2: IOC and FOK orders.
- v3: multiple instruments.
- v4: ITCH message parser.
