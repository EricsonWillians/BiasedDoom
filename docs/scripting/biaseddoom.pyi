"""Type stubs for BiasedDoom's embedded `biaseddoom` Python module.

Drop this file into a `typings/` folder at the root of your VSCode
workspace (Pylance's default stub search path) to get completions and
inline docs when writing BiasedDoom Python scripts:

    yourmap.scripts/
        typings/
            biaseddoom.pyi     <- this file
        scripts/
            main.py

Generated against BiasedDoom 4.15.8+unreleased. Actor class constants
reflect the built-in classes; mod- and script-defined classes also work
at runtime but are resolved dynamically (see `_ActorsRegistry.__getattr__`).
"""

from typing import Any, Callable, Optional, Union

API_VERSION: int
TICRATE: int
RUNTIME: str

# Number of generic custom action buttons (+pyaction1..+pyaction32); see the
# custom_action event and the custom_action_*/set_custom_action/input_binding
# functions below.
PYACTION_COUNT: int

# Persistent per-session dictionary shared by all scripts.
state: dict

# --- player input button masks (Player.set_input / Player.buttons) ---
BT_ATTACK: int
BT_ALTATTACK: int
BT_USE: int
BT_JUMP: int
BT_CROUCH: int
BT_RELOAD: int
BT_ZOOM: int
BT_USER1: int
BT_USER2: int
BT_USER3: int
BT_USER4: int

# --- change_level flags ---
CHANGELEVEL_NOINTERMISSION: int
CHANGELEVEL_RESETINVENTORY: int
CHANGELEVEL_RESETHEALTH: int
CHANGELEVEL_NOMONSTERS: int
CHANGELEVEL_KEEPFACING: int


class Actor:
    """Live handle to an in-level actor."""
    valid: bool
    class_name: str
    tid: int
    health: int
    alive: bool
    x: float
    y: float
    z: float
    position: tuple
    angle: float
    pitch: float
    roll: float
    angles: tuple
    velocity: tuple
    velocity_x: float
    velocity_y: float
    velocity_z: float
    floor_z: float
    ceiling_z: float
    radius: float
    height: float
    mass: int
    gravity: float
    speed: float
    alpha: float
    scale_x: float
    scale_y: float
    score: int
    special: int
    damage_factor: float
    damage_multiply: float
    tint: Optional[tuple]
    args: list
    tics: int
    water_level: int
    is_monster: bool
    is_player: bool
    target: Optional["Actor"]
    master: Optional["Actor"]
    tracer: Optional["Actor"]

    # damage_factor multiplies damage TAKEN by the actor; damage_multiply
    # multiplies damage it DEALS (both default 1.0, clamped >= 0). tint is
    # an (r, g, b) tuple (0-255) remapping the sprite's brightness ramp to
    # that color in both renderers (Diablo-style monster tints), or None
    # when the actor carries no Python tint; assign None to reset to the
    # class default. Tint tables are built lazily and are NOT serialized —
    # re-apply tints on the map_load after a savegame load.

    def activate(self, activator: Optional["Actor"] = None, deactivate: bool = False) -> None:
        """Activate or deactivate the actor."""
    def call_zscript(self, name: str, *args: Any) -> Any:
        """Call a public, supported ZScript method on this actor."""
    def check_sight(self, other: "Actor", flags: int = 0) -> bool:
        """Run the native sight check."""
    def clear_inventory(self) -> None:
        """Remove all inventory."""
    def damage(self, amount: int, damage_type: str = "None", inflictor: Optional["Actor"] = None,
               source: Optional["Actor"] = None, flags: int = 0) -> int:
        """Apply native gameplay damage."""
    def destroy(self) -> None:
        """Destroy the actor."""
    def distance_to(self, other: "Actor") -> float:
        """Return 3D distance to another actor."""
    def get_flag(self, name: str) -> bool:
        """Read an actor flag by engine name."""
    def give_inventory(self, class_name: str, amount: int = 1) -> None:
        """Give inventory through the native pickup path."""
    def heal(self, amount: int, maximum: int = 0) -> None:
        """Restore health through the native healing path."""
    def inventory_count(self, class_name: str) -> int:
        """Return an inventory amount."""
    def play_sound(self, sound: str, channel: int = 0, volume: float = 1.0,
                   looping: bool = False, attenuation: float = 1.0, local: bool = False,
                   pitch: float = 1.0) -> None:
        """Start an actor sound."""
    def set_flag(self, name: str, value: bool) -> None:
        """Change an actor flag by engine name."""
    def set_position(self, x: float, y: float, z: float, check: bool = True, fog: bool = True) -> None:
        """Move immediately, optionally checking collision."""
    def set_state(self, label: str, call_actions: bool = True) -> None:
        """Enter a named actor state."""
    def set_velocity(self, x: float, y: float, z: float, add: bool = False) -> None:
        """Replace or add to velocity."""
    def snapshot(self) -> dict:
        """Return a serialization-friendly snapshot."""
    def stop_sound(self, channel: int = 0) -> None:
        """Stop an actor sound channel."""
    def take_inventory(self, class_name: str, amount: int = 1) -> None:
        """Take inventory."""
    def thrust(self, x: float, y: float, add: bool = False) -> None:
        """Apply horizontal/vertical thrust."""
    def use_inventory(self, class_name: str) -> None:
        """Use a named inventory item."""


class Line:
    """Live handle to a map linedef."""
    index: int
    special: int
    args: list
    activation: int
    flags: int
    health: int
    alpha: float
    front_sector: "Sector"
    back_sector: Optional["Sector"]

    def activate(self, activator: Optional[Actor] = None, back_side: int = 0, clear: bool = False) -> None:
        """Execute this line's action special."""


class Sector:
    """Live handle to a map sector."""
    index: int
    floor_height: float
    ceiling_height: float
    light: int
    gravity: float
    damage: int
    damage_interval: int
    leakiness: int
    special: int
    tags: list

    def move_floor(self, height: float, speed: float = 1.0, crush: int = -1) -> None:
        """Move the floor toward an absolute height."""
    def move_ceiling(self, height: float, speed: float = 1.0, crush: int = -1) -> None:
        """Move the ceiling toward an absolute height."""


class Player:
    """Live handle to an in-game player."""
    index: int
    name: str
    valid: bool
    actor: Optional[Actor]
    buttons: int
    forward_move: float
    side_move: float
    up_move: float
    input_yaw: float
    input_pitch: float
    input_roll: float
    fov: float
    frag_count: int
    kill_count: int
    item_count: int
    secret_count: int

    def set_input(self, buttons: Optional[int] = None, forward: Optional[float] = None,
                  side: Optional[float] = None, up: Optional[float] = None,
                  yaw: Optional[float] = None, pitch: Optional[float] = None) -> None:
        """Override the current native user command."""
    def set_weapon(self, class_name: str) -> None:
        """Switch to an owned weapon class."""


# --- events -----------------------------------------------------------------

def on(event_name: str, *, every: int = 1, priority: int = 0,
       class_name: Optional[str] = None, tid: int = 0, player: int = -1) -> Callable:
    """Decorator registering a callback for a BiasedDoom lifecycle event.

    Events: engine_start, map_load, map_unload, pre_tick, tick, post_tick,
    actor_spawned, actor_died, actor_damaged, actor_before_damage,
    actor_destroyed, actor_revived,
    line_activated, line_activation_failed, player_entered, player_spawned,
    player_respawned, player_died, player_disconnected, item_picked,
    secret_found, item_dropped, weapon_changed, sector_entered, sector_exited,
    conversation_started, conversation_reply, ui_command, custom_action, save,
    load, engine_shutdown.

    actor_died event fields: actor, actor_ref, inflictor, inflictor_ref,
    attacker_ref, attacker_class, attacker_player_index. attacker_* report the
    killer (Actor.Die's source): the shooter for missile kills (the missile
    stays in inflictor_ref), the attacker itself for hitscan/melee, the bomb
    owner for explosions, None for environmental deaths or damage applied
    without a source.
    actor_before_damage event fields: actor_ref, inflictor_ref, attacker_ref,
    attacker_class, attacker_player_index, damage, damage_type, flags, angle,
    cancel. MUTABLE pre-damage filter: damage, damage_type and cancel are
    write-back keys — mutate the event dict in place to rewrite the incoming
    hit (damage is clamped to [0, 2**31-1]) or set cancel=True to swallow it
    entirely (no damage, no actor_damaged event). Offline-only: not dispatched
    in multiplayer or demo sessions.
    item_picked event fields: class_name, name, amount, player.
    secret_found event fields: player, found_secrets, total_secrets.
    item_dropped event fields: actor_ref, dropper_ref, player_index, class_name, amount.
    weapon_changed event fields: player_index, weapon, actor_ref, player_ref.
    sector_entered/sector_exited event fields: sector, tags, player_index, actor_ref.
    conversation_started event fields: npc_ref, pc_ref, player_index, npc_class.
    conversation_reply event fields: player_index, npc_ref, node, reply_index,
    log_number, log_string, next_node, item_changed.
    ui_command event fields: command. Fired by the `pyui <name>` console
    command; bridge for console aliases/key binds driving script UI.
    custom_action event fields: action (1..PYACTION_COUNT), pressed (bool).
    Fired on every press and release of the generic custom action buttons
    (+pyaction1..+pyaction32), including synthetic set_custom_action changes;
    scanned once per gametic before pre_tick dispatch.
    line_activation_failed event fields: line_index, special, args, actor_ref,
    activation_type, reason, reason_code. reason is one of "none",
    "unknown_special", "script_not_found", "locked", "activation_filtered",
    "insufficient_resources".
    """


# --- core runtime -----------------------------------------------------------

def log(message: Any, level: str = "info") -> None:
    """log(message, level='info') -> None; level: info/warning/error/debug."""

def current_map() -> Optional[str]:
    """Return the current map lump name or None."""

def level_time() -> int:
    """Return elapsed level time in 35 Hz tics."""

def headless() -> bool:
    """Return True when the engine runs on the null video driver (nothing
    renders; imgui_frame never fires)."""

def session_read_only() -> bool:
    """Return True when world mutations are blocked (multiplayer or demo session)."""

def get_cvar(name: str) -> Any:
    """Read a console variable using its native Python type."""

def set_cvar(name: str, value: Any) -> Any:
    """Set a console variable and return the applied value."""

def execute(command: str) -> None:
    """Queue an engine console command."""

def execute_acs(script: Any, arguments: Optional[list] = None, always: bool = False,
                want_result: bool = False) -> Any:
    """Start a numeric or named ACS script."""

def read_text(path: str) -> str:
    """Read a UTF-8 resource from the current mod."""

def read_bytes(path: str) -> bytes:
    """Read a binary resource from the current mod, capped at 32 MiB."""

def import_script(path: str, module_name: Optional[str] = None) -> Any:
    """Execute and return another Python module from the current mod. The
    module is also registered in sys.modules under module_name, so sibling
    scripts loaded afterwards can reach it with plain `import module_name`
    (import in dependency order; circular imports are not supported)."""

def schedule(callback: Callable, delay: int = 0, repeat: int = 0, map_local: bool = True) -> int:
    """Schedule a one-shot or repeating callable in engine tics; returns a task ID."""

def cancel_task(task_id: int) -> None:
    """Cancel a scheduled task by ID."""

def task_count() -> int:
    """Return the number of active scheduled tasks."""

def profile() -> dict:
    """Return per-callback timing and budget statistics."""

def reset_profile() -> None:
    """Reset callback timing and budget statistics."""

def assert_true(cond: Any, msg: str = "") -> None:
    """Fail the script test with a message when the condition is false. Prints
    SCRIPT ASSERT FAILED in red with the caller's file/line, feeds the
    -pyerrorlog JSON feed with severity "assert", and makes -scripttest exit
    1, but returns None instead of raising so a test can report several
    failures in one run."""

def warn(message: str) -> None:
    """Print a rate-limited yellow SCRIPT WARNING line and feed -pyerrorlog
    with severity "warning". Identical consecutive warnings are deduplicated
    (a repeat summary prints every ~10 seconds). Never counts as an error."""


# --- deterministic script RNG (serialized with savegames via state) ----------

def random() -> float:
    """Return the next deterministic float in [0, 1)."""

def randrange(lo: int, hi: Optional[int] = None) -> int:
    """Return the next deterministic integer in [lo, hi); randrange(hi) uses [0, hi)."""

def randint(lo: int, hi: int) -> int:
    """Return the next deterministic integer in [lo, hi] inclusive."""

def choice(sequence: Union[list, tuple]) -> Any:
    """Return a deterministic item from a non-empty sequence."""


# --- custom action buttons (local-only input; not recorded in demos) --------

def custom_action_down(n: int) -> bool:
    """True while custom action n (1..PYACTION_COUNT) is held. Raises
    ValueError when n is outside 1..PYACTION_COUNT."""

def custom_action_mask() -> int:
    """Bitmask of the held custom actions; bit n-1 is set while action n is
    down."""

def set_custom_action(n: int, down: bool) -> None:
    """Synthetically press (down=True) or release (down=False) custom action
    n, driving the same button state as the bound key, so the per-tic scan
    emits the same custom_action event on the next gametic. Idempotent:
    requesting the current state is a no-op. Raises ValueError when n is
    outside 1..PYACTION_COUNT."""

def input_binding(command: str) -> Optional[str]:
    """Display name of the first key bound to a console command like
    '+pyaction1' (engine-canonical names: 'Q', 'Mouse1', 'Space', ...), or
    None when unbound."""


# --- snapshots --------------------------------------------------------------

def players() -> list:
    """Return snapshots of all active players."""

def actor(tid: int) -> Optional[dict]:
    """Return the first actor snapshot for a TID, or None."""

def spawn_actor(class_name: str, x: float, y: float, z: float, angle: float = 0.0,
                tid: int = 0, force: bool = False) -> dict:
    """Spawn an actor and return its snapshot."""

def damage_actor(tid: int, damage: int, damage_type: str = "None") -> int:
    """Damage the first actor with a TID."""

def set_actor_velocity(tid: int, x: float, y: float, z: float) -> dict:
    """Set actor velocity by TID."""

def destroy_actor(tid: int) -> None:
    """Destroy the first actor with a TID."""


# --- live handles and native helpers ----------------------------------------

def actor_ref(tid: int) -> Optional[Actor]:
    """Return a live Actor handle for a TID, or None."""

def actor_refs(class_name: Optional[str] = None, tid: int = 0, limit: int = 4096,
               subclasses: bool = True, sphere: Any = None, z: Optional[float] = None) -> list:
    """Return lightweight live Actor handles. class_name matches derived
    classes unless subclasses=False (exact class match). sphere=(x, y, r)
    filters by 2D distance; with z it also requires |actor.z - z| <= r.
    All filters combine (AND) and run natively during the scan."""

def spawn(class_name: str, x: float, y: float, z: float, angle: float = 0.0,
          tid: int = 0, force: bool = False) -> Actor:
    """Spawn and return a live Actor handle."""

def actor_data(ref: Actor) -> dict:
    """Return the actor's persistent per-actor data dict, creating it on
    demand. Purged automatically when the actor is destroyed or the map
    changes; not saved in savegames. Raises ReferenceError on a stale
    handle."""

def actor_data_drop(ref: Actor) -> bool:
    """Drop the actor's per-actor data dict if present. Returns True when one
    existed. Idempotent: stale handles return False instead of raising."""

def player(index: int = 0) -> Player:
    """Return a live Player handle by slot."""

def player_refs() -> list:
    """Return all in-game Player handles."""

def line(index: int) -> Line:
    """Return a Line handle by index."""

def lines(line_id: Optional[int] = None) -> list:
    """Return Line handles, optionally by line ID (UDMF id; Hexen tag is args[0])."""

def sector(index: int) -> Sector:
    """Return a Sector handle by index."""

def sectors(tag: Optional[int] = None) -> list:
    """Return Sector handles, optionally by tag."""

def sector_at(x: float, y: float) -> Optional[Sector]:
    """Return the Sector containing the given point, or None."""

def actors_in_sector(sector: Any) -> list:
    """Return live Actor handles currently inside the given sector. Accepts a
    Sector handle or a sector index."""

def execute_special(special: Any, arguments: Optional[list] = None,
                    activator: Optional[Actor] = None, line: Optional[Line] = None,
                    back_side: int = 0) -> bool:
    """Execute any numeric or named action special."""

def radius_damage(spot: Any, damage: int, distance: float, source: Optional[Actor] = None,
                  damage_type: str = "None", hurt_source: bool = True) -> int:
    """Perform a native radius attack."""

def spawn_missile(source: Actor, target: Optional[Actor] = None, class_name: str = "",
                  position: Any = None, owner: Optional[Actor] = None, check: bool = True) -> Actor:
    """Spawn a native aimed missile."""

def line_attack(source: Actor, angle: float = 0.0, distance: float = 0.0, pitch: float = 0.0,
                damage: int = 0, damage_type: str = "None", puff_class: str = "",
                flags: int = 0) -> dict:
    """Fire a native hitscan and return its result."""

def apply_actor_batch(operations: list) -> int:
    """Apply many actor mutations in one C API crossing. Each operation is a
    tuple: ("position", actor, x, y, z), ("velocity"/"add_velocity", actor,
    x, y, z), ("health"/"damage", actor, amount), ("destroy", actor),
    ("speed"/"alpha"/"scale"/"damage_factor"/"damage_multiply", actor,
    value), or ("tint", actor, r, g, b) — the same sprite tint as the
    Actor.tint property."""

def actor_field_batch(refs: list, fields: list) -> list:
    """Read many actor fields in one C API crossing, returning a tuple per
    actor in the same order as refs. Stale or invalid handles yield a tuple
    of None values instead of raising. Valid field names: health, x, y, z,
    angle, pitch, roll, speed, alpha, tid, class_name, alive, is_player,
    is_monster, special, damage_factor. Unknown names raise ValueError."""

def exit_level(position: int = 0, secret: bool = False, keep_facing: bool = False) -> None:
    """Exit through the normal or secret route."""

def change_level(map_name: str, position: int = 0, flags: int = 0, next_skill: int = -1) -> None:
    """Request an explicit map transition."""

def center_message(message: str, bold: bool = False) -> None:
    """Display an immediate center-screen message."""

def set_music(name: str, order: int = 0, looping: bool = True, force: bool = False) -> None:
    """Change level music immediately."""

def player_log(player_index: int = 0) -> Optional[str]:
    """Return the player's conversation log text (the Strife journal line),
    or None when unset or the index has no player. Read-only; allowed in
    observer mode."""

def set_player_log(text: str, player_index: int = 0) -> None:
    """Set the player's conversation log text (the Strife journal line).
    Mutation-guarded; ValueError when the slot has no player."""

def start_conversation(npc: Actor) -> bool:
    """Start a Strife conversation between the local player and the given
    actor (USE-path arguments). Returns False when the actor cannot talk.
    Mutation-guarded."""


# --- gameplay director --------------------------------------------------------

class RngStream:
    """Deterministic random stream created by rng(seed). Independent of the
    engine's gameplay RNG and intentionally not serialized in savegames."""
    def int(self, lo: int, hi: int) -> int:
        """Inclusive random integer in [lo, hi]."""
    def float(self) -> float:
        """Random float in [0.0, 1.0)."""
    def choice(self, sequence: list) -> Any:
        """Random element of a non-empty sequence (ValueError if empty)."""


def rng(seed: int = 0) -> RngStream:
    """Create a deterministic random stream from a seed."""

def set_timescale(scale: float) -> float:
    """Set the game time scale (1.0 = normal, 0.05 minimum) and return the
    applied value. Forced to 1.0 in netgames."""

def get_timescale() -> float:
    """Return the current game time scale."""

def hud_text(text: str, id: int = 0, x: float = 0.5, y: float = 0.1,
             color: str = "gold", hold: float = 2.0, fade: float = 0.5) -> None:
    """Display a fading HUD message. x/y are fractions of the screen (the ACS
    HUDMessage convention); reusing an id replaces the previous message.
    color is a font color name (gold, red, green, blue, white, orange,
    yellow, cyan, ...); hold/fade are seconds. Requires an active status bar."""

def hud_clear(id: int = 0) -> None:
    """Remove the HUD message with the given id (no-op for id 0)."""

