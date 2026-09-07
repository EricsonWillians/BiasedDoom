# The Sunken Crypt (D&D Dungeon)

A grave-dark D&D dungeon-crawl mini-scenario on Doom II MAP02, built on
the **engine-shipped `bd_dnd` rules framework** (`src/python/lib/bd_dnd/`)
and dressed in the **`bd_horror` presentation pack**
(`src/python/lib/bd_horror/` — theme, toasts, atmosphere). Both are
importable from any Python mod with a plain `import`.

You are **Morrow, the Grave-Hardened**, a level-1 fighter (str 16,
dex 12, con 14, d10 hit die) built through the `bd_dnd`
`CreationWizard`; he has buried more friends than he has kept. **The
Hollow Warden** — a hexer who died on watch and kept
walking — descends with you, its bound **crypt hound** (a friendly
Demon) shadowing your steps. Two graverobber-thralls and a crypt imp
near the entrance are worth XP; the sealed red door can be *bashed open*
with an Athletics check instead of the red key; the entrance chamber
hides a dart trap with a DEX save for half damage — and every sprung
trap **spikes the dread meter**, because the crypt notices pain.

Torch-light gutters over the crypt's tagged sectors (bd_horror
`candle`/`fluorescent` light programs), and rest is only safe where the
light holds: **sanctuary rests** require sector light ≥ 160. Sleep in
the dark and the **nightmare** comes — a DEX save vs. DC 12; fail and
the dark eats a *resolve* charge ("The dark dreams with you."), succeed
and you wake fitful with half the benefit. When the crypt hound falls, a
deep bell tolls below.

## Architecture

Four modules, all listed in the `PYTHON` manifest in load order (the
engine executes every manifest line; siblings also reach each other
through `sys.modules`, exactly how `hello_world` imports
`pyscripts/helper.py`):

| File | Role |
|------|------|
| `pyscripts/content.py` | **Pure data + factories.** The delvers (Morrow is rolled up by a driven `CreationWizard` bound to the `FIGHTER` `CharacterClass`), probed MAP02 constants (door line/approach, trap tag, sector tags 13/7/12), light-program parameters, rest rules, prose and toast lines. No engine calls at import. |
| `pyscripts/systems.py` | **Behavior.** `arm_crypt_lights`, `DreadTrapZone` (trap springs spike dread), `try_long_rest` (sanctuary/nightmare/fitful branches), level-up and death-knell wiring, checkpoint quiescence. Registers no events at import. |
| `pyscripts/ui.py` | **Interface.** The character sheet as a bd_horror *reliquary* (Vessel / Vitality / Omens sections), the party roster chapel, and the toast stack. No-op headless. |
| `pyscripts/main.py` | **Bootstrap + autotest.** Object graph, event wiring, scenario setup, and the full deterministic test schedule. |

## What it teaches

- `roll("2d6+3")` / `d20(advantage=True)` dice: notation parsing,
  advantage/disadvantage, natural-20/natural-1 criticals, all through
  the deterministic savegame-serialized script RNG
