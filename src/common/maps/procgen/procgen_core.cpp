/*
** procgen_core.cpp
**
** Mission-graph-first procedural map generation. The coarse grid is only an
** embedding surface: progression, optional branches, locks, loops, and
** landmarks are planned before rooms and UDMF geometry are built.
**
**---------------------------------------------------------------------------
*/

#include "procgen_internal.h"

using namespace ProcGen;

namespace
{

// Keep recipe planning completely independent of FRandom. Apart from making
// previews cheap, this is important because adding a blueprint field must not
// silently move every later draw in the geometry stream.
uint32_t MixProcGenHash(uint32_t value)
{
	value ^= value >> 16;
	value *= 0x7feb352du;
	value ^= value >> 15;
	value *= 0x846ca68bu;
	value ^= value >> 16;
	return value;
}

uint32_t HashProcGenByte(uint32_t hash, uint8_t value)
{
	hash ^= value;
	hash *= 16777619u;
	return hash;
}

uint32_t HashProcGenInt(uint32_t hash, int value)
{
	const uint32_t bits = (uint32_t)value;
	for (int shift = 0; shift < 32; shift += 8)
		hash = HashProcGenByte(hash, (uint8_t)(bits >> shift));
	return hash;
}

uint32_t MakeRecipeHash(int seed, const FString& theme, int difficulty,
	int size, int layout, int verticality, int detail, int outdoors)
{
	uint32_t hash = 2166136261u;
	hash = HashProcGenInt(hash, seed);
	for (const char* text = theme.GetChars(); *text != 0; ++text)
		hash = HashProcGenByte(hash, (uint8_t)*text);
	// Delimit the variable-length theme before feeding the numeric recipe.
	hash = HashProcGenByte(hash, 0xffu);
	hash = HashProcGenInt(hash, difficulty);
	hash = HashProcGenInt(hash, size);
	hash = HashProcGenInt(hash, layout);
	hash = HashProcGenInt(hash, verticality);
	hash = HashProcGenInt(hash, detail);
	hash = HashProcGenInt(hash, outdoors);
	return MixProcGenHash(hash);
}

uint32_t BlueprintChannel(uint32_t recipeHash, uint32_t channel)
{
	return MixProcGenHash(recipeHash ^ (0x9e3779b9u * (channel + 1u)));
}

const char* RunProfileName(EProcGenRunProfile profile)
{
	switch (profile)
	{
	case PGRP_Assault: return "Assault";
	case PGRP_Infiltration: return "Infiltration";
	case PGRP_Circuit: return "Circuit";
	case PGRP_Siege: return "Siege";
	default: return "Expedition";
	}
}

const char* RunBriefingFor(EProcGenRunProfile profile)
{
	switch (profile)
	{
	case PGRP_Assault:
		return "Breach the line: pressure rises quickly, with relief between set pieces.";
	case PGRP_Infiltration:
		return "Slip through guarded districts: ambushes, shortcuts, and hidden rewards matter.";
	case PGRP_Circuit:
		return "Close the circuit: loops and cross-routes turn every key into a route choice.";
	case PGRP_Siege:
		return "Hold the advance: fortified arenas and recovery caches lead to a hard finale.";
	default:
		return "Chart the outskirts: optional caches and long routes reward exploration.";
	}
}

const char* RouteOrientationName(EProcGenRouteOrientation orientation)
{
	switch (orientation)
	{
	case PGRO_Westbound: return "westbound";
	case PGRO_Northbound: return "northbound";
	case PGRO_Southbound: return "southbound";
	default: return "eastbound";
	}
}

const char* EncounterCardName(int card)
{
	switch (card)
	{
	case PGEC_Breather: return "breather";
	case PGEC_Skirmish: return "skirmish";
	case PGEC_Crossfire: return "crossfire";
	case PGEC_Pincer: return "pincer";
	case PGEC_Ambush: return "ambush";
	case PGEC_CacheChallenge: return "cache_challenge";
	case PGEC_HoldingLine: return "holding_line";
	case PGEC_SetPiece: return "set_piece";
	default: return "none";
	}
}

const char* RunBeatName(int beat)
{
	switch (beat)
	{
	case PGRB_Opening: return "opening";
	case PGRB_Approach: return "approach";
	case PGRB_KeyObjective: return "key_objective";
	case PGRB_Recovery: return "recovery";
	case PGRB_SetPiece: return "set_piece";
	case PGRB_Finale: return "finale";
	case PGRB_Optional: return "optional";
	default: return "none";
	}
}

const char* FeatureMotifName(int motif)
{
	switch (motif)
	{
	case PGFM_Watercourse: return "watercourse";
	case PGFM_VerticalPressure: return "vertical_pressure";
	case PGFM_RemoteReveal: return "remote_reveal";
	case PGFM_ShrineSecrets: return "shrine_secrets";
	case PGFM_SightlineRecon: return "sightline_recon";
	default: return "none";
	}
}

const char* ArsenalTrackName(EProcGenArsenalTrack track)
{
	switch (track)
	{
	case PGAT_Demolition: return "demolition";
	case PGAT_Energy: return "energy";
	default: return "ballistic";
	}
}

const char* FinaleCardName(EProcGenFinaleCard finale)
{
	switch (finale)
	{
	case PGFC_Duel: return "duel";
	case PGFC_Siege: return "siege";
	case PGFC_Gauntlet: return "gauntlet";
	case PGFC_Fortress: return "fortress";
	default: return "none";
	}
}

const char* StageShapeName(EProcGenStageShape shape)
{
	switch (shape)
	{
	case PGSS_ForkRejoin: return "fork_rejoin";
	case PGSS_Ring: return "ring";
	case PGSS_Switchback: return "switchback";
	case PGSS_CourtyardSpokes: return "courtyard_spokes";
	default: return "spine";
	}
}

const char* LandmarkArchetypeName(EProcGenLandmarkArchetype archetype)
{
	switch (archetype)
	{
	case PGLA_Court: return "court";
	case PGLA_Nave: return "nave";
	case PGLA_Gatehouse: return "gatehouse";
	case PGLA_ShrineTerrace: return "shrine_terrace";
	case PGLA_BridgeBasin: return "bridge_basin";
	case PGLA_Bastion: return "bastion";
	case PGLA_Fortress: return "fortress";
	default: return "none";
	}
}

const char* DistrictRoleName(EProcGenDistrictRole role)
{
	switch (role)
	{
	case PGDR_Transit: return "transit";
	case PGDR_Work: return "work";
	case PGDR_Sanctum: return "sanctum";
	case PGDR_Defense: return "defense";
	case PGDR_Finale: return "finale";
	default: return "entry";
	}
}

const char* VerticalIntentName(EProcGenVerticalIntent intent)
{
	switch (intent)
	{
	case PGVI_StairHall: return "stair_hall";
	case PGVI_DoglegAscent: return "dogleg_ascent";
	case PGVI_DoglegDescent: return "dogleg_descent";
	case PGVI_TerraceOverlook: return "terrace_overlook";
	case PGVI_BridgeApproach: return "bridge_approach";
	default: return "flat";
	}
}

const char* RoomFootprintName(EProcGenRoomFootprint footprint)
{
	switch (footprint)
	{
	case PGRF_AsymmetricOctagon: return "asymmetric_octagon";
	case PGRF_TaperedBay: return "tapered_bay";
	case PGRF_Apse: return "apse";
	case PGRF_SteppedCompound: return "stepped_compound";
	case PGRF_CourtyardCut: return "courtyard_cut";
	case PGRF_FracturedWedge: return "fractured_wedge";
	default: return "safe_shell";
	}
}

const char* ConnectionProfileName(EProcGenConnectionProfile profile)
{
	switch (profile)
	{
	case PGCP_Narrow: return "narrow";
	case PGCP_Gallery: return "gallery";
	case PGCP_Grand: return "grand";
	default: return "standard";
	}
}

const char* MaterialFamilyName(EProcGenMaterialFamily family)
{
	switch (family)
	{
	case PGMF_TechAirlock: return "tech_airlock";
	case PGMF_TechCommandCourt: return "tech_command_court";
	case PGMF_TechReactorWell: return "tech_reactor_well";
	case PGMF_TechServiceSpine: return "tech_service_spine";
	case PGMF_IndustrialLoadingBay: return "industrial_loading_bay";
	case PGMF_IndustrialRefinery: return "industrial_refinery";
	case PGMF_IndustrialFoundry: return "industrial_foundry";
	case PGMF_IndustrialServiceDogleg: return "industrial_service_dogleg";
	case PGMF_HellBloodChapel: return "hell_blood_chapel";
	case PGMF_HellChasmBridge: return "hell_chasm_bridge";
	case PGMF_HellRitualPit: return "hell_ritual_pit";
	case PGMF_HellAshCitadel: return "hell_ash_citadel";
	case PGMF_GothicGatehouse: return "gothic_gatehouse";
	case PGMF_GothicNave: return "gothic_nave";
	case PGMF_GothicCloister: return "gothic_cloister";
	case PGMF_GothicThroneCourt: return "gothic_throne_court";
	case PGMF_CorruptedContainment: return "corrupted_containment";
	case PGMF_CorruptedBreachTerrace: return "corrupted_breach_terrace";
	case PGMF_CorruptedHellCore: return "corrupted_hell_core";
	case PGMF_CorruptedQuarantine: return "corrupted_quarantine";
	default: return "none";
	}
}

const char* ElevationRoleName(EProcGenElevationRole role)
{
	switch (role)
	{
	case PGER_Terrace: return "terrace";
	case PGER_Highland: return "highland";
	case PGER_Basin: return "basin";
	case PGER_Overlook: return "overlook";
	default: return "flat";
	}
}

int ConnectionClearWidth(EProcGenConnectionProfile profile)
{
	switch (profile)
	{
	case PGCP_Narrow: return 96;
	case PGCP_Gallery: return 176;
	case PGCP_Grand: return 224;
	default: return 128;
	}
}

int ConnectionDepth(EProcGenConnectionProfile profile)
{
	switch (profile)
	{
	case PGCP_Narrow: return 48;
	case PGCP_Gallery: return 96;
	case PGCP_Grand: return 128;
	default: return 64;
	}
}

EProcGenMaterialFamily PickMaterialFamily(ThemeStyle theme, EProcGenRunProfile profile,
	int stage, int stageCount, EProcGenLandmarkArchetype landmark, uint32_t choice)
{
	// Each group is intentionally four contiguous values. The room pass can use
	// the group-relative variant as a palette family without knowing the theme
	// string or consuming a geometry RNG draw.
	const int variant = (int)(choice % 4u);
	const bool finale = stage >= stageCount - 1 || landmark == PGLA_Fortress;
	switch (theme)
	{
	case ThemeIndustrial:
		if (finale) return PGMF_IndustrialFoundry;
		if (landmark == PGLA_BridgeBasin) return PGMF_IndustrialRefinery;
		if (landmark == PGLA_Gatehouse) return PGMF_IndustrialLoadingBay;
		return (EProcGenMaterialFamily)(PGMF_IndustrialLoadingBay + variant);
	case ThemeHell:
		if (finale) return PGMF_HellAshCitadel;
		if (landmark == PGLA_BridgeBasin) return PGMF_HellChasmBridge;
		if (landmark == PGLA_ShrineTerrace || landmark == PGLA_Nave)
			return PGMF_HellBloodChapel;
		return (EProcGenMaterialFamily)(PGMF_HellBloodChapel + variant);
	case ThemeGothic:
		if (finale) return PGMF_GothicThroneCourt;
		if (landmark == PGLA_Gatehouse) return PGMF_GothicGatehouse;
		if (landmark == PGLA_Nave) return PGMF_GothicNave;
		return (EProcGenMaterialFamily)(PGMF_GothicGatehouse + variant);
	case ThemeCorrupted:
		if (finale) return PGMF_CorruptedHellCore;
		if (landmark == PGLA_ShrineTerrace || landmark == PGLA_BridgeBasin)
			return PGMF_CorruptedBreachTerrace;
		return (EProcGenMaterialFamily)(PGMF_CorruptedContainment + variant);
	case ThemeTechbase:
	default:
		if (finale) return PGMF_TechReactorWell;
		if (landmark == PGLA_Gatehouse) return PGMF_TechAirlock;
		if (landmark == PGLA_Court || landmark == PGLA_Nave)
			return PGMF_TechCommandCourt;
		return (EProcGenMaterialFamily)(PGMF_TechAirlock +
			((variant + (profile == PGRP_Infiltration ? 1 : 0)) % 4));
	}
}

const char* ManualInteractionName(EProcGenManualInteraction interaction)
{
	switch (interaction)
	{
	case PGMI_KeyedDoor: return "keyed_door";
	case PGMI_SwitchCache: return "switch_cache";
	case PGMI_SecretDoor: return "secret_door";
	default: return "none";
	}
}

EProcGenStageShape PickStageShape(EProcGenRunProfile profile, uint32_t choice)
{
	// Repeating the favored entries weights each profile without turning a
	// profile into a fixed topology. The last option is always Spine, which is
	// useful on compact canvases even before feasibility fallback is considered.
	static const EProcGenStageShape Expedition[] = {
		PGSS_CourtyardSpokes, PGSS_CourtyardSpokes, PGSS_ForkRejoin,
		PGSS_Ring, PGSS_Switchback, PGSS_Spine
	};
	static const EProcGenStageShape Assault[] = {
		PGSS_ForkRejoin, PGSS_ForkRejoin, PGSS_CourtyardSpokes,
		PGSS_Spine, PGSS_Switchback, PGSS_Ring
	};
	static const EProcGenStageShape Infiltration[] = {
		PGSS_Switchback, PGSS_Switchback, PGSS_ForkRejoin,
		PGSS_Ring, PGSS_CourtyardSpokes, PGSS_Spine
	};
	static const EProcGenStageShape Circuit[] = {
		PGSS_Ring, PGSS_Ring, PGSS_ForkRejoin,
		PGSS_Switchback, PGSS_CourtyardSpokes, PGSS_Spine
	};
	static const EProcGenStageShape Siege[] = {
		PGSS_Spine, PGSS_CourtyardSpokes, PGSS_ForkRejoin,
		PGSS_Spine, PGSS_Switchback, PGSS_Ring
	};

	const EProcGenStageShape* choices = Expedition;
	int count = countof(Expedition);
	switch (profile)
	{
	case PGRP_Assault: choices = Assault; count = countof(Assault); break;
	case PGRP_Infiltration: choices = Infiltration; count = countof(Infiltration); break;
	case PGRP_Circuit: choices = Circuit; count = countof(Circuit); break;
	case PGRP_Siege: choices = Siege; count = countof(Siege); break;
	default: break;
	}
	return choices[choice % (uint32_t)count];
}

EProcGenDistrictRole PickDistrictRole(EProcGenRunProfile profile, int stage,
	int stageCount, uint32_t choice)
{
	if (stage <= 0) return PGDR_Entry;
	if (stage >= stageCount - 1) return PGDR_Finale;

	static const EProcGenDistrictRole Expedition[] = { PGDR_Transit, PGDR_Sanctum, PGDR_Work };
	static const EProcGenDistrictRole Assault[] = { PGDR_Defense, PGDR_Work, PGDR_Transit };
	static const EProcGenDistrictRole Infiltration[] = { PGDR_Transit, PGDR_Sanctum, PGDR_Work };
	static const EProcGenDistrictRole Circuit[] = { PGDR_Work, PGDR_Transit, PGDR_Defense };
	static const EProcGenDistrictRole Siege[] = { PGDR_Defense, PGDR_Work, PGDR_Transit };

	const EProcGenDistrictRole* choices = Expedition;
	int count = countof(Expedition);
	switch (profile)
	{
	case PGRP_Assault: choices = Assault; count = countof(Assault); break;
	case PGRP_Infiltration: choices = Infiltration; count = countof(Infiltration); break;
	case PGRP_Circuit: choices = Circuit; count = countof(Circuit); break;
	case PGRP_Siege: choices = Siege; count = countof(Siege); break;
	default: break;
	}
	return choices[choice % (uint32_t)count];
}

EProcGenLandmarkArchetype PickLandmarkArchetype(EProcGenRunProfile profile,
	ThemeStyle theme, int stage, int stageCount, uint32_t choice)
{
	if (stage >= stageCount - 1) return PGLA_Fortress;
	if (stage == 0)
	{
		if (profile == PGRP_Assault || profile == PGRP_Siege) return PGLA_Gatehouse;
		return theme == ThemeIndustrial ? PGLA_BridgeBasin : PGLA_Court;
	}

	static const EProcGenLandmarkArchetype Expedition[] = {
		PGLA_Court, PGLA_ShrineTerrace, PGLA_BridgeBasin, PGLA_Nave
	};
	static const EProcGenLandmarkArchetype Assault[] = {
		PGLA_Gatehouse, PGLA_Bastion, PGLA_Court, PGLA_Nave
	};
	static const EProcGenLandmarkArchetype Infiltration[] = {
		PGLA_BridgeBasin, PGLA_ShrineTerrace, PGLA_Nave, PGLA_Gatehouse
	};
	static const EProcGenLandmarkArchetype Circuit[] = {
		PGLA_Nave, PGLA_Court, PGLA_BridgeBasin, PGLA_Bastion
	};
	static const EProcGenLandmarkArchetype Siege[] = {
		PGLA_Gatehouse, PGLA_Bastion, PGLA_Court, PGLA_Nave
	};

	const EProcGenLandmarkArchetype* choices = Expedition;
	int count = countof(Expedition);
	switch (profile)
	{
	case PGRP_Assault: choices = Assault; count = countof(Assault); break;
	case PGRP_Infiltration: choices = Infiltration; count = countof(Infiltration); break;
	case PGRP_Circuit: choices = Circuit; count = countof(Circuit); break;
	case PGRP_Siege: choices = Siege; count = countof(Siege); break;
	default: break;
	}

	EProcGenLandmarkArchetype result = choices[choice % (uint32_t)count];
	// Infernal maps read better when at least one middle district becomes a
	// chapel/terrace, while factory-like themes get a basin or bastion instead
	// of a texture-only version of the same court.
	if ((theme == ThemeHell || theme == ThemeGothic) && (choice & 3u) == 0u)
		result = (choice & 4u) != 0u ? PGLA_Nave : PGLA_ShrineTerrace;
	else if (theme == ThemeIndustrial && (choice & 3u) == 0u)
		result = (choice & 4u) != 0u ? PGLA_Bastion : PGLA_BridgeBasin;
	else if (theme == ThemeCorrupted && (choice & 3u) == 0u)
		result = (choice & 4u) != 0u ? PGLA_Bastion : PGLA_ShrineTerrace;
	return result;
}

const char* RewardPlanName(int reward)
{
	switch (reward)
	{
	case PGRW_Emergency: return "emergency";
	case PGRW_Cache: return "cache";
	case PGRW_Armory: return "armory";
	case PGRW_KeyReserve: return "key_reserve";
	case PGRW_FinaleReserve: return "finale_reserve";
	default: return "none";
	}
}

RunBlueprint BuildRunBlueprint(int seed, const FString& theme, int difficulty,
	int size, int layout, int verticality, int detail, int outdoors)
{
	RunBlueprint result;
	result.RecipeHash = MakeRecipeHash(seed, theme, difficulty, size, layout,
		verticality, detail, outdoors);
	result.Profile = (EProcGenRunProfile)(BlueprintChannel(result.RecipeHash, 0) % 5u);
	result.Orientation = (EProcGenRouteOrientation)(BlueprintChannel(result.RecipeHash, 1) % 4u);
	result.GateRankJitter = (int)(BlueprintChannel(result.RecipeHash, 2) % 5u) - 2;
	result.ArsenalTrack = (EProcGenArsenalTrack)(BlueprintChannel(result.RecipeHash, 3) % 3u);
	result.FinaleCard = (EProcGenFinaleCard)(PGFC_Duel +
		(BlueprintChannel(result.RecipeHash, 4) % 4u));

	int keyOrder[] = { 1, 2, 3 };
	for (int index = 2; index > 0; --index)
	{
		const int other = (int)(BlueprintChannel(result.RecipeHash, 5 + index) %
			(uint32_t)(index + 1));
		const int swap = keyOrder[index];
		keyOrder[index] = keyOrder[other];
		keyOrder[other] = swap;
	}
	for (int index = 0; index < 3; ++index) result.KeyOrder[index] = keyOrder[index];

	switch (result.Profile)
	{
	case PGRP_Assault:
		result.RouteLengthPercent = 90;
		result.BranchCountPercent = 76;
		result.BranchRankBias = 1;
		result.LoopQuotaPercent = 72;
		result.LoopRankBias = 1;
		result.HubRankPercent = 27;
		result.ArenaRankPercent = 58;
		result.ThreatCurve = 2;
		result.RecoveryCadence = 2;
		break;
	case PGRP_Infiltration:
		result.RouteLengthPercent = 104;
		result.BranchCountPercent = 112;
		result.BranchRankBias = 0;
		result.LoopQuotaPercent = 125;
		result.LoopRankBias = 0;
		result.HubRankPercent = 38;
		result.ArenaRankPercent = 69;
		result.ThreatCurve = 0;
		result.RecoveryCadence = 2;
		break;
	case PGRP_Circuit:
		result.RouteLengthPercent = 110;
		result.BranchCountPercent = 96;
		result.BranchRankBias = -1;
		result.LoopQuotaPercent = 168;
		result.LoopRankBias = 0;
		result.HubRankPercent = 31;
		result.ArenaRankPercent = 72;
		result.ThreatCurve = 1;
		result.RecoveryCadence = 2;
		break;
	case PGRP_Siege:
		result.RouteLengthPercent = 96;
		result.BranchCountPercent = 88;
		result.BranchRankBias = 1;
		result.LoopQuotaPercent = 92;
		result.LoopRankBias = 1;
		result.HubRankPercent = 24;
		result.ArenaRankPercent = 61;
		result.ThreatCurve = 3;
		result.RecoveryCadence = 1;
		break;
	default: // Expedition
		result.RouteLengthPercent = 120;
		result.BranchCountPercent = 142;
		result.BranchRankBias = 0;
		result.LoopQuotaPercent = 118;
		result.LoopRankBias = -1;
		result.HubRankPercent = 36;
		result.ArenaRankPercent = 76;
		result.ThreatCurve = 0;
		result.RecoveryCadence = 2;
		break;
	}

	// Each profile gets a small, hash-derived variation inside its authored
	// bounds without consuming the layout stream.
	result.RouteLengthPercent += (int)(BlueprintChannel(result.RecipeHash, 9) % 9u) - 4;
	result.BranchCountPercent += (int)(BlueprintChannel(result.RecipeHash, 10) % 13u) - 6;
	result.LoopQuotaPercent += (int)(BlueprintChannel(result.RecipeHash, 11) % 13u) - 6;
	result.HubRankPercent += (int)(BlueprintChannel(result.RecipeHash, 12) % 9u) - 4;
	result.ArenaRankPercent += (int)(BlueprintChannel(result.RecipeHash, 13) % 9u) - 4;
	result.DistrictCount = size >= 5 ? 3 : 2;
	result.MotifCount = size >= 5 ? 3 : 2;

	const EProcGenFeatureMotif available[] = {
		PGFM_Watercourse, PGFM_VerticalPressure, PGFM_RemoteReveal,
		PGFM_ShrineSecrets, PGFM_SightlineRecon
	};
	int motifOrder[] = { 0, 1, 2, 3, 4 };
	for (int index = 4; index > 0; --index)
	{
		const int other = (int)(BlueprintChannel(result.RecipeHash, 20 + index) %
			(uint32_t)(index + 1));
		const int swap = motifOrder[index];
		motifOrder[index] = motifOrder[other];
		motifOrder[other] = swap;
	}
	for (int index = 0; index < result.MotifCount; ++index)
		result.Motifs[index] = available[motifOrder[index]];

	// A normal run has one stage before each gate and one final stage. Keep the
	// count independent of the realized maze path; Generate records the actual
	// count if an unusually compact route can only support fewer gates.
	result.PlannedStageCount = size >= 5 ? 4 : (size >= 3 ? 3 : 2);
	result.RealizedStageCount = result.PlannedStageCount;
	const int ExpeditionWeights[] = { 108, 100, 102, 94 };
	const int AssaultWeights[] = { 92, 105, 116, 104 };
	const int InfiltrationWeights[] = { 105, 94, 111, 90 };
	const int CircuitWeights[] = { 96, 109, 95, 106 };
	const int SiegeWeights[] = { 104, 91, 106, 123 };
	const int* stageWeights = ExpeditionWeights;
	switch (result.Profile)
	{
	case PGRP_Assault: stageWeights = AssaultWeights; break;
	case PGRP_Infiltration: stageWeights = InfiltrationWeights; break;
	case PGRP_Circuit: stageWeights = CircuitWeights; break;
	case PGRP_Siege: stageWeights = SiegeWeights; break;
	default: break;
	}
	const ThemeStyle themeStyle = GetThemeStyle(theme);
	// The terrain field is planned as a broad, signed destination rather than a
	// repeating distance modulo. Room composition later walks the actual graph in
	// <=64-unit steps, so even the 320-unit dramatic horizon never asks UDMF to
	// create an impassable ledge.
	const bool terrainRises = (BlueprintChannel(result.RecipeHash, 72) & 1u) != 0u;
	const int terrainSign = terrainRises ? 1 : -1;
	if (verticality == 2)
	{
		const int magnitude = size >= 5 ?
			192 + (int)(BlueprintChannel(result.RecipeHash, 73) % 3u) * 64 :
			128 + (int)(BlueprintChannel(result.RecipeHash, 73) % 3u) * 32;
		result.MainRouteElevationTarget = terrainSign * magnitude;
		if (size >= 5)
		{
			const int opposite = 192 +
				(int)(BlueprintChannel(result.RecipeHash, 74) % 3u) * 64;
			result.OptionalElevationTarget = -terrainSign * opposite;
		}
	}
	else if (verticality == 1)
	{
		result.MainRouteElevationTarget = terrainSign *
			// Moderate terrain still promises a clearly legible vertical district.
			// The serialized-map validation requires at least 96 units of usable
			// relief; a 64-unit broad target can be softened below that by safe
			// door/cycle projection before UDMF emission.
			(96 + (int)(BlueprintChannel(result.RecipeHash, 73) % 3u) * 16);
	}
	else if (size >= 3)
	{
		result.MainRouteElevationTarget = terrainSign * 32;
	}
	const int terrainStage = result.PlannedStageCount > 1 ? clamp(
		1 + (int)(BlueprintChannel(result.RecipeHash, 75) %
			(uint32_t)(result.PlannedStageCount - 1)), 1,
		result.PlannedStageCount - 1) : 0;
	for (int stage = 0; stage < RunBlueprint::MaxStages; ++stage)
	{
		const uint32_t shapeChannel = BlueprintChannel(result.RecipeHash, 48 + stage);
		const uint32_t landmarkChannel = BlueprintChannel(result.RecipeHash, 56 + stage);
		result.StageWeights[stage] = clamp(stageWeights[stage] +
			(int)(BlueprintChannel(result.RecipeHash, 44 + stage) % 25u) - 12, 68, 140);
		result.StageShapes[stage] = stage < result.PlannedStageCount ?
			PickStageShape(result.Profile, shapeChannel) : PGSS_Spine;
		result.RealizedStageShapes[stage] = result.StageShapes[stage];
		result.StageLandmarks[stage] = PickLandmarkArchetype(result.Profile, themeStyle,
			stage, result.PlannedStageCount, landmarkChannel);
		result.RealizedStageLandmarks[stage] = result.StageLandmarks[stage];
		result.StageDistrictRoles[stage] = PickDistrictRole(result.Profile, stage,
			result.PlannedStageCount, BlueprintChannel(result.RecipeHash, 64 + stage));
		result.StageMaterialFamilies[stage] = PickMaterialFamily(themeStyle, result.Profile,
			stage, result.PlannedStageCount, result.StageLandmarks[stage],
			BlueprintChannel(result.RecipeHash, 68 + stage));
		result.StageElevationRoles[stage] = PGER_Flat;
		if (stage < result.PlannedStageCount && result.MainRouteElevationTarget != 0)
		{
			if (stage == terrainStage)
				result.StageElevationRoles[stage] = result.MainRouteElevationTarget > 0 ?
					PGER_Highland : PGER_Basin;
			else if (stage < terrainStage)
				result.StageElevationRoles[stage] = PGER_Terrace;
			else
				result.StageElevationRoles[stage] = PGER_Overlook;
		}
		result.StageVerticalIntents[stage] = PGVI_Flat;
		result.RealizedStageVerticalIntents[stage] = PGVI_Flat;
		result.StageVerticalRises[stage] = 0;
	}

	// Verticality is an authored route promise, not a late floor-noise setting.
	// Size-three maps are the smallest normal maps with enough critical route to
	// ask for it. The placements are selected against actual safe cells later.
	if (size >= 3)
	{
		result.RequiredVerticalBeats = verticality == 0 ? 1 :
			(verticality == 1 ? 2 : 3);
		// The first vertical beat establishes the route's spatial vocabulary.
		// Keep the paired Varied/Dramatic doglegs explicit (their signed order is
		// part of the fairness contract), while profiles choose whether the
		// remaining ascent reads as a straightforward stair hall, a terrace
		// overlook, or a bridge approach. This is hash-only planning and does not
		// consume the shared layout stream.
		EProcGenVerticalIntent scenicIntent = PGVI_StairHall;
		switch (result.Profile)
		{
		case PGRP_Expedition:
		case PGRP_Siege:
			scenicIntent = PGVI_TerraceOverlook;
			break;
		case PGRP_Infiltration:
		case PGRP_Circuit:
			scenicIntent = PGVI_BridgeApproach;
			break;
		case PGRP_Assault:
		default:
			break;
		}
		const EProcGenVerticalIntent Gentle[] = { scenicIntent };
		const EProcGenVerticalIntent Varied[] = { PGVI_DoglegAscent, PGVI_DoglegDescent };
		const EProcGenVerticalIntent Dramatic[] = {
			scenicIntent, PGVI_DoglegAscent, PGVI_DoglegDescent
		};
		const EProcGenVerticalIntent* intents = verticality == 0 ? Gentle :
			(verticality == 1 ? Varied : Dramatic);
		for (int beat = 0; beat < result.RequiredVerticalBeats; ++beat)
		{
			const int stage = beat;
			result.StageVerticalIntents[stage] = intents[beat];
			if (intents[beat] == PGVI_DoglegAscent)
				result.StageVerticalRises[stage] = verticality >= 2 ? 48 : 32;
			else if (intents[beat] == PGVI_DoglegDescent)
				result.StageVerticalRises[stage] = -32;
			else
				result.StageVerticalRises[stage] = 32;
		}
	}
	return result;
}

bool IsForwardDirection(EProcGenRouteOrientation orientation, int direction)
{
	return (orientation == PGRO_Eastbound && direction == DIR_E) ||
		(orientation == PGRO_Westbound && direction == DIR_W) ||
		(orientation == PGRO_Northbound && direction == DIR_N) ||
		(orientation == PGRO_Southbound && direction == DIR_S);
}

bool IsBackwardDirection(EProcGenRouteOrientation orientation, int direction)
{
	return (orientation == PGRO_Eastbound && direction == DIR_W) ||
		(orientation == PGRO_Westbound && direction == DIR_E) ||
		(orientation == PGRO_Northbound && direction == DIR_S) ||
		(orientation == PGRO_Southbound && direction == DIR_N);
}

int ForwardCoordinate(EProcGenRouteOrientation orientation, int x, int y, int W, int H)
{
	switch (orientation)
	{
	case PGRO_Westbound: return W - 1 - x;
	case PGRO_Northbound: return H - 1 - y;
	case PGRO_Southbound: return y;
	default: return x;
	}
}

int PerpendicularCoordinate(EProcGenRouteOrientation orientation, int x, int y)
{
	return (orientation == PGRO_Eastbound || orientation == PGRO_Westbound) ? y : x;
}

} // namespace

