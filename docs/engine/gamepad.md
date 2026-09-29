# Gamepad Support

BiasedDoom supports modern gamepads out of the box on every platform, with a
classic-Doom default layout (no vertical aiming), selectable layout presets,
optional gyro look on supported controllers, and per-weapon haptic feedback.

## Platform support

| Platform | Backend | Notes |
|---|---|---|
| Linux | SDL GameController | Standard mappings for DualSense, DualShock 4, Xbox, Switch Pro and most generic pads; rumble (including trigger motors where the device has them); hotplug; gyro on DS4/DualSense. |
| Windows | XInput | Xbox-class controllers, full standard mapping + rumble. |
| Windows | DirectInput | Generic controllers. DualShock 4 (`054C:05C4`, `054C:09CC`) and DualSense (`054C:0CE6`, `054C:0DF2`) are detected by VID/PID and get the same standard pad mapping as everywhere else — sticks, analog L2/R2 triggers, d-pad, all buttons. Other devices use heuristic axis ordering. |
| Windows | Raw PS2 | Legacy PS2-controller USB adapters. |
| macOS | IOKit (Cocoa) or SDL | Generic HID pads; SDL builds get the GameController path. No rumble on IOKit. |

The DirectInput DualShock 4/DualSense mapping is implemented from publicly
documented HID layouts and is marked *hardware-verification pending*; unknown
devices always fall back to the previous heuristics, so nothing else changes.

## Default layout (Classic)

The defaults are tuned for the classic Doom experience: **aiming stays on the
horizontal plane** — there is no vertical look and no jump bound.

| Input | Action | Input | Action |
|---|---|---|---|
| Left stick | Move / strafe (analog) | Right stick X | Turn (analog) |
| Right stick Y | *(unbound — classic)* | RT | Fire |
| LT | Use / open | LB / RB | Previous / next weapon |
| Cross (A) | Use | Circle (B) | Run |
| Square (X) | *(unbound)* | Triangle (Y) | *(unbound, for mods)* |
| D-pad up | Automap | D-pad down | Use inventory item |
| D-pad left/right | Inventory prev/next | Start | Main menu |
| Back | Pause | L3 | Crouch |
| R3 | Center view | | |

Inventory binds are inert in Doom and active in Heretic/Hexen/Strife.
Keyboard/mouse bindings are unaffected — everything is additive.

## Layout presets

Options → Joystick Options → **Gamepad Layout**, or the console:

```
gamepadlayout 1   "Classic"        — the defaults above
gamepadlayout 2   "Classic + Move" — right stick Y moves forward/back (Doom 64 style)
gamepadlayout 3   "Modern"         — right stick freelook + jump on Triangle/Y
gamepadlayout 0   "Custom"         — your own bindings (status only)
```

A preset rewrites only the bindings it manages (sticks, triggers, shoulders,
face buttons, d-pad, Start/Back, L3/R3); everything else you bound is kept.
The cvar `joy_padlayout` records the last choice.

Presets also keep the aiming model coherent with the layout: choosing
**Classic** or **Classic + Move** automatically locks freelook off
(`freelook false`, `sv_freelook 1`) so the view stays on the horizon and the
engine restores vanilla vertical autoaim — shots snap to taller or elevated
targets on their own, which is what makes the no-aiming play style work.
Choosing **Modern** turns freelook back on (`freelook true`, `sv_freelook 2`).
This is a one-shot apply: changing freelook manually afterwards is respected
until you switch presets again.

The vertical autoaim itself is vanilla-faithful: instead of stopping at a
fixed 35° cone, a miss makes the engine widen the slope search (35° → 50° →
65° → 80°), exactly the way classic Doom keeps reaching higher and lower
targets — so ledges and flying monsters stay hittable without ever touching
a pitch control. Monsters get the same expanding search, as in vanilla. When
freelook is disallowed the view pitch is also pinned to the horizon every
tic, the way vanilla Doom has no vertical looking at all — mods that tilt
the view for recoil (e.g. Brutal Doom) can no longer leave the camera stuck
off-level, which would otherwise skew the autoaim cone.

