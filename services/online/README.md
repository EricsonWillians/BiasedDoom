# BiasedDoom Online Service

This is the first-party control plane and opaque UDP relay for BiasedDoom
online sessions. It keeps only short-lived session metadata in Redis. Game
packets and host-provided mod bytes are end-to-end encrypted by the clients and
are never stored by the service.

## Local development

Create a 32-byte-or-longer signing secret at
`services/online/secrets/online_signing_secret` (the directory is ignored by
Git), then run:

```bash
docker compose up --build
```

The control plane listens on TCP 8080 and the relay on UDP 3478. For a public
deployment, set both `BIASEDDOOM_ONLINE_TLS_CERT_FILE` and
`BIASEDDOOM_ONLINE_TLS_KEY_FILE` to mounted PEM files; the service then serves
HTTPS directly. A reverse proxy may terminate TLS instead, but never expose the
HTTP listener publicly. Supplying only one TLS setting fails fast rather than
silently downgrading the control plane. Clients reject unverified HTTPS
certificates. Set `BIASEDDOOM_ONLINE_RELAY_PUBLIC_ENDPOINT` to the public
`udp://host:port` address advertised to ticket holders; the Compose default is
loopback for local development.

Use `GET /healthz` for deployment health checks. It verifies that the required
Redis session store is reachable and returns `503` when it is not. `GET
/metrics` provides Prometheus text-format, aggregate-only counters for device
registration, sessions, joins/preflights/reconnections/admission failures,
accepted abuse reports, and opaque-relay traffic. The endpoint has no labels and deliberately
never includes player addresses, device IDs, session IDs, join codes, password
proofs, or content names/digests. Alert on a failing `/healthz`, sustained relay
packet rejections, and anomalous password/capacity rejection rates.
The HTTP server bounds header reads to five seconds and complete reads/writes
to fifteen seconds, in addition to per-endpoint request-size limits, so a slow
or stalled client cannot occupy a directory connection indefinitely.

## API contract

- `POST /v1/devices` registers an anonymous Ed25519 public key and returns a
  scoped bearer token. It too requires proof of possession of that key, and a
  later signed registration refreshes the bearer token while retaining the
  same anonymous device identity. Every state-changing request carries an
  Ed25519 signature over its method, path, body digest, timestamp, and
  single-use nonce.
- `POST /v1/sessions` and `POST /v1/sessions/{id}/renew` create and maintain
  a player-hosted session. Join tickets are relay-first and always carry the
  configured encrypted relay endpoint; legacy stored direct routes never
  displace that safe fallback. A relay-observed UDP mapping may additionally
  be issued privately for an authenticated, identity-bound direct probe; it
  never replaces the fallback. `leave` revokes a guest's active relay route
  while reserving its
  slot for the same anonymous device for five minutes; an authenticated relay
  keepalive lets the directory make the same transition after 30 seconds of
  guest silence during a listing refresh or admission. `close` tears down a
  host session and its relay authorization. `POST /v1/sessions/{id}/state`
  with `{"lobby_open":false}` is host-only and permanently closes lobby
  admission when gameplay starts. If the signed manifest advertises
  `allow_join_in_progress`, authenticated guests use the separate bounded
  snapshot/replay admission path; the state cannot be reopened as a legacy
  lobby. Reconnect reservations remain identity-bound in either state.
- `GET /v1/sessions` exposes only public session metadata. It never includes
  a host or guest address; `mode`, `players`, `reserved_players`,
  password/modded flags, `lobby_open`, and other listing fields are safe display metadata.
  `players` is the aggregate active-device count. `reserved_players` reports
  short reconnect holds, which also count toward `max_players` capacity but
  do not identify anyone.
