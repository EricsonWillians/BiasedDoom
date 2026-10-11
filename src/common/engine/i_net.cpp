// Emacs style mode select	 -*- C++ -*- 
//-----------------------------------------------------------------------------
//
// $Id: i_net.c,v 1.2 1997/12/29 19:50:54 pekangas Exp $
//
// Copyright (C) 1993-1996 by id Software, Inc.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see http://www.gnu.org/licenses/
//
//
//
// Alternatively the following applies:
//
// This source is available for distribution and/or modification
// only under the terms of the DOOM Source Code License as
// published by id Software. All rights reserved.
//
// The source is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// FITNESS FOR A PARTICULAR PURPOSE. See the DOOM Source Code License
// for more details.
//
//
// DESCRIPTION:
//		Low-level networking code. Uses BSD sockets for UDP networking.
//
//-----------------------------------------------------------------------------


/* [Petteri] Check if compiling for Win32:	*/
#if defined(__WINDOWS__) || defined(__NT__) || defined(_MSC_VER) || defined(_WIN32)
#ifndef __WIN32__
#	define __WIN32__
#endif
#endif
/* Follow #ifdef __WIN32__ marks */

#include <stdlib.h>
#include <string.h>

/* [Petteri] Use Winsock for Win32: */
#ifdef __WIN32__
#	define WIN32_LEAN_AND_MEAN
#	include <windows.h>
#	include <winsock.h>
#else
#	include <sys/socket.h>
#	include <netinet/in.h>
#	include <arpa/inet.h>
#	include <errno.h>
#	include <unistd.h>
#	include <netdb.h>
#	include <sys/ioctl.h>
#	ifdef __sun
#		include <fcntl.h>
#	endif
#endif

#include "i_system.h"
#include "m_argv.h"
#include "m_crc32.h"
#include "st_start.h"
#include "engineerrors.h"
#include "cmdlib.h"
#include "printf.h"
#include "i_interface.h"
#include "c_cvars.h"
#include "i_net.h"
#include "d_net.h"
#include "doomstat.h"
#include "m_random.h"

#ifdef BIASEDDOOM_ONLINE
#include "common/engine/filesystem.h"
#include "common/engine/i_specialpaths.h"
#include "common/online/online_service.h"
#include "common/online/online_join.h"
#include "common/online/online_snapshot.h"
#include "common/online/online_transport.h"
#include "g_game.h"
#include "version.h"
#include "d_netinf.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <limits>
#include <string>
#include <system_error>
#include <vector>
#endif

/* [Petteri] Get more portable: */
#ifndef __WIN32__
typedef int SOCKET;
#define SOCKET_ERROR		-1
#define INVALID_SOCKET		-1
#define closesocket			close
#define ioctlsocket			ioctl
#define Sleep(x)			usleep (x * 1000)
#define WSAEWOULDBLOCK		EWOULDBLOCK
#define WSAECONNRESET		ECONNRESET
#define WSAGetLastError()	errno
#endif

#ifndef IPPORT_USERRESERVED
#define IPPORT_USERRESERVED 5000
#endif

#ifdef __WIN32__
# include "common/scripting/dap/GameEventEmit.h"
typedef int socklen_t;
const char* neterror(void);
#else
#define neterror() strerror(errno)
#endif

// As per http://support.microsoft.com/kb/q192599/ the standard
// size for network buffers is 8k.
constexpr size_t MaxTransmitSize = 8000u;
constexpr size_t MinCompressionSize = 10u;
constexpr size_t MaxPasswordSize = 256u;

enum ENetConnectType : uint8_t
{
	PRE_HEARTBEAT,			// Host and guests are keep each other's connections alive
	PRE_CONNECT,			// Sent from guest to host for initial connection
	PRE_CONNECT_ACK,		// Sent from host to guest to confirm they've been connected
	PRE_DISCONNECT,			// Sent from host to guest when another guest leaves
	PRE_USER_INFO,			// Host and guests are sending each other user infos
	PRE_USER_INFO_ACK,		// Host and guests are confirming sent user infos
	PRE_GAME_INFO,			// Sent from host to guest containing general game info
	PRE_GAME_INFO_ACK,		// Sent from guest to host confirming game info was gotten
	PRE_GO,					// Sent from host to guest telling them to start the game

	PRE_FULL,				// Sent from host to guest if the lobby is full
	PRE_IN_PROGRESS,		// Sent from host to guest if the game has already started
	PRE_WRONG_PASSWORD,		// Sent from host to guest if their provided password was wrong
	PRE_WRONG_ENGINE,		// Sent from host to guest if their engine version doesn't match the host's
	PRE_INVALID_FILES,		// Sent from host to guest if their files do not match the host's
	PRE_KICKED,				// Sent from hsot to guest if the host kicked them from the game
	PRE_BANNED,				// Sent from host to guest if the host banned them from the game
};

enum EConnectionStatus
{
	CSTAT_NONE,			// Guest isn't connected
	CSTAT_CONNECTING,	// Guest is trying to connect
	CSTAT_WAITING,		// Guest is waiting for game info
	CSTAT_READY,		// Guest is ready to start the game
};

// These need to be synced with the window backends so information about each
// client can be properly displayed.
enum EConnectionFlags : unsigned int
{
	CFL_NONE			= 0,
	CFL_CONSOLEPLAYER	= 1,
	CFL_HOST			= 1 << 1,
};

struct FConnection
{
	EConnectionStatus Status = CSTAT_NONE;
	sockaddr_in Address = {};
	uint64_t InfoAck = 0u;
	bool bHasGameInfo = false;

	void Clear()
	{
		Status = CSTAT_NONE;
		Address = {};
		InfoAck = 0u;
		bHasGameInfo = false;
	}
};

bool netgame = false;
bool multiplayer = false;
ENetMode NetMode = NET_PeerToPeer;
int consoleplayer = 0;
int Net_Arbitrator = 0;
FClientStack NetworkClients = {};

uint8_t	TicDup = 1u;
int	MaxClients = 1;
int RemoteClient = -1;
size_t NetBufferLength = 0u;
uint8_t NetBuffer[MAX_MSGLEN] = {};

static FRandom		GameIDGen = {};
static uint8_t		GameID[8] = {};
static u_short		GamePort = (IPPORT_USERRESERVED + 29);
static SOCKET		MySocket = INVALID_SOCKET;
static FConnection	Connected[MAXPLAYERS] = {};
static uint8_t		TransmitBuffer[MaxTransmitSize] = {};
static TArray<sockaddr_in> BannedConnections = {};
static bool bGameStarted = false;

// d_net owns the legacy player roster and arbitrator reset. The online layer
// asks it to run that already-tested transition only after the directory has
// made an authenticated recovery election.
void Net_OnlineHostRecovered(int newHost);

#ifdef BIASEDDOOM_ONLINE
// Online sessions still use the proven legacy packet protocol, but packets
// travel through this encrypted relay adapter. `Connected[]` receives stable
// synthetic addresses, never a peer's public address.
static bool OnlineSessionActive = false;
static bool OnlineSessionIsHost = false;
// Set from the signed join ticket. This selects the isolated snapshot startup
// path instead of the legacy pre-game setup handshake.
static bool OnlineJoinInProgress = false;
static std::chrono::steady_clock::time_point OnlineNextLeaseRenewal = {};
static sockaddr_in OnlineRelayAddress = {};
static BdOnline::FOnlineRelayTransport OnlineRelayTransport = {};
static BdOnline::FOnlineSession OnlineSession = {};
static BdOnline::FOnlineDeviceIdentity OnlineIdentity = {};
// Keep only a bounded post-snapshot input history. Snapshot handoff is not
// publicly admitted until its whole restore path is complete, but recording
// the authoritative inputs here prevents a later admission path from having
// to reconstruct already-consumed legacy packets.
static BdOnline::FOnlineTicHistory OnlineTicHistory;
// Only one join handoff may consume the retained authoritative history at a
// time. That bound keeps an untrusted sequence of join attempts from turning
// a player-hosted game into a multi-gigabyte snapshot queue. A later admission
// coordinator can queue approved guests outside the real-time packet path.
struct FOnlineSnapshotHandoff
{
	BdOnline::FOnlineRelayTransport::FPeerId Target = {};
	BdOnline::FOnlineSnapshotSender Sender;
	BdOnline::FOnlineSnapshotMetadata Metadata;
	uint64_t RequestId = 0;
	int PlayerSlot = -1;
	uint64_t HighestReplaySent = 0;
	uint64_t HighestReplayAcknowledged = 0;
	uint64_t CompletionTic = 0;
	bool CompletionSent = false;
	bool CompletionAcknowledged = false;
	bool GuestReady = false;
	bool JoinEventQueued = false;
	std::chrono::steady_clock::time_point NextFrame = {};
	std::chrono::steady_clock::time_point Deadline = {};

	// Completion acknowledgement only proves that the guest received a bounded
	// archive/replay range. Keep this handoff alive through its later Ready and
	// Activated controls so a second requester cannot steal the same slot.
	bool IsActive() const { return Sender.IsActive(); }

	void Reset()
	{
		Target = {};
		Sender.Reset();
		Metadata = {};
		RequestId = 0;
		PlayerSlot = -1;
		HighestReplaySent = 0;
		HighestReplayAcknowledged = 0;
		CompletionTic = 0;
		CompletionSent = false;
		CompletionAcknowledged = false;
		GuestReady = false;
		JoinEventQueued = false;
		NextFrame = {};
		Deadline = {};
	}
};
static FOnlineSnapshotHandoff OnlineSnapshotHandoff;

// Never invoke a control-plane request from the UDP receive/parser path. A
// join request is authenticated by the relay, but the host still has to ask
// the directory whether that anonymous peer actually owns a session slot
// rather than a short-lived content-review route. Keeping this queue bounded
// also prevents relay traffic from turning into unbounded game-thread work.
struct FOnlinePendingJoinAdmission
{
	BdOnline::FOnlineRelayTransport::FPeerId Sender = {};
	BdOnline::FOnlineJoinRequestFrame Request;
};
static std::deque<FOnlinePendingJoinAdmission> OnlinePendingJoinAdmissions;
inline constexpr size_t OnlineMaxPendingJoinAdmissions = 4;
struct FOnlineSnapshotReception
{
	BdOnline::FOnlineSnapshotReceiver Receiver;
	BdOnline::FOnlineSnapshotReplayBuffer Replay;
	BdOnline::FOnlineSnapshotMetadata Metadata;
	uint64_t LiveTic = 0;
	bool CompletionFenceReceived = false;
	std::chrono::steady_clock::time_point NextAcknowledgement = {};

	bool Matches(const BdOnline::FOnlineSnapshotMetadata& candidate) const
	{
		return Metadata.Version == candidate.Version &&
			Metadata.SerializerVersion == candidate.SerializerVersion &&
			Metadata.Epoch == candidate.Epoch && Metadata.SnapshotTic == candidate.SnapshotTic &&
			Metadata.TransferId == candidate.TransferId &&
			Metadata.TotalBytes == candidate.TotalBytes && Metadata.UncompressedBytes == candidate.UncompressedBytes &&
			Metadata.CompressionMethod == candidate.CompressionMethod && Metadata.CRC32 == candidate.CRC32 &&
			Metadata.Digest == candidate.Digest;
	}

	bool MatchesIdentity(uint32_t epoch, uint64_t snapshotTic, uint64_t transferId,
		const std::string& digest) const
	{
		return Metadata.Epoch == epoch && Metadata.SnapshotTic == snapshotTic &&
			Metadata.TransferId == transferId && Metadata.Digest == digest;
	}

	void Reset()
	{
		Receiver.Reset();
		Replay.Reset();
		Metadata = {};
		LiveTic = 0;
		CompletionFenceReceived = false;
		NextAcknowledgement = {};
	}
};
// This is deliberately only the encrypted transport-side reception state.
// It cannot activate a player: the engine restore and dynamic-roster path own
// that final transition after a completed, contiguous replay range exists.
static FOnlineSnapshotReception OnlineSnapshotReception;

// Startup join-in-progress receives and validates the archive before the
// engine creates its ordinary initial map. d_main restores it after network
// event streams are initialized, then this state follows the final roster
// event through the same replay history as every other participant.
struct FOnlineStartupJoin
{
	bool Active = false;
	bool Accepted = false;
	bool ReadyForRestore = false;
	bool Restored = false;
	bool Activated = false;
	bool ActivationConfirmed = false;
	int PlayerSlot = -1;
	uint64_t AppliedTic = 0;
	BdOnline::FOnlineJoinRequestFrame Request;
	std::string Failure;
	std::chrono::steady_clock::time_point NextRequest = {};
	std::chrono::steady_clock::time_point NextReady = {};
	std::chrono::steady_clock::time_point NextActivated = {};
	std::chrono::steady_clock::time_point ActivationConfirmationDeadline = {};
	std::chrono::steady_clock::time_point StartupDeadline = {};

	void Reset()
	{
		Active = false;
		Accepted = false;
		ReadyForRestore = false;
		Restored = false;
		Activated = false;
		ActivationConfirmed = false;
		PlayerSlot = -1;
		AppliedTic = 0;
		Request = {};
		Failure.clear();
		NextRequest = {};
		NextReady = {};
		NextActivated = {};
		ActivationConfirmationDeadline = {};
		StartupDeadline = {};
	}
};
static FOnlineStartupJoin OnlineStartupJoin;
static std::unique_ptr<BdOnline::FOnlineServiceClient> OnlineServiceClient;
static std::map<BdOnline::FOnlineRelayTransport::FPeerId, sockaddr_in> OnlinePeerAddresses;
static std::map<uint32_t, BdOnline::FOnlineRelayTransport::FPeerId> OnlineAddressPeers;
// Direct routes are strictly an optimization. They are populated only after a
// signed identity probe from the relay-observed candidate and are cleared on
// every epoch change; the encrypted relay remains the safe fallback.
static std::map<BdOnline::FOnlineRelayTransport::FPeerId, sockaddr_in> OnlineDirectPeerAddresses;
static std::map<BdOnline::FOnlineRelayTransport::FPeerId, std::chrono::steady_clock::time_point> OnlineDirectPeerLastReceived;
static sockaddr_in OnlineHostDirectCandidate = {};
static bool OnlineHostDirectCandidateValid = false;
static std::string OnlineHostDirectPublicKey;
static std::chrono::steady_clock::time_point OnlineNextDirectProbe = {};
static unsigned OnlineDirectProbeAttempts = 0;
constexpr auto OnlineDirectRouteSilenceTimeout = std::chrono::seconds(12);
struct FOnlinePendingDirectProbe
{
	BdOnline::FOnlineRelayTransport::FPeerId Sender = {};
	sockaddr_in Source = {};
	uint64_t Timestamp = 0;
	std::array<uint8_t, 64> Signature = {};
};
static std::deque<FOnlinePendingDirectProbe> OnlinePendingDirectProbes;
inline constexpr size_t OnlineMaxPendingDirectProbes = 4;
static uint32_t NextOnlineAddress = 1;
// BDN1 has a relay prefix, sender label/capability, recipient label, epoch,
// and sequence in addition to the encrypted client datagram.
static std::array<uint8_t, BdOnline::OnlineMaxDatagramBytes + 96> OnlineReceiveBuffer = {};
static std::chrono::steady_clock::time_point OnlineLastHostPacket = {};
static std::chrono::steady_clock::time_point OnlineNextRecoveryAttempt = {};
static std::chrono::steady_clock::time_point OnlineNextLivenessHeartbeat = {};
constexpr auto OnlineHostSilenceTimeout = std::chrono::seconds(15);
constexpr auto OnlineHostLivenessInterval = std::chrono::seconds(5);
struct FOnlinePendingHostMigration
{
	bool Valid = false;
	uint32_t Epoch = 0;
	std::string PreviousHostPeerId;
	std::string HostPeerId;
};
static FOnlinePendingHostMigration OnlinePendingHostMigration = {};
struct FOnlineContentSource
{
	std::filesystem::path Path;
	uint64_t Size = 0;
};
static std::map<std::string, FOnlineContentSource> OnlineContentSources;

static bool ParseOnlineRelayEndpoint(const std::string& endpoint, sockaddr_in& address, std::string& error);
static bool ParseOnlineDirectCandidate(const std::string& endpoint, sockaddr_in& address, std::string& error);

#endif

CUSTOM_CVAR(String, net_password, "", CVAR_IGNORE)
{
	if (strlen(self) + 1 > MaxPasswordSize)
	{
		self = "";
		Printf(TEXTCOLOR_RED "Password cannot be greater than 255 characters\n");
	}
}

// Game-specific API
size_t Net_SetEngineInfo(uint8_t*& stream);
bool Net_VerifyEngine(uint8_t*& stream);
void Net_SetupUserInfo();
const char* Net_GetClientName(int client, unsigned int charLimit);
void Net_SetUserInfo(int client, TArrayView<uint8_t>& stream);
void Net_ReadUserInfo(int client, TArrayView<uint8_t>& stream);
void Net_ReadGameInfo(TArrayView<uint8_t>& stream);
void Net_SetGameInfo(TArrayView<uint8_t>& stream);

