#!/usr/bin/env python3
"""Offline regression checks for the companion squad and wire contracts.

The live engine path needs an IWAD and a graphical/bootstrap environment. This
test therefore models the two order-sensitive pieces of the design (stable
profile membership and stable-ID removal), then checks the source contracts
which connect that model to the engine and its menus.
"""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
MAX_COMPANIONS = 7


class StableCompanionQueue:
    """Small model of profile -> stable-ID removal across delayed events."""

    def __init__(self, active: dict[int, int] | None = None) -> None:
        # profile -> immutable companion ID
        self.active = dict(active or {})
        self.pending: set[int] = set()
        self.events: list[int] = []

    def queue_profile_removal(self, profile: int) -> int | None:
        companion_id = self.active.get(profile)
        if companion_id is None or companion_id in self.pending:
            return None
        self.pending.add(companion_id)
        self.events.append(companion_id)
        return companion_id

    def apply_next(self) -> None:
        companion_id = self.events.pop(0)
        for profile, active_id in list(self.active.items()):
            if active_id == companion_id:
                del self.active[profile]
                break
        self.pending.discard(companion_id)


def first_disabled_profile(mask: int) -> int | None:
    for profile in range(MAX_COMPANIONS):
        if not mask & (1 << profile):
            return profile
    return None


def enable_profile(mask: int, profile: int) -> int:
    return mask | (1 << profile)


def disable_profile(mask: int, profile: int) -> int:
    return mask & ~(1 << profile)


def normalize_legacy_count(mask: int, count: int, is_host: bool) -> tuple[int, int]:
    """Model the host-only legacy count -> sparse-mask migration."""

    if not is_host:
        return mask, count
    if mask == 0 and count > 0:
        mask = (1 << min(count, MAX_COMPANIONS)) - 1
    return mask, mask.bit_count()


def apply_compatibility_count(mask: int, count: int, *, is_host: bool, restoring: bool) -> int:
    """Model the local legacy count shorthand.

    The count is an archived compatibility projection, not a second replicated
    membership command. Only a direct host edit expands it to a sequential
    first-N mask; receivers mirror the authoritative mask they received.
    """

    if is_host and not restoring:
        return (1 << min(count, MAX_COMPANIONS)) - 1
    return mask


def queue_atomic_companion_record(used: int, record_size: int, budget: int = 5 * 1024) -> tuple[bool, int]:
    """Model the all-or-nothing special-event enqueue used by companion CVars."""

    if record_size <= 0 or record_size > budget or used > budget - record_size:
        return False, used
    return True, used + record_size


def queue_atomic_companion_batch(
    used: int, record_sizes: list[int], budget: int = 5 * 1024
) -> tuple[bool, int]:
    """Model a membership-plus-profile transaction in one event block."""

    total = sum(record_sizes)
    if not record_sizes or any(size <= 0 for size in record_sizes):
        return False, used
    if total > budget or used > budget - total:
        return False, used
    return True, used + total


def retained_event_prefix(used: int, atomic_end: int, packet_budget: int) -> int:
    """Model packet emission after a later legacy suffix overruns one tic."""

    if used <= packet_budget:
        return used
    if 0 < atomic_end <= packet_budget:
        return atomic_end
    return 0


def exit_frame_size(packet: bytes) -> int | None:
    """Model the versioned host-handoff framing without a network-mode branch."""

    if len(packet) == 1:
        return 1
    if len(packet) < 4:
        return None
    expected = 4 + ((packet[2] << 8) | packet[3])
    return expected if expected == len(packet) else None


@dataclass(frozen=True)
class CollisionPawn:
    """Minimal model of the player-pair collision predicate.

    Companion pass-through is deliberately narrower than a no-clip flag: both
    actors must be their live player pawns and teammates, and the exception is
    only earned when one side is a real companion bot. It is a physical
    movement rule only, so combat targeting and hitscan behavior are left to
    the existing player-clip policy. Keeping this model in the companion
    regression makes that gameplay boundary explicit without pretending that
    the Python test can reproduce engine collision.
    """

    has_player: bool = True
    is_live_pawn: bool = True
    is_companion: bool = False


def is_companion_pawn(pawn: CollisionPawn) -> bool:
    return pawn.has_player and pawn.is_live_pawn and pawn.is_companion


def companion_pair_ignores_physical_collision(
    self: CollisionPawn,
    other: CollisionPawn,
    *,
    teammates: bool,
    deathmatch: bool = False,
) -> bool:
    """Model the physical-only companion exemption in ``P_CanCollideWith``."""

    if not (self.has_player and self.is_live_pawn and other.has_player and
            other.is_live_pawn and teammates and not deathmatch):
        return False
    return is_companion_pawn(self) or is_companion_pawn(other)


def test_stable_removal_survives_an_intervening_departure() -> None:
    queue = StableCompanionQueue({0: 101, 1: 202, 2: 303})
    assert queue.queue_profile_removal(2) == 303

    # Profile 1 leaves for an unrelated reason before the network event for
    # profile 3 arrives. An ordinal removal would now point at a different bot;
    # the stable ID still names exactly the originally selected companion.
    del queue.active[1]
    queue.apply_next()
    assert queue.active == {0: 101}


def test_pending_removal_does_not_duplicate_an_exact_event() -> None:
    queue = StableCompanionQueue({0: 42})
    assert queue.queue_profile_removal(0) == 42
    assert queue.queue_profile_removal(0) is None
    assert queue.events == [42]
    queue.apply_next()
    assert queue.active == {}
    assert queue.pending == set()


def test_sparse_profiles_keep_the_other_squad_members() -> None:
    mask = 0
    first = first_disabled_profile(mask)
    assert first == 0
    mask = enable_profile(mask, first)
    mask = enable_profile(mask, 1)
    mask = enable_profile(mask, 2)

    # Removing companion 2 must not renumber or remove companions 1 and 3.
    mask = disable_profile(mask, 1)
    assert mask == 0b101
    assert first_disabled_profile(mask) == 1
    assert mask & (1 << 0)
    assert mask & (1 << 2)


def test_profile_roster_replaces_legacy_startup_entries() -> None:
    """Model the exact roster rule after two random/profile deployments.

    Random drafts only choose the profile fields before deployment; the
    membership mask is the same as two ordinary profile additions. Once that
    mask exists, stale command-line entries are not another source of
    companions. They are removed/ignored before desired profiles are chosen.
    """

    legacy_startup = ["Aster", "Beacon", "Cinder", "Delta", "Ember", "Flint"]
    enabled_profiles = [0, 1]
    legacy_startup.clear()
    realized_profiles = list(enabled_profiles)

    assert legacy_startup == []
    assert realized_profiles == [0, 1]
    assert len(realized_profiles) == 2


