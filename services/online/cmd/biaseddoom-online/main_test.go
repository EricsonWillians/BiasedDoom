package main

import (
	"context"
	"crypto/ed25519"
	"crypto/rand"
	"encoding/base64"
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"net"
	"net/http"
	"net/http/httptest"
	"os"
	"slices"
	"strconv"
	"strings"
	"sync"
	"testing"
	"time"
)

type memoryValue struct {
	value   []byte
	expires time.Time
}

type memoryStore struct {
	mu     sync.Mutex
	values map[string]memoryValue
	sets   map[string]map[string]bool
}

func newMemoryStore() *memoryStore {
	return &memoryStore{values: map[string]memoryValue{}, sets: map[string]map[string]bool{}}
}

func (m *memoryStore) put(_ context.Context, key string, value []byte, ttl time.Duration) error {
	m.mu.Lock()
	defer m.mu.Unlock()
	m.values[key] = memoryValue{append([]byte(nil), value...), time.Now().Add(ttl)}
	return nil
}

func (m *memoryStore) putIfAbsent(_ context.Context, key string, value []byte, ttl time.Duration) (bool, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	if existing, found := m.values[key]; found && existing.expires.After(time.Now()) {
		return false, nil
	}
	m.values[key] = memoryValue{append([]byte(nil), value...), time.Now().Add(ttl)}
	return true, nil
}

func (m *memoryStore) get(_ context.Context, key string) ([]byte, bool, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	value, found := m.values[key]
	if !found || !value.expires.After(time.Now()) {
		delete(m.values, key)
		return nil, false, nil
	}
	return append([]byte(nil), value.value...), true, nil
}

func (m *memoryStore) del(_ context.Context, key string) error {
	m.mu.Lock()
	defer m.mu.Unlock()
	delete(m.values, key)
	return nil
}

func (m *memoryStore) setAdd(_ context.Context, key, value string) error {
	m.mu.Lock()
	defer m.mu.Unlock()
	if m.sets[key] == nil {
		m.sets[key] = map[string]bool{}
	}
	m.sets[key][value] = true
	return nil
}

func (m *memoryStore) setRemove(_ context.Context, key, value string) error {
	m.mu.Lock()
	defer m.mu.Unlock()
	delete(m.sets[key], value)
	return nil
}

func (m *memoryStore) setMembers(_ context.Context, key string) ([]string, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	values := make([]string, 0, len(m.sets[key]))
	for value := range m.sets[key] {
		values = append(values, value)
	}
	return values, nil
}

func testAPI() *api {
	metrics := &onlineMetrics{}
	return &api{
		store:         newMemoryStore(),
		secret:        []byte("01234567890123456789012345678901"),
		relay:         &relayRegistry{endpoints: map[string]map[string]*net.UDPAddr{}, authorizations: map[string]time.Time{}, hostPeers: map[string]string{}, hostPublicKeys: map[string]ed25519.PublicKey{}, hostLastPackets: map[string]time.Time{}, metrics: metrics},
		relayEndpoint: "udp://relay.test:3478",
		limiter:       limiter{hits: map[string][]time.Time{}},
		metrics:       metrics,
	}
}

func TestHealthAndMetricsExposeOnlyAggregateState(t *testing.T) {
	a := testAPI()
	a.metrics.sessionsCreated.Add(2)
	a.metrics.joinPasswordDenied.Add(3)
	a.metrics.joinReconnected.Add(6)
	a.metrics.directCandidates.Add(7)
	a.metrics.relayPacketsIn.Add(4)
	a.metrics.relayPacketsOut.Add(5)

	health := httptest.NewRecorder()
	a.handleHealthz(health, httptest.NewRequest(http.MethodGet, "/healthz", nil))
	if health.Code != http.StatusOK || !strings.Contains(health.Body.String(), `"ok":true`) {
		t.Fatalf("healthy store did not pass health check: status=%d body=%s", health.Code, health.Body.String())
	}

	metrics := httptest.NewRecorder()
	a.handleMetrics(metrics, httptest.NewRequest(http.MethodGet, "/metrics", nil))
	body := metrics.Body.String()
	if metrics.Code != http.StatusOK || !strings.HasPrefix(metrics.Header().Get("Content-Type"), "text/plain") ||
		!strings.Contains(body, "biaseddoom_online_sessions_created_total 2") ||
		!strings.Contains(body, "biaseddoom_online_join_password_denied_total 3") ||
		!strings.Contains(body, "biaseddoom_online_join_reconnected_total 6") ||
		!strings.Contains(body, "biaseddoom_online_direct_candidates_total 7") ||
		!strings.Contains(body, "biaseddoom_online_relay_packets_in_total 4") ||
		!strings.Contains(body, "biaseddoom_online_relay_packets_out_total 5") {
		t.Fatalf("metrics endpoint did not expose expected aggregate counters: status=%d body=%s", metrics.Code, body)
	}
	for _, sensitive := range []string{"session_id", "join_code", "device_id", "password_proof", "relay_token", "relay_auth", "endpoint", "content_digest", "package_name"} {
		if strings.Contains(body, sensitive) {
			t.Fatalf("metrics leaked sensitive field name %q: %s", sensitive, body)
		}
	}
}

func TestTLSConfigurationRequiresCertificateAndKeyTogether(t *testing.T) {
	t.Setenv("BIASEDDOOM_ONLINE_TLS_CERT_FILE", "")
	t.Setenv("BIASEDDOOM_ONLINE_TLS_KEY_FILE", "")
	certificate, privateKey, err := tlsFiles()
	if err != nil || certificate != "" || privateKey != "" {
		t.Fatalf("empty TLS configuration was not accepted as development HTTP mode: certificate=%q key=%q err=%v", certificate, privateKey, err)
	}

	t.Setenv("BIASEDDOOM_ONLINE_TLS_CERT_FILE", "/run/tls/cert.pem")
	certificate, privateKey, err = tlsFiles()
	if err == nil || certificate != "" || privateKey != "" {
		t.Fatalf("certificate-only TLS configuration was accepted: certificate=%q key=%q err=%v", certificate, privateKey, err)
	}

	t.Setenv("BIASEDDOOM_ONLINE_TLS_KEY_FILE", "/run/tls/key.pem")
	certificate, privateKey, err = tlsFiles()
	if err != nil || certificate != "/run/tls/cert.pem" || privateKey != "/run/tls/key.pem" {
		t.Fatalf("complete TLS configuration was not preserved: certificate=%q key=%q err=%v", certificate, privateKey, err)
	}
}

func TestHTTPServerBoundsConnectionLifetimes(t *testing.T) {
	server := newHTTPServer(":8443", http.NewServeMux())
	if server.Addr != ":8443" || server.ReadHeaderTimeout != 5*time.Second ||
		server.ReadTimeout != 15*time.Second || server.WriteTimeout != 15*time.Second ||
		server.IdleTimeout != 60*time.Second {
		t.Fatalf("online HTTP server deadlines are not bounded as expected: %#v", server)
	}
}

func deviceRegistrationRequest(t *testing.T, private ed25519.PrivateKey) *http.Request {
	t.Helper()
	public := private.Public().(ed25519.PublicKey)
	body := `{"public_key":"` + base64.StdEncoding.EncodeToString(public) + `"}`
	raw := make([]byte, 16)
	if _, err := rand.Read(raw); err != nil {
		t.Fatal(err)
	}
	nonce := base64.StdEncoding.EncodeToString(raw)
	timestamp := strconv.FormatInt(time.Now().Unix(), 10)
	signature := ed25519.Sign(private, []byte(signaturePayload(http.MethodPost, "/v1/devices", timestamp, nonce, []byte(body))))
	req := httptest.NewRequest(http.MethodPost, "/v1/devices", strings.NewReader(body))
	req.Header.Set("X-BiasedDoom-Timestamp", timestamp)
	req.Header.Set("X-BiasedDoom-Nonce", nonce)
	req.Header.Set("X-BiasedDoom-Signature", base64.StdEncoding.EncodeToString(signature))
	return req
}

func registerTestDevice(t *testing.T, a *api) (ed25519.PrivateKey, string) {
	t.Helper()
	_, private, err := ed25519.GenerateKey(rand.Reader)
	if err != nil {
		t.Fatal(err)
	}
	response := httptest.NewRecorder()
	a.handleDevice(response, deviceRegistrationRequest(t, private))
	if response.Code != http.StatusCreated {
		t.Fatalf("registration failed: %s", response.Body.String())
	}
	var payload struct {
		AccessToken string `json:"access_token"`
	}
	if err := json.Unmarshal(response.Body.Bytes(), &payload); err != nil || payload.AccessToken == "" {
		t.Fatalf("invalid registration response: %v", err)
	}
	return private, payload.AccessToken
}

