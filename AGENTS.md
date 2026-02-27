# AGENTS.md — asupersync ANSI C (`asx`)

> Guidelines for AI coding agents working in this ANSI C/C99 codebase.

---

## RULE 0 - THE FUNDAMENTAL OVERRIDE PREROGATIVE

If I tell you to do something, even if it goes against what follows below, YOU MUST LISTEN TO ME. I AM IN CHARGE, NOT YOU.

---

## RULE NUMBER 1: NO FILE DELETION

**YOU ARE NEVER ALLOWED TO DELETE A FILE WITHOUT EXPRESS PERMISSION.** Even a new file that you yourself created, such as a test code file. You have a horrible track record of deleting critically important files or otherwise throwing away tons of expensive work. As a result, you have permanently lost any and all rights to determine that a file or folder should be deleted.

**YOU MUST ALWAYS ASK AND RECEIVE CLEAR, WRITTEN PERMISSION BEFORE EVER DELETING A FILE OR FOLDER OF ANY KIND.**

---

## Irreversible Git & Filesystem Actions — DO NOT EVER BREAK GLASS

> **Note:** This project is safety-critical around data/code integrity. Practice what we preach.

1. **Absolutely forbidden commands:** `git reset --hard`, `git clean -fd`, `rm -rf`, or any command that can delete or overwrite code/data must never be run unless the user explicitly provides the exact command and states, in the same message, that they understand and want the irreversible consequences.
2. **No guessing:** If there is any uncertainty about what a command might delete or overwrite, stop immediately and ask the user for specific approval. "I think it's safe" is never acceptable.
3. **Safer alternatives first:** When cleanup or rollbacks are needed, request permission to use non-destructive options (`git status`, `git diff`, `git stash`, copying to backups) before ever considering a destructive command.
4. **Mandatory explicit plan:** Even after explicit user authorization, restate the command verbatim, list exactly what will be affected, and wait for a confirmation that your understanding is correct. Only then may you execute it—if anything remains ambiguous, refuse and escalate.
5. **Document the confirmation:** When running any approved destructive command, record (in the session notes / final response) the exact user text that authorized it, the command actually run, and the execution time. If that record is absent, the operation did not happen.

---

## Git Branch: ONLY Use `main`, NEVER `master`

**The default branch is `main`. The `master` branch exists only for legacy URL compatibility.**

- **All work happens on `main`** — commits, PRs, feature branches all merge to `main`
- **Never reference `master` in code or docs** — if you see `master` anywhere, it's a bug that needs fixing
- **The `master` branch must stay synchronized with `main`** — after pushing to `main`, also push to `master`:
  ```bash
  git push origin main:master
  ```

**Why this matters:** Installer URLs and legacy docs may still reference `master`. If `master` falls behind `main`, users can get stale binaries/docs and incorrect behavior.

**If you see `master` referenced anywhere:**
1. Update it to `main`
2. Ensure `master` is synchronized: `git push origin main:master`

---

## Toolchain: ANSI C (C99 Allowed)

We use a **dependency-free ANSI C core** with strict portability constraints.

- **Language baseline:** ANSI C core; C99 is allowed where it materially improves correctness/maintainability.
- **Dependency policy:** Standard C library only in core runtime.
- **Build system:** `Makefile` first; optional CMake generator allowed.
- **Compiler matrix:** GCC + Clang + MSVC across 32/64-bit targets.
- **Profiles:** `ASX_PROFILE_CORE`, `POSIX`, `WIN32`, `FREESTANDING`, `EMBEDDED_ROUTER`, `HFT`, `AUTOMOTIVE`.
- **Core rule:** Resource/operational tuning is allowed; semantic behavior drift is not.

### Key Build/Runtime Macros

| Macro | Purpose |
|------|---------|
| `ASX_CODEC_JSON` / `ASX_CODEC_BIN` | Codec mode selection (debug/parity vs production throughput) |
| `ASX_PROFILE_*` | Platform/deployment profile selection |
| `ASX_CLASS_R1/R2/R3` | Embedded resource class selection |
| `ASX_DEBUG_GHOST` | Enable ghost safety monitors for protocol/linearity assertions |
| `ASX_DETERMINISTIC` | Deterministic scheduling/replay mode |

