#ifndef __I_NET_H__
#define __I_NET_H__

#include <stdint.h>
#include <string>
#include <vector>
#include "tarray.h"

inline constexpr size_t MAXPLAYERS = 64u;

enum ENetConstants
{
	BACKUPTICS = 35 * 5,	// Remember up to 5 seconds of data.
	MAXTICDUP = 3,
	MAXSENDTICS = 35 * 1,	// Only send up to 1 second of data at a time.
	STABILITYTICS = 17,
	LOCALCMDTICS = (BACKUPTICS * MAXTICDUP),
	MAX_MSGLEN = 14000,
};

enum ENetCommand
{
	CMD_NONE,
	CMD_SEND,
	CMD_GET,
};

enum ENetFlags
{
	NCMD_EXIT = 0x80,		// Client has left the game
	NCMD_RETRANSMIT = 0x40,		// 
	NCMD_SETUP = 0x20,		// Guest is letting the host know who it is
	NCMD_LEVELREADY = 0x10,		// After loading a level, guests send this over to the host who then sends it back after all are received
	NCMD_QUITTERS = 0x08,		// Client is getting info about one or more players quitting (packet server only)
	NCMD_COMPRESSED = 0x04,		// Remainder of packet is compressed
	NCMD_LATENCYACK = 0x02,		// A latency packet was just read, so let the sender know.
	NCMD_LATENCY = 0x01,		// Latency packet, used for measuring RTT.		
};

enum ENetMode
{
	NET_PeerToPeer,
	NET_PacketServer
};

struct FClientStack : public TArray<int>
{
	inline bool InGame(int i) const { return Find(i) < Size(); }

	void operator+=(const int i)
	{
		if (!InGame(i))
			SortedInsert(i);
	}

	void operator-=(const int i)
	{
		Delete(Find(i));
	}
};

extern bool netgame, multiplayer;
extern int consoleplayer;
extern int Net_Arbitrator;
extern FClientStack NetworkClients;
extern ENetMode NetMode;
extern uint8_t NetBuffer[MAX_MSGLEN];
extern size_t NetBufferLength;
extern uint8_t TicDup;
extern int RemoteClient;
extern int MaxClients;

bool I_InitNetwork();

// Runs before the resource filesystem is initialized for `-online-host`.
// It canonicalizes directory-backed host mods into session-local PK3 files so
// the host advertises and mounts precisely the same archive bytes.
bool I_OnlinePrepareHostContent(std::vector<std::string>& allwads, std::string& error);

// Runs before the resource filesystem is initialized for `-online-join`.
// It resolves the host manifest, obtains explicitly accepted content through
// the encrypted relay, and replaces only transferable launch resources with
// verified cache entries.
bool I_OnlinePreflightContent(std::vector<std::string>& allwads, std::string& error);
// Retrieves the host identity and the exact transferable content plan without
// reserving a player slot or downloading anything. The launcher uses this to
// bind approval to the reviewed host and content root before it adds
// -online-accept-mods.
struct FOnlineContentReviewPackage
{
	std::string DisplayName;
	std::string Digest;
	uint64_t Size = 0;
};

struct FOnlineJoinReview
{
	std::string HostPeerId;
	std::string SessionTitle;
	// The launcher binds an approval to this exact root. A second control-plane
	// lookup immediately before launch must match it before bytes are fetched.
	std::string ContentRootDigest;
	uint64_t TotalBytes = 0;
	std::vector<FOnlineContentReviewPackage> Packages;
};

bool I_OnlineInspectJoin(const std::string& serviceUrl, const std::string& joinCode,
	const std::string& password, FOnlineJoinReview& review, std::string& error);
// Submits a user-selected public-listing moderation signal using the same
// locally persisted anonymous device identity as hosting and joining. It is
// deliberately available to the launcher before a game is launched.
bool I_OnlineReportSession(const std::string& serviceUrl, const std::string& sessionId,
	const std::string& reason, std::string& error);
// Graceful online host migration is prepared through the directory before the
// legacy host-exit packet is emitted. The returned successor remains a legacy
// player slot, so d_net.cpp can keep its proven handoff framing.
bool I_OnlinePrepareHostMigration(int& successor);
void I_OnlineSendPreparedHostMigration();
void I_OnlineApplyHostMigration(int departingHost, int successor);
// Begins one bounded encrypted snapshot handoff to an already admitted relay
// peer. This is intentionally separate from the legacy lobby handshake: a
// caller may only make the player live after the peer confirms both the
// archive and its contiguous replay fence.
bool I_OnlineStartSnapshotHandoff(const std::string& peerId, std::string& error);
// Startup join-in-progress receives its encrypted archive before the regular
// first-map path. d_main calls this after Net_Initialize() so restoration can
// reuse ordinary serializer and event-stream ownership instead of creating a
// parallel world loader.
bool I_OnlineHasPendingJoinRestore();
bool I_OnlineRestorePendingJoin(std::string& error);
// Advances post-restore replay records before NetUpdate builds local input.
// A failure means the disposable joining world must not enter live play.
bool I_OnlineAdvancePendingJoinReplay(std::string& error);
// d_net calls this only while applying the host's deterministic roster event.
// It makes the restored local process own its assigned slot and returns the
// final encrypted activation acknowledgement to the host.
void I_OnlineActivateJoiningPlayer(int player);
// Records the fully accepted packet-server input for one simulation tic. It
// is deliberately called at the tic boundary before game code consumes the
// command stream, giving a future snapshot handoff a bounded, canonical
// catch-up history without changing normal legacy netplay.
void I_OnlineRecordSimulationTic(uint64_t tic);
void I_ClearClient(size_t client);
void I_NetCmd(ENetCommand cmd);
void I_NetDone();
void HandleIncomingConnection();
void CloseNetwork();

#endif
