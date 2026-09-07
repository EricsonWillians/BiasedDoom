"""Class-based progression and character creation for :mod:`bd_dnd`.

This module is pure rules: besides the deterministic script RNG (and
``bd.warn`` for recoverable misuse) it never touches the engine. Mods
define :class:`CharacterClass` objects as script-side constants
(exactly like quest definitions in ``bd_quests``), build characters
through the optional :class:`CreationWizard` model, and attach a class
with :func:`bind_class`::

    import biaseddoom as bd
    import bd_dnd


    def grant_second_wind(character):
        character.grant_resource("second_wind", 1)


    fighter = bd_dnd.CharacterClass(
        name="Fighter", hit_die=10,
        primary_abilities=("str", "dex"),
        proficient_saves=("str", "con"),
        class_skills=("athletics", "perception", "survival"),
        features={
            1: [{"id": "second_wind", "name": "Second Wind",
                 "description": "Dig in and rally once per rest.",
                 "apply": grant_second_wind}],
            2: [{"id": "action_surge", "name": "Action Surge",
                 "description": "Act twice, once per rest.", "apply": None}],
        },
        resources={"second_wind": 1},
        starting_equipment=[("Clip", 2)],
    )

    wizard = bd_dnd.CreationWizard(rng=bd)
    wizard.choose_class(fighter)
    wizard.set_name("Crawler")
    wizard.use_point_buy()
    wizard.set_score("str", 15)
    wizard.set_score("dex", 14)
    wizard.set_score("con", 13)
    wizard.set_score("int", 8)
    wizard.set_score("wis", 12)
    wizard.set_score("cha", 10)
    wizard.assign_skill("athletics")
    wizard.assign_skill("perception")
    wizard.assign_skill("survival")
    hero = wizard.finish()

    bd_dnd.bind_class(hero, fighter)   # warns: finish() already bound it

Behaviour summary:

- **Class binding.** :func:`bind_class` records ``character.class_id``
  (a plain attribute) and applies the level-1 feature dicts (calling
  their ``apply`` callables) plus the per-rest resource pools, then
  appends a hook to ``character.on_level_up`` so every later level runs
  :func:`apply_class_level`. Binding is idempotent: a second call warns
  and returns the character unchanged, which also makes re-running it
  after a save restore safe (see :class:`~bd_dnd.CharacterState`).
- **Ability score improvements.** Levels in :data:`CLASS_LEVELS_ASI`
  queue two points on ``character.pending_asi`` (a list of
  ``{"points", "spent"}`` dicts); the mod spends them, the engine never
  raises ability scores on its own.
- **Creation.** :class:`CreationWizard` is a pure model with no engine
  calls: standard array, point buy, or 4d6-drop-lowest rolls, exactly
  ``min(3, len(class_skills))`` class skills, and a validating
  :meth:`CreationWizard.finish` that raises ``ValueError`` naming the
  first problem found.
- **Use-based mastery.** :func:`track_skill_use` counts how often a
  skill is exercised; :func:`advancement_check` rolls d100 against
  ``90 + 10 * mastery`` and raises the tier on a success (capped at
  :data:`MASTERY_CAP`); :func:`skill_bonus` is the flat bonus a mod
  adds on top of a check result total.

Class definitions are **never** persisted. Only the plain ``class_id``
name and the ``skill_uses`` / ``masteries`` counters round-trip through
:class:`~bd_dnd.CharacterState`; the mod re-binds the live class
object at startup.
"""

from __future__ import annotations

from typing import Any, Dict, List, Optional, Set, Tuple

import biaseddoom as bd

from . import ABILITIES, SKILLS, AbilityScores, Character, _roll

#: Levels that grant an ability score improvement: two points queue on
#: ``character.pending_asi`` (see :func:`apply_class_level`).
CLASS_LEVELS_ASI: Tuple[int, ...] = (4, 8, 12, 16, 19)

#: Maximum use-based mastery tier (see :func:`advancement_check`).
MASTERY_CAP: int = 3

#: Default number of class skills a character creation picks.
_CLASS_SKILL_PICKS: int = 3

