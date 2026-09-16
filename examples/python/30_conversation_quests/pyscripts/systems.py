"""The Confessor - systems: event wiring, the quest rule, the booth candle.

Owns every ``bd.on`` handler that drives the fixture's mechanics:

- ``engine_start`` registers the quest and wires
  ``bd_quests.log.track_conversation_log`` so the accept reply's numeric
  ``log = "LOG77"`` completes its objective.
- ``conversation_started`` offers the quest (the classic "quest from an
  NPC" idiom) and whispers a toast.
- ``conversation_reply`` raises a ``bd_horror`` toast for the log update.
  The accept reply carries a *numeric* log, so the payload's ``log_string``
  is None and the ``$TXT_LOGTEXT77`` label cannot be resolved from Python:
  no localization lookup is exposed in the ``bd`` API (verified against
  ``docs/scripting/biaseddoom.pyi``). The toast therefore uses the fixed
  themed line from :mod:`confessor_content`; a hypothetical free-text
  ``log_string`` would be toasted verbatim instead. The accept also toasts
  the payout (a box of shells), and a decline toasts that the offer
  stands, since the conversation can simply be re-entered.
- ``map_load`` spawns the Confessor ahead of the player, arms a slow
  candle :class:`~bd_horror.atmosphere.LightProgram` over his booth
  sector, and raises the quest-giver marker: a gold "!" label plus a
  ground ring over the NPC, kept in sync with the quest state by a 7-tic
  map-local refresh task (see :data:`marker_state`). Stock light programs
  bind by sector *tag*, but the MAP01 start sector is untagged, so
  :class:`PositionCandle` (an example-local LightProgram subclass,
  public-API only) resolves its sector through ``bd.sector_at`` on the
  Confessor's live position. The example re-arms it from its own
  ``map_load``/``map_unload`` handlers instead of a LightManager.

Manifest entry 2 of 4; imports :mod:`confessor_content` through the
sys.modules alias content.py registers (with an ``import_script`` fallback
for standalone loading).
"""

import math

import biaseddoom as bd
import bd_quests
from bd_quests import Quest
from bd_horror import toasts
from bd_horror.atmosphere import LightProgram

try:
    import confessor_content as content
except ImportError:  # loaded outside the manifest (bd.import_script direct)
    content = bd.import_script("pyscripts/content.py",
                               module_name="confessor_content")

# --- module state ---------------------------------------------------------------

npc_ref = None
started_events = []
reply_events = []
last_rite_line = content.RITE_WAITING
candle = None

#: Quest-giver marker lifecycle, maintained by a 7-tic map-local task (see
#: ``_marker_sync``). The headless autotest reads this dict: ``shown`` tracks
#: whether the "!" label and ring are registered on the display list,
#: ``draws``/``clears`` count the display-list mutations, and ``task`` holds
#: the repeating task id (reset to None on map unload).
marker_state = {"task": None, "shown": False, "draws": 0, "clears": 0}


def player_pawn():
    """Live handle to the local player's pawn, or None."""
    try:
        player = bd.player(0)
        if player is None or not player.valid:
            return None
        pawn = player.actor
        return pawn if pawn is not None and pawn.valid else None
    except RuntimeError:
        return None


def pick_reply(node, reply):
    """Commit a conversation reply through the real netcode path.

    Queues the ``event convpick`` console command; the fixture's UI-scope
    ``ConvoReplyBridge`` StaticEventHandler turns it into the same
    ``ConversationMenu.SendConversationReply`` call the menu makes on ENTER,
    and ``HandleReply`` commits it on the next gametic.
    """
    bd.execute(f"event convpick {node} {reply}")


# --- the booth candle ------------------------------------------------------------


class PositionCandle(LightProgram):
    """A candle program bound to a live position instead of a sector tag.

    Stock ``LightProgram`` resolution goes through ``bd.sectors(tag=...)``
    and the booth sector on Doom II MAP01 has no tag (sector tags are
    read-only from Python), so this subclass overrides ``_resolve`` to find
    the sector under the Confessor via ``bd.sector_at``. Everything else
    (original-light capture/restore, write-on-change, deterministic RNG)
    is the shipped implementation. ``position_of`` is a callable returning
    ``(x, y)`` or None; when the Confessor's handle is stale the program
    simply resolves to no sectors and sits inert until the next arm.
    """

    def __init__(self, position_of, base=None, amplitude=24, period=9):
        super().__init__("candle", [0], {
            "base": base,
            "amplitude": max(0, int(amplitude)),
            "period": max(1, int(period)),
        })
        self._position_of = position_of

    def _resolve(self):
        try:
            pos = self._position_of()
        except Exception:
            return []
        if pos is None:
            return []
        try:
            sector = bd.sector_at(float(pos[0]), float(pos[1]))
        except Exception:
            return []
        return [sector] if sector is not None else []


