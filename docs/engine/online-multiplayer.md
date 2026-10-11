# Online Multiplayer Foundation

BiasedDoom's online multiplayer layer is designed for player-hosted sessions
without accounts. A local Ed25519 device key identifies a player to the
session service; mutating control-plane requests are signed with a timestamp
and single-use nonce in addition to a short-lived bearer credential. The
service returns short-lived tickets and does not expose player IP addresses in
public listings.

## Session content

The host's active non-IWAD, non-engine resources are ordered into a content
manifest. Each archive has a BLAKE2b-256 digest, byte size, extension, and
load order; the manifest itself has a BLAKE2b-256 root digest recomputed and
verified from those exact ordered fields. Guests must review and
accept a previously unseen set before its packages download from the host.
Commercial IWADs, their required companion archives, and engine resources are
local prerequisites and are never copied. The base-game fingerprint includes
the ordered IWAD companion set, so a guest with a different commercial edition
is rejected before any mod review or transfer.

Directory-backed mods are converted into reproducible, session-local PK3
bundles before the manifest is created. The resolved bundle mount plan—not
the original directory—is what the host advertises and starts with. Guests
canonicalize matching local directories through the same path before mounting,
so a matching tree never produces a different resource plan.
Symlinks, special files, malformed names, unverified hashes, incomplete
downloads, and cache-path escapes are rejected. Downloads are written to a
hash-keyed partial file, verified, and atomically promoted to the local cache.

## Implemented foundation

The online path is integrated with the engine's existing UDP protocol. A host
starts with `-online-host [slots] -online-service <https-url>` and receives a
join code. A guest uses `-online-join <code> -online-service <https-url>`.
The normal launcher also exposes Online Relay controls alongside the retained
direct-IP/LAN path. Its Browse tab lists public sessions with title, region,
game mode, admitted-player count, modded status, password status, and whether
the host lobby is still open. The
browser can filter by mode, region, minimum player count, password status, and
modded/base content; selecting a row opens the normal Join tab with that code.
The launcher saves the chosen service URL and retains up to ten recent
service-URL/join-code pairs locally for the Recent picker; it never retains a
password, device credential, or manifest in that history. Host and Join both
provide a masked **Session password (not saved)** field; the equivalent script
option is
`-password <password>`.
The Host tab also states the mod-sharing contract up front: every active host
mod is reviewed by a guest before its encrypted download and exact-order mount.
The final package list and hashes are resolved by the engine at launch, so the
launcher never guesses at autoloaded content.

The public browser has a local **Block selected session** action. It hides that
join code only on the current device (no report is sent and no IP, account, or
password is saved); **Clear local blocked sessions** restores all hidden
listings. A separate **Report selected session** action requires a concrete
reason, submits one privacy-preserving moderation signal from the local
anonymous device, and hides that listing locally. Repeating a report from the
same device does not increase its moderation weight.

The service creates a fresh session transport secret, anonymous relay peer
labels, and a distinct short-lived relay capability for every session member.
Only authenticated `create` and `join` responses receive those credentials;
public listings never include them. The relay checks that a capability matches
the claimed anonymous peer label and forwards only to the anonymous intended
recipient, so one admitted player cannot impersonate another or receive other
players' routed datagrams. The engine maps anonymous
relay peers to synthetic local socket addresses, so the legacy connection code
continues to operate without learning another player's public address.

The encrypted relay is always available, but it can now accelerate a session
through NAT when both players have usable UDP mappings. After a host sends its
signed relay liveness record, the directory may return that relay-observed
mapping and the host's public identity only to an admitted joiner (never a
review-only preflight). The
guest sends a short-lived `BDDP` direct-route probe from the same game socket;
the host asks the directory to confirm the guest's admission, public key, and
relay-observed mapping before accepting it, then signs an acknowledgement.
Only that identity-bound source address can carry direct game packets. Failed,
missing, IPv6-only, expired, or mismatched candidates quietly stay on the
encrypted relay—there is no port-forwarding step or degraded join experience.
Even after direct game packets are enabled, the guest's five-second service
keepalive remains relayed so reconnect reservations and route expiry continue
to be enforced by the directory. If no authenticated direct packet arrives for
twelve seconds, that local fast path is discarded and normal traffic returns
to the relay automatically.

