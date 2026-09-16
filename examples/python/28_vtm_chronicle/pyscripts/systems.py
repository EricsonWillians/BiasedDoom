"""The Last Feeding — systems: chronicle rules, directors, event wiring.

Definition-only at import time; main.py calls :func:`build_chronicle` and
:func:`setup_map` exactly once. Everything builds live objects from the
pure tables in content.py on top of the shipped ``bd_vtm`` / ``bd_quests``
/ ``bd_horror`` packages.

On top of the stock bd_vtm rules (blood pool, hunger, humanity,
disciplines, feeding, masquerade, factions, gated quests) this module
adds the hunt layer:

- **cowering mortals** — vessels who see the predator within
  ``COWER_RADIUS`` scramble away on a slow scheduled thrust,
- **hunger <-> dread** — the bd_horror dread meter mirrors the hunger
  track (``DREAD_PER_HUNGER`` per level),
- **darkness-aware feeding** — below ``DARK_LIGHT`` sector light the
  feeding witness radius is halved,
- **the answering pack** — a Masquerade breach spawns a Sabbat ambush
  around the player, out of sight where possible,
- **frenzy vignette storm** — while the Beast drives, the screen pulses
  red and the heartbeat runs at maximum rate.
"""

import math

import biaseddoom as bd
import bd_quests
from bd_quests import Objective, Quest
import bd_vtm
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


def nearest_living(class_name, pawn):
    """Nearest living actor of a class to the pawn, or None."""
    best = None
    best_dist = None
    try:
        refs = bd.actor_refs(class_name)
    except Exception:
        return None
    for ref in refs:
        try:
            if not ref.alive:
                continue
            dist = ref.distance_to(pawn)
        except Exception:
            continue
        if best is None or dist < best_dist:
            best, best_dist = ref, dist
    return best


class Roller:
    """Scripted rng test double: yields clamped values in order."""

    def __init__(self, values):
        self._values = list(values)
        self._index = 0

    def randint(self, lo, hi):
        value = self._values[self._index % len(self._values)]
        self._index += 1
        return max(lo, min(hi, value))


class Chronicle:
    """One run of the chronicle: VtM state, horror state, and the hunt."""

    def __init__(self, generation=13):
        self.state = bd_vtm.VtMState(generation=generation)
        self.horror = HorrorState()
        self.gated_quest = None      # GatedQuest wrapping "street_cred"
        self.ambush_log = []         # one dict per answering pack
        self.ambush_last = -10 ** 9
        self.cower_log = []          # one dict per cower thrust (autotests)
        self.cower_task = None
        self.storm = {"active": False, "pulses": 0, "task": None}
        self.log = bd_quests.log

    # -- input-path actions (used by the discipline panel) ----------------------

    def dominate_nearest(self, pawn):
        target = nearest_living("ShotgunGuy", pawn)
        return self.state.disciplines.get("dominate").use(pawn, target)

    def feed_nearest(self, pawn, content):
        victim = nearest_living(content.MORTAL_CLASS, pawn)
        if victim is None:
            toasts.toast(content.TOAST_FEED_NOBODY, kind="info")
            return None
        return feed(self, pawn, victim, content)

    def frenzy_check(self):
        return self.state.frenzy_check()

    def storm_active(self):
        return bool(self.storm["active"])


# --- construction ---------------------------------------------------------------------


def build_chronicle(content):
    """Build the chronicle and wire every rule. Call from engine_start."""
    chron = Chronicle(generation=13)
    state = chron.state

    state.register_discipline(bd_vtm.celerity(level=1))
    state.register_discipline(bd_vtm.obfuscate())
    state.register_discipline(bd_vtm.potence(level=1))
    state.register_discipline(bd_vtm.dominate())
    state.arm_persistence()
    chron.horror.arm_persistence()

    # Hunger drives dread: every blood-pool change re-derives hunger (the
    # state's own callback runs first), and dread follows.
    def on_blood(pool):
        chron.horror.dread.set_level(state.hunger.level
                                     * content.DREAD_PER_HUNGER)

    state.blood.on_blood_changed.append(on_blood)

    for name, dispositions in content.FACTIONS:
        state.factions.add(bd_vtm.Faction(name, dict(dispositions)))

    def on_frenzy(hunger, roll):
        bd.center_message(content.MSG_FRENZY)
        toasts.toast(content.TOAST_FRENZY, kind="harm")
        start_frenzy_storm(chron, content)

    state.hunger.on_frenzy.append(on_frenzy)
    state.masquerade.on_breach.append(
        lambda masq, reason: _ambush(chron, content, reason))

    log = chron.log
    first_spec, cred_spec = content.QUESTS
    first = Quest(first_spec["id"], first_spec["name"],
                  description=first_spec["description"],
                  giver=first_spec["giver"], faction=first_spec["faction"])
    for obj_id, text, count in first_spec["objectives"]:
        first.add_objective(Objective(obj_id, text, count=count))
    first.rewards["message"] = first_spec["reward_message"]
    log.add(first)
    log.track_kills("first_night", "put_down_pack", content.SABBAT_CLASS, 3)

    cred = Quest(cred_spec["id"], cred_spec["name"],
                 description=cred_spec["description"],
                 giver=cred_spec["giver"], faction=cred_spec["faction"])
    for obj_id, text, count in cred_spec["objectives"]:
        cred.add_objective(Objective(obj_id, text, count=count))
    cred.rewards["message"] = cred_spec["reward_message"]
    log.add(cred)
    chron.gated_quest = bd_vtm.requires_faction(cred, state.factions,
                                                "Anarchs", min_standing=1)

    def on_first_night_complete(quest):
        state.factions.change_reputation("Anarchs", 1,
                                         "survived the first night")
        bd.center_message(content.MSG_GATE_OPEN)
        toasts.toast(content.TOAST_FIRST_NIGHT_DONE, kind="quest")
        chron.gated_quest.start()

    first.on_complete = on_first_night_complete
    return chron


