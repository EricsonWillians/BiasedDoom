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
/*******************************
* B_spawn.c                    *
* Description:                 *
* various procedures that the  *
* bot need to work             *
*******************************/

#include <stdlib.h>

#include "doomtype.h"
#include "doomdef.h"
#include "doomstat.h"
#include "p_local.h"
#include "p_maputl.h"
#include "b_bot.h"
#include "g_game.h"
#include "d_event.h"
#include "d_player.h"
#include "p_spec.h"
#include "p_checkposition.h"
#include "p_lnspec.h"
#include "actorinlines.h"

static FRandom pr_botdofire ("BotDoFire");

namespace
{
	// Catch-up is an emergency anti-separation tool, not teleport pathfinding.
	// P_TeleportMove validates only the destination, so a route that crosses any
	// linedef can otherwise skip a midtexture, 3D floor, portal boundary, door,
	// key boundary, or a custom map action. Only an entirely line-free local
	// segment may use the fallback.
	bool IsProtectedRecoveryTransition(const line_t *line)
	{
		return line != nullptr;
	}

	bool CrossesProtectedRecoveryTransition(AActor *actor, const DVector2& destination)
	{
		FPathTraverse path(actor->Level, actor->X(), actor->Y(), destination.X, destination.Y, PT_ADDLINES);
		intercept_t *intercept;
		while ((intercept = path.Next()) != nullptr)
		{
			if (intercept->isaline && IsProtectedRecoveryTransition(intercept->d.line))
			{
				return true;
			}
		}
		return false;
	}

	// Route planning must ask the same 3D-floor-aware support query that the
	// mover will use at the next position. Sector floor planes alone describe
	// neither a walkable 3D platform nor the ceiling below an overhang. This is
	// deliberately a side-effect-free subset of P_CheckPosition: line collision
	// is checked separately by P_LineOpening at the crossed boundary.
	bool ProbeBotSupport(AActor *actor, sector_t *sector, const DVector2 &position,
		double standingZ, FCheckPosition &probe)
	{
		if (actor == nullptr || sector == nullptr)
		{
			return false;
		}

		probe.thing = actor;
		probe.sector = sector;
		probe.pos = DVector3(position, standingZ);
		probe.floorz = probe.dropoffz = NextLowestFloorAt(sector, position.X, position.Y,
			standingZ, 0, actor->MaxStepHeight, &probe.floorsector);
		probe.ceilingz = NextHighestCeilingAt(sector, position.X, position.Y,
			standingZ, standingZ + actor->Height, 0, &probe.ceilingsector);
		return probe.floorsector != nullptr && probe.ceilingsector != nullptr &&
			probe.ceilingz - probe.floorz >= actor->Height;
	}

	bool IsDangerousBotSupport(AActor *actor, const FCheckPosition &probe)
	{
		if (actor == nullptr)
		{
			return true;
		}

		const auto isDangerous = [actor, &probe](sector_t *sector)
		{
			return sector != nullptr && actor->Level->BotInfo.IsDangerous(actor, sector,
				probe.pos.XY(), probe.floorz);
		};
		// A 3D floor's damage is owned by the visual/base sector, while portal
		// support can name another sector. Check both without losing either case.
		return isDangerous(probe.sector) ||
			(probe.floorsector != probe.sector && isDangerous(probe.floorsector));
	}

	bool ProbePointPastLine(AActor *actor, const line_t *line, const sector_t *sector,
		const DVector2 &portal, DVector2 &probe)
	{
		if (actor == nullptr || line == nullptr || sector == nullptr)
		{
			return false;
		}

		const DVector2 delta = line->v2->fPos() - line->v1->fPos();
		const double length = delta.Length();
		if (length <= 1e-6)
		{
			return false;
		}

		// A line's front is to the right of v1 -> v2. Four map units avoids the
		// ambiguous point exactly on the boundary without leaping over a narrow
		// adjoining area.
		DVector2 normal(delta.Y / length, -delta.X / length);
		if (line->frontsector != sector)
		{
			if (line->backsector != sector)
			{
				return false;
			}
			normal *= -1.0;
		}
		probe = portal + normal * 4.0;
		return actor->Level->PointInSector(probe) == sector;
	}

	void GetBotLineOpeningAtSupport(FLineOpening &opening, AActor *actor,
		const line_t *line, const DVector2 &position, double supportZ)
	{
		const double savedZ = actor->Z();
		actor->SetZ(supportZ);
		P_LineOpening(opening, actor, line, position);
		actor->SetZ(savedZ);
	}

	// Cooperative combat must not depend on P_RoughMonsterSearch's first
	// blockmap candidate. That helper can hand us a hidden hostile and stop
	// before it reaches a visible one in the same nearby block. Keep the
	// original expanding block search (rather than scanning every thinker each
	// tic), but make a block yield only an attackable, visible enemy.
	AActor *FindVisibleBotEnemyInBlock(AActor *source, int blockIndex, void *)
	{
		for (FBlockNode *link = source->Level->blockmap.blocklinks[blockIndex]; link != nullptr;
			link = link->NextActor)
		{
			AActor *candidate = link->Me;
			if (candidate == nullptr || candidate->health <= 0 ||
				!(candidate->flags & MF_SHOOTABLE) ||
				!(candidate->flags3 & MF3_ISMONSTER))
			{
				continue;
			}

			// IsOkayToAttack includes the engine's friend, dormant, never-target,
			// and normal line-of-sight rules. That is more reliable than duplicating
			// a subset here, particularly for monsters supplied by a gameplay mod.
			if (source->IsOkayToAttack(candidate))
			{
				return candidate;
			}
		}
		return nullptr;
	}

