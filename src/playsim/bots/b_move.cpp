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
* Movement/Roaming code for     *
* the bot's					    *
*********************************/

#include "doomdef.h"
#include "doomstat.h"
#include "p_local.h"
#include "p_maputl.h"
#include "b_bot.h"
#include "g_game.h"
#include "p_lnspec.h"
#include "a_keys.h"
#include "d_event.h"
#include "p_enemy.h"
#include "d_player.h"
#include "p_spec.h"
#include "p_checkposition.h"
#include "actorinlines.h"

static FRandom pr_bottrywalk ("BotTryWalk");
static FRandom pr_botnewchasedir ("BotNewChaseDir");

// borrow some tables from p_enemy.cpp
extern dirtype_t opposite[9];
extern dirtype_t diags[4];

namespace
{
	constexpr int BotRouteRefreshTics = 8;
	constexpr int BotDoorRetryTics = TICRATE / 3;
	constexpr int BotBlockedDirectionTics = 8;
	// Small IWAD maps should retain the quick 512-sector search, while a
	// generated size-160 route can legitimately cross more sectors before it
	// reaches its leader.  The hard cap keeps hostile/exceptional PWAD graphs
	// from turning a one-tic companion decision into an unbounded search.
	constexpr unsigned int BotRouteMinExpansions = 512;
	constexpr unsigned int BotRouteMaxExpansions = 4096;

	// `movedir` is a world-space eight-way route choice, while a player bot's
	// movement command is relative to its current view angle. Keeping those
	// two independent is what made a bot repeatedly press into the outside of
	// a corner: NewChaseDir could find a clear lateral direction, but the
	// pending forward command still used the heading that led into the wall.
	DAngle MoveDirectionAngle(dirtype_t direction)
	{
		return DAngle::fromDeg(45.0 * static_cast<int>(direction));
	}

	int LineSideForSector(const line_t *line, const sector_t *sector)
	{
		if (line == nullptr || sector == nullptr)
		{
			return -1;
		}
		if (line->frontsector == sector)
		{
			return 0;
		}
		return line->backsector == sector ? 1 : -1;
	}

	// Do not reduce a routed boundary to its base sector planes. 3D platforms
	// and overhangs can make those planes disagree with the floor and clearance
	// that P_TryMove will actually choose. This probe intentionally does not run
	// P_CheckPosition: route planning must not trigger line specials or let a
	// transient actor block a static graph edge.
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
		// A 3D floor's damage lives on the visual/base sector. Portal support can
		// refer to a different sector, so checking only one misses a real hazard.
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

	DVector2 ClosestPointOnLineSegment(const DVector2 &position, const line_t *line)
	{
		const DVector2 first = line->v1->fPos();
		const DVector2 delta = line->v2->fPos() - first;
		const double lengthSquared = delta.LengthSquared();
		if (lengthSquared <= 1e-12)
		{
			return first;
		}
		const DVector2 relative = position - first;
		const double fraction = clamp((relative.X * delta.X + relative.Y * delta.Y) /
			lengthSquared, 0.0, 1.0);
		return first + delta * fraction;
	}

	bool IsNavigationLiftSpecial(int special)
	{
		switch (special)
		{
		case Generic_Lift:
		case Plat_DownWaitUpStay:
		case Plat_DownByValue:
		case Plat_UpWaitDownStay:
		case Plat_UpByValue:
		case Plat_UpNearestWaitDownStay:
		case Plat_DownWaitUpStayLip:
		case Plat_UpByValueStayTx:
			return true;
		default:
			return false;
		}
	}

	bool IsNavigationMoverSpecial(int special)
	{
		if (IsNavigationLiftSpecial(special))
		{
			return true;
		}
		switch (special)
		{
		case Door_Open:
		case Door_Raise:
		case Door_LockedRaise:
		case Door_Animated:
		case Door_WaitRaise:
		case Door_CloseWaitOpen:
		case Generic_Door:
			return true;
		default:
			return false;
		}
	}

	bool HasRequiredKey(AActor *actor, const line_t *line)
	{
		int lock = line->locknumber;
		if (lock == 0 && (line->special == Door_LockedRaise || line->special == Door_Animated))
		{
			lock = line->args[3];
		}
		else if (lock == 0 && line->special == Generic_Door)
		{
			lock = line->args[4];
		}
		return lock == 0 || P_CheckKeys(actor, lock, false, true);
	}

	// A tagged use special can operate a completely unrelated sector. It may be
	// a useful switch to a human, but it is not proof that the physical boundary
	// the route search is about to cross will open. Only plan across the manual,
	// local form of a known door/lift; ordinary geometry is still traversed as
	// soon as its real opening passes the clearance probe.
	bool ControlsAdjacentNavigationCrossing(const line_t *line)
	{
		switch (line->special)
		{
		case Generic_Door:
			// Generic_Door's local-door bit changes arg0 into a light tag, so
			// its mover target is local even if that light tag is nonzero.
			return line->args[0] == 0 || (line->args[2] & 128) != 0;

		case Door_Open:
		case Door_Raise:
		case Door_LockedRaise:
		case Door_Animated:
		case Door_WaitRaise:
		case Door_CloseWaitOpen:
		case Generic_Lift:
		case Plat_DownWaitUpStay:
		case Plat_DownByValue:
		case Plat_UpWaitDownStay:
		case Plat_UpByValue:
		case Plat_UpNearestWaitDownStay:
		case Plat_DownWaitUpStayLip:
		case Plat_UpByValueStayTx:
			return line->args[0] == 0;

		default:
			return false;
		}
	}