func TestDeviceRegistrationRequiresProofAndRefreshesSameIdentity(t *testing.T) {
	a := testAPI()
	public, private, err := ed25519.GenerateKey(rand.Reader)
	if err != nil {
		t.Fatal(err)
	}
	body := `{"public_key":"` + base64.StdEncoding.EncodeToString(public) + `"}`
	unsigned := httptest.NewRecorder()
	a.handleDevice(unsigned, httptest.NewRequest(http.MethodPost, "/v1/devices", strings.NewReader(body)))
	if unsigned.Code != http.StatusUnauthorized {
		t.Fatalf("unsigned device registration was accepted: %s", unsigned.Body.String())
	}

	first := httptest.NewRecorder()
	a.handleDevice(first, deviceRegistrationRequest(t, private))
	if first.Code != http.StatusCreated {
		t.Fatalf("signed device registration failed: %s", first.Body.String())
	}
	var firstPayload struct {
		DeviceID    string `json:"device_id"`
		AccessToken string `json:"access_token"`
	}
	if err := json.Unmarshal(first.Body.Bytes(), &firstPayload); err != nil || !validSessionID(firstPayload.DeviceID) || firstPayload.AccessToken == "" {
		t.Fatalf("invalid first registration response: %v", err)
	}

	refreshed := httptest.NewRecorder()
	a.handleDevice(refreshed, deviceRegistrationRequest(t, private))
	if refreshed.Code != http.StatusCreated {
		t.Fatalf("signed device refresh failed: %s", refreshed.Body.String())
	}
	var refreshedPayload struct {
		DeviceID    string `json:"device_id"`
		AccessToken string `json:"access_token"`
	}
	if err := json.Unmarshal(refreshed.Body.Bytes(), &refreshedPayload); err != nil || refreshedPayload.DeviceID != firstPayload.DeviceID || refreshedPayload.AccessToken == "" {
		t.Fatalf("refresh did not retain the anonymous device identity: %+v error=%v", refreshedPayload, err)
	}
	if a.metrics.deviceRegistrations.Load() != 1 {
		t.Fatalf("refresh was counted as a second device registration: %d", a.metrics.deviceRegistrations.Load())
	}

	legacyPublic, legacyPrivate, err := ed25519.GenerateKey(rand.Reader)
	if err != nil {
		t.Fatal(err)
	}
	legacyDevice := strings.Repeat("ab", 16)
	if err := a.store.put(context.Background(), "bdonline:device:"+legacyDevice,
		[]byte(base64.StdEncoding.EncodeToString(legacyPublic)), time.Hour); err != nil {
		t.Fatal(err)
	}
	legacyRefresh := deviceRegistrationRequest(t, legacyPrivate)
	legacyRefresh.Header.Set("Authorization", "Bearer "+a.issueToken(legacyDevice, time.Hour))
	legacyResponse := httptest.NewRecorder()
	a.handleDevice(legacyResponse, legacyRefresh)
	var legacyPayload struct {
		DeviceID string `json:"device_id"`
	}
	if legacyResponse.Code != http.StatusCreated || json.Unmarshal(legacyResponse.Body.Bytes(), &legacyPayload) != nil || legacyPayload.DeviceID != legacyDevice {
		t.Fatalf("valid legacy bearer did not preserve the device identity: status=%d body=%s", legacyResponse.Code, legacyResponse.Body.String())
	}
}

// This deliberately runs only when the container-backed integration driver
// supplies an isolated Redis address. Unit tests use memoryStore so normal Go
// development never depends on Docker, while CI still exercises the compact
// RESP client against the Redis version shipped in deployment.
func TestRedisStoreIntegration(t *testing.T) {
	address := os.Getenv("BIASEDDOOM_ONLINE_REDIS_INTEGRATION_ADDR")
	if address == "" {
		t.Skip("set BIASEDDOOM_ONLINE_REDIS_INTEGRATION_ADDR to exercise real Redis")
	}
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	store := redisStore{address: address}
	prefix := "bdonline:integration:" + randomHex(8)
	defer func() {
		_ = store.del(context.Background(), prefix+":value")
		_ = store.del(context.Background(), prefix+":once")
		_ = store.del(context.Background(), prefix+":expires")
		_ = store.del(context.Background(), prefix+":members")
	}()
	if err := store.put(ctx, prefix+":value", []byte("redis round trip\x00bytes"), time.Minute); err != nil {
		t.Fatalf("redis SET failed: %v", err)
	}
	value, found, err := store.get(ctx, prefix+":value")
	if err != nil || !found || string(value) != "redis round trip\x00bytes" {
		t.Fatalf("redis GET did not preserve binary session state: found=%v value=%q err=%v", found, value, err)
	}
	inserted, err := store.putIfAbsent(ctx, prefix+":once", []byte("first"), time.Minute)
	if err != nil || !inserted {
		t.Fatalf("redis SET NX did not reserve a join code: inserted=%v err=%v", inserted, err)
	}
	inserted, err = store.putIfAbsent(ctx, prefix+":once", []byte("second"), time.Minute)
	if err != nil || inserted {
		t.Fatalf("redis SET NX allowed a duplicate join-code reservation: inserted=%v err=%v", inserted, err)
	}
	if err := store.put(ctx, prefix+":expires", []byte("short lease"), 100*time.Millisecond); err != nil {
		t.Fatalf("redis short-lease SET failed: %v", err)
	}
	time.Sleep(250 * time.Millisecond)
	if _, found, err := store.get(ctx, prefix+":expires"); err != nil || found {
		t.Fatalf("redis expiry did not clear a short session lease: found=%v err=%v", found, err)
	}
	if err := store.setAdd(ctx, prefix+":members", "alpha"); err != nil || store.setAdd(ctx, prefix+":members", "bravo") != nil {
		t.Fatalf("redis SADD failed: %v", err)
	}
	members, err := store.setMembers(ctx, prefix+":members")
	if err != nil || len(members) != 2 || !slices.Contains(members, "alpha") || !slices.Contains(members, "bravo") {
		t.Fatalf("redis SMEMBERS did not return the expected session index: %v err=%v", members, err)
	}
	if err := store.setRemove(ctx, prefix+":members", "alpha"); err != nil {
		t.Fatalf("redis SREM failed: %v", err)
	}
	members, err = store.setMembers(ctx, prefix+":members")
	if err != nil || len(members) != 1 || members[0] != "bravo" {
		t.Fatalf("redis SREM did not update the session index: %v err=%v", members, err)
	}
}

func signedTestRequest(t *testing.T, method, path, token string, private ed25519.PrivateKey, body, nonce string) *http.Request {
	t.Helper()
	if nonce == "" {
		raw := make([]byte, 16)
		if _, err := rand.Read(raw); err != nil {
			t.Fatal(err)
		}
		nonce = base64.StdEncoding.EncodeToString(raw)
	}
	timestamp := time.Now().Unix()
	signature := ed25519.Sign(private, []byte(signaturePayload(method, path, strconv.FormatInt(timestamp, 10), nonce, []byte(body))))
	req := httptest.NewRequest(method, path, strings.NewReader(body))
	req.Header.Set("Authorization", "Bearer "+token)
	req.Header.Set("X-BiasedDoom-Timestamp", strconv.FormatInt(timestamp, 10))
	req.Header.Set("X-BiasedDoom-Nonce", nonce)
	req.Header.Set("X-BiasedDoom-Signature", base64.StdEncoding.EncodeToString(signature))
	return req
}

