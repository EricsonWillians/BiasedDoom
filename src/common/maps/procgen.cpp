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
#include "d_net.h"
#include "d_protocol.h"
#include "d_main.h"
#include "doomstat.h"
#include "g_level.h"
#include "g_mapinfo.h"
#include "gi.h"
#include "gamestate.h"
#include "i_net.h"
#include "i_system.h"
#include "menu.h"
#include "files.h"
#include "m_crc32.h"
#include <cstring>
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
	// The UDP transport has a smaller compressed-wire budget than MAX_MSGLEN.
	// A 3 KiB raw-archive record leaves reliable room for tic headers, user
	// commands, and outer packet compression without layering a second archive
	// codec into the deterministic map hand-off.
	constexpr size_t ProceduralTransferChunkSize = 3 * 1024;
	constexpr size_t MaxProceduralTransferReasonLength = 192;
	constexpr int ProceduralTransferTimeoutTics = TICRATE * 300;
	constexpr int ProceduralTransferGraceTics = TICRATE * 120;
	constexpr int MaxProceduralCoopParticipants = 8;
	constexpr uint8_t ProceduralRosterDoom1 = 1;
	constexpr uint8_t ProceduralRosterDoom2 = 2;
	FProceduralMapArchiveData CurrentProceduralMap;
	FProceduralMapArchiveData PendingProceduralMap;
	FProceduralMapArchiveData CompletedProceduralMap;
	bool HasCurrentProceduralMap = false;
	bool HasPendingProceduralMap = false;
	bool HasCompletedProceduralMap = false;

	struct FProceduralTransferHost
	{
		bool Active = false;
		bool BeginSent = false;
		bool FinishSent = false;
		bool CancelPending = false;
		bool ChangeMapPending = false;
		int TransferId = 0;
		uint8_t Roster = 0;
		FProceduralMapArchiveData Archive;
		size_t NextOffset = 0;
		int NextChunk = 0;
		int ChunkCount = 0;
		uint32_t Checksum = 0;
		uint64_t RequiredAcks = 0;
		uint64_t ReceivedAcks = 0;
		int Deadline = 0;
		int LastQueuedEventTic = -1;
		FString CancelReason;
	};

	struct FProceduralTransferReceiver
	{
		bool Active = false;
		bool Staged = false;
		bool CancelPending = false;
		bool AckPending = false;
		// Once the client has queued its verification ACK, the host may already
		// have queued the matching DEM_CHANGEMAP. Keep the staged archive alive
		// until that terminal host event arrives; clearing it in this window would
		// make this peer reject the map while every other peer transitions.
		bool TransitionCommitted = false;
		int TransferId = 0;
		int HostPlayer = -1;
		uint8_t Roster = 0;
		int Seed = 0;
		FString Theme;
		int Difficulty = 0;
		int Size = 0;
		int Layout = 0;
		int Verticality = 0;
		int Detail = 0;
		int Outdoors = 0;
		size_t ArchiveSize = 0;
		uint32_t Checksum = 0;
		int ChunkCount = 0;
		int NextChunk = 0;
		size_t NextOffset = 0;
		int Deadline = 0;
		int LastQueuedEventTic = -1;
		FString CancelReason;
		TArray<uint8_t> ArchiveBytes;
	};

	FProceduralTransferHost NetworkProceduralTransfer;
	FProceduralTransferReceiver NetworkProceduralReceiver;
	int NextProceduralTransferId = 1;

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

	uint64_t PlayerBit(int player)
	{
		return player >= 0 && player < 64 ? (uint64_t(1) << player) : 0;
	}

	uint64_t ConnectedPlayerMask()
	{
		uint64_t result = 0;
		for (auto player : NetworkClients)
			result |= PlayerBit(player);
		return result;
	}

	void ClearPendingProceduralMap()
	{
		PendingProceduralMap = FProceduralMapArchiveData();
		HasPendingProceduralMap = false;
	}

	void DiscardAbandonedProceduralArchive()
	{
		// A cancelled host transfer never reaches the normal level-loader
		// lifecycle, which is where a live map usually drops the generator's
		// cached TEXTMAP. The archive data held by Current/Pending/transfer state
		// is independently reference counted, so releasing the singleton cache
		// here is safe even while an older procedural level is still live.
		FProceduralMapGenerator::GetInstance().DiscardUDMFText();
	}

	void ConfigureProceduralGenerator(FProceduralMapGenerator& gen,
		const FProceduralMapArchiveData& recipe)
	{
		gen.SetSeed(recipe.Seed);
		gen.SetTheme(recipe.Theme.GetChars());
		gen.SetDifficulty(recipe.Difficulty);
		gen.SetSize(recipe.Size);
		gen.SetLayout(recipe.Layout);
		gen.SetVerticality(recipe.Verticality);
		gen.SetDetail(recipe.Detail);
		gen.SetOutdoors(recipe.Outdoors);
	}

	bool BuildProceduralArchiveFromCurrentCVars(FProceduralMapArchiveData& archive,
		FString& error)
	{
		FProceduralMapGenerator& gen = FProceduralMapGenerator::GetInstance();
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
			error = gen.GetLastError();
			return false;
		}

		archive.Seed = gen.GetSeed();
		archive.Theme = gen.GetTheme();
		archive.Difficulty = gen.GetDifficulty();
		archive.Size = gen.GetSize();
		archive.Layout = gen.GetLayout();
		archive.Verticality = gen.GetVerticality();
		archive.Detail = gen.GetDetail();
		archive.Outdoors = gen.GetOutdoors();
		archive.UDMF = gen.GetUDMFText();
		if (archive.UDMF.IsEmpty() || archive.UDMF.Len() > MaxArchivedProceduralMapSize)
		{
			error = "Generator produced an invalid archive size";
			return false;
		}
		return true;
	}

	uint8_t LocalProceduralRoster()
	{
		if (gameinfo.gametype != GAME_Doom)
			return 0;
		return (gameinfo.flags & GI_MAPxx) != 0 ? ProceduralRosterDoom2 :
			ProceduralRosterDoom1;
	}

	void AddArchiveChecksumByte(uint32_t& checksum, uint8_t value)
	{
		checksum = AddCRC32(checksum, &value, 1);
	}

	void AddArchiveChecksumInt(uint32_t& checksum, int32_t value)
	{
		const uint32_t bits = (uint32_t)value;
		uint8_t bytes[4] = {
			(uint8_t)(bits >> 24), (uint8_t)(bits >> 16),
			(uint8_t)(bits >> 8), (uint8_t)bits
		};
		checksum = AddCRC32(checksum, bytes, 4);
	}

	uint32_t CalculateProceduralArchiveChecksum(uint8_t roster, int seed,
		const FString& theme, int difficulty, int size, int layout, int verticality,
		int detail, int outdoors, const uint8_t* archive, size_t archiveSize)
	{
		uint32_t checksum = 0;
		AddArchiveChecksumByte(checksum, roster);
		AddArchiveChecksumInt(checksum, seed);
		AddArchiveChecksumByte(checksum, (uint8_t)difficulty);
		AddArchiveChecksumByte(checksum, (uint8_t)size);
		AddArchiveChecksumByte(checksum, (uint8_t)layout);
		AddArchiveChecksumByte(checksum, (uint8_t)verticality);
		AddArchiveChecksumByte(checksum, (uint8_t)detail);
		AddArchiveChecksumByte(checksum, (uint8_t)outdoors);
		checksum = AddCRC32(checksum, (const uint8_t*)theme.GetChars(),
			(unsigned)theme.Len() + 1);
		return AddCRC32(checksum, archive, (unsigned)archiveSize);
	}

	uint32_t CalculateProceduralArchiveChecksum(const FProceduralMapArchiveData& archive,
		uint8_t roster)
	{
		return CalculateProceduralArchiveChecksum(roster, archive.Seed, archive.Theme,
			archive.Difficulty, archive.Size, archive.Layout, archive.Verticality,
			archive.Detail, archive.Outdoors, (const uint8_t*)archive.UDMF.GetChars(),
			archive.UDMF.Len());
	}

	int ProceduralTransferDeadline(int chunkCount)
	{
		// One record is emitted per client event tic. Allow two missed/busy tics
		// for every chunk plus a fixed hand-off/ack grace period, while retaining
		// the familiar five-minute minimum for normal-sized maps.
		return gametic + max(ProceduralTransferTimeoutTics,
			chunkCount * 3 + ProceduralTransferGraceTics);
	}

	bool VerifyReceivedProceduralArchive(const FProceduralTransferReceiver& receiver,
		FString& output)
	{
		if (receiver.ArchiveSize == 0 ||
			receiver.ArchiveSize > MaxArchivedProceduralMapSize ||
			receiver.ArchiveBytes.Size() != receiver.ArchiveSize ||
			receiver.Roster != LocalProceduralRoster())
		{
			return false;
		}
		const uint32_t checksum = CalculateProceduralArchiveChecksum(receiver.Roster,
			receiver.Seed, receiver.Theme, receiver.Difficulty, receiver.Size,
			receiver.Layout, receiver.Verticality, receiver.Detail, receiver.Outdoors,
			receiver.ArchiveBytes.Data(), receiver.ArchiveBytes.Size());
		if (checksum != receiver.Checksum)
			return false;
		output = FString(receiver.ArchiveBytes);
		return true;
	}

	void ResetProceduralReceiver()
	{
		NetworkProceduralReceiver = FProceduralTransferReceiver();
	}

	void ResetProceduralTransfer()
	{
		NetworkProceduralTransfer = FProceduralTransferHost();
	}

	FString BoundedProceduralTransferReason(const char* reason)
	{
		const char* message = reason != nullptr && *reason != 0 ? reason : "cancelled";
		return FString(message, min(strlen(message), MaxProceduralTransferReasonLength));
	}

	bool QueueProceduralEvent(TArray<uint8_t>& bytes, int& lastQueuedEventTic)
	{
		const int eventTic = Net_GetCurrentEventTic();
		if (eventTic == lastQueuedEventTic || bytes.Size() == 0)
			return false;
		if (!Net_WriteEventAtomic(bytes.Data(), (int)bytes.Size()))
			return false;
		lastQueuedEventTic = eventTic;
		return true;
	}

	void BeginProceduralEvent(TArray<uint8_t>& bytes, size_t size, uint8_t command,
		TArrayView<uint8_t>& stream)
	{
		bytes.Resize((unsigned)size);
		stream = TArrayView<uint8_t>(bytes.Data(), bytes.Size());
		WriteInt8(command, stream);
	}

	bool QueueProceduralTransferAbort(int transferId, const char* reason,
		int& lastQueuedEventTic)
	{
		const FString message = BoundedProceduralTransferReason(reason);
		const size_t messageLength = message.Len() + 1;
		TArray<uint8_t> bytes;
		TArrayView<uint8_t> stream;
		BeginProceduralEvent(bytes, 1 + 4 + messageLength, DEM_PROCMAP_ABORT, stream);
		WriteInt32(transferId, stream);
		WriteString(message.GetChars(), stream);
		return QueueProceduralEvent(bytes, lastQueuedEventTic);
	}

	void RequestReceiverAbort(int transferId, int hostPlayer, const char* reason)
	{
		ResetProceduralReceiver();
		ClearPendingProceduralMap();
		NetworkProceduralReceiver.TransferId = transferId;
		NetworkProceduralReceiver.HostPlayer = hostPlayer;
		NetworkProceduralReceiver.CancelPending = transferId > 0;
		NetworkProceduralReceiver.CancelReason = BoundedProceduralTransferReason(reason);
	}

	void RejectProceduralTransfer(int transferId, int hostPlayer, const char* reason)
	{
		Printf(TEXTCOLOR_RED "Shared procedural map rejected: %s\n", reason);
		RequestReceiverAbort(transferId, hostPlayer, reason);
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
			// A live host owns generation and sends one exact archive to every
			// connected peer. Clients never regenerate from their local recipe.
			return P_StartNetworkProceduralMapTransfer();
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
	// In a live netgame only the host's checksummed archive may enter the map
	// loader. Falling back to local generation here would make a manual
	// changemap or an interrupted transfer silently desynchronize peers.
	if (netgame && !HasPendingProceduralMap)
	{
		Printf(TEXTCOLOR_RED "Shared procedural maps require a verified host archive.\n");
		return nullptr;
	}

	FProceduralMapGenerator& gen = FProceduralMapGenerator::GetInstance();
	bool restoringArchivedMap = false;
	if (HasPendingProceduralMap)
	{
		restoringArchivedMap = true;
		CurrentProceduralMap = std::move(PendingProceduralMap);
		PendingProceduralMap = FProceduralMapArchiveData();
		HasPendingProceduralMap = false;
		if (NetworkProceduralReceiver.Staged)
			ResetProceduralReceiver();
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

	// Even when an exact embedded TEXTMAP is restored, configure the singleton
	// from its recipe. The map itself is never regenerated here, but the
	// visible briefing/profile and ZScript accessors must describe the loaded
	// archive on clients as well as on the generating host.
	if (restoringArchivedMap)
		ConfigureProceduralGenerator(gen, CurrentProceduralMap);

	if (!restoringArchivedMap || CurrentProceduralMap.UDMF.IsEmpty())
	{
		// Re-seed and reconfigure from CVars to ensure deterministic generation
		// regardless of any previous Generate() calls that may have advanced the RNG.
		FProceduralMapArchiveData recipe;
		recipe.Seed = procgen_seed;
		recipe.Theme = procgen_theme;
		recipe.Difficulty = procgen_difficulty;
		recipe.Size = procgen_size;
		recipe.Layout = procgen_layout;
		recipe.Verticality = procgen_verticality;
		recipe.Detail = procgen_detail;
		recipe.Outdoors = procgen_outdoors;
		ConfigureProceduralGenerator(gen, recipe);

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

void P_ReleaseCurrentProceduralMapArchive()
{
	if (!HasCurrentProceduralMap)
		return;

	// CurrentProceduralMap normally shares the generator's FString backing
	// store. Clearing both references after the loader has consumed its owned
	// FileData copy actually releases a large generated TEXTMAP instead of
	// keeping it resident until another run happens to replace it.
	CurrentProceduralMap = FProceduralMapArchiveData();
	HasCurrentProceduralMap = false;
	FProceduralMapGenerator::GetInstance().DiscardUDMFText();
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

bool P_IsNetworkProceduralMapTransferActive()
{
	return NetworkProceduralTransfer.Active || NetworkProceduralReceiver.Active ||
		NetworkProceduralReceiver.Staged || NetworkProceduralReceiver.CancelPending ||
		NetworkProceduralReceiver.AckPending;
}

void P_ResetNetworkProceduralMapTransferState()
{
	ResetProceduralTransfer();
	ResetProceduralReceiver();
	ClearPendingProceduralMap();
	DiscardAbandonedProceduralArchive();
}

bool P_ConfirmNetworkProceduralMapTransition()
{
	if (!netgame || !HasPendingProceduralMap || NetworkProceduralTransfer.Active ||
		NetworkProceduralReceiver.CancelPending || NetworkProceduralReceiver.AckPending)
	{
		return false;
	}
	if (consoleplayer != Net_Arbitrator &&
		(!NetworkProceduralReceiver.Staged ||
			!NetworkProceduralReceiver.TransitionCommitted))
		return false;
	// The archive itself must survive through ChangeLevel until the map loader
	// consumes it. Only retire the transfer envelope so it cannot authorize a
	// later direct PROCMAP command.
	if (NetworkProceduralReceiver.Staged)
		ResetProceduralReceiver();
	return true;
}

bool P_StartNetworkProceduralMapTransfer()
{
	if (!netgame)
		return false;
	if (demoplayback || demorecording || gamestate != GS_LEVEL || !usergame)
	{
		Printf(TEXTCOLOR_RED "A shared procedural run must be started from a live, non-recording co-op game.\n");
		return false;
	}
	if (deathmatch)
	{
		Printf(TEXTCOLOR_RED "Procedural runs are cooperative only.\n");
		return false;
	}
	if (consoleplayer != Net_Arbitrator || !players[consoleplayer].settings_controller)
	{
		Printf(TEXTCOLOR_RED "Only the host can start a shared procedural run.\n");
		return false;
	}
	if (P_IsNetworkProceduralMapTransferActive())
	{
		Printf(TEXTCOLOR_YELLOW "A shared procedural map transfer is already in progress.\n");
		return false;
	}
	if (NetworkClients.Size() > MaxProceduralCoopParticipants)
	{
		Printf(TEXTCOLOR_RED "Shared procedural runs support at most %d human participants.\n",
			MaxProceduralCoopParticipants);
		return false;
	}
	const uint8_t roster = LocalProceduralRoster();
	if (roster == 0)
	{
		Printf(TEXTCOLOR_RED "Shared procedural runs require a Doom or Doom II IWAD.\n");
		return false;
	}

	FProceduralMapArchiveData archive;
	FString error;
	if (!BuildProceduralArchiveFromCurrentCVars(archive, error))
	{
		DiscardAbandonedProceduralArchive();
		Printf(TEXTCOLOR_RED "Procedural map generation failed: %s\n", error.GetChars());
		return false;
	}
	if (!P_StageProceduralMapArchive(archive.Seed, archive.Theme.GetChars(),
		archive.Difficulty, archive.Size, archive.Layout, archive.Verticality,
		archive.Detail, archive.Outdoors, archive.UDMF))
	{
		DiscardAbandonedProceduralArchive();
		Printf(TEXTCOLOR_RED "Generated procedural archive did not pass staging validation.\n");
		return false;
	}

	const int chunkCount = (int)((archive.UDMF.Len() + ProceduralTransferChunkSize - 1) /
		ProceduralTransferChunkSize);
	const uint64_t requiredAcks = ConnectedPlayerMask();
	if (chunkCount <= 0 || requiredAcks == 0)
	{
		ClearPendingProceduralMap();
		DiscardAbandonedProceduralArchive();
		Printf(TEXTCOLOR_RED "No eligible co-op participants for the shared procedural run.\n");
		return false;
	}

	int transferId = NextProceduralTransferId++;
	if (transferId <= 0)
	{
		NextProceduralTransferId = 2;
		transferId = 1;
	}
	NetworkProceduralTransfer = FProceduralTransferHost();
	NetworkProceduralTransfer.Active = true;
	NetworkProceduralTransfer.TransferId = transferId;
	NetworkProceduralTransfer.Roster = roster;
	NetworkProceduralTransfer.Archive = std::move(archive);
	NetworkProceduralTransfer.ChunkCount = chunkCount;
	NetworkProceduralTransfer.Checksum = CalculateProceduralArchiveChecksum(
		NetworkProceduralTransfer.Archive, roster);
	NetworkProceduralTransfer.RequiredAcks = requiredAcks;
	NetworkProceduralTransfer.ReceivedAcks = PlayerBit(consoleplayer);
	NetworkProceduralTransfer.Deadline = ProceduralTransferDeadline(chunkCount);

	Printf("Preparing shared procedural run for %u participant%s (%lu KiB archive).\n",
		(unsigned)NetworkClients.Size(), NetworkClients.Size() == 1 ? "" : "s",
		(unsigned long)((NetworkProceduralTransfer.Archive.UDMF.Len() + 1023) / 1024));
	return true;
}

void P_CancelNetworkProceduralMapTransfer(const char* reason)
{
	const FString message = BoundedProceduralTransferReason(reason);
	if (NetworkProceduralTransfer.Active)
	{
		if (!NetworkProceduralTransfer.CancelPending)
		{
			NetworkProceduralTransfer.CancelPending = true;
			NetworkProceduralTransfer.ChangeMapPending = false;
			NetworkProceduralTransfer.CancelReason = message;
			Printf(TEXTCOLOR_YELLOW "Shared procedural map transfer cancelling: %s\n", message.GetChars());
		}
		return;
	}

	if (NetworkProceduralReceiver.Active || NetworkProceduralReceiver.Staged ||
		NetworkProceduralReceiver.AckPending)
	{
		if (NetworkProceduralReceiver.TransitionCommitted)
		{
			// The ACK is a local commit: the host can legally have a PROCMAP
			// change event in flight already. Retain PendingProceduralMap so this
			// client is guaranteed to accept that event rather than desyncing.
			Printf(TEXTCOLOR_YELLOW "Shared procedural archive is verified and the map transition is already committed.\n");
			return;
		}
		const int transferId = NetworkProceduralReceiver.TransferId;
		const int host = NetworkProceduralReceiver.HostPlayer;
		RequestReceiverAbort(transferId, host, message.GetChars());
		Printf(TEXTCOLOR_YELLOW "Shared procedural map transfer cancelling: %s\n", message.GetChars());
	}
}

void P_TickNetworkProceduralMapTransfer()
{
	if (NetworkProceduralReceiver.CancelPending)
	{
		if (!netgame || demoplayback || consoleplayer == Net_Arbitrator ||
			NetworkProceduralReceiver.HostPlayer != Net_Arbitrator)
		{
			P_ResetNetworkProceduralMapTransferState();
		}
		else if (QueueProceduralTransferAbort(NetworkProceduralReceiver.TransferId,
			NetworkProceduralReceiver.CancelReason.GetChars(),
			NetworkProceduralReceiver.LastQueuedEventTic))
		{
			P_ResetNetworkProceduralMapTransferState();
		}
		return;
	}

	if (NetworkProceduralReceiver.Active || NetworkProceduralReceiver.Staged)
	{
		if (!netgame || demoplayback || gamestate != GS_LEVEL || deathmatch ||
			NetworkProceduralReceiver.HostPlayer != Net_Arbitrator ||
			!NetworkClients.InGame(NetworkProceduralReceiver.HostPlayer) ||
			(!NetworkProceduralReceiver.TransitionCommitted &&
				gametic >= NetworkProceduralReceiver.Deadline))
		{
			RequestReceiverAbort(NetworkProceduralReceiver.TransferId,
				NetworkProceduralReceiver.HostPlayer,
				NetworkProceduralReceiver.Staged ?
					"host did not complete the staged map transition" :
					"host transfer ended before completion");
			return;
		}
	}

	if (NetworkProceduralReceiver.AckPending)
	{
		TArray<uint8_t> bytes;
		TArrayView<uint8_t> stream;
		BeginProceduralEvent(bytes, 1 + 4 + 4, DEM_PROCMAP_ACK, stream);
		WriteInt32(NetworkProceduralReceiver.TransferId, stream);
		WriteInt32((int32_t)NetworkProceduralReceiver.Checksum, stream);
		if (QueueProceduralEvent(bytes, NetworkProceduralReceiver.LastQueuedEventTic))
		{
			NetworkProceduralReceiver.AckPending = false;
			NetworkProceduralReceiver.TransitionCommitted = true;
		}
	}

	if (!NetworkProceduralTransfer.Active)
		return;

	if (!netgame || demoplayback || demorecording || gamestate != GS_LEVEL || !usergame ||
		deathmatch || consoleplayer != Net_Arbitrator ||
		!players[consoleplayer].settings_controller)
	{
		P_CancelNetworkProceduralMapTransfer("host authority or co-op state changed");
	}
	else if (ConnectedPlayerMask() != NetworkProceduralTransfer.RequiredAcks)
	{
		P_CancelNetworkProceduralMapTransfer("the co-op roster changed");
	}
	else if (gametic >= NetworkProceduralTransfer.Deadline)
	{
		P_CancelNetworkProceduralMapTransfer("timed out waiting for peers");
	}

	if (NetworkProceduralTransfer.CancelPending)
	{
		if (QueueProceduralTransferAbort(NetworkProceduralTransfer.TransferId,
			NetworkProceduralTransfer.CancelReason.GetChars(),
			NetworkProceduralTransfer.LastQueuedEventTic))
		{
			P_ResetNetworkProceduralMapTransferState();
		}
		return;
	}

	if (!NetworkProceduralTransfer.BeginSent)
	{
		const auto& archive = NetworkProceduralTransfer.Archive;
		TArray<uint8_t> bytes;
		TArrayView<uint8_t> stream;
		BeginProceduralEvent(bytes, 1 + 4 + 4 + 1 + 6 + 4 + 4 + 4 +
			archive.Theme.Len() + 1, DEM_PROCMAP_BEGIN, stream);
		WriteInt32(NetworkProceduralTransfer.TransferId, stream);
		WriteInt32(archive.Seed, stream);
		WriteInt8(NetworkProceduralTransfer.Roster, stream);
		WriteInt8((uint8_t)archive.Difficulty, stream);
		WriteInt8((uint8_t)archive.Size, stream);
		WriteInt8((uint8_t)archive.Layout, stream);
		WriteInt8((uint8_t)archive.Verticality, stream);
		WriteInt8((uint8_t)archive.Detail, stream);
		WriteInt8((uint8_t)archive.Outdoors, stream);
		WriteInt32((int32_t)archive.UDMF.Len(), stream);
		WriteInt32((int32_t)NetworkProceduralTransfer.Checksum, stream);
		WriteInt32(NetworkProceduralTransfer.ChunkCount, stream);
		WriteString(archive.Theme.GetChars(), stream);
		if (QueueProceduralEvent(bytes, NetworkProceduralTransfer.LastQueuedEventTic))
			NetworkProceduralTransfer.BeginSent = true;
		return;
	}

	if (NetworkProceduralTransfer.NextOffset < NetworkProceduralTransfer.Archive.UDMF.Len())
	{
		const size_t remaining = NetworkProceduralTransfer.Archive.UDMF.Len() -
			NetworkProceduralTransfer.NextOffset;
		const size_t length = min(remaining, ProceduralTransferChunkSize);
		TArray<uint8_t> bytes;
		TArrayView<uint8_t> stream;
		BeginProceduralEvent(bytes, 1 + 4 + 4 + 2 + length, DEM_PROCMAP_CHUNK, stream);
		WriteInt32(NetworkProceduralTransfer.TransferId, stream);
		WriteInt32(NetworkProceduralTransfer.NextChunk, stream);
		WriteInt16((int16_t)length, stream);
		memcpy(stream.Data(), NetworkProceduralTransfer.Archive.UDMF.GetChars() +
			NetworkProceduralTransfer.NextOffset, length);
		AdvanceStream(stream, length);
		if (QueueProceduralEvent(bytes, NetworkProceduralTransfer.LastQueuedEventTic))
		{
			NetworkProceduralTransfer.NextOffset += length;
			++NetworkProceduralTransfer.NextChunk;
		}
		return;
	}

	if (!NetworkProceduralTransfer.FinishSent)
	{
		TArray<uint8_t> bytes;
		TArrayView<uint8_t> stream;
		BeginProceduralEvent(bytes, 1 + 4, DEM_PROCMAP_FINISH, stream);
		WriteInt32(NetworkProceduralTransfer.TransferId, stream);
		if (QueueProceduralEvent(bytes, NetworkProceduralTransfer.LastQueuedEventTic))
			NetworkProceduralTransfer.FinishSent = true;
		return;
	}

	if ((NetworkProceduralTransfer.ReceivedAcks & NetworkProceduralTransfer.RequiredAcks) ==
		NetworkProceduralTransfer.RequiredAcks)
	{
		NetworkProceduralTransfer.ChangeMapPending = true;
	}

	if (NetworkProceduralTransfer.ChangeMapPending)
	{
		TArray<uint8_t> bytes;
		TArrayView<uint8_t> stream;
		BeginProceduralEvent(bytes, 1 + strlen("PROCMAP") + 1, DEM_CHANGEMAP, stream);
		WriteString("PROCMAP", stream);
		if (QueueProceduralEvent(bytes, NetworkProceduralTransfer.LastQueuedEventTic))
		{
			const int transferId = NetworkProceduralTransfer.TransferId;
			ResetProceduralTransfer();
			Printf("All peers verified shared procedural archive %d; changing to PROCMAP.\n",
				transferId);
		}
	}
}

void P_HandleNetworkProceduralMapBegin(TArrayView<uint8_t>& stream, int player)
{
	const int transferId = ReadInt32(stream);
	const int seed = ReadInt32(stream);
	const uint8_t roster = ReadInt8(stream);
	const int difficulty = ReadInt8(stream);
	const int size = ReadInt8(stream);
	const int layout = ReadInt8(stream);
	const int verticality = ReadInt8(stream);
	const int detail = ReadInt8(stream);
	const int outdoors = ReadInt8(stream);
	const int32_t archiveSize = ReadInt32(stream);
	const uint32_t checksum = (uint32_t)ReadInt32(stream);
	const int32_t chunkCount = ReadInt32(stream);
	const FString theme = ReadStringConst(stream);

	// The host also observes its own event in the deterministic command stream;
	// it already staged the archive before sending metadata.
	if (consoleplayer == Net_Arbitrator)
		return;
	if (player != Net_Arbitrator)
		return;
	if (NetworkProceduralReceiver.Active || NetworkProceduralReceiver.Staged ||
		NetworkProceduralReceiver.CancelPending)
	{
		RejectProceduralTransfer(transferId, player,
			"received a second archive before the first completed");
		return;
	}

	FString normalizedTheme = theme;
	normalizedTheme.ToLower();
	const size_t rawLength = archiveSize > 0 ? (size_t)archiveSize : 0;
	const int expectedChunks = rawLength > 0 ? (int)((rawLength +
		ProceduralTransferChunkSize - 1) / ProceduralTransferChunkSize) : 0;
	if (transferId <= 0 || roster != LocalProceduralRoster() ||
		!IsProceduralTheme(normalizedTheme) || difficulty < 1 ||
		difficulty > 5 || size < FProceduralMapGenerator::MinMapSize ||
		size > FProceduralMapGenerator::MaxMapSize || layout < 0 || layout > 2 ||
		verticality < 0 || verticality > 2 || detail < 0 || detail > 2 ||
		outdoors < 0 || outdoors > 2 || rawLength == 0 ||
		rawLength > MaxArchivedProceduralMapSize || chunkCount != expectedChunks)
	{
		RejectProceduralTransfer(transferId, player,
			"host supplied incompatible or invalid transfer metadata");
		return;
	}

	NetworkProceduralReceiver = FProceduralTransferReceiver();
	NetworkProceduralReceiver.Active = true;
	NetworkProceduralReceiver.TransferId = transferId;
	NetworkProceduralReceiver.HostPlayer = player;
	NetworkProceduralReceiver.Roster = roster;
	NetworkProceduralReceiver.Seed = seed;
	NetworkProceduralReceiver.Theme = normalizedTheme;
	NetworkProceduralReceiver.Difficulty = difficulty;
	NetworkProceduralReceiver.Size = size;
	NetworkProceduralReceiver.Layout = layout;
	NetworkProceduralReceiver.Verticality = verticality;
	NetworkProceduralReceiver.Detail = detail;
	NetworkProceduralReceiver.Outdoors = outdoors;
	NetworkProceduralReceiver.ArchiveSize = rawLength;
	NetworkProceduralReceiver.Checksum = checksum;
	NetworkProceduralReceiver.ChunkCount = chunkCount;
	NetworkProceduralReceiver.Deadline = ProceduralTransferDeadline(chunkCount);
	NetworkProceduralReceiver.ArchiveBytes.Resize((unsigned)rawLength);
	Printf("Receiving shared procedural archive from host (%lu KiB).\n",
		(unsigned long)((rawLength + 1023) / 1024));
}

void P_HandleNetworkProceduralMapChunk(TArrayView<uint8_t>& stream, int player)
{
	const int transferId = ReadInt32(stream);
	const int chunkIndex = ReadInt32(stream);
	const int length = ReadInt16(stream);
	if (length < 0 || (size_t)length > stream.Size())
	{
		if (consoleplayer != Net_Arbitrator && player == Net_Arbitrator &&
			NetworkProceduralReceiver.Active &&
			transferId == NetworkProceduralReceiver.TransferId)
		{
			RejectProceduralTransfer(transferId, player, "received a malformed archive chunk");
		}
		return;
	}

	const uint8_t* bytes = stream.Data();
	AdvanceStream(stream, (size_t)length);
	if (consoleplayer == Net_Arbitrator || player != Net_Arbitrator)
		return;
	if (!NetworkProceduralReceiver.Active || transferId != NetworkProceduralReceiver.TransferId)
		return;

	const size_t remaining = NetworkProceduralReceiver.ArchiveSize -
		NetworkProceduralReceiver.NextOffset;
	const size_t expectedLength = min(remaining, ProceduralTransferChunkSize);
	if (chunkIndex != NetworkProceduralReceiver.NextChunk ||
		(size_t)length != expectedLength || length == 0)
	{
		RejectProceduralTransfer(transferId, player, "archive chunks arrived out of order");
		return;
	}
	memcpy(NetworkProceduralReceiver.ArchiveBytes.Data() +
		NetworkProceduralReceiver.NextOffset, bytes, (size_t)length);
	NetworkProceduralReceiver.NextOffset += length;
	++NetworkProceduralReceiver.NextChunk;
	NetworkProceduralReceiver.Deadline = ProceduralTransferDeadline(
		NetworkProceduralReceiver.ChunkCount);
}

void P_HandleNetworkProceduralMapFinish(TArrayView<uint8_t>& stream, int player)
{
	const int transferId = ReadInt32(stream);
	if (consoleplayer == Net_Arbitrator || player != Net_Arbitrator)
		return;
	if (!NetworkProceduralReceiver.Active || transferId != NetworkProceduralReceiver.TransferId)
		return;
	if (NetworkProceduralReceiver.NextChunk != NetworkProceduralReceiver.ChunkCount ||
		NetworkProceduralReceiver.NextOffset != NetworkProceduralReceiver.ArchiveSize)
	{
		RejectProceduralTransfer(transferId, player,
			"archive transfer ended before all chunks arrived");
		return;
	}

	FString udmf;
	if (!VerifyReceivedProceduralArchive(NetworkProceduralReceiver, udmf) ||
		!P_StageProceduralMapArchive(NetworkProceduralReceiver.Seed,
			NetworkProceduralReceiver.Theme.GetChars(), NetworkProceduralReceiver.Difficulty,
			NetworkProceduralReceiver.Size, NetworkProceduralReceiver.Layout,
			NetworkProceduralReceiver.Verticality, NetworkProceduralReceiver.Detail,
			NetworkProceduralReceiver.Outdoors, std::move(udmf)))
	{
		RejectProceduralTransfer(transferId, player,
			"archive checksum or staging validation failed");
		return;
	}

	NetworkProceduralReceiver.ArchiveBytes.Clear();
	NetworkProceduralReceiver.Active = false;
	NetworkProceduralReceiver.Staged = true;
	NetworkProceduralReceiver.AckPending = true;
	NetworkProceduralReceiver.Deadline = gametic + ProceduralTransferGraceTics;
	Printf("Shared procedural archive verified; waiting for the host to change maps.\n");
}

void P_HandleNetworkProceduralMapAck(TArrayView<uint8_t>& stream, int player)
{
	const int transferId = ReadInt32(stream);
	const uint32_t checksum = (uint32_t)ReadInt32(stream);
	if (consoleplayer != Net_Arbitrator || player == Net_Arbitrator ||
		!NetworkProceduralTransfer.Active ||
		transferId != NetworkProceduralTransfer.TransferId ||
		!NetworkClients.InGame(player))
	{
		return;
	}
	if (checksum != NetworkProceduralTransfer.Checksum)
	{
		P_CancelNetworkProceduralMapTransfer("a peer reported a checksum mismatch");
		return;
	}
	NetworkProceduralTransfer.ReceivedAcks |= PlayerBit(player);
}

void P_HandleNetworkProceduralMapAbort(TArrayView<uint8_t>& stream, int player)
{
	const int transferId = ReadInt32(stream);
	const FString reason = BoundedProceduralTransferReason(ReadStringConst(stream));
	if (consoleplayer == Net_Arbitrator)
	{
		if (NetworkProceduralTransfer.Active && transferId == NetworkProceduralTransfer.TransferId &&
			player != Net_Arbitrator && NetworkClients.InGame(player))
		{
			P_CancelNetworkProceduralMapTransfer(reason.GetChars());
		}
		return;
	}

	if (player == Net_Arbitrator && transferId == NetworkProceduralReceiver.TransferId &&
		(NetworkProceduralReceiver.Active || NetworkProceduralReceiver.Staged ||
			NetworkProceduralReceiver.CancelPending || NetworkProceduralReceiver.AckPending))
	{
		ResetProceduralReceiver();
		ClearPendingProceduralMap();
		Printf(TEXTCOLOR_YELLOW "Host cancelled shared procedural map transfer: %s\n",
			reason.GetChars());
	}
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

static bool CanEditProceduralRecipe()
{
	if (!netgame)
		return true;
	if (consoleplayer == Net_Arbitrator && players[consoleplayer].settings_controller)
		return true;
	Printf(TEXTCOLOR_RED "Only the host can change the shared procedural recipe.\n");
	return false;
}

CCMD(procmap_randomize_seed)
{
	if (!CanEditProceduralRecipe())
		return;
	procgen_seed = MakeProceduralMenuSeed();
	Printf("Procedural map seed set to %d.\n", (int)procgen_seed);
}

CCMD(procmap_restore_defaults)
{
	if (!CanEditProceduralRecipe())
		return;
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
	if (!CanEditProceduralRecipe())
		return;
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

CCMD(procmap_cancel)
{
	if (!P_IsNetworkProceduralMapTransferActive())
	{
		Printf("No shared procedural transfer is active.\n");
		return;
	}
	P_CancelNetworkProceduralMapTransfer("cancelled by a player");
}

CCMD(procmap_transfer_status)
{
	if (NetworkProceduralTransfer.Active)
	{
		Printf("Shared procedural transfer %d: chunk %d/%d%s.\n",
			NetworkProceduralTransfer.TransferId, NetworkProceduralTransfer.NextChunk,
			NetworkProceduralTransfer.ChunkCount,
			NetworkProceduralTransfer.CancelPending ? " (cancelling)" : "");
		return;
	}
	if (NetworkProceduralReceiver.Active || NetworkProceduralReceiver.Staged ||
		NetworkProceduralReceiver.AckPending || NetworkProceduralReceiver.CancelPending)
	{
		Printf("Shared procedural receive %d: chunk %d/%d%s%s.\n",
			NetworkProceduralReceiver.TransferId, NetworkProceduralReceiver.NextChunk,
			NetworkProceduralReceiver.ChunkCount,
			NetworkProceduralReceiver.Staged ? " (verified)" : "",
			NetworkProceduralReceiver.CancelPending ? " (cancelling)" : "");
		return;
	}
	Printf("No shared procedural transfer is active.\n");
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
	if (!CanEditProceduralRecipe())
		return;
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