static SOCKET CreateUDPSocket()
{
	SOCKET s = socket(PF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (s == INVALID_SOCKET)
		I_FatalError("Couldn't create socket: %s", neterror());

	return s;
}

static void BindToLocalPort(SOCKET s, u_short port)
{
	sockaddr_in address = {};
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = INADDR_ANY;
	address.sin_port = htons(port);

	int v = bind(s, (sockaddr *)&address, sizeof(address));
	if (v == SOCKET_ERROR)
		I_FatalError("Couldn't bind to port: %s", neterror());
}

static void BuildAddress(sockaddr_in& address, const char* addrName)
{
	FString target = {};
	u_short port = GamePort;
	const char* portName = strchr(addrName, ':');
	if (portName != nullptr)
	{
		target = FString(addrName, portName - addrName);
		u_short portConversion = atoi(portName + 1);
		if (!portConversion)
			Printf("Malformed port: %s (using %d)\n", portName + 1, GamePort);
		else
			port = portConversion;
	}
	else
	{
		target = addrName;
	}

	bool isNamed = false;
	char c = 0;
	for (size_t curChar = 0u; (c = target[curChar]); ++curChar)
	{
		if ((c < '0' || c > '9') && c != '.')
		{
			isNamed = true;
			break;
		}
	}

	address.sin_family = AF_INET;
	address.sin_port = htons(port);
	if (!isNamed)
	{
		address.sin_addr.s_addr = inet_addr(target.GetChars());
	}
	else
	{
		hostent* hostEntry = gethostbyname(target.GetChars());
		if (hostEntry == nullptr)
			I_FatalError("gethostbyname: Couldn't find %s\n%s", target.GetChars(), neterror());

		address.sin_addr.s_addr = *(int*)hostEntry->h_addr_list[0];
	}
}

#ifdef BIASEDDOOM_ONLINE
namespace
{

constexpr const char* OnlineProfileName = "online-device-v1";
bool OnlineDecodeHex(const std::string& encoded, uint8_t* output, size_t outputSize);
std::string OnlineEncodeDigest(const uint8_t* digest, size_t size);

uint64_t OnlineAddressKey(const sockaddr_in& address)
{
	return (static_cast<uint64_t>(address.sin_addr.s_addr) << 16) | address.sin_port;
}

bool OnlineReadProfileValue(std::istream& input, const char* key, std::string& value)
{
	std::string line;
	if (!std::getline(input, line) || line.rfind(key, 0) != 0)
		return false;
	value = line.substr(std::strlen(key));
	return value.find_first_of("\r\n") == std::string::npos;
}

// An explicit data root makes it possible to run independent local players on
// one computer. It scopes both the anonymous device credential and downloaded
// content cache, so test clients do not accidentally share an identity or
// silently reuse each other's host-provided mods. Normal players continue to
// use the platform application-data/cache locations.
bool GetOnlineDataRoot(std::filesystem::path& root, std::string& error)
{
	const char* configured = Args->CheckValue("-online-data-dir");
	if (configured == nullptr)
	{
		root = std::filesystem::path(M_GetAppDataPath(true).GetChars());
		return true;
	}
	if (configured[0] == '\0')
	{
		error = "-online-data-dir requires an absolute directory path";
		return false;
	}
	root = std::filesystem::path(configured);
	if (!root.is_absolute())
	{
		error = "-online-data-dir must be an absolute directory path";
		return false;
	}
	std::error_code code;
	std::filesystem::create_directories(root, code);
	if (code)
	{
		error = "could not create -online-data-dir";
		return false;
	}
	const auto status = std::filesystem::symlink_status(root, code);
	if (code || std::filesystem::is_symlink(status) || !std::filesystem::is_directory(status))
	{
		error = "-online-data-dir must be a real directory, not a symlink";
		return false;
	}
	return true;
}

bool GetOnlineProfilePath(std::filesystem::path& path, std::string& error)
{
	std::filesystem::path root;
	if (!GetOnlineDataRoot(root, error))
		return false;
	path = root / OnlineProfileName;
	return true;
}

bool GetOnlineContentCacheRoot(std::filesystem::path& root, std::string& error)
{
	if (Args->CheckValue("-online-data-dir") != nullptr)
	{
		if (!GetOnlineDataRoot(root, error))
			return false;
		root /= "online-content";
	}
	else
	{
		root = std::filesystem::path(M_GetCachePath(true).GetChars()) / "online-content";
	}
	std::error_code code;
	std::filesystem::create_directories(root, code);
	if (code)
	{
		error = "could not create the online content cache directory";
		return false;
	}
	const auto status = std::filesystem::symlink_status(root, code);
	if (code || std::filesystem::is_symlink(status) || !std::filesystem::is_directory(status))
	{
		error = "online content cache must be a real directory, not a symlink";
		return false;
	}
	return true;
}

bool LoadOnlineIdentity(const std::string& serviceUrl, BdOnline::FOnlineServiceConfig& config, bool& needsRegistration, std::string& error)
{
	config = {};
	config.BaseUrl = serviceUrl;
	config.Identity = &OnlineIdentity;
	needsRegistration = true;
	std::filesystem::path path;
	if (!GetOnlineProfilePath(path, error))
		return false;
	std::error_code code;
	const auto status = std::filesystem::symlink_status(path, code);
	if (code && code != std::errc::no_such_file_or_directory)
	{
		error = "could not inspect the online identity profile";
		return false;
	}
	if (code || !std::filesystem::exists(status))
		return OnlineIdentity.Generate(error);
	if (std::filesystem::is_symlink(status) || !std::filesystem::is_regular_file(status) || std::filesystem::file_size(path, code) > 16 * 1024)
	{
		error = "online identity profile is unsafe or invalid";
		return false;
	}
	std::ifstream input(path, std::ios::binary);
	std::string version;
	std::string profileService;
	std::string privateKey;
	std::string deviceId;
	std::string accessToken;
	if (!input || !OnlineReadProfileValue(input, "version=", version) || version != "1" ||
		!OnlineReadProfileValue(input, "service=", profileService) || !OnlineReadProfileValue(input, "private_key=", privateKey) ||
		!OnlineReadProfileValue(input, "device_id=", deviceId) || !OnlineReadProfileValue(input, "access_token=", accessToken))
	{
		error = "online identity profile is malformed";
		return false;
	}
	if (profileService == serviceUrl && OnlineIdentity.ImportPrivateKeyBase64(privateKey, error))
	{
		config.DeviceId = std::move(deviceId);
		config.AccessToken = std::move(accessToken);
		needsRegistration = config.DeviceId.empty() || config.AccessToken.empty();
		return true;
	}
	// A profile for another service must never leak its bearer credential to
	// this one. Start a fresh anonymous device registration instead.
	error.clear();
	return OnlineIdentity.Generate(error);
}

bool SaveOnlineIdentity(const std::string& serviceUrl, const BdOnline::FOnlineServiceClient& client, std::string& error)
{
	std::filesystem::path path;
	if (!GetOnlineProfilePath(path, error))
		return false;
	const auto temporary = std::filesystem::path(path.string() + ".tmp");
	std::error_code code;
	const auto oldStatus = std::filesystem::symlink_status(path, code);
	if (!code && std::filesystem::is_symlink(oldStatus))
	{
		error = "online identity profile path is a symlink";
		return false;
	}
	code.clear();
	const auto temporaryStatus = std::filesystem::symlink_status(temporary, code);
	if (!code && std::filesystem::is_symlink(temporaryStatus))
	{
		error = "online identity temporary profile path is a symlink";
		return false;
	}
	{
		std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
		if (!output)
		{
			error = "could not write the online identity profile";
			return false;
		}
		output << "version=1\nservice=" << serviceUrl << "\nprivate_key=" << OnlineIdentity.ExportPrivateKeyBase64()
			<< "\ndevice_id=" << client.DeviceId() << "\naccess_token=" << client.AccessToken() << '\n';
		if (!output)
		{
			error = "could not finish the online identity profile";
			return false;
		}
	}
	std::filesystem::permissions(temporary, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
		std::filesystem::perm_options::replace, code);
	if (code)
	{
		std::filesystem::remove(temporary, code);
		error = "could not protect the online identity profile";
		return false;
	}
	// std::filesystem::rename cannot replace an existing file on Windows.
	// Device-token refreshes always replace this profile, so use the native
	// atomic replacement primitive there instead of making returning players
	// manually delete their profile after the first launch.
#ifdef __WIN32__
	if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
	{
		std::filesystem::remove(temporary, code);
		error = "could not atomically save the online identity profile";
		return false;
	}
#else
	std::filesystem::rename(temporary, path, code);
	if (code)
	{
		std::filesystem::remove(temporary, code);
		error = "could not atomically save the online identity profile";
		return false;
	}
#endif
	return true;
}

bool ConnectOnlineServiceUrl(const std::string& serviceUrl, std::string& error)
{
	if (serviceUrl.empty())
	{
		error = "online multiplayer requires -online-service <https-url> (or loopback HTTP for local development)";
		return false;
	}
	BdOnline::FOnlineServiceConfig config;
	bool needsRegistration = false;
	if (!LoadOnlineIdentity(serviceUrl, config, needsRegistration, error))
		return false;
	OnlineServiceClient = std::make_unique<BdOnline::FOnlineServiceClient>(config);
	// RegisterDevice is proof-of-possession based and renews the short-lived
	// bearer token for an existing device. Do it at startup so a player who has
	// not opened the game for a month does not have to delete their profile or
	// lose the device identity that anchors their online sessions.
	if (!OnlineServiceClient->RegisterDevice(OnlineIdentity, error))
		return false;
	if (!SaveOnlineIdentity(serviceUrl, *OnlineServiceClient, error))
		return false;
	return true;
}

bool ConnectOnlineService(std::string& error)
{
	const char* serviceValue = Args->CheckValue("-online-service");
	return serviceValue != nullptr ? ConnectOnlineServiceUrl(serviceValue, error) :
		ConnectOnlineServiceUrl({}, error);
}

// A selected game can require companion archives in addition to its primary
// IWAD. They are not mods and must never cross the online content route: the
// guest has to own matching local copies. Fold their ordered hashes into the
// existing IWAD compatibility field so the check covers the complete base
// game, not merely the first archive.
bool BuildOnlineBaseFingerprint(const std::vector<BdOnline::FPackageSource>& sources, std::string& digest, std::string& error)
{
	if (sources.empty())
	{
		error = "the selected game has no base resources to verify";
		return false;
	}
	BdOnline::FContentManifest fingerprint;
	if (!BdOnline::OnlineBuildContentManifest(sources, fingerprint, error))
		return false;
	digest = std::move(fingerprint.RootDigest);
	return true;
}

bool BuildOnlineLocalManifest(BdOnline::FSessionManifest& manifest, std::string& error)
{
	const int iwadIndex = fileSystem.GetIwadNum();
	const int maxBaseResource = fileSystem.GetMaxIwadNum();
	if (iwadIndex < 0 || maxBaseResource < iwadIndex || maxBaseResource >= fileSystem.GetNumWads())
	{
		error = "could not identify the selected base game for online multiplayer";
		return false;
	}
	std::vector<BdOnline::FPackageSource> baseSources;
	baseSources.reserve(static_cast<size_t>(maxBaseResource - iwadIndex + 1));
	for (int index = iwadIndex; index <= maxBaseResource; ++index)
	{
		const char* path = fileSystem.GetResourceFileFullName(index);
		const char* name = fileSystem.GetResourceFileName(index);
		if (path == nullptr || name == nullptr)
		{
			error = "could not identify an active base-game resource";
			return false;
		}
		baseSources.push_back({ std::filesystem::path(path), name, static_cast<uint32_t>(index - iwadIndex + 1), false, false });
	}
	std::string baseDigest;
	if (!BuildOnlineBaseFingerprint(baseSources, baseDigest, error))
		return false;

	std::vector<BdOnline::FPackageSource> sources;
	const int firstTransferable = maxBaseResource + 1;
	for (int index = firstTransferable; index < fileSystem.GetNumWads(); ++index)
	{
		const char* path = fileSystem.GetResourceFileFullName(index);
		const char* name = fileSystem.GetResourceFileName(index);
		if (path == nullptr || name == nullptr)
		{
			error = "could not identify an active online content resource";
			return false;
		}
		std::error_code code;
		if (std::filesystem::is_directory(std::filesystem::path(path), code) && !code)
		{
			error = "directory-backed mods must be bundled before starting an online session";
			return false;
		}
		sources.push_back({ std::filesystem::path(path), name, static_cast<uint32_t>(index - firstTransferable + 1), false, false });
	}
	BdOnline::FContentManifest content;
	if (!BdOnline::OnlineBuildContentManifest(sources, content, error))
		return false;
	OnlineContentSources.clear();
	if (sources.size() != content.Packages.size())
	{
		error = "online content plan lost an active resource";
		return false;
	}
	for (size_t index = 0; index < sources.size(); ++index)
		OnlineContentSources.emplace(content.Packages[index].Digest, FOnlineContentSource{ sources[index].Path, content.Packages[index].Size });
	manifest = {};
	manifest.EngineVersion = GetVersionString();
	manifest.IWADDigest = std::move(baseDigest);
	const char* title = Args->CheckValue("-online-title");
	manifest.Title = title != nullptr && title[0] != '\0' ? title : "BiasedDoom online game";
	const char* region = Args->CheckValue("-online-region");
	manifest.Region = region != nullptr ? region : "";
	const char* requestedMode = Args->CheckValue("-online-mode");
	if (requestedMode != nullptr && requestedMode[0] != '\0')
		manifest.GameMode = requestedMode;
	else if (Args->CheckParm("-coop"))
		manifest.GameMode = "co-op";
	else if (Args->CheckParm("-deathmatch") || Args->CheckParm("-altdeath"))
		manifest.GameMode = "deathmatch";
	else
		manifest.GameMode = "standard";
	manifest.Visibility = Args->CheckParm("-online-public") ? BdOnline::ESessionVisibility::Public : BdOnline::ESessionVisibility::Private;
	const char* slots = Args->CheckValue("-online-slots");
	manifest.MaxPlayers = static_cast<uint32_t>(clamp<int>(slots != nullptr ? atoi(slots) : 2, 1, MAXPLAYERS));
	manifest.PasswordRequired = net_password[0] != '\0';
	// Online hosts take a bounded serializer snapshot and retain a replay fence
	// for one authenticated guest at a time, so this capability applies both to
	// the initial lobby and a session that has already entered gameplay.
	manifest.AllowJoinInProgress = true;
	manifest.Content = std::move(content);
	return manifest.Validate(error);
}

bool ValidateOnlineLocalManifest(const BdOnline::FSessionManifest& expected, std::string& error)
{
	BdOnline::FSessionManifest actual;
	if (!BuildOnlineLocalManifest(actual, error))
		return false;
	if (actual.EngineVersion != expected.EngineVersion)
	{
		error = "the online host uses a different BiasedDoom version";
		return false;
	}
	if (actual.IWADDigest != expected.IWADDigest)
	{
		error = "the selected IWAD does not match this online session";
		return false;
	}
	const int firstTransferable = fileSystem.GetMaxIwadNum() + 1;
	if (fileSystem.GetNumWads() - firstTransferable != static_cast<int>(expected.Content.Packages.size()))
	{
		error = "the active mod count does not match this online session";
		return false;
	}
	for (size_t index = 0; index < expected.Content.Packages.size(); ++index)
	{
		const int resourceIndex = firstTransferable + static_cast<int>(index);
		const char* path = fileSystem.GetResourceFileFullName(resourceIndex);
		if (path == nullptr)
		{
			error = "an active online resource has no local path";
			return false;
		}
		BdOnline::FContentPackage local;
		BdOnline::FPackageSource source = { path, expected.Content.Packages[index].DisplayName, expected.Content.Packages[index].LoadOrder, false, false };
		if (!BdOnline::OnlineHashContentSource(source, local, error))
			return false;
		const auto& advertised = expected.Content.Packages[index];
		if (local.Digest != advertised.Digest || local.Size != advertised.Size || local.FileExtension != advertised.FileExtension ||
			local.LoadOrder != advertised.LoadOrder)
		{
			error = "the active mod bytes or load order do not match this online session";
			return false;
		}
	}
	return true;
}

bool ConfigureOnlineRelay(const std::string& endpoint, const std::string& sessionKey, const std::string& relayToken,
	const std::string& localPeerId, const std::string& relayAuth, uint32_t epoch, std::string& error, bool preservePeerRoutes = false)
{
	constexpr const char* udp = "udp://";
	if (endpoint.rfind(udp, 0) != 0 || endpoint.size() <= std::strlen(udp) || endpoint.find_first_of("/?#", std::strlen(udp)) != std::string::npos)
	{
		error = "online service returned an invalid UDP endpoint";
		return false;
	}
	BuildAddress(OnlineRelayAddress, endpoint.c_str() + std::strlen(udp));
	if (!OnlineRelayTransport.Configure(sessionKey, relayToken, localPeerId, relayAuth, epoch, error))
		return false;
	if (!preservePeerRoutes)
	{
		OnlinePeerAddresses.clear();
		OnlineAddressPeers.clear();
		NextOnlineAddress = 1;
	}
	// Direct mappings are authenticated for one transport epoch only. Keeping
	// one across a host migration would accept packets whose key/roster context
	// is no longer current, so peers safely fall back to relay until they probe
	// again.
	OnlineDirectPeerAddresses.clear();
	OnlineDirectPeerLastReceived.clear();
	OnlineHostDirectCandidate = {};
	OnlineHostDirectCandidateValid = false;
	OnlineHostDirectPublicKey.clear();
	OnlinePendingDirectProbes.clear();
	OnlineNextDirectProbe = {};
	OnlineDirectProbeAttempts = 0;
	OnlineSessionActive = true;
	OnlineLastHostPacket = std::chrono::steady_clock::now();
	OnlineNextRecoveryAttempt = {};
	OnlineNextLivenessHeartbeat = std::chrono::steady_clock::now();
	return true;
}

sockaddr_in OnlineAddressForPeer(const BdOnline::FOnlineRelayTransport::FPeerId& peer)
{
	auto found = OnlinePeerAddresses.find(peer);
	if (found != OnlinePeerAddresses.end())
		return found->second;
	if (NextOnlineAddress >= 0x00fffffeu)
		I_FatalError("Online session has too many relay peers");
	sockaddr_in address = {};
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(0xfd000000u | NextOnlineAddress);
	address.sin_port = htons(static_cast<u_short>(NextOnlineAddress));
	OnlineAddressPeers.emplace(OnlineAddressKey(address), peer);
	OnlinePeerAddresses.emplace(peer, address);
	++NextOnlineAddress;
	return address;
}

bool OnlinePeerForAddress(const sockaddr_in& address, BdOnline::FOnlineRelayTransport::FPeerId& peer)
{
	auto found = OnlineAddressPeers.find(OnlineAddressKey(address));
	if (found == OnlineAddressPeers.end())
		return false;
	peer = found->second;
	return true;
}

bool SendOnlineRelayPacket(const BdOnline::FOnlineRelayTransport::FPeerId& target, const uint8_t* bytes, size_t size, std::string& error)
{
	std::vector<uint8_t> wire;
	if (!OnlineRelayTransport.Encapsulate(target, bytes, size, wire, error))
		return false;
	if (sendto(MySocket, reinterpret_cast<const char*>(wire.data()), static_cast<int>(wire.size()), 0,
		reinterpret_cast<const sockaddr*>(&OnlineRelayAddress), sizeof(OnlineRelayAddress)) == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK)
	{
		error = std::string("could not send online relay packet: ") + neterror();
		return false;
	}
	return true;
}

bool SendOnlinePacket(const sockaddr_in& to, const uint8_t* bytes, size_t size, std::string& error)
{
	BdOnline::FOnlineRelayTransport::FPeerId target = {};
	if (!OnlinePeerForAddress(to, target))
	{
		error = "online packet has no relay peer route";
		return false;
	}
	std::vector<uint8_t> wire;
	auto direct = OnlineDirectPeerAddresses.find(target);
	if (direct != OnlineDirectPeerAddresses.end())
	{
		const auto received = OnlineDirectPeerLastReceived.find(target);
		if (received == OnlineDirectPeerLastReceived.end() ||
			std::chrono::steady_clock::now() - received->second > OnlineDirectRouteSilenceTimeout)
		{
			// NAT mappings can expire silently. A direct route without a recent
			// authenticated reply is no longer trustworthy as a fast path.
			OnlineDirectPeerLastReceived.erase(target);
			OnlineDirectPeerAddresses.erase(direct);
			direct = OnlineDirectPeerAddresses.end();
		}
	}
	if (direct != OnlineDirectPeerAddresses.end())
	{
		std::string directError;
		if (OnlineRelayTransport.EncapsulateDirect(target, bytes, size, wire, directError) &&
			sendto(MySocket, reinterpret_cast<const char*>(wire.data()), static_cast<int>(wire.size()), 0,
				reinterpret_cast<const sockaddr*>(&direct->second), sizeof(direct->second)) != SOCKET_ERROR)
		{
			return true;
		}
		// A NAT mapping can disappear without warning. Do not turn that into a
		// disconnect: discard this optimization and resend through the relay.
		OnlineDirectPeerLastReceived.erase(target);
		OnlineDirectPeerAddresses.erase(direct);
	}
	return SendOnlineRelayPacket(target, bytes, size, error);
}

constexpr uint8_t OnlineContentMagic[] = { 'B', 'D', 'C', 'T' };
constexpr uint8_t OnlineContentRequest = 1;
constexpr uint8_t OnlineContentChunk = 2;
constexpr size_t OnlineContentChunkBytes = 7000;
constexpr uint8_t OnlineMigrationMagic[] = { 'B', 'D', 'M', 'G' };
constexpr uint8_t OnlineMigrationVersion = 1;
constexpr uint8_t OnlineRelayEnvelopeMagic[] = { 'B', 'D', 'O', 'R' };
constexpr uint8_t OnlineLivenessMagic[] = { 'B', 'D', 'L', 'V' };
// Guests keep their relay admission alive with an authenticated opaque packet
// addressed only to the host. The relay observes its already-validated sender
// label; the game consumes this marker before the legacy packet parser sees it.
constexpr uint8_t OnlineGuestLivenessMagic[] = { 'B', 'D', 'G', 'V' };
constexpr uint8_t OnlineDirectProbeMagic[] = { 'B', 'D', 'D', 'P' };
constexpr uint8_t OnlineDirectProbeVersion = 1;
constexpr uint8_t OnlineDirectProbeRequest = 1;
constexpr uint8_t OnlineDirectProbeAcknowledgement = 2;
constexpr size_t OnlineDirectProbeSignatureBytes = 64;
constexpr size_t OnlineDirectProbeBytes = sizeof(OnlineDirectProbeMagic) + 2 + 8 + OnlineDirectProbeSignatureBytes;
constexpr auto OnlineDirectProbeInterval = std::chrono::seconds(2);
constexpr auto OnlineDirectProbeLifetime = std::chrono::seconds(30);
constexpr unsigned OnlineMaxDirectProbeAttempts = 10;

void OnlineWriteU16(std::vector<uint8_t>& output, uint16_t value)
{
	output.push_back(static_cast<uint8_t>(value));
	output.push_back(static_cast<uint8_t>(value >> 8));
}

void OnlineWriteU32(std::vector<uint8_t>& output, uint32_t value)
{
	for (size_t index = 0; index < 4; ++index)
		output.push_back(static_cast<uint8_t>(value >> (index * 8)));
}

void OnlineWriteU64(std::vector<uint8_t>& output, uint64_t value)
{
	for (size_t index = 0; index < 8; ++index)
		output.push_back(static_cast<uint8_t>(value >> (index * 8)));
}

uint16_t OnlineReadU16(const uint8_t* input)
{
	return static_cast<uint16_t>(input[0]) | static_cast<uint16_t>(input[1]) << 8;
}

uint32_t OnlineReadU32(const uint8_t* input)
{
	uint32_t value = 0;
	for (size_t index = 0; index < 4; ++index)
		value |= static_cast<uint32_t>(input[index]) << (index * 8);
	return value;
}

uint64_t OnlineReadU64(const uint8_t* input)
{
	uint64_t value = 0;
	for (size_t index = 0; index < 8; ++index)
		value |= static_cast<uint64_t>(input[index]) << (index * 8);
	return value;
}

bool OnlineAddressesEqual(const sockaddr_in& left, const sockaddr_in& right)
{
	return left.sin_family == right.sin_family && left.sin_addr.s_addr == right.sin_addr.s_addr && left.sin_port == right.sin_port;
}

void OnlineBuildDirectProofMessage(const BdOnline::FOnlineRelayTransport::FPeerId& sender,
	const BdOnline::FOnlineRelayTransport::FPeerId& target, uint8_t type, uint64_t timestamp, std::vector<uint8_t>& message)
{
	message.clear();
	message.reserve(16 + sizeof(OnlineDirectProbeMagic) + 2 + 4 + sender.size() + target.size() + 8);
	std::array<uint8_t, 16> relayToken = {};
	if (!OnlineDecodeHex(OnlineSession.RelayToken, relayToken.data(), relayToken.size()))
		return;
	message.insert(message.end(), relayToken.begin(), relayToken.end());
	message.insert(message.end(), std::begin(OnlineDirectProbeMagic), std::end(OnlineDirectProbeMagic));
	message.push_back(OnlineDirectProbeVersion);
	message.push_back(type);
	OnlineWriteU32(message, OnlineSession.Epoch);
	message.insert(message.end(), sender.begin(), sender.end());
	message.insert(message.end(), target.begin(), target.end());
	OnlineWriteU64(message, timestamp);
}

bool OnlineSendDirectProof(const BdOnline::FOnlineRelayTransport::FPeerId& target, const sockaddr_in& endpoint,
	uint8_t type, std::string& error)
{
	if (MySocket == INVALID_SOCKET || (type != OnlineDirectProbeRequest && type != OnlineDirectProbeAcknowledgement))
	{
		error = "online direct route is unavailable";
		return false;
	}
	BdOnline::FOnlineRelayTransport::FPeerId local = {};
	if (!BdOnline::FOnlineRelayTransport::ParsePeerId(OnlineSession.LocalPeerId, local))
	{
		error = "online direct route has an invalid local identity";
		return false;
	}
	const auto timestamp = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
		std::chrono::system_clock::now().time_since_epoch()).count());
	std::vector<uint8_t> signedMessage;
	OnlineBuildDirectProofMessage(local, target, type, timestamp, signedMessage);
	if (signedMessage.empty())
	{
		error = "online direct route has invalid session credentials";
		return false;
	}
	std::vector<uint8_t> signature;
	if (!OnlineIdentity.SignDetached(signedMessage.data(), signedMessage.size(), signature, error) ||
		signature.size() != OnlineDirectProbeSignatureBytes)
	{
		return false;
	}
	std::vector<uint8_t> proof;
	proof.reserve(OnlineDirectProbeBytes);
	proof.insert(proof.end(), std::begin(OnlineDirectProbeMagic), std::end(OnlineDirectProbeMagic));
	proof.push_back(OnlineDirectProbeVersion);
	proof.push_back(type);
	OnlineWriteU64(proof, timestamp);
	proof.insert(proof.end(), signature.begin(), signature.end());
	std::vector<uint8_t> wire;
	if (!OnlineRelayTransport.EncapsulateDirect(target, proof.data(), proof.size(), wire, error))
		return false;
	if (sendto(MySocket, reinterpret_cast<const char*>(wire.data()), static_cast<int>(wire.size()), 0,
		reinterpret_cast<const sockaddr*>(&endpoint), sizeof(endpoint)) == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK)
	{
		error = std::string("could not send online direct route probe: ") + neterror();
		return false;
	}
	return true;
}

bool OnlineDecodeDirectProof(const BdOnline::FOnlineRelayTransport::FPeerId& sender,
	const BdOnline::FOnlineRelayTransport::FPeerId& target, const std::vector<uint8_t>& packet,
	uint8_t& type, uint64_t& timestamp, const uint8_t*& signature)
{
	if (packet.size() != OnlineDirectProbeBytes ||
		std::memcmp(packet.data(), OnlineDirectProbeMagic, sizeof(OnlineDirectProbeMagic)) != 0 ||
		packet[sizeof(OnlineDirectProbeMagic)] != OnlineDirectProbeVersion)
	{
		return false;
	}
	type = packet[sizeof(OnlineDirectProbeMagic) + 1];
	if (type != OnlineDirectProbeRequest && type != OnlineDirectProbeAcknowledgement)
		return false;
	timestamp = OnlineReadU64(packet.data() + sizeof(OnlineDirectProbeMagic) + 2);
	const auto now = std::chrono::system_clock::now();
	const auto sent = std::chrono::system_clock::time_point(std::chrono::seconds(timestamp));
	if (sent < now - OnlineDirectProbeLifetime || sent > now + OnlineDirectProbeLifetime)
		return false;
	signature = packet.data() + sizeof(OnlineDirectProbeMagic) + 2 + 8;
	return true;
}

bool OnlineVerifyDirectProof(const BdOnline::FOnlineRelayTransport::FPeerId& sender,
	const BdOnline::FOnlineRelayTransport::FPeerId& target, uint8_t type, uint64_t timestamp,
	const uint8_t* signature, const std::string& publicKey, std::string& error)
{
	std::vector<uint8_t> signedMessage;
	OnlineBuildDirectProofMessage(sender, target, type, timestamp, signedMessage);
	return !signedMessage.empty() && BdOnline::FOnlineDeviceIdentity::VerifyDetached(publicKey,
		signedMessage.data(), signedMessage.size(), signature, OnlineDirectProbeSignatureBytes, error);
}

// Handles only the signed direct-route handshake. Ordinary game traffic is
// accepted on a direct UDP source only after this routine (and, on the host,
// a directory admission lookup) has installed a binding for that peer.
bool OnlineHandleDirectProof(const BdOnline::FOnlineRelayTransport::FPeerId& sender, const sockaddr_in& source,
	const std::vector<uint8_t>& packet)
{
	BdOnline::FOnlineRelayTransport::FPeerId local = {};
	uint8_t type = 0;
	uint64_t timestamp = 0;
	const uint8_t* signature = nullptr;
	if (!BdOnline::FOnlineRelayTransport::ParsePeerId(OnlineSession.LocalPeerId, local) ||
		!OnlineDecodeDirectProof(sender, local, packet, type, timestamp, signature))
	{
		return false;
	}
	if (type == OnlineDirectProbeAcknowledgement)
	{
		BdOnline::FOnlineRelayTransport::FPeerId host = {};
		std::string error;
		if (OnlineSessionIsHost || !OnlineHostDirectCandidateValid || OnlineHostDirectPublicKey.empty() ||
			!BdOnline::FOnlineRelayTransport::ParsePeerId(OnlineSession.HostPeerId, host) || sender != host ||
			!OnlineAddressesEqual(source, OnlineHostDirectCandidate) ||
			!OnlineVerifyDirectProof(sender, local, type, timestamp, signature, OnlineHostDirectPublicKey, error))
		{
			return false;
		}
		const bool first = OnlineDirectPeerAddresses.emplace(sender, source).second;
		if (!first)
			OnlineDirectPeerAddresses[sender] = source;
		OnlineDirectPeerLastReceived[sender] = std::chrono::steady_clock::now();
		if (first)
			Printf("Online direct route established; encrypted relay remains available as fallback.\n");
		return true;
	}
	if (!OnlineSessionIsHost || OnlinePendingDirectProbes.size() >= OnlineMaxPendingDirectProbes)
		return false;
	const auto established = OnlineDirectPeerAddresses.find(sender);
	if (established != OnlineDirectPeerAddresses.end() && OnlineAddressesEqual(established->second, source))
	{
		// The acknowledgement can be lost independently of a regular game
		// packet. Once the source was identity-checked, reply to a retry without
		// another control-plane round trip or admitting a new route.
		std::string error;
		if (!OnlineSendDirectProof(sender, source, OnlineDirectProbeAcknowledgement, error))
			DPrintf(DMSG_NOTIFY, "Online direct route acknowledgement failed: %s\n", error.c_str());
		return true;
	}
	for (const auto& pending : OnlinePendingDirectProbes)
	{
		if (pending.Sender == sender && OnlineAddressesEqual(pending.Source, source) && pending.Timestamp == timestamp)
			return true;
	}
	FOnlinePendingDirectProbe pending;
	pending.Sender = sender;
	pending.Source = source;
	pending.Timestamp = timestamp;
	std::copy_n(signature, pending.Signature.size(), pending.Signature.begin());
	OnlinePendingDirectProbes.push_back(std::move(pending));
	return true;
}