#: The classic standard array, as an unordered multiset.
_STANDARD_ARRAY: Tuple[int, ...] = (15, 14, 13, 12, 10, 8)


def _check_feature(entry: Any) -> Dict[str, Any]:
    """Validate and normalize one feature dict.

    Every entry becomes a dict with exactly the keys ``id``, ``name``,
    ``description``, and ``apply`` (callable or None). Anything else
    raises ``ValueError``.
    """
    if not isinstance(entry, dict):
        raise ValueError(f"bd_dnd: feature entries must be dicts, got "
                         f"{entry!r}")
    fid = str(entry.get("id", "")).strip()
    if not fid:
        raise ValueError("bd_dnd: feature dicts need a non-empty 'id'")
    apply_fn = entry.get("apply")
    if apply_fn is not None and not callable(apply_fn):
        raise ValueError(f"bd_dnd: feature {fid!r} 'apply' must be "
                         f"callable or None, got {apply_fn!r}")
    return {"id": fid,
            "name": str(entry.get("name", fid)),
            "description": str(entry.get("description", "")),
            "apply": apply_fn}


class CharacterClass:
    """A character class definition (fighter, mage, ...): pure data.

    Feature dicts use the shape::

        {"id": "second_wind", "name": "Second Wind",
         "description": "...", "apply": callable(character) | None}

    ``apply`` runs when the feature is granted (level 1 through
    :func:`bind_class`, later levels through :func:`apply_class_level`);
    ``None`` marks a description-only feature. ``resources`` maps a
    per-rest pool name to the charges seeded at creation;
    ``starting_equipment`` is a list of ``(class_name, count)`` pairs
    the mod hands out however it likes. Unknown ability or skill names
    raise ``ValueError`` immediately, at construction time.
    """

    def __init__(self, name: str, hit_die: int,
                 primary_abilities: Any = (),
                 proficient_saves: Any = (),
                 class_skills: Any = (),
                 features: Optional[Dict[int, Any]] = None,
                 resources: Optional[Dict[str, int]] = None,
                 starting_equipment: Any = ()) -> None:
        name = str(name).strip()
        if not name:
            raise ValueError("bd_dnd: a class needs a non-empty name")
        self.name: str = name
        self.hit_die: int = max(2, int(hit_die))
        self.primary_abilities: Tuple[str, ...] = tuple(
            str(a).lower() for a in (primary_abilities or ()))
        bad = [a for a in self.primary_abilities if a not in ABILITIES]
        if bad:
            raise ValueError(f"bd_dnd: unknown abilities in "
                             f"primary_abilities: {bad}")
        self.proficient_saves: Tuple[str, ...] = tuple(
            str(a).lower() for a in (proficient_saves or ()))
        bad = [a for a in self.proficient_saves if a not in ABILITIES]
        if bad:
            raise ValueError(f"bd_dnd: unknown abilities in "
                             f"proficient_saves: {bad}")
        self.class_skills: Tuple[str, ...] = tuple(
            str(s) for s in (class_skills or ()))
        bad = [s for s in self.class_skills if s not in SKILLS]
        if bad:
            raise ValueError(f"bd_dnd: class_skills must be drawn from "
                             f"bd_dnd.SKILLS; unknown: {bad}")
        self.features: Dict[int, List[Dict[str, Any]]] = {}
        for level, entries in dict(features or {}).items():
            try:
                level = int(level)
            except (TypeError, ValueError):
                raise ValueError(f"bd_dnd: feature level keys must be "
                                 f"integers, got {level!r}")
            if level < 1:
                raise ValueError(f"bd_dnd: feature levels start at 1, "
                                 f"got {level}")
            self.features[level] = [
                _check_feature(entry) for entry in (entries or ())]
        self.resources: Dict[str, int] = {}
        for key, value in dict(resources or {}).items():
            try:
                charges = int(value)
            except (TypeError, ValueError):
                raise ValueError(f"bd_dnd: bad resource charge count for "
                                 f"{key!r}: {value!r}")
            self.resources[str(key)] = max(0, charges)
        self.starting_equipment: List[Tuple[str, int]] = []
        for item in starting_equipment or ():
            try:
                class_name, count = item
                count = int(count)
            except (TypeError, ValueError):
                raise ValueError(f"bd_dnd: starting_equipment entries "
                                 f"must be (class_name, count) pairs, "
                                 f"got {item!r}")
            if count < 1:
                raise ValueError(f"bd_dnd: starting_equipment counts "
                                 f"must be >= 1, got {item!r}")
            self.starting_equipment.append((str(class_name), count))

    def features_for(self, level: int) -> List[Dict[str, Any]]:
        """Feature dicts granted at ``level`` (empty list when none)."""
        try:
            level = int(level)
        except (TypeError, ValueError):
            return []
        return list(self.features.get(level, ()))

    def __repr__(self) -> str:  # pragma: no cover - debugging aid
        return (f"<CharacterClass {self.name!r} d{self.hit_die} "
                f"skills={len(self.class_skills)} "
                f"features={len(self.features)}>")


