# Embedded Python Scripting

This is the complete authoring, API, security, persistence, interoperability,
packaging, debugging, and testing guide for BiasedDoom's third scripting
runtime.

Python does **not** replace ACS or ZScript. The engine initializes and runs all
three independently:

- ACS remains the map-special and compiled legacy scripting path.
- ZScript remains the engine-native object, actor, event-handler, UI, and
  gameplay extension path.
- Python is a trusted, embedded CPython path for synchronous real-time gameplay,
  orchestration, stateful callbacks, and integration with the other two systems.

The public Python API version documented here is `2`.

## Read The Security Rule First

> [!CAUTION]
> A Python mod is arbitrary native-equivalent code. It is **not sandboxed**.
> It can use modules such as `os`, access the user's files, start processes if
> the platform permits it, open network connections, or otherwise do anything
> the account running BiasedDoom can do. Load Python only from authors you
> trust as much as an executable program.

For that reason, finding a `PYTHON` manifest is not enough to execute it.
Python stays inactive unless the player explicitly opts in with `-python` or
has persisted `py_enabled=true`. `-nopython` always overrides the archived
setting.

The opt-in is process-wide. It approves every Python manifest in the loaded
resource set; it is not a per-PK3 permission prompt.

The isolated interpreter configuration disables `site`, user-site packages,
CPython signal handlers, command-line parsing by CPython, and `.pyc` writes.
Those settings make startup reproducible. They do **not** create a security
sandbox.

## Choose The Right Language

| Need | Usually choose | Why |
|------|----------------|-----|
| Existing map scripts and line specials | ACS | It is the established map scripting contract. |
| Defining new actor/state/weapon classes, event handlers, menus, or renderer UI | ZScript | It owns the engine's class model, state compiler, and UI scopes. |
| Real-time single-player game logic, orchestration, data processing, or bundled Python libraries | Python | API v2 exposes live native handles, mutation, attacks, specials, scheduling, and a typed ZScript method bridge. |
| Deterministic multiplayer gameplay | ACS or ZScript | Python gameplay mutation is deliberately blocked in multiplayer and demos. |
| Compatibility with unmodified GZDoom | ACS or ZScript | BiasedDoom's Python contract is engine-specific. |

A mod can use all three. A PK3 may contain `PYTHON`, `ZSCRIPT`, and compiled ACS
`BEHAVIOR`/`LOADACS` content at the same time.

## Ten-Minute First Mod

### 1. Create the directory tree

```text
my-python-mod/
├── PYTHON
└── pyscripts/
    └── main.py
```

`PYTHON` must be at the resource root. Its name has no extension.
Use a differently named directory such as `pyscripts/` for the source files:
Windows and default macOS filesystems treat `PYTHON` and `python` as the same
name, so placing a `python/` directory beside the manifest is not portable.

### 2. Write the manifest

Put this in `my-python-mod/PYTHON`:

```text
pyscripts/main.py
```

### 3. Write the script

Put this in `my-python-mod/pyscripts/main.py`:

```python
import biaseddoom as bd


def on_engine_start(event):
    bd.log(f"Python API {bd.API_VERSION} started through {bd.RUNTIME}")


@bd.on("map_load")
def entered_map(event):
    bd.state["visits"] = bd.state.get("visits", 0) + 1
    bd.log(
        f"Entered {event['map']}; "
        f"savegame={event['from_savegame']}; visits={bd.state['visits']}"
    )


def on_tick(event):
    if event["level_time"] % (10 * bd.TICRATE) == 0:
        bd.log(f"Ten-second heartbeat at tic {event['level_time']}")
```

The example uses both callback styles:

- A conventional function name such as `on_engine_start` or `on_tick` is
  discovered after the module executes.
- `@bd.on("map_load")` explicitly registers any callable for an event.

### 4. Package it

From inside `my-python-mod`:

```bash
cmake -E tar cf ../my-python-mod.pk3 --format=zip PYTHON python
```

A PK3 is a ZIP file. Ordinary ZIP tools work too; make sure `PYTHON` is at the
archive root rather than inside an extra `my-python-mod/` directory.

During development, an unpacked directory can also be passed to `-file` if the
normal resource loader accepts it in the current build.

### 5. Run it

```bash
./build/biaseddoom \
    -iwad /path/to/DOOM2.WAD \
    -file /path/to/my-python-mod.pk3 \
    -python \
    -stdout \
    +logfile python-mod.log
```

The important flag is `-python`. `-stdout` and `+logfile` are strongly
recommended while developing.

### 6. Verify it

Open the console and run:

```text
py_status
```

A successful status reports that Python is compiled, active, and lists the
loaded module and callback counts.

## The `PYTHON` Manifest In Detail

BiasedDoom discovers one root-level `PYTHON` manifest from each loaded resource
container. A container is one WAD, PK3, or loaded resource file—not the merged
global VFS as a whole.

Each nonblank line names one Python source file in that same container:

```text
# Comments and blank lines are allowed.
pyscripts/main.py
pyscripts/monsters.py       # Inline comments are allowed.
"pyscripts/save support.py"
'pyscripts/quoted.py'
```

Rules:

1. The manifest is UTF-8. A UTF-8 BOM on its first line is accepted.
2. Leading and trailing whitespace is removed.
3. Text after `#` is removed before quote handling.
4. Matching single or double quotes around the entire remaining path are
   removed.
5. The path must be relative and end in lowercase `.py`.
6. Forward slashes are required.
7. Absolute paths, backslashes, and any path containing `..` are rejected.
8. The file must exist in the same resource container as the manifest.
9. Entries run in manifest order. Avoid listing the same file twice.
10. A bad entry is diagnosed and skipped; it does not suppress valid entries.

The same-container rule prevents accidental PK3 override behavior from making
one mod's helper resolve to another mod's file. It is a VFS isolation rule, not
a security boundary: trusted Python can still use normal operating-system file
APIs.

### Multiple mods

If three PK3s each contain a valid manifest, all three are loaded after the
player opts in. Their entry modules are independent Python module objects, and
their callbacks are appended in resource/manifest discovery order.

Do not design correctness around order between unrelated mods. In particular:

- Give CVar names a mod-specific prefix.
- Namespace persistent state under a unique key.
- Do not mutate an event dictionary received by a callback.
- Do not assume another mod has already run unless it is a declared part of
  the same package and manifest.

## Runtime Lifecycle

BiasedDoom embeds one CPython interpreter in the process. It executes on the
main engine thread under the normal GIL. Callbacks are synchronous: the engine
waits for each callback to return.

At startup, the engine:

1. Loads resources.
2. Parses existing actor definitions and ZScript.
3. Initializes the play simulation, including existing ACS/ZScript machinery.
4. Discovers root `PYTHON` manifests.
5. Checks the trust opt-in.
6. Initializes isolated CPython and the built-in `biaseddoom` module.
7. Executes each manifest entry.
8. Registers conventional callback names.
9. Dispatches `engine_start`.

If one module has a top-level syntax or runtime error, its traceback is logged,
that module is skipped, and other modules continue loading.

At normal engine cleanup, `engine_shutdown` runs before the interpreter is
finalized. Cleanup is not a safe time to create actors, change maps, or assume
level data still exists.

## Registering Callbacks

### Conventional names

These module-level names are recognized:

| Python name | Event |
|-------------|-------|
| `on_engine_start` | `engine_start` |
| `on_map_load` | `map_load` |
| `on_map_unload` | `map_unload` |
| `on_pre_tick` | `pre_tick` |
| `on_tick` | `tick` |
| `on_post_tick` | `post_tick` |
| `on_actor_spawned` | `actor_spawned` |
| `on_actor_died` | `actor_died` |
| `on_actor_damaged` | `actor_damaged` |
| `on_actor_before_damage` | `actor_before_damage` |
| `on_actor_destroyed` | `actor_destroyed` |
| `on_actor_revived` | `actor_revived` |
| `on_line_activated` | `line_activated` |
| `on_line_activation_failed` | `line_activation_failed` |
| `on_player_entered` | `player_entered` |
| `on_player_spawned` | `player_spawned` |
| `on_player_respawned` | `player_respawned` |
| `on_player_died` | `player_died` |
| `on_player_disconnected` | `player_disconnected` |
| `on_save` | `save` |
| `on_load` | `load` |
| `on_engine_shutdown` | `engine_shutdown` |
| `on_ui_command` | `ui_command` |

Each must be callable and accept one event dictionary.

### Decorator registration

```python
import biaseddoom as bd


@bd.on("actor_died", class_name="ZombieMan", tid=7001, priority=50)
def score_python_kill(event):
    actor = event["actor"]
    bd.log(f"{actor['class_name']} with TID {actor['tid']} died")
```

Unknown event names raise `ValueError` while the module loads. Passing a
non-callable raises `TypeError`.

The decorator accepts `every` (dispatch every N matching events), `priority`
(higher runs first), and native `class_name`, `tid`, and `player` filters. The
filters are applied before Python is called, which avoids spending the frame
budget on irrelevant high-frequency events.

The same callable registered twice for the same event is deduplicated. Two
different functions are two callbacks even if they have the same name.

### Failure isolation

If a callback raises an exception:

1. The full Python traceback is written to the engine log.
2. That one callback is marked failed.
3. It is skipped on later dispatches.
4. Other callbacks and other mods continue.
5. `py_reload` clears the failed status by rebuilding the interpreter.

This prevents a failing 35 Hz callback from flooding the log every tic.

## Common Event Fields

Every event dictionary contains:

| Key | Type | Meaning |
|-----|------|---------|
| `name` | `str` | Canonical event name. |
| `map` | `str` | Current map lump name, or `""` when no map is active. |
| `level_time` | `int` | Current level time in 35 Hz tics, or `0` outside a map. |

