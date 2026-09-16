# Pyre & Rime (Elemental Combat Rites)

A very dark elemental-combat mini-scenario on Doom II MAP01, built on
the **engine-shipped `bd_rpg` rules framework**
(`src/python/lib/bd_rpg/`) and dressed in the **`bd_horror`
presentation pack** (`src/python/lib/bd_horror/` — theme, toasts,
atmosphere). Both are importable from any Python mod with a plain
`import`.

You carry an **elemental focus** cycled with **F** between three sigils
— **Pyre** (the hunger that remembers the sun), **Rime** (the patience
of deep water, frozen mid-breath), and **Rot** (the quiet argument that
all things lose). Your native weapon hits are retyped and
affinity-scaled through the `actor_before_damage` mutable filter; **G**
hurls an elemental burst at the nearest horror, resolved end-to-end by
`bd_rpg.resolve_attack`. The horde ahead burns eagerly (imps are weak
to Pyre, x2) and the pinkies are rot-proof (immune to Rot, x0 —
immunity beats soak). Two orange **Cinder Thralls** set you ablaze when
their blows connect; the pale **Rime-Bound** chills your marrow — and
carries a **legendary relic** (an Ashen Idol, a Fingerbone Charm, a
Choir Bell) that surfaces with an omen toast when it dies. Every elite
death ends in a wet rattle (the bruise-colored screen fade was cut:
full-view tints fight your aim). Behind you
stands the **Warding Idol**: its blessing turns your skin to granite
(a stoneskin ward that cancels damage outright) — and its price is the
light, drunk from the alcove in a sector blackout: *"The idol drinks
the light."* A slow fluorescent corpse-light flickers over the horde
pen.

**The goal: break the horde.** Nine arena monsters carry the rite's
TIDs (four imps, two pinkies, three elites); when the last one falls,
the rite is complete: a center-screen call, a quest toast, and a
one-time **+100 souls** bonus through the kill-XP wiring. Feedback is
always on screen, never only in the closable HUD panel:

- A **control strip** at the bottom of the screen (a persistent
  `bd.draw_text` display-list item, refreshed by a 35-tic map-local
  task) shows the live focus, the *real* key bindings (resolved through
  `bd.input_binding`, so rebinding just works), and the horde count:
  `Horde: 4/9 broken`, flipping to the victory state when you win.
- Every weapon hit the focus filter retypes floats a **throttled
  affinity label** over the struck monster (one per monster per ~10
  tics): `x2 PYRE` in ember on a weakness, `x1/2 ROT` in sickly green
  on a resistance, `IMMUNE` in grey on an immunity. Neutral hits stay
  silent.
- The three elites wear **weakness labels** over their heads, read from
  their `actor_data` affix: `Cinder Thrall - weak: RIME`, `Rime-Bound -
  weak: PYRE`.

## Architecture

Four modules, all listed in the `PYTHON` manifest in load order (the
engine executes every manifest line; siblings also reach each other
through `sys.modules`, exactly how `hello_world` imports
`pyscripts/helper.py`):

