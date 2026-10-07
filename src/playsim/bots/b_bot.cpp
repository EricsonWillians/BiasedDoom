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
// Cajun bot
//
// [RH] Moved console commands out of d_netcmd.c (in Cajun source), because
// they don't really belong there.

#include "c_cvars.h"
#include "c_dispatch.h"
#include "b_bot.h"
#include "g_game.h"
#include "p_local.h"
#include "p_enemy.h"
#include "cmdlib.h"
#include "teaminfo.h"
#include "d_net.h"
#include "d_netinf.h"
#include "serializer_doom.h"
#include "serialize_obj.h"
#include "d_player.h"
#include "filesystem.h"
#include "vm.h"
#include "g_levellocals.h"
#include "d_main.h"
#include "menu.h"
#include "m_random.h"
#include "r_data/sprites.h"

IMPLEMENT_CLASS(DBot, false, true)

IMPLEMENT_POINTERS_START(DBot)
	IMPLEMENT_POINTER(dest)
	IMPLEMENT_POINTER(prev)
	IMPLEMENT_POINTER(enemy)
	IMPLEMENT_POINTER(missile)
	IMPLEMENT_POINTER(mate)
	IMPLEMENT_POINTER(last_mate)
	IMPLEMENT_POINTER(route_target)
IMPLEMENT_POINTERS_END

DEFINE_FIELD(DBot, dest)

void DBot::Construct()
{
	CompanionProfile = FCajunMaster::NoCompanionProfile;
	CompanionJoinOrder = 0;
	Clear ();
}

void DBot::Clear ()
{
	player = nullptr;
	Angle = nullAngle;
	dest = nullptr;
	prev = nullptr;
	enemy = nullptr;
	missile = nullptr;
	mate = nullptr;
	last_mate = nullptr;
	route_target = nullptr;
	route_waypoint = { 0, 0 };
	route_waypoint_floor = 0.0;
	route_waypoint_sector = -1;
	route_target_sector = -1;
	route_waypoint_valid = false;
	route_waypoint_allows_planned_descent = false;
	route_waypoint_uses_planned_descent = false;
	pending_navigation_use_line = nullptr;
	pending_navigation_use_side = -1;
	planned_descent_goal = { 0, 0 };
	planned_descent_floor = 0.0;
	planned_descent_active = false;
	memset(&skill, 0, sizeof(skill));
	t_active = 0;
	t_respawn = 0;
	t_strafe = 0;
	t_react = 0;
	t_fight = 0;
	t_roam = 0;
	t_rocket = 0;
	t_route = 0;
	t_stuck = 0;
	t_blocked_direction = 0;
	blocked_move_direction = -1;
	t_follow = 0;
	pendingRecoveryMoveDirection = -1;
	first_shot = true;
	sleft = false;
	allround = false;
	increase = false;
	movement_requested = false;
	old = { 0, 0 };
}

void DBot::ResetNavigationAfterTravel()
{
	// A travelling player and its mate keep their object pointers, while the
	// destination map commonly reuses sector numbers. Route cache validity is
	// therefore necessarily map-local: retaining it can send the first bot tic
	// on the new map toward an old-map waypoint. Do not call Clear() here; the
	// player, skill, and companion identity deliberately survive level travel.
	Angle = player != nullptr && player->mo != nullptr ? player->mo->Angles.Yaw : nullAngle;
	dest = nullptr;
	prev = nullptr;
	enemy = nullptr;
	missile = nullptr;
	mate = nullptr;
	last_mate = nullptr;
	route_target = nullptr;
	route_waypoint = { 0, 0 };
	route_waypoint_floor = 0.0;
	route_waypoint_sector = -1;
	route_target_sector = -1;
	route_waypoint_valid = false;
	route_waypoint_allows_planned_descent = false;
	route_waypoint_uses_planned_descent = false;
	pending_navigation_use_line = nullptr;
	pending_navigation_use_side = -1;
	planned_descent_goal = { 0, 0 };
	planned_descent_floor = 0.0;
	planned_descent_active = false;
	t_active = 0;
	t_strafe = 0;
	t_react = 0;
	t_fight = 0;
	t_roam = 0;
	t_rocket = 0;
	t_route = 0;
	t_stuck = 0;
	t_blocked_direction = 0;
	blocked_move_direction = -1;
	t_follow = 0;
	pendingRecoveryMoveDirection = -1;
	movement_requested = false;
	first_shot = true;

	if (player != nullptr && player->mo != nullptr)
	{
		player->mo->movedir = DI_NODIR;
		player->mo->movecount = -1;
		old = player->mo->Pos().XY();
	}
	else
	{
		old = { 0, 0 };
	}
}

FSerializer &Serialize(FSerializer &arc, const char *key, botskill_t &skill, botskill_t *def)
{
	if (arc.BeginObject(key))
	{
		arc("aiming", skill.aiming)
			("perfection", skill.perfection)
			("reaction", skill.reaction)
			("isp", skill.isp)
			.EndObject();
	}
	return arc;
}

void DBot::Serialize(FSerializer &arc)
{
	Super::Serialize (arc);

	arc("player", player)
		("angle", Angle)
		("dest", dest)
		("prev", prev)
		("enemy", enemy)
		("missile", missile)
		("mate", mate)
		("lastmate", last_mate)
		("routetarget", route_target)
		("routewaypoint", route_waypoint)
		("routewaypointfloor", route_waypoint_floor)
		("routewaypointsector", route_waypoint_sector)
		("routetargetsector", route_target_sector)
		("routewaypointvalid", route_waypoint_valid)
		("routewaypointallowsplanneddescent", route_waypoint_allows_planned_descent)
		("routewaypointusesplanneddescent", route_waypoint_uses_planned_descent)
		("skill", skill)
		("active", t_active)
		("respawn", t_respawn)
		("strafe", t_strafe)
		("react", t_react)
		("fight", t_fight)
		("roam", t_roam)
		("rocket", t_rocket)
		("route", t_route)
		("stuck", t_stuck)
		("blockeddirectionticks", t_blocked_direction)
		("blockeddirection", blocked_move_direction)
		("follow", t_follow)
		("firstshot", first_shot)
		("sleft", sleft)
		("allround", allround)
		("increase", increase)
		("movementrequested", movement_requested)
		("companionprofile", CompanionProfile)
		("companionjoinorder", CompanionJoinOrder)
		("old", old);
}

void DBot::Tick ()
{
	Super::Tick ();

	if (player->mo == nullptr || Level->isFrozen())
	{
		return;
	}

	BotThinkCycles.Clock();
	Level->BotInfo.m_Thinking = true;
	Think ();
	Level->BotInfo.m_Thinking = false;
	BotThinkCycles.Unclock();
}

CVAR (Int, bot_next_color, 11, 0)

namespace
{
	constexpr int CompanionStyleCount = 11;
	constexpr int CompanionSkillCount = 3;
	constexpr int CompanionProfileMask = (1 << FCajunMaster::MaxCompanions) - 1;
	bool SynchronizingCompanionMembership = false;

	void SynchronizeCompanionCountFromMask(int mask);
}

// A companion profile describes one position in the ordered co-op squad. The
// values are server info so every peer sees the same requested roster and
// appearance, but are also archived so a local player can configure a squad
// before opening either a normal or procedural map.
CVAR(String, bot_companion_name1, "", CVAR_ARCHIVE | CVAR_SERVERINFO)
CVAR(String, bot_companion_name2, "", CVAR_ARCHIVE | CVAR_SERVERINFO)
CVAR(String, bot_companion_name3, "", CVAR_ARCHIVE | CVAR_SERVERINFO)
CVAR(String, bot_companion_name4, "", CVAR_ARCHIVE | CVAR_SERVERINFO)
CVAR(String, bot_companion_name5, "", CVAR_ARCHIVE | CVAR_SERVERINFO)
CVAR(String, bot_companion_name6, "", CVAR_ARCHIVE | CVAR_SERVERINFO)
CVAR(String, bot_companion_name7, "", CVAR_ARCHIVE | CVAR_SERVERINFO)

