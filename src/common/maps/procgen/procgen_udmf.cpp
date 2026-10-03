/*
** procgen_udmf.cpp
**
** Robust UDMF emitter for procedural maps. Chambers are inset from the coarse
** grid and joined by explicit corridor sectors. This guarantees that every
** exposed wall is a closed, one-sided boundary and avoids ambiguous two-sided
** "solid" walls between unrelated sectors.
**
**---------------------------------------------------------------------------
*/

#include "procgen_internal.h"
#include "gamedata/gi.h"
#include "gametexture.h"
#include "texturemanager.h"

using namespace ProcGen;

namespace
{
	// Each texture part owns its transform.  UDMF's legacy side-wide offsets are
	// still retained for the deliberately fitted door and switch faces below,
	// but ordinary bands no longer inherit an arbitrary 128-unit phase from an
	// unrelated texture part.
	enum TextureAlignmentMode
	{
		TAM_World,
		TAM_Architectural,
		TAM_Centered,
		TAM_DoorFit,
		TAM_PanelFit,
	};

	struct BuildVertex
	{
		double x = 0.0;
		double y = 0.0;
	};

	struct BuildSector
	{
		double floorZ = 0.0;
		double ceilZ = 128.0;
		FString floorTex;
		FString ceilTex;
		int light = 160;
		int lightColor = 0xffffff;
		int fadeColor = 0;
		int special = 0;
		int id = 0;
		int damageAmount = 0;
		int damageInterval = 32;
		int leakiness = 0;
		FString damageType;
		bool damageTerrainEffect = false;
	};

	struct BuildSide
	{
		int sector = -1;
		FString top;
		FString middle;
		FString bottom;
		// Legacy shared values are used only by stock fitted door/panel art. The
		// normal emitter writes the per-part values below with zero shared pan.
		int offsetX = 0;
		int offsetY = 0;
		double offsetXTop = 0.0;
		double offsetYTop = 0.0;
		double offsetXMid = 0.0;
		double offsetYMid = 0.0;
		double offsetXBottom = 0.0;
		double offsetYBottom = 0.0;
		double scaleXTop = 1.0;
		double scaleYTop = 1.0;
		double scaleXMid = 1.0;
		double scaleYMid = 1.0;
		double scaleXBottom = 1.0;
		double scaleYBottom = 1.0;
		TextureAlignmentMode topAlignment = TAM_World;
		TextureAlignmentMode midAlignment = TAM_World;
		TextureAlignmentMode bottomAlignment = TAM_World;
		// Planned connector runs carry a stable key from the recipe planner.  It
		// is deliberately separate from the sector index: every eight-unit stair
		// tread receives a different sector, but must remain one architectural
		// material run.
		int alignmentGroup = -1;
	};

	struct TextureMetric
	{
		int width = 128;
		int height = 128;
		bool usedFallback = false;
	};

	struct TextureMetricCacheEntry
	{
		FString name;
		TextureMetric metric;
	};

	// Filled while the final transform pass owns both the resolved texture
	// metric and the exact sidedef/line orientation. It is intentionally local
	// to emission; a bounded, JSON-friendly witness is copied to the generator
	// only after every wall has been resolved.
	struct AlignmentSurfaceInfo
	{
		bool resolved = false;
		int line = -1;
		int side = -1;
		int part = 0;
		int mode = TAM_World;
		int width = 0;
		int height = 0;
		int alignmentGroup = -1;
		int64_t phaseOrigin = 0;
		int phaseShift = 0;
		bool reverse = false;
		bool twoSided = false;
		FString texture;
		double offsetX = 0.0;
		double offsetY = 0.0;
		double scaleX = 1.0;
		double scaleY = 1.0;
	};

	struct BuildLine
	{
		int v1 = -1;
		int v2 = -1;
		int sideFront = -1;
		int sideBack = -1;
		bool blocking = false;
		bool dontPegTop = false;
		bool dontPegBottom = false;
		bool playerUse = false;
		bool playerCross = false;
		bool repeatSpecial = false;
		bool blockMonsters = false;
		bool secret = false;
		int special = 0;
		int lockNumber = 0;
		int args[5] = { 0, 0, 0, 0, 0 };
	};

	struct BuildThing
	{
		double x = 0.0;
		double y = 0.0;
		int type = 0;
		int angle = 0;
		bool ambush = false;
		// Encounter ownership is diagnostic-only. It lets the manifest prove
		// that a room card retained real static combat placements after the
		// geometry pass, without serializing any non-UDMF metadata.
		int encounterRoom = -1;
		// Only decorative solid props opt into this field. Pickups and monsters
		// remain actors in the UDMF, but do not participate in the static-pinch
		// proof because they are not map geometry.
		double solidRadius = 0.0;
		// Optional cache rewards retain their realized closet sector long enough
		// for the post-use navigation proof. This is never serialized into UDMF.
		int accessibilitySector = -1;
	};

	// Clearance is authored before props are considered. A reservation is either
	// a route capsule (the segment between two points) or a round interaction
	// pad. Keeping this lightweight geometric representation lets decorative
	// placement protect real player lanes without relying on a later sector-only
	// connectivity audit.
	struct NavigationReservation
	{
		double x1 = 0.0;
		double y1 = 0.0;
		double x2 = 0.0;
		double y2 = 0.0;
		double radius = 0.0;
		bool pad = false;
	};

	// Each non-secret inter-room edge gets a concrete capsule range after its
	// final corridor, door, or stair geometry is emitted.  This is deliberately
	// independent of the coarse layout graph: the later key-state solver may
	// traverse a cell boundary only after this record and its serialized sector
	// links have both been proven.
	struct CollisionCorridorProof
	{
		int sourceCell = -1;
		int targetCell = -1;
		int firstReservation = -1;
		int reservationCount = 0;
	};

	struct ProvenNavigationEdge
	{
		int sourceCell = -1;
		int targetCell = -1;
		int lockType = 0;
	};

	// Fluid is emitted as a late, irregular sector loop. Keep its exact polygon
	// so room-slot offsets can be rejected against the realized basin rather
	// than only against the coarser reserved grid cell.
	struct EmittedFluidFootprint
	{
		TArray<std::pair<double, double>> points;
	};

	struct ConnectionRef
	{
		int sector = -1;
		int doorSector = -1;
		int stairIndex = -1;
		double halfWidth = 48.0;
		// Dogleg stairs enter and leave their connector through opposite
		// 64-unit lanes. Keeping the reduced mouth on the connection reference
		// lets the chamber shell remain responsible for its own solid shoulders.
		bool doglegPortal = false;
		double doglegPortalOffset = 0.0;
		double doglegPortalHalfWidth = 32.0;
		bool door = false;
		bool window = false;
		bool secret = false;
		int lockType = 0;
		double doorHeight = 128.0;
		FString doorTexture;
		int doorTextureWidth = 128;
		int doorTextureHeight = 128;
		int alignmentGroup = -1;
	};

	struct DoorProfile
	{
		const char* texture = "BIGDOOR1";
		int width = 128;
		int height = 96;
	};

	struct CellConnections
	{
		ConnectionRef refs[4];
	};

	struct StairConnection
	{
		// Ordered from the DIR_E / DIR_S source cell toward its neighbor.
		TArray<int> sectors;
		// A dogleg is a genuine route turn: a normal eight-unit flight reaches a
		// high landing, then a short perpendicular landing run reaches an offset
		// outlet. Tight connectors retain the conventional straight staircase.
		bool dogleg = false;
		int flightSteps = 0;
		int landingIndex = -1;
		int outletIndex = -1;
		int side = 1;
		int alignmentGroup = -1;
	};

	struct CellEdgeExtents
	{
		double edge[4] = { 0.0, 0.0, 0.0, 0.0 };
	};

	struct RevealProfile
	{
		double outerX = 0.0;
		double outerY = 0.0;
		double innerX = 0.0;
		double innerY = 0.0;
		double outerChamfer = 0.0;
		double innerChamfer = 0.0;
		double offsetX = 0.0;
		double offsetY = 0.0;
		double floorDelta = 0.0;
		double ceilingDrop = 0.0;
		int variant = 0;
	};

	enum RevealArchitecture
	{
		RevealPavilion,
		RevealWallAlcove,
		RevealFalseWall,
	};

	enum RevealCue
	{
		RevealHidden,
		RevealSubtle,
		RevealProminent,
	};

	enum FluidKind
	{
		FluidWater,
		FluidBlood,
		FluidNukage,
		FluidLava,
	};

	enum FluidArchitecture
	{
		FluidCentralPool,
		FluidTrenchPool,
		FluidPairedPools,
		FluidIrregularPool,
		FluidFloodedGrotto,
		FluidStraightRiver,
		FluidStaggeredRiver,
		FluidBendRiver,
		FluidArchitectureCount,
	};

	struct FluidDescriptor
	{
		int kind = FluidWater;
		int architecture = -1;
		bool primary = false;
		bool bridge = false;
		bool floodedRoom = false;
		int islandX = -1;
		int islandY = -1;
		double islandHalfX = 96.0;
		double islandHalfY = 96.0;
		TArray<std::pair<int, int>> cells;
	};

	static const char* SafeTexture(const FString& texture, const char* fallback)
	{
		return texture.IsEmpty() ? fallback : texture.GetChars();
	}
}

bool FProceduralMapGenerator::BuildUDMF(int W, int H)
{
	static const double CELL_HALF = CELL_SIZE * 0.5;
	static const double WALL_INSET = 24.0;
	static const double ROOM_HALF = CELL_HALF - WALL_INSET;
	const ThemeStyle themeStyle = GetThemeStyle(Theme);
	const RunBlueprint& blueprint = GetRunBlueprint();
	const uint32_t blueprintHash = blueprint.RecipeHash;
	// Keep descriptor selection stable for signed seeds, including INT_MIN,
	// without invoking the undefined overflow of abs(INT_MIN).
	const uint32_t variantSeed = Seed < 0 ? 0u - (uint32_t)Seed : (uint32_t)Seed;
	const int variantSeedMod3 = (int)(variantSeed % 3u);
	const int variantSeedThirdMod3 = (int)((variantSeed / 3u) % 3u);
	const bool infernalArchitecture = UsesInfernalArchitecture(themeStyle);

	TArray<BuildVertex> vertices;
	TArray<BuildSector> sectors;
	TArray<BuildSide> sides;
	TArray<BuildLine> lines;
	TArray<BuildThing> things;
	TArray<NavigationReservation> navigationReservations;
	int navigationPadReservations = 0;
	int navigationCorridorReservations = 0;
	TArray<EmittedFluidFootprint> emittedFluidFootprints;
	bool fluidThingPlacementFailed = false;
	// Resolve dimensions from the active texture manager, not from an assumed
	// stock tile size.  The cache is intentionally per-build: a changed IWAD or
	// texture replacement receives its own logical metrics on the next map.
	TArray<TextureMetricCacheEntry> textureMetricCache;
	int alignmentMetricTextures = 0;
	int alignmentFallbackTextures = 0;
	int alignmentWorldParts = 0;
	int alignmentArchitecturalParts = 0;
	int alignmentCenteredParts = 0;
	bool alignmentTransformsValid = true;
	// Retain emitted remote-cache door sectors for the interaction proof. Each
	// starts closed and must be paired with a reachable manual switch.
	TArray<int> remoteCacheDoorSectors;
	TMap<uint64_t, int> vertexLookup;
	TMap<uint64_t, int> solidWallLookup;
	constexpr double PlayerRadius = 16.0;
	constexpr double NavigationSafety = 16.0;
	// An operable lift is an optional obstacle, so its entire perimeter needs a
	// real alternate walking lane. Keep this separate from its 40-unit moving
	// footprint: later feature and decoration passes must honor the full ring.
	constexpr double LiftPlatformHalf = 40.0;
	constexpr double LiftBypassClearance = 96.0;
	// Fluids are always inset farther than the 64-unit player circulation
	// contract. Keeping this shared across pools and watercourse ends avoids a
	// maximum-size map losing its visible liquid districts to inconsistent
	// conservative margins.
	constexpr double FluidBankClearance = 72.0;

	auto ResolveWallMetric = [&](FString& texture, const char* fallback) -> TextureMetric
	{
		if (texture.IsEmpty() || texture.Compare("-") == 0)
			return TextureMetric();

		auto FindCached = [&](const FString& name, TextureMetric& metric) -> bool
		{
			for (const auto& entry : textureMetricCache)
			{
				if (entry.name.Compare(name) == 0)
				{
					metric = entry.metric;
					return true;
				}
			}
			return false;
		};

		TextureMetric cached;
		if (FindCached(texture, cached)) return cached;

		auto LookupTexture = [&](const FString& name) -> FGameTexture*
		{
			FTextureID textureId = TexMan.CheckForTexture(name.GetChars(),
				ETextureType::Wall, FTextureManager::TEXMAN_TryAny |
				FTextureManager::TEXMAN_ReturnFirst);
			return textureId.isValid() ? TexMan.GetGameTexture(textureId) : nullptr;
		};

		FGameTexture* gameTexture = LookupTexture(texture);
		bool usedFallback = gameTexture == nullptr;
		if (usedFallback)
		{
			texture = fallback;
			if (FindCached(texture, cached)) return cached;
			gameTexture = LookupTexture(texture);
		}

		TextureMetric metric;
		metric.usedFallback = usedFallback;
		if (gameTexture != nullptr)
		{
			metric.width = std::max(1, (int)lround(gameTexture->GetDisplayWidth()));
			metric.height = std::max(1, (int)lround(gameTexture->GetDisplayHeight()));
		}
		else
		{
			// STARTAN3 is present in both supported Doom IWAD families. A final
			// 128-unit metric keeps the UDMF finite if a user texture pack removes
			// even that fallback, while the serialized wall name stays role-safe.
			texture = fallback;
			metric.usedFallback = true;
		}

		TextureMetricCacheEntry entry;
		entry.name = texture;
		entry.metric = metric;
		textureMetricCache.Push(entry);
		alignmentMetricTextures++;
		if (metric.usedFallback) alignmentFallbackTextures++;
		return metric;
	};

	auto ReserveNavigationSegment = [&](double x1, double y1, double x2, double y2,
		double halfWidth)
	{
		NavigationReservation reservation;
		reservation.x1 = x1;
		reservation.y1 = y1;
		reservation.x2 = x2;
		reservation.y2 = y2;
		// Half-width is the lateral lane dimension. Include a player radius and
		// an additional safety band so a solid prop cannot make an aperture feel
		// technically open but physically unusable.
		reservation.radius = std::max(halfWidth, PlayerRadius + NavigationSafety);
		reservation.pad = false;
		navigationReservations.Push(reservation);
		navigationCorridorReservations++;
	};
	auto ReserveNavigationPad = [&](double x, double y, double radius)
	{
		NavigationReservation reservation;
		reservation.x1 = reservation.x2 = x;
		reservation.y1 = reservation.y2 = y;
		reservation.radius = std::max(radius, PlayerRadius + NavigationSafety);
		reservation.pad = true;
		navigationReservations.Push(reservation);
		navigationPadReservations++;
	};
	auto DistanceToReservation = [](const NavigationReservation& reservation,
		double x, double y) -> double
	{
		const double dx = reservation.x2 - reservation.x1;
		const double dy = reservation.y2 - reservation.y1;
		const double lengthSquared = dx * dx + dy * dy;
		if (reservation.pad || lengthSquared < 0.0001)
			return hypot(x - reservation.x1, y - reservation.y1);
		const double projection = clamp(((x - reservation.x1) * dx +
			(y - reservation.y1) * dy) / lengthSquared, 0.0, 1.0);
		const double nearestX = reservation.x1 + dx * projection;
		const double nearestY = reservation.y1 + dy * projection;
		return hypot(x - nearestX, y - nearestY);
	};

	auto AddVertex = [&](double x, double y) -> int
	{
		const int32_t quantizedX = (int32_t)llround(x * 1000.0);
		const int32_t quantizedY = (int32_t)llround(y * 1000.0);
		const uint64_t key = (uint64_t)(uint32_t)quantizedX << 32 |
			(uint64_t)(uint32_t)quantizedY;
		if (int* existing = vertexLookup.CheckKey(key)) return *existing;
		BuildVertex vertex;
		vertex.x = x;
		vertex.y = y;
		vertices.Push(vertex);
		const int index = vertices.Size() - 1;
		vertexLookup.Insert(key, index);
		return index;
	};

	auto AddSector = [&](double floorZ, double ceilZ, const char* floorTex,
		const char* ceilTex, int light, int id = 0) -> int
	{
		BuildSector sector;
		sector.floorZ = floorZ;
		sector.ceilZ = ceilZ;
		sector.floorTex = floorTex;
		sector.ceilTex = ceilTex;
		sector.light = clamp(light, 160, 224);
		sector.id = id;
		sectors.Push(sector);
		return sectors.Size() - 1;
	};

	auto AddSide = [&](int sector, const char* top, const char* middle,
		const char* bottom) -> int
	{
		BuildSide side;
		side.sector = sector;
		side.top = (top && top[0]) ? top : "-";
		side.middle = (middle && middle[0]) ? middle : "-";
		side.bottom = (bottom && bottom[0]) ? bottom : "-";
		sides.Push(side);
		return sides.Size() - 1;
	};


	auto SetSideAlignment = [](BuildSide& side, TextureAlignmentMode mode,
		int alignmentGroup = -1)
	{
		side.topAlignment = mode;
		side.midAlignment = mode;
		side.bottomAlignment = mode;
		side.alignmentGroup = mode == TAM_Architectural ? alignmentGroup : -1;
	};

	auto AddLine = [&](double x1, double y1, double x2, double y2,
		int frontSector, int backSector,
		const char* frontTop, const char* frontMiddle, const char* frontBottom,
		const char* backTop, const char* backMiddle, const char* backBottom,
		bool blocking, int special, int lockNumber,
		int arg0, int arg1, int arg2, int arg3, int arg4,
		bool playerUse, bool playerCross, bool repeatSpecial,
		bool dontPegTop = false, bool dontPegBottom = false,
		bool blockMonsters = false, int architecturalAlignmentGroup = -1) -> int
	{
		if (fabs(x1 - x2) < 0.001 && fabs(y1 - y2) < 0.001) return -1;

		BuildLine line;
		line.v1 = AddVertex(x1, y1);
		line.v2 = AddVertex(x2, y2);
		line.sideFront = AddSide(frontSector, frontTop, frontMiddle, frontBottom);
		// Two-sided geometry is usually a room boundary or portal shoulder.  It
		// stays on the continuous world phase unless an owning connector/stair
		// explicitly supplies its planned architectural run key.
		SetSideAlignment(sides[line.sideFront], architecturalAlignmentGroup >= 0 ?
			TAM_Architectural : TAM_World, architecturalAlignmentGroup);
		if (backSector >= 0)
		{
			line.sideBack = AddSide(backSector, backTop, backMiddle, backBottom);
			SetSideAlignment(sides[line.sideBack], architecturalAlignmentGroup >= 0 ?
				TAM_Architectural : TAM_World, architecturalAlignmentGroup);
		}
		line.blocking = blocking || backSector < 0;
		line.special = special;
		line.lockNumber = lockNumber;
		line.args[0] = arg0;
		line.args[1] = arg1;
		line.args[2] = arg2;
		line.args[3] = arg3;
		line.args[4] = arg4;
		line.playerUse = playerUse;
		line.playerCross = playerCross;
		line.repeatSpecial = repeatSpecial;
		line.blockMonsters = blockMonsters;
		line.dontPegTop = dontPegTop;
		line.dontPegBottom = dontPegBottom;
		lines.Push(line);
		return lines.Size() - 1;
	};

	auto AddWall = [&](double x1, double y1, double x2, double y2,
		int sector, const char* texture, int architecturalAlignmentGroup = -1) -> int
	{
		const int firstVertex = AddVertex(x1, y1);
		const int secondVertex = AddVertex(x2, y2);
		const uint32_t lowVertex = (uint32_t)std::min(firstVertex, secondVertex);
		const uint32_t highVertex = (uint32_t)std::max(firstVertex, secondVertex);
		const uint64_t wallKey = (uint64_t)lowVertex << 32 | highVertex;
		if (int* existingIndex = solidWallLookup.CheckKey(wallKey))
		{
			const int matchedLine = *existingIndex;
			BuildLine& existing = lines[matchedLine];
			const int existingSector = sides[existing.sideFront].sector;
			if (existing.sideBack < 0 && existing.v1 == secondVertex &&
				existing.v2 == firstVertex)
			{
				const int existingGroup = sides[existing.sideFront].alignmentGroup;
				const int mergedGroup = existingGroup >= 0 ? existingGroup :
					architecturalAlignmentGroup;
				const TextureAlignmentMode mergedMode = mergedGroup >= 0 ?
					TAM_Architectural : TAM_World;
				if (existingSector == sector)
				{
					// Two opposite solid faces from the same sector describe an
					// internal chamber/corridor seam, not a wall. Serialize one open
					// two-sided line so the BSP receives an unambiguous partition.
					existing.sideBack = AddSide(sector, nullptr, nullptr, nullptr);
					SetSideAlignment(sides[existing.sideFront], mergedMode, mergedGroup);
					SetSideAlignment(sides[existing.sideBack], mergedMode, mergedGroup);
					sides[existing.sideFront].top = "-";
					sides[existing.sideFront].middle = "-";
					sides[existing.sideFront].bottom = "-";
					existing.blocking = false;
					existing.dontPegBottom = false;
				}
				else
				{
					// Opposite faces owned by different sectors are one shared height
					// boundary. Keeping both one-sided lines creates coincident BSP
					// geometry; merge them into a conventional open two-sided edge.
					FString boundaryTexture = sides[existing.sideFront].middle;
					sides[existing.sideFront].top = boundaryTexture;
					sides[existing.sideFront].middle = "-";
					sides[existing.sideFront].bottom = boundaryTexture;
					existing.sideBack = AddSide(sector,
						boundaryTexture.GetChars(), nullptr, boundaryTexture.GetChars());
					SetSideAlignment(sides[existing.sideFront], mergedMode, mergedGroup);
					SetSideAlignment(sides[existing.sideBack], mergedMode, mergedGroup);
					existing.blocking = false;
					existing.dontPegBottom = true;
				}
				solidWallLookup.Remove(wallKey);
				return matchedLine;
			}
			if (existing.sideBack < 0 && existingSector == sector)
			{
				if (architecturalAlignmentGroup >= 0 &&
					sides[existing.sideFront].alignmentGroup < 0)
					SetSideAlignment(sides[existing.sideFront], TAM_Architectural,
						architecturalAlignmentGroup);
				return matchedLine;
			}
		}
		int lineIndex = AddLine(x1, y1, x2, y2, sector, -1,
			nullptr, texture, nullptr, nullptr, nullptr, nullptr,
			true, 0, 0, 0, 0, 0, 0, 0, false, false, false, false, true,
			architecturalAlignmentGroup);
		if (lineIndex >= 0)
		{
			solidWallLookup.Insert(wallKey, lineIndex);
			// Per-part offsets are assigned in the post-emission alignment pass.
			// A zero shared pan lets pegging anchor middle bands at their local
			// lower floor instead of forcing a global floor-height phase.
		}
		return lineIndex;
	};

	auto AddSwitchWall = [&](double x1, double y1, double x2, double y2,
		int sector, int targetTag) -> int
	{
		ReserveNavigationSegment(x1, y1, x2, y2, 56.0);
		const char* texture = infernalArchitecture ? "SW1GARG" : "SW1COMP";
		int lineIndex = AddLine(x1, y1, x2, y2, sector, -1,
			nullptr, texture, nullptr, nullptr, nullptr, nullptr,
			true, 11, 0, targetTag, 16, 0, 0, 0,
			true, false, false, false, true);
		if (lineIndex >= 0)
		{
			BuildSide& side = sides[lines[lineIndex].sideFront];
			// Defer stock-panel fitting until the active IWAD's real dimensions are
			// known. The panel must remain an isolated, native-size composition
			// rather than inheriting the surrounding wall's world phase.
			side.midAlignment = TAM_PanelFit;
		}
		return lineIndex;
	};

		auto PointInEmittedFluid = [&](double x, double y) -> bool
	{
		for (const EmittedFluidFootprint& footprint : emittedFluidFootprints)
		{
			if (footprint.points.Size() < 3) continue;
			bool inside = false;
			for (unsigned int point = 0; point < footprint.points.Size(); ++point)
			{
				const auto& first = footprint.points[point];
				const auto& second = footprint.points[(point + 1) % footprint.points.Size()];
				if ((first.second > y) == (second.second > y)) continue;
				const double crossing = first.first + (y - first.second) *
					(second.first - first.first) / (second.second - first.second);
				if (x < crossing) inside = !inside;
			}
			if (inside) return true;
		}
		return false;
	};

	auto MoveThingOutOfEmittedFluid = [&](double& x, double& y) -> bool
	{
		// A real fluid bank retains at least 72 units of dry floor. Move an
		// offset slot across its nearest bank by only a player-width margin,
		// retaining the chosen room, encounter, and reward rather than dropping it.
		const double escapeDistance = PlayerRadius + NavigationSafety + 4.0;
		for (unsigned int retry = 0; retry <= emittedFluidFootprints.Size(); ++retry)
		{
			if (!PointInEmittedFluid(x, y)) return true;
			double bestDistanceSquared = 1.0e30;
			double bestX = x;
			double bestY = y;
			for (const EmittedFluidFootprint& footprint : emittedFluidFootprints)
			{
				if (footprint.points.Size() < 3) continue;
				for (unsigned int point = 0; point < footprint.points.Size(); ++point)
				{
					const auto& first = footprint.points[point];
					const auto& second = footprint.points[(point + 1) % footprint.points.Size()];
					const double dx = second.first - first.first;
					const double dy = second.second - first.second;
					const double lengthSquared = dx * dx + dy * dy;
					if (lengthSquared <= 0.001) continue;
					const double fraction = clamp(((x - first.first) * dx +
						(y - first.second) * dy) / lengthSquared, 0.0, 1.0);
					const double nearX = first.first + dx * fraction;
					const double nearY = first.second + dy * fraction;
					const double length = sqrt(lengthSquared);
					const double normalX = -dy / length;
					const double normalY = dx / length;
					for (double side : { -1.0, 1.0 })
					{
						const double candidateX = nearX + normalX * side * escapeDistance;
						const double candidateY = nearY + normalY * side * escapeDistance;
						if (PointInEmittedFluid(candidateX, candidateY)) continue;
						const double candidateDistanceSquared =
							(candidateX - x) * (candidateX - x) +
							(candidateY - y) * (candidateY - y);
						if (candidateDistanceSquared < bestDistanceSquared)
						{
							bestDistanceSquared = candidateDistanceSquared;
							bestX = candidateX;
							bestY = candidateY;
						}
					}
				}
			}
			if (bestDistanceSquared >= 1.0e29) return false;
			x = bestX;
			y = bestY;
		}
		return !PointInEmittedFluid(x, y);
	};

	auto AddThing = [&](double x, double y, int type, int angle = 0, bool ambush = false,
		int encounterRoom = -1, int accessibilitySector = -1) -> bool
	{
		// Keep every static emission legal for the active stock IWAD. The
		// planner selects family-specific tables first; this final normalization
		// protects optional caches, decorations, and future cards as well.
		type = ProcGenCompatibleThing(type);
		if (!MoveThingOutOfEmittedFluid(x, y))
		{
			fluidThingPlacementFailed = true;
			return false;
		}
		BuildThing thing;
		thing.x = x;
		thing.y = y;
		thing.type = type;
		thing.angle = angle;
		thing.ambush = ambush;
		thing.encounterRoom = encounterRoom;
		thing.accessibilitySector = accessibilitySector;
		things.Push(thing);
		return true;
	};

	auto IsValidRoom = [&](int roomId) -> bool
	{
		return roomId >= 0 && roomId < (int)Rooms.Size() && Rooms[roomId].id >= 0;
	};
	int layoutMinX = W;
	int layoutMaxX = -1;
	int layoutMinY = H;
	int layoutMaxY = -1;
	for (int y = 0; y < H; y++)
	{
		for (int x = 0; x < W; x++)
		{
			if (!Grid[y][x].present) continue;
			layoutMinX = std::min(layoutMinX, x);
			layoutMaxX = std::max(layoutMaxX, x);
			layoutMinY = std::min(layoutMinY, y);
			layoutMaxY = std::max(layoutMaxY, y);
		}
	}
	const double layoutCenterX = layoutMaxX >= layoutMinX ?
		(layoutMinX + layoutMaxX + 1) * 0.5 : W * 0.5;
	const double layoutCenterY = layoutMaxY >= layoutMinY ?
		(layoutMinY + layoutMaxY + 1) * 0.5 : H * 0.5;
	// Keep the coarse planner integer-based, but do not serialize it as a perfect
	// drafting grid. Alternating 368/400-unit module gaps preserve the 16-unit
	// door bay and the huge-map coordinate bound while breaking the repeated
	// 384-unit automap cadence. The offsets are derived directly from the seed so
	// they do not perturb descriptor-selection RNG order.
	TArray<double> columnShift;
	TArray<double> rowShift;
	columnShift.Resize(W);
	rowShift.Resize(H);
	auto AxisShift = [&](int coordinate, uint32_t salt) -> double
	{
		uint32_t value = variantSeed ^ salt ^ ((uint32_t)coordinate * 0x9e3779b9u);
		value ^= value >> 16;
		value *= 0x7feb352du;
		value ^= value >> 15;
		return (value & 1u) ? 16.0 : 0.0;
	};
	for (int x = 0; x < W; x++) columnShift[x] = AxisShift(x, 0x51ed270bu);
	for (int y = 0; y < H; y++) rowShift[y] = AxisShift(y, 0x68bc21ebu);
	const double centerShiftX = layoutMaxX >= layoutMinX ?
		(columnShift[layoutMinX] + columnShift[layoutMaxX]) * 0.5 : 0.0;
	const double centerShiftY = layoutMaxY >= layoutMinY ?
		(rowShift[layoutMinY] + rowShift[layoutMaxY]) * 0.5 : 0.0;

	auto CellCenterX = [&](int x) -> double
	{
		return ((x + 0.5) - layoutCenterX) * CELL_SIZE +
			columnShift[x] - centerShiftX;
	};
	auto CellCenterY = [&](int y) -> double
	{
		return ((y + 0.5) - layoutCenterY) * CELL_SIZE +
			rowShift[y] - centerShiftY;
	};

	// Room profiles are assigned during the coherence pass. Cells in the same
	// room share a profile so their joins remain exact, while separate rooms can
	// vary substantially in width, depth, corner treatment, and vertical scale.
	TArray<double> roomHalfX;
	TArray<double> roomHalfY;
	roomHalfX.Resize(Rooms.Size());
	roomHalfY.Resize(Rooms.Size());
	// A source panel is emitted while the chamber shell is built, whereas a
	// false-wall target can still lose its last legal wall face during that same
	// shell pass.  Optional cache fallback must retire an already-emitted panel
	// as well as its plan; otherwise its Door_Open line points at a sector which
	// deliberately was never serialized.  Keep the harmless wall segment, but
	// make it an ordinary non-interactive architectural panel.
	auto DisableSerializedSwitchesForTag = [&](int targetTag)
	{
		if (targetTag <= 0) return;
		for (unsigned int lineIndex = 0; lineIndex < lines.Size(); ++lineIndex)
		{
			BuildLine& line = lines[lineIndex];
			if (line.special != 11 || line.args[0] != targetTag) continue;
			FString adjacentWallTexture;
			if (line.sideFront >= 0 && line.sideFront < (int)sides.Size())
			{
				const int hostSector = sides[line.sideFront].sector;
				// The switch was inserted by splitting an ordinary chamber wall
				// into shoulder/panel/shoulder segments. Reuse either shoulder's
				// material when retiring the panel, preserving one continuous
				// architectural run instead of leaving a non-functional SW1 decal.
				for (unsigned int candidateIndex = 0;
					candidateIndex < lines.Size() && adjacentWallTexture.IsEmpty();
					++candidateIndex)
				{
					if (candidateIndex == lineIndex) continue;
					const BuildLine& candidate = lines[candidateIndex];
					if (candidate.sideBack >= 0 || candidate.special != 0 ||
						candidate.sideFront < 0 || candidate.sideFront >= (int)sides.Size() ||
						sides[candidate.sideFront].sector != hostSector)
						continue;
					const bool sharesEndpoint = candidate.v1 == line.v1 ||
						candidate.v1 == line.v2 || candidate.v2 == line.v1 ||
						candidate.v2 == line.v2;
					const FString& texture = sides[candidate.sideFront].middle;
					if (sharesEndpoint && texture.Compare("-") != 0)
						adjacentWallTexture = texture;
				}
			}
			line.special = 0;
			line.playerUse = false;
			line.playerCross = false;
			line.repeatSpecial = false;
			for (int& arg : line.args) arg = 0;
			if (!adjacentWallTexture.IsEmpty() && line.sideFront >= 0 &&
				line.sideFront < (int)sides.Size())
			{
				BuildSide& side = sides[line.sideFront];
				side.middle = adjacentWallTexture;
				side.midAlignment = side.alignmentGroup >= 0 ?
					TAM_Architectural : TAM_World;
				side.offsetXMid = side.offsetYMid = 0.0;
				side.scaleXMid = side.scaleYMid = 1.0;
			}
		}
	};
	for (unsigned int ri = 0; ri < Rooms.Size(); ri++)
	{
		const RoomInfo& room = Rooms[ri];
		roomHalfX[ri] = clamp(room.halfWidth, 72.0, CELL_HALF - 8.0);
		roomHalfY[ri] = clamp(room.halfHeight, 72.0, CELL_HALF - 8.0);
	}

	auto HalfXForCell = [&](int x, int y) -> double
	{
		int room = Grid[y][x].roomId;
		return IsValidRoom(room) ? roomHalfX[room] : ROOM_HALF;
	};
	auto HalfYForCell = [&](int x, int y) -> double
	{
		int room = Grid[y][x].roomId;
		return IsValidRoom(room) ? roomHalfY[room] : ROOM_HALF;
	};

	// Secret rewards are selected after the general elevation pass. They use a
	// conventional hidden door and are deliberately dead ends, so inherit the
	// sole neighboring terrace rather than placing an impassable step under the
	// moving slab.
	for (unsigned int ri = 0; ri < Rooms.Size(); ri++)
	{
		RoomInfo& secret = Rooms[ri];
		if (!secret.isSecret || !secret.isDeadEnd) continue;
		bool aligned = false;
		for (int y = 0; y < H && !aligned; y++)
		{
			for (int x = 0; x < W && !aligned; x++)
			{
				if (!Grid[y][x].present || Grid[y][x].roomId != (int)ri) continue;
				for (int direction = 0; direction < 4; direction++)
				{
					if (!Grid[y][x].conn[direction]) continue;
					const int nx = x + DX[direction];
					const int ny = y + DY[direction];
					if (nx < 0 || nx >= W || ny < 0 || ny >= H ||
						!Grid[ny][nx].present) continue;
					const int neighborId = Grid[ny][nx].roomId;
					if (!IsValidRoom(neighborId) || neighborId == (int)ri) continue;
					const double clearHeight = secret.ceilZ - secret.floorZ;
					secret.floorZ = Rooms[neighborId].floorZ;
					secret.ceilZ = secret.floorZ + clearHeight;
					aligned = true;
					break;
				}
			}
		}
		// Room floors were copied into the coarse cells before the optional
		// secret was normalized to its single adjacent terrace. Keep that
		// serialized-navigation representation in lockstep with the sectors we
		// are about to emit: otherwise the final visual proof can mistake an old
		// secret floor for an impossible >64-unit walking transition even though
		// the actual hidden door and room are level.
		if (aligned)
		{
			for (int cellY = 0; cellY < H; ++cellY)
			{
				for (int cellX = 0; cellX < W; ++cellX)
				{
					ProcGenCell& cell = Grid[cellY][cellX];
					if (!cell.present || cell.roomId != (int)ri) continue;
					cell.floorZ = secret.floorZ;
					cell.ceilZ = secret.ceilZ;
				}
			}
		}
	}

	// The core planner binds dogleg intents to an ordinary, consecutive pair of
	// main-route cells. Resolve that exact edge here instead of inferring from an
	// arbitrary floor difference: branches may use terraces too, but only a
	// planned dogleg is entitled to offset its two portal mouths.
	auto DoglegAnchorForCells = [&](int x, int y, int nx, int ny) -> int
	{
		if (x < 0 || x >= W || y < 0 || y >= H ||
			nx < 0 || nx >= W || ny < 0 || ny >= H)
			return -1;
		const ProcGenCell& first = Grid[y][x];
		const ProcGenCell& second = Grid[ny][nx];
		auto IsDoglegAnchor = [](const ProcGenCell& anchor,
			const ProcGenCell& next) -> bool
		{
			return anchor.verticalAnchor && anchor.onMainPath && next.onMainPath &&
				(anchor.verticalIntent == PGVI_DoglegAscent ||
					anchor.verticalIntent == PGVI_DoglegDescent) &&
				anchor.verticalRise != 0 && anchor.lockStage == next.lockStage &&
				next.pathRank == anchor.pathRank + 1;
		};
		if (IsDoglegAnchor(first, second) && IsValidRoom(first.roomId) &&
			first.roomId != second.roomId)
			return first.roomId;
		if (IsDoglegAnchor(second, first) && IsValidRoom(second.roomId) &&
			first.roomId != second.roomId)
			return second.roomId;
		return -1;
	};

	// Directional extents shorten only the two chamber faces involved in a
	// vertical transition. A planned dogleg gets a deeper connector than a
	// straight stair: it needs a 64-unit landing and an offset outlet after the
	// full eight-unit flight, while all unrelated faces retain their variation.
	TArray<TArray<CellEdgeExtents>> cellEdges;
	cellEdges.Resize(H);
	for (int y = 0; y < H; y++)
	{
		cellEdges[y].Resize(W);
		for (int x = 0; x < W; x++)
		{
			const int roomId = Grid[y][x].roomId;
			const bool validRoom = Grid[y][x].present && IsValidRoom(roomId);
			const bool protectedRole = validRoom &&
				(Rooms[roomId].hasPlayerStart || Rooms[roomId].hasKey ||
				 Rooms[roomId].hasExit || Rooms[roomId].hasBoss || Rooms[roomId].isLocked);
			const int shapeHash = abs(x * 97 + y * 193 + roomId * 53 +
				(validRoom ? Rooms[roomId].visualVariant * 29 : 0));
			for (int direction = 0; direction < 4; direction++)
			{
				const double base = direction == DIR_N || direction == DIR_S ?
					HalfYForCell(x, y) : HalfXForCell(x, y);
				// Independent face depths move each chamber off the center of its
				// coarse module and create narrow naves, broad courts, wedges, and
				// asymmetric bays. Important progression rooms stay generous.
				const int sample = (shapeHash / (direction * 11 + 1)) % 4;
				double variation = sample * 8.0; // 0 .. +24
				if (themeStyle == ThemeHell || themeStyle == ThemeCorrupted)
					variation += ((shapeHash >> (direction + 2)) & 1) ? 8.0 : 0.0;
				if (protectedRole) variation = std::min(8.0, variation);
				// Feature/item placement uses the authored room half-size as its safe
				// interior. Faces may grow asymmetrically into the connector bay, but
				// never shrink inside that contract and collide with later geometry.
				cellEdges[y][x].edge[direction] = clamp(base + variation,
					base, CELL_HALF - 16.0);
			}
		}
	}
	// A face may grow into its connector bay for asymmetry, but opposing growth
	// must retain 32 units between adjacent chamber shells. This is especially
	// important in a shortened 368-unit module, where two unconstrained 176-unit
	// faces would otherwise collapse a door approach to zero area.
	for (int y = 0; y < H; y++)
	{
		for (int x = 0; x < W; x++)
		{
			if (!Grid[y][x].present) continue;
			for (int direction : { DIR_E, DIR_S })
			{
				const int nx = x + DX[direction];
				const int ny = y + DY[direction];
				if (nx < 0 || nx >= W || ny < 0 || ny >= H || !Grid[ny][nx].present)
					continue;
				const double distance = direction == DIR_E ?
					CellCenterX(nx) - CellCenterX(x) : CellCenterY(ny) - CellCenterY(y);
				double& first = cellEdges[y][x].edge[direction];
				double& second = cellEdges[ny][nx].edge[OPP[direction]];
				const double maximumCombined = distance - 32.0;
				if (first + second <= maximumCombined + 0.001) continue;
				const double excess = first + second - maximumCombined;
				const double firstReduction = std::min(excess * 0.5,
					first - (direction == DIR_E ? HalfXForCell(x, y) : HalfYForCell(x, y)));
				first -= std::max(0.0, firstReduction);
				second -= std::max(0.0, first + second - maximumCombined);
			}
		}
	}
	// The graph planner's connector contract is physical, not just manifest
	// decoration.  Large landmark mouths reserve a deeper bay by pulling their
	// two facing chamber walls back symmetrically.  We never steal a portal span
	// or a critical pad: if the two shells cannot yield the requested depth, the
	// edge is deterministically realized as the next smaller safe profile below.
	// Same-room joins intentionally remain a continuous room-owned contour and
	// therefore do not need a freestanding corridor bay.
	auto ConnectionWidthForProfile = [](EProcGenConnectionProfile profile) -> int
	{
		switch (profile)
		{
		case PGCP_Narrow: return 96;
		case PGCP_Gallery: return 176;
		case PGCP_Grand: return 224;
		default: return 128;
		}
	};
	auto ConnectionDepthForProfile = [](EProcGenConnectionProfile profile) -> int
	{
		switch (profile)
		{
		case PGCP_Narrow: return 48;
		case PGCP_Gallery: return 96;
		case PGCP_Grand: return 128;
		default: return 64;
		}
	};
	auto ProfileForContract = [](int width, int depth, bool mandatory) -> EProcGenConnectionProfile
	{
		if (width >= 224 && depth >= 128) return PGCP_Grand;
		if (width >= 176 && depth >= 96) return PGCP_Gallery;
		if (width >= 128 && depth >= 64) return PGCP_Standard;
		return mandatory ? PGCP_Standard : PGCP_Narrow;
	};
	auto RealizeConnectionContract = [&](ProcGenCell& first, int direction,
		ProcGenCell& second, int opposite, int width, int depth,
		bool mandatory)
	{
		EProcGenConnectionProfile realized = ProfileForContract(width, depth, mandatory);
		// The only legal Narrow connection is a deep optional branch.  If a
		// profile has become too tight for Standard, the cell-edge reservation
		// pass below must make room rather than silently making required travel
		// feel like a crawlspace.
		if (mandatory && realized == PGCP_Narrow) realized = PGCP_Standard;
		// A profile is a concrete geometry band, not an aspirational label. Once
		// an oversized gallery has fallen back, publish and emit the canonical
		// safe Standard/Narrow dimensions instead of a misleading hybrid.
		width = ConnectionWidthForProfile(realized);
		depth = ConnectionDepthForProfile(realized);
		first.connectionProfile[direction] = realized;
		second.connectionProfile[opposite] = realized;
		first.connectionClearWidth[direction] = width;
		second.connectionClearWidth[opposite] = width;
		first.connectionDepth[direction] = depth;
		second.connectionDepth[opposite] = depth;
	};
	for (int y = 0; y < H; y++)
	{
		for (int x = 0; x < W; x++)
		{
			if (!Grid[y][x].present) continue;
			for (int direction : { DIR_E, DIR_S })
			{
				if (!Grid[y][x].conn[direction]) continue;
				const int nx = x + DX[direction];
				const int ny = y + DY[direction];
				if (nx < 0 || nx >= W || ny < 0 || ny >= H ||
					!Grid[ny][nx].present) continue;
				ProcGenCell& firstCell = Grid[y][x];
				ProcGenCell& secondCell = Grid[ny][nx];
				const int opposite = OPP[direction];
				const int firstRoom = firstCell.roomId;
				const int secondRoom = secondCell.roomId;
				if (!IsValidRoom(firstRoom) || !IsValidRoom(secondRoom) ||
					firstRoom == secondRoom)
					continue;

				const bool keyed = firstCell.lockStage != secondCell.lockStage ||
					(firstCell.isLocked &&
						(firstCell.lockDir < 0 || firstCell.lockDir == direction)) ||
					(secondCell.isLocked &&
						(secondCell.lockDir < 0 || secondCell.lockDir == opposite));
				const bool stair = firstCell.connectionStairChain[direction] >= 0 ||
					secondCell.connectionStairChain[opposite] >= 0 ||
					fabs(firstCell.floorZ - secondCell.floorZ) > 0.001;
				const bool mandatory = keyed || stair ||
					(firstCell.onMainPath && secondCell.onMainPath) ||
					firstCell.hasPlayerStart || firstCell.hasKey || firstCell.hasExit ||
					secondCell.hasPlayerStart || secondCell.hasKey || secondCell.hasExit;
				int targetWidth = std::min(firstCell.connectionClearWidth[direction],
					secondCell.connectionClearWidth[opposite]);
				int targetDepth = std::min(firstCell.connectionDepth[direction],
					secondCell.connectionDepth[opposite]);
				if (mandatory)
				{
					targetWidth = std::max(targetWidth, 128);
					targetDepth = std::max(targetDepth, 64);
				}
				const double centerDistance = direction == DIR_E ?
					CellCenterX(nx) - CellCenterX(x) : CellCenterY(ny) - CellCenterY(y);
				double& firstFace = cellEdges[y][x].edge[direction];
				double& secondFace = cellEdges[ny][nx].edge[opposite];
				const bool firstCritical = firstCell.hasPlayerStart || firstCell.hasKey ||
					firstCell.hasExit || firstCell.isLocked;
				const bool secondCritical = secondCell.hasPlayerStart || secondCell.hasKey ||
					secondCell.hasExit || secondCell.isLocked;
				// The player start is a true staging terrace, not just an actor pad.
				// Keep its chamber face at least 160 units from the start center even
				// when a neighboring connector asks for more depth. With a stock
				// 64-unit portal this leaves the portal shoulders outside the start's
				// collision-clear radius as well, instead of creating a technically
				// open but visually cramped first doorway.
				const double firstMinimum = firstCell.hasPlayerStart ? 160.0 :
					(firstCritical ? 112.0 : 72.0);
				const double secondMinimum = secondCell.hasPlayerStart ? 160.0 :
					(secondCritical ? 112.0 : 72.0);
				double availableDepth = centerDistance - firstFace - secondFace;
				if (availableDepth + 0.001 < targetDepth)
				{
					double remaining = targetDepth - availableDepth;
					const double firstGive = std::min(std::max(0.0, firstFace - firstMinimum),
						remaining * 0.5);
					firstFace -= firstGive;
					remaining -= firstGive;
					const double secondGive = std::min(std::max(0.0, secondFace - secondMinimum),
						remaining);
					secondFace -= secondGive;
					remaining -= secondGive;
					if (remaining > 0.001)
					{
						const double extraFirst = std::min(std::max(0.0, firstFace - firstMinimum),
							remaining);
						firstFace -= extraFirst;
					}
					availableDepth = centerDistance - firstFace - secondFace;
				}
				const int realizedDepth = std::max(mandatory ? 64 : 48,
					(int)floor(std::max(0.0, availableDepth) / 8.0) * 8);
				RealizeConnectionContract(firstCell, direction, secondCell, opposite,
					targetWidth, realizedDepth, mandatory);
			}
		}
	}
	// A 128-unit mandatory door must retain shoulders even when one endpoint is
	// an organically narrow chamber. Widen only the two aperture axes involved
	// in progression-stage crossings; unrelated faces keep their variation.
	static const double MIN_LOCK_APERTURE_EDGE = 128.0;
	for (int y = 0; y < H; y++)
	{
		for (int x = 0; x < W; x++)
		{
			if (!Grid[y][x].present) continue;
			for (int direction : { DIR_E, DIR_S })
			{
				if (!Grid[y][x].conn[direction]) continue;
				const int nx = x + DX[direction];
				const int ny = y + DY[direction];
				if (nx < 0 || nx >= W || ny < 0 || ny >= H || !Grid[ny][nx].present)
					continue;
				if (Grid[y][x].lockStage == Grid[ny][nx].lockStage) continue;
				const int firstAxisA = direction == DIR_E ? DIR_N : DIR_W;
				const int firstAxisB = direction == DIR_E ? DIR_S : DIR_E;
				cellEdges[y][x].edge[firstAxisA] = std::max(
					cellEdges[y][x].edge[firstAxisA], MIN_LOCK_APERTURE_EDGE);
				cellEdges[y][x].edge[firstAxisB] = std::max(
					cellEdges[y][x].edge[firstAxisB], MIN_LOCK_APERTURE_EDGE);
				cellEdges[ny][nx].edge[firstAxisA] = std::max(
					cellEdges[ny][nx].edge[firstAxisA], MIN_LOCK_APERTURE_EDGE);
				cellEdges[ny][nx].edge[firstAxisB] = std::max(
					cellEdges[ny][nx].edge[firstAxisB], MIN_LOCK_APERTURE_EDGE);
			}
		}
	}
	// Reserve the transverse span before portal emission.  A wide gallery or
	// grand entrance is allowed to claim more of a landmark face, but never at
	// the expense of a keyed jamb or a stair mouth.  The extra corner allowance
	// is what keeps the final clear opening honest after the room-owned contour
	// removes its diagonal tips.
	for (int y = 0; y < H; y++)
	{
		for (int x = 0; x < W; x++)
		{
			if (!Grid[y][x].present) continue;
			for (int direction : { DIR_E, DIR_S })
			{
				if (!Grid[y][x].conn[direction]) continue;
				const int nx = x + DX[direction];
				const int ny = y + DY[direction];
				if (nx < 0 || nx >= W || ny < 0 || ny >= H ||
					!Grid[ny][nx].present) continue;
				const int roomA = Grid[y][x].roomId;
				const int roomB = Grid[ny][nx].roomId;
				if (!IsValidRoom(roomA) || !IsValidRoom(roomB) || roomA == roomB)
					continue;
				const int opposite = OPP[direction];
				const bool mandatory = Grid[y][x].lockStage != Grid[ny][nx].lockStage ||
					(Grid[y][x].isLocked &&
						(Grid[y][x].lockDir < 0 || Grid[y][x].lockDir == direction)) ||
					(Grid[ny][nx].isLocked &&
						(Grid[ny][nx].lockDir < 0 || Grid[ny][nx].lockDir == opposite)) ||
					Grid[y][x].connectionStairChain[direction] >= 0 ||
					Grid[ny][nx].connectionStairChain[opposite] >= 0 ||
					(Grid[y][x].onMainPath && Grid[ny][nx].onMainPath);
				const int clearWidth = std::max(mandatory ? 128 : 96,
					std::min(Grid[y][x].connectionClearWidth[direction],
						Grid[ny][nx].connectionClearWidth[opposite]));
				const double cornerAllowance = std::max(Rooms[roomA].cornerCut,
					Rooms[roomB].cornerCut);
				const double requiredEdge = std::min(CELL_HALF - 16.0,
					clearWidth * 0.5 + cornerAllowance + 4.0);
				const int firstAxisA = direction == DIR_E ? DIR_N : DIR_W;
				const int firstAxisB = direction == DIR_E ? DIR_S : DIR_E;
				cellEdges[y][x].edge[firstAxisA] = std::max(
					cellEdges[y][x].edge[firstAxisA], requiredEdge);
				cellEdges[y][x].edge[firstAxisB] = std::max(
					cellEdges[y][x].edge[firstAxisB], requiredEdge);
				cellEdges[ny][nx].edge[firstAxisA] = std::max(
					cellEdges[ny][nx].edge[firstAxisA], requiredEdge);
				cellEdges[ny][nx].edge[firstAxisB] = std::max(
					cellEdges[ny][nx].edge[firstAxisB], requiredEdge);
			}
		}
	}
	// Dogleg mouths use a conventional 64-unit half-aperture at both chambers.
	// Their chamfers are allowed to be more expressive than keyed-door jambs, so
	// reserve the full 128-unit transverse face before later aperture clamping.
	// Otherwise a perfectly long connector can be forced into a straight stair
	// solely because a decorative corner treatment consumed the offset lane.
	static const double MIN_DOGLEG_APERTURE_EDGE = 128.0;
	for (int y = 0; y < H; y++)
	{
		for (int x = 0; x < W; x++)
		{
			if (!Grid[y][x].present) continue;
			for (int direction : { DIR_E, DIR_S })
			{
				if (!Grid[y][x].conn[direction]) continue;
				const int nx = x + DX[direction];
				const int ny = y + DY[direction];
				if (nx < 0 || nx >= W || ny < 0 || ny >= H ||
					!Grid[ny][nx].present ||
					DoglegAnchorForCells(x, y, nx, ny) < 0)
					continue;
				const int firstAxisA = direction == DIR_E ? DIR_N : DIR_W;
				const int firstAxisB = direction == DIR_E ? DIR_S : DIR_E;
				cellEdges[y][x].edge[firstAxisA] = std::max(
					cellEdges[y][x].edge[firstAxisA], MIN_DOGLEG_APERTURE_EDGE);
				cellEdges[y][x].edge[firstAxisB] = std::max(
					cellEdges[y][x].edge[firstAxisB], MIN_DOGLEG_APERTURE_EDGE);
				cellEdges[ny][nx].edge[firstAxisA] = std::max(
					cellEdges[ny][nx].edge[firstAxisA], MIN_DOGLEG_APERTURE_EDGE);
				cellEdges[ny][nx].edge[firstAxisB] = std::max(
					cellEdges[ny][nx].edge[firstAxisB], MIN_DOGLEG_APERTURE_EDGE);
			}
		}
	}
	static const double STAIR_ROOM_EDGE = 136.0;
	static const double DOGLEG_ROOM_EDGE = 80.0;
	for (int y = 0; y < H; y++)
	{
		for (int x = 0; x < W; x++)
		{
			if (!Grid[y][x].present) continue;
			for (int direction : { DIR_E, DIR_S })
			{
				if (!Grid[y][x].conn[direction]) continue;
				const int nx = x + DX[direction];
				const int ny = y + DY[direction];
				if (nx < 0 || nx >= W || ny < 0 || ny >= H ||
					!Grid[ny][nx].present) continue;
				const int roomA = Grid[y][x].roomId;
				const int roomB = Grid[ny][nx].roomId;
				if (!IsValidRoom(roomA) || !IsValidRoom(roomB) || roomA == roomB ||
					fabs(Rooms[roomA].floorZ - Rooms[roomB].floorZ) < 0.001)
					continue;
				const double stairRoomEdge = DoglegAnchorForCells(x, y, nx, ny) >= 0 ?
					DOGLEG_ROOM_EDGE : STAIR_ROOM_EDGE;
				cellEdges[y][x].edge[direction] = std::min(
					cellEdges[y][x].edge[direction], stairRoomEdge);
				cellEdges[ny][nx].edge[OPP[direction]] = std::min(
					cellEdges[ny][nx].edge[OPP[direction]], stairRoomEdge);
			}
		}
	}
	// The preceding passes reserve individual faces for portals, dogleg mouths,
	// and landmark shoulders.  Reconcile those independent reservations here so
	// a late transverse expansion cannot leave a connector labelled Standard or
	// Gallery while its two chamber faces are a few units too close together.
	// Reducing a facing shell only makes the corridor deeper; it never consumes a
	// pad, doorway jamb, or walking span.  A dramatic profile deterministically
	// falls back to Standard before we would reject an otherwise playable map.
	for (int y = 0; y < H; ++y)
	{
		for (int x = 0; x < W; ++x)
		{
			if (!Grid[y][x].present) continue;
			for (int direction : { DIR_E, DIR_S })
			{
				if (!Grid[y][x].conn[direction]) continue;
				const int nx = x + DX[direction];
				const int ny = y + DY[direction];
				if (nx < 0 || nx >= W || ny < 0 || ny >= H ||
					!Grid[ny][nx].present) continue;
				ProcGenCell& first = Grid[y][x];
				ProcGenCell& second = Grid[ny][nx];
				const int opposite = OPP[direction];
				if (first.roomId == second.roomId) continue;
				const bool keyed = first.lockStage != second.lockStage ||
					(first.isLocked && (first.lockDir < 0 || first.lockDir == direction)) ||
					(second.isLocked && (second.lockDir < 0 || second.lockDir == opposite));
				const bool stair = first.connectionStairChain[direction] >= 0 ||
					second.connectionStairChain[opposite] >= 0 ||
					fabs(first.floorZ - second.floorZ) > 0.001;
				const bool mandatory = keyed || stair ||
					(first.onMainPath && second.onMainPath) || first.hasPlayerStart ||
					first.hasKey || first.hasExit || second.hasPlayerStart ||
					second.hasKey || second.hasExit;
				EProcGenConnectionProfile profile =
					(EProcGenConnectionProfile)first.connectionProfile[direction];
				int requiredDepth = std::max(mandatory ? 64 : 48,
					std::min(first.connectionDepth[direction],
						second.connectionDepth[opposite]));
				const double centerDistance = direction == DIR_E ?
					CellCenterX(nx) - CellCenterX(x) : CellCenterY(ny) - CellCenterY(y);
				double& firstFace = cellEdges[y][x].edge[direction];
				double& secondFace = cellEdges[ny][nx].edge[opposite];
				auto ReleaseDepth = [](double& face, double minimum, double& needed)
				{
					const double released = std::min(std::max(0.0, face - minimum), needed);
					face -= released;
					needed -= released;
				};
				auto MakeDepth = [&](int depth) -> bool
				{
					double available = centerDistance - firstFace - secondFace;
					double needed = depth - available;
					if (needed <= 0.001) return true;
					const double firstMinimum = first.hasPlayerStart ? 160.0 :
						((first.hasKey || first.hasExit || first.isLocked) ? 112.0 : 72.0);
					const double secondMinimum = second.hasPlayerStart ? 160.0 :
						((second.hasKey || second.hasExit || second.isLocked) ? 112.0 : 72.0);
					ReleaseDepth(firstFace, firstMinimum, needed);
					ReleaseDepth(secondFace, secondMinimum, needed);
					return needed <= 0.001;
				};
				if (MakeDepth(requiredDepth)) continue;

				if (profile != PGCP_Standard && profile != PGCP_Narrow)
				{
					profile = mandatory ? PGCP_Standard : PGCP_Narrow;
					requiredDepth = ConnectionDepthForProfile(profile);
					if (mandatory) requiredDepth = std::max(requiredDepth, 64);
					RealizeConnectionContract(first, direction, second, opposite,
						ConnectionWidthForProfile(profile), requiredDepth, mandatory);
					if (MakeDepth(requiredDepth)) continue;
				}

				LastError.Format("Could not reserve the required %d-unit connector depth at (%d,%d)-(%d,%d)",
					requiredDepth, x, y, nx, ny);
				return false;
			}
		}
	}
	auto EdgeForCell = [&](int x, int y, int direction) -> double
	{
		return cellEdges[y][x].edge[direction];
	};
	auto CellHasHeightTransition = [&](int x, int y) -> bool
	{
		if (!Grid[y][x].present || !IsValidRoom(Grid[y][x].roomId)) return false;
		const int roomId = Grid[y][x].roomId;
		for (int direction = 0; direction < 4; direction++)
		{
			if (!Grid[y][x].conn[direction]) continue;
			const int nx = x + DX[direction];
			const int ny = y + DY[direction];
			if (nx < 0 || nx >= W || ny < 0 || ny >= H ||
				!Grid[ny][nx].present || !IsValidRoom(Grid[ny][nx].roomId)) continue;
			const int neighborId = Grid[ny][nx].roomId;
			if (neighborId != roomId &&
				fabs(Rooms[neighborId].floorZ - Rooms[roomId].floorZ) >= 0.001)
				return true;
		}
		return false;
	};

	// The finale is always outdoors. The style/theme budget can then expose
	// additional eligible arenas, hubs, and broad route rooms; compact Enclosed
	// maps may deliberately keep the finale as their only courtyard.
	TArray<bool> outdoorRooms;
	outdoorRooms.Resize(Rooms.Size());
	for (unsigned int ri = 0; ri < Rooms.Size(); ri++) outdoorRooms[ri] = false;
	int outdoorBudget = Outdoors == 0 ? 1 + Size / 12 :
		(Outdoors == 2 ? 3 + Size : 2 + Size / 2);
	if (themeStyle == ThemeHell) outdoorBudget += 1 + Size / 5;
	else if (themeStyle == ThemeGothic) outdoorBudget += 1 + Size / 8;
	else if (themeStyle == ThemeIndustrial)
		outdoorBudget = std::max(1, outdoorBudget - std::max(1, Size / 5));
	else if (themeStyle == ThemeCorrupted) outdoorBudget += Size / 8;
	for (unsigned int ri = 0; ri < Rooms.Size(); ri++)
	{
		if (Rooms[ri].hasExit)
		{
			outdoorRooms[ri] = true;
			outdoorBudget--;
		}
	}
	for (int pass = 0; pass < 3 && outdoorBudget > 0; pass++)
	{
		for (unsigned int ri = 0; ri < Rooms.Size() && outdoorBudget > 0; ri++)
		{
			const RoomInfo& room = Rooms[ri];
			if (outdoorRooms[ri] || room.hasPlayerStart || room.hasKey || room.isLocked) continue;
			bool candidate = pass == 0 ? room.isArena :
				(pass == 1 ? room.isHub : (room.onMainPath && room.cellCount >= 3));
			if (!candidate || room.cellCount < 2) continue;
			outdoorRooms[ri] = true;
			outdoorBudget--;
		}
	}

	// Reserve the map's strongest non-critical spatial beat for the primary
	// fluid system before alcoves, perches, and lifts spend its cells. This makes
	// liquid a macro-layout decision instead of whatever decoration fits last.
	int primaryFluidRoom = -1;
	int primaryFluidScore = -1;
	for (unsigned int ri = 0; ri < Rooms.Size(); ri++)
	{
		const RoomInfo& room = Rooms[ri];
		if (room.cellCount < 3 || room.hasPlayerStart || room.hasKey || room.hasExit ||
			room.hasBoss || room.isLocked || room.isSecret || room.reservedSecret)
			continue;
		int score = room.cellCount * 100 + room.spatialClass * 80;
		if (room.isHub || room.isArena) score += 120;
		if (outdoorRooms[ri]) score += 80;
		if (room.onMainPath) score += 40;
		if (room.featureMotif == PGFM_Watercourse) score += 1200;
		else if (room.featureMotif == PGFM_VerticalPressure) score -= 120;
		score += abs(room.maxI - room.minI - (room.maxJ - room.minJ)) * 12;
		if (score > primaryFluidScore)
		{
			primaryFluidScore = score;
			primaryFluidRoom = ri;
		}
	}

	enum RevealKind
	{
		RevealNone,
		RevealSwitchCache,
	};
	TArray<int> revealKinds;
	TArray<int> revealTags;
	TArray<int> revealBorderTypes;
	TArray<int> revealCellX;
	TArray<int> revealCellY;
	TArray<int> revealDoorSides;
	TArray<double> revealProfileAdjustX;
	TArray<double> revealProfileAdjustY;
	TArray<int> revealVariants;
	TArray<int> revealArchitectures;
	TArray<int> revealCues;
	TArray<int> revealWallLineIndices;
	TArray<int> revealClosetSectors;
	TArray<int> revealRewardFirstThings;
	TArray<int> revealRewardThingCounts;
	TArray<int> switchTargetTags;
	TArray<int> perchTags;
	TArray<int> perchCellX;
	TArray<int> perchCellY;
	TArray<int> perchApproachSides;
	TArray<int> perchVariants;
	TArray<int> liftTags;
	TArray<int> liftCellX;
	TArray<int> liftCellY;
	// A sparse layout can legitimately have fewer leaf rooms than the secret
	// reward budget.  These are self-contained manual annexes in otherwise
	// ordinary optional rooms; unlike reclassifying a through-room as secret,
	// they cannot seal or hide a required route.
	TArray<int> secretAnnexCellX;
	TArray<int> secretAnnexCellY;
	TArray<int> secretAnnexDoorSides;
	TArray<int> secretAnnexVariants;
	TArray<FluidDescriptor> fluidDescriptors;
	TArray<bool> fluidCellReserved;
	TArray<bool> falseWallNeighborReserved;
	revealKinds.Resize(Rooms.Size());
	revealTags.Resize(Rooms.Size());
	revealBorderTypes.Resize(Rooms.Size());
	revealCellX.Resize(Rooms.Size());
	revealCellY.Resize(Rooms.Size());
	revealDoorSides.Resize(Rooms.Size());
	revealProfileAdjustX.Resize(Rooms.Size());
	revealProfileAdjustY.Resize(Rooms.Size());
	revealVariants.Resize(Rooms.Size());
	revealArchitectures.Resize(Rooms.Size());
	revealCues.Resize(Rooms.Size());
	revealWallLineIndices.Resize(Rooms.Size());
	revealClosetSectors.Resize(Rooms.Size());
	revealRewardFirstThings.Resize(Rooms.Size());
	revealRewardThingCounts.Resize(Rooms.Size());
	switchTargetTags.Resize(Rooms.Size());
	perchTags.Resize(Rooms.Size());
	perchCellX.Resize(Rooms.Size());
	perchCellY.Resize(Rooms.Size());
	perchApproachSides.Resize(Rooms.Size());
	perchVariants.Resize(Rooms.Size());
	liftTags.Resize(Rooms.Size());
	liftCellX.Resize(Rooms.Size());
	liftCellY.Resize(Rooms.Size());
	secretAnnexCellX.Resize(Rooms.Size());
	secretAnnexCellY.Resize(Rooms.Size());
	secretAnnexDoorSides.Resize(Rooms.Size());
	secretAnnexVariants.Resize(Rooms.Size());
	fluidDescriptors.Resize(Rooms.Size());
	fluidCellReserved.Resize(W * H);
	falseWallNeighborReserved.Resize(W * H);
	for (int index = 0; index < W * H; index++)
	{
		falseWallNeighborReserved[index] = false;
		fluidCellReserved[index] = false;
	}
	for (unsigned int ri = 0; ri < Rooms.Size(); ri++)
	{
		revealKinds[ri] = RevealNone;
		revealTags[ri] = 0;
		revealBorderTypes[ri] = 0;
		revealCellX[ri] = revealCellY[ri] = -1;
		revealDoorSides[ri] = -1;
		revealProfileAdjustX[ri] = revealProfileAdjustY[ri] = 0.0;
		revealVariants[ri] = -1;
		revealArchitectures[ri] = RevealPavilion;
		revealCues[ri] = RevealProminent;
		revealWallLineIndices[ri] = -1;
		revealClosetSectors[ri] = -1;
		revealRewardFirstThings[ri] = -1;
		revealRewardThingCounts[ri] = 0;
		switchTargetTags[ri] = 0;
		perchTags[ri] = 0;
		perchCellX[ri] = perchCellY[ri] = -1;
		perchApproachSides[ri] = -1;
		perchVariants[ri] = 0;
		liftTags[ri] = 0;
		liftCellX[ri] = liftCellY[ri] = -1;
		secretAnnexCellX[ri] = secretAnnexCellY[ri] = -1;
		secretAnnexDoorSides[ri] = -1;
		secretAnnexVariants[ri] = 0;
		fluidDescriptors[ri] = FluidDescriptor();
	}
	constexpr double RevealClearance = 64.0;
	auto BuildRevealProfile = [&](int roomId, int revealKind) -> RevealProfile
	{
		static const double DesiredX[] = {
			72.0, 104.0, 88.0, 112.0, 80.0, 96.0, 108.0, 76.0
		};
		static const double DesiredY[] = {
			104.0, 72.0, 96.0, 80.0, 112.0, 84.0, 76.0, 108.0
		};
		static const double MoatWidth[] = {
			16.0, 16.0, 24.0, 20.0, 18.0, 28.0, 20.0, 18.0
		};
		static const double Chamfer[] = {
			8.0, 12.0, 28.0, 20.0, 32.0, 16.0, 24.0, 10.0
		};
		const RoomInfo& room = Rooms[roomId];
		const int style = abs(room.id * 17 + room.visualVariant * 11 +
			room.progressionRank * 5 + revealKind * 7) % countof(DesiredX);
		const double roleGrowth = revealKind == RevealSwitchCache ? 6.0 : -2.0;
		const double maxOuterX = roomHalfX[roomId] - RevealClearance;
		const double maxOuterY = roomHalfY[roomId] - RevealClearance;

			RevealProfile profile;
			profile.outerX = std::min(DesiredX[style] + roleGrowth, maxOuterX) +
				revealProfileAdjustX[roomId];
			profile.outerY = std::min(DesiredY[style] + roleGrowth, maxOuterY) +
				revealProfileAdjustY[roomId];
			const double moat = MoatWidth[style];
			profile.innerX = profile.outerX - moat;
			profile.innerY = profile.outerY - moat;
			if (revealArchitectures[roomId] == RevealFalseWall &&
				(profile.innerX < 56.0 || profile.innerY < 56.0))
			{
				// A constrained one-cell host cannot circulate around a pavilion, but
				// its verified empty neighbor can hold a compact false-wall chamber.
				// Keep a 56-unit interior on the doorway tangent for two ambushers and
				// use the void-side axis for the chamber's extra depth.
				const int doorSide = clamp(revealDoorSides[roomId], 0, 3);
				const bool horizontalDoor = (doorSide & 1) == 0;
				profile.outerX = horizontalDoor ? 72.0 : 96.0;
				profile.outerY = horizontalDoor ? 96.0 : 72.0;
				profile.innerX = profile.outerX - 16.0;
				profile.innerY = profile.outerY - 16.0;
			}
		profile.outerChamfer = std::min(Chamfer[style],
			std::min(profile.outerX, profile.outerY) - 32.0);
		profile.innerChamfer = std::min(std::max(8.0, profile.outerChamfer - 6.0),
			std::min(profile.innerX, profile.innerY) - 32.0);
		if (revealArchitectures[roomId] == RevealWallAlcove)
		{
			// A wall bank reads as a deliberately constructed rectangular annex,
			// rather than another copy of the clipped freestanding pavilion.
			profile.outerChamfer = 6.0;
			profile.innerChamfer = 6.0;
		}

		// Use only the clearance beyond the 64-unit traversal contract for a
		// subtle off-center placement. The diagonal budget prevents that offset
		// from squeezing the room's clipped corners.
		double offsetX = std::min(24.0,
			floor(std::max(0.0, maxOuterX - profile.outerX) / 4.0) * 4.0);
		double offsetY = std::min(24.0,
			floor(std::max(0.0, maxOuterY - profile.outerY) / 4.0) * 4.0);
		const double diagonalBudget = floor(std::max(0.0,
			roomHalfX[roomId] + roomHalfY[roomId] - room.cornerCut -
			profile.outerX - profile.outerY - RevealClearance * sqrt(2.0)) / 4.0) * 4.0;
		if (offsetX + offsetY > diagonalBudget)
		{
			offsetY = std::max(0.0, diagonalBudget - offsetX);
			if (offsetX + offsetY > diagonalBudget)
				offsetX = std::max(0.0, diagonalBudget - offsetY);
		}
		const int offsetHash = abs(room.id * 29 + room.visualVariant * 13 + revealKind * 3);
		profile.offsetX = (offsetHash & 1) ? offsetX : -offsetX;
		profile.offsetY = (offsetHash & 2) ? offsetY : -offsetY;
		profile.variant = revealVariants[roomId] >= 0 ? revealVariants[roomId] : style;
		static const double CacheFloorDelta[] = { 8.0, 0.0, 16.0, -8.0 };
		static const double CeilingDrop[] = { 0.0, 16.0, 24.0, 8.0 };
		profile.floorDelta = CacheFloorDelta[profile.variant % countof(CacheFloorDelta)];
		profile.ceilingDrop = CeilingDrop[profile.variant % countof(CeilingDrop)];
		return profile;
	};
	auto CanHostReveal = [&](int roomId, int revealKind) -> bool
	{
		if (!IsValidRoom(roomId) || Rooms[roomId].cellCount < 2) return false;
		const RoomInfo& room = Rooms[roomId];
		const RevealProfile profile = BuildRevealProfile(roomId, revealKind);
		if (profile.innerX < 56.0 || profile.innerY < 56.0 ||
			profile.outerChamfer < 4.0 || profile.innerChamfer < 4.0)
			return false;
		const double sideClearanceX = roomHalfX[roomId] - profile.outerX - fabs(profile.offsetX);
		const double sideClearanceY = roomHalfY[roomId] - profile.outerY - fabs(profile.offsetY);
		const double chamferClearance =
			(roomHalfX[roomId] + roomHalfY[roomId] - room.cornerCut -
				profile.outerX - profile.outerY - fabs(profile.offsetX) -
				fabs(profile.offsetY)) / sqrt(2.0);
		return sideClearanceX >= RevealClearance &&
			sideClearanceY >= RevealClearance &&
			chamferClearance >= RevealClearance;
	};
	auto ChooseRevealDoorSide = [&](int roomId, int featureX, int featureY,
		int revealKind) -> int
	{
		// Side order is south, east, north, west. Prefer an edge that faces into
		// another cell of the composed room, producing an open approach instead of
		// pointing the door at the nearest perimeter wall.
		static const int DoorSideForGridDirection[4] = { 0, 2, 3, 1 };
		TArray<int> preferred;
		for (int direction = 0; direction < 4; direction++)
		{
			const int nx = featureX + DX[direction];
			const int ny = featureY + DY[direction];
			if (nx >= 0 && nx < W && ny >= 0 && ny < H &&
				Grid[ny][nx].present && Grid[ny][nx].roomId == roomId)
				preferred.Push(DoorSideForGridDirection[direction]);
		}
		if (preferred.Size() == 0)
		{
			for (int direction = 0; direction < 4; direction++)
				if (Grid[featureY][featureX].conn[direction])
					preferred.Push(DoorSideForGridDirection[direction]);
		}
		const int style = abs(Rooms[roomId].id * 19 + Rooms[roomId].visualVariant * 7 +
			revealKind * 5);
		return preferred.Size() > 0 ? preferred[style % preferred.Size()] : style % 4;
	};
	auto IsLandmarkAnchorCell = [&](int roomId, int x, int y) -> bool
	{
		const RoomInfo& room = Rooms[roomId];
		if (Grid[y][x].hasPlayerStart || Grid[y][x].hasKey || Grid[y][x].hasExit)
			return true;
		// Landmark emission uses the first cell in the room scan for every
		// archetype, not just arena and hub defaults. Reserve that same cell for
		// all authored landmarks so an optional lift cannot consume its footprint
		// before the later spatial-grammar pass emits it.
		if (room.landmarkArchetype == PGLA_None && !room.isArena && !room.isHub)
			return false;
		for (int candidateY = room.minJ; candidateY <= room.maxJ; candidateY++)
		{
			for (int candidateX = room.minI; candidateX <= room.maxI; candidateX++)
			{
				if (candidateX >= 0 && candidateX < W && candidateY >= 0 && candidateY < H &&
					Grid[candidateY][candidateX].present &&
					Grid[candidateY][candidateX].roomId == roomId)
					return candidateX == x && candidateY == y;
			}
		}
		return false;
	};

	auto PickFeatureCell = [&](int roomId, int revealKind,
		int& featureX, int& featureY) -> bool
	{
		if (!CanHostReveal(roomId, revealKind)) return false;
		for (int pass = 0; pass < 2; pass++)
		{
			TArray<std::pair<int, int>> candidates;
			for (int y = 0; y < H; y++)
			{
				for (int x = 0; x < W; x++)
				{
					const ProcGenCell& cell = Grid[y][x];
					if (!cell.present || cell.roomId != roomId) continue;
					if (cell.hasPlayerStart || cell.hasKey || cell.hasExit || cell.hasBoss || cell.isLocked)
						continue;
					if (pass == 0 && CellHasHeightTransition(x, y)) continue;
					if (IsLandmarkAnchorCell(roomId, x, y)) continue;
					candidates.Push(std::make_pair(x, y));
				}
			}
			if (candidates.Size() == 0) continue;
			const auto& selected = candidates[RNG() % candidates.Size()];
			featureX = selected.first;
			featureY = selected.second;
			return true;
		}
		return false;
	};
	static const int WallSideForGridDirection[4] = { 0, 2, 3, 1 };
	static const int OppositeGridDirection[4] = { DIR_S, DIR_N, DIR_E, DIR_W };
	auto PickWallAlcoveCell = [&](int roomId, int& featureX, int& featureY,
		int& doorSide) -> bool
	{
		struct WallAlcoveCandidate
		{
			int x;
			int y;
			int side;
		};
		for (int pass = 0; pass < 2; pass++)
		{
			TArray<WallAlcoveCandidate> candidates;
			for (int y = 0; y < H; y++)
			{
				for (int x = 0; x < W; x++)
				{
					const ProcGenCell& cell = Grid[y][x];
					if (!cell.present || cell.roomId != roomId || cell.hasPlayerStart ||
						cell.hasKey || cell.hasExit || cell.hasBoss || cell.isLocked ||
						CellHasHeightTransition(x, y) || IsLandmarkAnchorCell(roomId, x, y))
						continue;
					for (int backDirection = 0; backDirection < 4; backDirection++)
					{
						if (cell.conn[backDirection]) continue;
						const int bx = x + DX[backDirection];
						const int by = y + DY[backDirection];
						if (bx >= 0 && bx < W && by >= 0 && by < H && Grid[by][bx].present)
							continue;
						const int frontDirection = OppositeGridDirection[backDirection];
						const int nx = x + DX[frontDirection];
						const int ny = y + DY[frontDirection];
						const bool facesRoom = nx >= 0 && nx < W && ny >= 0 && ny < H &&
							Grid[ny][nx].present && Grid[ny][nx].roomId == roomId;
						if (pass == 0 && !facesRoom) continue;
						const double span = backDirection == DIR_N || backDirection == DIR_S ?
							roomHalfX[roomId] * 2.0 : roomHalfY[roomId] * 2.0;
						if (span - Rooms[roomId].cornerCut * 2.0 < 208.0) continue;
						const int backSide = WallSideForGridDirection[backDirection];
						candidates.Push({ x, y, (backSide + 2) % 4 });
					}
				}
			}
			if (candidates.Size() == 0) continue;
			const WallAlcoveCandidate& selected = candidates[RNG() % candidates.Size()];
			featureX = selected.x;
			featureY = selected.y;
			doorSide = selected.side;
			return true;
		}
		return false;
	};
	auto PickFalseWallCell = [&](int roomId, int& featureX, int& featureY,
		int& doorSide, int& neighborX, int& neighborY) -> bool
	{
		struct FalseWallCandidate
		{
			int x;
			int y;
			int side;
			int neighborX;
			int neighborY;
		};
		for (int pass = 0; pass < 2; pass++)
		{
			TArray<FalseWallCandidate> candidates;
			for (int y = 0; y < H; y++)
			{
				for (int x = 0; x < W; x++)
				{
					const ProcGenCell& cell = Grid[y][x];
					if (!cell.present || cell.roomId != roomId || cell.hasPlayerStart ||
						cell.hasKey || cell.hasExit || cell.hasBoss || cell.isLocked)
						continue;
					if (pass == 0 && CellHasHeightTransition(x, y)) continue;
					if (IsLandmarkAnchorCell(roomId, x, y)) continue;
					for (int direction = 0; direction < 4; direction++)
					{
						if (cell.conn[direction]) continue;
						const int nx = x + DX[direction];
						const int ny = y + DY[direction];
						// The closet grows into a reserved, in-bounds coarse-grid neighbor.
						// This prevents two independently selected false walls from sharing
						// the same void cell or extending beyond the verified layout.
						if (nx < 0 || nx >= W || ny < 0 || ny >= H || Grid[ny][nx].present ||
							falseWallNeighborReserved[ny * W + nx])
							continue;
						const double span = direction == DIR_N || direction == DIR_S ?
							roomHalfX[roomId] * 2.0 : roomHalfY[roomId] * 2.0;
						if (span - Rooms[roomId].cornerCut * 2.0 < 144.0) continue;
						candidates.Push({ x, y, WallSideForGridDirection[direction], nx, ny });
					}
				}
			}
			if (candidates.Size() == 0) continue;
			const FalseWallCandidate& selected = candidates[RNG() % candidates.Size()];
			featureX = selected.x;
			featureY = selected.y;
			doorSide = selected.side;
			neighborX = selected.neighborX;
			neighborY = selected.neighborY;
			return true;
		}
		return false;
	};
	int revealArchitectureOrdinal = variantSeedMod3;
	auto AssignRevealArchitecture = [&](int roomId, int& featureX,
		int& featureY, int revealKind)
	{
		const int ordinal = revealArchitectureOrdinal++;
		int architecture = ordinal % 3;
		int doorSide = -1;
		int neighborX = -1;
		int neighborY = -1;
		if (architecture == RevealFalseWall)
		{
			if (!PickFalseWallCell(roomId, featureX, featureY, doorSide,
				neighborX, neighborY))
			{
				architecture = RevealWallAlcove;
				if (!PickWallAlcoveCell(roomId, featureX, featureY, doorSide))
					architecture = RevealPavilion;
			}
		}
		else if (architecture == RevealWallAlcove &&
			!PickWallAlcoveCell(roomId, featureX, featureY, doorSide))
		{
			architecture = RevealFalseWall;
			if (!PickFalseWallCell(roomId, featureX, featureY, doorSide,
				neighborX, neighborY))
				architecture = RevealPavilion;
		}
		revealArchitectures[roomId] = architecture;
		// Cue cadence is independent of reveal role. Including revealKind made the
		// first key trap and first switch cache algebraically collapse to the same
		// cue on most maps despite belonging to different architecture families.
		revealCues[roomId] = (variantSeedThirdMod3 + ordinal) % 3;
		if (architecture == RevealWallAlcove || architecture == RevealFalseWall)
		{
			revealDoorSides[roomId] = doorSide;
			if (architecture == RevealFalseWall)
				falseWallNeighborReserved[neighborY * W + neighborX] = true;
		}
	};
	auto PickPerchCell = [&](int roomId, int& featureX, int& featureY) -> bool
	{
		// Prefer an untouched, level cell. Compact maps may legitimately use every
		// such cell for a terrace connector, so a second pass accepts a transition
		// cell: the perch compositor owns that cell and emits its own complete stair
		// sequence, preserving the same traversability contract.
		for (int pass = 0; pass < 2; pass++)
		{
			TArray<std::pair<int, int>> candidates;
			for (int y = 0; y < H; y++)
			{
				for (int x = 0; x < W; x++)
				{
					const ProcGenCell& cell = Grid[y][x];
					if (!cell.present || cell.roomId != roomId) continue;
					if (cell.hasPlayerStart || cell.hasKey || cell.hasExit || cell.hasBoss || cell.isLocked)
						continue;
					if (pass == 0 && CellHasHeightTransition(x, y)) continue;
					if (x == revealCellX[roomId] && y == revealCellY[roomId]) continue;
					if (IsLandmarkAnchorCell(roomId, x, y)) continue;
					candidates.Push(std::make_pair(x, y));
				}
			}
			if (candidates.Size() == 0) continue;
			const auto& selected = candidates[RNG() % candidates.Size()];
			featureX = selected.first;
			featureY = selected.second;
			return true;
		}
		return false;
	};
	auto ChoosePerchApproachSide = [&](int roomId, int featureX, int featureY) -> int
	{
		// Side order is south, east, north, west. A perch is only assigned to a
		// composed room, so point its staircase into another cell of that room and
		// keep the full run away from the local perimeter.
		static const int SideForGridDirection[4] = { 0, 2, 3, 1 };
		TArray<int> preferred;
		for (int direction = 0; direction < 4; direction++)
		{
			const int nx = featureX + DX[direction];
			const int ny = featureY + DY[direction];
			if (nx >= 0 && nx < W && ny >= 0 && ny < H &&
				Grid[ny][nx].present && Grid[ny][nx].roomId == roomId)
				preferred.Push(SideForGridDirection[direction]);
		}
		const int style = abs(Rooms[roomId].id * 23 +
			Rooms[roomId].visualVariant * 11 + featureX * 5 + featureY * 7);
		return preferred.Size() > 0 ? preferred[style % preferred.Size()] : style % 4;
	};

	auto StableRoomHash = [&](int roomId, uint32_t salt) -> uint32_t
	{
		uint32_t value = blueprintHash ^ salt ^ (uint32_t)roomId * 0x9e3779b9u;
		value = (value ^ (value >> 16)) * 0x21f0aaadu;
		value = (value ^ (value >> 15)) * 0x735a2d97u;
		return value ^ (value >> 15);
	};
	auto RankRoomsForMotif = [&](TArray<int>& roomIds, int motif, uint32_t salt)
	{
		for (int first = 0; first < (int)roomIds.Size(); first++)
		{
			int best = first;
			for (int candidate = first + 1; candidate < (int)roomIds.Size(); candidate++)
			{
				auto Score = [&](int roomId) -> int
				{
					const RoomInfo& room = Rooms[roomId];
					const int affinity = room.featureMotif == motif ? 100000 : 0;
					return affinity + room.featureMotifPriority * 1000 +
						(int)(StableRoomHash(roomId, salt) & 0x3ffu);
				};
				if (Score(roomIds[candidate]) > Score(roomIds[best])) best = candidate;
			}
			if (best != first)
			{
				const int saved = roomIds[first];
				roomIds[first] = roomIds[best];
				roomIds[best] = saved;
			}
		}
	};
	auto RoomsConnected = [&](int firstRoom, int secondRoom) -> bool
	{
		for (int y = 0; y < H; y++)
		{
			for (int x = 0; x < W; x++)
			{
				if (!Grid[y][x].present || Grid[y][x].roomId != firstRoom) continue;
				for (int direction = 0; direction < 4; direction++)
				{
					if (!Grid[y][x].conn[direction]) continue;
					const int nx = x + DX[direction];
					const int ny = y + DY[direction];
					if (nx >= 0 && nx < W && ny >= 0 && ny < H &&
						Grid[ny][nx].roomId == secondRoom)
						return true;
				}
			}
		}
		return false;
	};
	auto CanHostSwitchPanel = [&](int roomId) -> bool
	{
		if (!IsValidRoom(roomId)) return false;
		const RoomInfo& room = Rooms[roomId];
		for (int y = room.minJ; y <= room.maxJ; y++)
		{
			for (int x = room.minI; x <= room.maxI; x++)
			{
				if (x < 0 || x >= W || y < 0 || y >= H ||
					!Grid[y][x].present || Grid[y][x].roomId != roomId)
					continue;
				for (int direction = 0; direction < 4; direction++)
				{
					if (Grid[y][x].conn[direction]) continue;
					const double halfSpan = direction == DIR_N || direction == DIR_S ?
						roomHalfX[roomId] : roomHalfY[roomId];
					if (halfSpan * 2.0 - room.cornerCut * 2.0 >= 96.0)
						return true;
				}
			}
		}
		return false;
	};

	// Broad non-critical rooms can contain a remote supply cache. A switch is
	// authored on a real perimeter wall and opens the tagged closet permanently.
	TArray<int> switchRooms;
	int desiredSwitchRooms = Detail == 0 ? 1 + Size / 12 :
		(Detail == 2 ? 2 + Size / 4 : 1 + Size / 6);
	if (themeStyle == ThemeIndustrial) desiredSwitchRooms += 1 + Size / 12;
	else if (themeStyle == ThemeTechbase && Detail == 2) desiredSwitchRooms++;
	for (const RoomInfo& room : Rooms)
		if (room.featureMotif == PGFM_RemoteReveal) desiredSwitchRooms++;
	for (int pass = 0; pass < 2; pass++)
	{
		for (unsigned int ri = 0; ri < Rooms.Size(); ri++)
		{
			const RoomInfo& room = Rooms[ri];
			if ((int)ri == primaryFluidRoom || revealKinds[ri] != RevealNone ||
				!CanHostReveal(ri, RevealSwitchCache) ||
				!CanHostSwitchPanel(ri) ||
				room.hasPlayerStart ||
				room.hasKey || room.isLocked || room.isSecret)
				continue;
			if ((pass == 0 && room.isArena) || (pass == 1 && !room.isArena)) continue;
			if (room.isArena || room.isHub || room.isDeadEnd || room.onMainPath)
				switchRooms.Push(ri);
		}
		if (switchRooms.Size() >= (unsigned int)desiredSwitchRooms) break;
	}
	RankRoomsForMotif(switchRooms, PGFM_RemoteReveal, 0x52564c31u);
	int nextSwitchTag = 1500;
	int switchBudget = desiredSwitchRooms;
	for (unsigned int index = 0; index < switchRooms.Size() && switchBudget > 0; index++)
	{
		const int roomId = switchRooms[index];
		// A room already selected as a remote switch source owns one of its wall
		// faces. Do not subsequently turn that same room into another cache host;
		// doing so made the reveal compositor and switch compositor compete for the
		// last usable perimeter segment on constrained layouts.
		if (switchTargetTags[roomId] > 0) continue;
		int featureX, featureY;
		if (!PickFeatureCell(roomId, RevealSwitchCache, featureX, featureY)) continue;
		revealKinds[roomId] = RevealSwitchCache;
		revealTags[roomId] = nextSwitchTag++;
		revealBorderTypes[roomId] = 0;
		revealCellX[roomId] = featureX;
		revealCellY[roomId] = featureY;
		static const int SwitchRevealVariants[] = { 1, 3, 2, 0 };
		revealVariants[roomId] =
			SwitchRevealVariants[(revealTags[roomId] - 1500) % countof(SwitchRevealVariants)];
		AssignRevealArchitecture(roomId, featureX, featureY, RevealSwitchCache);
		revealCellX[roomId] = featureX;
		revealCellY[roomId] = featureY;
		if (revealArchitectures[roomId] == RevealPavilion)
			revealDoorSides[roomId] = ChooseRevealDoorSide(roomId,
				featureX, featureY, RevealSwitchCache);

		// Some opportunity switches live across the room boundary from their
		// cache. Keep the source in the same lock stage so the remote action adds
		// discovery and reuse without operating through an unavailable key gate.
		int switchRoomId = roomId;
		if ((revealVariants[roomId] & 1) != 0 ||
			revealArchitectures[roomId] == RevealFalseWall)
		{
			int bestScore = 1000000;
			for (unsigned int source = 0; source < Rooms.Size(); source++)
			{
				const RoomInfo& sourceRoom = Rooms[source];
				if ((int)source == roomId || switchTargetTags[source] > 0 ||
					revealKinds[source] != RevealNone || sourceRoom.isLocked ||
					sourceRoom.isSecret || sourceRoom.hasKey || sourceRoom.hasExit ||
					sourceRoom.hasPlayerStart || sourceRoom.lockStage != Rooms[roomId].lockStage ||
					!CanHostSwitchPanel(source))
					continue;
				int score = abs(sourceRoom.progressionRank - Rooms[roomId].progressionRank) * 12 +
					abs(sourceRoom.distFromStart - Rooms[roomId].distFromStart) * 4 +
					(RNG() % 5);
				if (RoomsConnected(roomId, source)) score -= 1000;
				if (score < bestScore)
				{
					bestScore = score;
					switchRoomId = source;
				}
			}
		}
		switchTargetTags[switchRoomId] = revealTags[roomId];
		Rooms[switchRoomId].manualInteraction = PGMI_SwitchCache;
		switchBudget--;
	}
	// A remote cache is an optional flourish. Compact or highly connected maps
	// may legitimately have no wall that can carry both its clearance envelope
	// and a visible manual switch, so omit the cache rather than failing a fair
	// run or falling back to an automatic trigger.
	for (unsigned int ri = 0; ri < Rooms.Size(); ++ri)
	{
		if (revealKinds[ri] == RevealNone) continue;
		const int x = revealCellX[ri];
		const int y = revealCellY[ri];
		bool touchesDogleg = false;
		for (int direction = 0; direction < 4 && !touchesDogleg; ++direction)
		{
			const int nx = x + DX[direction];
			const int ny = y + DY[direction];
			touchesDogleg = x >= 0 && x < W && y >= 0 && y < H &&
				nx >= 0 && nx < W && ny >= 0 && ny < H &&
				Grid[ny][nx].present &&
				DoglegAnchorForCells(x, y, nx, ny) >= 0;
		}
		if (!touchesDogleg) continue;

		// Keep the optional cache from competing with a mandatory turned stair.
		// Its pavilion ring needs the same chamber face that the dogleg reserves;
		// dropping the cache is always preferable to narrowing its circulation or
		// silently degrading the required route to a straight stair.
		const int missingTag = revealTags[ri];
		revealKinds[ri] = RevealNone;
		revealTags[ri] = 0;
		for (unsigned int source = 0; source < Rooms.Size(); ++source)
		{
			if (switchTargetTags[source] != missingTag) continue;
			switchTargetTags[source] = 0;
			Rooms[source].manualInteraction = Rooms[source].isLocked ?
				PGMI_KeyedDoor : (Rooms[source].isSecret ? PGMI_SecretDoor : PGMI_None);
		}
	}

	// A reveal pavilion needs its full circulation ring. If the only valid host
	// cell also owns a staircase, restore that chamber face; the opposite face
	// remains inset and still provides a useful connector run.
	for (unsigned int ri = 0; ri < Rooms.Size(); ri++)
	{
		if (revealKinds[ri] == RevealNone) continue;
		const int x = revealCellX[ri];
		const int y = revealCellY[ri];
		if (x < 0 || x >= W || y < 0 || y >= H) continue;
		// A reveal pavilion may reclaim its local circulation ring, but never at
		// the expense of a planned dogleg's connector bay. Re-expanding that face
		// after the stair pass shortened it was enough to turn a real four-tread
		// dogleg into a straight-stair fallback on otherwise feasible maps.
		bool revealCirculationFits = true;
		auto RestoreRevealFace = [&](int direction, double roomHalf)
		{
			const int nx = x + DX[direction];
			const int ny = y + DY[direction];
			if (nx >= 0 && nx < W && ny >= 0 && ny < H &&
				Grid[ny][nx].present &&
				DoglegAnchorForCells(x, y, nx, ny) >= 0)
				return;
			if (nx >= 0 && nx < W && ny >= 0 && ny < H &&
				Grid[ny][nx].present && Grid[y][x].conn[direction] &&
				Grid[ny][nx].roomId != Grid[y][x].roomId)
			{
				const ProcGenCell& first = Grid[y][x];
				const ProcGenCell& second = Grid[ny][nx];
				const int opposite = OPP[direction];
				const bool keyed = first.lockStage != second.lockStage ||
					(first.isLocked && (first.lockDir < 0 || first.lockDir == direction)) ||
					(second.isLocked &&
						(second.lockDir < 0 || second.lockDir == opposite));
				const bool stair = first.connectionStairChain[direction] >= 0 ||
					second.connectionStairChain[opposite] >= 0 ||
					fabs(first.floorZ - second.floorZ) > 0.001;
				const bool mandatory = keyed || stair ||
					(first.onMainPath && second.onMainPath) || first.hasPlayerStart ||
					first.hasKey || first.hasExit || second.hasPlayerStart ||
					second.hasKey || second.hasExit;
				const int requiredDepth = std::max(mandatory ? 64 : 48,
					std::min(first.connectionDepth[direction],
						second.connectionDepth[opposite]));
				const double centerDistance = direction == DIR_E ?
					CellCenterX(nx) - CellCenterX(x) :
					CellCenterY(ny) - CellCenterY(y);
				const double maximumFace = centerDistance -
					cellEdges[ny][nx].edge[opposite] - requiredDepth;
				if (maximumFace + 0.001 < roomHalf)
				{
					// A reveal is optional. Its pavilion requires the whole local
					// circulation ring, so do not retain a cramped partial version
					// that steals clearance from a real connector.
					revealCirculationFits = false;
					return;
				}
				roomHalf = std::min(roomHalf, maximumFace);
			}
			// Never overwrite a deeper pre-reserved connector bay. A reveal can use
			// spare shell area, but cannot reclaim it from a real walking corridor.
			if (roomHalf > cellEdges[y][x].edge[direction])
				cellEdges[y][x].edge[direction] = roomHalf;
		};
		RestoreRevealFace(DIR_N, roomHalfY[ri]);
		RestoreRevealFace(DIR_S, roomHalfY[ri]);
		RestoreRevealFace(DIR_W, roomHalfX[ri]);
		RestoreRevealFace(DIR_E, roomHalfX[ri]);
		if (!revealCirculationFits)
		{
			const int missingTag = revealTags[ri];
			revealKinds[ri] = RevealNone;
			revealTags[ri] = 0;
			if (missingTag > 0)
			{
				for (unsigned int source = 0; source < Rooms.Size(); ++source)
				{
					if (switchTargetTags[source] != missingTag) continue;
					switchTargetTags[source] = 0;
					Rooms[source].manualInteraction = Rooms[source].isLocked ?
						PGMI_KeyedDoor : (Rooms[source].isSecret ? PGMI_SecretDoor : PGMI_None);
				}
			}
		}
	}

	// Clamping against different host rooms can occasionally collapse every
	// profile to the same bounding box. If that happens, safely narrow one axis
	// by four units while retaining the 160-unit footprint and 56-unit interior
	// half-size contracts.
	int firstFootprintX = -1;
	int firstFootprintY = -1;
	int revealProfileCount = 0;
	bool variedFootprints = false;
	for (unsigned int ri = 0; ri < Rooms.Size(); ri++)
	{
		if (revealKinds[ri] == RevealNone) continue;
		const RevealProfile profile = BuildRevealProfile(ri, revealKinds[ri]);
		const int footprintX = (int)lround(profile.outerX * 2.0);
		const int footprintY = (int)lround(profile.outerY * 2.0);
		if (revealProfileCount == 0)
		{
			firstFootprintX = footprintX;
			firstFootprintY = footprintY;
		}
		else if (footprintX != firstFootprintX || footprintY != firstFootprintY)
			variedFootprints = true;
		revealProfileCount++;
	}
	if (revealProfileCount >= 2 && !variedFootprints)
	{
		for (unsigned int ri = 0; ri < Rooms.Size(); ri++)
		{
			if (revealKinds[ri] == RevealNone) continue;
			const RevealProfile profile = BuildRevealProfile(ri, revealKinds[ri]);
			if (profile.outerX >= 84.0 && profile.innerX >= 60.0)
			{
				revealProfileAdjustX[ri] = -4.0;
				break;
			}
			if (profile.outerY >= 84.0 && profile.innerY >= 60.0)
			{
				revealProfileAdjustY[ri] = -4.0;
				break;
			}
		}
	}

	// Three or more interactive structures should not all face the same axis.
	// Prefer rotating one toward open composed-room space; the
	// guaranteed circulation ring remains a safe fallback on linear layouts.
	int revealCount = 0;
	int revealAxisMask = 0;
	for (unsigned int ri = 0; ri < Rooms.Size(); ri++)
	{
		if (revealKinds[ri] == RevealNone) continue;
		revealCount++;
		revealAxisMask |= 1 << (revealDoorSides[ri] & 1);
	}
	if (revealCount >= 3 && revealAxisMask != 3)
	{
		const int desiredAxis = (revealAxisMask & 1) ? 1 : 0;
		static const int SideForGridDirection[4] = { 0, 2, 3, 1 };
		for (unsigned int ri = 0; ri < Rooms.Size(); ri++)
		{
			if (revealKinds[ri] == RevealNone ||
				revealArchitectures[ri] != RevealPavilion) continue;
			const int featureX = revealCellX[ri];
			const int featureY = revealCellY[ri];
			TArray<int> alternatives;
			for (int pass = 0; pass < 3 && alternatives.Size() == 0; pass++)
			{
				for (int direction = 0; direction < 4; direction++)
				{
					const int side = SideForGridDirection[direction];
					if ((side & 1) != desiredAxis) continue;
					const int nx = featureX + DX[direction];
					const int ny = featureY + DY[direction];
					const bool sameRoom = nx >= 0 && nx < W && ny >= 0 && ny < H &&
						Grid[ny][nx].present && Grid[ny][nx].roomId == (int)ri;
					const bool connected = Grid[featureY][featureX].conn[direction];
					if ((pass == 0 && sameRoom) || (pass == 1 && connected) || pass == 2)
						alternatives.Push(side);
				}
			}
			if (alternatives.Size() > 0)
			{
				const int style = abs(Rooms[ri].id * 31 + Rooms[ri].visualVariant * 7);
				revealDoorSides[ri] = alternatives[style % alternatives.Size()];
				break;
			}
		}
	}

	// A separate sample of open arenas receives a raised ranged platform;
	// sufficiently tall hubs and broad route rooms provide deterministic
	// fallbacks. Every platform points a staircase into the composed room so
	// elevation changes create combat choices without unreachable space.
	TArray<int> perchRooms;
	int perchBudgetTarget = Detail == 0 ? 1 + Size / 10 :
		(Detail == 2 ? 2 + Size / 3 : 1 + Size / 4);
	if (themeStyle == ThemeHell || themeStyle == ThemeGothic)
		perchBudgetTarget += 1 + Size / 12;
	auto HasPerchCandidate = [&](int roomId) -> bool
	{
		for (unsigned int index = 0; index < perchRooms.Size(); index++)
			if (perchRooms[index] == roomId) return true;
		return false;
	};
	for (int pass = 0; pass < 3; pass++)
	{
		for (unsigned int ri = 0; ri < Rooms.Size(); ri++)
		{
			const RoomInfo& room = Rooms[ri];
			if ((int)ri == primaryFluidRoom || HasPerchCandidate(ri) ||
				room.cellCount < 2 || room.hasPlayerStart ||
				room.hasKey || room.isLocked || room.isSecret ||
				room.ceilZ - room.floorZ < 128.0)
				continue;
			const bool preferred = room.isArena || (outdoorRooms[ri] && room.isHub);
			const bool finale = room.hasExit || room.hasBoss;
			if (pass == 0 && (!preferred || finale || revealKinds[ri] != RevealNone)) continue;
			if (pass == 1 && (finale ||
				(preferred && revealKinds[ri] == RevealNone))) continue;
			if (pass == 2 && !finale) continue;
			if (preferred || room.isHub || room.onMainPath || room.isDeadEnd || finale)
				perchRooms.Push(ri);
		}
		if (perchRooms.Size() >= (unsigned int)perchBudgetTarget) break;
	}
	RankRoomsForMotif(perchRooms, PGFM_VerticalPressure, 0x50455243u);
	int nextPerchTag = 2000;
	int perchBudget = perchBudgetTarget;
	for (unsigned int index = 0; index < perchRooms.Size() && perchBudget > 0; index++)
	{
		const int roomId = perchRooms[index];
		int featureX, featureY;
		if (!PickPerchCell(roomId, featureX, featureY)) continue;
		perchTags[roomId] = nextPerchTag++;
		perchCellX[roomId] = featureX;
		perchCellY[roomId] = featureY;
		perchApproachSides[roomId] = ChoosePerchApproachSide(roomId,
			featureX, featureY);
		perchVariants[roomId] = (variantSeedMod3 + nextPerchTag - 2001) % 3;
		perchBudget--;
	}
	if (nextPerchTag == 2000)
	{
		LastError = "Could not place an elevated ranged-monster perch";
		return false;
	}

	// Operable lifts add meaningful height variation without placing a mandatory
	// route behind moving geometry. They live in spare cells with a full walkable
	// ring, so a raised or occupied platform can always be bypassed.
	auto IsLiftCellCandidate = [&](int roomId, int x, int y,
		bool allowTransition) -> bool
	{
		const ProcGenCell& cell = Grid[y][x];
		if (!cell.present || cell.roomId != roomId || cell.hasPlayerStart ||
			cell.hasKey || cell.hasExit || cell.hasBoss || cell.isLocked)
			return false;
		if (!allowTransition && CellHasHeightTransition(x, y)) return false;
		const double requiredEdge = LiftPlatformHalf + LiftBypassClearance;
		const double left = EdgeForCell(x, y, DIR_W);
		const double right = EdgeForCell(x, y, DIR_E);
		const double bottom = EdgeForCell(x, y, DIR_N);
		const double top = EdgeForCell(x, y, DIR_S);
		if (left < requiredEdge || right < requiredEdge ||
			bottom < requiredEdge || top < requiredEdge)
			return false;

		// A courtyard cut is the one room-owned contour that moves an exterior
		// wall *into* its host cell. The generic edge test above proves the
		// 96-unit bypass against the unmodified shell, but a centered cut can
		// consume up to 48 more units after lifts have already been planned. Keep
		// that full deterministic inset out of an operable lift's ring. Other
		// footprint grammars grow outward, and the conservative corner test below
		// already covers their clipped shell corners.
		if ((EProcGenRoomFootprint)Rooms[roomId].footprint == PGRF_CourtyardCut)
		{
			const double contourInset = clamp(24.0 + Rooms[roomId].contourInset * 0.5 +
				(Rooms[roomId].footprintVariant & 1 ? 8.0 : 0.0), 24.0, 48.0);
			auto ExteriorFace = [&](int direction) -> bool
			{
				const int nx = x + DX[direction];
				const int ny = y + DY[direction];
				return nx < 0 || nx >= W || ny < 0 || ny >= H ||
					!Grid[ny][nx].present;
			};
			if ((ExteriorFace(DIR_W) && left < requiredEdge + contourInset) ||
				(ExteriorFace(DIR_E) && right < requiredEdge + contourInset) ||
				(ExteriorFace(DIR_N) && bottom < requiredEdge + contourInset) ||
				(ExteriorFace(DIR_S) && top < requiredEdge + contourInset))
				return false;
		}

		// The chamber polygon can clip each corner inward. Test the lift's four
		// perimeter corners against the most conservative possible clip for this
		// room, rather than relying on the nominal half-width alone. Actual clips
		// are never deeper than room.cornerCut, so this remains safe when a portal
		// later restores a square corner.
		const double cut = std::max(0.0, Rooms[roomId].cornerCut);
		auto DistanceToSegment = [](double px, double py, double ax, double ay,
			double bx, double by) -> double
		{
			const double dx = bx - ax;
			const double dy = by - ay;
			const double lengthSquared = dx * dx + dy * dy;
			if (lengthSquared < 0.0001) return hypot(px - ax, py - ay);
			const double fraction = clamp(((px - ax) * dx + (py - ay) * dy) /
				lengthSquared, 0.0, 1.0);
			return hypot(px - (ax + dx * fraction), py - (ay + dy * fraction));
		};
		auto CornerHasBypass = [&](double px, double py,
			double ax, double ay, double bx, double by) -> bool
		{
			return DistanceToSegment(px, py, ax, ay, bx, by) + 0.001 >=
				LiftBypassClearance;
		};
		if (!CornerHasBypass(LiftPlatformHalf, LiftPlatformHalf,
			right - cut, top, right, top - cut) ||
			!CornerHasBypass(LiftPlatformHalf, -LiftPlatformHalf,
			right, -bottom + cut, right - cut, -bottom) ||
			!CornerHasBypass(-LiftPlatformHalf, -LiftPlatformHalf,
			-left + cut, -bottom, -left, -bottom + cut) ||
			!CornerHasBypass(-LiftPlatformHalf, LiftPlatformHalf,
			-left, top - cut, -left + cut, top))
			return false;
		return !((x == revealCellX[roomId] && y == revealCellY[roomId]) ||
			(x == perchCellX[roomId] && y == perchCellY[roomId]) ||
			IsLandmarkAnchorCell(roomId, x, y));
	};
	auto HasLiftCell = [&](int roomId) -> bool
	{
		for (int y = 0; y < H; y++)
		{
			for (int x = 0; x < W; x++)
				if (IsLiftCellCandidate(roomId, x, y, true)) return true;
		}
		return false;
	};
	auto PickLiftCell = [&](int roomId, int& featureX, int& featureY) -> bool
	{
		for (int pass = 0; pass < 2; pass++)
		{
			TArray<std::pair<int, int>> candidates;
			for (int y = 0; y < H; y++)
			{
				for (int x = 0; x < W; x++)
				{
					if (!IsLiftCellCandidate(roomId, x, y, pass != 0)) continue;
					candidates.Push(std::make_pair(x, y));
				}
			}
			if (candidates.Size() == 0) continue;
			const auto& selected = candidates[RNG() % candidates.Size()];
			featureX = selected.first;
			featureY = selected.second;
			return true;
		}
		return false;
	};
	TArray<int> liftRooms;
	auto HasLiftRoom = [&](int roomId) -> bool
	{
		for (unsigned int index = 0; index < liftRooms.Size(); index++)
			if (liftRooms[index] == roomId) return true;
		return false;
	};
	int liftBudgetTarget = Detail == 0 ? 1 :
		(Detail == 2 ? 2 + Size / 5 : 1 + Size / 8);
	if (themeStyle == ThemeIndustrial) liftBudgetTarget += 1 + Size / 8;
	else if (themeStyle == ThemeTechbase && Detail == 2) liftBudgetTarget++;
	for (int pass = 0; pass < 3; pass++)
	{
		for (unsigned int ri = 0; ri < Rooms.Size(); ri++)
		{
			const RoomInfo& room = Rooms[ri];
			if ((int)ri == primaryFluidRoom || HasLiftRoom(ri) ||
				room.hasPlayerStart || room.hasKey ||
				room.hasExit || room.hasBoss || room.isLocked || room.isSecret ||
				room.ceilZ - room.floorZ < 96.0 || !HasLiftCell(ri))
				continue;
			const bool ordinaryRoute = room.onMainPath && !room.isArena && !room.isHub;
			const bool landmark = room.isArena || room.isHub;
			if ((pass == 0 && !ordinaryRoute) || (pass == 1 && !landmark) ||
				(pass == 2 && (ordinaryRoute || landmark)))
				continue;
			liftRooms.Push(ri);
		}
		if (liftRooms.Size() >= (unsigned int)liftBudgetTarget) break;
	}
	RankRoomsForMotif(liftRooms, PGFM_VerticalPressure, 0x4c494654u);
	int nextLiftTag = 3000;
	int liftBudget = liftBudgetTarget;
	for (unsigned int index = 0; index < liftRooms.Size() && liftBudget > 0; index++)
	{
		const int roomId = liftRooms[index];
		int featureX, featureY;
		if (!PickLiftCell(roomId, featureX, featureY)) continue;
		liftTags[roomId] = nextLiftTag++;
		liftCellX[roomId] = featureX;
		liftCellY[roomId] = featureY;
		liftBudget--;
	}
	// Lifts are optional scenery and never part of a required route. Compact
	// maps can correctly have no cell that satisfies the full bypass-ring proof;
	// retain the zero-tag fallback rather than rejecting an otherwise playable
	// recipe or weakening the clearance contract to force one in.

	// Fluid descriptors reserve their complete footprint before any thing is
	// placed. Pools range from local basins to broad composed-room grottos, while
	// watercourses can follow long straight or turning cell runs. Every family
	// leaves a wholly dry placement cell and at least 64 units beside its banks.
	auto FluidReservationCost = [&](int roomId) -> int
	{
		int reservedCells = revealKinds[roomId] != RevealNone ? 1 : 0;
		reservedCells += perchTags[roomId] > 0 ? 1 : 0;
		reservedCells += liftTags[roomId] > 0 ? 1 : 0;
		reservedCells += (Rooms[roomId].isArena || Rooms[roomId].isHub) ? 1 : 0;
		return reservedCells;
	};
	auto IsFluidCellCandidate = [&](int roomId, int x, int y) -> bool
	{
		if (x < 0 || x >= W || y < 0 || y >= H) return false;
		// A lift's bypass ring belongs to the whole host room. Fluid geometry is
		// planned after lifts and can extend beyond its nominated cell, so preserve
		// that ring by moving the optional basin/watercourse to another room.
		if (liftTags[roomId] > 0) return false;
		const ProcGenCell& cell = Grid[y][x];
		if (!cell.present || cell.roomId != roomId || cell.hasPlayerStart ||
			cell.hasKey || cell.hasExit || cell.hasBoss || cell.isLocked ||
			CellHasHeightTransition(x, y) || IsLandmarkAnchorCell(roomId, x, y))
			return false;
		return !((x == revealCellX[roomId] && y == revealCellY[roomId]) ||
			(x == perchCellX[roomId] && y == perchCellY[roomId]) ||
			(x == liftCellX[roomId] && y == liftCellY[roomId]));
	};
	auto CollectFluidCells = [&](int roomId) -> TArray<std::pair<int, int>>
	{
		TArray<std::pair<int, int>> candidates;
		for (int y = 0; y < H; y++)
			for (int x = 0; x < W; x++)
				if (IsFluidCellCandidate(roomId, x, y))
					candidates.Push(std::make_pair(x, y));
		return candidates;
	};
	auto FluidCellClearance = [&](int x, int y) -> double
	{
		return std::min(std::min(EdgeForCell(x, y, DIR_N), EdgeForCell(x, y, DIR_S)),
			std::min(EdgeForCell(x, y, DIR_W), EdgeForCell(x, y, DIR_E)));
	};
	auto BuildFluidDescriptor = [&](int roomId, int architecture,
		FluidDescriptor& descriptor) -> bool
	{
		descriptor = FluidDescriptor();
		descriptor.architecture = architecture;
		const RoomInfo& room = Rooms[roomId];
		const int existingReservations = FluidReservationCost(roomId);

		struct FluidPathCandidate
		{
			int x = 0;
			int y = 0;
			int direction = DIR_E;
			int turn = -1;
			int turnAt = 999;
			int length = 0;
		};

		if (architecture <= FluidFloodedGrotto)
		{
			if (room.cellCount < existingReservations + 2) return false;
			if ((architecture == FluidCentralPool || architecture == FluidTrenchPool) &&
				Size >= 3 && (room.spatialClass < 2 || roomHalfX[roomId] < 144.0 ||
					roomHalfY[roomId] < 144.0))
				return false;
			TArray<std::pair<int, int>> candidates = CollectFluidCells(roomId);
			if (candidates.Size() == 0) return false;

			// The old paired family was two tiny mirror-image pits in one cell. It is
			// now a broad two/three-cell reservoir divided by a traversable causeway.
			if (architecture == FluidPairedPools)
			{
				TArray<FluidPathCandidate> reservoirs;
				for (int length : { 3, 2 })
				{
					if (room.cellCount < existingReservations + length + 1) continue;
					for (int y = 0; y < H; y++)
					{
						for (int x = 0; x < W; x++)
						{
							for (int direction : { DIR_E, DIR_S })
							{
								bool valid = IsFluidCellCandidate(roomId, x, y);
								int px = x;
								int py = y;
								for (int step = 1; valid && step < length; step++)
								{
									if (!Grid[py][px].conn[direction]) { valid = false; break; }
									px += DX[direction];
									py += DY[direction];
									valid = IsFluidCellCandidate(roomId, px, py);
								}
								if (valid) reservoirs.Push({ x, y, direction, -1, 999, length });
							}
						}
					}
					if (reservoirs.Size() > 0) break;
				}
				if (reservoirs.Size() == 0) return false;
				const FluidPathCandidate& selected = reservoirs[RNG() % reservoirs.Size()];
				int x = selected.x;
				int y = selected.y;
				for (int step = 0; step < selected.length; step++)
				{
					descriptor.cells.Push(std::make_pair(x, y));
					x += DX[selected.direction];
					y += DY[selected.direction];
				}
				descriptor.bridge = true;
				return true;
			}

			// Flooded halls and irregular reservoirs follow a broad two/three-cell
			// bay. A former 2x2 bounding-box pool crossed the internal support at the
			// four-cell junction; a linear or bent watercourse is both larger than a
			// local pit and topologically honest in Doom's 2-D sector model.
			if (architecture == FluidFloodedGrotto || architecture == FluidIrregularPool)
			{
				TArray<FluidPathCandidate> poolRuns;
				for (int length : { 3, 2 })
				{
					if (room.cellCount < existingReservations + length + 1) continue;
					for (int y = 0; y < H; y++)
					{
						for (int x = 0; x < W; x++)
						{
							for (int direction : { DIR_E, DIR_S })
							{
								bool valid = IsFluidCellCandidate(roomId, x, y);
								int px = x;
								int py = y;
								for (int step = 1; valid && step < length; step++)
								{
									if (!Grid[py][px].conn[direction]) { valid = false; break; }
									px += DX[direction];
									py += DY[direction];
									valid = IsFluidCellCandidate(roomId, px, py);
								}
								if (valid) poolRuns.Push({ x, y, direction, -1, 999, length });
							}
						}
					}
					if (poolRuns.Size() > 0) break;
				}
				if (poolRuns.Size() > 0)
				{
					const FluidPathCandidate& run = poolRuns[RNG() % poolRuns.Size()];
					int x = run.x;
					int y = run.y;
					for (int step = 0; step < run.length; step++)
					{
						descriptor.cells.Push(std::make_pair(x, y));
						x += DX[run.direction];
						y += DY[run.direction];
					}
					return true;
				}
			}

			// Prefer the broadest remaining cell so even compact local pools occupy
			// a substantial part of the room rather than another 128-unit puddle.
			double bestClearance = -1.0;
			TArray<std::pair<int, int>> broadest;
			for (const auto& cell : candidates)
			{
				const double clearance = FluidCellClearance(cell.first, cell.second);
				if (clearance > bestClearance + 0.001)
				{
					bestClearance = clearance;
					broadest.Clear();
				}
				if (fabs(clearance - bestClearance) < 0.001) broadest.Push(cell);
			}
			descriptor.cells.Push(broadest[RNG() % broadest.Size()]);
			return true;
		}
		TArray<FluidPathCandidate> paths;
		if (architecture == FluidBendRiver)
		{
			if (room.cellCount < existingReservations + 5 ||
				roomHalfX[roomId] < 104.0 || roomHalfY[roomId] < 104.0)
				return false;
			static const int Perpendicular[4][2] = {
				{ DIR_W, DIR_E }, { DIR_W, DIR_E },
				{ DIR_N, DIR_S }, { DIR_N, DIR_S },
			};
			for (int totalLength = 6; totalLength >= 4 && paths.Size() == 0; totalLength--)
			{
				if (room.cellCount < existingReservations + totalLength + 1) continue;
				for (int y = 0; y < H; y++)
				{
					for (int x = 0; x < W; x++)
					{
						if (!IsFluidCellCandidate(roomId, x, y)) continue;
						for (int direction = 0; direction < 4; direction++)
						{
							for (int firstCells = 2; firstCells <= 4; firstCells++)
							{
								const int secondCells = totalLength - firstCells + 1;
								if (secondCells < 2 || secondCells > 4) continue;
								int mx = x;
								int my = y;
								bool firstValid = true;
								for (int step = 1; step < firstCells; step++)
								{
									if (!Grid[my][mx].conn[direction]) { firstValid = false; break; }
									mx += DX[direction];
									my += DY[direction];
									firstValid = IsFluidCellCandidate(roomId, mx, my);
									if (!firstValid) break;
								}
								if (!firstValid) continue;
								for (int turn : Perpendicular[direction])
								{
									int ex = mx;
									int ey = my;
									bool secondValid = true;
									for (int step = 1; step < secondCells; step++)
									{
										if (!Grid[ey][ex].conn[turn]) { secondValid = false; break; }
										ex += DX[turn];
										ey += DY[turn];
										secondValid = IsFluidCellCandidate(roomId, ex, ey);
										if (!secondValid) break;
									}
									if (secondValid)
										paths.Push({ x, y, direction, turn, firstCells, totalLength });
								}
							}
						}
					}
			}
			}
		}
		else
		{
			for (int length : { 6, 5, 4, 3, 2 })
			{
				if (room.cellCount < existingReservations + length + 1) continue;
				for (int y = 0; y < H; y++)
				{
					for (int x = 0; x < W; x++)
					{
						for (int direction : { DIR_E, DIR_S })
						{
							const bool horizontal = direction == DIR_E;
							if ((horizontal && roomHalfY[roomId] < 104.0) ||
								(!horizontal && roomHalfX[roomId] < 104.0))
								continue;
							bool valid = IsFluidCellCandidate(roomId, x, y);
							int px = x;
							int py = y;
							for (int step = 1; valid && step < length; step++)
							{
								if (!Grid[py][px].conn[direction]) { valid = false; break; }
								px += DX[direction];
								py += DY[direction];
								valid = IsFluidCellCandidate(roomId, px, py);
							}
							if (!valid) continue;
							FluidPathCandidate path;
							path.x = x;
							path.y = y;
							path.direction = direction;
							path.length = length;
							paths.Push(path);
						}
					}
				}
				if (paths.Size() > 0) break;
			}
		}
		if (paths.Size() == 0)
		{
			// Compact layouts may not merge enough cells for a room-spanning run.
			// Retain the watercourse family as a long, banked one-cell channel
			// instead of silently collapsing every constrained case to a pond.
			if (room.cellCount < existingReservations + 2 ||
				roomHalfX[roomId] < 136.0 || roomHalfY[roomId] < 136.0)
				return false;
			TArray<std::pair<int, int>> candidates = CollectFluidCells(roomId);
			if (candidates.Size() == 0) return false;
			descriptor.cells.Push(candidates[RNG() % candidates.Size()]);
			return true;
		}
		const FluidPathCandidate& selected = paths[RNG() % paths.Size()];
		int x = selected.x;
		int y = selected.y;
		descriptor.cells.Push(std::make_pair(x, y));
		for (int step = 1; step < selected.length; step++)
		{
			const int direction = selected.turn < 0 || step < selected.turnAt ?
				selected.direction : selected.turn;
			x += DX[direction];
			y += DY[direction];
			descriptor.cells.Push(std::make_pair(x, y));
		}
		return true;
	};
	auto ChooseFluidKind = [&](const RoomInfo& room, int ordinal) -> int
	{
		const bool hazardous = (ordinal & 1) != 0;
		if (themeStyle == ThemeHell)
			return hazardous ? FluidLava : FluidBlood;
		if (themeStyle == ThemeGothic)
		{
			if (hazardous) return FluidLava;
			return (ordinal % 3) == 0 ? FluidBlood : FluidWater;
		}
		if (themeStyle == ThemeCorrupted)
		{
			const bool infernalPhase = room.lockStage >= 2;
			return hazardous ? (infernalPhase ? FluidLava : FluidNukage) :
				(infernalPhase ? FluidBlood : FluidWater);
		}
		return hazardous ? FluidNukage : FluidWater;
	};
	// One optional room becomes a genuinely flooded space. Its room sector is the
	// liquid rather than a decorative inset; a dry island retains the room's
	// existing actors and rewards. Optional rooms are strongly preferred; when a
	// constrained layout uses a route room, the liquid is harmless and its dry
	// island/64-unit portals keep traversal safe.
	int floodedRoomId = -1;
	int floodedRoomScore = -1;
	for (unsigned int ri = 0; ri < Rooms.Size(); ri++)
	{
			const RoomInfo& room = Rooms[ri];
			if (room.cellCount < 3 || room.hasPlayerStart || room.hasKey ||
				room.hasExit || room.hasBoss || room.isLocked || room.isSecret ||
				room.reservedSecret || revealKinds[ri] != RevealNone ||
				perchTags[ri] > 0 || liftTags[ri] > 0)
				continue;
		const int score = room.cellCount * 100 + (!room.onMainPath ? 400 : 0) +
			(room.isDeadEnd ? 200 : 0) + room.spatialClass * 50;
		if (score > floodedRoomScore)
		{
			floodedRoomScore = score;
			floodedRoomId = ri;
		}
	}
	if (floodedRoomId >= 0)
	{
		FluidDescriptor flooded;
		flooded.architecture = FluidFloodedGrotto;
		flooded.kind = ChooseFluidKind(Rooms[floodedRoomId], 0);
		flooded.primary = true;
		flooded.floodedRoom = true;
		const double requestedIslandHalfX = clamp(roomHalfX[floodedRoomId] - 24.0,
			64.0, 112.0);
		const double requestedIslandHalfY = clamp(roomHalfY[floodedRoomId] - 24.0,
			64.0, 112.0);
		flooded.islandHalfX = requestedIslandHalfX;
		flooded.islandHalfY = requestedIslandHalfY;
		// A composed room can be broad while its first scan-order cell sits beside
		// an apse, portal shoulder, or chamfered exterior edge.  A dry island
		// sized from the aggregate room at that cell can touch the liquid shell,
		// producing a branched sector boundary.  Pick a cell that can actually
		// contain the island and retain a small wall/portal margin; the flooded
		// grotto is optional, so omit it rather than emit ambiguous geometry.
		constexpr double IslandShellMargin = 24.0;
		double bestIslandHalfX = 0.0;
		double bestIslandHalfY = 0.0;
		uint32_t bestIslandTie = UINT32_MAX;
		for (int y = 0; y < H; y++)
		{
			for (int x = 0; x < W; x++)
			{
				if (!Grid[y][x].present || Grid[y][x].roomId != floodedRoomId) continue;
				flooded.cells.Push(std::make_pair(x, y));
				if (CellHasHeightTransition(x, y) ||
					(x == revealCellX[floodedRoomId] && y == revealCellY[floodedRoomId]) ||
					(x == perchCellX[floodedRoomId] && y == perchCellY[floodedRoomId]) ||
					(x == liftCellX[floodedRoomId] && y == liftCellY[floodedRoomId]))
					continue;
				const double availableHalfX = std::min(EdgeForCell(x, y, DIR_W),
					EdgeForCell(x, y, DIR_E)) - IslandShellMargin;
				const double availableHalfY = std::min(EdgeForCell(x, y, DIR_N),
					EdgeForCell(x, y, DIR_S)) - IslandShellMargin;
				const double candidateHalfX = std::min(requestedIslandHalfX, availableHalfX);
				const double candidateHalfY = std::min(requestedIslandHalfY, availableHalfY);
				if (candidateHalfX < 64.0 || candidateHalfY < 64.0) continue;
				const double candidateClearance = std::min(candidateHalfX, candidateHalfY);
				const double bestClearance = std::min(bestIslandHalfX, bestIslandHalfY);
				const uint32_t tie = StableRoomHash(floodedRoomId,
					0x464c4f4fu ^ (uint32_t)(y * W + x + 1));
				if (flooded.islandX >= 0 &&
					(candidateClearance < bestClearance ||
						(candidateClearance == bestClearance && tie >= bestIslandTie)))
					continue;
				flooded.islandX = x;
				flooded.islandY = y;
				flooded.islandHalfX = candidateHalfX;
				flooded.islandHalfY = candidateHalfY;
				bestIslandHalfX = candidateHalfX;
				bestIslandHalfY = candidateHalfY;
				bestIslandTie = tie;
			}
		}
		if (flooded.islandX >= 0)
			fluidDescriptors[floodedRoomId] = std::move(flooded);
	}
	TArray<int> fluidRooms;
	// Liquid frequency scales with map area as well as detail. Large layouts need
	// repeated reservoirs and watercourses to remain a compositional theme instead
	// of shrinking to a few isolated accents on an enormous automap.
	int fluidBudgetTarget = Detail == 0 ? 1 + Size / 4 :
		(Detail == 2 ? 3 + (Size * 4) / 3 : 2 + Size);
	if (Outdoors == 2) fluidBudgetTarget += 1 + Size / 8;
	if (themeStyle == ThemeHell || themeStyle == ThemeIndustrial)
		fluidBudgetTarget += 1 + Size / 10;
	for (int pass = 0; pass < 3; pass++)
	{
		for (unsigned int ri = 0; ri < Rooms.Size(); ri++)
		{
			const RoomInfo& room = Rooms[ri];
			if (room.isSecret || room.cellCount < 2)
				continue;
			bool alreadySelected = false;
			for (unsigned int selected = 0; selected < fluidRooms.Size(); selected++)
				if (fluidRooms[selected] == (int)ri) alreadySelected = true;
			if (alreadySelected) continue;
			const bool landmark = room.isArena || room.isHub || outdoorRooms[ri];
			const bool optional = room.isDeadEnd || !room.onMainPath;
			if ((pass == 0 && !landmark) || (pass == 1 && !optional) ||
				(pass == 2 && (landmark || optional)))
				continue;
			if (room.cellCount < FluidReservationCost(ri) + 2 ||
				CollectFluidCells(ri).Size() == 0) continue;
			fluidRooms.Push(ri);
		}
	}
	bool primaryFluidPlaced = primaryFluidRoom >= 0 &&
		fluidDescriptors[primaryFluidRoom].architecture >= 0;
	if (primaryFluidRoom >= 0 && !primaryFluidPlaced)
	{
		static const int PrimaryFluidArchitectures[] = {
			FluidBendRiver, FluidStraightRiver, FluidFloodedGrotto,
			FluidStaggeredRiver, FluidIrregularPool, FluidPairedPools,
		};
		const int firstArchitecture =
			(int)(variantSeed % countof(PrimaryFluidArchitectures));
		for (unsigned int fallback = 0;
			fallback < countof(PrimaryFluidArchitectures); fallback++)
		{
			FluidDescriptor descriptor;
			const int architecture = PrimaryFluidArchitectures[
				(firstArchitecture + fallback) % countof(PrimaryFluidArchitectures)];
			if (!BuildFluidDescriptor(primaryFluidRoom, architecture, descriptor) ||
				descriptor.cells.Size() < 2)
				continue;
			descriptor.kind = ChooseFluidKind(Rooms[primaryFluidRoom], 0);
			descriptor.primary = true;
			descriptor.bridge = descriptor.bridge ||
				(descriptor.cells.Size() >= 3 && architecture <= FluidFloodedGrotto);
			fluidDescriptors[primaryFluidRoom] = std::move(descriptor);
			primaryFluidPlaced = true;
			break;
		}
	}
	int macroFluidPlaced = 0;
	for (unsigned int ri = 0; ri < fluidDescriptors.Size(); ri++)
		if (fluidDescriptors[ri].primary) macroFluidPlaced++;
	// A flooded room plus multiple broad reservoirs/watercourses establishes a
	// regional liquid identity. Two macro descriptors could still fall below a
	// meaningful share of an unusually broad composed floor plan, so standard
	// maps target three and large maps target four when compatible hosts exist.
	const int macroFluidTarget = Size >= 20 ? 4 : (Size >= 5 ? 3 : 1);
	for (int ordinal = macroFluidPlaced;
		ordinal < macroFluidTarget; ordinal++)
	{
		bool placed = false;
		TArray<uint8_t> infeasible;
		infeasible.Resize(Rooms.Size());
		for (unsigned int ri = 0; ri < infeasible.Size(); ri++) infeasible[ri] = 0;
		for (unsigned int attempt = 0; attempt < fluidRooms.Size() && !placed; attempt++)
		{
			int bestRoom = -1;
			int bestCells = -1;
			for (unsigned int candidate = 0; candidate < fluidRooms.Size(); candidate++)
			{
				const int roomId = fluidRooms[candidate];
				if (fluidDescriptors[roomId].architecture >= 0 || infeasible[roomId]) continue;
				if (Rooms[roomId].cellCount > bestCells)
				{
					bestCells = Rooms[roomId].cellCount;
					bestRoom = roomId;
				}
			}
			if (bestRoom < 0) break;
			for (int architecture : { FluidStraightRiver, FluidFloodedGrotto,
				FluidStaggeredRiver, FluidIrregularPool, FluidBendRiver,
				FluidPairedPools })
			{
				FluidDescriptor descriptor;
				if (!BuildFluidDescriptor(bestRoom, architecture, descriptor) ||
					descriptor.cells.Size() < 2)
					continue;
				descriptor.kind = ChooseFluidKind(Rooms[bestRoom], ordinal);
				descriptor.primary = true;
				descriptor.bridge = descriptor.bridge ||
					(descriptor.cells.Size() >= 3 && architecture <= FluidFloodedGrotto);
				fluidDescriptors[bestRoom] = std::move(descriptor);
				placed = true;
				macroFluidPlaced++;
				break;
			}
			if (!placed) infeasible[bestRoom] = 1;
		}
		if (!placed) break;
	}
	RankRoomsForMotif(fluidRooms, PGFM_Watercourse, 0x464c5544u);
	const unsigned int fluidCount = std::min(fluidRooms.Size(),
		(unsigned int)fluidBudgetTarget);
	bool hasRoomScaleFluid = primaryFluidPlaced;
	for (unsigned int index = 0; index < fluidCount; index++)
	{
		const int roomId = fluidRooms[index];
		if (fluidDescriptors[roomId].architecture >= 0) continue;
		int requested = (int)((variantSeed + index * 3u) % FluidArchitectureCount);
		FluidDescriptor descriptor;
		bool placed = false;
		for (int fallback = 0; fallback < FluidArchitectureCount; fallback++)
		{
			const int architecture = (requested + fallback) % FluidArchitectureCount;
			if (BuildFluidDescriptor(roomId, architecture, descriptor))
			{
				placed = true;
				break;
			}
		}
		if (!placed) continue;
		descriptor.kind = ChooseFluidKind(Rooms[roomId], index);
		fluidDescriptors[roomId] = std::move(descriptor);
		hasRoomScaleFluid |= fluidDescriptors[roomId].cells.Size() > 1;
	}
	// If any composed room can support it, force one genuinely room-scale liquid
	// descriptor. A one-cell fallback remains valid on constrained layouts but no
	// longer satisfies the map's river/grotto showcase by itself.
	if (!hasRoomScaleFluid)
	{
		for (unsigned int index = 0; index < fluidCount && !hasRoomScaleFluid; index++)
		{
			const int roomId = fluidRooms[index];
			FluidDescriptor river;
			for (int architecture : { FluidFloodedGrotto, FluidIrregularPool,
				FluidStaggeredRiver, FluidStraightRiver, FluidBendRiver })
			{
				if (!BuildFluidDescriptor(roomId, architecture, river)) continue;
				if (river.cells.Size() <= 1) continue;
				river.kind = fluidDescriptors[roomId].kind;
				fluidDescriptors[roomId] = std::move(river);
				hasRoomScaleFluid = true;
				break;
			}
		}
		for (unsigned int index = fluidCount;
			index < fluidRooms.Size() && !hasRoomScaleFluid; index++)
		{
			const int roomId = fluidRooms[index];
			FluidDescriptor river;
			for (int architecture : { FluidFloodedGrotto, FluidIrregularPool,
				FluidStaggeredRiver, FluidStraightRiver, FluidBendRiver })
			{
				if (!BuildFluidDescriptor(roomId, architecture, river)) continue;
				if (river.cells.Size() <= 1) continue;
				const int replacedRoom = fluidRooms[fluidCount - 1];
				river.kind = fluidDescriptors[replacedRoom].kind;
				fluidDescriptors[replacedRoom] = FluidDescriptor();
				fluidDescriptors[roomId] = std::move(river);
				hasRoomScaleFluid = true;
				break;
			}
		}
	}
	int placedFluids = 0;
	for (unsigned int ri = 0; ri < fluidDescriptors.Size(); ri++)
	{
		const FluidDescriptor& descriptor = fluidDescriptors[ri];
		if (descriptor.architecture < 0) continue;
		placedFluids++;
		for (const auto& cell : descriptor.cells)
			fluidCellReserved[cell.second * W + cell.first] = true;
	}
	if (placedFluids == 0)
	{
		LastError = "Could not place safely bypassable fluid architecture";
		return false;
	}

	// Adjacent rooms that are deliberately not traversally connected can still
	// preview one another through a raised, framed opening. These windows add
	// crossfire, landmarks, and route comprehension while their 48-unit sill
	// keeps the progression graph exactly as planned.
	TArray<TArray<uint8_t>> sightlineMask;
	sightlineMask.Resize(H);
	for (int y = 0; y < H; y++)
	{
		sightlineMask[y].Resize(W);
		for (int x = 0; x < W; x++) sightlineMask[y][x] = 0;
	}
	struct SightlineCandidate
	{
		int x = 0;
		int y = 0;
		int direction = DIR_E;
		int score = 0;
	};
	TArray<SightlineCandidate> sightlineCandidates;
	for (int y = 0; y < H; y++)
	{
		for (int x = 0; x < W; x++)
		{
			if (!Grid[y][x].present) continue;
			for (int direction : { DIR_E, DIR_S })
			{
				const int nx = x + DX[direction];
				const int ny = y + DY[direction];
				if (nx < 0 || nx >= W || ny < 0 || ny >= H ||
					!Grid[ny][nx].present || Grid[y][x].conn[direction])
					continue;
				const int roomA = Grid[y][x].roomId;
				const int roomB = Grid[ny][nx].roomId;
				if (!IsValidRoom(roomA) || !IsValidRoom(roomB) || roomA == roomB ||
					Rooms[roomA].lockStage != Rooms[roomB].lockStage)
					continue;
				const RoomInfo& first = Rooms[roomA];
				const RoomInfo& second = Rooms[roomB];
				if (first.isSecret || second.isSecret || first.isLocked || second.isLocked ||
					first.hasPlayerStart || second.hasPlayerStart || first.hasKey || second.hasKey ||
					first.hasExit || second.hasExit || first.hasBoss || second.hasBoss ||
					switchTargetTags[roomA] > 0 || switchTargetTags[roomB] > 0)
					continue;
				if (revealArchitectures[roomA] == RevealFalseWall &&
					revealCellX[roomA] == x && revealCellY[roomA] == y &&
					revealDoorSides[roomA] == WallSideForGridDirection[direction])
					continue;
				if (revealArchitectures[roomB] == RevealFalseWall &&
					revealCellX[roomB] == nx && revealCellY[roomB] == ny &&
					revealDoorSides[roomB] == WallSideForGridDirection[OPP[direction]])
					continue;
				const double sillFloor = std::max(first.floorZ, second.floorZ) + 48.0;
				const double windowCeil = std::min(first.ceilZ, second.ceilZ) - 24.0;
				if (windowCeil - sillFloor < 64.0) continue;
				int score = abs(first.progressionRank - second.progressionRank) * 12 +
					(first.spatialClass + second.spatialClass) * 18 + (RNG() % 17);
				if (first.featureMotif == PGFM_SightlineRecon ||
					second.featureMotif == PGFM_SightlineRecon)
					score += 1000;
				if (first.encounterCard == PGEC_Crossfire ||
					second.encounterCard == PGEC_Crossfire)
					score += 400;
				sightlineCandidates.Push({ x, y, direction, score });
			}
		}
	}
	for (unsigned int first = 0; first < sightlineCandidates.Size(); first++)
	{
		for (unsigned int second = first + 1; second < sightlineCandidates.Size(); second++)
		{
			if (sightlineCandidates[second].score <= sightlineCandidates[first].score) continue;
			const SightlineCandidate saved = sightlineCandidates[first];
			sightlineCandidates[first] = sightlineCandidates[second];
			sightlineCandidates[second] = saved;
		}
	}
	int sightlineBudget = Detail == 0 ? 1 + Size / 8 :
		(Detail == 2 ? 2 + Size / 2 : 1 + Size / 3);
	for (const RoomInfo& room : Rooms)
		if (room.featureMotif == PGFM_SightlineRecon) sightlineBudget++;
	for (unsigned int index = 0;
		index < sightlineCandidates.Size() && sightlineBudget > 0; index++)
	{
		const SightlineCandidate& candidate = sightlineCandidates[index];
		const int nx = candidate.x + DX[candidate.direction];
		const int ny = candidate.y + DY[candidate.direction];
		if (sightlineMask[candidate.y][candidate.x] != 0 ||
			sightlineMask[ny][nx] != 0)
			continue;
		sightlineMask[candidate.y][candidate.x] |= 1u << candidate.direction;
		sightlineMask[ny][nx] |= 1u << OPP[candidate.direction];
		sightlineBudget--;
	}

	// The room planner deliberately makes only real dead-end limbs into secret
	// rooms.  Macro rings and fork-rejoins can leave a fair, large map with too
	// few such limbs, however.  Fill only that shortfall with compact manual
	// annexes in independently reachable rooms.  The annex owns its own hidden
	// Door_Raise slab, so the host remains an ordinary, fully reachable room.
	const int requiredSecretCount = Size <= 1 ? 1 : (Size <= 4 ? 2 : std::min(8,
		std::min(2 + Size / 2, 3 + Size / 3 + (Detail >= 1 ? 1 : 0) +
			(Detail == 2 ? 1 : 0))));
	int plannedSecretRooms = 0;
	for (const RoomInfo& room : Rooms)
		if (room.isSecret) plannedSecretRooms++;
	int secretAnnexBudget = std::max(0, requiredSecretCount - plannedSecretRooms);
	struct SecretAnnexCandidate
	{
		int roomId = -1;
		int x = -1;
		int y = -1;
		int doorSide = -1;
		uint32_t score = 0;
	};
	auto SecretAnnexHash = [&](int roomId, int x, int y) -> uint32_t
	{
		uint32_t value = StableRoomHash(roomId, 0x53454352u) ^
			(uint32_t)(x + 1) * 0x85ebca6bu ^ (uint32_t)(y + 1) * 0xc2b2ae35u;
		value ^= value >> 16;
		value *= 0x7feb352du;
		return value ^ (value >> 15);
	};
	for (int pass = 0; pass < 4 && secretAnnexBudget > 0; ++pass)
	{
		// Prefer unused off-route rooms, then unused main-route rooms. If a dense
		// blueprint has already spent every such room on a planned set piece, the
		// final two passes may use a non-anchor cell in a district/feature room.
		// The individual-cell exclusions below keep that last-resort annex out of
		// the landmark, cache, perch, lift, and all mandatory circulation cells.
		const bool allowDistrictRoom = pass >= 2;
		const bool allowFeatureRoom = pass >= 3;
		TArray<SecretAnnexCandidate> candidates;
		for (unsigned int ri = 0; ri < Rooms.Size(); ++ri)
		{
			const RoomInfo& room = Rooms[ri];
			if (room.cellCount < 2 || room.hasPlayerStart || room.hasKey ||
				room.hasExit || room.hasBoss || room.isLocked || room.isSecret ||
				room.reservedSecret ||
				(!allowDistrictRoom && (room.isArena || room.isHub)) ||
				room.ceilZ - room.floorZ < 128.0 ||
				(!allowFeatureRoom &&
					(revealKinds[ri] != RevealNone || perchTags[ri] > 0)) ||
				liftTags[ri] > 0 ||
				fluidDescriptors[ri].architecture >= 0 ||
				(pass == 0 && room.onMainPath) ||
				(pass == 1 && !room.onMainPath))
				continue;
			if (!CanHostReveal((int)ri, RevealSwitchCache)) continue;
			for (int y = 0; y < H; ++y)
			{
				for (int x = 0; x < W; ++x)
				{
					const ProcGenCell& cell = Grid[y][x];
					if (!cell.present || cell.roomId != (int)ri || cell.hasPlayerStart ||
						cell.hasKey || cell.hasExit || cell.hasBoss || cell.isLocked ||
						CellHasHeightTransition(x, y) || IsLandmarkAnchorCell((int)ri, x, y) ||
						(x == revealCellX[ri] && y == revealCellY[ri]) ||
						(x == perchCellX[ri] && y == perchCellY[ri]) ||
						(x == liftCellX[ri] && y == liftCellY[ri]) ||
						fluidCellReserved[y * W + x] || sightlineMask[y][x] != 0)
						continue;
					bool opensIntoRoom = false;
					for (int direction = 0; direction < 4; ++direction)
					{
						const int nx = x + DX[direction];
						const int ny = y + DY[direction];
						if (nx >= 0 && nx < W && ny >= 0 && ny < H &&
							Grid[ny][nx].present && Grid[ny][nx].roomId == (int)ri)
						{
							opensIntoRoom = true;
							break;
						}
					}
					if (!opensIntoRoom) continue;
					const int doorSide = ChooseRevealDoorSide((int)ri, x, y,
						RevealSwitchCache);
					candidates.Push({ (int)ri, x, y, doorSide,
						SecretAnnexHash((int)ri, x, y) });
				}
			}
		}
		for (int index = 0; index < (int)candidates.Size() && secretAnnexBudget > 0;
			++index)
		{
			int best = index;
			for (int candidate = index + 1; candidate < (int)candidates.Size(); ++candidate)
				if (candidates[candidate].score > candidates[best].score) best = candidate;
			if (best != index)
			{
				const SecretAnnexCandidate saved = candidates[index];
				candidates[index] = candidates[best];
				candidates[best] = saved;
			}
			const SecretAnnexCandidate& selected = candidates[index];
			if (secretAnnexCellX[selected.roomId] >= 0) continue;
			secretAnnexCellX[selected.roomId] = selected.x;
			secretAnnexCellY[selected.roomId] = selected.y;
			secretAnnexDoorSides[selected.roomId] = selected.doorSide;
			secretAnnexVariants[selected.roomId] =
				(int)(selected.score % 4u);
			if (Rooms[selected.roomId].manualInteraction == PGMI_None)
				Rooms[selected.roomId].manualInteraction = PGMI_SecretDoor;
			secretAnnexBudget--;
		}
	}

	// A rectangular composed room can be serialized as one genuine exterior
	// envelope instead of a row of individually closed chamber pods. Normalize
	// only inward to the smallest already-reserved face: connector depth can
	// grow, but no portal span or protected threshold is ever consumed. Complex
	// L/T/courtyard and feature-host rooms retain the conservative cell shell
	// below and are reported as such in the realized contour metadata.
	TArray<bool> unifiedEnvelopeRooms;
	unifiedEnvelopeRooms.Resize(Rooms.Size());
	for (unsigned int ri = 0; ri < Rooms.Size(); ++ri)
	{
		unifiedEnvelopeRooms[ri] = false;
		const RoomInfo& room = Rooms[ri];
		if (room.id < 0 || room.cellCount < 2 || room.hasPlayerStart ||
			room.hasKey || room.hasExit || room.hasBoss || room.isLocked ||
			room.isSecret || room.verticalAnchor || room.terrainRouteReservation ||
			revealKinds[ri] != RevealNone || switchTargetTags[ri] > 0 ||
			secretAnnexCellX[ri] >= 0 ||
			perchTags[ri] > 0 || liftTags[ri] > 0 ||
			fluidDescriptors[ri].architecture >= 0)
			continue;
		const int rectangleWidth = room.maxI - room.minI + 1;
		const int rectangleHeight = room.maxJ - room.minJ + 1;
		if (rectangleWidth <= 0 || rectangleHeight <= 0 ||
			rectangleWidth * rectangleHeight != room.cellCount)
			continue;

		bool complete = true;
		double commonHalfX = DBL_MAX;
		double commonHalfY = DBL_MAX;
		double requiredHalfX = 0.0;
		double requiredHalfY = 0.0;
		for (int cellY = room.minJ; cellY <= room.maxJ && complete; ++cellY)
		{
			for (int cellX = room.minI; cellX <= room.maxI; ++cellX)
			{
				if (cellX < 0 || cellX >= W || cellY < 0 || cellY >= H ||
					!Grid[cellY][cellX].present || Grid[cellY][cellX].roomId != (int)ri)
				{
					complete = false;
					break;
				}
				commonHalfX = std::min(commonHalfX, std::min(
					EdgeForCell(cellX, cellY, DIR_W), EdgeForCell(cellX, cellY, DIR_E)));
				commonHalfY = std::min(commonHalfY, std::min(
					EdgeForCell(cellX, cellY, DIR_N), EdgeForCell(cellX, cellY, DIR_S)));
				for (int direction = 0; direction < 4; ++direction)
				{
					const int nx = cellX + DX[direction];
					const int ny = cellY + DY[direction];
					if (nx >= 0 && nx < W && ny >= 0 && ny < H &&
						Grid[ny][nx].present && Grid[ny][nx].roomId == (int)ri)
						continue;
					if (!Grid[cellY][cellX].conn[direction]) continue;
					const double required = Grid[cellY][cellX].connectionClearWidth[direction] * 0.5;
					if (direction == DIR_E || direction == DIR_W)
						requiredHalfY = std::max(requiredHalfY, required);
					else
						requiredHalfX = std::max(requiredHalfX, required);
				}
			}
		}
		if (!complete || commonHalfX + 0.001 < requiredHalfX ||
			commonHalfY + 0.001 < requiredHalfY)
			continue;

		for (int cellY = room.minJ; cellY <= room.maxJ; ++cellY)
		{
			for (int cellX = room.minI; cellX <= room.maxI; ++cellX)
			{
				cellEdges[cellY][cellX].edge[DIR_W] = commonHalfX;
				cellEdges[cellY][cellX].edge[DIR_E] = commonHalfX;
				cellEdges[cellY][cellX].edge[DIR_N] = commonHalfY;
				cellEdges[cellY][cellX].edge[DIR_S] = commonHalfY;
			}
		}
		unifiedEnvelopeRooms[ri] = true;
	}

	// Room sectors are shared by all chamber cells belonging to the composed
	// room. A unified envelope uses this one sector across its entire rectangle;
	// complex safe shells keep the existing per-cell boundary path.
	for (unsigned int ri = 0; ri < Rooms.Size(); ri++)
	{
		RoomInfo& room = Rooms[ri];
		if (room.id < 0) continue;
		const char* ceiling = outdoorRooms[ri] ? "F_SKY1" : SafeTexture(room.ceilTex, "CEIL3_5");
		int light = outdoorRooms[ri] ? std::max(room.light, 192) : std::max(room.light, 160);
		const FluidDescriptor& fluid = fluidDescriptors[ri];
		const bool flooded = fluid.architecture >= 0 && fluid.floodedRoom;
		const char* floor = flooded ?
			(fluid.kind == FluidWater ? "FWATER1" :
				(fluid.kind == FluidBlood ? "BLOOD1" :
					(fluid.kind == FluidNukage ? "NUKAGE1" : "LAVA1"))) :
			SafeTexture(room.floorTex, "FLOOR4_8");
		const double floorZ = flooded ? room.floorZ - 8.0 : room.floorZ;
		room.sectorIdx = AddSector(floorZ, room.ceilZ, floor, ceiling, light);
		sectors[room.sectorIdx].lightColor = room.lightColor;
		sectors[room.sectorIdx].fadeColor = outdoorRooms[ri] ? 0 : room.fadeColor;
		// ZDoom-namespace UDMF sector specials are already translated. Doom's raw
		// special 9 would therefore remain an ordinary non-secret effect; the
		// engine's canonical SECRET_MASK is the real automap/statistics flag.
		if (room.isSecret) sectors[room.sectorIdx].special = 0x0400;
	}
	auto ApplyRoomLighting = [&](int sectorIndex, const RoomInfo& room, bool sky)
	{
		if (sectorIndex < 0 || sectorIndex >= (int)sectors.Size()) return;
		sectors[sectorIndex].lightColor = room.lightColor;
		sectors[sectorIndex].fadeColor = sky ? 0 : room.fadeColor;
	};

	TArray<TArray<CellConnections>> connectionGrid;
	connectionGrid.Resize(H);
	for (int y = 0; y < H; y++) connectionGrid[y].Resize(W);
	TArray<CollisionCorridorProof> collisionCorridors;
	auto RecordCollisionCorridor = [&](int sourceX, int sourceY,
		int targetX, int targetY, int firstReservation)
	{
		const int reservationCount = (int)navigationReservations.Size() - firstReservation;
		if (reservationCount <= 0) return;
		CollisionCorridorProof proof;
		proof.sourceCell = sourceY * W + sourceX;
		proof.targetCell = targetY * W + targetX;
		proof.firstReservation = firstReservation;
		proof.reservationCount = reservationCount;
		collisionCorridors.Push(proof);
	};
	TArray<StairConnection> stairConnections;
	TArray<int> doglegFallbackAnchors;
	auto RecordDoglegFallback = [&](int roomId)
	{
		if (!IsValidRoom(roomId)) return;
		for (unsigned int index = 0; index < doglegFallbackAnchors.Size(); ++index)
			if (doglegFallbackAnchors[index] == roomId) return;
		doglegFallbackAnchors.Push(roomId);
	};

	TArray<std::pair<int, int>> doorPairs;
	auto PairHasDoor = [&](int roomA, int roomB) -> bool
	{
		int low = std::min(roomA, roomB);
		int high = std::max(roomA, roomB);
		for (const auto& pair : doorPairs)
			if (pair.first == low && pair.second == high) return true;
		return false;
	};
	auto RecordDoorPair = [&](int roomA, int roomB)
	{
		doorPairs.Push(std::make_pair(std::min(roomA, roomB), std::max(roomA, roomB)));
	};
	auto LockedDoorTexture = [&](int lockType) -> const char*
	{
		if (lockType == 1) return "BIGDOOR2";
		if (lockType == 2) return "BIGDOOR3";
		if (lockType == 3) return "BIGDOOR4";
		return "BIGDOOR1";
	};
	auto ChooseDoorProfile = [&](int roomA, int roomB, int lockType,
		bool secretDoor) -> DoorProfile
	{
		if (lockType > 0)
			return { LockedDoorTexture(lockType), 128, 128 };
		if (secretDoor)
		{
			// The face itself is replaced with the owning room's wall texture. A
			// compact stock-width opening keeps the hidden door indistinguishable
			// from an ordinary wall panel until used.
			const int height = ((Rooms[roomA].visualVariant + Rooms[roomB].visualVariant) & 1) ?
				96 : 128;
			return { nullptr, 64, height };
		}

		static const DoorProfile TechDoors[] = {
			{ "DOOR1", 64, 72 }, { "DOOR3", 64, 72 },
			{ "BIGDOOR1", 128, 96 }, { "BIGDOOR5", 128, 128 },
			{ "BIGDOOR7", 128, 128 }
		};
		static const DoorProfile IndustrialDoors[] = {
			// Keep the compact shutter on a distinct stable hash slot from the
			// reactor/loading-bay faces below.  Large Industrial maps often have
			// only a few ordinary doors; putting DOOR3 first made this particular
			// deterministic pair selection repeatedly choose only the two broad
			// panels.  This order retains the same IWAD-safe vocabulary while
			// letting the hash distribute a visibly smaller personnel shutter
			// among the loading-bay and machinery doors.
			{ "BIGDOOR1", 128, 96 }, { "DOOR3", 64, 72 },
			{ "BIGDOOR5", 128, 128 }, { "BIGDOOR7", 128, 128 }
		};
		static const DoorProfile HellDoors[] = {
			{ "BIGDOOR1", 128, 96 }, { "BIGDOOR6", 128, 112 },
			{ "BIGDOOR7", 128, 128 }, { "MARBFAC2", 128, 128 }
		};
		static const DoorProfile GothicDoors[] = {
			{ "BIGDOOR1", 128, 96 }, { "BIGDOOR6", 128, 112 },
			{ "BIGDOOR7", 128, 128 }, { "MARBFAC3", 128, 128 }
		};
		static const DoorProfile CorruptedDoors[] = {
			{ "DOOR1", 64, 72 }, { "BIGDOOR1", 128, 96 },
			{ "BIGDOOR6", 128, 112 }, { "BIGDOOR7", 128, 128 },
			{ "MARBFAC2", 128, 128 }
		};
		static const DoorProfile Doom2SpecialDoors[] = {
			{ "SPCDOOR1", 64, 128 }, { "SPCDOOR2", 64, 128 },
			{ "SPCDOOR3", 64, 128 }, { "SPCDOOR4", 64, 128 }
		};

		const int style = abs(roomA * 43 + roomB * 71 +
			Rooms[roomA].visualVariant * 11 + Rooms[roomB].visualVariant * 17 +
			Rooms[roomA].lockStage * 5);
		// Doom II computer/special doors appear primarily in Techbase and
		// Industrial maps, as they do in the stock campaign. They remain excluded
		// from Ultimate Doom, whose IWAD does not define the SPCDOOR family.
		if (ProcGenUsesDoom2Roster() &&
			(themeStyle == ThemeTechbase || themeStyle == ThemeIndustrial) &&
			(style % 5) == 0)
			return Doom2SpecialDoors[(style / 5) % countof(Doom2SpecialDoors)];

		const DoorProfile* profiles = TechDoors;
		int profileCount = countof(TechDoors);
		if (themeStyle == ThemeIndustrial)
		{
			profiles = IndustrialDoors;
			profileCount = countof(IndustrialDoors);
		}
		else if (themeStyle == ThemeHell)
		{
			profiles = HellDoors;
			profileCount = countof(HellDoors);
		}
		else if (themeStyle == ThemeGothic)
		{
			profiles = GothicDoors;
			profileCount = countof(GothicDoors);
		}
		else if (themeStyle == ThemeCorrupted)
		{
			profiles = CorruptedDoors;
			profileCount = countof(CorruptedDoors);
		}
		return profiles[style % profileCount];
	};

	int normalDoorBudget = 2 + Size;
	for (int y = 0; y < H; y++)
	{
		for (int x = 0; x < W; x++)
		{
			if (!Grid[y][x].present) continue;
			for (int direction : { DIR_E, DIR_S })
			{
				const bool sightline =
					(sightlineMask[y][x] & (1u << direction)) != 0;
				if (!Grid[y][x].conn[direction] && !sightline) continue;
				int nx = x + DX[direction];
				int ny = y + DY[direction];
				if (nx < 0 || nx >= W || ny < 0 || ny >= H || !Grid[ny][nx].present) continue;

				int roomA = Grid[y][x].roomId;
				int roomB = Grid[ny][nx].roomId;
				if (!IsValidRoom(roomA) || !IsValidRoom(roomB)) continue;
				const int opposite = OPP[direction];
				ProcGenCell& firstPlan = Grid[y][x];
				ProcGenCell& secondPlan = Grid[ny][nx];
				// Core planning assigns this stable hash before room merging. Carry it
				// through every concrete connector sector rather than deriving a phase
				// from each emitted stair sector.
				const int connectionAlignmentGroup = !sightline ?
					(firstPlan.connectionAlignmentGroup[direction] >= 0 ?
						firstPlan.connectionAlignmentGroup[direction] :
						secondPlan.connectionAlignmentGroup[opposite]) : -1;
				const int doglegAnchor = sightline ? -1 :
					DoglegAnchorForCells(x, y, nx, ny);
				const bool doglegRequested = doglegAnchor >= 0;
				// Keep the final physical emitter in lockstep with the earlier
				// connector reservations and the serialized visual proof.  A
				// non-dogleg eight-unit stair is still required travel: without this
				// predicate the emitter could reclassify its pre-reserved Standard
				// opening as Narrow after the terrain pass.
				const bool stairConnector = !sightline &&
					(firstPlan.connectionStairChain[direction] >= 0 ||
						secondPlan.connectionStairChain[opposite] >= 0 ||
						fabs(firstPlan.floorZ - secondPlan.floorZ) > 0.001);

				bool lockHere = !sightline && Grid[y][x].isLocked &&
					(Grid[y][x].lockDir < 0 || Grid[y][x].lockDir == direction);
				bool lockThere = !sightline && Grid[ny][nx].isLocked &&
					(Grid[ny][nx].lockDir < 0 || Grid[ny][nx].lockDir == OPP[direction]);
				int lockType = lockHere ? Grid[y][x].lockType : (lockThere ? Grid[ny][nx].lockType : 0);
				const bool crossesStage = Grid[y][x].lockStage != Grid[ny][nx].lockStage;
				if ((crossesStage && lockType <= 0) || (!crossesStage && lockType > 0))
				{
					LastError = crossesStage ?
						"A serialized opening would bypass a key stage" :
						"A keyed door was assigned inside one progression stage";
					return false;
				}

				bool secretDoor = !sightline && roomA != roomB &&
					(Rooms[roomA].isSecret || Rooms[roomB].isSecret);
				const bool protectedFeatureEndpoint =
					Rooms[roomA].hasPlayerStart || Rooms[roomA].hasKey || Rooms[roomA].hasExit ||
					Rooms[roomB].hasPlayerStart || Rooms[roomB].hasKey || Rooms[roomB].hasExit ||
					revealKinds[roomA] != RevealNone || revealKinds[roomB] != RevealNone ||
					perchTags[roomA] > 0 || perchTags[roomB] > 0 ||
					liftTags[roomA] > 0 || liftTags[roomB] > 0;
				bool door = lockType > 0 || secretDoor;
				const bool levelThreshold =
					fabs(Rooms[roomA].floorZ - Rooms[roomB].floorZ) < 0.001;
				// The start landmark is a guaranteed safe staging area. Its own
				// encounter budget is zero, and closed unlocked doors prevent
				// monsters in the first combat room from immediately flooding it.
				if (!sightline && !door && !protectedFeatureEndpoint && levelThreshold && roomA != roomB &&
					(Rooms[roomA].hasPlayerStart || Rooms[roomB].hasPlayerStart))
				{
					door = true;
					if (normalDoorBudget > 0) normalDoorBudget--;
				}
				if (!sightline && !door && !protectedFeatureEndpoint && levelThreshold && roomA != roomB &&
					normalDoorBudget > 0 && !PairHasDoor(roomA, roomB))
				{
					bool requested = Rooms[roomA].hasDoor || Rooms[roomB].hasDoor ||
						Rooms[roomA].hasKey || Rooms[roomB].hasKey;
					if (requested || ((Rooms[roomA].isArena || Rooms[roomB].isArena) && (RNG() % 100) < 18))
					{
						door = true;
						normalDoorBudget--;
					}
				}
				if (door && !levelThreshold)
				{
					// Locked, secret, and start-room doors are normalized to a
					// common terrace before this pass. Failing here is safer than
					// emitting a moving door with an impassable ledge under it.
					LastError = "A mandatory procedural door spans unequal floor heights";
					return false;
				}
					const bool mandatoryConnector = !sightline &&
						(lockType > 0 || stairConnector || doglegRequested ||
							(firstPlan.onMainPath && secondPlan.onMainPath) ||
						firstPlan.hasPlayerStart || firstPlan.hasKey || firstPlan.hasExit ||
						secondPlan.hasPlayerStart || secondPlan.hasKey || secondPlan.hasExit);
				int plannedClearWidth = sightline ? 128 : std::min(
					firstPlan.connectionClearWidth[direction],
					secondPlan.connectionClearWidth[opposite]);
					if (mandatoryConnector)
					{
						plannedClearWidth = std::max(128, plannedClearWidth);
					}
					DoorProfile doorProfile;
					if (door) doorProfile = ChooseDoorProfile(roomA, roomB, lockType, secretDoor);
					// A fitted stock panel is artwork, never permission to turn the
					// underlying transition into a 64-unit choke. Keep the complete
					// planned aperture for every door (and at least Standard for a
					// protected route); the alignment pass fits the native panel to that
					// physical opening after the final geometry is known.
					const int doorClearWidth = door ? std::max(128, plannedClearWidth) : 0;
					double halfWidth = door ? doorClearWidth * 0.5 :
						plannedClearWidth * 0.5;
				if (doglegRequested) halfWidth = std::max(64.0, halfWidth);
				// A cell can be inset on one unrelated face to make room for a
				// staircase. Clamp this portal against the actual directional
				// extents on both cells, otherwise a wide opening can overrun a
				// chamfer and leave a four-unit BSP sliver at the corner.
				double apertureA = direction == DIR_E ?
					std::min(EdgeForCell(x, y, DIR_N), EdgeForCell(x, y, DIR_S)) :
					std::min(EdgeForCell(x, y, DIR_W), EdgeForCell(x, y, DIR_E));
				double apertureB = direction == DIR_E ?
					std::min(EdgeForCell(nx, ny, DIR_N), EdgeForCell(nx, ny, DIR_S)) :
					std::min(EdgeForCell(nx, ny, DIR_W), EdgeForCell(nx, ny, DIR_E));
				double apertureHalf = std::min(apertureA, apertureB);
				if (roomA == roomB && !door)
				{
					// Internal joins nearly consume the full shared face. A localized
					// 24/32-unit shoulder remains at each grid junction, avoiding the
					// zero-area four-cell pinwheel while making a composed room read as
					// one L/T/hall envelope instead of pods joined by waists.
					const double shoulder = Rooms[roomA].spatialClass >= 3 ? 12.0 : 16.0;
					double desiredHalf = std::min(168.0, apertureHalf - shoulder);
					auto SameRoomCell = [&](int sampleX, int sampleY) -> bool
					{
						return sampleX >= 0 && sampleX < W && sampleY >= 0 && sampleY < H &&
							Grid[sampleY][sampleX].present &&
							Grid[sampleY][sampleX].roomId == roomA;
					};
					bool denseJunction = false;
					if (direction == DIR_E)
					{
						for (int offset : { -1, 1 })
							denseJunction |= SameRoomCell(x, y + offset) &&
								SameRoomCell(nx, ny + offset);
					}
					else
					{
						for (int offset : { -1, 1 })
							denseJunction |= SameRoomCell(x + offset, y) &&
								SameRoomCell(nx + offset, ny);
					}
					// Four-cell junctions retain a proper support instead of letting
					// two open unions touch at one zero-area pinwheel vertex.
					if (denseJunction) desiredHalf = std::min(128.0, desiredHalf);
					halfWidth = std::max(48.0, desiredHalf);
				}
				else
				{
						double connectionCut = std::max(Rooms[roomA].cornerCut, Rooms[roomB].cornerCut);
						// Door jambs own straight protected approaches. Their aperture does
						// not donate a cosmetic corner cut, unlike an ordinary open hall.
						double availableHalf = door ? apertureHalf : apertureHalf - connectionCut;
						if (door && halfWidth > availableHalf + 0.001)
						{
							// An unlocked scenic/start staging door is optional pacing, not a
							// reason to reject an otherwise safe recipe. Its planned art slab
							// can be wider than a contour-constrained shoulder even though a
							// Standard open portal still fits. Drop that normal door before
							// touching any keyed, secret, or otherwise protected crossing.
							if (lockType <= 0 && !secretDoor)
							{
								door = false;
								availableHalf = apertureHalf;
								halfWidth = std::min(halfWidth, availableHalf);
							}
							else
							{
								// Keyed and secret crossings cannot be silently opened. They can,
								// however, give up an optional Gallery/Grand treatment and retain
								// their full Standard 128-unit physical slab. Only an aperture
								// narrower than that safe baseline invalidates the recipe.
								if (apertureHalf < 64.0 - 0.001)
								{
									LastError.Format("A protected procedural door has no 128-unit jamb aperture "
										"(lock=%d secret=%d requested=%.0f available=%.0f)",
										lockType, secretDoor ? 1 : 0, halfWidth * 2.0, availableHalf * 2.0);
									return false;
								}
								availableHalf = apertureHalf;
								halfWidth = 64.0;
							}
						}
						if (!door)
					{
						// Open corridors own a straight band through the chamber face.
						// Their endpoint chamfers are capped to that band when the shell is
						// emitted below, so a cosmetic corner never turns the 96-unit
						// Narrow contract into an 80/88-unit physical pinch. Fitted doors
						// retain the stricter jamb aperture above.
						if (!door) availableHalf = apertureHalf;
							halfWidth = std::min(halfWidth, availableHalf);
					}
						if (!sightline && roomA != roomB)
						{
							const double centerDistance = direction == DIR_E ?
								CellCenterX(nx) - CellCenterX(x) :
								CellCenterY(ny) - CellCenterY(y);
							const int realizedWidth = std::max((mandatoryConnector || door) ? 128 : 96,
								(int)floor(std::max(0.0, halfWidth * 2.0) / 8.0) * 8);
							const int realizedDepth = std::max((mandatoryConnector || door) ? 64 : 48,
								(int)floor(std::max(0.0, centerDistance -
									EdgeForCell(x, y, direction) -
									EdgeForCell(nx, ny, opposite)) / 8.0) * 8);
							EProcGenConnectionProfile realizedProfile = ProfileForContract(
								realizedWidth, realizedDepth, mandatoryConnector || door);
							if ((mandatoryConnector || door) && realizedProfile == PGCP_Narrow)
								realizedProfile = PGCP_Standard;
						const int contractWidth = ConnectionWidthForProfile(realizedProfile);
						if (contractWidth * 0.5 > availableHalf + 0.001)
						{
								if (mandatoryConnector || door)
							{
								// A decorative contour may consume a gallery shoulder after
								// the profile reservation has already made the route safe.
								// Preserve the mandatory walk by falling back to its guaranteed
								// 128-unit Standard span, then let the chamber's straight portal
								// edge take priority over its optional corner treatment.
								realizedProfile = PGCP_Standard;
								if (apertureHalf < 64.0 - 0.001)
								{
									LastError = "A required procedural connector has no 128-unit portal span";
									return false;
								}
								availableHalf = apertureHalf;
							}
							else realizedProfile = PGCP_Narrow;
						}
						halfWidth = std::min(availableHalf,
							ConnectionWidthForProfile(realizedProfile) * 0.5);
							RealizeConnectionContract(firstPlan, direction, secondPlan, opposite,
								ConnectionWidthForProfile(realizedProfile),
								ConnectionDepthForProfile(realizedProfile), mandatoryConnector || door);
					}
				}
				if (!sightline)
				{
					if (door && roomA != roomB)
					{
						RecordDoorPair(roomA, roomB);
					}
						firstPlan.connectionHasDoor[direction] = door;
						secondPlan.connectionHasDoor[opposite] = door;
						firstPlan.connectionSecretDoor[direction] = secretDoor;
						secondPlan.connectionSecretDoor[opposite] = secretDoor;
						firstPlan.connectionDoorArtWidth[direction] = door ? doorProfile.width : 0;
						secondPlan.connectionDoorArtWidth[opposite] = door ? doorProfile.width : 0;
						firstPlan.connectionDoorArtHeight[direction] = door ? doorProfile.height : 0;
						secondPlan.connectionDoorArtHeight[opposite] = door ? doorProfile.height : 0;
				}

				int connectionSector = -1;
				int doorSector = -1;
				int approachSectorA = -1;
				int approachSectorB = -1;
				int stairIndex = -1;
				if (roomA == roomB && !door)
				{
					connectionSector = Rooms[roomA].sectorIdx;
				}
				else
				{
					double floorZ = std::max(Rooms[roomA].floorZ, Rooms[roomB].floorZ);
					double openCeil = std::min(Rooms[roomA].ceilZ, Rooms[roomB].ceilZ);
					if (openCeil < floorZ + 72.0) openCeil = floorZ + 72.0;
					bool sky = !door && outdoorRooms[roomA] && outdoorRooms[roomB];
					const char* ceiling = sky ? "F_SKY1" :
						(infernalArchitecture ? "FLAT5_1" : "CEIL3_5");
					int light = clamp((Rooms[roomA].light + Rooms[roomB].light) / 2, 160, 208);
					if (sky) light = std::max(light, 192);
					if (sightline)
					{
						floorZ += 48.0;
						openCeil = std::min(Rooms[roomA].ceilZ,
							Rooms[roomB].ceilZ) - 24.0;
						connectionSector = AddSector(floorZ, openCeil,
							SafeTexture(Rooms[roomA].floorTex, "FLOOR4_8"),
							ceiling, std::min(224, light + 8));
					}
					else if (door)
					{
						const double availableClearance = std::min(
							Rooms[roomA].ceilZ - floorZ, Rooms[roomB].ceilZ - floorZ);
						const double doorHeight = std::min<double>(doorProfile.height, availableClearance);
						if (doorHeight < 64.0)
						{
							LastError = "A procedural door has insufficient lintel clearance";
							return false;
						}
						const double doorCeil = floorZ + doorHeight;
						approachSectorA = AddSector(floorZ, doorCeil,
							SafeTexture(Rooms[roomA].floorTex, "FLOOR4_8"),
							SafeTexture(Rooms[roomA].ceilTex, "CEIL3_5"), Rooms[roomA].light);
						approachSectorB = AddSector(floorZ, doorCeil,
							SafeTexture(Rooms[roomB].floorTex, "FLOOR4_8"),
							SafeTexture(Rooms[roomB].ceilTex, "CEIL3_5"), Rooms[roomB].light);
						doorSector = AddSector(floorZ, floorZ,
							SafeTexture(Rooms[roomA].floorTex, "FLOOR4_8"), ceiling, light);
						sectors[approachSectorA].lightColor = Rooms[roomA].lightColor;
						sectors[approachSectorA].fadeColor = Rooms[roomA].fadeColor;
						sectors[approachSectorB].lightColor = Rooms[roomB].lightColor;
						sectors[approachSectorB].fadeColor = Rooms[roomB].fadeColor;
						sectors[doorSector].lightColor = Rooms[roomA].lightColor;
						sectors[doorSector].fadeColor = Rooms[roomA].fadeColor;
					}
					else if (fabs(Rooms[roomA].floorZ - Rooms[roomB].floorZ) < 0.001)
					{
						connectionSector = AddSector(floorZ, openCeil,
							SafeTexture(Rooms[roomA].floorTex, "FLOOR4_8"), ceiling, light);
					}
					else
					{
						const int floorDifference = (int)lround(
							Rooms[roomB].floorZ - Rooms[roomA].floorZ);
						if ((floorDifference % 8) != 0 || abs(floorDifference) > 64)
						{
							LastError.Format("A procedural terrace between rooms %d and %d has an invalid %d-unit rise",
								roomA, roomB, floorDifference);
							return false;
						}
							StairConnection staircase;
							staircase.alignmentGroup = connectionAlignmentGroup;
						const int stepCount = abs(floorDifference) / 8;
						const int stepDirection = floorDifference > 0 ? 1 : -1;
						const double connectorLength = direction == DIR_E ?
							(CellCenterX(nx) - EdgeForCell(nx, ny, DIR_W)) -
							(CellCenterX(x) + EdgeForCell(x, y, DIR_E)) :
							(CellCenterY(ny) - EdgeForCell(nx, ny, DIR_N)) -
							(CellCenterY(y) + EdgeForCell(x, y, DIR_S));
						// The 64-unit landing and 32-unit outlet leave the remaining
						// connector for an eight-unit stair flight. Six treads (the
						// largest planned dogleg rise) still retain at least 16 units
						// each. Any tighter portal becomes the deterministic straight
						// stair fallback rather than a cramped pseudo-dogleg.
						const bool doglegFits = doglegRequested && !door &&
							halfWidth >= 64.0 - 0.001 &&
							connectorLength >= 96.0 + stepCount * 16.0 - 0.001;
						if (doglegRequested && !doglegFits)
							RecordDoglegFallback(doglegAnchor);
						staircase.dogleg = doglegFits;
						staircase.flightSteps = doglegFits ? stepCount : 0;
						if (doglegFits)
						{
							const uint32_t sideHash = blueprintHash ^
								(uint32_t)roomA * 0x9e3779b9u ^
								(uint32_t)roomB * 0x85ebca6bu ^
								(uint32_t)x * 0xc2b2ae35u ^
								(uint32_t)y * 0x27d4eb2du;
							staircase.side = (sideHash & 1u) != 0 ? 1 : -1;
						}
						for (int step = 0; step < stepCount; step++)
						{
							const double stepFloor = Rooms[roomA].floorZ +
								(step + 1) * 8.0 * stepDirection;
							const char* stepFlat = step * 2 < stepCount ?
								SafeTexture(Rooms[roomA].floorTex, "FLOOR4_8") :
								SafeTexture(Rooms[roomB].floorTex, "FLOOR4_8");
							staircase.sectors.Push(AddSector(stepFloor, openCeil,
								stepFlat, ceiling, light));
						}
						if (doglegFits)
						{
							// The high landing turns ninety degrees to the offset outlet.
							// They intentionally share the destination floor: the actual
							// elevation change remains entirely in valid eight-unit treads.
							const char* destinationFlat = SafeTexture(
								Rooms[roomB].floorTex, "FLOOR4_8");
							staircase.landingIndex = staircase.sectors.Size();
							staircase.sectors.Push(AddSector(Rooms[roomB].floorZ, openCeil,
								destinationFlat, ceiling, light));
							staircase.outletIndex = staircase.sectors.Size();
							staircase.sectors.Push(AddSector(Rooms[roomB].floorZ, openCeil,
								destinationFlat, ceiling, light));
						}
						stairConnections.Push(std::move(staircase));
						stairIndex = stairConnections.Size() - 1;
						connectionSector = stairConnections[stairIndex].sectors[0];
					}
				}

				ConnectionRef refA;
				refA.sector = door ? approachSectorA : connectionSector;
				refA.doorSector = doorSector;
				refA.stairIndex = stairIndex;
				refA.halfWidth = halfWidth;
				refA.door = door;
				refA.window = sightline;
				refA.secret = secretDoor;
					refA.lockType = lockType;
					refA.alignmentGroup = connectionAlignmentGroup;
				if (stairIndex >= 0 && stairConnections[stairIndex].dogleg)
				{
					refA.doglegPortal = true;
					refA.doglegPortalOffset = stairConnections[stairIndex].side * 32.0;
					refA.doglegPortalHalfWidth = 32.0;
				}
				if (door)
				{
					refA.doorHeight = sectors[approachSectorA].ceilZ - sectors[approachSectorA].floorZ;
					refA.doorTexture = doorProfile.texture ? doorProfile.texture : "";
					refA.doorTextureWidth = doorProfile.width;
					refA.doorTextureHeight = doorProfile.height;
				}
				ConnectionRef refB = refA;
				refB.sector = door ? approachSectorB :
					(stairIndex >= 0 ? stairConnections[stairIndex].sectors.Last() : connectionSector);
				if (refB.doglegPortal)
					refB.doglegPortalOffset = -refA.doglegPortalOffset;
				connectionGrid[y][x].refs[direction] = refA;
				connectionGrid[ny][nx].refs[OPP[direction]] = refB;
			}
		}
	}

	// Manifest realization describes emitted geometry, not only the recipe. A
	// connector too tight for a 64-unit outlet remains a safe stair hall while
	// preserving its required rise; the record makes that deterministic fallback
	// inspectable instead of advertising a dogleg that does not exist.
	for (unsigned int fallback = 0; fallback < doglegFallbackAnchors.Size(); ++fallback)
	{
		const int roomId = doglegFallbackAnchors[fallback];
		if (!IsValidRoom(roomId)) continue;
		RoomInfo& room = Rooms[roomId];
		if (room.verticalIntent != PGVI_DoglegAscent &&
			room.verticalIntent != PGVI_DoglegDescent)
			continue;
		room.verticalIntent = PGVI_StairHall;
		if (room.lockStage >= 0 && room.lockStage < RunBlueprint::MaxStages)
			Blueprint.RealizedStageVerticalIntents[room.lockStage] = PGVI_StairHall;
		for (int cellY = 0; cellY < H; ++cellY)
		{
			for (int cellX = 0; cellX < W; ++cellX)
			{
				ProcGenCell& cell = Grid[cellY][cellX];
				if (cell.roomId == roomId && cell.verticalAnchor)
					cell.verticalIntent = PGVI_StairHall;
			}
		}
	}

	auto DoorTrackTexture = [&](int lockType) -> const char*
	{
		if (lockType == 1) return "DOORRED";
		if (lockType == 2) return "DOORBLU";
		if (lockType == 3) return "DOORYEL";
		return "DOORTRAK";
	};

	auto AddDoorFace = [&](double x1, double y1, double x2, double y2,
		int approachSector, int doorSector, const ConnectionRef& ref,
		const char* roomWall)
	{
		// The reservation follows the actual clear slab span. A 64-wide stock
		// texture may be fitted here, but it must not make the approach proof see
		// only half of a protected 128+/gallery doorway.
		ReserveNavigationSegment(x1, y1, x2, y2, ref.halfWidth);
		const char* doorTexture = ref.secret ? roomWall :
			SafeTexture(ref.doorTexture, "BIGDOOR1");
		// The upper texture is deliberately pegged to the moving door ceiling;
		// unlike the tracks, the door face must rise with the sector.
		int lineIndex = AddLine(x1, y1, x2, y2, approachSector, doorSector,
			doorTexture, nullptr, roomWall,
			doorTexture, nullptr, roomWall,
			false, 12, ref.lockType, 0, 16, 150, 0, 0,
			true, false, true, false, false);
		if (lineIndex >= 0)
		{
			lines[lineIndex].secret = ref.secret;
			// Door faces remain intentional fitted artwork. Their final crop and
			// vertical scale are calculated from the active texture's logical size
			// in the alignment pass, after all sector geometry is complete.
			sides[lines[lineIndex].sideFront].topAlignment = TAM_DoorFit;
			sides[lines[lineIndex].sideBack].topAlignment = TAM_DoorFit;
		}
	};

	auto AddPortal = [&](double x1, double y1, double x2, double y2,
		int roomSector, const ConnectionRef& ref, const char* roomWall)
	{
		if (ref.sector < 0 || ref.sector == roomSector) return;
		if (ref.doglegPortal)
		{
			// A planned dogleg owns only one 64-unit lane at each chamber. The
			// opposite lane is a real solid shoulder, so players cannot ignore the
			// landing turn by cutting straight across the connector mouth.
			const double dx = x2 - x1;
			const double dy = y2 - y1;
			const double length = hypot(dx, dy);
			if (length < 64.0 - 0.001) return;
			double centerT = 0.5;
			if (fabs(dx) >= fabs(dy) && fabs(dx) > 0.001)
				centerT = ((x1 + x2) * 0.5 + ref.doglegPortalOffset - x1) / dx;
			else if (fabs(dy) > 0.001)
				centerT = ((y1 + y2) * 0.5 + ref.doglegPortalOffset - y1) / dy;
			const double halfT = ref.doglegPortalHalfWidth / length;
			const double firstT = clamp(centerT - halfT, 0.0, 1.0);
			const double secondT = clamp(centerT + halfT, 0.0, 1.0);
			const double portalX1 = x1 + dx * firstT;
			const double portalY1 = y1 + dy * firstT;
			const double portalX2 = x1 + dx * secondT;
			const double portalY2 = y1 + dy * secondT;
			AddWall(x1, y1, portalX1, portalY1, roomSector, roomWall);
			ReserveNavigationSegment(portalX1, portalY1, portalX2, portalY2, 40.0);
			AddLine(portalX1, portalY1, portalX2, portalY2, roomSector, ref.sector,
				roomWall, nullptr, roomWall,
				roomWall, nullptr, roomWall,
				false, 0, 0, 0, 0, 0, 0, 0,
				false, false, false, true, true, false);
			AddWall(portalX2, portalY2, x2, y2, roomSector, roomWall);
			return;
		}
		if (!ref.window)
			ReserveNavigationSegment(x1, y1, x2, y2,
				ref.door ? ref.halfWidth : 56.0);
		// Door connections use a separate lowered approach sector. This portal
		// authors a real lintel between the tall room and the stock-height jamb,
		// containing the moving face instead of letting its texture share the
		// neighboring room wall's vertical span.
		AddLine(x1, y1, x2, y2, roomSector, ref.sector,
			roomWall, nullptr, roomWall,
			roomWall, nullptr, roomWall,
			false, 0, 0, 0, 0, 0, 0, 0,
			false, false, false, true, true, ref.window);
	};

	// Emit an actual dogleg rather than merely tagging a conventional stair. The
	// first flight owns every eight-unit rise; a high, perpendicular landing then
	// feeds a short offset outlet. The chamber mouths are narrowed by AddPortal,
	// making the turn mandatory instead of decorative. The local u axis follows
	// the grid connector and v is transverse; reverse the local winding for a
	// north/south connector so every one-sided wall still faces inward.
	auto EmitDoglegStair = [&](bool eastbound, double primaryStart,
		double primaryEnd, double crossCenter, const StairConnection& staircase,
		const char* corridorWall, const char* stepWall)
	{
		const int stepCount = staircase.flightSteps;
		if (!staircase.dogleg || stepCount <= 0 ||
			staircase.landingIndex != stepCount ||
			staircase.outletIndex != stepCount + 1 ||
			staircase.outletIndex >= (int)staircase.sectors.Size())
			return false;
		const double length = primaryEnd - primaryStart;
		const double flightEnd = length - 96.0;
		const double outletStart = length - 32.0;
		if (flightEnd < stepCount * 16.0 - 0.001 || outletStart <= flightEnd)
			return false;
		const double sourceCenter = staircase.side * 32.0;
		const double targetCenter = -sourceCenter;
		const double sourceLow = sourceCenter - 32.0;
		const double sourceHigh = sourceCenter + 32.0;
		const double targetLow = targetCenter - 32.0;
		const double targetHigh = targetCenter + 32.0;
		auto LocalPoint = [&](double u, double v, double& x, double& y)
		{
			if (eastbound)
			{
				x = primaryStart + u;
				y = crossCenter + v;
			}
			else
			{
				x = crossCenter + v;
				y = primaryStart + u;
			}
		};
		auto AddDoglegWall = [&](double u1, double v1, double u2, double v2,
			int sector, const char* texture)
		{
			double x1, y1, x2, y2;
			LocalPoint(u1, v1, x1, y1);
			LocalPoint(u2, v2, x2, y2);
			if (!eastbound)
			{
				std::swap(x1, x2);
				std::swap(y1, y2);
			}
			AddWall(x1, y1, x2, y2, sector, texture,
				staircase.alignmentGroup);
		};
		auto ReserveDoglegLane = [&](double u1, double v1, double u2, double v2)
		{
			double x1, y1, x2, y2;
			LocalPoint(u1, v1, x1, y1);
			LocalPoint(u2, v2, x2, y2);
			ReserveNavigationSegment(x1, y1, x2, y2, 32.0);
		};

		for (int step = 0; step < stepCount; ++step)
		{
			const double first = flightEnd * step / stepCount;
			const double second = flightEnd * (step + 1) / stepCount;
			const int sector = staircase.sectors[step];
			// Clockwise rectangle, omitting only the source mouth. Shared tread
			// edges are authored twice with opposite winding so AddWall upgrades
			// them to conventional two-sided stair risers.
			AddDoglegWall(first, sourceHigh, second, sourceHigh,
				sector, corridorWall);
			AddDoglegWall(second, sourceLow, first, sourceLow,
				sector, corridorWall);
			if (step > 0)
				AddDoglegWall(first, sourceLow, first, sourceHigh,
					sector, stepWall);
			AddDoglegWall(second, sourceHigh, second, sourceLow,
				sector, stepWall);
		}

		const int landingSector = staircase.sectors[staircase.landingIndex];
		const int outletSector = staircase.sectors[staircase.outletIndex];
		// The broad landing is split at v=0 wherever it meets a flight or the
		// outlet. That prevents partial overlapping linedefs and leaves a closed
		// shoulder over the lane which must not connect to the neighboring room.
		AddDoglegWall(flightEnd, 64.0, outletStart, 64.0,
			landingSector, corridorWall);
		if (staircase.side > 0)
		{
			AddDoglegWall(outletStart, 64.0, outletStart, 0.0,
				landingSector, corridorWall);
			AddDoglegWall(outletStart, 0.0, outletStart, -64.0,
				landingSector, stepWall);
		}
		else
		{
			AddDoglegWall(outletStart, 64.0, outletStart, 0.0,
				landingSector, stepWall);
			AddDoglegWall(outletStart, 0.0, outletStart, -64.0,
				landingSector, corridorWall);
		}
		AddDoglegWall(outletStart, -64.0, flightEnd, -64.0,
			landingSector, corridorWall);
		if (staircase.side > 0)
		{
			AddDoglegWall(flightEnd, -64.0, flightEnd, 0.0,
				landingSector, corridorWall);
			AddDoglegWall(flightEnd, 0.0, flightEnd, 64.0,
				landingSector, stepWall);
		}
		else
		{
			AddDoglegWall(flightEnd, -64.0, flightEnd, 0.0,
				landingSector, stepWall);
			AddDoglegWall(flightEnd, 0.0, flightEnd, 64.0,
				landingSector, corridorWall);
		}

		// The outlet reaches the destination chamber only through its offset
		// lane. Its far edge is the matching two-sided portal emitted above.
		AddDoglegWall(outletStart, targetHigh, length, targetHigh,
			outletSector, corridorWall);
		AddDoglegWall(length, targetLow, outletStart, targetLow,
			outletSector, corridorWall);
		AddDoglegWall(outletStart, targetLow, outletStart, targetHigh,
			outletSector, stepWall);

		ReserveDoglegLane(0.0, sourceCenter, flightEnd, sourceCenter);
		const double landingU = (flightEnd + outletStart) * 0.5;
		ReserveDoglegLane(landingU, sourceCenter, landingU, targetCenter);
		ReserveDoglegLane(outletStart, targetCenter, length, targetCenter);
		return true;
	};

	TArray<bool> switchWallEmitted;
	TArray<bool> footprintContourEmitted;
	switchWallEmitted.Resize(Rooms.Size());
	footprintContourEmitted.Resize(Rooms.Size());
	TArray<double> contourMinX, contourMaxX, contourMinY, contourMaxY;
	contourMinX.Resize(Rooms.Size());
	contourMaxX.Resize(Rooms.Size());
	contourMinY.Resize(Rooms.Size());
	contourMaxY.Resize(Rooms.Size());
	for (unsigned int ri = 0; ri < Rooms.Size(); ri++)
	{
		switchWallEmitted[ri] = false;
		footprintContourEmitted[ri] = false;
		contourMinX[ri] = contourMinY[ri] = DBL_MAX;
		contourMaxX[ri] = contourMaxY[ri] = -DBL_MAX;
		Rooms[ri].contourVertices = 0;
		Rooms[ri].contourEdges = 0;
		Rooms[ri].contourArea = 0.0;
		Rooms[ri].contourWidth = 0.0;
		Rooms[ri].contourHeight = 0.0;
		Rooms[ri].realizedFootprint = PGRF_SafeShell;
		Rooms[ri].contourUnified = false;
		Rooms[ri].contourLoops = 0;
		Rooms[ri].emittedContourSector = -1;
		Rooms[ri].emittedContourMinX = Rooms[ri].emittedContourMaxX = 0.0;
		Rooms[ri].emittedContourMinY = Rooms[ri].emittedContourMaxY = 0.0;
	}
	auto AddChamberWall = [&](int roomId, int cellX, int cellY, int direction,
		double x1, double y1, double x2, double y2, int sector,
		const char* texture, bool switchEligible)
	{
		const double length = hypot(x2 - x1, y2 - y1);
		if (revealArchitectures[roomId] == RevealFalseWall &&
			revealCellX[roomId] == cellX && revealCellY[roomId] == cellY &&
			revealDoorSides[roomId] == WallSideForGridDirection[direction] &&
			revealWallLineIndices[roomId] < 0 && length >= 144.0)
		{
			const double unitX = (x2 - x1) / length;
			const double unitY = (y2 - y1) / length;
			const double centerX = (x1 + x2) * 0.5;
			const double centerY = (y1 + y2) * 0.5;
			const double doorHalf = 48.0;
			const double doorX1 = centerX - unitX * doorHalf;
			const double doorY1 = centerY - unitY * doorHalf;
			const double doorX2 = centerX + unitX * doorHalf;
			const double doorY2 = centerY + unitY * doorHalf;
			AddWall(x1, y1, doorX1, doorY1, sector, texture);
			revealWallLineIndices[roomId] = AddWall(doorX1, doorY1,
				doorX2, doorY2, sector, texture);
			AddWall(doorX2, doorY2, x2, y2, sector, texture);
			return;
		}
		if (switchEligible && switchTargetTags[roomId] > 0 &&
			!switchWallEmitted[roomId] && length >= 96.0)
		{
			const double unitX = (x2 - x1) / length;
			const double unitY = (y2 - y1) / length;
			const double centerX = (x1 + x2) * 0.5;
			const double centerY = (y1 + y2) * 0.5;
			const double panelX1 = centerX - unitX * 32.0;
			const double panelY1 = centerY - unitY * 32.0;
			const double panelX2 = centerX + unitX * 32.0;
			const double panelY2 = centerY + unitY * 32.0;
			AddWall(x1, y1, panelX1, panelY1, sector, texture);
			if (AddSwitchWall(panelX1, panelY1, panelX2, panelY2,
				sector, switchTargetTags[roomId]) >= 0)
				switchWallEmitted[roomId] = true;
			AddWall(panelX2, panelY2, x2, y2, sector, texture);
			return;
		}
		AddWall(x1, y1, x2, y2, sector, texture);
	};

	// A footprint owns a real perimeter move, not merely a different octagon
	// label.  We only modify a face that opens onto an absent coarse cell: that
	// preserves every portal-bearing span, stair, key approach, and neighboring
	// room shell.  The compact perimeter moves below are all static one-sided
	// UDMF walls, so a requested apse/courtyard/wedge can safely fall back to the
	// normal chamber edge when the grid leaves no exterior face to shape.
	auto AddFootprintContour = [&](int roomId, int cellX, int cellY, int direction,
		double x1, double y1, double x2, double y2, int sector,
		const char* texture) -> bool
	{
		if (!IsValidRoom(roomId) || footprintContourEmitted[roomId]) return false;
		RoomInfo& room = Rooms[roomId];
		const EProcGenRoomFootprint footprint =
			(EProcGenRoomFootprint)room.footprint;
		if (footprint == PGRF_SafeShell || footprint == PGRF_AsymmetricOctagon)
			return false;
		const int nx = cellX + DX[direction];
		const int ny = cellY + DY[direction];
		if (nx >= 0 && nx < W && ny >= 0 && ny < H && Grid[ny][nx].present)
			return false;
		const double dx = x2 - x1;
		const double dy = y2 - y1;
		const double length = hypot(dx, dy);
		if (length < 128.0) return false;
		const double tx = dx / length;
		const double ty = dy / length;
		double ox = 0.0;
		double oy = 0.0;
		switch (direction)
		{
		case DIR_S: oy = 1.0; break;
		case DIR_N: oy = -1.0; break;
		case DIR_E: ox = 1.0; break;
		default: ox = -1.0; break;
		}
		const double span = std::min(80.0, length * 0.28);
		const double depth = clamp(24.0 + room.contourInset * 0.5 +
			(room.footprintVariant & 1 ? 8.0 : 0.0), 24.0, 48.0);
		TArray<BuildVertex> points;
		points.Push({ x1, y1 });
		auto Along = [&](double distance, double normal) -> BuildVertex
		{
			return { x1 + tx * distance + ox * normal,
				y1 + ty * distance + oy * normal };
		};
		double areaDelta = 0.0;
		switch (footprint)
		{
		case PGRF_Apse:
			points.Push(Along(length * 0.5 - span, 0.0));
			points.Push(Along(length * 0.5 - span * 0.72, depth * 0.62));
			points.Push(Along(length * 0.5, depth));
			points.Push(Along(length * 0.5 + span * 0.72, depth * 0.62));
			points.Push(Along(length * 0.5 + span, 0.0));
			areaDelta = depth * span * 1.55;
			break;
		case PGRF_CourtyardCut:
			points.Push(Along(length * 0.34, 0.0));
			points.Push(Along(length * 0.34, -depth));
			points.Push(Along(length * 0.66, -depth));
			points.Push(Along(length * 0.66, 0.0));
			areaDelta = -depth * length * 0.32;
			break;
		case PGRF_SteppedCompound:
			points.Push(Along(length * 0.27, 0.0));
			points.Push(Along(length * 0.27, depth));
			points.Push(Along(length * 0.73, depth));
			points.Push(Along(length * 0.73, 0.0));
			areaDelta = depth * length * 0.46;
			break;
		case PGRF_FracturedWedge:
			points.Push(Along(length * 0.29, 0.0));
			points.Push(Along(length * 0.56, depth));
			points.Push(Along(length * 0.77, depth * 0.28));
			areaDelta = depth * length * 0.24;
			break;
		case PGRF_TaperedBay:
			points.Push(Along(length * 0.18, depth * 0.25));
			points.Push(Along(length * 0.50, depth));
			points.Push(Along(length * 0.82, depth * 0.45));
			areaDelta = depth * length * 0.48;
			break;
		default:
			return false;
		}
		points.Push({ x2, y2 });
		for (unsigned int point = 0; point + 1 < points.Size(); ++point)
			AddWall(points[point].x, points[point].y,
				points[point + 1].x, points[point + 1].y, sector, texture);
		room.contourEdges += (int)points.Size() - 2;
		room.contourVertices += (int)points.Size() - 2;
		room.contourArea = std::max(0.0, room.contourArea + areaDelta);
		for (unsigned int point = 0; point < points.Size(); ++point)
		{
			contourMinX[roomId] = std::min(contourMinX[roomId], points[point].x);
			contourMaxX[roomId] = std::max(contourMaxX[roomId], points[point].x);
			contourMinY[roomId] = std::min(contourMinY[roomId], points[point].y);
			contourMaxY[roomId] = std::max(contourMaxY[roomId], points[point].y);
		}
		footprintContourEmitted[roomId] = true;
		return true;
	};

	// A conservative room-union path for full rectangular compositions. It owns
	// one exterior boundary in the room sector and deliberately leaves no
	// chamber-to-chamber shoulders or connector-side walls inside that envelope.
	// Portal spans still go through the existing emitter, so doors, stairs,
	// reservations, alignment groups, and manual key behavior remain identical
	// to the safe-shell path.
	struct EnvelopePortal
	{
		double first = 0.0;
		double last = 0.0;
		ConnectionRef ref;
	};
	auto EmitUnifiedEnvelopeSide = [&](int roomId, int direction,
		double x1, double y1, double x2, double y2, bool emit) -> bool
	{
		if (!IsValidRoom(roomId)) return false;
		const RoomInfo& room = Rooms[roomId];
		const double dx = x2 - x1;
		const double dy = y2 - y1;
		const double length = hypot(dx, dy);
		if (length < 1.0) return false;
		const double unitX = dx / length;
		const double unitY = dy / length;
		TArray<EnvelopePortal> portals;
		const bool horizontal = fabs(dx) >= fabs(dy);
		const int begin = horizontal ? room.minI : room.minJ;
		const int end = horizontal ? room.maxI : room.maxJ;
		for (int coordinate = begin; coordinate <= end; ++coordinate)
		{
			const int cellX = horizontal ? coordinate :
				(direction == DIR_E ? room.maxI : room.minI);
			const int cellY = horizontal ?
				(direction == DIR_S ? room.maxJ : room.minJ) : coordinate;
			const ConnectionRef& ref = connectionGrid[cellY][cellX].refs[direction];
			if (ref.sector < 0 || ref.sector == room.sectorIdx) continue;
			const double centerX = CellCenterX(cellX);
			const double centerY = CellCenterY(cellY);
			const double center = (centerX - x1) * unitX + (centerY - y1) * unitY;
			EnvelopePortal portal;
			portal.first = center - ref.halfWidth;
			portal.last = center + ref.halfWidth;
			portal.ref = ref;
			portals.Push(std::move(portal));
		}
		for (unsigned int index = 0; index < portals.Size(); ++index)
		{
			unsigned int best = index;
			for (unsigned int candidate = index + 1; candidate < portals.Size(); ++candidate)
				if (portals[candidate].first < portals[best].first) best = candidate;
			if (best != index)
			{
				const EnvelopePortal saved = portals[index];
				portals[index] = portals[best];
				portals[best] = saved;
			}
		}
		double cursor = 0.0;
		for (const EnvelopePortal& portal : portals)
		{
			if (portal.first < cursor - 0.01 || portal.first < -0.01 ||
				portal.last > length + 0.01 || portal.last - portal.first < 16.0)
				return false;
			cursor = portal.last;
		}
		if (!emit) return true;
		cursor = 0.0;
		auto PointAt = [&](double distance, double& x, double& y)
		{
			x = x1 + unitX * distance;
			y = y1 + unitY * distance;
		};
		for (const EnvelopePortal& portal : portals)
		{
			double segmentX1, segmentY1, segmentX2, segmentY2;
			if (portal.first > cursor + 0.01)
			{
				PointAt(cursor, segmentX1, segmentY1);
				PointAt(portal.first, segmentX2, segmentY2);
				AddWall(segmentX1, segmentY1, segmentX2, segmentY2,
					room.sectorIdx, SafeTexture(room.wallTex, "STARTAN3"));
			}
			PointAt(portal.first, segmentX1, segmentY1);
			PointAt(portal.last, segmentX2, segmentY2);
			AddPortal(segmentX1, segmentY1, segmentX2, segmentY2,
				room.sectorIdx, portal.ref, SafeTexture(room.wallTex, "STARTAN3"));
			cursor = portal.last;
		}
		if (cursor < length - 0.01)
		{
			double segmentX1, segmentY1, segmentX2, segmentY2;
			PointAt(cursor, segmentX1, segmentY1);
			PointAt(length, segmentX2, segmentY2);
			AddWall(segmentX1, segmentY1, segmentX2, segmentY2,
				room.sectorIdx, SafeTexture(room.wallTex, "STARTAN3"));
		}
		return true;
	};

	for (unsigned int ri = 0; ri < Rooms.Size(); ++ri)
	{
		if (!unifiedEnvelopeRooms[ri]) continue;
		RoomInfo& room = Rooms[ri];
		const double left = CellCenterX(room.minI) - EdgeForCell(room.minI, room.minJ, DIR_W);
		const double right = CellCenterX(room.maxI) + EdgeForCell(room.maxI, room.minJ, DIR_E);
		const double bottom = CellCenterY(room.minJ) - EdgeForCell(room.minI, room.minJ, DIR_N);
		const double top = CellCenterY(room.maxJ) + EdgeForCell(room.minI, room.maxJ, DIR_S);
		const bool fits = EmitUnifiedEnvelopeSide((int)ri, DIR_S, left, top, right, top, false) &&
			EmitUnifiedEnvelopeSide((int)ri, DIR_E, right, top, right, bottom, false) &&
			EmitUnifiedEnvelopeSide((int)ri, DIR_N, right, bottom, left, bottom, false) &&
			EmitUnifiedEnvelopeSide((int)ri, DIR_W, left, bottom, left, top, false);
		if (!fits)
		{
			unifiedEnvelopeRooms[ri] = false;
			continue;
		}
		EmitUnifiedEnvelopeSide((int)ri, DIR_S, left, top, right, top, true);
		EmitUnifiedEnvelopeSide((int)ri, DIR_E, right, top, right, bottom, true);
		EmitUnifiedEnvelopeSide((int)ri, DIR_N, right, bottom, left, bottom, true);
		EmitUnifiedEnvelopeSide((int)ri, DIR_W, left, bottom, left, top, true);
		room.contourArea = std::max(0.0, (right - left) * (top - bottom));
		room.contourVertices = 4;
		room.contourEdges = 4;
		room.contourWidth = right - left;
		room.contourHeight = top - bottom;
		room.contourUnified = true;
		room.contourLoops = 1;
		contourMinX[ri] = left;
		contourMaxX[ri] = right;
		contourMinY[ri] = bottom;
		contourMaxY[ri] = top;
		// The envelope is a truthful safe shell until a grammar-specific exterior
		// contour has actually been emitted. It is still materially different
		// from the old disconnected-cell realization.
		if (room.footprint != PGRF_SafeShell)
			room.footprintFallback = true;
	}

	// Emit one closed, chamfered chamber polygon per present coarse cell. The
	// 45-degree corners break up the coarse grid silhouette while every segment
	// remains part of a simple, clockwise boundary whose front side faces in.
	for (int y = 0; y < H; y++)
	{
		for (int x = 0; x < W; x++)
		{
			ProcGenCell& cell = Grid[y][x];
			if (!cell.present || !IsValidRoom(cell.roomId)) continue;
			if (unifiedEnvelopeRooms[cell.roomId]) continue;
			int roomSector = Rooms[cell.roomId].sectorIdx;
			const char* wall = SafeTexture(Rooms[cell.roomId].wallTex, "STARTAN3");
			RoomInfo& room = Rooms[cell.roomId];
			// Flat runs remain coherent, while their real chamfer seams can carry a
			// theme-specific support or damaged-detail material. Detail density controls
			// how often these architectural accents appear without creating fake splits.
			const int trimHash = abs(x * 17 + y * 29 + room.visualVariant * 7 + room.id * 3);
			const bool useTrim = Detail == 2 || (Detail == 1 && (trimHash % 3) != 0);
			const char* cornerWall = useTrim ?
				SafeTexture((Detail == 2 && (trimHash & 1)) ? room.detailTex : room.accentTex, wall) : wall;
			double cx = CellCenterX(x);
			double cy = CellCenterY(y);
			double leftHalf = EdgeForCell(x, y, DIR_W);
			double rightHalf = EdgeForCell(x, y, DIR_E);
			double bottomHalf = EdgeForCell(x, y, DIR_N);
			double topHalf = EdgeForCell(x, y, DIR_S);
			double left = cx - leftHalf;
			double right = cx + rightHalf;
			double bottom = cy - bottomHalf;
			double top = cy + topHalf;

			const ConnectionRef& topRef = connectionGrid[y][x].refs[DIR_S];
			const ConnectionRef& rightRef = connectionGrid[y][x].refs[DIR_E];
			const ConnectionRef& bottomRef = connectionGrid[y][x].refs[DIR_N];
			const ConnectionRef& leftRef = connectionGrid[y][x].refs[DIR_W];
			bool topFull = topRef.sector == roomSector &&
				topRef.halfWidth >= std::min(leftHalf, rightHalf) - 0.001;
			bool rightFull = rightRef.sector == roomSector &&
				rightRef.halfWidth >= std::min(bottomHalf, topHalf) - 0.001;
			bool bottomFull = bottomRef.sector == roomSector &&
				bottomRef.halfWidth >= std::min(leftHalf, rightHalf) - 0.001;
			bool leftFull = leftRef.sector == roomSector &&
				leftRef.halfWidth >= std::min(bottomHalf, topHalf) - 0.001;

			// A same-room connection consumes the whole coarse edge. Corners are
			// chamfered only where both adjacent edges belong to the true room
			// perimeter, so composed rooms read as one hall instead of pods joined
			// by repeated narrow waists.
			auto LocalCornerCut = [&](int corner, int axis) -> double
			{
				const int divisor = corner * 11 + axis * 5 + 1;
				const int sample = (trimHash / divisor + corner + axis * 2) % 4;
				return std::max(4.0, room.cornerCut - sample * 8.0);
			};
			// Horizontal and vertical clips differ deliberately. Stock Doom uses
			// many non-45-degree walls; asymmetric clips introduce that angular
			// vocabulary without compromising the simple clockwise cell boundary.
			double cutTRX = (topFull || rightFull) ? 0.0 : LocalCornerCut(0, 0);
			double cutTRY = (topFull || rightFull) ? 0.0 : LocalCornerCut(0, 1);
			double cutBRX = (rightFull || bottomFull) ? 0.0 : LocalCornerCut(1, 0);
			double cutBRY = (rightFull || bottomFull) ? 0.0 : LocalCornerCut(1, 1);
			double cutBLX = (bottomFull || leftFull) ? 0.0 : LocalCornerCut(2, 0);
			double cutBLY = (bottomFull || leftFull) ? 0.0 : LocalCornerCut(2, 1);
			double cutTLX = (leftFull || topFull) ? 0.0 : LocalCornerCut(3, 0);
			double cutTLY = (leftFull || topFull) ? 0.0 : LocalCornerCut(3, 1);
			// A portal is a circulation reservation, not a request to shave a
			// chamber's decorative corners until it fits. Cap only the two local
			// chamfers that touch its face so the emitted shell keeps a straight,
			// fully clear span equal to the connector reference. This is especially
			// important for optional Narrow links: their 96-unit contract may need
			// four more units than an otherwise attractive diagonal trim would
			// leave at each end.
			auto ReserveHorizontalPortal = [&](const ConnectionRef& ref,
				double& leftCut, double& rightCut)
			{
				if (ref.sector < 0) return;
				leftCut = std::min(leftCut, std::max(0.0, leftHalf - ref.halfWidth));
				rightCut = std::min(rightCut, std::max(0.0, rightHalf - ref.halfWidth));
			};
			auto ReserveVerticalPortal = [&](const ConnectionRef& ref,
				double& bottomCut, double& topCut)
			{
				if (ref.sector < 0) return;
				bottomCut = std::min(bottomCut, std::max(0.0, bottomHalf - ref.halfWidth));
				topCut = std::min(topCut, std::max(0.0, topHalf - ref.halfWidth));
			};
			ReserveHorizontalPortal(topRef, cutTLX, cutTRX);
			ReserveHorizontalPortal(bottomRef, cutBLX, cutBRX);
			ReserveVerticalPortal(rightRef, cutBRY, cutTRY);
			ReserveVerticalPortal(leftRef, cutBLY, cutTLY);
			double topLeft = left + cutTLX;
			double topRight = right - cutTRX;
			double rightTop = top - cutTRY;
			double rightBottom = bottom + cutBRY;
			double bottomRight = right - cutBRX;
			double bottomLeft = left + cutBLX;
			double leftBottom = bottom + cutBLY;
			double leftTop = top - cutTLY;
			auto AddCornerWall = [&](double x1, double y1, double x2, double y2)
			{
				// Asymmetric cuts may collapse one axis of a corner. In that
				// case this is a continuation of the adjacent flat wall, not a
				// visible trim break; keep its material continuous.
				const bool diagonal = fabs(x2 - x1) > 0.01 && fabs(y2 - y1) > 0.01;
				FString texture = diagonal ? cornerWall : wall;
				// A footprint contour may end on a diagonal that happens to be
				// collinear with this corner segment. The trim belongs at a turn,
				// not in the middle of that continuous contour run. The immediately
				// preceding line is the edge that arrived at this corner, so this
				// check remains local rather than turning the shell pass quadratic.
				if (diagonal && lines.Size() > 0)
				{
					const BuildLine& previous = lines[lines.Size() - 1];
					if (previous.sideBack < 0 && previous.sideFront >= 0 &&
						previous.special == 0 &&
						sides[previous.sideFront].sector == roomSector)
					{
						const BuildVertex& previousFirst = vertices[previous.v1];
						const BuildVertex& previousSecond = vertices[previous.v2];
						const bool firstAtCorner = fabs(previousFirst.x - x1) <= 0.01 &&
							fabs(previousFirst.y - y1) <= 0.01;
						const bool secondAtCorner = fabs(previousSecond.x - x1) <= 0.01 &&
							fabs(previousSecond.y - y1) <= 0.01;
						if (firstAtCorner || secondAtCorner)
						{
							const BuildVertex& other = firstAtCorner ? previousSecond : previousFirst;
							const double previousX = other.x - x1;
							const double previousY = other.y - y1;
							const double cornerX = x2 - x1;
							const double cornerY = y2 - y1;
							if (fabs(previousX * cornerY - previousY * cornerX) <= 0.01 &&
								previousX * cornerX + previousY * cornerY < 0.0 &&
								sides[previous.sideFront].middle.Compare("-") != 0)
								texture = sides[previous.sideFront].middle;
						}
					}
				}
				AddWall(x1, y1, x2, y2, roomSector, texture.GetChars());
			};

			// Record the room-owned contour as it is actually emitted.  Coarse cells
			// never overlap, so summing their clipped shell areas remains a useful
			// deterministic realization metric even for compound L/courtyard rooms.
			const int contourRoom = cell.roomId;
			const double clippedArea = (right - left) * (top - bottom) - 0.5 *
				(cutTRX * cutTRY + cutBRX * cutBRY + cutBLX * cutBLY + cutTLX * cutTLY);
			room.contourArea += std::max(0.0, clippedArea);
			room.contourVertices += 8;
			room.contourEdges += 8;
			contourMinX[contourRoom] = std::min(contourMinX[contourRoom], left);
			contourMaxX[contourRoom] = std::max(contourMaxX[contourRoom], right);
			contourMinY[contourRoom] = std::min(contourMinY[contourRoom], bottom);
			contourMaxY[contourRoom] = std::max(contourMaxY[contourRoom], top);

			// North/world-top edge: left -> right (grid DIR_S).
			if (topRef.sector >= 0)
			{
				AddChamberWall(cell.roomId, x, y, DIR_S,
					topLeft, top, cx - topRef.halfWidth, top,
					roomSector, wall, true);
				AddPortal(cx - topRef.halfWidth, top, cx + topRef.halfWidth, top,
					roomSector, topRef, wall);
				AddChamberWall(cell.roomId, x, y, DIR_S,
					cx + topRef.halfWidth, top, topRight, top,
					roomSector, wall, true);
			}
			else if (!AddFootprintContour(cell.roomId, x, y, DIR_S,
				topLeft, top, topRight, top, roomSector, wall))
				AddChamberWall(cell.roomId, x, y, DIR_S,
					topLeft, top, topRight, top, roomSector, wall, true);
			AddCornerWall(topRight, top, right, rightTop);

			// East edge: top -> bottom.
			if (rightRef.sector >= 0)
			{
				AddChamberWall(cell.roomId, x, y, DIR_E,
					right, rightTop, right, cy + rightRef.halfWidth,
					roomSector, wall, true);
				AddPortal(right, cy + rightRef.halfWidth, right, cy - rightRef.halfWidth,
					roomSector, rightRef, wall);
				AddChamberWall(cell.roomId, x, y, DIR_E,
					right, cy - rightRef.halfWidth, right, rightBottom,
					roomSector, wall, true);
			}
			else if (!AddFootprintContour(cell.roomId, x, y, DIR_E,
				right, rightTop, right, rightBottom, roomSector, wall))
				AddChamberWall(cell.roomId, x, y, DIR_E,
					right, rightTop, right, rightBottom,
					roomSector, wall, true);
			AddCornerWall(right, rightBottom, bottomRight, bottom);

			// South/world-bottom edge: right -> left (grid DIR_N).
			if (bottomRef.sector >= 0)
			{
				AddChamberWall(cell.roomId, x, y, DIR_N,
					bottomRight, bottom, cx + bottomRef.halfWidth,
					bottom, roomSector, wall, true);
				AddPortal(cx + bottomRef.halfWidth, bottom, cx - bottomRef.halfWidth, bottom,
					roomSector, bottomRef, wall);
				AddChamberWall(cell.roomId, x, y, DIR_N,
					cx - bottomRef.halfWidth, bottom, bottomLeft,
					bottom, roomSector, wall, true);
			}
			else if (!AddFootprintContour(cell.roomId, x, y, DIR_N,
				bottomRight, bottom, bottomLeft, bottom, roomSector, wall))
				AddChamberWall(cell.roomId, x, y, DIR_N,
					bottomRight, bottom, bottomLeft, bottom,
					roomSector, wall, true);
			AddCornerWall(bottomLeft, bottom, left, leftBottom);

			// West edge: bottom -> top.
			if (leftRef.sector >= 0)
			{
				AddChamberWall(cell.roomId, x, y, DIR_W,
					left, leftBottom, left, cy - leftRef.halfWidth,
					roomSector, wall, true);
				AddPortal(left, cy - leftRef.halfWidth, left, cy + leftRef.halfWidth,
					roomSector, leftRef, wall);
				AddChamberWall(cell.roomId, x, y, DIR_W,
					left, cy + leftRef.halfWidth, left, leftTop,
					roomSector, wall, true);
			}
			else if (!AddFootprintContour(cell.roomId, x, y, DIR_W,
				left, leftBottom, left, leftTop, roomSector, wall))
				AddChamberWall(cell.roomId, x, y, DIR_W,
					left, leftBottom, left, leftTop,
					roomSector, wall, true);
			AddCornerWall(left, leftTop, topLeft, top);
		}
	}
	for (unsigned int ri = 0; ri < Rooms.Size(); ++ri)
	{
		RoomInfo& room = Rooms[ri];
		if (!room.contourUnified)
			room.contourLoops = std::max(1, room.cellCount);
		// The clipped chamber shell is a safe baseline, not a realization of a
		// requested apse/courtyard/wedge. Keep requested and realized labels
		// separate so coverage cannot mistake a fallback for authored geometry.
		if (!room.contourUnified && room.footprint != PGRF_SafeShell &&
			room.footprint != PGRF_AsymmetricOctagon &&
			!footprintContourEmitted[ri])
			room.footprintFallback = true;
		if (contourMinX[ri] > contourMaxX[ri] || contourMinY[ri] > contourMaxY[ri])
		{
			room.footprintFallback = true;
			room.realizedFootprint = PGRF_SafeShell;
			continue;
		}
		room.contourWidth = contourMaxX[ri] - contourMinX[ri];
		room.contourHeight = contourMaxY[ri] - contourMinY[ri];
		room.emittedContourSector = room.sectorIdx;
		room.emittedContourMinX = contourMinX[ri];
		room.emittedContourMaxX = contourMaxX[ri];
		room.emittedContourMinY = contourMinY[ri];
		room.emittedContourMaxY = contourMaxY[ri];
		if (room.contourVertices < 3 || room.contourArea <= 0.0 ||
			room.contourWidth < 1.0 || room.contourHeight < 1.0)
			room.footprintFallback = true;
		if (room.footprintFallback)
			room.realizedFootprint = PGRF_SafeShell;
		else if (room.contourUnified || footprintContourEmitted[ri] ||
			(room.cellCount == 1 && room.footprint == PGRF_AsymmetricOctagon))
			room.realizedFootprint = room.footprint;
		else
			room.realizedFootprint = PGRF_SafeShell;
	}
	for (unsigned int ri = 0; ri < Rooms.Size(); ri++)
	{
		if (revealArchitectures[ri] == RevealFalseWall &&
			revealKinds[ri] != RevealNone && revealWallLineIndices[ri] < 0)
		{
			// This was an optional cache candidate. Retain the ordinary room shell
			// and discard the unreachable flourish, including any switch source
			// that would otherwise remain as a useless manual interaction.
			const int missingTag = revealTags[ri];
			revealKinds[ri] = RevealNone;
			revealTags[ri] = 0;
			DisableSerializedSwitchesForTag(missingTag);
			for (unsigned int source = 0; source < Rooms.Size(); ++source)
			{
				if (switchTargetTags[source] != missingTag) continue;
				switchTargetTags[source] = 0;
				Rooms[source].manualInteraction = Rooms[source].isLocked ?
					PGMI_KeyedDoor : (Rooms[source].isSecret ? PGMI_SecretDoor : PGMI_None);
			}
		}
		if (switchTargetTags[ri] > 0 && !switchWallEmitted[ri])
		{
			const int missingTag = switchTargetTags[ri];
			switchTargetTags[ri] = 0;
			DisableSerializedSwitchesForTag(missingTag);
			Rooms[ri].manualInteraction = Rooms[ri].isLocked ? PGMI_KeyedDoor :
				(Rooms[ri].isSecret ? PGMI_SecretDoor : PGMI_None);
			for (unsigned int host = 0; host < Rooms.Size(); ++host)
			{
				if (revealTags[host] != missingTag) continue;
				revealKinds[host] = RevealNone;
				revealTags[host] = 0;
			}
		}
	}

	// Corridor side walls complete the union between chamber openings. End
	// portals were emitted above and share deduplicated vertices with these.
	const char* corridorWall = themeStyle == ThemeIndustrial ? "SUPPORT3" :
		(themeStyle == ThemeGothic ? "WOOD1" :
			(infernalArchitecture ? "GSTVINE1" : "SUPPORT2"));
	for (int y = 0; y < H; y++)
	{
		for (int x = 0; x < W; x++)
		{
			if (!Grid[y][x].present) continue;

			const ConnectionRef& east = connectionGrid[y][x].refs[DIR_E];
			const bool eastInsideUnifiedEnvelope = x + 1 < W &&
				Grid[y][x + 1].present && Grid[y][x + 1].roomId == Grid[y][x].roomId &&
				IsValidRoom(Grid[y][x].roomId) &&
				unifiedEnvelopeRooms[Grid[y][x].roomId] &&
				Rooms[Grid[y][x].roomId].contourUnified;
			if (!eastInsideUnifiedEnvelope && east.sector >= 0 && x + 1 < W && Grid[y][x + 1].present)
			{
				double x1 = CellCenterX(x) + EdgeForCell(x, y, DIR_E);
				double x2 = CellCenterX(x + 1) - EdgeForCell(x + 1, y, DIR_W);
				double cy = CellCenterY(y);
				// Keep connector returns inside their owning coarse-cell half. A
				// maximum-width opening already reaches within eight units of the
				// cell midpoint; extending its reveal all the way to that midpoint
				// makes perpendicular connectors emit coincident solid lines at
				// four-way junctions. Those overlaps produce malformed GL-node holes
				// on large maps even though the UDMF itself still parses successfully.
				const double revealDepth = clamp(CELL_HALF - 8.0 - east.halfWidth,
					0.0, 8.0);
				const int collisionReservationFirst = navigationReservations.Size();
				if (east.door && east.doorSector >= 0)
				{
					int roomA = Grid[y][x].roomId;
					int roomB = Grid[y][x + 1].roomId;
					const ConnectionRef& west = connectionGrid[y][x + 1].refs[DIR_W];
					const int approachA = east.sector;
					const int approachB = west.sector;
					double mid = (x1 + x2) * 0.5;
					double doorLeft = mid - 8.0;
					double doorRight = mid + 8.0;
					double portalTop = cy + east.halfWidth;
					double portalBottom = cy - east.halfWidth;
					// The slab owns the complete protected portal width. Its native door
					// texture is fitted independently in the final alignment pass, so a
					// 64-wide DOOR/SPCDOOR motif never narrows a 128+/gallery entrance.
					double top = portalTop;
					double bottom = portalBottom;
					const char* track = east.secret ? corridorWall : DoorTrackTexture(east.lockType);
					const char* jamb = east.lockType > 0 ? track : corridorWall;
					const char* roomWallA = SafeTexture(Rooms[roomA].wallTex, "STARTAN3");
					const char* roomWallB = SafeTexture(Rooms[roomB].wallTex, "STARTAN3");

					// Step the connector walls outward behind 8-unit returns. The depth
					// break gives support/jamb materials a physical seam instead of
					// changing texture midway through a continuous chamber wall.
						AddWall(x1, portalTop, x1, top, approachA, roomWallA,
							east.alignmentGroup);
						AddWall(x1, top, doorLeft, top, approachA, jamb,
							east.alignmentGroup);
						AddWall(doorLeft, top, doorRight, top, east.doorSector, track,
							east.alignmentGroup);
						AddWall(doorRight, top, x2, top, approachB, jamb,
							east.alignmentGroup);
						AddWall(x2, top, x2, portalTop, approachB, roomWallB,
							east.alignmentGroup);
						AddWall(x2, portalBottom, x2, bottom, approachB, roomWallB,
							east.alignmentGroup);
						AddWall(doorLeft, bottom, x1, bottom, approachA, jamb,
							east.alignmentGroup);
						AddWall(doorRight, bottom, doorLeft, bottom, east.doorSector, track,
							east.alignmentGroup);
						AddWall(x2, bottom, doorRight, bottom, approachB, jamb,
							east.alignmentGroup);
						AddWall(x1, bottom, x1, portalBottom, approachA, roomWallA,
							east.alignmentGroup);

					AddDoorFace(doorLeft, top, doorLeft, bottom,
						approachA, east.doorSector, east,
						SafeTexture(Rooms[roomA].wallTex, "STARTAN3"));
					AddDoorFace(doorRight, bottom, doorRight, top,
						approachB, east.doorSector, west,
						SafeTexture(Rooms[roomB].wallTex, "STARTAN3"));
				}
				else
				{
					const double portalTop = cy + east.halfWidth;
					const double portalBottom = cy - east.halfWidth;
					const double top = portalTop + revealDepth;
					const double bottom = portalBottom - revealDepth;
					if (east.stairIndex >= 0)
					{
						const StairConnection& staircase = stairConnections[east.stairIndex];
						const char* stepWall = SafeTexture(
							Rooms[Grid[y][x].roomId].accentTex, "STEP1");
						if (staircase.dogleg)
						{
							if (!EmitDoglegStair(true, x1, x2, cy, staircase,
								corridorWall, stepWall))
							{
								LastError = "A planned dogleg staircase failed its connector fit check";
								return false;
							}
						}
						else
						{
							const int stepCount = staircase.sectors.Size();
							const int firstSector = staircase.sectors[0];
							const int lastSector = staircase.sectors.Last();
								AddWall(x1, portalTop, x1, top, firstSector, corridorWall,
									staircase.alignmentGroup);
								AddWall(x2, top, x2, portalTop, lastSector, corridorWall,
									staircase.alignmentGroup);
								AddWall(x2, portalBottom, x2, bottom, lastSector, corridorWall,
									staircase.alignmentGroup);
								AddWall(x1, bottom, x1, portalBottom, firstSector, corridorWall,
									staircase.alignmentGroup);
							for (int step = 0; step < stepCount; step++)
							{
								const double stepX1 = x1 + (x2 - x1) * step / stepCount;
								const double stepX2 = x1 + (x2 - x1) * (step + 1) / stepCount;
								const int stepSector = staircase.sectors[step];
									AddWall(stepX1, top, stepX2, top, stepSector, corridorWall,
										staircase.alignmentGroup);
									AddWall(stepX2, bottom, stepX1, bottom, stepSector, corridorWall,
										staircase.alignmentGroup);
								if (step + 1 < stepCount)
								{
									AddLine(stepX2, top, stepX2, bottom,
										stepSector, staircase.sectors[step + 1],
										stepWall, nullptr, stepWall,
										stepWall, nullptr, stepWall,
										false, 0, 0, 0, 0, 0, 0, 0,
										false, false, false, true, true, false,
										staircase.alignmentGroup);
								}
							}
						}
					}
					else
					{
						const int roomA = Grid[y][x].roomId;
						const int roomB = Grid[y][x + 1].roomId;
						const char* endWallA = east.sector == Rooms[roomA].sectorIdx ?
							SafeTexture(Rooms[roomA].wallTex, "STARTAN3") : corridorWall;
						const char* endWallB = east.sector == Rooms[roomB].sectorIdx ?
							SafeTexture(Rooms[roomB].wallTex, "STARTAN3") : corridorWall;
							AddWall(x1, portalTop, x1, top, east.sector, endWallA,
								east.alignmentGroup);
							AddWall(x1, top, x2, top, east.sector, corridorWall,
								east.alignmentGroup);
							AddWall(x2, top, x2, portalTop, east.sector, endWallB,
								east.alignmentGroup);
							AddWall(x2, portalBottom, x2, bottom, east.sector, endWallB,
								east.alignmentGroup);
							AddWall(x2, bottom, x1, bottom, east.sector, corridorWall,
								east.alignmentGroup);
							AddWall(x1, bottom, x1, portalBottom, east.sector, endWallA,
								east.alignmentGroup);
					}
				}
				if (!east.window && !east.secret)
				{
					// A straight capsule spans the exact final connector centerline.
					// Doglegs instead retain the three real flight/landing/outlet lanes
					// emitted above; a direct chord would incorrectly bypass their turn.
					const bool dogleg = east.stairIndex >= 0 &&
						stairConnections[east.stairIndex].dogleg;
					if (!dogleg)
						ReserveNavigationSegment(x1, cy, x2, cy,
							std::max(32.0, std::min(east.halfWidth, 64.0)));
					RecordCollisionCorridor(x, y, x + 1, y,
						collisionReservationFirst);
				}
			}

			const ConnectionRef& north = connectionGrid[y][x].refs[DIR_S];
			const bool northInsideUnifiedEnvelope = y + 1 < H &&
				Grid[y + 1][x].present && Grid[y + 1][x].roomId == Grid[y][x].roomId &&
				IsValidRoom(Grid[y][x].roomId) &&
				unifiedEnvelopeRooms[Grid[y][x].roomId] &&
				Rooms[Grid[y][x].roomId].contourUnified;
			if (!northInsideUnifiedEnvelope && north.sector >= 0 && y + 1 < H && Grid[y + 1][x].present)
			{
				double y1 = CellCenterY(y) + EdgeForCell(x, y, DIR_S);
				double y2 = CellCenterY(y + 1) - EdgeForCell(x, y + 1, DIR_N);
				double cx = CellCenterX(x);
				const double revealDepth = clamp(CELL_HALF - 8.0 - north.halfWidth,
					0.0, 8.0);
				const int collisionReservationFirst = navigationReservations.Size();
				if (north.door && north.doorSector >= 0)
				{
					int roomA = Grid[y][x].roomId;
					int roomB = Grid[y + 1][x].roomId;
					const ConnectionRef& south = connectionGrid[y + 1][x].refs[DIR_N];
					const int approachA = north.sector;
					const int approachB = south.sector;
					double mid = (y1 + y2) * 0.5;
					double doorBottom = mid - 8.0;
					double doorTop = mid + 8.0;
					double portalLeft = cx - north.halfWidth;
					double portalRight = cx + north.halfWidth;
					double left = portalLeft;
					double right = portalRight;
					const char* track = north.secret ? corridorWall : DoorTrackTexture(north.lockType);
					const char* jamb = north.lockType > 0 ? track : corridorWall;
					const char* roomWallA = SafeTexture(Rooms[roomA].wallTex, "STARTAN3");
					const char* roomWallB = SafeTexture(Rooms[roomB].wallTex, "STARTAN3");

						AddWall(portalRight, y2, right, y2, approachB, roomWallB,
							north.alignmentGroup);
						AddWall(right, doorBottom, right, y1, approachA, jamb,
							north.alignmentGroup);
						AddWall(right, doorTop, right, doorBottom, north.doorSector, track,
							north.alignmentGroup);
						AddWall(right, y2, right, doorTop, approachB, jamb,
							north.alignmentGroup);
						AddWall(right, y1, portalRight, y1, approachA, roomWallA,
							north.alignmentGroup);
						AddWall(portalLeft, y1, left, y1, approachA, roomWallA,
							north.alignmentGroup);
						AddWall(left, y1, left, doorBottom, approachA, jamb,
							north.alignmentGroup);
						AddWall(left, doorBottom, left, doorTop, north.doorSector, track,
							north.alignmentGroup);
						AddWall(left, doorTop, left, y2, approachB, jamb,
							north.alignmentGroup);
						AddWall(left, y2, portalLeft, y2, approachB, roomWallB,
							north.alignmentGroup);

					AddDoorFace(left, doorBottom, right, doorBottom,
						approachA, north.doorSector, north,
						SafeTexture(Rooms[roomA].wallTex, "STARTAN3"));
					AddDoorFace(right, doorTop, left, doorTop,
						approachB, north.doorSector, south,
						SafeTexture(Rooms[roomB].wallTex, "STARTAN3"));
				}
				else
				{
					const double portalLeft = cx - north.halfWidth;
					const double portalRight = cx + north.halfWidth;
					const double left = portalLeft - revealDepth;
					const double right = portalRight + revealDepth;
					if (north.stairIndex >= 0)
					{
						const StairConnection& staircase = stairConnections[north.stairIndex];
						const char* stepWall = SafeTexture(
							Rooms[Grid[y][x].roomId].accentTex, "STEP1");
						if (staircase.dogleg)
						{
							if (!EmitDoglegStair(false, y1, y2, cx, staircase,
								corridorWall, stepWall))
							{
								LastError = "A planned dogleg staircase failed its connector fit check";
								return false;
							}
						}
						else
						{
							const int stepCount = staircase.sectors.Size();
							const int firstSector = staircase.sectors[0];
							const int lastSector = staircase.sectors.Last();
								AddWall(portalRight, y2, right, y2, lastSector, corridorWall,
									staircase.alignmentGroup);
								AddWall(right, y1, portalRight, y1, firstSector, corridorWall,
									staircase.alignmentGroup);
								AddWall(portalLeft, y1, left, y1, firstSector, corridorWall,
									staircase.alignmentGroup);
								AddWall(left, y2, portalLeft, y2, lastSector, corridorWall,
									staircase.alignmentGroup);
							for (int step = 0; step < stepCount; step++)
							{
								const double stepY1 = y1 + (y2 - y1) * step / stepCount;
								const double stepY2 = y1 + (y2 - y1) * (step + 1) / stepCount;
								const int stepSector = staircase.sectors[step];
									AddWall(right, stepY2, right, stepY1, stepSector, corridorWall,
										staircase.alignmentGroup);
									AddWall(left, stepY1, left, stepY2, stepSector, corridorWall,
										staircase.alignmentGroup);
								if (step + 1 < stepCount)
								{
									AddLine(left, stepY2, right, stepY2,
										stepSector, staircase.sectors[step + 1],
										stepWall, nullptr, stepWall,
										stepWall, nullptr, stepWall,
										false, 0, 0, 0, 0, 0, 0, 0,
										false, false, false, true, true, false,
										staircase.alignmentGroup);
								}
							}
						}
					}
					else
					{
						const int roomA = Grid[y][x].roomId;
						const int roomB = Grid[y + 1][x].roomId;
						const char* endWallA = north.sector == Rooms[roomA].sectorIdx ?
							SafeTexture(Rooms[roomA].wallTex, "STARTAN3") : corridorWall;
						const char* endWallB = north.sector == Rooms[roomB].sectorIdx ?
							SafeTexture(Rooms[roomB].wallTex, "STARTAN3") : corridorWall;
							AddWall(portalRight, y2, right, y2, north.sector, endWallB,
								north.alignmentGroup);
							AddWall(right, y2, right, y1, north.sector, corridorWall,
								north.alignmentGroup);
							AddWall(right, y1, portalRight, y1, north.sector, endWallA,
								north.alignmentGroup);
							AddWall(portalLeft, y1, left, y1, north.sector, endWallA,
								north.alignmentGroup);
							AddWall(left, y1, left, y2, north.sector, corridorWall,
								north.alignmentGroup);
							AddWall(left, y2, portalLeft, y2, north.sector, endWallB,
								north.alignmentGroup);
					}
				}
				if (!north.window && !north.secret)
				{
					const bool dogleg = north.stairIndex >= 0 &&
						stairConnections[north.stairIndex].dogleg;
					if (!dogleg)
						ReserveNavigationSegment(cx, y1, cx, y2,
							std::max(32.0, std::min(north.halfWidth, 64.0)));
					RecordCollisionCorridor(x, y, x, y + 1,
						collisionReservationFirst);
				}
			}
		}
	}

	auto ChooseMonster = [&](const RoomInfo& room, int enemyIndex) -> int
	{
		static const int DoomEarly[] = { 3004, 3004, 9, 3001, 3002 };
		static const int DoomMid[] = { 9, 3001, 3002, 3005, 3006 };
		static const int DoomLate[] = { 3001, 3002, 3003, 3005, 3006 };
		static const int EarlyInfantry[] = { 3004, 3004, 9, 3001 };
		static const int EarlyDemons[] = { 3001, 3001, 3002 };
		static const int MidInfantry[] = { 9, 3001, 65, 66 };
		static const int MidDemons[] = { 3002, 3002, 3001, 69 };
		static const int MidFlyers[] = { 3005, 3005, 3006, 3001 };
		static const int LateBruisers[] = { 69, 3002, 66, 3003 };
		static const int LateHeavy[] = { 66, 69, 67, 3003 };
		static const int LateAir[] = { 3005, 71, 66, 69 };
		int family = (room.id + room.progressionRank + room.branchDepth +
			(int)(blueprintHash & 3u)) % 3;
		switch ((EProcGenEncounterCard)room.encounterCard)
		{
		case PGEC_Crossfire:
			family = 2; // flyers/ranged pressure where the room can support it
			break;
		case PGEC_Pincer:
		case PGEC_Ambush:
			family = 1; // melee screens and close threats
			break;
		case PGEC_HoldingLine:
			family = 0; // readable infantry line
			break;
		case PGEC_SetPiece:
			family = (int)(StableRoomHash(room.id, 0x53455450u) % 3u);
			break;
		default:
			break;
		}
		int jitter = enemyIndex +
			(int)(StableRoomHash(room.id, 0x4d4f4e53u + (uint32_t)enemyIndex) % 3u);
		if (!ProcGenUsesDoom2Roster())
		{
			if (room.monsterTier <= 2) return DoomEarly[jitter % countof(DoomEarly)];
			if (room.monsterTier <= 4) return DoomMid[jitter % countof(DoomMid)];
			return DoomLate[jitter % countof(DoomLate)];
		}

		if (room.monsterTier <= 2)
		{
			if ((family & 1) == 0) return EarlyInfantry[jitter % countof(EarlyInfantry)];
			return EarlyDemons[jitter % countof(EarlyDemons)];
		}
		if (room.monsterTier <= 4)
		{
			if (family == 0) return MidInfantry[jitter % countof(MidInfantry)];
			if (family == 1) return MidDemons[jitter % countof(MidDemons)];
			return MidFlyers[jitter % countof(MidFlyers)];
		}
		if (family == 0) return LateBruisers[jitter % countof(LateBruisers)];
		if (family == 1) return LateHeavy[jitter % countof(LateHeavy)];
		return LateAir[jitter % countof(LateAir)];
	};

	auto ChooseRangedMonster = [&](const RoomInfo& room, int salt) -> int
	{
		static const int DoomRanged[] = { 3004, 9, 3001, 3001 };
		static const int Doom2Ranged[] = { 3004, 9, 3001, 65, 66 };
		if (!ProcGenUsesDoom2Roster())
			return DoomRanged[(room.monsterTier + salt +
				(int)(StableRoomHash(room.id, 0x52414e47u + (uint32_t)salt) % countof(DoomRanged))) %
				countof(DoomRanged)];
		int count = room.monsterTier >= 4 ? countof(Doom2Ranged) : 3;
		return Doom2Ranged[(room.monsterTier + salt +
			(int)(StableRoomHash(room.id, 0x52414e47u + (uint32_t)salt) % (uint32_t)count)) % count];
	};

	auto AddRemoteDoorFace = [&](double x1, double y1, double x2, double y2,
		int roomSector, int doorSector, const char* doorTexture, const char* roomWall,
		bool secretFace = false)
	{
		// Optional caches never get to borrow clearance from a generic room: the
		// actual physical doorway is a reserved player lane before props emit.
		ReserveNavigationSegment(x1, y1, x2, y2, 48.0);
		int lineIndex = AddLine(x1, y1, x2, y2, roomSector, doorSector,
			doorTexture, nullptr, roomWall,
			doorTexture, nullptr, roomWall,
			false, 0, 0, 0, 0, 0, 0, 0,
			false, false, false, false, false);
		if (lineIndex < 0) return;
		if (doorSector >= 0) remoteCacheDoorSectors.Push(doorSector);
		lines[lineIndex].secret = secretFace;
		sides[lines[lineIndex].sideFront].topAlignment = TAM_DoorFit;
		sides[lines[lineIndex].sideBack].topAlignment = TAM_DoorFit;
	};
		auto AddManualSecretDoorFace = [&](double x1, double y1, double x2, double y2,
			int approachSector, int doorSector, const char* doorTexture, const char* wall)
		{
		ReserveNavigationSegment(x1, y1, x2, y2, 64.0);
		int lineIndex = AddLine(x1, y1, x2, y2, approachSector, doorSector,
			doorTexture, nullptr, wall, doorTexture, nullptr, wall,
			false, 12, 0, 0, 16, 150, 0, 0,
			true, false, true, false, false);
		if (lineIndex < 0) return;
		lines[lineIndex].secret = true;
		sides[lines[lineIndex].sideFront].topAlignment = TAM_DoorFit;
			sides[lines[lineIndex].sideBack].topAlignment = TAM_DoorFit;
		};

		auto BuildRevealOuterLoop = [&](double cx, double cy,
			const RevealProfile& profile, double doorHalf) -> TArray<std::pair<double, double>>
		{
			const double delta = profile.variant == 0 ? 0.0 :
				(profile.variant == 1 ? 6.0 : (profile.variant == 2 ? 10.0 : 8.0));
			double cuts[4] = { profile.outerChamfer, profile.outerChamfer,
				profile.outerChamfer, profile.outerChamfer };
			if (profile.variant == 1)
			{
				cuts[0] += delta;
				cuts[1] -= delta;
				cuts[2] += delta;
				cuts[3] -= delta;
			}
			else if (profile.variant == 2)
			{
				cuts[0] -= delta;
				cuts[1] -= delta;
				cuts[2] += delta;
				cuts[3] += delta;
			}
			else if (profile.variant == 3)
			{
				cuts[0] += delta;
				cuts[2] -= delta;
			}
			const double maxCut = std::max(6.0,
				std::min(profile.outerX, profile.outerY) - doorHalf - 2.0);
			for (double& cut : cuts) cut = clamp(cut, 6.0, maxCut);
			TArray<std::pair<double, double>> points;
			points.Push(std::make_pair(cx - profile.outerX + cuts[0], cy - profile.outerY));
			points.Push(std::make_pair(cx + profile.outerX - cuts[1], cy - profile.outerY));
			points.Push(std::make_pair(cx + profile.outerX, cy - profile.outerY + cuts[1]));
			points.Push(std::make_pair(cx + profile.outerX, cy + profile.outerY - cuts[2]));
			points.Push(std::make_pair(cx + profile.outerX - cuts[2], cy + profile.outerY));
			points.Push(std::make_pair(cx - profile.outerX + cuts[3], cy + profile.outerY));
			points.Push(std::make_pair(cx - profile.outerX, cy + profile.outerY - cuts[3]));
			points.Push(std::make_pair(cx - profile.outerX, cy - profile.outerY + cuts[0]));
			return points;
		};

		auto MeasureWallAlcoveBackingClearance = [&](const RoomInfo& room,
			const TArray<std::pair<double, double>>& outer, int backDirection,
			double cx, double cy, double tangentExtent) -> double
		{
			// A room footprint can reshape its nominal cell edge before the optional
			// cache is emitted. Anchor a wall alcove to that actual host-sector wall,
			// not merely to EdgeForCell's coarse-grid position.
			const double normalX = DX[backDirection];
			const double normalY = DY[backDirection];
			const double tangentX = -normalY;
			const double tangentY = normalX;
			double clearance = 64.0;
			bool found = false;
			for (const auto& point : outer)
			{
				for (const BuildLine& line : lines)
				{
					if (line.sideBack >= 0 || line.sideFront < 0 ||
						sides[line.sideFront].sector != room.sectorIdx)
						continue;
					const BuildVertex& first = vertices[line.v1];
					const BuildVertex& second = vertices[line.v2];
					const double segmentX = second.x - first.x;
					const double segmentY = second.y - first.y;
					const double segmentLengthSquared =
						segmentX * segmentX + segmentY * segmentY;
					if (segmentLengthSquared <= 0.001) continue;
					const double fraction = clamp(((point.first - first.x) * segmentX +
						(point.second - first.y) * segmentY) / segmentLengthSquared, 0.0, 1.0);
					const double closestX = first.x + segmentX * fraction;
					const double closestY = first.y + segmentY * fraction;
					const double ahead = (closestX - point.first) * normalX +
						(closestY - point.second) * normalY;
					if (ahead < -0.01) continue;
					const double tangentOffset = fabs((closestX - cx) * tangentX +
						(closestY - cy) * tangentY);
					if (tangentOffset > tangentExtent + 32.0) continue;
					const double distance = hypot(closestX - point.first,
						closestY - point.second);
					if (distance <= 48.0)
					{
						clearance = std::min(clearance, distance);
						found = true;
					}
				}
			}
			return found ? clearance : -1.0;
		};

		auto AddRevealCloset = [&](const RoomInfo& room, double cx, double cy,
		int targetTag, int borderType, const RevealProfile& profile,
		int doorSide, int architecture, int cue, bool manualSecret) -> int
	{
		const double doorHalf = manualSecret || architecture == RevealWallAlcove ? 32.0 : 40.0;
		const char* roomWall = SafeTexture(room.wallTex, "STARTAN3");
		const bool hiddenDoor = manualSecret || cue == RevealHidden;
		const bool subtleDoor = cue == RevealSubtle;
		const char* closetWall = (profile.variant & 2) != 0 ?
			SafeTexture(room.detailTex, roomWall) : SafeTexture(room.accentTex, roomWall);
		const char* prominentTexture = borderType > 0 ? LockedDoorTexture(borderType) :
			(themeStyle == ThemeIndustrial ? "BIGDOOR5" :
				(themeStyle == ThemeGothic || themeStyle == ThemeHell ? "BIGDOOR6" :
					(themeStyle == ThemeCorrupted ? "BIGDOOR7" : "BIGDOOR1")));
		const char* doorTexture = hiddenDoor ? roomWall :
			(subtleDoor ? SafeTexture(room.detailTex, roomWall) : prominentTexture);
		const char* track = borderType > 0 ? DoorTrackTexture(borderType) :
			(hiddenDoor ? roomWall :
				(subtleDoor ? SafeTexture(room.accentTex, roomWall) : DoorTrackTexture(0)));
		static const char* TechRevealFloors[] = {
			"FLOOR0_1", "FLAT20", "FLOOR5_2", "FLAT14"
		};
		static const char* HellRevealFloors[] = {
			"FLAT5_1", "FLOOR7_2", "FLAT8", "FLAT5_2"
		};
		static const char* IndustrialRevealFloors[] = {
			"FLOOR0_1", "FLAT20", "FLOOR5_2", "FLAT14"
		};
		static const char* GothicRevealFloors[] = {
			"FLAT10", "FLOOR7_2", "FLAT5_1", "FLAT8"
		};
		const char** revealFloors = TechRevealFloors;
		if (themeStyle == ThemeIndustrial) revealFloors = IndustrialRevealFloors;
		else if (themeStyle == ThemeGothic) revealFloors = GothicRevealFloors;
		else if (themeStyle == ThemeHell ||
			(themeStyle == ThemeCorrupted && room.lockStage >= 2))
			revealFloors = HellRevealFloors;
		const char* revealFloor = revealFloors[profile.variant % 4];
		const double closetFloor = manualSecret ? room.floorZ : room.floorZ + profile.floorDelta;
		const double closetCeil = manualSecret ? closetFloor + 128.0 :
			std::max(closetFloor + 96.0, room.ceilZ - profile.ceilingDrop);
		const int closetLight = room.light +
			(profile.variant == 0 ? 16 : (profile.variant == 1 ? -8 : 8));
		int closetSector = AddSector(closetFloor, closetCeil,
			revealFloor, SafeTexture(room.ceilTex, "CEIL3_5"), closetLight);
		int doorSector = AddSector(room.floorZ, room.floorZ,
			revealFloor, SafeTexture(room.ceilTex, "CEIL3_5"),
			closetLight, targetTag);
		ApplyRoomLighting(closetSector, room, false);
		ApplyRoomLighting(doorSector, room, false);
		// Switch-operated opportunity caches are genuine secrets regardless of
		// whether their reveal face is disguised, subtly framed, or prominent.
		// Key-triggered ambush chambers deliberately remain ordinary encounters.
		if (manualSecret || targetTag >= 1500)
			sectors[closetSector].special = 0x0400;

		auto MakeChamferedLoop = [&](double halfX, double halfY,
			double chamfer) -> TArray<std::pair<double, double>>
		{
			const double delta = profile.variant == 0 ? 0.0 :
				(profile.variant == 1 ? 6.0 : (profile.variant == 2 ? 10.0 : 8.0));
			double cuts[4] = { chamfer, chamfer, chamfer, chamfer };
			if (profile.variant == 1)
			{
				cuts[0] += delta;
				cuts[1] -= delta;
				cuts[2] += delta;
				cuts[3] -= delta;
			}
			else if (profile.variant == 2)
			{
				cuts[0] -= delta;
				cuts[1] -= delta;
				cuts[2] += delta;
				cuts[3] += delta;
			}
			else if (profile.variant == 3)
			{
				cuts[0] += delta;
				cuts[2] -= delta;
			}
			// Any cardinal edge can own the 80-unit door. Preserve shoulders on
			// every edge even when the asymmetric profile enlarges a corner cut.
			const double maxCut = std::max(6.0,
				std::min(halfX, halfY) - doorHalf - 2.0);
			for (double& cut : cuts) cut = clamp(cut, 6.0, maxCut);
			TArray<std::pair<double, double>> points;
			points.Push(std::make_pair(cx - halfX + cuts[0], cy - halfY));
			points.Push(std::make_pair(cx + halfX - cuts[1], cy - halfY));
			points.Push(std::make_pair(cx + halfX, cy - halfY + cuts[1]));
			points.Push(std::make_pair(cx + halfX, cy + halfY - cuts[2]));
			points.Push(std::make_pair(cx + halfX - cuts[2], cy + halfY));
			points.Push(std::make_pair(cx - halfX + cuts[3], cy + halfY));
			points.Push(std::make_pair(cx - halfX, cy + halfY - cuts[3]));
			points.Push(std::make_pair(cx - halfX, cy - halfY + cuts[0]));
			return points;
		};
		const TArray<std::pair<double, double>> outer = MakeChamferedLoop(
			profile.outerX, profile.outerY, profile.outerChamfer);
		// A manual secret uses Door_Raise, whose moving sector is deliberately
		// the classic 16-unit slab. Reveal profiles otherwise vary their moat
		// from 16 through 28 units for visual variety; carrying that variable
		// depth into a Door_Raise sector makes its two usable faces disagree with
		// the stock door contract. Keep the outer pavilion grammar, but realize
		// its inner loop exactly one slab depth inward.
		const double innerX = manualSecret ? profile.outerX - 16.0 : profile.innerX;
		const double innerY = manualSecret ? profile.outerY - 16.0 : profile.innerY;
		const double innerChamfer = manualSecret ? std::min(profile.outerChamfer,
			std::max(4.0, std::min(innerX, innerY) - doorHalf - 2.0)) :
			profile.innerChamfer;
		const TArray<std::pair<double, double>> inner = MakeChamferedLoop(
			innerX, innerY, innerChamfer);
		const int doorEdge = clamp(doorSide, 0, 3) * 2;

		auto DoorEndpoints = [&](const TArray<std::pair<double, double>>& points,
			double& ax, double& ay, double& bx, double& by)
		{
			const auto& first = points[doorEdge];
			const auto& second = points[(doorEdge + 1) % points.Size()];
			const double length = hypot(second.first - first.first,
				second.second - first.second);
			const double ux = (second.first - first.first) / length;
			const double uy = (second.second - first.second) / length;
			const double mx = (first.first + second.first) * 0.5;
			const double my = (first.second + second.second) * 0.5;
			ax = mx - ux * doorHalf;
			ay = my - uy * doorHalf;
			bx = mx + ux * doorHalf;
			by = my + uy * doorHalf;
		};
		double outerAX, outerAY, outerBX, outerBY;
		double innerAX, innerAY, innerBX, innerBY;
		DoorEndpoints(outer, outerAX, outerAY, outerBX, outerBY);
		// Project the outer doorway center onto the inner edge. Asymmetric
		// chamfers may shorten different corners, but the moving slab must remain
		// normal to both loops instead of acquiring a subtle diagonal skew.
		const auto& innerFirst = inner[doorEdge];
		const auto& innerSecond = inner[(doorEdge + 1) % inner.Size()];
		const double innerLength = hypot(innerSecond.first - innerFirst.first,
			innerSecond.second - innerFirst.second);
		const double innerUnitX = (innerSecond.first - innerFirst.first) / innerLength;
		const double innerUnitY = (innerSecond.second - innerFirst.second) / innerLength;
		const double outerCenterX = (outerAX + outerBX) * 0.5;
		const double outerCenterY = (outerAY + outerBY) * 0.5;
		double innerCenterDistance =
			(outerCenterX - innerFirst.first) * innerUnitX +
			(outerCenterY - innerFirst.second) * innerUnitY;
		innerCenterDistance = clamp(innerCenterDistance, doorHalf,
			innerLength - doorHalf);
		const double innerCenterX = innerFirst.first + innerUnitX * innerCenterDistance;
		const double innerCenterY = innerFirst.second + innerUnitY * innerCenterDistance;
		innerAX = innerCenterX - innerUnitX * doorHalf;
		innerAY = innerCenterY - innerUnitY * doorHalf;
		innerBX = innerCenterX + innerUnitX * doorHalf;
		innerBY = innerCenterY + innerUnitY * doorHalf;

		// The outer loop is counter-clockwise so its front/right side faces the
		// surrounding room. Four clipped corners replace the former rectangular
		// island, and only the selected cardinal edge contains a door gap.
		for (unsigned int index = 0; index < outer.Size(); index++)
		{
			const auto& first = outer[index];
			const auto& second = outer[(index + 1) % outer.Size()];
			if ((int)index == doorEdge)
			{
				AddWall(first.first, first.second, outerAX, outerAY,
					room.sectorIdx, roomWall);
				AddWall(outerBX, outerBY, second.first, second.second,
					room.sectorIdx, roomWall);
			}
			else AddWall(first.first, first.second, second.first, second.second,
				room.sectorIdx, roomWall);
		}

		// Reverse every inner edge so the front/right side faces the closet's
		// playable interior. Its chamfer and moat depth vary with the room profile.
		for (unsigned int index = 0; index < inner.Size(); index++)
		{
			const auto& first = inner[index];
			const auto& second = inner[(index + 1) % inner.Size()];
			if ((int)index == doorEdge)
			{
				AddWall(second.first, second.second, innerBX, innerBY,
					closetSector, closetWall);
				AddWall(innerAX, innerAY, first.first, first.second,
					closetSector, closetWall);
			}
			else AddWall(second.first, second.second, first.first, first.second,
				closetSector, closetWall);
		}
		if (architecture == RevealWallAlcove)
		{
			// Wall banks receive a pair of structural piers at their deep corners.
			// This makes them read as recessed galleries rather than a rectangular
			// version of the freestanding pavilion, without narrowing the door route.
			static const double TangentX[] = { 1.0, 0.0, -1.0, 0.0 };
			static const double TangentY[] = { 0.0, 1.0, 0.0, -1.0 };
			static const double InwardX[] = { 0.0, -1.0, 0.0, 1.0 };
			static const double InwardY[] = { 1.0, 0.0, -1.0, 0.0 };
			const double tangentExtent = (doorSide & 1) ? profile.innerY : profile.innerX;
			const double depthExtent = (doorSide & 1) ? profile.innerX : profile.innerY;
			for (double side : { -1.0, 1.0 })
			{
				const double pillarX = cx + TangentX[doorSide] * side * (tangentExtent - 30.0) +
					InwardX[doorSide] * (depthExtent - 30.0);
				const double pillarY = cy + TangentY[doorSide] * side * (tangentExtent - 30.0) +
					InwardY[doorSide] * (depthExtent - 30.0);
				const double half = 7.0;
				AddWall(pillarX - half, pillarY - half, pillarX + half, pillarY - half,
					closetSector, closetWall);
				AddWall(pillarX + half, pillarY - half, pillarX + half, pillarY + half,
					closetSector, closetWall);
				AddWall(pillarX + half, pillarY + half, pillarX - half, pillarY + half,
					closetSector, closetWall);
				AddWall(pillarX - half, pillarY + half, pillarX - half, pillarY - half,
					closetSector, closetWall);
			}
		}

		if (manualSecret)
		{
			// Door_Raise faces need a stock-height surface on both sides.  A compact
			// 64x16 antechamber gives the outer face the same 128-unit clearance as
			// the closet without changing the host room's floor or route geometry.
			const double doorLength = hypot(outerBX - outerAX, outerBY - outerAY);
			const double outwardX = (outerBY - outerAY) / doorLength;
			const double outwardY = -(outerBX - outerAX) / doorLength;
			const double apronDepth = 16.0;
			const double apronAX = outerAX + outwardX * apronDepth;
			const double apronAY = outerAY + outwardY * apronDepth;
			const double apronBX = outerBX + outwardX * apronDepth;
			const double apronBY = outerBY + outwardY * apronDepth;
			const int apronSector = AddSector(room.floorZ, room.floorZ + 128.0,
				revealFloor, SafeTexture(room.ceilTex, "CEIL3_5"), closetLight);
			ApplyRoomLighting(apronSector, room, false);
			ReserveNavigationSegment(apronAX, apronAY, apronBX, apronBY, 48.0);
			AddLine(apronBX, apronBY, apronAX, apronAY, apronSector, room.sectorIdx,
				roomWall, nullptr, roomWall, roomWall, nullptr, roomWall,
				false, 0, 0, 0, 0, 0, 0, 0,
				false, false, false, true, true);
			AddLine(apronAX, apronAY, outerAX, outerAY, apronSector, room.sectorIdx,
				roomWall, nullptr, roomWall, roomWall, nullptr, roomWall,
				false, 0, 0, 0, 0, 0, 0, 0,
				false, false, false, true, true);
			AddLine(outerBX, outerBY, apronBX, apronBY, apronSector, room.sectorIdx,
				roomWall, nullptr, roomWall, roomWall, nullptr, roomWall,
				false, 0, 0, 0, 0, 0, 0, 0,
				false, false, false, true, true);
			const double innerDoorLength = hypot(innerAX - innerBX, innerAY - innerBY);
			const double closetX = (innerAY - innerBY) / innerDoorLength;
			const double closetY = -(innerAX - innerBX) / innerDoorLength;
			const double innerApronAX = innerBX + closetX * apronDepth;
			const double innerApronAY = innerBY + closetY * apronDepth;
			const double innerApronBX = innerAX + closetX * apronDepth;
			const double innerApronBY = innerAY + closetY * apronDepth;
			const int innerApronSector = AddSector(room.floorZ, room.floorZ + 128.0,
				revealFloor, SafeTexture(room.ceilTex, "CEIL3_5"), closetLight);
			ApplyRoomLighting(innerApronSector, room, false);
			ReserveNavigationSegment(innerApronAX, innerApronAY,
				innerApronBX, innerApronBY, 48.0);
			AddLine(innerApronBX, innerApronBY, innerApronAX, innerApronAY,
				innerApronSector, closetSector,
				closetWall, nullptr, closetWall, closetWall, nullptr, closetWall,
				false, 0, 0, 0, 0, 0, 0, 0,
				false, false, false, true, true);
			AddLine(innerApronAX, innerApronAY, innerBX, innerBY,
				innerApronSector, closetSector,
				closetWall, nullptr, closetWall, closetWall, nullptr, closetWall,
				false, 0, 0, 0, 0, 0, 0, 0,
				false, false, false, true, true);
			AddLine(innerAX, innerAY, innerApronBX, innerApronBY,
				innerApronSector, closetSector,
				closetWall, nullptr, closetWall, closetWall, nullptr, closetWall,
				false, 0, 0, 0, 0, 0, 0, 0,
				false, false, false, true, true);
			AddManualSecretDoorFace(outerAX, outerAY, outerBX, outerBY,
				apronSector, doorSector, doorTexture, roomWall);
			AddManualSecretDoorFace(innerBX, innerBY, innerAX, innerAY,
				innerApronSector, doorSector, doorTexture, closetWall);
		}
		else
		{
			AddRemoteDoorFace(outerAX, outerAY, outerBX, outerBY,
				room.sectorIdx, doorSector, doorTexture, roomWall, hiddenDoor);
			AddRemoteDoorFace(innerBX, innerBY, innerAX, innerAY,
				closetSector, doorSector, doorTexture, closetWall, hiddenDoor);
		}
		AddWall(outerAX, outerAY, innerAX, innerAY, doorSector, track);
		AddWall(innerBX, innerBY, outerBX, outerBY, doorSector, track);
		return closetSector;
	};

	auto AddFalseWallCloset = [&](const RoomInfo& room, int wallLineIndex,
		int targetTag, int borderType, const RevealProfile& profile,
		int doorSide, int cue, double& actorCenterX, double& actorCenterY) -> int
	{
		if (wallLineIndex < 0 || wallLineIndex >= (int)lines.Size()) return -1;
		BuildLine& doorLine = lines[wallLineIndex];
		if (doorLine.sideBack >= 0) return -1;
		const double outerAX = vertices[doorLine.v1].x;
		const double outerAY = vertices[doorLine.v1].y;
		const double outerBX = vertices[doorLine.v2].x;
		const double outerBY = vertices[doorLine.v2].y;
		const double faceWidth = hypot(outerBX - outerAX, outerBY - outerAY);
		if (faceWidth < 95.9) return -1;

		const char* roomWall = SafeTexture(room.wallTex, "STARTAN3");
		const char* closetWall = (profile.variant & 2) != 0 ?
			SafeTexture(room.detailTex, roomWall) : SafeTexture(room.accentTex, roomWall);
		const char* prominentTexture = borderType > 0 ? LockedDoorTexture(borderType) :
			(themeStyle == ThemeIndustrial ? "BIGDOOR5" :
				(themeStyle == ThemeGothic || themeStyle == ThemeHell ? "BIGDOOR6" :
					(themeStyle == ThemeCorrupted ? "BIGDOOR7" : "BIGDOOR1")));
		const bool hiddenDoor = cue == RevealHidden;
		const bool subtleDoor = cue == RevealSubtle;
		const char* doorTexture = hiddenDoor ? roomWall :
			(subtleDoor ? SafeTexture(room.detailTex, roomWall) : prominentTexture);
		const char* track = borderType > 0 && !hiddenDoor ? DoorTrackTexture(borderType) :
			(hiddenDoor ? roomWall : SafeTexture(room.accentTex, DoorTrackTexture(0)));
		const char* revealFloor = themeStyle == ThemeHell ? "FLAT5_2" :
			(themeStyle == ThemeGothic ? "FLAT10" :
				(themeStyle == ThemeIndustrial ? "FLOOR0_1" :
					SafeTexture(room.floorTex, "FLAT20")));
		const double closetFloor = room.floorZ + profile.floorDelta;
		const double closetCeil = std::max(closetFloor + 96.0,
			room.ceilZ - profile.ceilingDrop);
		const int closetLight = room.light +
			(profile.variant == 0 ? 16 : (profile.variant == 1 ? -8 : 8));
		const int closetSector = AddSector(closetFloor, closetCeil,
			revealFloor, SafeTexture(room.ceilTex, "CEIL3_5"), closetLight);
		const int doorSector = AddSector(room.floorZ, room.floorZ,
			revealFloor, SafeTexture(room.ceilTex, "CEIL3_5"), closetLight, targetTag);
		ApplyRoomLighting(closetSector, room, false);
		ApplyRoomLighting(doorSector, room, false);
		if (targetTag >= 1500)
			sectors[closetSector].special = 0x0400;

		// Turn the reserved ordinary wall segment into one side of a remotely
		// operated door. The room-matching cue remains a genuine false wall and is
		// hidden on the automap; framed/prominent variants expose progressively
		// stronger architectural hints without changing activation behavior.
		BuildSide& roomSide = sides[doorLine.sideFront];
		roomSide.top = doorTexture;
		roomSide.middle = "-";
		roomSide.bottom = roomWall;
		roomSide.topAlignment = TAM_DoorFit;
		doorLine.sideBack = AddSide(doorSector, doorTexture, nullptr, roomWall);
		sides[doorLine.sideBack].topAlignment = TAM_DoorFit;
		sides[doorLine.sideBack].bottomAlignment = TAM_Architectural;
		doorLine.blocking = false;
		doorLine.dontPegBottom = false;
		doorLine.secret = hiddenDoor;
		// This outer face was an ordinary one-sided wall before it became the
		// room-to-door half of the remote cache. Reserve the actual converted
		// span, rather than assuming the inner closet face proves entry through
		// a false-wall cache. It is deliberately separate from the inner
		// AddRemoteDoorFace reservation below: both sides of the zero-height
		// door sector must be physically clear after the explicit switch use.
		ReserveNavigationSegment(outerAX, outerAY, outerBX, outerBY, 48.0);
		const uint32_t lowVertex = (uint32_t)std::min(doorLine.v1, doorLine.v2);
		const uint32_t highVertex = (uint32_t)std::max(doorLine.v1, doorLine.v2);
		solidWallLookup.Remove((uint64_t)lowVertex << 32 | highVertex);

		static const double OutwardX[] = { 0.0, 1.0, 0.0, -1.0 };
		static const double OutwardY[] = { -1.0, 0.0, 1.0, 0.0 };
		doorSide = clamp(doorSide, 0, 3);
		const double outwardX = OutwardX[doorSide];
		const double outwardY = OutwardY[doorSide];
		const double tangentX = (outerBX - outerAX) / faceWidth;
		const double tangentY = (outerBY - outerAY) / faceWidth;
		const double slabDepth = 16.0;
		const double innerAX = outerAX + outwardX * slabDepth;
		const double innerAY = outerAY + outwardY * slabDepth;
		const double innerBX = outerBX + outwardX * slabDepth;
		const double innerBY = outerBY + outwardY * slabDepth;
		const double centerX = (outerAX + outerBX) * 0.5;
		const double centerY = (outerAY + outerBY) * 0.5;
		auto Point = [&](double tangent, double outward, double& x, double& y)
		{
			x = centerX + tangentX * tangent + outwardX * outward;
			y = centerY + tangentY * tangent + outwardY * outward;
		};
		TArray<std::pair<double, double>> outline;
		double actorTangent = 0.0;
		double actorDepth = 0.0;
		auto AddOutlinePoint = [&](double tangent, double outward)
		{
			double x, y;
			Point(tangent, outward, x, y);
			outline.Push(std::make_pair(x, y));
		};
		if (profile.variant == 0)
		{
			// A narrow, deep firing slit.
			AddOutlinePoint(-48.0, slabDepth);
			AddOutlinePoint(-64.0, 80.0);
			AddOutlinePoint(-48.0, 208.0);
			AddOutlinePoint(48.0, 208.0);
			AddOutlinePoint(64.0, 80.0);
			AddOutlinePoint(48.0, slabDepth);
			actorDepth = 132.0;
		}
		else if (profile.variant == 1)
		{
			// A broad, shallow cache concealed behind an ordinary wall panel.
			AddOutlinePoint(-96.0, slabDepth);
			AddOutlinePoint(-112.0, 56.0);
			AddOutlinePoint(-88.0, 104.0);
			AddOutlinePoint(88.0, 104.0);
			AddOutlinePoint(112.0, 56.0);
			AddOutlinePoint(96.0, slabDepth);
			actorDepth = 64.0;
		}
		else if (profile.variant == 2)
		{
			// The chamber turns behind the wall, producing a genuine dogleg annex.
			AddOutlinePoint(-48.0, slabDepth);
			AddOutlinePoint(-64.0, 80.0);
			AddOutlinePoint(-64.0, 144.0);
			AddOutlinePoint(-8.0, 144.0);
			AddOutlinePoint(-8.0, 216.0);
			AddOutlinePoint(80.0, 216.0);
			AddOutlinePoint(80.0, 72.0);
			AddOutlinePoint(48.0, slabDepth);
			actorTangent = 28.0;
			actorDepth = 166.0;
		}
		else
		{
			// An expanding trapezoidal vault breaks the recurring closet box.
			AddOutlinePoint(-48.0, slabDepth);
			AddOutlinePoint(-88.0, 160.0);
			AddOutlinePoint(-72.0, 216.0);
			AddOutlinePoint(72.0, 216.0);
			AddOutlinePoint(88.0, 160.0);
			AddOutlinePoint(48.0, slabDepth);
			actorDepth = 150.0;
		}
		if (outline.Size() == 0) return -1;
		AddWall(innerAX, innerAY, outline[0].first, outline[0].second,
			closetSector, closetWall);
		for (unsigned int index = 0; index + 1 < outline.Size(); index++)
			AddWall(outline[index].first, outline[index].second,
				outline[index + 1].first, outline[index + 1].second,
				closetSector, closetWall);
		AddWall(outline.Last().first, outline.Last().second, innerBX, innerBY,
			closetSector, closetWall);
		AddRemoteDoorFace(innerBX, innerBY, innerAX, innerAY,
			closetSector, doorSector, doorTexture, closetWall, hiddenDoor);
		AddWall(outerAX, outerAY, innerAX, innerAY, doorSector, track);
		AddWall(innerBX, innerBY, outerBX, outerBY, doorSector, track);
		actorCenterX = centerX + tangentX * actorTangent + outwardX * actorDepth;
		actorCenterY = centerY + tangentY * actorTangent + outwardY * actorDepth;
		return closetSector;
	};

	auto AddSniperPerch = [&](const RoomInfo& room, double cx, double cy,
		int perchTag, bool sky, int approachSide, int variant,
		double& platformX, double& platformY) -> int
	{
		variant = clamp(variant, 0, 2);
		const double halfOutward = variant == 2 ? 48.0 : (variant == 1 ? 60.0 : 56.0);
		const double halfTangent = variant == 2 ? 72.0 : (variant == 1 ? 60.0 : 56.0);
		const double stairHalf = variant == 0 ? 40.0 : 32.0;
		const double stairOffset = variant == 1 ? 8.0 : 0.0;
		const double stepDepth = variant == 2 ? 48.0 : 24.0;
		const double doglegSign = ((room.id + room.visualVariant) & 1) != 0 ? 1.0 : -1.0;
		const double requestedRise = Difficulty >= 4 || variant == 1 ? 64.0 : 48.0;
		const double raisedFloor = std::min(room.floorZ + requestedRise,
			room.ceilZ - 80.0);
		const int riseSteps = clamp((int)lround((raisedFloor - room.floorZ) / 16.0), 3, 4);
		const int stairCount = riseSteps - 1;
		const char* floor = themeStyle == ThemeIndustrial ? "FLOOR0_1" :
			(themeStyle == ThemeGothic ? "FLOOR7_2" :
			(themeStyle == ThemeHell ? "FLAT5_2" :
			(themeStyle == ThemeCorrupted ? SafeTexture(room.floorTex, "FLAT5_2") : "FLAT20")));
		const char* wall = SafeTexture(room.accentTex, "STEP1");
		const char* ceiling = sky ? "F_SKY1" : SafeTexture(room.ceilTex, "CEIL3_5");
		int perchLight = std::min(room.light + 16, 224);
		if (sky) perchLight = std::max(perchLight, 192);
		int perchSector = AddSector(raisedFloor, room.ceilZ, floor,
			ceiling, perchLight, perchTag);
		ApplyRoomLighting(perchSector, room, sky);

		TArray<int> stairSectors;
		stairSectors.Resize(stairCount);
		for (int level = 0; level < stairCount; level++)
		{
			int stairLight = std::min(room.light + 4 * (level + 1), perchLight);
			if (sky) stairLight = std::max(stairLight, 192);
			stairSectors[level] = AddSector(room.floorZ + (level + 1) * 16.0,
				room.ceilZ, floor, ceiling, stairLight);
			ApplyRoomLighting(stairSectors[level], room, sky);
		}

		approachSide = clamp(approachSide, 0, 3);
		static const double OutwardX[] = { 0.0, 1.0, 0.0, -1.0 };
		static const double OutwardY[] = { -1.0, 0.0, 1.0, 0.0 };
		static const double TangentX[] = { 1.0, 0.0, -1.0, 0.0 };
		static const double TangentY[] = { 0.0, 1.0, 0.0, -1.0 };
		const double outwardX = OutwardX[approachSide];
		const double outwardY = OutwardY[approachSide];
		const double tangentX = TangentX[approachSide];
		const double tangentY = TangentY[approachSide];
		// The broad balcony backs toward the cell perimeter and returns its stair
		// along one side. The other profiles remain centered fighting platforms.
		platformX = cx - outwardX * (variant == 2 ? 48.0 : 0.0);
		platformY = cy - outwardY * (variant == 2 ? 48.0 : 0.0);
		auto Point = [&](double outward, double tangent,
			double& x, double& y)
		{
			x = platformX + outwardX * outward + tangentX * tangent;
			y = platformY + outwardY * outward + tangentY * tangent;
		};
		auto AddPerchEdge = [&](double x1, double y1, double x2, double y2,
			int frontSector, int backSector, bool retainMonster)
		{
			AddLine(x1, y1, x2, y2, frontSector, backSector,
				wall, nullptr, wall, wall, nullptr, wall,
				false, 0, 0, 0, 0, 0, 0, 0,
				false, false, false, true, true, retainMonster);
		};

		// Emit a centered square, an eight-sided turret, or a wide wall-backed
		// balcony. Every perimeter is clockwise and leaves exactly one stair mouth;
		// retaining edges constrain the initial ranged actor without blocking the
		// player's movement, shots, or deliberate drop-offs.
		double approachTopX, approachTopY, openingTopX, openingTopY;
		double openingBottomX, openingBottomY, approachBottomX, approachBottomY;
		const double approachExtent = variant == 1 ? halfTangent - 16.0 : halfTangent;
		Point(halfOutward, approachExtent, approachTopX, approachTopY);
		Point(halfOutward, stairOffset + stairHalf, openingTopX, openingTopY);
		Point(halfOutward, stairOffset - stairHalf, openingBottomX, openingBottomY);
		Point(halfOutward, -approachExtent, approachBottomX, approachBottomY);
		AddPerchEdge(approachTopX, approachTopY, openingTopX, openingTopY,
			perchSector, room.sectorIdx, true);
		AddPerchEdge(openingBottomX, openingBottomY,
			approachBottomX, approachBottomY, perchSector, room.sectorIdx, true);
		if (variant == 1)
		{
			const double chamfer = 16.0;
			const double localPoints[7][2] = {
				{ halfOutward - chamfer, -halfTangent },
				{ -halfOutward + chamfer, -halfTangent },
				{ -halfOutward, -halfTangent + chamfer },
				{ -halfOutward, halfTangent - chamfer },
				{ -halfOutward + chamfer, halfTangent },
				{ halfOutward - chamfer, halfTangent },
				{ halfOutward, halfTangent - chamfer },
			};
			double previousX, previousY;
			Point(halfOutward, -halfTangent + chamfer, previousX, previousY);
			for (const auto& local : localPoints)
			{
				double nextX, nextY;
				Point(local[0], local[1], nextX, nextY);
				AddPerchEdge(previousX, previousY, nextX, nextY,
					perchSector, room.sectorIdx, true);
				previousX = nextX;
				previousY = nextY;
			}
		}
		else
		{
			double firstX, firstY, secondX, secondY;
			Point(halfOutward, -halfTangent, firstX, firstY);
			Point(-halfOutward, -halfTangent, secondX, secondY);
			AddPerchEdge(firstX, firstY, secondX, secondY,
				perchSector, room.sectorIdx, true);
			Point(-halfOutward, halfTangent, firstX, firstY);
			AddPerchEdge(secondX, secondY, firstX, firstY,
				perchSector, room.sectorIdx, true);
			Point(halfOutward, halfTangent, secondX, secondY);
			AddPerchEdge(firstX, firstY, secondX, secondY,
				perchSector, room.sectorIdx, true);
		}

		// Lowest-to-highest 16-unit tiers. The balcony turns its lowest flight by
		// 90 degrees around the next landing; square and turret variants use straight
		// and offset flights. Only retaining sides block monsters, while every entry,
		// riser, landing, and platform connection remains traversable.
		const bool dogleg = variant == 2;
		for (int level = dogleg ? 1 : 0; level < stairCount; level++)
		{
			const double near = halfOutward + (stairCount - 1 - level) * stepDepth;
			const double far = near + stepDepth;
			double ax, ay, bx, by, cx2, cy2, dx, dy;
			Point(near, stairOffset + stairHalf, ax, ay);
			Point(far, stairOffset + stairHalf, bx, by);
			Point(far, stairOffset - stairHalf, cx2, cy2);
			Point(near, stairOffset - stairHalf, dx, dy);
			const bool turnLanding = dogleg && level == 1;
			AddPerchEdge(ax, ay, bx, by, stairSectors[level],
				turnLanding && doglegSign > 0.0 ? stairSectors[0] : room.sectorIdx,
				!(turnLanding && doglegSign > 0.0));
			AddPerchEdge(cx2, cy2, dx, dy, stairSectors[level],
				turnLanding && doglegSign < 0.0 ? stairSectors[0] : room.sectorIdx,
				!(turnLanding && doglegSign < 0.0));
			if (!dogleg && level == 0)
				AddPerchEdge(bx, by, cx2, cy2,
					stairSectors[level], room.sectorIdx, false);
			else if (turnLanding)
				AddPerchEdge(bx, by, cx2, cy2,
					stairSectors[level], room.sectorIdx, true);
			const int higherSector = level + 1 < stairCount ?
				stairSectors[level + 1] : perchSector;
			AddPerchEdge(dx, dy, ax, ay,
				stairSectors[level], higherSector, false);
		}
		if (dogleg)
		{
			// The lowest tread shares one long edge with the +32 landing and opens
			// to the room at its perpendicular end, forming a real L-shaped route.
			const double sharedNear = halfOutward + (stairCount - 2) * stepDepth;
			const double sharedFar = sharedNear + stepDepth;
			const double sharedTangent = doglegSign * stairHalf;
			const double outerTangent = sharedTangent + doglegSign * 64.0;
			double ax, ay, bx, by, cx2, cy2, dx, dy;
			if (doglegSign > 0.0)
			{
				Point(sharedNear, outerTangent, ax, ay);
				Point(sharedFar, outerTangent, bx, by);
				Point(sharedFar, sharedTangent, cx2, cy2);
				Point(sharedNear, sharedTangent, dx, dy);
			}
			else
			{
				Point(sharedFar, outerTangent, ax, ay);
				Point(sharedNear, outerTangent, bx, by);
				Point(sharedNear, sharedTangent, cx2, cy2);
				Point(sharedFar, sharedTangent, dx, dy);
			}
			AddPerchEdge(ax, ay, bx, by,
				stairSectors[0], room.sectorIdx, false);
			AddPerchEdge(bx, by, cx2, cy2,
				stairSectors[0], room.sectorIdx, true);
			AddPerchEdge(dx, dy, ax, ay,
				stairSectors[0], room.sectorIdx, true);
		}
		return perchSector;
	};

	auto AddLiftPlatform = [&](const RoomInfo& room, double cx, double cy,
		int liftTag, bool sky) -> int
	{
		const double half = LiftPlatformHalf;
		const double raisedFloor = room.floorZ + 32.0;
		const char* floor = themeStyle == ThemeIndustrial ? "FLOOR0_1" :
			(themeStyle == ThemeGothic ? "FLAT10" :
			(themeStyle == ThemeHell ? "FLAT5_1" :
			(themeStyle == ThemeCorrupted ? SafeTexture(room.floorTex, "FLAT5_1") : "FLAT20")));
		const char* liftWall = themeStyle == ThemeIndustrial ? "PLAT1" :
			(themeStyle == ThemeGothic ? "WOOD1" :
			(themeStyle == ThemeHell ? "MARBLE2" :
			(themeStyle == ThemeCorrupted ? SafeTexture(room.accentTex, "PLAT1") : "TEKWALL1")));
		const char* ceiling = sky ? "F_SKY1" : SafeTexture(room.ceilTex, "CEIL3_5");
		const int light = sky ? std::max(room.light + 16, 192) : room.light + 16;
		int liftSector = AddSector(raisedFloor, room.ceilZ, floor, ceiling,
			std::min(light, 224), liftTag);
		ApplyRoomLighting(liftSector, room, sky);
		auto AddLiftEdge = [&](double x1, double y1, double x2, double y2)
		{
			AddLine(x1, y1, x2, y2, liftSector, room.sectorIdx,
				liftWall, nullptr, liftWall, liftWall, nullptr, liftWall,
				false, 62, 0, liftTag, 16, 105, 0, 0,
				true, false, true, true, true, true);
		};
		// Clockwise, with the moving platform on the front/right side. Each face
		// can lower it, wait, and return; monster blocking keeps the mechanism from
		// being jammed while the surrounding 40+ unit route stays open.
		AddLiftEdge(cx - half, cy + half, cx + half, cy + half);
		AddLiftEdge(cx + half, cy + half, cx + half, cy - half);
		AddLiftEdge(cx + half, cy - half, cx - half, cy - half);
		AddLiftEdge(cx - half, cy - half, cx - half, cy + half);
		return liftSector;
	};

	auto AddFluidArchitecture = [&](const RoomInfo& room,
		const FluidDescriptor& descriptor, bool sky)
	{
		const int fluidKind = descriptor.kind;
		const bool hazardous = fluidKind == FluidNukage || fluidKind == FluidLava;
		const char* flat = fluidKind == FluidWater ? "FWATER1" :
			(fluidKind == FluidBlood ? "BLOOD1" :
				(fluidKind == FluidNukage ? "NUKAGE1" : "LAVA1"));
		const char* basinWall = SafeTexture(room.accentTex, "STEP1");
		const char* ceiling = sky ? "F_SKY1" : SafeTexture(room.ceilTex, "CEIL3_5");
		const double depth = hazardous ? 16.0 : 8.0;
		auto MakeLoop = [&](double centerX, double centerY, double halfX,
			double halfY, double chamfer) -> TArray<std::pair<double, double>>
		{
			TArray<std::pair<double, double>> points;
			points.Push(std::make_pair(centerX - halfX + chamfer, centerY + halfY));
			points.Push(std::make_pair(centerX + halfX - chamfer, centerY + halfY));
			points.Push(std::make_pair(centerX + halfX, centerY + halfY - chamfer));
			points.Push(std::make_pair(centerX + halfX, centerY - halfY + chamfer));
			points.Push(std::make_pair(centerX + halfX - chamfer, centerY - halfY));
			points.Push(std::make_pair(centerX - halfX + chamfer, centerY - halfY));
			points.Push(std::make_pair(centerX - halfX, centerY - halfY + chamfer));
			points.Push(std::make_pair(centerX - halfX, centerY + halfY - chamfer));
			return points;
		};
		auto AddFluidLoop = [&](const TArray<std::pair<double, double>>& points)
		{
			EmittedFluidFootprint footprint;
			for (const auto& point : points) footprint.points.Push(point);
			emittedFluidFootprints.Push(footprint);
			const int light = sky ? std::max(192, std::min(224,
				room.light + (hazardous ? 8 : 0))) :
				std::min(224, room.light + (hazardous ? 8 : 0));
			const int sectorIndex = AddSector(room.floorZ - depth, room.ceilZ,
				flat, ceiling, light);
			ApplyRoomLighting(sectorIndex, room, sky);
			BuildSector& sector = sectors[sectorIndex];
			if (fluidKind == FluidNukage)
			{
				sector.damageAmount = 5;
				sector.damageInterval = 32;
				sector.damageType = "Slime";
			}
			else if (fluidKind == FluidLava)
			{
				sector.damageAmount = 5;
				sector.damageInterval = 16;
				sector.leakiness = 256;
				sector.damageType = "Fire";
				sector.damageTerrainEffect = true;
			}
			for (unsigned int point = 0; point < points.Size(); point++)
			{
				const auto& first = points[point];
				const auto& second = points[(point + 1) % points.Size()];
				AddLine(first.first, first.second, second.first, second.second,
					sectorIndex, room.sectorIdx,
					basinWall, nullptr, basinWall,
					basinWall, nullptr, basinWall,
					false, 0, 0, 0, 0, 0, 0, 0,
					false, false, false, true, true);
			}
		};

		if (descriptor.cells.Size() == 0) return;
		if (descriptor.architecture <= FluidFloodedGrotto && descriptor.cells.Size() > 1)
		{
			int minCellX = descriptor.cells[0].first;
			int maxCellX = minCellX;
			int minCellY = descriptor.cells[0].second;
			int maxCellY = minCellY;
			for (const auto& cell : descriptor.cells)
			{
				minCellX = std::min(minCellX, cell.first);
				maxCellX = std::max(maxCellX, cell.first);
				minCellY = std::min(minCellY, cell.second);
				maxCellY = std::max(maxCellY, cell.second);
			}
			double leftMargin = 144.0;
			double rightMargin = 144.0;
			double bottomMargin = 144.0;
			double topMargin = 144.0;
			const bool horizontalRun = minCellY == maxCellY;
			const bool verticalRun = minCellX == maxCellX;
			// This is the dry bank, not the liquid half-width. The former code
			// subtracted a 112/128-unit "reserve" and accidentally left only a
			// 48-unit ribbon of liquid. A real 64-unit bypass now surrounds a
			// 192-224-unit-wide watercourse in major rooms.
			// Keep every bank eight units above the 64-unit circulation contract.
			// Major systems used to hold an additional 16 units, which made their
			// watercourses visually disappear across maximum-size districts.
			const double dryBypass = FluidBankClearance;
			for (const auto& cell : descriptor.cells)
			{
				if (cell.first == minCellX)
					leftMargin = std::min(leftMargin,
						EdgeForCell(cell.first, cell.second, DIR_W) - dryBypass);
				if (cell.first == maxCellX)
					rightMargin = std::min(rightMargin,
						EdgeForCell(cell.first, cell.second, DIR_E) - dryBypass);
				if (cell.second == minCellY)
					bottomMargin = std::min(bottomMargin,
						EdgeForCell(cell.first, cell.second, DIR_N) - dryBypass);
				if (cell.second == maxCellY)
					topMargin = std::min(topMargin,
						EdgeForCell(cell.first, cell.second, DIR_S) - dryBypass);
			}
			leftMargin = clamp(leftMargin, 24.0, 144.0);
			rightMargin = clamp(rightMargin, 24.0, 144.0);
			bottomMargin = clamp(bottomMargin, 24.0, 144.0);
			topMargin = clamp(topMargin, 24.0, 144.0);
			const double left = CellCenterX(minCellX) - leftMargin;
			const double right = CellCenterX(maxCellX) + rightMargin;
			const double bottom = CellCenterY(minCellY) - bottomMargin;
			const double top = CellCenterY(maxCellY) + topMargin;
			const double width = right - left;
			const double height = top - bottom;
			const double roughness = 8.0 + ((room.id + room.visualVariant) % 3) * 4.0;
			const double cornerInsetX = std::min(width * 0.38, room.cornerCut + 104.0);
			const double centerX = (left + right) * 0.5;
			const double centerY = (top + bottom) * 0.5;
			if (descriptor.bridge)
			{
				// Split one broad system at a dry 64-unit causeway. The aligned
				// banks and shared flat read as one river/reservoir continuing under
				// the crossing, while the bridge is ordinary room floor and remains
				// reachable from both dry banks without 3-D-floor dependencies.
				const bool horizontalSystem = horizontalRun || (!verticalRun && width >= height);
				const double bridgeOffset = ((room.id + room.visualVariant) & 1) ? 24.0 : -24.0;
				const double bridgeHalf = 32.0;
				if (horizontalSystem)
				{
					const double bridgeCenter = clamp(centerX + bridgeOffset,
						left + 96.0, right - 96.0);
					const double firstRight = bridgeCenter - bridgeHalf;
					const double secondLeft = bridgeCenter + bridgeHalf;
					AddFluidLoop(MakeLoop((left + firstRight) * 0.5, centerY,
						(firstRight - left) * 0.5, height * 0.5, 16.0));
					AddFluidLoop(MakeLoop((secondLeft + right) * 0.5, centerY,
						(right - secondLeft) * 0.5, height * 0.5, 20.0));
				}
				else
				{
					const double bridgeCenter = clamp(centerY + bridgeOffset,
						bottom + 96.0, top - 96.0);
					const double firstTop = bridgeCenter - bridgeHalf;
					const double secondBottom = bridgeCenter + bridgeHalf;
					AddFluidLoop(MakeLoop(centerX, (bottom + firstTop) * 0.5,
						width * 0.5, (firstTop - bottom) * 0.5, 16.0));
					AddFluidLoop(MakeLoop(centerX, (secondBottom + top) * 0.5,
						width * 0.5, (top - secondBottom) * 0.5, 20.0));
				}
				return;
			}
			TArray<std::pair<double, double>> points;
			// Broad rounded ends stay well clear of the host chamber's chamfered
			// corners; uneven intermediate banks keep the area from reading as a box.
			points.Push(std::make_pair(left + cornerInsetX, top - roughness));
			points.Push(std::make_pair(left + width * 0.46, top - roughness * 0.25));
			points.Push(std::make_pair(right - cornerInsetX, top - roughness * 1.05));
			points.Push(std::make_pair(right - roughness * 0.35, centerY + height * 0.10));
			points.Push(std::make_pair(right, centerY));
			points.Push(std::make_pair(right - roughness * 0.9, centerY - height * 0.12));
			points.Push(std::make_pair(right - cornerInsetX, bottom + roughness));
			points.Push(std::make_pair(left + width * 0.52, bottom + roughness * 0.35));
			points.Push(std::make_pair(left + cornerInsetX, bottom + roughness * 1.1));
			points.Push(std::make_pair(left + roughness * 0.4, centerY - height * 0.11));
			points.Push(std::make_pair(left, centerY));
			points.Push(std::make_pair(left + roughness, centerY + height * 0.13));
			AddFluidLoop(points);
			return;
		}

		const auto& hostCell = descriptor.cells[0];
		const double cx = CellCenterX(descriptor.cells[0].first);
		const double cy = CellCenterY(descriptor.cells[0].second);
		const double extentX = std::max(24.0, std::min(120.0,
			std::min(EdgeForCell(hostCell.first, hostCell.second, DIR_W),
				EdgeForCell(hostCell.first, hostCell.second, DIR_E)) - FluidBankClearance));
		const double extentY = std::max(24.0, std::min(120.0,
			std::min(EdgeForCell(hostCell.first, hostCell.second, DIR_N),
				EdgeForCell(hostCell.first, hostCell.second, DIR_S)) - FluidBankClearance));
		if (descriptor.architecture == FluidCentralPool)
		{
			AddFluidLoop(MakeLoop(cx, cy, extentX, extentY,
				std::min(24.0, std::min(extentX, extentY) * 0.22)));
		}
		else if (descriptor.architecture == FluidTrenchPool)
		{
			const bool horizontal = ((room.visualVariant + room.id) & 1) == 0;
			const double minor = std::max(28.0,
				std::min(56.0, (horizontal ? extentY : extentX) * 0.58));
			AddFluidLoop(MakeLoop(cx, cy, horizontal ? extentX : minor,
				horizontal ? minor : extentY, 10.0));
		}
		else if (descriptor.architecture == FluidPairedPools)
		{
			const bool horizontal = ((room.visualVariant + room.id) & 1) == 0;
			const double longExtent = horizontal ? extentX : extentY;
			const double crossExtent = horizontal ? extentY : extentX;
			const double offset = longExtent * 0.48;
			for (int side : { -1, 1 })
			{
				const double poolX = cx + (horizontal ? side * offset : 0.0);
				const double poolY = cy + (horizontal ? 0.0 : side * offset);
				AddFluidLoop(MakeLoop(poolX, poolY,
					horizontal ? longExtent * 0.38 : crossExtent * 0.62,
					horizontal ? crossExtent * 0.62 : longExtent * 0.38, 10.0));
			}
		}
		else if (descriptor.architecture == FluidIrregularPool ||
			descriptor.architecture == FluidFloodedGrotto)
		{
			TArray<std::pair<double, double>> points;
			points.Push(std::make_pair(cx - extentX, cy + extentY * 0.30));
			points.Push(std::make_pair(cx - extentX * 0.62, cy + extentY));
			points.Push(std::make_pair(cx + extentX * 0.22, cy + extentY * 0.86));
			points.Push(std::make_pair(cx + extentX, cy + extentY * 0.42));
			points.Push(std::make_pair(cx + extentX * 0.82, cy - extentY * 0.48));
			points.Push(std::make_pair(cx + extentX * 0.38, cy - extentY));
			points.Push(std::make_pair(cx - extentX * 0.30, cy - extentY * 0.82));
			points.Push(std::make_pair(cx - extentX * 0.92, cy - extentY * 0.26));
			AddFluidLoop(points);
		}
		else
		{
			if (descriptor.cells.Size() == 1)
			{
				const bool horizontal = ((room.visualVariant + room.id) & 1) == 0;
				const double alongExtent = horizontal ? extentX : extentY;
				const double crossExtent = horizontal ? extentY : extentX;
				const double normalX = horizontal ? 0.0 : -1.0;
				const double normalY = horizontal ? 1.0 : 0.0;
				if (descriptor.architecture == FluidStraightRiver)
				{
					const double half = std::max(16.0, std::min(48.0, crossExtent * 0.62));
					static const double Offset[] = { -6.0, 8.0, -10.0, 6.0, 0.0 };
					TArray<std::pair<double, double>> centers;
					for (int index = 0; index < 5; index++)
					{
						const double along = -alongExtent + alongExtent * 2.0 * index / 4.0;
						centers.Push(std::make_pair(horizontal ? cx + along : cx + Offset[index],
							horizontal ? cy + Offset[index] : cy + along));
					}
					TArray<std::pair<double, double>> points;
					for (unsigned int index = 0; index < centers.Size(); index++)
					{
						const double width = std::min(crossExtent,
							half + ((int(index) % 3) - 1) * 6.0);
						points.Push(std::make_pair(centers[index].first + normalX * width,
							centers[index].second + normalY * width));
					}
					for (int index = (int)centers.Size() - 1; index >= 0; index--)
					{
						const double width = std::min(crossExtent,
							half + ((index + 1) % 3 - 1) * 6.0);
						points.Push(std::make_pair(centers[index].first - normalX * width,
							centers[index].second - normalY * width));
					}
					AddFluidLoop(points);
					return;
				}
				if (descriptor.architecture == FluidStaggeredRiver)
				{
					TArray<std::pair<double, double>> centers;
					const double half = std::max(14.0, std::min(40.0, crossExtent * 0.46));
					const double offsetLimit = std::max(0.0, crossExtent - half - 4.0);
					static const double Offset[] = { -24.0, 32.0, -36.0, 28.0, -18.0, 22.0 };
					for (int index = 0; index < 6; index++)
					{
						const double along = -alongExtent + alongExtent * 2.0 * index / 5.0;
						const double offset = clamp(Offset[index], -offsetLimit, offsetLimit);
						centers.Push(std::make_pair(horizontal ? cx + along : cx + offset,
							horizontal ? cy + offset : cy + along));
					}
					TArray<std::pair<double, double>> points;
					for (unsigned int index = 0; index < centers.Size(); index++)
					{
						const double width = half + ((int(index) & 1) ? 5.0 : -3.0);
						points.Push(std::make_pair(centers[index].first + normalX * width,
							centers[index].second + normalY * width));
					}
					for (int index = (int)centers.Size() - 1; index >= 0; index--)
					{
						const double width = half + ((index & 1) ? -3.0 : 5.0);
						points.Push(std::make_pair(centers[index].first - normalX * width,
							centers[index].second - normalY * width));
					}
					AddFluidLoop(points);
					return;
				}

				// A broad right-angle blood/water course provides a compact river bend.
				// Rotate it by 180 degrees on alternate rooms while preserving winding.
				const double sign = ((room.visualVariant + room.id) & 2) ? -1.0 : 1.0;
				const double half = std::max(14.0,
					std::min(40.0, std::min(extentX, extentY) * 0.42));
				const double startX = -extentX;
				const double turnX = extentX * 0.35;
				const double runY = -extentY * 0.35;
				const double endY = extentY;
				TArray<std::pair<double, double>> points;
				auto PushLocal = [&](double x, double y)
				{
					points.Push(std::make_pair(cx + sign * x, cy + sign * y));
				};
				PushLocal(startX, runY + half);
				PushLocal(turnX - half, runY + half);
				PushLocal(turnX - half, endY);
				PushLocal(turnX + half, endY);
				PushLocal(turnX + half, runY - half);
				PushLocal(startX, runY - half);
				AddFluidLoop(points);
				return;
			}

			auto DirectionBetween = [&](const std::pair<int, int>& first,
				const std::pair<int, int>& second) -> int
			{
				if (second.first > first.first) return DIR_E;
				if (second.first < first.first) return DIR_W;
				if (second.second > first.second) return DIR_S;
				return DIR_N;
			};
			auto DryEndExtension = [&](const std::pair<int, int>& cell,
				int direction) -> double
			{
				return std::max(16.0, std::min(128.0,
					EdgeForCell(cell.first, cell.second, direction) - FluidBankClearance));
			};
			TArray<std::pair<double, double>> points;
			const auto& firstCell = descriptor.cells[0];
			const auto& lastCell = descriptor.cells.Last();
			const int firstDirection = DirectionBetween(firstCell, descriptor.cells[1]);
			const int lastDirection = DirectionBetween(
				descriptor.cells[descriptor.cells.Size() - 2], lastCell);
			const double startExtension = DryEndExtension(firstCell, OPP[firstDirection]);
			const double endExtension = DryEndExtension(lastCell, lastDirection);

			double availableHalf = 120.0;
			for (const auto& cell : descriptor.cells)
				availableHalf = std::min(availableHalf,
					FluidCellClearance(cell.first, cell.second) - FluidBankClearance);
			availableHalf = std::max(24.0, availableHalf);
			const bool bending = descriptor.architecture == FluidBendRiver;
			const double baseHalf = std::min(112.0,
				std::max(24.0, availableHalf * (bending ? 0.78 : 0.90)));
			const double offsetLimit = std::max(0.0, availableHalf - baseHalf - 4.0);
			const double firstNormalX = -DY[firstDirection];
			const double firstNormalY = DX[firstDirection];
			static const double RiverOffset[] = { -24.0, 32.0, -36.0, 28.0, -20.0, 24.0 };
			TArray<std::pair<double, double>> centers;
			TArray<double> offsets;
			for (unsigned int index = 0; index < descriptor.cells.Size(); index++)
			{
				const auto& cell = descriptor.cells[index];
				double offset = 0.0;
				if (!bending)
				{
					const double requested = descriptor.architecture == FluidStaggeredRiver ?
						RiverOffset[index % countof(RiverOffset)] :
						RiverOffset[(index + 2) % countof(RiverOffset)] * 0.28;
					offset = clamp(requested, -offsetLimit, offsetLimit);
				}
				double x = CellCenterX(cell.first) + firstNormalX * offset;
				double y = CellCenterY(cell.second) + firstNormalY * offset;
				if (index == 0)
				{
					x -= DX[firstDirection] * startExtension;
					y -= DY[firstDirection] * startExtension;
				}
				if (index + 1 == descriptor.cells.Size())
				{
					x += DX[lastDirection] * endExtension;
					y += DY[lastDirection] * endExtension;
				}
				centers.Push(std::make_pair(x, y));
				offsets.Push(offset);
			}
			// A two-cell course still needs enough bank articulation to read as a
			// natural channel rather than a four-sided strip. Interpolate a small
			// deterministic meander without approaching the protected dry banks.
			if (descriptor.cells.Size() == 2 && !bending)
			{
				const double meander = std::min(12.0,
					std::max(0.0, availableHalf - baseHalf - 6.0));
				static const double Bend[] = { 0.0, 1.0, -0.8, 0.6, 0.0 };
				for (int index = 0; index < 5; index++)
				{
					const double amount = index / 4.0;
					const double x = centers[0].first +
						(centers[1].first - centers[0].first) * amount;
					const double y = centers[0].second +
						(centers[1].second - centers[0].second) * amount;
					const double width = std::min(availableHalf - fabs(Bend[index] * meander),
						baseHalf + ((index % 3) - 1) * 4.0);
					points.Push(std::make_pair(x + firstNormalX * (Bend[index] * meander + width),
						y + firstNormalY * (Bend[index] * meander + width)));
				}
				for (int index = 4; index >= 0; index--)
				{
					const double amount = index / 4.0;
					const double x = centers[0].first +
						(centers[1].first - centers[0].first) * amount;
					const double y = centers[0].second +
						(centers[1].second - centers[0].second) * amount;
					const double width = std::min(availableHalf - fabs(Bend[index] * meander),
						baseHalf + (((index + 1) % 3) - 1) * 4.0);
					points.Push(std::make_pair(x + firstNormalX * (Bend[index] * meander - width),
						y + firstNormalY * (Bend[index] * meander - width)));
				}
				AddFluidLoop(points);
				return;
			}
			auto BankVector = [&](int index, double& nx, double& ny)
			{
				const int incoming = index == 0 ? firstDirection :
					DirectionBetween(descriptor.cells[index - 1], descriptor.cells[index]);
				const int outgoing = index + 1 == (int)descriptor.cells.Size() ? lastDirection :
					DirectionBetween(descriptor.cells[index], descriptor.cells[index + 1]);
				nx = -DY[incoming];
				ny = DX[incoming];
				if (incoming != outgoing)
				{
					nx += -DY[outgoing];
					ny += DX[outgoing];
				}
			};
			for (unsigned int index = 0; index < centers.Size(); index++)
			{
				double nx, ny;
				BankVector(index, nx, ny);
				const double width = std::max(12.0,
					std::min(availableHalf - fabs(offsets[index]),
						baseHalf + ((int(index) % 3) - 1) * 6.0));
				points.Push(std::make_pair(centers[index].first + nx * width,
					centers[index].second + ny * width));
			}
			for (int index = (int)centers.Size() - 1; index >= 0; index--)
			{
				double nx, ny;
				BankVector(index, nx, ny);
				const double width = std::max(12.0,
					std::min(availableHalf - fabs(offsets[index]),
						baseHalf + ((index + 1) % 3 - 1) * 6.0));
				points.Push(std::make_pair(centers[index].first - nx * width,
					centers[index].second - ny * width));
			}
			AddFluidLoop(points);
		}
	};

	auto AddFloodedIsland = [&](const RoomInfo& room, double cx, double cy,
		double halfX, double halfY, bool sky) -> int
	{
		const double cut = std::min(16.0, std::min(halfX, halfY) - 48.0);
		const char* ceiling = sky ? "F_SKY1" : SafeTexture(room.ceilTex, "CEIL3_5");
		const int islandSector = AddSector(room.floorZ, room.ceilZ,
			SafeTexture(room.floorTex, "FLOOR4_8"), ceiling,
			sky ? std::max(192, room.light) : room.light + 8);
		ApplyRoomLighting(islandSector, room, sky);
		const std::pair<double, double> points[] = {
			{ cx - halfX + cut, cy + halfY }, { cx + halfX - cut, cy + halfY },
			{ cx + halfX, cy + halfY - cut }, { cx + halfX, cy - halfY + cut },
			{ cx + halfX - cut, cy - halfY }, { cx - halfX + cut, cy - halfY },
			{ cx - halfX, cy - halfY + cut }, { cx - halfX, cy + halfY - cut },
		};
		for (unsigned int point = 0; point < countof(points); point++)
		{
			const auto& first = points[point];
			const auto& second = points[(point + 1) % countof(points)];
			AddLine(first.first, first.second, second.first, second.second,
				islandSector, room.sectorIdx,
				SafeTexture(room.accentTex, "STEP1"), nullptr,
				SafeTexture(room.accentTex, "STEP1"),
				SafeTexture(room.accentTex, "STEP1"), nullptr,
				SafeTexture(room.accentTex, "STEP1"),
				false, 0, 0, 0, 0, 0, 0, 0,
				false, false, false, true, true);
		}
		return islandSector;
	};

	auto AddLandmarkPlatform = [&](const RoomInfo& room, int anchorCellX,
		int anchorCellY, double cx, double cy, bool sky) -> int
	{
		// A landmark is a circulation-first room form, not a prop pile.  Each
		// archetype keeps a wide, collision-clear ring around its raised center;
		// the reservation is installed before actors, rewards, or decoration are
		// considered below.  Small rooms simply shrink to the compact rectangle
		// fallback instead of losing a route through an over-large silhouette.
		EProcGenLandmarkArchetype archetype =
			(EProcGenLandmarkArchetype)room.landmarkArchetype;
		if (archetype == PGLA_None)
		{
			archetype = room.hasExit ? PGLA_Fortress :
				(room.hasKey ? PGLA_ShrineTerrace :
				(room.isArena ? PGLA_Bastion :
				(room.isHub ? PGLA_Court : PGLA_Court)));
		}

		double halfX = room.isArena ? 80.0 : 64.0;
		double halfY = halfX;
		double raise = room.isArena ? 16.0 : 8.0;
		const bool longNorthSouth = room.shapeFamily == 2 ||
			(room.shapeFamily == 3 && ((room.id + room.progressionRank) & 1));
		switch (archetype)
		{
		case PGLA_Nave:
			halfX = longNorthSouth ? 56.0 : 124.0;
			halfY = longNorthSouth ? 124.0 : 56.0;
			raise = 8.0;
			break;
		case PGLA_Gatehouse:
			halfX = longNorthSouth ? 104.0 : 60.0;
			halfY = longNorthSouth ? 60.0 : 104.0;
			raise = 8.0;
			break;
		case PGLA_ShrineTerrace:
			halfX = halfY = 60.0;
			raise = 16.0;
			break;
		case PGLA_BridgeBasin:
			halfX = longNorthSouth ? 48.0 : 132.0;
			halfY = longNorthSouth ? 132.0 : 48.0;
			raise = 8.0;
			break;
		case PGLA_Bastion:
			halfX = halfY = 96.0;
			raise = 16.0;
			break;
		case PGLA_Fortress:
			halfX = halfY = 112.0;
			raise = 16.0;
			break;
		case PGLA_Court:
		default:
			halfX = halfY = room.isHub ? 88.0 : 72.0;
			raise = room.isHub ? 16.0 : 8.0;
			break;
		}
		if (room.hasKey) { halfX = halfY = 64.0; raise = 16.0; }
		if (room.hasPlayerStart) { halfX = halfY = 64.0; raise = 8.0; }
		if (room.hasExit) { halfX = halfY = 96.0; raise = 16.0; }
		// Room bounds describe the composed footprint, but a landmark is emitted
		// around one particular anchor cell.  An L-shaped composed room can put
		// that anchor beside a short exterior face even though the aggregate room
		// is broad.  Limit the platform to the *actual* four cell edges so its
		// outer tier cannot coincide with a chamber wall or connector.  Coincident
		// loops otherwise make an apparently valid apron unclosed to the node
		// builder (and are especially easy to hit in outdoor fortress districts).
		const double localOuterHalfX = std::min(
			EdgeForCell(anchorCellX, anchorCellY, DIR_W),
			EdgeForCell(anchorCellX, anchorCellY, DIR_E)) - 24.0;
		const double localOuterHalfY = std::min(
			EdgeForCell(anchorCellX, anchorCellY, DIR_N),
			EdgeForCell(anchorCellX, anchorCellY, DIR_S)) - 24.0;
		if (localOuterHalfX < 64.0 || localOuterHalfY < 64.0)
			return -1; // Deterministic compact fallback: retain the clear room.
		const double safeHalfX = std::max(40.0, std::min(
			room.halfWidth - room.cornerCut - 40.0, localOuterHalfX - 24.0));
		const double safeHalfY = std::max(40.0, std::min(
			room.halfHeight - room.cornerCut - 40.0, localOuterHalfY - 24.0));
		halfX = std::min(halfX, safeHalfX);
		halfY = std::min(halfY, safeHalfY);
		const double outerHalfX = std::min(halfX + 24.0, localOuterHalfX);
		const double outerHalfY = std::min(halfY + 24.0, localOuterHalfY);
		// The platform's outer lip needs a 24-unit circulation band. The actual
		// clearance check adds the prop radius, player radius, and safety margin,
		// leaving its corners available for a sparse landmark beacon without
		// crowding the key/start/exit pads (which reserve their own larger discs).
		ReserveNavigationPad(cx, cy, std::max(outerHalfX, outerHalfY) + 24.0);
		const bool techbase = themeStyle == ThemeTechbase;
		const bool hell = themeStyle == ThemeHell;
		const bool gothic = themeStyle == ThemeGothic;
		const bool industrial = themeStyle == ThemeIndustrial;
		const bool corrupted = themeStyle == ThemeCorrupted;
		const char* floor = industrial ? "FLOOR0_1" :
			(gothic ? "FLAT10" : (hell ? "FLOOR7_2" :
			(corrupted ? SafeTexture(room.floorTex, "FLOOR7_2") : "FLAT20")));
		if (room.hasKey) floor = (hell || gothic) ? "FLAT5_1" :
			(industrial ? "FLAT20" : "FLOOR0_1");
		else if (room.hasPlayerStart) floor = (hell || gothic) ? "FLOOR6_1" :
			(industrial ? "FLOOR0_1" : "FLOOR5_1");
		else if (room.hasExit) floor = "GATE1";
		const char* ceiling = sky ? "F_SKY1" : SafeTexture(room.ceilTex, "CEIL3_5");
		// Keep STEP1 on the load-bearing outline: the passage/clearance audit has
		// an explicit, conservative treatment for that trim. Theme identity lives
		// in the inset tiers below, which are all inside the already-reserved
		// landmark footprint and never form a collision obstacle.
		const char* step = room.hasExit ? "EXITDOOR" : "STEP1";
		const char* detailFloor = techbase ? "FLOOR5_1" :
			(industrial ? "FLOOR0_1" : (hell ? "FLAT5_2" :
			(gothic ? "FLOOR7_2" : (corrupted ? "FLAT5_2" : "FLAT20"))));
		const char* detailWall = techbase ? "TEKWALL1" :
			(industrial ? "SUPPORT3" : (hell ? "MARBLE2" :
			(gothic ? "WOOD1" :
			(corrupted ? SafeTexture(room.accentTex, "GSTVINE1") : "SUPPORT2"))));
		double featureCeil = room.ceilZ;
		if (!sky && room.isHub && !room.hasPlayerStart)
			featureCeil = std::max(room.floorZ + raise + 80.0, room.ceilZ - 16.0);
		int platformLight = room.hasExit ? 224 :
			(sky ? std::max(room.light + 8, 192) : room.light + 8);
		const bool tiered = raise >= 16.0;
		int outerSector = room.sectorIdx;
		auto AddRectangleLoopAt = [&](double loopX, double loopY,
			double hx, double hy, int frontSector, int backSector,
			const char* texture)
		{
			AddLine(loopX - hx, loopY + hy, loopX + hx, loopY + hy, frontSector, backSector,
				texture, nullptr, texture, texture, nullptr, texture,
				false, 0, 0, 0, 0, 0, 0, 0,
				false, false, false, true, true);
			AddLine(loopX + hx, loopY + hy, loopX + hx, loopY - hy, frontSector, backSector,
				texture, nullptr, texture, texture, nullptr, texture,
				false, 0, 0, 0, 0, 0, 0, 0,
				false, false, false, true, true);
			AddLine(loopX + hx, loopY - hy, loopX - hx, loopY - hy, frontSector, backSector,
				texture, nullptr, texture, texture, nullptr, texture,
				false, 0, 0, 0, 0, 0, 0, 0,
				false, false, false, true, true);
			AddLine(loopX - hx, loopY - hy, loopX - hx, loopY + hy, frontSector, backSector,
				texture, nullptr, texture, texture, nullptr, texture,
				false, 0, 0, 0, 0, 0, 0, 0,
				false, false, false, true, true);
		};
		auto AddOctagonLoopAt = [&](double loopX, double loopY,
			double hx, double hy, double cut, int frontSector, int backSector,
			const char* texture)
		{
			cut = std::min(cut, std::min(hx, hy) - 8.0);
			if (cut < 8.0)
			{
				AddRectangleLoopAt(loopX, loopY, hx, hy, frontSector, backSector, texture);
				return;
			}
			const std::pair<double, double> points[] = {
				{ loopX - hx + cut, loopY + hy },
				{ loopX + hx - cut, loopY + hy },
				{ loopX + hx, loopY + hy - cut },
				{ loopX + hx, loopY - hy + cut },
				{ loopX + hx - cut, loopY - hy },
				{ loopX - hx + cut, loopY - hy },
				{ loopX - hx, loopY - hy + cut },
				{ loopX - hx, loopY + hy - cut },
			};
			for (unsigned int point = 0; point < countof(points); point++)
			{
				const auto& first = points[point];
				const auto& second = points[(point + 1) % countof(points)];
				AddLine(first.first, first.second, second.first, second.second,
					frontSector, backSector,
					texture, nullptr, texture, texture, nullptr, texture,
					false, 0, 0, 0, 0, 0, 0, 0,
					false, false, false, true, true);
			}
		};
		auto AddOutlineAt = [&](double loopX, double loopY,
			double hx, double hy, int frontSector, int backSector,
			const char* texture, bool octagonal)
		{
			if (octagonal)
				AddOctagonLoopAt(loopX, loopY, hx, hy,
					std::min(24.0, std::min(hx, hy) * 0.28),
					frontSector, backSector, texture);
			else
				AddRectangleLoopAt(loopX, loopY, hx, hy, frontSector, backSector, texture);
		};
		// The terminal exit tier remains a four-sided platform. Its walkover line
		// and the mandatory two-step descent intentionally keep the old, plainly
		// auditable perimeter; fortress identity still comes from its scale, light,
		// floor, and surrounding district rather than a decorative edge change.
		const bool mainOctagonal = !room.hasExit &&
			(archetype == PGLA_Fortress || archetype == PGLA_Bastion ||
				(hell && archetype == PGLA_ShrineTerrace) ||
				(gothic && archetype == PGLA_Court) ||
				(corrupted && archetype == PGLA_ShrineTerrace));
		const bool outerOctagonal = mainOctagonal && room.spatialClass >= 2;
		if (tiered)
		{
			const int outerLight = sky ? std::max(platformLight - 8, 192) :
				std::max(room.light, platformLight - 8);
			outerSector = AddSector(room.floorZ + 8.0, featureCeil,
				floor, ceiling, outerLight);
			ApplyRoomLighting(outerSector, room, sky);
			AddOutlineAt(cx, cy, outerHalfX, outerHalfY, outerSector,
				room.sectorIdx, "STEP1", outerOctagonal);
		}
		int platformSector = AddSector(room.floorZ + raise, featureCeil,
			floor, ceiling, platformLight);
		ApplyRoomLighting(platformSector, room, sky);

		// Clockwise: the raised sector is always on the front/right side. Major
		// landmarks use two eight-unit tiers, providing readable Doom-scale
		// stairs rather than a curb.  The outline proportions establish the main
		// silhouette, while the inner grammar below distinguishes an airlock from
		// a chapel or a containment terrace without spending circulation space on
		// a blocking prop.
		AddOutlineAt(cx, cy, halfX, halfY, platformSector, outerSector, step,
			mainOctagonal);

		// All detail sectors remain inside the platform's existing reservation and
		// differ by eight units at most. They are deliberately omitted around a
		// start, key, or exit pad: those anchors retain an uninterrupted, level
		// interaction surface even when their enclosing landmark is ornate.
		const bool protectedPad = room.hasPlayerStart || room.hasKey || room.hasExit;
		auto AddInnerFeature = [&](double featureX, double featureY,
			double featureHalfX, double featureHalfY, double featureFloor,
			bool octagonal, int lightOffset)
		{
			featureHalfX = std::min(featureHalfX, halfX - 12.0);
			featureHalfY = std::min(featureHalfY, halfY - 12.0);
			if (featureHalfX < 20.0 || featureHalfY < 20.0 ||
				featureFloor > featureCeil - 56.0)
				return;
			// Outdoor landmark details inherit the sky readability floor. A dim
			// reactor well or ritual pit can be moody, but must not create a dark
			// F_SKY1 sector inside an otherwise bright courtyard.
			const int detailLight = sky ? std::max(192, platformLight + lightOffset) :
				platformLight + lightOffset;
			const int detailSector = AddSector(featureFloor, featureCeil,
				detailFloor, ceiling, detailLight);
			ApplyRoomLighting(detailSector, room, sky);
			AddOutlineAt(featureX, featureY, featureHalfX, featureHalfY,
				detailSector, platformSector, detailWall, octagonal);
		};
		const double loweredDetail = room.floorZ + std::max(0.0, raise - 8.0);
		const double raisedDetail = room.floorZ + raise + 8.0;
		auto AddCentralWell = [&]()
		{
			AddInnerFeature(cx, cy, std::min(56.0, halfX - 20.0),
				std::min(56.0, halfY - 20.0), loweredDetail, true, -4);
		};
		auto AddCentralDais = [&]()
		{
			AddInnerFeature(cx, cy, std::min(40.0, halfX - 20.0),
				std::min(40.0, halfY - 20.0), raisedDetail, true, 8);
		};
		auto AddAirlockBands = [&]()
		{
			const double along = longNorthSouth ? halfY : halfX;
			const double across = longNorthSouth ? halfX : halfY;
			if (along < 56.0 || across < 44.0) return;
			const double bandAlong = 12.0;
			const double bandAcross = std::min(40.0, across - 16.0);
			const double offset = along * 0.42;
			for (int side : { -1, 1 })
			{
				if (longNorthSouth)
					AddInnerFeature(cx, cy + side * offset, bandAcross, bandAlong,
						raisedDetail, false, 6);
				else
					AddInnerFeature(cx + side * offset, cy, bandAlong, bandAcross,
						raisedDetail, false, 6);
			}
		};
		auto AddSwitchbackTerraces = [&]()
		{
			const double along = longNorthSouth ? halfY : halfX;
			const double across = longNorthSouth ? halfX : halfY;
			if (along < 72.0 || across < 56.0) return;
			const double terraceAlong = std::min(32.0, along * 0.20);
			const double terraceAcross = std::min(24.0, across * 0.24);
			const double alongOffset = along * 0.38;
			const double acrossOffset = across * 0.30;
			if (longNorthSouth)
			{
				AddInnerFeature(cx - acrossOffset, cy + alongOffset,
					terraceAcross, terraceAlong, raisedDetail, false, 6);
				AddInnerFeature(cx + acrossOffset, cy - alongOffset,
					terraceAcross, terraceAlong, raisedDetail, false, 6);
			}
			else
			{
				AddInnerFeature(cx + alongOffset, cy + acrossOffset,
					terraceAlong, terraceAcross, raisedDetail, false, 6);
				AddInnerFeature(cx - alongOffset, cy - acrossOffset,
					terraceAlong, terraceAcross, raisedDetail, false, 6);
			}
		};
		auto AddChasmSides = [&]()
		{
			const double along = longNorthSouth ? halfY : halfX;
			const double across = longNorthSouth ? halfX : halfY;
			if (along < 64.0 || across < 60.0) return;
			const double stripAcross = std::min(18.0, across * 0.22);
			const double stripAlong = along - 18.0;
			const double offset = across * 0.58;
			for (int side : { -1, 1 })
			{
				if (longNorthSouth)
					AddInnerFeature(cx + side * offset, cy, stripAcross, stripAlong,
						loweredDetail, false, -6);
				else
					AddInnerFeature(cx, cy + side * offset, stripAlong, stripAcross,
						loweredDetail, false, -6);
			}
		};

		if (!protectedPad)
		{
			if (techbase)
			{
				// Airlocks bracket a route with two bright threshold strips; command
				// courts leave a low reactor well at their center, and defensive rooms
				// turn that well into a compact reactor sanctum.
				if (archetype == PGLA_Gatehouse || archetype == PGLA_Nave)
					AddAirlockBands();
				else if (archetype == PGLA_Court || archetype == PGLA_Bastion ||
					archetype == PGLA_Fortress)
					AddCentralWell();
				else
					AddCentralDais();
			}
			else if (industrial)
			{
				// Loading bays are broad and level; refinery rooms place alternating
				// terraces along their long axis, a readable static switchback. Bastions
				// sink to a cooled foundry basin rather than becoming another court.
				if (archetype == PGLA_Nave || archetype == PGLA_BridgeBasin)
					AddSwitchbackTerraces();
				else if (archetype == PGLA_Bastion || archetype == PGLA_Fortress)
					AddCentralWell();
				else if (archetype == PGLA_Gatehouse || archetype == PGLA_Court)
					AddAirlockBands();
				else
					AddCentralDais();
			}
			else if (hell)
			{
				// Blood chapels put a raised altar on the long axis; bridge basins keep
				// a dry central causeway between two shallow chasm strips; all other
				// sanctums resolve into a non-damaging ritual pit.
				if (archetype == PGLA_BridgeBasin)
					AddChasmSides();
				else if (archetype == PGLA_Nave || archetype == PGLA_Gatehouse)
					AddCentralDais();
				else
					AddCentralWell();
			}
			else if (gothic)
			{
				// Gatehouse bands read as a portcullis threshold; a nave's paired
				// terraces preserve a central aisle, while courts become cloisters with
				// a lower open center. Fortress tiers remain a throne court, while an
				// exit pad keeps its required perimeter deliberately rectangular.
				if (archetype == PGLA_Gatehouse)
					AddAirlockBands();
				else if (archetype == PGLA_Nave || archetype == PGLA_BridgeBasin)
					AddSwitchbackTerraces();
				else if (archetype == PGLA_Court || archetype == PGLA_ShrineTerrace)
					AddCentralWell();
				else
					AddCentralDais();
			}
			else if (corrupted)
			{
				// Containment halls repeat ordered threshold plates, breach terraces
				// break their rhythm diagonally, and bastions/fortresses expose a low
				// hell-core. None of these rely on scripts, hazards, or live spawning.
				if (archetype == PGLA_Gatehouse || archetype == PGLA_Nave)
					AddAirlockBands();
				else if (archetype == PGLA_Court || archetype == PGLA_ShrineTerrace ||
					archetype == PGLA_BridgeBasin)
					AddSwitchbackTerraces();
				else
					AddCentralWell();
			}
		}
		return platformSector;
	};

	// These two Hell signatures are a visual contract, but their normal actors
	// are solid. Track successful placement across rooms so an exit or outdoor
	// landmark with every collision-safe bay occupied can use an explicitly
	// non-solid infernal fallback instead of weakening its navigation proof.
	bool hellFinaleMarkerPlaced = false;
	bool hellOutdoorMarkerPlaced = false;

	for (unsigned int ri = 0; ri < Rooms.Size(); ri++)
	{
		RoomInfo& room = Rooms[ri];
		if (room.id < 0 || room.sectorIdx < 0) continue;

		TArray<std::pair<int, int>> roomCells;
		int playerCell = -1;
		int keyCell = -1;
		int exitCell = -1;
		for (int y = room.minJ; y <= room.maxJ; y++)
		{
			for (int x = room.minI; x <= room.maxI; x++)
			{
				if (x < 0 || x >= W || y < 0 || y >= H || Grid[y][x].roomId != (int)ri) continue;
				roomCells.Push(std::make_pair(x, y));
				int index = roomCells.Size() - 1;
				if (Grid[y][x].hasPlayerStart) playerCell = index;
				if (Grid[y][x].hasKey) keyCell = index;
				if (Grid[y][x].hasExit) exitCell = index;
			}
		}
		if (roomCells.Size() == 0) continue;
		int landmarkCell = 0;
			if (room.hasExit && exitCell >= 0) landmarkCell = exitCell;
			else if (room.hasKey && keyCell >= 0) landmarkCell = keyCell;
			else if (room.hasPlayerStart && playerCell >= 0) landmarkCell = playerCell;
			const FluidDescriptor& roomFluid = fluidDescriptors[ri];
			const bool landmarkRole = room.landmarkArchetype != PGLA_None ||
				room.isArena || room.isHub || room.hasPlayerStart || room.hasKey || room.hasExit;
			const bool scenicVerticalLandmark = room.verticalAnchor &&
				(room.verticalIntent == PGVI_TerraceOverlook ||
				 room.verticalIntent == PGVI_BridgeApproach);
			const bool landmarkHasSpace = room.cellCount >= 2 || room.hasKey || room.hasExit ||
				(room.landmarkArchetype != PGLA_None && room.spatialClass >= 2) ||
				scenicVerticalLandmark;
			// A flooded room and its dry chamfered island already form a strong
			// landmark. Do not overlap the ordinary concentric hub platform with that
			// island: overlapping sector loops are ambiguous to the node builder and
			// would put otherwise dry actors back in the liquid sector.
			const bool willHaveLandmark = landmarkRole && landmarkHasSpace &&
				!room.isLocked && !room.isSecret && !roomFluid.floodedRoom;
		int revealCell = -1;
		int perchCell = -1;
		int liftCell = -1;
		int secretAnnexCell = -1;
		int floodedIslandCell = -1;
			TArray<int> placementCells;
		for (unsigned int index = 0; index < roomCells.Size(); index++)
		{
			const int x = roomCells[index].first;
			const int y = roomCells[index].second;
			if (x == revealCellX[ri] && y == revealCellY[ri]) revealCell = index;
			if (x == perchCellX[ri] && y == perchCellY[ri]) perchCell = index;
			if (x == liftCellX[ri] && y == liftCellY[ri]) liftCell = index;
			if (x == secretAnnexCellX[ri] && y == secretAnnexCellY[ri])
				secretAnnexCell = index;
			if (roomFluid.floodedRoom && x == roomFluid.islandX && y == roomFluid.islandY)
				floodedIslandCell = index;
		}
		if (roomFluid.floodedRoom && floodedIslandCell >= 0)
			placementCells.Push(floodedIslandCell);
		else
		{
			for (unsigned int index = 0; index < roomCells.Size(); index++)
				if ((int)index != revealCell && (int)index != perchCell &&
					(int)index != liftCell && (int)index != secretAnnexCell &&
					!fluidCellReserved[roomCells[index].second * W + roomCells[index].first] &&
						(!willHaveLandmark || roomCells.Size() == 1 || (int)index != landmarkCell))
						placementCells.Push(index);
			}
			if (placementCells.Size() == 0 && !roomFluid.floodedRoom)
			{
				// A compact hub can spend one cell on a perch/reveal and the other on
				// its architectural landmark. Prefer the dry landmark cell for the
				// existing reward budget before falling back onto a feature footprint;
				// otherwise the perch-approach pickup and a room pickup can occupy the
				// same coordinate.
				for (unsigned int index = 0; index < roomCells.Size(); index++)
				{
					if ((int)index == revealCell || (int)index == perchCell ||
						(int)index == liftCell || (int)index == secretAnnexCell ||
						fluidCellReserved[roomCells[index].second * W + roomCells[index].first])
						continue;
					placementCells.Push(index);
					break;
				}
			}
			if (placementCells.Size() == 0) placementCells.Push(0);

		auto CellPosition = [&](int index, double& px, double& py)
		{
			index = clamp(index, 0, (int)roomCells.Size() - 1);
			px = CellCenterX(roomCells[index].first);
			py = CellCenterY(roomCells[index].second);
		};

		static const double slotX[] = {
			0, 80, -80, 0, 0, 80, -80, 80, -80,
			40, -40, 0, 0, 40, -40, 40, -40
		};
		static const double slotY[] = {
			0, 0, 0, 80, -80, 80, 80, -80, -80,
			0, 0, 40, -40, 40, 40, -40, -40
		};
		auto SlotPosition = [&](int slot, double& px, double& py)
		{
			int placementIndex = (slot / countof(slotX)) % (int)placementCells.Size();
			int cellIndex = placementCells[placementIndex];
			CellPosition(cellIndex, px, py);
			double offsetX = slotX[slot % countof(slotX)];
			double offsetY = slotY[slot % countof(slotY)];
			if (roomFluid.floodedRoom)
			{
				offsetX = clamp(offsetX, -roomFluid.islandHalfX + 24.0,
					roomFluid.islandHalfX - 24.0);
				offsetY = clamp(offsetY, -roomFluid.islandHalfY + 24.0,
					roomFluid.islandHalfY - 24.0);
			}
			px += offsetX;
			py += offsetY;
		};

		double anchorX, anchorY;
		CellPosition(0, anchorX, anchorY);
		int startFacingAngle = 0;
		int landmarkSector = -1;
		if (roomFluid.floodedRoom && floodedIslandCell >= 0)
		{
			double islandX, islandY;
			CellPosition(floodedIslandCell, islandX, islandY);
			AddFloodedIsland(room, islandX, islandY,
				roomFluid.islandHalfX, roomFluid.islandHalfY, outdoorRooms[ri]);
		}

		if (revealCell >= 0 && revealKinds[ri] != RevealNone)
		{
			double revealX, revealY;
			CellPosition(revealCell, revealX, revealY);
			const RevealProfile profile = BuildRevealProfile(ri, revealKinds[ri]);
			const int doorSide = clamp(revealDoorSides[ri], 0, 3);
			const bool falseWall = revealArchitectures[ri] == RevealFalseWall;
			int closetSector = -1;
			if (falseWall)
			{
				closetSector = AddFalseWallCloset(room, revealWallLineIndices[ri], revealTags[ri],
					revealBorderTypes[ri], profile, doorSide, revealCues[ri],
					revealX, revealY);
				if (closetSector < 0)
				{
					LastError.Format("Could not emit false-wall reveal for room %u", ri);
					return false;
				}
			}
			else
			{
				if (revealArchitectures[ri] == RevealWallAlcove)
				{
					// Back the rectangular bank against a real exposed wall while
					// retaining a narrow rendering seam. Its door faces into the room,
					// unlike the circulation-on-all-sides pavilion.
					static const int BackDirectionForDoorSide[4] = {
						DIR_S, DIR_W, DIR_N, DIR_E
					};
					const int backDirection = BackDirectionForDoorSide[doorSide];
					const double backExtent = EdgeForCell(
						revealCellX[ri], revealCellY[ri], backDirection);
					const double profileDepth = (doorSide & 1) != 0 ?
						profile.outerX : profile.outerY;
					const double wallX = CellCenterX(revealCellX[ri]) +
						DX[backDirection] * backExtent;
					const double wallY = CellCenterY(revealCellY[ri]) +
						DY[backDirection] * backExtent;
					revealX = wallX - DX[backDirection] * (profileDepth + 12.0);
					revealY = wallY - DY[backDirection] * (profileDepth + 12.0);
					// The shell may have turned this nominal cell edge into an apse or
					// wedge. Nudge the bank to the real emitted backing wall so the
					// visible seam remains a deliberate 12-unit reveal, rather than
					// becoming a stray gap at a shaped perimeter corner.
					const double tangentExtent = (doorSide & 1) != 0 ?
						profile.outerY : profile.outerX;
					for (int attempt = 0; attempt < 2; attempt++)
					{
						const TArray<std::pair<double, double>> outer = BuildRevealOuterLoop(
							revealX, revealY, profile, 32.0);
						const double clearance = MeasureWallAlcoveBackingClearance(room,
							outer, backDirection, revealX, revealY, tangentExtent);
						if (clearance < 0.0 || (clearance >= 8.0 && clearance <= 16.0))
							break;
						const double correction = clamp(clearance - 12.0, -16.0, 16.0);
						revealX += DX[backDirection] * correction;
						revealY += DY[backDirection] * correction;
					}
				}
				else
				{
					revealX += profile.offsetX;
					revealY += profile.offsetY;
				}
				closetSector = AddRevealCloset(room, revealX, revealY, revealTags[ri],
					revealBorderTypes[ri], profile, doorSide,
					revealArchitectures[ri], revealCues[ri], false);
			}
			if (closetSector < 0)
			{
				LastError.Format("Could not emit a reachable switch cache for room %u", ri);
				return false;
			}
			revealClosetSectors[ri] = closetSector;
			static const double TangentX[] = { 1.0, 0.0, -1.0, 0.0 };
			static const double TangentY[] = { 0.0, 1.0, 0.0, -1.0 };
			static const double InwardX[] = { 0.0, -1.0, 0.0, 1.0 };
			static const double InwardY[] = { 1.0, 0.0, -1.0, 0.0 };
			static const double OutwardX[] = { 0.0, 1.0, 0.0, -1.0 };
			static const double OutwardY[] = { -1.0, 0.0, 1.0, 0.0 };
			const double tangentHalf = (doorSide & 1) ? profile.innerY : profile.innerX;
			const double actorSpread = std::min(30.0, tangentHalf - 24.0);
			auto RevealPosition = [&](double tangent, double inward,
				double& x, double& y)
			{
				const double depthX = falseWall ? OutwardX[doorSide] : InwardX[doorSide];
				const double depthY = falseWall ? OutwardY[doorSide] : InwardY[doorSide];
				x = revealX + TangentX[doorSide] * tangent +
					depthX * inward;
				y = revealY + TangentY[doorSide] * tangent +
					depthY * inward;
			};
			{
				const int rewardFirstThing = things.Size();
				auto AddCacheReward = [&](double x, double y, int type) -> bool
				{
					if (!AddThing(x, y, type, 0, false, -1, closetSector)) return false;
					// Cache contents are post-use interaction anchors too. Reserve each
					// actual pickup rather than letting the closet's decorative grammar
					// claim reachability without a collision-clear standing space.
					ReserveNavigationPad(x, y, 40.0);
					return true;
				};
				double firstX, firstY, secondX, secondY;
				const double firstDepth = (profile.variant & 2) != 0 ? -20.0 : 12.0;
				const double secondDepth = (profile.variant & 1) != 0 ? 20.0 : -12.0;
				RevealPosition(-actorSpread, firstDepth, firstX, firstY);
				RevealPosition(actorSpread, secondDepth, secondX, secondY);
				if (!AddCacheReward(firstX, firstY, 2008) ||
					!AddCacheReward(secondX, secondY, 2012))
				{
					LastError.Format("Could not reserve switch-cache rewards for room %u", ri);
					return false;
				}
				int cachePowerup;
				if (room.lockStage <= 0)
					cachePowerup = (profile.variant & 1) ? 2023 : 8; // berserk/backpack
				else if (room.lockStage == 1)
					cachePowerup = (profile.variant & 1) ? 2026 : 2024; // map/invisibility
				else
					cachePowerup = (profile.variant & 1) ? 2013 : 2024; // soul sphere/invisibility
				double powerupX, powerupY;
				RevealPosition(0.0, profile.variant == 2 ? -24.0 : 24.0,
					powerupX, powerupY);
				if (!AddCacheReward(powerupX, powerupY, cachePowerup))
				{
					LastError.Format("Could not reserve switch-cache reward for room %u", ri);
					return false;
				}
				if (profile.variant == 3)
				{
					double bonusX, bonusY;
					RevealPosition(0.0, 0.0, bonusX, bonusY);
					if (!AddCacheReward(bonusX, bonusY, 2015))
					{
						LastError.Format("Could not reserve switch-cache bonus for room %u", ri);
						return false;
					}
				}
				revealRewardFirstThings[ri] = rewardFirstThing;
				revealRewardThingCounts[ri] = things.Size() - rewardFirstThing;
				if (revealRewardThingCounts[ri] <= 0)
				{
					LastError.Format("A switch cache emitted no post-use rewards for room %u", ri);
					return false;
				}
			}
		}

		if (secretAnnexCell >= 0)
		{
			double secretX, secretY;
			CellPosition(secretAnnexCell, secretX, secretY);
			RevealProfile profile = BuildRevealProfile(ri, RevealSwitchCache);
			profile.variant = secretAnnexVariants[ri];
			profile.floorDelta = 0.0;
			profile.ceilingDrop = 0.0;
			secretX += profile.offsetX;
			secretY += profile.offsetY;
			const int doorSide = clamp(secretAnnexDoorSides[ri], 0, 3);
			if (AddRevealCloset(room, secretX, secretY, 0, 0, profile, doorSide,
				RevealPavilion, RevealHidden, true) < 0)
			{
				LastError.Format("Could not emit a fallback manual secret annex for room %u", ri);
				return false;
			}
			const int reward = room.lockStage >= 2 ? 2013 :
				(room.lockStage == 1 ? 2024 : 2023);
			AddThing(secretX, secretY, reward);
			ReserveNavigationPad(secretX, secretY, 40.0);
		}

		if (perchCell >= 0 && perchTags[ri] > 0)
		{
			double perchX, perchY;
			CellPosition(perchCell, perchX, perchY);
			const int approachSide = clamp(perchApproachSides[ri], 0, 3);
			double platformX, platformY;
			AddSniperPerch(room, perchX, perchY, perchTags[ri],
				outdoorRooms[ri], approachSide, perchVariants[ri],
				platformX, platformY);
			AddThing(platformX, platformY, ChooseRangedMonster(room, 3),
				(room.progressionRank * 90) % 360);
			static const double InwardX[] = { 0.0, -1.0, 0.0, 1.0 };
			static const double InwardY[] = { 1.0, 0.0, -1.0, 0.0 };
				// Use an off-grid 88-unit approach offset. General room rewards use
				// 40/80-unit slots, so this keeps the perch pickup distinct even when a
				// compact landmark leaves no other unconstrained placement cell.
				AddThing(perchX + InwardX[approachSide] * 88.0,
					perchY + InwardY[approachSide] * 88.0,
					(RNG() & 1) ? 2007 : 2008);
				ReserveNavigationPad(perchX + InwardX[approachSide] * 88.0,
					perchY + InwardY[approachSide] * 88.0, 40.0);
		}

		if (liftCell >= 0 && liftTags[ri] > 0)
		{
			double liftX, liftY;
			CellPosition(liftCell, liftX, liftY);
			AddLiftPlatform(room, liftX, liftY, liftTags[ri], outdoorRooms[ri]);
			AddThing(liftX, liftY, (RNG() & 1) ? 2012 : 2008);
			ReserveNavigationPad(liftX, liftY,
				LiftPlatformHalf + LiftBypassClearance);
		}

		if (fluidDescriptors[ri].architecture >= 0 &&
			!fluidDescriptors[ri].floodedRoom)
		{
			AddFluidArchitecture(room, fluidDescriptors[ri], outdoorRooms[ri]);
		}

		if (willHaveLandmark)
		{
			CellPosition(landmarkCell, anchorX, anchorY);
			const auto& landmarkAnchor = roomCells[landmarkCell];
			landmarkSector = AddLandmarkPlatform(room, landmarkAnchor.first,
				landmarkAnchor.second, anchorX, anchorY, outdoorRooms[ri]);
		}
		if (room.hasPlayerStart)
		{
			CellPosition(playerCell >= 0 ? playerCell : 0, anchorX, anchorY);
			int startIndex = playerCell >= 0 ? playerCell : 0;
			int startX = roomCells[startIndex].first;
			int startY = roomCells[startIndex].second;
			for (int direction = 0; direction < 4; direction++)
			{
				if (!Grid[startY][startX].conn[direction]) continue;
				startFacingAngle = direction == DIR_E ? 0 : (direction == DIR_S ? 90 :
					(direction == DIR_W ? 180 : 270));
				break;
			}
			AddThing(anchorX, anchorY, 1, startFacingAngle);
			ReserveNavigationPad(anchorX, anchorY, 88.0);
		}
		if (room.hasKey && room.keyType >= 1 && room.keyType <= 3)
		{
			CellPosition(keyCell >= 0 ? keyCell : 0, anchorX, anchorY);
			int keyType = room.keyType == 1 ? 13 : (room.keyType == 2 ? 5 : 6);
			AddThing(anchorX, anchorY, keyType);
			ReserveNavigationPad(anchorX, anchorY, 88.0);
		}
		if (room.isSecret)
		{
			// A secret is never just an unmarked detour. This non-blocking bonus
			// anchors its reward in the discovered sector even if other cache items
			// were placed on a neighboring composed cell.
			if (landmarkSector < 0) CellPosition(0, anchorX, anchorY);
			AddThing(anchorX, anchorY, 2014);
			ReserveNavigationPad(anchorX, anchorY, 40.0);
		}

		int rewardSlot = 1;
		// Pickups that feed the mandatory arsenal/recovery ledger are interaction
		// anchors too. Reserve enough clearance for the player to reach them before
		// the decorative pass considers a prop; this also makes ordinary optional
		// rewards subject to the same no-blocker contract as keys and switches.
		auto AddReservedPickup = [&](double x, double y, int type)
		{
			if (MoveThingOutOfEmittedFluid(x, y) && AddThing(x, y, type))
				ReserveNavigationPad(x, y, 40.0);
		};
		if (room.hasWeapon)
		{
			double x = anchorX;
			double y = anchorY;
			if (!room.hasPlayerStart) SlotPosition(rewardSlot, x, y);
			else
			{
				double radians = startFacingAngle * (3.14159265358979323846 / 180.0);
				x += cos(radians) * 32.0;
				y += sin(radians) * 32.0;
			}
			rewardSlot++;
			AddReservedPickup(x, y, room.weaponType);
		}
		if (room.hasAmmo)
		{
			int packs = std::max(1, room.ammoCount);
			for (int pack = 0; pack < packs; pack++)
			{
				double x, y;
				SlotPosition(rewardSlot++, x, y);
				AddReservedPickup(x, y, room.ammoType);
			}
		}
		if (room.hasHealth)
		{
			int packs = std::max(1, room.healthCount);
			for (int pack = 0; pack < packs; pack++)
			{
				double x, y;
				SlotPosition(rewardSlot++, x, y);
				AddReservedPickup(x, y, room.healthType);
			}
		}
		for (int bonus = 0; bonus < room.healthBonusCount; bonus++)
		{
			double x, y;
			SlotPosition(rewardSlot++, x, y);
			AddReservedPickup(x, y, 2014);
		}
		if (room.hasArmor)
		{
			double x, y;
			SlotPosition(rewardSlot++, x, y);
			AddReservedPickup(x, y, room.armorType);
		}
		for (unsigned int powerup = 0; powerup < room.powerups.Size(); powerup++)
		{
			double x, y;
			SlotPosition(rewardSlot++, x, y);
			AddReservedPickup(x, y, room.powerups[powerup]);
		}

		if (room.hasBoss && !room.hasPlayerStart)
		{
			CellPosition(exitCell >= 0 ? exitCell : 0, anchorX, anchorY);
			int bossType;
			const int MinimumHeavyBossCells = 8;
			const bool prefersHeavyBoss = room.finaleCard == PGFC_Duel ||
				room.finaleCard == PGFC_Fortress;
			const bool hasHeavyArena = room.cellCount >= MinimumHeavyBossCells &&
				prefersHeavyBoss;
			if (!ProcGenUsesDoom2Roster())
			{
				// Ultimate Doom does not have Doom II's visually similar Hell Knight.
				bossType = Difficulty >= 5 && hasHeavyArena ?
					BossesHard[RNG() % countof(BossesHard)] : 3003;
			}
			else
			{
				bossType = Difficulty <= 3 ? BossesEasy[RNG() % countof(BossesEasy)] :
					(Difficulty <= 4 || !hasHeavyArena ? BossesMed[RNG() % countof(BossesMed)] :
						BossesHard[RNG() % countof(BossesHard)]);
			}
			AddThing(anchorX, anchorY, bossType, 0, false, (int)ri);
		}

		static const double enemyX[] = { -96, 96, -96, 96, 0, 0, -64, 64, -112, 112, -40, 40 };
		static const double enemyY[] = { -88, -88, 88, 88, -112, 112, -48, 48, 0, 0, 96, -96 };
		for (int enemy = 0; enemy < room.enemyCount; enemy++)
		{
			const uint32_t placementHash = StableRoomHash(room.id,
				0x504c4143u + (uint32_t)enemy * 0x9e3779b9u);
			int placementIndex = (enemy + room.progressionRank +
				(int)(placementHash % placementCells.Size())) % (int)placementCells.Size();
			int cellIndex = placementCells[placementIndex];
			if (room.hasBoss && placementCells.Size() > 1 && cellIndex == exitCell)
				cellIndex = placementCells[(placementIndex + 1) % placementCells.Size()];
			double x, y;
			CellPosition(cellIndex, x, y);
			double targetX = x;
			double targetY = y;
			int pattern = (enemy / std::max(1, (int)placementCells.Size()) + enemy +
				(int)((placementHash >> 8) % countof(enemyX))) % countof(enemyX);
			double safeX = std::max(32.0, room.halfWidth - room.cornerCut - 20.0);
			double safeY = std::max(32.0, room.halfHeight - room.cornerCut - 20.0);
			if (roomFluid.floodedRoom)
			{
				safeX = std::min(roomFluid.islandHalfX - 32.0, safeX);
				safeY = std::min(roomFluid.islandHalfY - 32.0, safeY);
			}
			x += clamp(enemyX[pattern], -safeX, safeX);
			y += clamp(enemyY[pattern], -safeY, safeY);
			int angle = (int)lround(atan2(targetY - y, targetX - x) *
				(180.0 / 3.14159265358979323846));
			if (angle < 0) angle += 360;
			AddThing(x, y, ChooseMonster(room, enemy), angle, false, (int)ri);
		}

		// Dense role-aware decoration is deliberately subordinate to navigation.
		// The old local checks could accept two 16-radius props forty units apart,
		// leaving a gap a Doom player cannot cross. Reserve player lanes first and
		// let visual density fall back rather than placing a beautiful blocker.
		auto SolidPropRadius = [](int type) -> double
		{
			switch (type)
			{
			case 43: // torch tree
			case 48: // tall tech column
			case 2028: // tall Doom I column fallback
				return 32.0;
			default:
				return 24.0;
			}
		};
		auto DecorationClearsNavigation = [&](double x, double y, double propRadius) -> bool
		{
			for (const NavigationReservation& reservation : navigationReservations)
			{
				if (DistanceToReservation(reservation, x, y) <
					reservation.radius + propRadius + NavigationSafety)
					return false;
			}
			return true;
		};
		auto DecorationSpotClear = [&](double x, double y, double propRadius,
			bool solid) -> bool
		{
			for (const auto& thing : things)
			{
				// Match the post-serialization proof's pair rule exactly. The
				// previous candidate-only radius could accept a short torch just
				// close enough to a wide tree/column to fail the final pinch audit.
				double clearance = 32.0;
				if (solid && thing.solidRadius > 0.0)
				{
					clearance = propRadius + thing.solidRadius +
						PlayerRadius * 2.0 + NavigationSafety;
				}
				else if (solid)
				{
					// Keep solid scenery off actors and pickups even though those
					// dynamic objects are deliberately outside the static proof.
					clearance = propRadius + PlayerRadius + NavigationSafety;
				}
				double dx = thing.x - x;
				double dy = thing.y - y;
				if (dx * dx + dy * dy < clearance * clearance) return false;
			}
			return true;
		};
		auto DecorationBlocksPassage = [&](double x, double y) -> bool
		{
			for (const auto& line : lines)
			{
				if (line.sideBack < 0 || line.blocking) continue;
				const int frontSector = sides[line.sideFront].sector;
				const int backSector = sides[line.sideBack].sector;
				if (frontSector == backSector) continue;
				const BuildSector& front = sectors[frontSector];
				const BuildSector& back = sectors[backSector];
				const bool operableDoor = line.special == 12;
				const bool operableLift = line.special == 62;
				auto UsesLandmarkTrim = [](const BuildSide& side) -> bool
				{
					return side.top.Compare("STEP1") == 0 || side.bottom.Compare("STEP1") == 0 ||
						side.top.Compare("EXITDOOR") == 0 || side.bottom.Compare("EXITDOOR") == 0;
				};
				const bool landmarkStep = line.special == 0 &&
					(UsesLandmarkTrim(sides[line.sideFront]) ||
						UsesLandmarkTrim(sides[line.sideBack]));
				const double approachDepth = landmarkStep ? 40.0 : 112.0;
				const double apertureMargin = landmarkStep ? 12.0 : 28.0;
				const double opening = std::min(front.ceilZ, back.ceilZ) -
					std::max(front.floorZ, back.floorZ);
				if (!operableDoor && !operableLift &&
					(opening < 56.0 || fabs(front.floorZ - back.floorZ) > 24.0))
					continue;

				const BuildVertex& first = vertices[line.v1];
				const BuildVertex& second = vertices[line.v2];
				const double dx = second.x - first.x;
				const double dy = second.y - first.y;
				const double length = hypot(dx, dy);
				if (length < 0.001) continue;
				const double unitX = dx / length;
				const double unitY = dy / length;
				const double relativeX = x - first.x;
				const double relativeY = y - first.y;
				const double along = relativeX * unitX + relativeY * unitY;
				const double normal = fabs(relativeX * unitY - relativeY * unitX);
				if (along >= -apertureMargin && along <= length + apertureMargin &&
					normal <= approachDepth)
					return true;
			}
			return false;
		};
		auto PlaceDecoration = [&](int type, bool solid, int salt) -> bool
		{
			const double propRadius = solid ? SolidPropRadius(type) : 0.0;
			static const double decorX[] = {
				-112.0, 112.0, 112.0, -112.0,
				-112.0, 0.0, 112.0, 0.0,
				-80.0, 80.0, 80.0, -80.0
			};
			static const double decorY[] = {
				112.0, 112.0, -112.0, -112.0,
				0.0, 112.0, 0.0, -112.0,
				80.0, 80.0, -80.0, -80.0
			};
			int attempts = (int)placementCells.Size() * countof(decorX);
			for (int attempt = 0; attempt < attempts; attempt++)
			{
				int placementIndex = (attempt / countof(decorX)) % (int)placementCells.Size();
				int cellIndex = placementCells[placementIndex];
				int corner = (attempt + salt) % countof(decorX);
				double x, y;
				CellPosition(cellIndex, x, y);
				double safeX = std::max(32.0, room.halfWidth - room.cornerCut - 18.0);
				double safeY = std::max(32.0, room.halfHeight - room.cornerCut - 18.0);
				if (roomFluid.floodedRoom)
				{
					safeX = std::min(roomFluid.islandHalfX - 40.0, safeX);
					safeY = std::min(roomFluid.islandHalfY - 40.0, safeY);
				}
				x += clamp(decorX[corner], -safeX, safeX);
				y += clamp(decorY[corner], -safeY, safeY);
				// Two wide solid props need an actual player-width channel between
				// their conservative collision cylinders, rather than merely distinct
				// origins. This deliberately skips ornamental density before it can
				// form a 16-unit pinch in a route or landmark lane.
				if (PointInEmittedFluid(x, y)) continue;
				if (!DecorationSpotClear(x, y, propRadius, solid)) continue;
				if (solid && !DecorationClearsNavigation(x, y, propRadius)) continue;
				if (solid && DecorationBlocksPassage(x, y)) continue;
				if (!AddThing(x, y, type, (corner * 90 + 45) % 360)) continue;
				if (solid) things.Last().solidRadius = propRadius;
				return true;
			}
			// Dense finales can occupy every traditional Doom corner slot. Search a
			// finer deterministic wall-bay lattice before dropping a mandatory theme
			// landmark; the same actor and portal-clearance checks still apply.
			for (unsigned int placementIndex = 0; placementIndex < placementCells.Size(); placementIndex++)
			{
				double centerX, centerY;
				CellPosition(placementCells[placementIndex], centerX, centerY);
				double safeX = std::max(32.0, room.halfWidth - room.cornerCut - 18.0);
				double safeY = std::max(32.0, room.halfHeight - room.cornerCut - 18.0);
				if (roomFluid.floodedRoom)
				{
					safeX = std::min(roomFluid.islandHalfX - 40.0, safeX);
					safeY = std::min(roomFluid.islandHalfY - 40.0, safeY);
				}
				for (int row = -3; row <= 3; row++)
				{
					for (int column = -3; column <= 3; column++)
					{
						if (abs(row) < 2 && abs(column) < 2) continue;
						double x = centerX + safeX * column / 3.0;
						double y = centerY + safeY * row / 3.0;
						if (PointInEmittedFluid(x, y)) continue;
						if (!DecorationSpotClear(x, y, propRadius, solid)) continue;
						if (solid && !DecorationClearsNavigation(x, y, propRadius)) continue;
						if (solid && DecorationBlocksPassage(x, y)) continue;
						if (!AddThing(x, y, type, ((column - row + 8) * 45) % 360)) continue;
						if (solid) things.Last().solidRadius = propRadius;
						return true;
					}
				}
			}
			return false;
		};

		const ThemeStyle themeStyle = GetThemeStyle(Theme);
		const bool corruptedInfernal = themeStyle == ThemeCorrupted &&
			(room.lockStage >= 2 || room.monsterTier >= 4);
		const bool infernalDecor = themeStyle == ThemeHell || themeStyle == ThemeGothic ||
			corruptedInfernal;
		const bool doom2Roster = ProcGenUsesDoom2Roster();
		bool majorLandmark = room.hasPlayerStart || room.hasKey || room.hasExit ||
			room.isHub || room.isArena || room.isSecret;
		int decorationCount = majorLandmark ? std::min(8, 4 + room.cellCount / 2) :
			(1 + abs(room.id * 5 + room.progressionRank + room.branchDepth) % 3);
		if (Detail == 0) decorationCount = std::max(1, (decorationCount + 1) / 2);
		else if (Detail == 2) decorationCount = std::min(12,
			decorationCount + 2 + room.cellCount / 3);
		if (themeStyle == ThemeGothic && majorLandmark) decorationCount = std::min(12, decorationCount + 2);
		else if (themeStyle == ThemeIndustrial && room.isHub) decorationCount = std::min(12, decorationCount + 2);
		// Key and finale rooms may still receive a beacon at a safe perimeter
		// slot. A normal door or a planned stair makes only its *approach* a
		// no-prop zone, not the entire room: the explicit reservation and passage
		// tests below reject every conflicting candidate. This preserves the hard
		// clearance contract while letting a Gothic nave, an industrial bay, or a
		// Hell terrace retain meaningful perimeter ornament instead of going bare
		// whenever it owns a route transition. One-cell connectors, start pads,
		// and locked transition rooms remain unconditional no-prop areas.
		const bool solidDecorationAllowed = !room.isLocked && room.spatialClass > 0 &&
			!room.hasPlayerStart;
		if (decorationCount > 0 && solidDecorationAllowed)
		{
			if (!infernalDecor)
			{
				int primary = doom2Roster ? (room.isArena ? 86 : 85) :
					(room.isArena ? 2028 : 48);
				if (themeStyle == ThemeIndustrial)
					primary = doom2Roster ? 86 : 48;
				if (outdoorRooms[ri]) primary = doom2Roster ? 85 : 48;
				for (int decor = 0; decor < decorationCount; decor++)
				{
					int type = primary;
					if (themeStyle == ThemeIndustrial && decor > 0)
					{
						const int machineryPart = decor % 4;
						if (machineryPart == 1) type = 48; // tall tech column
						else if (machineryPart == 2) type = 2035; // machinery barrel
						else if (machineryPart == 3) type = doom2Roster ? 85 : 2028;
					}
					else if (themeStyle == ThemeCorrupted && decor > 0 && (decor & 1))
						type = room.monsterTier >= 3 ? 56 : 55;
					else if (themeStyle == ThemeTechbase && decor > 0 && decor % 3 == 2)
						type = room.isArena ? 48 : (doom2Roster ? 86 : 2028);
					PlaceDecoration(type, true, room.id + decor * 2);
				}
			}
			else
			{
				int primary;
				if (room.hasKey && room.keyType == 2) primary = 44; // blue
				else if (room.hasKey && room.keyType == 1) primary = 46; // red
				else if (room.hasKey && room.keyType == 3) primary = 35; // yellow/gold
				else if (room.hasExit) primary = 41; // evil eye finale marker
				else if (room.isSecret || themeStyle == ThemeGothic) primary = 35; // candelabra
				else if (outdoorRooms[ri]) primary = 43; // torch tree
				else if (room.monsterTier <= 2) primary = 55; // short blue torch
				else if (room.monsterTier <= 4) primary = 56; // short green torch
				else primary = 57; // short red torch
				for (int decor = 0; decor < decorationCount; decor++)
				{
					int type = outdoorRooms[ri] && decor > 0 ? 43 : primary;
					if (themeStyle == ThemeGothic && !outdoorRooms[ri] && decor > 0)
						type = (decor & 1) ? 45 : 35;
					else if (themeStyle == ThemeHell && !outdoorRooms[ri] && decor > 0 &&
						decor % 3 == 0)
						type = 35;
					const bool placed = PlaceDecoration(type, true, room.id + decor * 2);
					if (themeStyle == ThemeHell)
					{
						if (type == 41 && placed) hellFinaleMarkerPlaced = true;
						if (type == 43 && placed) hellOutdoorMarkerPlaced = true;
					}
				}
			}
		}

		if (themeStyle == ThemeHell && room.hasExit && !hellFinaleMarkerPlaced)
		{
			// EvilEye is solid. A NonsolidMeat2 fallback still gives the finale an
			// unmistakably infernal hanging marker while preserving every reserved
			// route, portal, and interaction approach.
			hellFinaleMarkerPlaced = PlaceDecoration(41, true, room.id + 29);
			if (!hellFinaleMarkerPlaced)
				hellFinaleMarkerPlaced = PlaceDecoration(59, false, room.id + 31);
		}
		if (themeStyle == ThemeHell && outdoorRooms[ri] && !hellOutdoorMarkerPlaced)
		{
			// TorchTree is likewise solid. Keep the outdoor Hell grammar readable
			// without turning a dense courtyard's last clear passage into a pinch.
			hellOutdoorMarkerPlaced = PlaceDecoration(43, true, room.id + 37);
			if (!hellOutdoorMarkerPlaced)
				hellOutdoorMarkerPlaced = PlaceDecoration(60, false, room.id + 41);
		}

		if (!room.hasPlayerStart && room.enemyCount >= 2 &&
			(room.isArena || room.isSecret || ((room.id + room.branchDepth) % 3) == 0))
		{
			int corpse = infernalDecor ? 20 : 15; // dead imp / dead marine
			PlaceDecoration(corpse, false, room.id + 3);
			if (majorLandmark && room.enemyCount >= 4)
				PlaceDecoration(corpse, false, room.id + 9);
		}

		if (room.hasExit)
		{
			CellPosition(exitCell >= 0 ? exitCell : 0, anchorX, anchorY);
			// Exit_Normal with explicit walk activation. Both sides reference the
			// same sector, as is standard for an interior walkover trigger.
			int triggerSector = landmarkSector >= 0 && landmarkCell == (exitCell >= 0 ? exitCell : 0) ?
				landmarkSector : room.sectorIdx;
			AddLine(anchorX - 48.0, anchorY, anchorX + 48.0, anchorY,
				triggerSector, triggerSector,
				nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
				false, 243, 0, 0, 0, 0, 0, 0,
				false, true, false);
			ReserveNavigationPad(anchorX, anchorY, 112.0);
		}
	}

	if (fluidThingPlacementFailed)
	{
		LastError = "Could not move an initial actor or pickup onto a dry fluid bank";
		return false;
	}

	// Post-emission accessibility proof. This runs over the same connection
	// sectors, door faces, switch lines, interaction pads, and collision radii
	// that will be serialized below. It is intentionally stricter than the
	// coarse topology audit: a graph edge is useful only when its emitted portal
	// has a clear approach, and ordinary cells must be reachable in a valid key
	// inventory state.
	for (unsigned int first = 0; first < things.Size(); ++first)
	{
		const BuildThing& prop = things[first];
		if (prop.solidRadius <= 0.0) continue;
		for (const NavigationReservation& reservation : navigationReservations)
		{
			if (DistanceToReservation(reservation, prop.x, prop.y) <
				reservation.radius + prop.solidRadius + NavigationSafety)
			{
				LastError = "A static decoration overlaps a reserved navigation lane";
				return false;
			}
		}
		for (unsigned int second = first + 1; second < things.Size(); ++second)
		{
			const BuildThing& other = things[second];
			if (other.solidRadius <= 0.0) continue;
			const double required = prop.solidRadius + other.solidRadius +
				PlayerRadius * 2.0 + NavigationSafety;
			if (hypot(prop.x - other.x, prop.y - other.y) < required)
			{
				LastError = "Two static decorations form a player-blocking pinch";
				return false;
			}
		}
	}

	auto HasPadReservation = [&](double x, double y, double minimumRadius) -> bool
	{
		for (const NavigationReservation& reservation : navigationReservations)
		{
			if (!reservation.pad || reservation.radius + 0.001 < minimumRadius) continue;
			if (hypot(reservation.x1 - x, reservation.y1 - y) <= 1.0) return true;
		}
		return false;
	};
	auto HasDoorReservation = [&](const BuildLine& line) -> bool
	{
		if (line.v1 < 0 || line.v2 < 0) return false;
		const BuildVertex& first = vertices[line.v1];
		const BuildVertex& second = vertices[line.v2];
		const double x = (first.x + second.x) * 0.5;
		const double y = (first.y + second.y) * 0.5;
		for (const NavigationReservation& reservation : navigationReservations)
		{
			if (reservation.pad || reservation.radius < 56.0) continue;
			if (DistanceToReservation(reservation, x, y) <= reservation.radius + 1.0)
				return true;
		}
		return false;
	};
	auto ReservationRadiusAtPoint = [&](double x, double y, bool pad,
		double minimumRadius) -> double
	{
		double result = 0.0;
		for (const NavigationReservation& reservation : navigationReservations)
		{
			if (reservation.pad != pad || reservation.radius + 0.001 < minimumRadius)
				continue;
			if (DistanceToReservation(reservation, x, y) <= reservation.radius + 1.0)
				result = std::max(result, reservation.radius);
		}
		return result;
	};
	auto ReservationRadiusForLine = [&](const BuildLine& line,
		double minimumRadius) -> double
	{
		if (line.v1 < 0 || line.v2 < 0 || line.v1 >= (int)vertices.Size() ||
			line.v2 >= (int)vertices.Size())
			return 0.0;
		const BuildVertex& first = vertices[line.v1];
		const BuildVertex& second = vertices[line.v2];
		return ReservationRadiusAtPoint((first.x + second.x) * 0.5,
			(first.y + second.y) * 0.5, false, minimumRadius);
	};
	auto LineConnectsSectors = [&](const BuildLine& line, int firstSector,
		int secondSector) -> bool
	{
		if (line.sideFront < 0 || line.sideFront >= (int)sides.Size() ||
			line.sideBack < 0 || line.sideBack >= (int)sides.Size())
			return false;
		const int front = sides[line.sideFront].sector;
		const int back = sides[line.sideBack].sector;
		return (front == firstSector && back == secondSector) ||
			(front == secondSector && back == firstSector);
	};
	auto LineIsWalkable = [&](const BuildLine& line, int keyMask) -> bool
	{
		if (line.blocking || line.sideFront < 0 || line.sideBack < 0 ||
			line.sideFront >= (int)sides.Size() || line.sideBack >= (int)sides.Size())
			return false;
		const int frontSector = sides[line.sideFront].sector;
		const int backSector = sides[line.sideBack].sector;
		if (frontSector < 0 || frontSector >= (int)sectors.Size() ||
			backSector < 0 || backSector >= (int)sectors.Size())
			return false;
		if (line.special == 12)
		{
			if (!line.playerUse) return false;
			return line.lockNumber <= 0 ||
				(keyMask & (1 << (line.lockNumber - 1))) != 0;
		}
		const double opening = std::min(sectors[frontSector].ceilZ, sectors[backSector].ceilZ) -
			std::max(sectors[frontSector].floorZ, sectors[backSector].floorZ);
		return opening >= 56.0 - 0.001;
	};
	auto FindCollisionCorridor = [&](int sourceCell, int targetCell)
		-> const CollisionCorridorProof*
	{
		for (unsigned int index = 0; index < collisionCorridors.Size(); ++index)
		{
			const CollisionCorridorProof& proof = collisionCorridors[index];
			if ((proof.sourceCell == sourceCell && proof.targetCell == targetCell) ||
				(proof.sourceCell == targetCell && proof.targetCell == sourceCell))
				return &proof;
		}
		return nullptr;
	};
	auto PointToSegmentDistance = [](double px, double py, double x1, double y1,
		double x2, double y2) -> double
	{
		const double dx = x2 - x1;
		const double dy = y2 - y1;
		const double lengthSquared = dx * dx + dy * dy;
		if (lengthSquared < 0.0001) return hypot(px - x1, py - y1);
		const double fraction = clamp(((px - x1) * dx + (py - y1) * dy) /
			lengthSquared, 0.0, 1.0);
		return hypot(px - (x1 + dx * fraction), py - (y1 + dy * fraction));
	};
	auto SegmentsIntersect = [](double ax, double ay, double bx, double by,
		double cx, double cy, double dx, double dy) -> bool
	{
		auto Cross = [](double ox, double oy, double px, double py,
			double qx, double qy) -> double
		{
			return (px - ox) * (qy - oy) - (py - oy) * (qx - ox);
		};
		const double abC = Cross(ax, ay, bx, by, cx, cy);
		const double abD = Cross(ax, ay, bx, by, dx, dy);
		const double cdA = Cross(cx, cy, dx, dy, ax, ay);
		const double cdB = Cross(cx, cy, dx, dy, bx, by);
		if (((abC > 0.0001 && abD < -0.0001) ||
			(abC < -0.0001 && abD > 0.0001)) &&
			((cdA > 0.0001 && cdB < -0.0001) ||
			(cdA < -0.0001 && cdB > 0.0001)))
			return true;
		return false;
	};
	auto ReservationHasBlockingWall = [&](const NavigationReservation& reservation) -> bool
	{
		for (const BuildLine& line : lines)
		{
			if (!line.blocking || line.v1 < 0 || line.v2 < 0 ||
				line.v1 >= (int)vertices.Size() || line.v2 >= (int)vertices.Size())
				continue;
			const BuildVertex& first = vertices[line.v1];
			const BuildVertex& second = vertices[line.v2];
			if (SegmentsIntersect(reservation.x1, reservation.y1, reservation.x2,
				reservation.y2, first.x, first.y, second.x, second.y))
				return true;
			const double nearest = std::min(
				std::min(PointToSegmentDistance(first.x, first.y, reservation.x1,
					reservation.y1, reservation.x2, reservation.y2),
					PointToSegmentDistance(second.x, second.y, reservation.x1,
						reservation.y1, reservation.x2, reservation.y2)),
				std::min(PointToSegmentDistance(reservation.x1, reservation.y1, first.x,
					first.y, second.x, second.y),
					PointToSegmentDistance(reservation.x2, reservation.y2, first.x,
						first.y, second.x, second.y)));
			if (nearest < reservation.radius - 0.01) return true;
		}
		return false;
	};
	auto ProveSerializedConnector = [&](int sourceX, int sourceY, int targetX,
		int targetY, const ConnectionRef& forward, const ConnectionRef& reverse,
		int keyMask) -> bool
	{
		const int sourceRoom = Grid[sourceY][sourceX].roomId;
		const int targetRoom = Grid[targetY][targetX].roomId;
		if (!IsValidRoom(sourceRoom) || !IsValidRoom(targetRoom)) return false;
		const int sourceCell = sourceY * W + sourceX;
		const int targetCell = targetY * W + targetX;
		// Only a contour that actually emitted as one room can elide a cell-boundary
		// lane. Complex same-room safe shells retain a real opening, so they must
		// pass the exact same reservation and geometry proof as inter-room travel.
		const bool actualUnifiedEnvelope = sourceRoom == targetRoom &&
			unifiedEnvelopeRooms[sourceRoom] && Rooms[sourceRoom].contourUnified;
		if (actualUnifiedEnvelope)
			return forward.sector == Rooms[sourceRoom].sectorIdx &&
				reverse.sector == Rooms[sourceRoom].sectorIdx;
		if (forward.sector < 0 || reverse.sector < 0) return false;

		const CollisionCorridorProof* corridor = FindCollisionCorridor(sourceCell,
			targetCell);
		if (corridor == nullptr || corridor->reservationCount <= 0 ||
			corridor->firstReservation < 0 ||
			corridor->firstReservation + corridor->reservationCount >
				(int)navigationReservations.Size())
			return false;
		for (int index = 0; index < corridor->reservationCount; ++index)
		{
			const NavigationReservation& reservation = navigationReservations[
				corridor->firstReservation + index];
			if (reservation.pad || reservation.radius + 0.001 <
				PlayerRadius + NavigationSafety)
				return false;
			if (sourceRoom == targetRoom && ReservationHasBlockingWall(reservation))
				return false;
		}
		if (sourceRoom == targetRoom)
		{
			const int roomSector = Rooms[sourceRoom].sectorIdx;
			return forward.sector == roomSector && reverse.sector == roomSector;
		}

		auto HasReservedSectorLink = [&](int firstSector, int secondSector,
			bool doorFace) -> bool
		{
			if (firstSector == secondSector) return true;
			for (const BuildLine& line : lines)
			{
				if (!LineConnectsSectors(line, firstSector, secondSector) ||
					!LineIsWalkable(line, keyMask))
					continue;
				if (doorFace)
				{
					if (line.special != 12 || !line.playerUse) continue;
				}
				else if (line.special == 12)
					continue;
				if (ReservationRadiusForLine(line,
					PlayerRadius + NavigationSafety) > 0.0)
					return true;
			}
			return false;
		};

		const int sourceSector = Rooms[sourceRoom].sectorIdx;
		const int targetSector = Rooms[targetRoom].sectorIdx;
		if (!HasReservedSectorLink(sourceSector, forward.sector, false) ||
			!HasReservedSectorLink(reverse.sector, targetSector, false))
			return false;
		if (forward.door)
		{
			if (forward.doorSector < 0 || reverse.doorSector != forward.doorSector ||
				!HasReservedSectorLink(forward.sector, forward.doorSector, true) ||
				!HasReservedSectorLink(reverse.sector, forward.doorSector, true))
				return false;
		}

		TArray<int> allowedSectors;
		auto AddAllowedSector = [&](int sector)
		{
			if (sector < 0 || sector >= (int)sectors.Size()) return;
			for (unsigned int index = 0; index < allowedSectors.Size(); ++index)
				if (allowedSectors[index] == sector) return;
			allowedSectors.Push(sector);
		};
		auto IsAllowedSector = [&](int sector) -> bool
		{
			for (unsigned int index = 0; index < allowedSectors.Size(); ++index)
				if (allowedSectors[index] == sector) return true;
			return false;
		};
		AddAllowedSector(sourceSector);
		AddAllowedSector(forward.sector);
		AddAllowedSector(reverse.sector);
		AddAllowedSector(forward.doorSector);
		AddAllowedSector(targetSector);
		if (forward.stairIndex >= 0 && forward.stairIndex < (int)stairConnections.Size())
			for (unsigned int index = 0;
				index < stairConnections[forward.stairIndex].sectors.Size(); ++index)
				AddAllowedSector(stairConnections[forward.stairIndex].sectors[index]);

		TArray<unsigned char> seenSectors;
		seenSectors.Resize(sectors.Size());
		for (unsigned int index = 0; index < seenSectors.Size(); ++index)
			seenSectors[index] = 0;
		TArray<int> sectorQueue;
		seenSectors[sourceSector] = 1;
		sectorQueue.Push(sourceSector);
		for (unsigned int queueIndex = 0; queueIndex < sectorQueue.Size(); ++queueIndex)
		{
			const int current = sectorQueue[queueIndex];
			if (current == targetSector) return true;
			for (const BuildLine& line : lines)
			{
				if (!LineIsWalkable(line, keyMask) || line.sideFront < 0 ||
					line.sideBack < 0 || line.sideFront >= (int)sides.Size() ||
					line.sideBack >= (int)sides.Size())
					continue;
				const int front = sides[line.sideFront].sector;
				const int back = sides[line.sideBack].sector;
				const int next = front == current ? back : (back == current ? front : -1);
				if (next < 0 || !IsAllowedSector(next) || seenSectors[next] != 0) continue;
				seenSectors[next] = 1;
				sectorQueue.Push(next);
			}
		}
		return false;
	};
	auto FindReservedSectorLine = [&](int firstSector, int secondSector,
		bool doorFace, int keyMask) -> int
	{
		if (firstSector == secondSector) return -1;
		for (unsigned int lineIndex = 0; lineIndex < lines.Size(); ++lineIndex)
		{
			const BuildLine& line = lines[lineIndex];
			if (!LineConnectsSectors(line, firstSector, secondSector) ||
				!LineIsWalkable(line, keyMask))
				continue;
			if (doorFace)
			{
				if (line.special != 12 || !line.playerUse) continue;
			}
			else if (line.special == 12)
				continue;
			if (ReservationRadiusForLine(line,
				PlayerRadius + NavigationSafety) > 0.0)
				return (int)lineIndex;
		}
		return -1;
	};
	auto FindReservedCacheDoorFace = [&](int firstSector, int doorSector) -> int
	{
		if (firstSector < 0 || doorSector < 0) return -1;
		for (unsigned int lineIndex = 0; lineIndex < lines.Size(); ++lineIndex)
		{
			const BuildLine& line = lines[lineIndex];
			if (line.blocking || !LineConnectsSectors(line, firstSector, doorSector))
				continue;
			if (ReservationRadiusForLine(line,
				PlayerRadius + NavigationSafety) <= 0.0)
				continue;
			return (int)lineIndex;
		}
		return -1;
	};
	auto FindReservedCacheOuterDoorFace = [&](int doorSector, int closetSector,
		int& outerSector) -> int
	{
		outerSector = -1;
		if (doorSector < 0 || closetSector < 0) return -1;
		for (unsigned int lineIndex = 0; lineIndex < lines.Size(); ++lineIndex)
		{
			const BuildLine& line = lines[lineIndex];
			if (line.blocking || line.sideFront < 0 || line.sideBack < 0 ||
				line.sideFront >= (int)sides.Size() || line.sideBack >= (int)sides.Size())
				continue;
			const int front = sides[line.sideFront].sector;
			const int back = sides[line.sideBack].sector;
			const int otherSector = front == doorSector ? back :
				(back == doorSector ? front : -1);
			if (otherSector < 0 || otherSector == closetSector ||
				ReservationRadiusForLine(line,
					PlayerRadius + NavigationSafety) <= 0.0)
				continue;
			// Do not infer the cache-side sector from RoomInfo: safe-shell rooms
			// can place a false-wall face on a feature-cell sector. The physical
			// tagged-door face is the authoritative source for this witness.
			outerSector = otherSector;
			return (int)lineIndex;
		}
		return -1;
	};

	for (const BuildLine& line : lines)
	{
		if (line.special == 11 && line.playerCross)
		{
			LastError = "A procedural Door_Open trigger uses playercross activation";
			return false;
		}
		if (line.special == 12 && line.lockNumber > 0)
		{
			if (!line.playerUse || !HasDoorReservation(line))
			{
				LastError = "A keyed door lacks a collision-clear manual approach";
				return false;
			}
		}
	}

	// Every optional switch cache must be reachable through an explicit use
	// action. A cache that lost its feasible switch was already dropped above;
	// this scan guards against regressions that silently restore auto-open logic.
	for (unsigned int ri = 0; ri < Rooms.Size(); ++ri)
	{
		if (revealKinds[ri] != RevealSwitchCache || revealTags[ri] <= 0) continue;
		bool manualSwitch = false;
		for (const BuildLine& line : lines)
		{
			if (line.special == 11 && line.args[0] == revealTags[ri] &&
				line.playerUse && !line.playerCross)
			{
				manualSwitch = true;
				break;
			}
		}
		if (!manualSwitch)
		{
			LastError = "A switch cache has no explicit player-use interaction";
			return false;
		}
	}

	const int stateCellCount = W * H;
	auto StateCellIndex = [W](int x, int y) { return y * W + x; };
	int startStateCell = -1;
	int exitStateCell = -1;
	TArray<std::pair<int, int>> keyedEdges;
	TArray<int> keyedEdgeMasks;
	AccessibilityMandatoryAnchors = 0;
	AccessibilityOrdinaryCells = 0;
	for (int y = 0; y < H; ++y)
	{
		for (int x = 0; x < W; ++x)
		{
			const ProcGenCell& cell = Grid[y][x];
			if (!cell.present) continue;
			if (cell.hasPlayerStart || cell.hasKey || cell.hasExit)
			{
				const double padRadius = cell.hasExit ? 96.0 : 72.0;
				if (!HasPadReservation(CellCenterX(x), CellCenterY(y), padRadius))
				{
					LastError = "A mandatory start, key, or exit anchor lacks a clear pad";
					return false;
				}
				AccessibilityMandatoryAnchors++;
			}
			if (cell.hasPlayerStart) startStateCell = StateCellIndex(x, y);
			if (cell.hasExit) exitStateCell = StateCellIndex(x, y);
			const bool secretCell = cell.reservedSecret ||
				(IsValidRoom(cell.roomId) && Rooms[cell.roomId].isSecret);
			if (!secretCell) AccessibilityOrdinaryCells++;

		}
	}
	TArray<ProvenNavigationEdge> provenNavigationEdges;
	AccessibilityProvenCorridors = 0;
	AccessibilityRoomMergeCorridors = 0;
	AccessibilityCorridors.Clear();
	for (int y = 0; y < H; ++y)
	{
		for (int x = 0; x < W; ++x)
		{
			if (!Grid[y][x].present) continue;
			for (int direction : { DIR_E, DIR_S })
			{
				if (!Grid[y][x].conn[direction]) continue;
				const int nx = x + DX[direction];
				const int ny = y + DY[direction];
				if (nx < 0 || nx >= W || ny < 0 || ny >= H || !Grid[ny][nx].present)
					continue;
				const ConnectionRef& forward = connectionGrid[y][x].refs[direction];
				const ConnectionRef& reverse = connectionGrid[ny][nx].refs[OPP[direction]];
				if (forward.window || forward.secret) continue;
				const int proofMask = forward.lockType > 0 ?
					(1 << (forward.lockType - 1)) : 0;
				if (!ProveSerializedConnector(x, y, nx, ny, forward, reverse, proofMask))
				{
					LastError.Format("A procedural connection between cells (%d,%d) and (%d,%d) "
						"lacks a collision-clear serialized corridor", x, y, nx, ny);
					return false;
				}
				ProvenNavigationEdge edge;
				edge.sourceCell = StateCellIndex(x, y);
				edge.targetCell = StateCellIndex(nx, ny);
				edge.lockType = forward.lockType;
				provenNavigationEdges.Push(edge);
				const int sourceRoom = Grid[y][x].roomId;
				const int targetRoom = Grid[ny][nx].roomId;
				const bool actualUnifiedEnvelope = sourceRoom == targetRoom &&
					unifiedEnvelopeRooms[sourceRoom] && Rooms[sourceRoom].contourUnified;
				if (!actualUnifiedEnvelope)
				{
					AccessibilityProvenCorridors++;
					const CollisionCorridorProof* corridor = FindCollisionCorridor(
						edge.sourceCell, edge.targetCell);
					if (corridor == nullptr)
					{
						LastError = "A proven collision connection lost its corridor witness";
						return false;
					}
					ProcGenAccessibilityCorridor witness;
					witness.kind = sourceRoom == targetRoom ? "room_merge" :
						(forward.door ? "door" :
							(forward.stairIndex >= 0 ? "stair" : "connector"));
					witness.sourceCell = edge.sourceCell;
					witness.targetCell = edge.targetCell;
					witness.sourceSector = Rooms[sourceRoom].sectorIdx;
					witness.targetSector = Rooms[targetRoom].sectorIdx;
					witness.connectorSector = forward.sector;
					witness.doorSector = forward.doorSector;
					witness.reservationCount = corridor->reservationCount;
					if (sourceRoom != targetRoom)
					{
						witness.sourceLine = FindReservedSectorLine(witness.sourceSector,
							forward.sector, false, proofMask);
						witness.targetLine = FindReservedSectorLine(reverse.sector,
							witness.targetSector, false, proofMask);
					}
					for (int reservationIndex = 0;
						reservationIndex < corridor->reservationCount; ++reservationIndex)
					{
						const NavigationReservation& reservation = navigationReservations[
							corridor->firstReservation + reservationIndex];
						ProcGenAccessibilityLane lane;
						lane.x1 = reservation.x1;
						lane.y1 = reservation.y1;
						lane.x2 = reservation.x2;
						lane.y2 = reservation.y2;
						lane.clearRadius = reservation.radius;
						witness.lanes.Push(lane);
					}
					if (witness.lanes.Size() == 0)
					{
						LastError = "A proven collision connection has no exported lane";
						return false;
					}
					if (sourceRoom == targetRoom) AccessibilityRoomMergeCorridors++;
					AccessibilityCorridors.Push(std::move(witness));
				}
				if (forward.lockType > 0)
				{
					keyedEdges.Push(std::make_pair(edge.sourceCell, edge.targetCell));
					keyedEdgeMasks.Push(1 << (forward.lockType - 1));
				}
			}
		}
	}
	AccessibilityKeyStateEdges = provenNavigationEdges.Size();
	if (startStateCell < 0 || exitStateCell < 0)
	{
		LastError = "The serialized navigation proof could not find start or exit";
		return false;
	}

	TArray<unsigned char> reachedStates;
	reachedStates.Resize(stateCellCount * 8);
	for (unsigned int index = 0; index < reachedStates.Size(); ++index)
		reachedStates[index] = 0;
	TArray<std::pair<int, int>> stateQueue;
	auto AddKeyToMask = [&](int stateCell, int mask) -> int
	{
		const int x = stateCell % W;
		const int y = stateCell / W;
		const ProcGenCell& cell = Grid[y][x];
		if (cell.hasKey && cell.keyType >= 1 && cell.keyType <= 3)
			mask |= 1 << (cell.keyType - 1);
		return mask;
	};
	auto PushState = [&](int stateCell, int mask)
	{
		mask = AddKeyToMask(stateCell, mask);
		const int index = stateCell * 8 + mask;
		if (reachedStates[index] != 0) return;
		reachedStates[index] = 1;
		stateQueue.Push(std::make_pair(stateCell, mask));
	};
	PushState(startStateCell, 0);
	for (unsigned int qi = 0; qi < stateQueue.Size(); ++qi)
	{
		const int stateCell = stateQueue[qi].first;
		const int mask = stateQueue[qi].second;
		for (const ProvenNavigationEdge& edge : provenNavigationEdges)
		{
			int nextCell = -1;
			if (edge.sourceCell == stateCell) nextCell = edge.targetCell;
			else if (edge.targetCell == stateCell) nextCell = edge.sourceCell;
			if (nextCell < 0) continue;
			if (edge.lockType > 0 && (mask & (1 << (edge.lockType - 1))) == 0)
				continue;
			PushState(nextCell, mask);
		}
	}
	auto CellReached = [&](int stateCell, int neededMask) -> bool
	{
		for (int mask = 0; mask < 8; ++mask)
			if ((mask & neededMask) == neededMask &&
				reachedStates[stateCell * 8 + mask] != 0)
				return true;
		return false;
	};
	auto RoomReached = [&](int roomId) -> bool
	{
		for (int y = 0; y < H; ++y)
		{
			for (int x = 0; x < W; ++x)
			{
				if (!Grid[y][x].present || Grid[y][x].roomId != roomId) continue;
				if (CellReached(StateCellIndex(x, y), 0)) return true;
			}
		}
		return false;
	};
	auto RoomReachableMask = [&](int roomId) -> int
	{
		for (int y = 0; y < H; ++y)
		{
			for (int x = 0; x < W; ++x)
			{
				if (!Grid[y][x].present || Grid[y][x].roomId != roomId) continue;
				const int stateCell = StateCellIndex(x, y);
				for (int mask = 0; mask < 8; ++mask)
					if (reachedStates[stateCell * 8 + mask] != 0) return mask;
			}
		}
		return -1;
	};
	auto SectorReachableWithCacheAction = [&](int startSector, int targetSector,
		int keyMask, int openedDoorSector, int openedFirstLine,
		int openedSecondLine) -> bool
	{
		if (startSector < 0 || startSector >= (int)sectors.Size() ||
			targetSector < 0 || targetSector >= (int)sectors.Size())
			return false;
		TArray<unsigned char> seenSectors;
		seenSectors.Resize(sectors.Size());
		for (unsigned int index = 0; index < seenSectors.Size(); ++index)
			seenSectors[index] = 0;
		TArray<int> sectorQueue;
		seenSectors[startSector] = 1;
		sectorQueue.Push(startSector);
		for (unsigned int queueIndex = 0; queueIndex < sectorQueue.Size(); ++queueIndex)
		{
			const int current = sectorQueue[queueIndex];
			if (current == targetSector) return true;
			for (unsigned int lineIndex = 0; lineIndex < lines.Size(); ++lineIndex)
			{
				const BuildLine& line = lines[lineIndex];
				if (line.sideFront < 0 || line.sideBack < 0 ||
					line.sideFront >= (int)sides.Size() ||
					line.sideBack >= (int)sides.Size())
					continue;
				const int front = sides[line.sideFront].sector;
				const int back = sides[line.sideBack].sector;
				const int next = front == current ? back : (back == current ? front : -1);
				if (next < 0 || next >= (int)sectors.Size() || seenSectors[next] != 0)
					continue;
				bool walkable = LineIsWalkable(line, keyMask);
				// Door_Open is represented by a zero-height tagged sector. It is not
				// walkable before use; the proof models exactly one explicit action by
				// opening only that sector's two emitted faces afterward.
				if (!walkable && openedDoorSector >= 0 &&
					((int)lineIndex == openedFirstLine ||
						(int)lineIndex == openedSecondLine) &&
					(front == openedDoorSector || back == openedDoorSector) &&
					openedDoorSector < (int)sectors.Size() &&
					fabs(sectors[openedDoorSector].floorZ -
						sectors[openedDoorSector].ceilZ) < 0.001 &&
					ReservationRadiusForLine(line,
						PlayerRadius + NavigationSafety) > 0.0)
					walkable = true;
				if (!walkable) continue;
				seenSectors[next] = 1;
				sectorQueue.Push(next);
			}
		}
		return false;
	};
	for (int y = 0; y < H; ++y)
	{
		for (int x = 0; x < W; ++x)
		{
			const ProcGenCell& cell = Grid[y][x];
			if (!cell.present) continue;
			const bool secretCell = cell.reservedSecret ||
				(IsValidRoom(cell.roomId) && Rooms[cell.roomId].isSecret);
			const int stateCell = StateCellIndex(x, y);
			if (cell.hasKey && !CellReached(stateCell, 1 << (cell.keyType - 1)))
			{
				LastError = "A required key is unreachable in the serialized key-state proof";
				return false;
			}
			if (!secretCell && !CellReached(stateCell, 0))
			{
				LastError = "An ordinary procedural cell is unreachable after serialization";
				return false;
			}
		}
	}
	if (!CellReached(exitStateCell, 0))
	{
		LastError = "The procedural exit is unreachable in the serialized key-state proof";
		return false;
	}
	// A remote cache is not an automatic key closet. Prove the whole emitted
	// interaction state transition: a reachable explicit-use source opens one
	// closed tagged sector, and only the resulting state reaches the actual
	// closet-sector reward pads. This lets compact-map fallback drop the flourish
	// rather than accidentally restoring a walk-over trigger.
	AccessibilitySwitchCaches.Clear();
	AccessibilitySwitchCacheActions = 0;
	AccessibilitySwitchCacheRewards = 0;
	for (unsigned int host = 0; host < Rooms.Size(); ++host)
	{
		if (revealKinds[host] != RevealSwitchCache || revealTags[host] <= 0) continue;
		const int targetTag = revealTags[host];
		int switchLineIndex = -1;
		int sourceSector = -1;
		int sourceMask = -1;
		for (unsigned int source = 0; source < Rooms.Size(); ++source)
		{
			if (switchTargetTags[source] != targetTag) continue;
			const int reachableMask = RoomReachableMask((int)source);
			if (reachableMask < 0) continue;
			const int sourceRoomSector = Rooms[source].sectorIdx;
			if (sourceRoomSector < 0 || sourceRoomSector >= (int)sectors.Size()) continue;
			for (unsigned int lineIndex = 0; lineIndex < lines.Size(); ++lineIndex)
			{
				const BuildLine& line = lines[lineIndex];
				const int candidateSector = line.sideFront >= 0 &&
					line.sideFront < (int)sides.Size() ?
					sides[line.sideFront].sector : -1;
				if (line.special != 11 || line.args[0] != targetTag ||
					!line.playerUse || line.playerCross || line.sideFront < 0 ||
					line.sideFront >= (int)sides.Size() || candidateSector < 0 ||
					!SectorReachableWithCacheAction(sourceRoomSector, candidateSector,
						reachableMask, -1, -1, -1) ||
					ReservationRadiusForLine(line,
						PlayerRadius + NavigationSafety) <= 0.0)
					continue;
				switchLineIndex = (int)lineIndex;
				sourceSector = candidateSector;
				sourceMask = reachableMask;
				break;
			}
			if (switchLineIndex >= 0) break;
		}
		int doorSector = -1;
		for (int candidateDoorSector : remoteCacheDoorSectors)
		{
			if (candidateDoorSector < 0 || candidateDoorSector >= (int)sectors.Size()) continue;
			const BuildSector& sector = sectors[candidateDoorSector];
			if (sector.id == targetTag && fabs(sector.floorZ - sector.ceilZ) < 0.001)
			{
				// Each cache gets one tagged zero-height sector; duplicate references
				// simply name its two physical faces.
				doorSector = candidateDoorSector;
				break;
			}
		}
		const int closetSector = revealClosetSectors[host];
		const int rewardFirst = revealRewardFirstThings[host];
		const int rewardCount = revealRewardThingCounts[host];
		int cacheSector = -1;
		const int sourceDoorLine = FindReservedCacheOuterDoorFace(doorSector,
			closetSector, cacheSector);
		const int closetDoorLine = FindReservedCacheDoorFace(closetSector, doorSector);
		const double sourceDoorClearRadius = sourceDoorLine >= 0 ?
			ReservationRadiusForLine(lines[sourceDoorLine],
				PlayerRadius + NavigationSafety) : 0.0;
		const double closetDoorClearRadius = closetDoorLine >= 0 ?
			ReservationRadiusForLine(lines[closetDoorLine],
				PlayerRadius + NavigationSafety) : 0.0;
		if (switchLineIndex < 0 || sourceSector < 0 || sourceMask < 0 ||
			cacheSector < 0 || cacheSector >= (int)sectors.Size() || doorSector < 0 ||
			closetSector < 0 || closetSector >= (int)sectors.Size() ||
			sourceDoorLine < 0 || closetDoorLine < 0 ||
			sourceDoorClearRadius <= 0.0 || closetDoorClearRadius <= 0.0 ||
			rewardFirst < 0 || rewardCount <= 0 ||
			rewardFirst + rewardCount > (int)things.Size())
		{
			LastError = "A switch cache lacks a proven reachable manual interaction chain";
			return false;
		}
		for (int reward = 0; reward < rewardCount; ++reward)
		{
			const BuildThing& thing = things[rewardFirst + reward];
			if (thing.accessibilitySector != closetSector ||
				!HasPadReservation(thing.x, thing.y, 40.0))
			{
				LastError = "A switch-cache reward lacks a collision-clear closet anchor";
				return false;
			}
		}
		if (SectorReachableWithCacheAction(sourceSector, closetSector,
			sourceMask, -1, -1, -1))
		{
			LastError = "A switch-cache reward is reachable before its explicit action";
			return false;
		}
		if (!SectorReachableWithCacheAction(sourceSector, closetSector,
			sourceMask, doorSector, sourceDoorLine, closetDoorLine))
		{
			LastError = "A switch-cache reward is not reachable after its explicit action";
			return false;
		}
		ProcGenAccessibilitySwitchCache witness;
		witness.tag = targetTag;
		witness.switchLine = switchLineIndex;
		witness.sourceSector = sourceSector;
		witness.cacheSector = cacheSector;
		witness.doorSector = doorSector;
		witness.closetSector = closetSector;
		witness.sourceDoorLine = sourceDoorLine;
		witness.closetDoorLine = closetDoorLine;
		witness.sourceDoorClearRadius = sourceDoorClearRadius;
		witness.closetDoorClearRadius = closetDoorClearRadius;
		witness.rewardFirstThing = rewardFirst;
		witness.rewardThingCount = rewardCount;
		AccessibilitySwitchCaches.Push(witness);
		AccessibilitySwitchCacheActions++;
		AccessibilitySwitchCacheRewards += rewardCount;
	}
	for (unsigned int edgeIndex = 0; edgeIndex < keyedEdges.Size(); ++edgeIndex)
	{
		const auto& edge = keyedEdges[edgeIndex];
		const int neededMask = keyedEdgeMasks[edgeIndex];
		// Both approaches must be usable after the matching key has been
		// collected, rather than merely adjacent in a sector graph.
		if (!CellReached(edge.first, neededMask) ||
			!CellReached(edge.second, neededMask))
		{
			LastError = "A keyed door loses one of its serialized approaches";
			return false;
		}
	}

	// Persist a bounded set of player-critical witnesses. The full proof remains
	// internal (it covers every ordinary cell and emitted connector), while these
	// stable thing/linedef references let the manifest validator independently
	// audit starts, keys, exits, door approaches, and explicit switches.
	AccessibilityAnchors.Clear();
	auto AddAccessibilityAnchor = [&](const char* kind, int thingIndex, int lineIndex,
		int sideIndex, double x, double y, double clearRadius, int lock, int tag)
	{
		ProcGenAccessibilityAnchor anchor;
		anchor.kind = kind;
		anchor.thingIndex = thingIndex;
		anchor.line = lineIndex;
		anchor.side = sideIndex;
		anchor.x = x;
		anchor.y = y;
		anchor.clearRadius = clearRadius;
		anchor.lock = lock;
		anchor.tag = tag;
		AccessibilityAnchors.Push(std::move(anchor));
	};
	int startWitnesses = 0;
	int keyWitnesses = 0;
	for (unsigned int thingIndex = 0; thingIndex < things.Size(); ++thingIndex)
	{
		const BuildThing& thing = things[thingIndex];
		if (thing.type == 1)
		{
			const double radius = ReservationRadiusAtPoint(thing.x, thing.y, true, 72.0);
			if (radius <= 0.0)
			{
				LastError = "The serialized player start has no collision-clear anchor";
				return false;
			}
			AddAccessibilityAnchor("start", (int)thingIndex, -1, -1,
				thing.x, thing.y, radius, 0, 0);
			startWitnesses++;
		}
		int keyLock = thing.type == 13 ? 1 : (thing.type == 5 ? 2 :
			(thing.type == 6 ? 3 : 0));
		if (keyLock > 0)
		{
			const double radius = ReservationRadiusAtPoint(thing.x, thing.y, true, 72.0);
			if (radius <= 0.0)
			{
				LastError = "A serialized required key has no collision-clear anchor";
				return false;
			}
			AddAccessibilityAnchor("key", (int)thingIndex, -1, -1,
				thing.x, thing.y, radius, keyLock, 0);
			keyWitnesses++;
		}
	}
	if (startWitnesses != 1 || keyWitnesses == 0)
	{
		LastError = "The collision navigation proof could not witness the start and keys";
		return false;
	}
	for (const ProcGenAccessibilitySwitchCache& cache : AccessibilitySwitchCaches)
	{
		for (int reward = 0; reward < cache.rewardThingCount; ++reward)
		{
			const int thingIndex = cache.rewardFirstThing + reward;
			if (thingIndex < 0 || thingIndex >= (int)things.Size())
			{
				LastError = "A switch-cache witness has an invalid reward thing";
				return false;
			}
			const BuildThing& thing = things[thingIndex];
			const double radius = ReservationRadiusAtPoint(thing.x, thing.y, true, 40.0);
			if (thing.accessibilitySector != cache.closetSector || radius <= 0.0)
			{
				LastError = "A switch-cache reward has no serialized collision-clear pad";
				return false;
			}
			AddAccessibilityAnchor("switch_cache_reward", thingIndex, -1, -1,
				thing.x, thing.y, radius, 0, cache.tag);
		}
	}

	int exitWitnesses = 0;
	int keyedDoorWitnesses = 0;
	AccessibilityManualSwitches = 0;
	for (unsigned int lineIndex = 0; lineIndex < lines.Size(); ++lineIndex)
	{
		const BuildLine& line = lines[lineIndex];
		if (line.v1 < 0 || line.v2 < 0 || line.v1 >= (int)vertices.Size() ||
			line.v2 >= (int)vertices.Size())
			continue;
		const BuildVertex& first = vertices[line.v1];
		const BuildVertex& second = vertices[line.v2];
		const double x = (first.x + second.x) * 0.5;
		const double y = (first.y + second.y) * 0.5;
		if (line.special == 243 && line.playerCross)
		{
			const double radius = ReservationRadiusAtPoint(x, y, true, 96.0);
			if (radius <= 0.0)
			{
				LastError = "The serialized exit has no collision-clear anchor";
				return false;
			}
			AddAccessibilityAnchor("exit", -1, (int)lineIndex, line.sideFront,
				x, y, radius, 0, 0);
			exitWitnesses++;
		}
		if (line.special == 12 && line.lockNumber > 0)
		{
			const double radius = ReservationRadiusForLine(line, 56.0);
			if (!line.playerUse || radius <= 0.0)
			{
				LastError = "A keyed door witness has no collision-clear manual approach";
				return false;
			}
			AddAccessibilityAnchor("keyed_door_approach", -1, (int)lineIndex,
				line.sideFront, x, y, radius, line.lockNumber, 0);
			keyedDoorWitnesses++;
		}
		if (line.special == 11 && line.playerUse && !line.playerCross)
		{
			const double radius = ReservationRadiusForLine(line,
				PlayerRadius + NavigationSafety);
			if (radius <= 0.0)
			{
				LastError = "A manual switch witness has no collision-clear approach";
				return false;
			}
			bool sourceReachable = false;
			if (line.sideFront >= 0 && line.sideFront < (int)sides.Size())
			{
				const int sourceSector = sides[line.sideFront].sector;
				for (unsigned int roomIndex = 0; roomIndex < Rooms.Size(); ++roomIndex)
					if (Rooms[roomIndex].sectorIdx == sourceSector && RoomReached((int)roomIndex))
						sourceReachable = true;
			}
			if (!sourceReachable)
			{
				LastError = "A manual switch is not reachable in its valid key state";
				return false;
			}
			AddAccessibilityAnchor("manual_switch", -1, (int)lineIndex,
				line.sideFront, x, y, radius, 0, line.args[0]);
			AccessibilityManualSwitches++;
		}
	}
	if (exitWitnesses != 1 || keyedDoorWitnesses != (int)keyedEdges.Size() * 2)
	{
		LastError = "The collision navigation proof has incomplete exit or keyed-door witnesses";
		return false;
	}

	AccessibilityRequiredKeyMask = 0;
	for (unsigned int index = 0; index < keyedEdgeMasks.Size(); ++index)
		AccessibilityRequiredKeyMask |= keyedEdgeMasks[index];
	AccessibilityExitKeyMask = 0;
	for (int mask = 0; mask < 8; ++mask)
		if (reachedStates[exitStateCell * 8 + mask] != 0)
			AccessibilityExitKeyMask |= mask;
	if ((AccessibilityExitKeyMask & AccessibilityRequiredKeyMask) !=
		AccessibilityRequiredKeyMask)
	{
		LastError = "The collision navigation exit state lacks a required key";
		return false;
	}
	AccessibilityOrdinaryRewards = 0;
	for (unsigned int roomIndex = 0; roomIndex < Rooms.Size(); ++roomIndex)
	{
		const RoomInfo& room = Rooms[roomIndex];
		if (room.isSecret || room.reservedSecret) continue;
		const bool hasReward = room.rewardPlan != PGRW_None || room.hasWeapon ||
			room.hasAmmo || room.hasHealth || room.hasArmor || room.powerups.Size() > 0;
		if (!hasReward) continue;
		if (!RoomReached((int)roomIndex))
		{
			LastError = "A non-secret optional reward room is unreachable after serialization";
			return false;
		}
		AccessibilityOrdinaryRewards++;
	}

	// Encounter cards are static UDMF contracts, not a promise to spawn or
	// adapt at runtime. Record the realized evidence after every connection,
	// switch, actor, and reservation has been emitted so the manifest can prove
	// that each retained card has a fitting room footprint and its required
	// player-use interaction (when it actually planned one).
	TArray<int> cardStaticEnemies;
	TArray<int> cardOpenApproaches;
	TArray<int> cardDoorApproaches;
	cardStaticEnemies.Resize(Rooms.Size());
	cardOpenApproaches.Resize(Rooms.Size());
	cardDoorApproaches.Resize(Rooms.Size());
	for (unsigned int ri = 0; ri < Rooms.Size(); ++ri)
	{
		cardStaticEnemies[ri] = 0;
		cardOpenApproaches[ri] = 0;
		cardDoorApproaches[ri] = 0;
	}
	for (const BuildThing& thing : things)
	{
		if (thing.encounterRoom >= 0 && thing.encounterRoom < (int)Rooms.Size())
			cardStaticEnemies[thing.encounterRoom]++;
	}
	for (int y = 0; y < H; ++y)
	{
		for (int x = 0; x < W; ++x)
		{
			if (!Grid[y][x].present) continue;
			for (int direction : { DIR_E, DIR_S })
			{
				const int nx = x + DX[direction];
				const int ny = y + DY[direction];
				if (nx < 0 || nx >= W || ny < 0 || ny >= H ||
					!Grid[ny][nx].present)
					continue;
				const ConnectionRef& ref = connectionGrid[y][x].refs[direction];
				const int firstRoom = Grid[y][x].roomId;
				const int secondRoom = Grid[ny][nx].roomId;
				if (ref.sector < 0 || ref.window || ref.secret ||
					firstRoom == secondRoom || !IsValidRoom(firstRoom) ||
					!IsValidRoom(secondRoom))
					continue;
				if (ref.door)
				{
					cardDoorApproaches[firstRoom]++;
					cardDoorApproaches[secondRoom]++;
				}
				else
				{
					cardOpenApproaches[firstRoom]++;
					cardOpenApproaches[secondRoom]++;
				}
			}
		}
	}
	for (unsigned int ri = 0; ri < Rooms.Size(); ++ri)
	{
		RoomInfo& room = Rooms[ri];
		int manualActions = 0;
		switch ((EProcGenManualInteraction)room.manualInteraction)
		{
		case PGMI_KeyedDoor:
			for (const BuildLine& line : lines)
			{
				if (line.special == 12 && line.lockNumber == room.lockType &&
					line.playerUse)
				{
					manualActions = 1;
					break;
				}
			}
			break;
		case PGMI_SwitchCache:
			if (switchTargetTags[ri] > 0)
			{
				for (const BuildLine& line : lines)
				{
					if (line.special == 11 && line.args[0] == switchTargetTags[ri] &&
						line.playerUse && !line.playerCross)
					{
						manualActions = 1;
						break;
					}
				}
			}
			break;
		case PGMI_SecretDoor:
			for (const BuildLine& line : lines)
			{
				if (line.special == 12 && line.secret && line.playerUse)
				{
					manualActions = 1;
					break;
				}
			}
			break;
		default:
			break;
		}
		room.cardStaticEnemies = cardStaticEnemies[ri];
		room.cardManualActions = manualActions;
		const int floorSlots = std::max(1, (int)floor(
			(room.halfWidth * 2.0 * room.halfHeight * 2.0) / (128.0 * 128.0)));
		const int approachCount = cardOpenApproaches[ri] + cardDoorApproaches[ri];
		const bool wideFootprint = room.cellCount >= 2 ||
			(room.halfWidth >= 144.0 && room.halfHeight >= 144.0) ||
			approachCount >= 2;
		int capacity = std::max(room.cellCount, floorSlots);
		if (wideFootprint) capacity = std::max(capacity, 2);
		if (room.isArena || room.isHub || room.hasKey || room.hasExit ||
			room.hasBoss || room.landmarkArchetype != PGLA_None)
			capacity = std::max(capacity, 3);
		// An emitted static monster is itself a concrete supported placement. This
		// keeps capacity honest for compact rooms while the footprint calculation
		// still catches a label that needs more lanes than the room owns.
		capacity = std::max(capacity, room.cardStaticEnemies);
		room.cardCapacity = capacity;

		const bool hasCacheReward = room.rewardPlan == PGRW_Cache ||
			room.hasWeapon || room.hasAmmo || room.hasHealth || room.hasArmor ||
			room.powerups.Size() > 0;
		const bool plannedCacheSwitch = room.encounterCard == PGEC_CacheChallenge &&
			room.manualInteraction == PGMI_SwitchCache;
		bool feasible = true;
		switch ((EProcGenEncounterCard)room.encounterCard)
		{
		case PGEC_None:
			room.cardGeometry = "none";
			break;
		case PGEC_Breather:
			room.cardGeometry = (room.hasHealth || room.hasAmmo || room.recoveryBudget > 0) ?
				"recovery_pad" : "clear_floor";
			feasible = capacity >= 1 && room.cardStaticEnemies == 0;
			break;
		case PGEC_Skirmish:
			room.cardGeometry = "open_floor";
			feasible = capacity >= 1 && room.cardStaticEnemies >= 1;
			break;
		case PGEC_Crossfire:
			room.cardGeometry = "opposed_lanes";
			feasible = wideFootprint && capacity >= 2 && room.cardStaticEnemies >= 2 &&
				approachCount >= 2;
			break;
		case PGEC_Pincer:
			room.cardGeometry = "dual_flank";
			feasible = wideFootprint && capacity >= 2 && room.cardStaticEnemies >= 2 &&
				approachCount >= 2;
			break;
		case PGEC_Ambush:
			room.cardGeometry = "staged_threshold";
			feasible = capacity >= 2 && room.cardStaticEnemies >= 2 && approachCount >= 1;
			break;
		case PGEC_CacheChallenge:
			room.cardGeometry = plannedCacheSwitch ? "switch_cache" : "reward_cache";
			feasible = capacity >= 1 && room.cardStaticEnemies >= 1 && hasCacheReward &&
				(!plannedCacheSwitch || manualActions > 0);
			break;
		case PGEC_HoldingLine:
			room.cardGeometry = "defense_line";
			feasible = wideFootprint && capacity >= 3 && room.cardStaticEnemies >= 3 &&
				approachCount >= 1;
			break;
		case PGEC_SetPiece:
			room.cardGeometry = "arena_footprint";
			feasible = wideFootprint && capacity >= 3 && room.cardStaticEnemies >= 3 &&
				(room.isArena || room.hasKey || room.hasExit || room.hasBoss ||
					room.landmarkArchetype != PGLA_None);
			break;
		default:
			room.cardGeometry = "unknown";
			feasible = false;
			break;
		}
		if (!feasible && room.encounterCard != PGEC_None)
		{
			// Room composition can legitimately turn a planned flank, arena, or
			// breather into a different static space (for example when a landmark
			// consumes a branch cell). Never leave that mismatch as a decorative
			// manifest claim: retain the emitted combat as a skirmish, or a true
			// empty room as a breather. This is deterministic and happens only after
			// all UDMF geometry and manual interactions have been proven.
			if (room.cardStaticEnemies > 0)
			{
				room.encounterCard = PGEC_Skirmish;
				room.cardGeometry = "open_floor";
				feasible = capacity >= 1;
			}
			else
			{
				room.encounterCard = PGEC_Breather;
				room.cardGeometry = (room.hasHealth || room.hasAmmo || room.recoveryBudget > 0) ?
					"recovery_pad" : "clear_floor";
				feasible = capacity >= 1;
			}
		}
		room.cardFeasible = feasible;
	}
	AccessibilityKeyedDoorApproaches = keyedEdges.Size() * 2;
	AccessibilityPadReservations = navigationPadReservations;
	AccessibilityCorridorReservations = navigationCorridorReservations;
	AccessibilityReservations = navigationReservations.Size();
	if (AccessibilityPadReservations + AccessibilityCorridorReservations !=
		AccessibilityReservations)
	{
		LastError = "The collision navigation reservation accounting is inconsistent";
		return false;
	}
	AccessibilityProofPassed = true;

	// Resolve every visible wall part after all sectors, doors, stairs, and
	// landmark loops exist. This makes transform choice independent of emission
	// order and lets each side use the active IWAD's logical texture metrics.
	TArray<AlignmentSurfaceInfo> alignmentSurfaceInfo;
	alignmentSurfaceInfo.Resize(sides.Size() * 3);
	TArray<bool> alignmentStairSectors;
	alignmentStairSectors.Resize(sectors.Size());
	for (unsigned int index = 0; index < alignmentStairSectors.Size(); ++index)
		alignmentStairSectors[index] = false;
	for (unsigned int stairIndex = 0; stairIndex < stairConnections.Size(); ++stairIndex)
	{
		const StairConnection& staircase = stairConnections[stairIndex];
		for (unsigned int sectorIndex = 0; sectorIndex < staircase.sectors.Size(); ++sectorIndex)
		{
			const int sector = staircase.sectors[sectorIndex];
			if (sector >= 0 && sector < (int)alignmentStairSectors.Size())
				alignmentStairSectors[sector] = true;
		}
	}
	auto TextureNameHash = [](const FString& texture) -> uint32_t
	{
		uint32_t hash = 2166136261u;
		for (const char* character = texture.GetChars(); *character; ++character)
		{
			hash ^= (uint8_t)*character;
			hash *= 16777619u;
		}
		return hash;
	};
	auto MixAlignmentHash = [](uint32_t value) -> uint32_t
	{
		value ^= value >> 16;
		value *= 0x7feb352du;
		value ^= value >> 15;
		value *= 0x846ca68bu;
		value ^= value >> 16;
		return value;
	};
	auto PositivePhase = [](int64_t value, int width) -> double
	{
		if (width <= 0) return 0.0;
		int64_t phase = value % width;
		if (phase < 0) phase += width;
		return (double)phase;
	};

	for (unsigned int lineIndex = 0; lineIndex < lines.Size(); ++lineIndex)
	{
		const BuildLine& line = lines[lineIndex];
		if (line.sideFront < 0 || line.sideFront >= (int)sides.Size() ||
			line.v1 < 0 || line.v1 >= (int)vertices.Size() ||
			line.v2 < 0 || line.v2 >= (int)vertices.Size())
		{
			alignmentTransformsValid = false;
			continue;
		}

		auto AlignSide = [&](int sideIndex, int oppositeSector, bool reverse)
		{
			if (sideIndex < 0 || sideIndex >= (int)sides.Size())
			{
				alignmentTransformsValid = false;
				return;
			}
			BuildSide& side = sides[sideIndex];
			if (side.sector < 0 || side.sector >= (int)sectors.Size())
			{
				alignmentTransformsValid = false;
				return;
			}

			const BuildVertex& start = vertices[reverse ? line.v2 : line.v1];
			const BuildVertex& end = vertices[reverse ? line.v1 : line.v2];
			const double dx = end.x - start.x;
			const double dy = end.y - start.y;
			const double lineLength = hypot(dx, dy);
			if (lineLength < 0.001)
			{
				alignmentTransformsValid = false;
				return;
			}

			const bool hasFittedDoor = side.topAlignment == TAM_DoorFit ||
				side.midAlignment == TAM_DoorFit || side.bottomAlignment == TAM_DoorFit;
			// Ordinary bands always use explicit per-part panning. The legacy
			// shared offset survives only for fitted moving/cache door artwork,
			// whose existing UDMF contract intentionally crops all bands together.
			if (!hasFittedDoor)
			{
				side.offsetX = 0;
				side.offsetY = 0;
			}

			auto AlignSurface = [&](FString& texture, TextureAlignmentMode requestedMode,
				double& offsetX, double& offsetY, double& scaleX, double& scaleY,
				int part)
			{
				AlignmentSurfaceInfo& info = alignmentSurfaceInfo[sideIndex * 3 + part];
				info = AlignmentSurfaceInfo();
				info.line = (int)lineIndex;
				info.side = sideIndex;
				info.part = part;
				info.alignmentGroup = side.alignmentGroup;
				info.reverse = reverse;
				info.twoSided = line.sideBack >= 0;
				if (texture.Compare("-") == 0) return;
				const char* fallback = requestedMode == TAM_PanelFit ? "SW1COMP" :
					(line.special == 12 ? "BIGDOOR1" : "STARTAN3");
				TextureMetric metric = ResolveWallMetric(texture, fallback);
				if (metric.width <= 0 || metric.height <= 0)
				{
					alignmentTransformsValid = false;
					return;
				}
				info.resolved = true;
				info.texture = texture;
				info.width = metric.width;
				info.height = metric.height;

				TextureAlignmentMode mode = requestedMode;
				// Short diagonal chamfers and isolated trim wedges are the one
				// place where centered native art reads better than a global phase.
				if (mode == TAM_World && fabs(dx) > 0.001 && fabs(dy) > 0.001 &&
					lineLength <= 96.0)
					mode = TAM_Centered;
				info.mode = mode;
				auto CommitInfo = [&]()
				{
					// Ordinary parts compensate for the legacy side-wide pan during
					// serialization, while fitted door/panel art intentionally owns it.
					// Store the final effective UDMF value either way.
					const bool fitted = mode == TAM_DoorFit || mode == TAM_PanelFit;
					info.offsetX = fitted ? side.offsetX : offsetX;
					info.offsetY = fitted ? side.offsetY : offsetY;
					info.scaleX = scaleX;
					info.scaleY = scaleY;
				};

				if (mode == TAM_DoorFit)
				{
					const BuildSector& thisSector = sectors[side.sector];
					double lowFloor = thisSector.floorZ;
					double highCeiling = thisSector.ceilZ;
					if (oppositeSector >= 0 && oppositeSector < (int)sectors.Size())
					{
						lowFloor = std::min(lowFloor, sectors[oppositeSector].floorZ);
						highCeiling = std::max(highCeiling, sectors[oppositeSector].ceilZ);
					}
					const double faceHeight = std::max(1.0, highCeiling - lowFloor);
					// Secret and remote-cache faces intentionally retain the historic
					// 128x128 slab contract: they may borrow a room-wall material and
					// are visually composed as a compact disguised panel. Ordinary
					// Door_Raise faces use the resolved logical size of their selected
					// stock door texture.
					const int fitWidth = (line.secret || line.special != 12) ?
						128 : metric.width;
					const int fitHeight = (line.secret || line.special != 12) ?
						128 : metric.height;
					// Door faces narrower than their art keep the familiar centered
					// crop.  Wider physical apertures intentionally repeat the native
					// texture at scale 1, but they must still be centered as a pattern
					// rather than always beginning at texture coordinate zero.  The old
					// clamp made every 176/224-unit gallery door expose an arbitrary
					// left-hand tile fragment, which is especially conspicuous on the
					// asymmetric BIGDOOR/SPCDOOR faces.  A signed phase works for both
					// cases: at the door midpoint it lands on the midpoint of a native
					// tile, so the repeat is symmetric at both jambs.  Use the resolved
					// active-IWAD display width rather than the historic 128-unit
					// assumption; secret/cache panels retain their deliberate 128-wide
					// disguised-panel contract above.
					side.offsetX = (int)lround(((double)fitWidth - lineLength) * 0.5);
					side.offsetY = 0;
					scaleX = 1.0;
					scaleY = std::min(1.0, (double)fitHeight / faceHeight);
					CommitInfo();
					return;
				}
				if (mode == TAM_PanelFit)
				{
					const double wallHeight = std::max(1.0,
						sectors[side.sector].ceilZ - sectors[side.sector].floorZ);
					side.offsetX = 0;
					side.offsetY = 0;
					scaleX = 1.0;
					scaleY = (double)metric.height / wallHeight;
					CommitInfo();
					return;
				}

				// Follow the line's own forward direction. Collinear wall pieces
				// therefore meet at the same modulo phase even when their texture
				// dimensions differ; back sides use the reversed direction below.
				double phaseOrigin;
				if (fabs(dx) >= fabs(dy))
					phaseOrigin = dx >= 0.0 ? start.x : -start.x;
				else
					phaseOrigin = dy >= 0.0 ? start.y : -start.y;
				int64_t phase = (int64_t)llround(phaseOrigin);
				info.phaseOrigin = phase;
				if (mode == TAM_Architectural)
				{
					// Connector/stair shells deliberately outlive sector splits.  Use the
					// planner's edge key when available so every tread, landing, and
					// corridor return shares a single architectural phase. Other explicit
					// architectural forms retain a local sector fallback.
					const uint32_t phaseGroup = side.alignmentGroup >= 0 ?
						(uint32_t)side.alignmentGroup :
						(uint32_t)(side.sector + 1) * 0x9e3779b9u;
					const uint32_t groupHash = MixAlignmentHash(
						phaseGroup ^
						TextureNameHash(texture) ^ (uint32_t)(part + 1) * 0x85ebca6bu);
					info.phaseShift = (int)(groupHash % (uint32_t)metric.width);
					phase += info.phaseShift;
					alignmentArchitecturalParts++;
				}
				else if (mode == TAM_Centered)
				{
					offsetX = ((double)metric.width - lineLength) * 0.5;
					offsetY = 0.0;
					scaleX = 1.0;
					scaleY = 1.0;
					CommitInfo();
					alignmentCenteredParts++;
					return;
				}
				else
				{
					alignmentWorldParts++;
				}

				offsetX = PositivePhase(phase, metric.width);
				// The line's pegging flags provide the vertical anchor: middle
				// bands on solid walls stay at their lower floor, while two-sided
				// upper/bottom bands remain tied to their relevant ceiling/floor.
				// Keeping the shared pan at zero prevents one band from dragging
				// another away from that local architectural anchor.
				offsetY = 0.0;
				scaleX = 1.0;
				scaleY = 1.0;
				CommitInfo();
			};

			AlignSurface(side.top, side.topAlignment, side.offsetXTop, side.offsetYTop,
				side.scaleXTop, side.scaleYTop, 0);
			AlignSurface(side.middle, side.midAlignment, side.offsetXMid, side.offsetYMid,
				side.scaleXMid, side.scaleYMid, 1);
			AlignSurface(side.bottom, side.bottomAlignment,
				side.offsetXBottom, side.offsetYBottom, side.scaleXBottom,
				side.scaleYBottom, 2);
		};

		const int frontOther = line.sideBack >= 0 && line.sideBack < (int)sides.Size() ?
			sides[line.sideBack].sector : -1;
		AlignSide(line.sideFront, frontOther, false);
		if (line.sideBack >= 0)
			AlignSide(line.sideBack, sides[line.sideFront].sector, true);
	}

	// Preserve the active-IWAD metric cache and a bounded, independently
	// checkable sample of final transforms. The phase assertion covers every
	// world/architectural surface, not only the compact sample written to JSON.
	VisualProofAlignmentMetrics.Clear();
	VisualProofAlignmentFallbackTextures = 0;
	for (unsigned int index = 0; index < textureMetricCache.Size(); ++index)
	{
		const TextureMetricCacheEntry& entry = textureMetricCache[index];
		ProcGenAlignmentMetric metric;
		metric.texture = entry.name;
		metric.width = entry.metric.width;
		metric.height = entry.metric.height;
		metric.fallback = entry.metric.usedFallback;
		VisualProofAlignmentMetrics.Push(std::move(metric));
		if (entry.metric.usedFallback)
			VisualProofAlignmentFallbackTextures++;
	}
	VisualProofAlignmentWitnesses.Clear();
	VisualProofAlignmentWorldWitnesses = 0;
	VisualProofAlignmentTwoSidedWitnesses = 0;
	VisualProofAlignmentStairWitnesses = 0;
	VisualProofAlignmentPortalWitnesses = 0;
	bool alignmentPhaseContinuityValid = true;
	auto NormalizedPhase = [](double value, int width) -> double
	{
		if (width <= 0) return 0.0;
		double phase = fmod(value, (double)width);
		if (phase < 0.0) phase += width;
		return phase;
	};
	auto WitnessCountForKind = [&](const char* kind) -> int&
	{
		if (strcmp(kind, "two_sided") == 0)
			return VisualProofAlignmentTwoSidedWitnesses;
		if (strcmp(kind, "stair") == 0)
			return VisualProofAlignmentStairWitnesses;
		if (strcmp(kind, "portal") == 0)
			return VisualProofAlignmentPortalWitnesses;
		return VisualProofAlignmentWorldWitnesses;
	};
	for (unsigned int index = 0; index < alignmentSurfaceInfo.Size(); ++index)
	{
		const AlignmentSurfaceInfo& info = alignmentSurfaceInfo[index];
		if (!info.resolved || info.width <= 0 || info.height <= 0)
			continue;
		if (info.mode != TAM_World && info.mode != TAM_Architectural)
			continue;
		if (info.line < 0 || info.line >= (int)lines.Size() ||
			info.side < 0 || info.side >= (int)sides.Size())
		{
			alignmentPhaseContinuityValid = false;
			continue;
		}

		int expectedShift = 0;
		if (info.mode == TAM_Architectural)
		{
			const uint32_t phaseGroup = info.alignmentGroup >= 0 ?
				(uint32_t)info.alignmentGroup :
				(uint32_t)(sides[info.side].sector + 1) * 0x9e3779b9u;
			expectedShift = (int)(MixAlignmentHash(phaseGroup ^
				TextureNameHash(info.texture) ^
				(uint32_t)(info.part + 1) * 0x85ebca6bu) % (uint32_t)info.width);
		}
		const double expectedPhase = PositivePhase(info.phaseOrigin + expectedShift,
			info.width);
		if (expectedShift != info.phaseShift ||
			fabs(NormalizedPhase(info.offsetX, info.width) - expectedPhase) > 0.001)
		{
			alignmentPhaseContinuityValid = false;
			continue;
		}

		const BuildLine& line = lines[info.line];
		bool stairSurface = false;
		if (line.sideFront >= 0 && line.sideFront < (int)sides.Size())
		{
			const int sector = sides[line.sideFront].sector;
			stairSurface |= sector >= 0 && sector < (int)alignmentStairSectors.Size() &&
				alignmentStairSectors[sector];
		}
		if (line.sideBack >= 0 && line.sideBack < (int)sides.Size())
		{
			const int sector = sides[line.sideBack].sector;
			stairSurface |= sector >= 0 && sector < (int)alignmentStairSectors.Size() &&
				alignmentStairSectors[sector];
		}
		// The public witness set represents planned connector/stair runs. Local
		// architectural fallbacks without a stable run key are still checked by
		// the exhaustive in-engine phase assertion above, but do not pretend to
		// be a cross-sector phase group in the manifest.
		if (info.mode == TAM_Architectural && info.alignmentGroup < 0)
			continue;
		auto AddWitness = [&](const char* kind)
		{
			int& kindCount = WitnessCountForKind(kind);
			// Eight records per category give the test a useful spread across a
			// large map without turning dumpprocmanifest into a sidedef dump.
			if (kindCount >= 8) return;

			ProcGenAlignmentWitness witness;
			witness.kind = kind;
			witness.texture = info.texture;
			witness.part = info.part == 0 ? "top" : (info.part == 1 ? "mid" : "bottom");
			witness.mode = info.mode == TAM_Architectural ? "architectural" : "world";
			witness.verticalAnchor = info.part == 0 ?
				(line.dontPegTop ? "upper_ceiling" : "upper_band") :
				(line.dontPegBottom ? "lower_floor" : "lower_band");
			witness.line = info.line;
			witness.side = info.side;
			witness.width = info.width;
			witness.height = info.height;
			witness.alignmentGroup = info.alignmentGroup;
			witness.phaseOrigin = info.phaseOrigin;
			witness.phaseShift = info.phaseShift;
			witness.reverse = info.reverse;
			witness.twoSided = info.twoSided;
			witness.offsetX = info.offsetX;
			witness.offsetY = info.offsetY;
			witness.scaleX = info.scaleX;
			witness.scaleY = info.scaleY;
			VisualProofAlignmentWitnesses.Push(std::move(witness));
			kindCount++;
		};
		if (info.mode == TAM_Architectural)
		{
			// A two-sided tread or portal band proves two independent contracts:
			// its wall-band relationship and the stable phase of the planned
			// architectural run. Keep a witness for each rather than letting the
			// two-sided category hide all stair/portal evidence on a compact map.
			AddWitness(stairSurface ? "stair" : "portal");
			if (info.twoSided) AddWitness("two_sided");
		}
		else if (info.twoSided)
		{
			AddWitness("two_sided");
		}
		else
		{
			AddWitness("world");
		}
	}
	if (!alignmentPhaseContinuityValid)
		alignmentTransformsValid = false;
	const int alignedPartCount = alignmentWorldParts + alignmentArchitecturalParts +
		alignmentCenteredParts;
	if (!alignmentTransformsValid ||
		(alignedPartCount > 0 && alignmentMetricTextures == 0) ||
		alignmentFallbackTextures > alignmentMetricTextures)
	{
		LastError.Format("Could not resolve valid per-part wall transforms (%d metrics, %d fallbacks)",
			alignmentMetricTextures, alignmentFallbackTextures);
		return false;
	}

	// Serialize the visual contract only after every emitted sector, connector,
	// and sidedef has survived its concrete proof.  These checks deliberately
	// inspect the final shell/reference grid rather than trusting the earlier
	// recipe plan, so dumpprocmanifest cannot advertise an unrealized grand hall
	// or extreme terrace after a safe fallback.
	bool geometryProven = vertices.Size() > 0 && sectors.Size() > 0 && lines.Size() > 0;
	FString geometryProofFailure;
	// A claimed unified envelope is stronger than a contour label: it may host
	// an optional interior platform, but it may never retain a wall on one of the
	// old cell-to-cell faces. Checking those concrete face coordinates catches a
	// future cell-shell/corridor-side-wall regression without mistaking authored
	// landmark furniture for a disconnected chamber pod.
	auto UnifiedEnvelopeInternalCellFaceLine = [&](const BuildLine& line,
		const RoomInfo& room) -> bool
	{
		if (line.v1 < 0 || line.v1 >= (int)vertices.Size() ||
			line.v2 < 0 || line.v2 >= (int)vertices.Size())
			return true;
		const BuildVertex& first = vertices[line.v1];
		const BuildVertex& second = vertices[line.v2];
		const double epsilon = 0.01;
		for (int cellY = room.minJ; cellY <= room.maxJ; ++cellY)
		{
			for (int cellX = room.minI; cellX <= room.maxI; ++cellX)
			{
				if (cellX < 0 || cellX >= W || cellY < 0 || cellY >= H ||
					!Grid[cellY][cellX].present || Grid[cellY][cellX].roomId != room.id)
					continue;
				if (cellX + 1 <= room.maxI && Grid[cellY][cellX + 1].present &&
					Grid[cellY][cellX + 1].roomId == room.id)
				{
					const double firstFace = CellCenterX(cellX) + EdgeForCell(cellX, cellY, DIR_E);
					const double secondFace = CellCenterX(cellX + 1) - EdgeForCell(cellX + 1, cellY, DIR_W);
					if ((fabs(first.x - firstFace) <= epsilon && fabs(second.x - firstFace) <= epsilon) ||
						(fabs(first.x - secondFace) <= epsilon && fabs(second.x - secondFace) <= epsilon))
						return true;
				}
				if (cellY + 1 <= room.maxJ && Grid[cellY + 1][cellX].present &&
					Grid[cellY + 1][cellX].roomId == room.id)
				{
					const double firstFace = CellCenterY(cellY) + EdgeForCell(cellX, cellY, DIR_S);
					const double secondFace = CellCenterY(cellY + 1) - EdgeForCell(cellX, cellY + 1, DIR_N);
					if ((fabs(first.y - firstFace) <= epsilon && fabs(second.y - firstFace) <= epsilon) ||
						(fabs(first.y - secondFace) <= epsilon && fabs(second.y - secondFace) <= epsilon))
						return true;
				}
			}
		}
		return false;
	};
	for (unsigned int ri = 0; ri < Rooms.Size(); ++ri)
	{
		const RoomInfo& room = Rooms[ri];
		if (room.contourVertices < 3 || room.contourEdges < 3 ||
			room.contourArea <= 0.0 || room.contourWidth < 1.0 ||
			room.contourHeight < 1.0)
		{
			geometryProven = false;
			geometryProofFailure.Format("room %d has invalid contour metrics", room.id);
			break;
		}
		if (!room.contourUnified) continue;
		if (room.contourLoops != 1 || room.emittedContourSector != room.sectorIdx ||
			room.emittedContourMinX >= room.emittedContourMaxX ||
			room.emittedContourMinY >= room.emittedContourMaxY)
		{
			geometryProven = false;
			geometryProofFailure.Format("unified room %d has invalid exterior metadata", room.id);
			break;
		}
		for (unsigned int lineIndex = 0; lineIndex < lines.Size(); ++lineIndex)
		{
			const BuildLine& line = lines[lineIndex];
			auto SideUsesRoom = [&](int sideIndex) -> bool
			{
				return sideIndex >= 0 && sideIndex < (int)sides.Size() &&
					sides[sideIndex].sector == room.emittedContourSector;
			};
			if (!SideUsesRoom(line.sideFront) && !SideUsesRoom(line.sideBack)) continue;
			if (UnifiedEnvelopeInternalCellFaceLine(line, room))
			{
				geometryProven = false;
				const BuildVertex& first = vertices[line.v1];
				const BuildVertex& second = vertices[line.v2];
				geometryProofFailure.Format("unified room %d retains cell-face line %d (%.0f,%.0f)-(%.0f,%.0f), sectors=%d/%d special=%d",
					room.id, (int)lineIndex, first.x, first.y, second.x, second.y,
					line.sideFront >= 0 && line.sideFront < (int)sides.Size() ? sides[line.sideFront].sector : -1,
					line.sideBack >= 0 && line.sideBack < (int)sides.Size() ? sides[line.sideBack].sector : -1,
					line.special);
				break;
			}
		}
		if (!geometryProven) break;
	}
	bool connectorsProven = true;
	bool elevationProven = true;
	FString connectorProofFailure;
	TArray<int> realizedAlignmentGroups;
	for (int y = 0; y < H; ++y)
	{
		for (int x = 0; x < W; ++x)
		{
			if (!Grid[y][x].present) continue;
			for (int direction : { DIR_E, DIR_S })
			{
				if (!Grid[y][x].conn[direction]) continue;
				const int nx = x + DX[direction];
				const int ny = y + DY[direction];
				if (nx < 0 || nx >= W || ny < 0 || ny >= H ||
					!Grid[ny][nx].present) continue;
				const ProcGenCell& first = Grid[y][x];
				const ProcGenCell& second = Grid[ny][nx];
				const int opposite = OPP[direction];
				const ConnectionRef& ref = connectionGrid[y][x].refs[direction];
				const bool external = first.roomId != second.roomId;
				const bool keyed = first.lockStage != second.lockStage ||
					(first.isLocked && (first.lockDir < 0 || first.lockDir == direction)) ||
					(second.isLocked && (second.lockDir < 0 || second.lockDir == opposite));
				const bool stair = first.connectionStairChain[direction] >= 0 ||
					second.connectionStairChain[opposite] >= 0 ||
					fabs(first.floorZ - second.floorZ) > 0.001;
				const bool mandatory = keyed || stair ||
					(first.onMainPath && second.onMainPath) || first.hasPlayerStart ||
					first.hasKey || first.hasExit || second.hasPlayerStart ||
					second.hasKey || second.hasExit;
				if (external)
				{
					const bool fittedOptionalDoor = ref.door && !keyed;
					const EProcGenConnectionProfile profile =
						(EProcGenConnectionProfile)first.connectionProfile[direction];
					const int expectedWidth = ConnectionWidthForProfile(profile);
					const int expectedDepth = ConnectionDepthForProfile(profile);
					const double centerDistance = direction == DIR_E ?
						CellCenterX(nx) - CellCenterX(x) :
						CellCenterY(ny) - CellCenterY(y);
					const double actualDepth = centerDistance - EdgeForCell(x, y, direction) -
						EdgeForCell(nx, ny, opposite);
					if (ref.sector < 0 || (!fittedOptionalDoor &&
						(ref.halfWidth * 2.0 + 0.001 < expectedWidth ||
						 actualDepth + 0.001 < expectedDepth ||
						 (mandatory && profile == PGCP_Narrow))))
					{
						if (connectorProofFailure.IsEmpty())
						{
							connectorProofFailure.Format(
								"edge (%d,%d)-(%d,%d): profile=%d expected=%dx%d actual=%.0fx%.0f sector=%d door=%d mandatory=%d",
								x, y, nx, ny, (int)profile, expectedWidth, expectedDepth,
								ref.halfWidth * 2.0, actualDepth, ref.sector,
								ref.door ? 1 : 0, mandatory ? 1 : 0);
						}
						connectorsProven = false;
					}
					if (ref.door && fabs(first.floorZ - second.floorZ) > 0.001)
						elevationProven = false;
				}
				const int rise = (int)lround(second.floorZ - first.floorZ);
				if ((rise % 8) != 0 || abs(rise) > 64) elevationProven = false;
				const int alignmentGroup = first.connectionAlignmentGroup[direction] >= 0 ?
					first.connectionAlignmentGroup[direction] :
					second.connectionAlignmentGroup[opposite];
				if (alignmentGroup >= 0)
				{
					bool seen = false;
					for (unsigned int index = 0; index < realizedAlignmentGroups.Size(); ++index)
						if (realizedAlignmentGroups[index] == alignmentGroup) seen = true;
					if (!seen) realizedAlignmentGroups.Push(alignmentGroup);
				}
			}
		}
	}
	VisualProofAlignment = alignmentTransformsValid;
	VisualProofGeometry = geometryProven;
	VisualProofConnector = connectorsProven;
	VisualProofElevation = elevationProven;
	VisualProofAlignmentGroups = realizedAlignmentGroups.Size();
	VisualProofPassed = VisualProofAlignment && VisualProofGeometry &&
		VisualProofConnector && VisualProofElevation;
	if (!VisualProofPassed)
	{
		LastError.Format("Could not prove emitted visual geometry (alignment=%d geometry=%d connector=%d elevation=%d%s%s%s%s)",
			VisualProofAlignment ? 1 : 0, VisualProofGeometry ? 1 : 0,
			VisualProofConnector ? 1 : 0, VisualProofElevation ? 1 : 0,
			connectorProofFailure.IsEmpty() ? "" : ": ",
			connectorProofFailure.GetChars(), geometryProofFailure.IsEmpty() ? "" : "; ",
			geometryProofFailure.GetChars());
		return false;
	}

	FString& output = UDMFBuffer;
	output = "namespace = \"zdoom\";\n\n";

	for (const auto& vertex : vertices)
	{
		output.AppendFormat(
			"vertex\n{\n\tx = %.2f;\n\ty = %.2f;\n}\n\n",
			vertex.x, vertex.y);
	}

	for (const auto& sector : sectors)
	{
		output.AppendFormat(
			"sector\n{\n\theightfloor = %.0f;\n\theightceiling = %.0f;\n"
			"\ttexturefloor = \"%s\";\n\ttextureceiling = \"%s\";\n\tlightlevel = %d;\n",
			sector.floorZ, sector.ceilZ, sector.floorTex.GetChars(),
			sector.ceilTex.GetChars(), sector.light);
		if (sector.id > 0) output.AppendFormat("\tid = %d;\n", sector.id);
		if (sector.special > 0) output.AppendFormat("\tspecial = %d;\n", sector.special);
		if (sector.lightColor != 0xffffff)
			output.AppendFormat("\tlightcolor = %d;\n", sector.lightColor);
		if (sector.fadeColor != 0)
			output.AppendFormat("\tfadecolor = %d;\n", sector.fadeColor);
		if (sector.damageAmount > 0)
		{
			output.AppendFormat("\tdamageamount = %d;\n", sector.damageAmount);
			output.AppendFormat("\tdamageinterval = %d;\n", sector.damageInterval);
			if (!sector.damageType.IsEmpty())
				output.AppendFormat("\tdamagetype = \"%s\";\n", sector.damageType.GetChars());
			if (sector.leakiness > 0)
				output.AppendFormat("\tleakiness = %d;\n", sector.leakiness);
			if (sector.damageTerrainEffect)
				output += "\tdamageterraineffect = true;\n";
		}
		output += "}\n\n";
	}

	auto AppendSurfaceTransform = [&](const char* suffix, const FString& texture,
		TextureAlignmentMode mode, const BuildSide& side, double offsetX,
		double offsetY, double scaleX, double scaleY)
	{
		if (texture.Compare("-") == 0) return;
		// A fitted door uses the legacy shared crop so stock door validation and
		// engine door behavior remain intact. Every other surface compensates for
		// any shared value, yielding the requested final per-part phase after the
		// UDMF loader adds its legacy offset.
		if (mode != TAM_DoorFit && mode != TAM_PanelFit)
		{
			const double partOffsetX = offsetX - side.offsetX;
			const double partOffsetY = offsetY - side.offsetY;
			if (fabs(partOffsetX) > 0.000001)
				output.AppendFormat("\toffsetx_%s = %.6f;\n", suffix, partOffsetX);
			if (fabs(partOffsetY) > 0.000001)
				output.AppendFormat("\toffsety_%s = %.6f;\n", suffix, partOffsetY);
		}
		if (fabs(scaleX - 1.0) > 0.000001)
			output.AppendFormat("\tscalex_%s = %.6f;\n", suffix, scaleX);
		if (fabs(scaleY - 1.0) > 0.000001)
			output.AppendFormat("\tscaley_%s = %.6f;\n", suffix, scaleY);
	};

	for (const auto& side : sides)
	{
		output.AppendFormat("sidedef\n{\n\tsector = %d;\n", side.sector);
		if (side.top.Compare("-") != 0) output.AppendFormat("\ttexturetop = \"%s\";\n", side.top.GetChars());
		if (side.middle.Compare("-") != 0) output.AppendFormat("\ttexturemiddle = \"%s\";\n", side.middle.GetChars());
		if (side.bottom.Compare("-") != 0) output.AppendFormat("\ttexturebottom = \"%s\";\n", side.bottom.GetChars());
		AppendSurfaceTransform("top", side.top, side.topAlignment, side,
			side.offsetXTop, side.offsetYTop, side.scaleXTop, side.scaleYTop);
		AppendSurfaceTransform("mid", side.middle, side.midAlignment, side,
			side.offsetXMid, side.offsetYMid, side.scaleXMid, side.scaleYMid);
		AppendSurfaceTransform("bottom", side.bottom, side.bottomAlignment, side,
			side.offsetXBottom, side.offsetYBottom, side.scaleXBottom,
			side.scaleYBottom);
		output.AppendFormat("\toffsetx = %d;\n\toffsety = %d;\n}\n\n", side.offsetX, side.offsetY);
	}

	for (const auto& line : lines)
	{
		output.AppendFormat(
			"linedef\n{\n\tv1 = %d;\n\tv2 = %d;\n\tsidefront = %d;\n",
			line.v1, line.v2, line.sideFront);
		if (line.sideBack >= 0)
		{
			output.AppendFormat("\tsideback = %d;\n\ttwosided = true;\n", line.sideBack);
		}
		if (line.blocking) output += "\tblocking = true;\n";
		if (line.dontPegTop) output += "\tdontpegtop = true;\n";
		if (line.dontPegBottom) output += "\tdontpegbottom = true;\n";
		if (line.playerUse) output += "\tplayeruse = true;\n";
		if (line.playerCross) output += "\tplayercross = true;\n";
		if (line.repeatSpecial) output += "\trepeatspecial = true;\n";
		if (line.blockMonsters) output += "\tblockmonsters = true;\n";
		if (line.secret) output += "\tsecret = true;\n";
		if (line.special > 0)
		{
			output.AppendFormat("\tspecial = %d;\n", line.special);
			for (int arg = 0; arg < 5; arg++)
				output.AppendFormat("\targ%d = %d;\n", arg, line.args[arg]);
		}
		if (line.lockNumber > 0) output.AppendFormat("\tlocknumber = %d;\n", line.lockNumber);
		output += "}\n\n";
	}

	for (const auto& thing : things)
	{
		output.AppendFormat(
			"thing\n{\n\tx = %.2f;\n\ty = %.2f;\n\tangle = %d;\n\ttype = %d;\n",
			thing.x, thing.y, thing.angle, thing.type);
		if (thing.ambush) output += "\tambush = true;\n";
		output += "\tskill1 = true;\n\tskill2 = true;\n\tskill3 = true;\n"
			"\tskill4 = true;\n\tskill5 = true;\n\tsingle = true;\n"
			"\tcoop = true;\n\tdm = true;\n}\n\n";
	}

	return vertices.Size() > 0 && sectors.Size() > 0 && lines.Size() > 0;
}
