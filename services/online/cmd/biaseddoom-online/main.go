package main

import (
	"bufio"
	"bytes"
	"context"
	"crypto/ed25519"
	"crypto/hmac"
	"crypto/rand"
	"crypto/sha256"
	"encoding/base64"
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log"
	"net"
	"net/http"
	"net/url"
	"os"
	"sort"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
	"time"
	"unicode/utf8"
)

const (
	protocolVersion = 2
	// A host renews before this window expires, but the initial lease needs to
	// comfortably cover a normal co-op session even if the control plane is
	// briefly unavailable.
	sessionTTL                 = 2 * time.Hour
	joinTicketTTL              = 2 * time.Minute
	maxPlayers                 = 64
	maxContentFiles            = 256
	maxContentFileBytes uint64 = 2 << 30
	maxContentBytes     uint64 = 8 << 30
	maxArchiveBytes     uint64 = 512 << 20
	// Largest client datagram plus BDOR/token, BDN1, sender/capability,
	// recipient, epoch/sequence, and the encrypted recipient envelope.
	maxRelayPacketBytes = (16 << 10) + 84
	// Content preflight intentionally uses a short-lived socket before the
	// game binds its own UDP socket, so one player may briefly occupy two
	// relay endpoints. This does not increase the session player limit.
	maxRelayEndpoints = maxPlayers*2 + 1
	maxLimiterKeys    = 10000
	hostSilenceWindow = 15 * time.Second
	// Guests emit authenticated, opaque relay keepalives. Use a longer window
	// than host recovery so a short pause cannot evict an otherwise healthy
	// player from a player-hosted lobby.
	relayPeerSilenceWindow = 30 * time.Second
	// A short reservation preserves a dropped player's slot and anonymous
	// identity long enough to reconnect without turning an accidental network
	// blip into a permanent capacity race. Reserved slots still count toward
	// capacity, but not the public active-player count.
	reconnectReservationTTL = 5 * time.Minute
)

type contentPackage struct {
	Name            string `json:"name"`
	Digest          string `json:"digest"`
	Extension       string `json:"extension"`
	Size            uint64 `json:"size"`
	LoadOrder       uint32 `json:"load_order"`
	DirectoryBundle bool   `json:"directory_bundle"`
}

type contentManifest struct {
	Version    uint32           `json:"version"`
	RootDigest string           `json:"root_digest"`
	TotalBytes uint64           `json:"total_bytes"`
	Packages   []contentPackage `json:"packages"`
}

type sessionManifest struct {
	ProtocolVersion     uint32          `json:"protocol_version"`
	EngineVersion       string          `json:"engine_version"`
	IWADDigest          string          `json:"iwad_digest"`
	Title               string          `json:"title"`
	Region              string          `json:"region"`
	Mode                string          `json:"mode"`
	Visibility          string          `json:"visibility"`
	MaxPlayers          uint32          `json:"max_players"`
	PasswordRequired    bool            `json:"password_required"`
	AllowJoinInProgress bool            `json:"allow_join_in_progress"`
	Content             contentManifest `json:"content"`
}

type session struct {
	ID         string          `json:"id"`
	JoinCode   string          `json:"join_code"`
	HostDevice string          `json:"host_device"`
	Manifest   sessionManifest `json:"manifest"`
	Epoch      uint32          `json:"epoch"`
	Endpoint   string          `json:"endpoint"`
	RelayToken string          `json:"relay_token"`
	// This is a per-session transport secret. It is returned only from the
	// authenticated create/join endpoints and is never sent to public listing
	// consumers or written to relay packets.
	SessionKey string `json:"session_key"`
	// PasswordProof is a BLAKE2b-256 proof submitted over the signed TLS
	// control channel. It is never copied into Manifest, tickets, listings, or
	// relay packets, and lets the directory reject a wrong password before it
	// discloses protected content metadata.
	PasswordProof string `json:"password_proof,omitempty"`
	// RelayAuth holds per-device opaque capabilities. The relay checks the
	// capability in the datagram header before accepting a claimed peer label,
	// so sharing the session encryption key cannot impersonate another player.
	RelayAuth   map[string]string `json:"relay_auth"`
	CreatedAt   int64             `json:"created_at"`
	ReportCount uint32            `json:"report_count"`
	// Reporters is private, bounded moderation state. It prevents one device
	// from repeatedly raising the aggregate listing-report count; unlike
	// ReportCount it is never sent in a listing or ticket.
	Reporters map[string]bool `json:"reporters,omitempty"`
	// LobbyOpen is host-controlled admission state. It is distinct from
	// Manifest.AllowJoinInProgress: a closed snapshot-capable session may admit
	// a guest through the engine's bounded world-synchronization protocol.
	LobbyOpen bool `json:"lobby_open"`
	// Members are currently admitted anonymous device identifiers. They are
	// internal state used for capacity enforcement and public aggregate player
	// counts; they never appear in a listing, ticket, or relay packet.
	Members map[string]bool `json:"members"`
	// Reconnecting holds an expiring Unix-second reservation for a former
	// member. It protects the slot for that same anonymous device after a
	// disconnect while keeping its credential and endpoint revoked.
	Reconnecting map[string]int64 `json:"reconnecting,omitempty"`
}

type publicSession struct {
	SessionID           string `json:"session_id"`
	JoinCode            string `json:"join_code"`
	Title               string `json:"title"`
	Region              string `json:"region"`
	Mode                string `json:"mode"`
	Players             uint32 `json:"players"`
	ReservedPlayers     uint32 `json:"reserved_players"`
	MaxPlayers          uint32 `json:"max_players"`
	PasswordRequired    bool   `json:"password_required"`
	Modded              bool   `json:"modded"`
	AllowJoinInProgress bool   `json:"allow_join_in_progress"`
	LobbyOpen           bool   `json:"lobby_open"`
}

type createSessionRequest struct {
	sessionManifest
	PasswordProof string `json:"password_proof"`
}

type joinRequest struct {
	Preflight     bool   `json:"preflight"`
	PasswordProof string `json:"password_proof"`
}

// migrateRequest is intentionally limited to the still-connected legacy
// roster that the departing host can actually hand off to. The directory
// validates every listed identity against its admitted members and chooses the
// lexicographically first one itself; it never trusts the caller to nominate a
// successor.
type migrateRequest struct {
	EligiblePeerIDs []string `json:"eligible_peer_ids"`
}

type recoverRequest struct {
	EligiblePeerIDs []string `json:"eligible_peer_ids"`
}

// normalizeMembers keeps sessions written by earlier service versions valid
// while introducing reconnect reservations. It also guarantees a session can
// never reserve its current host, which remains the authority for the session
// lifetime rather than a reconnecting guest.
func (s *session) normalizeMembers() bool {
	changed := false
	if s.Members == nil {
		s.Members = map[string]bool{}
		changed = true
	}
	if !s.Members[s.HostDevice] {
		s.Members[s.HostDevice] = true
		changed = true
	}
	if s.Reconnecting == nil {
		s.Reconnecting = map[string]int64{}
		changed = true
	}
	if _, reserved := s.Reconnecting[s.HostDevice]; reserved {
		delete(s.Reconnecting, s.HostDevice)
		changed = true
	}
	return changed
}

// pruneReconnectReservations returns true if it released any expired or
// inconsistent reservation. Callers persist the changed session before they
// make an admission decision, so a stale reservation cannot indefinitely
// consume a lobby slot.
func (s *session) pruneReconnectReservations(now time.Time) bool {
	changed := s.normalizeMembers()
	for device, expiresAt := range s.Reconnecting {
		if !validSessionID(device) || device == s.HostDevice || s.Members[device] || expiresAt <= now.Unix() {
			delete(s.Reconnecting, device)
			changed = true
		}
	}
	return changed
}

func (s *session) occupiedSlots() uint32 {
	return uint32(len(s.Members) + len(s.Reconnecting))
}

func (s *session) reservedPlayerCount() uint32 {
	return uint32(len(s.Reconnecting))
}

// Store is deliberately small: only signed metadata and relay routing tokens
// live in Redis. Mod bytes never enter this service.
type store interface {
	put(context.Context, string, []byte, time.Duration) error
	putIfAbsent(context.Context, string, []byte, time.Duration) (bool, error)
	get(context.Context, string) ([]byte, bool, error)
	del(context.Context, string) error
	setAdd(context.Context, string, string) error
	setRemove(context.Context, string, string) error
	setMembers(context.Context, string) ([]string, error)
}

type redisStore struct{ address string }

func (r redisStore) command(ctx context.Context, args ...string) (any, error) {
	dialer := net.Dialer{Timeout: 3 * time.Second}
	conn, err := dialer.DialContext(ctx, "tcp", r.address)
	if err != nil {
		return nil, err
	}
	defer conn.Close()
	_ = conn.SetDeadline(time.Now().Add(5 * time.Second))
	w := bufio.NewWriter(conn)
	fmt.Fprintf(w, "*%d\r\n", len(args))
	for _, arg := range args {
		fmt.Fprintf(w, "$%d\r\n%s\r\n", len(arg), arg)
	}
	if err := w.Flush(); err != nil {
		return nil, err
	}
	return readRESP(bufio.NewReader(conn))
}

func readRESP(r *bufio.Reader) (any, error) {
	prefix, err := r.ReadByte()
	if err != nil {
		return nil, err
	}
	line, err := r.ReadString('\n')
	if err != nil {
		return nil, err
	}
	line = strings.TrimSuffix(strings.TrimSuffix(line, "\n"), "\r")
	switch prefix {
	case '+':
		return line, nil
	case '-':
		return nil, errors.New(line)
	case ':':
		return strconv.ParseInt(line, 10, 64)
	case '$':
		n, err := strconv.Atoi(line)
		if err != nil {
			return nil, err
		}
		if n == -1 {
			return nil, nil
		}
		data := make([]byte, n)
		if _, err := io.ReadFull(r, data); err != nil {
			return nil, err
		}
		if _, err := r.Discard(2); err != nil {
			return nil, err
		}
		return string(data), nil
	case '*':
		n, err := strconv.Atoi(line)
		if err != nil {
			return nil, err
		}
		if n == -1 {
			return nil, nil
		}
		values := make([]any, n)
		for i := range values {
			if values[i], err = readRESP(r); err != nil {
				return nil, err
			}
		}
		return values, nil
	default:
		return nil, fmt.Errorf("unknown redis response prefix %q", prefix)
	}
}