func TestSignedSessionLifecycleAndReplayProtection(t *testing.T) {
	a := testAPI()
	private, token := registerTestDevice(t, a)
	manifest := `{"protocol_version":2,"engine_version":"4.15.19","iwad_digest":"` + strings.Repeat("a", 64) + `","title":"Co-op","region":"global","mode":"co-op","visibility":"public","max_players":2,"password_required":false,"allow_join_in_progress":false,"content":{"version":2,"root_digest":"` + strings.Repeat("b", 64) + `","total_bytes":0,"packages":[]}}`
	request := signedTestRequest(t, http.MethodPost, "/v1/sessions", token, private, manifest, "")
	created := httptest.NewRecorder()
	a.handleSessions(created, request)
	if created.Code != http.StatusCreated {
		t.Fatalf("create failed: %s", created.Body.String())
	}
	var createdPayload struct {
		SessionID   string `json:"session_id"`
		JoinCode    string `json:"join_code"`
		SessionKey  string `json:"session_key"`
		RelayAuth   string `json:"relay_auth"`
		LocalPeerID string `json:"local_peer_id"`
	}
	if err := json.Unmarshal(created.Body.Bytes(), &createdPayload); err != nil || createdPayload.JoinCode == "" || createdPayload.SessionID == "" || !validSessionKey(createdPayload.SessionKey) || !validSessionID(createdPayload.RelayAuth) || !validSessionID(createdPayload.LocalPeerID) {
		t.Fatalf("invalid create response: %v", err)
	}

	listed := httptest.NewRecorder()
	a.handleSessions(listed, httptest.NewRequest(http.MethodGet, "/v1/sessions", nil))
	if listed.Code != http.StatusOK || strings.Contains(listed.Body.String(), "relay_token") || strings.Contains(listed.Body.String(), "endpoint") || strings.Contains(listed.Body.String(), "session_key") || strings.Contains(listed.Body.String(), "peer_id") || !strings.Contains(listed.Body.String(), `"mode":"co-op"`) || !strings.Contains(listed.Body.String(), `"players":1`) {
		t.Fatalf("public list leaked route data: %s", listed.Body.String())
	}

	joinPath := "/v1/join/" + createdPayload.JoinCode
	guestPrivate, guestToken := registerTestDevice(t, a)
	lowercaseCode := httptest.NewRecorder()
	a.handleJoin(lowercaseCode, signedTestRequest(t, http.MethodPost, "/v1/join/"+strings.ToLower(createdPayload.JoinCode), guestToken, guestPrivate, `{"preflight":true}`, ""))
	if lowercaseCode.Code != http.StatusOK {
		t.Fatalf("lower-case join code was not normalized: %s", lowercaseCode.Body.String())
	}
	inspected := httptest.NewRecorder()
	a.handleJoin(inspected, signedTestRequest(t, http.MethodPost, joinPath, guestToken, guestPrivate, `{"preflight":true}`, ""))
	if inspected.Code != http.StatusOK {
		t.Fatalf("preflight manifest review failed: %s", inspected.Body.String())
	}
	listed = httptest.NewRecorder()
	a.handleSessions(listed, httptest.NewRequest(http.MethodGet, "/v1/sessions", nil))
	if listed.Code != http.StatusOK || !strings.Contains(listed.Body.String(), `"players":1`) {
		t.Fatalf("preflight review consumed a player slot: %s", listed.Body.String())
	}
	joined := httptest.NewRecorder()
	a.handleJoin(joined, signedTestRequest(t, http.MethodPost, joinPath, guestToken, guestPrivate, "", ""))
	if joined.Code != http.StatusOK || !strings.Contains(joined.Body.String(), `"uses_relay":true`) || !strings.Contains(joined.Body.String(), "udp://relay.test:3478") || !strings.Contains(joined.Body.String(), `"relay_auth":"`) || !strings.Contains(joined.Body.String(), `"session_key":"`+createdPayload.SessionKey+`"`) {
		t.Fatalf("join ticket failed: %s", joined.Body.String())
	}
	var joinedPayload struct {
		LocalPeerID string `json:"local_peer_id"`
	}
	if err := json.Unmarshal(joined.Body.Bytes(), &joinedPayload); err != nil || !validSessionID(joinedPayload.LocalPeerID) {
		t.Fatalf("join response omitted its admitted peer identity: %v", err)
	}
	admissionPath := "/v1/sessions/" + createdPayload.SessionID + "/admission"
	admitted := httptest.NewRecorder()
	a.handleSessionAction(admitted, signedTestRequest(t, http.MethodPost, admissionPath, token, private,
		`{"peer_id":"`+joinedPayload.LocalPeerID+`"}`, ""))
	if admitted.Code != http.StatusOK || !strings.Contains(admitted.Body.String(), `"admitted":true`) {
		t.Fatalf("host could not verify an admitted peer: %s", admitted.Body.String())
	}
	unknownAdmission := httptest.NewRecorder()
	a.handleSessionAction(unknownAdmission, signedTestRequest(t, http.MethodPost, admissionPath, token, private,
		`{"peer_id":"`+strings.Repeat("c", 32)+`"}`, ""))
	if unknownAdmission.Code != http.StatusOK || !strings.Contains(unknownAdmission.Body.String(), `"admitted":false`) {
		t.Fatalf("host admission query accepted an unknown peer: %s", unknownAdmission.Body.String())
	}
	guestAdmission := httptest.NewRecorder()
	a.handleSessionAction(guestAdmission, signedTestRequest(t, http.MethodPost, admissionPath, guestToken, guestPrivate,
		`{"peer_id":"`+joinedPayload.LocalPeerID+`"}`, ""))
	if guestAdmission.Code != http.StatusForbidden {
		t.Fatalf("non-host was allowed to query session admission: %s", guestAdmission.Body.String())
	}
	listed = httptest.NewRecorder()
	a.handleSessions(listed, httptest.NewRequest(http.MethodGet, "/v1/sessions", nil))
	if listed.Code != http.StatusOK || !strings.Contains(listed.Body.String(), `"players":2`) {
		t.Fatalf("public list did not report the admitted guest: %s", listed.Body.String())
	}
	thirdPrivate, thirdToken := registerTestDevice(t, a)
	full := httptest.NewRecorder()
	a.handleJoin(full, signedTestRequest(t, http.MethodPost, joinPath, thirdToken, thirdPrivate, "", ""))
	if full.Code != http.StatusConflict {
		t.Fatalf("session capacity was not enforced: %s", full.Body.String())
	}
	leavePath := "/v1/sessions/" + createdPayload.SessionID + "/leave"
	left := httptest.NewRecorder()
	a.handleSessionAction(left, signedTestRequest(t, http.MethodPost, leavePath, guestToken, guestPrivate, `{}`, ""))
	if left.Code != http.StatusOK || !strings.Contains(left.Body.String(), `"reconnect_until":`) {
		t.Fatalf("guest leave failed: %s", left.Body.String())
	}
	listed = httptest.NewRecorder()
	a.handleSessions(listed, httptest.NewRequest(http.MethodGet, "/v1/sessions", nil))
	if listed.Code != http.StatusOK || !strings.Contains(listed.Body.String(), `"players":1`) || !strings.Contains(listed.Body.String(), `"reserved_players":1`) {
		t.Fatalf("public list did not expose the departing guest's reconnect reservation: %s", listed.Body.String())
	}
	stillReserved := httptest.NewRecorder()
	a.handleJoin(stillReserved, signedTestRequest(t, http.MethodPost, joinPath, thirdToken, thirdPrivate, "", ""))
	if stillReserved.Code != http.StatusConflict {
		t.Fatalf("reconnect reservation did not protect the guest slot: %s", stillReserved.Body.String())
	}
	rejoined := httptest.NewRecorder()
	a.handleJoin(rejoined, signedTestRequest(t, http.MethodPost, joinPath, guestToken, guestPrivate, "", ""))
	if rejoined.Code != http.StatusOK {
		t.Fatalf("reserved guest could not reconnect: %s", rejoined.Body.String())
	}
	listed = httptest.NewRecorder()
	a.handleSessions(listed, httptest.NewRequest(http.MethodGet, "/v1/sessions", nil))
	if listed.Code != http.StatusOK || !strings.Contains(listed.Body.String(), `"players":2`) || !strings.Contains(listed.Body.String(), `"reserved_players":0`) {
		t.Fatalf("public list did not reactivate the reconnecting guest: %s", listed.Body.String())
	}
	// A guest that vanishes without a clean leave must not consume capacity for
	// the entire session lease. The relay has no access to content or player IPs;
	// its authenticated peer label is enough for the directory to reserve the
	// anonymous device for a bounded reconnect window.
	stored, found, err := a.getSession(context.Background(), createdPayload.SessionID)
	if err != nil || !found {
		t.Fatalf("could not load relay-silence fixture: found=%v error=%v", found, err)
	}
	guestDeviceID := strings.Split(guestToken, ".")[0]
	a.relay.mu.Lock()
	a.relay.peerLastPackets[stored.RelayToken][guestDeviceID] = time.Now().Add(-relayPeerSilenceWindow - time.Second)
	a.relay.mu.Unlock()
	silentCapacity := httptest.NewRecorder()
	a.handleJoin(silentCapacity, signedTestRequest(t, http.MethodPost, joinPath, thirdToken, thirdPrivate, "", ""))
	if silentCapacity.Code != http.StatusConflict {
		t.Fatalf("silent guest reservation did not continue protecting capacity: %s", silentCapacity.Body.String())
	}
	listed = httptest.NewRecorder()
	a.handleSessions(listed, httptest.NewRequest(http.MethodGet, "/v1/sessions", nil))
	if listed.Code != http.StatusOK || !strings.Contains(listed.Body.String(), `"players":1`) || !strings.Contains(listed.Body.String(), `"reserved_players":1`) {
		t.Fatalf("relay-silent guest did not become a reconnect reservation: %s", listed.Body.String())
	}
	rejoined = httptest.NewRecorder()
	a.handleJoin(rejoined, signedTestRequest(t, http.MethodPost, joinPath, guestToken, guestPrivate, "", ""))
	if rejoined.Code != http.StatusOK {
		t.Fatalf("relay-silent guest could not reclaim its reservation: %s", rejoined.Body.String())
	}
	left = httptest.NewRecorder()
	a.handleSessionAction(left, signedTestRequest(t, http.MethodPost, leavePath, guestToken, guestPrivate, `{}`, ""))
	if left.Code != http.StatusOK {
		t.Fatalf("second guest leave failed: %s", left.Body.String())
	}
	stored, found, err = a.getSession(context.Background(), createdPayload.SessionID)
	if err != nil || !found {
		t.Fatalf("could not load reconnect fixture: found=%v error=%v", found, err)
	}
	stored.Reconnecting[strings.Split(guestToken, ".")[0]] = time.Now().Add(-time.Second).Unix()
	if err := a.putSession(context.Background(), stored); err != nil {
		t.Fatalf("could not expire reconnect fixture: %v", err)
	}
	reclaimed := httptest.NewRecorder()
	a.handleJoin(reclaimed, signedTestRequest(t, http.MethodPost, joinPath, thirdToken, thirdPrivate, "", ""))
	if reclaimed.Code != http.StatusOK {
		t.Fatalf("expired reconnect reservation did not release capacity: %s", reclaimed.Body.String())
	}

	badRoute := httptest.NewRecorder()
	badRoutePath := "/v1/sessions/" + createdPayload.SessionID + "/route"
	a.handleSessionAction(badRoute, signedTestRequest(t, http.MethodPost, badRoutePath, token, private, `{"endpoint":"https://not-udp.example"}`, ""))
	if badRoute.Code != http.StatusBadRequest {
		t.Fatalf("invalid direct route was accepted: %s", badRoute.Body.String())
	}

	reportPath := "/v1/sessions/" + createdPayload.SessionID + "/report"
	badReport := httptest.NewRecorder()
	a.handleSessionAction(badReport, signedTestRequest(t, http.MethodPost, reportPath, token, private, `{}`, ""))
	if badReport.Code != http.StatusBadRequest {
		t.Fatalf("empty abuse report was accepted: %s", badReport.Body.String())
	}
	selfReport := httptest.NewRecorder()
	a.handleSessionAction(selfReport, signedTestRequest(t, http.MethodPost, reportPath, token, private, `{"reason":"spam"}`, ""))
	if selfReport.Code != http.StatusForbidden {
		t.Fatalf("host could report its own session: %s", selfReport.Body.String())
	}
	goodReport := httptest.NewRecorder()
	a.handleSessionAction(goodReport, signedTestRequest(t, http.MethodPost, reportPath, thirdToken, thirdPrivate, `{"reason":"spam"}`, ""))
	if goodReport.Code != http.StatusAccepted {
		t.Fatalf("valid abuse report was rejected: %s", goodReport.Body.String())
	}
	duplicateReport := httptest.NewRecorder()
	a.handleSessionAction(duplicateReport, signedTestRequest(t, http.MethodPost, reportPath, thirdToken, thirdPrivate, `{"reason":"spam"}`, ""))
	if duplicateReport.Code != http.StatusAccepted {
		t.Fatalf("duplicate abuse report did not remain idempotent: %s", duplicateReport.Body.String())
	}
	stored, found, err = a.getSession(context.Background(), createdPayload.SessionID)
	if err != nil || !found || stored.ReportCount != 1 || len(stored.Reporters) != 1 {
		t.Fatalf("reporter deduplication did not preserve one aggregate report: found=%v error=%v session=%+v", found, err, stored)
	}
	if a.metrics.abuseReports.Load() != 1 {
		t.Fatalf("one unique abuse report did not produce exactly one aggregate metric increment: %d", a.metrics.abuseReports.Load())
	}

	nonce := base64.StdEncoding.EncodeToString(make([]byte, 16))
	first := httptest.NewRecorder()
	a.handleJoin(first, signedTestRequest(t, http.MethodPost, joinPath, token, private, "", nonce))
	second := httptest.NewRecorder()
	a.handleJoin(second, signedTestRequest(t, http.MethodPost, joinPath, token, private, "", nonce))
	if first.Code != http.StatusOK || second.Code != http.StatusUnauthorized {
		t.Fatalf("nonce replay protection failed: first=%d second=%d", first.Code, second.Code)
	}
	closePath := "/v1/sessions/" + createdPayload.SessionID + "/close"
	closed := httptest.NewRecorder()
	a.handleSessionAction(closed, signedTestRequest(t, http.MethodPost, closePath, token, private, `{}`, ""))
	if closed.Code != http.StatusOK {
		t.Fatalf("host close failed: %s", closed.Body.String())
	}
	listed = httptest.NewRecorder()
	a.handleSessions(listed, httptest.NewRequest(http.MethodGet, "/v1/sessions", nil))
	if listed.Code != http.StatusOK || strings.Contains(listed.Body.String(), createdPayload.SessionID) {
		t.Fatalf("closed session remained public: %s", listed.Body.String())
	}
}

