"""The Interrogation - systems: sessions, horror state, event wiring.

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
  Its ``choices()`` override adds two presentation layers: skill-check
  annotations gain the character's total bonus ("(DC 12 Persuasion,
  you +4)", computed with the bd_dnd helpers via :func:`skill_bonus`),
  and the active node's body gains a hint line when dread or attitude
  nears a hidden choice's threshold without crossing it.
- **Talk interaction.** Custom Action 1 (``+pyaction1``, auto-bound to Q
  at ``engine_start`` unless the player already bound it under Options ->
  Customize Controls, Custom Actions) fires the ``custom_action`` event ->
  :func:`talk_command`, which opens a session when the player is within
  ``TALK_RANGE`` of the Inquisitor. The ``talk`` console alias
  (``pyui talk`` -> ``ui_command``) routes into the same function, and the
  "no one near" feedback names the live binding.
- **Talk markers.** A gold "!" floats over the Inquisitor (display-list
  id ``MARKER_MARK_ID``) until the first conversation starts; after that
  a "[Q] Talk" label (id ``MARKER_TALK_ID``, live binding text) shows
  while the player is in ``TALK_RANGE`` with no session open. A 7-tic
  map-local task (:func:`refresh_markers`) owns the lifecycle and keeps
  :data:`marker_state` in step for the autotest; both markers keep the
  default ``occlude=True`` so they respect line of sight. A map-start
  toast names the binding and the annotation style.
- **Dusk ambience.** On ``map_load`` the horror state starts (dread tick +
  stalker windows + persistence), a slow candle
  :class:`PositionCandle` dresses the Inquisitor's (untagged) sector,
  resolved through ``bd.sector_at`` on his live position - and the
  ``StalkerDirector`` is enabled *while no session is active*. Talking to
  the Inquisitor holds the dark at bay: ``start_talk`` disables the
  director and the session's ``on_end`` re-enables it.

Manifest entry 2 of 4 (after content.py).
"""

import math

import biaseddoom as bd
import bd_dialogue
import bd_dnd
import bd_horror
import bd_quests
from bd_horror import toasts
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
    find the sector under him via ``bd.sector_at``. Everything else
    (original-light capture/restore, write-on-change, deterministic RNG)
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


def skill_bonus(character, skill):
    """The character's total bonus for a skill check (ability mod + prof).

    Computed with the bd_dnd helpers (``SKILLS`` for the governing
    ability, ``AbilityScores.mod`` for the modifier, and the character's
    ``proficiency``/``proficient_skills`` for the proficiency part), the
    same terms ``Character.skill_check`` rolls with. None when the
    character or skill is unknown.
    """
    try:
        if character is None:
            return None
        ability = bd_dnd.SKILLS.get(str(skill))
        if ability is None:
            return None
        bonus = int(character.abilities.mod(ability))
        if str(skill) in getattr(character, "proficient_skills", ()):
            bonus += int(character.proficiency)
        return bonus
    except Exception:
        return None


#: Node ids the hidden-choice hint lines can appear on (all other nodes,
#: notably the greeting node whose text its choice effect rewrites, are
#: left untouched).
_HINT_NODES = ("start", "smalltalk")


