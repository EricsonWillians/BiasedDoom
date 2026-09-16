"""Ashvale Crossing - systems: rules, state, and every engine hook.

Owns the campaign's behavior:

- **Character creation.** The shared ``CreationWizard`` (built at
  ``engine_start``) drives the UI window; :func:`finish_creation` validates
  and builds the hero, arms ``CharacterState`` persistence, hands out the
  class starting equipment, wires level-up feedback, and sends the three
  staggered onboarding pointers.
- **Hero progression.** :func:`wire_hero_progression` (shared by creation
  and the savegame cold-restore path) ties the RPG layer into real Doom
  combat: ``bd_dnd.track_xp_from_kills`` pays tabled XP for every monster
  the player kills, and an ``actor_before_damage`` mutable filter adds the
  hero's level bonus (plus the Scout's Skirmisher note) to every hit the
  local player lands on a live monster.
- **Class actives.** Custom Action 3 (``+pyaction3``, auto-bound to C, the
  ``class_active`` console alias rides the same ``pyui``/``ui_command``
  bridge as ``talk``) fires the hero's one class active from
  ``content.CLASS_ACTIVES``: the Mercenary's Second Wind heal, the Scout's
  Uncanny Step blur, the Lightkeeper's radiant burst.
- **The hub.** The ``NPCManager`` registration (definitions carry the content
  dialogue factories and the shop/services), the currency spawn probe, the
  savegame cold-restore path for the hero and party, and the shop stock
  snapshot persistence.
- **Quest wiring.** Trackers, the xp sink into the hero, the disposition
  sink into the store, the yard-zombie spawn on acceptance, and the
  kill-counter HUD feedback on every yard objective tick.
- **In-world guidance.** :func:`refresh_markers` keeps gold "!" labels over
  whichever NPC currently offers work and vertical beacons on the yard and
  the cache while their quests run; a persistent two-line HUD strip (hero
  status + the tracked objective) rides the display list.
- **Talk interaction.** Custom Action 1 (``+pyaction1``, auto-bound to Q
  at ``engine_start`` unless the player already bound it under Options ->
  Customize Controls, Custom Actions) fires the ``custom_action`` event ->
  :func:`talk`; the ``talk`` console alias (``pyui talk`` ->
  ``ui_command``) routes into the same function. Custom Action 2
  (``+pyaction2``, auto-bound to V) toggles the hero sheet through the
  same ``sheet.toggle()`` the ``toggle_sheet`` alias uses; the K bind from
  ``bind_sheet_toggle`` stays as well.

Manifest entry 2 of 4 (after content.py).
"""

import biaseddoom as bd
import bd_dialogue
import bd_dnd
import bd_npcs
import bd_quests
from bd_dnd.sheet import CharacterSheet

try:
    import ashvale_content as content
except ImportError:  # loaded outside the manifest (bd.import_script direct)
    content = bd.import_script("pyscripts/content.py",
                               module_name="ashvale_content")

# --- module state ---------------------------------------------------------------------

wizard = None               # the shared CreationWizard (pre-hero)
hero = None                 # the bd_dnd.Character, once creation finished
hero_state = None           # CharacterState persistence (armed at creation)
party = None                # bd_dnd.Party (hero + Korr) once recruited
party_state = None          # PartyState persistence (armed at recruitment)
korr_companion = None       # bd_dnd.Companion follower bound to Korr
manager = None              # bd_npcs.NPCManager
dobb_shop = None            # bd_npcs.Shop on the quartermaster
_services = {}              # npc_id -> tuple of Service objects
session = None              # the example's live DialogueSession
hero_sheet = None           # the hero's CharacterSheet (created at founding)
shop_open = False           # the ShopUI window toggle (set by Dobb's trade)
korr_recruited = False      # plain flag; recruit_korr is idempotent
currency_class = content.CURRENCY_CLASSES[0]
currency_ready = False      # the spawn probe ran
last_service_result = None  # {"ok", "message"} of the last service run

# --- hero progression state -----------------------------------------------------

_progression_wired = False    # wire_hero_progression runs once per session
hero_damage_filter = None     # the actor_before_damage filter body (testable)

# --- class active state -----------------------------------------------------------

#: Test hook: when set, the class-active dice roll uses this rng double
#: (any object with randint/int) instead of the engine's deterministic stream.
class_active_rng = None
last_active_message = ""      # the last class-active feedback line
_blur_active = False          # the Scout's blur currently holds
_blur_restore_task = None     # the scheduled blur-restore task id

# --- guidance state (markers + HUD strip) -----------------------------------------

#: Display-id -> what it currently shows ("sera", "dobb", "korr",
#: "yard_beacon", "cache_beacon"); the autotest reads the marker lifecycle
#: out of this dict headlessly.
marker_state = {}
_markers_task = None          # the 7-tic repeating map-local marker task
_hud_task = None              # the 35-tic repeating HUD strip task
_quest_start_seq = {}         # quest_id -> sequence number when first ACTIVE
_quest_start_counter = 0


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


# --- hero progression (kill XP + the level damage bonus) -------------------------