CVAR(String, bot_companion_skin1, "", CVAR_ARCHIVE | CVAR_SERVERINFO)
CVAR(String, bot_companion_skin2, "", CVAR_ARCHIVE | CVAR_SERVERINFO)
CVAR(String, bot_companion_skin3, "", CVAR_ARCHIVE | CVAR_SERVERINFO)
CVAR(String, bot_companion_skin4, "", CVAR_ARCHIVE | CVAR_SERVERINFO)
CVAR(String, bot_companion_skin5, "", CVAR_ARCHIVE | CVAR_SERVERINFO)
CVAR(String, bot_companion_skin6, "", CVAR_ARCHIVE | CVAR_SERVERINFO)
CVAR(String, bot_companion_skin7, "", CVAR_ARCHIVE | CVAR_SERVERINFO)

// 0 preserves the roster's own colour. Values 1–11 are the stable palette
// exposed by the companion profile menus.
CVAR(Int, bot_companion_style1, 0, CVAR_ARCHIVE | CVAR_SERVERINFO)
CVAR(Int, bot_companion_style2, 0, CVAR_ARCHIVE | CVAR_SERVERINFO)
CVAR(Int, bot_companion_style3, 0, CVAR_ARCHIVE | CVAR_SERVERINFO)
CVAR(Int, bot_companion_style4, 0, CVAR_ARCHIVE | CVAR_SERVERINFO)
CVAR(Int, bot_companion_style5, 0, CVAR_ARCHIVE | CVAR_SERVERINFO)
CVAR(Int, bot_companion_style6, 0, CVAR_ARCHIVE | CVAR_SERVERINFO)
CVAR(Int, bot_companion_style7, 0, CVAR_ARCHIVE | CVAR_SERVERINFO)

// A small per-companion skill choice adds useful variety without claiming to
// implement tactical orders that the classic Cajun AI does not actually have.
// 0 leaves an external bots.cfg personality untouched.
CVAR(Int, bot_companion_skill1, 0, CVAR_ARCHIVE | CVAR_SERVERINFO)
CVAR(Int, bot_companion_skill2, 0, CVAR_ARCHIVE | CVAR_SERVERINFO)
CVAR(Int, bot_companion_skill3, 0, CVAR_ARCHIVE | CVAR_SERVERINFO)
CVAR(Int, bot_companion_skill4, 0, CVAR_ARCHIVE | CVAR_SERVERINFO)
CVAR(Int, bot_companion_skill5, 0, CVAR_ARCHIVE | CVAR_SERVERINFO)
CVAR(Int, bot_companion_skill6, 0, CVAR_ARCHIVE | CVAR_SERVERINFO)
CVAR(Int, bot_companion_skill7, 0, CVAR_ARCHIVE | CVAR_SERVERINFO)

// A bit represents membership of one stable profile. Unlike a count, the
// mask can express “remove Companion 2 while Companion 1 and 3 stay”, which
// is the foundation for safe individual removal and re-adding. Keep the
// legacy count an archived local projection: the mask is the one replicated
// source of truth, avoiding a second server record that could race or flatten
// a deliberately sparse squad.
CUSTOM_CVAR(Int, bot_companion_enabled_mask, 0, CVAR_ARCHIVE | CVAR_SERVERINFO | CVAR_NOINITCALL)
{
	const int clamped = (int)self & CompanionProfileMask;
	if (self != clamped)
	{
		self = clamped;
		return;
	}
	if (!SynchronizingCompanionMembership)
	{
		SynchronizeCompanionCountFromMask(clamped);
	}
}

CVAR(Bool, bot_companion_respawn, true, CVAR_ARCHIVE | CVAR_SERVERINFO)
EXTERN_CVAR(Int, bot_companion_count)

namespace
{
	const char *const CompanionNameCVarNames[FCajunMaster::MaxCompanions] =
	{
		"bot_companion_name1", "bot_companion_name2", "bot_companion_name3", "bot_companion_name4",
		"bot_companion_name5", "bot_companion_name6", "bot_companion_name7",
	};
	const char *const CompanionSkinCVarNames[FCajunMaster::MaxCompanions] =
	{
		"bot_companion_skin1", "bot_companion_skin2", "bot_companion_skin3", "bot_companion_skin4",
		"bot_companion_skin5", "bot_companion_skin6", "bot_companion_skin7",
	};
	const char *const CompanionStyleCVarNames[FCajunMaster::MaxCompanions] =
	{
		"bot_companion_style1", "bot_companion_style2", "bot_companion_style3", "bot_companion_style4",
		"bot_companion_style5", "bot_companion_style6", "bot_companion_style7",
	};
	const char *const CompanionSkillCVarNames[FCajunMaster::MaxCompanions] =
	{
		"bot_companion_skill1", "bot_companion_skill2", "bot_companion_skill3", "bot_companion_skill4",
		"bot_companion_skill5", "bot_companion_skill6", "bot_companion_skill7",
	};

	void SynchronizeCompanionCountFromMask(int mask)
	{
		mask &= CompanionProfileMask;
		int count = 0;
		for (int bits = mask; bits != 0; bits >>= 1)
			count += bits & 1;

		SynchronizingCompanionMembership = true;
		if ((int)bot_companion_count != count)
			bot_companion_count = count;
		SynchronizingCompanionMembership = false;
	}

	const char *ProfileName(int profile)
	{
		switch (profile)
		{
		case 0: return bot_companion_name1;
		case 1: return bot_companion_name2;
		case 2: return bot_companion_name3;
		case 3: return bot_companion_name4;
		case 4: return bot_companion_name5;
		case 5: return bot_companion_name6;
		case 6: return bot_companion_name7;
		default: return "";
		}
	}

	const char *ProfileSkin(int profile)
	{
		switch (profile)
		{
		case 0: return bot_companion_skin1;
		case 1: return bot_companion_skin2;
		case 2: return bot_companion_skin3;
		case 3: return bot_companion_skin4;
		case 4: return bot_companion_skin5;
		case 5: return bot_companion_skin6;
		case 6: return bot_companion_skin7;
		default: return "";
		}
	}

	int ProfileStyle(int profile)
	{
		switch (profile)
		{
		case 0: return clamp((int)bot_companion_style1, 0, CompanionStyleCount);
		case 1: return clamp((int)bot_companion_style2, 0, CompanionStyleCount);
		case 2: return clamp((int)bot_companion_style3, 0, CompanionStyleCount);
		case 3: return clamp((int)bot_companion_style4, 0, CompanionStyleCount);
		case 4: return clamp((int)bot_companion_style5, 0, CompanionStyleCount);
		case 5: return clamp((int)bot_companion_style6, 0, CompanionStyleCount);
		case 6: return clamp((int)bot_companion_style7, 0, CompanionStyleCount);
		default: return 0;
		}
	}

	int ProfileSkill(int profile)
	{
		switch (profile)
		{
		case 0: return clamp((int)bot_companion_skill1, 0, CompanionSkillCount);
		case 1: return clamp((int)bot_companion_skill2, 0, CompanionSkillCount);
		case 2: return clamp((int)bot_companion_skill3, 0, CompanionSkillCount);
		case 3: return clamp((int)bot_companion_skill4, 0, CompanionSkillCount);
		case 4: return clamp((int)bot_companion_skill5, 0, CompanionSkillCount);
		case 5: return clamp((int)bot_companion_skill6, 0, CompanionSkillCount);
		case 6: return clamp((int)bot_companion_skill7, 0, CompanionSkillCount);
		default: return 0;
		}
	}

	void SetProfileName(int profile, const char *value)
	{
		switch (profile)
		{
		case 0: bot_companion_name1 = value; break;
		case 1: bot_companion_name2 = value; break;
		case 2: bot_companion_name3 = value; break;
		case 3: bot_companion_name4 = value; break;
		case 4: bot_companion_name5 = value; break;
		case 5: bot_companion_name6 = value; break;
		case 6: bot_companion_name7 = value; break;
		}
	}

	void SetProfileSkin(int profile, const char *value)
	{
		switch (profile)
		{
		case 0: bot_companion_skin1 = value; break;
		case 1: bot_companion_skin2 = value; break;
		case 2: bot_companion_skin3 = value; break;
		case 3: bot_companion_skin4 = value; break;
		case 4: bot_companion_skin5 = value; break;
		case 5: bot_companion_skin6 = value; break;
		case 6: bot_companion_skin7 = value; break;
		}
	}

