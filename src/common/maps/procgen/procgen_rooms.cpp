/*
** procgen_rooms.cpp
**
** Coarse-cell room composition, visual coherence, encounter pacing, and
** resource progression for procedural maps.
**
**---------------------------------------------------------------------------
*/

#include "procgen_internal.h"
#include "gamedata/gi.h"

using namespace ProcGen;

static bool ContainsRoom(const TArray<int>& rooms, int room)
{
	for (unsigned int i = 0; i < rooms.Size(); i++)
		if (rooms[i] == room) return true;
	return false;
}

// These hashes are intentionally local to presentation planning. They never
// touch FRandom, so adding a new footprint/material choice cannot perturb the
// mission graph or later actor-placement draws.
static uint32_t MixRoomPlanHash(uint32_t value)
{
	value ^= value >> 16;
	value *= 0x7feb352du;
	value ^= value >> 15;
	value *= 0x846ca68bu;
	value ^= value >> 16;
	return value;
}

static uint32_t RoomPlanHash(uint32_t recipeHash, int first, int second, int salt)
{
	uint32_t value = recipeHash ^ (uint32_t)(first + 1) * 0x9e3779b9u;
	value ^= (uint32_t)(second + 1) * 0x85ebca6bu;
	value ^= (uint32_t)(salt + 1) * 0xc2b2ae35u;
	return MixRoomPlanHash(value);
}

static int MaterialFamilyPalette(EProcGenMaterialFamily family)
{
	if (family <= PGMF_None) return 0;
	// EProcGenMaterialFamily deliberately stores four variants per theme.
	return ((int)family - 1) % 4;
}

static EProcGenRoomFootprint PickRoomFootprint(ThemeStyle theme,
	EProcGenLandmarkArchetype landmark, bool critical, uint32_t choice)
{
	if (critical) return PGRF_SafeShell;
	if (landmark == PGLA_Court || landmark == PGLA_Fortress)
		return PGRF_CourtyardCut;
	if (landmark == PGLA_Nave || landmark == PGLA_ShrineTerrace)
		return PGRF_Apse;
	if (landmark == PGLA_Gatehouse || landmark == PGLA_BridgeBasin)
		return PGRF_TaperedBay;
	if (landmark == PGLA_Bastion) return PGRF_SteppedCompound;

	switch (theme)
	{
	case ThemeIndustrial:
		return (choice & 1u) != 0u ? PGRF_TaperedBay : PGRF_SteppedCompound;
	case ThemeHell:
		return (choice % 3u) == 0u ? PGRF_CourtyardCut : PGRF_FracturedWedge;
	case ThemeGothic:
		return (choice & 1u) != 0u ? PGRF_Apse : PGRF_TaperedBay;
	case ThemeCorrupted:
		return (choice & 1u) != 0u ? PGRF_FracturedWedge : PGRF_SteppedCompound;
	case ThemeTechbase:
	default:
		return (choice % 3u) == 0u ? PGRF_TaperedBay : PGRF_AsymmetricOctagon;
	}
}

static int FootprintShapeFamily(EProcGenRoomFootprint footprint, uint32_t choice)
{
	switch (footprint)
	{
	case PGRF_TaperedBay:
	case PGRF_Apse:
		return (choice & 1u) != 0u ? 1 : 2;
	case PGRF_SteppedCompound:
	case PGRF_CourtyardCut:
	case PGRF_FracturedWedge:
		return 3;
	default:
		return 0;
	}
}

void FProceduralMapGenerator::MergeRooms(int W, int H)
{
	Rooms.Clear();
	const RunBlueprint& blueprint = GetRunBlueprint();
	const ThemeStyle themeStyle = GetThemeStyle(Theme);
	for (int y = 0; y < H; y++)
		for (int x = 0; x < W; x++)
			Grid[y][x].roomId = -1;

	auto IsSpecial = [&](const ProcGenCell& cell) -> bool
	{
		return cell.hasPlayerStart || cell.hasExit || cell.hasBoss || cell.hasKey ||
			cell.isLocked || cell.reservedSecret ||
			cell.landmarkArchetype != PGLA_None || cell.verticalAnchor ||
			cell.terrainRouteReservation;
	};

	auto Compatible = [&](const ProcGenCell& seed, const ProcGenCell& candidate) -> bool
	{
		if (candidate.lockStage != seed.lockStage) return false;
		if (candidate.reservedSecret != seed.reservedSecret) return false;
		if (candidate.isLocked || seed.isLocked)
			return candidate.isLocked && seed.isLocked && candidate.lockType == seed.lockType;
		// A required-route elevation anchor is one exact chamber-to-chamber
		// connector, not a vague property of a merged landmark. Keep it atomic so
		// its source cannot later absorb a key pad, a door threshold, or a hub and
		// force the vertical plan to be discarded during coherence.
		if (candidate.verticalAnchor || seed.verticalAnchor ||
			candidate.terrainRouteReservation || seed.terrainRouteReservation)
			return false;

		if (IsSpecial(candidate) && !IsSpecial(seed)) return false;
		if (candidate.hasPlayerStart != seed.hasPlayerStart && candidate.hasPlayerStart) return false;
		if (candidate.hasExit != seed.hasExit && candidate.hasExit) return false;
		if (candidate.hasBoss != seed.hasBoss && candidate.hasBoss) return false;
		if (candidate.hasKey && (!seed.hasKey || candidate.keyType != seed.keyType)) return false;
		// Landmark anchors own a complete, readable silhouette.  Their support
		// cells may be absorbed, but two different authored identities must never
		// blur into one generic mega-room merely because their grid cells touch.
		if (candidate.landmarkArchetype != PGLA_None &&
			seed.landmarkArchetype != PGLA_None &&
			candidate.landmarkArchetype != seed.landmarkArchetype)
			return false;
		if (seed.hasKey)
			return candidate.pathRank == seed.pathRank && candidate.isArena;
		if (seed.hasExit || seed.hasBoss)
			return candidate.pathRank == seed.pathRank && candidate.isArena;
		if (seed.hasPlayerStart)
			return candidate.pathRank == seed.pathRank;

		if (abs(candidate.pathRank - seed.pathRank) > 1) return false;
		if (candidate.onMainPath != seed.onMainPath)
		{
			if (!(seed.isHub || seed.isArena) || candidate.pathRank != seed.pathRank)
				return false;
		}
		if (candidate.branchDepth > 0 && seed.branchDepth > 0 &&
			abs(candidate.branchDepth - seed.branchDepth) > 1)
			return false;
		if ((candidate.isArena != seed.isArena || candidate.isHub != seed.isHub) &&
			(candidate.isArena || seed.isArena || candidate.isHub || seed.isHub))
			return candidate.pathRank == seed.pathRank;
		return true;
	};

	auto TargetRoomSize = [&](const ProcGenCell& seed) -> int
	{
		const int combatGrowth = Difficulty - 1;
		if (seed.isLocked) return 1;
		const int landmarkGrowth = 2 + Size / 4;
		if (seed.landmarkArchetype != PGLA_None)
		{
			// These are intentionally footprint targets rather than decoration
			// quotas.  The room merger may stop early at a stage boundary, which is
			// the compact, safe fallback for tightly packed maps.
			switch ((EProcGenLandmarkArchetype)seed.landmarkArchetype)
			{
			case PGLA_Gatehouse: return 3 + combatGrowth + (RNG() % 2);
			case PGLA_ShrineTerrace: return 4 + landmarkGrowth / 2 + (RNG() % 3);
			case PGLA_BridgeBasin: return 4 + landmarkGrowth / 2 + (RNG() % 3);
			case PGLA_Nave: return 6 + landmarkGrowth + (RNG() % 4);
			case PGLA_Bastion: return 7 + landmarkGrowth + combatGrowth + (RNG() % 4);
			case PGLA_Fortress: return 8 + landmarkGrowth + combatGrowth * 2 + (RNG() % 5);
			case PGLA_Court: return 5 + landmarkGrowth + (RNG() % 4);
			default: break;
			}
		}
		if (seed.hasExit || seed.hasBoss)
			return 5 + landmarkGrowth + combatGrowth * 2 + (RNG() % (3 + Size / 8));
		if (seed.hasKey)
			return 3 + landmarkGrowth + combatGrowth + (RNG() % (2 + Size / 10));
		if (seed.hasPlayerStart) return 2 + landmarkGrowth + (RNG() % 3);
		if (seed.isArena)
			return 4 + landmarkGrowth + combatGrowth * 2 + (RNG() % (3 + Size / 8));
		if (seed.isHub) return 3 + landmarkGrowth + combatGrowth / 2 + (RNG() % 4);

		// Maintain three strong authored scales plus occasional dominant rooms.
		// The proportions mirror the broad rhythm measured in the Doom II IWAD:
		// connectors and intimate rooms, ordinary combat rooms, then landmarks
		// several times the median footprint.
		const int roll = RNG() % 100;
		if (seed.onMainPath)
		{
			if (roll < 20) return 1;
			if (roll < 45) return 2;
			if (roll < 78) return 3 + (RNG() % (3 + Size / 12));
			return 7 + (RNG() % (4 + Size / 10));
		}
		if (seed.branchDepth >= 2)
		{
			if (roll < 38) return 1;
			if (roll < 72) return 2;
			if (roll < 95) return 3 + (RNG() % (3 + Size / 16));
			return 6 + (RNG() % (3 + Size / 16));
		}
		if (roll < 30) return 1;
		if (roll < 60) return 2;
		if (roll < 90) return 3 + (RNG() % (3 + Size / 14));
		return 7 + (RNG() % (3 + Size / 12));
	};

	// Process important cells first so their surrounding landmark footprint is
	// claimed before ordinary route cells consume it.
	for (int priority = 0; priority < 2; priority++)
	{
		for (int y = 0; y < H; y++)
		{
			for (int x = 0; x < W; x++)
			{
				ProcGenCell& seed = Grid[y][x];
				if (!seed.present || seed.roomId >= 0) continue;
				if ((priority == 0) != IsSpecial(seed)) continue;

				const int target = TargetRoomSize(seed);
				RoomInfo room;
				room.id = Rooms.Size();
					room.minI = room.maxI = x;
					room.minJ = room.maxJ = y;
					room.cellCount = 0;
					const uint32_t footprintHash = RoomPlanHash(blueprint.RecipeHash,
						x, y, seed.pathRank + seed.lockStage * 17);
					const bool criticalFootprint = seed.hasPlayerStart || seed.hasKey ||
						seed.hasExit || seed.hasBoss || seed.isLocked || seed.verticalAnchor;
					room.footprint = PickRoomFootprint(themeStyle,
						(EProcGenLandmarkArchetype)seed.landmarkArchetype,
						criticalFootprint, footprintHash);
					room.footprintVariant = (int)((footprintHash >> 5) & 3u);
					room.contourInset = 16 + (int)((footprintHash >> 11) % 3u) * 8;
					room.spatialClass = target <= 1 ? 0 :
					(target <= 2 ? 1 : (target <= 6 ? 2 : 3));
				if (target <= 1) room.shapeFamily = 0;
				else if (seed.landmarkArchetype == PGLA_Nave ||
					seed.landmarkArchetype == PGLA_BridgeBasin)
					room.shapeFamily = ((seed.pathRank + seed.lockStage) & 1) ? 1 : 2;
				else if (seed.landmarkArchetype == PGLA_Gatehouse ||
					seed.landmarkArchetype == PGLA_ShrineTerrace)
					room.shapeFamily = 0;
				else if (seed.landmarkArchetype == PGLA_Court ||
					seed.landmarkArchetype == PGLA_Bastion ||
					seed.landmarkArchetype == PGLA_Fortress)
					room.shapeFamily = 3;
				else if (seed.isArena || seed.isHub || seed.hasExit || seed.hasBoss)
					room.shapeFamily = 3;
					else
					{
						const int familyRoll = RNG() % 100;
						room.shapeFamily = familyRoll < 24 ? 0 :
							(familyRoll < 49 ? 1 : (familyRoll < 74 ? 2 : 3));
					}
					// Preserve the existing RNG draw above, then let the recipe-only
					// footprint grammar bias the merger toward a compatible silhouette.
					// Critical pads deliberately keep their compact safe shell.
					if (!criticalFootprint && target > 1 &&
						room.footprint != PGRF_AsymmetricOctagon)
						room.shapeFamily = FootprintShapeFamily(
							(EProcGenRoomFootprint)room.footprint, footprintHash);
					Rooms.Push(room);
				const int roomId = Rooms.Size() - 1;

				TArray<std::pair<int, int>> cells;
				cells.Push(std::make_pair(x, y));
				seed.roomId = roomId;
				while ((int)cells.Size() < target)
				{
					int bestX = -1;
					int bestY = -1;
					int bestScore = -100000;
					for (unsigned int ci = 0; ci < cells.Size(); ci++)
					{
						const int cx = cells[ci].first;
						const int cy = cells[ci].second;
						for (int d = 0; d < 4; d++)
						{
							const int nx = cx + DX[d];
							const int ny = cy + DY[d];
							if (nx < 0 || nx >= W || ny < 0 || ny >= H) continue;
							ProcGenCell& candidate = Grid[ny][nx];
							if (!candidate.present || candidate.roomId >= 0 || !Compatible(seed, candidate)) continue;

							int minX = std::min(Rooms[roomId].minI, nx);
							int maxX = std::max(Rooms[roomId].maxI, nx);
							int minY = std::min(Rooms[roomId].minJ, ny);
							int maxY = std::max(Rooms[roomId].maxJ, ny);
							int width = maxX - minX + 1;
							int height = maxY - minY + 1;
							if (width > height * 4 || height > width * 4) continue;

							int sameNeighbors = 0;
							int linkedNeighbors = 0;
							for (int od = 0; od < 4; od++)
							{
								const int ox = nx + DX[od];
								const int oy = ny + DY[od];
								if (ox < 0 || ox >= W || oy < 0 || oy >= H) continue;
								if (Grid[oy][ox].roomId == roomId)
								{
									sameNeighbors++;
									if (candidate.conn[od]) linkedNeighbors++;
								}
							}

							int score = linkedNeighbors * 18;
							const int family = Rooms[roomId].shapeFamily;
							if (family == 0)
							{
								score += sameNeighbors * 26;
								score -= abs(width - height) * 4;
							}
							else if (family == 1)
							{
								// Long bays and galleries remain thin instead of filling every
								// neighboring cell into another rounded rectangle.
								score += (width - height) * 15;
								score += sameNeighbors == 1 ? 20 : -sameNeighbors * 8;
							}
							else if (family == 2)
							{
								score += (height - width) * 15;
								score += sameNeighbors == 1 ? 20 : -sameNeighbors * 8;
							}
							else
							{
								// Compound rooms seek a turn and then branch, producing L, T,
								// cross, and stepped footprints rather than solid cell blocks.
								score += sameNeighbors == 1 ? 24 : (sameNeighbors == 2 ? 4 : -20);
								if (width > 1 && height > 1) score += 22;
								score += abs(width - height) * 2;
							}
							if (candidate.pathRank == seed.pathRank) score += 18;
							if (candidate.onMainPath == seed.onMainPath) score += 8;
							score += RNG() % 11;
							if (score > bestScore)
							{
								bestScore = score;
								bestX = nx;
								bestY = ny;
							}
						}
					}

					if (bestX < 0) break;
					Grid[bestY][bestX].roomId = roomId;
					cells.Push(std::make_pair(bestX, bestY));
					Rooms[roomId].minI = std::min(Rooms[roomId].minI, bestX);
					Rooms[roomId].maxI = std::max(Rooms[roomId].maxI, bestX);
					Rooms[roomId].minJ = std::min(Rooms[roomId].minJ, bestY);
					Rooms[roomId].maxJ = std::max(Rooms[roomId].maxJ, bestY);
				}

				Rooms[roomId].cellCount = cells.Size();
				const int realizedClass = cells.Size() <= 1 ? 0 :
					(cells.Size() <= 2 ? 1 : (cells.Size() <= 6 ? 2 : 3));
				Rooms[roomId].spatialClass = IsSpecial(seed) &&
					(seed.isArena || seed.isHub || seed.hasExit || seed.hasBoss || seed.hasKey) ?
					std::max(2, realizedClass) : realizedClass;
			}
		}
	}

	// Cells belonging to one room form continuous floor space even when their
	// original mission-graph connection was only implicit landmark expansion.
	for (int y = 0; y < H; y++)
	{
		for (int x = 0; x < W; x++)
		{
			if (!Grid[y][x].present) continue;
			for (int d = 0; d < 4; d++)
			{
				const int nx = x + DX[d];
				const int ny = y + DY[d];
				if (nx < 0 || nx >= W || ny < 0 || ny >= H) continue;
				if (Grid[ny][nx].present && Grid[ny][nx].roomId == Grid[y][x].roomId)
				{
					Grid[y][x].conn[d] = true;
					Grid[ny][nx].conn[OPP[d]] = true;
				}
			}
		}
	}
}

