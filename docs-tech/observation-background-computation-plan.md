# Observation Background Computation Plan

## Problem Statement

Observer output (VBI, quality metrics, closed captions, etc.) is recomputed
from scratch far more often than the underlying video data actually changes:

- **Observations are keyed to the live DAG instance, not to frame content.**
  `DAGFrameRenderer` caches renders by `(node_id, frame_id, dag_version)`
  (`orc/core/include/dag_frame_renderer.h:129-151`) and `ObservationCache`
  by `(node_id, frame_id)` (`orc/core/include/observation_cache.h:84-106`).
  Any project change rebuilds the DAG and clears both caches wholesale
  (`orc/presenters/src/render_presenter.cpp:211-255`,
  `orc/core/observation_cache.cpp:29`), so editing a parameter on one branch
  discards observations for every unaffected branch and frame.
- **All computation is demand-driven and synchronous.** Observations are only
  produced as a side effect of rendering a frame
  (`orc/core/dag_frame_renderer.cpp:220-228` runs all nine registered
  observers after each render). Opening an observer dialog or stepping through
  fields blocks on render + observe for the requested frame; whole-source
  analysis requires an explicit sink trigger that re-processes everything.
- **Nothing survives the session.** `ObservationContext` is an in-memory map
  (`orc/sdk/include/orc/stage/observation/observation_context.h:146`);
  project serialization stores only nodes/edges/parameters. Reopening a
  project recomputes every observation.

Goal: observations are computed once per *frame content*, in the background,
stored, and only recomputed when an upstream change actually affects that
frame — with the invalidation propagating down the DAG automatically.

## Target Architecture

### Frame-content identity instead of stage identity

The executor already derives a content-addressed artifact ID from
`stage_name : stage_version : input_artifact_ids : parameters`
(`orc/core/dag_executor.cpp:363-386`). Because each component is known
statically, an equivalent **node fingerprint** can be computed recursively
from the DAG alone, without executing anything. A frame's identity is then:

```
FrameProvenance = (node_fingerprint, frame_id)
ObservationRecordKey = (node_fingerprint, field_id, observer_id, observer_version)
```

This ties cached observations to the frame's *provenance* (everything that
determines its CVBS content) rather than to a live DAG node:

- A parameter change alters that node's fingerprint and — because fingerprints
  compose over inputs — every downstream node's fingerprint. Affected entries
  simply stop matching; nothing needs an explicit "dirty" walk for
  correctness. The *diff* between old and new fingerprint maps is the
  downstream-propagated change signal, and it drives background rescheduling.
- A DAG rebuild with unchanged parameters produces identical fingerprints, so
  previously stored observations remain valid and are reused — including
  across sessions once persisted.
- Source file changes are captured by folding file identity (size + mtime,
  via an injected provider) into SOURCE node fingerprints.

Provenance is a deliberate proxy for hashing actual CVBS samples: hashing
sample data would require rendering the frame first, which defeats the
purpose. The proxy is conservative (a parameter change that happens not to
alter pixels still invalidates) but never stale.

### Components

| Component | Layer | Role |
|-----------|-------|------|
| `NodeFingerprintMap` | `orc/core` | Static per-node provenance hashes computed from a `DAG`; diffable across rebuilds |
| `ObservationStore` | `orc/core` | Thread-safe store of observation records keyed by `ObservationRecordKey`; memory-budgeted; later backed by a SQLite sidecar |
| `ObservationScheduler` | `orc/core` | Worker thread owning its own `DAGExecutor`/`DAGFrameRenderer`; computes missing observations by priority; cancellable |
| Observation change/notification API | `orc/presenters` | Async request + subscribe interface for the GUI; invalidation events after project edits |
| Non-blocking observer UI | `orc/gui` | Dialogs/widgets show stored data instantly, "computing" states otherwise, update on notification |

Threading constraints: `DAGFrameRenderer` is single-threaded by contract
(`orc/core/include/dag_frame_renderer.h:69`), and an `IObserverHandle` must be
serialised per handle
(`orc/sdk/include/orc/stage/observation/observation_service_interface.h:96-106`).
The scheduler therefore owns dedicated renderer/executor/handle instances on
its worker thread; the `ObservationStore` is the sole thread-safe handoff
point. Stage/decode errors must be caught at the task boundary (worker threads
must never terminate the process).

References: observer registry
`orc/core/core_observation_service.cpp:49`, observer base
`orc/core/observers/observer.h:60`, observation context interface
`orc/sdk/include/orc/stage/observation/observation_context_interface.h:63`,
GUI consumption `orc/presenters/include/metrics_presenter.h:55`,
`orc/presenters/src/vbi_presenter.cpp:197`,
`orc/gui/mainwindow.cpp:4835-4998`, analysis-sink relationship
`docs-tech/analysis-sink-output-improvement-plan.md`.