func (r redisStore) put(ctx context.Context, key string, value []byte, ttl time.Duration) error {
	_, err := r.command(ctx, "SET", key, string(value), "PX", strconv.FormatInt(ttl.Milliseconds(), 10))
	return err
}
func (r redisStore) putIfAbsent(ctx context.Context, key string, value []byte, ttl time.Duration) (bool, error) {
	v, err := r.command(ctx, "SET", key, string(value), "PX", strconv.FormatInt(ttl.Milliseconds(), 10), "NX")
	if err != nil || v == nil {
		return false, err
	}
	_, ok := v.(string)
	return ok, nil
}
func (r redisStore) get(ctx context.Context, key string) ([]byte, bool, error) {
	v, err := r.command(ctx, "GET", key)
	if err != nil || v == nil {
		return nil, false, err
	}
	s, ok := v.(string)
	return []byte(s), ok, nil
}
func (r redisStore) del(ctx context.Context, key string) error {
	_, err := r.command(ctx, "DEL", key)
	return err
}
func (r redisStore) setAdd(ctx context.Context, key, value string) error {
	_, err := r.command(ctx, "SADD", key, value)
	return err
}
func (r redisStore) setRemove(ctx context.Context, key, value string) error {
	_, err := r.command(ctx, "SREM", key, value)
	return err
}
func (r redisStore) setMembers(ctx context.Context, key string) ([]string, error) {
	v, err := r.command(ctx, "SMEMBERS", key)
	if err != nil || v == nil {
		return nil, err
	}
	values, ok := v.([]any)
	if !ok {
		return nil, errors.New("invalid redis set response")
	}
	out := make([]string, 0, len(values))
	for _, value := range values {
		if s, ok := value.(string); ok {
			out = append(out, s)
		}
	}
	return out, nil
}

type limiter struct {
	mu   sync.Mutex
	hits map[string][]time.Time
}

func (l *limiter) pruneLocked(cutoff time.Time) {
	for key, values := range l.hits {
		kept := values[:0]
		for _, hit := range values {
			if hit.After(cutoff) {
				kept = append(kept, hit)
			}
		}
		if len(kept) == 0 {
			delete(l.hits, key)
		} else {
			l.hits[key] = kept
		}
	}
}

func (l *limiter) allow(key string, now time.Time) bool {
	l.mu.Lock()
	defer l.mu.Unlock()
	cutoff := now.Add(-time.Minute)
	if _, exists := l.hits[key]; !exists && len(l.hits) >= maxLimiterKeys {
		l.pruneLocked(cutoff)
		if len(l.hits) >= maxLimiterKeys {
			return false
		}
	}
	kept := l.hits[key][:0]
	for _, hit := range l.hits[key] {
		if hit.After(cutoff) {
			kept = append(kept, hit)
		}
	}
	if len(kept) >= 60 {
		l.hits[key] = kept
		return false
	}
	l.hits[key] = append(kept, now)
	return true
}

type relayRegistry struct {
	mu              sync.RWMutex
	endpoints       map[string]map[string]*net.UDPAddr
	endpointPeers   map[string]map[string]string
	authorizations  map[string]time.Time
	peerCredentials map[string]map[string]relayPeerCredential
	hostPeers       map[string]string
	hostPublicKeys  map[string]ed25519.PublicKey
	hostLastPackets map[string]time.Time
	peerLastPackets map[string]map[string]time.Time
	metrics         *onlineMetrics
}

type relayPeerCredential struct {
	Value   string
	Expires time.Time
}

// onlineMetrics intentionally has no labels. In particular, it must never
// carry a session id, join code, IP address, device id, password proof, or
// content name/digest. It exists so an operator can alert on aggregate service
// health without turning the metrics endpoint into a player-tracking API.
type onlineMetrics struct {
	deviceRegistrations atomic.Uint64
	sessionsCreated     atomic.Uint64
	joinRequests        atomic.Uint64
	joinPreflights      atomic.Uint64
	joinAdmitted        atomic.Uint64
	joinReconnected     atomic.Uint64
	joinPasswordDenied  atomic.Uint64
	joinCapacityDenied  atomic.Uint64
	abuseReports        atomic.Uint64
	hostMigrations      atomic.Uint64
	directCandidates    atomic.Uint64
	relayPacketsIn      atomic.Uint64
	relayPacketsOut     atomic.Uint64
	relayPacketsDenied  atomic.Uint64
}

func (r *relayRegistry) ensureMapsLocked() {
	if r.endpoints == nil {
		r.endpoints = map[string]map[string]*net.UDPAddr{}
	}
	if r.endpointPeers == nil {
		r.endpointPeers = map[string]map[string]string{}
	}
	if r.authorizations == nil {
		r.authorizations = map[string]time.Time{}
	}
	if r.peerCredentials == nil {
		r.peerCredentials = map[string]map[string]relayPeerCredential{}
	}
	if r.hostPeers == nil {
		r.hostPeers = map[string]string{}
	}
	if r.hostPublicKeys == nil {
		r.hostPublicKeys = map[string]ed25519.PublicKey{}
	}
	if r.hostLastPackets == nil {
		r.hostLastPackets = map[string]time.Time{}
	}
	if r.peerLastPackets == nil {
		r.peerLastPackets = map[string]map[string]time.Time{}
	}
}

func (r *relayRegistry) pruneLocked(now time.Time) {
	for token, expires := range r.authorizations {
		if !expires.After(now) {
			delete(r.authorizations, token)
			delete(r.endpoints, token)
			delete(r.endpointPeers, token)
			delete(r.peerCredentials, token)
			delete(r.hostPeers, token)
			delete(r.hostPublicKeys, token)
			delete(r.hostLastPackets, token)
			delete(r.peerLastPackets, token)
		}
	}
	for token, credentials := range r.peerCredentials {
		for peer, credential := range credentials {
			if !credential.Expires.After(now) {
				delete(credentials, peer)
				delete(r.peerLastPackets[token], peer)
				for address, endpointPeer := range r.endpointPeers[token] {
					if endpointPeer == peer {
						delete(r.endpointPeers[token], address)
						delete(r.endpoints[token], address)
					}
				}
			}
		}
	}
}

func (r *relayRegistry) allow(token string, expires time.Time, hostPeer ...string) {
	r.mu.Lock()
	defer r.mu.Unlock()
	now := time.Now()
	r.pruneLocked(now)
	r.ensureMapsLocked()
	r.authorizations[token] = expires
	if len(hostPeer) > 0 && validSessionID(hostPeer[0]) {
		previousHost, hadHost := r.hostPeers[token]
		r.hostPeers[token] = hostPeer[0]
		// Give a newly created or migrated host a short window to send its first
		// heartbeat. Refreshing a route for the same host must not let a guest
		// keep an absent host artificially alive by repeatedly joining.
		if !hadHost || previousHost != hostPeer[0] {
			r.hostLastPackets[token] = now
		}
	}
}

func (r *relayRegistry) setHostIdentity(token, hostPeer string, publicKey ed25519.PublicKey) bool {
	if !validSessionID(hostPeer) || len(publicKey) != ed25519.PublicKeySize {
		return false
	}
	r.mu.Lock()
	defer r.mu.Unlock()
	r.ensureMapsLocked()
	previousHost, hadHost := r.hostPeers[token]
	r.hostPeers[token] = hostPeer
	r.hostPublicKeys[token] = append(ed25519.PublicKey(nil), publicKey...)
	if !hadHost || previousHost != hostPeer {
		r.hostLastPackets[token] = time.Now()
	}
	return true
}
func (r *relayRegistry) revoke(token string) {
	r.mu.Lock()
	defer r.mu.Unlock()
	delete(r.authorizations, token)
	delete(r.endpoints, token)
	delete(r.endpointPeers, token)
	delete(r.peerCredentials, token)
	delete(r.hostPeers, token)
	delete(r.hostPublicKeys, token)
	delete(r.hostLastPackets, token)
	delete(r.peerLastPackets, token)
}

func (r *relayRegistry) setPeerCredential(token, peer, credential string, expires time.Time) bool {
	now := time.Now()
	if !validSessionID(peer) || !validSessionID(credential) || !expires.After(now) {
		return false
	}
	r.mu.Lock()
	defer r.mu.Unlock()
	r.ensureMapsLocked()
	if tokenExpiry, allowed := r.authorizations[token]; !allowed || !tokenExpiry.After(time.Now()) {
		return false
	}
	if r.peerCredentials[token] == nil {
		r.peerCredentials[token] = map[string]relayPeerCredential{}
	}
	previous, existed := r.peerCredentials[token][peer]
	r.peerCredentials[token][peer] = relayPeerCredential{Value: credential, Expires: expires}
	// Issue time is the initial grace period after admission. Refreshing a
	// session route must not make a disconnected peer appear alive forever.
	if !existed || previous.Value != credential {
		if r.peerLastPackets[token] == nil {
			r.peerLastPackets[token] = map[string]time.Time{}
		}
		r.peerLastPackets[token][peer] = now
	}
	return true
}

func (r *relayRegistry) revokePeer(token, peer string) {
	r.mu.Lock()
	defer r.mu.Unlock()
	delete(r.peerCredentials[token], peer)
	delete(r.peerLastPackets[token], peer)
	for address, endpointPeer := range r.endpointPeers[token] {
		if endpointPeer == peer {
			delete(r.endpointPeers[token], address)
			delete(r.endpoints[token], address)
		}
	}
}

