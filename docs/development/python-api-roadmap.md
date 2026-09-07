# Python API & Heresy Editor Integration — Analysis and Roadmap

This document is a critical analysis of BiasedDoom's embedded Python API
and its integration with Heresy Editor: what hurts today, why, and what
to do about it. It now also tracks the shipped ImGui overlay and RPG
framework tracks (§10–§12). Each topic is structured as **Current state
→ Problem → Proposal → Priority/Effort**. Items marked *(done)* shipped
after this analysis and are kept for context.

---

## 1. The silent-failure problem (event coverage)

**Current state.** `line_activated` fires only when the line's special
*succeeds* (`P_ActivateLine`: `if (buttonSuccess) WorldLineActivated(...)`).
A marker special like `ACS_Execute` with no backing script fails, and
nothing observable happens — no event, no log line, no console warning.
Discovering this rule required reading `p_spec.cpp` after a map appeared
to "do nothing".

**Problem.** The most common beginner failure — a trigger that does
nothing — produces zero diagnostic signal at the exact moment it happens.
The editor now auto-generates BEHAVIOR stub scripts to keep specials
succeeding, but maps from any other source can still hit this, and stubs
only cover `ACS_Execute`.

**Proposal.** *(done)* A `line_activation_failed` event carrying
`line_index`, `special`, `args`, `activation_type`, and `actor_ref`.
Future extensions in the same vein:

- **Reason codes.** *(done)* `line_activation_failed` now carries
  `reason` (string) and `reason_code` (int): `none`, `unknown_special`,
  `script_not_found`, `locked`, `activation_filtered`,
  `insufficient_resources`. `P_ExecuteSpecial`'s bool is supplemented by a
  reason out-param (`Level->LastSpecialFailReason`).
- **ZScript parity.** *(done)* `WorldLineActivationFailed` is a static
  event handler virtual (folded into the §1 proposal above), so ZScript
  mods receive the same signal, including the reason code.
- **Pickup/inventory events.** *(done)* `item_dropped` and
  `weapon_changed` shipped alongside the existing `item_picked`, replacing
  per-tick polling for the common gameplay cases.
- **Sector events.** *(done)* `sector_entered`/`sector_exited` per player
  now fire on sector transitions, replacing the linedef-grid idiom.

**Priority.** §1 is fully shipped; remaining event-coverage ideas would be
new proposals.

## 2. Snapshot vs live-handle duality

**Current state.** Two parallel object models: `bd.actors()` /
`bd.spawn_actor()` return plain **snapshot dicts** (stale the moment they
are made), while `bd.actor_ref()` / `bd.spawn()` return **live handles**
(`Actor`, `Line`, `Sector`, `Player`) with properties and methods. The
snapshots predate the handles; the handles were "API v2".

