/*
**
**
**---------------------------------------------------------------------------
** Copyright 1999 Martin Colberg
** Copyright 1999-2016 Randy Heit
** Copyright 2005-2016 Christoph Oelckers
** All rights reserved.
**
** Redistribution and use in source and binary forms, with or without
** modification, are permitted provided that the following conditions
** are met:
**
** 1. Redistributions of source code must retain the above copyright
**    notice, this list of conditions and the following disclaimer.
** 2. Redistributions in binary form must reproduce the above copyright
**    notice, this list of conditions and the following disclaimer in the
**    documentation and/or other materials provided with the distribution.
** 3. The name of the author may not be used to endorse or promote products
**    derived from this software without specific prior written permission.
**
** THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
** IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
** OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
** IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
** INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
** NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
** DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
** THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
** (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
** THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
**---------------------------------------------------------------------------
**
*/
/*******************************************
* B_game.h                                 *
* Description:                             *
* Misc things that has to do with the bot, *
* like it's spawning etc.                  *
* Makes the bot fit into game              *
*                                          *
*******************************************/
/*The files which are modified for Cajun Purpose
D_player.h (v0.85: added some variables)
D_netcmd.c (v0.71)
D_netcmd.h (v0.71)
D_main.c   (v0.71)
D_ticcmd.h (v0.71)
G_game.c   (v0.95: Too make demorecording work somewhat)
G_input.c  (v0.95: added some keycommands)
G_input.h  (v0.95)
P_mobj.c   (v0.95: changed much in the P_MobjThinker(), a little in P_SpawnPlayerMissile(), maybee something else )
P_mobj.h   (v0.95: Removed some unnecessary variables)
P_user.c   (v0.95: It's only one change maybee it already was there in 0.71)
P_inter.c  (v0.95: lot of changes)
P_pspr.c   (v0.71)
P_map.c    (v0.95: Test missile for bots)
P_tick.c   (v0.95: Freeze mode things only)
P_local.h  (v0.95: added> extern int tmsectortype)
Info.c     (v0.95: maybee same as 0.71)
Info.h     (v0.95: maybee same as 0.71)
M_menu.c   (v0.95: an extra menu in the key setup with the new commands)
R_main.c   (v0.95: Fix for bot's view)
wi_stuff.c (v0.97: To remove bots correct)

(v0.85) Removed all my code from: P_enemy.c
New file: b_move.c

******************************************
What I know has to be done. in near future.

- Do some hunting/fleeing functions.
- Make the roaming 100% flawfree.
- Fix all SIGSEVS (Below is known SIGSEVS)
      -Nada (but they might be there)
******************************************
Everything that is changed is marked (maybe commented) with "Added by MC"
*/

#include "doomdef.h"
#include "p_local.h"
#include "b_bot.h"
#include "g_game.h"
#include "doomstat.h"
#include "cmdlib.h"
#include "m_misc.h"
#include "sbar.h"
#include "p_acs.h"
#include "teaminfo.h"
#include "d_net.h"
#include "d_netinf.h"
#include "d_player.h"
#include "events.h"
#include "vm.h"
#include "g_levellocals.h"
#include "r_data/sprites.h"

#if !defined _WIN32 && !defined __APPLE__
#include "i_system.h"  // for SHARE_DIR
#endif // !_WIN32 && !__APPLE__

static FRandom pr_botspawn ("BotSpawn");

EXTERN_CVAR(Int, bot_companion_count)
EXTERN_CVAR(Flag, sv_coopsharekeys)

cycle_t BotThinkCycles, BotSupportCycles;
int BotWTG;

static const char *BotConfigStrings[] =
{
	"name",
	"aiming",
	"perfection",
	"reaction",
	"isp",
	"team",
	NULL
};

enum
{
	BOTCFG_NAME,
	BOTCFG_AIMING,
	BOTCFG_PERFECTION,
	BOTCFG_REACTION,
	BOTCFG_ISP,
	BOTCFG_TEAM
};

namespace
{
	constexpr uint8_t LegacyCompanionProfile = 0xff;

	bool IsValidCompanionProfile(int profile)
	{
		return profile >= 0 && profile < FCajunMaster::MaxCompanions;
	}

	// Only enabled profiles reserve an identity. A sparse squad is intentional:
	// profile 4 can be the sole configured companion without profiles 1--3
	// accidentally consuming its named roster entry.
	bool IsEnabledProfileName(const char *name, int exceptProfile)
	{
		if (name == nullptr || name[0] == '\0')
		{
			return false;
		}
		for (int profile = 0; profile < FCajunMaster::MaxCompanions; ++profile)
		{
			const char *configuredName = BotCompanionProfileName(profile);
			if (profile != exceptProfile && BotCompanionProfileEnabled(profile) &&
				configuredName != nullptr && configuredName[0] != '\0' &&
				!stricmp(configuredName, name))
			{
				return true;
			}
		}
		return false;
	}
}

FCajunMaster::~FCajunMaster()
{
	ForgetBots();
}