void OnlinePumpDirectProbes()
{
	if (!OnlineSessionActive)
		return;
	const auto now = std::chrono::steady_clock::now();
	if (!OnlineSessionIsHost && MySocket != INVALID_SOCKET && OnlineHostDirectCandidateValid && OnlineDirectProbeAttempts < OnlineMaxDirectProbeAttempts &&
		now >= OnlineNextDirectProbe)
	{
		BdOnline::FOnlineRelayTransport::FPeerId host = {};
		std::string error;
		if (BdOnline::FOnlineRelayTransport::ParsePeerId(OnlineSession.HostPeerId, host) &&
			OnlineDirectPeerAddresses.find(host) == OnlineDirectPeerAddresses.end() &&
			!OnlineSendDirectProof(host, OnlineHostDirectCandidate, OnlineDirectProbeRequest, error))
		{
			DPrintf(DMSG_NOTIFY, "Online direct route probe failed: %s\n", error.c_str());
		}
		++OnlineDirectProbeAttempts;
		OnlineNextDirectProbe = now + OnlineDirectProbeInterval;
	}
	if (!OnlineSessionIsHost || OnlinePendingDirectProbes.empty() || !OnlineServiceClient)
		return;
	FOnlinePendingDirectProbe pending = std::move(OnlinePendingDirectProbes.front());
	OnlinePendingDirectProbes.pop_front();
	BdOnline::FOnlinePeerAdmission admission;
	std::string error;
	const std::string peerId = OnlineEncodeDigest(pending.Sender.data(), pending.Sender.size());
	if (!OnlineServiceClient->IsPeerAdmitted(OnlineSession, peerId, admission, error) || !admission.Admitted ||
		admission.DirectEndpoint.empty())
	{
		return;
	}
	sockaddr_in observed = {};
	if (!ParseOnlineDirectCandidate(admission.DirectEndpoint, observed, error) || !OnlineAddressesEqual(observed, pending.Source) ||
		!OnlineVerifyDirectProof(pending.Sender, OnlineRelayTransport.LocalPeerId(), OnlineDirectProbeRequest,
			pending.Timestamp, pending.Signature.data(), admission.PublicKey, error))
	{
		return;
	}
	const bool first = OnlineDirectPeerAddresses.emplace(pending.Sender, pending.Source).second;
	if (!first)
		OnlineDirectPeerAddresses[pending.Sender] = pending.Source;
	OnlineDirectPeerLastReceived[pending.Sender] = std::chrono::steady_clock::now();
	if (!OnlineSendDirectProof(pending.Sender, pending.Source, OnlineDirectProbeAcknowledgement, error))
	{
		OnlineDirectPeerLastReceived.erase(pending.Sender);
		OnlineDirectPeerAddresses.erase(pending.Sender);
		return;
	}
	if (first)
		Printf("Online direct route established for a connected player; encrypted relay remains available as fallback.\n");
}

bool OnlineDecodeHex(const std::string& encoded, uint8_t* output, size_t outputSize)
{
	if (output == nullptr || encoded.size() != outputSize * 2)
		return false;
	auto nibble = [](char character) -> int
	{
		if (character >= '0' && character <= '9') return character - '0';
		if (character >= 'a' && character <= 'f') return character - 'a' + 10;
		return -1;
	};
	for (size_t index = 0; index < outputSize; ++index)
	{
		const int high = nibble(encoded[index * 2]);
		const int low = nibble(encoded[index * 2 + 1]);
		if (high < 0 || low < 0)
			return false;
		output[index] = static_cast<uint8_t>((high << 4) | low);
	}
	return true;
}

bool OnlineDecodeDigest(const std::string& digest, std::array<uint8_t, BdOnline::OnlineDigestBytes>& output)
{
	return OnlineDecodeHex(digest, output.data(), output.size());
}

std::string OnlineEncodeDigest(const uint8_t* digest, size_t size)
{
	static constexpr char Hex[] = "0123456789abcdef";
	std::string result;
	result.reserve(size * 2);
	for (size_t index = 0; index < size; ++index)
	{
		result.push_back(Hex[digest[index] >> 4]);
		result.push_back(Hex[digest[index] & 15]);
	}
	return result;
}

void SendOnlineHostLivenessHeartbeat()
{
	if (!OnlineSessionActive || !OnlineSessionIsHost || MySocket == INVALID_SOCKET)
		return;
	const auto now = std::chrono::steady_clock::now();
	if (now < OnlineNextLivenessHeartbeat)
		return;
	OnlineNextLivenessHeartbeat = now + OnlineHostLivenessInterval;

	std::array<uint8_t, 16> token = {};
	BdOnline::FOnlineRelayTransport::FPeerId peer = {};
	if (!OnlineDecodeHex(OnlineSession.RelayToken, token.data(), token.size()) ||
		!BdOnline::FOnlineRelayTransport::ParsePeerId(OnlineSession.LocalPeerId, peer))
	{
		DPrintf(DMSG_NOTIFY, "Online host liveness credentials are invalid.\n");
		return;
	}
	std::vector<uint8_t> signedRecord;
	signedRecord.reserve(token.size() + sizeof(OnlineLivenessMagic) + peer.size() + 8);
	signedRecord.insert(signedRecord.end(), token.begin(), token.end());
	signedRecord.insert(signedRecord.end(), std::begin(OnlineLivenessMagic), std::end(OnlineLivenessMagic));
	signedRecord.insert(signedRecord.end(), peer.begin(), peer.end());
	const auto timestamp = std::chrono::duration_cast<std::chrono::seconds>(
		std::chrono::system_clock::now().time_since_epoch()).count();
	OnlineWriteU64(signedRecord, static_cast<uint64_t>(timestamp));
	std::vector<uint8_t> signature;
	std::string error;
	if (!OnlineIdentity.SignDetached(signedRecord.data(), signedRecord.size(), signature, error))
	{
		DPrintf(DMSG_NOTIFY, "Online host liveness signature failed: %s\n", error.c_str());
		return;
	}
	std::vector<uint8_t> wire;
	wire.reserve(sizeof(OnlineRelayEnvelopeMagic) + signedRecord.size() + signature.size());
	wire.insert(wire.end(), std::begin(OnlineRelayEnvelopeMagic), std::end(OnlineRelayEnvelopeMagic));
	wire.insert(wire.end(), signedRecord.begin(), signedRecord.end());
	wire.insert(wire.end(), signature.begin(), signature.end());
	if (sendto(MySocket, reinterpret_cast<const char*>(wire.data()), static_cast<int>(wire.size()), 0,
		reinterpret_cast<const sockaddr*>(&OnlineRelayAddress), sizeof(OnlineRelayAddress)) == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK)
	{
		DPrintf(DMSG_NOTIFY, "Online host liveness heartbeat failed: %s\n", neterror());
	}
}

void SendOnlineGuestLivenessHeartbeat()
{
	if (!OnlineSessionActive || OnlineSessionIsHost || MySocket == INVALID_SOCKET)
		return;
	const auto now = std::chrono::steady_clock::now();
	if (now < OnlineNextLivenessHeartbeat)
		return;
	OnlineNextLivenessHeartbeat = now + OnlineHostLivenessInterval;

	BdOnline::FOnlineRelayTransport::FPeerId host = {};
	if (!BdOnline::FOnlineRelayTransport::ParsePeerId(OnlineSession.HostPeerId, host))
	{
		DPrintf(DMSG_NOTIFY, "Online guest liveness host identity is invalid.\n");
		return;
	}
	std::string error;
	// Keep the service's reconnect and route-expiry authority alive even after
	// normal game traffic has moved to a direct path. This packet is deliberately
	// relayed so it cannot be mistaken for a direct-route health signal.
	if (!SendOnlineRelayPacket(host, OnlineGuestLivenessMagic, sizeof(OnlineGuestLivenessMagic), error))
		DPrintf(DMSG_NOTIFY, "Online guest liveness heartbeat failed: %s\n", error.c_str());
}

bool OnlinePeerForClient(int client, BdOnline::FOnlineRelayTransport::FPeerId& peer)
{
	if (client < 0 || client >= static_cast<int>(MAXPLAYERS))
		return false;
	if (client == consoleplayer)
		return BdOnline::FOnlineRelayTransport::ParsePeerId(OnlineSession.LocalPeerId, peer);
	return OnlinePeerForAddress(Connected[client].Address, peer);
}

bool ApplyPendingOnlineHostMigration(int /*departingHost*/, int successor)
{
	if (!OnlinePendingHostMigration.Valid || !OnlineSessionActive || successor < 0)
		return false;
	BdOnline::FOnlineRelayTransport::FPeerId successorPeer = {};
	if (!OnlinePeerForClient(successor, successorPeer) ||
		OnlineEncodeDigest(successorPeer.data(), successorPeer.size()) != OnlinePendingHostMigration.HostPeerId)
	{
		DPrintf(DMSG_NOTIFY, "Online host migration identity did not match the legacy handoff.\n");
		OnlinePendingHostMigration = {};
		return false;
	}
	if (OnlinePendingHostMigration.Epoch <= OnlineSession.Epoch)
	{
		DPrintf(DMSG_NOTIFY, "Online host migration carried a stale epoch.\n");
		OnlinePendingHostMigration = {};
		return false;
	}
	std::string error;
	if (!ConfigureOnlineRelay(OnlineSession.Endpoint, OnlineSession.SessionKey, OnlineSession.RelayToken,
		OnlineSession.LocalPeerId, OnlineSession.RelayAuth, OnlinePendingHostMigration.Epoch, error, true))
	{
		OnlineSession.Fail("could not apply online host migration: " + error);
		DPrintf(DMSG_NOTIFY, "%s\n", OnlineSession.LastError.c_str());
		OnlinePendingHostMigration = {};
		return false;
	}
	// A snapshot is bound to the previous relay epoch. It cannot survive a host
	// migration even if the same peer is elected, because the guest must never
	// restore an archive authenticated under traffic that the new epoch rejects.
	OnlineSnapshotHandoff.Reset();
	OnlineSnapshotReception.Reset();
	OnlinePendingJoinAdmissions.clear();
	OnlineStartupJoin.Reset();
	OnlineSession.HostPeerId = OnlinePendingHostMigration.HostPeerId;
	OnlineSession.Epoch = OnlinePendingHostMigration.Epoch;
	OnlineSessionIsHost = OnlineSession.LocalPeerId == OnlineSession.HostPeerId;
	if (OnlineSessionIsHost)
	{
		// A promoted guest may have mounted verified cache files rather than the
		// original host paths. Rebuild the source table from its actual mounted
		// resources before it accepts another preflight/content request. This is
		// also a last integrity check: a peer whose live resources no longer
		// match the agreed manifest must not become a new content distributor.
		BdOnline::FSessionManifest localManifest;
		if (!BuildOnlineLocalManifest(localManifest, error) ||
			localManifest.EngineVersion != OnlineSession.Manifest.EngineVersion ||
			localManifest.IWADDigest != OnlineSession.Manifest.IWADDigest ||
			localManifest.Content.RootDigest != OnlineSession.Manifest.Content.RootDigest)
		{
			if (error.empty())
				error = "the elected host no longer has the session's verified content";
			OnlineSession.Fail("could not promote online host: " + error);
			Printf("Online host migration failed: %s\n", OnlineSession.LastError.c_str());
			DPrintf(DMSG_NOTIFY, "%s\n", OnlineSession.LastError.c_str());
			OnlineSessionIsHost = false;
			OnlinePendingHostMigration = {};
			return false;
		}
	}
	OnlineSession.Phase = BdOnline::EOnlineSessionPhase::Connected;
	OnlineNextLeaseRenewal = std::chrono::steady_clock::now() + std::chrono::minutes(30);
	OnlineLastHostPacket = std::chrono::steady_clock::now();
	OnlineNextRecoveryAttempt = {};
	OnlineNextLivenessHeartbeat = std::chrono::steady_clock::now();
	OnlinePendingHostMigration = {};
	Printf("Online host migration complete: player %d is the new host (epoch %u).\n", successor + 1, OnlineSession.Epoch);
	return true;
}

bool OnlineHandleMigrationPacket(const BdOnline::FOnlineRelayTransport::FPeerId& sender, const std::vector<uint8_t>& packet)
{
	constexpr size_t migrationSize = 4 + 1 + 4 + BdOnline::FOnlineRelayTransport::FPeerId{}.size();
	if (packet.size() < 4 || std::memcmp(packet.data(), OnlineMigrationMagic, sizeof(OnlineMigrationMagic)) != 0)
		return false;
	if (packet.size() != migrationSize || packet[4] != OnlineMigrationVersion)
		return true;
	BdOnline::FOnlineRelayTransport::FPeerId expectedHost = {};
	if (!BdOnline::FOnlineRelayTransport::ParsePeerId(OnlineSession.HostPeerId, expectedHost) || sender != expectedHost)
		return true;
	const uint32_t epoch = OnlineReadU32(packet.data() + 5);
	const std::string nextHost = OnlineEncodeDigest(packet.data() + 9, expectedHost.size());
	if (epoch <= OnlineSession.Epoch || nextHost == OnlineSession.HostPeerId)
		return true;
	OnlinePendingHostMigration = { true, epoch, OnlineSession.HostPeerId, nextHost };
	// Relay and legacy UDP packets can be reordered. If the legacy exit was
	// processed first, promote immediately once this authenticated migration
	// record finally arrives.
	BdOnline::FOnlineRelayTransport::FPeerId currentHost = {};
	if (OnlinePeerForClient(Net_Arbitrator, currentHost) &&
		OnlineEncodeDigest(currentHost.data(), currentHost.size()) == nextHost)
	{
		ApplyPendingOnlineHostMigration(-1, Net_Arbitrator);
	}
	return true;
}

void MaybeRecoverOnlineHost()
{
	if (!OnlineSessionActive || OnlineSessionIsHost || !OnlineServiceClient || !netgame ||
		NetworkClients.Size() <= 1 || Net_Arbitrator < 0 || Net_Arbitrator >= static_cast<int>(MAXPLAYERS))
	{
		return;
	}
	const auto now = std::chrono::steady_clock::now();
	if (now < OnlineNextRecoveryAttempt || now - OnlineLastHostPacket < OnlineHostSilenceTimeout)
		return;
	OnlineNextRecoveryAttempt = now + std::chrono::seconds(3);

	std::vector<std::string> eligible;
	for (const auto client : NetworkClients)
	{
		if (client == Net_Arbitrator)
			continue;
		BdOnline::FOnlineRelayTransport::FPeerId peer = {};
		if (!OnlinePeerForClient(client, peer))
		{
			DPrintf(DMSG_NOTIFY, "Online host recovery could not identify a connected relay peer.\n");
			return;
		}
		eligible.push_back(OnlineEncodeDigest(peer.data(), peer.size()));
	}
	if (eligible.empty())
		return;
	std::sort(eligible.begin(), eligible.end());

	BdOnline::FOnlineHostMigration recovery;
	std::string error;
	if (!OnlineServiceClient->RecoverSession(OnlineSession, eligible, recovery, error))
	{
		DPrintf(DMSG_NOTIFY, "Online host recovery retry failed: %s\n", error.c_str());
		return;
	}
	if (recovery.Epoch == OnlineSession.Epoch && recovery.HostPeerId == OnlineSession.HostPeerId)
	{
		// The directory still sees authenticated host traffic. Keep waiting for
		// the relay stream rather than allowing one impaired guest to fork it.
		return;
	}
	if (recovery.Epoch <= OnlineSession.Epoch || recovery.HostPeerId == OnlineSession.HostPeerId)
	{
		DPrintf(DMSG_NOTIFY, "Online service returned an inconsistent host recovery decision.\n");
		return;
	}
	int successor = -1;
	for (const auto client : NetworkClients)
	{
		if (client == Net_Arbitrator)
			continue;
		BdOnline::FOnlineRelayTransport::FPeerId peer = {};
		if (OnlinePeerForClient(client, peer) && OnlineEncodeDigest(peer.data(), peer.size()) == recovery.HostPeerId)
		{
			successor = client;
			break;
		}
	}
	if (successor < 0)
	{
		DPrintf(DMSG_NOTIFY, "Online host recovery selected a peer that is no longer in the legacy roster.\n");
		return;
	}

	OnlinePendingHostMigration = { true, recovery.Epoch, OnlineSession.HostPeerId, recovery.HostPeerId };
	OnlineSession.Phase = BdOnline::EOnlineSessionPhase::RecoveringHost;
	Printf(TEXTCOLOR_YELLOW "Online host stopped responding; recovering the session with player %d.\n" TEXTCOLOR_NORMAL, successor + 1);
	Net_OnlineHostRecovered(successor);
}

// This is called only after AEAD validation. The host reads a bounded chunk
// from its resolved launch plan and sends it back over the same encrypted
// relay. Neither the directory service nor the relay retains mod bytes.
bool OnlineHandleContentPacket(const BdOnline::FOnlineRelayTransport::FPeerId& sender, const std::vector<uint8_t>& packet)
{
	if (packet.size() < 5 || std::memcmp(packet.data(), OnlineContentMagic, sizeof(OnlineContentMagic)) != 0)
		return false;
	if (packet[4] != OnlineContentRequest || packet.size() != 4 + 1 + BdOnline::OnlineDigestBytes + 8 + 2)
		return true;
	const auto digest = OnlineEncodeDigest(packet.data() + 5, BdOnline::OnlineDigestBytes);
	const auto source = OnlineContentSources.find(digest);
	if (source == OnlineContentSources.end())
		return true;
	const uint64_t offset = OnlineReadU64(packet.data() + 5 + BdOnline::OnlineDigestBytes);
	const uint16_t requested = OnlineReadU16(packet.data() + 5 + BdOnline::OnlineDigestBytes + 8);
	if (offset > source->second.Size || requested == 0)
		return true;
	const size_t count = static_cast<size_t>(std::min<uint64_t>(source->second.Size - offset,
		std::min<size_t>(requested, OnlineContentChunkBytes)));
	std::vector<uint8_t> frame;
	frame.reserve(4 + 1 + BdOnline::OnlineDigestBytes + 8 + 1 + 2 + count);
	frame.insert(frame.end(), std::begin(OnlineContentMagic), std::end(OnlineContentMagic));
	frame.push_back(OnlineContentChunk);
	frame.insert(frame.end(), packet.begin() + 5, packet.begin() + 5 + BdOnline::OnlineDigestBytes);
	OnlineWriteU64(frame, offset);
	frame.push_back(offset + count == source->second.Size ? 1 : 0);
	OnlineWriteU16(frame, static_cast<uint16_t>(count));
	if (count != 0)
	{
		std::ifstream input(source->second.Path, std::ios::binary);
		if (!input || offset > static_cast<uint64_t>(std::numeric_limits<std::streamoff>::max()))
			return true;
		input.seekg(static_cast<std::streamoff>(offset));
		const auto dataStart = frame.size();
		frame.resize(dataStart + count);
		input.read(reinterpret_cast<char*>(frame.data() + dataStart), static_cast<std::streamsize>(count));
		if (input.gcount() != static_cast<std::streamsize>(count))
			return true;
	}
	std::string error;
	const auto address = OnlineAddressForPeer(sender);
	if (!SendOnlinePacket(address, frame.data(), frame.size(), error))
		DPrintf(DMSG_NOTIFY, "Online content relay reply failed: %s\n", error.c_str());
	return true;
}

bool OnlineFlushSnapshotAcknowledgement(const BdOnline::FOnlineRelayTransport::FPeerId& host)
{
	BdOnline::FOnlineSnapshotAckFrame acknowledgement;
	std::string error;
	if (!OnlineSnapshotReception.Receiver.NextAcknowledgement(acknowledgement, error))
		return error == "online snapshot receiver has no acknowledgement to send";
	std::vector<uint8_t> frame;
	if (!BdOnline::OnlineEncodeSnapshotAck(acknowledgement, frame, error))
	{
		DPrintf(DMSG_NOTIFY, "Online snapshot acknowledgement was rejected: %s\n", error.c_str());
		return false;
	}
	if (!SendOnlinePacket(OnlineAddressForPeer(host), frame.data(), frame.size(), error))
	{
		DPrintf(DMSG_NOTIFY, "Online snapshot acknowledgement could not be relayed: %s\n", error.c_str());
		return false;
	}
	return true;
}

bool OnlineFlushSnapshotReplayAcknowledgement(const BdOnline::FOnlineRelayTransport::FPeerId& host)
{
	uint64_t highestTic = 0;
	if (!OnlineSnapshotReception.Replay.GetHighestContiguousTic(highestTic))
		return true;
	BdOnline::FOnlineSnapshotReplayAckFrame acknowledgement;
	acknowledgement.Epoch = OnlineSnapshotReception.Metadata.Epoch;
	acknowledgement.SnapshotTic = OnlineSnapshotReception.Metadata.SnapshotTic;
	acknowledgement.TransferId = OnlineSnapshotReception.Metadata.TransferId;
	acknowledgement.AcknowledgedTic = highestTic;
	acknowledgement.Digest = OnlineSnapshotReception.Metadata.Digest;
	std::vector<uint8_t> frame;
	std::string error;
	if (!BdOnline::OnlineEncodeSnapshotReplayAck(acknowledgement, frame, error))
	{
		DPrintf(DMSG_NOTIFY, "Online snapshot replay acknowledgement was rejected: %s\n", error.c_str());
		return false;
	}
	if (!SendOnlinePacket(OnlineAddressForPeer(host), frame.data(), frame.size(), error))
	{
		DPrintf(DMSG_NOTIFY, "Online snapshot replay acknowledgement could not be relayed: %s\n", error.c_str());
		return false;
	}
	return true;
}

// A completion fence is meaningful only if every replay record through its
// declared live tic is present. Do not turn receipt of an unreliable fence
// packet into an admission signal: the host waits for this separate, encrypted
// acknowledgement before it may release its bounded handoff state.
bool OnlineFlushSnapshotCompletionAcknowledgement(const BdOnline::FOnlineRelayTransport::FPeerId& host)
{
	// The archive and replay are only transport-complete at this point. The
	// guest must first restore and execute them in its isolated startup phase;
	// otherwise a host could schedule a player that merely downloaded bytes.
	if (OnlineStartupJoin.Active && !OnlineStartupJoin.Restored)
		return true;
	if (!OnlineSnapshotReception.CompletionFenceReceived || !OnlineSnapshotReception.Receiver.IsComplete())
		return true;
	uint64_t highestTic = 0;
	if (!OnlineSnapshotReception.Replay.GetHighestContiguousTic(highestTic) ||
		highestTic < OnlineSnapshotReception.LiveTic)
	{
		return true;
	}
	BdOnline::FOnlineSnapshotCompleteAckFrame acknowledgement;
	acknowledgement.Epoch = OnlineSnapshotReception.Metadata.Epoch;
	acknowledgement.SnapshotTic = OnlineSnapshotReception.Metadata.SnapshotTic;
	acknowledgement.TransferId = OnlineSnapshotReception.Metadata.TransferId;
	acknowledgement.LiveTic = OnlineSnapshotReception.LiveTic;
	acknowledgement.Digest = OnlineSnapshotReception.Metadata.Digest;
	std::vector<uint8_t> frame;
	std::string error;
	if (!BdOnline::OnlineEncodeSnapshotCompleteAck(acknowledgement, frame, error))
	{
		DPrintf(DMSG_NOTIFY, "Online snapshot completion acknowledgement was rejected: %s\n", error.c_str());
		return false;
	}
	if (!SendOnlinePacket(OnlineAddressForPeer(host), frame.data(), frame.size(), error))
	{
		DPrintf(DMSG_NOTIFY, "Online snapshot completion acknowledgement could not be relayed: %s\n", error.c_str());
		return false;
	}
	return true;
}

bool OnlineHandoffMatches(const BdOnline::FOnlineRelayTransport::FPeerId& sender, uint32_t epoch,
	uint64_t snapshotTic, uint64_t transferId, const std::string& digest)
{
	return OnlineSnapshotHandoff.IsActive() && sender == OnlineSnapshotHandoff.Target &&
		epoch == OnlineSnapshotHandoff.Metadata.Epoch && snapshotTic == OnlineSnapshotHandoff.Metadata.SnapshotTic &&
		transferId == OnlineSnapshotHandoff.Metadata.TransferId &&
		digest == OnlineSnapshotHandoff.Metadata.Digest;
}

bool OnlinePeerIsInActiveRoster(const BdOnline::FOnlineRelayTransport::FPeerId& peer)
{
	for (const int client : NetworkClients)
	{
		if (client == consoleplayer)
			continue;
		BdOnline::FOnlineRelayTransport::FPeerId candidate = {};
		if (client >= 0 && client < static_cast<int>(MAXPLAYERS) &&
			OnlinePeerForAddress(Connected[client].Address, candidate) && candidate == peer)
		{
			return true;
		}
	}
	return false;
}

const char* OnlineJoinFailureMessage(BdOnline::EOnlineJoinRejectReason reason)
{
	// These strings surface in the startup window rather than only in a debug
	// log. Keep them short, factual, and actionable; relay control codes are
	// not meaningful to a player deciding whether to retry from the hub.
	switch (reason)
	{
	case BdOnline::EOnlineJoinRejectReason::Busy:
		return "The host is synchronizing another player. Please try again in a moment.";
	case BdOnline::EOnlineJoinRejectReason::SessionFull:
		return "This online session is full.";
	case BdOnline::EOnlineJoinRejectReason::InvalidRequest:
		return "Your online admission is no longer valid. Return to Multiplayer and join again.";
	case BdOnline::EOnlineJoinRejectReason::SnapshotUnavailable:
		return "The host cannot safely prepare the current game for joining. Ask the host to try again.";
	default:
		return "The host rejected the online join request.";
	}
}

void OnlineSendJoinReject(const BdOnline::FOnlineRelayTransport::FPeerId& target, uint32_t epoch,
	uint64_t requestId, BdOnline::EOnlineJoinRejectReason reason)
{
	BdOnline::FOnlineJoinRejectFrame reject;
	reject.Epoch = epoch;
	reject.RequestId = requestId;
	reject.Reason = reason;
	std::vector<uint8_t> frame;
	std::string error;
	if (!BdOnline::OnlineEncodeJoinReject(reject, frame, error) ||
		!SendOnlinePacket(OnlineAddressForPeer(target), frame.data(), frame.size(), error))
	{
		DPrintf(DMSG_NOTIFY, "Online join rejection could not be relayed: %s\n", error.c_str());
	}
}