The directory records anonymous admitted devices to enforce the host's slot
limit and report aggregate active and reconnect-reserved counts in public
listings. While the lobby is open, guests emit a five-second authenticated
relay keepalive; when a listing refresh or a later join observes 30 seconds of
relay silence, it revokes that guest's route and reserves the slot for the
same anonymous device for five minutes. A graceful leave follows the same
reservation path immediately. The public browser shows that capacity as
**reconnecting** instead of falsely advertising it as an open slot. Once the
host starts gameplay, the lobby closes; snapshot-capable sessions instead show
**In progress (joinable)** and admit a guest through the bounded world-sync
path described below. Hosts renew their two-hour lease every 30 minutes and
close the listing and relay route when their game ends.

When an online host leaves gracefully, the directory validates the encrypted
legacy roster supplied by that host and selects the lexicographically first
eligible admitted device. It advances the session epoch, then the former host
sends the successor and epoch through the existing encrypted relay before its
legacy handoff packet. Peers reject old-epoch traffic, preserve their synthetic
relay addresses, and the elected player assumes the session lease. This keeps
the established deterministic handoff behavior while preventing a departing
host from closing the migrated listing.

If a host vanishes unexpectedly, it emits no more five-second Ed25519-signed
relay liveness records. After 15 seconds of *verified* silence, an admitted
peer can ask the directory to make the same deterministic election and advance
the epoch. Relay packet headers alone never count as host activity, so another
admitted player cannot suppress recovery by spoofing a host label. The elected
peer preserves the legacy packet-server state and re-establishes its encrypted
relay epoch; clients retain their synthetic peer routes throughout the
transition.

For a passworded session, the client submits a domain-separated BLAKE2b-256
password proof in its signed HTTPS control request. The raw password never
enters the manifest, ticket, public listing, relay packet, or session store.
The directory compares the proof before returning the manifest or issuing a
preflight route, so an incorrect password cannot inspect the host's mod list,
download its content, or consume a player slot. The legacy lobby repeats the
password check when it establishes the game connection.

When a guest's active resource plan differs from the host manifest, startup
stops until the guest explicitly approves that exact content-plan root and
anonymous host identity with
`-online-accept-mods <BLAKE2b-256-root>:<host-id>`. In the
launcher, **Review host mods** performs authenticated preflight without
reserving a player slot or downloading bytes, displays the anonymous host ID,
and lists every package's name, size, and BLAKE2b-256 digest. The approval
checkbox remains disabled until that review succeeds, and any edit to the
service URL, join code, or password clears the approval. Before any transfer,
launch re-resolves the manifest and rejects it if the reviewed root or host
identity changed. The command-line
path prints the same plan on a non-approved attempt. Once approved, the guest
resolves the host manifest delivered through the authenticated control plane
*before* mounting game resources, downloads each
package in bounded encrypted relay chunks, resumes existing partial files,
verifies every hash/archive, atomically promotes it into the hash-keyed cache,
and mounts the verified cache copies in the host's order. The directory and
relay never receive or retain mod bytes. Direct IP and LAN hosting remain
available for offline setups.

When every package in a later manifest is already present and verified in that
cache, the guest mounts that exact ordered cache plan immediately—there is no
second approval prompt or transfer. A changed root, package field, archive,
or hash invalidates that reuse path.

Manifest review and preflight transfer never consume one of the host's player
slots. A slot is admitted only when the guest proceeds from the Join tab into
the actual lobby connection or an authenticated in-progress snapshot request.

## Join in progress

When a snapshot-capable host has already begun play, the directory signs
`lobby_open: false` into the join ticket. The client then shows **Synchronizing
game in progress** rather than running the legacy lobby handshake. It captures
one bounded compact user-info string, generates a random request ID, and sends
an encrypted request to the host. The host verifies with the directory that
the relay peer is an admitted session member (a content-review-only relay
credential is not enough), reserves one unused player slot, and transfers a
versioned world snapshot plus a contiguous authoritative tic history.

