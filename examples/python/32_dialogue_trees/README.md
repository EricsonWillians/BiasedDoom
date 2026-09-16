# The Interrogation

A **branching horror dialogue** fixture built on the engine-shipped
`bd_dialogue` framework, reskinned with the `bd_horror` pack. In the
candle-dim entrance of Doom II MAP01, **The Inquisitor**, an occult
quartermaster of the Hollow Choir, waits in the ash-light under a
floating golden `!`. A map-start toast names the talk key; walk up to
him and a `[Q] Talk` label appears in range. Press **Q** (Custom
Action 1): a blood-framed ImGui window opens with his live portrait,
faction-colored name, wrapped prose, judgment lines for every skill
check, and numbered choices annotated with their locks, their DCs, and
*your* bonus ("(DC 12 Persuasion, you +4)"). Hidden choices announce
themselves early: near a threshold, the Inquisitor's prose says so.
While you talk, the dark holds its breath; end the conversation and the
stalker director resumes its hunt.

## Architecture

Four modules, all listed in the root `PYTHON` manifest in dependency
order. Each sibling self-registers a stable `sys.modules` alias (a live
proxy: the engine registers manifest modules under mangled names only
after execution), so later entries reach it with a plain `import`, the
same sibling-import mechanics `hello_world` gets via `bd.import_script`.