def _apply_hero_damage_bonus(event):
    """The ``actor_before_damage`` filter: the hero's own hits hit harder.

    Every hit the local player lands on a live monster gains
    ``min(8, hero.level - 1)`` bonus damage plus the Scout's
    ``mod_notes["skirmisher_damage"]`` value when present. The event dict
    is the engine's mutable contract; only ``damage`` is rewritten. Kept as
    a plain function (not a closure over ``hero``) so it always reads the
    current module-level hero and the autotest can drive it with synthetic
    event dicts.
    """
    try:
        current = hero
        if current is None:
            return
        if event.get("attacker_player_index") != 0:
            return
        ref = event.get("actor_ref")
        if ref is None or not ref.valid or not ref.alive:
            return
        if not ref.is_monster:
            return
        bonus = min(8, max(0, int(current.level) - 1))
        notes = getattr(current, "mod_notes", None) or {}
        try:
            bonus += int(notes.get("skirmisher_damage", 0) or 0)
        except (TypeError, ValueError):
            pass
        if bonus <= 0:
            return
        event["damage"] = int(event.get("damage") or 0) + bonus
    except Exception as exc:
        bd.warn(f"ashvale: damage filter failed: {exc!r}")


def wire_hero_progression(target):
    """Tie the hero into real Doom combat (once per session).

    ``bd_dnd.track_xp_from_kills`` pays tabled XP for every monster the
    local player kills (exact pawn credit), and the
    ``actor_before_damage`` mutable filter adds the level bonus to the
    player's hits. Both register permanent engine handlers, so this runs
    exactly once; the handlers read the module-level ``hero`` at event
    time, which keeps a cold-restored hero covered without double-wiring.
    """
    global _progression_wired, hero_damage_filter
    if _progression_wired:
        return
    _progression_wired = True
    hero_damage_filter = _apply_hero_damage_bonus
    try:
        bd_dnd.track_xp_from_kills(target, player_index=0)
    except Exception as exc:
        bd.warn(f"ashvale: kill xp wiring failed: {exc!r}")
    bd.on("actor_before_damage")(_apply_hero_damage_bonus)


# --- creation -------------------------------------------------------------------------

#: Interactive-only: while the founding window is visible on a live map the
#: world is engine-paused so the player can read every option unmolested.
#: The autotest and screenshot drivers tick the world on schedules, so
#: main.py disables this there (a paused world would freeze their steps).
creation_pause_enabled = True
_world_paused_for_creation = False


def pause_for_creation():
    """Engine-pause the world while the founding window is open."""
    global _world_paused_for_creation
    if not creation_pause_enabled or _world_paused_for_creation:
        return
    _world_paused_for_creation = True
    try:
        bd.execute("pause")
    except Exception:
        _world_paused_for_creation = False


def unpause_for_creation():
    """Lift the creation pause (idempotent; also manual-pause safe)."""
    global _world_paused_for_creation
    if not _world_paused_for_creation:
        return
    _world_paused_for_creation = False
    try:
        bd.execute("pause")  # toggles; see discard_creation_pause for resets
    except Exception:
        pass


def discard_creation_pause():
    """Forget the pause bookkeeping without toggling (map change resets
    the engine pause state on its own)."""
    global _world_paused_for_creation
    _world_paused_for_creation = False



def _on_hero_level_up(changed, new_level):
    try:
        bd.center_message(f"{changed.name} reaches level {new_level}!")
        bd.play_ui_sound(content.LEVEL_UP_SOUND, volume=0.8)
        bd.hud_text(f"LEVEL {new_level}", id=90011, y=0.16, color="gold",
                    hold=1.2, fade=0.4)
    except Exception:
        pass  # headless: no renderer to flash


def _class_level_hook(changed, level):
    """The level-up hook for cold-restored heroes (see _cold_restore)."""
    cls = getattr(changed, "cls", None)
    if cls is not None:
        bd_dnd.apply_class_level(changed, cls, level)


def finish_creation(wiz=None):
    """Validate the wizard and found the hero.

    Builds the character (the wizard's ``finish`` binds the class and applies
    level-1 features), arms ``CharacterState`` persistence, hands the class
    starting equipment to the player pawn, and wires level-up feedback.
    Returns the hero; raises ``ValueError`` naming the first problem.
    """
    global hero, hero_state
    wiz = wiz if wiz is not None else wizard
    if wiz is None:
        raise ValueError("ashvale: character creation has not started")
    if hero is not None:
        return hero
    new_hero = wiz.finish()  # validates; binds class + level-1 features
    unpause_for_creation()
    new_hero.cls = content.CLASSES_BY_ID.get(
        getattr(new_hero, "class_id", ""), getattr(new_hero, "cls", None))
    new_hero.on_level_up.append(_on_hero_level_up)
    hero = new_hero
    hero_state = bd_dnd.CharacterState(hero)
    hero_state.arm_persistence()
    wire_hero_progression(hero)
    pawn = player_pawn()
    if pawn is not None and hero.cls is not None:
        for class_name, count in hero.cls.starting_equipment:
            try:
                pawn.give_inventory(class_name, int(count))
            except Exception as exc:
                bd.warn(f"ashvale: starting equipment {class_name!r} "
                        f"failed: {exc!r}")
    else:
        bd.log("ashvale: no pawn yet; starting equipment waits for the map")
    bd.log(f"ashvale: {hero.name} the {hero.class_id} walks into Ashvale")
    ensure_hero_sheet()
    _onboard_after_creation()
    return hero


def ensure_hero_sheet():
    """Create (once) the hero's CharacterSheet; the UI draws it.

    Lives here rather than in the UI so the sheet exists even when no
    frame ever renders: the Custom Action 2 toggle and the autotest drive
    it headlessly, where ``imgui_frame`` never fires.
    """
    global hero_sheet
    if hero_sheet is None and hero is not None:
        hero_sheet = CharacterSheet(
            hero, title=f"{content.SHEET_TITLE} - {hero.class_id} "
                        f"{hero.level}")
    return hero_sheet