	void SetProfileStyle(int profile, int value)
	{
		value = clamp(value, 0, CompanionStyleCount);
		switch (profile)
		{
		case 0: bot_companion_style1 = value; break;
		case 1: bot_companion_style2 = value; break;
		case 2: bot_companion_style3 = value; break;
		case 3: bot_companion_style4 = value; break;
		case 4: bot_companion_style5 = value; break;
		case 5: bot_companion_style6 = value; break;
		case 6: bot_companion_style7 = value; break;
		}
	}

	void SetProfileSkill(int profile, int value)
	{
		value = clamp(value, 0, CompanionSkillCount);
		switch (profile)
		{
		case 0: bot_companion_skill1 = value; break;
		case 1: bot_companion_skill2 = value; break;
		case 2: bot_companion_skill3 = value; break;
		case 3: bot_companion_skill4 = value; break;
		case 4: bot_companion_skill5 = value; break;
		case 5: bot_companion_skill6 = value; break;
		case 6: bot_companion_skill7 = value; break;
		}
	}

	bool AppendProfileClearChanges(FCompanionServerInfoChange *changes, unsigned &count, int profile)
	{
		if (profile < 0 || profile >= FCajunMaster::MaxCompanions)
		{
			return false;
		}
		FBaseCVar *name = FindCVar(CompanionNameCVarNames[profile], nullptr);
		FBaseCVar *skin = FindCVar(CompanionSkinCVarNames[profile], nullptr);
		FBaseCVar *style = FindCVar(CompanionStyleCVarNames[profile], nullptr);
		FBaseCVar *skill = FindCVar(CompanionSkillCVarNames[profile], nullptr);
		if (name == nullptr || skin == nullptr || style == nullptr || skill == nullptr)
		{
			return false;
		}
		changes[count++] = { name, UCVarValue(""), CVAR_String };
		changes[count++] = { skin, UCVarValue(""), CVAR_String };
		changes[count++] = { style, UCVarValue(0), CVAR_Int };
		changes[count++] = { skill, UCVarValue(0), CVAR_Int };
		return true;
	}

	// Keep a random companion profile as one transaction. A random profile is
	// still ordinary saved/replicated state, not a local roll that a peer or a
	// later map has to reproduce. That also makes a retry after a busy network
	// tic leave the previous profile untouched.
	bool AppendProfileConfigurationChanges(FCompanionServerInfoChange *changes, unsigned &count,
		int profile, const char *name, const char *skin, int style, int skill)
	{
		if (profile < 0 || profile >= FCajunMaster::MaxCompanions || name == nullptr || skin == nullptr)
		{
			return false;
		}
		FBaseCVar *nameCVar = FindCVar(CompanionNameCVarNames[profile], nullptr);
		FBaseCVar *skinCVar = FindCVar(CompanionSkinCVarNames[profile], nullptr);
		FBaseCVar *styleCVar = FindCVar(CompanionStyleCVarNames[profile], nullptr);
		FBaseCVar *skillCVar = FindCVar(CompanionSkillCVarNames[profile], nullptr);
		if (nameCVar == nullptr || skinCVar == nullptr || styleCVar == nullptr || skillCVar == nullptr)
		{
			return false;
		}
		changes[count++] = { nameCVar, UCVarValue(name), CVAR_String };
		changes[count++] = { skinCVar, UCVarValue(skin), CVAR_String };
		changes[count++] = { styleCVar, UCVarValue(clamp(style, 0, CompanionStyleCount)), CVAR_Int };
		changes[count++] = { skillCVar, UCVarValue(clamp(skill, 0, CompanionSkillCount)), CVAR_Int };
		return true;
	}

	bool AppendMembershipChange(FCompanionServerInfoChange *changes, unsigned &count, int mask)
	{
		FBaseCVar *membership = FindCVar("bot_companion_enabled_mask", nullptr);
		if (membership == nullptr)
		{
			return false;
		}
		changes[count++] = { membership, UCVarValue(mask & CompanionProfileMask), CVAR_Int };
		return true;
	}

	bool ClearProfile(int profile)
	{
		FCompanionServerInfoChange changes[4];
		unsigned count = 0;
		return AppendProfileClearChanges(changes, count, profile) &&
			D_ApplyCompanionServerInfoChangesAtomically(changes, count);
	}

	bool ClearProfileAndDisable(int profile)
	{
		FCompanionServerInfoChange changes[5];
		unsigned count = 0;
		const int mask = BotCompanionEnabledProfileMask() & ~(1 << profile);
		return AppendMembershipChange(changes, count, mask) &&
			AppendProfileClearChanges(changes, count, profile) &&
			D_ApplyCompanionServerInfoChangesAtomically(changes, count);
	}

	FCajunMaster *CompanionMaster()
	{
		return primaryLevel == nullptr ? nullptr : &primaryLevel->BotInfo;
	}

	int GetCompanionProfileState(int profile)
	{
		if (!BotCompanionProfileEnabled(profile))
		{
			return 0;
		}

		FCajunMaster *master = CompanionMaster();
		if (master != nullptr)
		{
			if (master->GetCompanionSlotByProfile(profile) >= 0)
			{
				return 2; // Active.
			}
			if (master->IsCompanionProfilePending(profile))
			{
				return 3; // The replicated add was accepted.
			}
			// A profile can be deliberately prepared before a level or while all
			// current co-op places are full. Reserve “waiting” for the latter;
			// otherwise the UI would falsely imply that a normal spawn delay or
			// an event-tic retry had failed.
			if (!deathmatch && gamestate == GS_LEVEL &&
				master->CountCompanions() + master->CountPendingCompanions() >= master->GetCompanionCapacity())
			{
				return 4;
			}
		}
		return 1; // Prepared.
	}

	void SetCompanionMembershipProjection(int mask)
	{
		mask &= CompanionProfileMask;
		int count = 0;
		for (int bits = mask; bits != 0; bits >>= 1)
			count += bits & 1;

		SynchronizingCompanionMembership = true;
		// SERVERINFO values are committed atomically before they are applied
		// locally. Do not advance the archived compatibility count if a crowded
		// event stream rejected the canonical mask record.
		if ((int)bot_companion_enabled_mask != mask)
			bot_companion_enabled_mask = mask;
		if ((int)bot_companion_enabled_mask == mask && (int)bot_companion_count != count)
			bot_companion_count = count;
		SynchronizingCompanionMembership = false;
	}
}

const char *BotCompanionProfileName(int profile)
{
	return ProfileName(profile);
}

bool BotCompanionSetProfileName(int profile, const char *name)
{
	if (profile < 0 || profile >= FCajunMaster::MaxCompanions)
	{
		return false;
	}

	FBaseCVar *cvar = FindCVar(CompanionNameCVarNames[profile], nullptr);
	if (cvar == nullptr)
	{
		return false;
	}
	const char *value = name == nullptr ? "" : name;
	const FCompanionServerInfoChange change = { cvar, UCVarValue(value), CVAR_String };
	return D_ApplyCompanionServerInfoChangesAtomically(&change, 1) && !stricmp(ProfileName(profile), value);
}

const char *BotCompanionProfileSkin(int profile)
{
	return ProfileSkin(profile);
}

int BotCompanionProfileStyle(int profile)
{
	return ProfileStyle(profile);
}

int BotCompanionProfileSkill(int profile)
{
	return ProfileSkill(profile);
}

bool BotCompanionProfileEnabled(int profile)
{
	return profile >= 0 && profile < FCajunMaster::MaxCompanions &&
		((int(bot_companion_enabled_mask) & (1 << profile)) != 0);
}

int BotCompanionEnabledProfileMask()
{
	return int(bot_companion_enabled_mask) & CompanionProfileMask;
}

int BotCompanionEnabledProfileCount()
{
	int mask = BotCompanionEnabledProfileMask();
	int count = 0;
	while (mask != 0)
	{
		count += mask & 1;
		mask >>= 1;
	}
	return count;
}

