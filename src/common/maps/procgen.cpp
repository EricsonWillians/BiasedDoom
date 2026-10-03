/*
** procgen.cpp
**
** Procedural map generation for BiasedDoom
** Generates UDMF TEXTMAP data in memory for runtime map loading.
**
** This file now contains only console commands, CVars, and the MapData factory.
** The generation logic lives in src/common/maps/procgen/.
**
**---------------------------------------------------------------------------
*/

#include "procgen.h"
#include "p_setup.h"
#include "filesystem.h"
#include "g_levellocals.h"
#include "c_cvars.h"
#include "c_dispatch.h"
#include "d_event.h"
#include "d_main.h"
#include "doomstat.h"
#include "g_level.h"
#include "g_mapinfo.h"
#include "gamestate.h"
#include "i_system.h"
#include "menu.h"
#include "files.h"
#include <utility>

// ---------------------------------------------------------------------------
// MapData factory
// ---------------------------------------------------------------------------

EXTERN_CVAR(Int, procgen_seed)
EXTERN_CVAR(String, procgen_theme)
EXTERN_CVAR(Int, procgen_difficulty)
EXTERN_CVAR(Int, procgen_size)
EXTERN_CVAR(Int, procgen_layout)
EXTERN_CVAR(Int, procgen_verticality)
EXTERN_CVAR(Int, procgen_detail)
EXTERN_CVAR(Int, procgen_outdoors)

namespace
{
	constexpr size_t MaxArchivedProceduralMapSize = 64 * 1024 * 1024;
	FProceduralMapArchiveData CurrentProceduralMap;
	FProceduralMapArchiveData PendingProceduralMap;
	FProceduralMapArchiveData CompletedProceduralMap;
	bool HasCurrentProceduralMap = false;
	bool HasPendingProceduralMap = false;
	bool HasCompletedProceduralMap = false;

	void CopyProceduralRecipe(FProceduralMapArchiveData& destination,
		const FProceduralMapArchiveData& source)
	{
		destination.Seed = source.Seed;
		destination.Theme = source.Theme;
		destination.Difficulty = source.Difficulty;
		destination.Size = source.Size;
		destination.Layout = source.Layout;
		destination.Verticality = source.Verticality;
		destination.Detail = source.Detail;
		destination.Outdoors = source.Outdoors;
		// A completion remembers the recipe, not a second 64 MiB TEXTMAP copy.
		destination.UDMF = "";
	}

	bool IsProceduralTheme(const FString& theme)
	{
		return theme.Compare("techbase") == 0 || theme.Compare("hell") == 0 ||
			theme.Compare("industrial") == 0 || theme.Compare("gothic") == 0 ||
			theme.Compare("corrupted") == 0;
	}

	bool IsIWADMap(const FString& mapname)
	{
		for (int wad = fileSystem.GetIwadNum(); wad <= fileSystem.GetMaxIwadNum(); ++wad)
		{
			if (fileSystem.CheckNumForName(mapname.GetChars(), FileSys::ns_global, wad) >= 0)
				return true;
		}
		return false;
	}

	uint32_t ProceduralMusicHash(int seed)
	{
		// An independent avalanche keeps soundtrack selection deterministic without
		// consuming layout RNG state or coupling music changes to geometry changes.
		uint32_t value = uint32_t(seed) + 0x9e3779b9u;
		value = (value ^ (value >> 16)) * 0x21f0aaadu;
		value = (value ^ (value >> 15)) * 0x735a2d97u;
		return value ^ (value >> 15);
	}

	int MakeProceduralMenuSeed()
	{
		int seed = (int)(I_MakeRNGSeed() & 0x7fffffffU);
		return seed == 0 ? 1 : seed;
	}

	int MakeDistinctProceduralMenuSeed(int previousSeed)
	{
		// A collision is already very unlikely, but a menu action promising a
		// new run must never quietly rebuild the just-completed recipe.
		for (int attempt = 0; attempt < 8; ++attempt)
		{
			const int seed = MakeProceduralMenuSeed();
			if (seed != previousSeed)
				return seed;
		}

		uint32_t fallback = uint32_t(previousSeed) ^ 0x9e3779b9u;
		fallback ^= fallback >> 16;
		fallback *= 0x85ebca6bu;
		fallback ^= fallback >> 13;
		int seed = (int)(fallback & 0x7fffffffu);
		if (seed == 0) seed = 1;
		if (seed == previousSeed) seed = seed == 0x7fffffff ? 1 : seed + 1;
		return seed;
	}

	void ApplyProceduralRecipeToCVars(const FProceduralMapArchiveData& recipe, int seed)
	{
		procgen_seed = seed;
		procgen_theme = recipe.Theme.GetChars();
		procgen_difficulty = recipe.Difficulty;
		procgen_size = recipe.Size;
		procgen_layout = recipe.Layout;
		procgen_verticality = recipe.Verticality;
		procgen_detail = recipe.Detail;
		procgen_outdoors = recipe.Outdoors;
	}