- `POST /v1/join/{code}` returns a short-lived signed ticket, a content
  manifest, encrypted relay-route details, and—when the host has an observed
  IPv4 mapping—an optional host direct candidate plus its Ed25519 public key.
  The candidate is released only after an actual signed admission (never a
  review-only preflight) and is never listed publicly; a guest must complete an identity-bound direct probe before
  using it. The relay remains available if that probe cannot establish a route.
  Passworded sessions
  require the client to include a domain-separated BLAKE2b-256
  `password_proof`; the raw password is never stored or returned. The
  directory verifies this proof before releasing the manifest. A signed
  `{"preflight":true,"password_proof":"..."}` request supports content
  review/transfer without admitting a player; the normal join request enforces
  the slot limit. A closed session without `allow_join_in_progress` rejects
  admission with `409 Conflict`. For an eligible closed session, the ticket
  carries `lobby_open:false`; the client must use authenticated snapshot/replay
  admission and may not fall back to the legacy lobby handshake.
- `POST /v1/sessions/{id}/report` accepts a concise abuse reason as a listing
  moderation signal. A device may count only once per session, repeated
  submissions are idempotent, and a host cannot report its own session; the
  service retains only the aggregate count and never exposes report detail or
  reporter identity in a public response or listing.
- `POST /v1/sessions/{id}/admission` is host-only. It returns whether one
  supplied anonymous peer is admitted and, only when it is, that peer's
  registered public key plus its current relay-observed IPv4 mapping. The
  engine uses this private response to verify a signed direct-route probe; it
  is not a roster API and never appears in public browsing.
- `POST /v1/sessions/{id}/migrate` accepts the current host's bounded,
  authenticated legacy roster, validates every member, deterministically picks
  the lexicographically first eligible successor, advances the session epoch,
  and refreshes the relay lease. The departing engine distributes that epoch
  to connected peers over the existing encrypted relay; the endpoint never
  exposes an address or session key.
- `POST /v1/sessions/{id}/recover` accepts an admitted peer's bounded legacy
  roster only after the relay has observed 15 seconds without a valid host
  liveness proof. It makes the same deterministic election and advances the
  epoch; repeat callers observe the already elected host rather than advancing
  it again.
- `GET /healthz` confirms the directory can reach Redis; `GET /metrics`
  exposes privacy-safe aggregate counters suitable for service health and abuse
  alerts, including how often a private direct candidate was issued (never its
  address or whether a specific player used it).

Relay datagrams start with `BDOR`, a 16-byte opaque relay token, a `BDN1`
sender label, a per-device capability, and an anonymous recipient label before
the client-encrypted payload. The relay drops unknown/expired tokens and
invalid label/capability pairs, then forwards a packet only to the intended
authorized endpoint. This prevents one admitted player from forging another
player's relay label without making game traffic visible to the relay or
needlessly fanning it out to other players. Production deployments should use Redis-backed
relay authorization on each relay instance; the bundled single-process
deployment uses the API process's expiring relay registry.

Authenticated direct datagrams use `BDOD` instead of `BDOR` and retain the
same token, anonymous sender/recipient labels, epoch, sequence, and encrypted
payload. They omit the relay capability because the relay never sees them. The
engine accepts a `BDOD` source only after a signed `BDDP` probe matches both
the current host's private admission lookup and the relay-observed candidate;
otherwise it drops the packet and keeps using `BDOR`.

Hosts also send a compact `BDLV` liveness envelope every five seconds. Its
relay token, host peer label, and timestamp are covered by the host's
registered Ed25519 signature. The relay verifies this record and retains only
the most recent authenticated timestamp; it does not decrypt or inspect game
traffic. A peer cannot keep a dead host alive merely by forging a sender label.

Guests send an opaque, client-encrypted `BDGV` keepalive to the host at the
same cadence. The relay validates its existing per-device capability before it
records only the anonymous peer label's last-seen time; it neither decrypts nor
retains the payload. A listing refresh or new admission that observes 30
seconds of guest silence revokes that peer's relay route and creates the
five-minute reconnect reservation described above.

The bundled directory is intentionally one process so capacity admission can
be serialized before it persists the short-lived session in Redis. Deploy a
Redis transaction-backed admission primitive before horizontally scaling the
directory API.