# --- class actives (Custom Action 3) ------------------------------------------------


def _active_feedback(message):
    """Toast + center-screen feedback for a class active; the last line is
    kept in ``last_active_message`` so the autotest can assert it."""
    global last_active_message
    last_active_message = message
    try:
        from bd_horror import toasts
        toasts.toast(message, kind="info")
    except Exception:
        pass
    try:
        bd.center_message(message)
    except Exception:
        pass


def use_class_active():
    """Fire the hero's one class active (Custom Action 3 / ``class_active``).

    Spends one charge of the class's per-rest resource and applies the pawn
    effect. With no charges left, says so (naming the resource and the rest
    that refills it) and spends nothing.
    """
    current = hero
    if current is None:
        return
    spec = content.CLASS_ACTIVES.get(getattr(current, "class_id", ""))
    if spec is None:
        return
    resource = spec["resource"]
    pawn = player_pawn()
    if pawn is None:
        return  # off-map: no pawn to affect, no charge spent
    if not current.use_resource(resource):
        _active_feedback(f"{spec['name']}: no {resource} charges left; "
                         f"Wren's care restores them.")
        return
    try:
        if spec["id"] == "second_wind":
            _apply_second_wind(current, pawn)
        elif spec["id"] == "uncanny_step":
            _apply_uncanny_step(pawn)
        elif spec["id"] == "light":
            _apply_light(current, pawn)
        else:
            bd.warn(f"ashvale: no effect branch for active {spec['id']!r}")
    except Exception as exc:
        bd.warn(f"ashvale: class active {spec['id']!r} failed: {exc!r}")


def _apply_second_wind(current, pawn):
    """Mercenary: heal the sheet and the pawn by hit die + level."""
    rng = class_active_rng if class_active_rng is not None else bd
    amount = bd_dnd.roll(f"d{current.hit_die}", rng=rng)["total"] \
        + int(current.level)
    current.set_hp(current.hp + amount)
    pawn.heal(amount)
    _active_feedback(f"Second Wind rallies you: +{amount} blood.")


def _apply_uncanny_step(pawn):
    """Scout: a 175-tic blur; refreshing extends instead of stacking."""
    global _blur_active, _blur_restore_task
    if _blur_restore_task is not None:
        try:
            bd.cancel_task(_blur_restore_task)
        except Exception:
            pass
        _blur_restore_task = None
    pawn.damage_factor = content.UNCANNY_STEP_FACTOR
    _blur_active = True

    def _restore_blur():
        global _blur_active, _blur_restore_task
        _blur_restore_task = None
        _blur_active = False
        try:
            live = player_pawn()
            if live is not None:
                live.damage_factor = 1.0
        except Exception:
            pass

    try:
        _blur_restore_task = bd.schedule(
            _restore_blur, delay=content.UNCANNY_STEP_TICS, map_local=False)
    except Exception as exc:
        bd.warn(f"ashvale: blur restore could not be scheduled: {exc!r}")
    _active_feedback("Uncanny Step: you blur (damage taken cut to about a "
                     "third for five seconds).")


def _reset_blur():
    """map_unload guard: never let the blur's damage_factor leak."""
    global _blur_active, _blur_restore_task
    if _blur_restore_task is not None:
        try:
            bd.cancel_task(_blur_restore_task)
        except Exception:
            pass
        _blur_restore_task = None
    if not _blur_active:
        return
    _blur_active = False
    try:
        pawn = player_pawn()
        if pawn is not None:
            pawn.damage_factor = 1.0
    except Exception:
        pass


def _apply_light(current, pawn):
    """Lightkeeper: a radiant burst around the pawn."""
    damage = 12 + 2 * int(current.level)
    bd.radius_damage(pawn, damage, content.LIGHT_RADIUS, source=pawn,
                     damage_type="Fire", hurt_source=False)
    _active_feedback(f"Light: the burst burns for {damage}.")


def _onboard_after_creation():
    """Three staggered pointers for a brand-new hero on a live map.

    Sent at ~1/2/3 seconds after the founding: who to talk to (the gold
    marker and the talk key), the journal/sheet keys, and the class active
    with its one-line effect. Off-map there is nobody to point at.
    """
    if player_pawn() is None:
        return

    def _hint(command, fallback):
        try:
            name = bd.input_binding(command)
        except Exception:
            name = None
        return name or fallback

    def _send(text):
        try:
            from bd_horror import toasts
            toasts.toast(text, kind="info")
        except Exception:
            pass
        try:
            bd.center_message(text)
        except Exception:
            pass

    spec = content.CLASS_ACTIVES.get(getattr(hero, "class_id", ""), {})
    lines = (
        (35, content.ONBOARD_TALK.format(talk_key=action_key_hint(1))),
        (70, content.ONBOARD_WINDOWS.format(
            journal_key=_hint("toggle_journal", "J"),
            sheet_key=_hint("toggle_sheet", "K"))),
        (105, content.ONBOARD_ACTIVE.format(
            active_key=action_key_hint(3),
            active_name=spec.get("name", "class active"),
            active_effect=spec.get("effect", ""))),
    )
    for delay, text in lines:
        try:
            bd.schedule(lambda text=text: _send(text), delay=delay,
                        map_local=True)
        except Exception as exc:
            bd.warn(f"ashvale: onboarding schedule failed: {exc!r}")


# --- recruitment ------------------------------------------------------------------------


