# The Sunken Crypt (D&D Dungeon)

A grave-dark D&D delve built on the **engine-shipped `bd_dnd` rules
framework** (`src/python/lib/bd_dnd/`) and dressed in the **`bd_horror`
presentation pack** (`src/python/lib/bd_horror/` — theme, toasts, light
programs). Both are importable from any Python mod with a plain `import`.

You are **Morrow, the Grave-Hardened**, a level-1 fighter (str 16,
dex 12, con 14, d10 hit die) built through the `bd_dnd`
`CreationWizard`; he has buried more friends than he has kept. **The
Hollow Warden** — a hexer who died on watch and kept
walking — descends with you, its bound **crypt hound** (a friendly
Demon) shadowing your steps.

**The delve rides every map of the WAD, not one.** Every tabled monster
in Doom, Heretic, and Hexen pays kill XP; each level toughens your blows
(+1 damage per level past the first, capped, through the
`actor_before_damage` filter); any locked door in the game can be *bashed
open* with an Athletics check instead of its key; any hit can be rolled
with on a DEX save for half; and rest heals the marine's body as well as
the sheet. A persistent strip at the bottom of the screen tracks name,
level, XP, Blood, and the current damage bonus.

MAP02 is where the crypt proper shows itself: two graverobber-thralls
and a crypt imp near the entrance as an opening tribute, the sealed red
door on line 111 as the bash's reference fixture, a dart trap in the
entrance chamber (sector 7, tag 13), and torch-light guttering over the
tagged sectors (bd_horror `candle`/`fluorescent` light programs — no
Dread meter, no heartbeat: the example retired them). Rest is only safe
where the light holds: **sanctuary rests** require sector light ≥ 160.
Sleep in the dark and the **nightmare** comes — a DEX save vs. DC 12;
fail and the dark eats a *resolve* charge ("The dark dreams with you."),
succeed and you wake fitful with half the benefit. When the crypt hound
falls, a deep bell tolls below.

## Architecture

Four modules, all listed in the `PYTHON` manifest in load order (the
engine executes every manifest line; siblings also reach each other
through `sys.modules`, exactly how `hello_world` imports
`pyscripts/helper.py`):

| File | Role |
|------|------|
| `pyscripts/content.py` | **Pure data + factories.** The delvers (Morrow is rolled up by a driven `CreationWizard` bound to the `FIGHTER` `CharacterClass` with the Grave-Hardened and Second Wind level-1 features), probed MAP02 constants (door line/approach, trap tag, sector tags 13/7/12), progression tuning (level damage cap, save DC/cooldown, HUD refresh), rest rules, prose and toast lines. No engine calls at import. |
| `pyscripts/systems.py` | **Behavior.** `arm_crypt_lights` (MAP02), `DoorBashRules` (any locked line on any map, lending the Doom key cards for one native activation; refused lines are remembered and never spam), `wire_level_damage` (level bonus through `actor_before_damage`), `use_second_wind` (active heal, sheet and pawn), `try_long_rest` (sanctuary/nightmare/fitful branches, mending the pawn too), the progression strip, level-up and death-knell wiring, checkpoint quiescence. Registers no events at import. |
| `pyscripts/ui.py` | **Interface.** The character sheet as a bd_horror *reliquary* (Vessel / Vocation / Vitality / Omens sections; Vessel spells out the modifier rule, Vocation reads the bound class's hit die, saves, skills, and feature descriptions), the party roster chapel, and the toast stack. No-op headless. |
| `pyscripts/main.py` | **Bootstrap + autotest.** Object graph, event wiring, the MAP02 gating for the set pieces, and the full deterministic test schedule. |

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
  Grave-Hardened and Second Wind features, seeding the resolve and
  second_wind pools. Later levels run through the `on_level_up` hook
  (Death Knell is flavor at level 2) and level 4 queues two ASI points on
  `pending_asi`
- `Character`: `skill_check` / `ability_check` / `saving_throw` with
  rich result dicts and a roll log, `award_xp` with the 5e threshold
  table, hit-die level-ups with `on_level_up` hooks, short/long rests,
  and per-rest `resources` (`grant_resource` / `use_resource` /
  `restore_resources`)
- `track_xp_from_kills`: the full-roster monster XP table (Doom II,
  Heretic, Hexen — see `DEFAULT_XP_TABLE`) wired to `actor_died` with
  exact player-credit attribution (`player_index=0`: only the local
  player's kills award XP; `player_index=None` restores the legacy
  any-death policy)
- `actor_before_damage` as a progression hook: the mutable pre-damage
  filter adds the level bonus to every hit the local player lands on a
  monster
- `DamageSaveRule`: `actor_damaged` on the player -> DEX save ->
  retroactive heal refund (half on a success, full on a natural 20),
  with cooldown and damage-type filtering
- `Party` / `PartyState`: a two-member roster (Morrow plus the Hollow
  Warden) with shared/solo XP awards, persisted through the checkpoint
  round-trip
- `Companion`: the Warden is bound to a friendly crypt hound in the
  world — it shadows the player (follow loop + teleport catch-up,
  re-binding itself on every map), fights the player's recent attackers
  through its native AI (`Actor.target` is writable), its actor health
  *is* the Warden's RPG hp (two-way sync through
  `Character.on_hp_changed` / `set_hp`), death incapacitates the Warden
  and `revive()` respawns it at half hp; the descriptor rides along in
  the `PartyState` checkpoint and re-binds by TID after a load
