# The Confessor

A **native Strife-conversation horror fixture** for the
`conversation_started` / `conversation_reply` Python hooks. In the
candle-dim entrance of Doom II MAP01, **The Confessor**, an ashen,
vestment-dark cleric, trades absolution for secrets through a genuine
ZSDF dialogue tree, while the shipped `bd_horror` pack dresses the scene:
a slow candle flame breathes in his booth sector, diegetic toasts whisper
in the corner, and a themed ImGui **Rite panel** tracks your penance.
A gold **"!" label and ground ring** hover over him until his quest is
accepted, so the quest giver reads at a glance, and a decline is never a
dead end: the offer stands until you take it.

The mechanics are byte-identical to the original conversation-quests
fixture (same `DIALOG01` structure, node/reply indices, `log = "LOG77"`,
`giveitem = 921`, conversation IDs); only the prose, and everything
around it, went into the dark.

## Architecture

The example is four Python modules, all listed in the root `PYTHON`
manifest in dependency order. Each sibling module self-registers a stable
`sys.modules` alias (a live proxy, since the engine registers manifest
modules under mangled names only *after* execution), so later entries
reach it with a plain `import`, the same sibling-import mechanics
`hello_world` gets via `bd.import_script(..., module_name=...)`.

| Module | Role |
|---|---|
| `pyscripts/content.py` | **Pure data + factories**: fixture constants (conversation IDs, log number/label), the quest factory, candle tuning, marker ids/tuning, and every player-visible string (toasts, Rite-panel lines, the inactive hint). No engine calls at import. |
| `pyscripts/systems.py` | **Event wiring and rules**: quest registration, `conversation_started`/`conversation_reply` handlers, the log-update/reward/offer-stands toasts, the NPC spawn, the quest-giver marker (module-level `marker_state` dict plus a 7-tic map-local refresh task), and `PositionCandle` (an example-local `bd_horror.atmosphere.LightProgram` subclass that binds to the Confessor's sector by *position* via `bd.sector_at`, since the MAP01 booth sector is untagged and sector tags are read-only from Python). |
| `pyscripts/ui.py` | **The Rite panel**: a `bd_horror.theme`-skinned ImGui window (quest state out of `bd_quests`, the Confessor's presence, the last line of the rite as `omen_text`, and a plain hint line while the quest is INACTIVE) plus the toast queue. Lives entirely inside the `imgui_frame` handler, so it is a pure no-op under `-headless`; also carries the no-`+map` autowarp fallback. |
| `pyscripts/main.py` | **Thin bootstrap**: the `BD_EXAMPLE_AUTOTEST=1` schedule and the `BD_EXAMPLE_SCREENSHOT=1` pose. All assertions live here, reading state out of `systems` and prose out of `content`. |

## What it teaches

- **How dialogue lumps are registered.** The map loader
  (`MapLoader::LoadStrifeConversations`) looks for a `DIALOGxx` lump
  matching the map name, `DIALOG01` for MAP01, in the global namespace,
  for **any** game, not just Strife. Text lumps parse as USDF/ZSDF
  (grammar: [`specs/usdf.txt`](../../specs/usdf.txt)); binary lumps are
  original Strife `SCRIPTxx` data.
- **Two classes bound by one table.** The MAPINFO `conversationids` block
  registers `920 = QuestScribe` (the Confessor's actor class) and
  `921 = Shell` (the reply's `giveitem`). The ZScript `ConversationID`
  actor property is **DECORATE-only**, so ZScript-defined NPCs bind
  through MAPINFO, exactly what Strife itself does in
  `wadsrc/static/mapinfo/conversationids.txt`.
- **Namespace choice matters.** Only the Strife namespace has numeric log
  entries (`log = "LOG77"`), which `player_t::SetLogNumber` resolves to
  the `$TXT_LOGTEXT77` label defined in the bundled `LANGUAGE` lump, and
  which `bd_quests.log.track_conversation_log` matches on.
- **Driving a conversation from Python.** `bd.start_conversation(npc)`
  opens the real `ConversationMenu`; replies commit through the real
  netcode via the fixture's UI-scope `ConvoReplyBridge` static event
  handler (see `ZSCRIPT`), reachable from Python as
  `bd.execute("event convpick <node> <reply>")`.
- **Quest composition.** Talking to the Confessor starts the *Price of
  Absolution* quest from the `conversation_started` handler; the accept
  reply's numeric log completes its objective via
  `track_conversation_log`.
- **Quest-giver marking.** A gold `bd.draw_world_text` "!" label and a
  `bd.draw_world_ring` hover over the Confessor under the stable
  display-list ids `97000`/`97001` (the bd.ui toolkit owns ids >=
  900000; this example owns the 97000-97009 block). A slow repeating
  `bd.schedule` task (7 tics, `map_local=True`, so the engine cancels it
  on unload) re-syncs the marker with the quest state, drawing it while
  the quest can still be accepted and clearing it the tic the rite is
  sealed (the quest's `on_complete` hook calls the same sync, so the
  clear is immediate). The module-level `marker_state` dict (`shown`,
  `draws`/`clears` counters, task id) is what the headless autotest
  asserts on, since display-list drawing is a no-op under `-headless`.
- **A decline is not a dead end.** Declining leaves the quest ACTIVE:
  the marker keeps burning, a toast says the offer stands, and the
  conversation can simply be re-entered and accepted.
- **The `bd_horror` pack.** `toasts.toast(..., kind="quest")` on the
  log-update reply (with a `history` ring for headless assertions), a
  `kind="loot"` toast naming the shell payout, a `LightProgram` candle
  over an untagged sector (subclass `_resolve`, re-arm on `map_load`,
  disarm-without-restore on `map_unload`), and a
  `theme.apply()`/`begin_window`/`section`/`kv_row`/`faded_text`/`omen_text`
  panel.

### A note on the log toast

The accept reply carries a *numeric* log, so `conversation_reply`'s
`log_string` is None and the `$TXT_LOGTEXT77` label cannot be resolved
from Python: **no localization lookup is exposed in the `bd` API**
(verified against `docs/scripting/biaseddoom.pyi`). The toast therefore
carries a fixed themed line from `content.py`; `systems._toast_reply`
toasts a free-text `log_string` verbatim when one ever appears.

## The fixture

`DIALOG01` has one page with two choices:

| Choice | Effect |
|---|---|
| "Bless me, Confessor, for I have sinned." | Sets log 77 (`$TXT_LOGTEXT77`), gives a box of shells as alms, completes the quest, toasts the sealed rite and the shell payout, stands the marker down, closes the dialog |
| "My sins are my own." | A dismissal: closes the dialog, leaves log and inventory untouched, keeps the quest ACTIVE, toasts that the offer stands |

Observable quirks the autotest pins down:

- `bd.player_log()` returns the `$TXT_LOGTEXT77` **label**, not the
  resolved text: `SetLogNumber` stores the label so language switches
  re-translate. Before any accept it returns `None` (the log is unset),
  which is how the autotest proves a decline leaves the log untouched.
- `conversation_reply` reports `log_string = None` for numeric logs and
  `next_node = -1` when the reply has no `nextpage`.
- While the menu is open the NPC carries the internal `INCONVERSATION`
  flag; it clears when the reply commits. The flag is *internal*, so
  `Actor.get_flag` cannot see it: the NPC's ZScript `InConversation()`
  accessor reads `bINCONVERSATION` via `Actor.call_zscript`.
- The conversation can be re-entered after a decline: the native menu
  closes cleanly on the reply (see `ConvoReplyBridge` in `ZSCRIPT`), so
  `bd.start_conversation(npc)` simply opens it again.

## Running it

```bash
./build/biaseddoom -python -iwad ~/games/doom2.wad \
    -file examples/python/30_conversation_quests +map map01
```

Walk up to the marked figure ahead of you (the gold "!" and ring) and
press **USE**: the native conversation menu opens over the candle-lit
booth, the Rite panel tracks your penance on the left (with a hint line
until you first talk), and toasts whisper top-right. Decline and the
offer stands; kneel again whenever you are ready.

Headless autotest (deterministic; starts the conversation
programmatically, declines first to prove the quest survives a refusal,
then re-enters and accepts, asserting both reply payloads, the marker
lifecycle out of `marker_state`, the candle program, and every toast in
the `bd_horror` history):

```bash
BD_EXAMPLE_AUTOTEST=1 ./build/biaseddoom -headless \
    -iwad ~/games/doom2.wad -file examples/python/30_conversation_quests \
    -python -scripttest 1400 4 -nosound +map MAP01
# -> SCRIPT TEST: PASS
```

Documentation capture (poses the player, opens the menu, screenshots to
`/tmp/confessor_rite.png`, quits; note the screenshot/quit tasks are
scheduled *before* the conversation menu's dialogue freeze, which stops
the Python task clock ~20 gametics after the menu opens):

```bash
BD_EXAMPLE_SCREENSHOT=1 xvfb-run -a ./build/biaseddoom \
    -iwad ~/games/doom2.wad -file examples/python/30_conversation_quests \
    -python -nosound +map MAP01
```

## Note on reply driving

There is **no** `bd.*` function that picks a conversation reply, and menu
input cannot be injected from Python. The fixture therefore uses the
narrowest available real path: the `event convpick <node> <reply>`
console command (issued via `bd.execute`) reaching a UI-scope
`StaticEventHandler` that calls the native
`ConversationMenu.SendConversationReply`, the very function the menu
calls on ENTER, so the reply still travels through the genuine
`DEM_CONVREPLY` netcode and `HandleReply` commit point. If a future API
exposes reply selection directly, only `ConvoReplyBridge` needs to
change; every assertion stays valid.

## Expanding it

- **Re-voice or extend the tree:** edit the prose in `DIALOG01` /
  `LANGUAGE` (keep `log = "LOG77"` and the reply order, or update the
  constants in `content.py` and the autotest indices in `main.py`
  together). New pages need new `FIRST_NODE`-style constants.
- **New toasts / panel lines:** add strings to `content.py` and map them
  in `systems._toast_reply` / `RITE_LINES`; the UI picks them up
  automatically.
- **More light:** `PositionCandle` accepts any position provider: arm
  one per NPC, or switch `kind` to `"fluorescent"` for a harsher rite.
- **More quests:** `content.build_quest()` plus another
  `track_conversation_log(quest_id, objective_id, log_number)` line in
  `systems.setup_quests`. Give each new giver its own marker ids from a
  fresh block (this example owns 97000-97009; the bd.ui toolkit owns
  everything >= 900000).