The guest validates the archive and replay fence while still outside normal
world initialization. It restores through the ordinary serializer lifecycle,
replays to the reported live tic, acknowledges the exact completion fence, and
only then asks the host to queue a deterministic roster event. That event
travels through the normal packet-server tic stream. Applying it gives the
guest its assigned local slot and sends a final encrypted activation
acknowledgement; until then the host retains the handoff and no second join can
claim the reserved slot. Failed, malformed, cancelled, or timed-out transfers
never add a player to the running game. World synchronization has a five-minute
upper bound (followed by a fifteen-second activation-confirmation window), so
a vanished guest cannot hold the host's single handoff slot indefinitely.

Anonymous device credentials are held in a local profile under the
application-data directory. Startup refreshes the short-lived bearer token
using an Ed25519 proof from the locally stored private key while retaining the
same anonymous device identity. The profile is written through a permissions-
restricted temporary file and atomic rename; credentials are never placed in
launch arguments, listings, manifests, or game packets.

## Current limitations

Every online ticket carries a usable encrypted relay route, so hosting never
depends on port forwarding. Direct NAT acceleration is opportunistic and
currently supports relay-observed IPv4 UDP mappings; it never replaces the
relay as a fallback. Direct-IP and LAN remain available separately under
Advanced for offline or trusted local setups. Cache entries are verified, reusable, and evicted
least-recently-used when they exceed the 8 GiB default quota. Advanced users
can choose a 1–8192 MiB cache limit with `-online-cache-mb <MiB>`.

The engine still uses its existing pre-game lobby protocol for a session whose
ticket reports `lobby_open: true`. A closed session without the advertised
snapshot capability remains unavailable, while a closed snapshot-capable
session uses the explicit synchronization flow above. At most one such handoff
runs at a time, and the host refuses malformed user-info, changed epochs,
unadmitted relay peers, a full roster, unsafe archives, archive/hash failures,
or a replay gap. Reconnect reservations remain identity-bound. Unexpected-host
recovery depends on the directory and relay remaining reachable, and begins
only after the 15-second authenticated-silence window.

The engine provides a deliberately internal snapshot bridge:
`G_BuildOnlineSnapshotBundle` captures a detached, caller-owned active world
plus the same global server/RNG/script state categories written by a normal
save, and `G_RestoreOnlineSnapshotBundle` feeds a fully validated bundle back
through the ordinary savegame lifecycle. Neither function replaces the
hub/savegame snapshot buffer. Its bounded snapshot bundle carries
separately described world and global-state archives (with optional hub and
procedural-map components), so the live join bridge cannot mistake a level-only
archive for a complete restore set. The encoded bundle is wrapped as a stored,
CRC-checked archive before transport; no caller has to manufacture a serializer
buffer around transient vector memory. After its encrypted outer digest is
accepted, every inner serializer component is decompressed only within the
aggregate 256 MiB limit and checked against its declared CRC before it may be
given to the engine restore path. Demo record/playback modes reject this bridge
outright. Network-facing global and procedural serializers inspect parse errors
and reject malformed data without escalating a recoverable join failure into a
process-fatal savegame error.
Its protocol retries the authenticated Begin frame and
each missing chunk until the receiver acknowledges them, and waits for a final
archive-validation acknowledgement before completing. Snapshot chunks and
replay records are capped at 12 KiB, so they always fit inside one encrypted
relay datagram after its routing and authentication overhead. Duplicate Begin frames
preserve already-verified chunks, so ordinary relay loss never restarts a
transfer; an acknowledgement is accepted only after that exact chunk has been
emitted by the host. Every handoff has a fresh random transfer identity bound
into the Begin metadata and every data/control frame, so delayed encrypted
relay traffic from an earlier attempt cannot advance, complete, or cancel a
later attempt even if both captures have the same state and tic. The receiver retains a verified completed archive while
cycling acknowledgements, so a lost acknowledgement or reordering cannot leave
the host waiting forever. Versioned replay-tic frames bind every post-snapshot
event to the same epoch, snapshot tic, and archive digest before it can be
applied; the receiver keeps them in a bounded, ordered buffer and rejects a
handoff with any missing or conflicting tic. A separate authenticated
completion fence names the last replay tic, so the receiver cannot guess its
live boundary from packet timing; the guest also acknowledges its highest
contiguous replay tic, allowing the host to resend a gap instead of claiming a
complete handoff based on UDP delivery. It sends a distinct completion
acknowledgement only after it has the verified archive and that entire
contiguous replay range; the host retries the completion fence until that
acknowledgement arrives. The packet-server baseline (roster mask,
tic clock, consistency counters, arbitrator, and lobby epoch) is captured
alongside the snapshot, and the host retains a bounded canonical input history
before simulation consumes each tic. The restore bridge applies each verified
record with the ordinary `G_Ticker`/consistency ordering for every `TicDup`
simulation step, without accepting live packets or prediction; a command-reset
while replaying rejects the disposable join attempt rather than allowing it to
continue from ambiguous state. The final roster transition is an append-only
host event: every participant activates the approved slot with the same compact
userinfo at one tic boundary. The live control-plane handshake performs this
offline restore and deterministic admission before the browser advertises an
in-progress session as joinable.