ABI constraint for all phases: the current branch has already bumped the
plugin ABI to 11 (`orc/sdk/include/orc/abi/orc_plugin_abi.h`), and that bump
has not shipped. Any SDK header change made by this plan is folded into the
existing version-11 entry in `orc/sdk/abi_history.yaml` — no further ABI/API
version bump.

---

## Phase 1 — Frame provenance identity

Goal: a stable, statically computable fingerprint per DAG node that changes
exactly when a node's output content can change.

### Task 1.1 — Node fingerprint computation

- Add `orc/core/frame_provenance.{h,cpp}`: compute a `NodeFingerprint`
  (stable hash string or 128-bit value) per node from
  `stage_name : stage_version : ordered parameters : input fingerprints`,
  mirroring the composition in
  `DAGExecutor::compute_expected_artifact_id()`
  (`orc/core/dag_executor.cpp:363-386`). Produce a
  `NodeFingerprintMap` (`NodeID → NodeFingerprint`) for a whole `DAG` in one
  pass (topological order).
- Parameter serialization must be deterministic (ordered keys, fixed
  floating-point formatting).

**Acceptance criteria**

- Unit tests (`unit` label): identical DAG → identical map; parameter change
  on node N → changed fingerprints for N and all transitive descendants only;
  unrelated branches byte-identical; node insertion/removal changes only the
  affected chain.

### Task 1.2 — Source content identity

- Define `IFileIdentityProvider` (size + mtime lookup) in `orc/core`; fold the
  identity of each SOURCE node's file parameter(s) into that node's
  fingerprint. Capture identity at fingerprint-map construction time.
- Default implementation stats the filesystem; unit tests inject a mock per
  `TESTING.md` (no filesystem access in unit tests).

**Acceptance criteria**

- Unit tests: changed mtime/size → source and all downstream fingerprints
  change; unchanged file → stable fingerprints; missing file handled without
  throwing (distinct "unknown" identity).

### Task 1.3 — Executor alignment

- Refactor `DAGExecutor::compute_expected_artifact_id()` to reuse the
  fingerprint machinery (single definition of provenance), preserving
  current cache-hit behaviour.

**Acceptance criteria**

- Existing executor/cache unit tests pass unchanged; a parity test asserts
  artifact-ID stability for a representative DAG before/after the refactor.

---

## Phase 2 — Provenance-keyed observation store

Goal: observations survive DAG rebuilds; recomputation happens only on
fingerprint miss.

### Task 2.1 — ObservationStore

- Add `orc/core/observation_store.{h,cpp}`: thread-safe (documented
  guarantees per coding standards §5.3.3) store mapping
  `(NodeFingerprint, FieldID, observer_id, observer_version)` → the
  observer's namespaced observation values for that field.
- API: `has()`, `get()`, `put()`, `load_into(IObservationContext&)`,
  `retain_only(fingerprint_set, budget)` (eviction support), memory budget
  with LRU eviction.

**Acceptance criteria**

- Unit tests: round-trip of all `ObservationValue` variants; concurrent
  reader/writer test; eviction respects budget and retention set.

### Task 2.2 — Read-through observer pass

- In `DAGFrameRenderer` (`orc/core/dag_frame_renderer.cpp:220-228`): give the
  renderer an optional shared `ObservationStore` and the current
  `NodeFingerprintMap`. Before running observer *i* for a frame, check the
  store for both derived field keys; on hit, load stored values into the
  `ObservationContext` instead of re-running; on miss, run the observer and
  write results back to the store.

**Acceptance criteria**

- Unit tests: second render of the same frame performs zero observer runs
  (mock/spy observation service); stored values loaded into the context are
  identical to freshly computed ones; renderer without a store behaves as
  today.

### Task 2.3 — Store survives DAG rebuilds

- `ObservationCache` (`orc/core/include/observation_cache.h`) and
  `RenderPresenter::rebuildRenderersFromDAG()`
  (`orc/presenters/src/render_presenter.cpp:211-255`) share one
  `ObservationStore` owned at presenter level; `update_dag()` recomputes the
  fingerprint map and stops clearing stored observations.

**Acceptance criteria**

- Test at presenter boundary: after a parameter change on branch A,
  requesting observations on unaffected branch B triggers no observer runs;
  requesting on branch A recomputes.

### Task 2.4 — Consumer parity

- Verify `MetricsPresenter`, `VbiPresenter`, the observation presenters, and
  `cc_sink` (which drives its own `closed_caption` handle,
  `orc/plugins/stages/cc_sink/cc_sink_stage_deps.cpp:53-59`) produce
  identical results whether values come from the store or a fresh run.

**Acceptance criteria**

- Existing contract/unit suites pass; a new contract test compares
  store-sourced vs freshly-observed context contents for a mocked frame.

---

## Phase 3 — Change propagation and invalidation events