func TestPasswordProtectsManifestBeforePreflightOrAdmission(t *testing.T) {
	a := testAPI()
	hostPrivate, hostToken := registerTestDevice(t, a)
	passwordProof := strings.Repeat("c", 64)
	manifest := `{"protocol_version":2,"engine_version":"4.15.19","iwad_digest":"` + strings.Repeat("a", 64) + `","title":"Locked co-op","region":"global","mode":"co-op","visibility":"public","max_players":2,"password_required":true,"allow_join_in_progress":false,"content":{"version":2,"root_digest":"` + strings.Repeat("b", 64) + `","total_bytes":0,"packages":[]},"password_proof":"` + passwordProof + `"}`
	created := httptest.NewRecorder()
	a.handleSessions(created, signedTestRequest(t, http.MethodPost, "/v1/sessions", hostToken, hostPrivate, manifest, ""))
	if created.Code != http.StatusCreated {
		t.Fatalf("passworded create failed: %s", created.Body.String())
	}
	var createdPayload struct {
		JoinCode string `json:"join_code"`
	}
	if err := json.Unmarshal(created.Body.Bytes(), &createdPayload); err != nil || createdPayload.JoinCode == "" {
		t.Fatalf("invalid passworded create response: %v", err)
	}

	guestPrivate, guestToken := registerTestDevice(t, a)
	joinPath := "/v1/join/" + createdPayload.JoinCode
	for _, body := range []string{"{}", `{"preflight":true,"password_proof":"` + strings.Repeat("d", 64) + `"}`} {
		denied := httptest.NewRecorder()
		a.handleJoin(denied, signedTestRequest(t, http.MethodPost, joinPath, guestToken, guestPrivate, body, ""))
		if denied.Code != http.StatusForbidden || strings.Contains(denied.Body.String(), "manifest") || strings.Contains(denied.Body.String(), passwordProof) {
			t.Fatalf("invalid password disclosed session metadata: status=%d body=%s", denied.Code, denied.Body.String())
		}
	}

	preflight := httptest.NewRecorder()
	a.handleJoin(preflight, signedTestRequest(t, http.MethodPost, joinPath, guestToken, guestPrivate, `{"preflight":true,"password_proof":"`+passwordProof+`"}`, ""))
	if preflight.Code != http.StatusOK || !strings.Contains(preflight.Body.String(), `"manifest"`) || strings.Contains(preflight.Body.String(), passwordProof) {
		t.Fatalf("correct password did not receive a safe manifest review: %s", preflight.Body.String())
	}
	joined := httptest.NewRecorder()
	a.handleJoin(joined, signedTestRequest(t, http.MethodPost, joinPath, guestToken, guestPrivate, `{"password_proof":"`+passwordProof+`"}`, ""))
	if joined.Code != http.StatusOK {
		t.Fatalf("correct password did not admit the guest: %s", joined.Body.String())
	}
}