FProceduralMapGenerator FProceduralMapGenerator::Instance;

FProceduralMapGenerator& FProceduralMapGenerator::GetInstance()
{
	return Instance;
}

FProceduralMapGenerator::FProceduralMapGenerator()
	: Seed(0), Difficulty(3), Size(DefaultMapSize),
	  Layout(DefaultStyleSetting), Verticality(DefaultStyleSetting),
	  Detail(DefaultStyleSetting), Outdoors(DefaultStyleSetting)
{
	Theme = "techbase";
}

void FProceduralMapGenerator::InvalidateBlueprint()
{
	BlueprintValid = false;
	HasGeneratedBlueprint = false;
	GeneratedRecipeHash = 0;
	RunManifestText = "";
}

void FProceduralMapGenerator::RefreshBlueprint() const
{
	const uint32_t recipeHash = MakeRecipeHash(Seed, Theme, Difficulty, Size,
		Layout, Verticality, Detail, Outdoors);
	if (BlueprintValid && Blueprint.RecipeHash == recipeHash) return;

	Blueprint = BuildRunBlueprint(Seed, Theme, Difficulty, Size, Layout,
		Verticality, Detail, Outdoors);
	RunProfileText = RunProfileName(Blueprint.Profile);
	RunBriefingText = RunBriefingFor(Blueprint.Profile);
	BlueprintValid = true;
	RunManifestText = "";
}

const FString& FProceduralMapGenerator::GetRunProfile() const
{
	RefreshBlueprint();
	return RunProfileText;
}

const FString& FProceduralMapGenerator::GetRunBriefing() const
{
	RefreshBlueprint();
	return RunBriefingText;
}

EProcGenRunProfile FProceduralMapGenerator::GetRunProfileKind() const
{
	RefreshBlueprint();
	return Blueprint.Profile;
}

EProcGenArsenalTrack FProceduralMapGenerator::GetArsenalTrackKind() const
{
	RefreshBlueprint();
	return Blueprint.ArsenalTrack;
}

EProcGenFinaleCard FProceduralMapGenerator::GetFinaleCardKind() const
{
	RefreshBlueprint();
	return Blueprint.FinaleCard;
}

const RunBlueprint& FProceduralMapGenerator::GetRunBlueprint() const
{
	RefreshBlueprint();
	return Blueprint;
}