void BotCompanionNormalizeProfileMask()
{
	// Only the local game or the network host may perform the legacy
	// count-to-mask migration. A client can momentarily receive the two
	// server-info CVars in separate packets; treating that transient
	// mask=0/count>0 state as an old INI would resurrect profiles the host just
	// removed.
	if (netgame && consoleplayer != Net_Arbitrator)
	{
		return;
	}

	int mask = BotCompanionEnabledProfileMask();
	// INI files from before stable profile membership have only the old count.
	// Migrate them once to the first N profiles without disrupting an explicit
	// zero-sized squad.
	if (mask == 0 && bot_companion_count > 0)
	{
		const int count = clamp((int)bot_companion_count, 0, FCajunMaster::MaxCompanions);
		mask = (1 << count) - 1;
	}
	SetCompanionMembershipProjection(mask);
}

bool BotCompanionSetEnabled(int profile, bool enabled)
{
	if (profile < 0 || profile >= FCajunMaster::MaxCompanions)
		return false;

	int mask = BotCompanionEnabledProfileMask();
	if (enabled)
		mask |= 1 << profile;
	else
		mask &= ~(1 << profile);
	SetCompanionMembershipProjection(mask);
	return BotCompanionProfileEnabled(profile) == enabled;
}

void BotCompanionClearProfile(int profile)
{
	if (profile >= 0 && profile < FCajunMaster::MaxCompanions)
		ClearProfile(profile);
}

bool BotCompanionClearAllProfiles()
{
	// Membership is the authoritative desired roster. Reset it and every
	// archived cosmetic field as one event transaction: a crowded network tic
	// must never turn "dismiss and clear" into a correctly dismissed squad that
	// quietly resurrects old identity/appearance choices on the next add.
	FCompanionServerInfoChange changes[1 + FCajunMaster::MaxCompanions * 4];
	unsigned count = 0;
	if (!AppendMembershipChange(changes, count, 0))
	{
		return false;
	}
	for (int profile = 0; profile < FCajunMaster::MaxCompanions; ++profile)
	{
		if (!AppendProfileClearChanges(changes, count, profile))
		{
			return false;
		}
	}
	return D_ApplyCompanionServerInfoChangesAtomically(changes, count);
}

void BotApplyCompanionSkillPreset(int profile, botskill_t &skill)
{
	switch (ProfileSkill(profile))
	{
	case 1: // Gentle helper: forgiving aim and modest aggression.
		skill = { 35, 0, 35, 45 };
		break;
	case 2: // Balanced: close to a typical competent bots.cfg profile.
		skill = { 55, 0, 55, 60 };
		break;
	case 3: // Veteran: accurate and responsive, but still subject to all safety checks.
		skill = { 80, 0, 80, 70 };
		break;
	default:
		break;
	}
}

bool BotCompanionProfileNameReserved(const char *name, int exceptProfile, int profileLimit)
{
	if (name == nullptr || name[0] == '\0')
	{
		return false;
	}

	profileLimit = clamp(profileLimit, 0, FCajunMaster::MaxCompanions);
	for (int profile = 0; profile < profileLimit; ++profile)
	{
		const char *configuredName = ProfileName(profile);
		if (profile != exceptProfile && BotCompanionProfileEnabled(profile) && configuredName[0] != '\0' &&
			!stricmp(configuredName, name))
		{
			return true;
		}
	}
	return false;
}

EXTERN_CVAR(Flag, sv_coopsharekeys)

namespace
{
	// The menu operates on stable profile membership. Keep the old numeric CVar
	// as a compatibility view of that membership, so scripts and old INI files
	// still start with a sensible first N-profile squad.
	int CompanionDraftProfile = -1;

	bool CanManageCompanions()
	{
		// A configured demo is always a solo capture/replay. Its serialized
		// companion target must not make the menu or a CVar restore synthesize
		// new bot events into the demo timeline.
		if (demorecording || demoplayback)
			return false;
		return !netgame || (consoleplayer == Net_Arbitrator &&
			players[consoleplayer].settings_controller);
	}

	bool CanDeployCompanions()
	{
		// Profiles remain editable in every mode, but a current or selected
		// deathmatch is never a deployment target. Returning false before its
		// map loads keeps the draft and roster wording honest as well.
		return !deathmatch;
	}

	void ApplyCompanionTargetNow()
	{
		// Important server CVars are restored before G_DoLoadGame replaces the
		// old level with the save's snapshot. Reconciliation here would spawn
		// companions into that outgoing level and leak lifecycle events into the
		// restore. Init/Main will reconcile the restored map once it is live.
		if (savegamerestore || !CanDeployCompanions() || gamestate != GS_LEVEL || !CanManageCompanions())
			return;

		FCajunMaster& bots = primaryLevel->BotInfo;
		bots.SetDesiredCompanionCount(BotCompanionEnabledProfileCount());
		bots.ReconcileCompanions(primaryLevel);
	}

	// Resolve a profile before it becomes part of the live squad. In
	// particular, the empty identity option is not left as a per-map random
	// roll: we choose one available roster name once and retain it in the
	// profile, so a companion remains recognisable across normal-map and
	// procedural-map transitions.
	bool ResolveCompanionProfileIdentity(FCajunMaster &bots, int profile)
	{
		if (!bots.EnsureRosterLoaded() || profile < 0 || profile >= FCajunMaster::MaxCompanions)
		{
			return false;
		}

		const char *chosen = ProfileName(profile);
		if (chosen != nullptr && chosen[0] != '\0')
		{
			for (int index = 0; index < bots.GetRosterCount(); ++index)
			{
				const char *candidate = bots.GetRosterName(index);
				if (!stricmp(chosen, candidate) && bots.GetRosterState(index) == BOTINUSE_No &&
					!BotCompanionProfileNameReserved(candidate, profile, FCajunMaster::MaxCompanions))
				{
					// Preserve the roster's canonical spelling in a saved profile.
					SetProfileName(profile, candidate);
					return !stricmp(ProfileName(profile), candidate);
				}
			}
			return false;
		}

		for (int index = 0; index < bots.GetRosterCount(); ++index)
		{
			const char *candidate = bots.GetRosterName(index);
			if (bots.GetRosterState(index) == BOTINUSE_No &&
				!BotCompanionProfileNameReserved(candidate, profile, FCajunMaster::MaxCompanions))
			{
				SetProfileName(profile, candidate);
				return !stricmp(ProfileName(profile), candidate);
			}
		}
		return false;
	}

	int FindFreeCompanionProfile(const FCajunMaster &bots)
	{
		for (int profile = 0; profile < FCajunMaster::MaxCompanions; ++profile)
		{
			if (!BotCompanionProfileEnabled(profile) &&
				bots.GetCompanionSlotByProfile(profile) < 0 &&
				!bots.IsCompanionProfilePending(profile))
			{
				return profile;
			}
		}
		return -1;
	}

	bool CanBeginCompanionDraft(FCajunMaster &bots)
	{
		if (!bots.EnsureRosterLoaded())
		{
			return false;
		}

		const int profile = FindFreeCompanionProfile(bots);
		if (profile < 0)
		{
			return false;
		}

		// A draft starts blank, so it needs at least one roster identity that is
		// neither live/pending nor reserved by another saved profile. Checking
		// this before opening the menu avoids a configuration screen whose only
		// possible outcome is an error at deployment.
		for (int index = 0; index < bots.GetRosterCount(); ++index)
		{
			const char *candidate = bots.GetRosterName(index);
			if (bots.GetRosterState(index) == BOTINUSE_No &&
				!BotCompanionProfileNameReserved(candidate, profile, FCajunMaster::MaxCompanions))
			{
				return true;
			}
		}
		return false;
	}

