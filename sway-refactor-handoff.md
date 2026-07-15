# Sway Refactor — New Session Handoff

- Prepared: 2026-07-15
- Repository: `/home/kamer/os`
- Handoff commit at preparation time: `69fb255`
- Authoritative plan: `sway-refactor-plan.md`
- Root agent target: `gpt-5.6-sol`
- Worker target: `terra-high` = `gpt-5.6-terra` with `model_reasoning_effort = "high"`

## 1. New-session activation

Start a new Codex session from the repository root after this handoff is present.

The project configuration in `.codex/config.toml` selects Sol for the root session and caps the workflow at four concurrent threads: one root plus three workers. The project custom agent in `.codex/agents/terra-high.toml` pins worker sessions to Terra High.

At the beginning of the new session:

1. Confirm the repository is trusted so project `.codex` configuration is loaded.
2. Confirm the root model is `gpt-5.6-sol`.
3. Confirm the custom agent named `terra-high` is visible.
4. When the client exposes agent details, confirm `gpt-5.6-terra` and reasoning `high` before delegating implementation.
5. If the current orchestration surface cannot explicitly select `terra-high`, do not claim that workers are pinned. Stop delegation and report the limitation.
6. Read `AGENTS.md` completely.
7. Read `sway-refactor-plan.md` completely.
8. Read this handoff completely.
9. Inspect `git status` and preserve every existing change.

## 2. State carried from the previous session

The previous session created:

- `sway-refactor-plan.md`
- `.codex/config.toml`
- `.codex/agents/terra-high.toml`
- this handoff

At handoff creation time, `sway-refactor-plan.md` was untracked. Do not discard or overwrite it.

Three default subagents were briefly started for read-only investigation and then deliberately interrupted:

- `lifecycle_slice`
- `fd_ownership_slice`
- `test_observability_slice`

They made no code changes. Their incomplete investigations are not accepted findings and must not be described as completed work.

No kernel code was edited. No build, rootfs sync, QEMU run, or test execution was performed while creating the plan and handoff.

## 3. Operating model

The Sol root owns:

- architecture and contract design
- task boundaries and file ownership
- kernel and ABI decisions
- worker monitoring
- diff review
- integration order
- shared QEMU and battery execution
- final correctness decisions

Terra High workers own:

- one short implementation patch
- fine-grained tests for that patch
- narrow investigation needed to implement it
- concise evidence and handback

Workers must not receive broad prompts such as “understand all fd handling” or “fix Sway.” Every task packet must state:

- one concrete outcome
- exact allowed files or directories
- prohibited files
- invariant being established
- red or targeted test
- commands the worker may run
- task-specific artifact directory
- conditions that require stopping and returning to the root

Do not keep all three workers busy for appearance. Spawn only independent, bounded work. Because all agents share one worktree, no two workers may edit the same file concurrently. Rootfs sync and QEMU are serialized.

## 4. Short-term objective

The first short-term milestone is:

> Establish one correct, generic child-lifecycle path for Foot and other Linux processes: child exit must wake a blocked parent without periodic polling, SIGCHLD must use the normal signal-frame machinery, waitpid must reap the exact child, and repeated execution must not leak process or fd state.

This milestone is intentionally narrower than removing `lpr_sway_launcher.c`. It removes one of the most dangerous lifecycle foundations before service leases and full launcher removal.

It must not:

- add a Foot- or Sway-specific branch
- touch the kernel
- redesign all fd kinds
- solve drmd ownership in the same patch
- change performance targets
- hide failure with a shorter polling interval

## 5. Why this is first

Current `69fb255` added Foot reachability but also contains provisional child-lifecycle work:

- supervisor process-state flags for pending SIGCHLD and child presence
- LPR epoll polling of supervisor state
- pending signal dispatch outside the established signal-frame path
- temporary `P6_PAGE_DIAG` output
- Foot tests whose success still depends on `lpr_sway_launcher` and timeout-driven control

Foot window visibility proves that the stack reaches the compositor; it does not prove correct child exit, signal delivery, reaping, or resource cleanup. Completing the generic child path gives later fd lease and launcher-removal work a trustworthy process lifecycle.

## 6. Immediate work packets

The packets are sequential unless the Sol root proves their file sets do not overlap.

### Packet A — exact generic red test

Assign one `terra-high` worker.

Purpose:

- Add a small non-Sway Linux fixture that forks a child while the parent blocks in the same wait mechanism used by GUI event loops.
- Require child exit to interrupt or wake the parent through the real signal path.
- Require the SIGCHLD handler, exact wait status, and one successful reap.
- Exercise more than one child sequentially so stale pending state is visible.

The Sol root must choose the exact expected Linux behavior before spawning the worker. The worker implements the fixture, build/pack entry, and focused runner only.

Candidate coverage:

- handler installed with `sigaction`
- child exits with a nonzero known status
- parent blocks in `epoll_wait` or the selected generic event-loop wait
- wait is interrupted or awakened according to the chosen Linux contract
- handler runs through the normal signal frame
- `waitpid` returns the intended PID and status
- a second child does not consume stale state
- no child remains reapable after completion

