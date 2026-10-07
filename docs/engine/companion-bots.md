# Companion Bots

BiasedDoom can add friendly companion bots to a local cooperative game or to a
hosted cooperative session. They work on ordinary maps as well as Doom-family
procedural runs; a generated map is not a separate bot mode.

> **Procedural scope** — Procedural generation currently supports Doom and Doom
> II IWAD families only. Heretic and Hexen procedural generation is explicitly
> deferred for this release, because those games need their own actor,
> inventory, key, texture, and map-action grammars.

## Build a squad, one companion at a time

Open **Options → Gameplay → Companion Bots** or **Options → Multiplayer →
Companion Bots**. This is the only in-menu companion configuration surface.
**Procedural Game** setup does not configure companions: it simply uses the
same saved squad when the generated map starts. Keeping it out of the recipe
screen prevents a second setup path from accidentally building a duplicate
squad.

Enabled profiles are the complete desired co-op squad. When at least one
profile is enabled, the engine does not merge it with an old `-bots` startup
list: stale unprofiled co-op bots are retired and only the selected profiles
are realized. Start a run with no enabled profiles when the intention is to use
the classic startup-list behavior instead.

Every new roster starts at **zero** companions. Select **Add Companion…** to
open a draft before any default identity is resolved or a bot is deployed. In
that draft, choose the companion's:

- **Identity** — select an available roster name, or leave it as “choose
  available on deploy.”
- **Skin override** — select a loaded skin or retain the player class default.
- **Appearance style** — preserve the roster appearance or choose a stable
  palette colour.
- **Combat skill** — preserve the roster personality or choose Gentle helper,
  Balanced, or Veteran.

Choose **Add companion to squad** to explicitly deploy the draft. If the
identity was left open, the game resolves one available identity then and
saves it to that companion's profile. During a live deathmatch, the action is
instead **Save companion to squad**: it commits the same profile but never
spawns a companion into that competitive level. The saved squad joins the next
eligible cooperative map. Cancelling or backing out of a draft discards it; it
does not add a bot or alter the existing squad. Repeat the flow for every
companion, up to the available capacity.

**Add Random Companion…** opens that same reviewable draft, but fills every
choice first: an available roster identity, either a loaded skin or the class
default, a visible appearance style, and one of the three helper skill levels.
**Randomize this configuration** re-rolls those four fields before deployment.
Neither action enables a profile or spawns a pawn until **Add companion to
squad** is chosen. The host commits each random result as one replicated
profile transaction, so guests see the exact same result and a busy network
tick cannot leave a half-randomized companion behind. It resolves into the
saved companion profile rather than the procedural recipe, so it never changes
generated-map determinism or a shared run's seed output.

Each deployed companion has a stable numbered profile and saved identity. The
same person keeps that identity, skin choice, appearance style, and combat
skill across ordinary and procedural map transitions while the identity remains
in the active roster. If a mod or edited `bots.cfg` removes a saved name, the
engine warns and resolves a saved fallback identity before it queues that
companion, rather than blocking the rest of the squad. Editing an active member
prepares its next join; it does not rewrite a live bot's user information in
place.

**Browse identity library** is read-only and shows which names are available,
joining, or active. A name already reserved by a profile cannot be picked by a
second draft. This prevents the menu from offering a choice that cannot join.

Select a deployed companion in the roster to inspect and customize that exact
profile. **Remove this companion** asks for confirmation, then removes only the
selected profile, including an in-flight join, and clears that profile's saved
configuration. The other
companions retain their identities and positions. **Dismiss all companions** is
the deliberate reset: after confirmation it removes the active/queued squad,
clears every saved profile, and returns the roster to zero. Removal and
dismissal are atomic roster transactions: if a crowded network tic cannot
carry every membership and appearance change, nothing is locally committed and
the host is told to retry rather than leaving a hidden saved identity behind.

## Where companions can join

Prepare a squad before starting a map, or add it while an ordinary cooperative
level or a procedural run is active. The host-owned roster reconciles with the
current map, so a prepared squad follows normal level transitions without
having to be rebuilt in each map.

Companions occupy normal cooperative player slots and use the map's ordinary
co-op placement. Human players and companions share the classic P1–P8 contract:

```
min(7, 8 - connected human players)
```

is the upper bound for companions. A smaller active identity roster can reduce
it further. When all co-op slots are occupied, an enabled profile is shown as
waiting rather than replacing a human player or silently dropping another
companion. A map can separately reject a companion when it has no
collision-clear co-op spawn location: that profile remains prepared and the
engine stops retrying the same unsafe placement until the next map transition.

