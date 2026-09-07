# Ashvale Crossing

The capstone mini-RPG of the example suite: a playable hub on Doom II MAP01
that wires **every shipped RPG framework** into one campaign. You found a
character at the gate (**bd_dnd** `CreationWizard` + three custom
`CharacterClass` definitions), meet four registered NPCs with persistent
dispositions (**bd_npcs**), talk through branching trees with skill checks
and disposition gates (**bd_dialogue**), trade, heal, and train
(**bd_npcs** services + `ShopUI`), run quests that pay XP and standings
(**bd_quests** reward hooks), level through class features, and recruit
**Korr** as a world-bound companion (**bd_dnd** `Party` + `Companion`).

The cast of Ashvale:

- **Sera Voss** (quest giver): smalltalk, a persuasion-gated rumor (DC 12)
  that activates a hidden starter quest, and the yard job. Accepting shifts
  her disposition +10.
- **Dobb the Quartermaster**: a real shop (shells, clip boxes, stims, one
  one-off green armor) with buy/sell math and restock timers.
- **Wren** (healer/trainer): heals for a fee behind a standing floor and
  trains `medicine` mastery with d100 advancement checks.
- **Korr** (recruitable): joins once Sera is warm (>= 40) or the yard is
  cleared, spawning a friendly companion that shadows you and whose actor
  health *is* Korr's RPG hp.

## Architecture

Four modules, all listed in the root `PYTHON` manifest in dependency order.
Each sibling self-registers a stable `sys.modules` alias (a live proxy), so
later entries reach it with a plain `import`, the same sibling-import
mechanics `hello_world` gets via `bd.import_script`.

| Module | Role |
|---|---|
| `pyscripts/content.py` | **Pure data + factories**: the three `CharacterClass` definitions (hit dice, class skills, level 1/2 features with apply lambdas, starting equipment), the `GAME_CONTENT` spawn-class table with cross-game notes, probe-verified MAP01 fixture constants, NPC spawn offsets/tints, the shop stock and service parameters, the three quests, all four dialogue trees with their condition/effect callables, and every string. No engine calls at import. |
| `pyscripts/systems.py` | **Rules and event wiring**: the shared `CreationWizard` and `finish_creation` (hero + `CharacterState` + equipment + level-up feedback), the `NPCManager` registration, the currency spawn probe with fallback, quest wiring (kill/pickup trackers, the xp sink into the hero, the disposition sink into the store, yard spawn on accept, prove-worth on clear), `recruit_korr()`, the service ctx builder + `run_service`, `bind e talk` -> `pyui talk` -> `ui_command` -> `start_talk`, savegame cold-restore for hero/party, and the shop stock snapshot persistence. |
| `pyscripts/ui.py` | **The interface**: the founding window (name, class radios, score method radios, +/- ability buttons with the point-buy budget line, class-skill list with quota, Finish + validation error line), the live-sprite dialogue window with standing-colored speaker and annotated choices, the talk prompt overlay (`manager.prompt` each frame while no session is active), the `ShopUI` embed bound to Dobb (the "Show me your wares." choice sets `systems.shop_open`), plus `CharacterSheet` (K) and `JournalUI` (J). All inert headless; one `bd.warn` per frame at worst, balanced begin/end. |
| `pyscripts/main.py` | **Thin bootstrap**: the `BD_EXAMPLE_AUTOTEST=1` schedule (twelve stages of assertions reading state out of `systems`) and the `BD_EXAMPLE_SCREENSHOT=1` pose. |

## What it teaches

- **The full creation flow.** `CreationWizard` validation (name, class,
  score method, standard-array multiset, point-buy budget, class-skill
  quota) surfaces as the UI's error line; `finish()` binds the class and
  applies the level-1 feature package automatically.
- **Class progression as data.** Features at levels 1 and 2 are dicts with
  `apply` lambdas (grant a resource, set a mod note); level 4 queues ASI
  points on `character.pending_asi` through `apply_class_level`.
- **Registered NPCs.** `NPCDefinition` + `NPCManager.spawn_all` (friendly,
  still, tinted, facing the player) with stable `tid_base` TIDs, savegame
  re-bind by TID, `nearest()`/`prompt()` targeting, and disposition
  standings that never regress on load.
- **Every dialogue gate kind.** `skill_check` (persuasion rumor with both
  branches scripted through `session.rng`), `condition` (the handout hides
  once active; the recruitment choice hides once Korr signed on), and a
  disposition/quest gate built from ctx data (`dispositions`, `quest_log`).
- **Quest reward hooks.** `log.on_xp_reward` feeds `hero.award_xp` (level-ups
  fire class features), `log.on_disposition_reward` feeds the store; item
  and message rewards apply natively.
- **Services and the shop.** `HealerService` (standing floor + fee + broke
  refusal), `TrainerService` (scripted d100 mastery advancement), and the
  `Shop` economy with stock counts, half-price sell-backs, and scheduled
  restocks.
- **A world-bound companion.** `recruit_korr()` builds the party, binds a
  `Companion` actor (friendly, `COUNTKILL` cleared, hp-synced both ways),
  and raises a toast; the descriptor rides `PartyState` through the
  checkpoint and re-binds by TID after the load.