Treat the dictionary and nested snapshots as read-only input. Mutating them
does not mutate the engine and could affect later callbacks that receive the
same event object. The single exception is
[`actor_before_damage`](#actor_before_damage-mutable-pre-damage-filter), whose
dictionary is a deliberate write-back channel.

## Event Reference

### `engine_start`

Extra fields: none.

Runs once after all manifest entry modules have executed. Use it to initialize
keys in `bd.state`, validate required CVars/classes, or write a startup log.

It runs before the first playable map. Actor mutation functions therefore
raise `RuntimeError` here because no active level exists.

### `map_load`

Extra field:

| Key | Type | Meaning |
|-----|------|---------|
| `from_savegame` | `bool` | `True` when the map was entered by restoring a save. |

Runs after existing static and local ZScript `WorldLoaded` handlers and before
deferred ACS scripts are processed for an ordinary map entry.

On a savegame restore, the Python `load` callback runs after actor restoration
and before this `map_load` callback. This means `map_load` can immediately see
the restored `bd.state` and actors.

### `map_unload`

Extra field:

| Key | Type | Meaning |
|-----|------|---------|
| `next_map` | `str \| None` | Destination map when known; otherwise `None`. |

On an ordinary level transition, ACS unloading scripts and ZScript unload
handlers run before Python's callback. A savegame load can produce an unload
with `next_map=None` before it rebuilds the saved map.

Actor mutation and `execute_acs` raise `RuntimeError` during this teardown
callback. Record state or queue non-gameplay work instead.

Do not rely on `map_unload` being an engine-shutdown notification; use
`engine_shutdown` for process cleanup.

### `pre_tick`, `tick`, and `post_tick`

Extra field:

| Key | Type | Meaning |
|-----|------|---------|
| `paused` | `bool` | Engine pause state at dispatch time. |
| `python_time_us` | `int` | `post_tick` only: Python time already consumed this tic. |

BiasedDoom normally runs at `bd.TICRATE == 35` tics per second. `pre_tick`
runs before native player thinking, so `Player.set_input()` can affect the
current tic. `tick` runs after player thinking and local ZScript `WorldTick`,
but before actor thinkers. `post_tick` runs after actors and world specials.
The three phases share one whole-tic Python budget.

Keep this callback small. See [Performance](#performance-and-determinism).

### `actor_spawned`

Extra field:

| Key | Type | Meaning |
|-----|------|---------|
| `actor` | `dict` | Snapshot of the actor after the normal ZScript spawn handlers. |
| `actor_ref` | `Actor` | Live, lightweight handle to the same actor. |

Spawn/event timing inside the engine can be reentrant. If a Python callback
spawns and immediately damages an actor, do not assume its spawn and death log
messages will have intuitive wall-clock ordering. Use TIDs and explicit state,
not log order, for correctness.

### `actor_died`

Extra fields:

| Key | Type | Meaning |
|-----|------|---------|
| `actor` | `dict` | Snapshot of the dying actor. |
| `inflictor` | `dict \| None` | Snapshot of the inflicting actor when available. |
| `actor_ref` | `Actor` | Live handle to the dying actor while it remains valid. |
| `inflictor_ref` | `Actor \| None` | Live inflictor handle when available. |
| `attacker_ref` | `Actor \| None` | Live handle to the killer (`Die`'s `source`), or `None`. |
| `attacker_class` | `str \| None` | Killer's class name, or `None` when there is no attacker. |
| `attacker_player_index` | `int \| None` | Killer's player index when the attacker is a player pawn, else `None`. |

The `attacker_*` fields report `AActor.Die`'s `source`. For projectile kills
this is the **shooter** — `P_DamageMobj` receives the missile's `target` as
the source, while the missile itself stays in `inflictor`/`inflictor_ref`.
For hitscan and melee kills it is the attacker itself; for explosions the
bomb owner. It is `None` for environmental deaths (crushers, falling damage,
damaging terrain) and for kills through `bd.damage_actor`/`Actor.damage`
that pass no `source`.

The same value is exposed to ZScript as `WorldEvent.DamageSource` on
`WorldThingDied`.

### `actor_before_damage` (mutable pre-damage filter)

Extra fields:

| Key | Type | Mutable | Meaning |
|-----|------|---------|---------|
| `actor_ref` | `Actor` | no | Live handle to the actor about to take damage. |
| `inflictor_ref` | `Actor \| None` | no | Live handle to the inflicting actor (missile, puff, ...). |
| `attacker_ref` | `Actor \| None` | no | Live handle to the damage source, or `None`. |
| `attacker_class` | `str \| None` | no | Source's class name, or `None` when there is no source. |
| `attacker_player_index` | `int \| None` | no | Source's player index when it is a player pawn, else `None`. |
| `damage` | `int` | **yes** | Incoming damage, before armor and damage factors. |
| `damage_type` | `str` | **yes** | Damage type name (`"None"`, `"Fire"`, ...). |
| `flags` | `int` | no | Damage flags (`DMG_*`). |
| `angle` | `float` | no | Attack angle in degrees. |
| `cancel` | `bool` | **yes** | Set truthy to swallow the hit entirely. |

This is the one event whose dictionary is a **mutable contract**: the engine
reads three keys back after every handler has run. Mutate the event dict in
place to rewrite the incoming hit:

```python
@bd.on("actor_before_damage", class_name="ZombieMan")
def nerf_zombies(event):
    event["damage"] = event["damage"] // 2        # halve incoming damage

@bd.on("actor_before_damage", tid=7001)
def invulnerable_boss(event):
    event["cancel"] = True                        # no damage, no actor_damaged

@bd.on("actor_before_damage")
def everything_burns(event):
    event["damage_type"] = "Fire"                 # rewrite the damage type
```

Read-back rules are defensive: `damage` must be a number (ints and floats are
accepted) and is clamped to `[0, 2**31 - 1]`; `damage_type` must be a
non-empty string (an invalid or empty value keeps the original type and logs
a rate-limited script warning); `cancel` is a plain truthiness check. A
handler that raises mid-write is disabled as usual and whatever it wrote
before raising survives.

Setting `cancel` stops the hit completely: the damage pipeline returns 0, the
target keeps its health, and **no `actor_damaged` event fires** — consistent
with the rule that `actor_damaged` only reports resultant damage.

Scoping rules:

- Fires at the top of the native damage pipeline, **before** armor,
  damage factors, pain, and death processing.
- **Offline-only**: dispatch is skipped in multiplayer and demo
  playback/recording sessions, to protect synchronization determinism.
- ZScript `DamageMobj` overrides that never call `Super.DamageMobj()` bypass
  this filter entirely (they route around the native pipeline).
- When no handler is registered, the hook costs a single array read per
  damage event; keep handlers small regardless — this is the hottest gameplay
  path in the engine.

### Other real-time gameplay events

| Event | Extra fields |
|-------|--------------|
| `actor_damaged` | `actor_ref`, `inflictor_ref`, `source_ref`, `damage`, `damage_type`, `flags`, `angle` |
| `actor_before_damage` | mutable pre-damage filter — see [`actor_before_damage`](#actor_before_damage-mutable-pre-damage-filter) |
| `actor_destroyed` | `actor_ref`, final `actor` snapshot |
| `actor_revived` | `actor_ref` |
| `line_activated` | `line_index`, `actor_ref`, `activation_type` |
| `line_activation_failed` | `line_index`, `special`, `args` (5 ints), `actor_ref`, `activation_type`, `reason`, `reason_code` |
| `player_entered`, `player_spawned`, `player_respawned`, `player_died`, `player_disconnected` | `player_index`, `from_hub`, `actor_ref` |
| `item_picked` | `class_name`, `name`, `amount`, `player` |
| `secret_found` | `player`, `found_secrets`, `total_secrets` |
| `item_dropped` | `actor_ref`, `dropper_ref`, `player_index`, `class_name`, `amount` |
| `weapon_changed` | `player_index`, `weapon`, `actor_ref`, `player_ref` |
| `sector_entered`, `sector_exited` | `sector`, `tags`, `player_index`, `actor_ref` |
| `conversation_started` | `npc_ref`, `pc_ref`, `player_index`, `npc_class` |
| `conversation_reply` | `player_index`, `npc_ref`, `node`, `reply_index`, `log_number`, `log_string`, `next_node`, `item_changed` |
| `ui_command` | `command` |
| `custom_action` | `action`, `pressed` |

`item_picked` and `secret_found` are part of the
[gameplay director API](#gameplay-director-api).

#### `item_dropped`

Fires once per successful inventory drop, for both the player/console
`DropInventory` path and death drops (`DropItem` lists, `A_DropItem`), after
the tossed item exists in the world.

Extra fields:

| Key | Type | Meaning |
|-----|------|---------|
| `actor_ref` | `Actor` | Live handle to the dropped item. |
| `dropper_ref` | `Actor \| None` | Live handle to the actor that dropped it. |
| `player_index` | `int \| None` | Dropper's player index, or `None` when the dropper is not a player. |
| `class_name` | `str` | Dropped item's class name. |
| `amount` | `int` | Number of units dropped. |

#### `weapon_changed`

Fires when a player's ready weapon actually changes during weapon bring-up
(`PlayerPawn.BringUpWeapon` reassigning `ReadyWeapon` from the pending
weapon). Mods that fully override `BringUpWeapon` without calling the base
implementation bypass this event.

Extra fields:

| Key | Type | Meaning |
|-----|------|---------|
| `player_index` | `int` | Index of the player who switched weapons. |
| `weapon` | `str \| None` | New weapon's class name, or `None` when the player ends up empty-handed. |
| `actor_ref` | `Actor \| None` | Live handle to the new weapon actor, or `None`. |
| `player_ref` | `Player \| None` | Handle to the player. |

#### `sector_entered` / `sector_exited`

Per-player sector transitions, checked every post-tick. `sector_exited` fires
for the previous sector (when known) before `sector_entered` fires for the new
one. After map load the player's starting sector is reported as a
`sector_entered` without a matching `sector_exited`.

Extra fields:

| Key | Type | Meaning |
|-----|------|---------|
| `sector` | `int` | Sector array index being entered or exited. |
| `tags` | `list[int]` | All tags of that sector (same source as the `Sector` handle's `tags`). |
| `player_index` | `int` | Index of the player who crossed the boundary. |
| `actor_ref` | `Actor \| None` | Live handle to the player's pawn. |

#### `conversation_started`

Fires when a Strife conversation is successfully entered
(`P_StartConversation`, after all early-out checks): from the USE-talk path,
`AActor.StartConversation`, `bd.start_conversation`, and continuation nodes
shown when a reply keeps the dialogue open. Fires for every player, not just
the console player.

Extra fields:

| Key | Type | Meaning |
|-----|------|---------|
| `npc_ref` | `Actor \| None` | Live handle to the NPC being talked to. |
| `pc_ref` | `Actor \| None` | Live handle to the talking player's pawn. |
| `player_index` | `int \| None` | Talking player's index, or `None` when not a player. |
| `npc_class` | `str \| None` | NPC's class name. |

#### `conversation_reply`

Fires exactly once per committed conversation reply, from the netcode reply
handler (`HandleReply`, reachable only via `P_ConversationCommand`), on every
machine. Replies that are rejected (the default/empty reply, or missing
requisite items) do not fire it.

Extra fields:

| Key | Type | Meaning |
|-----|------|---------|
| `player_index` | `int \| None` | Index of the replying player. |
| `npc_ref` | `Actor \| None` | Live handle to the conversation NPC. |
| `node` | `int` | Dialogue node number (index into the map's Strife dialogue tree). |
| `reply_index` | `int` | Index of the chosen reply within the node. |
| `log_number` | `int` | Reply's quest-log number, or `-1` when none. |
| `log_string` | `str \| None` | Reply's raw quest-log text (may be a `$LABEL` string-table reference), or `None`. |
| `next_node` | `int` | Node the reply moves the NPC to, or `-1` when unchanged. |
| `item_changed` | `bool` | Whether the reply gave or took inventory items. |

A complete runnable fixture — a talkative NPC driven by engine-native ZSDF
dialogue data whose reply completes a `bd_quests` objective — ships as
[`examples/python/30_conversation_quests`](../../examples/python/30_conversation_quests/).

Notes revealed by that fixture:

- There is no `bd.*` function that picks a reply, and menu input cannot be
  injected from Python. The working programmatic path is the same one the
  menu uses: `ConversationMenu.SendConversationReply(node, reply)` (a
  UI-scope native static, so unreachable from `Actor.call_zscript`) invoked
  from a UI-scope `StaticEventHandler.ConsoleProcess` override registered via
  MAPINFO `gameinfo` `AddEventHandlers`, triggered by the `event <name>
  [args]` console command through `bd.execute`. The reply then travels the
  real `DEM_CONVREPLY` netcode into `HandleReply`.
- A conversation menu left open pauses the world in single player 20 gametics
  after opening (the menu's `Ticker` forces `menuactive = On`) unless the map
  sets `no_dlg_freeze`; driving code should close the menu after committing a
  reply, like the menu's own ENTER handler does.
- For numeric Strife logs (`log = "LOG#"`), `bd.player_log()` returns the
  `$TXT_LOGTEXT<n>` string-table *label*, not the resolved text — the engine
  stores the label so a language change re-translates it.

#### `ui_command`

Fires when the `pyui <name>` console command runs. This is the bridge that
lets console aliases and key bindings drive script UI — the problem being
that aliases can only run console commands, not Python.

Extra field:

| Key | Type | Meaning |
|-----|------|---------|
| `command` | `str` | The `<name>` argument passed to `pyui`. |

Canonical recipe — bind a journal toggle to a key:

```python
import biaseddoom as bd

journal_visible = False


def on_engine_start(event):
    bd.execute('alias toggle_journal "pyui journal"')
    # The player can now run: bind j toggle_journal


@bd.on("ui_command")
def ui_command(event):
    global journal_visible
    if event["command"] == "journal":
        journal_visible = not journal_visible
```

`pyui` is a plain console command (like `py_status`): it never mutates world
state, and it is a no-op when no script subscribed to `ui_command` or when
Python is not active.

#### `custom_action`

Fires on every press and release of the 32 generic custom action buttons
(`+pyaction1` .. `+pyaction32`), including synthetic
[`bd.set_custom_action`](#custom-actions) changes. See
[Custom Actions](#custom-actions) for the full input API.

Extra fields:

| Key | Type | Meaning |
|-----|------|---------|
| `action` | `int` | Action number, 1..32. |
| `pressed` | `bool` | `True` on the press edge, `False` on the release edge. |

Events are scanned once per gametic before `pre_tick` dispatch, after the
engine has latched that tic's input edges, so each transition fires exactly
once and `bd.custom_action_down()`/`bd.custom_action_mask()` already reflect
the new state when a handler runs. A press or release caused by a handler
(e.g. from `pre_tick`) surfaces on the **next** gametic's scan.

`line_activated` only fires when the line's special **succeeds** — a marker
special like `ACS_Execute` with no backing script fails silently. Subscribe
to `line_activation_failed` to debug dead triggers: it fires for any line
with a nonzero special whose execution failed, and reports the special
number and arguments so you can see exactly what the map asked for.

`line_activation_failed` also carries a machine-readable failure cause:
`reason_code` (int) and its stable string form `reason`:

| `reason_code` | `reason` | Meaning |
|---------------|----------|---------|
| 0 | `"none"` | No specific reason recorded. |
| 1 | `"unknown_special"` | Special number has no implementation. |
| 2 | `"script_not_found"` | ACS script number has no backing script. |
| 3 | `"locked"` | Activator lacks the required key. |
| 4 | `"activation_filtered"` | Rejected before execution (wrong side/type, monster on player-only line, handler veto). |
| 5 | `"insufficient_resources"` | Activator lacks required resources (reserved). |

`Actor` values can become invalid during or after destruction/unload. Check
`.valid` when retaining a handle and expect `ReferenceError` from operations
on stale handles.

### `save`

Extra fields: none.

Runs immediately before `bd.state` is JSON-encoded into the primary level's
save data. Use it to copy last-minute values into the persistent dictionary or
remove transient/non-JSON objects.

Actor mutation and `execute_acs` are blocked while this snapshot is being
prepared. Use `bd.state` for save bookkeeping here.

### `load`

Extra fields: none.

Runs only when a nonempty valid Python state dictionary was restored. Actors,
players, and other level objects have been deserialized before it runs.

For a normal savegame restore, the observed order is:

```text
map_unload (old world, when one exists)
load       (JSON state and actors restored)
map_load   (from_savegame=True)
tick
```

Old saves without `pythonstate` simply do not dispatch `load`.

### `engine_shutdown`

Extra fields: none.

Use this for final logging or Python-owned cleanup. Do not depend on a live
map, renderer, menu, or mutable playsim here.

## The `biaseddoom` Module

Import the engine module with:

```python
import biaseddoom as bd
```

### Editor completions (VSCode)

The `biaseddoom` module only exists inside the running engine, so editors
cannot resolve the import on their own. BiasedDoom ships a type stub at
`docs/scripting/biaseddoom.pyi` describing the full API: every function
signature, the `Actor`/`Line`/`Sector`/`Player` handle members, the
`bd.actors` registry, and all built-in actor class constants
(`bd.actors.DOOM_IMP` and friends).

To enable completions in VSCode, copy the stub into a `typings/` folder at
the root of the workspace you edit (Pylance's default stub path):

```
test.scripts/
    typings/
        biaseddoom.pyi
    scripts/
        main.py
```

Open the sidecar folder as the workspace, reload the window if prompted,
and `import biaseddoom as bd` gains full completions and inline
documentation. Mod-defined classes are not in the static stub; the
registry resolves them at runtime, and the stub's `__getattr__` keeps
them type-check clean.

The stub's actor-constant block is generated. With the game running,
`dumppystub <path>` refreshes that block in place from the live class
registry (or writes a full skeleton when the file does not exist) and
warns if any public API is missing from the stub.

### Images

`image()` accepts two texture forms. A **name string** is resolved with the
same `MiscPatch` + `TryAny` lookup as the display list's `draw_texture`,
with a **sprite-namespace fallback** so sprite frame names like `"PLAYA1"`
also work (precedence: MiscPatch lookup first, sprite fallback only when
the plain name misses). An **Actor handle** draws the actor's current
sprite frame texture (rotation 0, the front view). `w`/`h` default to 0,
meaning the texture's natural display size (its scale factors applied);
`uv0`/`uv1` select a sub-rectangle in normalized texture coordinates,
`tint` is an RGBA float multiplier, and a `border` tuple with alpha > 0
draws a 1px border of that color. `image_size(texture)` returns the natural
`(w, h)` without drawing and is legal outside `imgui_frame`.

### Docking

The vendored ImGui is the **docking branch** with
`ImGuiConfigFlags_DockingEnable` set. Call `dock_space_over_viewport()`
once per frame to cover the screen with a dockspace (flags are
`ImGuiDockNodeFlags_*`, e.g. `2` = PassthruCentralNode), or `dock_space(id)`
inside a window; windows can then be dragged by their title bar onto the
dockspace edges or onto each other, producing split panes and tab bars.
Use `set_next_window_dock_id(dock_id)` before `begin()` to dock a window
programmatically instead of by dragging (windows sharing one node become
tabs). **Multi-viewport is not supported**: `ImGuiConfigFlags_ViewportsEnable` is
deliberately left off because the overlay renders through the single
`F2DDrawer` canvas and cannot create platform windows, so windows never
leave the screen edge.

### Constants and attributes

| Name | Type | Value/meaning |
|------|------|---------------|
| `bd.API_VERSION` | `int` | Public API revision; currently `2`. |
| `bd.TICRATE` | `int` | Engine tic rate; currently `35`. |
| `bd.RUNTIME` | `str` | Runtime label; currently `"CPython"`. |
| `bd.PYACTION_COUNT` | `int` | Number of generic custom action buttons; currently `32`. See [Custom Actions](#custom-actions). |
| `bd.state` | `dict` | Shared JSON-persisted state dictionary. |
| `bd.on(name)` | decorator | Registers a callback. |

## Logging And Output

### `bd.log(message, level="info") -> None`

Converts `message` with `str()` and writes it to the engine console/log.

```python
bd.log("Loaded")
bd.log("That setting is suspicious", level="warning")
bd.log("Required resource missing", level="error")
bd.log({"structured": "values are stringified"}, level="debug")
```

`error` is red, and `warning`/`warn` is yellow. Other labels are printed as
`[Python:<label>]`.

### `print`, `sys.stdout`, and `sys.stderr`

The runtime replaces Python's stdout and stderr with line-buffered engine
writers:

```python
print("This appears as [Python] ...")
print("This is stderr", file=sys.stderr)
```

Partial lines are buffered until a newline, explicit flush, reload, or
shutdown. Prefer `bd.log` when severity matters.

### Automated testing (CI)

Two command-line options make scripts testable without a human:

- `-scripttest <tics> [ff]` — after the level loads, the engine runs it for the
  given number of tics, prints `SCRIPT TEST: PASS` or
  `SCRIPT TEST: FAIL (N Python error(s) in M tics)`, and exits with status
  0 (pass), 1 (errors), or 2 (no level loaded). Every reported Python
  error counts, including deduplicated repeats. The optional second value
  `ff` (default 1) is a fast-forward factor: the engine time scale is raised
  so `ff` times as many tics elapse per wall-clock second, and only every
  8th loop iteration is rendered (window events are still pumped every
  iteration). Tic counting is unchanged — the summary always reports the
  requested tic count — and the time scale drops back to `1.0` for the last
  `ff` tics so the run does not overshoot.
- `-pyerrorlog <file>` — appends each reported Python error, warning, or
  failed assertion as a JSON line (`time_ms`, `map`, `context`, `source`,
  `severity` — `"error"`/`"warning"`/`"assert"`, `repeats_suppressed`,
  `heartbeat`, `exc_type`, `exc_file`, `exc_line`, `exc_func`, `traceback`)
  for editors and CI tooling. The `exc_*` fields describe the innermost
  user-script traceback frame and are empty when unavailable.

```bash
biaseddoom -iwad doom2.wad -file mymap.wad mymap.scripts -python \
    -warp 1 -scripttest 700 -pyerrorlog /tmp/pyerrors.jsonl
```

Pair with a drive script that injects input through `bd.execute` (for
example `bd.execute("+forward")`) or `Player.set_input` to exercise
triggers unattended.

#### Headless runs (no X server)

`-headless` (or the `BIASEDDOOM_HEADLESS=1` environment variable) boots the
engine on a **null video driver**: SDL runs on its dummy video backend, no
window, GL context, or Vulkan device is ever created, and the frame render
is skipped entirely (game logic, the tic loop, netcode, GC, and Python
events are unaffected). This is the CI mode for runners without X11 — no
`xvfb-run` required:

```bash
biaseddoom -headless -iwad doom2.wad -file mymod -python \
    -warp 1 -scripttest 700 -pyerrorlog /tmp/pyerrors.jsonl
```

Headless caveats: screenshots print `Screenshot unavailable in headless
mode.` instead of writing a file (so golden-image tests still need a real
display), and the `imgui_frame` event never fires because nothing renders —
scripts that auto-warp from an `imgui_frame` handler should pass `+map`
explicitly in headless runs. Video-mode commands (`vid_setsize`, fullscreen
toggles) no-op with a notice. `bd.headless()` returns `True` in this mode so
test scripts can skip rendering-dependent assertions (the shipped examples'
autotests gate their ImGui draw checks on it).

#### Golden screenshots

Pixel-comparison regression testing lives **in the test harness, not the
engine**. The recipe:

1. **Capture goldens interactively.** Run the mod by hand and, at the frame
   you want to pin down, grab the frame from a script with
   `bd.execute("screenshot /absolute/path/to/golden")` (writes PNG; the
   `.png` extension is appended when missing). Alternatively use the
   `screenshot` console command or the `-shotdir` option.
2. **Check the golden PNGs into the mod repo**, next to the test scripts.
3. **In CI**, re-capture the same frame under `-scripttest` (same map, same
   tic — have the script request the screenshot at a fixed `bd.level_time()`)
   and compare:

```bash
# Run the engine with -scripttest as usual; the script captures /tmp/actual.png.
biaseddoom -iwad doom2.wad -file mymod -python -warp 1 -scripttest 700
python3 tools/compare_screenshots.py tests/golden.png /tmp/actual.png \
    --threshold 8 --max-diff-pct 0.5 --diff-out /tmp/diff.png
```

`tools/compare_screenshots.py` is **stdlib-only by design** (no Pillow): it
decodes PNGs with `zlib` + `struct` so it runs on bare CI runners. It
accepts 8-bit RGB/RGBA non-interlaced PNGs (all scanline filter types),
which is exactly what the engine's screenshot writer produces today — the
custom writer `M_CreatePNG`/`M_SaveBitmap` in
`src/common/textures/m_png.cpp` emits bit depth 8, color type 2 (RGB),
interlace 0. If the engine writer ever changes that format, this tool must
be updated.

Flags and exit codes:

| Flag / outcome | Meaning |
|---|---|
| `--threshold FLOAT` | Per-channel absolute tolerance, 0-255 (default **8**), absorbing nondeterministic dither/gamma noise. |
| `--max-diff-pct FLOAT` | Percent of pixels allowed to exceed `--threshold` (default **0.5**), absorbing moving particles/HUD flicker. |
| `--diff-out PATH` | Writes a grayscale PNG diff map (white = diff beyond threshold, black = same) for debugging. |
| exit `0` | PASS — images match within tolerance. |
| exit `1` | FAIL — tolerance exceeded, or dimension/format mismatch (the report includes dimensions, mismatched pixel count, mean absolute channel error, and worst channel delta). |
| exit `2` | Usage error or undecodable/unsupported input. |

All report lines are prefixed `GOLDEN TEST:` so harness logs can grep for
them cleanly.

### Where output goes, and copying it out

Everything above lands in the **in-game console** (`~`), the OS terminal
stdout, the DAP log event stream, and the `+logfile` file when one is
active. To copy console text to the OS clipboard (Windows / Linux / macOS):

- `copyconsole` — copy the whole scrollback; `copyconsole 50` copies the
  last 50 lines.
- With the console open, **drag the mouse** over the scrollback to select
  text (the selection is highlighted), then **Ctrl+C** copies just the
  selection. **Ctrl+A** selects the entire scrollback (you see everything
  highlighted) and copies it in one step. **Ctrl+C** with no selection
  copies the input line instead. Click once, press **Escape**, or start
  typing to clear the selection.

### Errors and tracebacks

Uncaught exceptions in callbacks print a full red traceback to the console.
Pending `print()` output is flushed first, so it appears before the error.
Identical consecutive errors are printed once and then summarized
(`... repeated N times; duplicates suppressed`), so a failing `tick`
handler cannot flood the console at 35 tracebacks per second.

### `bd.assert_true(cond, msg="") -> None`

Scripted test assertion. When `cond` is falsy it prints a red
`SCRIPT ASSERT FAILED: <msg> (<file>:<line>)` line with the caller's
location, feeds `-pyerrorlog` with `severity: "assert"`, and counts toward
the `-scripttest` failure total — but returns `None` instead of raising, so
one test run can report several failures. When `cond` is truthy it is a
no-op.

### `bd.warn(msg) -> None`

Prints a yellow `SCRIPT WARNING: <msg>` console line and feeds `-pyerrorlog`
with `severity: "warning"`, without counting as an error. Identical
consecutive warnings are deduplicated with the same repeat-summary cadence
as errors, tracked separately so warnings and errors never suppress each
other.

## Map And Time Queries

### `bd.current_map() -> str | None`

Returns the active map lump name, such as `"MAP01"`, or `None` outside a map.

### `bd.level_time() -> int`

Returns current level time in tics. It returns `0` when no level is active.

The callback event already contains these values. The functions are useful in
helpers called outside the immediate event function.

## Player Queries

### `bd.players() -> list[dict]`

Returns one snapshot per active player:

```python
[
    {
        "index": 0,
        "name": "Player",
        "in_game": True,
        "actor": { ... actor snapshot ... },
    }
]
```

`actor` can be `None` during lifecycle windows where the player has no pawn.

```python
for player in bd.players():
    pawn = player["actor"]
    if pawn is not None:
        bd.log(f"{player['name']} has {pawn['health']} health")
```

## Custom Actions

The engine ships 32 generic "Custom Action" buttons reserved for Python mods:
`+pyaction1` .. `+pyaction32` (`bd.PYACTION_COUNT` is `32`). They are
ordinary engine buttons, so they get conflict-free user key bindings, the
`bind` console command, and a Customize Controls page, and they are consumed
from Python through the [`custom_action`](#custom_action) event plus the
query/synthesis APIs below. They are **local-only input state**: they are
never added to the network usercmd, so they work headless, only drive the
local player's scripts, and are **not recorded in demos**.

Binding, from the console or a script:

```
bind q +pyaction1
```

or through the menu: **Options -> Customize Controls -> Custom Actions**.
All 32 actions are unbound by default; mods may suggest binds (for example
from `on_engine_start` with `bd.execute("bind ...")`) but must not overwrite
a binding the user already set without asking.

Event payload and ordering: `{"action": n, "pressed": bool}` (plus the
[common fields](#common-event-fields)); both edges fire, press first.
`bd.on("custom_action")` handlers run once per gametic before `pre_tick`
dispatch, after the engine latched that tic's key input, so state queries in
a handler already see the new state, and a synthetic change made by a handler
surfaces on the next gametic.

### `bd.custom_action_down(n) -> bool`

`True` while custom action `n` (1..32) is held. Raises `ValueError` when `n`
is outside 1..32.

### `bd.custom_action_mask() -> int`

Bitmask of the currently held actions: bit `n-1` is set while action `n` is
down. Useful for one-call polling of several actions inside a `tick` handler.

### `bd.set_custom_action(n, down) -> None`

Synthetically presses (`down=True`) or releases (`down=False`) action `n`,
driving the same button state as the bound key (or typing `+pyactionN` /
`-pyactionN` at the console), so the per-tic scan emits the same
`custom_action` event on the next gametic. Calls are idempotent: requesting
the current state is a no-op, so a mod may "ensure held" every tick without
re-firing events. Raises `ValueError` when `n` is outside 1..32.

### `bd.input_binding(command) -> str | None`

Reverse binding lookup: returns the display name of the first key bound to
the given console command, using the engine's canonical key names (for
example `"+pyaction3"` returns `"Q"` after `bind q +pyaction3`, or
`"Mouse1"`/`"Space"` for those keys), or `None` when the command is unbound.
The result stays valid until the binding changes.

```python
import biaseddoom as bd


@bd.on("custom_action")
def custom_action(event):
    if event["action"] == 1 and event["pressed"]:
        bd.log("custom action 1 pressed")
    if not event["pressed"] and bd.custom_action_mask() == 0:
        bd.log("all custom actions released")


@bd.on("engine_start")
def engine_start(event):
    if bd.input_binding("+pyaction1") is None:
        bd.execute("bind q +pyaction1")  # only suggest; respect user binds
    bd.set_custom_action(2, True)  # press; event fires on the next gametic
    bd.schedule(release_action_2, delay=35)


def release_action_2():
    bd.set_custom_action(2, False)  # release; event fires on the next gametic
```

A press and its release must happen in different gametics: both calls in the
same handler cancel out before the next scan runs, and no event is emitted.

## Live Actor Handles (API v2)

This is the default API for real-time querying and mutation. The legacy
snapshot/TID functions survive only for compatibility and JSON persistence —
see [Legacy Snapshot API](#legacy-snapshot-api-json-persistence) below. New
code should use native handles:

```python
pawn = bd.player().actor
monster = bd.spawn(
    "ZombieMan", pawn.x + 128, pawn.y, pawn.z, tid=9001, force=True
)
monster.target = pawn
monster.health = 75
monster.set_velocity(4, 0, 2)
```

`Actor`, `Player`, `Sector`, and `Line` are small C++-backed Python objects;
property reads and writes cross directly into the playsim instead of rebuilding
dictionaries. They may only be used on the engine callback thread. Mutating
operations require an active map and a single-player, non-demo session.

### Handle lookup and lifetime

| Function | Result |
|----------|--------|
| `bd.actor_ref(tid)` | First live `Actor` for a nonzero TID, or `None`. |
| `bd.actor_refs(class_name=None, tid=0, limit=4096, subclasses=True, sphere=None, z=None)` | Filtered live actors; `limit` is `0..1000000`. |
| `bd.spawn(class_name, x, y, z, angle=0, tid=0, force=False)` | Newly spawned `Actor`. |
| `bd.player(index=consoleplayer)` / `bd.player_refs()` | One/all in-game `Player` handles. |
| `bd.sector(index)` / `bd.sectors(tag=None)` | Sector by array index or all/by tag. |
| `bd.sector_at(x, y)` | `Sector` containing the point, or `None`. |
| `bd.actors_in_sector(sector)` | Live `Actor` handles inside a `Sector` handle or sector index. |
| `bd.line(index)` / `bd.lines(line_id=None)` | Line by array index or all/by line ID. |

Actor handles are GC-aware and safe to retain across tics. `.valid` becomes
false after native destruction or map unload; using stale actors raises
`ReferenceError`. Player handles become invalid when the player leaves.
Sector/line handles are generation-checked and raise `ReferenceError` after
their map unloads. Do not place handles in `bd.state`; save snapshots, TIDs,
player indices, tags, or line IDs instead.

`actor_refs` filters are pushed down into the native thinker scan and combine
(AND) without per-actor Python crossings: `class_name` is case-insensitive
and matches derived classes unless `subclasses=False` (exact class match);
`sphere=(x, y, r)` keeps actors within 2D distance `r` of `(x, y)`, and the
optional `z` adds a vertical band `|actor.z - z| <= r`.

```python
bosses = bd.actor_refs(class_name="BaronOfHell", subclasses=False)
nearby = bd.actor_refs(sphere=(pawn.x, pawn.y, 512), z=pawn.z)
floor_sector = bd.sector_at(pawn.x, pawn.y)
occupants = bd.actors_in_sector(floor_sector) if floor_sector else []
```

### Per-actor data: `bd.actor_data(ref)` / `bd.actor_data_drop(ref)`

`bd.actor_data(ref)` returns the actor's persistent per-actor data `dict`,
creating it on demand; two calls with handles to the same actor return the
same dictionary, so scripts can attach arbitrary Python state to a live actor:

```python
data = bd.actor_data(zombie)
data["enraged"] = True
data["ticks_left"] = 140
```

Lifetime and purge semantics:

- The dict is keyed by the actor's handle slot **and generation**, so a
  recycled slot can never resurrect a previous actor's data.
- It is purged automatically when the actor is destroyed (detected at handle
  resolution/GC time) and when the map changes or unloads.
- It is **not saved in savegames**. To persist per-actor state, store it in
  `bd.state` keyed by TID and rebind handles after load — see
  [Rebinding actor handles after load](#rebinding-actor-handles-after-load).
- A stale handle raises `ReferenceError`, exactly like any other handle use.

`bd.actor_data_drop(ref)` drops the dict if one exists and returns `True`
when one existed. Drop is idempotent: a stale handle returns `False` instead
of raising.

### `Actor` properties and methods

Writable scalar properties are `tid`, `health`, `x`, `y`, `z`,
`velocity_x/y/z`, `angle`, `pitch`, `roll`, `radius`, `height`, `speed`,
`gravity`, `mass`, `alpha`, `scale_x/y`, `tics`, `score`, `special`,
`damage_factor` (multiplies damage the actor TAKES), and `damage_multiply`
(multiplies damage it DEALS; both default to 1.0 and clamp at 0).
Writable tuple/reference properties are `position`, `velocity`, `angles`,
`args`, `target`, `master`, and `tracer`. Read-only properties include
`valid`, `class_name`, `alive`, `is_player`, `is_monster`, `water_level`,
`floor_z`, and `ceiling_z`.

`actor.tint` assigns an `(r, g, b)` sprite tint (0-255 per component): the
sprite's brightness ramp is remapped to that color in both renderers —
the Diablo-style colored-monster effect. Read it back as a tuple, or
`None` when untinted; assign `None` to restore the class default. Tint
tables are built lazily and are not serialized, so re-apply tints on the
`map_load` following a savegame load (other mutated stats serialize
normally).

The gameplay-aware methods are:

| Method | Purpose |
|--------|---------|
| `snapshot()` | Return a JSON-friendly current snapshot. |
| `set_position(x, y, z, check=True, fog=False)` | Move now, optionally collision-checking or using teleport fog. |
| `set_velocity(x, y, z, add=False)` / `thrust(angle, force, vertical=0, replace=False)` | Native movement control. |
| `damage(amount, damage_type="None", inflictor=None, source=None, flags=0)` / `heal(amount, maximum=0)` | Use damage/healing paths and return their result. |
| `destroy()` | Remove the actor and invalidate the handle. |
| `distance_to(other)` / `check_sight(other, flags=0)` | Native spatial queries. |
| `get_flag(name)` / `set_flag(name, enabled)` | Read or modify accessible actor flags. |
| `set_state(label, call_actions=True)` | Enter a named state. |
| `inventory_count`, `give_inventory`, `take_inventory`, `use_inventory`, `clear_inventory` | Native inventory operations. |
| `play_sound(...)` / `stop_sound(channel=...)` | Actor-attached audio control. |
| `activate(activator=None, deactivate=False)` | Call the actor activation path. |
| `call_zscript(method, *args)` | Invoke a supported public ZScript actor method synchronously. |

Direct scalar assignment is deliberately raw. Prefer `damage`, `heal`,
`set_position`, and inventory methods when engine side effects, event dispatch,
collision, or gameplay credit matter.

`snapshot()` exists for the same reason as the legacy snapshot API:
serialization. Call it from a `save` handler and store the returned dict in
`bd.state`; do not treat it as a live view of the actor.

### `Player`, `Sector`, and `Line`

`Player` exposes `valid`, `index`, `name`, `actor`, input fields
(`buttons`, `input_pitch/yaw/roll`, `forward_move`, `side_move`, `up_move`),
`fov`, and frag/kill/item/secret counts. `set_input(...)` changes the current
native user command; use it from `pre_tick`. `set_weapon(class_name)` switches
to an owned weapon. `BT_ATTACK`, `BT_USE`, `BT_JUMP`, `BT_CROUCH`,
`BT_ALTATTACK`, `BT_RELOAD`, `BT_ZOOM`, and `BT_USER1..4` are exported button
bits.

`Sector` exposes `index`, `tags`, writable `light`, `gravity`, `special`,
`damage`, `damage_interval`, and `leakiness`, plus read-only center
`floor_height`/`ceiling_height`. `move_floor(height, speed=0, crush=-1)` and
`move_ceiling(...)` perform one native plane movement step; use an action
special when a persistent mover thinker is desired.

`Line` exposes `index`, `front_sector`, `back_sector`, and writable `args`,
`special`, `flags`, `activation`, `alpha`, and `health`.
`activate(activator=None, back_side=False, clear=False)` executes its special.

### Native world/gameplay operations

| Function | Purpose |
|----------|---------|
| `execute_special(special, arguments=None, activator=None, line=None, back_side=False)` | Run any numeric or named action special with up to five arguments. |
| `radius_damage(spot, damage, distance, source=None, damage_type="Explosion", hurt_source=True)` | Native radius attack. |
| `spawn_missile(source, target, class_name, position=None, owner=None, check=True)` | Native aimed missile spawn. |
| `line_attack(source, angle=None, distance=8192, pitch=None, damage=5, damage_type="None", puff_class="BulletPuff", flags=0)` | Native hitscan; returns `target`, `puff`, and applied `damage`. |
| `exit_level(position=0, secret=False, keep_facing=False)` | Normal/secret level exit. |
| `change_level(map_name, position=0, flags=0, next_skill=-1)` | Explicit map transition. |
| `center_message(message, bold=False)` | Immediate center-screen message. |
| `set_music(name, order=0, looping=True, force=False)` | Change music and return success. |
| `player_log(player_index=0)` | Return the player's conversation log text (the Strife journal line), or `None` when unset/invalid. For numeric `LOG#` replies this is the `$TXT_LOGTEXT<n>` string-table label, not the resolved text. Read-only; allowed in observer mode. |
| `set_player_log(text, player_index=0)` | Set the player's conversation log text (the Strife journal line). Mutation-guarded. |
| `start_conversation(npc)` | Start a Strife conversation between the local player and the given actor (USE-path arguments). Returns `False` when the actor cannot talk. Mutation-guarded. |

`CHANGELEVEL_KEEPFACING`, `CHANGELEVEL_RESETINVENTORY`,
`CHANGELEVEL_NOMONSTERS`, `CHANGELEVEL_NOINTERMISSION`, and
`CHANGELEVEL_RESETHEALTH` are exported flag constants.

For large homogeneous changes, `bd.apply_actor_batch(operations)` reduces
Python/C crossings. Supported tuples are `("velocity", actor, x, y, z)`,
`("add_velocity", ...)`, `("position", ...)`, `("health", actor, value)`,
`("damage", actor, amount)`, `("destroy", actor)`,
`("speed", actor, value)`, `("alpha", actor, value)`,
`("scale", actor, value)` (uniform x/y), `("damage_factor", actor, value)`,
`("damage_multiply", actor, value)`, and `("tint", actor, r, g, b)` —
the same sprite tint as `actor.tint`. It returns the number applied.
Operations whose `Actor` handle went stale since the batch was built are
skipped (a rate-limited warning reports how many); any other invalid
operation stops the batch at the first failure — validate generated batches
before submitting them.

The read-side counterpart is `bd.actor_field_batch(refs, fields)`: it reads
the whitelisted fields `health`, `x`, `y`, `z`, `angle`, `pitch`, `roll`,
`speed`, `alpha`, `tid`, `class_name`, `alive`, `is_player`, `is_monster`,
`special`, and `damage_factor` for many actors in one C API crossing and
returns a list of tuples in the same order as `refs`. Stale or invalid
handles yield a tuple of `None` values instead of raising; an unknown field
name raises `ValueError` listing the valid names.

```python
rows = bd.actor_field_batch(nearby, ["health", "x", "y", "alive"])
for ref, (health, x, y, alive) in zip(nearby, rows):
    if alive:
        bd.log(f"{ref.class_name} at {x},{y} hp={health}")
```

## Actor Class Registry (`bd.actors`)

`bd.actors` doubles as a registry of every actor class the engine knows
about — including classes defined by loaded mods, ZScript, DECORATE, and
MAPINFO `doomednums`. Calling it still queries snapshots (see the legacy
section below); accessing attributes on it gives you named constants so you
never have to hardcode class strings:

```python
bd.spawn(bd.actors.DOOM_IMP, SPOT_X, SPOT_Y, 0.0)
```

Constants are `UPPER_SNAKE` versions of the engine class names
(`DOOM_IMP` → `"DoomImp"`, `MBF_HELPER_DOG` → `"MBFHelperDog"`). An
unknown constant raises `AttributeError` with a hint. Use
`dir(bd.actors)` or `bd.actors.constants()` to list every constant, and
`bd.actors.names()` for the class-name strings.

Discovery helpers:

```python
bd.actors.names()              # all actor class names, sorted
bd.actors.constants()          # all CONST names, sorted
bd.actors.resolve("DOOM_IMP")   # "DoomImp" (accepts either form, None if unknown)
bd.actors.children_of("Weapon")  # ["BFG9000", "Chaingun", "Pistol", ...]
bd.actors.monsters()           # shootable, kill-counted actors
bd.actors.projectiles()        # missile actors
bd.actors.weapons()            # Weapon descendants
bd.actors.items()              # Inventory descendants
bd.actors.players()            # PlayerPawn descendants
```

Random selection and spawning:

```python
bd.actors.random()                    # any actor class
bd.actors.random("monsters")          # category: monsters, projectiles,
                                      # weapons, items, players
bd.actors.random("DOOM_IMP")          # among a class and its descendants
bd.actors.spawn_random(x, y, z)       # random monster at (x, y, z)
bd.actors.spawn_random(x, y, z, kind="items", angle=90.0)
```

`random()` raises `ValueError` for an unknown category or class.
`spawn_random()` forwards extra keyword arguments (`angle`, `tid`,
`force`) to `bd.spawn_actor`. Both draw from the deterministic
`bd.random()` stream, so selections are reproducible across save/load.

## Legacy Snapshot API (JSON persistence)

The functions in this section return plain dictionaries captured at call
time. They are stale the moment they are made — health, position, and even
existence can change on the next tic, and nothing ever updates the dict.
Their remaining legitimate use case is serialization: building
JSON-compatible data for `bd.state` in a `save` handler. For anything else,
use the live handles documented above. All mutation functions here require
an active primary level and a single-player, non-demo session; violations
raise `RuntimeError`.

### Snapshot dictionary fields

| Key | Type | Meaning |
|-----|------|---------|
| `class_name` | `str` | Runtime actor class after replacement. |
| `tid` | `int` | Thing ID; `0` means it cannot be targeted by TID APIs. |
| `health` | `int` | Health at snapshot time. |
| `x`, `y`, `z` | `float` | World position in map units. |
| `angle` | `float` | Yaw in degrees. |
| `pitch` | `float` | Pitch in degrees. |
| `velocity_x`, `velocity_y`, `velocity_z` | `float` | Current velocity components. |
| `alive` | `bool` | Whether health was greater than zero. |
| `is_monster` | `bool` | Actor has the engine monster flag. |
| `is_player` | `bool` | Actor is a player pawn. |
| `player_index` | `int` | Player slot or `-1`. |

Never retain a snapshot and assume the engine actor is unchanged. Query again
by TID when you need current data — or, better, keep a live `Actor` handle and
check `.valid`.

### `bd.actors(class_name=None, tid=0, limit=1024) -> list[dict]`

Returns actor snapshots from the primary level. In new code this belongs in a
`save` handler:

```python
def on_save(event):
    mine["zombie_tids"] = [
        a["tid"] for a in bd.actors(class_name="ZombieMan") if a["alive"]
    ]
```

Details:

- `class_name` uses engine class lookup and accepts derived classes through
  the normal `IsKindOf` relationship.
- `tid=0` means no TID filter; it does not mean “find actors whose TID is 0.”
- `limit` must be from `1` through `100000`, otherwise `ValueError` is raised.
- An unknown class raises `ValueError`.
- Outside a level, the function returns an empty list.

### `bd.actor(tid) -> dict | None`

Returns a snapshot of the first actor with a nonzero TID or `None`.
`bd.actor_ref(tid)` is the handle-returning equivalent for live work.

```python
door_guard = bd.actor(500)
if door_guard is not None:
    mine["guard"] = {"health": door_guard["health"], "tid": 500}
```

Maps often contain actors with TID `0`. Assign important targets a TID in the
map, ACS, ZScript, or Python spawn call.

### `bd.spawn_actor(class_name, x, y, z, angle=0.0, tid=0, force=False) -> dict`

Spawns an actor with normal class replacement enabled and returns a snapshot.
`bd.spawn` is the handle-returning equivalent for live work.

```python
spawned = bd.spawn_actor(
    "ZombieMan",
    128.0,
    -64.0,
    0.0,
    angle=180.0,
    tid=9001,
)
```

Details:

- An unknown class raises `ValueError`.
- With `force=False`, an actor that does not fit is destroyed and
  `RuntimeError` is raised.
- `force=True` skips the fit rejection; use it carefully.
- With `tid=0`, the engine allocates an unused TID starting in the
  `10000..99999` search range.
- If no TID can be allocated, the actor is destroyed and `RuntimeError` is
  raised.
- The returned snapshot's `class_name` may reflect actor replacement.

### `bd.damage_actor(tid, damage, damage_type="None") -> int`

Damages the first actor with the TID and returns the engine damage result.

```python
applied = bd.damage_actor(9001, 25, damage_type="Fire")
```

No Python actor is supplied as source or inflictor. A missing TID raises
`LookupError`.

### `bd.set_actor_velocity(tid, x, y, z) -> dict`

Sets velocity components and returns the new snapshot:

```python
after = bd.set_actor_velocity(9001, 4.0, 0.0, 6.0)
```

A missing TID raises `LookupError`.

### `bd.destroy_actor(tid) -> bool`

Destroys the first actor with the TID, clears its kill/item counters, and
returns `True`. Returns `False` when no actor exists.

Use damage when gameplay credit, death states, or death events matter. Direct
destruction is removal, not a normal kill.

## Gameplay Director API

These functions let a mod direct presentation and run structure — time
flow, HUD messaging, screen effects, UI audio, seeded randomness, and
named checkpoints — without touching actor internals.

### `bd.random()` / `bd.randrange()` / `bd.randint()` / `bd.choice()`

Module-level deterministic randomness backed by one engine RNG stream:
`bd.random()` returns a float in `[0, 1)`, `bd.randrange(lo, hi)` an int in
`[lo, hi)` (`bd.randrange(hi)` uses `[0, hi)`), `bd.randint(lo, hi)` an int
in `[lo, hi]` inclusive, and `bd.choice(seq)` an element of a non-empty
sequence. The stream is seeded once per map load from the run's RNG seed
and level number, and its full state is serialized with `bd.state` into
savegames — sequences replay exactly across save/load within a playthrough.
`bd.actors.random()`/`spawn_random()` draw from this same stream. Usable
before any level loads.

### `bd.rng(seed=0) -> RngStream`

Creates an independent deterministic random stream. `.int(lo, hi)` is
inclusive on both ends, `.float()` returns `[0, 1)`, and `.choice(seq)`
picks an element. Streams are independent of the gameplay RNG and are
**not** serialized into savegames — re-create them from your own saved
seed when determinism must survive a reload. Ideal for seeded roguelike
mutators:

```python
stream = bd.rng(seed=1337)
mutator = stream.choice(["double_speed", "glass_cannon", "rich_pickups"])
bonus = stream.int(1, 10)   # inclusive: 1..10
chance = stream.float()     # 0.0 <= chance < 1.0
```

### `bd.set_timescale(scale) -> float` / `bd.get_timescale()`

Scales the flow of game time; `1.0` is normal. The engine clamps the
minimum to `0.05` and forces `1.0` in netgames; the return value is the
applied scale. This is the bullet-time primitive:

```python
applied = bd.set_timescale(0.25)   # slow motion
...
bd.set_timescale(1.0)              # restore when the key is released
```

### `bd.hud_text(...)` / `bd.hud_clear(id=0)`

`bd.hud_text(text, id=0, x=0.5, y=0.1, color="gold", hold=2.0,
fade=0.5)` draws positioned, fading HUD text. `x`/`y` are screen
fractions following the ACS `HUDMessage` convention; reusing an `id`
replaces the previous message. `color` is a font color name (`gold`,
`red`, `green`, `blue`, `white`, `orange`, `yellow`, `cyan`); `hold` and
`fade` are in seconds. Text scale is font-based and not directly
controllable. An active status bar is required, so calls before level
start raise `RuntimeError` — wrap them defensively. `bd.hud_clear(id=0)`
removes a message immediately. Combo meters, objective trackers, and
wave counters:

```python
try:
    bd.hud_text(f"COMBO x{streak}", id=1, y=0.2, color="gold", hold=1.5)
except RuntimeError:
    pass  # no status bar yet
```

### `bd.screen_flash(r, g, b, alpha)` / `bd.screen_fade(...)`

`bd.screen_flash` applies an instant, one-frame full-screen color;
`r`/`g`/`b` are `0..255` and `alpha` is `0..1`.
`bd.screen_fade(r, g, b, alpha, seconds=1.0)` fades to transparent over
the given time; `seconds <= 0` clears the fade immediately. Damage
vignettes, flashbangs, and dramatic transitions:

```python
bd.screen_flash(255, 0, 0, 0.35)            # damage vignette
bd.screen_fade(255, 255, 255, 0.9, 2.0)     # flashbang recovery
bd.screen_fade(0, 0, 0, 0.0, seconds=0)     # clear any fade now
```

### `bd.play_ui_sound(name, volume=1.0)`

Plays a non-positional UI sound by its SNDINFO logical name — announcer
dings and UI feedback:

```python
bd.play_ui_sound("misc/secret")               # announcer ding
bd.play_ui_sound("menu/choose", volume=0.5)   # quieter UI feedback
bd.play_ui_sound("switches/normbutn")         # any SNDINFO logical name
```

### `bd.save_checkpoint(...)` / `bd.load_checkpoint(name="checkpoint")`

`bd.save_checkpoint(name="checkpoint", description="")` writes a named
checkpoint slot; `bd.load_checkpoint` restores it. Both are deferred to
the next tic boundary. The slot file is `<savedir>/<name>.zds`, and the
name is sanitized to `[A-Za-z0-9_-]`. Loading a missing slot raises
`FileNotFoundError`, and a load aborts the current level. Roguelike
checkpoints, save-per-wave, and permadeath runs:

```python
bd.save_checkpoint("wave_5", description=f"Wave 5 cleared ({score} pts)")
...
try:
    bd.load_checkpoint("wave_5")
except FileNotFoundError:
    bd.log("no checkpoint yet", level="warning")
```

### New events: `item_picked` and `secret_found`

| Event | Extra fields |
|-------|--------------|
| `item_picked` | `class_name`, `name`, `amount`, `player` |
| `secret_found` | `player`, `found_secrets`, `total_secrets` |

Both work with decorator registration and conventional top-level names,
and suit achievements, secret-hunt trackers, and pickup-driven mutators:

```python
@bd.on("secret_found")
def celebrate(event):
    bd.play_ui_sound("misc/secret")

def on_item_picked(event):  # conventional-name style
    bd.log(f"picked {event['amount']}x {event['name']}")
```

### New events: `item_dropped`, `weapon_changed`, `sector_entered`, `sector_exited`

| Event | Extra fields |
|-------|--------------|
| `item_dropped` | `actor_ref`, `dropper_ref`, `player_index`, `class_name`, `amount` |
| `weapon_changed` | `player_index`, `weapon`, `actor_ref`, `player_ref` |
| `sector_entered`, `sector_exited` | `sector`, `tags`, `player_index`, `actor_ref` |

All four work with decorator registration and conventional top-level names
(`on_item_dropped`, `on_weapon_changed`, `on_sector_entered`,
`on_sector_exited`), and suit loot-tracking mutators, weapon-switch HUD
helpers, and zone-based triggers:

```python
def on_item_dropped(event):  # conventional-name style
    bd.log(f"dropped {event['amount']}x {event['class_name']}")

@bd.on("weapon_changed")
def announce(event):
    if event["weapon"] is not None:
        bd.log(f"player {event['player_index']} raised {event['weapon']}")

@bd.on("sector_entered")
def zone_check(event):
    if 9 in event["tags"]:
        bd.log(f"player {event['player_index']} entered tagged sector {event['sector']}")
```

### Future directions

A scripted cutscene camera override (overriding the `player.camera`
actor) is a candidate for a future API. Chase-cam is already
controllable today via `bd.set_cvar("chase_enabled", True)` and the
`chase_*` CVars.

## Canvas Drawing API

The canvas functions build a **persistent display list**: a script
registers a drawing item once, and the engine re-renders it every HUD
frame with no per-frame Python cost. To update or animate an item, call
the same draw function again with the same `id`, which replaces the
stored entry; `id` is always required. Screen-space coordinates and
sizes are normalized fractions of the screen (`0..1`), and world-anchored
items are projected to screen space every frame. All canvas colors are
`(r, g, b)` tuples with components in `0..255`, except text `color`,
which also accepts a font color name string (`"gold"`, `"red"`, ...).

### `bd.draw_text(...)` / `bd.draw_rect(...)`

`bd.draw_text(text, *, id, x=0.0, y=0.0, font="smallfont",
color=(255, 255, 255), scale=1.0, alpha=1.0, shadow=False,
outline=False, align="left", layer=0, height=0.0, duration=None)` draws text
at a normalized screen position. `scale` is a float or an `(sx, sy)`
tuple of raw pixel multipliers, so the text gets relatively smaller as
the resolution rises; prefer `height`, a normalized `0..1` screen-height
fraction for one text line that is recomputed against the live drawer
size every frame (e.g. `height=0.02` fills 2% of the screen height at
any resolution). `height > 0` overrides `scale`. An unknown font name
raises `ValueError`.
`bd.draw_rect(*, id, x=0.0, y=0.0, w=0.0, h=0.0, color=(255, 255, 255),
alpha=0.75, color2=None, layer=0, duration=None)` draws a filled
rectangle. A boss health panel:

```python
def show_boss_panel(name, frac):
    bd.draw_rect(id=100, x=0.25, y=0.05, w=0.5, h=0.03, color=(20, 20, 20), alpha=0.8)
    bd.draw_rect(id=101, x=0.25, y=0.05, w=0.5 * frac, h=0.03, color=(200, 30, 30), alpha=0.9)
    bd.draw_text(f"{name}  {int(frac * 100)}%", id=102, x=0.25, y=0.09, color="gold")
```

### `bd.draw_line(...)`

`bd.draw_line(*, id, x1=0.0, y1=0.0, x2=0.0, y2=0.0,
color=(255, 255, 255), alpha=1.0, layer=0, duration=None)` draws a
line between two normalized screen points. A crosshair helper:

```python
def draw_crosshair(color=(0, 255, 0)):
    bd.draw_line(id=1, x1=0.49, y1=0.5, x2=0.51, y2=0.5, color=color)
    bd.draw_line(id=2, x1=0.5, y1=0.49, x2=0.5, y2=0.51, color=color)
```

### `bd.draw_frame(...)`

`bd.draw_frame(*, id, x=0.0, y=0.0, w=0.0, h=0.0,
color=(255, 255, 255), thickness=2, alpha=1.0, layer=0, duration=None)`
draws a hollow rectangle (border only); the border is drawn **inside** the
rect, so the given box is the outer edge. `thickness` is in pixels.

```python
bd.draw_frame(id=30, x=0.3, y=0.1, w=0.4, h=0.2, color=(255, 140, 40), thickness=2)
```

### `bd.draw_texture(...)`

`bd.draw_texture(name, *, id, x=0.0, y=0.0, scale=1.0, alpha=1.0,
tint=None, rotate=0.0, layer=0, duration=None)` draws a texture lump
at a normalized screen position. An
unknown texture name raises `ValueError`. `tint=(r, g, b)` renders a
**solid-color silhouette** (a stencil fill of the texture's shape), not
a multiply tint — ideal for status icons:

```python
bd.draw_texture("MEDIA0", id=20, x=0.02, y=0.02, scale=2.0)
bd.draw_texture("MEDIA0", id=21, x=0.07, y=0.02, tint=(255, 80, 0))  # orange silhouette
bd.draw_texture("STGNUM0", id=22, x=0.12, y=0.02, alpha=0.5)          # faded, untinted
```

### `bd.draw_world_bar(...)` / `bd.draw_world_text(...)`

`bd.draw_world_bar(actor, *, id, offset_z=0.0, width=0.06, height=0.008,
track="health", frac=None, fg=None, bg=(20, 20, 20),
max_distance=2048.0, occlude=True, label=False,
label_color=(255, 255, 255), label_scale=1.5, label_font="smallfont",
layer=0, duration=None)`
anchors a bar above an actor (`offset_z` above its top).
`track="health"` follows the actor's health per frame; `track=None`
requires a static `frac` in `0..1` (any other track value raises
`ValueError`). With `track="health"` and no explicit `fg`, the fill color
is an **automatic per-frame gradient** — green above 60% health, yellow
between 30% and 60%, red at or below 30% — so a plain call already reads
like a proper health bar. Passing an explicit `fg` tuple overrides the
gradient with a static color; this is how status-effect tinting works
(see *Custom status effects* below). Bars are drawn with an opaque black
2px border over a padded, ~85% opacity `bg` background automatically —
no manual styling needed. `bd.draw_world_text(actor, *, id, text,
offset_x=0.0, offset_y=0.0, offset_z=0.0, font="smallfont",
color=(255, 255, 255), scale=0.75,
alpha=1.0, max_distance=2048.0, occlude=True, shadow=False,
outline=False, layer=0, height=0.0, duration=None)` anchors a centered text
label the same way (offset_x/offset_y are world-unit lateral offsets for
scatter/arc animations); `height` sizes it as a normalized screen-height
fraction (resolution-independent, overriding `scale`). Both world kinds default to `occlude=True`, which
hides the item when the player has no line of sight to the actor
(caveat: the sight test runs from the player actor even in chase cam).
For bars, `label=True` additionally draws the actor's `GetTag()` name
centered above the bar, styled with `label_font`/`label_scale`/
`label_color` (an `(r, g, b)` tuple or font color name string). World
items fade out over the last 20% of `max_distance`, are hidden entirely
beyond it or behind the camera, and vanish automatically when the actor
is destroyed or the map unloads — no cleanup code needed. Dead actors
draw nothing (they may be revived), with one exception: **transient
world text** (registered with `duration=`) plays out at the corpse's
position, so killing-blow combat text is never lost. The marquee use case, floating monster health
bars with name labels:

```python
@bd.on("actor_spawned")
def add_health_bar(event):
    actor = event["actor_ref"]
    if actor.is_monster:
        bd.draw_world_bar(actor, id=1000 + actor.tid, label=True)

# No fg: the bar gets the automatic green/yellow/red health gradient.
# No removal handler: bars disappear with their monster automatically.
```

### `bd.draw_clear(id)` / `bd.draw_clear_all()`

`bd.draw_clear(id)` removes one item (a no-op if the id is absent);
`bd.draw_clear_all()` removes everything:

```python
bd.draw_clear(102)      # remove just the boss caption
bd.draw_clear_all()     # wipe the whole display list (e.g. on map_unload)
```

### Z-order and lifetimes

Every `draw_*` function accepts two keyword-only arguments.
`layer=0` controls z-order: higher layers render on top of lower ones,
and within a layer items render in registration order — so raise the
layer to keep a caption above a panel instead of juggling call order.
`duration=None` is an auto-expiry in seconds, counted in game time (it
pauses with the game); when it elapses the item removes itself, no
`bd.schedule` cleanup needed:

```python
bd.draw_rect(id=200, x=0.2, y=0.1, w=0.6, h=0.05, color=(0, 0, 0), alpha=0.7, layer=1)
bd.draw_text("LEVEL COMPLETE", id=201, x=0.5, y=0.11, align="center", layer=2)
bd.draw_text("+1000", id=202, x=0.5, y=0.5, color="gold", align="center", duration=2.0)
```

### Typography: alignment, outlines, shadows

`draw_text` gained `align="left"|"center"|"right"` (screen-space only —
anything else raises `ValueError`; world labels stay centered) and
`outline=False`, which adds a black 1px outline for readability over
busy backdrops. `draw_world_text` gained `outline` and `shadow` too.
Note the shadow fix: **`shadow=` now renders a real dark offset shadow;
it was inert before** (dead engine code), so existing mods that set it
will change appearance. Text also supports inline color escapes:

| Escape      | Effect                                   |
| ----------- | ---------------------------------------- |
| `\x1c[Gold]` | switch to a named font color             |
| `\x1c-`      | reset to the item's own `color`          |
| `\x1c+`      | toggle bold                              |

Caveat: with a tuple-RGB `color` the escapes only modulate brightness —
pass a named color (e.g. `color="white"`) OR use escapes, not both.
`bd.measure_text(text, font="smallfont", scale=1.0)` returns the
`(width, height)` a string would occupy in **pixels**, for layout math:

```python
w, h = bd.measure_text("WAVE 3", font="bigfont", scale=1.5)
bd.draw_text("WAVE 3", id=50, x=0.5, y=0.3, font="bigfont", scale=1.5,
             align="center", outline=True, shadow=True)
bd.draw_text("\x1c[Gold]GOLD\x1c- and \x1c+bold\x1c+", id=51, x=0.02, y=0.9, color="white")
```

### Gradients, circles and rotated sprites

`draw_rect` accepts `color2=None`: when set, the fill becomes a vertical
gradient (`color` at the top blending to `color2` at the bottom).
`bd.draw_circle(*, id, x=0.0, y=0.0, radius=0.0, color=(255, 255, 255),
alpha=1.0, fill=False, layer=0, duration=None)` draws a circle at a
normalized position; `radius` is X-normalized (scaled by screen width
only, so circles stay round). The outline is a 32-segment polyline and
`fill=True` rasterizes chord scanlines, so slight gaps are possible at
small radii. `draw_texture` accepts `rotate=0.0` (degrees, around the
anchor point):

```python
bd.draw_rect(id=300, x=0.0, y=0.0, w=1.0, h=0.08,
             color=(40, 0, 60), color2=(0, 0, 0), alpha=0.9)  # gradient banner
bd.draw_circle(id=301, x=0.5, y=0.5, radius=0.03, color=(0, 255, 0))          # reticle
bd.draw_circle(id=302, x=0.5, y=0.5, radius=0.06, color=(0, 255, 0), fill=True, alpha=0.2)
bd.draw_texture("MEDIA0", id=303, x=0.9, y=0.05, scale=2.0, rotate=45.0)      # tilted icon
```

### World icons and beams

`bd.draw_world_texture(actor, name, *, id, offset_z=0.0, size=24.0,
alpha=1.0, tint=None, occlude=True, max_distance=2048.0, layer=0,
duration=None)` floats an icon/sprite over an actor: `size` is in map
units and scales with distance, `tint` renders a solid silhouette, and
the same occlusion/distance/lifetime rules as other world items apply.
`bd.draw_world_line(a, b, *, id, color=(255, 255, 255), alpha=1.0,
layer=0, duration=None)` draws a 1px beam between two endpoints; each
endpoint is an `Actor` handle (anchored at the actor's center, following
movement) or an `(x, y, z)` tuple for a static point. The beam is
skipped when an endpoint is behind the camera, and there is no
thickness control:

```python
bd.draw_world_texture(boss, "MEDIA0", id=400, offset_z=16.0, size=32.0)   # marker over the boss
bd.draw_world_line(player, boss, id=401, color=(255, 64, 64))             # tether beam
bd.draw_world_line(player, (512.0, -256.0, 64.0), id=402, duration=5.0)   # beam to a static point
```

### Ground rings (affix auras)

`bd.draw_world_ring(actor, *, id, radius=20.0, color=(255, 255, 255),
alpha=1.0, offset_z=2.0, segments=28, max_distance=2048.0, occlude=True,
layer=0, duration=None)` draws a flat ring around an actor's feet that
follows it every frame — the Diablo-style champion/unique aura. `radius`
is in world units and `segments` (3-128) sets how smooth the projected
polyline is:

```python
bd.draw_world_ring(unique, id=700, radius=26.0, color=(255, 200, 40))  # gold unique aura
```

### Custom fonts and sizes

The built-in font names are `smallfont` (the default), `smallfont2`,
`bigfont`, `bigupper`, `confont`, and `indexfont`. Text size is per-call:
`height` (a normalized `0..1` screen-height fraction, resolution-
independent and recommended for HUD work) or `scale` (a float or an
`(sx, sy)` tuple of raw pixel multipliers), and `shadow=True` adds a drop
shadow to `draw_text`:

```python
bd.draw_text("WAVE 3", id=50, x=0.4, y=0.3, font="bigfont", scale=1.5, shadow=True)
bd.draw_text("secret!", id=51, x=0.4, y=0.4, font="confont", scale=(2.0, 1.0))
```

To ship a custom font, add the font graphics to your PK3 and declare it
with a standard `FONTDEFS` lump:

```text
MYFONT
{
    TEMPLATE "FON7%03d"
    START 33
    END 126
}
```

Then reference it by name: `bd.draw_text("hello", id=1, font="myfont")`.

### Custom status effects

World items update only when a script re-registers them, so a custom
status effect is just a tint pattern plus a `bd.schedule`-driven expiry:
register a world bar with custom `fg`/`bg` colors and a floating label
over the actor, then schedule a task that re-calls the same ids to
refresh or removes them when the effect ends. Because world items
vanish with their actor, no defensive cleanup is needed for monster
deaths mid-effect. Example
[`16_monster_health_bars`](../../examples/python/16_monster_health_bars/)
implements this pattern with a custom *Burning* status effect:

```python
def ignite(actor):
    key = 5000 + actor.tid
    bd.draw_world_bar(actor, id=key, track="health", fg=(255, 120, 0), bg=(40, 10, 0))
    bd.draw_world_text(actor, id=key + 1, text="BURNING", color=(255, 140, 0),
                       offset_z=8.0, height=0.016, outline=True)
    bd.schedule(lambda: (bd.draw_clear(key), bd.draw_clear(key + 1)), delay=5 * bd.TICRATE)
```

## UI Toolkit (bd.ui)

`bd.ui` is a small **pure-Python UI toolkit embedded in the biaseddoom
module**, built on top of the canvas display list — no extra files or
imports needed. It provides themed HUD panels, transient toasts and
center-screen announcements, so a mod gets polished HUD chrome without
hand-laying-out `draw_rect`/`draw_text` calls. All packaged examples use
it; see [`15_roguelike_run`](../../examples/python/15_roguelike_run/),
[`12_level_ui_audio`](../../examples/python/12_level_ui_audio/) and
[`03_combat_and_inventory`](../../examples/python/03_combat_and_inventory/)
for complete HUDs.

### The theme palette

`bd.ui.theme` is a mutable palette; assign new tuples to restyle every
panel created afterwards. Colors are `(r, g, b)` or `(r, g, b, a)` tuples
with components in `0..255` (the optional alpha is also `0..255`):

| Field    | Default             | Used for                                |
| -------- | ------------------- | --------------------------------------- |
| `bg`     | `(10, 10, 26, 200)` | panel backdrop, gradient top            |
| `bg2`    | `(24, 14, 40, 200)` | panel backdrop, gradient bottom         |
| `border` | `(255, 140, 40)`    | panel frame                             |
| `text`   | `(235, 230, 220)`   | row labels, toast default color         |
| `dim`    | `(150, 145, 135)`   | bar frames                              |
| `accent` | `(255, 180, 60)`    | default row value color                 |
| `good`   | `(90, 220, 110)`    | bar fill above 60%                      |
| `warn`   | `(240, 210, 80)`    | bar fill 30-60%                         |
| `bad`    | `(235, 70, 60)`     | bar fill at/below 30%                   |
| `gold`   | `(255, 200, 80)`    | panel titles, flash, announce default   |

### Panels

`bd.ui.panel(*, x, y, w, title=None, anchor="tl", id=None)` creates a
panel anchored to a screen corner (`"tl"`, `"tr"`, `"bl"` or `"br"`);
`x`/`y` refer to that corner, `w` is the normalized width, and the height
grows automatically with the rows. All panel text is sized with the
resolution-independent `height=` parameter, so panels are compact and
equally readable at any resolution. `id` optionally overrides the
allocated id base. The returned panel supports chaining:

```python
panel = bd.ui.panel(x=0.02, y=0.02, w=0.28, title="RUN", anchor="tl")
panel.row("Kills", "0")                       # label/value row
panel.bar("Health", 1.0)                      # gradient bar, auto good/warn/bad color
panel.bar("Armor", 0.45, fg=bd.ui.theme.gold) # fg overrides the auto color
panel.row("Score", "+500", value_color=bd.ui.theme.good, flash=True)  # brief gold flash
```

Re-calling `row()`/`bar()` with an existing label updates the row in
place. `panel.hide()` re-registers every item at alpha 0 (the panel stays
registered), `panel.show()` restores it, and `panel.close()` removes every
display-list item the panel owns.

### Toasts and announcements

`bd.ui.toast(text, *, color=None, duration=1.5, y=0.72)` shows a small
centered outlined toast that auto-expires; `bd.ui.announce(title, *,
subtitle=None, color=None, duration=2.5)` shows a big outlined bigfont
title with an optional smallfont subtitle, a screen-fade accent and a UI
sound:

```python
bd.ui.toast("CHECKPOINT SAVED", color=bd.ui.theme.good)
bd.ui.announce("WAVE 3", subtitle="THEY KEEP COMING", color=bd.ui.theme.gold)
```

**Layers and id ownership.** Panels draw backdrop on layer 0, content and
bars on layer 1 and the frame on layer 2; toasts/announcements sit on
layer 3. The toolkit owns all display-list ids from base 900000 (100 ids
reserved per panel, allocated automatically; toast/announce ids live at
999000+) — keep your own canvas ids below that range or pass an explicit
`id` base well clear of it.

**Level transitions.** All live panels automatically re-register their
items on `map_load`, so toolkit UI survives map changes with no
script-side cleanup; draws attempted while no level/HUD is active are
swallowed and retried on the next update or on that re-render.

**Color asymmetry caveat.** Unlike `draw_text` (which accepts font color
name strings like `"gold"`), the toolkit's `toast()`/`announce()` colors
and every theme field accept **only `(r, g, b)` / `(r, g, b, a)` tuples**
— named color strings are not accepted there.

## Dear ImGui UI (`bd.imgui`)

`bd.imgui` binds the vendored **Dear ImGui 1.92.8** (docking branch) into
an engine overlay for rich, interactive debug and mod UI: draggable
windows, docking, sliders, input fields, tables, plots, menus, tooltips.
Unlike the canvas display list (persistent primitives re-rendered every
frame), ImGui is **immediate mode**: your script submits the whole UI every
frame and keeps all state itself.

### The `imgui_frame` event

Every `bd.imgui` call is only legal inside an **`imgui_frame`** handler,
which fires once per rendered frame between ImGui's `NewFrame` and
`Render`. Calling from anywhere else raises `RuntimeError("bd.imgui calls
are only valid inside an imgui_frame handler")`. The any-event exceptions
are `set_master_visible()` / `master_visible()` and
`set_nav_enabled()` / `nav_enabled()` (engine-state passthroughs),
`image_size()`, the font registry (`add_font_ttf()` through
`get_default_font()`), and `set_ui_scale()` / `get_ui_scale()` plus the
persistent style accessors (`get_style_color()` through `style_theme()`,
which require one rendered frame before they work). See the API summary
below for the per-call gating. `imgui_frame` also fires at the title screen,
before any level exists; guard gameplay access accordingly
(`bd.player(0)` raises `RuntimeError` without an active level).

### Performance rules

- One CPython crossing per widget call — a panel of 30 widgets is 30
  crossings per frame. Keep per-frame widget counts modest; hide collapsed
  sections instead of submitting them.
- When no script registers `imgui_frame`, the entire feature costs a single
  `HasCallbacks` check per frame.
- When `py_imgui` is off, frames still run `NewFrame`/`Render` (window
  state persists) but dispatch nothing and draw nothing.

### Master switch, input capture, demo window

- **`py_imgui` CVar** (archived, default `true`): master visibility gate.
  `bd.imgui.set_master_visible(False)` / `bd.imgui.master_visible()` are
  script passthroughs.
- **Keyboard navigation**: `ImGuiConfigFlags_NavEnableKeyboard` is enabled
  by default, so menus and windows are drivable with the keyboard (Tab/
  arrows/Enter). `bd.imgui.set_nav_enabled(False)` / `bd.imgui.nav_enabled()`
  toggle and read it from any event.
- **`py_imgui_demo` console command**: toggles the stock ImGui demo window
  (also available per-script via `show_demo_window(open)`). On the docking
  branch the demo includes a DockSpace section.
- **Input capture**: while the overlay wants the mouse or keyboard, the
  engine routes GUI events to it instead of the game, so dragging a slider
  does not turn the player. `want_capture_mouse()` /
  `want_capture_keyboard()` expose that state.

### Quick start

```python
import biaseddoom as bd
imgui = bd.imgui

panel_open = True
speed = 1.0

@bd.on("imgui_frame")
def draw(event):
    global panel_open, speed
    if not panel_open:
        return
    imgui.set_next_window_pos(24, 40, imgui.Cond.FirstUseEver)
    # open= gives the window a close button; returns (expanded, open).
    expanded, panel_open = imgui.begin("My Panel", panel_open)
    if expanded:
        imgui.text("Immediate-mode UI from Python")
        changed, speed = imgui.slider_float("Speed", speed, 0.1, 4.0)
        if imgui.button("Reset"):
            speed = 1.0
    imgui.end()
```

State flows **through your variables**: widgets that edit a value return
`(changed, new_value)` tuples; you keep the value and pass it back next
frame. `begin()` is the exception with two return forms: `open=None`
(default) returns just the `expanded` bool, any other `open` value returns
`(expanded, open)`. Always call `end()` even when `begin()` returned False.

### API summary

| Function | Description |
|---|---|
| `begin(name, open=None, flags=0)` | Push a window; returns `expanded` or `(expanded, open)` |
| `end()` | Pop the current window (always required) |
| `begin_child(id, size=(0,0), border=False, flags=0)` / `end_child()` | Scrolling child region |
| `set_next_window_pos(x, y, cond=0)` / `set_next_window_size(w, h, cond=0)` | Place/size the next window |
| `set_next_window_collapsed(collapsed, cond=0)` / `set_next_window_bg_alpha(a)` | Collapse state / background alpha of the next window |
| `is_window_focused()` / `is_window_hovered()` | Current-window state |
| `get_window_pos()` / `get_window_size()` | Current window rect, `(x, y)` tuples |
| `text(s)` / `text_colored(r,g,b,a,s)` / `text_disabled(s)` / `text_wrapped(s)` | Text variants |
| `label_text(label, s)` / `bullet_text(s)` | Value-label and bulleted text |
| `button(label, w=0, h=0)` / `small_button(label)` | True on click |
| `checkbox(label, checked)` | `(changed, new_value)` |
| `radio_button(label, active)` | True when pressed |
| `slider_int(label, v, min, max)` / `slider_float(..., format="%.3f")` | `(changed, value)` |
| `drag_int(label, v, speed=1.0, min=0, max=0)` / `drag_float(...)` | Unbounded when `min >= max` |
| `input_text(label, text, max_length=256, flags=0)` | `(changed, new_text)`; `flags=32` = EnterReturnsTrue |
| `input_int(label, v, step=1, step_fast=100)` / `input_float(...)` | `(changed, value)` |
| `combo(label, current_index, items)` | Drop-down over a str sequence; `(changed, new_index)` |
| `list_box(label, current_index, items, height_items=-1)` | Scrolling list; `(changed, new_index)` |
| `selectable(label, selected, flags=0)` | True when pressed |
| `tree_node(label)` / `tree_pop()` / `collapsing_header(label, flags=0)` | Tree sections |
| `separator()` / `same_line(offset=0.0, spacing=-1.0)` / `spacing()` / `newline()` | Layout |
| `indent(width=0.0)` / `unindent(width=0.0)` / `align_text_to_frame_padding()` | Layout |
| `begin_table(id, columns, flags=0)` / `end_table()` | Tables (end only when True) |
| `table_next_row(flags=0, min_height=0.0)` / `table_next_column()` | Row/column advance |
| `table_setup_column(label, flags=0, init_width=0.0)` / `table_headers_row()` | Column declaration / header row |
| `progress_bar(fraction, w=-1, h=0, overlay=None)` | Horizontal bar; `w < 0` fills the row |
| `color_edit3(label, r, g, b)` / `color_edit4(label, r, g, b, a)` | `(changed, ...)` color editors |
| `plot_lines(label, values, overlay=None, scale_min=FLT_MAX, scale_max=FLT_MAX, w=0, h=0)` | Line plot; FLT_MAX = auto-scale |
| `image(texture, w=0, h=0, uv0=(0,0), uv1=(1,1), tint=(1,1,1,1), border=(0,0,0,0))` | Game texture or Actor's current sprite; `w`/`h` 0 = natural display size; unknown name raises ValueError |
| `image_size(texture) -> (w, h)` | Natural display size of a texture accepted by `image()`; callable from any event |
| `dock_space_over_viewport(flags=0)` / `dock_space(id, w=0, h=0, flags=0)` | Docking (see below); return the dockspace id |
| `set_next_window_dock_id(id, cond=0)` | Dock the next `begin()` window into a dockspace node programmatically |
| `begin_main_menu_bar()` / `end_main_menu_bar()` | Screen-top menu bar |
| `begin_menu(label)` / `end_menu()` / `menu_item(label, shortcut=None, selected=False, enabled=True)` | Menus |
| `begin_tooltip()` / `end_tooltip()` / `set_tooltip(s)` | Tooltips |
| `set_keyboard_focus_here(offset=0.0)` | Focus the next widget (positive `offset` addresses a sub component, -1 the previous widget) |
| `is_item_hovered()` / `is_item_clicked(button=0)` / `is_item_active()` / `is_any_item_active()` | Last-item state |
| `push_style_color(idx, r, g, b, a)` / `pop_style_color(count=1)` | Style colors; `idx` from `imgui.Col` |
| `push_style_var(idx, x, y=None)` / `pop_style_var(count=1)` | Style vars; `y=None` = scalar |
| `get_font_size()` | Font height in pixels |
| `show_demo_window(open)` / `show_metrics_window(open)` | Stock debug windows; return still-open |
| `want_capture_mouse()` / `want_capture_keyboard()` | ImGui input capture state |
| `set_master_visible(visible)` / `master_visible()` | `py_imgui` CVar passthrough (any event) |
| `set_nav_enabled(enabled)` / `nav_enabled()` | Keyboard navigation flag, on by default (any event) |
| `add_font_ttf(name, data, size)` | Register a TTF/OTF font from a `bytes` object (e.g. `bd.read_bytes()`) under `name` at pixel size 4..96 (any event) |
| `add_font_default(name, size, bitmap=False)` | Register ImGui's embedded default face, `bitmap=True` picks the classic pixel font (any event) |
| `remove_font(name)` / `clear_fonts()` | Drop a script font / all script fonts; the built-in `Default` font survives (any event) |
| `list_fonts()` | Registry snapshot, `[(name, size, bitmap, builtin), ...]` (any event) |
| `set_default_font(name)` / `get_default_font()` | The font every `imgui_frame` starts on (the overlay pushes it around the event); unknown name returns False (any event) |
| `push_font(name)` / `pop_font()` | Switch font inside the frame; raises `ValueError` for an unknown name |
| `set_ui_scale(factor)` / `get_ui_scale()` | Global UI scale, clamped 0.5..4.0, composed via `ScaleAllSizes()` by the ratio between old and new factor (any event) |
| `set_window_font_scale(scale)` | Per-window font scale for the current window; prefer `set_ui_scale()` for global scaling |
| `get_style_color(idx)` / `set_style_color(idx, r, g, b, a)` | Read/write a persistent style color, `idx` from `imgui.Col` (any event, needs one rendered frame) |
| `get_style_var(idx)` / `set_style_var(idx, x, y=None)` | Read/write a persistent style var, `idx` from `imgui.StyleVar`; ImVec2-backed vars require `y` (any event, needs one rendered frame) |
| `style_theme(name)` | Reset all colors to `'dark'`, `'classic'` or `'light'`; UI scale factors are preserved (any event, needs one rendered frame) |
| `is_key_down(key)` | True while the `imgui.Key` value is held |
| `is_key_pressed(key, repeat=False)` | True on the frame the key went down; `repeat=True` also reports held-key repeats |
| `is_key_chord_pressed(key, mods=0)` | True on the frame the `imgui.Mod`-OR'd chord went down; no focus routing, prefer `shortcut()` |
| `shortcut(key, mods=0)` | Chord with ImGui focus routing; the deepest focused window wins |
| `set_item_default_focus()` | Make the last submitted item the Enter-activated default of a newly appearing window |
| `open_popup(str_id)` / `begin_popup(str_id)` / `end_popup()` / `close_current_popup()` / `is_popup_open(str_id)` | Popup lifecycle; call `open_popup()` from an event-ish context (e.g. a button press), not every frame; `end_popup()` only when `begin_popup()` returned True |
| `calc_text_size(s)` | `(w, h)` of a string in the current font, in pixels |
| `set_cursor_pos(x, y)` / `get_cursor_pos()` / `get_cursor_screen_pos()` | Cursor in window-local / absolute screen coordinates |

### Fonts

Fonts live in a runtime registry keyed by name. `add_font_ttf(name,
data, size)` registers a TTF/OTF face from a `bytes` object (load it
with `bd.read_bytes()`) at a requested pixel size of 4..96;
`add_font_default(name, size, bitmap=False)` registers ImGui's embedded
face instead (`bitmap=True` selects the classic ProggyClean pixel font,
the default `False` the scalable ProggyForever vector face). Duplicate
or empty names are rejected with a console warning and a `False` return;
drop entries with `remove_font(name)` / `clear_fonts()`.

The TTF bytes are copied and owned by the overlay for the process
lifetime, so the Python buffer can be dropped immediately and later
atlas rebuilds can re-add the font without dangling. Every mutation
rebuilds the atlas outside the frame and uploads it under a new uniquely
named texture; draw data produced before a rebuild keeps referencing the
old atlas texture, which stays valid for the process lifetime. A
mutation requested during `imgui_frame` is applied after the frame
renders, so the font becomes usable on the next frame.

The built-in `"Default"` font registers when the overlay first
initializes and cannot be removed, so `list_fonts()` called before the
first frame only shows script-added fonts. `set_default_font(name)`
selects the font every `imgui_frame` starts on: the overlay pushes it
around the event (and pops it afterwards), so script-side
`push_font()` / `pop_font()` pairs always start balanced.

### Scaling and themes

`set_ui_scale(factor)` is the global knob, clamped to [0.5, 4.0]. It
sets `style.FontScaleMain` and rescales all spacing/padding sizes via
`ScaleAllSizes()` by the ratio between the new and the previous factor,
so repeated calls compose: 2.0 then 2.0 yields 4x spacing, not 8x.
`style_theme("dark" | "classic" | "light")` resets every style color to
a stock theme and preserves the UI scale factors across the reset.

Both are any-event calls, as are `get_style_color()` /
`set_style_color()` and `get_style_var()` / `set_style_var()`; the style
accessors require the overlay to have rendered at least one frame
(first use before that raises the "no frame has run yet"
`RuntimeError`). `set_window_font_scale(scale)` is different: it is
per-window, frame-gated, and the tool for one-off emphasis inside a
single window (prefer `set_ui_scale()` for global scaling).

### Keyboard interaction

`imgui.Key` holds the named key table (`Tab`, `Left`, `Right`, `Up`,
`Down`, `PageUp`, `PageDown`, `Home`, `End`, `Insert`, `Delete`,
`Backspace`, `Space`, `Enter`, `KeyPadEnter`, `Escape`, `A` through `Z`,
`F1` through `F12`, and the digits `0`-`9`, which are not valid
identifiers so they are reached with `getattr(imgui.Key, "7")`).
`imgui.Mod` holds `Ctrl`, `Shift`, `Alt` and `Super`; OR them together
for the `mods` argument. Keyboard navigation (Tab/arrows/Enter between
widgets) is on by default, toggleable with `set_nav_enabled()`.

`is_key_pressed(key, repeat=False)` reports the frame a key went down,
with `repeat=True` also reporting held-key repeats; it is a raw poll
with no focus routing, which is what menu digit and arrow hotkeys want.
`is_key_chord_pressed(key, mods)` adds modifiers but still does no
routing, while `shortcut(key, mods)` routes through ImGui so the deepest
focused window wins and the chord does not fire while an unrelated
window owns the keyboard (use it for commands like Ctrl+M).

For Esc-style dismissal, poll `is_key_pressed(imgui.Key.Escape)` inside
the frame and close a popup first (`close_current_popup()`) before
popping whatever screen is on top. `set_item_default_focus()` after a
widget makes it the Enter-activated default of a newly appearing window
(pin "No" on a quit confirmation), and `set_keyboard_focus_here()` moves
focus to the next widget (e.g. an input field when its window appears).

### Popups

`open_popup(str_id)` marks a popup open and must be called from an
event-ish context (a button press inside the frame), not unconditionally
every frame. `begin_popup(str_id)` returns True while the popup is open:
submit its contents and call `end_popup()` only in that case.
`is_popup_open(str_id)` queries without submitting, and
`close_current_popup()` closes the popup open in the current scope. The
usual Esc handler checks a popup first and closes it before unwinding
the screen stack.

### Constants

`imgui.Col` covers the full `ImGuiCol_*` set: `Text`, `TextDisabled`,
`WindowBg`, `ChildBg`, `PopupBg`, `Border`, `BorderShadow`, `FrameBg`,
`FrameBgHovered`, `FrameBgActive`, `TitleBg`, `TitleBgActive`,
`TitleBgCollapsed`, `MenuBarBg`, `ScrollbarBg`, `ScrollbarGrab`,
`ScrollbarGrabHovered`, `ScrollbarGrabActive`, `CheckMark`,
`CheckboxSelectedBg`, `SliderGrab`, `SliderGrabActive`, `Button`,
`ButtonHovered`, `ButtonActive`, `Header`, `HeaderHovered`,
`HeaderActive`, `Separator`, `SeparatorHovered`, `SeparatorActive`,
`ResizeGrip`, `ResizeGripHovered`, `ResizeGripActive`,
`InputTextCursor`, `TabHovered`, `Tab`, `TabSelected`,
`TabSelectedOverline`, `TabDimmed`, `TabDimmedSelected`,
`TabDimmedSelectedOverline`, `DockingPreview`, `DockingEmptyBg`,
`PlotLines`, `PlotLinesHovered`, `PlotHistogram`,
`PlotHistogramHovered`, `TableHeaderBg`, `TableBorderStrong`,
`TableBorderLight`, `TableRowBg`, `TableRowBgAlt`, `TextLink`,
`TextSelectedBg`, `TreeLines`, `DragDropTarget`, `DragDropTargetBg`,
`UnsavedMarker`, `NavCursor`, `NavWindowingHighlight`,
`NavWindowingDimBg`, `ModalWindowDimBg`.

`imgui.StyleVar` holds every `ImGuiStyleVar_*` index (`Alpha`,
`DisabledAlpha`, `WindowPadding`, `WindowRounding`, `WindowBorderSize`,
`WindowMinSize`, `WindowTitleAlign`, `ChildRounding`, `ChildBorderSize`,
`PopupRounding`, `PopupBorderSize`, `FramePadding`, `FrameRounding`,
`FrameBorderSize`, `ItemSpacing`, `ItemInnerSpacing`, `IndentSpacing`,
`CellPadding`, `ScrollbarSize`, `ScrollbarRounding`, `ScrollbarPadding`,
`GrabMinSize`, `GrabRounding`, `ImageRounding`, `ImageBorderSize`,
`TabRounding`, `TabBorderSize`, `TabMinWidthBase`, `TabMinWidthShrink`,
`TabBarBorderSize`, `TabBarOverlineSize`, `TableAngledHeadersAngle`,
`TableAngledHeadersTextAlign`, `TreeLinesSize`, `TreeLinesRounding`,
`DragDropTargetRounding`, `ButtonTextAlign`, `SelectableTextAlign`,
`SeparatorSize`, `SeparatorTextBorderSize`, `SeparatorTextAlign`,
`SeparatorTextPadding`, `DockingSeparatorSize`). The vec2-backed vars
(`WindowPadding`, `WindowMinSize`, `WindowTitleAlign`, `FramePadding`,
`ItemSpacing`, `ItemInnerSpacing`, `CellPadding`,
`TableAngledHeadersTextAlign`, `ButtonTextAlign`, `SelectableTextAlign`,
`SeparatorTextAlign`, `SeparatorTextPadding`) read and write as `(x, y)`
tuples, the rest as scalars.

`imgui.Key` holds the named keys listed above under
[Keyboard interaction](#keyboard-interaction); `imgui.Mod` has `Ctrl`,
`Shift`, `Alt`, `Super`. `imgui.InputTextFlags` has `EnterReturnsTrue`
(32; report changes only on Enter), `ReadOnly`, `Password`,
`CharsDecimal`, `CharsHexadecimal`, `CharsUppercase`, `CharsNoBlank`,
`AutoSelectAll`. `imgui.WindowFlags` has `NoTitleBar`, `NoResize`,
`NoMove`, `NoCollapse`, `NoBackground`, `NoScrollbar`, `MenuBar`,
`AlwaysAutoResize`. `imgui.Cond` has `Always`, `Once`, `FirstUseEver`,
`Appearing` (0 means Always).

See [`examples/python/26_imgui_overlays/`](../../examples/python/26_imgui_overlays/)
for a complete widget reference panel (menu bar, texture images, progress
bar, slider, spawn button, kill counter, health plot), and
[`examples/python/34_scripted_menus/`](../../examples/python/34_scripted_menus/)
("Overture Menu Kit") for a full keyboard-first menu suite built on the
extended API: runtime fonts, UI scaling and themes, key queries and
chords, popups, focus management, docking, and settings persistence.

## CVars

### `bd.get_cvar(name) -> bool | int | float | str`

Returns the CVar in its native Python representation:

```python
gravity = bd.get_cvar("sv_gravity")
```

Integer/color CVars become `int`, float CVars become `float`, bool CVars become
`bool`, and other types become `str`. An unknown name raises `KeyError`.

### `bd.set_cvar(name, value) -> bool | int | float | str`

Converts the value to the target CVar type, applies it, and returns the applied
value:

```python
actual = bd.set_cvar("sv_gravity", 700.0)
```

An unknown name raises `KeyError`; bad numeric conversion or a value outside
the engine's 32-bit integer range raises the corresponding Python conversion
or `OverflowError`. Write-protected, system-only, ignored, and currently
cheat-locked CVars raise `PermissionError`. Writes are blocked in multiplayer
and demos.

For inter-mod communication, define a uniquely prefixed mod CVar with the
normal `CVARINFO` mechanism, then let ZScript and Python read it. Remember that
players can also change user CVars manually.

## Console Commands

### `bd.execute(command) -> None`

Queues an engine console command:

```python
bd.execute('echo "queued from Python"')
bd.execute("quit")
```

The command is not executed inside the Python call. It runs when the engine
drains its command queue, so do not read state immediately and assume the
command has taken effect.

Because console commands can mutate gameplay, `execute` is blocked in
multiplayer and demo sessions.

Never concatenate untrusted text into a console command. Python itself is
trusted, but mod data or user input may still need quoting and validation.

## Calling ACS

### `bd.execute_acs(script, arguments=None, always=False, want_result=False)`

Starts numeric or named ACS in the primary level with the console player's pawn
as activator when one exists.

Numeric script:

```python
started = bd.execute_acs(80, arguments=[10, 20])
```

Named script:

```python
started = bd.execute_acs("OpenArena", arguments=(1,), always=True)
```

Synchronous result request:

```python
result = bd.execute_acs("CalculateReward", arguments=[3], want_result=True)
```

Rules:

- `script` must be an `int` or `str`; other values raise `TypeError`.
- `arguments` is `None` or a sequence of at most four integers.
- More than four arguments raises `ValueError`.
- Non-integer arguments raise Python conversion errors.
- With `want_result=False`, the return is a `bool` indicating whether the
  script was started.
- With `want_result=True`, ACS is requested with result semantics and an `int`
  is returned.
- `always=True` adds ACS's `ACS_ALWAYS` behavior.
- The call is blocked outside a level, in multiplayer, and in demos.

This API does not compile ACS. Package normal compiled ACS lumps as before.

## VFS Text And Helper Modules

### `bd.read_text(path) -> str`

Reads any UTF-8 resource from the current callback/module's own container:

```python
settings = bd.read_text("pyscripts/data/defaults.json")
```

The path must be relative, use forward slashes, and contain no `..`. Missing or
invalid paths raise `FileNotFoundError`. Invalid UTF-8 raises
`UnicodeDecodeError`.

It can only be called while a manifest module or one of that module's callbacks
is executing. Calling it without current-mod context raises `RuntimeError`.

### `bd.read_bytes(path) -> bytes`

Binary companion of `bd.read_text()`. Same container scoping (relative paths,
forward slashes, no `..`), same `FileNotFoundError` contract for missing or
invalid paths, but returns a `bytes` object without decoding. Payloads larger
than 32 MiB raise `ValueError`. Useful for loading binary assets such as font
files:

```python
font_data = bd.read_bytes("fonts/NotoSans-Regular.ttf")
```

It can only be called while a manifest module or one of that module's callbacks
is executing. Calling it without current-mod context raises `RuntimeError`.

### `bd.import_script(path, module_name=None) -> module`

Executes another `.py` resource from the same container and returns its module:

```python
helper = bd.import_script(
    "pyscripts/lib/rewards.py",
    module_name="my_mod_rewards",
)

reward = helper.reward_for_skill(3)
```

Important differences from normal `import`:

- The path must end in lowercase `.py`.
- Resolution is inside the current mod container.
- The helper is executed on each call.
- The module is also registered in `sys.modules` under `module_name`, so
  siblings loaded afterwards can reach it with a plain
  `import my_mod_rewards` — import in dependency order, because a module
  only becomes importable once its `import_script` call has finished
  (circular imports are not supported).
- Store the returned module instead of calling `import_script` every tic.
- Conventional `on_*` names inside a helper are not auto-registered. The
  helper can explicitly use `@bd.on(...)` if it intentionally owns callbacks.

Normal imports such as `import json`, `import collections`, and `import os` use the
bundled CPython standard library. PK3 source directories are deliberately not
added to `sys.path`; use `import_script` for packaged helpers.

### Engine-shipped packages (`src/python/lib/`)

The build stages every package found in the source tree's
`src/python/lib/` directory into the same folder as the embedded standard
library (`python/lib/python3.x/` on Linux, `python/Lib/` on Windows), both
in the build tree and in installs. Because that folder is already on the
interpreter's `sys.path`, mods import these frameworks directly:

```python
import bd_quests
```

Shipped packages are ordinary, dependency-free Python (only `biaseddoom`
is guaranteed to exist). Currently shipped:

| Package | Purpose |
|---------|---------|
| `bd_quests` | Data-driven quest framework: `Quest`/`Objective` definitions, engine-event auto-wiring (`track_kills` with exact player-credit kill attribution by default: `killer="player"`, `killer_class=`, `killer="any"` for the legacy any-death policy, plus `track_pickup`, `track_sector`, `track_conversation_log`), automatic `bd.state` persistence, and an optional Dear ImGui journal (`bd_quests.journal_ui.JournalUI`) with `bind_journal_toggle(key=...)` wiring a console alias/key bind through `pyui`/`ui_command`. Beyond items and messages, a quest's `rewards` dict may carry an `"xp"` amount and `"disposition"` deltas (`(npc_id, delta)` pairs), dispatched on completion to the owning log's `on_xp_reward` / `on_disposition_reward` callback lists (wire them to a rules engine such as `bd_dnd` and a `bd_npcs` `Disposition` store; callback failures only warn and never block completion). See the package docstrings and [`examples/python/27_quest_journal/`](../../examples/python/27_quest_journal/) for a complete campaign. |
| `bd_vtm` | Vampire-the-masquerade-inspired chronicle rules: generation-sized `BloodPool` with nightly upkeep, `Hunger` accrual and deterministic frenzy checks, `Humanity` degeneration rolls, blood-powered `Discipline`s with cooldowns (built-in `celerity`/`obfuscate`/`potence`/`dominate`), `feed()` with witness-driven `Masquerade` violations, `Factions` reputation gating `bd_quests` quests (`requires_faction`), and a `VtMState` container that persists everything through `bd.state`. `bd_vtm.hud.VtMHud` renders the state as a Dear ImGui window, with `bind_hud_toggle(hud, key=...)` for a console alias/key-bind toggle. See the package docstrings and [`examples/python/28_vtm_chronicle/`](../../examples/python/28_vtm_chronicle/) for a complete chronicle. |
| `bd_dnd` | D&D-inspired d20 rules: `roll("2d6+3")` dice notation and `d20()` advantage/criticals, 5e ability scores/modifiers, skill/ability/save checks with proficiency, `Character` XP levels with hit-die level-ups and per-rest resources, `track_xp_from_kills` monster XP with exact player-credit attribution (`player_index=0` default; `None` for the legacy any-death policy), `Party`/`PartyState` multi-character rosters with shared XP and `bd.state` persistence, world helpers (`LockedDoorCheck` bash/pick a locked door, `TrapZone` sector damage saves, `DamageSaveRule` retroactive heal-back saves against incoming damage, `DialogueSkillGate` conversation checks), and a `CharacterState` container that persists everything through `bd.state` (mutually exclusive with `PartyState`: use one per character). `bd_dnd.sheet.CharacterSheet` renders the character (stats, HP/XP bars, color-coded roll log) as a Dear ImGui window, `PartySheet` adds a selectable roster column, and `bind_sheet_toggle(sheet, key=...)` wires a console alias/key-bind toggle. `bd_dnd.Companion` (lazily re-exported from `bd_dnd.companions`) binds a party member to a world actor: a friendly follower (FRIENDLY set, COUNTKILL cleared) that shadows the player on a scheduled follow loop with fit-checked teleport catch-up (spawn and catch-up try a candidate ring around the player and never clip into geometry; `bind(party, anchor=...)` pins the first spawn to a probed slot; a follower that cannot close distance for 30 tics, for example across a ledge, teleports early instead of grinding against geometry), fights what hurts the player, what the player hurts, and what hurts it, and, while combat is recent, proactively engages the nearest visible hostile near the player by assigning its (writable) `target` and nudging the native chase AI (a target the native AI acquired itself is left alone), and two-way-syncs its actor health with the member's RPG hp via the new `Character.on_hp_changed` hook (`set_hp`/`rest`/`level_up` fire it). Companion death incapacitates the member (`hp` 0) and `revive()` respawns the actor at half max hp with a fresh TID; descriptors registered through `PartyState.add_companion` ride along in saves and re-bind by TID after a checkpoint load. The `bd_dnd.classes` layer adds class-based progression: `CharacterClass` definitions (hit die, primary abilities, save proficiencies, class skills, per-level feature dicts with optional `apply` callables, per-rest resource pools, starting equipment) attach with `bind_class`, which applies the level-1 feature package immediately and later levels through the character's `on_level_up` hook (levels in `CLASS_LEVELS_ASI` queue two points on `character.pending_asi`). `CreationWizard` is a pure, engine-free creation model (standard array, point buy, or rolled scores, plus the class-skill picks) with a validating `finish()` that raises `ValueError`, and `track_skill_use`/`advancement_check`/`skill_bonus` grow a flat use-based mastery bonus; class definitions are never persisted (only the `class_id` name and the use/mastery counters ride along in `CharacterState`). See the package docstrings and [`examples/python/29_dnd_dungeon/`](../../examples/python/29_dnd_dungeon/) for a map-agnostic delve. |
| `bd_rpg` | Elemental combat RPG layer: a `DamageTypes` registry (physical/fire/ice/poison/acid/shock/holy/dark pre-registered, plus custom types), per-actor and class-level damage affinities (`set_affinity`/`affinity_of`/`set_class_affinity` — 1.0 neutral, 0.0 immune, 2.0 weak; per-actor overrides class, class defaults match lazily by class name), and `resolve_attack(attacker, defender, attack)` running the full pipeline — optional d20-style hit check, crit roll, `NdM+K` dice (a deliberately small local parser keeping the pack independent of `bd_dnd`), affinity multiplier, flat soak (`min(soak, dmg-1)` so at least 1 gets through unless immune) — applied through `Actor.damage`, so the `actor_before_damage` mutable filter has the last word. The `StatusEngine` singleton `status` applies timed `refresh`/`stack`/`independent` effects from ONE consolidated repeating task (built-ins `burning`/`poisoned`/`slowed`/`stunned`/`regenerating`), with state in `bd.actor_data`. `LootTable` weighted drops plus `LootRules` wiring `actor_died` with exact player-credit attribution and common/uncommon/rare/legendary flash+sound feedback (the screen flash is optional: `LootRules(screen_feedback=False)` skips it and keeps the sound), and genre-neutral kill-XP glue (`award_kill_xp`/`track_kill_xp`). `RpgState` persists player affinities/soak and TID-tagged status timers through `bd.state` (definitions are script-side constants, never persisted). See the package docstrings and [`examples/python/31_elemental_combat/`](../../examples/python/31_elemental_combat/) for a complete scenario. |
| `bd_dialogue` | Branching NPC dialogue trees: data-driven `Dialogue`/`Node`/`Choice` definitions with build-time validation of every `next`/`fail_next` reference, gated choices (`condition(ctx)` hiding, `faction_gate=(name, min_standing)` locking behind `bd_vtm` reputation, `skill_check=(skill, dc)` routing success/failure through `bd_dnd` `Character.skill_check` with an `rng=` escape hatch for scripted test doubles), per-choice `effect(ctx)` hooks and native Strife journal writes via `log=(text, number)`, and real-time `DialogueSession`s (NPC velocity zeroed, one session at a time, stale-NPC auto-end; sessions are transient — nothing is saved mid-dialogue). `bd_dialogue.ui.DialogueUI` renders the active session as a Dear ImGui window — live NPC sprite portrait via `imgui.image(npc_ref)`, faction-colored speaker name, wrapped text, a ~3 s skill-check result flash, and numbered selectable choices (mouse, ImGui keyboard navigation, or per-key digit hotkeys via `bd.imgui.is_key_pressed`). Coexists with the native Strife conversation system. See the package docstrings and [`examples/python/32_dialogue_trees/`](../../examples/python/32_dialogue_trees/) for a complete fixture. |
| `bd_horror` | Horror UX layer: `bd_horror.theme` pushes a full horror skin over `bd.imgui` (near-black windows, dried-blood accents, bone text, square frames; `apply()`/`clear()` track exact push counts) with widget helpers (`begin_window`, `section`, per-tone pulsing `bar`, `omen_text`, `kv_row`, bordered `frame_image` portrait plates) — all legal only inside `imgui_frame`. `bd_horror.toasts` queues diegetic notifications (`info`/`quest`/`loot`/`omen`/`harm`, per-kind palette color, ASCII symbol prefix, stock-Doom UI sound) rendered top-right with fade-in/hold/fade-out timing, plus a history ring for headless autotests. `bd_horror.atmosphere` is the dread machine: `Dread` (a 0-100 meter rising in darkness and near monsters, spiked by player damage, with 25/50/75/100 threshold callbacks, a dread-scaled heartbeat, a display-list vignette, and whisper stings), `LightManager` sector-light programs (`candle`/`fluorescent`/`blackout`, tag-based and restore-on-stop), and `StalkerDirector` (spawns a monster behind the player at high dread). `HorrorState` persists dread and program descriptors through `bd.state` like `bd_vtm.VtMState`. See the package docstrings for usage. |
| `bd_npcs` | NPC hub layer: `NPCDefinition`/`NPCManager` register world NPCs (actor class, relative-to-player or absolute spawn, packed `tint`, dialogue source, `tid_base` stable TIDs) and spawn them friendly/still on `map_load`, adopting savegame-restored actors by TID instead of duplicating them. `Disposition` keeps per-NPC values in [-100, 100] with `hostile`/`cold`/`neutral`/`warm`/`trusted` standings, persisted through `bd.state` (a definition's `start_disposition` seeds only NPCs never met). `manager.nearest`/`prompt`/`begin_talk` give nearest-NPC talk targeting: `begin_talk` opens a `bd_dialogue` session whose ctx adds `disposition`, `standing`, `npc_id`, and `dispositions` (one conversation at a time), with a restylable `PROMPT_FORMAT`. `NPCManager.retire(npc_id)` takes an NPC off duty (the recruit who becomes a follower): the actor leaves the world, the manager stops tracking and respawning it across savegames, and its disposition standing survives. `Service`/`HealerService` (currency fee behind a standing floor, heals the pawn and a `bd_dnd` character's RPG hp) and `TrainerService` (`bd_dnd.classes.advancement_check` rolls) implement offers; `Shop` is a currency store with stock counts, restock timers, refund-on-failure buys, and count-only persistence, plus a guarded ImGui `ShopUI`. See the package docstrings for usage. |

`bd_horror` is the horror UX layer: it bundles presentation (the ImGui skin and the toast queue) with atmosphere simulation (dread, light programs, stalkers) so a mod can stand up a coherent survival-horror feel with a few calls. Everything world-facing runs on `bd.schedule` tasks of at least 35 tics and pushes actor filters into `bd.actor_refs` keyword arguments, per the [performance guide](../development/python-performance.md); all randomness flows through the deterministic script RNG, so candle flicker and stalker rolls resume exactly after a checkpoint load. Light programs bind to sector tags rather than TIDs, which is what lets them rebind naturally across map transitions and savegames.

## Persistent State And Savegames

`bd.state` is one shared dictionary created before mod modules execute:

```python
bd.state.setdefault("com.example.my_mod", {})
mine = bd.state["com.example.my_mod"]
mine["bosses_defeated"] = mine.get("bosses_defeated", 0) + 1
```

Always namespace your data. A reverse-domain name, repository slug, or another
globally distinctive key prevents collisions.

Mutate this dictionary in place. Do not assign a new object to `bd.state` or
delete the attribute; save/load then raises `TypeError` rather than persisting
ambiguous state. Use `bd.state.clear()` when a deliberate full reset is needed.

### JSON-compatible values

The entire dictionary is serialized with Python's `json` module. Store only:

- dictionaries with string keys;
- lists;
- strings;
- integers and finite floats;
- booleans;
- `None`.

Do not store modules, functions, sets, bytes, actor snapshots that you expect
to stay live, open files, custom class instances, or other non-JSON objects.

Tuples encode as JSON arrays and return as lists. Avoid `NaN` and infinities
for portability even though a particular Python JSON implementation may emit
them.

### Save sequence

When saving the primary level:

1. BiasedDoom dispatches `save`.
2. It runs `json.dumps(bd.state, sort_keys=True, ensure_ascii=False)`.
3. The resulting UTF-8 JSON string is written as `pythonstate` in level data.
4. Existing ACS module/deferred/global serialization and ZScript thinker/event
   serialization continue through their normal paths.

If encoding fails, the traceback is logged. Fix the bad state type; do not
assume a save contains Python state merely because the rest of the game saved.

### Load sequence

When reading valid nonempty `pythonstate`:

1. JSON is parsed.
2. The result must be a dictionary.
3. The existing `bd.state` object is cleared and updated in place. References
   to the dictionary itself remain valid.
4. Actors and players finish restoration.
5. `load` runs.
6. The later `map_load` event has `from_savegame=True`.

Use stable identifiers—TIDs, class names, and your own IDs—in persistent data.
Never attempt to serialize a pointer or treat an old snapshot as a live actor.

### Rebinding actor handles after load

Handles never survive a savegame: store **TIDs** (not handles) in `bd.state`,
then re-resolve live handles in the `load` event with `bd.actor_ref`. Check
`.valid` to detect actors that no longer exist after the load:

```python
watchers = []  # live handles; rebuilt on load, never persisted


def on_save(event):
    mine["watcher_tids"] = [w.tid for w in watchers if w.valid]


def on_load(event):
    watchers.clear()
    for tid in mine.get("watcher_tids", []):
        actor = bd.actor_ref(tid)
        if actor is not None and actor.valid:
            watchers.append(actor)
        else:
            bd.warn(f"watcher TID {tid} no longer exists after load")
```

Give every actor you intend to track a nonzero TID at spawn time; actors with
TID `0` cannot be re-resolved this way. The same recipe applies to
`map_unload`/`map_load` transitions, where handles are also invalidated.

### Reload behavior

The console command:

```text
py_reload
```

does the following:

1. JSON-encodes the current `bd.state`.
2. Dispatches `engine_shutdown` and finalizes CPython.
3. Rediscovers manifests and starts a fresh interpreter.
4. Executes modules and dispatches `engine_start` with a fresh dictionary.
5. Restores the encoded state.
6. Dispatches `map_load` if a level was active.

Therefore `engine_start` during reload does not see the old state yet. Put
default initialization in `setdefault` calls so restoration can safely replace
the fresh values afterward.

If reload fails after shutdown, the Python runtime remains inactive; inspect
the traceback, correct the source, and run `py_reload` again.

## ACS And ZScript Coexistence

No ACS or ZScript loader was removed or redirected. Python integration is made
at lifecycle points after the established handlers.

### One PK3 using all three

```text
hybrid-mod.pk3
├── PYTHON
├── ZSCRIPT
├── LOADACS               # when the mod uses library ACS
├── acs/
│   └── mylibrary.o
└── pyscripts/
    └── main.py
```

Or a map WAD can retain its `BEHAVIOR` lump while a containing/companion PK3
adds Python.

### Python to ACS

Use `bd.execute_acs` for a direct script start. The ACS script keeps its normal
number/name, activation behavior, map variables, and save serialization.

### Python to ZScript

API version 2 lets a live `Actor` call supported methods on its runtime ZScript
class. This keeps class-specific behavior in ZScript while Python orchestrates
it without a console command, polling CVar, or one-tic queue:

```c
class PythonDrivenImp : DoomImp
{
    int BoostFromPython(int healthGain, Vector3 impulse, Actor newTarget)
    {
        health += healthGain;
        Vel += impulse;
        target = newTarget;
        return health;
    }
}
```

```python
imp = bd.spawn("PythonDrivenImp", x, y, z, force=True)
new_health = imp.call_zscript("BoostFromPython", 10, (2, 0, 1), pawn)
```

The bridge accepts integer-compatible values, floats, strings, 2/3/4-component
floating vectors, `Actor` subclasses, and `None` for nullable actor arguments.
It supports zero or one return value of the same categories and dispatches
virtual overrides. Arguments are checked against the reflected prototype
before entering the VM.

For safety and ABI clarity it rejects private/protected/internal, static,
action, abstract, UI-scope, unsafe, vararg, `out`/`ref`, multi-return, and
unsupported pointer/container signatures. A rejected signature raises
`PermissionError` or `TypeError`; a VM abort becomes `RuntimeError`. It does
not expose arbitrary `DObject` references, static functions, state actions, or
raw reflection data.

CVars, ACS, TIDs, and independent events remain useful looser boundaries when
the two scripts should not share a direct actor-class contract.

### Callback ordering that matters

Current integration intentionally preserves established behavior:

- ZScript actor-spawn/death handlers run before the matching Python event.
- Local ZScript `WorldTick` runs before Python `tick`.
- Normal ZScript world-load handlers run before Python `map_load`.
- Transition ACS unloading and ZScript unload handlers run before Python
  `map_unload`.
- Python state is added alongside, not instead of, existing ACS/ZScript save
  data.

Ordering is useful for observation, but avoid tightly coupling unrelated mods
to it.

## Performance And Determinism

Python runs synchronously on the engine thread. A slow callback delays the
game, rendering, input processing, and every other script runtime.

When Python is not opted in, tic/gameplay hooks take only the inactive native
fast path: CPython is not initialized and no event dictionaries or handles are
allocated. With Python active, a cached per-event presence bit keeps
unsubscribed hooks allocation-free. For subscribed hot paths, live handles,
native decorator filters, and `apply_actor_batch` avoid snapshot construction
and excessive Python/C crossings.

Every `biaseddoom` function must be called on that scripting thread. Do not
call the API from `threading.Thread`, executor workers, or callbacks owned by a
third-party background thread: the function raises `RuntimeError`. Background
work must hand plain data back for a later engine callback to consume, and the
mod remains responsible for making that handoff safe. Direct `bd.state`
access is still ordinary Python dictionary access, but keeping all mod state
on the callback thread is strongly recommended.

`py_tick_budget_ms` defaults to `3`. Scheduled tasks plus `pre_tick`, `tick`,
and `post_tick` share that whole-tic wall-clock budget. With
`py_tick_hard_budget=true`, once the budget is consumed the dispatcher skips
remaining Python callables until the next tic. This limits cumulative Python
work without adding tracing overhead to every Python line.

An individual callable cannot be interrupted safely while it is executing.
It may exceed the limit once; the runtime records and warns about that
overrun, then prevents later work in the same tic. By default,
`py_tick_overrun_limit=3` disables a callback (or cancels a repeating task)
after three consecutive individual overruns. `0` disables repeat-offender
removal. `py_tick_budget_ms=0` disables budget enforcement and warnings.
`py_reload` re-enables budget-disabled callbacks.

`bd.profile()` returns per-callback/task call counts, total/max microseconds,
budget skips/overruns, failure/disable state, and current budget settings.
`bd.reset_profile()` clears measurements but does not re-enable callbacks.
See the
[Python performance guide](../development/python-performance.md) for the
crossing cost model, profiling walkthrough, and batching recipes.

### Synchronous task scheduling

`bd.schedule(callback, delay=1, repeat=0, map_local=True)` returns a task ID.
Tasks run at the start of a future `pre_tick` under the same engine-thread and
budget rules. `repeat=0` is one-shot; a repeating callable may also return
`False` to cancel itself. Map-local tasks are cancelled on unload.
`bd.cancel_task(id)` returns whether it cancelled an active task, and
`bd.task_count()` returns the active count. `py_max_tasks` bounds the queue.

Recommended tick patterns:

```python
def on_tick(event):
    # Once per second rather than every tic.
    if event["level_time"] % bd.TICRATE != 0:
        return
    update_objectives()
```

Guidelines:

- Do not scan `bd.actor_refs(limit=1000000)` every tic.
- Filter by class or TID and cache only stable IDs.
- Move rare work to map/spawn/death callbacks.
- Break long work across tics with explicit state.
- Load/parse static VFS data once at module load or `engine_start`.
- Do not perform blocking network, subprocess, or disk operations in callbacks.
- Profile release builds as well as debug builds.

### Multiplayer and demos

API version 2 does not define a deterministic Python networking protocol.
While `netgame`, `multiplayer`, demo playback, or demo recording is active,
the session is *read-only* for world state. All live-handle gameplay
mutation and these legacy functions reject calls with `RuntimeError`:

- `spawn_actor`
- `damage_actor`
- `set_actor_velocity`
- `destroy_actor`
- `set_cvar`
- `execute`
- `execute_acs`

The same applies to `spawn`, `execute_special`, `radius_damage`,
`apply_actor_batch`, `spawn_missile`, `line_attack`, `exit_level`,
`change_level`, `set_timescale`, `save_checkpoint`, and `load_checkpoint`.
Each blocked call also emits a deduplicated yellow
`SCRIPT WARNING: mutation blocked during multiplayer/demo session` line,
which is not counted as a `-scripttest` error.

Observer mode: queries, logging, VFS reads — and purely local
presentation — keep working. `center_message`, `set_music`, `hud_text`,
`hud_clear`, `screen_flash`, `screen_fade`, `play_ui_sound`, and the
`bd.draw_*` display list only affect the local console player's screen and
audio, so they are allowed whenever a level is active, even in multiplayer
and demo sessions. Use `bd.session_read_only()` to tell the modes apart:

```python
if bd.session_read_only():
    bd.hud_text("observer", id=99)   # local presentation: fine in-level
else:
    bd.spawn("DoomImp", x, y, z)     # world mutation: solo sessions only
```

Do not use Python to implement
multiplayer-authoritative gameplay in this API version.

## Console And Configuration Reference

### Command-line switches

| Switch | Meaning |
|--------|---------|
| `-python` | Opt into all discovered trusted Python mods for this process. |
| `-nopython` | Force Python off even when `py_enabled` is archived true. |

### CVars

| CVar | Default | Meaning |
|------|---------|---------|
| `py_enabled` | `false` | Archived, user-owned global trust opt-in. Prefer `-python` while testing individual mods. |
| `py_tick_budget_ms` | `3` | Whole-tic Python budget in milliseconds; `0` disables enforcement/warnings. |
| `py_tick_hard_budget` | `true` | Skip later tasks/callbacks after the current tic consumes its budget. |
| `py_tick_overrun_limit` | `3` | Consecutive individual overruns before disabling/cancelling; `0` disables this containment. |
| `py_max_tasks` | `4096` | Scheduled-task ceiling, clamped internally to `1..100000`. |

Be conservative with `py_enabled=true`: any future command line that loads a
Python-bearing PK3 will then execute it unless `-nopython` is supplied.

### Console commands

| Command | Meaning |
|---------|---------|
| `py_status` | Show compiled/active/requested state, manifests, modules, and callback count. |
| `py_reload` | Rebuild the interpreter and scripts while preserving JSON-compatible state. |
| `pyui <name>` | Dispatch a `ui_command` event to Python scripts (for console aliases and key binds). |

`py_reload` is an unsafe console command under the engine's normal command
security classification.

## Building Python Support

Python support is enabled by default when CPython development files version
3.10 or newer are found.

| CMake option | Default | Meaning |
|--------------|---------|---------|
| `BIASEDDOOM_ENABLE_PYTHON` | `ON` | Attempt to build the embedded runtime. |
| `BIASEDDOOM_REQUIRE_PYTHON` | `OFF` | Fail configuration instead of compiling stubs when CPython is unavailable. |

Recommended verification configure:

```bash
cmake -S . -B build \
    -DBIASEDDOOM_ENABLE_PYTHON=ON \
    -DBIASEDDOOM_REQUIRE_PYTHON=ON
cmake --build build --target zdoom --parallel
```

Look for:

```text
-- Embedded Python scripting enabled with CPython 3.x.y
```

### Linux

Install the development package. Debian/Ubuntu example:

```bash
sudo apt install python3-dev
```

Fedora:

```bash
sudo dnf install python3-devel
```

Arch:

```bash
sudo pacman -S python
```

The build stages a private standard library under
`python/lib/python<major>.<minor>/` and the matching `libpython` SONAME beside
`biaseddoom`. CPython's test suite, bytecode caches, and build configuration
directory are excluded from packages because they are not runtime libraries.
The redistributed CPython terms are retained at `python/LICENSE.txt`.

Keep the executable, `libpython*.so*`, and `python/` directory together in a
portable package.

### Native Windows (MSVC)

The native vcpkg build automatically enables the `vcpkg-python` feature for
the static `x64-windows-static` triplet. The standard library is staged as:

```text
biaseddoom.exe
python/
├── LICENSE.txt
└── Lib/
    └── encodings/
        └── __init__.py
```

The normal helper builds and validates it:

```powershell
powershell -ExecutionPolicy Bypass -File tools\build-windows.ps1 `
    -Configuration Release -Package
```

Use `-NoPython` only when intentionally producing a stub build. The package
validator requires both `python\Lib\encodings\__init__.py` and
`python\LICENSE.txt` otherwise.

### Windows MinGW

The vcpkg CPython port used by this project does not support MinGW. The
cross-MinGW helper explicitly builds the stub path:

```bash
./tools/build-windows-mingw.sh --package
```

That package supports ACS and ZScript normally but cannot run Python mods. Use
the native MSVC Windows package when Python is required.

### macOS

The manifest automatically enables the pinned vcpkg CPython port on macOS.
The build requests static linkage so the application does not retain a
dependency on the build machine's Homebrew prefix. The private stdlib is staged
beside the executable inside the app bundle. Bootstrap/use the repository's
vcpkg toolchain as shown in the normal build instructions.

### Stub behavior

With `BIASEDDOOM_ENABLE_PYTHON=OFF`, missing development files, or MinGW, the
engine compiles lightweight stubs. ACS and ZScript remain enabled. If the user
requests Python, the engine logs that the executable lacks CPython support.

Use `BIASEDDOOM_REQUIRE_PYTHON=ON` in CI/release configurations so an accidental
stub build fails at configure time.

## Packaged Examples

The [example suite](../../examples/python/) contains sixteen focused mods. Each
has its own root `PYTHON` manifest, source, and README, and can be packaged and
loaded independently. Together they cover lifecycle events, live handles,
combat and inventory, player input, sectors and lines, native event filters,
scheduling, save state, VFS helpers, typed ZScript calls, batched updates,
profiling, UI/audio, level flow, gameplay direction (time scaling, HUD
text, screen effects, seeded RNG, and checkpoints), and canvas drawing
(persistent display-list HUD items and world-anchored bars/labels).

Build the complete suite or a selected subset:

```bash
./tools/build-python-examples.sh
./tools/build-python-examples.sh 02_live_actor_handles 10_zscript_bridge
```

Packages are written to `build/python-examples/`. Validate every source and
archive with `./tools/test-python-examples.sh`; pass `--iwad PATH` to also load
MAP01 with each package, exercise its initial callbacks, and require a clean
Python-driven exit through a Python-enabled BiasedDoom executable.

### Full integration fixture

The repository contains a complete hybrid example at:

```text
examples/python/hello_world/
├── PYTHON
├── ZSCRIPT
└── pyscripts/
    ├── autotest_failure.py
    ├── helper.py
    └── main.py
```

Its ZScript marker and `PythonBridgeProbe` prove that the engine parses both
languages and that Python can invoke a public class method through the typed
bridge. The Python source demonstrates:

- same-container helper import;
- conventional and decorator callbacks;
- stdout redirection and logging;
- legacy snapshots plus live actor/player/sector/line handles;
- CVar reads;
- direct fields, relationships, inventory, batch mutation, action specials,
  actor spawn/velocity/damage/destruction events, and ZScript invocation;
- pre/tick/post phases, profiling, hard budget skipping, and overrun disable;
- shared state;
- save and load callbacks;
- clean command-queue shutdown in test mode.
- worker-thread API rejection and failed-import callback rollback in test mode.

Build it with:

```bash
./tools/build-python-example.sh
```

Output:

```text
build/python-hello-world.pk3
```

Run it interactively:

```bash
./build/biaseddoom \
    -iwad /path/to/DOOM2.WAD \
    -file ./build/python-hello-world.pk3 \
    -python -stdout \
    +logfile python-example.log
```

The `BIASEDDOOM_PYTHON_AUTOTEST` variables used in the source are test-harness
controls. Ordinary interactive launches do not set them and remain playable.

## Automated Integration Test

After building `build/biaseddoom`, run:

```bash
./tools/test-python-scripting.sh --iwad /path/to/DOOM2.WAD
```

Or:

```bash
BIASEDDOOM_TEST_IWAD=/path/to/DOOM2.WAD \
    ./tools/test-python-scripting.sh
```

Options:

```text
--exe PATH       test a different executable
--timeout SEC    per-run timeout, default 45
--keep-temp      retain logs/config/save/stdout for inspection
```

Prerequisites:

- a built Python-enabled BiasedDoom;
- a compatible IWAD;
- GNU `timeout`;
- a graphical display, or `xvfb-run` on a headless machine;
- optional host `python3` for the preflight syntax check.

The script performs two actual engine processes.

### Active run coverage

It:

1. Packages the example.
2. Optionally compiles its source with host `py_compile`, redirecting bytecode
   outside the repository.
3. Starts the first map with `-python`.
4. Verifies embedded startup and same-PK3 VFS import.
5. Verifies lifecycle callbacks and pre/tick/post phases.
6. Queries snapshots and native handles, then mutates actors/world data.
7. Verifies spawn, damage, death, destruction, filtering, and invalidation.
8. Calls typed ZScript methods and verifies private-method rejection.
9. Forces one budget overrun and verifies same-tic skipping plus containment.
10. Queues a real engine save.
11. Waits for the save file to exist before queueing a load.
12. Verifies `save`, JSON state restoration, `load`, and
    `map_load(from_savegame=True)`.
13. Queues a clean engine shutdown from Python.
14. Verifies `engine_shutdown` and the created save file.

### Inactive run coverage

It starts the same PK3 without `-python`, verifies the explicit trust warning,
and verifies that `PYTEST engine_start` never appears. This catches accidental
removal of the security gate.

The final success text is:

```text
PASS: Python startup, VFS import, native real-time handles/mutations, callbacks,
      JSON save/load, typed ZScript bridge, shutdown, and trust opt-in all passed.
```

## Manual Test Matrix

Use this matrix for changes that touch lifecycle, serialization, packaging, or
public APIs.

| Test | Procedure | Expected result |
|------|-----------|-----------------|
| Compile-on | Configure with enable+require ON | Configure reports CPython and build links. |
| Compile-off | Configure with enable OFF | Build succeeds with stubs; ACS/ZScript still parse. |
| Trust default | Load example without `-python` | Warning appears; no Python marker executes. |
| Trust opt-in | Add `-python` | Module and callbacks load. |
| Trust override | Archive `py_enabled=true`, launch `-nopython` | No Python executes. |
| Status | Run `py_status` | Compiled/active/manifests/modules/callbacks are accurate. |
| Startup error | Add a syntax error to one entry | Traceback identifies resource/path; other entries continue. |
| Callback error | Raise in `on_tick` | One traceback and disable message; engine and other callbacks continue. |
| Reload | Fix source and run `py_reload` | Failed callback returns; JSON-compatible state survives. |
| VFS isolation | Put same helper path in two PK3s | Each entry reads/imports its own container's helper. |
| VFS traversal | Call `read_text("../secret")` | `FileNotFoundError`; no VFS traversal. |
| Map entry | Start first map | `map_load` has correct map and `from_savegame=False`. |
| Level transition | Exit to next map | `map_unload` then next `map_load`. |
| Actor query | Query/filter known actors | Snapshots contain all documented fields. |
| Spawn fit | Spawn into a blocked point without force | Runtime error and no surviving actor. |
| TID mutation | Spawn with a unique TID, mutate, query | Returned/query snapshots reflect changes. |
| Save/load | Change namespaced state, save, change it, load | Saved JSON state returns before `map_load(True)`. |
| Bad state | Put a set/function in `bd.state`, save | Serialization traceback clearly identifies failure. |
| Old save | Load save with no Python state | Game loads; no Python `load` callback. |
| Multiplayer guard | Try mutation in a network game | `RuntimeError`; synchronization is not changed. |
| Demo guard | Try mutation while recording/playback | `RuntimeError`. |
| Tick budget | Exceed a 1 ms hard budget before a lower-priority callback | Later work is skipped; repeat offender is disabled at its configured limit. |
| ACS bridge | Call packaged numeric and named ACS | Script starts with up to four arguments. |
| ZScript hybrid | Call a probe actor's typed public method and then a private method | Public mutation/return succeeds; private call raises `PermissionError`. |
| Legacy-only mod | Load existing ACS/ZScript mod with no manifest | Behavior is unchanged; CPython need not initialize. |
| Windows package | Run helper with `-Package` | `python/Lib/encodings` is present. |
| Linux package | Inspect executable directory/AppDir | stdlib and matching `libpython` SONAME are present. |
| MinGW package | Run `py_status` | Reports not compiled; ACS/ZScript remain usable. |

## Debugging Workflow

Use this command line while developing:

```bash
./build/biaseddoom \
    -iwad /path/to/DOOM2.WAD \
    -file /path/to/mod.pk3 \
    -python -stdout \
    +developer 1 \
    +logfile /tmp/biaseddoom-python.log
```

Then:

1. Run `py_status` after startup.
2. Search the log for `Python`, the PK3 filename, and the VFS source path.
3. Fix the first traceback, since later failures may be consequences.
4. Run `py_reload` for source-only iteration.
5. Fully restart when testing manifests, resource load order, startup flags,
   native packaging, or save compatibility.

Traceback filenames use the packaged path, and helper module `__file__` values
use a `vfs://` label where applicable.

## Troubleshooting

### “Python scripting was requested, but this executable was built without CPython support”

Reconfigure with Python 3.10+ development files and:

```bash
-DBIASEDDOOM_ENABLE_PYTHON=ON -DBIASEDDOOM_REQUIRE_PYTHON=ON
```

Use the native MSVC package rather than MinGW on Windows.

### “Python script was found but not executed”

This is the trust gate working. Add `-python` only after reviewing/trusting all
loaded Python mods.

### Fatal error mentioning `encodings`

The interpreter library and private stdlib do not match or the `python/`
directory was omitted from the package. Restore the directory produced by the
same build as the executable. Do not mix Python trees from different releases.

### Manifest is ignored

Check that:

- its archive path is exactly `PYTHON` at root;
- it is not `PYTHON.txt`;
- every script uses a same-container relative path;
- `.py` is lowercase;
- the ZIP has no extra enclosing directory;
- `py_status` sees valid entries.

### `ModuleNotFoundError` for another PK3 file

PK3 paths are not normal Python packages. Replace:

```python
import helper
```

with:

```python
helper = bd.import_script("pyscripts/helper.py", module_name="my_helper")
```

### `read_text`/`import_script` says there is no current mod

Call it at manifest module top level or from a registered callback. A detached
function invoked after the callback context ends has no implicit container.

### Actor lookup returns `None`

The actor may have been destroyed, replaced, never assigned that TID, or may
have TID `0`. Re-query from the primary level and assign stable nonzero TIDs to
objects Python must mutate.

### Mutation raises a synchronization error

You are in multiplayer, demo playback, or demo recording. This is an API
contract, not a CVar to bypass. Move deterministic gameplay to ACS/ZScript.

### Mutation says no level is active

Do it from `map_load`, `tick`, or a later level callback—not
`engine_start`/`engine_shutdown`.

### Callback stopped firing

Find the earlier traceback or budget “disabled until py_reload” line. Use
`bd.profile()`/`py_status` to distinguish an exception from consecutive
overruns, fix or split the work, and run `py_reload`.

### Save state is missing after load

Look for a JSON serialization traceback during `save`. Confirm your values are
JSON-compatible and that the save was created after Python became active.

### `py_reload` resets something unexpectedly

Only `bd.state` is preserved. Ordinary module globals are rebuilt. Also,
`engine_start` runs before the old state is restored during reload; use
`setdefault` and perform restored-state work in `map_load`.

### Game stutters every tic

Reduce actor scans, lower callback frequency, move work to event callbacks,
and inspect `bd.profile()`, budget warnings, skips, and disabled callbacks. The
GIL and main-thread execution are intentional in API version 2.

## Versioning And Forward Compatibility

Check the API before depending on future additions:

```python
import biaseddoom as bd

if bd.API_VERSION < 2:
    raise RuntimeError("This mod requires BiasedDoom Python API 2")
```

API version 2 guarantees the names and core semantics documented in this file.
New keys or functions may be added compatibly. Mods should:

- read event dictionaries by named keys;
- tolerate extra keys;
- avoid depending on callback order between unrelated mods;
- avoid importing internal engine modules;
- use `bd.RUNTIME` for diagnostics rather than assuming a particular patch
  version from `sys.version`;
- ship a clear security note telling players why `-python` is needed.

Callbacks registered while another callback is running become eligible on the
next event dispatch. They are never inserted into the event currently being
iterated.

## Current Intentional Limits

- Python is trusted, not sandboxed.
- The opt-in is process-wide, not per mod.
- Gameplay mutation is single-player/non-demo only.
- Live handles cover playsim actors, players, sectors, and lines, not arbitrary
  engine `DObject`, renderer, menu, or VM reflection objects.
- The ZScript bridge is deliberately typed and actor-method-only; actions,
  statics, UI/unsafe/private/ref/out/container/multi-return signatures remain
  unavailable.
- PK3 modules use `import_script`, not automatic `sys.path` mounting.
- The bundled standard library is authoritative, but modules that depend on
  optional native CPython extensions can vary by platform/build; `pip`, user
  site packages, and arbitrary host installations are not exposed.
- State persistence is JSON only.
- Python callbacks are synchronous and single-threaded with engine execution.
- A running Python callable cannot be forcibly preempted; hard budgets skip
  later work and disable consecutive offenders.
- Engine API calls from Python-created background threads are rejected.
- MinGW builds contain stubs because the selected vcpkg CPython port does not
  support that toolchain.

These limits protect engine lifetime, save compatibility, VFS ownership, and
network/demo synchronization while leaving ACS and ZScript available for the
jobs they already perform well.
