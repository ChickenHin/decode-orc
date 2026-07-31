# Plugin manager harmonization (GUI ↔ CLI)

Bring `orc-gui`'s Plugin Manager and `orc-cli plugins` to the same capability
set and the same vocabulary, and give script-only users the plugin and stage
introspection requested in
[issue #226](https://github.com/simoninns/decode-orc/issues/226).

## Scope

In scope:

- Every plugin action available in one front end is available in the other.
- Both front ends use one set of user-facing words for the same concept.
- Both front ends derive state (will-load, untrusted, ABI mismatch, missing)
  from one presenter-computed value rather than each re-deriving it.
- CLI gains stage/plugin introspection (parameters, description, instructions)
  so a script-only user can discover what a plugin offers.

Out of scope / explicit non-goals:

- **Do not reintroduce a "Trusted" column or a standalone trust control in the
  GUI.** Its removal was a deliberate UX decision: in the GUI, adding,
  installing and ticking **Enabled** are the trust-granting actions, each
  gated by the trust warning. Harmonization moves the CLI toward that model
  and keeps `plugins trust` / `plugins untrust` only as scriptable primitives.
  See the `trust_state` row in
  [`docs/technical/plugin-architecture.md`](../docs/technical/plugin-architecture.md).
- No registry YAML schema changes and no curated-index schema changes
  (`orc-plugin-registry/`). This work is presentation and command surface only.
- No change to when plugins are loaded (still next application launch).

## Selector contract (cross-cutting requirement)

Every identifier a command **prints** must be accepted verbatim as the
identifier another command **takes**. Listing output is machine input; a user
or script must never have to transform, guess or hand-assemble a selector.

Rules that every phase below is held to:

- **One selector type per object.** A plugin is addressed by its
  *plugin selector*; a stage by its *stage name*; an index entry by its
  *index id*. No command invents a fourth.
- **Round trip.** For every line `plugins list` emits, the selector on that
  line is accepted by `plugins info`, `enable`, `disable`, `trust`, `untrust`,
  `remove` and `update`. For every line `plugins search` emits, the selector
  is accepted by `plugins info` and `install`. For every line `stages list`
  emits, the name is accepted by `stages info`, `stages help`, and by
  `--source/--filters/--sink`.
- **Never print a non-selector where a selector belongs.** Today
  [`command_plugins.cpp`](../orc/cli/command_plugins.cpp) prints the literal
  `<unnamed>` for entries with an empty `plugin_id` — a string that no command
  accepts. Placeholder text is banned from selector fields; an entry with no
  id prints a path-based selector instead.
- **Ambiguity is an error, not a guess.** If a selector matches more than one
  entry, the command fails with the not-found/ambiguous exit code and lists
  the unambiguous selectors to choose from.
- **The GUI resolves identity the same way.** The GUI's id + path + asset-URL
  removal and the CLI's selector resolve through one presenter function, so
  the two front ends can never disagree about which entry a user meant.
- **JSON carries the selector explicitly.** Every JSON object that describes an
  addressable thing includes a `selector` field holding exactly what to pass
  back — a script should need `.selector`, never string surgery on other
  fields.

## Current state

Capability parity as implemented today:

| Capability | GUI ([`pluginmanagerdialog.cpp`](../orc/gui/pluginmanagerdialog.cpp), [`pluginbrowsedialog.cpp`](../orc/gui/pluginbrowsedialog.cpp)) | CLI ([`command_plugins.cpp`](../orc/cli/command_plugins.cpp)) |
|---|---|---|
| List registered plugins | Table (ID, Path, Version, Update, Source, Enabled) | `plugins list` (id, path, version, license, enabled, trusted, core, exists, loaded, host ABI) |
| Hide core plugins by default | Yes — **Show core plugins** unticked | No — always lists core entries |
| Show update status inline | Yes — **Update** column, background check on open | Separate `plugins updates` command |
| Add from local file | **Add Plugin… → Local plugin file** | `plugins add <path>` |
| Add from GitHub releases URL | **Add Plugin… → Remote GitHub releases URL** | **Missing** |
| Install from curated index | **Browse Plugins…** dialog | `plugins search` + `plugins install` |
| Browse index with no search term | Yes — full list on open | No — `search` requires a term |
| Remove | **Remove** (by id + path + asset URL) | `plugins remove <id>` (id only) |
| Enable / disable | **Enabled** checkbox | `plugins enable` / `plugins disable` |
| Trust granted with an explicit warning | Yes, on add / install / enable / post-update | **No** — `plugins trust` records trust silently |
| Untrust | No (by design) | `plugins untrust <id>` |
| Update one plugin | **Update** button (confirm → download → trust prompt) | `plugins update <id>` (records untrusted) |
| Update all | No | No |
| Details for an index entry | Right-hand details pane | `plugins info <id>` |
| Details for an installed entry | No | No (`info` is index-only) |
| Plugin load diagnostics | Logged; error dialog at startup only | Printed by `process` / `filter` runs only |
| Stage list / parameters / instructions | Stage menu, `StageParameterDialog`, `stage_help_dialog` | **Missing** |
| Machine-readable output | n/a | **Missing** |

Terminology divergences to remove:

| Concept | GUI today | CLI today | Canonical term |
|---|---|---|---|
| Curated index | "Browse Plugins" | "search the curated plugin index" | **Available plugins** (index); "browse" is the GUI verb for it |
| Local registry | "Registry" | "registry" | **Installed plugins** in prose; `registry` only for the file itself |
| Will load next launch | **Enabled** tick (enabled AND trusted) | `enabled: yes` + `trusted: yes`, unfused | **Enabled** = will load at next launch |
| Not yet confirmed | unticked Enabled + tooltip | `trusted: no` | **Not trusted yet** |
| Wrong ABI | "⚠ … must be rebuilt for ABI N" on Version | "needs rebuild for ABI N" | **Needs a rebuild for Orc ABI N** |
| Bundled plugin | "core plugins" | `core: yes` | **Core plugin** |
| Where it came from | **Source** column | not shown | **Source** |
| Post-change note | "Registry changes take effect on the next application launch." | same sentence | keep, single-sourced |
| Trust warning | "Warning! Plugins execute code locally on your computer - Are you sure you trust the source and author of this plugin?" | none | keep this text, single-sourced |

---

## Phase 1 — Canonical vocabulary and one status derivation

### Task 1.1 — Single-source the user-facing plugin strings

Add `orc/view-types/plugin_ux_strings.h` holding the canonical strings: trust
warning, restart/relaunch note, load-state labels, index-status labels
(`offline — showing the last cached index`, `no compatible build for this
host`, `already installed`), and field labels used by both front ends.

- [`orc/gui/plugintrustdialog.cpp`](../orc/gui/plugintrustdialog.cpp),
  [`orc/gui/pluginmanagerdialog.cpp`](../orc/gui/pluginmanagerdialog.cpp),
  [`orc/gui/pluginbrowsedialog.cpp`](../orc/gui/pluginbrowsedialog.cpp) and
  [`orc/cli/command_plugins.cpp`](../orc/cli/command_plugins.cpp) consume the
  header instead of holding literals.

**Acceptance criteria**

- No plugin-facing user string is spelled out in more than one translation
  unit; `grep` for "execute code locally" and "next application launch" hits
  only the new header.
- The header lives in `orc/view-types` (allowed for both `orc/gui` and
  `orc/cli` per the module table in [`AGENTS.md`](../AGENTS.md) §8) and pulls
  in no Qt.
- `ctest -R MVPArchitectureCheck` passes.

### Task 1.2 — Presenter-computed load state

Extend `PluginRegistryEntryInfo` in
[`orc/presenters/include/project_presenter_types.h`](../orc/presenters/include/project_presenter_types.h)
with `PluginLoadState load_state` (`WillLoad`, `Disabled`, `NotTrusted`,
`AbiMismatch`, `FileMissing`, `Core`) and `std::string load_state_detail`,
computed once in `ProjectPresenter::readPluginRegistry()`.

- GUI: the **Enabled** check state, its tooltip and the Version "⚠" come from
  `load_state`, not from re-deriving `is_core_plugin || trust_state ==
  "trusted"` inline.
- CLI: `plugins list` prints a `status:` line rendered from the same value.

**Acceptance criteria**

- The strings `"trusted"` and `required_host_abi` comparisons no longer appear
  in `orc/gui` or `orc/cli`; both read `load_state`.
- New core unit tests (`orc-tests/core/unit`, label `unit`) cover each state,
  including core-plugin exemption from the ABI gate and the enabled-but-
  untrusted case.
- A `gui-model` test asserts the dialog's tick state follows `load_state`.

### Task 1.3 — One selector, resolved in the presenter

Add plugin selectors to the presenter layer so both front ends address entries
identically:

- `PluginRegistryEntryInfo` gains `std::string selector` — the canonical
  handle for the entry, populated by `readPluginRegistry()`. It is the
  `plugin_id` when non-empty, otherwise `path:<path>`, otherwise
  `url:<release_asset_url>`. It is never empty and never a placeholder.
- `IProjectPresenter` gains
  `PluginSelectorResolution resolvePluginSelector(const std::string&) const`
  returning the matched entry, `NotFound`, or `Ambiguous` with the candidate
  selectors. Bare paths and asset URLs resolve without the prefix too, so
  copy-pasting a `path:` line's payload also works.
- Every mutating presenter call (`setPluginEnabled`, `setPluginTrusted`,
  `removePluginEntry`, `updatePluginToLatestRelease`) accepts a selector, not
  just a `plugin_id`.

**Acceptance criteria**

- No caller resolves plugin identity itself; `orc/gui` and `orc/cli` pass
  selectors straight through.
- Core unit tests (label `unit`) cover: id selector, `path:` selector, bare
  path, `url:` selector, not found, and two entries sharing a path resolving
  as `Ambiguous` with both candidates listed.
- `selector` is non-empty for every entry the presenter returns, including
  id-less and core entries — asserted in a test over a mixed fixture registry.

### Task 1.4 — One removal seam through the selector

Add `removePluginEntry(selector)` to
[`IProjectPresenter`](../orc/presenters/include/i_project_presenter.h), backed
by the existing `ProjectPresenter::removePluginRegistryEntry`. Route
`PluginManagerModel` through it (today
[`pluginmanagerdialog.cpp`](../orc/gui/pluginmanagerdialog.cpp) calls the
static directly, bypassing the mockable seam), have the GUI carry the row's
`selector` in its item data instead of the current id/path/asset-URL triple,
and make `plugins remove <selector>` use the same call.

**Acceptance criteria**

- The GUI dialog contains no direct `ProjectPresenter::` static calls for
  registry mutation; all go through `PluginManagerModel`.
- `orc-cli plugins remove path:<path>` removes an entry with an empty
  `plugin_id`, which is currently unreachable from the CLI, and removes the
  same entry the GUI's **Remove** button removes for that row.
- `plugins remove --dry-run <selector>` resolves and reports the entry that
  *would* be removed without writing the registry; the round-trip gate in
  Task 6.5 relies on it to check every printed selector non-destructively.
- `gui-model` test with a mock presenter verifies the selector passed for a
  selected row; a `unit;cli` ctest covers the `path:` form.

---

## Phase 2 — CLI reaches GUI parity for registry management

### Task 2.1 — Add from a GitHub releases URL

`plugins add --url <releases-url>` calling `addPluginFromUrl`, mirroring the
GUI's "Remote GitHub releases URL" mode. `plugins add <path>` stays the local
form; passing both is a usage error.

**Acceptance criteria**

- `orc-cli plugins add --url https://github.com/…/releases` records the same
  registry entry the GUI records for the same URL.
- Usage text lists both forms; `plugins add` with neither fails with a message
  naming both.
- `unit;cli` ctests for the usage-error paths (no network in tests).

### Task 2.2 — Trust confirmation parity

`add`, `install`, `update` and `enable` show the canonical trust warning from
Task 1.1 and require confirmation before recording trust, matching the GUI.

- `--yes` skips the prompt for scripted use.
- When stdin is not a TTY and `--yes` is absent, the command fails with a
  message naming `--yes` rather than silently proceeding or hanging.
- `enable` on an entry whose `load_state` is `NotTrusted` grants trust after
  confirmation (this is exactly what ticking **Enabled** does in the GUI).
- `plugins trust` / `plugins untrust` remain, documented as explicit
  primitives, and also honour the prompt/`--yes` rule.

**Acceptance criteria**

- `install` no longer leaves an entry untrusted after an interactive run; the
  "trust it before it will be downloaded" follow-up note disappears from the
  confirmed path and is printed only when the user declines.
- Declining the prompt records nothing and exits non-zero with the trust
  refusal code from Task 5.2.
- `unit;cli` ctests: non-TTY without `--yes` fails; `--yes` path proceeds.

### Task 2.3 — `plugins list` shows what the GUI table shows

- Hide core plugins by default; `--core` (or `--all`) includes them, matching
  the GUI's unticked **Show core plugins**.
- Add `source:` (release asset URL, else source repo URL, else path, else
  `Core`) and `status:` (Task 1.2) fields.
- `--check-updates` adds an `update:` field per entry using the same labels as
  the GUI **Update** column; without the flag no network request is made.
- Keep `license`, `exists`, `loaded`, `host ABI` — these have no GUI
  equivalent yet and are picked up by Phase 3.
- Print the Task 1.3 `selector` as the entry's first field, replacing the
  current `id:` line that can read `<unnamed>`. Show `id:` separately only
  when it differs from the selector.

**Acceptance criteria**

- Default `plugins list` output and the default GUI table list the same
  entries for the same registry.
- Every selector printed is accepted unchanged by `plugins info`, `enable`,
  `disable`, `trust`, `untrust`, `remove` and `update` — pinned by the
  round-trip gate in Task 6.4.
- The string `<unnamed>` no longer appears in any output.
- The default path performs no network I/O (verify with an unreachable
  proxy/env in a `unit;cli` test asserting prompt-fast exit).
- The behaviour change (core plugins hidden by default) is called out in the
  CLI user guide and in the command's own `--help`.

### Task 2.4 — Browse-equivalent search and bulk update

- `plugins search` with no term lists every index entry (the GUI Browse dialog
  opens on a full list); the term stays optional, not required.
- `--installed` / `--available` / `--compatible` filters.
- `plugins update --all` updates every entry reporting `UpdateAvailable`,
  applying the Task 2.2 confirmation once for the batch.

**Acceptance criteria**

- Entry annotations use the canonical labels — `installed`, `incompatible`,
  `unreachable` — identical to
  [`pluginbrowsedialog.cpp`](../orc/gui/pluginbrowsedialog.cpp)'s list labels.
- The offline/cached banner text matches the GUI status banner exactly
  (both from Task 1.1).
- The index id printed per entry is accepted unchanged by `plugins info` and
  `plugins install`; for an entry that is already installed, the same string
  also resolves as a registry selector, so `search` output feeds `enable` and
  `update` directly.
- `unit;cli` ctest: `plugins search` with no argument exits 0.

---

## Phase 3 — GUI reaches CLI parity for inspection

### Task 3.1 — Details for an installed plugin

Add a details pane (or **Details…** action) to the Plugin Manager showing the
selected registry entry with the same field names and order as
`plugins info`: id, name, version, license, source, path, exists, loaded,
required/host ABI, status, update status.

**Acceptance criteria**

- Every field printed by `orc-cli plugins info <installed-id>` appears in the
  GUI pane with the same label text.
- `gui-widget` smoke test plus a field-population test against a mock
  presenter.

### Task 3.2 — `plugins info` covers installed plugins

Extend `plugins info <id>` to fall back to the local registry when the id is
not in the curated index, so both front ends can describe the same set of
plugins. Index-only fields are omitted rather than shown empty.

**Acceptance criteria**

- `plugins info` accepts any selector `plugins list` prints and any id
  `plugins search` prints, and echoes the canonical selector in its output so
  the result is itself usable as input.
- `plugins info` on a locally added plugin (never indexed) succeeds and prints
  registry-derived fields.
- Unknown id in neither source fails with a message naming both places
  searched; an ambiguous selector fails listing the candidates.
- `unit;cli` ctest for the unknown-id failure.

### Task 3.3 — Plugin diagnostics in the GUI

Surface `listPluginDiagnostics()` in the Plugin Manager (a collapsible
"Diagnostics" section), matching what
[`command_process.cpp`](../orc/cli/command_process.cpp) and
[`command_filter.cpp`](../orc/cli/command_filter.cpp) already print, and add
`plugins doctor` to the CLI printing the same list plus the search paths from
`listPluginSearchPaths()`.

**Acceptance criteria**

- The same diagnostic message text appears in both front ends for the same
  registry state.
- Severity wording (`Info` / `Warning` / `Error`) is shared via Task 1.1.
- `gui-widget` test with a mock presenter returning one diagnostic of each
  severity; `unit;cli` ctest that `plugins doctor` exits 0 on a clean registry.

### Task 3.4 — Browse dialog wording alignment

Align the Browse dialog's labels, details pane and status banner to the
canonical strings, and show the installed-version/update relationship for
entries already installed (currently only "Already installed").

**Acceptance criteria**

- `plugin_browse_dialog_test.cpp` extended to assert label text comes from the
  shared strings header.
- An installed-but-outdated index entry reads the same in both front ends.

---

## Phase 4 — Stage introspection for script-only users (issue #226)

### Task 4.1 — `orc-cli stages list`

New `stages` command group backed by `listAllStages()` /
`listAvailableStagesForFormat()`. Options: `--kind source|filter|sink`,
`--plugin <id>`, `--format NTSC|PAL|PAL-M`, `--core` / `--all` using the same
default-hide rule as `plugins list`.

**Acceptance criteria**

- Output lists stage name, display name, kind, owning plugin id and whether it
  is core, using the same wording as the GUI stage menu categories
  ([`node_type_helper.h`](../orc/gui/node_type_helper.h)).
- The stage name printed is the internal `StageInfo::name` — the exact token
  `stages info`, `stages help` and `--source/--filters/--sink` accept. Display
  names are shown in a separate field and never in the name column.
- The owning plugin id printed is a valid plugin selector, so
  `stages list --plugin <id>` and `plugins info <id>` take the same string.
- Works with `--safe-core-plugins`.
- `unit;cli` ctest asserting a known core stage (for example `tbc_source`)
  appears.

### Task 4.2 — `orc-cli stages info <stage>`

Print the stage description plus every parameter from `getStageParameters()`:
name, type, default, min/max, allowed values, required flag, dependency
(`depends_on`), and the file-extension/output hints — the same descriptor data
[`stageparameterdialog.cpp`](../orc/gui/stageparameterdialog.cpp) renders.

**Acceptance criteria**

- Parameter display names and descriptions are byte-identical to what the GUI
  dialog shows for the same stage.
- Numbers that the GUI presents 1-based (frame/line numbers) are presented the
  same way here, via the `frame_numbering.h` helpers — the CLI must not print
  raw 0-based values where the GUI prints 1-based ones.
- Unknown stage name fails with the not-found exit code and lists near matches.

### Task 4.3 — Paste-ready parameter output

`stages info <stage> --yaml` emits a `.orcprj`-shaped parameter block with
defaults filled in; `--filtergraph` emits the `stage=key=value:key=value` form
used by `--source/--filters/--sink`.

**Acceptance criteria**

- The `--yaml` block loads unmodified when pasted into a project file's node
  parameters.
- The `--filtergraph` string runs unmodified through the parser in
  [`filtergraph_parser_test.cpp`](../orc/cli/filtergraph_parser_test.cpp);
  add a round-trip case there.

### Task 4.4 — `orc-cli stages help <stage>`

Print the stage's `instructions.md` via `getStageInstructions()` — the same
source the GUI help dialog renders
([`stage_help_dialog.cpp`](../orc/gui/stage_help_dialog.cpp)), read at runtime
from beside the plugin binary per [`AGENTS.md`](../AGENTS.md) §9.1.

**Acceptance criteria**

- Output for a core stage is identical to the Markdown the GUI dialog loads.
- A stage with no `instructions.md` produces a clear "no instructions shipped
  with this stage" message and a non-zero exit, not an empty success.
- `unit;cli` ctest against a core stage.

---

## Phase 5 — Scripting contract

### Task 5.1 — `--json` output

Add `--json` to `plugins list`, `plugins search`, `plugins info`,
`plugins updates`, `plugins doctor`, `stages list` and `stages info`. One
object or array per command, field names matching the presenter type field
names in
[`project_presenter_types.h`](../orc/presenters/include/project_presenter_types.h).

#### Why JSON when the rest of the design emits YAML

The two formats serve opposite directions, and the split is a rule rather than
an inconsistency: **YAML is what Orc parses back; JSON is what a script parses
and Orc never reads.** `stages info --yaml` and `--filtergraph` (Task 4.3) are
paste-ready fragments consumed by the project loader and the filtergraph
parser, so they must be in the formats those readers accept. The `--json`
surface is query output — nothing in Orc ever reads it.

Given that direction, JSON is the format that holds the selector contract:

- **Lossless scalars.** A YAML 1.1 reader coerces unquoted scalars: a version
  `1.10` becomes the float `1.1`, an id or tag `no`/`on`/`y` becomes a boolean,
  a leading-zero build id becomes octal. The contract in
  [Selector contract](#selector-contract-cross-cutting-requirement) requires a
  script to feed `.selector` back with no string surgery, so a format with one
  string form and quoted keys is the one that satisfies it by construction.
- **No new dependency, and no risk of a hand-rolled emitter being wrong.**
  `orc-cli` links only `orc-presenters` and `orc-common`
  ([`orc/cli/CMakeLists.txt`](../orc/cli/CMakeLists.txt)); `yaml-cpp` arrives
  via `orc-core`, which is PRIVATE to `orc-presenters` by MVP enforcement. A
  YAML mode would either breach that boundary or need a hand-written emitter,
  and YAML's quoting rules are context-dependent. JSON escaping is one small
  fully-specified function, so the writer lives in `orc/cli` with no new link.
- **Ubiquitous readers.** `python3 -m json.tool` and `jq` need nothing
  installed; YAML needs PyYAML or `yq`. Task 6.5's selector round-trip gate has
  to run in the default `unit` lane on any machine, and
  [`check_plugin_index.sh`](../cmake/check_plugin_index.sh) already shows the
  cost of a PyYAML-dependent gate.

Every YAML advantage — comments, anchors, block scalars — serves human
authorship, and the human-facing mode here is the aligned table, not YAML.

#### One source, two projections

The single data structure the human and JSON modes share is the **`*Info`
struct**, not the rendered field list:

| Command | Struct |
|---|---|
| `plugins list`, `plugins info` (installed part) | `PluginRegistryEntryInfo` |
| `plugins search`, `plugins info` (index part) | `PluginIndexEntryInfo` |
| `plugins updates`, `plugins info` (update part) | `PluginUpdateStatusInfo` |
| `plugins doctor` | `PluginDiagnosticInfo` |
| `stages list`, `stages info` | `StageInfo` + `ParameterDescriptor` |

`makePluginDetails()`
([`plugin_details.h`](../orc/presenters/include/plugin_details.h)) and
`makeStageDetails()` / `makeStageParameterDetails()`
([`stage_details.h`](../orc/presenters/include/stage_details.h)) are the
**human** projection of those structs; the JSON writer is a sibling projection
of the same structs. JSON is never serialised from the `PluginDetailField` /
`StageDetailField` list — those hold canonical *display labels* ("Plugin ID"),
pre-rendered strings, omitted-when-empty entries and 1-based index conversion,
so serialising them would emit label-keyed, type-erased output and contradict
the stable-identifier rule below. Both projections read the same struct, so
neither can drift about *what* is described, while each stays free about how.

Consequences that follow from the projection rule and must be honoured:

- Enums are written with the existing stable-id helpers — `pluginLoadStateId()`
  ([`plugin_load_state.h`](../orc/presenters/include/plugin_load_state.h)),
  `pluginDiagnosticSeverityId()`, `stageKindId()` — not the `…Label()` pairs.
  Any enum reaching JSON without an `…Id()` helper gains one in the presenter,
  beside its label function.
- Booleans and numbers are emitted as JSON booleans and numbers, not as the
  `yes`/`no` words the table prints.
- Parameter defaults use `stageParameterStoredDefault()` — the **0-based**
  stored form, matching `--yaml` and `--filtergraph` — because JSON is
  machine-facing. The 1-based presentation form belongs only to the human
  projection, per the frame-numbering convention in
  [`AGENTS.md`](../AGENTS.md). Fields whose JSON and table values differ this
  way are called out in the CLI guide (Task 6.1).
- Fields the human projection omits when empty are still present in JSON, as
  `""`, `[]` or `null`, so a script sees one stable object shape per command.

**Acceptance criteria**

- Output parses with `python3 -m json.tool` in a `unit;cli` ctest.
- Human and JSON modes are produced from one data structure; no second
  formatting path can drift.
- JSON keys are the `*Info` struct field names. A `unit;cli` ctest asserts no
  key in any command's output contains a space or an upper-case letter, which a
  display label would.
- Enum values are emitted as stable lowercase identifiers, not display labels.
- `stages info --json` and `stages info --yaml` report the same default for
  every parameter, including indexed-spec parameters whose table value is
  1-based.
- Every object describing an addressable thing carries a `selector` field
  (plugins) or `name` field (stages) that the corresponding command accepts
  unchanged, so a script can feed `.selector` straight back in without string
  editing. A `unit;cli` ctest extracts every `selector` with `python3` and
  passes each to `plugins info`, expecting exit 0 for all of them.
- `orc-cli` gains no new link dependency for this task; `ctest -R
  MVPArchitectureCheck` passes.

### Task 5.2 — Exit-code contract

Define and apply: `0` success, `1` usage or unexpected error, `2` not found
(plugin/stage id), `3` network or index unavailable, `4` trust declined.
Document the table in the CLI user guide.

**Acceptance criteria**

- Every `plugins`/`stages` failure path returns a code from the table.
- `unit;cli` ctests pin at least the `2` and `4` paths.

### Task 5.3 — Non-interactive rules

`--yes` on every confirming command, honoured uniformly; no command blocks on
input when stdin is not a TTY.

**Acceptance criteria**

- Running each mutating command under `</dev/null` either completes (with
  `--yes`) or fails fast with a message naming `--yes` — never hangs.
- A `unit;cli` ctest exercises the redirected-stdin case for one command per
  family.

---

## Phase 6 — Documentation and a parity gate

### Task 6.1 — CLI user guide

Add a "Plugin management" section to
[`docs/cli-user-guide/overview.md`](../docs/cli-user-guide/overview.md)
covering every `plugins` and `stages` subcommand, the trust model, the exit
codes and `--json`, using the canonical terms.

**Acceptance criteria**

- Every subcommand in `command_plugins.cpp` usage text appears in the guide.
- Terms match the canonical column of the terminology table above.

### Task 6.2 — GUI user guide

Update
[`docs/gui-user-guide/dialogues/main.md`](../docs/gui-user-guide/dialogues/main.md):
document the **Browse Plugins…** dialog (currently undocumented), the details
pane (Task 3.1) and the diagnostics section (Task 3.3), and add the CLI
equivalent alongside each GUI action.

**Acceptance criteria**

- Each documented GUI action names its `orc-cli plugins …` equivalent.
- The Plugin Manager section's wording matches the CLI guide's wording for the
  same concept.

### Task 6.3 — Technical docs sync

Update [`docs/technical/plugin-architecture.md`](../docs/technical/plugin-architecture.md)
where it describes the CLI/GUI trust split, the update flow and the curated
index, per the doc-sync table in [`AGENTS.md`](../AGENTS.md) §9.

**Acceptance criteria**

- The `trust_state` row reflects the CLI's new confirmation prompt.
- No statement remains that a capability exists in only one front end unless
  that is still true (untrust, GUI restart prompt).

### Task 6.4 — Parity and round-trip regression gate

Add a capability manifest (`orc/plugin_ux_capabilities.yaml`: capability id,
CLI invocation, GUI control name) and a ctest that fails when a capability has
no CLI invocation matched in `orc-cli plugins --help` / `orc-cli stages
--help`, mirroring the existing `unit;cli` help-regex tests in
[`orc/cli/CMakeLists.txt`](../orc/cli/CMakeLists.txt).

**Acceptance criteria**

- Removing a subcommand from the CLI usage text fails the gate.
- A new capability added to only one front end fails the gate until the
  manifest records both sides.
- The gate runs in the default `unit` lane and needs no network or GUI.

### Task 6.5 — Selector round-trip gate

Add a `unit;cli` gate script that enforces the selector contract end to end
against a fixture registry (core plugins plus an id-less local entry, via
`XDG_CONFIG_HOME`/`HOME` redirection as the existing CLI tests already do):

- take every selector from `plugins list --json`, feed each to
  `plugins info`, `enable`, `disable` and a `remove --dry-run`, and require
  exit 0 from all of them;
- take every stage name from `stages list --json`, feed each to `stages info`
  and `stages help`, and require the name to parse as a filtergraph token;
- assert no output field matching the placeholder pattern (`<...>`) appears in
  any selector position.

**Acceptance criteria**

- The gate fails if any command's printed identifier is rejected by a command
  that takes one.
- No network access and no curated-index fetch (registry-only paths).
- A deliberate regression — printing a display name where a stage name belongs
  — is caught by the gate.

---

## Deviations settled during implementation

Where the built behaviour deliberately differs from a task's literal wording,
the behaviour below is the contract; the task text above is not updated
retroactively.

- **`untrust` never prompts** (Task 2.2). Only the granting direction asks the
  trust question; `untrust`, `disable` and `remove` take capability away and
  run silently. All of them still accept `--yes` so scripts can pass the flag
  uniformly.
- **The Plugin Manager details pane shows the registry projection only**
  (Task 3.1). Index-derived fields (`name`, `description`, `maintainer`,
  `tags`, `compatible`, `installed`) appear in `plugins info` when the id is
  indexed and in the Browse dialog's pane, but not in the Manager pane, which
  performs no network I/O. Field order is `makePluginDetails()`'s order for
  both front ends, which supersedes the order the Task 3.1 prose lists.
- **Load-state precedence**: `NotTrusted` outranks `Disabled` (trust is the
  decision still pending), and `FileMissing` is suppressed for
  `github_release_asset` entries whose binary is fetched at the next launch.
- **Selectors gained an `index:<n>` last-resort form** for hand-edited entries
  with no identity at all, keeping "never empty" true. A loaded plugin's
  runtime id also resolves onto the id-less registry row recording its path,
  through the same resolver every command (including `remove --dry-run`) uses.
- **`stages info --json` writes `kind`** (the stable id `--kind` accepts)
  rather than the `node_type` struct field name, and `allowed_strings` as
  `{value, label}` objects.
- **`--kind sink` lists everything the `--sink` slot accepts**, analysis sinks
  included; `--kind analysis` narrows to those alone.
- **`stages help` on a stage with no instructions exits 1, not 2** — the stage
  exists; the missing document is a documentation gap, not a lookup failure.
- **The `--yaml` block is emitted at node depth** (the indentation a node's
  `parameters:` key has in a written `.orcprj`), which is what makes the
  "loads unmodified when pasted" criterion literally true.