**Problem.** Two ways to do everything confuses exactly the users the
Python API is for. `bd.actors(class_name="ZombieMan")` looks like it
returns usable objects; it returns dicts that silently go stale. The
documentation has to teach both models and when each is safe, which is
cognitive overhead that shows up as bugs ("I stored the dict and health
never changes").

**Proposal.**

- **Make handles the documented default.** *(done)* Every guide example
  in the querying chapter uses live handles; snapshots are documented in a
  "Legacy Snapshot API (JSON persistence)" section.
- **Snapshot ergonomics for save/load.** *(done)* Snapshots remain
  genuinely useful for JSON persistence (`bd.state`); the docs now present
  that as *the* snapshot use case, with an explicit `Actor.snapshot()`
  parity note.
- **Long-term (breaking, defer):** `bd.actors()` returning handles
  directly, snapshots available via `.snapshot()`. Defer until a major
  API bump; the dual model is tolerable if documented crisply.

**Priority.** Docs-first clarification is shipped; no breaking change
before API v3.

## 3. Testing infrastructure

**Current state.** *(done)* `-scripttest <tics>` runs a level for N
tics, prints `SCRIPT TEST: PASS/FAIL` with the Python error count, and
exits 0/1/2. `-pyerrorlog <file>` emits JSON-lines errors. Drive scripts
inject input through `bd.execute("+forward")` or `Player.set_input`.

**Problem.** PASS/FAIL on "no Python errors" catches crashes but not
wrong behavior: the ambush that doesn't spawn, the door that opens the
wrong sector. And real-time pacing (35 tics/s) makes long tests slow.

**Proposal.**

- **Assertions in scripts.** *(done)* `bd.assert_true(cond, "message")`
  fails the script test with file/line and composes with `-scripttest`
  exit codes.
- **Fast-forward.** *(done)* `-scripttest <tics> [ff]` raises the engine
  time scale so `ff` times as many tics elapse per wall second and renders
  only every 8th loop iteration; tic counting is unchanged and the boost is
  released for the final `ff` tics to avoid overshoot.
- **Golden screenshots.** *(done)* `tools/compare_screenshots.py` is a
  stdlib-only PNG pixel comparator (tunable per-channel tolerance and
  diff percentage, optional diff-map output) for the test harness;
  documented in the Python guide's CI chapter. `bd.execute("screenshot
  x")` remains the capture path.
- **Headless rendering.** *(done)* `-headless` (or
  `BIASEDDOOM_HEADLESS=1`) boots the engine on a null video driver
  (`NullVideo`/`NullFrameBuffer` in
  `src/common/rendering/nullvideo/`): SDL runs on the dummy video driver
  so the event pump stays alive, `D_Display` early-outs after GC/delta
  bookkeeping, and no GL/Vulkan code is ever touched. `-scripttest`
  (including fast-forward) passes with no X server; screenshots warn
  instead of crashing.

**Priority.** §3 is fully shipped.

## 4. Error ergonomics

**Current state.** Tracebacks print to console/stdout/logfile in red;
identical errors are deduplicated with periodic "repeated N times"
summaries; `print()` output is flushed before tracebacks so ordering is
natural. `-pyerrorlog` *(done)* gives tools a JSON feed; Heresy *(done)*
surfaces that feed in its log window via Script → Show Python Errors.

**Problem.** Console-first errors are fine in-game but weak in the
authoring loop: the mapper edits in VSCode, tests from Heresy, and the
error lives in a third place (the game console).

**Proposal.**

- **In-editor markers.** Heresy parses the `-pyerrorlog` JSON (it has
  file/line in the traceback) and jumps the external editor to the
  failing line, or annotates the status bar on sync. *Effort: medium.
  Impact: high.*
- **Structured error records.** *(done)* `-pyerrorlog` JSON lines now
  carry `severity`, `exc_type`, `exc_file`, `exc_line`, and `exc_func`
  alongside the preformatted traceback.
- **Runtime warnings channel.** *(done)* `bd.warn(msg)` prints a
  rate-limited `SCRIPT WARNING` without failing the script test; the same
  channel reports blocked mutations in multiplayer/demo sessions and
  stale-handle skips in `apply_actor_batch`.

**Priority.** In-editor error markers close the loop; everything else
is polish on an already-decent pipeline.

## 5. Performance and the C-crossing budget

**Current state.** Every event dispatch, query, and mutation crosses
the C++/CPython boundary. Whole-tic budgets with per-callback profiling
exist (`bd.profile()`), and `apply_actor_batch` batches mutations into
one crossing.

**Problem.** Query patterns are per-actor crossings: a script scanning
500 actors for a custom condition costs 500 crossings per tick. The
batch API covers mutations only.

**Proposal.**

- **Query batching / filtering in C.** *(done)*
  `bd.actor_refs(class_name=..., subclasses=..., sphere=(x, y, r), z=...)`
  pushes the common filters into the native thinker scan; combined with
  `bd.sector_at`/`bd.actors_in_sector` this eliminates most full-level
  scans.
- **Vectorized reads.** *(done)* `bd.actor_field_batch(refs, fields)`
  reads whitelisted fields for many actors in one crossing, complementing
  `apply_actor_batch`.
- **Document the budget model.** *(done)* See
  [python-performance.md](python-performance.md): crossing cost model,
  `bd.profile()` walkthrough, push-down/batching recipes, and a budget
  checklist.

**Priority.** All three shipped; further work here is measurement-driven.

## 6. Determinism, savegames, and multiplayer

**Current state.** `bd.state` is JSON-persisted into savegames with
`save`/`load` events for (de)hydration. Mutation APIs are hard-blocked
in multiplayer and demo playback. Actor handles can go stale across
load; `.valid` exists for detection.

**Problem.** `random()` from Python's stdlib is outside the engine's
deterministic RNG, so save/load and demo sync drift is possible for
gameplay-affecting scripts. Stale-handle detection exists but nothing
helps rebind handles after a load.

**Proposal.**

- **Engine-seeded RNG helper.** *(done)* `bd.random()`,
  `bd.randrange()`, `bd.randint()`, and `bd.choice()` draw from one
  deterministic, savegame-persisted engine stream; scripts are
  deterministic by default.
- **TID-based rebinding recipe.** *(done)* The canonical pattern is
  documented in the persistence chapter of `python.md`: keep TIDs in
  `bd.state`, re-resolve handles on `load` with `bd.actor_ref`, check
  `.valid`. Consider `bd.on_load_rebind(mapping)` sugar later.
- **Read-only observer mode for MP/demo.** *(done)* Queries plus local
  presentation (`center_message`, `set_music`, `hud_text`, `hud_clear`,
  `screen_flash`, `screen_fade`, `play_ui_sound`, `bd.draw_*`) are allowed
  in multiplayer/demo sessions via `PythonRuntime::CheckLocalPresentation()`;
  world mutations stay hard-blocked and emit a deduplicated script warning;
  `bd.session_read_only()` reports the mode. Useful for spectator overlays
  and replay analysis.

**Priority.** §6 is fully shipped for this API version; determinism and
savegame hygiene are now documented invariants.

## 7. Security and trust model

**Current state.** Python manifests are trusted same-container code with
full CPython (file I/O, `os`, sockets) — the model is explicitly
"trusted mod code", documented in the security section of the Python
docs.

**Problem.** Full stdlib access means a malicious or careless script can
do anything the user can. There is no middle ground between "no Python"
and "full Python".

**Proposal.** *(done — decision final)* Document, don't restrict. The
audience is mappers running their own scripts; over-restriction kills
the feature. The loud documentation and the `-python` opt-in are the
trust model.

**Eliminated.** The optional sandbox profile (`trusted=false` manifest
mode) is rejected, not deferred: CPython cannot be reliably sandboxed
(the stdlib is riddled with escape hatches, and a half-sandbox is worse
than none because it implies safety it cannot deliver), and the threat
model doesn't justify it — scripts are same-container code from the map
author, equivalent in trust to the ACS/ZScript and native code the
engine already loads. Downloaded untrusted maps are handled by the
`-python` opt-in plus documentation; users who don't trust a mod
shouldn't grant it the Python runtime.

**Priority.** Closed.

## 8. Heresy Editor integration

**Current state.** Sidecar `<map>.scripts/` workspace with ACS/ZScript/
Python parity templates; auto-sync on save; acc compilation; BEHAVIOR
stub generation for trigger args; script-actor introspection into the
thing browser; Draw Trigger Line; test-map auto-args (`-file`, `-python`,
`-debug`); *(done)* typings auto-install and Python error surfacing.

**Problem / next steps.**

- **Project-aware script docs.** Multi-map projects share patterns but
  each map gets an isolated sidecar; a project-level `scripts/` shared
  folder option would help campaigns. *Effort: medium. Impact: medium.*
- **In-editor error markers** (see §4). *Effort: medium. Impact: high.*
- **One-click "run script test"** driving `-scripttest` and reporting
  PASS/FAIL in the editor, reusing H2's plumbing. *Effort: low (the
  pieces exist). Impact: high — turns CI mode into a button.*