const FString& FProceduralMapGenerator::GetRunManifest() const
{
	RefreshBlueprint();
	RunManifestText = "{\n";
	RunManifestText.AppendFormat("  \"schema\": 1,\n");
	RunManifestText.AppendFormat("  \"iwad_roster\": \"%s\",\n",
		ProcGenIwadRosterName());
	RunManifestText.AppendFormat("  \"difficulty\": %d,\n", Difficulty);
	RunManifestText.AppendFormat("  \"profile\": \"%s\",\n", RunProfileName(Blueprint.Profile));
	RunManifestText.AppendFormat("  \"briefing\": \"%s\",\n", RunBriefingFor(Blueprint.Profile));
	RunManifestText.AppendFormat("  \"orientation\": \"%s\",\n", RouteOrientationName(Blueprint.Orientation));
	RunManifestText.AppendFormat("  \"key_order\": [%d, %d, %d],\n",
		Blueprint.KeyOrder[0], Blueprint.KeyOrder[1], Blueprint.KeyOrder[2]);
	const int plannedStageCount = clamp(Blueprint.PlannedStageCount, 1,
		RunBlueprint::MaxStages);
	const int stageCount = clamp(Blueprint.RealizedStageCount, 1, RunBlueprint::MaxStages);
	RunManifestText.AppendFormat("  \"planned_stage_count\": %d,\n", plannedStageCount);
	RunManifestText.AppendFormat("  \"realized_stage_count\": %d,\n", stageCount);
	RunManifestText.AppendFormat("  \"stages\": [\n");
	for (int stage = 0; stage < stageCount; ++stage)
	{
		const int gateRank = stage + 1 < stageCount ? Blueprint.StageGateRanks[stage] : -1;
		RunManifestText.AppendFormat(
			"    {\"stage\": %d, \"weight\": %d, \"shape\": \"%s\", "
			"\"realized_shape\": \"%s\", \"landmark_archetype\": \"%s\", "
			"\"realized_landmark_archetype\": \"%s\", \"district_role\": \"%s\", "
			"\"material_family\": \"%s\", \"elevation_role\": \"%s\", "
			"\"vertical_intent\": \"%s\", \"realized_vertical_intent\": \"%s\", "
			"\"vertical_rise\": %d, \"realized_vertical_rise\": %d, \"gate_rank\": %d}%s\n",
			stage, Blueprint.StageWeights[stage], StageShapeName(Blueprint.StageShapes[stage]),
			StageShapeName(Blueprint.RealizedStageShapes[stage]),
			LandmarkArchetypeName(Blueprint.StageLandmarks[stage]),
			LandmarkArchetypeName(Blueprint.RealizedStageLandmarks[stage]),
			DistrictRoleName(Blueprint.StageDistrictRoles[stage]),
			MaterialFamilyName(Blueprint.StageMaterialFamilies[stage]),
			ElevationRoleName(Blueprint.StageElevationRoles[stage]),
			VerticalIntentName(Blueprint.StageVerticalIntents[stage]),
			VerticalIntentName(Blueprint.RealizedStageVerticalIntents[stage]),
			Blueprint.StageVerticalRises[stage], Blueprint.RealizedStageVerticalRises[stage], gateRank,
			stage + 1 < stageCount ? "," : "");
	}
	RunManifestText.AppendFormat("  ],\n");
	RunManifestText.AppendFormat("  \"motifs\": [");
	for (int index = 0; index < Blueprint.MotifCount; ++index)
	{
		if (index > 0) RunManifestText.AppendFormat(", ");
		RunManifestText.AppendFormat("\"%s\"", FeatureMotifName(Blueprint.Motifs[index]));
	}
	RunManifestText.AppendFormat("],\n");
	RunManifestText.AppendFormat("  \"arsenal_track\": \"%s\",\n",
		ArsenalTrackName(Blueprint.ArsenalTrack));
	RunManifestText.AppendFormat("  \"finale\": \"%s\",\n", FinaleCardName(Blueprint.FinaleCard));
	RunManifestText.AppendFormat("  \"main_route_elevation_target\": %d,\n",
		Blueprint.MainRouteElevationTarget);
	RunManifestText.AppendFormat("  \"optional_elevation_target\": %d,\n",
		Blueprint.OptionalElevationTarget);
	RunManifestText.AppendFormat("  \"realized_main_route_elevation\": %d,\n",
		Blueprint.RealizedMainRouteElevation);
	RunManifestText.AppendFormat("  \"realized_optional_elevation\": %d,\n",
		Blueprint.RealizedOptionalElevation);
	RunManifestText.AppendFormat("  \"rooms\": [");

	const bool includeRooms = HasGeneratedBlueprint &&
		GeneratedRecipeHash == Blueprint.RecipeHash;
	if (includeRooms && Rooms.Size() > 0) RunManifestText.AppendFormat("\n");
	for (unsigned int index = 0; includeRooms && index < Rooms.Size(); ++index)
	{
		const RoomInfo& room = Rooms[index];
		RunManifestText.AppendFormat(
			"    {\"id\": %d, \"rank\": %d, \"main_path\": %s, "
			"\"beat\": \"%s\", \"card\": \"%s\", \"motif\": \"%s\", "
			"\"card_feasible\": %s, \"card_geometry\": \"%s\", "
			"\"card_capacity\": %d, \"card_static_enemies\": %d, "
			"\"card_manual_actions\": %d, "
			"\"district\": %d, \"stage\": %d, \"stage_shape\": \"%s\", "
			"\"district_role\": \"%s\", \"landmark_archetype\": \"%s\", "
				"\"material_family\": \"%s\", \"footprint\": \"%s\", "
				"\"requested_footprint\": \"%s\", \"realized_footprint\": \"%s\", "
				"\"footprint_variant\": %d, \"footprint_fallback\": %s, "
				"\"contour_unified\": %s, \"contour_loops\": %d, "
				"\"contour_sector\": %d, \"contour_bounds\": [%.2f, %.2f, %.2f, %.2f], "
				"\"contour_vertices\": %d, \"contour_edges\": %d, "
			"\"contour_area\": %.2f, \"contour_width\": %.2f, "
			"\"contour_height\": %.2f, \"floor_z\": %.0f, \"clear_height\": %.0f, "
			"\"elevation_role\": \"%s\", "
			"\"vertical_intent\": \"%s\", \"vertical_rise\": %d, "
			"\"vertical_anchor\": %s, \"manual_interaction\": \"%s\", \"threat\": %d, "
			"\"recovery\": %d, \"weapon\": %d, \"ammo\": %d, "
			"\"ammo_count\": %d, \"reward\": \"%s\", "
			"\"optional_armory\": %s, \"arsenal_track\": \"%s\", "
			"\"finale\": \"%s\"}%s\n",
			room.id, room.progressionRank, room.onMainPath ? "true" : "false",
			RunBeatName(room.runBeat), EncounterCardName(room.encounterCard),
			FeatureMotifName(room.featureMotif),
			room.cardFeasible ? "true" : "false", room.cardGeometry.GetChars(),
			room.cardCapacity, room.cardStaticEnemies, room.cardManualActions,
			room.district, room.lockStage,
			StageShapeName((EProcGenStageShape)room.stageShape),
			DistrictRoleName((EProcGenDistrictRole)room.districtRole),
			LandmarkArchetypeName((EProcGenLandmarkArchetype)room.landmarkArchetype),
				MaterialFamilyName((EProcGenMaterialFamily)room.materialFamily),
				RoomFootprintName((EProcGenRoomFootprint)room.realizedFootprint),
				RoomFootprintName((EProcGenRoomFootprint)room.footprint),
				RoomFootprintName((EProcGenRoomFootprint)room.realizedFootprint),
				room.footprintVariant, room.footprintFallback ? "true" : "false",
				room.contourUnified ? "true" : "false", room.contourLoops,
				room.emittedContourSector, room.emittedContourMinX,
				room.emittedContourMinY, room.emittedContourMaxX,
				room.emittedContourMaxY, room.contourVertices,
				room.contourEdges, room.contourArea,
			room.contourWidth, room.contourHeight, room.floorZ,
			room.ceilZ - room.floorZ,
			ElevationRoleName((EProcGenElevationRole)room.elevationRole),
			VerticalIntentName((EProcGenVerticalIntent)room.verticalIntent),
			room.verticalRise, room.verticalAnchor ? "true" : "false",
			ManualInteractionName((EProcGenManualInteraction)room.manualInteraction), room.threatBudget,
			room.recoveryBudget, room.weaponType,
			room.hasAmmo ? room.ammoType : 0, room.hasAmmo ? room.ammoCount : 0,
			RewardPlanName(room.rewardPlan),
			room.optionalArmory ? "true" : "false", ArsenalTrackName((EProcGenArsenalTrack)room.arsenalTrack),
			FinaleCardName((EProcGenFinaleCard)room.finaleCard),
			index + 1 < Rooms.Size() ? "," : "");
	}
	if (includeRooms && Rooms.Size() > 0) RunManifestText.AppendFormat("  ");
	RunManifestText.AppendFormat("],\n");
	RunManifestText.AppendFormat("  \"connections\": [");
	bool firstConnection = true;
	if (includeRooms)
	{
		for (unsigned int y = 0; y < Grid.Size(); ++y)
		{
			for (unsigned int x = 0; x < Grid[y].Size(); ++x)
			{
				const ProcGenCell& first = Grid[y][x];
				if (!first.present) continue;
				for (int direction : { DIR_E, DIR_S })
				{
					const int nx = (int)x + DX[direction];
					const int ny = (int)y + DY[direction];
					if (ny < 0 || ny >= (int)Grid.Size() || nx < 0 ||
						nx >= (int)Grid[ny].Size() || !first.conn[direction] ||
						!Grid[ny][nx].present)
						continue;
					const ProcGenCell& second = Grid[ny][nx];
					const int opposite = OPP[direction];
					const bool keyed = first.lockStage != second.lockStage ||
						(first.isLocked && (first.lockDir < 0 || first.lockDir == direction)) ||
						(second.isLocked && (second.lockDir < 0 ||
							second.lockDir == opposite));
						// Every graph crossing remains inspectable. A fitted stock panel is
						// separate from its physical clear aperture, so compact art cannot
						// hide a narrowed manual transition from the manifest.
						const bool hasDoor = first.connectionHasDoor[direction] ||
							second.connectionHasDoor[opposite];
						const bool secretDoor = first.connectionSecretDoor[direction] ||
							second.connectionSecretDoor[opposite];
						const int doorArtWidth = std::max(
							first.connectionDoorArtWidth[direction],
							second.connectionDoorArtWidth[opposite]);
						const int doorArtHeight = std::max(
							first.connectionDoorArtHeight[direction],
							second.connectionDoorArtHeight[opposite]);
						const char* doorKind = !hasDoor ? "none" :
							(keyed ? "keyed" : (secretDoor ? "secret" : "manual"));
					const bool stair = first.connectionStairChain[direction] >= 0 ||
						second.connectionStairChain[opposite] >= 0 ||
						first.floorZ != second.floorZ;
					const bool mainRoute = first.onMainPath && second.onMainPath;
					const bool roomMerge = first.roomId == second.roomId;
					const bool mandatory = !roomMerge && (keyed || stair || mainRoute ||
						first.hasPlayerStart || first.hasKey || first.hasExit ||
						second.hasPlayerStart || second.hasKey || second.hasExit);
					const char* routeRole = roomMerge ? "room_merge" : (keyed ? "keyed" :
						(stair ? "stair" : (mainRoute ? "main_route" :
						"optional")));
					const int clearWidth = std::min(first.connectionClearWidth[direction],
						second.connectionClearWidth[opposite]);
					const int depth = std::min(first.connectionDepth[direction],
						second.connectionDepth[opposite]);
					const int stairChain = std::max(first.connectionStairChain[direction],
						second.connectionStairChain[opposite]);
					const int alignmentGroup = first.connectionAlignmentGroup[direction] >= 0 ?
						first.connectionAlignmentGroup[direction] :
						second.connectionAlignmentGroup[opposite];
					if (!firstConnection) RunManifestText.AppendFormat(",");
					RunManifestText.AppendFormat(
						"\n    {\"source\": %d, \"target\": %d, \"source_cell\": [%d, %d], "
						"\"target_cell\": [%d, %d], \"route_role\": \"%s\", "
							"\"mandatory\": %s, \"keyed\": %s, \"profile\": \"%s\", "
							"\"clear_width\": %d, \"depth\": %d, \"has_door\": %s, "
							"\"door_kind\": \"%s\", \"door_clear_width\": %d, "
							"\"door_art_width\": %d, \"door_art_height\": %d, \"rise\": %.0f, "
							"\"stair_chain\": %d, \"alignment_group\": %d}",
						first.roomId, second.roomId, (int)x, (int)y, nx, ny, routeRole,
						mandatory ? "true" : "false", keyed ? "true" : "false",
							ConnectionProfileName((EProcGenConnectionProfile)first.connectionProfile[direction]),
							clearWidth, depth, hasDoor ? "true" : "false", doorKind,
							hasDoor ? clearWidth : 0, doorArtWidth, doorArtHeight,
							second.floorZ - first.floorZ, stairChain, alignmentGroup);
					firstConnection = false;
				}
			}
		}
	}
	if (!firstConnection) RunManifestText.AppendFormat("\n  ");
	RunManifestText.AppendFormat("],\n");
	RunManifestText.AppendFormat(
		"  \"visual_proof\": {\"status\": \"%s\", \"alignment_groups\": %d, "
		"\"alignment\": {\"status\": \"%s\", \"metric_textures\": %u, "
		"\"fallback_textures\": %d, \"world_witnesses\": %d, "
		"\"two_sided_witnesses\": %d, \"stair_witnesses\": %d, "
		"\"portal_witnesses\": %d, \"metrics\": [",
		VisualProofPassed ? "proven" : "pending", VisualProofAlignmentGroups,
		VisualProofAlignment ? "proven" : "pending",
		VisualProofAlignmentMetrics.Size(), VisualProofAlignmentFallbackTextures,
		VisualProofAlignmentWorldWitnesses, VisualProofAlignmentTwoSidedWitnesses,
		VisualProofAlignmentStairWitnesses, VisualProofAlignmentPortalWitnesses);
	for (unsigned int index = 0; index < VisualProofAlignmentMetrics.Size(); ++index)
	{
		const ProcGenAlignmentMetric& metric = VisualProofAlignmentMetrics[index];
		if (index > 0) RunManifestText.AppendFormat(", ");
		RunManifestText.AppendFormat("{\"texture\": \"%s\", \"width\": %d, "
			"\"height\": %d, \"fallback\": %s}",
			metric.texture.GetChars(), metric.width, metric.height,
			metric.fallback ? "true" : "false");
	}
	RunManifestText.AppendFormat("], \"witnesses\": [");
	for (unsigned int index = 0; index < VisualProofAlignmentWitnesses.Size(); ++index)
	{
		const ProcGenAlignmentWitness& witness = VisualProofAlignmentWitnesses[index];
		if (index > 0) RunManifestText.AppendFormat(", ");
		RunManifestText.AppendFormat(
			"{\"kind\": \"%s\", \"texture\": \"%s\", \"part\": \"%s\", "
			"\"mode\": \"%s\", \"vertical_anchor\": \"%s\", "
			"\"line\": %d, \"side\": %d, \"width\": %d, \"height\": %d, "
			"\"alignment_group\": %d, \"phase_origin\": %lld, "
			"\"phase_shift\": %d, \"reverse\": %s, \"two_sided\": %s, "
			"\"offset_x\": %.6f, \"offset_y\": %.6f, "
			"\"scale_x\": %.6f, \"scale_y\": %.6f}",
			witness.kind.GetChars(), witness.texture.GetChars(), witness.part.GetChars(),
			witness.mode.GetChars(), witness.verticalAnchor.GetChars(), witness.line,
			witness.side, witness.width, witness.height, witness.alignmentGroup,
			(long long)witness.phaseOrigin, witness.phaseShift,
			witness.reverse ? "true" : "false", witness.twoSided ? "true" : "false",
			witness.offsetX, witness.offsetY, witness.scaleX, witness.scaleY);
	}
	RunManifestText.AppendFormat("]}, \"geometry\": \"%s\", \"connector\": \"%s\", "
		"\"elevation\": \"%s\"},\n",
		VisualProofGeometry ? "proven" : "pending",
		VisualProofConnector ? "proven" : "pending",
		VisualProofElevation ? "proven" : "pending");
	RunManifestText.AppendFormat(
		"  \"accessibility\": {\"status\": \"%s\", \"mandatory_anchors\": %d, "
		"\"ordinary_cells\": %d, \"keyed_door_approaches\": %d, "
		"\"navigation_reservations\": %d, \"collision_navigation\": {"
		"\"status\": \"%s\", \"player_radius\": 16, \"safety_margin\": 16, "
		"\"pad_reservations\": %d, \"corridor_reservations\": %d, "
		"\"reservation_count\": %d, \"proven_corridors\": %d, "
		"\"key_state_edges\": %d, \"mandatory_anchor_count\": %d, "
		"\"ordinary_cells_reached\": %d, \"ordinary_rewards_reached\": %d, "
		"\"keyed_door_approaches_reached\": %d, \"manual_switches_reached\": %d, "
		"\"physical_corridor_witnesses\": %d, \"room_merge_corridors\": %d, "
		"\"switch_cache_actions\": %d, \"switch_cache_rewards_reached\": %d, "
		"\"required_key_mask\": %d, \"exit_key_mask\": %d, \"anchors\": [",
		AccessibilityProofPassed ? "proven" : "pending",
		AccessibilityMandatoryAnchors, AccessibilityOrdinaryCells,
		AccessibilityKeyedDoorApproaches, AccessibilityReservations,
		AccessibilityProofPassed ? "proven" : "pending",
		AccessibilityPadReservations, AccessibilityCorridorReservations,
		AccessibilityReservations, AccessibilityProvenCorridors,
		AccessibilityKeyStateEdges, AccessibilityMandatoryAnchors,
		AccessibilityOrdinaryCells, AccessibilityOrdinaryRewards,
		AccessibilityKeyedDoorApproaches, AccessibilityManualSwitches,
		(int)AccessibilityCorridors.Size(), AccessibilityRoomMergeCorridors,
		AccessibilitySwitchCacheActions, AccessibilitySwitchCacheRewards,
		AccessibilityRequiredKeyMask, AccessibilityExitKeyMask);
	for (unsigned int index = 0; index < AccessibilityAnchors.Size(); ++index)
	{
		const ProcGenAccessibilityAnchor& anchor = AccessibilityAnchors[index];
		if (index > 0) RunManifestText.AppendFormat(", ");
		RunManifestText.AppendFormat(
			"{\"kind\": \"%s\", \"thing_index\": %d, \"line\": %d, "
			"\"side\": %d, \"x\": %.3f, \"y\": %.3f, "
			"\"clear_radius\": %.3f, \"lock\": %d, \"tag\": %d}",
			anchor.kind.GetChars(), anchor.thingIndex, anchor.line, anchor.side,
			anchor.x, anchor.y, anchor.clearRadius, anchor.lock, anchor.tag);
	}
	RunManifestText.AppendFormat("], \"corridors\": [");
	for (unsigned int index = 0; index < AccessibilityCorridors.Size(); ++index)
	{
		const ProcGenAccessibilityCorridor& corridor = AccessibilityCorridors[index];
		if (index > 0) RunManifestText.AppendFormat(", ");
		RunManifestText.AppendFormat(
			"{\"kind\": \"%s\", \"source_cell\": %d, \"target_cell\": %d, "
			"\"source_sector\": %d, \"target_sector\": %d, "
			"\"connector_sector\": %d, \"door_sector\": %d, "
			"\"source_line\": %d, \"target_line\": %d, "
			"\"reservation_count\": %d, \"lanes\": [",
			corridor.kind.GetChars(), corridor.sourceCell, corridor.targetCell,
			corridor.sourceSector, corridor.targetSector, corridor.connectorSector,
			corridor.doorSector, corridor.sourceLine, corridor.targetLine,
			corridor.reservationCount);
		for (unsigned int laneIndex = 0; laneIndex < corridor.lanes.Size(); ++laneIndex)
		{
			const ProcGenAccessibilityLane& lane = corridor.lanes[laneIndex];
			if (laneIndex > 0) RunManifestText.AppendFormat(", ");
			RunManifestText.AppendFormat(
				"{\"x1\": %.3f, \"y1\": %.3f, \"x2\": %.3f, \"y2\": %.3f, "
				"\"clear_radius\": %.3f}",
				lane.x1, lane.y1, lane.x2, lane.y2, lane.clearRadius);
		}
		RunManifestText.AppendFormat("]}");
	}
	RunManifestText.AppendFormat("], \"switch_caches\": [");
	for (unsigned int index = 0; index < AccessibilitySwitchCaches.Size(); ++index)
	{
		const ProcGenAccessibilitySwitchCache& cache = AccessibilitySwitchCaches[index];
		if (index > 0) RunManifestText.AppendFormat(", ");
		RunManifestText.AppendFormat(
			"{\"tag\": %d, \"switch_line\": %d, \"source_sector\": %d, "
			"\"cache_sector\": %d, "
			"\"door_sector\": %d, \"closet_sector\": %d, "
			"\"source_door_line\": %d, \"closet_door_line\": %d, "
			"\"source_door_clear_radius\": %.3f, \"closet_door_clear_radius\": %.3f, "
			"\"reward_first_thing\": %d, \"reward_thing_count\": %d}",
			cache.tag, cache.switchLine, cache.sourceSector, cache.cacheSector,
			cache.doorSector,
			cache.closetSector, cache.sourceDoorLine, cache.closetDoorLine,
			cache.sourceDoorClearRadius, cache.closetDoorClearRadius,
			cache.rewardFirstThing, cache.rewardThingCount);
	}
	const bool cooperativeStartsProven = AccessibilityProofPassed &&
		CooperativeStarts.Size() == 8;
	double cooperativeStartSeparation = 0.0;
	if (CooperativeStarts.Size() > 1)
	{
		cooperativeStartSeparation = 1.0e30;
		for (unsigned int first = 0; first < CooperativeStarts.Size(); ++first)
		{
			for (unsigned int second = first + 1; second < CooperativeStarts.Size(); ++second)
			{
				const double dx = CooperativeStarts[first].x - CooperativeStarts[second].x;
				const double dy = CooperativeStarts[first].y - CooperativeStarts[second].y;
				cooperativeStartSeparation = std::min(cooperativeStartSeparation,
					sqrt(dx * dx + dy * dy));
			}
		}
	}
	RunManifestText.AppendFormat("], \"cooperative_starts\": {\"status\": \"%s\", "
		"\"native_slots\": 8, \"canonical_player\": 1, "
		"\"minimum_separation\": %.3f, \"starts\": [",
		cooperativeStartsProven ? "proven" : "pending", cooperativeStartSeparation);
	for (unsigned int index = 0; index < CooperativeStarts.Size(); ++index)
	{
		const ProcGenCooperativeStart& start = CooperativeStarts[index];
		if (index > 0) RunManifestText.AppendFormat(", ");
		RunManifestText.AppendFormat(
			"{\"player\": %d, \"thing_index\": %d, \"thing_type\": %d, "
			"\"room\": %d, \"landmark_sector\": %d, \"x\": %.3f, "
			"\"y\": %.3f, \"clear_radius\": %.3f}",
			start.player, start.thingIndex, start.thingType, start.roomId,
			start.landmarkSector, start.x, start.y, start.clearRadius);
	}
	// Close the cooperative-start object, collision navigation object,
	// accessibility object, and root manifest in that order. Keep this one
	// schema-1 tail explicit: dumpprocmanifest must remain valid JSON even when
	// its additive cooperative evidence is the final emitted field.
	RunManifestText.AppendFormat("]}}}\n}");
	return RunManifestText;
}

void FProceduralMapGenerator::SetSeed(int seed)
{
	Seed = seed;
	RNG.Init((uint32_t)seed);
	InvalidateBlueprint();
}

void FProceduralMapGenerator::SetTheme(const char* theme)
{
	Theme = theme;
	Theme.ToLower();
	if (Theme.Compare("techbase") != 0 && Theme.Compare("hell") != 0 &&
		Theme.Compare("industrial") != 0 && Theme.Compare("gothic") != 0 &&
		Theme.Compare("corrupted") != 0)
		Theme = "techbase";
	InvalidateBlueprint();
}

void FProceduralMapGenerator::SetDifficulty(int difficulty)
{
	Difficulty = clamp(difficulty, 1, 5);
	InvalidateBlueprint();
}

