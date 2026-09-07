"""The Confessor — systems: event wiring, the quest rule, the booth candle.

Owns every ``bd.on`` handler that drives the fixture's mechanics:

- ``engine_start`` registers the quest and wires
  ``bd_quests.log.track_conversation_log`` so the accept reply's numeric
  ``log = "LOG77"`` completes its objective.
- ``conversation_started`` offers the quest (the classic "quest from an
  NPC" idiom) and whispers a toast.
- ``conversation_reply`` raises a ``bd_horror`` toast for the log update.
  The accept reply carries a *numeric* log, so the payload's ``log_string``
  is None and the ``$TXT_LOGTEXT77`` label cannot be resolved from Python —
  no localization lookup is exposed in the ``bd`` API (verified against
  ``docs/scripting/biaseddoom.pyi``). The toast therefore uses the fixed
  themed line from :mod:`confessor_content`; a hypothetical free-text
  ``log_string`` would be toasted verbatim instead.
- ``map_load`` spawns the Confessor ahead of the player and arms a slow
  candle :class:`~bd_horror.atmosphere.LightProgram` over his booth sector.
  Stock light programs bind by sector *tag*, but the MAP01 start sector is
  untagged, so :class:`PositionCandle` (an example-local LightProgram
  subclass, public-API only) resolves its sector through ``bd.sector_at``
  on the Confessor's live position. The example re-arms it from its own
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
    the sector under the Confessor via ``bd.sector_at``. Everything else —
    original-light capture/restore, write-on-change, deterministic RNG —
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


# --- event wiring -----------------------------------------------------------------


@bd.on("engine_start")
def setup_quests(event):
    bd.imgui.set_master_visible(True)  # the Rite panel + toasts render
    bd_quests.log.add(content.build_quest())
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
    """Raise the log-update toast for a committed reply."""
    global last_rite_line
    log_string = event.get("log_string")
    if event.get("log_number") == content.LOG_NUMBER:
        # Numeric log: the payload carries no text and $LABELs cannot be
        # resolved from Python (see the module docstring) — toast the fixed
        # themed line.
        toasts.toast(content.TOAST_ACCEPT, kind="quest")
    elif log_string:
        # Free-text log (not produced by this fixture): toast it verbatim
        # unless it is an unresolvable $LABEL.
        if str(log_string).startswith("$"):
            toasts.toast(content.TOAST_ACCEPT, kind="quest")
        else:
            toasts.toast(str(log_string), kind="quest")
    else:
        toasts.toast(content.TOAST_DECLINE, kind="info")
    last_rite_line = content.RITE_LINES.get(
        (event.get("node"), event.get("reply_index")), content.RITE_WAITING)


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


@bd.on("map_unload")
def on_map_unload(event):
    # World mutation is blocked during map_unload: disarm without
    # restoring (the map is going away).
    if candle is not None:
        candle.disarm(restore=False)


def quest_state_text():
    """Human-readable quest state for the Rite panel."""
    quest = bd_quests.log.get(content.QUEST_ID)
    if quest is None:
        return "unwritten"
    return quest.state


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
