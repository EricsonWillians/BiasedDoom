"""Whispers in the Walls — systems: event wiring, directors, game rules.

Everything here is *definition-only* at import time; main.py calls
:func:`build_world` / :func:`setup_engine` / :func:`setup_map` exactly
once and owns the actual event registration order. The module builds
live objects from the pure tables in content.py:

- the three chained quests and their ``bd_quests`` trackers
  (pickup / sector / player-attributed kills), with the log's
  ``on_xp_reward`` sink crediting the example's favor ledger,
- the :class:`RiteDirector`, which fails the rite when the player steps
  out of the circle mid-wave (the campaign's fail branch),
- the ``bd_horror`` atmosphere: a blackout plus fluorescent flicker on
  the ritual sector when the circle is entered, and a StalkerDirector
  that is enabled only while the rite runs.
"""

import math

import biaseddoom as bd
import bd_quests
from bd_horror import toasts
from bd_horror.atmosphere import HorrorState


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


class World:
    """One run of the campaign: quest log, horror state, and test probes."""

    def __init__(self):
        self.log = bd_quests.log
        self.horror = HorrorState()
        self.director = None            # RiteDirector, set by setup_engine
        self.ritual_light_before = None  # captured the moment the rite begins
        self.blackout_armed = False
        self.flicker_armed = False
        self.choir_spawned = False
        self.probe_log = None           # fail-branch probe log (autotest)
        # The example-local XP ledger: quest id -> XP awarded through the
        # log's on_xp_reward sink (no character rules engine here; the
        # running total is the campaign's light progression counter and
        # feeds a Grimoire footer line plus a favor toast).
        self.xp_ledger = {}


class RiteDirector:
    """Watches the circle while the kill wave runs; abandoning it fails the rite.

    Rite activity is *derived* from quest state — the rite is live exactly
    while the quest is ACTIVE and its wave objective is the current one —
    so the flag needs no persistence of its own: the ``bd_quests``
    checkpoint round-trip restores it for free. ``bind_events`` is called
    exactly once (for the production director); autotest probes drive
    :meth:`on_sector_exited` directly with a synthetic event.
    """

    def __init__(self, log, quest_id, tag, enter_obj="enter_circle",
                 wave_obj="silence_dead", fail_reason="", fail_toast=""):
        self.log = log
        self.quest_id = str(quest_id)
        self.tag = int(tag)
        self.enter_obj = enter_obj
        self.wave_obj = wave_obj
        self.fail_reason = fail_reason
        self.fail_toast = fail_toast

    def rite_active(self):
        quest = self.log.get(self.quest_id)
        if quest is None or quest.state != bd_quests.Quest.ACTIVE:
            return False
        current = quest.current_objective
        return current is not None and current.id == self.wave_obj

    def on_sector_exited(self, event):
        """Fail the rite if the player leaves the circle mid-wave."""
        try:
            tags = [int(tag) for tag in (event.get("tags") or [])]
        except (TypeError, ValueError):
            return False
        if self.tag not in tags or not self.rite_active():
            return False
        if self.log.fail_quest(self.quest_id, self.fail_reason):
            toasts.toast(self.fail_toast, kind="omen")
            return True
        return False

    def bind_events(self):
        director = self

        @bd.on("sector_exited")
        def _on_sector_exited(event):
            director.on_sector_exited(event)


# --- construction ------------------------------------------------------------------


def build_world():
    return World()


def setup_engine(world, content):
    """Register quests, trackers, hooks, and directors. Call from engine_start."""
    log = world.log
    for quest in content.build_quests():
        log.add(quest)

    pages = log.get("pages")
    rite = log.get("rite")
    choir = log.get("choir")

    pages.on_objective_progress = lambda q, o: toasts.toast(
        content.TOAST_PAGE_PROGRESS.format(p=o.progress, n=o.count),
        kind="loot")
    pages.on_complete = lambda q: _on_pages_complete(world, content)

    def on_rite_objective_complete(quest, obj):
        if obj.id == "enter_circle":
            begin_rite(world, content)

    rite.on_objective_complete = on_rite_objective_complete
    rite.on_objective_progress = lambda q, o: (
        toasts.toast(content.TOAST_WAVE_PROGRESS.format(p=o.progress,
                                                        n=o.count),
                     kind="info") if o.id == "silence_dead" else None)
    rite.on_complete = lambda q: _on_rite_complete(world, content)
    rite.on_fail = lambda q: _on_rite_fail(world, content)

    choir.on_complete = lambda q: toasts.toast(content.TOAST_CHOIR_DONE,
                                               kind="quest")

    # Quest xp rewards: the log's on_xp_reward sink credits the example's
    # favor ledger (quest id -> XP) and toasts the running total. No
    # character rules engine in this example; the ledger IS the
    # progression counter, and the Grimoire renders it.
    def on_xp_reward(amount, quest):
        amount = int(amount)
        world.xp_ledger[quest.id] = world.xp_ledger.get(quest.id, 0) + amount
        total = sum(world.xp_ledger.values())
        toasts.toast(content.TOAST_XP_REWARD.format(amount=amount,
                                                    total=total),
                     kind="quest")

    log.on_xp_reward.append(on_xp_reward)

    # Auto-wiring. track_kills keeps its default killer="player": only
    # player-sourced kills count (exact credit via actor_died's
    # attacker_player_index).
    log.track_pickup("pages", "recover_pages", content.PAGE_CLASS,
                     len(content.PAGE_TIDS))
    log.track_sector("rite", "enter_circle", [content.RITUAL_TAG])
    log.track_kills("rite", "silence_dead", content.WAVE_CLASS,
                    len(content.WAVE_TIDS))
    log.track_kills("choir", "silence_choir", content.CHOIR_CLASS, 1)

    world.director = RiteDirector(
        log, "rite", content.RITUAL_TAG,
        fail_reason=content.row("rite")["fail_reason"],
        fail_toast=content.TOAST_RITE_FAILED)
    world.director.bind_events()
    world.horror.arm_persistence()