	constexpr double CompanionHitscanLaneMargin = 24.0;

	bool HasTeammateInHitscanLane(AActor *source, const DVector3 &start,
		const DVector3 &targetCenter, DAngle autoAim)
	{
		const DVector2 lane = targetCenter.XY() - start.XY();
		const double laneLength = lane.Length();
		if (laneLength <= 1.0)
		{
			return true;
		}
		const DVector2 laneDirection = lane / laneLength;
		const double laneSlope = tan(max(autoAim.Radians(), 0.0));

		for (unsigned int i = 0; i < MAXPLAYERS; ++i)
		{
			AActor *friendActor = playeringame[i] ? players[i].mo : nullptr;
			if (friendActor == nullptr || friendActor == source || friendActor->health <= 0 ||
				!source->IsTeammate(friendActor))
			{
				continue;
			}

			const DVector2 toFriend = friendActor->Pos().XY() - start.XY();
			const double along = toFriend | laneDirection;
			if (along <= source->radius || along >= laneLength + friendActor->radius)
			{
				continue;
			}

			const DVector2 nearest = start.XY() + laneDirection * along;
			const double laneRadius = laneSlope * along + friendActor->radius +
				CompanionHitscanLaneMargin;
			if ((friendActor->Pos().XY() - nearest).LengthSquared() <= laneRadius * laneRadius)
			{
				return true;
			}
		}
		return false;
	}

	bool CanUseKnownHitscanAimLane(AActor *source, AActor *target,
		const BotInfoData &weaponInfo, DAngle firingYaw, DAngle firingPitch,
		const DVector3 &start, const DVector3 &targetCenter)
	{
		// Unknown mod weapons have no firing-class metadata. They still work when
		// their exact ray reaches the target, but only known BOTSUPP hitscan
		// weapons may use autoaim's wider lane. This prevents an unclassified mod
		// projectile from borrowing a hitscan exception near a teammate.
		if ((weaponInfo.flags & BIF_BOT_FALLBACK) != 0 ||
			weaponInfo.projectileType != nullptr ||
			(weaponInfo.flags & (BIF_BOT_EXPLOSIVE | BIF_BOT_BFG | BIF_BOT_NO_FIRE)) != 0 ||
			source->player == nullptr || source->player->ReadyWeapon == nullptr ||
			source->player->userinfo.GetAimDist() <= 0 ||
			(source->player->ReadyWeapon->IntVar(NAME_WeaponFlags) & WIF_NOAUTOAIM) != 0)
		{
			return false;
		}

		// P_BulletSlope does not search a continuous cone. It tries the queued
		// yaw, then the two exact horizontal autoaim offsets. Mirror that real
		// candidate set here rather than accepting a target merely because it is
		// somewhere inside a broad angular wedge.
		const DAngle horizontalAutoAim = DAngle::fromDeg(
			clamp(source->player->userinfo.GetAutoaimHorizontal(), 0.0, 8.0));
		if (horizontalAutoAim <= nullAngle ||
			HasTeammateInHitscanLane(source, start, targetCenter, horizontalAutoAim))
		{
			return false;
		}

		// P_AimLineAttack derives the same vertical slope the native autoaim
		// path will use. It is still not enough to trust its chosen target alone:
		// smart aiming can skip a friendly actor, while a real shot cannot safely
		// cross one. Trace the resulting full 3D ray and require the target to be
		// its first shootable collision.
		const DAngle savedPitch = source->Angles.Pitch;
		source->Angles.Pitch = firingPitch;
		const DAngle offsets[] = { nullAngle, horizontalAutoAim, -horizontalAutoAim };
		for (DAngle offset : offsets)
		{
			FTranslatedLineTarget autoAimTarget = {};
			const DAngle autoAimPitch = P_AimLineAttack(source, firingYaw + offset,
				16.0 * 64.0, &autoAimTarget);
			if (autoAimTarget.linetarget != target)
			{
				continue;
			}

			const double pitchCos = autoAimPitch.Cos();
			const DVector3 autoAimDirection(
				pitchCos * (firingYaw + offset).Cos(),
				pitchCos * (firingYaw + offset).Sin(),
				-autoAimPitch.Sin());
			FTraceResults autoAimTrace = {};
			if (Trace(start, source->Sector, autoAimDirection, 16.0 * 64.0,
				MF_SHOOTABLE, ML_BLOCKEVERYTHING | ML_BLOCKHITSCAN, source,
				autoAimTrace, TRACE_NoSky) &&
				autoAimTrace.HitType == TRACE_HitActor && autoAimTrace.Actor == target)
			{
				source->Angles.Pitch = savedPitch;
				return true;
			}
		}
		source->Angles.Pitch = savedPitch;
		return false;
	}
}


