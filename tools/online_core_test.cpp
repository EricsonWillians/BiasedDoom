#include "common/online/online_session.h"
#include "common/online/online_service.h"
#include "common/online/online_join.h"
#include "common/online/online_snapshot.h"
#include "common/online/online_transport.h"

#include <chrono>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <iostream>

#include <miniz.h>

using namespace BdOnline;

namespace
{

[[noreturn]] void Fail(const char* message)
{
	std::cerr << "FAIL: " << message << '\n';
	std::exit(1);
}

void Require(bool value, const char* message)
{
	if (!value)
		Fail(message);
}

void RequireSuccess(bool value, const std::string& error)
{
	if (!value)
		Fail(error.c_str());
}

void WriteFile(const std::filesystem::path& path, const std::string& contents)
{
	std::filesystem::create_directories(path.parent_path());
	std::ofstream output(path, std::ios::binary | std::ios::trunc);
	Require(static_cast<bool>(output), "could not create fixture file");
	output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
	Require(static_cast<bool>(output), "could not write fixture file");
}

} // namespace

int main()
{
	const auto root = std::filesystem::temp_directory_path() / "biaseddoom-online-core-test";
	std::error_code code;
	std::filesystem::remove_all(root, code);
	WriteFile(root / "mods" / "alpha.wad", "alpha-content");
	WriteFile(root / "mods" / "directory" / "actors" / "thing.txt", "directory-content");
	WriteFile(root / "mods" / "malformed.pk3", "this is not a zip archive");

	std::vector<FPackageSource> sources = {
		{ root / "doom2.wad", "doom2.wad", 0, true, false },
		{ root / "mods" / "alpha.wad", "alpha.wad", 1, false, false },
		{ root / "mods" / "directory", "directory", 2, false, false },
	};
	std::string error;
	FContentManifest manifest;
	std::vector<FResolvedContentPackage> resolved;
	RequireSuccess(OnlineResolveContentPlan(sources, root / "bundles", resolved, manifest, error), error);
	Require(manifest.Packages.size() == 2, "manifest included local IWAD");
	Require(!manifest.RootDigest.empty() && manifest.Validate(error), "manifest did not validate");
	FContentManifest alteredRoot = manifest;
	alteredRoot.RootDigest[0] = alteredRoot.RootDigest[0] == '0' ? '1' : '0';
	Require(!alteredRoot.Validate(error), "manifest accepted a root digest that does not cover its packages");
	FContentManifest alteredPackage = manifest;
	alteredPackage.Packages[0].DisplayName = "substituted.wad";
	Require(!alteredPackage.Validate(error), "manifest accepted packages that no longer match its root digest");
	FPackageSource zeroOrderSource = sources[1];
	zeroOrderSource.LoadOrder = 0;
	FContentManifest zeroOrderManifest;
	Require(!OnlineBuildContentManifest(std::vector<FPackageSource>{ zeroOrderSource }, zeroOrderManifest, error),
		"manifest accepted a transferable package in the reserved IWAD load-order slot");
	Require(resolved.size() == 2 && resolved[1].Package.IsDirectoryBundle, "directory source was not canonicalized");
	RequireSuccess(OnlineVerifyContentFile(resolved[1].MountPath, resolved[1].Package, error), error);
	Require(!OnlineBuildContentManifest(sources, manifest, error), "manifest accepted an unmaterialized directory");
	FSessionManifest unsafeMetadata;
	unsafeMetadata.EngineVersion = "4.15.19";
	unsafeMetadata.IWADDigest = std::string(OnlineDigestBytes * 2, 'a');
	unsafeMetadata.Title = "unsafe\nlisting";
	unsafeMetadata.GameMode = "co-op";
	unsafeMetadata.MaxPlayers = 2;
	Require(!unsafeMetadata.Validate(error), "manifest accepted unsafe public metadata");
	FSessionManifest unsupportedProtocol;
	unsupportedProtocol.ProtocolVersion = OnlineProtocolVersion + 1;
	unsupportedProtocol.EngineVersion = "4.15.19";
	unsupportedProtocol.IWADDigest = std::string(OnlineDigestBytes * 2, 'a');
	unsupportedProtocol.Title = "valid metadata";
	unsupportedProtocol.GameMode = "co-op";
	unsupportedProtocol.MaxPlayers = 2;
	Require(!unsupportedProtocol.Validate(error), "manifest accepted an unsupported protocol version");
	FSessionManifest unsupportedJoinInProgress = unsafeMetadata;
	unsupportedJoinInProgress.Title = "valid metadata";
	unsupportedJoinInProgress.AllowJoinInProgress = true;
	Require(!unsupportedJoinInProgress.Validate(error), "manifest advertised unsupported join-in-progress");
	std::string approvalToken;
	RequireSuccess(OnlineBuildContentApprovalToken(manifest.RootDigest, std::string(32, 'c'), approvalToken, error), error);
	Require(approvalToken == manifest.RootDigest + ":" + std::string(32, 'c'),
		"content approval token did not bind the reviewed root and host");
	Require(!OnlineBuildContentApprovalToken("not-a-digest", std::string(32, 'c'), approvalToken, error),
		"content approval token accepted an invalid root");
	Require(!OnlineBuildContentApprovalToken(manifest.RootDigest, "different-host", approvalToken, error),
		"content approval token accepted an invalid host identity");

	// Join admission control stays separate from snapshot transport. It binds a
	// relay-authenticated peer request to one slot and the fresh handoff ID
	// before any state is streamed, so late/retried controls cannot activate a
	// different handoff.
	FOnlineJoinRequestFrame joinRequest;
	joinRequest.Epoch = 3;
	RequireSuccess(OnlineGenerateJoinRequestId(joinRequest.RequestId, error), error);
	Require(joinRequest.RequestId != 0, "online join request identity was zero");
	joinRequest.UserInfo = { '\\', 'n', 'a', 'm', 'e', '\\', 'J', 'o', 'i', 'n', 'e', 'r', 0 };
	std::vector<uint8_t> joinWire;
	RequireSuccess(OnlineEncodeJoinRequest(joinRequest, joinWire, error), error);
	FOnlineJoinRequestFrame decodedJoinRequest;
	RequireSuccess(OnlineDecodeJoinRequest(joinWire.data(), joinWire.size(), decodedJoinRequest, error), error);
	Require(OnlineIsJoinFrame(joinWire.data(), joinWire.size()) && decodedJoinRequest.RequestId == joinRequest.RequestId &&
		decodedJoinRequest.UserInfo == joinRequest.UserInfo, "online join request did not round-trip");
	joinWire.back() = 1;
	Require(!OnlineDecodeJoinRequest(joinWire.data(), joinWire.size(), decodedJoinRequest, error),
		"online join request accepted an unterminated userinfo payload");
	joinRequest.UserInfo = { '\\', 'n', 'a', 'm', 'e', '\\', 'E', 'v', 'e', 0, 'x', 0 };
	Require(!OnlineEncodeJoinRequest(joinRequest, joinWire, error),
		"online join request accepted userinfo with trailing bytes after its NUL");
	joinRequest.UserInfo = { 'n', 'a', 'm', 'e', 0 };
	Require(!OnlineEncodeJoinRequest(joinRequest, joinWire, error),
		"online join request accepted malformed legacy userinfo");
	joinRequest.UserInfo = { '\\', 'n', 'a', 'm', 'e', '\\', 'J', 'o', 'i', 'n', 'e', 'r', 0 };
	FOnlineJoinAcceptFrame joinAccept = { 3, joinRequest.RequestId, 0x4a4f494e5452414eull, 7 };
	RequireSuccess(OnlineEncodeJoinAccept(joinAccept, joinWire, error), error);
	FOnlineJoinAcceptFrame decodedJoinAccept;
	RequireSuccess(OnlineDecodeJoinAccept(joinWire.data(), joinWire.size(), decodedJoinAccept, error), error);
	Require(decodedJoinAccept.TransferId == joinAccept.TransferId && decodedJoinAccept.PlayerSlot == joinAccept.PlayerSlot,
		"online join acceptance did not round-trip");
	FOnlineJoinReadyFrame joinReady = { 3, joinRequest.RequestId, joinAccept.TransferId, joinAccept.PlayerSlot };
	RequireSuccess(OnlineEncodeJoinReady(joinReady, joinWire, error), error);
	FOnlineJoinReadyFrame decodedJoinReady;
	RequireSuccess(OnlineDecodeJoinReady(joinWire.data(), joinWire.size(), decodedJoinReady, error), error);
	Require(decodedJoinReady.RequestId == joinReady.RequestId && decodedJoinReady.TransferId == joinReady.TransferId,
		"online join readiness did not round-trip");
	FOnlineJoinActivatedFrame joinActivated = { 3, joinRequest.RequestId, joinAccept.TransferId, joinAccept.PlayerSlot };
	RequireSuccess(OnlineEncodeJoinActivated(joinActivated, joinWire, error), error);
	FOnlineJoinActivatedFrame decodedJoinActivated;
	RequireSuccess(OnlineDecodeJoinActivated(joinWire.data(), joinWire.size(), decodedJoinActivated, error), error);
	Require(decodedJoinActivated.RequestId == joinActivated.RequestId && decodedJoinActivated.TransferId == joinActivated.TransferId,
		"online join activation acknowledgement did not round-trip");
	FOnlineJoinRejectFrame joinReject = { 3, joinRequest.RequestId, EOnlineJoinRejectReason::Busy };
	RequireSuccess(OnlineEncodeJoinReject(joinReject, joinWire, error), error);
	FOnlineJoinRejectFrame decodedJoinReject;
	RequireSuccess(OnlineDecodeJoinReject(joinWire.data(), joinWire.size(), decodedJoinReject, error), error);
	Require(decodedJoinReject.Reason == EOnlineJoinRejectReason::Busy,
		"online join rejection did not round-trip");
	joinReject.RequestId = 0;
	Require(!OnlineEncodeJoinReject(joinReject, joinWire, error), "online join rejection accepted a missing request identity");

	FContentPackage bundle;
	RequireSuccess(OnlineWriteDirectoryBundle(sources[2], root / "another-bundle" / "directory.pk3", bundle, error), error);
	Require(bundle.IsDirectoryBundle && bundle.FileExtension == ".pk3", "directory bundle metadata is wrong");
	RequireSuccess(OnlineVerifyContentFile(root / "another-bundle" / "directory.pk3", bundle, error), error);
	FContentPackage repeatedBundle;
	RequireSuccess(OnlineWriteDirectoryBundle(sources[2], root / "repeat-bundle" / "directory.pk3", repeatedBundle, error), error);
	Require(repeatedBundle.Digest == bundle.Digest && repeatedBundle.Size == bundle.Size &&
		repeatedBundle.FileExtension == bundle.FileExtension, "directory bundle was not reproducible across session paths");

	FOnlineContentCache cache(root / "cache");
	std::filesystem::create_directories(cache.PartialPath(manifest.Packages[0]).parent_path());
	std::filesystem::copy_file(sources[1].Path, cache.PartialPath(manifest.Packages[0]), std::filesystem::copy_options::overwrite_existing);
	RequireSuccess(cache.CommitPartial(manifest.Packages[0], error), error);
	RequireSuccess(cache.HasVerified(manifest.Packages[0], error), error);
	uint64_t resumableBytes = 999;
	Require(cache.PartialSize(manifest.Packages[0], resumableBytes, error) && resumableBytes == 0,
		"verified content still appeared as a resumable partial");
	WriteFile(root / "mods" / "beta.wad", "beta-content");
	FPackageSource betaSource = { root / "mods" / "beta.wad", "beta.wad", 3, false, false };
	FContentPackage beta;
	RequireSuccess(OnlineHashContentSource(betaSource, beta, error), error);
	std::filesystem::create_directories(cache.PartialPath(beta).parent_path());
	std::filesystem::copy_file(betaSource.Path, cache.PartialPath(beta), std::filesystem::copy_options::overwrite_existing);
	RequireSuccess(cache.CommitPartial(beta, error), error);
	FContentManifest cachedManifest;
	const bool builtCachedManifest = OnlineBuildContentManifest(std::vector<FPackageSource>{ betaSource }, cachedManifest, error);
	RequireSuccess(builtCachedManifest, error);
	std::vector<std::filesystem::path> cachedPaths;
	bool cacheComplete = false;
	Require(OnlineResolveCachedContent(root / "cache", cachedManifest, cachedPaths, cacheComplete, error) && cacheComplete &&
		cachedPaths.size() == 1 && cachedPaths[0] == cache.FinalPath(beta), "verified content did not resolve from cache");
	std::filesystem::last_write_time(cache.FinalPath(manifest.Packages[0]), std::filesystem::file_time_type::clock::now() - std::chrono::hours(1), code);
	Require(!code, "could not age an LRU fixture");
	RequireSuccess(cache.EnsureSpace(manifest.Packages[0].Size,
		manifest.Packages[0].Size + beta.Size, std::vector<FContentPackage>{ beta }, error), error);
	Require(!std::filesystem::exists(cache.FinalPath(manifest.Packages[0])) && cache.HasVerified(beta, error),
		"LRU cache pruning did not preserve the current session content");
	FContentPackage invalidCachePackage = manifest.Packages[0];
	invalidCachePackage.Digest = "../outside-cache";
	Require(cache.FinalPath(invalidCachePackage).empty() && cache.PartialPath(invalidCachePackage).empty(),
		"unsafe cache key escaped the cache root");
	WriteFile(cache.FinalPath(manifest.Packages[0]), std::string(static_cast<size_t>(manifest.Packages[0].Size), '!'));
	Require(!cache.HasVerified(manifest.Packages[0], error) && error.empty() && !std::filesystem::exists(cache.FinalPath(manifest.Packages[0])),
		"cache retained a corrupt verified-content entry");
	std::filesystem::create_directories(cache.PartialPath(manifest.Packages[0]).parent_path());
	const std::string corruptBytes(static_cast<size_t>(manifest.Packages[0].Size), '!');
	WriteFile(cache.PartialPath(manifest.Packages[0]), corruptBytes);
	Require(cache.PartialSize(manifest.Packages[0], resumableBytes, error) && resumableBytes == corruptBytes.size(),
		"cache did not report resumable partial size");
	Require(!cache.CommitPartial(manifest.Packages[0], error), "cache accepted corrupt partial content");
	Require(!std::filesystem::exists(cache.PartialPath(manifest.Packages[0])), "cache retained a terminally corrupt partial download");
	Require(OnlineResolveCachedContent(root / "cache", manifest, cachedPaths, cacheComplete, error) && !cacheComplete && cachedPaths.empty(),
		"incomplete cache was incorrectly accepted as a complete content plan");
	const auto unsafeRoot = root / "unsafe-cache-root";
	std::filesystem::create_directory_symlink(root / "mods", unsafeRoot, code);
	Require(!code, "could not create unsafe cache-root symlink fixture");
	FOnlineContentCache unsafeRootCache(unsafeRoot);
	Require(!unsafeRootCache.PreparePackageDirectory(manifest.Packages[0], error),
		"content cache accepted a symlink root");
	const auto unsafeBucketRoot = root / "unsafe-cache-bucket";
	std::filesystem::create_directories(unsafeBucketRoot, code);
	Require(!code, "could not create unsafe cache-bucket fixture");
	std::filesystem::create_directory_symlink(root / "mods", unsafeBucketRoot / manifest.Packages[0].Digest.substr(0, 2), code);
	Require(!code, "could not create unsafe cache-bucket symlink fixture");
	FOnlineContentCache unsafeBucketCache(unsafeBucketRoot);
	Require(!unsafeBucketCache.PreparePackageDirectory(manifest.Packages[0], error),
		"content cache accepted a symlink bucket");

	FPackageSource malformedSource = { root / "mods" / "malformed.pk3", "malformed.pk3", 3, false, false };
	FContentPackage malformedPackage;
	Require(!OnlineHashContentSource(malformedSource, malformedPackage, error), "malformed PK3 was accepted for hosting");

	std::array<uint8_t, OnlineKeyBytes> key = {};
	for (size_t index = 0; index < key.size(); ++index)
		key[index] = static_cast<uint8_t>(index + 1);
	FEncryptedDatagramCodec sender;
	FEncryptedDatagramCodec receiver;
	RequireSuccess(sender.Initialize(key, 7, error), error);
	RequireSuccess(receiver.Initialize(key, 7, error), error);
	const std::string payload = "trusted packet";
	FEncryptedDatagram first;
	FEncryptedDatagram second;
	RequireSuccess(sender.Seal(3, reinterpret_cast<const uint8_t*>(payload.data()), payload.size(), first, error), error);
	RequireSuccess(sender.Seal(3, reinterpret_cast<const uint8_t*>(payload.data()), payload.size(), second, error), error);
	std::vector<uint8_t> opened;
	RequireSuccess(receiver.Open(second, opened, error), error);
	Require(receiver.Open(first, opened, error), "receiver rejected a valid re-ordered packet");
	Require(!receiver.Open(first, opened, error), "receiver accepted a replayed packet");
	FEncryptedDatagramCodec otherDirection;
	RequireSuccess(otherDirection.Initialize(key, 8, error), error);
	FEncryptedDatagram directionPacket;
	RequireSuccess(otherDirection.Seal(3, reinterpret_cast<const uint8_t*>(payload.data()), payload.size(), directionPacket, error), error);
	FEncryptedDatagramCodec wrongDirectionReceiver;
	RequireSuccess(wrongDirectionReceiver.Initialize(key, 7, error), error);
	Require(!wrongDirectionReceiver.Open(directionPacket, opened, error), "datagram accepted a packet from a different direction key space");
	FOnlineDeviceIdentity directIdentity;
	FOnlineDeviceIdentity wrongDirectIdentity;
	RequireSuccess(directIdentity.Generate(error), error);
	RequireSuccess(wrongDirectIdentity.Generate(error), error);
	const std::array<uint8_t, 5> directProof = { 'B', 'D', 'D', 'P', 1 };
	std::vector<uint8_t> directSignature;
	RequireSuccess(directIdentity.SignDetached(directProof.data(), directProof.size(), directSignature, error), error);
	Require(FOnlineDeviceIdentity::VerifyDetached(directIdentity.PublicKeyBase64(), directProof.data(), directProof.size(),
		directSignature.data(), directSignature.size(), error), "direct route identity proof did not verify");
	Require(!FOnlineDeviceIdentity::VerifyDetached(wrongDirectIdentity.PublicKeyBase64(), directProof.data(), directProof.size(),
		directSignature.data(), directSignature.size(), error), "direct route identity proof verified for the wrong device");

	std::vector<uint8_t> serializedWorld(OnlineSnapshotChunkBytes * 2 + 13);
	for (size_t index = 0; index < serializedWorld.size(); ++index)
		serializedWorld[index] = static_cast<uint8_t>(index * 17u + 3u);
	FOnlineSnapshotBundle snapshotBundle;
	snapshotBundle.MapName = "MAP01";
	FOnlineSnapshotComponent worldComponent;
	worldComponent.Type = EOnlineSnapshotComponentType::World;
	worldComponent.Name = "MAP01";
	worldComponent.UncompressedBytes = serializedWorld.size();
	worldComponent.CompressionMethod = FileSys::METHOD_STORED;
	worldComponent.CRC32 = static_cast<uint32_t>(crc32(0, serializedWorld.data(), serializedWorld.size()));
	worldComponent.Bytes = serializedWorld;
	snapshotBundle.Components.push_back(worldComponent);
	FOnlineSnapshotComponent globalsComponent;
	globalsComponent.Type = EOnlineSnapshotComponentType::Globals;
	globalsComponent.Name = "globals";
	globalsComponent.UncompressedBytes = 4;
	globalsComponent.CompressionMethod = FileSys::METHOD_STORED;
	globalsComponent.Bytes = { 8, 6, 7, 5 };
	globalsComponent.CRC32 = static_cast<uint32_t>(crc32(0, globalsComponent.Bytes.data(), globalsComponent.Bytes.size()));
	snapshotBundle.Components.push_back(globalsComponent);
	std::vector<uint8_t> bundleWire;
	RequireSuccess(OnlineEncodeSnapshotBundle(snapshotBundle, bundleWire, error), error);
	FOnlineSnapshotBundle decodedBundle;
	RequireSuccess(OnlineDecodeSnapshotBundle(bundleWire.data(), bundleWire.size(), decodedBundle, error), error);
	Require(decodedBundle.MapName == snapshotBundle.MapName && decodedBundle.Components.size() == 2 &&
		decodedBundle.Components[0].Bytes == serializedWorld && decodedBundle.Components[1].Bytes == globalsComponent.Bytes,
		"snapshot bundle did not preserve complete restore components");
	RequireSuccess(OnlineValidateSnapshotBundleArchives(decodedBundle, error), error);
	std::vector<uint8_t> compressedWorld(serializedWorld.size() + 1024);
	const size_t compressedWorldSize = tdefl_compress_mem_to_mem(compressedWorld.data(), compressedWorld.size(),
		serializedWorld.data(), serializedWorld.size(), TDEFL_DEFAULT_MAX_PROBES);
	Require(compressedWorldSize != 0, "could not create a raw-deflate snapshot fixture");
	compressedWorld.resize(compressedWorldSize);
	FOnlineSnapshotBundle deflatedBundle = snapshotBundle;
	deflatedBundle.Components[0].CompressionMethod = FileSys::METHOD_DEFLATE;
	deflatedBundle.Components[0].Bytes = std::move(compressedWorld);
	RequireSuccess(OnlineValidateSnapshotBundleArchives(deflatedBundle, error), error);
	FOnlineSnapshotBundle invalidComponentCRC = decodedBundle;
	invalidComponentCRC.Components[0].CRC32 ^= 1u;
	Require(!OnlineValidateSnapshotBundleArchives(invalidComponentCRC, error),
		"snapshot bundle accepted a component with a bad CRC");
	FOnlineSnapshotMetadata storedBundleMetadata;
	std::vector<uint8_t> storedBundleBytes;
	RequireSuccess(OnlineMakeStoredSnapshotMetadata(bundleWire.data(), bundleWire.size(), 41519, 3, 12345,
		storedBundleMetadata, storedBundleBytes, error), error);
	Require(storedBundleMetadata.TransferId != 0 && storedBundleMetadata.CompressionMethod == FileSys::METHOD_STORED &&
		storedBundleMetadata.UncompressedBytes == bundleWire.size() && storedBundleBytes == bundleWire,
		"stored snapshot metadata did not preserve the encoded bundle");
	FOnlineSnapshotMetadata retryStoredBundleMetadata;
	std::vector<uint8_t> retryStoredBundleBytes;
	RequireSuccess(OnlineMakeStoredSnapshotMetadata(bundleWire.data(), bundleWire.size(), 41519, 3, 12345,
		retryStoredBundleMetadata, retryStoredBundleBytes, error), error);
	Require(retryStoredBundleMetadata.TransferId != storedBundleMetadata.TransferId,
		"a repeated snapshot capture reused its handoff identity");
	FOnlineSnapshotBundle restoredBundle;
	RequireSuccess(OnlineDecodeSnapshotBundle(storedBundleBytes.data(), storedBundleBytes.size(), restoredBundle, error), error);
	Require(restoredBundle.Components.size() == snapshotBundle.Components.size(),
		"stored snapshot bundle could not be decoded after metadata capture");
	RequireSuccess(OnlineValidateSnapshotBundleArchives(restoredBundle, error), error);
	Require(!OnlineMakeStoredSnapshotMetadata(nullptr, 0, 41519, 3, 12345,
		storedBundleMetadata, storedBundleBytes, error),
		"stored snapshot metadata accepted an empty archive");
	bundleWire.pop_back();
	Require(!OnlineDecodeSnapshotBundle(bundleWire.data(), bundleWire.size(), decodedBundle, error),
		"snapshot bundle accepted a truncated component");
	FOnlineSnapshotBundle duplicateBundle = snapshotBundle;
	duplicateBundle.Components.push_back(worldComponent);
	Require(!duplicateBundle.Validate(error), "snapshot bundle accepted a duplicate component");
	FOnlineSnapshotBundle expandedBundle = snapshotBundle;
	expandedBundle.Components[0].CompressionMethod = FileSys::METHOD_DEFLATE;
	expandedBundle.Components[0].Bytes = { 0x00 };
	expandedBundle.Components[0].UncompressedBytes = OnlineMaxSnapshotBytes;
	Require(!expandedBundle.Validate(error), "snapshot bundle accepted components that expand past its aggregate cap");
	FOnlineSnapshotMetadata snapshotMetadata;
	snapshotMetadata.SerializerVersion = 41519;
	snapshotMetadata.Epoch = 3;
	snapshotMetadata.SnapshotTic = 12345;
	snapshotMetadata.TransferId = 0x4a4f494e534e4150ull;
	snapshotMetadata.TotalBytes = serializedWorld.size();
	snapshotMetadata.UncompressedBytes = serializedWorld.size();
	snapshotMetadata.CompressionMethod = FileSys::METHOD_STORED;
	snapshotMetadata.CRC32 = 0x12345678u;
	RequireSuccess(OnlineHashSnapshot(serializedWorld.data(), serializedWorld.size(), snapshotMetadata.Digest, error), error);
	FOnlineSnapshotMetadata invalidSnapshotMetadata = snapshotMetadata;
	invalidSnapshotMetadata.CompressionMethod = FileSys::METHOD_LZMA;
	Require(!invalidSnapshotMetadata.Validate(error), "snapshot accepted an unsupported serializer compression method");
	invalidSnapshotMetadata = snapshotMetadata;
	invalidSnapshotMetadata.UncompressedBytes = OnlineMaxSnapshotBytes + 1;
	Require(!invalidSnapshotMetadata.Validate(error), "snapshot accepted an oversized expanded serializer archive");
	FileSys::FCompressedBuffer serializerBuffer = { serializedWorld.size(), serializedWorld.size(), FileSys::METHOD_STORED,
		snapshotMetadata.CRC32, reinterpret_cast<char*>(serializedWorld.data()), nullptr };
	FOnlineSnapshotMetadata capturedMetadata;
	std::vector<uint8_t> capturedSnapshot;
	RequireSuccess(OnlineMakeSnapshotMetadata(serializerBuffer, snapshotMetadata.SerializerVersion, snapshotMetadata.Epoch,
		snapshotMetadata.SnapshotTic, capturedMetadata, capturedSnapshot, error), error);
	Require(capturedMetadata.TotalBytes == serializedWorld.size() && capturedMetadata.UncompressedBytes == serializedWorld.size() &&
		capturedMetadata.CompressionMethod == FileSys::METHOD_STORED && capturedSnapshot == serializedWorld,
		"engine snapshot metadata did not preserve the serializer archive");
	FileSys::FCompressedBuffer restoredArchive = { 0, 0, 0, 0, nullptr, nullptr };
	RequireSuccess(OnlineRestoreSnapshotArchive(capturedMetadata, capturedSnapshot.data(), capturedSnapshot.size(), restoredArchive, error), error);
	Require(restoredArchive.mSize == serializedWorld.size() && restoredArchive.mCompressedSize == serializedWorld.size() &&
		restoredArchive.mMethod == FileSys::METHOD_STORED && restoredArchive.mCRC32 == snapshotMetadata.CRC32 &&
		std::memcmp(restoredArchive.mBuffer, serializedWorld.data(), serializedWorld.size()) == 0,
		"engine snapshot archive did not round-trip");
	restoredArchive.Clean();
	capturedSnapshot[0] ^= 0xff;
	Require(!OnlineRestoreSnapshotArchive(capturedMetadata, capturedSnapshot.data(), capturedSnapshot.size(), restoredArchive, error),
		"snapshot archive accepted a digest mismatch");
	std::vector<uint8_t> snapshotWire;
	RequireSuccess(OnlineEncodeSnapshotBegin(snapshotMetadata, 3, snapshotWire, error), error);
	FOnlineSnapshotMetadata decodedSnapshotMetadata;
	size_t decodedChunkCount = 0;
	RequireSuccess(OnlineDecodeSnapshotBegin(snapshotWire.data(), snapshotWire.size(), decodedSnapshotMetadata, decodedChunkCount, error), error);
	Require(decodedChunkCount == 3 && decodedSnapshotMetadata.Digest == snapshotMetadata.Digest &&
		decodedSnapshotMetadata.SnapshotTic == snapshotMetadata.SnapshotTic &&
		decodedSnapshotMetadata.TransferId == snapshotMetadata.TransferId, "snapshot begin frame did not round-trip");
	FOnlineSnapshotAckFrame beginAcknowledgement;
	beginAcknowledgement.Epoch = snapshotMetadata.Epoch;
	beginAcknowledgement.SnapshotTic = snapshotMetadata.SnapshotTic;
	beginAcknowledgement.TransferId = snapshotMetadata.TransferId;
	beginAcknowledgement.Digest = snapshotMetadata.Digest;
	beginAcknowledgement.ChunkIndex = OnlineSnapshotBeginAcknowledgement;
	RequireSuccess(OnlineEncodeSnapshotAck(beginAcknowledgement, snapshotWire, error), error);
	FOnlineSnapshotAckFrame decodedAcknowledgement;
	RequireSuccess(OnlineDecodeSnapshotAck(snapshotWire.data(), snapshotWire.size(), decodedAcknowledgement, error), error);
	Require(decodedAcknowledgement.ChunkIndex == OnlineSnapshotBeginAcknowledgement &&
		decodedAcknowledgement.TransferId == snapshotMetadata.TransferId &&
		decodedAcknowledgement.Digest == snapshotMetadata.Digest, "snapshot begin acknowledgement did not round-trip");
	FOnlineSnapshotReplayFrame replayFrame;
	replayFrame.Epoch = snapshotMetadata.Epoch;
	replayFrame.SnapshotTic = snapshotMetadata.SnapshotTic;
	replayFrame.TransferId = snapshotMetadata.TransferId;
	replayFrame.Tic = snapshotMetadata.SnapshotTic + 1;
	replayFrame.Digest = snapshotMetadata.Digest;
	replayFrame.Bytes = { 1, 4, 9, 16 };
	RequireSuccess(OnlineEncodeSnapshotReplayTic(replayFrame, snapshotWire, error), error);
	FOnlineSnapshotReplayFrame decodedReplayFrame;
	RequireSuccess(OnlineDecodeSnapshotReplayTic(snapshotWire.data(), snapshotWire.size(), decodedReplayFrame, error), error);
	Require(decodedReplayFrame.Tic == replayFrame.Tic && decodedReplayFrame.TransferId == replayFrame.TransferId &&
		decodedReplayFrame.Digest == replayFrame.Digest &&
		decodedReplayFrame.Bytes == replayFrame.Bytes, "snapshot replay frame did not round-trip");
	const std::vector<uint8_t> replayWire = snapshotWire;
	FOnlineSnapshotCompleteFrame completionFence;
	completionFence.Epoch = snapshotMetadata.Epoch;
	completionFence.SnapshotTic = snapshotMetadata.SnapshotTic;
	completionFence.TransferId = snapshotMetadata.TransferId;
	completionFence.LiveTic = replayFrame.Tic + 1;
	completionFence.Digest = snapshotMetadata.Digest;
	RequireSuccess(OnlineEncodeSnapshotComplete(completionFence, snapshotWire, error), error);
	FOnlineSnapshotCompleteFrame decodedCompletionFence;
	RequireSuccess(OnlineDecodeSnapshotComplete(snapshotWire.data(), snapshotWire.size(), decodedCompletionFence, error), error);
	Require(decodedCompletionFence.LiveTic == completionFence.LiveTic &&
		decodedCompletionFence.TransferId == completionFence.TransferId &&
		decodedCompletionFence.Digest == completionFence.Digest,
		"snapshot completion fence did not round-trip");
	completionFence.LiveTic = completionFence.SnapshotTic - 1;
	Require(!OnlineEncodeSnapshotComplete(completionFence, snapshotWire, error),
		"snapshot completion fence accepted a replay boundary before its snapshot");
	FOnlineSnapshotCompleteAckFrame completionAcknowledgement;
	completionAcknowledgement.Epoch = snapshotMetadata.Epoch;
	completionAcknowledgement.SnapshotTic = snapshotMetadata.SnapshotTic;
	completionAcknowledgement.TransferId = snapshotMetadata.TransferId;
	completionAcknowledgement.LiveTic = replayFrame.Tic + 1;
	completionAcknowledgement.Digest = snapshotMetadata.Digest;
	RequireSuccess(OnlineEncodeSnapshotCompleteAck(completionAcknowledgement, snapshotWire, error), error);
	FOnlineSnapshotCompleteAckFrame decodedCompletionAcknowledgement;
	RequireSuccess(OnlineDecodeSnapshotCompleteAck(snapshotWire.data(), snapshotWire.size(), decodedCompletionAcknowledgement, error), error);
	Require(decodedCompletionAcknowledgement.LiveTic == completionAcknowledgement.LiveTic &&
		decodedCompletionAcknowledgement.TransferId == completionAcknowledgement.TransferId &&
		decodedCompletionAcknowledgement.Digest == completionAcknowledgement.Digest,
		"snapshot completion acknowledgement did not round-trip");
	completionAcknowledgement.LiveTic = completionAcknowledgement.SnapshotTic - 1;
	Require(!OnlineEncodeSnapshotCompleteAck(completionAcknowledgement, snapshotWire, error),
		"snapshot completion acknowledgement accepted a replay boundary before its snapshot");
	FOnlineSnapshotCancelFrame cancellation;
	cancellation.Epoch = snapshotMetadata.Epoch;
	cancellation.SnapshotTic = snapshotMetadata.SnapshotTic;
	cancellation.TransferId = snapshotMetadata.TransferId;
	cancellation.Digest = snapshotMetadata.Digest;
	cancellation.Reason = EOnlineSnapshotCancelReason::ReplayExpired;
	RequireSuccess(OnlineEncodeSnapshotCancel(cancellation, snapshotWire, error), error);
	FOnlineSnapshotCancelFrame decodedCancellation;
	RequireSuccess(OnlineDecodeSnapshotCancel(snapshotWire.data(), snapshotWire.size(), decodedCancellation, error), error);
	Require(decodedCancellation.Reason == EOnlineSnapshotCancelReason::ReplayExpired &&
		decodedCancellation.TransferId == cancellation.TransferId && decodedCancellation.Digest == cancellation.Digest,
		"snapshot cancellation did not round-trip");
	snapshotWire.back() = 0;
	Require(!OnlineDecodeSnapshotCancel(snapshotWire.data(), snapshotWire.size(), decodedCancellation, error),
		"snapshot cancellation accepted an invalid reason");
	FOnlineSnapshotReplayAckFrame replayAcknowledgement;
	replayAcknowledgement.Epoch = snapshotMetadata.Epoch;
	replayAcknowledgement.SnapshotTic = snapshotMetadata.SnapshotTic;
	replayAcknowledgement.TransferId = snapshotMetadata.TransferId;
	replayAcknowledgement.AcknowledgedTic = replayFrame.Tic;
	replayAcknowledgement.Digest = snapshotMetadata.Digest;
	RequireSuccess(OnlineEncodeSnapshotReplayAck(replayAcknowledgement, snapshotWire, error), error);
	FOnlineSnapshotReplayAckFrame decodedReplayAcknowledgement;
	RequireSuccess(OnlineDecodeSnapshotReplayAck(snapshotWire.data(), snapshotWire.size(), decodedReplayAcknowledgement, error), error);
	Require(decodedReplayAcknowledgement.AcknowledgedTic == replayAcknowledgement.AcknowledgedTic &&
		decodedReplayAcknowledgement.TransferId == replayAcknowledgement.TransferId &&
		decodedReplayAcknowledgement.Digest == replayAcknowledgement.Digest,
		"snapshot replay acknowledgement did not round-trip");
	std::vector<uint8_t> mutatedReplayWire = replayWire;
	mutatedReplayWire.back() ^= 0xff;
	Require(OnlineDecodeSnapshotReplayTic(mutatedReplayWire.data(), mutatedReplayWire.size(), decodedReplayFrame, error),
		"replay payload mutation should remain structurally valid until simulation-level validation");
	mutatedReplayWire.resize(mutatedReplayWire.size() - 1);
	Require(!OnlineDecodeSnapshotReplayTic(mutatedReplayWire.data(), mutatedReplayWire.size(), decodedReplayFrame, error),
		"snapshot replay decoder accepted a truncated frame");
	FOnlineSnapshotReplayBuffer replayBuffer(16);
	RequireSuccess(replayBuffer.Start(snapshotMetadata, error), error);
	FOnlineSnapshotReplayFrame laterReplay = replayFrame;
	laterReplay.Tic += 1;
	laterReplay.Bytes = { 25, 36, 49 };
	RequireSuccess(replayBuffer.Accept(laterReplay, error), error);
	std::vector<FOnlineTicRecord> bufferedReplay;
	Require(!replayBuffer.CopyThrough(laterReplay.Tic, bufferedReplay, error),
		"snapshot replay buffer accepted a gap before the live boundary");
	RequireSuccess(replayBuffer.Accept(replayFrame, error), error);
	RequireSuccess(replayBuffer.Accept(replayFrame, error), error);
	uint64_t highestReplayTic = 0;
	RequireSuccess(replayBuffer.GetHighestContiguousTic(highestReplayTic), error);
	Require(highestReplayTic == laterReplay.Tic,
		"snapshot replay buffer did not report its contiguous acknowledgement boundary");
	RequireSuccess(replayBuffer.CopyThrough(laterReplay.Tic, bufferedReplay, error), error);
	Require(bufferedReplay.size() == 2 && bufferedReplay[0].Tic == replayFrame.Tic &&
		bufferedReplay[1].Tic == laterReplay.Tic, "snapshot replay buffer did not sort a reordered stream");
	FOnlineSnapshotReplayFrame conflictingReplay = replayFrame;
	conflictingReplay.Bytes[0] ^= 0xff;
	Require(!replayBuffer.Accept(conflictingReplay, error), "snapshot replay buffer accepted a conflicting duplicate");
	replayBuffer.DiscardThrough(laterReplay.Tic);
	Require(replayBuffer.ByteCount() == 0, "snapshot replay buffer did not discard consumed tics");
	FOnlineSnapshotReplayBuffer smallReplayBuffer(replayFrame.Bytes.size());
	RequireSuccess(smallReplayBuffer.Start(snapshotMetadata, error), error);
	RequireSuccess(smallReplayBuffer.Accept(replayFrame, error), error);
	Require(!smallReplayBuffer.Accept(laterReplay, error), "snapshot replay buffer exceeded its byte cap");
	FOnlineSnapshotChunkFrame firstSnapshotChunk;
	firstSnapshotChunk.Epoch = snapshotMetadata.Epoch;
	firstSnapshotChunk.SnapshotTic = snapshotMetadata.SnapshotTic;
	firstSnapshotChunk.TransferId = snapshotMetadata.TransferId;
	firstSnapshotChunk.Digest = snapshotMetadata.Digest;
	firstSnapshotChunk.ChunkIndex = 0;
	firstSnapshotChunk.Bytes.assign(serializedWorld.begin(), serializedWorld.begin() + OnlineSnapshotChunkBytes);
	RequireSuccess(OnlineEncodeSnapshotChunk(firstSnapshotChunk, snapshotWire, error), error);
	FOnlineSnapshotChunkFrame decodedSnapshotChunk;
	RequireSuccess(OnlineDecodeSnapshotChunk(snapshotWire.data(), snapshotWire.size(), decodedSnapshotChunk, error), error);
	Require(decodedSnapshotChunk.ChunkIndex == 0 && decodedSnapshotChunk.Bytes == firstSnapshotChunk.Bytes &&
		decodedSnapshotChunk.Digest == snapshotMetadata.Digest, "snapshot chunk frame did not round-trip");
	snapshotWire[4] = OnlineSnapshotFrameVersion + 1;
	Require(!OnlineDecodeSnapshotChunk(snapshotWire.data(), snapshotWire.size(), decodedSnapshotChunk, error),
		"snapshot decoder accepted an unsupported frame version");
	FOnlineSnapshotAssembler snapshotAssembler;
	RequireSuccess(snapshotAssembler.Begin(snapshotMetadata, 3, error), error);
	RequireSuccess(snapshotAssembler.AcceptChunk(2, serializedWorld.data() + OnlineSnapshotChunkBytes * 2,
		serializedWorld.size() - OnlineSnapshotChunkBytes * 2, error), error);
	RequireSuccess(snapshotAssembler.AcceptChunk(0, serializedWorld.data(), OnlineSnapshotChunkBytes, error), error);
	RequireSuccess(snapshotAssembler.Begin(snapshotMetadata, 3, error), error);
	RequireSuccess(snapshotAssembler.AcceptChunk(1, serializedWorld.data() + OnlineSnapshotChunkBytes, OnlineSnapshotChunkBytes, error), error);
	Require(snapshotAssembler.AcceptChunk(1, serializedWorld.data() + OnlineSnapshotChunkBytes, OnlineSnapshotChunkBytes, error),
		"snapshot rejected an identical reliable retry");
	std::vector<uint8_t> restoredWorld;
	RequireSuccess(snapshotAssembler.Finalize(restoredWorld, error), error);
	Require(restoredWorld == serializedWorld, "snapshot chunks did not restore exact bytes");
	Require(!snapshotAssembler.Begin(snapshotMetadata, 2, error), "snapshot accepted a mismatched chunk count");
	snapshotMetadata.Digest.assign(OnlineDigestBytes * 2, '0');
	RequireSuccess(snapshotAssembler.Begin(snapshotMetadata, 3, error), error);
	RequireSuccess(snapshotAssembler.AcceptChunk(0, serializedWorld.data(), OnlineSnapshotChunkBytes, error), error);
	RequireSuccess(snapshotAssembler.AcceptChunk(1, serializedWorld.data() + OnlineSnapshotChunkBytes, OnlineSnapshotChunkBytes, error), error);
	RequireSuccess(snapshotAssembler.AcceptChunk(2, serializedWorld.data() + OnlineSnapshotChunkBytes * 2,
		serializedWorld.size() - OnlineSnapshotChunkBytes * 2, error), error);
	Require(!snapshotAssembler.Finalize(restoredWorld, error), "snapshot accepted a bad digest");

	FOnlineSnapshotSender snapshotSender;
	RequireSuccess(snapshotSender.Start(capturedMetadata, serializedWorld.data(), serializedWorld.size(), error), error);
	beginAcknowledgement = { capturedMetadata.Epoch, capturedMetadata.SnapshotTic, capturedMetadata.TransferId,
		capturedMetadata.Digest, OnlineSnapshotBeginAcknowledgement };
	Require(!snapshotSender.Acknowledge(beginAcknowledgement, error),
		"snapshot sender accepted a Begin acknowledgement before emitting Begin");
	RequireSuccess(snapshotSender.NextFrame(snapshotWire, error), error);
	RequireSuccess(OnlineDecodeSnapshotBegin(snapshotWire.data(), snapshotWire.size(), decodedSnapshotMetadata, decodedChunkCount, error), error);
	beginAcknowledgement = { capturedMetadata.Epoch, capturedMetadata.SnapshotTic, capturedMetadata.TransferId,
		capturedMetadata.Digest, OnlineSnapshotBeginAcknowledgement };
	FOnlineSnapshotAckFrame prematureChunkAcknowledgement = beginAcknowledgement;
	prematureChunkAcknowledgement.ChunkIndex = 0;
	Require(!snapshotSender.Acknowledge(prematureChunkAcknowledgement, error),
		"snapshot sender accepted a chunk acknowledgement before Begin");
	RequireSuccess(snapshotSender.Acknowledge(beginAcknowledgement, error), error);
	Require(!snapshotSender.Acknowledge(prematureChunkAcknowledgement, error),
		"snapshot sender accepted a chunk acknowledgement before emitting that chunk");
	RequireSuccess(snapshotSender.NextFrame(snapshotWire, error), error);
	RequireSuccess(OnlineDecodeSnapshotChunk(snapshotWire.data(), snapshotWire.size(), decodedSnapshotChunk, error), error);
	const uint32_t firstSentChunk = static_cast<uint32_t>(decodedSnapshotChunk.ChunkIndex);
	// Drop the first chunk once. The sender rotates through the unacknowledged
	// window and must retry it after the other chunks are acknowledged.
	RequireSuccess(snapshotSender.NextFrame(snapshotWire, error), error);
	RequireSuccess(OnlineDecodeSnapshotChunk(snapshotWire.data(), snapshotWire.size(), decodedSnapshotChunk, error), error);
	FOnlineSnapshotAckFrame chunkAcknowledgement = { capturedMetadata.Epoch, capturedMetadata.SnapshotTic,
		capturedMetadata.TransferId, capturedMetadata.Digest, static_cast<uint32_t>(decodedSnapshotChunk.ChunkIndex) };
	RequireSuccess(snapshotSender.Acknowledge(chunkAcknowledgement, error), error);
	RequireSuccess(snapshotSender.NextFrame(snapshotWire, error), error);
	RequireSuccess(OnlineDecodeSnapshotChunk(snapshotWire.data(), snapshotWire.size(), decodedSnapshotChunk, error), error);
	chunkAcknowledgement.ChunkIndex = static_cast<uint32_t>(decodedSnapshotChunk.ChunkIndex);
	RequireSuccess(snapshotSender.Acknowledge(chunkAcknowledgement, error), error);
	RequireSuccess(snapshotSender.NextFrame(snapshotWire, error), error);
	RequireSuccess(OnlineDecodeSnapshotChunk(snapshotWire.data(), snapshotWire.size(), decodedSnapshotChunk, error), error);
	Require(decodedSnapshotChunk.ChunkIndex == firstSentChunk, "snapshot sender did not retry the dropped chunk");
	chunkAcknowledgement.ChunkIndex = firstSentChunk;
	RequireSuccess(snapshotSender.Acknowledge(chunkAcknowledgement, error), error);
	FOnlineSnapshotAckFrame prematureComplete = { capturedMetadata.Epoch, capturedMetadata.SnapshotTic,
		capturedMetadata.TransferId, capturedMetadata.Digest, OnlineSnapshotCompleteAcknowledgement };
	RequireSuccess(snapshotSender.Acknowledge(prematureComplete, error), error);
	Require(snapshotSender.IsComplete(), "snapshot sender did not require and accept a final acknowledgement");
	FOnlineSnapshotAckFrame unrelatedAcknowledgement = prematureComplete;
	unrelatedAcknowledgement.TransferId ^= 1;
	Require(!snapshotSender.Acknowledge(unrelatedAcknowledgement, error),
		"snapshot sender accepted an acknowledgement from an earlier handoff");
	unrelatedAcknowledgement = prematureComplete;
	unrelatedAcknowledgement.Digest.assign(OnlineDigestBytes * 2, '1');
	Require(!snapshotSender.Acknowledge(unrelatedAcknowledgement, error), "snapshot sender accepted an acknowledgement for another archive");

	// Exercise the complete unreliable sender/receiver exchange: lose the
	// first Begin acknowledgement and one chunk, then keep polling the bounded
	// acknowledgement schedule until both peers agree the archive is complete.
	FOnlineSnapshotSender reliableSender;
	FOnlineSnapshotReceiver reliableReceiver;
	RequireSuccess(reliableSender.Start(capturedMetadata, serializedWorld.data(), serializedWorld.size(), error), error);
	bool droppedBeginAcknowledgement = false;
	bool droppedChunk = false;
	for (size_t step = 0; step < 64 && !reliableSender.IsComplete(); ++step)
	{
		if (reliableSender.NextFrame(snapshotWire, error))
		{
			Require(OnlineIsSnapshotFrame(snapshotWire.data(), snapshotWire.size()), "sender produced a non-snapshot frame");
			if (snapshotWire[5] == static_cast<uint8_t>(EOnlineSnapshotFrameType::Begin))
			{
				RequireSuccess(OnlineDecodeSnapshotBegin(snapshotWire.data(), snapshotWire.size(), decodedSnapshotMetadata, decodedChunkCount, error), error);
				RequireSuccess(reliableReceiver.ReceiveBegin(decodedSnapshotMetadata, decodedChunkCount, error), error);
			}
			else
			{
				RequireSuccess(OnlineDecodeSnapshotChunk(snapshotWire.data(), snapshotWire.size(), decodedSnapshotChunk, error), error);
				if (!droppedChunk && decodedSnapshotChunk.ChunkIndex == 1)
				{
					droppedChunk = true;
				}
				else
				{
					RequireSuccess(reliableReceiver.ReceiveChunk(decodedSnapshotChunk, error), error);
				}
			}
		}
		else
		{
			Require(error == "online snapshot sender is awaiting final receiver acknowledgement",
				"reliable sender stopped before waiting for completion");
		}

		FOnlineSnapshotAckFrame acknowledgement;
		if (reliableReceiver.NextAcknowledgement(acknowledgement, error))
		{
			if (!droppedBeginAcknowledgement && acknowledgement.ChunkIndex == OnlineSnapshotBeginAcknowledgement)
			{
				droppedBeginAcknowledgement = true;
			}
			else
			{
				RequireSuccess(reliableSender.Acknowledge(acknowledgement, error), error);
			}
		}
	}
	Require(droppedBeginAcknowledgement && droppedChunk, "reliable snapshot fixture did not exercise loss");
	Require(reliableSender.IsComplete() && reliableReceiver.IsComplete(), "reliable snapshot exchange did not converge");
	FOnlineSnapshotMetadata receivedMetadata;
	std::vector<uint8_t> receivedSnapshot;
	RequireSuccess(reliableReceiver.CopyCompletedArchive(receivedMetadata, receivedSnapshot, error), error);
	Require(receivedMetadata.Digest == capturedMetadata.Digest && receivedSnapshot == serializedWorld,
		"snapshot receiver completed a different archive");
	FileSys::FCompressedBuffer receivedArchive = { 0, 0, 0, 0, nullptr, nullptr };
	RequireSuccess(OnlineRestoreSnapshotArchive(receivedMetadata, receivedSnapshot.data(), receivedSnapshot.size(), receivedArchive, error), error);
	receivedArchive.Clean();
	FOnlineSnapshotChunkFrame conflictingRetry;
	conflictingRetry.Epoch = receivedMetadata.Epoch;
	conflictingRetry.SnapshotTic = receivedMetadata.SnapshotTic;
	conflictingRetry.TransferId = receivedMetadata.TransferId;
	conflictingRetry.Digest = receivedMetadata.Digest;
	conflictingRetry.Bytes.assign(receivedSnapshot.begin(), receivedSnapshot.begin() + OnlineSnapshotChunkBytes);
	conflictingRetry.Bytes[0] ^= 0xff;
	Require(!reliableReceiver.ReceiveChunk(conflictingRetry, error),
		"snapshot receiver accepted a conflicting retry after completion");

	FOnlineTicHistory ticHistory(8);
	const std::array<uint8_t, 4> ticOne = { 1, 2, 3, 4 };
	const std::array<uint8_t, 4> ticTwo = { 5, 6, 7, 8 };
	const std::array<uint8_t, 4> ticThree = { 9, 10, 11, 12 };
	RequireSuccess(ticHistory.Append(100, ticOne.data(), ticOne.size(), error), error);
	RequireSuccess(ticHistory.Append(101, ticTwo.data(), ticTwo.size(), error), error);
	RequireSuccess(ticHistory.Append(102, ticThree.data(), ticThree.size(), error), error);
	const auto replay = ticHistory.RecordsAfter(100);
	Require(replay.size() == 2 && replay[0].Tic == 101 && replay[1].Tic == 102, "tic history did not retain its bounded replay tail");
	Require(!ticHistory.Append(102, ticThree.data(), ticThree.size(), error), "tic history accepted an ambiguous duplicate tic");
	Require(!ticHistory.Append(99, ticThree.data(), ticThree.size(), error), "tic history accepted an older tic after trimming");

	FOnlineRelayTransport hostRelay;
	FOnlineRelayTransport guestRelay;
	const std::string sessionKey(64, 'a');
	const std::string relayToken(32, 'b');
	const std::string hostPeer(32, 'c');
	const std::string guestPeer(32, 'd');
	const std::string hostRelayAuth(32, 'e');
	const std::string guestRelayAuth(32, 'f');
	const std::string observerPeer(32, '1');
	const std::string observerRelayAuth(32, '2');
	RequireSuccess(hostRelay.Configure(sessionKey, relayToken, hostPeer, hostRelayAuth, 3, error), error);
	RequireSuccess(guestRelay.Configure(sessionKey, relayToken, guestPeer, guestRelayAuth, 3, error), error);
	FOnlineRelayTransport observerRelay;
	RequireSuccess(observerRelay.Configure(sessionKey, relayToken, observerPeer, observerRelayAuth, 3, error), error);
	FOnlineRelayTransport::FPeerId guestPeerBytes = {};
	Require(FOnlineRelayTransport::ParsePeerId(guestPeer, guestPeerBytes), "guest peer id did not parse");
	const std::array<uint8_t, 3> relayPayload = { 4, 5, 6 };
	std::vector<uint8_t> wire;
	RequireSuccess(hostRelay.Encapsulate(guestPeerBytes, relayPayload.data(), relayPayload.size(), wire, error), error);
	FOnlineRelayTransport::FPeerId relaySender = {};
	RequireSuccess(guestRelay.Decapsulate(wire.data(), wire.size(), relaySender, opened, error), error);
	Require(opened == std::vector<uint8_t>(relayPayload.begin(), relayPayload.end()), "relay payload did not round-trip");
	Require(!observerRelay.Decapsulate(wire.data(), wire.size(), relaySender, opened, error),
		"relay accepted a packet addressed to a different peer");
	Require(!guestRelay.Decapsulate(wire.data(), wire.size(), relaySender, opened, error), "relay replay was accepted");
	RequireSuccess(hostRelay.EncapsulateDirect(guestPeerBytes, relayPayload.data(), relayPayload.size(), wire, error), error);
	RequireSuccess(guestRelay.DecapsulateDirect(wire.data(), wire.size(), relaySender, opened, error), error);
	Require(opened == std::vector<uint8_t>(relayPayload.begin(), relayPayload.end()), "direct payload did not round-trip");
	Require(!observerRelay.DecapsulateDirect(wire.data(), wire.size(), relaySender, opened, error),
		"direct route accepted a packet addressed to a different peer");
	Require(!guestRelay.DecapsulateDirect(wire.data(), wire.size(), relaySender, opened, error),
		"direct route accepted a replayed packet");
	std::vector<uint8_t> oldEpochWire;
	RequireSuccess(hostRelay.EncapsulateDirect(guestPeerBytes, relayPayload.data(), relayPayload.size(), oldEpochWire, error), error);
	RequireSuccess(hostRelay.Configure(sessionKey, relayToken, hostPeer, hostRelayAuth, 4, error), error);
	RequireSuccess(guestRelay.Configure(sessionKey, relayToken, guestPeer, guestRelayAuth, 4, error), error);
	Require(!guestRelay.DecapsulateDirect(oldEpochWire.data(), oldEpochWire.size(), relaySender, opened, error),
		"direct route accepted an outgoing packet from a former migration epoch");
	RequireSuccess(hostRelay.Encapsulate(guestPeerBytes, relayPayload.data(), relayPayload.size(), wire, error), error);
	RequireSuccess(guestRelay.Decapsulate(wire.data(), wire.size(), relaySender, opened, error), error);

	std::filesystem::remove_all(root, code);
	std::cout << "PASS: online content cache, bundles, encrypted datagrams, and snapshot transfer\n";
	return 0;
}