	bool RandomizeCompanionDraft(FCajunMaster &bots, int profile)
	{
		if (profile < 0 || profile >= FCajunMaster::MaxCompanions ||
			BotCompanionProfileEnabled(profile) || bots.GetCompanionSlotByProfile(profile) >= 0 ||
			bots.IsCompanionProfilePending(profile) || !bots.EnsureRosterLoaded())
		{
			return false;
		}

		// A configured name can be stale when a mod changes bots.cfg. Select from
		// the current usable library and never compete with an enabled profile.
		TArray<int> availableIdentities;
		for (int index = 0; index < bots.GetRosterCount(); ++index)
		{
			const char *candidate = bots.GetRosterName(index);
			if (candidate != nullptr && candidate[0] != '\0' &&
				bots.GetRosterState(index) == BOTINUSE_No &&
				!BotCompanionProfileNameReserved(candidate, profile, FCajunMaster::MaxCompanions))
			{
				availableIdentities.Push(index);
			}
		}
		if (availableIdentities.Size() == 0)
		{
			return false;
		}

		const int identityIndex = availableIdentities[M_Random((int)availableIdentities.Size())];
		const char *identity = bots.GetRosterName(identityIndex);
		// Include the class default in the skin draw: it stays valid with mods
		// that expose no compatible skins, while installed skins remain equally
		// eligible. Styles and skills deliberately avoid their "roster default"
		// setting so Random always produces a complete visible/helper profile.
		const unsigned int skinChoices = Skins.Size() + 1;
		const unsigned int skinChoice = (unsigned int)M_Random((int)skinChoices);
		const char *skin = skinChoice == 0 ? "" : Skins[skinChoice - 1].Name.GetChars();
		const int style = 1 + M_Random(CompanionStyleCount);
		const int skill = 1 + M_Random(CompanionSkillCount);

		FCompanionServerInfoChange changes[4];
		unsigned count = 0;
		return AppendProfileConfigurationChanges(changes, count, profile, identity, skin, style, skill) &&
			D_ApplyCompanionServerInfoChangesAtomically(changes, count);
	}

	bool AddConsoleCompanion(FCajunMaster &bots, const char *requestedName)
	{
		const int profile = FindFreeCompanionProfile(bots);
		if (profile < 0)
		{
			Printf(TEXTCOLOR_YELLOW "Every companion profile is already in use. Remove one from the Companion Bots menu first.\n");
			return false;
		}

		if (!ClearProfile(profile))
		{
			Printf(TEXTCOLOR_YELLOW "Companion settings could not be queued; try adding it again.\n");
			return false;
		}
		if (requestedName != nullptr && requestedName[0] != '\0')
		{
			SetProfileName(profile, requestedName);
			if (stricmp(ProfileName(profile), requestedName) != 0)
			{
				Printf(TEXTCOLOR_YELLOW "Companion settings could not be queued; try adding it again.\n");
				return false;
			}
		}
		if (!ResolveCompanionProfileIdentity(bots, profile))
		{
			ClearProfile(profile);
			if (requestedName != nullptr && requestedName[0] != '\0')
				Printf(TEXTCOLOR_RED "Could not add companion %s: that identity is unavailable.\n", requestedName);
			else
				Printf(TEXTCOLOR_RED "Could not add a companion: no available identity remains in the active roster.\n");
			return false;
		}

		if (!BotCompanionSetEnabled(profile, true))
		{
			Printf(TEXTCOLOR_YELLOW "Companion settings could not be queued; try adding it again.\n");
			return false;
		}
		ApplyCompanionTargetNow();
		Printf("Companion %d (%s) added to the squad.\n", profile + 1, ProfileName(profile));
		return true;
	}

	bool RemoveConfiguredCompanion(int profile)
	{
		if (!BotCompanionProfileEnabled(profile))
		{
			return false;
		}
		if (!ClearProfileAndDisable(profile))
		{
			return false;
		}
		if (CompanionDraftProfile == profile)
		{
			CompanionDraftProfile = -1;
		}
		ApplyCompanionTargetNow();
		return true;
	}

	bool DismissConfiguredCompanionSquad()
	{
		// Clearing only the live pawns is not enough: profile membership is the
		// desired roster and Main() will faithfully restore any remaining bits on
		// the next tic or map. This is shared by the explicit squad command and
		// the classic removebots alias in cooperative play.
		if (!BotCompanionClearAllProfiles())
		{
			return false;
		}
		CompanionDraftProfile = -1;
		ApplyCompanionTargetNow();
		return true;
	}
}

CUSTOM_CVAR(Int, bot_companion_count, 0, CVAR_ARCHIVE | CVAR_NOINITCALL)
{
	const int clamped = clamp((int)self, 0, FCajunMaster::MaxCompanions);
	const bool membershipSync = SynchronizingCompanionMembership;
	const bool replicatedUpdate = D_IsApplyingServerInfoChange() || savegamerestore;
	// A normal custom CVar invokes its callback again when it assigns itself.
	// Only write back an out-of-range value, then let that bounded callback
	// perform the actual roster update. An unconditional self-assignment here
	// recurses through the server-info callback until a demo CVar restore
	// overflows the stack.
	if (self != clamped)
	{
		self = clamped;
		return;
	}
	if (!membershipSync && !replicatedUpdate && CanManageCompanions())
	{
		// Console users of the legacy count CVar still get an intuitive
		// sequential squad. The menu never takes this path: it keeps sparse,
		// stable profile membership through BotCompanionSetEnabled(). The mask
		// is atomically queued before its local mutation; if a crowded network
		// tic rejects it, restore this compatibility projection to the actual
		// retained mask instead of advertising a squad that peers never saw.
		const int requestedMask = (1 << clamped) - 1;
		SynchronizingCompanionMembership = true;
		bot_companion_enabled_mask = requestedMask;
		if (BotCompanionEnabledProfileMask() != requestedMask)
			self = BotCompanionEnabledProfileCount();
		SynchronizingCompanionMembership = false;
	}
	if (primaryLevel != nullptr && !deathmatch)
	{
		primaryLevel->BotInfo.SetDesiredCompanionCount(BotCompanionEnabledProfileCount());
	}
	if (self > 0 && !replicatedUpdate && CanManageCompanions() &&
		!deathmatch && !demorecording && !demoplayback)
	{
		// Companion sessions use the engine's normal co-op key-sharing rule.
		// It is server info, so a host's decision is visible to every peer.
		sv_coopsharekeys = true;
	}
	// BotCompanionNormalizeProfileMask/BotCompanionSetEnabled already own the
	// reconciliation that follows a synchronized count update. Calling back
	// into it here would recurse through Main -> Normalize -> this CVar hook.
	if (!membershipSync && !replicatedUpdate)
	{
		ApplyCompanionTargetNow();
	}
}

// The menu implementation uses these narrowly-scoped native accessors rather
// than exposing the whole bot roster or arbitrary CVar names to UI scripts.
// They are read-only except for the three profile fields, and those only
// accept changes from the active menu on the host/settings controller.
DEFINE_ACTION_FUNCTION(FCompanionBots, GetRosterCount)
{
	PARAM_PROLOGUE;
	FCajunMaster *master = CompanionMaster();
	if (master != nullptr)
	{
		master->EnsureRosterLoaded();
	}
	ACTION_RETURN_INT(master == nullptr ? 0 : master->GetRosterCount());
}

DEFINE_ACTION_FUNCTION(FCompanionBots, GetRosterName)
{
	PARAM_PROLOGUE;
	PARAM_INT(index);
	FCajunMaster *master = CompanionMaster();
	if (master != nullptr)
	{
		master->EnsureRosterLoaded();
	}
	ACTION_RETURN_STRING(master == nullptr ? "" : master->GetRosterName(index));
}

DEFINE_ACTION_FUNCTION(FCompanionBots, GetRosterState)
{
	PARAM_PROLOGUE;
	PARAM_INT(index);
	FCajunMaster *master = CompanionMaster();
	if (master != nullptr)
	{
		master->EnsureRosterLoaded();
	}
	ACTION_RETURN_INT(master == nullptr ? BOTINUSE_No : master->GetRosterState(index));
}

DEFINE_ACTION_FUNCTION(FCompanionBots, CanManage)
{
	PARAM_PROLOGUE;
	ACTION_RETURN_BOOL(CanManageCompanions());
}

DEFINE_ACTION_FUNCTION(FCompanionBots, GetEnabledCount)
{
	PARAM_PROLOGUE;
	ACTION_RETURN_INT(BotCompanionEnabledProfileCount());
}

DEFINE_ACTION_FUNCTION(FCompanionBots, GetCapacity)
{
	PARAM_PROLOGUE;
	FCajunMaster *master = CompanionMaster();
	if (master != nullptr)
	{
		master->EnsureRosterLoaded();
	}
	ACTION_RETURN_INT(master == nullptr ? FCajunMaster::MaxCompanions : master->GetCompanionCapacity());
}

