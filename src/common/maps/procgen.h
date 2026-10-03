#pragma once

#include "tarray.h"
#include "zstring.h"
#include "m_random.h"
#include <stdint.h>

struct MapData;

// The blueprint is deliberately recipe-derived rather than sampled from the
// generator RNG. This makes a run's identity inspectable without perturbing
// the random stream used to embed its actual geometry.
enum EProcGenRunProfile
{
	PGRP_Expedition,
	PGRP_Assault,
	PGRP_Infiltration,
	PGRP_Circuit,
	PGRP_Siege,
};

enum EProcGenRouteOrientation
{
	PGRO_Eastbound,
	PGRO_Westbound,
	PGRO_Northbound,
	PGRO_Southbound,
};

enum EProcGenRunBeat
{
	PGRB_None,
	PGRB_Opening,
	PGRB_Approach,
	PGRB_KeyObjective,
	PGRB_Recovery,
	PGRB_SetPiece,
	PGRB_Finale,
	PGRB_Optional,
};

enum EProcGenEncounterCard
{
	PGEC_None,
	PGEC_Breather,
	PGEC_Skirmish,
	PGEC_Crossfire,
	PGEC_Pincer,
	PGEC_Ambush,
	PGEC_CacheChallenge,
	PGEC_HoldingLine,
	PGEC_SetPiece,
};

enum EProcGenFeatureMotif
{
	PGFM_None,
	PGFM_Watercourse,
	PGFM_VerticalPressure,
	PGFM_RemoteReveal,
	PGFM_ShrineSecrets,
	PGFM_SightlineRecon,
};

enum EProcGenArsenalTrack
{
	PGAT_Ballistic,
	PGAT_Demolition,
	PGAT_Energy,
};

enum EProcGenFinaleCard
{
	PGFC_None,
	PGFC_Duel,
	PGFC_Siege,
	PGFC_Gauntlet,
	PGFC_Fortress,
};

// A stage is the portion of the mandatory route between two key boundaries.
// These values deliberately describe topology rather than decoration: the
// layout pass can fall back to Spine when the coarse grid cannot safely hold
// a more elaborate form without making a new crossing between lock stages.
enum EProcGenStageShape
{
	PGSS_Spine,
	PGSS_ForkRejoin,
	PGSS_Ring,
	PGSS_Switchback,
	PGSS_CourtyardSpokes,
};

// Landmark forms are shared by every theme. The room/UDMF passes turn them
// into theme-local architecture (for example a Techbase command court or a
// Hell blood chapel) using only the active IWAD's assets.
enum EProcGenLandmarkArchetype
{
	PGLA_None,
	PGLA_Court,
	PGLA_Nave,
	PGLA_Gatehouse,
	PGLA_ShrineTerrace,
	PGLA_BridgeBasin,
	PGLA_Bastion,
	PGLA_Fortress,
};

// Districts are intentionally semantic instead of material names. This lets
// each theme read as several connected places without hard-coding assets into
// the mission planner.
enum EProcGenDistrictRole
{
	PGDR_Entry,
	PGDR_Transit,
	PGDR_Work,
	PGDR_Sanctum,
	PGDR_Defense,
	PGDR_Finale,
};

// A vertical intent belongs to a safe main-route connector. Geometry owns its
// concrete stairs and landing shape, but the planner owns the required route
// cadence and rise so cosmetic perches cannot satisfy that requirement.
enum EProcGenVerticalIntent
{
	PGVI_Flat,
	PGVI_StairHall,
	PGVI_DoglegAscent,
	PGVI_DoglegDescent,
	PGVI_TerraceOverlook,
	PGVI_BridgeApproach,
};

// A footprint is a room-owned spatial grammar, not a late decoration choice.
// The UDMF pass may use SafeShell when a requested contour cannot preserve all
// portal spans and navigation reservations, but it must never replace a safe
// footprint with a less-clear decorative form.
enum EProcGenRoomFootprint
{
	PGRF_SafeShell,
	PGRF_AsymmetricOctagon,
	PGRF_TaperedBay,
	PGRF_Apse,
	PGRF_SteppedCompound,
	PGRF_CourtyardCut,
	PGRF_FracturedWedge,
};