void OnlineSendJoinAccept(const BdOnline::FOnlineRelayTransport::FPeerId& target,
	const FOnlineSnapshotHandoff& handoff)
{
	if (handoff.RequestId == 0 || handoff.PlayerSlot < 0 || handoff.PlayerSlot >= static_cast<int>(MAXPLAYERS))
		return;
	BdOnline::FOnlineJoinAcceptFrame accept;
	accept.Epoch = handoff.Metadata.Epoch;
	accept.RequestId = handoff.RequestId;
	accept.TransferId = handoff.Metadata.TransferId;
	accept.PlayerSlot = static_cast<uint8_t>(handoff.PlayerSlot);
	std::vector<uint8_t> frame;
	std::string error;
	if (!BdOnline::OnlineEncodeJoinAccept(accept, frame, error) ||
		!SendOnlinePacket(OnlineAddressForPeer(target), frame.data(), frame.size(), error))
	{
		DPrintf(DMSG_NOTIFY, "Online join acceptance could not be relayed: %s\n", error.c_str());
	}
}

void OnlineSendJoinActivationConfirmation(const BdOnline::FOnlineRelayTransport::FPeerId& target,
	const BdOnline::FOnlineJoinActivatedFrame& activated)
{
	std::vector<uint8_t> frame;
	std::string error;
	if (!BdOnline::OnlineEncodeJoinActivated(activated, frame, error) ||
		!SendOnlinePacket(OnlineAddressForPeer(target), frame.data(), frame.size(), error))
	{
		DPrintf(DMSG_NOTIFY, "Online join activation confirmation could not be relayed: %s\n", error.c_str());
	}
}

int OnlineFindJoiningSlot()
{
	const int limit = clamp<int>(MaxClients, 1, MAXPLAYERS);
	for (int slot = 0; slot < limit; ++slot)
	{
		if (!NetworkClients.InGame(slot) && Connected[slot].Status == CSTAT_NONE)
			return slot;
	}
	return -1;
}

bool OnlineApplyJoiningUserInfo(int slot, const std::vector<uint8_t>& userInfo)
{
	if (slot < 0 || slot >= static_cast<int>(MAXPLAYERS) || userInfo.empty())
		return false;
	TArrayView<uint8_t> stream(const_cast<uint8_t*>(userInfo.data()), userInfo.size());
	D_ReadUserInfoStrings(slot, stream, false);
	return stream.Size() == 0;
}

void OnlinePumpJoinAdmissions()
{
	if (!OnlineSessionActive || !OnlineSessionIsHost || OnlinePendingJoinAdmissions.empty())
		return;

	// Process at most one admission per network pump. The directory check is
	// deliberately outside ReceiveOnlinePacket(), but it remains bounded and
	// cannot starve command/relay processing under a burst of requests.
	FOnlinePendingJoinAdmission admission = std::move(OnlinePendingJoinAdmissions.front());
	OnlinePendingJoinAdmissions.pop_front();
	if (admission.Request.Epoch != OnlineSession.Epoch ||
		OnlinePeerIsInActiveRoster(admission.Sender))
	{
		OnlineSendJoinReject(admission.Sender, admission.Request.Epoch, admission.Request.RequestId,
			BdOnline::EOnlineJoinRejectReason::InvalidRequest);
		return;
	}
	if (OnlineSnapshotHandoff.IsActive())
	{
		if (admission.Sender == OnlineSnapshotHandoff.Target &&
			admission.Request.RequestId == OnlineSnapshotHandoff.RequestId)
		{
			// A UDP retry is expected. Resending the same acceptance is safe only
			// because it repeats the original fresh transfer identity and slot.
			OnlineSendJoinAccept(admission.Sender, OnlineSnapshotHandoff);
		}
		else
		{
			OnlineSendJoinReject(admission.Sender, admission.Request.Epoch, admission.Request.RequestId,
				BdOnline::EOnlineJoinRejectReason::Busy);
		}
		return;
	}
	if (!OnlineServiceClient)
	{
		OnlineSendJoinReject(admission.Sender, admission.Request.Epoch, admission.Request.RequestId,
			BdOnline::EOnlineJoinRejectReason::SnapshotUnavailable);
		return;
	}

	BdOnline::FOnlinePeerAdmission peerAdmission;
	std::string error;
	const std::string peerId = OnlineEncodeDigest(admission.Sender.data(), admission.Sender.size());
	if (!OnlineServiceClient->IsPeerAdmitted(OnlineSession, peerId, peerAdmission, error))
	{
		DPrintf(DMSG_NOTIFY, "Online join admission check failed: %s\n", error.c_str());
		OnlineSendJoinReject(admission.Sender, admission.Request.Epoch, admission.Request.RequestId,
			BdOnline::EOnlineJoinRejectReason::SnapshotUnavailable);
		return;
	}
	if (!peerAdmission.Admitted)
	{
		OnlineSendJoinReject(admission.Sender, admission.Request.Epoch, admission.Request.RequestId,
			BdOnline::EOnlineJoinRejectReason::InvalidRequest);
		return;
	}
	const int slot = OnlineFindJoiningSlot();
	if (slot < 0)
	{
		OnlineSendJoinReject(admission.Sender, admission.Request.Epoch, admission.Request.RequestId,
			BdOnline::EOnlineJoinRejectReason::SessionFull);
		return;
	}
	if (!OnlineApplyJoiningUserInfo(slot, admission.Request.UserInfo))
	{
		OnlineSendJoinReject(admission.Sender, admission.Request.Epoch, admission.Request.RequestId,
			BdOnline::EOnlineJoinRejectReason::InvalidRequest);
		return;
	}
	if (!I_OnlineStartSnapshotHandoff(peerId, error))
	{
		DPrintf(DMSG_NOTIFY, "Online join snapshot could not start: %s\n", error.c_str());
		OnlineSendJoinReject(admission.Sender, admission.Request.Epoch, admission.Request.RequestId,
			BdOnline::EOnlineJoinRejectReason::SnapshotUnavailable);
		return;
	}
	OnlineSnapshotHandoff.RequestId = admission.Request.RequestId;
	OnlineSnapshotHandoff.PlayerSlot = slot;
	OnlineSendJoinAccept(admission.Sender, OnlineSnapshotHandoff);
}

void OnlineAbortSnapshotHandoff(const char* reason,
	BdOnline::EOnlineSnapshotCancelReason cancelReason = BdOnline::EOnlineSnapshotCancelReason::TransferFailed);

bool OnlineQueueAcceptedJoin()
{
	if (!OnlineSnapshotHandoff.IsActive() || !OnlineSnapshotHandoff.CompletionAcknowledged ||
		!OnlineSnapshotHandoff.GuestReady || OnlineSnapshotHandoff.JoinEventQueued ||
		OnlineSnapshotHandoff.PlayerSlot < 0 || OnlineSnapshotHandoff.PlayerSlot >= static_cast<int>(MAXPLAYERS))
	{
		return false;
	}
	const int slot = OnlineSnapshotHandoff.PlayerSlot;
	if (NetworkClients.InGame(slot))
		return false;
	Connected[slot].Address = OnlineAddressForPeer(OnlineSnapshotHandoff.Target);
	Connected[slot].Status = CSTAT_READY;
	if (!Net_QueueOnlinePlayerJoin(slot))
	{
		Connected[slot].Clear();
		return false;
	}
	OnlineSnapshotHandoff.JoinEventQueued = true;
	// Once the roster event is queued the archive is no longer the slow path.
	// Leave a short, explicit activation window instead of holding the only
	// handoff slot forever if the joining process disappears at this boundary.
	OnlineSnapshotHandoff.Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
	Printf("Online joiner is ready; scheduling player %d on the authoritative tic stream.\n", slot + 1);
	return true;
}

bool OnlineHandleJoinPacket(const BdOnline::FOnlineRelayTransport::FPeerId& sender,
	const std::vector<uint8_t>& packet)
{
	if (!BdOnline::OnlineIsJoinFrame(packet.data(), packet.size()))
		return false;

	std::string error;
	const auto type = static_cast<BdOnline::EOnlineJoinFrameType>(packet[5]);
	if (OnlineSessionIsHost)
	{
		if (type == BdOnline::EOnlineJoinFrameType::Ready)
		{
			BdOnline::FOnlineJoinReadyFrame ready;
			if (!BdOnline::OnlineDecodeJoinReady(packet.data(), packet.size(), ready, error) ||
				sender != OnlineSnapshotHandoff.Target || ready.Epoch != OnlineSnapshotHandoff.Metadata.Epoch ||
				ready.RequestId != OnlineSnapshotHandoff.RequestId || ready.TransferId != OnlineSnapshotHandoff.Metadata.TransferId ||
				ready.PlayerSlot != OnlineSnapshotHandoff.PlayerSlot)
			{
				DPrintf(DMSG_NOTIFY, "Online join readiness was rejected: %s\n",
					error.empty() ? "it did not match the active handoff" : error.c_str());
				return true;
			}
			OnlineSnapshotHandoff.GuestReady = true;
			if (OnlineSnapshotHandoff.CompletionAcknowledged && !OnlineSnapshotHandoff.JoinEventQueued &&
				!OnlineQueueAcceptedJoin())
				OnlineAbortSnapshotHandoff("could not schedule the accepted joining player");
			return true;
		}
		if (type == BdOnline::EOnlineJoinFrameType::Activated)
		{
			BdOnline::FOnlineJoinActivatedFrame activated;
			if (!BdOnline::OnlineDecodeJoinActivated(packet.data(), packet.size(), activated, error) ||
				sender != OnlineSnapshotHandoff.Target || !OnlineSnapshotHandoff.JoinEventQueued ||
				activated.Epoch != OnlineSnapshotHandoff.Metadata.Epoch || activated.RequestId != OnlineSnapshotHandoff.RequestId ||
				activated.TransferId != OnlineSnapshotHandoff.Metadata.TransferId || activated.PlayerSlot != OnlineSnapshotHandoff.PlayerSlot)
			{
				DPrintf(DMSG_NOTIFY, "Online join activation acknowledgement was rejected: %s\n",
					error.empty() ? "it did not match the active handoff" : error.c_str());
				return true;
			}
			OnlineSendJoinActivationConfirmation(sender, activated);
			Printf("Online player %d completed join-in-progress activation.\n", OnlineSnapshotHandoff.PlayerSlot + 1);
			OnlineSnapshotHandoff.Reset();
			if (OnlineSession.Phase == BdOnline::EOnlineSessionPhase::SynchronizingWorld)
				OnlineSession.Phase = BdOnline::EOnlineSessionPhase::Connected;
			return true;
		}
		if (type != BdOnline::EOnlineJoinFrameType::Request)
			return true;
		BdOnline::FOnlineJoinRequestFrame request;
		if (!BdOnline::OnlineDecodeJoinRequest(packet.data(), packet.size(), request, error))
		{
			DPrintf(DMSG_NOTIFY, "Online join request was malformed: %s\n", error.c_str());
			return true;
		}
		if (request.Epoch != OnlineSession.Epoch)
		{
			OnlineSendJoinReject(sender, request.Epoch, request.RequestId,
				BdOnline::EOnlineJoinRejectReason::InvalidRequest);
			return true;
		}
		for (const auto& queued : OnlinePendingJoinAdmissions)
		{
			if (queued.Sender == sender && queued.Request.RequestId == request.RequestId)
				return true; // idempotent UDP retry while the directory check is pending.
		}
		if (OnlinePendingJoinAdmissions.size() >= OnlineMaxPendingJoinAdmissions)
		{
			OnlineSendJoinReject(sender, request.Epoch, request.RequestId,
				BdOnline::EOnlineJoinRejectReason::Busy);
			return true;
		}
		OnlinePendingJoinAdmissions.push_back({ sender, std::move(request) });
		return true;
	}

	BdOnline::FOnlineRelayTransport::FPeerId host = {};
	if (!BdOnline::FOnlineRelayTransport::ParsePeerId(OnlineSession.HostPeerId, host) || sender != host)
		return true;
	if (type == BdOnline::EOnlineJoinFrameType::Accept)
	{
		BdOnline::FOnlineJoinAcceptFrame accept;
		if (!OnlineStartupJoin.Active || !BdOnline::OnlineDecodeJoinAccept(packet.data(), packet.size(), accept, error) ||
			accept.Epoch != OnlineSession.Epoch || accept.RequestId != OnlineStartupJoin.Request.RequestId)
		{
			DPrintf(DMSG_NOTIFY, "Online join acceptance was rejected: %s\n",
				error.empty() ? "it did not match the active request" : error.c_str());
			return true;
		}
		if (OnlineStartupJoin.Accepted &&
			(OnlineSnapshotReception.Metadata.TransferId != accept.TransferId || OnlineStartupJoin.PlayerSlot != accept.PlayerSlot))
		{
			DPrintf(DMSG_NOTIFY, "Online join acceptance conflicted with the active handoff.\n");
			return true;
		}
		OnlineStartupJoin.Accepted = true;
		OnlineStartupJoin.PlayerSlot = accept.PlayerSlot;
		OnlineSnapshotReception.Reset();
		// Store only the expected identity until Begin supplies the remaining
		// authenticated snapshot metadata.
		OnlineSnapshotReception.Metadata.Epoch = accept.Epoch;
		OnlineSnapshotReception.Metadata.TransferId = accept.TransferId;
		OnlineSession.Phase = BdOnline::EOnlineSessionPhase::SynchronizingWorld;
		return true;
	}
	if (type == BdOnline::EOnlineJoinFrameType::Reject)
	{
		BdOnline::FOnlineJoinRejectFrame reject;
		if (BdOnline::OnlineDecodeJoinReject(packet.data(), packet.size(), reject, error) &&
			OnlineStartupJoin.Active && reject.Epoch == OnlineSession.Epoch &&
			reject.RequestId == OnlineStartupJoin.Request.RequestId)
		{
			OnlineStartupJoin.Failure = OnlineJoinFailureMessage(reject.Reason);
			DPrintf(DMSG_NOTIFY, "Online join was rejected before state transfer: %s\n",
				OnlineStartupJoin.Failure.c_str());
		}
		return true;
	}
	if (type == BdOnline::EOnlineJoinFrameType::Activated)
	{
		BdOnline::FOnlineJoinActivatedFrame activated;
		if (OnlineStartupJoin.Active && OnlineStartupJoin.Restored && OnlineStartupJoin.Activated &&
			BdOnline::OnlineDecodeJoinActivated(packet.data(), packet.size(), activated, error) &&
			activated.Epoch == OnlineSession.Epoch && activated.RequestId == OnlineStartupJoin.Request.RequestId &&
			activated.TransferId == OnlineSnapshotReception.Metadata.TransferId &&
			activated.PlayerSlot == OnlineStartupJoin.PlayerSlot)
		{
			OnlineStartupJoin.ActivationConfirmed = true;
			OnlineSession.Phase = BdOnline::EOnlineSessionPhase::Connected;
		}
		return true;
	}
	return true;
}

void OnlineAbortSnapshotHandoff(const char* reason,
	BdOnline::EOnlineSnapshotCancelReason cancelReason)
{
	if (!OnlineSnapshotHandoff.IsActive())
		return;
	BdOnline::FOnlineSnapshotCancelFrame cancel;
	cancel.Epoch = OnlineSnapshotHandoff.Metadata.Epoch;
	cancel.SnapshotTic = OnlineSnapshotHandoff.Metadata.SnapshotTic;
	cancel.TransferId = OnlineSnapshotHandoff.Metadata.TransferId;
	cancel.Digest = OnlineSnapshotHandoff.Metadata.Digest;
	cancel.Reason = cancelReason;
	std::vector<uint8_t> frame;
	std::string sendError;
	if (BdOnline::OnlineEncodeSnapshotCancel(cancel, frame, sendError) &&
		!SendOnlinePacket(OnlineAddressForPeer(OnlineSnapshotHandoff.Target), frame.data(), frame.size(), sendError))
	{
		DPrintf(DMSG_NOTIFY, "Online snapshot cancellation could not be relayed: %s\n", sendError.c_str());
	}
	DPrintf(DMSG_NOTIFY, "Online join snapshot handoff was cancelled: %s\n", reason);
	OnlineSnapshotHandoff.Reset();
	if (OnlineSession.Phase == BdOnline::EOnlineSessionPhase::SynchronizingWorld)
		OnlineSession.Phase = BdOnline::EOnlineSessionPhase::Connected;
}

void OnlinePumpSnapshotHandoff()
{
	if (!OnlineSnapshotHandoff.IsActive())
		return;
	const auto now = std::chrono::steady_clock::now();
	if (now >= OnlineSnapshotHandoff.Deadline)
	{
		OnlineAbortSnapshotHandoff(OnlineSnapshotHandoff.JoinEventQueued
			? "the joining player did not confirm activation in time"
			: "the joining player did not complete world synchronization in time");
		return;
	}
	if (now < OnlineSnapshotHandoff.NextFrame)
		return;
	OnlineSnapshotHandoff.NextFrame = now + std::chrono::milliseconds(5);

	std::vector<uint8_t> frame;
	std::string error;
	if (!OnlineSnapshotHandoff.Sender.IsComplete())
	{
		if (!OnlineSnapshotHandoff.Sender.NextFrame(frame, error))
		{
			if (error != "online snapshot sender is awaiting final receiver acknowledgement")
				OnlineAbortSnapshotHandoff(error.c_str());
			return;
		}
		if (!SendOnlinePacket(OnlineAddressForPeer(OnlineSnapshotHandoff.Target), frame.data(), frame.size(), error))
			DPrintf(DMSG_NOTIFY, "Online snapshot archive frame could not be relayed: %s\n", error.c_str());
		return;
	}

	std::vector<BdOnline::FOnlineTicRecord> records;
	try
	{
		records = OnlineTicHistory.RecordsAfter(OnlineSnapshotHandoff.Metadata.SnapshotTic);
	}
	catch (const std::bad_alloc&)
	{
		OnlineAbortSnapshotHandoff("not enough memory to read the replay history");
		return;
	}
	if (!records.empty() && (OnlineSnapshotHandoff.Metadata.SnapshotTic == UINT64_MAX ||
		records.front().Tic != OnlineSnapshotHandoff.Metadata.SnapshotTic + 1))
	{
		OnlineAbortSnapshotHandoff("the required replay history expired before the archive arrived",
			BdOnline::EOnlineSnapshotCancelReason::ReplayExpired);
		return;
	}

	const uint64_t latestTic = records.empty() ? OnlineSnapshotHandoff.Metadata.SnapshotTic : records.back().Tic;
	if (OnlineSnapshotHandoff.HighestReplayAcknowledged < latestTic &&
		(!OnlineSnapshotHandoff.CompletionSent ||
			(OnlineSnapshotHandoff.CompletionAcknowledged && OnlineSnapshotHandoff.GuestReady)))
	{
		const uint64_t wantedTic = OnlineSnapshotHandoff.HighestReplayAcknowledged + 1;
		auto record = std::lower_bound(records.begin(), records.end(), wantedTic,
			[](const BdOnline::FOnlineTicRecord& candidate, uint64_t tic)
			{
				return candidate.Tic < tic;
			});
		if (record == records.end() || record->Tic != wantedTic)
		{
			OnlineAbortSnapshotHandoff("the required replay tic is no longer available",
				BdOnline::EOnlineSnapshotCancelReason::ReplayExpired);
			return;
		}
		BdOnline::FOnlineSnapshotReplayFrame replay;
		replay.Epoch = OnlineSnapshotHandoff.Metadata.Epoch;
		replay.SnapshotTic = OnlineSnapshotHandoff.Metadata.SnapshotTic;
		replay.TransferId = OnlineSnapshotHandoff.Metadata.TransferId;
		replay.Tic = record->Tic;
		replay.Digest = OnlineSnapshotHandoff.Metadata.Digest;
		replay.Bytes = record->Bytes;
		if (!BdOnline::OnlineEncodeSnapshotReplayTic(replay, frame, error))
		{
			OnlineAbortSnapshotHandoff(error.c_str());
			return;
		}
		OnlineSnapshotHandoff.HighestReplaySent = std::max(OnlineSnapshotHandoff.HighestReplaySent, replay.Tic);
		if (!SendOnlinePacket(OnlineAddressForPeer(OnlineSnapshotHandoff.Target), frame.data(), frame.size(), error))
			DPrintf(DMSG_NOTIFY, "Online snapshot replay tic could not be relayed: %s\n", error.c_str());
		return;
	}
	// Once the initial completion fence is acknowledged, the guest has restored
	// through that tic. It now receives only post-ready replay records until it
	// executes the ordinary roster event and returns Activated; never send a
	// second completion fence that could be mistaken for a new restore point.
	if (OnlineSnapshotHandoff.CompletionAcknowledged)
		return;

	if (!OnlineSnapshotHandoff.CompletionSent)
	{
		OnlineSnapshotHandoff.CompletionTic = latestTic;
		OnlineSnapshotHandoff.CompletionSent = true;
	}
	BdOnline::FOnlineSnapshotCompleteFrame complete;
	complete.Epoch = OnlineSnapshotHandoff.Metadata.Epoch;
	complete.SnapshotTic = OnlineSnapshotHandoff.Metadata.SnapshotTic;
	complete.TransferId = OnlineSnapshotHandoff.Metadata.TransferId;
	complete.LiveTic = OnlineSnapshotHandoff.CompletionTic;
	complete.Digest = OnlineSnapshotHandoff.Metadata.Digest;
	if (!BdOnline::OnlineEncodeSnapshotComplete(complete, frame, error))
	{
		OnlineAbortSnapshotHandoff(error.c_str());
		return;
	}
	if (!SendOnlinePacket(OnlineAddressForPeer(OnlineSnapshotHandoff.Target), frame.data(), frame.size(), error))
		DPrintf(DMSG_NOTIFY, "Online snapshot completion fence could not be relayed: %s\n", error.c_str());
}

