# Ashvale Crossing

The capstone mini-RPG of the example suite: a playable hub on Doom II MAP01
that wires **every shipped RPG framework** into one campaign, and wires them
into *real Doom combat*. You found a character at the gate (**bd_dnd**
`CreationWizard` + three custom `CharacterClass` definitions), meet four
registered NPCs with persistent dispositions (**bd_npcs**), talk through
branching trees with skill checks and disposition gates (**bd_dialogue**),
trade, heal, and train (**bd_npcs** services + `ShopUI`), run quests that pay
XP and standings (**bd_quests** reward hooks), earn XP for every monster you
personally put down (**bd_dnd** `track_xp_from_kills`), hit harder as you
level (the `actor_before_damage` mutable filter), fire your class active on
a real key, and recruit **Korr** as a world-bound companion who actually
fights (**bd_dnd** `Party` + `Companion`).

The cast of Ashvale:

- **Sera Voss** (quest giver): smalltalk, a persuasion-gated rumor (DC 12)
  that points you at Korr (starting *The Watch Grows*), the yard job
  (accepting shifts her disposition +10), and, once the yard is clean, a
  "Who else can I trust here?" pointer that starts the same quest.
- **Dobb the Quartermaster**: a real shop (shells, clip boxes, stims, one
  one-off green armor) with buy/sell math and restock timers, plus the
  cache errand: he tells you exactly where the crate is, and his greeting
  mentions the stash while the job is still open.