The same capacity rule applies to normal maps and generated maps. Generated
maps reserve protected P1–P8 pads, while ordinary maps use the engine's normal
cooperative placement and fallback behavior. A procedural run consumes the
centrally configured roster; it has no companion-specific settings of its own.

An otherwise single-player ordinary map temporarily uses its normal
cooperative rules while a companion is present, whether the squad was prepared
before the map started or added partway through it. Removing the final
companion restores local single-player mode when no network, deathmatch, or
other human player owns the multiplayer session. That local ownership survives
save/load and hub restoration, so clearing the squad does not leave a solo run
silently flagged as co-op.

Saved companion profiles are cooperative-only. They never enter a live
deathmatch session or consume its bot slots. The roster can still be reviewed,
edited, and saved during a live deathmatch for a later co-op run; the Companion
Bots menu labels that action as saving rather than deploying.
Classic deathmatch bots remain separate: they keep their usual transient
behavior and persist across deathmatch map changes without being removed or
replaced by the saved cooperative squad.

## Authority and multiplayer visibility

In local play, the settings controller manages the roster. In a network game,
only the host/settings controller can begin or deploy a draft, randomize a
draft, edit a profile, remove a profile, dismiss the squad, or change companion
settings. Guests can
open the same menus and inspect the squad and its statuses, but the controls
are intentionally read-only and their mutation requests are rejected.

Profiles are archived locally and replicated as server information. This keeps
the host's saved squad consistent for peers without giving a guest a local,
conflicting version of the roster. Each profile edit is queued before it is
shown locally. A busy network tick therefore leaves the current value intact
instead of making the host and guests disagree; the UI simply retains the
previous choice until the host retries.

Companion menu actions and co-op companion commands are intentionally inert
while recording or playing back a demo. Archived settings may still be present,
but a configured demo never synthesizes companion join events into its recorded
timeline.

## Console and compatibility controls

The profile menu is the recommended way to create and edit a squad. The legacy
count CVar remains available for existing configs and scripts, while the
one-shot console commands now create and remove persistent companion profiles.
They are immediate actions rather than a second version of the draft UI.

| Command or CVar | Use |
|---|---|
| `bot_companion_count <0-7>` | Compatibility count view, defaulting to `0`. Setting it creates a sequential first-*N* profile squad on the local controller or host; it cannot express a deliberately sparse roster or choose a profile to remove. It is a local archived projection; the replicated membership mask remains authoritative. Use the roster menu for those operations. |
| `bot_companion_enabled_mask` | Menu-managed archived and replicated membership mask for the seven stable profiles. It permits a sparse squad (for example, profiles 1 and 4) and is the roster's source of truth; use the menu or supported commands instead of editing the mask directly. |
| `bot_companion_respawn <0/1>` | Controls whether cooperative companions use the normal in-level respawn flow after death. It defaults to `1`; deathmatch retains its legacy bot-respawn behavior. |
| `bot_companion_name1` … `bot_companion_name7` | Archived, replicated identity storage for the seven profiles. The menu validates availability and records a resolved identity on deployment. |
| `bot_companion_skin1` … `bot_companion_skin7` | Saved optional skin overrides. An empty value uses the selected bot/player class's normal skin. |
| `bot_companion_style1` … `bot_companion_style7` | Saved appearance-style values: `0` preserves the roster style and `1`–`11` select a stable player-colour palette. |
| `bot_companion_skill1` … `bot_companion_skill7` | Saved combat-skill values: `0` preserves the roster personality; `1`, `2`, and `3` select Gentle helper, Balanced, and Veteran. |
| `addcompanion [name]` | Active non-deathmatch level command that creates and deploys one persistent companion profile. With no name it resolves and saves an available default identity; with a name it searches the active roster case-insensitively. It does not open a configuration draft, so use **Add Companion…** when identity, skin, style, and skill must be chosen first. |
| `addbot [name]` | Legacy active-level command. In co-op it creates the same persistent companion profile as `addcompanion`; in deathmatch it retains classic transient-bot behavior. |
| `removecompanion [1-7]` | Active-level command that removes the selected persistent profile and clears its saved configuration. Without a number it deterministically removes the highest-numbered member of the squad. It can also edit a saved squad during deathmatch, where no companion pawn is deployed. Use **Remove this companion** in that member's roster page for a visible exact-selection UI. |
| `dismisscompanions` | Clears the active and queued companion squad, clears saved companion profiles, and returns the companion count to zero. Outside a level it performs the same reset for the next run. |
| `removebots` | Legacy command. In co-op it is an alias for `dismisscompanions`, clearing live/queued companions and saved profiles; in deathmatch it retains classic transient-bot removal. |
| `listbots` | Prints the complete identity roster and marks joining/active names. It also opens a compact dismissible summary, which keeps the result visible behind an option menu. |