# --- map setup -------------------------------------------------------------------------


def setup_map(chron, content, event):
    """Arm per-map systems; spawn the herd and the pack on fresh maps only."""
    chron.state.start_systems()
    chron.horror.start()
    start_cowardice(chron, content)
    if chron.storm["active"]:
        # A load mid-storm cancels the pulse task; the Beast is spent.
        chron.storm["active"] = False
        chron.storm["task"] = None

    quest = chron.log.get("first_night")
    if quest is not None:
        quest.start()
    if chron.gated_quest is not None:
        chron.gated_quest.start()  # blocked until the Anarchs trust you

    pawn = player_pawn()
    if pawn is None:
        return

    if event.get("from_savegame"):
        _reapply_tints(chron, content)  # tints are not serialized
        return

    angle = math.radians(pawn.angle)
    fx, fy = math.cos(angle), math.sin(angle)
    sx, sy = -fy, fx
    # The mortal herd: harmless tinted zombiemen standing in as vessels.
    for i, spec in enumerate(content.MORTALS):
        side = (i - 1) * 72.0
        mortal = bd.spawn(spec["cls"], pawn.x + fx * 176.0 + sx * side,
                          pawn.y + fy * 176.0 + sy * side, pawn.z,
                          angle=(pawn.angle + 180.0) % 360.0,
                          tid=spec["tid"], force=True)
        try:
            mortal.tint = spec["tint"]
            mortal.damage_multiply = 0.0    # stand-ins: harmless
        except Exception:
            pass
    # The Sabbat pack: dark-red shotgun guys, fully hostile.
    for i, spec in enumerate(content.SABBAT):
        side = (i - 1) * 80.0
        shovelhead = bd.spawn(spec["cls"], pawn.x + fx * 352.0 + sx * side,
                              pawn.y + fy * 352.0 + sy * side, pawn.z,
                              angle=(pawn.angle + 180.0) % 360.0,
                              tid=spec["tid"], force=True)
        try:
            shovelhead.tint = content.SABBAT_TINT
        except Exception:
            pass


def _reapply_tints(chron, content):
    """Tint tables are not serialized; repaint the cast after a load."""
    for spec in content.MORTALS:
        ref = bd.actor_ref(spec["tid"])
        if ref is not None and ref.valid:
            try:
                ref.tint = spec["tint"]
            except Exception:
                pass
    for tid in list(content.SABBAT_TIDS) + list(content.AMBUSH_TIDS):
        ref = bd.actor_ref(tid)
        if ref is not None and ref.valid:
            try:
                ref.tint = (content.SABBAT_TINT if tid in content.SABBAT_TIDS
                            else content.AMBUSH_TINT)
            except Exception:
                pass


# --- darkness-aware feeding -------------------------------------------------------------


def witness_radius_for(predator, content):
    """The effective feeding witness radius around a predator right now.

    Darkness keeps secrets: when the predator's own sector is darker than
    ``DARK_LIGHT``, the radius shrinks by ``WITNESS_DARK_FACTOR``.
    """
    light = 255
    try:
        sector = bd.sector_at(predator.x, predator.y)
        if sector is not None:
            light = int(sector.light)
    except Exception:
        pass
    if light < content.DARK_LIGHT:
        return content.WITNESS_RADIUS_LIT * content.WITNESS_DARK_FACTOR
    return content.WITNESS_RADIUS_LIT


