# 28 — The Last Feeding

A Vampire: the Masquerade mini-chronicle on Doom II MAP01. You are a
13th-generation fledgling, thin-blooded and three nights dead, cornered
in a transit concourse where the district's last mortal herd still
huddles. The Sabbat have followed you across the river. Feed, keep the
Masquerade, and do not let the Beast drive.

Built on the shipped `bd_vtm`, `bd_quests`, and `bd_horror` packages:
generation-sized blood pools, hunger accrual and deterministic frenzy
checks, humanity degeneration rolls, blood-powered disciplines with
cooldowns (celerity, obfuscate, potence, dominate), feeding with
witnesses and Masquerade violations, faction reputation gating a quest,
and two `bd_horror`-skinned ImGui overlays.

## Architecture

Four modules, listed in load order in the `PYTHON` manifest. The libraries
are definition-only when executed; `main.py` imports them as siblings via
`bd.import_script` (the hello_world pattern) and owns all event wiring.

| Module | Role |
|---|---|
| `pyscripts/content.py` | Pure data: the mortal herd (names/tints), Sabbat pack and ambush pack, discipline set, faction matrix, quests, prose, and every hunt tunable. **No engine calls at import time.** |
| `pyscripts/systems.py` | The rules engine: stock `bd_vtm` wiring plus the hunt layer — cowering mortals, hunger-driven dread, darkness-aware feeding, the breach ambush, the frenzy heartbeat storm. |
| `pyscripts/ui.py` | `VitaeHud` (blood/hunger/humanity/exposure bars, blood-drop hunger row, dread) and `DisciplinePanel` (discipline buttons with cooldown sweeps, feed/frenzy actions, the active chronicle). |
| `pyscripts/main.py` | Thin bootstrap: sibling imports, `engine_start`/`map_load` wiring, the manual `toggle_hud` alias/key bridge, the full deterministic autotest. |

## The hunt layer

- **Cowering mortals** — a vessel that sees you within 128 units scrambles
  away on a slow scheduled thrust (capped speed).
- **Hunger ↔ dread** — the `bd_horror` dread meter mirrors the hunger
  track (20 dread per hunger level).
- **Darkness-aware feeding** — below sector light 96 the feeding witness
  radius halves (512 → 256). The dark keeps your secrets.
- **The answering pack** — a Masquerade breach (5 violations) spawns a
  Sabbat ambush around you, behind your back where possible, with a harm
  toast.
- **Frenzy heartbeat storm** — while the Beast drives, the heartbeat
  runs at maximum rate (the red screen pulses were cut: full-view tints
  fight your aim).

Engine-honesty note: `bd_vtm.feed`'s witness scan trusts the engine sight
check — anything alive with line of sight counts, including dropped items
near a corpse. Feed away from fresh kills, or deeper in the dark.

## The chronicle

- **The First Night** — put down the three Sabbat shovelheads
  (`track_kills`, exact player attribution). Completing it earns Anarchs
  standing...
- **Street Cred** — ...which unseals this faction-gated follow-up
  (`bd_vtm.requires_faction`, min standing 1).

## Adding content

A mortal is one row in `content.py`'s `MORTALS`; a quest is one row in
`QUESTS` plus a tracker line in `systems.build_chronicle`; a discipline is
one name in `DISCIPLINES` (factories live in `bd_vtm`). HUD, toasts, and
persistence pick new content up automatically.

## Controls

- **H** or console `toggle_hud` — open/close the Vitae HUD
  (wired manually via `alias` → `pyui` → `ui_command`).
- The Discipline panel closes with its window button; its buttons are the
  hunt's input path (disciplines, feeding, frenzy checks).

## Autotest

`BD_EXAMPLE_AUTOTEST=1` with `-scripttest` drives everything headlessly:
blood/hunger/humanity/frenzy determinism, discipline denial and cooldowns
(including potence), feeding and witnessed violations, the
darkness-halved witness radius (same feed, dark vs lit), cowering
displacement, the forced-breach ambush, the frenzy storm lifecycle, the
HUD toggle, and a checkpoint round-trip proving the script RNG stream and
the whole VtMState resume exactly. `BD_EXAMPLE_SCREENSHOT=1` schedules a
documentation capture instead.
