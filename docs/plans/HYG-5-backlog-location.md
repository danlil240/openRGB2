# HYG-5: Backlog location and the evidence policy

Status: proposed · Size: S · Source: audit 2026-11-28 Hygiene ("Backlog location")

## Goal

There should be one place in git where open work lives, and one consistent rule
for what QA evidence is committed. Today the QA backlog is invisible to anyone
cloning the repo, while 12 MB of one task's screenshots and logs are committed.

## Current state

- **QA reports and evidence stay local.** `.gitignore` says "QA session reports
  and evidence (docs/qa/) stay local, not in git" and ignores `docs/qa/`. The
  N-series backlog items (N01, N03 and so on) were defined there. Only the
  audit doc mentions them in git now
  (`docs/audit-2026-11-28-open-findings.md:861-862`).
  - In this worktree `docs/qa/` does not exist.
  - The main checkout has two session folders (2026-09-30 and 2026-10-01 full
    sessions). Listed, not read.
- **One task's evidence is committed.** `task12-evidence/` is committed: 159
  files, 12 MB (85 PNG, 31 JSON, 19 logs, 8 Python, and others). It is
  referenced from `task-12-report.md:8`, `:82`, `:88` and
  `docs/studio-import.md:74`.
  - Its own `.gitignore` drops `package/` and `__pycache__/`.
  - `tools/qa/shots/` is committed by design (`.gitignore` comment, 65 files,
    7.2 MB).
- **The open backlog is the audit doc.** `AGENTS.md:26` already points agents
  at `docs/audit-2026-11-28-open-findings.md` ("read it before picking up new
  work"). The plans in `docs/plans/` will now hang off it.

## Design

**The rule.** *Findings and decisions are committed. Bulk evidence is not,
except what a committed report cites as proof.*

1. **Backlog.**
   - Make `docs/backlog.md` the single living index. Each line holds an ID, a
     one-line status and a link to its plan, its audit entry or a QA note.
   - The audit doc becomes a dated snapshot that the backlog links to. The
     coordinator's `docs/plans/README.md` can serve as this index if Daniel
     prefers one file fewer.
   - Copy each still-open item from `docs/qa/` sessions into it as text:
     ID, symptom, repro and status, with no screenshots.
2. **QA session notes.** Change the ignore rule to `docs/qa/**/evidence/`.
   - Session *reports* (`.md`) under `docs/qa/<session>/` are then committed.
   - Screenshots and logs stay local, or go to the vault or Drive with a
     pointer.
3. **`task12-evidence/`.**
   - Keep the files the report cites: `ctest-full.log`, `qa/workflow.py`,
     `package-manifest.json`, `responsiveness.json`, and the shots it names.
   - `git rm --cached` the rest and archive them to the vault.
   - History keeps every file anyway, so this only shrinks future clones'
     working trees, not the pack.
4. **`tools/qa/shots/`.** Apply the same rule: keep the shots that a test or a
   report references, and drop the rest from the index.

**Rejected alternatives:**
- **Committing `docs/qa/` wholesale.** That adds tens of MB of screenshots and
  the same problem in a second place.
- **Rewriting history to purge the PNGs.** It is destructive for 19 MB in a
  private repo. Not worth it.
- **GitHub Issues as the backlog.** It splits the backlog from the code agents
  read, and the issues are not available offline. Revisit if CI (HYG-1) brings
  more GitHub workflow.

## Steps

1. Daniel decides between a new `docs/backlog.md` and the coordinator's plans
   index as the single index, and whether `docs/qa` reports may be committed.
   Test: none.
2. Create the index and transcribe the open N-items from the local `docs/qa/`
   sessions. This needs access to the main checkout. Test: every ID in the audit
   doc and in `docs/plans/` appears once.
3. Change the `.gitignore` rule to ignore only the evidence folders, and commit
   the session reports, after reading each one for personal data. Test: run
   `git status` on a scratch session folder and check that only the `.md` is
   staged.
4. Prune `task12-evidence/` and `tools/qa/shots/` to the cited set, and update
   the citations if paths change. Test: a grep finds no dangling
   `task12-evidence/` or `tools/qa/shots/` references in `*.md`, `*.py` or
   `tests/**`.
5. Update `AGENTS.md:26` to point at the index. Test: none.

## Tests

The reference-integrity grep in step 4 can become a small CTest script check
(label `core`), so a future prune cannot break a citation.

## Risks and open questions

- **Personal data.** QA reports may hold paths with the username, device
  serials or MACs. Step 3 needs a read-through, or the HYG-3 sanitizer, before
  anything is committed.
- **Breaking evidence.** Some `tools/qa/shots/` files may be fixtures that a
  test compares against. Grep `tests/` for each one before pruning.
- **The vault rule.** Archiving evidence to the vault means it follows the
  vault's own `AGENTS.md` commit rules. Daniel's call whether the vault or Drive
  is the better home for binary evidence.