- `LockedDoorCheck`: `line_activation_failed` (reason `"locked"`) ->
  Athletics bash -> native door opening; the example's `DoorBashRules`
  dispatcher scales it from one probed door to *every* locked line,
  lending all three Doom key cards (locks 1-3 and 129-134 all accept
  them, per `wadsrc/static/lockdefs.txt`) and remembering lines whose
  activation is refused
- `TrapZone`: `sector_entered` on tagged sectors -> DEX save -> full or
  halved native damage, with `once`/cooldown control — armed only on the
  crypt map so no other map's tag 13 can spring it
- `DialogueSkillGate`: `conversation_reply` -> skill check -> callbacks
  (unit-tested in the autotest; MAP02 has no Strife NPCs)
- `CharacterState` persistence: level/XP/HP/abilities/proficiencies/
  resources round-trip through `bd.save_checkpoint` / `bd.load_checkpoint`
  with zero extra mod-side code
- `bd.draw_text` as a persistent HUD: the progression strip is one
  display-list item with a stable id, refreshed by a slow repeating task
- `bd_horror.theme`: the reliquary skin — `apply()`/`clear()`,
  `begin_window`, `section`, `bar` (blood/ember/bruise tones, pulse),
  `omen_text`, `kv_row`; roll-log omens colored success=sickly /
  fail=blood / crit=ember
- `bd_horror.toasts`: diegetic toast queue (`quest`/`harm`/`info` kinds)
  with an assertable history ring
- `bd_horror.atmosphere`: the `LightManager` half of `HorrorState`
  running `candle` and `fluorescent` programs over the crypt's tagged
  sectors, persisted through `bd.state` (the `Dread` meter and the
  `StalkerDirector` are deliberately never started in this example)

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
check (`P_CheckKeys`) rejects a null activator — so the bash lends the
Doom key cards (`RedCard`/`BlueCard`/`YellowCard`) to the activator for
one native `activate(activator, clear=True)` and reclaims them
immediately.

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

Any map works — `+map MAP01`, a megawad, Heretic or Hexen with their own
bestiary rows: the crypt's set pieces stay home on MAP02 and the rules
layer (XP, toughened blows, rests, bashing, reflex saves, the hound)
travels with you.