func (r *relayRegistry) hostRecentlyActive(token, hostPeer string, now time.Time) bool {
	r.mu.RLock()
	defer r.mu.RUnlock()
	last, found := r.hostLastPackets[token]
	return found && r.hostPeers[token] == hostPeer && last.After(now.Add(-hostSilenceWindow))
}

// peerRecentlyActive is deliberately an in-memory relay signal rather than a
// player-address record. The directory sees only an anonymous peer label and
// turns expired liveness into an expiring reconnect reservation.
func (r *relayRegistry) peerRecentlyActive(token, peer string, now time.Time) bool {
	r.mu.RLock()
	defer r.mu.RUnlock()
	last, found := r.peerLastPackets[token][peer]
	return found && last.After(now.Add(-relayPeerSilenceWindow))
}

// directEndpoint returns a relay-observed UDP mapping only to an authenticated
// session member. It is never a public-listing field and is intentionally
// short lived: the game still requires a signed direct probe before using it.
// IPv6 candidates remain on the encrypted relay for now because the engine's
// legacy socket adapter is IPv4-only; a missing candidate is a normal and
// safe relay-fallback outcome.
func (r *relayRegistry) directEndpoint(token, peer string, now time.Time) string {
	r.mu.RLock()
	defer r.mu.RUnlock()
	if expires, authorized := r.authorizations[token]; !authorized || !expires.After(now) {
		return ""
	}
	credential, admitted := r.peerCredentials[token][peer]
	if !admitted || !credential.Expires.After(now) {
		return ""
	}
	if r.hostPeers[token] == peer {
		if last, active := r.hostLastPackets[token]; !active || !last.After(now.Add(-hostSilenceWindow)) {
			return ""
		}
	} else if last, active := r.peerLastPackets[token][peer]; !active || !last.After(now.Add(-relayPeerSilenceWindow)) {
		return ""
	}
	for address, endpointPeer := range r.endpointPeers[token] {
		if endpointPeer != peer {
			continue
		}
		endpoint := r.endpoints[token][address]
		if endpoint == nil || endpoint.IP.To4() == nil || endpoint.Port == 0 {
			continue
		}
		return "udp://" + net.JoinHostPort(endpoint.IP.To4().String(), strconv.Itoa(endpoint.Port))
	}
	return ""
}

// bindEndpointLocked remembers the most recent relay-observed mapping for one
// game socket. A client uses one UDP socket per session, so keeping older
// mappings can make a later direct probe pick an expired NAT translation.
// The caller already holds r.mu.
func (r *relayRegistry) bindEndpointLocked(token, peer string, from *net.UDPAddr) bool {
	if from == nil {
		return false
	}
	r.ensureMapsLocked()
	peers := r.endpoints[token]
	if peers == nil {
		peers = map[string]*net.UDPAddr{}
		r.endpoints[token] = peers
	}
	endpointPeers := r.endpointPeers[token]
	if endpointPeers == nil {
		endpointPeers = map[string]string{}
		r.endpointPeers[token] = endpointPeers
	}
	for address, endpointPeer := range endpointPeers {
		if endpointPeer == peer && address != from.String() {
			delete(endpointPeers, address)
			delete(peers, address)
		}
	}
	if _, known := peers[from.String()]; !known && len(peers) >= maxRelayEndpoints {
		return false
	}
	peers[from.String()] = from
	endpointPeers[from.String()] = peer
	return true
}
func (r *relayRegistry) forward(conn *net.UDPConn, packet []byte, from *net.UDPAddr) {
	// BDOR + 16 opaque relay-token bytes + encrypted payload. The token only
	// selects a fan-out group; relay code neither decrypts nor persists bytes.
	if r.metrics != nil {
		r.metrics.relayPacketsIn.Add(1)
	}
	if len(packet) <= 20 || len(packet) > maxRelayPacketBytes || string(packet[:4]) != "BDOR" {
		if r.metrics != nil {
			r.metrics.relayPacketsDenied.Add(1)
		}
		return
	}
	token := hex.EncodeToString(packet[4:20])
	r.mu.Lock()
	now := time.Now()
	expires, authorized := r.authorizations[token]
	if !authorized || !expires.After(now) {
		if authorized {
			delete(r.authorizations, token)
			delete(r.endpoints, token)
			delete(r.endpointPeers, token)
			delete(r.peerCredentials, token)
			delete(r.hostPeers, token)
			delete(r.hostPublicKeys, token)
			delete(r.hostLastPackets, token)
			delete(r.peerLastPackets, token)
		}
		r.mu.Unlock()
		if r.metrics != nil {
			r.metrics.relayPacketsDenied.Add(1)
		}
		return
	}
	// A host liveness heartbeat is separate from game datagrams. It carries the
	// host's Ed25519 signature over its relay token, BDLV envelope, peer ID,
	// and timestamp. The relay therefore never decrypts or stores game bytes,
	// but an admitted guest cannot spoof host activity to suppress recovery.
	if len(packet) >= 24 && string(packet[20:24]) == "BDLV" {
		const heartbeatSize = 4 + 16 + 4 + 16 + 8 + ed25519.SignatureSize
		validHeartbeat := len(packet) == heartbeatSize && r.hostPeers[token] == hex.EncodeToString(packet[24:40])
		if validHeartbeat {
			timestamp := int64(binary.LittleEndian.Uint64(packet[40:48]))
			validHeartbeat = timestamp >= now.Add(-hostSilenceWindow).Unix() && timestamp <= now.Add(hostSilenceWindow).Unix()
		}
		if validHeartbeat {
			publicKey := r.hostPublicKeys[token]
			message := make([]byte, 0, 16+4+16+8)
			message = append(message, packet[4:20]...)
			message = append(message, packet[20:48]...)
			validHeartbeat = len(publicKey) == ed25519.PublicKeySize && ed25519.Verify(publicKey, message, packet[48:])
		}
		if validHeartbeat {
			// A host may have no ordinary game packet to send until the first
			// guest tries to connect. Register its endpoint only after signature
			// verification so that first encrypted setup packet has a relay route
			// without allowing a guest to forge a host presence.
			validHeartbeat = r.bindEndpointLocked(token, r.hostPeers[token], from)
		}
		if validHeartbeat {
			r.hostLastPackets[token] = now
		} else if r.metrics != nil {
			r.metrics.relayPacketsDenied.Add(1)
		}
		r.mu.Unlock()
		return
	}
	// Normal relay datagrams are BDN1 frames with a sender label, a capability
	// bound to that label, and an anonymous recipient label. The payload remains
	// opaque, but the relay will not route a packet that merely *claims* to be
	// another peer, nor fan valid packets out to unrelated session members.
	const authenticatedHeader = 4 + 16 + 4 + 16 + 16 + 16
	if len(packet) < authenticatedHeader || string(packet[20:24]) != "BDN1" {
		r.mu.Unlock()
		if r.metrics != nil {
			r.metrics.relayPacketsDenied.Add(1)
		}
		return
	}
	peerID := hex.EncodeToString(packet[24:40])
	credential := hex.EncodeToString(packet[40:56])
	targetPeerID := hex.EncodeToString(packet[56:72])
	expected, allowedPeer := r.peerCredentials[token][peerID]
	targetCredential, allowedTarget := r.peerCredentials[token][targetPeerID]
	if peerID == targetPeerID || !allowedPeer || !expected.Expires.After(now) ||
		!allowedTarget || !targetCredential.Expires.After(now) || !hmac.Equal([]byte(expected.Value), []byte(credential)) {
		r.mu.Unlock()
		if r.metrics != nil {
			r.metrics.relayPacketsDenied.Add(1)
		}
		return
	}
	if !r.bindEndpointLocked(token, peerID, from) {
		r.mu.Unlock()
		if r.metrics != nil {
			r.metrics.relayPacketsDenied.Add(1)
		}
		return
	}
	peers := r.endpoints[token]
	if r.peerLastPackets[token] == nil {
		r.peerLastPackets[token] = map[string]time.Time{}
	}
	r.peerLastPackets[token][peerID] = now
	targets := make([]*net.UDPAddr, 0, 1)
	for address, peer := range peers {
		endpointPeer := r.endpointPeers[token][address]
		endpointCredential, endpointAllowed := r.peerCredentials[token][endpointPeer]
		if endpointPeer == targetPeerID && address != from.String() && endpointAllowed && endpointCredential.Expires.After(now) {
			targets = append(targets, peer)
		}
	}
	r.mu.Unlock()
	for _, target := range targets {
		_, _ = conn.WriteToUDP(packet, target)
	}
	if r.metrics != nil {
		r.metrics.relayPacketsOut.Add(uint64(len(targets)))
	}
}

type api struct {
	store         store
	secret        []byte
	relay         *relayRegistry
	relayEndpoint string
	limiter       limiter
	metrics       *onlineMetrics
	// Device registration is a proof-of-possession operation. Serialize the
	// public-key-to-device mapping so two concurrent refreshes cannot assign
	// separate anonymous device IDs to the same local key.
	deviceMu sync.Mutex
	// Session updates are serialized in this directory process so simultaneous
	// joins cannot overbook a player-hosted lobby. The deployment is a single
	// directory instance backed by Redis; horizontal sharding needs a Redis
	// transaction/Lua equivalent before adding more directory replicas.
	sessionMu sync.Mutex
}

func randomHex(bytes int) string {
	raw := make([]byte, bytes)
	if _, err := rand.Read(raw); err != nil {
		panic(err)
	}
	return hex.EncodeToString(raw)
}
func randomCode() string {
	const alphabet = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789"
	raw := make([]byte, 8)
	if _, err := rand.Read(raw); err != nil {
		panic(err)
	}
	for i := range raw {
		raw[i] = alphabet[int(raw[i])%len(alphabet)]
	}
	return string(raw[:4]) + "-" + string(raw[4:])
}