- **Wren** (healer/trainer): heals for a fee behind a standing floor
  (her care is also the breather: a successful heal restores your class
  active's charges) and trains `medicine` mastery with d100 advancement
  checks.
- **Korr** (recruitable): joins once Sera is warm (>= 40) or the yard is
  cleared. His hub NPC retires (the static actor leaves the world for
  good, across savegames too) and his slot becomes the companion's
  first-spawn anchor: one friendly Demon follower that shadows you, fights
  what hurts you (and what you hurt), and whose actor health *is* Korr's
  RPG hp.

## The goal flow

1. **Found your character** in the gate window (the world pauses while it
   is open). Three staggered pointers afterwards teach the keys.
2. **Sera's yard job**: talk to her (**Q**, she carries a gold `!`), take
   "I need work.", and five risen dead spawn in the west hall (AMBUSH: they
   hold until you walk in). A gold beacon marks the yard while the job runs,
   and each kill flashes a counter ("Risen dead put down: 2/5"). Kills pay
   tabled XP on top of the quest reward.
3. **Dobb's cache**: he carries a gold `!` while the errand waits. Take
   "Anything you need moved?" and walk the entry hall west, past the pillar
   row, to the far west end; a beacon marks the crate. Grabbed it before he
   asked? Accepting pays out on the spot.
4. **Recruit Korr**: after the yard (or a successful rumor), Sera points at
   him; he carries the `!` while *The Watch Grows* is open. "Fight beside
   me." turns the hub NPC into your follower.
5. At all times the bottom strip shows your sheet in one line (name, class,
   level, XP, blood, and the class active with live charges and key) and
   the tracked objective in a second line, so the next step is always
   visible without opening the journal.

## Architecture

Four modules, all listed in the root `PYTHON` manifest in dependency order.
Each sibling self-registers a stable `sys.modules` alias (a live proxy), so
later entries reach it with a plain `import`, the same sibling-import
mechanics `hello_world` gets via `bd.import_script`.

| Module | Role |
|---|---|
| `pyscripts/content.py` | **Pure data + factories**: the three `CharacterClass` definitions (hit dice, class skills, level 1/2 features with apply lambdas that raise the active's resource pool, starting equipment), the `CLASS_ACTIVES` table (id, name, per-rest resource, one-line effect), the creation guidance layer (`CLASS_CONCEPTS`, `ABILITY_BLURBS`, `SKILL_BLURBS`, `MODIFIER_HINT`), three one-click `CLASS_PRESETS` per class with the shared `apply_preset`/`class_briefing` helpers, the `GAME_CONTENT` spawn-class table with cross-game notes, probe-verified MAP01 fixture constants, NPC spawn offsets/tints, marker/HUD display-list ids (95000+), the shop stock and service parameters, the three quests, all four dialogue trees with their condition/effect callables, and every string. No engine calls at import. |
| `pyscripts/systems.py` | **Rules and event wiring**: the shared `CreationWizard` and `finish_creation` (hero + `CharacterState` + equipment + level-up feedback + onboarding pointers), `wire_hero_progression` (kill XP from real kills via `track_xp_from_kills`, plus the `actor_before_damage` level/Skirmisher bonus filter), the class actives on Custom Action 3 (Second Wind heal, Uncanny Step blur, Lightkeeper radiant burst), the `NPCManager` registration, the currency spawn probe with fallback, quest wiring (kill/pickup trackers, the xp sink into the hero, the disposition sink into the store, yard spawn on accept, the kill-counter HUD line), `refresh_markers` + the persistent HUD strip on the display list, `recruit_korr()` (anchor capture, `manager.retire`, companion bind, watch_grows completion), the service ctx builder + `run_service`, Custom Action 1 (auto-bound to Q) -> the `custom_action` event / `pyui talk` -> `ui_command` -> `start_talk`, savegame cold-restore for hero/party, and the shop stock snapshot persistence. |
| `pyscripts/ui.py` | **The interface**: the founding window (name, class radios, a per-class briefing panel derived from the live class object, one-click preset buttons, score method radios, +/- ability buttons annotated with the derived modifier and per-ability blurbs, class-skill list with quota and per-skill blurbs, Finish + validation error line), the live-sprite dialogue window with standing-colored speaker and annotated choices, the talk prompt overlay (`manager.prompt` each frame while no session is active, with the live Custom Action 1 key swapped in), the `ShopUI` embed bound to Dobb (the "Show me your wares." choice sets `systems.shop_open`), plus `CharacterSheet` (K or Custom Action 2) and `JournalUI` (J). All inert headless; one `bd.warn` per frame at worst, balanced begin/end. |
| `pyscripts/main.py` | **Thin bootstrap**: the `BD_EXAMPLE_AUTOTEST=1` schedule (twenty-odd stages of assertions reading state out of `systems`) and the `BD_EXAMPLE_SCREENSHOT=1` pose. |

## What it teaches

- **The full creation flow.** `CreationWizard` validation (name, class,
  score method, standard-array multiset, point-buy budget, class-skill
  quota) surfaces as the UI's error line; `finish()` binds the class and
  applies the level-1 feature package automatically.
- **Legible character creation.** Picking a class opens a briefing panel
  (concept, hit die, primary ability, save proficiencies, trained skills,
  starting gear, and every level feature with its description) derived from
  the live `CharacterClass` via `content.class_briefing`, so the panel can
  never drift from the rules. Three one-click `CLASS_PRESETS` per class
  (e.g. Pit Fighter / Watch Sergeant / Old Survivor) fill the standard
  array and the skill picks through `content.apply_preset`, the same helper
  the autotest exercises; every ability row shows its derived modifier and
  a one-line blurb, and every skill pick names its governing ability.
- **Class progression as data.** Features at levels 1 and 2 are dicts with
  `apply` lambdas (seed a resource, raise its max through
  `character.resource_max` + `grant_resource`, set a mod note); level 4
  queues ASI points on `character.pending_asi` through `apply_class_level`.
- **RPG stats that mean something in the shooter.** `track_xp_from_kills`
  pays the hero tabled XP for every monster the player personally kills
  (exact pawn credit), and an `actor_before_damage` mutable-filter handler
  adds `min(8, level - 1)` plus the Scout's Skirmisher note to every hit
  the local player lands on a live monster.
- **Class actives as real buttons.** One usable active per class on
  Custom Action 3 (auto-bound to **C**, console alias `class_active`
  through the same `pyui`/`ui_command` bridge as `talk`): the Mercenary's
  Second Wind heals sheet *and* pawn by hit die + level, the Scout's
  Uncanny Step blurs for 175 tics (`damage_factor` 0.35, refreshed uses
  extend instead of stacking, defensively reset on `map_unload`), the
  Lightkeeper's Light is a `bd.radius_damage` fire burst. No charge left
  names the resource and the rest that refills it. The level-2 features
  raise the pools (Press On: +1 max second_wind; Uncanny Step grants the
  blur charge; Warding Flame: +1 max light), so progression reads on the
  button.
- **Registered NPCs.** `NPCDefinition` + `NPCManager.spawn_all` (friendly,
  still, tinted, facing the player) with stable `tid_base` TIDs, savegame
  re-bind by TID, `nearest()`/`prompt()` targeting, and disposition
  standings that never regress on load.
- **Retiring an NPC into a follower.** `recruit_korr()` captures the live
  hub actor's `(x, y, z, angle)` as the companion's `anchor`, calls
  `manager.retire("korr")` (the static actor is destroyed; `nearest()`,
  `prompt()`, savegame rebind, and later `spawn_all` calls skip him; the
  retired set round-trips through savegames; his disposition is kept),
  then binds the `Companion` at the anchor. One actor total, and the
  framework's proactive combat (engage what hurts you, what you hurt,
  what hurts it) makes him fight.
- **Every dialogue gate kind.** `skill_check` (persuasion rumor with both
  branches scripted through `session.rng`), `condition` (the handout hides
  once active; Dobb's errand hides once taken; Sera's trust pointer needs
  a cleared yard and an unstarted watch; the recruitment choice hides once
  Korr signed on), and a disposition/quest gate built from ctx data
  (`dispositions`, `quest_log`). Dobb's greeting text swaps at dialogue
  build time (the factory re-runs per session) to mention the stash only
  while the errand waits.
- **Quest reward hooks.** `log.on_xp_reward` feeds `hero.award_xp` (level-ups
  fire class features), `log.on_disposition_reward` feeds the store; item
  and message rewards apply natively. `recruit_korr()` completes
  watch_grows' objective through `log.complete_objective` (guarded on the
  quest being ACTIVE), which auto-completes the quest and pays out.