DEFINE_ACTION_FUNCTION(FCompanionBots, IsProfileEnabled)
{
	PARAM_PROLOGUE;
	PARAM_INT(profile);
	ACTION_RETURN_BOOL(BotCompanionProfileEnabled(profile));
}

DEFINE_ACTION_FUNCTION(FCompanionBots, GetProfileState)
{
	PARAM_PROLOGUE;
	PARAM_INT(profile);
	ACTION_RETURN_INT(GetCompanionProfileState(profile));
}

DEFINE_ACTION_FUNCTION(FCompanionBots, GetProfileStateText)
{
	PARAM_PROLOGUE;
	PARAM_INT(profile);
	const int state = GetCompanionProfileState(profile);
	switch (state)
	{
	case 1: ACTION_RETURN_STRING("Prepared");
	case 2: ACTION_RETURN_STRING("Active");
	case 3: ACTION_RETURN_STRING("Joining");
	// This label appears beside an identity in the narrow roster row. Keep it
	// compact; the squad page already explains why a prepared companion waits.
	case 4: ACTION_RETURN_STRING("Waiting");
	default: ACTION_RETURN_STRING("Not in squad");
	}
}

DEFINE_ACTION_FUNCTION(FCompanionBots, GetProfileLiveName)
{
	PARAM_PROLOGUE;
	PARAM_INT(profile);
	FCajunMaster *master = CompanionMaster();
	ACTION_RETURN_STRING(master == nullptr ? "" : master->GetCompanionNameByProfile(profile));
}

DEFINE_ACTION_FUNCTION(FCompanionBots, CanBeginDraft)
{
	PARAM_PROLOGUE;
	FCajunMaster *master = CompanionMaster();
	// A live deathmatch must never receive companion pawns, but the persistent
	// squad is still useful preparation for the next cooperative map. Keep the
	// authoring gate separate from the in-level deployment gate: otherwise the
	// menu promises that a squad can be prepared for later while refusing to
	// open its zero-to-one draft.
	ACTION_RETURN_BOOL(CanManageCompanions() && master != nullptr &&
		CanBeginCompanionDraft(*master));
}

DEFINE_ACTION_FUNCTION(FCompanionBots, CanDeploy)
{
	PARAM_PROLOGUE;
	ACTION_RETURN_BOOL(CanDeployCompanions());
}

DEFINE_ACTION_FUNCTION(FCompanionBots, BeginDraft)
{
	PARAM_PROLOGUE;
	if (!DMenu::InMenu || !CanManageCompanions())
	{
		ACTION_RETURN_INT(-1);
	}

	FCajunMaster *master = CompanionMaster();
	if (master == nullptr || !master->EnsureRosterLoaded())
	{
		ACTION_RETURN_INT(-1);
	}

	if (CompanionDraftProfile >= 0)
	{
		// A host handoff may destroy the menu after authority was lost. Do not
		// let a later promotion resurrect that abandoned local token over an
		// enabled or otherwise repurposed stable profile.
		if (CompanionDraftProfile < FCajunMaster::MaxCompanions &&
			!BotCompanionProfileEnabled(CompanionDraftProfile) &&
			master->GetCompanionSlotByProfile(CompanionDraftProfile) < 0 &&
			!master->IsCompanionProfilePending(CompanionDraftProfile))
		{
			ACTION_RETURN_INT(CompanionDraftProfile);
		}
		CompanionDraftProfile = -1;
	}

	if (!CanBeginCompanionDraft(*master))
	{
		ACTION_RETURN_INT(-1);
	}
	const int profile = FindFreeCompanionProfile(*master);
	if (!ClearProfile(profile))
	{
		ACTION_RETURN_INT(-1);
	}
	CompanionDraftProfile = profile;
	ACTION_RETURN_INT(profile);
}

DEFINE_ACTION_FUNCTION(FCompanionBots, BeginRandomDraft)
{
	PARAM_PROLOGUE;
	if (!DMenu::InMenu || !CanManageCompanions() || CompanionDraftProfile >= 0)
	{
		ACTION_RETURN_INT(-1);
	}

	FCajunMaster *master = CompanionMaster();
	if (master == nullptr || !CanBeginCompanionDraft(*master))
	{
		ACTION_RETURN_INT(-1);
	}
	const int profile = FindFreeCompanionProfile(*master);
	if (profile < 0 || !RandomizeCompanionDraft(*master, profile))
	{
		ACTION_RETURN_INT(-1);
	}
	CompanionDraftProfile = profile;
	ACTION_RETURN_INT(profile);
}

DEFINE_ACTION_FUNCTION(FCompanionBots, GetDraftProfile)
{
	PARAM_PROLOGUE;
	ACTION_RETURN_INT(CompanionDraftProfile);
}

DEFINE_ACTION_FUNCTION(FCompanionBots, RandomizeDraft)
{
	PARAM_PROLOGUE;
	if (!DMenu::InMenu || !CanManageCompanions() || CompanionDraftProfile < 0)
	{
		ACTION_RETURN_BOOL(false);
	}

	FCajunMaster *master = CompanionMaster();
	ACTION_RETURN_BOOL(master != nullptr &&
		RandomizeCompanionDraft(*master, CompanionDraftProfile));
}

DEFINE_ACTION_FUNCTION(FCompanionBots, DeployDraft)
{
	PARAM_PROLOGUE;
	// "Deploy" here commits the profile to the persistent squad. Reconciliation
	// below remains responsible for deciding whether it may spawn now, so a
	// deathmatch host can save a carefully configured squad for a later co-op
	// map without injecting a bot into the competitive session.
	if (!DMenu::InMenu || !CanManageCompanions() || CompanionDraftProfile < 0)
	{
		ACTION_RETURN_INT(-1);
	}

	FCajunMaster *master = CompanionMaster();
	const int profile = CompanionDraftProfile;
	if (master == nullptr || !master->EnsureRosterLoaded() || profile >= FCajunMaster::MaxCompanions ||
		BotCompanionProfileEnabled(profile) || master->GetCompanionSlotByProfile(profile) >= 0 ||
		master->IsCompanionProfilePending(profile))
	{
		ACTION_RETURN_INT(-1);
	}

	if (!ResolveCompanionProfileIdentity(*master, profile))
	{
		ACTION_RETURN_INT(-1);
	}

	if (!BotCompanionSetEnabled(profile, true))
	{
		ACTION_RETURN_INT(-1);
	}
	CompanionDraftProfile = -1;
	ApplyCompanionTargetNow();
	ACTION_RETURN_INT(profile);
}

DEFINE_ACTION_FUNCTION(FCompanionBots, CancelDraft)
{
	PARAM_PROLOGUE;
	// OnDestroy can run after the menu framework has already cleared its
	// in-menu flag. A draft is local, uncommitted state, so it must still be
	// discarded in that teardown path rather than leaking into a later visit.
	const int profile = CompanionDraftProfile;
	// Always discard the local token. A client that loses host authority must
	// not keep a half-created draft and claim it again after a future handoff.
	CompanionDraftProfile = -1;
	if (CanManageCompanions() && profile >= 0 && profile < FCajunMaster::MaxCompanions &&
		!BotCompanionProfileEnabled(profile))
	{
		ClearProfile(profile);
	}
	return 0;
}

DEFINE_ACTION_FUNCTION(FCompanionBots, RemoveProfile)
{
	PARAM_PROLOGUE;
	PARAM_INT(profile);
	if (!DMenu::InMenu || !CanManageCompanions() || !BotCompanionProfileEnabled(profile))
	{
		ACTION_RETURN_BOOL(false);
	}

	// ReconcileCompanions owns the exact, stable-ID removal event. Clearing
	// membership first means a pending add for this profile is safely ignored.
	ACTION_RETURN_BOOL(RemoveConfiguredCompanion(profile));
}

DEFINE_ACTION_FUNCTION(FCompanionBots, GetSkinCount)
{
	PARAM_PROLOGUE;
	ACTION_RETURN_INT(Skins.Size());
}

DEFINE_ACTION_FUNCTION(FCompanionBots, GetSkinName)
{
	PARAM_PROLOGUE;
	PARAM_INT(index);
	ACTION_RETURN_STRING(index >= 0 && (unsigned)index < Skins.Size() ? Skins[index].Name : "");
}