def screen_flash(r: int, g: int, b: int, alpha: float) -> None:
    """Instantly set the console player's screen blend (r/g/b are 0-255,
    alpha is 0-1). Lasts one frame; use screen_fade for timed effects."""

def screen_fade(r: int, g: int, b: int, alpha: float, seconds: float = 1.0) -> None:
    """Fade the console player's screen from the given color/alpha to
    transparent over seconds. seconds <= 0 clears the blend immediately."""

def play_ui_sound(name: str, volume: float = 1.0) -> None:
    """Play a UI (non-positional) sound for the local player."""

def save_checkpoint(name: str = "checkpoint", description: str = "") -> None:
    """Save the game into a named checkpoint slot (<savedir>/<name>.zds) at
    the next tic boundary. name is sanitized to [A-Za-z0-9_-]."""

def load_checkpoint(name: str = "checkpoint") -> None:
    """Load a named checkpoint slot at the next tic boundary, aborting the
    current level (FileNotFoundError if the slot does not exist)."""


# --- canvas drawing -----------------------------------------------------------

# Persistent display list: scripts register an item once and the engine
# re-renders it every HUD frame at no per-frame Python cost. Re-calling a
# draw function with the same id replaces that item (this is how scripts
# update/animate). Screen-space coordinates and sizes are normalized
# fractions (0..1) of the screen; world-anchored items are projected to
# screen space every frame.
#
# Every draw_* function accepts two extra keyword-only arguments:
#   layer=0      z-order; higher layers render on top, and within a layer
#                items render in registration order.
#   duration=None  seconds after which the item auto-expires (counted in
#                game time, so it pauses with the game); None = permanent.

def draw_text(text: str, *, id: int, x: float = 0.0, y: float = 0.0,
              font: str = "smallfont", color: Any = (255, 255, 255),
              scale: Any = 1.0, alpha: float = 1.0, shadow: bool = False,
              outline: bool = False, align: str = "left",
              layer: int = 0, height: float = 0.0,
              duration: Optional[float] = None) -> None:
    """Register/replace screen text at normalized (x, y). id is required;
    reusing it replaces the item. color accepts an (r, g, b) tuple (0-255)
    or a font color name string (e.g. "gold", "red"). scale is a float or
    an (sx, sy) tuple of RAW PIXEL multipliers (the text gets relatively
    smaller as the screen resolution rises). Prefer height: a normalized
    0..1 screen-height fraction for one text line, recomputed against the
    live drawer size every frame, so the text stays the same relative size
    at any resolution (e.g. height=0.02 fills 2% of the screen height).
    height > 0 overrides scale. align is 'left' (default), 'center' or
    'right' (screen-space only; anything else raises ValueError).
    shadow=True renders a real dark offset shadow (in older versions this
    flag was inert). outline=True adds a black outline whose thickness
    tracks the text size. Text supports inline
    color escapes: \x1c[Gold] switches to a named font color, \x1c-
    resets, \x1c+ toggles bold; note that with a tuple-RGB color the
    escapes only modulate brightness, so use a named color OR escapes.
    layer/duration: see the section header. Unknown font raises
    ValueError."""

def measure_text(text: str, *, font: str = "smallfont", scale: Any = 1.0) -> tuple:
    """Measure text for a font and scale; returns (width, height) in
    PIXELS (width = widest line, height = line count x font line height).
    scale is a float or an (sx, sy) tuple. Unknown font raises
    ValueError."""

def draw_rect(*, id: int, x: float = 0.0, y: float = 0.0, w: float = 0.0,
              h: float = 0.0, color: tuple = (255, 255, 255), alpha: float = 0.75,
              color2: Optional[tuple] = None, layer: int = 0,
              duration: Optional[float] = None) -> None:
    """Register/replace a filled rectangle; (x, y, w, h) are normalized
    screen fractions. color is an (r, g, b) tuple (0-255). color2, when
    given, makes the fill a vertical gradient (color at the top blending
    to color2 at the bottom). id is required; reusing it replaces the
    item. layer/duration: see the section header."""

def draw_line(*, id: int, x1: float = 0.0, y1: float = 0.0, x2: float = 0.0,
              y2: float = 0.0, color: tuple = (255, 255, 255), alpha: float = 1.0,
              layer: int = 0, duration: Optional[float] = None) -> None:
    """Register/replace a line between normalized points (x1, y1) and
    (x2, y2). color is an (r, g, b) tuple (0-255). id is required; reusing
    it replaces the item. layer/duration: see the section header."""

def draw_circle(*, id: int, x: float = 0.0, y: float = 0.0, radius: float = 0.0,
                color: tuple = (255, 255, 255), alpha: float = 1.0,
                fill: bool = False, layer: int = 0,
                duration: Optional[float] = None) -> None:
    """Register/replace a circle centered at normalized (x, y). radius is
    X-normalized (scaled by the screen width only, so circles stay round).
    fill=False draws a 32-segment polyline outline; fill=True rasterizes
    chord scanlines (slight gaps are possible at small radii). color is an
    (r, g, b) tuple (0-255). id is required; reusing it replaces the item.
    layer/duration: see the section header."""

def draw_frame(*, id: int, x: float = 0.0, y: float = 0.0, w: float = 0.0,
               h: float = 0.0, color: tuple = (255, 255, 255), thickness: int = 2,
               alpha: float = 1.0, layer: int = 0,
               duration: Optional[float] = None) -> None:
    """Register/replace a hollow rectangle (border only) at normalized
    (x, y, w, h); the border is drawn INSIDE the rect, so the given box is
    the outer edge. thickness is in pixels. color is an (r, g, b) tuple
    (0-255). id is required; reusing it replaces the item.
    layer/duration: see the section header."""

def draw_texture(name: str, *, id: int, x: float = 0.0, y: float = 0.0,
                 scale: Any = 1.0, alpha: float = 1.0,
                 tint: Optional[tuple] = None, rotate: float = 0.0,
                 layer: int = 0, duration: Optional[float] = None) -> None:
    """Register/replace a texture (lump name) at normalized (x, y). scale is
    a float or an (sx, sy) tuple. tint=(r, g, b) renders a solid-color
    silhouette (stencil fill), NOT a multiply tint. rotate is in degrees,
    around the anchor point. id is required; reusing it replaces the item.
    layer/duration: see the section header. Unknown texture name raises
    ValueError."""

def draw_world_bar(actor: Actor, *, id: int, offset_z: float = 0.0,
                   width: float = 0.06, height: float = 0.008,
                   track: Optional[str] = "health", frac: Optional[float] = None,
                   fg: Optional[tuple] = None, bg: tuple = (20, 20, 20),
                   max_distance: float = 2048.0, occlude: bool = True,
                   label: bool = False, label_color: Any = (255, 255, 255),
                   label_scale: float = 1.5, label_font: str = "smallfont",
                   layer: int = 0, duration: Optional[float] = None) -> None:
    """Register/replace a bar floating above actor (offset_z above its top).
    track="health" follows actor health per frame; track=None requires a
    static frac (0..1, ValueError otherwise). When track="health" and fg is
    not given, the fill color is an automatic per-frame gradient: green
    above 60% health, yellow 30-60%, red at or below 30%; passing an
    explicit fg (r, g, b) tuple overrides the gradient with a static color
    (fallback without gradient: (220, 40, 40)). bg is the inset background
    color, (20, 20, 20) by default. The bar is drawn with an opaque black
    2px border over a padded, ~85% opacity background automatically.
    occlude=True hides the bar when the player has no line of sight to the
    actor (caveat: the sight test runs from the player actor even in chase
    cam). label=True draws the actor's GetTag() name centered above the bar
    using label_font/label_scale; label_color accepts an (r, g, b) tuple or
    a font color name string. The bar fades out over the last 20% of
    max_distance and is hidden entirely beyond it or behind the camera,
    draws nothing while the actor is dead, and vanishes automatically when
    the actor is destroyed or the map unloads. width/height are normalized
    screen fractions. id is required; reusing it replaces the item.
    Unknown label_font raises ValueError."""

def draw_world_text(actor: Actor, *, id: int, text: str, offset_x: float = 0.0,
                    offset_y: float = 0.0, offset_z: float = 0.0,
                    font: str = "smallfont", color: Any = (255, 255, 255),
                    scale: Any = 0.75, alpha: float = 1.0,
                    max_distance: float = 2048.0, occlude: bool = True,
                    shadow: bool = False, outline: bool = False,
                    layer: int = 0, height: float = 0.0,
                    duration: Optional[float] = None) -> None:
    """Register/replace a text label floating above actor (offset_z above
    its top, offset_x/offset_y world-unit lateral offsets, centered).
    color accepts an (r, g, b) tuple (0-255) or a font color name string;
    scale is a float or an (sx, sy) tuple of raw pixel multipliers, while
    height is a normalized 0..1 screen-height fraction (resolution-
    independent, like draw_text's height; height > 0 overrides scale).
    Multiline text works via '\n'. Transient labels (duration set) keep
    playing at the anchor's position even after the actor dies, so
    killing-blow feedback is never lost. shadow=True
    renders a dark offset shadow; outline=True adds a black 1px outline.
    occlude=True hides the label when the player has no line of sight to
    the actor (caveat: the sight test runs from the player actor even in
    chase cam). The item is hidden beyond max_distance or behind the
    camera, draws nothing while the actor is dead, and vanishes
    automatically when the actor is destroyed or the map unloads. id is
    required; reusing it replaces the item. layer/duration: see the
    section header. Unknown font raises ValueError."""

def draw_world_texture(actor: Actor, name: str, *, id: int,
                       offset_z: float = 0.0, size: float = 24.0,
                       alpha: float = 1.0, tint: Optional[tuple] = None,
                       occlude: bool = True, max_distance: float = 2048.0,
                       layer: int = 0, duration: Optional[float] = None) -> None:
    """Register/replace a floating icon/sprite (texture lump name) over
    actor (offset_z above its top). size is in map units and is scaled by
    distance, so the icon tracks perspective. tint=(r, g, b) renders a
    solid-color silhouette (stencil fill), NOT a multiply tint.
    occlude=True hides the icon without line of sight; it is hidden beyond
    max_distance or behind the camera and vanishes with the actor or the
    map. id is required; reusing it replaces the item. layer/duration:
    see the section header. Unknown texture name raises ValueError."""

def draw_world_line(a: Any, b: Any, *, id: int,
                    color: tuple = (255, 255, 255), alpha: float = 1.0,
                    layer: int = 0, duration: Optional[float] = None) -> None:
    """Register/replace a 1px beam between two world endpoints. Each
    endpoint is an Actor handle (anchored at the actor's center, followed
    per frame) or an (x, y, z) tuple (static point). The beam is skipped
    when an endpoint is behind the camera; there is no thickness control.
    id is required; reusing it replaces the item. layer/duration: see the
    section header."""

def draw_world_ring(actor: "Actor", *, id: int, radius: float = 20.0,
                    color: tuple = (255, 255, 255), alpha: float = 1.0,
                    offset_z: float = 2.0, segments: int = 28,
                    max_distance: float = 2048.0, occlude: bool = True,
                    layer: int = 0, duration: Optional[float] = None) -> None:
    """Register/replace a flat ground ring around the actor's feet (offset_z
    above them) — a Diablo-style affix aura that follows the actor every
    frame. radius is in world units; the ring is a segments-sided polyline
    (3..128), projected per segment and distance-faded over the last 20% of
    max_distance. occlude=True hides it without line of sight; it vanishes
    with the actor or the map. id is required; reusing it replaces the
    item. layer/duration: see the section header."""

def draw_clear(id: int) -> None:
    """Remove the display-list item with the given id (no-op if absent)."""

def draw_clear_all() -> None:
    """Remove every display-list item."""


# --- Dear ImGui overlay (bd.imgui) ---------------------------------------------

# bd.imgui binds the vendored Dear ImGui 1.92.8 (docking branch) as an
# engine overlay. Every
# function is only valid inside an imgui_frame handler (they raise
# RuntimeError anywhere else) except the font registry (add_font_ttf(),
# add_font_default(), remove_font(), clear_fonts(), list_fonts(),
# set_default_font(), get_default_font()), the global scale
# (set_ui_scale()/get_ui_scale()), the persistent style accessors
# (get_style_color()/set_style_color(), get_style_var()/set_style_var(),
# style_theme()), set_master_visible()/master_visible() and
# set_nav_enabled()/nav_enabled(),
# which toggle engine-side state from any event. State flows through your
# variables: value-editing widgets return (changed, new_value) tuples. The
# overlay is gated by the py_imgui CVar; the py_imgui_demo console command
# toggles the stock demo window.

class _ImguiCol:
    """ImGuiCol_* indices for push_style_color()/get_style_color()/set_style_color()."""
    Text: int
    TextDisabled: int
    WindowBg: int
    ChildBg: int
    PopupBg: int
    Border: int
    BorderShadow: int
    FrameBg: int
    FrameBgHovered: int
    FrameBgActive: int
    TitleBg: int
    TitleBgActive: int
    TitleBgCollapsed: int
    MenuBarBg: int
    ScrollbarBg: int
    ScrollbarGrab: int
    ScrollbarGrabHovered: int
    ScrollbarGrabActive: int
    CheckMark: int
    CheckboxSelectedBg: int
    SliderGrab: int
    SliderGrabActive: int
    Button: int
    ButtonHovered: int
    ButtonActive: int
    Header: int
    HeaderHovered: int
    HeaderActive: int
    Separator: int
    SeparatorHovered: int
    SeparatorActive: int
    ResizeGrip: int
    ResizeGripHovered: int
    ResizeGripActive: int
    InputTextCursor: int
    TabHovered: int
    Tab: int
    TabSelected: int
    TabSelectedOverline: int
    TabDimmed: int
    TabDimmedSelected: int
    TabDimmedSelectedOverline: int
    DockingPreview: int
    DockingEmptyBg: int
    PlotLines: int
    PlotLinesHovered: int
    PlotHistogram: int
    PlotHistogramHovered: int
    TableHeaderBg: int
    TableBorderStrong: int
    TableBorderLight: int
    TableRowBg: int
    TableRowBgAlt: int
    TextLink: int
    TextSelectedBg: int
    TreeLines: int
    DragDropTarget: int
    DragDropTargetBg: int
    UnsavedMarker: int
    NavCursor: int
    NavWindowingHighlight: int
    NavWindowingDimBg: int
    ModalWindowDimBg: int

class _ImguiStyleVar:
    """ImGuiStyleVar_* indices for push_style_var()/get_style_var()/set_style_var()."""
    Alpha: int
    DisabledAlpha: int
    WindowPadding: int
    WindowRounding: int
    WindowBorderSize: int
    WindowMinSize: int
    WindowTitleAlign: int
    ChildRounding: int
    ChildBorderSize: int
    PopupRounding: int
    PopupBorderSize: int
    FramePadding: int
    FrameRounding: int
    FrameBorderSize: int
    ItemSpacing: int
    ItemInnerSpacing: int
    IndentSpacing: int
    CellPadding: int
    ScrollbarSize: int
    ScrollbarRounding: int
    ScrollbarPadding: int
    GrabMinSize: int
    GrabRounding: int
    ImageRounding: int
    ImageBorderSize: int
    TabRounding: int
    TabBorderSize: int
    TabMinWidthBase: int
    TabMinWidthShrink: int
    TabBarBorderSize: int
    TabBarOverlineSize: int
    TableAngledHeadersAngle: int
    TableAngledHeadersTextAlign: int
    TreeLinesSize: int
    TreeLinesRounding: int
    DragDropTargetRounding: int
    ButtonTextAlign: int
    SelectableTextAlign: int
    SeparatorSize: int
    SeparatorTextBorderSize: int
    SeparatorTextAlign: int
    SeparatorTextPadding: int
    DockingSeparatorSize: int

class _ImguiKey:
    """ImGuiKey_* values for is_key_down()/is_key_pressed()/is_key_chord_pressed()/shortcut()."""
    Tab: int
    Left: int
    Right: int
    Up: int
    Down: int
    PageUp: int
    PageDown: int
    Home: int
    End: int
    Insert: int
    Delete: int
    Backspace: int
    Space: int
    Enter: int
    KeyPadEnter: int
    Escape: int
    A: int
    B: int
    C: int
    D: int
    E: int
    F: int
    G: int
    H: int
    I: int
    J: int
    K: int
    L: int
    M: int
    N: int
    O: int
    P: int
    Q: int
    R: int
    S: int
    T: int
    U: int
    V: int
    W: int
    X: int
    Y: int
    Z: int
    F1: int
    F2: int
    F3: int
    F4: int
    F5: int
    F6: int
    F7: int
    F8: int
    F9: int
    F10: int
    F11: int
    F12: int

class _ImguiMod:
    """ImGuiMod_* modifier bits for the mods argument of is_key_chord_pressed()/shortcut()."""
    Ctrl: int
    Shift: int
    Alt: int
    Super: int

class _ImguiInputTextFlags:
    """ImGuiInputTextFlags_* bits for input_text()."""
    CharsDecimal: int
    CharsHexadecimal: int
    CharsUppercase: int
    CharsNoBlank: int
    EnterReturnsTrue: int
    ReadOnly: int
    Password: int
    AutoSelectAll: int

class _ImguiWindowFlags:
    """ImGuiWindowFlags_* bits for imgui.begin()/begin_child()."""
    NoTitleBar: int
    NoResize: int
    NoMove: int
    NoCollapse: int
    NoBackground: int
    NoScrollbar: int
    MenuBar: int
    AlwaysAutoResize: int

class _ImguiCond:
    """ImGuiCond_* values for imgui.set_next_window_*() calls (0 = Always)."""
    Always: int
    Once: int
    FirstUseEver: int
    Appearing: int

