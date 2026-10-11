#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

// The online layer deliberately uses standard C++ types at its boundary.  It
// is used before the engine has mounted a game, so it must be usable by both
// the launcher and the normal command-line startup path.
namespace BdOnline
{

// Version 2 adds the closed-lobby snapshot join contract. Older clients must
// reject it before they can mistake an in-progress ticket for the legacy
// pre-game handshake.
inline constexpr uint32_t OnlineProtocolVersion = 2;
inline constexpr size_t OnlineDigestBytes = 32;
inline constexpr size_t OnlineKeyBytes = 32;
inline constexpr size_t OnlineMaxContentPackages = 256;
inline constexpr uint64_t OnlineMaxContentPackageBytes = 2ull * 1024 * 1024 * 1024;
inline constexpr uint64_t OnlineMaxContentBytes = 8ull * 1024 * 1024 * 1024;
inline constexpr uint64_t OnlineDefaultContentCacheBytes = 8ull * 1024 * 1024 * 1024;
inline constexpr size_t OnlineMaxArchiveEntries = 100000;
// Archives are inspected before they are mounted. Keep the container small
// enough that a hostile central directory cannot force an excessive allocator
// request in the ZIP reader. Large standalone WADs remain covered by the
// broader per-package limit above.
inline constexpr uint64_t OnlineMaxArchiveContainerBytes = 512ull * 1024 * 1024;
inline constexpr uint64_t OnlineMaxArchiveUncompressedBytes = 8ull * 1024 * 1024 * 1024;
inline constexpr size_t OnlineMaxDatagramBytes = 16 * 1024;

enum class ESessionVisibility : uint8_t
{
	Private,
	Public,
};

enum class EOnlineSessionPhase : uint8_t
{
	Idle,
	Preflight,
	Advertising,
	Joining,
	SynchronizingContent,
	SynchronizingWorld,
	Connected,
	RecoveringHost,
	Failed,
};

// This is populated from the resolved launch plan, not from a user-provided
// display string. Absolute paths never leave the local machine.
struct FPackageSource
{
	std::filesystem::path Path;
	std::string DisplayName;
	uint32_t LoadOrder = 0;
	bool IsIWAD = false;
	bool IsEngineResource = false;
};

struct FContentPackage
{
	std::string DisplayName;
	std::string Digest; // lower-case BLAKE2b-256 hex
	std::string FileExtension; // retained so the cache can mount the original container type
	uint64_t Size = 0;
	uint32_t LoadOrder = 0;
	bool IsDirectoryBundle = false;
};

// The exact local file that backs an advertised package. For a directory mod,
// this is the canonical, session-scoped PK3 rather than the original tree.
struct FResolvedContentPackage
{
	FContentPackage Package;
	std::filesystem::path MountPath;
};

struct FContentManifest
{
	uint32_t Version = OnlineProtocolVersion;
	std::vector<FContentPackage> Packages;
	std::string RootDigest;
	uint64_t TotalBytes = 0;

	bool Validate(std::string& error) const;
	bool Empty() const { return Packages.empty(); }
};

struct FSessionManifest
{
	uint32_t ProtocolVersion = OnlineProtocolVersion;
	std::string EngineVersion;
	std::string IWADDigest;
	std::string Title;
	std::string Region;
	// Safe display metadata for public browsing; gameplay still validates the
	// authoritative legacy lobby options after connection.
	std::string GameMode;
	ESessionVisibility Visibility = ESessionVisibility::Private;
	uint32_t MaxPlayers = 1;
	bool PasswordRequired = false;
	// Joining an already-running game requires a verified world snapshot. Keep
	// this opt-in false until that protocol is available end-to-end.
	bool AllowJoinInProgress = false;
	FContentManifest Content;

	bool Validate(std::string& error) const;
};

struct FJoinTicket
{
	std::string SessionId;
	std::string Ticket;
	uint64_t ExpiresAtUnixSeconds = 0;
	uint32_t Epoch = 0;
	// The directory signs this state into the join response so clients can keep
	// the ordinary lobby protocol for a pre-game session and switch to the
	// snapshot path only after the host has entered gameplay.
	bool LobbyOpen = false;
	bool UsesRelay = false;
	std::string Endpoint;
	std::string RelayToken;
	// Per-device capability checked by the relay before it accepts a datagram
	// claiming this anonymous peer label. It is never public listing metadata.
	std::string RelayAuth;
	// Hex-encoded 256-bit session key returned only to authenticated members.
	// It is intentionally never included in a public listing or manifest.
	std::string SessionKey;
	// Anonymous, service-issued device identifiers used only as relay routing
	// labels. They are not public IP addresses or player display names.
	std::string LocalPeerId;
	std::string HostPeerId;
	// Returned only to a signed join request, never a public listing. This is a
	// relay-observed UDP candidate plus the host identity required to verify a
	// direct-route probe; either field may be empty and then relay remains used.
	std::string HostPublicKey;
	std::string HostDirectEndpoint;
};

struct FOnlinePeerAdmission
{
	bool Admitted = false;
	// The directory releases this only to the current host and only for an
	// already-admitted peer. It binds a direct probe to the same anonymous
	// Ed25519 device identity that owns the relay route.
	std::string PublicKey;
	std::string DirectEndpoint;
};

struct FOnlineSession
{
	ESessionVisibility Visibility = ESessionVisibility::Private;
	EOnlineSessionPhase Phase = EOnlineSessionPhase::Idle;
	std::string SessionId;
	std::string JoinCode;
	std::string RelayToken;
	std::string RelayAuth;
	std::string Endpoint;
	std::string SessionKey;
	std::string LocalPeerId;
	std::string HostPeerId;
	uint32_t Epoch = 0;
	FSessionManifest Manifest;
	std::string LastError;