func validDigest(value string) bool {
	if len(value) != 64 {
		return false
	}
	for _, c := range value {
		if !(c >= '0' && c <= '9' || c >= 'a' && c <= 'f') {
			return false
		}
	}
	return true
}

func validPasswordProof(value string) bool {
	return validDigest(value)
}
func validSessionID(value string) bool {
	if len(value) != 32 {
		return false
	}
	for _, c := range value {
		if !(c >= '0' && c <= '9' || c >= 'a' && c <= 'f') {
			return false
		}
	}
	return true
}
func validSessionKey(value string) bool {
	if len(value) != 64 {
		return false
	}
	for _, c := range value {
		if !(c >= '0' && c <= '9' || c >= 'a' && c <= 'f') {
			return false
		}
	}
	return true
}
func validPackageName(value string) bool {
	if len(value) == 0 || len(value) > 128 || value == "." || value == ".." || strings.ContainsAny(value, "/\\\x00") {
		return false
	}
	for _, c := range value {
		if c < 32 || c == 127 {
			return false
		}
	}
	return true
}
func validExtension(value string) bool {
	if len(value) < 2 || len(value) > 16 || value[0] != '.' {
		return false
	}
	for _, c := range value[1:] {
		if !(c >= 'a' && c <= 'z' || c >= '0' && c <= '9') {
			return false
		}
	}
	return true
}
func validDisplayText(value string, maximum int, required bool) bool {
	if (required && value == "") || len(value) > maximum || !utf8.ValidString(value) {
		return false
	}
	for _, c := range value {
		if c < 32 || c == 127 {
			return false
		}
	}
	return true
}
func validJoinCode(value string) bool {
	if len(value) != 9 || value[4] != '-' {
		return false
	}
	for i, c := range value {
		if i == 4 {
			continue
		}
		if !strings.ContainsRune("ABCDEFGHJKLMNPQRSTUVWXYZ23456789", c) {
			return false
		}
	}
	return true
}
func validManifest(m sessionManifest) error {
	if m.ProtocolVersion != protocolVersion || !validDisplayText(m.EngineVersion, 128, true) || !validDigest(m.IWADDigest) || !validDisplayText(m.Title, 96, true) || !validDisplayText(m.Region, 32, false) || !validDisplayText(m.Mode, 32, true) || m.MaxPlayers == 0 || m.MaxPlayers > maxPlayers || (m.Visibility != "public" && m.Visibility != "private") {
		return errors.New("invalid session manifest")
	}
	if m.Content.Version != protocolVersion || !validDigest(m.Content.RootDigest) {
		return errors.New("invalid content manifest")
	}
	if len(m.Content.Packages) > maxContentFiles {
		return errors.New("content manifest has too many packages")
	}
	var total uint64
	var last uint32
	for i, p := range m.Content.Packages {
		if !validPackageName(p.Name) || !validDigest(p.Digest) || !validExtension(p.Extension) || p.LoadOrder == 0 || p.Size > maxContentFileBytes || ((p.Extension == ".pk3" || p.Extension == ".zip") && p.Size > maxArchiveBytes) || (p.DirectoryBundle && p.Extension != ".pk3") || (i > 0 && p.LoadOrder <= last) {
			return errors.New("invalid content package")
		}
		if total > maxContentBytes-p.Size {
			return errors.New("content manifest exceeds the online limit")
		}
		total += p.Size
		last = p.LoadOrder
	}
	if total != m.Content.TotalBytes {
		return errors.New("content manifest byte total mismatch")
	}
	return nil
}

func validUDPEndpoint(value string) bool {
	parsed, err := url.Parse(value)
	if err != nil || parsed.Scheme != "udp" || parsed.Host == "" || parsed.User != nil || parsed.Path != "" || parsed.RawQuery != "" || parsed.Fragment != "" {
		return false
	}
	_, port, err := net.SplitHostPort(parsed.Host)
	if err != nil {
		return false
	}
	portNumber, err := strconv.ParseUint(port, 10, 16)
	return err == nil && portNumber != 0
}

func (a *api) issueToken(device string, ttl time.Duration) string {
	expires := time.Now().Add(ttl).Unix()
	payload := device + "." + strconv.FormatInt(expires, 10)
	mac := hmac.New(sha256.New, a.secret)
	_, _ = mac.Write([]byte(payload))
	return payload + "." + base64.RawURLEncoding.EncodeToString(mac.Sum(nil))
}
func (a *api) authorize(r *http.Request) (string, error) {
	parts := strings.Fields(r.Header.Get("Authorization"))
	if len(parts) != 2 || parts[0] != "Bearer" {
		return "", errors.New("missing bearer token")
	}
	items := strings.Split(parts[1], ".")
	if len(items) != 3 {
		return "", errors.New("invalid bearer token")
	}
	expiry, err := strconv.ParseInt(items[1], 10, 64)
	if err != nil || time.Now().Unix() >= expiry {
		return "", errors.New("expired bearer token")
	}
	payload := items[0] + "." + items[1]
	mac := hmac.New(sha256.New, a.secret)
	_, _ = mac.Write([]byte(payload))
	actual, err := base64.RawURLEncoding.DecodeString(items[2])
	if err != nil || !hmac.Equal(actual, mac.Sum(nil)) {
		return "", errors.New("invalid bearer token")
	}
	return items[0], nil
}

func signaturePayload(method, path, timestamp, nonce string, body []byte) string {
	digest := sha256.Sum256(body)
	return method + "\n" + path + "\n" + timestamp + "\n" + nonce + "\n" + base64.StdEncoding.EncodeToString(digest[:])
}

// authorizeSigned ties every state-changing call to the locally held Ed25519
// device key. The nonce is a short-lived single-use Redis key, so a captured
// request cannot be replayed during its timestamp window.
func (a *api) authorizeSigned(r *http.Request) (string, error) {
	device, err := a.authorize(r)
	if err != nil {
		return "", err
	}
	timestamp, err := strconv.ParseInt(r.Header.Get("X-BiasedDoom-Timestamp"), 10, 64)
	if err != nil || time.Since(time.Unix(timestamp, 0)) > time.Minute || time.Until(time.Unix(timestamp, 0)) > time.Minute {
		return "", errors.New("invalid request timestamp")
	}
	nonce := r.Header.Get("X-BiasedDoom-Nonce")
	nonceBytes, err := base64.StdEncoding.DecodeString(nonce)
	if err != nil || len(nonceBytes) != 16 {
		return "", errors.New("invalid request nonce")
	}
	signature, err := base64.StdEncoding.DecodeString(r.Header.Get("X-BiasedDoom-Signature"))
	if err != nil || len(signature) != ed25519.SignatureSize {
		return "", errors.New("invalid device signature")
	}
	body, err := io.ReadAll(io.LimitReader(r.Body, 512<<10))
	if err != nil {
		return "", errors.New("could not read request body")
	}
	r.Body.Close()
	r.Body = io.NopCloser(bytes.NewReader(body))
	encodedKey, found, err := a.store.get(r.Context(), "bdonline:device:"+device)
	if err != nil || !found {
		return "", errors.New("unknown device")
	}
	publicKey, err := base64.StdEncoding.DecodeString(string(encodedKey))
	if err != nil || len(publicKey) != ed25519.PublicKeySize || !ed25519.Verify(ed25519.PublicKey(publicKey), []byte(signaturePayload(r.Method, r.URL.EscapedPath(), strconv.FormatInt(timestamp, 10), nonce, body)), signature) {
		return "", errors.New("invalid device signature")
	}
	accepted, err := a.store.putIfAbsent(r.Context(), "bdonline:nonce:"+device+":"+nonce, []byte("1"), 2*time.Minute)
	if err != nil {
		return "", errors.New("nonce store unavailable")
	}
	if !accepted {
		return "", errors.New("replayed device request")
	}
	return device, nil
}

func writeJSON(w http.ResponseWriter, status int, value any) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	_ = json.NewEncoder(w).Encode(value)
}
func decodeJSON(r *http.Request, target any) error {
	defer r.Body.Close()
	decoder := json.NewDecoder(io.LimitReader(r.Body, 512<<10))
	decoder.DisallowUnknownFields()
	if err := decoder.Decode(target); err != nil {
		return err
	}
	if err := decoder.Decode(&struct{}{}); err != io.EOF {
		if err == nil {
			return errors.New("request contains multiple JSON values")
		}
		return err
	}
	return nil
}
func (a *api) limited(w http.ResponseWriter, r *http.Request) bool {
	host, _, err := net.SplitHostPort(r.RemoteAddr)
	if err != nil {
		host = r.RemoteAddr
	}
	if a.limiter.allow(host, time.Now()) {
		return false
	}
	writeJSON(w, http.StatusTooManyRequests, map[string]string{"error": "rate limit exceeded"})
	return true
}

func (a *api) putSession(ctx context.Context, s *session) error {
	data, err := json.Marshal(s)
	if err != nil {
		return err
	}
	return a.store.put(ctx, "bdonline:session:"+s.ID, data, sessionTTL)
}
func (a *api) getSession(ctx context.Context, id string) (*session, bool, error) {
	data, ok, err := a.store.get(ctx, "bdonline:session:"+id)
	if err != nil || !ok {
		return nil, ok, err
	}
	var s session
	if err := json.Unmarshal(data, &s); err != nil {
		return nil, false, err
	}
	return &s, true, nil
}

// relayHostPublicKey reloads the current host's registered public key when a
// session is created, renewed, joined, or migrated. The relay keeps this only
// in memory so it can verify liveness without accepting a player-supplied
// "host" marker in an opaque game packet.
func (a *api) devicePublicKey(ctx context.Context, device string) (ed25519.PublicKey, error) {
	encoded, found, err := a.store.get(ctx, "bdonline:device:"+device)
	if err != nil || !found {
		return nil, errors.New("host device key is unavailable")
	}
	publicKey, err := base64.StdEncoding.DecodeString(string(encoded))
	if err != nil || len(publicKey) != ed25519.PublicKeySize {
		return nil, errors.New("host device key is invalid")
	}
	return ed25519.PublicKey(publicKey), nil
}