//Checks TRUE reachability from bot to a looker.
bool DBot::Reachable(AActor *rtarget, bool allowPlannedDescent,
	double *plannedLandingFloor, bool *usesPlannedDescent)
{
	if (plannedLandingFloor != nullptr)
	{
		*plannedLandingFloor = 0.0;
	}
	if (usesPlannedDescent != nullptr)
	{
		*usesPlannedDescent = false;
	}
	if (rtarget == nullptr || player == nullptr || player->mo == nullptr ||
		player->mo == rtarget || rtarget->Sector == nullptr)
		return false;

	sector_t *last_s = player->mo->Sector;
	if (last_s == nullptr)
		return false;
	FCheckPosition startSupport;
	if (!ProbeBotSupport(player->mo, last_s, player->mo->Pos().XY(), player->mo->Z(), startSupport) ||
		IsDangerousBotSupport(player->mo, startSupport))
	{
		return false;
	}
	double last_z = startSupport.floorz;
	double firstPlannedLandingFloor = 0.0;
	bool usedPlannedDescent = false;

	// This is intentionally a current-geometry direct-path test, not a graph
	// search. Do not add actors here: pickups, corpses, and team mates are not
	// permanent walls and used to make dense ordinary maps look unreachable.
	FPathTraverse it(Level, player->mo->X(), player->mo->Y(), rtarget->X(), rtarget->Y(), PT_ADDLINES);
	intercept_t *in;
	while ((in = it.Next()))
	{
		line_t *line;
		sector_t *s;

		if (in->isaline)
		{
			line = in->d.line;

			if (!(line->flags & ML_TWOSIDED) || (line->flags & (ML_BLOCKING|ML_BLOCKEVERYTHING|ML_BLOCK_PLAYERS)))
			{
				return false; //Cannot continue.
			}
			else
			{
				//Determine if going to use backsector/frontsector.
				s = getNextSector(line, last_s);
				if (s == nullptr)
					return false;
				// A two-sided linedef is not automatically wide enough for this
				// pawn. Keep the direct ray path consistent with the sector-route
				// guide: a narrow portal must make this probe fail so Roam() can
				// choose a real alternate route instead of repeatedly driving into
				// an opening the collision cylinder cannot cross.
				const double portalWidth = (line->v2->fPos() - line->v1->fPos()).Length();
				if (portalWidth < player->mo->radius * 2.0 + 8.0)
					return false;
				const DVector2 hit = it.InterceptPoint(in);
				DVector2 supportPoint;
				if (!ProbePointPastLine(player->mo, line, s, hit, supportPoint))
					return false;
				FCheckPosition nextSupport;
				if (!ProbeBotSupport(player->mo, s, supportPoint, last_z, nextSupport))
					return false;
				FLineOpening opening;
				if (fabs(last_z - player->mo->Z()) <= EQUAL_EPSILON)
				{
					P_LineOpening(opening, player->mo, line, hit);
				}
				else
				{
					GetBotLineOpeningAtSupport(opening, player->mo, line, hit, last_z);
				}
				const double floorheight = nextSupport.floorz;

				// A sector pair alone is not a walkable portal. Honor the exact
				// line opening and the actual support/clearance just beyond it
				// (including 3D floors and mid-textures). A verified leader route may
				// use one of the bounded player-pawn ledges that classic Doom maps
				// expect humans to walk down; arbitrary item and combat probes retain
				// the normal conservative drop limit.
				const bool openingDropIsNormal = opening.lowfloor <= LINEOPEN_MIN ||
					BotCanTraverseDrop(last_z, opening.lowfloor,
						player->mo->MaxDropOffHeight, false);
				const bool supportDropIsNormal = BotCanTraverseDrop(last_z, floorheight,
					player->mo->MaxDropOffHeight, false);
				if (opening.range >= player->mo->Height &&
					opening.top >= last_z + player->mo->Height &&
					opening.bottom <= last_z + player->mo->MaxStepHeight &&
					(opening.lowfloor <= LINEOPEN_MIN ||
						BotCanTraverseDrop(last_z, opening.lowfloor,
							player->mo->MaxDropOffHeight, allowPlannedDescent)) &&
					!Level->BotInfo.IsDangerous(player->mo, s, hit, floorheight) &&
					!IsDangerousBotSupport(player->mo, nextSupport) &&
					floorheight <= (last_z + player->mo->MaxStepHeight) &&
					BotCanTraverseDrop(last_z, floorheight,
						player->mo->MaxDropOffHeight, allowPlannedDescent))
				{
					if ((!openingDropIsNormal || !supportDropIsNormal) && !usedPlannedDescent)
					{
						usedPlannedDescent = true;
						firstPlannedLandingFloor = floorheight;
					}
					last_z = floorheight;
					last_s = s;
					continue;
				}
				else
				{
					return false;
				}
			}
		}

	}

	FCheckPosition targetSupport;
	// Resolve the target from its own standing support, not the floor of the
	// last sector crossed by the bot. In a sector containing a raised 3D-floor
	// bridge or platform those are distinct walkable surfaces; probing at last_z
	// could incorrectly substitute the base floor and claim a direct route that
	// ends below the leader or pickup.
	if (!ProbeBotSupport(player->mo, rtarget->Sector, rtarget->Pos().XY(),
		rtarget->floorz, targetSupport))
	{
		return false;
	}
	const double targetFloor = targetSupport.floorz;
	const bool targetDropIsNormal = BotCanTraverseDrop(last_z, targetFloor,
		player->mo->MaxDropOffHeight, false);
	const bool reachable = !Level->BotInfo.IsDangerous(player->mo, rtarget->Sector, rtarget->Pos().XY(), targetFloor) &&
		!IsDangerousBotSupport(player->mo, targetSupport) &&
		targetFloor <= last_z + player->mo->MaxStepHeight &&
		BotCanTraverseDrop(last_z, targetFloor, player->mo->MaxDropOffHeight,
			allowPlannedDescent);
	if (reachable && !targetDropIsNormal && !usedPlannedDescent)
	{
		usedPlannedDescent = true;
		firstPlannedLandingFloor = targetFloor;
	}
	if (reachable && plannedLandingFloor != nullptr)
	{
		*plannedLandingFloor = usedPlannedDescent ? firstPlannedLandingFloor : targetFloor;
	}
	if (reachable && usesPlannedDescent != nullptr)
	{
		*usesPlannedDescent = usedPlannedDescent;
	}
	return reachable;
}