def _npc_position():
    npc = npc_ref
    try:
        if npc is not None and npc.valid:
            return (npc.x, npc.y)
    except Exception:
        pass
    return None


def candle_active():
    """True when the booth candle is armed and its step task is running."""
    return candle is not None and candle._task is not None


# --- the quest-giver marker -------------------------------------------------------


def markers_wanted():
    """True while the Confessor's quest can still be accepted.

    The marker only stands down once the rite is sealed (COMPLETED) or the
    quest is FAILED: a decline leaves it burning, because the offer stands.
    """
    quest = bd_quests.log.get(content.QUEST_ID)
    if quest is None:
        return False
    return quest.state not in (Quest.COMPLETED, Quest.FAILED)


def _markers_show(npc):
    try:
        bd.draw_world_text(npc, id=content.MARKER_TEXT_ID,
                           text=content.MARKER_TEXT, offset_z=12.0,
                           color=content.MARKER_COLOR,
                           height=content.MARKER_TEXT_HEIGHT,
                           outline=True)
        bd.draw_world_ring(npc, id=content.MARKER_RING_ID,
                           radius=content.MARKER_RING_RADIUS,
                           color=content.MARKER_COLOR, alpha=0.9,
                           offset_z=2.0, segments=24)
    except Exception as exc:
        bd.warn(f"confessor: quest-giver marker draw failed: {exc!r}")
        return
    marker_state["shown"] = True
    marker_state["draws"] += 1


def _markers_clear():
    try:
        bd.draw_clear(content.MARKER_TEXT_ID)
        bd.draw_clear(content.MARKER_RING_ID)
    except Exception as exc:
        bd.warn(f"confessor: quest-giver marker clear failed: {exc!r}")
    marker_state["shown"] = False
    marker_state["clears"] += 1


def _marker_sync():
    """Reconcile the marker with the quest state (7-tic map-local task).

    Also invoked directly on map load (so the marker appears without
    waiting a refresh) and from the quest's ``on_complete`` hook (so it
    stands down the tic the rite is sealed).
    """
    npc = npc_ref
    try:
        live = npc is not None and npc.valid
    except Exception:
        live = False
    if live and markers_wanted():
        _markers_show(npc)
    elif marker_state["shown"]:
        _markers_clear()
    return True


# --- event wiring -----------------------------------------------------------------


@bd.on("engine_start")
def setup_quests(event):
    bd.imgui.set_master_visible(True)  # the Rite panel + toasts render
    quest = content.build_quest()
    # Stand the quest-giver marker down the very tic the rite is sealed
    # (the 7-tic refresh task below is the general maintainer; this hook
    # just makes the clear immediate).
    quest.on_complete = lambda q: _marker_sync()
    bd_quests.log.add(quest)
    # When a conversation_reply with log_number 77 commits, the quest
    # auto-starts if needed and the objective completes.
    bd_quests.log.track_conversation_log(content.QUEST_ID,
                                         content.OBJECTIVE_ID,
                                         content.LOG_NUMBER)


@bd.on("conversation_started")
def on_conversation_started(event):
    global last_rite_line
    started_events.append(event)
    # The classic "quest from dialogue" idiom: talking to the NPC offers
    # the quest. (track_conversation_log would also auto-start it on the
    # reply.)
    quest = bd_quests.log.get(content.QUEST_ID)
    if quest is not None and quest.state == Quest.INACTIVE:
        quest.start()
    last_rite_line = content.RITE_WAITING
    toasts.toast(content.TOAST_STARTED, kind="omen")


