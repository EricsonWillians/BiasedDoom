"""The Last Feeding — ui: bd_horror-skinned vitae HUD and discipline panel.

Definition-only at import time. Both windows use the engine-shipped
``bd_horror.theme`` skin: the vitae bar in blood, humanity in sickly
green, Masquerade exposure in ember, hunger as a row of ASCII blood
drops, and discipline cooldowns as progress sweeps under each button.

Like every ``bd.imgui`` consumer, both ``draw`` methods are only legal
inside an ``imgui_frame`` handler — which never fires under
``-headless``, so every draw path here no-ops automatically in headless
runs.
"""

import biaseddoom as bd
import bd_vtm
from bd_horror import theme
from bd_horror.theme import PALETTE


class VitaeHud:
    """Immediate-mode ImGui window rendering the chronicle's vitals.

    The window starts visible; the close button and :meth:`toggle` stay
    in sync because ``begin``'s ``open`` result is fed back into
    :attr:`visible` every frame.
    """

    def __init__(self, chron, title="Vitae"):
        self.chron = chron
        self.title = str(title)
        self.visible = True

    def toggle(self):
        """Flip visibility and return the new state."""
        self.visible = not self.visible
        return self.visible

    def draw(self):
        """Submit the window. Call from an ``imgui_frame`` handler only."""
        if not self.visible:
            return
        imgui = bd.imgui
        theme.apply()
        try:
            expanded, self.visible = theme.begin_window(
                f"{self.title}###last_feeding_hud", pos=(40.0, 420.0),
                size=(310.0, 0.0), open=self.visible)
            try:
                if expanded:
                    self._draw_contents(imgui)
            except Exception as exc:
                bd.warn(f"vitae hud draw error: {exc!r}")
            finally:
                imgui.end()
        finally:
            theme.clear()

    # -- internals -------------------------------------------------------------

    def _draw_contents(self, imgui):
        state = self.chron.state

        blood = state.blood
        frac = blood.current / blood.max if blood.max > 0 else 0.0
        theme.bar("Vitae", frac,
                  overlay=f"{blood.current}/{blood.max} - gen "
                          f"{blood.generation}",
                  tone="blood", pulse=state.hunger.level >= 4)

        # Hunger as a row of blood drops: filled '*' / empty '-'.
        level = state.hunger.level
        drops = "*" * level + "-" * (bd_vtm.Hunger.MAX_LEVEL - level)
        color = (PALETTE["crimson"] if level >= bd_vtm.Hunger.FRENZY_MIN
                 else PALETTE["bone"])
        imgui.text_colored(*color, f"Hunger  {drops}  "
                                   f"({level}/{bd_vtm.Hunger.MAX_LEVEL})")

        humanity = state.humanity
        theme.bar("Humanity", humanity.rating / bd_vtm.Humanity.MAX_RATING,
                  overlay=f"{humanity.rating}/{bd_vtm.Humanity.MAX_RATING}",
                  tone="sickly")

        masquerade = state.masquerade
        theme.bar("Exposure", masquerade.level / bd_vtm.Masquerade.MAX_LEVEL,
                  overlay=f"Masquerade {masquerade.level}/"
                          f"{bd_vtm.Masquerade.MAX_LEVEL}",
                  tone="ember",
                  pulse=masquerade.level >= bd_vtm.Masquerade.MAX_LEVEL - 1)

        theme.kv_row("Anarchs",
                     str(state.factions.reputation("Anarchs")))
        theme.kv_row("Dread", f"{self.chron.horror.dread.level:.0f}")

        if level >= bd_vtm.Hunger.FRENZY_MIN:
            theme.omen_text("the Beast claws at the inside of your ribs.")
        if self.chron.storm_active():
            theme.omen_text("the Beast is driving. hold on to nothing.")


class DisciplinePanel:
    """The hunt's input path: disciplines, feeding, frenzy checks.

    Cooldowns render as a progress sweep under each button (elapsed
    fraction of the cooldown, bone 'ready' overlay when usable). The
    panel acts through the :class:`~systems.Chronicle`'s action methods,
    so all game rules stay in systems.py.
    """

    #: Disciplines fired untargeted, in display order.
    INSTANT = ("celerity", "obfuscate", "potence")

    def __init__(self, chron, content, title="Disciplines"):
        self.chron = chron
        self.content = content
        self.title = str(title)
        self.visible = True

    def toggle(self):
        self.visible = not self.visible
        return self.visible

    def draw(self):
        """Submit the window. Call from an ``imgui_frame`` handler only."""
        if not self.visible:
            return
        imgui = bd.imgui
        theme.apply()
        try:
            expanded, self.visible = theme.begin_window(
                f"{self.title}###last_feeding_panel", pos=(500.0, 60.0),
                size=(330.0, 0.0), open=self.visible)
            try:
                if expanded:
                    self._draw_contents(imgui)
            except Exception as exc:
                bd.warn(f"discipline panel draw error: {exc!r}")
            finally:
                imgui.end()
        finally:
            theme.clear()

    # -- internals -------------------------------------------------------------

    def _draw_contents(self, imgui):
        chron = self.chron
        pawn = _pawn()
        theme.section("Disciplines")
        for name in self.INSTANT:
            self._draw_discipline(imgui, name, pawn)
        dominate = chron.state.disciplines.get("dominate")
        if dominate is not None:
            if imgui.button(f"dominate nearest sabbat "
                            f"({dominate.blood_cost} blood)") and pawn:
                chron.dominate_nearest(pawn)
            self._draw_cooldown(dominate)

        theme.section("The Hunt")
        if imgui.button("feed on the nearest vessel") and pawn:
            chron.feed_nearest(pawn, self.content)
        if imgui.button("check frenzy"):
            chron.frenzy_check()

        active = chron.log.active()
        if active:
            theme.section("Chronicle")
            for quest in active:
                imgui.text_wrapped(quest.name)
                for obj in quest.active_objectives():
                    fraction = (obj.progress / obj.count
                                if obj.count > 0 else 0.0)
                    theme.bar(obj.text, fraction,
                              overlay=f"{obj.progress}/{obj.count}",
                              tone="bruise")

    def _draw_discipline(self, imgui, name, pawn):
        discipline = self.chron.state.disciplines.get(name)
        if discipline is None:
            return
        if imgui.button(f"{discipline.name} ({discipline.blood_cost} "
                        f"blood)") and pawn is not None:
            discipline.use(pawn)
        self._draw_cooldown(discipline)

    def _draw_cooldown(self, discipline):
        remaining = discipline.ready_in()
        total = max(1, discipline.cooldown_tics)
        fraction = 1.0 - min(1.0, remaining / total)
        overlay = "ready" if remaining <= 0 else f"{remaining}t"
        theme.bar("", fraction, overlay=overlay, tone="bruise")


def _pawn():
    try:
        player = bd.player(0)
        pawn = player.actor if player is not None else None
        return pawn if pawn is not None and pawn.valid else None
    except Exception:
        return None
