#!/usr/bin/env bash
# Vertical autoaim test: with freelook disabled (classic aiming), shots must
# hit monsters above/below the player via vanilla-style vertical autoaim.
#
# Runs the engine under xvfb with a generated UDMF map: the player faces two
# dormant zombiemen on raised platforms straight ahead — one inside the
# engine's historical +/-35 degree autoaim cone (~20 degrees elevation) and
# one far beyond it (~57 degrees, which vanilla Doom still hits through its
# expanding slope search). The driver disables freelook, holds fire with the
# pistol, and reports both zombies' health.
#
# Asserts: the near zombie dies (autoaim works at all) and the high zombie
# dies too (classic vanilla-like vertical autoaim, not a hard 35 degree cap).
set -euo pipefail

usage() {
    cat <<'USAGE'
Verify classic vertical autoaim in the real engine.

Usage:
  ./tools/test-vertical-autoaim.sh --iwad PATH [options]

Options:
  --iwad PATH       IWAD to run (required, e.g. doom2.wad)
  --exe PATH        executable for the test (default: build/biaseddoom)
  --mod PATH        optional mod PK3 to load (e.g. Brutal Doom, whose weapons
                    are flagged NOAUTOAIM — the classic-aim override must win)
  --timeout SEC     maximum seconds for the engine process (default: 120)
  --keep-temp       retain the driver PK3, config, and logs
  -h, --help        show this help
USAGE
}

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "${script_dir}/.." && pwd)"
engine_exe="${repo_root}/build/biaseddoom"
iwad_path=""
mod_path=""
test_timeout=120
keep_temp=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --iwad)
            [[ $# -ge 2 ]] || { printf 'error: --iwad requires a path\n' >&2; exit 2; }
            iwad_path="$2"
            shift 2
            ;;
        --exe)
            [[ $# -ge 2 ]] || { printf 'error: --exe requires a path\n' >&2; exit 2; }
            engine_exe="$2"
            shift 2
            ;;
        --timeout)
            [[ $# -ge 2 ]] || { printf 'error: --timeout requires seconds\n' >&2; exit 2; }
            test_timeout="$2"
            shift 2
            ;;
        --mod)
            [[ $# -ge 2 ]] || { printf 'error: --mod requires a path\n' >&2; exit 2; }
            mod_path="$2"
            shift 2
            ;;
        --keep-temp)
            keep_temp=1
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            printf 'error: unknown option %s\n' "$1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

[[ -n "${iwad_path}" ]] || { printf 'error: --iwad is required\n' >&2; usage >&2; exit 2; }
[[ -f "${iwad_path}" ]] || { printf 'error: IWAD not found: %s\n' "${iwad_path}" >&2; exit 2; }
[[ -z "${mod_path}" || -f "${mod_path}" ]] || { printf 'error: mod not found: %s\n' "${mod_path}" >&2; exit 2; }
[[ -x "${engine_exe}" ]] || { printf 'error: executable not found: %s\n' "${engine_exe}" >&2; exit 2; }
[[ "${test_timeout}" =~ ^[1-9][0-9]*$ ]] || {
    printf 'error: --timeout must be a positive integer\n' >&2; exit 2
}
command -v timeout >/dev/null 2>&1 || { printf 'error: GNU timeout is required\n' >&2; exit 2; }
command -v python3 >/dev/null 2>&1 || { printf 'error: python3 is required\n' >&2; exit 2; }

display_runner=()
if command -v xvfb-run >/dev/null 2>&1; then
    display_runner=(xvfb-run -a -s "-screen 0 1280x800x24")
elif [[ -z "${DISPLAY:-}" && -z "${WAYLAND_DISPLAY:-}" ]]; then
    printf 'error: no display available and xvfb-run is missing\n' >&2
    exit 2
fi

test_root="$(mktemp -d "${TMPDIR:-/tmp}/biaseddoom-autoaim-test.XXXXXX")"
cleanup() {
    if [[ "${keep_temp}" -eq 0 ]]; then
        rm -rf "${test_root}"
    else
        printf 'keeping test artifacts in %s\n' "${test_root}"
    fi
}
trap cleanup EXIT

# ---------------------------------------------------------------------------
# Generate the driver PK3 (test map + Python driver)
# ---------------------------------------------------------------------------

driver_src="${test_root}/driver"
mkdir -p "${driver_src}/pyscripts" "${driver_src}/maps"

cat > "${driver_src}/MAPINFO" <<'EOF'
map TESTAIM "Autoaim Test"
{
    sky1 = "SKY1"
}
EOF

cat > "${driver_src}/PYTHON" <<'EOF'
# One UTF-8 VFS path per line. Paths are resolved inside this PK3 only.
pyscripts/main.py
EOF

cat > "${driver_src}/DECORATE" <<'EOF'
// Inert shootable target: no AI, no movement, no knockback, no wake-on-noise.
// Mods (e.g. Brutal Doom) do not replace or manage this custom class, which
// keeps the autoaim measurement deterministic under mod load.
ACTOR AimTarget 31000
{
    Health 20
    Radius 20
    Height 56
    Mass 0x7FFFFFFF
    +SOLID
    +SHOOTABLE
    +NOBLOOD
    States
    {
    Spawn:
        TNT1 A -1
        Stop
    Death:
        TNT1 A -1
        Stop
    }
}
EOF

cat > "${driver_src}/pyscripts/main.py" <<'EOF'
"""Vertical autoaim test driver."""

import os

import biaseddoom as bd

MONSTER = "AimTarget"
IS_MOD = os.environ.get("AIM_MOD", "") == "1"


def player_pawn():
    for actor in bd.actor_refs("PlayerPawn"):
        return actor
    return None


def zombies():
    found = []
    for actor in bd.actor_refs(MONSTER):
        found.append((round(actor.z), actor.health, actor.alive))
    found.sort()
    return found


def zombies_verbose():
    out = []
    for actor in bd.actor_refs(MONSTER):
        out.append((actor.class_name, round(actor.x), round(actor.y),
                    round(actor.z), round(actor.height),
                    actor.health, actor.alive))
    out.sort(key=lambda e: (e[1], e[2]))
    return out


def ammo():
    pawn = player_pawn()
    return pawn.inventory_count("Clip") if pawn else -1


def rifle_ammo():
    if not IS_MOD:
        return None
    pawn = player_pawn()
    if not pawn:
        return -1
    try:
        return (pawn.inventory_count("RifleAmmo"),
                pawn.inventory_count("Clip2"),
                pawn.inventory_count("Clip"))
    except Exception:
        return None


def step_watch():
    pawn = player_pawn()
    bd.log(f"PYTEST AIMWATCH z={zombies_verbose()} pitch={pawn.pitch if pawn else 'n/a'} ammo={rifle_ammo()}")


def step_setup():
    bd.execute("freelook false")
    bd.execute("sv_freelook 1")
    bd.execute("autoaimenabled true")
    bd.execute("autoaim 35")
    if IS_MOD:
        # Classic-aim override must defeat the mod arsenal's NOAUTOAIM flags.
        # (Test-sensitivity hook: AIM_NO_OVERRIDE=1 skips this so the failure
        # mode of an unmodified engine can be demonstrated.)
        if os.environ.get("AIM_NO_OVERRIDE", "") != "1":
            bd.execute("bd_classic_autoaim true")
        # Deterministic weapon regardless of the mod's player-class system.
        bd.execute("give Rifle")
        bd.execute("give RifleAmmo 200")
        bd.execute("give Clip2 200")
        bd.execute("use Rifle")
    else:
        bd.execute("give Clip 200")
    bd.execute("god")
    pawn = player_pawn()
    bd.log(f"PYTEST AIMSETUP zombies={zombies()} clip={ammo()} pitch={pawn.pitch if pawn else 'n/a'}")


def step_fire():
    bd.log(f"PYTEST AIMFIRE clip={ammo()}")
    bd.execute("+attack")


def step_stop():
    bd.execute("-attack")


def step_report():
    bd.log(f"PYTEST AIMRESULT zombies={zombies()} clip={ammo()} ammo={rifle_ammo()}")
    bd.execute("echo PYTEST AIM_DONE; wait 5; quit")


@bd.on("map_load")
def on_map_load(event):
    bd.schedule(step_setup, delay=10)
    bd.schedule(step_fire, delay=40)
    bd.schedule(step_watch, delay=45)
    bd.schedule(step_watch, delay=120)
    bd.schedule(step_watch, delay=230)
    bd.schedule(step_stop, delay=40 + 200)
    bd.schedule(step_report, delay=40 + 220)
EOF

# UDMF test map: player at the west end facing east (+x) in a sealed room;
# two raised pillar platforms straight ahead, each sealed into its own
# sector by four two-sided blocking lines, each with a dormant zombieman:
#   - pillar A floor 256 (monster center elevation ~20 deg, inside 35 deg)
#   - pillar B floor 1024 (monster center elevation ~57 deg, vanilla hits this)
python3 - "${driver_src}" <<'PYEOF'
import os
import struct
import sys

src = sys.argv[1]

textmap = """namespace = "zdoom";

thing { x = 0.0; y = 0.0; angle = 0; type = 1; skill1 = true; skill2 = true; skill3 = true; skill4 = true; skill5 = true; skill6 = true; skill7 = true; skill8 = true; single = true; coop = true; dm = true; }
thing { x = 520.0; y = -16.0; angle = 180; type = 31000; skill1 = true; skill2 = true; skill3 = true; skill4 = true; skill5 = true; skill6 = true; skill7 = true; skill8 = true; single = true; coop = true; dm = true; }  // on pillar A (z=256)
thing { x = 520.0; y = 16.0; angle = 180; type = 31000; skill1 = true; skill2 = true; skill3 = true; skill4 = true; skill5 = true; skill6 = true; skill7 = true; skill8 = true; single = true; coop = true; dm = true; }   // on pillar B (z=1024)
thing { x = 300.0; y = 16.0; angle = 180; type = 31000; skill1 = true; skill2 = true; skill3 = true; skill4 = true; skill5 = true; skill6 = true; skill7 = true; skill8 = true; single = true; coop = true; dm = true; }   // control: same floor as the player

vertex { x = -256.0; y = -512.0; }  // 0..3: room
vertex { x = 1024.0; y = -512.0; }
vertex { x = 1024.0; y = 512.0; }
vertex { x = -256.0; y = 512.0; }
vertex { x = 512.0; y = -80.0; }   // 4..7: pillar A
vertex { x = 768.0; y = -80.0; }
vertex { x = 768.0; y = -8.0; }
vertex { x = 512.0; y = -8.0; }
vertex { x = 512.0; y = 8.0; }     // 8..11: pillar B
vertex { x = 768.0; y = 8.0; }
vertex { x = 768.0; y = 80.0; }
vertex { x = 512.0; y = 80.0; }

sector  // 0: room, floor 0, solid high ceiling
{
    heightfloor = 0;
    heightceiling = 1536;
    lightlevel = 255;
    texturefloor = "FLOOR4_8";
    textureceiling = "CEIL3_5";
}

sector  // 1: pillar A, floor 256
{
    heightfloor = 256;
    heightceiling = 1536;
    lightlevel = 255;
    texturefloor = "FLOOR4_8";
    textureceiling = "CEIL3_5";
}

sector  // 2: pillar B, floor 768
{
    heightfloor = 768;
    heightceiling = 1536;
    lightlevel = 255;
    texturefloor = "FLOOR4_8";
    textureceiling = "CEIL3_5";
}

sidedef  // 0: room side of pillar faces (step texture) and outer walls
{
    sector = 0;
    texturemiddle = "BROWN96";
    texturelower = "BROWN96";
}
sidedef { sector = 1; }  // 1: inside of pillar A
sidedef { sector = 2; }  // 2: inside of pillar B

// outer room boundary (one-sided, front faces into the room)
linedef { v1 = 0; v2 = 3; sidefront = 0; }
linedef { v1 = 2; v2 = 3; sidefront = 0; }
linedef { v1 = 2; v2 = 1; sidefront = 0; }
linedef { v1 = 1; v2 = 0; sidefront = 0; }

// pillar A: four two-sided lines, front = room (outward), back = pillar A
linedef { v1 = 4; v2 = 5; sidefront = 0; sideback = 1; twosided = true; blocking = true; }
linedef { v1 = 5; v2 = 6; sidefront = 0; sideback = 1; twosided = true; blocking = true; }
linedef { v1 = 6; v2 = 7; sidefront = 0; sideback = 1; twosided = true; blocking = true; }
linedef { v1 = 7; v2 = 4; sidefront = 0; sideback = 1; twosided = true; blocking = true; }

// pillar B: four two-sided lines, front = room (outward), back = pillar B
linedef { v1 = 8; v2 = 9; sidefront = 0; sideback = 2; twosided = true; blocking = true; }
linedef { v1 = 9; v2 = 10; sidefront = 0; sideback = 2; twosided = true; blocking = true; }
linedef { v1 = 10; v2 = 11; sidefront = 0; sideback = 2; twosided = true; blocking = true; }
linedef { v1 = 11; v2 = 8; sidefront = 0; sideback = 2; twosided = true; blocking = true; }
"""

lumps = [("TESTAIM", b""), ("TEXTMAP", textmap.encode("utf-8")), ("ENDMAP", b"")]
payload = b""
directory = b""
offset = 12
for name, data in lumps:
    payload += data
    directory += struct.pack("<II8s", offset, len(data), name.encode("ascii").ljust(8, b"\0"))
    offset += len(data)

wad = b"PWAD" + struct.pack("<II", len(lumps), offset) + payload + directory
with open(os.path.join(src, "maps", "testaim.wad"), "wb") as fh:
    fh.write(wad)
PYEOF

driver_pk3="${test_root}/autoaim_test.pk3"
python3 - "${driver_src}" "${driver_pk3}" <<'EOF'
import os
import sys
import zipfile

src, out = sys.argv[1], sys.argv[2]
with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as pk3:
    for root, _dirs, files in os.walk(src):
        for name in files:
            full = os.path.join(root, name)
            pk3.write(full, os.path.relpath(full, src))
EOF

# ---------------------------------------------------------------------------
# Run the engine
# ---------------------------------------------------------------------------

stdout_file="${test_root}/autoaim.stdout"
log_file="${test_root}/autoaim.log"

mod_args=()
if [[ -n "${mod_path}" ]]; then
    mod_args=(-file "${mod_path}")
    export AIM_MOD=1
else
    export AIM_MOD=0
fi

set +e
timeout --signal=INT --kill-after=5s "${test_timeout}s" \
    "${display_runner[@]}" "${engine_exe}" \
    -stdout -nosound -nointro -python \
    -config "${test_root}/autoaim.ini" \
    -iwad "${iwad_path}" "${mod_args[@]}" -file "${driver_pk3}" \
    +vid_activeinbackground true +i_pauseinbackground false \
    +vid_defwidth 640 +vid_defheight 480 +vid_fullscreen false \
    +logfile "${log_file}" +map testaim \
    >"${stdout_file}" 2>&1
status=$?
set -e

fail() {
    printf 'FAIL: %s\n' "$1" >&2
    printf '%s\n' '--- engine output tail ---' >&2
    tail -n 60 "${stdout_file}" >&2 || true
    exit 1
}

[[ "${status}" -eq 0 ]] || fail "engine exited with status ${status}"
grep -Fq 'PYTEST AIM_DONE' "${stdout_file}" || fail "driver did not complete"

setup_line="$(grep -a 'PYTEST AIMSETUP' "${stdout_file}" | tail -1)"
fire_line="$(grep -a 'PYTEST AIMFIRE' "${stdout_file}" | tail -1)"
result_line="$(grep -a 'PYTEST AIMRESULT' "${stdout_file}" | tail -1)"
[[ -n "${setup_line}" && -n "${result_line}" ]] || fail "missing driver reports"
printf '%s\n%s\n' "${setup_line}" "${result_line}"

# Ammo must have been consumed, otherwise nothing was fired at all. The
# baseline is taken at fire time because `give` lands a tic after setup. (Mod
# ammo classes vary, so this check only runs in the vanilla case.)
clip_before="$(printf '%s' "${fire_line}" | sed -n 's/.*clip=\([0-9-]*\).*/\1/p')"
clip_after="$(printf '%s' "${result_line}" | sed -n 's/.*clip=\([0-9-]*\).*/\1/p')"
if [[ -z "${mod_path}" ]]; then
    [[ -n "${clip_before}" && -n "${clip_after}" ]] || fail "missing ammo report"
    awk -v b="${clip_before}" -v a="${clip_after}" 'BEGIN { exit !(a < b) }' \
        || fail "no shots were fired (clip ${clip_before} -> ${clip_after}); console +attack did not work"
fi

# Evaluate the zombie report: the zombie inside the old 35-degree cone must
# die (autoaim works at all) and the zombie at ~55 degrees must die too
# (classic vanilla-like expanding vertical autoaim).
python3 - "${result_line}" <<'EOF' || fail "vertical autoaim results wrong (see report above)"
import ast
import sys

line = sys.argv[1]
report = ast.literal_eval(line.split("zombies=", 1)[1].split(" clip=", 1)[0])
by_z = {z: (health, alive) for z, health, alive in report}

def dead(z):
    health, alive = by_z.get(z, (None, None))
    return health is not None and (not alive or health <= 0)

errors = []
if not dead(0):
    errors.append("same-floor control zombie survived (fire path broken)")
if not dead(256):
    errors.append("zombie inside the 35-degree cone survived (autoaim broken)")
if not dead(768):
    errors.append("zombie at ~55 degrees survived (no vanilla-like expanding vertical autoaim)")
for err in errors:
    print(f"FAIL: {err}", file=sys.stderr)
sys.exit(1 if errors else 0)
EOF

printf 'PASS: classic vertical autoaim hits elevated targets at any vanilla-reachable angle\n'