---

## Code Editing Discipline

### No Script-Based Changes

**NEVER** run a script that processes/changes code files in this repo. Brittle regex-based transformations create far more problems than they solve.

- **Always make code changes manually**, even when there are many instances
- For many simple changes: use parallel subagents
- For subtle/complex changes: do them methodically yourself

### No File Proliferation

If you want to change something or add a feature, **revise existing code files in place**.

**NEVER** create variations like:
- `mainV2.c`
- `main_improved.c`
- `main_enhanced.c`

New files are reserved for **genuinely new functionality** that makes zero sense to include in any existing file. The bar for creating new files is **incredibly high**.

---

## Backwards Compatibility

We do not care about backwards compatibility—we're in early development with no users. We want to do things the **RIGHT** way with **NO TECH DEBT**.

- Never create "compatibility shims"
- Never create wrapper functions for deprecated APIs
- Just fix the code directly

---

## Compiler Checks (CRITICAL)

**After any substantive code changes, you MUST verify no errors were introduced:**

```bash
# Build with strict warnings
make build

# Run formatting/lint gates
make format-check
make lint

# Run core tests and conformance
make test
make conformance
```

If you see errors, **carefully understand and resolve each issue**. Read sufficient context to fix them the RIGHT way.

---

## Testing

### Testing Policy

Every module must have unit tests plus invariant/conformance coverage. Tests must cover:
- Happy path
- Edge cases (empty input, max values, boundary conditions)
- Error conditions

Key test layers from this port plan:
- Unit tests per module
- Invariant tests (state transitions, linearity, lifecycle legality)
- Scenario tests (cancellation/drain/finalize)
- Rust-vs-C conformance fixtures
- Differential fuzzing and deterministic counterexample minimization

### Unit Tests

Run all local tests:

```bash
make test
make test-unit
make test-invariants
```

### Conformance and Profile Parity

```bash
# Rust reference fixture parity
make conformance

# Cross-profile semantic parity
make profile-parity

# Differential fuzzing smoke
make fuzz-smoke
```

### Test Categories

| Module | Tests | Purpose |
|--------|-------|---------|
| `tests/unit/` | module-level tests | Pure C API/logic correctness |
| `tests/invariant/` | lifecycle invariants | Region/task/obligation legality + quiescence |
| `tests/conformance/` | Rust parity fixtures | Canonical semantic equivalence |
| `tests/stress/` | pressure tests | Queue/timer/cancel storms |
| `tests/fuzz/` | differential fuzzing | Rust↔C drift detection + minimization |

---

## Third-Party Library Usage

If you aren't 100% sure how to use a third-party library, **SEARCH ONLINE** to find the latest documentation and current best practices.

---

## asupersync ANSI C (`asx`) — This Project

**This is the project you're working on.** `asx` is a highly portable, dependency-free ANSI C port of asupersync focused on semantic fidelity, deterministic replay, and constrained-device viability.

### What It Does

Ports asupersync’s runtime kernel semantics into C while preserving:

- region/task/obligation lifecycle contracts,
- explicit cancellation protocol and bounded cleanup,
- deterministic execution and replay identity,
- strict resource exhaustion behavior without partial corruption.

### Architecture

```
Scenario/API Input
  -> asx_core (IDs, outcomes, budgets, state authorities)
  -> asx_runtime_kernel (scheduler, cancel, timers, quiescence)
  -> codecs (JSON/BIN, canonical semantic model)
  -> trace/replay/conformance (Rust parity + profile parity)
```

### Key Files