//This function is called every tick (from g_game.c).
void FCajunMaster::Main(FLevelLocals *Level)
{
	BotThinkCycles.Reset();

	// Configured demos intentionally record/replay a fresh solo run. Their
	// stored companion target is ordinary server configuration, not a request
	// to synthesize DEM_ADDBOT events while recording or while parsing playback.
	if (demorecording || demoplayback || savegamerestore || gamestate != GS_LEVEL || consoleplayer != Net_Arbitrator)
		return;

	// Profile membership is the authoritative companion roster. Do not infer
	// it from a count: users may deliberately keep a sparse squad, and a
	// selected removal must stay attached to its configured profile while a
	// network add/remove is still in flight.
	BotCompanionNormalizeProfileMask();
	if (!deathmatch && (BotCompanionEnabledProfileMask() != 0 || HasProfileManagedCompanions()))
	{
		// This is normally set when a companion is added, but a packet-server
		// host handoff can promote a peer from the roster snapshot before the
		// old host's ordinary sv_coopsharekeys event reached it. Reassert the
		// co-op rule from the authoritative profile state without waiting for a
		// map transition.
		if (!sv_coopsharekeys)
			sv_coopsharekeys = true;
		if (!Level->isFrozen())
		{
			ReconcileCompanions(Level);
		}
		return;
	}

	// Legacy -bots/addbot sessions retain their count-driven behavior. New
	// companion UI flows always take the profile branch above.
	const int target = min(GetDesiredCompanionCount(), GetCompanionCapacity());
	// A classic deathmatch addbot is deliberately a live, transient entrant;
	// it is not represented by the co-op profile target until End() records
	// its name for the next deathmatch map.  Do not reinterpret that original
	// behavior as a request to remove it just because a player also has a
	// saved co-op squad.  Profile-mode co-op removal is handled by
	// ReconcileCompanions(), where it can remove an exact stable identity.
	if (!deathmatch && CountCompanions() > target)
	{
		// A cvar change can race an already-emitted DEM_ADDBOT. Reconcile the
		// reduction here as well as in the cvar callback, once that delayed add
		// has actually become a player.
		QueueCompanionRemovals(target);
	}
	else if (!companion_spawns_blocked)
	{
		const int joinedOrQueued = CountCompanions() + CountPendingCompanions();
		if (target > joinedOrQueued && !Level->isFrozen())
		{
			const int outstanding = target - joinedOrQueued;
			// A target can be raised again while an earlier removal event is in
			// flight. In that case its old countdown may already be below this
			// threshold; treating the join as overdue lets reconciliation recover
			// instead of waiting forever for an equality it has passed.
			if (t_join <= (outstanding * SPAWN_DELAY))
			{
				const char *name = spawn_tries >= 0 && (unsigned int)spawn_tries < getspawned.Size() ? getspawned[spawn_tries].GetChars() : nullptr;
				if (!SpawnBot(name, NOCOLOR, NoCompanionProfile))
				{
					// A full special-event stream is temporary. Keep both the target
					// and legacy name cursor intact so this exact add retries on the
					// next tic instead of silently dropping a companion.
					if (!spawn_event_rejected)
					{
						wanted_botnum--;
						spawn_tries++;
					}
				}
				else
				{
					spawn_tries++;
				}
			}

			t_join--;
		}
	}

	//Check if player should go observer. Or un observe
	FLinkContext ctx;
}

void FCajunMaster::Init ()
{
	// The menu/count CVar can be chosen before a run begins, while the bot
	// master is reset here. A configured profile squad is an exact, authoritative
	// co-op roster; legacy -bots state is only retained when no profile is in
	// use. Runtime changes to the CVar update wanted_botnum directly, so this
	// does not resurrect companions the player later dismissed.
	BotCompanionNormalizeProfileMask();
	// Saved companion profiles are a co-op feature. Preserve classic transient
	// deathmatch bot targets, but never turn a previously configured squad into
	// deathmatch entrants merely because its archived CVar is still nonzero.
	if (!deathmatch)
	{
		const int profileCount = BotCompanionEnabledProfileCount();
		SetDesiredCompanionCount(profileCount != 0 ? profileCount : wanted_botnum);
	}
	else if (BotCompanionEnabledProfileMask() != 0 && getspawned.Size() == 0)
	{
		// The scalar target may still contain the count from the preceding co-op
		// map. Do not reinterpret that persistent squad as classic deathmatch
		// bots on a mode switch. A later co-op Init() rebuilds its target from
		// the saved profile mask. Conversely, a populated getspawned list is
		// the original deathmatch continuation contract (including -bots), and
		// must win here so a saved co-op squad never erases classic DM bots.
		SetDesiredCompanionCount(0);
	}
	else
	{
		SetDesiredCompanionCount(wanted_botnum);
	}
	if (GetDesiredCompanionCount() > 0 && !deathmatch && !demorecording && !demoplayback)
		sv_coopsharekeys = true;
	botnum = 0;
	pending_companion_removals.Clear();
	companion_spawns_blocked = false;
	companion_forced_multiplayer = G_IsAutomaticCompanionMultiplayerMode();
	firstthing = nullptr;
	spawn_tries = 0;
	body1 = nullptr;
	body2 = nullptr;

	if (ctf && teamplay == false)
		teamplay = true; //Need teamplay for ctf. (which is not done yet)

	t_join = (GetDesiredCompanionCount() + 1) * SPAWN_DELAY; //The + is to let player get away before the bots come in.

	if (botinfo == NULL)
	{
		LoadBots ();
	}
	else
	{
		botinfo_t *thebot = botinfo;

		while (thebot != NULL)
		{
			thebot->inuse = BOTINUSE_No;
			thebot->companionProfile = NoCompanionProfile;
			thebot->companionId = NoCompanionId;
			thebot = thebot->next;
		}
	}
}

void FCajunMaster::BeginCompanionMap()
{
	// A failure means that this map has exhausted every collision-clear
	// companion candidate. It must not permanently change the player's desired
	// squad, but retrying while the same geometry is loaded just queues an
	// endless stream of doomed DEM_ADDBOT events.
	companion_spawns_blocked = false;
	// World transitions discard queued special events. A removal reservation
	// left behind by a command that lost that race would otherwise convince the
	// new map that a disabled profile is still being removed, stranding its
	// travelling bot forever. Live stable IDs are re-evaluated by Main().
	pending_companion_removals.Clear();
	for (botinfo_t *bot = botinfo; bot != nullptr; bot = bot->next)
	{
		// Likewise, a waiting reservation only represents a DEM_ADDBOT that has
		// not yet made a player pawn. If the transition discarded that event,
		// release it now so the desired profile can be retried on this new map.
		// An already joined companion has a stable live ID and remains reserved.
		if (bot->inuse == BOTINUSE_Waiting && GetCompanionSlotById(bot->companionId) < 0)
		{
			bot->inuse = BOTINUSE_No;
			bot->companionProfile = NoCompanionProfile;
			bot->companionId = NoCompanionId;
		}
	}
}

void FCajunMaster::RejectUnplaceableCompanion(FLevelLocals *Level, unsigned int player, bool notifyDisconnect)
{
	if (Level == nullptr || player >= MAXPLAYERS || !playeringame[player] || players[player].Bot == nullptr)
	{
		return;
	}

	companion_spawns_blocked = true;
	if (notifyDisconnect && players[player].mo != nullptr)
	{
		// Existing companions have already entered the world, so preserve the
		// normal disconnect semantics while removing them transactionally.
		RemoveCompanionAt(Level, player, true);
	}
	else
	{
		// A fresh DEM_ADDBOT has not fired PlayerEntered yet. Clearing it without
		// a disconnect avoids presenting a phantom player to event handlers.
		ClearPlayer(player, true);
		botnum = CountCompanions();
	}
}

//Called on each level exit (from g_game.c).
void FCajunMaster::End ()
{
	unsigned int i;

	//Arrange wanted botnum and their names, so they can be spawned next level.
	getspawned.Clear();
	if (deathmatch)
	{
		for (i = 0; i < MAXPLAYERS; i++)
		{
			if (players[i].Bot != NULL)
			{
				getspawned.Push(players[i].userinfo.GetName());
			}
		}

		wanted_botnum = botnum;
	}
}