The existing `bots.cfg` roster remains supported and takes precedence when it
is valid. If it is unavailable, BiasedDoom uses its shipped companion roster,
so ordinary local cooperative play does not depend on a custom file. In
cooperative play, legacy `addbot` also creates a persistent companion profile;
in deathmatch it keeps its classic transient-bot behavior. In cooperative
play, `removebots` is an alias for **Dismiss all companions**: it removes the
live/queued squad *and* clears saved profiles so they cannot reappear on the
next map. The roster still supplies selected-profile removal.

Named `-bots` startup entries remain classic, unprofiled bots when no saved
companion profile squad is active. A configured profile squad is exact and
authoritative: activating it discards stale startup entries and retires any
already queued or live unprofiled co-op bots before its profiles are realized.
This prevents a legacy launch list from silently multiplying a two-companion
squad. Classic deathmatch bots remain separate from that cooperative rule.

## Appearance, respawning, and behavior

Skin choices are populated from the currently loaded engine skin list rather
than assumed from a particular IWAD. At spawn, the engine uses its ordinary
player-class and skin compatibility rules, so an unavailable or incompatible
skin safely falls back to the active game's normal appearance. This keeps the
same menu usable with Doom, Doom II, and compatible mod/player-class setups
without claiming that every skin exists in every game.

**Respawn companions after death** controls only the automatic in-level
cooperative respawn input. Turning it off leaves a dead companion down until
the normal game/lifecycle path returns it; it does not alter human-player
respawn rules or legacy deathmatch bot behavior.

An enabled co-op profile squad enables the server's normal
`sv_coopsharekeys` rule and reasserts it while that squad, or one of its
profile-managed pawns, remains active. A key collected by a human or a
companion is available to the whole group; companions neither receive a private
key path nor bypass a keyed door. Once the final profile-managed pawn has been
removed, clearing the final profile stops that reassertion but does not silently
reset the server CVar, so the host can then choose the normal key-sharing
setting for later play.

This release does not add tactical orders. Companions use the engine's
follow-and-fight behavior; there are no formation, hold-position, target,
loot, revive, or direct-movement orders.

## Navigation, combat safety, and map compatibility

The same movement and combat code is used on ordinary maps and procedural
maps. A companion first takes a direct, collision-clear route; when its leader
or objective is around an ordinary corner, it can build a short, deterministic
route through nearby sectors and their safe crossings. This lets it recover around
loops, stair rooms, and recognized manual doors or lifts instead of treating a
lost line of sight as an unreachable destination.

In co-op, the human leader is a real navigation priority rather than merely
one candidate among pickups and monsters. If the leader is around a corner,
more than 320 map units away, or the bot has failed to make progress for three
movement attempts, the companion pins its destination to that leader and uses
the route planner. A visible monster may still be fired on safely while the
bot returns, but it cannot turn the companion into a long-distance pursuer.
Likewise, an optional pickup must be local to both the companion and its leader
before it can replace a follow goal. This keeps a helper beside the player
instead of letting a reward on the other side of a room pull it ahead or leave
it lost behind.

In cooperative play, a companion pawn is physically passable to its human
teammates and to other companions. This prevents a follower from pinning
someone in a narrow doorway or small room. It is a player-pair collision rule,
not noclip: companions still collide with walls, doors, floors, monsters, and
map triggers. Weapon, projectile, and friendly-fire behavior is unchanged.

Every requested movement—following, roaming, strafing in combat, and recovery
from a blockage—passes the same final clearance check. It rejects solid
geometry, insufficient headroom, rises above the pawn's real step height,
unsafe drops, damaging floors, and crushing/damaging 3D-floor space. Its
3D-floor check follows the actual support surface, including a change between
two surfaces in the same base sector. A bot that makes no real progress
abandons the blocked lane quickly and chooses a different one rather than
pushing into the same corner for its full roaming timer.

When following a human leader, a companion can deliberately take one ordinary
descent of up to 64 map units when the current portal, landing support,
headroom, and hazard checks all prove it safe. This covers common IWAD layout
such as Doom II MAP01's opening ledge without broadly teaching bots to jump
off cliffs: item runs, combat strafing, projectile safety probes, and random
recovery directions retain the normal conservative drop limit. The proof is
goal-directed and is checked again when the final movement command is emitted.

