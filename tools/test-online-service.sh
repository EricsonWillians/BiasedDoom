#!/usr/bin/env bash
# Run the online directory/relay tests without assuming that a Go toolchain is
# installed on the developer workstation. CI has Go natively; contributors can
# use the same race-enabled suite through Docker when they only need to build
# the engine locally.
set -euo pipefail

root_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
service_dir="$root_dir/services/online"

if ! test -f "$service_dir/go.mod"; then
    echo "Online service source was not found at $service_dir" >&2
    exit 1
fi

if command -v go >/dev/null 2>&1; then
    (
        cd "$service_dir"
        go test -race ./...
    )
    exit 0
fi

if ! command -v docker >/dev/null 2>&1; then
    echo "Go is not installed. Install Go 1.22+ or Docker, then rerun this test." >&2
    exit 1
fi

echo "Go was not found; running the online service tests in golang:1.22."
exec docker run --rm \
    --mount "type=bind,src=$service_dir,dst=/workspace,readonly" \
    --workdir /workspace \
    golang:1.22 \
    go test -race ./...