DEFINE_ACTION_FUNCTION(FCompanionBots, GetProfileName)
{
	PARAM_PROLOGUE;
	PARAM_INT(profile);
	ACTION_RETURN_STRING(ProfileName(profile));
}

DEFINE_ACTION_FUNCTION(FCompanionBots, GetProfileSkin)
{
	PARAM_PROLOGUE;
	PARAM_INT(profile);
	ACTION_RETURN_STRING(ProfileSkin(profile));
}

DEFINE_ACTION_FUNCTION(FCompanionBots, GetProfileStyle)
{
	PARAM_PROLOGUE;
	PARAM_INT(profile);
	ACTION_RETURN_INT(ProfileStyle(profile));
}

DEFINE_ACTION_FUNCTION(FCompanionBots, GetProfileSkill)
{
	PARAM_PROLOGUE;
	PARAM_INT(profile);
	ACTION_RETURN_INT(ProfileSkill(profile));
}

DEFINE_ACTION_FUNCTION(FCompanionBots, GetStyleName)
{
	PARAM_PROLOGUE;
	PARAM_INT(style);
	static const char *StyleNames[] =
	{
		"Roster default", "Green", "Gray", "Indigo", "Deep red", "White",
		"Amber", "Red", "Blue", "Navy", "Yellow", "Bone",
	};
	style = clamp(style, 0, CompanionStyleCount);
	ACTION_RETURN_STRING(StyleNames[style]);
}

DEFINE_ACTION_FUNCTION(FCompanionBots, GetSkillName)
{
	PARAM_PROLOGUE;
	PARAM_INT(skill);
	static const char *SkillNames[] =
	{
		"Roster default", "Gentle helper", "Balanced", "Veteran",
	};
	skill = clamp(skill, 0, CompanionSkillCount);
	ACTION_RETURN_STRING(SkillNames[skill]);
}

DEFINE_ACTION_FUNCTION(FCompanionBots, SetProfileName)
{
	PARAM_PROLOGUE;
	PARAM_INT(profile);
	PARAM_STRING(value);
	if (!DMenu::InMenu || !CanManageCompanions() || profile < 0 || profile >= FCajunMaster::MaxCompanions)
	{
		ACTION_RETURN_BOOL(false);
	}

	if (value.IsEmpty())
	{
		SetProfileName(profile, "");
		ACTION_RETURN_BOOL(ProfileName(profile)[0] == '\0');
	}

	FCajunMaster *master = CompanionMaster();
	if (master != nullptr)
	{
		master->EnsureRosterLoaded();
		for (int i = 0; i < master->GetRosterCount(); ++i)
		{
			const char *candidate = master->GetRosterName(i);
		if (!stricmp(candidate, value.GetChars()))
		{
			const bool unchanged = !stricmp(ProfileName(profile), candidate);
			// An active companion retains its current identity until it next
			// joins. If the player temporarily selected a different future
			// identity, allow them to select that same live name again instead of
			// treating their own bot as somebody else's unavailable roster entry.
			const bool ownLiveIdentity = !stricmp(master->GetCompanionNameByProfile(profile), candidate);
			if (BotCompanionProfileNameReserved(candidate, profile, FCajunMaster::MaxCompanions) ||
				(!unchanged && !ownLiveIdentity && master->GetRosterState(i) != BOTINUSE_No))
				{
					ACTION_RETURN_BOOL(false);
				}
				SetProfileName(profile, candidate);
				ACTION_RETURN_BOOL(!stricmp(ProfileName(profile), candidate));
			}
		}
	}
	ACTION_RETURN_BOOL(false);
}

DEFINE_ACTION_FUNCTION(FCompanionBots, SetProfileSkin)
{
	PARAM_PROLOGUE;
	PARAM_INT(profile);
	PARAM_STRING(value);
	if (!DMenu::InMenu || !CanManageCompanions() || profile < 0 || profile >= FCajunMaster::MaxCompanions)
	{
		ACTION_RETURN_BOOL(false);
	}

	if (value.IsEmpty())
	{
		SetProfileSkin(profile, "");
		ACTION_RETURN_BOOL(ProfileSkin(profile)[0] == '\0');
	}
	for (const FPlayerSkin &skin : Skins)
	{
		if (!skin.Name.CompareNoCase(value))
		{
			SetProfileSkin(profile, skin.Name.GetChars());
			ACTION_RETURN_BOOL(!stricmp(ProfileSkin(profile), skin.Name.GetChars()));
		}
	}
	ACTION_RETURN_BOOL(false);
}

DEFINE_ACTION_FUNCTION(FCompanionBots, SetProfileStyle)
{
	PARAM_PROLOGUE;
	PARAM_INT(profile);
	PARAM_INT(style);
	if (!DMenu::InMenu || !CanManageCompanions() || profile < 0 || profile >= FCajunMaster::MaxCompanions)
	{
		ACTION_RETURN_BOOL(false);
	}
	SetProfileStyle(profile, style);
	ACTION_RETURN_BOOL(ProfileStyle(profile) == clamp(style, 0, CompanionStyleCount));
}

DEFINE_ACTION_FUNCTION(FCompanionBots, SetProfileSkill)
{
	PARAM_PROLOGUE;
	PARAM_INT(profile);
	PARAM_INT(skill);
	if (!DMenu::InMenu || !CanManageCompanions() || profile < 0 || profile >= FCajunMaster::MaxCompanions)
	{
		ACTION_RETURN_BOOL(false);
	}
	SetProfileSkill(profile, skill);
	ACTION_RETURN_BOOL(ProfileSkill(profile) == clamp(skill, 0, CompanionSkillCount));
}

CCMD (addbot)
{
	if (gamestate != GS_LEVEL)
	{
		Printf ("Bots cannot be added when not in a game!\n");
		return;
	}

	if (!players[consoleplayer].settings_controller)
	{
		Printf ("Only setting controllers can add bots\n");
		return;
	}
	if (netgame && consoleplayer != Net_Arbitrator)
	{
		Printf("Only the host can add bots in a network game\n");
		return;
	}

	if (argv.argc() > 2)
	{
		Printf ("addbot [botname] : add a bot to the game (a companion in co-op)\n");
		return;
	}

	FCajunMaster& bots = primaryLevel->BotInfo;
	const char* name = argv.argc() > 1 ? argv[1] : nullptr;
	if (!deathmatch)
	{
		// Demo recording/playback deliberately keeps companion profile changes
		// inert. The explicit companion commands already use this authority gate;
		// preserve the same contract for the familiar co-op addbot alias.
		if (!CanManageCompanions())
		{
			Printf("Companion profiles cannot be changed while recording or playing a demo.\n");
			return;
		}
		// Keep the familiar command useful on ordinary single-player/co-op maps,
		// but route it through a real profile. The resulting bot can then be
		// customized or individually removed in the Companion Bots menu instead
		// of being an anonymous one-off that a later reconciliation discards.
		AddConsoleCompanion(bots, name);
		return;
	}

	// Competitive addbot keeps the classic transient-bot behavior. It does
	// not consume one of the co-op squad's persistent profiles.
	const int activeOrQueued = bots.CountCompanions() + bots.CountPendingCompanions();
	if (activeOrQueued >= bots.GetCompanionCapacity())
	{
		Printf(TEXTCOLOR_YELLOW "No bot slot is currently free.\n");
		return;
	}
	bots.SpawnBot(name, NOCOLOR, FCajunMaster::NoCompanionProfile);
}

CCMD(addcompanion)
{
	if (gamestate != GS_LEVEL)
	{
		Printf("Companions can be queued from the Companion Bots menu before a run, or added during a level.\n");
		return;
	}
	if (!CanDeployCompanions())
	{
		Printf("Companion bots are available in cooperative play, not during a deathmatch level.\n");
		return;
	}
	if (!CanManageCompanions())
	{
		Printf("Only the host can manage companion bots.\n");
		return;
	}
	if (argv.argc() > 2)
	{
		Printf("addcompanion [botname] : add a cooperative bot to the current run\n");
		return;
	}

	AddConsoleCompanion(primaryLevel->BotInfo, argv.argc() == 2 ? argv[1] : nullptr);
}

