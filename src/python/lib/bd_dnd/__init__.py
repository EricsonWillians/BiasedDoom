"""D&D-inspired d20 RPG framework for BiasedDoom Python mods.

``bd_dnd`` is an engine-shipped, dependency-free package (it only needs
``biaseddoom``) that gives mods a tabletop-d20-style rules layer —
generic, evocative mechanics, no licensed assets:

- **Dice.** :func:`roll` parses classic ``NdM+K`` notation (``"2d6+3"``,
  ``"1 d20 - 1"``, bare ``"d20"``, case-insensitive) and
  :func:`d20` performs the signature d20 roll with advantage /
  disadvantage and natural-20 / natural-1 criticals. Every die flows
  through the engine's deterministic, savegame-serialized script RNG.
- **Abilities and skills.** The six classic ability scores with 5e-style
  modifiers (:func:`modifier`), a 5e skill-to-ability table
  (:data:`SKILLS`), and level-based proficiency
  (:func:`proficiency_bonus`).
- **Characters.** :class:`Character` owns ability scores, level/XP with
  the 5e threshold table (:attr:`Character.XP_TABLE`, moddable),
  skill checks / ability checks / saving throws with advantage support,
  hit points with hit-die level-ups, per-rest resources (rage slots and
  the like), short/long rests, and an ``on_level_up`` hook list.
- **Classes and creation.** ``bd_dnd.classes`` (lazily re-exported)
  adds class-based progression: :class:`CharacterClass` describes a
  class (hit die, primary abilities, save proficiencies, class skills,
  per-level feature dicts with optional ``apply`` callables, per-rest
  resources, starting equipment) and :func:`bind_class` attaches one to
  a :class:`Character`, applying level-1 features and resources now and
  later levels through the ``on_level_up`` hook (levels in
  :data:`CLASS_LEVELS_ASI` queue points on
  ``character.pending_asi``). :class:`CreationWizard` is a pure,
  engine-free creation model (standard array, point buy, or rolled
  scores, plus class-skill picks) with a validating ``finish()``.
  Use-based mastery (:func:`track_skill_use`, :func:`advancement_check`,
  :func:`skill_bonus`) grows a flat skill bonus the more a skill is
  exercised. Class definitions are never persisted; only the
  ``class_id`` name and the use/mastery counters ride along in
  :class:`CharacterState` saves.
- **Kill XP.** :func:`track_xp_from_kills` turns ``actor_died`` events
  into experience awards using a Doom-monster XP table
  (:data:`DEFAULT_XP_TABLE`, moddable), with exact player-credit
  attribution by default (``player_index``).
- **Parties.** :class:`Party` groups several characters with an active
  member, shared/solo XP awards, and per-member state;
  :class:`PartyState` persists the whole roster through ``bd.state``.
- **Companions.** :class:`Companion` (``bd_dnd.companions``, lazily
  re-exported) binds a party member to a follower actor in the world:
  the actor shadows the player (follow / teleport catch-up on a
  scheduled loop), fights the player's recent attackers through its
  native monster AI, and its actor health *is* the member's RPG HP
  (two-way synced). Companion descriptors ride along in
  :class:`PartyState` saves and re-bind by TID after a checkpoint load.
- **World integration.** :class:`LockedDoorCheck` gates a locked map
  door behind an Athletics bash / Sleight-of-Hand pick check and opens
  it natively on success; :class:`TrapZone` turns tagged sectors into
  saving-throw traps with half damage on a save;
  :class:`DamageSaveRule` rolls a saving throw against incoming damage
  and refunds part of it as a retroactive heal;
  :class:`DialogueSkillGate` gates a Strife conversation reply behind a
  skill check.
- **Persistence.** :class:`CharacterState` owns a :class:`Character` and
  mirrors ``bd_vtm``: ``save()``/``load()`` round-trip through
  ``bd.state["bd_dnd"]`` on the engine's ``save``/``load`` events.
  :class:`PartyState` does the same for a whole :class:`Party` under
  ``bd.state["bd_dnd_party"]`` — use one or the other per character.
  Class bindings and skill masteries persist as plain ``class_id`` /
  ``skill_uses`` / ``masteries`` keys next to the character (see the
  Classes bullet).
- **UI.** ``bd_dnd.sheet.CharacterSheet`` renders a character (stats,
  HP/XP bars, roll log) as a Dear ImGui window from an ``imgui_frame``
  handler, and ``bd_dnd.sheet.PartySheet`` adds a selectable roster
  column for a :class:`Party`. ``bd_dnd.sheet.bind_sheet_toggle`` wires
  a console alias and an optional key bind (through the engine's
  ``pyui`` command and the ``ui_command`` event) to flip visibility.

Minimal usage::

    import biaseddoom as bd
    import bd_dnd

    hero = bd_dnd.Character("Crawler",
                            bd_dnd.AbilityScores(str=16, con=14))
    state = bd_dnd.CharacterState(hero)
    door = bd_dnd.LockedDoorCheck(111, hero, mode="str", dc=15,
                                  key_class="RedCard")

    @bd.on("map_load")
    def begin(event):
        state.arm_persistence()   # save/load round-trip via bd.state

Determinism
-----------

All randomness flows through the engine's deterministic script RNG
(``bd.randint`` via the ``rng=`` parameters, defaulting to the ``bd``
module itself). The script RNG stream position is serialized into
savegames, so checks resume exactly after a checkpoint load. For
isolated unit-style tests every roll accepts any object exposing
``randint(lo, hi)`` (or ``int(lo, hi)``, matching ``bd.rng()`` streams)
— a scripted test double works.

Engine-honesty notes
--------------------

- ``Character.hp`` is the RPG layer's authoritative hit-point pool.
  Engine actor health remains the engine's; syncing the two (via
  ``Actor.heal``/``Actor.damage``) is a mod-side choice. The built-in
  world helpers damage the *actor* (native gameplay health) because that
  is what kills the player pawn; they leave ``Character.hp`` alone.
- :func:`track_xp_from_kills` defaults to **exact kill credit**:
  ``actor_died`` carries ``attacker_ref`` / ``attacker_class`` /
  ``attacker_player_index`` (the ``Die`` source: the shooter for missile
  kills, the attacker for hitscan/melee, the bomb owner for explosions,
  ``None`` for environmental deaths and source-less scripted damage), so
  only kills by the pawn of ``player_index`` award XP. Pass
  ``player_index=None`` for the legacy policy where any matching death
  counts (infighting and crushers included).
- :class:`DamageSaveRule` cannot truly *prevent* damage — the
  ``actor_damaged`` event fires after the hit landed — so a successful
  save refunds hit points through ``Actor.heal``. Overkill therefore
  still kills; the rule skips actors that already died. See its
  docstring before building balance on it.
- :class:`LockedDoorCheck` opens a door through the native
  ``Line.activate`` path. Engine lock checks run *inside* the door
  special and reject a null activator, so for key-locked doors the check
  temporarily grants ``key_class`` to the activator for the single
  activation and reclaims it immediately afterwards (verified against
  Doom II MAP02's red door).
- :class:`Companion` combat is real but AI-bound: the engine's Actor
  ``target``/``master``/``tracer`` getsets are writable, so the
  companion sets its ``target`` to the player's most recent attacker
  (tracked from ``actor_damaged`` on the player pawn) and nudges the
  native chase AI with ``set_state("See")``; from there the actor
  class's own A_Chase / missile logic does the fighting. There is no
  scripted per-frame combat brain — accuracy, reaction time, and target
  retention belong to the monster class. The companion is flagged
  ``FRIENDLY`` (and ``COUNTKILL`` cleared) so it neither hunts players
  nor inflames the kill count.
- For a companion-bound member the two health pools *are* linked
  (``actor_damaged`` on the companion actor lowers ``Character.hp``;
  ``Character.set_hp``/rest/level-up heal or wound the actor through
  the new ``on_hp_changed`` hook). The boundary is the same as above:
  :class:`DamageSaveRule`-style refunds heal the *actor* only, and a
  plain ``Actor.heal`` on the companion does not raise ``Character.hp``
  (heals fire no damage event); keep healing on the RPG side
  (``set_hp``/``rest``) so both pools move together.
"""