// Snapshot frames bypass the legacy 8 KiB packet envelope: chunk frames are
// intentionally 12 KiB and are already authenticated by the relay codec.
// Consume every self-identifying snapshot frame before the legacy parser can
// mistake it for a game packet.
bool OnlineHandleSnapshotPacket(const BdOnline::FOnlineRelayTransport::FPeerId& sender,
	const std::vector<uint8_t>& packet)
{
	if (!BdOnline::OnlineIsSnapshotFrame(packet.data(), packet.size()))
		return false;

	BdOnline::FOnlineRelayTransport::FPeerId host = {};
	if (!BdOnline::FOnlineRelayTransport::ParsePeerId(OnlineSession.HostPeerId, host))
		return true;
	const uint8_t type = packet[5];
	std::string error;
	if (OnlineSessionIsHost)
	{
		switch (static_cast<BdOnline::EOnlineSnapshotFrameType>(type))
		{
		case BdOnline::EOnlineSnapshotFrameType::Acknowledge:
		{
			BdOnline::FOnlineSnapshotAckFrame acknowledgement;
			if (!BdOnline::OnlineDecodeSnapshotAck(packet.data(), packet.size(), acknowledgement, error))
			{
				DPrintf(DMSG_NOTIFY, "Online snapshot archive acknowledgement was malformed: %s\n", error.c_str());
				break;
			}
			if (!OnlineHandoffMatches(sender, acknowledgement.Epoch, acknowledgement.SnapshotTic,
				acknowledgement.TransferId, acknowledgement.Digest))
				break; // A late retry after completion is normal over an unreliable relay.
			if (!OnlineSnapshotHandoff.Sender.Acknowledge(acknowledgement, error))
				DPrintf(DMSG_NOTIFY, "Online snapshot archive acknowledgement was rejected: %s\n", error.c_str());
			break;
		}

		case BdOnline::EOnlineSnapshotFrameType::ReplayAcknowledge:
		{
			BdOnline::FOnlineSnapshotReplayAckFrame acknowledgement;
			if (!BdOnline::OnlineDecodeSnapshotReplayAck(packet.data(), packet.size(), acknowledgement, error))
			{
				DPrintf(DMSG_NOTIFY, "Online snapshot replay acknowledgement was malformed: %s\n", error.c_str());
				break;
			}
			if (!OnlineHandoffMatches(sender, acknowledgement.Epoch, acknowledgement.SnapshotTic,
				acknowledgement.TransferId, acknowledgement.Digest))
				break;
			if (acknowledgement.AcknowledgedTic > OnlineSnapshotHandoff.HighestReplaySent)
			{
				DPrintf(DMSG_NOTIFY, "Online snapshot replay acknowledgement named an unsent tic.\n");
				break;
			}
			OnlineSnapshotHandoff.HighestReplayAcknowledged = std::max(
				OnlineSnapshotHandoff.HighestReplayAcknowledged, acknowledgement.AcknowledgedTic);
			break;
		}

		case BdOnline::EOnlineSnapshotFrameType::CompleteAcknowledge:
		{
			BdOnline::FOnlineSnapshotCompleteAckFrame acknowledgement;
			if (!BdOnline::OnlineDecodeSnapshotCompleteAck(packet.data(), packet.size(), acknowledgement, error))
			{
				DPrintf(DMSG_NOTIFY, "Online snapshot completion acknowledgement was malformed: %s\n", error.c_str());
				break;
			}
			if (!OnlineHandoffMatches(sender, acknowledgement.Epoch, acknowledgement.SnapshotTic,
				acknowledgement.TransferId, acknowledgement.Digest))
				break;
			if (!OnlineSnapshotHandoff.CompletionSent || acknowledgement.LiveTic != OnlineSnapshotHandoff.CompletionTic)
			{
				DPrintf(DMSG_NOTIFY, "Online snapshot completion acknowledgement did not match the completion fence.\n");
				break;
			}
			OnlineSnapshotHandoff.CompletionAcknowledged = true;
			// Keep the public diagnostic/manual handoff API backward-compatible:
			// it has no admission request or slot, so a successful completion is
			// its terminal state. Only the join-in-progress coordinator waits for
			// Ready and the roster-event activation acknowledgement.
			if (OnlineSnapshotHandoff.RequestId == 0)
			{
				Printf("Online join snapshot restored through tic %llu.\n",
					static_cast<unsigned long long>(acknowledgement.LiveTic));
				OnlineSnapshotHandoff.Reset();
				if (OnlineSession.Phase == BdOnline::EOnlineSessionPhase::SynchronizingWorld)
					OnlineSession.Phase = BdOnline::EOnlineSessionPhase::Connected;
				break;
			}
			Printf("Online join snapshot restored through tic %llu; awaiting authenticated readiness.\n",
				static_cast<unsigned long long>(acknowledgement.LiveTic));
			if (OnlineSnapshotHandoff.GuestReady && !OnlineSnapshotHandoff.JoinEventQueued &&
				!OnlineQueueAcceptedJoin())
				OnlineAbortSnapshotHandoff("could not schedule the accepted joining player");
			break;
		}

		default:
			break;
		}
		return true;
	}
	if (sender != host)
		return true;

	switch (static_cast<BdOnline::EOnlineSnapshotFrameType>(type))
	{
	case BdOnline::EOnlineSnapshotFrameType::Begin:
	{
		BdOnline::FOnlineSnapshotMetadata metadata;
		size_t chunks = 0;
		if (!BdOnline::OnlineDecodeSnapshotBegin(packet.data(), packet.size(), metadata, chunks, error))
		{
			OnlineSnapshotReception.Reset();
			DPrintf(DMSG_NOTIFY, "Online snapshot begin was rejected: %s\n", error.c_str());
			return true;
		}
		if (!OnlineStartupJoin.Active || !OnlineStartupJoin.Accepted ||
			metadata.Epoch != OnlineSession.Epoch || metadata.Epoch != OnlineSnapshotReception.Metadata.Epoch ||
			metadata.TransferId != OnlineSnapshotReception.Metadata.TransferId)
		{
			DPrintf(DMSG_NOTIFY, "Online snapshot begin was rejected: it did not match the accepted join handoff.\n");
			return true;
		}
		const bool restartReplay = !OnlineSnapshotReception.Matches(metadata) || !OnlineSnapshotReception.Replay.IsActive();
		if (!OnlineSnapshotReception.Receiver.ReceiveBegin(metadata, chunks, error) ||
			(restartReplay && !OnlineSnapshotReception.Replay.Start(metadata, error)))
		{
			OnlineSnapshotReception.Reset();
			DPrintf(DMSG_NOTIFY, "Online snapshot begin was rejected: %s\n", error.c_str());
			return true;
		}
		OnlineSnapshotReception.Metadata = std::move(metadata);
		OnlineSnapshotReception.LiveTic = 0;
		OnlineSnapshotReception.CompletionFenceReceived = false;
		OnlineFlushSnapshotAcknowledgement(host);
		return true;
	}

	case BdOnline::EOnlineSnapshotFrameType::Chunk:
	{
		BdOnline::FOnlineSnapshotChunkFrame chunk;
		if (!BdOnline::OnlineDecodeSnapshotChunk(packet.data(), packet.size(), chunk, error) ||
			!OnlineSnapshotReception.Receiver.ReceiveChunk(chunk, error))
		{
			DPrintf(DMSG_NOTIFY, "Online snapshot chunk was rejected: %s\n", error.c_str());
			return true;
		}
		OnlineFlushSnapshotAcknowledgement(host);
		return true;
	}

	case BdOnline::EOnlineSnapshotFrameType::ReplayTic:
	{
		BdOnline::FOnlineSnapshotReplayFrame replay;
		if (!BdOnline::OnlineDecodeSnapshotReplayTic(packet.data(), packet.size(), replay, error) ||
			!OnlineSnapshotReception.Replay.Accept(replay, error))
		{
			DPrintf(DMSG_NOTIFY, "Online snapshot replay tic was rejected: %s\n", error.c_str());
		}
		else
		{
			OnlineFlushSnapshotReplayAcknowledgement(host);
		}
		return true;
	}

	case BdOnline::EOnlineSnapshotFrameType::Complete:
	{
		BdOnline::FOnlineSnapshotCompleteFrame complete;
		if (!BdOnline::OnlineDecodeSnapshotComplete(packet.data(), packet.size(), complete, error) ||
			!OnlineSnapshotReception.Receiver.IsComplete() ||
			complete.Epoch != OnlineSnapshotReception.Metadata.Epoch ||
			complete.SnapshotTic != OnlineSnapshotReception.Metadata.SnapshotTic ||
			complete.TransferId != OnlineSnapshotReception.Metadata.TransferId ||
			complete.Digest != OnlineSnapshotReception.Metadata.Digest)
		{
			DPrintf(DMSG_NOTIFY, "Online snapshot completion fence was rejected: %s\n",
				error.empty() ? "archive is incomplete or does not match" : error.c_str());
			return true;
		}
		OnlineSnapshotReception.LiveTic = complete.LiveTic;
		OnlineSnapshotReception.CompletionFenceReceived = true;
		OnlineStartupJoin.ReadyForRestore = true;
		return true;
	}

	case BdOnline::EOnlineSnapshotFrameType::Cancel:
	{
		BdOnline::FOnlineSnapshotCancelFrame cancel;
		if (!BdOnline::OnlineDecodeSnapshotCancel(packet.data(), packet.size(), cancel, error) ||
			!OnlineSnapshotReception.MatchesIdentity(cancel.Epoch, cancel.SnapshotTic,
				cancel.TransferId, cancel.Digest))
		{
			DPrintf(DMSG_NOTIFY, "Online snapshot cancellation was rejected: %s\n",
				error.empty() ? "it did not match the active handoff" : error.c_str());
			return true;
		}
		const char* reason = "the host stopped the handoff";
		switch (cancel.Reason)
		{
		case BdOnline::EOnlineSnapshotCancelReason::ReplayExpired:
			reason = "the host could no longer provide the required replay history";
			break;
		case BdOnline::EOnlineSnapshotCancelReason::TransferFailed:
			reason = "the host could not complete the transfer";
			break;
		case BdOnline::EOnlineSnapshotCancelReason::EpochChanged:
			reason = "the host changed during the transfer";
			break;
		default:
			break;
		}
		OnlineSnapshotReception.Reset();
		if (OnlineStartupJoin.Active && !OnlineStartupJoin.Restored)
			OnlineStartupJoin.Failure = std::string("online world synchronization was cancelled: ") + reason;
		DPrintf(DMSG_NOTIFY, "Online snapshot handoff was cancelled: %s.\n", reason);
		return true;
	}

	case BdOnline::EOnlineSnapshotFrameType::Acknowledge:
		// A guest never treats a peer acknowledgement as game traffic. The
		// future sender owns these frames after it creates a handoff state.
		return true;

	default:
		return true;
	}
}

void OnlinePumpSnapshotAcknowledgements()
{
	if (!OnlineSessionActive || OnlineSessionIsHost ||
		(!OnlineSnapshotReception.Receiver.IsActive() && !OnlineSnapshotReception.Receiver.IsComplete()))
	{
		return;
	}
	const auto now = std::chrono::steady_clock::now();
	if (now < OnlineSnapshotReception.NextAcknowledgement)
		return;
	OnlineSnapshotReception.NextAcknowledgement = now + std::chrono::milliseconds(20);

	BdOnline::FOnlineRelayTransport::FPeerId host = {};
	if (BdOnline::FOnlineRelayTransport::ParsePeerId(OnlineSession.HostPeerId, host))
	{
		OnlineFlushSnapshotAcknowledgement(host);
		OnlineFlushSnapshotReplayAcknowledgement(host);
		OnlineFlushSnapshotCompletionAcknowledgement(host);
	}
}

void OnlinePumpStartupJoinControls()
{
	if (!OnlineStartupJoin.Active || !OnlineStartupJoin.Restored || OnlineSessionIsHost)
		return;
	BdOnline::FOnlineRelayTransport::FPeerId host = {};
	if (!BdOnline::FOnlineRelayTransport::ParsePeerId(OnlineSession.HostPeerId, host))
		return;
	const auto now = std::chrono::steady_clock::now();
	std::vector<uint8_t> frame;
	std::string error;
	if (!OnlineStartupJoin.Activated)
	{
		if (now < OnlineStartupJoin.NextReady)
			return;
		BdOnline::FOnlineJoinReadyFrame ready;
		ready.Epoch = OnlineSession.Epoch;
		ready.RequestId = OnlineStartupJoin.Request.RequestId;
		ready.TransferId = OnlineSnapshotReception.Metadata.TransferId;
		ready.PlayerSlot = static_cast<uint8_t>(OnlineStartupJoin.PlayerSlot);
		if (!BdOnline::OnlineEncodeJoinReady(ready, frame, error) ||
			!SendOnlinePacket(OnlineAddressForPeer(host), frame.data(), frame.size(), error))
		{
			DPrintf(DMSG_NOTIFY, "Online join readiness could not be relayed: %s\n", error.c_str());
		}
		OnlineStartupJoin.NextReady = now + std::chrono::milliseconds(500);
		return;
	}
	if (OnlineStartupJoin.ActivationConfirmed)
		return;
	if (now >= OnlineStartupJoin.ActivationConfirmationDeadline)
	{
		// The host processed a valid activation before the local process became
		// live. A missing echo cannot safely roll that transition back; stop the
		// bounded retry loop and let normal liveness/recovery handle a dead host.
		DPrintf(DMSG_NOTIFY, "Online join activation confirmation timed out; continuing with normal liveness checks.\n");
		OnlineStartupJoin.ActivationConfirmed = true;
		OnlineSession.Phase = BdOnline::EOnlineSessionPhase::Connected;
		return;
	}
	if (now < OnlineStartupJoin.NextActivated)
		return;
	BdOnline::FOnlineJoinActivatedFrame activated;
	activated.Epoch = OnlineSession.Epoch;
	activated.RequestId = OnlineStartupJoin.Request.RequestId;
	activated.TransferId = OnlineSnapshotReception.Metadata.TransferId;
	activated.PlayerSlot = static_cast<uint8_t>(OnlineStartupJoin.PlayerSlot);
	if (!BdOnline::OnlineEncodeJoinActivated(activated, frame, error) ||
		!SendOnlinePacket(OnlineAddressForPeer(host), frame.data(), frame.size(), error))
	{
		DPrintf(DMSG_NOTIFY, "Online join activation acknowledgement could not be relayed: %s\n", error.c_str());
	}
	OnlineStartupJoin.NextActivated = now + std::chrono::milliseconds(500);
}

bool ReceiveOnlinePacket(const uint8_t* wire, sockaddr_in& from, int& size, std::string& error)
{
	BdOnline::FOnlineRelayTransport::FPeerId sender = {};
	std::vector<uint8_t> packet;
	const bool directWire = size >= 4 && std::memcmp(wire, "BDOD", 4) == 0;
	if (directWire)
	{
		if (!OnlineRelayTransport.DecapsulateDirect(wire, static_cast<size_t>(size), sender, packet, error))
			return false;
		const auto route = OnlineDirectPeerAddresses.find(sender);
		const bool knownDirectSource = route != OnlineDirectPeerAddresses.end() && OnlineAddressesEqual(route->second, from);
		if (knownDirectSource)
			OnlineDirectPeerLastReceived[sender] = std::chrono::steady_clock::now();
		if (OnlineHandleDirectProof(sender, from, packet))
		{
			size = 0;
			return false;
		}
		if (!knownDirectSource)
		{
			error = "online direct packet arrived before its identity-bound route was established";
			return false;
		}
	}
	else if (!OnlineRelayTransport.Decapsulate(wire, static_cast<size_t>(size), sender, packet, error))
	{
		return false;
	}
	BdOnline::FOnlineRelayTransport::FPeerId host = {};
	if (BdOnline::FOnlineRelayTransport::ParsePeerId(OnlineSession.HostPeerId, host) && sender == host)
		OnlineLastHostPacket = std::chrono::steady_clock::now();
	from = OnlineAddressForPeer(sender);
	if (OnlineHandleContentPacket(sender, packet))
	{
		size = 0;
		return false;
	}
	if (OnlineHandleMigrationPacket(sender, packet))
	{
		size = 0;
		return false;
	}
	if (OnlineHandleJoinPacket(sender, packet))
	{
		size = 0;
		return false;
	}
	if (OnlineHandleSnapshotPacket(sender, packet))
	{
		size = 0;
		return false;
	}
	if (packet.size() == sizeof(OnlineGuestLivenessMagic) &&
		std::memcmp(packet.data(), OnlineGuestLivenessMagic, sizeof(OnlineGuestLivenessMagic)) == 0)
	{
		size = 0;
		return false;
	}
	if (packet.size() > MaxTransmitSize)
	{
		error = "online relay packet exceeds the legacy packet limit";
		return false;
	}
	std::memcpy(TransmitBuffer, packet.data(), packet.size());
	size = static_cast<int>(packet.size());
	return true;
}

} // namespace
#endif

bool I_OnlineStartSnapshotHandoff(const std::string& peerId, std::string& error)
{
#ifdef BIASEDDOOM_ONLINE
	error.clear();
	if (!OnlineSessionActive || !OnlineSessionIsHost || !netgame || NetMode != NET_PacketServer ||
		consoleplayer != Net_Arbitrator || OnlineSession.Epoch == 0)
	{
		error = "only the active online packet-server host can start a snapshot handoff";
		return false;
	}
	if (OnlineSnapshotHandoff.IsActive())
	{
		error = "another online join snapshot is already synchronizing";
		return false;
	}
	BdOnline::FOnlineRelayTransport::FPeerId target = {};
	BdOnline::FOnlineRelayTransport::FPeerId local = {};
	if (!BdOnline::FOnlineRelayTransport::ParsePeerId(peerId, target) ||
		!BdOnline::FOnlineRelayTransport::ParsePeerId(OnlineSession.LocalPeerId, local) || target == local)
	{
		error = "online join handoff has an invalid relay peer identity";
		return false;
	}
	FOnlineNetSnapshotState networkState;
	if (!Net_CaptureOnlineSnapshotState(networkState))
	{
		error = "online snapshot handoff must begin at a packet-server tic boundary";
		return false;
	}
	BdOnline::FOnlineSnapshotBundle bundle;
	if (!G_BuildOnlineSnapshotBundle(bundle, error))
		return false;
	std::vector<uint8_t> bundleBytes;
	if (!BdOnline::OnlineEncodeSnapshotBundle(bundle, bundleBytes, error))
		return false;
	BdOnline::FOnlineSnapshotMetadata metadata;
	std::vector<uint8_t> archiveBytes;
	if (!BdOnline::OnlineMakeStoredSnapshotMetadata(bundleBytes.data(), bundleBytes.size(), SAVEVER,
		OnlineSession.Epoch, networkState.CompletedTic, metadata, archiveBytes, error))
	{
		return false;
	}
	BdOnline::FOnlineSnapshotSender sender;
	if (!sender.Start(metadata, archiveBytes.data(), archiveBytes.size(), error))
		return false;

	OnlineSnapshotHandoff.Reset();
	OnlineSnapshotHandoff.Target = target;
	OnlineSnapshotHandoff.Metadata = std::move(metadata);
	OnlineSnapshotHandoff.HighestReplaySent = OnlineSnapshotHandoff.Metadata.SnapshotTic;
	OnlineSnapshotHandoff.HighestReplayAcknowledged = OnlineSnapshotHandoff.Metadata.SnapshotTic;
	OnlineSnapshotHandoff.Sender = std::move(sender);
	OnlineSnapshotHandoff.NextFrame = std::chrono::steady_clock::now();
	OnlineSnapshotHandoff.Deadline = OnlineSnapshotHandoff.NextFrame + std::chrono::minutes(5);
	OnlineSession.Phase = BdOnline::EOnlineSessionPhase::SynchronizingWorld;
	Printf("Synchronizing an online join snapshot (%llu bytes).\n",
		static_cast<unsigned long long>(OnlineSnapshotHandoff.Metadata.TotalBytes));
	return true;
#else
	(void)peerId;
	error = "this build was made without online multiplayer support";
	return false;
#endif
}

bool I_OnlineHasPendingJoinRestore()
{
#ifdef BIASEDDOOM_ONLINE
	return OnlineStartupJoin.Active && OnlineStartupJoin.Accepted && OnlineStartupJoin.ReadyForRestore &&
		!OnlineStartupJoin.Restored && OnlineStartupJoin.Failure.empty();
#else
	return false;
#endif
}

bool I_OnlineRestorePendingJoin(std::string& error)
{
#ifdef BIASEDDOOM_ONLINE
	error.clear();
	if (!I_OnlineHasPendingJoinRestore() || OnlineStartupJoin.PlayerSlot < 0 ||
		OnlineStartupJoin.PlayerSlot >= static_cast<int>(MAXPLAYERS))
	{
		error = "online join snapshot is not ready for restoration";
		return false;
	}
	// The snapshot intentionally omits the joining slot until its later roster
	// event. Keep a valid existing index while the regular savegame lifecycle
	// reconstructs the world; using -1 here lets unrelated player setup code
	// index outside the player array before deterministic admission.
	consoleplayer = 0;
	BdOnline::FOnlineSnapshotMetadata metadata;
	std::vector<uint8_t> archiveBytes;
	if (!OnlineSnapshotReception.Receiver.CopyCompletedArchive(metadata, archiveBytes, error))
		return false;
	if (!OnlineSnapshotReception.Matches(metadata) || metadata.Epoch != OnlineSession.Epoch ||
		metadata.TransferId != OnlineSnapshotReception.Metadata.TransferId)
	{
		error = "online join snapshot identity changed before restoration";
		return false;
	}
	FileSys::FCompressedBuffer archive = { 0, 0, 0, 0, nullptr, nullptr };
	if (!BdOnline::OnlineRestoreSnapshotArchive(metadata, archiveBytes.data(), archiveBytes.size(), archive, error))
		return false;
	BdOnline::FOnlineSnapshotBundle bundle;
	const bool decoded = archive.mBuffer != nullptr && archive.mCompressedSize > 0 &&
		BdOnline::OnlineDecodeSnapshotBundle(reinterpret_cast<const uint8_t*>(archive.mBuffer), archive.mCompressedSize, bundle, error);
	archive.Clean();
	if (!decoded || !G_RestoreOnlineSnapshotBundle(bundle, error))
		return false;

	std::vector<BdOnline::FOnlineTicRecord> records;
	if (!OnlineSnapshotReception.Replay.CopyThrough(OnlineSnapshotReception.LiveTic, records, error))
		return false;
	for (const auto& record : records)
	{
		if (!Net_RunOnlineReplayTic(record.Tic, record.Bytes.data(), record.Bytes.size(), error))
			return false;
	}
	OnlineStartupJoin.AppliedTic = OnlineSnapshotReception.LiveTic;
	OnlineStartupJoin.Restored = true;
	OnlineStartupJoin.NextReady = std::chrono::steady_clock::time_point{};
	OnlineSession.Phase = BdOnline::EOnlineSessionPhase::SynchronizingWorld;
	// The host's completion acknowledgement is deliberately delayed until the
	// restore and full initial replay succeeded. It may now schedule the normal
	// roster event after it receives Ready.
	BdOnline::FOnlineRelayTransport::FPeerId host = {};
	if (BdOnline::FOnlineRelayTransport::ParsePeerId(OnlineSession.HostPeerId, host))
		OnlineFlushSnapshotCompletionAcknowledgement(host);
	OnlinePumpStartupJoinControls();
	return true;
#else
	(void)error;
	error = "this build was made without online multiplayer support";
	return false;
#endif
}

bool I_OnlineAdvancePendingJoinReplay(std::string& error)
{
#ifdef BIASEDDOOM_ONLINE
	error.clear();
	if (!OnlineStartupJoin.Active || !OnlineStartupJoin.Restored || OnlineStartupJoin.Activated)
		return true;
	uint64_t highestTic = 0;
	if (!OnlineSnapshotReception.Replay.GetHighestContiguousTic(highestTic) || highestTic <= OnlineStartupJoin.AppliedTic)
		return true;
	std::vector<BdOnline::FOnlineTicRecord> records;
	if (!OnlineSnapshotReception.Replay.CopyThrough(highestTic, records, error))
		return false;
	for (const auto& record : records)
	{
		if (record.Tic <= OnlineStartupJoin.AppliedTic)
			continue;
		if (record.Tic != OnlineStartupJoin.AppliedTic + 1 ||
			!Net_RunOnlineReplayTic(record.Tic, record.Bytes.data(), record.Bytes.size(), error))
		{
			if (error.empty())
				error = "online join replay contains a tic gap after restoration";
			return false;
		}
		OnlineStartupJoin.AppliedTic = record.Tic;
	}
	return true;
#else
	(void)error;
	return true;
#endif
}

void I_OnlineActivateJoiningPlayer(int player)
{
#ifdef BIASEDDOOM_ONLINE
	if (!OnlineStartupJoin.Active || !OnlineStartupJoin.Restored || OnlineStartupJoin.Activated ||
		player != OnlineStartupJoin.PlayerSlot || player < 0 || player >= static_cast<int>(MAXPLAYERS))
	{
		return;
	}
	BdOnline::FOnlineRelayTransport::FPeerId host = {};
	if (!BdOnline::FOnlineRelayTransport::ParsePeerId(OnlineSession.HostPeerId, host))
		return;
	const sockaddr_in hostAddress = OnlineAddressForPeer(host);
	if (Net_Arbitrator >= 0 && Net_Arbitrator < static_cast<int>(MAXPLAYERS))
	{
		// The startup route temporarily occupied slot zero before the snapshot
		// told us who the current packet-server arbitrator is. Avoid leaving a
		// duplicate address that would make FindClient attribute the host packet
		// to the wrong legacy slot after a migrated-host restore.
		if (Net_Arbitrator != 0)
			Connected[0].Clear();
		Connected[Net_Arbitrator].Address = hostAddress;
		Connected[Net_Arbitrator].Status = CSTAT_READY;
	}
	Connected[player].Status = CSTAT_READY;
	consoleplayer = player;
	OnlineStartupJoin.Activated = true;
	OnlineStartupJoin.NextActivated = std::chrono::steady_clock::time_point{};
	OnlineStartupJoin.ActivationConfirmationDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	OnlinePumpStartupJoinControls();
#else
	(void)player;
#endif
}

bool I_OnlineInspectJoin(const std::string& serviceUrl, const std::string& joinCode,
	const std::string& password, FOnlineJoinReview& review, std::string& error)
{
	review = {};
#ifdef BIASEDDOOM_ONLINE
	if (!ConnectOnlineServiceUrl(serviceUrl, error))
		return false;
	BdOnline::FJoinTicket ticket;
	BdOnline::FSessionManifest manifest;
	if (!OnlineServiceClient->InspectJoinCode(joinCode, password, ticket, manifest, error))
		return false;
	if (!ticket.UsesRelay)
	{
		error = "this online session requires an unsupported direct route";
		return false;
	}
	review.HostPeerId = ticket.HostPeerId;
	review.SessionTitle = manifest.Title;
	review.ContentRootDigest = manifest.Content.RootDigest;
	review.TotalBytes = manifest.Content.TotalBytes;
	review.Packages.reserve(manifest.Content.Packages.size());
	for (const auto& package : manifest.Content.Packages)
		review.Packages.push_back({ package.DisplayName, package.Digest, package.Size });
	return true;
#else
	(void)serviceUrl;
	(void)joinCode;
	(void)password;
	error = "this build was made without online multiplayer support";
	return false;
#endif
}

bool I_OnlineReportSession(const std::string& serviceUrl, const std::string& sessionId,
	const std::string& reason, std::string& error)
{
#ifdef BIASEDDOOM_ONLINE
	if (!ConnectOnlineServiceUrl(serviceUrl, error))
		return false;
	if (!OnlineServiceClient->ReportSession(sessionId, reason, error))
		return false;
	return true;
#else
	(void)serviceUrl;
	(void)sessionId;
	(void)reason;
	error = "this build was made without online multiplayer support";
	return false;
#endif
}

// Returns the requested legacy lobby size while keeping online CLI use simple:
// `-online-host` means two slots unless an explicit positive count follows it
// or `-online-slots` is supplied.
#ifdef BIASEDDOOM_ONLINE
static int OnlineHostSlots(int onlineHostArg)
{
	if (const char* slots = Args->CheckValue("-online-slots"))
		return clamp<int>(atoi(slots), 1, MAXPLAYERS);
	const int valueIndex = onlineHostArg + 1;
	if (valueIndex < Args->NumArgs())
	{
		const char* value = Args->GetArg(valueIndex);
		if (value != nullptr && value[0] != '-' && value[0] != '+' && atoi(value) > 0)
			return clamp<int>(atoi(value), 1, MAXPLAYERS);
	}
	return 2;
}

static bool PrepareOnlineHost(int onlineHostArg, std::string& error)
{
	if (!ConnectOnlineService(error))
		return false;
	BdOnline::FSessionManifest manifest;
	if (!BuildOnlineLocalManifest(manifest, error))
		return false;
	manifest.MaxPlayers = static_cast<uint32_t>(OnlineHostSlots(onlineHostArg));
	const char* passwordArg = Args->CheckValue("-password");
	const std::string password = passwordArg != nullptr ? passwordArg : "";
	if (!manifest.Validate(error) || !OnlineServiceClient->CreateSession(manifest, password, OnlineSession, error))
		return false;
	if (!ConfigureOnlineRelay(OnlineSession.Endpoint, OnlineSession.SessionKey, OnlineSession.RelayToken,
		OnlineSession.LocalPeerId, OnlineSession.RelayAuth, OnlineSession.Epoch, error))
	{
		std::string releaseError;
		OnlineServiceClient->CloseSession(OnlineSession, releaseError);
		return false;
	}
	OnlineSessionIsHost = true;
	// Sessions have a two-hour lease; renew well before the deadline without
	// adding control-plane traffic to the real-time packet path.
	OnlineNextLeaseRenewal = std::chrono::steady_clock::now() + std::chrono::minutes(30);
	// Relay peers intentionally use the existing authoritative packet-server
	// topology. It avoids exposing synthetic peer routes to other guests.
	NetMode = NET_PacketServer;
	Printf("Online session ready. Share join code: " TEXTCOLOR_GREEN "%s" TEXTCOLOR_NORMAL "\n", OnlineSession.JoinCode.c_str());
	return true;
}