//doesnt check LOS, checks visibility with a set view angle.
//B_Checksight checks LOS (straight line)
//----------------------------------------------------------------------
//Check if mo1 has free line to mo2
//and if mo2 is within mo1 viewangle (vangle) given with normal degrees.
//if these conditions are true, the function returns true.
//GOOD TO KNOW is that the player's view angle
//in doom is 90 degrees infront.
bool DBot::Check_LOS (AActor *to, DAngle vangle)
{
	if (!P_CheckSight (player->mo, to, SF_SEEPASTBLOCKEVERYTHING))
		return false; // out of sight
	if (vangle >= DAngle::fromDeg(360.))
		return true;
	if (vangle == nullAngle)
		return false; //Looker seems to be blind.

	return absangle(player->mo->AngleTo(to), player->mo->Angles.Yaw) <= (vangle/2);
}

//-------------------------------------
//Bot_Dofire()
//-------------------------------------
//The bot will check if it's time to fire
//and do so if that is the case.
bool DBot::CanFireAt(AActor *target, const BotInfoData &weaponInfo,
	DAngle firingYaw, DAngle firingPitch) const
{
	if (player == nullptr || player->mo == nullptr || target == nullptr ||
		player->mo->Sector == nullptr || target->Sector == nullptr ||
		target->health <= 0 || !(target->flags & MF_SHOOTABLE))
	{
		return false;
	}

	AActor *source = player->mo;
	const bool explosive = (weaponInfo.flags & BIF_BOT_EXPLOSIVE) != 0;
	const bool bfg = (weaponInfo.flags & BIF_BOT_BFG) != 0;
	const DVector3 start = source->PosAtZ(source->Center() - source->Floorclip + source->AttackOffset());
	const DVector3 targetCenter = target->PosAtZ(target->Center());
	const double distance = (targetCenter - start).Length();
	if (distance < 1.0)
	{
		return false;
	}

	// P_CheckSight deliberately ignores actors. More importantly, Dofire runs
	// before TurnToAng has quantized the next usercmd. Trace the yaw/pitch that
	// P_PlayerThink will apply to this attack, not a synthetic ray from the bot
	// to the target centre: while turning, leading, or using an inaccurate
	// weapon, those are different lanes.
	const double pitchCos = firingPitch.Cos();
	const DVector3 firingDirection(
		pitchCos * firingYaw.Cos(),
		pitchCos * firingYaw.Sin(),
		-firingPitch.Sin());

	// Prefer the exact ray actually represented by the queued command. This is
	// required for projectiles, explosives, BFG-class weapons, and unfamiliar
	// mod weapons: none of those can safely borrow a hitscan autoaim exception.
	FTraceResults trace = {};
	const bool exactTargetHit = Trace(start, source->Sector, firingDirection, distance + target->radius,
		MF_SHOOTABLE, ML_BLOCKEVERYTHING | ML_BLOCKHITSCAN, source, trace, TRACE_NoSky) &&
		(trace.HitType == TRACE_HitActor && trace.Actor == target);
	if (!exactTargetHit)
	{
		// Do not turn a blocked ray into a permissive cone. In particular, a
		// teammate, a friendly actor, or a different monster in the physical
		// lane remains an absolute no-fire result. The only relaxed case below
		// is a clear geometry ray for a known hitscan weapon which native player
		// autoaim can safely correct toward the selected hostile.
		if (explosive || bfg || trace.HitType == TRACE_HitActor ||
			!CanUseKnownHitscanAimLane(source, target, weaponInfo, firingYaw,
				firingPitch, start, targetCenter))
		{
			return false;
		}
		return true;
	}

	if (!explosive && !bfg)
	{
		return true;
	}

	const double blastRadius = bfg ? 256.0 : 160.0;
	const DVector2 impactXY = trace.HitPos.XY();
	// FireRox performs a short projectile clearance probe for rockets, but BFG
	// and modded explosive entries arrive here too. Never rely on a weapon's
	// later explosion to discover that the actual impact point is inside the
	// bot's own blast radius.
	if ((source->Pos().XY() - impactXY).LengthSquared() <=
		(blastRadius + source->radius) * (blastRadius + source->radius))
	{
		return false;
	}
	const DVector2 startXY = start.XY();
	const DVector2 shot = impactXY - startXY;
	const double shotLengthSquared = shot.LengthSquared();
	for (unsigned int i = 0; i < MAXPLAYERS; ++i)
	{
		AActor *friendActor = playeringame[i] ? players[i].mo : nullptr;
		if (friendActor == nullptr || friendActor == source || friendActor->health <= 0 ||
			!source->IsTeammate(friendActor))
		{
			continue;
		}

		if ((friendActor->Pos().XY() - impactXY).LengthSquared() <=
			(blastRadius + friendActor->radius) * (blastRadius + friendActor->radius))
		{
			return false;
		}

		const DVector2 toFriend = friendActor->Pos().XY() - startXY;
		const double fraction = clamp((toFriend.X * shot.X + toFriend.Y * shot.Y) /
			max(shotLengthSquared, 1.0), 0.0, 1.0);
		const DVector2 closest = startXY + shot * fraction;
		if ((friendActor->Pos().XY() - closest).LengthSquared() <=
			(friendActor->radius + 24.0) * (friendActor->radius + 24.0))
		{
			return false;
		}
	}

	return true;
}