	bool CanPlanNavigationLineFromSide(AActor *actor, const line_t *line, int side)
	{
		if (actor == nullptr || line == nullptr ||
			!IsNavigationMoverSpecial(line->special) ||
			!ControlsAdjacentNavigationCrossing(line) || !HasRequiredKey(actor, line))
		{
			return false;
		}

		if (side != 0 && side != 1)
		{
			return false;
		}
		if (side == 1)
		{
			return (line->flags & ML_FIRSTSIDEONLY) == 0 &&
				(line->activation & SPAC_UseBack) != 0;
		}
		// SPAC_UseBack alone is an explicitly back-side-only action. Treating it
		// as a front-side use made the graph claim a one-way door was available
		// even though P_UseLines would correctly refuse it.
		return (line->activation & (SPAC_Use | SPAC_UseThrough)) != 0;
	}

	bool CanPlanNavigationLine(AActor *actor, const line_t *line, const sector_t *from)
	{
		return CanPlanNavigationLineFromSide(actor, line, LineSideForSector(line, from));
	}

	// This is deliberately a non-activating mirror of the traversal portion of
	// P_UseLines/P_UseTraverse. A bot's actual BT_USE is consumed on the next
	// player tic, so P_TestActivateLine alone is not enough: an intervening
	// actor, special, solid wall, or line portal can make P_UseLines stop before
	// the selected door. Be conservative around everything that would require
	// invoking map code to classify; declining a use pulse is always safer than
	// accidentally using an unrelated special.
	bool NavigationUseRayReachesLine(AActor *actor, line_t *line, int expectedSide,
		DAngle yaw, DVector3 &activationPosition)
	{
		if (actor == nullptr || line == nullptr || (expectedSide != 0 && expectedSide != 1))
		{
			return false;
		}

		const DVector2 start = actor->GetPortalTransition(actor->Height / 2).XY();
		const DVector2 end = start + yaw.ToVector(actor->FloatVar(NAME_UseRange));
		FPathTraverse it(actor->Level, start.X, start.Y, end.X, end.Y,
			PT_ADDLINES | PT_ADDTHINGS);
		intercept_t *in;
		// P_UseTraverse starts this at the transitioned XY but the actor's live Z.
		// PortalRelocate updates it for each linked portal before the target line's
		// side and switch-range test, which is why using actor->Pos() here would
		// not prove the same trace.
		DVector3 xpos = { start.X, start.Y, actor->Z() };

		while ((in = it.Next()))
		{
			if (!in->isaline)
			{
				AActor *intercepted = in->d.thing;
				if (intercepted == nullptr)
				{
					return false;
				}
				if (intercepted == actor)
				{
					continue;
				}
				// P_UseTraverse can call arbitrary Used/puzzle hooks on an
				// intercepting actor. Do not call those hooks while planning and
				// do not let a later door pulse risk calling one for real. Ordinary
				// actors without a use hook are transparent to P_UseTraverse.
				if (intercepted->flags5 & MF5_USESPECIAL || intercepted->special == UsePuzzleItem)
				{
					return false;
				}
				IFVIRTUALPTR(intercepted, AActor, Used)
				{
					return false;
				}
				continue;
			}

			if (it.PortalRelocate(in, PT_ADDLINES | PT_ADDTHINGS, &xpos))
			{
				continue;
			}

			line_t *traversed = in->d.line;
			if (traversed == line)
			{
				if (P_PointOnLineSide(xpos.XY(), traversed) != expectedSide)
				{
					return false;
				}
				activationPosition = xpos;
				return true;
			}

			// A prior use-capable special could consume the interaction (including
			// USETHROUGH, which may execute and then continue) and a dry planner
			// must never invoke it merely to find out. Reject it instead.
			if (traversed->special != 0 &&
				(traversed->activation & (SPAC_Use | SPAC_UseThrough | SPAC_UseBack)) != 0)
			{
				return false;
			}

			// Match P_UseTraverse's non-special/wrong-activation wall check. An
			// open ordinary boundary is safe to see through; a use-blocking line,
			// closed opening, or compatibility-blocking special is not.
			FLineOpening open;
			if (traversed->flags & (ML_BLOCKEVERYTHING | ML_BLOCKUSE))
			{
				open.range = 0;
			}
			else
			{
				P_LineOpening(open, nullptr, traversed, it.InterceptPoint(in));
			}
			if (open.range <= 0 ||
				(traversed->special != 0 && (actor->Level->i_compatflags & COMPATF_USEBLOCKING)))
			{
				return false;
			}
		}

		return false;
	}

	bool CanUseNavigationLineAtYaw(AActor *actor, line_t *line, int expectedSide, DAngle yaw)
	{
		if (!CanPlanNavigationLineFromSide(actor, line, expectedSide))
		{
			return false;
		}
		DVector3 activationPosition;
		if (!NavigationUseRayReachesLine(actor, line, expectedSide, yaw, activationPosition))
		{
			return false;
		}
		const int activation = expectedSide == 1 ? SPAC_UseBack : SPAC_Use;
		// P_TestActivateLine honors ML_CHECKSWITCHRANGE using the actor's yaw.
		// Feed it the portal-relocated position that P_UseTraverse would pass to
		// P_ActivateLine, then restore the temporary test yaw without activating
		// a map special.
		const DAngle savedYaw = actor->Angles.Yaw;
		actor->Angles.Yaw = yaw;
		const bool usable = P_TestActivateLine(line, actor, expectedSide, activation,
			&activationPosition);
		actor->Angles.Yaw = savedYaw;
		return usable;
	}