// These are full clear opening dimensions.  The geometry pass owns the exact
// portal polygon, but cannot silently make a narrow optional connector into a
// mandatory choke point or shrink a planned keyed/stair approach.
enum EProcGenConnectionProfile
{
	PGCP_Narrow,
	PGCP_Standard,
	PGCP_Gallery,
	PGCP_Grand,
};

// Material families give every theme a handful of recognizable districts.
// They intentionally describe compatible IWAD-safe treatments rather than
// individual texture names; room composition resolves the concrete palette.
enum EProcGenMaterialFamily
{
	PGMF_None,
	PGMF_TechAirlock,
	PGMF_TechCommandCourt,
	PGMF_TechReactorWell,
	PGMF_TechServiceSpine,
	PGMF_IndustrialLoadingBay,
	PGMF_IndustrialRefinery,
	PGMF_IndustrialFoundry,
	PGMF_IndustrialServiceDogleg,
	PGMF_HellBloodChapel,
	PGMF_HellChasmBridge,
	PGMF_HellRitualPit,
	PGMF_HellAshCitadel,
	PGMF_GothicGatehouse,
	PGMF_GothicNave,
	PGMF_GothicCloister,
	PGMF_GothicThroneCourt,
	PGMF_CorruptedContainment,
	PGMF_CorruptedBreachTerrace,
	PGMF_CorruptedHellCore,
	PGMF_CorruptedQuarantine,
};

// A planned terrain role exposes the readable large-scale elevation field in
// the manifest without confusing it with the single edge-level stair intent.
enum EProcGenElevationRole
{
	PGER_Flat,
	PGER_Terrace,
	PGER_Highland,
	PGER_Basin,
	PGER_Overlook,
};

// Only explicit player actions are exposed here. Keys remain inventory
// requirements for the ordinary manual Door_Raise crossings; no key pickup
// ever becomes a walkover trigger or a hidden automatic interaction.
enum EProcGenManualInteraction
{
	PGMI_None,
	PGMI_KeyedDoor,
	PGMI_SwitchCache,
	PGMI_SecretDoor,
};

enum EProcGenRewardPlan
{
	PGRW_None,
	PGRW_Emergency,
	PGRW_Cache,
	PGRW_Armory,
	PGRW_KeyReserve,
	PGRW_FinaleReserve,
};

struct RunBlueprint
{
	static constexpr int MaxStages = 4;

	uint32_t RecipeHash = 0;
	EProcGenRunProfile Profile = PGRP_Expedition;
	EProcGenRouteOrientation Orientation = PGRO_Eastbound;
	int KeyOrder[3] = { 2, 1, 3 }; // blue, red, yellow
	int GateRankJitter = 0;
	int RouteLengthPercent = 100;
	int BranchCountPercent = 100;
	int BranchRankBias = 0; // -1 early, 0 broad, 1 late
	int LoopQuotaPercent = 100;
	int LoopRankBias = 0;   // -1 early, 0 broad, 1 late
	int HubRankPercent = 33;
	int ArenaRankPercent = 67;
	int DistrictCount = 2;
	int MotifCount = 2;
	EProcGenFeatureMotif Motifs[3] = { PGFM_None, PGFM_None, PGFM_None };
	EProcGenArsenalTrack ArsenalTrack = PGAT_Ballistic;
	EProcGenFinaleCard FinaleCard = PGFC_Duel;
	int ThreatCurve = 0;
	int RecoveryCadence = 2;