| Module | Role |
|---|---|
| `pyscripts/content.py` | **Pure data + factories**: fixture constants, every player-visible string (including the marker, toast, and hint-line strings), and the factories: `build_dialogue()` (the whole tree), `build_quest()`, `build_factions()` (bd_vtm), `build_character()` (bd_dnd), plus the choice `condition`/`effect` callables. No engine calls at import. |
| `pyscripts/systems.py` | **Rules and event wiring**: the `InquisitionSession` wrapper (a `DialogueSession` subclass whose ctx carries the live `dread` level, the `HorrorState`, and the example-local Inquisitor attitude: `attitude`, `attitude_standing`, and a `shift_attitude` callable, persisted under its own `bd.state` key; its `choices()` override adds the "you +N" check annotations and the hidden-choice hint lines), the Custom Action 1 talk interaction (auto-bound to Q, rebindable under Options -> Customize Controls, Custom Actions), the world-marker lifecycle (gold "!" until first contact, then a live-binding "[Q] Talk" label in range, kept by a 7-tic map-local task with `marker_state` mirrored for tests), the NPC/crate spawns, and the dusk ambience: a `PositionCandle` (a `LightProgram` subclass bound to the Inquisitor's untagged sector by position via `bd.sector_at`) and the `StalkerDirector` enabled only while no session is active. |
| `pyscripts/ui.py` | **The Interrogation window**: a fully `bd_horror.theme`-skinned dialogue UI composed in the example (reading `session.choices()` / `session.choose()` rather than subclassing the framework's `DialogueUI`): `frame_image` portrait plate, faction-colored speaker, bone body text (hint lines included, as the session composes them), judgment lines ("The Inquisitor is swayed." sickly / "He sees the lie." in text-safe wound red), annotated choice rows, and a pulsing Dread bar. No-op under `-headless`. |
| `pyscripts/main.py` | **Thin bootstrap**: the `BD_EXAMPLE_AUTOTEST=1` schedule (every assertion, reading state out of `systems`) and the `BD_EXAMPLE_SCREENSHOT=1` pose. |

## What it teaches

- **Data-driven trees.** `Dialogue` / `Node` / `Choice` model the whole
  conversation; dangling `next`/`fail_next` references raise `ValueError`
  at validation (session construction validates automatically).
- **Every gate kind, composed from the other shipped packs.**
  - `condition(ctx)`: hides a choice (the handout vanishes once the
    quest is accepted; **the mark choice hides until the session ctx
    carries `dread >= 50`**).
  - `faction_gate=("The Hollow Choir", 1)`: renders `[LOCKED]` and is
    non-selectable until reputation reaches +1 (granted by the quest).
  - `skill_check=("persuasion", 12)` and `skill_check=("intimidation",
    12)`: roll `bd_dnd` `Character.skill_check` on selection; success
    routes to `next`, failure to `fail_next`. Rows show their DC **and
    your total bonus**: "(DC 12 Persuasion, you +4)". The session
    wrapper's `choices()` override extends the framework annotation via
    `skill_bonus()`, which reads the character through bd_dnd's own
    helpers (`SKILLS`, `AbilityScores.mod`, `proficiency`,
    `proficient_skills`), so the odds are knowable before you commit.
    With no character attached the rows render `(unavailable)`.
  - `effect(ctx)`: runs *after* the roll, branching on
    `ctx["check_result"]["success"]` (the discount grants 20 shells, the
    threat grants 30 rifle rounds *and* bumps the room's dread; a failed
    threat also erodes the Inquisitor's `attitude` by 20).
  - `log=("text", 0)`: writes the native Strife journal line via
    `bd.set_player_log` (the errand and the Inquisitor's confession).
- **Extending the session context.** `InquisitionSession.context()` adds
  `dread` and `horror` keys plus the example-local Inquisitor attitude:
  `attitude` (int), `attitude_standing` (hostile/cold/neutral/warm/
  trusted, same thresholds as `bd_npcs` but reimplemented locally), and a
  `shift_attitude` callable effects use to move it. Conditions read them
  live on every `choices()` call, so a threshold crossed
  mid-conversation reveals the hidden choice on the next render. Raw
  framework sessions never see the mark or the greeting; the wrapper is
  what exposes them.
- **Hidden choices advertise themselves.** When dread comes within 20 of
  the mark threshold (30 <= dread < 50) the smalltalk node gains "The
  Inquisitor studies the dread on you..."; when attitude drops to -30 or
  below while the greeting is still hidden (above -50) the start node
  gains "The Inquisitor's patience with you wears thin...". The session
  wrapper composes the hint into the node's body text on every
  `choices()` call and lifts it (an exact base-text restore) once the
  choice is revealed or the session ends.
- **Talk markers.** A gold `!` (`bd.draw_world_text`, display-list id
  98000) floats over the Inquisitor until the first conversation starts;
  after that a `[Q] Talk` label (id 98001, text read from the live
  binding each refresh) shows while you are within 128 units with no
  session open. A 7-tic map-local `bd.schedule` task owns registration,
  both markers keep the default `occlude=True` so line of sight applies,
  and `systems.marker_state` mirrors the lifecycle for the autotest. A
  map-start toast names the binding and explains the annotations.
- **Real-time sessions.** `DialogueSession` zeroes the NPC's velocity
  while talking, one session at a time (a second `start()` warns and
  no-ops), and a stale NPC handle auto-ends the conversation.
  `session.rng` accepts a scripted roller (`randint(lo, hi)`) for
  deterministic tests.
- **The `bd_horror` atmosphere.** `HorrorState` runs the dread meter
  (darkness/monsters/damage), the candle program dresses the
  Inquisitor's sector, and the `StalkerDirector` hunts at dread >= 75,
  suppressed while a session is active, resumed from the session's
  `on_end` callback.
- **Interaction through a Custom Action.** Custom Action 1
  (`+pyaction1`, auto-bound to Q unless you bound it under Options ->
  Customize Controls, Custom Actions) fires the `custom_action` event;
  the handler starts a session within 128 units. The `talk` console
  alias (`pyui talk` -> `ui_command`) routes into the same handler, and
  the "no one near" feedback names the live binding. Choices activate by
  mouse click or ImGui keyboard navigation (arrow keys + ENTER); the
  `1.`/`2.` prefixes are visual hints only.

## The tree

| Choice on the start node | Gate | Route |
|---|---|---|
| "How does business fare in the ashes?" | none | smalltalk loop |
| "What does the Choir whisper?" | Choir rep >= 1 | rumor node |
| "A penitent's coin is thin - show me mercy on shells." | Persuasion DC 12 (row shows "you +4") | success → +20 shells; failure → refusal |
| "Sell to me, or the dark learns your name." | Intimidation DC 12 (row shows "you +4") | success → +30 rifle rounds and +dread; failure → the threat node |
| "Is there work for the damned?" | only while the quest is inactive | starts *The Ash Tithe* + sets the player log |
| "[The mark on your throat pulses] 'You know what I am.'" | hidden until dread >= 50 | the confession node → writes a second log line |
| "You again." | hidden until the Inquisitor's attitude <= -50 | the greeting node, its body line rewritten to the current standing (hostile at -60) |
| "Another time, Inquisitor." | none | farewell → ends |

The hidden choices telegraph themselves: the smalltalk node gains the
hint line "The Inquisitor studies the dread on you..." while dread is
within 20 below the mark threshold, and the start node gains "The
Inquisitor's patience with you wears thin..." while attitude is at or
below -30 with the greeting still hidden.

Picking up the reliquary (a ShellBox in the yard) completes the quest
objective through `bd_quests.log.track_pickup`, completes the quest, and
grants Choir reputation +1, unlocking the rumor branch next time.

## Coexistence with native Strife dialogue

`bd_dialogue` is a pure Python/ImGui layer; it does not touch the
engine's native Strife conversation system. See
[`30_conversation_quests`](../30_conversation_quests/) for the
engine-native ZSDF/`DIALOGxx` approach.

## Running it

```bash
./build/biaseddoom -iwad /path/to/DOOM2.WAD \
    -file examples/python/32_dialogue_trees \
    -python -stdout +map MAP01
```

Follow the golden `!` (the map-start toast and the `[Q] Talk` label name
the key). Walk up to the Inquisitor and press **Q** (Custom Action 1;
rebindable under Options -> Customize Controls, Custom Actions). Click
choices or use the arrow keys and ENTER.

Headless autotest (deterministic, scripted RNG doubles):

```bash
BD_EXAMPLE_AUTOTEST=1 ./build/biaseddoom -headless \
    -iwad /path/to/DOOM2.WAD -file examples/python/32_dialogue_trees \
    -python -scripttest 1400 4 -nosound +map MAP01
# -> SCRIPT TEST: PASS
```

The autotest asserts: model validation errors, the `talk` ui_command
path plus the synthetic Custom Action 1 press path (event payload,
session start, release edge), smalltalk routing, the faction gate locked
(pre-rep) and unlocked
(post-rep), both persuasion and both intimidation branches (ammo only on
success, dread bump on a landed threat), the check annotations naming
your bonus ("(DC 12 Persuasion, you +4)", cross-checked against
`systems.skill_bonus`) with `(unavailable)` preserved when no character
is attached (raw and wrapped sessions alike), the marker lifecycle (the
"!" pre-contact, its retirement on first contact, the "[Q] Talk" label
in range with no session open, hidden during a session / out of range /
after the NPC is destroyed, the intro toast fired), the hint lines at
both thresholds and their exact lift, the attitude ctx keys with three
failed threats eroding the Inquisitor 0 → -20 → -40 → -60 and the "You
again." greeting appearing exactly at -50 with the hostile standing line,
both player-log writes, the hidden mark choice absent below dread 50 /
present at-and-above it (via the shipped `Dread.set_level` debug setter),
the hidden handout choice, the one-session guard, stale-NPC auto-end, the
armed candle program, and the stalker director's suppression during a
session.

Documentation capture (poses the player, opens the window, fails a
haggle on purpose so the judgment line and the full annotated choice
list, mark choice included, are in frame):

```bash
BD_EXAMPLE_SCREENSHOT=1 xvfb-run -a ./build/biaseddoom \
    -iwad /path/to/DOOM2.WAD -file examples/python/32_dialogue_trees \
    -python -nosound +map MAP01
# -> /tmp/interrogation.png
```

## Expanding it

- **New branches:** add nodes/choices in `content.build_dialogue()`;
  extend the autotest in `main.py` (choice indices live at its top).
- **New hidden choices:** any `condition(ctx)` can read `ctx["dread"]`
  or `ctx["horror"]`; the wrapper already feeds them. For people-shaped
  gates, read `ctx["attitude"]` / `ctx["attitude_standing"]` and move the
  value with `ctx["shift_attitude"]` from an effect (see the "You again."
  greeting for the full pattern). To hint a new hidden choice early, add
  its node to `_HINT_NODES` and a case to `_hint_for` in `systems.py`.
- **Re-voice:** every string is in `content.py`; no logic lives there,
  so tone passes are safe.
- **Harsher ambience:** tune `CANDLE_*` in `content.py`, or enable the
  stalker director's `chance`/`class_name` in `systems.on_map`.