def recruit_korr():
    """Bind Korr to the watch: party + companion actor + toast.

    Idempotent: a second call reports the existing party. The hub NPC's live
    slot is captured as the companion's first-spawn anchor, then the NPC is
    retired (the static actor is destroyed and the manager stops tracking or
    respawning him, across savegames too), so the single resulting actor is
    the ``bd_dnd.Companion`` follower, proactive combat included. When the
    NPC is somehow gone the companion spawns beside the player instead. Its
    descriptor rides ``PartyState`` through checkpoints.
    """
    global party, party_state, korr_companion, korr_recruited
    if hero is None:
        bd.warn("ashvale: cannot recruit before the hero exists")
        return False
    if korr_recruited and party is not None:
        return True
    korr = content.build_korr()
    party = bd_dnd.Party([hero, korr], name=content.PARTY_NAME)
    party_state = bd_dnd.PartyState(party)
    korr_companion = bd_dnd.Companion(content.KORR_NAME,
                                      class_name=content.GAME_CONTENT[
                                          "companion_class"])
    party_state.add_companion(korr_companion)
    party_state.arm_persistence()
    korr_recruited = True
    anchor = None
    if manager is not None:
        try:
            npc_ref = manager.actor_for("korr")
            if npc_ref is not None and npc_ref.valid:
                anchor = (npc_ref.x, npc_ref.y, npc_ref.z, npc_ref.angle)
        except Exception as exc:
            bd.warn(f"ashvale: could not probe Korr's slot: {exc!r}")
            anchor = None
        try:
            manager.retire("korr")  # destroys the static hub actor
        except Exception as exc:
            bd.warn(f"ashvale: retiring the hub Korr failed: {exc!r}")
    spawned = korr_companion.bind(party, anchor=anchor)
    if spawned:
        try:
            ref = korr_companion.actor()
            if ref is not None:
                ref.tint = ((content.NPC_TINTS["korr"] >> 16) & 255,
                            (content.NPC_TINTS["korr"] >> 8) & 255,
                            content.NPC_TINTS["korr"] & 255)
        except Exception:
            pass
    # The watch grows: completing the objective pays the quest's rewards.
    try:
        watch = bd_quests.log.get(content.QUEST_WATCH_GROWS)
        if watch is not None and watch.state == bd_quests.Quest.ACTIVE:
            bd_quests.log.complete_objective(content.QUEST_WATCH_GROWS,
                                             "recruit_korr", 1)
    except Exception as exc:
        bd.warn(f"ashvale: watch_grows objective credit failed: {exc!r}")
    _toast_recruit()
    return True


def _toast_recruit():
    try:
        from bd_horror import toasts
        toasts.toast(content.TOAST_RECRUIT, kind="quest")
    except Exception:
        pass
    try:
        bd.center_message(content.CENTER_RECRUIT)
    except Exception:
        pass


# --- services ---------------------------------------------------------------------------


def service_ctx(npc_id):
    """The documented Service ctx dict (services.py module docstring)."""
    return {
        "pawn": player_pawn(),
        "character": hero,
        "dispositions": manager.dispositions if manager is not None else None,
        "manager": manager,
        "shop": dobb_shop if npc_id == "dobb" else None,
        "rng": bd,
    }


def run_healer():
    """Wren's heal service, doubled as the campaign's breather.

    Runs her HealerService (fee, standing floor, pawn + sheet healing)
    and, when the heal lands, restores the hero's class-active resource
    pools: her care is the "rest" the class-active charges refer to, so
    the no-charge message can point at her. Returns the result dict (the
    message extended on success); the broke refusal restores nothing.
    """
    global last_service_result
    definition = manager.definition("wren") if manager is not None else None
    if definition is None or not definition.services:
        last_service_result = {"ok": False, "message": "No such service."}
        return last_service_result
    result = definition.services[0].run("wren", service_ctx("wren"))
    if result.get("ok") and hero is not None:
        try:
            hero.restore_resources()
            result = dict(result)
            result["message"] = (str(result.get("message", "")).rstrip(".")
                                 + "; her care settles your breathing "
                                   "(active charges restored).")
        except Exception as exc:
            bd.warn(f"ashvale: healer resource restore failed: {exc!r}")
    last_service_result = result
    try:
        bd.center_message(str(result.get("message", "")))
    except Exception:
        pass
    return result


def run_service(npc_id, index, extra=None):
    """Run one registered service and surface its message; result dict."""
    global last_service_result
    definition = manager.definition(npc_id) if manager is not None else None
    if definition is None or not (0 <= int(index) < len(definition.services)):
        last_service_result = {"ok": False, "message": "No such service."}
        return last_service_result
    ctx = service_ctx(npc_id)
    if extra:
        ctx.update(extra)
    result = definition.services[index].run(npc_id, ctx)
    last_service_result = result
    try:
        bd.center_message(str(result.get("message", "")))
    except Exception:
        pass
    return result


# --- talk interaction ---------------------------------------------------------------------


def start_talk():
    """Open a session with the nearest talkable NPC (manager.begin_talk)."""
    global session
    if bd_dialogue.active_session() is not None:
        return None
    pawn = player_pawn()
    if pawn is None or manager is None:
        return None
    new_session = manager.begin_talk(pawn, character=hero,
                                     quest_log=bd_quests.log)
    if new_session is None:
        return None
    new_session.on_end.append(_on_session_ended)
    session = new_session
    return new_session


def current_session():
    """The example's live session (or the just-ended one), for the UI."""
    return session


def _on_session_ended(ended_session):
    global session, shop_open
    if session is ended_session:
        session = None
    shop_open = False