class _ImguiNamespace:
    """Dear ImGui immediate-mode UI overlay.

    All widget calls in this module submit ImGui draw commands and are only
    valid inside an imgui_frame event handler (they raise RuntimeError
    anywhere else). Not frame-gated: the font registry (add_font_ttf(),
    add_font_default(), remove_font(), clear_fonts(), list_fonts(),
    set_default_font(), get_default_font()), the global scale
    (set_ui_scale()/get_ui_scale()), the persistent style accessors
    (get_style_color()/set_style_color(), get_style_var()/set_style_var(),
    style_theme()), set_master_visible()/master_visible() and
    set_nav_enabled()/nav_enabled(); the style accessors require the
    overlay to have rendered at least one frame.
    Cost model: one CPython crossing per widget call, and when no script
    registers imgui_frame the whole feature costs a single HasCallbacks
    check per frame. The overlay is gated by the py_imgui CVar (default on)
    and the py_imgui_demo console command opens the stock demo window."""

    Col: _ImguiCol
    StyleVar: _ImguiStyleVar
    Key: _ImguiKey
    Mod: _ImguiMod
    InputTextFlags: _ImguiInputTextFlags
    WindowFlags: _ImguiWindowFlags
    Cond: _ImguiCond

    def begin(self, name: str, open: Any = None, flags: int = 0) -> Any:
        """begin(name, open=None, flags=0) -> bool | (bool, bool)
        Push a window onto the stack; every call must be paired with end().
        With open=None (the default) returns a single bool: False when the window
        is collapsed/clipped (still call end()). With open set to a bool the window
        gets a close button and the return is a (expanded, open) tuple; assign the
        second element back to your visibility state."""
    def end(self) -> None:
        """end() -> None
        Pop the current window. Always call it, even when begin() returned False."""
    def begin_child(self, id: str, size: tuple = (0, 0), border: bool = False, flags: int = 0) -> bool:
        """begin_child(id, size=(0, 0), border=False, flags=0) -> bool
        Begin a scrolling child region; returns False when clipped (still call end_child())."""
    def end_child(self) -> None:
        """end_child() -> None
        End the current child region."""
    def set_next_window_pos(self, x: float, y: float, cond: int = 0) -> None:
        """set_next_window_pos(x, y, cond=0) -> None
        Set the position of the next begin() window. cond is an imgui.Cond value (0 = always)."""
    def set_next_window_size(self, w: float, h: float, cond: int = 0) -> None:
        """set_next_window_size(w, h, cond=0) -> None
        Set the size of the next begin() window; use 0 on an axis for auto-fit."""
    def set_next_window_collapsed(self, collapsed: bool, cond: int = 0) -> None:
        """set_next_window_collapsed(collapsed, cond=0) -> None
        Force the collapsed state of the next begin() window."""
    def set_next_window_bg_alpha(self, a: float) -> None:
        """set_next_window_bg_alpha(a) -> None
        Override the background alpha of the next begin() window (0.0 - 1.0)."""
    def is_window_focused(self) -> bool:
        """is_window_focused() -> bool
        True when the current window is focused."""
    def is_window_hovered(self) -> bool:
        """is_window_hovered() -> bool
        True when the current window is hovered. For input dispatch decisions use want_capture_mouse() instead."""
    def get_window_pos(self) -> tuple:
        """get_window_pos() -> (float, float)
        Current window position in screen pixels."""
    def get_window_size(self) -> tuple:
        """get_window_size() -> (float, float)
        Current window size in screen pixels."""
    def text(self, s: str) -> None:
        """text(s) -> None
        Unformatted text (no printf interpretation)."""
    def text_colored(self, r: float, g: float, b: float, a: float, s: str) -> None:
        """text_colored(r, g, b, a, s) -> None
        Text with an explicit RGBA color (components 0.0 - 1.0)."""
    def text_disabled(self, s: str) -> None:
        """text_disabled(s) -> None
        Text drawn in the disabled color."""
    def text_wrapped(self, s: str) -> None:
        """text_wrapped(s) -> None
        Text wrapped at the window width."""
    def label_text(self, label: str, s: str) -> None:
        """label_text(label, s) -> None
        Text with a right-aligned label, like the value+label widgets."""
    def bullet_text(self, s: str) -> None:
        """bullet_text(s) -> None
        Text prefixed with a bullet."""
    def button(self, label: str, w: float = 0, h: float = 0) -> bool:
        """button(label, w=0, h=0) -> bool
        Standard button; True on the frame it is clicked."""
    def small_button(self, label: str) -> bool:
        """small_button(label) -> bool
        Compact button for embedding in text lines."""
    def checkbox(self, label: str, checked: bool) -> tuple:
        """checkbox(label, checked) -> (bool, bool)
        Returns (changed, new_value)."""
    def radio_button(self, label: str, active: bool) -> bool:
        """radio_button(label, active) -> bool
        True when pressed; pass value == button_value as active and assign on True."""
    def slider_int(self, label: str, value: int, min: int, max: int) -> tuple:
        """slider_int(label, value, min, max) -> (bool, int)
        Returns (changed, value)."""
    def slider_float(self, label: str, value: float, min: float, max: float, format: str = "%.3f") -> tuple:
        """slider_float(label, value, min, max, format='%.3f') -> (bool, float)
        Returns (changed, value)."""
    def drag_int(self, label: str, value: int, speed: float = 1.0, min: int = 0, max: int = 0) -> tuple:
        """drag_int(label, value, speed=1.0, min=0, max=0) -> (bool, int)
        Drag widget; min >= max means unbounded. Returns (changed, value)."""
    def drag_float(self, label: str, value: float, speed: float = 1.0, min: float = 0.0, max: float = 0.0, format: str = "%.3f") -> tuple:
        """drag_float(label, value, speed=1.0, min=0.0, max=0.0, format='%.3f') -> (bool, float)
        min >= max means unbounded. Returns (changed, value)."""
    def input_text(self, label: str, text: str, max_length: int = 256, flags: int = 0) -> tuple:
        """input_text(label, text, max_length=256, flags=0) -> (bool, str)
        Single-line edit box backed by a fixed buffer of max_length + 1 bytes.
        The returned text is the current buffer contents every frame; what 'changed'
        means depends on flags (with no flags it is True on every edit; pass
        ImGuiInputTextFlags_EnterReturnsTrue (value 32) to report only on Enter)."""
    def input_int(self, label: str, value: int, step: int = 1, step_fast: int = 100) -> tuple:
        """input_int(label, value, step=1, step_fast=100) -> (bool, int)
        Integer input with +/- steppers. Returns (changed, value)."""
    def input_float(self, label: str, value: float, step: float = 0.0, step_fast: float = 0.0, format: str = "%.3f") -> tuple:
        """input_float(label, value, step=0.0, step_fast=0.0, format='%.3f') -> (bool, float)
        Float input; step 0 hides the steppers. Returns (changed, value)."""
    def combo(self, label: str, current_index: int, items: Any) -> tuple:
        """combo(label, current_index, items) -> (bool, int)
        Drop-down over a sequence of str. Returns (changed, new_index)."""
    def list_box(self, label: str, current_index: int, items: Any, height_items: int = -1) -> tuple:
        """list_box(label, current_index, items, height_items=-1) -> (bool, int)
        Framed scrolling list over a sequence of str. Returns (changed, new_index)."""
    def selectable(self, label: str, selected: bool, flags: int = 0) -> bool:
        """selectable(label, selected, flags=0) -> bool
        True when pressed; toggle your own selection state on True."""
    def tree_node(self, label: str) -> bool:
        """tree_node(label) -> bool
        True when open; emit children and then call tree_pop() only in that case."""
    def tree_pop(self) -> None:
        """tree_pop() -> None
        Close a tree_node() that returned True."""
    def collapsing_header(self, label: str, flags: int = 0) -> bool:
        """collapsing_header(label, flags=0) -> bool
        True when the header is open; no tree_pop() needed."""
    def separator(self) -> None:
        """separator() -> None
        Horizontal separator line (vertical inside menu bars)."""
    def same_line(self, offset: float = 0.0, spacing: float = -1.0) -> None:
        """same_line(offset=0.0, spacing=-1.0) -> None
        Keep the next widget on the current line."""
    def spacing(self) -> None:
        """spacing() -> None
        Vertical spacing."""
    def newline(self) -> None:
        """newline() -> None
        Undo a same_line() / force a line break."""
    def indent(self, width: float = 0.0) -> None:
        """indent(width=0.0) -> None
        Move content right (0 = style default spacing)."""
    def unindent(self, width: float = 0.0) -> None:
        """unindent(width=0.0) -> None
        Move content back left."""
    def align_text_to_frame_padding(self) -> None:
        """align_text_to_frame_padding() -> None
        Align text baseline to framed widgets on the same line."""
    def begin_table(self, id: str, columns: int, flags: int = 0) -> bool:
        """begin_table(id, columns, flags=0) -> bool
        Begin a table; call end_table() only when it returns True."""
    def end_table(self) -> None:
        """end_table() -> None
        End a begin_table() that returned True."""
    def table_next_row(self, flags: int = 0, min_height: float = 0.0) -> None:
        """table_next_row(flags=0, min_height=0.0) -> None
        Advance into the first cell of a new row."""
    def table_next_column(self) -> bool:
        """table_next_column() -> bool
        Advance into the next column; False when the column is clipped."""
    def table_setup_column(self, label: str, flags: int = 0, init_width: float = 0.0) -> None:
        """table_setup_column(label, flags=0, init_width=0.0) -> None
        Declare one column before the first row."""
    def table_headers_row(self) -> None:
        """table_headers_row() -> None
        Emit a header row from the declared columns."""
    def progress_bar(self, fraction: float, w: float = -1, h: float = 0, overlay: Optional[str] = None) -> None:
        """progress_bar(fraction, w=-1, h=0, overlay=None) -> None
        Horizontal progress bar; fraction is 0.0 - 1.0, w < 0 fills the row."""
    def color_edit3(self, label: str, r: float, g: float, b: float) -> tuple:
        """color_edit3(label, r, g, b) -> (bool, float, float, float)
        RGB color editor. Returns (changed, r, g, b)."""
    def color_edit4(self, label: str, r: float, g: float, b: float, a: float) -> tuple:
        """color_edit4(label, r, g, b, a) -> (bool, float, float, float, float)
        RGBA color editor. Returns (changed, r, g, b, a)."""
    def plot_lines(self, label: str, values: Any, overlay: Optional[str] = None, scale_min: float = 3.4028234663852886e+38, scale_max: float = 3.4028234663852886e+38, w: float = 0, h: float = 0) -> None:
        """plot_lines(label, values, overlay=None, scale_min=FLT_MAX, scale_max=FLT_MAX, w=0, h=0) -> None
        Line plot over a sequence of floats. Leave scale_min/scale_max at their defaults
        for auto-scaling (the FLT_MAX sentinel is ImGui's 'compute from data' semantic)."""
    def image(self, texture: Any, w: float = 0, h: float = 0, uv0: tuple = (0, 0), uv1: tuple = (1, 1), tint: tuple = (1, 1, 1, 1), border: tuple = (0, 0, 0, 0)) -> None:
        """image(texture, w=0, h=0, uv0=(0, 0), uv1=(1, 1), tint=(1, 1, 1, 1), border=(0, 0, 0, 0)) -> None
        Draw a game texture inside the current window. texture is either a lump name
        (MiscPatch lookup first, sprite-namespace fallback for PLAYA1 style names) or an
        Actor handle (its current sprite frame texture, rotation 0). w/h 0 means the
        texture's natural display size; uv0/uv1 select a sub-rectangle; tint is an RGBA
        multiplier; border with alpha > 0 draws a 1px border of that color. Unknown
        texture raises ValueError."""
    def image_size(self, texture: Any) -> tuple:
        """image_size(texture) -> (float, float)
        Natural display size (w, h) of a texture accepted by image(). Callable from any event."""
    def dock_space_over_viewport(self, flags: int = 0) -> int:
        """dock_space_over_viewport(flags=0) -> int
        Create a dockspace covering the whole viewport and return its dockspace id.
        flags are ImGuiDockNodeFlags_* (2 = PassthruCentralNode). Requires the docking
        branch (always vendored); multi-viewport is not supported."""
    def dock_space(self, id: int, w: float = 0, h: float = 0, flags: int = 0) -> int:
        """dock_space(id, w=0, h=0, flags=0) -> int
        Submit a dockspace node with the given id inside the current window; returns the
        node id. Windows become dockable while any dockspace exists."""
    def set_next_window_dock_id(self, id: int, cond: int = 0) -> None:
        """set_next_window_dock_id(id, cond=0) -> None
        Dock the next begin() window into the dockspace node with the given id (a value
        returned by dock_space_over_viewport()/dock_space()). cond is an imgui.Cond value."""
    def begin_main_menu_bar(self) -> bool:
        """begin_main_menu_bar() -> bool
        Begin the screen-top menu bar; call end_main_menu_bar() only when True."""
    def end_main_menu_bar(self) -> None:
        """end_main_menu_bar() -> None
        End the main menu bar."""
    def begin_menu(self, label: str) -> bool:
        """begin_menu(label) -> bool
        Begin a sub-menu; call end_menu() only when True."""
    def end_menu(self) -> None:
        """end_menu() -> None
        End a begin_menu() that returned True."""
    def menu_item(self, label: str, shortcut: Optional[str] = None, selected: bool = False, enabled: bool = True) -> bool:
        """menu_item(label, shortcut=None, selected=False, enabled=True) -> bool
        True when the item is activated."""
    def begin_tooltip(self) -> bool:
        """begin_tooltip() -> bool
        Begin a tooltip window; call end_tooltip() only when True."""
    def end_tooltip(self) -> None:
        """end_tooltip() -> None
        End a begin_tooltip() that returned True."""
    def set_tooltip(self, s: str) -> None:
        """set_tooltip(s) -> None
        Text-only tooltip, typically after is_item_hovered()."""
    def set_keyboard_focus_here(self, offset: float = 0.0) -> None:
        """set_keyboard_focus_here(offset=0.0) -> None
        Focus keyboard on the next widget (or on a sub component with a positive offset; -1 addresses the previous widget)."""
    def is_item_hovered(self) -> bool:
        """is_item_hovered() -> bool
        True when the last submitted item is hovered."""
    def is_item_clicked(self, button: int = 0) -> bool:
        """is_item_clicked(button=0) -> bool
        True when the last item was clicked with the given mouse button."""
    def is_item_active(self) -> bool:
        """is_item_active() -> bool
        True while the last item is being interacted with."""
    def is_any_item_active(self) -> bool:
        """is_any_item_active() -> bool
        True while any item is active."""
    def push_style_color(self, idx: int, r: float, g: float, b: float, a: float) -> None:
        """push_style_color(idx, r, g, b, a) -> None
        Override a style color (idx is an imgui.Col value). Pair with pop_style_color()."""
    def pop_style_color(self, count: int = 1) -> None:
        """pop_style_color(count=1) -> None
        Undo style color pushes."""
    def push_style_var(self, idx: int, x: float, y: Optional[float] = None) -> None:
        """push_style_var(idx, x, y=None) -> None
        Override a style variable; y=None pushes a scalar var, a number pushes an ImVec2 var. Pair with pop_style_var()."""
    def pop_style_var(self, count: int = 1) -> None:
        """pop_style_var(count=1) -> None
        Undo style var pushes."""
    def get_font_size(self) -> float:
        """get_font_size() -> float
        Current font height in pixels after global scaling."""
    def add_font_ttf(self, name: str, data: bytes, size: float) -> bool:
        """add_font_ttf(name, data, size) -> bool
        Register a TTF/OTF font from a bytes object (e.g. bd.read_bytes()) under name, requested
        pixel size 4..96. The atlas rebuilds outside the frame; a mutation during imgui_frame is
        applied after the frame renders, so the font becomes usable on the next frame. Duplicate
        or empty names are rejected (returns False). Callable from any event."""
    def add_font_default(self, name: str, size: float, bitmap: bool = False) -> bool:
        """add_font_default(name, size, bitmap=False) -> bool
        Register a font from ImGui's embedded default data; bitmap=True selects the classic
        pixel font, False the scalable vector font. Same rebuild and naming rules as add_font_ttf().
        Callable from any event."""
    def remove_font(self, name: str) -> bool:
        """remove_font(name) -> bool
        Remove a script-registered font and rebuild the atlas. The built-in 'Default' font cannot
        be removed. Callable from any event."""
    def clear_fonts(self) -> None:
        """clear_fonts() -> None
        Remove all script-registered fonts, keeping 'Default'. Callable from any event."""
    def list_fonts(self) -> list:
        """list_fonts() -> [(name, size, bitmap, builtin), ...]
        Snapshot of the font registry. Callable from any event."""
    def set_default_font(self, name: str) -> bool:
        """set_default_font(name) -> bool
        Select the font every imgui_frame starts on. Returns False for an unknown name.
        Callable from any event."""
    def get_default_font(self) -> Optional[str]:
        """get_default_font() -> str | None
        Name of the font every imgui_frame starts on. Callable from any event."""
    def push_font(self, name: str) -> None:
        """push_font(name) -> None
        Switch to a registered font; pair with pop_font(). Raises ValueError for an unknown name."""
    def pop_font(self) -> None:
        """pop_font() -> None
        Undo push_font()."""
    def set_ui_scale(self, factor: float) -> bool:
        """set_ui_scale(factor) -> bool
        Global UI scale, clamped to [0.5, 4.0]. Sets style.FontScaleMain and rescales all
        spacing/padding via ScaleAllSizes() by the ratio between the new and the previous factor,
        so repeated calls compose. Callable from any event."""
    def get_ui_scale(self) -> float:
        """get_ui_scale() -> float
        Current global UI scale factor. Callable from any event."""
    def set_window_font_scale(self, scale: float) -> None:
        """set_window_font_scale(scale) -> None
        Per-window font scale for the current window; prefer set_ui_scale() for global scaling."""
    def get_style_color(self, idx: int) -> tuple:
        """get_style_color(idx) -> (r, g, b, a)
        Read a style color (idx is an imgui.Col value). Callable from any event."""
    def set_style_color(self, idx: int, r: float, g: float, b: float, a: float) -> None:
        """set_style_color(idx, r, g, b, a) -> None
        Write a style color (idx is an imgui.Col value). Callable from any event."""
    def get_style_var(self, idx: int) -> Any:
        """get_style_var(idx) -> float | (x, y)
        Read a style variable (idx is an imgui.StyleVar value); ImVec2-backed vars return a tuple.
        Callable from any event."""
    def set_style_var(self, idx: int, x: float, y: Optional[float] = None) -> None:
        """set_style_var(idx, x, y=None) -> None
        Write a style variable; ImVec2-backed vars require y. Callable from any event."""
    def style_theme(self, name: str) -> None:
        """style_theme(name) -> None
        Reset all colors to a stock theme: 'dark', 'classic' or 'light'. The UI scale factors are
        preserved. Callable from any event."""
    def is_key_down(self, key: int) -> bool:
        """is_key_down(key) -> bool
        True while the key is held (key is an imgui.Key value)."""
    def is_key_pressed(self, key: int, repeat: bool = False) -> bool:
        """is_key_pressed(key, repeat=False) -> bool
        True on the frame the key went down; repeat=True also reports held-key repeats."""
    def is_key_chord_pressed(self, key: int, mods: int = 0) -> bool:
        """is_key_chord_pressed(key, mods=0) -> bool
        True on the frame the mods+key chord went down (mods is an OR of imgui.Mod values).
        Does no focus routing; prefer shortcut()."""
    def shortcut(self, key: int, mods: int = 0) -> bool:
        """shortcut(key, mods=0) -> bool
        Like is_key_chord_pressed() but with ImGui focus routing, so the deepest focused window wins."""
    def set_item_default_focus(self) -> None:
        """set_item_default_focus() -> None
        Make the last submitted item the default focused item of a newly appearing window."""
    def open_popup(self, str_id: str) -> None:
        """open_popup(str_id) -> None
        Mark a popup as open; call on an event (e.g. a button press), not every frame."""
    def begin_popup(self, str_id: str) -> bool:
        """begin_popup(str_id) -> bool
        True when the popup is open; call end_popup() only in that case."""
    def end_popup(self) -> None:
        """end_popup() -> None
        End a begin_popup() that returned True."""
    def close_current_popup(self) -> None:
        """close_current_popup() -> None
        Close the popup currently open in this scope."""
    def is_popup_open(self, str_id: str) -> bool:
        """is_popup_open(str_id) -> bool
        True when the popup with this id is open."""
    def calc_text_size(self, s: str) -> tuple:
        """calc_text_size(s) -> (w, h)
        Size of a text string in the current font, in pixels."""
    def set_cursor_pos(self, x: float, y: float) -> None:
        """set_cursor_pos(x, y) -> None
        Set the cursor position inside the current window (window-local coordinates)."""
    def get_cursor_pos(self) -> tuple:
        """get_cursor_pos() -> (x, y)
        Cursor position in window-local coordinates."""
    def get_cursor_screen_pos(self) -> tuple:
        """get_cursor_screen_pos() -> (x, y)
        Cursor position in absolute screen coordinates."""
    def show_demo_window(self, open: bool) -> bool:
        """show_demo_window(open) -> bool
        Show the ImGui demo window while open is True; returns the still-open state (the window's close button flips it)."""
    def show_metrics_window(self, open: bool) -> bool:
        """show_metrics_window(open) -> bool
        Show the ImGui metrics/debugger window; returns the still-open state."""
    def want_capture_mouse(self) -> bool:
        """want_capture_mouse() -> bool
        True when ImGui is consuming the mouse this frame."""
    def want_capture_keyboard(self) -> bool:
        """want_capture_keyboard() -> bool
        True when ImGui is consuming the keyboard this frame."""
    def set_master_visible(self, visible: bool) -> None:
        """set_master_visible(visible) -> None
        Toggle the whole overlay (the py_imgui CVar). Callable from any event, not just imgui_frame."""
    def master_visible(self) -> bool:
        """master_visible() -> bool
        Read the py_imgui master switch. Callable from any event."""
    def set_nav_enabled(self, enabled: bool) -> None:
        """set_nav_enabled(enabled) -> None
        Enable or disable ImGui keyboard navigation (ImGuiConfigFlags_NavEnableKeyboard, on by default). Callable from any event, not just imgui_frame."""
    def nav_enabled(self) -> bool:
        """nav_enabled() -> bool
        True when ImGui keyboard navigation is enabled. Callable from any event."""

imgui: _ImguiNamespace


# --- ui toolkit (embedded pure-python) ----------------------------------------

