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
/********************************
* B_Think.c                     *
* Description:                  *
* Functions for the different   *
* states that the bot           *
* uses. These functions are     *
* the main AI                   *
*                               *
*********************************/

#include "doomdef.h"
#include "doomstat.h"
#include "p_local.h"
#include "p_enemy.h"
#include "b_bot.h"
#include "g_game.h"
#include "d_net.h"
#include "d_event.h"
#include "d_player.h"
#include "actorinlines.h"

static FRandom pr_botmove ("BotMove");
// Cooperative bots should feel like companions rather than independent
// deathmatch roamers.  These bands are deliberately wider than a pawn's
// collision radius (and than the 176/224-unit spawn rings), but short enough
// that a monster or pickup cannot pull a companion into the next district.
static constexpr double CompanionSoftLeashDistance = 320.0;
static constexpr double CompanionHardLeashDistance = 512.0;
static constexpr double CompanionLocalPickupDistance = 320.0;
// Keep the emergency teleport slower than the normal route follower. This
// leaves enough time for a real door/stair/loop traversal and prevents a
// routine corner from being mistaken for a recovery case.
static constexpr int CompanionCatchupTics = 2 * TICRATE;
// A pickup is never worth abandoning a nearby human leader or a live fight
// for. Keep the bound at the final selection point because both BotTick and
// the opportunistic inventory iterator feed WhatToGet.
static constexpr double BotPickupMaxDistance = 1024.0;

//This function is called each tic for each bot,
//so this is what the bot does.
void DBot::Think ()
{
	usercmd_t *cmd = &player->cmd;

	// Commands are consumed after the previous Think pass. Track real world
	// progress instead of letting a randomized movecount hold the bot against a
	// crowd, prop, or closed door for seconds at a time. A deliberate interaction
	// wait never sets movement_requested, so lifts and opening doors do not look
	// like collisions.
	if (movement_requested)
	{
		if ((player->mo->Pos().XY() - old).LengthSquared() < 4.0)
		{
			++t_stuck;
		}
		else
		{
			t_stuck = 0;
		}
	}
	else
	{
		t_stuck = 0;
	}
	movement_requested = false;

	memset (cmd, 0, sizeof(*cmd));
	pendingRecoveryMoveDirection = DI_NODIR;
	pending_navigation_use_line = nullptr;
	pending_navigation_use_side = -1;
	planned_descent_active = false;
	planned_descent_goal = { 0, 0 };
	planned_descent_floor = 0.0;

	if (enemy && enemy->health <= 0)
		enemy = nullptr;

	if (player->mo->health > 0) //Still alive
	{
		if (teamplay || !deathmatch)
			mate = Choose_Mate ();

		AActor *actor = player->mo;
		DAngle oldyaw = actor->Angles.Yaw;
		DAngle oldpitch = actor->Angles.Pitch;

		Set_enemy ();
		ThinkForMove (cmd);
		TurnToAng ();

		cmd->yaw = (short)((actor->Angles.Yaw - oldyaw).Degrees() * (65536 / 360.f));
		cmd->pitch = (short)((oldpitch - actor->Angles.Pitch).Degrees() * (65536 / 360.f));
		if (cmd->pitch == -32768)
			cmd->pitch = -32767;
		const DAngle quantizedYaw = DAngle::fromDeg(cmd->yaw * (360 / 65536.f));
		const DAngle quantizedPitch = DAngle::fromDeg(cmd->pitch * (360 / 65536.f));
		actor->Angles.Yaw = oldyaw + quantizedYaw;
		actor->Angles.Pitch = oldpitch - quantizedPitch;
		// P_PlayerThink applies cmd->yaw before consuming forward/side movement
		// next tic. A recovery command and attack safety trace must use those
		// predicted view angles, not the current angles that TurnToAng has only
		// partially advanced. PlayerPawn clamps pitch (or pins it level in a
		// no-freelook map) immediately before weapon code runs, so mirror that
		// final constraint for the firing-lane test.
		const DAngle nextTickYaw = actor->Angles.Yaw + quantizedYaw;
		const DAngle nextTickPitch = actor->Level->IsFreelookAllowed()
			? clamp(actor->Angles.Pitch - quantizedPitch, player->MinPitch, player->MaxPitch)
			: nullAngle;
		ApplyPendingRecoveryMoveCommand(cmd, nextTickYaw);
		ValidateMovementCommand(cmd, nextTickYaw);
		ValidateNavigationUseCommand(cmd, nextTickYaw);
		ValidateAttackCommand(cmd, nextTickYaw, nextTickPitch);
	}

	if (t_active)	t_active--;
	if (t_strafe)	t_strafe--;
	if (t_react)	t_react--;
	if (t_fight)	t_fight--;
	if (t_rocket)	t_rocket--;
	if (t_roam)		t_roam--;
	if (t_route)		t_route--;
	if (t_blocked_direction)	t_blocked_direction--;

	//Respawn ticker
	if (t_respawn)
	{
		t_respawn--;
	}
	else if (player->mo->health <= 0 && (deathmatch || bot_companion_respawn))
	{ // Time to respawn
		// Cooperative companions only send their normal use-to-respawn input
		// when the host has enabled it. Competitive legacy bots retain their
		// established deathmatch behavior regardless of the co-op setting.
		cmd->buttons |= BT_USE;
	}
}

