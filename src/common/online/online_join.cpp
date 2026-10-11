#include "online_join.h"

#include <sodium.h>

#include <cstring>
#include <limits>

namespace BdOnline
{
namespace
{

void WriteU16(std::vector<uint8_t>& output, uint16_t value)
{
	output.push_back(static_cast<uint8_t>(value));
	output.push_back(static_cast<uint8_t>(value >> 8));
}

void WriteU32(std::vector<uint8_t>& output, uint32_t value)
{
	for (size_t index = 0; index < 4; ++index)
		output.push_back(static_cast<uint8_t>(value >> (index * 8)));
}

void WriteU64(std::vector<uint8_t>& output, uint64_t value)
{
	for (size_t index = 0; index < 8; ++index)
		output.push_back(static_cast<uint8_t>(value >> (index * 8)));
}

uint16_t ReadU16(const uint8_t* bytes)
{
	return static_cast<uint16_t>(bytes[0]) | static_cast<uint16_t>(bytes[1]) << 8;
}

uint32_t ReadU32(const uint8_t* bytes)
{
	uint32_t value = 0;
	for (size_t index = 0; index < 4; ++index)
		value |= static_cast<uint32_t>(bytes[index]) << (index * 8);
	return value;
}

uint64_t ReadU64(const uint8_t* bytes)
{
	uint64_t value = 0;
	for (size_t index = 0; index < 8; ++index)
		value |= static_cast<uint64_t>(bytes[index]) << (index * 8);
	return value;
}

bool IsFrame(const uint8_t* bytes, size_t size, EOnlineJoinFrameType type)
{
	return bytes != nullptr && size >= 6 &&
		std::memcmp(bytes, OnlineJoinFrameMagic.data(), OnlineJoinFrameMagic.size()) == 0 &&
		bytes[4] == OnlineJoinFrameVersion && bytes[5] == static_cast<uint8_t>(type);
}

bool IsValidSlot(uint8_t slot)
{
	return slot < OnlineMaxJoinPlayerSlots;
}

bool IsValidRejectReason(uint8_t reason)
{
	return reason >= static_cast<uint8_t>(EOnlineJoinRejectReason::Busy) &&
		reason <= static_cast<uint8_t>(EOnlineJoinRejectReason::SnapshotUnavailable);
}

// D_ReadUserInfoStrings deliberately consumes one NUL-terminated legacy
// string. A second NUL or trailing bytes here would leave data in its stream
// and let an admission frame desynchronize the following event. Validate the
// small grammar at the wire boundary, before the engine roster ever sees it.
bool IsValidUserInfo(const uint8_t* bytes, size_t size)
{
	return bytes != nullptr && size > 1 && size <= OnlineMaxJoinUserInfoBytes &&
		bytes[0] == '\\' && bytes[size - 1] == 0 &&
		std::memchr(bytes, 0, size - 1) == nullptr;
}

void BeginFrame(std::vector<uint8_t>& frame, EOnlineJoinFrameType type)
{
	frame.clear();
	frame.insert(frame.end(), OnlineJoinFrameMagic.begin(), OnlineJoinFrameMagic.end());
	frame.push_back(OnlineJoinFrameVersion);
	frame.push_back(static_cast<uint8_t>(type));
}

} // namespace

bool OnlineIsJoinFrame(const uint8_t* bytes, size_t size)
{
	return bytes != nullptr && size >= 6 &&
		std::memcmp(bytes, OnlineJoinFrameMagic.data(), OnlineJoinFrameMagic.size()) == 0 &&
		bytes[4] == OnlineJoinFrameVersion;
}

bool OnlineGenerateJoinRequestId(uint64_t& requestId, std::string& error)
{
	requestId = 0;
	if (sodium_init() < 0)
	{
		error = "libsodium could not initialize the online join request identity";
		return false;
	}
	// A zero request ID is reserved as an invalid/uninitialized value on the
	// wire. Draw again only for that one value instead of weakening the identity
	// with a clock or process-local counter.
	do
	{
		randombytes_buf(&requestId, sizeof(requestId));
	}
	while (requestId == 0);
	return true;
}

bool OnlineEncodeJoinRequest(const FOnlineJoinRequestFrame& request, std::vector<uint8_t>& frame, std::string& error)
{
	if (request.Epoch == 0 || request.RequestId == 0 ||
		request.UserInfo.size() > std::numeric_limits<uint16_t>::max() ||
		!IsValidUserInfo(request.UserInfo.data(), request.UserInfo.size()))
	{
		error = "online join request is invalid";
		return false;
	}
	frame.reserve(6 + 4 + 8 + 2 + request.UserInfo.size());
	BeginFrame(frame, EOnlineJoinFrameType::Request);
	WriteU32(frame, request.Epoch);
	WriteU64(frame, request.RequestId);
	WriteU16(frame, static_cast<uint16_t>(request.UserInfo.size()));
	frame.insert(frame.end(), request.UserInfo.begin(), request.UserInfo.end());
	return true;
}

bool OnlineDecodeJoinRequest(const uint8_t* bytes, size_t size, FOnlineJoinRequestFrame& request, std::string& error)
{
	constexpr size_t HeaderSize = 6 + 4 + 8 + 2;
	if (!IsFrame(bytes, size, EOnlineJoinFrameType::Request) || size < HeaderSize)
	{
		error = "online join request frame is malformed";
		return false;
	}
	const uint16_t payloadSize = ReadU16(bytes + 18);
	if (payloadSize == 0 || size != HeaderSize + payloadSize ||
		!IsValidUserInfo(bytes + HeaderSize, payloadSize))
	{
		error = "online join request frame has an invalid userinfo payload";
		return false;
	}
	request = {};
	request.Epoch = ReadU32(bytes + 6);
	request.RequestId = ReadU64(bytes + 10);
	request.UserInfo.assign(bytes + HeaderSize, bytes + size);
	if (request.Epoch == 0 || request.RequestId == 0)
	{
		error = "online join request frame has an invalid identity";
		return false;
	}
	return true;
}

bool OnlineEncodeJoinAccept(const FOnlineJoinAcceptFrame& accept, std::vector<uint8_t>& frame, std::string& error)
{
	if (accept.Epoch == 0 || accept.RequestId == 0 || accept.TransferId == 0 || !IsValidSlot(accept.PlayerSlot))
	{
		error = "online join acceptance is invalid";
		return false;
	}
	frame.reserve(6 + 4 + 8 + 8 + 1);
	BeginFrame(frame, EOnlineJoinFrameType::Accept);
	WriteU32(frame, accept.Epoch);
	WriteU64(frame, accept.RequestId);
	WriteU64(frame, accept.TransferId);
	frame.push_back(accept.PlayerSlot);
	return true;
}

bool OnlineDecodeJoinAccept(const uint8_t* bytes, size_t size, FOnlineJoinAcceptFrame& accept, std::string& error)
{
	constexpr size_t FrameSize = 6 + 4 + 8 + 8 + 1;
	if (!IsFrame(bytes, size, EOnlineJoinFrameType::Accept) || size != FrameSize)
	{
		error = "online join acceptance frame is malformed";
		return false;
	}
	accept = {};
	accept.Epoch = ReadU32(bytes + 6);
	accept.RequestId = ReadU64(bytes + 10);
	accept.TransferId = ReadU64(bytes + 18);
	accept.PlayerSlot = bytes[26];
	if (accept.Epoch == 0 || accept.RequestId == 0 || accept.TransferId == 0 || !IsValidSlot(accept.PlayerSlot))
	{
		error = "online join acceptance frame is out of range";
		return false;
	}
	return true;
}

bool OnlineEncodeJoinReady(const FOnlineJoinReadyFrame& ready, std::vector<uint8_t>& frame, std::string& error)
{
	if (ready.Epoch == 0 || ready.RequestId == 0 || ready.TransferId == 0 || !IsValidSlot(ready.PlayerSlot))
	{
		error = "online join readiness is invalid";
		return false;
	}
	frame.reserve(6 + 4 + 8 + 8 + 1);
	BeginFrame(frame, EOnlineJoinFrameType::Ready);
	WriteU32(frame, ready.Epoch);
	WriteU64(frame, ready.RequestId);
	WriteU64(frame, ready.TransferId);
	frame.push_back(ready.PlayerSlot);
	return true;
}

bool OnlineDecodeJoinReady(const uint8_t* bytes, size_t size, FOnlineJoinReadyFrame& ready, std::string& error)
{
	constexpr size_t FrameSize = 6 + 4 + 8 + 8 + 1;
	if (!IsFrame(bytes, size, EOnlineJoinFrameType::Ready) || size != FrameSize)
	{
		error = "online join readiness frame is malformed";
		return false;
	}
	ready = {};
	ready.Epoch = ReadU32(bytes + 6);
	ready.RequestId = ReadU64(bytes + 10);
	ready.TransferId = ReadU64(bytes + 18);
	ready.PlayerSlot = bytes[26];
	if (ready.Epoch == 0 || ready.RequestId == 0 || ready.TransferId == 0 || !IsValidSlot(ready.PlayerSlot))
	{
		error = "online join readiness frame is out of range";
		return false;
	}
	return true;
}

bool OnlineEncodeJoinActivated(const FOnlineJoinActivatedFrame& activated, std::vector<uint8_t>& frame, std::string& error)
{
	if (activated.Epoch == 0 || activated.RequestId == 0 || activated.TransferId == 0 || !IsValidSlot(activated.PlayerSlot))
	{
		error = "online join activation acknowledgement is invalid";
		return false;
	}
	frame.reserve(6 + 4 + 8 + 8 + 1);
	BeginFrame(frame, EOnlineJoinFrameType::Activated);
	WriteU32(frame, activated.Epoch);
	WriteU64(frame, activated.RequestId);
	WriteU64(frame, activated.TransferId);
	frame.push_back(activated.PlayerSlot);
	return true;
}

bool OnlineDecodeJoinActivated(const uint8_t* bytes, size_t size, FOnlineJoinActivatedFrame& activated, std::string& error)
{
	constexpr size_t FrameSize = 6 + 4 + 8 + 8 + 1;
	if (!IsFrame(bytes, size, EOnlineJoinFrameType::Activated) || size != FrameSize)
	{
		error = "online join activation acknowledgement is malformed";
		return false;
	}
	activated = {};
	activated.Epoch = ReadU32(bytes + 6);
	activated.RequestId = ReadU64(bytes + 10);
	activated.TransferId = ReadU64(bytes + 18);
	activated.PlayerSlot = bytes[26];
	if (activated.Epoch == 0 || activated.RequestId == 0 || activated.TransferId == 0 || !IsValidSlot(activated.PlayerSlot))
	{
		error = "online join activation acknowledgement is out of range";
		return false;
	}
	return true;
}

bool OnlineEncodeJoinReject(const FOnlineJoinRejectFrame& reject, std::vector<uint8_t>& frame, std::string& error)
{
	const uint8_t reason = static_cast<uint8_t>(reject.Reason);
	if (reject.Epoch == 0 || reject.RequestId == 0 || !IsValidRejectReason(reason))
	{
		error = "online join rejection is invalid";
		return false;
	}
	frame.reserve(6 + 4 + 8 + 1);
	BeginFrame(frame, EOnlineJoinFrameType::Reject);
	WriteU32(frame, reject.Epoch);
	WriteU64(frame, reject.RequestId);
	frame.push_back(reason);
	return true;
}

bool OnlineDecodeJoinReject(const uint8_t* bytes, size_t size, FOnlineJoinRejectFrame& reject, std::string& error)
{
	constexpr size_t FrameSize = 6 + 4 + 8 + 1;
	if (!IsFrame(bytes, size, EOnlineJoinFrameType::Reject) || size != FrameSize)
	{
		error = "online join rejection frame is malformed";
		return false;
	}
	reject = {};
	reject.Epoch = ReadU32(bytes + 6);
	reject.RequestId = ReadU64(bytes + 10);
	const uint8_t reason = bytes[18];
	if (reject.Epoch == 0 || reject.RequestId == 0 || !IsValidRejectReason(reason))
	{
		error = "online join rejection frame is out of range";
		return false;
	}
	reject.Reason = static_cast<EOnlineJoinRejectReason>(reason);
	return true;
}

} // namespace BdOnline