def bind_class(character: Character, class_: CharacterClass) -> Character:
    """Attach ``class_`` to ``character`` and apply its level-1 package.

    Records ``character.class_id`` (a plain attribute, the name the
    saves persist) and ``character.cls`` (the live class object),
    applies every level-1 feature dict (calling its ``apply`` callable
    when not None), and seeds the per-rest resource pools. Hit points
    are left alone; the character already has them. An ``on_level_up``
    hook is appended so each reached level runs :func:`apply_class_level`.

    Idempotent: when the character already carries a ``class_id`` the
    call warns through ``bd.warn`` and returns the character unchanged
    (no second hook). That makes re-running ``bind_class`` after a save
    restore safe: :class:`~bd_dnd.CharacterState` restores the
    ``class_id`` name only, and the class object bound earlier in the
    session is still attached to the same character.
    """
    if not isinstance(class_, CharacterClass):
        raise ValueError(f"bd_dnd: bind_class needs a CharacterClass, "
                         f"got {class_!r}")
    bound = getattr(character, "class_id", None)
    if bound is not None:
        bd.warn(f"bd_dnd: {character.name} is already bound to class "
                f"{bound!r}; not rebinding to {class_.name!r}")
        return character
    character.cls = class_
    character.class_id = class_.name
    for feature in class_.features_for(1):
        apply_fn = feature.get("apply")
        if apply_fn is not None:
            apply_fn(character)
    for resource, charges in class_.resources.items():
        character.grant_resource(resource, charges)

    def _on_class_level(changed: Character, level: int) -> None:
        apply_class_level(changed, class_, level)

    character.on_level_up.append(_on_class_level)
    return character


def apply_class_level(character: Character, class_: CharacterClass,
                      level: int) -> List[Dict[str, Any]]:
    """Apply one class level's features (and ASI points) to a character.

    Returns the feature dicts granted at ``level`` (their ``apply``
    callables invoked when not None). Levels in :data:`CLASS_LEVELS_ASI`
    also queue two points on ``character.pending_asi`` (created empty
    when missing): an existing entry with unspent points gains two
    points, otherwise a fresh ``{"points": 2, "spent": 0}`` dict is
    appended. Plain levels (no features, no ASI) never raise and
    return an empty list.
    """
    applied: List[Dict[str, Any]] = []
    try:
        level = int(level)
    except (TypeError, ValueError):
        return applied
    for feature in class_.features_for(level):
        apply_fn = feature.get("apply")
        if apply_fn is not None:
            apply_fn(character)
        applied.append(feature)
    if level in CLASS_LEVELS_ASI:
        pending = getattr(character, "pending_asi", None)
        if pending is None:
            pending = []
            character.pending_asi = pending
        for entry in pending:
            if not isinstance(entry, dict):
                continue
            try:
                points = int(entry.get("points", 0))
                spent = int(entry.get("spent", 0))
            except (TypeError, ValueError):
                continue
            if points - spent > 0:
                entry["points"] = points + 2
                break
        else:
            pending.append({"points": 2, "spent": 0})
    return applied


def _point_buy_cost(score: int) -> int:
    """Point-buy cost of a single score (8 costs 0, 15 costs 9)."""
    score = int(score)
    if score <= 8:
        return 0
    if score <= 13:
        return score - 8
    if score <= 15:
        return 5 + (score - 13) * 2
    return 9 + (score - 15) * 3