- `ABILITIES` / `modifier()` / `SKILLS` / `proficiency_bonus()`: the
  5e math tables (floored modifiers, 2 + (level-1)//4 proficiency)
- `CharacterClass` / `CreationWizard` / `bind_class`: the classes
  layer. Morrow is data: a Fighter class definition (d10 hit die, str/con
  saves, athletics/perception class skills, level features) plus a driven
  wizard whose `finish()` binds the class and applies the level-1
  Grave-Hardened feature, seeding the resolve pool. Later levels run
  through the `on_level_up` hook (Death Knell is flavor at level 2) and
  level 4 queues two ASI points on `pending_asi`
- `Character`: `skill_check` / `ability_check` / `saving_throw` with
  rich result dicts and a roll log, `award_xp` with the 5e threshold
  table, hit-die level-ups with `on_level_up` hooks, short/long rests,
  and per-rest `resources` (`grant_resource` / `use_resource` /
  `restore_resources`)
- `track_xp_from_kills`: monster-class XP table wired to `actor_died`
  with exact player-credit attribution (`player_index=0`: only the local
  player's kills award XP; `player_index=None` restores the legacy
  any-death policy)
- `DamageSaveRule`: `actor_damaged` on the player -> DEX save ->
  retroactive heal refund (half on a success, full on a natural 20),
  with cooldown and damage-type filtering
- `Party` / `PartyState`: a two-member roster (Morrow plus the Hollow
  Warden) with shared/solo XP awards, persisted through the checkpoint
  round-trip
- `Companion`: the Warden is bound to a friendly crypt hound in the
  world — it shadows the player (follow loop + teleport catch-up),
  fights the player's recent attackers through its native AI
  (`Actor.target` is writable), its actor health *is* the Warden's RPG
  hp (two-way sync through `Character.on_hp_changed` / `set_hp`), death
  incapacitates the Warden and `revive()` respawns it at half hp; the
  descriptor rides along in the `PartyState` checkpoint and re-binds by
  TID after a load
- `LockedDoorCheck`: `line_activation_failed` (reason `"locked"`) ->
  Athletics bash / Sleight-of-Hand pick -> native door opening
- `TrapZone`: `sector_entered` on tagged sectors -> DEX save -> full or
  halved native damage, with `once`/cooldown control — here subclassed
  (`DreadTrapZone`) to spike `bd_horror.Dread` on every spring
- `DialogueSkillGate`: `conversation_reply` -> skill check -> callbacks
  (unit-tested in the autotest; MAP02 has no Strife NPCs)
- `CharacterState` persistence: level/XP/HP/abilities/proficiencies/
  resources round-trip through `bd.save_checkpoint` / `bd.load_checkpoint`
  with zero extra mod-side code
- `bd_horror.theme`: the reliquary skin — `apply()`/`clear()`,
  `begin_window`, `section`, `bar` (blood/ember/bruise tones, pulse),
  `omen_text`, `kv_row`; roll-log omens colored success=sickly /
  fail=blood / crit=ember
- `bd_horror.toasts`: diegetic toast queue (`quest`/`harm`/`info` kinds)
  with an assertable history ring
- `bd_horror.atmosphere`: `HorrorState` owning a `Dread` meter
  (darkness/monster/damage-driven, with heartbeat, vignette, and
  whisper stings) and a `LightManager` running `candle` and
  `fluorescent` programs over the crypt's tagged sectors, all persisted
  through `bd.state`

## The map probes behind the numbers

MAP02 has no *visible* classic locked specials through the engine
because GZDoom translates Doom-format specials. Probing `bd.lines()`
shows the red door as engine special 13 (`Door_LockedRaise`) with args
`[7, 64, 0, 129, 0]` (lock 129 = red key) on **lines 111/112**; the raw
WAD has it as special 135. It is approached from **(752, 1328)** facing
**angle 270**. Using it without the key fires `line_activation_failed`
with `reason == "locked"` (drive `BT_USE` in `pre_tick` with 1-tic
pulses, and set `actor.angle` directly — `set_input(yaw=...)` is a
delta, not an absolute facing).

`Line.activate` executes the special directly, and the engine's lock
check (`P_CheckKeys`) rejects a null activator — so `LockedDoorCheck`
opens key-locked doors by granting `key_class` (`"RedCard"`) to the
activator for one native `activate(activator, clear=True)` and
reclaiming it immediately.

`bd.sectors()` tags (verified live): **sector 7** (the player start
room, light 144) carries **tag 13**; **sectors 40-43** (the drowned
passage beyond the red door, light 128) carry **tag 7**; **sector 47**
(the corpse-light hall, light 96) carries **tag 12**. The door approach
point and the south corridor are untagged (sectors 38 and 0), which
makes them safe autotest fixtures for the forced-darkness rest
experiments. `sector_entered` fires for teleports with
`set_position(..., check=False)` and for the initial spawn at t=0.

One checkpoint caveat, probe-verified: candle/fluorescent programs draw
from the deterministic script RNG on every step, and after a savegame
load their task phase re-anchors to the load time — so a checkpoint
taken mid-flicker cannot resume the *exact* RNG stream. The autotest
therefore quiesces the light programs just before the save and re-arms
them after the stream assertion; interactive play lets them ride
`HorrorState` persistence normally.

## Running it

Interactive picker (auto-detects the engine and IWAD):

```bash
tools/play-python-example.py     # choose 29_dnd_dungeon
```

Or directly:

```bash
./build/biaseddoom -python -iwad ~/games/doom2.wad \
    -file examples/python/29_dnd_dungeon +map MAP02
```

Controls: **K** (or `toggle_sheet` at the console) opens/closes the
reliquary sheet; **R** (or `crypt_rest`) attempts a rest — find light
≥ 160 for a true sanctuary rest. Kill the welcoming committee for XP
(watch the reliquary's Omens and the ember level-up flash), walk south
out of the entrance chamber and back in to brave the dart trap, then
follow the corridor and **use the sealed door**: the bash check rolls in
the Omens — "The door holds fast." on a failure, and the door grinds
open on a success.

Headless autotest (deterministic; asserts the whole rules engine
including the `CreationWizard`/`CharacterClass` classes layer, the light
programs, the trap dread spikes, the nightmare/fitful/sanctuary
rest branches, the companion spawn/damage-sync/teleport/kill/revive
paths and its death toll, the real door-bash and trap paths, then a
checkpoint save/load round-trip verifying the script RNG stream resumes
exactly and all character, party, companion, and horror state
survives):

```bash
BD_EXAMPLE_AUTOTEST=1 ./build/biaseddoom -headless \
    -iwad ~/games/doom2.wad -file examples/python/29_dnd_dungeon \
    -python -scripttest 1400 4 -nosound +map MAP02
# -> SCRIPT TEST: PASS
```

Screenshot capture (writes `/tmp/sunken_crypt.png` a few seconds in):

```bash
BD_EXAMPLE_SCREENSHOT=1 xvfb-run -a ./build/biaseddoom \
    -iwad ~/games/doom2.wad -file examples/python/29_dnd_dungeon \
    -python +map MAP02
```

## Expanding the crypt

- **New monsters worth XP**: add to `MONSTER_SPAWNS` in `content.py`
  (class, TID, corridor offset) — kill XP comes from bd_dnd's
  `DEFAULT_XP_TABLE`, which a mod may override per class.
- **Re-rolling Morrow**: edit `HERO_ABILITY_SCORES` or the skill picks
  in `content.make_hero_via_wizard()`; the wizard validates the build
  (standard array, point buy, or rolled scores) and `bind_class` applies
  the level-1 feature package.
- **New traps**: construct another `systems.DreadTrapZone` with a
  different tag/DC/damage; the dread spike is automatic.
- **More darkness**: add tags to `CANDLE_TAGS` / `CORPSE_LIGHT_TAGS`
  (probe with `bd.sectors()`), or arm a `bd_horror.StalkerDirector`
  (`horror.stalker.enabled = True`) so high dread spawns things behind
  you.
- **Rest rules**: tune `SANCTUARY_LIGHT`, `NIGHTMARE_DC`, and
  `FITFUL_HEAL_FRACTION` in `content.py`; the branches and toasts live
  in `systems.try_long_rest`.
- **Presentation**: re-tint the whole interface by editing
  `bd_horror.theme.PALETTE` before `apply()`, or add sections to
  `ui._draw_reliquary_body`.