int FCajunMaster::GetDesiredCompanionCount() const
{
	// Once a profile-managed companion exists, the mask is the only source of
	// truth. This lets clearing the final selected profile dismiss it instead
	// of reviving an old scalar target on the next map.
	if (!deathmatch && (BotCompanionEnabledProfileMask() != 0 || HasProfileManagedCompanions()))
	{
		return BotCompanionEnabledProfileCount();
	}
	return clamp(wanted_botnum, 0, MaxCompanions);
}

void FCajunMaster::SetDesiredCompanionCount(int count)
{
	wanted_botnum = clamp(count, 0, MaxCompanions);
}

int FCajunMaster::CountRosterEntries() const
{
	int count = 0;
	for (const botinfo_t *bot = botinfo; bot != nullptr; bot = bot->next)
	{
		count++;
	}
	return count;
}

int FCajunMaster::GetCompanionCapacity() const
{
	int humans = 0;
	for (unsigned int i = 0; i < MAXPLAYERS; ++i)
	{
		if (playeringame[i] && players[i].Bot == nullptr)
		{
			humans++;
		}
	}

	int capacity = min(MaxCompanions, max(0, MaxCoopParticipants - humans));
	// Before the first level the roster is intentionally not loaded yet. Once
	// it has been resolved, an external roster may impose a smaller practical
	// ceiling than the multiplayer policy does.
	if (roster_load_attempted)
	{
		capacity = min(capacity, CountRosterEntries());
	}
	return capacity;
}

int FCajunMaster::CountCompanions() const
{
	int count = 0;
	for (unsigned int i = 0; i < MAXPLAYERS; ++i)
	{
		if (playeringame[i] && players[i].Bot != nullptr)
		{
			count++;
		}
	}
	return count;
}

int FCajunMaster::CountPendingCompanions() const
{
	int count = 0;
	for (const botinfo_t *bot = botinfo; bot != nullptr; bot = bot->next)
	{
		if (bot->inuse == BOTINUSE_Waiting)
		{
			count++;
		}
	}
	return count;
}

int FCajunMaster::GetRosterCount() const
{
	return CountRosterEntries();
}

const char *FCajunMaster::GetRosterName(int index) const
{
	if (index < 0)
	{
		return "";
	}
	for (const botinfo_t *bot = botinfo; bot != nullptr; bot = bot->next)
	{
		if (index-- == 0)
		{
			return bot->Name.GetChars();
		}
	}
	return "";
}

int FCajunMaster::GetRosterState(int index) const
{
	if (index < 0)
	{
		return BOTINUSE_No;
	}
	for (const botinfo_t *bot = botinfo; bot != nullptr; bot = bot->next)
	{
		if (index-- == 0)
		{
			return bot->inuse;
		}
	}
	return BOTINUSE_No;
}

void FCajunMaster::RehydrateCompanionRosterUsage()
{
	// Each machine has its own roster object. DEM_ADDBOT historically marked
	// the source machine's entry only, which is normally fine until that
	// source leaves and a client becomes the host. Do not let the promoted
	// host's untouched roster select an identity that is already playing.
	for (botinfo_t *bot = botinfo; bot != nullptr; bot = bot->next)
	{
		// Waiting entries belonged to the previous host's event stream. A new
		// host will issue any still-needed addition itself, after checking the
		// live players below.
		bot->inuse = BOTINUSE_No;
		bot->companionProfile = NoCompanionProfile;
		bot->companionId = NoCompanionId;
	}

	for (unsigned int i = 0; i < MAXPLAYERS; ++i)
	{
		if (!playeringame[i] || players[i].Bot == nullptr)
		{
			continue;
		}

		const char *name = players[i].userinfo.GetName();
		for (botinfo_t *bot = botinfo; bot != nullptr; bot = bot->next)
		{
			if (!stricmp(name, bot->Name.GetChars()))
			{
				// Mark every same-named entry. A malformed external roster with
				// duplicate names must still never produce a duplicate player.
				bot->inuse = BOTINUSE_Yes;
				bot->companionProfile = players[i].Bot->CompanionProfile;
				bot->companionId = players[i].Bot->CompanionJoinOrder;
			}
		}
	}

	// Removal records scheduled by the previous host cannot be owned by the
	// promoted machine. Live stable IDs are authoritative; Main() reconciles
	// profile membership after the handoff.
	pending_companion_removals.Clear();
	botnum = CountCompanions();
}

int FCajunMaster::QueueCompanionRemovals(int target)
{
	const int active = CountCompanions();
	target = clamp(target, 0, active);

	// Legacy count callers still dismiss the newest members first, but choose
	// their stable IDs before writing any events. Ordinal records were unsafe:
	// an unrelated removal could shift the requested target between queueing
	// and receipt on another peer.
	const int requested = active - target;
	int queued = 0;
	while (queued < requested)
	{
		uint32_t newest = NoCompanionId;
		for (unsigned int i = 0; i < MAXPLAYERS; ++i)
		{
			if (!playeringame[i] || players[i].Bot == nullptr)
			{
				continue;
			}
			const uint32_t companionId = players[i].Bot->CompanionJoinOrder;
			if (companionId != NoCompanionId && !IsCompanionRemovalPending(companionId) &&
				companionId > newest)
			{
				newest = companionId;
			}
		}
		if (newest == NoCompanionId || !QueueCompanionRemoval(newest))
		{
			break;
		}
		++queued;
	}
	return queued;
}

bool FCajunMaster::IsCompanionRemovalPending(uint32_t companionId) const
{
	return companionId != NoCompanionId &&
		pending_companion_removals.Find(companionId) < pending_companion_removals.Size();
}

bool FCajunMaster::QueueCompanionRemoval(uint32_t companionId)
{
	if (companionId == NoCompanionId || IsCompanionRemovalPending(companionId))
	{
		return false;
	}

	const uint8_t event[] =
	{
		DEM_REMOVECOMPANION,
		(uint8_t)(companionId >> 24),
		(uint8_t)(companionId >> 16),
		(uint8_t)(companionId >> 8),
		(uint8_t)companionId,
	};
	if (!Net_WriteEventAtomic(event, countof(event)))
	{
		return false;
	}
	pending_companion_removals.Push(companionId);
	return true;
}