	bool CanUseNavigationLine(AActor *actor, line_t *line, sector_t *from, const DVector2 &aimPoint)
	{
		// Planning runs before TurnToAng applies this tic's requested turn. Pick
		// the closest valid point for the initial conservative line selection;
		// the command itself is separately validated at its actual next-tic yaw.
		DAngle aimYaw = actor->Angles.Yaw;
		if ((aimPoint - actor->Pos().XY()).LengthSquared() > 1.0)
		{
			aimYaw = (aimPoint - actor->Pos().XY()).Angle();
		}
		return CanUseNavigationLineAtYaw(actor, line, LineSideForSector(line, from), aimYaw);
	}

	// `P_CheckPosition` reports the lowest floor touched by the actor's
	// collision cylinder, which is exactly what the ordinary dropoff guard uses.
	// A bot may only override that guard when the movement planner already
	// proved a route to this lower support. Requiring both a near-matching target
	// floor and forward alignment prevents combat strafing or a stale route from
	// turning a nearby unrelated edge into an allowed fall.
	bool IsPlannedDescent(AActor *actor, double x, double y, const FCheckPosition &probe,
		const DVector2 *goal, double plannedFloor)
	{
		if (actor == nullptr || goal == nullptr || probe.dropoffisportal ||
			!BotCanTraverseDrop(probe.floorz, probe.dropoffz,
				actor->MaxDropOffHeight, true) ||
			fabs(plannedFloor - probe.dropoffz) > actor->MaxStepHeight)
		{
			return false;
		}

		const DVector2 position = actor->Pos().XY();
		const DVector2 approach(x - position.X, y - position.Y);
		const DVector2 towardGoal = *goal - position;
		const double approachLength = approach.Length();
		const double goalLength = towardGoal.Length();
		if (approachLength <= 1e-6 || goalLength <= max(actor->radius, approachLength))
		{
			return false;
		}

		// Keep the approved descent inside a 60-degree cone around the planned
		// route. A smaller cone is unnecessarily brittle at ordinary doorways;
		// a wider one starts to admit avoidance strafes alongside the ledge.
		const double alignment = (approach.X * towardGoal.X + approach.Y * towardGoal.Y) /
			(approachLength * goalLength);
		return alignment >= 0.5;
	}

}

void DBot::ApplyPendingRecoveryMoveCommand(usercmd_t *cmd, DAngle nextTickYaw)
{
	const int direction = pendingRecoveryMoveDirection;
	pendingRecoveryMoveDirection = DI_NODIR;
	if (direction < DI_EAST || direction >= DI_NODIR)
	{
		return;
	}

	// Bot thinking happens after P_PlayerThink. The command written here is
	// consumed on the next tic, after PlayerPawn applies cmd->yaw. Project
	// against that final yaw rather than the current visual yaw, otherwise a
	// freshly selected escape lane is rotated into the same corner we just
	// rejected. Forward and side have different legal command maxima, so use
	// the largest common vector magnitude that stays inside both axes instead
	// of skewing diagonals with FORWARDRUN/SIDERUN independently.
	const DAngle turn = deltaangle(nextTickYaw, MoveDirectionAngle((dirtype_t)direction));
	const double forward = turn.Cos();
	const double side = -turn.Sin();
	const double forwardMagnitude = fabs(forward);
	const double sideMagnitude = fabs(side);
	const double scale = min(FORWARDRUN / max(forwardMagnitude, 1e-12),
		SIDERUN / max(sideMagnitude, 1e-12));
	cmd->forwardmove = clamp(xs_CRoundToInt(scale * forward), -FORWARDRUN, FORWARDRUN);
	cmd->sidemove = clamp(xs_CRoundToInt(scale * side), -SIDERUN, SIDERUN);
}

void DBot::ValidateMovementCommand(usercmd_t *cmd, DAngle nextTickYaw)
{
	if (player == nullptr || player->mo == nullptr ||
		(cmd->forwardmove == 0 && cmd->sidemove == 0))
	{
		return;
	}

	// Follow and combat historically wrote raw player commands directly, which
	// bypassed CleanAhead completely. Project the final relative input into a
	// short world-space capsule probe so the same walls, dropoffs, crushers, and
	// damaging 3D floors that protect the rover also protect strafing and leader
	// following on arbitrary maps.
	const DVector2 forward(nextTickYaw.Cos(), nextTickYaw.Sin());
	const DAngle sideYaw = nextTickYaw - DAngle::fromDeg(90.0);
	const DVector2 side(sideYaw.Cos(), sideYaw.Sin());
	DVector2 direction = forward * cmd->forwardmove + side * cmd->sidemove;
	const double length = direction.Length();
	if (length <= 1e-6)
	{
		return;
	}
	direction /= length;

	if (!Level->BotInfo.CleanAhead(player->mo,
		player->mo->X() + direction.X * 16.0,
		player->mo->Y() + direction.Y * 16.0, cmd,
		planned_descent_active ? &planned_descent_goal : nullptr,
		planned_descent_floor))
	{
		cmd->forwardmove = 0;
		cmd->sidemove = 0;
		player->mo->movecount = -1;
		route_waypoint_valid = false;
		t_route = 0;
	}
}

