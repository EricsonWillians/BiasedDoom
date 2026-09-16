# The Delve (D&D rules layer)

A map-agnostic **D&D rules layer** for BiasedDoom's embedded Python,
built on the engine-shipped **`bd_dnd`** framework, the **`bd_horror`**
presentation pack (toasts and the sheet skin), and the **`bd_quests`**
journal pack. It takes its shape from `15_roguelike_run`: no probed
fixtures, no map gating, one clear loop that works identically on every
map, and everything announced loudly.

**The loop.** You found a delver, and every map deals you a contract:
a **blood tribute** (put down N of the map's monsters) and a crowned
**Warden** to slay. Kills pay XP with floating gold popups; levels heal
you fully, toughen your blows, and offer a **boon**; every check is a
**visible d20**; locked doors force or pick open on rolls; rests are
safe only in the light and spawn real nightmares in the dark; the
**Guild Hound** fights at your side. And there is one pool of blood:
the character sheet's hp and the marine's health bar are the same
thing, kept at the same ratio everywhere.

## Founding (resembles D&D)

The first interactive map opens a world-paused **founding window** (the
engine is literally paused while you read). A name field (prefilled
"Delver") and three class cards; one click drives a real
`bd_dnd.CreationWizard` (standard array, preset scores, class skill
picks, `finish()`), no shortcuts:

| Card | Hit die | Saves | Skills | Active ([C]) |
|------|---------|-------|--------|--------------|
| **Fighter** | d10 | str/con | athletics, perception | **Second Wind**: heal hit die + level, sheet and body |
| **Rogue** | d8 | dex/int | sleight_of_hand, perception | **Uncanny Dodge**: 5 seconds at ~1/3 incoming damage; also picks locked doors (DEX sleight_of_hand, DC - 2) instead of bashing |
| **Cleric** | d8 | wis/cha | religion, insight | **Turn the Unholy**: 12 + 2/level fire damage to everything within 160 units |

Each class's level-1 feature grants the active's charge; its level-2
feature raises the maximum by one. **Custom Action 3** (auto-bound to
**C**, alias `class_active`) fires the active; with no charges the
message names the refill (the rest action). The party is the hero plus
the **Guild Hound**, a second `bd_dnd.Character` that a `bd_dnd.Companion`
(a friendly Demon) binds to: it spawns fit-checked near you each map,
fights what hurts you, what you hurt, and what hurts it, and if it
cannot close distance for 30 tics (a ledge, a wall, a lift) it teleports
to your side anyway.

## Blood is the body (health unification)

There is **one health pool**, kept at one ratio everywhere:
`sheet.hp / sheet.max_hp == pawn.health / 100`. Vanilla Doom health is
fully integrated; nothing is ignored and nothing is double-counted.

- **Pawn damage** removes `damage * sheet.max_hp / 100` sheet hp,
  immediately, through the `actor_damaged` hook. The event reports what
  landed **after armor**, so armor integrates naturally by reducing
  pawn damage pre-sync. Armor and ammo stay vanilla.
- **Pawn-health gains** the 10-tic map-local diff detector sees
  (medikits, stimpacks, soulspheres, `pawn.heal` from RPG effects like
  the reflex-save refund) heal the sheet by the same ratio.
- **RPG-side heals** (rests, Second Wind, level-up) heal the pawn back
  toward the sheet's ratio, upward only.
- **Level-up and map_load** hard-resync the pawn to the sheet's ratio
  (both directions), absorbing per-event rounding.

A `_syncing` guard (the same pattern the companion module uses) keeps
the resync's own corrections from feeding back. The Delver window and
the strip tell the one truth: `Blood 8/12` next to `Body 67`.

## Skills that matter in Doom

Proficiency (+2) matters in every one of these, and each bonus shows
up in the Delver window with its live context:

| Skill | Doom effect |
|-------|-------------|
| **Athletics** | Door bash (STR, DC 15). **CQB training** (proficient): +2 damage on your hits within 96 units, through the `actor_before_damage` filter, distance from the event's refs. |
| **Perception** | **Trap sense**: the first entry per map into a sector with `sector.damage > 0` (nukage, lava) rolls Perception vs DC 12 through the visible announce path ("Your skin prickles: the floor is death here.", once per map). **Dead-eye** (proficient): +1 damage on your hits beyond 512 units, same filter. |
| **Sleight of hand** | Rogue lockpicking: doors pick with DEX sleight_of_hand at DC - 2 instead of STR bash. |
| **Religion** | Proficient: the sanctuary rest threshold drops 160 to 140, and the nightmare save rolls **WIS** instead of DEX ("faith wards the dark"; the announced check names the ability actually rolled). |
| **Insight** | **Examine**: a 7-tic geometric probe (aim cone + native sight check, fires nothing) reads the crosshair monster; the examine line shows name + HP for everyone, plus its XP value (`DEFAULT_XP_TABLE`) and a threat note ("deadly for your level" when its XP >= 4 x 25 x your level) only when Insight-proficient. Cached by handle identity; headless-safe. |

## The Delver window (Q)

**Custom Action 1** (auto-bound to **Q**, alias `toggle_sheet`) opens
the full character window, every value read live: **Body** (Blood bar,
sheet pool, plus a body bar for the pawn), **Experience** (XP bar and
"N to level L+1"), **Abilities** (six scores with modifiers), **Vocation**
(class, hit die, saves, level features), **Skills** (each class skill's
total bonus, modifier + proficiency + mastery, with its live Doom
context string: "bash DC 15: need 11+ on d20; CQB +2 within 96u",
"pick DC 13: need 7+", "sanctuary at 140; wards the nightmare (WIS)",
"examine the crosshair"), **Active** (charges and the refill hint),
**Contract** (both objectives with progress, the map's modifier, and
the depth), **Examine** (the crosshair target's data), and **Boons**
(pending and taken this run).

## The HUD strip

Two `bd.draw_text` lines (35-tic refresh), plus a third only while the
crosshair holds a monster:

```
LV n  XP x/y (need N)  Blood s/max  Body hp  [C] <active> xN
Rest: sanctuary|the dark dreams  Contract: tribute k/N - Warden alive|slain  [MODIFIER]  Depth n
Warden of MAP01 (Demon)  HP 240/240  250 XP  deadly
```

Segments that do not apply are omitted (no contract on empty maps, no
bracket on a plain contract). Display-list ids live in the documented
block in `content.py` (887100-887102, 887300+).

## Depth and contract modifiers (replayability)

- **Depth** (`bd.state["delve_depth"]`, contracts completed, persisted):
  Warden health multiplier 2.5 + 0.25/depth (cap 5), Warden XP
  multiplier 5 + depth (cap 10), tribute goal +1/depth (cap census - 1),
  contract reward 200 + 50/depth. The strip carries it.
- **One modifier per map**, rolled deterministically from
  `bd.rng(bd.state["delve_seed"] (default 29) + byte-sum(map name))`,
  announced center-screen (no screen tint): **IRON WARDEN** (Warden
  health x2 more, XP +2 shares), **HORDE** (tribute x2 capped
  census - 1, +100 XP), **DARK DELVE** (sanctuary +20, nightmare
  DC +2, contract XP x1.5), **GUILD BOUNTY** (contract XP x2),
  **BLOODHOUND** (the hound at +50% hp this map), and a plain contract
  at ~30% weight. The weighted table lives in `content.py`.

## Level-up boons

Every level-up queues a **pick-one-of-three** in a world-paused chooser
(mouse buttons or keys 1/2/3, all driving the same `systems.pick_boon`
the autotest calls): **Toughness** (+2 max sheet hp), **Deadly** (+1
damage through the level filter, stacking), **Prepared** (+1 max
class-active charge). Pending and taken boons persist through bd.state
save/load handlers.

## Doors, saves, and the combat loop

- **Any locked door** in the game opens on a check instead of its key:
  `DoorBashRules` dispatches a card-lending `LockedDoorCheck` per locked
  line (locks 1-3 and 129-134 all accept the Doom key cards, per
  `wadsrc/static/lockdefs.txt`; refused lines are remembered and never
  spammed). The first touch of a line toasts "Barred. Use it again to
  force it." Fighters and Clerics bash (STR athletics, DC 15); Rogues
  pick (DEX sleight_of_hand, DC 13). MAP02's red door on line 111
  survives only as the autotest's real locked-door fixture, commented
  as such in `content.py`.
- **Reflex save**: any incoming hit rolls an announced DEX save vs DC 12
  (35-tic cooldown): half is refunded on a success, all of it on a
  natural 20. The refund heals the body, so the sheet gains the same
  ratio through the health sync. Because it rolls mid-combat on every
  hit, it skips the center-screen banner (`announce_check(center=False)`)
  and shows only the floating d20 readout.
- **Kill XP everywhere**: the full Doom/Heretic/Hexen table credits the
  local player's kills exactly (`track_xp_from_kills`,
  `player_index=0`), with a floating gold "+N XP" over each credited
  kill.
- **Levels**: level-up heals sheet and pawn fully (hard resync), fires
  a gold ring burst, a chime, and "LEVEL N", queues a boon, and each
  level past the first adds +1 damage to every hit you land on a
  monster (capped, through the `actor_before_damage` filter).
- Fanfares stay in the world and the HUD: no full-view screen tints.

## Architecture

Four modules, all listed in the `PYTHON` manifest in load order (the
engine executes every manifest line; siblings reach each other through
`sys.modules`):

| File | Role |
|------|------|
| `pyscripts/content.py` | **Pure data + factories.** The three classes with their level-1/level-2 active features, preset standard arrays and card prose, `make_delver`, the contract factory, skill constants (CQB/dead-eye/trap-sense/religion/examine), the health-unification contract, the modifier table, boon definitions, tuning, the MAP02 autotest door fixture (clearly commented), display-list ids, prose. No engine calls at import. |
| `pyscripts/systems.py` | **Behavior.** The health sync (`arm_health_sync`, the actor_damaged hook, ratio heals, hard resync), `hit_bonus_for` (level + Deadly + CQB + dead-eye), `announce_check`, trap sense, the examine probe and grading, religion-aware rests, the founding, the contract (census, crown, modifiers, depth, tribute/warden death handling, fanfare), `DoorBashRules`, `ReflexSaveRule`, class actives, boons (`queue_boon`/`pick_boon`/persistence), the cold restore, the two-line strip. Registers no events at import. |
| `pyscripts/ui.py` | **Interface.** The world-paused founding window, the full Delver window, and the world-paused boon chooser. No-op headless. |
| `pyscripts/main.py` | **Bootstrap + autotest.** Object wiring, custom actions and aliases, the map_load/founding/arm flow, and the full deterministic test schedule. |

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

Any map works, any game with a tabled bestiary (Doom, Heretic, Hexen):
the loop is identical everywhere.

Headless autotest (deterministic; asserts the founding through the same
function the cards call, all three classes' presets/features/actives,
the contract end to end (deterministic Warden pick, credited-only
tribute counting, the Warden kill and its depth-scaled bounty,
completion XP and fanfare, the deterministic modifier from the same
seed function), the visible-roll log (real line-111 bash drive, reflex
save, both rest branches, the WIS religion roll, the trap-sense roll),
health sync both ways (pawn wound to sheet ratio, diff-detected gain,
RPG heal to pawn ratio, level-up hard resync), CQB and dead-eye at
planted distances, the nightmare spawn (hostile, not FRIENDLY), the
examine probe and its Insight gating, class actives (heal/blur/burst),
the boons (queue on level-up, each card's effect, checkpoint
persistence), the companion (damage sync, teleport catch-up, the
stuck-teleport across a 136-unit drop, kill knell, revive), and a
checkpoint round-trip (RNG stream, CharacterState, PartyState,
companion rebind, contract + depth + modifier + boon state)):

```bash
BD_EXAMPLE_AUTOTEST=1 ./build/biaseddoom -headless \
    -iwad ~/games/doom2.wad -file examples/python/29_dnd_dungeon \
    -python -scripttest 1800 4 -nosound +map MAP02
# -> SCRIPT TEST: PASS
```

Universality smokes (headless founds the default Fighter; the contract
must crown a Warden and nothing may error):

```bash
./build/biaseddoom -headless -iwad ~/games/doom2.wad \
    -file examples/python/29_dnd_dungeon -python -scripttest 400 4 \
    -nosound +map MAP01
# -> SCRIPT TEST: PASS (Warden of MAP01 crowned, zero errors)
./build/biaseddoom -headless -iwad ~/games/doom2.wad \
    -file examples/python/29_dnd_dungeon -python -scripttest 400 4 \
    -nosound +map MAP03
# -> SCRIPT TEST: PASS (Warden of MAP03 crowned, zero errors)
```

Screenshot capture (writes `/tmp/the_delve.png` a few seconds in):

```bash
BD_EXAMPLE_SCREENSHOT=1 xvfb-run -a ./build/biaseddoom \
    -iwad ~/games/doom2.wad -file examples/python/29_dnd_dungeon \
    -python -nosound +map MAP01
```

## Probe appendix (autotest fixtures only)

The rebuild kept a handful of probed fixtures, all autotest-only.
MAP02's red door is engine special 13 (`Door_LockedRaise`) with args
`[7, 64, 0, 129, 0]` (lock 129 = red key) on **lines 111/112**,
approached from **(752, 1328, 48)** facing **angle 270**; the bash's
card-lending `activate(activator, clear=True)` consumes the special
(13 -> 0), which the test asserts directly. The stuck-teleport fixture
uses two probed spots: the corridor floor at **(820, 1236, 48)** and
the water hall at **(500, 1100, -88)**, with an unclimbable 136-unit
wall and ~348 units of separation (inside `teleport_distance`, so only
the stuck-teleport can close it). The burst fixture: GZDoom's radius
falloff subtracts the target's radius (probe-verified), so a
point-blank target takes the burst's full damage. The examine probe is
pure geometry (`bd.actor_refs` with the C++ sphere push-down, one
`bd.actor_field_batch` read, a bearing/pitch cone, `check_sight` for
walls); it fires nothing, because even a zero-damage `bd.line_attack`
spawns BulletPuffs and bullet decals along the trace. The trap-sense
stage drives a synthetic `sector_entered` over a temporarily damaging
sector (`sector.damage` is writable), because MAP02 carries no nukage
of its own.

## Expanding the Delve

- **A fourth class card**: add a `CharacterClass` (features granting the
  active's charge at level 1 and +1 max at level 2), a `CLASS_PRESETS`
  row, a `CLASS_ACTIVES` row, and an effect branch in
  `systems.use_class_active`. The founding window picks it up from
  `CLASS_LIST` on its own.
- **A fifth boon**: add a row to `content.BOONS` and an effect branch
  in `systems.pick_boon`; the chooser renders it from the table.
- **New modifiers**: add a weighted row to `content.CONTRACT_MODIFIERS`
  and wire its id into the `systems` helpers (warden mults, tribute
  goal, reward, sanctuary threshold, nightmare DC, hound buff).
- **Skill tuning**: `CQB_RANGE` / `CQB_BONUS`, `DEADEYE_RANGE` /
  `DEADEYE_BONUS`, `TRAP_SENSE_DC`, `RELIGION_SANCTUARY_LIGHT`,
  `DEADLY_BASE_BOUNTY` / `THREAT_MULTIPLIER` in `content.py`.
- **Rest tuning**: `SANCTUARY_LIGHT`, `NIGHTMARE_DC`,
  `FITFUL_HEAL_FRACTION`, `REST_COOLDOWN_TICS`, and the nightmare spawn
  table.
- **Contract/depth tuning**: `WARDEN_HEALTH_MULT(_PER_DEPTH/_CAP)`,
  `WARDEN_XP_MULT(_PER_DEPTH/_CAP)`, `TRIBUTE_GOAL_PER_DEPTH`,
  `CONTRACT_XP(_PER_DEPTH)`, and the modifier deltas.
- **Presentation**: re-tint the interface via `bd_horror.theme.PALETTE`
  before `apply()`, or add sections to the Delver window's body.