def talk():
    """The ui_command entry point: try to talk, else say why not."""
    if bd_dialogue.active_session() is not None:
        return
    if start_talk() is None:
        bd.center_message(f"[{action_key_hint(1)}] {content.NO_ONE_NEAR}")


# --- custom actions ---------------------------------------------------------------------


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
        bd.warn(f"ashvale: could not bind +pyaction{n}: {exc!r}")


def action_key_hint(n):
    """The live display name of the ``+pyactionN`` binding (or a fallback)."""
    try:
        name = bd.input_binding(f"+pyaction{n}")
    except Exception:
        name = None
    return name or f"Custom Action {n} (bind in Customize Controls)"


@bd.on("custom_action")
def on_custom_action(event):
    """Action 1 talks (the ui_command alias path), action 2 toggles the
    hero sheet through the same ``toggle()`` the K key and the
    ``toggle_sheet`` alias use, action 3 fires the hero's class active."""
    try:
        action = int(event.get("action") or 0)
        pressed = bool(event.get("pressed"))
        action_log.append((action, pressed))
        if not pressed:
            return
        if action == 1:
            talk()
        elif action == 2 and hero_sheet is not None:
            hero_sheet.toggle()
        elif action == 3:
            use_class_active()
    except Exception as exc:
        bd.warn(f"ashvale: custom action failed: {exc!r}")


# --- the yard ------------------------------------------------------------------------------


def spawn_yard_zombies():
    """Spawn the five risen dead (AMBUSH, so they hold until approached)."""
    spawned = []
    for tid, (x, y, z) in zip(content.YARD_TIDS, content.YARD_SPAWNS):
        try:
            ref = bd.spawn(content.GAME_CONTENT["yard_monster"], x, y, z,
                           angle=90.0, tid=tid, force=True)
            ref.set_flag("AMBUSH", True)
            spawned.append(ref)
        except Exception as exc:
            bd.warn(f"ashvale: yard spawn failed: {exc!r}")
    return spawned


def _on_clear_yard_started(quest):
    spawn_yard_zombies()


def _on_clear_yard_progress(quest, objective):
    """Kill-counter feedback on every yard objective tick."""
    try:
        bd.hud_text(f"Risen dead put down: {objective.progress}/"
                    f"{objective.count}", id=90013, y=0.28, color="gold",
                    hold=1.0, fade=0.4)
    except Exception:
        pass  # headless: no status bar to flash


# --- quest + reward sinks --------------------------------------------------------------------


def _xp_sink(amount, quest):
    if hero is None:
        return
    try:
        hero.award_xp(int(amount))
        bd.hud_text(f"+{int(amount)} XP ({quest.name})", id=90012, y=0.22,
                    color="gold", hold=1.5, fade=0.5)
    except Exception as exc:
        bd.warn(f"ashvale: xp reward failed: {exc!r}")


def _disposition_sink(npc_id, delta, quest):
    if manager is None:
        return
    try:
        manager.dispositions.shift(str(npc_id), int(delta))
    except Exception as exc:
        bd.warn(f"ashvale: disposition reward failed: {exc!r}")


def _wire_quests():
    quests = content.build_quests()
    clear = quests[content.QUEST_CLEAR_YARD]
    clear.on_start = _on_clear_yard_started
    clear.on_objective_progress = _on_clear_yard_progress
    for quest in quests.values():
        bd_quests.log.add(quest)
    bd_quests.log.track_kills(content.QUEST_CLEAR_YARD, "kill_zombies",
                              content.GAME_CONTENT["yard_monster"], 5)
    bd_quests.log.track_pickup(content.QUEST_FETCH_CACHE, "recover_cache",
                               content.GAME_CONTENT["cache_class"], 1)
    bd_quests.log.on_xp_reward.append(_xp_sink)
    bd_quests.log.on_disposition_reward.append(_disposition_sink)


# --- in-world markers + the objective HUD strip ------------------------------------


def _marker_clear(draw_id):
    """Drop one marker: clear the display-list item and the bookkeeping."""
    if draw_id not in marker_state:
        return
    marker_state.pop(draw_id, None)
    try:
        bd.draw_clear(draw_id)
    except Exception:
        pass  # headless or no display list: the bookkeeping still moved


def _marker_npc(npc_id, draw_id, show):
    """A gold "!" over a registered NPC while ``show`` holds."""
    ref = None
    if show and manager is not None:
        try:
            if not manager.is_retired(npc_id):
                ref = manager.actor_for(npc_id)
        except Exception:
            ref = None
        if ref is not None:
            try:
                if not (ref.valid and ref.alive):
                    ref = None
            except Exception:
                ref = None
    if ref is None:
        _marker_clear(draw_id)
        return
    try:
        bd.draw_world_text(ref, id=draw_id, text="!", color=content.MARKER_GOLD,
                           height=0.05, occlude=True, layer=8)
        marker_state[draw_id] = npc_id
    except Exception:
        _marker_clear(draw_id)


def _marker_beacon(pos, draw_id, tag, show):
    """A vertical gold beacon line over a fixed point while ``show`` holds."""
    if not show:
        _marker_clear(draw_id)
        return
    try:
        x, y, z = pos
        bd.draw_world_line((x, y, z), (x, y, z + 96.0), id=draw_id,
                           color=content.MARKER_GOLD, layer=8)
        marker_state[draw_id] = tag
    except Exception:
        _marker_clear(draw_id)