static bool PrepareOnlineJoin(int onlineJoinArg, std::string& error)
{
	if (onlineJoinArg + 1 >= Args->NumArgs() || Args->GetArg(onlineJoinArg + 1)[0] == '-' || Args->GetArg(onlineJoinArg + 1)[0] == '+')
	{
		error = "online join requires a code, for example -online-join ABCD-EFGH";
		return false;
	}
	if (!ConnectOnlineService(error))
		return false;
	const char* passwordArg = Args->CheckValue("-password");
	const std::string password = passwordArg != nullptr ? passwordArg : "";
	BdOnline::FJoinTicket ticket;
	BdOnline::FSessionManifest manifest;
	if (!OnlineServiceClient->ResolveJoinCode(Args->GetArg(onlineJoinArg + 1), password, ticket, manifest, error))
		return false;
	auto releaseAdmission = [&]()
	{
		BdOnline::FOnlineSession admitted;
		admitted.SessionId = ticket.SessionId;
		std::string releaseError;
		if (!OnlineServiceClient->LeaveSession(admitted, releaseError))
			DPrintf(DMSG_NOTIFY, "Online session admission release failed: %s\n", releaseError.c_str());
	};
	const auto now = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
		std::chrono::system_clock::now().time_since_epoch()).count());
	if (ticket.ExpiresAtUnixSeconds <= now)
	{
		error = "online join ticket expired; try the join code again";
		releaseAdmission();
		return false;
	}
	if (!ticket.UsesRelay)
	{
		error = "this online session requires an unsupported direct route";
		releaseAdmission();
		return false;
	}
	if (!ValidateOnlineLocalManifest(manifest, error))
	{
		releaseAdmission();
		return false;
	}
	OnlineSession = {};
	OnlineSession.SessionId = ticket.SessionId;
	OnlineSession.RelayToken = ticket.RelayToken;
	OnlineSession.RelayAuth = ticket.RelayAuth;
	OnlineSession.SessionKey = ticket.SessionKey;
	OnlineSession.LocalPeerId = ticket.LocalPeerId;
	OnlineSession.HostPeerId = ticket.HostPeerId;
	OnlineSession.Endpoint = ticket.Endpoint;
	OnlineSession.Epoch = ticket.Epoch;
	OnlineSession.Manifest = manifest;
	OnlineSession.Phase = BdOnline::EOnlineSessionPhase::Joining;
	OnlineJoinInProgress = !ticket.LobbyOpen && manifest.AllowJoinInProgress;
	if (!ConfigureOnlineRelay(ticket.Endpoint, ticket.SessionKey, ticket.RelayToken, ticket.LocalPeerId, ticket.RelayAuth, ticket.Epoch, error))
	{
		OnlineJoinInProgress = false;
		releaseAdmission();
		return false;
	}
	// Direct routing is opportunistic. A missing/stale candidate must never
	// prevent a player from joining through the authenticated relay.
	if (!ticket.HostDirectEndpoint.empty() && !ticket.HostPublicKey.empty())
	{
		sockaddr_in candidate = {};
		std::string candidateError;
		if (ParseOnlineDirectCandidate(ticket.HostDirectEndpoint, candidate, candidateError))
		{
			OnlineHostDirectCandidate = candidate;
			OnlineHostDirectCandidateValid = true;
			OnlineHostDirectPublicKey = ticket.HostPublicKey;
		}
		else
		{
			DPrintf(DMSG_NOTIFY, "Online direct candidate was ignored: %s\n", candidateError.c_str());
		}
	}
	OnlineSessionIsHost = false;
	BdOnline::FOnlineRelayTransport::FPeerId host = {};
	if (!BdOnline::FOnlineRelayTransport::ParsePeerId(ticket.HostPeerId, host))
	{
		error = "online service returned an invalid host identity";
		releaseAdmission();
		OnlineSessionActive = false;
		OnlineJoinInProgress = false;
		OnlinePeerAddresses.clear();
		OnlineAddressPeers.clear();
		return false;
	}
	// The legacy client always addresses slot zero. Give it a relay-backed
	// synthetic address before JoinGame starts sending its setup packets.
	Connected[0].Address = OnlineAddressForPeer(host);
	return true;
}

static bool BuildOnlinePreflightManifest(const std::vector<std::string>& allwads, BdOnline::FContentManifest& content,
	std::string& iwadDigest, std::string& error)
{
	const int iwadIndex = fileSystem.GetIwadNum();
	const int maxBaseResource = fileSystem.GetMaxIwadNum();
	const int firstTransferable = maxBaseResource + 1;
	if (iwadIndex < 0 || iwadIndex > maxBaseResource || maxBaseResource >= static_cast<int>(allwads.size()) ||
		firstTransferable < 0 || firstTransferable > static_cast<int>(allwads.size()))
	{
		error = "could not determine the pre-launch online resource plan";
		return false;
	}
	std::vector<BdOnline::FPackageSource> baseSources;
	baseSources.reserve(static_cast<size_t>(maxBaseResource - iwadIndex + 1));
	for (int index = iwadIndex; index <= maxBaseResource; ++index)
	{
		const auto path = std::filesystem::path(allwads[index]);
		baseSources.push_back({ path, path.filename().string(), static_cast<uint32_t>(index - iwadIndex + 1), false, false });
	}
	if (!BuildOnlineBaseFingerprint(baseSources, iwadDigest, error))
		return false;
	std::vector<BdOnline::FPackageSource> sources;
	for (int index = firstTransferable; index < static_cast<int>(allwads.size()); ++index)
	{
		const auto path = std::filesystem::path(allwads[index]);
		std::error_code code;
		if (std::filesystem::is_directory(path, code) && !code)
		{
			error = "directory-backed mods must be packaged as PK3 before online joining";
			return false;
		}
		sources.push_back({ path, path.filename().string(), static_cast<uint32_t>(index - firstTransferable + 1), false, false });
	}
	return BdOnline::OnlineBuildContentManifest(sources, content, error);
}

// Directory-backed mods cannot be advertised as their source tree: hosts and
// guests must mount the exact canonical PK3 whose bytes were hashed. This is
// intentionally shared by host setup and join preflight so a guest with the
// same local directory does not accidentally join using a different mount.
bool CanonicalizeOnlineDirectoryContent(std::vector<std::string>& allwads, const char* planName, std::string& error)
{
	const int firstTransferable = fileSystem.GetMaxIwadNum() + 1;
	if (firstTransferable < 0 || firstTransferable > static_cast<int>(allwads.size()))
	{
		error = "could not determine the online pre-launch content plan";
		return false;
	}

	std::vector<BdOnline::FPackageSource> sources;
	sources.reserve(allwads.size() - static_cast<size_t>(firstTransferable));
	for (int index = firstTransferable; index < static_cast<int>(allwads.size()); ++index)
	{
		const auto path = std::filesystem::path(allwads[index]);
		sources.push_back({ path, path.filename().string(), static_cast<uint32_t>(index - firstTransferable + 1), false, false });
	}

	const FString sessionId = GenerateUUID();
	std::filesystem::path contentCacheRoot;
	if (!GetOnlineContentCacheRoot(contentCacheRoot, error))
		return false;
	const auto bundleRoot = contentCacheRoot / planName / sessionId.GetChars();
	std::vector<BdOnline::FResolvedContentPackage> resolved;
	BdOnline::FContentManifest manifest;
	if (!BdOnline::OnlineResolveContentPlan(sources, bundleRoot, resolved, manifest, error))
		return false;
	if (resolved.size() != sources.size())
	{
		error = "online content plan lost an active resource";
		return false;
	}

	for (size_t index = 0; index < resolved.size(); ++index)
		allwads[firstTransferable + static_cast<int>(index)] = resolved[index].MountPath.string();
	return true;
}

bool I_OnlinePrepareHostContent(std::vector<std::string>& allwads, std::string& error)
{
	if (!Args->CheckParm("-online-host"))
		return true;
	if (Args->CheckParm("-host") || Args->CheckParm("-join") || Args->CheckParm("-online-join"))
	{
		error = "choose exactly one multiplayer start method";
		return false;
	}
	return CanonicalizeOnlineDirectoryContent(allwads, "session-bundles", error);
}

static bool ParseOnlineRelayEndpoint(const std::string& endpoint, sockaddr_in& address, std::string& error)
{
	constexpr const char* udp = "udp://";
	if (endpoint.rfind(udp, 0) != 0 || endpoint.size() <= std::strlen(udp) || endpoint.find_first_of("/?#", std::strlen(udp)) != std::string::npos)
	{
		error = "online service returned an invalid relay endpoint";
		return false;
	}
	BuildAddress(address, endpoint.c_str() + std::strlen(udp));
	return true;
}

// Direct candidates come from the relay's observed UDP source. Do not feed a
// candidate supplied by a remote control plane into BuildAddress(): that
// helper intentionally supports user-entered host names and can fatal on an
// unresolvable name. Direct acceleration only supports numeric IPv4 here, so
// malformed/IPv6 candidates safely leave the encrypted relay in use.
static bool ParseOnlineDirectCandidate(const std::string& endpoint, sockaddr_in& address, std::string& error)
{
	constexpr const char* udp = "udp://";
	if (endpoint.rfind(udp, 0) != 0 || endpoint.size() <= std::strlen(udp) ||
		endpoint.find_first_of("/?#[]", std::strlen(udp)) != std::string::npos)
	{
		error = "online service returned an invalid direct UDP candidate";
		return false;
	}
	const std::string authority = endpoint.substr(std::strlen(udp));
	const size_t separator = authority.rfind(':');
	if (separator == std::string::npos || separator == 0 || separator + 1 >= authority.size() ||
		authority.find(':') != separator)
	{
		error = "online service returned an invalid direct UDP candidate";
		return false;
	}
	char* end = nullptr;
	const unsigned long port = std::strtoul(authority.c_str() + separator + 1, &end, 10);
	if (end == nullptr || *end != '\0' || port == 0 || port > UINT16_MAX)
	{
		error = "online service returned an invalid direct UDP candidate";
		return false;
	}
	const std::string host = authority.substr(0, separator);
	const unsigned long rawAddress = inet_addr(host.c_str());
	if (rawAddress == INADDR_NONE || rawAddress == INADDR_ANY)
	{
		error = "online service returned an invalid direct UDP candidate";
		return false;
	}
	address = {};
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = rawAddress;
	address.sin_port = htons(static_cast<u_short>(port));
	return true;
}

static uint64_t OnlineContentCacheQuota(std::string& error)
{
	const char* value = Args->CheckValue("-online-cache-mb");
	if (value == nullptr)
		return BdOnline::OnlineDefaultContentCacheBytes;
	char* end = nullptr;
	const unsigned long long megabytes = std::strtoull(value, &end, 10);
	if (value[0] == '\0' || end == nullptr || *end != '\0' || megabytes == 0 ||
		megabytes > BdOnline::OnlineMaxContentBytes / (1024ull * 1024ull))
	{
		error = "-online-cache-mb must be between 1 and 8192";
		return 0;
	}
	return static_cast<uint64_t>(megabytes) * 1024ull * 1024ull;
}

static void PrintOnlineContentReview(const BdOnline::FJoinTicket& ticket, const BdOnline::FSessionManifest& manifest)
{
	Printf("Online content review for '%s' (anonymous host %.12s): %u package%s, %llu bytes\n",
		manifest.Title.c_str(), ticket.HostPeerId.c_str(), static_cast<unsigned>(manifest.Content.Packages.size()),
		manifest.Content.Packages.size() == 1 ? "" : "s", static_cast<unsigned long long>(manifest.Content.TotalBytes));
	Printf("  Content-plan BLAKE2b-256 %s\n", manifest.Content.RootDigest.c_str());
	std::string approval;
	std::string approvalError;
	if (BdOnline::OnlineBuildContentApprovalToken(manifest.Content.RootDigest, ticket.HostPeerId, approval, approvalError))
		Printf("  Approval token %s\n", approval.c_str());
	for (const auto& package : manifest.Content.Packages)
	{
		Printf("  %u. %s (%llu bytes, %s) BLAKE2b-256 %s\n", package.LoadOrder, package.DisplayName.c_str(),
			static_cast<unsigned long long>(package.Size), package.FileExtension.c_str(), package.Digest.c_str());
	}
}

static bool DownloadOnlinePackage(SOCKET socket, const sockaddr_in& relay, BdOnline::FOnlineRelayTransport& transport,
	const BdOnline::FOnlineRelayTransport::FPeerId& host, const BdOnline::FContentPackage& package,
	const std::filesystem::path& cacheRoot, std::filesystem::path& mountedPath, std::string& error)
{
	BdOnline::FOnlineContentCache cache(cacheRoot);
	if (cache.HasVerified(package, error))
	{
		mountedPath = cache.FinalPath(package);
		return true;
	}
	if (!error.empty())
		return false;
	error.clear();
	const auto partial = cache.PartialPath(package);
	const auto final = cache.FinalPath(package);
	if (partial.empty() || final.empty())
	{
		error = "online content cache path is invalid";
		return false;
	}
	if (!cache.PreparePackageDirectory(package, error))
		return false;
	std::error_code code;
	uint64_t offset = 0;
	if (!cache.PartialSize(package, offset, error))
		return false;
	const auto storage = std::filesystem::space(cacheRoot, code);
	if (code || storage.available < package.Size - offset)
	{
		error = "not enough storage is available for the host-provided content";
		return false;
	}
	std::array<uint8_t, BdOnline::OnlineDigestBytes> digest = {};
	if (!OnlineDecodeDigest(package.Digest, digest))
	{
		error = "host content digest is invalid";
		return false;
	}
	std::ofstream output(partial, std::ios::binary | std::ios::app);
	if (!output)
	{
		error = "could not open the online content download partial";
		return false;
	}
	while (offset < package.Size || (package.Size == 0 && offset == 0))
	{
		std::vector<uint8_t> request;
		request.insert(request.end(), std::begin(OnlineContentMagic), std::end(OnlineContentMagic));
		request.push_back(OnlineContentRequest);
		request.insert(request.end(), digest.begin(), digest.end());
		OnlineWriteU64(request, offset);
		OnlineWriteU16(request, static_cast<uint16_t>(OnlineContentChunkBytes));
		bool received = false;
		for (int attempt = 0; attempt < 3 && !received; ++attempt)
		{
			std::vector<uint8_t> wire;
			if (!transport.Encapsulate(host, request.data(), request.size(), wire, error))
				return false;
			if (sendto(socket, reinterpret_cast<const char*>(wire.data()), static_cast<int>(wire.size()), 0,
				reinterpret_cast<const sockaddr*>(&relay), sizeof(relay)) == SOCKET_ERROR)
			{
				error = std::string("could not request host content: ") + neterror();
				return false;
			}
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
			while (std::chrono::steady_clock::now() < deadline && !received)
			{
				sockaddr_in ignored = {};
				socklen_t ignoredSize = sizeof(ignored);
				const int receivedSize = recvfrom(socket, reinterpret_cast<char*>(OnlineReceiveBuffer.data()), static_cast<int>(OnlineReceiveBuffer.size()), 0,
					reinterpret_cast<sockaddr*>(&ignored), &ignoredSize);
				if (receivedSize == SOCKET_ERROR)
				{
					if (WSAGetLastError() != WSAEWOULDBLOCK)
					{
						error = std::string("could not receive host content: ") + neterror();
						return false;
					}
					Sleep(10);
					continue;
				}
				BdOnline::FOnlineRelayTransport::FPeerId sender = {};
				std::vector<uint8_t> response;
				std::string ignoredError;
				if (!transport.Decapsulate(OnlineReceiveBuffer.data(), static_cast<size_t>(receivedSize), sender, response, ignoredError) || sender != host)
					continue;
				const size_t header = 4 + 1 + BdOnline::OnlineDigestBytes + 8 + 1 + 2;
				if (response.size() < header || std::memcmp(response.data(), OnlineContentMagic, sizeof(OnlineContentMagic)) != 0 ||
					response[4] != OnlineContentChunk || std::memcmp(response.data() + 5, digest.data(), digest.size()) != 0 ||
					OnlineReadU64(response.data() + 5 + BdOnline::OnlineDigestBytes) != offset)
					continue;
				const bool finalChunk = response[5 + BdOnline::OnlineDigestBytes + 8] != 0;
				const uint16_t count = OnlineReadU16(response.data() + 5 + BdOnline::OnlineDigestBytes + 8 + 1);
				if (response.size() != header + count || count > OnlineContentChunkBytes || count > package.Size - offset ||
					(finalChunk != (offset + count == package.Size)))
				{
					error = "host content transfer returned an invalid chunk";
					return false;
				}
				if (count != 0)
				{
					output.write(reinterpret_cast<const char*>(response.data() + header), count);
					if (!output)
					{
						error = "could not write host-provided content";
						return false;
					}
				}
				offset += count;
				received = true;
			}
		}
		if (!received)
		{
			error = "timed out while downloading host-provided content";
			return false;
		}
		if (package.Size == 0 || offset == package.Size)
			break;
	}
	output.close();
	if (!cache.CommitPartial(package, error))
		return false;
	mountedPath = cache.FinalPath(package);
	return true;
}