#define THINKDISTSQ (50000.*50000./(65536.*65536.))
//how the bot moves.
//MAIN movement function.
void DBot::ThinkForMove (usercmd_t *cmd)
{
	double dist;
	bool stuck;
	int r;
	// A close pickup or a visible monster must not leave a cooperative bot
	// stranded above a leader it can already prove is reachable through one
	// small, ordinary descent. The general follower branch below deliberately
	// yields to combat and item goals, so establish this narrow navigation
	// priority before selecting a movement mode. Reachable() only reports this
	// flag after it has checked the crossed portal, support floor, clearance,
	// and hazards; ordinary flat follow and every unproven route keep the
	// legacy priority rules.
	double forcedMateFloor = 0.0;
	bool forcedMateUsesPlannedDescent = false;
	const bool forcedMateReachable = mate != nullptr &&
		Reachable(mate, true, &forcedMateFloor, &forcedMateUsesPlannedDescent);
	const bool mustFollowPlannedDescent = forcedMateReachable &&
		forcedMateUsesPlannedDescent;
	const double mateDistance = mate != nullptr ? player->mo->Distance2D(mate) : 0.0;
	// Regrouping is an intent, not just a fallback for an idle bot.  A leader
	// behind a corner, door, stair, or route loop must retain the destination
	// even when a visible monster or an opportunistic pickup would otherwise
	// win the old branch ordering.  The existing Roam path is the only one
	// that knows how to validate portals, operate an ordinary manual door, and
	// select a lateral lane after a blocked command.
	const bool mustRegroupWithMate = !deathmatch && mate != nullptr &&
		(mustFollowPlannedDescent || !forcedMateReachable ||
			mateDistance > CompanionSoftLeashDistance || t_stuck >= 3);

	stuck = false;
	dist = dest ? player->mo->Distance2D(dest) : 0;

	if (missile &&
		((!missile->Vel.X && !missile->Vel.Y) || !Check_LOS(missile, DAngle::fromDeg(360.))))
	{
		sleft = !sleft;
		missile = nullptr; //Probably ended its travel.
	}

#if 0	// this has always been broken and without any reference it cannot be fixed.
	if (player->mo->Angles.Pitch > 0)
		player->mo->Angles.Pitch -= 80;
	else if (player->mo->Angles.Pitch <= -60)
		player->mo->Angles.Pitch += 80;
#endif

	//HOW TO MOVE:
	if (missile && (player->mo->Distance2D(missile)<AVOID_DIST)) //try avoid missile got from P_Mobj.c thinking part.
	{
		Pitch (missile);
		Angle = player->mo->AngleTo(missile);
		cmd->sidemove = sleft ? -SIDERUN : SIDERUN;
		cmd->forwardmove = -FORWARDRUN; //Back IS best.

		if ((player->mo->Pos() - old).LengthSquared() < THINKDISTSQ
			&& t_strafe<=0)
		{
			t_strafe = 5;
			sleft = !sleft;
		}

		//If able to see enemy while avoiding missile, still fire at enemy.
		if (enemy && Check_LOS (enemy, DAngle::fromDeg(SHOOTFOV)))
			Dofire (cmd); //Order bot to fire current weapon
	}
	else if (mustRegroupWithMate)
	{
		// Never leave this at an old item, enemy, or null destination.  In
		// particular, the previous unreachable-leader branch jumped into the
		// randomized roaming selector with dest often null, so a companion could
		// keep looking at a wall or choose an unrelated pickup instead of taking
		// the first valid turn toward its leader.
		dest = mate;

		if (!forcedMateReachable && mateDistance >= CompanionHardLeashDistance)
		{
			if (t_follow < CompanionCatchupTics)
			{
				++t_follow;
			}
			if (t_follow >= CompanionCatchupTics)
			{
				// Physical routing always gets its full recovery interval first. The emergency
				// recovery remains restricted to collision-checked positions in the
				// leader's current sector, so it cannot bypass a lock or trigger.
				if (TryCatchUpToMate())
				{
					t_follow = 0;
					dest = mate;
					return;
				}
				// Do not retry every tic when a leader-adjacent slot is occupied.
				t_follow = TICRATE / 2;
			}
		}
		else
		{
			t_follow = 0;
		}

		// Permit a safe shot while returning, but let Roam restore the bearing
		// toward the route.  ValidateAttackCommand will discard the shot unless
		// that actual movement bearing is also a safe firing lane.
		if (enemy && Check_LOS(enemy, DAngle::fromDeg(SHOOTFOV)))
			Dofire(cmd);
		Roam(cmd);
	}
	// An incoming missile still wins above, but a companion that has already
	// proven a small leader-directed descent must not be pinned on the upper
	// ledge by ordinary combat strafing. The follow branch keeps its weapon
	// safety checks and can fire at this visible hostile while it completes the
	// transition.
	else if (!mustFollowPlannedDescent && enemy &&
		P_CheckSight(player->mo, enemy, 0)) // Fight!
	{
		Pitch (enemy);

		//Check if it's more important to get an item than fight.
		if (dest && (dest->flags&MF_SPECIAL)) //Must be an item, that is close enough.
		{
#define is(x) dest->IsKindOf (PClass::FindClass (#x))
			if (
				(
				 (player->mo->health < skill.isp &&
				  (is (Medikit) ||
				   is (Stimpack) ||
				   is (Soulsphere) ||
				   is (Megasphere) ||
				   is (CrystalVial)
				  )
				 ) || (
				  is (Invulnerability) ||
				  is (Invisibility) ||
				  is (Megasphere)
				 ) || 
				 dist < (GETINCOMBAT/4) ||
				 (GetBotInfo(player->ReadyWeapon).MoveCombatDist == 0)
				)
				&& (dist < GETINCOMBAT || (GetBotInfo(player->ReadyWeapon).MoveCombatDist == 0))
				&& Reachable (dest))
#undef is
			{
				goto roam; //Pick it up, no matter the situation. All bonuses are nice close up.
			}
		}

		dest = nullptr; //To let bot turn right

			if (!(enemy->flags3 & MF3_ISMONSTER))
				t_fight = AFTERTICS;

		if (t_strafe <= 0 &&
			((player->mo->Pos() - old).LengthSquared() < THINKDISTSQ
			|| ((pr_botmove()%30)==10))
			)
		{
			stuck = true;
			t_strafe = 5;
			sleft = !sleft;
		}

		Angle = player->mo->AngleTo(enemy);

		if (player->ReadyWeapon == NULL ||
			player->mo->Distance2D(enemy) >
			GetBotInfo(player->ReadyWeapon).MoveCombatDist)
		{
			// If a monster, use lower speed (just for cooler apperance while strafing down doomed monster)
			cmd->forwardmove = (enemy->flags3 & MF3_ISMONSTER) ? FORWARDWALK : FORWARDRUN;
		}
		else if (!stuck) //Too close, so move away.
		{
			// If a monster, use lower speed (just for cooler apperance while strafing down doomed monster)
			cmd->forwardmove = (enemy->flags3 & MF3_ISMONSTER) ? -FORWARDWALK : -FORWARDRUN;
		}

		//Strafing.
		if (enemy->flags3 & MF3_ISMONSTER) //It's just a monster so take it down cool.
		{
			cmd->sidemove = sleft ? -SIDEWALK : SIDEWALK;
		}
		else
		{
			cmd->sidemove = sleft ? -SIDERUN : SIDERUN;
		}
		Dofire (cmd); //Order bot to fire current weapon
	}
	else if (mate && (mustFollowPlannedDescent || (!enemy && (!dest || dest==mate)))) //Follow mate move.
	{
		double matedist;

		Pitch (mate);
		matedist = player->mo->Distance2D(mate);
		double mateFloor = forcedMateFloor;
		bool mateUsesPlannedDescent = forcedMateUsesPlannedDescent;

		if (!mustFollowPlannedDescent &&
			!Reachable(mate, true, &mateFloor, &mateUsesPlannedDescent))
		{
			// A normal cooperative level can put the leader around a corner,
			// while procedural maps routinely do so through a loop, stair hall,
			// or manual keyed door. Let the legacy rover try first; only after
			// sustained, large separation use the collision-checked catch-up
			// point next to the leader.
			if (!deathmatch && matedist >= CompanionHardLeashDistance)
			{
				if (t_follow < CompanionCatchupTics)
				{
					++t_follow;
				}
				if (t_follow >= CompanionCatchupTics)
				{
					if (TryCatchUpToMate())
					{
						t_follow = 0;
						dest = mate;
						return;
					}
					// Avoid trying every tic when all leader-adjacent slots are
					// temporarily occupied. A short retry interval keeps the
					// fallback inexpensive and deterministic.
					t_follow = TICRATE;
				}
			}
			else
			{
				t_follow = 0;
			}
			// Keep the actual leader as the route goal.  Randomly clearing it
			// here used to hand a cornered companion to the generic target picker.
			dest = mate;
			goto roam;
		}
		t_follow = 0;
		if (mateUsesPlannedDescent)
		{
			// Keep the route target coherent as well as this tick's command.
			// Otherwise a pickup that was selected just before the leader crossed
			// the ledge would reclaim dest on the next tic and recreate the stall.
			dest = mate;
			// The direct reachability test above has checked every crossed portal,
			// support volume, and hazard. Preserve that proof through the final raw
			// player-command guard so a companion can follow a leader down a normal
			// bounded ledge (for example Doom II MAP01's start platform), without
			// granting the same exception to combat strafing.
			planned_descent_goal = mate->Pos().XY();
			planned_descent_floor = mateFloor;
			planned_descent_active = true;
		}

		Angle = player->mo->AngleTo(mate);

		// Horizontal distance alone is not a valid stopping condition across a
		// ledge. MAP01's player starts are only roughly 150 units apart across a
		// required 48-unit descent, below FRIEND_DIST, so preserve the verified
		// planned-descent intent long enough to actually walk to the crossing.
		if (mateUsesPlannedDescent)
			cmd->forwardmove = FORWARDRUN;
		else if (matedist > (FRIEND_DIST*2))
			cmd->forwardmove = FORWARDRUN;
		else if (matedist > FRIEND_DIST)
			cmd->forwardmove = FORWARDWALK; //Walk, when starting to get close.
		else if (deathmatch && matedist < FRIEND_DIST-(FRIEND_DIST/3)) //Competitive bots keep their personal space.
			cmd->forwardmove = -FORWARDWALK;

		// A verified leader transition owns the movement bearing. Dofire normally
		// turns Angle toward its target, which would rotate this forward command
		// away from the proven descent. Restore the leader bearing immediately;
		// the final attack validator will retain the shot only if the hostile is
		// also safe in that actual firing lane.
		if (mustFollowPlannedDescent && enemy && Check_LOS(enemy, DAngle::fromDeg(SHOOTFOV)))
		{
			Dofire(cmd);
			Pitch(mate);
			Angle = player->mo->AngleTo(mate);
		}
	}
	else //Roam after something.
	{
		first_shot = true;

	/////
	roam:
	/////
		if (enemy && Check_LOS (enemy, DAngle::fromDeg(SHOOTFOV*3/2))) //If able to see enemy while avoiding missile , still fire at it.
			Dofire (cmd); //Order bot to fire current weapon

		if (dest && !(dest->flags&MF_SPECIAL) && dest->health < 0)
		{ //Roaming after something dead.
			dest = nullptr;
		}

		if (dest == NULL)
		{
			if (t_fight && enemy) //Enemy/bot has jumped around corner. So what to do?
			{
				if (enemy->player)
				{
					if (((enemy->player->ReadyWeapon != NULL && GetBotInfo(enemy->player->ReadyWeapon).flags & BIF_BOT_EXPLOSIVE) ||
						(pr_botmove()%100)>skill.isp) && (GetBotInfo(player->ReadyWeapon).MoveCombatDist != 0))
						dest = enemy;//Dont let enemy kill the bot by supressive fire. So charge enemy.
					else //hide while t_fight, but keep view at enemy.
						Angle = player->mo->AngleTo(enemy);
				} //Just a monster, so kill it.
				else
					dest = enemy;

				//VerifFavoritWeapon(player); //Dont know why here.., but it must be here, i know the reason, but not why at this spot, uh.
			}
			else //Choose a distant target. to get things going.
			{
				r = pr_botmove();
				if (r < 128)
				{
					auto it = player->mo->Level->GetThinkerIterator<AActor>(NAME_Inventory, MAX_STATNUM+1, Level->BotInfo.firstthing);
					auto item = it.Next();

					if (item != NULL || (item = it.Next()) != NULL)
					{
						r &= 63;	// Only scan up to 64 entries at a time
						while (r)
						{
							--r;
							item = it.Next();
						}
						if (item == NULL)
							{
								item = it.Next();
							}
							if (item != nullptr)
							{
								Level->BotInfo.firstthing = item;
								// Keep the opportunistic inventory scan under the same
								// local, needed, direct-route policy as BotTick. A random
								// pickup on the far side of a map must not displace a leader.
								WhatToGet(item);
							}
						}
				}
				else if (mate && (r < 179 || P_CheckSight(player->mo, mate)))
				{
					dest = mate;
				}
				else
				{
					const unsigned int slot = r & (MAXPLAYERS - 1);
					// A companion held out of a legacy map remains a player slot so it
					// can retry next map, but it has no pawn in this level.
					if (playeringame[slot] && players[slot].mo != nullptr && players[slot].mo->health > 0)
					{
						dest = players[slot].mo;
					}
				}
			}

			if (dest)
			{
				t_roam = MAXROAM;
			}
		}
		if (dest)
		{ //Bot has a target so roam after it.
			Roam (cmd);
		}

	} //End of movement main part.

	if (!t_roam && dest)
	{
		prev = dest;
		dest = nullptr;
	}

	movement_requested = cmd->forwardmove != 0 || cmd->sidemove != 0;
	old = player->mo->Pos().XY();
}