func TestJoinAlwaysIssuesRelayFallbackWhenLegacyRouteExists(t *testing.T) {
	a := testAPI()
	hostPrivate, hostToken := registerTestDevice(t, a)
	manifest := `{"protocol_version":2,"engine_version":"4.15.19","iwad_digest":"` + strings.Repeat("a", 64) + `","title":"Relay first","region":"global","mode":"co-op","visibility":"private","max_players":2,"password_required":false,"allow_join_in_progress":false,"content":{"version":2,"root_digest":"` + strings.Repeat("b", 64) + `","total_bytes":0,"packages":[]}}`
	created := httptest.NewRecorder()
	a.handleSessions(created, signedTestRequest(t, http.MethodPost, "/v1/sessions", hostToken, hostPrivate, manifest, ""))
	if created.Code != http.StatusCreated {
		t.Fatalf("relay-first session create failed: %s", created.Body.String())
	}
	var sessionInfo struct {
		SessionID string `json:"session_id"`
		JoinCode  string `json:"join_code"`
	}
	if err := json.Unmarshal(created.Body.Bytes(), &sessionInfo); err != nil || sessionInfo.SessionID == "" || sessionInfo.JoinCode == "" {
		t.Fatalf("invalid relay-first create response: %v", err)
	}
	stored, found, err := a.getSession(context.Background(), sessionInfo.SessionID)
	if err != nil || !found {
		t.Fatalf("could not load relay-first fixture: found=%v error=%v", found, err)
	}
	stored.Endpoint = "udp://198.51.100.7:4242"
	if err := a.putSession(context.Background(), stored); err != nil {
		t.Fatalf("could not store legacy direct route fixture: %v", err)
	}

	guestPrivate, guestToken := registerTestDevice(t, a)
	joined := httptest.NewRecorder()
	a.handleJoin(joined, signedTestRequest(t, http.MethodPost, "/v1/join/"+sessionInfo.JoinCode, guestToken, guestPrivate, `{}`, ""))
	if joined.Code != http.StatusOK || !strings.Contains(joined.Body.String(), `"uses_relay":true`) ||
		!strings.Contains(joined.Body.String(), "udp://relay.test:3478") || strings.Contains(joined.Body.String(), stored.Endpoint) {
		t.Fatalf("join ticket did not preserve relay fallback: status=%d body=%s", joined.Code, joined.Body.String())
	}
}

func TestDirectCandidatesRemainPrivateAndIdentityBound(t *testing.T) {
	a := testAPI()
	hostPrivate, hostToken := registerTestDevice(t, a)
	guestPrivate, guestToken := registerTestDevice(t, a)
	manifest := `{"protocol_version":2,"engine_version":"4.15.19","iwad_digest":"` + strings.Repeat("a", 64) + `","title":"Direct candidate","region":"global","mode":"co-op","visibility":"private","max_players":2,"password_required":false,"allow_join_in_progress":false,"content":{"version":2,"root_digest":"` + strings.Repeat("b", 64) + `","total_bytes":0,"packages":[]}}`
	created := httptest.NewRecorder()
	a.handleSessions(created, signedTestRequest(t, http.MethodPost, "/v1/sessions", hostToken, hostPrivate, manifest, ""))
	if created.Code != http.StatusCreated {
		t.Fatalf("host session creation failed: %s", created.Body.String())
	}
	var sessionInfo struct {
		SessionID string `json:"session_id"`
		JoinCode  string `json:"join_code"`
		HostPeer  string `json:"local_peer_id"`
	}
	if err := json.Unmarshal(created.Body.Bytes(), &sessionInfo); err != nil {
		t.Fatal(err)
	}
	s, found, err := a.getSession(context.Background(), sessionInfo.SessionID)
	if err != nil || !found {
		t.Fatalf("could not load created session: found=%v err=%v", found, err)
	}
	// These mappings model an authenticated host/guest sending a normal relay
	// packet from a NAT endpoint. They never appear in a public listing.
	a.relay.mu.Lock()
	if !a.relay.bindEndpointLocked(s.RelayToken, sessionInfo.HostPeer, &net.UDPAddr{IP: net.ParseIP("198.51.100.19"), Port: 40999}) ||
		!a.relay.bindEndpointLocked(s.RelayToken, sessionInfo.HostPeer, &net.UDPAddr{IP: net.ParseIP("198.51.100.20"), Port: 41000}) {
		a.relay.mu.Unlock()
		t.Fatal("could not establish host relay endpoint fixture")
	}
	a.relay.hostLastPackets[s.RelayToken] = time.Now()
	a.relay.mu.Unlock()

	preflight := httptest.NewRecorder()
	a.handleJoin(preflight, signedTestRequest(t, http.MethodPost, "/v1/join/"+sessionInfo.JoinCode, guestToken, guestPrivate, `{"preflight":true}`, ""))
	if preflight.Code != http.StatusOK || strings.Contains(preflight.Body.String(), "198.51.100.20") ||
		!strings.Contains(preflight.Body.String(), `"host_direct_endpoint":""`) {
		t.Fatalf("review-only preflight leaked the host direct candidate: status=%d body=%s", preflight.Code, preflight.Body.String())
	}

	joined := httptest.NewRecorder()
	a.handleJoin(joined, signedTestRequest(t, http.MethodPost, "/v1/join/"+sessionInfo.JoinCode, guestToken, guestPrivate, `{}`, ""))
	if joined.Code != http.StatusOK {
		t.Fatalf("guest join failed: %s", joined.Body.String())
	}
	var ticket struct {
		LocalPeer          string `json:"local_peer_id"`
		HostPublicKey      string `json:"host_public_key"`
		HostDirectEndpoint string `json:"host_direct_endpoint"`
	}
	if err := json.Unmarshal(joined.Body.Bytes(), &ticket); err != nil || ticket.HostPublicKey != base64.StdEncoding.EncodeToString(hostPrivate.Public().(ed25519.PublicKey)) || ticket.HostDirectEndpoint != "udp://198.51.100.20:41000" {
		t.Fatalf("join ticket omitted its authenticated direct candidate: %+v err=%v", ticket, err)
	}
	// Add the guest's relay-observed mapping only after it has been admitted.
	a.relay.mu.Lock()
	if !a.relay.bindEndpointLocked(s.RelayToken, ticket.LocalPeer, &net.UDPAddr{IP: net.ParseIP("198.51.100.21"), Port: 41001}) {
		a.relay.mu.Unlock()
		t.Fatal("could not establish guest relay endpoint fixture")
	}
	if a.relay.peerLastPackets[s.RelayToken] == nil {
		a.relay.peerLastPackets[s.RelayToken] = map[string]time.Time{}
	}
	a.relay.peerLastPackets[s.RelayToken][ticket.LocalPeer] = time.Now()
	a.relay.mu.Unlock()

	admission := httptest.NewRecorder()
	a.handleSessionAction(admission, signedTestRequest(t, http.MethodPost, "/v1/sessions/"+sessionInfo.SessionID+"/admission", hostToken, hostPrivate,
		`{"peer_id":"`+ticket.LocalPeer+`"}`, ""))
	if admission.Code != http.StatusOK || !strings.Contains(admission.Body.String(), `"admitted":true`) ||
		!strings.Contains(admission.Body.String(), base64.StdEncoding.EncodeToString(guestPrivate.Public().(ed25519.PublicKey))) ||
		!strings.Contains(admission.Body.String(), "udp://198.51.100.21:41001") {
		t.Fatalf("host admission lookup omitted the identity-bound direct candidate: status=%d body=%s", admission.Code, admission.Body.String())
	}

	listing := httptest.NewRecorder()
	a.listSessions(listing, httptest.NewRequest(http.MethodGet, "/v1/sessions", nil))
	if listing.Code != http.StatusOK || strings.Contains(listing.Body.String(), "198.51.100.20") || strings.Contains(listing.Body.String(), "198.51.100.21") || strings.Contains(listing.Body.String(), "host_public_key") {
		t.Fatalf("public listing leaked a direct candidate or identity: status=%d body=%s", listing.Code, listing.Body.String())
	}
}