- **In-world markers + an objective HUD.** A 7-tic map-local
  `refresh_markers` task keeps a gold `!` over whichever NPC offers work
  and vertical beacons on the yard/cache while their quests run (cleared
  otherwise); a 35-tic task refreshes the two-line bottom strip (`bd.draw_text`,
  layer 8): hero status with the live class-active key, then the most
  recently started quest's current objective with its progress. The
  module-level `marker_state` dict mirrors what is showing so the
  autotest can assert the lifecycle headlessly.
- **Services and the shop.** `HealerService` (standing floor + fee + broke
  refusal), `TrainerService` (scripted d100 mastery advancement), and the
  `Shop` economy with stock counts, half-price sell-backs, and scheduled
  restocks.
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
  stock, NPC TIDs, the retired set, quest states, the HUD strip task, and
  the cleared markers all asserted post-load.
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
(`HereticImp`, `Ettin`, ...), so porting the example is a table swap.

## Running it

```bash
./build/biaseddoom -iwad /path/to/DOOM2.WAD \
    -file examples/python/33_rpg_campaign \
    -python -stdout +map MAP01
```

Found your character in the gate window: the world is engine-paused while
it is open, so you can read every option unmolested (the pause lifts the
moment Finish validates; the autotest and screenshot drivers disable this
so their scheduled steps keep ticking). Picking a class opens its briefing
(concept, hit die, saves, trained skills, gear, and features), and the
preset buttons (Pit Fighter, Ghost, Chirurgeon, ...) fill the standard
array and skill picks in one click; every ability row explains itself.
Three staggered pointers after the founding teach the keys: **Q** (Custom
Action 1) talks to whoever is closest (the prompt names them, their
standing, and the live binding; quest givers carry a gold `!`), **J**
opens the journal, **K** or **V** (Custom Action 2) the character sheet,
and **C** (Custom Action 3) fires your class active. The bottom strip
always shows where you stand and what to do next. Sera hands out the yard
job; Dobb's errand marks the cache; Wren patches and trains from the
dialogue tree; Korr signs on once you have proven yourself, and from then
on he fights at your side. The yard zombies hold (AMBUSH) until you walk
their hall.