| File | Role |
|------|------|
| `pyscripts/content.py` | **Pure data + factories.** Elements (colors, tones, prose), the elite roster, loot tables and relic names, probed TIDs, the horde/strip/feedback/weakness constants, prose. No engine calls at import. |
| `pyscripts/systems.py` | **Behavior.** Element registry, affinity tables, loot rules with relic toasts, the focus/burst rites, the damage filters, elite afflictions and death rattles, the Warding Idol, `SectorLightProgram` (light programs bound by sector *index*, because MAP01's arena is untagged), and the combat feedback layer: the control strip, per-hit affinity labels, elite weakness labels, and the "break the horde" win condition. Registers no events at import. |
| `pyscripts/ui.py` | **Interface.** The bd_horror-themed combat HUD: three sigil buttons (active ringed in blood), affliction bars toned per element, the souls bar, the kill/loot litany as omen lines. No-op headless. |
| `pyscripts/main.py` | **Bootstrap + autotest.** World spawning, sector probing, post-load rebinding, and the full deterministic test schedule. |

## What it teaches

- `DamageTypes`: registering custom damage types (`pyre`, `rime`,
  `rot`) alongside the eight builtins
- `set_class_affinity` / `set_affinity` / `affinity_of`: per-class and
  per-actor multipliers (imps x2 to Pyre, pinkies x0 to Rot)
- `resolve_attack`: d20 hit check, crits, NdM+K dice, affinity
  multiplier, flat soak with the min-1 rule — one call, one rich
  result dict
- `actor_before_damage` as a mutable filter: retyping vanilla hits into
  elemental ones (the focus filter) and cancelling damage outright
  (the Warding Idol's stoneskin ward)
- `StatusEngine`: refresh/stack/independent application rules, the
  built-in `burning` / `slowed` effects applied by elite blows, custom
  ticking effects, and TID-based persistence recipes
- `LootTable` / `LootRules`: weighted drops with rarity tiers and exact
  killer attribution — the Rime-Bound's relic drops at *legendary*
  rarity with a `bd_horror` omen toast
- `bd_rpg.track_kill_xp`: genre-neutral per-player kill XP pools (plus
  `award_kill_xp` for the one-time victory bonus)
- The canvas display list as combat feedback: a persistent
  `bd.draw_text` control strip with live `bd.input_binding` key names,
  throttled per-hit `bd.draw_world_text` affinity labels, persistent
  weakness labels over the elites, and a TID-counted win condition with
  a quest toast
- `RpgState`: player affinities and TID-tagged status timers
  round-tripping through `bd.save_checkpoint` / `bd.load_checkpoint`
- `bd_horror.atmosphere.LightProgram` subclassing:
  `SectorLightProgram` binds by sector index when the target map's
  sectors are untagged (probed at `map_load`); the Warding Idol's
  blackout and the horde pen's fluorescent corpse-light both ride it

## Running it

Interactive picker (auto-detects the engine and IWAD):

```bash
tools/play-python-example.py     # choose 31_elemental_combat
```

Or directly:

```bash
./build/biaseddoom -python -iwad ~/games/doom2.wad \
    -file examples/python/31_elemental_combat +map MAP01
```

Controls: **F** cycles the element sigil (or click a sigil in the HUD),
**G** hurls an elemental burst at the nearest horror; the control strip
at the bottom of the screen always shows the live bindings, your focus,
and the horde count, even after you close the HUD panel. Turn around at
spawn and take the Warding Idol — then watch its alcove go black. Pyre
burst the imps (x2), don't waste Rot on the pinkies (x0), read the
weakness labels over the elites, and put the Rime-Bound down for its
relic. Break all nine monsters to complete the rite (+100 souls).

Headless autotest (deterministic; asserts the resolver math branches,
the affinity tables, the status engine semantics, the stoneskin cancel,
the idol blackout and omen toast, elite afflictions with the negative
case, elite death rattles, loot drops with exact attribution, the
feedback layer (weakness labels, strip shape, per-hit label throttle),
then a
checkpoint round-trip verifying the loot RNG stream resumes exactly,
the status timers and player affinity survive, and the Rime-Bound's
legendary relic drops with its omen toast, and finally the win path:
the surviving horde dies by TID and the test asserts the completion
latch, the quest toast, the one-time bonus, and the strip's victory
state):

```bash
BD_EXAMPLE_AUTOTEST=1 ./build/biaseddoom -headless \
    -iwad ~/games/doom2.wad -file examples/python/31_elemental_combat \
    -python -scripttest 1400 4 -nosound +map MAP01
# -> SCRIPT TEST: PASS
```

Screenshot capture (writes `/tmp/pyre_rime.png` a few seconds in):

```bash
BD_EXAMPLE_SCREENSHOT=1 xvfb-run -a ./build/biaseddoom \
    -iwad ~/games/doom2.wad -file examples/python/31_elemental_combat \
    -python +map MAP01
```

## Expanding the rite

- **New elements**: add to `ELEMENTS`, `ELEMENT_COLORS`,
  `ELEMENT_TONES`, and `ELEMENT_PROSE` in `content.py`; the sigil row,
  focus cycling, and burst pick them up automatically.
- **New elites**: extend `ELITES` (TID, name, tint, affix); the touch
  handler maps affixes to statuses (`pyre` -> burning, `rime` ->
  slowed; add your own in `systems.elite_touch`), and the weakness
  label reads the affix through `AFFIX_WEAKNESS` automatically. New
  elites in `ELITES` join `HORDE_TIDS` and the win count on their own.
- **New relics**: extend `RELIC_LOOT` (spawnable class, weight, relic
  name); legendary toasts and the litany use the display-name tables.
- **More darkness**: probe more sectors at `map_load` and arm
  additional `SectorLightManager` programs — candle walks, faster
  fluorescents, or pickup-triggered blackouts.
- **Presentation**: re-tint the whole interface by editing
  `bd_horror.theme.PALETTE`, or retone the sigils via `ELEMENT_TONES`.