	bool StartProceduralMapFromCurrentCVars()
	{
		if (netgame)
		{
			Printf(TEXTCOLOR_RED "Procedural games can only be started in single-player.\n");
			return false;
		}
		if (D_SetStartupMap("PROCMAP"))
			return true;

		G_DeferedInitNew("PROCMAP");
		if (gamestate == GS_FULLCONSOLE)
		{
			gamestate = GS_HIDECONSOLE;
			gameaction = ga_newgame;
		}
		M_ClearMenus();
		return true;
	}

	void ConfigureProceduralGenerator(FProceduralMapGenerator& gen, FCommandLine& argv)
	{
		gen.SetSeed(argv.argc() > 1 ? atoi(argv[1]) : 0);
		gen.SetTheme(argv.argc() > 2 ? argv[2] : "techbase");
		gen.SetDifficulty(argv.argc() > 3 ? atoi(argv[3]) : 3);
		gen.SetSize(argv.argc() > 4 ? atoi(argv[4]) : 3);
		gen.SetLayout(argv.argc() > 5 ? atoi(argv[5]) : 1);
		gen.SetVerticality(argv.argc() > 6 ? atoi(argv[6]) : 1);
		gen.SetDetail(argv.argc() > 7 ? atoi(argv[7]) : 1);
		gen.SetOutdoors(argv.argc() > 8 ? atoi(argv[8]) : 1);
	}

	bool WriteProceduralDump(const FString& path, const FString& contents, const char* description)
	{
		FileWriter* file = FileWriter::Open(path.GetChars());
		if (file == nullptr)
		{
			Printf(TEXTCOLOR_RED "Could not open %s for writing.\n", path.GetChars());
			return false;
		}

		const size_t expected = contents.Len();
		const bool written = file->Write(contents.GetChars(), expected) == expected;
		delete file;
		if (!written)
		{
			Printf(TEXTCOLOR_RED "Could not fully write %s.\n", path.GetChars());
			return false;
		}

		Printf("Dumped %s to %s (%lu bytes)\n", description, path.GetChars(),
			(unsigned long)expected);
		return true;
	}
}

bool P_IsProceduralMapName(const char* mapname)
{
	if (!mapname) return false;
	return !stricmp(mapname, "PROCMAP") || !strnicmp(mapname, "PROC", 4);
}

FString P_GetProceduralMusic()
{
	if (!HasCurrentProceduralMap)
		return FString();

	TArray<const level_info_t*> candidates;
	for (auto& level : wadlevelinfos)
	{
		if (level.MapName.IsNotEmpty() && level.Music.IsNotEmpty() && IsIWADMap(level.MapName))
			candidates.Push(&level);
	}

	if (candidates.Size() == 0)
		return FString();

	const level_info_t* selected = candidates[
		ProceduralMusicHash(CurrentProceduralMap.Seed) % candidates.Size()];
	DPrintf(DMSG_NOTIFY, "Procedural soundtrack selected from %s: %s\n",
		selected->MapName.GetChars(), selected->Music.GetChars());
	return selected->Music;
}

