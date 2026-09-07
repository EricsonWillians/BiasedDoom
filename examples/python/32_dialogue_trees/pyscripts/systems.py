"""The Interrogation — systems: sessions, horror state, event wiring.

Owns the rules and every engine hook of the fixture:

- **Session wrapper.** :class:`InquisitionSession` subclasses
  ``bd_dialogue.DialogueSession`` and extends the condition/effect context
  with ``dread`` (the live ``bd_horror.HorrorState.dread`` level) and
  ``horror`` (the state itself), which is what the hidden mark choice and
  the intimidation dread bump read. It also carries the example-local
  Inquisitor attitude: ``attitude`` (int, -100..100), its ``attitude_standing``
  word (same thresholds as ``bd_npcs``, reimplemented locally so this
  example never imports the pack just for the mapping), and a
  ``shift_attitude`` callable for effects. Composition over framework edits.
- **Talk interaction.** Custom Action 1 (``+pyaction1``, auto-bound to Q
  at ``engine_start`` unless the player already bound it under Options ->
  Customize Controls, Custom Actions) fires the ``custom_action`` event ->
  :func:`talk_command`, which opens a session when the player is within
  ``TALK_RANGE`` of the Inquisitor. The ``talk`` console alias
  (``pyui talk`` -> ``ui_command``) routes into the same function, and the
  "no one near" feedback names the live binding.
- **Dusk ambience.** On ``map_load`` the horror state starts (dread tick +
  stalker windows + persistence), a slow candle
  :class:`PositionCandle` dresses the Inquisitor's (untagged) sector —
  resolved through ``bd.sector_at`` on his live position — and the
  ``StalkerDirector`` is enabled *while no session is active*. Talking to
  the Inquisitor holds the dark at bay: ``start_talk`` disables the
  director and the session's ``on_end`` re-enables it.

Manifest entry 2 of 4 (after content.py).
"""

import math

import biaseddoom as bd
import bd_dialogue
import bd_horror
import bd_quests
from bd_horror.atmosphere import LightProgram

try:
    import inquisition_content as content
except ImportError:  # loaded outside the manifest (bd.import_script direct)
    content = bd.import_script("pyscripts/content.py",
                               module_name="inquisition_content")

# --- module state ---------------------------------------------------------------

factions = None
character = None
dialogue = None
horror = bd_horror.HorrorState()
npc_ref = None
crate_pos = None
session = None
candle = None

# --- the Inquisitor's attitude (example-local disposition) ------------------------
#
# A plain int in [-100, 100] in module state: 0 (neutral) at first contact,
# -20 whenever an intimidation attempt fails (content.ATTITUDE_DROP), never
# allowed below -100. Persisted under its own bd.state key through the
# engine's save/load events; the threshold words deliberately mirror
# bd_npcs.disposition.DEFAULT_THRESHOLDS without importing the pack.

ATTITUDE_STATE_KEY = "inquisition_attitude"
ATTITUDE_MIN = -100
ATTITUDE_MAX = 100
ATTITUDE_START = 0

_ATTITUDE_STANDINGS = ("hostile", "cold", "neutral", "warm", "trusted")
#: (lower bound, standing) pairs; the LAST bound a value meets wins.
_ATTITUDE_THRESHOLDS = ((-100, "hostile"), (-50, "cold"), (-10, "neutral"),
                        (40, "warm"), (75, "trusted"))

attitude = ATTITUDE_START


def attitude_standing(value=None):
    """The standing word for an attitude value (default: the live one)."""
    if value is None:
        value = attitude
    try:
        value = int(value)
    except (TypeError, ValueError):
        value = ATTITUDE_START
    result = _ATTITUDE_STANDINGS[0]
    for bound, standing in _ATTITUDE_THRESHOLDS:
        if value >= bound:
            result = standing
    return result


def shift_attitude(delta):
    """Shift the Inquisitor's attitude, clamped; returns the new value."""
    global attitude
    attitude = max(ATTITUDE_MIN, min(ATTITUDE_MAX, attitude + int(delta)))
    return attitude


