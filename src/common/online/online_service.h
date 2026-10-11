#pragma once

#include "online_session.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace BdOnline
{

// Long-lived private-key storage belongs to the platform credential store.
// This value object deliberately has no filesystem persistence policy; the
// launcher can export/import it only through a platform-approved store.
class FOnlineDeviceIdentity
{
public:
	FOnlineDeviceIdentity() = default;
	~FOnlineDeviceIdentity();
	FOnlineDeviceIdentity(const FOnlineDeviceIdentity&) = delete;
	FOnlineDeviceIdentity& operator=(const FOnlineDeviceIdentity&) = delete;
	FOnlineDeviceIdentity(FOnlineDeviceIdentity&&) = delete;
	FOnlineDeviceIdentity& operator=(FOnlineDeviceIdentity&&) = delete;

	bool Generate(std::string& error);
	bool ImportPrivateKeyBase64(const std::string& encoded, std::string& error);
	std::string ExportPrivateKeyBase64() const;
	std::string PublicKeyBase64() const;
	bool SignControlRequest(const std::string& method, const std::string& path, const std::string& timestamp,
		const std::string& nonce, const std::string& body, std::string& signature, std::string& error) const;
	// Signs a compact binary protocol record without ever exposing the private
	// key. Relay liveness uses this rather than trusting a sender label in an
	// otherwise opaque UDP packet.
	bool SignDetached(const uint8_t* message, size_t size, std::vector<uint8_t>& signature, std::string& error) const;
	// Verifies an Ed25519 signature made by a directory-registered device key.
	// Keeping the decoder here avoids giving engine call sites raw key storage.
	static bool VerifyDetached(const std::string& publicKeyBase64, const uint8_t* message, size_t size,
		const uint8_t* signature, size_t signatureSize, std::string& error);
	bool IsValid() const { return Valid; }

private:
	std::array<uint8_t, 32> PublicKey = {};
	std::array<uint8_t, 64> PrivateKey = {};
	bool Valid = false;
};

struct FOnlineServiceConfig
{
	std::string BaseUrl;
	std::string AccessToken;
	std::string DeviceId;
	const FOnlineDeviceIdentity* Identity = nullptr;
	long ConnectTimeoutSeconds = 10;
	long RequestTimeoutSeconds = 30;
};

struct FOnlineSessionListing
{
	std::string SessionId;
	std::string JoinCode;
	std::string Title;
	std::string Region;
	std::string GameMode;
	uint32_t Players = 0;
	// Active players with a short reconnect reservation are reported
	// separately so the browser never presents a reserved-full session as an
	// unexpectedly joinable slot.
	uint32_t ReservedPlayers = 0;
	uint32_t MaxPlayers = 0;
	bool PasswordRequired = false;
	bool Modded = false;
	bool AllowJoinInProgress = false;
	// False means the host has entered gameplay. Such a session remains
	// joinable only when AllowJoinInProgress is true.
	bool LobbyOpen = false;
};

// Returned only to the authenticated departing host. The new transport epoch
// is distributed to connected peers over the already encrypted relay path;
// it is never present in a public listing.
struct FOnlineHostMigration
{
	std::string HostPeerId;
	uint32_t Epoch = 0;
	bool RecoveredAfterHostLoss = false;
};

// Synchronous control-plane client. It is intentionally launcher-safe; engine
// callers schedule it away from the simulation tick.
class FOnlineServiceClient
{
public:
	explicit FOnlineServiceClient(FOnlineServiceConfig config);

	// Generates no key material itself. Callers own platform credential storage;
	// a successful registration fills the client with a scoped bearer token and
	// binds future mutating calls to the supplied Ed25519 identity.
	bool RegisterDevice(const FOnlineDeviceIdentity& identity, std::string& error);
	// The password itself never becomes part of a session manifest or listing.
	// It is converted to a fixed-size proof for the authenticated control plane,
	// so an invalid password is rejected before a guest sees/mod-downloads a
	// protected session's manifest.
	bool CreateSession(const FSessionManifest& manifest, const std::string& password, FOnlineSession& session, std::string& error) const;
	bool RenewSession(const FOnlineSession& session, std::string& error) const;
	bool UpdateRoute(const FOnlineSession& session, const std::string& endpoint, std::string& error) const;
	// The service validates the currently connected legacy roster and chooses a
	// deterministic successor from it. Call this before the old host emits its
	// graceful handoff frame; abrupt-host recovery uses a separate path.
	bool MigrateSession(const FOnlineSession& session, const std::vector<std::string>& eligiblePeerIds,
		FOnlineHostMigration& migration, std::string& error) const;
	// Queries the authoritative host epoch and, only after the relay reports a
	// sustained host-silence window, elects a replacement from the supplied
	// connected roster. The result is also used by peers that only need to
	// observe a recovery another member already initiated.
	bool RecoverSession(const FOnlineSession& session, const std::vector<std::string>& eligiblePeerIds,
		FOnlineHostMigration& migration, std::string& error) const;
	bool LeaveSession(const FOnlineSession& session, std::string& error) const;
	bool CloseSession(const FOnlineSession& session, std::string& error) const;
	// Irreversibly marks the pre-game lobby as entered. Snapshot-capable
	// sessions may continue to admit one authenticated join-in-progress guest.
	bool CloseLobby(const FOnlineSession& session, std::string& error) const;
	bool ListSessions(std::vector<FOnlineSessionListing>& sessions, std::string& error) const;
	// Retrieves a manifest and short-lived transfer route for explicit content
	// review. Unlike ResolveJoinCode it never reserves a player slot.
	bool InspectJoinCode(const std::string& joinCode, const std::string& password, FJoinTicket& ticket, FSessionManifest& manifest, std::string& error) const;
	bool ResolveJoinCode(const std::string& joinCode, const std::string& password, FJoinTicket& ticket, FSessionManifest& manifest, std::string& error) const;
	// The relay can temporarily authorize a non-member for encrypted content
	// review. Before a live host starts a snapshot handoff it therefore asks the
	// directory whether this anonymous peer owns an actual session admission.
	// This endpoint exposes no roster or address data and is host-authorized.
	bool IsPeerAdmitted(const FOnlineSession& session, const std::string& peerId, FOnlinePeerAdmission& admission, std::string& error) const;
	bool ReportSession(const std::string& sessionId, const std::string& reason, std::string& error) const;
	const std::string& AccessToken() const { return Config.AccessToken; }
	const std::string& DeviceId() const { return Config.DeviceId; }

private:
	bool Request(const char* method, const std::string& path, const std::string& body, long& status, std::string& response, std::string& error) const;
	bool ResolveJoinCodeInternal(const std::string& joinCode, const std::string& password, bool preflight, FJoinTicket& ticket,
		FSessionManifest& manifest, std::string& error) const;

	FOnlineServiceConfig Config;
};

} // namespace BdOnline
