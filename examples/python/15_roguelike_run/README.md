# Roguelike Run

This example turns a Doom II playthrough into a seeded, checkpointed
**roguelike run**: monsters spawn with Diablo-2-style **affixes**
("Burning Zombieman of the Bear") that tint their sprites and change real
attributes, every map rolls depth-scaled **mutators** and crowns
gold-ringed **unique** monsters, kills feed an **XP/level** system, and
death ends the run — a stats panel counts down and reloads the run's
checkpoint.

The code is a small multi-module package (see *Code layout* below) — a
template for organizing bigger Python mods.

## Run rules

- **Deterministic seed.** Each map's content is rolled from streams seeded
  by `run_seed + map_salt(map) + depth`. `run_seed` lives in `bd.state`
  (persistent across maps, saves, and reloads) and defaults to **1337**;
  `map_salt` is a stable per-map offset (`str hash()` would vary between
  processes). A companion mod can set `bd.state["run_seed"]` before map
  load, or you can edit `DEFAULT_SEED` in `pyscripts/config.py`.
- **Depth scaling.** `bd.state["maps"]` counts maps entered (depth). The
  score multiplier is `1 + depth * 0.5`, mutators grow stronger with
  depth, and affix chances rise by 4% per map. From depth 3 on, there is a
  30% chance of a **second, distinct mutator** per map.
- **Persistent meta state.** `bd.state` carries `score`, `maps`, `kills`,
  `secrets`, `deaths`, `uniques`, `best_score`, `xp`, `level`, `runs` and
  the rolled `mutators` through the checkpoint save/load cycle.

## Monster affixes (Diablo-2 style)

Every spawned monster (map-placed or mid-map) rolls affixes from a
deterministic per-spawn stream: a 25% base chance (+4%/depth) of a
**prefix**, and affixed monsters have a 45% chance of a **suffix** on top.
The affix is visible three ways at once: the sprite is **tinted** in the
affix color, an overhead title shows the full name, and the attributes
really change:

| Prefix | Tint | Effect |
|---|---|---|
| Burning | orange | damage dealt x1.5 |
| Frozen | ice blue | speed x0.85, damage taken x0.9 |
| Venomous | green | health x1.3 |
| Charged | yellow | speed x1.4 |
| Spectral | pale | alpha 0.55, damage taken x0.7 |
| Stone | gray | health x1.8, speed x0.7 |
| Savage | red | damage dealt x1.3, scale x1.1 |
| Raging | bright orange | damage dealt x1.2, speed x1.2 |

| Suffix | Effect |
|---|---|
| of the Bear | health x1.5 |
| of the Wolf | speed x1.2 |
| of the Titan | scale x1.2, health x2.0, XP x2 |
| of Ruin | damage dealt x1.25 |
| of Greed | XP x2 |
| of Mist | alpha 0.75 |

A prefix-only monster is **affixed** (tint + title); prefix + suffix makes
a **champion**, which also gets a colored **ground ring** and a matching
health bar. Each affix also multiplies the monster's XP bounty. Titles are
sized with `bd.draw_world_text`'s resolution-independent `height=`
parameter and grow with tier — a unique's name towers over a plain
affixed monster's.

The record is packed into the monster's `args[4]` with a magic marker, so
it survives checkpoint save/load: after a reload the map rescan re-applies
the (non-serialized) tint and re-registers the visuals without re-rolling.

## Uniques

Each map crowns 1 **unique** (+1 with ELITE HUNT) from live monsters: two
prefixes + a suffix, **3x health**, a **gold ground ring**, gold health
bar and title, and **5x XP**. Killing one pays the bounty regardless of
who landed the hit, flashes a big gold `UNIQUE SLAIN!` announcement, and
plays `misc/secret`.

## XP and levels

Player-credited kills grant XP = `10 * affix XP multipliers * depth
multiplier` (uniques: x5 on top). Level n needs `250 * n` XP; leveling up
announces `LEVEL N`, heals you fully, and awards 150 bonus score per
level. The stats panel shows a live XP bar.

## Player effects

Player-centered feedback lives in `playerfx.py`, built on the same
`bd.draw_world_ring` primitive as the monster auras:

- **Level up**: a gold aura ring bursts around your feet (auto-expiring)
  with a pickup sound.
- **VAMPIRE proc**: every kill that feeds you 2 HP pulses a brief green
  ring under you.

No screen tints anywhere in this example: full-view flashes, fades, and
frame overlays fight your aim mid-combat, so first-person feedback stays
in the world (rings) or in the HUD (text color).