void FCajunMaster::NotifyCompanionRemovalProcessed(uint32_t companionId)
{
	// Only the current host owns locally queued events. Clients execute the
	// record too, but have no reservation to release.
	if ((!netgame || consoleplayer == Net_Arbitrator) && companionId != NoCompanionId)
	{
		const unsigned int index = pending_companion_removals.Find(companionId);
		if (index < pending_companion_removals.Size())
		{
			pending_companion_removals.Delete(index);
		}
	}
}

uint32_t FCajunMaster::AllocateCompanionJoinOrder()
{
	// The ordinary path simply advances this counter. If a marathon host ever
	// wraps the full uint32 range, scan the tiny live/pending roster instead of
	// reusing ID 1 merely because UINT32_MAX happened to be active.
	const int attempts = MAXPLAYERS + CountRosterEntries() + 1;
	for (int attempt = 0; attempt < attempts; ++attempt)
	{
		if (next_companion_join_order == NoCompanionId)
		{
			next_companion_join_order = 1;
		}

		const uint32_t candidate = next_companion_join_order++;
		bool used = GetCompanionSlotById(candidate) >= 0;
		for (const botinfo_t *bot = botinfo; !used && bot != nullptr; bot = bot->next)
		{
			used = bot->inuse == BOTINUSE_Waiting && bot->companionId == candidate;
		}
		if (!used)
		{
			return candidate;
		}
	}
	return NoCompanionId;
}

int FCajunMaster::GetCompanionSlotByOrdinal(unsigned int ordinal) const
{
	if (ordinal >= (unsigned int)CountCompanions())
	{
		return -1;
	}

	uint32_t priorOrder = 0;
	int priorSlot = -1;
	for (unsigned int current = 0; current <= ordinal; ++current)
	{
		int selected = -1;
		uint32_t selectedOrder = 0;
		for (unsigned int i = 0; i < MAXPLAYERS; ++i)
		{
			if (!playeringame[i] || players[i].Bot == nullptr)
			{
				continue;
			}

			const uint32_t order = players[i].Bot->CompanionJoinOrder;
			if (order < priorOrder || (order == priorOrder && (int)i <= priorSlot))
			{
				continue;
			}
			if (selected < 0 || order < selectedOrder || (order == selectedOrder && i < (unsigned int)selected))
			{
				selected = (int)i;
				selectedOrder = order;
			}
		}
		if (selected < 0)
		{
			return -1;
		}
		if (current == ordinal)
		{
			return selected;
		}
		priorOrder = selectedOrder;
		priorSlot = selected;
	}
	return -1;
}

int FCajunMaster::GetCompanionSlotById(uint32_t companionId) const
{
	if (companionId == NoCompanionId)
	{
		return -1;
	}
	for (unsigned int i = 0; i < MAXPLAYERS; ++i)
	{
		if (playeringame[i] && players[i].Bot != nullptr &&
			players[i].Bot->CompanionJoinOrder == companionId)
		{
			return (int)i;
		}
	}
	return -1;
}

int FCajunMaster::GetCompanionSlotByProfile(int profile) const
{
	if (!IsValidCompanionProfile(profile))
	{
		return -1;
	}
	for (unsigned int i = 0; i < MAXPLAYERS; ++i)
	{
		if (playeringame[i] && players[i].Bot != nullptr &&
			players[i].Bot->CompanionProfile == profile)
		{
			return (int)i;
		}
	}
	return -1;
}

uint32_t FCajunMaster::GetCompanionIdByProfile(int profile) const
{
	const int player = GetCompanionSlotByProfile(profile);
	return player >= 0 ? players[player].Bot->CompanionJoinOrder : NoCompanionId;
}

const char *FCajunMaster::GetCompanionNameByProfile(int profile) const
{
	const int player = GetCompanionSlotByProfile(profile);
	return player >= 0 ? players[player].userinfo.GetName() : "";
}

bool FCajunMaster::IsCompanionProfilePending(int profile) const
{
	if (!IsValidCompanionProfile(profile))
	{
		return false;
	}
	for (const botinfo_t *bot = botinfo; bot != nullptr; bot = bot->next)
	{
		if (bot->inuse == BOTINUSE_Waiting && bot->companionProfile == profile)
		{
			return true;
		}
	}
	return false;
}

bool FCajunMaster::HasProfileManagedCompanions() const
{
	for (unsigned int i = 0; i < MAXPLAYERS; ++i)
	{
		if (playeringame[i] && players[i].Bot != nullptr &&
			IsValidCompanionProfile(players[i].Bot->CompanionProfile))
		{
			return true;
		}
	}
	for (const botinfo_t *bot = botinfo; bot != nullptr; bot = bot->next)
	{
		if (bot->inuse == BOTINUSE_Waiting && IsValidCompanionProfile(bot->companionProfile))
		{
			return true;
		}
	}
	return false;
}

bool FCajunMaster::RemoveCompanionAt(FLevelLocals *Level, unsigned int player, bool keepTeam)
{
	if (Level == nullptr || player >= MAXPLAYERS || !playeringame[player] || players[player].Bot == nullptr)
	{
		return false;
	}

	// If a human is observing the departing companion, restore their own
	// camera before tearing down the pawn. This mirrors the legacy all-bots
	// path, but makes an individual removal safe.
	for (unsigned int observer = 0; observer < MAXPLAYERS; ++observer)
	{
		if (observer != player && Level->PlayerInGame(observer) && players[observer].Bot == nullptr &&
			players[observer].camera == players[player].mo)
		{
			players[observer].camera = players[observer].mo;
			if (Level->isConsolePlayer(players[observer].mo))
			{
				StatusBar->AttachToPlayer(&players[observer]);
			}
		}
	}

	Level->localEventManager->PlayerDisconnected(player);
	Level->Behaviors.StartTypedScripts(SCRIPT_Disconnect, players[player].mo, true, player, true);
	ClearPlayer(player, keepTeam);
	botnum = CountCompanions();
	RestoreSinglePlayerModeIfIdle();
	return true;
}

void FCajunMaster::RestoreSinglePlayerModeIfIdle()
{
	// Only undo a promotion that DoAddBot made for a local single-player map.
	// An ordinary co-op launch, a network game, a deathmatch, or a map with
	// another real player owns multiplayer mode independently of companions.
	if (!companion_forced_multiplayer || CountCompanions() != 0 ||
		CountPendingCompanions() != 0 || GetDesiredCompanionCount() != 0 ||
		netgame || deathmatch)
	{
		return;
	}

	int humanPlayers = 0;
	for (unsigned int player = 0; player < MAXPLAYERS; ++player)
	{
		if (playeringame[player] && players[player].Bot == nullptr)
		{
			++humanPlayers;
		}
	}
	if (humanPlayers <= 1)
	{
		multiplayer = false;
		companion_forced_multiplayer = false;
	}
}