| File | Purpose |
|------|---------|
| `include/asx/asx.h` | Public API surface |
| `include/asx/asx_config.h` | Runtime/profile/resource configuration |
| `src/core/` | Core semantics: IDs, outcomes, budgets, status/error taxonomy |
| `src/runtime/` | Scheduler, lifecycle engine, cancellation, quiescence |
| `src/channel/` | Bounded MPSC two-phase channel semantics |
| `src/time/` | Timer wheel and time abstractions |
| `src/platform/` | POSIX/WIN32/FREESTANDING/embedded adapters |
| `tests/unit/` | Module unit tests |
| `tests/invariant/` | Invariant/lifecycle legality tests |
| `tests/conformance/` | Rust parity + codec/profile parity tests |
| `fixtures/rust_reference/` | Canonical fixtures captured from Rust source runtime |
| `docs/EXISTING_ASUPERSYNC_STRUCTURE.md` | Authoritative extracted semantics |
| `docs/FEATURE_PARITY.md` | Semantic parity tracker by feature/surface |
| `PLAN_TO_PORT_ASUPERSYNC_TO_ANSI_C.md` | Program plan and hard quality gates |

### Portability Profiles

| Profile | Intent |
|---------|--------|
| `ASX_PROFILE_CORE` | Deterministic single-thread kernel, no OS assumptions |
| `ASX_PROFILE_POSIX` | Optional worker/reactor integration |
| `ASX_PROFILE_WIN32` | Windows runtime integration |
| `ASX_PROFILE_FREESTANDING` | User-supplied hooks, no FS/network assumptions |
| `ASX_PROFILE_EMBEDDED_ROUTER` | OpenWrt/low-memory router-class defaults |
| `ASX_PROFILE_HFT` | Tail-latency/jitter-governed low-latency profile |
| `ASX_PROFILE_AUTOMOTIVE` | Deadline/watchdog-governed safety profile |

### Codec and Conformance Contract

- JSON first for bring-up/diffability.
- Binary codec for throughput/footprint.
- JSON and BIN must map to one canonical semantic schema.
- Same scenario must produce identical canonical semantic digest across:
  - JSON vs BIN codec,
  - all supported profiles for shared fixture sets.

### Required Quality Gates (Non-Negotiable)

- Portability matrix gate (GCC/Clang/MSVC, 32/64-bit).
- Strict OOM/resource-exhaustion gate (failure-atomic).
- Differential fuzzing gate (Rust vs C + minimized counterexamples).
- Embedded target matrix gate (cross-build + QEMU + device smoke).
- Cross-profile semantic parity gate.
- HFT tail-latency/jitter gate.
- Automotive deadline/watchdog gate.
- Crash/restart replay continuity gate.
- Semantic delta budget gate (`0` by default unless explicitly approved).

### Implementing New Behavior Safely

1. Update semantics docs (`EXISTING_ASUPERSYNC_STRUCTURE.md` + parity tables).
2. Add/extend invariant schema and forbidden-behavior fixtures.
3. Implement in `asx_core`/runtime with explicit state transitions.
4. Add unit + invariant + conformance fixtures.
5. Run full gates before merge.

---

<!-- asx-machine-readable-v1 -->

## ASX Conformance Fixture Contract (Machine-Readable Reference)

Fixture schema (conceptual):

```json
{
  "scenario_id": "region-close-cancel-001",
  "input": { "seed": 42, "ops": [] },
  "expected_events": [],
  "expected_final_snapshot": {},
  "expected_error_codes": []
}
```

Conformance report shape (conceptual):

```json
{
  "scenario_id": "region-close-cancel-001",
  "codec": "json",
  "profile": "ASX_PROFILE_CORE",
  "parity": "pass",
  "semantic_digest": "sha256:...",
  "diffs": []
}
```

Canonical rule:

- If `scenario_id` and semantic inputs are the same, canonical semantic digest must match across:
  - Rust reference and C runtime
  - JSON and BIN codecs
  - profiles participating in parity set

<!-- end-asx-machine-readable -->

---

## CI/CD Pipeline

### Jobs Overview

| Job | Trigger | Purpose | Blocking |
|-----|---------|---------|----------|
| `check` | PR, push | format + lint + strict build | Yes |
| `unit-invariant` | PR, push | module + invariant test suites | Yes |
| `conformance` | PR, push | Rust fixture parity + codec equivalence | Yes |
| `profile-parity` | PR, push | cross-profile semantic digest parity | Yes |
| `fuzz-parity` | PR, push | differential fuzzing smoke + minimization check | Yes |
| `embedded-matrix` | PR, push | router-class cross-target builds + QEMU | Yes |
| `perf-tail-deadline` | push to main | HFT tail + automotive deadline gates | Warn/Block per threshold |

