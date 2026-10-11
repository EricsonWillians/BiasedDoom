#!/usr/bin/env bash
# Exercise the actual engine path with two independent local player profiles:
# host a public relay session, make a guest review/approve a host PK3, transfer
# it through the opaque relay, and complete the legacy lobby into gameplay.
set -euo pipefail

usage() {
	cat <<'USAGE'
Usage: tools/test-online-e2e.sh --engine PATH --iwad PATH

The engine must be built with BIASEDDOOM_ENABLE_ONLINE=ON. Docker, curl, zip,
and timeout are required. The script creates only temporary profiles, a tiny
test PK3, and an isolated Redis/directory/relay fixture, then cleans them up.
USAGE
}

engine=""
iwad=""
while [[ $# -gt 0 ]]; do
	case "$1" in
		--engine)
			[[ $# -ge 2 ]] || { usage >&2; exit 2; }
			engine="$2"
			shift 2
			;;
		--iwad)
			[[ $# -ge 2 ]] || { usage >&2; exit 2; }
			iwad="$2"
			shift 2
			;;
		-h|--help)
			usage
			exit 0
			;;
		*)
			usage >&2
			exit 2
			;;
	esac
done

[[ -n "$engine" && -x "$engine" ]] || { printf '%s\n' 'An executable --engine is required.' >&2; exit 2; }
[[ -n "$iwad" && -f "$iwad" ]] || { printf '%s\n' 'A readable --iwad is required.' >&2; exit 2; }
for command in curl docker grep sed timeout zip; do
	command -v "$command" >/dev/null 2>&1 || { printf 'Required command is unavailable: %s\n' "$command" >&2; exit 1; }
done

root_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
service_dir="$root_dir/services/online"
suffix="${RANDOM}${RANDOM}"
network_name="biaseddoom-online-e2e-$suffix"
image_name="biaseddoom-online-e2e:$suffix"
http_port=$((31000 + RANDOM % 1000))
udp_port=$((32000 + RANDOM % 1000))
temp_dir=$(mktemp -d)
secret_file="$temp_dir/online_signing_secret"
host_log="$temp_dir/host.log"
review_log="$temp_dir/review.log"
wrong_password_log="$temp_dir/wrong-password.log"
guest_log="$temp_dir/guest.log"
mod_source="$temp_dir/ONLINE-E2E.txt"
mod_path="$temp_dir/online-e2e.pk3"
redis_id=""
online_id=""
host_pid=""
guest_pid=""

show_diagnostics() {
	printf '%s\n' '--- online E2E host log ---' >&2
	sed -n '1,240p' "$host_log" >&2 2>/dev/null || true
	printf '%s\n' '--- online E2E guest review log ---' >&2
	sed -n '1,240p' "$review_log" >&2 2>/dev/null || true
	printf '%s\n' '--- online E2E wrong-password log ---' >&2
	sed -n '1,160p' "$wrong_password_log" >&2 2>/dev/null || true
	printf '%s\n' '--- online E2E guest log ---' >&2
	sed -n '1,240p' "$guest_log" >&2 2>/dev/null || true
	if [[ -n "$online_id" ]]; then
		printf '%s\n' '--- online E2E service log ---' >&2
		docker logs "$online_id" >&2 2>/dev/null || true
		printf '%s\n' '--- online E2E relay metrics ---' >&2
		curl -fsS "$service_url/metrics" >&2 2>/dev/null || true
	fi
}

cleanup() {
	local status=$?
	if [[ -n "$guest_pid" ]]; then
		kill -TERM "$guest_pid" >/dev/null 2>&1 || true
		wait "$guest_pid" >/dev/null 2>&1 || true
	fi
	if [[ -n "$host_pid" ]]; then
		kill -TERM "$host_pid" >/dev/null 2>&1 || true
		wait "$host_pid" >/dev/null 2>&1 || true
	fi
	if (( status != 0 )); then
		show_diagnostics
	fi
	if [[ -n "$online_id" ]]; then
		docker rm -f "$online_id" >/dev/null 2>&1 || true
	fi
	if [[ -n "$redis_id" ]]; then
		docker rm -f "$redis_id" >/dev/null 2>&1 || true
	fi
	docker network rm "$network_name" >/dev/null 2>&1 || true
	docker image rm "$image_name" >/dev/null 2>&1 || true
	if [[ "${BIASEDDOOM_ONLINE_E2E_KEEP_TEMP:-}" == "1" ]]; then
		printf 'Online E2E temporary files retained at %s\n' "$temp_dir" >&2
	else
		rm -rf "$temp_dir"
	fi
	return "$status"
}
trap cleanup EXIT

head -c 32 /dev/urandom > "$secret_file"
# The production image runs as distroless non-root. This is a random,
# short-lived test secret and must be readable through its bind mount.
chmod 644 "$secret_file"
printf '%s\n' 'This harmless PK3 proves host-to-guest online mod transfer.' > "$mod_source"
zip -q -j "$mod_path" "$mod_source"
mkdir -p "$temp_dir/host-profile" "$temp_dir/guest-profile"

docker network create "$network_name" >/dev/null
redis_id=$(docker run -d --rm --network "$network_name" --network-alias redis \
	redis:7-alpine redis-server --save '' --appendonly no)
docker build -q -t "$image_name" "$service_dir" >/dev/null
online_id=$(docker run -d --network "$network_name" --network-alias online \
	-p "127.0.0.1:${http_port}:8080/tcp" \
	-p "127.0.0.1:${udp_port}:3478/udp" \
	--mount "type=bind,src=$secret_file,dst=/run/secrets/online_signing_secret,readonly" \
	-e BIASEDDOOM_ONLINE_REDIS_ADDR=redis:6379 \
	-e BIASEDDOOM_ONLINE_LISTEN=:8080 \
	-e BIASEDDOOM_ONLINE_RELAY_LISTEN=:3478 \
	-e BIASEDDOOM_ONLINE_RELAY_PUBLIC_ENDPOINT="udp://127.0.0.1:${udp_port}" \
	-e BIASEDDOOM_ONLINE_SIGNING_SECRET_FILE=/run/secrets/online_signing_secret \
	"$image_name")

service_url="http://127.0.0.1:${http_port}"
for attempt in $(seq 1 30); do
	if curl -fsS "$service_url/healthz" | grep -q '"ok":true'; then
		break
	fi
	if [[ "$attempt" == 30 ]]; then
		printf '%s\n' 'Timed out waiting for the online service health endpoint.' >&2
		exit 1
	fi
	sleep 1
done

# A cold full-engine launch must parse the IWAD and the engine PK3 before it
# reaches I_InitNetwork. Give that real startup path enough room on a loaded
# CI runner; the readiness loop below still exits as soon as the host lists.
timeout --foreground --signal=TERM --kill-after=10s 180s "$engine" \
	-headless -nosound -nomusic -noautoload -stdout -iwad "$iwad" -file "$mod_path" \
	-online-host 2 -online-public -online-title 'Online E2E' \
	-password 'online-e2e-password' \
	-online-service "$service_url" -online-data-dir "$temp_dir/host-profile" \
	>"$host_log" 2>&1 &
host_pid=$!

join_code=""
for attempt in $(seq 1 120); do
	listing=$(curl -fsS "$service_url/v1/sessions" 2>/dev/null || true)
	join_code=$(printf '%s' "$listing" | grep -oE '"join_code":"[A-Z0-9]{4}-[A-Z0-9]{4}"' | head -n 1 | cut -d '"' -f 4 || true)
	if [[ -n "$join_code" ]]; then
		break
	fi
	if ! kill -0 "$host_pid" >/dev/null 2>&1; then
		printf '%s\n' 'Online host exited before publishing a join code.' >&2
		exit 1
	fi
	if [[ "$attempt" == 120 ]]; then
		printf '%s\n' 'Timed out waiting for the online host join code.' >&2
		exit 1
	fi
	sleep 1
done

# A password proof is checked before the manifest is returned, so a wrong
# password cannot reveal the host's mod list or issue a content-route ticket.
set +e
timeout --foreground --signal=TERM --kill-after=10s 120s "$engine" \
	-headless -nosound -nomusic -noautoload -stdout -iwad "$iwad" \
	-online-join "$join_code" -online-service "$service_url" \
	-password 'not-the-e2e-password' -online-data-dir "$temp_dir/guest-profile" \
	-errorlog "$wrong_password_log"
wrong_password_status=$?
set -e
if [[ "$wrong_password_status" == 0 ]] || ! grep -aq 'invalid session password' "$wrong_password_log"; then
	printf '%s\n' 'Wrong password did not reject online manifest review.' >&2
	exit 1
fi

# A new mod set must not be silently transferred. The review-only attempt
# prints the identity-bound approval token and intentionally exits before
# player admission.
set +e
timeout --foreground --signal=TERM --kill-after=10s 120s "$engine" \
	-headless -nosound -nomusic -noautoload -stdout -iwad "$iwad" \
	-online-join "$join_code" -online-service "$service_url" \
	-password 'online-e2e-password' \
	-online-data-dir "$temp_dir/guest-profile" \
	-errorlog "$review_log"
review_status=$?
set -e
if [[ "$review_status" == 0 ]]; then
	printf '%s\n' 'Guest joined without approving the host mod plan.' >&2
	exit 1
fi
approval=$(grep -aoE 'Approval token [0-9a-f]{64}:[0-9a-f]{32}' "$review_log" | head -n 1 | sed 's/^Approval token //' || true)
if [[ -z "$approval" ]]; then
	printf '%s\n' 'Guest review did not print an identity-bound mod approval token.' >&2
	exit 1
fi
# The commercial IWAD's declared companion archives and engine-owned visual
# packs are local prerequisites, never host-supplied mods. This fixture has
# exactly one transferable archive: the PK3 created above.
if ! grep -aqE ': 1 package(,|$)' "$review_log" || grep -aqE 'extras\.wad|game_widescreen_gfx\.pk3' "$review_log"; then
	printf '%s\n' 'Online content review incorrectly included a base-game or engine resource.' >&2
	exit 1
fi

timeout --foreground --signal=TERM --kill-after=10s 180s "$engine" \
	-headless -nosound -nomusic -noautoload -stdout -iwad "$iwad" \
	-online-join "$join_code" -online-service "$service_url" \
	-password 'online-e2e-password' \
	-online-accept-mods "$approval" -online-data-dir "$temp_dir/guest-profile" \
	>"$guest_log" 2>&1 &
guest_pid=$!

# A diagnostic-only pause lets a developer inspect the relay's aggregate
# counters while the approved transfer socket is live. It is intentionally
# opt-in and has no effect on CI.
if [[ "${BIASEDDOOM_ONLINE_E2E_PAUSE_AFTER_GUEST:-0}" =~ ^[1-9][0-9]*$ ]]; then
	sleep "$BIASEDDOOM_ONLINE_E2E_PAUSE_AFTER_GUEST"
fi

for attempt in $(seq 1 120); do
	listing=$(curl -fsS "$service_url/v1/sessions" 2>/dev/null || true)
	if [[ "$listing" == *'"lobby_open":false'* ]] && find "$temp_dir/guest-profile/online-content" -type f -name '*.pk3' -print -quit 2>/dev/null | grep -q .; then
		break
	fi
	if ! kill -0 "$host_pid" >/dev/null 2>&1 || ! kill -0 "$guest_pid" >/dev/null 2>&1; then
		printf '%s\n' 'Host or approved guest exited before online gameplay began.' >&2
		exit 1
	fi
	if [[ "$attempt" == 120 ]]; then
		printf '%s\n' 'Timed out waiting for approved mod transfer and lobby start.' >&2
		exit 1
	fi
	sleep 1
done

printf '%s\n' "PASS: online engine E2E rejected the wrong password, reviewed and transferred the host PK3, then started gameplay"