MapData* P_OpenProceduralMapData(const char* mapname)
{
	if (!P_IsProceduralMapName(mapname))
		return nullptr;

	FProceduralMapGenerator& gen = FProceduralMapGenerator::GetInstance();
	bool restoringArchivedMap = false;
	if (HasPendingProceduralMap)
	{
		restoringArchivedMap = true;
		CurrentProceduralMap = std::move(PendingProceduralMap);
		PendingProceduralMap = FProceduralMapArchiveData();
		HasPendingProceduralMap = false;
		HasCurrentProceduralMap = true;
		procgen_seed = CurrentProceduralMap.Seed;
		procgen_theme = CurrentProceduralMap.Theme.GetChars();
		procgen_difficulty = CurrentProceduralMap.Difficulty;
		procgen_size = CurrentProceduralMap.Size;
		procgen_layout = CurrentProceduralMap.Layout;
		procgen_verticality = CurrentProceduralMap.Verticality;
		procgen_detail = CurrentProceduralMap.Detail;
		procgen_outdoors = CurrentProceduralMap.Outdoors;
	}

	if (!restoringArchivedMap || CurrentProceduralMap.UDMF.IsEmpty())
	{
		// Re-seed and reconfigure from CVars to ensure deterministic generation
		// regardless of any previous Generate() calls that may have advanced the RNG.
		gen.SetSeed(procgen_seed);
		gen.SetTheme(procgen_theme);
		gen.SetDifficulty(procgen_difficulty);
		gen.SetSize(procgen_size);
		gen.SetLayout(procgen_layout);
		gen.SetVerticality(procgen_verticality);
		gen.SetDetail(procgen_detail);
		gen.SetOutdoors(procgen_outdoors);

		if (!gen.Generate())
		{
			HasCurrentProceduralMap = false;
			Printf(TEXTCOLOR_RED "Procedural map generation failed: %s\n", gen.GetLastError());
			return nullptr;
		}

		CurrentProceduralMap.Seed = gen.GetSeed();
		CurrentProceduralMap.Theme = gen.GetTheme();
		CurrentProceduralMap.Difficulty = gen.GetDifficulty();
		CurrentProceduralMap.Size = gen.GetSize();
		CurrentProceduralMap.Layout = gen.GetLayout();
		CurrentProceduralMap.Verticality = gen.GetVerticality();
		CurrentProceduralMap.Detail = gen.GetDetail();
		CurrentProceduralMap.Outdoors = gen.GetOutdoors();
		CurrentProceduralMap.UDMF = gen.GetUDMFText();
		HasCurrentProceduralMap = true;
	}

	const FString& udmf = CurrentProceduralMap.UDMF;
	if (udmf.Len() == 0)
	{
		Printf(TEXTCOLOR_RED "Procedural map generation produced empty UDMF.\n");
		return nullptr;
	}

	// A fresh run is not eligible for the replay action until it reaches its
	// own real Exit_Normal/Exit_Secret. This also prevents a restored save from
	// accidentally offering a stale recipe from a previous level.
	HasCompletedProceduralMap = false;
	CompletedProceduralMap = FProceduralMapArchiveData();

	MapData* map = new MapData;
	map->isText = true;

	FileSys::FileData data(udmf.GetChars(), udmf.Len(), true);
	map->MapLumps[ML_TEXTMAP].Reader.OpenMemoryArray(data);
	strncpy(map->MapLumps[ML_TEXTMAP].Name, "TEXTMAP", 8);

	return map;
}

const FProceduralMapArchiveData* P_GetCurrentProceduralMapArchive()
{
	return HasCurrentProceduralMap ? &CurrentProceduralMap : nullptr;
}

void P_MarkCurrentProceduralMapCompleted()
{
	if (!HasCurrentProceduralMap || CurrentProceduralMap.UDMF.IsEmpty())
		return;

	CopyProceduralRecipe(CompletedProceduralMap, CurrentProceduralMap);
	HasCompletedProceduralMap = true;
}

bool P_PrepareNextProceduralMap()
{
	if (!HasCompletedProceduralMap)
		return false;

	ApplyProceduralRecipeToCVars(CompletedProceduralMap,
		MakeDistinctProceduralMenuSeed(CompletedProceduralMap.Seed));
	return true;
}

bool P_StageProceduralMapArchive(int seed, const char* theme, int difficulty,
	int size, int layout, int verticality, int detail, int outdoors, FString udmf)
{
	FString normalizedTheme = theme ? theme : "";
	normalizedTheme.ToLower();
	if (!IsProceduralTheme(normalizedTheme) || difficulty < 1 || difficulty > 5 ||
		size < FProceduralMapGenerator::MinMapSize ||
		size > FProceduralMapGenerator::MaxMapSize ||
		layout < 0 || layout > 2 || verticality < 0 || verticality > 2 ||
		detail < 0 || detail > 2 || outdoors < 0 || outdoors > 2 ||
		udmf.Len() > MaxArchivedProceduralMapSize)
		return false;
	if (udmf.IsNotEmpty() &&
		(strstr(udmf.GetChars(), "namespace = \"zdoom\"") == nullptr ||
		 strstr(udmf.GetChars(), "sector\n{") == nullptr ||
		 strstr(udmf.GetChars(), "linedef\n{") == nullptr))
		return false;

	PendingProceduralMap.Seed = seed;
	PendingProceduralMap.Theme = normalizedTheme;
	PendingProceduralMap.Difficulty = difficulty;
	PendingProceduralMap.Size = size;
	PendingProceduralMap.Layout = layout;
	PendingProceduralMap.Verticality = verticality;
	PendingProceduralMap.Detail = detail;
	PendingProceduralMap.Outdoors = outdoors;
	PendingProceduralMap.UDMF = std::move(udmf);
	HasPendingProceduralMap = true;
	return true;
}

//===========================================================================
//
// Console commands
//
//===========================================================================

CVAR(Int, procgen_seed, 0, CVAR_ARCHIVE);
CVAR(String, procgen_theme, "techbase", CVAR_ARCHIVE);
CVAR(Int, procgen_difficulty, 3, CVAR_ARCHIVE);
CVAR(Int, procgen_size, 3, CVAR_ARCHIVE);
CVAR(Int, procgen_layout, 1, CVAR_ARCHIVE);
CVAR(Int, procgen_verticality, 1, CVAR_ARCHIVE);
CVAR(Int, procgen_detail, 1, CVAR_ARCHIVE);
CVAR(Int, procgen_outdoors, 1, CVAR_ARCHIVE);