## Mutators

One is rolled per map (two from depth 3, 30% chance). The roll is revealed
with a `bd.ui.announce` center-screen announcement (big outlined title,
subtitle, color-matched screen fade, sound) plus a brief timescale dip.

| Mutator | Effect (depth = maps entered) |
|---|---|
| GLASS CANNON | Monster health x (50% - 5%/depth), floor 10% |
| SWIFT | `bd.set_timescale(1.15 + 0.05/depth)`, restored on map unload |
| FRAGILE | Monster health x 0.75 |
| TANK | Monster health x (1.5 + 0.25/depth) |
| VAMPIRE | Player-credited kills heal you 2 HP |
| RICH PICKUP | Rare pickups score double |
| ELITE HUNT | One extra unique is crowned this map |

Health mutations are applied to every live monster in a single
`bd.apply_actor_batch` call, and monster-free maps are a safe no-op.
Mutators are stored in `bd.state`, so a checkpoint reload restores them
without re-applying (deserialized actors already carry the mutated stats).

## Permadeath and checkpoints

Five tics after each map load the run saves with
`bd.save_checkpoint("roguelike", ...)`, landing in the save directory as
**`roguelike.zds`** (a `bd.ui.toast` confirms it). When you die, a red fade
and a centered `bd.ui` **RUN ENDED** panel show the run's stats and a
3-second countdown, then `bd.load_checkpoint("roguelike")` reloads the run.
If the checkpoint is missing, the panel tells you to restart the map instead.

**The checkpoint is the run: delete `roguelike.zds` from the save
directory to end a run and start over.**

## Scoring

Score gains are multiplied by `1 + depth * 0.5` and pop up as floating
gold `+N` text; kill popups show `+N XP` in the monster's affix color.

| Event | Points |
|---|---|
| Player-credited kill | 10 |
| Secret found | 100 |
| Rare pickup (Backpack / BFG / Invulnerability) | 50 (x2 with RICH PICKUP) |
| Unique slain | 50 (plus its 5x XP bounty) |
| Level up | 150 per level |
| Map clear (on map unload, not on death) | 25 |

## HUD

Slim by default so the view stays clear: a big outlined `LV / SCORE` line
with the live multiplier (top-left, flashes gold on gains), a dim context
line (`DEPTH · mutators · SEED`), and a `USER4: RUN STATS` hint. Press
**User4** to toggle the compact framed `bd.ui.panel` (top-right) with
combined stat rows and the XP bar; press again to hide. Rows re-render
only when their values change. All HUD text is sized with `bd.draw_text`'s
resolution-independent `height=` parameter (a normalized screen-height
fraction), so it stays equally readable at any resolution — unlike
`scale=`, which is a raw pixel multiplier.

Bind User4 in *Options -> Customize Controls -> Weapons* (it is listed as
"Weapon State 4") or with `bind f +user4` in the console.

## Code layout

`pyscripts/main.py` is only a bootstrap: it loads the modules below with
`bd.import_script` (registered in `sys.modules`, so siblings use plain
`import`) in dependency order, then installs run-state defaults. Each
module self-registers its `@bd.on` handlers at import.

| Module | Responsibility |
|---|---|
| `config.py` | Every tunable: seeds, chances, scores, XP table, canvas ids |
| `affixes.py` | Prefix/suffix tables, args[4] packing, titles, mod combining |
| `runstate.py` | `bd.state` accessors, deterministic streams, score/XP mutation |
| `mutators.py` | Per-map mutator table, roll/apply/reveal |
| `hud.py` | Slim strip, stats panel, popups, secret/pickup scoring events |
| `playerfx.py` | Player aura rings, level-up burst, affix damage feedback |
| `monsters.py` | Affix rolling/application, uniques, world visuals, kills |
| `deathloop.py` | Checkpoints, RUN ENDED panel, permadeath countdown |

## Running it

Interactive picker (auto-detects the engine and IWAD):

```bash
tools/play-python-example.py     # choose 15_roguelike_run
```

Or directly:

```bash
./build/biaseddoom -python -iwad ~/games/doom2.wad \
    -file examples/python/15_roguelike_run +map map01
```

Headless smoke test:

```bash
xvfb-run -a ./build/biaseddoom -stdout -nosound -nointro -python \
    -iwad ~/games/doom2.wad -file examples/python/15_roguelike_run \
    +map map01 -scripttest 350
```