void DBot::ValidateNavigationUseCommand(usercmd_t *cmd, DAngle nextTickYaw)
{
	line_t *line = pending_navigation_use_line;
	const int expectedSide = pending_navigation_use_side;
	pending_navigation_use_line = nullptr;
	pending_navigation_use_side = -1;
	if (cmd == nullptr || line == nullptr || player == nullptr || player->mo == nullptr ||
		t_active > 0 || (cmd->buttons & BT_USE) != 0)
	{
		return;
	}

	// Move planned this line against its closest point, but P_CheckUse sees the
	// command only after PlayerPawn has applied cmd->yaw. A sharp L-turn used to
	// pulse BT_USE immediately while still looking down the old corridor, then
	// suppress retry while t_active expired. Test the exact facing ray (and the
	// side recorded when the physical transition was selected) before emitting
	// the one interaction pulse.
	if (CanUseNavigationLineAtYaw(player->mo, line, expectedSide, nextTickYaw))
	{
		cmd->buttons |= BT_USE;
		t_active = BotDoorRetryTics;
	}
}

bool DBot::FindRouteWaypoint(AActor *target, DVector2 &waypoint,
	double &waypointFloor, bool &usesPlannedDescent, bool allowPlannedDescent)
{
	waypointFloor = 0.0;
	usesPlannedDescent = false;
	if (target == nullptr || player == nullptr || player->mo == nullptr ||
		player->mo->Sector == nullptr || target->Sector == nullptr)
	{
		return false;
	}

	const int startSector = player->mo->Sector->sectornum;
	const int targetSector = target->Sector->sectornum;
	const unsigned int sectorCount = Level->sectors.Size();
	if (startSector < 0 || targetSector < 0 || (unsigned int)startSector >= sectorCount ||
		(unsigned int)targetSector >= sectorCount || startSector == targetSector)
	{
		return false;
	}

	if (route_waypoint_valid && route_target == target && t_route > 0 &&
		route_target_sector == targetSector && route_waypoint_sector != startSector &&
		route_waypoint_allows_planned_descent == allowPlannedDescent)
	{
		waypoint = route_waypoint;
		waypointFloor = route_waypoint_floor;
		usesPlannedDescent = route_waypoint_uses_planned_descent;
		return true;
	}
	if (!route_waypoint_valid && route_target == target && t_route > 0 &&
		route_target_sector == targetSector && route_waypoint_sector == startSector &&
		route_waypoint_allows_planned_descent == allowPlannedDescent)
	{
		// A route miss is just as expensive as a route hit on a dense map. Keep
		// a very short, target-and-sector-specific negative cache so a closed
		// route does not rebuild a 512-sector search every tic. Dynamic geometry
		// and a sector change invalidate it on the next planning pass.
		return false;
	}

	route_target = target;
	route_waypoint_valid = false;
	route_target_sector = targetSector;
	route_waypoint_sector = -1;
	route_waypoint_floor = 0.0;
	route_waypoint_allows_planned_descent = allowPlannedDescent;
	route_waypoint_uses_planned_descent = false;
	FCheckPosition startSupport;
	if (!ProbeBotSupport(player->mo, player->mo->Sector, player->mo->Pos().XY(),
		player->mo->Z(), startSupport) || IsDangerousBotSupport(player->mo, startSupport))
	{
		route_waypoint_sector = startSector;
		t_route = BotRouteRefreshTics;
		return false;
	}

	TArray<int> parent;
	TArray<line_t *> incomingLine;
	TArray<double> supportFloor;
	TArray<int> queue;
	parent.Resize(sectorCount);
	incomingLine.Resize(sectorCount);
	supportFloor.Resize(sectorCount);
	for (unsigned int i = 0; i < sectorCount; ++i)
	{
		parent[i] = -2;
		incomingLine[i] = nullptr;
	}
	parent[startSector] = -1;
	supportFloor[startSector] = startSupport.floorz;
	queue.Push(startSector);

	const unsigned int routeExpansionBudget = clamp(sectorCount,
		BotRouteMinExpansions, BotRouteMaxExpansions);
	unsigned int head = 0;
	unsigned int expansions = 0;
	while (head < queue.Size() && expansions++ < routeExpansionBudget)
	{
		const int currentIndex = queue[head++];
		sector_t *current = &Level->sectors[currentIndex];
		const double currentFloor = supportFloor[currentIndex];
		for (line_t *line : current->Lines)
		{
			sector_t *next = getNextSector(line, current);
			if (next == nullptr || next->sectornum < 0 ||
				(unsigned int)next->sectornum >= sectorCount ||
				parent[next->sectornum] != -2 ||
				(line->flags & (ML_BLOCKING | ML_BLOCKEVERYTHING | ML_BLOCK_PLAYERS | ML_SECRET)))
			{
				continue;
			}

			const DVector2 portal = (line->v1->fPos() + line->v2->fPos()) * 0.5;
			DVector2 supportPoint;
			if (!ProbePointPastLine(player->mo, line, next, portal, supportPoint))
			{
				continue;
			}
			FCheckPosition nextSupport;
			if (!ProbeBotSupport(player->mo, next, supportPoint, currentFloor, nextSupport))
			{
				continue;
			}
			const double nextFloor = nextSupport.floorz;
			FLineOpening opening;
			if (fabs(currentFloor - player->mo->Z()) <= EQUAL_EPSILON)
			{
				P_LineOpening(opening, player->mo, line, portal);
			}
			else
			{
				GetBotLineOpeningAtSupport(opening, player->mo, line, portal, currentFloor);
			}
			const double portalWidth = (line->v2->fPos() - line->v1->fPos()).Length();
			const bool portalWideEnough = portalWidth >= player->mo->radius * 2.0 + 8.0;
			const bool landingSafe =
				(opening.lowfloor <= LINEOPEN_MIN ||
					BotCanTraverseDrop(currentFloor, opening.lowfloor,
						player->mo->MaxDropOffHeight, allowPlannedDescent)) &&
				nextFloor <= currentFloor + player->mo->MaxStepHeight &&
				BotCanTraverseDrop(currentFloor, nextFloor,
					player->mo->MaxDropOffHeight, allowPlannedDescent);
			const bool clearNow = portalWideEnough && landingSafe &&
				opening.range >= player->mo->Height &&
				opening.top >= currentFloor + player->mo->Height &&
				opening.bottom <= currentFloor + player->mo->MaxStepHeight;
			// Do not infer that a sector is passable merely because some unrelated
			// wall in it happens to be a door or lift. The exact crossed line must
			// be a verified manual mover, otherwise bots could route through a
			// closed ceiling, a remote switch puzzle, or the wrong lift boundary.
			const bool canOperate = CanPlanNavigationLine(player->mo, line, current);
			if (!portalWideEnough || !landingSafe || (!clearNow && !canOperate) ||
				Level->BotInfo.IsDangerous(player->mo, next, portal, nextFloor) ||
				IsDangerousBotSupport(player->mo, nextSupport))
			{
				continue;
			}

			parent[next->sectornum] = currentIndex;
			incomingLine[next->sectornum] = line;
			supportFloor[next->sectornum] = nextFloor;
			if (next->sectornum == targetSector)
			{
				head = queue.Size();
				break;
			}
			queue.Push(next->sectornum);
		}
	}

	if (parent[targetSector] == -2)
	{
		route_waypoint_sector = startSector;
		t_route = BotRouteRefreshTics;
		return false;
	}

	int firstSector = targetSector;
	while (parent[firstSector] != startSector && parent[firstSector] >= 0)
	{
		firstSector = parent[firstSector];
	}
	line_t *portalLine = incomingLine[firstSector];
	if (portalLine == nullptr)
	{
		route_waypoint_sector = startSector;
		t_route = BotRouteRefreshTics;
		return false;
	}

	sector_t *next = &Level->sectors[firstSector];
	const DVector2 portal = (portalLine->v1->fPos() + portalLine->v2->fPos()) * 0.5;
	DVector2 inward = next->centerspot - portal;
	const double inwardLength = inward.Length();
	if (inwardLength > 1.0)
	{
		inward /= inwardLength;
		const DVector2 candidate = portal + inward * max(player->mo->radius + 8.0, 24.0);
		if (Level->PointInSector(candidate) == next)
		{
			waypoint = candidate;
		}
		else
		{
			waypoint = portal;
		}
	}
	else
	{
		waypoint = portal;
	}

	route_waypoint = waypoint;
	route_waypoint_floor = supportFloor[firstSector];
	FLineOpening firstOpening;
	if (fabs(startSupport.floorz - player->mo->Z()) <= EQUAL_EPSILON)
	{
		P_LineOpening(firstOpening, player->mo, portalLine, portal);
	}
	else
	{
		GetBotLineOpeningAtSupport(firstOpening, player->mo, portalLine, portal,
			startSupport.floorz);
	}
	const bool firstOpeningDropIsNormal = firstOpening.lowfloor <= LINEOPEN_MIN ||
		BotCanTraverseDrop(startSupport.floorz, firstOpening.lowfloor,
			player->mo->MaxDropOffHeight, false);
	const bool firstSupportDropIsNormal = BotCanTraverseDrop(startSupport.floorz,
		route_waypoint_floor, player->mo->MaxDropOffHeight, false);
	route_waypoint_uses_planned_descent = !firstOpeningDropIsNormal ||
		!firstSupportDropIsNormal;
	route_waypoint_sector = firstSector;
	route_waypoint_valid = true;
	t_route = BotRouteRefreshTics;
	waypointFloor = route_waypoint_floor;
	usesPlannedDescent = route_waypoint_uses_planned_descent;
	return true;
}