- **Actor registry awareness.** The thing browser's script-actor group
  could expose "insert spawn call" snippets using `bd.actors.*`
  constants. *Effort: low. Impact: medium.*
- **Stub sync policy.** Heresy bundles `common/biaseddoom.pyi`; refresh
  it from BiasedDoom's `docs/scripting/biaseddoom.pyi` at release time
  (the file is `dumppystub`-regenerable in-engine).

## 9. Single-sourcing: stub, docs, website

**Current state.** *(done)* `biaseddoom.pyi` ships in
`docs/scripting/`, its actor-constant block is engine-generated via
`dumppystub`, and the website's API reference is generated from the same
file.

**Problem.** Three audiences (engine docs, editor completions, website)
historically meant three copies to rot.

**Proposal.** Keep the pipeline strict: engine docstrings → `dumppystub`
→ `.pyi` → (Pylance, website API pages). The rule: *prose lives in
docstrings, never in generated files.* When a function's docstring is
too thin for the website, enrich the C++ docstring, not the .pyi.

**Priority.** This is now the invariant; guard it in release checklists.

## 10. Python-scripted Dear ImGui overlays

**Current state.** *(done)* Dear ImGui 1.92.8 is vendored in
`libraries/imgui/` (MIT, core only). The engine overlay layer
(`src/common/imgui/bd_imgui.cpp`, `namespace BdImGui`) runs an ImGui
frame inside `DrawOverlays()` and translates `ImDrawList` primitives
into `F2DDrawer` commands, so it works unchanged on OpenGL, Vulkan,
GLES, and softpoly. Input is captured on all platforms by OR-ing
`BdImGui::WantsGuiCapture()` into the `WantGuiCapture` system callback
(`d_main.cpp`), which every platform's input pump consults. The `bd.imgui` submodule (~60 functions: windows,
widgets, tables, style stacks, menus, tooltips, `image()`,
`plot_lines`, demo/metrics windows, master visibility) is driven by the
new `imgui_frame` event, gated by the `py_imgui` CVar and the
`BIASEDDOOM_ENABLE_IMGUI` CMake option, with the `py_imgui_demo` CCMD
as a smoke test. Example: `26_imgui_overlays`.

