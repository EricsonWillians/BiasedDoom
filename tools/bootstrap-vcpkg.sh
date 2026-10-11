#!/usr/bin/env bash
# Bootstrap the pinned vcpkg checkout without making a CI run depend on one
# transient GitHub release-object request. vcpkg normally downloads its tool;
# after bounded retries this script builds the exact tagged vcpkg-tool source.
set -euo pipefail

usage() {
	printf '%s\n' 'Usage: tools/bootstrap-vcpkg.sh [vcpkg-directory]'
}

if [[ $# -gt 1 ]]; then
	usage >&2
	exit 2
fi

root_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
vcpkg_dir="${1:-$root_dir/vcpkg}"
if [[ ! -f "$vcpkg_dir/bootstrap-vcpkg.sh" || ! -f "$vcpkg_dir/scripts/vcpkg-tool-metadata.txt" ]]; then
	printf 'error: %s is not a complete vcpkg checkout\n' "$vcpkg_dir" >&2
	exit 1
fi

force_source="${BIASEDDOOM_VCPKG_BOOTSTRAP_FORCE_SOURCE:-0}"
if [[ "$force_source" != "0" && "$force_source" != "1" ]]; then
	printf 'error: BIASEDDOOM_VCPKG_BOOTSTRAP_FORCE_SOURCE must be 0 or 1\n' >&2
	exit 2
fi

if [[ "$force_source" == "0" ]] && [[ -x "$vcpkg_dir/vcpkg" ]] && "$vcpkg_dir/vcpkg" version --disable-metrics >/dev/null 2>&1; then
	exit 0
fi

attempts="${BIASEDDOOM_VCPKG_BOOTSTRAP_ATTEMPTS:-5}"
if ! [[ "$attempts" =~ ^[1-9][0-9]*$ ]]; then
	printf 'error: BIASEDDOOM_VCPKG_BOOTSTRAP_ATTEMPTS must be a positive integer\n' >&2
	exit 2
fi

if [[ "$force_source" == "0" ]]; then
	for attempt in $(seq 1 "$attempts"); do
		rm -f "$vcpkg_dir/vcpkg"
		if "$vcpkg_dir/bootstrap-vcpkg.sh" -disableMetrics; then
			exit 0
		fi
		if [[ "$attempt" -lt "$attempts" ]]; then
			printf 'vcpkg binary bootstrap attempt %s/%s failed; retrying.\n' "$attempt" "$attempts" >&2
			sleep $((attempt * 5))
		fi
	done
else
	printf 'Forcing vcpkg-tool source bootstrap for verification.\n' >&2
fi

# The tool tag is part of the pinned vcpkg checkout, so source fallback builds
# exactly the tool release it requested rather than silently using a newer
# system vcpkg. Tags deliberately use YYYY-MM-DD, which also prevents an
# unexpected shell argument from escaping the clone command.
tool_tag=$(sed -n 's/^VCPKG_TOOL_RELEASE_TAG=//p' "$vcpkg_dir/scripts/vcpkg-tool-metadata.txt")
if ! [[ "$tool_tag" =~ ^[0-9]{4}-[0-9]{2}-[0-9]{2}$ ]]; then
	printf 'error: vcpkg checkout has an invalid VCPKG_TOOL_RELEASE_TAG\n' >&2
	exit 1
fi

source_dir=$(mktemp -d)
cleanup() {
	rm -rf "$source_dir"
}
trap cleanup EXIT

printf 'vcpkg binary bootstrap failed after %s attempts; building vcpkg-tool %s from source.\n' "$attempts" "$tool_tag" >&2
git clone --depth 1 --branch "$tool_tag" https://github.com/microsoft/vcpkg-tool.git "$source_dir/vcpkg-tool"
generator=()
if command -v ninja >/dev/null 2>&1; then
	generator=(-G Ninja)
fi
cmake -S "$source_dir/vcpkg-tool" -B "$source_dir/build" "${generator[@]}" \
	-DCMAKE_BUILD_TYPE=Release -DVCPKG_DEVELOPMENT_WARNINGS=OFF
cmake --build "$source_dir/build" --parallel 2
install -m 755 "$source_dir/build/vcpkg" "$vcpkg_dir/vcpkg"
"$vcpkg_dir/vcpkg" version --disable-metrics >/dev/null
touch "$vcpkg_dir/vcpkg.disable-metrics"