func (a *api) relayHostPublicKey(ctx context.Context, device string) (ed25519.PublicKey, error) {
	return a.devicePublicKey(ctx, device)
}

func (a *api) allowRelayHost(ctx context.Context, s *session, expires time.Time) error {
	publicKey, err := a.relayHostPublicKey(ctx, s.HostDevice)
	if err != nil {
		return err
	}
	a.relay.allow(s.RelayToken, expires, s.HostDevice)
	if !a.relay.setHostIdentity(s.RelayToken, s.HostDevice, publicKey) {
		return errors.New("could not configure relay host identity")
	}
	for peer, credential := range s.RelayAuth {
		if !a.relay.setPeerCredential(s.RelayToken, peer, credential, expires) {
			return errors.New("could not configure relay peer credentials")
		}
	}
	return nil
}

// reserveInactiveMembers converts only relay-silent guests to bounded
// reservations. Device identifiers double as relay peer labels, but remain
// private session state: no endpoint or device identity is exposed in the
// public browser. The caller holds sessionMu and persists the returned change.
func (a *api) reserveInactiveMembers(s *session, now time.Time, exceptDevice string) bool {
	changed := s.pruneReconnectReservations(now)
	for device := range s.Members {
		if device == s.HostDevice || device == exceptDevice || a.relay.peerRecentlyActive(s.RelayToken, device, now) {
			continue
		}
		delete(s.Members, device)
		delete(s.RelayAuth, device)
		if s.Reconnecting == nil {
			s.Reconnecting = map[string]int64{}
		}
		s.Reconnecting[device] = now.Add(reconnectReservationTTL).Unix()
		a.relay.revokePeer(s.RelayToken, device)
		changed = true
	}
	return changed
}

func decodeJSONBytes(body []byte, target any) error {
	decoder := json.NewDecoder(bytes.NewReader(body))
	decoder.DisallowUnknownFields()
	if err := decoder.Decode(target); err != nil {
		return err
	}
	if err := decoder.Decode(&struct{}{}); err != io.EOF {
		if err == nil {
			return errors.New("request contains multiple JSON values")
		}
		return err
	}
	return nil
}

// authorizeDeviceRegistration proves possession of the identity key before
// the endpoint can mint or refresh a bearer token for that key. A raw public
// key is public information, so it must never be sufficient to obtain a
// device's token on its own.
func (a *api) authorizeDeviceRegistration(r *http.Request, publicKey ed25519.PublicKey, body []byte) error {
	timestamp, err := strconv.ParseInt(r.Header.Get("X-BiasedDoom-Timestamp"), 10, 64)
	if err != nil || time.Since(time.Unix(timestamp, 0)) > time.Minute || time.Until(time.Unix(timestamp, 0)) > time.Minute {
		return errors.New("invalid request timestamp")
	}
	nonce := r.Header.Get("X-BiasedDoom-Nonce")
	nonceBytes, err := base64.StdEncoding.DecodeString(nonce)
	if err != nil || len(nonceBytes) != 16 {
		return errors.New("invalid request nonce")
	}
	signature, err := base64.StdEncoding.DecodeString(r.Header.Get("X-BiasedDoom-Signature"))
	if err != nil || len(signature) != ed25519.SignatureSize ||
		!ed25519.Verify(publicKey, []byte(signaturePayload(r.Method, r.URL.EscapedPath(), strconv.FormatInt(timestamp, 10), nonce, body)), signature) {
		return errors.New("invalid device registration signature")
	}
	fingerprint := sha256.Sum256(publicKey)
	accepted, err := a.store.putIfAbsent(r.Context(), "bdonline:registration-nonce:"+hex.EncodeToString(fingerprint[:])+":"+nonce, []byte("1"), 2*time.Minute)
	if err != nil {
		return errors.New("nonce store unavailable")
	}
	if !accepted {
		return errors.New("replayed device registration")
	}
	return nil
}

func (a *api) handleDevice(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		writeJSON(w, http.StatusMethodNotAllowed, map[string]string{"error": "method not allowed"})
		return
	}
	defer r.Body.Close()
	body, err := io.ReadAll(io.LimitReader(r.Body, 512<<10))
	if err != nil {
		writeJSON(w, http.StatusBadRequest, map[string]string{"error": "could not read device registration"})
		return
	}
	var request struct {
		PublicKey string `json:"public_key"`
	}
	if err := decodeJSONBytes(body, &request); err != nil {
		writeJSON(w, http.StatusBadRequest, map[string]string{"error": "invalid device public key"})
		return
	}
	publicKey, err := base64.StdEncoding.DecodeString(request.PublicKey)
	if err != nil || len(publicKey) != ed25519.PublicKeySize {
		writeJSON(w, http.StatusBadRequest, map[string]string{"error": "invalid device public key"})
		return
	}
	if err := a.authorizeDeviceRegistration(r, ed25519.PublicKey(publicKey), body); err != nil {
		writeJSON(w, http.StatusUnauthorized, map[string]string{"error": err.Error()})
		return
	}

	a.deviceMu.Lock()
	defer a.deviceMu.Unlock()
	fingerprint := sha256.Sum256(publicKey)
	mappingKey := "bdonline:device-key:" + hex.EncodeToString(fingerprint[:])
	device := ""
	// During a rolling upgrade, an existing client may have a valid bearer
	// token but no public-key index yet. Preserve that identity and backfill the
	// index rather than silently creating a second device record.
	if claimedDevice, claimErr := a.authorize(r); claimErr == nil {
		if storedKey, keyFound, keyErr := a.store.get(r.Context(), "bdonline:device:"+claimedDevice); keyErr != nil {
			writeJSON(w, 503, map[string]string{"error": "device store unavailable"})
			return
		} else if keyFound && hmac.Equal(storedKey, []byte(request.PublicKey)) {
			device = claimedDevice
		}
	}
	if device == "" {
		if existing, found, lookupErr := a.store.get(r.Context(), mappingKey); lookupErr != nil {
			writeJSON(w, 503, map[string]string{"error": "device store unavailable"})
			return
		} else if found && validSessionID(string(existing)) {
			if storedKey, keyFound, keyErr := a.store.get(r.Context(), "bdonline:device:"+string(existing)); keyErr != nil {
				writeJSON(w, 503, map[string]string{"error": "device store unavailable"})
				return
			} else if keyFound && hmac.Equal(storedKey, []byte(request.PublicKey)) {
				device = string(existing)
			}
		}
	}
	newDevice := device == ""
	if newDevice {
		device = randomHex(16)
	}
	if err := a.store.put(r.Context(), "bdonline:device:"+device, []byte(request.PublicKey), 365*24*time.Hour); err != nil ||
		a.store.put(r.Context(), mappingKey, []byte(device), 365*24*time.Hour) != nil {
		writeJSON(w, 503, map[string]string{"error": "device store unavailable"})
		return
	}
	if newDevice && a.metrics != nil {
		a.metrics.deviceRegistrations.Add(1)
	}
	writeJSON(w, 201, map[string]string{"device_id": device, "access_token": a.issueToken(device, 30*24*time.Hour)})
}

func (a *api) handleSessions(w http.ResponseWriter, r *http.Request) {
	if r.Method == http.MethodGet {
		a.listSessions(w, r)
		return
	}
	if r.Method != http.MethodPost {
		writeJSON(w, 405, map[string]string{"error": "method not allowed"})
		return
	}
	device, err := a.authorizeSigned(r)
	if err != nil {
		writeJSON(w, 401, map[string]string{"error": err.Error()})
		return
	}
	var request createSessionRequest
	if err := decodeJSON(r, &request); err != nil || validManifest(request.sessionManifest) != nil ||
		(request.PasswordRequired && !validPasswordProof(request.PasswordProof)) ||
		(!request.PasswordRequired && request.PasswordProof != "") {
		writeJSON(w, 400, map[string]string{"error": "invalid session manifest"})
		return
	}
	s := &session{ID: randomHex(16), HostDevice: device, Manifest: request.sessionManifest, PasswordProof: request.PasswordProof, Epoch: 1, RelayToken: randomHex(16), SessionKey: randomHex(32), CreatedAt: time.Now().Unix(), LobbyOpen: true, Members: map[string]bool{device: true}, Reconnecting: map[string]int64{}, RelayAuth: map[string]string{device: randomHex(16)}}
	for attempt := 0; attempt < 8; attempt++ {
		code := randomCode()
		reserved, reservationErr := a.store.putIfAbsent(r.Context(), "bdonline:join:"+code, []byte(s.ID), sessionTTL)
		if reservationErr != nil {
			writeJSON(w, 503, map[string]string{"error": "session store unavailable"})
			return
		}
		if reserved {
			s.JoinCode = code
			break
		}
	}
	if s.JoinCode == "" {
		writeJSON(w, 503, map[string]string{"error": "could not allocate a join code"})
		return
	}
	if err := a.putSession(r.Context(), s); err != nil || a.store.setAdd(r.Context(), "bdonline:sessions", s.ID) != nil {
		writeJSON(w, 503, map[string]string{"error": "session store unavailable"})
		return
	}
	if err := a.allowRelayHost(r.Context(), s, time.Now().Add(sessionTTL)); err != nil {
		writeJSON(w, 503, map[string]string{"error": "relay host identity is unavailable"})
		return
	}
	if a.metrics != nil {
		a.metrics.sessionsCreated.Add(1)
	}
	writeJSON(w, 201, map[string]any{"session_id": s.ID, "join_code": s.JoinCode, "epoch": s.Epoch, "endpoint": a.relayEndpoint, "relay_token": s.RelayToken, "relay_auth": s.RelayAuth[device], "session_key": s.SessionKey, "local_peer_id": device, "host_peer_id": device})
}