- **A known framework edge, worked around.** `bd_npcs.Shop.buy` treats
  `give_inventory`'s return value as the delivery signal, but the native
  pickup path consumes health and armor items and folds ammo subclasses
  into their parent ammo (`ClipBox : Clip` grants Clip bullets), so a give
  that actually landed reports 0 and the framework refunds a successful
  purchase. `systems.AshvaleShop` (a documented `Shop` subclass) signals
  delivery by the give not raising, keeping Dobb's stock fully purchasable;
  the sell side stays honest (there is no ClipBox *item* to buy back in
  Doom II, only the parent ammo) and the autotest asserts both.
- **The checkpoint round-trip.** `bd.save_checkpoint`/`load_checkpoint`
  with the script RNG stream, hero, party, companion, dispositions, shop
  stock, NPC TIDs, and quest states all asserted post-load.
- **The currency probe pattern.** The shop wants a `Coin` class; the systems
  layer spawns one at a far map edge on the first `map_load` and destroys
  it, falling back to `Clip` (and logging the choice) when the probe fails.
  Note for porters: GZDoom ships the Strife `Coin` definition in the base
  zscript, so the probe succeeds even on Doom II and the shop trades in
  coins; on configurations without the class, clips take over. Heretic and
  Hexen mods should list `GoldCoin` first in `CURRENCY_CLASSES`.

## The map probes behind the numbers

MAP01 player 1 start is **(-96, 784, 56)** facing north. The west entry
hall (floor 56) fits the four NPC slots **(0, -48)**, **(-64, -48)**,
**(96, 16)**, **(0, 48)** relative to the start for a ZombieMan,
ShotgunGuy, DoomImp, and Demon respectively (each fit-probed with
`bd.spawn` without `force`), the five-yard zombie line at **x -224..-352,
y 800**, and the cache ClipBox at **(-416, 800, 56)**. Pillars reject
spawns at e.g. (-64, 800) and (-192, 800); probe before you move the
fixture. The engine exposes no IWAD query: `GAME_CONTENT` in `content.py`
holds the Doom II table with commented Heretic/Hexen substitutions
(`Gargoyle`, `Ettin`, ...), so porting the example is a table swap.

## Running it

```bash
./build/biaseddoom -iwad /path/to/DOOM2.WAD \
    -file examples/python/33_rpg_campaign \
    -python -stdout +map MAP01
```

Found your character in the gate window: the world is engine-paused while
it is open, so you can read every option unmolested (the pause lifts the
moment Finish validates; the autotest and screenshot drivers disable this
so their scheduled steps keep ticking). Then: **E** talks to whoever is
closest (the prompt names them and their
standing), **J** opens the journal, **K** the character sheet. Sera hands
out the yard job; Dobb's "Show me your wares." opens the shop; Wren patches
and trains from the dialogue tree; Korr signs on once you have proven
yourself. The yard zombies hold (AMBUSH) until you walk their hall.

Closed or hidden windows come back the same way they opened: the journal
and sheet re-toggle with **J** / **K** (console aliases `toggle_journal` /
`toggle_sheet`), the shop reopens through Dobb's dialogue (**E**), and the
console cvar `py_imgui true` (or `bd.imgui.set_master_visible(True)` from a
script) brings back the whole overlay if you turned it off.

Headless autotest (deterministic, scripted RNG doubles):

```bash
BD_EXAMPLE_AUTOTEST=1 ./build/biaseddoom -headless \
    -iwad /path/to/DOOM2.WAD -file examples/python/33_rpg_campaign \
    -python -stdout -nosound -nointro +map map01 -scripttest 1200 ff
# -> SCRIPT TEST: PASS
```

The autotest asserts: wizard validation errors and a full Mercenary
founding; unit-level progression through level 4 (level 2 feature, ASI
queue); the spawned hub (four friendly tinted NPCs with stable TIDs, the
cache, an empty yard); nearest-NPC targeting; the real console `talk` path
into a disposition-carrying session; the handout (quest active, Sera +10,
five AMBUSH zombies); both persuasion branches (hidden starter quest and
player log only on success); the shop (buy/sell math, stock counts,
restock scheduling); the healer (heal, fee, broke refusal); the trainer
(scripted mastery tier + bonus); the quest flow (pawn-sourced kills, XP,
level-up feature, +20 disposition, prove-worth on coattails, cache pickup);
recruitment (party of two, hp-synced companion, toast); and the checkpoint
round-trip (RNG stream, hero, dispositions, shop stock, party, companion
rebind, NPC TIDs, quest states).

Documentation capture (poses the creation wizard beside a live Sera
conversation):

```bash
BD_EXAMPLE_SCREENSHOT=1 xvfb-run -a ./build/biaseddoom \
    -iwad /path/to/DOOM2.WAD -file examples/python/33_rpg_campaign \
    -python -nosound +map MAP01
# -> /tmp/ashvale.png
```

## Expanding it

- **New classes**: add a `CharacterClass` in `content.py`; the wizard UI,
  sheet, and progression pick it up automatically.
- **New NPCs**: one `NPCDefinition` row (offset probed!) plus a dialogue
  factory; the manager, prompt, and persistence handle the rest.
- **New quests**: one `Quest` in `build_quests()` plus a tracker line in
  `systems._wire_quests()`; rewards flow through the sinks.
- **New services**: implement the `available`/`run` contract and add the
  instance to the definition's `services` tuple.
- **Re-voice**: every string lives in `content.py`; no logic lives there.