	// These are recipe-derived planning inputs. The realized values are updated
	// only after the grid proves that a requested shape/vertical connector fits;
	// none of them consume the shared layout RNG stream.
	int PlannedStageCount = 2;
	int RealizedStageCount = 2;
	int StageWeights[MaxStages] = { 100, 100, 100, 100 };
	EProcGenStageShape StageShapes[MaxStages] = {
		PGSS_Spine, PGSS_Spine, PGSS_Spine, PGSS_Spine
	};
	EProcGenStageShape RealizedStageShapes[MaxStages] = {
		PGSS_Spine, PGSS_Spine, PGSS_Spine, PGSS_Spine
	};
	EProcGenLandmarkArchetype StageLandmarks[MaxStages] = {
		PGLA_Court, PGLA_Court, PGLA_Court, PGLA_Fortress
	};
	EProcGenLandmarkArchetype RealizedStageLandmarks[MaxStages] = {
		PGLA_Court, PGLA_Court, PGLA_Court, PGLA_Fortress
	};
	EProcGenDistrictRole StageDistrictRoles[MaxStages] = {
		PGDR_Entry, PGDR_Transit, PGDR_Defense, PGDR_Finale
	};
	EProcGenMaterialFamily StageMaterialFamilies[MaxStages] = {
		PGMF_TechAirlock, PGMF_TechCommandCourt, PGMF_TechReactorWell,
		PGMF_TechServiceSpine
	};
	EProcGenElevationRole StageElevationRoles[MaxStages] = {
		PGER_Flat, PGER_Flat, PGER_Flat, PGER_Flat
	};
	// Large terrain targets are expressed as absolute world-height intentions.
	// Individual walking edges remain independently bounded to 64 units.
	int MainRouteElevationTarget = 0;
	int OptionalElevationTarget = 0;
	int RealizedMainRouteElevation = 0;
	int RealizedOptionalElevation = 0;
	EProcGenVerticalIntent StageVerticalIntents[MaxStages] = {
		PGVI_Flat, PGVI_Flat, PGVI_Flat, PGVI_Flat
	};
	EProcGenVerticalIntent RealizedStageVerticalIntents[MaxStages] = {
		PGVI_Flat, PGVI_Flat, PGVI_Flat, PGVI_Flat
	};
	int StageVerticalRises[MaxStages] = { 0, 0, 0, 0 };
	// Keep the recipe plan immutable after generation. A short stage can move a
	// beat to a later safe stage, which belongs in realization data rather than
	// silently rewriting the profile plan used by a subsequent deterministic run.
	int RealizedStageVerticalRises[MaxStages] = { 0, 0, 0, 0 };
	int StageGateRanks[MaxStages - 1] = { -1, -1, -1 };
	int RequiredVerticalBeats = 0;
	int RealizedVerticalBeats = 0;
};