Goal: project edits produce an explicit changed-node set that flows to the
scheduler and the GUI; stale store entries are garbage-collected.

### Task 3.1 — Fingerprint diffing on project mutation

- On every DAG rebuild, compute old-vs-new `NodeFingerprintMap` diff in the
  presenter layer and produce an `ObservationInvalidation` value (changed
  node IDs; removed fingerprints). Downstream propagation is inherent in the
  fingerprint composition — no separate graph walk.

**Acceptance criteria**

- Unit tests: parameter edit yields exactly the edited node + descendants;
  topology edit yields the affected chain; no-op save yields an empty diff.

### Task 3.2 — Store garbage collection

- After a diff, call `retain_only()` with the current DAG's fingerprint set
  plus a bounded set of recently unreachable fingerprints (so undo restores
  cached observations), evicting the rest by budget.

**Acceptance criteria**

- Unit tests: undo of a parameter change reuses stored observations (zero
  observer runs); fingerprints unreachable beyond the retention window are
  evicted.

### Task 3.3 — Presenter invalidation notifications

- Add subscribe/unsubscribe for observation invalidation events on the
  observation presenter API (plain callback, view-types payload only — MVP
  rules forbid Qt in presenters' core-facing side). GUI marshals to the main
  thread in its coordinator.

**Acceptance criteria**

- `gui-model` tier tests: callback fires with correct node set on project
  edit; unsubscribe stops delivery; no GUI header leaks into presenters
  (`ctest -R MVPArchitectureCheck` passes).

---

## Phase 4 — Background observation scheduler

Goal: missing observations are computed ahead of demand on a worker thread,
with interactive requests taking priority.

### Task 4.1 — Observer statefulness metadata

- Add a `stateless` flag to `ObserverRegistryEntry`
  (`orc/core/core_observation_service.cpp:49`) and surface it via
  `ObserverInfo`
  (`orc/sdk/include/orc/stage/observation/observation_service_interface.h:40`).
  Classify all nine observers (e.g. `closed_caption` and
  `colour_frame_phase` carry cross-frame state; `white_snr` does not).
- SDK header change: follow `AGENTS.md` §9. Do **not** bump
  `kStagePluginHostAbiVersion`/`kStagePluginApiVersion` — this branch has
  already bumped the ABI to 11 and that bump has not shipped; fold this
  change into the existing version-11 entry in `orc/sdk/abi_history.yaml`,
  regenerate docs with `tools/gen_abi_history_docs.sh`, and keep the
  `ctest -L sdk` gates green.

**Acceptance criteria**

- Registry/contract tests assert the flag for each observer; SDK gates pass.

### Task 4.2 — ObservationScheduler core

- Add `orc/core/observation_scheduler.{h,cpp}`: one worker thread owning a
  dedicated `DAGExecutor`/`DAGFrameRenderer` and per-observer handles.
  Work items are `(node_id, frame range, observer set)`; a priority queue
  orders interactive requests above prefetch above sweep.
- Stateful observers process frames in ascending order within a work item;
  stateless observers may be chunked freely.
- Cancellation: DAG change aborts in-flight and queued work referencing stale
  fingerprints before requeueing against the new map. All stage/decode
  exceptions are caught at the work-item boundary and reported as item
  failures — the worker thread never exits the process.

**Acceptance criteria**

- Unit tests (mocked renderer/service): priority ordering; cancellation
  drops stale items; stateful ordering preserved; failure of one item does
  not stop the worker; clean shutdown joins the thread.

### Task 4.3 — Scheduling policy

- Inject an `IObservationSchedulingPolicy` deciding what to enqueue:
  (1) the interactively requested frame, (2) a prefetch window around the
  current preview position, (3) a background sweep of nodes of interest
  (previewed node, nodes with open observer dialogs). On invalidation
  (Phase 3), re-enqueue only changed nodes' frames.

**Acceptance criteria**

- Unit tests with a scripted policy: sweep enqueued on project load;
  invalidation re-enqueues only affected nodes; prefetch window follows the
  preview position.

### Task 4.4 — Progress and completion reporting

- Scheduler exposes per-node progress (frames observed / total) and
  completion callbacks; presenter forwards them alongside Phase 3
  notifications.

**Acceptance criteria**

- Unit tests: monotonic progress; completion fires exactly once per work
  item; callbacks never fire after shutdown.

---

## Phase 5 — Non-blocking GUI consumption

Goal: observer dialogs and metrics read stored data instantly and update
live; no synchronous render-on-request from the UI thread.

### Task 5.1 — Async presenter request API

- Add `requestObservations(node_id, field_id, callback)` to the observation
  presenter surface: answered immediately from the `ObservationStore` when
  present, otherwise enqueued at interactive priority with the callback fired
  on completion. Include request-generation tracking so stale responses are
  suppressed (same semantics required of `RenderCoordinator` by
  `AGENTS.md` §4.5).

**Acceptance criteria**

- `gui-model` tests: immediate answer on store hit; deferred answer on miss;
  stale-response suppression when the requested field changes mid-flight;
  clean shutdown.

### Task 5.2 — Dialog and widget integration

- Convert `VideoParameterObserverDialog`, `NtscObserverDialog`
  (`orc/gui/mainwindow.cpp:4835-4998`), the quality-metrics widget, and VBI
  display to the async API: render stored values immediately, show a pending
  state otherwise, update on completion/invalidation notifications marshalled
  to the main thread.

**Acceptance criteria**

- `gui-widget` offscreen smoke tests for each dialog's pending → populated
  transition; no UI-thread DAG execution in these paths (verified by
  presenter-boundary mocks).

### Task 5.3 — Analysis and cc sinks reuse stored observations

- Where a triggered sink drives standard observers over frames the store
  already covers (e.g. `cc_sink`,
  `orc/plugins/stages/cc_sink/cc_sink_stage_deps.cpp:190-192`), pre-load
  stored values into the trigger's `ObservationContext` and skip re-running
  those observers for covered fields, via existing SDK interfaces only
  (`AGENTS.md` §9 — expand the SDK if a capability is missing rather than
  bypassing it).

**Acceptance criteria**

- Sink unit tests: with a fully covered store, the sink runs zero observer
  frames and produces identical output; with partial coverage, only missing
  frames are observed.

### Task 5.4 — Status-line background progress indication

- Aggregate the scheduler's per-item reporting (`ObservationProgress` /
  `ObservationCompletion`, `orc/core/include/observation_scheduler.h`) into an
  overall workload snapshot: frames observed vs frames total across all
  outstanding work items, exposed by the scheduler (or a thin core-side
  aggregator fed by the Task 4.4 callbacks) and resetting to idle when the
  queue drains. Newly enqueued work while active may lower the percentage —
  the value reflects the current outstanding workload, not a monotonic
  session total.