// Called while the bot moves after its dest mobj, which can be a weapon,
// enemy, item, or cooperative leader. Direct visibility is cheap and best;
// when it fails, a short-lived sector-route waypoint guides the legacy rover
// through ordinary doors, loops, and stair rooms without claiming to solve
// arbitrary scripted map logic.
void DBot::Roam (usercmd_t *cmd)
{
	if (dest == nullptr)
	{
		return;
	}

	// Only a cooperative leader-follow route can deliberately cross a small,
	// verified one-way descent. A hostile may be visible while a companion is
	// regrouping, but it never changes the route target: pickups, lost enemy
	// pursuit, and combat roaming retain the normal drop limit, so this cannot
	// turn a tactical sidestep into a cliff jump.
	const bool allowPlannedDescent = dest == mate && mate != nullptr && !deathmatch;
	double targetFloor = 0.0;
	bool directUsesPlannedDescent = false;
	const bool direct = Reachable(dest, allowPlannedDescent, &targetFloor,
		&directUsesPlannedDescent);
	DVector2 waypoint;
	double waypointFloor = 0.0;
	bool routeUsesPlannedDescent = false;
	const bool routed = !direct && FindRouteWaypoint(dest, waypoint, waypointFloor,
		routeUsesPlannedDescent, allowPlannedDescent);
	const DVector2 goal = routed ? waypoint : dest->Pos().XY();

	if (direct || routed)
	{
		if (allowPlannedDescent &&
			((direct && directUsesPlannedDescent) || (routed && routeUsesPlannedDescent)))
		{
			planned_descent_goal = goal;
			planned_descent_floor = routed ? waypointFloor : targetFloor;
			planned_descent_active = true;
		}
		Angle = (goal - player->mo->Pos().XY()).Angle();
		// Do not keep an old randomized roam lane for up to sixty tics after a
		// leader, enemy, or route waypoint moves to the opposite side of the bot.
		// Re-probe immediately when that lane now points broadly away from the
		// current safe goal; NewChaseDir will still choose a collision-clear
		// eight-way alternative rather than blindly following Angle.
		if (player->mo->movedir < DI_NODIR &&
			absangle(Angle, MoveDirectionAngle((dirtype_t)player->mo->movedir)) > DAngle::fromDeg(67.5))
		{
			player->mo->movecount = -1;
		}
	}
	else if (player->mo->movedir < DI_NODIR)
	{
		Angle = MoveDirectionAngle((dirtype_t)player->mo->movedir);
	}

	// Move() emits a single real BT_USE pulse for a verified movement link and
	// marks t_active while the geometry starts moving. Do not turn an opening
	// door or lowering lift into a random escape decision while it is working.
	if (!direct && t_active > 0)
	{
		return;
	}

	if (t_stuck >= 3)
	{
		player->mo->movecount = -1;
		route_waypoint_valid = false;
		t_route = 0;
	}

	if (--player->mo->movecount < 0 || t_stuck >= 3 || !Move(cmd))
	{
		NewChaseDir(cmd, routed ? &goal : nullptr);
	}
}