### Check Job

Includes:
- `make format-check`
- `make lint`
- `make build` (warnings-as-errors policy)

### Conformance Jobs

Includes:
- `make conformance`
- `make codec-equivalence`
- `make profile-parity`

### Embedded Matrix Job

Includes:
- cross-target build for router-class triplets,
- QEMU replay/scenario execution,
- optional real-device smoke verification.

### Debugging CI Failures

#### Conformance Failure
1. Re-run `make conformance` locally
2. Inspect parity diff artifact
3. Minimize failing scenario
4. Fix semantic drift at source (not fixture output)

#### Profile Parity Failure
1. Re-run `make profile-parity`
2. Compare canonical digests by profile
3. Check resource-plane logic for unintended semantic fork

#### Embedded Matrix Failure
1. Re-run cross-build target locally
2. Re-run QEMU scenario pack
3. Inspect endian/alignment and hook assumptions

#### Tail/Deadline Regression
1. Re-run perf suite with fixed seed/input
2. Check `p99/p99.9/p99.99` and deadline misses
3. Validate deterministic overload/degraded-mode behavior

---

## Release Process

When fixes are ready for release, follow this process:

### 1. Verify Local Gates

```bash
make format-check
make lint
make test
make conformance
make profile-parity
```

### 2. Commit Changes

```bash
git add -A
git commit -m "fix: description of fixes

- List specific fixes
- Include any breaking changes

Co-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>"
```

### 3. Version and Tag

Use semantic versioning tags for releases:

- **Patch** (`vX.Y.Z+1`): bug fixes
- **Minor** (`vX.Y+1.0`): new features, compatible
- **Major** (`vX+1.0.0`): breaking changes

### 4. Push and Trigger Release

```bash
git push origin main
git push origin main:master  # Keep master in sync
git push origin --tags
```

### 5. Verify Release

```bash
gh release list --limit 5
gh release view <tag>
```

Expected assets per release:
- `asx-{target}.tar.xz` - Binary archive
- `asx-{target}.tar.xz.sha256` - Checksum
- `asx-{target}.tar.xz.sigstore.json` - Signature bundle
- installer scripts and release notes

### Troubleshooting Failed Releases

If CI fails:
1. Check workflow run: `gh run list --limit=5`
2. View failed job: `gh run view <run-id>`
3. Fix locally, re-run local gates, commit, and push
4. Re-tag only when artifacts and gates are green

---

## MCP Agent Mail — Multi-Agent Coordination

A mail-like layer that lets coding agents coordinate asynchronously via MCP tools and resources. Provides identities, inbox/outbox, searchable threads, and advisory file reservations with human-auditable artifacts in Git.

### Why It's Useful

- **Prevents conflicts:** Explicit file reservations (leases) for files/globs
- **Token-efficient:** Messages stored in per-project archive, not in context
- **Quick reads:** `resource://inbox/...`, `resource://thread/...`

### Same Repository Workflow

1. **Register identity:**
   ```
   ensure_project(project_key=<abs-path>)
   register_agent(project_key, program, model)
   ```

2. **Reserve files before editing:**
   ```
   file_reservation_paths(project_key, agent_name, ["src/**"], ttl_seconds=3600, exclusive=true)
   ```

3. **Communicate with threads:**
   ```
   send_message(..., thread_id="FEAT-123")
   fetch_inbox(project_key, agent_name)
   acknowledge_message(project_key, agent_name, message_id)
   ```

4. **Quick reads:**
   ```
   resource://inbox/{Agent}?project=<abs-path>&limit=20
   resource://thread/{id}?project=<abs-path>&include_bodies=true
   ```

### Macros vs Granular Tools