def feed(chron, predator, victim, content, drain=5, blood_gain=1):
    """Feed through bd_vtm.feed with the darkness-adjusted witness radius.

    Returns bd_vtm.feed's result dict plus ``"radius"`` (the witness
    radius actually used) so autotests can assert the darkness math.
    """
    radius = witness_radius_for(predator, content)
    result = bd_vtm.feed(predator, victim, chron.state.blood,
                         humanity=chron.state.humanity,
                         masquerade=chron.state.masquerade,
                         drain=drain, blood_gain=blood_gain,
                         witness_radius=radius)
    result["radius"] = radius
    if result["killed"]:
        name = content.mortal_name(getattr(victim, "tid", None) or -1)
        toasts.toast(f"{content.TOAST_FEED_KILL} ({name})", kind="harm")
    elif result["drained"] > 0:
        name = content.mortal_name(getattr(victim, "tid", None) or -1)
        toasts.toast(content.TOAST_FEED_SIP.format(name=name), kind="info")
    return result


# --- cowering mortals ---------------------------------------------------------------------


def start_cowardice(chron, content):
    """(Re)arm the cower tick. Idempotent; call from map_load."""
    if chron.cower_task is not None:
        try:
            bd.cancel_task(chron.cower_task)
        except Exception:
            pass
        chron.cower_task = None
    chron.cower_task = bd.schedule(lambda: _cower_tick(chron, content),
                                   delay=content.COWER_TICS,
                                   repeat=content.COWER_TICS, map_local=True)


def _cower_tick(chron, content):
    try:
        if bd.current_map() is None:
            return
    except Exception:
        return
    pawn = player_pawn()
    if pawn is None:
        return
    for spec in content.MORTALS:
        ref = bd.actor_ref(spec["tid"])
        if ref is None:
            continue
        try:
            if not ref.valid or not ref.alive:
                continue
            dist = ref.distance_to(pawn)
            if dist < 1.0 or dist > content.COWER_RADIUS:
                continue
            if not ref.check_sight(pawn):
                continue
            vx, vy, _ = ref.velocity
            if math.hypot(vx, vy) >= content.COWER_MAX_SPEED:
                continue
            dx = (ref.x - pawn.x) / dist
            dy = (ref.y - pawn.y) / dist
            ref.set_velocity(dx * content.COWER_SPEED,
                             dy * content.COWER_SPEED, 0.0, add=True)
            chron.cower_log.append({"tid": spec["tid"],
                                    "t": bd.level_time()})
        except Exception:
            continue


# --- the answering pack (Masquerade breach) -------------------------------------------------


def _ambush(chron, content, reason):
    now = bd.level_time()
    if now - chron.ambush_last < content.AMBUSH_COOLDOWN_TICS:
        return
    pawn = player_pawn()
    if pawn is None:
        return
    chron.ambush_last = now
    spawned = []
    base = math.radians(pawn.angle + 180.0)
    for i, tid in enumerate(content.AMBUSH_TIDS):
        ref = None
        # Prefer a spot behind the player, out of sight where possible;
        # each pack member starts from its own spread so a forced spawn
        # never telefrags its packmate.
        home = i * 40.0 - 40.0
        for spread in (home, home + 25.0, home - 25.0, 90.0, -90.0):
            angle = base + math.radians(spread)
            x = pawn.x + math.cos(angle) * content.AMBUSH_RADIUS
            y = pawn.y + math.sin(angle) * content.AMBUSH_RADIUS
            try:
                sector = bd.sector_at(x, y)
            except Exception:
                sector = None
            if sector is None:
                continue
            try:
                ref = bd.spawn(content.AMBUSH_CLASS, x, y,
                               float(sector.floor_height),
                               angle=float(pawn.angle), tid=tid, force=True)
            except Exception:
                ref = None
            if ref is not None:
                break
        if ref is not None:
            try:
                ref.tint = content.AMBUSH_TINT
            except Exception:
                pass
            spawned.append(tid)
    chron.ambush_log.append({"reason": str(reason), "tids": spawned,
                             "t": now})
    if spawned:
        toasts.toast(content.TOAST_BREACH, kind="harm")


# --- frenzy vignette storm --------------------------------------------------------------------


def start_frenzy_storm(chron, content):
    """A racing heartbeat while the Beast drives (no screen tint: the
    pulses used to fade the view red, which fought the player's aim)."""
    if chron.storm["active"]:
        return
    chron.storm["active"] = True
    chron.storm["pulses"] = 0

    def pulse():
        if not chron.storm["active"]:
            return
        count = chron.storm["pulses"]
        try:
            bd.play_ui_sound(content.STORM_HEARTBEAT, volume=0.7)
        except Exception:
            pass
        chron.storm["pulses"] = count + 1

    def end():
        chron.storm["active"] = False
        chron.storm["task"] = None

    chron.storm["task"] = bd.schedule(pulse, delay=1,
                                      repeat=content.STORM_PULSE_TICS,
                                      map_local=True)
    bd.schedule(end, delay=content.STORM_TICS, map_local=True)