bool DBot::Move (usercmd_t *cmd)
{
	double tryx, tryy;
	bool try_ok;
	bool usableTransition;
	EBotMoveBlockReason blockReason;

	if (player->mo->movedir >= DI_NODIR)
	{
		player->mo->movedir = DI_NODIR;	// make sure it's valid.
		return false;
	}

	tryx = player->mo->X() + 8*xspeed[player->mo->movedir];
	tryy = player->mo->Y() + 8*yspeed[player->mo->movedir];

	try_ok = Level->BotInfo.CleanAhead(player->mo, tryx, tryy, cmd,
		planned_descent_active ? &planned_descent_goal : nullptr,
		planned_descent_floor, &blockReason);

	if (!try_ok) // Anything blocking that could be opened etc..
	{
		// A usable special line is relevant for collision geometry, or for a
		// verified lift that is presently too high to step onto. CleanAhead also
		// rejects unsafe but otherwise open space (damaging floors, bad clearance,
		// and drops); do not send BT_USE at an adjacent door/lift after one of
		// those safety checks has already ruled out the transition.
		const bool canOperateTransition = blockReason == EBotMoveBlockReason::Geometry ||
			blockReason == EBotMoveBlockReason::VerticalStep;
		if (!canOperateTransition || !spechit.Size())
		{
			blocked_move_direction = player->mo->movedir;
			t_blocked_direction = BotBlockedDirectionTics;
			return false;
		}

		const int attemptedDirection = player->mo->movedir;
		player->mo->movedir = DI_NODIR;
		usableTransition = false;
		spechit_t spechit1;
		line_t *ld;
		int selectedInteractionSide = -1;

		while (spechit.Pop (spechit1))
		{
			ld = spechit1.line;
			if (blockReason == EBotMoveBlockReason::VerticalStep &&
				!IsNavigationLiftSpecial(ld->special))
			{
				continue;
			}
			// Never treat arbitrary map scripts, exits, or push-only actions as a
			// door. The actual use command must match a conservative movement
			// transition and the real approach side, including UDMF locknumber.
			const DVector2 interactionPoint = ClosestPointOnLineSegment(player->mo->Pos().XY(), ld);
			const int interactionSide = LineSideForSector(ld, player->mo->Sector);
			if (CanUseNavigationLine(player->mo, ld, player->mo->Sector, interactionPoint))
			{
				usableTransition = true;
				selectedInteractionSide = interactionSide;
				break;
			}
		}
		if (usableTransition)
		{
			// Aim the next-tic BT_USE trace at the exact verified line rather than
			// merely at a distant destination beyond it. This matters for narrow
			// doors and for a route that reaches the line from its back side.
			// P_TestActivateLine has already verified that this exact line is a
			// legal mover. Aim at its closest point, not its midpoint: the midpoint
			// of a long door or lift edge can be beyond the player's use trace even
			// when the near part of the same line was in range.
			const DVector2 interactionPoint = ClosestPointOnLineSegment(player->mo->Pos().XY(), ld);
			if ((interactionPoint - player->mo->Pos().XY()).LengthSquared() > 1.0)
			{
				Angle = (interactionPoint - player->mo->Pos().XY()).Angle();
			}
			// TurnToAng has not produced the final queued yaw yet. Defer the pulse
			// until Think can verify P_TestActivateLine against that actual next-tic
			// facing ray; see ValidateNavigationUseCommand().
			pending_navigation_use_line = ld;
			pending_navigation_use_side = selectedInteractionSide;
			return true;
		}

		blocked_move_direction = attemptedDirection;
		t_blocked_direction = BotBlockedDirectionTics;
		return false;
	}
	else //Move forward.
		cmd->forwardmove = FORWARDRUN;

	return true;
}

