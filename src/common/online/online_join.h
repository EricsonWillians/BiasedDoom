#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace BdOnline
{

// Admission controls are intentionally separate from snapshot frames. The
// relay authenticates their sender label before this parser sees them, while
// the request and transfer identities make delayed packets harmless across a
// retry, reconnect, or a new handoff to the same anonymous peer.
inline constexpr std::array<uint8_t, 4> OnlineJoinFrameMagic = { 'B', 'D', 'J', 'P' };
inline constexpr uint8_t OnlineJoinFrameVersion = 1;
inline constexpr size_t OnlineMaxJoinUserInfoBytes = 4096;
// Keep this protocol limit explicit at the standalone boundary; the engine's
// MAXPLAYERS currently has the same value, and i_net validates it again before
// applying an accepted slot.
inline constexpr uint8_t OnlineMaxJoinPlayerSlots = 64;

enum class EOnlineJoinFrameType : uint8_t
{
	Request = 1,
	Accept = 2,
	Ready = 3,
	Reject = 4,
	Activated = 5,
};

enum class EOnlineJoinRejectReason : uint8_t
{
	Busy = 1,
	SessionFull = 2,
	InvalidRequest = 3,
	SnapshotUnavailable = 4,
};

// Compact userinfo is an opaque, NUL-terminated legacy userinfo payload. It
// is never rendered from this control frame; the host validates and applies it
// only while forming its ordinary deterministic roster event.
struct FOnlineJoinRequestFrame
{
	uint32_t Epoch = 0;
	uint64_t RequestId = 0;
	std::vector<uint8_t> UserInfo;
};

struct FOnlineJoinAcceptFrame
{
	uint32_t Epoch = 0;
	uint64_t RequestId = 0;
	uint64_t TransferId = 0;
	uint8_t PlayerSlot = 0;
};

// Sent only after the guest restored the accepted archive and replayed the
// complete authenticated range. The host still queues a normal roster event;
// this control is merely proof that it may safely schedule that event.
struct FOnlineJoinReadyFrame
{
	uint32_t Epoch = 0;
	uint64_t RequestId = 0;
	uint64_t TransferId = 0;
	uint8_t PlayerSlot = 0;
};

// Sent after the joining process replayed the host's ordinary roster event
// and switched its local console player to the assigned slot. It is the
// handoff's terminal acknowledgement; it is not an extra roster mutation.
struct FOnlineJoinActivatedFrame
{
	uint32_t Epoch = 0;
	uint64_t RequestId = 0;
	uint64_t TransferId = 0;
	uint8_t PlayerSlot = 0;
};

struct FOnlineJoinRejectFrame
{
	uint32_t Epoch = 0;
	uint64_t RequestId = 0;
	EOnlineJoinRejectReason Reason = EOnlineJoinRejectReason::InvalidRequest;
};

bool OnlineIsJoinFrame(const uint8_t* bytes, size_t size);
// Generates a fresh nonzero request identity with libsodium. A request ID is
// independent from the host-generated snapshot transfer ID, so a lost join
// request can be retried without making a delayed acceptance bind to a newer
// attempt.
bool OnlineGenerateJoinRequestId(uint64_t& requestId, std::string& error);
bool OnlineEncodeJoinRequest(const FOnlineJoinRequestFrame& request, std::vector<uint8_t>& frame, std::string& error);
bool OnlineDecodeJoinRequest(const uint8_t* bytes, size_t size, FOnlineJoinRequestFrame& request, std::string& error);
bool OnlineEncodeJoinAccept(const FOnlineJoinAcceptFrame& accept, std::vector<uint8_t>& frame, std::string& error);
bool OnlineDecodeJoinAccept(const uint8_t* bytes, size_t size, FOnlineJoinAcceptFrame& accept, std::string& error);
bool OnlineEncodeJoinReady(const FOnlineJoinReadyFrame& ready, std::vector<uint8_t>& frame, std::string& error);
bool OnlineDecodeJoinReady(const uint8_t* bytes, size_t size, FOnlineJoinReadyFrame& ready, std::string& error);
bool OnlineEncodeJoinActivated(const FOnlineJoinActivatedFrame& activated, std::vector<uint8_t>& frame, std::string& error);
bool OnlineDecodeJoinActivated(const uint8_t* bytes, size_t size, FOnlineJoinActivatedFrame& activated, std::string& error);
bool OnlineEncodeJoinReject(const FOnlineJoinRejectFrame& reject, std::vector<uint8_t>& frame, std::string& error);
bool OnlineDecodeJoinReject(const uint8_t* bytes, size_t size, FOnlineJoinRejectFrame& reject, std::string& error);

} // namespace BdOnline