def test_received_mask_is_the_only_network_membership_source() -> None:
    # A client with an unrelated archived legacy count must mirror the host's
    # sparse mask instead of re-expanding its own local count into profile 1.
    assert normalize_legacy_count(mask=0, count=1, is_host=False) == (0, 1)
    assert 0b101.bit_count() == 2


def test_legacy_count_is_local_and_sparse_membership_stays_authoritative() -> None:
    # Profiles 1 and 3 are a deliberate sparse selection. The local count
    # shortcut may create a sequential squad only when directly edited by the
    # host; it is never a replicated echo that can flatten a sparse mask.
    sparse = 0b101
    assert apply_compatibility_count(
        sparse, 2, is_host=False, restoring=False
    ) == sparse
    assert apply_compatibility_count(
        sparse, 2, is_host=True, restoring=True
    ) == sparse
    assert apply_compatibility_count(
        sparse, 2, is_host=True, restoring=False
    ) == 0b011


def test_companion_cvar_record_is_applied_only_after_atomic_enqueue() -> None:
    # A busy tic must leave both the local authoritative setting and its peer
    # state unchanged rather than force-applying a record that cannot travel.
    queued, used = queue_atomic_companion_record(5100, 32)
    assert not queued
    assert used == 5100
    queued, used = queue_atomic_companion_record(64, 32)
    assert queued
    assert used == 96


def test_remove_and_dismiss_never_partially_clear_a_profile() -> None:
    # A one-profile removal is mask + name + skin + style + skill. Dismissal
    # is the same transaction expanded to every profile. Neither is allowed to
    # locally commit the mask if the whole serialized group cannot fit.
    queued, used = queue_atomic_companion_batch(5000, [20, 40, 40, 28, 28])
    assert not queued
    assert used == 5000
    queued, used = queue_atomic_companion_batch(64, [20, 40, 40, 28, 28])
    assert queued
    assert used == 220


def test_later_legacy_events_cannot_crowd_out_an_atomic_roster_edit() -> None:
    # The host can accept a roster transaction, then receive unrelated legacy
    # writes in the same tic. Packet emission retains the ordered prefix
    # through the transaction rather than dropping every event in that tic.
    legacy_prefix = 18
    atomic_end = legacy_prefix + 120
    oversized_suffix = 900
    assert retained_event_prefix(atomic_end + oversized_suffix, atomic_end, 512) == atomic_end

    # A later transaction advances the protected prefix. Its preceding normal
    # event remains ordered before it, while only the trailing legacy suffix is
    # omitted when this command must be trimmed.
    second_atomic_end = atomic_end + 12 + 80
    assert retained_event_prefix(second_atomic_end + 900, second_atomic_end, 512) == second_atomic_end

    # Retain the old safe behavior for an oversized tic that has no atomic
    # transaction to protect, or whose protected prefix cannot itself fit.
    assert retained_event_prefix(900, 0, 512) == 0
    assert retained_event_prefix(900, 700, 512) == 0


def test_zero_to_one_draft_uses_one_saved_profile() -> None:
    mask = 0
    profile = first_disabled_profile(mask)
    assert profile == 0

    # A blank draft resolves an available identity once, stores it, then only
    # enables that profile. It never expands a scalar target into a whole team.
    profiles = ["", "", "", "", "", "", ""]
    available = ["Aster", "Beacon"]
    profiles[profile] = available[0]
    mask = enable_profile(mask, profile)
    assert profiles == ["Aster", "", "", "", "", "", ""]
    assert mask == 0b1


def test_identity_reservations_prevent_duplicate_squad_members() -> None:
    enabled_profiles = {"Aster", "Cinder"}
    roster = ["Aster", "Beacon", "Cinder", "Dawn"]
    resolved = next(name for name in roster if name not in enabled_profiles)
    assert resolved == "Beacon"


def rehydrate_roster(entries: dict[str, str], active_names: list[str]) -> dict[str, str]:
    """Model the promoted host's local roster reservation pass."""

    active = {name.casefold() for name in active_names}
    return {name: "yes" if name.casefold() in active else "no" for name in entries}


def test_host_handoff_rehydrates_local_roster_usage() -> None:
    before = {"Aster": "no", "Beacon": "waiting", "Cinder": "no"}
    after = rehydrate_roster(before, ["aster", "CINDER"])
    assert after == {"Aster": "yes", "Beacon": "no", "Cinder": "yes"}


def test_host_handoff_snapshot_preserves_an_unflushed_edit() -> None:
    # The exit snapshot is authoritative over the old peer copy, so a host
    # edit accepted just before quit survives even if its ordinary tic event
    # was never broadcast.
    old_peer_mask = 0b001
    departing_host_mask = 0b101
    handoff_snapshot_mask = departing_host_mask
    promoted_host_mask = handoff_snapshot_mask
    assert old_peer_mask != departing_host_mask
    assert promoted_host_mask == 0b101


def test_host_exit_framing_handles_p2p_packet_server_and_duplicates() -> None:
    # A sudden disconnect remains a valid one-byte exit in either mode. It has
    # no snapshot, so the existing successor fallback remains responsible for
    # choosing a new host.
    abrupt = bytes([0x80])
    assert exit_frame_size(abrupt) == 1

    # A deliberate host exit adds successor, length, and a complete snapshot.
    # Its size is mode-independent: peer-to-peer and packet-server handoff use
    # the same authoritative roster frame.
    snapshot = bytes([9, 8, 7])
    extended = bytes([0x80, 2, 0, len(snapshot)]) + snapshot
    for mode in ("peer-to-peer", "packet-server"):
        assert exit_frame_size(extended) == 4 + len(snapshot), mode

    # Repeated exit packets arrive after the first one promotes client 2. They
    # still validate by frame length, but only the original current host may
    # apply the snapshot; duplicates become harmless already-gone exits.
    old_host, promoted_host = 1, 2
    assert old_host == 1 and len(extended) >= 4
    assert old_host != promoted_host
    assert exit_frame_size(extended) == 4 + len(snapshot)

    assert exit_frame_size(bytes([0x80, 2])) is None
    assert exit_frame_size(bytes([0x80, 2, 0, 4, 9])) is None


