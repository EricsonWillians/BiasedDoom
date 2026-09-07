# Embedded Python Example Suite

These are small, independently packageable mods for BiasedDoom Python API v2.
They complement the exhaustive `hello_world` integration fixture by teaching
one idea at a time.

> Python mods are trusted native-equivalent code. Review them and launch with
> `-python`; merely loading a PK3 does not opt in.

## Try them interactively

```sh
tools/play-python-example.py
```

An arrow-key menu that auto-detects your engine build and `doom2.wad`, then
launches the selected example on MAP01 -- no packaging or flags to remember.
Press `t` instead of ENTER for a headless self-check (`-scripttest`, wrapped
in `xvfb-run` when no display is available). `BIASEDDOOM_EXE` and
`BIASEDDOOM_IWAD` override detection.

| Example | Capability |
|---------|------------|
| [`01_lifecycle_events`](01_lifecycle_events/) | Startup, map, pre/tick/post, player, save/load, unload, shutdown |
| [`02_live_actor_handles`](02_live_actor_handles/) | GC-safe Actor handles, properties, relationships, flags, scheduled steering |
| [`03_combat_and_inventory`](03_combat_and_inventory/) | Inventory, healing, sounds, hitscan, missiles, direct/radius damage |
| [`04_player_controller`](04_player_controller/) | Pre-tick input override, button constants, FOV, native player filters |
| [`05_sectors_and_lines`](05_sectors_and_lines/) | Sector/Line handles, light, plane movement, line activation |
| [`06_filtered_events`](06_filtered_events/) | Class/TID/player filters, priorities, spawn/damage/death/destroy events |
| [`07_scheduled_tasks`](07_scheduled_tasks/) | One-shot/repeating tasks, self-cancel, explicit cancel, task counts |
| [`08_persistent_state`](08_persistent_state/) | Namespaced JSON state, save/load callbacks, reload-safe initialization |
| [`09_vfs_and_helpers`](09_vfs_and_helpers/) | Same-PK3 JSON, `read_text`, `import_script`, standard-library parsing |
| [`10_zscript_bridge`](10_zscript_bridge/) | Typed public ZScript methods, vectors/strings/actors, access rejection |
| [`11_batch_updates`](11_batch_updates/) | Lightweight handles, filtered ticks, bulk mutation, profiling |
| [`12_level_ui_audio`](12_level_ui_audio/) | Messages, sounds, music, action specials, console, level transitions |
| [`13_bullet_time`](13_bullet_time/) | Hold `User1` for slow motion with a screen tint and HUD banner |
| [`14_killstreak_announcer`](14_killstreak_announcer/) | Kill-streak combo meter with HUD text, sounds, and screen flashes |
| [`15_roguelike_run`](15_roguelike_run/) | Seeded roguelike: Diablo-2 monster affixes (tints/titles/attributes), uniques, XP levels, mutators, checkpoints |
| [`16_monster_health_bars`](16_monster_health_bars/) | Floating monster health bars with names, health-gradient colors, wall occlusion, and critical-state alerts |
| [`17_custom_status_effects`](17_custom_status_effects/) | Burning and Chilled monster statuses with bar tints, world labels, DoT, and slows |
| [`18_font_showcase`](18_font_showcase/) | Builtin/custom fonts, size ladders, named and RGB colors, animated rainbow text |
| [`19_elemental_squads`](19_elemental_squads/) | Sprite tints via `Actor.tint`, damage scalars, squad legend panel, tint reset toggle |
| [`20_boss_encounter`](20_boss_encounter/) | Mid-map boss upgrade, gold ring aura, screen-space boss bar, phase summons |
| [`21_treasure_goblin`](21_treasure_goblin/) | Mid-map spawning, fleeing monster steering, escape timers, score shower |
| [`22_patrol_routes`](22_patrol_routes/) | Waypoint orbit steering, world-line circuit visualization, AI handoff |
| [`23_damage_numbers`](23_damage_numbers/) | `actor_damaged` payloads, pooled rising combat text, crit styling |
| [`24_wave_defense`](24_wave_defense/) | Endless horde mode: map-native spawn pools, champion/unique affixes, wave mutators, weapon-ladder loot, boss waves, persistent bests |
| [`25_pickup_magnet`](25_pickup_magnet/) | `get_flag` pickup detection, item steering, ring-pool pull visualization |
| [`26_imgui_overlays`](26_imgui_overlays/) | Dear ImGui overlay via `bd.imgui`, `imgui_frame` event, menu bar, widgets |
| [`27_quest_journal`](27_quest_journal/) | **Whispers in the Walls**, occult investigation: chained `bd_quests` quests with `xp` reward hooks feeding a favor ledger (toasts + Grimoire footer), blackout/flicker rituals, grimoire UI (`bd_horror` theme + toasts) |
| [`28_vtm_chronicle`](28_vtm_chronicle/) | **The Last Feeding**, `bd_vtm` chronicle: cowering mortals, hunger-driven dread heartbeat, darkness-aware feeding, breach ambushes, themed vitae HUD |
| [`29_dnd_dungeon`](29_dnd_dungeon/) | **The Sunken Crypt**, `bd_dnd` delve across the whole WAD: `CreationWizard`/`CharacterClass` hero (level features, ASI), full-roster kill XP, level-toughened blows, any-locked-door bashing, reflex saves, Second Wind heal, sanctuary/nightmare rests that mend the pawn, world-bound companion, reliquary character sheet; MAP02 carries the probed set pieces (sheet/rest/second wind on Custom Actions 1/2/3, auto-bound Q/V/C) |
| [`30_conversation_quests`](30_conversation_quests/) | **The Confessor** — native ZSDF dialogue fixture: `conversation_started`/`conversation_reply` hooks, quest objective from a reply, candle-lit rite panel |
| [`31_elemental_combat`](31_elemental_combat/) | **Pyre & Rime** — `bd_rpg` rites: elemental focus, elite affixes, `actor_before_damage` wards, rarity loot omens, themed combat HUD |
| [`32_dialogue_trees`](32_dialogue_trees/) | **The Interrogation**, `bd_dialogue` trees: persuasion/intimidation skill checks, faction gates, dread-revealed hidden choices, an attitude-tracked hostile greeting, portrait dialogue UI (talk on Custom Action 1, auto-bound Q) |
| [`33_rpg_campaign`](33_rpg_campaign/) | **Ashvale Crossing**: capstone mini-RPG hub with `bd_dnd` character creation/classes (guided wizard: per-class briefing, one-click presets, annotated abilities/skills), `bd_npcs` dispositions/shop/services, `bd_dialogue` trees, `bd_quests` reward hooks, a recruitable companion, and a checkpoint round-trip (talk/sheet on Custom Actions 1/2, K kept) |
| [`34_scripted_menus`](34_scripted_menus/) | **Overture Menu Kit**: keyboard-first ImGui menu suite (title/pause/settings/credits screens, help and confirm-quit popups, docked tool panel) on the expanded `bd.imgui` API: runtime fonts, UI scale, style themes, key chords, `pyui` bridge (menu toggle on Custom Action 4, Esc fallback) |
| [`hello_world`](hello_world/) | Full lifecycle/savegame integration fixture used by CI-style testing |

## Build and run

Build every example:

```bash
./tools/build-python-examples.sh
```

Or select examples:

```bash
./tools/build-python-examples.sh 02_live_actor_handles 10_zscript_bridge
```

Then run one against a Doom II-compatible IWAD:

```bash
./build/biaseddoom -iwad /path/to/DOOM2.WAD \
    -file ./build/python-examples/02_live_actor_handles.pk3 \
    -python -stdout
```

Several interactive examples use the engine's `User1` through `User4` action
buttons. Bind those actions in Customize Controls before trying the indicated
keys. Examples that name `ZombieMan`, `Rocket`, `Clip`, or Doom sound/music
lumps are intentionally Doom II-oriented; the lifecycle, state, task, VFS,
and handle patterns themselves are game-independent.

Validate source and package structure with:

```bash
./tools/test-python-examples.sh
```

Pass `--iwad PATH` to additionally start MAP01 with every PK3 in a real engine
process, run its initial map/tick callbacks, and verify a clean Python-driven
exit.

See the [complete API guide](../../docs/scripting/python.md) for exact contracts,
security, performance budgets, packaging, and savegame behavior.
