# 27: Whispers in the Walls

An occult-investigation mini-campaign on Doom II MAP01. Three leaves were
torn from the parish choirbook and fed to the walls; the mortar has been
whispering ever since. Recover the pages, climb to the ritual walkway, and
finish the rite the dead congregation began, then silence what the rite
summons.

The hunt explains itself in engine. A fresh map opens with the goal
centered on screen, followed by two staggered whispers naming the journal
key (J) and the markers. Pale rings and floating "!" labels ride every
uncollected page, a red beacon burns over the circle while the rite is
current, the rite's one rule is stated plainly the moment it begins, and
the Choir's arrival is announced center-screen.

Built on the shipped `bd_quests` and `bd_horror` packages: data-driven
quests with auto-wired trackers (pickups, sector tags, player-attributed
kills) and `xp` reward hooks that credit an example-local favor ledger
(toasted and totaled in the Grimoire), a blackout/flicker light program
on the ritual sector, a stalker director, diegetic toasts, and a
grimoire-styled ImGui journal.

## Architecture

Four modules, listed in load order in the `PYTHON` manifest. The libraries
are definition-only when executed; `main.py` imports them as siblings via
`bd.import_script` (the hello_world pattern) and owns all event wiring.

| Module | Role |
|---|---|
| `pyscripts/content.py` | Pure data + factories: the `QUESTS` table, cast (pages / whispering dead / The Choir), map geometry, marker and beacon presentation constants, all prose. **No engine calls at import time.** |
| `pyscripts/systems.py` | Game rules built from content: quest trackers and hooks, the `RiteDirector` fail branch, blackout + fluorescent flicker on sector tag 5, stalker gating, spawning, tint re-application after loads, the opening beat, and the guiding markers (`marker_state` + a 7-tic map-local sweep). |
| `pyscripts/ui.py` | `Grimoire`, a `bd_horror.theme`-skinned journal window: per-quest ornament sections, bone names, blood/sickly/wound state markers, objective progress bars, faded completed/sealed entries. |
| `pyscripts/main.py` | Thin bootstrap: sibling imports, `engine_start`/`map_load` wiring, the manual `toggle_journal` alias/key bridge, the full deterministic autotest. |

A tiny `ZSCRIPT` lump provides the `RitualPage` pickup class; all behavior
lives in Python.

## The campaign

1. **The Desecrated Pages**: recover 3 pages (real world pickups tracked
   by `track_pickup`; each uncollected page carries a pale ground ring and
   a floating `!` marker). Pays 8 shells and 75 favor (an `xp` reward wired
   to the ledger's `on_xp_reward` sink).
2. **Cleanse the Ritual Site**: step into the circle (sector tag 5,
   `track_sector`; a vertical red beacon burns over it while this quest is
   current): the lights die, the whispering dead rise, the stalker wakes.
   Put down 3 of them (`track_kills`, exact player attribution).
   **Fail branch:** stepping out of the circle mid-rite fails the quest
   with an omen toast: the circle drinks your absence. The rule is stated
   plainly when the rite begins: "Stay inside the circle until the dead
   fall. Leaving breaks the rite."
3. **Silence the Whispering Dead**: unsealed when the rite completes;
   the Choir's arrival is announced center-screen. Put down The Choir,
   a liturgical-red BaronOfHell, for another 150 favor.

## Guidance: markers and announcements

Three layers keep the hunt legible without opening the journal:

- **The opening beat** (fresh maps only, never on savegame loads): the
  goal centered on screen, then two staggered whispers, the journal key
  (J) first, the markers second.
- **Page markers**: while *The Desecrated Pages* is active, every
  uncollected page carries a bone-pale ground ring and a floating `!`,
  kept in sync by a 7-tic map-local sweep and cleared as each page is
  taken.
- **The rite beacon**: while *Cleanse the Ritual Site* is current, a
  vertical liturgical-red beam rises from the circle (clamped to the
  sector ceiling) so the walkway can be found from the halls below; it
  gutters when the rite ends, completed or failed.

Marker display-list ids live in a fresh 96000+ block documented in
`systems.py` (96000/96010 + page index for the rings and labels, 96020
for the beacon), clear of bd_horror's vignette base (888000) and its
toast/announce range (999000+). `systems.marker_state` mirrors what is
registered so the headless autotest can assert the lifecycle.

## Adding a quest

One row in `content.py`'s `QUESTS` (id, name, description, objectives,
rewards, optional `reward_xp`), plus one tracker line in
`systems.setup_engine` if it needs event wiring
(`log.track_pickup/track_sector/track_kills`). The Grimoire, the favor
ledger, persistence, and toasts pick it up automatically.

## Controls

- **J** or console `toggle_journal`: open/close the Grimoire
  (wired manually via `alias` → `pyui` → `ui_command`). The key is named
  in the opening whispers on every fresh map.

## Autotest

`BD_EXAMPLE_AUTOTEST=1` with `-scripttest` drives the whole campaign
headlessly: the fail branch (synthetic sector-exit probe on a throwaway
quest log), the opening beat (goal centered, both whispers toasted),
the page-marker lifecycle (lit over all three pages before the walk,
cleared after it), a real three-page pickup walk, the rite's plain
stay-inside rule toast, blackout fired/restored on the ritual sector,
the beacon lifecycle (lit mid-rite, guttered once the rite completes),
player-attributed wave kills, the quest chain, the Choir's center-screen
announcement, the exact `xp`-reward amounts landing in the favor ledger
(75 then 225 total), the journal toggle, and a checkpoint save/load
round-trip.

```bash
BD_EXAMPLE_AUTOTEST=1 ./build/biaseddoom -headless \
    -iwad ~/games/doom2.wad -file examples/python/27_quest_journal \
    -python -stdout -nosound -nointro +map map01 -scripttest 320 ff
# -> SCRIPT TEST: PASS
```

`BD_EXAMPLE_SCREENSHOT=1` schedules a documentation capture instead.
