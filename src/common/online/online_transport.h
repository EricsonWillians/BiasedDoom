#pragma once

#include "online_session.h"

#include <array>
#include <functional>
#include <map>

namespace BdOnline
{

enum class EOnlineRoute : uint8_t
{
	Direct,
	Relay,
};

// Adapts the legacy packet producer/consumer to a direct or relay route while
// keeping relay payloads encrypted. Socket ownership stays in i_net.cpp.
class FOnlineTransport
{
public:
	using FSendFunction = std::function<bool(EOnlineRoute, const FEncryptedDatagram&, std::string&)>;

	bool Configure(EOnlineRoute route, FEncryptedDatagramCodec sendCodec, FEncryptedDatagramCodec receiveCodec, FSendFunction send, std::string& error);
	bool Send(uint32_t epoch, const uint8_t* bytes, size_t size, std::string& error);
	bool Receive(const FEncryptedDatagram& packet, std::vector<uint8_t>& bytes, std::string& error);
	EOnlineRoute Route() const { return ActiveRoute; }

private:
	EOnlineRoute ActiveRoute = EOnlineRoute::Direct;
	FEncryptedDatagramCodec SendCodec;
	FEncryptedDatagramCodec ReceiveCodec;
	FSendFunction SendFunction;
	bool Configured = false;
};

// Wire adapter for the official UDP relay. The relay sees the `BDOR` routing
// token, anonymous sender/recipient labels, and sender-bound capability
// before opaque bytes; it cannot inspect the legacy packet or game traffic.
// The recipient label lets it forward only to that member's endpoint. Peer
// IDs are anonymous 128-bit service device IDs, never IPs.
class FOnlineRelayTransport
{
public:
	using FPeerId = std::array<uint8_t, 16>;

	bool Configure(const std::string& sessionKeyHex, const std::string& relayTokenHex,
		const std::string& localPeerIdHex, const std::string& relayAuthHex, uint32_t epoch, std::string& error);
	bool Encapsulate(const FPeerId& target, const uint8_t* bytes, size_t size,
		std::vector<uint8_t>& wire, std::string& error);
	bool Decapsulate(const uint8_t* wire, size_t size, FPeerId& sender,
		std::vector<uint8_t>& bytes, std::string& error);
	// A successfully authenticated direct route uses the same opaque payload as
	// the relay, but deliberately omits the relay-only sender capability. The
	// engine enables this format only after the directory has matched the UDP
	// source to a signed device identity; accepting it eagerly would allow a
	// session member that knows the group key to impersonate another member.
	bool EncapsulateDirect(const FPeerId& target, const uint8_t* bytes, size_t size,
		std::vector<uint8_t>& wire, std::string& error);
	bool DecapsulateDirect(const uint8_t* wire, size_t size, FPeerId& sender,
		std::vector<uint8_t>& bytes, std::string& error);

	bool IsConfigured() const { return Configured; }
	uint32_t Epoch() const { return SessionEpoch; }
	const FPeerId& LocalPeerId() const { return LocalPeer; }
	static bool ParsePeerId(const std::string& value, FPeerId& output);

private:
	bool MakePeerCodec(const FPeerId& peer, FEncryptedDatagramCodec& codec, std::string& error) const;

	std::array<uint8_t, OnlineKeyBytes> SessionKey = {};
	std::array<uint8_t, 16> RelayToken = {};
	std::array<uint8_t, 16> RelayAuth = {};
	FPeerId LocalPeer = {};
	uint32_t SessionEpoch = 0;
	FEncryptedDatagramCodec SendCodec;
	std::map<FPeerId, FEncryptedDatagramCodec> ReceiveCodecs;
	bool Configured = false;
};

} // namespace BdOnline