bool DBot::TryWalk (usercmd_t *cmd)
{
	// NewChaseDir has already collision-tested this eight-way direction. Make
	// the generated player command use that same world-space lane. The command
	// itself is projected after TurnToAng has produced next tic's yaw.
	const dirtype_t direction = (dirtype_t)player->mo->movedir;
	Angle = MoveDirectionAngle(direction);
	if (!Move (cmd))
	{
		return false;
	}

	pendingRecoveryMoveDirection = direction;

    player->mo->movecount = pr_bottrywalk() & 60;
    return true;
}

void DBot::NewChaseDir (usercmd_t *cmd, const DVector2 *goal)
{
    dirtype_t   d[3];

    int         tdir;
    dirtype_t   olddir;

    dirtype_t   turnaround;
	bool			attempted[DI_NODIR] = {};

    if (!dest)
	{
#ifndef BOT_RELEASE_COMPILE
        Printf ("Bot tried move without destination\n");
#endif
		return;
	}

    olddir = (dirtype_t)player->mo->movedir;
    turnaround = opposite[olddir];

	// The original Cajun copy retried the same blocked direction through the
	// direct, axis, and fallback branches. A tight corner can therefore consume
	// an entire reroute pass without ever testing a viable escape. Mirror the
	// engine monster chaser's one-probe-per-direction rule.
	auto TryDirection = [this, cmd, &attempted](dirtype_t direction)
	{
		if (direction >= DI_NODIR || attempted[direction])
		{
			return false;
		}
		// An actual movement failure is stronger evidence than the old random
		// movecount. Give that world-space lane a short cooling-off period so a
		// bot trapped by a prop or another player genuinely tries an alternate.
		if (t_blocked_direction > 0 && direction == blocked_move_direction)
		{
			return false;
		}
		attempted[direction] = true;
		player->mo->movedir = direction;
		return TryWalk(cmd);
	};

	DVector2 delta = goal != nullptr ? *goal - player->mo->Pos().XY() : player->mo->Vec2To(dest);

    if (delta.X > 10)
        d[1] = DI_EAST;
    else if (delta.X < -10)
        d[1] = DI_WEST;
    else
        d[1] = DI_NODIR;

    if (delta.Y < -10)
        d[2] = DI_SOUTH;
    else if (delta.Y > 10)
        d[2] = DI_NORTH;
    else
        d[2] = DI_NODIR;

    // try direct route
    if (d[1] != DI_NODIR && d[2] != DI_NODIR)
    {
		dirtype_t direct = diags[((delta.Y < 0) << 1) + (delta.X > 0)];
		if (direct != turnaround && TryDirection(direct))
            return;
    }

    // try other directions
	if (pr_botnewchasedir() > 200
		|| fabs(delta.Y) > fabs(delta.X))
	{
		tdir = d[1];
		d[1] = d[2];
		d[2] = (dirtype_t)tdir;
	}

    if (d[1]==turnaround)
        d[1]=DI_NODIR;
    if (d[2]==turnaround)
        d[2]=DI_NODIR;

    if (d[1]!=DI_NODIR)
    {
		if (TryDirection(d[1]))
            return;
    }

    if (d[2]!=DI_NODIR)
    {
		if (TryDirection(d[2]))
            return;
    }

    // there is no direct path to the player,
    // so pick another direction.
    if (olddir!=DI_NODIR)
    {
		if (TryDirection(olddir))
            return;
    }

    // randomly determine direction of search
    if (pr_botnewchasedir()&1)
    {
        for ( tdir=DI_EAST;
              tdir<=DI_SOUTHEAST;
              tdir++ )
        {
            if (tdir!=turnaround)
            {
				if (TryDirection((dirtype_t)tdir))
                    return;
            }
        }
    }
    else
    {
        for ( tdir=DI_SOUTHEAST;
              tdir != (DI_EAST-1);
              tdir-- )
        {
            if (tdir!=turnaround)
            {
				if (TryDirection((dirtype_t)tdir))
                    return;
            }
        }
    }

    if (turnaround !=  DI_NODIR)
    {
		if (TryDirection(turnaround))
            return;
    }

    player->mo->movedir = DI_NODIR;  // can not move
}


