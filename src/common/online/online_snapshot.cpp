#include "online_snapshot.h"

#include "common/thirdparty/m_crc32.h"

#include <sodium.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <new>

namespace BdOnline
{
namespace
{

bool EnsureSodium(std::string& error)
{
	if (sodium_init() < 0)
	{
		error = "libsodium could not initialize";
		return false;
	}
	return true;
}

bool GenerateTransferId(uint64_t& transferId, std::string& error)
{
	if (!EnsureSodium(error))
		return false;
	do
	{
		randombytes_buf(&transferId, sizeof(transferId));
	}
	while (transferId == 0);
	return true;
}

bool IsLowerHexDigest(const std::string& digest)
{
	if (digest.size() != OnlineDigestBytes * 2)
		return false;
	return std::all_of(digest.begin(), digest.end(), [](unsigned char character)
	{
		return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
	});
}

bool DecodeDigest(const std::string& digest, std::array<uint8_t, OnlineDigestBytes>& output)
{
	if (!IsLowerHexDigest(digest))
		return false;
	for (size_t index = 0; index < output.size(); ++index)
	{
		auto decodeNibble = [](char character) -> uint8_t
		{
			return character <= '9' ? static_cast<uint8_t>(character - '0') : static_cast<uint8_t>(character - 'a' + 10);
		};
		output[index] = static_cast<uint8_t>((decodeNibble(digest[index * 2]) << 4) | decodeNibble(digest[index * 2 + 1]));
	}
	return true;
}

std::string EncodeDigest(const uint8_t* bytes, size_t size)
{
	static constexpr char Hex[] = "0123456789abcdef";
	std::string digest;
	digest.reserve(size * 2);
	for (size_t index = 0; index < size; ++index)
	{
		digest.push_back(Hex[bytes[index] >> 4]);
		digest.push_back(Hex[bytes[index] & 15]);
	}
	return digest;
}

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

bool IsFrame(const uint8_t* bytes, size_t size, EOnlineSnapshotFrameType type)
{
	return bytes != nullptr && size >= 6 && std::memcmp(bytes, OnlineSnapshotFrameMagic.data(), OnlineSnapshotFrameMagic.size()) == 0 &&
		bytes[4] == OnlineSnapshotFrameVersion && bytes[5] == static_cast<uint8_t>(type);
}

bool IsSupportedSnapshotCompression(uint32_t method)
{
	return method == static_cast<uint32_t>(FileSys::METHOD_STORED) ||
		method == static_cast<uint32_t>(FileSys::METHOD_DEFLATE);
}

bool IsSafeSnapshotBundleName(const std::string& name)
{
	if (name.empty() || name.size() > 64)
		return false;
	return std::all_of(name.begin(), name.end(), [](unsigned char character)
	{
		return character >= 33 && character <= 126 && character != '/' && character != '\\';
	});
}

bool IsSnapshotComponentType(EOnlineSnapshotComponentType type)
{
	switch (type)
	{
	case EOnlineSnapshotComponentType::World:
	case EOnlineSnapshotComponentType::Globals:
	case EOnlineSnapshotComponentType::ProceduralMap:
	case EOnlineSnapshotComponentType::Hub:
		return true;
	default:
		return false;
	}
}

size_t ChunkSizeFor(const FOnlineSnapshotMetadata& metadata, size_t chunkIndex)
{
	const uint64_t offset = static_cast<uint64_t>(chunkIndex) * OnlineSnapshotChunkBytes;
	const uint64_t remaining = metadata.TotalBytes - offset;
	return static_cast<size_t>(std::min<uint64_t>(remaining, OnlineSnapshotChunkBytes));
}

bool SameSnapshotMetadata(const FOnlineSnapshotMetadata& left, const FOnlineSnapshotMetadata& right)
{
	return left.Version == right.Version && left.SerializerVersion == right.SerializerVersion &&
		left.Epoch == right.Epoch && left.SnapshotTic == right.SnapshotTic &&
		left.TransferId == right.TransferId &&
		left.TotalBytes == right.TotalBytes && left.UncompressedBytes == right.UncompressedBytes &&
		left.CompressionMethod == right.CompressionMethod && left.CRC32 == right.CRC32 &&
		left.Digest == right.Digest;
}

bool SameSnapshotIdentity(const FOnlineSnapshotMetadata& metadata, const FOnlineSnapshotAckFrame& acknowledgement)
{
	return metadata.Epoch == acknowledgement.Epoch && metadata.SnapshotTic == acknowledgement.SnapshotTic &&
		metadata.TransferId == acknowledgement.TransferId &&
		metadata.Digest == acknowledgement.Digest;
}

bool SameSnapshotIdentity(const FOnlineSnapshotMetadata& metadata, const FOnlineSnapshotChunkFrame& chunk)
{
	return metadata.Epoch == chunk.Epoch && metadata.SnapshotTic == chunk.SnapshotTic &&
		metadata.TransferId == chunk.TransferId &&
		metadata.Digest == chunk.Digest;
}

bool SameSnapshotIdentity(const FOnlineSnapshotMetadata& metadata, const FOnlineSnapshotReplayFrame& replay)
{
	return metadata.Epoch == replay.Epoch && metadata.SnapshotTic == replay.SnapshotTic &&
		metadata.TransferId == replay.TransferId &&
		metadata.Digest == replay.Digest;
}

bool ValidateReplayFrame(const FOnlineSnapshotReplayFrame& replay, std::string& error)
{
	if (replay.Epoch == 0 || replay.TransferId == 0 || replay.Tic <= replay.SnapshotTic ||
		replay.Bytes.empty() || replay.Bytes.size() > OnlineMaxTicRecordBytes)
	{
		error = "online snapshot replay frame is invalid";
		return false;
	}
	if (!IsLowerHexDigest(replay.Digest))
	{
		error = "online snapshot replay frame has an invalid digest";
		return false;
	}
	return true;
}

} // namespace

bool FOnlineSnapshotMetadata::Validate(std::string& error) const
{
	if (Version != OnlineSnapshotProtocolVersion)
	{
		error = "online snapshot uses an unsupported protocol version";
		return false;
	}
	if (SerializerVersion == 0)
	{
		error = "online snapshot is missing an engine serializer version";
		return false;
	}
	if (Epoch == 0)
	{
		error = "online snapshot is missing a session epoch";
		return false;
	}
	if (TransferId == 0)
	{
		error = "online snapshot is missing a transfer identity";
		return false;
	}
	if (TotalBytes == 0 || TotalBytes > OnlineMaxSnapshotBytes ||
		TotalBytes > static_cast<uint64_t>(std::numeric_limits<size_t>::max()) ||
		UncompressedBytes == 0 || UncompressedBytes > OnlineMaxSnapshotBytes ||
		UncompressedBytes > static_cast<uint64_t>(std::numeric_limits<size_t>::max()) ||
		!IsSupportedSnapshotCompression(CompressionMethod))
	{
		error = "online snapshot has unsafe serializer metadata";
		return false;
	}
	if (!IsLowerHexDigest(Digest))
	{
		error = "online snapshot has an invalid content digest";
		return false;
	}
	return true;
}

bool FOnlineSnapshotBundle::Validate(std::string& error) const
{
	if (Version != OnlineSnapshotBundleVersion || !IsSafeSnapshotBundleName(MapName) || Components.empty() ||
		Components.size() > OnlineMaxSnapshotComponents)
	{
		error = "online snapshot bundle has invalid metadata";
		return false;
	}
	uint64_t totalBytes = 0;
	uint64_t totalUncompressedBytes = 0;
	size_t worldCount = 0;
	size_t globalsCount = 0;
	size_t proceduralMapCount = 0;
	for (size_t index = 0; index < Components.size(); ++index)
	{
		const auto& component = Components[index];
		if (!IsSnapshotComponentType(component.Type) || !IsSafeSnapshotBundleName(component.Name) ||
			component.UncompressedBytes == 0 || component.UncompressedBytes > OnlineMaxSnapshotBytes ||
			component.Bytes.empty() || component.Bytes.size() > OnlineMaxSnapshotBytes ||
			!IsSupportedSnapshotCompression(component.CompressionMethod))
		{
			error = "online snapshot bundle has an unsafe component";
			return false;
		}
		if (totalBytes > OnlineMaxSnapshotBytes - component.Bytes.size())
		{
			error = "online snapshot bundle exceeds the allowed size";
			return false;
		}
		totalBytes += component.Bytes.size();
		if (totalUncompressedBytes > OnlineMaxSnapshotBytes - component.UncompressedBytes)
		{
			error = "online snapshot bundle expands beyond the allowed size";
			return false;
		}
		totalUncompressedBytes += component.UncompressedBytes;
		for (size_t prior = 0; prior < index; ++prior)
		{
			if (Components[prior].Type == component.Type && Components[prior].Name == component.Name)
			{
				error = "online snapshot bundle repeats a component";
				return false;
			}
		}
		if (component.Type == EOnlineSnapshotComponentType::World)
		{
			if (++worldCount != 1 || component.Name != MapName)
			{
				error = "online snapshot bundle has an invalid world component";
				return false;
			}
		}
		else if (component.Type == EOnlineSnapshotComponentType::Globals)
		{
			if (++globalsCount != 1 || component.Name != "globals")
			{
				error = "online snapshot bundle has an invalid global-state component";
				return false;
			}
		}
		else if (component.Type == EOnlineSnapshotComponentType::ProceduralMap && ++proceduralMapCount != 1)
		{
			error = "online snapshot bundle has multiple procedural-map components";
			return false;
		}
	}
	if (worldCount != 1 || globalsCount != 1)
	{
		error = "online snapshot bundle is missing world or global state";
		return false;
	}
	return true;
}

bool OnlineEncodeSnapshotBundle(const FOnlineSnapshotBundle& bundle, std::vector<uint8_t>& bytes, std::string& error)
{
	if (!bundle.Validate(error))
		return false;
	try
	{
		bytes.clear();
		bytes.reserve(7 + bundle.MapName.size());
		bytes.insert(bytes.end(), { 'B', 'D', 'J', 'B' });
		bytes.push_back(static_cast<uint8_t>(bundle.Version));
		bytes.push_back(static_cast<uint8_t>(bundle.MapName.size()));
		bytes.push_back(static_cast<uint8_t>(bundle.Components.size()));
		bytes.insert(bytes.end(), bundle.MapName.begin(), bundle.MapName.end());
		for (const auto& component : bundle.Components)
		{
			bytes.push_back(static_cast<uint8_t>(component.Type));
			bytes.push_back(static_cast<uint8_t>(component.Name.size()));
			bytes.insert(bytes.end(), component.Name.begin(), component.Name.end());
			WriteU64(bytes, component.UncompressedBytes);
			WriteU32(bytes, component.CompressionMethod);
			WriteU32(bytes, component.CRC32);
			WriteU64(bytes, component.Bytes.size());
			bytes.insert(bytes.end(), component.Bytes.begin(), component.Bytes.end());
		}
	}
	catch (const std::bad_alloc&)
	{
		bytes.clear();
		error = "not enough memory to encode online snapshot bundle";
		return false;
	}
	if (bytes.size() > OnlineMaxSnapshotBytes)
	{
		bytes.clear();
		error = "encoded online snapshot bundle exceeds the allowed size";
		return false;
	}
	return true;
}

bool OnlineDecodeSnapshotBundle(const uint8_t* bytes, size_t size, FOnlineSnapshotBundle& bundle, std::string& error)
{
	bundle = {};
	if (bytes == nullptr || size < 7 || size > OnlineMaxSnapshotBytes ||
		std::memcmp(bytes, "BDJB", 4) != 0 || bytes[4] != OnlineSnapshotBundleVersion)
	{
		error = "online snapshot bundle is malformed";
		return false;
	}
	const size_t mapNameLength = bytes[5];
	const size_t componentCount = bytes[6];
	if (mapNameLength == 0 || componentCount == 0 || componentCount > OnlineMaxSnapshotComponents || 7 + mapNameLength > size)
	{
		error = "online snapshot bundle has an invalid header";
		return false;
	}
	size_t position = 7;
	bundle.MapName.assign(reinterpret_cast<const char*>(bytes + position), mapNameLength);
	position += mapNameLength;
	try
	{
		bundle.Components.reserve(componentCount);
		for (size_t index = 0; index < componentCount; ++index)
		{
			if (position + 2 > size)
			{
				error = "online snapshot bundle component header is truncated";
				return false;
			}
			FOnlineSnapshotComponent component;
			component.Type = static_cast<EOnlineSnapshotComponentType>(bytes[position++]);
			const size_t nameLength = bytes[position++];
			constexpr size_t MetadataBytes = 8 + 4 + 4 + 8;
			if (nameLength == 0 || position > size - nameLength || position + nameLength > size ||
				position + nameLength > size - MetadataBytes)
			{
				error = "online snapshot bundle component metadata is truncated";
				return false;
			}
			component.Name.assign(reinterpret_cast<const char*>(bytes + position), nameLength);
			position += nameLength;
			component.UncompressedBytes = ReadU64(bytes + position); position += 8;
			component.CompressionMethod = ReadU32(bytes + position); position += 4;
			component.CRC32 = ReadU32(bytes + position); position += 4;
			const uint64_t storedBytes = ReadU64(bytes + position); position += 8;
			if (storedBytes == 0 || storedBytes > OnlineMaxSnapshotBytes || storedBytes > size - position)
			{
				error = "online snapshot bundle component has an invalid length";
				return false;
			}
			component.Bytes.assign(bytes + position, bytes + position + static_cast<size_t>(storedBytes));
			position += static_cast<size_t>(storedBytes);
			bundle.Components.push_back(std::move(component));
		}
	}
	catch (const std::bad_alloc&)
	{
		bundle = {};
		error = "not enough memory to decode online snapshot bundle";
		return false;
	}
	if (position != size)
	{
		bundle = {};
		error = "online snapshot bundle has trailing bytes";
		return false;
	}
	return bundle.Validate(error);
}

bool OnlineValidateSnapshotBundleArchives(const FOnlineSnapshotBundle& bundle, std::string& error)
{
	if (!bundle.Validate(error))
		return false;
	for (const auto& component : bundle.Components)
	{
		if (component.CompressionMethod == static_cast<uint32_t>(FileSys::METHOD_STORED))
		{
			if (component.Bytes.size() != component.UncompressedBytes ||
				CalcCRC32(component.Bytes.data(), static_cast<unsigned int>(component.Bytes.size())) != component.CRC32)
			{
				error = "online stored snapshot component fails its size or CRC check";
				return false;
			}
			continue;
		}

		std::vector<uint8_t> expanded;
		try
		{
			expanded.resize(static_cast<size_t>(component.UncompressedBytes));
		}
		catch (const std::bad_alloc&)
		{
			error = "not enough memory to validate an online snapshot component";
			return false;
		}
		const size_t decompressed = tinfl_decompress_mem_to_mem(expanded.data(), expanded.size(),
			component.Bytes.data(), component.Bytes.size(), 0);
		if (decompressed != expanded.size() ||
			CalcCRC32(expanded.data(), static_cast<unsigned int>(expanded.size())) != component.CRC32)
		{
			error = "online compressed snapshot component fails its decompression or CRC check";
			return false;
		}
	}
	return true;
}

bool OnlineMakeSnapshotMetadata(const FileSys::FCompressedBuffer& input, uint32_t serializerVersion,
	uint32_t epoch, uint64_t tic, FOnlineSnapshotMetadata& metadata, std::vector<uint8_t>& bytes, std::string& error)
{
	metadata = {};
	bytes.clear();
	if (serializerVersion == 0 || epoch == 0 || input.mBuffer == nullptr || input.mSize == 0 || input.mCompressedSize == 0 ||
		input.mSize > OnlineMaxSnapshotBytes || input.mCompressedSize > OnlineMaxSnapshotBytes ||
		input.mSize > static_cast<size_t>(std::numeric_limits<uint64_t>::max()) ||
		input.mCompressedSize > static_cast<size_t>(std::numeric_limits<uint64_t>::max()) ||
		!IsSupportedSnapshotCompression(static_cast<uint32_t>(input.mMethod)))
	{
		error = "engine serializer produced an unsafe online snapshot";
		return false;
	}

	try
	{
		bytes.assign(reinterpret_cast<const uint8_t*>(input.mBuffer),
			reinterpret_cast<const uint8_t*>(input.mBuffer) + input.mCompressedSize);
	}
	catch (const std::bad_alloc&)
	{
		error = "not enough memory to copy the online snapshot";
		return false;
	}
	metadata.SerializerVersion = serializerVersion;
	metadata.Epoch = epoch;
	metadata.SnapshotTic = tic;
	if (!GenerateTransferId(metadata.TransferId, error))
		return false;
	metadata.TotalBytes = input.mCompressedSize;
	metadata.UncompressedBytes = input.mSize;
	metadata.CompressionMethod = static_cast<uint32_t>(input.mMethod);
	metadata.CRC32 = input.mCRC32;
	if (!OnlineHashSnapshot(bytes.data(), bytes.size(), metadata.Digest, error))
		return false;
	return metadata.Validate(error);
}

bool OnlineMakeStoredSnapshotMetadata(const uint8_t* input, size_t size, uint32_t serializerVersion,
	uint32_t epoch, uint64_t tic, FOnlineSnapshotMetadata& metadata, std::vector<uint8_t>& bytes, std::string& error)
{
	metadata = {};
	bytes.clear();
	if (serializerVersion == 0 || epoch == 0 || input == nullptr || size == 0 || size > OnlineMaxSnapshotBytes)
	{
		error = "online stored snapshot has unsafe metadata";
		return false;
	}
	try
	{
		bytes.assign(input, input + size);
	}
	catch (const std::bad_alloc&)
	{
		error = "not enough memory to copy the online stored snapshot";
		return false;
	}
	metadata.SerializerVersion = serializerVersion;
	metadata.Epoch = epoch;
	metadata.SnapshotTic = tic;
	if (!GenerateTransferId(metadata.TransferId, error))
		return false;
	metadata.TotalBytes = size;
	metadata.UncompressedBytes = size;
	metadata.CompressionMethod = FileSys::METHOD_STORED;
	metadata.CRC32 = CalcCRC32(input, static_cast<unsigned int>(size));
	if (!OnlineHashSnapshot(bytes.data(), bytes.size(), metadata.Digest, error))
		return false;
	return metadata.Validate(error);
}

bool OnlineRestoreSnapshotArchive(const FOnlineSnapshotMetadata& metadata, const uint8_t* bytes, size_t size,
	FileSys::FCompressedBuffer& output, std::string& error)
{
	output.Clean();
	output = { 0, 0, 0, 0, nullptr, nullptr };
	if (!metadata.Validate(error) || bytes == nullptr || size != metadata.TotalBytes)
	{
		if (error.empty())
			error = "online snapshot archive length does not match its metadata";
		return false;
	}
	std::string digest;
	if (!OnlineHashSnapshot(bytes, size, digest, error))
		return false;
	if (digest != metadata.Digest)
	{
		error = "online snapshot archive digest does not match its metadata";
		return false;
	}

	char* copy = nullptr;
	try
	{
		copy = new char[size];
	}
	catch (const std::bad_alloc&)
	{
		error = "not enough memory to restore the online snapshot";
		return false;
	}
	std::memcpy(copy, bytes, size);
	output.mSize = static_cast<size_t>(metadata.UncompressedBytes);
	output.mCompressedSize = size;
	output.mMethod = static_cast<int>(metadata.CompressionMethod);
	output.mCRC32 = metadata.CRC32;
	output.mBuffer = copy;
	output.filename = nullptr;
	return true;
}

bool OnlineHashSnapshot(const uint8_t* bytes, size_t size, std::string& digest, std::string& error)
{
	if (size > OnlineMaxSnapshotBytes || (bytes == nullptr && size != 0))
	{
		error = "online snapshot exceeds the allowed size";
		return false;
	}
	if (!EnsureSodium(error))
		return false;

	crypto_generichash_state state = {};
	crypto_generichash_init(&state, nullptr, 0, OnlineDigestBytes);
	static constexpr char Domain[] = "biaseddoom-online-snapshot-v1";
	crypto_generichash_update(&state, reinterpret_cast<const unsigned char*>(Domain), sizeof(Domain) - 1);
	if (size != 0)
		crypto_generichash_update(&state, bytes, static_cast<unsigned long long>(size));
	std::array<unsigned char, OnlineDigestBytes> binary = {};
	if (crypto_generichash_final(&state, binary.data(), binary.size()) != 0)
	{
		error = "could not finalize online snapshot hash";
		return false;
	}
	std::array<char, OnlineDigestBytes * 2 + 1> hexadecimal = {};
	sodium_bin2hex(hexadecimal.data(), hexadecimal.size(), binary.data(), binary.size());
	digest = hexadecimal.data();
	return true;
}

bool OnlineIsSnapshotFrame(const uint8_t* bytes, size_t size)
{
	return bytes != nullptr && size >= 6 && std::memcmp(bytes, OnlineSnapshotFrameMagic.data(), OnlineSnapshotFrameMagic.size()) == 0 &&
		bytes[4] == OnlineSnapshotFrameVersion;
}

bool OnlineEncodeSnapshotBegin(const FOnlineSnapshotMetadata& metadata, size_t chunkCount,
	std::vector<uint8_t>& frame, std::string& error)
{
	if (!metadata.Validate(error))
		return false;
	const uint64_t expected = (metadata.TotalBytes + OnlineSnapshotChunkBytes - 1) / OnlineSnapshotChunkBytes;
	if (expected == 0 || expected > OnlineMaxSnapshotChunks || chunkCount != static_cast<size_t>(expected))
	{
		error = "online snapshot begin has an invalid chunk count";
		return false;
	}
	std::array<uint8_t, OnlineDigestBytes> digest = {};
	if (!DecodeDigest(metadata.Digest, digest))
	{
		error = "online snapshot begin has an invalid digest";
		return false;
	}
	frame.clear();
	frame.reserve(6 + 4 + 4 + 4 + 8 + 8 + 8 + 8 + 4 + 4 + 4 + digest.size());
	frame.insert(frame.end(), OnlineSnapshotFrameMagic.begin(), OnlineSnapshotFrameMagic.end());
	frame.push_back(OnlineSnapshotFrameVersion);
	frame.push_back(static_cast<uint8_t>(EOnlineSnapshotFrameType::Begin));
	WriteU32(frame, metadata.Version);
	WriteU32(frame, metadata.SerializerVersion);
	WriteU32(frame, metadata.Epoch);
	WriteU64(frame, metadata.SnapshotTic);
	WriteU64(frame, metadata.TransferId);
	WriteU64(frame, metadata.TotalBytes);
	WriteU64(frame, metadata.UncompressedBytes);
	WriteU32(frame, metadata.CompressionMethod);
	WriteU32(frame, metadata.CRC32);
	WriteU32(frame, static_cast<uint32_t>(chunkCount));
	frame.insert(frame.end(), digest.begin(), digest.end());
	return true;
}

bool OnlineDecodeSnapshotBegin(const uint8_t* bytes, size_t size, FOnlineSnapshotMetadata& metadata,
	size_t& chunkCount, std::string& error)
{
	constexpr size_t HeaderSize = 6 + 4 + 4 + 4 + 8 + 8 + 8 + 8 + 4 + 4 + 4 + OnlineDigestBytes;
	if (!IsFrame(bytes, size, EOnlineSnapshotFrameType::Begin) || size != HeaderSize)
	{
		error = "online snapshot begin frame is malformed";
		return false;
	}
	metadata = {};
	metadata.Version = ReadU32(bytes + 6);
	metadata.SerializerVersion = ReadU32(bytes + 10);
	metadata.Epoch = ReadU32(bytes + 14);
	metadata.SnapshotTic = ReadU64(bytes + 18);
	metadata.TransferId = ReadU64(bytes + 26);
	metadata.TotalBytes = ReadU64(bytes + 34);
	metadata.UncompressedBytes = ReadU64(bytes + 42);
	metadata.CompressionMethod = ReadU32(bytes + 50);
	metadata.CRC32 = ReadU32(bytes + 54);
	chunkCount = ReadU32(bytes + 58);
	metadata.Digest = EncodeDigest(bytes + 62, OnlineDigestBytes);
	if (!metadata.Validate(error))
		return false;
	const uint64_t expected = (metadata.TotalBytes + OnlineSnapshotChunkBytes - 1) / OnlineSnapshotChunkBytes;
	if (expected == 0 || expected > OnlineMaxSnapshotChunks || chunkCount != static_cast<size_t>(expected))
	{
		error = "online snapshot begin chunk count does not match its declared size";
		return false;
	}
	return true;
}

bool OnlineEncodeSnapshotChunk(const FOnlineSnapshotChunkFrame& chunk, std::vector<uint8_t>& frame, std::string& error)
{
	if (chunk.Epoch == 0 || chunk.TransferId == 0 || chunk.Bytes.empty() || chunk.Bytes.size() > OnlineSnapshotChunkBytes ||
		chunk.ChunkIndex >= OnlineMaxSnapshotChunks || chunk.ChunkIndex > std::numeric_limits<uint32_t>::max())
	{
		error = "online snapshot chunk is invalid";
		return false;
	}
	std::array<uint8_t, OnlineDigestBytes> digest = {};
	if (!DecodeDigest(chunk.Digest, digest))
	{
		error = "online snapshot chunk has an invalid digest";
		return false;
	}
	frame.clear();
	frame.reserve(6 + 4 + 8 + 8 + 4 + digest.size() + 2 + chunk.Bytes.size());
	frame.insert(frame.end(), OnlineSnapshotFrameMagic.begin(), OnlineSnapshotFrameMagic.end());
	frame.push_back(OnlineSnapshotFrameVersion);
	frame.push_back(static_cast<uint8_t>(EOnlineSnapshotFrameType::Chunk));
	WriteU32(frame, chunk.Epoch);
	WriteU64(frame, chunk.SnapshotTic);
	WriteU64(frame, chunk.TransferId);
	WriteU32(frame, static_cast<uint32_t>(chunk.ChunkIndex));
	frame.insert(frame.end(), digest.begin(), digest.end());
	WriteU16(frame, static_cast<uint16_t>(chunk.Bytes.size()));
	frame.insert(frame.end(), chunk.Bytes.begin(), chunk.Bytes.end());
	return true;
}

bool OnlineDecodeSnapshotChunk(const uint8_t* bytes, size_t size, FOnlineSnapshotChunkFrame& chunk, std::string& error)
{
	constexpr size_t HeaderSize = 6 + 4 + 8 + 8 + 4 + OnlineDigestBytes + 2;
	if (!IsFrame(bytes, size, EOnlineSnapshotFrameType::Chunk) || size < HeaderSize)
	{
		error = "online snapshot chunk frame is malformed";
		return false;
	}
	const uint16_t payloadSize = ReadU16(bytes + HeaderSize - 2);
	if (payloadSize == 0 || payloadSize > OnlineSnapshotChunkBytes || size != HeaderSize + payloadSize)
	{
		error = "online snapshot chunk frame has an invalid length";
		return false;
	}
	chunk = {};
	chunk.Epoch = ReadU32(bytes + 6);
	chunk.SnapshotTic = ReadU64(bytes + 10);
	chunk.TransferId = ReadU64(bytes + 18);
	chunk.ChunkIndex = ReadU32(bytes + 26);
	chunk.Digest = EncodeDigest(bytes + 30, OnlineDigestBytes);
	chunk.Bytes.assign(bytes + HeaderSize, bytes + size);
	if (chunk.Epoch == 0 || chunk.TransferId == 0 || chunk.ChunkIndex >= OnlineMaxSnapshotChunks)
	{
		error = "online snapshot chunk frame is out of range";
		return false;
	}
	return true;
}

bool OnlineEncodeSnapshotAck(const FOnlineSnapshotAckFrame& acknowledgement, std::vector<uint8_t>& frame, std::string& error)
{
	if (acknowledgement.Epoch == 0 || acknowledgement.TransferId == 0 ||
		(acknowledgement.ChunkIndex != OnlineSnapshotBeginAcknowledgement &&
			acknowledgement.ChunkIndex != OnlineSnapshotCompleteAcknowledgement &&
			acknowledgement.ChunkIndex >= OnlineMaxSnapshotChunks))
	{
		error = "online snapshot acknowledgement is invalid";
		return false;
	}
	std::array<uint8_t, OnlineDigestBytes> digest = {};
	if (!DecodeDigest(acknowledgement.Digest, digest))
	{
		error = "online snapshot acknowledgement has an invalid digest";
		return false;
	}
	frame.clear();
	frame.reserve(6 + 4 + 8 + 8 + digest.size() + 4);
	frame.insert(frame.end(), OnlineSnapshotFrameMagic.begin(), OnlineSnapshotFrameMagic.end());
	frame.push_back(OnlineSnapshotFrameVersion);
	frame.push_back(static_cast<uint8_t>(EOnlineSnapshotFrameType::Acknowledge));
	WriteU32(frame, acknowledgement.Epoch);
	WriteU64(frame, acknowledgement.SnapshotTic);
	WriteU64(frame, acknowledgement.TransferId);
	frame.insert(frame.end(), digest.begin(), digest.end());
	WriteU32(frame, acknowledgement.ChunkIndex);
	return true;
}

bool OnlineDecodeSnapshotAck(const uint8_t* bytes, size_t size, FOnlineSnapshotAckFrame& acknowledgement, std::string& error)
{
	constexpr size_t FrameSize = 6 + 4 + 8 + 8 + OnlineDigestBytes + 4;
	if (!IsFrame(bytes, size, EOnlineSnapshotFrameType::Acknowledge) || size != FrameSize)
	{
		error = "online snapshot acknowledgement is malformed";
		return false;
	}
	acknowledgement = {};
	acknowledgement.Epoch = ReadU32(bytes + 6);
	acknowledgement.SnapshotTic = ReadU64(bytes + 10);
	acknowledgement.TransferId = ReadU64(bytes + 18);
	acknowledgement.Digest = EncodeDigest(bytes + 26, OnlineDigestBytes);
	acknowledgement.ChunkIndex = ReadU32(bytes + 26 + OnlineDigestBytes);
	if (acknowledgement.Epoch == 0 || acknowledgement.TransferId == 0 ||
		(acknowledgement.ChunkIndex != OnlineSnapshotBeginAcknowledgement &&
			acknowledgement.ChunkIndex != OnlineSnapshotCompleteAcknowledgement &&
			acknowledgement.ChunkIndex >= OnlineMaxSnapshotChunks))
	{
		error = "online snapshot acknowledgement is out of range";
		return false;
	}
	return true;
}

bool OnlineEncodeSnapshotReplayTic(const FOnlineSnapshotReplayFrame& replay, std::vector<uint8_t>& frame, std::string& error)
{
	if (!ValidateReplayFrame(replay, error))
		return false;
	std::array<uint8_t, OnlineDigestBytes> digest = {};
	if (!DecodeDigest(replay.Digest, digest))
	{
		error = "online snapshot replay frame has an invalid digest";
		return false;
	}
	frame.clear();
	frame.reserve(6 + 4 + 8 + 8 + 8 + digest.size() + 4 + replay.Bytes.size());
	frame.insert(frame.end(), OnlineSnapshotFrameMagic.begin(), OnlineSnapshotFrameMagic.end());
	frame.push_back(OnlineSnapshotFrameVersion);
	frame.push_back(static_cast<uint8_t>(EOnlineSnapshotFrameType::ReplayTic));
	WriteU32(frame, replay.Epoch);
	WriteU64(frame, replay.SnapshotTic);
	WriteU64(frame, replay.TransferId);
	WriteU64(frame, replay.Tic);
	frame.insert(frame.end(), digest.begin(), digest.end());
	WriteU32(frame, static_cast<uint32_t>(replay.Bytes.size()));
	frame.insert(frame.end(), replay.Bytes.begin(), replay.Bytes.end());
	return true;
}

bool OnlineDecodeSnapshotReplayTic(const uint8_t* bytes, size_t size, FOnlineSnapshotReplayFrame& replay, std::string& error)
{
	constexpr size_t HeaderSize = 6 + 4 + 8 + 8 + 8 + OnlineDigestBytes + 4;
	if (!IsFrame(bytes, size, EOnlineSnapshotFrameType::ReplayTic) || size < HeaderSize)
	{
		error = "online snapshot replay frame is malformed";
		return false;
	}
	const uint32_t payloadSize = ReadU32(bytes + HeaderSize - 4);
	if (payloadSize == 0 || payloadSize > OnlineMaxTicRecordBytes || size != HeaderSize + static_cast<size_t>(payloadSize))
	{
		error = "online snapshot replay frame has an invalid length";
		return false;
	}
	replay = {};
	replay.Epoch = ReadU32(bytes + 6);
	replay.SnapshotTic = ReadU64(bytes + 10);
	replay.TransferId = ReadU64(bytes + 18);
	replay.Tic = ReadU64(bytes + 26);
	replay.Digest = EncodeDigest(bytes + 34, OnlineDigestBytes);
	replay.Bytes.assign(bytes + HeaderSize, bytes + size);
	return ValidateReplayFrame(replay, error);
}

bool OnlineEncodeSnapshotComplete(const FOnlineSnapshotCompleteFrame& complete,
	std::vector<uint8_t>& frame, std::string& error)
{
	if (complete.Epoch == 0 || complete.TransferId == 0 || complete.LiveTic < complete.SnapshotTic)
	{
		error = "online snapshot completion fence is invalid";
		return false;
	}
	std::array<uint8_t, OnlineDigestBytes> digest = {};
	if (!DecodeDigest(complete.Digest, digest))
	{
		error = "online snapshot completion fence has an invalid digest";
		return false;
	}
	frame.clear();
	frame.reserve(6 + 4 + 8 + 8 + 8 + digest.size());
	frame.insert(frame.end(), OnlineSnapshotFrameMagic.begin(), OnlineSnapshotFrameMagic.end());
	frame.push_back(OnlineSnapshotFrameVersion);
	frame.push_back(static_cast<uint8_t>(EOnlineSnapshotFrameType::Complete));
	WriteU32(frame, complete.Epoch);
	WriteU64(frame, complete.SnapshotTic);
	WriteU64(frame, complete.TransferId);
	WriteU64(frame, complete.LiveTic);
	frame.insert(frame.end(), digest.begin(), digest.end());
	return true;
}

bool OnlineDecodeSnapshotComplete(const uint8_t* bytes, size_t size,
	FOnlineSnapshotCompleteFrame& complete, std::string& error)
{
	constexpr size_t FrameSize = 6 + 4 + 8 + 8 + 8 + OnlineDigestBytes;
	if (!IsFrame(bytes, size, EOnlineSnapshotFrameType::Complete) || size != FrameSize)
	{
		error = "online snapshot completion fence is malformed";
		return false;
	}
	complete = {};
	complete.Epoch = ReadU32(bytes + 6);
	complete.SnapshotTic = ReadU64(bytes + 10);
	complete.TransferId = ReadU64(bytes + 18);
	complete.LiveTic = ReadU64(bytes + 26);
	complete.Digest = EncodeDigest(bytes + 34, OnlineDigestBytes);
	if (complete.Epoch == 0 || complete.TransferId == 0 || complete.LiveTic < complete.SnapshotTic)
	{
		error = "online snapshot completion fence is out of range";
		return false;
	}
	return true;
}

bool OnlineEncodeSnapshotCompleteAck(const FOnlineSnapshotCompleteAckFrame& acknowledgement,
	std::vector<uint8_t>& frame, std::string& error)
{
	if (acknowledgement.Epoch == 0 || acknowledgement.TransferId == 0 ||
		acknowledgement.LiveTic < acknowledgement.SnapshotTic)
	{
		error = "online snapshot completion acknowledgement is invalid";
		return false;
	}
	std::array<uint8_t, OnlineDigestBytes> digest = {};
	if (!DecodeDigest(acknowledgement.Digest, digest))
	{
		error = "online snapshot completion acknowledgement has an invalid digest";
		return false;
	}
	frame.clear();
	frame.reserve(6 + 4 + 8 + 8 + 8 + digest.size());
	frame.insert(frame.end(), OnlineSnapshotFrameMagic.begin(), OnlineSnapshotFrameMagic.end());
	frame.push_back(OnlineSnapshotFrameVersion);
	frame.push_back(static_cast<uint8_t>(EOnlineSnapshotFrameType::CompleteAcknowledge));
	WriteU32(frame, acknowledgement.Epoch);
	WriteU64(frame, acknowledgement.SnapshotTic);
	WriteU64(frame, acknowledgement.TransferId);
	WriteU64(frame, acknowledgement.LiveTic);
	frame.insert(frame.end(), digest.begin(), digest.end());
	return true;
}

bool OnlineDecodeSnapshotCompleteAck(const uint8_t* bytes, size_t size,
	FOnlineSnapshotCompleteAckFrame& acknowledgement, std::string& error)
{
	constexpr size_t FrameSize = 6 + 4 + 8 + 8 + 8 + OnlineDigestBytes;
	if (!IsFrame(bytes, size, EOnlineSnapshotFrameType::CompleteAcknowledge) || size != FrameSize)
	{
		error = "online snapshot completion acknowledgement is malformed";
		return false;
	}
	acknowledgement = {};
	acknowledgement.Epoch = ReadU32(bytes + 6);
	acknowledgement.SnapshotTic = ReadU64(bytes + 10);
	acknowledgement.TransferId = ReadU64(bytes + 18);
	acknowledgement.LiveTic = ReadU64(bytes + 26);
	acknowledgement.Digest = EncodeDigest(bytes + 34, OnlineDigestBytes);
	if (acknowledgement.Epoch == 0 || acknowledgement.TransferId == 0 ||
		acknowledgement.LiveTic < acknowledgement.SnapshotTic)
	{
		error = "online snapshot completion acknowledgement is out of range";
		return false;
	}
	return true;
}

bool OnlineEncodeSnapshotCancel(const FOnlineSnapshotCancelFrame& cancel,
	std::vector<uint8_t>& frame, std::string& error)
{
	const uint8_t reason = static_cast<uint8_t>(cancel.Reason);
	if (cancel.Epoch == 0 || cancel.TransferId == 0 ||
		reason < static_cast<uint8_t>(EOnlineSnapshotCancelReason::HostStopped) ||
		reason > static_cast<uint8_t>(EOnlineSnapshotCancelReason::EpochChanged))
	{
		error = "online snapshot cancellation is invalid";
		return false;
	}
	std::array<uint8_t, OnlineDigestBytes> digest = {};
	if (!DecodeDigest(cancel.Digest, digest))
	{
		error = "online snapshot cancellation has an invalid digest";
		return false;
	}
	frame.clear();
	frame.reserve(6 + 4 + 8 + 8 + digest.size() + 1);
	frame.insert(frame.end(), OnlineSnapshotFrameMagic.begin(), OnlineSnapshotFrameMagic.end());
	frame.push_back(OnlineSnapshotFrameVersion);
	frame.push_back(static_cast<uint8_t>(EOnlineSnapshotFrameType::Cancel));
	WriteU32(frame, cancel.Epoch);
	WriteU64(frame, cancel.SnapshotTic);
	WriteU64(frame, cancel.TransferId);
	frame.insert(frame.end(), digest.begin(), digest.end());
	frame.push_back(reason);
	return true;
}

bool OnlineDecodeSnapshotCancel(const uint8_t* bytes, size_t size,
	FOnlineSnapshotCancelFrame& cancel, std::string& error)
{
	constexpr size_t FrameSize = 6 + 4 + 8 + 8 + OnlineDigestBytes + 1;
	if (!IsFrame(bytes, size, EOnlineSnapshotFrameType::Cancel) || size != FrameSize)
	{
		error = "online snapshot cancellation is malformed";
		return false;
	}
	cancel = {};
	cancel.Epoch = ReadU32(bytes + 6);
	cancel.SnapshotTic = ReadU64(bytes + 10);
	cancel.TransferId = ReadU64(bytes + 18);
	cancel.Digest = EncodeDigest(bytes + 26, OnlineDigestBytes);
	const uint8_t reason = bytes[26 + OnlineDigestBytes];
	if (cancel.Epoch == 0 || cancel.TransferId == 0 ||
		reason < static_cast<uint8_t>(EOnlineSnapshotCancelReason::HostStopped) ||
		reason > static_cast<uint8_t>(EOnlineSnapshotCancelReason::EpochChanged))
	{
		error = "online snapshot cancellation is out of range";
		return false;
	}
	cancel.Reason = static_cast<EOnlineSnapshotCancelReason>(reason);
	return true;
}

bool OnlineEncodeSnapshotReplayAck(const FOnlineSnapshotReplayAckFrame& acknowledgement,
	std::vector<uint8_t>& frame, std::string& error)
{
	if (acknowledgement.Epoch == 0 || acknowledgement.TransferId == 0 ||
		acknowledgement.AcknowledgedTic < acknowledgement.SnapshotTic)
	{
		error = "online snapshot replay acknowledgement is invalid";
		return false;
	}
	std::array<uint8_t, OnlineDigestBytes> digest = {};
	if (!DecodeDigest(acknowledgement.Digest, digest))
	{
		error = "online snapshot replay acknowledgement has an invalid digest";
		return false;
	}
	frame.clear();
	frame.reserve(6 + 4 + 8 + 8 + 8 + digest.size());
	frame.insert(frame.end(), OnlineSnapshotFrameMagic.begin(), OnlineSnapshotFrameMagic.end());
	frame.push_back(OnlineSnapshotFrameVersion);
	frame.push_back(static_cast<uint8_t>(EOnlineSnapshotFrameType::ReplayAcknowledge));
	WriteU32(frame, acknowledgement.Epoch);
	WriteU64(frame, acknowledgement.SnapshotTic);
	WriteU64(frame, acknowledgement.TransferId);
	WriteU64(frame, acknowledgement.AcknowledgedTic);
	frame.insert(frame.end(), digest.begin(), digest.end());
	return true;
}

bool OnlineDecodeSnapshotReplayAck(const uint8_t* bytes, size_t size,
	FOnlineSnapshotReplayAckFrame& acknowledgement, std::string& error)
{
	constexpr size_t FrameSize = 6 + 4 + 8 + 8 + 8 + OnlineDigestBytes;
	if (!IsFrame(bytes, size, EOnlineSnapshotFrameType::ReplayAcknowledge) || size != FrameSize)
	{
		error = "online snapshot replay acknowledgement is malformed";
		return false;
	}
	acknowledgement = {};
	acknowledgement.Epoch = ReadU32(bytes + 6);
	acknowledgement.SnapshotTic = ReadU64(bytes + 10);
	acknowledgement.TransferId = ReadU64(bytes + 18);
	acknowledgement.AcknowledgedTic = ReadU64(bytes + 26);
	acknowledgement.Digest = EncodeDigest(bytes + 34, OnlineDigestBytes);
	if (acknowledgement.Epoch == 0 || acknowledgement.TransferId == 0 ||
		acknowledgement.AcknowledgedTic < acknowledgement.SnapshotTic)
	{
		error = "online snapshot replay acknowledgement is out of range";
		return false;
	}
	return true;
}

bool FOnlineSnapshotAssembler::Begin(const FOnlineSnapshotMetadata& metadata, size_t chunkCount, std::string& error)
{
	if (!metadata.Validate(error))
		return false;
	const uint64_t expected = (metadata.TotalBytes + OnlineSnapshotChunkBytes - 1) / OnlineSnapshotChunkBytes;
	if (expected == 0 || expected > OnlineMaxSnapshotChunks || chunkCount != static_cast<size_t>(expected))
	{
		error = "online snapshot chunk count does not match its declared size";
		return false;
	}
	// A Begin frame is deliberately retried until it is acknowledged. Retain
	// already verified chunks when that retry describes the same archive rather
	// than turning normal UDP loss into a full transfer restart.
	if (Active && ExpectedChunks == chunkCount && SameSnapshotMetadata(Header, metadata))
		return true;
	Reset();

	try
	{
		Bytes.resize(static_cast<size_t>(metadata.TotalBytes));
		Received.assign(chunkCount, 0);
	}
	catch (const std::bad_alloc&)
	{
		Reset();
		error = "not enough memory to receive online snapshot";
		return false;
	}
	Header = metadata;
	ExpectedChunks = chunkCount;
	Active = true;
	return true;
}

bool FOnlineSnapshotAssembler::AcceptChunk(size_t chunkIndex, const uint8_t* bytes, size_t size, std::string& error)
{
	if (!Active)
	{
		error = "online snapshot has not been announced";
		return false;
	}
	if (chunkIndex >= ExpectedChunks || (bytes == nullptr && size != 0))
	{
		error = "online snapshot chunk is invalid";
		return false;
	}
	const size_t expectedSize = ChunkSizeFor(Header, chunkIndex);
	if (size != expectedSize)
	{
		error = "online snapshot chunk has an invalid length";
		return false;
	}
	const size_t offset = chunkIndex * OnlineSnapshotChunkBytes;
	if (Received[chunkIndex] != 0)
	{
		if (std::memcmp(Bytes.data() + offset, bytes, size) != 0)
		{
			error = "online snapshot duplicate chunk conflicts with the first copy";
			return false;
		}
		return true;
	}
	std::memcpy(Bytes.data() + offset, bytes, size);
	Received[chunkIndex] = 1;
	++ReceivedChunks;
	return true;
}

bool FOnlineSnapshotAssembler::Finalize(std::vector<uint8_t>& snapshot, std::string& error)
{
	if (!IsComplete())
	{
		error = "online snapshot transfer is incomplete";
		return false;
	}
	std::string digest;
	if (!OnlineHashSnapshot(Bytes.data(), Bytes.size(), digest, error))
		return false;
	if (digest != Header.Digest)
	{
		error = "online snapshot content digest does not match the host manifest";
		return false;
	}
	snapshot = std::move(Bytes);
	Reset();
	return true;
}

void FOnlineSnapshotAssembler::Reset()
{
	Header = {};
	Bytes.clear();
	Received.clear();
	ExpectedChunks = 0;
	ReceivedChunks = 0;
	Active = false;
}

bool FOnlineSnapshotSender::Start(const FOnlineSnapshotMetadata& metadata, const uint8_t* bytes, size_t size, std::string& error)
{
	Reset();
	if (!metadata.Validate(error) || bytes == nullptr || size != metadata.TotalBytes)
	{
		if (error.empty())
			error = "online snapshot sender bytes do not match metadata";
		return false;
	}
	std::string digest;
	if (!OnlineHashSnapshot(bytes, size, digest, error))
		return false;
	if (digest != metadata.Digest)
	{
		error = "online snapshot sender bytes do not match metadata digest";
		return false;
	}
	const uint64_t chunkCount64 = (metadata.TotalBytes + OnlineSnapshotChunkBytes - 1) / OnlineSnapshotChunkBytes;
	if (chunkCount64 == 0 || chunkCount64 > OnlineMaxSnapshotChunks)
	{
		error = "online snapshot sender has an invalid chunk count";
		return false;
	}
	try
	{
		Bytes.assign(bytes, bytes + size);
		SentChunks.assign(static_cast<size_t>(chunkCount64), 0);
		AcknowledgedChunks.assign(static_cast<size_t>(chunkCount64), 0);
	}
	catch (const std::bad_alloc&)
	{
		Reset();
		error = "not enough memory to queue an online snapshot";
		return false;
	}
	Header = metadata;
	Active = true;
	return true;
}

bool FOnlineSnapshotSender::NextFrame(std::vector<uint8_t>& frame, std::string& error)
{
	if (!Active || CompleteAcknowledged)
	{
		error = "online snapshot sender is not active";
		return false;
	}
	if (!BeginAcknowledged)
	{
		if (!OnlineEncodeSnapshotBegin(Header, AcknowledgedChunks.size(), frame, error))
			return false;
		BeginSent = true;
		return true;
	}
	for (size_t attempts = 0; attempts < AcknowledgedChunks.size(); ++attempts)
	{
		const size_t chunkIndex = NextChunk;
		NextChunk = (NextChunk + 1) % AcknowledgedChunks.size();
		if (AcknowledgedChunks[chunkIndex] != 0)
			continue;
		const size_t offset = chunkIndex * OnlineSnapshotChunkBytes;
		FOnlineSnapshotChunkFrame chunk;
		chunk.Epoch = Header.Epoch;
		chunk.SnapshotTic = Header.SnapshotTic;
		chunk.TransferId = Header.TransferId;
		chunk.Digest = Header.Digest;
		chunk.ChunkIndex = chunkIndex;
		chunk.Bytes.assign(Bytes.begin() + static_cast<std::ptrdiff_t>(offset),
			Bytes.begin() + static_cast<std::ptrdiff_t>(offset + ChunkSizeFor(Header, chunkIndex)));
		if (!OnlineEncodeSnapshotChunk(chunk, frame, error))
			return false;
		SentChunks[chunkIndex] = 1;
		return true;
	}
	// Every chunk was acknowledged. Keep asking the receiver for its final
	// archive validation rather than silently treating a one-way UDP stream as
	// complete; a receiver may still reject a hash or serializer mismatch.
	error = "online snapshot sender is awaiting final receiver acknowledgement";
	return false;
}

bool FOnlineSnapshotSender::Acknowledge(const FOnlineSnapshotAckFrame& acknowledgement, std::string& error)
{
	if (!Active || !SameSnapshotIdentity(Header, acknowledgement))
	{
		error = "online snapshot acknowledgement does not match the active transfer";
		return false;
	}
	if (acknowledgement.ChunkIndex == OnlineSnapshotBeginAcknowledgement)
	{
		if (!BeginSent)
		{
			error = "online snapshot Begin was acknowledged before the host sent it";
			return false;
		}
		BeginAcknowledged = true;
		return true;
	}
	if (acknowledgement.ChunkIndex == OnlineSnapshotCompleteAcknowledgement)
	{
		if (!BeginAcknowledged)
		{
			error = "online snapshot completed before its begin frame was acknowledged";
			return false;
		}
		if (std::find(AcknowledgedChunks.begin(), AcknowledgedChunks.end(), 0) != AcknowledgedChunks.end())
		{
			// A completion acknowledgement may overtake one of the individual
			// chunk acknowledgements on UDP. It is not malformed or hostile; the
			// receiver cycles it again after the missing acknowledgements arrive.
			return true;
		}
		CompleteAcknowledged = true;
		return true;
	}
	if (acknowledgement.ChunkIndex >= AcknowledgedChunks.size())
	{
		error = "online snapshot acknowledgement chunk is out of range";
		return false;
	}
	if (!BeginAcknowledged)
	{
		error = "online snapshot chunk was acknowledged before its begin frame";
		return false;
	}
	if (SentChunks[acknowledgement.ChunkIndex] == 0)
	{
		error = "online snapshot chunk was acknowledged before the host sent it";
		return false;
	}
	AcknowledgedChunks[acknowledgement.ChunkIndex] = 1;
	return true;
}

void FOnlineSnapshotSender::Reset()
{
	Header = {};
	Bytes.clear();
	SentChunks.clear();
	AcknowledgedChunks.clear();
	NextChunk = 0;
	BeginSent = false;
	BeginAcknowledged = false;
	CompleteAcknowledged = false;
	Active = false;
}

bool FOnlineSnapshotReceiver::ReceiveBegin(const FOnlineSnapshotMetadata& metadata, size_t chunkCount, std::string& error)
{
	if (!metadata.Validate(error))
		return false;
	const uint64_t expected = (metadata.TotalBytes + OnlineSnapshotChunkBytes - 1) / OnlineSnapshotChunkBytes;
	if (expected == 0 || expected > OnlineMaxSnapshotChunks || chunkCount != static_cast<size_t>(expected))
	{
		error = "online snapshot receiver got an invalid chunk count";
		return false;
	}
	if (Complete && ExpectedChunks == chunkCount && SameSnapshotMetadata(CompletedHeader, metadata))
	{
		BeginAcknowledgementPending = true;
		return true;
	}
	if (Active && ExpectedChunks == chunkCount && SameSnapshotMetadata(ActiveHeader, metadata))
	{
		if (!Assembler.Begin(metadata, chunkCount, error))
			return false;
		BeginAcknowledgementPending = true;
		return true;
	}

	Reset();
	if (!Assembler.Begin(metadata, chunkCount, error))
		return false;
	ActiveHeader = metadata;
	ExpectedChunks = chunkCount;
	BeginAcknowledgementPending = true;
	Active = true;
	return true;
}

bool FOnlineSnapshotReceiver::ReceiveChunk(const FOnlineSnapshotChunkFrame& chunk, std::string& error)
{
	if (Active)
	{
		if (!SameSnapshotIdentity(ActiveHeader, chunk) || chunk.ChunkIndex >= ExpectedChunks)
		{
			error = "online snapshot chunk does not match the active archive";
			return false;
		}
		if (!Assembler.AcceptChunk(chunk.ChunkIndex, chunk.Bytes.data(), chunk.Bytes.size(), error))
			return false;
		BeginAcknowledgementPending = false;
		PendingChunkAcknowledgement = static_cast<uint32_t>(chunk.ChunkIndex);
		ChunkAcknowledgementPending = true;
		if (!Assembler.IsComplete())
			return true;

		std::vector<uint8_t> completed;
		if (!Assembler.Finalize(completed, error))
			return false;
		CompletedHeader = ActiveHeader;
		CompletedBytes = std::move(completed);
		CompleteAcknowledgementCursor = 0;
		Active = false;
		Complete = true;
		return true;
	}

	if (!Complete || !SameSnapshotIdentity(CompletedHeader, chunk) || chunk.ChunkIndex >= ExpectedChunks ||
		chunk.Bytes.size() != ChunkSizeFor(CompletedHeader, chunk.ChunkIndex))
	{
		error = "online snapshot chunk arrived without a matching archive";
		return false;
	}
	const size_t offset = chunk.ChunkIndex * OnlineSnapshotChunkBytes;
	if (std::memcmp(CompletedBytes.data() + offset, chunk.Bytes.data(), chunk.Bytes.size()) != 0)
	{
		error = "online snapshot retry chunk conflicts with the completed archive";
		return false;
	}
	PendingChunkAcknowledgement = static_cast<uint32_t>(chunk.ChunkIndex);
	ChunkAcknowledgementPending = true;
	return true;
}

bool FOnlineSnapshotReceiver::NextAcknowledgement(FOnlineSnapshotAckFrame& acknowledgement, std::string& error)
{
	if (!Active && !Complete)
	{
		error = "online snapshot receiver is not active";
		return false;
	}
	const auto& metadata = Complete ? CompletedHeader : ActiveHeader;
	acknowledgement = { metadata.Epoch, metadata.SnapshotTic, metadata.TransferId,
		metadata.Digest, OnlineSnapshotBeginAcknowledgement };
	if (BeginAcknowledgementPending)
	{
		BeginAcknowledgementPending = false;
		return true;
	}
	if (ChunkAcknowledgementPending)
	{
		acknowledgement.ChunkIndex = PendingChunkAcknowledgement;
		ChunkAcknowledgementPending = false;
		return true;
	}
	if (!Complete)
	{
		error = "online snapshot receiver has no acknowledgement to send";
		return false;
	}
	if (CompleteAcknowledgementCursor < ExpectedChunks)
	{
		acknowledgement.ChunkIndex = static_cast<uint32_t>(CompleteAcknowledgementCursor++);
		return true;
	}
	CompleteAcknowledgementCursor = 0;
	acknowledgement.ChunkIndex = OnlineSnapshotCompleteAcknowledgement;
	return true;
}

bool FOnlineSnapshotReceiver::CopyCompletedArchive(FOnlineSnapshotMetadata& metadata, std::vector<uint8_t>& bytes, std::string& error) const
{
	if (!Complete)
	{
		error = "online snapshot receiver has not completed an archive";
		return false;
	}
	try
	{
		metadata = CompletedHeader;
		bytes = CompletedBytes;
	}
	catch (const std::bad_alloc&)
	{
		error = "not enough memory to copy the completed online snapshot";
		return false;
	}
	return true;
}

void FOnlineSnapshotReceiver::Reset()
{
	Assembler.Reset();
	ActiveHeader = {};
	CompletedHeader = {};
	CompletedBytes.clear();
	ExpectedChunks = 0;
	CompleteAcknowledgementCursor = 0;
	PendingChunkAcknowledgement = 0;
	BeginAcknowledgementPending = false;
	ChunkAcknowledgementPending = false;
	Active = false;
	Complete = false;
}

FOnlineSnapshotReplayBuffer::FOnlineSnapshotReplayBuffer(size_t maxBytes)
	: Limit(maxBytes)
{
}

bool FOnlineSnapshotReplayBuffer::Start(const FOnlineSnapshotMetadata& metadata, std::string& error)
{
	Reset();
	if (!metadata.Validate(error) || Limit == 0 || Limit > OnlineMaxSnapshotBytes)
	{
		if (error.empty())
			error = "online snapshot replay buffer has an unsafe byte limit";
		return false;
	}
	Header = metadata;
	Active = true;
	return true;
}

bool FOnlineSnapshotReplayBuffer::Accept(const FOnlineSnapshotReplayFrame& replay, std::string& error)
{
	if (!Active || !SameSnapshotIdentity(Header, replay))
	{
		error = "online snapshot replay does not match the active archive";
		return false;
	}
	if (!ValidateReplayFrame(replay, error))
		return false;
	const auto position = std::lower_bound(Records.begin(), Records.end(), replay.Tic,
		[](const FOnlineTicRecord& record, uint64_t tic)
		{
			return record.Tic < tic;
		});
	if (position != Records.end() && position->Tic == replay.Tic)
	{
		if (position->Bytes != replay.Bytes)
		{
			error = "online snapshot replay duplicate conflicts with the first copy";
			return false;
		}
		return true;
	}
	if (replay.Bytes.size() > Limit - Bytes)
	{
		error = "online snapshot replay exceeds the bounded handoff buffer";
		return false;
	}
	try
	{
		Records.insert(position, { replay.Tic, replay.Bytes });
	}
	catch (const std::bad_alloc&)
	{
		error = "not enough memory to queue online snapshot replay";
		return false;
	}
	Bytes += replay.Bytes.size();
	return true;
}

bool FOnlineSnapshotReplayBuffer::CopyThrough(uint64_t liveTic, std::vector<FOnlineTicRecord>& records, std::string& error) const
{
	records.clear();
	if (!Active || liveTic < Header.SnapshotTic)
	{
		error = "online snapshot replay boundary is invalid";
		return false;
	}
	if (liveTic == std::numeric_limits<uint64_t>::max())
	{
		error = "online snapshot replay boundary exceeds the supported tic range";
		return false;
	}
	if (liveTic == Header.SnapshotTic)
		return true;
	uint64_t expected = Header.SnapshotTic + 1;
	for (const auto& record : Records)
	{
		if (record.Tic > liveTic)
			break;
		if (record.Tic != expected)
		{
			records.clear();
			error = "online snapshot replay has a tic gap";
			return false;
		}
		try
		{
			records.push_back(record);
		}
		catch (const std::bad_alloc&)
		{
			records.clear();
			error = "not enough memory to copy online snapshot replay";
			return false;
		}
		if (expected == std::numeric_limits<uint64_t>::max())
			break;
		++expected;
	}
	if (expected != liveTic + 1)
	{
		records.clear();
		error = "online snapshot replay does not reach the live boundary";
		return false;
	}
	return true;
}

bool FOnlineSnapshotReplayBuffer::GetHighestContiguousTic(uint64_t& tic) const
{
	tic = 0;
	if (!Active)
		return false;
	tic = Header.SnapshotTic;
	if (tic == std::numeric_limits<uint64_t>::max())
		return true;
	uint64_t expected = tic + 1;
	for (const auto& record : Records)
	{
		if (record.Tic != expected)
			break;
		tic = record.Tic;
		if (tic == std::numeric_limits<uint64_t>::max())
			break;
		++expected;
	}
	return true;
}

void FOnlineSnapshotReplayBuffer::DiscardThrough(uint64_t tic)
{
	while (!Records.empty() && Records.front().Tic <= tic)
	{
		Bytes -= Records.front().Bytes.size();
		Records.erase(Records.begin());
	}
}

void FOnlineSnapshotReplayBuffer::Reset()
{
	Header = {};
	Records.clear();
	Bytes = 0;
	Active = false;
}

FOnlineTicHistory::FOnlineTicHistory(size_t maxBytes)
	: Limit(maxBytes)
{
}

bool FOnlineTicHistory::Append(uint64_t tic, const uint8_t* bytes, size_t size, std::string& error)
{
	if (size == 0 || size > OnlineMaxTicRecordBytes || size > Limit || (bytes == nullptr && size != 0))
	{
		error = "online tic history record exceeds the allowed size";
		return false;
	}
	if (HasLastTic && tic <= LastTic)
	{
		error = "online tic history is not strictly ordered";
		return false;
	}
	while (!Records.empty() && Bytes > Limit - size)
	{
		Bytes -= Records.front().Bytes.size();
		Records.erase(Records.begin());
	}
	FOnlineTicRecord record;
	record.Tic = tic;
	record.Bytes.assign(bytes, bytes + size);
	Bytes += size;
	Records.push_back(std::move(record));
	LastTic = tic;
	HasLastTic = true;
	return true;
}

std::vector<FOnlineTicRecord> FOnlineTicHistory::RecordsAfter(uint64_t tic) const
{
	auto first = std::upper_bound(Records.begin(), Records.end(), tic, [](uint64_t value, const FOnlineTicRecord& record)
	{
		return value < record.Tic;
	});
	return std::vector<FOnlineTicRecord>(first, Records.end());
}

void FOnlineTicHistory::DiscardThrough(uint64_t tic)
{
	auto first = std::upper_bound(Records.begin(), Records.end(), tic, [](uint64_t value, const FOnlineTicRecord& record)
	{
		return value < record.Tic;
	});
	for (auto record = Records.begin(); record != first; ++record)
		Bytes -= record->Bytes.size();
	Records.erase(Records.begin(), first);
}

void FOnlineTicHistory::Clear()
{
	Records.clear();
	Bytes = 0;
	LastTic = 0;
	HasLastTic = false;
}

} // namespace BdOnline