func (a *api) listSessions(w http.ResponseWriter, r *http.Request) {
	// Listing also retires expired reconnect reservations. Serialize it with
	// admission so the displayed availability and the next join decision use
	// the same bounded session state.
	a.sessionMu.Lock()
	defer a.sessionMu.Unlock()
	ids, err := a.store.setMembers(r.Context(), "bdonline:sessions")
	if err != nil {
		writeJSON(w, 503, map[string]string{"error": "session store unavailable"})
		return
	}
	result := make([]publicSession, 0, len(ids))
	for _, id := range ids {
		s, ok, err := a.getSession(r.Context(), id)
		if err != nil {
			continue
		}
		if !ok {
			_ = a.store.setRemove(r.Context(), "bdonline:sessions", id)
			continue
		}
		if a.reserveInactiveMembers(s, time.Now(), "") {
			if err := a.putSession(r.Context(), s); err != nil {
				writeJSON(w, 503, map[string]string{"error": "session store unavailable"})
				return
			}
		}
		if s.Manifest.Visibility != "public" || s.ReportCount >= 3 {
			continue
		}
		result = append(result, publicSession{SessionID: s.ID, JoinCode: s.JoinCode, Title: s.Manifest.Title, Region: s.Manifest.Region, Mode: s.Manifest.Mode, Players: uint32(len(s.Members)), ReservedPlayers: s.reservedPlayerCount(), MaxPlayers: s.Manifest.MaxPlayers, PasswordRequired: s.Manifest.PasswordRequired, Modded: len(s.Manifest.Content.Packages) > 0, AllowJoinInProgress: s.Manifest.AllowJoinInProgress, LobbyOpen: s.LobbyOpen})
	}
	sort.Slice(result, func(i, j int) bool { return result[i].Title < result[j].Title })
	writeJSON(w, 200, map[string]any{"sessions": result})
}

func (a *api) sessionByCode(ctx context.Context, code string) (*session, bool, error) {
	data, found, err := a.store.get(ctx, "bdonline:join:"+code)
	if err != nil || !found || !validSessionID(string(data)) {
		return nil, false, err
	}
	s, found, err := a.getSession(ctx, string(data))
	if err != nil || !found || s.JoinCode != code {
		return nil, false, err
	}
	return s, true, nil
}
func (a *api) handleJoin(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		writeJSON(w, 405, map[string]string{"error": "method not allowed"})
		return
	}
	// authorizeSigned reads the original body for the signature and restores a
	// bounded reader for schema parsing below. Parsing first would consume the
	// signed bytes and make a valid preflight request look tampered with.
	device, err := a.authorizeSigned(r)
	if err != nil {
		writeJSON(w, 401, map[string]string{"error": err.Error()})
		return
	}
	var request joinRequest
	if r.ContentLength > 0 && decodeJSON(r, &request) != nil {
		writeJSON(w, http.StatusBadRequest, map[string]string{"error": "invalid join request"})
		return
	}
	// Join codes are displayed in upper case, but a copied/mobile-entered code
	// should not fail simply because its letters were lower-cased. Normalize
	// before both validation and lookup; ambiguous characters remain rejected
	// by validJoinCode.
	code := strings.ToUpper(strings.TrimPrefix(r.URL.Path, "/v1/join/"))
	if !validJoinCode(code) {
		writeJSON(w, 400, map[string]string{"error": "invalid join code"})
		return
	}
	if a.metrics != nil {
		a.metrics.joinRequests.Add(1)
		if request.Preflight {
			a.metrics.joinPreflights.Add(1)
		}
	}
	// Serialize both admission and review against the host's lobby-close
	// transition. A preflight does not reserve a slot; a closed session may
	// proceed only when its manifest advertises the snapshot protocol.
	a.sessionMu.Lock()
	defer a.sessionMu.Unlock()
	s, ok, err := a.sessionByCode(r.Context(), code)
	if err != nil {
		writeJSON(w, 503, map[string]string{"error": "session store unavailable"})
		return
	}
	if !ok {
		writeJSON(w, 404, map[string]string{"error": "session not found"})
		return
	}
	if s.Manifest.PasswordRequired && (!validPasswordProof(request.PasswordProof) || !hmac.Equal([]byte(s.PasswordProof), []byte(request.PasswordProof))) {
		if a.metrics != nil {
			a.metrics.joinPasswordDenied.Add(1)
		}
		writeJSON(w, http.StatusForbidden, map[string]string{"error": "invalid session password"})
		return
	}
	// A closed lobby is admissible only for a host that explicitly committed to
	// the versioned snapshot protocol. The signed response below carries the
	// state so the client cannot accidentally run the lobby handshake against a
	// live world.
	if !s.LobbyOpen && !s.Manifest.AllowJoinInProgress {
		writeJSON(w, http.StatusConflict, map[string]string{"error": "session is already in progress"})
		return
	}
	relayAuth := ""
	if request.Preflight {
		// Preflight is deliberately not admission. Give this device a short-lived
		// relay-only capability so it can review/transfer content without taking
		// a player slot or receiving a durable in-session credential.
		relayAuth = randomHex(16)
	} else {
		now := time.Now()
		changed := a.reserveInactiveMembers(s, now, "")
		if s.RelayAuth == nil {
			s.RelayAuth = map[string]string{}
			changed = true
		}
		admitted := false
		reconnected := false
		if !s.Members[device] {
			if _, reserved := s.Reconnecting[device]; reserved {
				delete(s.Reconnecting, device)
				reconnected = true
				changed = true
			} else if s.occupiedSlots() >= s.Manifest.MaxPlayers {
				// Persist a just-detected silent departure even though its
				// reconnect hold keeps this new admission full. Otherwise a
				// service restart could resurrect the revoked active route.
				if changed {
					if err := a.putSession(r.Context(), s); err != nil {
						writeJSON(w, 503, map[string]string{"error": "session store unavailable"})
						return
					}
				}
				if a.metrics != nil {
					a.metrics.joinCapacityDenied.Add(1)
				}
				writeJSON(w, http.StatusConflict, map[string]string{"error": "session is full"})
				return
			}
			s.Members[device] = true
			changed = true
			admitted = !reconnected
		}
		if !validSessionID(s.RelayAuth[device]) {
			s.RelayAuth[device] = randomHex(16)
			changed = true
		}
		if changed {
			if err := a.putSession(r.Context(), s); err != nil {
				writeJSON(w, 503, map[string]string{"error": "session store unavailable"})
				return
			}
			if admitted && a.metrics != nil {
				a.metrics.joinAdmitted.Add(1)
			}
			if reconnected && a.metrics != nil {
				a.metrics.joinReconnected.Add(1)
			}
		}
		relayAuth = s.RelayAuth[device]
	}
	if !validSessionKey(s.SessionKey) {
		writeJSON(w, 503, map[string]string{"error": "session transport credentials are unavailable"})
		return
	}
	// Every ticket remains relay-first. A legacy/manual route may still be
	// present in stored metadata, but it must never displace the authenticated
	// relay fallback. Direct acceleration uses a separately issued, identity-
	// bound relay-observed candidate below, rather than trusting that legacy
	// endpoint as an unauthenticated game route.
	endpoint := a.relayEndpoint
	relay := true
	if err := a.allowRelayHost(r.Context(), s, time.Now().Add(sessionTTL)); err != nil {
		writeJSON(w, 503, map[string]string{"error": "relay host identity is unavailable"})
		return
	}
	ticketExpiry := time.Now().Add(joinTicketTTL)
	if request.Preflight && !a.relay.setPeerCredential(s.RelayToken, device, relayAuth, ticketExpiry) {
		writeJSON(w, 503, map[string]string{"error": "relay preflight credential is unavailable"})
		return
	}
	// The candidate is only a NAT-punching hint. It is returned after an actual
	// signed admission (not a review-only preflight), never in listings, and
	// direct packets still need a device-key signature before game routing.
	hostPublicKey := ""
	hostCandidate := ""
	if !request.Preflight {
		key, keyErr := a.devicePublicKey(r.Context(), s.HostDevice)
		if keyErr != nil {
			writeJSON(w, 503, map[string]string{"error": "host identity is unavailable"})
			return
		}
		hostPublicKey = base64.StdEncoding.EncodeToString(key)
		hostCandidate = a.relay.directEndpoint(s.RelayToken, s.HostDevice, time.Now())
		if hostCandidate != "" && a.metrics != nil {
			a.metrics.directCandidates.Add(1)
		}
	}
	writeJSON(w, 200, map[string]any{"session_id": s.ID, "ticket": a.issueToken(s.ID, joinTicketTTL), "expires_at": ticketExpiry.Unix(), "epoch": s.Epoch, "lobby_open": s.LobbyOpen, "uses_relay": relay, "endpoint": endpoint, "relay_token": s.RelayToken, "relay_auth": relayAuth, "session_key": s.SessionKey, "local_peer_id": device, "host_peer_id": s.HostDevice, "host_public_key": hostPublicKey, "host_direct_endpoint": hostCandidate, "manifest": s.Manifest})
}