//
// B_CleanAhead
// Check if a place is ok to move towards.
// This is also a traverse function for
// bots pre-rocket fire (preventing suicide)
//
bool FCajunMaster::CleanAhead(AActor *thing, double x, double y, usercmd_t *,
	const DVector2 *plannedDescentGoal, double plannedDescentFloor,
	EBotMoveBlockReason *blockReason)
{
	FCheckPosition tm;
	const auto setBlockReason = [blockReason](EBotMoveBlockReason reason)
	{
		if (blockReason != nullptr)
		{
			*blockReason = reason;
		}
	};
	setBlockReason(EBotMoveBlockReason::None);

	if (!SafeCheckPosition(thing, x, y, tm))
	{
		setBlockReason(EBotMoveBlockReason::Geometry);
		return false; // solid wall or thing
	}

	if (!(thing->flags & MF_NOCLIP) )
	{
		if (tm.ceilingz - tm.floorz < thing->Height)
		{
			setBlockReason(EBotMoveBlockReason::Unsafe);
			return false;       // doesn't fit
		}

		if (!(thing->flags&MF_MISSILE))
		{
			sector_t *landingSector = tm.floorsector != nullptr ? tm.floorsector : tm.sector;
			if (IsDangerous(thing, landingSector, DVector2(x, y), tm.floorz))
			{
				setBlockReason(EBotMoveBlockReason::Unsafe);
				return false;
			}

			// Bot player movement has to obey the pawn's real step height. The old
			// code pressed jump for a high step and immediately rejected that exact
			// move, which was both misleading and incapable of completing a route.
			if (tm.floorz > thing->floorz + thing->MaxStepHeight)
			{
				setBlockReason(EBotMoveBlockReason::VerticalStep);
				return false;
			}

			if (!(thing->flags & MF_TELEPORT) && tm.ceilingz < thing->Top())
			{
				setBlockReason(EBotMoveBlockReason::Unsafe);
				return false; // mobj must lower itself to fit
			}

	        // jump out of water
//	        if((thing->eflags & (MF_UNDERWATER|MF_TOUCHWATER))==(MF_UNDERWATER|MF_TOUCHWATER))
//	            maxstep=37;

			if (!(thing->flags & MF_TELEPORT) &&
				(tm.floorz - thing->Z() > thing->MaxStepHeight))
			{
				setBlockReason(EBotMoveBlockReason::VerticalStep);
				return false; // too big a step up
			}

			// Preserve the conservative cliff rule for every raw movement command.
			// The sole exception is a bounded, collision-checked route that was
			// explicitly planned toward a lower player/waypoint; this is how player
			// companions can cross ordinary Doom ledges without making combat
			// strafing or FakeFire's trace accept a speculative drop.
			if (!(thing->flags & MF_FLOAT) &&
				tm.floorz - tm.dropoffz > thing->MaxDropOffHeight &&
				!IsPlannedDescent(thing, x, y, tm, plannedDescentGoal, plannedDescentFloor))
			{
				setBlockReason(EBotMoveBlockReason::Unsafe);
				return false;       // unplanned or unsafe dropoff
			}

		}
	}
	return true;
}

#define OKAYRANGE (5) //counts *2, when angle is in range, turning is not executed.
#define MAXTURN (15) //Max degrees turned in one tic. Lower is smother but may cause the bot not getting where it should = crash
#define TURNSENS 3 //Higher is smoother but slower turn.

void DBot::TurnToAng ()
{
    double maxturn = MAXTURN;

	if (player->ReadyWeapon != NULL)
	{
		if (GetBotInfo(player->ReadyWeapon).flags & BIF_BOT_EXPLOSIVE)
		{
			// Keep a just-fired projectile's presentation steady only while the
			// bot has no combat target. The old t_roam test froze all turning for
			// item/follow routes, including after a visible enemy appeared.
			if (t_rocket && !enemy && !missile)
			{
				return;
			}
		}


		// Deathmatch retains the old gentle aim turn for visual/player-versus-player
		// behaviour. A cooperative companion needs to face a newly visible hostile
		// promptly; attack validation still owns every actual firing safety check.
		if(deathmatch && enemy)
			if(!dest) //happens when running after item in combat situations, or normal, prevents weak turns
				if(GetBotInfo(player->ReadyWeapon).projectileType == NULL && GetBotInfo(player->ReadyWeapon).MoveCombatDist > 0)
					if(Check_LOS(enemy, DAngle::fromDeg(SHOOTFOV+5)))
						maxturn = 3;
	}

	DAngle distance = deltaangle(player->mo->Angles.Yaw, Angle);

	if (fabs (distance) < DAngle::fromDeg(OKAYRANGE) && !enemy)
		return;

	distance /= TURNSENS;
	if (fabs (distance) > DAngle::fromDeg(maxturn))
		distance = DAngle::fromDeg(distance < nullAngle ? -maxturn : maxturn);

	player->mo->Angles.Yaw += distance;
}

void DBot::Pitch (AActor *target)
{
	if (target == nullptr || player == nullptr || player->mo == nullptr)
	{
		return;
	}
	const double horizontalDistance = max(player->mo->Distance2D(target), 1.0);
	const double verticalDifference = target->Center() - player->mo->Center();
	player->mo->Angles.Pitch = DAngle::fromRad(g_atan(verticalDifference / horizontalDistance));
}

//Checks if a sector is dangerous.
bool FCajunMaster::IsDangerous (sector_t *sec)
{
	if (sec == nullptr)
	{
		return true;
	}
	const double floorz = sec->floorplane.ZatPoint(sec->centerspot);
	return sec->IsDangerous(DVector3(sec->centerspot, floorz), 56.0);
}

bool FCajunMaster::IsDangerous(AActor *actor, sector_t *sec, const DVector2 &position, double floorz)
{
	if (sec == nullptr)
	{
		return true;
	}
	const double height = actor != nullptr ? actor->Height : 56.0;
	return sec->IsDangerous(DVector3(position, floorz), height);
}
