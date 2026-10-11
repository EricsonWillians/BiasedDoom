#include "common/online/online_snapshot.h"
#include "common/online/online_join.h"

#include <array>
#include <cstddef>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <random>
#include <string>
#include <vector>

using namespace BdOnline;

namespace
{

[[noreturn]] void Fail(const char* message)
{
	std::cerr << "FAIL: " << message << "\n";
	std::exit(1);
}

void Require(bool value, const char* message)
{
	if (!value)
		Fail(message);
}

void FuzzDecoder(const std::vector<uint8_t>& bytes)
{
	std::string error;
	FOnlineSnapshotBundle bundle;
	if (OnlineDecodeSnapshotBundle(bytes.data(), bytes.size(), bundle, error))
	{
		Require(bundle.Validate(error) && !bundle.Components.empty() && bundle.Components.size() <= OnlineMaxSnapshotComponents,
			"snapshot bundle decoder returned an unsafe bundle");
	}

	FOnlineSnapshotMetadata metadata;
	size_t chunkCount = 0;
	if (OnlineDecodeSnapshotBegin(bytes.data(), bytes.size(), metadata, chunkCount, error))
	{
		Require(metadata.Validate(error), "snapshot Begin decoder returned invalid metadata");
		Require(chunkCount > 0 && chunkCount <= OnlineMaxSnapshotChunks,
			"snapshot Begin decoder returned an unsafe chunk count");
	}

	FOnlineSnapshotChunkFrame chunk;
	if (OnlineDecodeSnapshotChunk(bytes.data(), bytes.size(), chunk, error))
	{
		Require(chunk.Epoch != 0 && chunk.TransferId != 0 && !chunk.Bytes.empty() && chunk.Bytes.size() <= OnlineSnapshotChunkBytes,
			"snapshot chunk decoder returned an unsafe frame");
	}

	FOnlineSnapshotAckFrame acknowledgement;
	if (OnlineDecodeSnapshotAck(bytes.data(), bytes.size(), acknowledgement, error))
	{
		Require(acknowledgement.Epoch != 0 && acknowledgement.TransferId != 0 &&
			acknowledgement.Digest.size() == OnlineDigestBytes * 2,
			"snapshot acknowledgement decoder returned an unsafe frame");
	}

	FOnlineSnapshotReplayFrame replay;
	if (OnlineDecodeSnapshotReplayTic(bytes.data(), bytes.size(), replay, error))
	{
		Require(replay.Epoch != 0 && replay.TransferId != 0 && replay.Tic > replay.SnapshotTic && !replay.Bytes.empty() &&
			replay.Bytes.size() <= OnlineMaxTicRecordBytes,
			"snapshot replay decoder returned an unsafe frame");
	}

	FOnlineSnapshotCompleteFrame complete;
	if (OnlineDecodeSnapshotComplete(bytes.data(), bytes.size(), complete, error))
	{
		Require(complete.Epoch != 0 && complete.TransferId != 0 && complete.LiveTic >= complete.SnapshotTic &&
			complete.Digest.size() == OnlineDigestBytes * 2,
			"snapshot completion fence decoder returned an unsafe frame");
	}

	FOnlineSnapshotCompleteAckFrame completeAcknowledgement;
	if (OnlineDecodeSnapshotCompleteAck(bytes.data(), bytes.size(), completeAcknowledgement, error))
	{
		Require(completeAcknowledgement.Epoch != 0 && completeAcknowledgement.TransferId != 0 &&
			completeAcknowledgement.LiveTic >= completeAcknowledgement.SnapshotTic &&
			completeAcknowledgement.Digest.size() == OnlineDigestBytes * 2,
			"snapshot completion acknowledgement decoder returned an unsafe frame");
	}

	FOnlineSnapshotCancelFrame cancellation;
	if (OnlineDecodeSnapshotCancel(bytes.data(), bytes.size(), cancellation, error))
	{
		Require(cancellation.Epoch != 0 && cancellation.TransferId != 0 &&
			cancellation.Digest.size() == OnlineDigestBytes * 2 &&
			static_cast<uint8_t>(cancellation.Reason) >= static_cast<uint8_t>(EOnlineSnapshotCancelReason::HostStopped) &&
			static_cast<uint8_t>(cancellation.Reason) <= static_cast<uint8_t>(EOnlineSnapshotCancelReason::EpochChanged),
			"snapshot cancellation decoder returned an unsafe frame");
	}

	FOnlineSnapshotReplayAckFrame replayAcknowledgement;
	if (OnlineDecodeSnapshotReplayAck(bytes.data(), bytes.size(), replayAcknowledgement, error))
	{
		Require(replayAcknowledgement.Epoch != 0 && replayAcknowledgement.TransferId != 0 &&
			replayAcknowledgement.AcknowledgedTic >= replayAcknowledgement.SnapshotTic &&
			replayAcknowledgement.Digest.size() == OnlineDigestBytes * 2,
			"snapshot replay acknowledgement decoder returned an unsafe frame");
	}

	FOnlineJoinRequestFrame joinRequest;
	if (OnlineDecodeJoinRequest(bytes.data(), bytes.size(), joinRequest, error))
	{
		Require(joinRequest.Epoch != 0 && joinRequest.RequestId != 0 && !joinRequest.UserInfo.empty() &&
			joinRequest.UserInfo.size() <= OnlineMaxJoinUserInfoBytes && joinRequest.UserInfo.front() == '\\' &&
			joinRequest.UserInfo.back() == 0 && std::memchr(joinRequest.UserInfo.data(), 0,
				joinRequest.UserInfo.size() - 1) == nullptr,
			"online join request decoder returned an unsafe frame");
	}
	FOnlineJoinAcceptFrame joinAccept;
	if (OnlineDecodeJoinAccept(bytes.data(), bytes.size(), joinAccept, error))
	{
		Require(joinAccept.Epoch != 0 && joinAccept.RequestId != 0 && joinAccept.TransferId != 0 &&
			joinAccept.PlayerSlot < OnlineMaxJoinPlayerSlots,
			"online join acceptance decoder returned an unsafe frame");
	}
	FOnlineJoinReadyFrame joinReady;
	if (OnlineDecodeJoinReady(bytes.data(), bytes.size(), joinReady, error))
	{
		Require(joinReady.Epoch != 0 && joinReady.RequestId != 0 && joinReady.TransferId != 0 &&
			joinReady.PlayerSlot < OnlineMaxJoinPlayerSlots,
			"online join readiness decoder returned an unsafe frame");
	}
	FOnlineJoinActivatedFrame joinActivated;
	if (OnlineDecodeJoinActivated(bytes.data(), bytes.size(), joinActivated, error))
	{
		Require(joinActivated.Epoch != 0 && joinActivated.RequestId != 0 && joinActivated.TransferId != 0 &&
			joinActivated.PlayerSlot < OnlineMaxJoinPlayerSlots,
			"online join activation acknowledgement decoder returned an unsafe frame");
	}
	FOnlineJoinRejectFrame joinReject;
	if (OnlineDecodeJoinReject(bytes.data(), bytes.size(), joinReject, error))
	{
		Require(joinReject.Epoch != 0 && joinReject.RequestId != 0 &&
			static_cast<uint8_t>(joinReject.Reason) >= static_cast<uint8_t>(EOnlineJoinRejectReason::Busy) &&
			static_cast<uint8_t>(joinReject.Reason) <= static_cast<uint8_t>(EOnlineJoinRejectReason::SnapshotUnavailable),
			"online join rejection decoder returned an unsafe frame");
	}
}

} // namespace