**Design rationale.** Immediate-mode UI matches how mod scripts already
think — rebuild the panel every frame from live state. Rendering
through `F2DDrawer` instead of a native ImGui backend keeps the layer
backend-agnostic: one translation point, no per-renderer shader code to
maintain.

**Problem / next steps.**

- **Docking branch.** *(done)* The vendored copy now tracks the docking
  branch (`v1.92.8-docking`); `bd.imgui.dock_space_over_viewport()`,
  `dock_space()`, and `set_next_window_dock_id()` expose it. Multi-viewport
  is deliberately not enabled — a single `F2DDrawer` canvas cannot host
  platform windows.
- **Richer `image()`.** *(done)* `image(texture, w, h, uv0, uv1, tint,
  border)` accepts texture names (with sprite-namespace fallback) or live
  Actor handles (current sprite frame), UV sub-rects, tint, and border;
  `image_size()` queries natural display size from any event.
- **Per-widget batching.** *(done — resolved without new code)*
  `F2DDrawer::AddCommand` already merges consecutive compatible commands
  (texture/scissor/style/transform), so ImGui's same-texture runs collapse
  automatically (~60–90 draw calls to roughly a quarter for dense panels);
  the analysis lives in a comment in `bd_imgui.cpp`.

**Priority.** The overlay is shipped and backend-complete; the remaining
items are ergonomics.

## 11. Conversation events and RPG frameworks (`bd_quests`, `bd_vtm`)

**Current state.** *(done)* `conversation_started` and
`conversation_reply` events hook `P_StartConversation` and
`HandleReply` in `p_conversation.cpp`, and `bd.player_log` /
`bd.set_player_log` / `bd.start_conversation` let scripts read and
drive dialogue state. On top of those primitives ship two engine-staged
framework packages under `src/python/lib/` (copied beside the embedded
stdlib by the always-run `stage_python_frameworks` CMake target):
`bd_quests` (data-driven Quest/Objective/QuestLog,
`track_kills`/`track_pickup`/`track_sector`/`track_conversation_log`
auto-wiring, `bd.state` persistence, ImGui `JournalUI`) and `bd_vtm`
(VtM-inspired chronicle rules: generation-based BloodPool with upkeep,
Hunger with deterministic frenzy checks, Humanity degeneration rolls,
data-driven Disciplines with celerity/obfuscate/potence/dominate
built-ins, `feed()` with witness detection, Masquerade violations,
Factions with a disposition matrix plus player reputation and
faction-gated `bd_quests` integration, `VtMState` persistence, ImGui
vitae HUD). Examples: `27_quest_journal`, `28_vtm_chronicle`.