bool FCajunMaster::RemoveCompanion(FLevelLocals *Level, unsigned int ordinal, bool keepTeam)
{
	const int player = GetCompanionSlotByOrdinal(ordinal);
	return player >= 0 && RemoveCompanionAt(Level, (unsigned int)player, keepTeam);
}

bool FCajunMaster::RemoveCompanionById(FLevelLocals *Level, uint32_t companionId, bool keepTeam)
{
	const int player = GetCompanionSlotById(companionId);
	return player >= 0 && RemoveCompanionAt(Level, (unsigned int)player, keepTeam);
}

bool FCajunMaster::ReconcileCompanions(FLevelLocals *Level)
{
	if (Level == nullptr || deathmatch)
	{
		return false;
	}

	BotCompanionNormalizeProfileMask();
	const bool profileMode = BotCompanionEnabledProfileMask() != 0 || HasProfileManagedCompanions();
	if (!profileMode)
	{
		// Preserve startup lists and old addbot behavior. These have no stable
		// profile membership, so they deliberately stay count based.
		const int target = min(GetDesiredCompanionCount(), GetCompanionCapacity());
		if (CountCompanions() > target)
		{
			return QueueCompanionRemovals(target) > 0;
		}
		if (companion_spawns_blocked)
		{
			return false;
		}

		bool changed = false;
		int attempts = CountRosterEntries() + 1;
		while (CountCompanions() + CountPendingCompanions() < target && attempts-- > 0)
		{
			const char *name = spawn_tries >= 0 && (unsigned int)spawn_tries < getspawned.Size() ? getspawned[spawn_tries].GetChars() : nullptr;
			spawn_tries++;
			if (SpawnBot(name, NOCOLOR, NoCompanionProfile))
			{
				changed = true;
			}
			else if (spawn_event_rejected)
			{
				spawn_tries--;
				break;
			}
			else if (name == nullptr)
			{
				break;
			}
		}
		return changed;
	}

	// Once profile mode is selected it owns the complete cooperative roster.
	// In particular, do not combine the old -bots startup list with configured
	// profiles: the two independent requests used to make a two-profile squad
	// appear as six or more live bots. Clear the local startup cursor and return
	// any already queued or live unprofiled entrants through their stable IDs.
	// This work remains host-only (Main/ApplyCompanionTargetNow enforce that),
	// so no local-only startup list needs to be replicated to guests.
	bool changed = false;
	if (getspawned.Size() != 0)
	{
		getspawned.Clear();
		spawn_tries = 0;
		changed = true;
	}
	SetDesiredCompanionCount(BotCompanionEnabledProfileCount());

	for (unsigned int i = 0; i < MAXPLAYERS; ++i)
	{
		if (playeringame[i] && players[i].Bot != nullptr &&
			!IsValidCompanionProfile(players[i].Bot->CompanionProfile))
		{
			const uint32_t companionId = players[i].Bot->CompanionJoinOrder;
			if (companionId != NoCompanionId)
			{
				changed = QueueCompanionRemoval(companionId) || changed;
			}
		}
	}
	for (const botinfo_t *bot = botinfo; bot != nullptr; bot = bot->next)
	{
		if (bot->inuse == BOTINUSE_Waiting && !IsValidCompanionProfile(bot->companionProfile) &&
			bot->companionId != NoCompanionId)
		{
			// A queued DEM_ADDBOT is followed by this exact removal record on every
			// peer, so it cannot consume a slot after the profile roster takes over.
			changed = QueueCompanionRemoval(bot->companionId) || changed;
		}
	}

	bool desired[MaxCompanions] = {};
	const int profileCapacity = GetCompanionCapacity();
	int selectedProfiles = 0;
	for (int profile = 0; profile < MaxCompanions; ++profile)
	{
		if (BotCompanionProfileEnabled(profile) && selectedProfiles < profileCapacity)
		{
			desired[profile] = true;
			++selectedProfiles;
		}
	}

	for (int profile = 0; profile < MaxCompanions; ++profile)
	{
		if (desired[profile])
		{
			continue;
		}

		const uint32_t activeId = GetCompanionIdByProfile(profile);
		if (activeId != NoCompanionId)
		{
			changed = QueueCompanionRemoval(activeId) || changed;
		}
		for (const botinfo_t *bot = botinfo; bot != nullptr; bot = bot->next)
		{
			if (bot->inuse == BOTINUSE_Waiting && bot->companionProfile == profile &&
				bot->companionId != NoCompanionId)
			{
				changed = QueueCompanionRemoval(bot->companionId) || changed;
			}
		}
	}

	if (companion_spawns_blocked)
	{
		return changed;
	}

	for (int profile = 0; profile < MaxCompanions; ++profile)
	{
		if (!desired[profile] || GetCompanionSlotByProfile(profile) >= 0 ||
			IsCompanionProfilePending(profile))
		{
			continue;
		}
		// Wait for a previously queued exact removal to apply rather than exceed
		// the player-capacity invariant for a tic just to replace its profile.
		if (CountCompanions() + CountPendingCompanions() >= GetCompanionCapacity())
		{
			break;
		}
		if (SpawnConfiguredCompanion(profile))
		{
			changed = true;
		}
		else if (spawn_event_rejected)
		{
			break;
		}
	}
	return changed;
}

bool FCajunMaster::SpawnConfiguredCompanion(int profile, const char *legacyName)
{
	// An explicit console or startup-list name remains an explicit request.
	// The numbered profile still supplies its skin/style overlay, but it must
	// not silently replace `addcompanion SomeName` with a different configured
	// identity.
	if (legacyName != nullptr && legacyName[0] != '\0')
	{
		return SpawnBot(legacyName, NOCOLOR, profile);
	}

	const char *configuredName = BotCompanionProfileName(profile);
	if (configuredName != nullptr && configuredName[0] != '\0')
	{
		if (SpawnBot(configuredName, NOCOLOR, profile))
		{
			return true;
		}
		if (spawn_event_rejected)
		{
			// A packet-budget rejection is not a stale profile. Preserve this
			// identity and let the normal host reconciler retry it next tic.
			return false;
		}
		// A stale profile (for example, after removing an external bots.cfg
		// entry) must never prevent the rest of the configured squad from
		// entering a normal map. Keep the profile's appearance and use an
		// otherwise available identity instead.
		Printf(TEXTCOLOR_YELLOW "Companion profile %d could not use '%s'; selecting an available identity.\n",
			profile + 1, configuredName);
	}

	// Do not leave the stale name in the profile and randomly roll a different
	// bot on every future map. Commit a valid fallback before queuing its bot
	// event: if this tic has no room for the add event, reconciliation retries
	// the same saved identity next tic rather than creating a transient bot
	// whose replacement identity was never replicated.
	for (int index = 0; index < GetRosterCount(); ++index)
	{
		const char *candidate = GetRosterName(index);
		if (GetRosterState(index) != BOTINUSE_No || IsEnabledProfileName(candidate, profile))
		{
			continue;
		}
		if (!BotCompanionSetProfileName(profile, candidate))
		{
			Printf(TEXTCOLOR_YELLOW "Companion profile %d could not save its fallback identity; retrying.\n",
				profile + 1);
			return false;
		}
		if (SpawnBot(candidate, NOCOLOR, profile))
		{
			return true;
		}
		if (spawn_event_rejected)
		{
			return false;
		}
	}
	return false;
}