@bd.on("save")
def persist_attitude_save(event):
    try:
        bd.state[ATTITUDE_STATE_KEY] = int(attitude)
    except Exception as exc:
        bd.warn(f"inquisition: could not save the attitude: {exc!r}")


@bd.on("load")
def persist_attitude_load(event):
    global attitude
    try:
        attitude = max(ATTITUDE_MIN,
                       min(ATTITUDE_MAX,
                           int(bd.state.get(ATTITUDE_STATE_KEY,
                                            ATTITUDE_START))))
    except (TypeError, ValueError) as exc:
        bd.warn(f"inquisition: could not restore the attitude: {exc!r}")


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


# --- the dusk candle ----------------------------------------------------------------


class PositionCandle(LightProgram):
    """A candle program bound to a live position instead of a sector tag.

    Stock ``LightProgram`` resolution goes through ``bd.sectors(tag=...)``
    and the Inquisitor's sector on Doom II MAP01 has no tag (sector tags
    are read-only from Python), so this subclass overrides ``_resolve`` to
    find the sector under him via ``bd.sector_at``. Everything else —
    original-light capture/restore, write-on-change, deterministic RNG —
    is the shipped implementation. When his handle is stale the program
    resolves to no sectors and sits inert until the next arm.
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
    """True when the dusk candle is armed and its step task is running."""
    return candle is not None and candle._task is not None


# --- the session wrapper ------------------------------------------------------------


class InquisitionSession(bd_dialogue.DialogueSession):
    """A DialogueSession whose ctx carries the live dread level and attitude.

    ``context()`` gains four keys over the framework default: ``dread``
    (float, the HorrorState dread level at call time — re-read on every
    ``choices()`` call, so a threshold crossed mid-conversation reveals
    the mark choice on the next render), ``horror`` (the HorrorState,
    for effects that push dread around), ``attitude`` (int, the
    Inquisitor's live disposition toward the penitent),
    ``attitude_standing`` (its threshold word: hostile/cold/neutral/
    warm/trusted), and ``shift_attitude`` (callable delta -> new value,
    the hook the intimidation failure effect uses to erode him).
    """

    def __init__(self, *args, horror_state=None, **kwargs):
        super().__init__(*args, **kwargs)
        self.horror = horror_state

    def context(self):
        ctx = super().context()
        level = 0.0
        if self.horror is not None:
            try:
                level = float(self.horror.dread.level)
            except Exception:
                level = 0.0
        ctx["dread"] = level
        ctx["horror"] = self.horror
        ctx["attitude"] = int(attitude)
        ctx["attitude_standing"] = attitude_standing(attitude)
        ctx["shift_attitude"] = shift_attitude
        return ctx


def _on_session_ended(ended_session):
    """The conversation is over: the dark resumes its hunt."""
    horror.stalker.enabled = True


def start_talk():
    """Open a session with the Inquisitor when close enough."""
    global session
    if bd_dialogue.active_session() is not None:
        return None
    pawn = player_pawn()
    npc = npc_ref
    if pawn is None or npc is None or dialogue is None:
        return None
    try:
        if not npc.valid or pawn.distance_to(npc) > content.TALK_RANGE:
            return None
    except Exception:
        return None
    new_session = InquisitionSession(
        dialogue, npc, character=character, factions=factions,
        quest_log=bd_quests.log, faction=content.FACTION,
        horror_state=horror)
    if not new_session.start():
        return None
    new_session.on_end.append(_on_session_ended)
    horror.stalker.enabled = False  # his candle holds the dark back
    session = new_session
    return new_session


def current_session():
    """The example's live session (or the just-ended one), for the UI."""
    return session


# --- custom actions -------------------------------------------------------------------


#: Every custom_action payload the example saw, as (action, pressed) pairs.
#: The autotest asserts on the exact transitions.
action_log = []


def ensure_custom_action_binding(n, default_key):
    """Bind ``default_key`` to ``+pyactionN`` when the player has not.

    Custom Actions are ordinary engine buttons, so a binding set through
    Options -> Customize Controls, Custom Actions always wins; this only
    fills in a stock-unbound default so the example is playable out of the
    box. Safe from ``engine_start`` (console commands queue pre-map).
    """
    try:
        if bd.input_binding(f"+pyaction{n}") is None:
            bd.execute(f"bind {default_key} +pyaction{n}")
    except Exception as exc:
        bd.warn(f"inquisition: could not bind +pyaction{n}: {exc!r}")


def action_key_hint(n):
    """The live display name of the ``+pyactionN`` binding (or a fallback)."""
    try:
        name = bd.input_binding(f"+pyaction{n}")
    except Exception:
        name = None
    return name or f"Custom Action {n} (bind in Customize Controls)"


def talk_command():
    """The talk interaction shared by the console alias and Custom Action 1."""
    if bd_dialogue.active_session() is not None:
        return
    if start_talk() is None:
        bd.center_message(f"[{action_key_hint(1)}] {content.NO_ONE_NEAR}")


@bd.on("custom_action")
def on_custom_action(event):
    try:
        action = int(event.get("action") or 0)
        pressed = bool(event.get("pressed"))
        action_log.append((action, pressed))
        if action == 1 and pressed:
            talk_command()
    except Exception as exc:
        bd.warn(f"inquisition: custom action failed: {exc!r}")


# --- event wiring -------------------------------------------------------------------


@bd.on("engine_start")
def setup_example(event):
    global factions, character, dialogue
    bd.imgui.set_master_visible(True)
    bd.log(f"bd_dialogue shipped from: {bd_dialogue.__file__}")

    factions = content.build_factions()
    character = content.build_character()
    dialogue = content.build_dialogue()

    bd_quests.log.add(content.build_quest(factions))
    bd_quests.log.track_pickup(content.QUEST_ID, "fetch_reliquary",
                               content.CRATE_CLASS, 1)

    # Console alias through the pyui/ui_command bridge; the key itself lives
    # on Custom Action 1 (auto-bound below, rebindable in Customize Controls
    # -> Custom Actions).
    bd.execute('alias talk "pyui talk"')
    ensure_custom_action_binding(1, "q")


@bd.on("ui_command")
def on_ui_command(event):
    if event.get("command") != "talk":
        return
    talk_command()


@bd.on("map_load")
def on_map(event):
    global npc_ref, crate_pos, candle
    horror.start()              # dread tick + stalker windows
    horror.arm_persistence()    # save/load round-trip via bd.state
    if event.get("from_savegame"):
        return
    pawn = player_pawn()
    if pawn is None:
        return
    heading = math.radians(pawn.angle)
    dx, dy = math.cos(heading), math.sin(heading)
    try:
        npc_ref = bd.spawn(content.NPC_CLASS, pawn.x + content.NPC_OFFSET * dx,
                           pawn.y + content.NPC_OFFSET * dy, pawn.z,
                           angle=(pawn.angle + 180.0) % 360.0,
                           tid=content.NPC_TID, force=True)
        npc_ref.set_flag("FRIENDLY", True)
        npc_ref.set_flag("STANDSTILL", True)  # no wandering out of range
        npc_ref.speed = 0.0
        npc_ref.tint = content.NPC_TINT
    except Exception as exc:
        bd.warn(f"inquisition spawn NPC failed: {exc!r}")
    crate_pos = (pawn.x + content.CRATE_OFFSET * dx,
                 pawn.y + content.CRATE_OFFSET * dy, pawn.z)
    try:
        bd.spawn(content.CRATE_CLASS, *crate_pos, force=True)
    except Exception as exc:
        bd.warn(f"inquisition spawn crate failed: {exc!r}")

    # Dusk ambience: a candle over the Inquisitor's sector, and the stalker
    # director hunting while no confession is in progress.
    candle = PositionCandle(_npc_position, base=content.CANDLE_BASE,
                            amplitude=content.CANDLE_AMPLITUDE,
                            period=content.CANDLE_PERIOD)
    candle.arm(fresh=True)
    horror.stalker.enabled = bd_dialogue.active_session() is None


@bd.on("map_unload")
def on_map_unload(event):
    # Mutation is blocked during map_unload: disarm without restoring.
    if candle is not None:
        candle.disarm(restore=False)


# --- sibling-import registration ----------------------------------------------------

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


_sys.modules.setdefault("inquisition_systems",
                        _LiveAlias("inquisition_systems", globals()))
del _sys, _types