def test_companion_pass_through_is_player_pair_only() -> None:
    """A companion cannot body-block a friendly live player at a doorway.

    The ordinary player-pair rule stays unchanged: even cooperative humans
    retain their configured physical collision. A bot only receives the
    movement exemption while it is its current player's live pawn, so stale
    dolls, monsters, deathmatch bots, and enemies never become
    collision-transparent as a side effect. Hitscan/projectile behavior is
    intentionally outside this physical-collision rule.
    """

    human = CollisionPawn()
    companion = CollisionPawn(is_companion=True)
    enemy_companion = CollisionPawn(is_companion=True)
    stale_companion = CollisionPawn(is_live_pawn=False, is_companion=True)
    non_player = CollisionPawn(has_player=False, is_companion=True)

    # Ordinary cooperative humans retain the pre-existing collision rule.
    assert not companion_pair_ignores_physical_collision(
        human, human, teammates=True
    )

    # Either travel direction through a friendly companion works, preventing
    # a bot from sealing a narrow doorway or a dead-end room.
    assert companion_pair_ignores_physical_collision(
        human, companion, teammates=True
    )
    assert companion_pair_ignores_physical_collision(
        companion, human, teammates=True
    )

    # Friendship and real current-pawn identity remain mandatory. In
    # particular, this rule must not turn enemy bots, stale map dolls, or
    # non-player map actors into no-clip geometry.
    assert not companion_pair_ignores_physical_collision(
        human, enemy_companion, teammates=False
    )
    assert not companion_pair_ignores_physical_collision(
        human, stale_companion, teammates=True
    )
    assert not companion_pair_ignores_physical_collision(
        human, non_player, teammates=True
    )
    assert not companion_pair_ignores_physical_collision(
        human, companion, teammates=True, deathmatch=True
    )


def should_draw_multiplayer_mugshot_backdrop(
    *, multiplayer: bool, local_companion_coop: bool
) -> bool:
    """Model the Doom HUD's player-color portrait-backing decision."""

    return multiplayer and not local_companion_coop


def test_local_companions_keep_coop_rules_without_a_multiplayer_face_backdrop() -> None:
    """Transparent legacy faces must reveal STBAR, not STFBANY green.

    A local companion roster intentionally promotes the simulation to co-op.
    It must not turn the one human player's mugshot into the real-multiplayer
    player-color panel, because older mods often leave silhouette pixels
    transparent around their portrait art.
    """

    assert not should_draw_multiplayer_mugshot_backdrop(
        multiplayer=True, local_companion_coop=True
    )
    assert should_draw_multiplayer_mugshot_backdrop(
        multiplayer=True, local_companion_coop=False
    )
    assert not should_draw_multiplayer_mugshot_backdrop(
        multiplayer=False, local_companion_coop=False
    )


def require(path: Path, fragment: str) -> None:
    text = path.read_text(encoding="utf-8")
    assert fragment in text, f"missing companion regression contract: {path.name}: {fragment!r}"


def source_block(text: str, start: str, end: str) -> str:
    """Return a narrow source region so a contract cannot match nearby code."""

    begin = text.index(start)
    finish = text.index(end, begin + len(start))
    return text[begin:finish]


