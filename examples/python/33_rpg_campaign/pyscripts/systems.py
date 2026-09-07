"""Ashvale Crossing - systems: rules, state, and every engine hook.

Owns the campaign's behavior:

- **Character creation.** The shared ``CreationWizard`` (built at
  ``engine_start``) drives the UI window; :func:`finish_creation` validates
  and builds the hero, arms ``CharacterState`` persistence, hands out the
  class starting equipment, and wires level-up feedback.
- **The hub.** The ``NPCManager`` registration (definitions carry the content
  dialogue factories and the shop/services), the currency spawn probe, the
  savegame cold-restore path for the hero and party, and the shop stock
  snapshot persistence.
- **Quest wiring.** Trackers, the xp sink into the hero, the disposition
  sink into the store, the yard-zombie spawn on acceptance, and the
  prove-worth completion that rides on clear_yard.
- **Talk interaction.** ``bind e talk`` -> ``pyui talk`` -> ``ui_command``
  -> :func:`start_talk`, which opens a disposition-injecting
  ``bd_dialogue.DialogueSession`` with the nearest talkable NPC.

Manifest entry 2 of 4 (after content.py).
"""

import biaseddoom as bd
import bd_dialogue
import bd_dnd
import bd_npcs
import bd_quests

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
shop_open = False           # the ShopUI window toggle (set by Dobb's trade)
korr_recruited = False      # plain flag; recruit_korr is idempotent
currency_class = content.CURRENCY_CLASSES[0]
currency_ready = False      # the spawn probe ran
last_service_result = None  # {"ok", "message"} of the last service run


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
    return hero


# --- recruitment ------------------------------------------------------------------------


def recruit_korr():
    """Bind Korr to the watch: party + companion actor + toast.

    Idempotent: a second call reports the existing party. The companion is a
    ``bd_dnd.Companion`` follower (the same actor class and tint as the camp
    NPC); its descriptor rides ``PartyState`` through checkpoints.
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
    spawned = korr_companion.bind(party)
    if spawned:
        try:
            ref = korr_companion.actor()
            if ref is not None:
                ref.tint = ((content.NPC_TINTS["korr"] >> 16) & 255,
                            (content.NPC_TINTS["korr"] >> 8) & 255,
                            content.NPC_TINTS["korr"] & 255)
        except Exception:
            pass
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
        bd.center_message(content.NO_ONE_NEAR)


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


def _on_clear_yard_completed(quest):
    prove = bd_quests.log.get(content.QUEST_PROVE_WORTH)
    if prove is not None and prove.state == bd_quests.Quest.ACTIVE:
        bd_quests.log.complete_objective(content.QUEST_PROVE_WORTH,
                                         "yard_cleared", 1)


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
    clear.on_complete = _on_clear_yard_completed
    for quest in quests.values():
        bd_quests.log.add(quest)
    bd_quests.log.track_kills(content.QUEST_CLEAR_YARD, "kill_zombies",
                              content.GAME_CONTENT["yard_monster"], 5)
    bd_quests.log.track_pickup(content.QUEST_FETCH_CACHE, "recover_cache",
                               content.GAME_CONTENT["cache_class"], 1)
    bd_quests.log.on_xp_reward.append(_xp_sink)
    bd_quests.log.on_disposition_reward.append(_disposition_sink)


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

    # Console alias + key bind through the pyui/ui_command bridge.
    bd.execute('alias talk "pyui talk"')
    bd.execute("bind e talk")


@bd.on("ui_command")
def on_ui_command(event):
    if event.get("command") != "talk":
        return
    talk()


@bd.on("map_load")
def on_map(event):
    global shop_open
    shop_open = False
    _probe_currency()
    from_savegame = bool(event.get("from_savegame"))
    if manager is not None:
        manager.spawn_all(from_savegame=from_savegame)
    if from_savegame:
        return
    pawn = player_pawn()
    if pawn is None:
        return
    # The buried cache sits at the west end of the entry hall.
    try:
        bd.spawn(content.GAME_CONTENT["cache_class"], *content.CACHE_POS,
                 angle=0.0, tid=content.CACHE_TID, force=True)
    except Exception as exc:
        bd.warn(f"ashvale: cache spawn failed: {exc!r}")
    # The fetch quest is ambient (a crate visible in the hall from tic 0);
    # savegame loads keep their restored state instead.
    fetch = bd_quests.log.get(content.QUEST_FETCH_CACHE)
    if fetch is not None and fetch.state == bd_quests.Quest.INACTIVE:
        fetch.start()
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
