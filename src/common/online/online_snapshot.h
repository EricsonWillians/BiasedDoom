#pragma once

#include "online_session.h"
#include "common/filesystem/include/fs_decompress.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace BdOnline
{

// A world snapshot is deliberately transported separately from a legacy game
// packet. The game packet protocol predates join-in-progress and cannot make
// a partial, stale, or oversized save safe to restore. This small, versioned
// envelope is shared by the eventual engine serializer bridge and the relay.
// The transfer identifier was added with protocol version 3. It distinguishes
// two handoffs that happen to capture byte-identical state at the same tic:
// delayed authenticated relay packets from the old attempt must never advance
// or cancel the newer attempt.
inline constexpr uint32_t OnlineSnapshotProtocolVersion = 3;
inline constexpr uint64_t OnlineMaxSnapshotBytes = 256ull * 1024 * 1024;
inline constexpr size_t OnlineSnapshotChunkBytes = 12 * 1024;
inline constexpr size_t OnlineMaxSnapshotChunks =
	static_cast<size_t>((OnlineMaxSnapshotBytes + OnlineSnapshotChunkBytes - 1) / OnlineSnapshotChunkBytes);
inline constexpr size_t OnlineDefaultTicHistoryBytes = 8 * 1024 * 1024;
// Replay tics travel as one authenticated relay datagram. Keep their payload
// below the same conservative 12 KiB budget as snapshot chunks so the replay
// header, encrypted tag, and relay recipient label still fit in the 16 KiB
// transport envelope; a 64 KiB logical record would be impossible to relay.
inline constexpr size_t OnlineMaxTicRecordBytes = 12 * 1024;
inline constexpr std::array<uint8_t, 4> OnlineSnapshotFrameMagic = { 'B', 'D', 'J', 'S' };
inline constexpr uint8_t OnlineSnapshotFrameVersion = 2;

enum class EOnlineSnapshotFrameType : uint8_t
{
	Begin = 1,
	Chunk = 2,
	ReplayTic = 3,
	Complete = 4,
	Cancel = 5,
	Acknowledge = 6,
	ReplayAcknowledge = 7,
	CompleteAcknowledge = 8,
};

// Ack ChunkIndex reserves two values outside the bounded chunk range. A
// receiver first acknowledges a Begin frame, then every validated chunk, and
// finally confirms that it has the complete archive. This makes the transfer
// restart-safe over the UDP relay without treating delivery as reliable.
inline constexpr uint32_t OnlineSnapshotBeginAcknowledgement = UINT32_MAX;
inline constexpr uint32_t OnlineSnapshotCompleteAcknowledgement = UINT32_MAX - 1;

struct FOnlineSnapshotMetadata
{
	uint32_t Version = OnlineSnapshotProtocolVersion;
	// The caller sets this to the engine save/serializer format version. It is
	// distinct from the online wire protocol so an engine upgrade cannot be
	// accidentally restored by an older client.
	uint32_t SerializerVersion = 0;
	uint32_t Epoch = 0;
	uint64_t SnapshotTic = 0;
	// Fresh, non-zero random value generated for every host handoff. This is
	// transport state rather than a property of the archived world, but lives
	// in the signed/encrypted Begin metadata so every following frame can bind
	// itself to one exact attempt.
	uint64_t TransferId = 0;
	// TotalBytes is the compressed (or stored) byte sequence transferred in
	// chunks. The remaining fields describe the existing FCompressedBuffer
	// representation so a receiver can validate it before it ever asks the
	// engine serializer to decompress or parse it.
	uint64_t TotalBytes = 0;
	uint64_t UncompressedBytes = 0;
	uint32_t CompressionMethod = 0;
	uint32_t CRC32 = 0;
	std::string Digest; // lower-case BLAKE2b-256 hex over the exact transferred bytes

	bool Validate(std::string& error) const;
};

// The wire snapshot transports a bounded bundle instead of assuming that the
// current level archive alone is sufficient. The engine bridge supplies the
// current world plus its global/hub/procedural serializer archives as named
// components. The enclosing FOnlineSnapshotMetadata authenticates the exact
// bundle bytes; component metadata preserves each archive's decompression and
// CRC contract for the engine-side restore step.
inline constexpr uint32_t OnlineSnapshotBundleVersion = 1;
inline constexpr size_t OnlineMaxSnapshotComponents = 64;

enum class EOnlineSnapshotComponentType : uint8_t
{
	World = 1,
	Globals = 2,
	ProceduralMap = 3,
	Hub = 4,
};

struct FOnlineSnapshotComponent
{
	EOnlineSnapshotComponentType Type = EOnlineSnapshotComponentType::World;
	std::string Name;
	uint64_t UncompressedBytes = 0;
	uint32_t CompressionMethod = 0;
	uint32_t CRC32 = 0;
	std::vector<uint8_t> Bytes;
};

struct FOnlineSnapshotBundle
{
	uint32_t Version = OnlineSnapshotBundleVersion;
	std::string MapName;
	std::vector<FOnlineSnapshotComponent> Components;

	bool Validate(std::string& error) const;
};

bool OnlineEncodeSnapshotBundle(const FOnlineSnapshotBundle& bundle, std::vector<uint8_t>& bytes, std::string& error);
bool OnlineDecodeSnapshotBundle(const uint8_t* bytes, size_t size, FOnlineSnapshotBundle& bundle, std::string& error);
// Decompresses every component only into bounded temporary memory and verifies
// its declared CRC. Call this after the authenticated outer bundle has been
// reassembled and before giving any component to the engine serializer.
bool OnlineValidateSnapshotBundleArchives(const FOnlineSnapshotBundle& bundle, std::string& error);

// A serializer archive is a byte-for-byte copy of the engine's existing
// FCompressedBuffer. These helpers provide the only permitted conversion at
// the online boundary: they retain size, compression, and CRC metadata, cap
// both compressed and expanded sizes, and never retain a pointer into level or
// hub storage. The output FCompressedBuffer must be value-initialized before
// its first use and Clean()'d by its owner when no longer needed.
bool OnlineMakeSnapshotMetadata(const FileSys::FCompressedBuffer& input, uint32_t serializerVersion,
	uint32_t epoch, uint64_t tic, FOnlineSnapshotMetadata& metadata, std::vector<uint8_t>& bytes, std::string& error);
// Wraps an already encoded snapshot bundle (or another bounded in-memory
// envelope) as a stored archive ready for the reliable snapshot sender. This
// avoids making callers fabricate an FCompressedBuffer around vector memory.
bool OnlineMakeStoredSnapshotMetadata(const uint8_t* input, size_t size, uint32_t serializerVersion,
	uint32_t epoch, uint64_t tic, FOnlineSnapshotMetadata& metadata, std::vector<uint8_t>& bytes, std::string& error);
bool OnlineRestoreSnapshotArchive(const FOnlineSnapshotMetadata& metadata, const uint8_t* bytes, size_t size,
	FileSys::FCompressedBuffer& output, std::string& error);

// Computes the digest used by FOnlineSnapshotMetadata. Kept public so the
// engine serializer bridge can hash an in-memory FDoomSerializer result before
// advertising it to a guest.
bool OnlineHashSnapshot(const uint8_t* bytes, size_t size, std::string& digest, std::string& error);

// Snapshot frames travel inside the already encrypted datagram/relay payload.
// They are self-identifying so i_net.cpp can consume them before passing a
// packet to the legacy protocol. All integer fields are little-endian.
bool OnlineIsSnapshotFrame(const uint8_t* bytes, size_t size);
bool OnlineEncodeSnapshotBegin(const FOnlineSnapshotMetadata& metadata, size_t chunkCount,
	std::vector<uint8_t>& frame, std::string& error);
bool OnlineDecodeSnapshotBegin(const uint8_t* bytes, size_t size, FOnlineSnapshotMetadata& metadata,
	size_t& chunkCount, std::string& error);

struct FOnlineSnapshotChunkFrame
{
	uint32_t Epoch = 0;
	uint64_t SnapshotTic = 0;
	uint64_t TransferId = 0;
	std::string Digest;
	size_t ChunkIndex = 0;
	std::vector<uint8_t> Bytes;
};

bool OnlineEncodeSnapshotChunk(const FOnlineSnapshotChunkFrame& chunk, std::vector<uint8_t>& frame, std::string& error);
bool OnlineDecodeSnapshotChunk(const uint8_t* bytes, size_t size, FOnlineSnapshotChunkFrame& chunk, std::string& error);

struct FOnlineSnapshotAckFrame
{
	uint32_t Epoch = 0;
	uint64_t SnapshotTic = 0;
	uint64_t TransferId = 0;
	std::string Digest;
	uint32_t ChunkIndex = OnlineSnapshotBeginAcknowledgement;
};

bool OnlineEncodeSnapshotAck(const FOnlineSnapshotAckFrame& acknowledgement, std::vector<uint8_t>& frame, std::string& error);
bool OnlineDecodeSnapshotAck(const uint8_t* bytes, size_t size, FOnlineSnapshotAckFrame& acknowledgement, std::string& error);

// A deterministic event/tic record captured after a world snapshot. It is
// intentionally independent from legacy packet framing so the eventual join
// bridge can validate epoch, snapshot identity, and bounded payload size
// before replaying anything into an offline-restored world.
struct FOnlineSnapshotReplayFrame
{
	uint32_t Epoch = 0;
	uint64_t SnapshotTic = 0;
	uint64_t TransferId = 0;
	uint64_t Tic = 0;
	std::string Digest;
	std::vector<uint8_t> Bytes;
};

bool OnlineEncodeSnapshotReplayTic(const FOnlineSnapshotReplayFrame& replay, std::vector<uint8_t>& frame, std::string& error);
bool OnlineDecodeSnapshotReplayTic(const uint8_t* bytes, size_t size, FOnlineSnapshotReplayFrame& replay, std::string& error);

// A completion fence is emitted only after the host has sent every replay
// record through LiveTic. It closes the otherwise timing-dependent gap
// between a validated archive and admission: the guest may restore only after
// it has the complete contiguous replay range [SnapshotTic + 1, LiveTic].
// It is intentionally distinct from the receiver's archive acknowledgement,
// which merely proves that the snapshot bytes arrived.
struct FOnlineSnapshotCompleteFrame
{
	uint32_t Epoch = 0;
	uint64_t SnapshotTic = 0;
	uint64_t TransferId = 0;
	uint64_t LiveTic = 0;
	std::string Digest;
};

bool OnlineEncodeSnapshotComplete(const FOnlineSnapshotCompleteFrame& complete,
	std::vector<uint8_t>& frame, std::string& error);
bool OnlineDecodeSnapshotComplete(const uint8_t* bytes, size_t size,
	FOnlineSnapshotCompleteFrame& complete, std::string& error);

// The completion fence itself is unreliable. A joining peer emits this only
// after it has both validated the archive and buffered every replay tic named
// by the fence, so the host never treats a one-way fence datagram as proof of
// a recoverable handoff.
struct FOnlineSnapshotCompleteAckFrame
{
	uint32_t Epoch = 0;
	uint64_t SnapshotTic = 0;
	uint64_t TransferId = 0;
	uint64_t LiveTic = 0;
	std::string Digest;
};

bool OnlineEncodeSnapshotCompleteAck(const FOnlineSnapshotCompleteAckFrame& acknowledgement,
	std::vector<uint8_t>& frame, std::string& error);
bool OnlineDecodeSnapshotCompleteAck(const uint8_t* bytes, size_t size,
	FOnlineSnapshotCompleteAckFrame& acknowledgement, std::string& error);

// Cancellation carries the same snapshot identity as every other handoff
// control frame, so an old relay retry cannot erase a newer transfer. Reasons
// are intentionally fixed codes rather than host-provided text; diagnostics
// remain local and untrusted peers cannot inject UI strings into a joiner.
enum class EOnlineSnapshotCancelReason : uint8_t
{
	HostStopped = 1,
	ReplayExpired = 2,
	TransferFailed = 3,
	EpochChanged = 4,
};

struct FOnlineSnapshotCancelFrame
{
	uint32_t Epoch = 0;
	uint64_t SnapshotTic = 0;
	uint64_t TransferId = 0;
	std::string Digest;
	EOnlineSnapshotCancelReason Reason = EOnlineSnapshotCancelReason::TransferFailed;
};

bool OnlineEncodeSnapshotCancel(const FOnlineSnapshotCancelFrame& cancel,
	std::vector<uint8_t>& frame, std::string& error);
bool OnlineDecodeSnapshotCancel(const uint8_t* bytes, size_t size,
	FOnlineSnapshotCancelFrame& cancel, std::string& error);

// Unlike snapshot chunks, replay tics remain live while the archive is in
// flight and can be lost independently. The guest acknowledges only its
// highest contiguous tic after SnapshotTic, allowing the host to resend a
// bounded history instead of declaring a completion fence over a gap.
struct FOnlineSnapshotReplayAckFrame
{
	uint32_t Epoch = 0;
	uint64_t SnapshotTic = 0;
	uint64_t TransferId = 0;
	uint64_t AcknowledgedTic = 0;
	std::string Digest;
};

bool OnlineEncodeSnapshotReplayAck(const FOnlineSnapshotReplayAckFrame& acknowledgement,
	std::vector<uint8_t>& frame, std::string& error);
bool OnlineDecodeSnapshotReplayAck(const uint8_t* bytes, size_t size,
	FOnlineSnapshotReplayAckFrame& acknowledgement, std::string& error);

// Reassembles a reliable chunk stream without permitting a malformed host (or
// a stale relay packet) to force an unbounded allocation. A duplicate chunk is
// accepted only when its bytes are exactly identical to the first copy.
class FOnlineSnapshotAssembler
{
public:
	bool Begin(const FOnlineSnapshotMetadata& metadata, size_t chunkCount, std::string& error);
	bool AcceptChunk(size_t chunkIndex, const uint8_t* bytes, size_t size, std::string& error);
	bool Finalize(std::vector<uint8_t>& snapshot, std::string& error);
	void Reset();

	bool IsActive() const { return Active; }
	bool IsComplete() const { return Active && ReceivedChunks == ExpectedChunks; }
	const FOnlineSnapshotMetadata& Metadata() const { return Header; }

private:
	FOnlineSnapshotMetadata Header;
	std::vector<uint8_t> Bytes;
	std::vector<uint8_t> Received;
	size_t ExpectedChunks = 0;
	size_t ReceivedChunks = 0;
	bool Active = false;
};

// Host-side reliable sender for the snapshot archive. Call NextFrame at a
// bounded cadence, deliver its result over the encrypted relay, and feed back
// authenticated acknowledgements through Acknowledge(). Until Begin is
// acknowledged it is retried; unacknowledged chunks are then round-robin
// retried indefinitely. The stream completes only after every chunk and the
// receiver's final archive confirmation have been authenticated.
class FOnlineSnapshotSender
{
public:
	bool Start(const FOnlineSnapshotMetadata& metadata, const uint8_t* bytes, size_t size, std::string& error);
	bool NextFrame(std::vector<uint8_t>& frame, std::string& error);
	bool Acknowledge(const FOnlineSnapshotAckFrame& acknowledgement, std::string& error);
	void Reset();

	bool IsActive() const { return Active; }
	bool IsComplete() const { return Active && CompleteAcknowledged; }
	const FOnlineSnapshotMetadata& Metadata() const { return Header; }

private:
	FOnlineSnapshotMetadata Header;
	std::vector<uint8_t> Bytes;
	std::vector<uint8_t> SentChunks;
	std::vector<uint8_t> AcknowledgedChunks;
	size_t NextChunk = 0;
	bool BeginSent = false;
	bool BeginAcknowledged = false;
	bool CompleteAcknowledged = false;
	bool Active = false;
};

// Guest-side counterpart to FOnlineSnapshotSender. It accepts frames only
// from the single declared archive, retains a completed archive long enough
// to answer retry traffic, and emits bounded acknowledgements. Callers send
// NextAcknowledgement() results through the authenticated relay; after the
// transfer has completed it cycles every chunk acknowledgement and the final
// archive acknowledgement so ordinary datagram reordering cannot strand the
// host in its completion state.
class FOnlineSnapshotReceiver
{
public:
	bool ReceiveBegin(const FOnlineSnapshotMetadata& metadata, size_t chunkCount, std::string& error);
	bool ReceiveChunk(const FOnlineSnapshotChunkFrame& chunk, std::string& error);
	bool NextAcknowledgement(FOnlineSnapshotAckFrame& acknowledgement, std::string& error);
	bool CopyCompletedArchive(FOnlineSnapshotMetadata& metadata, std::vector<uint8_t>& bytes, std::string& error) const;
	void Reset();

	bool IsActive() const { return Active; }
	bool IsComplete() const { return Complete; }

private:
	FOnlineSnapshotAssembler Assembler;
	FOnlineSnapshotMetadata ActiveHeader;
	FOnlineSnapshotMetadata CompletedHeader;
	std::vector<uint8_t> CompletedBytes;
	size_t ExpectedChunks = 0;
	size_t CompleteAcknowledgementCursor = 0;
	uint32_t PendingChunkAcknowledgement = 0;
	bool BeginAcknowledgementPending = false;
	bool ChunkAcknowledgementPending = false;
	bool Active = false;
	bool Complete = false;
};

// The host retains a bounded, ordered replay window after the snapshot tic.
// It supplies the joiner with deterministic updates that happened while the
// snapshot was in flight. One record represents the fully assembled events for
// one tic; callers must combine same-tic packets before appending.
struct FOnlineTicRecord
{
	uint64_t Tic = 0;
	std::vector<uint8_t> Bytes;
};

// Buffers host events that occurred after an accepted snapshot while a guest
// restores offline. Frames may arrive out of order, but the bounded handoff
// only succeeds when every tic through the requested live boundary is present
// exactly once (or is an identical retransmission).
class FOnlineSnapshotReplayBuffer
{
public:
	explicit FOnlineSnapshotReplayBuffer(size_t maxBytes = OnlineDefaultTicHistoryBytes);

	bool Start(const FOnlineSnapshotMetadata& metadata, std::string& error);
	bool Accept(const FOnlineSnapshotReplayFrame& replay, std::string& error);
	bool CopyThrough(uint64_t liveTic, std::vector<FOnlineTicRecord>& records, std::string& error) const;
	bool GetHighestContiguousTic(uint64_t& tic) const;
	void DiscardThrough(uint64_t tic);
	void Reset();

	bool IsActive() const { return Active; }
	size_t ByteCount() const { return Bytes; }

private:
	FOnlineSnapshotMetadata Header;
	std::vector<FOnlineTicRecord> Records;
	size_t Bytes = 0;
	size_t Limit = 0;
	bool Active = false;
};

class FOnlineTicHistory
{
public:
	explicit FOnlineTicHistory(size_t maxBytes = OnlineDefaultTicHistoryBytes);

	bool Append(uint64_t tic, const uint8_t* bytes, size_t size, std::string& error);
	std::vector<FOnlineTicRecord> RecordsAfter(uint64_t tic) const;
	void DiscardThrough(uint64_t tic);
	void Clear();

	size_t ByteCount() const { return Bytes; }
	size_t MaxBytes() const { return Limit; }

private:
	std::vector<FOnlineTicRecord> Records;
	size_t Bytes = 0;
	size_t Limit = 0;
	uint64_t LastTic = 0;
	bool HasLastTic = false;
};

} // namespace BdOnline
