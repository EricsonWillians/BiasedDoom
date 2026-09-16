"""Whispers in the Walls, ui: the Grimoire, a bd_horror-skinned journal.

Definition-only at import time. :class:`Grimoire` is an immediate-mode
window rendered with the engine-shipped ``bd_horror.theme`` skin:
per-quest ornament sections, the quest name in bone, a state marker in
blood/sickly/wound, objective progress bars, and faded entries for
completed and still-sealed quests.

Like every ``bd.imgui`` consumer, :meth:`Grimoire.draw` is only legal
inside an ``imgui_frame`` handler, which never fires under ``-headless``,
so every draw path here no-ops automatically in headless runs.
"""

import biaseddoom as bd
from bd_quests import Quest
from bd_horror import theme
from bd_horror.theme import PALETTE

#: quest state -> (marker line, PALETTE key for the marker)
_STATE_MARKERS = {
    Quest.ACTIVE: ("[ the ink is wet ]", "blood"),
    Quest.COMPLETED: ("[ laid to rest ]", "sickly"),
    Quest.FAILED: ("[ broken ]", "wound"),
}


class Grimoire:
    """Immediate-mode ImGui window rendering a quest log as a grimoire.

    The window starts visible; the close button and :meth:`toggle` stay
    in sync because ``begin``'s ``open`` result is fed back into
    :attr:`visible` every frame. Sealed (inactive) quests render as faded
    entries so chained quests are visible before they unlock. When an
    ``xp_ledger`` mapping (quest id -> XP) is supplied, a footer line
    renders the running favor total earned through quest xp rewards.
    """

    def __init__(self, log, title="Grimoire", xp_ledger=None):
        self.log = log
        self.title = str(title)
        self.visible = True
        self.xp_ledger = xp_ledger

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
                f"{self.title}###witw_grimoire", pos=(36.0, 56.0),
                size=(420.0, 0.0), open=self.visible)
            try:
                if expanded:
                    self._draw_contents(imgui)
            except Exception as exc:
                bd.warn(f"grimoire draw error: {exc!r}")
            finally:
                imgui.end()
        finally:
            theme.clear()

    # -- internals -------------------------------------------------------------

    def _draw_contents(self, imgui):
        quests = self.log.all()
        if not quests:
            theme.faded_text("The pages are blank. The walls are not.")
            return
        first = True
        for quest in quests:
            if not first:
                theme.section("")  # ornament divider between entries
            first = False
            self._draw_quest(imgui, quest)
        if self.xp_ledger:
            total = sum(int(v) for v in self.xp_ledger.values())
            if total:
                theme.section("")
                theme.kv_row("Favor", f"{total} owed by the parish")

    def _draw_quest(self, imgui, quest):
        if quest.state == Quest.INACTIVE:
            # Sealed: the entry exists but cannot be read yet.
            theme.faded_text(f"{quest.name}  -  sealed")
            return
        theme.section(quest.name)
        marker, tone = _STATE_MARKERS.get(quest.state,
                                          ("[ ? ]", "marrow"))
        imgui.text_colored(*PALETTE[tone], marker)
        if quest.state == Quest.COMPLETED:
            if quest.description:
                theme.faded_text(quest.description)
        elif quest.state == Quest.FAILED:
            reason = quest.fail_reason or "no reason recorded"
            theme.omen_text(f"the rite broke: {reason}")
        elif quest.description:
            imgui.text_wrapped(quest.description)  # bone body text
        if quest.giver:
            theme.kv_row("Giver", quest.giver)
        for obj in quest.objectives:
            self._draw_objective(imgui, quest, obj)

    def _draw_objective(self, imgui, quest, obj):
        if obj.done:
            theme.faded_text(f"x  {obj.text}")
        elif obj in quest.active_objectives():
            fraction = obj.progress / obj.count if obj.count > 0 else 0.0
            theme.bar(obj.text, fraction,
                      overlay=f"{obj.progress}/{obj.count}", tone="blood",
                      pulse=obj.progress > 0 and not obj.done)
        else:
            imgui.text_disabled(f"-  {obj.text}")