from __future__ import annotations

import re
from typing import Any, Callable, Dict, List, Optional, Set

import biaseddoom as bd

__all__ = [
    "roll", "d20", "ABILITIES", "modifier", "AbilityScores", "SKILLS",
    "proficiency_bonus", "Character", "track_xp_from_kills",
    "DEFAULT_XP_TABLE", "LockedDoorCheck", "TrapZone", "DamageSaveRule",
    "DialogueSkillGate", "Party", "CharacterState", "PartyState",
    "Companion", "CharacterClass", "CLASS_LEVELS_ASI", "bind_class",
    "apply_class_level", "CreationWizard", "track_skill_use", "mastery",
    "advancement_check", "skill_bonus",
    "STATE_KEY", "PARTY_STATE_KEY", "__version__",
]

__version__ = "1.2.0"

#: Key under which CharacterState persists itself in ``bd.state``.
STATE_KEY = "bd_dnd"

#: Key under which PartyState persists itself in ``bd.state``.
PARTY_STATE_KEY = "bd_dnd_party"

# Builtin ``int`` captured for classes whose __init__ shadows it with the
# tabletop ability name ``int``.
_INT = int


# --- helpers -----------------------------------------------------------------


def _roll(rng: Any, lo: int, hi: int) -> int:
    """Inclusive ``lo..hi`` roll through the engine's deterministic RNG.

    ``rng`` may be the ``biaseddoom`` module itself (``bd.randint``), a
    ``bd.rng()`` stream (``.int``), or any test double exposing either.
    """
    roller = getattr(rng, "randint", None)
    if roller is None:
        roller = getattr(rng, "int", None)
    if roller is None:
        raise TypeError("rng must provide randint(lo, hi) or int(lo, hi)")
    return _INT(roller(lo, hi))


def _level_time() -> int:
    """Current map time in tics, or 0 when no level is loaded."""
    try:
        return bd.level_time()
    except Exception:
        return 0


def _fire(callbacks: List[Callable], *args: Any) -> None:
    """Invoke every registered callback; a buggy hook only warns."""
    for callback in list(callbacks):
        try:
            callback(*args)
        except Exception as exc:
            bd.warn(f"bd_dnd: callback {callback!r} raised: {exc!r}")


def _center_message(message: str) -> None:
    """Center-screen message that degrades to a log line when off-map."""
    try:
        bd.center_message(message)
    except Exception:
        bd.log(f"bd_dnd: {message}")


def _is_player_actor(ref: Any) -> bool:
    """True when the Actor handle is a (valid) player pawn."""
    try:
        return ref is not None and ref.valid and ref.is_player
    except Exception:
        return False


# --- dice ----------------------------------------------------------------------

#: "2d6+3", "1 d20 - 1", bare "d20"; case-insensitive, spaces optional.
_DICE_RE = re.compile(
    r"^\s*(\d*)\s*[dD]\s*(\d+)\s*(?:([+-])\s*(\d+))?\s*$")

#: Sane upper bound on dice rolled per call (guards the RNG stream and
#: the event budget against absurd notations like "1000000d6").
MAX_DICE_PER_ROLL: int = 1000


def roll(notation: str, rng: Any = bd) -> Dict[str, Any]:
    """Roll classic dice notation; returns the full breakdown.

    Accepts ``NdM``, ``NdM+K`` / ``NdM-K``, with optional spaces and a
    default count of 1 (``"d20"`` == ``"1d20"``); case-insensitive.
    Raises ``ValueError`` on malformed input. The result dict::

        {"notation": "2d6+3", "rolls": [4, 2], "modifier": 3, "total": 9}
    """
    text = str(notation)
    match = _DICE_RE.match(text)
    if not match:
        raise ValueError(f"malformed dice notation: {text!r}")
    count = _INT(match.group(1)) if match.group(1) else 1
    sides = _INT(match.group(2))
    if count < 1 or sides < 1:
        raise ValueError(f"dice count and sides must be >= 1: {text!r}")
    if count > MAX_DICE_PER_ROLL:
        raise ValueError(
            f"too many dice (max {MAX_DICE_PER_ROLL}): {text!r}")
    sign = -1 if match.group(3) == "-" else 1
    modifier = sign * _INT(match.group(4)) if match.group(4) else 0
    rolls = [_roll(rng, 1, sides) for _ in range(count)]
    return {"notation": text.strip(), "rolls": rolls,
            "modifier": modifier, "total": sum(rolls) + modifier}


def d20(mod: int = 0, advantage: bool = False, disadvantage: bool = False,
        rng: Any = bd) -> Dict[str, Any]:
    """The signature d20 roll, with advantage/disadvantage and criticals.

    With ``advantage`` (or ``disadvantage``) two dice are rolled and the
    higher (lower) is kept; when both are set they cancel out into a
    straight roll. The result dict::

        {"roll": 14, "kept": 14, "total": 17, "critical": None}

    advantage/disadvantage adds a ``"discarded"`` entry with the die that
    was thrown away. ``critical`` is ``"hit"`` on a natural 20,
    ``"miss"`` on a natural 1, ``None`` otherwise. ``roll`` equals
    ``kept`` (the natural die face); ``total`` adds ``mod``.
    """
    mod = _INT(mod)
    discarded: Optional[int] = None
    if advantage and not disadvantage:
        a, b = _roll(rng, 1, 20), _roll(rng, 1, 20)
        kept, discarded = max(a, b), min(a, b)
    elif disadvantage and not advantage:
        a, b = _roll(rng, 1, 20), _roll(rng, 1, 20)
        kept, discarded = min(a, b), max(a, b)
    else:  # straight roll (both set cancel out, per 5e)
        kept = _roll(rng, 1, 20)
    critical = "hit" if kept == 20 else ("miss" if kept == 1 else None)
    result: Dict[str, Any] = {"roll": kept, "kept": kept,
                              "total": kept + mod, "critical": critical}
    if discarded is not None:
        result["discarded"] = discarded
    return result


# --- abilities and skills --------------------------------------------------------

#: The six ability scores, in classic sheet order.
ABILITIES = ("str", "dex", "con", "int", "wis", "cha")


def modifier(score: int) -> int:
    """5e ability modifier: ``(score - 10) // 2``, floored.

    Python's ``//`` floors toward negative infinity, which is exactly the
    tabletop rule for odd and low scores: 9 -> -1, 1 -> -5 (a truncating
    C-style division would give 9 -> 0 and 1 -> -4 — a classic porting
    bug this helper avoids on purpose).
    """
    return (_INT(score) - 10) // 2


class AbilityScores:
    """The six ability scores; ``mod(name)`` gives the 5e modifier.

    Parameters use the tabletop names (``str=16, dex=12, ...``); each is
    an attribute of the same name, so mods can adjust scores directly.
    """

    def __init__(self, str: int = 10, dex: int = 10, con: int = 10,  # noqa: A002
                 int: int = 10, wis: int = 10, cha: int = 10) -> None:  # noqa: A002
        self.str: int = _INT(str)
        self.dex: int = _INT(dex)
        self.con: int = _INT(con)
        self.int: int = _INT(int)
        self.wis: int = _INT(wis)
        self.cha: int = _INT(cha)

    def mod(self, name: str) -> int:
        """5e modifier for the named ability (``"str"`` ... ``"cha"``)."""
        return modifier(getattr(self, str(name)))

    def score(self, name: str) -> int:
        """Raw score for the named ability."""
        return _INT(getattr(self, str(name)))

    def serialize(self) -> Dict[str, int]:
        """JSON-safe snapshot in :data:`ABILITIES` order."""
        return {name: self.score(name) for name in ABILITIES}

    def restore(self, data: Any) -> None:
        """Tolerant restore of a :meth:`serialize` snapshot."""
        if not isinstance(data, dict):
            return
        for name in ABILITIES:
            try:
                setattr(self, name, _INT(data.get(name, self.score(name))))
            except (TypeError, ValueError):
                continue

    def __repr__(self) -> str:  # pragma: no cover - debugging aid
        inner = " ".join(f"{n}={self.score(n)}" for n in ABILITIES)
        return f"<AbilityScores {inner}>"