func (a *api) handleSessionAction(w http.ResponseWriter, r *http.Request) {
	device, err := a.authorizeSigned(r)
	if err != nil {
		writeJSON(w, 401, map[string]string{"error": err.Error()})
		return
	}
	rest := strings.TrimPrefix(r.URL.Path, "/v1/sessions/")
	pieces := strings.Split(rest, "/")
	if len(pieces) != 2 || !validSessionID(pieces[0]) {
		writeJSON(w, 404, map[string]string{"error": "not found"})
		return
	}
	a.sessionMu.Lock()
	defer a.sessionMu.Unlock()
	s, ok, err := a.getSession(r.Context(), pieces[0])
	if err != nil {
		writeJSON(w, 503, map[string]string{"error": "session store unavailable"})
		return
	}
	if !ok {
		writeJSON(w, 404, map[string]string{"error": "session not found"})
		return
	}
	switch pieces[1] {
	case "admission":
		// A relay peer label alone is not a player credential: content review
		// receives a short-lived relay route before it consumes a slot. The
		// current host performs this signed lookup before streaming any world
		// state, so a review-only device cannot turn into an in-game player.
		if r.Method != http.MethodPost || s.HostDevice != device {
			writeJSON(w, http.StatusForbidden, map[string]string{"error": "host authorization required"})
			return
		}
		var request struct {
			PeerID string `json:"peer_id"`
		}
		if err := decodeJSON(r, &request); err != nil || !validSessionID(request.PeerID) {
			writeJSON(w, http.StatusBadRequest, map[string]string{"error": "invalid peer admission query"})
			return
		}
		// Do not leak the anonymous member roster: this is only callable by the
		// authenticated current host. A host peer is never a valid new guest.
		admitted := request.PeerID != s.HostDevice && s.Members[request.PeerID] && validSessionID(s.RelayAuth[request.PeerID])
		response := map[string]any{"admitted": admitted, "public_key": "", "direct_endpoint": ""}
		if admitted {
			publicKey, keyErr := a.devicePublicKey(r.Context(), request.PeerID)
			if keyErr != nil {
				writeJSON(w, http.StatusServiceUnavailable, map[string]string{"error": "peer identity is unavailable"})
				return
			}
			response["public_key"] = base64.StdEncoding.EncodeToString(publicKey)
			response["direct_endpoint"] = a.relay.directEndpoint(s.RelayToken, request.PeerID, time.Now())
		}
		writeJSON(w, http.StatusOK, response)
	case "state":
		if r.Method != http.MethodPost || s.HostDevice != device {
			writeJSON(w, http.StatusForbidden, map[string]string{"error": "host authorization required"})
			return
		}
		var request struct {
			LobbyOpen *bool `json:"lobby_open"`
		}
		if err := decodeJSON(r, &request); err != nil || request.LobbyOpen == nil {
			writeJSON(w, http.StatusBadRequest, map[string]string{"error": "invalid session state"})
			return
		}
		// Reopening would advertise a live world as a lobby. A new session is
		// required once a host has begun play; closing is idempotent for retries.
		if *request.LobbyOpen {
			writeJSON(w, http.StatusConflict, map[string]string{"error": "a closed lobby cannot be reopened"})
			return
		}
		if s.LobbyOpen {
			s.LobbyOpen = false
			if err := a.putSession(r.Context(), s); err != nil {
				writeJSON(w, 503, map[string]string{"error": "session store unavailable"})
				return
			}
		}
		writeJSON(w, 200, map[string]bool{"ok": true, "lobby_open": false})
	case "renew":
		if r.Method != http.MethodPost || s.HostDevice != device {
			writeJSON(w, 403, map[string]string{"error": "host authorization required"})
			return
		}
		if err := a.putSession(r.Context(), s); err != nil || a.store.put(r.Context(), "bdonline:join:"+s.JoinCode, []byte(s.ID), sessionTTL) != nil {
			writeJSON(w, 503, map[string]string{"error": "session store unavailable"})
			return
		}
		if err := a.allowRelayHost(r.Context(), s, time.Now().Add(sessionTTL)); err != nil {
			writeJSON(w, 503, map[string]string{"error": "relay host identity is unavailable"})
			return
		}
		writeJSON(w, 200, map[string]bool{"ok": true})
	case "route":
		if r.Method != http.MethodPost || s.HostDevice != device {
			writeJSON(w, 403, map[string]string{"error": "host authorization required"})
			return
		}
		var request struct {
			Endpoint string `json:"endpoint"`
		}
		if err := decodeJSON(r, &request); err != nil || len(request.Endpoint) > 255 || !validUDPEndpoint(request.Endpoint) {
			writeJSON(w, 400, map[string]string{"error": "invalid route"})
			return
		}
		s.Endpoint = request.Endpoint
		if err := a.putSession(r.Context(), s); err != nil {
			writeJSON(w, 503, map[string]string{"error": "session store unavailable"})
			return
		}
		writeJSON(w, 200, map[string]bool{"ok": true})
	case "leave":
		if r.Method != http.MethodPost {
			writeJSON(w, http.StatusMethodNotAllowed, map[string]string{"error": "method not allowed"})
			return
		}
		// The host is the authority for session lifetime. A host departure must
		// use close instead of turning a live listing into an orphan.
		if s.HostDevice == device {
			writeJSON(w, http.StatusForbidden, map[string]string{"error": "host must close the session"})
			return
		}
		now := time.Now()
		// This authenticated request itself proves that the leaving guest is
		// still present, so do not classify it as relay-silent first.
		changed := a.reserveInactiveMembers(s, now, device)
		reconnectUntil := int64(0)
		if s.Members[device] {
			delete(s.Members, device)
			if s.Reconnecting == nil {
				s.Reconnecting = map[string]int64{}
			}
			reconnectUntil = now.Add(reconnectReservationTTL).Unix()
			s.Reconnecting[device] = reconnectUntil
			changed = true
		}
		if s.RelayAuth != nil {
			if _, exists := s.RelayAuth[device]; exists {
				delete(s.RelayAuth, device)
				changed = true
			}
		}
		if changed {
			if err := a.putSession(r.Context(), s); err != nil {
				writeJSON(w, 503, map[string]string{"error": "session store unavailable"})
				return
			}
		}
		a.relay.revokePeer(s.RelayToken, device)
		response := map[string]any{"ok": true}
		if reconnectUntil != 0 {
			response["reconnect_until"] = reconnectUntil
		}
		writeJSON(w, 200, response)
	case "close":
		if r.Method != http.MethodPost || s.HostDevice != device {
			writeJSON(w, http.StatusForbidden, map[string]string{"error": "host authorization required"})
			return
		}
		if err := a.store.del(r.Context(), "bdonline:session:"+s.ID); err != nil ||
			a.store.del(r.Context(), "bdonline:join:"+s.JoinCode) != nil ||
			a.store.setRemove(r.Context(), "bdonline:sessions", s.ID) != nil {
			writeJSON(w, 503, map[string]string{"error": "session store unavailable"})
			return
		}
		a.relay.revoke(s.RelayToken)
		writeJSON(w, 200, map[string]bool{"ok": true})
	case "migrate":
		if r.Method != http.MethodPost || s.HostDevice != device {
			writeJSON(w, http.StatusForbidden, map[string]string{"error": "host authorization required"})
			return
		}
		var request migrateRequest
		if err := decodeJSON(r, &request); err != nil || len(request.EligiblePeerIDs) == 0 || len(request.EligiblePeerIDs) >= maxPlayers {
			writeJSON(w, http.StatusBadRequest, map[string]string{"error": "invalid migration roster"})
			return
		}
		seen := make(map[string]bool, len(request.EligiblePeerIDs))
		eligible := make([]string, 0, len(request.EligiblePeerIDs))
		for _, peer := range request.EligiblePeerIDs {
			if !validSessionID(peer) || peer == device || seen[peer] || !s.Members[peer] {
				writeJSON(w, http.StatusBadRequest, map[string]string{"error": "invalid migration roster"})
				return
			}
			seen[peer] = true
			eligible = append(eligible, peer)
		}
		// The service, rather than the departing host, makes the stable election
		// from the currently connected eligible set. All parties can reproduce
		// this ordering from the encrypted handoff frame.
		sort.Strings(eligible)
		if s.Epoch == ^uint32(0) {
			writeJSON(w, http.StatusConflict, map[string]string{"error": "session epoch exhausted"})
			return
		}
		s.HostDevice = eligible[0]
		s.Epoch++
		if err := a.putSession(r.Context(), s); err != nil {
			writeJSON(w, 503, map[string]string{"error": "session store unavailable"})
			return
		}
		// A migration refreshes the live relay authorization at the same time
		// that it advances the authenticated packet epoch.
		if err := a.allowRelayHost(r.Context(), s, time.Now().Add(sessionTTL)); err != nil {
			writeJSON(w, 503, map[string]string{"error": "relay host identity is unavailable"})
			return
		}
		if a.metrics != nil {
			a.metrics.hostMigrations.Add(1)
		}
		writeJSON(w, 200, map[string]any{"host_peer_id": s.HostDevice, "epoch": s.Epoch})
	case "recover":
		if r.Method != http.MethodPost || !s.Members[device] {
			writeJSON(w, http.StatusForbidden, map[string]string{"error": "admitted-player authorization required"})
			return
		}
		var request recoverRequest
		if err := decodeJSON(r, &request); err != nil || len(request.EligiblePeerIDs) == 0 || len(request.EligiblePeerIDs) >= maxPlayers {
			writeJSON(w, http.StatusBadRequest, map[string]string{"error": "invalid recovery roster"})
			return
		}
		seen := make(map[string]bool, len(request.EligiblePeerIDs))
		eligible := make([]string, 0, len(request.EligiblePeerIDs))
		for _, peer := range request.EligiblePeerIDs {
			if !validSessionID(peer) || peer == s.HostDevice || seen[peer] || !s.Members[peer] {
				writeJSON(w, http.StatusBadRequest, map[string]string{"error": "invalid recovery roster"})
				return
			}
			seen[peer] = true
			eligible = append(eligible, peer)
		}
		now := time.Now()
		if a.relay.hostRecentlyActive(s.RelayToken, s.HostDevice, now) {
			// A guest may have lost the host's packet stream while the host is
			// alive. Return the authoritative current epoch so it can wait/retry
			// without allowing a spurious election.
			writeJSON(w, 200, map[string]any{"host_peer_id": s.HostDevice, "epoch": s.Epoch, "recovered": false})
			return
		}
		sort.Strings(eligible)
		if s.Epoch == ^uint32(0) {
			writeJSON(w, http.StatusConflict, map[string]string{"error": "session epoch exhausted"})
			return
		}
		s.HostDevice = eligible[0]
		s.Epoch++
		if err := a.putSession(r.Context(), s); err != nil {
			writeJSON(w, 503, map[string]string{"error": "session store unavailable"})
			return
		}
		if err := a.allowRelayHost(r.Context(), s, now.Add(sessionTTL)); err != nil {
			writeJSON(w, 503, map[string]string{"error": "relay host identity is unavailable"})
			return
		}
		if a.metrics != nil {
			a.metrics.hostMigrations.Add(1)
		}
		writeJSON(w, 200, map[string]any{"host_peer_id": s.HostDevice, "epoch": s.Epoch, "recovered": true})
	case "report":
		if r.Method != http.MethodPost {
			writeJSON(w, 405, map[string]string{"error": "method not allowed"})
			return
		}
		var request struct {
			Reason string `json:"reason"`
		}
		if err := decodeJSON(r, &request); err != nil || !validDisplayText(request.Reason, 256, true) {
			writeJSON(w, 400, map[string]string{"error": "invalid report"})
			return
		}
		if s.HostDevice == device {
			writeJSON(w, http.StatusForbidden, map[string]string{"error": "a host cannot report its own session"})
			return
		}
		if s.ReportCount < 3 {
			if s.Reporters == nil {
				s.Reporters = map[string]bool{}
			}
			if !s.Reporters[device] {
				s.Reporters[device] = true
				s.ReportCount++
				if err := a.putSession(r.Context(), s); err != nil {
					writeJSON(w, 503, map[string]string{"error": "session store unavailable"})
					return
				}
				if a.metrics != nil {
					a.metrics.abuseReports.Add(1)
				}
			}
		}
		writeJSON(w, 202, map[string]bool{"accepted": true})
	default:
		writeJSON(w, 404, map[string]string{"error": "not found"})
	}
}