class CreationWizard:
    """Pure creation model: class, name, scores, skills, then finish.

    No engine calls beyond the RNG (used by :meth:`use_rolled`); every
    invalid state raises ``ValueError`` with a message naming the
    problem, so the wizard works headless and under ``-scripttest``.

    Typical flow::

        wizard = bd_dnd.CreationWizard(rng=bd)
        wizard.choose_class(fighter)
        wizard.set_name("Crawler")
        wizard.use_point_buy()          # or use_standard_array()
        wizard.set_score("str", 15)     # ... all six abilities
        wizard.assign_skill("athletics")
        hero = wizard.finish()

    Score methods:

    - ``use_standard_array()``: the multiset ``{15, 14, 13, 12, 10, 8}``
      must be used exactly once (validated in :meth:`finish`).
    - ``use_point_buy(budget=27)``: scores 8..15; per-score cost is 1
      per point up to 13, 2 per point for 14-15, 3 per point above 15
      (scores above 15 are rejected outright); the total must not
      exceed the budget. The classic table expects the full budget
      spent; underspending is tolerated.
    - ``use_rolled()``: rolls 4d6 drop-lowest per ability immediately,
      in ``str/dex/con/int/wis/cha`` order, through the wizard's ``rng``;
      :meth:`set_score` may swap values afterwards (clamped to 3..18).

    Skills: exactly ``min(3, len(class_skills))`` distinct picks, all
    drawn from the chosen class's skill list.
    """

    def __init__(self, rng: Any = bd) -> None:
        self.rng: Any = rng
        self.class_: Optional[CharacterClass] = None
        self._name: Optional[str] = None
        self._method: Optional[str] = None
        self._budget: int = 27
        self._scores: Dict[str, int] = {}
        self._skills: Set[str] = set()

    # -- inputs ---------------------------------------------------------------

    def choose_class(self, class_: CharacterClass) -> None:
        """Pick the character class (required before :meth:`finish`)."""
        if not isinstance(class_, CharacterClass):
            raise ValueError(f"bd_dnd: choose_class needs a "
                             f"CharacterClass, got {class_!r}")
        self.class_ = class_

    def set_name(self, name: str) -> None:
        """Set the character name; must be non-empty after stripping."""
        name = str(name).strip()
        if not name:
            raise ValueError("bd_dnd: character name must not be empty")
        self._name = name

    def use_standard_array(self) -> None:
        """Score method: the classic ``{15, 14, 13, 12, 10, 8}`` array."""
        self._method = "standard_array"

    def use_point_buy(self, budget: int = 27) -> None:
        """Score method: point buy with ``budget`` points (default 27).

        Validation happens in :meth:`finish`.
        """
        budget = int(budget)
        if budget < 0:
            raise ValueError(f"bd_dnd: point-buy budget must be >= 0, "
                             f"got {budget}")
        self._method = "point_buy"
        self._budget = budget

    def use_rolled(self) -> None:
        """Score method: roll 4d6 drop-lowest per ability right now.

        Rolls flow through the wizard's ``rng`` in
        :data:`~bd_dnd.ABILITIES` order (str, dex, con, int, wis, cha)
        and become the pending scores; :meth:`set_score` may swap any
        of them afterwards.
        """
        self._method = "rolled"
        self._scores = {}
        for ability in ABILITIES:
            rolls = [_roll(self.rng, 1, 6) for _ in range(4)]
            self._scores[ability] = sum(rolls) - min(rolls)

    def set_score(self, ability: str, value: Any) -> None:
        """Assign one pending ability score.

        Rolled scores are clamped to 3..18; the other methods validate
        their full shape in :meth:`finish`.
        """
        ability = str(ability).lower()
        if ability not in ABILITIES:
            raise ValueError(f"unknown ability: {ability!r}")
        try:
            value = int(value)
        except (TypeError, ValueError):
            raise ValueError(f"bd_dnd: ability scores must be integers, "
                             f"got {value!r}")
        if self._method == "rolled":
            value = max(3, min(18, value))
        self._scores[ability] = value

    def assign_skill(self, skill: str) -> None:
        """Add one class skill pick.

        Must name a known skill; once a class is chosen it must also
        appear on that class's skill list, and at most
        ``min(3, len(class_skills))`` picks are kept.
        """
        skill = str(skill)
        if skill not in SKILLS:
            raise ValueError(f"unknown skill: {skill!r}")
        if self.class_ is not None:
            if skill not in self.class_.class_skills:
                raise ValueError(f"{skill!r} is not a class skill of "
                                 f"{self.class_.name!r}")
            limit = min(_CLASS_SKILL_PICKS, len(self.class_.class_skills))
            if skill not in self._skills and len(self._skills) >= limit:
                raise ValueError(f"bd_dnd: at most {limit} class skill(s) "
                                 f"may be chosen for {self.class_.name!r}")
        self._skills.add(skill)

    def unassign_skill(self, skill: str) -> None:
        """Drop one class skill pick (no-op when it was not picked)."""
        self._skills.discard(str(skill))

    # -- views ---------------------------------------------------------------

    @property
    def name(self) -> Optional[str]:
        """The name set with :meth:`set_name`, or None."""
        return self._name

    @property
    def method(self) -> Optional[str]:
        """Active score method (``"standard_array"``, ``"point_buy"``,
        ``"rolled"``, or None)."""
        return self._method

    @property
    def scores(self) -> Dict[str, int]:
        """Copy of the pending ability scores assigned so far."""
        return dict(self._scores)

    @property
    def skills(self) -> frozenset:
        """The current class skill picks."""
        return frozenset(self._skills)

    @property
    def points_remaining(self) -> int:
        """Point-buy budget not yet spent (0 outside point buy)."""
        if self._method != "point_buy":
            return 0
        return self._budget - sum(_point_buy_cost(v)
                                  for v in self._scores.values())

    # -- validation and build -------------------------------------------------

    def _validate_scores(self) -> Dict[str, int]:
        missing = [a for a in ABILITIES if a not in self._scores]
        if missing:
            raise ValueError(f"bd_dnd: ability scores not assigned "
                             f"for: {missing}")
        scores = {a: int(self._scores[a]) for a in ABILITIES}
        if self._method == "standard_array":
            if sorted(scores.values()) != sorted(_STANDARD_ARRAY):
                raise ValueError(
                    "bd_dnd: the standard array is the multiset "
                    "{15, 14, 13, 12, 10, 8} used exactly once; got "
                    f"{sorted(scores.values())}")
        elif self._method == "point_buy":
            for ability, value in scores.items():
                if not 8 <= value <= 15:
                    raise ValueError(
                        f"bd_dnd: point-buy scores must be 8..15 "
                        f"({ability}={value})")
            spent = sum(_point_buy_cost(v) for v in scores.values())
            if spent > self._budget:
                raise ValueError(
                    f"bd_dnd: point buy spends {spent} of "
                    f"{self._budget} points")
        elif self._method == "rolled":
            for ability, value in scores.items():
                if not 3 <= value <= 18:
                    raise ValueError(
                        f"bd_dnd: rolled scores must be 3..18 "
                        f"({ability}={value})")
        else:
            raise ValueError(
                "bd_dnd: choose a score method (use_standard_array, "
                "use_point_buy, or use_rolled)")
        return scores

    def _validate_skills(self) -> Tuple[str, ...]:
        assert self.class_ is not None
        needed = min(_CLASS_SKILL_PICKS, len(self.class_.class_skills))
        chosen = sorted(self._skills)
        if len(chosen) != needed:
            raise ValueError(
                f"bd_dnd: {self.class_.name} needs exactly {needed} "
                f"class skill(s); {len(chosen)} chosen")
        bad = [s for s in chosen if s not in self.class_.class_skills]
        if bad:
            raise ValueError(
                f"bd_dnd: skills not on the {self.class_.name} list: "
                f"{bad}")
        return tuple(chosen)

    def finish(self) -> Character:
        """Validate everything and build the :class:`~bd_dnd.Character`.

        The new character gets the class hit die, the chosen save
        proficiencies, the picked class skills, the class resource
        pools, and the level-1 feature package (through
        :func:`bind_class`, which also records ``character.class_id``).
        Hit points come from the hit die and CON modifier as usual.
        Raises ``ValueError`` naming the first problem found.
        """
        if self._name is None:
            raise ValueError("bd_dnd: character creation needs a name "
                             "(set_name)")
        if self.class_ is None:
            raise ValueError("bd_dnd: character creation needs a class "
                             "(choose_class)")
        scores = self._validate_scores()
        skills = self._validate_skills()
        character = Character(self._name, AbilityScores(**scores),
                              hit_die=self.class_.hit_die,
                              proficient_skills=skills,
                              proficient_saves=self.class_.proficient_saves)
        bind_class(character, self.class_)
        return character

    def __repr__(self) -> str:  # pragma: no cover - debugging aid
        chosen = self.class_.name if self.class_ is not None else None
        return (f"<CreationWizard name={self._name!r} class={chosen!r} "
                f"method={self._method!r}>")