void DBot::ValidateAttackCommand(usercmd_t *cmd, DAngle nextTickYaw,
	DAngle nextTickPitch)
{
	if (cmd == nullptr || (cmd->buttons & BT_ATTACK) == 0 ||
		player == nullptr || player->mo == nullptr || player->ReadyWeapon == nullptr)
	{
		return;
	}

	const BotInfoData weaponInfo = GetBotInfo(player->ReadyWeapon);
	if (!CanFireAt(enemy, weaponInfo, nextTickYaw, nextTickPitch))
	{
		cmd->buttons &= ~BT_ATTACK;
	}
}

void DBot::Dofire (usercmd_t *cmd)
{
	bool no_fire; //used to prevent bot from pumping rockets into nearby walls.
	int aiming_penalty=0; //For shooting at shading target, if screen is red, MAKEME: When screen red.
	int aiming_value; //The final aiming value.
	double Dist;
	DAngle an;
	DAngle m;
	double fm;

	if (!enemy || !(enemy->flags & MF_SHOOTABLE) || enemy->health <= 0)
		return;

	if (player->ReadyWeapon == NULL)
		return;
	const BotInfoData weaponInfo = GetBotInfo(player->ReadyWeapon);
	// A BOTSUPP entry is required before the bot may fire an unfamiliar
	// explosive or BFG-class weapon. This avoids guessing projectile arcs or
	// blast safety from a mod weapon's display metadata.
	if (weaponInfo.flags & BIF_BOT_NO_FIRE)
		return;

	if (player->damagecount > (unsigned)skill.isp)
	{
		first_shot = true;
		return;
	}

	//Reaction skill thing.
	if (first_shot &&
		!(weaponInfo.flags & BIF_BOT_REACTION_SKILL_THING))
	{
		t_react = (100-skill.reaction+1)/((pr_botdofire()%3)+3);
	}
	first_shot = false;
	if (t_react)
		return;

	//MAKEME: Decrease the rocket suicides even more.

	no_fire = true;
	//Distance to enemy.
	Dist = player->mo->Distance2D(enemy, player->mo->Vel.X - enemy->Vel.X, player->mo->Vel.Y - enemy->Vel.Y);

	//FIRE EACH TYPE OF WEAPON DIFFERENT: Here should all the different weapons go.
	if (weaponInfo.MoveCombatDist == 0)
	{
		//*4 is for atmosphere,  the chainsaws sounding and all..
		no_fire = (Dist > DEFMELEERANGE*4);
	}
	else if (weaponInfo.flags & BIF_BOT_BFG)
	{
		//MAKEME: This should be smarter.
		if ((pr_botdofire()%200)<=skill.reaction)
			if(Check_LOS(enemy, DAngle::fromDeg(SHOOTFOV)))
				no_fire = false;
	}
	else if (weaponInfo.projectileType != NULL)
	{
		if (weaponInfo.flags & BIF_BOT_EXPLOSIVE)
		{
			//Special rules for RL
			an = FireRox (enemy, cmd);
			if(an != nullAngle)
			{
				Angle = an;
				//have to be somewhat precise. to avoid suicide.
				if (absangle(an, player->mo->Angles.Yaw) < DAngle::fromDeg(12.))
				{
					t_rocket = 9;
					no_fire = false;
				}
			}
		}
		else
		{
			// Generic projectile weapons (plasma, imp shots, mod weapons with a
			// BOTSUPP entry) lead their target. Explosives must only fire through
			// FireRox's wall/close-range probe above; falling through to this path
			// used to turn a rejected rocket into an accepted one.
			Dist = player->mo->Distance2D(enemy);
			fm = Dist / max(GetDefaultByType (weaponInfo.projectileType)->Speed, 1.0);
			// Lead by the projectile's actual one-way flight time. The previous
			// double-time estimate routinely aimed past moving targets and around
			// corners that the projectile could not safely clear.
			Level->BotInfo.SetBodyAt(Level, enemy->Pos() + enemy->Vel.XY() * fm, 1);
			Angle = player->mo->AngleTo(Level->BotInfo.body1);
			if (Check_LOS (enemy, DAngle::fromDeg(SHOOTFOV)))
				no_fire = false;
		}
	}
	else
	{
		//Other weapons, mostly instant hit stuff.
		Angle = player->mo->AngleTo(enemy);
		// Friendly companions should acquire a visible hostile deliberately.
		// Native weapon spread still applies, but injecting an additional
		// personality offset made their command ray regularly miss the same
		// target that Doom's player autoaim could legitimately see. Keep that
		// legacy duelling imprecision for deathmatch only.
		if (deathmatch)
		{
			aiming_penalty = 0;
			if (enemy->flags & MF_SHADOW)
				aiming_penalty += (pr_botdofire()%25)+10;
			if (enemy->Sector->lightlevel<WHATS_DARK/* && !(player->powers & PW_INFRARED)*/)
				aiming_penalty += pr_botdofire()%40;//Dark
			if (player->damagecount)
				aiming_penalty += player->damagecount; //Blood in face makes it hard to aim
			aiming_value = skill.aiming - aiming_penalty;
			if (aiming_value <= 0)
				aiming_value = 1;
			m = DAngle::fromDeg(((SHOOTFOV/2)-(aiming_value*SHOOTFOV/200))); //Higher skill is more accurate
			if (m <= nullAngle)
				m = DAngle::fromDeg(1.); //Prevents lock.

			if (m != nullAngle)
			{
				if (increase)
					Angle += m;
				else
					Angle -= m;
			}

			if (absangle(Angle, player->mo->Angles.Yaw) < DAngle::fromDeg(4.))
			{
				increase = !increase;
			}
		}

		if (Check_LOS (enemy, DAngle::fromDeg(SHOOTFOV/2)))
			no_fire = false;
	}
	if (!no_fire)
	{
		// The actual friend/geometry safety test runs after Think has quantized
		// the same usercmd yaw and pitch PlayerPawn will consume next tic.
		cmd->buttons |= BT_ATTACK;
	}
	//Prevents bot from jerking, when firing automatic things with low skill.
}

