#include "online_transport.h"

#include <sodium.h>

#include <algorithm>
#include <cstring>
#include <iterator>
#include <limits>

namespace BdOnline
{

bool FOnlineTransport::Configure(EOnlineRoute route, FEncryptedDatagramCodec sendCodec, FEncryptedDatagramCodec receiveCodec, FSendFunction send, std::string& error)
{
	if (!send)
	{
		error = "online transport requires a send function";
		return false;
	}
	ActiveRoute = route;
	SendCodec = std::move(sendCodec);
	ReceiveCodec = std::move(receiveCodec);
	SendFunction = std::move(send);
	Configured = true;
	return true;
}

bool FOnlineTransport::Send(uint32_t epoch, const uint8_t* bytes, size_t size, std::string& error)
{
	if (!Configured)
	{
		error = "online transport is not configured";
		return false;
	}
	FEncryptedDatagram packet;
	return SendCodec.Seal(epoch, bytes, size, packet, error) && SendFunction(ActiveRoute, packet, error);
}

bool FOnlineTransport::Receive(const FEncryptedDatagram& packet, std::vector<uint8_t>& bytes, std::string& error)
{
	if (!Configured)
	{
		error = "online transport is not configured";
		return false;
	}
	return ReceiveCodec.Open(packet, bytes, error);
}

namespace
{

constexpr size_t RelayTokenSize = 16;
constexpr size_t RelayPrefixSize = 4 + RelayTokenSize;
// The relay needs the anonymous recipient label in clear text to route a
// packet to precisely one endpoint. The actual recipient is also present in
// the encrypted payload, which binds this routable hint to the AEAD-protected
// packet before the legacy protocol ever receives it.
constexpr size_t RelayHeaderSize = RelayPrefixSize + 4 + 16 + 16 + 16 + 4 + 8;
// Direct frames retain the session token, sender/recipient labels, epoch, and
// sequence so they are still bound to the exact encrypted session. They omit
// only the relay capability, which is meaningful exclusively to the relay.
constexpr size_t DirectHeaderSize = RelayPrefixSize + 4 + 16 + 16 + 4 + 8;
constexpr uint8_t RelayPrefix[] = { 'B', 'D', 'O', 'R' };
constexpr uint8_t DirectPrefix[] = { 'B', 'D', 'O', 'D' };
constexpr uint8_t RelayWireMagic[] = { 'B', 'D', 'N', '1' };

bool DecodeHex(const std::string& value, uint8_t* output, size_t outputSize)
{
	return value.size() == outputSize * 2 &&
		sodium_hex2bin(output, outputSize, value.c_str(), value.size(), nullptr, nullptr, nullptr) == 0;
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

uint32_t ReadU32(const uint8_t* input)
{
	uint32_t value = 0;
	for (size_t index = 0; index < 4; ++index)
		value |= static_cast<uint32_t>(input[index]) << (index * 8);
	return value;
}

uint64_t ReadU64(const uint8_t* input)
{
	uint64_t value = 0;
	for (size_t index = 0; index < 8; ++index)
		value |= static_cast<uint64_t>(input[index]) << (index * 8);
	return value;
}

} // namespace

bool FOnlineRelayTransport::ParsePeerId(const std::string& value, FPeerId& output)
{
	return DecodeHex(value, output.data(), output.size());
}

bool FOnlineRelayTransport::MakePeerCodec(const FPeerId& peer, FEncryptedDatagramCodec& codec, std::string& error) const
{
	std::array<uint8_t, OnlineKeyBytes> peerKey = {};
	crypto_generichash_state state = {};
	if (crypto_generichash_init(&state, SessionKey.data(), SessionKey.size(), peerKey.size()) != 0 ||
		crypto_generichash_update(&state, peer.data(), peer.size()) != 0 ||
		crypto_generichash_final(&state, peerKey.data(), peerKey.size()) != 0)
	{
		error = "could not derive an online peer transport key";
		return false;
	}
	// Each peer receives a distinct derived AEAD key, so nonce reuse cannot
	// occur even if two independent clients happen to start at sequence zero.
	return codec.Initialize(peerKey, 0, error);
}

bool FOnlineRelayTransport::Configure(const std::string& sessionKeyHex, const std::string& relayTokenHex,
	const std::string& localPeerIdHex, const std::string& relayAuthHex, uint32_t epoch, std::string& error)
{
	if (sodium_init() < 0 || !DecodeHex(sessionKeyHex, SessionKey.data(), SessionKey.size()) ||
		!DecodeHex(relayTokenHex, RelayToken.data(), RelayToken.size()) || !DecodeHex(relayAuthHex, RelayAuth.data(), RelayAuth.size()) ||
		!ParsePeerId(localPeerIdHex, LocalPeer) || epoch == 0)
	{
		error = "online relay credentials are invalid";
		return false;
	}
	FEncryptedDatagramCodec sendCodec;
	if (!MakePeerCodec(LocalPeer, sendCodec, error))
		return false;
	SendCodec = std::move(sendCodec);
	ReceiveCodecs.clear();
	SessionEpoch = epoch;
	Configured = true;
	return true;
}

bool FOnlineRelayTransport::Encapsulate(const FPeerId& target, const uint8_t* bytes, size_t size,
	std::vector<uint8_t>& wire, std::string& error)
{
	if (!Configured || (size != 0 && bytes == nullptr))
	{
		error = "online relay transport is not configured";
		return false;
	}
	if (sodium_memcmp(target.data(), LocalPeer.data(), target.size()) == 0)
	{
		error = "online relay cannot send a packet to its local peer";
		return false;
	}
	if (size > OnlineMaxDatagramBytes - crypto_aead_chacha20poly1305_ietf_ABYTES - target.size())
	{
		error = "online relay payload exceeds the transport limit";
		return false;
	}
	std::vector<uint8_t> plaintext;
	plaintext.reserve(target.size() + size);
	plaintext.insert(plaintext.end(), target.begin(), target.end());
	plaintext.insert(plaintext.end(), bytes, bytes + size);
	FEncryptedDatagram packet;
	if (!SendCodec.Seal(SessionEpoch, plaintext.data(), plaintext.size(), packet, error))
		return false;
	if (packet.Ciphertext.size() > std::numeric_limits<size_t>::max() - RelayHeaderSize)
	{
		error = "online relay packet is too large";
		return false;
	}
	wire.clear();
	wire.reserve(RelayHeaderSize + packet.Ciphertext.size());
	wire.insert(wire.end(), std::begin(RelayPrefix), std::end(RelayPrefix));
	wire.insert(wire.end(), RelayToken.begin(), RelayToken.end());
	wire.insert(wire.end(), std::begin(RelayWireMagic), std::end(RelayWireMagic));
	wire.insert(wire.end(), LocalPeer.begin(), LocalPeer.end());
	wire.insert(wire.end(), RelayAuth.begin(), RelayAuth.end());
	wire.insert(wire.end(), target.begin(), target.end());
	WriteU32(wire, packet.Epoch);
	WriteU64(wire, packet.Sequence);
	wire.insert(wire.end(), packet.Ciphertext.begin(), packet.Ciphertext.end());
	return true;
}

bool FOnlineRelayTransport::Decapsulate(const uint8_t* wire, size_t size, FPeerId& sender,
	std::vector<uint8_t>& bytes, std::string& error)
{
	if (!Configured || wire == nullptr || size < RelayHeaderSize + crypto_aead_chacha20poly1305_ietf_ABYTES + LocalPeer.size())
	{
		error = "online relay packet is invalid";
		return false;
	}
	if (std::memcmp(wire, RelayPrefix, sizeof(RelayPrefix)) != 0 ||
		sodium_memcmp(wire + sizeof(RelayPrefix), RelayToken.data(), RelayToken.size()) != 0 ||
		std::memcmp(wire + RelayPrefixSize, RelayWireMagic, sizeof(RelayWireMagic)) != 0)
	{
		error = "online relay packet is for a different session";
		return false;
	}
	std::copy_n(wire + RelayPrefixSize + sizeof(RelayWireMagic), sender.size(), sender.begin());
	if (sodium_memcmp(sender.data(), LocalPeer.data(), sender.size()) == 0)
	{
		error = "online relay reflected a local packet";
		return false;
	}
	FPeerId recipient = {};
	std::copy_n(wire + RelayPrefixSize + sizeof(RelayWireMagic) + sender.size() + RelayAuth.size(), recipient.size(), recipient.begin());
	if (sodium_memcmp(recipient.data(), LocalPeer.data(), recipient.size()) != 0)
	{
		error = "online relay packet was addressed to a different peer";
		return false;
	}
	const size_t packetOffset = RelayPrefixSize + sizeof(RelayWireMagic) + sender.size() + RelayAuth.size() + recipient.size();
	FEncryptedDatagram packet;
	packet.Epoch = ReadU32(wire + packetOffset);
	packet.Sequence = ReadU64(wire + packetOffset + 4);
	packet.Ciphertext.assign(wire + RelayHeaderSize, wire + size);
	if (packet.Epoch != SessionEpoch)
	{
		error = "online relay packet has an outdated session epoch";
		return false;
	}
	auto iterator = ReceiveCodecs.find(sender);
	if (iterator == ReceiveCodecs.end())
	{
		FEncryptedDatagramCodec codec;
		if (!MakePeerCodec(sender, codec, error))
			return false;
		iterator = ReceiveCodecs.emplace(sender, std::move(codec)).first;
	}
	std::vector<uint8_t> plaintext;
	if (!iterator->second.Open(packet, plaintext, error))
		return false;
	if (plaintext.size() < LocalPeer.size() || sodium_memcmp(plaintext.data(), LocalPeer.data(), LocalPeer.size()) != 0)
	{
		error = "online relay packet was addressed to a different peer";
		return false;
	}
	bytes.assign(plaintext.begin() + LocalPeer.size(), plaintext.end());
	return true;
}

bool FOnlineRelayTransport::EncapsulateDirect(const FPeerId& target, const uint8_t* bytes, size_t size,
	std::vector<uint8_t>& wire, std::string& error)
{
	if (!Configured || (size != 0 && bytes == nullptr))
	{
		error = "online direct transport is not configured";
		return false;
	}
	if (sodium_memcmp(target.data(), LocalPeer.data(), target.size()) == 0)
	{
		error = "online direct transport cannot send a packet to its local peer";
		return false;
	}
	if (size > OnlineMaxDatagramBytes - crypto_aead_chacha20poly1305_ietf_ABYTES - target.size())
	{
		error = "online direct payload exceeds the transport limit";
		return false;
	}
	std::vector<uint8_t> plaintext;
	plaintext.reserve(target.size() + size);
	plaintext.insert(plaintext.end(), target.begin(), target.end());
	plaintext.insert(plaintext.end(), bytes, bytes + size);
	FEncryptedDatagram packet;
	if (!SendCodec.Seal(SessionEpoch, plaintext.data(), plaintext.size(), packet, error))
		return false;
	if (packet.Ciphertext.size() > std::numeric_limits<size_t>::max() - DirectHeaderSize)
	{
		error = "online direct packet is too large";
		return false;
	}
	wire.clear();
	wire.reserve(DirectHeaderSize + packet.Ciphertext.size());
	wire.insert(wire.end(), std::begin(DirectPrefix), std::end(DirectPrefix));
	wire.insert(wire.end(), RelayToken.begin(), RelayToken.end());
	wire.insert(wire.end(), std::begin(RelayWireMagic), std::end(RelayWireMagic));
	wire.insert(wire.end(), LocalPeer.begin(), LocalPeer.end());
	wire.insert(wire.end(), target.begin(), target.end());
	WriteU32(wire, packet.Epoch);
	WriteU64(wire, packet.Sequence);
	wire.insert(wire.end(), packet.Ciphertext.begin(), packet.Ciphertext.end());
	return true;
}

bool FOnlineRelayTransport::DecapsulateDirect(const uint8_t* wire, size_t size, FPeerId& sender,
	std::vector<uint8_t>& bytes, std::string& error)
{
	if (!Configured || wire == nullptr || size < DirectHeaderSize + crypto_aead_chacha20poly1305_ietf_ABYTES + LocalPeer.size())
	{
		error = "online direct packet is invalid";
		return false;
	}
	if (std::memcmp(wire, DirectPrefix, sizeof(DirectPrefix)) != 0 ||
		sodium_memcmp(wire + sizeof(DirectPrefix), RelayToken.data(), RelayToken.size()) != 0 ||
		std::memcmp(wire + RelayPrefixSize, RelayWireMagic, sizeof(RelayWireMagic)) != 0)
	{
		error = "online direct packet is for a different session";
		return false;
	}
	std::copy_n(wire + RelayPrefixSize + sizeof(RelayWireMagic), sender.size(), sender.begin());
	if (sodium_memcmp(sender.data(), LocalPeer.data(), sender.size()) == 0)
	{
		error = "online direct packet was reflected locally";
		return false;
	}
	FPeerId recipient = {};
	std::copy_n(wire + RelayPrefixSize + sizeof(RelayWireMagic) + sender.size(), recipient.size(), recipient.begin());
	if (sodium_memcmp(recipient.data(), LocalPeer.data(), recipient.size()) != 0)
	{
		error = "online direct packet was addressed to a different peer";
		return false;
	}
	const size_t packetOffset = RelayPrefixSize + sizeof(RelayWireMagic) + sender.size() + recipient.size();
	FEncryptedDatagram packet;
	packet.Epoch = ReadU32(wire + packetOffset);
	packet.Sequence = ReadU64(wire + packetOffset + 4);
	packet.Ciphertext.assign(wire + DirectHeaderSize, wire + size);
	if (packet.Epoch != SessionEpoch)
	{
		error = "online direct packet has an outdated session epoch";
		return false;
	}
	auto iterator = ReceiveCodecs.find(sender);
	if (iterator == ReceiveCodecs.end())
	{
		FEncryptedDatagramCodec codec;
		if (!MakePeerCodec(sender, codec, error))
			return false;
		iterator = ReceiveCodecs.emplace(sender, std::move(codec)).first;
	}
	std::vector<uint8_t> plaintext;
	if (!iterator->second.Open(packet, plaintext, error))
		return false;
	if (plaintext.size() < LocalPeer.size() || sodium_memcmp(plaintext.data(), LocalPeer.data(), LocalPeer.size()) != 0)
	{
		error = "online direct packet was addressed to a different peer";
		return false;
	}
	bytes.assign(plaintext.begin() + LocalPeer.size(), plaintext.end());
	return true;
}

} // namespace BdOnline