struct RoomInfo
{
	int id = -1;
	int minI = 0, maxI = 0; // inclusive column bounds
	int minJ = 0, maxJ = 0; // inclusive row bounds
	int cellCount = 0;
	int sectorIdx = -1;
	double floorZ = 0.0;
	double ceilZ = 128.0;
	FString floorTex;
	FString ceilTex;
	FString wallTex;
	FString accentTex;
	FString detailTex;
	double halfWidth = 176.0;
	double halfHeight = 176.0;
	double cornerCut = 24.0;
	int visualVariant = 0;
	// Internal composition descriptors. Spatial class is 0=connector, 1=small,
	// 2=medium, 3=major; shape family is 0=compact, 1=horizontal,
	// 2=vertical, 3=compound/bent. They are serialized only through geometry.
	int spatialClass = 1;
	int shapeFamily = 0;
	int light = 160;
	int lightColor = 0xffffff;
	int fadeColor = 0;
	bool hasPlayerStart = false;
	bool hasExit = false;
	bool hasBoss = false;
	bool hasKey = false;
	int keyType = 0;
	bool isLocked = false;
	int lockType = 0;
	int enemyCount = 0;
	int monsterTier = 1;
	bool hasWeapon = false;
	int weaponType = 0;
	bool hasAmmo = false;
	int ammoType = 0;
	int ammoCount = 0;
	bool hasHealth = false;
	int healthType = 0;
	int healthCount = 0;
	int healthBonusCount = 0;
	bool hasArmor = false;
	int armorType = 0;
	TArray<int> powerups; // progression-aware optional/secret rewards
	int distFromStart = 0; // BFS distance from player start
	int progressionRank = 9999;
	int lockStage = 0;
	int branchDepth = 0;
	bool isDeadEnd = false; // room has only one connection to other rooms
	bool reservedSecret = false; // planner-owned leaf that must remain a secret
	bool isSecret = false;  // optional dead-end reward hidden behind a secret door
	bool hasDoor = false;   // entrance has a door (monster closet)
	bool onMainPath = false;
	bool isArena = false;
	bool isHub = false;
	// Blueprint-owned gameplay planning. These values are copied from the
	// coarse cells before encounter and reward composition.
	int runBeat = PGRB_None;
	int encounterCard = PGEC_None;
	int featureMotif = PGFM_None;
	int featureMotifPriority = 0;
	int district = 0;
	int stageShape = PGSS_Spine;
	int landmarkArchetype = PGLA_None;
	int districtRole = PGDR_Entry;
	int materialFamily = PGMF_None;
	// `footprint` is the hash-planned grammar. The UDMF pass records the
	// actually emitted grammar separately: a constrained compound may take the
	// safe shell rather than advertising an apse/courtyard it could not build.
	int footprint = PGRF_SafeShell;
	int realizedFootprint = PGRF_SafeShell;
	int footprintVariant = 0;
	int contourInset = 0;
	bool footprintFallback = false;
	// A true room envelope owns one exterior loop and has no cell-to-cell pod
	// walls inside it. Legacy-safe shells retain their measured contour data but
	// are explicitly distinguishable in dumpprocmanifest.
	bool contourUnified = false;
	int contourLoops = 0;
	// The final UDMF sector and exterior bounds are emitted alongside the
	// contour facts. They let dumpprocmanifest consumers independently inspect
	// a claimed unified envelope instead of trusting a planner-only label.
	int emittedContourSector = -1;
	double emittedContourMinX = 0.0;
	double emittedContourMaxX = 0.0;
	double emittedContourMinY = 0.0;
	double emittedContourMaxY = 0.0;
	// These are measured from the final serialized chamber shell, rather than
	// inferred from the coarse room bounding box.  They make it possible to
	// distinguish a true asymmetric/concave composition from a palette-only
	// footprint label in dumpprocmanifest.
	int contourVertices = 0;
	int contourEdges = 0;
	double contourArea = 0.0;
	double contourWidth = 0.0;
	double contourHeight = 0.0;
	int elevationRole = PGER_Flat;
	int elevationTarget = 0;
	// The optional dramatic opposite extreme must remain an ordinary reachable
	// district.  Secret-door normalization intentionally levels hidden rooms to
	// their neighboring terrace, so the anchor is protected from that later pass.
	bool optionalTerrainAnchor = false;
	// A large Dramatic run may reserve an existing optional limb as a sequence
	// of real, open stair terraces.  These rooms must not be rejoined through a
	// cosmetic normal door before the terrain solver has assigned the opposite
	// highland/basin; keyed and other critical terraces retain their usual
	// level-component contract.
	bool terrainRouteReservation = false;
	int verticalIntent = PGVI_Flat;
	int verticalRise = 0;
	bool verticalAnchor = false;
	int manualInteraction = PGMI_None;
	int threatBudget = 0;
	int recoveryBudget = 0;
	bool optionalArmory = false;
	int arsenalTrack = PGAT_Ballistic;
	int finaleCard = PGFC_None;
	int rewardPlan = PGRW_None;
	// Filled from the post-emission UDMF proof. These retain the concrete
	// static capacity and interaction evidence behind a card rather than
	// exposing only the planner's label in the run manifest.
	bool cardFeasible = false;
	FString cardGeometry;
	int cardCapacity = 0;
	int cardStaticEnemies = 0;
	int cardManualActions = 0;
};