# bd.ui is a small pure-Python toolkit embedded in the biaseddoom module,
# built on top of the canvas display list. Panels own their canvas ids
# (allocated from base 900000, 100 ids reserved per panel; toast/announce
# ids live at 999000+) and all live panels automatically re-register their
# items on map_load, so toolkit UI survives level transitions with no
# script-side cleanup. All toolkit colors are (r, g, b) or (r, g, b, a)
# tuples with components in 0-255; unlike draw_text, named font color
# strings are NOT accepted by toast()/announce() or any theme field.

class UiTheme:
    """Mutable color theme for bd.ui. Colors are (r, g, b) or (r, g, b, a);
    the optional alpha is a 0-255 component. Assign new tuples to restyle
    every panel created afterwards."""
    bg: tuple      # panel backdrop, gradient top: (10, 10, 26, 200)
    bg2: tuple     # panel backdrop, gradient bottom: (24, 14, 40, 200)
    border: tuple  # panel frame: (255, 140, 40)
    text: tuple    # row labels, toast default: (235, 230, 220)
    dim: tuple     # bar frames: (150, 145, 135)
    accent: tuple  # default row value color: (255, 180, 60)
    good: tuple    # bar fill above 60%: (90, 220, 110)
    warn: tuple    # bar fill 30-60%: (240, 210, 80)
    bad: tuple     # bar fill at/below 30%: (235, 70, 60)
    gold: tuple    # panel titles, flash highlight, announce default: (255, 200, 80)

class UiPanel:
    """Themed HUD panel: gradient backdrop, framed, auto-height from rows.
    Layers: backdrop 0, content/bars 1, frame 2. All row text is smallfont,
    scale 1.25, outlined."""
    id: int
    x: float
    y: float
    w: float
    title: Any
    anchor: str
    def row(self, label: str, value: str = "", *, value_color: Any = None,
            flash: bool = False) -> "UiPanel":
        """Add a label/value row, or update it in place if label exists.
        value_color is an (r, g, b) tuple (theme.accent when None);
        flash=True briefly highlights the value in gold. Returns self for
        chaining."""
    def bar(self, label: str, frac: float, *, fg: Any = None) -> "UiPanel":
        """Add/update a bordered gradient bar row; fg (an (r, g, b) tuple)
        overrides the good->warn->bad color picked from frac. Returns self
        for chaining."""
    def hide(self) -> None:
        """Keep the panel registered but re-register every item at alpha 0."""
    def show(self) -> None:
        """Restore a hidden panel at full opacity."""
    def close(self) -> None:
        """Remove every display-list item owned by this panel."""

class _UiNamespace:
    """bd.ui - embedded UI toolkit: themed panels, toasts and announcements."""
    theme: UiTheme
    def panel(self, *, x: float, y: float, w: float, title: Any = None,
              anchor: str = "tl", id: Any = None) -> UiPanel:
        """Create a panel anchored to a screen corner ('tl', 'tr', 'bl' or
        'br'). x, y refer to that corner; w is the normalized width; height
        grows with the rows. id optionally overrides the allocated id base."""
    def toast(self, text: str, *, color: Any = None, duration: float = 1.5,
              y: float = 0.72) -> None:
        """Small centered outlined toast; auto-expires after duration
        seconds. color is an (r, g, b) tuple (theme.text when None) -
        named color strings are NOT accepted."""
    def announce(self, title: str, *, subtitle: Any = None, color: Any = None,
                 duration: float = 2.5) -> None:
        """Big centered announcement: outlined bigfont title, smallfont
        subtitle, a screen-fade accent and a UI sound. color is an
        (r, g, b) tuple (theme.gold when None) - named color strings are
        NOT accepted."""

ui: _UiNamespace


# --- actor class registry ---------------------------------------------------