def _toast_reply(event):
    """Raise the toasts for a committed reply: the log update and payout on
    accept, the standing offer on a decline."""
    global last_rite_line
    log_string = event.get("log_string")
    node = event.get("node")
    reply_index = event.get("reply_index")
    if event.get("log_number") == content.LOG_NUMBER:
        # Numeric log: the payload carries no text and $LABELs cannot be
        # resolved from Python (see the module docstring), so toast the
        # fixed themed line, then name the payout so it is visible.
        toasts.toast(content.TOAST_ACCEPT, kind="quest")
        toasts.toast(content.TOAST_REWARD, kind="loot")
    elif log_string:
        # Free-text log (not produced by this fixture): toast it verbatim
        # unless it is an unresolvable $LABEL.
        if str(log_string).startswith("$"):
            toasts.toast(content.TOAST_ACCEPT, kind="quest")
        else:
            toasts.toast(str(log_string), kind="quest")
    else:
        toasts.toast(content.TOAST_DECLINE, kind="info")
        # A decline never fails the quest: the marker stays lit and the
        # conversation can simply be re-entered. Say so out loud.
        if (node, reply_index) == (content.FIRST_NODE, content.REPLY_DECLINE):
            toasts.toast(content.TOAST_OFFER_STANDS, kind="quest")
    last_rite_line = content.RITE_LINES.get((node, reply_index),
                                            content.RITE_WAITING)


@bd.on("conversation_reply")
def on_conversation_reply(event):
    reply_events.append(event)
    _toast_reply(event)


@bd.on("map_load")
def on_map(event):
    global npc_ref, candle
    pawn = player_pawn()
    if pawn is None:
        return

    angle = math.radians(pawn.angle)
    npc_ref = bd.spawn(content.NPC_CLASS, pawn.x + math.cos(angle) * 128.0,
                       pawn.y + math.sin(angle) * 128.0, pawn.z,
                       angle=(pawn.angle + 180.0) % 360.0, force=True)
    try:
        npc_ref.tint = (125, 95, 100)  # ash and vestment-purple
    except Exception:
        pass
    toasts.toast(content.SPAWN_TOAST, kind="info", sound=False)

    # Dress the booth: a slow candle flame over the Confessor's sector.
    candle = PositionCandle(_npc_position, base=content.CANDLE_BASE,
                            amplitude=content.CANDLE_AMPLITUDE,
                            period=content.CANDLE_PERIOD)
    candle.arm(fresh=True)

    # Raise the quest-giver marker (the gold "!" and ring) at once, then
    # keep it in sync with the quest state on a slow map-local refresh
    # task; the engine cancels map-local tasks on unload.
    _marker_sync()
    if marker_state["task"] is None:
        marker_state["task"] = bd.schedule(
            _marker_sync, delay=content.MARKER_REFRESH_TICS,
            repeat=content.MARKER_REFRESH_TICS, map_local=True)


@bd.on("map_unload")
def on_map_unload(event):
    # World mutation is blocked during map_unload: disarm without
    # restoring (the map is going away).
    if candle is not None:
        candle.disarm(restore=False)
    # The marker's display-list items vanish with the map on their own and
    # the engine cancels the map-local refresh task; just reset the
    # bookkeeping so the next map starts clean.
    marker_state["task"] = None
    marker_state["shown"] = False


def quest_state_text():
    """Human-readable quest state for the Rite panel."""
    quest = bd_quests.log.get(content.QUEST_ID)
    if quest is None:
        return "unwritten"
    return quest.state


def quest_inactive():
    """True while the quest has not been started (the offer is fresh)."""
    quest = bd_quests.log.get(content.QUEST_ID)
    return quest is not None and quest.state == Quest.INACTIVE


# --- sibling-import registration ---------------------------------------------------

import sys as _sys
import types as _types


class _LiveAlias(_types.ModuleType):
    """sys.modules alias that reads/writes through to the module's live
    globals (the engine registers manifest modules under a mangled name
    only after execution, so the real module object cannot be aliased
    from inside itself)."""

    def __init__(self, name, namespace):
        super().__init__(name)
        object.__setattr__(self, "_bd_live_ns", namespace)

    def __getattr__(self, key):
        try:
            return object.__getattribute__(self, "_bd_live_ns")[key]
        except KeyError:
            raise AttributeError(key)

    def __setattr__(self, key, value):
        object.__getattribute__(self, "_bd_live_ns")[key] = value


_sys.modules.setdefault("confessor_systems",
                        _LiveAlias("confessor_systems", globals()))
del _sys, _types