**Design rationale.** The engine provides primitives (events, state,
RNG, ImGui); genre rules live in pure-Python packages so they iterate
at script speed, stay moddable by end users, and never grow the C++
API surface.

**Problem / next steps.**

- **Killer attribution on `actor_died`.** *(done)* The event payload now
  carries `attacker_ref` / `attacker_class` / `attacker_player_index` from
  `AActor::Die`'s `source` (plumbed through
  `EventManager::WorldThingDied(actor, inflictor, source)`), so kill credit
  is exact for player, monster, and infighting kills; ZScript sees the same
  value as `WorldEvent.DamageSource` on `WorldThingDied`.
- **Real Strife-map conversation coverage.** *(done)*
  `examples/python/30_conversation_quests` is a permanent fixture with a
  talkative NPC driven by engine-native ZSDF dialogue (a `DIALOG01` lump +
  MAPINFO `conversationids` + `LANGUAGE` log strings), asserting
  `conversation_started`/`conversation_reply` payloads and completing a
  `bd_quests` objective from a dialogue reply under `-scripttest`.
- **Console-alias toggles for framework UIs.** *(done)* The `pyui <name>`
  console command dispatches a generic `ui_command` event
  (`event["command"] == name`), so
  `bd.execute('alias toggle_journal "pyui journal"')` plus a
  `bd.on("ui_command")` handler is the shipped one-line pattern.

**Priority.** Both packages shipped; follow-ups are accuracy and test
hardening, not new surface.

## 12. Tabletop rules as a library (`bd_dnd`)

**Current state.** *(done)* `bd_dnd` (`src/python/lib/bd_dnd/`) ships
d20-style rules in pure Python: `roll()`/`d20()` with advantage on the
deterministic engine RNG, six abilities with modifiers, a 5e-style
skill table with proficiency, a `Character` with
`skill_check`/`saving_throw`/`award_xp`/`level_up`/`rest`/resources,
`track_xp_from_kills`, `LockedDoorCheck` on `line_activation_failed`
(`reason == "locked"`), `TrapZone` on `sector_entered`,
`DialogueSkillGate` on `conversation_reply`, `CharacterState`
persistence, and an ImGui `CharacterSheet`. Example: `29_dnd_dungeon`
(MAP02-based, including a real locked-door bash that grants a RedCard
on a successful Athletics check).

**Design rationale.** Same split as §11: the package composes shipped
API v2 primitives (reason codes, sector events, deterministic RNG,
ImGui). It doubles as validation that those primitives suffice for a
complete rules system, not just demos.

**Problem / next steps.**

- **Killer attribution** *(done — see §11)*.
- **Damage-typed saves.** *(done)* `DamageSaveRule` wires
  `actor_damaged` to saving throws (type filter, cooldown, retroactive
  half/negate-on-crit refunds — engine damage can't be cancelled
  post-hoc, documented).
- **Multi-character parties.** *(done)* `Party` roster + `PartyState`
  persistence (mutually exclusive with `CharacterState`) +
  `PartySheet` ImGui UI with selectable roster.
- **Party-shared resources and companions as world actors.** *(done)*
  `bd_dnd.Companion` binds party members to friendly follower actors
  (TID-based persistence through `PartyState`, follow/teleport AI on
  the deterministic scheduler, real combat targeting via the writable
  `Actor.target`, HP sync both ways, `revive()`). Party resources stay
  per-member by design.

**Priority.** Shipped; everything listed is package-level iteration.

---

## 13. Genre-agnostic combat core and script-authored dialogue