bool FCajunMaster::IsLeader (player_t *player)
{
	for (unsigned int count = 0; count < MAXPLAYERS; count++)
	{
		if (players[count].Bot != NULL
			&& players[count].Bot->mate == player->mo)
		{
			return true;
		}
	}

	return false;
}

extern int BotWTG;

void FCajunMaster::BotTick(AActor *mo)
{
	BotSupportCycles.Clock();
	m_Thinking = true;
	for (unsigned int i = 0; i < MAXPLAYERS; i++)
	{
		// A companion can be intentionally held out until a later map when a
		// legacy level has no collision-clear spawn. Its DBot thinker remains
		// alive for the retry, but it has no pawn to target, path from, or use
		// for line-of-sight checks in this level.
		if (!playeringame[i] || players[i].Bot == NULL || players[i].mo == NULL)
			continue;

		if (mo->flags3 & MF3_ISMONSTER)
		{
			DBot *bot = players[i].Bot;
			const bool hostile = mo->player == nullptr || !mo->IsTeammate(players[i].mo);
			const bool visible = P_CheckSight(players[i].mo, mo, SF_SEEPASTBLOCKEVERYTHING);
			const double candidateDistance = mo->Distance2D(players[i].mo);
			const bool currentIsInvalid = bot->enemy == nullptr || bot->enemy->health <= 0 ||
				!(bot->enemy->flags & MF_SHOOTABLE);
			const bool candidateIsCloser = !currentIsInvalid &&
				candidateDistance + 32.0 < bot->enemy->Distance2D(players[i].mo);

			// BotTick is called as actors think, so the old unparenthesized ternary
			// could overwrite a valid target based on thinker order. Keep only the
			// nearest visible legal hostile in the bot's normal engagement radius.
			if (mo->health > 0 && (mo->flags & MF_SHOOTABLE) && hostile &&
				candidateDistance < MAX_MONSTER_TARGET_DIST && visible &&
				(currentIsInvalid || candidateIsCloser))
			{
				bot->enemy = mo;
			}
		}
		else if (mo->flags & MF_SPECIAL)
		{
			// Items are reported for the whole level. Restrict this inexpensive
			// callback to nearby, visible pickups; WhatToGet performs the final
			// direct-route check before an item is allowed to displace follow/combat.
			if (mo->Distance2D(players[i].mo) <= 1024.0 &&
				P_CheckSight(players[i].mo, mo, SF_SEEPASTBLOCKEVERYTHING))
			{
				players[i].Bot->WhatToGet(mo);
				BotWTG++;
			}
		}
		else if (mo->flags & MF_MISSILE)
		{
			DBot *bot = players[i].Bot;
			AActor *botPawn = players[i].mo;
			const DVector2 toBot = botPawn->Pos().XY() - mo->Pos().XY();
			const DVector2 velocity = mo->Vel.XY();
			const double speedSquared = velocity.LengthSquared();
			const double closing = toBot.X * velocity.X + toBot.Y * velocity.Y;
			const double eta = speedSquared > 1e-6 ? closing / speedSquared : -1.0;
			const DVector2 closest = eta > 0.0 ? toBot - velocity * eta : toBot;
			const double dangerRadius = botPawn->radius + mo->radius + 48.0;
			const bool incoming = (mo->flags3 & MF3_WARNBOT) && mo->target != botPawn &&
				eta > 0.0 && closest.LengthSquared() <= dangerRadius * dangerRadius;

			// A warned projectile is not automatically dangerous forever. The old
			// first-seen policy made a companion keep avoiding an outbound shot while
			// it ignored a newer, closer inbound one.
			if (bot->missile == mo && !incoming)
			{
				bot->missile = nullptr;
			}
			if (incoming && bot->Check_LOS(mo, DAngle::fromDeg(360.)))
			{
				AActor *current = bot->missile;
				auto threatEta = [botPawn](AActor *missile)
				{
					if (missile == nullptr)
					{
						return DBL_MAX;
					}
					const DVector2 relative = botPawn->Pos().XY() - missile->Pos().XY();
					const DVector2 missileVelocity = missile->Vel.XY();
					const double missileSpeedSquared = missileVelocity.LengthSquared();
					if (missileSpeedSquared <= 1e-6)
					{
						return DBL_MAX;
					}
					return (relative.X * missileVelocity.X + relative.Y * missileVelocity.Y) /
						missileSpeedSquared;
				};
				if (current == nullptr || current->health <= 0 ||
					eta + 0.25 < threatEta(current))
				{
					bot->missile = mo;
				}
			}
		}
	}
	m_Thinking = false;
	BotSupportCycles.Unclock();
}