Recognized local manual doors and lifts are approached from the correct side,
with their real key requirement checked before use. Tagged remote controls are
not mistaken for the physical doorway a bot is trying to cross. The bot sends
a single use press, waits for the geometry to respond, and then replans if it
cannot pass. It deliberately does not guess at arbitrary scripted switches,
exits, teleports, or custom objectives: those map actions can change a level
in ways that a navigation probe cannot safely predict. If an unusual map
mechanism cannot be represented as a safe walk/use crossing, companions retain
their normal safe follow behavior. The emergency co-op catch-up fallback is
limited to a line-free local segment; it never teleports a companion across a
door, mid-texture, portal boundary, key gate, or scripted trigger.

Direct traces use the same pawn-width rule as sector routing and resolve an
objective from its own 3D-floor support, so a narrow opening or a leader on a
raised platform cannot masquerade as a reachable route. A verified local lift
may be used from its low side when the only current problem is its vertical
step; an unsafe drop, damaging floor, or clearance failure never causes a bot
to press an unrelated nearby special.

Pickup selection is local, visible, and directly reachable. A distant item
behind a lock, pit, or custom mechanism cannot pull a companion away from the
human group. Companions also prefer a human co-op leader over another bot when
choosing whom to follow, which prevents follower chains from wandering across a
normal map.

Before firing, a companion verifies that the first shootable actor in its
firing lane is the intended hostile. Explosive and BFG-class weapons add a
conservative teammate lane and blast-radius check. These safeguards intentionally
favor holding fire over harming a human or companion in a doorway.

The route guide is bounded and deterministic (at least 512 and at most 4096
sector expansions, scaled to the loaded map), and its transient route state is
saved with the bot. It is not a universal navmesh or a promise that every
third-party scripted puzzle is automatable; it is a conservative engine-level
improvement that works without procedural-map metadata and preserves normal
map actions for the player.

## Regression coverage

The headless doorway regression freezes a companion in a one-player-wide
corridor, proves that Player 1 crosses its former blocking position, and then
proves that the terminal wall remains solid. Run it against a Doom-compatible
IWAD after building the engine:

```bash
tools/test-bot-doorway-passability.sh --iwad /path/to/doom2.wad
```

The companion-route regression places the leader around a two-portal L turn
and proves that the original companion reaches the normal formation band by
walking through both physical crossings, rather than randomly roaming or using
the emergency recovery fallback:

```bash
tools/test-bot-companion-route.sh --iwad /path/to/doom2.wad
```

The profile-authority regression starts the two profile paths that previously
could grow into a larger squad: a six-name legacy `-bots` startup list followed
by two public profile deployments, and a clean `bot_companion_count=2` setup.
It proves that both runs remain at exactly two live companions:

```bash
tools/test-bot-companion-profile-authority.sh --iwad /path/to/doom2.wad
```

## Procedural multiplayer and host handoff

A network procedural run is host-authored. The host/settings controller
generates the requested recipe once, packages the resulting UDMF as a
checksummed embedded archive, and sends that exact archive to all connected
peers before the normal map change. Peers validate and stage the archive, then
acknowledge it; they never regenerate the map from the seed. Every peer must
use compatible Doom-family game data and the same procedural roster context
(Ultimate Doom or Doom II); the host rejects a roster mismatch instead of
attempting cross-IWAD conversion. A shared procedural session also accepts at
most eight human participants, before any companion capacity is considered.
Normal-map companion bots do not use this procedural archive path and retain
their ordinary cross-family map support.
The host starts this transfer from the existing live cooperative session; it
finishes before the standard map change and is unavailable in deathmatch.

This means a procedural map is identical for every participant even when an
engine release changes the generator's output for an old seed. It also means
there is no join-in-progress while that transfer is underway. A transfer that
fails validation, is cancelled, or times out leaves the group on its current
map rather than loading a partial run.

Generated maps reserve protected P1–P8 pads in the start landmark. P1 remains
the canonical progression start, while the other native starts provide clear
co-op and companion placement. Participant count and companion count do not
alter a recipe, generated UDMF, or manifest; savegames keep their exact
archived UDMF.

If the network host legitimately hands off during an active session, the new
host becomes the settings controller and may author a later procedural run.
An in-progress procedural transfer cancels safely rather than allowing two
hosts to issue competing map changes. The new host also owns subsequent
companion changes, subject to the same eight-occupant capacity. The departing
host includes an authoritative companion-profile snapshot in its
handoff packet, so even an edit accepted immediately before the handoff is not
lost while waiting for the next normal network tic. The promoted host rebuilds
its local identity reservations from that snapshot and the live squad.