def _uses_of(character: Character, skill: str) -> int:
    """Current use counter of ``skill`` (0 when never tracked)."""
    uses = getattr(character, "skill_uses", None)
    if not isinstance(uses, dict):
        return 0
    try:
        return max(0, int(uses.get(skill, 0)))
    except (TypeError, ValueError):
        return 0


def track_skill_use(character: Character, skill: str) -> int:
    """Record one use of ``skill``; returns the character's new total.

    Counters live in ``character.skill_uses``, a plain dict attribute
    (created on first use, persisted by
    :class:`~bd_dnd.CharacterState`).
    """
    skill = str(skill)
    if skill not in SKILLS:
        raise ValueError(f"unknown skill: {skill!r}")
    uses = getattr(character, "skill_uses", None)
    if not isinstance(uses, dict):
        uses = {}
        character.skill_uses = uses
    uses[skill] = _uses_of(character, skill) + 1
    return uses[skill]


def mastery(character: Character, skill: str) -> int:
    """Current use-based mastery tier of ``skill`` (0 by default)."""
    tiers = getattr(character, "masteries", None)
    if not isinstance(tiers, dict):
        return 0
    try:
        return max(0, int(tiers.get(str(skill), 0)))
    except (TypeError, ValueError):
        return 0


def advancement_check(character: Character, skill: str, rng: Any = bd
                      ) -> Dict[str, Any]:
    """Roll a d100 mastery advancement check for ``skill``.

    The difficulty is ``90 + 10 * mastery``: tier 0 improves on a 90
    or better, tier 1 on a natural 100, and a capped skill (see
    :data:`MASTERY_CAP`) reports ``"reason": "cap"`` without rolling.
    On a success the tier rises by one. The result dict carries
    ``improved``, ``roll``, ``difficulty``, ``mastery``, and ``uses``;
    failures add a ``reason``.
    """
    skill = str(skill)
    if skill not in SKILLS:
        raise ValueError(f"unknown skill: {skill!r}")
    current = mastery(character, skill)
    uses = _uses_of(character, skill)
    if current >= MASTERY_CAP:
        return {"improved": False, "roll": None, "difficulty": 0,
                "mastery": current, "uses": uses, "reason": "cap"}
    difficulty = 90 + 10 * current
    roll_value = _roll(rng, 1, 100)
    improved = roll_value >= difficulty
    if improved:
        tiers = getattr(character, "masteries", None)
        if not isinstance(tiers, dict):
            tiers = {}
            character.masteries = tiers
        tiers[skill] = current + 1
    result: Dict[str, Any] = {"improved": improved, "roll": roll_value,
                              "difficulty": difficulty,
                              "mastery": mastery(character, skill),
                              "uses": uses}
    if not improved:
        result["reason"] = "failed"
    return result


def skill_bonus(character: Character, skill: str) -> int:
    """Flat modifier a mod adds on top of a check result total.

    Equals :func:`mastery`: use-based mastery tiers 0..3 stack on the
    dice and the ability modifier, they never replace them.
    """
    return mastery(character, skill)