func (a *api) handleHealthz(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		writeJSON(w, http.StatusMethodNotAllowed, map[string]string{"error": "method not allowed"})
		return
	}
	// A small read verifies the directory's required Redis dependency without
	// creating durable health-check state.
	if _, _, err := a.store.get(r.Context(), "bdonline:health"); err != nil {
		writeJSON(w, http.StatusServiceUnavailable, map[string]string{"error": "session store unavailable"})
		return
	}
	writeJSON(w, http.StatusOK, map[string]bool{"ok": true})
}

func (a *api) handleMetrics(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		writeJSON(w, http.StatusMethodNotAllowed, map[string]string{"error": "method not allowed"})
		return
	}
	metrics := a.metrics
	if metrics == nil {
		metrics = &onlineMetrics{}
	}
	w.Header().Set("Content-Type", "text/plain; version=0.0.4; charset=utf-8")
	// Prometheus text exposition: intentionally aggregate counters only.
	for _, metric := range []struct {
		name  string
		help  string
		value uint64
	}{
		{"biaseddoom_online_device_registrations_total", "Anonymous device registrations accepted.", metrics.deviceRegistrations.Load()},
		{"biaseddoom_online_sessions_created_total", "Online sessions created.", metrics.sessionsCreated.Load()},
		{"biaseddoom_online_join_requests_total", "Validated join or content-preflight requests.", metrics.joinRequests.Load()},
		{"biaseddoom_online_join_preflights_total", "Validated content-review preflight requests.", metrics.joinPreflights.Load()},
		{"biaseddoom_online_join_admitted_total", "New guests admitted to a session.", metrics.joinAdmitted.Load()},
		{"biaseddoom_online_join_reconnected_total", "Reserved guests that reconnected before their reservation expired.", metrics.joinReconnected.Load()},
		{"biaseddoom_online_join_password_denied_total", "Join requests rejected for an invalid password proof.", metrics.joinPasswordDenied.Load()},
		{"biaseddoom_online_join_capacity_denied_total", "Join requests rejected because a session was full.", metrics.joinCapacityDenied.Load()},
		{"biaseddoom_online_abuse_reports_total", "Unique public-listing abuse reports accepted.", metrics.abuseReports.Load()},
		{"biaseddoom_online_host_migrations_total", "Service-coordinated graceful host migrations.", metrics.hostMigrations.Load()},
		{"biaseddoom_online_direct_candidates_total", "Private relay-observed direct candidates issued after admission.", metrics.directCandidates.Load()},
		{"biaseddoom_online_relay_packets_in_total", "Packets received by the opaque relay.", metrics.relayPacketsIn.Load()},
		{"biaseddoom_online_relay_packets_out_total", "Packets forwarded by the opaque relay.", metrics.relayPacketsOut.Load()},
		{"biaseddoom_online_relay_packets_denied_total", "Relay packets rejected before forwarding.", metrics.relayPacketsDenied.Load()},
	} {
		fmt.Fprintf(w, "# HELP %s %s\n# TYPE %s counter\n%s %d\n", metric.name, metric.help, metric.name, metric.name, metric.value)
	}
}

// tlsFiles returns an all-or-nothing TLS configuration. A public control
// plane must never silently fall back to plaintext because only one mounted
// secret made it into a deployment. Empty values intentionally preserve the
// loopback-development HTTP mode documented for Docker Compose.
func tlsFiles() (string, string, error) {
	certificate := strings.TrimSpace(os.Getenv("BIASEDDOOM_ONLINE_TLS_CERT_FILE"))
	privateKey := strings.TrimSpace(os.Getenv("BIASEDDOOM_ONLINE_TLS_KEY_FILE"))
	if (certificate == "") != (privateKey == "") {
		return "", "", errors.New("BIASEDDOOM_ONLINE_TLS_CERT_FILE and BIASEDDOOM_ONLINE_TLS_KEY_FILE must be set together")
	}
	return certificate, privateKey, nil
}

// newHTTPServer centralizes the public control-plane limits so a slow body or
// blocked client cannot keep a directory connection open indefinitely after
// it has passed the header timeout. Endpoint-specific size limits still live
// in the request decoders; these deadlines protect the socket itself.
func newHTTPServer(listen string, handler http.Handler) *http.Server {
	return &http.Server{
		Addr:              listen,
		Handler:           handler,
		ReadHeaderTimeout: 5 * time.Second,
		ReadTimeout:       15 * time.Second,
		WriteTimeout:      15 * time.Second,
		IdleTimeout:       60 * time.Second,
	}
}

func main() {
	redisAddress := os.Getenv("BIASEDDOOM_ONLINE_REDIS_ADDR")
	if redisAddress == "" {
		redisAddress = "127.0.0.1:6379"
	}
	secret, err := os.ReadFile(os.Getenv("BIASEDDOOM_ONLINE_SIGNING_SECRET_FILE"))
	if err != nil || len(secret) < 32 {
		log.Fatal("BIASEDDOOM_ONLINE_SIGNING_SECRET_FILE must contain at least 32 bytes")
	}
	relayAddress := os.Getenv("BIASEDDOOM_ONLINE_RELAY_LISTEN")
	if relayAddress == "" {
		relayAddress = ":3478"
	}
	relayEndpoint := os.Getenv("BIASEDDOOM_ONLINE_RELAY_PUBLIC_ENDPOINT")
	if relayEndpoint == "" {
		relayEndpoint = "udp://127.0.0.1:3478"
	}
	if !validUDPEndpoint(relayEndpoint) {
		log.Fatal("BIASEDDOOM_ONLINE_RELAY_PUBLIC_ENDPOINT must be a udp://host:port endpoint")
	}
	metrics := &onlineMetrics{}
	a := &api{store: redisStore{address: redisAddress}, secret: secret, relay: &relayRegistry{endpoints: map[string]map[string]*net.UDPAddr{}, authorizations: map[string]time.Time{}, hostPeers: map[string]string{}, hostPublicKeys: map[string]ed25519.PublicKey{}, hostLastPackets: map[string]time.Time{}, metrics: metrics}, relayEndpoint: relayEndpoint, limiter: limiter{hits: map[string][]time.Time{}}, metrics: metrics}
	relayConn, err := net.ListenUDP("udp", mustUDPAddr(relayAddress))
	if err != nil {
		log.Fatal(err)
	}
	go runRelay(relayConn, a.relay)
	mux := http.NewServeMux()
	mux.HandleFunc("/healthz", a.handleHealthz)
	mux.HandleFunc("/metrics", a.handleMetrics)
	mux.HandleFunc("/v1/devices", func(w http.ResponseWriter, r *http.Request) {
		if !a.limited(w, r) {
			a.handleDevice(w, r)
		}
	})
	mux.HandleFunc("/v1/sessions", func(w http.ResponseWriter, r *http.Request) {
		if !a.limited(w, r) {
			a.handleSessions(w, r)
		}
	})
	mux.HandleFunc("/v1/join/", func(w http.ResponseWriter, r *http.Request) {
		if !a.limited(w, r) {
			a.handleJoin(w, r)
		}
	})
	mux.HandleFunc("/v1/sessions/", func(w http.ResponseWriter, r *http.Request) {
		if !a.limited(w, r) {
			a.handleSessionAction(w, r)
		}
	})
	listen := os.Getenv("BIASEDDOOM_ONLINE_LISTEN")
	if listen == "" {
		listen = ":8080"
	}
	tlsCertificate, tlsPrivateKey, err := tlsFiles()
	if err != nil {
		log.Fatal(err)
	}
	server := newHTTPServer(listen, mux)
	log.Printf("BiasedDoom online service listening on %s; relay %s", listen, relayAddress)
	if tlsCertificate != "" {
		log.Fatal(server.ListenAndServeTLS(tlsCertificate, tlsPrivateKey))
	}
	log.Fatal(server.ListenAndServe())
}

func mustUDPAddr(value string) *net.UDPAddr {
	address, err := net.ResolveUDPAddr("udp", value)
	if err != nil {
		panic(err)
	}
	return address
}
func runRelay(conn *net.UDPConn, registry *relayRegistry) {
	defer conn.Close()
	buffer := make([]byte, 64<<10)
	for {
		size, from, err := conn.ReadFromUDP(buffer)
		if err != nil {
			return
		}
		packet := append([]byte(nil), buffer[:size]...)
		registry.forward(conn, packet, from)
	}
}