def refresh_markers():
    """Maintain the in-world quest markers (runs every 7 tics, map-local).

    A gold "!" floats over Sera while clear_yard waits to be taken, over
    Dobb while fetch_cache waits, and over Korr while watch_grows is open
    and he has not signed on (never over retired NPCs); a vertical beacon
    stands at the yard while clear_yard runs and at the cache while
    fetch_cache runs. Everything else is cleared.
    """
    try:
        clear = bd_quests.log.get(content.QUEST_CLEAR_YARD)
        fetch = bd_quests.log.get(content.QUEST_FETCH_CACHE)
        watch = bd_quests.log.get(content.QUEST_WATCH_GROWS)
    except Exception:
        return
    clear_state = clear.state if clear is not None else None
    fetch_state = fetch.state if fetch is not None else None
    watch_state = watch.state if watch is not None else None
    _marker_npc("sera", content.MARKER_SERA_ID,
                clear_state == bd_quests.Quest.INACTIVE)
    _marker_npc("dobb", content.MARKER_DOBB_ID,
                fetch_state == bd_quests.Quest.INACTIVE)
    _marker_npc("korr", content.MARKER_KORR_ID,
                watch_state == bd_quests.Quest.ACTIVE and not korr_recruited)
    _marker_beacon(content.YARD_CENTER, content.MARKER_YARD_BEACON_ID,
                   "yard_beacon", clear_state == bd_quests.Quest.ACTIVE)
    _marker_beacon(content.CACHE_POS, content.MARKER_CACHE_BEACON_ID,
                   "cache_beacon", fetch_state == bd_quests.Quest.ACTIVE)


def _arm_markers():
    """(Re)arm the marker refresh task on every map load (map-local)."""
    global _markers_task
    if _markers_task is not None:
        try:
            bd.cancel_task(_markers_task)
        except Exception:
            pass
        _markers_task = None
    try:
        _markers_task = bd.schedule(refresh_markers, delay=7, repeat=7,
                                    map_local=True)
    except Exception as exc:
        bd.warn(f"ashvale: marker task failed to arm: {exc!r}")
        _markers_task = None
    refresh_markers()  # land the bookkeeping immediately, not in 7 tics


def _note_quest_starts():
    """Record when each quest first reads ACTIVE (start order for the HUD).

    Quests start from dialogue effects and world events, so the tracker
    polls: an ACTIVE quest with no sequence number yet gets the next one.
    """
    global _quest_start_counter
    try:
        for quest in bd_quests.log.all():
            if quest.state == bd_quests.Quest.ACTIVE \
                    and quest.id not in _quest_start_seq:
                _quest_start_counter += 1
                _quest_start_seq[quest.id] = _quest_start_counter
    except Exception:
        pass


def _tracked_objective_text():
    """HUD line 2: the most recently started ACTIVE quest's current step."""
    try:
        best = None
        for quest in bd_quests.log.all():
            if quest.state != bd_quests.Quest.ACTIVE:
                continue
            obj = quest.current_objective
            if obj is None:
                continue
            seq = _quest_start_seq.get(quest.id, 0)
            if best is None or seq > best[0]:
                best = (seq, quest, obj)
    except Exception:
        return ""
    if best is None:
        return content.HUD_NO_OBJECTIVE
    _seq, quest, obj = best
    return f"{quest.name}: {obj.text} ({obj.progress}/{obj.count})"


def _refresh_hud_strip():
    """Redraw the persistent two-line bottom strip (runs every 35 tics)."""
    current = hero
    if current is None:
        line1 = content.HUD_PRE_FOUNDING
    else:
        try:
            next_xp = current.xp_for_next_level()
        except Exception:
            next_xp = None
        next_text = str(next_xp) if next_xp is not None else "MAX"
        spec = content.CLASS_ACTIVES.get(getattr(current, "class_id", ""))
        active_text = ""
        if spec:
            try:
                charges = int(current.resources.get(spec["resource"], 0))
            except Exception:
                charges = 0
            active_text = (f"  [{action_key_hint(3)}] {spec['name']} "
                           f"x{charges}")
        line1 = (f"{current.name} the {getattr(current, 'class_id', '?')}  "
                 f"Lv {current.level}  XP {current.xp}/{next_text}  "
                 f"Blood {current.hp}/{current.max_hp}{active_text}")
    _note_quest_starts()
    line2 = _tracked_objective_text()
    try:
        bd.draw_text(line1, id=content.HUD_STRIP_STATUS_ID, x=0.01, y=0.90,
                     color="gold", height=0.022, shadow=True, layer=8)
        bd.draw_text(line2, id=content.HUD_STRIP_OBJECTIVE_ID, x=0.01,
                     y=0.93, color=(230, 230, 230), height=0.02,
                     shadow=True, layer=8)
    except Exception:
        pass  # headless or no display list: the strip simply does not draw


def _arm_hud_strip():
    """(Re)arm the HUD strip refresh task; exactly one ever lives.

    Scheduled tasks are not savegame state and the engine may drop them
    across a map change, so map_load re-arms: cancel the old id (if any)
    and schedule fresh. Called from engine_start and on_map.
    """
    global _hud_task
    if _hud_task is not None:
        try:
            bd.cancel_task(_hud_task)
        except Exception:
            pass
        _hud_task = None
    try:
        _hud_task = bd.schedule(_refresh_hud_strip, delay=35, repeat=35,
                                map_local=False)
    except Exception as exc:
        bd.warn(f"ashvale: HUD strip task failed to arm: {exc!r}")
        _hud_task = None
    _refresh_hud_strip()  # draw the strip immediately, not in 35 tics