# --- map setup ---------------------------------------------------------------------


def setup_map(world, content, event):
    """Arm per-map systems; spawn the cast on fresh maps only."""
    world.horror.start()  # dread tick + stalker windows (stalker gated)
    if event.get("from_savegame"):
        _reapply_tints(world, content)  # tints are not serialized
        return
    pawn = player_pawn()
    if pawn is None:
        return
    _spawn_pages(world, content, pawn)
    quest = world.log.get("pages")
    if quest is not None:
        quest.start()  # the rite and the Choir stay sealed for now


# --- the rite ----------------------------------------------------------------------


def begin_rite(world, content):
    """The circle is entered: lights out, the dead rise, the stalker wakes."""
    sector = None
    try:
        sector = bd.sector_at(content.RITUAL_SPOT[0], content.RITUAL_SPOT[1])
    except Exception:
        pass
    if sector is not None:
        try:
            world.ritual_light_before = int(sector.light)
        except Exception:
            world.ritual_light_before = None
    world.horror.lights.blackout([content.RITUAL_TAG],
                                 duration_tics=content.BLACKOUT_TICS)
    world.blackout_armed = True
    toasts.toast(content.TOAST_RITE_ENTERED, kind="omen")
    _spawn_wave(world, content)
    world.horror.stalker.enabled = True

    def arm_flicker():
        world.horror.lights.fluorescent([content.RITUAL_TAG],
                                        dropout_chance=0.12)
        world.flicker_armed = True

    # The flicker takes over once the blackout has restored the lights.
    bd.schedule(arm_flicker, delay=content.FLICKER_DELAY_TICS, map_local=True)


def _on_pages_complete(world, content):
    quest = world.log.get("rite")
    if quest is not None:
        quest.start()
    toasts.toast(content.TOAST_PAGES_DONE, kind="quest")


def _on_rite_complete(world, content):
    world.horror.stalker.enabled = False
    _spawn_choir(world, content)
    quest = world.log.get("choir")
    if quest is not None:
        quest.start()
    toasts.toast(content.TOAST_RITE_DONE, kind="omen")


def _on_rite_fail(world, content):
    world.horror.stalker.enabled = False
    world.horror.lights.clear()  # restores the circle's original light


# --- spawning ----------------------------------------------------------------------


def _spawn_pages(world, content, pawn):
    for i, tid in enumerate(content.PAGE_TIDS):
        x, y, z = content.PAGE_POSITIONS[i]
        bd.spawn(content.PAGE_CLASS, x, y, z, tid=tid, force=True)


def _spawn_wave(world, content):
    x0, y0, z0 = content.RITUAL_SPOT
    for i, tid in enumerate(content.WAVE_TIDS):
        angle = math.radians(i * 120.0 + 30.0)
        ref = bd.spawn(content.WAVE_CLASS, x0 + math.cos(angle) * content.WAVE_RADIUS,
                       y0 + math.sin(angle) * content.WAVE_RADIUS, z0,
                       angle=math.degrees(angle) + 180.0, tid=tid, force=True)
        if ref is not None:
            try:
                ref.tint = content.WAVE_TINT
            except Exception:
                pass


def _spawn_choir(world, content):
    x0, y0, _ = content.RITUAL_SPOT
    for dx, dy in content.CHOIR_OFFSETS:
        x, y = x0 + dx, y0 + dy
        try:
            sector = bd.sector_at(x, y)
        except Exception:
            sector = None
        if sector is None:
            continue
        ref = bd.spawn(content.CHOIR_CLASS, x, y, float(sector.floor_height),
                       tid=content.CHOIR_TID, force=True)
        if ref is not None:
            try:
                ref.tint = content.CHOIR_TINT
                bd.play_ui_sound("baron/sight", volume=0.8)
            except Exception:
                pass
            world.choir_spawned = True
            return ref
    return None


def _reapply_tints(world, content):
    """Tint tables are not serialized; repaint the cast after a load."""
    for tid in content.WAVE_TIDS:
        ref = bd.actor_ref(tid)
        if ref is not None and ref.valid:
            try:
                ref.tint = content.WAVE_TINT
            except Exception:
                pass
    ref = bd.actor_ref(content.CHOIR_TID)
    if ref is not None and ref.valid:
        try:
            ref.tint = content.CHOIR_TINT
        except Exception:
            pass