**Current state.** *(done)* Two native power-ups close the last engine
boundaries that forced RPG scripts into workarounds. The
`actor_before_damage` event fires at the top of `DoDamageMobj` — before
armor and damage factors — with a **mutable** event dict: handlers write
back `damage`, `damage_type`, or set `cancel` to negate the hit entirely
(cancelled damage never fires `actor_damaged`). The dispatch reuses the
standard event machinery, so the same dict object handlers receive is the
one the engine reads back; it is offline-only (skipped in multiplayer and
demos for determinism) and free when unregistered (one `HasCallbacks`
array read in the hottest gameplay function in the engine).
`bd.actor_data(ref)` / `bd.actor_data_drop(ref)` give every actor a
persistent script-owned dict, keyed by the handle registry's
slot/generation pair and purged from the same invalidation choke points
(`InvalidateActorSlot`, `InvalidateWorld`) that retire handles, so data
can never leak or reattach to the wrong actor. It is memory-only by
design; savegames stay a framework concern via the TID-rebind recipe.

On top of those primitives ship two more pure-Python packs.
`bd_rpg` (`src/python/lib/bd_rpg/`) is the genre-agnostic combat core:
a `DamageTypes` registry, per-actor and per-class elemental affinities
living in `actor_data`, a `resolve_attack` pipeline (hit check → crit →
dice → affinity → soak → `Actor.damage`, deliberately layered *under* the
native filter so scripts retain the last word), a `StatusEngine` with
refresh/stack/independent rules and built-in burning/poisoned/slowed/
stunned/regenerating effects driven by one consolidated scheduler task,
weighted `LootTable`/`LootRules` with exact player attribution and rarity
feedback, kill-XP glue, and `RpgState` save/load. Example:
`31_elemental_combat`. `bd_dialogue` (`src/python/lib/bd_dialogue/`) is
script-authored branching dialogue: `Dialogue`/`Node`/`Choice` with
build-time validation, condition/skill-check/faction gating (composing
bd_dnd and bd_vtm), native player-log writes, real-time sessions, and an
ImGui presentation layer with live NPC portraits and keyboard navigation
(the overlay now enables `ImGuiConfigFlags_NavEnableKeyboard`, togglable
via `bd.imgui.set_nav_enabled`). It coexists with map-native Strife
conversations (example `30_conversation_quests`), which keep firing their
events untouched. Example: `32_dialogue_trees`.

**Design rationale.** Same invariant as §11/§12: the engine boundary gets
exactly two small, sharp C++ additions (a mutable filter at the damage
choke point, and lifetime-managed per-actor storage); every rule above
them iterates at script speed. The filter's mutable-dict contract required
no new machinery — `InvokeEvent` always passed one dict by reference —
which is why the read-back is defensive (type-checked, rate-limited
warnings) rather than contractual.

**Problem / next steps.**

- **`actor_data` lifetime vs handle GC.** Data is purged when the last
  live Python handle to an actor is released, not only at actor death;
  packs that annotate actors must retain a handle (bd_rpg does).
  A `weak=False` pin option or refcount-decoupled purge would remove the
  footgun. *Effort: low-medium. Impact: medium.*
- **Damage-type taxonomy.** `bd_rpg` ships its own registry parallel to
  the engine's `FName` damage types; unifying them (query engine types,
  register custom ones natively) would help ZScript interop. *Effort:
  medium. Impact: low-medium.*
- **Dialogue camera/staging.** Sessions are real-time with the NPC pinned;
  letterboxing, camera moves, and timed text reveal are presentation
  polish scripts currently write by hand. *Effort: medium (pure Python).
  Impact: low-medium.*

**Priority.** All shipped; follow-ups are ergonomics.

---

## 14. Shared horror UX layer and showcase re-architecture