class InquisitionSession(bd_dialogue.DialogueSession):
    """A DialogueSession whose ctx carries the live dread level and attitude.

    ``context()`` gains four keys over the framework default: ``dread``
    (float, the HorrorState dread level at call time - re-read on every
    ``choices()`` call, so a threshold crossed mid-conversation reveals
    the mark choice on the next render), ``horror`` (the HorrorState,
    for effects that push dread around), ``attitude`` (int, the
    Inquisitor's live disposition toward the penitent),
    ``attitude_standing`` (its threshold word: hostile/cold/neutral/
    warm/trusted), and ``shift_attitude`` (callable delta -> new value,
    the hook the intimidation failure effect uses to erode him).

    ``choices()`` adds two presentation layers over the framework
    implementation:

    - skill-check annotations name the character's total bonus:
      "(DC 12 Persuasion, you +4)" instead of "(DC 12 Persuasion)", so
      the odds are knowable before committing;
    - :meth:`_refresh_hints` rewrites the active node's body with a
      discoverability hint when dread (smalltalk node) or attitude
      (start node) nears a hidden choice's threshold without crossing
      it, and restores the base text otherwise (and on :meth:`end`).
    """

    def __init__(self, *args, horror_state=None, **kwargs):
        super().__init__(*args, **kwargs)
        self.horror = horror_state
        #: Base body text per hinted node id, captured clean on first
        #: sight so hint lines compose and lift exactly.
        self._base_texts = {}

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

    def choices(self):
        """Visible choices, with node hints applied and check bonuses shown."""
        self._refresh_hints()
        entries = super().choices()
        return [(choice, enabled, self._bonus_annotation(choice, annotation))
                for choice, enabled, annotation in entries]

    def end(self, reason="manual"):
        """End the conversation; hint lines lift with it."""
        self._restore_hints()
        super().end(reason)

    def _bonus_annotation(self, choice, annotation):
        """Extend the framework's "(DC N Skill)" with ", you +bonus"."""
        if choice.skill_check is None or self.character is None:
            return annotation  # "(unavailable)" and plain rows pass through
        if not choice.show_failed:
            return annotation  # a hidden DC stays hidden
        skill, dc = choice.skill_check
        bonus = skill_bonus(self.character, skill)
        if bonus is None:
            return annotation
        label = skill.replace("_", " ").title()
        base = f"(DC {dc} {label})"
        extended = f"(DC {dc} {label}, you {bonus:+d})"
        if base in annotation:
            return annotation.replace(base, extended)
        return extended if not annotation else f"{annotation} {extended}"

    def _refresh_hints(self):
        """Compose/lift the hidden-choice hint lines on the hinted nodes.

        Runs on every ``choices()`` call (i.e. every render): the active
        node gets its hint when the threshold is near, every hinted node
        reverts to its captured base text otherwise. Inert once the
        session ends (``end()`` already restored the bases).
        """
        if not self.active:
            return
        for node_id in _HINT_NODES:
            node = self.dialogue.node(node_id)
            if node is None:
                continue
            base = self._base_texts.get(node_id)
            if base is None:
                base = self._base_texts[node_id] = node.text
            hint = self._hint_for(node_id) if node is self.active_node else None
            node.text = base if hint is None else f"{base}\n\n{hint}"

    def _hint_for(self, node_id):
        """The hint line a node should show right now (or None)."""
        if node_id == "smalltalk":
            dread = 0.0
            if self.horror is not None:
                try:
                    dread = float(self.horror.dread.level)
                except Exception:
                    dread = 0.0
            low = content.DREAD_MARK_THRESHOLD - content.DREAD_HINT_WINDOW
            if low <= dread < content.DREAD_MARK_THRESHOLD:
                return content.HINT_SMALLTALK_DREAD
        elif node_id == "start":
            if (content.ATTITUDE_GREETING_THRESHOLD < int(attitude)
                    <= content.ATTITUDE_HINT_THRESHOLD):
                return content.HINT_START_ATTITUDE
        return None

    def _restore_hints(self):
        """Revert every hinted node to its captured base text (idempotent)."""
        for node_id, base in self._base_texts.items():
            node = self.dialogue.node(node_id)
            if node is not None:
                node.text = base


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
    marker_state["talked"] = True  # first contact retires the "!" marker
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


# --- talk markers (display list) -------------------------------------------


#: Marker lifecycle, read by the autotest: "mark" (the gold "!" is
#: registered over the Inquisitor), "talk"/"talk_text" (the context
#: label and its live text), "talked" (the first conversation has
#: started, retiring the "!" for the rest of the map).
marker_state = {"mark": False, "talk": False, "talk_text": "",
                "talked": False}