func TestClosedLobbyRejectsPreflightAndAdmission(t *testing.T) {
	a := testAPI()
	hostPrivate, hostToken := registerTestDevice(t, a)
	manifest := `{"protocol_version":2,"engine_version":"4.15.19","iwad_digest":"` + strings.Repeat("a", 64) + `","title":"Lobby boundary","region":"global","mode":"co-op","visibility":"public","max_players":2,"password_required":false,"allow_join_in_progress":false,"content":{"version":2,"root_digest":"` + strings.Repeat("b", 64) + `","total_bytes":0,"packages":[]}}`
	created := httptest.NewRecorder()
	a.handleSessions(created, signedTestRequest(t, http.MethodPost, "/v1/sessions", hostToken, hostPrivate, manifest, ""))
	if created.Code != http.StatusCreated {
		t.Fatalf("lobby-boundary session create failed: %s", created.Body.String())
	}
	var sessionInfo struct {
		SessionID string `json:"session_id"`
		JoinCode  string `json:"join_code"`
	}
	if err := json.Unmarshal(created.Body.Bytes(), &sessionInfo); err != nil || sessionInfo.SessionID == "" || sessionInfo.JoinCode == "" {
		t.Fatalf("invalid lobby-boundary create response: %v", err)
	}

	listed := httptest.NewRecorder()
	a.handleSessions(listed, httptest.NewRequest(http.MethodGet, "/v1/sessions", nil))
	if listed.Code != http.StatusOK || !strings.Contains(listed.Body.String(), `"lobby_open":true`) {
		t.Fatalf("new public session was not advertised as an open lobby: %s", listed.Body.String())
	}

	guestPrivate, guestToken := registerTestDevice(t, a)
	statePath := "/v1/sessions/" + sessionInfo.SessionID + "/state"
	nonHost := httptest.NewRecorder()
	a.handleSessionAction(nonHost, signedTestRequest(t, http.MethodPost, statePath, guestToken, guestPrivate, `{"lobby_open":false}`, ""))
	if nonHost.Code != http.StatusForbidden {
		t.Fatalf("guest could close the host lobby: %s", nonHost.Body.String())
	}
	malformed := httptest.NewRecorder()
	a.handleSessionAction(malformed, signedTestRequest(t, http.MethodPost, statePath, hostToken, hostPrivate, `{}`, ""))
	if malformed.Code != http.StatusBadRequest {
		t.Fatalf("missing lobby state was accepted: %s", malformed.Body.String())
	}

	closed := httptest.NewRecorder()
	a.handleSessionAction(closed, signedTestRequest(t, http.MethodPost, statePath, hostToken, hostPrivate, `{"lobby_open":false}`, ""))
	if closed.Code != http.StatusOK || !strings.Contains(closed.Body.String(), `"lobby_open":false`) {
		t.Fatalf("host could not close lobby admission: %s", closed.Body.String())
	}
	listed = httptest.NewRecorder()
	a.handleSessions(listed, httptest.NewRequest(http.MethodGet, "/v1/sessions", nil))
	if listed.Code != http.StatusOK || !strings.Contains(listed.Body.String(), `"lobby_open":false`) {
		t.Fatalf("public listing did not expose the closed lobby: %s", listed.Body.String())
	}

	joinPath := "/v1/join/" + sessionInfo.JoinCode
	for _, body := range []string{`{"preflight":true}`, `{}`} {
		denied := httptest.NewRecorder()
		a.handleJoin(denied, signedTestRequest(t, http.MethodPost, joinPath, guestToken, guestPrivate, body, ""))
		if denied.Code != http.StatusConflict || strings.Contains(denied.Body.String(), `"manifest"`) {
			t.Fatalf("closed lobby allowed a join path: status=%d body=%s", denied.Code, denied.Body.String())
		}
	}
	stillClosed := httptest.NewRecorder()
	a.handleSessionAction(stillClosed, signedTestRequest(t, http.MethodPost, statePath, hostToken, hostPrivate, `{"lobby_open":false}`, ""))
	if stillClosed.Code != http.StatusOK {
		t.Fatalf("idempotent lobby close failed: %s", stillClosed.Body.String())
	}
	reopen := httptest.NewRecorder()
	a.handleSessionAction(reopen, signedTestRequest(t, http.MethodPost, statePath, hostToken, hostPrivate, `{"lobby_open":true}`, ""))
	if reopen.Code != http.StatusConflict {
		t.Fatalf("closed lobby was allowed to reopen: %s", reopen.Body.String())
	}
}

func TestClosedSnapshotSessionAllowsAuthenticatedAdmission(t *testing.T) {
	a := testAPI()
	hostPrivate, hostToken := registerTestDevice(t, a)
	manifest := `{"protocol_version":2,"engine_version":"4.15.19","iwad_digest":"` + strings.Repeat("a", 64) + `","title":"Snapshot co-op","region":"global","mode":"co-op","visibility":"public","max_players":2,"password_required":false,"allow_join_in_progress":true,"content":{"version":2,"root_digest":"` + strings.Repeat("b", 64) + `","total_bytes":0,"packages":[]}}`
	created := httptest.NewRecorder()
	a.handleSessions(created, signedTestRequest(t, http.MethodPost, "/v1/sessions", hostToken, hostPrivate, manifest, ""))
	if created.Code != http.StatusCreated {
		t.Fatalf("snapshot session create failed: %s", created.Body.String())
	}
	var sessionInfo struct {
		SessionID string `json:"session_id"`
		JoinCode  string `json:"join_code"`
	}
	if err := json.Unmarshal(created.Body.Bytes(), &sessionInfo); err != nil || sessionInfo.SessionID == "" || sessionInfo.JoinCode == "" {
		t.Fatalf("invalid snapshot session create response: %v", err)
	}

	statePath := "/v1/sessions/" + sessionInfo.SessionID + "/state"
	closed := httptest.NewRecorder()
	a.handleSessionAction(closed, signedTestRequest(t, http.MethodPost, statePath, hostToken, hostPrivate, `{"lobby_open":false}`, ""))
	if closed.Code != http.StatusOK {
		t.Fatalf("snapshot session could not enter gameplay: %s", closed.Body.String())
	}

	guestPrivate, guestToken := registerTestDevice(t, a)
	joinPath := "/v1/join/" + sessionInfo.JoinCode
	preflight := httptest.NewRecorder()
	a.handleJoin(preflight, signedTestRequest(t, http.MethodPost, joinPath, guestToken, guestPrivate, `{"preflight":true}`, ""))
	if preflight.Code != http.StatusOK || !strings.Contains(preflight.Body.String(), `"lobby_open":false`) || !strings.Contains(preflight.Body.String(), `"allow_join_in_progress":true`) {
		t.Fatalf("closed snapshot session preflight was not usable: status=%d body=%s", preflight.Code, preflight.Body.String())
	}
	joined := httptest.NewRecorder()
	a.handleJoin(joined, signedTestRequest(t, http.MethodPost, joinPath, guestToken, guestPrivate, `{}`, ""))
	if joined.Code != http.StatusOK || !strings.Contains(joined.Body.String(), `"lobby_open":false`) {
		t.Fatalf("closed snapshot session admission was not usable: status=%d body=%s", joined.Code, joined.Body.String())
	}
}

func TestServiceCoordinatedHostMigrationSelectsDeterministicEligiblePeer(t *testing.T) {
	a := testAPI()
	hostPrivate, hostToken := registerTestDevice(t, a)
	manifest := `{"protocol_version":2,"engine_version":"4.15.19","iwad_digest":"` + strings.Repeat("a", 64) + `","title":"Migration test","region":"global","mode":"co-op","visibility":"private","max_players":3,"password_required":false,"allow_join_in_progress":false,"content":{"version":2,"root_digest":"` + strings.Repeat("b", 64) + `","total_bytes":0,"packages":[]}}`
	created := httptest.NewRecorder()
	a.handleSessions(created, signedTestRequest(t, http.MethodPost, "/v1/sessions", hostToken, hostPrivate, manifest, ""))
	if created.Code != http.StatusCreated {
		t.Fatalf("migration session create failed: %s", created.Body.String())
	}
	var sessionInfo struct {
		SessionID string `json:"session_id"`
		JoinCode  string `json:"join_code"`
	}
	if err := json.Unmarshal(created.Body.Bytes(), &sessionInfo); err != nil || sessionInfo.SessionID == "" || sessionInfo.JoinCode == "" {
		t.Fatalf("invalid migration session response: %v", err)
	}

	firstPrivate, firstToken := registerTestDevice(t, a)
	secondPrivate, secondToken := registerTestDevice(t, a)
	joinPath := "/v1/join/" + sessionInfo.JoinCode
	for _, guest := range []struct {
		private ed25519.PrivateKey
		token   string
	}{{firstPrivate, firstToken}, {secondPrivate, secondToken}} {
		joined := httptest.NewRecorder()
		a.handleJoin(joined, signedTestRequest(t, http.MethodPost, joinPath, guest.token, guest.private, "", ""))
		if joined.Code != http.StatusOK {
			t.Fatalf("migration guest join failed: %s", joined.Body.String())
		}
	}
	firstID := strings.Split(firstToken, ".")[0]
	secondID := strings.Split(secondToken, ".")[0]
	requested := secondID + `","` + firstID
	migrationPath := "/v1/sessions/" + sessionInfo.SessionID + "/migrate"
	migrated := httptest.NewRecorder()
	a.handleSessionAction(migrated, signedTestRequest(t, http.MethodPost, migrationPath, hostToken, hostPrivate,
		`{"eligible_peer_ids":["`+requested+`"]}`, ""))
	if migrated.Code != http.StatusOK {
		t.Fatalf("host migration failed: %s", migrated.Body.String())
	}
	expected := firstID
	if secondID < firstID {
		expected = secondID
	}
	if !strings.Contains(migrated.Body.String(), `"host_peer_id":"`+expected+`"`) || !strings.Contains(migrated.Body.String(), `"epoch":2`) {
		t.Fatalf("migration did not select the deterministic successor: %s", migrated.Body.String())
	}
	if a.metrics.hostMigrations.Load() != 1 {
		t.Fatalf("migration was not recorded in aggregate metrics: %d", a.metrics.hostMigrations.Load())
	}

	oldClose := httptest.NewRecorder()
	a.handleSessionAction(oldClose, signedTestRequest(t, http.MethodPost, "/v1/sessions/"+sessionInfo.SessionID+"/close", hostToken, hostPrivate, `{}`, ""))
	if oldClose.Code != http.StatusForbidden {
		t.Fatalf("departing host retained close authority after migration: %s", oldClose.Body.String())
	}
	newHostPrivate, newHostToken := firstPrivate, firstToken
	if expected == secondID {
		newHostPrivate, newHostToken = secondPrivate, secondToken
	}
	renewed := httptest.NewRecorder()
	a.handleSessionAction(renewed, signedTestRequest(t, http.MethodPost, "/v1/sessions/"+sessionInfo.SessionID+"/renew", newHostToken, newHostPrivate, `{}`, ""))
	if renewed.Code != http.StatusOK {
		t.Fatalf("elected host could not renew migrated session: %s", renewed.Body.String())
	}

	malformed := httptest.NewRecorder()
	a.handleSessionAction(malformed, signedTestRequest(t, http.MethodPost, migrationPath, newHostToken, newHostPrivate,
		`{"eligible_peer_ids":["`+expected+`"]}`, ""))
	if malformed.Code != http.StatusBadRequest {
		t.Fatalf("current host was allowed to migrate to itself: %s", malformed.Body.String())
	}
}