void FProceduralMapGenerator::ApplyCoherence(int W, int H)
{
	TArray<TArray<int>> adjacency;
	adjacency.Resize(Rooms.Size());
	const RunBlueprint& blueprint = GetRunBlueprint();
	const ThemeStyle themeStyle = GetThemeStyle(Theme);
	// Room merging can absorb optional support cells into a main-route chamber.
	// Preserve the plan carried by the earliest actual route cell as that
	// room's player-facing beat/card; scan order and enum ordering are not
	// meaningful design priorities here.
	TArray<int> primaryPlanRank;
	TArray<int> primaryPlanIsMain;
	primaryPlanRank.Resize(Rooms.Size());
	primaryPlanIsMain.Resize(Rooms.Size());

	for (unsigned int ri = 0; ri < Rooms.Size(); ri++)
	{
		RoomInfo& room = Rooms[ri];
		room.hasPlayerStart = false;
		room.hasExit = false;
		room.hasBoss = false;
		room.hasKey = false;
		room.keyType = 0;
		room.isLocked = false;
		room.lockType = 0;
		room.onMainPath = false;
		room.isArena = false;
		room.isHub = false;
		room.branchDepth = 0;
		room.distFromStart = -1;
		room.progressionRank = 9999;
		room.lockStage = -1;
		room.enemyCount = 0;
		room.monsterTier = 1;
		room.isSecret = false;
		room.reservedSecret = false;
		room.hasDoor = false;
		room.hasWeapon = false;
		room.hasAmmo = false;
		room.ammoCount = 0;
		room.hasHealth = false;
		room.healthCount = 0;
		room.healthBonusCount = 0;
		room.hasArmor = false;
		room.powerups.Clear();
		room.runBeat = PGRB_None;
		room.encounterCard = PGEC_None;
		room.featureMotif = PGFM_None;
		room.featureMotifPriority = 0;
		room.district = 0;
		room.stageShape = PGSS_Spine;
		room.landmarkArchetype = PGLA_None;
		room.districtRole = PGDR_Entry;
		room.materialFamily = PGMF_None;
		room.footprint = PGRF_SafeShell;
		room.footprintVariant = 0;
		room.contourInset = 0;
		room.footprintFallback = false;
		room.elevationRole = PGER_Flat;
		room.elevationTarget = 0;
		room.optionalTerrainAnchor = false;
		room.terrainRouteReservation = false;
		room.verticalIntent = PGVI_Flat;
		room.verticalRise = 0;
		room.verticalAnchor = false;
		room.manualInteraction = PGMI_None;
		room.threatBudget = 0;
		room.recoveryBudget = 0;
		room.optionalArmory = false;
		room.arsenalTrack = (int)GetArsenalTrackKind();
		room.finaleCard = (int)GetFinaleCardKind();
		room.rewardPlan = PGRW_None;
		room.cardFeasible = false;
		room.cardGeometry = "";
		room.cardCapacity = 0;
		room.cardStaticEnemies = 0;
		room.cardManualActions = 0;
		primaryPlanRank[ri] = 999999;
		primaryPlanIsMain[ri] = 0;
	}

	int startRoom = -1;
	for (int y = 0; y < H; y++)
	{
		for (int x = 0; x < W; x++)
		{
			ProcGenCell& cell = Grid[y][x];
			if (!cell.present || cell.roomId < 0 || cell.roomId >= (int)Rooms.Size()) continue;
			RoomInfo& room = Rooms[cell.roomId];
			if (room.lockStage < 0) room.lockStage = cell.lockStage;
			room.hasPlayerStart = room.hasPlayerStart || cell.hasPlayerStart;
			room.hasExit = room.hasExit || cell.hasExit;
			room.hasBoss = room.hasBoss || cell.hasBoss;
			room.onMainPath = room.onMainPath || cell.onMainPath;
			room.isArena = room.isArena || cell.isArena;
			room.isHub = room.isHub || cell.isHub;
			room.reservedSecret = room.reservedSecret || cell.reservedSecret;
			room.terrainRouteReservation = room.terrainRouteReservation ||
				cell.terrainRouteReservation;
			room.branchDepth = std::max(room.branchDepth, cell.branchDepth);
			if (cell.pathRank >= 0) room.progressionRank = std::min(room.progressionRank, cell.pathRank);
			if (cell.hasKey)
			{
				room.hasKey = true;
				room.keyType = cell.keyType;
			}
			if (cell.isLocked)
			{
				room.isLocked = true;
				room.lockType = cell.lockType;
				room.manualInteraction = PGMI_KeyedDoor;
			}
			const int planRank = cell.pathRank >= 0 ? cell.pathRank : 999999;
			const int planIsMain = cell.onMainPath ? 1 : 0;
			const int roomId = cell.roomId;
			if ((!primaryPlanIsMain[roomId] && planIsMain) ||
				(primaryPlanIsMain[roomId] == planIsMain &&
				 planRank < primaryPlanRank[roomId]))
			{
				primaryPlanRank[roomId] = planRank;
				primaryPlanIsMain[roomId] = planIsMain;
				room.runBeat = cell.runBeat;
				room.encounterCard = cell.encounterCard;
					room.stageShape = cell.stageShape;
					room.landmarkArchetype = cell.landmarkArchetype;
					room.districtRole = cell.districtRole;
					room.materialFamily = cell.materialFamily;
					room.elevationRole = cell.elevationRole;
					room.elevationTarget = cell.elevationTarget;
					room.verticalIntent = cell.verticalIntent;
				room.verticalRise = cell.verticalRise;
				// The flat successor sentinel protects the other endpoint of a
				// planned stair during merging, but only the signed source is a
				// room-facing vertical anchor or manifest beat.
				room.verticalAnchor = cell.verticalAnchor && cell.verticalRise != 0;
			}
			// A supporting cell can carry a unique landmark anchor after a room
			// merger.  Prefer that authored form over an ordinary main-path cell;
			// it is the one case where semantic identity should outrank rank.
			if (cell.landmarkArchetype != PGLA_None &&
				(room.landmarkArchetype == PGLA_None || cell.verticalAnchor))
			{
				room.landmarkArchetype = cell.landmarkArchetype;
				room.districtRole = cell.districtRole;
			}
			if (cell.verticalAnchor && cell.verticalRise != 0 &&
				!room.hasPlayerStart && !room.hasKey &&
				!room.hasExit && !room.isLocked)
			{
				room.verticalIntent = cell.verticalIntent;
				room.verticalRise = cell.verticalRise;
				room.verticalAnchor = true;
			}
			if (cell.featureMotif != PGFM_None &&
				(cell.featureMotifPriority >= room.featureMotifPriority ||
				 room.featureMotif == PGFM_None))
			{
				room.featureMotif = cell.featureMotif;
				room.featureMotifPriority = cell.featureMotifPriority;
			}
			room.district = std::max(room.district, cell.district);
			room.threatBudget = std::max(room.threatBudget, cell.threatBudget);
			room.recoveryBudget = std::max(room.recoveryBudget, cell.recoveryBudget);
			room.optionalArmory = room.optionalArmory || cell.optionalArmory;
			room.arsenalTrack = cell.arsenalTrack;
			room.finaleCard = cell.finaleCard;
			room.rewardPlan = std::max(room.rewardPlan, cell.rewardPlan);
			if (cell.hasPlayerStart) startRoom = cell.roomId;

			for (int d = 0; d < 4; d++)
			{
				if (!cell.conn[d]) continue;
				const int nx = x + DX[d];
				const int ny = y + DY[d];
				if (nx < 0 || nx >= W || ny < 0 || ny >= H || !Grid[ny][nx].present) continue;
				const int other = Grid[ny][nx].roomId;
				if (other != cell.roomId && other >= 0 && !ContainsRoom(adjacency[cell.roomId], other))
					adjacency[cell.roomId].Push(other);
			}
		}
	}

	// A generated room can span supporting cells around a landmark.  Those
	// cells legitimately carry their own recovery or optional plans, but they
	// must never obscure the player-facing identity of the room that owns a
	// start, required key, or exit.  Restore these semantic anchors after the
	// cell aggregation rather than relying on enum order or merge scan order.
	for (unsigned int ri = 0; ri < Rooms.Size(); ++ri)
	{
		RoomInfo& room = Rooms[ri];
		if (room.hasPlayerStart)
		{
			room.runBeat = PGRB_Opening;
			room.encounterCard = PGEC_Breather;
			room.rewardPlan = PGRW_Emergency;
		}
		else if (room.hasExit)
		{
			room.runBeat = PGRB_Finale;
			room.encounterCard = PGEC_SetPiece;
			room.rewardPlan = PGRW_FinaleReserve;
		}
		else if (room.hasKey)
		{
			room.runBeat = PGRB_KeyObjective;
			room.encounterCard = PGEC_SetPiece;
			room.rewardPlan = PGRW_KeyReserve;
		}
		// Mandatory pads, keyed thresholds, and the exit are deliberately not
		// vertical anchors.  Their landmark form remains visible, but any planned
		// rise is realized on a neighboring ordinary route connector instead.
		if (room.hasPlayerStart || room.hasKey || room.hasExit || room.isLocked)
		{
			room.verticalIntent = PGVI_Flat;
			room.verticalRise = 0;
			room.verticalAnchor = false;
		}
		if (room.hasExit || room.hasBoss)
		{
			room.landmarkArchetype = PGLA_Fortress;
			room.districtRole = PGDR_Finale;
		}
		else if (room.isLocked)
		{
			room.landmarkArchetype = PGLA_Gatehouse;
			room.districtRole = PGDR_Defense;
		}
		else if (room.hasKey && room.landmarkArchetype == PGLA_None)
		{
			room.landmarkArchetype = PGLA_ShrineTerrace;
			room.districtRole = PGDR_Sanctum;
		}
		const int stage = clamp(room.lockStage, 0, RunBlueprint::MaxStages - 1);
		if (room.materialFamily == PGMF_None)
			room.materialFamily = blueprint.StageMaterialFamilies[stage];
		const uint32_t footprintHash = RoomPlanHash(blueprint.RecipeHash,
			room.minI * 257 + room.maxI, room.minJ * 257 + room.maxJ,
			room.id * 31 + room.lockStage * 7 + room.cellCount);
		const bool criticalFootprint = room.hasPlayerStart || room.hasKey ||
			room.hasExit || room.hasBoss || room.isLocked || room.verticalAnchor;
		room.footprint = PickRoomFootprint(themeStyle,
			(EProcGenLandmarkArchetype)room.landmarkArchetype, criticalFootprint,
			footprintHash);
		if (room.cellCount <= 1 && (room.footprint == PGRF_SteppedCompound ||
			room.footprint == PGRF_CourtyardCut || room.footprint == PGRF_FracturedWedge))
			room.footprint = PGRF_AsymmetricOctagon;
		room.footprintVariant = (int)((footprintHash >> 4) & 3u);
		room.contourInset = 16 + (int)((footprintHash >> 12) % 4u) * 8;
		room.footprintFallback = false;
		if (!criticalFootprint && room.cellCount > 1 &&
			room.footprint != PGRF_AsymmetricOctagon)
			room.shapeFamily = FootprintShapeFamily(
				(EProcGenRoomFootprint)room.footprint, footprintHash);
	}

	if (startRoom < 0 && Rooms.Size() > 0) startRoom = 0;
	TArray<int> queue;
	if (startRoom >= 0)
	{
		Rooms[startRoom].distFromStart = 0;
		queue.Push(startRoom);
	}
	for (unsigned int qi = 0; qi < queue.Size(); qi++)
	{
		const int roomId = queue[qi];
		for (unsigned int ai = 0; ai < adjacency[roomId].Size(); ai++)
		{
			const int other = adjacency[roomId][ai];
			if (Rooms[other].distFromStart >= 0) continue;
			Rooms[other].distFromStart = Rooms[roomId].distFromStart + 1;
			queue.Push(other);
		}
	}

	int maxDistance = 1;
	for (unsigned int ri = 0; ri < Rooms.Size(); ri++)
	{
		RoomInfo& room = Rooms[ri];
		if (room.progressionRank == 9999) room.progressionRank = 0;
		if (room.distFromStart < 0) room.distFromStart = room.progressionRank;
		maxDistance = std::max(maxDistance, room.distFromStart);
		room.isDeadEnd = adjacency[ri].Size() <= 1 && !room.hasPlayerStart && !room.hasExit;
		if (adjacency[ri].Size() >= 3 && room.onMainPath) room.isHub = true;
	}

	static const char* TechWallZones[4][6] = {
		{ "STARTAN3", "STARTAN2", "BROWN96", "BROWNGRN", "BROWN1", "STONE2" },
		{ "BROWN1", "BROWN96", "BROWNGRN", "STARTAN2", "STONE2", "STARTAN3" },
		{ "STONE2", "STONE3", "METAL1", "COMPSPAN", "BROWN96", "TEKWALL1" },
		{ "TEKWALL1", "TEKWALL4", "COMPSPAN", "METAL1", "STONE3", "STARTAN2" }
	};
	static const char* TechFloorZones[4][6] = {
		{ "FLOOR4_8", "FLOOR4_1", "FLOOR4_6", "FLOOR5_1", "FLOOR5_2", "FLAT1" },
		{ "FLOOR5_1", "FLOOR5_2", "FLAT1", "FLOOR4_6", "FLOOR0_1", "FLOOR4_8" },
		{ "FLOOR0_1", "FLAT14", "FLOOR4_1", "FLOOR5_2", "FLAT20", "FLOOR4_6" },
		{ "FLAT20", "FLOOR4_8", "FLAT14", "FLOOR0_1", "FLAT10", "FLOOR7_2" }
	};
	static const char* TechCeilZones[4][6] = {
		{ "CEIL3_5", "CEIL3_6", "CEIL5_1", "FLAT20", "CEIL5_2", "FLOOR0_1" },
		{ "FLAT20", "CEIL5_2", "CEIL3_5", "FLOOR0_1", "CEIL3_6", "FLAT14" },
		{ "CEIL5_1", "CEIL5_2", "FLAT14", "CEIL3_6", "FLAT20", "FLAT10" },
		{ "FLOOR7_2", "FLAT20", "CEIL5_1", "FLAT10", "CEIL5_2", "FLAT14" }
	};
	static const char* HellWallZones[4][6] = {
		{ "STONE2", "STONE3", "GSTONE1", "GSTONE2", "MARBLE1", "GSTVINE1" },
		{ "MARBLE1", "MARBLE2", "MARBLE3", "STONE3", "GSTONE2", "WOOD1" },
		{ "GSTVINE1", "GSTVINE2", "GSTONE1", "WOOD1", "MARBLE2", "STONE2" },
		{ "SP_HOT1", "GSTONE2", "MARBLE3", "WOOD1", "GSTVINE2", "MARBLE1" }
	};
	static const char* HellFloorZones[4][6] = {
		{ "FLOOR6_1", "FLOOR6_2", "FLAT5_1", "FLAT5_2", "FLOOR7_1", "FLAT8" },
		{ "FLAT5_1", "FLAT5_2", "FLOOR7_1", "FLOOR6_1", "FLOOR6_2", "FLAT10" },
		{ "FLOOR7_2", "FLOOR7_1", "FLAT8", "FLAT10", "FLAT5_2", "FLOOR6_2" },
		{ "FLOOR6_2", "FLAT8", "FLAT10", "FLOOR7_2", "FLAT5_1", "FLOOR7_1" }
	};
	static const char* HellCeilZones[4][6] = {
		{ "FLAT5_1", "FLAT5_2", "FLOOR6_1", "CEIL5_1", "FLAT8", "CEIL5_2" },
		{ "FLOOR7_2", "FLAT10", "FLAT5_2", "CEIL5_2", "FLAT8", "FLOOR6_2" },
		{ "FLAT10", "FLAT8", "FLOOR7_1", "CEIL5_1", "FLAT5_2", "FLOOR6_1" },
		{ "CEIL5_1", "FLAT10", "FLAT8", "FLOOR6_2", "FLOOR7_1", "FLAT5_1" }
	};
	static const char* IndustrialWallZones[4][6] = {
		{ "BROWN96", "BROWN1", "STARTAN2", "SUPPORT3", "METAL1", "BROWNGRN" },
		{ "METAL1", "BROWN96", "COMPSPAN", "SUPPORT3", "STONE2", "TEKWALL1" },
		{ "METAL1", "COMPSPAN", "TEKWALL1", "TEKWALL4", "STONE3", "BROWN1" },
		{ "TEKWALL4", "COMPSPAN", "METAL1", "STONE3", "TEKWALL1", "BROWN96" }
	};
	static const char* IndustrialFloorZones[4][6] = {
		{ "FLOOR5_1", "FLOOR5_2", "FLAT1", "FLOOR4_6", "FLOOR0_1", "FLOOR4_8" },
		{ "FLOOR0_1", "FLAT14", "FLOOR5_2", "FLOOR4_1", "FLAT20", "FLOOR4_6" },
		{ "FLAT20", "FLOOR0_1", "FLAT14", "FLOOR5_2", "FLAT10", "FLOOR7_2" },
		{ "FLAT20", "FLAT14", "FLOOR0_1", "FLOOR7_2", "FLAT10", "FLOOR4_8" }
	};
	static const char* IndustrialCeilZones[4][6] = {
		{ "CEIL5_1", "CEIL3_5", "CEIL5_2", "FLAT20", "CEIL3_6", "FLOOR0_1" },
		{ "FLAT20", "CEIL5_2", "CEIL3_5", "FLOOR0_1", "FLAT14", "CEIL3_6" },
		{ "CEIL5_2", "FLAT14", "FLAT20", "CEIL5_1", "FLOOR0_1", "FLAT10" },
		{ "FLAT20", "FLAT10", "CEIL5_1", "FLAT14", "CEIL5_2", "FLOOR7_2" }
	};
	static const char* GothicWallZones[4][6] = {
		{ "STONE2", "STONE3", "GSTONE1", "MARBLE1", "GSTONE2", "WOOD1" },
		{ "MARBLE1", "MARBLE2", "MARBLE3", "STONE3", "WOOD1", "GSTONE2" },
		{ "WOOD1", "GSTVINE1", "MARBLE2", "GSTONE1", "GSTVINE2", "MARBLE3" },
		{ "MARBLE3", "WOOD1", "SP_HOT1", "GSTVINE2", "GSTONE2", "MARBLE1" }
	};
	static const char* GothicFloorZones[4][6] = {
		{ "FLAT5_1", "FLOOR6_1", "FLAT5_2", "FLOOR6_2", "FLOOR7_1", "FLAT8" },
		{ "FLOOR7_2", "FLAT5_1", "FLAT8", "FLOOR6_1", "FLAT10", "FLAT5_2" },
		{ "FLOOR7_2", "FLAT8", "FLAT10", "FLOOR7_1", "FLAT5_2", "FLOOR6_2" },
		{ "FLAT10", "FLAT8", "FLOOR7_2", "FLOOR6_2", "FLAT5_1", "FLOOR7_1" }
	};
	static const char* GothicCeilZones[4][6] = {
		{ "FLAT5_1", "FLOOR6_1", "CEIL5_1", "FLAT5_2", "FLAT8", "CEIL5_2" },
		{ "FLOOR7_2", "FLAT10", "CEIL5_2", "FLAT8", "FLOOR6_2", "FLAT5_2" },
		{ "FLAT10", "FLAT8", "FLOOR7_1", "CEIL5_1", "FLOOR6_1", "FLAT5_2" },
		{ "CEIL5_1", "FLAT10", "FLAT8", "FLOOR6_2", "FLOOR7_1", "FLAT5_1" }
	};
	static const char* CorruptedWallZones[4][6] = {
		{ "STARTAN3", "COMPSPAN", "BROWN96", "STARTAN2", "TEKWALL1", "STONE2" },
		{ "COMPSPAN", "STONE3", "BROWNGRN", "GSTVINE1", "METAL1", "MARBLE1" },
		{ "GSTVINE1", "GSTONE1", "COMPSPAN", "SP_HOT1", "TEKWALL4", "MARBLE3" },
		{ "SP_HOT1", "GSTVINE2", "MARBLE3", "TEKWALL4", "GSTONE2", "WOOD1" }
	};
	static const char* CorruptedFloorZones[4][6] = {
		{ "FLOOR4_8", "FLOOR5_1", "FLAT1", "FLOOR4_6", "FLOOR0_1", "FLAT20" },
		{ "FLOOR0_1", "FLAT14", "FLOOR5_2", "FLAT5_1", "FLOOR7_1", "FLOOR4_1" },
		{ "FLAT5_1", "FLOOR7_2", "FLAT10", "FLOOR0_1", "FLAT8", "FLOOR6_2" },
		{ "FLAT8", "FLAT10", "FLOOR7_2", "FLAT5_2", "FLOOR6_2", "FLAT20" }
	};
	static const char* CorruptedCeilZones[4][6] = {
		{ "CEIL3_5", "FLAT20", "CEIL5_1", "CEIL3_6", "FLOOR0_1", "FLAT14" },
		{ "FLAT20", "CEIL5_2", "FLAT14", "FLAT5_1", "CEIL3_5", "FLOOR7_2" },
		{ "FLAT5_2", "FLAT10", "FLAT8", "CEIL5_1", "FLOOR7_1", "FLAT20" },
		{ "FLAT10", "FLAT8", "FLOOR6_2", "CEIL5_1", "FLOOR7_1", "FLAT5_1" }
	};
	static const char* TechAccents[] = {
		"SUPPORT2", "SUPPORT3", "METAL1", "COMPSPAN", "BROWN96", "TEKWALL4"
	};
	static const char* TechDetails[] = {
		"COMPSPAN", "TEKWALL1", "SUPPORT3", "STONE3", "BROWNGRN", "METAL1"
	};
	static const char* HellAccents[] = {
		"GSTVINE2", "GSTONE2", "MARBLE2", "WOOD1", "STONE3", "SP_HOT1"
	};
	static const char* HellDetails[] = {
		"MARBLE3", "GSTVINE1", "WOOD1", "GSTONE1", "STONE2", "GSTVINE2"
	};
	static const char* IndustrialAccents[] = {
		"METAL1", "SUPPORT3", "COMPSPAN", "TEKWALL4", "BROWN96", "SUPPORT2"
	};
	static const char* IndustrialDetails[] = {
		"COMPSPAN", "SUPPORT3", "TEKWALL1", "METAL1", "BROWNGRN", "STONE3"
	};
	static const char* GothicAccents[] = {
		"WOOD1", "MARBLE2", "GSTONE2", "MARBLE3", "GSTVINE2", "STONE3"
	};
	static const char* GothicDetails[] = {
		"MARBLE3", "WOOD1", "GSTVINE1", "GSTONE1", "MARBLE2", "GSTVINE2"
	};
	static const char* CorruptedAccents[4][6] = {
		{ "SUPPORT2", "COMPSPAN", "METAL1", "TEKWALL4", "BROWN96", "SUPPORT3" },
		{ "COMPSPAN", "GSTVINE1", "METAL1", "MARBLE2", "SUPPORT3", "STONE3" },
		{ "GSTVINE2", "COMPSPAN", "MARBLE2", "TEKWALL4", "GSTONE2", "METAL1" },
		{ "GSTVINE2", "MARBLE2", "SP_HOT1", "WOOD1", "TEKWALL4", "GSTONE2" }
	};
	static const char* CorruptedDetails[4][6] = {
		{ "COMPSPAN", "TEKWALL1", "SUPPORT3", "BROWNGRN", "METAL1", "STONE3" },
		{ "TEKWALL1", "GSTVINE1", "COMPSPAN", "STONE3", "MARBLE3", "BROWNGRN" },
		{ "GSTVINE1", "MARBLE3", "COMPSPAN", "GSTONE1", "TEKWALL4", "WOOD1" },
		{ "MARBLE3", "GSTVINE1", "WOOD1", "TEKWALL4", "GSTONE1", "GSTVINE2" }
	};
	static const double HalfProfiles[12][2] = {
		{ 88.0, 160.0 }, { 160.0, 88.0 }, { 104.0, 136.0 }, { 136.0, 104.0 },
		{ 120.0, 176.0 }, { 176.0, 120.0 }, { 136.0, 152.0 }, { 152.0, 136.0 },
		{ 144.0, 168.0 }, { 168.0, 144.0 }, { 160.0, 160.0 }, { 176.0, 176.0 }
	};
	static const double CornerProfiles[] = { 20.0, 28.0, 36.0, 44.0, 52.0 };
		TArray<int> roomClearHeights;
	roomClearHeights.Resize(Rooms.Size());

	for (unsigned int ri = 0; ri < Rooms.Size(); ri++)
	{
		RoomInfo& room = Rooms[ri];
		const int phase = clamp(room.distFromStart * 4 / (maxDistance + 1), 0, 3);
		int styleHash = abs(room.id * 37 + room.minI * 17 + room.maxJ * 29 +
			room.cellCount * 13 + room.progressionRank * 7 + room.branchDepth * 19);
		room.visualVariant = styleHash % countof(HalfProfiles);
		// A district's recipe-planned material family controls its shared wall
		// treatment. Optional deep branches may shift one related subpalette, but
		// ordinary neighboring rooms no longer march through a distance-only cycle.
		const EProcGenMaterialFamily materialFamily =
			(EProcGenMaterialFamily)room.materialFamily;
		const int materialPalette = MaterialFamilyPalette(materialFamily);
		bool infernalSurfaces = themeStyle == ThemeHell || themeStyle == ThemeGothic ||
			(themeStyle == ThemeCorrupted &&
				(materialFamily == PGMF_CorruptedBreachTerrace ||
				 materialFamily == PGMF_CorruptedHellCore || phase >= 2));
		int surfacePalette = materialPalette;
		if (!room.onMainPath && !room.hasKey && !room.hasExit && room.branchDepth >= 2)
			surfacePalette = (surfacePalette + 1) % 4;
		int familyShift = 0;
		if (themeStyle == ThemeIndustrial)
		{
			familyShift = 1;
		}
		else if (themeStyle == ThemeGothic)
		{
			familyShift = 2;
		}
		else if (themeStyle == ThemeCorrupted)
		{
			familyShift = infernalSurfaces ? 1 : 3;
		}
		int textureVariant = (materialPalette * 2 + room.lockStage + familyShift) % 6;
		if (room.branchDepth >= 2) textureVariant = (textureVariant + 1) % 6;
		if (room.isArena || room.isHub || room.hasKey || room.hasExit)
			textureVariant = (textureVariant + 2) % 6;
		// The contour grammar gives a bounded local variation without making a
		// district read as a random texture lottery.
		if (room.footprint == PGRF_Apse || room.footprint == PGRF_CourtyardCut)
			textureVariant = (textureVariant + 1) % 6;
		else if (room.footprint == PGRF_FracturedWedge)
			textureVariant = (textureVariant + 2) % 6;
		if (room.featureMotif == PGFM_ShrineSecrets)
			textureVariant = (textureVariant + 1) % 6;
		const int floorVariant = (textureVariant + room.cellCount + styleHash / 11) % 6;
		const int ceilingVariant = (textureVariant + 2 + styleHash / 17) % 6;
		const char* (*wallZones)[6] = infernalSurfaces ? HellWallZones : TechWallZones;
		const char* (*floorZones)[6] = infernalSurfaces ? HellFloorZones : TechFloorZones;
		const char* (*ceilZones)[6] = infernalSurfaces ? HellCeilZones : TechCeilZones;
		if (themeStyle == ThemeIndustrial)
		{
			wallZones = IndustrialWallZones;
			floorZones = IndustrialFloorZones;
			ceilZones = IndustrialCeilZones;
		}
		else if (themeStyle == ThemeGothic)
		{
			wallZones = GothicWallZones;
			floorZones = GothicFloorZones;
			ceilZones = GothicCeilZones;
		}
		else if (themeStyle == ThemeCorrupted)
		{
			wallZones = CorruptedWallZones;
			floorZones = CorruptedFloorZones;
			ceilZones = CorruptedCeilZones;
		}
		room.wallTex = wallZones[surfacePalette][textureVariant];
		room.floorTex = floorZones[surfacePalette][floorVariant];
		room.ceilTex = ceilZones[surfacePalette][ceilingVariant];
		if (themeStyle == ThemeIndustrial)
		{
			room.accentTex = IndustrialAccents[(textureVariant + surfacePalette) % countof(IndustrialAccents)];
			room.detailTex = IndustrialDetails[(textureVariant + room.visualVariant) % countof(IndustrialDetails)];
		}
		else if (themeStyle == ThemeGothic)
		{
			room.accentTex = GothicAccents[(textureVariant + surfacePalette) % countof(GothicAccents)];
			room.detailTex = GothicDetails[(textureVariant + room.visualVariant) % countof(GothicDetails)];
		}
		else if (themeStyle == ThemeCorrupted)
		{
			room.accentTex = CorruptedAccents[surfacePalette]
				[(textureVariant + surfacePalette) % 6];
			room.detailTex = CorruptedDetails[surfacePalette]
				[(textureVariant + room.visualVariant) % 6];
		}
		else
		{
			room.accentTex = infernalSurfaces ?
				HellAccents[(textureVariant + surfacePalette) % countof(HellAccents)] :
				TechAccents[(textureVariant + surfacePalette) % countof(TechAccents)];
			room.detailTex = infernalSurfaces ?
				HellDetails[(textureVariant + room.visualVariant) % countof(HellDetails)] :
				TechDetails[(textureVariant + room.visualVariant) % countof(TechDetails)];
		}

		room.halfWidth = HalfProfiles[room.visualVariant][0];
		room.halfHeight = HalfProfiles[room.visualVariant][1];
		if (room.spatialClass == 0)
		{
			// One-cell connectors are deliberately intimate and directional.
			if ((room.shapeFamily + room.visualVariant) & 1)
				room.halfWidth = std::min(room.halfWidth, 104.0);
			else
				room.halfHeight = std::min(room.halfHeight, 104.0);
		}
		else if (room.spatialClass == 1)
		{
			room.halfWidth = std::min(room.halfWidth, 152.0);
			room.halfHeight = std::min(room.halfHeight, 152.0);
		}
		else if (room.spatialClass == 3)
		{
			room.halfWidth = std::max(room.halfWidth, 160.0);
			room.halfHeight = std::max(room.halfHeight, 160.0);
		}
		int spanX = room.maxI - room.minI;
		int spanY = room.maxJ - room.minJ;
		if (spanX > spanY)
		{
			room.halfWidth = std::max(room.halfWidth, 168.0);
			room.halfHeight = std::min(room.halfHeight, 144.0);
		}
		else if (spanY > spanX)
		{
			room.halfWidth = std::min(room.halfWidth, 144.0);
			room.halfHeight = std::max(room.halfHeight, 168.0);
		}
		// Themes have architectural silhouettes, not just different wallpaper.
		// Industrial cells form long machine bays, Gothic cells read as broad
		// cathedral modules, Hell is irregular, and Corrupted Tech becomes more
		// distorted as its progression phase advances.
		if (themeStyle == ThemeIndustrial)
		{
			if (room.visualVariant & 1)
			{
				room.halfHeight = std::max(168.0, room.halfHeight);
				room.halfWidth = std::min(136.0, room.halfWidth);
			}
			else
			{
				room.halfWidth = std::max(168.0, room.halfWidth);
				room.halfHeight = std::min(136.0, room.halfHeight);
			}
		}
		else if (themeStyle == ThemeGothic)
		{
			// Cathedral modules alternate narrow aisles and broad transepts instead
			// of normalizing every room to the same square footprint.
			room.halfWidth = std::min(176.0, room.halfWidth + 8.0);
			room.halfHeight = std::min(176.0, room.halfHeight + 8.0);
		}
		else if (themeStyle == ThemeHell)
		{
			if (room.visualVariant & 1) room.halfWidth = std::min(184.0, room.halfWidth + 8.0);
			else room.halfHeight = std::min(184.0, room.halfHeight + 8.0);
		}
		else if (themeStyle == ThemeCorrupted && phase >= 2)
		{
			if (room.visualVariant & 1) room.halfWidth = std::max(104.0, room.halfWidth - 16.0);
			else room.halfHeight = std::max(104.0, room.halfHeight - 16.0);
		}
		// The footprint controls proportion as well as the contour that the UDMF
		// pass will emit. This keeps a tapered bay or apse from being merely an
		// octagon with a different texture family.
		switch ((EProcGenRoomFootprint)room.footprint)
		{
		case PGRF_TaperedBay:
			if (room.footprintVariant & 1)
			{
				room.halfWidth = std::max(160.0, room.halfWidth);
				room.halfHeight = std::min(136.0, room.halfHeight);
			}
			else
			{
				room.halfWidth = std::min(136.0, room.halfWidth);
				room.halfHeight = std::max(160.0, room.halfHeight);
			}
			break;
		case PGRF_Apse:
			if (room.footprintVariant & 1)
				room.halfWidth = std::max(168.0, room.halfWidth);
			else
				room.halfHeight = std::max(168.0, room.halfHeight);
			break;
		case PGRF_SteppedCompound:
			room.halfWidth = std::max(152.0, room.halfWidth);
			room.halfHeight = std::max(152.0, room.halfHeight);
			break;
		case PGRF_CourtyardCut:
			room.halfWidth = std::max(160.0, room.halfWidth);
			room.halfHeight = std::max(160.0, room.halfHeight);
			break;
		case PGRF_FracturedWedge:
			if (room.footprintVariant & 1)
			{
				room.halfWidth = std::max(156.0, room.halfWidth);
				room.halfHeight = std::min(144.0, room.halfHeight);
			}
			else
			{
				room.halfWidth = std::min(144.0, room.halfWidth);
				room.halfHeight = std::max(156.0, room.halfHeight);
			}
			break;
		default:
			break;
		}
		if (room.isArena || room.hasExit)
			room.halfWidth = room.halfHeight = 184.0;
		else if (room.isHub || room.hasKey)
		{
			room.halfWidth = std::max(room.halfWidth, 176.0);
			room.halfHeight = std::max(room.halfHeight, 168.0);
		}
		if (room.hasPlayerStart)
			room.halfWidth = room.halfHeight = 176.0;
		if (room.isLocked)
			room.halfWidth = room.halfHeight = 160.0;
		// Leave a physical connector bay between adjacent coarse cells. Two
		// 176-unit chambers leave 32 units between their faces: enough for the
		// 16-unit moving-door slab and an eight-unit recessed approach/lintel on
		// each side. Reserving this before room features are sized keeps doors
		// from stealing space later from stairs, reveals, or landmark clearances.
		// The serialized module cadence includes occasional 368-unit gaps. A
		// 168-unit chamber contract on both sides preserves a 32-unit connector
		// bay: an eight-unit approach, 16-unit door slab, and second approach.
		room.halfWidth = std::min(room.halfWidth, 168.0);
		room.halfHeight = std::min(room.halfHeight, 168.0);
		room.cornerCut = CornerProfiles[(styleHash / 5) % countof(CornerProfiles)];
		if (room.footprint != PGRF_SafeShell)
			room.cornerCut = std::max(room.cornerCut, (double)room.contourInset);
		if (Detail == 0) room.cornerCut = std::min(room.cornerCut, 28.0);
		else if (Detail == 2) room.cornerCut += 8.0;
		if (themeStyle == ThemeTechbase) room.cornerCut = std::min(room.cornerCut, 36.0);
		else if (themeStyle == ThemeIndustrial) room.cornerCut = std::min(room.cornerCut, 28.0);
		else if (themeStyle == ThemeGothic) room.cornerCut = std::min(room.cornerCut, 24.0);
		else if (themeStyle == ThemeHell) room.cornerCut = std::max(room.cornerCut, 36.0);
		else if (themeStyle == ThemeCorrupted)
			room.cornerCut = phase >= 2 ? std::max(room.cornerCut, 36.0) :
				std::min(room.cornerCut, 28.0);
		if (room.isArena || room.isHub || room.hasKey || room.hasBoss)
			room.cornerCut = std::min(room.cornerCut, 16.0);
		room.cornerCut = std::min(room.cornerCut,
			std::max(0.0, std::min(room.halfWidth, room.halfHeight) - 56.0));

		// The actual height is assigned by the graph terrain field after every
		// room has a final role and adjacency. Starting from zero here prevents a
		// distance-modulo cadence from leaking back into a constrained fallback.
		room.floorZ = 0;

		int clearHeight = 160 + (room.visualVariant % 4) * 16;
		if (room.spatialClass == 0) clearHeight = 128 + (room.visualVariant % 3) * 16;
		else if (room.spatialClass == 3) clearHeight += 32;
		if (room.cellCount == 1 && !room.hasKey && !room.hasExit)
			clearHeight = 144 + (room.visualVariant % 3) * 16;
		if (room.isHub) clearHeight = 192 + (room.visualVariant % 3) * 16;
		if (room.isArena) clearHeight = 240 + (room.visualVariant % 3) * 16;
		if (room.hasExit || room.hasBoss) clearHeight = 288 + (room.visualVariant % 3) * 16;
		if (themeStyle == ThemeTechbase && !room.isArena && !room.isHub)
			clearHeight = std::max(144, clearHeight - 16);
		else if (themeStyle == ThemeHell)
			clearHeight += room.isArena || room.hasExit ? 32 : 16;
		else if (themeStyle == ThemeIndustrial)
			clearHeight += (room.visualVariant & 1) ? 32 : 0;
		else if (themeStyle == ThemeGothic)
			clearHeight += room.isArena || room.isHub || room.hasExit ? 64 : 48;
		else if (themeStyle == ThemeCorrupted)
			clearHeight += phase * 16;
		roomClearHeights[ri] = clearHeight;

		room.light = 192 - phase * 8;
		if (!room.onMainPath) room.light -= 8;
		if (room.branchDepth >= 2) room.light -= 8;
		if (room.isArena || room.isHub) room.light += 8;
		if (room.hasPlayerStart || room.hasKey || room.hasExit) room.light += 8;
		if (themeStyle == ThemeTechbase) room.light += 8;
		else if (themeStyle == ThemeHell) room.light -= (styleHash & 1) ? 8 : 0;
		else if (themeStyle == ThemeIndustrial) room.light -= 8;
		else if (themeStyle == ThemeGothic)
			room.light += (room.isArena || room.hasKey || room.hasExit) ? 8 : -16;
		else if (themeStyle == ThemeCorrupted) room.light -= phase * 8;
		room.light = clamp((room.light / 8) * 8, 160, 208);
		if (room.district == 1) room.light = std::min(208, room.light + 8);
		else if (room.district >= 2) room.light = std::max(160, room.light - 8);
		static const int TechLightColors[] = { 0xe8f2ff, 0xdcecff, 0xd4e6ff, 0xc8dcf4 };
		static const int HellLightColors[] = { 0xffddc8, 0xffc4a8, 0xffa080, 0xff8068 };
		static const int IndustrialLightColors[] = { 0xf0ead8, 0xe6dcc4, 0xdcd0b4, 0xd4c6a8 };
		static const int GothicLightColors[] = { 0xe4e0ff, 0xd8d0f4, 0xc8c0e8, 0xb8acd8 };
		static const int CorruptedLightColors[] = { 0xe4f0ff, 0xd0d8e8, 0xe8b098, 0xff8068 };
		const int* lightColors = TechLightColors;
		if (themeStyle == ThemeHell) lightColors = HellLightColors;
		else if (themeStyle == ThemeIndustrial) lightColors = IndustrialLightColors;
		else if (themeStyle == ThemeGothic) lightColors = GothicLightColors;
		else if (themeStyle == ThemeCorrupted) lightColors = CorruptedLightColors;
		room.lightColor = lightColors[(phase + materialPalette) % countof(TechLightColors)];
		room.fadeColor = themeStyle == ThemeHell ? 0x100000 :
			(themeStyle == ThemeGothic ? 0x080810 :
			(themeStyle == ThemeIndustrial ? 0x080704 :
			(themeStyle == ThemeCorrupted && phase >= 2 ? 0x100000 : 0x04080c)));

		if (room.hasPlayerStart)
		{
			if (themeStyle == ThemeHell)
			{
				room.wallTex = "STONE2"; room.floorTex = "FLOOR6_1";
				room.accentTex = "GSTONE2"; room.detailTex = "GSTVINE1";
			}
			else if (themeStyle == ThemeGothic)
			{
				room.wallTex = "MARBLE1"; room.floorTex = "FLAT5_1";
				room.accentTex = "WOOD1"; room.detailTex = "MARBLE3";
			}
			else if (themeStyle == ThemeIndustrial)
			{
				room.wallTex = "BROWN96"; room.floorTex = "FLOOR5_1";
				room.accentTex = "METAL1"; room.detailTex = "COMPSPAN";
			}
			else
			{
				room.wallTex = "STARTAN3"; room.floorTex = "FLOOR4_8";
				room.accentTex = "SUPPORT2"; room.detailTex = "COMPSPAN";
			}
			room.floorZ = 0;
			roomClearHeights[ri] = 192;
			room.enemyCount = 0;
			room.monsterTier = 1;
		}
		else
		{
			// The old monotonic phase formula is now a safety baseline. Blueprint
			// cards add a readable combat question while the tier/footprint caps keep
			// the result fair for stock IWAD monsters.
			int pressure = (Difficulty - 1) / 2 + (Difficulty >= 4 ? 1 : 0);
			const int landmarkPressure = Difficulty / 2 + (Difficulty >= 5 ? 1 : 0);
			if (Difficulty == 2 && ((room.id + phase) % 4) == 0) pressure++;
			if (phase >= 2) pressure++;
			if (Difficulty >= 5 && room.onMainPath && phase > 0) pressure++;
			if (room.branchDepth >= 2) pressure--;
			pressure += room.threatBudget + blueprint.ThreatCurve;

			int cardFloor = 1;
			int cardBonus = 0;
			int cardCap = 3;
			switch ((EProcGenEncounterCard)room.encounterCard)
			{
			case PGEC_Breather:
				cardFloor = 0; cardCap = 0; break;
			case PGEC_Skirmish:
				cardFloor = 1; cardCap = 2; break;
			case PGEC_Crossfire:
				cardFloor = 2; cardBonus = 1; cardCap = 4; break;
			case PGEC_Pincer:
				cardFloor = 2; cardBonus = 1; cardCap = 4; break;
			case PGEC_Ambush:
				cardFloor = 2; cardCap = 4; break;
			case PGEC_CacheChallenge:
				cardFloor = 1; cardBonus = 1; cardCap = 3; break;
			case PGEC_HoldingLine:
				cardFloor = 3; cardBonus = 1; cardCap = 4; break;
			case PGEC_SetPiece:
				cardFloor = 3; cardBonus = 2; cardCap = 5; break;
			default:
				break;
			}
			if (room.hasKey && cardFloor == 0)
			{
				// A mandatory key shrine still needs a small authored confrontation.
				cardFloor = 2;
				cardCap = 4;
				room.encounterCard = PGEC_Skirmish;
			}
			room.enemyCount = cardFloor == 0 ? 0 :
				clamp(pressure + cardBonus + (int)(RNG() % 2), cardFloor, cardCap);
			if (room.isDeadEnd && !room.hasKey)
				room.enemyCount = std::min(room.enemyCount, 1 + Difficulty / 2 +
					(room.encounterCard == PGEC_CacheChallenge ? 1 : 0));
			if (room.isHub) room.enemyCount = clamp(std::max(cardFloor,
				1 + landmarkPressure + phase / 2 + cardBonus), cardFloor, std::max(cardCap, 4));
			if (room.isArena) room.enemyCount = clamp(std::max(cardFloor,
				1 + landmarkPressure + phase / 2 + room.cellCount / 6 + cardBonus),
				cardFloor, std::max(cardCap, 5));
			if (room.hasKey) room.enemyCount = clamp(std::max(cardFloor,
				1 + landmarkPressure + phase / 2 + room.cellCount / 6 + cardBonus),
				cardFloor, std::max(cardCap, 5));
			if (room.isLocked) room.enemyCount = clamp(std::max(cardFloor,
				1 + Difficulty / 3 + (Difficulty >= 5 ? 1 : 0) + phase / 2),
				cardFloor, std::max(cardCap, 4));
			if (room.hasExit) room.enemyCount = clamp(std::max(cardFloor,
				1 + landmarkPressure + Size / 6 + room.cellCount / 8 + cardBonus),
				cardFloor, 5);
			if (room.hasBoss)
				room.enemyCount = std::min(room.enemyCount, std::max(1, Difficulty - 2) +
					(room.finaleCard == PGFC_Siege ? 1 : 0));
			if (room.distFromStart == 1 && !room.isArena && !room.hasKey && !room.isLocked)
				room.enemyCount = std::min(room.enemyCount, 2);
			room.monsterTier = clamp(1 + phase + (Difficulty >= 4 ? 1 : 0) +
				(room.hasBoss && Difficulty >= 5 ? 1 : 0) +
				(room.encounterCard == PGEC_HoldingLine ? 1 : 0), 1, 5);
			if (room.cellCount <= 1) room.monsterTier = std::min(room.monsterTier, 2);
			else if (room.cellCount <= 2) room.monsterTier = std::min(room.monsterTier, 3);
		}

		// Doors punctuate a route. Locks are handled per-edge by BuildUDMF; only
		// selected rewards, deep branches, and transitions request normal doors.
		room.hasDoor = false;
		if (room.hasKey) room.hasDoor = true;
		else if (room.encounterCard == PGEC_Pincer || room.encounterCard == PGEC_Ambush)
			room.hasDoor = true;
		else if (room.isDeadEnd && room.branchDepth >= 2) room.hasDoor = (RNG() % 100) < 55;
		else if (!room.onMainPath && room.branchDepth >= 2) room.hasDoor = (RNG() % 100) < 25;
		else if (room.onMainPath && phase >= 2 && !room.isHub && !room.isArena)
			room.hasDoor = (RNG() % 100) < 18;
		// Keep normal-door rolls in the shared stream so an added terrain
		// reservation does not perturb later actor/reward placement. The route is
		// emitted open afterwards, leaving every reserved edge available to become
		// a real stair terrace rather than a door-and-ledge threshold.
		if (room.terrainRouteReservation) room.hasDoor = false;
	}

	// Seed floors from a recipe-planned terrain field over the realized room
	// graph. This deliberately replaces the former `distance % cadence` wave:
	// a dramatic map has one readable highland or basin, while branches acquire
	// locally varied terraces from an already connected parent. The later
	// component projection is still the safety authority for every <=64 step.
	auto QuantizeToEight = [](int value) -> int
	{
		return value >= 0 ? ((value + 4) / 8) * 8 : ((value - 4) / 8) * 8;
	};
	const int terrainLimit = Verticality == 0 ? 96 : (Verticality == 1 ? 160 : 320);
	const int branchStep = Verticality == 0 ? 16 : (Verticality == 1 ? 32 : 48);
	int maxMainRank = 1;
	int terrainPeakRank = -1;
	for (unsigned int ri = 0; ri < Rooms.Size(); ++ri)
	{
		const RoomInfo& room = Rooms[ri];
		if (!room.onMainPath) continue;
		maxMainRank = std::max(maxMainRank, std::max(0, room.progressionRank));
		if ((room.elevationRole == PGER_Highland || room.elevationRole == PGER_Basin) &&
			terrainPeakRank < 0)
			terrainPeakRank = std::max(0, room.progressionRank);
	}
	if (terrainPeakRank < 0)
		terrainPeakRank = clamp(maxMainRank / 2, 1, maxMainRank);

	TArray<bool> elevationAssigned;
	elevationAssigned.Resize(Rooms.Size());
	for (unsigned int ri = 0; ri < Rooms.Size(); ++ri)
	{
		RoomInfo& room = Rooms[ri];
		const bool criticalTerrace = room.hasPlayerStart || room.hasKey || room.hasExit ||
			room.hasBoss || room.isLocked;
		if (!room.onMainPath && !criticalTerrace)
		{
			elevationAssigned[ri] = false;
			continue;
		}
		int target = 0;
		if (!criticalTerrace && blueprint.MainRouteElevationTarget != 0)
		{
			const int rank = clamp(room.progressionRank, 0, maxMainRank);
			if (rank <= terrainPeakRank)
				target = blueprint.MainRouteElevationTarget * rank /
					std::max(1, terrainPeakRank);
			else
				target = blueprint.MainRouteElevationTarget * (maxMainRank - rank) /
					std::max(1, maxMainRank - terrainPeakRank);
		}
		room.elevationTarget = clamp(QuantizeToEight(target), -terrainLimit, terrainLimit);
		room.floorZ = room.elevationTarget;
		if (criticalTerrace)
			room.elevationRole = PGER_Flat;
		else if (abs(room.elevationTarget) >= 96)
			room.elevationRole = room.elevationTarget > 0 ? PGER_Highland : PGER_Basin;
		else if (room.elevationTarget != 0)
			room.elevationRole = PGER_Terrace;
		elevationAssigned[ri] = true;
	}

	// Choose one deep, reachable optional district as the opposite dramatic
	// horizon. It remains an intention, not a failure condition: if its graph
	// path is too short, the safety projection below trims it to legal stairs.
	int optionalExtremeRoom = -1;
	if (blueprint.OptionalElevationTarget != 0)
	{
		uint32_t optionalScore = UINT32_MAX;
		const int requiredDistance = std::max(3,
			abs(blueprint.OptionalElevationTarget) / std::max(16, branchStep));
		for (unsigned int ri = 0; ri < Rooms.Size(); ++ri)
		{
			const RoomInfo& room = Rooms[ri];
			if (room.onMainPath || room.hasPlayerStart || room.hasKey || room.hasExit ||
				room.hasBoss || room.isLocked || room.branchDepth < 2 ||
				room.distFromStart < requiredDistance)
				continue;
			const uint32_t score = RoomPlanHash(blueprint.RecipeHash, room.id,
				room.progressionRank, room.branchDepth + room.lockStage * 13);
			if (score < optionalScore)
			{
				optionalScore = score;
				optionalExtremeRoom = (int)ri;
			}
		}
	}

	for (int pass = 0; pass < (int)Rooms.Size(); ++pass)
	{
		bool changed = false;
		for (unsigned int ri = 0; ri < Rooms.Size(); ++ri)
		{
			if (elevationAssigned[ri]) continue;
			RoomInfo& room = Rooms[ri];
			int parent = -1;
			for (unsigned int ai = 0; ai < adjacency[ri].Size(); ++ai)
			{
				const int other = adjacency[ri][ai];
				if (!elevationAssigned[other]) continue;
				if (parent < 0 || Rooms[other].distFromStart < Rooms[parent].distFromStart ||
					(Rooms[other].distFromStart == Rooms[parent].distFromStart && other < parent))
					parent = other;
			}
			if (parent < 0) continue;
			const uint32_t hash = RoomPlanHash(blueprint.RecipeHash, room.id, parent,
				room.branchDepth * 19 + room.lockStage);
			const int direction = (hash & 1u) != 0u ? 1 : -1;
			int delta = direction * (room.branchDepth >= 2 ? branchStep : 16);
			if (room.featureMotif == PGFM_VerticalPressure ||
				room.landmarkArchetype == PGLA_BridgeBasin)
				delta += direction * 16;
			room.elevationTarget = clamp(QuantizeToEight(
				Rooms[parent].elevationTarget + delta), -terrainLimit, terrainLimit);
			room.floorZ = room.elevationTarget;
			room.elevationRole = room.elevationTarget >= 64 ? PGER_Highland :
				(room.elevationTarget <= -64 ? PGER_Basin : PGER_Terrace);
			elevationAssigned[ri] = true;
			changed = true;
		}
		if (!changed) break;
	}
	for (unsigned int ri = 0; ri < Rooms.Size(); ++ri)
	{
		if (elevationAssigned[ri]) continue;
		Rooms[ri].elevationTarget = 0;
		Rooms[ri].floorZ = 0;
		Rooms[ri].elevationRole = PGER_Flat;
	}
	if (optionalExtremeRoom >= 0)
	{
		RoomInfo& optional = Rooms[optionalExtremeRoom];
		optional.elevationTarget = clamp(blueprint.OptionalElevationTarget,
			-terrainLimit, terrainLimit);
		optional.floorZ = optional.elevationTarget;
		optional.elevationRole = optional.elevationTarget > 0 ?
			PGER_Highland : PGER_Basin;
	}

	// Bind each cell-planned elevation anchor to the following required-route
	// room. Room merging may have changed IDs, so resolve this against the room
	// graph rather than assuming an anchor's next grid cell survived unchanged.
	// The core planner never nominates a pad, key, gate, or exit; if composition
	// cannot retain that promise we drop the beat rather than putting stairs in a
	// door threshold.
	struct VerticalRouteConstraint
	{
		int from = -1;
		int to = -1;
		int rise = 0;
		// Keep the actual cell edge that earned this beat.  Room composition and
		// level-terrace projection are allowed to reject the relation later; in
		// that case the stale stair-chain marker must be removed rather than
		// leaving a zero-rise connector advertised as a dogleg.
		int sourceX = -1;
		int sourceY = -1;
		int direction = -1;
		int stage = -1;
		EProcGenVerticalIntent intent = PGVI_Flat;
	};
	TArray<VerticalRouteConstraint> verticalConstraints;
	for (unsigned int ri = 0; ri < Rooms.Size(); ++ri)
	{
		RoomInfo& from = Rooms[ri];
		if (!from.verticalAnchor || !from.onMainPath || from.verticalRise == 0 ||
			from.hasPlayerStart || from.hasKey || from.hasExit || from.isLocked)
			continue;
		int nextRoom = -1;
		int nextX = -1;
		int nextY = -1;
		int nextDirection = -1;
		// The anchor cell and its next main-path cell are both protected from
		// merging, but the *next* cell can legitimately belong to a composed room
		// whose earliest progression rank is earlier than this edge. Looking only
		// at RoomInfo::progressionRank then drops a perfectly valid planned dogleg
		// after room composition. Rebind through the original consecutive cell pair
		// first; its route rank and stage are the actual contract the core planner
		// proved before merging.
		for (int y = 0; y < H; ++y)
		{
			for (int x = 0; x < W; ++x)
			{
				const ProcGenCell& anchor = Grid[y][x];
				if (!anchor.present || anchor.roomId != (int)ri ||
					!anchor.verticalAnchor || !anchor.onMainPath)
					continue;
				for (int direction = 0; direction < 4; ++direction)
				{
					if (!anchor.conn[direction]) continue;
					const int nx = x + DX[direction];
					const int ny = y + DY[direction];
					if (nx < 0 || nx >= W || ny < 0 || ny >= H) continue;
					const ProcGenCell& next = Grid[ny][nx];
					if (!next.present || !next.onMainPath ||
						next.pathRank != anchor.pathRank + 1 ||
						next.lockStage != anchor.lockStage ||
						next.roomId < 0 || next.roomId >= (int)Rooms.Size())
						continue;
					RoomInfo& to = Rooms[next.roomId];
					if (to.hasPlayerStart || to.hasKey || to.hasExit || to.isLocked)
						continue;
					if (nextRoom < 0 || next.roomId < nextRoom ||
						(next.roomId == nextRoom &&
							(y < nextY || (y == nextY &&
								(x < nextX || (x == nextX && direction < nextDirection))))))
					{
						nextRoom = next.roomId;
						nextX = x;
						nextY = y;
						nextDirection = direction;
					}
				}
			}
		}
		// Retain the graph-level search as a compact fallback for any unusual
		// composed layout whose original cell relation could not be retained.
		if (nextRoom < 0)
		{
			for (unsigned int ai = 0; ai < adjacency[ri].Size(); ++ai)
			{
				const int otherId = adjacency[ri][ai];
				RoomInfo& to = Rooms[otherId];
				if (!to.onMainPath || to.lockStage != from.lockStage ||
					to.progressionRank != from.progressionRank + 1 ||
					to.hasPlayerStart || to.hasKey || to.hasExit || to.isLocked)
					continue;
				if (nextRoom < 0 || otherId < nextRoom) nextRoom = otherId;
			}
		}
		if (nextRoom < 0)
		{
			from.verticalAnchor = false;
			from.verticalIntent = PGVI_Flat;
			from.verticalRise = 0;
			continue;
		}
		RoomInfo& to = Rooms[nextRoom];
		// A normal door is just as unsuitable as a keyed door for a mandatory
		// elevation transition. Keep both rooms open so BuildUDMF emits a wide
		// eight-unit stair connector instead of a door-and-ledge combination.
		from.hasDoor = false;
		to.hasDoor = false;
		verticalConstraints.Push({ (int)ri, nextRoom,
			clamp(from.verticalRise, -64, 64), nextX, nextY, nextDirection,
			from.lockStage, (EProcGenVerticalIntent)from.verticalIntent });
	}

	// A real door cannot also serve as a staircase. Collapse components that
	// require a door (including ordinary planned doors), leaving only the
	// explicitly selected route edges free to become visible height transitions.
	TArray<int> floorParent;
	floorParent.Resize(Rooms.Size());
	for (unsigned int ri = 0; ri < Rooms.Size(); ri++) floorParent[ri] = ri;
	auto FindFloorRoot = [&](int roomId) -> int
	{
		int root = roomId;
		while (floorParent[root] != root) root = floorParent[root];
		while (floorParent[roomId] != roomId)
		{
			const int next = floorParent[roomId];
			floorParent[roomId] = root;
			roomId = next;
		}
		return root;
	};
	auto JoinFloorComponents = [&](int first, int second)
	{
		const int firstRoot = FindFloorRoot(first);
		const int secondRoot = FindFloorRoot(second);
		if (firstRoot != secondRoot) floorParent[secondRoot] = firstRoot;
	};
	for (unsigned int ri = 0; ri < Rooms.Size(); ri++)
	{
		for (unsigned int ai = 0; ai < adjacency[ri].Size(); ai++)
		{
			const int other = adjacency[ri][ai];
			const RoomInfo& first = Rooms[ri];
			const RoomInfo& second = Rooms[other];
			// Planned vertical endpoints cleared their normal-door request above.
			// Every remaining door or critical pad now becomes one level terrace;
			// this makes the opening contract explicit instead of relying on the
			// emitter to quietly discard an impossible door-and-ledge combination.
			const bool criticalTerrace = first.lockStage != second.lockStage ||
				first.hasPlayerStart || second.hasPlayerStart ||
				first.hasKey || second.hasKey || first.hasExit || second.hasExit ||
				first.hasBoss || second.hasBoss || first.isLocked || second.isLocked;
			const bool normalDoor = (first.hasDoor || second.hasDoor) &&
				!first.terrainRouteReservation && !second.terrainRouteReservation;
			if (criticalTerrace || normalDoor) JoinFloorComponents(ri, other);
		}
	}

	// A planned stair can stay open itself yet be absorbed into one level
	// component through other mandatory door terraces after room composition.
	// Rebinding is safe only after those terraces have been formed: a candidate
	// must be an unbypassed, ordinary main-route edge whose endpoints are still
	// distinct components. This preserves the door-level guarantee and makes the
	// selected rise a real serialized stair rather than a manifest-only promise.
	auto ClearVerticalConstraintMarker = [&](const VerticalRouteConstraint& constraint)
	{
		auto ClearEdge = [&](int x, int y, int direction)
		{
			if (x < 0 || x >= W || y < 0 || y >= H || direction < 0 || direction >= 4)
				return;
			const int nx = x + DX[direction];
			const int ny = y + DY[direction];
			if (nx < 0 || nx >= W || ny < 0 || ny >= H) return;
			ProcGenCell& source = Grid[y][x];
			ProcGenCell& target = Grid[ny][nx];
			source.connectionStairChain[direction] = -1;
			target.connectionStairChain[OPP[direction]] = -1;
			if (source.verticalAnchor && source.verticalRise == constraint.rise)
			{
				source.verticalAnchor = false;
				source.verticalIntent = PGVI_Flat;
				source.verticalRise = 0;
			}
			// Core marks the successor only as a room-merging sentinel. It carries no
			// beat itself, so discard it with its failed source edge.
			if (target.verticalAnchor && target.verticalRise == 0)
				target.verticalAnchor = false;
		};

		if (constraint.sourceX >= 0 && constraint.sourceY >= 0 &&
			constraint.direction >= 0)
		{
			ClearEdge(constraint.sourceX, constraint.sourceY, constraint.direction);
			return;
		}
		// Graph-level fallback bindings do not carry coordinates. Resolve only the
		// exact consecutive route edge that points at the recorded target room;
		// never erase a different stage's marker from a composed chamber.
		for (int y = 0; y < H; ++y)
		{
			for (int x = 0; x < W; ++x)
			{
				const ProcGenCell& source = Grid[y][x];
				if (!source.present || source.roomId != constraint.from ||
					!source.verticalAnchor || source.verticalRise != constraint.rise)
					continue;
				for (int direction = 0; direction < 4; ++direction)
				{
					const int nx = x + DX[direction];
					const int ny = y + DY[direction];
					if (!source.conn[direction] || nx < 0 || nx >= W || ny < 0 || ny >= H)
						continue;
					const ProcGenCell& target = Grid[ny][nx];
					if (target.present && target.roomId == constraint.to &&
						target.onMainPath && target.pathRank == source.pathRank + 1)
						ClearEdge(x, y, direction);
				}
			}
		}
	};
	auto HasSameStageStairBypass = [&](int sourceX, int sourceY, int targetX,
		int targetY, int stage) -> bool
	{
		TArray<uint8_t> visited;
		visited.Resize(W * H);
		for (unsigned int index = 0; index < visited.Size(); ++index)
			visited[index] = 0;
		TArray<int> queue;
		const int sourceIndex = sourceY * W + sourceX;
		const int targetIndex = targetY * W + targetX;
		queue.Push(sourceIndex);
		visited[sourceIndex] = 1;
		for (unsigned int qi = 0; qi < queue.Size(); ++qi)
		{
			const int current = queue[qi];
			const int currentX = current % W;
			const int currentY = current / W;
			const ProcGenCell& cell = Grid[currentY][currentX];
			for (int direction = 0; direction < 4; ++direction)
			{
				if (!cell.conn[direction]) continue;
				const int nextX = currentX + DX[direction];
				const int nextY = currentY + DY[direction];
				if (nextX < 0 || nextX >= W || nextY < 0 || nextY >= H) continue;
				const ProcGenCell& next = Grid[nextY][nextX];
				if (!next.present || cell.lockStage != stage || next.lockStage != stage)
					continue;
				const bool selectedEdge =
					(currentX == sourceX && currentY == sourceY &&
						nextX == targetX && nextY == targetY) ||
					(currentX == targetX && currentY == targetY &&
						nextX == sourceX && nextY == sourceY);
				if (selectedEdge) continue;
				const int nextIndex = nextY * W + nextX;
				if (visited[nextIndex]) continue;
				if (nextIndex == targetIndex) return true;
				visited[nextIndex] = 1;
				queue.Push(nextIndex);
			}
		}
		return false;
	};
	auto RebindCollapsedVerticalConstraint = [&](VerticalRouteConstraint& constraint) -> bool
	{
		if (constraint.stage < 0 || constraint.stage >= RunBlueprint::MaxStages)
			return false;
		int chosenX = -1;
		int chosenY = -1;
		int chosenDirection = -1;
		int chosenFrom = -1;
		int chosenTo = -1;
		uint32_t chosenScore = UINT32_MAX;
		for (int y = 0; y < H; ++y)
		{
			for (int x = 0; x < W; ++x)
			{
				const ProcGenCell& source = Grid[y][x];
				if (!source.present || !source.onMainPath || source.lockStage != constraint.stage ||
					source.hasPlayerStart || source.hasKey || source.hasExit || source.isLocked ||
					source.verticalAnchor || source.roomId < 0 ||
					source.roomId >= (int)Rooms.Size() || Rooms[source.roomId].cellCount != 1)
					continue;
				for (int direction = 0; direction < 4; ++direction)
				{
					if (!source.conn[direction] || source.connectionStairChain[direction] >= 0)
						continue;
					const int nextX = x + DX[direction];
					const int nextY = y + DY[direction];
					if (nextX < 0 || nextX >= W || nextY < 0 || nextY >= H) continue;
					const ProcGenCell& target = Grid[nextY][nextX];
					if (!target.present || !target.onMainPath ||
						target.lockStage != constraint.stage ||
						target.pathRank != source.pathRank + 1 ||
						target.hasPlayerStart || target.hasKey || target.hasExit || target.isLocked ||
						target.verticalAnchor || target.roomId < 0 ||
						target.roomId >= (int)Rooms.Size() ||
						target.roomId == source.roomId ||
						target.connectionStairChain[OPP[direction]] >= 0)
						continue;
					if (FindFloorRoot(source.roomId) == FindFloorRoot(target.roomId) ||
						HasSameStageStairBypass(x, y, nextX, nextY, constraint.stage))
						continue;
					const uint32_t score = RoomPlanHash(blueprint.RecipeHash, y * W + x,
						constraint.stage, source.pathRank * 37 + (int)constraint.intent * 11 + direction);
					if (score >= chosenScore) continue;
					chosenScore = score;
					chosenX = x;
					chosenY = y;
					chosenDirection = direction;
					chosenFrom = source.roomId;
					chosenTo = target.roomId;
				}
			}
		}
		if (chosenDirection < 0) return false;

		ProcGenCell& source = Grid[chosenY][chosenX];
		const int targetX = chosenX + DX[chosenDirection];
		const int targetY = chosenY + DY[chosenDirection];
		ProcGenCell& target = Grid[targetY][targetX];
		RoomInfo& fromRoom = Rooms[chosenFrom];
		RoomInfo& toRoom = Rooms[chosenTo];
		fromRoom.hasDoor = false;
		toRoom.hasDoor = false;
		fromRoom.verticalIntent = constraint.intent;
		fromRoom.verticalRise = constraint.rise;
		fromRoom.verticalAnchor = true;
		source.verticalIntent = constraint.intent;
		source.verticalRise = constraint.rise;
		source.verticalAnchor = true;
		const int chain = constraint.stage * 4096 + source.pathRank;
		source.connectionStairChain[chosenDirection] = chain;
		target.connectionStairChain[OPP[chosenDirection]] = chain;
		if (source.connectionProfile[chosenDirection] == PGCP_Narrow ||
			target.connectionProfile[OPP[chosenDirection]] == PGCP_Narrow)
		{
			source.connectionProfile[chosenDirection] = PGCP_Standard;
			target.connectionProfile[OPP[chosenDirection]] = PGCP_Standard;
		}
		source.connectionClearWidth[chosenDirection] = std::max(
			source.connectionClearWidth[chosenDirection], 128);
		target.connectionClearWidth[OPP[chosenDirection]] = std::max(
			target.connectionClearWidth[OPP[chosenDirection]], 128);
		source.connectionDepth[chosenDirection] = std::max(
			source.connectionDepth[chosenDirection], 64);
		target.connectionDepth[OPP[chosenDirection]] = std::max(
			target.connectionDepth[OPP[chosenDirection]], 64);
		constraint.from = chosenFrom;
		constraint.to = chosenTo;
		constraint.sourceX = chosenX;
		constraint.sourceY = chosenY;
		constraint.direction = chosenDirection;
		return true;
	};
	for (unsigned int ci = 0; ci < verticalConstraints.Size(); ++ci)
	{
		VerticalRouteConstraint& constraint = verticalConstraints[ci];
		if (constraint.from < 0 || constraint.to < 0 ||
			FindFloorRoot(constraint.from) != FindFloorRoot(constraint.to))
			continue;
		ClearVerticalConstraintMarker(constraint);
		RoomInfo& oldFrom = Rooms[constraint.from];
		oldFrom.verticalAnchor = false;
		oldFrom.verticalIntent = PGVI_Flat;
		oldFrom.verticalRise = 0;
		RebindCollapsedVerticalConstraint(constraint);
	}
	// Apply the signed design rises after rooms choose their door state but
	// before component projection. The endpoints intentionally remain separate
	// components, so the stair edge retains its authored ascent/descent instead
	// of being averaged away by a nearby keyed threshold.
	for (const VerticalRouteConstraint& constraint : verticalConstraints)
	{
		if (constraint.from < 0 || constraint.to < 0) continue;
		Rooms[constraint.to].floorZ = Rooms[constraint.from].floorZ + constraint.rise;
	}
	// Do not publish the constraints as realized yet. Component projection below
	// deliberately smooths room floors around ordinary door thresholds and can
	// otherwise turn a requested 32-unit dogleg into a shorter, misleading stair
	// while the manifest still advertises the original rise. We finalize both the
	// component floors and the realization record together after that projection.

	TArray<int> componentFloor;
	TArray<int> componentMembers;
	TArray<int> componentDistance;
	componentFloor.Resize(Rooms.Size());
	componentMembers.Resize(Rooms.Size());
	componentDistance.Resize(Rooms.Size());
	for (unsigned int ri = 0; ri < Rooms.Size(); ri++)
	{
		componentFloor[ri] = 0;
		componentMembers[ri] = 0;
		componentDistance[ri] = 0x3fffffff;
	}
	for (unsigned int ri = 0; ri < Rooms.Size(); ri++)
	{
		const int root = FindFloorRoot(ri);
		componentFloor[root] += (int)lround(Rooms[ri].floorZ);
		componentMembers[root]++;
		componentDistance[root] = std::min(componentDistance[root], Rooms[ri].distFromStart);
	}
	int startRoot = startRoom >= 0 ? FindFloorRoot(startRoom) : -1;
	for (unsigned int ri = 0; ri < Rooms.Size(); ri++)
	{
		if (componentMembers[ri] == 0) continue;
		componentFloor[ri] = (int)lround(
			componentFloor[ri] / (double)componentMembers[ri] / 16.0) * 16;
	}
	if (startRoot >= 0) componentFloor[startRoot] = 0;

	// Limit one inter-room staircase to 64 units (eight short risers). Iterate
	// over graph cycles while keeping the player-start terrace anchored at zero.
	// Dramatic verticality on an Exploratory size-80 graph can have hundreds of
	// composed-room components. Use a graph-sized convergence bound instead of a
	// small constant so the 64-unit stair constraint propagates all the way from
	// the anchored start terrace through the longest optional limb.
	const int floorRelaxationPasses = std::max(24, (int)Rooms.Size() + 4);
	for (int pass = 0; pass < floorRelaxationPasses; pass++)
	{
		bool changed = false;
		for (unsigned int ri = 0; ri < Rooms.Size(); ri++)
		{
			for (unsigned int ai = 0; ai < adjacency[ri].Size(); ai++)
			{
				const int firstRoot = FindFloorRoot(ri);
				const int secondRoot = FindFloorRoot(adjacency[ri][ai]);
				if (firstRoot == secondRoot) continue;
				const int difference = componentFloor[firstRoot] - componentFloor[secondRoot];
				if (abs(difference) <= 64) continue;
				int movingRoot;
				int fixedRoot;
				if (firstRoot == startRoot)
				{
					movingRoot = secondRoot;
					fixedRoot = firstRoot;
				}
				else if (secondRoot == startRoot)
				{
					movingRoot = firstRoot;
					fixedRoot = secondRoot;
				}
				else if (componentDistance[firstRoot] > componentDistance[secondRoot] ||
					(componentDistance[firstRoot] == componentDistance[secondRoot] &&
						firstRoot > secondRoot))
				{
					movingRoot = firstRoot;
					fixedRoot = secondRoot;
				}
				else
				{
					movingRoot = secondRoot;
					fixedRoot = firstRoot;
				}
				componentFloor[movingRoot] = componentFloor[fixedRoot] +
					(componentFloor[movingRoot] > componentFloor[fixedRoot] ? 64 : -64);
				changed = true;
			}
		}
		if (!changed) break;
	}
	bool terraceConstraintViolated = false;
	for (unsigned int ri = 0; ri < Rooms.Size() && !terraceConstraintViolated; ri++)
	{
		for (unsigned int ai = 0; ai < adjacency[ri].Size(); ai++)
		{
			const int firstRoot = FindFloorRoot(ri);
			const int secondRoot = FindFloorRoot(adjacency[ri][ai]);
			if (firstRoot != secondRoot &&
				abs(componentFloor[firstRoot] - componentFloor[secondRoot]) > 64)
			{
				terraceConstraintViolated = true;
				break;
			}
		}
	}
	if (terraceConstraintViolated)
	{
		// Cyclic graphs can make local target-preserving projections oscillate.
		// Fall back to a component-graph BFS cadence: endpoints of every edge then
		// differ in graph distance by at most one, and every cadence step is at most
		// 48 units. Phase transitions may add 16, retaining the hard 64-unit bound.
		TArray<TArray<int>> componentAdjacency;
		componentAdjacency.Resize(Rooms.Size());
		for (unsigned int ri = 0; ri < Rooms.Size(); ri++)
		{
			const int firstRoot = FindFloorRoot(ri);
			for (unsigned int ai = 0; ai < adjacency[ri].Size(); ai++)
			{
				const int secondRoot = FindFloorRoot(adjacency[ri][ai]);
				if (firstRoot == secondRoot) continue;
				if (!ContainsRoom(componentAdjacency[firstRoot], secondRoot))
					componentAdjacency[firstRoot].Push(secondRoot);
				if (!ContainsRoom(componentAdjacency[secondRoot], firstRoot))
					componentAdjacency[secondRoot].Push(firstRoot);
			}
		}
		TArray<int> graphDistance;
		graphDistance.Resize(Rooms.Size());
		for (unsigned int ri = 0; ri < graphDistance.Size(); ri++) graphDistance[ri] = -1;
		TArray<int> componentQueue;
		if (startRoot >= 0)
		{
			graphDistance[startRoot] = 0;
			componentQueue.Push(startRoot);
		}
		for (unsigned int qi = 0; qi < componentQueue.Size(); qi++)
		{
			const int root = componentQueue[qi];
			for (unsigned int ai = 0; ai < componentAdjacency[root].Size(); ai++)
			{
				const int other = componentAdjacency[root][ai];
				if (graphDistance[other] >= 0) continue;
				graphDistance[other] = graphDistance[root] + 1;
				componentQueue.Push(other);
			}
		}
		// The rare cyclic fallback remains graph-derived too: one BFS layer is at
		// most 48 units from the next, so it cannot reintroduce an implicit ledge
		// after the terrain field above was rejected by a cycle.
		const int fallbackStep = Verticality == 0 ? 16 :
			(Verticality == 1 ? 32 : 48);
		const int fallbackSign = blueprint.MainRouteElevationTarget == 0 ?
			((blueprint.RecipeHash & 1u) != 0u ? 1 : -1) :
			(blueprint.MainRouteElevationTarget > 0 ? 1 : -1);
		for (unsigned int ri = 0; ri < Rooms.Size(); ri++)
		{
			if (FindFloorRoot(ri) != (int)ri) continue;
			const int distance = std::max(0, graphDistance[ri]);
			const int ripple = distance > 1 &&
				(RoomPlanHash(blueprint.RecipeHash, (int)ri, distance, 97) & 3u) == 0u ?
				8 : 0;
			int floor = fallbackSign * std::min(terrainLimit,
				distance * fallbackStep + ripple);
			componentFloor[ri] = floor;
		}
	}

	// Component averaging and the rare graph-distance fallback above are useful
	// for keeping arbitrary branch connections within the 64-unit staircase
	// bound, but an authored main-route vertical beat is a stronger contract. Put
	// every feasible constraint back on its exact component-to-component delta
	// now that the broad projection has settled. This moves a complete component,
	// so all door-linked rooms remain on one terrace; it never creates a step
	// beneath a door threshold. If a cyclic branch makes the exact delta unsafe,
	// leave the safe projection intact and truthfully drop that beat instead of
	// claiming a dogleg that the serialized map cannot contain.
	// Treat every accepted vertical beat as a rigid, signed relation between
	// component terraces: floor(to) = floor(from) + rise.  This weighted union
	// keeps a target's neighboring branch components free to move with it during
	// the normal 64-unit projection instead of making a correct route beat lose
	// to an unrelated local average.
	TArray<int> verticalParent;
	TArray<int> verticalOffset;
	verticalParent.Resize(Rooms.Size());
	verticalOffset.Resize(Rooms.Size());
	for (unsigned int ri = 0; ri < Rooms.Size(); ++ri)
	{
		verticalParent[ri] = (int)ri;
		verticalOffset[ri] = 0;
	}
	auto FindVerticalRoot = [&](int component, int& offset) -> int
	{
		offset = 0;
		while (verticalParent[component] != component)
		{
			offset += verticalOffset[component];
			component = verticalParent[component];
		}
		return component;
	};

	auto VerticalRelationsFit = [&]() -> bool
	{
		for (unsigned int ri = 0; ri < Rooms.Size(); ++ri)
		{
			const int firstComponent = FindFloorRoot((int)ri);
			for (unsigned int ai = 0; ai < adjacency[ri].Size(); ++ai)
			{
				const int secondComponent = FindFloorRoot(adjacency[ri][ai]);
				if (firstComponent == secondComponent) continue;
				int firstOffset = 0;
				int secondOffset = 0;
				const int firstGroup = FindVerticalRoot(firstComponent, firstOffset);
				const int secondGroup = FindVerticalRoot(secondComponent, secondOffset);
				if (firstGroup == secondGroup && abs(firstOffset - secondOffset) > 64)
				{
					return false;
				}
			}
		}
		return true;
	};

	TArray<bool> realizedConstraints;
	realizedConstraints.Resize(verticalConstraints.Size());
	for (unsigned int ci = 0; ci < realizedConstraints.Size(); ++ci)
		realizedConstraints[ci] = false;
	for (unsigned int ci = 0; ci < verticalConstraints.Size(); ++ci)
	{
		const VerticalRouteConstraint& constraint = verticalConstraints[ci];
		const int fromComponent = FindFloorRoot(constraint.from);
		const int toComponent = FindFloorRoot(constraint.to);
		int fromOffset = 0;
		int toOffset = 0;
		const int fromGroup = FindVerticalRoot(fromComponent, fromOffset);
		const int toGroup = FindVerticalRoot(toComponent, toOffset);
		if (fromGroup == toGroup)
		{
			realizedConstraints[ci] = toOffset == fromOffset + constraint.rise;
			continue;
		}

		// Make the target group's base relative to the source group's base. If
		// that would put an ordinary edge inside one rigid group beyond a legal
		// stair rise, undo only this beat and retain the rest of the plan.
		verticalParent[toGroup] = fromGroup;
		verticalOffset[toGroup] = fromOffset + constraint.rise - toOffset;
		if (!VerticalRelationsFit())
		{
			verticalParent[toGroup] = toGroup;
			verticalOffset[toGroup] = 0;
			continue;
		}
		realizedConstraints[ci] = true;
	}

	TArray<int> groupForComponent;
	TArray<int> groupOffset;
	TArray<int> groupBase;
	TArray<int> groupMembers;
	TArray<int> groupDistance;
	groupForComponent.Resize(Rooms.Size());
	groupOffset.Resize(Rooms.Size());
	groupBase.Resize(Rooms.Size());
	groupMembers.Resize(Rooms.Size());
	groupDistance.Resize(Rooms.Size());
	for (unsigned int ri = 0; ri < Rooms.Size(); ++ri)
	{
		groupForComponent[ri] = -1;
		groupOffset[ri] = 0;
		groupBase[ri] = 0;
		groupMembers[ri] = 0;
		groupDistance[ri] = 0x3fffffff;
	}
	for (unsigned int ri = 0; ri < Rooms.Size(); ++ri)
	{
		if (FindFloorRoot((int)ri) != (int)ri) continue;
		int offset = 0;
		const int group = FindVerticalRoot((int)ri, offset);
		groupForComponent[ri] = group;
		groupOffset[ri] = offset;
		groupBase[group] += componentFloor[ri] - offset;
		groupMembers[group]++;
		groupDistance[group] = std::min(groupDistance[group], componentDistance[ri]);
	}
	for (unsigned int group = 0; group < groupMembers.Size(); ++group)
	{
		if (groupMembers[group] == 0) continue;
		groupBase[group] = (int)lround(
			groupBase[group] / (double)groupMembers[group] / 16.0) * 16;
	}
	const int startGroup = startRoot >= 0 ? groupForComponent[startRoot] : -1;
	if (startGroup >= 0)
		groupBase[startGroup] = -groupOffset[startRoot];
	auto ProjectedFloor = [&](int component) -> int
	{
		return groupBase[groupForComponent[component]] + groupOffset[component];
	};
	auto ProjectedTransitionsFit = [&]() -> bool
	{
		for (unsigned int ri = 0; ri < Rooms.Size(); ++ri)
		{
			const int firstComponent = FindFloorRoot((int)ri);
			for (unsigned int ai = 0; ai < adjacency[ri].Size(); ++ai)
			{
				const int secondComponent = FindFloorRoot(adjacency[ri][ai]);
				if (abs(ProjectedFloor(firstComponent) -
					ProjectedFloor(secondComponent)) > 64)
					return false;
			}
		}
		return true;
	};

	const int constrainedRelaxationPasses = std::max(24, (int)Rooms.Size() + 4);
	for (int pass = 0; pass < constrainedRelaxationPasses; ++pass)
	{
		bool changed = false;
		for (unsigned int ri = 0; ri < Rooms.Size(); ++ri)
		{
			const int firstComponent = FindFloorRoot((int)ri);
			for (unsigned int ai = 0; ai < adjacency[ri].Size(); ++ai)
			{
				const int secondComponent = FindFloorRoot(adjacency[ri][ai]);
				const int firstGroup = groupForComponent[firstComponent];
				const int secondGroup = groupForComponent[secondComponent];
				if (firstGroup == secondGroup) continue;
				const int firstFloor = ProjectedFloor(firstComponent);
				const int secondFloor = ProjectedFloor(secondComponent);
				if (abs(firstFloor - secondFloor) <= 64) continue;

				int movingComponent = firstComponent;
				int fixedComponent = secondComponent;
				if (firstGroup == startGroup)
				{
					movingComponent = secondComponent;
					fixedComponent = firstComponent;
				}
				else if (secondGroup != startGroup &&
					(groupDistance[secondGroup] > groupDistance[firstGroup] ||
						(groupDistance[secondGroup] == groupDistance[firstGroup] &&
							secondGroup > firstGroup)))
				{
					movingComponent = secondComponent;
					fixedComponent = firstComponent;
				}
				const int movingFloor = ProjectedFloor(movingComponent);
				const int fixedFloor = ProjectedFloor(fixedComponent);
				const int desiredFloor = fixedFloor +
					(movingFloor > fixedFloor ? 64 : -64);
				groupBase[groupForComponent[movingComponent]] =
					desiredFloor - groupOffset[movingComponent];
				changed = true;
			}
		}
		if (!changed) break;
	}

	if (ProjectedTransitionsFit())
	{
		for (unsigned int ri = 0; ri < Rooms.Size(); ++ri)
		{
			if (FindFloorRoot((int)ri) == (int)ri)
				componentFloor[ri] = ProjectedFloor((int)ri);
		}
	}
	else
	{
		// This is an extremely constrained cyclic fallback. Preserve the original
		// safe terrain and report no fake vertical realization rather than letting
		// a route beat violate the 64-unit stair contract.
		for (unsigned int ci = 0; ci < realizedConstraints.Size(); ++ci)
			realizedConstraints[ci] = false;
	}

	// The broad target field above is intentionally gentle around doors, keys,
	// and cycles.  On a large dramatic run that conservatism could previously
	// erase the one promised highland/basin altogether.  Re-anchor a feasible
	// component-graph chain here, after every door terrace and eight-unit stair
	// relation is known.  This solves only group bases; vertical beat offsets
	// remain rigid, and every graph edge is still clamped to a 64-unit walk.
	const bool wantsExtremeTerrain = Verticality == 2 && Size >= 3 &&
		blueprint.MainRouteElevationTarget != 0 && startGroup >= 0;
	if (wantsExtremeTerrain)
	{
		TArray<TArray<int>> terrainGroupAdjacency;
		terrainGroupAdjacency.Resize(Rooms.Size());
		for (unsigned int ri = 0; ri < Rooms.Size(); ++ri)
		{
			const int firstComponent = FindFloorRoot((int)ri);
			const int firstGroup = groupForComponent[firstComponent];
			if (firstGroup < 0) continue;
			for (unsigned int ai = 0; ai < adjacency[ri].Size(); ++ai)
			{
				const int secondComponent = FindFloorRoot(adjacency[ri][ai]);
				const int secondGroup = groupForComponent[secondComponent];
				if (secondGroup < 0 || firstGroup == secondGroup) continue;
				if (!ContainsRoom(terrainGroupAdjacency[firstGroup], secondGroup))
					terrainGroupAdjacency[firstGroup].Push(secondGroup);
				if (!ContainsRoom(terrainGroupAdjacency[secondGroup], firstGroup))
					terrainGroupAdjacency[secondGroup].Push(firstGroup);
			}
		}
		auto BuildTerrainDistances = [&](int source, TArray<int>& distances)
		{
			distances.Resize(Rooms.Size());
			for (unsigned int index = 0; index < distances.Size(); ++index)
				distances[index] = -1;
			if (source < 0 || source >= (int)terrainGroupAdjacency.Size()) return;
			TArray<int> queue;
			distances[source] = 0;
			queue.Push(source);
			for (unsigned int qi = 0; qi < queue.Size(); ++qi)
			{
				const int group = queue[qi];
				for (unsigned int ai = 0; ai < terrainGroupAdjacency[group].Size(); ++ai)
				{
					const int other = terrainGroupAdjacency[group][ai];
					if (distances[other] >= 0) continue;
					distances[other] = distances[group] + 1;
					queue.Push(other);
				}
			}
		};

		struct TerrainAnchor
		{
			int room = -1;
			int component = -1;
			int group = -1;
			uint32_t score = UINT32_MAX;
		};
		auto IsTerrainAnchor = [](const RoomInfo& room) -> bool
		{
			return !room.hasPlayerStart && !room.hasKey && !room.hasExit &&
				!room.hasBoss && !room.isLocked && !room.isSecret;
		};
		const int mainTarget = blueprint.MainRouteElevationTarget;
		// Size-five dramatic runs promise an opposite reachable district, not a
		// merely decorative 64-unit terrace.  Some compact recipes deliberately
		// start with that small broad-field suggestion; promote it only for this
		// anchor search to the smallest contractual opposite extreme.  Do not
		// publish the promoted value here: Blueprint is updated below only after
		// ApplyTerrainAnchors has proved and committed the complete stair chain.
		const int optionalTarget = Verticality == 2 && Size >= 5 &&
			abs(blueprint.OptionalElevationTarget) < 192 ?
			(mainTarget < 0 ? 192 : -192) : blueprint.OptionalElevationTarget;
		const int mainSteps = (abs(mainTarget) + 63) / 64;
		// A compact dramatic route may contain a rigid dogleg group whose other
		// member sits 32--64 units above the nominated horizon room.  The recipe
		// target is intentionally only the broad destination; the actual route
		// still has to stay inside the documented size-three/four 128--192 band.
		// Keep a global cap here rather than flattening the dogleg after the fact:
		// rejecting an over-cap anchor preserves both its explicit stair relation
		// and every adjacent <=64-unit walking constraint.
		const int dramaticTerrainCap = Size >= 5 ? 320 : 192;
		const int dramaticTerrainFloor = Size >= 5 ? 192 : 128;
		auto ReboundMainTarget = [&](const TerrainAnchor& anchor) -> int
		{
			if (Verticality != 2 || Size < 3) return mainTarget;
			int rebound = mainTarget;
			for (unsigned int ri = 0; ri < Rooms.Size(); ++ri)
			{
				const RoomInfo& room = Rooms[ri];
				if (!room.onMainPath || room.hasPlayerStart) continue;
				const int component = FindFloorRoot((int)ri);
				if (groupForComponent[component] != anchor.group) continue;
				const int relativeOffset = groupOffset[component] -
					groupOffset[anchor.component];
				if (mainTarget > 0)
					rebound = std::min(rebound, dramaticTerrainCap - relativeOffset);
				else
					rebound = std::max(rebound, -dramaticTerrainCap - relativeOffset);
			}
			rebound = QuantizeToEight(rebound);
			if (abs(rebound) < dramaticTerrainFloor ||
				(mainTarget > 0) != (rebound > 0))
				return 0;
			return rebound;
		};
		TArray<int> fromStart;
		BuildTerrainDistances(startGroup, fromStart);
		TArray<TerrainAnchor> mainAnchors;
		for (unsigned int ri = 0; ri < Rooms.Size(); ++ri)
		{
			const RoomInfo& room = Rooms[ri];
			if (!room.onMainPath || !IsTerrainAnchor(room)) continue;
			const int component = FindFloorRoot((int)ri);
			const int group = groupForComponent[component];
			if (group < 0 || group == startGroup || fromStart[group] < mainSteps) continue;
			uint32_t score = RoomPlanHash(blueprint.RecipeHash, room.id,
				room.progressionRank, 0x741);
			if ((room.elevationTarget > 0) != (mainTarget > 0)) score >>= 1;
			if (room.terrainRouteReservation) score >>= 2;
			TerrainAnchor anchor;
			anchor.room = (int)ri;
			anchor.component = component;
			anchor.group = group;
			anchor.score = score;
			mainAnchors.Push(anchor);
		}
		auto ApplyTerrainAnchors = [&](const TerrainAnchor& mainAnchor,
			int requestedMainTarget, const TerrainAnchor* optionalAnchor,
			int requestedOptionalTarget) -> bool
		{
			TArray<int> savedBase;
			savedBase.Resize(groupBase.Size());
			for (unsigned int index = 0; index < groupBase.Size(); ++index)
				savedBase[index] = groupBase[index];

			// A highland and an opposite optional basin are a set of bounded
			// difference constraints, not a direction-dependent smoothing pass.
			// The former relaxation moved the first non-fixed side of each edge;
			// on a cyclic large map that can make an otherwise feasible pair of
			// anchors look impossible (or soften it below the promised extreme).
			// Keep the component offsets from authored doglegs rigid, then solve
			// every walking edge as |floor(a) - floor(b)| <= 64.  Bellman-Ford on
			// the equivalent upper-bound system is deterministic and proves the
			// fixed start/main/optional terraces are jointly feasible before we
			// commit their bases.
			TArray<bool> fixedGroup;
			fixedGroup.Resize(groupBase.Size());
			TArray<int> fixedBase;
			fixedBase.Resize(groupBase.Size());
			for (unsigned int index = 0; index < fixedGroup.Size(); ++index)
			{
				fixedGroup[index] = false;
				fixedBase[index] = 0;
			}
			auto PinTerrainGroup = [&](int group, int base) -> bool
			{
				if (group < 0 || group >= (int)fixedGroup.Size()) return false;
				if (fixedGroup[group]) return fixedBase[group] == base;
				fixedGroup[group] = true;
				fixedBase[group] = base;
				return true;
			};
			if (!PinTerrainGroup(startGroup, -groupOffset[startRoot]) ||
				!PinTerrainGroup(mainAnchor.group,
					requestedMainTarget - groupOffset[mainAnchor.component]))
				return false;
			if (optionalAnchor != nullptr)
			{
				if (!PinTerrainGroup(optionalAnchor->group,
					requestedOptionalTarget - groupOffset[optionalAnchor->component]))
					return false;
			}

			struct TerrainDifferenceConstraint
			{
				int from = -1;
				int to = -1;
				int upperBound = 0;
			};
			TArray<TerrainDifferenceConstraint> terrainConstraints;
			auto AddTerrainConstraint = [&](int from, int to, int upperBound)
			{
				if (from >= 0 && to >= 0)
					terrainConstraints.Push({ from, to, upperBound });
			};
			for (unsigned int ri = 0; ri < Rooms.Size(); ++ri)
			{
				const int firstComponent = FindFloorRoot((int)ri);
				const int firstGroup = groupForComponent[firstComponent];
				for (unsigned int ai = 0; ai < adjacency[ri].Size(); ++ai)
				{
					const int secondComponent = FindFloorRoot(adjacency[ri][ai]);
					const int secondGroup = groupForComponent[secondComponent];
					if (firstGroup < 0 || secondGroup < 0 || firstGroup == secondGroup)
						continue;
					const int firstOffset = groupOffset[firstComponent];
					const int secondOffset = groupOffset[secondComponent];
					// base(second) - base(first) <= 64 + offset(first) - offset(second)
					AddTerrainConstraint(firstGroup, secondGroup,
						64 + firstOffset - secondOffset);
					AddTerrainConstraint(secondGroup, firstGroup,
						64 + secondOffset - firstOffset);
				}
			}
			const int terrainSource = (int)groupBase.Size();
			if (Verticality == 2 && Size >= 3)
			{
				// Bound every main-route room, not just the room nominated as the
				// horizon anchor. Rigid dogleg offsets can otherwise make a sibling
				// terrace silently exceed the size-specific highland/basin range.
				for (unsigned int ri = 0; ri < Rooms.Size(); ++ri)
				{
					const RoomInfo& room = Rooms[ri];
					if (!room.onMainPath || room.hasPlayerStart) continue;
					const int component = FindFloorRoot((int)ri);
					const int group = groupForComponent[component];
					if (group < 0) continue;
					const int offset = groupOffset[component];
					const int oppositeTransitionLimit = dramaticTerrainFloor - 8;
					if (requestedMainTarget > 0)
					{
						// A local dogleg may dip through a shallow neutral band, but
						// only the optional district may reach the opposite dramatic
						// extreme. -limit <= floor <= cap.
						AddTerrainConstraint(group, terrainSource,
							oppositeTransitionLimit + offset);
						AddTerrainConstraint(terrainSource, group,
							dramaticTerrainCap - offset);
					}
					else
					{
						// -cap <= floor <= limit for a basin-oriented main route.
						AddTerrainConstraint(terrainSource, group,
							oppositeTransitionLimit - offset);
						AddTerrainConstraint(group, terrainSource,
							dramaticTerrainCap + offset);
					}
				}
			}
			for (unsigned int group = 0; group < fixedGroup.Size(); ++group)
			{
				if (!fixedGroup[group]) continue;
				// Pin base(group) exactly relative to an otherwise free source.
				AddTerrainConstraint(terrainSource, (int)group, fixedBase[group]);
				AddTerrainConstraint((int)group, terrainSource, -fixedBase[group]);
			}
			TArray<int> terrainPotential;
			terrainPotential.Resize(terrainSource + 1);
			for (unsigned int index = 0; index < terrainPotential.Size(); ++index)
				terrainPotential[index] = 0;
			bool impossible = false;
			for (int pass = 0; pass <= terrainSource; ++pass)
			{
				bool changed = false;
				for (const TerrainDifferenceConstraint& constraint : terrainConstraints)
				{
					const int candidate = terrainPotential[constraint.from] +
						constraint.upperBound;
					if (candidate >= terrainPotential[constraint.to]) continue;
					terrainPotential[constraint.to] = candidate;
					changed = true;
				}
				if (!changed) break;
				if (pass == terrainSource)
				{
					impossible = true;
					break;
				}
			}
			if (!impossible)
			{
				for (unsigned int group = 0; group < groupMembers.Size(); ++group)
				{
					if (groupMembers[group] == 0) continue;
					groupBase[group] = terrainPotential[group] - terrainPotential[terrainSource];
				}
				if (!ProjectedTransitionsFit())
					impossible = true;
			}
			if (!impossible)
			{
				const int realizedMain = groupBase[mainAnchor.group] +
					groupOffset[mainAnchor.component];
				const int realizedOptional = optionalAnchor == nullptr ? 0 :
					groupBase[optionalAnchor->group] + groupOffset[optionalAnchor->component];
				if (realizedMain == requestedMainTarget &&
					(optionalAnchor == nullptr || realizedOptional == requestedOptionalTarget))
				{
					bool routeWithinBound = true;
					if (Verticality == 2 && Size >= 3)
					{
						for (unsigned int ri = 0; ri < Rooms.Size(); ++ri)
						{
							const RoomInfo& room = Rooms[ri];
							if (!room.onMainPath || room.hasPlayerStart) continue;
							const int component = FindFloorRoot((int)ri);
							const int floor = groupBase[groupForComponent[component]] +
								groupOffset[component];
							if (abs(floor) > dramaticTerrainCap ||
								(requestedMainTarget > 0 ?
									floor <= -dramaticTerrainFloor :
									floor >= dramaticTerrainFloor))
							{
								routeWithinBound = false;
								break;
							}
						}
					}
					if (routeWithinBound) return true;
				}
			}
			for (unsigned int index = 0; index < groupBase.Size(); ++index)
				groupBase[index] = savedBase[index];
			return false;
		};

		bool terrainAnchored = false;
		int realizedTerrainMainRoom = -1;
		int realizedTerrainMainTarget = 0;
		int realizedTerrainOptionalRoom = -1;
		int realizedTerrainOptionalTarget = 0;
		TArray<bool> mainTried;
		mainTried.Resize(mainAnchors.Size());
		for (unsigned int index = 0; index < mainTried.Size(); ++index) mainTried[index] = false;
		for (unsigned int mainAttempt = 0;
			mainAttempt < mainAnchors.Size() && mainAttempt < 16 && !terrainAnchored;
			++mainAttempt)
		{
			int bestMain = -1;
			for (unsigned int index = 0; index < mainAnchors.Size(); ++index)
				if (!mainTried[index] && (bestMain < 0 ||
					mainAnchors[index].score < mainAnchors[bestMain].score))
					bestMain = (int)index;
			if (bestMain < 0) break;
			mainTried[bestMain] = true;
			const TerrainAnchor& mainAnchor = mainAnchors[bestMain];
			const int candidateMainTarget = ReboundMainTarget(mainAnchor);
			if (candidateMainTarget == 0) continue;
			TArray<int> fromMain;
			BuildTerrainDistances(mainAnchor.group, fromMain);
			if (optionalTarget == 0)
			{
				terrainAnchored = ApplyTerrainAnchors(mainAnchor, candidateMainTarget,
					nullptr, 0);
				if (terrainAnchored)
				{
					realizedTerrainMainRoom = mainAnchor.room;
					realizedTerrainMainTarget = candidateMainTarget;
				}
				continue;
			}

			// A 320-unit opposite district needs five clean 64-unit walks from
			// its local terrace and nine from a 256-unit main horizon. A compact
			// but otherwise healthy graph can legitimately have only seven such
			// links. Keep the required opposite extreme rather than silently
			// flattening it: deterministically try the recipe's requested target,
			// then the smaller 256/192-unit alternatives that the size-5 contract
			// still explicitly permits.
			const int optionalSign = optionalTarget < 0 ? -1 : 1;
			for (int magnitude = abs(optionalTarget);
				magnitude >= 192 && !terrainAnchored; magnitude -= 64)
			{
				const int candidateOptionalTarget = optionalSign * magnitude;
				const int optionalSteps = (abs(candidateOptionalTarget) + 63) / 64;
				const int oppositeSteps = (abs(candidateMainTarget - candidateOptionalTarget) + 63) / 64;
				TArray<TerrainAnchor> optionalAnchors;
				for (unsigned int ri = 0; ri < Rooms.Size(); ++ri)
				{
					const RoomInfo& room = Rooms[ri];
					if (room.onMainPath || room.branchDepth < 1)
						continue;
					if (!IsTerrainAnchor(room)) continue;
					const int component = FindFloorRoot((int)ri);
					const int group = groupForComponent[component];
					if (group < 0 || group == mainAnchor.group || group == startGroup ||
						fromStart[group] < optionalSteps)
						continue;
					if (fromMain[group] < oppositeSteps) continue;
					TerrainAnchor anchor;
					anchor.room = (int)ri;
					anchor.component = component;
					anchor.group = group;
					anchor.score = RoomPlanHash(blueprint.RecipeHash, room.id,
						room.branchDepth, 0x9b1);
					if (room.terrainRouteReservation) anchor.score >>= 2;
					optionalAnchors.Push(anchor);
				}
				TArray<bool> optionalTried;
				optionalTried.Resize(optionalAnchors.Size());
				for (unsigned int index = 0; index < optionalTried.Size(); ++index)
					optionalTried[index] = false;
				for (unsigned int optionalAttempt = 0;
					optionalAttempt < optionalAnchors.Size() && optionalAttempt < 16 && !terrainAnchored;
					++optionalAttempt)
				{
					int bestOptional = -1;
					for (unsigned int index = 0; index < optionalAnchors.Size(); ++index)
					{
						if (!optionalTried[index] && (bestOptional < 0 ||
							optionalAnchors[index].score < optionalAnchors[bestOptional].score))
							bestOptional = (int)index;
					}
					if (bestOptional < 0) break;
					const TerrainAnchor optionalAnchor = optionalAnchors[bestOptional];
					optionalTried[bestOptional] = true;
					terrainAnchored = ApplyTerrainAnchors(mainAnchor, candidateMainTarget,
						&optionalAnchor, candidateOptionalTarget);
					if (terrainAnchored)
					{
						realizedTerrainMainRoom = mainAnchor.room;
						realizedTerrainMainTarget = candidateMainTarget;
						realizedTerrainOptionalRoom = optionalAnchor.room;
						realizedTerrainOptionalTarget = candidateOptionalTarget;
					}
				}
			}
		}
		// If a recipe asked for a 256/320-unit pair but the selected, protected
		// optional limb only has the six real terrace transitions required by the
		// size-five contract, rebind both ends to the valid +/-192 fallback.  This
		// is deliberately a second solver pass rather than a manifest rewrite:
		// the graph proof still has to establish every <=64-unit walk, and a map
		// with no feasible pair remains on the ordinary safe terrain path.
		if (!terrainAnchored && optionalTarget != 0 && Size >= 5)
		{
			const int fallbackMainTarget = mainTarget < 0 ? -192 : 192;
			const int fallbackOptionalTarget = -fallbackMainTarget;
			TArray<TerrainAnchor> fallbackMainAnchors;
			for (unsigned int ri = 0; ri < Rooms.Size(); ++ri)
			{
				const RoomInfo& room = Rooms[ri];
				if (!room.onMainPath || !IsTerrainAnchor(room)) continue;
				const int component = FindFloorRoot((int)ri);
				const int group = groupForComponent[component];
				if (group < 0 || group == startGroup || fromStart[group] < 3) continue;
				TerrainAnchor anchor;
				anchor.room = (int)ri;
				anchor.component = component;
				anchor.group = group;
				anchor.score = RoomPlanHash(blueprint.RecipeHash, room.id,
					room.progressionRank, 0x5d7);
				if (room.terrainRouteReservation) anchor.score >>= 2;
				fallbackMainAnchors.Push(anchor);
			}
			TArray<bool> fallbackMainTried;
			fallbackMainTried.Resize(fallbackMainAnchors.Size());
			for (unsigned int index = 0; index < fallbackMainTried.Size(); ++index)
				fallbackMainTried[index] = false;
			for (unsigned int mainAttempt = 0;
				mainAttempt < fallbackMainAnchors.Size() && mainAttempt < 64 && !terrainAnchored;
				++mainAttempt)
			{
				int bestMain = -1;
				for (unsigned int index = 0; index < fallbackMainAnchors.Size(); ++index)
					if (!fallbackMainTried[index] && (bestMain < 0 ||
						fallbackMainAnchors[index].score < fallbackMainAnchors[bestMain].score))
						bestMain = (int)index;
				if (bestMain < 0) break;
				fallbackMainTried[bestMain] = true;
				const TerrainAnchor& mainAnchor = fallbackMainAnchors[bestMain];
				TArray<int> fromMain;
				BuildTerrainDistances(mainAnchor.group, fromMain);
				TArray<TerrainAnchor> fallbackOptionalAnchors;
				for (unsigned int ri = 0; ri < Rooms.Size(); ++ri)
				{
					const RoomInfo& room = Rooms[ri];
					if (room.onMainPath || room.branchDepth < 1 || !IsTerrainAnchor(room))
						continue;
					const int component = FindFloorRoot((int)ri);
					const int group = groupForComponent[component];
					if (group < 0 || group == startGroup || group == mainAnchor.group ||
						fromStart[group] < 3 || fromMain[group] < 6)
						continue;
					TerrainAnchor anchor;
					anchor.room = (int)ri;
					anchor.component = component;
					anchor.group = group;
					anchor.score = RoomPlanHash(blueprint.RecipeHash, room.id,
						room.branchDepth, 0xa3d);
					if (room.terrainRouteReservation) anchor.score >>= 2;
					fallbackOptionalAnchors.Push(anchor);
				}
				TArray<bool> fallbackOptionalTried;
				fallbackOptionalTried.Resize(fallbackOptionalAnchors.Size());
				for (unsigned int index = 0; index < fallbackOptionalTried.Size(); ++index)
					fallbackOptionalTried[index] = false;
				for (unsigned int optionalAttempt = 0;
					optionalAttempt < fallbackOptionalAnchors.Size() && optionalAttempt < 64 &&
					!terrainAnchored; ++optionalAttempt)
				{
					int bestOptional = -1;
					for (unsigned int index = 0; index < fallbackOptionalAnchors.Size(); ++index)
						if (!fallbackOptionalTried[index] && (bestOptional < 0 ||
							fallbackOptionalAnchors[index].score <
							fallbackOptionalAnchors[bestOptional].score))
							bestOptional = (int)index;
					if (bestOptional < 0) break;
					fallbackOptionalTried[bestOptional] = true;
					const TerrainAnchor optionalAnchor = fallbackOptionalAnchors[bestOptional];
					terrainAnchored = ApplyTerrainAnchors(mainAnchor, fallbackMainTarget,
						&optionalAnchor, fallbackOptionalTarget);
					if (terrainAnchored)
					{
						realizedTerrainMainRoom = mainAnchor.room;
						realizedTerrainMainTarget = fallbackMainTarget;
						realizedTerrainOptionalRoom = optionalAnchor.room;
						realizedTerrainOptionalTarget = fallbackOptionalTarget;
					}
				}
			}
		}
		if (terrainAnchored)
		{
			for (unsigned int ri = 0; ri < Rooms.Size(); ++ri)
				if (FindFloorRoot((int)ri) == (int)ri)
					componentFloor[ri] = ProjectedFloor((int)ri);
			if (realizedTerrainMainRoom >= 0)
			{
				RoomInfo& mainRoom = Rooms[realizedTerrainMainRoom];
				mainRoom.elevationTarget = realizedTerrainMainTarget;
				mainRoom.elevationRole = realizedTerrainMainTarget > 0 ? PGER_Highland : PGER_Basin;
			}
			if (realizedTerrainOptionalRoom >= 0)
			{
				optionalExtremeRoom = realizedTerrainOptionalRoom;
				RoomInfo& optionalRoom = Rooms[realizedTerrainOptionalRoom];
				optionalRoom.elevationTarget = realizedTerrainOptionalTarget;
				optionalRoom.elevationRole = realizedTerrainOptionalTarget > 0 ?
					PGER_Highland : PGER_Basin;
				Blueprint.OptionalElevationTarget = realizedTerrainOptionalTarget;
			}
			}
		}

	// The manifest is realization data, not a restatement of the recipe. Reset
	// it from the exact component constraints we could keep, and make a failed
	// anchor flat before the later room-to-cell propagation reaches BuildUDMF.
	Blueprint.RealizedVerticalBeats = 0;
	for (int stage = 0; stage < RunBlueprint::MaxStages; ++stage)
	{
		Blueprint.RealizedStageVerticalIntents[stage] = PGVI_Flat;
		Blueprint.RealizedStageVerticalRises[stage] = 0;
	}
	for (unsigned int ci = 0; ci < verticalConstraints.Size(); ++ci)
	{
		const VerticalRouteConstraint& constraint = verticalConstraints[ci];
		RoomInfo& from = Rooms[constraint.from];
		if (!realizedConstraints[ci])
		{
			ClearVerticalConstraintMarker(constraint);
			from.verticalAnchor = false;
			from.verticalIntent = PGVI_Flat;
			from.verticalRise = 0;
			continue;
		}
		Blueprint.RealizedVerticalBeats++;
		if (from.lockStage >= 0 && from.lockStage < RunBlueprint::MaxStages)
		{
			Blueprint.RealizedStageVerticalIntents[from.lockStage] =
				(EProcGenVerticalIntent)from.verticalIntent;
			Blueprint.RealizedStageVerticalRises[from.lockStage] = constraint.rise;
		}
	}
	for (unsigned int ri = 0; ri < Rooms.Size(); ri++)
	{
		RoomInfo& room = Rooms[ri];
		room.floorZ = componentFloor[FindFloorRoot(ri)];
		room.ceilZ = room.floorZ + roomClearHeights[ri];
	}
	Blueprint.RealizedMainRouteElevation = 0;
	int realizedMainExtremeRoom = -1;
	for (unsigned int ri = 0; ri < Rooms.Size(); ++ri)
	{
		const RoomInfo& room = Rooms[ri];
		if (!room.onMainPath || room.hasPlayerStart) continue;
		const int floor = (int)lround(room.floorZ);
		if (abs(floor) > abs(Blueprint.RealizedMainRouteElevation))
		{
			Blueprint.RealizedMainRouteElevation = floor;
			realizedMainExtremeRoom = (int)ri;
		}
	}
	// The graph solver may place the tallest terrace in a rigid dogleg sibling
	// rather than the originally nominated horizon room.  Mark that exact
	// emitted witness as the highland/basin so the manifest describes the map
	// the player can traverse, not just the earlier planning preference.
	if (Verticality == 2 && Size >= 3 && realizedMainExtremeRoom >= 0)
	{
		RoomInfo& extreme = Rooms[realizedMainExtremeRoom];
		extreme.elevationTarget = Blueprint.RealizedMainRouteElevation;
		extreme.elevationRole = Blueprint.RealizedMainRouteElevation > 0 ?
			PGER_Highland : PGER_Basin;
	}
	for (unsigned int ri = 0; ri < Rooms.Size(); ++ri)
		Rooms[ri].optionalTerrainAnchor = false;
	Blueprint.RealizedOptionalElevation = 0;
	if (optionalExtremeRoom >= 0 && optionalExtremeRoom < (int)Rooms.Size())
	{
		RoomInfo& optional = Rooms[optionalExtremeRoom];
		optional.optionalTerrainAnchor = true;
		const int realizedOptional = (int)lround(optional.floorZ);
		Blueprint.RealizedOptionalElevation = realizedOptional;
		// A constrained cycle can legally soften an optional scenic district.
		// Keep the manifest target tied to the actual reachable terrace rather
		// than advertising the pre-projection horizon as if it had been emitted.
		if (Blueprint.OptionalElevationTarget != realizedOptional)
		{
			Blueprint.OptionalElevationTarget = realizedOptional;
			optional.elevationTarget = realizedOptional;
			optional.elevationRole = realizedOptional == 0 ? PGER_Flat :
				(abs(realizedOptional) >= 96 ?
					(realizedOptional > 0 ? PGER_Highland : PGER_Basin) : PGER_Terrace);
		}
	}

	for (int y = 0; y < H; y++)
	{
		for (int x = 0; x < W; x++)
		{
			ProcGenCell& cell = Grid[y][x];
			if (!cell.present || cell.roomId < 0) continue;
			RoomInfo& room = Rooms[cell.roomId];
			cell.floorZ = room.floorZ;
			cell.ceilZ = room.ceilZ;
			cell.floorTex = room.floorTex;
			cell.ceilTex = room.ceilTex;
			cell.wallTex = room.wallTex;
			cell.materialFamily = room.materialFamily;
			cell.footprint = room.footprint;
			cell.elevationRole = room.elevationRole;
			cell.elevationTarget = room.elevationTarget;
			cell.light = room.light;
			cell.enemyCount = room.enemyCount;
			cell.monsterTier = room.monsterTier;
		}
	}
}