The pending admission control protocol is an independent, relay-authenticated
frame family: a bounded NUL-terminated compact-userinfo request receives an
exact slot plus the fresh snapshot transfer identity, and only a matching
post-replay ready frame permits the host to schedule the normal roster event.
Its request, accept, ready, activated, and fixed-code rejection frames are
fuzzed beside the snapshot codec. The live control plane uses only these
request-bound frames; it cannot fall back to unbound legacy setup packets.

Before the host starts a handoff, it queues the relay-authenticated request
outside the UDP parser and asks the directory whether that anonymous peer is
an admitted session member. This is deliberately a host-only Boolean query:
a content-review preflight has a temporary relay route so it can fetch approved
mods, but it cannot use that route to receive a world snapshot or consume a
game slot. The host processes one pending admission at a time, assigns only an
unused configured slot, and repeats an acceptance only for the same request,
slot, and fresh transfer identity. Compact userinfo is constrained to one
well-formed NUL-terminated legacy string before it reaches the roster event;
trailing bytes and embedded NULs are rejected.

If a handoff can no longer complete (for example, its bounded replay history
expired), the host sends an authenticated, identity-bound cancellation with a
fixed reason code. A joiner accepts that only for its current archive and
transfer identity, so a delayed cancellation from an earlier archive attempt
cannot wipe a newer transfer.

## Compatibility

Online sessions require matching engine protocol versions (version 2 adds the
in-progress snapshot contract), IWAD fingerprints,
and exact content manifests. A guest never silently drops, reorders, or
replaces a host resource to obtain a connection.

## Testing

The fast checks require neither an IWAD nor a deployed service:

```bash
python3 tools/test-online-foundation.py
cmake -S . -B build -DBIASEDDOOM_BUILD_ONLINE_TESTS=ON
cmake --build build --target biaseddoom-online-core-test
./build/biaseddoom-online-core-test
cmake --build build --target biaseddoom-online-snapshot-fuzz
./build/biaseddoom-online-snapshot-fuzz
tools/test-online-service.sh
tools/test-online-redis-integration.sh
tools/test-online-service-container.sh
tools/test-online-e2e.sh --engine ./build/biaseddoom --iwad /path/to/doom2.wad
```

`tools/test-online-service.sh` uses a locally installed Go 1.22+ toolchain
when available, otherwise runs the same race-enabled service tests in the
official `golang:1.22` container. Docker therefore provides a no-install
fallback for engine-only contributors.

`tools/test-online-redis-integration.sh` starts an isolated Redis 7 container
and verifies the production RESP persistence client (binary values, TTL-backed
reservation, and session-index set operations). It cleans the fixture up on
success or failure.

`tools/test-online-service-container.sh` builds the production non-root image,
starts it with Redis on an isolated Docker network, and verifies the health and
aggregate-only metrics endpoints. It is the quickest deployment-shape smoke
test and needs no source-level Go installation. It does require Docker and
OpenSSL: the test generates a short-lived, self-signed certificate so it also
proves the service's direct-TLS startup path.

