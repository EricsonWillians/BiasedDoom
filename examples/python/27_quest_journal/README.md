# 27 — Whispers in the Walls

An occult-investigation mini-campaign on Doom II MAP01. Three leaves were
torn from the parish choirbook and fed to the walls; the mortar has been
whispering ever since. Recover the pages, climb to the ritual walkway, and
finish the rite the dead congregation began — then silence what the rite
summons.

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
| `pyscripts/content.py` | Pure data + factories: the `QUESTS` table, cast (pages / whispering dead / The Choir), map geometry, all prose. **No engine calls at import time.** |
| `pyscripts/systems.py` | Game rules built from content: quest trackers and hooks, the `RiteDirector` fail branch, blackout + fluorescent flicker on sector tag 5, stalker gating, spawning, tint re-application after loads. |
| `pyscripts/ui.py` | `Grimoire` — a `bd_horror.theme`-skinned journal window: per-quest ornament sections, bone names, blood/sickly/wound state markers, objective progress bars, faded completed/sealed entries. |
| `pyscripts/main.py` | Thin bootstrap: sibling imports, `engine_start`/`map_load` wiring, the manual `toggle_journal` alias/key bridge, the full deterministic autotest. |

A tiny `ZSCRIPT` lump provides the `RitualPage` pickup class; all behavior
lives in Python.

## The campaign

1. **The Desecrated Pages** — recover 3 pages (real world pickups tracked
   by `track_pickup`). Pays 8 shells and 75 favor (an `xp` reward wired
   to the ledger's `on_xp_reward` sink).
2. **Cleanse the Ritual Site** — step into the circle (sector tag 5,
   `track_sector`): the lights die, the whispering dead rise, the stalker
   wakes. Put down 3 of them (`track_kills`, exact player attribution).
   **Fail branch:** stepping out of the circle mid-rite fails the quest
   with an omen toast: the circle drinks your absence.
3. **Silence the Whispering Dead** — unsealed when the rite completes:
   put down The Choir, a liturgical-red BaronOfHell, for another 150
   favor.

## Adding a quest

One row in `content.py`'s `QUESTS` (id, name, description, objectives,
rewards, optional `reward_xp`), plus one tracker line in
`systems.setup_engine` if it needs event wiring
(`log.track_pickup/track_sector/track_kills`). The Grimoire, the favor
ledger, persistence, and toasts pick it up automatically.

## Controls

- **J** or console `toggle_journal` — open/close the Grimoire
  (wired manually via `alias` → `pyui` → `ui_command`).

## Autotest

`BD_EXAMPLE_AUTOTEST=1` with `-scripttest` drives the whole campaign
headlessly: the fail branch (synthetic sector-exit probe on a throwaway
quest log), a real three-page pickup walk, blackout fired/restored on the
ritual sector, player-attributed wave kills, the quest chain, the exact
`xp`-reward amounts landing in the favor ledger (75 then 225 total), the
journal toggle, and a checkpoint save/load round-trip.
`BD_EXAMPLE_SCREENSHOT=1` schedules a documentation capture instead.
