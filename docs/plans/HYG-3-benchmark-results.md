# HYG-3: Benchmark results live only in gitignored `profiles/local/`

Status: proposed · Size: S · Source: audit 2026-11-28 Hygiene ("Benchmarks")

## Goal

Every benchmark write-up in `docs/benchmarks/` should be backed by committed,
reproducible inputs: the tool and command line, the sanitized raw or summary
data, and the analysis script. That way a number can be re-derived or compared
after a change such as P-7, P-12 or E-2. Unique hardware IDs stay out of git.

## Current state

- **Write-ups only.** The 9 write-ups in `docs/benchmarks/*.md` are committed.
  The data they cite went to `profiles/local/<run>/`, which is gitignored
  (`.gitignore`: "Local user state: resolved hardware identities, session
  records"). That covers:
  - `bench-2026-09-26` and `m2-2026-09-26` (`baseline.md:26`, `playback-result.md:28`)
  - `skew-2026-09-27`
  - `lifecycle/run2`
  - `modequal`, `nonint`, `soak` (`mixed-and-soak.md:258-264`)

  None of this is in the tree.
- **Soak scripts are gone too.** The scripts (`sample.py`, `analyze.py`,
  `keepawake.ps1`, `mixed-and-soak.md:28`) were in `profiles/local/soak/`, so
  the soak is not reproducible from the repo.
- **Committed tools.** `tools/bench/lianli_bench` and `skew_bench` are
  committed, and their `--out <csv>` takes any path. Their CSV columns are
  `dev_id,dev_name,leds,pattern,rate_fps,frame,timestamp_us`
  (`tools/bench/bench_producer.cpp:553`). No serials are written there.
  `--list` prints serial and location (`:350-351`), and the baseline write-up
  redacts them (`baseline.md:149`).
- **Raw data may survive locally.** It may still exist in the main checkout's
  `profiles/local/`. That is not verified: the planner works in a worktree.

## Design

- **A committed home:** `docs/benchmarks/data/<run-id>/`, holding:
  - `run.json`: git sha, backend pin, tool and arguments, hardware profile
    reference (`docs/hardware/daniel-pc.md`), date, operator notes
  - the summary CSVs the write-up's tables come from
  - raw CSVs, when each run stays under a size budget (proposal: 2 MB per
    run). Above the budget, commit only the summaries and keep the raw files in
    the vault or Drive with a pointer in `run.json`.
- **Sanitize before commit.** `tools/bench/sanitize.py` drops or hashes serial,
  location, MAC and USB path fields, and refuses a file that still matches a
  MAC-shaped or serial-shaped regex. It runs on everything that goes under
  `data/`.
- **Scripts in the repo.** Recreate the soak sampler and analyzer as
  `tools/soak/` (see ROADMAP-gaps G3/G11). Analysis scripts that produced a
  write-up's tables live next to it in `tools/` and are referenced from the
  write-up.
- **Write-up footer.** Every write-up gets a "Data" line linking its `data/<run-id>/`.

**Rejected alternatives:**
- **Un-ignoring `profiles/local/`.** It holds real user state and unique IDs by
  design.
- **Git LFS.** It is another dependency for what is probably a few MB. Revisit
  if the budget is exceeded often.
- **The vault only.** It is not reproducible from a clone of the repo.

## Steps

1. Daniel checks whether the old run folders still exist in his main checkout's
   `profiles/local/`. If they do, salvage them through steps 3–4. If they do
   not, mark the existing write-ups "raw data lost; numbers as recorded". Test:
   none.
2. Add `tools/bench/sanitize.py` with a self-test of known-bad lines (a MAC, a
   serial field). Register it as a CTest target (label `core`, plain Python) if
   Python is on the build PATH; otherwise as a script check. Test: the
   self-test.
3. Add the `docs/benchmarks/data/` convention, a `run.json` schema in
   `docs/benchmarks/README.md`, and the size budget. Test: none (docs).
4. Backfill whatever was salvaged in step 1, and add "Data" footers. Test:
   sanitize passes on every committed file.
5. Point `lianli_bench` and `skew_bench` README examples at
   `profiles/local/<run>` for the raw output, plus a documented
   `sanitize → data/` step. Test: none.

## Tests

The `sanitize.py` self-test. No hardware is needed for this plan; future runs
produce the data.

## Risks and open questions

- Is a 2 MB-per-run budget right? Soak CSVs at 60 s sampling are small; frame
  timing CSVs at 60 fps for minutes are not (guess). Daniel decides between the
  budget and LFS.
- Whether hashed device IDs are still identifying enough to matter in a private
  repo is Daniel's call. The default is to drop them.
