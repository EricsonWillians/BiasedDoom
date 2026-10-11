#!/usr/bin/env bash
# Build and smoke the deployable directory+relay image against an isolated
# Redis 7 network. This intentionally checks only public health/metrics: API
# authorization and relay behavior are covered by the race-enabled Go suite.
set -euo pipefail

root_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
service_dir="$root_dir/services/online"

if ! command -v docker >/dev/null 2>&1; then
	printf '%s\n' 'Docker is required for the online service container smoke test.' >&2
	exit 1
fi
if ! command -v openssl >/dev/null 2>&1; then
	printf '%s\n' 'OpenSSL is required for the online service TLS smoke test.' >&2
	exit 1
fi

suffix="${RANDOM}${RANDOM}"
network_name="biaseddoom-online-smoke-$suffix"
image_name="biaseddoom-online-smoke:$suffix"
secret_file=$(mktemp)
tls_dir=$(mktemp -d)
certificate_file="$tls_dir/certificate.pem"
private_key_file="$tls_dir/private-key.pem"
redis_id=""
online_id=""
tls_id=""

cleanup() {
	if [[ -n "$tls_id" ]]; then
		docker rm -f "$tls_id" >/dev/null 2>&1 || true
	fi
	if [[ -n "$online_id" ]]; then
		docker rm -f "$online_id" >/dev/null 2>&1 || true
	fi
	if [[ -n "$redis_id" ]]; then
		docker rm -f "$redis_id" >/dev/null 2>&1 || true
	fi
	docker network rm "$network_name" >/dev/null 2>&1 || true
	rm -f "$secret_file"
	rm -rf "$tls_dir"
}
trap cleanup EXIT

head -c 32 /dev/urandom > "$secret_file"
# The production image deliberately runs as the distroless non-root user. A
# bind-mounted development secret retains host ownership, so it must be
# readable by that container user. This is an isolated, random test secret
# that the cleanup trap removes.
chmod 644 "$secret_file"
openssl req -x509 -newkey rsa:2048 -nodes \
	-keyout "$private_key_file" -out "$certificate_file" -days 1 \
	-subj '/CN=online-tls' >/dev/null 2>&1
# The production image is deliberately non-root. These random, short-lived
# test credentials must therefore be readable through their bind mounts.
chmod 644 "$certificate_file" "$private_key_file"
docker network create "$network_name" >/dev/null
redis_id=$(docker run -d --rm --network "$network_name" --network-alias redis \
	redis:7-alpine redis-server --save '' --appendonly no)
online_id=$(docker build -q -t "$image_name" "$service_dir")
online_id=$(docker run -d --network "$network_name" --network-alias online \
	--mount "type=bind,src=$secret_file,dst=/run/secrets/online_signing_secret,readonly" \
	-e BIASEDDOOM_ONLINE_REDIS_ADDR=redis:6379 \
	-e BIASEDDOOM_ONLINE_LISTEN=:8080 \
	-e BIASEDDOOM_ONLINE_RELAY_LISTEN=:3478 \
	-e BIASEDDOOM_ONLINE_RELAY_PUBLIC_ENDPOINT=udp://online:3478 \
	-e BIASEDDOOM_ONLINE_SIGNING_SECRET_FILE=/run/secrets/online_signing_secret \
	"$image_name")

for attempt in $(seq 1 15); do
	if health=$(docker run --rm --network "$network_name" busybox:1.36 wget -qO- http://online:8080/healthz 2>/dev/null) && [[ "$health" == *'"ok":true'* ]]; then
		break
	fi
	if [[ "$attempt" == 15 ]]; then
		docker logs "$online_id" >&2 || true
		printf '%s\n' 'Timed out waiting for the online directory health endpoint.' >&2
		exit 1
	fi
	sleep 1
done

metrics=$(docker run --rm --network "$network_name" busybox:1.36 wget -qO- http://online:8080/metrics)
if [[ "$metrics" != *'biaseddoom_online_sessions_created_total'* ]] || [[ "$metrics" == *'session_id'* ]] || [[ "$metrics" == *'relay_token'* ]]; then
	printf '%s\n' 'Online directory container exposed invalid metrics.' >&2
	exit 1
fi

# A public client requires HTTPS. Verify the production non-root image can
# consume mounted TLS material and serves the same health signal over TLS.
tls_id=$(docker run -d --network "$network_name" --network-alias online-tls \
	--mount "type=bind,src=$secret_file,dst=/run/secrets/online_signing_secret,readonly" \
	--mount "type=bind,src=$certificate_file,dst=/run/tls/certificate.pem,readonly" \
	--mount "type=bind,src=$private_key_file,dst=/run/tls/private-key.pem,readonly" \
	-e BIASEDDOOM_ONLINE_REDIS_ADDR=redis:6379 \
	-e BIASEDDOOM_ONLINE_LISTEN=:8443 \
	-e BIASEDDOOM_ONLINE_RELAY_LISTEN=:3479 \
	-e BIASEDDOOM_ONLINE_RELAY_PUBLIC_ENDPOINT=udp://online-tls:3479 \
	-e BIASEDDOOM_ONLINE_SIGNING_SECRET_FILE=/run/secrets/online_signing_secret \
	-e BIASEDDOOM_ONLINE_TLS_CERT_FILE=/run/tls/certificate.pem \
	-e BIASEDDOOM_ONLINE_TLS_KEY_FILE=/run/tls/private-key.pem \
	"$image_name")

for attempt in $(seq 1 15); do
	# curl's TLS stack is used here rather than BusyBox wget: the latter's
	# compact HTTPS implementation is not compatible with all Go TLS versions
	# and can turn a healthy service into a false-negative handshake failure.
	if health=$(docker run --rm --network "$network_name" curlimages/curl:8.10.1 \
		-ksSf https://online-tls:8443/healthz 2>/dev/null) && [[ "$health" == *'"ok":true'* ]]; then
		break
	fi
	if [[ "$attempt" == 15 ]]; then
		docker logs "$tls_id" >&2 || true
		printf '%s\n' 'Timed out waiting for the HTTPS online directory health endpoint.' >&2
		exit 1
	fi
	sleep 1
done

printf '%s\n' 'PASS: containerized online directory, relay, Redis health, aggregate metrics, and HTTPS'
