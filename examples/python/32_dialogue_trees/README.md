# The Interrogation

A **branching horror dialogue** fixture built on the engine-shipped
`bd_dialogue` framework, reskinned with the `bd_horror` pack. In the
candle-dim entrance of Doom II MAP01, **The Inquisitor** — an
occult quartermaster of the Hollow Choir — waits in the ash-light. Walk
up to him and press **Q** (Custom Action 1): a blood-framed ImGui window
opens with his
live portrait, faction-colored name, wrapped prose, judgment lines for
every skill check, and numbered choices annotated with their locks and
DCs. While you talk, the dark holds its breath — end the conversation
and the stalker director resumes its hunt.

## Architecture

Four modules, all listed in the root `PYTHON` manifest in dependency
order. Each sibling self-registers a stable `sys.modules` alias (a live
proxy — the engine registers manifest modules under mangled names only
after execution), so later entries reach it with a plain `import`, the
same sibling-import mechanics `hello_world` gets via `bd.import_script`.

| Module | Role |
|---|---|
| `pyscripts/content.py` | **Pure data + factories** — fixture constants, every player-visible string, and the factories: `build_dialogue()` (the whole tree), `build_quest()`, `build_factions()` (bd_vtm), `build_character()` (bd_dnd), plus the choice `condition`/`effect` callables. No engine calls at import. |
| `pyscripts/systems.py` | **Rules and event wiring**: the `InquisitionSession` wrapper (a `DialogueSession` subclass whose ctx carries the live `dread` level, the `HorrorState`, and the example-local Inquisitor attitude: `attitude`, `attitude_standing`, and a `shift_attitude` callable, persisted under its own `bd.state` key), the Custom Action 1 talk interaction (auto-bound to Q, rebindable under Options -> Customize Controls, Custom Actions), the NPC/crate spawns, and the dusk ambience: a `PositionCandle` (a `LightProgram` subclass bound to the Inquisitor's untagged sector by position via `bd.sector_at`) and the `StalkerDirector` enabled only while no session is active. |
| `pyscripts/ui.py` | **The Interrogation window** — a fully `bd_horror.theme`-skinned dialogue UI composed in the example (reading `session.choices()` / `session.choose()` rather than subclassing the framework's `DialogueUI`): `frame_image` portrait plate, faction-colored speaker, bone body text, judgment lines ("The Inquisitor is swayed." sickly / "He sees the lie." in text-safe wound red), annotated choice rows, and a pulsing Dread bar. No-op under `-headless`. |
| `pyscripts/main.py` | **Thin bootstrap** — the `BD_EXAMPLE_AUTOTEST=1` schedule (every assertion, reading state out of `systems`) and the `BD_EXAMPLE_SCREENSHOT=1` pose. |

## What it teaches

- **Data-driven trees.** `Dialogue` / `Node` / `Choice` model the whole
  conversation; dangling `next`/`fail_next` references raise `ValueError`
  at validation (session construction validates automatically).
- **Every gate kind, composed from the other shipped packs.**
  - `condition(ctx)` — hides a choice (the handout vanishes once the
    quest is accepted; **the mark choice hides until the session ctx
    carries `dread >= 50`**).
  - `faction_gate=("The Hollow Choir", 1)` — renders `[LOCKED]` and is
    non-selectable until reputation reaches +1 (granted by the quest).
  - `skill_check=("persuasion", 12)` and `skill_check=("intimidation",
    12)` — roll `bd_dnd` `Character.skill_check` on selection; success
    routes to `next`, failure to `fail_next`. Rows show their DC; with
    no character attached they render `(unavailable)`.
  - `effect(ctx)` — runs *after* the roll, branching on
    `ctx["check_result"]["success"]` (the discount grants 20 shells, the
    threat grants 30 rifle rounds *and* bumps the room's dread; a failed
    threat also erodes the Inquisitor's `attitude` by 20).
  - `log=("text", 0)` — writes the native Strife journal line via
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
- **Real-time sessions.** `DialogueSession` zeroes the NPC's velocity
  while talking, one session at a time (a second `start()` warns and
  no-ops), and a stale NPC handle auto-ends the conversation.
  `session.rng` accepts a scripted roller (`randint(lo, hi)`) for
  deterministic tests.
- **The `bd_horror` atmosphere.** `HorrorState` runs the dread meter
  (darkness/monsters/damage), the candle program dresses the
  Inquisitor's sector, and the `StalkerDirector` hunts at dread ≥ 75 —
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
| "How does business fare in the ashes?" | — | smalltalk loop |
| "What does the Choir whisper?" | Choir rep ≥ 1 | rumor node |
| "A penitent's coin is thin — show me mercy on shells." | Persuasion DC 12 | success → +20 shells; failure → refusal |
| "Sell to me, or the dark learns your name." | Intimidation DC 12 | success → +30 rifle rounds and +dread; failure → the threat node |
| "Is there work for the damned?" | only while the quest is inactive | starts *The Ash Tithe* + sets the player log |
| "[The mark on your throat pulses] 'You know what I am.'" | hidden until dread ≥ 50 | the confession node → writes a second log line |
| "You again." | hidden until the Inquisitor's attitude ≤ -50 | the greeting node, its body line rewritten to the current standing (hostile at -60) |
| "Another time, Inquisitor." | — | farewell → ends |

Picking up the reliquary (a ShellBox in the yard) completes the quest
objective through `bd_quests.log.track_pickup`, completes the quest, and
grants Choir reputation +1 — unlocking the rumor branch next time.

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

Walk up to the Inquisitor and press **Q** (Custom Action 1; rebindable
under Options -> Customize Controls, Custom Actions). Click choices or
use the arrow keys and ENTER.

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
success, dread bump on a landed threat), the attitude ctx keys with three
failed threats eroding the Inquisitor 0 → -20 → -40 → -60 and the "You
again." greeting appearing exactly at -50 with the hostile standing line,
both player-log writes, the hidden mark choice absent below dread 50 /
present at-and-above it (via the shipped `Dread.set_level` debug setter),
the hidden handout choice, the one-session guard, stale-NPC auto-end, the
armed candle program, and the stalker director's suppression during a
session.

Documentation capture (poses the player, opens the window, fails a
haggle on purpose so the judgment line and the full annotated choice
list — mark choice included — are in frame):

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
  greeting for the full pattern).
- **Re-voice:** every string is in `content.py`; no logic lives there,
  so tone passes are safe.
- **Harsher ambience:** tune `CANDLE_*` in `content.py`, or enable the
  stalker director's `chance`/`class_name` in `systems.on_map`.