struct ProcGenCell
{
	bool present = false;
	bool conn[4] = { false, false, false, false }; // N, S, W, E
	// Each open edge is planned before UDMF emission. The mirrored neighbor owns
	// identical values, allowing serialization and geometry to reason about a
	// connector without depending on grid scan order.
	int connectionProfile[4] = { PGCP_Standard, PGCP_Standard, PGCP_Standard, PGCP_Standard };
	int connectionClearWidth[4] = { 128, 128, 128, 128 };
	int connectionDepth[4] = { 64, 64, 64, 64 };
	int connectionAlignmentGroup[4] = { -1, -1, -1, -1 };
	int connectionStairChain[4] = { -1, -1, -1, -1 };
	// Realized by the emitter. The profile is the physical clear opening even
	// when a fitted manual door is present; `connectionDoorArtWidth` describes
	// only the native stock panel, never a reduced player aperture.
	bool connectionHasDoor[4] = { false, false, false, false };
	bool connectionSecretDoor[4] = { false, false, false, false };
	int connectionDoorArtWidth[4] = { 0, 0, 0, 0 };
	int connectionDoorArtHeight[4] = { 0, 0, 0, 0 };
	int sectorIdx = -1;
	int roomId = -1;
	int neighborCount = 0;  // present neighbors (geometric adjacency)
	int connectionCount = 0; // open connections
	double floorZ = 0.0;
	double ceilZ = 128.0;
	FString floorTex;
	FString ceilTex;
	FString wallTex;
	int light = 160;
	bool hasPlayerStart = false;
	bool hasExit = false;
	bool hasBoss = false;
	bool hasKey = false;
	int keyType = 0;    // 1=red, 2=blue, 3=yellow
	bool isLocked = false;
	int lockType = 0;   // 1=red, 2=blue, 3=yellow
	int lockDir = -1;   // boundary direction that owns the lock; -1 = legacy/all
	int enemyCount = 0;
	int monsterTier = 1;
	bool hasWeapon = false;
	int weaponType = 0;
	bool hasAmmo = false;
	int ammoType = 0;
	bool hasHealth = false;
	int healthType = 0;
	bool hasArmor = false;
	int armorType = 0;
	int pathRank = -1;
	int lockStage = 0;
	int branchDepth = 0;
	bool onMainPath = false;
	bool isArena = false;
	bool isHub = false;
	bool reservedSecret = false; // protected optional-branch endpoint
	int runBeat = PGRB_None;
	int encounterCard = PGEC_None;
	int featureMotif = PGFM_None;
	int featureMotifPriority = 0;
	int district = 0;
	int stageShape = PGSS_Spine;
	int landmarkArchetype = PGLA_None;
	int districtRole = PGDR_Entry;
	int materialFamily = PGMF_None;
	int footprint = PGRF_SafeShell;
	int elevationRole = PGER_Flat;
	int elevationTarget = 0;
	// Pre-merge counterpart of RoomInfo::terrainRouteReservation.  It keeps a
	// selected same-stage optional limb atomic so its required multi-flight
	// terrain chain cannot disappear into one composed chamber.
	bool terrainRouteReservation = false;
	int verticalIntent = PGVI_Flat;
	int verticalRise = 0;
	bool verticalAnchor = false;
	int threatBudget = 0;
	int recoveryBudget = 0;
	bool optionalArmory = false;
	int arsenalTrack = PGAT_Ballistic;
	int finaleCard = PGFC_None;
	int rewardPlan = PGRW_None;
};

// The UDMF emitter resolves wall dimensions from the active texture manager.
// Keep a compact copy of that post-emission evidence in the manifest so the
// regression harness can validate native-size phase transforms without
// assuming a universal 128-unit texture tile.
struct ProcGenAlignmentMetric
{
	FString texture;
	int width = 0;
	int height = 0;
	bool fallback = false;
};

// A bounded sample of final sidedef transforms. The record deliberately uses
// serialized UDMF indices, so the validator can independently recompute the
// phase from real vertices, the active-IWAD metric, and the alignment mode.
// It is diagnostic-only and never feeds back into generation.
struct ProcGenAlignmentWitness
{
	FString kind;
	FString texture;
	FString part;
	FString mode;
	FString verticalAnchor;
	int line = -1;
	int side = -1;
	int width = 0;
	int height = 0;
	int alignmentGroup = -1;
	int64_t phaseOrigin = 0;
	int phaseShift = 0;
	bool reverse = false;
	bool twoSided = false;
	double offsetX = 0.0;
	double offsetY = 0.0;
	double scaleX = 1.0;
	double scaleY = 1.0;
};

// A compact post-emission navigation witness.  These deliberately refer to
// stable serialized things and linedefs, rather than trying to expose every
// merged room contour as a pseudo navigation mesh.  The generator still
// proves every corridor internally; this bounded record lets the manifest
// and regression suite independently verify the player-critical anchors.
struct ProcGenAccessibilityAnchor
{
	FString kind;
	int thingIndex = -1;
	int line = -1;
	int side = -1;
	int lock = 0;
	int tag = 0;
	double x = 0.0;
	double y = 0.0;
	double clearRadius = 0.0;
};