//This function is called every
//tick (for each bot) to set
//the mate (teammate coop mate).
AActor *DBot::Choose_Mate ()
{
	unsigned int count;
	double closest_dist, test;
	AActor *target;

	//is mate alive?
	if (mate)
	{
		if (mate->health <= 0)
			mate = nullptr;
		else
			last_mate = mate;
	}
	if (mate) //Still is..
		return mate;

	//Check old_mates status.
	if (last_mate)
		if (last_mate->health <= 0)
			last_mate = nullptr;

	target = NULL;
	closest_dist = FLT_MAX;

	//Check for player friends
	for (count = 0; count < MAXPLAYERS; count++)
	{
		player_t *client = &players[count];

		if (playeringame[count]
			&& client->mo
			&& player->mo != client->mo
			&& (player->mo->IsTeammate (client->mo) || !deathmatch)
			&& client->mo->health > 0
			&& ((player->mo->health/2) <= client->mo->health || !deathmatch)
			// In cooperative play every companion should anchor on a real player,
			// not form a fragile bot-following chain that breaks around doors and
			// loops. Competitive team bots retain the legacy one-leader rule.
			&& (!deathmatch ? client->Bot == nullptr : !Level->BotInfo.IsLeader(client)))
		{
			const bool visible = P_CheckSight(player->mo, client->mo, SF_IGNOREVISIBILITY);
			if (visible || !deathmatch)
			{
				test = client->mo->Distance2D(player->mo);
				// Retaining an out-of-sight human leader lets the route guide work
				// through normal rooms and manual doors, while still preferring a
				// visible teammate when there is a choice.
				if (!visible)
				{
					test += 256.0;
				}

				if (test < closest_dist)
				{
					closest_dist = test;
					target = client->mo;
				}
			}
		}
	}

/*
	//Make a introducing to mate.
	if(target && target!=last_mate)
	{
		if((P_Random()%(200*Level->BotInfo.botnum))<3)
		{
			chat = c_teamup;
			if(target->bot)
					strcpy(c_target, botsingame[target->bot_id]);
						else if(target->player)
					strcpy(c_target, player_names[target->play_id]);
		}
	}
*/

	return target;

}

// A generated map deliberately contains turns, loops, keyed doors, and
// vertical links that the 1999-era Cajun straight-line rover cannot solve as
// a general pathfinding graph. Companions still get normal movement time
// first; this is a conservative recovery for a human leader that has become
// genuinely unreachable. Every candidate stays in the leader's current
// sector and P_TeleportMove performs the engine's normal collision check, so
// recovery only rejoins the bot at a place the leader has already reached and
// never overlaps a player or solid prop. It is deliberately a recovery, not
// a general graph pathfinder.
bool DBot::TryCatchUpToMate()
{
	if (mate == nullptr || mate->health <= 0 || mate->Sector == nullptr || player == nullptr || player->mo == nullptr)
	{
		return false;
	}

	static const DVector2 offsets[] =
	{
		DVector2(176, 0), DVector2(0, 176), DVector2(-176, 0), DVector2(0, -176),
		DVector2(176, 176), DVector2(-176, 176), DVector2(-176, -176), DVector2(176, -176),
		DVector2(96, 0), DVector2(0, 96), DVector2(-96, 0), DVector2(0, -96),
	};
	const unsigned int begin = CompanionJoinOrder % countof(offsets);
	const DVector2 leaderPosition = mate->Pos().XY();
	for (unsigned int step = 0; step < countof(offsets); ++step)
	{
		const DVector2 candidate = leaderPosition + offsets[(begin + step) % countof(offsets)];
		if (Level->PointInSector(candidate) != mate->Sector)
		{
			continue;
		}
		if (CrossesProtectedRecoveryTransition(player->mo, candidate))
		{
			continue;
		}
		// P_TeleportMove expects a real Z coordinate; unlike P_Teleport it does
		// not resolve ONFLOORZ. Probe the exact collision volume first so the
		// move lands on the valid local floor (including 3D-floor adjustments)
		// instead of ever sending a companion to the ONFLOORZ sentinel.
		FCheckPosition position;
		if (!Level->BotInfo.SafeCheckPosition(player->mo, candidate.X, candidate.Y, position) ||
			position.ceilingz - position.floorz < player->mo->Height ||
			Level->BotInfo.IsDangerous(player->mo,
				position.floorsector != nullptr ? position.floorsector : position.sector,
				candidate, position.floorz))
		{
			continue;
		}
		if (P_TeleportMove(player->mo, DVector3(candidate, position.floorz), false))
		{
			player->mo->Vel.Zero();
			return true;
		}
	}
	return false;
}

//MAKEME: Make this a smart decision
AActor *DBot::Find_enemy ()
{
	unsigned int count;
	double closest_dist, temp; //To target.
	AActor *target;
	DAngle vangle;

	if (!deathmatch)
	{ // [RH] Take advantage of the Heretic/Hexen code to be a little smarter
		return P_BlockmapSearch(player->mo, 20, FindVisibleBotEnemyInBlock, nullptr);
	}

	//Note: It's hard to ambush a bot who is not alone
	if (allround || mate)
		vangle = DAngle::fromDeg(360.);
	else
		vangle = DAngle::fromDeg(ENEMY_SCAN_FOV);
	allround = false;

	target = NULL;
	closest_dist = FLT_MAX;

	for (count = 0; count < MAXPLAYERS; count++)
	{
		player_t *client = &players[count];
		if (playeringame[count]
			&& !player->mo->IsTeammate (client->mo)
			&& client->mo->health > 0
			&& player->mo != client->mo)
		{
			if (Check_LOS (client->mo, vangle)) //Here's a strange one, when bot is standing still, the P_CheckSight within Check_LOS almost always returns false. tought it should be the same checksight as below but.. (below works) something must be fuckin wierd screded up. 
			//if(P_CheckSight(player->mo, players[count].mo))
			{
				temp = client->mo->Distance2D(player->mo);

				//Too dark?
				if (temp > DARK_DIST &&
					client->mo->Sector->lightlevel < WHATS_DARK /*&&
					player->Powers & PW_INFRARED*/)
					continue;

				if (temp < closest_dist)
				{
					closest_dist = temp;
					target = client->mo;
				}
			}
		}
	}

	return target;
}