**Current state.** *(done)* The six RPG showcase examples
(27–32) were re-architected from monolithic scripts into a uniform
four-module layout (`main.py` bootstrap, `content.py` pure data,
`systems.py` wiring, `ui.py` presentation) and re-themed as a coherent
dark showcase set: *Whispers in the Walls*, *The Last Feeding*,
*The Sunken Crypt*, *The Confessor*, *Pyre & Rime*, and
*The Interrogation*. Everything shared lives in a new engine-shipped
pack, `bd_horror` (`src/python/lib/bd_horror/`): `theme.py` (a complete
ImGui horror skin — palette, style push/pop discipline, themed window/
section/bar/portrait helpers), `toasts.py` (diegetic fade-in/out
notifications with per-kind tones), and `atmosphere.py` (the `Dread`
meter with threshold callbacks, `LightProgram` flicker/blackout programs
writing the *writable* `Sector.light`, a `StalkerDirector`, and
`HorrorState` savegame persistence). All autotests kept passing
throughout — moved, never deleted — and new systems added assertions
rather than replacing them.

**Design rationale.** Expandability is the point: new content is a row
in `content.py`, new UI a themed window in `ui.py`, new rules a system
in `systems.py`. The shared pack keeps the UX language consistent and
gives modders the same building blocks the showcases use.

**Problem / next steps.**

- **Viewport-size query for bd.imgui.** Toast right-alignment currently
  measures the display via a one-frame menu-bar probe; a native
  `bd.imgui.display_size()` would make overlay layout exact. *Effort:
  low. Impact: low-medium.*
- **No-inputs window flag.** Toast windows are technically clickable;
  exposing `ImGuiWindowFlags_NoInputs` would make purely informational
  overlays cleaner. *Effort: low. Impact: low.*

**Priority.** Shipped; follow-ups are small binding additions.

---

## Summary table

| Item | Effort | Impact | Status |
|------|--------|--------|--------|
| `line_activation_failed` event | low | high | done |
| `-scripttest` CI mode | medium | high | done |
| `-pyerrorlog` JSON feed | low | medium | done |
| `dumppystub` + single-sourcing | medium | medium | done |
| Heresy typings auto-install | low | medium | done |
| Heresy error surfacing | medium | high | done |
| Activation failure reason codes | medium | high | done |
| `bd.assert_true` + test conventions | low | high | done |
| Query push-down filters (sphere/class) | low-medium | high | done |
| Engine-seeded `bd.random()` | low | medium | done |
| In-editor error markers | medium | high | proposed |
| One-click script test in Heresy | low | high | proposed |
| Pickup/inventory events | medium | high | done |
| Sector enter/exit events | medium-high | high | done |
| Read-only MP/demo observer mode | medium | medium | done |
| Fast-forward for `-scripttest` | medium | medium | done |
| Structured error records (`-pyerrorlog` fields) | low | medium | done |
| Runtime warnings channel (`bd.warn`) | medium | medium | done |
| Vectorized reads (`actor_field_batch`) | medium | medium | done |
| Snapshot-vs-handle docs clarification | docs | medium-high | done |
| TID-rebinding recipe | docs | medium | done |
| Performance guide page | docs | medium | done |
| Dear ImGui overlay + `bd.imgui` | high | high | done |
| Conversation events + `player_log` APIs | medium | high | done |
| `bd_quests` framework package | medium | high | done |
| `bd_vtm` chronicle framework | medium-high | medium | done |
| `bd_dnd` rules framework | medium | medium | done |
| ImGui docking branch + richer `image()` | medium | medium | done |
| Killer attribution (`actor_died` `attacker_*`) | medium | medium | done |
| `ui_command` event + `pyui` CCMD (UI toggles) | low | low-medium | done |
| Strife-format conversation fixture | low-medium | medium | done |
| `bd_dnd` damage-typed saves + parties | medium | medium | done |
| Headless video driver (`-headless`, null video) | high | medium | done |
| Golden screenshot comparator (`tools/compare_screenshots.py`) | low | medium | done |
| `bd_dnd` world-bound companions | medium | low-medium | done |
| Mutable pre-damage filter (`actor_before_damage`) | medium | high | done |
| Per-actor script storage (`bd.actor_data`) | low | high | done |
| `bd_rpg` elemental combat core | medium-high | high | done |
| `bd_dialogue` script-authored dialogue trees | medium | high | done |
| `bd_horror` shared horror UX layer + showcase re-architecture | medium | medium | done |
| ~~Sandbox profile for untrusted mods~~ | — | — | eliminated (§7) |