// A final collision lane owned by one proven coarse-cell connection.  A
// room-merge lane intentionally has no portal linedef: its two cells share a
// sector only after the emitter has kept this explicit clear capsule.  The
// manifest retains that distinction so tooling can audit a real open gap
// rather than accepting same-sector adjacency as proof.
struct ProcGenAccessibilityLane
{
	double x1 = 0.0;
	double y1 = 0.0;
	double x2 = 0.0;
	double y2 = 0.0;
	double clearRadius = 0.0;
};

struct ProcGenAccessibilityCorridor
{
	FString kind;
	int sourceCell = -1;
	int targetCell = -1;
	int sourceSector = -1;
	int targetSector = -1;
	int connectorSector = -1;
	int doorSector = -1;
	// Direct same-sector room merges correctly have no portal linedef.  Other
	// emitted connections retain their two approach lines when one exists.
	int sourceLine = -1;
	int targetLine = -1;
	int reservationCount = 0;
	TArray<ProcGenAccessibilityLane> lanes;
};

// A bounded witness for an optional switch cache.  It names the only manual
// action that may open the closed tagged sector and the static reward things
// that become reachable afterward.
struct ProcGenAccessibilitySwitchCache
{
	int tag = 0;
	int switchLine = -1;
	int sourceSector = -1;
	int cacheSector = -1;
	int doorSector = -1;
	int closetSector = -1;
	int sourceDoorLine = -1;
	int closetDoorLine = -1;
	double sourceDoorClearRadius = 0.0;
	double closetDoorClearRadius = 0.0;
	int rewardFirstThing = -1;
	int rewardThingCount = 0;
};

class FProceduralMapGenerator
{
public:
	static constexpr int MinMapSize = 1;
	// The engine accepts coordinates through +/-262144 (MAX_MAP_COORD in
	// doomdef.h); the generator keeps its own wider safety margin below.
	static constexpr int MaxMapSize = 160;
	static constexpr int DefaultMapSize = 3;
	static constexpr int DefaultStyleSetting = 1;

	FProceduralMapGenerator();

	void SetSeed(int seed);
	void SetTheme(const char* theme);
	void SetDifficulty(int difficulty); // 1-5
	void SetSize(int size);             // 1-160, affects grid dimensions
	void SetLayout(int layout);         // 0=directed, 1=balanced, 2=exploratory
	void SetVerticality(int verticality); // 0=gentle, 1=varied, 2=dramatic
	void SetDetail(int detail);         // 0=sparse, 1=detailed, 2=lavish
	void SetOutdoors(int outdoors);     // 0=enclosed, 1=mixed, 2=open
	int GetSeed() const { return Seed; }
	const FString& GetTheme() const { return Theme; }
	int GetDifficulty() const { return Difficulty; }
	int GetSize() const { return Size; }
	int GetLayout() const { return Layout; }
	int GetVerticality() const { return Verticality; }
	int GetDetail() const { return Detail; }
	int GetOutdoors() const { return Outdoors; }
	const FString& GetRunProfile() const;
	const FString& GetRunBriefing() const;
	const FString& GetRunManifest() const;
	EProcGenRunProfile GetRunProfileKind() const;
	EProcGenArsenalTrack GetArsenalTrackKind() const;
	EProcGenFinaleCard GetFinaleCardKind() const;
	const RunBlueprint& GetRunBlueprint() const;

	bool Generate();
	const FString& GetUDMFText() const { return UDMFBuffer; }
	const char* GetLastError() const { return LastError.GetChars(); }

	static FProceduralMapGenerator& GetInstance();

private:
	bool BuildUDMF(int W, int H);
	void MergeRooms(int W, int H);
	void PlaceWeapons(int W, int H);
	void ApplyCoherence(int W, int H);
	void InvalidateBlueprint();
	void RefreshBlueprint() const;