`tools/test-online-e2e.sh` is the engine-level integration check. It creates a
temporary PK3, starts independent host and guest profiles against an isolated
Redis/directory/relay fixture, confirms that a wrong password cannot reveal
the manifest and that an unapproved guest cannot join, uses the printed
identity-bound approval token, verifies the guest cache receives the host
package, and waits for the lobby to start. Linux CI runs it against the
`freedoom2.wad` supplied by the `freedoom` package. It needs an online-enabled
engine build, Docker, curl, zip, and timeout.

On Debian/Ubuntu, vcpkg builds the pinned `libsodium` port from source. If
CMake reports missing Autotools programs, install them once before configuring:

```bash
sudo apt install autoconf autoconf-archive automake libtool
```

For a local service smoke test, create a development-only signing secret, then
start the directory and relay:

```bash
mkdir -p services/online/secrets
umask 077; head -c 32 /dev/urandom > services/online/secrets/online_signing_secret
(cd services/online && docker compose up --build)
curl -fsS http://127.0.0.1:8080/healthz
curl -fsS http://127.0.0.1:8080/metrics
```

The client permits HTTP only for loopback development. Use HTTPS and a public
relay endpoint in every real deployment. Each player needs a distinct local
online identity. For a two-player test on one machine, use an absolute
`-online-data-dir` for each process; it isolates both the anonymous device key
and verified mod cache without needing a second OS account. The option rejects
relative paths and symlinks. With the local service running, create two empty
directories, start the host, copy its printed join code, then start the second
player using the same IWAD:

```bash
mkdir -p /tmp/biaseddoom-online-host /tmp/biaseddoom-online-guest
./build/biaseddoom -iwad /path/to/doom2.wad -online-host 2 \
  -online-data-dir /tmp/biaseddoom-online-host \
  -online-service http://127.0.0.1:8080 -online-title "Local co-op"
./build/biaseddoom -iwad /path/to/doom2.wad -online-join ABCD-EFGH \
  -online-data-dir /tmp/biaseddoom-online-guest \
  -online-service http://127.0.0.1:8080
```

Join codes are case-insensitive; the service displays uppercase codes with
ambiguous letters omitted for readability.

On separate computers, omit `-online-data-dir` and each normal application
profile supplies its own device identity automatically. Treat a custom data
directory as private player data; it contains an anonymous device key and any
approved host-provided content.

For the NAT-acceleration path, wait for the host to enter the lobby (it sends
its authenticated relay heartbeat first), then join from a distinct profile.
On two IPv4-capable peers—or two local profiles using the development relay—the
console should report **Online direct route established** within about twenty
seconds. Continue playing normally; the route is encrypted and a failed probe
silently remains on the relay. This is a diagnostic, not a different join mode:
do not open ports or share an address to make it work.

To exercise the same flow without command-line switches, open **Network
Game**, select **Host** or **Join Online Relay by code**, and enter the local
service URL. On a modded join, select **Review host mods** first: it must show
the host ID and package table before the download-approval checkbox becomes
available. Edit the join code, service URL, or password afterward and confirm
that approval clears again. For command-line automation, a modded guest first
runs the join command without `-online-accept-mods` to review the manifest,
then repeats it with `-online-accept-mods <printed-approval-token>` to accept
that exact host and transfer. For host
migration, start three players, then have the host quit cleanly and verify
the lexicographically first connected anonymous device continues the session.
For unexpected-loss recovery, terminate the host process instead, wait at
least 15 seconds for the signed-liveness timeout, and verify the remaining
players report the elected successor and a new epoch. The relay-silence
reservation can be observed by closing a guest process, waiting 30 seconds,
then refreshing Browse or attempting another join **before the host starts the
game**.

To exercise join in progress, host with three slots, connect one guest while
the lobby is open, and start a map. From a third profile, browse or enter the
same code: the row must say **In progress (joinable)** and the guest must show
**Synchronizing game in progress**. It should review/approve any previously
unknown host mods, restore into the running map without a new-game transition,
and become active only after the roster event at a tic boundary. Cancel that
join or kill the joining process during sync and confirm that the host retains
no extra player; retrying from that profile should work. A closed session
without the advertised join-in-progress capability is correctly unavailable,
not a successful join test.