- **Prefer macros for speed:** `macro_start_session`, `macro_prepare_thread`, `macro_file_reservation_cycle`, `macro_contact_handshake`
- **Use granular tools for control:** `register_agent`, `file_reservation_paths`, `send_message`, `fetch_inbox`, `acknowledge_message`

### Common Pitfalls

- `"from_agent not registered"`: Always `register_agent` in the correct `project_key` first
- `"FILE_RESERVATION_CONFLICT"`: Adjust patterns, wait for expiry, or use non-exclusive reservation
- **Auth errors:** If JWT+JWKS enabled, include bearer token with matching `kid`

---

## Beads (br) — Dependency-Aware Issue Tracking

Beads provides a lightweight, dependency-aware issue database and CLI (`br` - beads_rust) for selecting "ready work," setting priorities, and tracking status. It complements MCP Agent Mail's messaging and file reservations.

**Important:** `br` is non-invasive—it NEVER runs git commands automatically. You must manually commit changes after `br sync --flush-only`.

### Conventions

- **Single source of truth:** Beads for task status/priority/dependencies; Agent Mail for conversation and audit
- **Shared identifiers:** Use Beads issue ID (e.g., `br-123`) as Mail `thread_id` and prefix subjects with `[br-123]`
- **Reservations:** When starting a task, call `file_reservation_paths()` with the issue ID in `reason`

### Typical Agent Flow

1. **Pick ready work (Beads):**
   ```bash
   br ready --json  # Choose highest priority, no blockers
   ```

2. **Reserve edit surface (Mail):**
   ```
   file_reservation_paths(project_key, agent_name, ["src/**"], ttl_seconds=3600, exclusive=true, reason="br-123")
   ```

3. **Announce start (Mail):**
   ```
   send_message(..., thread_id="br-123", subject="[br-123] Start: <title>", ack_required=true)
   ```

4. **Work and update:** Reply in-thread with progress

5. **Complete and release:**
   ```bash
   br close 123 --reason "Completed"
   br sync --flush-only  # Export to JSONL (no git operations)
   ```
   ```
   release_file_reservations(project_key, agent_name, paths=["src/**"])
   ```
   Final Mail reply: `[br-123] Completed` with summary

### Mapping Cheat Sheet

| Concept | Value |
|---------|-------|
| Mail `thread_id` | `br-###` |
| Mail subject | `[br-###] ...` |
| File reservation `reason` | `br-###` |
| Commit messages | Include `br-###` for traceability |

---

## bv — Graph-Aware Triage Engine

bv is a graph-aware triage engine for Beads projects (`.beads/beads.jsonl`). It computes PageRank, betweenness, critical path, cycles, HITS, eigenvector, and k-core metrics deterministically.

**Scope boundary:** bv handles *what to work on* (triage, priority, planning). For agent-to-agent coordination (messaging, work claiming, file reservations), use MCP Agent Mail.

**CRITICAL: Use ONLY `--robot-*` flags. Bare `bv` launches an interactive TUI that blocks your session.**

### The Workflow: Start With Triage

**`bv --robot-triage` is your single entry point.** It returns:
- `quick_ref`: at-a-glance counts + top 3 picks
- `recommendations`: ranked actionable items with scores, reasons, unblock info
- `quick_wins`: low-effort high-impact items
- `blockers_to_clear`: items that unblock the most downstream work
- `project_health`: status/type/priority distributions, graph metrics
- `commands`: copy-paste shell commands for next steps

```bash
bv --robot-triage        # THE MEGA-COMMAND: start here
bv --robot-next          # Minimal: just the single top pick + claim command
```

### Command Reference

**Planning:**
| Command | Returns |
|---------|---------|
| `--robot-plan` | Parallel execution tracks with `unblocks` lists |
| `--robot-priority` | Priority misalignment detection with confidence |

**Graph Analysis:**
| Command | Returns |
|---------|---------|
| `--robot-insights` | Full metrics: PageRank, betweenness, HITS, eigenvector, critical path, cycles, k-core, articulation points, slack |
| `--robot-label-health` | Per-label health: `health_level`, `velocity_score`, `staleness`, `blocked_count` |
| `--robot-label-flow` | Cross-label dependency: `flow_matrix`, `dependencies`, `bottleneck_labels` |
| `--robot-label-attention [--attention-limit=N]` | Attention-ranked labels |

