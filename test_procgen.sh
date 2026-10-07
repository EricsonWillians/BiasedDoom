#!/bin/bash
# Headless procedural map generator smoke and structural validation.

set -u

ROOT="$(cd "$(dirname "$0")" && pwd)"
BIN="${BIN:-$ROOT/build/biaseddoom}"

usage() {
    cat <<'USAGE'
Usage: ./test_procgen.sh [--iwad PATH] [--doom1-iwad PATH] [--bin PATH] <mode> [mode arguments]

Runs the procedural-generation structural and runtime checks.

Input selection (highest priority first):
  --iwad PATH / IWAD / BIASEDDOOM_TEST_IWAD
      Doom II IWAD used by the standard test modes.
  --doom1-iwad PATH / DOOM1_IWAD / BIASEDDOOM_TEST_DOOM1_IWAD
      Doom or Ultimate Doom IWAD used by doom1, alignment, features, and music.
  --bin PATH / BIN
      Engine binary (defaults to build/biaseddoom).

When an IWAD is not specified, the script checks its repository root, the
engine binary directory, DOOMWADDIR, and DOOMWADPATH for the conventional
doom2.wad/doom.wad filenames. It never relies on a contributor-specific path.
Global options must precede the mode. Run --help for the available modes.
USAGE
}

iwad_request="${IWAD:-${BIASEDDOOM_TEST_IWAD:-}}"
doom1_iwad_request="${DOOM1_IWAD:-${BIASEDDOOM_TEST_DOOM1_IWAD:-}}"

# Keep the long-standing IWAD= and BIN= entry points, then offer explicit
# command-line forms shared with the newer integration scripts. Parse these
# before the mode so positional seed arguments remain exactly compatible.
while [ "$#" -gt 0 ]; do
    case "$1" in
        --iwad)
            if [ "$#" -lt 2 ]; then
                echo "ERROR: --iwad requires a path" >&2
                usage >&2
                exit 2
            fi
            iwad_request="$2"
            shift 2
            ;;
        --doom1-iwad)
            if [ "$#" -lt 2 ]; then
                echo "ERROR: --doom1-iwad requires a path" >&2
                usage >&2
                exit 2
            fi
            doom1_iwad_request="$2"
            shift 2
            ;;
        --bin)
            if [ "$#" -lt 2 ]; then
                echo "ERROR: --bin requires a path" >&2
                usage >&2
                exit 2
            fi
            BIN="$2"
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        --)
            shift
            break
            ;;
        *)
            break
            ;;
    esac
done

find_iwad() {
    local requested="$1"
    shift
    local -a names=("$@")
    local -a search_dirs=("$ROOT" "$(dirname "$BIN")")
    local -a path_dirs=()
    local directory name

    if [ -n "$requested" ]; then
        [ -f "$requested" ] || return 1
        printf '%s\n' "$requested"
        return 0
    fi

    if [ -n "${DOOMWADDIR:-}" ]; then
        search_dirs+=("$DOOMWADDIR")
    fi
    if [ -n "${DOOMWADPATH:-}" ]; then
        IFS=: read -r -a path_dirs <<<"$DOOMWADPATH"
        for directory in "${path_dirs[@]}"; do
            [ -n "$directory" ] && search_dirs+=("$directory")
        done
    fi

    for directory in "${search_dirs[@]}"; do
        [ -d "$directory" ] || continue
        for name in "${names[@]}"; do
            if [ -f "$directory/$name" ]; then
                printf '%s\n' "$directory/$name"
                return 0
            fi
        done
    done
    return 1
}

if ! IWAD="$(find_iwad "$iwad_request" doom2.wad DOOM2.WAD)"; then
    echo "ERROR: Doom II IWAD not found. Pass --iwad PATH or set IWAD/BIASEDDOOM_TEST_IWAD." >&2
    echo "       Automatic lookup checks the repository, binary directory, DOOMWADDIR, and DOOMWADPATH." >&2
    exit 2
fi
DOOM2_IWAD="$IWAD"

# The default matrix needs only Doom II. Resolve Doom I lazily so the ordinary
# validation commands remain usable on installations that own just Doom II.
DOOM1_IWAD=""
ensure_doom1_iwad() {
    if [ -n "$DOOM1_IWAD" ] && [ -f "$DOOM1_IWAD" ]; then
        return 0
    fi
    if ! DOOM1_IWAD="$(find_iwad "$doom1_iwad_request" doom.wad DOOM.WAD)"; then
        echo "ERROR: Doom or Ultimate Doom IWAD not found. Pass --doom1-iwad PATH or set DOOM1_IWAD/BIASEDDOOM_TEST_DOOM1_IWAD." >&2
        echo "       Automatic lookup checks the repository, binary directory, DOOMWADDIR, and DOOMWADPATH." >&2
        return 1
    fi
    return 0
}

if [ ! -x "$BIN" ]; then
    echo "ERROR: binary not found at $BIN" >&2
    exit 2
fi

# Keep this non-interactive suite independent of a developer's live settings
# and writable home directory. A shared private config is safe because this
# script intentionally serializes its engine processes and it makes every
# invocation start from the same deterministic defaults.
TEST_CONFIG=$(mktemp "${TMPDIR:-/tmp}/procmap_config.XXXXXX") || {
    echo "ERROR: could not create an isolated test config" >&2
    exit 1
}
rm -f "$TEST_CONFIG"
trap 'rm -f "$TEST_CONFIG"' EXIT

run_dump_export() {
    local output_path=$1
    local success_pattern=$2
    shift 2
    local timeout_seconds=$1
    shift
    local log pid completed=0

    # The console dump commands finish writing their artifact before printing
    # their success line, but some audio/device backends can keep the process
    # alive after `+quit`. End that known post-export tail immediately rather
    # than making a sequential corpus wait for its full watchdog timeout.
    log=$(mktemp /tmp/procmap_export.XXXXXX) || return 1
    rm -f "$output_path"
    SDL_AUDIODRIVER=dummy ALSOFT_DRIVERS=null setsid stdbuf -oL -eL "$BIN" \
        -config "$TEST_CONFIG" -headless -nosound -nomusic -nogui -iwad "$IWAD" \
        "$@" >"$log" 2>&1 &
    pid=$!
    for ((tick = 0; tick < timeout_seconds * 10; ++tick)); do
        if [ -s "$output_path" ] && grep -q "$success_pattern" "$log"; then
            completed=1
            break
        fi
        if ! kill -0 "$pid" 2>/dev/null; then
            break
        fi
        sleep 0.1
    done
    if kill -0 "$pid" 2>/dev/null; then
        kill -KILL -- "-$pid" 2>/dev/null || true
    fi
    wait "$pid" 2>/dev/null || true
    cat "$log"
    rm -f "$log"
    [ "$completed" -eq 1 ]
}

run_test() {
    local seed=$1
    local theme=${2:-techbase}
	local difficulty=${3:-3}
	local size=${4:-3}
	local layout=${5:-1}
	local verticality=${6:-1}
	local detail=${7:-1}
	local outdoors=${8:-1}
	local output_path=${9:-/tmp/procmap_test.udmf}
	local timeout_seconds=$((20 + size * 2))
	run_dump_export "$output_path" 'Dumped UDMF to ' "$timeout_seconds" \
		+dumpprocudmf "$seed" "$theme" "$difficulty" "$size" \
		"$layout" "$verticality" "$detail" "$outdoors" "$output_path" +quit
}

# Keep the manifest command separate from the UDMF dumper. Replayability
# checks intentionally compare both artifacts from two fresh engine processes,
# so an accidental dependence on process state cannot pass as determinism.
run_manifest() {
    local seed=$1
    local theme=${2:-techbase}
	local difficulty=${3:-3}
	local size=${4:-3}
	local layout=${5:-1}
	local verticality=${6:-1}
	local detail=${7:-1}
	local outdoors=${8:-1}
	local output_path=${9:-/tmp/procmap_manifest.json}
	local timeout_seconds=$((20 + size * 2))
	run_dump_export "$output_path" 'Dumped procedural run manifest to ' "$timeout_seconds" \
		+dumpprocmanifest "$seed" "$theme" "$difficulty" "$size" \
		"$layout" "$verticality" "$detail" "$outdoors" "$output_path" +quit
}

# The generator is a singleton in the running engine. Fresh-process comparisons
# catch hash/RNG drift, while this helper catches accidental mutation of cached
# recipe planning after the first call (for example a vertical-beat fallback).
run_manifest_pair_same_process() {
	local seed=$1
	local theme=$2
	local difficulty=$3
	local size=$4
	local layout=$5
	local verticality=$6
	local detail=$7
	local outdoors=$8
	local first_path=$9
	local second_path=${10}
	local timeout_seconds=$((20 + size * 2))
	local log pid completed=0

	rm -f "$first_path" "$second_path"
	log=$(mktemp /tmp/procmap_same_process.XXXXXX) || return 1
	SDL_AUDIODRIVER=dummy ALSOFT_DRIVERS=null setsid stdbuf -oL -eL "$BIN" \
		-config "$TEST_CONFIG" -headless -nosound -nomusic -nogui -iwad "$IWAD" \
		+dumpprocmanifest "$seed" "$theme" "$difficulty" "$size" \
		"$layout" "$verticality" "$detail" "$outdoors" "$first_path" \
		+dumpprocmanifest "$seed" "$theme" "$difficulty" "$size" \
		"$layout" "$verticality" "$detail" "$outdoors" "$second_path" +quit \
		>"$log" 2>&1 &
	pid=$!
	for ((tick = 0; tick < timeout_seconds * 10; ++tick)); do
		if [ -s "$first_path" ] && [ -s "$second_path" ] &&
				[ "$(grep -c 'Dumped procedural run manifest to ' "$log" || true)" -ge 2 ]; then
			completed=1
			break
		fi
		if ! kill -0 "$pid" 2>/dev/null; then break; fi
		sleep 0.1
	done
	if kill -0 "$pid" 2>/dev/null; then
		kill -KILL -- "-$pid" 2>/dev/null || true
	fi
	wait "$pid" 2>/dev/null || true
	cat "$log"
	rm -f "$log"
	[ "$completed" -eq 1 ]
}

run_runtime_load() {
    local seed=$1
    local theme=$2
    local difficulty=$3
    local size=$4
    local iwad=$5
    local log=$6
	local developer_level=${7:-0}
	local layout=${8:-1}
	local verticality=${9:-1}
	local detail=${10:-1}
	local outdoors=${11:-1}
    local max_wait=$((20 + size * 2))
    local pid reached=0

    rm -f "$log"
    SDL_AUDIODRIVER=dummy ALSOFT_DRIVERS=null setsid stdbuf -oL -eL "$BIN" \
        -config "$TEST_CONFIG" -headless -nosound -nomusic -nogui -iwad "$iwad" \
        +developer "$developer_level" \
        +procgen_seed "$seed" +procgen_theme "$theme" \
		+procgen_difficulty "$difficulty" +procgen_size "$size" \
		+procgen_layout "$layout" +procgen_verticality "$verticality" \
		+procgen_detail "$detail" +procgen_outdoors "$outdoors" \
		+map PROCMAP >"$log" 2>&1 &
    pid=$!
    for ((second = 0; second < max_wait; second++)); do
        if grep -q '^PROCMAP - ' "$log" 2>/dev/null; then
            reached=1
            # Let texture lookup and initial level setup finish logging before
            # stopping the otherwise interactive engine process.
            sleep 1
            break
        fi
        if ! kill -0 "$pid" 2>/dev/null; then break; fi
        sleep 1
    done
    if kill -0 "$pid" 2>/dev/null; then
        kill -KILL -- "-$pid" 2>/dev/null || true
    fi
    wait "$pid" 2>/dev/null || true

    if [ "$reached" -ne 1 ]; then
        return 1
    fi
    ! grep -Eqi 'procedural map generation failed|invalid map|nodebuilder.*failed|unconnected|missing texture|unknown texture|unclosed loop|adding dummy subsector' "$log"
}

capture_proc_music() {
    local seed=$1
    local iwad=$2
    local log=$3
    local pid reached=0

    rm -f "$log"
    SDL_AUDIODRIVER=dummy ALSOFT_DRIVERS=null setsid stdbuf -oL -eL "$BIN" \
        -config "$TEST_CONFIG" -headless -nosound -nomusic -nogui -iwad "$iwad" \
        +developer 3 +procgen_seed "$seed" +map PROCMAP >"$log" 2>&1 &
    pid=$!
    for _ in $(seq 1 20); do
		if grep -q 'Procedural soundtrack active: ' "$log" 2>/dev/null; then
            reached=1
            break
        fi
        if ! kill -0 "$pid" 2>/dev/null; then break; fi
        sleep 1
    done
    if kill -0 "$pid" 2>/dev/null; then
        kill -KILL -- "-$pid" 2>/dev/null || true
    fi
    wait "$pid" 2>/dev/null || true
    [ "$reached" -eq 1 ]
}

run_software_midi_smoke() {
    local seed=$1
    local iwad=$2
    local log=$3
    local pid reached=0

    rm -f "$log"
    SDL_AUDIODRIVER=dummy ALSOFT_DRIVERS=null setsid stdbuf -oL -eL "$BIN" \
        -config "$TEST_CONFIG" -headless -nogui -noautoload -iwad "$iwad" \
        +developer 3 +snd_mididevice -5 +snd_musicvolume 1 +mus_enabled true \
        +procgen_seed "$seed" +map PROCMAP >"$log" 2>&1 &
    pid=$!
    for _ in $(seq 1 20); do
        if grep -q 'Procedural soundtrack selected from ' "$log" 2>/dev/null; then
            reached=1
            sleep 2
            break
        fi
        if ! kill -0 "$pid" 2>/dev/null; then break; fi
        sleep 1
    done
    if kill -0 "$pid" 2>/dev/null; then
        kill -KILL -- "-$pid" 2>/dev/null || true
    fi
    wait "$pid" 2>/dev/null || true

	[ "$reached" -eq 1 ] && grep -q 'Opened device No Output' "$log" &&
		grep -Eq 'Procedural soundtrack active: .+' "$log" &&
		! grep -q 'Procedural soundtrack active: <none>' "$log" &&
		! grep -Eqi 'Received AL error|Unable to (load|start).*music|Failed to play music|Unable to open any MIDI Device' "$log"
}

count_blocks() {
    local block=$1
    grep -c "^${block}$" /tmp/procmap_test.udmf 2>/dev/null || true
}

measure_exit_room_area() {
    python3 - <<'PY'
import collections
import re

text = open('/tmp/procmap_test.udmf', encoding='utf-8').read()

def blocks(kind):
    return [dict((key, value.strip('"')) for key, value in
                 re.findall(r'^\s*(\w+)\s*=\s*([^;]+);', body, re.M))
            for body in re.findall(r'(?m)^' + kind + r'\s*\n\{(.*?)\n\}', text, re.S)]

vertices = blocks('vertex')
sectors = blocks('sector')
sides = blocks('sidedef')
lines = blocks('linedef')
exit_line = next((line for line in lines if line.get('special') == '243'), None)
if exit_line is None:
    print(0)
    raise SystemExit

trigger_sector = int(sides[int(exit_line['sidefront'])]['sector'])
adjacency = collections.defaultdict(set)
for line in lines:
    front = int(sides[int(line['sidefront'])]['sector'])
    back = int(sides[int(line['sideback'])]['sector']) if 'sideback' in line else -1
    if back >= 0 and front != back:
        adjacency[front].add(back)
        adjacency[back].add(front)

distance = {trigger_sector: 0}
queue = [trigger_sector]
for current in queue:
    if distance[current] >= 2:
        continue
    for neighbor in adjacency[current]:
        if neighbor not in distance:
            distance[neighbor] = distance[current] + 1
            queue.append(neighbor)
candidate_sectors = [sector for sector, depth in distance.items() if 1 <= depth <= 2]
if not candidate_sectors:
    print(0)
    raise SystemExit

def sector_area(room_sector):
    double_area = 0.0
    for line in lines:
        front = int(sides[int(line['sidefront'])]['sector'])
        back = int(sides[int(line['sideback'])]['sector']) if 'sideback' in line else -1
        if front == room_sector and back == room_sector:
            continue
        first = vertices[int(line['v1'])]
        second = vertices[int(line['v2'])]
        cross = (float(first['x']) * float(second['y']) -
                 float(second['x']) * float(first['y']))
        if front == room_sector:
            double_area += cross
        if back == room_sector:
            double_area -= cross
    return abs(double_area) * 0.5

base_sector = max(candidate_sectors, key=sector_area)
area = sector_area(base_sector)
liquid_flats = {'FWATER1', 'BLOOD1', 'NUKAGE1', 'LAVA1'}
area += sum(sector_area(neighbor) for neighbor in adjacency[base_sector]
            if sectors[neighbor].get('texturefloor') in liquid_flats)
print(round(area))
PY
}

report_dump() {
    local sectors things locks keys monsters decorations powerups
    sectors=$(count_blocks sector)
    things=$(count_blocks thing)
    locks=$(grep -c '^\s*locknumber = ' /tmp/procmap_test.udmf 2>/dev/null || true)
    keys=$(grep -Ec '^\s*type = (5|6|13);' /tmp/procmap_test.udmf 2>/dev/null || true)
    monsters=$(grep -Ec '^\s*type = (7|9|16|58|64|65|66|67|68|69|71|72|84|88|89|3001|3002|3003|3004|3005|3006);' \
        /tmp/procmap_test.udmf 2>/dev/null || true)
    decorations=$(grep -Ec '^\s*type = (15|20|35|41|43|44|45|46|48|55|56|57|59|60|85|86|2028|2035);' \
        /tmp/procmap_test.udmf 2>/dev/null || true)
    powerups=$(grep -Ec '^\s*type = (8|83|2013|2022|2023|2024|2026|2045);' \
        /tmp/procmap_test.udmf 2>/dev/null || true)
    echo "  sectors=$sectors things=$things monsters=$monsters decorations=$decorations powerups=$powerups locks=$locks keys=$keys"
}

validate_geometry() {
	local size=${1:-3}
	local verticality=${2:-1}
	local detail=${3:-1}
	python3 - "$size" "$verticality" "$detail" <<'PY'
import collections
import math
import re
import statistics
import sys

size = int(sys.argv[1])
verticality = int(sys.argv[2])
detail = int(sys.argv[3])

text = open('/tmp/procmap_test.udmf', encoding='utf-8').read()

def blocks(kind):
    result = []
    for body in re.findall(r'(?m)^' + kind + r'\s*\n\{(.*?)\n\}', text, re.S):
        result.append(dict((key, value.strip('"')) for key, value in
                           re.findall(r'^\s*(\w+)\s*=\s*([^;]+);', body, re.M)))
    return result

vertices = blocks('vertex')
sectors = blocks('sector')
sides = blocks('sidedef')
lines = blocks('linedef')
things = blocks('thing')
errors = []
adjacency = collections.defaultdict(set)
referenced = set()
boundary_diagonal_lines = 0
boundary_non45_lines = 0
boundary_lengths = set()
sector_double_areas = [0.0] * len(sectors)
sector_line_indices = collections.defaultdict(set)
solid_walls = []
solid_walls_at_vertex = collections.defaultdict(list)
silhouette_wall_count = 0
geometric_lines = collections.defaultdict(list)
sector_boundary_edges = collections.defaultdict(list)

for index, vertex in enumerate(vertices):
    if abs(float(vertex['x'])) > 24500.0 or abs(float(vertex['y'])) > 24500.0:
        errors.append(f'vertex {index} exceeds the guarded procedural coordinate range')

def point_segment_distance(px, py, ax, ay, bx, by):
    dx, dy = bx - ax, by - ay
    length_squared = dx * dx + dy * dy
    if length_squared == 0.0:
        return math.hypot(px - ax, py - ay)
    amount = max(0.0, min(1.0, ((px - ax) * dx + (py - ay) * dy) / length_squared))
    return math.hypot(px - (ax + amount * dx), py - (ay + amount * dy))

for index, line in enumerate(lines):
    try:
        v1, v2 = int(line['v1']), int(line['v2'])
        front = int(line['sidefront'])
    except (KeyError, ValueError):
        errors.append(f'linedef {index} has malformed references')
        continue
    if not (0 <= v1 < len(vertices) and 0 <= v2 < len(vertices)):
        errors.append(f'linedef {index} has an invalid vertex reference')
        continue
    if vertices[v1]['x'] == vertices[v2]['x'] and vertices[v1]['y'] == vertices[v2]['y']:
        errors.append(f'linedef {index} has zero length')

    if not 0 <= front < len(sides):
        errors.append(f'linedef {index} has an invalid front side')
        continue
    front_sector = int(sides[front]['sector'])
    referenced.add(front_sector)
    sector_line_indices[front_sector].add(index)
    x1, y1 = float(vertices[v1]['x']), float(vertices[v1]['y'])
    x2, y2 = float(vertices[v2]['x']), float(vertices[v2]['y'])
    cross_product = x1 * y2 - x2 * y1
    sector_double_areas[front_sector] += cross_product

    if 'sideback' not in line:
        if line.get('blocking') != 'true':
            errors.append(f'one-sided linedef {index} is not blocking')
        if 'texturemiddle' not in sides[front]:
            errors.append(f'one-sided linedef {index} has no middle texture')
        if line.get('dontpegbottom') != 'true':
            errors.append(f'one-sided linedef {index} is not bottom-pegged')
        length = math.hypot(x2 - x1, y2 - y1)
        if length > 8.01:
            silhouette_wall_count += 1
        if x1 != x2 and y1 != y2:
            boundary_diagonal_lines += 1
            if abs(abs(x2 - x1) - abs(y2 - y1)) > 0.01:
                boundary_non45_lines += 1
        boundary_lengths.add(round(length))
        solid_walls.append((x1, y1, x2, y2))
        middle = sides[front].get('texturemiddle', '')
        is_switch = middle.startswith('SW1') and line.get('special') == '11'
        solid_walls_at_vertex[v1].append((index, v2, front_sector, middle, is_switch))
        solid_walls_at_vertex[v2].append((index, v1, front_sector, middle, is_switch))
        if is_switch:
            if abs(length - 64.0) > 0.01:
                errors.append(f'switch linedef {index} is {length:.1f} units wide instead of 64')
            if int(sides[front].get('offsetx', '0')) != 0 or int(sides[front].get('offsety', '0')) != 0:
                errors.append(f'switch linedef {index} does not start at one clean texture origin')
            if abs(float(sides[front].get('scalex_mid', '1.0')) - 1.0) > 0.001:
                errors.append(f'switch linedef {index} repeats or crops horizontally')
            wall_height = (float(sectors[front_sector]['heightceiling']) -
                           float(sectors[front_sector]['heightfloor']))
            expected_scale = 128.0 / max(1.0, wall_height)
            actual_scale = float(sides[front].get('scaley_mid', '1.0'))
            if abs(actual_scale - expected_scale) > 0.001:
                errors.append(f'switch linedef {index} repeats vertically '
                              f'(scale={actual_scale:.3f}, expected={expected_scale:.3f})')
        else:
            # Ordinary walls are aligned from the active IWAD texture's real
            # logical metrics, not from a fictional universal 128-unit motif.
            # The serialized test deliberately validates only invariant-safe
            # transform properties here: texture dimensions and intentional
            # architectural phase groups belong to the generator/manifest,
            # while stock switches and doors retain exact-fit contracts above.
            for field in ('offsetx', 'offsety'):
                try:
                    value = float(sides[front].get(field, '0'))
                except ValueError:
                    errors.append(f'one-sided linedef {index} has a nonnumeric {field}')
                    continue
                if not math.isfinite(value):
                    errors.append(f'one-sided linedef {index} has a non-finite {field}')
            for field in ('scalex_mid', 'scaley_mid'):
                try:
                    value = float(sides[front].get(field, '1.0'))
                except ValueError:
                    errors.append(f'one-sided linedef {index} has a nonnumeric {field}')
                    continue
                if not math.isfinite(value) or value <= 0.0:
                    errors.append(f'one-sided linedef {index} has an invalid {field}={value!r}')
    else:
        back = int(line['sideback'])
        if not 0 <= back < len(sides):
            errors.append(f'linedef {index} has an invalid back side')
            continue
        if line.get('blocking') == 'true':
            errors.append(f'two-sided linedef {index} incorrectly masquerades as a solid wall')
        back_sector = int(sides[back]['sector'])
        sector_double_areas[back_sector] -= cross_product
        sector_line_indices[back_sector].add(index)
        referenced.add(back_sector)
        if front_sector != back_sector:
            adjacency[front_sector].add(back_sector)
            adjacency[back_sector].add(front_sector)

    # Coincident linedefs and zero-area boundary loops are accepted by the UDMF
    # parser but can make the GL node builder leave black floor/ceiling holes.
    # Track them independently of gameplay topology so huge-map regressions are
    # rejected before the engine reaches the renderer.
    geometric_lines[tuple(sorted((v1, v2)))].append(index)
    back_sector = (int(sides[int(line['sideback'])]['sector'])
                   if 'sideback' in line else None)
    if front_sector != back_sector:
        sector_boundary_edges[front_sector].append((v1, v2, index))
        if back_sector is not None:
            sector_boundary_edges[back_sector].append((v2, v1, index))

sector_areas = [abs(double_area) * 0.5 for double_area in sector_double_areas]

duplicate_geometry = [indices for indices in geometric_lines.values() if len(indices) > 1]
if duplicate_geometry:
    errors.append(f'map contains {len(duplicate_geometry)} coincident linedef groups')

for sector_index, boundary in sector_boundary_edges.items():
    incoming = collections.Counter(second for first, second, line in boundary)
    outgoing = collections.Counter(first for first, second, line in boundary)
    boundary_vertices = set(incoming) | set(outgoing)
    malformed = [vertex for vertex in boundary_vertices
                 if incoming[vertex] != 1 or outgoing[vertex] != 1]
    if malformed:
        errors.append(f'sector {sector_index} has {len(malformed)} branched or open '
                      'boundary vertices')
        continue

    next_edge = {first: (second, line) for first, second, line in boundary}
    unused = set(next_edge)
    while unused:
        start = next(iter(unused))
        current = start
        loop = []
        while current in unused:
            unused.remove(current)
            loop.append(current)
            current = next_edge[current][0]
        if current != start:
            errors.append(f'sector {sector_index} has an unclosed boundary loop')
            break
        points = [(float(vertices[vertex]['x']), float(vertices[vertex]['y']))
                  for vertex in loop]
        double_area = sum(
            points[position][0] * points[(position + 1) % len(points)][1] -
            points[(position + 1) % len(points)][0] * points[position][1]
            for position in range(len(points)))
        if abs(double_area) < 0.01:
            errors.append(f'sector {sector_index} has a zero-area boundary loop')

# Huge maps contain tens of thousands of solid segments. Index their bounding
# boxes once so clearance assertions inspect only geometrically nearby walls
# instead of performing millions of point-to-segment calculations.
spatial_bucket_size = 256.0
solid_wall_buckets = collections.defaultdict(list)
for wall in solid_walls:
    ax, ay, bx, by = wall
    min_bucket_x = math.floor(min(ax, bx) / spatial_bucket_size)
    max_bucket_x = math.floor(max(ax, bx) / spatial_bucket_size)
    min_bucket_y = math.floor(min(ay, by) / spatial_bucket_size)
    max_bucket_y = math.floor(max(ay, by) / spatial_bucket_size)
    for bucket_x in range(min_bucket_x, max_bucket_x + 1):
        for bucket_y in range(min_bucket_y, max_bucket_y + 1):
            solid_wall_buckets[(bucket_x, bucket_y)].append(wall)

def nearby_solid_walls(px, py, radius):
    result = set()
    min_bucket_x = math.floor((px - radius) / spatial_bucket_size)
    max_bucket_x = math.floor((px + radius) / spatial_bucket_size)
    min_bucket_y = math.floor((py - radius) / spatial_bucket_size)
    max_bucket_y = math.floor((py + radius) / spatial_bucket_size)
    for bucket_x in range(min_bucket_x, max_bucket_x + 1):
        for bucket_y in range(min_bucket_y, max_bucket_y + 1):
            result.update(solid_wall_buckets.get((bucket_x, bucket_y), ()))
    return result

thing_buckets = collections.defaultdict(list)
for thing in things:
    thing_buckets[(math.floor(float(thing['x']) / spatial_bucket_size),
                   math.floor(float(thing['y']) / spatial_bucket_size))].append(thing)

for index, side in enumerate(sides):
    sector = int(side.get('sector', '-1'))
    if not 0 <= sector < len(sectors):
        errors.append(f'sidedef {index} has an invalid sector reference')
    # UDMF normally shares offsets across the side, but engines/extensions may
    # expose per-part offsets. Validate every serialized transform without
    # assuming a particular texture's native dimensions or phase origin.
    for field in ('offsetx', 'offsety', 'offsetx_top', 'offsety_top',
                  'offsetx_mid', 'offsety_mid', 'offsetx_bottom', 'offsety_bottom'):
        if field not in side:
            continue
        try:
            value = float(side[field])
        except ValueError:
            errors.append(f'sidedef {index} has a nonnumeric {field}')
            continue
        if not math.isfinite(value):
            errors.append(f'sidedef {index} has a non-finite {field}')
    for texture_field, scale_suffix in (
            ('texturetop', 'top'), ('texturemiddle', 'mid'), ('texturebottom', 'bottom')):
        texture = side.get(texture_field, '-')
        if texture == '-':
            continue
        for axis in ('x', 'y'):
            field = f'scale{axis}_{scale_suffix}'
            try:
                value = float(side.get(field, '1.0'))
            except ValueError:
                errors.append(f'sidedef {index} has a nonnumeric {field}')
                continue
            if not math.isfinite(value) or value <= 0.0:
                errors.append(f'sidedef {index} has an invalid {field}={value!r}')

# A texture family may change at a door, portal, corner, step, or other visible
# architectural seam. It must not change where two collinear solid segments
# merely meet in the middle of an otherwise continuous flat wall.
flat_texture_seams = set()
for shared_vertex, attached in solid_walls_at_vertex.items():
    origin = vertices[shared_vertex]
    ox, oy = float(origin['x']), float(origin['y'])
    for first_index in range(len(attached)):
        first_line, first_other, first_sector, first_texture, first_switch = attached[first_index]
        ax = float(vertices[first_other]['x']) - ox
        ay = float(vertices[first_other]['y']) - oy
        for second_index in range(first_index + 1, len(attached)):
            second_line, second_other, second_sector, second_texture, second_switch = attached[second_index]
            if first_sector != second_sector or first_texture == second_texture:
                continue
            if first_switch or second_switch:
                continue
            bx = float(vertices[second_other]['x']) - ox
            by = float(vertices[second_other]['y']) - oy
            cross = ax * by - ay * bx
            if abs(cross) <= 0.001 and ax * bx + ay * by < 0.0:
                flat_texture_seams.add(tuple(sorted((first_line, second_line))))
if flat_texture_seams:
    errors.append(f'{len(flat_texture_seams)} abrupt texture changes split continuous flat walls')

doors = [line for line in lines if line.get('special') == '12']
door_sectors = collections.defaultdict(list)
door_texture_sizes = {
    'DOOR1': (64, 72), 'DOOR3': (64, 72),
    'BIGDOOR1': (128, 96), 'BIGDOOR2': (128, 128),
    'BIGDOOR3': (128, 128), 'BIGDOOR4': (128, 128),
    'BIGDOOR5': (128, 128), 'BIGDOOR6': (128, 112),
    'BIGDOOR7': (128, 128),
    'SPCDOOR1': (64, 128), 'SPCDOOR2': (64, 128),
    'SPCDOOR3': (64, 128), 'SPCDOOR4': (64, 128),
    'MARBFAC2': (128, 128), 'MARBFAC3': (128, 128),
}
door_face_heights = set()
door_face_textures = set()

# Door art is allowed to use the legacy side-wide offset as well as the
# UDMF per-part fields used by the general alignment pass.  Evaluate the
# effective top-band transform so an emitter cannot accidentally move the
# door face through a newly added per-part field and escape the exact-fit
# check below.
def effective_part_offset(side, side_index, axis, part):
    shared_name = f'offset{axis}'
    part_name = f'offset{axis}_{part}'
    try:
        shared = float(side.get(shared_name, '0'))
        part_offset = float(side.get(part_name, '0'))
    except (TypeError, ValueError):
        errors.append(f'sidedef {side_index} has a nonnumeric {shared_name}/{part_name} transform')
        return None
    if not math.isfinite(shared) or not math.isfinite(part_offset):
        errors.append(f'sidedef {side_index} has a non-finite {shared_name}/{part_name} transform')
        return None
    return shared + part_offset

def part_scale(side, side_index, axis, part):
    field = f'scale{axis}_{part}'
    try:
        value = float(side.get(field, '1.0'))
    except (TypeError, ValueError):
        errors.append(f'sidedef {side_index} has a nonnumeric {field}')
        return None
    if not math.isfinite(value) or value <= 0.0:
        errors.append(f'sidedef {side_index} has an invalid {field}={value!r}')
        return None
    return value

if not doors:
    errors.append('map contains no functional Door_Raise linedefs')
for index, door in enumerate(doors):
    if door.get('playeruse') != 'true' or door.get('repeatspecial') != 'true':
        errors.append(f'door {index} lacks use/repeat activation')
    if door.get('arg0') != '0' or door.get('arg1') != '16' or door.get('arg2') != '150':
        errors.append(f'door {index} has invalid Door_Raise arguments')
    back = int(door['sideback'])
    door_sector = int(sides[back]['sector'])
    door_sectors[door_sector].append(door)
    sector = sectors[door_sector]
    if sector['heightfloor'] != sector['heightceiling']:
        errors.append(f'door sector {door_sector} does not start closed')
    front = int(door['sidefront'])
    face_texture = sides[front].get('texturetop')
    secret_door = door.get('secret') == 'true'
    if face_texture in ('DOORTRAK', 'DOORRED', 'DOORBLU', 'DOORYEL', None):
        errors.append(f'door {index} has an invalid face texture')
    if door.get('dontpegtop') == 'true':
        errors.append(f'door {index} incorrectly pins its moving face')
    a, b = vertices[int(door['v1'])], vertices[int(door['v2'])]
    face_width = math.dist((float(a['x']), float(a['y'])),
                           (float(b['x']), float(b['y'])))
    native_width, native_height = ((128, 128) if secret_door else
                                   door_texture_sizes.get(face_texture, (0, 0)))
    if native_width == 0:
        errors.append(f'door {index} uses unclassified stock texture {face_texture}')
    elif face_width < (64.0 if secret_door else 128.0) - 0.01:
        # A stock 64-unit texture is art, not a permission to emit a 64-unit
        # manual/keyed traversal slab. Optional disguised cache panels may retain
        # their compact 64-unit shell; every ordinary Door_Raise face has a full
        # Standard physical aperture and uses native art fitting independently.
        errors.append(f'door {index} has only {face_width:.1f}-unit physical clearance')
    # Door art is never horizontally scaled.  A compact face receives a
    # centered crop, while a gallery/grand face repeats at native scale with a
    # signed centered phase.  In both cases the physical door midpoint maps to
    # the midpoint of a native tile; clamping the old crop at zero left every
    # wider-than-a-tile door visibly biased toward one jamb.
    expected_phase = round((native_width - face_width) * 0.5)
    front_x = effective_part_offset(sides[front], front, 'x', 'top')
    back_x = effective_part_offset(sides[back], back, 'x', 'top')
    front_y = effective_part_offset(sides[front], front, 'y', 'top')
    back_y = effective_part_offset(sides[back], back, 'y', 'top')
    if front_x is not None and abs(front_x - expected_phase) > 0.01:
        errors.append(f'door {index} does not center {face_texture} across its jambs')
    if back_x is not None and abs(back_x - expected_phase) > 0.01:
        errors.append(f'door {index} has a mismatched back-face horizontal phase')
    if front_y is not None and abs(front_y) > 0.01:
        errors.append(f'door {index} has an unexpected front-face vertical top-band offset')
    if back_y is not None and abs(back_y) > 0.01:
        errors.append(f'door {index} has an unexpected back-face vertical top-band offset')
    front_sector = sectors[int(sides[front]['sector'])]
    face_height = float(front_sector['heightceiling']) - float(sector['heightfloor'])
    if not secret_door and abs(face_height - native_height) > 0.01:
        errors.append(f'door {index} has {face_height:.1f}-unit lintel clearance but '
                      f'{face_texture} is {native_height} units tall')
    if secret_door and round(face_height) not in (96, 128):
        errors.append(f'secret door {index} has non-stock {face_height:.1f}-unit clearance')
    expected_scale = min(1.0, native_height / max(1.0, face_height))
    actual_scale = part_scale(sides[front], front, 'y', 'top')
    actual_x_scale = part_scale(sides[front], front, 'x', 'top')
    if actual_scale is not None and abs(actual_scale - expected_scale) > 0.001:
        errors.append(f'door {index} vertically repeats instead of fitting once '
                      f'(scale={actual_scale:.3f}, expected={expected_scale:.3f})')
    if actual_x_scale is not None and abs(actual_x_scale - 1.0) > 0.001:
        errors.append(f'door {index} horizontally scales instead of using its fitted crop')
    actual_back_scale = part_scale(sides[back], back, 'y', 'top')
    actual_back_x_scale = part_scale(sides[back], back, 'x', 'top')
    if actual_back_scale is not None and abs(actual_back_scale - expected_scale) > 0.001:
        errors.append(f'door {index} has mismatched back-face vertical scale')
    if actual_back_x_scale is not None and abs(actual_back_x_scale - 1.0) > 0.001:
        errors.append(f'door {index} has a mismatched back-face horizontal scale')
    approach_sector = int(sides[front]['sector'])
    approach_neighbors = adjacency[approach_sector]
    if door_sector not in approach_neighbors or len(approach_neighbors) != 2:
        errors.append(f'door {index} approach sector is not a contained room/lintel transition')
    door_face_heights.add(round(face_height))
    if not secret_door:
        door_face_textures.add(face_texture)

track_for_lock = {0: 'DOORTRAK', 1: 'DOORRED', 2: 'DOORBLU', 3: 'DOORYEL'}
for sector_index, faces in door_sectors.items():
    if len(faces) != 2:
        errors.append(f'door sector {sector_index} has {len(faces)} faces instead of two')
        continue
    centers = []
    for face in faces:
        a, b = vertices[int(face['v1'])], vertices[int(face['v2'])]
        centers.append(((float(a['x']) + float(b['x'])) * 0.5,
                        (float(a['y']) + float(b['y'])) * 0.5))
    if abs(math.dist(centers[0], centers[1]) - 16.0) > 0.01:
        errors.append(f'door sector {sector_index} is not a classic 16-unit slab')
    lock = int(faces[0].get('locknumber', '0'))
    secret_door = all(face.get('secret') == 'true' for face in faces)
    tracks = []
    for line_index in sector_line_indices[sector_index]:
        line = lines[line_index]
        front = int(line['sidefront'])
        if ('sideback' not in line and int(sides[front]['sector']) == sector_index and
                'texturemiddle' in sides[front]):
            tracks.append((line, sides[front]))
    if len(tracks) != 2:
        errors.append(f'door sector {sector_index} has {len(tracks)} track walls instead of two')
    for line, side in tracks:
        if not secret_door and side.get('texturemiddle') != track_for_lock.get(lock, 'DOORTRAK'):
            errors.append(f'door sector {sector_index} has the wrong keyed track texture')
        if line.get('dontpegbottom') != 'true':
            errors.append(f'door sector {sector_index} has a moving track texture')

# Remove every keyed door sector and reconstruct the remaining traversable
# topology. The two approaches to each lock must land in distinct components;
# otherwise an ordinary door or open portal provides a keyless bypass. Treat
# normal doors as eventually traversable so this proves the stronger gameplay
# property rather than merely checking the initially closed map state.
keyed_door_sectors = {
    sector_index for sector_index, faces in door_sectors.items()
    if any(int(face.get('locknumber', '0')) > 0 for face in faces)
}
unlocked_adjacency = collections.defaultdict(set)
for line in lines:
    if 'sideback' not in line:
        continue
    front = int(sides[int(line['sidefront'])]['sector'])
    back = int(sides[int(line['sideback'])]['sector'])
    if front == back or front in keyed_door_sectors or back in keyed_door_sectors:
        continue
    unlocked_adjacency[front].add(back)
    unlocked_adjacency[back].add(front)

unlocked_component = {}
component_index = 0
for sector_index in range(len(sectors)):
    if sector_index in keyed_door_sectors or sector_index in unlocked_component:
        continue
    unlocked_component[sector_index] = component_index
    queue = collections.deque([sector_index])
    while queue:
        current = queue.popleft()
        for neighbor in unlocked_adjacency[current]:
            if neighbor not in unlocked_component:
                unlocked_component[neighbor] = component_index
                queue.append(neighbor)
    component_index += 1

locked_component_edges = set()
for sector_index in keyed_door_sectors:
    approaches = {
        int(sides[int(face['sidefront'])]['sector'])
        for face in door_sectors[sector_index]
    }
    if len(approaches) != 2:
        errors.append(f'keyed door sector {sector_index} does not have two distinct approaches')
        continue
    first, second = approaches
    first_component = unlocked_component.get(first)
    second_component = unlocked_component.get(second)
    if first_component == second_component:
        errors.append(f'keyed door sector {sector_index} can be bypassed through an '
                      'ordinary unlocked door or opening')
        continue
    edge = tuple(sorted((first_component, second_component)))
    if edge in locked_component_edges:
        errors.append(f'keyed door sector {sector_index} duplicates a progression cut')
    locked_component_edges.add(edge)

sector_ids = {}
for index, sector in enumerate(sectors):
    if 'id' not in sector:
        continue
    sector_id = int(sector['id'])
    if sector_id in sector_ids:
        errors.append(f'sector id {sector_id} is duplicated')
    sector_ids[sector_id] = index

# Ordinary traversable boundaries must remain within Doom's step and headroom
# contracts. Monster-retaining sides, closed doors, and operable lifts are
# validated separately; the open route onto every perch is checked here too.
for index, line in enumerate(lines):
    if 'sideback' not in line:
        continue
    front_sector = int(sides[int(line['sidefront'])]['sector'])
    back_sector = int(sides[int(line['sideback'])]['sector'])
    if front_sector == back_sector:
        continue
    first, second = sectors[front_sector], sectors[back_sector]
    first_floor, second_floor = float(first['heightfloor']), float(second['heightfloor'])
    first_ceiling = float(first['heightceiling'])
    second_ceiling = float(second['heightceiling'])
    if first_floor == first_ceiling or second_floor == second_ceiling:
        continue
    tagged_ids = {int(first.get('id', '0')), int(second.get('id', '0'))}
    if line.get('blockmonsters') == 'true':
        continue
    if any(3000 <= sector_id < 4000 for sector_id in tagged_ids):
        continue
    first_vertex = vertices[int(line['v1'])]
    second_vertex = vertices[int(line['v2'])]
    boundary_width = math.hypot(
        float(second_vertex['x']) - float(first_vertex['x']),
        float(second_vertex['y']) - float(first_vertex['y']))
    if boundary_width < 32.0:
        # Short two-sided shoulder seams can border a raised sill, but a Doom
        # player cannot physically fit through them. The actual window aperture
        # is monster-blocked and validated separately below.
        continue
    opening = min(first_ceiling, second_ceiling) - max(first_floor, second_floor)
    if opening < 56.0:
        errors.append(f'traversable linedef {index} has only {opening:.1f} units of headroom')
    if abs(first_floor - second_floor) > 24.0:
        errors.append(f'traversable linedef {index} has an impassable '
                      f'{abs(first_floor - second_floor):.1f}-unit floor step')

# Inter-room elevation is authored as broad terraces connected by repeated
# eight-unit risers. Check both the overall silhouette and enough full-width
# risers to prevent a regression to shallow per-room height jitter.
playable_floors = [float(sector['heightfloor']) for sector in sectors
                   if float(sector['heightceiling']) > float(sector['heightfloor'])]
floor_levels = {round(height) for height in playable_floors}
minimum_floor_range = (64.0, 96.0, 128.0)[verticality]
if not playable_floors or max(playable_floors) - min(playable_floors) < minimum_floor_range:
    errors.append(f'procedural elevation range is below the {minimum_floor_range:.0f}-unit contract')
minimum_floor_levels = (6, 8, 10)[verticality]
if len(floor_levels) < minimum_floor_levels:
    errors.append(f'procedural map has only {len(floor_levels)} distinct floor levels')

full_width_risers = 0
for line in lines:
    if 'sideback' not in line or line.get('dontpegtop') != 'true' or \
            line.get('dontpegbottom') != 'true':
        continue
    front_index = int(sides[int(line['sidefront'])]['sector'])
    back_index = int(sides[int(line['sideback'])]['sector'])
    rise = abs(float(sectors[front_index]['heightfloor']) -
               float(sectors[back_index]['heightfloor']))
    if abs(rise - 8.0) > 0.01:
        continue
    first, second = vertices[int(line['v1'])], vertices[int(line['v2'])]
    length = math.hypot(float(second['x']) - float(first['x']),
                        float(second['y']) - float(first['y']))
    if length >= 127.9:
        full_width_risers += 1
minimum_risers = (8 + size * 5, 12 + size * 10, 16 + size * 13)[verticality]
if full_width_risers < minimum_risers:
    errors.append(f'only {full_width_risers} full-width 8-unit stair risers '
                  f'(expected at least {minimum_risers})')

# Varied and Dramatic blueprints promise two named dogleg route beats. A
# manifest label alone is not sufficient: each realized dogleg owns a 64-unit
# offset mouth and a chain of eight-unit risers, which a conventional wide
# connector cannot accidentally satisfy. Two four-tread runs are the minimum
# possible pair (one ascent and one descent).
if size >= 3 and verticality >= 1:
    narrow_dogleg_risers = 0
    for line in lines:
        if 'sideback' not in line:
            continue
        front_index = int(sides[int(line['sidefront'])]['sector'])
        back_index = int(sides[int(line['sideback'])]['sector'])
        first_sector = sectors[front_index]
        second_sector = sectors[back_index]
        if (float(first_sector['heightceiling']) <= float(first_sector['heightfloor']) or
                float(second_sector['heightceiling']) <= float(second_sector['heightfloor'])):
            continue
        first, second = vertices[int(line['v1'])], vertices[int(line['v2'])]
        width = math.hypot(float(second['x']) - float(first['x']),
                           float(second['y']) - float(first['y']))
        rise = abs(float(first_sector['heightfloor']) -
                   float(second_sector['heightfloor']))
        if abs(width - 64.0) <= 0.01 and abs(rise - 8.0) <= 0.01:
            narrow_dogleg_risers += 1
    if narrow_dogleg_risers < 8:
        errors.append(f'only {narrow_dogleg_risers} narrow dogleg risers were emitted '
                      f'(expected at least 8 for Varied/Dramatic route turns)')

remote_openers = [line for line in lines if line.get('special') == '11']
# A remote cache is an optional motif, not a mandatory obstruction. The
# generator must omit it when it cannot reserve its switch, closed slab, and
# circulation clearance without weakening the proven route. Validate every
# cache that does serialize, but do not turn that safe fallback into a corpus
# failure merely because this recipe has no feasible switch cache.
for opener_index, opener in enumerate(remote_openers):
    try:
        target = int(opener.get('arg0', '0'))
    except ValueError:
        target = -1
    # Door_Open is deliberately a player choice. Key collection and ordinary
    # floor traversal must never fire a remote reveal or an off-screen ambush.
    if opener.get('playeruse') != 'true':
        errors.append(f'Door_Open linedef {opener_index} is not a manual-use switch')
    if opener.get('playercross') == 'true':
        errors.append(f'Door_Open linedef {opener_index} incorrectly uses player-cross activation')
    if 'sideback' in opener:
        errors.append(f'Door_Open linedef {opener_index} is not a one-sided switch panel')
    if not 1500 <= target < 2000:
        errors.append(f'Door_Open linedef {opener_index} targets non-cache sector id {target}')
    if target not in sector_ids:
        errors.append(f'Door_Open trigger targets missing sector id {target}')
        continue
    target_sector = sectors[sector_ids[target]]
    if target_sector['heightfloor'] != target_sector['heightceiling']:
        errors.append(f'remote door sector id {target} does not start closed')
    if opener.get('arg1') != '16':
        errors.append(f'remote door sector id {target} uses the wrong speed')
for opener in remote_openers:
    side = sides[int(opener['sidefront'])]
    if side.get('texturemiddle') not in {'SW1COMP', 'SW1GARG'}:
        errors.append('remote Door_Open use line does not use a fitted 64x128 switch texture')
switch_counts = collections.Counter(int(opener.get('arg0', '0')) for opener in remote_openers)
for target, count in switch_counts.items():
    if count != 1:
        errors.append(f'remote reveal sector id {target} has {count} switch panels instead of one')

# Reveals may be clipped freestanding pavilions, rectangular wall banks, or
# false-wall chambers grown into an empty neighboring cell. Recover their door
# and closet topology from the serialized graph instead of assuming one shell.
reveal_targets = sorted(set(int(opener.get('arg0', '0')) for opener in remote_openers))
reveal_footprints = set()
reveal_orientations = set()
reveal_silhouettes = set()
reveal_vertical_profiles = set()
reveal_architectures = set()
reveal_cues = set()

def one_sided_component(sector_index, seed_vertices):
    by_vertex = collections.defaultdict(list)
    for line_index in sector_line_indices[sector_index]:
        line = lines[line_index]
        if 'sideback' in line:
            continue
        front_sector = int(sides[int(line['sidefront'])]['sector'])
        if front_sector != sector_index:
            continue
        by_vertex[int(line['v1'])].append(line_index)
        by_vertex[int(line['v2'])].append(line_index)
    selected = set()
    pending_vertices = list(seed_vertices)
    seen_vertices = set(pending_vertices)
    for vertex_index in pending_vertices:
        for line_index in by_vertex[vertex_index]:
            if line_index in selected:
                continue
            selected.add(line_index)
            line = lines[line_index]
            for endpoint in (int(line['v1']), int(line['v2'])):
                if endpoint not in seen_vertices:
                    seen_vertices.add(endpoint)
                    pending_vertices.append(endpoint)
    return selected, seen_vertices

one_sided_counts = collections.Counter(
    int(sides[int(line['sidefront'])]['sector'])
    for line in lines if 'sideback' not in line)

for target in reveal_targets:
    if target not in sector_ids:
        continue
    target_sector_index = sector_ids[target]
    faces = []
    for line_index in sector_line_indices[target_sector_index]:
        line = lines[line_index]
        if 'sideback' not in line:
            continue
        front_sector = int(sides[int(line['sidefront'])]['sector'])
        back_sector = int(sides[int(line['sideback'])]['sector'])
        if target_sector_index not in (front_sector, back_sector):
            continue
        if front_sector == back_sector:
            continue
        a, b = vertices[int(line['v1'])], vertices[int(line['v2'])]
        ax, ay = float(a['x']), float(a['y'])
        bx, by = float(b['x']), float(b['y'])
        faces.append((line_index, line, front_sector, ax, ay, bx, by))
    if len(faces) != 2:
        errors.append(f'reveal sector id {target} has {len(faces)} door faces instead of two')
        continue
    widths = [math.hypot(bx - ax, by - ay) for _, _, _, ax, ay, bx, by in faces]
    allowed_widths = (64.0, 80.0, 96.0)
    if any(min(abs(width - allowed) for allowed in allowed_widths) > 0.01
           for width in widths):
        errors.append(f'reveal sector id {target} has unsupported doorway widths {widths}')
    centers = [((ax + bx) * 0.5, (ay + by) * 0.5)
               for _, _, _, ax, ay, bx, by in faces]
    slab_depth = math.dist(centers[0], centers[1])
    false_wall = abs(slab_depth - 16.0) <= 0.1
    if not false_wall and not 15.9 <= slab_depth <= 30.1:
        errors.append(f'reveal sector id {target} has an incoherent {slab_depth:.1f}-unit door depth')
        continue

    # Structural piers and dogleg shells invalidate fixed line-count heuristics.
    # The playable room/outer loop always encloses more area than the closet's
    # inner boundary, so use actual connected geometry to identify the two faces.
    face_components = []
    for face in faces:
        component_lines, component_vertices = one_sided_component(
            face[2], (int(face[1]['v1']), int(face[1]['v2'])))
        component_points = [(float(vertices[index]['x']), float(vertices[index]['y']))
                            for index in component_vertices]
        component_area = ((max(point[0] for point in component_points) -
                           min(point[0] for point in component_points)) *
                          (max(point[1] for point in component_points) -
                           min(point[1] for point in component_points)))
        component_min_x = min(point[0] for point in component_points)
        component_max_x = max(point[0] for point in component_points)
        component_min_y = min(point[1] for point in component_points)
        component_max_y = max(point[1] for point in component_points)
        expected_contents = any(
            component_min_x < float(thing['x']) < component_max_x and
            component_min_y < float(thing['y']) < component_max_y and
            thing.get('type') in {'2008', '2012'}
            for thing in things)
        face_components.append((component_area, expected_contents, face,
                                component_lines, component_vertices))
    # A long narrow outer room can have a smaller bounding box than a broad
    # false-wall chamber. Contents identify the playable closet directly;
    # bounding-area order remains the constrained fallback for empty geometry.
    inner_entry = max(face_components, key=lambda entry: (entry[1], -entry[0]))
    if inner_entry[1] == 0:
        inner_entry = min(face_components, key=lambda entry: entry[0])
    outer_entry = next(entry for entry in face_components if entry is not inner_entry)
    _, _, inner_face, inner_lines, inner_vertices = inner_entry
    _, _, outer_face, outer_lines, outer_vertices = outer_entry
    outer_door_side = sides[int(outer_face[1]['sidefront'])]
    if outer_face[1].get('secret') == 'true':
        reveal_cues.add('hidden')
    elif outer_door_side.get('texturetop') in {
            'BIGDOOR1', 'BIGDOOR2', 'BIGDOOR3', 'BIGDOOR4',
            'BIGDOOR5', 'BIGDOOR6', 'BIGDOOR7'}:
        reveal_cues.add('prominent')
    else:
        reveal_cues.add('subtle')
    doorway_width = round(widths[0])
    architecture = ('false-wall' if false_wall else
                    ('wall-alcove' if doorway_width == 64 else 'pavilion'))
    reveal_architectures.add(architecture)
    if false_wall:
        if not 5 <= len(inner_lines) <= 10:
            errors.append(f'false-wall reveal sector id {target} has an invalid '
                          f'{len(inner_lines)}-edge chamber shell')
            continue
    elif not (7 <= len(outer_lines) <= 9) or not (7 <= len(inner_lines) <= 9):
        errors.append(f'reveal sector id {target} does not form two bounded '
                      f'feature loops (outer={len(outer_lines)}, inner={len(inner_lines)})')
        continue
    outer_diagonals = sum(
        vertices[int(lines[index]['v1'])]['x'] != vertices[int(lines[index]['v2'])]['x'] and
        vertices[int(lines[index]['v1'])]['y'] != vertices[int(lines[index]['v2'])]['y']
        for index in outer_lines)
    inner_diagonals = sum(
        vertices[int(lines[index]['v1'])]['x'] != vertices[int(lines[index]['v2'])]['x'] and
        vertices[int(lines[index]['v1'])]['y'] != vertices[int(lines[index]['v2'])]['y']
        for index in inner_lines)
    if not false_wall and (outer_diagonals != 4 or inner_diagonals != 4):
        errors.append(f'reveal sector id {target} has malformed pavilion/alcove corners '
                      f'(outer diagonals={outer_diagonals}, inner diagonals={inner_diagonals})')

    diagonal_lengths = []
    for line_index in outer_lines:
        line = lines[line_index]
        a, b = vertices[int(line['v1'])], vertices[int(line['v2'])]
        if a['x'] != b['x'] and a['y'] != b['y']:
            diagonal_lengths.append(round(math.dist(
                (float(a['x']), float(a['y'])),
                (float(b['x']), float(b['y']))), 1))
    if false_wall:
        reveal_silhouettes.add('false-wall')
    else:
        reveal_silhouettes.add('asymmetric' if len(set(diagonal_lengths)) > 1 else 'balanced')
    outer_sector_index = outer_face[2]
    inner_sector_index = inner_face[2]
    reveal_vertical_profiles.add((
        round(float(sectors[inner_sector_index]['heightfloor']) -
              float(sectors[outer_sector_index]['heightfloor'])),
        round(float(sectors[inner_sector_index]['heightceiling']) -
              float(sectors[outer_sector_index]['heightceiling'])),
    ))

    outer_points = [(float(vertices[index]['x']), float(vertices[index]['y']))
                    for index in outer_vertices]
    inner_points = [(float(vertices[index]['x']), float(vertices[index]['y']))
                    for index in inner_vertices]
    footprint_points = inner_points if false_wall else outer_points
    min_x = min(point[0] for point in footprint_points)
    max_x = max(point[0] for point in footprint_points)
    min_y = min(point[1] for point in footprint_points)
    max_y = max(point[1] for point in footprint_points)
    width, height = max_x - min_x, max_y - min_y
    valid_footprint = ((79.9 <= width <= 224.1 and 79.9 <= height <= 224.1)
                       if false_wall else
                       (139.9 <= width <= 224.1 and 139.9 <= height <= 224.1))
    if not valid_footprint:
        errors.append(f'reveal sector id {target} has an invalid varied footprint '
                      f'{width:.1f}x{height:.1f}')
    reveal_footprints.add((round(width), round(height)))
    outer_line = outer_face[1]
    a = vertices[int(outer_line['v1'])]
    b = vertices[int(outer_line['v2'])]
    reveal_orientations.add('horizontal' if a['y'] == b['y'] else 'vertical')

    if not false_wall:
        center_x = (min_x + max_x) * 0.5
        center_y = (min_y + max_y) * 0.5
        bounds = (min_x, min_y, max_x, max_y)
        external_walls = set()
        for wall in solid_walls:
            ax, ay, bx, by = wall
            local = (bounds[0] - 0.01 <= ax <= bounds[2] + 0.01 and
                     bounds[1] - 0.01 <= ay <= bounds[3] + 0.01 and
                     bounds[0] - 0.01 <= bx <= bounds[2] + 0.01 and
                     bounds[1] - 0.01 <= by <= bounds[3] + 0.01)
            if not local:
                external_walls.add(wall)
        if architecture == 'wall-alcove':
            backing_clearance = min((point_segment_distance(px, py, *wall)
                                     for px, py in outer_points
                                     for wall in nearby_solid_walls(px, py, 24.0)
                                     if wall in external_walls), default=24.0)
            if not 7.9 <= backing_clearance <= 16.1:
                errors.append(f'wall-alcove reveal sector id {target} is '
                              f'{backing_clearance:.1f} units from its backing wall')
            door_ax, door_ay = float(a['x']), float(a['y'])
            door_bx, door_by = float(b['x']), float(b['y'])
            samples = [
                (door_ax, door_ay), (door_bx, door_by),
                ((door_ax + door_bx) * 0.5, (door_ay + door_by) * 0.5),
            ]
        else:
            samples = outer_points + [
                (min_x, center_y), (max_x, center_y),
                (center_x, min_y), (center_x, max_y),
            ]
        clearance = min((point_segment_distance(px, py, *wall)
                         for px, py in samples
                         for wall in nearby_solid_walls(px, py, 64.0)
                         if wall in external_walls), default=64.0)
        if clearance < 63.9:
            errors.append(f'reveal sector id {target} leaves only {clearance:.1f} units '
                          'of circulation clearance')

    inner_min_x = min(point[0] for point in inner_points)
    inner_max_x = max(point[0] for point in inner_points)
    inner_min_y = min(point[1] for point in inner_points)
    inner_max_y = max(point[1] for point in inner_points)
    contained = [thing for thing in things
                 if inner_min_x < float(thing['x']) < inner_max_x and
                 inner_min_y < float(thing['y']) < inner_max_y]
    contained_types = {thing.get('type') for thing in contained}
    if not {'2008', '2012'}.issubset(contained_types):
        errors.append(f'switch reveal sector id {target} is missing its ammo/health cache')

if len(reveal_targets) >= 3 and len(reveal_cues) < 2:
    errors.append('all opportunity reveals use the same pre-opening visual cue')
if len(reveal_targets) >= 3 and len(reveal_orientations) < 2:
    errors.append('all opportunity reveal entrances use the same axis')

key_border_textures = {'13': 'DOORRED', '5': 'DOORBLU', '6': 'DOORYEL'}
for key_type, border in key_border_textures.items():
    if not any(thing.get('type') == key_type for thing in things):
        continue
    border_count = sum(side.get('texturemiddle') == border for side in sides)
    if border_count < 6:
        errors.append(f'{border} appears on only {border_count} keyed-door border segments')

monster_types = {'7', '9', '16', '58', '64', '65', '66', '67', '68', '69',
                 '71', '72', '84', '88', '89', '3001', '3002', '3003', '3004',
                 '3005', '3006'}
perch_sector_ids = [sector_id for sector_id in sector_ids if 2000 <= sector_id < 3000]
# Perches are an optional motif: compact, progression-dense layouts may have
# no safe multi-cell host once keys, locks, starts, and required landmarks are
# reserved. Validate every perch that was emitted here; the dedicated features
# matrix below keeps the three supported perch architectures covered globally.
perch_footprints = set()
perch_approaches = set()
for sector_id in perch_sector_ids:
    sector_index = sector_ids[sector_id]
    boundary = []
    adjacent = set()
    points = set()
    for line_index in sector_line_indices[sector_index]:
        line = lines[line_index]
        line_sectors = []
        for side_name in ('sidefront', 'sideback'):
            if side_name in line:
                line_sectors.append(int(sides[int(line[side_name])]['sector']))
        if sector_index not in line_sectors:
            continue
        boundary.append(line)
        points.add(int(line['v1']))
        points.add(int(line['v2']))
        adjacent.update(candidate for candidate in line_sectors if candidate != sector_index)
    retaining_edges = [line for line in boundary if line.get('blockmonsters') == 'true']
    open_edges = [line for line in boundary if line.get('blockmonsters') != 'true']
    if len(boundary) < 6 or len(retaining_edges) != len(boundary) - 1 or len(open_edges) != 1:
        errors.append(f'perch sector id {sector_id} does not have one traversable stair mouth')
    surrounding_floor = None
    if not adjacent:
        errors.append(f'perch sector id {sector_id} has no surrounding room sector')
    else:
        surrounding_floor = min(float(sectors[index]['heightfloor']) for index in adjacent)
        if float(sectors[sector_index]['heightfloor']) - surrounding_floor < 48.0:
            errors.append(f'perch sector id {sector_id} is not meaningfully elevated')

    # Prove reachability from serialized UDMF rather than trusting generator
    # intent. Traverse only non-blocking, monster-open boundaries with normal
    # Doom headroom and <=24-unit steps, then require an exact 16-unit descent
    # sequence from the platform to a base sector 48 or 64 units below it.
    parents = {sector_index: None}
    parent_lines = {}
    queue = collections.deque([sector_index])
    while queue:
        current = queue.popleft()
        for line_index in sector_line_indices[current]:
            line = lines[line_index]
            if ('sideback' not in line or line.get('blocking') == 'true' or
                    line.get('blockmonsters') == 'true'):
                continue
            front = int(sides[int(line['sidefront'])]['sector'])
            back = int(sides[int(line['sideback'])]['sector'])
            if current == front:
                neighbor = back
            elif current == back:
                neighbor = front
            else:
                continue
            if neighbor in parents:
                continue
            current_floor = float(sectors[current]['heightfloor'])
            neighbor_floor = float(sectors[neighbor]['heightfloor'])
            opening = (min(float(sectors[current]['heightceiling']),
                           float(sectors[neighbor]['heightceiling'])) -
                       max(current_floor, neighbor_floor))
            if opening < 56.0 or abs(current_floor - neighbor_floor) > 24.0:
                continue
            parents[neighbor] = current
            parent_lines[neighbor] = line_index
            queue.append(neighbor)

    perch_floor = float(sectors[sector_index]['heightfloor'])
    entry_line = None
    base_candidates = [] if surrounding_floor is None else [
        candidate for candidate in parents
        if abs(float(sectors[candidate]['heightfloor']) - surrounding_floor) <= 0.01]
    if not base_candidates:
        errors.append(f'perch sector id {sector_id} has no traversable stair descent')
    else:
        base = base_candidates[0]
        path = []
        current = base
        while current is not None:
            path.append(current)
            current = parents[current]
        path.reverse()
        path_floors = [float(sectors[index]['heightfloor']) for index in path]
        base_floor = path_floors[-1]
        rise = perch_floor - base_floor
        expected_floors = [perch_floor - 16.0 * step
                           for step in range(int(round(rise / 16.0)) + 1)]
        if (min(abs(rise - expected) for expected in (48.0, 64.0)) > 0.01 or
                abs(rise / 16.0 - round(rise / 16.0)) > 0.001 or
                len(path_floors) != len(expected_floors) or
                any(abs(actual - expected) > 0.01
                    for actual, expected in zip(path_floors, expected_floors))):
            errors.append(f'perch sector id {sector_id} lacks a continuous 16-unit stair sequence')
        if len(path) - 2 < 2:
            errors.append(f'perch sector id {sector_id} has fewer than two intermediate stair tiers')
        if any(lines[parent_lines[node]].get('blockmonsters') == 'true'
               for node in path[1:]):
            errors.append(f'perch sector id {sector_id} blocks monsters on its stair route')
        if base in parent_lines:
            entry_line = lines[parent_lines[base]]
    if points:
        xs = [float(vertices[point]['x']) for point in points]
        ys = [float(vertices[point]['y']) for point in points]
        footprint = tuple(sorted((round(max(xs) - min(xs)), round(max(ys) - min(ys)))))
        perch_footprints.add(footprint)
        if footprint not in {(112, 112), (120, 120), (96, 144)}:
            errors.append(f'perch sector id {sector_id} has unsupported footprint {footprint}')
        if len(open_edges) == 1 and entry_line is not None:
            def line_axis(line):
                first = vertices[int(line['v1'])]
                second = vertices[int(line['v2'])]
                if abs(float(first['y']) - float(second['y'])) <= 0.01:
                    return 'horizontal'
                if abs(float(first['x']) - float(second['x'])) <= 0.01:
                    return 'vertical'
                return 'diagonal'

            opening = open_edges[0]
            opening_axis = line_axis(opening)
            entry_axis = line_axis(entry_line)
            first = vertices[int(opening['v1'])]
            second = vertices[int(opening['v2'])]
            if opening_axis == 'horizontal':
                opening_offset = abs((float(first['x']) + float(second['x'])) * 0.5 -
                                     (min(xs) + max(xs)) * 0.5)
            else:
                opening_offset = abs((float(first['y']) + float(second['y'])) * 0.5 -
                                     (min(ys) + max(ys)) * 0.5)
            if footprint == (112, 112):
                perch_approaches.add('straight')
                if opening_axis != entry_axis or opening_offset > 0.1:
                    errors.append(f'square perch sector id {sector_id} lacks its '
                                  'centered straight stair')
            elif footprint == (120, 120):
                perch_approaches.add('offset')
                if opening_axis != entry_axis or opening_offset < 7.9:
                    errors.append(f'chamfered perch sector id {sector_id} lacks its '
                                  'offset stair')
            elif footprint == (96, 144):
                perch_approaches.add('dogleg')
                if opening_axis == entry_axis or 'diagonal' in {opening_axis, entry_axis}:
                    errors.append(f'wall-backed perch sector id {sector_id} lacks its '
                                  'perpendicular dogleg stair')
        if not any(thing.get('type') in monster_types and
                   min(xs) < float(thing['x']) < max(xs) and
                   min(ys) < float(thing['y']) < max(ys)
                   for thing in things):
            errors.append(f'perch sector id {sector_id} contains no ranged monster')
if len(perch_sector_ids) >= 2 and len(perch_footprints) < 2:
    errors.append('all elevated ranged-monster areas use the same architecture')

lift_sector_ids = [sector_id for sector_id in sector_ids if 3000 <= sector_id < 4000]
# Lifts are optional scenery, not a progression requirement. Compact or
# circulation-dense runs can correctly have no cell that preserves the full
# bypass ring; validate every lift that is emitted below rather than rejecting
# the safe zero-lift fallback.
pickup_types = {'17', '2007', '2008', '2010', '2011', '2012', '2014', '2015',
                '2018', '2019', '2046', '2047', '2048', '2049'}
for sector_id in lift_sector_ids:
    sector_index = sector_ids[sector_id]
    boundary = []
    adjacent = set()
    points = set()
    for line_index in sector_line_indices[sector_index]:
        line = lines[line_index]
        line_sectors = []
        for side_name in ('sidefront', 'sideback'):
            if side_name in line:
                line_sectors.append(int(sides[int(line[side_name])]['sector']))
        if sector_index not in line_sectors:
            continue
        boundary.append(line)
        points.add(int(line['v1']))
        points.add(int(line['v2']))
        adjacent.update(candidate for candidate in line_sectors if candidate != sector_index)
    if len(boundary) != 4:
        errors.append(f'lift sector id {sector_id} has {len(boundary)} edges instead of four')
    for line in boundary:
        if (line.get('special') != '62' or line.get('playeruse') != 'true' or
                line.get('repeatspecial') != 'true' or line.get('blockmonsters') != 'true'):
            errors.append(f'lift sector id {sector_id} has a non-operable perimeter edge')
            break
        if (line.get('arg0') != str(sector_id) or line.get('arg1') != '16' or
                line.get('arg2') != '105'):
            errors.append(f'lift sector id {sector_id} has invalid Plat_DownWaitUpStay arguments')
            break
    if len(adjacent) != 1:
        errors.append(f'lift sector id {sector_id} does not belong to one coherent room')
        continue
    surrounding_index = next(iter(adjacent))
    lift_floor = float(sectors[sector_index]['heightfloor'])
    surrounding_floor = float(sectors[surrounding_index]['heightfloor'])
    if abs((lift_floor - surrounding_floor) - 32.0) > 0.01:
        errors.append(f'lift sector id {sector_id} is not raised exactly 32 units')
    if float(sectors[sector_index]['heightceiling']) - lift_floor < 64.0:
        errors.append(f'lift sector id {sector_id} has insufficient raised-state headroom')
    if points:
        xs = [float(vertices[point]['x']) for point in points]
        ys = [float(vertices[point]['y']) for point in points]
        if abs((max(xs) - min(xs)) - 80.0) > 0.01 or abs((max(ys) - min(ys)) - 80.0) > 0.01:
            errors.append(f'lift sector id {sector_id} is not an 80-unit square')
        if not any(thing.get('type') in pickup_types and
                   min(xs) < float(thing['x']) < max(xs) and
                   min(ys) < float(thing['y']) < max(ys)
                   for thing in things):
            errors.append(f'lift sector id {sector_id} contains no visible reward')
        samples = [
            (min(xs), (min(ys) + max(ys)) * 0.5),
            (max(xs), (min(ys) + max(ys)) * 0.5),
            ((min(xs) + max(xs)) * 0.5, min(ys)),
            ((min(xs) + max(xs)) * 0.5, max(ys)),
            (min(xs), min(ys)), (max(xs), min(ys)),
            (min(xs), max(ys)), (max(xs), max(ys)),
        ]
        clearance = min((point_segment_distance(px, py, *wall)
                         for px, py in samples
                         for wall in nearby_solid_walls(px, py, 96.0)), default=96.0)
        if clearance < 95.9:
            errors.append(f'lift sector id {sector_id} leaves only {clearance:.1f} units '
                          'of bypass clearance')

exits = [line for line in lines if line.get('special') == '243']
if len(exits) != 1 or exits[0].get('playercross') != 'true':
    errors.append('exit trigger is missing explicit player-cross activation')
if len(exits) == 1:
    exit_sector_index = int(sides[int(exits[0]['sidefront'])]['sector'])
    if sectors[exit_sector_index].get('texturefloor') != 'GATE1':
        errors.append('exit trigger is not placed on the distinctive GATE1 pad')
    exit_borders = 0
    for line_index in sector_line_indices[exit_sector_index]:
        line = lines[line_index]
        for side_name in ('sidefront', 'sideback'):
            if side_name not in line:
                continue
            side = sides[int(line[side_name])]
            if int(side['sector']) == exit_sector_index and (
                    side.get('texturetop') == 'EXITDOOR' or
                    side.get('texturebottom') == 'EXITDOOR'):
                exit_borders += 1
                break
    if exit_borders < 4:
        errors.append(f'exit pad has only {exit_borders} EXITDOOR border segments')
    inner_floor = float(sectors[exit_sector_index]['heightfloor'])
    outer_candidates = [candidate for candidate in adjacency[exit_sector_index]
                        if abs(inner_floor - float(sectors[candidate]['heightfloor']) - 8.0) < 0.01]
    if len(outer_candidates) != 1:
        errors.append('exit pad does not descend through one coherent 8-unit outer stair tier')
    else:
        outer_sector_index = outer_candidates[0]
        outer_floor = float(sectors[outer_sector_index]['heightfloor'])
        base_candidates = [candidate for candidate in adjacency[outer_sector_index]
                           if candidate != exit_sector_index and
                           abs(outer_floor - float(sectors[candidate]['heightfloor']) - 8.0) < 0.01]
        if len(base_candidates) != 1:
            errors.append('exit stair does not descend through a second coherent 8-unit tier')
        inner_edges = 0
        outer_edges = 0
        candidate_line_indices = (sector_line_indices[exit_sector_index] |
                                  sector_line_indices[outer_sector_index])
        for line_index in candidate_line_indices:
            line = lines[line_index]
            if 'sideback' not in line:
                continue
            pair = {int(sides[int(line['sidefront'])]['sector']),
                    int(sides[int(line['sideback'])]['sector'])}
            if pair == {exit_sector_index, outer_sector_index}:
                inner_edges += 1
            elif base_candidates and pair == {outer_sector_index, base_candidates[0]}:
                outer_edges += 1
        if inner_edges != 4 or outer_edges != 4:
            errors.append(f'exit stair perimeter is incomplete '
                          f'(inner={inner_edges}, outer={outer_edges})')
if any(line.get('special') in ('1', '13') for line in lines):
    errors.append('obsolete polyobject/locked-door special remains in generated output')
sky_sectors = [sector for sector in sectors if sector.get('textureceiling') == 'F_SKY1']
if len(sky_sectors) < 2:
    errors.append(f'map has only {len(sky_sectors)} open sky sectors')
large_sky_courtyard = False
for sector_index, sector in enumerate(sectors):
    if sector.get('textureceiling') != 'F_SKY1':
        continue
    if int(sector.get('lightlevel', '0')) < 192:
        errors.append(f'sky sector {sector_index} is too dark to read as outdoors')
    points = set()
    for line_index in sector_line_indices[sector_index]:
        line = lines[line_index]
        for side_name in ('sidefront', 'sideback'):
            if side_name in line and int(sides[int(line[side_name])]['sector']) == sector_index:
                points.add(int(line['v1']))
                points.add(int(line['v2']))
    if points:
        xs = [float(vertices[point]['x']) for point in points]
        ys = [float(vertices[point]['y']) for point in points]
        if max(xs) - min(xs) >= 400.0 or max(ys) - min(ys) >= 400.0:
            large_sky_courtyard = True
if not large_sky_courtyard:
    errors.append('map has no room-scale outdoor courtyard')
if any(int(sector.get('lightlevel', '0')) < 160 for sector in sectors):
    errors.append('map contains a sector below the minimum readable light level')
secret_sector_indices = [index for index, sector in enumerate(sectors)
                         if sector.get('special') == '1024']
if any(sector.get('special') == '9' for sector in sectors):
    errors.append('map still uses untranslated Doom special 9 instead of SECRET_MASK')
if not secret_sector_indices:
    errors.append('map contains no real SECRET_MASK reward sector')
expected_secrets = (1 if size <= 1 else 2 if size <= 4 else min(
    8, 2 + size // 2,
    3 + size // 3 + (1 if detail >= 1 else 0) + (1 if detail == 2 else 0)))
if len(secret_sector_indices) < expected_secrets:
    errors.append(f'map contains only {len(secret_sector_indices)} secrets; '
                  f'expected at least {expected_secrets} for size {size}')
if not any(line.get('special') == '12' and line.get('secret') == 'true' for line in lines):
    errors.append('map contains no wall-aligned secret door')
secret_door_sectors = {
    sector_index for sector_index, faces in door_sectors.items()
    if faces and all(face.get('secret') == 'true' for face in faces)
}
secret_door_sectors.update(
    sector_index for sector_id, sector_index in sector_ids.items()
    if 1500 <= sector_id < 2000)
for line in lines:
    if line.get('secret') != 'true' or 'sideback' not in line:
        continue
    for side_name in ('sidefront', 'sideback'):
        candidate = int(sides[int(line[side_name])]['sector'])
        if (sectors[candidate]['heightfloor'] == sectors[candidate]['heightceiling']):
            secret_door_sectors.add(candidate)
for secret_sector in secret_sector_indices:
    directly_hidden = bool(adjacency[secret_sector] & secret_door_sectors)
    recessed_hidden = any(adjacency[approach] & secret_door_sectors
                          for approach in adjacency[secret_sector])
    if not directly_hidden and not recessed_hidden:
        errors.append(f'secret sector {secret_sector} is not behind its hidden door/lintel')

sector_ray_edges = collections.defaultdict(list)
for line in lines:
    front = int(sides[int(line['sidefront'])]['sector'])
    back = (int(sides[int(line['sideback'])]['sector'])
            if 'sideback' in line else -1)
    if front == back:
        continue
    first = vertices[int(line['v1'])]
    second = vertices[int(line['v2'])]
    edge = (float(first['x']), float(first['y']),
            float(second['x']), float(second['y']))
    sector_ray_edges[front].append(edge)
    if back >= 0:
        sector_ray_edges[back].append(edge)

def point_in_sector(px, py, sector_index):
    inside = False
    for x1, y1, x2, y2 in sector_ray_edges[sector_index]:
        if (y1 > py) != (y2 > py):
            intersection = x1 + (py - y1) * (x2 - x1) / (y2 - y1)
            if px < intersection:
                inside = not inside
    return inside

liquid_flats = {'FWATER1', 'BLOOD1', 'NUKAGE1', 'LAVA1'}
liquid_sector_indices = [index for index, sector in enumerate(sectors)
                         if sector.get('texturefloor') in liquid_flats]
if not liquid_sector_indices:
    errors.append('map contains no animated fluid sector')
paired_liquid_groups = collections.Counter()
liquid_spans = []
liquid_areas = []
natural_liquid_sectors = 0
for sector_index in liquid_sector_indices:
    sector = sectors[sector_index]
    flat = sector['texturefloor']
    neighbors = adjacency[sector_index]
    if not neighbors:
        errors.append(f'fluid sector {sector_index} borders no dry sector')
        continue
    flooded_room = len(neighbors) > 1
    dry_sector_index = min(
        neighbors,
        key=lambda neighbor: abs(
            float(sectors[neighbor]['heightfloor']) - float(sector['heightfloor'])))
    if not flooded_room:
        paired_liquid_groups[(dry_sector_index, flat)] += 1
    floor_drops = [float(sectors[neighbor]['heightfloor']) -
                   float(sector['heightfloor']) for neighbor in neighbors]
    expected_drop = 16.0 if flat in {'NUKAGE1', 'LAVA1'} else 8.0
    if min(abs(drop - expected_drop) for drop in floor_drops) > 0.01:
        errors.append(f'{flat} sector {sector_index} has no dry bank at its '
                      f'{expected_drop:.0f}-unit step')
    if flat in {'FWATER1', 'BLOOD1'}:
        if int(sector.get('damageamount', '0')) != 0:
            errors.append(f'harmless {flat} sector {sector_index} deals damage')
    elif flat == 'NUKAGE1':
        if (sector.get('damageamount') != '5' or
                sector.get('damageinterval') != '32' or
                sector.get('damagetype') != 'Slime' or
                int(sector.get('leakiness', '0')) != 0 or
                sector.get('damageterraineffect') == 'true'):
            errors.append(f'nukage sector {sector_index} has non-classic damage metadata')
    else:
        if (sector.get('damageamount') != '5' or
                sector.get('damageinterval') != '16' or
                sector.get('damagetype') != 'Fire' or
                sector.get('leakiness') != '256' or
                sector.get('damageterraineffect') != 'true'):
            errors.append(f'lava sector {sector_index} has non-classic damage metadata')

    boundary_points = set()
    for line_index in sector_line_indices[sector_index]:
        line = lines[line_index]
        boundary_points.add(int(line['v1']))
        boundary_points.add(int(line['v2']))
    fluid_thing_candidates = things
    if boundary_points:
        xs = [float(vertices[point]['x']) for point in boundary_points]
        ys = [float(vertices[point]['y']) for point in boundary_points]
        min_bucket_x = math.floor(min(xs) / spatial_bucket_size)
        max_bucket_x = math.floor(max(xs) / spatial_bucket_size)
        min_bucket_y = math.floor(min(ys) / spatial_bucket_size)
        max_bucket_y = math.floor(max(ys) / spatial_bucket_size)
        fluid_thing_candidates = [
            thing
            for bucket_x in range(min_bucket_x, max_bucket_x + 1)
            for bucket_y in range(min_bucket_y, max_bucket_y + 1)
            for thing in thing_buckets.get((bucket_x, bucket_y), ())
        ]
        footprint = tuple(sorted((round(max(xs) - min(xs)),
                                  round(max(ys) - min(ys)))))
        liquid_spans.append(max(footprint))
        if len(boundary_points) < 6:
            errors.append(f'fluid sector {sector_index} has only '
                          f'{len(boundary_points)} bank vertices')
        if (len(boundary_points) >= 10 or
                (len(boundary_points) >= 6 and max(footprint) >= 300)):
            natural_liquid_sectors += 1
        liquid_area = sector_areas[sector_index]
        liquid_areas.append(liquid_area)
        if liquid_area < 2400.0:
            errors.append(f'fluid sector {sector_index} is only '
                          f'{liquid_area:.0f} square units')
    bank_clearances = [] if flooded_room else sorted(
        min((point_segment_distance(float(vertices[point]['x']),
                                    float(vertices[point]['y']), *wall)
             for wall in nearby_solid_walls(float(vertices[point]['x']),
                                            float(vertices[point]['y']), 128.0)),
            default=128.0)
        for point in boundary_points)
    # Causeways and structural islands intentionally approach a local bank.
    # Reject intersections, then require the median bank vertex to retain the
    # full 64-unit dry route instead of demanding 64 units around every bridge
    # abutment and thereby outlawing traversal-shaping liquid architecture.
    if not flooded_room and bank_clearances and bank_clearances[0] < 0.1:
        errors.append(f'fluid sector {sector_index} intersects solid architecture')
    median_clearance = (bank_clearances[len(bank_clearances) // 2]
                        if bank_clearances else 128.0)
    if not flooded_room and median_clearance < 63.9:
        errors.append(f'fluid sector {sector_index} has only {median_clearance:.1f} '
                      'units of median dry-bank circulation')
    if any(point_in_sector(float(thing['x']), float(thing['y']), sector_index)
           for thing in fluid_thing_candidates):
        errors.append(f'fluid sector {sector_index} contains an initial actor or pickup')
for (dry_sector_index, flat), count in paired_liquid_groups.items():
    if count > 1 and count != 2:
        errors.append(f'paired {flat} basin in dry sector {dry_sector_index} has '
                      f'{count} pools instead of two')
if size >= 3 and liquid_spans and max(liquid_spans) < 300.0:
    errors.append(f'map has no room-scale liquid area (largest span={max(liquid_spans):.0f})')
if size >= 5 and liquid_spans and max(liquid_spans) < 500.0:
    errors.append(f'large map has no multi-cell liquid area '
                  f'(largest span={max(liquid_spans):.0f})')
if size >= 3 and natural_liquid_sectors == 0:
    errors.append('map has no liquid area with a natural multi-segment bank')

# Fluids are macro composition, not isolated decoration. Measure their share of
# the walkable floor plan and compare the dominant watercourse/reservoir against
# the median gameplay sector. Concentric inset sectors partition floor area, so
# shoelace areas remain additive here rather than double-counting bounding boxes.
playable_sector_indices = [
    index for index, sector in enumerate(sectors)
    if float(sector['heightceiling']) > float(sector['heightfloor']) and
    sector_areas[index] > 0.01
]
playable_area = sum(sector_areas[index] for index in playable_sector_indices)
liquid_area_total = sum(sector_areas[index] for index in liquid_sector_indices)
if size >= 3 and playable_area > 0.0:
    liquid_share = liquid_area_total / playable_area
    minimum_liquid_share = 0.03 if size >= 20 else (0.035 if size < 5 else 0.045)
    if liquid_share < minimum_liquid_share:
        errors.append(f'liquid architecture occupies only {liquid_share:.1%} of the '
                      f'playable floor plan (expected at least {minimum_liquid_share:.1%})')

# Room-scale sectors must demonstrate real dimensional composition, not merely
# repeat one coarse square with different textures. Exclude tagged mechanisms
# and liquids, then compare the bounding dimensions of playable large sectors.
room_scale_shapes = []
for sector_index, edges in sector_ray_edges.items():
    if sector_index >= len(sectors) or not edges:
        continue
    sector = sectors[sector_index]
    if 'id' in sector or sector.get('texturefloor') in liquid_flats:
        continue
    xs = [coordinate for x1, _, x2, _ in edges for coordinate in (x1, x2)]
    ys = [coordinate for _, y1, _, y2 in edges for coordinate in (y1, y2)]
    width = max(xs) - min(xs)
    height = max(ys) - min(ys)
    if width >= 180.0 and height >= 180.0:
        room_scale_shapes.append((width, height))
if size >= 3:
    distinct_shapes = {
        (round(width / 16.0), round(height / 16.0))
        for width, height in room_scale_shapes
    }
    aspect_ratios = [max(width, height) / min(width, height)
                     for width, height in room_scale_shapes]
    if len(room_scale_shapes) < 6:
        errors.append(f'map has only {len(room_scale_shapes)} room-scale sectors')
    elif len(distinct_shapes) < 5:
        errors.append(f'room-scale sectors use only {len(distinct_shapes)} dimensions')
    if aspect_ratios and max(aspect_ratios) < 1.45:
        errors.append('map contains no clearly elongated room-scale sector')

# Doom-style hierarchy needs several legible scales in the floor plan. Ignore
# mechanisms, liquids, and tiny trim rings; compare actual polygon area rather
# than grid-cell counts or bounding boxes so L/T/compound rooms are represented
# faithfully. The thresholds deliberately describe a broad distribution instead
# of asking for arbitrary decorative subdivision.
meaningful_sector_areas = sorted(
    sector_areas[index] for index in playable_sector_indices
    if 'id' not in sectors[index] and
    sectors[index].get('texturefloor') not in liquid_flats and
    sector_areas[index] >= 8192.0
)
if size >= 3:
    if len(meaningful_sector_areas) < 12:
        errors.append(f'map has only {len(meaningful_sector_areas)} meaningful-scale sectors')
    else:
        median_area = statistics.median(meaningful_sector_areas)
        p90_index = max(0, math.ceil(len(meaningful_sector_areas) * 0.9) - 1)
        p90_area = meaningful_sector_areas[p90_index]
        # 128x128 and 256x256 are useful classic-Doom scale landmarks. Their
        # areas divide connectors/small chambers, ordinary rooms, and major
        # halls without making the classification depend on whichever band
        # happens to contain the median for a particular settings profile.
        scale_bands = (
            sum(area < 16384.0 for area in meaningful_sector_areas),
            sum(16384.0 <= area < 65536.0
                for area in meaningful_sector_areas),
            sum(area >= 65536.0 for area in meaningful_sector_areas),
        )
        minimum_band = max(2, math.ceil(len(meaningful_sector_areas) * 0.05))
        if min(scale_bands) < minimum_band:
            errors.append(f'sector area hierarchy lacks a small/medium/large band '
                          f'(counts={scale_bands}, minimum={minimum_band})')
        if meaningful_sector_areas[-1] < median_area * 5.0:
            errors.append('largest gameplay sector is less than five times the median area')
        if p90_area < median_area * 2.0:
            errors.append('upper gameplay-sector scale is too close to the median')
        if liquid_areas and max(liquid_areas) < median_area * 2.75:
            errors.append('largest liquid feature is not macro-scale relative to ordinary sectors')
    if boundary_non45_lines < max(8, size * 2):
        errors.append(f'map has only {boundary_non45_lines} non-45-degree silhouette lines')
    if len(boundary_lengths) < 12:
        errors.append(f'map silhouette uses only {len(boundary_lengths)} distinct wall lengths')

# Raised sill sectors connect two otherwise separate rooms visually. Their
# monster-blocking aperture and 48-unit step preserve progression while creating
# previews and cross-room sightlines.
sightline_sectors = []
for sector_index in playable_sector_indices:
    sector = sectors[sector_index]
    if 'id' in sector or len(adjacency[sector_index]) < 2:
        continue
    neighbor_floors = [float(sectors[neighbor]['heightfloor'])
                       for neighbor in adjacency[sector_index]
                       if float(sectors[neighbor]['heightceiling']) >
                       float(sectors[neighbor]['heightfloor'])]
    blocking_apertures = sum(
        lines[line_index].get('blockmonsters') == 'true' and
        'sideback' in lines[line_index]
        for line_index in sector_line_indices[sector_index])
    if (len(neighbor_floors) >= 2 and
            float(sector['heightfloor']) - min(neighbor_floors) >= 47.9 and
            float(sector['heightceiling']) - float(sector['heightfloor']) >= 63.9 and
            blocking_apertures >= 2):
        sightline_sectors.append(sector_index)
if size >= 5 and not sightline_sectors:
    errors.append('map contains no raised cross-room sightline window')
powerup_types = {'8', '83', '2013', '2022', '2023', '2024', '2026', '2045'}
powerups = [thing for thing in things if thing.get('type') in powerup_types]
if not any(thing.get('type') == '8' for thing in powerups):
    errors.append('map contains no progression backpack reward')
if not any(thing.get('type') == '2024' for thing in powerups):
    errors.append('map contains no partial-invisibility reward')
if size >= 4 and not any(thing.get('type') == '2023' for thing in powerups):
    errors.append('map contains no midgame berserk reward')
if size >= 5 and not any(thing.get('type') == '2013' for thing in powerups):
    errors.append('map contains no deep soul-sphere reward')
if size >= 8 and not any(thing.get('type') == '2026' for thing in powerups):
    errors.append('map contains no exploratory computer-map reward')
if secret_sector_indices and not any(
        any(point_in_sector(float(thing['x']), float(thing['y']), sector_index)
            for sector_index in secret_sector_indices)
        for thing in powerups):
    errors.append('no powerup is physically placed inside a counted secret sector')
secret_reward_types = powerup_types | {
    '17', '82', '2001', '2002', '2003', '2004', '2005', '2006', '2007',
    '2008', '2010', '2011', '2012', '2014', '2015', '2018', '2019',
    '2046', '2047', '2048', '2049',
}
for sector_index in secret_sector_indices:
    if not any(thing.get('type') in secret_reward_types and
               point_in_sector(float(thing['x']), float(thing['y']), sector_index)
               for thing in things):
        errors.append(f'counted secret sector {sector_index} contains no tangible reward')
if silhouette_wall_count and boundary_diagonal_lines / silhouette_wall_count < 0.12:
    errors.append('map silhouette has too few diagonal linedefs to break up the coarse grid')
clear_heights = {
    round(float(sector['heightceiling']) - float(sector['heightfloor']))
    for sector in sectors
    if float(sector['heightceiling']) > float(sector['heightfloor'])
}
if len(boundary_lengths) < 8:
    errors.append(f'room geometry has only {len(boundary_lengths)} distinct boundary lengths')
if len(clear_heights) < 6:
    errors.append(f'room geometry has only {len(clear_heights)} distinct clear heights')
starts = [thing for thing in things if thing.get('type') == '1']
shotguns = [thing for thing in things if thing.get('type') == '2001']
if len(starts) == 1:
    start = starts[0]
    sx, sy = float(start['x']), float(start['y'])
    if abs(sx) > 24000.0 or abs(sy) > 24000.0:
        errors.append(f'player start ({sx:.1f}, {sy:.1f}) is too close to the UDMF coordinate edge')
    angle = math.radians(float(start.get('angle', '0')))
    nearby = []
    for thing in shotguns:
        dx, dy = float(thing['x']) - sx, float(thing['y']) - sy
        nearby.append((math.hypot(dx, dy), dx * math.cos(angle) + dy * math.sin(angle)))
    if not any(distance <= 40.0 and forward > 0.0 for distance, forward in nearby):
        errors.append('guaranteed start shotgun is not directly ahead of the player')
    start_clearance = min((point_segment_distance(sx, sy, *wall)
                           for wall in nearby_solid_walls(sx, sy, 160.0)), default=160.0)
    if start_clearance < 159.9:
        errors.append(f'player start has only {start_clearance:.1f} units of wall clearance')
if any(thing.get('type') == '64' for thing in things):
    errors.append('random Arch-Vile placement bypasses the encounter roster budget')
if any(thing.get('type') == '7' for thing in things):
    errors.append('Spider Mastermind placement exceeds the coarse-cell clearance contract')
for boss in (thing for thing in things if thing.get('type') == '16'):
    bx, by = float(boss['x']), float(boss['y'])
    clearance = min((point_segment_distance(bx, by, *wall)
                     for wall in nearby_solid_walls(bx, by, 144.0)), default=144.0)
    if clearance < 144.0:
        errors.append(f'Cyberdemon has only {clearance:.1f} units of wall clearance')
decoration_types = {'15', '20', '35', '41', '43', '44', '45', '46', '48',
                    '55', '56', '57', '59', '60', '85', '86', '2028', '2035'}
solid_decoration_types = decoration_types - {'15', '20', '59', '60'}
decorations = [thing for thing in things if thing.get('type') in decoration_types]
if len(decorations) < 2:
    errors.append('map contains too few role-aware decorative things')
gameplay_things = [thing for thing in things if thing.get('type') not in decoration_types]
gameplay_thing_buckets = collections.defaultdict(list)
for thing in gameplay_things:
    thing_x, thing_y = float(thing['x']), float(thing['y'])
    gameplay_thing_buckets[(math.floor(thing_x / spatial_bucket_size),
                            math.floor(thing_y / spatial_bucket_size))].append(
        (thing_x, thing_y))
for decoration in decorations:
    if decoration.get('type') not in solid_decoration_types:
        continue
    x, y = float(decoration['x']), float(decoration['y'])
    bucket_x = math.floor(x / spatial_bucket_size)
    bucket_y = math.floor(y / spatial_bucket_size)
    nearby_gameplay = (
        position
        for offset_x in (-1, 0, 1)
        for offset_y in (-1, 0, 1)
        for position in gameplay_thing_buckets.get(
            (bucket_x + offset_x, bucket_y + offset_y), ()))
    if any(math.hypot(thing_x - x, thing_y - y) < 36.0
           for thing_x, thing_y in nearby_gameplay):
        errors.append('solid decoration obstructs a gameplay actor or pickup')
        break

# The generic actor/pickup separation above catches overlap. Critical route
# anchors need a little more room than that: a player has to turn, collect a
# key, and operate a panel without a solid prop consuming the usable radius.
critical_navigation_types = {'1', '5', '6', '13', '2001'}
for anchor in (thing for thing in things if thing.get('type') in critical_navigation_types):
    anchor_x, anchor_y = float(anchor['x']), float(anchor['y'])
    nearest_prop = min((math.hypot(float(decoration['x']) - anchor_x,
                                   float(decoration['y']) - anchor_y)
                        for decoration in decorations
                        if decoration.get('type') in solid_decoration_types), default=40.0)
    if nearest_prop < 39.9:
        errors.append(f'critical navigation thing type {anchor.get("type")} has only '
                      f'{nearest_prop:.1f} units of solid-prop clearance')
        break

passage_regions = []
passage_buckets = collections.defaultdict(list)
for line_index, line in enumerate(lines):
    if 'sideback' not in line or line.get('blocking') == 'true':
        continue
    front_index = int(sides[int(line['sidefront'])]['sector'])
    back_index = int(sides[int(line['sideback'])]['sector'])
    if front_index == back_index:
        continue
    front, back = sectors[front_index], sectors[back_index]
    operable = line.get('special') in {'12', '62'}
    trim_textures = {
        sides[int(line['sidefront'])].get('texturetop'),
        sides[int(line['sidefront'])].get('texturebottom'),
        sides[int(line['sideback'])].get('texturetop'),
        sides[int(line['sideback'])].get('texturebottom'),
    }
    landmark_step = line.get('special', '0') == '0' and bool(
        trim_textures & {'STEP1', 'EXITDOOR'})
    approach_depth = 40.0 if landmark_step else 112.0
    aperture_margin = 12.0 if landmark_step else 28.0
    opening = (min(float(front['heightceiling']), float(back['heightceiling'])) -
               max(float(front['heightfloor']), float(back['heightfloor'])))
    floor_step = abs(float(front['heightfloor']) - float(back['heightfloor']))
    if not operable and (opening < 56.0 or floor_step > 24.0):
        continue
    first, second = vertices[int(line['v1'])], vertices[int(line['v2'])]
    ax, ay = float(first['x']), float(first['y'])
    dx = float(second['x']) - ax
    dy = float(second['y']) - ay
    length = math.hypot(dx, dy)
    if length < 0.001:
        continue
    region = (line_index, ax, ay, dx / length, dy / length, length,
              approach_depth, aperture_margin)
    region_index = len(passage_regions)
    passage_regions.append(region)
    extent = approach_depth + aperture_margin
    min_bucket_x = math.floor((min(ax, ax + dx) - extent) / spatial_bucket_size)
    max_bucket_x = math.floor((max(ax, ax + dx) + extent) / spatial_bucket_size)
    min_bucket_y = math.floor((min(ay, ay + dy) - extent) / spatial_bucket_size)
    max_bucket_y = math.floor((max(ay, ay + dy) + extent) / spatial_bucket_size)
    for bucket_x in range(min_bucket_x, max_bucket_x + 1):
        for bucket_y in range(min_bucket_y, max_bucket_y + 1):
            passage_buckets[(bucket_x, bucket_y)].append(region_index)

# A switch panel is not a sector portal, but it is a required manual waypoint.
# Give it the same serialized player-radius clearance audit as a doorway so a
# decorative column cannot make the only operation point unreachable.
for line_index, line in enumerate(lines):
    if line.get('special') != '11' or 'sideback' in line:
        continue
    first, second = vertices[int(line['v1'])], vertices[int(line['v2'])]
    ax, ay = float(first['x']), float(first['y'])
    dx = float(second['x']) - ax
    dy = float(second['y']) - ay
    length = math.hypot(dx, dy)
    if length < 0.001:
        continue
    approach_depth = 64.0
    aperture_margin = 28.0
    region = (line_index, ax, ay, dx / length, dy / length, length,
              approach_depth, aperture_margin)
    region_index = len(passage_regions)
    passage_regions.append(region)
    extent = approach_depth + aperture_margin
    min_bucket_x = math.floor((min(ax, ax + dx) - extent) / spatial_bucket_size)
    max_bucket_x = math.floor((max(ax, ax + dx) + extent) / spatial_bucket_size)
    min_bucket_y = math.floor((min(ay, ay + dy) - extent) / spatial_bucket_size)
    max_bucket_y = math.floor((max(ay, ay + dy) + extent) / spatial_bucket_size)
    for bucket_x in range(min_bucket_x, max_bucket_x + 1):
        for bucket_y in range(min_bucket_y, max_bucket_y + 1):
            passage_buckets[(bucket_x, bucket_y)].append(region_index)

passage_obstruction = None
for decoration in decorations:
    if decoration.get('type') not in solid_decoration_types:
        continue
    px, py = float(decoration['x']), float(decoration['y'])
    bucket = (math.floor(px / spatial_bucket_size),
              math.floor(py / spatial_bucket_size))
    for region_index in passage_buckets.get(bucket, ()):
        (line_index, ax, ay, unit_x, unit_y, length,
         approach_depth, aperture_margin) = passage_regions[region_index]
        relative_x, relative_y = px - ax, py - ay
        along = relative_x * unit_x + relative_y * unit_y
        normal = abs(relative_x * unit_y - relative_y * unit_x)
        if -aperture_margin <= along <= length + aperture_margin and normal <= approach_depth:
            passage_obstruction = (decoration.get('type'), line_index)
            break
    if passage_obstruction:
        break
if passage_obstruction:
    errors.append(f'solid decoration type {passage_obstruction[0]} obstructs '
                  f'passage, door, stair, lift, or manual-switch approach at linedef '
                  f'{passage_obstruction[1]}')

# Recovery bundles use a larger slot vocabulary than their maximum authored
# item count. Assert that a dense cache never folds back onto itself and hides
# several supplies at one coordinate.
distributed_pickup_types = {
	'5', '6', '8', '13', '17', '82', '83', '2001', '2002', '2003', '2004',
	'2005', '2006', '2007', '2008', '2010', '2011', '2012', '2013', '2014',
	'2015', '2018', '2019', '2022', '2023', '2024', '2026', '2045', '2046',
	'2047', '2048', '2049',
}
pickup_positions = collections.defaultdict(list)
for thing in things:
    if thing.get('type') in distributed_pickup_types:
        pickup_positions[(thing['x'], thing['y'])].append(thing['type'])
overlapping_pickups = [types for types in pickup_positions.values() if len(types) > 1]
if overlapping_pickups:
    errors.append(f'{len(overlapping_pickups)} item positions stack multiple survival pickups')

seen = set()
components = 0
for root in range(len(sectors)):
    if root in seen:
        continue
    components += 1
    seen.add(root)
    queue = [root]
    for current in queue:
        for neighbor in adjacency[current]:
            if neighbor not in seen:
                seen.add(neighbor)
                queue.append(neighbor)
if components != 1:
    errors.append(f'sector graph has {components} disconnected components')
if len(referenced) != len(sectors):
    errors.append(f'only {len(referenced)} of {len(sectors)} sectors are referenced')

for error in errors:
    print(f'    {error}')
sys.exit(1 if errors else 0)
PY
}

# Model the serialized sector graph as a player would: ordinary doors may be
# opened, but a keyed door sector cannot be entered or left until the matching
# key has been collected. This complements the lock-cut audit above by proving
# an actual inventory-state route from the player start to the exit.
validate_key_progression() {
	python3 - <<'PY'
import collections
import re
import sys

text = open('/tmp/procmap_test.udmf', encoding='utf-8').read()

def blocks(kind):
    return [dict((key, value.strip('"')) for key, value in
                 re.findall(r'^\s*(\w+)\s*=\s*([^;]+);', body, re.M))
            for body in re.findall(r'(?m)^' + kind + r'\s*\n\{(.*?)\n\}', text, re.S)]

vertices = blocks('vertex')
sectors = blocks('sector')
sides = blocks('sidedef')
lines = blocks('linedef')
things = blocks('thing')
errors = []

if not sectors or not sides or not lines:
    print('    symbolic key-state solver found no serialized map topology')
    raise SystemExit(1)

sector_edges = collections.defaultdict(list)
sector_rays = collections.defaultdict(list)
for line in lines:
    front = int(sides[int(line['sidefront'])]['sector'])
    back = (int(sides[int(line['sideback'])]['sector'])
            if 'sideback' in line else None)
    first = vertices[int(line['v1'])]
    second = vertices[int(line['v2'])]
    edge = (float(first['x']), float(first['y']),
            float(second['x']), float(second['y']))
    sector_rays[front].append(edge)
    if back is None:
        continue
    sector_rays[back].append(edge)
    if front != back:
        sector_edges[front].append(back)
        sector_edges[back].append(front)

def locate_sector(x, y):
    matches = []
    for sector_index in range(len(sectors)):
        inside = False
        for x1, y1, x2, y2 in sector_rays[sector_index]:
            if (y1 > y) != (y2 > y):
                intersection = x1 + (y - y1) * (x2 - x1) / (y2 - y1)
                if x < intersection:
                    inside = not inside
        if inside:
            matches.append(sector_index)
    return matches

# Door_Raise faces name the thin, closed door sector through sideback. Both
# faces must agree on their lock; unresolved/conflicting locks are a serialized
# map defect rather than a solver ambiguity.
door_locks = {}
for line in lines:
    if line.get('special') != '12' or 'sideback' not in line:
        continue
    lock = int(line.get('locknumber', '0'))
    if lock <= 0:
        continue
    door_sector = int(sides[int(line['sideback'])]['sector'])
    previous = door_locks.get(door_sector)
    if previous is not None and previous != lock:
        errors.append(f'keyed door sector {door_sector} has conflicting locks {previous}/{lock}')
    door_locks[door_sector] = lock

if not door_locks:
    errors.append('symbolic key-state solver found no keyed door sector')

key_to_lock = {'13': 1, '5': 2, '6': 3}
keys_at = collections.defaultdict(set)
for thing in things:
    lock = key_to_lock.get(thing.get('type'))
    if lock is None:
        continue
    matches = locate_sector(float(thing['x']), float(thing['y']))
    if len(matches) != 1:
        errors.append(f'key type {thing["type"]} at ({thing["x"]}, {thing["y"]}) '
                      f'belongs to {len(matches)} sectors')
        continue
    keys_at[matches[0]].add(lock)

cooperative_start_types = ('1', '2', '3', '4', '4001', '4002', '4003', '4004')
starts_by_type = {
    start_type: [thing for thing in things if thing.get('type') == start_type]
    for start_type in cooperative_start_types
}
for slot, start_type in enumerate(cooperative_start_types, 1):
    if len(starts_by_type[start_type]) != 1:
        errors.append(f'cooperative start P{slot} (thing type {start_type}) must appear exactly once, '
                      f'got {len(starts_by_type[start_type])}')
cooperative_starts = [starts_by_type[start_type][0]
                      for start_type in cooperative_start_types
                      if len(starts_by_type[start_type]) == 1]
cooperative_start_sectors = []
for slot, start in enumerate(cooperative_starts, 1):
    matches = locate_sector(float(start['x']), float(start['y']))
    if len(matches) != 1:
        errors.append(f'cooperative start P{slot} belongs to {len(matches)} sectors')
    else:
        cooperative_start_sectors.append(matches[0])
for first in range(len(cooperative_starts)):
    for second in range(first):
        dx = float(cooperative_starts[first]['x']) - float(cooperative_starts[second]['x'])
        dy = float(cooperative_starts[first]['y']) - float(cooperative_starts[second]['y'])
        if (dx * dx + dy * dy) ** 0.5 < 48.0 - 0.001:
            errors.append(f'cooperative starts P{second + 1}/P{first + 1} are closer than 48 units')
if cooperative_start_sectors and len(set(cooperative_start_sectors)) != 1:
    errors.append('cooperative starts are not all staged in one landmark sector')

# P1 remains the canonical single-player origin. The remaining native starts
# must never change this solver's initial state or key-route semantics.
player_starts = starts_by_type['1']
if len(player_starts) != 1:
    errors.append(f'symbolic key-state solver expected one canonical P1 start, got {len(player_starts)}')
    start_sector = None
else:
    start_matches = locate_sector(float(player_starts[0]['x']), float(player_starts[0]['y']))
    if len(start_matches) != 1:
        errors.append(f'player start belongs to {len(start_matches)} sectors')
        start_sector = None
    else:
        start_sector = start_matches[0]

exit_sectors = {
    int(sides[int(line['sidefront'])]['sector'])
    for line in lines if line.get('special') == '243'
}
if len(exit_sectors) != 1:
    errors.append(f'symbolic key-state solver expected one exit sector, got {len(exit_sectors)}')

required_locks = set(door_locks.values())
for lock in sorted(required_locks):
    if not any(lock in values for values in keys_at.values()):
        errors.append(f'keyed door lock {lock} has no matching reachable key thing')

if start_sector is not None and len(exit_sectors) == 1:
    start_inventory = frozenset(keys_at[start_sector])
    queue = collections.deque([(start_sector, start_inventory)])
    visited = {(start_sector, start_inventory)}
    reached_inventory = None
    while queue:
        sector, inventory = queue.popleft()
        if sector in exit_sectors and required_locks.issubset(inventory):
            reached_inventory = inventory
            break
        for neighbor in sector_edges[sector]:
            # Entering or leaving a keyed door sector requires the key. This
            # makes a closed slab an inventory gate while retaining normal
            # use-open doors and ordinary portals as traversable topology.
            locks = {door_locks[candidate]
                     for candidate in (sector, neighbor) if candidate in door_locks}
            if len(locks) > 1:
                errors.append(f'adjacent keyed door sectors {sector}/{neighbor} require different keys')
                continue
            if locks and next(iter(locks)) not in inventory:
                continue
            next_inventory = frozenset(set(inventory) | keys_at[neighbor])
            state = (neighbor, next_inventory)
            if state not in visited:
                visited.add(state)
                queue.append(state)
    if reached_inventory is None:
        errors.append('no serialized key-inventory route reaches the exit')
    elif not required_locks.issubset(reached_inventory):
        errors.append('exit route does not acquire every required keyed-door inventory item')

for error in errors:
    print(f'    {error}')
if errors:
    raise SystemExit(1)
print(f'  symbolic key-state route passed: start -> {len(required_locks)} key gate(s) -> exit')
PY
}

# The manifest is a developer-facing contract, not a loose debug log. Validate
# the complete planned run before comparing a corpus of runs, so an accidental
# JSON change cannot make the diversity test pass by silently dropping data.
validate_manifest() {
	local manifest_path=$1
	local size=$2
	local difficulty=$3
	local verticality=${4:-1}
	python3 - "$manifest_path" "$size" "$difficulty" "$verticality" <<'PY'
import json
import sys

path = sys.argv[1]
size = int(sys.argv[2])
difficulty = int(sys.argv[3])
verticality = int(sys.argv[4])

try:
    with open(path, encoding='utf-8') as handle:
        manifest = json.load(handle)
except (OSError, json.JSONDecodeError) as error:
    print(f'    could not parse run manifest {path}: {error}')
    raise SystemExit(1)

errors = []
if not isinstance(manifest, dict):
    errors.append('manifest root is not an object')

def expect(condition, message):
    if not condition:
        errors.append(message)

def finite_number(value):
    return (isinstance(value, (int, float)) and not isinstance(value, bool) and
            value == value and value not in (float('inf'), float('-inf')))

def optional_nonempty_string(record, field, context):
    if field in record:
        expect(isinstance(record[field], str) and record[field].strip(),
               f'{context} has an empty or non-string {field}')

expect(manifest.get('schema') == 1, f'manifest schema must be 1, got {manifest.get("schema")!r}')
expect(manifest.get('iwad_roster') in {'doom1', 'doom2'},
       f'manifest must identify its active Doom-family IWAD roster, got {manifest.get("iwad_roster")!r}')
expect(manifest.get('difficulty') == difficulty,
       f'manifest difficulty must be {difficulty}, got {manifest.get("difficulty")!r}')
profiles = {'expedition', 'assault', 'infiltration', 'circuit', 'siege'}
orientations = {'eastbound', 'westbound', 'northbound', 'southbound'}
motif_names = {
    'watercourse', 'vertical_pressure', 'remote_reveal', 'shrine_secrets',
    'sightline_recon',
}
tracks = {'ballistic', 'demolition', 'energy'}
finales = {'duel', 'siege', 'gauntlet', 'fortress'}
stage_shapes = {'spine', 'fork_rejoin', 'ring', 'switchback', 'courtyard_spokes'}
landmark_archetypes = {
    'none', 'court', 'nave', 'gatehouse', 'shrine_terrace', 'bridge_basin',
    'bastion', 'fortress',
}
district_roles = {'entry', 'transit', 'work', 'sanctum', 'defense', 'finale'}
vertical_intents = {
    'flat', 'stair_hall', 'dogleg_ascent', 'dogleg_descent',
    'terrace_overlook', 'bridge_approach',
}
elevation_roles = {'flat', 'terrace', 'highland', 'basin', 'overlook'}
room_footprints = {
    'safe_shell', 'asymmetric_octagon', 'tapered_bay', 'apse',
    'stepped_compound', 'courtyard_cut', 'fractured_wedge',
}
beats = {
    'none', 'opening', 'approach', 'key_objective', 'recovery', 'set_piece',
    'finale', 'optional',
}
cards = {
    'none', 'breather', 'skirmish', 'crossfire', 'pincer', 'ambush',
    'cache_challenge', 'holding_line', 'set_piece',
}
card_geometry_by_card = {
    'none': {'none'},
    'breather': {'recovery_pad', 'clear_floor'},
    'skirmish': {'open_floor'},
    'crossfire': {'opposed_lanes'},
    'pincer': {'dual_flank'},
    'ambush': {'staged_threshold'},
    'cache_challenge': {'reward_cache', 'switch_cache'},
    'holding_line': {'defense_line'},
    'set_piece': {'arena_footprint'},
}
rewards = {'none', 'emergency', 'cache', 'armory', 'key_reserve', 'finale_reserve'}
manual_interactions = {'none', 'keyed_door', 'switch_cache', 'secret_door'}
high_pressure_cards = {'crossfire', 'pincer', 'ambush', 'holding_line', 'set_piece'}

profile = manifest.get('profile')
orientation = manifest.get('orientation')
briefing = manifest.get('briefing')
expect(isinstance(profile, str) and profile.casefold() in profiles,
       f'unknown run profile {profile!r}')
expect(isinstance(orientation, str) and orientation.casefold() in orientations,
       f'unknown route orientation {orientation!r}')
expect(isinstance(briefing, str) and briefing.strip(), 'manifest briefing is empty')

key_order = manifest.get('key_order')
expect(isinstance(key_order, list) and len(key_order) == 3 and sorted(key_order) == [1, 2, 3],
       f'key_order must be a blue/red/yellow permutation, got {key_order!r}')

# The recipe plans a stage budget before topology is embedded, but a compact
# realized route can safely need fewer keyed crossings. Keep both values
# visible: the former explains the blueprint while the latter governs actual
# gate, stage, and accessibility assertions below.
expected_planned_stage_count = 2 if size <= 2 else (4 if size >= 5 else 3)
planned_stage_count = manifest.get('planned_stage_count')
realized_stage_count = manifest.get('realized_stage_count')
expect(isinstance(planned_stage_count, int) and
       planned_stage_count == expected_planned_stage_count,
       f'size {size} manifest must plan {expected_planned_stage_count} macro stages, '
       f'got {planned_stage_count!r}')
expect(isinstance(planned_stage_count, int) and
       isinstance(realized_stage_count, int) and
       1 <= realized_stage_count <= planned_stage_count,
       f'manifest realized_stage_count must be within its planned stage budget, '
       f'got planned={planned_stage_count!r} realized={realized_stage_count!r}')

# The generator proves collision-aware navigation after UDMF emission, rather
# than treating a connected room graph as sufficient. Keep this additive
# manifest summary strict: it makes a lost approach reservation or a disabled
# key-state proof visible to the replayability corpus immediately.
accessibility = manifest.get('accessibility')
required_accessibility_fields = {
    'status', 'mandatory_anchors', 'ordinary_cells',
    'navigation_reservations', 'keyed_door_approaches',
}
if not isinstance(accessibility, dict):
    errors.append('manifest accessibility is not an object')
else:
    missing = sorted(required_accessibility_fields - accessibility.keys())
    if missing:
        errors.append(f'manifest accessibility is missing {", ".join(missing)}')
    else:
        expect(accessibility['status'] == 'proven',
               f'accessibility proof status must be proven, got {accessibility["status"]!r}')
        for field in ('mandatory_anchors', 'ordinary_cells', 'navigation_reservations'):
            expect(isinstance(accessibility[field], int) and accessibility[field] > 0,
                   f'accessibility {field} must be a positive integer, '
                   f'got {accessibility[field]!r}')
        realized_keyed_gates = max(0, realized_stage_count - 1) \
            if isinstance(realized_stage_count, int) else 0
        expected_keyed_approaches = realized_keyed_gates * 2
        approaches = accessibility['keyed_door_approaches']
        expect(isinstance(approaches, int) and approaches >= 0,
               f'accessibility keyed_door_approaches must be a nonnegative integer, '
               f'got {approaches!r}')
        if expected_keyed_approaches:
            expect(approaches > 0 and approaches % 2 == 0,
                   f'accessibility keyed_door_approaches must be positive and even, '
                   f'got {approaches!r}')
            expect(approaches == expected_keyed_approaches,
                   f'accessibility keyed_door_approaches must cover both sides of '
                   f'{realized_keyed_gates} realized keyed gates '
                   f'({expected_keyed_approaches}), got {approaches!r}')

    collision_navigation = accessibility.get('collision_navigation')
    collision_fields = {
        'status', 'player_radius', 'safety_margin', 'pad_reservations',
        'corridor_reservations', 'reservation_count', 'proven_corridors',
        'key_state_edges', 'mandatory_anchor_count',
        'ordinary_cells_reached', 'ordinary_rewards_reached',
        'keyed_door_approaches_reached', 'manual_switches_reached',
        'physical_corridor_witnesses', 'room_merge_corridors',
        'switch_cache_actions', 'switch_cache_rewards_reached',
        'required_key_mask', 'exit_key_mask', 'anchors',
        'corridors', 'switch_caches', 'cooperative_starts',
    }
    if not isinstance(collision_navigation, dict):
        errors.append('accessibility collision_navigation is not an object')
    else:
        missing = sorted(collision_fields - collision_navigation.keys())
        if missing:
            errors.append('accessibility collision_navigation is missing ' + ', '.join(missing))
        else:
            expect(collision_navigation['status'] == 'proven',
                   'collision navigation proof status must be proven')
            for field in ('player_radius', 'safety_margin', 'pad_reservations',
                          'corridor_reservations', 'reservation_count',
                          'proven_corridors', 'key_state_edges',
                          'mandatory_anchor_count', 'ordinary_cells_reached',
                          'ordinary_rewards_reached',
                          'keyed_door_approaches_reached',
                          'manual_switches_reached',
                          'physical_corridor_witnesses',
                          'room_merge_corridors', 'switch_cache_actions',
                          'switch_cache_rewards_reached', 'required_key_mask',
                          'exit_key_mask'):
                expect(isinstance(collision_navigation[field], int) and
                       not isinstance(collision_navigation[field], bool) and
                       collision_navigation[field] >= 0,
                       f'collision navigation {field} must be a nonnegative integer, '
                       f'got {collision_navigation[field]!r}')
            expect(collision_navigation['player_radius'] >= 16 and
                   collision_navigation['safety_margin'] >= 16,
                   'collision navigation uses less than the 16-unit player/safety contract')
            if required_accessibility_fields.issubset(accessibility):
                expect(collision_navigation['pad_reservations'] +
                       collision_navigation['corridor_reservations'] ==
                       collision_navigation['reservation_count'] ==
                       accessibility['navigation_reservations'],
                       'collision navigation reservation counters do not reconcile')
                expect(collision_navigation['mandatory_anchor_count'] ==
                       accessibility['mandatory_anchors'],
                       'collision navigation mandatory-anchor count does not reconcile')
                expect(collision_navigation['ordinary_cells_reached'] ==
                       accessibility['ordinary_cells'],
                       'collision navigation ordinary-cell count does not reconcile')
                expect(collision_navigation['keyed_door_approaches_reached'] ==
                       accessibility['keyed_door_approaches'],
                       'collision navigation keyed-door approaches do not reconcile')
            expect(collision_navigation['proven_corridors'] > 0 and
                   collision_navigation['key_state_edges'] > 0,
                   'collision navigation has no proven emitted corridors or key-state edges')
            expect((collision_navigation['exit_key_mask'] &
                    collision_navigation['required_key_mask']) ==
                   collision_navigation['required_key_mask'],
                   'collision navigation exit state lacks a required key')
            expect(isinstance(collision_navigation['anchors'], list) and
                   collision_navigation['anchors'],
                   'collision navigation has no stable UDMF anchors')
            expect(isinstance(collision_navigation['corridors'], list) and
                   len(collision_navigation['corridors']) ==
                   collision_navigation['physical_corridor_witnesses'],
                   'collision navigation has inconsistent physical corridor witnesses')
            expect(isinstance(collision_navigation['switch_caches'], list) and
                   len(collision_navigation['switch_caches']) ==
                   collision_navigation['switch_cache_actions'],
                   'collision navigation has inconsistent switch-cache witnesses')
            cooperative_starts = collision_navigation['cooperative_starts']
            cooperative_fields = {
                'status', 'native_slots', 'canonical_player',
                'minimum_separation', 'starts',
            }
            if not isinstance(cooperative_starts, dict):
                errors.append('collision navigation cooperative_starts is not an object')
            else:
                missing_cooperative = sorted(cooperative_fields - cooperative_starts.keys())
                if missing_cooperative:
                    errors.append('collision navigation cooperative_starts is missing ' +
                                  ', '.join(missing_cooperative))
                else:
                    expect(cooperative_starts['status'] == 'proven',
                           'cooperative-start proof status must be proven')
                    expect(cooperative_starts['native_slots'] == 8,
                           'cooperative-start proof must cover native P1 through P8')
                    expect(cooperative_starts['canonical_player'] == 1,
                           'cooperative-start proof must retain P1 as canonical')
                    expect(finite_number(cooperative_starts['minimum_separation']) and
                           cooperative_starts['minimum_separation'] >= 48.0 - 0.001,
                           'cooperative starts violate the 48-unit separation contract')
                    starts = cooperative_starts['starts']
                    expect(isinstance(starts, list) and len(starts) == 8,
                           'cooperative-start proof must contain exactly eight starts')
                    expected_types = (1, 2, 3, 4, 4001, 4002, 4003, 4004)
                    if isinstance(starts, list) and len(starts) == 8:
                        for slot, start in enumerate(starts, 1):
                            fields = {'player', 'thing_index', 'thing_type', 'room',
                                      'landmark_sector', 'x', 'y', 'clear_radius'}
                            if not isinstance(start, dict) or fields - start.keys():
                                errors.append(f'cooperative start P{slot} has incomplete metadata')
                                continue
                            expect(start['player'] == slot and
                                   start['thing_type'] == expected_types[slot - 1],
                                   f'cooperative start P{slot} has the wrong native player type')
                            expect(all(isinstance(start[field], int) and
                                       not isinstance(start[field], bool)
                                       for field in ('thing_index', 'room', 'landmark_sector')),
                                   f'cooperative start P{slot} has invalid integer metadata')
                            expect(all(finite_number(start[field]) for field in
                                       ('x', 'y', 'clear_radius')) and
                                   start['clear_radius'] >= 48.0 - 0.001,
                                   f'cooperative start P{slot} has no conservative clear pad')

# Schema 1 is the shipping spatial-grammar contract.  It deliberately stays at
# schema 1, but current generators must not be allowed to silently drop the
# post-emission visual proof or connection witnesses and still pass an older
# generic-manifest check.
visual_proof = manifest.get('visual_proof')
if not isinstance(visual_proof, dict):
    errors.append('manifest visual_proof is not an object')
else:
    required_visual_proof_fields = {
        'status', 'alignment_groups', 'geometry', 'connector', 'elevation',
    }
    missing = sorted(required_visual_proof_fields - visual_proof.keys())
    if missing:
        errors.append(f'manifest visual_proof is missing {", ".join(missing)}')
    else:
        expect(visual_proof['status'] == 'proven',
               f'visual proof status must be proven, got {visual_proof["status"]!r}')
        for field in ('alignment_groups', 'geometry', 'connector', 'elevation'):
            witness = visual_proof[field]
            valid = ((isinstance(witness, bool)) or
                     (finite_number(witness) and witness >= 0) or
                     (isinstance(witness, str) and witness.strip()) or
                     (isinstance(witness, (list, tuple)) and len(witness) > 0) or
                     (isinstance(witness, dict) and len(witness) > 0))
            expect(valid, f'visual proof {field} has no usable witness')
            if visual_proof['status'] == 'proven' and isinstance(witness, str):
                expect(witness == 'proven',
                       f'visual proof {field} is inconsistent with proven status: {witness!r}')

        # The alignment proof keeps schema 1 but is no longer a bare group
        # count. It records active-IWAD metrics and final UDMF transform
        # witnesses so tests do not silently regress to a 128-unit assumption.
        alignment = visual_proof.get('alignment')
        required_alignment_fields = {
            'status', 'metric_textures', 'fallback_textures',
            'world_witnesses', 'two_sided_witnesses', 'stair_witnesses',
            'portal_witnesses', 'metrics', 'witnesses',
        }
        if not isinstance(alignment, dict):
            errors.append('visual proof alignment is not an object')
        else:
            missing = sorted(required_alignment_fields - alignment.keys())
            if missing:
                errors.append(f'visual proof alignment is missing {", ".join(missing)}')
            else:
                expect(alignment['status'] == 'proven',
                       f'visual proof alignment status must be proven, got {alignment["status"]!r}')
                for field in ('metric_textures', 'fallback_textures',
                              'world_witnesses', 'two_sided_witnesses',
                              'stair_witnesses', 'portal_witnesses'):
                    value = alignment[field]
                    expect(isinstance(value, int) and not isinstance(value, bool) and value >= 0,
                           f'visual proof alignment {field} must be a nonnegative integer, got {value!r}')
                metrics = alignment['metrics']
                witnesses = alignment['witnesses']
                expect(isinstance(metrics, list) and metrics,
                       'visual proof alignment has no active-IWAD metrics')
                expect(isinstance(witnesses, list) and witnesses,
                       'visual proof alignment has no UDMF phase witnesses')
                if isinstance(metrics, list):
                    expect(alignment['metric_textures'] == len(metrics),
                           'visual proof alignment metric_textures does not match metrics array')
                    fallback_count = 0
                    for metric_index, metric in enumerate(metrics):
                        if not isinstance(metric, dict):
                            errors.append(f'visual proof metric {metric_index} is not an object')
                            continue
                        expect(isinstance(metric.get('texture'), str) and metric['texture'],
                               f'visual proof metric {metric_index} has no texture name')
                        for field in ('width', 'height'):
                            expect(isinstance(metric.get(field), int) and metric[field] > 0,
                                   f'visual proof metric {metric_index} has invalid {field}')
                        expect(isinstance(metric.get('fallback'), bool),
                               f'visual proof metric {metric_index} has non-boolean fallback')
                        fallback_count += metric.get('fallback') is True
                    expect(alignment['fallback_textures'] == fallback_count,
                           'visual proof alignment fallback_textures does not match metrics array')

# Targets and their final realized extrema are separate schema-1 fields.  A
# constrained graph may take a smaller safe *within-band* scenic climb, but it
# may not advertise a highland/basin that its final room field never realizes.
main_elevation_target = manifest.get('main_route_elevation_target')
optional_elevation_target = manifest.get('optional_elevation_target')
realized_main_elevation = manifest.get('realized_main_route_elevation')
realized_optional_elevation = manifest.get('realized_optional_elevation')
terrain_fields = (
    ('main_route_elevation_target', main_elevation_target),
    ('optional_elevation_target', optional_elevation_target),
    ('realized_main_route_elevation', realized_main_elevation),
    ('realized_optional_elevation', realized_optional_elevation),
)
valid_terrain_fields = True
for field, value in terrain_fields:
    valid = isinstance(value, int) and not isinstance(value, bool) and value % 8 == 0
    expect(valid, f'manifest {field} must be an 8-unit integer, got {value!r}')
    valid_terrain_fields = valid_terrain_fields and valid
if verticality == 2 and size >= 3 and valid_terrain_fields:
    lower, upper = ((192, 320) if size >= 5 else (128, 192))
    expect(lower <= abs(main_elevation_target) <= upper,
           f'Dramatic main-route elevation target {main_elevation_target!r} '
           f'is outside {lower}..{upper}')
    expect(lower <= abs(realized_main_elevation) <= upper,
           f'Dramatic realized main-route elevation {realized_main_elevation!r} '
           f'is outside {lower}..{upper}')
    expect(main_elevation_target * realized_main_elevation > 0,
           'Dramatic realized main-route elevation reverses or loses its planned highland/basin side')
    if size >= 5:
        expect(optional_elevation_target != 0 and
               optional_elevation_target * main_elevation_target < 0,
               'large Dramatic optional elevation target is not opposite the main route')
        expect(lower <= abs(optional_elevation_target) <= upper,
               f'large Dramatic optional elevation target {optional_elevation_target!r} '
               f'is outside {lower}..{upper}')
        expect(lower <= abs(realized_optional_elevation) <= upper,
               f'large Dramatic realized optional elevation {realized_optional_elevation!r} '
               f'is outside {lower}..{upper}')
        expect(realized_optional_elevation * realized_main_elevation < 0,
               'large Dramatic realized optional elevation is not opposite the main route')
        expect(realized_optional_elevation * optional_elevation_target > 0,
               'large Dramatic realized optional elevation reverses its planned side')

motifs = manifest.get('motifs')
expected_motifs = 3 if size >= 5 else 2
expect(isinstance(motifs, list) and len(motifs) == expected_motifs,
       f'size {size} manifest must select {expected_motifs} motifs, got {motifs!r}')
if isinstance(motifs, list):
    expect(len(set(motifs)) == len(motifs), f'motifs are not distinct: {motifs!r}')
    expect(all(isinstance(motif, str) and motif in motif_names for motif in motifs),
           f'unknown or fallback motif in {motifs!r}')

track = manifest.get('arsenal_track')
finale = manifest.get('finale')
expect(isinstance(track, str) and track in tracks, f'unknown arsenal track {track!r}')
expect(isinstance(finale, str) and finale in finales, f'unknown finale card {finale!r}')

# Macro stages are planned before room emission. They make the profile's
# structural identity inspectable while allowing a deterministic safe fallback
# from an infeasible requested shape to a spine.
stages = manifest.get('stages')
required_stage_fields = {
    'stage', 'weight', 'shape', 'realized_shape', 'landmark_archetype',
    'realized_landmark_archetype', 'district_role', 'vertical_intent',
    'realized_vertical_intent', 'vertical_rise', 'realized_vertical_rise', 'gate_rank',
    'material_family', 'elevation_role',
}
stage_by_index = {}
if not isinstance(stages, list):
    errors.append('manifest stages is not an array')
else:
    if isinstance(realized_stage_count, int):
        expect(len(stages) == realized_stage_count,
               f'manifest must serialize {realized_stage_count} realized macro stages, '
               f'got {len(stages)}')
    for index, stage in enumerate(stages):
        if not isinstance(stage, dict):
            errors.append(f'stage {index} is not an object')
            continue
        missing = sorted(required_stage_fields - stage.keys())
        if missing:
            errors.append(f'stage {index} is missing {", ".join(missing)}')
            continue
        expect(isinstance(stage['stage'], int) and stage['stage'] >= 0,
               f'stage {index} has invalid stage index {stage["stage"]!r}')
        expect(stage['stage'] == index,
               f'stage entry {index} is serialized as stage {stage["stage"]!r}')
        expect(isinstance(stage['weight'], int) and stage['weight'] > 0,
               f'stage {index} has invalid positive planning weight {stage["weight"]!r}')
        expect(stage['shape'] in stage_shapes,
               f'stage {index} has unknown requested shape {stage["shape"]!r}')
        expect(stage['realized_shape'] in stage_shapes,
               f'stage {index} has unknown realized shape {stage["realized_shape"]!r}')
        expect(stage['landmark_archetype'] in landmark_archetypes,
               f'stage {index} has unknown landmark archetype {stage["landmark_archetype"]!r}')
        expect(stage['realized_landmark_archetype'] in landmark_archetypes,
               f'stage {index} has unknown realized landmark archetype '
               f'{stage["realized_landmark_archetype"]!r}')
        expect(stage['district_role'] in district_roles,
               f'stage {index} has unknown district role {stage["district_role"]!r}')
        optional_nonempty_string(stage, 'material_family', f'stage {index}')
        optional_nonempty_string(stage, 'elevation_role', f'stage {index}')
        if 'elevation_role' in stage:
            expect(stage['elevation_role'] in elevation_roles,
                   f'stage {index} has unknown elevation role {stage["elevation_role"]!r}')
        expect(stage['vertical_intent'] in vertical_intents,
               f'stage {index} has unknown vertical intent {stage["vertical_intent"]!r}')
        expect(stage['realized_vertical_intent'] in vertical_intents,
               f'stage {index} has unknown realized vertical intent '
               f'{stage["realized_vertical_intent"]!r}')
        expect(isinstance(stage['vertical_rise'], int) and -64 <= stage['vertical_rise'] <= 64,
               f'stage {index} has invalid bounded vertical rise {stage["vertical_rise"]!r}')
        expect(isinstance(stage['realized_vertical_rise'], int) and
               -64 <= stage['realized_vertical_rise'] <= 64,
               f'stage {index} has invalid bounded realized vertical rise '
               f'{stage["realized_vertical_rise"]!r}')
        # `vertical_rise` records the requested signed beat so a safe fallback
        # can remain inspectable even when its realized intent becomes flat.
        if stage['vertical_intent'] == 'flat':
            expect(stage['vertical_rise'] == 0,
                   f'flat stage {index} carries a nonzero rise {stage["vertical_rise"]!r}')
        else:
            expect(stage['vertical_rise'] in {-64, -48, -32, 32, 48, 64},
                   f'vertical stage {index} has unsupported route rise '
                   f'{stage["vertical_rise"]!r}')
        if stage['realized_vertical_intent'] == 'flat':
            expect(stage['realized_vertical_rise'] == 0,
                   f'flat realized stage {index} carries a nonzero rise '
                   f'{stage["realized_vertical_rise"]!r}')
        else:
            expect(stage['realized_vertical_rise'] in {-64, -48, -32, 32, 48, 64},
                   f'realized vertical stage {index} has unsupported route rise '
                   f'{stage["realized_vertical_rise"]!r}')
        if index == len(stages) - 1:
            expect(stage['gate_rank'] == -1,
                   f'final stage {index} must have no outgoing gate rank')
        else:
            expect(isinstance(stage['gate_rank'], int) and stage['gate_rank'] >= 0,
                   f'nonfinal stage {index} has invalid gate rank {stage["gate_rank"]!r}')
        stage_by_index[stage['stage']] = stage

    gate_ranks = [stage.get('gate_rank') for stage in stages[:-1]
                  if isinstance(stage, dict) and isinstance(stage.get('gate_rank'), int)]
    expect(gate_ranks == sorted(gate_ranks),
           f'macro-stage gate ranks are not ordered: {gate_ranks!r}')
    expected_gate_count = max(0, realized_stage_count - 1) \
        if isinstance(realized_stage_count, int) else 0
    expect(len(gate_ranks) == expected_gate_count,
           f'manifest exposes {len(gate_ranks)} gate ranks for '
           f'{expected_gate_count} realized keyed boundaries')
    # Key branches need room to be meaningful choices, and each later stage
    # needs its own travel beat. These are the planner's fixed lower bounds,
    # not an old evenly-spaced campaign cadence.
    if gate_ranks:
        expect(gate_ranks[0] >= 3,
               f'first keyed boundary is too close to the opening: {gate_ranks[0]}')
        expect(all(later - earlier >= 3
                   for earlier, later in zip(gate_ranks, gate_ranks[1:])),
               f'keyed boundaries violate the three-rank minimum spacing: {gate_ranks!r}')
    planned_vertical_stages = [stage for stage in stages
                               if isinstance(stage, dict) and
                               stage.get('realized_vertical_intent') != 'flat']
    # The blueprint's verticality promise begins at normal size. Treat these as
    # route beats, not merely a range of room floor heights: Gentle owns one,
    # Varied owns an ascent and a descent, and Dramatic owns all three with a
    # visibly stronger 48--64-unit change.
    if size >= 3:
        required_vertical_beats = (1, 2, 3)[verticality]
        style_name = ('Gentle', 'Varied', 'Dramatic')[verticality]
        expect(len(planned_vertical_stages) >= required_vertical_beats,
               f'{style_name} verticality selected fewer than '
               f'{required_vertical_beats} non-flat mandatory route stage(s)')
        if verticality >= 1:
            ascents = [stage for stage in planned_vertical_stages
                       if stage['realized_vertical_intent'] == 'dogleg_ascent' and
                       stage['realized_vertical_rise'] > 0]
            descents = [stage for stage in planned_vertical_stages
                        if stage['realized_vertical_intent'] == 'dogleg_descent' and
                        stage['realized_vertical_rise'] < 0]
            expect(ascents,
                   f'{style_name} verticality is missing its positive dogleg ascent')
            expect(descents,
                   f'{style_name} verticality is missing its negative dogleg descent')
        if verticality >= 2:
            expect(any(48 <= abs(stage['realized_vertical_rise']) <= 64
                       for stage in planned_vertical_stages),
                   'Dramatic verticality is missing a 48--64-unit route rise')

# Connections are serialized independently of the room graph so the final
# aperture profile, corridor depth, stair chain, and alignment grouping remain
# inspectable after UDMF emission. They are required current-schema evidence;
# `visual_proof.connector == proven` above is the emitter's final ConnectionRef
# witness for the corresponding serialized geometry.
connection_profiles = {
    'narrow': (96, 48),
    'standard': (128, 64),
    'gallery': (176, 96),
    'grand': (224, 128),
}
connections = manifest.get('connections')
if not isinstance(connections, list):
    errors.append('manifest connections is not an array')
else:
    required_connection_fields = {
        'source', 'target', 'source_cell', 'target_cell', 'route_role',
        'mandatory', 'keyed', 'profile', 'clear_width', 'depth', 'rise',
        'stair_chain', 'alignment_group', 'has_door', 'door_kind',
        'door_clear_width', 'door_art_width', 'door_art_height',
    }
    connection_cells = set()
    for index, connection in enumerate(connections):
        if not isinstance(connection, dict):
            errors.append(f'connection {index} is not an object')
            continue
        missing = sorted(required_connection_fields - connection.keys())
        if missing:
            errors.append(f'connection {index} is missing {", ".join(missing)}')
            continue
        for field in ('source', 'target'):
            expect(isinstance(connection[field], int) and connection[field] >= 0,
                   f'connection {index} has invalid {field} {connection[field]!r}')
        if (isinstance(connection['source'], int) and
                isinstance(connection['target'], int)):
            expect(connection['source'] != connection['target'] or
                   connection['route_role'] == 'room_merge',
                   f'connection {index} joins a room to itself outside a room merge')
        cell_points = []
        for field in ('source_cell', 'target_cell'):
            cell = connection[field]
            valid_cell = (isinstance(cell, list) and len(cell) == 2 and
                          all(isinstance(value, int) and not isinstance(value, bool)
                              for value in cell))
            expect(valid_cell, f'connection {index} has invalid {field} {cell!r}')
            if valid_cell:
                cell_points.append(tuple(cell))
        if len(cell_points) == 2:
            expect(abs(cell_points[0][0] - cell_points[1][0]) +
                   abs(cell_points[0][1] - cell_points[1][1]) == 1,
                   f'connection {index} cells are not cardinal neighbors: {cell_points!r}')
            cell_key = tuple(sorted(cell_points))
            expect(cell_key not in connection_cells,
                   f'connection {index} duplicates the cell edge {cell_key!r}')
            connection_cells.add(cell_key)
        optional_nonempty_string(connection, 'route_role', f'connection {index}')
        for field in ('mandatory', 'keyed'):
            expect(isinstance(connection[field], bool),
                   f'connection {index} has non-boolean {field} {connection[field]!r}')
        has_door = connection['has_door']
        door_kind = connection['door_kind']
        expect(isinstance(has_door, bool),
               f'connection {index} has non-boolean has_door {has_door!r}')
        expect(door_kind in {'none', 'manual', 'keyed', 'secret'},
               f'connection {index} has invalid door_kind {door_kind!r}')
        for field in ('door_clear_width', 'door_art_width', 'door_art_height'):
            expect(isinstance(connection[field], int) and not isinstance(connection[field], bool) and
                   connection[field] >= 0,
                   f'connection {index} has invalid {field} {connection[field]!r}')
        if has_door:
            expect(door_kind != 'none',
                   f'connection {index} marks a real door as kind none')
            expect(connection['door_clear_width'] == connection['clear_width'] and
                   connection['door_clear_width'] >= 128 and connection['depth'] >= 64,
                   f'connection {index} narrows a physical door below Standard clearance')
            expect(connection['door_art_width'] > 0 and connection['door_art_height'] > 0,
                   f'connection {index} has a door without native art dimensions')
        else:
            expect(door_kind == 'none' and connection['door_clear_width'] == 0 and
                   connection['door_art_width'] == 0 and connection['door_art_height'] == 0,
                   f'connection {index} retains door facts without a real door')
        group = connection['alignment_group']
        expect((isinstance(group, int) and group >= 0) or
               (isinstance(group, str) and group.strip()),
               f'connection {index} has invalid alignment_group {group!r}')
        connection_profile = connection['profile']
        expect(isinstance(connection_profile, str) and
               connection_profile.casefold() in connection_profiles,
               f'connection {index} has unknown profile {connection_profile!r}')
        normalized_profile = connection_profile.casefold() \
            if isinstance(connection_profile, str) else None
        for field in ('clear_width', 'depth'):
            expect(finite_number(connection[field]) and connection[field] > 0,
                   f'connection {index} has invalid {field} {connection[field]!r}')
        if normalized_profile in connection_profiles:
            expected_width, expected_depth = connection_profiles[normalized_profile]
            expect(connection['clear_width'] == expected_width,
                   f'connection {index} {normalized_profile} width is not its canonical '
                   f'{expected_width}-unit contract')
            expect(connection['depth'] == expected_depth,
                   f'connection {index} {normalized_profile} depth is not its canonical '
                   f'{expected_depth}-unit contract')
        rise = connection['rise']
        expect(isinstance(rise, int) and not isinstance(rise, bool) and
               rise % 8 == 0 and abs(rise) <= 64,
               f'connection {index} has invalid 8-unit bounded rise {rise!r}')
        chain = connection['stair_chain']
        expect(chain is None or
               (isinstance(chain, int) and chain >= -1) or
               (isinstance(chain, str) and chain.strip()),
               f'connection {index} has invalid stair_chain {chain!r}')
        if connection['keyed']:
            expect(connection['mandatory'],
                   f'keyed connection {index} is not marked mandatory')
            expect(has_door and door_kind == 'keyed',
                   f'keyed connection {index} does not serialize its keyed physical door')
        if connection['mandatory']:
            expect(normalized_profile != 'narrow' and
                   connection['clear_width'] >= 128 and connection['depth'] >= 64,
                   f'mandatory connection {index} does not retain the Standard minimum')
        if normalized_profile == 'narrow':
            normalized_role = connection['route_role'].casefold() \
                if isinstance(connection['route_role'], str) else ''
            mandatory_terms = ('main', 'mandatory', 'key', 'gate', 'stair', 'required')
            expect(not any(term in normalized_role for term in mandatory_terms),
                   f'connection {index} uses a narrow profile on a protected route role '
                   f'{connection["route_role"]!r}')
            expect(not connection['mandatory'],
                   f'connection {index} uses a narrow profile on a mandatory route')
            expect(not connection['keyed'],
                   f'connection {index} uses a narrow profile on a keyed route')
            expect(not rise and chain in (None, -1),
                   f'connection {index} uses a narrow profile for a stair chain')

rooms = manifest.get('rooms')
expect(isinstance(rooms, list) and rooms, 'manifest contains no generated room plan')
required_room_fields = {
    'id', 'rank', 'main_path', 'beat', 'card', 'motif', 'district', 'threat',
    'recovery', 'weapon', 'ammo', 'ammo_count', 'reward', 'optional_armory',
    'arsenal_track', 'finale', 'stage', 'stage_shape', 'district_role',
    'landmark_archetype', 'vertical_intent', 'vertical_rise', 'vertical_anchor',
    'manual_interaction', 'card_feasible', 'card_geometry', 'card_capacity',
    'card_static_enemies', 'card_manual_actions',
    'footprint', 'requested_footprint', 'realized_footprint',
    'footprint_fallback', 'contour_unified', 'contour_loops',
    'contour_sector', 'contour_bounds', 'material_family', 'floor_z', 'clear_height',
    'contour_vertices', 'contour_edges', 'contour_area', 'contour_width',
    'contour_height', 'elevation_role',
}
main_rooms = []
planned_rooms = []
legal_ammo = {17, 2007, 2008, 2010, 2046, 2047, 2048, 2049}
for index, room in enumerate(rooms if isinstance(rooms, list) else []):
    if not isinstance(room, dict):
        errors.append(f'room {index} is not an object')
        continue
    missing = sorted(required_room_fields - room.keys())
    if missing:
        errors.append(f'room {index} is missing {", ".join(missing)}')
        continue
    expect(isinstance(room['id'], int), f'room {index} id is not an integer')
    expect(isinstance(room['rank'], int) and room['rank'] >= 0,
           f'room {index} has invalid progression rank {room["rank"]!r}')
    expect(isinstance(room['main_path'], bool), f'room {index} main_path is not boolean')
    expect(room['beat'] in beats, f'room {index} has unknown beat {room["beat"]!r}')
    expect(room['card'] in cards, f'room {index} has unknown card {room["card"]!r}')
    expect(isinstance(room['card_feasible'], bool),
           f'room {index} card_feasible is not boolean')
    expect(isinstance(room['card_geometry'], str) and room['card_geometry'],
           f'room {index} has no post-emission card geometry evidence')
    expect(room['card_geometry'] in card_geometry_by_card.get(room['card'], set()),
           f'room {index} card {room["card"]!r} has incompatible geometry '
           f'{room["card_geometry"]!r}')
    expect(isinstance(room['card_capacity'], int) and room['card_capacity'] >= 1,
           f'room {index} has invalid card capacity {room["card_capacity"]!r}')
    expect(isinstance(room['card_static_enemies'], int) and
           room['card_static_enemies'] >= 0,
           f'room {index} has invalid static-enemy evidence '
           f'{room["card_static_enemies"]!r}')
    expect(isinstance(room['card_manual_actions'], int) and
           0 <= room['card_manual_actions'] <= 1,
           f'room {index} has invalid manual-action evidence '
           f'{room["card_manual_actions"]!r}')
    expect(room['card_static_enemies'] <= room['card_capacity'],
           f'room {index} emits more static encounter enemies than its proven capacity')
    if room['card'] != 'none':
        expect(room['card_feasible'],
               f'room {index} retains infeasible encounter card {room["card"]!r}')
        expect(room['card_capacity'] >= 1,
               f'room {index} card {room["card"]!r} has no proven capacity')
    else:
        expect(room['card_feasible'],
               f'room {index} marks empty-card room infeasible')
    if room['card'] in {'skirmish', 'cache_challenge'}:
        expect(room['card_static_enemies'] >= 1,
               f'room {index} {room["card"]} has no static enemy evidence')
    if room['card'] in {'crossfire', 'pincer', 'ambush'}:
        expect(room['card_capacity'] >= 2 and room['card_static_enemies'] >= 2,
               f'room {index} {room["card"]} lacks its two-lane enemy capacity')
    if room['card'] in {'holding_line', 'set_piece'}:
        expect(room['card_capacity'] >= 3 and room['card_static_enemies'] >= 3,
               f'room {index} {room["card"]} lacks its three-slot encounter capacity')
    if room['card'] == 'breather':
        expect(room['card_static_enemies'] == 0,
               f'room {index} breather has static encounter enemies')
    if room['card'] == 'cache_challenge' and room['card_geometry'] == 'switch_cache':
        expect(room['manual_interaction'] == 'switch_cache',
               f'room {index} switch-cache card does not serialize its switch interaction')
        expect(room['card_manual_actions'] >= 1,
               f'room {index} switch-cache card has no proven manual action')
    expect(room['motif'] == 'none' or room['motif'] in motif_names,
           f'room {index} has unknown motif {room["motif"]!r}')
    expect(isinstance(room['district'], int) and room['district'] >= 0,
           f'room {index} has invalid district {room["district"]!r}')
    expect(isinstance(room['stage'], int) and room['stage'] in stage_by_index,
           f'room {index} refers to unknown macro stage {room["stage"]!r}')
    expect(room['stage_shape'] in stage_shapes,
           f'room {index} has unknown stage shape {room["stage_shape"]!r}')
    expect(room['district_role'] in district_roles,
           f'room {index} has unknown district role {room["district_role"]!r}')
    optional_nonempty_string(room, 'footprint', f'room {index}')
    optional_nonempty_string(room, 'material_family', f'room {index}')
    optional_nonempty_string(room, 'elevation_role', f'room {index}')
    if 'footprint' in room:
        expect(room['footprint'] in room_footprints,
               f'room {index} has unknown footprint {room["footprint"]!r}')
    expect(room['requested_footprint'] in room_footprints,
           f'room {index} has unknown requested footprint {room["requested_footprint"]!r}')
    expect(room['realized_footprint'] in room_footprints and
           room['footprint'] == room['realized_footprint'],
           f'room {index} does not expose its realized footprint truthfully')
    expect(isinstance(room['contour_unified'], bool),
           f'room {index} contour_unified is not boolean')
    expect(isinstance(room['contour_loops'], int) and room['contour_loops'] >= 1,
           f'room {index} has invalid contour_loops {room["contour_loops"]!r}')
    expect(isinstance(room['contour_sector'], int) and room['contour_sector'] >= 0,
           f'room {index} has invalid contour_sector {room["contour_sector"]!r}')
    bounds = room['contour_bounds']
    valid_bounds = (isinstance(bounds, list) and len(bounds) == 4 and
                    all(finite_number(value) for value in bounds) and
                    bounds[0] < bounds[2] and bounds[1] < bounds[3])
    expect(valid_bounds, f'room {index} has invalid emitted contour_bounds {bounds!r}')
    if room['contour_unified']:
        expect(room['contour_loops'] == 1 and room['contour_vertices'] == 4 and
               room['contour_edges'] == 4,
               f'room {index} claims a unified envelope without one realized exterior loop')
    if 'elevation_role' in room:
        expect(room['elevation_role'] in elevation_roles,
               f'room {index} has unknown elevation role {room["elevation_role"]!r}')
    if 'footprint_variant' in room:
        expect(isinstance(room['footprint_variant'], int) and room['footprint_variant'] >= 0,
               f'room {index} has invalid footprint_variant {room["footprint_variant"]!r}')
    if 'contour_inset' in room:
        expect(isinstance(room['contour_inset'], int) and room['contour_inset'] >= 0,
               f'room {index} has invalid contour_inset {room["contour_inset"]!r}')
    if 'footprint_fallback' in room:
        expect(isinstance(room['footprint_fallback'], bool),
               f'room {index} footprint_fallback is not boolean')
    if 'elevation_target' in room:
        expect(isinstance(room['elevation_target'], int) and room['elevation_target'] % 8 == 0,
               f'room {index} has invalid elevation_target {room["elevation_target"]!r}')
    if 'floor_z' in room:
        expect(isinstance(room['floor_z'], int) and room['floor_z'] % 8 == 0,
               f'room {index} has an invalid 8-unit floor_z {room["floor_z"]!r}')
    if 'clear_height' in room:
        expect(isinstance(room['clear_height'], int) and room['clear_height'] >= 56,
               f'room {index} has insufficient clear_height {room["clear_height"]!r}')
    for field, minimum in (
            ('contour_vertices', 3), ('contour_edges', 3),
            ('contour_area', 1), ('contour_width', 1), ('contour_height', 1)):
        if field in room:
            expect(finite_number(room[field]) and room[field] >= minimum,
                   f'room {index} has invalid {field} {room[field]!r}')
    if 'contour' in room:
        contour = room['contour']
        expect((isinstance(contour, dict) and contour) or
               (isinstance(contour, list) and len(contour) >= 3),
               f'room {index} has an unusable contour summary')
    expect(room['landmark_archetype'] in landmark_archetypes,
           f'room {index} has unknown landmark archetype {room["landmark_archetype"]!r}')
    expect(room['vertical_intent'] in vertical_intents,
           f'room {index} has unknown vertical intent {room["vertical_intent"]!r}')
    expect(isinstance(room['vertical_rise'], int) and -64 <= room['vertical_rise'] <= 64,
           f'room {index} has invalid bounded vertical rise {room["vertical_rise"]!r}')
    expect(isinstance(room['vertical_anchor'], bool),
           f'room {index} vertical_anchor is not boolean')
    expect(room['manual_interaction'] in manual_interactions,
           f'room {index} has unknown manual interaction '
           f'{room["manual_interaction"]!r}')
    if room['vertical_intent'] == 'flat':
        expect(room['vertical_rise'] == 0,
               f'flat room {index} carries a nonzero rise {room["vertical_rise"]!r}')
    else:
        expect(room['vertical_rise'] in {-64, -48, -32, 32, 48, 64},
               f'vertical room {index} has unsupported route rise {room["vertical_rise"]!r}')
    if room['stage'] in stage_by_index:
        expect(room['stage_shape'] == stage_by_index[room['stage']]['realized_shape'],
               f'room {index} disagrees with its macro stage realized shape')
    expect(isinstance(room['threat'], int) and room['threat'] >= 0,
           f'room {index} has invalid threat budget {room["threat"]!r}')
    expect(isinstance(room['recovery'], int) and room['recovery'] >= 0,
           f'room {index} has invalid recovery budget {room["recovery"]!r}')
    expect(isinstance(room['weapon'], int) and room['weapon'] >= 0,
           f'room {index} has invalid weapon type {room["weapon"]!r}')
    expect(isinstance(room['ammo'], int) and room['ammo'] >= 0,
           f'room {index} has invalid ammo type {room["ammo"]!r}')
    expect(isinstance(room['ammo_count'], int) and room['ammo_count'] >= 0,
           f'room {index} has invalid ammo count {room["ammo_count"]!r}')
    expect(room['reward'] in rewards, f'room {index} has unknown reward plan {room["reward"]!r}')
    expect(isinstance(room['optional_armory'], bool),
           f'room {index} optional_armory is not boolean')
    expect(room['arsenal_track'] == track,
           f'room {index} does not inherit top-level arsenal track')
    expect(room['finale'] == 'none' or room['finale'] == finale,
           f'room {index} has unrelated finale card {room["finale"]!r}')
    # Difficulty 1 may retain a named optional ambush as a low-threat sightline
    # or cache opportunity. Standard and higher difficulties must budget every
    # high-pressure card explicitly.
    if difficulty >= 3 and room['card'] in high_pressure_cards:
        expect(room['threat'] > 0,
               f'room {index} assigns high-pressure card {room["card"]} no threat budget')
    if room['card'] == 'cache_challenge':
        expect(room['reward'] != 'none' or room['recovery'] > 0,
               f'cache challenge room {index} has no cache or recovery plan')
    if room['optional_armory']:
        expect(not room['main_path'] and room['reward'] == 'armory' and room['weapon'] > 0,
               f'optional armory room {index} lacks its optional weapon reward')
    if room['ammo'] == 0:
        expect(room['ammo_count'] == 0,
               f'room {index} marks absent ammo but keeps count {room["ammo_count"]}')
    else:
        expect(room['ammo'] in legal_ammo and room['ammo_count'] > 0,
               f'room {index} has invalid ammo plan {room["ammo"]}/{room["ammo_count"]}')
    planned_rooms.append(room)
    if room['main_path']:
        main_rooms.append(room)

# Blueprint motif selection is not merely a top-level label. Every selected
# motif must retain a deterministic host room after composition. A host's
# landmark grammar is separately serialized and can safely fall back (for
# example, a clearance-constrained remote reveal may retain its motif without
# a gatehouse); the paired manifest comparison in replayability proves that
# this recorded fallback is deterministic rather than silently disappearing.
if isinstance(motifs, list):
    for motif in motifs:
        hosts = [room for room in planned_rooms if room['motif'] == motif]
        expect(hosts, f'selected motif {motif!r} has no realized manifest host room')
    for room in planned_rooms:
        expect(room['motif'] == 'none' or room['motif'] in motifs,
               f'room {room["id"]} realizes unselected motif {room["motif"]!r}')

if main_rooms:
    main_rooms.sort(key=lambda room: (room['rank'], room['id']))
    ranks = [room['rank'] for room in main_rooms]
    expect(len(ranks) == len(set(ranks)), 'multiple main-path rooms share a progression rank')
    expect(main_rooms[0]['beat'] == 'opening', 'first main-path room is not the opening beat')
    expect(main_rooms[-1]['beat'] == 'finale', 'last main-path room is not the finale beat')
    expect(main_rooms[-1]['finale'] == finale,
           'final main-path room does not own the selected finale card')
    expect(main_rooms[0]['weapon'] == 2001,
           'opening room does not expose the guaranteed starting shotgun')

    # The terrain summary is not merely a target label.  Require a real
    # highland/basin room at the published extremum.  The post-emission
    # accessibility proof is already mandatory above, so the optional witness
    # below is also covered by the serialized valid-key-state reachability pass.
    if verticality == 2 and size >= 3 and valid_terrain_fields:
        main_floors = [room['floor_z'] for room in main_rooms
                       if isinstance(room.get('floor_z'), int) and
                       not isinstance(room.get('floor_z'), bool)]
        expect(main_floors and max(abs(floor) for floor in main_floors) ==
               abs(realized_main_elevation),
               'Dramatic realized main-route elevation does not match its final room floors')
        main_role = 'highland' if realized_main_elevation > 0 else 'basin'
        main_witnesses = [room for room in main_rooms
                          if room.get('floor_z') == realized_main_elevation and
                          room.get('elevation_role') == main_role]
        expect(main_witnesses,
               'Dramatic realized main-route elevation has no matching main-route '
               'highland/basin room witness')
        if size >= 5:
            optional_role = 'highland' if realized_optional_elevation > 0 else 'basin'
            optional_witnesses = [room for room in planned_rooms
                                  if not room.get('main_path') and
                                  room.get('floor_z') == realized_optional_elevation and
                                  room.get('elevation_role') == optional_role]
            expect(optional_witnesses,
                   'large Dramatic realized optional elevation has no matching reachable '
                   'optional highland/basin room witness')

    # A non-flat macro stage must be owned by an actual main-route room, not
    # merely advertised in the briefing while vertical geometry lands in an
    # optional branch. The anchor is the room that owns the planned rise.
    for stage_index, stage in sorted(stage_by_index.items()):
        stage_main_rooms = [room for room in main_rooms if room['stage'] == stage_index]
        expect(stage_main_rooms,
               f'macro stage {stage_index} has no main-route room')
        if stage['realized_vertical_intent'] == 'flat':
            continue
        anchors = [room for room in stage_main_rooms if room['vertical_anchor']]
        expect(anchors,
               f'non-flat macro stage {stage_index} has no main-route vertical anchor')
        expect(any(room['vertical_intent'] == stage['realized_vertical_intent'] and
                   room['vertical_rise'] == stage['realized_vertical_rise'] for room in anchors),
               f'non-flat macro stage {stage_index} is not represented by its vertical anchor')

    pressure_streak = 0
    for room in main_rooms:
        if room['card'] in high_pressure_cards:
            pressure_streak += 1
            expect(pressure_streak <= 2,
                   f'main path has {pressure_streak} consecutive high-pressure cards')
        else:
            pressure_streak = 0

    for index, room in enumerate(main_rooms):
        if room['card'] not in {'holding_line', 'set_piece'}:
            continue
        follow_up = main_rooms[index + 1:index + 3]
        if room['beat'] == 'finale':
            expect(room['recovery'] > 0 or room['reward'] == 'finale_reserve',
                   f'finale {room["card"]} at rank {room["rank"]} lacks its reserve')
        else:
            expect(any(next_room['recovery'] > 0 or next_room['weapon'] > 0 or
                       next_room['reward'] != 'none' for next_room in follow_up),
                   f'{room["card"]} at rank {room["rank"]} lacks a recovery or choice follow-up')

    finale_window = main_rooms[max(0, len(main_rooms) - 3):]
    expect(any(room['recovery'] > 0 or room['reward'] == 'finale_reserve'
               for room in finale_window),
           'finale has no planned recovery or finale reserve')
else:
    errors.append('manifest contains no main-path room plan')

# A key objective is a side branch before its matching stage gate. The
# serialized UDMF key-state solver proves the colors and physical route; this
# manifest check keeps the blueprint timing itself visible and protects the
# key-before-gate contract from a metadata regression.
if isinstance(stages, list):
    keyed_stages = [(index, stage.get('gate_rank')) for index, stage in enumerate(stages[:-1])
                    if isinstance(stage, dict)]
    for stage_index, gate_rank in keyed_stages:
        key_hosts = [room for room in planned_rooms
                     if room['stage'] == stage_index and room['beat'] == 'key_objective']
        expect(key_hosts,
               f'keyed stage {stage_index} has no serialized key-objective room')
        if isinstance(gate_rank, int):
            expect(any(room['rank'] < gate_rank for room in key_hosts),
                   f'keyed stage {stage_index} places its key objective at/after gate rank '
                   f'{gate_rank}')

# Ammo can reward a weapon in the same room, but rocket and cell ammo must
# never become mandatory-route bait before a compatible launcher exists.
# Optional armories are deliberate side choices; they may make their own ammo
# usable, but they must never be counted as a weapon source for the required
# route or a later unrelated side cache.
ammo_dependencies = {
    2008: {2001, 82}, 2049: {2001, 82},      # shells / shell box
    2010: {2003}, 2046: {2003},              # rockets / rocket box
    2047: {2004, 2006}, 17: {2004, 2006},    # cells / cell pack
}
mandatory_weapons = {2001}  # The opening puts the required shotgun in reach.
mandatory_weapons_by_rank = {}
for rank in sorted({room['rank'] for room in planned_rooms}):
    rank_main_rooms = [room for room in planned_rooms
                       if room['rank'] == rank and room['main_path']]
    # Same-rank main-route weapon and ammo are one ordered player beat.
    mandatory_weapons.update(room['weapon'] for room in rank_main_rooms
                             if room['weapon'] > 0)
    mandatory_weapons_by_rank[rank] = set(mandatory_weapons)
    for room in rank_main_rooms:
        required_weapons = ammo_dependencies.get(room['ammo'])
        if required_weapons and not (mandatory_weapons & required_weapons):
            errors.append(f'mandatory room {room["id"]} exposes ammo {room["ammo"]} '
                          'before a compatible mandatory-route weapon')

for room in planned_rooms:
    if room['main_path']:
        continue
    available_weapons = set(mandatory_weapons_by_rank.get(room['rank'], {2001}))
    # A side room may contain its own optional weapon and matching ammo, but
    # its weapon remains local to this check rather than leaking into the
    # mandatory resource ledger.
    if room['weapon'] > 0:
        available_weapons.add(room['weapon'])
    required_weapons = ammo_dependencies.get(room['ammo'])
    if required_weapons and not (available_weapons & required_weapons):
        errors.append(f'optional room {room["id"]} exposes ammo {room["ammo"]} '
                      'without its own or an already-mandatory compatible weapon')

for room in planned_rooms:
    if not room['optional_armory']:
        continue
    expect(not room['main_path'] and room['reward'] == 'armory' and room['weapon'] > 0,
           f'optional armory room {room["id"]} is not a safe side weapon choice')
    expect(room['weapon'] != 2001,
           f'optional armory room {room["id"]} duplicates the guaranteed starting shotgun')

for error in errors:
    print(f'    {error}')
if errors:
    raise SystemExit(1)
print(f'  manifest profile={profile} orientation={orientation} motifs={",".join(motifs)} '
      f'arsenal={track} finale={finale} rooms={len(rooms)}')
PY
}

# The final UDMF has no stable ID for every open corridor: room merges,
# multi-flight stairs, and fitted ordinary doors intentionally split/reuse
# sectors.  Keyed doors do retain a one-sector/two-face serialization contract,
# however.  Cross-check that safe subset against the manifest, while the
# mandatory visual_proof.connector witness covers the emitter's final aperture
# and depth proof for all other connection kinds.
validate_manifest_udmf_connectors() {
	local manifest_path=$1
	local udmf_path=$2
	python3 - "$manifest_path" "$udmf_path" <<'PY'
import collections
import json
import math
import re
import sys

manifest_path, udmf_path = sys.argv[1:3]
errors = []

def load_json(path):
    try:
        with open(path, encoding='utf-8') as handle:
            return json.load(handle)
    except (OSError, json.JSONDecodeError) as error:
        errors.append(f'could not parse run manifest {path}: {error}')
        return {}

def load_udmf(path):
    try:
        with open(path, encoding='utf-8') as handle:
            return handle.read()
    except OSError as error:
        errors.append(f'could not read UDMF {path}: {error}')
        return ''

def blocks(text, kind):
    return [dict((key, value.strip('"')) for key, value in
                 re.findall(r'^\s*(\w+)\s*=\s*([^;]+);', body, re.M))
            for body in re.findall(r'(?m)^' + kind + r'\s*\n\{(.*?)\n\}', text, re.S)]

def integer(value):
    try:
        return int(value)
    except (TypeError, ValueError):
        return None

manifest = load_json(manifest_path)
text = load_udmf(udmf_path)
visual_proof = manifest.get('visual_proof') if isinstance(manifest, dict) else None
if not isinstance(visual_proof, dict) or visual_proof.get('status') != 'proven' or \
        visual_proof.get('connector') != 'proven':
    errors.append('manifest lacks a proven post-emission connector witness')

profiles = {
    'narrow': (96, 48),
    'standard': (128, 64),
    'gallery': (176, 96),
    'grand': (224, 128),
}
connections = manifest.get('connections') if isinstance(manifest, dict) else None
keyed_connections = []
door_connections = []
seen_cells = set()
if not isinstance(connections, list):
    errors.append('manifest lacks its required connections array')
else:
    for index, connection in enumerate(connections):
        if not isinstance(connection, dict):
            errors.append(f'connection {index} is not an object')
            continue
        source_cell = connection.get('source_cell')
        target_cell = connection.get('target_cell')
        valid_cells = (isinstance(source_cell, list) and len(source_cell) == 2 and
                       isinstance(target_cell, list) and len(target_cell) == 2 and
                       all(isinstance(value, int) and not isinstance(value, bool)
                           for value in source_cell + target_cell))
        if not valid_cells:
            errors.append(f'connection {index} has unusable source/target cells')
        else:
            source, target = tuple(source_cell), tuple(target_cell)
            if abs(source[0] - target[0]) + abs(source[1] - target[1]) != 1:
                errors.append(f'connection {index} cells are not cardinal neighbors')
            pair = tuple(sorted((source, target)))
            if pair in seen_cells:
                errors.append(f'connection {index} duplicates a serialized cell edge')
            seen_cells.add(pair)
        profile = connection.get('profile')
        profile_key = profile.casefold() if isinstance(profile, str) else None
        if profile_key not in profiles:
            errors.append(f'connection {index} has unknown profile {profile!r}')
        else:
            width, depth = profiles[profile_key]
            if connection.get('clear_width') != width or connection.get('depth') != depth:
                errors.append(f'connection {index} {profile_key} does not retain canonical '
                              f'{width}x{depth} dimensions')
        mandatory = connection.get('mandatory')
        keyed = connection.get('keyed')
        has_door = connection.get('has_door')
        door_kind = connection.get('door_kind')
        door_width = connection.get('door_clear_width')
        door_art_width = connection.get('door_art_width')
        door_art_height = connection.get('door_art_height')
        rise = connection.get('rise')
        stair_chain = connection.get('stair_chain')
        if not isinstance(mandatory, bool) or not isinstance(keyed, bool):
            errors.append(f'connection {index} lacks boolean mandatory/keyed flags')
        if not isinstance(has_door, bool) or door_kind not in {'none', 'manual', 'keyed', 'secret'}:
            errors.append(f'connection {index} has incomplete door facts')
        elif has_door:
            if (door_kind == 'none' or door_width != connection.get('clear_width') or
                    not isinstance(door_width, int) or door_width < 128 or
                    not isinstance(door_art_width, int) or door_art_width <= 0 or
                    not isinstance(door_art_height, int) or door_art_height <= 0):
                errors.append(f'connection {index} has an unsafe physical door contract')
            else:
                door_connections.append(connection)
        elif door_kind != 'none' or any(value != 0 for value in
                                        (door_width, door_art_width, door_art_height)):
            errors.append(f'connection {index} keeps door dimensions without a door')
        if keyed:
            keyed_connections.append(connection)
            if not mandatory:
                errors.append(f'keyed connection {index} is not mandatory')
            if not has_door or door_kind != 'keyed':
                errors.append(f'keyed connection {index} lacks a keyed physical door')
        stair = (isinstance(rise, int) and not isinstance(rise, bool) and rise != 0) or \
            (isinstance(stair_chain, int) and stair_chain >= 0)
        if profile_key == 'narrow' and (mandatory is True or keyed is True or stair):
            errors.append(f'connection {index} uses Narrow on a protected route')

lines = blocks(text, 'linedef')
sides = blocks(text, 'sidedef')
vertices = blocks(text, 'vertex')
keyed_faces = collections.defaultdict(list)
door_faces = collections.defaultdict(list)
for index, line in enumerate(lines):
    lock = integer(line.get('locknumber'))
    if line.get('special') != '12':
        if lock is not None and lock > 0:
            errors.append(f'keyed UDMF line {index} is not a Door_Raise lock')
        continue
    back = integer(line.get('sideback'))
    if back is None or not 0 <= back < len(sides):
        errors.append(f'Door_Raise line {index} has no valid back-sided door sector')
        continue
    sector = integer(sides[back].get('sector'))
    if sector is None:
        errors.append(f'Door_Raise line {index} has no valid back-sided sector')
        continue
    try:
        first = vertices[integer(line.get('v1'))]
        second = vertices[integer(line.get('v2'))]
        width = math.dist((float(first['x']), float(first['y'])),
                          (float(second['x']), float(second['y'])))
    except (KeyError, IndexError, TypeError, ValueError):
        errors.append(f'Door_Raise line {index} has invalid geometry')
        continue
    door_faces[sector].append({
        'width': width,
        'lock': lock if lock is not None else 0,
        'secret': line.get('secret') == 'true',
    })
    if lock is None or lock <= 0:
        continue
    if line.get('special') != '12':
        errors.append(f'keyed UDMF line {index} is not a Door_Raise lock')
        continue
    keyed_faces[sector].append(lock)

for sector, locks in keyed_faces.items():
    if len(locks) != 2:
        errors.append(f'keyed UDMF sector {sector} has {len(locks)} faces instead of two')
    elif len(set(locks)) != 1:
        errors.append(f'keyed UDMF sector {sector} mixes lock types {locks!r}')
if isinstance(connections, list):
    if len(keyed_faces) != len(keyed_connections):
        errors.append(f'manifest has {len(keyed_connections)} keyed crossings but UDMF has '
                      f'{len(keyed_faces)} keyed door sectors')
    physical_locks = {locks[0] for locks in keyed_faces.values() if locks}
    key_order = manifest.get('key_order', [])
    allowed_locks = set(key_order) if isinstance(key_order, list) else set()
    if len(physical_locks) != len(keyed_faces):
        errors.append('UDMF reuses one key lock across distinct serialized keyed crossings')
    if not physical_locks.issubset(allowed_locks):
        errors.append(f'UDMF keyed door locks {sorted(physical_locks)} are outside manifest '
                      f'key order {key_order!r}')

# Every normal/keyed door sector is a concrete two-face physical aperture. The
# manifest's art metrics may be 64 units, but its clear-width fact must match a
# real 128+ UDMF slab; hidden cache panels are allowed as additional secret
# sectors and therefore do not weaken the graph-door count.
physical_widths = collections.defaultdict(collections.Counter)
for sector, faces in door_faces.items():
    if len(faces) != 2:
        errors.append(f'Door_Raise sector {sector} has {len(faces)} faces instead of two')
        continue
    widths = [face['width'] for face in faces]
    if abs(widths[0] - widths[1]) > 0.01:
        errors.append(f'Door_Raise sector {sector} has mismatched face widths {widths!r}')
        continue
    locks = [face['lock'] for face in faces]
    secrets = [face['secret'] for face in faces]
    if len(set(locks)) != 1 or len(set(secrets)) != 1:
        errors.append(f'Door_Raise sector {sector} mixes lock/secret states')
        continue
    width = int(round(widths[0]))
    if not secrets[0] and width < 128:
        errors.append(f'Door_Raise sector {sector} narrows a normal/keyed slab to {width}')
    kind = 'keyed' if locks[0] > 0 else ('secret' if secrets[0] else 'manual')
    physical_widths[kind][width] += 1

if isinstance(connections, list):
    expected_widths = collections.defaultdict(collections.Counter)
    for connection in door_connections:
        expected_widths[connection['door_kind']][connection['door_clear_width']] += 1
    for kind, expected in expected_widths.items():
        available = physical_widths[kind]
        missing = expected - available
        if missing:
            errors.append(f'manifest {kind} door apertures {dict(expected)} are not realized by '
                          f'UDMF sectors {dict(available)}')
    if sum(sum(widths.values()) for widths in physical_widths.values()) < len(door_connections):
        errors.append('UDMF has fewer real door sectors than manifest graph-door connections')

for error in errors:
    print(f'    {error}')
if errors:
    raise SystemExit(1)
print(f'  manifest/UDMF connector contract passed: keyed_crossings={len(keyed_connections)} '
      f'graph_doors={len(door_connections)}')
PY
}

# The emitter's collision-navigation proof deliberately uses final UDMF
# corridors and interaction anchors instead of trusting coarse-grid adjacency.
# Cross-check the bounded manifest witnesses against a separately dumped map.
validate_manifest_udmf_collision_navigation() {
	local manifest_path=$1
	local udmf_path=$2
	local require_room_merge=0
	local require_switch_cache=0
	if [ "$#" -ge 3 ]; then require_room_merge=$3; fi
	if [ "$#" -ge 4 ]; then require_switch_cache=$4; fi
	python3 - "$manifest_path" "$udmf_path" "$require_room_merge" "$require_switch_cache" <<'PY'
import collections
import json
import math
import re
import sys

manifest_path, udmf_path, require_room_merge, require_switch_cache = sys.argv[1:5]
require_room_merge = require_room_merge == '1'
require_switch_cache = require_switch_cache == '1'
errors = []

def load_json(path):
    try:
        with open(path, encoding='utf-8') as handle:
            return json.load(handle)
    except (OSError, json.JSONDecodeError) as error:
        errors.append(f'could not parse collision-navigation manifest {path}: {error}')
        return {}

def load_udmf(path):
    try:
        with open(path, encoding='utf-8') as handle:
            return handle.read()
    except OSError as error:
        errors.append(f'could not read collision-navigation UDMF {path}: {error}')
        return ''

def blocks(text, kind):
    return [dict((key, value.strip('"')) for key, value in
                 re.findall(r'^\s*(\w+)\s*=\s*([^;]+);', body, re.M))
            for body in re.findall(r'(?m)^' + kind + r'\s*\n\{(.*?)\n\}', text, re.S)]

def integer(value):
    try:
        return int(value)
    except (TypeError, ValueError):
        return None

def number(value):
    try:
        result = float(value)
    except (TypeError, ValueError):
        return None
    return result if math.isfinite(result) else None

def point_segment_distance(px, py, ax, ay, bx, by):
    dx, dy = bx - ax, by - ay
    length_squared = dx * dx + dy * dy
    if length_squared <= 1.0e-9:
        return math.hypot(px - ax, py - ay)
    fraction = max(0.0, min(1.0, ((px - ax) * dx + (py - ay) * dy) / length_squared))
    return math.hypot(px - (ax + fraction * dx), py - (ay + fraction * dy))

def segments_intersect(ax, ay, bx, by, cx, cy, dx, dy):
    def cross(ox, oy, px, py, qx, qy):
        return (px - ox) * (qy - oy) - (py - oy) * (qx - ox)
    ab_c = cross(ax, ay, bx, by, cx, cy)
    ab_d = cross(ax, ay, bx, by, dx, dy)
    cd_a = cross(cx, cy, dx, dy, ax, ay)
    cd_b = cross(cx, cy, dx, dy, bx, by)
    return (((ab_c > 1.0e-4 and ab_d < -1.0e-4) or
             (ab_c < -1.0e-4 and ab_d > 1.0e-4)) and
            ((cd_a > 1.0e-4 and cd_b < -1.0e-4) or
             (cd_a < -1.0e-4 and cd_b > 1.0e-4)))

def segment_distance(ax, ay, bx, by, cx, cy, dx, dy):
    if segments_intersect(ax, ay, bx, by, cx, cy, dx, dy):
        return 0.0
    return min(point_segment_distance(ax, ay, cx, cy, dx, dy),
               point_segment_distance(bx, by, cx, cy, dx, dy),
               point_segment_distance(cx, cy, ax, ay, bx, by),
               point_segment_distance(dx, dy, ax, ay, bx, by))

def expect(condition, message):
    if not condition:
        errors.append(message)

manifest = load_json(manifest_path)
text = load_udmf(udmf_path)
accessibility = manifest.get('accessibility') if isinstance(manifest, dict) else None
navigation = accessibility.get('collision_navigation') if isinstance(accessibility, dict) else None
if not isinstance(navigation, dict):
    errors.append('manifest lacks collision_navigation accessibility proof')
    navigation = {}

vertices = blocks(text, 'vertex')
sides = blocks(text, 'sidedef')
lines = blocks(text, 'linedef')
things = blocks(text, 'thing')
sectors = blocks(text, 'sector')
required = {
    'status', 'player_radius', 'safety_margin', 'pad_reservations',
    'corridor_reservations', 'reservation_count', 'proven_corridors',
    'key_state_edges', 'mandatory_anchor_count', 'ordinary_cells_reached',
    'ordinary_rewards_reached', 'keyed_door_approaches_reached',
    'manual_switches_reached', 'physical_corridor_witnesses',
    'room_merge_corridors', 'switch_cache_actions',
    'switch_cache_rewards_reached', 'required_key_mask', 'exit_key_mask',
    'anchors', 'corridors', 'switch_caches', 'cooperative_starts',
}
missing = sorted(required - navigation.keys())
if missing:
    errors.append('collision-navigation proof is missing ' + ', '.join(missing))
if navigation.get('status') != 'proven':
    errors.append('collision-navigation proof status is not proven')
for field in required - {'status', 'anchors', 'corridors', 'switch_caches',
                         'cooperative_starts'}:
    if field in navigation and (not isinstance(navigation[field], int) or
                                isinstance(navigation[field], bool) or navigation[field] < 0):
        errors.append(f'collision-navigation {field} is not a nonnegative integer')
if all(field in navigation for field in
       ('pad_reservations', 'corridor_reservations', 'reservation_count')):
    expect(navigation['pad_reservations'] + navigation['corridor_reservations'] ==
           navigation['reservation_count'],
           'collision-navigation pad/corridor reservation counters do not reconcile')
if isinstance(accessibility, dict) and 'navigation_reservations' in accessibility and \
        'reservation_count' in navigation:
    expect(navigation['reservation_count'] == accessibility['navigation_reservations'],
           'collision-navigation reservation count differs from accessibility summary')
if all(field in navigation for field in ('exit_key_mask', 'required_key_mask')):
    expect((navigation['exit_key_mask'] & navigation['required_key_mask']) ==
           navigation['required_key_mask'],
           'collision-navigation exit state does not include every required key')
if navigation.get('proven_corridors', 0) <= 0 or navigation.get('key_state_edges', 0) <= 0:
    errors.append('collision-navigation proof has no serialized corridor/key-state evidence')

line_geometry = {}
for line_index, line in enumerate(lines):
    v1, v2 = integer(line.get('v1')), integer(line.get('v2'))
    if v1 is None or v2 is None or not (0 <= v1 < len(vertices) and 0 <= v2 < len(vertices)):
        continue
    coords = tuple(number(vertices[vertex].get(axis))
                   for vertex in (v1, v2) for axis in ('x', 'y'))
    if all(value is not None for value in coords):
        line_geometry[line_index] = coords

corridor_lane_shapes = []
corridors = navigation.get('corridors')
if not isinstance(corridors, list):
    errors.append('collision-navigation proof has no corridor witnesses')
    corridors = []
else:
    expect(navigation.get('physical_corridor_witnesses') == len(corridors),
           'collision-navigation physical-corridor count differs from its witnesses')
    expect(navigation.get('proven_corridors') == len(corridors),
           'collision-navigation proven-corridor count differs from its witnesses')
room_merge_pairs = set()
room_merge_witnesses = 0
for corridor_index, corridor in enumerate(corridors):
    if not isinstance(corridor, dict):
        errors.append(f'collision corridor {corridor_index} is not an object')
        continue
    fields = {'kind', 'source_cell', 'target_cell', 'source_sector', 'target_sector',
              'connector_sector', 'door_sector', 'source_line', 'target_line',
              'reservation_count', 'lanes'}
    missing_fields = sorted(fields - corridor.keys())
    if missing_fields:
        errors.append(f'collision corridor {corridor_index} is missing ' +
                      ', '.join(missing_fields))
        continue
    kind = corridor['kind']
    if kind not in {'room_merge', 'connector', 'door', 'stair'}:
        errors.append(f'collision corridor {corridor_index} has unknown kind {kind!r}')
        continue
    int_fields = fields - {'kind', 'lanes'}
    if any(not isinstance(corridor[field], int) or isinstance(corridor[field], bool)
           for field in int_fields):
        errors.append(f'collision corridor {corridor_index} has non-integer metadata')
        continue
    if corridor['source_cell'] < 0 or corridor['target_cell'] < 0 or \
            corridor['source_cell'] == corridor['target_cell']:
        errors.append(f'collision corridor {corridor_index} has invalid cell endpoints')
    for sector_field in ('source_sector', 'target_sector', 'connector_sector'):
        sector = corridor[sector_field]
        if not 0 <= sector < len(sectors):
            errors.append(f'collision corridor {corridor_index} has invalid {sector_field}')
    door_sector = corridor['door_sector']
    if door_sector != -1 and not 0 <= door_sector < len(sectors):
        errors.append(f'collision corridor {corridor_index} has invalid door_sector')
    lanes = corridor['lanes']
    if not isinstance(lanes, list) or not lanes:
        errors.append(f'collision corridor {corridor_index} has no clear lanes')
        continue
    if corridor['reservation_count'] != len(lanes):
        errors.append(f'collision corridor {corridor_index} reservation count does not match lanes')
    for lane_index, lane in enumerate(lanes):
        if not isinstance(lane, dict):
            errors.append(f'collision corridor {corridor_index} lane {lane_index} is not an object')
            continue
        lane_fields = {'x1', 'y1', 'x2', 'y2', 'clear_radius'}
        if lane_fields - lane.keys() or any(not isinstance(lane[field], (int, float)) or
                                             isinstance(lane[field], bool) or
                                             not math.isfinite(lane[field])
                                             for field in lane_fields):
            errors.append(f'collision corridor {corridor_index} lane {lane_index} is invalid')
            continue
        if lane['clear_radius'] < 32.0:
            errors.append(f'collision corridor {corridor_index} lane {lane_index} is below player/safety clearance')
        corridor_lane_shapes.append((lane['x1'], lane['y1'], lane['x2'], lane['y2'],
                                      lane['clear_radius'], 'corridor', corridor_index))
        if kind == 'room_merge':
            for line_index, line in enumerate(lines):
                if line.get('blocking') != 'true' or line_index not in line_geometry:
                    continue
                if segment_distance(lane['x1'], lane['y1'], lane['x2'], lane['y2'],
                                    *line_geometry[line_index]) < lane['clear_radius'] - 0.01:
                    errors.append(f'room-merge corridor {corridor_index} lane {lane_index} '
                                  f'is intersected by blocking UDMF line {line_index}')
                    break
    source_line = corridor['source_line']
    target_line = corridor['target_line']
    if kind == 'room_merge':
        room_merge_witnesses += 1
        pair = tuple(sorted((corridor['source_cell'], corridor['target_cell'])))
        if pair in room_merge_pairs:
            errors.append(f'room-merge corridor {corridor_index} duplicates cell pair {pair}')
        room_merge_pairs.add(pair)
        if (corridor['source_sector'] != corridor['target_sector'] or
                corridor['source_sector'] != corridor['connector_sector'] or
                corridor['door_sector'] != -1 or source_line != -1 or target_line != -1):
            errors.append(f'room-merge corridor {corridor_index} is not a direct shared-sector lane')
    else:
        for line_field, line_index in (('source_line', source_line),
                                       ('target_line', target_line)):
            if line_index not in line_geometry:
                errors.append(f'collision corridor {corridor_index} has invalid {line_field}')
                continue
            line = lines[line_index]
            if line.get('blocking') == 'true' or integer(line.get('sideback')) is None:
                errors.append(f'collision corridor {corridor_index} {line_field} is not a real portal')
if isinstance(navigation.get('room_merge_corridors'), int):
    expect(navigation['room_merge_corridors'] == room_merge_witnesses,
           'collision-navigation room-merge count differs from its lanes')
if require_room_merge:
    expect(room_merge_witnesses > 0,
           'focused collision-navigation case expected at least one room-merge lane')

anchors = navigation.get('anchors')
if not isinstance(anchors, list) or not anchors:
    errors.append('collision-navigation proof has no anchors')
    anchors = []
by_kind = collections.defaultdict(list)
keyed_anchor_lines = set()
switch_anchor_lines = set()
cache_reward_anchor_things = collections.defaultdict(list)
clearance_shapes = []
for anchor_index, anchor in enumerate(anchors):
    if not isinstance(anchor, dict):
        errors.append(f'collision-navigation anchor {anchor_index} is not an object')
        continue
    fields = {'kind', 'thing_index', 'line', 'side', 'x', 'y', 'clear_radius', 'lock', 'tag'}
    anchor_missing = sorted(fields - anchor.keys())
    if anchor_missing:
        errors.append(f'collision-navigation anchor {anchor_index} is missing ' +
                      ', '.join(anchor_missing))
        continue
    kind = anchor['kind']
    if kind not in {'start', 'key', 'exit', 'keyed_door_approach',
                    'manual_switch', 'switch_cache_reward'}:
        errors.append(f'collision-navigation anchor {anchor_index} has unknown kind {kind!r}')
        continue
    if any(not isinstance(anchor[field], int) or isinstance(anchor[field], bool)
           for field in ('thing_index', 'line', 'side', 'lock', 'tag')) or \
            any(not isinstance(anchor[field], (int, float)) or isinstance(anchor[field], bool) or
                not math.isfinite(anchor[field]) for field in ('x', 'y', 'clear_radius')) or \
            anchor['clear_radius'] <= 0.0:
        errors.append(f'collision-navigation anchor {anchor_index} has invalid fields')
        continue
    by_kind[kind].append(anchor)
    if kind in {'start', 'key', 'switch_cache_reward'}:
        thing_index = anchor['thing_index']
        if anchor['line'] != -1 or not 0 <= thing_index < len(things):
            errors.append(f'{kind} anchor {anchor_index} has no valid UDMF thing')
            continue
        thing = things[thing_index]
        x, y = number(thing.get('x')), number(thing.get('y'))
        if x is None or y is None or math.hypot(x - anchor['x'], y - anchor['y']) > 0.01:
            errors.append(f'{kind} anchor {anchor_index} does not match UDMF thing coordinates')
        if kind == 'start':
            if thing.get('type') != '1' or anchor['lock'] != 0:
                errors.append(f'start anchor {anchor_index} does not reference PlayerStart')
        elif kind == 'key':
            expected_lock = {'13': 1, '5': 2, '6': 3}.get(thing.get('type'))
            if expected_lock != anchor['lock']:
                errors.append(f'key anchor {anchor_index} does not reference its key color')
        else:
            if anchor['lock'] != 0 or anchor['tag'] <= 0 or anchor['clear_radius'] < 40.0:
                errors.append(f'switch-cache reward anchor {anchor_index} lacks tag/pad contract')
            cache_reward_anchor_things[anchor['tag']].append(anchor)
        clearance_shapes.append((anchor['x'], anchor['y'], anchor['x'], anchor['y'],
                                 anchor['clear_radius'], kind, anchor_index))
        continue
    line_index = anchor['line']
    if anchor['thing_index'] != -1 or line_index not in line_geometry:
        errors.append(f'{kind} anchor {anchor_index} has no valid UDMF line')
        continue
    x1, y1, x2, y2 = line_geometry[line_index]
    if math.hypot((x1 + x2) * 0.5 - anchor['x'],
                  (y1 + y2) * 0.5 - anchor['y']) > 0.01:
        errors.append(f'{kind} anchor {anchor_index} does not match UDMF line midpoint')
    line = lines[line_index]
    if integer(line.get('sidefront')) != anchor['side']:
        errors.append(f'{kind} anchor {anchor_index} has stale front sidedef')
    if kind == 'exit':
        if line.get('special') != '243' or line.get('playercross') != 'true' or anchor['lock'] != 0:
            errors.append(f'exit anchor {anchor_index} is not an Exit_Normal walk trigger')
    elif kind == 'keyed_door_approach':
        if (line.get('special') != '12' or line.get('playeruse') != 'true' or
                integer(line.get('locknumber')) != anchor['lock'] or anchor['lock'] <= 0):
            errors.append(f'keyed-door anchor {anchor_index} is not a matching manual Door_Raise')
        keyed_anchor_lines.add(line_index)
    else:
        if (line.get('special') != '11' or line.get('playeruse') != 'true' or
                line.get('playercross') == 'true' or integer(line.get('arg0')) != anchor['tag'] or
                anchor['tag'] <= 0):
            errors.append(f'manual-switch anchor {anchor_index} is not an explicit Door_Open switch')
        switch_anchor_lines.add(line_index)
    # The exit's clear reservation is a circular pad at the walkover midpoint;
    # its trigger line is deliberately wider than that pad. Door/switch
    # reservations instead follow their actual usable linedef span.
    if kind == 'exit':
        clearance_shapes.append((anchor['x'], anchor['y'], anchor['x'], anchor['y'],
                                 anchor['clear_radius'], kind, anchor_index))
    else:
        clearance_shapes.append((x1, y1, x2, y2, anchor['clear_radius'], kind, anchor_index))

expect(len(by_kind['start']) == 1,
       f'collision-navigation proof must witness one start, got {len(by_kind["start"])}')
expect(len(by_kind['exit']) == 1,
       f'collision-navigation proof must witness one exit, got {len(by_kind["exit"])}')
expected_key_things = {index for index, thing in enumerate(things)
                       if thing.get('type') in {'5', '6', '13'}}
actual_key_things = {anchor['thing_index'] for anchor in by_kind['key']}
expect(actual_key_things == expected_key_things,
       'collision-navigation key anchors do not cover exactly the serialized keys')
keyed_lines = {index for index, line in enumerate(lines)
               if line.get('special') == '12' and (integer(line.get('locknumber')) or 0) > 0}
expect(keyed_anchor_lines == keyed_lines,
       'collision-navigation keyed-door anchors do not cover exactly the keyed faces')
keyed_sectors = collections.defaultdict(list)
for line_index in keyed_lines:
    sideback = integer(lines[line_index].get('sideback'))
    if sideback is None or not 0 <= sideback < len(sides):
        errors.append(f'keyed line {line_index} has no valid back side')
        continue
    sector = integer(sides[sideback].get('sector'))
    if sector is None:
        errors.append(f'keyed line {line_index} has no valid door sector')
        continue
    keyed_sectors[sector].append(line_index)
for sector, face_lines in keyed_sectors.items():
    if len(face_lines) != 2:
        errors.append(f'keyed door sector {sector} has {len(face_lines)} faces, not two')
manual_lines = {index for index, line in enumerate(lines)
                if line.get('special') == '11' and line.get('playeruse') == 'true' and
                line.get('playercross') != 'true'}
expect(switch_anchor_lines == manual_lines,
       'collision-navigation switch anchors do not cover exactly explicit switches')
if 'keyed_door_approaches_reached' in navigation:
    expect(navigation['keyed_door_approaches_reached'] == len(keyed_anchor_lines),
           'collision-navigation keyed-door count differs from its UDMF witnesses')
if 'manual_switches_reached' in navigation:
    expect(navigation['manual_switches_reached'] == len(switch_anchor_lines),
           'collision-navigation switch count differs from its UDMF witnesses')

def line_sector_pair(line):
    front_side = integer(line.get('sidefront'))
    back_side = integer(line.get('sideback'))
    if front_side is None or back_side is None or \
            not 0 <= front_side < len(sides) or not 0 <= back_side < len(sides):
        return None
    front_sector = integer(sides[front_side].get('sector'))
    back_sector = integer(sides[back_side].get('sector'))
    if front_sector is None or back_sector is None:
        return None
    return front_sector, back_sector

def line_connects(line_index, first_sector, second_sector):
    if not 0 <= line_index < len(lines):
        return False
    pair = line_sector_pair(lines[line_index])
    return pair is not None and set(pair) == {first_sector, second_sector}

def sector_opening(line):
    pair = line_sector_pair(line)
    if pair is None:
        return None
    front_sector, back_sector = pair
    if not 0 <= front_sector < len(sectors) or not 0 <= back_sector < len(sectors):
        return None
    floor_front = number(sectors[front_sector].get('heightfloor'))
    floor_back = number(sectors[back_sector].get('heightfloor'))
    ceil_front = number(sectors[front_sector].get('heightceiling'))
    ceil_back = number(sectors[back_sector].get('heightceiling'))
    if None in (floor_front, floor_back, ceil_front, ceil_back):
        return None
    return min(ceil_front, ceil_back) - max(floor_front, floor_back)

def sector_path(start_sector, target_sector, opened_door_sector=-1, opened_faces=frozenset()):
    if not 0 <= start_sector < len(sectors) or not 0 <= target_sector < len(sectors):
        return False
    seen = {start_sector}
    queue = collections.deque([start_sector])
    while queue:
        current = queue.popleft()
        if current == target_sector:
            return True
        for line_index, line in enumerate(lines):
            pair = line_sector_pair(line)
            if pair is None:
                continue
            front_sector, back_sector = pair
            if current == front_sector:
                next_sector = back_sector
            elif current == back_sector:
                next_sector = front_sector
            else:
                continue
            if next_sector in seen:
                continue
            if line_index in opened_faces:
                walkable = (opened_door_sector >= 0 and
                            opened_door_sector in pair and
                            line.get('blocking') != 'true')
            elif line.get('blocking') == 'true':
                walkable = False
            elif line.get('special') == '12':
                walkable = line.get('playeruse') == 'true'
            else:
                opening = sector_opening(line)
                walkable = opening is not None and opening >= 56.0 - 0.001
            if not walkable:
                continue
            seen.add(next_sector)
            queue.append(next_sector)
    return False

def point_in_sector(px, py, sector_index):
    crossings = 0
    for line_index, line in enumerate(lines):
        front_side = integer(line.get('sidefront'))
        back_side = integer(line.get('sideback'))
        front_sector = (integer(sides[front_side].get('sector'))
                        if front_side is not None and 0 <= front_side < len(sides)
                        else None)
        back_sector = (integer(sides[back_side].get('sector'))
                       if back_side is not None and 0 <= back_side < len(sides)
                       else None)
        if (sector_index not in {front_sector, back_sector} or
                (front_sector == sector_index and back_sector == sector_index) or
                line_index not in line_geometry):
            continue
        x1, y1, x2, y2 = line_geometry[line_index]
        if (y1 > py) == (y2 > py):
            continue
        crossing_x = x1 + (py - y1) * (x2 - x1) / (y2 - y1)
        if crossing_x > px:
            crossings += 1
    return crossings % 2 == 1

# Multiplayer membership is deliberately not a procedural input: every map
# contains the native P1--P8 start set. Audit the manifest's separate proof
# against the serialized things and collision lanes without changing the
# canonical P1-only key-state anchor above.
cooperative = navigation.get('cooperative_starts')
cooperative_fields = {
    'status', 'native_slots', 'canonical_player',
    'minimum_separation', 'starts',
}
if not isinstance(cooperative, dict):
    errors.append('collision-navigation proof has no cooperative-start evidence')
    cooperative = {}
else:
    missing_fields = sorted(cooperative_fields - cooperative.keys())
    if missing_fields:
        errors.append('cooperative-start proof is missing ' + ', '.join(missing_fields))
if cooperative.get('status') != 'proven':
    errors.append('cooperative-start proof status is not proven')
expect(cooperative.get('native_slots') == 8,
       'cooperative-start proof must cover exactly native P1 through P8')
expect(cooperative.get('canonical_player') == 1,
       'cooperative-start proof must retain P1 as the canonical solver origin')
minimum_separation = cooperative.get('minimum_separation')
minimum_separation_valid = (isinstance(minimum_separation, (int, float)) and
                            not isinstance(minimum_separation, bool) and
                            math.isfinite(minimum_separation) and
                            minimum_separation >= 48.0 - 0.001)
if not minimum_separation_valid:
    errors.append('cooperative-start proof violates the 48-unit separation contract')
cooperative_records = cooperative.get('starts')
if not isinstance(cooperative_records, list) or len(cooperative_records) != 8:
    errors.append('cooperative-start proof must contain exactly eight records')
    cooperative_records = []
native_start_types = (1, 2, 3, 4, 4001, 4002, 4003, 4004)
seen_cooperative_indices = set()
seen_cooperative_types = set()
cooperative_positions = []
landmark_sectors = set()
for slot, record in enumerate(cooperative_records, 1):
    fields = {'player', 'thing_index', 'thing_type', 'room', 'landmark_sector',
              'x', 'y', 'clear_radius'}
    if not isinstance(record, dict):
        errors.append(f'cooperative start P{slot} is not an object')
        continue
    missing_fields = sorted(fields - record.keys())
    if missing_fields:
        errors.append(f'cooperative start P{slot} is missing ' + ', '.join(missing_fields))
        continue
    if record.get('player') != slot or record.get('thing_type') != native_start_types[slot - 1]:
        errors.append(f'cooperative start P{slot} has the wrong native slot/type')
    integer_fields = ('player', 'thing_index', 'thing_type', 'room', 'landmark_sector')
    if any(not isinstance(record[field], int) or isinstance(record[field], bool)
           for field in integer_fields):
        errors.append(f'cooperative start P{slot} has invalid integer metadata')
        continue
    x, y, clear_radius = (record[field] for field in ('x', 'y', 'clear_radius'))
    if any(not isinstance(value, (int, float)) or isinstance(value, bool) or
           not math.isfinite(value) for value in (x, y, clear_radius)) or clear_radius < 48.0 - 0.001:
        errors.append(f'cooperative start P{slot} has no conservative clear pad')
        continue
    thing_index = record['thing_index']
    if thing_index in seen_cooperative_indices or not 0 <= thing_index < len(things):
        errors.append(f'cooperative start P{slot} has a duplicate or invalid UDMF thing index')
        continue
    seen_cooperative_indices.add(thing_index)
    if record['thing_type'] in seen_cooperative_types:
        errors.append(f'cooperative start P{slot} duplicates a native start type')
    seen_cooperative_types.add(record['thing_type'])
    thing = things[thing_index]
    thing_x, thing_y = number(thing.get('x')), number(thing.get('y'))
    if (thing.get('type') != str(record['thing_type']) or thing_x is None or thing_y is None or
            math.hypot(thing_x - x, thing_y - y) > 0.01):
        errors.append(f'cooperative start P{slot} does not match its UDMF thing')
    landmark_sector = record['landmark_sector']
    if record['room'] < 0 or not 0 <= landmark_sector < len(sectors):
        errors.append(f'cooperative start P{slot} has an invalid landmark owner')
    elif not point_in_sector(x, y, landmark_sector):
        errors.append(f'cooperative start P{slot} lies outside its landmark sector')
    else:
        landmark_sectors.add(landmark_sector)
    cooperative_positions.append((slot, x, y))
    clearance_shapes.append((x, y, x, y, clear_radius,
                             'cooperative_start', slot))
if len(seen_cooperative_indices) != 8 or seen_cooperative_types != set(native_start_types):
    errors.append('cooperative-start proof does not cover each serialized native start exactly once')
if len(landmark_sectors) != 1:
    errors.append('cooperative starts are not all staged in one landmark sector')
for first_index, first in enumerate(cooperative_positions):
    for second in cooperative_positions[:first_index]:
        separation = math.hypot(first[1] - second[1], first[2] - second[2])
        if separation < 48.0 - 0.001:
            errors.append(f'cooperative starts P{second[0]}/P{first[0]} are too close: '
                          f'{separation:.3f} < 48')
if len(cooperative_positions) > 1 and minimum_separation_valid:
    actual_minimum = min(math.hypot(first[1] - second[1], first[2] - second[2])
                         for first_index, first in enumerate(cooperative_positions)
                         for second in cooperative_positions[:first_index])
    if abs(actual_minimum - float(minimum_separation)) > 0.01:
        errors.append('cooperative-start manifest separation does not match serialized positions')
if len(by_kind['start']) == 1 and cooperative_records:
    canonical = cooperative_records[0]
    start_anchor = by_kind['start'][0]
    if (start_anchor['thing_index'] != canonical.get('thing_index') or
            math.hypot(start_anchor['x'] - canonical.get('x', 0),
                       start_anchor['y'] - canonical.get('y', 0)) > 0.01):
        errors.append('canonical P1 accessibility anchor disagrees with cooperative-start proof')

switch_caches = navigation.get('switch_caches')
if not isinstance(switch_caches, list):
    errors.append('collision-navigation proof has no switch-cache witnesses')
    switch_caches = []
else:
    expect(navigation.get('switch_cache_actions') == len(switch_caches),
           'collision-navigation switch-cache action count differs from witnesses')
if require_switch_cache:
    expect(len(switch_caches) > 0,
           'focused collision-navigation case expected an explicit switch cache')
cache_reward_total = 0
cache_tags = set()
for cache_index, cache in enumerate(switch_caches):
    if not isinstance(cache, dict):
        errors.append(f'switch-cache witness {cache_index} is not an object')
        continue
    fields = {'tag', 'switch_line', 'source_sector', 'cache_sector',
              'door_sector', 'closet_sector',
              'source_door_line', 'closet_door_line',
              'source_door_clear_radius', 'closet_door_clear_radius',
              'reward_first_thing', 'reward_thing_count'}
    missing_fields = sorted(fields - cache.keys())
    if missing_fields:
        errors.append(f'switch-cache witness {cache_index} is missing ' +
                      ', '.join(missing_fields))
        continue
    int_fields = fields - {'source_door_clear_radius', 'closet_door_clear_radius'}
    if any(not isinstance(cache[field], int) or isinstance(cache[field], bool)
           for field in int_fields) or \
            any(not isinstance(cache[field], (int, float)) or
                isinstance(cache[field], bool) or not math.isfinite(cache[field])
                for field in ('source_door_clear_radius', 'closet_door_clear_radius')):
        errors.append(f'switch-cache witness {cache_index} has invalid field types')
        continue
    tag = cache['tag']
    if tag <= 0 or tag in cache_tags:
        errors.append(f'switch-cache witness {cache_index} has duplicate/invalid tag {tag}')
    cache_tags.add(tag)
    switch_line = cache['switch_line']
    source_sector = cache['source_sector']
    cache_sector = cache['cache_sector']
    door_sector = cache['door_sector']
    closet_sector = cache['closet_sector']
    source_door_line = cache['source_door_line']
    closet_door_line = cache['closet_door_line']
    if not all(0 <= sector < len(sectors)
               for sector in (source_sector, cache_sector, door_sector, closet_sector)):
        errors.append(f'switch-cache witness {cache_index} has invalid sector reference')
        continue
    if switch_line not in line_geometry or not 0 <= switch_line < len(lines):
        errors.append(f'switch-cache witness {cache_index} has invalid switch line')
        continue
    switch_line_data = lines[switch_line]
    switch_front = integer(switch_line_data.get('sidefront'))
    switch_front_sector = (integer(sides[switch_front].get('sector'))
                           if switch_front is not None and 0 <= switch_front < len(sides)
                           else None)
    if (switch_line_data.get('special') != '11' or
            switch_line_data.get('playeruse') != 'true' or
            switch_line_data.get('playercross') == 'true' or
            integer(switch_line_data.get('arg0')) != tag or
            switch_front_sector != source_sector):
        errors.append(f'switch-cache witness {cache_index} does not name its explicit source action')
    door = sectors[door_sector]
    if (integer(door.get('id')) != tag or
            number(door.get('heightfloor')) is None or
            number(door.get('heightceiling')) is None or
            abs(number(door.get('heightfloor')) - number(door.get('heightceiling'))) > 0.01):
        errors.append(f'switch-cache witness {cache_index} does not name a closed tagged door sector')
    if source_door_line == closet_door_line or \
            not line_connects(source_door_line, cache_sector, door_sector) or \
            not line_connects(closet_door_line, closet_sector, door_sector):
        errors.append(f'switch-cache witness {cache_index} does not name both physical cache door faces')
        continue
    for face_name, line_index, clear_radius in (
            ('source', source_door_line, cache['source_door_clear_radius']),
            ('closet', closet_door_line, cache['closet_door_clear_radius'])):
        if (line_index not in line_geometry or clear_radius < 32.0 or
                lines[line_index].get('blocking') == 'true'):
            errors.append(f'switch-cache witness {cache_index} has invalid {face_name} door clearance')
            continue
        x1, y1, x2, y2 = line_geometry[line_index]
        if math.hypot(x2 - x1, y2 - y1) < 64.0 - 0.01:
            errors.append(f'switch-cache witness {cache_index} {face_name} door face is too narrow')
    if sector_path(source_sector, closet_sector):
        errors.append(f'switch-cache witness {cache_index} reaches its closet before use')
    if not sector_path(source_sector, closet_sector, door_sector,
                       frozenset((source_door_line, closet_door_line))):
        errors.append(f'switch-cache witness {cache_index} cannot reach its closet after use')
    reward_first = cache['reward_first_thing']
    reward_count = cache['reward_thing_count']
    if reward_first < 0 or reward_count <= 0 or reward_first + reward_count > len(things):
        errors.append(f'switch-cache witness {cache_index} has invalid reward range')
        continue
    expected_reward_indices = set(range(reward_first, reward_first + reward_count))
    actual_reward_indices = {anchor['thing_index']
                             for anchor in cache_reward_anchor_things.get(tag, [])
                             if anchor['thing_index'] in expected_reward_indices}
    if actual_reward_indices != expected_reward_indices:
        errors.append(f'switch-cache witness {cache_index} reward pads are not tag-tied anchors')
    for thing_index in expected_reward_indices:
        thing = things[thing_index]
        x, y = number(thing.get('x')), number(thing.get('y'))
        if x is None or y is None or not point_in_sector(x, y, closet_sector):
            errors.append(f'switch-cache witness {cache_index} reward {thing_index} is outside its closet sector')
    cache_reward_total += reward_count
expect(navigation.get('switch_cache_rewards_reached') == cache_reward_total,
       'collision-navigation switch-cache reward count differs from witnesses')

if isinstance(accessibility, dict) and 'mandatory_anchors' in accessibility:
    expect(accessibility['mandatory_anchors'] ==
           len(by_kind['start']) + len(by_kind['key']) + len(by_kind['exit']),
           'accessibility mandatory count differs from stable collision anchors')

# Every exported physical lane receives the same decoration overlap audit as a
# start/key/door anchor. In particular this independently protects non-unified
# same-room gaps, which intentionally have no portal linedef to inspect.
clearance_shapes.extend(corridor_lane_shapes)

# Repeat the static collision budget for every exported pad/door/switch anchor.
# This independently catches a later decoration pass that stops honoring a
# reservation while leaving the topological key-state graph connected.
solid_radius = {
    '43': 32.0, '48': 32.0, '2028': 32.0,
    '35': 24.0, '41': 24.0, '44': 24.0, '45': 24.0, '46': 24.0,
    '55': 24.0, '56': 24.0, '57': 24.0, '85': 24.0, '86': 24.0,
    '2035': 24.0,
}
safety = max(16.0, float(navigation.get('safety_margin', 16)))
solid_props = []
for thing_index, thing in enumerate(things):
    radius = solid_radius.get(thing.get('type'))
    x, y = number(thing.get('x')), number(thing.get('y'))
    if radius is None or x is None or y is None:
        continue
    solid_props.append((thing_index, x, y, radius))
    for ax, ay, bx, by, clear_radius, kind, anchor_index in clearance_shapes:
        if point_segment_distance(x, y, ax, ay, bx, by) < clear_radius + radius + safety - 0.01:
            errors.append(f'solid decoration {thing_index} overlaps {kind} '
                          f'collision anchor {anchor_index}')
            break
for first_index, first in enumerate(solid_props):
    for second in solid_props[first_index + 1:]:
        if math.hypot(first[1] - second[1], first[2] - second[2]) < \
                first[3] + second[3] + 48.0 - 0.01:
            errors.append(f'solid decorations {first[0]} and {second[0]} form a player-blocking pinch')
            break

for error in errors:
    print(f'    {error}')
if errors:
    raise SystemExit(1)
print('  collision-clear UDMF navigation proof passed: ' +
      f'anchors={len(anchors)} corridors={navigation.get("proven_corridors", 0)} '
      f'keyed_faces={len(keyed_anchor_lines)} switches={len(switch_anchor_lines)}')
PY
}

# Validate the native-metric wall phase contract from the emitted UDMF rather
# than trusting a manifest summary. The proof carries only a bounded sample,
# but the engine verifies every resolved world/architectural surface before it
# serializes that sample.
validate_manifest_udmf_alignment() {
	local manifest_path=$1
	local udmf_path=$2
	local require_all_kinds=${3:-0}
	python3 - "$manifest_path" "$udmf_path" "$require_all_kinds" <<'PY'
import json
import math
import re
import sys

manifest_path, udmf_path, require_all_kinds = sys.argv[1:]
require_all_kinds = require_all_kinds == '1'
errors = []

def load_json(path):
    try:
        with open(path, encoding='utf-8') as handle:
            return json.load(handle)
    except (OSError, json.JSONDecodeError) as error:
        errors.append(f'could not parse alignment manifest {path}: {error}')
        return {}

def load_udmf(path):
    try:
        with open(path, encoding='utf-8') as handle:
            return handle.read()
    except OSError as error:
        errors.append(f'could not read alignment UDMF {path}: {error}')
        return ''

def blocks(text, kind):
    return [dict((key, value.strip('"')) for key, value in
                 re.findall(r'^\s*(\w+)\s*=\s*([^;]+);', body, re.M))
            for body in re.findall(r'(?m)^' + kind + r'\s*\n\{(.*?)\n\}', text, re.S)]

def as_int(value):
    try:
        return int(value)
    except (TypeError, ValueError):
        return None

def as_float(value):
    try:
        result = float(value)
    except (TypeError, ValueError):
        return None
    return result if math.isfinite(result) else None

def positive_mod(value, width):
    result = math.fmod(value, width)
    return result + width if result < 0.0 else result

def llround(value):
    return math.floor(value + 0.5) if value >= 0.0 else math.ceil(value - 0.5)

mask = 0xffffffff
def mix_alignment_hash(value):
    value &= mask
    value ^= value >> 16
    value = (value * 0x7feb352d) & mask
    value ^= value >> 15
    value = (value * 0x846ca68b) & mask
    value ^= value >> 16
    return value & mask

def texture_name_hash(texture):
    value = 2166136261
    for byte in texture.encode('utf-8'):
        value ^= byte
        value = (value * 16777619) & mask
    return value

manifest = load_json(manifest_path)
text = load_udmf(udmf_path)
visual = manifest.get('visual_proof') if isinstance(manifest, dict) else None
alignment = visual.get('alignment') if isinstance(visual, dict) else None
if not isinstance(alignment, dict) or alignment.get('status') != 'proven':
    errors.append('manifest lacks a proven active-IWAD alignment proof')
    alignment = {}

metrics = alignment.get('metrics')
witnesses = alignment.get('witnesses')
metric_by_texture = {}
if not isinstance(metrics, list) or not metrics:
    errors.append('alignment proof has no texture metrics')
else:
    for index, metric in enumerate(metrics):
        if not isinstance(metric, dict):
            errors.append(f'alignment metric {index} is not an object')
            continue
        texture = metric.get('texture')
        width = metric.get('width')
        height = metric.get('height')
        fallback = metric.get('fallback')
        if not isinstance(texture, str) or not texture:
            errors.append(f'alignment metric {index} has no texture name')
            continue
        if not isinstance(width, int) or width <= 0 or not isinstance(height, int) or height <= 0:
            errors.append(f'alignment metric {index} has invalid native dimensions')
            continue
        if not isinstance(fallback, bool):
            errors.append(f'alignment metric {index} has invalid fallback flag')
            continue
        prior = metric_by_texture.get(texture)
        if prior is not None and prior != (width, height, fallback):
            errors.append(f'alignment metric {texture} has conflicting cached dimensions')
        metric_by_texture[texture] = (width, height, fallback)

vertices = blocks(text, 'vertex')
sides = blocks(text, 'sidedef')
lines = blocks(text, 'linedef')
if not isinstance(witnesses, list) or not witnesses:
    errors.append('alignment proof has no final UDMF witnesses')
    witnesses = []

part_index = {'top': 0, 'mid': 1, 'bottom': 2}
part_texture = {'top': 'texturetop', 'mid': 'texturemiddle', 'bottom': 'texturebottom'}
seen_kinds = set()
kind_counts = {'world': 0, 'two_sided': 0, 'stair': 0, 'portal': 0}
for index, witness in enumerate(witnesses):
    if not isinstance(witness, dict):
        errors.append(f'alignment witness {index} is not an object')
        continue
    required = {'kind', 'texture', 'part', 'mode', 'vertical_anchor', 'line', 'side',
                'width', 'height', 'alignment_group', 'phase_origin', 'phase_shift',
                'reverse', 'two_sided', 'offset_x', 'offset_y', 'scale_x', 'scale_y'}
    missing = sorted(required - witness.keys())
    if missing:
        errors.append(f'alignment witness {index} is missing {", ".join(missing)}')
        continue
    kind = witness['kind']
    part = witness['part']
    mode = witness['mode']
    if kind not in kind_counts or part not in part_index or mode not in {'world', 'architectural'}:
        errors.append(f'alignment witness {index} has invalid kind/part/mode')
        continue
    line_index = witness['line']
    side_index = witness['side']
    if not isinstance(line_index, int) or not isinstance(side_index, int) or \
            not (0 <= line_index < len(lines)) or not (0 <= side_index < len(sides)):
        errors.append(f'alignment witness {index} references an invalid line/side')
        continue
    line = lines[line_index]
    if as_int(line.get('sidefront')) != side_index and as_int(line.get('sideback')) != side_index:
        errors.append(f'alignment witness {index} side is not owned by its linedef')
        continue
    two_sided = 'sideback' in line
    if witness['two_sided'] is not two_sided:
        errors.append(f'alignment witness {index} has a stale two_sided flag')
    if not isinstance(witness['reverse'], bool):
        errors.append(f'alignment witness {index} has non-boolean reverse')
        continue
    texture = witness['texture']
    if not isinstance(texture, str) or sides[side_index].get(part_texture[part], '-') != texture:
        errors.append(f'alignment witness {index} texture does not match emitted sidedef')
        continue
    metric = metric_by_texture.get(texture)
    if metric is None or metric[:2] != (witness['width'], witness['height']):
        errors.append(f'alignment witness {index} does not use its serialized active-IWAD metric')
        continue
    width, height, _fallback = metric
    if not isinstance(witness['alignment_group'], int) or not isinstance(witness['phase_origin'], int) or \
            not isinstance(witness['phase_shift'], int):
        errors.append(f'alignment witness {index} has invalid phase fields')
        continue
    actual_x = as_float(sides[side_index].get('offsetx', '0'))
    part_x = as_float(sides[side_index].get(f'offsetx_{part}', '0'))
    actual_y = as_float(sides[side_index].get('offsety', '0'))
    part_y = as_float(sides[side_index].get(f'offsety_{part}', '0'))
    scale_x = as_float(sides[side_index].get(f'scalex_{part}', '1'))
    scale_y = as_float(sides[side_index].get(f'scaley_{part}', '1'))
    expected_numbers = (witness['offset_x'], witness['offset_y'], witness['scale_x'], witness['scale_y'])
    if any(not isinstance(value, (int, float)) or isinstance(value, bool) or not math.isfinite(value)
           for value in expected_numbers) or None in (actual_x, part_x, actual_y, part_y, scale_x, scale_y):
        errors.append(f'alignment witness {index} has non-finite transform data')
        continue
    if abs((actual_x + part_x) - witness['offset_x']) > 0.001 or \
            abs((actual_y + part_y) - witness['offset_y']) > 0.001 or \
            abs(scale_x - witness['scale_x']) > 0.001 or abs(scale_y - witness['scale_y']) > 0.001:
        errors.append(f'alignment witness {index} transform differs from final UDMF')
    v1, v2 = as_int(line.get('v1')), as_int(line.get('v2'))
    if v1 is None or v2 is None or not (0 <= v1 < len(vertices) and 0 <= v2 < len(vertices)):
        errors.append(f'alignment witness {index} has invalid linedef vertices')
        continue
    first = vertices[v2 if witness['reverse'] else v1]
    second = vertices[v1 if witness['reverse'] else v2]
    values = tuple(as_float(point.get(axis)) for point in (first, second) for axis in ('x', 'y'))
    if any(value is None for value in values):
        errors.append(f'alignment witness {index} has nonnumeric vertex coordinates')
        continue
    x1, y1, x2, y2 = values
    dx, dy = x2 - x1, y2 - y1
    origin = x1 if abs(dx) >= abs(dy) and dx >= 0 else \
        (-x1 if abs(dx) >= abs(dy) else (y1 if dy >= 0 else -y1))
    origin = llround(origin)
    if origin != witness['phase_origin']:
        errors.append(f'alignment witness {index} phase origin is not derived from its line direction')
    expected_shift = 0
    if mode == 'architectural':
        group = witness['alignment_group']
        if group < 0:
            errors.append(f'alignment witness {index} architectural run has no group')
            continue
        expected_shift = mix_alignment_hash((group & mask) ^ texture_name_hash(texture) ^
                                             ((part_index[part] + 1) * 0x85ebca6b)) % width
    if witness['phase_shift'] != expected_shift:
        errors.append(f'alignment witness {index} has an invalid native-metric phase shift')
    expected_phase = positive_mod(origin + expected_shift, width)
    if abs(positive_mod(actual_x + part_x, width) - expected_phase) > 0.001:
        errors.append(f'alignment witness {index} breaks its {mode} phase continuity')
    expected_anchor = ('upper_ceiling' if line.get('dontpegtop') == 'true' else 'upper_band') \
        if part == 'top' else ('lower_floor' if line.get('dontpegbottom') == 'true' else 'lower_band')
    if witness['vertical_anchor'] != expected_anchor:
        errors.append(f'alignment witness {index} has a stale vertical anchor')
    if kind == 'world' and (mode != 'world' or two_sided):
        errors.append(f'alignment witness {index} misclassifies a world wall')
    if kind == 'two_sided' and not two_sided:
        errors.append(f'alignment witness {index} misclassifies a two-sided band')
    if kind == 'two_sided' and mode == 'architectural' and witness['alignment_group'] < 0:
        errors.append(f'alignment witness {index} loses its architectural run group')
    if kind in {'stair', 'portal'} and mode != 'architectural':
        errors.append(f'alignment witness {index} misclassifies an architectural run')
    seen_kinds.add(kind)
    kind_counts[kind] += 1

for kind, field in {
        'world': 'world_witnesses', 'two_sided': 'two_sided_witnesses',
        'stair': 'stair_witnesses', 'portal': 'portal_witnesses'}.items():
    if alignment.get(field) != kind_counts[kind]:
        errors.append(f'alignment {field} does not match its witness records')
if require_all_kinds:
    missing = sorted(set(kind_counts) - seen_kinds)
    if missing:
        errors.append(f'focused alignment fixture lacks witness kinds: {", ".join(missing)}')

# Moving door art is a separate alignment contract from ordinary wall bands.
# It must use the metric resolved by this exact IWAD, remain at native
# horizontal scale, and center the *whole repeated pattern* over the physical
# slab.  Checking the midpoint phase catches the old `max(0, crop)` behavior:
# it happened to work for a compact door, but left a 176/224-wide opening
# anchored to an arbitrary tile edge whenever its texture repeated.
door_sides = 0
repeating_door_sides = 0
for line_index, line in enumerate(lines):
    if as_int(line.get('special')) != 12:
        continue
    v1, v2 = as_int(line.get('v1')), as_int(line.get('v2'))
    if v1 is None or v2 is None or not (0 <= v1 < len(vertices) and 0 <= v2 < len(vertices)):
        errors.append(f'door line {line_index} has invalid vertices')
        continue
    x1, y1 = as_float(vertices[v1].get('x')), as_float(vertices[v1].get('y'))
    x2, y2 = as_float(vertices[v2].get('x')), as_float(vertices[v2].get('y'))
    if None in (x1, y1, x2, y2):
        errors.append(f'door line {line_index} has nonnumeric vertices')
        continue
    face_width = math.hypot(x2 - x1, y2 - y1)
    secret = line.get('secret') == 'true'
    side_indices = (as_int(line.get('sidefront')), as_int(line.get('sideback')))
    if None in side_indices or any(not 0 <= side_index < len(sides) for side_index in side_indices):
        errors.append(f'door line {line_index} has incomplete front/back faces')
        continue
    for side_index in side_indices:
        side = sides[side_index]
        texture = side.get('texturetop')
        metric = metric_by_texture.get(texture)
        if metric is None:
            errors.append(f'door line {line_index} texture {texture!r} has no active-IWAD metric')
            continue
        native_width = 128 if secret else metric[0]
        shared_x = as_float(side.get('offsetx', '0'))
        part_x = as_float(side.get('offsetx_top', '0'))
        scale_x = as_float(side.get('scalex_top', '1'))
        if None in (shared_x, part_x, scale_x):
            errors.append(f'door line {line_index} has nonnumeric horizontal transform')
            continue
        expected_phase = llround((native_width - face_width) * 0.5)
        actual_phase = shared_x + part_x
        if abs(actual_phase - expected_phase) > 0.01:
            errors.append(f'door line {line_index} does not center {texture} across its {face_width:.1f}-unit face')
        if abs(scale_x - 1.0) > 0.001:
            errors.append(f'door line {line_index} horizontally scales {texture} instead of repeating natively')
        midpoint_phase = positive_mod(actual_phase + face_width * 0.5, native_width)
        if abs(midpoint_phase - native_width * 0.5) > 0.501:
            errors.append(f'door line {line_index} does not place {texture}\'s native center at the slab midpoint')
        door_sides += 1
        if face_width > native_width + 0.01:
            repeating_door_sides += 1
if door_sides == 0:
    errors.append('alignment fixture has no Door_Raise texture faces')
if require_all_kinds and repeating_door_sides == 0:
    errors.append('focused alignment fixture has no natively repeating door faces')

for error in errors:
    print(f'    {error}')
if errors:
    raise SystemExit(1)
print('  native-metric UDMF phase proof passed: ' +
      ', '.join(f'{kind}={kind_counts[kind]}' for kind in sorted(kind_counts)) +
      f', door_faces={door_sides}, repeating_door_faces={repeating_door_sides}')
PY
}

validate_manifest_difficulty_curve() {
	local low_manifest=$1
	local high_manifest=$2
	python3 - "$low_manifest" "$high_manifest" <<'PY'
import json
import sys

def load(path):
    try:
        with open(path, encoding='utf-8') as handle:
            return json.load(handle)
    except (OSError, json.JSONDecodeError) as error:
        print(f'    could not parse difficulty manifest {path}: {error}')
        raise SystemExit(1)

low = load(sys.argv[1])
high = load(sys.argv[2])
errors = []

def main_path(manifest):
    return sorted((room for room in manifest.get('rooms', []) if room.get('main_path')),
                  key=lambda room: (room.get('rank', -1), room.get('id', -1)))

low_rooms = main_path(low)
high_rooms = main_path(high)
low_pressure = sum(room.get('threat', 0) for room in low_rooms)
high_pressure = sum(room.get('threat', 0) for room in high_rooms)
if low.get('difficulty') != 1:
    errors.append(f'low-difficulty manifest reports {low.get("difficulty")!r}, expected 1')
if high.get('difficulty') != 5:
    errors.append(f'high-difficulty manifest reports {high.get("difficulty")!r}, expected 5')
if not low_rooms or not high_rooms:
    errors.append('difficulty manifests have no main-path rooms')
elif high_pressure < low_pressure:
    errors.append(f'high difficulty lowers main-path threat ({high_pressure} < {low_pressure})')
if high_rooms and max(room.get('threat', 0) for room in high_rooms) < max(
        (room.get('threat', 0) for room in low_rooms), default=0):
    errors.append('high difficulty lowers the maximum planned main-path threat')

for error in errors:
    print(f'    {error}')
if errors:
    raise SystemExit(1)
print(f'  difficulty threat progression passed: difficulty1={low_pressure} difficulty5={high_pressure}')
PY
}

validate_manifest_corpus() {
	local cases_file=$1
	python3 - "$cases_file" <<'PY'
import collections
import json
import math
import re
import sys

cases_file = sys.argv[1]
profiles = {'expedition', 'assault', 'infiltration', 'circuit', 'siege'}
orientations = {'eastbound', 'westbound', 'northbound', 'southbound'}
motifs = {
    'watercourse', 'vertical_pressure', 'remote_reveal', 'shrine_secrets',
    'sightline_recon',
}
tracks = {'ballistic', 'demolition', 'energy'}
finales = {'duel', 'siege', 'gauntlet', 'fortress'}
cards = {
    'breather', 'skirmish', 'crossfire', 'pincer', 'ambush',
    'cache_challenge', 'holding_line', 'set_piece',
}
themes = {'techbase', 'industrial', 'hell', 'gothic', 'corrupted'}
stage_shapes = {'spine', 'fork_rejoin', 'ring', 'switchback', 'courtyard_spokes'}
landmark_archetypes = {
    'court', 'nave', 'gatehouse', 'shrine_terrace', 'bridge_basin',
    'bastion', 'fortress',
}
district_roles = {'entry', 'transit', 'work', 'sanctum', 'defense', 'finale'}
implemented_vertical_intents = {
    'stair_hall', 'dogleg_ascent', 'dogleg_descent',
    'terrace_overlook', 'bridge_approach',
}

errors = []
seen_profiles = set()
seen_orientations = set()
seen_motifs = set()
seen_tracks = set()
seen_finales = set()
seen_cards = set()
seen_themes = set()
seen_requested_shapes = set()
seen_realized_shapes = set()
seen_requested_landmark_archetypes = set()
seen_realized_landmark_archetypes = set()
seen_district_roles = set()
seen_vertical_intents = set()
seen_footprints = set()
seen_material_families = set()
seen_connection_profiles = set()
visual_metadata_cases = 0
connection_metadata_cases = 0
cooperative_start_metadata_cases = 0
unified_envelope_count = 0
realized_non_safe_room_count = 0
realized_non_safe_footprints = set()
primary_footprints_by_theme = collections.defaultdict(set)
material_families_by_theme = collections.defaultdict(set)
themes_with_non_orthogonal_geometry = set()
dramatic_small_target_cases = 0
dramatic_large_target_cases = 0
dramatic_small_realized_cases = 0
dramatic_large_realized_cases = 0
dramatic_large_optional_witness_cases = 0
signatures = collections.defaultdict(set)
case_count = 0

def blocks(text, kind):
    return [dict((key, value.strip('"')) for key, value in
                 re.findall(r'^\s*(\w+)\s*=\s*([^;]+);', body, re.M))
            for body in re.findall(r'(?m)^' + kind + r'\s*\n\{(.*?)\n\}', text, re.S)]

def graph_shape(path):
    try:
        with open(path, encoding='utf-8') as handle:
            text = handle.read()
    except OSError as error:
        raise ValueError(f'cannot read UDMF {path}: {error}')
    vertices = blocks(text, 'vertex')
    sectors = blocks(text, 'sector')
    sides = blocks(text, 'sidedef')
    lines = blocks(text, 'linedef')
    adjacency = {index: set() for index in range(len(sectors))}
    for line in lines:
        if 'sideback' not in line:
            continue
        try:
            front = int(sides[int(line['sidefront'])]['sector'])
            back = int(sides[int(line['sideback'])]['sector'])
        except (KeyError, IndexError, ValueError):
            continue
        if front != back:
            adjacency[front].add(back)
            adjacency[back].add(front)
    degree_histogram = sorted(collections.Counter(
        len(neighbors) for neighbors in adjacency.values()).items())
    locks = sorted(int(line['locknumber']) for line in lines
                   if line.get('locknumber', '').isdigit())
    non_orthogonal_lines = 0
    diagonal_slopes = set()
    for line in lines:
        try:
            first = vertices[int(line['v1'])]
            second = vertices[int(line['v2'])]
            dx = float(second['x']) - float(first['x'])
            dy = float(second['y']) - float(first['y'])
        except (KeyError, IndexError, ValueError):
            continue
        if abs(dx) > 0.001 and abs(dy) > 0.001:
            non_orthogonal_lines += 1
            diagonal_slopes.add(round(abs(dy / dx), 3))
    return {
        'sectors': len(sectors),
        'two_sided_edges': sum(len(neighbors) for neighbors in adjacency.values()) // 2,
        'degree_histogram': degree_histogram,
        'locked_crossings': locks,
        'non_orthogonal_lines': non_orthogonal_lines,
        'diagonal_slopes': sorted(diagonal_slopes),
    }

def unified_envelope_evidence(path, rooms):
    """Cross-check a manifest's unified room claim against final UDMF sides.

    The generator's post-emission proof rejects former same-room cell faces;
    this independent parser also requires every advertised exterior rectangle
    to own serialized boundary geometry on all four sides.
    """
    try:
        with open(path, encoding='utf-8') as handle:
            text = handle.read()
    except OSError as error:
        errors.append(f'cannot read unified-envelope UDMF {path}: {error}')
        return 0
    vertices = blocks(text, 'vertex')
    sides = blocks(text, 'sidedef')
    lines = blocks(text, 'linedef')
    actual = 0
    for room in rooms:
        if not isinstance(room, dict) or not room.get('contour_unified'):
            continue
        sector = room.get('contour_sector')
        bounds = room.get('contour_bounds')
        if (not isinstance(sector, int) or not isinstance(bounds, list) or len(bounds) != 4 or
                not all(isinstance(value, (int, float)) and math.isfinite(value) for value in bounds)):
            errors.append(f'unified room {room.get("id")} lacks usable UDMF contour facts')
            continue
        min_x, min_y, max_x, max_y = bounds
        expected = {'left', 'right', 'bottom', 'top'}
        realized = set()
        for line in lines:
            try:
                side_indices = [int(line['sidefront'])]
                if 'sideback' in line:
                    side_indices.append(int(line['sideback']))
                if not any(int(sides[index]['sector']) == sector for index in side_indices):
                    continue
                first = vertices[int(line['v1'])]
                second = vertices[int(line['v2'])]
                x1, y1 = float(first['x']), float(first['y'])
                x2, y2 = float(second['x']), float(second['y'])
            except (KeyError, IndexError, TypeError, ValueError):
                errors.append(f'unified room {room.get("id")} has malformed UDMF boundary data')
                break
            epsilon = 0.01
            if abs(x1 - min_x) <= epsilon and abs(x2 - min_x) <= epsilon:
                realized.add('left')
            if abs(x1 - max_x) <= epsilon and abs(x2 - max_x) <= epsilon:
                realized.add('right')
            if abs(y1 - min_y) <= epsilon and abs(y2 - min_y) <= epsilon:
                realized.add('bottom')
            if abs(y1 - max_y) <= epsilon and abs(y2 - max_y) <= epsilon:
                realized.add('top')
        missing = expected - realized
        if missing:
            errors.append(f'unified room {room.get("id")} has no final UDMF boundary on '
                          f'{", ".join(sorted(missing))}')
            continue
        actual += 1
    return actual

with open(cases_file, encoding='utf-8') as handle:
    cases = [line.rstrip('\n').split('\t') for line in handle if line.strip()]

for fields in cases:
    if len(fields) != 4:
        errors.append(f'malformed replayability corpus entry: {fields!r}')
        continue
    path, udmf_path, size, theme = fields
    try:
        with open(path, encoding='utf-8') as handle:
            manifest = json.load(handle)
    except (OSError, json.JSONDecodeError) as error:
        errors.append(f'cannot read corpus manifest {path}: {error}')
        continue
    try:
        map_graph = graph_shape(udmf_path)
    except ValueError as error:
        errors.append(str(error))
        continue
    case_count += 1
    cooperative = (manifest.get('accessibility', {})
                   if isinstance(manifest.get('accessibility'), dict) else {})
    cooperative = (cooperative.get('collision_navigation', {})
                   if isinstance(cooperative.get('collision_navigation'), dict) else {})
    cooperative = cooperative.get('cooperative_starts')
    native_start_types = (1, 2, 3, 4, 4001, 4002, 4003, 4004)
    if not isinstance(cooperative, dict):
        errors.append(f'corpus manifest {path} is missing cooperative-start evidence')
    else:
        starts = cooperative.get('starts')
        valid_cooperative = (
            cooperative.get('status') == 'proven' and
            cooperative.get('native_slots') == 8 and
            cooperative.get('canonical_player') == 1 and
            isinstance(cooperative.get('minimum_separation'), (int, float)) and
            not isinstance(cooperative.get('minimum_separation'), bool) and
            math.isfinite(cooperative['minimum_separation']) and
            cooperative['minimum_separation'] >= 48.0 - 0.001 and
            isinstance(starts, list) and len(starts) == 8)
        if valid_cooperative:
            for slot, start in enumerate(starts, 1):
                if (not isinstance(start, dict) or start.get('player') != slot or
                        start.get('thing_type') != native_start_types[slot - 1] or
                        not isinstance(start.get('thing_index'), int) or
                        not isinstance(start.get('landmark_sector'), int) or
                        not isinstance(start.get('clear_radius'), (int, float)) or
                        isinstance(start.get('clear_radius'), bool) or
                        start['clear_radius'] < 48.0 - 0.001):
                    valid_cooperative = False
                    break
        if valid_cooperative:
            cooperative_start_metadata_cases += 1
        else:
            errors.append(f'corpus manifest {path} has incomplete cooperative-start evidence')
    if map_graph['non_orthogonal_lines'] > 0:
        themes_with_non_orthogonal_geometry.add(theme)
    profile = manifest.get('profile', '').casefold()
    orientation = manifest.get('orientation', '').casefold()
    track = manifest.get('arsenal_track')
    finale = manifest.get('finale')
    seen_profiles.add(profile)
    seen_orientations.add(orientation)
    seen_tracks.add(track)
    seen_finales.add(finale)
    seen_themes.add(theme)
    seen_motifs.update(manifest.get('motifs', []))
    visual_proof = manifest.get('visual_proof')
    if not isinstance(visual_proof, dict):
        errors.append(f'corpus manifest {path} is missing visual_proof')
    elif {'status', 'alignment_groups', 'geometry', 'connector', 'elevation'} - visual_proof.keys():
        errors.append(f'corpus manifest {path} has incomplete visual_proof')
    else:
        alignment = visual_proof.get('alignment')
        required_alignment = {
            'status', 'metric_textures', 'fallback_textures',
            'world_witnesses', 'two_sided_witnesses', 'stair_witnesses',
            'portal_witnesses', 'metrics', 'witnesses',
        }
        if not isinstance(alignment, dict) or required_alignment - alignment.keys():
            errors.append(f'corpus manifest {path} has incomplete alignment proof')
    stages = manifest.get('stages', [])
    if not isinstance(stages, list):
        errors.append(f'corpus manifest {path} is missing its stages array')
        stages = []
    for stage in stages:
        if not isinstance(stage, dict):
            errors.append(f'corpus manifest {path} has a non-object stage')
            continue
        if {'material_family', 'elevation_role'} - stage.keys():
            errors.append(f'corpus manifest {path} has a stage without spatial metadata')
        seen_requested_shapes.add(stage.get('shape'))
        seen_realized_shapes.add(stage.get('realized_shape'))
        requested_archetype = stage.get('landmark_archetype')
        if requested_archetype != 'none':
            seen_requested_landmark_archetypes.add(requested_archetype)
        realized_archetype = stage.get('realized_landmark_archetype')
        if realized_archetype != 'none':
            seen_realized_landmark_archetypes.add(realized_archetype)
        seen_district_roles.add(stage.get('district_role'))
        if isinstance(stage.get('material_family'), str) and stage['material_family']:
            seen_material_families.add(stage['material_family'])
            material_families_by_theme[theme].add(stage['material_family'])
        intent = stage.get('realized_vertical_intent')
        if intent != 'flat':
            seen_vertical_intents.add(intent)
    rooms = manifest.get('rooms', [])
    if not isinstance(rooms, list):
        errors.append(f'corpus manifest {path} is missing its rooms array')
        rooms = []
    main_rooms = sorted(
        (room for room in rooms if isinstance(room, dict) and room.get('main_path')),
        key=lambda room: (room.get('rank', -1), room.get('id', -1)))
    for room in rooms:
        if not isinstance(room, dict):
            errors.append(f'corpus manifest {path} has a non-object room')
            continue
        if {'footprint', 'requested_footprint', 'realized_footprint',
            'footprint_fallback', 'contour_unified', 'contour_loops',
            'contour_sector', 'contour_bounds', 'material_family', 'floor_z', 'clear_height',
            'contour_vertices', 'contour_edges', 'contour_area', 'contour_width',
            'contour_height', 'elevation_role'} - room.keys():
            errors.append(f'corpus manifest {path} has a room without spatial metadata')
        realized_footprint = room.get('realized_footprint')
        if (isinstance(room.get('footprint'), str) and room['footprint'] and
                room.get('footprint') == realized_footprint):
            seen_footprints.add(realized_footprint)
            if realized_footprint != 'safe_shell':
                realized_non_safe_room_count += 1
                realized_non_safe_footprints.add(realized_footprint)
            if room.get('main_path'):
                primary_footprints_by_theme[theme].add(realized_footprint)
        else:
            errors.append(f'corpus room {room.get("id")} advertises an untruthful footprint label')
        if isinstance(room.get('material_family'), str) and room['material_family']:
            seen_material_families.add(room['material_family'])
            material_families_by_theme[theme].add(room['material_family'])
    unified_envelope_count += unified_envelope_evidence(udmf_path, rooms)
    connections = manifest.get('connections')
    if isinstance(connections, list):
        if all(isinstance(connection, dict) and
               {'source', 'target', 'source_cell', 'target_cell', 'route_role',
                'mandatory', 'keyed', 'profile', 'clear_width', 'depth', 'has_door',
                'door_kind', 'door_clear_width', 'door_art_width', 'door_art_height',
                'rise', 'stair_chain', 'alignment_group'} <= connection.keys()
               for connection in connections):
            connection_metadata_cases += 1
        for connection in connections:
            if isinstance(connection, dict) and isinstance(connection.get('profile'), str):
                seen_connection_profiles.add(connection['profile'])
            else:
                errors.append(f'corpus manifest {path} has an incomplete connection witness')
    else:
        errors.append(f'corpus manifest {path} is missing its connections array')
        connections = []
    if isinstance(visual_proof, dict):
        visual_metadata_cases += 1
    if {'main_route_elevation_target', 'optional_elevation_target',
        'realized_main_route_elevation', 'realized_optional_elevation'} - manifest.keys():
        errors.append(f'corpus manifest {path} lacks realized terrain metadata')
    target = manifest.get('main_route_elevation_target')
    realized_target = manifest.get('realized_main_route_elevation')
    optional_target = manifest.get('optional_elevation_target')
    realized_optional = manifest.get('realized_optional_elevation')
    if isinstance(target, int):
        if 3 <= int(size) <= 4 and 128 <= abs(target) <= 192:
            dramatic_small_target_cases += 1
            if (isinstance(realized_target, int) and
                    128 <= abs(realized_target) <= 192 and
                    realized_target * target > 0):
                dramatic_small_realized_cases += 1
        if int(size) >= 5 and 192 <= abs(target) <= 320:
            dramatic_large_target_cases += 1
            if (isinstance(realized_target, int) and
                    192 <= abs(realized_target) <= 320 and
                    realized_target * target > 0):
                dramatic_large_realized_cases += 1
            optional_role = 'highland' if isinstance(realized_optional, int) and \
                realized_optional > 0 else 'basin'
            if (isinstance(optional_target, int) and isinstance(realized_target, int) and
                    isinstance(realized_optional, int) and
                    192 <= abs(realized_optional) <= 320 and
                    realized_optional * realized_target < 0 and
                    realized_optional * optional_target > 0 and
                    any(not room.get('main_path') and
                        room.get('floor_z') == realized_optional and
                        room.get('elevation_role') == optional_role
                        for room in rooms if isinstance(room, dict))):
                dramatic_large_optional_witness_cases += 1
    seen_cards.update(room.get('card') for room in rooms
                      if isinstance(room, dict) and room.get('card') != 'none')
    # A structural/beat signature deliberately excludes raw room IDs and
    # UDMF hashes. It combines the serialized sector graph with macro-stage
    # shape/archetype/vertical plans, gate order, cards, arsenal, rewards, and
    # recovery curve so a cosmetic room shuffle cannot satisfy a profile's
    # diversity requirement.
    signature = json.dumps({
        'graph_shape': map_graph,
        'orientation': orientation,
        'theme': theme,
        'gate_order': manifest.get('key_order'),
        'motifs': manifest.get('motifs'),
        'arsenal_track': track,
        'finale': finale,
        'terrain_targets': [manifest.get('main_route_elevation_target'),
                            manifest.get('optional_elevation_target'),
                            manifest.get('realized_main_route_elevation'),
                            manifest.get('realized_optional_elevation')],
        'stages': [
            [stage.get('stage'), stage.get('shape'), stage.get('realized_shape'),
             stage.get('landmark_archetype'), stage.get('realized_landmark_archetype'),
             stage.get('district_role'), stage.get('vertical_intent'),
             stage.get('realized_vertical_intent'), stage.get('vertical_rise'),
             stage.get('realized_vertical_rise'), stage.get('material_family'),
             stage.get('elevation_role'),
             stage.get('gate_rank')]
            for stage in stages if isinstance(stage, dict)
        ],
        'main_path': [
            [room.get('rank'), room.get('beat'), room.get('card'), room.get('motif'), room.get('district'),
             room.get('stage'), room.get('stage_shape'), room.get('district_role'),
             room.get('landmark_archetype'), room.get('vertical_intent'),
             room.get('vertical_rise'), room.get('vertical_anchor'), room.get('manual_interaction'),
             room.get('threat'), room.get('recovery'), room.get('weapon'),
             room.get('ammo'), room.get('ammo_count'), room.get('reward'), room.get('optional_armory'),
             room.get('requested_footprint'), room.get('realized_footprint'),
             room.get('contour_unified'), room.get('contour_loops'),
             room.get('footprint'), room.get('material_family'), room.get('floor_z'),
             room.get('clear_height'), room.get('contour_vertices'), room.get('contour_edges'),
             room.get('contour_area'), room.get('contour_width'), room.get('contour_height'),
             room.get('footprint_variant'), room.get('contour_inset'),
             room.get('footprint_fallback'), room.get('elevation_role'), room.get('elevation_target')]
            for room in main_rooms
        ],
        'connections': [
            [connection.get('source'), connection.get('target'), connection.get('route_role'),
             connection.get('profile'), connection.get('clear_width'), connection.get('depth'),
             connection.get('has_door'), connection.get('door_kind'),
             connection.get('door_clear_width'), connection.get('door_art_width'),
             connection.get('rise'), connection.get('stair_chain'), connection.get('alignment_group')]
            for connection in connections if isinstance(connection, dict)
        ],
        'cooperative_starts': cooperative,
        'visual_proof': manifest.get('visual_proof'),
    }, sort_keys=True, separators=(',', ':'))
    signatures[profile].add(signature)

def require_coverage(name, expected, actual):
    missing = sorted(expected - actual)
    if missing:
        errors.append(f'replayability corpus misses {name}: {", ".join(missing)}')

require_coverage('profiles', profiles, seen_profiles)
require_coverage('route orientations', orientations, seen_orientations)
require_coverage('themes', themes, seen_themes)
require_coverage('feature motifs', motifs, seen_motifs)
require_coverage('arsenal tracks', tracks, seen_tracks)
require_coverage('finale cards', finales, seen_finales)
require_coverage('encounter cards', cards, seen_cards)
require_coverage('requested macro shapes', stage_shapes, seen_requested_shapes)
require_coverage('realized macro shapes', stage_shapes, seen_realized_shapes)
require_coverage('requested landmark archetypes', landmark_archetypes,
                 seen_requested_landmark_archetypes)
require_coverage('realized landmark archetypes', landmark_archetypes,
                 seen_realized_landmark_archetypes)
require_coverage('district roles', district_roles, seen_district_roles)
require_coverage('implemented non-flat vertical intents', implemented_vertical_intents,
                 seen_vertical_intents)
for profile in sorted(profiles):
    count = len(signatures[profile])
    if count < 2:
        errors.append(f'profile {profile} has only {count} player-visible structural/beat signature(s)')
# Current schema-1 manifests must carry this spatial evidence in every corpus
# case. Do not let a removed field turn the coverage checks below into a no-op.
if visual_metadata_cases != case_count:
    errors.append(f'visual manifest metadata is missing from '
                  f'{case_count - visual_metadata_cases} corpus case(s)')
if connection_metadata_cases != case_count:
    errors.append(f'connection manifest metadata is missing from '
                  f'{case_count - connection_metadata_cases} corpus case(s)')
if cooperative_start_metadata_cases != case_count:
    errors.append(f'cooperative-start manifest metadata is missing from '
                  f'{case_count - cooperative_start_metadata_cases} corpus case(s)')
if len(seen_footprints) < 2:
    errors.append(f'visual corpus covered only one room footprint: {sorted(seen_footprints)}')
if unified_envelope_count < 10:
    errors.append(f'visual corpus realizes only {unified_envelope_count} UDMF-backed unified envelopes')
if realized_non_safe_room_count < 10 or len(realized_non_safe_footprints) < 2:
    errors.append('visual corpus lacks enough actually realized non-safe footprint grammar: ' +
                  f'rooms={realized_non_safe_room_count} kinds={sorted(realized_non_safe_footprints)}')
if len(seen_connection_profiles) < 2:
    errors.append('visual corpus covered only one connector profile')
if len(seen_material_families) < 2:
    errors.append('visual corpus covered only one material family')
if case_count:
    require_coverage('connector profiles',
                     {'narrow', 'standard', 'gallery', 'grand'},
                     {profile.casefold() for profile in seen_connection_profiles})
if case_count:
    # These are corpus properties, not per-map quotas: a constrained map may
    # legitimately take a safe rectangular fallback, but the curated matrix
    # must still demonstrate the authored grammar in every theme.
    non_safe_primary = {
        footprint for footprints in primary_footprints_by_theme.values()
        for footprint in footprints if footprint != 'safe_shell'
    }
    if len(non_safe_primary) < 2:
        errors.append('visual corpus lacks a mix of asymmetric/concave primary-room footprints')
    missing_primary_themes = sorted(
        theme for theme in themes if not (primary_footprints_by_theme[theme] - {'safe_shell'}))
    if missing_primary_themes:
        errors.append('visual corpus lacks a non-rectilinear primary room for theme(s): ' +
                      ', '.join(missing_primary_themes))
    missing_material_themes = sorted(
        theme for theme in themes if len(material_families_by_theme[theme]) < 2)
    if missing_material_themes:
        errors.append('visual corpus lacks multiple district material families for theme(s): ' +
                      ', '.join(missing_material_themes))
    missing_diagonal_themes = sorted(themes - themes_with_non_orthogonal_geometry)
    if missing_diagonal_themes:
        errors.append('visual corpus lacks non-orthogonal contour geometry for theme(s): ' +
                      ', '.join(missing_diagonal_themes))
    if dramatic_small_target_cases == 0:
        errors.append('visual corpus lacks a size-3/4 Dramatic highland or basin target')
    if dramatic_small_realized_cases == 0:
        errors.append('visual corpus lacks a realized size-3/4 Dramatic highland or basin')
    if dramatic_large_target_cases == 0:
        errors.append('visual corpus lacks a size-5+ Dramatic highland or basin target')
    if dramatic_large_realized_cases == 0:
        errors.append('visual corpus lacks a realized size-5+ Dramatic main-route highland or basin')
    if dramatic_large_optional_witness_cases == 0:
        errors.append('visual corpus lacks a reachable opposite-altitude Dramatic optional district')

for error in errors:
    print(f'    {error}')
if errors:
    raise SystemExit(1)
print(f'  replayability coverage passed: cases={case_count} profiles={len(seen_profiles)} '
      f'orientations={len(seen_orientations)} themes={len(seen_themes)} motifs={len(seen_motifs)} '
      f'arsenals={len(seen_tracks)} finales={len(seen_finales)} cards={len(seen_cards)} '
      f'shapes={len(seen_realized_shapes)} landmarks={len(seen_realized_landmark_archetypes)} '
      f'vertical_intents={len(seen_vertical_intents)} footprints={len(seen_footprints)} '
      f'materials={len(seen_material_families)} connector_profiles={len(seen_connection_profiles)} '
      f'unified_envelopes={unified_envelope_count} '
      f'realized_non_safe_rooms={realized_non_safe_room_count} '
      f'visual_metadata_cases={visual_metadata_cases} '
      f'connection_metadata_cases={connection_metadata_cases} '
      f'cooperative_start_metadata_cases={cooperative_start_metadata_cases}')
PY
}

validate_dump() {
	local size=$1
	local theme=${2:-techbase}
	local verticality=${3:-1}
	local detail=${4:-1}
	local layout=${5:-1}
    local failures=0
    local sectors things players exits locks keys monsters ammo health direct_health health_bonuses
    local decorations weapons super_shotguns
    local min_sectors max_sectors max_things min_monsters max_monsters
    local unique_walls unique_floors unique_ceilings
    sectors=$(count_blocks sector)
    things=$(count_blocks thing)
    players=$(grep -c '^\s*type = 1;' /tmp/procmap_test.udmf 2>/dev/null || true)
    exits=$(grep -c '^\s*special = 243;' /tmp/procmap_test.udmf 2>/dev/null || true)
    locks=$(grep -c '^\s*locknumber = ' /tmp/procmap_test.udmf 2>/dev/null || true)
    keys=$(grep -Ec '^\s*type = (5|6|13);' /tmp/procmap_test.udmf 2>/dev/null || true)
    monsters=$(grep -Ec '^\s*type = (7|9|16|58|64|65|66|67|68|69|71|72|84|88|89|3001|3002|3003|3004|3005|3006);' \
        /tmp/procmap_test.udmf 2>/dev/null || true)
    ammo=$(grep -Ec '^\s*type = (17|2007|2008|2010|2046|2047|2048|2049);' /tmp/procmap_test.udmf 2>/dev/null || true)
    health=$(grep -Ec '^\s*type = (2011|2012|2013|2014|2015|2018|2019);' /tmp/procmap_test.udmf 2>/dev/null || true)
    direct_health=$(grep -Ec '^\s*type = (2011|2012);' /tmp/procmap_test.udmf 2>/dev/null || true)
    health_bonuses=$(grep -c '^\s*type = 2014;' /tmp/procmap_test.udmf 2>/dev/null || true)
	decorations=$(grep -Ec '^\s*type = (15|20|35|41|43|44|45|46|48|55|56|57|59|60|85|86|2028|2035);' \
        /tmp/procmap_test.udmf 2>/dev/null || true)
    weapons=$(grep -Ec '^\s*type = (82|2001|2002|2003|2004|2005|2006);' /tmp/procmap_test.udmf 2>/dev/null || true)
    super_shotguns=$(grep -c '^\s*type = 82;' /tmp/procmap_test.udmf 2>/dev/null || true)
	unique_walls=$(sed -n 's/^\s*texturemiddle = "\([^"]*\)";/\1/p' /tmp/procmap_test.udmf |
		grep -Ev '^(DOORTRAK|DOORRED|DOORBLU|DOORYEL|STEP1)$' | sort -u | wc -l)
	unique_floors=$(sed -n 's/^\s*texturefloor = "\([^"]*\)";/\1/p' /tmp/procmap_test.udmf | sort -u | wc -l)
	unique_ceilings=$(sed -n 's/^\s*textureceiling = "\([^"]*\)";/\1/p' /tmp/procmap_test.udmf |
		grep -v '^F_SKY1$' | sort -u | wc -l)
    min_sectors=$((18 + size * 6))
    # Stair-served perches, sightline sills, macro-liquid banks, and inter-room
    # terraces are explicit sectors. The cap remains linear through the
    # shipping maximum while allowing authored structures instead of rewarding
    # flat empty space.
	max_sectors=$((200 + size * 80))
	if [ "$verticality" -eq 2 ]; then max_sectors=$((max_sectors + size * 15)); fi
	if [ "$detail" -eq 2 ]; then max_sectors=$((max_sectors + size * 8)); fi
	if [ "$layout" -eq 2 ]; then max_sectors=$((max_sectors + size * 12)); fi
    # Huge high-difficulty maps reserve up to one direct recovery pickup per
    # four monsters, in addition to encounter, decoration, and bonus actors.
	max_things=$((300 + size * 110))
	if [ "$detail" -eq 2 ]; then max_things=$((max_things + size * 25)); fi
	if [ "$layout" -eq 2 ]; then max_things=$((max_things + size * 25)); fi
	# Gothic landmarks deliberately carry denser candelabra and torch framing.
	# Keep that authored identity bounded linearly instead of forcing every theme
	# under the plainer Techbase/Industrial actor ceiling.
	if [ "$theme" = "gothic" ]; then max_things=$((max_things + size)); fi
    # Easy compact maps intentionally permit a slightly lighter opening run;
    # arena growth and the upper difficulties are covered by balance_test.
    min_monsters=$((14 + size * 6))
    # Blueprint set pieces and recovery-ledger encounters add bounded pressure
    # beyond the legacy encounter slope. Keep the ceiling linear at the true
    # size-160 limit (6,440), which covers the validated all-high run without
    # turning the budget into an unbounded exemption.
    max_monsters=$((40 + size * 25 + size * 15))

    if [ "$players" -ne 1 ]; then
        echo "    expected one player start, got $players"
        failures=$((failures + 1))
    fi
    if [ "$exits" -ne 1 ]; then
        echo "    expected one exit trigger, got $exits"
        failures=$((failures + 1))
    fi
    if [ "$keys" -lt 1 ] || [ "$keys" -gt 3 ]; then
        echo "    expected one to three keys, got $keys"
        failures=$((failures + 1))
    fi
    if [ "$locks" -ne $((keys * 2)) ]; then
        echo "    expected two lock linedefs per key, got locks=$locks keys=$keys"
        failures=$((failures + 1))
    fi
    if [ "$sectors" -lt "$min_sectors" ] || [ "$sectors" -gt "$max_sectors" ]; then
        echo "    sector budget out of range: $sectors (expected $min_sectors..$max_sectors)"
        failures=$((failures + 1))
    fi
    if [ "$things" -lt 30 ] || [ "$things" -gt "$max_things" ]; then
        echo "    thing budget out of range: $things (expected 30..$max_things)"
        failures=$((failures + 1))
    fi
    if [ "$monsters" -lt "$min_monsters" ] || [ "$monsters" -gt "$max_monsters" ]; then
        echo "    encounter budget out of range: $monsters (expected $min_monsters..$max_monsters)"
        failures=$((failures + 1))
    fi
    if [ $((ammo * 5)) -lt "$monsters" ]; then
        echo "    ammunition support is too sparse: ammo=$ammo monsters=$monsters"
        failures=$((failures + 1))
    fi
    if [ $((direct_health * 4)) -lt "$monsters" ] || [ $((health * 2)) -lt "$monsters" ]; then
        echo "    survival support is too sparse: direct-health=$direct_health bonuses=$health_bonuses total=$health monsters=$monsters"
        failures=$((failures + 1))
    fi
    if [ "$direct_health" -lt $((6 + size)) ]; then
        echo "    too few substantial recovery pickups: direct-health=$direct_health expected-at-least=$((6 + size))"
        failures=$((failures + 1))
    fi
	# Architectural windows, stepped landmarks, and large fluid banks legitimately
	# add sectors without demanding actor padding. Keep decoration authored and
	# bounded by map scale instead of tying it to an implementation-detail count.
	local min_decorations=$((4 + size * (detail + 1)))
	if [ "$decorations" -lt "$min_decorations" ]; then
        echo "    decorative vocabulary is too sparse: decorations=$decorations expected-at-least=$min_decorations"
        failures=$((failures + 1))
    fi
    if [ "$weapons" -lt 2 ]; then
        echo "    weapon progression is missing: weapons=$weapons"
        failures=$((failures + 1))
    fi
	if [ "$size" -ge 3 ] && { [ "$unique_walls" -lt 8 ] || [ "$unique_floors" -lt 8 ] || [ "$unique_ceilings" -lt 6 ]; }; then
		echo "    room surface variation is too low: walls=$unique_walls floors=$unique_floors ceilings=$unique_ceilings"
		failures=$((failures + 1))
	fi
    if [ "$(basename "$IWAD")" = "doom2.wad" ] && [ "$super_shotguns" -lt 1 ]; then
        echo "    Doom II weapon progression is missing the super shotgun"
        failures=$((failures + 1))
    fi
	if [ "$theme" = "hell" ]; then
		if ! grep -q '^\s*type = 41;' /tmp/procmap_test.udmf &&
				! grep -q '^\s*type = 59;' /tmp/procmap_test.udmf; then
			echo "    Hell finale semiotics are missing both the evil-eye and non-solid infernal fallback"
			failures=$((failures + 1))
		fi
		if ! grep -q '^\s*type = 43;' /tmp/procmap_test.udmf &&
				! grep -q '^\s*type = 60;' /tmp/procmap_test.udmf; then
			echo "    Hell outdoor semiotics are missing both the torch-tree and non-solid infernal fallback"
            failures=$((failures + 1))
        fi
        if grep -q '^\s*type = 5;' /tmp/procmap_test.udmf &&
                ! grep -q '^\s*type = 44;' /tmp/procmap_test.udmf &&
                ! grep -q 'texturemiddle = "DOORBLU"' /tmp/procmap_test.udmf; then
            echo "    blue key landmark lacks both its safe blue torch and blue keyed-door trim"
            failures=$((failures + 1))
        fi
        if grep -q '^\s*type = 13;' /tmp/procmap_test.udmf &&
                ! grep -q '^\s*type = 46;' /tmp/procmap_test.udmf &&
                ! grep -q 'texturemiddle = "DOORRED"' /tmp/procmap_test.udmf; then
            echo "    red key landmark lacks both its safe red torch and red keyed-door trim"
            failures=$((failures + 1))
        fi
        if grep -q '^\s*type = 6;' /tmp/procmap_test.udmf &&
                ! grep -q '^\s*type = 35;' /tmp/procmap_test.udmf &&
                ! grep -q 'texturemiddle = "DOORYEL"' /tmp/procmap_test.udmf; then
            echo "    yellow key landmark lacks both its safe gold candelabra and yellow keyed-door trim"
            failures=$((failures + 1))
        fi
	elif [ "$theme" != "gothic" ] && [ "$(basename "$IWAD")" = "doom2.wad" ]; then
        if ! grep -q '^\s*type = 85;' /tmp/procmap_test.udmf; then
            echo "    techbase landmarks are missing their Doom II lamp language"
            failures=$((failures + 1))
        fi
	elif [ "$theme" != "gothic" ] && ! grep -Eq '^\s*type = (48|2028);' /tmp/procmap_test.udmf; then
        echo "    techbase landmarks are missing their Ultimate Doom pillar fallback"
        failures=$((failures + 1))
    fi
    if [ "$theme" = "industrial" ]; then
        if ! grep -q 'texturemiddle = "SUPPORT3"' /tmp/procmap_test.udmf ||
                ! grep -q 'texturemiddle = "METAL1"' /tmp/procmap_test.udmf ||
                ! grep -q '^\s*type = 48;' /tmp/procmap_test.udmf ||
                ! grep -q '^\s*type = 2035;' /tmp/procmap_test.udmf; then
            echo "    industrial theme is missing heavy supports, metal bays, columns, or machinery barrels"
            failures=$((failures + 1))
        fi
	elif [ "$theme" = "gothic" ]; then
		if ! grep -q 'texturemiddle = "WOOD1"' /tmp/procmap_test.udmf ||
				! grep -Eq 'texturemiddle = "MARBLE[123]"' /tmp/procmap_test.udmf ||
				! grep -q '^\s*type = 35;' /tmp/procmap_test.udmf; then
			# Tall torches and TorchTree are optional solid props. They are
			# intentionally skipped whenever their conservative collision radius
			# would compromise an accessible landmark lane, so the enduring Gothic
			# grammar witness is its material pairing plus a safe candelabra.
			echo "    gothic theme is missing marble/wood architecture or safe candelabra rhythm"
			failures=$((failures + 1))
		fi
    elif [ "$theme" = "corrupted" ]; then
        if ! grep -Eq 'texturemiddle = "(STARTAN[23]|BROWN1|BROWN96|BROWNGRN|TEKWALL[14]|COMPSPAN|METAL1)"' \
                    /tmp/procmap_test.udmf ||
                ! grep -Eq 'texturemiddle = "(STONE[23]|GSTONE[12]|GSTVINE[12]|MARBLE[123]|WOOD1|SP_HOT1)"' \
                    /tmp/procmap_test.udmf ||
                ! grep -Eq '^\s*type = (55|56|57);' /tmp/procmap_test.udmf; then
            echo "    corrupted theme does not visibly transition from techbase to infernal language"
            failures=$((failures + 1))
        fi
    fi
    if grep -q 'texturemiddle = "-"' /tmp/procmap_test.udmf; then
        echo "    explicit missing middle texture found"
        failures=$((failures + 1))
    fi
	if ! validate_geometry "$size" "$verticality" "$detail"; then
        failures=$((failures + 1))
    fi
	if ! validate_key_progression; then
        failures=$((failures + 1))
    fi
    return "$failures"
}

case "${1:-validate}" in
    seeds)
        shift
        if [ $# -eq 0 ]; then
            seeds=(1 42 99 123 999 12345 0 7 13 21 501721273)
        else
            seeds=("$@")
        fi
        failures=0
        index=0
        for seed in "${seeds[@]}"; do
            size=$((index % 5 + 1))
            difficulty=$(((index * 2) % 5 + 1))
            if [ $((index % 2)) -eq 0 ]; then theme=techbase; else theme=hell; fi
            if [ "$seed" -eq 501721273 ]; then
                theme=hell
                difficulty=4
                size=1
            fi
            echo "=== seed=$seed theme=$theme difficulty=$difficulty size=$size ==="
            output=$(run_test "$seed" "$theme" "$difficulty" "$size")
            echo "$output" | grep -E "Dumped UDMF|Generation failed" || true
            if ! echo "$output" | grep -q "Dumped UDMF"; then
                failures=$((failures + 1))
                index=$((index + 1))
                continue
            fi
            report_dump
            if ! validate_dump "$size" "$theme"; then
                failures=$((failures + 1))
            fi
            index=$((index + 1))
        done
        if [ "$failures" -ne 0 ]; then
            echo "Seed stress validation failed for $failures configuration(s)"
            exit 1
        fi
        echo "Seed stress validation passed for ${#seeds[@]} configurations"
        ;;
    inspect)
        shift
        seed="${1:-12345}"
        run_test "$seed" | grep -E "Dumped UDMF|Generation failed"
        report_dump
        echo "--- locknumber lines ---"
        grep -n "locknumber" /tmp/procmap_test.udmf
        echo "--- key thing lines ---"
        grep -nE '^\s*type = (5|6|13);' /tmp/procmap_test.udmf
        echo "--- exit trigger ---"
        grep -n "special = 243" /tmp/procmap_test.udmf
        ;;
    size)
        shift
        for size in 1 3 5 10 20 40 80 160; do
            echo "=== size=$size ==="
            run_test 12345 techbase 3 "$size" | grep -E "Dumped UDMF|Generation failed"
            report_dump
        done
        ;;
	determinism)
        run_test 424242 techbase 3 3 >/dev/null
        first=$(sha256sum /tmp/procmap_test.udmf | cut -d' ' -f1)
        run_test 424242 techbase 3 3 >/dev/null
        second=$(sha256sum /tmp/procmap_test.udmf | cut -d' ' -f1)
        run_test 424243 techbase 3 3 >/dev/null
        different=$(sha256sum /tmp/procmap_test.udmf | cut -d' ' -f1)
        if [ "$first" != "$second" ] || [ "$first" = "$different" ]; then
            echo "Determinism check failed"
            exit 1
        fi
		same_dir=$(mktemp -d /tmp/procmap_sameprocess.XXXXXX) || exit 1
		same_first="$same_dir/first.json"
		same_second="$same_dir/second.json"
		if ! run_manifest_pair_same_process 1 techbase 4 6 1 1 1 1 \
				"$same_first" "$same_second" >/dev/null ||
				! cmp -s "$same_first" "$same_second"; then
			rm -rf "$same_dir"
			echo "Same-process manifest determinism check failed"
			exit 1
		fi
		rm -rf "$same_dir"
		echo "Determinism check passed: $first (including same-process blueprint reuse)"
		;;
	replayability)
		# This is intentionally sequential. Every recipe is generated in two fresh
		# processes for UDMF and two fresh processes for its manifest, which catches
		# both shared-RNG drift and accidentally cached planning state.
		suite_dir=$(mktemp -d /tmp/procmap_replayability.XXXXXX) || exit 1
		trap 'rm -rf "$suite_dir"; rm -f "$TEST_CONFIG"' EXIT
		cases_file="$suite_dir/cases.tsv"
			# Curated recipe corpus: two independently shaped runs for each profile,
			# plus deterministic feasibility cases for a realized ring, bastion, the
			# complete dramatic vertical-route trio, and both scenic vertical forms.
			# Together they cover all four
			# route orientations, three arsenal tracks, four finales, five motifs,
			# five macro shapes, seven landmark archetypes, the encounter cards, and
			# all five theme grammars.
		specs=(
			"1 techbase 4 6 1 1 1 1"
			"2 techbase 4 6 1 1 1 1"
			"4 techbase 4 6 1 1 1 1"
			"10 techbase 4 6 1 1 1 1"
			"5 techbase 4 6 1 1 1 1"
			"19 techbase 4 6 1 1 1 1"
			"6 techbase 4 6 1 1 1 1"
			"25 techbase 4 6 1 1 1 1"
				"41 techbase 4 6 1 1 1 1"
				"35 techbase 4 6 1 1 1 1"
			"33 techbase 4 6 2 1 1 1"
			"1 techbase 4 6 2 1 1 1"
			"0 techbase 4 6 2 2 1 1"
			"0 techbase 4 6 1 0 1 1"
			"6 techbase 4 6 1 0 1 1"
			# Assault + Gentle deterministically realizes the plain stair-hall
			# scenic vocabulary, complementing the bridge and terrace cases above.
			"4 techbase 4 6 1 0 1 1"
			# Regression: this compact Gothic Assault needs all three Dramatic
			# route beats while its first key branch leaves beside the only safe
			# stair junction. The branch anchor is not the key pad and must not
			# make an otherwise playable recipe fail generation.
			"972996588 gothic 3 3 2 2 1 2"
			"42 hell 4 6 1 1 1 1"
			"99 industrial 4 6 1 1 1 1"
			"123 gothic 4 6 1 1 1 1"
			# A complex safe-shell Gothic composition must retain and prove every
			# same-room central gap; it is the focused room-merge lane regression.
			"42 gothic 3 5 2 2 2 2"
			"999 corrupted 4 6 1 1 1 1"
			)
		failures=0
		index=0
		for spec in "${specs[@]}"; do
			read -r seed theme difficulty size layout verticality detail outdoors <<<"$spec"
			udmf_a="$suite_dir/case_${index}.udmf"
			udmf_b="$suite_dir/case_${index}.repeat.udmf"
			manifest_a="$suite_dir/case_${index}.manifest.json"
			manifest_b="$suite_dir/case_${index}.repeat.manifest.json"
			echo "=== replayability seed=$seed theme=$theme difficulty=$difficulty size=$size ==="

			output=$(run_test "$seed" "$theme" "$difficulty" "$size" "$layout" \
				"$verticality" "$detail" "$outdoors" "$udmf_a")
			if ! echo "$output" | grep -q 'Dumped UDMF' || [ ! -s "$udmf_a" ]; then
				echo "$output" | grep -E 'Generation failed|Dumped UDMF' || true
				failures=$((failures + 1))
				index=$((index + 1))
				continue
			fi
			output=$(run_test "$seed" "$theme" "$difficulty" "$size" "$layout" \
				"$verticality" "$detail" "$outdoors" "$udmf_b")
			if ! echo "$output" | grep -q 'Dumped UDMF' || [ ! -s "$udmf_b" ] ||
					! cmp -s "$udmf_a" "$udmf_b"; then
				echo "UDMF same-recipe determinism failed for seed=$seed"
				failures=$((failures + 1))
				index=$((index + 1))
				continue
			fi

			cp "$udmf_a" /tmp/procmap_test.udmf
			if ! validate_dump "$size" "$theme" "$verticality" "$detail" "$layout"; then
				echo "Structural/key-state validation failed for replayability seed=$seed"
				failures=$((failures + 1))
				index=$((index + 1))
				continue
			fi

			output=$(run_manifest "$seed" "$theme" "$difficulty" "$size" "$layout" \
				"$verticality" "$detail" "$outdoors" "$manifest_a")
			if [ ! -s "$manifest_a" ]; then
				echo "$output" | grep -E 'Generation failed|Dumped.*manifest' || true
				echo "Manifest generation failed for seed=$seed"
				failures=$((failures + 1))
				index=$((index + 1))
				continue
			fi
			output=$(run_manifest "$seed" "$theme" "$difficulty" "$size" "$layout" \
				"$verticality" "$detail" "$outdoors" "$manifest_b")
			if [ ! -s "$manifest_b" ] || ! cmp -s "$manifest_a" "$manifest_b"; then
				echo "Manifest same-recipe determinism failed for seed=$seed"
				failures=$((failures + 1))
				index=$((index + 1))
				continue
			fi
			require_room_merge=0
			require_switch_cache=0
			if [ "$seed" = "42" ] && [ "$theme" = "gothic" ] &&
					[ "$difficulty" = "3" ] && [ "$size" = "5" ] &&
					[ "$layout" = "2" ] && [ "$verticality" = "2" ] &&
					[ "$detail" = "2" ] && [ "$outdoors" = "2" ]; then
				require_room_merge=1
			fi
			if { [ "$seed" = "10" ] && [ "$theme" = "techbase" ] &&
					[ "$difficulty" = "4" ] && [ "$size" = "6" ] &&
					[ "$layout" = "1" ] && [ "$verticality" = "1" ] &&
					[ "$detail" = "1" ] && [ "$outdoors" = "1" ]; } ||
					{ [ "$seed" = "33" ] && [ "$theme" = "techbase" ] &&
					[ "$difficulty" = "4" ] && [ "$size" = "6" ] &&
					[ "$layout" = "2" ] && [ "$verticality" = "1" ] &&
					[ "$detail" = "1" ] && [ "$outdoors" = "1" ]; }; then
				require_switch_cache=1
			fi
			if ! validate_manifest "$manifest_a" "$size" "$difficulty" "$verticality" ||
					! validate_manifest_udmf_connectors "$manifest_a" "$udmf_a" ||
					! validate_manifest_udmf_collision_navigation "$manifest_a" "$udmf_a" \
						"$require_room_merge" "$require_switch_cache" ||
					! validate_manifest_udmf_alignment "$manifest_a" "$udmf_a"; then
				echo "Manifest/card-economy/connector/navigation/alignment validation failed for seed=$seed"
				failures=$((failures + 1))
				index=$((index + 1))
				continue
			fi
			printf '%s\t%s\t%s\t%s\n' "$manifest_a" "$udmf_a" "$size" "$theme" >>"$cases_file"
			index=$((index + 1))
		done
		if [ "$failures" -ne 0 ]; then
			echo "Replayability corpus failed for $failures recipe(s)"
			exit 1
		fi
		if ! validate_manifest_corpus "$cases_file"; then
			echo "Replayability corpus coverage failed"
			exit 1
		fi
		# The compact Dramatic regression must also become a live map, not just
		# serialize cleanly. This exercises the exact player-facing failure path
		# reported for the recipe above.
		regression_runtime_log="$suite_dir/gothic_dramatic_size3.log"
		if ! run_runtime_load 972996588 gothic 3 3 "$IWAD" "$regression_runtime_log" \
				3 2 2 1 2; then
			echo "Compact Gothic Dramatic regression failed runtime load"
			grep -Ei 'generation failed|invalid|node|texture|unclosed|dummy subsector' \
				"$regression_runtime_log" | tail -30 || true
			exit 1
		fi
		low_manifest="$suite_dir/difficulty_1.manifest.json"
		high_manifest="$suite_dir/difficulty_5.manifest.json"
		for difficulty in 1 5; do
			if [ "$difficulty" -eq 1 ]; then difficulty_manifest="$low_manifest"; else difficulty_manifest="$high_manifest"; fi
			output=$(run_manifest 1 techbase "$difficulty" 6 1 1 1 1 "$difficulty_manifest")
			if [ ! -s "$difficulty_manifest" ] ||
					! validate_manifest "$difficulty_manifest" 6 "$difficulty" 1; then
				echo "$output" | grep -E 'Generation failed|Dumped.*manifest' || true
				echo "Difficulty manifest generation failed for difficulty=$difficulty"
				exit 1
			fi
		done
		if ! validate_manifest_difficulty_curve "$low_manifest" "$high_manifest"; then
			echo "Difficulty threat progression validation failed"
			exit 1
		fi
		echo "Replayability corpus passed: ${#specs[@]} deterministic, structurally diverse runs"
		;;
	settings)
		# Hold seed/theme/difficulty/size constant and vary one menu setting at a
		# time. This proves that every exposed control changes its named generation
		# dimension instead of being a cosmetic menu value.
		declare -A setting_sectors setting_range setting_sky setting_features
		declare -A setting_decor setting_hash
		specs=(
			"layout0 0 1 1 1"
			"layout2 2 1 1 1"
			"vertical0 1 0 1 1"
			"vertical2 1 2 1 1"
			"detail0 1 1 0 1"
			"detail2 1 1 2 1"
			"outdoors0 1 1 1 0"
			"outdoors2 1 1 1 2"
		)
		for spec in "${specs[@]}"; do
			read -r label layout verticality detail outdoors <<<"$spec"
			echo "=== $label layout=$layout verticality=$verticality detail=$detail outdoors=$outdoors ==="
			output=$(run_test 314159 techbase 3 8 "$layout" "$verticality" "$detail" "$outdoors")
			if ! echo "$output" | grep -q 'Dumped UDMF'; then
				echo "$output" | grep -E 'Generation failed|Dumped UDMF' || true
				exit 1
			fi
			if ! validate_dump 8 techbase "$verticality" "$detail" "$layout"; then
				echo "Structural validation failed for $label"
				exit 1
			fi
			setting_sectors[$label]=$(count_blocks sector)
			setting_range[$label]=$(python3 - <<'PY'
import re
text = open('/tmp/procmap_test.udmf', encoding='utf-8').read()
floors = [float(value) for value in re.findall(r'heightfloor = ([^;]+);', text)]
print(round(max(floors) - min(floors)))
PY
			)
			setting_sky[$label]=$(grep -c 'textureceiling = "F_SKY1"' /tmp/procmap_test.udmf || true)
			setting_features[$label]=$(grep -Ec '^\s*id = (1[05][0-9][0-9]|2[0-9][0-9][0-9]|3[0-9][0-9][0-9]);' \
				/tmp/procmap_test.udmf || true)
			setting_decor[$label]=$(grep -Ec '^\s*type = (15|20|35|41|43|44|45|46|48|55|56|57|59|60|85|86|2028|2035);' \
				/tmp/procmap_test.udmf || true)
			setting_hash[$label]=$(sha256sum /tmp/procmap_test.udmf | cut -d' ' -f1)
			echo "  sectors=${setting_sectors[$label]} range=${setting_range[$label]} sky=${setting_sky[$label]} features=${setting_features[$label]} decorations=${setting_decor[$label]}"
		done
		if [ "${setting_sectors[layout2]}" -le "${setting_sectors[layout0]}" ]; then
			echo "Exploratory layout did not grow topology beyond Directed"
			exit 1
		fi
		if [ "${setting_range[vertical2]}" -le "${setting_range[vertical0]}" ]; then
			echo "Dramatic verticality did not exceed Gentle elevation range"
			exit 1
		fi
		if [ "${setting_features[detail2]}" -le "${setting_features[detail0]}" ] ||
				[ "${setting_decor[detail2]}" -le "${setting_decor[detail0]}" ]; then
			echo "Lavish detail did not add interactive architecture and decoration"
			exit 1
		fi
		if [ "${setting_sky[outdoors2]}" -le "${setting_sky[outdoors0]}" ]; then
			echo "Open-Air setting did not add sky courtyards"
			exit 1
		fi
		for pair in 'layout0 layout2' 'vertical0 vertical2' 'detail0 detail2' 'outdoors0 outdoors2'; do
			read -r first_label second_label <<<"$pair"
			if [ "${setting_hash[$first_label]}" = "${setting_hash[$second_label]}" ]; then
				echo "Setting pair $pair generated identical output"
				exit 1
			fi
		done
		for spec in '271828 hell 0 0 0 0 sparse' '161803 gothic 2 2 2 2 lavish'; do
			read -r seed theme layout verticality detail outdoors label <<<"$spec"
			log="/tmp/procmap_settings_${label}.log"
			if ! run_runtime_load "$seed" "$theme" 4 8 "$IWAD" "$log" 3 \
					"$layout" "$verticality" "$detail" "$outdoors"; then
				echo "Runtime/node validation failed for $label settings"
				grep -Ei 'error|failed|invalid|unknown|node|texture|dummy subsector' "$log" | tail -20 || true
				exit 1
			fi
		done
		echo "All four procedural menu controls materially affect validated generation"
		;;
	themes)
		# Compare themes under an identical recipe. These assertions cover structural
		# identity: openness, machinery, cathedral scale, corruption progression,
		# lighting, and texture vocabulary must diverge—not merely the final hash.
		declare -A theme_sky theme_lifts theme_clear theme_walls theme_hash
		fingerprints=()
		for theme in techbase hell industrial gothic corrupted; do
			echo "=== theme=$theme seed=777777 difficulty=3 size=8 ==="
			output=$(run_test 777777 "$theme" 3 8 1 1 1 1)
			if ! echo "$output" | grep -q 'Dumped UDMF'; then
				echo "$output" | grep -E 'Generation failed|Dumped UDMF' || true
				exit 1
			fi
			if ! validate_dump 8 "$theme" 1 1; then
				echo "Theme validation failed for $theme"
				exit 1
			fi
			theme_sky[$theme]=$(grep -c 'textureceiling = "F_SKY1"' /tmp/procmap_test.udmf || true)
			theme_lifts[$theme]=$(grep -Ec '^\s*id = 3[0-9][0-9][0-9];' /tmp/procmap_test.udmf || true)
			theme_clear[$theme]=$(python3 - <<'PY'
import re
text = open('/tmp/procmap_test.udmf', encoding='utf-8').read()
sectors = re.findall(r'(?m)^sector\s*\n\{(.*?)\n\}', text, re.S)
clear = []
for sector in sectors:
    floor = float(re.search(r'heightfloor = ([^;]+);', sector).group(1))
    ceiling = float(re.search(r'heightceiling = ([^;]+);', sector).group(1))
    if ceiling > floor:
        clear.append(ceiling - floor)
print(round(sum(clear) / len(clear)))
PY
			)
			theme_walls[$theme]=$(sed -n 's/^\s*texturemiddle = "\([^"]*\)";/\1/p' \
				/tmp/procmap_test.udmf | sort -u | wc -l)
			light_colors=$(sed -n 's/^\s*lightcolor = \([^;]*\);/\1/p' \
				/tmp/procmap_test.udmf | sort -u | wc -l)
			if [ "$light_colors" -lt 3 ]; then
				echo "$theme has only $light_colors authored lighting colors"
				exit 1
			fi
			theme_hash[$theme]=$(sha256sum /tmp/procmap_test.udmf | cut -d' ' -f1)
			fingerprints+=("${theme_hash[$theme]}")
			echo "  sky=${theme_sky[$theme]} lifts=${theme_lifts[$theme]} avg-clear=${theme_clear[$theme]} walls=${theme_walls[$theme]} light-colors=$light_colors"
		done
		if [ "$(printf '%s\n' "${fingerprints[@]}" | sort -u | wc -l)" -ne 5 ]; then
			echo "Theme matrix did not produce five distinct authored maps"
			exit 1
		fi
		if [ "${theme_sky[hell]}" -le "${theme_sky[industrial]}" ]; then
			echo "Hell is not more open-air than Industrial"
			exit 1
		fi
		if [ "${theme_lifts[industrial]}" -le "${theme_lifts[techbase]}" ]; then
			echo "Industrial did not add machinery/lift architecture"
			exit 1
		fi
		if [ "${theme_clear[gothic]}" -le "${theme_clear[techbase]}" ]; then
			echo "Gothic did not create taller cathedral volumes"
			exit 1
		fi
		if [ "${theme_walls[corrupted]}" -le "${theme_walls[techbase]}" ]; then
			echo "Corrupted Tech did not broaden its mixed texture vocabulary"
			exit 1
		fi
		echo "Five themes passed structural differentiation and texture/lighting validation"
		;;
	music)
		ensure_doom1_iwad || exit 2
		music_dir=/tmp/procmap_music_matrix
		rm -rf "$music_dir"
		mkdir -p "$music_dir"
		if ! capture_proc_music 12345 "$DOOM2_IWAD" "$music_dir/doom2_a.log" ||
				! capture_proc_music 12345 "$DOOM2_IWAD" "$music_dir/doom2_b.log" ||
				! capture_proc_music 54321 "$DOOM2_IWAD" "$music_dir/doom2_c.log" ||
				! capture_proc_music 12345 "$DOOM1_IWAD" "$music_dir/doom1.log"; then
			echo "Procedural soundtrack selection did not reach a loaded map"
			exit 1
		fi
		if ! python3 - "$DOOM2_IWAD" "$DOOM1_IWAD" "$music_dir" <<'PY'
import os
import re
import struct
import sys

doom2, doom1, root = sys.argv[1:]

def selection(name):
    text = open(os.path.join(root, name), encoding='utf-8').read()
    matches = re.findall(r'Procedural soundtrack selected from ([^:]+): (\S+)', text)
    if not matches:
        raise SystemExit(f'{name}: no procedural soundtrack selection')
    return matches[-1]

def wad_names(path):
    with open(path, 'rb') as wad:
        magic, count, directory = struct.unpack('<4sii', wad.read(12))
        if magic not in {b'IWAD', b'PWAD'} or count < 0 or directory < 0:
            raise SystemExit(f'{path}: invalid WAD')
        wad.seek(directory)
        result = set()
        for _ in range(count):
            entry = wad.read(16)
            if len(entry) != 16:
                raise SystemExit(f'{path}: truncated WAD directory')
            result.add(struct.unpack('<ii8s', entry)[2].rstrip(b'\0').decode('ascii').upper())
        return result

first = selection('doom2_a.log')
repeat = selection('doom2_b.log')
different = selection('doom2_c.log')
doom = selection('doom1.log')
errors = []
if first != repeat:
    errors.append(f'same seed changed soundtrack: {first} != {repeat}')
if first == different:
    errors.append(f'fixed differentiation seeds selected the same soundtrack: {first}')
if not re.fullmatch(r'MAP\d\d', first[0]) or first[0] not in wad_names(doom2):
    errors.append(f'Doom II selection is not an IWAD map: {first}')
if not re.fullmatch(r'E\dM\d+', doom[0]) or doom[0] not in wad_names(doom1):
    errors.append(f'Doom selection is not an IWAD map: {doom}')
if not first[1].startswith('$MUSIC_') or not doom[1].startswith('$MUSIC_'):
    errors.append(f'selected map has no IWAD music definition: {first}, {doom}')
for error in errors:
    print(f'  {error}')
if errors:
    raise SystemExit(1)
print(f'  deterministic Doom II choice={first[0]} {first[1]}')
print(f'  differentiated Doom II choice={different[0]} {different[1]}')
print(f'  Ultimate Doom choice={doom[0]} {doom[1]}')
PY
		then
			exit 1
		fi
		if ! python3 - "$ROOT/src/common/audio/sound/oalsound.cpp" <<'PY'
import sys

lines = open(sys.argv[1], encoding='utf-8').read().splitlines()
calls = [index for index, line in enumerate(lines)
         if 'alSourcef' in line and 'AL_DOPPLER_FACTOR' in line]
errors = []
if len(calls) != 3:
    errors.append(f'expected three per-source Doppler assignments, found {len(calls)}')
for index in calls:
    context = '\n'.join(lines[max(0, index - 2):index])
    if 'EXT_source_distance_model' not in context:
        errors.append(f'line {index + 1} applies AL_DOPPLER_FACTOR without its extension guard')
for error in errors:
    print(f'  {error}')
if errors:
    raise SystemExit(1)
print('  all sound-effect and stream Doppler assignments are extension-guarded')
PY
		then
			exit 1
		fi
		if ! run_software_midi_smoke 12345 "$DOOM2_IWAD" "$music_dir/software_midi.log"; then
			echo "FluidSynth/OpenAL streaming smoke test failed"
			grep -Ei 'openal|midi|music|error|unable|failed' "$music_dir/software_midi.log" | tail -50 || true
			exit 1
		fi
		echo "OpenAL error regression, software MIDI streaming, and deterministic IWAD soundtrack selection passed"
		;;
	features)
		feature_dir=/tmp/procmap_feature_matrix
		rm -rf "$feature_dir"
		mkdir -p "$feature_dir"
		ensure_doom1_iwad || exit 2
		if ! python3 - "$DOOM1_IWAD" "$DOOM2_IWAD" <<'PY'
import os
import struct
import sys

required = {'FWATER1', 'BLOOD1', 'NUKAGE1', 'LAVA1'}
for path in sys.argv[1:]:
    with open(path, 'rb') as wad:
        header = wad.read(12)
        if len(header) != 12:
            raise SystemExit(f'{path}: truncated WAD header')
        magic, count, directory_offset = struct.unpack('<4sii', header)
        if magic not in {b'IWAD', b'PWAD'} or count < 0 or directory_offset < 0:
            raise SystemExit(f'{path}: invalid WAD directory')
        wad.seek(directory_offset)
        names = set()
        for _ in range(count):
            entry = wad.read(16)
            if len(entry) != 16:
                raise SystemExit(f'{path}: truncated WAD directory')
            _, _, raw_name = struct.unpack('<ii8s', entry)
            names.add(raw_name.rstrip(b'\0').decode('ascii', errors='ignore').upper())
    missing = required - names
    if missing:
        raise SystemExit(f'{path}: missing liquid flats {sorted(missing)}')
    print(f'  {os.path.basename(path)} contains all classic liquid flats')
PY
		then
			exit 1
		fi
		specs=(
			"1 techbase"
			"2 industrial"
			"3 hell"
			"4 gothic"
			"5 corrupted"
			"6 techbase"
			"21 techbase"
		)
		for spec in "${specs[@]}"; do
			read -r seed theme <<<"$spec"
			echo "=== feature matrix seed=$seed theme=$theme size=8 ==="
			output=$(run_test "$seed" "$theme" 3 8)
			if ! echo "$output" | grep -q 'Dumped UDMF'; then
				echo "$output" | grep -E 'Generation failed|Dumped UDMF' || true
				exit 1
			fi
			if ! validate_dump 8 "$theme"; then
				echo "Feature structural validation failed for seed=$seed theme=$theme"
				exit 1
			fi
			cp /tmp/procmap_test.udmf "$feature_dir/${theme}_${seed}.udmf"
		done
		if ! python3 - "$feature_dir" <<'PY'
import collections
import glob
import math
import os
import re
import sys

root = sys.argv[1]

def blocks(text, kind):
    return [dict((key, value.strip('"')) for key, value in
                 re.findall(r'^\s*(\w+)\s*=\s*([^;]+);', body, re.M))
            for body in re.findall(r'(?m)^' + kind + r'\s*\n\{(.*?)\n\}', text, re.S)]

liquids = set()
basin_profiles = set()
door_widths = set()
door_depths = set()
cues = set()
perch_footprints = set()
perch_approaches = set()
mixed_liquid_maps = 0
room_scale_liquid_maps = 0
broad_grotto_maps = 0
long_watercourse_maps = 0
multi_family_maps = 0
for path in glob.glob(os.path.join(root, '*.udmf')):
    text = open(path, encoding='utf-8').read()
    vertices = blocks(text, 'vertex')
    sectors = blocks(text, 'sector')
    sides = blocks(text, 'sidedef')
    lines = blocks(text, 'linedef')
    file_liquids = {sector.get('texturefloor') for sector in sectors
                    if sector.get('texturefloor') in
                    {'FWATER1', 'BLOOD1', 'NUKAGE1', 'LAVA1'}}
    liquids.update(file_liquids)
    if (file_liquids & {'FWATER1', 'BLOOD1'} and
            file_liquids & {'NUKAGE1', 'LAVA1'}):
        mixed_liquid_maps += 1
    sector_points = collections.defaultdict(set)
    sector_edges = collections.defaultdict(list)
    sector_adjacency = collections.defaultdict(set)
    for line in lines:
        line_sectors = []
        for name in ('sidefront', 'sideback'):
            if name in line:
                sector_index = int(sides[int(line[name])]['sector'])
                sector_points[sector_index].update(
                    (int(line['v1']), int(line['v2'])))
                sector_edges[sector_index].append(
                    (int(line['v1']), int(line['v2'])))
                line_sectors.append(sector_index)
        if len(line_sectors) == 2 and line_sectors[0] != line_sectors[1]:
            sector_adjacency[line_sectors[0]].add(line_sectors[1])
            sector_adjacency[line_sectors[1]].add(line_sectors[0])
    liquid_groups = collections.defaultdict(list)
    for sector_index, sector in enumerate(sectors):
        flat = sector.get('texturefloor')
        if flat not in {'FWATER1', 'BLOOD1', 'NUKAGE1', 'LAVA1'}:
            continue
        dry_sector = next((neighbor for neighbor in sector_adjacency[sector_index]
                           if sectors[neighbor].get('texturefloor') not in
                           {'FWATER1', 'BLOOD1', 'NUKAGE1', 'LAVA1'}), -1)
        liquid_groups[(dry_sector, flat)].append(sector_index)
    paired_sectors = {sector_index
                      for group in liquid_groups.values() if len(group) == 2
                      for sector_index in group}
    file_room_scale = False
    file_broad_grotto = False
    file_long_watercourse = False
    for sector_index, sector in enumerate(sectors):
        if sector.get('texturefloor') not in file_liquids:
            continue
        points = sector_points[sector_index]
        if not points:
            continue
        xs = [float(vertices[point]['x']) for point in points]
        ys = [float(vertices[point]['y']) for point in points]
        footprint = tuple(sorted((max(xs) - min(xs), max(ys) - min(ys))))
        minor, major = footprint
        axis_edges = sum(
            vertices[first]['x'] == vertices[second]['x'] or
            vertices[first]['y'] == vertices[second]['y']
            for first, second in sector_edges[sector_index])
        point_count = len(points)
        profile = None
        flooded_room = len(sector_adjacency[sector_index]) > 1
        if flooded_room and minor >= 300.0:
            profile = 'flooded-room'
            file_broad_grotto = True
        elif sector_index in paired_sectors:
            profile = 'paired'
        elif point_count == 12 and minor >= 400.0:
            profile = 'grotto'
            file_broad_grotto = True
        elif point_count in (6, 8) and major >= 300.0:
            profile = 'bend-river'
            file_long_watercourse = True
        elif point_count == 10 and major >= 300.0:
            profile = 'straight-river'
            file_long_watercourse = True
        elif point_count == 12:
            profile = 'staggered-river'
        elif point_count == 8 and axis_edges < 4:
            profile = 'irregular'
        elif point_count == 8 and major / max(1.0, minor) >= 1.4:
            profile = 'trench'
        elif point_count == 8:
            profile = 'central'
        if profile:
            basin_profiles.add(profile)
        if major >= 500.0:
            file_room_scale = True
    room_scale_liquid_maps += file_room_scale
    broad_grotto_maps += file_broad_grotto
    long_watercourse_maps += file_long_watercourse
    sector_ids = {int(sector['id']): index for index, sector in enumerate(sectors)
                  if 'id' in sector}
    one_sided_counts = collections.Counter(
        int(sides[int(line['sidefront'])]['sector'])
        for line in lines if 'sideback' not in line)
    file_reveal_architectures = set()
    for target, target_sector in sector_ids.items():
        if 1000 <= target < 2000:
            faces = []
            for line in lines:
                if 'sideback' not in line:
                    continue
                front = int(sides[int(line['sidefront'])]['sector'])
                back = int(sides[int(line['sideback'])]['sector'])
                if target_sector not in (front, back) or front == back:
                    continue
                a, b = vertices[int(line['v1'])], vertices[int(line['v2'])]
                width = math.dist((float(a['x']), float(a['y'])),
                                  (float(b['x']), float(b['y'])))
                other = back if front == target_sector else front
                faces.append((line, other, width,
                              ((float(a['x']) + float(b['x'])) * 0.5,
                               (float(a['y']) + float(b['y'])) * 0.5)))
            if len(faces) != 2:
                continue
            width = round(faces[0][2])
            depth = round(math.dist(faces[0][3], faces[1][3]))
            door_widths.add(width)
            door_depths.add(depth)
            file_reveal_architectures.add(
                'false-wall' if depth == 16 else
                ('wall-alcove' if width == 64 else 'pavilion'))
            outer = max(faces, key=lambda face: one_sided_counts[face[1]])
            if outer[0].get('secret') == 'true':
                cues.add('hidden')
            else:
                texture = sides[int(outer[0]['sidefront'])].get('texturetop', '')
                cues.add('prominent' if texture.startswith('BIGDOOR') else 'subtle')
        elif 2000 <= target < 3000:
            points = set()
            for line in lines:
                line_sectors = {
                    int(sides[int(line[name])]['sector'])
                    for name in ('sidefront', 'sideback') if name in line
                }
                if target_sector in line_sectors:
                    points.add(int(line['v1']))
                    points.add(int(line['v2']))
            if points:
                xs = [float(vertices[point]['x']) for point in points]
                ys = [float(vertices[point]['y']) for point in points]
                footprint = tuple(sorted((round(max(xs) - min(xs)),
                                          round(max(ys) - min(ys)))))
                perch_footprints.add(footprint)
                approach_by_footprint = {
                    (112, 112): 'straight',
                    (120, 120): 'offset',
                    (96, 144): 'dogleg',
                }
                if footprint in approach_by_footprint:
                    perch_approaches.add(approach_by_footprint[footprint])
    if len(file_reveal_architectures) >= 2:
        multi_family_maps += 1

errors = []
if liquids != {'FWATER1', 'BLOOD1', 'NUKAGE1', 'LAVA1'}:
    errors.append(f'fluid matrix covered only {sorted(liquids)}')
expected_fluids = {'central', 'trench', 'paired', 'irregular', 'flooded-room',
                   'straight-river', 'staggered-river', 'bend-river'}
if basin_profiles != expected_fluids:
    errors.append(f'fluid matrix covered only architecture profiles={sorted(basin_profiles)}')
if mixed_liquid_maps == 0:
    errors.append('fluid matrix contains no map mixing harmless and hazardous liquid')
if room_scale_liquid_maps < 4:
    errors.append(f'only {room_scale_liquid_maps} feature maps contain room-scale liquid')
if broad_grotto_maps == 0 or long_watercourse_maps == 0:
    errors.append(f'natural liquid matrix lacks broad areas or long watercourses: '
                  f'grottos={broad_grotto_maps} watercourses={long_watercourse_maps}')
if not {64, 80, 96}.issubset(door_widths) or 16 not in door_depths:
    errors.append(f'reveal matrix lacks all architectures: widths={sorted(door_widths)} '
                  f'depths={sorted(door_depths)}')
if cues != {'hidden', 'subtle', 'prominent'}:
    errors.append(f'reveal matrix covered only cues={sorted(cues)}')
if multi_family_maps == 0:
    errors.append('reveal matrix contains no multi-family opportunity map')
expected_perches = {(112, 112), (120, 120), (96, 144)}
if not expected_perches.issubset(perch_footprints):
    errors.append(f'perch matrix covered only {sorted(perch_footprints)}')
if perch_approaches != {'straight', 'offset', 'dogleg'}:
    errors.append(f'perch matrix covered only approaches={sorted(perch_approaches)}')
for error in errors:
    print(f'    {error}')
if errors:
    raise SystemExit(1)
print(f'  liquids={sorted(liquids)} basins={sorted(basin_profiles)} '
      f'mixed-maps={mixed_liquid_maps} room-scale-maps={room_scale_liquid_maps} '
      f'grottos={broad_grotto_maps} watercourses={long_watercourse_maps}')
print(f'  reveal-widths={sorted(door_widths)} cues={sorted(cues)} '
      f'multi-family-maps={multi_family_maps}')
print(f'  perches={sorted(perch_footprints)} '
      f'approaches={sorted(perch_approaches)}')
PY
		then
			exit 1
		fi
		feature_runtime_log=/tmp/procmap_feature_runtime.log
		if ! run_runtime_load 1 techbase 3 8 "$IWAD" "$feature_runtime_log" 3; then
			echo "Feature-family runtime/node validation failed"
			grep -Ei 'error|failed|invalid|unknown|node|texture|unclosed|dummy subsector' \
				"$feature_runtime_log" | tail -30 || true
			exit 1
		fi
		echo "Fluid, opportunity, cue, and elevated-architecture matrix passed"
		;;
	doors)
		door_dir=/tmp/procmap_door_matrix
		rm -rf "$door_dir"
		mkdir -p "$door_dir"
			specs=(
				"1 techbase"
				"19 techbase"
				"3 gothic"
				"5 hell"
				"777777 techbase"
			"777777 hell"
			"777777 industrial"
			"777777 gothic"
			"777777 corrupted"
		)
		for spec in "${specs[@]}"; do
			read -r seed theme <<<"$spec"
			echo "=== door profile seed=$seed theme=$theme size=8 ==="
			output=$(run_test "$seed" "$theme" 3 8)
			if ! echo "$output" | grep -q 'Dumped UDMF'; then
				echo "$output" | grep -E 'Generation failed|Dumped UDMF' || true
				exit 1
			fi
			if ! validate_dump 8 "$theme"; then
				echo "Door geometry validation failed for seed=$seed theme=$theme"
				exit 1
			fi
			cp /tmp/procmap_test.udmf "$door_dir/${theme}_${seed}.udmf"
		done
		if ! python3 - "$door_dir" <<'PY'
import glob
import math
import os
import re
import sys

root = sys.argv[1]

def blocks(text, kind):
    return [dict((key, value.strip('"')) for key, value in
                 re.findall(r'^\s*(\w+)\s*=\s*([^;]+);', body, re.M))
            for body in re.findall(r'(?m)^' + kind + r'\s*\n\{(.*?)\n\}', text, re.S)]

textures = set()
heights = set()
widths = set()
profile_count = 0
for path in glob.glob(os.path.join(root, '*.udmf')):
    text = open(path, encoding='utf-8').read()
    vertices = blocks(text, 'vertex')
    sectors = blocks(text, 'sector')
    sides = blocks(text, 'sidedef')
    lines = blocks(text, 'linedef')
    for line in lines:
        if (line.get('special') != '12' or line.get('secret') == 'true' or
                int(line.get('locknumber', '0')) != 0):
            continue
        front = sides[int(line['sidefront'])]
        back = sides[int(line['sideback'])]
        texture = front.get('texturetop')
        first = vertices[int(line['v1'])]
        second = vertices[int(line['v2'])]
        width = round(math.dist((float(first['x']), float(first['y'])),
                                (float(second['x']), float(second['y']))))
        height = round(float(sectors[int(front['sector'])]['heightceiling']) -
                       float(sectors[int(back['sector'])]['heightfloor']))
        textures.add(texture)
        widths.add(width)
        heights.add(height)
        profile_count += 1

errors = []
if not widths or min(widths) < 128 or not any(width > 128 for width in widths):
    errors.append(f'door matrix lacks Standard-plus physical apertures: {sorted(widths)}')
if not {72, 96, 112, 128}.issubset(heights):
    errors.append(f'door matrix lacks stock height range: {sorted(heights)}')
if len(textures) < 8:
    errors.append(f'door matrix uses only {len(textures)} ordinary textures')
if not any(texture and texture.startswith('SPCDOOR') for texture in textures):
    errors.append('Doom II Techbase matrix contains no SPCDOOR profile')
if not {'DOOR1', 'DOOR3'} & textures:
    errors.append('door matrix contains no compact classic DOOR profile')
if 'BIGDOOR6' not in textures:
    errors.append('infernal/gothic matrix contains no 112-unit BIGDOOR6 profile')

for error in errors:
    print(f'    {error}')
if errors:
    raise SystemExit(1)
print(f'  ordinary-faces={profile_count} textures={len(textures)} '
      f'widths={sorted(widths)} heights={sorted(heights)}')
PY
		then
			exit 1
		fi
		for spec in '1 techbase' '777777 gothic'; do
			read -r seed theme <<<"$spec"
			log="/tmp/procmap_door_runtime_${theme}.log"
			if ! run_runtime_load "$seed" "$theme" 3 8 "$IWAD" "$log" 3; then
				echo "Door runtime/node validation failed for theme=$theme"
				grep -Ei 'error|failed|invalid|unknown|node|texture|unclosed|dummy subsector' \
					"$log" | tail -20 || true
				exit 1
			fi
		done
		echo "Standard-plus physical door slabs with native fitted art passed"
		;;
	rewards)
		seed=20260713
		theme=hell
		difficulty=5
		size=20
		echo "=== Doom II secret reward progression seed=$seed theme=$theme difficulty=$difficulty size=$size ==="
		output=$(run_test "$seed" "$theme" "$difficulty" "$size")
		if ! echo "$output" | grep -q 'Dumped UDMF'; then
			echo "$output" | grep -E 'Generation failed|Dumped UDMF' || true
			exit 1
		fi
		report_dump
		if ! validate_dump "$size" "$theme"; then
			echo "Secret reward structural validation failed"
			exit 1
		fi
		declare -A reward_names=(
			[8]=backpack
			[83]=megasphere
			[2013]=soulsphere
			[2022]=invulnerability
			[2023]=berserk
			[2024]=partial-invisibility
			[2026]=computer-map
			[2045]=light-amplification
		)
		for type in 8 83 2013 2022 2023 2024 2026 2045; do
			count=$(grep -c "^[[:space:]]*type = $type;" /tmp/procmap_test.udmf || true)
			if [ "$count" -lt 1 ]; then
				echo "Missing Doom powerup: ${reward_names[$type]} (type $type)"
				exit 1
			fi
			echo "  ${reward_names[$type]}=$count"
		done
		if grep -q '^[[:space:]]*special = 9;' /tmp/procmap_test.udmf ||
				! grep -q '^[[:space:]]*special = 1024;' /tmp/procmap_test.udmf; then
			echo "Generated reward rooms do not use the engine SECRET_MASK"
			exit 1
		fi
		log=/tmp/procmap_rewards_runtime.log
		if ! run_runtime_load "$seed" "$theme" "$difficulty" "$size" "$IWAD" "$log" 3; then
			echo "Secret reward runtime/node validation failed"
			grep -Ei 'error|failed|invalid|unknown|node|texture|unclosed|dummy subsector' \
				"$log" | tail -20 || true
			exit 1
		fi
		echo "Doom II powerup progression and engine-counted secrets passed"
		;;
	maxsettings)
		seed=314159
		echo "=== maximum all-high settings seed=$seed theme=techbase difficulty=3 size=160 ==="
		output=$(run_test "$seed" techbase 3 160 2 2 2 2)
		echo "$output" | grep -E 'Dumped UDMF|Generation failed' || true
		if ! echo "$output" | grep -q 'Dumped UDMF'; then
			exit 1
		fi
		report_dump
		if ! validate_dump 160 techbase 2 2 2; then
			echo "Maximum all-high settings failed serialized structural validation"
			exit 1
		fi
		log=/tmp/procmap_maximum_settings.log
		if ! run_runtime_load "$seed" techbase 3 160 "$IWAD" "$log" 3 2 2 2 2; then
			echo "Maximum all-high settings failed runtime/node validation"
			grep -Ei 'error|failed|invalid|unknown|node|texture|unclosed|dummy subsector' \
				"$log" | tail -30 || true
			exit 1
		fi
		echo "Maximum all-high settings passed structural and runtime/node validation"
		;;
	network)
		# This state-machine contract deliberately stays source-level: a short
		# headless process cannot create two synchronized peers, but these checks
		# cover the ordering that prevents an ACKed client from clearing its staged
		# archive while the host's already-queued map-change event is in flight.
		procgen_source="$ROOT/src/common/maps/procgen.cpp"
		required_network_transfer_patterns=(
			'bool TransitionCommitted = false;'
			'NetworkProceduralReceiver.TransitionCommitted = true;'
			'if (NetworkProceduralReceiver.TransitionCommitted)'
			'(!NetworkProceduralReceiver.Staged ||'
			'!NetworkProceduralReceiver.TransitionCommitted))'
			'(!NetworkProceduralReceiver.TransitionCommitted &&'
			'DiscardAbandonedProceduralArchive();'
		)
		for pattern in "${required_network_transfer_patterns[@]}"; do
			if ! grep -Fq "$pattern" "$procgen_source"; then
				echo "Procedural co-op transfer safety contract is missing: $pattern"
				exit 1
			fi
		done
		if [ "$(grep -Fc 'DiscardAbandonedProceduralArchive();' "$procgen_source")" -lt 4 ]; then
			echo "Abandoned procedural archives are not released on every transfer reset path"
			exit 1
		fi
		echo "Procedural co-op ACK-commit and abandoned-archive contracts passed"
		;;
	menu)
        menu_dump=/tmp/procmap_menu_definition.txt
        menu_config=/tmp/procmap_menu_test.ini
        menu_log=/tmp/procmap_menu_test.log
        launch_log=/tmp/procmap_menu_launch.log
        menu_fixture="$ROOT/tools/fixtures/procgen-menu.ini"
        rm -f "$menu_dump" "$menu_config" "$menu_log" "$launch_log"

        # A first-run configuration opens the IWAD launcher before deferred
        # console commands can execute. Keep this smoke self-contained and
        # noninteractive without inheriting a developer's personal settings.
        if ! cp "$menu_fixture" "$menu_config"; then
            echo "Could not seed the procedural menu test configuration"
            exit 1
        fi
        menu_engine_args=(
            -stdout -headless -nosound -nomusic -nogui -noautoload
            -config "$menu_config" -iwad "$IWAD"
        )
        run_menu_command() {
            local output_log=$1
            local log_pattern=$2
            local config_pattern=$3
            shift 3
            local pid completed=0

            rm -f "$output_log"
            SDL_AUDIODRIVER=dummy ALSOFT_DRIVERS=null setsid stdbuf -oL -eL "$BIN" \
                "${menu_engine_args[@]}" \
                "$@" >"$output_log" 2>&1 &
            pid=$!
            for ((tick = 0; tick < 300; ++tick)); do
                if grep -Eq "$log_pattern" "$output_log" 2>/dev/null &&
                        { [ -z "$config_pattern" ] ||
                          grep -Eq "$config_pattern" "$menu_config" 2>/dev/null; }; then
                    completed=1
                    break
                fi
                if ! kill -0 "$pid" 2>/dev/null; then
                    break
                fi
                sleep 0.1
            done
            if kill -0 "$pid" 2>/dev/null; then
                kill -KILL -- "-$pid" 2>/dev/null || true
            fi
            wait "$pid" 2>/dev/null || true
            [ "$completed" -eq 1 ]
        }

        if ! unzip -p "$ROOT/build/biaseddoom.pk3" menudef.txt >"$menu_dump"; then
            echo "Could not read packed MENUDEF"
            exit 1
        fi
        required_menu_patterns=(
            'TextItem "$PGMNU_TITLE", "p", "ProceduralMapMenu"'
            'OptionMenu "ProceduralMapMenu"'
            'TextField "Seed", "procgen_seed"'
            '"procmap_randomize_seed"'
            '"procgen_theme", "ProcGenThemes"'
			'"industrial", "Industrial"'
			'"gothic", "Gothic"'
			'"corrupted", "Corrupted Tech"'
			'"procgen_difficulty", "ProcGenDifficulties"'
			'Slider "Map Size", "procgen_size", 1, 160, 1, 0'
			'"procgen_layout", "ProcGenLayouts"'
			'"procgen_verticality", "ProcGenVerticality"'
			'"procgen_detail", "ProcGenDetail"'
			'"procgen_outdoors", "ProcGenOutdoors"'
            '"procmap", 1, 1'
            '"procmap random", 1, 1'
			'"procmap_next", 1, 1'
            '"procmap_restore_defaults"'
        )
        for pattern in "${required_menu_patterns[@]}"; do
            if ! grep -Fq "$pattern" "$menu_dump"; then
                echo "Packed procedural menu is missing: $pattern"
                exit 1
            fi
        done

        # Run identity is recipe-derived. A profile selector would create an
        # unsupported second source of generation state, so reject both a
        # hidden CVar and a stray menu entry rather than merely omitting it
        # from this expected-menu list.
        if rg -n '\bprocgen_profile\b' "$ROOT/src" "$ROOT/wadsrc" >/dev/null; then
            echo "Procedural generation exposes an unsupported profile selector"
            exit 1
        fi

        procgen_api_source="$ROOT/wadsrc/static/zscript/procgen/procgen.zs"
        procgen_binding_source="$ROOT/src/playsim/procgen_zscript.cpp"
        level_source="$ROOT/src/g_level.cpp"
        level_header="$ROOT/src/g_levellocals.h"
        required_profile_api_patterns=(
            'native static String GetRunProfile();'
            'native static String GetRunBriefing();'
        )
        for pattern in "${required_profile_api_patterns[@]}"; do
            if ! grep -Fq "$pattern" "$procgen_api_source"; then
                echo "Procedural ZScript API is missing: $pattern"
                exit 1
            fi
        done
        required_profile_binding_patterns=(
            'DEFINE_ACTION_FUNCTION_NATIVE(ProceduralMapGenerator, GetRunProfile, ZSF_GetRunProfile)'
            'DEFINE_ACTION_FUNCTION_NATIVE(ProceduralMapGenerator, GetRunBriefing, ZSF_GetRunBriefing)'
        )
        for pattern in "${required_profile_binding_patterns[@]}"; do
            if ! grep -Fq "$pattern" "$procgen_binding_source"; then
                echo "Procedural ZScript binding is missing: $pattern"
                exit 1
            fi
        done
        # This is intentionally a source-level regression gate: savegame and
        # hub restoration cannot be reproduced safely in a short headless menu
        # smoke. IsReentering covers savegames and valid hub snapshots, and
        # FromSnapshot covers the map-restoration path itself.
        if ! grep -Fq 'P_IsProceduralMapName(MapName.GetChars()) && !IsReentering() && !FromSnapshot' \
                "$level_source" ||
                ! grep -Fq 'GetRunProfile()' "$level_source" ||
                ! grep -Fq 'GetRunBriefing()' "$level_source" ||
                ! grep -Fq 'message.Format("%s\n%s", profile.GetChars(), briefing.GetChars());' "$level_source" ||
                ! grep -Fq 'C_MidPrint(nullptr, message.GetChars(), true);' "$level_source" ||
                ! grep -Fq 'return savegamerestore' "$level_header" ||
                ! grep -Fq 'info->Snapshot.mBuffer != nullptr' "$level_header"; then
            echo "Procedural briefing is not gated to a fresh PROCMAP load"
            exit 1
        fi

		# The endless-run action is intentionally available only after the map's
		# real exit. It copies the completed archive recipe (without copying its
		# large UDMF), forces a distinct random seed, and starts a fresh game;
		# save/hub restoration must never be repurposed as a replay request.
		procgen_source="$ROOT/src/common/maps/procgen.cpp"
		procgen_header="$ROOT/src/common/maps/procgen.h"
		required_endless_run_patterns=(
			'void P_MarkCurrentProceduralMapCompleted()'
			'bool P_PrepareNextProceduralMap()'
			'CopyProceduralRecipe(CompletedProceduralMap, CurrentProceduralMap);'
			'MakeDistinctProceduralMenuSeed(CompletedProceduralMap.Seed)'
			'ApplyProceduralRecipeToCVars(CompletedProceduralMap,'
			'CCMD(procmap_next)'
			'StartProceduralMapFromCurrentCVars();'
			'Finish a procedural run before starting its next seed.'
		)
		for pattern in "${required_endless_run_patterns[@]}"; do
			if ! grep -Fq "$pattern" "$procgen_source"; then
				echo "Procedural endless-run flow is missing: $pattern"
				exit 1
			fi
		done
		if ! grep -Fq 'void P_MarkCurrentProceduralMapCompleted();' "$procgen_header" ||
				! grep -Fq 'bool P_PrepareNextProceduralMap();' "$procgen_header" ||
				[ "$(grep -Fc 'P_MarkCurrentProceduralMapCompleted();' "$level_source")" -lt 2 ]; then
			echo "Procedural exit does not arm the post-completion replay action"
			exit 1
		fi

        # MENUDEF lumps from gameplay mods may replace MainMenu after the
        # engine definition. Verify that the native post-parse reconciliation
        # remains present for both graphic and localized/text-only layouts.
        menu_source="$ROOT/src/common/menu/menudef.cpp"
        required_compat_patterns=(
            'static void EnsureProceduralMenuEntries()'
            'EnsureProceduralMenuEntry(NAME_MainMenu);'
            'EnsureProceduralMenuEntry(NAME_MainMenuTextOnly);'
            'item->mAction == action'
        )
        for pattern in "${required_compat_patterns[@]}"; do
            if ! grep -Fq "$pattern" "$menu_source"; then
                echo "Procedural menu override compatibility is missing: $pattern"
                exit 1
            fi
        done

        listmenu_source="$ROOT/wadsrc/static/zscript/engine/ui/menu/listmenu.zs"
        required_scroll_patterns=(
            'double mScrollOffset;'
            'UIEvent.Type_WheelDown'
            'case MKEY_PageDown:'
            'case MKEY_Home:'
            'case MKEY_End:'
            'EnsureSelectionVisible();'
        )
        for pattern in "${required_scroll_patterns[@]}"; do
            if ! grep -Fq "$pattern" "$listmenu_source"; then
                echo "Scrollable replacement-menu support is missing: $pattern"
                exit 1
            fi
        done

		if ! run_menu_command "$menu_log" 'Procedural map settings restored to defaults' '^procgen_seed=0$' \
			+procgen_seed 123456 +procgen_theme hell +procgen_difficulty 5 +procgen_size 5 \
			+procgen_layout 2 +procgen_verticality 2 +procgen_detail 2 +procgen_outdoors 2 \
			+procmap_restore_defaults +quit; then
            echo "Procedural menu default restore command did not complete"
            exit 1
        fi
        if ! grep -q 'Procedural map settings restored to defaults' "$menu_log" ||
                ! grep -q '^procgen_seed=0$' "$menu_config" ||
                ! grep -q '^procgen_theme=techbase$' "$menu_config" ||
				! grep -q '^procgen_difficulty=3$' "$menu_config" ||
				! grep -q '^procgen_size=3$' "$menu_config" ||
				! grep -q '^procgen_layout=1$' "$menu_config" ||
				! grep -q '^procgen_verticality=1$' "$menu_config" ||
				! grep -q '^procgen_detail=1$' "$menu_config" ||
				! grep -q '^procgen_outdoors=1$' "$menu_config"; then
            echo "Procedural menu defaults did not persist correctly"
            exit 1
        fi

		if ! run_menu_command "$menu_log" 'Procedural map seed set to' '^procgen_seed=[1-9][0-9]*$' \
                +procmap_randomize_seed +quit; then
            echo "Procedural menu seed randomization command did not complete"
            exit 1
        fi
        random_seed=$(sed -n 's/^procgen_seed=//p' "$menu_config")
        if ! grep -q 'Procedural map seed set to' "$menu_log" ||
                ! [[ "$random_seed" =~ ^[1-9][0-9]*$ ]]; then
            echo "Procedural menu seed randomization failed"
            exit 1
        fi

		# A menu command must not fabricate an endless chain from a title-screen
		# configuration. It becomes available only after ExitLevel/SecretExitLevel
		# records the completed procedural recipe.
		if ! run_menu_command "$menu_log" 'Finish a procedural run before starting its next seed' '' \
				+procmap_next +quit; then
			echo "Procedural endless-run action was not safely gated before completion"
			exit 1
		fi
		if ! grep -q "^procgen_seed=${random_seed}$" "$menu_config"; then
			echo "Procedural endless-run action changed the recipe before completion"
			exit 1
		fi

        status=0
        # Stop after the map reaches its title rather than waiting on the
        # interactive title loop. The log marker is emitted only after the
        # generated archive has become the live level.
        if ! run_menu_command "$launch_log" '^PROCMAP - ' '' +procgen_theme hell \
                +procgen_difficulty 4 +procgen_size 1 +procmap random; then
            status=1
        fi
        if ! grep -q '^PROCMAP - ' "$launch_log"; then
            echo "Procedural menu launch command did not enter PROCMAP (exit=$status)"
            exit 1
        fi
        if ! grep -Eq 'Run profile: (Expedition|Assault|Infiltration|Circuit|Siege)' "$launch_log" ||
                ! grep -Eq 'Run briefing: .+' "$launch_log"; then
            echo "procmap did not print its recipe-derived profile and briefing"
            exit 1
        fi
        if grep -Eqi 'procedural map generation failed|invalid map|nodebuilder.*failed|missing texture|unknown texture' "$launch_log"; then
            echo "Procedural menu launch reported a runtime error"
            exit 1
        fi
        echo "Procedural main-menu integration passed"
        ;;
    balance)
        previous=0
        previous_area=0
        for difficulty in 1 2 3 4 5; do
            output=$(run_test 2024 techbase "$difficulty" 3)
            if ! echo "$output" | grep -q "Dumped UDMF"; then
                echo "Balance generation failed at difficulty=$difficulty"
                exit 1
            fi
            monsters=$(grep -Ec '^\s*type = (7|9|16|58|64|65|66|67|68|69|71|72|84|88|89|3001|3002|3003|3004|3005|3006);' \
                /tmp/procmap_test.udmf 2>/dev/null || true)
            ammo=$(grep -Ec '^\s*type = (17|2007|2008|2010|2046|2047|2048|2049);' /tmp/procmap_test.udmf 2>/dev/null || true)
            health=$(grep -Ec '^\s*type = (2011|2012|2013|2014|2015|2018|2019);' /tmp/procmap_test.udmf 2>/dev/null || true)
            exit_area=$(measure_exit_room_area)
            echo "difficulty=$difficulty monsters=$monsters ammo=$ammo health+armor=$health exit-area=$exit_area"
            if [ "$monsters" -lt "$previous" ]; then
                echo "Encounter pressure regressed from $previous to $monsters"
                exit 1
            fi
            if ! validate_dump 3 techbase; then
                echo "Balance validation failed at difficulty=$difficulty"
                exit 1
            fi
            if [ "$previous_area" -gt 0 ] && [ "$exit_area" -le "$previous_area" ]; then
                echo "Finale arena did not grow from $previous_area at difficulty=$difficulty (area=$exit_area)"
                exit 1
            fi
            previous=$monsters
            previous_area=$exit_area
        done
        if [ "$previous_area" -lt 1500000 ]; then
            echo "Nightmare finale arena is too small: $previous_area"
            exit 1
        fi
        echo "Difficulty balance progression passed"
        ;;
    alignment)
        ensure_doom1_iwad || exit 2
        alignment_dir=$(mktemp -d /tmp/procmap_alignment.XXXXXX) || exit 1
        trap 'rm -rf "$alignment_dir"; rm -f "$TEST_CONFIG"' EXIT
        alignment_cases="$alignment_dir/cases.tsv"
        # These two focused recipes exercise large dramatic stair/portal runs
        # and the compact Gothic dogleg regression. Run them against both IWAD
        # families so the cache cannot accidentally keep Doom II dimensions.
        specs=(
            "0 techbase 4 6 2 2 1 1"
            "972996588 gothic 3 3 2 2 1 2"
        )
        for family in doom1 doom2; do
            if [ "$family" = doom1 ]; then
                IWAD="$DOOM1_IWAD"
            else
                IWAD="$DOOM2_IWAD"
            fi
            index=0
            for spec in "${specs[@]}"; do
                read -r seed theme difficulty size layout verticality detail outdoors <<<"$spec"
                udmf="$alignment_dir/${family}_${index}.udmf"
                manifest="$alignment_dir/${family}_${index}.json"
                echo "=== alignment $family seed=$seed theme=$theme size=$size ==="
                output=$(run_test "$seed" "$theme" "$difficulty" "$size" "$layout" \
                    "$verticality" "$detail" "$outdoors" "$udmf")
                if ! echo "$output" | grep -q 'Dumped UDMF' || [ ! -s "$udmf" ]; then
                    echo "$output" | grep -E 'Generation failed|Dumped UDMF' || true
                    echo "Native-metric alignment UDMF generation failed for $family/$seed"
                    exit 1
                fi
                output=$(run_manifest "$seed" "$theme" "$difficulty" "$size" "$layout" \
                    "$verticality" "$detail" "$outdoors" "$manifest")
                if [ ! -s "$manifest" ] ||
                        ! validate_manifest "$manifest" "$size" "$difficulty" "$verticality" ||
						! validate_manifest_udmf_collision_navigation "$manifest" "$udmf" ||
						! validate_manifest_udmf_alignment "$manifest" "$udmf" 1; then
                    echo "$output" | grep -E 'Generation failed|Dumped.*manifest' || true
                    echo "Native-metric alignment manifest validation failed for $family/$seed"
                    exit 1
                fi
                printf '%s\t%s\n' "$family" "$manifest" >>"$alignment_cases"
                index=$((index + 1))
            done
        done
        if ! python3 - "$alignment_cases" <<'PY'
import json
import sys

cases = {}
with open(sys.argv[1], encoding='utf-8') as handle:
    for line in handle:
        family, path = line.rstrip('\n').split('\t')
        with open(path, encoding='utf-8') as manifest_file:
            cases.setdefault(family, []).append(json.load(manifest_file))

errors = []
metrics_by_family = {}
for family in ('doom1', 'doom2'):
    manifests = cases.get(family, [])
    if not manifests:
        errors.append(f'{family} has no alignment manifests')
        continue
    kinds = set()
    dimensions = set()
    metrics_by_texture = {}
    non_128 = False
    for manifest in manifests:
        if manifest.get('iwad_roster') != family:
            errors.append(f'{family} manifest reports iwad_roster={manifest.get("iwad_roster")!r}')
        alignment = manifest.get('visual_proof', {}).get('alignment', {})
        metrics = alignment.get('metrics', [])
        witnesses = alignment.get('witnesses', [])
        kinds.update(witness.get('kind') for witness in witnesses if isinstance(witness, dict))
        for metric in metrics:
            if not isinstance(metric, dict):
                continue
            width, height = metric.get('width'), metric.get('height')
            texture = metric.get('texture')
            if isinstance(texture, str) and isinstance(width, int) and isinstance(height, int):
                dimensions.add((width, height))
                non_128 |= (width, height) != (128, 128)
                metrics_by_texture.setdefault(texture, set()).add((width, height))
    missing = {'world', 'two_sided', 'stair', 'portal'} - kinds
    if missing:
        errors.append(f'{family} misses alignment witness kinds: {", ".join(sorted(missing))}')
    if len(dimensions) < 2 or not non_128:
        errors.append(f'{family} did not expose varied native texture metrics: {sorted(dimensions)}')
    metrics_by_family[family] = metrics_by_texture

# The fixtures deliberately include common step/door material in both IWAD
# families. Require an actual native-size difference for at least one shared
# name, so a future hard-coded 128-unit cache cannot pass merely because the
# two themes happen to select different texture names.
doom1_metrics = metrics_by_family.get('doom1', {})
doom2_metrics = metrics_by_family.get('doom2', {})
shared_metric_differences = [texture for texture in sorted(set(doom1_metrics) & set(doom2_metrics))
                             if doom1_metrics[texture] != doom2_metrics[texture]]
if not shared_metric_differences:
    errors.append('Doom I/Doom II fixtures did not expose a shared texture with different native metrics')

for error in errors:
    print(f'  {error}')
if errors:
    raise SystemExit(1)
print('  Doom I/Doom II active-IWAD metric and phase coverage passed')
PY
        then
            exit 1
        fi
        echo "Native-metric texture alignment validation passed"
        ;;
    doom1)
        ensure_doom1_iwad || exit 2
        IWAD="$DOOM1_IWAD"
        specs=(
            "2718 techbase 4 4"
            "31337 hell 5 5"
            "414 industrial 3 3"
            "515 gothic 4 4"
            "616 corrupted 5 5"
        )
        for spec in "${specs[@]}"; do
            read -r seed theme difficulty size <<<"$spec"
            output=$(run_test "$seed" "$theme" "$difficulty" "$size")
            if ! echo "$output" | grep -q "Dumped UDMF"; then
                echo "Ultimate Doom compatibility generation failed"
                exit 1
            fi
            echo "$output" | grep "Dumped UDMF"
            report_dump
            if ! validate_dump "$size" "$theme"; then
                echo "Ultimate Doom structural validation failed"
                exit 1
            fi
            if grep -Eq '^\s*type = (64|65|66|67|68|69|71|72|82|83|84|88|89);' /tmp/procmap_test.udmf; then
                echo "Ultimate Doom map contains a Doom II-only actor"
                exit 1
            fi
            if grep -Eq '^\s*type = (85|86);' /tmp/procmap_test.udmf; then
                echo "Ultimate Doom map contains Doom II-only tech lamps"
                exit 1
            fi
			if grep -Eq '^\s*texture(top|middle|bottom) = "SPCDOOR' /tmp/procmap_test.udmf; then
				echo "Ultimate Doom map contains a Doom II-only SPCDOOR texture"
				exit 1
			fi
        done
        for spec in "${specs[@]}"; do
            read -r seed theme difficulty size <<<"$spec"
            doom1_runtime_log="/tmp/procmap_doom1_runtime_${theme}.log"
            if ! run_runtime_load "$seed" "$theme" "$difficulty" "$size" \
                    "$DOOM1_IWAD" "$doom1_runtime_log"; then
                echo "Ultimate Doom runtime load failed for theme=$theme"
                grep -Ei 'error|failed|invalid|unknown|node|texture' "$doom1_runtime_log" | tail -20 || true
                exit 1
            fi
        done
        echo "Ultimate Doom roster and all-theme runtime compatibility passed"
        ;;
    load)
        specs=(
            "7 techbase 2 1"
            "42 hell 3 3"
            "999 industrial 5 5"
            "20260713 gothic 5 20"
			"8080 corrupted 3 160"
        )
        for spec in "${specs[@]}"; do
            read -r seed theme difficulty size <<<"$spec"
            log="/tmp/procmap_runtime_load_${seed}.log"
            if ! run_runtime_load "$seed" "$theme" "$difficulty" "$size" "$IWAD" "$log"; then
                echo "Runtime load failed for seed=$seed theme=$theme size=$size"
                grep -Ei 'error|failed|invalid|unknown|node' "$log" | tail -20 || true
                exit 1
            fi
            echo "Runtime load passed: seed=$seed theme=$theme size=$size"
        done
        ;;
    extreme)
        seed=1771465796
        themes=(techbase hell industrial gothic corrupted)
        failures=0
        fingerprints=()
        for theme in "${themes[@]}"; do
            echo "=== extreme seed=$seed theme=$theme difficulty=3 size=80 ==="
            output=$(run_test "$seed" "$theme" 3 80)
            echo "$output" | grep -E "Dumped UDMF|Generation failed" || true
            if ! echo "$output" | grep -q "Dumped UDMF"; then
                failures=$((failures + 1))
                continue
            fi
            report_dump
            if ! validate_dump 80 "$theme"; then
                failures=$((failures + 1))
                continue
            fi
            fingerprints+=("$(sha256sum /tmp/procmap_test.udmf | cut -d' ' -f1)")
            log="/tmp/procmap_extreme_${theme}.log"
            if ! run_runtime_load "$seed" "$theme" 3 80 "$IWAD" "$log" 3; then
                echo "Extreme runtime load failed for theme=$theme"
                grep -Ei 'error|failed|invalid|unknown|node|texture' "$log" | tail -20 || true
                failures=$((failures + 1))
                continue
            fi
            echo "Extreme runtime load passed: theme=$theme"
        done
        unique_fingerprints=$(printf '%s\n' "${fingerprints[@]}" | sort -u | sed '/^$/d' | wc -l)
        if [ "$unique_fingerprints" -ne "${#themes[@]}" ]; then
            echo "Extreme themes did not produce five distinct authored outputs"
            failures=$((failures + 1))
        fi
        if [ "$failures" -ne 0 ]; then
            echo "Extreme all-theme regression failed for $failures check(s)"
            exit 1
        fi
        echo "Extreme seed $seed passed structural and runtime validation for all themes"
        ;;
    huge)
        # Keep the broad 80-cell corpus for turnaround, but the final case is
        # the actual supported ceiling.  It exercises both UDMF structural
        # validation and a live node-builder/runtime load at size 160 instead
        # of merely documenting that limit.
        specs=(
            "1 techbase 2 80"
            "42 hell 4 80"
            "8080 industrial 3 80"
            "2147483647 gothic 5 80"
            "501721273 corrupted 3 160"
        )
        failures=0
        for spec in "${specs[@]}"; do
            read -r seed theme difficulty size <<<"$spec"
            echo "=== huge seed=$seed theme=$theme difficulty=$difficulty size=$size ==="
            output=$(run_test "$seed" "$theme" "$difficulty" "$size")
            echo "$output" | grep -E "Dumped UDMF|Generation failed" || true
            if ! echo "$output" | grep -q "Dumped UDMF"; then
                failures=$((failures + 1))
                continue
            fi
            report_dump
            if ! validate_dump "$size" "$theme"; then
                failures=$((failures + 1))
                continue
            fi
            log="/tmp/procmap_huge_${seed}_${theme}.log"
            if ! run_runtime_load "$seed" "$theme" "$difficulty" "$size" \
                    "$IWAD" "$log" 3; then
                echo "Huge runtime/node validation failed for seed=$seed theme=$theme"
                grep -Ei 'error|failed|invalid|unknown|node|unclosed|dummy subsector' \
                    "$log" | tail -30 || true
                failures=$((failures + 1))
                continue
            fi
            echo "Huge structural/render-node validation passed: seed=$seed theme=$theme"
        done
        if [ "$failures" -ne 0 ]; then
            echo "Huge multi-seed regression failed for $failures check(s)"
            exit 1
        fi
        echo "Huge multi-seed regression passed for ${#specs[@]} independent maps"
        ;;
    validate)
        failures=0
        specs=(
            "1 techbase 2 1"
            "42 hell 3 2"
            "99 industrial 3 3"
            "123 gothic 4 4"
            "999 corrupted 5 5"
            "20260713 hell 5 20"
            "8080 industrial 3 80"
        )
        for spec in "${specs[@]}"; do
            read -r seed theme difficulty size <<<"$spec"
            echo "=== seed=$seed theme=$theme difficulty=$difficulty size=$size ==="
            output=$(run_test "$seed" "$theme" "$difficulty" "$size")
            echo "$output" | grep -E "Dumped UDMF|Generation failed" || true
            if ! echo "$output" | grep -q "Dumped UDMF"; then
                failures=$((failures + 1))
                continue
            fi
            report_dump
            if ! validate_dump "$size" "$theme"; then
                failures=$((failures + 1))
            fi
        done
		# This compact recipe used to reject the entire map because every safe
		# multi-cell host was consumed by progression geometry before an optional
		# raised perch could be placed. It must retain the full serialized
		# key/collision proof and reach a live map even when that flourish falls
		# back cleanly.
		fallback_seed=12
		fallback_theme=techbase
		fallback_difficulty=3
		fallback_size=1
		fallback_layout=0
		fallback_verticality=1
		fallback_detail=1
		fallback_outdoors=0
		fallback_manifest=/tmp/procmap_compact_perch_fallback.json
		fallback_runtime_log=/tmp/procmap_compact_perch_fallback.log
		echo "=== compact optional-perch fallback seed=$fallback_seed ==="
		output=$(run_test "$fallback_seed" "$fallback_theme" "$fallback_difficulty" \
			"$fallback_size" "$fallback_layout" "$fallback_verticality" \
			"$fallback_detail" "$fallback_outdoors")
		if ! echo "$output" | grep -q "Dumped UDMF"; then
			echo "$output" | grep -E "Dumped UDMF|Generation failed" || true
			echo "Compact optional-perch fallback failed to generate"
			failures=$((failures + 1))
		else
			output=$(run_manifest "$fallback_seed" "$fallback_theme" "$fallback_difficulty" \
				"$fallback_size" "$fallback_layout" "$fallback_verticality" \
				"$fallback_detail" "$fallback_outdoors" "$fallback_manifest")
			if [ ! -s "$fallback_manifest" ] ||
					! validate_manifest "$fallback_manifest" "$fallback_size" \
						"$fallback_difficulty" "$fallback_verticality" ||
					! validate_manifest_udmf_connectors "$fallback_manifest" /tmp/procmap_test.udmf ||
					! validate_manifest_udmf_collision_navigation "$fallback_manifest" \
						/tmp/procmap_test.udmf ||
					! validate_key_progression; then
				echo "$output" | grep -E "Dumped.*manifest|Generation failed" || true
				echo "Compact optional-perch fallback lost structural accessibility"
				failures=$((failures + 1))
			fi
			if ! run_runtime_load "$fallback_seed" "$fallback_theme" \
					"$fallback_difficulty" "$fallback_size" "$IWAD" \
					"$fallback_runtime_log" 3 "$fallback_layout" "$fallback_verticality" \
					"$fallback_detail" "$fallback_outdoors"; then
				echo "Compact optional-perch fallback failed runtime load"
				grep -Ei 'generation failed|invalid|node|texture|unclosed|dummy subsector' \
					"$fallback_runtime_log" | tail -30 || true
				failures=$((failures + 1))
			fi
		fi
        if [ "$failures" -ne 0 ]; then
            echo "Validation failed for $failures configuration(s)"
            exit 1
        fi
        echo "All procedural generation validations passed"
        ;;
    udmf)
        head -100 /tmp/procmap_test.udmf
        ;;
    *)
		echo "Usage: $0 {validate|seeds|inspect|size|determinism|replayability|settings|themes|music|features|doors|alignment|rewards|maxsettings|network|menu|balance|doom1|load|extreme|huge|udmf} [args...]"
        exit 2
        ;;
esac