Do not use Sway launcher markers as this test's oracle.

### Packet B — supervisor notification

Spawn only after Packet A and root review.

Assign one `terra-high` worker a bounded supervisor-side patch.

Purpose:

- Replace global “children exist / pending SIGCHLD” sampling with a process-specific state-change notification contract.
- Preserve exact child PID and state until reaped.
- Avoid lost notifications between state publication and parent blocking.

The Sol root must first define:

- notification ownership
- queueing/coalescing semantics
- generation or sequence handling
- close/HUP behavior
- whether the private supervisor ABI must change

If a private ABI change is required, the root must provide its justification, exact operation placement and numbering, and producer/consumer list before delegation. The worker must not invent the ABI.

Limit this packet to the supervisor, its protocol, and supervisor-focused unit tests. Do not edit LPR runtime in parallel.

### Packet C — LPR wait and signal-frame integration

Spawn after Packet B is reviewed.

Assign one `terra-high` worker.

Purpose:

- Connect the process-specific notification to the existing LPR wait graph.
- Deliver SIGCHLD through the normal pending-signal and signal-frame machinery.
- Remove the child-specific fixed-period supervisor sampling from the completed path.
- Remove direct invocation of a user handler from LPR C code.

Required local regressions:

- Packet A fixture
- existing async-signal test
- existing signal-owner test
- existing fork/pthread test
- existing epoll test
- existing PTY probe

Do not run Sway or the full battery inside the worker unless the root explicitly assigns the shared QEMU slot.

### Packet D — integration and cleanup

The Sol root reviews all diffs, then assigns a short test/cleanup worker if needed.

Actions:

- remove temporary `P6_PAGE_DIAG` output made obsolete by the new path
- confirm no 10 ms child polling remains
- run the focused QEMU sequence in a single serialized slot
- run the Foot probe only after generic lifecycle tests pass
- compare process, thread, LPR fd, and native fd snapshots before and after repeated child cycles
- document any performance change without rejecting an otherwise correct patch solely for transitional overhead

## 7. Short-term exit criteria

The milestone is complete only when:

- the generic child-lifecycle fixture is green
- a blocked event-loop parent is woken by a real process-specific notification
- SIGCHLD uses the normal signal-frame machinery
- `waitpid` reaps the exact child once
- sequential children do not inherit stale pending state
- child handling has no fixed-period supervisor poll
- no user handler is directly called from internal LPR C dispatch
- temporary child-lifecycle `P6_PAGE_DIAG` output is removed or replaced by structured runtime-gated tracing
- async-signal, signal-owner, fork/pthread, epoll, and PTY focused regressions are green
- the Foot child can exit and be reaped without adding a Foot-specific compatibility path
- repeated child cycles return process, thread, LPR fd, and native fd counts to the same-run starting values

Sway launcher removal, drmd lease cleanup, generic SCM_RIGHTS, SMP, dynamic input, and performance work remain later milestones.

## 8. Work queued immediately after this milestone

Do not start these until the child-lifecycle milestone is green:

1. Strict Sway/Foot resource oracle without leak allowances.
2. Generic service lease, beginning with the service/object path proven to leak under forced process exit.
3. drmd owner/export lifetime replacement; remove next-open orphan reaping.
4. Generic SCM_RIGHTS retransfer for the Sway-required backends.
5. Persistent seatd/session environment and direct `/usr/bin/sway`.
6. wlroots memfd keymap path and preload deletion.
7. Correct PRIME completion and `LP_NUM_THREADS=0` deletion.
8. `lpr_sway_launcher.c` deletion after all responsibilities have moved.

## 9. Kernel and build boundary

The short-term milestone is userland work. Workers must not edit `kernel/`.

Before any later kernel edit:

1. prove why userland cannot implement the required mechanism
2. list exact kernel files and invariants
3. define the red test
4. ask the user for explicit permission

If kernel verification is later approved, follow `AGENTS.md` exactly. Do not run `zig build-obj` at repository root. Runtime artifacts belong under `.artifacts/`. Use the repository's approved WSL clang path rather than substituting `zig cc`.

## 10. Suggested opening prompt for the new Sol session

```text
Read AGENTS.md, sway-refactor-plan.md, and sway-refactor-handoff.md completely.
Confirm that this root session is using gpt-5.6-sol and that the project custom
agent terra-high resolves to gpt-5.6-terra with high reasoning. Preserve the
current worktree.

Act as the architecture owner and monitor. Use terra-high workers only for
short, bounded implementation and fine-grained tests; do not delegate broad or
long investigations. Do not edit kernel code without presenting evidence and
obtaining my explicit permission.

Begin with the short-term child-lifecycle milestone in the handoff. First inspect
the exact current process notification and signal-frame code yourself, then
freeze Packet A's Linux contract and delegate only Packet A. Review its diff
before starting Packet B.
```