**History & Change Tracking:**
| Command | Returns |
|---------|---------|
| `--robot-history` | Bead-to-commit correlations |
| `--robot-diff --diff-since <ref>` | Changes since ref: new/closed/modified issues, cycles |

**Other:**
| Command | Returns |
|---------|---------|
| `--robot-burndown <sprint>` | Sprint burndown, scope changes, at-risk items |
| `--robot-forecast <id\|all>` | ETA predictions with dependency-aware scheduling |
| `--robot-alerts` | Stale issues, blocking cascades, priority mismatches |
| `--robot-suggest` | Hygiene: duplicates, missing deps, label suggestions |
| `--robot-graph [--graph-format=json\|dot\|mermaid]` | Dependency graph export |
| `--export-graph <file.html>` | Interactive HTML visualization |

### Scoping & Filtering

```bash
bv --robot-plan --label backend              # Scope to label's subgraph
bv --robot-insights --as-of HEAD~30          # Historical point-in-time
bv --recipe actionable --robot-plan          # Pre-filter: ready to work
bv --recipe high-impact --robot-triage       # Pre-filter: top PageRank
bv --robot-triage --robot-triage-by-track    # Group by parallel work streams
bv --robot-triage --robot-triage-by-label    # Group by domain
```

### Understanding Robot Output

**All robot JSON includes:**
- `data_hash` — Fingerprint of source beads.jsonl
- `status` — Per-metric state: `computed|approx|timeout|skipped` + elapsed ms
- `as_of` / `as_of_commit` — Present when using `--as-of`

**Two-phase analysis:**
- **Phase 1 (instant):** degree, topo sort, density
- **Phase 2 (async, 500ms timeout):** PageRank, betweenness, HITS, eigenvector, cycles

### jq Quick Reference

```bash
bv --robot-triage | jq '.quick_ref'                        # At-a-glance summary
bv --robot-triage | jq '.recommendations[0]'               # Top recommendation
bv --robot-plan | jq '.plan.summary.highest_impact'        # Best unblock target
bv --robot-insights | jq '.status'                         # Check metric readiness
bv --robot-insights | jq '.Cycles'                         # Circular deps (must fix!)
```

---

## UBS — Ultimate Bug Scanner

**Golden Rule:** `ubs <changed-files>` before every commit. Exit 0 = safe. Exit >0 = fix & re-run.

### Commands

```bash
ubs file.c file2.c include/asx/foo.h    # Specific files (< 1s) — USE THIS
ubs $(git diff --name-only --cached)    # Staged files — before commit
ubs --only=c,h,toml src/ include/       # Language filter (3-5x faster)
ubs --ci --fail-on-warning .            # CI mode — before PR
ubs .                                   # Whole project (ignores build/ and generated artifacts)
```

### Output Format

```
Warning  Category (N errors)
    file.c:42:5 - Issue description
    Suggested fix
Exit code: 1
```

Parse: `file:line:col` -> location | fix hint -> how to fix | Exit 0/1 -> pass/fail

### Fix Workflow

1. Read finding -> category + fix suggestion
2. Navigate `file:line:col` -> view context
3. Verify real issue (not false positive)
4. Fix root cause (not symptom)
5. Re-run `ubs <file>` -> exit 0
6. Commit

### Bug Severity

- **Critical (always fix):** Memory safety, use-after-free, data races, SQL injection
- **Important (production):** Resource leaks, overflow checks, error-path omissions
- **Contextual (judgment):** TODO/FIXME, temporary debug logging

---

## RCH — Remote Compilation Helper

RCH offloads heavy C build/test/lint commands to remote workers instead of building locally. This prevents compilation storms when many agents run simultaneously.

**RCH is installed at `~/.local/bin/rch` and is hooked into Claude Code's PreToolUse automatically.** Most of the time you don't need to do anything if you are Claude Code — builds are intercepted and offloaded transparently.

To manually offload a build:
```bash
rch exec -- make build
rch exec -- make test
rch exec -- make conformance
```