func TestUnexpectedHostRecoveryRequiresRelaySilenceAndIsIdempotent(t *testing.T) {
	a := testAPI()
	hostPrivate, hostToken := registerTestDevice(t, a)
	manifest := `{"protocol_version":2,"engine_version":"4.15.19","iwad_digest":"` + strings.Repeat("a", 64) + `","title":"Recovery test","region":"global","mode":"co-op","visibility":"private","max_players":3,"password_required":false,"allow_join_in_progress":false,"content":{"version":2,"root_digest":"` + strings.Repeat("b", 64) + `","total_bytes":0,"packages":[]}}`
	created := httptest.NewRecorder()
	a.handleSessions(created, signedTestRequest(t, http.MethodPost, "/v1/sessions", hostToken, hostPrivate, manifest, ""))
	if created.Code != http.StatusCreated {
		t.Fatalf("recovery session create failed: %s", created.Body.String())
	}
	var sessionInfo struct {
		SessionID string `json:"session_id"`
		JoinCode  string `json:"join_code"`
	}
	if err := json.Unmarshal(created.Body.Bytes(), &sessionInfo); err != nil {
		t.Fatal(err)
	}
	firstPrivate, firstToken := registerTestDevice(t, a)
	secondPrivate, secondToken := registerTestDevice(t, a)
	joinPath := "/v1/join/" + sessionInfo.JoinCode
	for _, guest := range []struct {
		private ed25519.PrivateKey
		token   string
	}{{firstPrivate, firstToken}, {secondPrivate, secondToken}} {
		joined := httptest.NewRecorder()
		a.handleJoin(joined, signedTestRequest(t, http.MethodPost, joinPath, guest.token, guest.private, "", ""))
		if joined.Code != http.StatusOK {
			t.Fatalf("recovery guest join failed: %s", joined.Body.String())
		}
	}
	firstID := strings.Split(firstToken, ".")[0]
	secondID := strings.Split(secondToken, ".")[0]
	recoveryPath := "/v1/sessions/" + sessionInfo.SessionID + "/recover"
	body := `{"eligible_peer_ids":["` + secondID + `","` + firstID + `"]}`

	// A live host cannot be replaced merely because one guest asks. The relay
	// has recorded the host's creation grace window as recent activity.
	live := httptest.NewRecorder()
	a.handleSessionAction(live, signedTestRequest(t, http.MethodPost, recoveryPath, firstToken, firstPrivate, body, ""))
	if live.Code != http.StatusOK || !strings.Contains(live.Body.String(), `"epoch":1`) || !strings.Contains(live.Body.String(), `"recovered":false`) {
		t.Fatalf("live host recovery response was unsafe: %s", live.Body.String())
	}

	s, found, err := a.getSession(context.Background(), sessionInfo.SessionID)
	if err != nil || !found {
		t.Fatalf("could not inspect recovery fixture: found=%v err=%v", found, err)
	}
	a.relay.mu.Lock()
	a.relay.hostLastPackets[s.RelayToken] = time.Now().Add(-hostSilenceWindow - time.Second)
	a.relay.mu.Unlock()
	recovered := httptest.NewRecorder()
	a.handleSessionAction(recovered, signedTestRequest(t, http.MethodPost, recoveryPath, firstToken, firstPrivate, body, ""))
	expected := firstID
	if secondID < firstID {
		expected = secondID
	}
	if recovered.Code != http.StatusOK || !strings.Contains(recovered.Body.String(), `"host_peer_id":"`+expected+`"`) ||
		!strings.Contains(recovered.Body.String(), `"epoch":2`) || !strings.Contains(recovered.Body.String(), `"recovered":true`) {
		t.Fatalf("silent host was not recovered deterministically: %s", recovered.Body.String())
	}

	// A second guest observes the elected host; it cannot repeatedly advance
	// the epoch while the new host is inside its authenticated grace window.
	observerPrivate, observerToken, observerID := secondPrivate, secondToken, secondID
	if expected == secondID {
		observerPrivate, observerToken, observerID = firstPrivate, firstToken, firstID
	}
	observed := httptest.NewRecorder()
	a.handleSessionAction(observed, signedTestRequest(t, http.MethodPost, recoveryPath, observerToken, observerPrivate,
		`{"eligible_peer_ids":["`+observerID+`"]}`, ""))
	if observed.Code != http.StatusOK || !strings.Contains(observed.Body.String(), `"epoch":2`) || !strings.Contains(observed.Body.String(), `"recovered":false`) {
		t.Fatalf("recovery was not idempotent after election: %s", observed.Body.String())
	}

	outsiderPrivate, outsiderToken := registerTestDevice(t, a)
	denied := httptest.NewRecorder()
	a.handleSessionAction(denied, signedTestRequest(t, http.MethodPost, recoveryPath, outsiderToken, outsiderPrivate, body, ""))
	if denied.Code != http.StatusForbidden {
		t.Fatalf("non-member was allowed to initiate recovery: %s", denied.Body.String())
	}
}

func TestManifestRejectsUnsafeOrOversizedContent(t *testing.T) {
	valid := sessionManifest{
		ProtocolVersion: protocolVersion,
		EngineVersion:   "4.15.19",
		IWADDigest:      strings.Repeat("a", 64),
		Title:           "test",
		Region:          "global",
		Mode:            "co-op",
		Visibility:      "private",
		MaxPlayers:      2,
		Content: contentManifest{
			Version:    protocolVersion,
			RootDigest: strings.Repeat("b", 64),
			Packages:   []contentPackage{{Name: "safe.pk3", Digest: strings.Repeat("c", 64), Extension: ".pk3", Size: 1, LoadOrder: 1}},
			TotalBytes: 1,
		},
	}
	if err := validManifest(valid); err != nil {
		t.Fatalf("valid manifest rejected: %v", err)
	}
	unsupportedProtocol := valid
	unsupportedProtocol.ProtocolVersion++
	if validManifest(unsupportedProtocol) == nil {
		t.Fatal("unsupported protocol version accepted")
	}
	joinInProgress := valid
	joinInProgress.AllowJoinInProgress = true
	if err := validManifest(joinInProgress); err != nil {
		t.Fatalf("snapshot-capable join-in-progress manifest was rejected: %v", err)
	}
	unsafe := valid
	unsafe.Content.Packages[0].Extension = ".PK3"
	if validManifest(unsafe) == nil {
		t.Fatal("unsafe extension accepted")
	}
	reservedLoadOrder := valid
	reservedLoadOrder.Content.Packages[0].LoadOrder = 0
	if validManifest(reservedLoadOrder) == nil {
		t.Fatal("reserved IWAD load order accepted for transferable content")
	}
	overlarge := valid
	overlarge.Content.Packages[0].Size = maxContentFileBytes + 1
	overlarge.Content.TotalBytes = maxContentFileBytes + 1
	if validManifest(overlarge) == nil {
		t.Fatal("oversized content package accepted")
	}
	overlargeArchive := valid
	overlargeArchive.Content.Packages[0].Size = maxArchiveBytes + 1
	overlargeArchive.Content.TotalBytes = maxArchiveBytes + 1
	if validManifest(overlargeArchive) == nil {
		t.Fatal("oversized content archive accepted")
	}
	unsafeMetadata := valid
	unsafeMetadata.Title = "Co-op\nListing"
	if validManifest(unsafeMetadata) == nil {
		t.Fatal("control character in public session title accepted")
	}
	unsafeBundle := valid
	unsafeBundle.Content.Packages[0].DirectoryBundle = true
	unsafeBundle.Content.Packages[0].Extension = ".wad"
	if validManifest(unsafeBundle) == nil {
		t.Fatal("directory bundle with a non-PK3 extension accepted")
	}
}