- Forward the aggregate through the observation presenter as a view-types
  payload (active flag, percent complete, outstanding node count) using the
  same plain-callback mechanism as the Phase 3 / Task 4.4 notifications
  (MVP rules: no Qt on the presenters' core-facing side).
- `MainWindow` subscribes via its coordinator, marshals updates to the main
  thread, and shows a processing percentage in the window status bar
  (existing `statusBar()` usage in `orc/gui/mainwindow.cpp`) whenever
  background observation work is in progress — e.g.
  "Computing observations… 42%" — clearing the message when the aggregate
  returns to idle. Message formatting lives in a Tier 1 testable helper.

**Acceptance criteria**

- Core unit tests: percentage stays within 0–100; idle is reported exactly
  when the queue drains; invalidation purges (Phase 3) reset the aggregate;
  no callbacks after shutdown.
- `gui-model` tests: coordinator observes idle → active(percent) → idle
  transitions delivered on the main thread; unsubscribe/shutdown stops
  delivery.
- `gui-logic` tests: status-line formatting helper (percentage rounding,
  idle → empty message).

---

## Phase 6 — Persistent observation sidecar

Goal: observations survive application restarts; reopening an unchanged
project shows observer data immediately.

### Task 6.1 — SQLite sidecar schema

- Define a sidecar database stored beside the project file
  (SQLite is already a project dependency — `vcpkg.json`): table of
  observation records `(node_fingerprint, field_id, observer_id,
  observer_version, namespace, key, value_type, value)` plus a schema-version
  table. Access wrapped behind an `IObservationPersistence` interface in
  `orc/core` so the store is testable without a database.

**Acceptance criteria**

- Unit tests against a mock persistence interface; schema documented in the
  header; MVP boundaries unchanged.

### Task 6.2 — Write-behind persistence and warm start

- `ObservationStore` writes completed records to the sidecar asynchronously
  (batched, off the UI and scheduler-critical path) and, on project open,
  loads records matching the current fingerprint map so the GUI starts warm.
  Fingerprint mismatch (edited pipeline or changed source file) naturally
  loads nothing for affected nodes.

**Acceptance criteria**

- Functional-tier test (`functional` label): open → observe → close → reopen
  round-trip restores observations with zero observer runs for unchanged
  fingerprints; a modified source file (mtime/size) yields cold entries for
  its downstream nodes only.

### Task 6.3 — Sidecar lifecycle

- Garbage-collect persisted records whose fingerprints have been unreachable
  beyond a retention cap; on corruption or schema mismatch, discard and
  rebuild the sidecar (log a warning, never fail project open); observer
  version bumps invalidate that observer's records only.

**Acceptance criteria**

- Unit tests via the persistence interface: GC removes only unreachable
  records; corrupted-database path recovers by rebuilding; version bump of
  one observer leaves other observers' records intact.