CCMD(removecompanion)
{
	if (gamestate != GS_LEVEL)
	{
		Printf("Companions can only be removed during a level.\n");
		return;
	}
	if (!CanManageCompanions())
	{
		Printf("Only the host can manage companion bots.\n");
		return;
	}
	if (argv.argc() > 2)
	{
		Printf("removecompanion [1-7] : remove one configured companion (default: highest-numbered squad member)\n");
		return;
	}

	int profile = -1;
	if (argv.argc() == 2)
	{
		const int requested = atoi(argv[1]);
		if (requested < 1 || requested > FCajunMaster::MaxCompanions)
		{
			Printf("Choose a companion profile from 1 to %d.\n", FCajunMaster::MaxCompanions);
			return;
		}
		profile = requested - 1;
	}
	else
	{
		// The menu is the preferred exact-selection UI. The console default is
		// deliberately deterministic as well: it never removes whichever bot
		// happened to join last after a map transition.
		for (int candidate = FCajunMaster::MaxCompanions - 1; candidate >= 0; --candidate)
		{
			if (BotCompanionProfileEnabled(candidate))
			{
				profile = candidate;
				break;
			}
		}
	}

	if (profile < 0)
	{
		Printf("That companion profile is not part of the current squad.\n");
		return;
	}
	if (!RemoveConfiguredCompanion(profile))
	{
		// The profile was selected from the authoritative mask above. A failure
		// here is a full transactional queue rejection, not an ordinal mismatch.
		Printf(TEXTCOLOR_YELLOW "Companion removal could not be queued; try again.\n");
		return;
	}
	Printf("Companion %d removed from the squad.\n", profile + 1);
}

CCMD(dismisscompanions)
{
	if (!CanManageCompanions())
	{
		Printf("Only the host can manage companion bots.\n");
		return;
	}
	// This is intentionally stronger than assigning the legacy count CVar to
	// zero: the action promises to clear the squad, so its saved identities,
	// appearance and skill choices must not resurrect on a later normal map.
	if (DismissConfiguredCompanionSquad())
		Printf("Companion squad dismissed and saved profiles cleared.\n");
	else
		Printf(TEXTCOLOR_YELLOW "Companion squad could not be dismissed yet; try again.\n");
}

void FCajunMaster::ClearPlayer (int i, bool keepTeam)
{
	if (players[i].mo)
	{
		players[i].mo->Destroy ();
		players[i].mo = nullptr;
	}
	botinfo_t *bot = botinfo;
	while (bot && stricmp (players[i].userinfo.GetName(), bot->Name.GetChars()))
		bot = bot->next;
	if (bot)
	{
		bot->inuse = BOTINUSE_No;
		bot->lastteam = keepTeam ? players[i].userinfo.GetTeam() : TEAM_NONE;
	}
	if (players[i].Bot != nullptr)
	{
		players[i].Bot->Destroy ();
		players[i].Bot = nullptr;
	}
	players[i].~player_t();
	::new(&players[i]) player_t;
	players[i].userinfo.Reset(i);
	playeringame[i] = false;
}

CCMD (removebots)
{
	if (!players[consoleplayer].settings_controller)
	{
		Printf ("Only setting controllers can remove bots\n");
		return;
	}
	if (netgame && consoleplayer != Net_Arbitrator)
	{
		Printf("Only the host can remove bots in a network game\n");
		return;
	}
	if (!deathmatch)
	{
		// Like addbot above, this co-op alias owns persistent profile state and
		// must not mutate it while configured demo playback/recording is active.
		if (!CanManageCompanions())
		{
			Printf("Companion profiles cannot be changed while recording or playing a demo.\n");
			return;
		}
		// In co-op, removebots is a long-standing discoverable alias for
		// removing the whole squad. Preserve that expectation while clearing the
		// saved profile membership too; otherwise the host reconciler would
		// immediately add the same companions back.
		if (DismissConfiguredCompanionSquad())
			Printf("Companion squad dismissed and saved profiles cleared.\n");
		else
			Printf(TEXTCOLOR_YELLOW "Companion squad could not be dismissed yet; try again.\n");
		return;
	}

	Net_WriteInt8 (DEM_KILLBOTS);
}

CCMD (freeze)
{
	if (CheckCheatmode ())
		return;

	if (netgame && !players[consoleplayer].settings_controller)
	{
		Printf ("Only setting controllers can use freeze mode\n");
		return;
	}

	Net_WriteInt8(DEM_GENERICCHEAT);
	Net_WriteInt8(CHT_FREEZE);
}

CCMD (listbots)
{
	FCajunMaster& bots = primaryLevel->BotInfo;
	bots.EnsureRosterLoaded();

	FString visibleRoster("AVAILABLE COMPANIONS\n\n");
	const int count = bots.GetRosterCount();
	const int visibleLimit = 12;
	for (int i = 0; i < count; ++i)
	{
		const char* name = bots.GetRosterName(i);
		const int state = bots.GetRosterState(i);
		const char* suffix = state == BOTINUSE_Yes ? " (active)" :
			state == BOTINUSE_Waiting ? " (joining)" : "";
		Printf("%s%s\n", name, suffix);
		if (i < visibleLimit)
		{
			visibleRoster.AppendFormat("%s%s\n", name, suffix);
		}
	}
	Printf("> %d bots\n", count);
	if (count == 0)
	{
		visibleRoster << "No bot identities are available.";
	}
	else if (count > visibleLimit)
	{
		visibleRoster.AppendFormat("… and %d more. Open Companion Roster for the full list.", count - visibleLimit);
	}
	else
	{
		visibleRoster << "Open Companion Roster to assign an identity to a numbered profile.";
	}
	// The former menu action only wrote to the console, which is invisible
	// behind an option menu. Keep the console output for scripts and show a
	// dismissible in-menu roster for players.
	M_StartMessage(visibleRoster.GetChars(), 1);
}

// set the bot specific weapon information
// This is intentionally not in the weapon definition anymore.

BotInfoMap BotInfo;

void InitBotStuff()
{
	int lump;
	int lastlump = 0;
	while (-1 != (lump = fileSystem.FindLump("BOTSUPP", &lastlump)))
	{
		FScanner sc(lump);
		sc.SetCMode(true);
		while (sc.GetString())
		{
			PClassActor *wcls = PClass::FindActor(sc.String);
			if (wcls != nullptr && wcls->IsDescendantOf(NAME_Weapon))
			{
				BotInfoData bi = {};
				sc.MustGetStringName(",");
				sc.MustGetNumber();
				bi.MoveCombatDist = sc.Number;
				while (sc.CheckString(","))
				{
					sc.MustGetString();
					if (sc.Compare("BOT_REACTION_SKILL_THING"))
					{
						bi.flags |= BIF_BOT_REACTION_SKILL_THING;
					}
					else if (sc.Compare("BOT_EXPLOSIVE"))
					{
						bi.flags |= BIF_BOT_EXPLOSIVE;
					}
					else if (sc.Compare("BOT_BFG"))
					{
						bi.flags |= BIF_BOT_BFG;
					}
					else
					{
						PClassActor *cls = PClass::FindActor(sc.String);
						bi.projectileType = cls;
						if (cls == nullptr)
						{
							sc.ScriptError("Unknown token %s", sc.String);
						}
					}
				}
				BotInfo[wcls->TypeName] = bi;
				// A BOTSUPP explosive with an explicit projectile is also a
				// threat other companions should recognize. This makes custom
				// BOTSUPP extensions participate in the same missile-avoidance
				// safety path as the built-in roster.
				if ((bi.flags & BIF_BOT_EXPLOSIVE) && bi.projectileType != nullptr)
				{
					AActor *projectile = GetDefaultByType(bi.projectileType);
					if (projectile != nullptr)
					{
						projectile->flags3 |= MF3_WARNBOT;
					}
				}
			}
			else
			{
				sc.ScriptError("%s is not a weapon type", sc.String);
			}
		}
	}

	// Fixme: Export these, too.
	static const char *warnbotmissiles[] = { "PlasmaBall", "Ripper", "HornRodFX1" };
	for(unsigned i=0;i<countof(warnbotmissiles);i++)
	{
		AActor *a = GetDefaultByName (warnbotmissiles[i]);
		if (a != nullptr)
		{
			a->flags3|=MF3_WARNBOT;
		}
	}
}