//Name can be optional, if = NULL
//then a random bot is spawned.
//If no bot with name = name found
//the function will CONS print an
//error message and will not spawn
//anything.
//The color parameter can be either a
//color (range from 0-10), or = NOCOLOR.
//The color parameter overides bots
//individual colors if not = NOCOLOR.

bool FCajunMaster::SpawnBot (const char *name, int color, int companionProfile)
{
	spawn_event_rejected = false;
	if (companionProfile != NoCompanionProfile && !IsValidCompanionProfile(companionProfile))
	{
		return false;
	}

	//COLORS
	static const char colors[11][17] =
	{
		"\\color\\40 cf 00",	//0  = Green
		"\\color\\b0 b0 b0",	//1  = Gray
		"\\color\\50 50 60",	//2  = Indigo
		"\\color\\8f 00 00",	//3  = Deep Red
		"\\color\\ff ff ff",	//4  = White
		"\\color\\ff af 3f",	//5  = Bright Brown
		"\\color\\bf 00 00",	//6  = Red
		"\\color\\00 00 ff",	//7  = Blue
		"\\color\\00 00 7f",	//8  = Dark Blue
		"\\color\\ff ff 00",	//9  = Yellow
		"\\color\\cf df 90"		//10 = Bleached Bone
	};

	botinfo_t *thebot = botinfo;
	int botshift = 0;

	if (name)
	{
		// Check if exist or already in the game.
		while (thebot && thebot->Name.CompareNoCase(name))
		{
			botshift++;
			thebot = thebot->next;
		}

		if (thebot == NULL)
		{
   		 	Printf ("couldn't find %s in %s\n", name, BOTFILENAME);
			return false;
		}
		else if (thebot->inuse == BOTINUSE_Waiting)
		{
			return false;
		}
		else if (thebot->inuse == BOTINUSE_Yes)
		{
   		 	Printf ("%s is already in the thick\n", name);
			return false;
		}
	}
	else
	{
		//Spawn a random bot from bots.cfg if no name given.
		TArray<botinfo_t *> BotInfoAvailable;
		while (thebot)
		{
			if (thebot->inuse == BOTINUSE_No &&
				!IsEnabledProfileName(thebot->Name.GetChars(), companionProfile))
				BotInfoAvailable.Push (thebot);

			thebot = thebot->next;
		}

		if (BotInfoAvailable.Size () == 0)
		{
			Printf ("Couldn't spawn bot; no bot left in %s\n", BOTFILENAME);
			return false;
		}

		thebot = BotInfoAvailable[pr_botspawn() % BotInfoAvailable.Size ()];

		botinfo_t *thebot2 = botinfo;
		while (thebot2)
		{
			if (thebot == thebot2)
				break;

			botshift++;
			thebot2 = thebot2->next;
		}
	}

	// A profile's explicit style wins over the legacy global one-shot
	// colour. Style 0 deliberately leaves the bot roster's own colour
	// alone; this preserves custom bots.cfg appearance defaults.
	FString concat = thebot->Info;
	int selectedColor = color;
	const int profileStyle = BotCompanionProfileStyle(companionProfile);
	if (profileStyle > 0)
	{
		selectedColor = profileStyle - 1;
	}
	if (selectedColor >= 0 && selectedColor < NOCOLOR)
	{
		concat << colors[selectedColor];
	}
	else if (selectedColor == NOCOLOR && bot_next_color < NOCOLOR && bot_next_color >= 0)
	{
		concat << colors[bot_next_color];
	}
	const char *profileSkin = BotCompanionProfileSkin(companionProfile);
	if (profileSkin != nullptr && profileSkin[0] != '\0')
	{
		// Only write a canonical skin name sourced from the active skin list.
		// That keeps profile CVars from becoming an unescaped userinfo path.
		for (const FPlayerSkin &skin : Skins)
		{
			if (!skin.Name.CompareNoCase(profileSkin))
			{
				concat << "\\skin\\" << skin.Name;
				break;
			}
		}
	}
	if (FTeam::IsValid (thebot->lastteam))
	{ // Keep the bot on the same team when switching levels
		concat.AppendFormat("\\team\\%d\n", thebot->lastteam);
	}

	// DEM_ADDBOT has a variable-length userinfo field. It must enter the
	// shared special-event stream as one record: legacy field-by-field writes
	// can leave a trailing command/shift without a terminating string when a
	// crowded tic reaches its packet budget, and receivers then over-read it.
	botskill_t skill = thebot->skill;
	BotApplyCompanionSkillPreset(companionProfile, skill);
	const uint32_t companionId = AllocateCompanionJoinOrder();
	if (companionId == NoCompanionId)
	{
		Printf(TEXTCOLOR_RED "Couldn't allocate a stable companion identity.\n");
		return false;
	}

	// The profile/ID tail was added with DEMOGAMEVERSION 0x224. A byte value
	// of 0xff retains compatibility for ordinary legacy addbot callers; every
	// bot still receives a nonzero stable ID so it can be removed safely.
	const size_t eventSize = 2 + concat.Len() + 1 + 4 + 1 + 4;
	if (eventSize > NetAtomicEventMaxSize)
	{
		// An external bots.cfg can contain arbitrarily long userinfo. This is
		// permanent for this identity, unlike a temporarily busy tic, so do not
		// set spawn_event_rejected and make Main retry it forever.
		Printf(TEXTCOLOR_RED "Couldn't spawn %s: its bot configuration is too large to replicate.\n",
			thebot->Name.GetChars());
		return false;
	}
	TArray<uint8_t> event;
	event.Resize((unsigned)eventSize);
	TArrayView<uint8_t> stream(event.Data(), event.Size());
	WriteInt8(DEM_ADDBOT, stream);
	WriteInt8((uint8_t)botshift, stream);
	WriteString(concat.GetChars(), stream);
	WriteInt8(skill.aiming, stream);
	WriteInt8(skill.perfection, stream);
	WriteInt8(skill.reaction, stream);
	WriteInt8(skill.isp, stream);
	WriteInt8(IsValidCompanionProfile(companionProfile) ? (uint8_t)companionProfile : LegacyCompanionProfile, stream);
	WriteInt32((int32_t)companionId, stream);
	if (!Net_WriteEventAtomic(event.Data(), (int)event.Size()))
	{
		spawn_event_rejected = true;
		return false;
	}

	// Reserve the identity only after its complete record was accepted. A
	// rejected record leaves no bytes behind, so it must not leave a phantom
	// pending companion behind either.
	thebot->inuse = BOTINUSE_Waiting;
	thebot->companionProfile = companionProfile;
	thebot->companionId = companionId;

	return true;
}