# --- currency probe -------------------------------------------------------------------------


def _probe_currency():
    """Spawn the preferred currency class at a far corner and destroy it.

    On failure fall back to the next candidate (and log the choice). GZDoom
    ships the Strife Coin in the base zscript, so Doom II runs usually probe
    OK; the fallback keeps the economy alive where the class is absent.
    """
    global currency_class, currency_ready
    if currency_ready:
        return currency_class
    chosen = None
    for candidate in content.CURRENCY_CLASSES:
        try:
            ref = bd.spawn(candidate, *content.COIN_PROBE_POS, force=True)
            ref.destroy()
            chosen = candidate
            break
        except Exception:
            continue
    if chosen is None:  # every candidate failed: stay with the first
        chosen = content.CURRENCY_CLASSES[0]
    currency_class = chosen
    if currency_class == content.CURRENCY_CLASSES[0]:
        bd.log(content.CURRENCY_OK_LOG % currency_class)
    else:
        bd.log(content.CURRENCY_FALLBACK_LOG
               % (content.CURRENCY_CLASSES[0], currency_class))
    _apply_currency()
    currency_ready = True
    return currency_class


def _apply_currency():
    if dobb_shop is not None:
        dobb_shop.currency_class = currency_class
        dobb_shop.currency_name = content.CURRENCY_NAMES.get(currency_class,
                                                             currency_class)
    for services in _services.values():
        for service in services:
            if hasattr(service, "currency_class"):
                service.currency_class = currency_class


# --- savegame cold restore --------------------------------------------------------------------


def _cold_restore():
    """Rebuild hero/party from ``bd.state`` when none exist (a loaded game).

    The engine's ``load`` event fires before ``map_load``; in-process heroes
    are restored by their armed CharacterState/PartyState handlers, so this
    only runs for a genuinely fresh session that loaded a savegame.
    """
    global hero, hero_state, party, party_state, korr_companion
    global korr_recruited
    if hero is None:
        saved = bd.state.get(bd_dnd.STATE_KEY)
        if isinstance(saved, dict) and isinstance(saved.get("character"), dict):
            hero = bd_dnd.Character(str(saved["character"].get(
                "name", content.HERO_DEFAULT_NAME)))
            hero.on_level_up.append(_on_hero_level_up)
            hero_state = bd_dnd.CharacterState(hero)
            hero_state.restore(saved)
            hero.cls = content.CLASSES_BY_ID.get(
                getattr(hero, "class_id", ""))
            if hero.cls is not None:
                hero.on_level_up.append(_class_level_hook)
            hero_state.arm_persistence()
            wire_hero_progression(hero)
            bd.log(f"ashvale: restored {hero.name} (level {hero.level}) "
                   f"from the save")
    saved_party = bd.state.get(bd_dnd.PARTY_STATE_KEY)
    if party is None and hero is not None and \
            isinstance(saved_party, dict) and saved_party.get("party"):
        party = bd_dnd.Party([hero, content.build_korr()],
                             name=content.PARTY_NAME)
        party_state = bd_dnd.PartyState(party)
        korr_companion = bd_dnd.Companion(
            content.KORR_NAME,
            class_name=content.GAME_CONTENT["companion_class"])
        party_state.add_companion(korr_companion)
        party_state.arm_persistence()
        party_state.restore(saved_party)
        korr_companion.bind(party, spawn_near_player=False)
        korr_recruited = True


# --- the shop (with a worked-around framework bug) -----------------------------------------


class AshvaleShop(bd_npcs.Shop):
    """Dobb's store: the stock/spec live in content; only the delivery
    check differs from the framework.

    WORKAROUND (framework bug, reported): ``bd_npcs.Shop.buy`` treats
    ``give_inventory``'s return value as the delivery signal, but the native
    pickup path consumes health/armor items and folds ammo subclasses into
    their parent ammo (``ClipBox : Clip``), so a give that actually landed
    reports 0 and the framework refunds a successful purchase. Delivery is
    therefore signaled here by the give NOT raising (the only true failure
    an unknown item class), mirroring the framework's guards, restock
    scheduling, and messages otherwise.
    """

    def buy(self, pawn, index):
        index = int(index)
        if not 0 <= index < len(self.stock):
            raise ValueError(f"bd_npcs: shop index {index} out of range "
                             f"({len(self.stock)} entries)")
        entry = self.stock[index]
        name = entry["name"]
        price = self.price_of(entry)
        if self._counts[index] <= 0:
            return {"ok": False, "message": f"{name} is out of stock.",
                    "price": price}
        if self.currency(pawn) < price:
            return {"ok": False,
                    "message": f"You cannot afford {name} ({price} "
                               f"{self.currency_name}).",
                    "price": price}
        try:
            pawn.take_inventory(self.currency_class, price)
        except Exception as exc:
            return {"ok": False, "message": f"Payment failed: {exc}.",
                    "price": price}
        try:
            pawn.give_inventory(entry["class_name"], 1)
        except Exception as exc:
            # Unknown item class is a mod bug: refund and report.
            try:
                pawn.give_inventory(self.currency_class, price)
            except Exception:
                pass
            return {"ok": False,
                    "message": f"Cannot stock {name}: {exc}.",
                    "price": price}
        self._counts[index] -= 1
        if entry["restock_tics"] > 0:
            try:
                bd.schedule(lambda i=index: self._restock(i),
                            delay=entry["restock_tics"], map_local=True)
            except Exception as exc:
                bd.warn(f"bd_npcs: restock scheduling failed: {exc!r}")
        try:
            bd.play_ui_sound("menu/change")
        except Exception:
            pass
        return {"ok": True,
                "message": f"Bought {name} for {price} "
                           f"{self.currency_name}.",
                "price": price}