def _safe_draw(func, *args, **kwargs):
    # Draw calls are display-list registrations: headless-safe no-ops,
    # but they raise RuntimeError while the world mutates (map unload).
    try:
        func(*args, **kwargs)
    except (RuntimeError, ValueError):
        pass


def _set_mark(show):
    """Register/clear the gold "!" over the Inquisitor (idempotent)."""
    if bool(show) == marker_state["mark"]:
        return
    marker_state["mark"] = bool(show)
    if show:
        _safe_draw(bd.draw_world_text, npc_ref, id=content.MARKER_MARK_ID,
                   text=content.MARKER_MARK_TEXT,
                   offset_z=content.MARKER_OFFSET_Z,
                   color=content.MARKER_MARK_COLOR,
                   height=content.MARKER_MARK_HEIGHT)
    else:
        _safe_draw(bd.draw_clear, content.MARKER_MARK_ID)


def _set_talk(show, text=""):
    """Register/clear the context talk label (idempotent); redraws when
    the text changes, so a live rebind shows within one refresh."""
    if show and marker_state["talk"] and marker_state["talk_text"] == text:
        return
    if not show and not marker_state["talk"]:
        return
    marker_state["talk"] = bool(show)
    marker_state["talk_text"] = text if show else ""
    if show:
        _safe_draw(bd.draw_world_text, npc_ref, id=content.MARKER_TALK_ID,
                   text=text, offset_z=content.MARKER_OFFSET_Z,
                   color=content.MARKER_TALK_COLOR,
                   height=content.MARKER_TALK_HEIGHT)
    else:
        _safe_draw(bd.draw_clear, content.MARKER_TALK_ID)


def reset_markers():
    """Fresh map: clear any stale registrations and the lifecycle state.

    World items vanish on map unload on their own; the draw_clear calls
    cover same-map resets, and marker_state is what the refresh task (and
    the autotest) reads.
    """
    marker_state["mark"] = False
    marker_state["talk"] = False
    marker_state["talk_text"] = ""
    marker_state["talked"] = False
    _safe_draw(bd.draw_clear, content.MARKER_MARK_ID)
    _safe_draw(bd.draw_clear, content.MARKER_TALK_ID)


def refresh_markers():
    """Slow repeating task (every MARKER_REFRESH_TICS tics, map-local):
    owns the Inquisitor's world-marker lifecycle.

    A gold "!" floats over him until the first conversation starts; after
    that, a "[Q] Talk" label (text from the live Custom Action 1 binding)
    shows while the player is within TALK_RANGE and no session is open.
    Both keep the default occlude=True, so line of sight applies, and
    both vanish with the map on their own; this task only keeps
    registration and marker_state in step.
    """
    npc = npc_ref
    try:
        npc_ok = npc is not None and npc.valid and npc.alive
    except Exception:
        npc_ok = False
    if not npc_ok:
        _set_mark(False)
        _set_talk(False)
        return True
    if not marker_state["talked"]:
        _set_mark(True)
        _set_talk(False)
        return True
    _set_mark(False)
    pawn = player_pawn()
    in_range = False
    if pawn is not None:
        try:
            in_range = pawn.distance_to(npc) <= content.TALK_RANGE
        except Exception:
            in_range = False
    if in_range and bd_dialogue.active_session() is None:
        _set_talk(True, text=content.TALK_LABEL_TEMPLATE.format(
            key=action_key_hint(1)))
    else:
        _set_talk(False)
    return True


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
    reset_markers()
    bd.schedule(refresh_markers, delay=content.MARKER_REFRESH_TICS,
                repeat=content.MARKER_REFRESH_TICS)  # map-local: dies on unload
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
    try:
        toasts.toast(content.INTRO_TOAST.format(key=action_key_hint(1)),
                     kind="omen")
    except Exception as exc:
        bd.warn(f"inquisition: intro toast failed: {exc!r}")

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
