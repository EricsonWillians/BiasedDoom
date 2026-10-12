#!/usr/bin/env bash
# Exercise the production Redis RESP store against an isolated Redis 7
# container. The normal service tests remain fast/in-memory; this closes the
# gap between those handlers and the persistence protocol used in deployment.
set -euo pipefail

root_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
service_dir="$root_dir/services/online"

if ! command -v docker >/dev/null 2>&1; then
	printf '%s\n' 'Docker is required for the Redis online-service integration test.' >&2
	exit 1
fi

redis_id=""
network_name=""
cleanup() {
	if [[ -n "$redis_id" ]]; then
		docker rm -f "$redis_id" >/dev/null 2>&1 || true
	fi
	if [[ -n "$network_name" ]]; then
		docker network rm "$network_name" >/dev/null 2>&1 || true
	fi
}
trap cleanup EXIT

wait_for_redis() {
	for _ in $(seq 1 30); do
		if docker exec "$redis_id" redis-cli ping >/dev/null 2>&1; then
			return 0
		fi
		sleep 1
	done
	printf '%s\n' 'Timed out waiting for the Redis integration fixture.' >&2
	return 1
}

if command -v go >/dev/null 2>&1; then
	redis_id=$(docker run -d --rm -p 127.0.0.1:0:6379 redis:7-alpine redis-server --save '' --appendonly no)
	wait_for_redis
	endpoint=$(docker port "$redis_id" 6379/tcp | head -n 1)
	redis_port=${endpoint##*:}
	if [[ -z "$redis_port" || "$redis_port" == "$endpoint" ]]; then
		printf '%s\n' 'Could not determine the Redis integration fixture port.' >&2
		exit 1
	fi
	(
		cd "$service_dir"
		BIASEDDOOM_ONLINE_REDIS_INTEGRATION_ADDR="127.0.0.1:$redis_port" \
			go test -race -run '^TestRedisStoreIntegration$' ./...
	)
	exit 0
fi

# Keep the no-local-Go fallback self-contained too: the test compiler and
# Redis share an ephemeral Docker network instead of relying on host routing.
network_name="biaseddoom-online-redis-${RANDOM}${RANDOM}"
docker network create "$network_name" >/dev/null
redis_id=$(docker run -d --rm --network "$network_name" --network-alias redis redis:7-alpine redis-server --save '' --appendonly no)
wait_for_redis
docker run --rm --network "$network_name" \
	--mount "type=bind,src=$service_dir,dst=/workspace,readonly" \
	--workdir /workspace \
	-e BIASEDDOOM_ONLINE_REDIS_INTEGRATION_ADDR=redis:6379 \
	golang:1.22 \
	go test -race -run '^TestRedisStoreIntegration$' ./...