Closed or hidden windows come back the same way they opened: the journal
and sheet re-toggle with **J** / **K** or Custom Action 2 (console
aliases `toggle_journal` / `toggle_sheet`), the shop reopens through
Dobb's dialogue (**Q**), and the
console cvar `py_imgui true` (or `bd.imgui.set_master_visible(True)` from a
script) brings back the whole overlay if you turned it off.

**Custom Actions**: this example uses **Custom Action 1** (talk,
auto-bound to **Q**), **Custom Action 2** (character sheet, auto-bound to
**V**), and **Custom Action 3** (class active, auto-bound to **C**). They
appear as "Custom Action 1/2/3" under Options -> Customize Controls,
Custom Actions; the auto-bind only fills in when the slot is unbound, so
your own rebinds always win and are shown live in the talk prompt and the
HUD strip.

Headless autotest (deterministic, scripted RNG doubles):

```bash
BD_EXAMPLE_AUTOTEST=1 ./build/biaseddoom -headless \
    -iwad /path/to/DOOM2.WAD -file examples/python/33_rpg_campaign \
    -python -stdout -nosound -nointro +map map01 -scripttest 1200 ff
# -> SCRIPT TEST: PASS
```

The autotest asserts: wizard validation errors and a full Mercenary
founding; every class preset applied through the shared `apply_preset`
helper and finished into a valid hero (scores, skills, class binding);
unit-level progression through level 4 (level-2 resource raises, ASI
queue); the spawned hub (four friendly tinted NPCs with stable TIDs, the
cache, an empty yard, the opening markers); nearest-NPC targeting; the
real console `talk` path into a disposition-carrying session; the handout
(quest active, Sera +10, five AMBUSH zombies, the "!" swapped for the yard
beacon); both persuasion branches (watch_grows starts only on success);
Dobb's fetch-accept dialogue (quest active, cache beacon up); the shop
(buy/sell math, stock counts, restock scheduling); the healer (heal, fee,
broke refusal); the trainer (scripted mastery tier + bonus); the quest
flow (pawn-sourced kills, kill XP plus quest XP, the level-2 feature
raising Second Wind to two charges, +20 disposition); Sera's post-yard
trust choice starting watch_grows; the cache pickup completing the fetch;
recruitment (Korr retired from the hub, exactly one companion actor at his
old slot, watch_grows completed); companion combat (the follower marks and
targets a monster the player hurt); the hero damage filter on a synthetic
event; the class active (no-charge message, then a scripted die heal on
sheet and pawn with the charge spent); and the checkpoint round-trip (RNG
stream, hero, dispositions, shop stock, party, companion rebind, NPC TIDs,
the retired set, quest states, the HUD strip task, cleared markers); and
the synthetic Custom Action path (action 1 press starts a talk session
with the documented event payload, action 2 press/release flips the hero
sheet both ways).

Documentation capture (poses the creation wizard beside a live Sera
conversation):

```bash
BD_EXAMPLE_SCREENSHOT=1 xvfb-run -a ./build/biaseddoom \
    -iwad /path/to/DOOM2.WAD -file examples/python/33_rpg_campaign \
    -python -nosound +map MAP01
# -> /tmp/ashvale.png
```

## Expanding it

- **New classes**: add a `CharacterClass` in `content.py` (plus a
  `CLASS_CONCEPTS` entry, a `CLASS_PRESETS` row, and a `CLASS_ACTIVES`
  row with its effect branch in `systems.use_class_active`); the wizard
  UI, briefing panel, preset buttons, sheet, progression, and the C key
  pick it up automatically.
- **New NPCs**: one `NPCDefinition` row (offset probed!) plus a dialogue
  factory; the manager, prompt, and persistence handle the rest.
- **New quests**: one `Quest` in `build_quests()` plus a tracker line in
  `systems._wire_quests()`; rewards flow through the sinks, and the
  markers/HUD pick up ACTIVE/INACTIVE state on their own for the givers
  already keyed in `refresh_markers`.
- **New services**: implement the `available`/`run` contract and add the
  instance to the definition's `services` tuple.
- **Re-voice**: every string lives in `content.py`; no logic lives there.