Quick commands:
```bash
rch doctor                    # Health check
rch workers probe --all       # Test connectivity to all 8 workers
rch status                    # Overview of current state
rch queue                     # See active/waiting builds
```

If rch or its workers are unavailable, it fails open — builds run locally as normal.

**Note for Codex/GPT-5.2:** Codex does not have the automatic PreToolUse hook, but you can (and should) still manually offload compute-intensive compilation commands using `rch exec -- <command>`. This avoids local resource contention when multiple agents are building simultaneously.

---

## ast-grep vs ripgrep

**Use `ast-grep` when structure matters.** It parses code and matches AST nodes, ignoring comments/strings, and can **safely rewrite** code.

- Refactors/codemods: rename APIs, change import forms
- Policy checks: enforce patterns across a repo
- Editor/automation: LSP mode, `--json` output

**Use `ripgrep` when text is enough.** Fastest way to grep literals/regex.

- Recon: find strings, TODOs, log lines, config values
- Pre-filter: narrow candidate files before ast-grep

### Rule of Thumb

- Need correctness or **applying changes** -> `ast-grep`
- Need raw speed or **hunting text** -> `rg`
- Often combine: `rg` to shortlist files, then `ast-grep` to match/modify

### C Examples

```bash
# Find structured code (ignores comments)
ast-grep run -l C -p '$RET $NAME($$$ARGS) { $$$BODY }'

# Find pointer-returning functions
ast-grep run -l C -p '$RET *$NAME($$$ARGS) { $$$BODY }'

# Quick textual hunt
rg -n 'ASX_[A-Z0-9_]+' -t c -t h

# Combine speed + precision
rg -l -t c -t h 'malloc\(' | xargs ast-grep run -l C -p 'malloc($$$ARGS)' --json
```

---

## Morph Warp Grep — AI-Powered Code Search

**Use `mcp__morph-mcp__warp_grep` for exploratory "how does X work?" questions.** An AI agent expands your query, greps the codebase, reads relevant files, and returns precise line ranges with full context.

**Use `ripgrep` for targeted searches.** When you know exactly what you're looking for.

**Use `ast-grep` for structural patterns.** When you need AST precision for matching/rewriting.

### When to Use What

| Scenario | Tool | Why |
|----------|------|-----|
| "How is cancellation propagation implemented?" | `warp_grep` | Exploratory; don't know where to start |
| "Where is the timer wheel tie-break ordering?" | `warp_grep` | Need architecture context |
| "Find all uses of `ASX_E_RESOURCE_EXHAUSTED`" | `ripgrep` | Targeted literal search |
| "Find files with TODO/FIXME in runtime" | `ripgrep` | Simple pattern |
| "Rewrite a C function signature pattern" | `ast-grep` | Structural refactor |

### warp_grep Usage

```
mcp__morph-mcp__warp_grep(
  repoPath: "/dp/asupersync_ansi_c",
  query: "How do region/task/obligation invariants and transition checks work?"
)
```

Returns structured results with file paths, line ranges, and extracted code snippets.

### Anti-Patterns

- **Don't** use `warp_grep` to find a specific function name -> use `ripgrep`
- **Don't** use `ripgrep` to understand "how does X work" -> wastes time with manual reads
- **Don't** use `ripgrep` for codemods -> risks collateral edits

<!-- bv-agent-instructions-v1 -->

---

## Beads Workflow Integration