func TestRelayForwardsOpaqueAuthorizedPackets(t *testing.T) {
	relayConn, err := net.ListenUDP("udp", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1), Port: 0})
	if err != nil {
		t.Fatal(err)
	}
	defer relayConn.Close()
	metrics := &onlineMetrics{}
	registry := &relayRegistry{endpoints: map[string]map[string]*net.UDPAddr{}, authorizations: map[string]time.Time{}, metrics: metrics}
	token := strings.Repeat("ab", 16)
	registry.allow(token, time.Now().Add(time.Minute))
	firstPeer := strings.Repeat("cd", 16)
	secondPeer := strings.Repeat("ef", 16)
	thirdPeer := strings.Repeat("56", 16)
	firstCredential := strings.Repeat("12", 16)
	secondCredential := strings.Repeat("34", 16)
	thirdCredential := strings.Repeat("78", 16)
	if !registry.setPeerCredential(token, firstPeer, firstCredential, time.Now().Add(time.Minute)) ||
		!registry.setPeerCredential(token, secondPeer, secondCredential, time.Now().Add(time.Minute)) ||
		!registry.setPeerCredential(token, thirdPeer, thirdCredential, time.Now().Add(time.Minute)) {
		t.Fatal("could not configure authorized relay peers")
	}
	go runRelay(relayConn, registry)

	first, err := net.ListenUDP("udp", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1), Port: 0})
	if err != nil {
		t.Fatal(err)
	}
	defer first.Close()
	second, err := net.ListenUDP("udp", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1), Port: 0})
	if err != nil {
		t.Fatal(err)
	}
	defer second.Close()
	third, err := net.ListenUDP("udp", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1), Port: 0})
	if err != nil {
		t.Fatal(err)
	}
	defer third.Close()
	packet := func(peer, credential, target, payload string) []byte {
		out := append([]byte("BDOR"), make([]byte, 16)...)
		if _, err := hex.Decode(out[4:20], []byte(token)); err != nil {
			t.Fatal(err)
		}
		out = append(out, []byte("BDN1")...)
		peerBytes, err := hex.DecodeString(peer)
		if err != nil {
			t.Fatal(err)
		}
		credentialBytes, err := hex.DecodeString(credential)
		if err != nil {
			t.Fatal(err)
		}
		targetBytes, err := hex.DecodeString(target)
		if err != nil {
			t.Fatal(err)
		}
		out = append(out, peerBytes...)
		out = append(out, credentialBytes...)
		out = append(out, targetBytes...)
		return append(out, []byte(payload)...)
	}
	if _, err := first.WriteToUDP(packet(firstPeer, firstCredential, secondPeer, "first-register"), relayConn.LocalAddr().(*net.UDPAddr)); err != nil {
		t.Fatal(err)
	}
	if _, err := second.WriteToUDP(packet(secondPeer, secondCredential, firstPeer, "second-register"), relayConn.LocalAddr().(*net.UDPAddr)); err != nil {
		t.Fatal(err)
	}
	if _, err := third.WriteToUDP(packet(thirdPeer, thirdCredential, firstPeer, "third-register"), relayConn.LocalAddr().(*net.UDPAddr)); err != nil {
		t.Fatal(err)
	}
	buffer := make([]byte, 128)
	if err := first.SetReadDeadline(time.Now().Add(time.Second)); err != nil {
		t.Fatal(err)
	}
	size, _, err := first.ReadFromUDP(buffer)
	if err != nil {
		t.Fatal(err)
	}
	if got, want := string(buffer[:size]), string(packet(secondPeer, secondCredential, firstPeer, "second-register")); got != want {
		t.Fatalf("relay changed opaque packet: got %q want %q", got, want)
	}
	if _, err := first.WriteToUDP(packet(firstPeer, firstCredential, secondPeer, "content-chunk"), relayConn.LocalAddr().(*net.UDPAddr)); err != nil {
		t.Fatal(err)
	}
	if err := second.SetReadDeadline(time.Now().Add(time.Second)); err != nil {
		t.Fatal(err)
	}
	size, _, err = second.ReadFromUDP(buffer)
	if err != nil {
		t.Fatal(err)
	}
	if got, want := string(buffer[:size]), string(packet(firstPeer, firstCredential, secondPeer, "content-chunk")); got != want {
		t.Fatalf("relay changed content packet: got %q want %q", got, want)
	}
	if !registry.peerRecentlyActive(token, firstPeer, time.Now()) || !registry.peerRecentlyActive(token, secondPeer, time.Now()) || !registry.peerRecentlyActive(token, thirdPeer, time.Now()) {
		t.Fatal("authenticated relay traffic did not refresh peer liveness")
	}
	if err := third.SetReadDeadline(time.Now().Add(100 * time.Millisecond)); err != nil {
		t.Fatal(err)
	}
	if _, _, err := third.ReadFromUDP(buffer); err == nil {
		t.Fatal("relay fanned a targeted packet out to an unrelated peer")
	}
	registry.mu.Lock()
	registry.peerLastPackets[token][firstPeer] = time.Now().Add(-relayPeerSilenceWindow - time.Second)
	registry.mu.Unlock()
	forged := packet(firstPeer, secondCredential, secondPeer, "forged")
	if _, err := first.WriteToUDP(forged, relayConn.LocalAddr().(*net.UDPAddr)); err != nil {
		t.Fatal(err)
	}
	if err := second.SetReadDeadline(time.Now().Add(100 * time.Millisecond)); err != nil {
		t.Fatal(err)
	}
	if _, _, err := second.ReadFromUDP(buffer); err == nil {
		t.Fatal("relay forwarded a peer label with another peer's credential")
	}
	if registry.peerRecentlyActive(token, firstPeer, time.Now()) {
		t.Fatal("forged relay credential refreshed peer liveness")
	}
	if metrics.relayPacketsDenied.Load() == 0 {
		t.Fatal("forged relay credential was not denied")
	}
}

func TestRelayAcceptsOnlySignedHostLiveness(t *testing.T) {
	relayConn, err := net.ListenUDP("udp", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1), Port: 0})
	if err != nil {
		t.Fatal(err)
	}
	defer relayConn.Close()
	publicKey, privateKey, err := ed25519.GenerateKey(rand.Reader)
	if err != nil {
		t.Fatal(err)
	}
	metrics := &onlineMetrics{}
	registry := &relayRegistry{endpoints: map[string]map[string]*net.UDPAddr{}, authorizations: map[string]time.Time{}, hostPeers: map[string]string{}, hostPublicKeys: map[string]ed25519.PublicKey{}, hostLastPackets: map[string]time.Time{}, metrics: metrics}
	token := strings.Repeat("ab", 16)
	hostPeer := strings.Repeat("cd", 16)
	registry.allow(token, time.Now().Add(time.Minute), hostPeer)
	if !registry.setHostIdentity(token, hostPeer, publicKey) {
		t.Fatal("could not register a test host identity")
	}

	buildHeartbeat := func(key ed25519.PrivateKey) []byte {
		tokenBytes, err := hex.DecodeString(token)
		if err != nil {
			t.Fatal(err)
		}
		peerBytes, err := hex.DecodeString(hostPeer)
		if err != nil {
			t.Fatal(err)
		}
		message := make([]byte, 0, 16+4+16+8)
		message = append(message, tokenBytes...)
		message = append(message, []byte("BDLV")...)
		message = append(message, peerBytes...)
		stamp := make([]byte, 8)
		binary.LittleEndian.PutUint64(stamp, uint64(time.Now().Unix()))
		message = append(message, stamp...)
		packet := append([]byte("BDOR"), message...)
		return append(packet, ed25519.Sign(key, message)...)
	}

	registry.mu.Lock()
	registry.hostLastPackets[token] = time.Now().Add(-hostSilenceWindow - time.Second)
	registry.mu.Unlock()
	from := &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1), Port: 39001}
	registry.forward(relayConn, buildHeartbeat(privateKey), from)
	if !registry.hostRecentlyActive(token, hostPeer, time.Now()) {
		t.Fatal("valid signed liveness heartbeat did not refresh the host")
	}
	registry.mu.RLock()
	_, endpointRegistered := registry.endpoints[token][from.String()]
	registry.mu.RUnlock()
	if !endpointRegistered {
		t.Fatal("valid signed liveness heartbeat did not register the host relay endpoint")
	}

	registry.mu.Lock()
	registry.hostLastPackets[token] = time.Now().Add(-hostSilenceWindow - time.Second)
	registry.mu.Unlock()
	forged := buildHeartbeat(privateKey)
	forged[len(forged)-1] ^= 0x80
	registry.forward(relayConn, forged, from)
	if registry.hostRecentlyActive(token, hostPeer, time.Now()) {
		t.Fatal("forged liveness heartbeat kept the host active")
	}
	if metrics.relayPacketsDenied.Load() == 0 {
		t.Fatal("forged liveness heartbeat was not counted as denied")
	}
}