	void Fail(std::string error);
	bool Transition(EOnlineSessionPhase expected, EOnlineSessionPhase next, std::string& error);
};

struct FEncryptedDatagram
{
	uint32_t Epoch = 0;
	uint64_t Sequence = 0;
	std::vector<uint8_t> Ciphertext;
};

// ChaCha20-Poly1305 wraps the existing UDP payload. The relay sees routing
// metadata only; simulation packets and content chunks remain opaque. Each
// sender sharing a session key must be assigned a unique Direction value so
// `Initialize` derives a direction-scoped AEAD key from the supplied key, so
// the 96-bit IETF nonce can safely contain the full (epoch, sequence) pair.
// Each sender sharing an origin key must still use a unique Direction.
class FEncryptedDatagramCodec
{
public:
	bool Initialize(const std::array<uint8_t, OnlineKeyBytes>& key, uint32_t direction, std::string& error);
	bool Seal(uint32_t epoch, const uint8_t* bytes, size_t size, FEncryptedDatagram& output, std::string& error);
	bool Open(const FEncryptedDatagram& input, std::vector<uint8_t>& output, std::string& error);

private:
	std::array<uint8_t, OnlineKeyBytes> Key = {};
	uint64_t NextSequence = 0;
	uint64_t LargestReceivedSequence = 0;
	uint64_t ReceivedWindow = 0;
	uint32_t Direction = 0;
	bool Initialized = false;
	bool HasReceivedPacket = false;
};

// Hashes a regular file or a directory tree. Directories are represented as a
// canonical tree digest until the launch layer writes its temporary PK3 bundle.
bool OnlineHashContentSource(const FPackageSource& source, FContentPackage& output, std::string& error);
// Converts directory-backed mods into session-local PK3s and returns the
// mount plan used both for the advertised manifest and engine startup. This
// must run before a host publishes a manifest; OnlineBuildContentManifest
// rejects an unmaterialized directory to prevent a tree hash from being
// advertised for different archive bytes.
bool OnlineResolveContentPlan(const std::vector<FPackageSource>& sources, const std::filesystem::path& bundleDirectory,
	std::vector<FResolvedContentPackage>& resolved, FContentManifest& manifest, std::string& error);
bool OnlineBuildContentManifest(const std::vector<FPackageSource>& sources, FContentManifest& output, std::string& error);
bool OnlineVerifyContentFile(const std::filesystem::path& path, const FContentPackage& expected, std::string& error);
// Creates a reproducible temporary PK3 for a directory-backed mod. The caller
// advertises the returned package instead of the source directory.
bool OnlineWriteDirectoryBundle(const FPackageSource& source, const std::filesystem::path& outputPath, FContentPackage& output, std::string& error);

bool OnlineIsSafeCacheKey(const std::string& key);
// An explicit download approval is bound to both the manifest root and the
// anonymous host that presented it. The token is command-line safe and has no
// secret material; it prevents a refreshed ticket from reusing approval for a
// changed plan or a different host.
bool OnlineBuildContentApprovalToken(const std::string& contentRootDigest, const std::string& hostPeerId,
	std::string& token, std::string& error);

class FOnlineContentCache
{
public:
	explicit FOnlineContentCache(std::filesystem::path root);

	std::filesystem::path FinalPath(const FContentPackage& package) const;
	std::filesystem::path PartialPath(const FContentPackage& package) const;
	// Creates and validates the digest bucket used for a transfer. Neither the
	// cache root nor that bucket may be a symlink, so an untrusted manifest can
	// never turn a resumable download into a write outside the content cache.
	bool PreparePackageDirectory(const FContentPackage& package, std::string& error) const;
	bool HasVerified(const FContentPackage& package, std::string& error) const;
	// Returns zero for no resumable partial. Existing partials must be regular
	// files whose size does not exceed the advertised package size; callers use
	// this to reserve only the remaining bytes against the cache quota.
	bool PartialSize(const FContentPackage& package, uint64_t& size, std::string& error) const;
	// Makes room for an upcoming download by evicting least-recently-used,
	// verified cache entries. Entries listed in preserve are never removed;
	// their partial downloads are protected as well so a cancelled join can
	// resume safely.
	bool EnsureSpace(uint64_t incomingBytes, uint64_t quotaBytes, const std::vector<FContentPackage>& preserve, std::string& error) const;
	bool CommitPartial(const FContentPackage& package, std::string& error) const;

private:
	bool ValidateRoot(std::string& error) const;
	std::filesystem::path Root;
};

// Resolves a complete, already-verified manifest from the local cache. A
// false `complete` result is a normal cache miss; an error means an unsafe or
// unreadable cache entry was encountered. This allows a user who approved a
// manifest once to rejoin later without a second approval prompt.
bool OnlineResolveCachedContent(const std::filesystem::path& cacheRoot, const FContentManifest& manifest,
	std::vector<std::filesystem::path>& paths, bool& complete, std::string& error);

} // namespace BdOnline