int main()
{
	std::mt19937_64 random(0x42444a535f46555aull);
	std::uniform_int_distribution<size_t> sizeDistribution(0, OnlineSnapshotChunkBytes + 96);
	std::uniform_int_distribution<unsigned> byteDistribution(0, 255);

	// Fuzz independently generated malformed datagrams. Decoders must remain
	// bounded and may only report a structurally valid output on success.
	for (size_t iteration = 0; iteration < 10000; ++iteration)
	{
		std::vector<uint8_t> bytes(sizeDistribution(random));
		for (auto& byte : bytes)
			byte = static_cast<uint8_t>(byteDistribution(random));
		FuzzDecoder(bytes);
	}

	// Mutate every wire format that the host and guest deliberately accept.
	// This catches offsets and length checks that random noise is unlikely to
	// reach while keeping the test deterministic and inexpensive in CI.
	const std::array<uint8_t, 3> archive = { 7, 11, 13 };
	FOnlineSnapshotMetadata metadata;
	metadata.SerializerVersion = 41519;
	metadata.Epoch = 2;
	metadata.SnapshotTic = 99;
	metadata.TransferId = 0x4a4f494e534e4150ull;
	metadata.TotalBytes = archive.size();
	metadata.UncompressedBytes = archive.size();
	metadata.CompressionMethod = FileSys::METHOD_STORED;
	metadata.CRC32 = 0x7a11c0deu;
	std::string error;
	Require(OnlineHashSnapshot(archive.data(), archive.size(), metadata.Digest, error),
		"could not create a valid snapshot fuzz fixture");

	std::vector<std::vector<uint8_t>> frames;
	std::vector<uint8_t> frame;
	FOnlineSnapshotBundle bundle;
	bundle.MapName = "MAP01";
	FOnlineSnapshotComponent world;
	world.Type = EOnlineSnapshotComponentType::World;
	world.Name = "MAP01";
	world.UncompressedBytes = archive.size();
	world.CompressionMethod = FileSys::METHOD_STORED;
	world.CRC32 = 0x1234u;
	world.Bytes.assign(archive.begin(), archive.end());
	bundle.Components.push_back(world);
	FOnlineSnapshotComponent globals;
	globals.Type = EOnlineSnapshotComponentType::Globals;
	globals.Name = "globals";
	globals.UncompressedBytes = 1;
	globals.CompressionMethod = FileSys::METHOD_STORED;
	globals.CRC32 = 0x5678u;
	globals.Bytes = { 1 };
	bundle.Components.push_back(globals);
	Require(OnlineEncodeSnapshotBundle(bundle, frame, error), "could not encode snapshot bundle fixture");
	frames.push_back(frame);
	Require(OnlineEncodeSnapshotBegin(metadata, 1, frame, error), "could not encode snapshot Begin fixture");
	frames.push_back(frame);
	FOnlineSnapshotChunkFrame chunk;
	chunk.Epoch = metadata.Epoch;
	chunk.SnapshotTic = metadata.SnapshotTic;
	chunk.TransferId = metadata.TransferId;
	chunk.Digest = metadata.Digest;
	chunk.Bytes.assign(archive.begin(), archive.end());
	Require(OnlineEncodeSnapshotChunk(chunk, frame, error), "could not encode snapshot chunk fixture");
	frames.push_back(frame);
	FOnlineSnapshotAckFrame acknowledgement;
	acknowledgement.Epoch = metadata.Epoch;
	acknowledgement.SnapshotTic = metadata.SnapshotTic;
	acknowledgement.TransferId = metadata.TransferId;
	acknowledgement.Digest = metadata.Digest;
	Require(OnlineEncodeSnapshotAck(acknowledgement, frame, error), "could not encode snapshot acknowledgement fixture");
	frames.push_back(frame);
	FOnlineSnapshotReplayFrame replay;
	replay.Epoch = metadata.Epoch;
	replay.SnapshotTic = metadata.SnapshotTic;
	replay.TransferId = metadata.TransferId;
	replay.Tic = metadata.SnapshotTic + 1;
	replay.Digest = metadata.Digest;
	replay.Bytes = { 42 };
	Require(OnlineEncodeSnapshotReplayTic(replay, frame, error), "could not encode snapshot replay fixture");
	frames.push_back(frame);
	FOnlineSnapshotCompleteFrame complete;
	complete.Epoch = metadata.Epoch;
	complete.SnapshotTic = metadata.SnapshotTic;
	complete.TransferId = metadata.TransferId;
	complete.LiveTic = metadata.SnapshotTic + 1;
	complete.Digest = metadata.Digest;
	Require(OnlineEncodeSnapshotComplete(complete, frame, error), "could not encode snapshot completion fixture");
	frames.push_back(frame);
	FOnlineSnapshotCompleteAckFrame completionAcknowledgement;
	completionAcknowledgement.Epoch = metadata.Epoch;
	completionAcknowledgement.SnapshotTic = metadata.SnapshotTic;
	completionAcknowledgement.TransferId = metadata.TransferId;
	completionAcknowledgement.LiveTic = metadata.SnapshotTic + 1;
	completionAcknowledgement.Digest = metadata.Digest;
	Require(OnlineEncodeSnapshotCompleteAck(completionAcknowledgement, frame, error),
		"could not encode snapshot completion acknowledgement fixture");
	frames.push_back(frame);
	FOnlineSnapshotCancelFrame cancellation;
	cancellation.Epoch = metadata.Epoch;
	cancellation.SnapshotTic = metadata.SnapshotTic;
	cancellation.TransferId = metadata.TransferId;
	cancellation.Digest = metadata.Digest;
	cancellation.Reason = EOnlineSnapshotCancelReason::ReplayExpired;
	Require(OnlineEncodeSnapshotCancel(cancellation, frame, error), "could not encode snapshot cancellation fixture");
	frames.push_back(frame);
	FOnlineSnapshotReplayAckFrame replayAcknowledgement;
	replayAcknowledgement.Epoch = metadata.Epoch;
	replayAcknowledgement.SnapshotTic = metadata.SnapshotTic;
	replayAcknowledgement.TransferId = metadata.TransferId;
	replayAcknowledgement.AcknowledgedTic = metadata.SnapshotTic + 1;
	replayAcknowledgement.Digest = metadata.Digest;
	Require(OnlineEncodeSnapshotReplayAck(replayAcknowledgement, frame, error),
		"could not encode snapshot replay acknowledgement fixture");
	frames.push_back(frame);
	FOnlineJoinRequestFrame joinRequest;
	joinRequest.Epoch = metadata.Epoch;
	joinRequest.RequestId = 0x4a4f494e52455131ull;
	joinRequest.UserInfo = { '\\', 'n', 'a', 'm', 'e', '\\', 'F', 'u', 'z', 'z', 0 };
	Require(OnlineEncodeJoinRequest(joinRequest, frame, error), "could not encode online join request fixture");
	frames.push_back(frame);
	FOnlineJoinAcceptFrame joinAccept = { metadata.Epoch, joinRequest.RequestId, metadata.TransferId, 1 };
	Require(OnlineEncodeJoinAccept(joinAccept, frame, error), "could not encode online join acceptance fixture");
	frames.push_back(frame);
	FOnlineJoinReadyFrame joinReady = { metadata.Epoch, joinRequest.RequestId, metadata.TransferId, 1 };
	Require(OnlineEncodeJoinReady(joinReady, frame, error), "could not encode online join readiness fixture");
	frames.push_back(frame);
	FOnlineJoinActivatedFrame joinActivated = { metadata.Epoch, joinRequest.RequestId, metadata.TransferId, 1 };
	Require(OnlineEncodeJoinActivated(joinActivated, frame, error), "could not encode online join activation acknowledgement fixture");
	frames.push_back(frame);
	FOnlineJoinRejectFrame joinReject = { metadata.Epoch, joinRequest.RequestId, EOnlineJoinRejectReason::Busy };
	Require(OnlineEncodeJoinReject(joinReject, frame, error), "could not encode online join rejection fixture");
	frames.push_back(frame);

	for (const auto& validFrame : frames)
	{
		for (size_t position = 0; position < validFrame.size(); ++position)
		{
			std::vector<uint8_t> mutated = validFrame;
			mutated[position] ^= 0xa5;
			FuzzDecoder(mutated);
		}
		for (size_t length = 0; length < validFrame.size(); ++length)
		{
			std::vector<uint8_t> truncated(validFrame.begin(), validFrame.begin() + static_cast<std::ptrdiff_t>(length));
			FuzzDecoder(truncated);
		}
	}

	std::cout << "PASS: online snapshot malformed-frame fuzz regression\n";
	return 0;
}