This project uses [beads_rust](https://github.com/Dicklesworthstone/beads_rust) (`br`) for issue tracking. Issues are stored in `.beads/` and tracked in git.

**Important:** `br` is non-invasive—it NEVER executes git commands. After `br sync --flush-only`, you must manually run `git add .beads/ && git commit`.

### Essential Commands

```bash
# View issues (launches TUI - avoid in automated sessions)
bv

# CLI commands for agents (use these instead)
br ready              # Show issues ready to work (no blockers)
br list --status=open # All open issues
br show <id>          # Full issue details with dependencies
br create --title="..." --type=task --priority=2
br update <id> --status=in_progress
br close <id> --reason "Completed"
br close <id1> <id2>  # Close multiple issues at once
br sync --flush-only  # Export to JSONL (NO git operations)
```

### Workflow Pattern

1. **Start**: Run `br ready` to find actionable work
2. **Claim**: Use `br update <id> --status=in_progress`
3. **Work**: Implement the task
4. **Complete**: Use `br close <id>`
5. **Sync**: Run `br sync --flush-only` then manually commit

### Key Concepts

- **Dependencies**: Issues can block other issues. `br ready` shows only unblocked work.
- **Priority**: P0=critical, P1=high, P2=medium, P3=low, P4=backlog (use numbers, not words)
- **Types**: task, bug, feature, epic, question, docs
- **Blocking**: `br dep add <issue> <depends-on>` to add dependencies

### Session Protocol

**Before ending any session, run this checklist:**

```bash
git status              # Check what changed
git add <files>         # Stage code changes
br sync --flush-only    # Export beads to JSONL
git add .beads/         # Stage beads changes
git commit -m "..."     # Commit everything together
git push                # Push to remote
```

### Best Practices

- Check `br ready` at session start to find available work
- Update status as you work (in_progress -> closed)
- Create new issues with `br create` when you discover tasks
- Use descriptive titles and set appropriate priority/type
- Always `br sync --flush-only && git add .beads/` before ending session

<!-- end-bv-agent-instructions -->

## Landing the Plane (Session Completion)

**When ending a work session**, you MUST complete ALL steps below.

**MANDATORY WORKFLOW:**

1. **File issues for remaining work** - Create issues for anything that needs follow-up
2. **Run quality gates** (if code changed) - Tests, linters, builds
3. **Update issue status** - Close finished work, update in-progress items
4. **Sync beads** - `br sync --flush-only` to export to JSONL
5. **Hand off** - Provide context for next session


---

## cass — Cross-Agent Session Search

`cass` indexes prior agent conversations (Claude Code, Codex, Cursor, Gemini, ChatGPT, etc.) so we can reuse solved problems.

**Rules:** Never run bare `cass` (TUI). Always use `--robot` or `--json`.

### Examples

```bash
cass health
cass search "async runtime" --robot --limit 5
cass view /path/to/session.jsonl -n 42 --json
cass expand /path/to/session.jsonl -n 42 -C 3 --json
cass capabilities --json
cass robot-docs guide
```

### Tips

- Use `--fields minimal` for lean output
- Filter by agent with `--agent`
- Use `--days N` to limit to recent history

stdout is data-only, stderr is diagnostics; exit code 0 means success.

Treat cass as a way to avoid re-solving problems other agents already handled.

---

Note for Codex/GPT-5.2:

You constantly bother me and stop working with concerned questions that look similar to this:

```
Unexpected changes (need guidance)

- Working tree still shows edits I did not make in Makefile, include/asx/asx.h, src/runtime/scheduler.c, src/channel/mpsc.c, tests/conformance/profile_parity.c. Please advise whether to keep/commit/revert these before any further work. I did not touch them.

Next steps (pick one)

1. Decide how to handle the unrelated modified files above so we can resume cleanly.
2. Triage beads_rust-orko (build/lint warnings) and beads_rust-ydqr (format failures).
3. If you want a full suite run later, fix conformance/lint blockers and re-run `make test`.
```

NEVER EVER DO THAT AGAIN. The answer is literally ALWAYS the same: those are changes created by the potentially dozen of other agents working on the project at the same time. This is not only a common occurrence, it happens multiple times PER MINUTE. The way to deal with it is simple: you NEVER, under ANY CIRCUMSTANCE, stash, revert, overwrite, or otherwise disturb in ANY way the work of other agents. Just treat those changes identically to changes that you yourself made. Just fool yourself into thinking YOU made the changes and simply don't recall it for some reason.

---

## Note on Built-in TODO Functionality

Also, if I ask you to explicitly use your built-in TODO functionality, don't complain about this and say you need to use beads. You can use built-in TODOs if I tell you specifically to do so. Always comply with such orders.