//Creates a temporary mobj (invisible) at the given location.
void FCajunMaster::SetBodyAt (FLevelLocals *Level, const DVector3 &pos, int hostnum)
{
	if (hostnum == 1)
	{
		if (body1)
		{
			body1->SetOrigin (pos, false);
		}
		else
		{
			body1 = Spawn (Level, "CajunBodyNode", pos, NO_REPLACE);
		}
	}
	else if (hostnum == 2)
	{
		if (body2)
		{
			body2->SetOrigin (pos, false);
		}
		else
		{
			body2 = Spawn (Level, "CajunBodyNode", pos, NO_REPLACE);
		}
	}
}

//------------------------------------------
//    FireRox()
//
//Returns NULL if shouldn't fire
//else an angle (in degrees) are given
//This function assumes actor->player->angle
//has been set an is the main aiming angle.


//Emulates missile travel. Returns distance travelled.
double FCajunMaster::FakeFire (AActor *source, AActor *dest, usercmd_t *cmd)
{
	AActor *th = Spawn (source->Level, "CajunTrace", source->PosPlusZ(4*8.), NO_REPLACE);
	
	th->target = source;		// where it came from


	th->Vel = source->Vec3To(dest).Resized(th->Speed);

	double dist = 0;

	while (dist < SAFE_SELF_MISDIST)
	{
		dist += th->Speed;
		th->Move(th->Vel);
		if (!CleanAhead (th, th->X(), th->Y(), cmd))
			break;
	}
	th->Destroy ();
	return dist;
}

DAngle DBot::FireRox (AActor *enemy, usercmd_t *cmd)
{
	double dist;
	AActor *actor;
	double m;

	Level->BotInfo.SetBodyAt(Level, player->mo->PosPlusZ(player->mo->Height / 2) + player->mo->Vel.XY() * 5, 2);

	actor = Level->BotInfo.body2;

	dist = actor->Distance2D (enemy);
	if (dist < SAFE_SELF_MISDIST)
		return nullAngle;
	// Predict with the projectile this weapon actually fires. BOTSUPP covers
	// Heretic, Hexen, and Strife explosives too; using Doom's Rocket here made
	// their safety trace lead targets with the wrong speed. An incomplete or
	// modded BOTSUPP entry is unsafe to guess, so decline that explosive shot.
	const BotInfoData weaponInfo = GetBotInfo(player->ReadyWeapon);
	const AActor *projectile = weaponInfo.projectileType == nullptr
		? nullptr : GetDefaultByType(weaponInfo.projectileType);
	if (projectile == nullptr || projectile->Speed <= 0)
		return nullAngle;
	m = ((dist + 1) / projectile->Speed);

	Level->BotInfo.SetBodyAt(Level, DVector3((enemy->Pos().XY() + enemy->Vel * (m + 2)), ONFLOORZ), 1);
	
	//try the predicted location
	if (P_CheckSight (actor, Level->BotInfo.body1, SF_IGNOREVISIBILITY)) //See the predicted location, so give a test missile
	{
		FCheckPosition tm;
		if (Level->BotInfo.SafeCheckPosition (player->mo, actor->X(), actor->Y(), tm))
		{
			if (Level->BotInfo.FakeFire (actor, Level->BotInfo.body1, cmd) >= SAFE_SELF_MISDIST)
			{
				return actor->AngleTo(Level->BotInfo.body1);
			}
		}
	}
	//Try fire straight.
	if (P_CheckSight (actor, enemy, 0))
	{
		if (Level->BotInfo.FakeFire (player->mo, enemy, cmd) >= SAFE_SELF_MISDIST)
		{
			return player->mo->AngleTo(enemy);
		}
	}
	return nullAngle;
}

// [RH] We absolutely do not want to pick things up here. The bot code is
// executed apart from all the other simulation code, so we don't want it
// creating side-effects during gameplay.
bool FCajunMaster::SafeCheckPosition (AActor *actor, double x, double y, FCheckPosition &tm)
{
	ActorFlags savedFlags = actor->flags;
	actor->flags &= ~MF_PICKUP;
	bool res = P_CheckPosition (actor, DVector2(x, y), tm);
	actor->flags = savedFlags;
	return res;
}

void FCajunMaster::StartTravel ()
{
	for (unsigned int i = 0; i < MAXPLAYERS; ++i)
	{
		if (players[i].Bot != NULL)
		{
			players[i].Bot->ChangeStatNum (STAT_TRAVELLING);
		}
	}
}

void FCajunMaster::FinishTravel ()
{
	for (unsigned int i = 0; i < MAXPLAYERS; ++i)
	{
		if (players[i].Bot != NULL)
		{
			players[i].Bot->ChangeStatNum (STAT_BOT);
		}
	}
}