bool I_OnlinePreflightContent(std::vector<std::string>& allwads, std::string& error)
{
	const int joinArg = Args->CheckParm("-online-join");
	if (!joinArg)
		return true;
	if (Args->CheckParm("-join") || Args->CheckParm("-host") || Args->CheckParm("-online-host"))
	{
		error = "choose exactly one multiplayer start method";
		return false;
	}
	if (joinArg + 1 >= Args->NumArgs())
	{
		error = "online join requires a join code";
		return false;
	}
	if (!ConnectOnlineService(error))
		return false;
	const char* passwordArg = Args->CheckValue("-password");
	const std::string password = passwordArg != nullptr ? passwordArg : "";
	BdOnline::FJoinTicket ticket;
	BdOnline::FSessionManifest manifest;
	if (!OnlineServiceClient->InspectJoinCode(Args->GetArg(joinArg + 1), password, ticket, manifest, error))
		return false;
	if (!ticket.UsesRelay)
	{
		error = "this online session requires an unsupported direct route";
		return false;
	}
	// Resolve local directories before comparing manifests. If the local bytes
	// already match the host, this makes the guest mount exactly the same
	// canonical PK3; if they do not, the plan below is replaced by verified
	// host content before the file system is initialized.
	if (!CanonicalizeOnlineDirectoryContent(allwads, "preflight-bundles", error))
		return false;
	BdOnline::FContentManifest localContent;
	std::string localIWAD;
	if (!BuildOnlinePreflightManifest(allwads, localContent, localIWAD, error))
		return false;
	if (manifest.EngineVersion != GetVersionString() || manifest.IWADDigest != localIWAD)
	{
		error = "this online session needs a different engine version or IWAD";
		return false;
	}
	if (localContent.RootDigest == manifest.Content.RootDigest)
		return true;
	std::filesystem::path cacheRoot;
	if (!GetOnlineContentCacheRoot(cacheRoot, error))
		return false;
	std::vector<std::filesystem::path> cachedPaths;
	bool cacheComplete = false;
	if (!BdOnline::OnlineResolveCachedContent(cacheRoot, manifest.Content, cachedPaths, cacheComplete, error))
		return false;
	if (cacheComplete)
	{
		const int firstTransferable = fileSystem.GetMaxIwadNum() + 1;
		allwads.erase(allwads.begin() + firstTransferable, allwads.end());
		for (const auto& path : cachedPaths)
			allwads.push_back(path.string());
		Printf("Using %u verified cached online content package%s.\n", static_cast<unsigned>(cachedPaths.size()),
			cachedPaths.size() == 1 ? "" : "s");
		return true;
	}
	PrintOnlineContentReview(ticket, manifest);
	const char* approvedContent = Args->CheckValue("-online-accept-mods");
	std::string expectedApproval;
	std::string approvalError;
	if (!BdOnline::OnlineBuildContentApprovalToken(manifest.Content.RootDigest, ticket.HostPeerId, expectedApproval, approvalError))
	{
		error = "online service returned an invalid host content approval identity";
		return false;
	}
	if (approvedContent == nullptr || expectedApproval != approvedContent)
	{
		error = "the host or mod plan was not approved; review the package list above and restart with -online-accept-mods " +
			expectedApproval + " to permit this exact download";
		return false;
	}
	sockaddr_in relay = {};
	if (!ParseOnlineRelayEndpoint(ticket.Endpoint, relay, error))
		return false;
	BdOnline::FOnlineRelayTransport transport;
	if (!transport.Configure(ticket.SessionKey, ticket.RelayToken, ticket.LocalPeerId, ticket.RelayAuth, ticket.Epoch, error))
		return false;
	BdOnline::FOnlineRelayTransport::FPeerId host = {};
	if (!BdOnline::FOnlineRelayTransport::ParsePeerId(ticket.HostPeerId, host))
	{
		error = "online service returned an invalid host identity";
		return false;
	}
#ifdef __WIN32__
	WSADATA data;
	if (WSAStartup(0x0101, &data))
	{
		error = "could not initialize sockets for online content transfer";
		return false;
	}
#endif
	SOCKET contentSocket = socket(PF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (contentSocket == INVALID_SOCKET)
	{
#ifdef __WIN32__
		WSACleanup();
#endif
		error = std::string("could not create the online content socket: ") + neterror();
		return false;
	}
	u_long nonblocking = 1;
	if (ioctlsocket(contentSocket, FIONBIO, &nonblocking) == SOCKET_ERROR)
	{
		closesocket(contentSocket);
#ifdef __WIN32__
		WSACleanup();
#endif
		error = "could not configure the online content socket";
		return false;
	}
	BdOnline::FOnlineContentCache cache(cacheRoot);
	uint64_t incomingBytes = 0;
	for (const auto& package : manifest.Content.Packages)
	{
		std::string cacheError;
		if (!cache.HasVerified(package, cacheError))
		{
			if (!cacheError.empty())
			{
				closesocket(contentSocket);
#ifdef __WIN32__
				WSACleanup();
#endif
				error = cacheError;
				return false;
			}
			uint64_t resumedBytes = 0;
			if (!cache.PartialSize(package, resumedBytes, cacheError))
			{
				closesocket(contentSocket);
#ifdef __WIN32__
				WSACleanup();
#endif
				error = cacheError;
				return false;
			}
			const uint64_t remainingBytes = package.Size - resumedBytes;
			if (incomingBytes > std::numeric_limits<uint64_t>::max() - remainingBytes)
			{
				closesocket(contentSocket);
#ifdef __WIN32__
				WSACleanup();
#endif
				error = "online content download size overflow";
				return false;
			}
			incomingBytes += remainingBytes;
		}
	}
	const uint64_t quota = OnlineContentCacheQuota(error);
	if (quota == 0 || !cache.EnsureSpace(incomingBytes, quota, manifest.Content.Packages, error))
	{
		closesocket(contentSocket);
#ifdef __WIN32__
		WSACleanup();
#endif
		return false;
	}
	std::vector<std::string> downloaded;
	for (const auto& package : manifest.Content.Packages)
	{
		std::filesystem::path mounted;
		if (!DownloadOnlinePackage(contentSocket, relay, transport, host, package, cacheRoot, mounted, error))
		{
			closesocket(contentSocket);
#ifdef __WIN32__
			WSACleanup();
#endif
			return false;
		}
		downloaded.push_back(mounted.string());
	}
	closesocket(contentSocket);
#ifdef __WIN32__
	WSACleanup();
#endif
	const int firstTransferable = fileSystem.GetMaxIwadNum() + 1;
	allwads.erase(allwads.begin() + firstTransferable, allwads.end());
	allwads.insert(allwads.end(), downloaded.begin(), downloaded.end());
	for (size_t index = 0; index < manifest.Content.Packages.size(); ++index)
	{
		if (!BdOnline::OnlineVerifyContentFile(downloaded[index], manifest.Content.Packages[index], error))
			return false;
	}
	return true;
}

bool I_OnlinePrepareHostMigration(int& successor)
{
	successor = -1;
	if (!OnlineSessionActive || !OnlineSessionIsHost || consoleplayer != Net_Arbitrator ||
		!OnlineServiceClient || OnlineSession.SessionId.empty())
	{
		return false;
	}

	std::vector<std::string> eligible;
	for (const auto client : NetworkClients)
	{
		if (client == consoleplayer)
			continue;
		BdOnline::FOnlineRelayTransport::FPeerId peer = {};
		if (!OnlinePeerForClient(client, peer))
		{
			DPrintf(DMSG_NOTIFY, "Online host migration could not identify a connected relay peer.\n");
			return false;
		}
		eligible.push_back(OnlineEncodeDigest(peer.data(), peer.size()));
	}
	if (eligible.empty())
		return false;
	std::sort(eligible.begin(), eligible.end());

	BdOnline::FOnlineHostMigration migration;
	std::string error;
	if (!OnlineServiceClient->MigrateSession(OnlineSession, eligible, migration, error))
	{
		DPrintf(DMSG_NOTIFY, "Online host migration could not be coordinated: %s\n", error.c_str());
		return false;
	}
	if (migration.HostPeerId != eligible.front() || migration.Epoch <= OnlineSession.Epoch)
	{
		DPrintf(DMSG_NOTIFY, "Online service returned an inconsistent host migration decision.\n");
		return false;
	}
	for (const auto client : NetworkClients)
	{
		if (client == consoleplayer)
			continue;
		BdOnline::FOnlineRelayTransport::FPeerId peer = {};
		if (OnlinePeerForClient(client, peer) && OnlineEncodeDigest(peer.data(), peer.size()) == migration.HostPeerId)
		{
			successor = client;
			break;
		}
	}
	if (successor < 0)
	{
		DPrintf(DMSG_NOTIFY, "Online host migration selected a peer that is no longer in the legacy roster.\n");
		return false;
	}

	OnlinePendingHostMigration = { true, migration.Epoch, OnlineSession.HostPeerId, migration.HostPeerId };
	// The service has already promoted the successor. On shutdown the old host
	// must leave as a former member, never close the freshly migrated session.
	OnlineSessionIsHost = false;
	OnlineSession.Phase = BdOnline::EOnlineSessionPhase::RecoveringHost;
	return true;
}

void I_OnlineSendPreparedHostMigration()
{
	if (!OnlineSessionActive || !OnlinePendingHostMigration.Valid || MySocket == INVALID_SOCKET)
		return;
	BdOnline::FOnlineRelayTransport::FPeerId nextHost = {};
	if (!BdOnline::FOnlineRelayTransport::ParsePeerId(OnlinePendingHostMigration.HostPeerId, nextHost))
		return;
	std::vector<uint8_t> packet;
	packet.reserve(4 + 1 + 4 + nextHost.size());
	packet.insert(packet.end(), std::begin(OnlineMigrationMagic), std::end(OnlineMigrationMagic));
	packet.push_back(OnlineMigrationVersion);
	OnlineWriteU32(packet, OnlinePendingHostMigration.Epoch);
	packet.insert(packet.end(), nextHost.begin(), nextHost.end());
	for (const auto client : NetworkClients)
	{
		if (client == consoleplayer)
			continue;
		std::string error;
		if (!SendOnlinePacket(Connected[client].Address, packet.data(), packet.size(), error))
			DPrintf(DMSG_NOTIFY, "Online host migration notification failed: %s\n", error.c_str());
	}
}

void I_OnlineApplyHostMigration(int /*departingHost*/, int successor)
{
	ApplyPendingOnlineHostMigration(-1, successor);
}

void I_OnlineRecordSimulationTic(uint64_t tic)
{
	// Only the active packet-server arbitrator can author a replay record.
	// Clients receive the same command stream, but retaining a second local
	// history would waste memory and could accidentally be used as authority.
	if (!OnlineSessionActive || !OnlineSessionIsHost || consoleplayer != Net_Arbitrator ||
		!netgame || NetMode != NET_PacketServer)
	{
		return;
	}
	// G_Ticker runs once for every duplicated simulation tic, while a replay
	// record represents exactly one packet-server command sequence. Silently
	// ignore the duplicate calls instead of treating a normal TicDup setting as
	// a malformed history entry and clearing a viable handoff window.
	if (TicDup == 0 || gametic < 0 || (gametic % TicDup) != 0 ||
		tic != static_cast<uint64_t>(gametic / TicDup))
	{
		return;
	}

	std::vector<uint8_t> record;
	std::string error;
	if (!Net_BuildOnlineReplayRecord(tic, record, error) ||
		!OnlineTicHistory.Append(tic, record.data(), record.size(), error))
	{
		// A gap is more dangerous than losing this optional pre-admission
		// history: a future handoff must start from a fresh snapshot instead of
		// replaying a non-contiguous stream.
		OnlineTicHistory.Clear();
		DPrintf(DMSG_NOTIFY, "Online replay history was reset: %s\n", error.c_str());
	}
}
#endif

#ifndef BIASEDDOOM_ONLINE
bool I_OnlinePrepareHostContent(std::vector<std::string>& /*allwads*/, std::string& error)
{
	if (Args->CheckParm("-online-host"))
	{
		error = "this build was made without online multiplayer support";
		return false;
	}
	return true;
}

bool I_OnlinePreflightContent(std::vector<std::string>& /*allwads*/, std::string& error)
{
	if (Args->CheckParm("-online-join"))
	{
		error = "this build was made without online multiplayer support";
		return false;
	}
	return true;
}

bool I_OnlinePrepareHostMigration(int& successor)
{
	successor = -1;
	return false;
}

void I_OnlineSendPreparedHostMigration()
{
}

void I_OnlineApplyHostMigration(int /*departingHost*/, int /*successor*/)
{
}

void I_OnlineRecordSimulationTic(uint64_t /*tic*/)
{
}
#endif

static void StartNetwork(bool autoPort)
{
#ifdef __WIN32__
	WSADATA data;
	if (!DebugServer::RuntimeEvents::IsDebugServerRunning()) {
		if (WSAStartup(0x0101, &data))
			I_FatalError("Couldn't initialize Windows sockets");
	}
#endif

	netgame = true;
	multiplayer = true;
	MySocket = CreateUDPSocket();
	BindToLocalPort(MySocket, autoPort ? 0 : GamePort);

	u_long trueVal = 1u;
#ifndef __sun
	ioctlsocket(MySocket, FIONBIO, &trueVal);
#else
	fcntl(MySocket, F_SETFL, trueVal | O_NONBLOCK);
#endif
}

void CloseNetwork()
{
	if (MySocket != INVALID_SOCKET)
	{
		closesocket(MySocket);
		MySocket = INVALID_SOCKET;
		netgame = false;
	}
#ifdef BIASEDDOOM_ONLINE
	if (OnlineSessionActive && OnlineServiceClient && !OnlineSession.SessionId.empty())
	{
		std::string error;
		const bool released = OnlineSessionIsHost
			? OnlineServiceClient->CloseSession(OnlineSession, error)
			: OnlineServiceClient->LeaveSession(OnlineSession, error);
		if (!released)
			DPrintf(DMSG_NOTIFY, "Online session release failed: %s\n", error.c_str());
	}
	OnlineSessionActive = false;
	OnlineSessionIsHost = false;
	OnlineJoinInProgress = false;
	OnlineNextLeaseRenewal = {};
	OnlinePendingHostMigration = {};
	OnlineLastHostPacket = {};
	OnlineNextRecoveryAttempt = {};
	OnlineNextLivenessHeartbeat = {};
	OnlineTicHistory.Clear();
	OnlineSnapshotReception.Reset();
	OnlineSnapshotHandoff.Reset();
	OnlinePendingJoinAdmissions.clear();
	OnlineStartupJoin.Reset();
	OnlinePeerAddresses.clear();
	OnlineAddressPeers.clear();
#endif
#ifdef __WIN32__
	if (!DebugServer::RuntimeEvents::IsDebugServerRunning()){
		WSACleanup();
	}
#endif
}

static int PrivateNetOf(const in_addr& in)
{
	int addr = ntohl(in.s_addr);
	if ((addr & 0xFFFF0000) == 0xC0A80000)		// 192.168.0.0
	{
		return 0xC0A80000;
	}
	else if ((addr & 0xFFFF0000) >= 0xAC100000 && (addr & 0xFFFF0000) <= 0xAC1F0000)	// 172.16.0.0 - 172.31.0.0
	{
		return 0xAC100000;
	}
	else if ((addr & 0xFF000000) == 0x0A000000)		// 10.0.0.0
	{
		return 0x0A000000;
	}
	else if ((addr & 0xFF000000) == 0x7F000000)		// 127.0.0.0 (localhost)
	{
		return 0x7F000000;
	}
	// Not a private IP
	return 0;
}

// The best I can really do here is check if the others are on the same
// private network, since that means we (probably) are too.
static bool ClientsOnSameNetwork()
{
	int start = 1;
	for (; start < MaxClients; ++start)
	{
		if (Connected[start].Status != CSTAT_NONE)
			break;
	}

	if (start >= MaxClients)
		return false;

	const int firstClient = PrivateNetOf(Connected[start].Address.sin_addr);
	if (!firstClient)
		return false;

	for (int i = 1; i < MaxClients; ++i)
	{
		if (i == start)
			continue;

		if (Connected[i].Status == CSTAT_NONE || PrivateNetOf(Connected[i].Address.sin_addr) != firstClient)
			return false;
	}

	return true;
}

static void GenerateGameID()
{
	const uint64_t val = GameIDGen.GenRand64();
	memcpy(GameID, &val, sizeof(val));
}

// Print a network-related message to the console. This doesn't print to the window so should
// not be used for that and is mainly for logging.
static void I_NetLog(const char* text, ...)
{
	// todo: use better abstraction once everything is migrated to in-game start screens.
#if defined _WIN32 || defined __APPLE__
	va_list ap;
	va_start(ap, text);
	VPrintf(PRINT_HIGH, text, ap);
	Printf("\n");
	va_end(ap);
#else
	FString str;
	va_list argptr;

	va_start(argptr, text);
	str.VFormat(text, argptr);
	va_end(argptr);
	fprintf(stderr, "\r%-40s\n", str.GetChars());
#endif
}

// Gracefully closes the net window so that any error messaging can be properly displayed.
static void I_NetError(const char* error)
{
	StartWindow->NetClose();
	I_FatalError("%s", error);
}

static void I_NetInit(const char* msg, bool host)
{
	StartWindow->NetInit(msg, host);
}

// todo: later these must be dispatched by the main menu, not the start screen.
// Updates the general status of the lobby.
static void I_NetMessage(const char* msg)
{
	StartWindow->NetMessage(msg);
}

// Listen for incoming connections while the lobby is active. The main thread needs to be locked up
// here to prevent the engine from continuing to start the game until everyone is ready.
static bool I_NetLoop(bool (*loopCallback)(void*), void* data)
{
	return StartWindow->NetLoop(loopCallback, data);
}

// A new client has just entered the game, so add them to the player list.
static void I_NetClientConnected(int client, unsigned int charLimit = 0u)
{
	const char* name = Net_GetClientName(client, charLimit);
	unsigned int flags = CFL_NONE;
	if (client == 0)
		flags |= CFL_HOST;
	if (client == consoleplayer)
		flags |= CFL_CONSOLEPLAYER;

	StartWindow->NetConnect(client, name, flags, Connected[client].Status);
}

// A client changed ready state.
static void I_NetClientUpdated(int client)
{
	StartWindow->NetUpdate(client, Connected[client].Status);
}

static void I_NetClientDisconnected(int client)
{
	StartWindow->NetDisconnect(client);
}

static void I_NetUpdatePlayers(int current, int limit)
{
	StartWindow->NetProgress(current, limit);
}

static bool I_ShouldStartNetGame()
{
	return StartWindow->ShouldStartNet();
}

static void I_GetKickClients(TArray<int>& clients)
{
	clients.Clear();

	int c = -1;
	while ((c = StartWindow->GetNetKickClient()) != -1)
		clients.Push(c);
}

static void I_GetBanClients(TArray<int>& clients)
{
	clients.Clear();

	int c = -1;
	while ((c = StartWindow->GetNetBanClient()) != -1)
		clients.Push(c);
}

void I_NetDone()
{
	StartWindow->NetDone();
}

void I_ClearClient(size_t client)
{
	Connected[client].Clear();
}

static int FindClient(const sockaddr_in& address)
{
	int i = 0;
	for (; i < MaxClients; ++i)
	{
		if (Connected[i].Status == CSTAT_NONE)
			continue;

		if (address.sin_addr.s_addr == Connected[i].Address.sin_addr.s_addr
			&& address.sin_port == Connected[i].Address.sin_port)
		{
			break;
		}
	}

	return i >= MaxClients ? -1 : i;
}

static void SendPacket(const sockaddr_in& to)
{
	// Huge packets should be sent out as sequences, not as one big packet, otherwise it's prone
	// to high amounts of congestion and reordering needed.
	if (NetBufferLength > MAX_MSGLEN)
		I_FatalError("Netbuffer overflow: Tried to send %lu bytes of data", NetBufferLength);

	assert(!(NetBuffer[0] & NCMD_COMPRESSED));

	uint8_t* dataStart = &TransmitBuffer[4];
	uLong size = MaxTransmitSize - 5u;
	if (NetBufferLength >= MinCompressionSize)
	{
		*dataStart = NetBuffer[0] | NCMD_COMPRESSED;
		const int res = compress2(dataStart + 1, &size, NetBuffer + 1, NetBufferLength - 1u, 9);
		if (res != Z_OK)
			I_Error("Net compression failed (zlib error %d)", res);

		++size;
	}
	else
	{
		memcpy(dataStart, NetBuffer, NetBufferLength);
		size = NetBufferLength;
	}

	if (size + 4 > MaxTransmitSize)
		I_Error("Failed to compress data down to acceptable transmission size");

	// If a connection packet, don't check the game id since they might not have it yet.
	const uint32_t crc = (NetBuffer[0] & NCMD_SETUP) ? CalcCRC32(dataStart, size) : AddCRC32(CalcCRC32(dataStart, size), GameID, std::extent_v<decltype(GameID)>);
	TransmitBuffer[0] = crc >> 24;
	TransmitBuffer[1] = crc >> 16;
	TransmitBuffer[2] = crc >> 8;
	TransmitBuffer[3] = crc;

#ifdef BIASEDDOOM_ONLINE
	if (OnlineSessionActive)
	{
		std::string error;
		if (!SendOnlinePacket(to, TransmitBuffer, size + 4, error))
			I_Error("%s", error.c_str());
		return;
	}
#endif
	sendto(MySocket, (const char*)TransmitBuffer, size + 4, 0, (const sockaddr*)&to, sizeof(to));
}

static void GetPacket(sockaddr_in* const from = nullptr)
{
	sockaddr_in fromAddress;
	socklen_t fromSize = sizeof(fromAddress);
	uint8_t* receiveBuffer = TransmitBuffer;
	size_t receiveCapacity = MaxTransmitSize;
#ifdef BIASEDDOOM_ONLINE
	if (OnlineSessionActive)
	{
		receiveBuffer = OnlineReceiveBuffer.data();
		receiveCapacity = OnlineReceiveBuffer.size();
	}
#endif

	int msgSize = recvfrom(MySocket, reinterpret_cast<char*>(receiveBuffer), static_cast<int>(receiveCapacity), 0,
			  (sockaddr *)&fromAddress, &fromSize);

	#ifdef BIASEDDOOM_ONLINE
	if (OnlineSessionActive && msgSize > 0)
	{
		std::string error;
		if (!ReceiveOnlinePacket(receiveBuffer, fromAddress, msgSize, error))
		{
			// Packets can be stale after a route refresh or migration. They are
			// authenticated and discarded before the legacy parser sees them.
			msgSize = 0;
		}
	}
	#endif

	int client = FindClient(fromAddress);
	if (client >= 0 && msgSize == SOCKET_ERROR)
	{
		int err = WSAGetLastError();
		if (err == WSAECONNRESET)
		{
			if (consoleplayer == -1)
			{
				client = -1;
				msgSize = 0;
			}
			else
			{
				// The remote node aborted unexpectedly, so pretend it sent an exit packet. If in packet server
				// mode and it was the host, just consider the game too bricked to continue since the host has
				// to determine the new host properly.
				if (NetMode == NET_PacketServer && client == Net_Arbitrator)
				{
#ifdef BIASEDDOOM_ONLINE
					if (OnlineSessionActive)
					{
						// The next control-plane recovery attempt is deliberately
						// deferred to I_NetCmd, outside this socket parser.
						OnlineLastHostPacket = std::chrono::steady_clock::now() - OnlineHostSilenceTimeout;
						client = -1;
						msgSize = 0;
					}
					else
#endif
					{
						I_NetError("Host unexpectedly disconnected");
					}
				}

				if (msgSize != 0)
				{
					NetBuffer[0] = NCMD_EXIT;
					msgSize = 1;
				}
			}
		}
		else if (err != WSAEWOULDBLOCK)
		{
			I_Error("Failed to get packet: %s", neterror());
		}
		else
		{
			client = -1;
			msgSize = 0;
		}
	}
	else if (msgSize > 0)
	{
		// A valid datagram holds at least a command byte plus the 4-byte CRC; reject runts
		// so the msgSize - 4 / msgSize - 5 sizes below can't underflow into huge lengths.
		if (msgSize < 5)
		{
			client = -1;
			msgSize = 0;
		}
		else
		{
			const uint8_t* dataStart = &TransmitBuffer[4];
			if (client == -1 && !(*dataStart & NCMD_SETUP))
			{
				msgSize = 0;
			}
			else if (client == -1 && bGameStarted)
			{
				NetBuffer[0] = NCMD_SETUP;
				NetBuffer[1] = PRE_IN_PROGRESS;
				NetBufferLength = 2u;
				SendPacket(fromAddress);
				msgSize = 0;
			}
			else
			{
				const uint32_t check = (*dataStart & NCMD_SETUP) ? CalcCRC32(dataStart, msgSize - 4) : AddCRC32(CalcCRC32(dataStart, msgSize - 4), GameID, std::extent_v<decltype(GameID)>);
				const uint32_t crc = (TransmitBuffer[0] << 24) | (TransmitBuffer[1] << 16) | (TransmitBuffer[2] << 8) | TransmitBuffer[3];
				if (check != crc)
				{
					DPrintf(DMSG_NOTIFY, "Checksum on packet failed: expected %u, got %u", check, crc);
					client = -1;
					msgSize = 0;
				}
				else
				{
					NetBuffer[0] = (*dataStart & ~NCMD_COMPRESSED);
					if (*dataStart & NCMD_COMPRESSED)
					{
						uLongf size = MAX_MSGLEN - 1;
						int err = uncompress(NetBuffer + 1, &size, dataStart + 1, msgSize - 5);
						if (err != Z_OK)
						{
							Printf("Net decompression failed (zlib error %s)\n", M_ZLibError(err).GetChars());
							client = -1;
							msgSize = 0;
						}
						else
						{
							msgSize = size + 1;
						}
					}
					else
					{
						msgSize -= 4;
						memcpy(NetBuffer + 1, dataStart + 1, msgSize - 1);
					}
				}
			}
		}
	}
	else
	{
		client = -1;
	}

	RemoteClient = client;
	NetBufferLength = max<int>(msgSize, 0);
	if (from != nullptr)
		*from = fromAddress;
}

void I_NetCmd(ENetCommand cmd)
{
#ifdef BIASEDDOOM_ONLINE
	SendOnlineHostLivenessHeartbeat();
	SendOnlineGuestLivenessHeartbeat();
	if (OnlineSessionActive && OnlineSessionIsHost && OnlineServiceClient &&
		std::chrono::steady_clock::now() >= OnlineNextLeaseRenewal)
	{
		std::string error;
		if (OnlineServiceClient->RenewSession(OnlineSession, error))
		{
			OnlineNextLeaseRenewal = std::chrono::steady_clock::now() + std::chrono::minutes(30);
		}
		else
		{
			// Keep the game responsive during a temporary directory outage. The
			// next retry occurs well before the two-hour lease can expire.
			OnlineNextLeaseRenewal = std::chrono::steady_clock::now() + std::chrono::minutes(2);
			DPrintf(DMSG_NOTIFY, "Online session renewal failed: %s\n", error.c_str());
		}
	}
	MaybeRecoverOnlineHost();
	OnlinePumpDirectProbes();
	OnlinePumpJoinAdmissions();
	OnlinePumpSnapshotHandoff();
	OnlinePumpSnapshotAcknowledgements();
	OnlinePumpStartupJoinControls();
#endif
	if (cmd == CMD_SEND)
	{
		if (RemoteClient >= 0)
			SendPacket(Connected[RemoteClient].Address);
	}
	else if (cmd == CMD_GET)
	{
		GetPacket();
	}
}

static void SetClientAck(size_t client, size_t from, bool add)
{
	const uint64_t bit = (uint64_t)1u << from;
	if (add)
		Connected[client].InfoAck |= bit;
	else
		Connected[client].InfoAck &= ~bit;
}

static bool ClientGotAck(size_t client, size_t from)
{
	return (Connected[client].InfoAck & ((uint64_t)1u << from));
}

static bool GetConnection(sockaddr_in& from)
{
	GetPacket(&from);
	return NetBufferLength > 0;
}

static void RejectConnection(const sockaddr_in& to, ENetConnectType reason)
{
	NetBuffer[0] = NCMD_SETUP;
	NetBuffer[1] = reason;
	NetBufferLength = 2u;

	SendPacket(to);
}

static void AddClientConnection(const sockaddr_in& from, int client)
{
	Connected[client].Status = CSTAT_CONNECTING;
	Connected[client].Address = from;
	NetworkClients += client;
	I_NetLog("Client %u joined the lobby", client);
	I_NetClientUpdated(client);

	// Make sure any ready clients are marked as needing the new client's info.
	for (int i = 1; i < MaxClients; ++i)
	{
		if (Connected[i].Status == CSTAT_READY)
		{
			Connected[i].Status = CSTAT_WAITING;
			I_NetClientUpdated(i);
		}
	}
}

static void RemoveClientConnection(int client)
{
	I_NetClientDisconnected(client);
	I_ClearClient(client);
	NetworkClients -= client;
	I_NetLog("Client %u left the lobby", client);

	// Let everyone else know the user left as well.
	NetBuffer[0] = NCMD_SETUP;
	NetBuffer[1] = PRE_DISCONNECT;
	NetBuffer[2] = client;
	NetBufferLength = 3u;

	for (int i = 1; i < MaxClients; ++i)
	{
		if (Connected[i].Status == CSTAT_NONE)
			continue;

		SetClientAck(i, client, false);
		for (int i = 0; i < 4; ++i)
			SendPacket(Connected[i].Address);
	}
}

void HandleIncomingConnection()
{
	if (consoleplayer != Net_Arbitrator || RemoteClient == -1)
		return;

	if (Connected[RemoteClient].Status == CSTAT_READY)
	{
		NetBuffer[0] = NCMD_SETUP;
		NetBuffer[1] = PRE_GO;
		NetBuffer[2] = NetMode;
		NetBufferLength = 3u;
		SendPacket(Connected[RemoteClient].Address);
	}
}

static bool Host_CheckForConnections(void* connected)
{
	const bool forceStarting = I_ShouldStartNetGame();
	const bool hasPassword = strlen(net_password) > 0;
	int* connectedPlayers = (int*)connected;

	TArray<int> toBoot = {};
	I_GetKickClients(toBoot);
	for (auto client : toBoot)
	{
		if (client <= 0 || Connected[client].Status == CSTAT_NONE)
			continue;

		sockaddr_in booted = Connected[client].Address;

		RemoveClientConnection(client);
		--*connectedPlayers;
		I_NetUpdatePlayers(*connectedPlayers, MaxClients);

		RejectConnection(booted, PRE_KICKED);
	}

	I_GetBanClients(toBoot);
	for (auto client : toBoot)
	{
		if (client <= 0 || Connected[client].Status == CSTAT_NONE)
			continue;

		sockaddr_in booted = Connected[client].Address;
		BannedConnections.Push(booted);

		RemoveClientConnection(client);
		--*connectedPlayers;
		I_NetUpdatePlayers(*connectedPlayers, MaxClients);

		RejectConnection(booted, PRE_BANNED);
	}

	sockaddr_in from;
	while (GetConnection(from))
	{
		if (NetBuffer[0] == NCMD_EXIT)
		{
			if (RemoteClient >= 0)
			{
				RemoveClientConnection(RemoteClient);
				--*connectedPlayers;
				I_NetUpdatePlayers(*connectedPlayers, MaxClients);
			}

			continue;
		}

		if (NetBuffer[0] != NCMD_SETUP)
			continue;

		if (NetBuffer[1] == PRE_CONNECT)
		{
			if (RemoteClient >= 0)
				continue;

			uint8_t* engineInfo = &NetBuffer[2];
			size_t banned = 0u;
			for (; banned < BannedConnections.Size(); ++banned)
			{
				if (BannedConnections[banned].sin_addr.s_addr == from.sin_addr.s_addr)
					break;
			}

			if (banned < BannedConnections.Size())
			{
				RejectConnection(from, PRE_BANNED);
			}
			else if (!Net_VerifyEngine(engineInfo))
			{
				RejectConnection(from, PRE_WRONG_ENGINE);
			}
			else if (*connectedPlayers >= MaxClients)
			{
				RejectConnection(from, PRE_FULL);
			}
			else if (forceStarting)
			{
				RejectConnection(from, PRE_IN_PROGRESS);
			}
			// Truncated packets carry no password bytes, and the compare must stay inside the datagram.
			else if (hasPassword && (NetBufferLength < 6u || strncmp(net_password, (const char*)&NetBuffer[5], NetBufferLength - 5u)))
			{
				RejectConnection(from, PRE_WRONG_PASSWORD);
			}
			else
			{
				int free = 1;
				for (; free < MaxClients; ++free)
				{
					if (Connected[free].Status == CSTAT_NONE)
						break;
				}

				AddClientConnection(from, free);
				++*connectedPlayers;
				I_NetUpdatePlayers(*connectedPlayers, MaxClients);
			}
		}
		else if (RemoteClient < 0)
		{
			// The remaining setup commands index Connected[] with RemoteClient; ignore unknown senders.
			continue;
		}
		else if (NetBuffer[1] == PRE_USER_INFO)
		{
			if (Connected[RemoteClient].Status == CSTAT_CONNECTING)
			{
				if (NetBufferLength < 2u)
					continue; // Truncated packet; there is no user info to parse.
				// Size the stream to the actual datagram so the parser can't consume stale bytes.
				TArrayView<uint8_t> stream = TArrayView(&NetBuffer[2], NetBufferLength - 2u);
				Net_ReadUserInfo(RemoteClient, stream);
				Connected[RemoteClient].Status = CSTAT_WAITING;
				I_NetClientConnected(RemoteClient, 16u);
			}
		}
		else if (NetBuffer[1] == PRE_USER_INFO_ACK)
		{
			// The ack bit index goes into a 64-bit mask; reject out-of-range values.
			if (NetBuffer[2] >= MAXPLAYERS)
				continue;
			SetClientAck(RemoteClient, NetBuffer[2], true);
		}
		else if (NetBuffer[1] == PRE_GAME_INFO_ACK)
		{
			Connected[RemoteClient].bHasGameInfo = true;
		}
	}

	const size_t addrSize = sizeof(sockaddr_in);
	bool ready = true;
	NetBuffer[0] = NCMD_SETUP;
	for (int client = 1; client < MaxClients; ++client)
	{
		auto& con = Connected[client];
		// If we're starting before the lobby is full, only check against connected clients.
		if (con.Status != CSTAT_READY && (!forceStarting || con.Status != CSTAT_NONE))
			ready = false;

		if (con.Status == CSTAT_CONNECTING)
		{
			NetBuffer[1] = PRE_CONNECT_ACK;
			NetBuffer[2] = client;
			NetBuffer[3] = *connectedPlayers;
			NetBuffer[4] = MaxClients;
			NetBufferLength = 5u;
			SendPacket(con.Address);
		}
		else if (con.Status == CSTAT_WAITING)
		{
			bool clientReady = true;
			if (!ClientGotAck(client, client))
			{
				NetBuffer[1] = PRE_USER_INFO_ACK;
				NetBufferLength = 2u;
				SendPacket(con.Address);
				clientReady = false;
			}

			if (!con.bHasGameInfo)
			{
				NetBuffer[1] = PRE_GAME_INFO;
				NetBuffer[2] = TicDup;
				memcpy(&NetBuffer[3], GameID, 8);
				NetBufferLength = 11u;

				TArrayView<uint8_t> stream = TArrayView(&NetBuffer[NetBufferLength], MAX_MSGLEN - NetBufferLength);
				Net_SetGameInfo(stream);
				NetBufferLength += stream.Data() - &NetBuffer[NetBufferLength];
				SendPacket(con.Address);
				clientReady = false;
			}

			NetBuffer[1] = PRE_USER_INFO;
			for (int i = 0; i < MaxClients; ++i)
			{
				if (i == client || Connected[i].Status == CSTAT_NONE)
					continue;

				if (!ClientGotAck(client, i))
				{
					if (Connected[i].Status >= CSTAT_WAITING)
					{
						NetBuffer[2] = uint8_t(i);
						NetBufferLength = 3u;
						// Client will already have the host connection information.
						if (i > 0)
						{
							memcpy(&NetBuffer[NetBufferLength], &Connected[i].Address, addrSize);
							NetBufferLength += addrSize;
						}

						TArrayView<uint8_t> stream = TArrayView(&NetBuffer[NetBufferLength], MAX_MSGLEN - NetBufferLength);
						Net_SetUserInfo(i, stream);
						NetBufferLength += stream.Data() - &NetBuffer[NetBufferLength];
						SendPacket(con.Address);
					}
					clientReady = false;
				}
			}

			if (clientReady)
			{
				con.Status = CSTAT_READY;
				I_NetClientUpdated(client);
			}
		}
		else if (con.Status == CSTAT_READY)
		{
			NetBuffer[1] = PRE_HEARTBEAT;
			NetBuffer[2] = *connectedPlayers;
			NetBuffer[3] = MaxClients;
			NetBufferLength = 4u;
			SendPacket(con.Address);
		}
	}

	return ready && (*connectedPlayers >= MaxClients || forceStarting);
}

// Boon TODO: Add cool down between sends
static void SendAbort()
{
	NetBuffer[0] = NCMD_EXIT;
	NetBufferLength = 1u;

	if (consoleplayer == 0)
	{
		for (int client = 1; client < MaxClients; ++client)
		{
			if (Connected[client].Status != CSTAT_NONE)
				SendPacket(Connected[client].Address);
		}
	}
	else
	{
		SendPacket(Connected[0].Address);
	}
}

static bool HostGame(int arg, bool forcedNetMode)
{
	if (arg >= Args->NumArgs() || !(MaxClients = atoi(Args->GetArg(arg))))
	{	// No player count specified, assume 2
		MaxClients = 2u;
	}

	if ((unsigned)MaxClients > MAXPLAYERS)
		I_FatalError("Cannot host a game with %u players. The limit is currently %lu", MaxClients, MAXPLAYERS);

	GenerateGameID();
	NetworkClients += 0;
	Connected[consoleplayer].Status = CSTAT_READY;
	Net_SetupUserInfo();

	// If only 1 player, don't bother starting the network
	if (MaxClients == 1)
	{
		TicDup = 1u;
		multiplayer = true;
		return true;
	}

	StartNetwork(false);
#ifdef BIASEDDOOM_ONLINE
	// The lobby can spend time before its first normal packet poll. Register the
	// host's just-bound UDP endpoint immediately so a review-approved guest can
	// request encrypted content during that window; the periodic heartbeat in
	// I_NetCmd keeps the registration alive thereafter.
	SendOnlineHostLivenessHeartbeat();
#endif
	I_NetInit("Waiting for other players...", true);
	I_NetUpdatePlayers(1, MaxClients);
	I_NetClientConnected(0u, 16u);

	// Wait for the lobby to be full.
	int connectedPlayers = 1;
	if (!I_NetLoop(Host_CheckForConnections, (void*)&connectedPlayers))
	{
		SendAbort();
		throw CExitEvent(0);
	}

	// Now go
	I_NetMessage("Starting game");
	I_NetDone();

	// If the player force started with only themselves in the lobby, start the game
	// immediately.
	if (connectedPlayers == 1)
	{
		CloseNetwork();
		MaxClients = 1;
		TicDup = 1u;
		return true;
	}

	if (!forcedNetMode)
	{
		if (MaxClients < 3)
			NetMode = NET_PeerToPeer;
		else if (!ClientsOnSameNetwork())
			NetMode = NET_PacketServer;
	}

	I_NetLog("Go");

	NetBuffer[0] = NCMD_SETUP;
	NetBuffer[1] = PRE_GO;
	NetBuffer[2] = NetMode;
	NetBufferLength = 3u;
	for (size_t client = 1u; client < (size_t)MaxClients; ++client)
	{
		if (Connected[client].Status != CSTAT_NONE)
			SendPacket(Connected[client].Address);
	}

	I_NetLog("Total players: %u", connectedPlayers);

	return true;
}

static bool Guest_ContactHost(void* unused)
{
	// Listen for a reply.
	const size_t addrSize = sizeof(sockaddr_in);
	sockaddr_in from;
	while (GetConnection(from))
	{
		if (RemoteClient != 0)
			continue;

		if (NetBuffer[0] == NCMD_EXIT)
			I_NetError("The host cancelled the game");

		if (NetBuffer[0] != NCMD_SETUP)
			continue;

		if (NetBuffer[1] == PRE_HEARTBEAT)
		{
			// Keep the host-supplied client count within the Connected[] array.
			MaxClients = clamp<int>(NetBuffer[3], 1, MAXPLAYERS);
			I_NetUpdatePlayers(NetBuffer[2], MaxClients);
		}
		else if (NetBuffer[1] == PRE_DISCONNECT)
		{
			if (NetBuffer[2] >= MAXPLAYERS)
				continue; // Host sent an out-of-range client index; ignore the packet.
			I_ClearClient(NetBuffer[2]);
			NetworkClients -= NetBuffer[2];
			SetClientAck(consoleplayer, NetBuffer[2], false);
			I_NetClientDisconnected(NetBuffer[2]);
		}
		else if (NetBuffer[1] == PRE_FULL)
		{
			I_NetError("The game is full");
		}
		else if (NetBuffer[1] == PRE_IN_PROGRESS)
		{
			I_NetError("The game has already started. This session accepts players only while its lobby is open.");
		}
		else if (NetBuffer[1] == PRE_WRONG_PASSWORD)
		{
			I_NetError("Invalid password");
		}
		else if (NetBuffer[1] == PRE_WRONG_ENGINE)
		{
			I_NetError("Engine version does not match the host's engine version");
		}
		else if (NetBuffer[1] == PRE_INVALID_FILES)
		{
			I_NetError("Files do not match the host's files");
		}
		else if (NetBuffer[1] == PRE_KICKED)
		{
			I_NetError("You have been kicked from the game");
		}
		else if (NetBuffer[1] == PRE_BANNED)
		{
			I_NetError("You have been banned from the game");
		}
		else if (NetBuffer[1] == PRE_CONNECT_ACK)
		{
			if (consoleplayer == -1)
			{
				if (NetBuffer[2] >= MAXPLAYERS)
					continue; // Host assigned an out-of-range client index; ignore the packet.
				NetworkClients += 0;
				Connected[0].Status = CSTAT_WAITING;
				I_NetClientUpdated(0);

				consoleplayer = NetBuffer[2];
				NetworkClients += consoleplayer;
				Connected[consoleplayer].Status = CSTAT_CONNECTING;
				Net_SetupUserInfo();

				// Keep the host-supplied client count within the Connected[] array.
				MaxClients = clamp<int>(NetBuffer[4], 1, MAXPLAYERS);
				I_NetMessage("Sending game information");
				I_NetUpdatePlayers(NetBuffer[3], MaxClients);
				I_NetClientConnected(consoleplayer, 16u);
			}
		}
		else if (NetBuffer[1] == PRE_USER_INFO_ACK)
		{
			// The host will only ever send us this to confirm they've gotten our data.
			SetClientAck(consoleplayer, consoleplayer, true);
			if (Connected[consoleplayer].Status == CSTAT_CONNECTING)
			{
				Connected[consoleplayer].Status = CSTAT_WAITING;
				I_NetClientUpdated(consoleplayer);
				I_NetMessage("Waiting for game to start");
			}

			NetBuffer[0] = NCMD_SETUP;
			NetBuffer[1] = PRE_USER_INFO_ACK;
			NetBuffer[2] = consoleplayer;
			NetBufferLength = 3u;
			SendPacket(from);
		}
		else if (NetBuffer[1] == PRE_GAME_INFO)
		{
			if (NetBufferLength < 11u)
				continue; // Truncated game info; ignore instead of reading past the datagram.
			if (!Connected[consoleplayer].bHasGameInfo)
			{
				TicDup = clamp<int>(NetBuffer[2], 1, MAXTICDUP);
				memcpy(GameID, &NetBuffer[3], 8);
				// Size the stream to the actual datagram so the parser can't consume stale bytes.
				TArrayView<uint8_t> stream = TArrayView(&NetBuffer[11], NetBufferLength - 11u);
				Net_ReadGameInfo(stream);
				Connected[consoleplayer].bHasGameInfo = true;
			}

			NetBuffer[0] = NCMD_SETUP;
			NetBuffer[1] = PRE_GAME_INFO_ACK;
			NetBufferLength = 2u;
			SendPacket(from);
		}
		else if (NetBuffer[1] == PRE_USER_INFO)
		{
			const int c = NetBuffer[2];
			if ((size_t)c >= MAXPLAYERS)
				continue; // Out-of-range client index; Connected[] and the ack bitmask can't hold it.
			if (!ClientGotAck(consoleplayer, c))
			{
				const size_t byte = 3u + (c > 0 ? addrSize : 0u);
				if (NetBufferLength < byte)
					continue; // Truncated packet; ignore instead of reading past the datagram.
				NetworkClients += c;
				if (c > 0)
				{
					Connected[c].Status = CSTAT_WAITING;
					memcpy(&Connected[c].Address, &NetBuffer[3u], addrSize);
				}
				else
				{
					Connected[c].Status = CSTAT_READY;
				}
				// Size the stream to the actual datagram so the parser can't consume stale bytes.
				TArrayView<uint8_t> stream = TArrayView(&NetBuffer[byte], NetBufferLength - byte);
				Net_ReadUserInfo(c, stream);
				SetClientAck(consoleplayer, c, true);

				I_NetClientConnected(c, 16u);
			}

			NetBuffer[0] = NCMD_SETUP;
			NetBuffer[1] = PRE_USER_INFO_ACK;
			NetBuffer[2] = c;
			NetBufferLength = 3u;
			SendPacket(from);
		}
		else if (NetBuffer[1] == PRE_GO)
		{
			NetMode = static_cast<ENetMode>(NetBuffer[2]);
			I_NetMessage("Starting game");
			I_NetLog("Received GO");
			return true;
		}
	}

	NetBuffer[0] = NCMD_SETUP;
	if (consoleplayer == -1)
	{
		NetBuffer[1] = PRE_CONNECT;
		uint8_t* engineInfo = &NetBuffer[2];
		const size_t end = 2u + Net_SetEngineInfo(engineInfo);
		const size_t passSize = strlen(net_password) + 1;
		memcpy(&NetBuffer[end], net_password, passSize);
		NetBufferLength = end + passSize;
		SendPacket(Connected[0].Address);
	}
	else
	{
		auto& con = Connected[consoleplayer];
		if (con.Status == CSTAT_CONNECTING)
		{
			NetBuffer[1] = PRE_USER_INFO;
			NetBufferLength = 2u;

			TArrayView<uint8_t> stream = TArrayView(&NetBuffer[NetBufferLength], MAX_MSGLEN - NetBufferLength);
			Net_SetUserInfo(consoleplayer, stream);
			NetBufferLength += stream.Data() - &NetBuffer[NetBufferLength];
			SendPacket(Connected[0].Address);
		}
		else if (con.Status == CSTAT_WAITING)
		{
			NetBuffer[1] = PRE_HEARTBEAT;
			NetBufferLength = 2u;
			SendPacket(Connected[0].Address);
		}
	}

	return false;
}

static bool JoinGame(int arg)
{
	if (arg >= Args->NumArgs()
		|| Args->GetArg(arg)[0] == '-' || Args->GetArg(arg)[0] == '+')
	{
		I_FatalError("You need to specify the host machine's address");
	}

	consoleplayer = -1;
	StartNetwork(true);

	// Host is always client 0. Online preparation installed a synthetic relay
	// address; direct/LAN clients still resolve the typed address here.
#ifdef BIASEDDOOM_ONLINE
	if (!OnlineSessionActive)
#endif
		BuildAddress(Connected[0].Address, Args->GetArg(arg));
	Connected[0].Status = CSTAT_CONNECTING;

	I_NetInit("Contacting host...", false);
	I_NetUpdatePlayers(0u, MaxClients);
	I_NetClientUpdated(0);

	if (!I_NetLoop(Guest_ContactHost, nullptr))
	{
		SendAbort();
		throw CExitEvent(0);
	}

	for (int i = 1u; i < MaxClients; ++i)
	{
		if (Connected[i].Status != CSTAT_NONE)
			Connected[i].Status = CSTAT_READY;
	}

	I_NetLog("Total players: %u", MaxClients);
	I_NetDone();

	return true;
}

#ifdef BIASEDDOOM_ONLINE
// A join-in-progress guest intentionally never enters the legacy PRE_* setup
// exchange. Its assigned slot does not exist until the host has streamed a
// verified snapshot and later emits the roster event into the normal tic
// history. This small startup loop is therefore limited to authenticated
// join/snapshot controls; it leaves world construction to d_main.
static bool Guest_ReceiveOnlineSnapshot(void* unused)
{
	(void)unused;
	sockaddr_in from = {};
	while (GetConnection(from))
	{
		// Online control and snapshot frames are consumed by ReceiveOnlinePacket.
		// A legacy datagram is never valid on this startup-only route.
	}
	if (!OnlineStartupJoin.Failure.empty())
		I_NetError(OnlineStartupJoin.Failure.c_str());
	if (OnlineStartupJoin.ReadyForRestore)
		return true;

	const auto now = std::chrono::steady_clock::now();
	if (now >= OnlineStartupJoin.StartupDeadline)
		I_NetError("Timed out while synchronizing the online game in progress");
	if (OnlineStartupJoin.Accepted || now < OnlineStartupJoin.NextRequest)
		return false;
	std::vector<uint8_t> frame;
	std::string error;
	if (!BdOnline::OnlineEncodeJoinRequest(OnlineStartupJoin.Request, frame, error) ||
		!SendOnlinePacket(Connected[0].Address, frame.data(), frame.size(), error))
	{
		DPrintf(DMSG_NOTIFY, "Online join request could not be relayed: %s\n", error.c_str());
	}
	OnlineStartupJoin.NextRequest = now + std::chrono::milliseconds(500);
	return false;
}

static bool JoinOnlineGameInProgress()
{
	BdOnline::FOnlineRelayTransport::FPeerId host = {};
	if (!OnlineSessionActive || OnlineSessionIsHost || !OnlineJoinInProgress ||
		!BdOnline::FOnlineRelayTransport::ParsePeerId(OnlineSession.HostPeerId, host))
	{
		I_FatalError("Online join-in-progress is not configured for this session");
	}

	// User info is captured before a slot exists locally. The authenticated host
	// validates and projects exactly this single compact string into the reserved
	// slot immediately before it begins the handoff.
	Net_SetupUserInfo();
	const FString userInfo = D_GetUserInfoStrings(0, true);
	if (userInfo.Len() == 0 || userInfo.Len() + 1 > 4096)
		I_FatalError("Could not prepare local player information for online joining");

	OnlineStartupJoin.Reset();
	OnlineStartupJoin.Active = true;
	OnlineStartupJoin.Request.Epoch = OnlineSession.Epoch;
	OnlineStartupJoin.Request.UserInfo.assign(reinterpret_cast<const uint8_t*>(userInfo.GetChars()),
		reinterpret_cast<const uint8_t*>(userInfo.GetChars()) + userInfo.Len() + 1);
	std::string requestError;
	if (!BdOnline::OnlineGenerateJoinRequestId(OnlineStartupJoin.Request.RequestId, requestError))
		I_FatalError("Could not prepare online join request: %s", requestError.c_str());
	OnlineStartupJoin.StartupDeadline = std::chrono::steady_clock::now() + std::chrono::minutes(5);

	consoleplayer = -1;
	StartNetwork(true);
	Connected[0].Address = OnlineAddressForPeer(host);
	Connected[0].Status = CSTAT_CONNECTING;
	MaxClients = clamp<int>(static_cast<int>(OnlineSession.Manifest.MaxPlayers), 1, MAXPLAYERS);

	I_NetInit("Synchronizing game in progress...", false);
	I_NetMessage("Requesting an authenticated world snapshot");
	if (!I_NetLoop(Guest_ReceiveOnlineSnapshot, nullptr))
	{
		CloseNetwork();
		throw CExitEvent(0);
	}
	I_NetLog("Online snapshot received; restoring game state");
	I_NetDone();
	return true;
}
#endif

//
// I_InitNetwork
//
// Returns true if packet server mode might be a good idea.
//
bool I_InitNetwork()
{
	// set up for network
	const char* v = Args->CheckValue("-dup");
	if (v != nullptr)
		TicDup = clamp<int>(atoi(v), 1, MAXTICDUP);

	v = Args->CheckValue("-port");
	if (v != nullptr)
	{
		GamePort = atoi(v);
		Printf("Using alternate port %d\n", GamePort);
	}

	v = Args->CheckValue("-netmode");
	if (v != nullptr)
		NetMode = atoi(v) ? NET_PacketServer : NET_PeerToPeer;

	net_password = Args->CheckValue("-password");

	// Parse network game options:
	//	player 1: -host <numplayers> or -online-host [numplayers]
	//	player x: -join <host address> or -online-join <join code>
	int arg = -1;
	const int onlineHost = Args->CheckParm("-online-host");
	const int onlineJoin = Args->CheckParm("-online-join");
	if ((onlineHost || onlineJoin) && (Args->CheckParm("-host") || Args->CheckParm("-join") || (onlineHost && onlineJoin)))
	{
		I_FatalError("Choose exactly one multiplayer start method: direct/LAN or online");
	}
#ifndef BIASEDDOOM_ONLINE
	if (onlineHost || onlineJoin)
		I_FatalError("This build was made without online multiplayer support");
#else
	if (onlineHost || onlineJoin)
	{
		std::string onlineError;
		if (onlineHost && !PrepareOnlineHost(onlineHost, onlineError))
			I_FatalError("Could not start online session: %s", onlineError.c_str());
		if (onlineJoin && !PrepareOnlineJoin(onlineJoin, onlineError))
			I_FatalError("Could not join online session: %s", onlineError.c_str());
	}
#endif
	if ((arg = Args->CheckParm("-host")) || onlineHost)
	{
		const int hostArg = onlineHost ? onlineHost : arg;
		if (!HostGame(hostArg + 1, v != nullptr || onlineHost != 0))
			return false;
	}
	else if ((arg = Args->CheckParm("-join")) || onlineJoin)
	{
		const int joinArg = onlineJoin ? onlineJoin : arg;
	#ifdef BIASEDDOOM_ONLINE
		if (onlineJoin && OnlineJoinInProgress)
		{
			if (!JoinOnlineGameInProgress())
				return false;
		}
		else
	#endif
		if (!JoinGame(joinArg + 1))
			return false;
	}
	else
	{
		// single player game
		GenerateGameID();
		TicDup = 1;
		NetworkClients += 0;
		Connected[0].Status = CSTAT_READY;
		Net_SetupUserInfo();
	}

	bGameStarted = true;
#ifdef BIASEDDOOM_ONLINE
	// The directory needs an explicit, durable lobby-to-gameplay transition.
	// Snapshot-capable sessions remain eligible for the separate bounded
	// join-in-progress path; leave failures are non-fatal locally so a running
	// host is never interrupted merely because the listing could not update.
	if (OnlineSessionActive && OnlineSessionIsHost && OnlineServiceClient)
	{
		std::string onlineError;
		if (!OnlineServiceClient->CloseLobby(OnlineSession, onlineError))
			DPrintf(DMSG_ERROR, "Online lobby could not be closed after game start: %s\n", onlineError.c_str());
	}
#endif
	return true;
}

#ifdef __WIN32__
const char* neterror()
{
	static char neterr[16];
	int			code;

	switch (code = WSAGetLastError()) {
		case WSAEACCES:				return "EACCES";
		case WSAEADDRINUSE:			return "EADDRINUSE";
		case WSAEADDRNOTAVAIL:		return "EADDRNOTAVAIL";
		case WSAEAFNOSUPPORT:		return "EAFNOSUPPORT";
		case WSAEALREADY:			return "EALREADY";
		case WSAECONNABORTED:		return "ECONNABORTED";
		case WSAECONNREFUSED:		return "ECONNREFUSED";
		case WSAECONNRESET:			return "ECONNRESET";
		case WSAEDESTADDRREQ:		return "EDESTADDRREQ";
		case WSAEFAULT:				return "EFAULT";
		case WSAEHOSTDOWN:			return "EHOSTDOWN";
		case WSAEHOSTUNREACH:		return "EHOSTUNREACH";
		case WSAEINPROGRESS:		return "EINPROGRESS";
		case WSAEINTR:				return "EINTR";
		case WSAEINVAL:				return "EINVAL";
		case WSAEISCONN:			return "EISCONN";
		case WSAEMFILE:				return "EMFILE";
		case WSAEMSGSIZE:			return "EMSGSIZE";
		case WSAENETDOWN:			return "ENETDOWN";
		case WSAENETRESET:			return "ENETRESET";
		case WSAENETUNREACH:		return "ENETUNREACH";
		case WSAENOBUFS:			return "ENOBUFS";
		case WSAENOPROTOOPT:		return "ENOPROTOOPT";
		case WSAENOTCONN:			return "ENOTCONN";
		case WSAENOTSOCK:			return "ENOTSOCK";
		case WSAEOPNOTSUPP:			return "EOPNOTSUPP";
		case WSAEPFNOSUPPORT:		return "EPFNOSUPPORT";
		case WSAEPROCLIM:			return "EPROCLIM";
		case WSAEPROTONOSUPPORT:	return "EPROTONOSUPPORT";
		case WSAEPROTOTYPE:			return "EPROTOTYPE";
		case WSAESHUTDOWN:			return "ESHUTDOWN";
		case WSAESOCKTNOSUPPORT:	return "ESOCKTNOSUPPORT";
		case WSAETIMEDOUT:			return "ETIMEDOUT";
		case WSAEWOULDBLOCK:		return "EWOULDBLOCK";
		case WSAHOST_NOT_FOUND:		return "HOST_NOT_FOUND";
		case WSANOTINITIALISED:		return "NOTINITIALISED";
		case WSANO_DATA:			return "NO_DATA";
		case WSANO_RECOVERY:		return "NO_RECOVERY";
		case WSASYSNOTREADY:		return "SYSNOTREADY";
		case WSATRY_AGAIN:			return "TRY_AGAIN";
		case WSAVERNOTSUPPORTED:	return "VERNOTSUPPORTED";
		case WSAEDISCON:			return "EDISCON";

		default:
			mysnprintf(neterr, countof(neterr), "%d", code);
			return neterr;
	}
}
#endif