### Mods that disable autoaim (Brutal Doom and friends)

Some mods flag their entire arsenal `+WEAPON.NOAUTOAIM` to enforce manual
aiming — Brutal Doom does this on its `BrutalWeapon` base class. The classic
layouts additionally set the opt-in cvar `bd_classic_autoaim` (default
`false`; Classic/Classic + Move set it `true`, Modern sets it `false`), which
makes the engine ignore weapon `NOAUTOAIM` flags and aim anyway. With a
classic layout this restores full vanilla vertical autoaim under Brutal
Doom; leave the cvar off if you prefer the mod's intended free-aim behavior.
(BD's own *Purist* player class ships `-WEAPON.NOAUTOAIM` weapon variants,
so it gets autoaim even without this cvar.)

| CVar | Default | Purpose |
|---|---|---|
| `bd_classic_autoaim` | `false` | Treat weapons as autoaim-capable even when a mod marks them `NOAUTOAIM`. Set automatically by the gamepad layout presets. |

## Gyro look (DualSense / DualShock 4)

Off by default. Enable in Joystick Options or with cvars:

| CVar | Default | Purpose |
|---|---|---|
| `joy_gyro_look` | `false` | Use the controller gyroscope for looking. |
| `joy_gyro_sensitivity_yaw` | `1.0` | Gyro turn sensitivity. |
| `joy_gyro_sensitivity_pitch` | `0.6` | Gyro pitch sensitivity. |
| `joy_gyro_invert_yaw` | `false` | Invert gyro turning. |
| `joy_gyro_invert_pitch` | `false` | Invert gyro pitch. |

Pitch follows the usual freelook rules, so with freelook disabled the gyro
only turns — the classic experience is untouched. Gyro is available on the SDL
backend (Linux, and macOS SDL builds) on controllers that expose a gyroscope
through SDL; it is a no-op everywhere else.

## Haptics

Rumble is driven by game sounds through SNDINFO `$rumble`/`$rumbledef`
declarations (see `wadsrc/static/sndinfo.txt` and the per-game filters) plus
direct player events:

- **Weapons**: every Doom weapon has a curated feel — pistol snaps with a
  trigger tick, the shotgun thumps deep, the super shotgun booms on every
  motor, the chaingun chatters per shot, the rocket launcher whooshes low,
  plasma crackles high, the BFG sweeps long and heavy, and the chainsaw
  growls while it grinds. Trigger-motor channels are used on pads that have
  them (e.g. Xbox impulse triggers) and are harmless elsewhere.
- **Damage**: feedback scales with the actual damage taken — chip damage
  taps lightly, heavy hits jolt — implemented natively in `P_DamageMobj`.
  Death rumbles at full intensity.
- **World/menus**: teleports, secrets, quakes, landings and menu navigation
  have their own mappings.

Control it in Options → Joystick Options → Haptics, or with the
`haptics_strength`, `haptics_strength_lf/_hf/_lt/_rt`, `haptics_do_menus`,
`haptics_do_world`, `haptics_do_damage`, `haptics_do_action`, and
`haptics_compat` cvars. Mods can define their own `$rumbledef` types and hook
`PlayerHurtMakeRumble`/`PlayerDiedMakeRumble` and friends in ZScript.

## Per-device tuning

Options → Joystick Options → Configure (per device): sensitivity, per-axis
deadzone, scale, digital threshold, and response curve (linear/quadratic/
cubic/custom bezier). The same settings are scriptable with the `gamepad`
console command, e.g. `gamepad deadzone 0.2 0.15`. Devices persist their
configuration in the INI under `Joy:<identifier>` sections.

## Testing

`tools/test-gamepad.sh --iwad /path/to/doom2.wad` verifies the default
bindings, the layout presets, the cvars, and the haptics content in a real
engine run. Device-level behavior (stick feel, gyro, rumble intensity)
requires physical hardware.