void FCajunMaster::TryAddBot (FLevelLocals *Level, TArrayView<uint8_t>& stream, int player)
{
	int botshift = ReadInt8 (stream);
	char *info = ReadString (stream);
	botskill_t skill;
	skill.aiming = ReadInt8 (stream);
	skill.perfection = ReadInt8 (stream);
	skill.reaction = ReadInt8 (stream);
	skill.isp = ReadInt8 (stream);
	const uint8_t profileByte = ReadInt8(stream);
	const int companionProfile = profileByte == LegacyCompanionProfile ? NoCompanionProfile : (int)profileByte;
	const uint32_t companionId = (uint32_t)ReadInt32(stream);

	if ((companionProfile != NoCompanionProfile && !IsValidCompanionProfile(companionProfile)) ||
		companionId == NoCompanionId)
	{
		Printf(TEXTCOLOR_RED "Ignoring malformed DEM_ADDBOT companion identity.\n");
		delete[] info;
		return;
	}

	botinfo_t *thebot = NULL;

	if (consoleplayer == player)
	{
		thebot = botinfo;

		// DEM_ADDBOT stores a roster offset in one byte. A malformed or
		// stale host record must not walk past this machine's local roster and
		// dereference nullptr before the normal add path can consume it.
		while (botshift > 0 && thebot != nullptr)
		{
			thebot = thebot->next;
			botshift--;
		}
	}

	if (DoAddBot (Level, TArrayView((uint8_t*)info, strlen(info)+1), skill, companionProfile, companionId))
	{
		//Increment this.
		botnum++;

		if (thebot != NULL)
		{
			thebot->inuse = BOTINUSE_Yes;
			thebot->companionProfile = companionProfile;
			thebot->companionId = companionId;
		}
	}
	else
	{
		if (thebot != NULL)
		{
			thebot->inuse = BOTINUSE_No;
			thebot->companionProfile = NoCompanionProfile;
			thebot->companionId = NoCompanionId;
		}
	}

	delete[] info;
}

bool FCajunMaster::DoAddBot (FLevelLocals *Level, TArrayView<uint8_t> info, botskill_t skill,
	int companionProfile, uint32_t companionId)
{
	const bool wasMultiplayer = multiplayer;
	const bool promotesLocalSinglePlayer = !wasMultiplayer && !netgame && !deathmatch;
	unsigned int bnum;
	if ((companionProfile != NoCompanionProfile && !IsValidCompanionProfile(companionProfile)) ||
		companionId == NoCompanionId || GetCompanionSlotById(companionId) >= 0 ||
		(IsValidCompanionProfile(companionProfile) && GetCompanionSlotByProfile(companionProfile) >= 0))
	{
		return false;
	}

	for (bnum = 0; bnum < MAXPLAYERS; bnum++)
	{
		if (!playeringame[bnum])
		{
			break;
		}
	}

	if (bnum == MAXPLAYERS)
	{
		Printf ("The maximum of %lu players/bots has been reached\n", MAXPLAYERS);
		return false;
	}

	D_ReadUserInfoStrings (bnum, info, false);

	multiplayer = true; //Prevents cheating and so on; emulates real netgame (almost).
	players[bnum].Bot = Level->CreateThinker<DBot>();
	players[bnum].Bot->player = &players[bnum];
	players[bnum].Bot->skill = skill;
	players[bnum].Bot->CompanionProfile = companionProfile;
	players[bnum].Bot->CompanionJoinOrder = companionId;
	playeringame[bnum] = true;
	players[bnum].mo = NULL;
	players[bnum].playerstate = PST_ENTER;

	if (teamplay)
		Printf ("%s joined the %s team\n", players[bnum].userinfo.GetName(), Teams[players[bnum].userinfo.GetTeam()].GetName());
	else
		Printf ("%s joined the game\n", players[bnum].userinfo.GetName());

	Level->DoReborn(bnum, false, true);
	if (!playeringame[bnum] || players[bnum].mo == nullptr)
	{
		// DoReborn discarded an impossible fresh spawn before PlayerEntered.
		// Tell TryAddBot to release the roster's Waiting entry and avoid an
		// accounting increment for a companion that never joined. Do not leave
		// a solo map in co-op mode solely because this failed addition briefly
		// needed normal co-op spawn semantics.
		multiplayer = wasMultiplayer;
		return false;
	}
	if (promotesLocalSinglePlayer)
	{
		companion_forced_multiplayer = true;
	}
	Level->localEventManager->PlayerEntered(bnum, false);
	return true;
}

void FCajunMaster::RemoveAllBots (FLevelLocals *Level, bool fromlist)
{
	while (CountCompanions() > 0)
	{
		if (!RemoveCompanion(Level, 0, !fromlist))
		{
			break;
		}
	}

	if (fromlist)
	{
		wanted_botnum = 0;
	}
	botnum = CountCompanions();
	RestoreSinglePlayerModeIfIdle();
}


//------------------
//Reads data for bot from
//a .bot file.
//The skills and other data should
//be arranged as follows in the bot file:
//
//{
// Name			bot's name
// Aiming		0-100
// Perfection	0-100
// Reaction		0-100
// Isp			0-100 (Instincts of Self Preservation)
// ???			any other valid userinfo strings can go here
//}

static void appendinfo (FString &front, const char *back)
{
	front << "\\" << back;
}