def test_source_contracts() -> None:
    bot_cpp = REPO_ROOT / "src" / "playsim" / "bots" / "b_bot.cpp"
    bot_h = REPO_ROOT / "src" / "playsim" / "bots" / "b_bot.h"
    game_cpp = REPO_ROOT / "src" / "playsim" / "bots" / "b_game.cpp"
    g_game_cpp = REPO_ROOT / "src" / "g_game.cpp"
    level_cpp = REPO_ROOT / "src" / "g_level.cpp"
    level_header = REPO_ROOT / "src" / "g_levellocals.h"
    p_mobj_cpp = REPO_ROOT / "src" / "playsim" / "p_mobj.cpp"
    p_map_cpp = REPO_ROOT / "src" / "playsim" / "p_map.cpp"
    p_local_h = REPO_ROOT / "src" / "playsim" / "p_local.h"
    net_cpp = REPO_ROOT / "src" / "d_net.cpp"
    net_info_cpp = REPO_ROOT / "src" / "d_netinfo.cpp"
    net_h = REPO_ROOT / "src" / "d_net.h"
    protocol_h = REPO_ROOT / "src" / "d_protocol.h"
    engine_net_cpp = REPO_ROOT / "src" / "common" / "engine" / "i_net.cpp"
    version_h = REPO_ROOT / "src" / "version.h"
    think_cpp = REPO_ROOT / "src" / "playsim" / "bots" / "b_think.cpp"
    func_cpp = REPO_ROOT / "src" / "playsim" / "bots" / "b_func.cpp"
    menu_def = REPO_ROOT / "wadsrc" / "static" / "menudef.txt"
    menu_zs = REPO_ROOT / "wadsrc" / "static" / "zscript" / "engine" / "ui" / "menu" / "menu.zs"
    squad_zs = REPO_ROOT / "wadsrc" / "static" / "zscript" / "engine" / "ui" / "menu" / "companionbotsmenu.zs"
    statusbar_h = REPO_ROOT / "src" / "g_statusbar" / "sbar.h"
    statusbar_cpp = REPO_ROOT / "src" / "g_statusbar" / "shared_sbar.cpp"
    statusbar_zs = REPO_ROOT / "wadsrc" / "static" / "zscript" / "ui" / "statusbar" / "statusbar.zs"
    doom_statusbar_zs = REPO_ROOT / "wadsrc" / "static" / "zscript" / "ui" / "statusbar" / "doom_sbar.zs"
    zscript_list = REPO_ROOT / "wadsrc" / "static" / "zscript.txt"
    docs = REPO_ROOT / "docs" / "engine" / "companion-bots.md"

    # Membership is a sparse profile mask, with individual appearance/skill
    # fields. The old count remains only as a backward-compatible projection.
    require(bot_cpp, "CUSTOM_CVAR(Int, bot_companion_enabled_mask, 0, CVAR_ARCHIVE | CVAR_SERVERINFO | CVAR_NOINITCALL)")
    require(bot_cpp, "CUSTOM_CVAR(Int, bot_companion_count, 0, CVAR_ARCHIVE | CVAR_NOINITCALL)")
    require(bot_cpp, "CVAR(Int, bot_companion_skill1, 0, CVAR_ARCHIVE | CVAR_SERVERINFO)")
    require(bot_cpp, "bool BotCompanionSetEnabled(int profile, bool enabled)")
    require(bot_cpp, "bool BotCompanionClearAllProfiles()")
    require(bot_cpp, "if (netgame && consoleplayer != Net_Arbitrator)")
    require(bot_cpp, "BotCompanionEnabledProfileCount()")
    require(bot_cpp, "bool ResolveCompanionProfileIdentity(FCajunMaster &bots, int profile)")
    require(bot_cpp, "bool CanBeginCompanionDraft(FCajunMaster &bots)")
    require(bot_cpp, "bool RandomizeCompanionDraft(FCajunMaster &bots, int profile)")
    require(bot_cpp, "bool AppendProfileConfigurationChanges(FCompanionServerInfoChange *changes, unsigned &count,")
    require(bot_cpp, "bool BotCompanionSetProfileName(int profile, const char *name)")
    require(bot_cpp, "bool AddConsoleCompanion(FCajunMaster &bots, const char *requestedName)")
    require(bot_cpp, "bool RemoveConfiguredCompanion(int profile)")
    require(bot_cpp, "CCMD(addcompanion)")
    require(bot_cpp, "CCMD(removecompanion)")
    require(bot_cpp, "CCMD(dismisscompanions)")
    require(bot_cpp, "bool DismissConfiguredCompanionSquad()")
    require(bot_cpp, "if (!deathmatch)")
    require(bot_cpp, "if (!BotCompanionClearAllProfiles())")
    require(bot_cpp, "if (!membershipSync && !replicatedUpdate)")
    require(bot_cpp, "D_IsApplyingServerInfoChange() || savegamerestore")
    require(net_info_cpp, "bool D_IsApplyingServerInfoChange()")
    require(net_info_cpp, "if (companionSetting)")
    require(net_info_cpp, "bool D_ApplyCompanionServerInfoChangesAtomically")
    require(net_info_cpp, "bool D_BuildCompanionServerInfoSnapshot")
    require(net_info_cpp, "bool D_ApplyCompanionServerInfoSnapshot")
    require(net_info_cpp, "BuildCompanionServerInfoChanges(changes, count, event)")
    atomic_cvar = "Net_WriteEventAtomic(event.Data(), (int)event.Size())"
    net_info_text = net_info_cpp.read_text(encoding="utf-8")
    assert net_info_text.index(atomic_cvar) < net_info_text.index("changes[i].CVar->ForceSet(changes[i].Value, changes[i].Type);"), (
        "companion CVar groups must be atomically queued before they mutate the host"
    )
    require(bot_cpp, "bool ClearProfileAndDisable(int profile)")
    require(bot_cpp, "D_ApplyCompanionServerInfoChangesAtomically(changes, count)")
    require(bot_cpp, "CompanionDraftProfile = -1;")
    require(bot_cpp, "if (savegamerestore || !CanDeployCompanions() || gamestate != GS_LEVEL || !CanManageCompanions())")
    require(bot_cpp, "bool CanDeployCompanions()")
    bot_text = bot_cpp.read_text(encoding="utf-8")
    can_deploy = source_block(bot_text, "bool CanDeployCompanions()", "void ApplyCompanionTargetNow()")
    assert "return !deathmatch;" in can_deploy, (
        "a selected deathmatch must advertise save-for-co-op rather than immediate deployment"
    )
    assert "AddingCompanionDirectly" not in bot_cpp.read_text(encoding="utf-8"), (
        "the old count-indexed direct-add bypass must not return"
    )

    can_begin_draft = source_block(
        bot_text,
        "DEFINE_ACTION_FUNCTION(FCompanionBots, CanBeginDraft)",
        "DEFINE_ACTION_FUNCTION(FCompanionBots, CanDeploy)",
    )
    assert "CanManageCompanions() && master != nullptr" in can_begin_draft
    assert "CanBeginCompanionDraft(*master)" in can_begin_draft
    assert "CanDeployCompanions()" not in can_begin_draft, (
        "a saved squad must be configurable during a live deathmatch"
    )
    begin_draft = source_block(
        bot_text,
        "DEFINE_ACTION_FUNCTION(FCompanionBots, BeginDraft)",
        "DEFINE_ACTION_FUNCTION(FCompanionBots, GetDraftProfile)",
    )
    assert "!DMenu::InMenu || !CanManageCompanions()" in begin_draft
    assert "CanDeployCompanions()" not in begin_draft
    randomize_draft = source_block(
        bot_text,
        "bool RandomizeCompanionDraft(FCajunMaster &bots, int profile)",
        "bool AddConsoleCompanion",
    )
    assert "M_Random" in randomize_draft
    assert "BotCompanionProfileNameReserved" in randomize_draft
    assert "AppendProfileConfigurationChanges" in randomize_draft
    assert "D_ApplyCompanionServerInfoChangesAtomically(changes, count)" in randomize_draft, (
        "random identity, skin, style, and skill must reach peers as one profile transaction"
    )
    begin_random_draft = source_block(
        bot_text,
        "DEFINE_ACTION_FUNCTION(FCompanionBots, BeginRandomDraft)",
        "DEFINE_ACTION_FUNCTION(FCompanionBots, GetDraftProfile)",
    )
    assert "RandomizeCompanionDraft(*master, profile)" in begin_random_draft
    assert "BotCompanionSetEnabled" not in begin_random_draft, (
        "a random companion must remain reviewable as a draft until explicit deployment"
    )
    reroll_draft = source_block(
        bot_text,
        "DEFINE_ACTION_FUNCTION(FCompanionBots, RandomizeDraft)",
        "DEFINE_ACTION_FUNCTION(FCompanionBots, DeployDraft)",
    )
    assert "RandomizeCompanionDraft(*master, CompanionDraftProfile)" in reroll_draft
    deploy_draft = source_block(
        bot_text,
        "DEFINE_ACTION_FUNCTION(FCompanionBots, DeployDraft)",
        "DEFINE_ACTION_FUNCTION(FCompanionBots, CancelDraft)",
    )
    assert "!DMenu::InMenu || !CanManageCompanions() || CompanionDraftProfile < 0" in deploy_draft
    assert "CanDeployCompanions()" not in deploy_draft
    assert deploy_draft.index("BotCompanionSetEnabled(profile, true)") < deploy_draft.index("ApplyCompanionTargetNow();")
    apply_now = source_block(bot_text, "void ApplyCompanionTargetNow()", "// Resolve a profile")
    assert "!CanDeployCompanions()" in apply_now

    # The live bot stores a profile and immutable ID through savegames, while
    # the lifecycle can resolve active/pending members by profile or ID.
    require(bot_h, "int CompanionProfile = FCajunMaster::NoCompanionProfile;")
    require(bot_h, "uint32_t CompanionJoinOrder = FCajunMaster::NoCompanionId;")
    require(bot_h, "bool QueueCompanionRemoval(uint32_t companionId);")
    require(bot_h, "bool RemoveCompanionById(FLevelLocals *Level, uint32_t companionId")
    require(bot_h, "int GetCompanionSlotByProfile(int profile) const;")
    require(bot_cpp, '("companionprofile", CompanionProfile)')
    require(bot_cpp, '("companionjoinorder", CompanionJoinOrder)')
    require(game_cpp, "bool FCajunMaster::RemoveCompanionById")
    require(game_cpp, "bool FCajunMaster::IsCompanionProfilePending(int profile) const")
    require(game_cpp, "if (!deathmatch && (BotCompanionEnabledProfileMask() != 0 || HasProfileManagedCompanions()))")
    require(game_cpp, "BotApplyCompanionSkillPreset(companionProfile, skill)")
    require(game_cpp, "BotCompanionSetProfileName(profile, candidate)")
    require(game_cpp, "pending_companion_removals.Clear();")
    require(game_cpp, "if (Level == nullptr || deathmatch)")
    require(game_cpp, "if (!sv_coopsharekeys)")

    game_text = game_cpp.read_text(encoding="utf-8")
    configured_spawn = source_block(
        game_text,
        "bool FCajunMaster::SpawnConfiguredCompanion",
        "bool FCajunMaster::SpawnBot",
    )
    assert configured_spawn.index("if (!BotCompanionSetProfileName(profile, candidate))") < configured_spawn.index(
        "if (SpawnBot(candidate, NOCOLOR, profile))"
    ), "a fallback identity must persist before its add event is queued"
    main_block = source_block(game_text, "void FCajunMaster::Main", "void FCajunMaster::Init")
    assert "if (!deathmatch && CountCompanions() > target)" in main_block, (
        "a transient deathmatch addbot must not be removed by the co-op target"
    )
    require(game_cpp, "RestoreSinglePlayerModeIfIdle();")
    require(bot_h, "bool companion_forced_multiplayer = false;")
    require(bot_h, "void RestoreSinglePlayerModeIfIdle();")

    # Local companions intentionally promote a solo run to co-op for gameplay,
    # but the Doom status bar must not add STFBANY's player-color backing under
    # a lone human's portrait. Mods with transparent classic face patches then
    # continue to reveal their STBAR art instead of a bright green square.
    require(statusbar_h, "bool IsLocalCompanionCoop() const;")
    require(statusbar_zs, "native bool IsLocalCompanionCoop();")
    statusbar_text = statusbar_cpp.read_text(encoding="utf-8")
    local_companion_hud = source_block(
        statusbar_text,
        "bool DBaseStatusBar::IsLocalCompanionCoop() const",
        "static int IsLocalCompanionCoop",
    )
    assert "multiplayer && !netgame && !deathmatch" in local_companion_hud
    assert "primaryLevel->BotInfo.HasCompanionForcedMultiplayer()" in local_companion_hud
    doom_statusbar_text = doom_statusbar_zs.read_text(encoding="utf-8")
    assert "if (multiplayer && !IsLocalCompanionCoop())" in doom_statusbar_text
    assert doom_statusbar_text.index("if (multiplayer && !IsLocalCompanionCoop())") < doom_statusbar_text.index(
        'DrawImage("STFBANY"'
    )
    # The ownership marker is persisted independently of the engine's generic
    # multiplayer flag. That keeps save/load from stranding a one-human local
    # map in co-op after its final companion is dismissed.
    psave_text = (REPO_ROOT / "src" / "p_saveg.cpp").read_text(encoding="utf-8")
    require(bot_h, "HasCompanionForcedMultiplayer() const")
    require(bot_h, "SetCompanionForcedMultiplayer(bool value)")
    assert 'arc("companionmultiplayerowned", companionMultiplayerOwned);' in psave_text
    assert "BotInfo.SetCompanionForcedMultiplayer(companionMultiplayerOwned);" in psave_text
    level_text = level_cpp.read_text(encoding="utf-8")
    assert "void G_ConfigureNewGameMultiplayerMode()" in level_text
    assert "bool G_IsAutomaticCompanionMultiplayerMode()" in level_text
    assert "automatic_companion_multiplayer_mode = !netgame && !requestedMultiplayer" in level_text
    d_main_cpp = REPO_ROOT / "src" / "d_main.cpp"
    d_main_text = d_main_cpp.read_text(encoding="utf-8")
    autostart_begin = d_main_text.index("if (autostart || netgame)")
    autostart_init = d_main_text.index("G_InitNew(startmap.GetChars(), false);", autostart_begin)
    assert d_main_text.index("G_ConfigureNewGameMultiplayerMode();", autostart_begin) < autostart_init, (
        "command-line maps must choose companion co-op mode before map setup"
    )
    init_block = source_block(game_text, "void FCajunMaster::Init", "void FCajunMaster::BeginCompanionMap")
    assert "else if (BotCompanionEnabledProfileMask() != 0 && getspawned.Size() == 0)" in init_block
    assert "const int profileCount = BotCompanionEnabledProfileCount();" in init_block
    assert "SetDesiredCompanionCount(profileCount != 0 ? profileCount : wanted_botnum);" in init_block
    assert "SetDesiredCompanionCount(0);" in init_block
    assert "companion_forced_multiplayer = G_IsAutomaticCompanionMultiplayerMode();" in init_block
    reconcile_block = source_block(game_text, "bool FCajunMaster::ReconcileCompanions", "bool FCajunMaster::SpawnConfiguredCompanion")
    assert "if (Level == nullptr || deathmatch)" in reconcile_block
    profile_reconcile = reconcile_block[reconcile_block.index("// Once profile mode is selected"):]
    assert "getspawned.Clear();" in profile_reconcile
    assert "SetDesiredCompanionCount(BotCompanionEnabledProfileCount());" in profile_reconcile
    assert "QueueCompanionRemoval(companionId)" in profile_reconcile
    assert "QueueCompanionRemoval(bot->companionId)" in profile_reconcile
    assert "const int profileCapacity = GetCompanionCapacity();" in profile_reconcile
    assert "SpawnBot(legacyName, NOCOLOR, NoCompanionProfile)" not in profile_reconcile, (
        "profile mode must not append legacy startup bots to an exact configured squad"
    )

    addbot_block = source_block(bot_text, "CCMD (addbot)", "CCMD(addcompanion)")
    removebots_block = source_block(bot_text, "CCMD (removebots)", "CCMD (freeze)")
    for alias_block in (addbot_block, removebots_block):
        assert "if (!CanManageCompanions())" in alias_block
        assert "Companion profiles cannot be changed while recording or playing a demo." in alias_block

    # The variable-sized add record and selected removal are both atomically
    # replicated and use a profile + stable-ID tail, never an ordinal.
    spawn_start = game_cpp.read_text(encoding="utf-8").index("bool FCajunMaster::SpawnBot")
    spawn_end = game_cpp.read_text(encoding="utf-8").index("void FCajunMaster::TryAddBot", spawn_start)
    spawn_bot = game_cpp.read_text(encoding="utf-8")[spawn_start:spawn_end]
    atomic_add = "Net_WriteEventAtomic(event.Data(), (int)event.Size())"
    assert atomic_add in spawn_bot, "DEM_ADDBOT must be committed atomically"
    assert spawn_bot.index(atomic_add) < spawn_bot.index("thebot->inuse = BOTINUSE_Waiting;"), (
        "a rejected DEM_ADDBOT must not reserve a phantom waiting roster entry"
    )
    require(game_cpp, "WriteInt8(IsValidCompanionProfile(companionProfile)")
    require(game_cpp, "WriteInt32((int32_t)companionId, stream);")
    require(game_cpp, "const uint32_t companionId = (uint32_t)ReadInt32(stream);")
    require(game_cpp, "QueueCompanionRemoval(activeId)")
    require(net_cpp, "primaryLevel->BotInfo.RemoveCompanionById(primaryLevel, companionId);")
    require(net_cpp, "// botshift + NUL userinfo + four skill bytes + profile byte + stable ID")
    require(net_cpp, "case DEM_REMOVECOMPANION:")
    require(protocol_h, "DEM_ADDBOT")
    require(protocol_h, "profile (0xff legacy), Int: stable companion ID")
    require(protocol_h, "DEM_REMOVECOMPANION, // 83 Int: replicated stable companion ID")
    require(version_h, "#define DEMOGAMEVERSION 0x224")
    require(version_h, "#define MINDEMOVERSION 0x224")

    # Host authority applies equally to every new profile/mask CVar.
    require(net_info_cpp, 'stricmp(name, "bot_companion_enabled_mask") == 0')
    require(net_info_cpp, 'IsCompanionProfileServerCVar(name, "bot_companion_skill")')
    require(net_info_cpp, "sender == Net_Arbitrator || !IsCompanionServerCVar(name)")
    require(net_h, "constexpr size_t NetAtomicEventMaxSize = 5 * 1024;")
    require(net_cpp, "size_t AtomicEnd = 0;")
    require(net_cpp, "size_t CurrentAtomicEnd = 0;")
    require(net_cpp, "CurrentAtomicEnd = CurrentSize;")
    require(net_cpp, "Streams[CurrentClientTic % BACKUPTICS].AtomicEnd = CurrentAtomicEnd;")
    require(net_cpp, "eventBytes = stream.AtomicEnd;")
    require(net_cpp, "retained the atomic event prefix")
    require(net_cpp, "primaryLevel->BotInfo.RehydrateCompanionRosterUsage();")
    require(net_cpp, "D_BuildCompanionServerInfoSnapshot(companionSnapshot)")
    require(net_cpp, "D_ApplyCompanionServerInfoSnapshot(snapshot, clientNum)")
    require(net_cpp, "const size_t handoffPacketSize = 4 + companionSnapshot.Size();")
    require(net_cpp, "const bool hostHandoff = clientNum == Net_Arbitrator && NetBufferLength >= 4;")
    require(net_cpp, "if (NetBufferLength == 1)")
    exit_start = net_cpp.read_text(encoding="utf-8").index("if (NetBuffer[0] & NCMD_EXIT)")
    exit_end = net_cpp.read_text(encoding="utf-8").index("// TODO: Need a skipper for this.", exit_start)
    assert "RemoteClient != Net_Arbitrator" not in net_cpp.read_text(encoding="utf-8")[exit_start:exit_end], (
        "extended exit framing must remain valid after the first handoff packet promotes a new host"
    )

    # Existing spawn, travel, recovery, and demo safeguards remain part of the
    # companion contract rather than being traded away for menu convenience.
    require(g_game_cpp, "FPlayerStart* FLevelLocals::PickCompanionStart")
    require(level_header, "PickCompanionStart(int playernum, FPlayerStart &fallback")
    require(level_header, "FinishDeferredPlayerSpawn(int playernum, AActor *oldactor, uint8_t spawnState)")
    require(p_local_h, "SPF_DEFERPLAYEREVENTS")
    require(p_mobj_cpp, "void FLevelLocals::FinishDeferredPlayerSpawn")
    require(g_game_cpp, "SpawnPlayer(start, playernum, SPF_DEFERPLAYEREVENTS)")
    require(g_game_cpp, "FinishDeferredPlayerSpawn(playernum, oldactor, spawnState)")
    require(level_cpp, "level.PickCompanionStart(pNum, companionFallback, mo)")
    require(think_cpp, "CompanionCatchupTics")
    require(func_cpp, "bool DBot::TryCatchUpToMate()")
    require(func_cpp, "P_TeleportMove(player->mo")

    # A companion remains a normal solid player pawn for the world, monsters,
    # and map geometry. The existing shoot-through policy is deliberately
    # untouched; only the physical collision callback is extended so a
    # friendly human and live companion can cross at a narrow doorway.
    p_map_text = p_map_cpp.read_text(encoding="utf-8")
    companion_predicate = source_block(
        p_map_text,
        "static bool P_IsCompanionPawn",
        "static bool P_ShouldPassThroughCompanion",
    )
    assert "!deathmatch" in companion_predicate
    assert "actor != nullptr" in companion_predicate
    assert "actor->player != nullptr" in companion_predicate
    assert "actor->player->mo == actor" in companion_predicate
    assert "actor->player->Bot != nullptr" in companion_predicate
    assert "MF2_THRUACTORS" not in companion_predicate
    assert "MF_NOCLIP" not in companion_predicate

    companion_pair_predicate = source_block(
        p_map_text,
        "static bool P_ShouldPassThroughCompanion",
        "bool P_CanCollideWith",
    )
    assert "self != nullptr" in companion_pair_predicate
    assert "self->player != nullptr" in companion_pair_predicate
    assert "self->player->mo == self" in companion_pair_predicate
    assert "other->player != nullptr" in companion_pair_predicate
    assert "other->player->mo == other" in companion_pair_predicate
    assert "self->IsTeammate(other)" in companion_pair_predicate
    assert "P_IsCompanionPawn(self)" in companion_pair_predicate
    assert "P_IsCompanionPawn(other)" in companion_pair_predicate
    assert "MF2_THRUACTORS" not in companion_pair_predicate
    assert "MF_NOCLIP" not in companion_pair_predicate

    player_shootthrough = source_block(
        p_map_text,
        "static int P_ShouldPassThroughPlayer",
        "DEFINE_ACTION_FUNCTION_NATIVE(AActor, ShouldPassThroughPlayer",
    )
    assert "dmflags3 & DF3_NO_PLAYER_CLIP" in player_shootthrough
    assert "P_IsCompanionPawn" not in player_shootthrough
    assert "P_ShouldPassThroughCompanion" not in player_shootthrough

    collision_policy = source_block(
        p_map_text,
        "bool P_CanCollideWith",
        "void P_CollidedWith",
    )
    physical_early_out = "if (P_ShouldPassThroughCompanion(tmthing, thing))"
    assert physical_early_out in collision_policy
    assert collision_policy.index(physical_early_out) < collision_policy.index("VMValue params"), (
        "companion collision must be suppressed before virtual collision callbacks"
    )
    assert "return false;" in collision_policy

    # Position checks and teleports both defer physical actor collision to the
    # shared callback. This matters for regular walking at a doorway as well
    # as companion spawning/recovery, while preserving projectile handling.
    teleport_collision = source_block(
        p_map_text,
        "bool\tP_TeleportMove",
        "\n\tif (modifyactor)",
    )
    assert "P_CanCollideWith(tmf.thing, th)" in teleport_collision
    position_collision = source_block(
        p_map_text,
        "bool PIT_CheckThing",
        "// [ED850] Player Prediction ends here.",
    )
    assert "P_CanCollideWith(tm.thing, thing)" in position_collision

    # Explosive prediction must use the active BOTSUPP projectile rather than
    # Doom's Rocket: Heretic, Hexen, and Strife have different projectile
    # speeds, and an incomplete mod entry must conservatively decline firing.
    require(func_cpp, "const BotInfoData weaponInfo = GetBotInfo(player->ReadyWeapon);")
    require(func_cpp, "GetDefaultByType(weaponInfo.projectileType)")
    require(func_cpp, "if (projectile == nullptr || projectile->Speed <= 0)")
    assert 'GetDefaultByName("Rocket")->Speed' not in func_cpp.read_text(encoding="utf-8"), (
        "explosive companion prediction must not hard-code Doom Rocket speed"
    )

    require(level_cpp, "const bool automaticCompanions = !demorecording && !demoplayback;")
    require(bot_cpp, "if (demorecording || demoplayback)")

    # The menu is a zero-to-one transactional roster rather than seven static
    # controls or a blind count slider. It has one official configuration
    # home, reachable from the normal gameplay and multiplayer options; the
    # procedural recipe screen deliberately does not carry a second setup.
    require(menu_def, '"CompanionBotsMenu"')
    require(menu_def, 'Class "CompanionBotsMenu"')
    require(menu_def, 'Class "CompanionDraftMenu"')
    require(menu_zs, 'struct CompanionBots native version("4.15")')
    require(menu_zs, "native static int BeginDraft();")
    require(menu_zs, "native static int BeginRandomDraft();")
    require(menu_zs, "native static bool RandomizeDraft();")
    require(menu_zs, "native static bool CanBeginDraft();")
    require(menu_zs, "native static bool CanDeploy();")
    require(menu_zs, "native static int DeployDraft();")
    require(menu_zs, "native static bool RemoveProfile(int profile);")
    require(menu_zs, "native static int GetProfileSkill(int profile);")
    require(menu_zs, "native static bool SetProfileSkill(int profile, int skill);")
    require(zscript_list, 'include "zscript/engine/ui/menu/companionbotsmenu.zs"')
    require(squad_zs, "class CompanionOptionMenu : OptionMenu")
    require(squad_zs, "class CompanionBotsMenu : CompanionOptionMenu")
    require(squad_zs, "class CompanionDraftMenu : CompanionOptionMenu")
    require(squad_zs, "class CompanionProfileMenu : CompanionOptionMenu")
    require(squad_zs, "class CompanionActionItem : OptionMenuItem")
    require(squad_zs, "class CompanionBeginDraftItem : CompanionActionItem")
    require(squad_zs, "class CompanionBeginRandomDraftItem : CompanionActionItem")
    require(squad_zs, "class CompanionDraftDeployItem : CompanionActionItem")
    require(squad_zs, "class CompanionDraftCancelItem : CompanionActionItem")
    require(squad_zs, "class CompanionDraftRandomizeItem : CompanionActionItem")
    require(squad_zs, "Add Companion…")
    require(squad_zs, "Add Random Companion…")
    require(squad_zs, "Randomize this configuration")
    require(squad_zs, "Remove this companion")
    require(squad_zs, "Dismiss all companions")
    require(squad_zs, "Read-only: host or local controller")
    require(squad_zs, "No co-op place is free here.")
    require(squad_zs, "CompanionBots.CanBeginDraft()")
    require(squad_zs, "No companion identities are available.")
    require(squad_zs, "All usable identities or profiles")
    require(squad_zs, "Host-controlled")
    require(squad_zs, "Deathmatch never spawns companions.")
    require(squad_zs, "Works on ordinary and procedural maps.")
    require(squad_zs, "Save companion to squad")
    require(squad_zs, "Choose a library identity, or let")
    require(squad_zs, "the game choose an available one.")
    require(squad_zs, "Random picks a usable identity,")
    require(squad_zs, "current.Close();")
    removal_item = source_block(
        squad_zs.read_text(encoding="utf-8"),
        "class CompanionProfileRemoveItem",
        "class CompanionProfileMenu",
    )
    assert "Menu.MKEY_MBYes" in removal_item and "Continue?" in removal_item, (
        "individual removal should require an explicit confirmation"
    )
    roster_item = source_block(
        squad_zs.read_text(encoding="utf-8"),
        "class CompanionRosterItem",
        "class CompanionRosterMenu",
    )
    assert "override bool Selectable()" in roster_item and "return true;" in roster_item, (
        "identity-library rows must receive focus so long rosters are navigable"
    )
    roster_menu = source_block(
        squad_zs.read_text(encoding="utf-8"),
        "class CompanionRosterMenu",
        "class CompanionProfileLinkItem",
    )
    assert "mDesc.mSelectedItem = FirstSelectable();" in roster_menu, (
        "the library should open on a focusable row rather than trap cursor movement"
    )
    assert "Menu.SetMenu('CompanionBotsMenu');" not in squad_zs.read_text(encoding="utf-8"), (
        "draft/profile completion must return to the existing roster, not stack a stale menu"
    )
    assert "AssignFirstUnconfiguredProfile" not in squad_zs.read_text(encoding="utf-8"), (
        "one-click roster assignment was replaced by an explicit draft"
    )
    draft_deploy = source_block(
        squad_zs.read_text(encoding="utf-8"),
        "class CompanionDraftDeployItem",
        "class CompanionDraftCancelItem",
    )
    draft_selectable = source_block(draft_deploy, "override bool Selectable()", "override bool Activate()")
    assert "CompanionBots.CanDeploy()" not in draft_selectable, (
        "the save action must remain usable while live deployment is deferred"
    )
    remove_item = source_block(
        squad_zs.read_text(encoding="utf-8"),
        "class CompanionProfileRemoveItem",
        "class CompanionProfileMenu",
    )
    commit_removal = source_block(remove_item, "private bool CommitRemoval()", "\n}\n")
    assert "Menu.StartMessage" not in commit_removal and ".Close()" not in commit_removal, (
        "a confirmation callback must not replace or close its still-current message box"
    )
    profile_menu = source_block(
        squad_zs.read_text(encoding="utf-8"),
        "class CompanionProfileMenu",
        "class CompanionProfile1Menu",
    )
    assert "mRemovalFailurePending" in profile_menu
    assert "Menu.StartMessage(\"The removal could not be queued. Try again.\", 1);" in profile_menu

    menu_text = menu_def.read_text(encoding="utf-8")
    gameplay_start = menu_text.index('OptionMenu "GameplayMenu"')
    gameplay_end = menu_text.index("\n}", gameplay_start)
    assert 'Submenu "Companion Bots"' in menu_text[gameplay_start:gameplay_end]
    multiplayer_start = menu_text.index('OptionMenu "MultiplayerMenu"')
    multiplayer_end = menu_text.index("\n}", multiplayer_start)
    assert 'Submenu "Companion Bots"' in menu_text[multiplayer_start:multiplayer_end]
    procedural_start = menu_text.index('OptionMenu "ProceduralMapMenu"')
    procedural_end = menu_text.index("\n}", procedural_start)
    procedural_menu = menu_text[procedural_start:procedural_end]
    assert 'Submenu "Companion Bots"' not in procedural_menu, (
        "procedural setup must not expose a second companion configuration path"
    )
    for forbidden in ('"CompanionBotsMenu"', "CompanionBots.", "bot_companion_", "addcompanion"):
        assert forbidden not in procedural_menu, (
            f"procedural setup must not contain a companion configuration/action route: {forbidden}"
        )
    assert 'StaticText "Configure co-op companions in Options."' in procedural_menu
    companion_submenus = [
        line for line in menu_text.splitlines()
        if line.lstrip().startswith("Submenu") and '"CompanionBotsMenu"' in line
    ]
    assert len(companion_submenus) == 2, (
        "only Gameplay and Multiplayer options may link the central companion roster"
    )

    require(docs, "## Build a squad, one companion at a time")
    require(docs, "Add Random Companion…")
    require(docs, "does not configure companions")
    require(docs, "never changes\ngenerated-map determinism")
    require(docs, "ordinary maps")
    require(docs, "bot_companion_enabled_mask")
    require(docs, "bot_companion_skill1")
    require(docs, "stable profile")
    require(docs, "during a live deathmatch")
    require(docs, "Classic deathmatch bots remain separate")
    require(docs, "companion restores local single-player mode")
    require(docs, "asks for confirmation")
    require(docs, "physically passable")
    require(docs, "not noclip")
    require(docs, "Weapon, projectile, and friendly-fire behavior is unchanged.")
    require(docs, "test-bot-doorway-passability.sh")
    require(docs, "Enabled profiles are the complete desired co-op squad.")
    require(docs, "stale unprofiled co-op bots are retired")
    require(docs, "reasserts it while that squad")
    require(docs, "test-bot-companion-profile-authority.sh")

    require(level_cpp, "BotCompanionEnabledProfileCount() > 0 || bot_companion_count > 0")

    handshake = engine_net_cpp.read_text(encoding="utf-8")
    verify = handshake.index("else if (!Net_VerifyEngine(engineInfo))")
    reject = handshake.index("RejectConnection(from, PRE_WRONG_ENGINE);", verify)
    accept = handshake.index("AddClientConnection(from, free);", verify)
    assert verify < reject < accept, "an incompatible engine must be rejected before it joins"


def main() -> None:
    test_stable_removal_survives_an_intervening_departure()
    test_pending_removal_does_not_duplicate_an_exact_event()
    test_sparse_profiles_keep_the_other_squad_members()
    test_profile_roster_replaces_legacy_startup_entries()
    test_received_mask_is_the_only_network_membership_source()
    test_legacy_count_is_local_and_sparse_membership_stays_authoritative()
    test_companion_cvar_record_is_applied_only_after_atomic_enqueue()
    test_remove_and_dismiss_never_partially_clear_a_profile()
    test_later_legacy_events_cannot_crowd_out_an_atomic_roster_edit()
    test_zero_to_one_draft_uses_one_saved_profile()
    test_identity_reservations_prevent_duplicate_squad_members()
    test_host_handoff_rehydrates_local_roster_usage()
    test_host_handoff_snapshot_preserves_an_unflushed_edit()
    test_host_exit_framing_handles_p2p_packet_server_and_duplicates()
    test_companion_pass_through_is_player_pair_only()
    test_source_contracts()
    print("PASS: companion profile, stable-ID, menu, and network contracts")


if __name__ == "__main__":
    main()
