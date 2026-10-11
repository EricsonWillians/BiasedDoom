#!/usr/bin/env python3
"""Fast source-contract regression checks for the online foundation.

The integration suite runs against the containerized service. This lightweight
check keeps security-critical declarations wired into normal CI without
requiring Docker or an external IWAD.
"""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def require(path: str, fragment: str) -> None:
    text = (ROOT / path).read_text(encoding="utf-8")
    if fragment not in text:
        raise AssertionError(f"{path} is missing required online contract: {fragment!r}")


def main() -> None:
    require("CMakeLists.txt", "BIASEDDOOM_ENABLE_ONLINE")
    require("vcpkg.json", '"libsodium"')
    require("vcpkg.json", '"curl"')
    # Release packages build the same client, so they need the same resilient
    # bootstrap path and Autotools prerequisites as pull-request CI.
    require(".github/workflows/release.yml", "tools/bootstrap-vcpkg.sh")
    require(".github/workflows/release.yml", "autoconf-archive")
    require("README.md", "autoconf-archive")
    require("src/common/online/online_session.h", "FOnlineSession")
    require("src/common/online/online_session.h", "FContentManifest")
    require("src/common/online/online_session.h", "FJoinTicket")
    require("src/common/online/online_session.cpp", "crypto_aead_chacha20poly1305_ietf_encrypt")
    require("src/common/online/online_session.cpp", "OnlineWriteDirectoryBundle")
    require("src/common/online/online_session.cpp", "EnsureSpace")
    require("src/common/online/online_session.cpp", "PartialSize")
    require("src/common/online/online_session.cpp", "discard the corrupt download partial")
    require("src/common/online/online_session.cpp", "discard the corrupt content cache entry")
    require("src/common/online/online_session.cpp", "online content cache root is not a real directory")
    require("src/common/online/online_session.cpp", "online content cache bucket is not a real directory")
    require("tools/online_core_test.cpp", "content cache accepted a symlink root")
    require("tools/online_core_test.cpp", "content cache accepted a symlink bucket")
    require("src/common/online/online_session.cpp", "OnlineResolveCachedContent")
    require("src/common/online/online_session.cpp", "content manifest root digest does not match its packages")
    require("src/common/online/online_session.cpp", "directory-backed content cannot contain symlinks")
    require("src/common/online/online_snapshot.h", "FOnlineSnapshotAssembler")
    require("src/common/online/online_join.h", "FOnlineJoinRequestFrame")
    require("src/common/online/online_join.h", "FOnlineJoinReadyFrame")
    require("src/common/online/online_join.h", "FOnlineJoinActivatedFrame")
    require("src/common/online/online_join.h", "OnlineGenerateJoinRequestId")
    require("src/common/online/online_join.cpp", "OnlineEncodeJoinRequest")
    require("src/common/online/online_join.cpp", "online join request frame has an invalid userinfo payload")
    require("src/common/online/online_join.cpp", "would leave data in its stream")
    require("src/common/online/online_snapshot.h", "uint64_t TransferId")
    require("src/common/online/online_snapshot.h", "FOnlineTicHistory")
    require("src/common/online/online_snapshot.h", "OnlineEncodeSnapshotBegin")
    require("src/common/online/online_snapshot.h", "OnlineEncodeSnapshotChunk")
    require("src/common/online/online_snapshot.h", "FOnlineSnapshotSender")
    require("src/common/online/online_snapshot.h", "FOnlineSnapshotReceiver")
    require("src/common/online/online_snapshot.h", "OnlineSnapshotCompleteAcknowledgement")
    require("src/common/online/online_snapshot.h", "FOnlineSnapshotReplayFrame")
    require("src/common/online/online_snapshot.h", "FOnlineSnapshotCompleteFrame")
    # A receiver must confirm the actual completion fence, not merely the archive.
    require("src/common/online/online_snapshot.h", "FOnlineSnapshotCompleteAckFrame")
    require("src/common/online/online_snapshot.h", "FOnlineSnapshotCancelFrame")
    require("src/common/online/online_snapshot.h", "OnlineMaxTicRecordBytes = 12 * 1024")
    require("src/common/online/online_snapshot.h", "FOnlineSnapshotReplayAckFrame")
    require("src/common/online/online_snapshot.h", "FOnlineSnapshotReplayBuffer")
    require("src/common/online/online_snapshot.h", "FOnlineSnapshotBundle")
    require("src/common/online/online_snapshot.h", "OnlineMakeStoredSnapshotMetadata")
    require("src/common/online/online_snapshot.h", "OnlineValidateSnapshotBundleArchives")
    require("src/common/online/online_snapshot.cpp", "online snapshot chunk was acknowledged before its begin frame")
    require("src/common/online/online_snapshot.cpp", "GenerateTransferId")
    # A receiver cannot acknowledge an archive announcement it has never been sent.
    require("src/common/online/online_snapshot.cpp", "online snapshot Begin was acknowledged before the host sent it")
    require("src/common/online/online_snapshot.cpp", "online snapshot chunk was acknowledged before the host sent it")
    require("src/common/online/online_snapshot.cpp", "online snapshot retry chunk conflicts with the completed archive")
    require("src/common/online/online_snapshot.cpp", "online snapshot replay frame has an invalid length")
    require("src/common/online/online_snapshot.cpp", "online snapshot replay has a tic gap")
    require("src/common/online/online_snapshot.cpp", "online snapshot bundle is missing world or global state")
    require("tools/online_core_test.cpp", "snapshot sender did not retry the dropped chunk")
    require("tools/online_core_test.cpp", "reliable snapshot exchange did not converge")
    require("tools/online_core_test.cpp", "online join acceptance did not round-trip")
    require("tools/online_core_test.cpp", "snapshot sender accepted an acknowledgement from an earlier handoff")
    require("tools/online_core_test.cpp", "snapshot replay buffer did not sort a reordered stream")
    require("tools/online_core_test.cpp", "snapshot bundle did not preserve complete restore components")
    require("tools/online_core_test.cpp", "stored snapshot metadata did not preserve the encoded bundle")
    require("tools/online_core_test.cpp", "snapshot bundle accepted a component with a bad CRC")
    require("tools/online_snapshot_fuzz.cpp", "online snapshot malformed-frame fuzz regression")
    require("src/CMakeLists.txt", "biaseddoom-online-snapshot-fuzz")
    require("src/common/online/online_snapshot.cpp", "online snapshot content digest does not match")
    require("src/common/online/online_snapshot.cpp", "online snapshot chunk count does not match")
    require("src/common/online/online_snapshot.cpp", "online snapshot chunk frame has an invalid length")
    require("src/common/online/online_service.cpp", "CURLOPT_SSL_VERIFYPEER")
    require("src/common/online/online_service.cpp", "crypto_sign_detached")
    # RapidJSON's default 48-bit pointer compression is incompatible with the
    # engine allocator on wide-address hosts. The service parser must retain
    # full pointers so a real control-plane response cannot abort the client.
    require("src/common/online/online_service.cpp", "RAPIDJSON_48BITPOINTER_OPTIMIZATION 0")
    require("src/common/online/online_service.h", "SignDetached")
    require("src/d_main.cpp", "!batchrun && !I_IsHeadless()")
    # Declared IWAD companion archives are verified locally as part of the
    # base-game fingerprint; they must not appear in a host mod download.
    require("src/d_iwad.cpp", "online multiplayer verifies it locally instead of offering it for transfer")
    require("src/common/engine/i_net.cpp", "BuildOnlineBaseFingerprint")
    require("tools/test-online-e2e.sh", "incorrectly included a base-game or engine resource")
    require("src/common/engine/i_net.cpp", "host's just-bound UDP endpoint immediately")
    # A passworded public listing must not let a guest inspect mod metadata or
    # consume a lobby slot before proving the session password.
    require("src/common/online/online_service.cpp", "BuildPasswordProof")
    require("src/common/online/online_service.h", "FOnlineDeviceIdentity")
    require("src/common/online/online_service.h", "CloseSession")
    # A started game closes the legacy lobby, but a capability-advertising host
    # must route a new guest through the snapshot startup flow instead.
    require("src/common/online/online_service.h", "CloseLobby")
    require("src/common/engine/i_net.cpp", "CloseLobby(OnlineSession")
    require("src/launcher/networkpage.cpp", "In progress")
    require("src/common/online/online_service.h", "InspectJoinCode")
    require("src/common/online/online_service.h", "IsPeerAdmitted")
    require("src/common/online/online_service.cpp", "online peer admission query is invalid")
    require("services/online/cmd/biaseddoom-online/main.go", 'case "admission"')
    # Public control-plane deployments can serve TLS directly; incomplete
    # mounted credential configuration must fail before the service binds an
    # accidental plaintext endpoint.
    require("services/online/cmd/biaseddoom-online/main.go", "func tlsFiles()")
    require("services/online/cmd/biaseddoom-online/main.go", "ListenAndServeTLS")
    require("services/online/cmd/biaseddoom-online/main.go", "must be set together")
    require("services/online/cmd/biaseddoom-online/main.go", "ReadTimeout:       15 * time.Second")
    require("services/online/cmd/biaseddoom-online/main.go", "WriteTimeout:      15 * time.Second")
    require("services/online/cmd/biaseddoom-online/main_test.go", "TestTLSConfigurationRequiresCertificateAndKeyTogether")
    require("services/online/cmd/biaseddoom-online/main_test.go", "TestHTTPServerBoundsConnectionLifetimes")
    require("services/online/README.md", "BIASEDDOOM_ONLINE_TLS_CERT_FILE")
    require("services/online/README.md", "complete reads/writes")
    require("tools/test-online-service-container.sh", "https://online-tls:8443/healthz")
    require("tools/test-online-e2e.sh", "Approval token")
    require("tools/test-online-e2e.sh", "invalid session password")
    require("tools/test-online-e2e.sh", "online engine E2E")
    require(".github/workflows/continuous_integration.yml", "test-online-e2e.sh")
    # The online route must be part of the real startup path, not merely a
    # standalone library that can drift away from the engine.
    require("src/common/engine/i_net.cpp", "PrepareOnlineHost")
    require("src/common/engine/i_net.cpp", "PrepareOnlineJoin")
    require("src/common/engine/i_net.cpp", "OnlineNextLeaseRenewal")
    require("src/common/engine/i_net.cpp", "I_OnlinePreflightContent")
    # Launcher approval is intentionally gated behind a fresh authenticated
    # inspection. The displayed plan includes host identity plus each package's
    # name, digest, and size; stale edits disable approval again. The root is
    # carried into launch, so a replacement manifest cannot inherit approval.
    require("src/common/engine/i_net.h", "FOnlineJoinReview")
    require("src/common/engine/i_net.cpp", "I_OnlineInspectJoin")
    require("src/launcher/networkpage.cpp", "Review host mods")
    require("src/launcher/networkpage.cpp", "Host ID ")
    require("src/launcher/networkpage.cpp", "HasCurrentOnlineReview")
    require("src/launcher/networkpage.cpp", "OnlineReviewComplete")
    require("src/common/engine/i_net.h", "ContentRootDigest")
    require("src/launcher/networkpage.cpp", "ReviewedContentRootDigest")
    require("src/launcher/networkpage.cpp", "ReviewedHostPeerId")
    require("src/common/engine/i_net.cpp", "expectedApproval")
    require("src/common/engine/i_net.cpp", "host or mod plan was not approved")
    require("src/common/online/online_session.h", "OnlineBuildContentApprovalToken")
    require("tools/online_core_test.cpp", "content approval token did not bind the reviewed root and host")
    # The browser's report action uses the existing persisted anonymous device
    # identity rather than inventing a throwaway identity for every click.
    require("src/common/engine/i_net.cpp", "I_OnlineReportSession")
    require("src/common/engine/i_net.cpp", "I_OnlinePrepareHostContent")
    require("src/launcher/networkpage.cpp", "Online Relay is unavailable in this build")
    require("src/launcher/networkpage.cpp", "supports joining games in progress")
    require("src/launcher/networkpage.cpp", "Join Online Relay by code")
    # The host wizard must state the safety contract for the mod set it will
    # advertise, rather than leaving that decision implicit until a guest joins.
    require("src/launcher/networkpage.cpp", "Mod sharing: guests review host mods")
    require("src/launcher/networkpage.cpp", "static_cast<const char*>(defaultonlinerecents)")
    require("src/launcher/networkpage.cpp", "static_cast<const char*>(defaultonlineblocked)")
    require("src/common/engine/i_net.cpp", "This session accepts players only while its lobby is open.")
    require("src/g_levellocals.h", "bool BuildSnapshot(FCompressedBuffer &snapshot)")
    require("src/p_saveg.cpp", "bool FLevelLocals::BuildSnapshot(FCompressedBuffer &snapshot)")
    require("src/p_saveg.cpp", "BuildSnapshot(info->Snapshot);")
    require("src/g_game.h", "G_BuildOnlineSnapshotBundle")
    require("src/g_game.cpp", "G_BuildOnlineSnapshotBundle")
    # Snapshot restore now carries the packet-server baseline and a bounded
    # authoritative input history, but public JIP admission remains gated
    # until the dynamic roster transition is complete.
    require("src/d_net.h", "FOnlineNetSnapshotState")
    require("src/d_net.cpp", "Net_BuildOnlineReplayRecord")
    require("src/d_net.cpp", "Net_ApplyOnlineReplayRecord")
    require("src/d_net.cpp", "Net_RunOnlineReplayTic")
    require("src/d_net.cpp", "online replay crossed a network command reset")
    require("src/d_net.cpp", "Net_QueueOnlinePlayerJoin")
    require("src/d_protocol.h", "DEM_ONLINEPLAYERJOIN")
    require("src/common/engine/i_net.cpp", "I_OnlineRecordSimulationTic")
    require("src/common/engine/i_net.cpp", "OnlineHandleSnapshotPacket")
    require("src/common/engine/i_net.cpp", "JoinOnlineGameInProgress")
    require("src/common/engine/i_net.cpp", "Guest_ReceiveOnlineSnapshot")
    require("src/common/engine/i_net.cpp", "OnlinePumpJoinAdmissions")
    require("src/common/engine/i_net.cpp", "I_OnlineRestorePendingJoin")
    require("src/common/engine/i_net.cpp", "I_OnlineActivateJoiningPlayer")
    require("src/d_main.cpp", "restoredOnlineJoin")
    require("src/d_net.cpp", "I_OnlineAdvancePendingJoinReplay")
    require("src/common/engine/i_net.cpp", "OnlineFlushSnapshotAcknowledgement")
    require("src/common/engine/i_net.cpp", "OnlinePumpSnapshotAcknowledgements")
    require("src/common/engine/i_net.cpp", "OnlineFlushSnapshotReplayAcknowledgement")
    # The host must retain the handoff until it receives an acknowledgement for
    # the exact replay completion fence it emitted.
    require("src/common/engine/i_net.cpp", "I_OnlineStartSnapshotHandoff")
    require("src/common/engine/i_net.cpp", "OnlinePumpSnapshotHandoff")
    require("src/common/engine/i_net.cpp", "OnlineFlushSnapshotCompletionAcknowledgement")
    require("src/common/engine/i_net.cpp", "CompleteAcknowledge")
    require("src/g_game.cpp", "online_net_tic")
    # The restore bridge stays disconnected from public JIP admission until its
    # replay path is wired, but it must still use the normal snapshot ownership
    # and validation boundaries rather than a parallel deserializer.
    require("src/g_game.h", "G_RestoreOnlineSnapshotBundle")
    require("src/g_game.cpp", "G_RestoreOnlineSnapshotBundle")
    require("src/g_game.cpp", "OnlineValidateSnapshotBundleArchives")
    require("src/g_level.h", "G_InstallSerializedSnapshots")
    require("src/g_level.cpp", "G_InstallSerializedSnapshots")
    # Existing save/load call sites remain strict. Only the network bridge opts
    # into suppressed reporting after it has inspected a malformed archive.
    require("src/common/engine/serializer.h", "void Close(bool reportErrors = true)")
    require("src/common/engine/serializer.cpp", "if (reportErrors && mErrors > 0)")
    require("src/g_game.cpp", "StaticWriteRNGState")
    require("src/g_game.cpp", "online snapshot has too many hub components")
    require("src/common/engine/i_net.cpp", "CanonicalizeOnlineDirectoryContent")
    require("src/common/engine/i_net.cpp", "PrintOnlineContentReview")
    require("src/common/engine/i_net.cpp", "-online-accept-mods")
    require("src/common/engine/i_net.cpp", "OnlineHandleContentPacket")
    require("src/common/engine/i_net.cpp", "Using %u verified cached online content package")
    # Local automated/manual test players need separate anonymous identities,
    # not a shared default application profile.
    require("src/common/engine/i_net.cpp", "-online-data-dir must be an absolute directory path")
    require("src/common/engine/i_net.cpp", "-online-data-dir must be a real directory, not a symlink")
    require("docs/engine/online-multiplayer.md", "-online-data-dir")
    # Graceful host departures must use the service-selected successor and an
    # authenticated relay-epoch handoff, rather than closing the session.
    require("src/common/engine/i_net.cpp", "I_OnlinePrepareHostMigration")
    require("src/common/engine/i_net.cpp", "OnlineMigrationMagic")
    require("src/common/engine/i_net.cpp", "elected host no longer has the session's verified content")
    require("src/common/engine/i_net.cpp", "SendOnlineHostLivenessHeartbeat")
    require("src/common/engine/i_net.cpp", "SendOnlineGuestLivenessHeartbeat")
    require("src/common/engine/i_net.cpp", "OnlineGuestLivenessMagic")
    require("src/common/engine/i_net.cpp", "MaybeRecoverOnlineHost")
    require("src/common/engine/i_net.cpp", "OnlineLivenessMagic")
    require("src/common/online/online_service.h", "MigrateSession")
    require("src/common/online/online_service.h", "RecoverSession")
    require("src/common/online/online_transport.h", "FOnlineRelayTransport")
    require("src/common/online/online_transport.cpp", "RelayAuth")
    # Target labels are public only to the relay and are checked before it
    # forwards opaque traffic, preventing a valid packet from being broadcast
    # to every admitted member.
    require("src/common/online/online_transport.cpp", "recipient label")
    # Direct NAT acceleration is optional, but it must remain identity-bound and
    # preserve the encrypted relay as a safe fallback.
    require("src/common/online/online_transport.h", "EncapsulateDirect")
    require("src/common/online/online_transport.cpp", "DirectPrefix")
    require("tools/online_core_test.cpp", "direct payload did not round-trip")
    require("tools/online_core_test.cpp", "direct route identity proof did not verify")
    require("src/common/online/online_service.h", "VerifyDetached")
    require("src/common/engine/i_net.cpp", "OnlineDirectProbeMagic")
    require("src/common/engine/i_net.cpp", "OnlinePumpDirectProbes")
    require("src/common/engine/i_net.cpp", "ParseOnlineDirectCandidate")
    require("src/common/engine/i_net.cpp", "SendOnlineRelayPacket(host, OnlineGuestLivenessMagic")
    require("src/common/engine/i_net.cpp", "OnlineDirectPeerLastReceived")
    require("src/common/engine/i_net.cpp", "identity-bound route was established")
    require("services/online/cmd/biaseddoom-online/main.go", "directEndpoint")
    require("services/online/cmd/biaseddoom-online/main.go", "directCandidates")
    require("services/online/cmd/biaseddoom-online/main.go", '"host_direct_endpoint"')
    require("services/online/cmd/biaseddoom-online/main_test.go", "TestDirectCandidatesRemainPrivateAndIdentityBound")
    require("services/online/cmd/biaseddoom-online/main_test.go", "review-only preflight leaked the host direct candidate")
    require("src/d_main.cpp", "I_OnlinePreflightContent(allwads")
    require("src/launcher/networkpage.cpp", "Use Online Relay")
    require("src/launcher/networkpage.cpp", "Session password (not saved)")
    require("src/launcher/networkpage.cpp", "Block selected session")
    require("src/launcher/networkpage.cpp", "Report selected session")
    require("src/launcher/networkpage.cpp", "Recent sessions...")
    require("src/common/engine/i_interface.cpp", "defaultonlineblocked")
    require("src/common/engine/i_interface.cpp", "defaultonlinerecents")
    # The MinGW packager builds libsodium for the online client. Keep its
    # prerequisite check explicit so local packaging fails before vcpkg emits
    # an opaque configure error.
    require("tools/build-windows-mingw.sh", "require_autoconf_archive")
    require("tools/build-windows-mingw.sh", "autoconf-archive is required to build the online client")
    # The current Actions failure was an upstream vcpkg release-object 500.
    # Both regular and MinGW builds use the bounded retry plus pinned-source
    # fallback, rather than relying on a flaky bootstrap download.
    require("tools/bootstrap-vcpkg.sh", "building vcpkg-tool")
    require("tools/bootstrap-vcpkg.sh", "vcpkg-tool.git")
    require("tools/build-windows-mingw.sh", "bootstrap-vcpkg.sh")
    # The linked MinGW build failed before its own sources because the public
    # GL header depended on a loader typedef that is not consistently visible
    # across Windows include orders. Keep the opaque storage spelling stable.
    require("src/common/rendering/gl/gl_framebuffer.h", "::__GLsync *Fence")
    require(".github/workflows/continuous_integration.yml", "autoconf-archive")
    require(".github/workflows/continuous_integration.yml", "tools/bootstrap-vcpkg.sh")
    require(".github/workflows/continuous_integration.yml", "./build/biaseddoom-online-snapshot-fuzz")
    require("src/launcher/networkpage.cpp", "I approve downloading and mounting")
    require("src/launcher/networkpage.cpp", "Refresh public sessions")
    require("services/online/cmd/biaseddoom-online/main.go", '"/v1/join/"')
    # Operations endpoints are aggregate-only and must remain part of the
    # first-party service contract alongside the privacy-sensitive join path.
    require("services/online/cmd/biaseddoom-online/main.go", '"/healthz"')
    require("services/online/cmd/biaseddoom-online/main.go", '"/metrics"')
    # The directory remains the deterministic authority for a graceful election.
    require("services/online/cmd/biaseddoom-online/main.go", 'case "migrate"')
    require("services/online/cmd/biaseddoom-online/main.go", 'case "recover"')
    # Public browsing must accurately advertise the lobby boundary, and a
    # closed session must be accepted only when the host declared snapshot JIP.
    require("services/online/cmd/biaseddoom-online/main.go", 'case "state"')
    require("services/online/cmd/biaseddoom-online/main.go", '"lobby_open"')
    # Review is not admission, but it must still be serialized with closing a
    # lobby so a preflight cannot race across the start boundary.
    require("services/online/cmd/biaseddoom-online/main.go", "Serialize both admission and review")
    require("services/online/cmd/biaseddoom-online/main_test.go", "TestClosedLobbyRejectsPreflightAndAdmission")
    require("services/online/cmd/biaseddoom-online/main_test.go", "TestClosedSnapshotSessionAllowsAuthenticatedAdmission")
    require("services/online/cmd/biaseddoom-online/main_test.go", "TestJoinAlwaysIssuesRelayFallbackWhenLegacyRouteExists")
    require("services/online/cmd/biaseddoom-online/main.go", '"BDLV"')
    require("services/online/cmd/biaseddoom-online/main.go", "reconnectReservationTTL")
    require("services/online/cmd/biaseddoom-online/main.go", "relayPeerSilenceWindow")
    require("services/online/cmd/biaseddoom-online/main.go", "reserveInactiveMembers")
    require("services/online/cmd/biaseddoom-online/main.go", '"reserved_players"')
    # A report can hide a public listing, so repeated reports from the same
    # anonymous device must not multiply its moderation weight.
    require("services/online/cmd/biaseddoom-online/main.go", "Reporters map[string]bool")
    require("services/online/cmd/biaseddoom-online/main_test.go", "reporter deduplication")
    require("services/online/cmd/biaseddoom-online/main.go", "relay code neither decrypts nor persists bytes")
    require("services/online/cmd/biaseddoom-online/main.go", "authorizeSigned")
    # Device-token refresh must remain proof-of-possession based; a public key
    # by itself must never be enough to obtain a bearer token.
    require("services/online/cmd/biaseddoom-online/main.go", "authorizeDeviceRegistration")
    require("services/online/cmd/biaseddoom-online/main_test.go", "TestDeviceRegistrationRequiresProofAndRefreshesSameIdentity")
    require("services/online/cmd/biaseddoom-online/main.go", "replayed device request")
    require("services/online/cmd/biaseddoom-online/main.go", '"session_key": s.SessionKey')
    require("services/online/cmd/biaseddoom-online/main.go", '"relay_auth": relayAuth')
    require("services/online/cmd/biaseddoom-online/main.go", "targetPeerID")
    require("services/online/cmd/biaseddoom-online/main_test.go", "TestSignedSessionLifecycleAndReplayProtection")
    require("services/online/cmd/biaseddoom-online/main_test.go", "reconnect reservation did not protect the guest slot")
    require("services/online/cmd/biaseddoom-online/main_test.go", "relay-silent guest did not become a reconnect reservation")
    require("services/online/cmd/biaseddoom-online/main_test.go", "TestPasswordProtectsManifestBeforePreflightOrAdmission")
    # The successor must be tested against an unsorted eligible roster.
    require("services/online/cmd/biaseddoom-online/main_test.go", "TestServiceCoordinatedHostMigrationSelectsDeterministicEligiblePeer")
    require("services/online/cmd/biaseddoom-online/main_test.go", "TestUnexpectedHostRecoveryRequiresRelaySilenceAndIsIdempotent")
    require("services/online/cmd/biaseddoom-online/main_test.go", "TestRelayAcceptsOnlySignedHostLiveness")
    # A future refactor must keep the privacy-safe metrics surface test-covered.
    require("services/online/cmd/biaseddoom-online/main_test.go", "TestHealthAndMetricsExposeOnlyAggregateState")
    require("services/online/docker-compose.yml", "redis")
    require("tools/test-online-service.sh", "go test -race ./...")
    require("tools/test-online-service.sh", "golang:1.22")
    # The normal handler suite is in-memory; a dedicated container fixture keeps
    # the production Redis RESP store from drifting away from deployment.
    require("tools/test-online-redis-integration.sh", "TestRedisStoreIntegration")
    require("services/online/cmd/biaseddoom-online/main_test.go", "TestRedisStoreIntegration")
    require(".github/workflows/continuous_integration.yml", "Test Redis-backed online service storage")
    # The image runs non-root, so the container smoke test catches deployment
    # drift such as an unreadable mounted signing secret.
    require("tools/test-online-service-container.sh", "distroless non-root user")
    require("tools/test-online-service-container.sh", "aggregate metrics")
    require(".github/workflows/continuous_integration.yml", "Smoke containerized online service deployment")
    print("PASS: online session, encrypted transport, content, and service contracts")


if __name__ == "__main__":
    main()