int FCajunMaster::LoadBuiltInBots()
{
	// These identities deliberately carry only generic userinfo. In particular,
	// they do not pin PlayerClass: the active game's normal class selection
	// supplies Doom, Heretic, Hexen, or Strife's appropriate player pawn.
	struct BuiltInBot
	{
		const char *name;
		botskill_t skill;
	};
	static const BuiltInBot BuiltInRoster[] =
	{
		{ "Aster",   { 68, 52, 54, 70 } },
		{ "Beacon",  { 58, 64, 48, 76 } },
		{ "Cinder",  { 74, 46, 62, 58 } },
		{ "Delta",   { 62, 60, 56, 66 } },
		{ "Ember",   { 54, 70, 44, 80 } },
		{ "Flint",   { 78, 42, 68, 54 } },
		{ "Harbor",  { 60, 58, 52, 74 } },
		{ "Lumen",   { 70, 50, 60, 62 } },
	};

	for (const auto &definition : BuiltInRoster)
	{
		botinfo_t *newinfo = new botinfo_t;
		newinfo->Info = "\\autoaim\\0\\movebob\\.25";
		appendinfo(newinfo->Info, "name");
		appendinfo(newinfo->Info, definition.name);
		appendinfo(newinfo->Info, "team");
		appendinfo(newinfo->Info, "255");
		newinfo->Name = definition.name;
		newinfo->skill = definition.skill;
		newinfo->lastteam = TEAM_NONE;
		newinfo->next = botinfo;
		botinfo = newinfo;
	}
	using_builtin_roster = true;
	Printf("No " BOTFILENAME "; using %d built-in companion identities\n", (int)countof(BuiltInRoster));
	return (int)countof(BuiltInRoster);
}

void FCajunMaster::ForgetBots ()
{
	botinfo_t *thebot = botinfo;

	while (thebot)
	{
		botinfo_t *next = thebot->next;
		delete thebot;
		thebot = next;
	}

	botinfo = NULL;
	using_builtin_roster = false;
}

#if defined _WIN32 || defined __APPLE__

FString M_GetCajunPath(const char* botfilename)
{
	FString path;

	path << progdir << "zcajun/" << botfilename;
	if (!FileExists(path))
	{
		path = "";
	}
	return path;
}

#else

FString M_GetCajunPath(const char* botfilename)
{
	FString path;

	// Check first in $HOME/.config/zdoom/botfilename.
	path = GetUserFile(botfilename);
	if (!FileExists(path))
	{
		// Then check in SHARE_DIR/botfilename.
		path = SHARE_DIR;
		path << botfilename;
		if (!FileExists(path))
		{
			path = "";
		}
	}
	return path;
}

#endif

bool FCajunMaster::LoadBots ()
{
	FScanner sc;
	FString tmp;
	bool gotteam = false;
	int loaded_bots = 0;

	roster_load_attempted = true;
	ForgetBots ();
	tmp = M_GetCajunPath(BOTFILENAME);
	if (tmp.IsEmpty())
	{
		return LoadBuiltInBots() > 0;
	}
	if (!sc.OpenFile(tmp.GetChars()))
	{
		Printf("Unable to open %s; using the built-in companion roster instead\n", tmp.GetChars());
		return LoadBuiltInBots() > 0;
	}

	while (sc.GetString ())
	{
		if (!sc.Compare ("{"))
		{
			sc.ScriptError ("Unexpected token '%s'\n", sc.String);
		}

		botinfo_t *newinfo = new botinfo_t;
		bool gotclass = false;

		newinfo->Info = "\\autoaim\\0\\movebob\\.25";

		for (;;)
		{
			sc.MustGetString ();
			if (sc.Compare ("}"))
				break;

			switch (sc.MatchString (BotConfigStrings))
			{
			case BOTCFG_NAME:
				sc.MustGetString ();
				appendinfo (newinfo->Info, "name");
				appendinfo (newinfo->Info, sc.String);
				newinfo->Name = sc.String;
				break;

			case BOTCFG_AIMING:
				sc.MustGetNumber ();
				newinfo->skill.aiming = sc.Number;
				break;

			case BOTCFG_PERFECTION:
				sc.MustGetNumber ();
				newinfo->skill.perfection = sc.Number;
				break;

			case BOTCFG_REACTION:
				sc.MustGetNumber ();
				newinfo->skill.reaction = sc.Number;
				break;

			case BOTCFG_ISP:
				sc.MustGetNumber ();
				newinfo->skill.isp = sc.Number;
				break;

			case BOTCFG_TEAM:
				{
					char teamstr[16];
					uint8_t teamnum;

					sc.MustGetString ();
					if (IsNum (sc.String))
					{
						teamnum = atoi (sc.String);
						if (!FTeam::IsValid (teamnum))
						{
							teamnum = TEAM_NONE;
						}
					}
					else
					{
						teamnum = TEAM_NONE;
						for (unsigned int i = 0; i < Teams.Size(); ++i)
						{
							if (stricmp (Teams[i].GetName (), sc.String) == 0)
							{
								teamnum = i;
								break;
							}
						}
					}
					appendinfo (newinfo->Info, "team");
					mysnprintf (teamstr, countof(teamstr), "%d", teamnum);
					appendinfo (newinfo->Info, teamstr);
					gotteam = true;
					break;
				}

			default:
				if (stricmp (sc.String, "playerclass") == 0)
				{
					gotclass = true;
				}
				appendinfo (newinfo->Info, sc.String);
				sc.MustGetString ();
				appendinfo (newinfo->Info, sc.String);
				break;
			}
		}
		if (!gotclass)
		{ // Bots that don't specify a class get a random one
			appendinfo (newinfo->Info, "playerclass");
			appendinfo (newinfo->Info, "random");
		}
		if (!gotteam)
		{ // Same for bot teams
			appendinfo (newinfo->Info, "team");
			appendinfo (newinfo->Info, "255");
		}
		newinfo->next = botinfo;
		newinfo->lastteam = TEAM_NONE;
		botinfo = newinfo;
		loaded_bots++;
	}
	if (loaded_bots == 0)
	{
		Printf("No identities were read from %s; using the built-in companion roster instead\n", BOTFILENAME);
		return LoadBuiltInBots() > 0;
	}
	Printf ("%d bots read from %s\n", loaded_bots, BOTFILENAME);
	return true;
}

bool FCajunMaster::EnsureRosterLoaded()
{
	return botinfo != nullptr || LoadBots();
}

ADD_STAT (bots)
{
	FString out;
	out.Format ("think = %04.1f ms  support = %04.1f ms  wtg = %d",
		BotThinkCycles.TimeMS(), BotSupportCycles.TimeMS(),
		BotWTG);
	return out;
}