# --- event wiring -------------------------------------------------------------------------

_engine_started = False


@bd.on("engine_start")
def setup_campaign(event):
    global wizard, manager, dobb_shop, _engine_started
    if _engine_started:
        return
    _engine_started = True
    bd.imgui.set_master_visible(True)
    bd.log(f"bd_dnd shipped from: {bd_dnd.__file__}")
    bd.log(f"bd_npcs shipped from: {bd_npcs.__file__}")
    bd.log(f"bd_quests shipped from: {bd_quests.__file__}")
    bd.log(f"bd_dialogue shipped from: {bd_dialogue.__file__}")

    wizard = bd_dnd.CreationWizard(rng=bd)

    # The quartermaster's economy and Wren's services. Currency class is
    # patched after the first map-load spawn probe (see _probe_currency).
    dobb_shop = AshvaleShop(
        currency_class=content.CURRENCY_CLASSES[0],
        currency_name=content.CURRENCY_NAMES[content.CURRENCY_CLASSES[0]],
        stock=[dict(entry) for entry in content.SHOP_STOCK],
        sellables=content.SHOP_SELLABLES,
    )
    _services["dobb"] = ()
    _services["wren"] = (
        bd_npcs.HealerService(cost=content.HEALER_COST,
                              min_standing=content.HEALER_MIN_STANDING),
        bd_npcs.TrainerService(skill=content.TRAINER_SKILL,
                               cost=content.TRAINER_COST),
    )

    manager = bd_npcs.NPCManager()
    for npc_id in ("sera", "dobb", "wren", "korr"):
        services = _services.get(npc_id, ())
        manager.register(bd_npcs.NPCDefinition(
            npc_id, content.NPC_DISPLAY_NAMES[npc_id],
            content.GAME_CONTENT[npc_id + "_class"],
            tint=content.NPC_TINTS[npc_id],
            dialogue=content.DIALOGUE_FACTORIES[npc_id],
            services=services,
            start_disposition=0,
            talk_range=content.TALK_RANGE,
            spawn_offset=content.NPC_SPAWNS[npc_id],
            tid_base=content.NPC_TID_BASE))
    manager.dispositions.arm_persistence()
    manager.arm_persistence()

    _wire_quests()

    # Shop stock counts persist next to the frameworks' state.
    @bd.on("save")
    def _on_save(event):
        try:
            if dobb_shop is not None:
                bd.state[content.SHOP_STATE_KEY] = \
                    dobb_shop.stock_snapshot()
        except Exception as exc:
            bd.warn(f"ashvale: shop snapshot failed: {exc!r}")

    @bd.on("load")
    def _on_load(event):
        try:
            if dobb_shop is not None:
                dobb_shop.restore(bd.state.get(content.SHOP_STATE_KEY))
        except Exception as exc:
            bd.warn(f"ashvale: shop restore failed: {exc!r}")
        _cold_restore()

    # Console aliases through the pyui/ui_command bridge; the keys live on
    # Custom Actions 1 (talk), 2 (sheet), and 3 (class active), auto-bound
    # below and rebindable in Options -> Customize Controls, Custom Actions.
    bd.execute('alias talk "pyui talk"')
    bd.execute('alias class_active "pyui class_active"')
    ensure_custom_action_binding(1, "q")
    ensure_custom_action_binding(2, "v")
    ensure_custom_action_binding(3, "c")
    _arm_hud_strip()


@bd.on("ui_command")
def on_ui_command(event):
    command = event.get("command")
    if command == "talk":
        talk()
    elif command == "class_active":
        use_class_active()


@bd.on("map_load")
def on_map(event):
    global shop_open
    shop_open = False
    _probe_currency()
    from_savegame = bool(event.get("from_savegame"))
    if manager is not None:
        manager.spawn_all(from_savegame=from_savegame)
    _arm_markers()
    _arm_hud_strip()
    if from_savegame:
        return
    pawn = player_pawn()
    if pawn is None:
        return
    # The buried cache sits at the west end of the entry hall; Dobb's
    # dialogue starts the fetch quest (no silent auto-start).
    try:
        bd.spawn(content.GAME_CONTENT["cache_class"], *content.CACHE_POS,
                 angle=0.0, tid=content.CACHE_TID, force=True)
    except Exception as exc:
        bd.warn(f"ashvale: cache spawn failed: {exc!r}")
    # Returning to the hub mid-quest: the yard fills back up.
    clear = bd_quests.log.get(content.QUEST_CLEAR_YARD)
    if clear is not None and clear.state == bd_quests.Quest.ACTIVE:
        spawn_yard_zombies()


@bd.on("map_unload")
def on_map_unload(event):
    # Sessions are transient; the NPC handle goes stale across the unload
    # and any active conversation auto-ends itself.
    global session, shop_open
    session = None
    shop_open = False
    discard_creation_pause()
    _reset_blur()


# --- sibling-import registration ----------------------------------------------------

import sys as _sys
import types as _types


class _LiveAlias(_types.ModuleType):
    """sys.modules alias that reads/writes through to the module's live
    globals (the engine registers manifest modules under a mangled name only
    after execution, so the real module object cannot be aliased from inside
    itself)."""

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


_sys.modules.setdefault("ashvale_systems",
                        _LiveAlias("ashvale_systems", globals()))
del _sys, _types