Controls: **Custom Action 1** (auto-bound to **Q**; or `toggle_sheet`
at the console) opens/closes the reliquary sheet; **Custom Action 2**
(auto-bound to **V**; or `crypt_rest`) attempts a rest — find light
>= 160 for a true sanctuary rest that mends body and sheet; **Custom
Action 3** (auto-bound to **C**; or `second_wind`) spends the Second
Wind charge to heal by the hit die plus your level. All three appear as
"Custom Action N" under Options -> Customize Controls, Custom Actions,
and any binding you set there is respected. Kill anything tabled for XP
(the "+XP" flashes stack into the level-up flash and a harder-hitting
marine), brave the dart trap on MAP02 by leaving the entrance chamber
and walking back in, then follow the corridor and **use the sealed
door**: the bash check rolls in the Omens — "The door holds fast." on a
failure, and the door grinds open on a success. The same bash works on
any locked door in the game.

Headless autotest (deterministic; asserts the whole rules engine
including the `CreationWizard`/`CharacterClass` classes layer, the light
programs, the nightmare/fitful/sanctuary rest branches on sheet and
pawn, the generic door-bash dispatcher, the level-damage filter, the
Second Wind press, the companion spawn/damage-sync/teleport/kill/revive
paths and its death toll, the real door-bash and trap paths, then a
checkpoint save/load round-trip verifying the script RNG stream resumes
exactly and all character, party, companion, and light-program state
survives):

```bash
BD_EXAMPLE_AUTOTEST=1 ./build/biaseddoom -headless \
    -iwad ~/games/doom2.wad -file examples/python/29_dnd_dungeon \
    -python -scripttest 1400 4 -nosound +map MAP02
# -> SCRIPT TEST: PASS
```

Cross-map smoke (the point of the redesign — nothing MAP02-specific may
fire, and nothing may error, on another map):

```bash
./build/biaseddoom -headless \
    -iwad ~/games/doom2.wad -file examples/python/29_dnd_dungeon \
    -python -scripttest 400 4 -nosound +map MAP01
# -> SCRIPT TEST: PASS (no scenario, no errors; rules layer idle-safe)
```

Screenshot capture (writes `/tmp/sunken_crypt.png` a few seconds in):

```bash
BD_EXAMPLE_SCREENSHOT=1 xvfb-run -a ./build/biaseddoom \
    -iwad ~/games/doom2.wad -file examples/python/29_dnd_dungeon \
    -python +map MAP02
```

## Expanding the crypt

- **New monsters worth XP**: the roster lives in bd_dnd's
  `DEFAULT_XP_TABLE` (Doom II, Heretic, Hexen); a mod may override it
  per class or pass its own table to `track_xp_from_kills`. MAP02's
  opening tribute is `MONSTER_SPAWNS` in `content.py`.
- **Re-rolling Morrow**: edit `HERO_ABILITY_SCORES` or the skill picks
  in `content.make_hero_via_wizard()`; the wizard validates the build
  (standard array, point buy, or rolled scores) and `bind_class` applies
  the level-1 feature package.
- **New traps**: construct another `bd_dnd.TrapZone` with a different
  tag/DC/damage, armed from `on_map` when `systems.is_crypt_map()` (or
  your own map gate) holds.
- **More darkness**: add tags to `CANDLE_TAGS` / `CORPSE_LIGHT_TAGS`
  (probe with `bd.sectors()`). Want the Dread meter and its heartbeat
  back? `bd_horror.Dread` still ships in the framework —
  `horror.dread.start()` is one call away; this example just chooses
  silence.
- **Rest rules**: tune `SANCTUARY_LIGHT`, `NIGHTMARE_DC`, and
  `FITFUL_HEAL_FRACTION` in `content.py`; the branches and toasts live
  in `systems.try_long_rest`.
- **Progression tuning**: `LEVEL_DAMAGE_BONUS_CAP`, `SAVE_DC`,
  `SAVE_COOLDOWN_TICS`, and `HUD_REFRESH_TICS` in `content.py`.
- **Presentation**: re-tint the whole interface by editing
  `bd_horror.theme.PALETTE` before `apply()`, or add sections to
  `ui._draw_reliquary_body`.