void FProceduralMapGenerator::SetSize(int size)
{
	Size = clamp(size, MinMapSize, MaxMapSize);
	InvalidateBlueprint();
}

void FProceduralMapGenerator::SetLayout(int layout)
{
	Layout = clamp(layout, 0, 2);
	InvalidateBlueprint();
}

void FProceduralMapGenerator::SetVerticality(int verticality)
{
	Verticality = clamp(verticality, 0, 2);
	InvalidateBlueprint();
}

void FProceduralMapGenerator::SetDetail(int detail)
{
	Detail = clamp(detail, 0, 2);
	InvalidateBlueprint();
}

void FProceduralMapGenerator::SetOutdoors(int outdoors)
{
	Outdoors = clamp(outdoors, 0, 2);
	InvalidateBlueprint();
}

bool FProceduralMapGenerator::Generate()
{
	LastError = "";
	UDMFBuffer = "";
	Grid.Clear();
	Rooms.Clear();
	// TODO(procgen): Heretic and Hexen need their own weapon, inventory, key,
	// monster, texture, and map-action grammars. They are intentionally out of
	// scope for this release. The same is true of every non-Doom game family:
	// fail clearly instead of emitting Doom things into an incompatible IWAD.
	if (gameinfo.gametype != GAME_Doom)
	{
		LastError = (gameinfo.gametype == GAME_Heretic || gameinfo.gametype == GAME_Hexen) ?
			"Heretic and Hexen procedural generation is not supported in this release." :
			"Procedural generation currently supports Doom and Doom II IWADs only.";
		return false;
	}
	// A generation may relocate a beat after testing the concrete grid. Rebuild
	// the recipe-only blueprint here so that no realization state can affect a
	// later call on this singleton, even when every recipe field is unchanged.
	Blueprint = BuildRunBlueprint(Seed, Theme, Difficulty, Size, Layout,
		Verticality, Detail, Outdoors);
	RunProfileText = RunProfileName(Blueprint.Profile);
	RunBriefingText = RunBriefingFor(Blueprint.Profile);
	BlueprintValid = true;
	// Generate may be called repeatedly on the singleton (for example from a
	// manifest/determinism test), so restart the shared layout stream here.
	RNG.Init((uint32_t)Seed);
	HasGeneratedBlueprint = false;
	GeneratedRecipeHash = 0;
	RunManifestText = "";
	AccessibilityProofPassed = false;
	AccessibilityMandatoryAnchors = 0;
	AccessibilityOrdinaryCells = 0;
	AccessibilityKeyedDoorApproaches = 0;
	AccessibilityReservations = 0;
	AccessibilityPadReservations = 0;
	AccessibilityCorridorReservations = 0;
	AccessibilityProvenCorridors = 0;
	AccessibilityKeyStateEdges = 0;
	AccessibilityManualSwitches = 0;
	AccessibilityOrdinaryRewards = 0;
	AccessibilityRequiredKeyMask = 0;
	AccessibilityExitKeyMask = 0;
	AccessibilityAnchors.Clear();
	CooperativeStarts.Clear();
	AccessibilityRoomMergeCorridors = 0;
	AccessibilitySwitchCacheActions = 0;
	AccessibilitySwitchCacheRewards = 0;
	AccessibilityCorridors.Clear();
	AccessibilitySwitchCaches.Clear();
	VisualProofPassed = false;
	VisualProofAlignment = false;
	VisualProofGeometry = false;
	VisualProofConnector = false;
	VisualProofElevation = false;
	VisualProofAlignmentGroups = 0;
	VisualProofAlignmentFallbackTextures = 0;
	VisualProofAlignmentWorldWitnesses = 0;
	VisualProofAlignmentTwoSidedWitnesses = 0;
	VisualProofAlignmentStairWitnesses = 0;
	VisualProofAlignmentPortalWitnesses = 0;
	VisualProofAlignmentMetrics.Clear();
	VisualProofAlignmentWitnesses.Clear();
	RunBlueprint& blueprint = Blueprint;
	// Generation can be repeated with one cached recipe. Reset all realization
	// data here so a failed shape or a shorter route from a prior run cannot
	// leak into the next deterministic manifest.
	blueprint.RealizedStageCount = blueprint.PlannedStageCount;
	blueprint.RealizedVerticalBeats = 0;
	for (int stage = 0; stage < RunBlueprint::MaxStages; ++stage)
	{
		blueprint.RealizedStageShapes[stage] = blueprint.StageShapes[stage];
		blueprint.RealizedStageLandmarks[stage] = blueprint.StageLandmarks[stage];
		blueprint.RealizedStageVerticalIntents[stage] = PGVI_Flat;
		blueprint.RealizedStageVerticalRises[stage] = 0;
		if (stage < RunBlueprint::MaxStages - 1) blueprint.StageGateRanks[stage] = -1;
	}
	const ThemeStyle themeStyle = GetThemeStyle(Theme);
	auto ScaleSetting = [](int value, int setting, int lowPercent, int highPercent) -> int
	{
		const int percent = setting <= 0 ? lowPercent : (setting >= 2 ? highPercent : 100);
		return std::max(1, (value * percent + 50) / 100);
	};

	// A rectangular canvas better matches the broad, directional footprints of
	// classic Doom maps. Above size 40, transfer each additional column of width
	// growth into height. This keeps the absurd settings at least as capacious
	// without forcing their start and exit against UDMF's horizontal limit.
	const int extremeReflow = std::max(0, Size - 40);
	const int W = 8 + Size * 2 - extremeReflow;
	const int H = 7 + Size + extremeReflow;
	// The engine accepts coordinates through +/-262144 (MAX_MAP_COORD in
	// doomdef.h), but extreme maps should not make starts, blockmaps, or node
	// partitions live against that boundary. This band retains a generous
	// safety margin of a full coordinate decade while allowing maps with a
	// footprint more than five times wider than the old +/-24500 band.
	const double MaxCoordinate = 131072.0;
	const double extentX = (W * 0.5 - 1.5) * CELL_SIZE + 192.0;
	const double extentY = (H * 0.5 - 1.5) * CELL_SIZE + 192.0;
	if (extentX > MaxCoordinate || extentY > MaxCoordinate)
	{
		LastError = "Requested map size exceeds the procedural coordinate safety range";
		return false;
	}

	Grid.Resize(H);
	for (int y = 0; y < H; y++)
	{
		Grid[y].Resize(W);
		for (int x = 0; x < W; x++)
			Grid[y][x] = ProcGenCell();
	}

	auto InBounds = [&](int x, int y) -> bool
	{
		return x >= 1 && x < W - 1 && y >= 1 && y < H - 1;
	};

	auto ConnectCells = [&](int ax, int ay, int bx, int by)
	{
		for (int d = 0; d < 4; d++)
		{
			if (ax + DX[d] == bx && ay + DY[d] == by)
			{
				Grid[ay][ax].conn[d] = true;
				Grid[by][bx].conn[OPP[d]] = true;
				return;
			}
		}
	};

	auto DirectionBetween = [&](int ax, int ay, int bx, int by) -> int
	{
		for (int d = 0; d < 4; d++)
			if (ax + DX[d] == bx && ay + DY[d] == by)
				return d;
		return -1;
	};

	// Build a private randomized spanning tree. It supplies natural bends and
	// detours, but only the selected route and branches become map geometry.
	TArray<TArray<bool>> visited;
	TArray<TArray<int>> depth;
	TArray<TArray<std::pair<int, int>>> parent;
	visited.Resize(H);
	depth.Resize(H);
	parent.Resize(H);
	for (int y = 0; y < H; y++)
	{
		visited[y].Resize(W);
		depth[y].Resize(W);
		parent[y].Resize(W);
		for (int x = 0; x < W; x++)
		{
			visited[y][x] = false;
			depth[y][x] = -1;
			parent[y][x] = std::make_pair(-1, -1);
		}
	}

	int sx = 1;
	int sy = 1;
	const int startOffset = RNG() %
		((blueprint.Orientation == PGRO_Eastbound || blueprint.Orientation == PGRO_Westbound) ?
			(H - 2) : (W - 2));
	switch (blueprint.Orientation)
	{
	case PGRO_Westbound:
		sx = W - 2;
		sy = 1 + startOffset;
		break;
	case PGRO_Northbound:
		sx = 1 + startOffset;
		sy = H - 2;
		break;
	case PGRO_Southbound:
		sx = 1 + startOffset;
		sy = 1;
		break;
	default:
		sx = 1;
		sy = 1 + startOffset;
		break;
	}
	TArray<std::pair<int, int>> stack;
	stack.Push(std::make_pair(sx, sy));
	visited[sy][sx] = true;
	depth[sy][sx] = 0;

	while (stack.Size() > 0)
	{
		const int cx = stack.Last().first;
		const int cy = stack.Last().second;
		int bestDir = -1;
		int bestScore = -100000;

		for (int d = 0; d < 4; d++)
		{
			const int nx = cx + DX[d];
			const int ny = cy + DY[d];
			if (!InBounds(nx, ny) || visited[ny][nx]) continue;

			int score = (int)(RNG() % 100);
			const int forwardBias = Layout == 0 ? 34 : (Layout == 2 ? 8 : 18);
			if (IsForwardDirection(blueprint.Orientation, d)) score += forwardBias;
			if (IsBackwardDirection(blueprint.Orientation, d))
				score -= Layout == 0 ? 15 : (Layout == 2 ? 3 : 8);
			if (Layout == 2 && !IsForwardDirection(blueprint.Orientation, d) &&
				!IsBackwardDirection(blueprint.Orientation, d)) score += 7;
			if (themeStyle == ThemeIndustrial && IsForwardDirection(blueprint.Orientation, d)) score += 5;
			if ((themeStyle == ThemeHell || themeStyle == ThemeGothic) &&
				!IsForwardDirection(blueprint.Orientation, d) &&
				!IsBackwardDirection(blueprint.Orientation, d)) score += 4;
			if (ny == 1 || ny == H - 2) score -= 5;
			if (score > bestScore)
			{
				bestScore = score;
				bestDir = d;
			}
		}

		if (bestDir < 0)
		{
			stack.Pop();
			continue;
		}

		const int nx = cx + DX[bestDir];
		const int ny = cy + DY[bestDir];
		visited[ny][nx] = true;
		depth[ny][nx] = depth[cy][cx] + 1;
		parent[ny][nx] = std::make_pair(cx, cy);
		stack.Push(std::make_pair(nx, ny));
	}

	const int desiredRoute = std::max(8, ScaleSetting(9 + Size * 4, Layout, 78, 122) *
		blueprint.RouteLengthPercent / 100);
	// Reserve enough parent-chain depth for every blueprint-owned mandatory stair
	// before selecting the exit. This is the deterministic topology replan for a
	// compact route: choose a deeper already-carved maze endpoint, rather than
	// later weakening a gate buffer or dropping a promised vertical beat.
	int minimumVerticalRouteDepth = 7;
	if (Size >= 3)
	{
		const int plannedKeys = blueprint.PlannedStageCount - 1;
		int requiredDepth = 0;
		for (int stage = 0; stage < plannedKeys; ++stage)
		{
			const bool needsVerticalBeat = blueprint.StageVerticalIntents[stage] != PGVI_Flat;
			requiredDepth += needsVerticalBeat ? (stage == 0 ? 4 : 5) : 3;
		}
		const bool finalNeedsVerticalBeat =
			blueprint.StageVerticalIntents[plannedKeys] != PGVI_Flat;
		requiredDepth += finalNeedsVerticalBeat ? 4 : 1;
		minimumVerticalRouteDepth = std::max(minimumVerticalRouteDepth, requiredDepth);
	}
	int ex = sx;
	int ey = sy;
	int bestExitScore = -1000000;
	const int routeAxisLength = (blueprint.Orientation == PGRO_Eastbound ||
		blueprint.Orientation == PGRO_Westbound) ? W : H;
	const int perpendicularAxisLength = (blueprint.Orientation == PGRO_Eastbound ||
		blueprint.Orientation == PGRO_Westbound) ? H : W;
	const int startPerpendicular = PerpendicularCoordinate(blueprint.Orientation, sx, sy);
	for (int y = 1; y < H - 1; y++)
	{
		for (int x = 1; x < W - 1; x++)
		{
			if (!visited[y][x] || depth[y][x] < minimumVerticalRouteDepth) continue;
			const int forward = ForwardCoordinate(blueprint.Orientation, x, y, W, H);
			const int perpendicular = PerpendicularCoordinate(blueprint.Orientation, x, y);
			int score = forward * 28 - abs(depth[y][x] - desiredRoute) * 8;
			if (forward >= routeAxisLength - 3) score += 100;
			if (abs(perpendicular - startPerpendicular) >= perpendicularAxisLength / 3) score += 18;
			if (score > bestExitScore)
			{
				bestExitScore = score;
				ex = x;
				ey = y;
			}
		}
	}

	if (ex == sx && ey == sy)
	{
		LastError = "Could not embed a sufficiently long main route";
		return false;
	}

	TArray<std::pair<int, int>> mainPath;
	for (int x = ex, y = ey, guard = W * H + 4; guard-- > 0;)
	{
		mainPath.Push(std::make_pair(x, y));
		if (x == sx && y == sy) break;
		auto p = parent[y][x];
		if (p.first < 0)
		{
			LastError = "Main route parent chain is incomplete";
			return false;
		}
		x = p.first;
		y = p.second;
	}
	for (int a = 0, b = (int)mainPath.Size() - 1; a < b; a++, b--)
	{
		auto tmp = mainPath[a];
		mainPath[a] = mainPath[b];
		mainPath[b] = tmp;
	}

	if (mainPath.Size() < 8)
	{
		LastError = "Generated main route is too short";
		return false;
	}

	TArray<TArray<bool>> keep;
	keep.Resize(H);
	for (int y = 0; y < H; y++)
	{
		keep[y].Resize(W);
		for (int x = 0; x < W; x++) keep[y][x] = false;
	}

	for (int rank = 0; rank < (int)mainPath.Size(); rank++)
	{
		const int x = mainPath[rank].first;
		const int y = mainPath[rank].second;
		keep[y][x] = true;
		Grid[y][x].present = true;
		Grid[y][x].pathRank = rank;
		Grid[y][x].onMainPath = true;
		if (rank > 0)
			ConnectCells(mainPath[rank - 1].first, mainPath[rank - 1].second, x, y);
	}

	// Grow a deliberate optional limb. Branches avoid touching the main path
	// away from their anchor, preventing accidental shortcuts around locks.
	auto GrowBranch = [&](int anchorRank, int wanted, TArray<std::pair<int, int>>& result) -> bool
	{
		result.Clear();
		if (anchorRank <= 0 || anchorRank >= (int)mainPath.Size() - 1) return false;

		int cx = mainPath[anchorRank].first;
		int cy = mainPath[anchorRank].second;
		for (int step = 1; step <= wanted; step++)
		{
			int bestX = -1;
			int bestY = -1;
			int bestScore = -100000;
			for (int d = 0; d < 4; d++)
			{
				const int nx = cx + DX[d];
				const int ny = cy + DY[d];
				if (!InBounds(nx, ny) || keep[ny][nx]) continue;

				int touchesMain = 0;
				int touchesKept = 0;
				int openness = 0;
				for (int od = 0; od < 4; od++)
				{
					const int ox = nx + DX[od];
					const int oy = ny + DY[od];
					if (!InBounds(ox, oy)) continue;
					if (keep[oy][ox])
					{
						touchesKept++;
						if (Grid[oy][ox].onMainPath && !(ox == cx && oy == cy)) touchesMain++;
					}
					else openness++;
				}

				int score = (int)(RNG() % 31) + openness * 7 - touchesKept * 6 - touchesMain * 80;
				if (nx == 1 || nx == W - 2 || ny == 1 || ny == H - 2) score += 5;
				if (score > bestScore)
				{
					bestScore = score;
					bestX = nx;
					bestY = ny;
				}
			}

			if (bestX < 0) break;
			keep[bestY][bestX] = true;
			Grid[bestY][bestX].present = true;
			Grid[bestY][bestX].pathRank = anchorRank;
			Grid[bestY][bestX].branchDepth = step;
			ConnectCells(cx, cy, bestX, bestY);
			result.Push(std::make_pair(bestX, bestY));
			cx = bestX;
			cy = bestY;
		}
		return result.Size() > 0;
	};

	struct KeyPlan
	{
		int x = -1;
		int y = -1;
		int anchorRank = -1;
		int gateRank = -1;
		int type = 0;
	};
	TArray<KeyPlan> keys;
	TArray<int> gateRanks;
	const int targetKeys = (Size >= 5 && mainPath.Size() >= 18) ? 3 :
		((Size >= 3 && mainPath.Size() >= 13) ? 2 : 1);
	const int minGateSpacing = 3;
	// A gate boundary needs more than the ordinary three ranks when its stage
	// owns a mandatory stair: the endpoint beside a keyed threshold is protected,
	// as are the start and exit pads. Reserve the first safe interior edge before
	// placing key branches and landmark loops, rather than discovering late that a
	// fully valid progression topology has no place to realize its vertical beat.
	// These bounds are recipe-only topology planning; they do not alter layout RNG.
	auto StageNeedsVerticalBeat = [&](int stage) -> bool
	{
		return stage >= 0 && stage < blueprint.PlannedStageCount &&
			blueprint.StageVerticalIntents[stage] != PGVI_Flat;
	};
	auto MinimumStageSpan = [&](int stage) -> int
	{
		if (!StageNeedsVerticalBeat(stage)) return minGateSpacing;
		// Stage zero can use rank 1 -> 2 once its first gate is at rank 4.
		// Later stages must clear both their entering and leaving gate buffers,
		// leaving rank previous+2 -> previous+3 as their first legal connector.
		return stage == 0 ? 4 : 5;
	};
	auto FinalStageTail = [&]() -> int
	{
		// The final stage's exit pad consumes the final rank. A vertical beat needs
		// a non-exit successor after its entering-gate buffer; non-vertical finales
		// retain the historic one-rank minimum.
		return StageNeedsVerticalBeat(targetKeys) ? 4 : 1;
	};
	const int plannedStageCount = targetKeys + 1;
	int totalStageWeight = 0;
	for (int stage = 0; stage < plannedStageCount; ++stage)
		totalStageWeight += std::max(1, blueprint.StageWeights[stage]);
	int accumulatedStageWeight = 0;

	for (int k = 0; k < targetKeys; k++)
	{
		const int minimumGateRank = gateRanks.Size() > 0 ?
			gateRanks.Last() + MinimumStageSpan(k) : MinimumStageSpan(0);
		int reservedTail = FinalStageTail();
		for (int futureStage = k + 1; futureStage < targetKeys; ++futureStage)
			reservedTail += MinimumStageSpan(futureStage);
		const int maximumGateRank = (int)mainPath.Size() - 1 - reservedTail;
		accumulatedStageWeight += std::max(1, blueprint.StageWeights[k]);
		const int localJitter = (int)(BlueprintChannel(blueprint.RecipeHash, 40 + k) % 5u) - 2;
		// Stage weights replace fixed fractions while clamps retain both a
		// meaningful key branch before each gate and the future stages' minimum
		// room to breathe. The result is deterministic recipe data, not a draw
		// from the layout RNG.
		const int nominalGateRank = (int)(((int64_t)((int)mainPath.Size() - 1) *
			accumulatedStageWeight + totalStageWeight / 2) / totalStageWeight) +
			blueprint.GateRankJitter + localJitter;
		const int gateRank = clamp(nominalGateRank, minimumGateRank, maximumGateRank);
		const int stageStartRank = gateRanks.Size() > 0 ? gateRanks.Last() : 0;
		int anchor = std::max(stageStartRank + 1,
			gateRank - std::max(2, (int)mainPath.Size() / (targetKeys + 3)));
		TArray<std::pair<int, int>> limb;
		bool placed = false;
		for (int attempt = 0; attempt < 8 && !placed; attempt++)
		{
			int tryRank = clamp(anchor + ((attempt + 1) / 2) * ((attempt & 1) ? 1 : -1),
				stageStartRank + 1, gateRank - 1);
			const int keyBranchLength = std::max(1, ScaleSetting(2 + Size / 2 + (RNG() % 2),
				Layout, 78, 122) * blueprint.BranchCountPercent / 100);
			placed = GrowBranch(tryRank, keyBranchLength, limb);
			if (placed) anchor = tryRank;
		}
		if (!placed) continue;

		auto tip = limb.Last();
		Grid[tip.second][tip.first].hasKey = true;
		Grid[tip.second][tip.first].keyType = blueprint.KeyOrder[k];
		Grid[tip.second][tip.first].isArena = true;

		KeyPlan plan;
		plan.x = tip.first;
		plan.y = tip.second;
		plan.anchorRank = anchor;
		plan.gateRank = gateRank;
		plan.type = blueprint.KeyOrder[k];
		keys.Push(plan);
		gateRanks.Push(gateRank);
		blueprint.StageGateRanks[keys.Size() - 1] = gateRank;
	}

	if (keys.Size() == 0)
	{
		LastError = "Could not place a key branch";
		return false;
	}
	blueprint.RealizedStageCount = clamp((int)keys.Size() + 1, 1,
		RunBlueprint::MaxStages);

	// A lock belongs to one directed boundary, not to an entire room. This
	// prevents the former two-to-eight locked doors around a single gate cell.
	for (unsigned int k = 0; k < keys.Size(); k++)
	{
		const int rank = keys[k].gateRank;
		const int gx = mainPath[rank].first;
		const int gy = mainPath[rank].second;
		const int px = mainPath[rank - 1].first;
		const int py = mainPath[rank - 1].second;
		Grid[gy][gx].isLocked = true;
		Grid[gy][gx].lockType = keys[k].type;
		Grid[gy][gx].lockDir = DirectionBetween(gx, gy, px, py);
	}

	// Optional branches are spaced across the critical path, giving the player
	// reasons to explore without turning the map into a uniform maze.
	int sideBranchTarget = std::max(1, ScaleSetting(4 + Size + Size / 4, Layout, 52, 155) *
		blueprint.BranchCountPercent / 100);
	if (Size >= 5 && themeStyle == ThemeHell) sideBranchTarget += std::max(1, Size / 6);
	else if (Size >= 5 && themeStyle == ThemeGothic) sideBranchTarget += std::max(1, Size / 8);
	else if (Size >= 5 && themeStyle == ThemeIndustrial) sideBranchTarget += std::max(1, Size / 10);
	int reservedSecretX = -1;
	int reservedSecretY = -1;
	for (int b = 0; b < sideBranchTarget; b++)
	{
		const int branchNumerator = b + 1;
		const int branchDenominator = sideBranchTarget + 1;
		int plannedRank = (int)mainPath.Size() * branchNumerator / branchDenominator;
		if (blueprint.BranchRankBias < 0)
		{
			plannedRank = (int)((int64_t)mainPath.Size() * branchNumerator * branchNumerator /
				((int64_t)branchDenominator * branchDenominator));
		}
		else if (blueprint.BranchRankBias > 0)
		{
			const int remaining = branchDenominator - branchNumerator;
			plannedRank = (int)mainPath.Size() - (int)((int64_t)mainPath.Size() * remaining * remaining /
				((int64_t)branchDenominator * branchDenominator));
		}
		int rank = clamp(plannedRank +
			(int)(RNG() % 3) - 1, 1, (int)mainPath.Size() - 2);
		bool nearKeyBranch = false;
		for (unsigned int k = 0; k < keys.Size(); k++)
			if (abs(keys[k].anchorRank - rank) <= 1) nearKeyBranch = true;
		if (nearKeyBranch) rank = clamp(rank + 2, 1, (int)mainPath.Size() - 2);

		TArray<std::pair<int, int>> limb;
		const int baseLength = 1 + (RNG() % (2 + Size / 2));
		GrowBranch(rank, ScaleSetting(baseLength, Layout, 65, 145), limb);
		if (limb.Size() > 0 && reservedSecretX < 0)
		{
			reservedSecretX = limb.Last().first;
			reservedSecretY = limb.Last().second;
		}
		if (limb.Size() >= 2 && (b & 1))
			Grid[limb.Last().second][limb.Last().first].isArena = true;
	}

	// Keep one optional leaf structurally reserved for a conventional hidden
	// reward. Compact layouts can spend their first few free cells on key limbs,
	// so retry every route anchor before landmark growth if the ordinary branch
	// budget found no room. The reserved tip is isolated from later loops and
	// room merging; this makes the secret a true one-door leaf rather than a
	// cosmetic flag on a through route.
	if (reservedSecretX < 0)
	{
		for (int rank = 1; rank < (int)mainPath.Size() - 1 && reservedSecretX < 0; rank++)
		{
			TArray<std::pair<int, int>> limb;
			if (!GrowBranch(rank, 2, limb) || limb.Size() == 0) continue;
			reservedSecretX = limb.Last().first;
			reservedSecretY = limb.Last().second;
		}
	}
	if (reservedSecretX < 0)
	{
		LastError = "Could not reserve an optional secret branch";
		return false;
	}
	Grid[reservedSecretY][reservedSecretX].reservedSecret = true;

	// Expand selected beats into recognisable chambers. Added cells inherit one
	// progression rank, so the room pass can merge them into a single landmark.
	auto ExpandLandmark = [&](int cx, int cy, int budget, bool hub, bool arena)
	{
		if (!InBounds(cx, cy) || !keep[cy][cx]) return;
		Grid[cy][cx].isHub = Grid[cy][cx].isHub || hub;
		Grid[cy][cx].isArena = Grid[cy][cx].isArena || arena;
		TArray<std::pair<int, int>> cluster;
		cluster.Push(std::make_pair(cx, cy));

		for (int n = 0; n < budget; n++)
		{
			int bestX = -1;
			int bestY = -1;
			int bestScore = -100000;
			for (unsigned int ci = 0; ci < cluster.Size(); ci++)
			{
				const int ax = cluster[ci].first;
				const int ay = cluster[ci].second;
				for (int d = 0; d < 4; d++)
				{
					const int nx = ax + DX[d];
					const int ny = ay + DY[d];
					if (!InBounds(nx, ny) || keep[ny][nx]) continue;
					int adjacentCluster = 0;
					for (int od = 0; od < 4; od++)
					{
						const int ox = nx + DX[od];
						const int oy = ny + DY[od];
						if (InBounds(ox, oy) && keep[oy][ox] &&
							Grid[oy][ox].pathRank == Grid[cy][cx].pathRank)
							adjacentCluster++;
					}
					int score = adjacentCluster * 18 - (abs(nx - cx) + abs(ny - cy)) * 5 + (RNG() % 9);
					if (score > bestScore)
					{
						bestScore = score;
						bestX = nx;
						bestY = ny;
					}
				}
			}

			if (bestX < 0) break;
			keep[bestY][bestX] = true;
			Grid[bestY][bestX].present = true;
			Grid[bestY][bestX].pathRank = Grid[cy][cx].pathRank;
			Grid[bestY][bestX].branchDepth = Grid[cy][cx].branchDepth;
			// Landmark support cells widen the encounter space but are not an
			// additional traversal beat. Keeping the flag on the one route cell
			// makes the manifest's main-path sequence unambiguous after rooms merge.
			Grid[bestY][bestX].onMainPath = false;
			Grid[bestY][bestX].isHub = hub;
			Grid[bestY][bestX].isArena = arena;
			for (int d = 0; d < 4; d++)
			{
				const int nx = bestX + DX[d];
				const int ny = bestY + DY[d];
				if (InBounds(nx, ny) && keep[ny][nx] &&
						!Grid[ny][nx].reservedSecret &&
						Grid[ny][nx].pathRank == Grid[bestY][bestX].pathRank)
					ConnectCells(bestX, bestY, nx, ny);
			}
			cluster.Push(std::make_pair(bestX, bestY));
		}
	};

	const int combatGrowth = Difficulty - 1;
	auto LandmarkBudget = [&](int budget) -> int
	{
		int result = ScaleSetting(budget, Detail, 68, 140);
		if (Size >= 5 && themeStyle == ThemeGothic && budget >= 2) result++;
		if (Size >= 5 && themeStyle == ThemeHell && budget >= 3) result++;
		return result;
	};
	Grid[ey][ex].hasExit = true;
	Grid[ey][ex].hasBoss = (Difficulty >= 5 || (Difficulty >= 4 && Size >= 4));

	Grid[sy][sx].hasPlayerStart = true;
	ExpandLandmark(sx, sy, LandmarkBudget(1 + Size / 2), true, false);
	// Reserve the finale before secondary landmarks consume nearby empty cells.
	// This keeps the heavyweight-boss capacity check meaningful on compact maps.
	ExpandLandmark(ex, ey, LandmarkBudget(3 + Size / 2 + combatGrowth * 2), false, true);

	const int firstHubRank = clamp((int)mainPath.Size() * blueprint.HubRankPercent / 100,
		2, (int)mainPath.Size() - 3);
	ExpandLandmark(mainPath[firstHubRank].first, mainPath[firstHubRank].second,
		LandmarkBudget(2 + Size / 2 + combatGrowth / 2), true, false);
	const int arenaRank = clamp((int)mainPath.Size() * blueprint.ArenaRankPercent / 100,
		firstHubRank + 1, (int)mainPath.Size() - 2);
	ExpandLandmark(mainPath[arenaRank].first, mainPath[arenaRank].second,
		LandmarkBudget(2 + Size / 2 + combatGrowth * 2), false, true);
	for (unsigned int k = 0; k < keys.Size(); k++)
		ExpandLandmark(keys[k].x, keys[k].y,
			LandmarkBudget(1 + Size / 2 + combatGrowth), false, true);

	// Add local circulation only inside the same lock stage. These loops create
	// classic Doom re-use and cross-views without bypassing key progression.
	auto StageForRank = [&](int rank) -> int
	{
		int stage = 0;
		for (unsigned int k = 0; k < gateRanks.Size(); k++)
			if (rank >= gateRanks[k]) stage++;
		return stage;
	};

	// Plan a recognizable same-stage macro form before room merging. These
	// small templates deliberately preserve the original spine as the reliable
	// route, then add a bypass, circuit, or court only where every new cell and
	// every rejoin stays in the same key stage. A requested form that cannot fit
	// is explicitly realized as Spine rather than gambling on an unsafe shortcut.
	auto IsSafeStageMainCell = [&](int x, int y, int stage) -> bool
	{
		if (!InBounds(x, y) || !keep[y][x]) return false;
		const ProcGenCell& cell = Grid[y][x];
		if (!cell.onMainPath || StageForRank(cell.pathRank) != stage) return false;
		return !cell.hasPlayerStart && !cell.hasExit && !cell.hasKey &&
			!cell.isLocked && !cell.reservedSecret;
	};

	auto AddStageDetourCell = [&](int x, int y, int rank, int depth)
	{
		keep[y][x] = true;
		ProcGenCell& cell = Grid[y][x];
		cell.present = true;
		cell.pathRank = rank;
		cell.branchDepth = depth;
		cell.onMainPath = false;
	};

	auto TryParallelBypass = [&](int stage, int span) -> bool
	{
		const int firstRank = stage > 0 ? gateRanks[stage - 1] : 0;
		const int lastRank = stage < (int)gateRanks.Size() ?
			gateRanks[stage] - 1 : (int)mainPath.Size() - 1;
		if (lastRank - firstRank < span + 2) return false;

		for (int rank = firstRank + 1; rank + span <= lastRank - 1; ++rank)
		{
			bool safe = true;
			int travelDirection = -1;
			for (int step = 0; step <= span; ++step)
			{
				const int px = mainPath[rank + step].first;
				const int py = mainPath[rank + step].second;
				if (!IsSafeStageMainCell(px, py, stage)) safe = false;
				if (step > 0)
				{
					const int previousDirection = DirectionBetween(mainPath[rank + step - 1].first,
						mainPath[rank + step - 1].second, px, py);
					if (travelDirection < 0) travelDirection = previousDirection;
					else if (travelDirection != previousDirection) safe = false;
				}
			}
			if (!safe || travelDirection < 0) continue;

			int sideDirections[2];
			if (travelDirection == DIR_E || travelDirection == DIR_W)
			{
				sideDirections[0] = DIR_N;
				sideDirections[1] = DIR_S;
			}
			else
			{
				sideDirections[0] = DIR_W;
				sideDirections[1] = DIR_E;
			}
			if (BlueprintChannel(blueprint.RecipeHash, 96 + stage + rank) & 1u)
				std::swap(sideDirections[0], sideDirections[1]);

			for (int sideIndex = 0; sideIndex < 2; ++sideIndex)
			{
				const int side = sideDirections[sideIndex];
				bool clear = true;
				for (int step = 0; step <= span; ++step)
				{
					const int px = mainPath[rank + step].first + DX[side];
					const int py = mainPath[rank + step].second + DY[side];
					if (!InBounds(px, py) || keep[py][px])
					{
						clear = false;
						break;
					}
				}
				if (!clear) continue;

				for (int step = 0; step <= span; ++step)
				{
					const int px = mainPath[rank + step].first + DX[side];
					const int py = mainPath[rank + step].second + DY[side];
					AddStageDetourCell(px, py, rank + step, step + 1);
				}
				ConnectCells(mainPath[rank].first, mainPath[rank].second,
					mainPath[rank].first + DX[side], mainPath[rank].second + DY[side]);
				for (int step = 0; step < span; ++step)
				{
					const int ax = mainPath[rank + step].first + DX[side];
					const int ay = mainPath[rank + step].second + DY[side];
					const int bx = mainPath[rank + step + 1].first + DX[side];
					const int by = mainPath[rank + step + 1].second + DY[side];
					ConnectCells(ax, ay, bx, by);
				}
				ConnectCells(mainPath[rank + span].first + DX[side],
					mainPath[rank + span].second + DY[side],
					mainPath[rank + span].first, mainPath[rank + span].second);
				return true;
			}
		}
		return false;
	};

	auto TryCourtyardSpokes = [&](int stage) -> bool
	{
		const int firstRank = stage > 0 ? gateRanks[stage - 1] : 0;
		const int lastRank = stage < (int)gateRanks.Size() ?
			gateRanks[stage] - 1 : (int)mainPath.Size() - 1;
		for (int rank = firstRank + 1; rank < lastRank; ++rank)
		{
			const int cx = mainPath[rank].first;
			const int cy = mainPath[rank].second;
			if (!IsSafeStageMainCell(cx, cy, stage)) continue;
			int candidates[4] = { DIR_N, DIR_E, DIR_S, DIR_W };
			const int rotation = (int)(BlueprintChannel(blueprint.RecipeHash, 128 + stage + rank) % 4u);
			int chosen[2] = { -1, -1 };
			int chosenCount = 0;
			for (int offset = 0; offset < 4 && chosenCount < 2; ++offset)
			{
				const int direction = candidates[(rotation + offset) % 4];
				const int nx = cx + DX[direction];
				const int ny = cy + DY[direction];
				if (!InBounds(nx, ny) || keep[ny][nx]) continue;
				chosen[chosenCount++] = direction;
			}
			if (chosenCount < 2) continue;

			Grid[cy][cx].isHub = true;
			for (int spoke = 0; spoke < chosenCount; ++spoke)
			{
				const int nx = cx + DX[chosen[spoke]];
				const int ny = cy + DY[chosen[spoke]];
				AddStageDetourCell(nx, ny, rank, 1);
				ConnectCells(cx, cy, nx, ny);
			}
			return true;
		}
		return false;
	};

	for (int stage = 0; stage < blueprint.RealizedStageCount; ++stage)
	{
		const EProcGenStageShape requested = blueprint.StageShapes[stage];
		bool realized = requested == PGSS_Spine;
		switch (requested)
		{
		case PGSS_ForkRejoin: realized = TryParallelBypass(stage, 1); break;
		case PGSS_Ring: realized = TryParallelBypass(stage, 2); break;
		case PGSS_Switchback:
			// The long parallel return is deliberately left for the geometry pass
			// to turn into a dogleg/switchback silhouette rather than another
			// circular room ring.
			realized = TryParallelBypass(stage, 2);
			break;
		case PGSS_CourtyardSpokes: realized = TryCourtyardSpokes(stage); break;
		default: break;
		}
		blueprint.RealizedStageShapes[stage] = realized ? requested : PGSS_Spine;
	}

	int loopBudget = std::max(0, ScaleSetting(2 + Size + Size / 2, Layout, 35, 180) *
		blueprint.LoopQuotaPercent / 100);
	if (themeStyle == ThemeTechbase || themeStyle == ThemeIndustrial)
		loopBudget += std::max(1, Size / 8);
	const int loopChance = clamp((Layout == 0 ? 20 : (Layout == 2 ? 58 : 38)) +
		(blueprint.LoopQuotaPercent - 100) / 3, 12, 78);
	for (int pass = 0; pass < 3 && loopBudget > 0; pass++)
	{
		for (int y = 1; y < H - 1 && loopBudget > 0; y++)
		{
			for (int x = 1; x < W - 1 && loopBudget > 0; x++)
			{
				if (!keep[y][x]) continue;
				for (int d : { DIR_E, DIR_S })
				{
					const int nx = x + DX[d];
					const int ny = y + DY[d];
					if (!InBounds(nx, ny) || !keep[ny][nx] || Grid[y][x].conn[d]) continue;
					if (Grid[y][x].reservedSecret || Grid[ny][nx].reservedSecret) continue;
					// Landmark pads and keyed thresholds keep their deliberately simple
					// circulation. A generic late loop into one of these protected cells
					// can make every otherwise safe stair candidate bypassable, while a
					// normal room-to-room reconnection preserves the intended exploration.
					if (Grid[y][x].hasPlayerStart || Grid[ny][nx].hasPlayerStart ||
						Grid[y][x].hasExit || Grid[ny][nx].hasExit ||
						Grid[y][x].hasKey || Grid[ny][nx].hasKey ||
						Grid[y][x].isLocked || Grid[ny][nx].isLocked)
						continue;
					if (StageForRank(Grid[y][x].pathRank) != StageForRank(Grid[ny][nx].pathRank)) continue;
					const int rankGap = abs(Grid[y][x].pathRank - Grid[ny][nx].pathRank);
					const int maximumFoldback = 7 + Size / 5;
					if (rankGap > maximumFoldback) continue;
					// Spend the first pass on reconnections that fold a route back by
					// several beats. Later passes may add local circulation if the
					// layout cannot physically accommodate enough long loops.
					if (pass == 0 && rankGap < 3) continue;
					const int averageRankPercent = (Grid[y][x].pathRank + Grid[ny][nx].pathRank) * 50 /
						std::max(1, (int)mainPath.Size() - 1);
					int preferredChance = loopChance;
					if (blueprint.LoopRankBias < 0)
						preferredChance += (50 - averageRankPercent) / 2;
					else if (blueprint.LoopRankBias > 0)
						preferredChance += (averageRankPercent - 50) / 2;
					if ((RNG() % 100) >= clamp(preferredChance, 8, 85)) continue;
					ConnectCells(x, y, nx, ny);
					loopBudget--;
					if (loopBudget <= 0) break;
				}
			}
		}
	}

	// Bind planned elevation to a real, ordinary main-route connector. The
	// geometry pass receives an anchor and a signed rise, never a request to
	// improvise stairs through a locked threshold, key pad, or landmark pad.
	TArray<bool> verticalRankUsed;
	verticalRankUsed.Resize(mainPath.Size());
	for (unsigned int rank = 0; rank < verticalRankUsed.Size(); ++rank)
		verticalRankUsed[rank] = false;
	// A counted route stair must be unavoidable inside its current key stage.
	// Same-stage loops are valuable for exploration, but placing a vertical beat
	// on one side of a fork lets the player simply walk around it and also makes
	// floor-coherence treat the two stair endpoints as one rigid terrace. Locate
	// an alternate same-stage path with the planned edge omitted; the caller can
	// then prefer a bridge or, as a compact topology fallback, retract only the
	// reconnection that makes a safe main-route edge bypassable.
	auto FindSameStageBypass = [&](int routeRank, int stage,
		TArray<std::pair<int, int>>& path) -> bool
	{
		path.Clear();
		if (routeRank < 0 || routeRank + 1 >= (int)mainPath.Size()) return false;
		const int sourceX = mainPath[routeRank].first;
		const int sourceY = mainPath[routeRank].second;
		const int targetX = mainPath[routeRank + 1].first;
		const int targetY = mainPath[routeRank + 1].second;
		if (!InBounds(sourceX, sourceY) || !InBounds(targetX, targetY)) return false;
		if (StageForRank(Grid[sourceY][sourceX].pathRank) != stage ||
			StageForRank(Grid[targetY][targetX].pathRank) != stage)
			return false;

		TArray<uint8_t> visited;
		TArray<int> predecessor;
		visited.Resize(W * H);
		predecessor.Resize(W * H);
		for (unsigned int i = 0; i < visited.Size(); ++i)
		{
			visited[i] = 0;
			predecessor[i] = -1;
		}
		TArray<std::pair<int, int>> queue;
		queue.Push(std::make_pair(sourceX, sourceY));
		visited[sourceY * W + sourceX] = 1;
		predecessor[sourceY * W + sourceX] = sourceY * W + sourceX;
		for (unsigned int qi = 0; qi < queue.Size(); ++qi)
		{
			const int x = queue[qi].first;
			const int y = queue[qi].second;
			const ProcGenCell& cell = Grid[y][x];
			for (int direction = 0; direction < 4; ++direction)
			{
				if (!cell.conn[direction]) continue;
				const int nx = x + DX[direction];
				const int ny = y + DY[direction];
				if (!InBounds(nx, ny) || !Grid[ny][nx].present ||
					StageForRank(Grid[ny][nx].pathRank) != stage)
					continue;
				const bool isPlannedEdge =
					(x == sourceX && y == sourceY && nx == targetX && ny == targetY) ||
					(x == targetX && y == targetY && nx == sourceX && ny == sourceY);
				if (isPlannedEdge) continue;
				const int index = ny * W + nx;
				if (visited[index]) continue;
				visited[index] = 1;
				predecessor[index] = y * W + x;
				if (nx == targetX && ny == targetY)
				{
					for (int current = index;; current = predecessor[current])
					{
						path.Push(std::make_pair(current % W, current / W));
						if (current == sourceY * W + sourceX) break;
					}
					for (unsigned int first = 0, last = path.Size() - 1;
						first < last; ++first, --last)
						std::swap(path[first], path[last]);
					return true;
				}
				queue.Push(std::make_pair(nx, ny));
			}
		}
		return false;
	};
	auto IsSameStageBridge = [&](int routeRank, int stage) -> bool
	{
		TArray<std::pair<int, int>> bypass;
		return !FindSameStageBypass(routeRank, stage, bypass);
	};
	auto RetractSameStageBypasses = [&](int routeRank, int stage) -> bool
	{
		struct RemovedConnection
		{
			int x = 0;
			int y = 0;
			int direction = 0;
		};
		TArray<RemovedConnection> removed;
		auto RestoreRemoved = [&]()
		{
			for (unsigned int index = 0; index < removed.Size(); ++index)
			{
				const RemovedConnection& connection = removed[index];
				const int nx = connection.x + DX[connection.direction];
				const int ny = connection.y + DY[connection.direction];
				Grid[connection.y][connection.x].conn[connection.direction] = true;
				Grid[ny][nx].conn[OPP[connection.direction]] = true;
			}
		};
		// The normal pass deliberately leaves every key and locked landmark edge
		// untouched. On a compact map that can leave one otherwise redundant side
		// of a key landmark as the last bypass around a required stair. Before
		// relaxing that conservative filter, prove that cutting this exact edge
		// retains every cell in the current lock stage and in the whole map. This
		// is topology-only and consumes no layout RNG, so it is a deterministic
		// last resort rather than a new generation path.
		auto RetractionPreservesConnectivity = [&](int cutX, int cutY,
			int cutDirection) -> bool
		{
			const int cutNX = cutX + DX[cutDirection];
			const int cutNY = cutY + DY[cutDirection];
			if (!InBounds(cutX, cutY) || !InBounds(cutNX, cutNY) ||
				!Grid[cutY][cutX].present || !Grid[cutNY][cutNX].present)
				return false;

			TArray<uint8_t> visited;
			visited.Resize(W * H);
			for (unsigned int index = 0; index < visited.Size(); ++index)
				visited[index] = 0;
			const int rootX = mainPath[routeRank].first;
			const int rootY = mainPath[routeRank].second;
			if (!InBounds(rootX, rootY) || !Grid[rootY][rootX].present ||
				StageForRank(Grid[rootY][rootX].pathRank) != stage)
				return false;

			TArray<std::pair<int, int>> queue;
			queue.Push(std::make_pair(rootX, rootY));
			visited[rootY * W + rootX] = 1;
			for (unsigned int queueIndex = 0; queueIndex < queue.Size(); ++queueIndex)
			{
				const int x = queue[queueIndex].first;
				const int y = queue[queueIndex].second;
				const ProcGenCell& cell = Grid[y][x];
				for (int direction = 0; direction < 4; ++direction)
				{
					if (!cell.conn[direction]) continue;
					if ((x == cutX && y == cutY && direction == cutDirection) ||
						(x == cutNX && y == cutNY && direction == OPP[cutDirection]))
						continue;
					const int nx = x + DX[direction];
					const int ny = y + DY[direction];
					if (!InBounds(nx, ny) || !Grid[ny][nx].present ||
						StageForRank(Grid[ny][nx].pathRank) != stage)
						continue;
					const int cellIndex = ny * W + nx;
					if (visited[cellIndex]) continue;
					visited[cellIndex] = 1;
					queue.Push(std::make_pair(nx, ny));
				}
			}
			for (int y = 1; y < H - 1; ++y)
			{
				for (int x = 1; x < W - 1; ++x)
				{
					if (Grid[y][x].present &&
						StageForRank(Grid[y][x].pathRank) == stage &&
						!visited[y * W + x])
						return false;
				}
			}

			// The stage-local proof protects pre-key traversal. Also retain the
			// complete graph proof: this side chord must not strand a later district
			// or an optional ordinary space through an unexpected connection shape.
			for (unsigned int index = 0; index < visited.Size(); ++index)
				visited[index] = 0;
			if (!Grid[sy][sx].present) return false;
			queue.Clear();
			queue.Push(std::make_pair(sx, sy));
			visited[sy * W + sx] = 1;
			for (unsigned int queueIndex = 0; queueIndex < queue.Size(); ++queueIndex)
			{
				const int x = queue[queueIndex].first;
				const int y = queue[queueIndex].second;
				const ProcGenCell& cell = Grid[y][x];
				for (int direction = 0; direction < 4; ++direction)
				{
					if (!cell.conn[direction]) continue;
					if ((x == cutX && y == cutY && direction == cutDirection) ||
						(x == cutNX && y == cutNY && direction == OPP[cutDirection]))
						continue;
					const int nx = x + DX[direction];
					const int ny = y + DY[direction];
					if (!InBounds(nx, ny) || !Grid[ny][nx].present) continue;
					const int cellIndex = ny * W + nx;
					if (visited[cellIndex]) continue;
					visited[cellIndex] = 1;
					queue.Push(std::make_pair(nx, ny));
				}
			}
			for (int y = 1; y < H - 1; ++y)
			{
				for (int x = 1; x < W - 1; ++x)
				{
					if (Grid[y][x].present && !visited[y * W + x]) return false;
				}
			}
			return true;
		};

		for (int pass = 0; pass < W * H; ++pass)
		{
			TArray<std::pair<int, int>> bypass;
			if (!FindSameStageBypass(routeRank, stage, bypass))
			{
				// The stage remains coherent and connected through the nominated
				// main-route edge; only a reconnection was retracted. Record the
				// honest compact fallback rather than advertising a retained ring or
				// fork that no longer exists at this exact beat.
				if (removed.Size() > 0)
					blueprint.RealizedStageShapes[stage] = PGSS_Spine;
				return true;
			}

			int chosenSegment = -1;
			uint32_t chosenScore = UINT32_MAX;
			for (unsigned int index = 0; index + 1 < bypass.Size(); ++index)
			{
				const int ax = bypass[index].first;
				const int ay = bypass[index].second;
				const int bx = bypass[index + 1].first;
				const int by = bypass[index + 1].second;
				const ProcGenCell& first = Grid[ay][ax];
				const ProcGenCell& second = Grid[by][bx];
				const bool mainRouteEdge = first.onMainPath && second.onMainPath &&
					abs(first.pathRank - second.pathRank) == 1;
				// Do not retract a mandatory pad, keyed threshold, or ordinary
				// main-route edge. Any remaining edge in this alternate path is a
				// same-stage reconnection and is safe to remove: the planned edge
				// itself completes the cycle.
				if (mainRouteEdge || first.hasPlayerStart || second.hasPlayerStart ||
					first.hasKey || second.hasKey || first.hasExit || second.hasExit ||
					first.isLocked || second.isLocked || first.reservedSecret ||
					second.reservedSecret)
					continue;
				const uint32_t score = MixProcGenHash(blueprint.RecipeHash ^
					(uint32_t)(stage + 1) * 0x94d049bbu ^
					(uint32_t)(std::min(ay * W + ax, by * W + bx) + 1) * 0x27d4eb2du);
				if (score < chosenScore)
				{
					chosenScore = score;
					chosenSegment = (int)index;
				}
			}
			if (chosenSegment < 0)
			{
				// Keep keys, locks, secret leaves, and every primary route edge intact.
				// This narrowly admits only an extra start/exit chord after both the
				// stage-local and full retained-graph proofs have passed.
				for (unsigned int index = 0; index + 1 < bypass.Size(); ++index)
				{
					const int ax = bypass[index].first;
					const int ay = bypass[index].second;
					const int bx = bypass[index + 1].first;
					const int by = bypass[index + 1].second;
					const ProcGenCell& first = Grid[ay][ax];
					const ProcGenCell& second = Grid[by][bx];
					const bool mainRouteEdge = first.onMainPath && second.onMainPath &&
						abs(first.pathRank - second.pathRank) == 1;
					const bool touchesStartOrExit = first.hasPlayerStart || second.hasPlayerStart ||
						first.hasExit || second.hasExit;
					if (mainRouteEdge || !touchesStartOrExit || first.hasKey || second.hasKey ||
						first.isLocked || second.isLocked || first.reservedSecret ||
						second.reservedSecret)
						continue;
					int direction = -1;
					for (int candidate = 0; candidate < 4; ++candidate)
						if (ax + DX[candidate] == bx && ay + DY[candidate] == by)
							direction = candidate;
					if (direction < 0 ||
						!RetractionPreservesConnectivity(ax, ay, direction))
						continue;
					const uint32_t score = MixProcGenHash(blueprint.RecipeHash ^
						(uint32_t)(stage + 1) * 0x632be59bu ^
						(uint32_t)(std::min(ay * W + ax, by * W + bx) + 1) * 0x85157af5u);
					if (score < chosenScore)
					{
						chosenScore = score;
						chosenSegment = (int)index;
					}
				}
			}
			if (chosenSegment < 0)
			{
				RestoreRemoved();
				return false;
			}

			const int ax = bypass[chosenSegment].first;
			const int ay = bypass[chosenSegment].second;
			const int bx = bypass[chosenSegment + 1].first;
			const int by = bypass[chosenSegment + 1].second;
			int direction = -1;
			for (int candidate = 0; candidate < 4; ++candidate)
				if (ax + DX[candidate] == bx && ay + DY[candidate] == by)
					direction = candidate;
			if (direction < 0)
			{
				RestoreRemoved();
				return false;
			}
			Grid[ay][ax].conn[direction] = false;
			Grid[by][bx].conn[OPP[direction]] = false;
			removed.Push({ ax, ay, direction });
		}
		RestoreRemoved();
		return false;
	};
	auto TryPlaceVerticalIntent = [&](int stage, EProcGenVerticalIntent intent,
		int rise) -> bool
	{
		const int firstRank = stage > 0 ? gateRanks[stage - 1] : 0;
		const int lastRank = stage < (int)gateRanks.Size() ?
			gateRanks[stage] - 1 : (int)mainPath.Size() - 1;
		int selectedRank = -1;
		uint32_t selectedScore = UINT32_MAX;
		struct RankedFallback
		{
			uint32_t score = 0;
			int rank = -1;
		};
		TArray<RankedFallback> fallbackCandidates;
		for (int pass = 0; pass < 2 && selectedRank < 0; ++pass)
		{
			for (int rank = firstRank + 1; rank < lastRank; ++rank)
			{
				if (verticalRankUsed[rank] || verticalRankUsed[rank + 1]) continue;
				const int ax = mainPath[rank].first;
				const int ay = mainPath[rank].second;
				const int bx = mainPath[rank + 1].first;
				const int by = mainPath[rank + 1].second;
				const ProcGenCell& first = Grid[ay][ax];
				const ProcGenCell& second = Grid[by][bx];
				if (!first.onMainPath || !second.onMainPath ||
					StageForRank(first.pathRank) != stage ||
					StageForRank(second.pathRank) != stage)
					continue;
				if (first.hasPlayerStart || second.hasPlayerStart || first.hasExit || second.hasExit ||
					first.hasKey || second.hasKey || first.isLocked || second.isLocked ||
					first.reservedSecret || second.reservedSecret)
					continue;
				if (pass == 0 && (first.isHub || second.isHub || first.isArena || second.isArena))
					continue;

				bool nearGate = false;
				for (unsigned int k = 0; k < keys.Size(); ++k)
				{
					// A key's branch *anchor* is ordinary main-route floor, not the
					// key pad itself. It may safely host a stair junction as long as
					// neither endpoint is the key cell (checked above). Keep a full
					// buffer around the actual locked crossing, but do not spend the
					// only compact-map stair candidate merely because a side branch
					// leaves nearby.
					if (abs(rank - keys[k].gateRank) <= 1 ||
						abs(rank + 1 - keys[k].gateRank) <= 1)
					{
						nearGate = true;
						break;
					}
				}
				if (nearGate) continue;

				const uint32_t score = MixProcGenHash(blueprint.RecipeHash ^
					(uint32_t)(stage + 1) * 0x27d4eb2du ^ (uint32_t)rank * 0x165667b1u);
				if (IsSameStageBridge(rank, stage))
				{
					if (score < selectedScore)
					{
						selectedScore = score;
						selectedRank = rank;
					}
				}
				else if (pass == 1)
				{
					// The first pass may have ruled this out only because it is a
					// landmark edge. Keep every pass-two bypass candidate so one
					// protected local loop cannot make us give up before trying the
					// next deterministic safe main-route connector.
					fallbackCandidates.Push({ score, rank });
				}
			}
		}
		// A retraction can legitimately refuse one candidate: its only alternate
		// path may run through the start pad, a key, or another protected landmark.
		// Try the remaining same-stage candidates in stable hash order before
		// omitting an authored beat. RetractSameStageBypasses restores every failed
		// trial, so no rejected candidate changes the following attempt or RNG state.
		TArray<bool> fallbackTried;
		fallbackTried.Resize(fallbackCandidates.Size());
		for (unsigned int index = 0; index < fallbackTried.Size(); ++index)
			fallbackTried[index] = false;
		for (unsigned int attempt = 0;
			selectedRank < 0 && attempt < fallbackCandidates.Size(); ++attempt)
		{
			int bestIndex = -1;
			uint32_t bestScore = UINT32_MAX;
			for (unsigned int index = 0; index < fallbackCandidates.Size(); ++index)
			{
				const RankedFallback& candidate = fallbackCandidates[index];
				if (fallbackTried[index] ||
					(candidate.score > bestScore) ||
					(candidate.score == bestScore && bestIndex >= 0 &&
						candidate.rank >= fallbackCandidates[bestIndex].rank))
					continue;
				bestScore = candidate.score;
				bestIndex = (int)index;
			}
			if (bestIndex < 0) break;
			fallbackTried[bestIndex] = true;
			if (RetractSameStageBypasses(fallbackCandidates[bestIndex].rank, stage))
				selectedRank = fallbackCandidates[bestIndex].rank;
		}
		if (selectedRank < 0) return false;
		ProcGenCell& anchor = Grid[mainPath[selectedRank].second][mainPath[selectedRank].first];
		anchor.verticalIntent = intent;
		anchor.verticalRise = rise;
		anchor.verticalAnchor = true;
		// Keep the exact next main-route cell atomic through room composition as
		// well. It is an approach sentinel, not a second stair beat: the source
		// retains the signed intent while the successor stays flat. Without this,
		// a broad composed room can absorb the successor and replace its precise
		// rank with an earlier one, leaving ApplyCoherence unable to bind the
		// planned connector after merging.
		ProcGenCell& approach = Grid[mainPath[selectedRank + 1].second]
			[mainPath[selectedRank + 1].first];
			approach.verticalIntent = PGVI_Flat;
			approach.verticalRise = 0;
			approach.verticalAnchor = true;
			const int direction = DirectionBetween(mainPath[selectedRank].first,
				mainPath[selectedRank].second, mainPath[selectedRank + 1].first,
				mainPath[selectedRank + 1].second);
			if (direction >= 0)
			{
				// The UDMF pass turns this identity into one or more eight-unit
				// sectors. It is deliberately tied to the graph edge, not a room id,
				// so later room merging cannot make the manifest ambiguous.
				const int chain = stage * 4096 + selectedRank;
				anchor.connectionStairChain[direction] = chain;
				approach.connectionStairChain[OPP[direction]] = chain;
			}
			verticalRankUsed[selectedRank] = true;
		verticalRankUsed[selectedRank + 1] = true;
		return true;
	};

	// A very short early stage can consist almost entirely of a start, key, and
	// gate buffer. Do not silently lose a required Varied/Dramatic beat in that
	// case: move its hash-planned intent to the next feasible *flat* stage. The
	// transfer is deterministic, stays within one lock stage, and never relaxes
	// the exclusion around pads and keyed thresholds.
	struct PlannedVerticalBeat
	{
		int stage = -1;
		EProcGenVerticalIntent intent = PGVI_Flat;
		int rise = 0;
	};
		TArray<PlannedVerticalBeat> plannedVerticalBeats;
		// A realized record is earned only by a connector that survived the actual
		// grid. Do not leave the recipe's optimistic default visible when a compact
		// topology has to omit a scenic feature.
		blueprint.RealizedVerticalBeats = 0;
		for (int stage = 0; stage < RunBlueprint::MaxStages; ++stage)
		{
			blueprint.RealizedStageVerticalIntents[stage] = PGVI_Flat;
			blueprint.RealizedStageVerticalRises[stage] = 0;
		}
		for (int stage = 0; stage < blueprint.RealizedStageCount; ++stage)
	{
		if (blueprint.StageVerticalIntents[stage] == PGVI_Flat) continue;
		plannedVerticalBeats.Push({ stage, blueprint.StageVerticalIntents[stage],
			blueprint.StageVerticalRises[stage] });
	}
	for (const PlannedVerticalBeat& beat : plannedVerticalBeats)
	{
		int realizedStage = -1;
		for (int offset = 0; offset < blueprint.RealizedStageCount; ++offset)
		{
			const int candidate = (beat.stage + offset) % blueprint.RealizedStageCount;
			if (candidate != beat.stage &&
				blueprint.StageVerticalIntents[candidate] != PGVI_Flat)
				continue;
			if (!TryPlaceVerticalIntent(candidate, beat.intent, beat.rise)) continue;
			realizedStage = candidate;
			break;
		}
			if (realizedStage < 0)
			{
				// A map must remain playable when a compact or unusually branchy grid
				// cannot keep an optional scenic elevation chain clear of pads and
				// locks. The graph's ordinary terrain field still supplies safe local
				// height variation; only this exact authored connector is omitted.
				continue;
			}
		blueprint.RealizedStageVerticalIntents[realizedStage] = beat.intent;
		blueprint.RealizedStageVerticalRises[realizedStage] = beat.rise;
		blueprint.RealizedVerticalBeats++;
	}

	// Assign the room-facing identity only after all topology has settled. A
	// support cell inherits its stage/district so merger and UDMF passes can
	// preserve a landmark's circulation rather than turning it into an
	// unrelated decorative room.
	for (int y = 1; y < H - 1; ++y)
	{
		for (int x = 1; x < W - 1; ++x)
		{
			if (!keep[y][x]) continue;
			ProcGenCell& cell = Grid[y][x];
			const int stage = clamp(StageForRank(cell.pathRank), 0,
				blueprint.RealizedStageCount - 1);
			cell.stageShape = blueprint.RealizedStageShapes[stage];
			cell.districtRole = blueprint.StageDistrictRoles[stage];
			cell.materialFamily = blueprint.StageMaterialFamilies[stage];
			cell.elevationRole = blueprint.StageElevationRoles[stage];
			cell.elevationTarget = cell.elevationRole == PGER_Highland ||
				cell.elevationRole == PGER_Basin ?
				blueprint.MainRouteElevationTarget : 0;
			if (cell.hasExit)
				cell.landmarkArchetype = PGLA_Fortress;
			else if (cell.isLocked)
				cell.landmarkArchetype = PGLA_Gatehouse;
			else if (cell.hasKey)
				cell.landmarkArchetype = (themeStyle == ThemeHell || themeStyle == ThemeGothic) ?
					PGLA_ShrineTerrace : blueprint.RealizedStageLandmarks[stage];
			else if (cell.verticalAnchor && cell.verticalIntent == PGVI_TerraceOverlook)
				cell.landmarkArchetype = PGLA_ShrineTerrace;
			else if (cell.verticalAnchor && cell.verticalIntent == PGVI_BridgeApproach)
				cell.landmarkArchetype = PGLA_BridgeBasin;
			else if (cell.isHub || cell.isArena)
				cell.landmarkArchetype = blueprint.RealizedStageLandmarks[stage];
		}
	}

	// Materialize the progression stage on every kept cell, then audit every
	// connection before room composition can hide its coarse-grid origin. A
	// stage boundary may have exactly one crossing: the directed keyed edge that
	// enters its gate rank. Ordinary portals and unlocked doors are never valid
	// substitutes on that cut.
	for (int y = 1; y < H - 1; y++)
		for (int x = 1; x < W - 1; x++)
			if (keep[y][x])
				Grid[y][x].lockStage = StageForRank(Grid[y][x].pathRank);

	// Large Dramatic maps promise both a main highland/basin and a reachable
	// optional district at the opposite altitude.  Room composition can collapse
	// an otherwise long side limb through ordinary doors, leaving too few real
	// <=64-unit stair transitions between the two terraces.  Reserve an existing
	// same-stage limb before merging: it remains an ordinary side route, its
	// cells cannot merge into one chamber, and only optional rejoin edges that
	// would shortcut the chain are retracted.  The later terrain solver still
	// proves all floor deltas, so failure here simply leaves the normal safe
	// terrain rather than manufacturing an inaccessible extreme.
	if (Verticality == 2 && Size >= 5)
	{
		constexpr int TerrainLimbMinimumSteps = 6; // +/-192 needs six <=64 walks.
		struct TerrainLimbCandidate
		{
			int source = -1;
			int endpoint = -1;
			int steps = 0;
			uint32_t score = UINT32_MAX;
		};
		struct TerrainLimbEdge
		{
			int index = -1;
			int direction = -1;
		};
		auto TerrainCellSafe = [](const ProcGenCell& cell) -> bool
		{
			// Arena and hub support cells are eligible only when they carry none
			// of the critical pads below. Reserving them keeps their terrain limb
			// atomic; the post-retraction distance and full-connectivity audits
			// still prove this is an existing playable route, not new topology.
			return cell.present && !cell.hasPlayerStart && !cell.hasExit &&
				!cell.hasBoss && !cell.hasKey && !cell.isLocked &&
				!cell.reservedSecret && !cell.verticalAnchor;
		};
		auto BuildTerrainLimbPath = [&](int source, int endpoint,
			TArray<int>& path) -> bool
		{
			path.Clear();
			if (source < 0 || endpoint < 0) return false;
			const int sourceX = source % W;
			const int sourceY = source / W;
			const int stage = Grid[sourceY][sourceX].lockStage;
			TArray<int> previous;
			previous.Resize(W * H);
			for (unsigned int index = 0; index < previous.Size(); ++index)
				previous[index] = -2;
			TArray<int> queue;
			previous[source] = -1;
			queue.Push(source);
			for (unsigned int qi = 0; qi < queue.Size(); ++qi)
			{
				const int current = queue[qi];
				if (current == endpoint) break;
				const int x = current % W;
				const int y = current / W;
				const ProcGenCell& cell = Grid[y][x];
				for (int direction = 0; direction < 4; ++direction)
				{
					if (!cell.conn[direction]) continue;
					const int nx = x + DX[direction];
					const int ny = y + DY[direction];
					if (!InBounds(nx, ny)) continue;
					const int next = ny * W + nx;
					if (previous[next] != -2) continue;
					const ProcGenCell& other = Grid[ny][nx];
					if (other.lockStage != stage || !TerrainCellSafe(other) ||
						other.onMainPath)
						continue;
					previous[next] = current;
					queue.Push(next);
				}
			}
			if (previous[endpoint] == -2) return false;
			for (int current = endpoint; current >= 0; current = previous[current])
				path.Push(current);
			for (unsigned int first = 0, last = path.Size() - 1; first < last;
				++first, --last)
				std::swap(path[first], path[last]);
			return path.Size() >= 2 && path[0] == source;
		};
		TArray<int> terrainSources;
		for (unsigned int rank = 1; rank + 1 < mainPath.Size(); ++rank)
		{
			const int x = mainPath[rank].first;
			const int y = mainPath[rank].second;
			const ProcGenCell& cell = Grid[y][x];
			if (!TerrainCellSafe(cell) || !cell.onMainPath) continue;
			const int index = y * W + x;
			const uint32_t score = MixProcGenHash(blueprint.RecipeHash ^
				(uint32_t)(rank + 1) * 0x85ebca6bu ^ 0x19f4a7cdu);
			int insert = (int)terrainSources.Size();
			for (unsigned int candidate = 0; candidate < terrainSources.Size(); ++candidate)
			{
				const int other = terrainSources[candidate];
				const int otherRank = Grid[other / W][other % W].pathRank;
				const uint32_t otherScore = MixProcGenHash(blueprint.RecipeHash ^
					(uint32_t)(otherRank + 1) * 0x85ebca6bu ^ 0x19f4a7cdu);
				if (score < otherScore)
				{
					insert = (int)candidate;
					break;
				}
			}
			terrainSources.Insert(insert, index);
		}
		if (terrainSources.Size() > 32) terrainSources.Resize(32);

		TArray<TerrainLimbCandidate> terrainCandidates;
		auto ConsiderTerrainCandidate = [&](int source, int endpoint, int steps)
		{
			const uint32_t score = MixProcGenHash(blueprint.RecipeHash ^
				(uint32_t)(source + 1) * 0x9e3779b9u ^
				(uint32_t)(endpoint + 1) * 0x85ebca6bu);
			TerrainLimbCandidate candidate;
			candidate.source = source;
			candidate.endpoint = endpoint;
			candidate.steps = steps;
			candidate.score = score;
			auto IsBetter = [](const TerrainLimbCandidate& first,
				const TerrainLimbCandidate& second) -> bool
			{
				return first.steps != second.steps ? first.steps < second.steps :
					first.score < second.score;
			};
			if (terrainCandidates.Size() < 64)
			{
				terrainCandidates.Push(candidate);
				return;
			}
			int worst = 0;
			for (unsigned int index = 1; index < terrainCandidates.Size(); ++index)
				if (IsBetter(terrainCandidates[worst], terrainCandidates[index]))
					worst = (int)index;
			if (IsBetter(candidate, terrainCandidates[worst]))
				terrainCandidates[worst] = candidate;
		};
		for (unsigned int sourceOrder = 0; sourceOrder < terrainSources.Size(); ++sourceOrder)
		{
			const int source = terrainSources[sourceOrder];
			const int sourceX = source % W;
			const int sourceY = source / W;
			const int stage = Grid[sourceY][sourceX].lockStage;
			TArray<int> previous;
			TArray<int> distance;
			previous.Resize(W * H);
			distance.Resize(W * H);
			for (unsigned int index = 0; index < previous.Size(); ++index)
			{
				previous[index] = -2;
				distance[index] = -1;
			}
			TArray<int> queue;
			previous[source] = -1;
			distance[source] = 0;
			queue.Push(source);
			for (unsigned int qi = 0; qi < queue.Size(); ++qi)
			{
				const int current = queue[qi];
				const int x = current % W;
				const int y = current / W;
				const ProcGenCell& cell = Grid[y][x];
				for (int direction = 0; direction < 4; ++direction)
				{
					if (!cell.conn[direction]) continue;
					const int nx = x + DX[direction];
					const int ny = y + DY[direction];
					if (!InBounds(nx, ny)) continue;
					const int next = ny * W + nx;
					if (previous[next] != -2) continue;
					const ProcGenCell& other = Grid[ny][nx];
					if (other.lockStage != stage || !TerrainCellSafe(other) ||
						other.onMainPath)
						continue;
					previous[next] = current;
					distance[next] = distance[current] + 1;
					queue.Push(next);
				}
			}
			for (unsigned int index = 0; index < distance.Size(); ++index)
			{
				if (distance[index] < TerrainLimbMinimumSteps) continue;
				const ProcGenCell& endpoint = Grid[index / W][index % W];
				if (!TerrainCellSafe(endpoint) || endpoint.onMainPath ||
					endpoint.branchDepth < 1)
					continue;
				ConsiderTerrainCandidate(source, (int)index, distance[index]);
			}
		}
		TArray<bool> terrainCandidateTried;
		terrainCandidateTried.Resize(terrainCandidates.Size());
		for (unsigned int index = 0; index < terrainCandidateTried.Size(); ++index)
			terrainCandidateTried[index] = false;
		for (unsigned int attempt = 0; attempt < terrainCandidates.Size(); ++attempt)
		{
			int best = -1;
			for (unsigned int index = 0; index < terrainCandidates.Size(); ++index)
			{
				if (terrainCandidateTried[index]) continue;
				if (best < 0 || terrainCandidates[index].steps < terrainCandidates[best].steps ||
					(terrainCandidates[index].steps == terrainCandidates[best].steps &&
					 terrainCandidates[index].score < terrainCandidates[best].score))
					best = (int)index;
			}
			if (best < 0) break;
			terrainCandidateTried[best] = true;
			const TerrainLimbCandidate& candidate = terrainCandidates[best];
			TArray<int> path;
			if (!BuildTerrainLimbPath(candidate.source, candidate.endpoint, path) ||
				(int)path.Size() - 1 < TerrainLimbMinimumSteps)
				continue;
			TArray<int> pathOrder;
			pathOrder.Resize(W * H);
			for (unsigned int index = 0; index < pathOrder.Size(); ++index)
				pathOrder[index] = -1;
			for (unsigned int index = 0; index < path.Size(); ++index)
				pathOrder[path[index]] = (int)index;
			TArray<TerrainLimbEdge> removedEdges;
			auto AddRemovedEdge = [&](int index, int direction)
			{
				for (unsigned int existing = 0; existing < removedEdges.Size(); ++existing)
				{
					const int existingIndex = removedEdges[existing].index;
					const int existingDirection = removedEdges[existing].direction;
					const int existingNext = existingIndex + DX[existingDirection] +
						DY[existingDirection] * W;
					const int next = index + DX[direction] + DY[direction] * W;
					if ((existingIndex == index && existingDirection == direction) ||
						(existingIndex == next && existingNext == index))
						return;
				}
				removedEdges.Push({ index, direction });
			};
			bool removable = true;
			for (unsigned int position = 1; position < path.Size() && removable; ++position)
			{
				const int index = path[position];
				const int x = index % W;
				const int y = index / W;
				const int stage = Grid[y][x].lockStage;
				for (int direction = 0; direction < 4; ++direction)
				{
					if (!Grid[y][x].conn[direction]) continue;
					const int nx = x + DX[direction];
					const int ny = y + DY[direction];
					if (!InBounds(nx, ny))
					{
						removable = false;
						break;
					}
					const int next = ny * W + nx;
					const int nextPosition = pathOrder[next];
					if (nextPosition >= 0 && abs(nextPosition - (int)position) == 1)
						continue;
					const ProcGenCell& other = Grid[ny][nx];
					// A key/gate/landmark edge is part of the existing progression
					// graph and must not be retracted just to make this scenic limb
					// look isolated. Leave it intact, then prove below that it does
					// not shorten the reserved source-to-endpoint stair route.
					if (other.lockStage != stage || other.hasPlayerStart || other.hasExit ||
						other.hasBoss || other.hasKey || other.isLocked ||
						other.reservedSecret || other.verticalAnchor || other.onMainPath)
						continue;
					AddRemovedEdge(index, direction);
				}
			}
			if (!removable) continue;
			auto SetTerrainLimbEdge = [&](const TerrainLimbEdge& edge, bool connected)
			{
				const int x = edge.index % W;
				const int y = edge.index / W;
				const int nx = x + DX[edge.direction];
				const int ny = y + DY[edge.direction];
				Grid[y][x].conn[edge.direction] = connected;
				Grid[ny][nx].conn[OPP[edge.direction]] = connected;
			};
			for (const TerrainLimbEdge& edge : removedEdges)
				SetTerrainLimbEdge(edge, false);

			const int start = mainPath[0].second * W + mainPath[0].first;
			const int terrainStage = Grid[candidate.source / W][candidate.source % W].lockStage;
			int presentCells = 0;
			for (int y = 0; y < H; ++y)
				for (int x = 0; x < W; ++x)
					if (Grid[y][x].present) ++presentCells;
			auto CountReachableCells = [&]() -> int
			{
				TArray<uint8_t> reachable;
				reachable.Resize(W * H);
				for (unsigned int index = 0; index < reachable.Size(); ++index)
					reachable[index] = 0;
				TArray<int> queue;
				reachable[start] = 1;
				queue.Push(start);
				for (unsigned int qi = 0; qi < queue.Size(); ++qi)
				{
					const int index = queue[qi];
					const int x = index % W;
					const int y = index / W;
					for (int direction = 0; direction < 4; ++direction)
					{
						if (!Grid[y][x].conn[direction]) continue;
						const int nx = x + DX[direction];
						const int ny = y + DY[direction];
						if (!InBounds(nx, ny) || !Grid[ny][nx].present) continue;
						const int next = ny * W + nx;
						if (reachable[next]) continue;
						reachable[next] = 1;
						queue.Push(next);
					}
				}
				return (int)queue.Size();
			};
			// Retaining a support edge is permitted only when it reconnects a
			// genuine side branch and does not shorten the six-step source-to-end
			// stair chain.  This keeps ordinary side content reachable without
			// manufacturing a terrain limb through a gate or a critical landmark.
			auto TerrainLimbDistance = [&]() -> int
			{
				TArray<int> limbDistance;
				limbDistance.Resize(W * H);
				for (unsigned int index = 0; index < limbDistance.Size(); ++index)
					limbDistance[index] = -1;
				TArray<int> limbQueue;
				limbDistance[candidate.source] = 0;
				limbQueue.Push(candidate.source);
				for (unsigned int qi = 0; qi < limbQueue.Size(); ++qi)
				{
					const int index = limbQueue[qi];
					const int x = index % W;
					const int y = index / W;
					for (int direction = 0; direction < 4; ++direction)
					{
						if (!Grid[y][x].conn[direction]) continue;
						const int nx = x + DX[direction];
						const int ny = y + DY[direction];
						if (!InBounds(nx, ny)) continue;
						const int next = ny * W + nx;
						if (limbDistance[next] >= 0 || !Grid[ny][nx].present ||
							Grid[ny][nx].lockStage != terrainStage)
							continue;
						limbDistance[next] = limbDistance[index] + 1;
						limbQueue.Push(next);
					}
				}
				return limbDistance[candidate.endpoint];
			};
			int reachableCount = CountReachableCells();
			for (unsigned int edgeIndex = 0; edgeIndex < removedEdges.Size() &&
				reachableCount < presentCells; ++edgeIndex)
			{
				const TerrainLimbEdge& edge = removedEdges[edgeIndex];
				SetTerrainLimbEdge(edge, true);
				const int trialReachable = CountReachableCells();
				if (trialReachable > reachableCount &&
					TerrainLimbDistance() >= TerrainLimbMinimumSteps)
				{
					reachableCount = trialReachable;
					continue;
				}
				SetTerrainLimbEdge(edge, false);
			}
			if (reachableCount < presentCells ||
				TerrainLimbDistance() < TerrainLimbMinimumSteps)
			{
				for (const TerrainLimbEdge& edge : removedEdges)
					SetTerrainLimbEdge(edge, true);
				continue;
			}
			for (unsigned int index = 0; index < path.Size(); ++index)
				Grid[path[index] / W][path[index] % W].terrainRouteReservation = true;
			break;
		}
	}

	TArray<int> gateCrossings;
	gateCrossings.Resize(keys.Size());
	for (unsigned int k = 0; k < gateCrossings.Size(); k++)
	{
		gateCrossings[k] = 0;
		if (StageForRank(keys[k].anchorRank) + 1 != StageForRank(keys[k].gateRank))
		{
			LastError = "A key was placed outside the stage immediately before its gate";
			return false;
		}
	}

	for (int y = 1; y < H - 1; y++)
	{
		for (int x = 1; x < W - 1; x++)
		{
			if (!keep[y][x]) continue;
			for (int direction : { DIR_E, DIR_S })
			{
				if (!Grid[y][x].conn[direction]) continue;
				const int nx = x + DX[direction];
				const int ny = y + DY[direction];
				if (!InBounds(nx, ny) || !keep[ny][nx]) continue;
				const ProcGenCell& first = Grid[y][x];
				const ProcGenCell& second = Grid[ny][nx];
				if (first.lockStage == second.lockStage)
				{
					const bool ownsLock = (first.isLocked && first.lockDir == direction) ||
						(second.isLocked && second.lockDir == OPP[direction]);
					if (ownsLock)
					{
						LastError = "A keyed edge does not separate two progression stages";
						return false;
					}
					continue;
				}

				if (abs(first.lockStage - second.lockStage) != 1)
				{
					LastError = "A connection skips one or more key progression stages";
					return false;
				}

				const ProcGenCell& later = first.lockStage > second.lockStage ? first : second;
				const int directionToEarlier = first.lockStage > second.lockStage ?
					direction : OPP[direction];
				if (!later.isLocked || later.lockDir != directionToEarlier || later.lockType <= 0)
				{
					LastError = "An unlocked opening bypasses a key progression boundary";
					return false;
				}

				int gateIndex = -1;
				for (unsigned int k = 0; k < keys.Size(); k++)
				{
					if (later.pathRank == keys[k].gateRank && later.lockType == keys[k].type)
					{
						gateIndex = k;
						break;
					}
				}
				if (gateIndex < 0)
				{
					LastError = "A progression boundary is not owned by its planned key gate";
					return false;
				}
				gateCrossings[gateIndex]++;
			}
		}
	}
	for (unsigned int k = 0; k < gateCrossings.Size(); k++)
	{
		if (gateCrossings[k] != 1)
		{
			LastError = "A key gate does not own exactly one progression crossing";
			return false;
		}
	}

	for (int y = 0; y < H; y++)
	{
		for (int x = 0; x < W; x++)
		{
			if (!keep[y][x])
			{
				Grid[y][x] = ProcGenCell();
				continue;
			}

			Grid[y][x].present = true;
			Grid[y][x].neighborCount = 0;
			Grid[y][x].connectionCount = 0;
			for (int d = 0; d < 4; d++)
			{
				const int nx = x + DX[d];
				const int ny = y + DY[d];
				if (!InBounds(nx, ny) || !keep[ny][nx])
					Grid[y][x].conn[d] = false;
				else
				{
					Grid[y][x].neighborCount++;
					if (Grid[y][x].conn[d]) Grid[y][x].connectionCount++;
				}
			}
			if (Grid[y][x].connectionCount >= 3 && Grid[y][x].onMainPath)
				Grid[y][x].isHub = true;
		}
	}

	// Translate the blueprint into room-facing beats before composition. The
	// cards describe static encounter grammar; the room pass remains responsible
	// for choosing legal monsters, pickups, and concrete geometry.
	auto CardForRank = [&](int rank) -> EProcGenEncounterCard
	{
		static const EProcGenEncounterCard Expedition[] = {
			PGEC_Breather, PGEC_Skirmish, PGEC_CacheChallenge, PGEC_Crossfire,
			PGEC_Breather, PGEC_Pincer, PGEC_SetPiece
		};
		static const EProcGenEncounterCard Assault[] = {
			PGEC_Breather, PGEC_Skirmish, PGEC_Crossfire, PGEC_Breather,
			PGEC_Pincer, PGEC_HoldingLine, PGEC_Breather, PGEC_SetPiece
		};
		static const EProcGenEncounterCard Infiltration[] = {
			PGEC_Breather, PGEC_Ambush, PGEC_CacheChallenge, PGEC_Breather,
			PGEC_Crossfire, PGEC_Ambush, PGEC_Breather, PGEC_SetPiece
		};
		static const EProcGenEncounterCard Circuit[] = {
			PGEC_Breather, PGEC_Skirmish, PGEC_CacheChallenge, PGEC_Crossfire,
			PGEC_Breather, PGEC_Pincer, PGEC_HoldingLine, PGEC_SetPiece
		};
		static const EProcGenEncounterCard Siege[] = {
			PGEC_Breather, PGEC_HoldingLine, PGEC_Skirmish, PGEC_Breather,
			PGEC_Crossfire, PGEC_HoldingLine, PGEC_Breather, PGEC_SetPiece
		};

		const EProcGenEncounterCard* cards = Expedition;
		int cardCount = countof(Expedition);
		switch (blueprint.Profile)
		{
		case PGRP_Assault: cards = Assault; cardCount = countof(Assault); break;
		case PGRP_Infiltration: cards = Infiltration; cardCount = countof(Infiltration); break;
		case PGRP_Circuit: cards = Circuit; cardCount = countof(Circuit); break;
		case PGRP_Siege: cards = Siege; cardCount = countof(Siege); break;
		default: break;
		}
		const int index = clamp(rank * cardCount / std::max(1, (int)mainPath.Size()),
			0, cardCount - 1);
		return cards[index];
	};

	auto IsHighPressure = [](int card) -> bool
	{
		return card == PGEC_Crossfire || card == PGEC_Pincer || card == PGEC_Ambush ||
			card == PGEC_HoldingLine || card == PGEC_SetPiece;
	};

	auto CardThreat = [](int card) -> int
	{
		switch (card)
		{
		case PGEC_Crossfire: return 2;
		case PGEC_Pincer: return 2;
		case PGEC_HoldingLine: return 3;
		case PGEC_SetPiece: return 3;
		case PGEC_Skirmish: return 1;
		case PGEC_Ambush: return 1;
		case PGEC_CacheChallenge: return 1;
		default: return 0;
		}
	};

	for (int y = 1; y < H - 1; ++y)
	{
		for (int x = 1; x < W - 1; ++x)
		{
			if (!keep[y][x]) continue;
			ProcGenCell& cell = Grid[y][x];
			const int rank = std::max(0, cell.pathRank);
			const int phase = rank * 4 / std::max(1, (int)mainPath.Size() - 1);
			cell.arsenalTrack = blueprint.ArsenalTrack;
			// Finale intent is map-wide: every composed room can tailor its static
			// setup around the impending finale, while only the exit room emits it.
			cell.finaleCard = blueprint.FinaleCard;
			cell.featureMotif = PGFM_None;
			cell.featureMotifPriority = 0;
			cell.optionalArmory = false;
			cell.rewardPlan = PGRW_None;
			cell.district = clamp(rank * blueprint.DistrictCount /
				std::max(1, (int)mainPath.Size()), 0, blueprint.DistrictCount - 1);
			cell.runBeat = cell.onMainPath ? PGRB_Approach : PGRB_Optional;
			cell.encounterCard = cell.onMainPath ? CardForRank(rank) :
				(cell.branchDepth >= 2 ? PGEC_CacheChallenge : PGEC_Ambush);
			if (cell.hasPlayerStart)
			{
				cell.runBeat = PGRB_Opening;
				cell.encounterCard = PGEC_Breather;
				cell.rewardPlan = PGRW_Emergency;
			}
			else if (cell.hasExit)
			{
				cell.runBeat = PGRB_Finale;
				cell.encounterCard = PGEC_SetPiece;
				cell.finaleCard = blueprint.FinaleCard;
				cell.rewardPlan = PGRW_FinaleReserve;
			}
			else if (cell.hasKey)
			{
				cell.runBeat = PGRB_KeyObjective;
				cell.encounterCard = PGEC_SetPiece;
				cell.rewardPlan = PGRW_KeyReserve;
			}
			else if (cell.isHub)
			{
				cell.runBeat = PGRB_Recovery;
				cell.encounterCard = PGEC_Breather;
				cell.rewardPlan = PGRW_Emergency;
			}
			else if (cell.isArena || cell.isLocked)
			{
				cell.runBeat = PGRB_SetPiece;
				cell.encounterCard = cell.isLocked && blueprint.Profile == PGRP_Siege ?
					PGEC_HoldingLine : PGEC_SetPiece;
			}
			else if (cell.reservedSecret)
			{
				cell.encounterCard = PGEC_CacheChallenge;
				cell.rewardPlan = PGRW_Cache;
			}

			cell.threatBudget = std::max(0, phase + blueprint.ThreatCurve +
				CardThreat(cell.encounterCard) + (Difficulty - 2));
			if (!cell.onMainPath && !cell.hasKey) cell.threatBudget = std::max(0, cell.threatBudget - 1);
			cell.recoveryBudget = (cell.runBeat == PGRB_Recovery ||
				cell.encounterCard == PGEC_Breather) ? 1 : 0;
			if (cell.hasKey || cell.isArena || cell.hasExit)
				cell.recoveryBudget = std::max(cell.recoveryBudget, blueprint.RecoveryCadence);
		}
	}

	// A profile may pressure the player through two adjacent beats, but never a
	// third. The next ordinary main-route cell becomes a recovery/choice beat;
	// this applies after landmark expansion so it also covers actual arenas.
	bool recoveryDue = false;
	int pressureStreak = 0;
	for (int rank = 0; rank < (int)mainPath.Size(); ++rank)
	{
		ProcGenCell& cell = Grid[mainPath[rank].second][mainPath[rank].first];
		if (recoveryDue && !cell.hasExit && !cell.hasKey && !cell.isLocked)
		{
			cell.runBeat = PGRB_Recovery;
			cell.encounterCard = PGEC_Breather;
			cell.rewardPlan = PGRW_Emergency;
			cell.threatBudget = std::max(0, cell.threatBudget - 2);
			cell.recoveryBudget = std::max(cell.recoveryBudget, blueprint.RecoveryCadence);
			recoveryDue = false;
			pressureStreak = 0;
		}

		const bool highPressure = IsHighPressure(cell.encounterCard) || cell.isArena ||
			cell.hasKey || cell.hasExit;
		if (highPressure)
		{
			pressureStreak++;
			if (pressureStreak >= 2) recoveryDue = true;
		}
		else
		{
			pressureStreak = 0;
		}
	}

	// Pick one optional armory by hash rank instead of the first branch found
	// while scanning the grid. It is deliberately outside the mandatory route.
	int armoryX = -1;
	int armoryY = -1;
	uint32_t armoryScore = UINT32_MAX;
	for (int y = 1; y < H - 1; ++y)
	{
		for (int x = 1; x < W - 1; ++x)
		{
			const ProcGenCell& cell = Grid[y][x];
			if (!cell.present || cell.onMainPath || cell.hasKey || cell.hasExit ||
				cell.hasPlayerStart || cell.reservedSecret) continue;
			if (cell.branchDepth <= 0) continue;
			const uint32_t score = MixProcGenHash(blueprint.RecipeHash ^
				(uint32_t)x * 0x85ebca6bu ^ (uint32_t)y * 0xc2b2ae35u);
			if (score < armoryScore)
			{
				armoryScore = score;
				armoryX = x;
				armoryY = y;
			}
		}
	}
	if (armoryX >= 0)
	{
		ProcGenCell& armory = Grid[armoryY][armoryX];
		armory.optionalArmory = true;
		armory.rewardPlan = PGRW_Armory;
		armory.recoveryBudget = std::max(armory.recoveryBudget, 1);
	}

	// Motif targets are similarly hash-ranked. Strict candidates capture each
	// motif's intended role; the relaxed pass keeps a requested motif visible on
	// compact maps where no ideal host survived topology planning.
	auto SelectMotifTarget = [&](EProcGenFeatureMotif motif, int priority)
	{
		int selectedX = -1;
		int selectedY = -1;
		for (int pass = 0; pass < 2 && selectedX < 0; ++pass)
		{
			uint32_t bestScore = UINT32_MAX;
			for (int y = 1; y < H - 1; ++y)
			{
				for (int x = 1; x < W - 1; ++x)
				{
					const ProcGenCell& cell = Grid[y][x];
					if (!cell.present || cell.featureMotif != PGFM_None || cell.hasPlayerStart ||
						cell.hasExit || cell.isLocked) continue;
					bool eligible = false;
					switch (motif)
					{
					case PGFM_Watercourse:
						eligible = cell.onMainPath && !cell.isArena && cell.connectionCount >= 2;
						break;
					case PGFM_VerticalPressure:
						eligible = cell.onMainPath && !cell.isHub && !cell.isArena;
						break;
					case PGFM_RemoteReveal:
						eligible = cell.branchDepth > 0 || cell.connectionCount >= 3;
						break;
					case PGFM_ShrineSecrets:
						eligible = cell.reservedSecret || cell.branchDepth >= 2;
						break;
					case PGFM_SightlineRecon:
						eligible = cell.isHub || cell.isArena || cell.connectionCount >= 3;
						break;
					default: break;
					}
					if (!eligible && pass == 0) continue;
					const uint32_t score = MixProcGenHash(blueprint.RecipeHash ^
						(uint32_t)(priority + 1) * 0x27d4eb2du ^ (uint32_t)x * 0x165667b1u ^
						(uint32_t)y * 0xd3a2646cu);
					if (score < bestScore)
					{
						bestScore = score;
						selectedX = x;
						selectedY = y;
					}
				}
			}
		}
		if (selectedX >= 0)
		{
			ProcGenCell& selected = Grid[selectedY][selectedX];
			selected.featureMotif = motif;
			selected.featureMotifPriority = priority;
			// A motif gives the room pass an architectural target as well as a
			// gameplay tag. Never replace a real keyed gate, key shrine, or finale.
			if (!selected.hasExit && !selected.hasKey && !selected.isLocked)
			{
				switch (motif)
				{
				case PGFM_Watercourse: selected.landmarkArchetype = PGLA_BridgeBasin; break;
				case PGFM_VerticalPressure: selected.landmarkArchetype = PGLA_Nave; break;
				case PGFM_RemoteReveal: selected.landmarkArchetype = PGLA_Gatehouse; break;
				case PGFM_ShrineSecrets: selected.landmarkArchetype = PGLA_ShrineTerrace; break;
				case PGFM_SightlineRecon: selected.landmarkArchetype = PGLA_Court; break;
				default: break;
				}
			}
		}
	};
		for (int motif = 0; motif < blueprint.MotifCount; ++motif)
			SelectMotifTarget(blueprint.Motifs[motif], motif + 1);

		// Give every surviving graph edge a geometry-independent connection
		// contract before room merging. Width is a gameplay property: only a deep,
		// entirely optional branch may use Narrow; keys, stairs, and all required
		// travel retain at least a Standard 128x64 clear opening.
		auto IsCriticalConnectorCell = [](const ProcGenCell& cell) -> bool
		{
			return cell.hasPlayerStart || cell.hasKey || cell.hasExit || cell.hasBoss ||
				cell.isLocked || cell.verticalAnchor || cell.terrainRouteReservation;
		};
		for (int y = 0; y < H; ++y)
		{
			for (int x = 0; x < W; ++x)
			{
				ProcGenCell& first = Grid[y][x];
				if (!first.present) continue;
				for (int direction : { DIR_E, DIR_S })
				{
					if (!first.conn[direction]) continue;
					const int nx = x + DX[direction];
					const int ny = y + DY[direction];
					if (!InBounds(nx, ny) || !Grid[ny][nx].present) continue;
					ProcGenCell& second = Grid[ny][nx];
					const bool stair = first.connectionStairChain[direction] >= 0 ||
						second.connectionStairChain[OPP[direction]] >= 0;
					const bool keyed = first.lockStage != second.lockStage ||
						(first.isLocked && (first.lockDir < 0 || first.lockDir == direction)) ||
						(second.isLocked && (second.lockDir < 0 ||
							second.lockDir == OPP[direction]));
					const bool required = keyed || stair ||
						(first.onMainPath && second.onMainPath) ||
						IsCriticalConnectorCell(first) || IsCriticalConnectorCell(second);
					const bool deepOptional = !required && !first.onMainPath &&
						!second.onMainPath && first.branchDepth >= 2 &&
						second.branchDepth >= 2;
					const bool landmark = first.landmarkArchetype != PGLA_None ||
						second.landmarkArchetype != PGLA_None || first.isArena ||
						second.isArena || first.isHub || second.isHub;
					const uint32_t edgeHash = MixProcGenHash(blueprint.RecipeHash ^
						(uint32_t)(y * W + x + 1) * 0x85ebca6bu ^
						(uint32_t)(ny * W + nx + 1) * 0xc2b2ae35u);
					EProcGenConnectionProfile profile = PGCP_Standard;
					if (deepOptional && (edgeHash % 100u) < 62u)
						profile = PGCP_Narrow;
					else if (!keyed && !stair && landmark && (edgeHash % 100u) < 44u)
						profile = PGCP_Grand;
					else if (!keyed && !stair &&
						((first.onMainPath && second.onMainPath) || landmark ||
						 first.connectionCount >= 3 || second.connectionCount >= 3))
						profile = PGCP_Gallery;

					const int alignmentGroup = (int)(edgeHash & 0x7fffffffu);
					first.connectionProfile[direction] = profile;
					second.connectionProfile[OPP[direction]] = profile;
					first.connectionClearWidth[direction] = ConnectionClearWidth(profile);
					second.connectionClearWidth[OPP[direction]] = ConnectionClearWidth(profile);
					first.connectionDepth[direction] = ConnectionDepth(profile);
					second.connectionDepth[OPP[direction]] = ConnectionDepth(profile);
					first.connectionAlignmentGroup[direction] = alignmentGroup;
					second.connectionAlignmentGroup[OPP[direction]] = alignmentGroup;
				}
			}
		}

		MergeRooms(W, H);
		// Room composition may turn two adjacent support cells into a continuous
		// same-room opening after the original graph-edge contracts were planned.
		// Those new joins still need a stable alignment key and an explicit
		// manifest contract; otherwise they serialize as an unplanned `-1` group
		// even though the emitter correctly builds their room-owned portal.
		for (int y = 0; y < H; ++y)
		{
			for (int x = 0; x < W; ++x)
			{
				ProcGenCell& first = Grid[y][x];
				if (!first.present) continue;
				for (int direction : { DIR_E, DIR_S })
				{
					if (!first.conn[direction]) continue;
					const int nx = x + DX[direction];
					const int ny = y + DY[direction];
					if (!InBounds(nx, ny) || !Grid[ny][nx].present) continue;
					ProcGenCell& second = Grid[ny][nx];
					const int opposite = OPP[direction];
					if (first.connectionAlignmentGroup[direction] >= 0 &&
						second.connectionAlignmentGroup[opposite] < 0)
					{
						second.connectionProfile[opposite] = first.connectionProfile[direction];
						second.connectionClearWidth[opposite] = first.connectionClearWidth[direction];
						second.connectionDepth[opposite] = first.connectionDepth[direction];
						second.connectionAlignmentGroup[opposite] =
							first.connectionAlignmentGroup[direction];
						continue;
					}
					if (second.connectionAlignmentGroup[opposite] >= 0 &&
						first.connectionAlignmentGroup[direction] < 0)
					{
						first.connectionProfile[direction] = second.connectionProfile[opposite];
						first.connectionClearWidth[direction] = second.connectionClearWidth[opposite];
						first.connectionDepth[direction] = second.connectionDepth[opposite];
						first.connectionAlignmentGroup[direction] =
							second.connectionAlignmentGroup[opposite];
						continue;
					}
					if (first.connectionAlignmentGroup[direction] >= 0) continue;

					const uint32_t edgeHash = MixProcGenHash(blueprint.RecipeHash ^
						(uint32_t)(y * W + x + 1) * 0x85ebca6bu ^
						(uint32_t)(ny * W + nx + 1) * 0xc2b2ae35u ^ 0x7f4a7c15u);
					const EProcGenConnectionProfile profile = first.roomId == second.roomId ?
						PGCP_Gallery : PGCP_Standard;
					const int alignmentGroup = (int)(edgeHash & 0x7fffffffu);
					first.connectionProfile[direction] = profile;
					second.connectionProfile[opposite] = profile;
					first.connectionClearWidth[direction] = ConnectionClearWidth(profile);
					second.connectionClearWidth[opposite] = ConnectionClearWidth(profile);
					first.connectionDepth[direction] = ConnectionDepth(profile);
					second.connectionDepth[opposite] = ConnectionDepth(profile);
					first.connectionAlignmentGroup[direction] = alignmentGroup;
					second.connectionAlignmentGroup[opposite] = alignmentGroup;
				}
			}
		}
	ApplyCoherence(W, H);
	PlaceWeapons(W, H);
	const bool built = BuildUDMF(W, H);
	if (built)
	{
		GeneratedRecipeHash = blueprint.RecipeHash;
		HasGeneratedBlueprint = true;
	}
	return built;
}
