#!/bin/bash
# Headless procedural map generator smoke and structural validation.

set -u

ROOT="$(cd "$(dirname "$0")" && pwd)"
RERELEASE_IWAD="/home/ericson-willians/.steam/debian-installation/steamapps/common/Ultimate Doom/rerelease/doom2.wad"
RERELEASE_DOOM_IWAD="/home/ericson-willians/.steam/debian-installation/steamapps/common/Ultimate Doom/rerelease/doom.wad"
CONFIG_IWAD="/home/ericson-willians/.config/biaseddoom/doom2.wad"
BIN="${BIN:-$ROOT/build/biaseddoom}"
IWAD="${IWAD:-}"

if [ -z "$IWAD" ]; then
    if [ -f "$RERELEASE_IWAD" ]; then
        IWAD="$RERELEASE_IWAD"
    else
        IWAD="$CONFIG_IWAD"
    fi
fi

if [ ! -f "$IWAD" ]; then
    echo "ERROR: IWAD not found. Set IWAD=/path/to/doom2.wad"
    exit 1
fi

if [ ! -x "$BIN" ]; then
    echo "ERROR: binary not found at $BIN"
    exit 1
fi

run_test() {
    local seed=$1
    local theme=${2:-techbase}
	local difficulty=${3:-3}
	local size=${4:-3}
	local layout=${5:-1}
	local verticality=${6:-1}
	local detail=${7:-1}
	local outdoors=${8:-1}
	local timeout_seconds=$((20 + size * 2))
	{ timeout --signal=KILL "$timeout_seconds" "$BIN" -nosound -nomusic -nogui -iwad "$IWAD" \
		+dumpprocudmf "$seed" "$theme" "$difficulty" "$size" \
		"$layout" "$verticality" "$detail" "$outdoors" +quit 2>&1 || true; } 2>/dev/null
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
    setsid stdbuf -oL -eL "$BIN" -nosound -nomusic -nogui -iwad "$iwad" \
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
    setsid stdbuf -oL -eL "$BIN" -nosound -nomusic -nogui -iwad "$iwad" \
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
    ALSOFT_DRIVERS=null setsid stdbuf -oL -eL "$BIN" -nogui -noautoload -iwad "$iwad" \
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
    decorations=$(grep -Ec '^\s*type = (15|20|35|41|43|44|45|46|48|55|56|57|85|86|2028|2035);' \
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

def lround(value):
    return math.floor(value + 0.5) if value >= 0.0 else math.ceil(value - 0.5)

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
            expected_x = lround((128.0 - length) * 0.5)
            if int(sides[front].get('offsetx', '0')) != expected_x:
                errors.append(f'one-sided linedef {index} is not symmetrically centered horizontally')
            expected_y = -round(float(sectors[front_sector]['heightfloor']))
            if int(sides[front].get('offsety', '0')) != expected_y:
                errors.append(f'one-sided linedef {index} is not world-aligned vertically')
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
    elif secret_door and round(face_width) not in (64, 128):
        errors.append(f'secret door {index} has non-stock {face_width:.1f}-unit width')
    elif not secret_door and abs(face_width - native_width) > 0.01:
        errors.append(f'door {index} is {face_width:.1f} units wide but '
                      f'{face_texture} is {native_width} units wide')
    expected_crop = round(max(0.0, (native_width - face_width) * 0.5))
    if int(sides[front].get('offsetx', '0')) != expected_crop:
        errors.append(f'door {index} does not contain {face_texture} inside its jambs')
    front_sector = sectors[int(sides[front]['sector'])]
    face_height = float(front_sector['heightceiling']) - float(sector['heightfloor'])
    if not secret_door and abs(face_height - native_height) > 0.01:
        errors.append(f'door {index} has {face_height:.1f}-unit lintel clearance but '
                      f'{face_texture} is {native_height} units tall')
    if secret_door and round(face_height) not in (96, 128):
        errors.append(f'secret door {index} has non-stock {face_height:.1f}-unit clearance')
    expected_scale = min(1.0, native_height / max(1.0, face_height))
    actual_scale = float(sides[front].get('scaley_top', '1.0'))
    if abs(actual_scale - expected_scale) > 0.001:
        errors.append(f'door {index} vertically repeats instead of fitting once '
                      f'(scale={actual_scale:.3f}, expected={expected_scale:.3f})')
    actual_back_scale = float(sides[back].get('scaley_top', '1.0'))
    if abs(actual_back_scale - expected_scale) > 0.001:
        errors.append(f'door {index} has mismatched back-face vertical scale')
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

remote_openers = [line for line in lines if line.get('special') == '11']
switch_openers = [line for line in remote_openers
                  if line.get('playeruse') == 'true' and
                  1500 <= int(line.get('arg0', '0')) < 2000]
key_triggers = [line for line in remote_openers
                if line.get('playercross') == 'true' and
                1000 <= int(line.get('arg0', '0')) < 1500]
if not switch_openers:
    errors.append('map contains no usable switch-operated reveal area')
if not key_triggers:
    errors.append('map contains no key-platform ambush trigger')
for opener in switch_openers + key_triggers:
    target = int(opener.get('arg0', '0'))
    if target not in sector_ids:
        errors.append(f'Door_Open trigger targets missing sector id {target}')
        continue
    target_sector = sectors[sector_ids[target]]
    if target_sector['heightfloor'] != target_sector['heightceiling']:
        errors.append(f'remote door sector id {target} does not start closed')
    if opener.get('arg1') != '16':
        errors.append(f'remote door sector id {target} uses the wrong speed')
for opener in switch_openers:
    side = sides[int(opener['sidefront'])]
    if side.get('texturemiddle') not in {'SW1COMP', 'SW1GARG'}:
        errors.append('remote Door_Open use line does not use a fitted 64x128 switch texture')
switch_counts = collections.Counter(int(opener.get('arg0', '0')) for opener in switch_openers)
for target, count in switch_counts.items():
    if count != 1:
        errors.append(f'remote reveal sector id {target} has {count} switch panels instead of one')

# Reveals may be clipped freestanding pavilions, rectangular wall banks, or
# false-wall chambers grown into an empty neighboring cell. Recover their door
# and closet topology from the serialized graph instead of assuming one shell.
reveal_targets = sorted(set(int(opener.get('arg0', '0'))
                            for opener in switch_openers + key_triggers))
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
            ((target < 1500 and thing.get('ambush') == 'true') or
             (target >= 1500 and thing.get('type') in {'2008', '2012'}))
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
    inner_walls = []
    for line_index in inner_lines:
        line = lines[line_index]
        a, b = vertices[int(line['v1'])], vertices[int(line['v2'])]
        inner_walls.append((float(a['x']), float(a['y']),
                            float(b['x']), float(b['y'])))
    contained = [thing for thing in things
                 if inner_min_x < float(thing['x']) < inner_max_x and
                 inner_min_y < float(thing['y']) < inner_max_y]
    if target < 1500:
        contained_ambushers = [thing for thing in contained if thing.get('ambush') == 'true']
        if len(contained_ambushers) != 2:
            errors.append(f'key reveal sector id {target} contains '
                          f'{len(contained_ambushers)} ambushers instead of two')
        for thing in contained_ambushers:
            actor_clearance = min(point_segment_distance(
                float(thing['x']), float(thing['y']), *wall) for wall in inner_walls)
            if actor_clearance < 21.9:
                errors.append(f'key reveal sector id {target} embeds an ambusher only '
                              f'{actor_clearance:.1f} units from its shaped wall')
    else:
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
if not perch_sector_ids:
    errors.append('map contains no elevated ranged-monster perch')
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
if not lift_sector_ids:
    errors.append('map contains no operable bypassable lift')
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

ambushers = [thing for thing in things
             if thing.get('type') in monster_types and thing.get('ambush') == 'true']
if len(ambushers) < 2:
    errors.append('key reveal closet contains fewer than two ambush monsters')

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
                    '55', '56', '57', '85', '86', '2028', '2035'}
solid_decoration_types = decoration_types - {'15', '20'}
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
                  f'passage/door approach at linedef {passage_obstruction[1]}')

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
    decorations=$(grep -Ec '^\s*type = (15|20|35|41|43|44|45|46|48|55|56|57|85|86|2028|2035);' \
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
    # terraces are explicit sectors. The cap remains linear at size 80 while
    # allowing those authored structures instead of rewarding flat empty space.
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
    max_monsters=$((40 + size * 25 + size * 6))

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
		if ! grep -q '^\s*type = 41;' /tmp/procmap_test.udmf ||
				! grep -q '^\s*type = 43;' /tmp/procmap_test.udmf; then
			echo "    Hell finale/outdoor semiotics are missing their evil-eye or torch-tree marker"
            failures=$((failures + 1))
        fi
        if grep -q '^\s*type = 5;' /tmp/procmap_test.udmf &&
                ! grep -q '^\s*type = 44;' /tmp/procmap_test.udmf; then
            echo "    blue key shrine is missing its blue torch marker"
            failures=$((failures + 1))
        fi
        if grep -q '^\s*type = 13;' /tmp/procmap_test.udmf &&
                ! grep -q '^\s*type = 46;' /tmp/procmap_test.udmf; then
            echo "    red key shrine is missing its red torch marker"
            failures=$((failures + 1))
        fi
        if grep -q '^\s*type = 6;' /tmp/procmap_test.udmf &&
                ! grep -q '^\s*type = 35;' /tmp/procmap_test.udmf; then
            echo "    yellow key shrine is missing its gold candelabra marker"
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
				! grep -q '^\s*type = 35;' /tmp/procmap_test.udmf ||
				! grep -q '^\s*type = 45;' /tmp/procmap_test.udmf ||
				! grep -q '^\s*type = 43;' /tmp/procmap_test.udmf; then
            echo "    gothic theme is missing marble/wood architecture or candelabra/tall-torch rhythm"
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
        for size in 1 3 5 10 20 40 80; do
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
		echo "Determinism check passed: $first"
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
			setting_decor[$label]=$(grep -Ec '^\s*type = (15|20|35|41|43|44|45|46|48|55|56|57|85|86|2028|2035);' \
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
		if [ ! -f "$RERELEASE_IWAD" ] || [ ! -f "$RERELEASE_DOOM_IWAD" ]; then
			echo "ERROR: Doom and Doom II IWADs are required for soundtrack validation"
			exit 1
		fi
		music_dir=/tmp/procmap_music_matrix
		rm -rf "$music_dir"
		mkdir -p "$music_dir"
		if ! capture_proc_music 12345 "$RERELEASE_IWAD" "$music_dir/doom2_a.log" ||
				! capture_proc_music 12345 "$RERELEASE_IWAD" "$music_dir/doom2_b.log" ||
				! capture_proc_music 54321 "$RERELEASE_IWAD" "$music_dir/doom2_c.log" ||
				! capture_proc_music 12345 "$RERELEASE_DOOM_IWAD" "$music_dir/doom1.log"; then
			echo "Procedural soundtrack selection did not reach a loaded map"
			exit 1
		fi
		if ! python3 - "$RERELEASE_IWAD" "$RERELEASE_DOOM_IWAD" "$music_dir" <<'PY'
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
		if ! run_software_midi_smoke 12345 "$RERELEASE_IWAD" "$music_dir/software_midi.log"; then
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
		if [ ! -f "$RERELEASE_IWAD" ] || [ ! -f "$RERELEASE_DOOM_IWAD" ]; then
			echo "ERROR: Doom and Doom II IWADs are required for fluid-flat validation"
			exit 1
		fi
		if ! python3 - "$RERELEASE_DOOM_IWAD" "$RERELEASE_IWAD" <<'PY'
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
if not {64, 128}.issubset(widths):
    errors.append(f'door matrix lacks both stock widths: {sorted(widths)}')
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
		echo "Stock-width door recesses and 72/96/112/128-unit profiles passed"
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
		echo "=== maximum all-high settings seed=$seed theme=techbase difficulty=3 size=80 ==="
		output=$(run_test "$seed" techbase 3 80 2 2 2 2)
		echo "$output" | grep -E 'Dumped UDMF|Generation failed' || true
		if ! echo "$output" | grep -q 'Dumped UDMF'; then
			exit 1
		fi
		report_dump
		if ! validate_dump 80 techbase 2 2 2; then
			echo "Maximum all-high settings failed serialized structural validation"
			exit 1
		fi
		log=/tmp/procmap_maximum_settings.log
		if ! run_runtime_load "$seed" techbase 3 80 "$IWAD" "$log" 3 2 2 2 2; then
			echo "Maximum all-high settings failed runtime/node validation"
			grep -Ei 'error|failed|invalid|unknown|node|texture|unclosed|dummy subsector' \
				"$log" | tail -30 || true
			exit 1
		fi
		echo "Maximum all-high settings passed structural and runtime/node validation"
		;;
	menu)
        menu_dump=/tmp/procmap_menu_definition.txt
        menu_config=/tmp/procmap_menu_test.ini
        menu_log=/tmp/procmap_menu_test.log
        launch_log=/tmp/procmap_menu_launch.log
        rm -f "$menu_dump" "$menu_config" "$menu_log" "$launch_log"

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
            '"procmap_restore_defaults"'
        )
        for pattern in "${required_menu_patterns[@]}"; do
            if ! grep -Fq "$pattern" "$menu_dump"; then
                echo "Packed procedural menu is missing: $pattern"
                exit 1
            fi
        done

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

		"$BIN" -nosound -nomusic -nogui -config "$menu_config" -iwad "$IWAD" \
			+procgen_seed 123456 +procgen_theme hell +procgen_difficulty 5 +procgen_size 5 \
			+procgen_layout 2 +procgen_verticality 2 +procgen_detail 2 +procgen_outdoors 2 \
			+procmap_restore_defaults +quit >"$menu_log" 2>&1
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

        "$BIN" -nosound -nomusic -nogui -config "$menu_config" -iwad "$IWAD" \
            +procmap_randomize_seed +quit >"$menu_log" 2>&1
        random_seed=$(sed -n 's/^procgen_seed=//p' "$menu_config")
        if ! grep -q 'Procedural map seed set to' "$menu_log" ||
                ! [[ "$random_seed" =~ ^[1-9][0-9]*$ ]]; then
            echo "Procedural menu seed randomization failed"
            exit 1
        fi

        status=0
        # Keep redirected engine output line-buffered and stop without entering
        # the interactive engine's signal-driven shutdown path.
        { timeout --signal=KILL 30 stdbuf -oL -eL "$BIN" -nosound -nomusic -nogui \
            -config "$menu_config" -iwad "$IWAD" +procgen_theme hell \
            +procgen_difficulty 4 +procgen_size 1 +procmap random >"$launch_log" 2>&1; } \
            2>/dev/null || status=$?
        if ! grep -q '^PROCMAP - ' "$launch_log"; then
            echo "Procedural menu launch command did not enter PROCMAP (exit=$status)"
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
    doom1)
        if [ ! -f "$RERELEASE_DOOM_IWAD" ]; then
            echo "ERROR: Ultimate Doom IWAD not found at $RERELEASE_DOOM_IWAD"
            exit 1
        fi
        IWAD="$RERELEASE_DOOM_IWAD"
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
        done
        for spec in "${specs[@]}"; do
            read -r seed theme difficulty size <<<"$spec"
            doom1_runtime_log="/tmp/procmap_doom1_runtime_${theme}.log"
            if ! run_runtime_load "$seed" "$theme" "$difficulty" "$size" \
                    "$RERELEASE_DOOM_IWAD" "$doom1_runtime_log"; then
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
            "8080 corrupted 3 80"
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
        specs=(
            "1 techbase 2 80"
            "42 hell 4 80"
            "8080 industrial 3 80"
            "2147483647 gothic 5 80"
            "501721273 corrupted 3 80"
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
		echo "Usage: $0 {validate|seeds|inspect|size|determinism|settings|themes|music|features|doors|rewards|maxsettings|menu|balance|doom1|load|extreme|huge|udmf} [args...]"
        exit 2
        ;;
esac