class _ActorsRegistry:
    """Actor class registry: named constants, discovery, random spawns.

    Attribute access maps UPPER_SNAKE constants to engine class names:
    ``bd.actors.DOOM_IMP`` -> ``"DoomImp"``. The registry stays callable,
    so ``bd.actors(...)`` still queries live actor snapshots.
    """

    MONSTER: int
    PROJECTILE: int
    WEAPON: int
    INVENTORY: int
    PLAYER: int

    def __call__(self, class_name: Optional[str] = None, tid: int = 0, limit: int = 1024) -> list:
        """Return actor snapshots, optionally filtered by class or TID."""

    def names(self) -> list:
        """All registered actor class names, sorted (e.g. "DoomImp")."""

    def constants(self) -> list:
        """All constant names, sorted (e.g. "DOOM_IMP")."""

    def resolve(self, name: str) -> Optional[str]:
        """Accept a CONST name or an engine class name; return the class name or None."""

    def children_of(self, parent: str) -> list:
        """Sorted class names descending from parent (inclusive);
        parent may be a CONST name or an engine class name."""

    def monsters(self) -> list:
        """Class names of shootable, kill-counted actors."""

    def projectiles(self) -> list:
        """Class names of missile actors."""

    def weapons(self) -> list:
        """Class names descending from Weapon."""

    def items(self) -> list:
        """Class names descending from Inventory."""

    def players(self) -> list:
        """Class names descending from PlayerPawn."""

    def random(self, kind: Optional[str] = None) -> str:
        """Return a random actor class name. kind may be None (any actor),
        a category ("monsters", "projectiles", "weapons", "items",
        "players"), or a class/CONST name to pick among its descendants."""

    def spawn_random(self, x: float, y: float, z: float, kind: str = "monsters", **kwargs: Any) -> dict:
        """Spawn a random actor of the given category at (x, y, z)."""

    def __getattr__(self, name: str) -> str:
        """Resolve an UPPER_SNAKE constant to its engine class name."""

    # -- generated actor class constants (built-in classes) --
    # @@GENERATED ACTOR CONSTANTS BEGIN@@
    ACOLYTE: str  # Acolyte
    ACOLYTE_BLUE: str  # AcolyteBlue
    ACOLYTE_D_GREEN: str  # AcolyteDGreen
    ACOLYTE_GOLD: str  # AcolyteGold
    ACOLYTE_GRAY: str  # AcolyteGray
    ACOLYTE_L_GREEN: str  # AcolyteLGreen
    ACOLYTE_RED: str  # AcolyteRed
    ACOLYTE_RUST: str  # AcolyteRust
    ACOLYTE_SHADOW: str  # AcolyteShadow
    ACOLYTE_TAN: str  # AcolyteTan
    ACOLYTE_TO_BE: str  # AcolyteToBe
    ACTIVATED_TIME_BOMB: str  # ActivatedTimeBomb
    ACTOR: str  # Actor
    ACTOR_MOVER: str  # ActorMover
    AIMING_CAMERA: str  # AimingCamera
    ALIEN_ASP_CLIMBER: str  # AlienAspClimber
    ALIEN_BUBBLE_COLUMN: str  # AlienBubbleColumn
    ALIEN_CEILING_BUBBLE: str  # AlienCeilingBubble
    ALIEN_CHUNK_LARGE: str  # AlienChunkLarge
    ALIEN_CHUNK_SMALL: str  # AlienChunkSmall
    ALIEN_FLOOR_BUBBLE: str  # AlienFloorBubble
    ALIEN_SPECTRE1: str  # AlienSpectre1
    ALIEN_SPECTRE2: str  # AlienSpectre2
    ALIEN_SPECTRE3: str  # AlienSpectre3
    ALIEN_SPECTRE4: str  # AlienSpectre4
    ALIEN_SPECTRE5: str  # AlienSpectre5
    ALIEN_SPIDER_LIGHT: str  # AlienSpiderLight
    ALLMAP: str  # Allmap
    AMBIENT_SOUND: str  # AmbientSound
    AMBIENT_SOUND_NO_GRAVITY: str  # AmbientSoundNoGravity
    AMMO: str  # Ammo
    AMMO_FILLER: str  # AmmoFiller
    AMMO_FILLUP: str  # AmmoFillup
    AMMO_SATCHEL: str  # AmmoSatchel
    AMULET_OF_WARDING: str  # AmuletOfWarding
    ANVIL: str  # Anvil
    ARACHNOTRON: str  # Arachnotron
    ARACHNOTRON_PLASMA: str  # ArachnotronPlasma
    ARCHVILE: str  # Archvile
    ARCHVILE_FIRE: str  # ArchvileFire
    ARMOR: str  # Armor
    ARMORED_FLEMOIDUS_BIPEDICUS: str  # ArmoredFlemoidusBipedicus
    ARMORER: str  # Armorer
    ARMOR_BONUS: str  # ArmorBonus
    ARROW: str  # Arrow
    ARTI_BLAST_RADIUS: str  # ArtiBlastRadius
    ARTI_BOOST_ARMOR: str  # ArtiBoostArmor
    ARTI_BOOST_MANA: str  # ArtiBoostMana
    ARTI_DARK_SERVANT: str  # ArtiDarkServant
    ARTI_EGG: str  # ArtiEgg
    ARTI_FLY: str  # ArtiFly
    ARTI_HEALING_RADIUS: str  # ArtiHealingRadius
    ARTI_HEALTH: str  # ArtiHealth
    ARTI_INVISIBILITY: str  # ArtiInvisibility
    ARTI_INVULNERABILITY: str  # ArtiInvulnerability
    ARTI_INVULNERABILITY2: str  # ArtiInvulnerability2
    ARTI_POISON_BAG: str  # ArtiPoisonBag
    ARTI_POISON_BAG1: str  # ArtiPoisonBag1
    ARTI_POISON_BAG2: str  # ArtiPoisonBag2
    ARTI_POISON_BAG3: str  # ArtiPoisonBag3
    ARTI_POISON_BAG_GIVER: str  # ArtiPoisonBagGiver
    ARTI_POISON_BAG_SHOOTER: str  # ArtiPoisonBagShooter
    ARTI_PORK: str  # ArtiPork
    ARTI_SPEED_BOOTS: str  # ArtiSpeedBoots
    ARTI_SUPER_HEALTH: str  # ArtiSuperHealth
    ARTI_TELEPORT: str  # ArtiTeleport
    ARTI_TELEPORT_OTHER: str  # ArtiTeleportOther
    ARTI_TIME_BOMB: str  # ArtiTimeBomb
    ARTI_TOME_OF_POWER: str  # ArtiTomeOfPower
    ARTI_TORCH: str  # ArtiTorch
    ASSAULT_GUN: str  # AssaultGun
    ASSAULT_GUN_STANDING: str  # AssaultGunStanding
    AXE_BLOOD: str  # AxeBlood
    AXE_PUFF: str  # AxePuff
    AXE_PUFF_GLOW: str  # AxePuffGlow
    BACKPACK: str  # Backpack
    BACKPACK_ITEM: str  # BackpackItem
    BAG_OF_HOLDING: str  # BagOfHolding
    BANG4_CLOUD: str  # Bang4Cloud
    BARON_BALL: str  # BaronBall
    BARON_OF_HELL: str  # BaronOfHell
    BARREL: str  # Barrel
    BARRICADE_COLUMN: str  # BarricadeColumn
    BAR_KEEP: str  # BarKeep
    BASE_KEY: str  # BaseKey
    BASIC_ARMOR: str  # BasicArmor
    BASIC_ARMOR_BONUS: str  # BasicArmorBonus
    BASIC_ARMOR_PICKUP: str  # BasicArmorPickup
    BAT: str  # Bat
    BAT_SPAWNER: str  # BatSpawner
    BEAK: str  # Beak
    BEAK_POWERED: str  # BeakPowered
    BEAK_PUFF: str  # BeakPuff
    BEAST: str  # Beast
    BEAST_BALL: str  # BeastBall
    BEGGAR: str  # Beggar
    BEGGAR1: str  # Beggar1
    BEGGAR2: str  # Beggar2
    BEGGAR3: str  # Beggar3
    BEGGAR4: str  # Beggar4
    BEGGAR5: str  # Beggar5
    BELDINS_RING: str  # BeldinsRing
    BERSERK: str  # Berserk
    BETA_SKULL: str  # BetaSkull
    BFG9000: str  # BFG9000
    BFG_BALL: str  # BFGBall
    BFG_EXTRA: str  # BFGExtra
    BIG_TREE: str  # BigTree
    BIG_TREE2: str  # BigTree2
    BISHOP: str  # Bishop
    BISHOP_BLUR: str  # BishopBlur
    BISHOP_FX: str  # BishopFX
    BISHOP_MISSILE: str  # BishopMissile
    BISHOP_PAIN_BLUR: str  # BishopPainBlur
    BISHOP_PUFF: str  # BishopPuff
    BLACK_PARTICLE_FOUNTAIN: str  # BlackParticleFountain
    BLASTEFFECT: str  # blasteffect
    BLASTER: str  # Blaster
    BLASTER_AMMO: str  # BlasterAmmo
    BLASTER_FX1: str  # BlasterFX1
    BLASTER_HEFTY: str  # BlasterHefty
    BLASTER_POWERED: str  # BlasterPowered
    BLASTER_PUFF: str  # BlasterPuff
    BLASTER_SMOKE: str  # BlasterSmoke
    BLOOD: str  # Blood
    BLOODSCOURGE_DROP: str  # BloodscourgeDrop
    BLOODY_FIGHTER_SKULL: str  # BloodyFighterSkull
    BLOODY_SKULL: str  # BloodySkull
    BLOODY_TWITCH: str  # BloodyTwitch
    BLOOD_POOL: str  # BloodPool
    BLOOD_SPLASH: str  # BloodSplash
    BLOOD_SPLASH_BASE: str  # BloodSplashBase
    BLOOD_SPLATTER: str  # BloodSplatter
    BLUE_ARMOR: str  # BlueArmor
    BLUE_ARMOR_FOR_MEGASPHERE: str  # BlueArmorForMegasphere
    BLUE_CARD: str  # BlueCard
    BLUE_CRYSTAL_KEY: str  # BlueCrystalKey
    BLUE_PARTICLE_FOUNTAIN: str  # BlueParticleFountain
    BLUE_SKULL: str  # BlueSkull
    BLUE_TORCH: str  # BlueTorch
    BLUR_SPHERE: str  # BlurSphere
    BOOTSPOON: str  # Bootspoon
    BOSS_BRAIN: str  # BossBrain
    BOSS_EYE: str  # BossEye
    BOSS_SPOT: str  # BossSpot
    BOSS_TARGET: str  # BossTarget
    BOWL_OF_FRUIT: str  # BowlOfFruit
    BOWL_OF_VEGETABLES: str  # BowlOfVegetables
    BOX_OF_BULLETS: str  # BoxOfBullets
    BRAIN_STEM: str  # BrainStem
    BRASS_KEY: str  # BrassKey
    BRASS_TORCH: str  # BrassTorch
    BRIDGE: str  # Bridge
    BRIDGE_BALL: str  # BridgeBall
    BROKEN_POWER_COUPLING: str  # BrokenPowerCoupling
    BROWN_PILLAR: str  # BrownPillar
    BULLET_PUFF: str  # BulletPuff
    BURNING_BARREL: str  # BurningBarrel
    BURNING_BOWL: str  # BurningBowl
    BURNING_BRAZIER: str  # BurningBrazier
    CACODEMON: str  # Cacodemon
    CACODEMON_BALL: str  # CacodemonBall
    CAGE_LIGHT: str  # CageLight
    CAJUN_BODY_NODE: str  # CajunBodyNode
    CAJUN_TRACE: str  # CajunTrace
    CANDELABRA: str  # Candelabra
    CANDLE: str  # Candle
    CANDLESTICK: str  # Candlestick
    CATACOMB_KEY: str  # CatacombKey
    CAVE_PILLAR_BOTTOM: str  # CavePillarBottom
    CAVE_PILLAR_TOP: str  # CavePillarTop
    CEILING_CHAIN: str  # CeilingChain
    CEILING_TURRET: str  # CeilingTurret
    CELL: str  # Cell
    CELL_PACK: str  # CellPack
    CENTAUR: str  # Centaur
    CENTAUR_FX: str  # CentaurFX
    CENTAUR_LEADER: str  # CentaurLeader
    CENTAUR_MASH: str  # CentaurMash
    CENTAUR_SHIELD: str  # CentaurShield
    CENTAUR_SWORD: str  # CentaurSword
    CHAINGUN: str  # Chaingun
    CHAINGUN_GUY: str  # ChaingunGuy
    CHAINSAW: str  # Chainsaw
    CHANDELIER: str  # Chandelier
    CHAPEL_KEY: str  # ChapelKey
    CHEX_APPLE_TREE: str  # ChexAppleTree
    CHEX_ARMOR: str  # ChexArmor
    CHEX_BANANA_TREE: str  # ChexBananaTree
    CHEX_BLUE_CARD: str  # ChexBlueCard
    CHEX_CAVERN_COLUMN: str  # ChexCavernColumn
    CHEX_CAVERN_STALAGMITE: str  # ChexCavernStalagmite
    CHEX_CHEMICAL_BURNER: str  # ChexChemicalBurner
    CHEX_CHEMICAL_FLASK: str  # ChexChemicalFlask
    CHEX_CIVILIAN1: str  # ChexCivilian1
    CHEX_CIVILIAN2: str  # ChexCivilian2
    CHEX_CIVILIAN3: str  # ChexCivilian3
    CHEX_FLAG_ON_POLE: str  # ChexFlagOnPole
    CHEX_GAS_TANK: str  # ChexGasTank
    CHEX_LANDING_LIGHT: str  # ChexLandingLight
    CHEX_LIGHT_COLUMN: str  # ChexLightColumn
    CHEX_MINE_CART: str  # ChexMineCart
    CHEX_ORANGE_TREE: str  # ChexOrangeTree
    CHEX_PLAYER: str  # ChexPlayer
    CHEX_RED_CARD: str  # ChexRedCard
    CHEX_SLIME_FOUNTAIN: str  # ChexSlimeFountain
    CHEX_SOUL: str  # ChexSoul
    CHEX_SPACESHIP: str  # ChexSpaceship
    CHEX_SUBMERGED_PLANT: str  # ChexSubmergedPlant
    CHEX_TALL_FLOWER: str  # ChexTallFlower
    CHEX_TALL_FLOWER2: str  # ChexTallFlower2
    CHEX_YELLOW_CARD: str  # ChexYellowCard
    CHICKEN: str  # Chicken
    CHICKEN_PLAYER: str  # ChickenPlayer
    CHIMNEY_STACK: str  # ChimneyStack
    CIRCLE_FLAME: str  # CircleFlame
    CLERIC_BOSS: str  # ClericBoss
    CLERIC_PLAYER: str  # ClericPlayer
    CLERIC_WEAPON: str  # ClericWeapon
    CLERIC_WEAPON_PIECE: str  # ClericWeaponPiece
    CLINK: str  # Clink
    CLIP: str  # Clip
    CLIP_BOX: str  # ClipBox
    CLIP_OF_BULLETS: str  # ClipOfBullets
    CLOSE_DOOR222: str  # CloseDoor222
    COIN: str  # Coin
    COLON_GIBS: str  # ColonGibs
    COLOR_SETTER: str  # ColorSetter
    COLUMN: str  # Column
    COMMANDER_KEEN: str  # CommanderKeen
    COMMUNICATOR: str  # Communicator
    COMPUTER: str  # Computer
    COMPUTER_AREA_MAP: str  # ComputerAreaMap
    CORE_KEY: str  # CoreKey
    CORPSE_BIT: str  # CorpseBit
    CORPSE_BLOOD_DRIP: str  # CorpseBloodDrip
    CRATE_OF_MISSILES: str  # CrateOfMissiles
    CROSSBOW: str  # Crossbow
    CROSSBOW_AMMO: str  # CrossbowAmmo
    CROSSBOW_FX1: str  # CrossbowFX1
    CROSSBOW_FX2: str  # CrossbowFX2
    CROSSBOW_FX3: str  # CrossbowFX3
    CROSSBOW_FX4: str  # CrossbowFX4
    CROSSBOW_HEFTY: str  # CrossbowHefty
    CROSSBOW_POWERED: str  # CrossbowPowered
    CRUSADER: str  # Crusader
    CRUSADER_MISSILE: str  # CrusaderMissile
    CRYSTAL_VIAL: str  # CrystalVial
    CUSTOM_BRIDGE: str  # CustomBridge
    CUSTOM_INVENTORY: str  # CustomInventory
    CUSTOM_SPRITE: str  # CustomSprite
    CYBERDEMON: str  # Cyberdemon
    C_FLAME_FLOOR: str  # CFlameFloor
    C_FLAME_MISSILE: str  # CFlameMissile
    C_STAFF_MISSILE: str  # CStaffMissile
    C_STAFF_PUFF: str  # CStaffPuff
    C_WEAPON_PIECE1: str  # CWeaponPiece1
    C_WEAPON_PIECE2: str  # CWeaponPiece2
    C_WEAPON_PIECE3: str  # CWeaponPiece3
    C_WEAP_FLAME: str  # CWeapFlame
    C_WEAP_MACE: str  # CWeapMace
    C_WEAP_STAFF: str  # CWeapStaff
    C_WEAP_WRAITHVERGE: str  # CWeapWraithverge
    DART: str  # Dart
    DEAD_ACOLYTE: str  # DeadAcolyte
    DEAD_CACODEMON: str  # DeadCacodemon
    DEAD_CRUSADER: str  # DeadCrusader
    DEAD_DEMON: str  # DeadDemon
    DEAD_DOOM_IMP: str  # DeadDoomImp
    DEAD_LOST_SOUL: str  # DeadLostSoul
    DEAD_MARINE: str  # DeadMarine
    DEAD_PEASANT: str  # DeadPeasant
    DEAD_REAVER: str  # DeadReaver
    DEAD_REBEL: str  # DeadRebel
    DEAD_SHOTGUN_GUY: str  # DeadShotgunGuy
    DEAD_STICK: str  # DeadStick
    DEAD_STRIFE_PLAYER: str  # DeadStrifePlayer
    DEAD_ZOMBIE_MAN: str  # DeadZombieMan
    DECAL: str  # Decal
    DEGNIN_ORE: str  # DegninOre
    DEHACKED_PICKUP: str  # DehackedPickup
    DEH__ACTOR_145: str  # Deh_Actor_145
    DEH__ACTOR_146: str  # Deh_Actor_146
    DEH__ACTOR_147: str  # Deh_Actor_147
    DEH__ACTOR_148: str  # Deh_Actor_148
    DEH__ACTOR_149: str  # Deh_Actor_149
    DEH__ACTOR_150: str  # Deh_Actor_150
    DEH__ACTOR_151: str  # Deh_Actor_151
    DEH__ACTOR_152: str  # Deh_Actor_152
    DEH__ACTOR_153: str  # Deh_Actor_153
    DEH__ACTOR_154: str  # Deh_Actor_154
    DEH__ACTOR_155: str  # Deh_Actor_155
    DEH__ACTOR_156: str  # Deh_Actor_156
    DEH__ACTOR_157: str  # Deh_Actor_157
    DEH__ACTOR_158: str  # Deh_Actor_158
    DEH__ACTOR_159: str  # Deh_Actor_159
    DEH__ACTOR_160: str  # Deh_Actor_160
    DEH__ACTOR_161: str  # Deh_Actor_161
    DEH__ACTOR_162: str  # Deh_Actor_162
    DEH__ACTOR_163: str  # Deh_Actor_163
    DEH__ACTOR_164: str  # Deh_Actor_164
    DEH__ACTOR_165: str  # Deh_Actor_165
    DEH__ACTOR_166: str  # Deh_Actor_166
    DEH__ACTOR_167: str  # Deh_Actor_167
    DEH__ACTOR_168: str  # Deh_Actor_168
    DEH__ACTOR_169: str  # Deh_Actor_169
    DEH__ACTOR_170: str  # Deh_Actor_170
    DEH__ACTOR_171: str  # Deh_Actor_171
    DEH__ACTOR_172: str  # Deh_Actor_172
    DEH__ACTOR_173: str  # Deh_Actor_173
    DEH__ACTOR_174: str  # Deh_Actor_174
    DEH__ACTOR_175: str  # Deh_Actor_175
    DEH__ACTOR_176: str  # Deh_Actor_176
    DEH__ACTOR_177: str  # Deh_Actor_177
    DEH__ACTOR_178: str  # Deh_Actor_178
    DEH__ACTOR_179: str  # Deh_Actor_179
    DEH__ACTOR_180: str  # Deh_Actor_180
    DEH__ACTOR_181: str  # Deh_Actor_181
    DEH__ACTOR_182: str  # Deh_Actor_182
    DEH__ACTOR_183: str  # Deh_Actor_183
    DEH__ACTOR_184: str  # Deh_Actor_184
    DEH__ACTOR_185: str  # Deh_Actor_185
    DEH__ACTOR_186: str  # Deh_Actor_186
    DEH__ACTOR_187: str  # Deh_Actor_187
    DEH__ACTOR_188: str  # Deh_Actor_188
    DEH__ACTOR_189: str  # Deh_Actor_189
    DEH__ACTOR_190: str  # Deh_Actor_190
    DEH__ACTOR_191: str  # Deh_Actor_191
    DEH__ACTOR_192: str  # Deh_Actor_192
    DEH__ACTOR_193: str  # Deh_Actor_193
    DEH__ACTOR_194: str  # Deh_Actor_194
    DEH__ACTOR_195: str  # Deh_Actor_195
    DEH__ACTOR_196: str  # Deh_Actor_196
    DEH__ACTOR_197: str  # Deh_Actor_197
    DEH__ACTOR_198: str  # Deh_Actor_198
    DEH__ACTOR_199: str  # Deh_Actor_199
    DEH__ACTOR_200: str  # Deh_Actor_200
    DEH__ACTOR_201: str  # Deh_Actor_201
    DEH__ACTOR_202: str  # Deh_Actor_202
    DEH__ACTOR_203: str  # Deh_Actor_203
    DEH__ACTOR_204: str  # Deh_Actor_204
    DEH__ACTOR_205: str  # Deh_Actor_205
    DEH__ACTOR_206: str  # Deh_Actor_206
    DEH__ACTOR_207: str  # Deh_Actor_207
    DEH__ACTOR_208: str  # Deh_Actor_208
    DEH__ACTOR_209: str  # Deh_Actor_209
    DEH__ACTOR_210: str  # Deh_Actor_210
    DEH__ACTOR_211: str  # Deh_Actor_211
    DEH__ACTOR_212: str  # Deh_Actor_212
    DEH__ACTOR_213: str  # Deh_Actor_213
    DEH__ACTOR_214: str  # Deh_Actor_214
    DEH__ACTOR_215: str  # Deh_Actor_215
    DEH__ACTOR_216: str  # Deh_Actor_216
    DEH__ACTOR_217: str  # Deh_Actor_217
    DEH__ACTOR_218: str  # Deh_Actor_218
    DEH__ACTOR_219: str  # Deh_Actor_219
    DEH__ACTOR_220: str  # Deh_Actor_220
    DEH__ACTOR_221: str  # Deh_Actor_221
    DEH__ACTOR_222: str  # Deh_Actor_222
    DEH__ACTOR_223: str  # Deh_Actor_223
    DEH__ACTOR_224: str  # Deh_Actor_224
    DEH__ACTOR_225: str  # Deh_Actor_225
    DEH__ACTOR_226: str  # Deh_Actor_226
    DEH__ACTOR_227: str  # Deh_Actor_227
    DEH__ACTOR_228: str  # Deh_Actor_228
    DEH__ACTOR_229: str  # Deh_Actor_229
    DEH__ACTOR_230: str  # Deh_Actor_230
    DEH__ACTOR_231: str  # Deh_Actor_231
    DEH__ACTOR_232: str  # Deh_Actor_232
    DEH__ACTOR_233: str  # Deh_Actor_233
    DEH__ACTOR_234: str  # Deh_Actor_234
    DEH__ACTOR_235: str  # Deh_Actor_235
    DEH__ACTOR_236: str  # Deh_Actor_236
    DEH__ACTOR_237: str  # Deh_Actor_237
    DEH__ACTOR_238: str  # Deh_Actor_238
    DEH__ACTOR_239: str  # Deh_Actor_239
    DEH__ACTOR_240: str  # Deh_Actor_240
    DEH__ACTOR_241: str  # Deh_Actor_241
    DEH__ACTOR_242: str  # Deh_Actor_242
    DEH__ACTOR_243: str  # Deh_Actor_243
    DEH__ACTOR_244: str  # Deh_Actor_244
    DEH__ACTOR_245: str  # Deh_Actor_245
    DEH__ACTOR_246: str  # Deh_Actor_246
    DEH__ACTOR_247: str  # Deh_Actor_247
    DEH__ACTOR_248: str  # Deh_Actor_248
    DEH__ACTOR_249: str  # Deh_Actor_249
    DEH__ACTOR_250: str  # Deh_Actor_250
    DEMON: str  # Demon
    DEMON1: str  # Demon1
    DEMON1_CHUNK1: str  # Demon1Chunk1
    DEMON1_CHUNK2: str  # Demon1Chunk2
    DEMON1_CHUNK3: str  # Demon1Chunk3
    DEMON1_CHUNK4: str  # Demon1Chunk4
    DEMON1_CHUNK5: str  # Demon1Chunk5
    DEMON1_FX1: str  # Demon1FX1
    DEMON1_MASH: str  # Demon1Mash
    DEMON2: str  # Demon2
    DEMON2_CHUNK1: str  # Demon2Chunk1
    DEMON2_CHUNK2: str  # Demon2Chunk2
    DEMON2_CHUNK3: str  # Demon2Chunk3
    DEMON2_CHUNK4: str  # Demon2Chunk4
    DEMON2_CHUNK5: str  # Demon2Chunk5
    DEMON2_FX1: str  # Demon2FX1
    DEMON2_MASH: str  # Demon2Mash
    DEMON_CHUNK: str  # DemonChunk
    DIRT1: str  # Dirt1
    DIRT2: str  # Dirt2
    DIRT3: str  # Dirt3
    DIRT4: str  # Dirt4
    DIRT5: str  # Dirt5
    DIRT6: str  # Dirt6
    DIRT_CLUMP: str  # DirtClump
    DOOM_BUILDER_CAMERA: str  # DoomBuilderCamera
    DOOM_IMP: str  # DoomImp
    DOOM_IMP_BALL: str  # DoomImpBall
    DOOM_KEY: str  # DoomKey
    DOOM_PLAYER: str  # DoomPlayer
    DOOM_UNUSED_STATES: str  # DoomUnusedStates
    DOOM_WEAPON: str  # DoomWeapon
    DRAGON: str  # Dragon
    DRAGON_EXPLOSION: str  # DragonExplosion
    DRAGON_FIREBALL: str  # DragonFireball
    DUMMY_STRIFE_ITEM: str  # DummyStrifeItem
    DYNAMIC_LIGHT: str  # DynamicLight
    EAR: str  # Ear
    EGG_FX: str  # EggFX
    ELECTRIC_BOLT: str  # ElectricBolt
    ELECTRIC_BOLTS: str  # ElectricBolts
    ENCHANTED_SHIELD: str  # EnchantedShield
    ENERGY_PACK: str  # EnergyPack
    ENERGY_POD: str  # EnergyPod
    ENTITY_BOSS: str  # EntityBoss
    ENTITY_NEST: str  # EntityNest
    ENTITY_POD: str  # EntityPod
    ENTITY_SECOND: str  # EntitySecond
    ENVIRONMENTAL_SUIT: str  # EnvironmentalSuit
    ETTIN: str  # Ettin
    ETTIN_MACE: str  # EttinMace
    ETTIN_MASH: str  # EttinMash
    EVIL_EYE: str  # EvilEye
    EVIL_SCEPTRE: str  # EvilSceptre
    EXPLOSIVE_BARREL: str  # ExplosiveBarrel
    EXPLOSIVE_BARREL2: str  # ExplosiveBarrel2
    FACTORY_KEY: str  # FactoryKey
    FADE_SETTER: str  # FadeSetter
    FAKE_INVENTORY: str  # FakeInventory
    FALCON_SHIELD: str  # FalconShield
    FAST_FLAME_MISSILE: str  # FastFlameMissile
    FAST_PROJECTILE: str  # FastProjectile
    FATSO: str  # Fatso
    FAT_SHOT: str  # FatShot
    FEATHER: str  # Feather
    FIGHTER_BOSS: str  # FighterBoss
    FIGHTER_PLAYER: str  # FighterPlayer
    FIGHTER_WEAPON: str  # FighterWeapon
    FIGHTER_WEAPON_PIECE: str  # FighterWeaponPiece
    FIRE_BALL: str  # FireBall
    FIRE_BOMB: str  # FireBomb
    FIRE_BRAZIER: str  # FireBrazier
    FIRE_DEMON: str  # FireDemon
    FIRE_DEMON_MISSILE: str  # FireDemonMissile
    FIRE_DEMON_ROCK1: str  # FireDemonRock1
    FIRE_DEMON_ROCK2: str  # FireDemonRock2
    FIRE_DEMON_ROCK3: str  # FireDemonRock3
    FIRE_DEMON_ROCK4: str  # FireDemonRock4
    FIRE_DEMON_ROCK5: str  # FireDemonRock5
    FIRE_DEMON_SPLOTCH1: str  # FireDemonSplotch1
    FIRE_DEMON_SPLOTCH2: str  # FireDemonSplotch2
    FIRE_DROPLET: str  # FireDroplet
    FIRE_THING: str  # FireThing
    FIST: str  # Fist
    FLAME_LARGE: str  # FlameLarge
    FLAME_LARGE2: str  # FlameLarge2
    FLAME_LARGE_TEMP: str  # FlameLargeTemp
    FLAME_MISSILE: str  # FlameMissile
    FLAME_PUFF: str  # FlamePuff
    FLAME_PUFF2: str  # FlamePuff2
    FLAME_SMALL: str  # FlameSmall
    FLAME_SMALL2: str  # FlameSmall2
    FLAME_SMALL_TEMP: str  # FlameSmallTemp
    FLAME_THROWER: str  # FlameThrower
    FLAME_THROWER_PARTS: str  # FlameThrowerParts
    FLEMBRANE: str  # Flembrane
    FLEMOIDUS_BIPEDICUS: str  # FlemoidusBipedicus
    FLEMOIDUS_COMMONUS: str  # FlemoidusCommonus
    FLEMOIDUS_CYCLOPTIS_COMMONUS: str  # FlemoidusCycloptisCommonus
    FLOATING_SKULL: str  # FloatingSkull
    FOG_PATCH_LARGE: str  # FogPatchLarge
    FOG_PATCH_MEDIUM: str  # FogPatchMedium
    FOG_PATCH_SMALL: str  # FogPatchSmall
    FOG_SPAWNER: str  # FogSpawner
    FORCE_FIELD_GUARD: str  # ForceFieldGuard
    FROST_MISSILE: str  # FrostMissile
    FS__MAPSPOT: str  # FS_Mapspot
    F_SWORD_FLAME: str  # FSwordFlame
    F_SWORD_MISSILE: str  # FSwordMissile
    F_WEAPON_PIECE1: str  # FWeaponPiece1
    F_WEAPON_PIECE2: str  # FWeaponPiece2
    F_WEAPON_PIECE3: str  # FWeaponPiece3
    F_WEAP_AXE: str  # FWeapAxe
    F_WEAP_FIST: str  # FWeapFist
    F_WEAP_HAMMER: str  # FWeapHammer
    F_WEAP_QUIETUS: str  # FWeapQuietus
    GAUNTLETS: str  # Gauntlets
    GAUNTLETS_POWERED: str  # GauntletsPowered
    GAUNTLET_PUFF1: str  # GauntletPuff1
    GAUNTLET_PUFF2: str  # GauntletPuff2
    GIBBED_MARINE: str  # GibbedMarine
    GIBBED_MARINE_EXTRA: str  # GibbedMarineExtra
    GIBS: str  # Gibs
    GLASS_JUNK: str  # GlassJunk
    GLASS_OF_WATER: str  # GlassOfWater
    GLASS_SHARD: str  # GlassShard
    GOLD10: str  # Gold10
    GOLD25: str  # Gold25
    GOLD300: str  # Gold300
    GOLD50: str  # Gold50
    GOLD_KEY: str  # GoldKey
    GOLD_WAND: str  # GoldWand
    GOLD_WAND_AMMO: str  # GoldWandAmmo
    GOLD_WAND_FX1: str  # GoldWandFX1
    GOLD_WAND_FX2: str  # GoldWandFX2
    GOLD_WAND_HEFTY: str  # GoldWandHefty
    GOLD_WAND_POWERED: str  # GoldWandPowered
    GOLD_WAND_PUFF1: str  # GoldWandPuff1
    GOLD_WAND_PUFF2: str  # GoldWandPuff2
    GOVS_KEY: str  # GovsKey
    GREEN_ARMOR: str  # GreenArmor
    GREEN_PARTICLE_FOUNTAIN: str  # GreenParticleFountain
    GREEN_TORCH: str  # GreenTorch
    GRENADE: str  # Grenade
    GRENADE_SMOKE_TRAIL: str  # GrenadeSmokeTrail
    GUARD_UNIFORM: str  # GuardUniform
    GUN_TRAINING: str  # GunTraining
    HAMMER_MISSILE: str  # HammerMissile
    HAMMER_PUFF: str  # HammerPuff
    HANGING_CORPSE: str  # HangingCorpse
    HANG_B_NO_BRAIN: str  # HangBNoBrain
    HANG_NO_GUTS: str  # HangNoGuts
    HANG_T_LOOKING_DOWN: str  # HangTLookingDown
    HANG_T_LOOKING_UP: str  # HangTLookingUp
    HANG_T_NO_BRAIN: str  # HangTNoBrain
    HANG_T_SKULL: str  # HangTSkull
    HATE_TARGET: str  # HateTarget
    HEADS_ON_A_STICK: str  # HeadsOnAStick
    HEAD_CANDLES: str  # HeadCandles
    HEAD_FX1: str  # HeadFX1
    HEAD_FX2: str  # HeadFX2
    HEAD_FX3: str  # HeadFX3
    HEAD_ON_A_STICK: str  # HeadOnAStick
    HEALTH: str  # Health
    HEALTH_BONUS: str  # HealthBonus
    HEALTH_FILLUP: str  # HealthFillup
    HEALTH_PICKUP: str  # HealthPickup
    HEALTH_TRAINING: str  # HealthTraining
    HEARTS_IN_TANK: str  # HeartsInTank
    HEART_COLUMN: str  # HeartColumn
    HELL_KNIGHT: str  # HellKnight
    HERESIARCH: str  # Heresiarch
    HERETIC_IMP: str  # HereticImp
    HERETIC_IMP_BALL: str  # HereticImpBall
    HERETIC_IMP_CHUNK1: str  # HereticImpChunk1
    HERETIC_IMP_CHUNK2: str  # HereticImpChunk2
    HERETIC_IMP_LEADER: str  # HereticImpLeader
    HERETIC_KEY: str  # HereticKey
    HERETIC_PLAYER: str  # HereticPlayer
    HERETIC_SOUND_SEQUENCE1: str  # HereticSoundSequence1
    HERETIC_SOUND_SEQUENCE10: str  # HereticSoundSequence10
    HERETIC_SOUND_SEQUENCE2: str  # HereticSoundSequence2
    HERETIC_SOUND_SEQUENCE3: str  # HereticSoundSequence3
    HERETIC_SOUND_SEQUENCE4: str  # HereticSoundSequence4
    HERETIC_SOUND_SEQUENCE5: str  # HereticSoundSequence5
    HERETIC_SOUND_SEQUENCE6: str  # HereticSoundSequence6
    HERETIC_SOUND_SEQUENCE7: str  # HereticSoundSequence7
    HERETIC_SOUND_SEQUENCE8: str  # HereticSoundSequence8
    HERETIC_SOUND_SEQUENCE9: str  # HereticSoundSequence9
    HERETIC_WEAPON: str  # HereticWeapon
    HEXEN_ARMOR: str  # HexenArmor
    HEXEN_KEY: str  # HexenKey
    HE_GRENADE: str  # HEGrenade
    HE_GRENADE_ROUNDS: str  # HEGrenadeRounds
    HOLY_MISSILE: str  # HolyMissile
    HOLY_MISSILE_PUFF: str  # HolyMissilePuff
    HOLY_PUFF: str  # HolyPuff
    HOLY_SPIRIT: str  # HolySpirit
    HOLY_TAIL: str  # HolyTail
    HOLY_TAIL_TRAIL: str  # HolyTailTrail
    HORN_ROD_FX1: str  # HornRodFX1
    HORN_ROD_FX2: str  # HornRodFX2
    HUGE_TORCH: str  # HugeTorch
    H_WATER_DRIP: str  # HWaterDrip
    ICE_CHUNK: str  # IceChunk
    ICE_CHUNK_HEAD: str  # IceChunkHead
    ICE_FX_PUFF: str  # IceFXPuff
    ICE_GUY: str  # IceGuy
    ICE_GUY_BIT: str  # IceGuyBit
    ICE_GUY_FX: str  # IceGuyFX
    ICE_GUY_FX2: str  # IceGuyFX2
    ICE_GUY_WISP1: str  # IceGuyWisp1
    ICE_GUY_WISP2: str  # IceGuyWisp2
    ICE_SHARD: str  # IceShard
    ID24_AMBIENT_KLAXON: str  # ID24AmbientKlaxon
    ID24_AMBIENT_PORTAL_CLOSE: str  # ID24AmbientPortalClose
    ID24_AMBIENT_PORTAL_LOOP: str  # ID24AmbientPortalLoop
    ID24_AMBIENT_PORTAL_OPEN: str  # ID24AmbientPortalOpen
    ID24_BANSHEE: str  # ID24Banshee
    ID24_BUSH_SHORT: str  # ID24BushShort
    ID24_BUSH_SHORT_BURNED1: str  # ID24BushShortBurned1
    ID24_BUSH_SHORT_BURNED2: str  # ID24BushShortBurned2
    ID24_BUSH_TALL: str  # ID24BushTall
    ID24_BUSH_TALL_BURNED1: str  # ID24BushTallBurned1
    ID24_BUSH_TALL_BURNED2: str  # ID24BushTallBurned2
    ID24_CALAMITY_BLADE: str  # ID24CalamityBlade
    ID24_CANDELABRA_SHORT: str  # ID24CandelabraShort
    ID24_CAVE_ROCK_COLUMN: str  # ID24CaveRockColumn
    ID24_CAVE_STALACTITE_LARGE: str  # ID24CaveStalactiteLarge
    ID24_CAVE_STALACTITE_LARGE_SOLID: str  # ID24CaveStalactiteLargeSolid
    ID24_CAVE_STALACTITE_MEDIUM: str  # ID24CaveStalactiteMedium
    ID24_CAVE_STALACTITE_MEDIUM_SOLID: str  # ID24CaveStalactiteMediumSolid
    ID24_CAVE_STALACTITE_SMALL: str  # ID24CaveStalactiteSmall
    ID24_CAVE_STALACTITE_SMALL_SOLID: str  # ID24CaveStalactiteSmallSolid
    ID24_CAVE_STALAGMITE_LARGE: str  # ID24CaveStalagmiteLarge
    ID24_CAVE_STALAGMITE_MEDIUM: str  # ID24CaveStalagmiteMedium
    ID24_CAVE_STALAGMITE_SMALL: str  # ID24CaveStalagmiteSmall
    ID24_CEILING_LAMP: str  # ID24CeilingLamp
    ID24_FUEL: str  # ID24Fuel
    ID24_FUEL_TANK: str  # ID24FuelTank
    ID24_GHOUL: str  # ID24Ghoul
    ID24_GHOUL_BALL: str  # ID24GhoulBall
    ID24_GRAY_STALAGMITE: str  # ID24GrayStalagmite
    ID24_HANGING_BARON_OF_HELL: str  # ID24HangingBaronOfHell
    ID24_HANGING_BARON_OF_HELL_SOLID: str  # ID24HangingBaronOfHellSolid
    ID24_HANGING_BODY_ARMS_BOUND: str  # ID24HangingBodyArmsBound
    ID24_HANGING_BODY_ARMS_BOUND_SOLID: str  # ID24HangingBodyArmsBoundSolid
    ID24_HANGING_BODY_BOTH_LEGS: str  # ID24HangingBodyBothLegs
    ID24_HANGING_BODY_BOTH_LEGS_SOLID: str  # ID24HangingBodyBothLegsSolid
    ID24_HANGING_BODY_CRUCIFIED: str  # ID24HangingBodyCrucified
    ID24_HANGING_BODY_CRUCIFIED_SOLID: str  # ID24HangingBodyCrucifiedSolid
    ID24_HANGING_CHAINED_BODY: str  # ID24HangingChainedBody
    ID24_HANGING_CHAINED_BODY_SOLID: str  # ID24HangingChainedBodySolid
    ID24_HANGING_CHAINED_TORSO: str  # ID24HangingChainedTorso
    ID24_HANGING_CHAINED_TORSO_SOLID: str  # ID24HangingChainedTorsoSolid
    ID24_HUMAN_BBQ1: str  # ID24HumanBBQ1
    ID24_HUMAN_BBQ2: str  # ID24HumanBBQ2
    ID24_INCINERATOR: str  # ID24Incinerator
    ID24_INCINERATOR_FLAME: str  # ID24IncineratorFlame
    ID24_INCINERATOR_PROJECTILE: str  # ID24IncineratorProjectile
    ID24_LARGE_CORPSE_PILE: str  # ID24LargeCorpsePile
    ID24_MINDWEAVER: str  # ID24Mindweaver
    ID24_OFFICE_CHAIR: str  # ID24OfficeChair
    ID24_OFFICE_LAMP: str  # ID24OfficeLamp
    ID24_PLASMA_GUY: str  # ID24PlasmaGuy
    ID24_PLASMA_GUY_HEAD: str  # ID24PlasmaGuyHead
    ID24_PLASMA_GUY_TORSO: str  # ID24PlasmaGuyTorso
    ID24_SKULL_GIBS: str  # ID24SkullGibs
    ID24_SKULL_POLE_TRIO: str  # ID24SkullPoleTrio
    ID24_TYRANT: str  # ID24Tyrant
    ID24_TYRANT_BOSS1: str  # ID24TyrantBoss1
    ID24_TYRANT_BOSS2: str  # ID24TyrantBoss2
    ID24_VASSAGO: str  # ID24Vassago
    ID24_VASSAGO_FLAME: str  # ID24VassagoFlame
    ID_BADGE: str  # IDBadge
    ID_CARD: str  # IDCard
    INFO: str  # info
    INFRARED: str  # Infrared
    INQUISITOR: str  # Inquisitor
    INQUISITOR_ARM: str  # InquisitorArm
    INQUISITOR_SHOT: str  # InquisitorShot
    INTERPOLATION_POINT: str  # InterpolationPoint
    INTERPOLATION_SPECIAL: str  # InterpolationSpecial
    INTERROGATOR_REPORT: str  # InterrogatorReport
    INVENTORY: str  # Inventory
    INVISIBLE_BRIDGE: str  # InvisibleBridge
    INVISIBLE_BRIDGE16: str  # InvisibleBridge16
    INVISIBLE_BRIDGE32: str  # InvisibleBridge32
    INVISIBLE_BRIDGE8: str  # InvisibleBridge8
    INVULNERABILITY_SPHERE: str  # InvulnerabilitySphere
    IRONLICH: str  # Ironlich
    ITEM_FOG: str  # ItemFog
    JUNK: str  # junk
    KEY: str  # Key
    KEY_AXE: str  # KeyAxe
    KEY_BLUE: str  # KeyBlue
    KEY_CASTLE: str  # KeyCastle
    KEY_CAVE: str  # KeyCave
    KEY_DUNGEON: str  # KeyDungeon
    KEY_EMERALD: str  # KeyEmerald
    KEY_FIRE: str  # KeyFire
    KEY_GIZMO_BLUE: str  # KeyGizmoBlue
    KEY_GIZMO_FLOAT_BLUE: str  # KeyGizmoFloatBlue
    KEY_GIZMO_FLOAT_GREEN: str  # KeyGizmoFloatGreen
    KEY_GIZMO_FLOAT_YELLOW: str  # KeyGizmoFloatYellow
    KEY_GIZMO_GREEN: str  # KeyGizmoGreen
    KEY_GIZMO_YELLOW: str  # KeyGizmoYellow
    KEY_GREEN: str  # KeyGreen
    KEY_HORN: str  # KeyHorn
    KEY_RUSTED: str  # KeyRusted
    KEY_SILVER: str  # KeySilver
    KEY_STEEL: str  # KeySteel
    KEY_SWAMP: str  # KeySwamp
    KEY_YELLOW: str  # KeyYellow
    KLAXON_WARNING_LIGHT: str  # KlaxonWarningLight
    KNEELING_GUY: str  # KneelingGuy
    KNIGHT: str  # Knight
    KNIGHT_AXE: str  # KnightAxe
    KNIGHT_GHOST: str  # KnightGhost
    KORAX: str  # Korax
    KORAX_BOLT: str  # KoraxBolt
    KORAX_SPIRIT: str  # KoraxSpirit
    LARGE_TORCH: str  # LargeTorch
    LARGE_ZORCHER: str  # LargeZorcher
    LARGE_ZORCH_PACK: str  # LargeZorchPack
    LARGE_ZORCH_RECHARGE: str  # LargeZorchRecharge
    LAVA_SMOKE: str  # LavaSmoke
    LAVA_SPLASH: str  # LavaSplash
    LAZ_BALL: str  # LAZBall
    LAZ_DEVICE: str  # LAZDevice
    LEAF1: str  # Leaf1
    LEAF2: str  # Leaf2
    LEAF_SPAWNER: str  # LeafSpawner
    LEATHER_ARMOR: str  # LeatherArmor
    LIGHTNING: str  # Lightning
    LIGHTNING_CEILING: str  # LightningCeiling
    LIGHTNING_FLOOR: str  # LightningFloor
    LIGHTNING_ZAP: str  # LightningZap
    LIGHT_BROWN_FLUORESCENT: str  # LightBrownFluorescent
    LIGHT_GLOBE: str  # LightGlobe
    LIGHT_GOLD_FLUORESCENT: str  # LightGoldFluorescent
    LIGHT_SILVER_FLUORESCENT: str  # LightSilverFluorescent
    LITTLE_FLY: str  # LittleFly
    LIVE_STICK: str  # LiveStick
    LOREMASTER: str  # Loremaster
    LORE_SHOT: str  # LoreShot
    LORE_SHOT2: str  # LoreShot2
    LOST_SOUL: str  # LostSoul
    LOWER_STACK_LOOK_ONLY: str  # LowerStackLookOnly
    MACE: str  # Mace
    MACE_AMMO: str  # MaceAmmo
    MACE_FX1: str  # MaceFX1
    MACE_FX2: str  # MaceFX2
    MACE_FX3: str  # MaceFX3
    MACE_FX4: str  # MaceFX4
    MACE_HEFTY: str  # MaceHefty
    MACE_POWERED: str  # MacePowered
    MACE_SPAWNER: str  # MaceSpawner
    MACIL1: str  # Macil1
    MACIL2: str  # Macil2
    MAGE_BOSS: str  # MageBoss
    MAGE_PLAYER: str  # MagePlayer
    MAGE_STAFF_FX2: str  # MageStaffFX2
    MAGE_WAND_MISSILE: str  # MageWandMissile
    MAGE_WAND_SMOKE: str  # MageWandSmoke
    MAGE_WEAPON: str  # MageWeapon
    MAGE_WEAPON_PIECE: str  # MageWeaponPiece
    MANA1: str  # Mana1
    MANA2: str  # Mana2
    MANA3: str  # Mana3
    MAP_MARKER: str  # MapMarker
    MAP_REVEALER: str  # MapRevealer
    MAP_SPOT: str  # MapSpot
    MAP_SPOT_GRAVITY: str  # MapSpotGravity
    MARINE_BERSERK: str  # MarineBerserk
    MARINE_BFG: str  # MarineBFG
    MARINE_CHAINGUN: str  # MarineChaingun
    MARINE_CHAINSAW: str  # MarineChainsaw
    MARINE_FIST: str  # MarineFist
    MARINE_PISTOL: str  # MarinePistol
    MARINE_PLASMA: str  # MarinePlasma
    MARINE_RAILGUN: str  # MarineRailgun
    MARINE_ROCKET: str  # MarineRocket
    MARINE_SHOTGUN: str  # MarineShotgun
    MARINE_SSG: str  # MarineSSG
    MAULER: str  # Mauler
    MAULER2: str  # Mauler2
    MAULER_KEY: str  # MaulerKey
    MAULER_PUFF: str  # MaulerPuff
    MAULER_TORPEDO: str  # MaulerTorpedo
    MAULER_TORPEDO_WAVE: str  # MaulerTorpedoWave
    MAX_HEALTH: str  # MaxHealth
    MBF_HELPER_DOG: str  # MBFHelperDog
    MEAT: str  # MEAT
    MEAT2: str  # Meat2
    MEAT3: str  # Meat3
    MEAT4: str  # Meat4
    MEAT5: str  # Meat5
    MEDIC: str  # Medic
    MEDICAL_KIT: str  # MedicalKit
    MEDIKIT: str  # Medikit
    MEDIUM_TORCH: str  # MediumTorch
    MED_PATCH: str  # MedPatch
    MEGASPHERE: str  # Megasphere
    MEGASPHERE_HEALTH: str  # MegasphereHealth
    MERCHANT: str  # Merchant
    MESH_ARMOR: str  # MeshArmor
    METAL_ARMOR: str  # MetalArmor
    METAL_POT: str  # MetalPot
    MILITARY_ID: str  # MilitaryID
    MINE_KEY: str  # MineKey
    MINI_MISSILE: str  # MiniMissile
    MINI_MISSILES: str  # MiniMissiles
    MINI_MISSILE_LAUNCHER: str  # MiniMissileLauncher
    MINI_MISSILE_PUFF: str  # MiniMissilePuff
    MINI_ZORCHER: str  # MiniZorcher
    MINI_ZORCH_PACK: str  # MiniZorchPack
    MINI_ZORCH_RECHARGE: str  # MiniZorchRecharge
    MINOTAUR: str  # Minotaur
    MINOTAUR_FRIEND: str  # MinotaurFriend
    MINOTAUR_FX1: str  # MinotaurFX1
    MINOTAUR_FX2: str  # MinotaurFX2
    MINOTAUR_FX3: str  # MinotaurFX3
    MINOTAUR_SMOKE: str  # MinotaurSmoke
    MINOTAUR_SMOKE_EXIT: str  # MinotaurSmokeExit
    MORPHED_MONSTER: str  # MorphedMonster
    MORPH_PROJECTILE: str  # MorphProjectile
    MOSS1: str  # Moss1
    MOSS2: str  # Moss2
    MOVING_CAMERA: str  # MovingCamera
    MUG: str  # Mug
    MUMMY: str  # Mummy
    MUMMY_FX1: str  # MummyFX1
    MUMMY_GHOST: str  # MummyGhost
    MUMMY_LEADER: str  # MummyLeader
    MUMMY_LEADER_GHOST: str  # MummyLeaderGhost
    MUMMY_SOUL: str  # MummySoul
    MUSIC_CHANGER: str  # MusicChanger
    M_WEAPON_PIECE1: str  # MWeaponPiece1
    M_WEAPON_PIECE2: str  # MWeaponPiece2
    M_WEAPON_PIECE3: str  # MWeaponPiece3
    M_WEAP_BLOODSCOURGE: str  # MWeapBloodscourge
    M_WEAP_FROST: str  # MWeapFrost
    M_WEAP_LIGHTNING: str  # MWeapLightning
    M_WEAP_WAND: str  # MWeapWand
    NEW_KEY5: str  # NewKey5
    NONSOLID_MEAT2: str  # NonsolidMeat2
    NONSOLID_MEAT3: str  # NonsolidMeat3
    NONSOLID_MEAT4: str  # NonsolidMeat4
    NONSOLID_MEAT5: str  # NonsolidMeat5
    NONSOLID_TWITCH: str  # NonsolidTwitch
    OFFERING_CHALICE: str  # OfferingChalice
    OFFICERS_UNIFORM: str  # OfficersUniform
    OPEN_DOOR222: str  # OpenDoor222
    OPEN_DOOR224: str  # OpenDoor224
    ORACLE: str  # Oracle
    ORACLE_KEY: str  # OracleKey
    ORACLE_PASS: str  # OraclePass
    ORDER_KEY: str  # OrderKey
    ORTHOGRAPHIC_CAMERA: str  # OrthographicCamera
    OUTSIDE_LAMP: str  # OutsideLamp
    PAIN_ELEMENTAL: str  # PainElemental
    PALM_TREE: str  # PalmTree
    PARTICLE_FOUNTAIN: str  # ParticleFountain
    PASSCARD: str  # Passcard
    PATH_FOLLOWER: str  # PathFollower
    PATROL_POINT: str  # PatrolPoint
    PATROL_SPECIAL: str  # PatrolSpecial
    PEASANT: str  # Peasant
    PEASANT1: str  # Peasant1
    PEASANT10: str  # Peasant10
    PEASANT11: str  # Peasant11
    PEASANT12: str  # Peasant12
    PEASANT13: str  # Peasant13
    PEASANT14: str  # Peasant14
    PEASANT15: str  # Peasant15
    PEASANT16: str  # Peasant16
    PEASANT17: str  # Peasant17
    PEASANT18: str  # Peasant18
    PEASANT19: str  # Peasant19
    PEASANT2: str  # Peasant2
    PEASANT20: str  # Peasant20
    PEASANT21: str  # Peasant21
    PEASANT22: str  # Peasant22
    PEASANT3: str  # Peasant3
    PEASANT4: str  # Peasant4
    PEASANT5: str  # Peasant5
    PEASANT6: str  # Peasant6
    PEASANT7: str  # Peasant7
    PEASANT8: str  # Peasant8
    PEASANT9: str  # Peasant9
    PHASE_ZORCH_MISSILE: str  # PhaseZorchMissile
    PHASING_ZORCH: str  # PhasingZorch
    PHASING_ZORCHER: str  # PhasingZorcher
    PHASING_ZORCH_PACK: str  # PhasingZorchPack
    PHOENIX_FX1: str  # PhoenixFX1
    PHOENIX_FX2: str  # PhoenixFX2
    PHOENIX_PUFF: str  # PhoenixPuff
    PHOENIX_ROD: str  # PhoenixRod
    PHOENIX_ROD_AMMO: str  # PhoenixRodAmmo
    PHOENIX_ROD_HEFTY: str  # PhoenixRodHefty
    PHOENIX_ROD_POWERED: str  # PhoenixRodPowered
    PHOSPHOROUS_FIRE: str  # PhosphorousFire
    PHOSPHOROUS_GRENADE: str  # PhosphorousGrenade
    PHOSPHORUS_GRENADE_ROUNDS: str  # PhosphorusGrenadeRounds
    PICKUP_FLASH: str  # PickupFlash
    PIG: str  # Pig
    PIG_PLAYER: str  # PigPlayer
    PILE_OF_GUTS: str  # PileOfGuts
    PILLAR_ALIEN_POWER: str  # PillarAlienPower
    PILLAR_AZTEC: str  # PillarAztec
    PILLAR_AZTEC_DAMAGED: str  # PillarAztecDamaged
    PILLAR_AZTEC_RUINED: str  # PillarAztecRuined
    PILLAR_HUGE_TECH: str  # PillarHugeTech
    PILLAR_TECHNO: str  # PillarTechno
    PISTOL: str  # Pistol
    PISTON: str  # Piston
    PITCHER: str  # Pitcher
    PLASMA_BALL: str  # PlasmaBall
    PLASMA_BALL1: str  # PlasmaBall1
    PLASMA_BALL2: str  # PlasmaBall2
    PLASMA_RIFLE: str  # PlasmaRifle
    PLATINUM_HELM: str  # PlatinumHelm
    PLAYER_CHUNK: str  # PlayerChunk
    PLAYER_PAWN: str  # PlayerPawn
    PLAYER_SPEED_TRAIL: str  # PlayerSpeedTrail
    POD: str  # Pod
    POD_GENERATOR: str  # PodGenerator
    POD_GOO: str  # PodGoo
    POINT_LIGHT: str  # PointLight
    POINT_LIGHT_ADDITIVE: str  # PointLightAdditive
    POINT_LIGHT_ATTENUATED: str  # PointLightAttenuated
    POINT_LIGHT_FLICKER: str  # PointLightFlicker
    POINT_LIGHT_FLICKER_ADDITIVE: str  # PointLightFlickerAdditive
    POINT_LIGHT_FLICKER_ATTENUATED: str  # PointLightFlickerAttenuated
    POINT_LIGHT_FLICKER_RANDOM: str  # PointLightFlickerRandom
    POINT_LIGHT_FLICKER_RANDOM_ADDITIVE: str  # PointLightFlickerRandomAdditive
    POINT_LIGHT_FLICKER_RANDOM_ATTENUATED: str  # PointLightFlickerRandomAttenuated
    POINT_LIGHT_FLICKER_RANDOM_SUBTRACTIVE: str  # PointLightFlickerRandomSubtractive
    POINT_LIGHT_FLICKER_SUBTRACTIVE: str  # PointLightFlickerSubtractive
    POINT_LIGHT_PULSE: str  # PointLightPulse
    POINT_LIGHT_PULSE_ADDITIVE: str  # PointLightPulseAdditive
    POINT_LIGHT_PULSE_ATTENUATED: str  # PointLightPulseAttenuated
    POINT_LIGHT_PULSE_SUBTRACTIVE: str  # PointLightPulseSubtractive
    POINT_LIGHT_SUBTRACTIVE: str  # PointLightSubtractive
    POINT_PULLER: str  # PointPuller
    POINT_PUSHER: str  # PointPusher
    POISON_BAG: str  # PoisonBag
    POISON_BOLT: str  # PoisonBolt
    POISON_BOLTS: str  # PoisonBolts
    POISON_CLOUD: str  # PoisonCloud
    POISON_DART: str  # PoisonDart
    POLE_LANTERN: str  # PoleLantern
    PORK_FX: str  # PorkFX
    POT: str  # Pot
    POTTED_TREE: str  # PottedTree
    POTTERY1: str  # Pottery1
    POTTERY2: str  # Pottery2
    POTTERY3: str  # Pottery3
    POTTERY_BIT: str  # PotteryBit
    POWER1_KEY: str  # Power1Key
    POWER2_KEY: str  # Power2Key
    POWER3_KEY: str  # Power3Key
    POWERUP: str  # Powerup
    POWERUP_GIVER: str  # PowerupGiver
    POWER_BUDDHA: str  # PowerBuddha
    POWER_COUPLING: str  # PowerCoupling
    POWER_CRYSTAL: str  # PowerCrystal
    POWER_DAMAGE: str  # PowerDamage
    POWER_DOUBLE_FIRING_SPEED: str  # PowerDoubleFiringSpeed
    POWER_DRAIN: str  # PowerDrain
    POWER_FLIGHT: str  # PowerFlight
    POWER_FRIGHTENER: str  # PowerFrightener
    POWER_GHOST: str  # PowerGhost
    POWER_HIGH_JUMP: str  # PowerHighJump
    POWER_INFINITE_AMMO: str  # PowerInfiniteAmmo
    POWER_INVISIBILITY: str  # PowerInvisibility
    POWER_INVULNERABLE: str  # PowerInvulnerable
    POWER_IRON_FEET: str  # PowerIronFeet
    POWER_LIGHT_AMP: str  # PowerLightAmp
    POWER_MASK: str  # PowerMask
    POWER_MINOTAUR: str  # PowerMinotaur
    POWER_MORPH: str  # PowerMorph
    POWER_PROTECTION: str  # PowerProtection
    POWER_REFLECTION: str  # PowerReflection
    POWER_REGENERATION: str  # PowerRegeneration
    POWER_SCANNER: str  # PowerScanner
    POWER_SHADOW: str  # PowerShadow
    POWER_SPEED: str  # PowerSpeed
    POWER_STRENGTH: str  # PowerStrength
    POWER_TARGETER: str  # PowerTargeter
    POWER_TIME_FREEZER: str  # PowerTimeFreezer
    POWER_TORCH: str  # PowerTorch
    POWER_WEAPON_LEVEL2: str  # PowerWeaponLevel2
    PRISON_KEY: str  # PrisonKey
    PRISON_PASS: str  # PrisonPass
    PROGRAMMER: str  # Programmer
    PROGRAMMER_BASE: str  # ProgrammerBase
    PROG_LEVEL_ENDER: str  # ProgLevelEnder
    PROJECTILE_BLADE: str  # ProjectileBlade
    PROPULSOR_MISSILE: str  # PropulsorMissile
    PROPULSOR_ZORCH: str  # PropulsorZorch
    PROPULSOR_ZORCH_PACK: str  # PropulsorZorchPack
    PUFFY: str  # Puffy
    PUNCH_DAGGER: str  # PunchDagger
    PUNCH_PUFF: str  # PunchPuff
    PURPLE_PARTICLE_FOUNTAIN: str  # PurpleParticleFountain
    PUZZLE_ITEM: str  # PuzzleItem
    PUZZ_BOOK1: str  # PuzzBook1
    PUZZ_BOOK2: str  # PuzzBook2
    PUZZ_C_WEAPON: str  # PuzzCWeapon
    PUZZ_FLAME_MASK: str  # PuzzFlameMask
    PUZZ_F_WEAPON: str  # PuzzFWeapon
    PUZZ_GEAR1: str  # PuzzGear1
    PUZZ_GEAR2: str  # PuzzGear2
    PUZZ_GEAR3: str  # PuzzGear3
    PUZZ_GEAR4: str  # PuzzGear4
    PUZZ_GEM_BIG: str  # PuzzGemBig
    PUZZ_GEM_BLUE1: str  # PuzzGemBlue1
    PUZZ_GEM_BLUE2: str  # PuzzGemBlue2
    PUZZ_GEM_GREEN1: str  # PuzzGemGreen1
    PUZZ_GEM_GREEN2: str  # PuzzGemGreen2
    PUZZ_GEM_RED: str  # PuzzGemRed
    PUZZ_M_WEAPON: str  # PuzzMWeapon
    PUZZ_SKULL: str  # PuzzSkull
    QUEST_ITEM: str  # QuestItem
    QUEST_ITEM1: str  # QuestItem1
    QUEST_ITEM10: str  # QuestItem10
    QUEST_ITEM11: str  # QuestItem11
    QUEST_ITEM12: str  # QuestItem12
    QUEST_ITEM13: str  # QuestItem13
    QUEST_ITEM14: str  # QuestItem14
    QUEST_ITEM15: str  # QuestItem15
    QUEST_ITEM16: str  # QuestItem16
    QUEST_ITEM17: str  # QuestItem17
    QUEST_ITEM18: str  # QuestItem18
    QUEST_ITEM19: str  # QuestItem19
    QUEST_ITEM2: str  # QuestItem2
    QUEST_ITEM20: str  # QuestItem20
    QUEST_ITEM21: str  # QuestItem21
    QUEST_ITEM22: str  # QuestItem22
    QUEST_ITEM23: str  # QuestItem23
    QUEST_ITEM24: str  # QuestItem24
    QUEST_ITEM25: str  # QuestItem25
    QUEST_ITEM26: str  # QuestItem26
    QUEST_ITEM27: str  # QuestItem27
    QUEST_ITEM28: str  # QuestItem28
    QUEST_ITEM29: str  # QuestItem29
    QUEST_ITEM3: str  # QuestItem3
    QUEST_ITEM30: str  # QuestItem30
    QUEST_ITEM31: str  # QuestItem31
    QUEST_ITEM4: str  # QuestItem4
    QUEST_ITEM5: str  # QuestItem5
    QUEST_ITEM6: str  # QuestItem6
    QUEST_ITEM7: str  # QuestItem7
    QUEST_ITEM8: str  # QuestItem8
    QUEST_ITEM9: str  # QuestItem9
    QUIETUS_DROP: str  # QuietusDrop
    RAD_SUIT: str  # RadSuit
    RAIN_PILLAR: str  # RainPillar
    RAIN_TRACKER: str  # RainTracker
    RAISE_ALARM: str  # RaiseAlarm
    RANDOM_SPAWNER: str  # RandomSpawner
    RAPID_ZORCHER: str  # RapidZorcher
    RAT_BUDDY: str  # RatBuddy
    REAL_GIBS: str  # RealGibs
    REAVER: str  # Reaver
    REBEL: str  # Rebel
    REBEL1: str  # Rebel1
    REBEL2: str  # Rebel2
    REBEL3: str  # Rebel3
    REBEL4: str  # Rebel4
    REBEL5: str  # Rebel5
    REBEL6: str  # Rebel6
    REBEL_BOOTS: str  # RebelBoots
    REBEL_HELMET: str  # RebelHelmet
    REBEL_SHIRT: str  # RebelShirt
    RED_AXE: str  # RedAxe
    RED_CARD: str  # RedCard
    RED_CRYSTAL_KEY: str  # RedCrystalKey
    RED_PARTICLE_FOUNTAIN: str  # RedParticleFountain
    RED_SKULL: str  # RedSkull
    RED_TORCH: str  # RedTorch
    REVENANT: str  # Revenant
    REVENANT_TRACER: str  # RevenantTracer
    REVENANT_TRACER_SMOKE: str  # RevenantTracerSmoke
    RIPPER: str  # RIPPER
    RIPPER_BALL: str  # RipperBall
    ROCK1: str  # Rock1
    ROCK2: str  # Rock2
    ROCK3: str  # Rock3
    ROCKET: str  # Rocket
    ROCKET_AMMO: str  # RocketAmmo
    ROCKET_BOX: str  # RocketBox
    ROCKET_LAUNCHER: str  # RocketLauncher
    ROCKET_SMOKE_TRAIL: str  # RocketSmokeTrail
    ROCKET_TRAIL: str  # RocketTrail
    RUBBLE1: str  # Rubble1
    RUBBLE2: str  # Rubble2
    RUBBLE3: str  # Rubble3
    RUBBLE4: str  # Rubble4
    RUBBLE5: str  # Rubble5
    RUBBLE6: str  # Rubble6
    RUBBLE7: str  # Rubble7
    RUBBLE8: str  # Rubble8
    SACRIFICED_GUY: str  # SacrificedGuy
    SCANNER: str  # Scanner
    SCORE_ITEM: str  # ScoreItem
    SCRIPTED_MARINE: str  # ScriptedMarine
    SECRET_TRIGGER: str  # SecretTrigger
    SECTOR_ACTION: str  # SectorAction
    SECTOR_FLAG_SETTER: str  # SectorFlagSetter
    SECTOR_POINT_LIGHT: str  # SectorPointLight
    SECTOR_POINT_LIGHT_ADDITIVE: str  # SectorPointLightAdditive
    SECTOR_POINT_LIGHT_ATTENUATED: str  # SectorPointLightAttenuated
    SECTOR_POINT_LIGHT_SUBTRACTIVE: str  # SectorPointLightSubtractive
    SECTOR_SILENCER: str  # SectorSilencer
    SECTOR_SPOT_LIGHT: str  # SectorSpotLight
    SECTOR_SPOT_LIGHT_ADDITIVE: str  # SectorSpotLightAdditive
    SECTOR_SPOT_LIGHT_ATTENUATED: str  # SectorSpotLightAttenuated
    SECTOR_SPOT_LIGHT_SUBTRACTIVE: str  # SectorSpotLightSubtractive
    SECURITY_CAMERA: str  # SecurityCamera
    SECURITY_KEY: str  # SecurityKey
    SEC_ACT_DAMAGE3_D: str  # SecActDamage3D
    SEC_ACT_DAMAGE_CEILING: str  # SecActDamageCeiling
    SEC_ACT_DAMAGE_FLOOR: str  # SecActDamageFloor
    SEC_ACT_DEATH3_D: str  # SecActDeath3D
    SEC_ACT_DEATH_CEILING: str  # SecActDeathCeiling
    SEC_ACT_DEATH_FLOOR: str  # SecActDeathFloor
    SEC_ACT_ENTER: str  # SecActEnter
    SEC_ACT_EXIT: str  # SecActExit
    SEC_ACT_EYES_ABOVE_C: str  # SecActEyesAboveC
    SEC_ACT_EYES_BELOW_C: str  # SecActEyesBelowC
    SEC_ACT_EYES_DIVE: str  # SecActEyesDive
    SEC_ACT_EYES_SURFACE: str  # SecActEyesSurface
    SEC_ACT_HIT_CEIL: str  # SecActHitCeil
    SEC_ACT_HIT_FAKE_FLOOR: str  # SecActHitFakeFloor
    SEC_ACT_HIT_FLOOR: str  # SecActHitFloor
    SEC_ACT_USE: str  # SecActUse
    SEC_ACT_USE_WALL: str  # SecActUseWall
    SENTINEL: str  # Sentinel
    SENTINEL_FX1: str  # SentinelFX1
    SENTINEL_FX2: str  # SentinelFX2
    SERPENT: str  # Serpent
    SERPENT_FX: str  # SerpentFX
    SERPENT_GIB1: str  # SerpentGib1
    SERPENT_GIB2: str  # SerpentGib2
    SERPENT_GIB3: str  # SerpentGib3
    SERPENT_HEAD: str  # SerpentHead
    SERPENT_LEADER: str  # SerpentLeader
    SERPENT_TORCH: str  # SerpentTorch
    SEVERED_HAND: str  # SeveredHand
    SG_SHARD0: str  # SGShard0
    SG_SHARD1: str  # SGShard1
    SG_SHARD2: str  # SGShard2
    SG_SHARD3: str  # SGShard3
    SG_SHARD4: str  # SGShard4
    SG_SHARD5: str  # SGShard5
    SG_SHARD6: str  # SGShard6
    SG_SHARD7: str  # SGShard7
    SG_SHARD8: str  # SGShard8
    SG_SHARD9: str  # SGShard9
    SHADOW_ARMOR: str  # ShadowArmor
    SHELL: str  # Shell
    SHELL_BOX: str  # ShellBox
    SHORT_BLUE_TORCH: str  # ShortBlueTorch
    SHORT_BUSH: str  # ShortBush
    SHORT_GREEN_COLUMN: str  # ShortGreenColumn
    SHORT_GREEN_TORCH: str  # ShortGreenTorch
    SHORT_RED_COLUMN: str  # ShortRedColumn
    SHORT_RED_TORCH: str  # ShortRedTorch
    SHOTGUN: str  # Shotgun
    SHOTGUN_GUY: str  # ShotgunGuy
    SIGIL: str  # Sigil
    SIGIL1: str  # Sigil1
    SIGIL2: str  # Sigil2
    SIGIL3: str  # Sigil3
    SIGIL4: str  # Sigil4
    SIGIL5: str  # Sigil5
    SIGIL_BANNER: str  # SigilBanner
    SILVER_KEY: str  # SilverKey
    SILVER_SHIELD: str  # SilverShield
    SKULL_COLUMN: str  # SkullColumn
    SKULL_HANG35: str  # SkullHang35
    SKULL_HANG45: str  # SkullHang45
    SKULL_HANG60: str  # SkullHang60
    SKULL_HANG70: str  # SkullHang70
    SKULL_ROD: str  # SkullRod
    SKULL_ROD_AMMO: str  # SkullRodAmmo
    SKULL_ROD_HEFTY: str  # SkullRodHefty
    SKULL_ROD_POWERED: str  # SkullRodPowered
    SKY_CAM_COMPAT: str  # SkyCamCompat
    SKY_PICKER: str  # SkyPicker
    SKY_VIEWPOINT: str  # SkyViewpoint
    SLIDESHOW_STARTER: str  # SlideshowStarter
    SLIME_CHUNK: str  # SlimeChunk
    SLIME_PROOF_SUIT: str  # SlimeProofSuit
    SLIME_REPELLENT: str  # SlimeRepellent
    SLIME_SPLASH: str  # SlimeSplash
    SLUDGE_CHUNK: str  # SludgeChunk
    SLUDGE_SPLASH: str  # SludgeSplash
    SMALL_BLOOD_POOL: str  # SmallBloodPool
    SMALL_PILLAR: str  # SmallPillar
    SMALL_TORCH_LIT: str  # SmallTorchLit
    SMALL_TORCH_UNLIT: str  # SmallTorchUnlit
    SNAKE: str  # Snake
    SNAKE_PROJ_A: str  # SnakeProjA
    SNAKE_PROJ_B: str  # SnakeProjB
    SNOUT: str  # Snout
    SNOUT_PUFF: str  # SnoutPuff
    SORCERER1: str  # Sorcerer1
    SORCERER2: str  # Sorcerer2
    SORCERER2_FX1: str  # Sorcerer2FX1
    SORCERER2_FX2: str  # Sorcerer2FX2
    SORCERER2_FX_SPARK: str  # Sorcerer2FXSpark
    SORCERER2_TELEFADE: str  # Sorcerer2Telefade
    SORCERER_FX1: str  # SorcererFX1
    SORC_BALL: str  # SorcBall
    SORC_BALL1: str  # SorcBall1
    SORC_BALL2: str  # SorcBall2
    SORC_BALL3: str  # SorcBall3
    SORC_FX1: str  # SorcFX1
    SORC_FX2: str  # SorcFX2
    SORC_FX2_T1: str  # SorcFX2T1
    SORC_FX3: str  # SorcFX3
    SORC_FX3_EXPLOSION: str  # SorcFX3Explosion
    SORC_FX4: str  # SorcFX4
    SORC_SPARK1: str  # SorcSpark1
    SOULSPHERE: str  # Soulsphere
    SOUND_ENVIRONMENT: str  # SoundEnvironment
    SOUND_SEQUENCE: str  # SoundSequence
    SOUND_SEQUENCE_SLOT: str  # SoundSequenceSlot
    SOUND_WATERFALL: str  # SoundWaterfall
    SOUND_WIND: str  # SoundWind
    SOUND_WIND_HEXEN: str  # SoundWindHexen
    SPARK: str  # Spark
    SPAWN_FIRE: str  # SpawnFire
    SPAWN_SHOT: str  # SpawnShot
    SPEAKER_ICON: str  # SpeakerIcon
    SPECIAL_SPOT: str  # SpecialSpot
    SPECTATOR_CAMERA: str  # SpectatorCamera
    SPECTRAL_LIGHTNING_BALL1: str  # SpectralLightningBall1
    SPECTRAL_LIGHTNING_BALL2: str  # SpectralLightningBall2
    SPECTRAL_LIGHTNING_BASE: str  # SpectralLightningBase
    SPECTRAL_LIGHTNING_BIG_BALL1: str  # SpectralLightningBigBall1
    SPECTRAL_LIGHTNING_BIG_BALL2: str  # SpectralLightningBigBall2
    SPECTRAL_LIGHTNING_BIG_V1: str  # SpectralLightningBigV1
    SPECTRAL_LIGHTNING_BIG_V2: str  # SpectralLightningBigV2
    SPECTRAL_LIGHTNING_DEATH1: str  # SpectralLightningDeath1
    SPECTRAL_LIGHTNING_DEATH2: str  # SpectralLightningDeath2
    SPECTRAL_LIGHTNING_DEATH_SHORT: str  # SpectralLightningDeathShort
    SPECTRAL_LIGHTNING_H1: str  # SpectralLightningH1
    SPECTRAL_LIGHTNING_H2: str  # SpectralLightningH2
    SPECTRAL_LIGHTNING_H3: str  # SpectralLightningH3
    SPECTRAL_LIGHTNING_H_TAIL: str  # SpectralLightningHTail
    SPECTRAL_LIGHTNING_SPOT: str  # SpectralLightningSpot
    SPECTRAL_LIGHTNING_V1: str  # SpectralLightningV1
    SPECTRAL_LIGHTNING_V2: str  # SpectralLightningV2
    SPECTRAL_MONSTER: str  # SpectralMonster
    SPECTRE: str  # Spectre
    SPIDER_MASTERMIND: str  # SpiderMastermind
    SPOT_LIGHT: str  # SpotLight
    SPOT_LIGHT_ADDITIVE: str  # SpotLightAdditive
    SPOT_LIGHT_ATTENUATED: str  # SpotLightAttenuated
    SPOT_LIGHT_FLICKER: str  # SpotLightFlicker
    SPOT_LIGHT_FLICKER_ADDITIVE: str  # SpotLightFlickerAdditive
    SPOT_LIGHT_FLICKER_ATTENUATED: str  # SpotLightFlickerAttenuated
    SPOT_LIGHT_FLICKER_RANDOM: str  # SpotLightFlickerRandom
    SPOT_LIGHT_FLICKER_RANDOM_ADDITIVE: str  # SpotLightFlickerRandomAdditive
    SPOT_LIGHT_FLICKER_RANDOM_ATTENUATED: str  # SpotLightFlickerRandomAttenuated
    SPOT_LIGHT_FLICKER_RANDOM_SUBTRACTIVE: str  # SpotLightFlickerRandomSubtractive
    SPOT_LIGHT_FLICKER_SUBTRACTIVE: str  # SpotLightFlickerSubtractive
    SPOT_LIGHT_PULSE: str  # SpotLightPulse
    SPOT_LIGHT_PULSE_ADDITIVE: str  # SpotLightPulseAdditive
    SPOT_LIGHT_PULSE_ATTENUATED: str  # SpotLightPulseAttenuated
    SPOT_LIGHT_PULSE_SUBTRACTIVE: str  # SpotLightPulseSubtractive
    SPOT_LIGHT_SUBTRACTIVE: str  # SpotLightSubtractive
    STACK_POINT: str  # StackPoint
    STAFF: str  # Staff
    STAFF_POWERED: str  # StaffPowered
    STAFF_PUFF: str  # StaffPuff
    STAFF_PUFF2: str  # StaffPuff2
    STALACTITE_LARGE: str  # StalactiteLarge
    STALACTITE_SMALL: str  # StalactiteSmall
    STALAGMITE: str  # Stalagmite
    STALAGMITE_LARGE: str  # StalagmiteLarge
    STALAGMITE_SMALL: str  # StalagmiteSmall
    STALAGTITE: str  # Stalagtite
    STALKER: str  # Stalker
    STATE_PROVIDER: str  # StateProvider
    STATUE: str  # Statue
    STATUE_RUINED: str  # StatueRuined
    STEALTH_ARACHNOTRON: str  # StealthArachnotron
    STEALTH_ARCHVILE: str  # StealthArchvile
    STEALTH_BARON: str  # StealthBaron
    STEALTH_CACODEMON: str  # StealthCacodemon
    STEALTH_CHAINGUN_GUY: str  # StealthChaingunGuy
    STEALTH_DEMON: str  # StealthDemon
    STEALTH_DOOM_IMP: str  # StealthDoomImp
    STEALTH_FATSO: str  # StealthFatso
    STEALTH_HELL_KNIGHT: str  # StealthHellKnight
    STEALTH_REVENANT: str  # StealthRevenant
    STEALTH_SHOTGUN_GUY: str  # StealthShotgunGuy
    STEALTH_ZOMBIE_MAN: str  # StealthZombieMan
    STICK_IN_WATER: str  # StickInWater
    STIMPACK: str  # Stimpack
    STOOL: str  # Stool
    STRIFE_BISHOP: str  # StrifeBishop
    STRIFE_BURNING_BARREL: str  # StrifeBurningBarrel
    STRIFE_CANDELABRA: str  # StrifeCandelabra
    STRIFE_CROSSBOW: str  # StrifeCrossbow
    STRIFE_CROSSBOW2: str  # StrifeCrossbow2
    STRIFE_GRENADE_LAUNCHER: str  # StrifeGrenadeLauncher
    STRIFE_GRENADE_LAUNCHER2: str  # StrifeGrenadeLauncher2
    STRIFE_HUMANOID: str  # StrifeHumanoid
    STRIFE_KEY: str  # StrifeKey
    STRIFE_MAP: str  # StrifeMap
    STRIFE_PLAYER: str  # StrifePlayer
    STRIFE_PUFF: str  # StrifePuff
    STRIFE_SPARK: str  # StrifeSpark
    STRIFE_WEAPON: str  # StrifeWeapon
    SUMMONING_DOLL: str  # SummoningDoll
    SUPERCHARGE_BREAKFAST: str  # SuperchargeBreakfast
    SUPER_BOOTSPORK: str  # SuperBootspork
    SUPER_CHEX_ARMOR: str  # SuperChexArmor
    SUPER_LARGE_ZORCHER: str  # SuperLargeZorcher
    SUPER_MAP: str  # SuperMap
    SUPER_SHOTGUN: str  # SuperShotgun
    SURGERY_CRAB: str  # SurgeryCrab
    SURGERY_KIT: str  # SurgeryKit
    SVE_BLUE_CHALICE: str  # SVEBlueChalice
    SVE_FLAG_SPOT_BLUE: str  # SVEFlagSpotBlue
    SVE_FLAG_SPOT_RED: str  # SVEFlagSpotRed
    SVE_LIGHT: str  # SVELight
    SVE_LIGHT7958: str  # SVELight7958
    SVE_LIGHT7959: str  # SVELight7959
    SVE_LIGHT7960: str  # SVELight7960
    SVE_LIGHT7961: str  # SVELight7961
    SVE_LIGHT7962: str  # SVELight7962
    SVE_LIGHT7964: str  # SVELight7964
    SVE_LIGHT7965: str  # SVELight7965
    SVE_LIGHT7971: str  # SVELight7971
    SVE_LIGHT7972: str  # SVELight7972
    SVE_LIGHT7973: str  # SVELight7973
    SVE_LIGHT7974: str  # SVELight7974
    SVE_OPEN_DOOR225: str  # SVEOpenDoor225
    SVE_ORE_SPAWNER: str  # SVEOreSpawner
    SVE_TALISMAN_BLUE: str  # SVETalismanBlue
    SVE_TALISMAN_GREEN: str  # SVETalismanGreen
    SVE_TALISMAN_POWERUP: str  # SVETalismanPowerup
    SVE_TALISMAN_RED: str  # SVETalismanRed
    SWITCHABLE_DECORATION: str  # SwitchableDecoration
    SWITCHING_DECORATION: str  # SwitchingDecoration
    S_ROCK1: str  # SRock1
    S_ROCK2: str  # SRock2
    S_ROCK3: str  # SRock3
    S_ROCK4: str  # SRock4
    S_STALACTITE_BIG: str  # SStalactiteBig
    S_STALACTITE_SMALL: str  # SStalactiteSmall
    S_STALAGMITE_BIG: str  # SStalagmiteBig
    S_STALAGMITE_SMALL: str  # SStalagmiteSmall
    TABLE_SHIT1: str  # TableShit1
    TABLE_SHIT10: str  # TableShit10
    TABLE_SHIT2: str  # TableShit2
    TABLE_SHIT3: str  # TableShit3
    TABLE_SHIT4: str  # TableShit4
    TABLE_SHIT5: str  # TableShit5
    TABLE_SHIT6: str  # TableShit6
    TABLE_SHIT7: str  # TableShit7
    TABLE_SHIT8: str  # TableShit8
    TABLE_SHIT9: str  # TableShit9
    TALL_BUSH: str  # TallBush
    TALL_GREEN_COLUMN: str  # TallGreenColumn
    TALL_RED_COLUMN: str  # TallRedColumn
    TANK1: str  # Tank1
    TANK2: str  # Tank2
    TANK3: str  # Tank3
    TANK4: str  # Tank4
    TANK5: str  # Tank5
    TANK6: str  # Tank6
    TARGETER: str  # Targeter
    TARGET_PRACTICE: str  # TargetPractice
    TECH_LAMP: str  # TechLamp
    TECH_LAMP2: str  # TechLamp2
    TECH_LAMP_BRASS: str  # TechLampBrass
    TECH_LAMP_SILVER: str  # TechLampSilver
    TECH_PILLAR: str  # TechPillar
    TELEPORTER_BEACON: str  # TeleporterBeacon
    TELEPORT_DEST: str  # TeleportDest
    TELEPORT_DEST2: str  # TeleportDest2
    TELEPORT_DEST3: str  # TeleportDest3
    TELEPORT_FOG: str  # TeleportFog
    TELEPORT_SWIRL: str  # TeleportSwirl
    TELE_GLITTER1: str  # TeleGlitter1
    TELE_GLITTER2: str  # TeleGlitter2
    TELE_GLITTER_GENERATOR1: str  # TeleGlitterGenerator1
    TELE_GLITTER_GENERATOR2: str  # TeleGlitterGenerator2
    TELE_SMOKE: str  # TeleSmoke
    TEL_OTHER_FX1: str  # TelOtherFX1
    TEL_OTHER_FX2: str  # TelOtherFX2
    TEL_OTHER_FX3: str  # TelOtherFX3
    TEL_OTHER_FX4: str  # TelOtherFX4
    TEL_OTHER_FX5: str  # TelOtherFX5
    TEMPLAR: str  # Templar
    THROWING_BOMB: str  # ThrowingBomb
    THRUST_FLOOR: str  # ThrustFloor
    THRUST_FLOOR_DOWN: str  # ThrustFloorDown
    THRUST_FLOOR_UP: str  # ThrustFloorUp
    TORCH_TREE: str  # TorchTree
    TRAY: str  # Tray
    TREE_DESTRUCTIBLE: str  # TreeDestructible
    TREE_STUB: str  # TreeStub
    TUB: str  # Tub
    UNHOLY_BIBLE: str  # UnholyBible
    UNKNOWN: str  # Unknown
    UPGRADE_ACCURACY: str  # UpgradeAccuracy
    UPGRADE_STAMINA: str  # UpgradeStamina
    UPPER_STACK_LOOK_ONLY: str  # UpperStackLookOnly
    VAVOOM_LIGHT: str  # VavoomLight
    VAVOOM_LIGHT_COLOR: str  # VavoomLightColor
    VAVOOM_LIGHT_WHITE: str  # VavoomLightWhite
    VOLCANO: str  # Volcano
    VOLCANO_BLAST: str  # VolcanoBlast
    VOLCANO_T_BLAST: str  # VolcanoTBlast
    WALL_TORCH: str  # WallTorch
    WAREHOUSE_KEY: str  # WarehouseKey
    WATERFALL_SPLASH: str  # WaterfallSplash
    WATERZONE: str  # Waterzone
    WATER_BOTTLE: str  # WaterBottle
    WATER_DRIP: str  # WaterDrip
    WATER_DROP_ON_FLOOR: str  # WaterDropOnFloor
    WATER_FOUNTAIN: str  # WaterFountain
    WATER_SPLASH: str  # WaterSplash
    WATER_SPLASH_BASE: str  # WaterSplashBase
    WEAPON: str  # Weapon
    WEAPON_GIVER: str  # WeaponGiver
    WEAPON_HOLDER: str  # WeaponHolder
    WEAPON_PIECE: str  # WeaponPiece
    WEAPON_SMITH: str  # WeaponSmith
    WHIRLWIND: str  # Whirlwind
    WHITE_PARTICLE_FOUNTAIN: str  # WhiteParticleFountain
    WIZARD: str  # Wizard
    WIZARD_FX1: str  # WizardFX1
    WOLFENSTEIN_SS: str  # WolfensteinSS
    WOODEN_BARREL: str  # WoodenBarrel
    WRAITH: str  # Wraith
    WRAITHVERGE_DROP: str  # WraithvergeDrop
    WRAITH_BURIED: str  # WraithBuried
    WRAITH_FX1: str  # WraithFX1
    WRAITH_FX2: str  # WraithFX2
    WRAITH_FX3: str  # WraithFX3
    WRAITH_FX4: str  # WraithFX4
    WRAITH_FX5: str  # WraithFX5
    YELLOW_CARD: str  # YellowCard
    YELLOW_PARTICLE_FOUNTAIN: str  # YellowParticleFountain
    YELLOW_SKULL: str  # YellowSkull
    ZOMBIE: str  # Zombie
    ZOMBIEMAN: str  # Zombieman
    ZOMBIE_SPAWNER: str  # ZombieSpawner
    ZORCHPACK: str  # Zorchpack
    ZORCH_PROPULSOR: str  # ZorchPropulsor
    Z_ARMOR_CHUNK: str  # ZArmorChunk
    Z_BANNER_TATTERED: str  # ZBannerTattered
    Z_BARREL: str  # ZBarrel
    Z_BELL: str  # ZBell
    Z_BLUE_CANDLE: str  # ZBlueCandle
    Z_BRIDGE: str  # ZBridge
    Z_BUCKET: str  # ZBucket
    Z_CANDLE: str  # ZCandle
    Z_CAULDRON: str  # ZCauldron
    Z_CAULDRON_UNLIT: str  # ZCauldronUnlit
    Z_CHAIN_BIT32: str  # ZChainBit32
    Z_CHAIN_BIT64: str  # ZChainBit64
    Z_CHAIN_END_HEART: str  # ZChainEndHeart
    Z_CHAIN_END_HOOK1: str  # ZChainEndHook1
    Z_CHAIN_END_HOOK2: str  # ZChainEndHook2
    Z_CHAIN_END_SKULL: str  # ZChainEndSkull
    Z_CHAIN_END_SPIKE: str  # ZChainEndSpike
    Z_CHANDELIER: str  # ZChandelier
    Z_CHANDELIER_UNLIT: str  # ZChandelierUnlit
    Z_CORPSE_HANGING: str  # ZCorpseHanging
    Z_CORPSE_KABOB: str  # ZCorpseKabob
    Z_CORPSE_LYNCHED: str  # ZCorpseLynched
    Z_CORPSE_LYNCHED_NO_HEART: str  # ZCorpseLynchedNoHeart
    Z_CORPSE_SITTING: str  # ZCorpseSitting
    Z_CORPSE_SLEEPING: str  # ZCorpseSleeping
    Z_FIRE_BULL: str  # ZFireBull
    Z_FIRE_BULL_UNLIT: str  # ZFireBullUnlit
    Z_GEM_PEDESTAL: str  # ZGemPedestal
    Z_IRON_MAIDEN: str  # ZIronMaiden
    Z_LOG: str  # ZLog
    Z_MOSS_CEILING1: str  # ZMossCeiling1
    Z_MOSS_CEILING2: str  # ZMossCeiling2
    Z_POISON_SHROOM: str  # ZPoisonShroom
    Z_ROCK1: str  # ZRock1
    Z_ROCK2: str  # ZRock2
    Z_ROCK3: str  # ZRock3
    Z_ROCK4: str  # ZRock4
    Z_ROCK_BLACK: str  # ZRockBlack
    Z_ROCK_BROWN1: str  # ZRockBrown1
    Z_ROCK_BROWN2: str  # ZRockBrown2
    Z_RUBBLE1: str  # ZRubble1
    Z_RUBBLE2: str  # ZRubble2
    Z_RUBBLE3: str  # ZRubble3
    Z_SHROOM_LARGE1: str  # ZShroomLarge1
    Z_SHROOM_LARGE2: str  # ZShroomLarge2
    Z_SHROOM_LARGE3: str  # ZShroomLarge3
    Z_SHROOM_SMALL1: str  # ZShroomSmall1
    Z_SHROOM_SMALL2: str  # ZShroomSmall2
    Z_SHROOM_SMALL3: str  # ZShroomSmall3
    Z_SHROOM_SMALL4: str  # ZShroomSmall4
    Z_SHROOM_SMALL5: str  # ZShroomSmall5
    Z_SHRUB1: str  # ZShrub1
    Z_SHRUB2: str  # ZShrub2
    Z_STALACTITE_ICE_LARGE: str  # ZStalactiteIceLarge
    Z_STALACTITE_ICE_MEDIUM: str  # ZStalactiteIceMedium
    Z_STALACTITE_ICE_SMALL: str  # ZStalactiteIceSmall
    Z_STALACTITE_ICE_TINY: str  # ZStalactiteIceTiny
    Z_STALACTITE_LARGE: str  # ZStalactiteLarge
    Z_STALACTITE_MEDIUM: str  # ZStalactiteMedium
    Z_STALACTITE_SMALL: str  # ZStalactiteSmall
    Z_STALAGMITE_ICE_LARGE: str  # ZStalagmiteIceLarge
    Z_STALAGMITE_ICE_MEDIUM: str  # ZStalagmiteIceMedium
    Z_STALAGMITE_ICE_SMALL: str  # ZStalagmiteIceSmall
    Z_STALAGMITE_ICE_TINY: str  # ZStalagmiteIceTiny
    Z_STALAGMITE_LARGE: str  # ZStalagmiteLarge
    Z_STALAGMITE_MEDIUM: str  # ZStalagmiteMedium
    Z_STALAGMITE_PILLAR: str  # ZStalagmitePillar
    Z_STALAGMITE_SMALL: str  # ZStalagmiteSmall
    Z_STATUE_GARGOYLE_BLUE_SHORT: str  # ZStatueGargoyleBlueShort
    Z_STATUE_GARGOYLE_BLUE_TALL: str  # ZStatueGargoyleBlueTall
    Z_STATUE_GARGOYLE_DARK_RED_SHORT: str  # ZStatueGargoyleDarkRedShort
    Z_STATUE_GARGOYLE_DARK_RED_TALL: str  # ZStatueGargoyleDarkRedTall
    Z_STATUE_GARGOYLE_GREEN_SHORT: str  # ZStatueGargoyleGreenShort
    Z_STATUE_GARGOYLE_GREEN_TALL: str  # ZStatueGargoyleGreenTall
    Z_STATUE_GARGOYLE_RED_SHORT: str  # ZStatueGargoyleRedShort
    Z_STATUE_GARGOYLE_RED_TALL: str  # ZStatueGargoyleRedTall
    Z_STATUE_GARGOYLE_RUST_SHORT: str  # ZStatueGargoyleRustShort
    Z_STATUE_GARGOYLE_RUST_TALL: str  # ZStatueGargoyleRustTall
    Z_STATUE_GARGOYLE_STRIPE_TALL: str  # ZStatueGargoyleStripeTall
    Z_STATUE_GARGOYLE_TAN_SHORT: str  # ZStatueGargoyleTanShort
    Z_STATUE_GARGOYLE_TAN_TALL: str  # ZStatueGargoyleTanTall
    Z_STUMP_BARE: str  # ZStumpBare
    Z_STUMP_BURNED: str  # ZStumpBurned
    Z_STUMP_SWAMP1: str  # ZStumpSwamp1
    Z_STUMP_SWAMP2: str  # ZStumpSwamp2
    Z_SUIT_OF_ARMOR: str  # ZSuitOfArmor
    Z_SWAMP_VINE: str  # ZSwampVine
    Z_TOMBSTONE_BIG_CROSS: str  # ZTombstoneBigCross
    Z_TOMBSTONE_BRIAN_P: str  # ZTombstoneBrianP
    Z_TOMBSTONE_BRIAN_R: str  # ZTombstoneBrianR
    Z_TOMBSTONE_CROSS_CIRCLE: str  # ZTombstoneCrossCircle
    Z_TOMBSTONE_RIP: str  # ZTombstoneRIP
    Z_TOMBSTONE_SHANE: str  # ZTombstoneShane
    Z_TOMBSTONE_SMALL_CROSS: str  # ZTombstoneSmallCross
    Z_TREE: str  # ZTree
    Z_TREE_DEAD: str  # ZTreeDead
    Z_TREE_GNARLED1: str  # ZTreeGnarled1
    Z_TREE_GNARLED2: str  # ZTreeGnarled2
    Z_TREE_LARGE1: str  # ZTreeLarge1
    Z_TREE_LARGE2: str  # ZTreeLarge2
    Z_TREE_SWAMP120: str  # ZTreeSwamp120
    Z_TREE_SWAMP150: str  # ZTreeSwamp150
    Z_TWINED_TORCH: str  # ZTwinedTorch
    Z_TWINED_TORCH_UNLIT: str  # ZTwinedTorchUnlit
    Z_VASE_PILLAR: str  # ZVasePillar
    Z_WALL_TORCH: str  # ZWallTorch
    Z_WALL_TORCH_UNLIT: str  # ZWallTorchUnlit
    Z_WINGED_STATUE: str  # ZWingedStatue
    Z_WINGED_STATUE_NO_SKULL: str  # ZWingedStatueNoSkull
    Z_XMAS_TREE: str  # ZXmasTree
    # @@GENERATED ACTOR CONSTANTS END@@

actors: _ActorsRegistry
