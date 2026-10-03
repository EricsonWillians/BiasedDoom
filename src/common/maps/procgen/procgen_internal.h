#pragma once

#include "../procgen.h"
#include "cmdlib.h"
#include "gamedata/gi.h"
#include <math.h>

namespace ProcGen {

// A 384-unit module leaves enough lateral distance for projectile dodging and
// crossfire even in the smallest authored chamber. The size-160 canvas retains
// a guarded safety margin inside UDMF's coordinate range.
static const int CELL_SIZE = 384;

static const int DIR_N = 0;
static const int DIR_S = 1;
static const int DIR_W = 2;
static const int DIR_E = 3;

enum ThemeStyle
{
	ThemeTechbase,
	ThemeHell,
	ThemeIndustrial,
	ThemeGothic,
	ThemeCorrupted,
};

inline ThemeStyle GetThemeStyle(const FString& theme)
{
	if (theme.Compare("hell") == 0) return ThemeHell;
	if (theme.Compare("industrial") == 0) return ThemeIndustrial;
	if (theme.Compare("gothic") == 0) return ThemeGothic;
	if (theme.Compare("corrupted") == 0) return ThemeCorrupted;
	return ThemeTechbase;
}

inline bool UsesInfernalArchitecture(ThemeStyle theme)
{
	return theme == ThemeHell || theme == ThemeGothic || theme == ThemeCorrupted;
}

// Procedural maps deliberately target the stock Doom actor and texture
// families. Keep the small Doom II extension set in one place: a MAPxx IWAD
// gets its Super Shotgun, MegaSphere, extra bestiary, lamps, and SPCDOOR art;
// an episode-format Doom IWAD always receives a stock Doom equivalent.
//
// This is not a generic game-family detector. Heretic and Hexen use different
// actors, inventory, keys, and surface vocabularies, so their procedural
// support is intentionally deferred for this release (see Generate()).
inline bool ProcGenUsesDoom2Roster()
{
	return gameinfo.gametype == GAME_Doom && (gameinfo.flags & GI_MAPxx) != 0;
}

inline const char* ProcGenIwadRosterName()
{
	return ProcGenUsesDoom2Roster() ? "doom2" : "doom1";
}

inline int ProcGenCompatibleThing(int type)
{
	if (ProcGenUsesDoom2Roster()) return type;

	// These are the stock Doom II-only thing numbers. Most call sites already
	// select the correct table, but normalizing at the emission boundary keeps a
	// new card, cache, or landmark from quietly serializing an unavailable actor
	// into Ultimate Doom.
	switch (type)
	{
	case 82: return 2001;  // Super Shotgun -> Shotgun
	case 83: return 2013;  // MegaSphere -> Soul Sphere
	case 64: return 3001;  // Arch-vile -> Imp
	case 65: return 9;     // Chaingunner -> Shotgun Guy
	case 66: return 3002;  // Revenant -> Demon
	case 67: return 3003;  // Mancubus -> Baron of Hell
	case 68: return 3003;  // Arachnotron -> Baron of Hell
	case 69: return 3003;  // Hell Knight -> Baron of Hell
	case 71: return 3005;  // Pain Elemental -> Cacodemon
	case 72: return 3001;  // Commander Keen -> Imp
	case 84: return 3001;  // Wolfenstein SS -> Imp
	case 85: return 48;    // Tech lamp -> tech column
	case 86: return 2028;  // short tech lamp -> floor lamp
	case 88: return 3003;  // Boss Brain -> Baron of Hell
	case 89: return 3001;  // Boss Shooter -> Imp
	default: return type;
	}
}

inline constexpr int DX[4] = { 0, 0, -1, 1 };
inline constexpr int DY[4] = { -1, 1, 0, 0 };
inline constexpr int OPP[4] = { 1, 0, 3, 2 };

// Boss ednums by difficulty. The Spider Mastermind's 128-unit radius still
// needs more authored clearance than a single coarse cell, so heavyweight
// finales use the substantially smaller Cyberdemon after arena-capacity validation.
inline constexpr int BossesEasy[] = { 3003 };
inline constexpr int BossesMed[]  = { 3003, 69 };
inline constexpr int BossesHard[] = { 16 };

inline constexpr int HealthDrops[] = { 2011, 2012, 2014 };
inline constexpr int ArmorDrops[] = { 2015, 2018, 2019 };

} // namespace ProcGen