CCMD(procmap_randomize_seed)
{
	procgen_seed = MakeProceduralMenuSeed();
	Printf("Procedural map seed set to %d.\n", (int)procgen_seed);
}

CCMD(procmap_restore_defaults)
{
	procgen_seed = 0;
	procgen_theme = "techbase";
	procgen_difficulty = 3;
	procgen_size = 3;
	procgen_layout = 1;
	procgen_verticality = 1;
	procgen_detail = 1;
	procgen_outdoors = 1;
	Printf("Procedural map settings restored to defaults.\n");
}

CCMD(procmap_next)
{
	if (netgame)
	{
		Printf(TEXTCOLOR_RED "Procedural games can only be started in single-player.\n");
		return;
	}

	if (!P_PrepareNextProceduralMap())
	{
		Printf(TEXTCOLOR_RED "Finish a procedural run before starting its next seed.\n");
		return;
	}

	FProceduralMapGenerator& gen = FProceduralMapGenerator::GetInstance();
	gen.SetSeed(procgen_seed);
	gen.SetTheme(procgen_theme);
	gen.SetDifficulty(procgen_difficulty);
	gen.SetSize(procgen_size);
	gen.SetLayout(procgen_layout);
	gen.SetVerticality(procgen_verticality);
	gen.SetDetail(procgen_detail);
	gen.SetOutdoors(procgen_outdoors);

	Printf("Starting next procedural run with the completed setup "
		"(seed=%d, theme=%s, diff=%d, size=%d, layout=%d, verticality=%d, detail=%d, outdoors=%d)...\n",
		(int)procgen_seed, (const char*)procgen_theme, (int)procgen_difficulty,
		(int)procgen_size, (int)procgen_layout, (int)procgen_verticality,
		(int)procgen_detail, (int)procgen_outdoors);
	Printf("Run profile: %s\n", gen.GetRunProfile().GetChars());
	Printf("Run briefing: %s\n", gen.GetRunBriefing().GetChars());
	StartProceduralMapFromCurrentCVars();
}

CCMD(dumpprocudmf)
{
	FProceduralMapGenerator& gen = FProceduralMapGenerator::GetInstance();
	ConfigureProceduralGenerator(gen, argv);
	if (gen.Generate())
	{
		const FString path = argv.argc() > 9 ? argv[9] : "/tmp/procmap_test.udmf";
		WriteProceduralDump(path, gen.GetUDMFText(), "UDMF");
	}
	else
	{
		Printf(TEXTCOLOR_RED "Generation failed: %s\n", gen.GetLastError());
	}
}

CCMD(dumpprocmanifest)
{
	FProceduralMapGenerator& gen = FProceduralMapGenerator::GetInstance();
	ConfigureProceduralGenerator(gen, argv);
	if (gen.Generate())
	{
		const FString path = argv.argc() > 9 ? argv[9] : "/tmp/procmap_manifest.json";
		WriteProceduralDump(path, gen.GetRunManifest(), "procedural run manifest");
	}
	else
	{
		Printf(TEXTCOLOR_RED "Generation failed: %s\n", gen.GetLastError());
	}
}

CCMD(procmap)
{
	if (argv.argc() > 1)
	{
		procgen_seed = !stricmp(argv[1], "random") ? MakeProceduralMenuSeed() : atoi(argv[1]);
	}

	FProceduralMapGenerator& gen = FProceduralMapGenerator::GetInstance();
	gen.SetSeed(procgen_seed);
	gen.SetTheme(procgen_theme);
	gen.SetDifficulty(procgen_difficulty);
	gen.SetSize(procgen_size);
	gen.SetLayout(procgen_layout);
	gen.SetVerticality(procgen_verticality);
	gen.SetDetail(procgen_detail);
	gen.SetOutdoors(procgen_outdoors);

	Printf("Generating procedural map (seed=%d, theme=%s, diff=%d, size=%d, "
		"layout=%d, verticality=%d, detail=%d, outdoors=%d)...\n",
		(int)procgen_seed, (const char*)procgen_theme, (int)procgen_difficulty,
		(int)procgen_size, (int)procgen_layout, (int)procgen_verticality,
		(int)procgen_detail, (int)procgen_outdoors);
	Printf("Run profile: %s\n", gen.GetRunProfile().GetChars());
	Printf("Run briefing: %s\n", gen.GetRunBriefing().GetChars());

	// Do NOT call Generate() here. P_OpenProceduralMapData will generate
	// the map when the engine loads PROCMAP, ensuring a single generation
	// and proper MapData construction.
	StartProceduralMapFromCurrentCVars();
}