int P_GetRealMaxHealth(AActor *actor, int max);

//BOT_WhatToGet
//
//Determines if the bot will roam after an item or not.
void DBot::WhatToGet (AActor *item)
{
	if (player == nullptr || player->mo == nullptr || item == nullptr ||
		(item->renderflags & RF_INVISIBLE) //Under respawn and away.
		|| item == prev)
	{
		return;
	}
	// Do not leave this restriction solely in BotTick: the periodic inventory
	// scan below starts at an arbitrary thinker and calls WhatToGet directly.
	// Both paths therefore share the same local, visible pickup policy before a
	// pickup can displace follow or combat behavior.
	if (item->Distance2D(player->mo) > BotPickupMaxDistance ||
		!P_CheckSight(player->mo, item, SF_SEEPASTBLOCKEVERYTHING))
	{
		return;
	}
	// A co-op companion may take a reward only while it remains a local choice
	// for both itself and the leader.  Without this second anchor a pickup on
	// the far side of a room could overwrite a perfectly good follow target and
	// make the bot appear to run ahead or get lost behind the player.
	if (!deathmatch && mate != nullptr &&
		(player->mo->Distance2D(mate) > CompanionSoftLeashDistance ||
			item->Distance2D(mate) > CompanionLocalPickupDistance))
	{
		return;
	}
	// A visible local item can still sit beyond a locked door, lift, or
	// map-script transition. It is never important enough to justify a
	// speculative whole-map roam. The route navigator remains available for
	// player goals.
	if (!Reachable(item))
	{
		return;
	}
	int weapgiveammo = (alwaysapplydmflags || deathmatch) && !(dmflags & DF_WEAPONS_STAY);

	if (item->IsKindOf(NAME_Weapon))
	{
		// FIXME
		auto heldWeapon = player->mo->FindInventory(item->GetClass());
		if (heldWeapon != NULL)
		{
			if (!weapgiveammo)
				return;
			auto ammo1 = heldWeapon->PointerVar<AActor>(NAME_Ammo1);
			auto ammo2 = heldWeapon->PointerVar<AActor>(NAME_Ammo2);
			if ((ammo1 == NULL || ammo1->IntVar(NAME_Amount) >= ammo1->IntVar(NAME_MaxAmount)) &&
				(ammo2 == NULL || ammo2->IntVar(NAME_Amount) >= ammo2->IntVar(NAME_MaxAmount)))
			{
				return;
			}
		}
	}
	else if (item->IsKindOf (PClass::FindActor(NAME_Ammo)))
	{
		auto ac = PClass::FindActor(NAME_Ammo);
		auto parent = item->GetClass();
		while (parent->ParentClass != ac) parent = static_cast<PClassActor*>(parent->ParentClass);
		AActor *holdingammo = player->mo->FindInventory(parent);
		if (holdingammo != NULL && holdingammo->IntVar(NAME_Amount) >= holdingammo->IntVar(NAME_MaxAmount))
		{
			return;
		}
	}
	else if (item->GetClass()->TypeName == NAME_Megasphere || item->IsKindOf(NAME_Health))
	{
		// do the test with the health item that's actually given.
		AActor* const testItem = item->GetClass()->TypeName == NAME_Megasphere
			? GetDefaultByName(NAME_MegasphereHealth)
			: item;
		if (nullptr != testItem)
		{
			const int maxhealth = P_GetRealMaxHealth(player->mo, testItem->IntVar(NAME_MaxAmount));
			if (player->mo->health >= maxhealth)
				return;
		}
	}

	if ((dest == NULL ||
		!(dest->flags & MF_SPECIAL)/* ||
		!Reachable (dest)*/)/* &&
		Reachable (item)*/)	// Calling Reachable slows this down tremendously
	{
		prev = dest;
		dest = item;
		t_roam = MAXROAM;
	}
}

void DBot::Set_enemy ()
{
	AActor *oldenemy;

	if (enemy
		&& enemy->health > 0
		&& (enemy->flags & MF_SHOOTABLE)
		&& !player->mo->IsFriend(enemy)
		&& P_CheckSight (player->mo, enemy))
	{
		oldenemy = enemy;
	}
	else
	{
		oldenemy = NULL;
	}

	// Do not let a monster that disappeared around a corner pin a companion in
	// combat forever. Visible targets are retained; otherwise reacquire before
	// the movement planner gives up following its leader or a useful item.
	if (deathmatch || !oldenemy)
	{
		allround = !!oldenemy;
		enemy = Find_enemy();
		if (!enemy)
			enemy = oldenemy; //Try go for last (it will be NULL if there wasn't anyone)
	}
	//Verify that that enemy is really something alive that bot can kill.
	if (enemy && ((enemy->health < 0 || !(enemy->flags&MF_SHOOTABLE)) || player->mo->IsFriend(enemy)))
		enemy = nullptr;
}