	FRandom RNG;
	int Seed;
	FString Theme;
	int Difficulty;
	int Size;
	int Layout;
	int Verticality;
	int Detail;
	int Outdoors;
	mutable RunBlueprint Blueprint;
	mutable bool BlueprintValid = false;
	mutable FString RunProfileText;
	mutable FString RunBriefingText;
	mutable FString RunManifestText;
	uint32_t GeneratedRecipeHash = 0;
	bool HasGeneratedBlueprint = false;
	// Filled by the post-emission traversal in BuildUDMF. These values are
	// deliberately diagnostic-only: they expose the hard accessibility contract
	// in the manifest without introducing another generation setting.
	bool AccessibilityProofPassed = false;
	int AccessibilityMandatoryAnchors = 0;
	int AccessibilityOrdinaryCells = 0;
	int AccessibilityKeyedDoorApproaches = 0;
	int AccessibilityReservations = 0;
	int AccessibilityPadReservations = 0;
	int AccessibilityCorridorReservations = 0;
	int AccessibilityProvenCorridors = 0;
	int AccessibilityKeyStateEdges = 0;
	int AccessibilityManualSwitches = 0;
	int AccessibilityOrdinaryRewards = 0;
	int AccessibilityRequiredKeyMask = 0;
	int AccessibilityExitKeyMask = 0;
	TArray<ProcGenAccessibilityAnchor> AccessibilityAnchors;
	int AccessibilityRoomMergeCorridors = 0;
	int AccessibilitySwitchCacheActions = 0;
	int AccessibilitySwitchCacheRewards = 0;
	TArray<ProcGenAccessibilityCorridor> AccessibilityCorridors;
	TArray<ProcGenAccessibilitySwitchCache> AccessibilitySwitchCaches;
	// Visual proof is kept alongside the accessibility proof because both are
	// post-emission contracts.  It is diagnostic data only: it never feeds back
	// into the recipe or consumes the layout RNG stream.
	bool VisualProofPassed = false;
	bool VisualProofAlignment = false;
	bool VisualProofGeometry = false;
	bool VisualProofConnector = false;
	bool VisualProofElevation = false;
	int VisualProofAlignmentGroups = 0;
	int VisualProofAlignmentFallbackTextures = 0;
	int VisualProofAlignmentWorldWitnesses = 0;
	int VisualProofAlignmentTwoSidedWitnesses = 0;
	int VisualProofAlignmentStairWitnesses = 0;
	int VisualProofAlignmentPortalWitnesses = 0;
	TArray<ProcGenAlignmentMetric> VisualProofAlignmentMetrics;
	TArray<ProcGenAlignmentWitness> VisualProofAlignmentWitnesses;

	TArray<TArray<ProcGenCell>> Grid;
	TArray<RoomInfo> Rooms;
	FString UDMFBuffer;
	FString LastError;

	static FProceduralMapGenerator Instance;
};

// A savegame carries both the deterministic recipe and the exact generated
// TEXTMAP. Keeping the TEXTMAP makes procedural saves resilient to later
// generator changes, while the recipe remains available for diagnostics and
// subsequent regeneration.
struct FProceduralMapArchiveData
{
	int Seed = 0;
	FString Theme;
	int Difficulty = 3;
	int Size = FProceduralMapGenerator::DefaultMapSize;
	int Layout = FProceduralMapGenerator::DefaultStyleSetting;
	int Verticality = FProceduralMapGenerator::DefaultStyleSetting;
	int Detail = FProceduralMapGenerator::DefaultStyleSetting;
	int Outdoors = FProceduralMapGenerator::DefaultStyleSetting;
	FString UDMF;
};

MapData* P_OpenProceduralMapData(const char* mapname);
bool P_IsProceduralMapName(const char* mapname);
FString P_GetProceduralMusic();
const FProceduralMapArchiveData* P_GetCurrentProceduralMapArchive();

// A completed procedural map can be replayed from the menu with a fresh seed
// while retaining its exact seven non-seed recipe settings. The completion
// marker is deliberately runtime-only: savegames continue to restore their
// archived TEXTMAP rather than silently turning a restore into a new run.
void P_MarkCurrentProceduralMapCompleted();
bool P_PrepareNextProceduralMap();

bool P_StageProceduralMapArchive(int seed, const char* theme, int difficulty,
	int size, int layout, int verticality, int detail, int outdoors, FString udmf);