#: 5e skill -> governing ability. Plain dict — mods may edit or extend it.
SKILLS: Dict[str, str] = {
    "athletics": "str",
    "acrobatics": "dex", "sleight_of_hand": "dex", "stealth": "dex",
    "arcana": "int", "history": "int", "investigation": "int",
    "nature": "int", "religion": "int",
    "animal_handling": "wis", "insight": "wis", "medicine": "wis",
    "perception": "wis", "survival": "wis",
    "deception": "cha", "intimidation": "cha", "performance": "cha",
    "persuasion": "cha",
}


def proficiency_bonus(level: int) -> int:
    """5e proficiency bonus: 2 at levels 1-4, +1 every four levels."""
    return 2 + max(0, _INT(level) - 1) // 4


# --- characters ------------------------------------------------------------------


class Character:
    """A d20 character: abilities, level/XP, checks, HP, resources.

    ``hp``/``max_hp`` are the RPG layer's authoritative hit-point pool —
    see the module docstring for the split with engine actor health.
    ``hit_die`` (default d10, fighter-style) sizes both the level-1 pool
    (``hit_die + con mod``) and level-up gains (``hit_die average +
    con mod``, minimum 1; the average of a d10 is 6).

    :attr:`resources` / :attr:`resource_max` are mod-managed per-rest
    pools (rage charges, second wind, ...): :meth:`grant_resource` seeds
    a pool, :meth:`use_resource` spends one charge, and
    :meth:`restore_resources` refills every pool to its max (a long rest
    does this automatically).

    Every check appends a compact entry to :attr:`roll_log` (capped at
    :attr:`ROLL_LOG_LIMIT`), which ``bd_dnd.sheet.CharacterSheet``
    renders. Level-ups fire the ``on_level_up`` callback list with
    ``(character, new_level)``. Hit-point changes made through
    :meth:`set_hp`, :meth:`rest`, and :meth:`level_up` fire the
    ``on_hp_changed`` callback list with ``(character)`` — the hook
    :class:`Companion` uses to keep the follower actor's health in sync.
    Direct ``character.hp = ...`` assignments bypass the hook; prefer
    :meth:`set_hp` when world sync matters.
    """

    #: 5e XP thresholds, indexed by level - 1 (XP_TABLE[1] = 300 is the
    #: level-2 threshold). Plain list — mods may edit it.
    XP_TABLE: List[int] = [
        0, 300, 900, 2700, 6500, 14000, 23000, 34000, 48000, 64000,
        85000, 100000, 120000, 140000, 165000, 195000, 225000, 265000,
        305000, 355000,
    ]
    #: Level cap; 20 in the tabletop game.
    MAX_LEVEL: int = 20
    #: Roll-log ring size.
    ROLL_LOG_LIMIT: int = 60

    def __init__(self, name: str,
                 abilities: Optional[AbilityScores] = None,
                 level: int = 1, xp: int = 0, hit_die: int = 10,
                 max_hp: Optional[int] = None, hp: Optional[int] = None,
                 proficient_skills: Any = None,
                 proficient_saves: Any = None) -> None:
        self.name: str = str(name)
        self.abilities: AbilityScores = (abilities if abilities is not None
                                         else AbilityScores())
        self.level: int = max(1, _INT(level))
        self.xp: int = max(0, _INT(xp))
        self.hit_die: int = max(2, _INT(hit_die))
        con_mod = self.abilities.mod("con")
        self.max_hp: int = (_INT(max_hp) if max_hp is not None
                            else max(1, self.hit_die + con_mod))
        self.hp: int = (_INT(hp) if hp is not None else self.max_hp)
        self.hp = max(0, min(self.max_hp, self.hp))
        self.proficient_skills: Set[str] = {
            str(s) for s in (proficient_skills or ())}
        self.proficient_saves: Set[str] = {
            str(s) for s in (proficient_saves or ())}
        self.resources: Dict[str, int] = {}
        self.resource_max: Dict[str, int] = {}
        self.on_level_up: List[Callable[["Character", int], None]] = []
        self.on_hp_changed: List[Callable[["Character"], None]] = []
        self.roll_log: List[Dict[str, Any]] = []

    # -- derived stats ---------------------------------------------------------

    @property
    def proficiency(self) -> int:
        """Proficiency bonus for the current level (grows automatically)."""
        return proficiency_bonus(self.level)

    @property
    def hit_die_average(self) -> int:
        """Average rounded up, tabletop-style: d10 -> 6, d8 -> 5."""
        return self.hit_die // 2 + 1

    def xp_for_next_level(self) -> Optional[int]:
        """XP threshold of the next level, or None at the level cap."""
        if self.level >= self.MAX_LEVEL or self.level >= len(self.XP_TABLE):
            return None
        return self.XP_TABLE[self.level]

    # -- checks ------------------------------------------------------------------

    def _log_roll(self, label: str, total: int, dc: Optional[int] = None,
                  success: Optional[bool] = None,
                  critical: Optional[str] = None, detail: str = "") -> None:
        self.roll_log.append({
            "label": str(label), "total": int(total),
            "dc": dc if dc is None else int(dc),
            "success": success, "critical": critical,
            "detail": str(detail),
        })
        if len(self.roll_log) > self.ROLL_LOG_LIMIT:
            del self.roll_log[: len(self.roll_log) - self.ROLL_LOG_LIMIT]

    def skill_check(self, skill: str, dc: int, advantage: bool = False,
                    disadvantage: bool = False, rng: Any = bd
                    ) -> Dict[str, Any]:
        """Skill check: d20 + ability mod + proficiency vs. ``dc``.

        Proficiency applies when ``skill`` is in ``proficient_skills``.
        Raises ``ValueError`` for a skill missing from :data:`SKILLS`.
        Returns the rich result dict (see the source for the full field
        list); it is also appended to :attr:`roll_log`.
        """
        skill = str(skill)
        ability = SKILLS.get(skill)
        if ability is None:
            raise ValueError(f"unknown skill: {skill!r}")
        ability_mod = self.abilities.mod(ability)
        prof = self.proficiency if skill in self.proficient_skills else 0
        roll_dict = d20(mod=ability_mod + prof, advantage=advantage,
                        disadvantage=disadvantage, rng=rng)
        total = roll_dict["total"]
        success = total >= int(dc)
        discarded = roll_dict.get("discarded")
        detail = f"d20({roll_dict['kept']}" + \
                 (f", dropped {discarded}" if discarded is not None else "") + \
                 f"){ability_mod:+d}{prof:+d}"
        result = {
            "kind": "skill", "skill": skill, "ability": ability,
            "roll": roll_dict, "ability_mod": ability_mod, "prof": prof,
            "total": total, "dc": int(dc), "success": success,
            "critical": roll_dict["critical"],
        }
        self._log_roll(f"{skill} check", total, dc=int(dc), success=success,
                       critical=roll_dict["critical"], detail=detail)
        return result

    def ability_check(self, ability: str, dc: int, advantage: bool = False,
                      disadvantage: bool = False, rng: Any = bd
                      ) -> Dict[str, Any]:
        """Raw ability check: d20 + ability mod vs. ``dc`` (no proficiency)."""
        ability = str(ability)
        if ability not in ABILITIES:
            raise ValueError(f"unknown ability: {ability!r}")
        ability_mod = self.abilities.mod(ability)
        roll_dict = d20(mod=ability_mod, advantage=advantage,
                        disadvantage=disadvantage, rng=rng)
        total = roll_dict["total"]
        success = total >= int(dc)
        result = {
            "kind": "ability", "skill": None, "ability": ability,
            "roll": roll_dict, "ability_mod": ability_mod, "prof": 0,
            "total": total, "dc": int(dc), "success": success,
            "critical": roll_dict["critical"],
        }
        self._log_roll(f"{ability} check", total, dc=int(dc), success=success,
                       critical=roll_dict["critical"],
                       detail=f"d20({roll_dict['kept']}){ability_mod:+d}")
        return result

    def saving_throw(self, ability: str, dc: int, advantage: bool = False,
                     disadvantage: bool = False, rng: Any = bd
                     ) -> Dict[str, Any]:
        """Saving throw: like :meth:`ability_check`, plus proficiency when
        ``ability`` is in ``proficient_saves``."""
        ability = str(ability)
        if ability not in ABILITIES:
            raise ValueError(f"unknown ability: {ability!r}")
        ability_mod = self.abilities.mod(ability)
        prof = self.proficiency if ability in self.proficient_saves else 0
        roll_dict = d20(mod=ability_mod + prof, advantage=advantage,
                        disadvantage=disadvantage, rng=rng)
        total = roll_dict["total"]
        success = total >= int(dc)
        result = {
            "kind": "save", "skill": None, "ability": ability,
            "roll": roll_dict, "ability_mod": ability_mod, "prof": prof,
            "total": total, "dc": int(dc), "success": success,
            "critical": roll_dict["critical"],
        }
        self._log_roll(f"{ability} save", total, dc=int(dc), success=success,
                       critical=roll_dict["critical"],
                       detail=f"d20({roll_dict['kept']}){ability_mod:+d}"
                              f"{prof:+d}")
        return result

    # -- XP and levels -----------------------------------------------------------

    def award_xp(self, amount: int) -> List[Dict[str, Any]]:
        """Award XP; returns one event dict per level gained.

        Each event is ``{"level": new_level, "max_hp": new_max_hp,
        "hp_gained": gain}``. Level-ups fire ``on_level_up``.
        """
        amount = _INT(amount)
        if amount <= 0:
            return []
        self.xp += amount
        events: List[Dict[str, Any]] = []
        while (self.level < self.MAX_LEVEL
               and self.level < len(self.XP_TABLE)
               and self.xp >= self.XP_TABLE[self.level]):
            gain = self.level_up()
            events.append({"level": self.level, "max_hp": self.max_hp,
                           "hp_gained": gain})
        return events

    def level_up(self) -> int:
        """Advance one level; returns the HP gain.

        ``max_hp`` grows by ``hit_die_average + con mod`` (minimum 1) and
        current ``hp`` heals by the same amount (the classic "you feel
        stronger" bump). Fires ``on_level_up(character, new_level)``.
        """
        gain = max(1, self.hit_die_average + self.abilities.mod("con"))
        self.level = min(self.MAX_LEVEL, self.level + 1)
        self.max_hp += gain
        self.set_hp(self.hp + gain)
        _fire(self.on_level_up, self, self.level)
        return gain

    # -- hit points ---------------------------------------------------------------

    def set_hp(self, value: int) -> int:
        """Set ``hp`` clamped to ``[0, max_hp]``; returns the new value.

        Fires ``on_hp_changed(character)`` when the value actually
        changed — the sync point :class:`Companion` binds to. Direct
        ``hp`` attribute writes bypass the hook, so prefer this helper
        whenever a companion (or any other observer) tracks the pool.
        :meth:`rest` and :meth:`level_up` flow through here too;
        :class:`DamageSaveRule` refunds do not — they heal the *actor*,
        never this pool.
        """
        new_hp = max(0, min(self.max_hp, _INT(value)))
        if new_hp != self.hp:
            self.hp = new_hp
            _fire(self.on_hp_changed, self)
        return self.hp

    # -- rests and resources -------------------------------------------------------

    def grant_resource(self, name: str, charges: int) -> None:
        """Seed a per-rest resource pool with ``charges`` charges."""
        charges = max(0, _INT(charges))
        self.resource_max[str(name)] = charges
        self.resources[str(name)] = charges

    def use_resource(self, name: str) -> bool:
        """Spend one charge of a resource; False when it is exhausted."""
        name = str(name)
        if self.resources.get(name, 0) <= 0:
            return False
        self.resources[name] -= 1
        return True

    def restore_resources(self) -> None:
        """Refill every resource pool to its max (part of a long rest)."""
        self.resources = dict(self.resource_max)

    def rest(self, short: bool = True, rng: Any = bd) -> Dict[str, Any]:
        """Take a rest; returns ``{"kind", "healed"}``.

        Short rest: recover 25% of max HP (at least 1), capped at
        ``max_hp`` — a deliberately simple stand-in for spending hit
        dice. Long rest: full heal and :meth:`restore_resources`.
        Healing flows through :meth:`set_hp`, so ``on_hp_changed``
        fires.
        """
        before = self.hp
        if short:
            self.set_hp(self.hp + max(1, self.max_hp // 4))
        else:
            self.set_hp(self.max_hp)
            self.restore_resources()
        return {"kind": "short" if short else "long",
                "healed": self.hp - before}

    # -- persistence -----------------------------------------------------------------

    def serialize(self) -> Dict[str, Any]:
        """Plain JSON-able snapshot (no Actor handles, no callables)."""
        return {
            "version": 1,
            "name": self.name,
            "level": self.level,
            "xp": self.xp,
            "hit_die": self.hit_die,
            "max_hp": self.max_hp,
            "hp": self.hp,
            "abilities": self.abilities.serialize(),
            "proficient_skills": sorted(self.proficient_skills),
            "proficient_saves": sorted(self.proficient_saves),
            "resources": {k: int(v) for k, v in self.resources.items()},
            "resource_max": {k: int(v) for k, v in self.resource_max.items()},
        }

    def restore(self, data: Any) -> None:
        """Restore from a :meth:`serialize` snapshot; tolerant of junk."""
        if not isinstance(data, dict):
            return
        try:
            self.name = str(data.get("name", self.name))
            self.level = max(1, _INT(data.get("level", self.level)))
            self.xp = max(0, _INT(data.get("xp", self.xp)))
            self.hit_die = max(2, _INT(data.get("hit_die", self.hit_die)))
            self.max_hp = max(1, _INT(data.get("max_hp", self.max_hp)))
            self.hp = max(0, min(self.max_hp,
                                 _INT(data.get("hp", self.hp))))
        except (TypeError, ValueError):
            pass
        self.abilities.restore(data.get("abilities"))
        skills = data.get("proficient_skills")
        if isinstance(skills, (list, tuple, set)):
            self.proficient_skills = {str(s) for s in skills}
        saves = data.get("proficient_saves")
        if isinstance(saves, (list, tuple, set)):
            self.proficient_saves = {str(s) for s in saves}
        for target, source in ((self.resources, data.get("resources")),
                               (self.resource_max, data.get("resource_max"))):
            if not isinstance(source, dict):
                continue
            target.clear()
            for key, value in source.items():
                try:
                    target[str(key)] = _INT(value)
                except (TypeError, ValueError):
                    continue

    def __repr__(self) -> str:  # pragma: no cover - debugging aid
        return (f"<Character {self.name!r} level {self.level} "
                f"hp {self.hp}/{self.max_hp} xp {self.xp}>")


# --- XP from kills -----------------------------------------------------------------

#: Default monster XP values (5e-flavored challenge rewards for Doom's
#: roster). Keys are matched case-insensitively against the actor's
#: class name. Plain dict — mods may edit it or pass their own table to
#: :func:`track_xp_from_kills`.
DEFAULT_XP_TABLE: Dict[str, int] = {
    "ZombieMan": 25,
    "ShotgunGuy": 50,
    "DoomImp": 50,
    "Demon": 100,
    "Cacodemon": 200,
    "HellKnight": 400,
    "BaronOfHell": 1100,
}


def track_xp_from_kills(character: Character, xp_table: Any = None,
                        announce: bool = True, *,
                        player_index: Optional[int] = 0) -> Callable:
    """Award ``character`` XP whenever a tabled monster dies.

    Wires the engine's ``actor_died`` event: a death of a monster whose
    class name appears (case-insensitively) in ``xp_table`` (default
    :data:`DEFAULT_XP_TABLE`) awards its XP, firing level-ups as usual.
    Returns the registered handler for testability.

    Kill credit is **exact by default**: only deaths whose ``actor_died``
    attacker is the pawn of ``player_index`` (default 0, the local
    player) count — missile kills credit the shooter, hitscan/melee the
    attacker, explosions the bomb owner; environmental deaths and
    scripted ``Actor.damage`` calls without a source credit nobody. This
    is a deliberate breaking change from the original release, which
    counted any matching death because the engine exposed no killer
    field; pass ``player_index=None`` to restore that approximate
    any-death policy (infighting and crushers then award XP too).
    """
    source = DEFAULT_XP_TABLE if xp_table is None else xp_table
    lookup = {str(name).lower(): int(xp) for name, xp in dict(source).items()}

    @bd.on("actor_died")
    def _on_actor_died(event: Dict[str, Any]) -> None:
        if player_index is not None:
            try:
                attacker_index = event.get("attacker_player_index")
            except Exception:
                attacker_index = None
            if attacker_index is None or int(attacker_index) != player_index:
                return
        class_name: Optional[str] = None
        ref = event.get("actor_ref")
        try:
            if ref is not None and ref.valid:
                class_name = str(ref.class_name)
        except Exception:
            class_name = None
        if class_name is None:
            snapshot = event.get("actor")
            if isinstance(snapshot, dict):
                raw = snapshot.get("class_name")
                class_name = str(raw) if raw else None
        if not class_name:
            return
        xp = lookup.get(class_name.lower())
        if not xp:
            return
        level_events = character.award_xp(xp)
        if announce:
            try:
                bd.hud_text(f"+{xp} XP ({class_name})", id=90001, y=0.22,
                            color="gold", hold=1.5, fade=0.5)
            except Exception:
                pass
            for entry in level_events:
                _center_message(
                    f"{character.name} reaches level {entry['level']}!")

    return _on_actor_died


# --- world integration -------------------------------------------------------------


class LockedDoorCheck:
    """Gate a locked map door behind an Athletics bash or lockpick check.

    Wires ``line_activation_failed``, filtered to ``line_index`` with
    ``reason == "locked"`` and a player activator. When the player uses
    the door, an ability check fires: ``mode="str"`` runs an
    ``athletics`` (STR) bash, ``mode="dex"`` a ``sleight_of_hand`` (DEX)
    pick — both are skill checks, so proficiency applies. On success the
    door opens through the native ``Line.activate`` path (see below) and
    a triumph message displays; on failure, "The door holds fast."

    **Opening locked doors natively.** The engine runs the lock check
    *inside* the door special, and it rejects a null activator
    (``P_CheckKeys(NULL)`` fails). So for key-locked doors pass
    ``key_class`` (e.g. ``"RedCard"`` for Doom lock 129, the red door):
    the check grants the key to the activator, performs one native
    ``line.activate(activator, clear=True)``, and immediately reclaims
    the key. Without ``key_class`` the check tries
    ``line.activate(None, clear=True)``, which only works for
    lock-scripted specials that accept a null activator; a refusal only
    warns.

    ``once=True`` disarms the check after the first *successful* open
    (failures may be retried). Because the engine offers no event
    unregistration, "disarmed" means the handler becomes a no-op.

    :meth:`attempt` is the check body, callable directly with a
    scripted ``rng=`` for deterministic tests.
    """

    #: mode -> (skill, flavor verb).
    MODES = {"str": ("athletics", "bash"), "dex": ("sleight_of_hand", "pick")}

    def __init__(self, line_index: int, character: Character,
                 mode: str = "str", dc: int = 15, once: bool = False,
                 key_class: Optional[str] = None) -> None:
        mode = str(mode).lower()
        if mode not in self.MODES:
            raise ValueError(f"mode must be one of {sorted(self.MODES)}")
        self.line_index: int = int(line_index)
        self.character: Character = character
        self.mode: str = mode
        self.dc: int = int(dc)
        self.once: bool = bool(once)
        self.key_class: Optional[str] = (str(key_class) if key_class else None)
        self.armed: bool = True
        self.attempts: int = 0
        self.opened: bool = False
        self.last_result: Optional[Dict[str, Any]] = None

        @bd.on("line_activation_failed")
        def _handler(event: Dict[str, Any]) -> None:
            self._on_activation_failed(event)

        self._handler = _handler

    def disarm(self) -> None:
        """Permanently disarm the check (the handler becomes a no-op)."""
        self.armed = False

    def _on_activation_failed(self, event: Dict[str, Any]
                              ) -> Optional[Dict[str, Any]]:
        if not self.armed:
            return None
        if event.get("reason") != "locked":
            return None
        if event.get("line_index") != self.line_index:
            return None
        activator = event.get("actor_ref")
        if not _is_player_actor(activator):
            return None
        return self.attempt(activator)

    def attempt(self, activator: Any = None, rng: Any = bd
                ) -> Dict[str, Any]:
        """Run the bash/pick check now; opens the door on success."""
        skill, verb = self.MODES[self.mode]
        result = self.character.skill_check(skill, self.dc, rng=rng)
        self.attempts += 1
        self.last_result = result
        if result["success"]:
            opened = self._open_door(activator)
            self.opened = self.opened or opened
            if opened:
                _center_message(
                    f"{self.character.name} {verb}s the door open! "
                    f"({skill} {result['total']} vs DC {self.dc})")
            if self.once:
                self.disarm()
        else:
            _center_message("The door holds fast.")
        return result

    def _open_door(self, activator: Any) -> bool:
        """Native door activation; see the class docstring for key_class."""
        try:
            line = bd.line(self.line_index)
        except Exception as exc:
            bd.warn(f"bd_dnd: cannot resolve line {self.line_index}: {exc!r}")
            return False
        result = 0
        if self.key_class is not None and activator is not None:
            granted = False
            try:
                activator.give_inventory(self.key_class, 1)
                granted = True
                result = line.activate(activator, clear=True)
            except Exception as exc:
                bd.warn(f"bd_dnd: door activation failed: {exc!r}")
            finally:
                if granted:
                    try:
                        activator.take_inventory(self.key_class, 1)
                    except Exception:
                        pass
        else:
            try:
                result = line.activate(None, clear=True)
            except Exception as exc:
                bd.warn(f"bd_dnd: door activation failed: {exc!r}")
        if not result:
            bd.warn(f"bd_dnd: line {self.line_index} refused to activate "
                    f"(lock-bearing specials reject a null activator — "
                    f"pass key_class=, e.g. 'RedCard' for Doom lock 129)")
            return False
        return True


class TrapZone:
    """Saving-throw trap bound to one or more sector tags.

    Wires ``sector_entered``: when a *player* enters a sector carrying
    any of ``sector_tags``, the character makes a ``save_ability`` save
    (default DEX) against ``dc``. Failure applies ``roll(damage)`` to
    the victim's actor through the native damage path; success halves it
    (minimum 1). The trap does not touch ``Character.hp`` — see the
    module docstring for the engine/RPG health split.

    ``once=True`` (default) springs the trap a single time per sector;
    with ``once=False`` a per-sector ``cooldown_tics`` gate stops it
    re-firing every tic while the player stands inside. Because the
    engine offers no event unregistration, :meth:`disarm` turns the
    handler into a no-op.

    :meth:`spring` is the trap body, callable directly with a scripted
    ``rng=`` for deterministic tests.
    """

    DEFAULT_COOLDOWN_TICS: int = 70

    def __init__(self, sector_tags: Any, character: Character, dc: int = 13,
                 damage: str = "2d6", save_ability: str = "dex",
                 once: bool = True,
                 cooldown_tics: Optional[int] = None) -> None:
        save_ability = str(save_ability).lower()
        if save_ability not in ABILITIES:
            raise ValueError(f"save_ability must be one of {ABILITIES}")
        if isinstance(sector_tags, (list, tuple, set)):
            self.sector_tags: Set[int] = {int(t) for t in sector_tags}
        else:
            self.sector_tags = {int(sector_tags)}
        # Validate the damage notation eagerly (rolls nothing).
        probe = _DICE_RE.match(str(damage))
        if not probe:
            raise ValueError(f"malformed damage notation: {damage!r}")
        self.character: Character = character
        self.dc: int = int(dc)
        self.damage: str = str(damage)
        self.save_ability: str = save_ability
        self.once: bool = bool(once)
        self.cooldown_tics: int = (int(cooldown_tics) if cooldown_tics
                                   else self.DEFAULT_COOLDOWN_TICS)
        self.armed: bool = True
        self.springs: int = 0
        self.last_outcome: Optional[Dict[str, Any]] = None
        self._sprung: Set[int] = set()
        self._last_fired: Dict[int, int] = {}

        @bd.on("sector_entered")
        def _handler(event: Dict[str, Any]) -> None:
            self._on_sector_entered(event)

        self._handler = _handler

    def disarm(self) -> None:
        """Permanently disarm the trap (the handler becomes a no-op)."""
        self.armed = False

    def _on_sector_entered(self, event: Dict[str, Any]
                           ) -> Optional[Dict[str, Any]]:
        if not self.armed:
            return None
        try:
            if int(event.get("player_index", -1)) < 0:
                return None
        except (TypeError, ValueError):
            return None
        tags = event.get("tags") or []
        try:
            tag_set = {int(t) for t in tags}
        except (TypeError, ValueError):
            return None
        if not self.sector_tags.intersection(tag_set):
            return None
        victim = event.get("actor_ref")
        if not _is_player_actor(victim):
            return None
        sector = event.get("sector")
        try:
            key = int(sector)
        except (TypeError, ValueError):
            key = -1
        if self.once and key in self._sprung:
            return None
        now = _level_time()
        if now - self._last_fired.get(key, -1 << 30) < self.cooldown_tics:
            return None
        return self.spring(victim, sector_index=key)

    def spring(self, victim: Any, sector_index: int = -1, rng: Any = bd
               ) -> Dict[str, Any]:
        """Spring the trap on ``victim`` now; returns the outcome dict."""
        save = self.character.saving_throw(self.save_ability, self.dc,
                                           rng=rng)
        rolled = max(0, roll(self.damage, rng=rng)["total"])
        applied = rolled if not save["success"] else max(1, rolled // 2)
        dealt = 0
        if victim is not None and applied > 0:
            try:
                dealt = int(victim.damage(applied))
            except Exception as exc:
                bd.warn(f"bd_dnd: trap damage failed: {exc!r}")
        self.springs += 1
        self._sprung.add(sector_index)
        self._last_fired[sector_index] = _level_time()
        if save["success"]:
            _center_message("You dodge the worst of the trap!")
        else:
            _center_message("The trap catches you!")
        outcome = {"save": save, "damage_rolled": rolled,
                   "damage_applied": applied, "damage_dealt": dealt}
        self.last_outcome = outcome
        return outcome


class DamageSaveRule:
    """Saving throw against incoming damage, resolved as a retroactive heal.

    Wires ``actor_damaged`` filtered to the pawn of ``player_index`` (0 =
    the local player; Python gameplay mutation is single-player only, so
    any live player pawn matches once ``bd.player(player_index)``
    resolves). Whenever matching damage lands — ``damage_type`` given:
    exact, case-insensitive match on the event's ``damage_type`` (e.g.
    ``"Fire"``); ``None``: all damage — the character rolls an
    ``ability`` saving throw against ``dc``:

    - natural 20 with ``negate_on_critical=True`` (default): the hit is
      fully negated — the whole ``damage`` is healed back;
    - success with ``half_on_success=True`` (default): the hit is halved
      like the tabletop "save for half" rule — the kept share is
      ``max(1, damage // 2)``, so the refund is
      ``damage - max(1, damage // 2)``;
    - failure: nothing happens.

    **Engine-honesty note: engine damage cannot be cancelled after the
    fact.** ``actor_damaged`` fires from inside the native damage path
    once the hit has already been applied, so the save *refunds* hit
    points through ``Actor.heal`` instead of preventing the hit. Two
    consequences:

    - **Overkill still kills.** If the hit dropped the actor to 0 the
      death already happened (``actor_died`` fired, death states
      entered); refunding HP cannot undo it, so the rule *skips* actors
      that already died (reported as ``{"skipped": "dead"}`` in
      :attr:`last_result`).
    - The refund is a heal: it is clamped by the actor's normal maximum
      health and does not touch ``Character.hp`` (see the module
      docstring for the engine/RPG health split).

    ``cooldown_tics`` (default 0) gates re-rolls by map time, so a
    multi-hit melt (several fireballs in the same second) cannot burn
    the dice log or heal-lock the player — hits inside the cooldown are
    skipped with ``{"skipped": "cooldown"}``. Because the engine offers
    no event unregistration, :meth:`disarm` turns the handler into a
    no-op.

    :meth:`apply` is the rule body, callable directly with a scripted
    ``rng=`` for deterministic tests (same pattern as
    :meth:`TrapZone.spring`). :attr:`last_result` holds the most recent
    outcome dict::

        {"save": <save result or None>, "damage": int,
         "damage_type": str | None, "refunded": int, "negated": bool,
         "skipped": None | "cooldown" | "dead"}
    """

    def __init__(self, character: Character, dc: int, ability: str = "dex",
                 damage_type: Optional[str] = None,
                 half_on_success: bool = True,
                 negate_on_critical: bool = True,
                 cooldown_tics: int = 0, player_index: int = 0) -> None:
        ability = str(ability).lower()
        if ability not in ABILITIES:
            raise ValueError(f"ability must be one of {ABILITIES}")
        self.character: Character = character
        self.dc: int = int(dc)
        self.ability: str = ability
        self.damage_type: Optional[str] = (None if damage_type is None
                                           else str(damage_type).lower())
        self.half_on_success: bool = bool(half_on_success)
        self.negate_on_critical: bool = bool(negate_on_critical)
        self.cooldown_tics: int = max(0, int(cooldown_tics))
        self.player_index: int = int(player_index)
        self.armed: bool = True
        self.last_result: Optional[Dict[str, Any]] = None
        self._last_fired: int = -(1 << 30)

        @bd.on("actor_damaged")
        def _handler(event: Dict[str, Any]) -> None:
            self._on_actor_damaged(event)

        self._handler = _handler

    def disarm(self) -> None:
        """Permanently disarm the rule (the handler becomes a no-op)."""
        self.armed = False

    def _is_tracked_player(self, ref: Any) -> bool:
        """True when ref is the pawn of the tracked player index."""
        if not _is_player_actor(ref):
            return False
        try:
            player = bd.player(self.player_index)
            if player is None:
                return False
            pawn = player.actor
            if pawn is not None and pawn.valid:
                # Handles are fresh objects per call: compare TIDs when
                # present, else accept any player pawn (single-player).
                if pawn.tid != 0 and ref.tid != 0:
                    return pawn.tid == ref.tid
                return True
        except Exception:
            pass
        return True  # off-map / synthetic tests: the pawn check is enough

    def _on_actor_damaged(self, event: Dict[str, Any]
                          ) -> Optional[Dict[str, Any]]:
        if not self.armed:
            return None
        try:
            damage = int(event.get("damage") or 0)
        except (TypeError, ValueError):
            return None
        if damage <= 0:
            return None
        if self.damage_type is not None:
            event_type = str(event.get("damage_type") or "").lower()
            if event_type != self.damage_type:
                return None
        victim = event.get("actor_ref")
        if not self._is_tracked_player(victim):
            return None
        return self.apply(victim, damage,
                          damage_type=event.get("damage_type"))

    def apply(self, victim: Any, damage: int, damage_type: Any = None,
              rng: Any = bd) -> Dict[str, Any]:
        """Run the save against a hit that already landed; heals the refund."""
        damage = max(0, _INT(damage))
        result: Dict[str, Any] = {"save": None, "damage": damage,
                                  "damage_type": damage_type,
                                  "refunded": 0, "negated": False,
                                  "skipped": None}
        now = _level_time()
        if now - self._last_fired < self.cooldown_tics:
            result["skipped"] = "cooldown"
            self.last_result = result
            return result
        try:
            alive = victim is not None and victim.valid and victim.alive
        except Exception:
            alive = False
        if not alive:
            result["skipped"] = "dead"  # overkill: a refund cannot undo death
            self.last_result = result
            return result
        self._last_fired = now
        save = self.character.saving_throw(self.ability, self.dc, rng=rng)
        result["save"] = save
        refund = 0
        if save["critical"] == "hit" and self.negate_on_critical:
            refund = damage
            result["negated"] = True
        elif save["success"] and self.half_on_success:
            refund = damage - max(1, damage // 2)
        if refund > 0:
            try:
                victim.heal(refund)
            except Exception as exc:
                bd.warn(f"bd_dnd: damage-save heal failed: {exc!r}")
                refund = 0
        result["refunded"] = refund
        if result["negated"]:
            _center_message(
                f"{self.character.name} shrugs off the hit! (natural 20)")
        elif refund > 0:
            _center_message(
                f"{self.character.name} dodges the worst of it! "
                f"({self.ability} save {save['total']} vs DC {self.dc})")
        self.last_result = result
        return result


class DialogueSkillGate:
    """Gate a Strife conversation reply behind a skill check.

    Wires ``conversation_reply`` filtered to ``log_number``; each
    matching reply runs ``character.skill_check(skill, dc)`` and fires
    ``on_success(character, result)`` or ``on_fail(character, result)``
    (a buggy callback only warns). What "gated" means in-fiction is the
    mod's business — typically the callbacks grant an item, open a path,
    or apply a price through more replies.

    :meth:`check` is the gate body, callable directly with a scripted
    ``rng=`` for deterministic tests.
    """

    def __init__(self, character: Character, log_number: int, skill: str,
                 dc: int, on_success: Optional[Callable] = None,
                 on_fail: Optional[Callable] = None) -> None:
        skill = str(skill)
        if skill not in SKILLS:
            raise ValueError(f"unknown skill: {skill!r}")
        self.character: Character = character
        self.log_number: int = int(log_number)
        self.skill: str = skill
        self.dc: int = int(dc)
        self.on_success = on_success
        self.on_fail = on_fail
        self.armed: bool = True
        self.attempts: int = 0
        self.last_result: Optional[Dict[str, Any]] = None

        @bd.on("conversation_reply")
        def _handler(event: Dict[str, Any]) -> None:
            self._on_reply(event)

        self._handler = _handler

    def disarm(self) -> None:
        """Permanently disarm the gate (the handler becomes a no-op)."""
        self.armed = False

    def _on_reply(self, event: Dict[str, Any]) -> Optional[Dict[str, Any]]:
        if not self.armed:
            return None
        try:
            if int(event.get("log_number", -1)) != self.log_number:
                return None
        except (TypeError, ValueError):
            return None
        return self.check()

    def check(self, rng: Any = bd) -> Dict[str, Any]:
        """Run the gated skill check now and fire the outcome callback."""
        result = self.character.skill_check(self.skill, self.dc, rng=rng)
        self.attempts += 1
        self.last_result = result
        callback = self.on_success if result["success"] else self.on_fail
        if callback is not None:
            _fire([callback], self.character, result)
        return result


# --- persistent character state ---------------------------------------------------


class CharacterState:
    """Owns a :class:`Character` and persists it via ``bd.state``.

    Mirrors ``bd_vtm.VtMState``: :meth:`save`/:meth:`load` serialize the
    character (name, level, XP, HP, hit die, ability scores, skill and
    save proficiencies, resource pools) JSON-safe into
    ``bd.state["bd_dnd"]``; :meth:`arm_persistence` registers the engine
    ``save``/``load`` handlers exactly once.

    **Class bindings and mastery** (see ``bd_dnd.classes``) persist as
    plain data only: the ``class_id`` name plus the ``skill_uses`` and
    ``masteries`` counters ride along in the snapshot when present.
    Class *definitions* are script-side constants and are never
    persisted; the mod re-binds the live class object at startup with
    ``bind_class`` (idempotent, so re-running it after a restore only
    warns). Saves written before classes existed simply lack the new
    keys; restore skips them, so old checkpoints keep working.
    """

    def __init__(self, character: Character,
                 state_key: str = STATE_KEY) -> None:
        self.character: Character = character
        self.state_key: str = str(state_key)
        self._persistence_armed: bool = False

    def serialize(self) -> Dict[str, Any]:
        """Plain JSON-able snapshot (no Actor handles, no callables)."""
        data: Dict[str, Any] = {"version": 1,
                                "character": self.character.serialize()}
        class_id = getattr(self.character, "class_id", None)
        if class_id:
            data["class_id"] = str(class_id)
        skill_uses = getattr(self.character, "skill_uses", None)
        if isinstance(skill_uses, dict) and skill_uses:
            data["skill_uses"] = {str(k): int(v)
                                  for k, v in skill_uses.items()}
        masteries = getattr(self.character, "masteries", None)
        if isinstance(masteries, dict) and masteries:
            data["masteries"] = {str(k): int(v)
                                 for k, v in masteries.items()}
        return data

    def restore(self, data: Any) -> None:
        """Restore from a :meth:`serialize` snapshot; tolerant of junk.

        Next to the character itself, the plain class/mastery
        attributes written by ``bd_dnd.classes`` re-apply here:
        ``class_id``, ``skill_uses``, and ``masteries``. Class
        *definitions* are never persisted, so a restored character
        knows its class name only; the mod re-binds the live class
        object at startup with ``bind_class`` (safe to call again:
        it warns and keeps the existing binding).
        """
        if not isinstance(data, dict):
            return
        self.character.restore(data.get("character"))
        class_id = data.get("class_id")
        if class_id:
            self.character.class_id = str(class_id)
        for key in ("skill_uses", "masteries"):
            values = data.get(key)
            if not isinstance(values, dict) or not values:
                continue
            cleaned: Dict[str, int] = {}
            for name, tier in values.items():
                try:
                    cleaned[str(name)] = int(tier)
                except (TypeError, ValueError):
                    continue
            setattr(self.character, key, cleaned)

    def save(self) -> None:
        """Write the snapshot into ``bd.state`` (call from a save event)."""
        bd.state[self.state_key] = self.serialize()

    def load(self) -> None:
        """Restore from ``bd.state`` (call from a load event)."""
        self.restore(bd.state.get(self.state_key))

    def arm_persistence(self) -> None:
        """Register the save/load handlers exactly once per state."""
        if self._persistence_armed:
            return
        self._persistence_armed = True
        state = self

        @bd.on("save")
        def _on_save(event: Dict[str, Any]) -> None:
            try:
                state.save()
            except Exception as exc:
                bd.warn(f"bd_dnd: could not serialize character state: {exc!r}")

        @bd.on("load")
        def _on_load(event: Dict[str, Any]) -> None:
            try:
                state.load()
            except Exception as exc:
                bd.warn(f"bd_dnd: could not restore character state: {exc!r}")


# --- parties ---------------------------------------------------------------------


class Party:
    """A roster of :class:`Character` members with shared XP awards.

    The party owns no resources of its own — each member keeps its own
    HP/XP/resources (and its own ``on_level_up`` hooks). The party adds
    an *active* member cursor (the one a sheet shows, the one solo
    rewards go to) and :meth:`award_xp` with party-style splitting.

    ::

        party = bd_dnd.Party([fighter, mage], name="The Delvers")
        party.set_active("Mage")        # by name, or by index
        party.award_xp(101)             # 50 each, remainder 1 to active
        party.award_xp(250, share=False)  # 250 to the active member only

    Persist a whole party with :class:`PartyState` (mutually exclusive
    with :class:`CharacterState` — use one container per character).
    """

    def __init__(self, members: Any = None, name: str = "Party") -> None:
        self.name: str = str(name)
        self._members: List[Character] = []
        self._active_index: int = 0
        for member in members or ():
            self.add(member)

    # -- roster ---------------------------------------------------------------

    def add(self, character: Character) -> Character:
        """Append a member (unique by name) and return it."""
        if any(member.name == character.name for member in self._members):
            raise ValueError(f"bd_dnd: duplicate party member "
                             f"{character.name!r}")
        self._members.append(character)
        return character

    def remove(self, name: str) -> Optional[Character]:
        """Remove a member by name; returns it, or None when absent."""
        name = str(name)
        for index, member in enumerate(self._members):
            if member.name == name:
                removed = self._members.pop(index)
                if self._active_index >= len(self._members):
                    self._active_index = max(0, len(self._members) - 1)
                return removed
        return None

    def get(self, name: str) -> Optional[Character]:
        """Return the member with this name, or None."""
        name = str(name)
        for member in self._members:
            if member.name == name:
                return member
        return None

    @property
    def members(self) -> List[Character]:
        """All members, in join order (a copy — mutate via add/remove)."""
        return list(self._members)

    def __iter__(self):
        return iter(self._members)

    def __len__(self) -> int:
        return len(self._members)

    # -- active member -----------------------------------------------------------

    @property
    def active(self) -> Optional[Character]:
        """The active member, or None when the party is empty."""
        if not self._members:
            return None
        return self._members[self._active_index]

    def set_active(self, name_or_index: Any) -> Optional[Character]:
        """Select the active member by name or roster index.

        Returns the new active member. Raises ``ValueError`` for an
        unknown name or an out-of-range index.
        """
        if isinstance(name_or_index, _INT) and not isinstance(name_or_index, bool):
            index = _INT(name_or_index)
            if not 0 <= index < len(self._members):
                raise ValueError(f"bd_dnd: party index {index} out of range "
                                 f"(0..{len(self._members) - 1})")
            self._active_index = index
            return self.active
        member = self.get(str(name_or_index))
        if member is None:
            raise ValueError(f"bd_dnd: no party member named "
                             f"{name_or_index!r}")
        self._active_index = self._members.index(member)
        return member

    # -- XP ----------------------------------------------------------------------

    def award_xp(self, amount: int, share: bool = True
                 ) -> Dict[str, List[Dict[str, Any]]]:
        """Award XP to the party; returns ``{member_name: [level events]}``.

        With ``share=True`` (default) the amount is split evenly by
        integer division and the remainder goes to the active member;
        with ``share=False`` the active member gets the full amount.
        Level-up hooks fire per member through
        :meth:`Character.award_xp`, and each member's events appear
        under their name in the result.
        """
        amount = _INT(amount)
        results: Dict[str, List[Dict[str, Any]]] = {}
        if amount <= 0 or not self._members:
            return results
        if share:
            base = amount // len(self._members)
            remainder = amount - base * len(self._members)
            for member in self._members:
                share_amount = base + (remainder if member is self.active else 0)
                results[member.name] = member.award_xp(share_amount)
        else:
            active = self.active
            if active is not None:
                results[active.name] = active.award_xp(amount)
        return results

    # -- persistence ---------------------------------------------------------------

    def serialize(self) -> Dict[str, Any]:
        """Plain JSON-able snapshot (no handles, no callables)."""
        return {
            "version": 1,
            "name": self.name,
            "active_index": self._active_index,
            "members": [member.serialize() for member in self._members],
        }

    def restore(self, data: Any) -> None:
        """Restore from a :meth:`serialize` snapshot; tolerant of junk.

        The roster itself always comes from mod code (like quest
        definitions in ``bd_quests``): saved members are matched onto
        the current roster by name; unknown names and malformed entries
        are skipped.
        """
        if not isinstance(data, dict):
            return
        saved_members = data.get("members")
        if isinstance(saved_members, list):
            by_name = {}
            for entry in saved_members:
                if isinstance(entry, dict) and "name" in entry:
                    by_name[str(entry["name"])] = entry
            for member in self._members:
                saved = by_name.get(member.name)
                if saved is not None:
                    member.restore(saved)
        try:
            index = _INT(data.get("active_index", self._active_index))
            if 0 <= index < len(self._members):
                self._active_index = index
        except (TypeError, ValueError):
            pass

    def __repr__(self) -> str:  # pragma: no cover - debugging aid
        names = ", ".join(member.name for member in self._members)
        return f"<Party {self.name!r} [{names}] active={self.active!r}>"


class PartyState:
    """Owns a :class:`Party` and persists it via ``bd.state``.

    Mirrors :class:`CharacterState` (which itself mirrors
    ``bd_vtm.VtMState``): :meth:`save`/:meth:`load` serialize every
    member plus the active-member cursor JSON-safe into
    ``bd.state["bd_dnd_party"]``; :meth:`arm_persistence` registers the
    engine ``save``/``load`` handlers exactly once.

    **Companions ride along.** Register world-bound companions with
    :meth:`add_companion` (or the ``companions=`` parameter); their
    descriptors (member name, actor class, follow distances, current
    TID, dead flag — never Actor handles) serialize next to the party
    and are handed back to the matching live :class:`Companion` on load,
    which re-binds to the restored world actor by TID on the next
    ``map_load`` (or respawns near the player when the TID is gone).
    Saves written before companions existed simply lack the
    ``"companions"`` key — restore skips it and nothing happens, so old
    checkpoints keep working.

    **CharacterState and PartyState are mutually exclusive choices** —
    use one container per character. Persisting the same Character
    through both is harmless only because same-moment snapshots restore
    idempotently; once they diverge mid-session the later load handler
    wins, which is never what you want.
    """

    def __init__(self, party: Party, state_key: str = PARTY_STATE_KEY,
                 companions: Any = None) -> None:
        self.party: Party = party
        self.state_key: str = str(state_key)
        self.companions: List[Any] = list(companions or ())
        self._persistence_armed: bool = False

    def add_companion(self, companion: Any) -> Any:
        """Register a :class:`Companion` for save/load round-trips."""
        self.companions.append(companion)
        return companion

    def serialize(self) -> Dict[str, Any]:
        """Plain JSON-able snapshot (no Actor handles, no callables)."""
        data: Dict[str, Any] = {"version": 2, "party": self.party.serialize()}
        if self.companions:
            data["companions"] = [c.serialize() for c in self.companions]
        return data

    def restore(self, data: Any) -> None:
        """Restore from a :meth:`serialize` snapshot; tolerant of junk.

        Companion entries are matched onto the registered live
        companions by member name; each match re-reads its descriptor
        (including the TID it had at save time) and re-binds on the next
        ``map_load`` — the engine fires that event with the restored
        world in place, which is when TID resolution is meaningful.
        """
        if not isinstance(data, dict):
            return
        self.party.restore(data.get("party"))
        saved = data.get("companions")
        if not isinstance(saved, list):
            return
        by_member = {str(c.member_name): c for c in self.companions}
        for entry in saved:
            if not isinstance(entry, dict):
                continue
            companion = by_member.get(str(entry.get("member_name", "")))
            if companion is not None:
                try:
                    companion.restore(entry)
                except Exception as exc:
                    bd.warn(f"bd_dnd: companion restore failed: {exc!r}")

    def save(self) -> None:
        """Write the snapshot into ``bd.state`` (call from a save event)."""
        bd.state[self.state_key] = self.serialize()

    def load(self) -> None:
        """Restore from ``bd.state`` (call from a load event)."""
        self.restore(bd.state.get(self.state_key))

    def arm_persistence(self) -> None:
        """Register the save/load handlers exactly once per state."""
        if self._persistence_armed:
            return
        self._persistence_armed = True
        state = self

        @bd.on("save")
        def _on_save(event: Dict[str, Any]) -> None:
            try:
                state.save()
            except Exception as exc:
                bd.warn(f"bd_dnd: could not serialize party state: {exc!r}")

        @bd.on("load")
        def _on_load(event: Dict[str, Any]) -> None:
            try:
                state.load()
            except Exception as exc:
                bd.warn(f"bd_dnd: could not restore party state: {exc!r}")


def __getattr__(name: str) -> Any:
    # Lazy convenience accessors so `bd_dnd.CharacterSheet` (and friends)
    # work without paying for the UI module unless it is actually used
    # (mirrors bd_quests / bd_vtm).
    if name == "CharacterSheet":
        from .sheet import CharacterSheet
        return CharacterSheet
    if name == "PartySheet":
        from .sheet import PartySheet
        return PartySheet
    if name == "bind_sheet_toggle":
        from .sheet import bind_sheet_toggle
        return bind_sheet_toggle
    if name == "Companion":
        from .companions import Companion
        return Companion
    if name in ("CharacterClass", "CLASS_LEVELS_ASI", "bind_class",
                "apply_class_level", "CreationWizard", "track_skill_use",
                "mastery", "advancement_check", "skill_bonus"):
        import importlib
        return getattr(importlib.import_module(".classes", __name__), name)
    raise AttributeError(f"module {__name__!r} has no attribute {name!r}")