void FProceduralMapGenerator::PlaceWeapons(int W, int H)
{
	(void)W;
	(void)H;
	TArray<int> mainRooms;
	TArray<int> sideRooms;
	TArray<int> plannedArmoryRooms;
	int startRoom = -1;
	int maxProgressionRank = 1;
	const ThemeStyle themeStyle = GetThemeStyle(Theme);
	const RunBlueprint& blueprint = GetRunBlueprint();
	const EProcGenArsenalTrack arsenalTrack = GetArsenalTrackKind();

	for (unsigned int ri = 0; ri < Rooms.Size(); ri++)
	{
		RoomInfo& room = Rooms[ri];
		maxProgressionRank = std::max(maxProgressionRank, room.progressionRank);
		if (room.hasPlayerStart) startRoom = ri;
		else if (room.onMainPath && !room.hasExit) mainRooms.Push(ri);
		else if (!room.hasExit) sideRooms.Push(ri);

		// The cell-level planner nominates an armory before room composition.
		// A landmark merge can absorb that cell into the main route, where it is
		// no longer an optional choice. Retain only safe side-room nominations;
		// the deterministic fallback below will choose another feasible room.
		if (room.optionalArmory)
		{
			if (!room.onMainPath && !room.hasExit && !room.hasKey && !room.isLocked)
				plannedArmoryRooms.Push(ri);
			room.optionalArmory = false;
			if (room.rewardPlan == PGRW_Armory)
				room.rewardPlan = PGRW_Cache;
		}
	}

	auto SortByDistance = [&](TArray<int>& list)
	{
		for (int i = 0; i < (int)list.Size(); i++)
		{
			int best = i;
			for (int j = i + 1; j < (int)list.Size(); j++)
				if (Rooms[list[j]].distFromStart < Rooms[list[best]].distFromStart) best = j;
			if (best != i)
			{
				int tmp = list[i];
				list[i] = list[best];
				list[best] = tmp;
			}
		}
	};
	auto SortByProgression = [&](TArray<int>& list)
	{
		for (int i = 0; i < (int)list.Size(); i++)
		{
			int best = i;
			for (int j = i + 1; j < (int)list.Size(); j++)
			{
				const RoomInfo& candidate = Rooms[list[j]];
				const RoomInfo& current = Rooms[list[best]];
				if (candidate.progressionRank < current.progressionRank ||
					(candidate.progressionRank == current.progressionRank && candidate.id < current.id))
					best = j;
			}
			if (best != i)
			{
				int tmp = list[i];
				list[i] = list[best];
				list[best] = tmp;
			}
		}
	};
	SortByProgression(mainRooms);
	SortByDistance(sideRooms);

	auto GiveWeapon = [&](int roomId, int type)
	{
		if (roomId < 0 || roomId >= (int)Rooms.Size() || Rooms[roomId].hasExit) return;
		type = ProcGenCompatibleThing(type);
		Rooms[roomId].hasWeapon = true;
		Rooms[roomId].weaponType = type;
	};
	auto GiveProgressionWeapon = [&](int preferredIndex, int type)
	{
		if (mainRooms.Size() == 0) return;
		preferredIndex = clamp(preferredIndex, 0, (int)mainRooms.Size() - 1);
		for (int distance = 0; distance < (int)mainRooms.Size(); distance++)
		{
			int candidates[] = { preferredIndex + distance, preferredIndex - distance };
			for (int candidate : candidates)
			{
				if (candidate < 0 || candidate >= (int)mainRooms.Size()) continue;
				int roomId = mainRooms[candidate];
				if (Rooms[roomId].hasWeapon) continue;
				GiveWeapon(roomId, type);
				return;
			}
		}
	};
	auto GiveOptionalWeapon = [&](int type)
	{
		for (int pass = 0; pass < 2; pass++)
		{
			for (int index = (int)sideRooms.Size() - 1; index >= 0; index--)
			{
				RoomInfo& room = Rooms[sideRooms[index]];
				if (room.hasWeapon || room.hasExit || room.hasKey || room.isLocked) continue;
				if (pass == 0 && !ContainsRoom(plannedArmoryRooms, room.id)) continue;
				GiveWeapon(room.id, type);
				room.optionalArmory = true;
				room.rewardPlan = std::max(room.rewardPlan, (int)PGRW_Armory);
				return;
			}
		}
	};

	if (startRoom >= 0) GiveWeapon(startRoom, 2001); // shotgun: immediate agency
	const bool doom2Roster = ProcGenUsesDoom2Roster();
	const int routeJitter = (int)((blueprint.RecipeHash >> 13) % 3u) - 1;
	auto RouteIndex = [&](int numerator, int denominator) -> int
	{
		if (mainRooms.Size() == 0) return 0;
		return clamp((int)mainRooms.Size() * numerator / denominator + routeJitter,
			0, (int)mainRooms.Size() - 1);
	};
	// Tracks reorder familiar stock Doom tools rather than making a mandatory
	// route depend on a rare pickup. Optional armories provide the complementary
	// playstyle and are always outside the completion path.
	switch (arsenalTrack)
	{
	case PGAT_Demolition:
		if (mainRooms.Size() > 2) GiveProgressionWeapon(RouteIndex(1, 4), 2002);
		if (Size >= 2 && mainRooms.Size() > 3) GiveProgressionWeapon(RouteIndex(2, 5), 2003);
		if (doom2Roster && mainRooms.Size() > 1) GiveProgressionWeapon(RouteIndex(3, 5), 82);
		if (Size >= 4 && mainRooms.Size() > 4) GiveProgressionWeapon(RouteIndex(4, 5), 2004);
		GiveOptionalWeapon(doom2Roster ? 82 : 2002);
		break;
	case PGAT_Energy:
		if (mainRooms.Size() > 2) GiveProgressionWeapon(RouteIndex(1, 4), 2002);
		if (doom2Roster && mainRooms.Size() > 1) GiveProgressionWeapon(RouteIndex(1, 3), 82);
		if (Size >= 2 && mainRooms.Size() > 3) GiveProgressionWeapon(RouteIndex(1, 2), 2003);
		if (Size >= 4 && mainRooms.Size() > 4) GiveProgressionWeapon(RouteIndex(2, 3), 2004);
		GiveOptionalWeapon(2003);
		break;
	case PGAT_Ballistic:
	default:
		if (doom2Roster && mainRooms.Size() > 1) GiveProgressionWeapon(RouteIndex(1, 4), 82);
		if (mainRooms.Size() > 2) GiveProgressionWeapon(RouteIndex(1, 3), 2002);
		if (Size >= 2 && mainRooms.Size() > 3) GiveProgressionWeapon(RouteIndex(1, 2), 2003);
		if (Size >= 4 && mainRooms.Size() > 4) GiveProgressionWeapon(RouteIndex(3, 4), 2004);
		GiveOptionalWeapon(arsenalTrack == PGAT_Ballistic ? 2003 : 2002);
		break;
	}
	if (Size >= 5 && Difficulty >= 5 && sideRooms.Size() > 0)
	{
		GiveOptionalWeapon(2006); // high-risk late BFG contract
	}

	auto HasMainPathWeaponBefore = [&](const RoomInfo& room, int type) -> bool
	{
		if (room.weaponType == type) return true;
		for (unsigned int ri = 0; ri < Rooms.Size(); ri++)
		{
			const RoomInfo& candidate = Rooms[ri];
			if (candidate.onMainPath && candidate.hasWeapon && candidate.weaponType == type &&
				candidate.progressionRank <= room.progressionRank)
				return true;
		}
		return false;
	};

	auto AmmoForStage = [&](const RoomInfo& room) -> int
	{
		if (room.hasWeapon)
		{
			if (room.weaponType == 2001 || room.weaponType == 82) return 2008;
			if (room.weaponType == 2002) return 2007;
			if (room.weaponType == 2003) return 2010;
			if (room.weaponType == 2004 || room.weaponType == 2006) return 2047;
		}
		int phase = clamp(room.progressionRank * 4 / (maxProgressionRank + 1), 0, 3);
		if (Size >= 4 && HasMainPathWeaponBefore(room, 2004) &&
			(arsenalTrack == PGAT_Energy || phase >= 3)) return 2047;
		if (Size >= 2 && HasMainPathWeaponBefore(room, 2003) &&
			(arsenalTrack == PGAT_Demolition || phase >= 2)) return 2010;
		return (RNG() & 1) ? 2008 : 2007;
	};
	auto LargeAmmoForStage = [&](const RoomInfo& room) -> int
	{
		int small = AmmoForStage(room);
		if (small == 2008) return 2049; // shell box
		if (small == 2007) return 2048; // bullet box
		if (small == 2010) return 2046; // rocket box
		if (small == 2047) return 17;   // cell pack
		return small;
	};

	// A cache challenge is an optional combat choice with a guaranteed useful
	// payoff, never an empty label inherited from a branch cell that later
	// merged into a larger room.
	for (RoomInfo& room : Rooms)
	{
		if (room.encounterCard != PGEC_CacheChallenge) continue;
		room.rewardPlan = std::max(room.rewardPlan, (int)PGRW_Cache);
		room.recoveryBudget = std::max(room.recoveryBudget, 1);
	}

	for (unsigned int ri = 0; ri < Rooms.Size(); ri++)
	{
		RoomInfo& room = Rooms[ri];
		if (room.hasPlayerStart)
		{
			room.hasAmmo = true;
			room.ammoType = 2008;
			room.ammoCount = 2;
			room.hasHealth = true;
			room.healthType = 2011;
			room.healthCount = 2;
			room.healthBonusCount = 4;
			continue;
		}

		const bool cardSetPiece = room.encounterCard == PGEC_SetPiece ||
			room.encounterCard == PGEC_HoldingLine;
		const bool majorFight = room.enemyCount >= 5 || room.isArena || room.hasKey ||
			room.hasExit || cardSetPiece;
		const bool sustainedFight = Difficulty >= 4 && room.enemyCount >= 3;
		const bool plannedReserve = room.rewardPlan == PGRW_Emergency ||
			room.rewardPlan == PGRW_KeyReserve || room.rewardPlan == PGRW_FinaleReserve;
		const bool reward = room.hasWeapon || room.optionalArmory || room.hasKey ||
			room.isDeadEnd || room.rewardPlan == PGRW_Cache ||
			room.rewardPlan == PGRW_Armory;
		if (room.hasWeapon || majorFight || sustainedFight || plannedReserve ||
			room.recoveryBudget > 0 || (room.onMainPath && (RNG() % 100) < 48))
		{
			room.hasAmmo = true;
			room.ammoType = majorFight ? LargeAmmoForStage(room) : AmmoForStage(room);
			room.ammoCount = majorFight || plannedReserve ? 2 : 1;
		}
		if (majorFight || reward || plannedReserve || room.recoveryBudget > 0 ||
			(sustainedFight && room.onMainPath) || (room.onMainPath && (RNG() % 100) < 62))
		{
			room.hasHealth = true;
			room.healthType = majorFight ? 2012 : 2011;
			room.healthCount = majorFight || plannedReserve ? 2 : 1;
		}
		if ((!room.hasHealth && room.onMainPath && (RNG() % 100) < 55) ||
			(!room.onMainPath && room.branchDepth >= 2))
			room.healthBonusCount = 2 + (room.branchDepth >= 2 ? 2 : 0);
		if (room.hasKey || room.hasBoss || (room.isDeadEnd && room.branchDepth >= 2 && (RNG() % 100) < 40))
		{
			room.hasArmor = true;
			room.armorType = room.hasBoss ? 2019 : 2015;
		}
	}

	// Blueprint planning never allows a third consecutive major card on the
	// critical path. The fallback reduces the third room to a skirmish instead
	// of deleting a key/exit encounter, so topology remains untouched.
	int highPressureRun = 0;
	for (unsigned int index = 0; index < mainRooms.Size(); index++)
	{
		RoomInfo& room = Rooms[mainRooms[index]];
		const bool highPressure = room.enemyCount >= 4 ||
			room.encounterCard == PGEC_Crossfire || room.encounterCard == PGEC_Pincer ||
			room.encounterCard == PGEC_Ambush || room.encounterCard == PGEC_SetPiece ||
			room.encounterCard == PGEC_HoldingLine;
		if (!highPressure)
		{
			highPressureRun = 0;
			continue;
		}
		highPressureRun++;
		if (highPressureRun <= 2) continue;
		room.encounterCard = PGEC_Skirmish;
		room.enemyCount = std::min(room.enemyCount, 2);
		room.threatBudget = std::min(room.threatBudget, 1);
		room.recoveryBudget = std::max(room.recoveryBudget, 1);
		room.rewardPlan = std::max(room.rewardPlan, (int)PGRW_Emergency);
		room.hasHealth = true;
		room.healthType = 2011;
		room.healthCount = std::max(room.healthCount, 1);
		room.hasAmmo = true;
		room.ammoType = AmmoForStage(room);
		room.ammoCount = std::max(room.ammoCount, 1);
		highPressureRun = 0;
	}

	// Never leave a long run of the critical path without recovery. Two rooms
	// may be dry for pacing, but the third always offers at least stimpacks plus
	// a few health bonuses. This remains independent of random item rolls.
	int dryMainRooms = 0;
	for (unsigned int index = 0; index < mainRooms.Size(); index++)
	{
		RoomInfo& room = Rooms[mainRooms[index]];
		if (room.hasHealth || room.healthBonusCount > 0)
		{
			dryMainRooms = 0;
			continue;
		}
		dryMainRooms++;
		if (dryMainRooms >= 3)
		{
			room.hasHealth = true;
			room.healthType = 2011;
			room.healthCount = 2;
			room.healthBonusCount = 2;
			dryMainRooms = 0;
		}
	}

	// Key arenas and set pieces deliberately hand the player a breather before
	// the next campaign beat. This is a small forward-looking ledger rather
	// than a probabilistic refill roll, and it applies equally to every profile.
	for (unsigned int index = 0; index + 1 < mainRooms.Size(); index++)
	{
		const RoomInfo& source = Rooms[mainRooms[index]];
		const bool needsRecovery = source.hasKey || source.isArena ||
			source.encounterCard == PGEC_SetPiece || source.encounterCard == PGEC_HoldingLine;
		if (!needsRecovery) continue;
		RoomInfo& target = Rooms[mainRooms[index + 1]];
		target.hasHealth = true;
		target.healthType = 2012;
		target.healthCount = std::max(target.healthCount, 2);
		target.hasAmmo = true;
		target.ammoType = LargeAmmoForStage(target);
		target.ammoCount = std::max(target.ammoCount, 1);
		target.recoveryBudget = std::max(target.recoveryBudget, 1);
		target.rewardPlan = std::max(target.rewardPlan, (int)PGRW_Emergency);
	}

	// Deep optional rooms are explicit survival opportunities, not decorative
	// dead ends. Seeded selection favors the far ends of side limbs and grants a
	// recovery bundle substantial enough to justify exploration.
	int shrineRooms = 0;
	for (const RoomInfo& room : Rooms)
		if (room.featureMotif == PGFM_ShrineSecrets) shrineRooms++;
	int survivalCacheBudget = std::max(2, 1 + Size / 3) + (shrineRooms > 0 ? 1 : 0);
	for (int pass = 0; pass < 3 && survivalCacheBudget > 0; pass++)
	{
		for (int index = (int)sideRooms.Size() - 1; index >= 0 && survivalCacheBudget > 0; index--)
		{
			RoomInfo& room = Rooms[sideRooms[index]];
			if (room.hasKey || room.hasExit || room.isLocked) continue;
			if (pass == 0 && (!room.isDeadEnd || room.branchDepth < 2)) continue;
			if (pass == 1 && room.branchDepth < 2) continue;
			if (pass == 2 && room.branchDepth < 1) continue;
			if (room.healthCount >= 2 && room.ammoCount >= 2) continue;
			room.hasHealth = true;
			room.healthType = 2012;
			room.healthCount = std::max(room.healthCount, 2);
			room.healthBonusCount = std::max(room.healthBonusCount, 4);
			room.hasAmmo = true;
			room.ammoType = LargeAmmoForStage(room);
			room.ammoCount = std::max(room.ammoCount, 2);
			room.hasDoor = room.hasDoor || room.isDeadEnd;
			room.rewardPlan = std::max(room.rewardPlan, (int)PGRW_Cache);
			if ((survivalCacheBudget & 1) == 0 && !room.hasArmor)
			{
				room.hasArmor = true;
				room.armorType = 2015;
			}
			survivalCacheBudget--;
		}
	}

	// Turn a few optional dead ends into real Doom-style secrets. Selection is
	// deterministic and favors the deepest side limbs; every secret receives a
	// useful recovery bundle even when weapon progression chose another room.
	int secretBudget = 3 + Size / 3 + (Detail >= 1 ? 1 : 0) + (Detail == 2 ? 1 : 0) +
		(shrineRooms > 0 ? 1 : 0);
	// The terrain pass may reserve one deep optional district as the opposite
	// dramatic horizon.  A secret room is deliberately re-levelled to its sole
	// neighbour by BuildUDMF so its hidden door cannot become a ledge; therefore
	// it cannot also carry that promised, reachable extreme.  Keep the actual
	// terrain anchor explorable as an ordinary optional district instead.
	auto IsOptionalTerrainAnchor = [&](const RoomInfo& room) -> bool
	{
		return room.optionalTerrainAnchor;
	};
	auto MakeSecret = [&](RoomInfo& room)
	{
		room.isSecret = true;
		room.manualInteraction = PGMI_SecretDoor;
		room.hasDoor = true;
		room.hasAmmo = true;
		room.ammoType = AmmoForStage(room);
		room.ammoCount = std::max(room.ammoCount, 2);
		room.hasHealth = true;
		room.healthType = 2012;
		room.healthCount = std::max(room.healthCount, 2);
		room.healthBonusCount = std::max(room.healthBonusCount, 4);
		if (!room.hasArmor)
		{
			room.hasArmor = true;
			room.armorType = 2015;
		}
		room.rewardPlan = std::max(room.rewardPlan, (int)PGRW_Cache);
	};
	for (int index = (int)sideRooms.Size() - 1; index >= 0 && secretBudget > 0; index--)
	{
		RoomInfo& room = Rooms[sideRooms[index]];
		if (!room.reservedSecret || !room.isDeadEnd || room.hasKey ||
			room.hasExit || room.isLocked || IsOptionalTerrainAnchor(room))
			continue;
		MakeSecret(room);
		secretBudget--;
	}
	for (int pass = 0; pass < 3 && secretBudget > 0; pass++)
	{
		for (int index = (int)sideRooms.Size() - 1; index >= 0 && secretBudget > 0; index--)
		{
			RoomInfo& room = Rooms[sideRooms[index]];
			if (room.isSecret || !room.isDeadEnd || room.hasKey || room.hasExit ||
				room.isLocked || IsOptionalTerrainAnchor(room))
				continue;
			if (pass == 0 && room.branchDepth < 2) continue;
			if (pass == 2 && room.branchDepth < 1) continue;
			MakeSecret(room);
			secretBudget--;
		}
	}

	// Secrets are part of the item-progression contract, not merely rooms with
	// extra medikits. Stock Doom powerups arrive in a deliberate order: basic
	// carrying/combat utility first, exploration and defensive rewards deeper in
	// the map, and encounter-skipping artifacts only on large, hard layouts.
	TArray<int> secretRooms;
	for (unsigned int ri = 0; ri < Rooms.Size(); ri++)
		if (Rooms[ri].isSecret) secretRooms.Push(ri);
	SortByDistance(secretRooms);
	auto AddSecretPowerup = [&](int roomIndex, int type)
	{
		if (secretRooms.Size() == 0) return;
		type = ProcGenCompatibleThing(type);
		roomIndex = clamp(roomIndex, 0, (int)secretRooms.Size() - 1);
		RoomInfo& room = Rooms[secretRooms[roomIndex]];
		for (unsigned int item = 0; item < room.powerups.Size(); item++)
			if (room.powerups[item] == type) return;
		room.powerups.Push(type);
	};
	if (secretRooms.Size() > 0)
	{
		AddSecretPowerup(0, 8); // backpack: early optional carrying capacity
		AddSecretPowerup(secretRooms.Size() - 1, 2024); // partial invisibility
		if (Size >= 4)
			AddSecretPowerup(secretRooms.Size() / 2, 2023); // berserk
		if (Size >= 5)
			AddSecretPowerup(secretRooms.Size() - 1, 2013); // soul sphere
		if (Size >= 8)
			AddSecretPowerup(std::max(0, (int)secretRooms.Size() - 2), 2026); // map
		if (Size >= 12 && (themeStyle == ThemeHell || themeStyle == ThemeGothic ||
			themeStyle == ThemeCorrupted))
			AddSecretPowerup(secretRooms.Size() / 2, 2045); // light amplification
		if (Size >= 12 && Difficulty >= 4)
			AddSecretPowerup(secretRooms.Size() - 1, 2022); // invulnerability
		if (doom2Roster && Size >= 20 && Difficulty >= 4)
			AddSecretPowerup(secretRooms.Size() - 1, 83); // Doom II megasphere
	}

	// Large, high-difficulty layouts contain enough individually modest fights
	// that per-room random recovery rolls can under-supply the campaign in
	// aggregate. Guarantee at least one substantial pickup per four authored
	// monsters, first filling combat rooms that received none and only then
	// adding a second pickup to existing caches. This is deterministic and
	// scales with actual encounter pressure rather than the canvas dimensions.
	int totalEnemies = 0;
	int directRecovery = 0;
	for (const RoomInfo& room : Rooms)
	{
		if (room.id < 0) continue;
		totalEnemies += room.enemyCount;
		directRecovery += room.hasHealth ? room.healthCount : 0;
	}
	const int minimumDirectRecovery = (totalEnemies + 3) / 4;
	for (int pass = 0; pass < 3 && directRecovery < minimumDirectRecovery; pass++)
	{
		for (RoomInfo& room : Rooms)
		{
			if (room.id < 0 || room.hasPlayerStart || room.enemyCount <= 0)
				continue;
			if (pass == 0 && (room.hasHealth || (!room.onMainPath && room.enemyCount < 3)))
				continue;
			if (pass == 1 && room.hasHealth)
				continue;
			if (pass == 2 && (!room.hasHealth || room.healthCount >= 3))
				continue;

			if (!room.hasHealth)
			{
				room.hasHealth = true;
				room.healthType = (room.enemyCount >= 4 || room.isArena ||
					room.hasKey || room.hasExit) ? 2012 : 2011;
				room.healthCount = 1;
			}
			else room.healthCount++;
			directRecovery++;
			if (directRecovery >= minimumDirectRecovery) break;
		}
	}

	for (int y = 0; y < H; y++)
	{
		for (int x = 0; x < W; x++)
		{
			ProcGenCell& cell = Grid[y][x];
			if (!cell.present || cell.roomId < 0) continue;
			RoomInfo& room = Rooms[cell.roomId];
			cell.hasWeapon = room.hasWeapon;
			cell.weaponType = room.weaponType;
			cell.hasAmmo = room.hasAmmo;
			cell.ammoType = room.ammoType;
			cell.hasHealth = room.hasHealth;
			cell.healthType = room.healthType;
			cell.hasArmor = room.hasArmor;
			cell.armorType = room.armorType;
			cell.enemyCount = room.enemyCount;
			cell.monsterTier = room.monsterTier;
			cell.runBeat = room.runBeat;
			cell.encounterCard = room.encounterCard;
			cell.featureMotif = room.featureMotif;
			cell.featureMotifPriority = room.featureMotifPriority;
			cell.district = room.district;
			cell.stageShape = room.stageShape;
			cell.landmarkArchetype = room.landmarkArchetype;
			cell.districtRole = room.districtRole;
			cell.verticalIntent = room.verticalIntent;
			cell.verticalRise = room.verticalRise;
			cell.verticalAnchor = room.verticalAnchor;
			cell.threatBudget = room.threatBudget;
			cell.recoveryBudget = room.recoveryBudget;
			cell.optionalArmory = room.optionalArmory;
			cell.arsenalTrack = room.arsenalTrack;
			cell.finaleCard = room.finaleCard;
			cell.rewardPlan = room.rewardPlan;
		}
	}
}
