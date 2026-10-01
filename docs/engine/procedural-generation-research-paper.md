# Mission-Graph-First Procedural Level Synthesis for Doom

## A deterministic, progression-safe, runtime UDMF generator in BiasedDoom 4.15+

**BiasedDoom contributors**

**Implementation paper — October 2026**

## Abstract

BiasedDoom generates complete, playable Doom levels at runtime without selecting
from prefabricated maps. The generator accepts a seed, visual theme, difficulty,
size, layout shape, verticality, architectural-detail density, and outdoor
cadence; constructs a directed critical path with
optional branches, staged keys, locks, loops, hubs, arenas, secrets, and an
exit; embeds that mission graph on a bounded grid; composes adjacent cells into
rooms; assigns geometry, materials, lighting, encounters, weapons, and recovery
resources; and serializes the result as an in-memory UDMF `TEXTMAP`. The normal
engine map loader and node builder then consume that text exactly as they would
consume a map stored in a WAD or PK3.

The current release adds an automatic, recipe-derived `RunBlueprint` before
mission-graph construction. It selects one of five gameplay profiles, a
cardinal route direction, shuffled key order, planned encounter/recovery
beats, feature motifs, an arsenal track, and a finale card without consuming
the layout RNG. This changes campaign rhythm between runs while retaining the
same static UDMF, single-player, IWAD-safe implementation model.

The central design choice is to separate *progression topology* from *physical
geometry*. A randomized spanning tree is used only as an embedding substrate.
The emitted map is a deliberately selected subset whose critical path is known
before doors or rooms exist. Keys are placed on optional limbs before their
corresponding gate ranks, and circulation loops may connect only rooms in the
same lock stage. This makes progression safety a construction property rather
than a post-generation repair problem.

The current implementation also treats Doom geometry as a set of explicit
topological contracts. Every exposed wall is a blocking, one-sided linedef with
a real middle texture; traversable joins use explicit two-sided portals;
functional doors are closed 16-unit sectors with two `Door_Raise` faces and
static tracks; and sector, sidedef, and linedef references are validated after
serialization. Room-level visual coherence is produced by theme-local material
families, realized contour grammar with UDMF-backed compound envelopes,
role-aware landmarks, varied clearances, and
collision-safe decoration. A pure recipe hash also plans footprint grammar,
connection profile, district material identity, and a graph elevation field
without perturbing the layout RNG. Four orthogonal style controls alter layout
topology, terrain intensity, architectural density, and outdoor cadence.
Encounter pressure remains bounded per room, while guaranteed weapon milestones
and resource budgets preserve player agency. The logical 384-unit spatial
module is serialized with seed-stable 368/384/400-unit center spacing; narrow,
standard, gallery, and grand connections, coherent compound rooms, and major
landmarks create a strong scale hierarchy while native door profiles preserve
stock art.

Tagged reveal sectors add manual, usable wall switches through freestanding
pavilions, framed wall alcoves, or perimeter false-wall chambers. Their cache
doors never use walkover activation; combat ambushes are instead selected as
ordinary static encounter cards. Stair-served platforms, chamfered turrets, and
wall-backed balconies add reachable vertical pressure. Theme-aware shallow
liquids form whole flooded rooms, irregular reservoirs, and multi-cell
watercourses crossed by dry causeways. Raised framed windows add cross-room
previews without adding a traversable progression edge. Stock switch art is
fitted exactly once, major landmarks use 8-unit stair tiers, and optional
four-sided lifts add operable vertical motion without becoming mandatory route
gates.

The representative validation matrix spans five themes, all difficulty bands,
Ultimate Doom and Doom II actor vocabularies, and compact through absurd map
sizes. It reconstructs serialized sector topology, measures small/medium/large
floor-area bands, requires a dominant room at least five times the median,
checks all liquid/reveal/perch families, validates dry hazardous bypasses and
actor-free liquid, verifies exactly two locked faces per key, and performs real
runtime/node-builder loading. Repeated generation of the same input produces
byte-identical UDMF.

## 1. Problem statement

A Doom level generator must satisfy several concerns that are easy to conflate:

1. **Progression:** the exit must be reachable, every required key must be
   obtainable before its lock, and optional loops must not bypass gates.
2. **Embedding:** logical rooms and branches must occupy non-overlapping space
   within a practical map footprint.
3. **Geometry:** sector boundaries must have correct winding, valid references,
   closed perimeters, and renderer-safe textures.
4. **Playability:** height transitions, door behavior, actor clearance, and
   starts/exits must obey engine rules.
5. **Pacing:** encounter strength, weapon availability, ammunition, health, and
   armor must evolve coherently across the map.
6. **Authorship:** rooms need recognizable roles and visual variety rather than
   looking like a uniformly decorated maze.
7. **Reproducibility:** a seed must reproduce the same map for debugging,
   sharing, and regression testing.
8. **Compatibility:** output must use the correct actor vocabulary for both
   Ultimate Doom and Doom II and remain usable with normal mod loading.

A generator that begins by filling a grid and only later attempts to infer a
mission structure makes these properties mutually fragile. A newly inserted
door can make the exit impossible; a random loop can make a key irrelevant; a
merged sector can erase a wall boundary; and an encounter randomizer can place
heavy monsters before an appropriate weapon. BiasedDoom instead establishes a
sequence of representations in which each pass owns a smaller, explicit set of
decisions.

## 2. Contributions

The implementation makes the following concrete contributions:

- A mission-graph-first pipeline whose key/lock ordering is guaranteed by
  construction and whose loops are constrained by lock stage.
- A bounded grid embedding that produces broad, directional maps but retains
  deterministic branch and landmark placement.
- A semantic room compositor that merges cells according to role,
  progression, branch depth, connectivity, spatial scale, and compact/axial/
  compound shape family rather than proximity alone.
- A theme-local visual grammar with recipe-hashed district material families,
  deterministic per-room variation, readable light bounds, outdoor landmarks,
  and semantic props.
- UDMF-backed unified envelopes for eligible rectangular compounds, requested/
  realized asymmetric, tapered, apse, stepped, courtyard-cut, and fractured
  contour grammar for the remaining safe shells; four protected connector
  profiles; and a multi-flight terrain field
  that makes extreme altitude walkable through 8-unit stairs.
- Explicit, closed UDMF geometry with deduplicated vertices, correct sidedef
  winding, functional recessed doors, fitted door art, aligned wall textures,
  and collision-aware thing placement.
- Interactive tagged geometry comprising usable manual switch caches,
  stair-accessible ranged platforms, strongly keyed door
  borders, fitted single-copy switch panels, traversal-safe inset clearances,
  three reveal families and cue strengths, three raised-position families and
  stair approaches, raised cross-room sightlines, tiered landmarks, bypassable
  reward lifts, and an unmistakable exit pad.
- Macro-scale fluid descriptors for flooded rooms, irregular reservoirs,
  trenches, paired basins, and straight/staggered/bending watercourses with
  theme-correct damage and guaranteed dry circulation.
- A bounded encounter/economy model with guaranteed weapon milestones,
  phase-aware ammunition, major-fight recovery, progression-aware secret
  artifacts, IWAD-aware actor tables, and difficulty monotonicity tests.
- An in-memory map-loading bridge and native ZScript interface that require no
  temporary WAD and no separate executable.
- A validation suite that examines the serialized artifact and real runtime
  loading rather than relying only on internal generator assertions.
- A hash-derived RunBlueprint layer with five campaign profiles, four route
  orientations, three arsenal tracks, four finale cards, planned static
  encounter cards, two/three feature motifs, and inspectable room-level
  threat/recovery plans.

## 3. System interface and generation contract

The generator is a process-wide `FProceduralMapGenerator` singleton. Its public
configuration is intentionally small:

| Input | Domain | Meaning |
|---|---:|---|
| `seed` | signed integer, consumed as 32 bits | deterministic random stream |
| `theme` | `techbase`, `hell`, `industrial`, `gothic`, or `corrupted` | material and decoration vocabulary |
| `difficulty` | 1–5 | encounter count, monster tier, boss policy |
| `size` | 1–160 | canvas size, route target, branches, keys, landmarks, weapon milestones |
| `layout` | 0–2 | directed/balanced/exploratory route, branch, loop, and embedding policy |
| `verticality` | 0–2 | gentle/varied/dramatic graph terrain field and stair-chain budget |
| `detail` | 0–2 | landmark, reveal, perch, lift, trim, and prop density |
| `outdoors` | 0–2 | enclosed/mixed/open-air sky-landmark cadence |

All numeric settings clamp out-of-range values. The archived CVars
`procgen_seed`, `procgen_theme`, `procgen_difficulty`, `procgen_size`,
`procgen_layout`, `procgen_verticality`, `procgen_detail`, and
`procgen_outdoors` expose the contract to the console and menu. `procmap` starts a single-player game on
the virtual map name `PROCMAP`; `dumpprocudmf` serializes the same result to
`/tmp/procmap_test.udmf` for inspection; `dumpprocmanifest` serializes the
schema-1 run plan to `/tmp/procmap_manifest.json`, including each room's
realized manual-interaction role (`none`, keyed door, switch cache, or secret
door), post-emission card-feasibility evidence (geometry role, capacity,
owned static-encounter count, and 0/1 player-use witness), requested/realized
footprint grammar, unified-envelope sector/bounds evidence, material family,
`floor_z`, clear height, and contour metrics. It also records
the planned/realized macro-stage counts; stage material/elevation roles; every
realized connector's route role, profile, clear width, depth, physical-door and
native-art dimensions, rise, stair-chain, and alignment group; a `visual_proof`
summary; and each macro stage's planned
`vertical_rise` plus `realized_vertical_rise`. Dramatic manifests also retain
the recipe's `main_route_elevation_target` and `optional_elevation_target`
separately from realized floors, making a clearance-safe scenic fallback
inspectable. The full three-color
`key_order` remains the recipe plan even when a compact realized route needs
fewer keyed crossings. Both dump commands accept the eight recipe arguments followed by an
optional output path.

The map-loading boundary is important. `P_OpenMapData` asks
`P_OpenProceduralMapData` to handle names equal to `PROCMAP` or beginning with
`PROC`. Immediately before generation, the factory re-applies all eight CVars
and re-seeds the generator. The resulting string is installed as the
`ML_TEXTMAP` lump of a newly allocated textual `MapData`. The rest of the
engine—including parsing, node construction, sector creation, and actor
spawning—uses the ordinary map path.

This boundary gives the generator the following observable contract:

```text
(seed, theme, difficulty, size, layout, verticality, detail, outdoors,
 IWAD family, engine build)
                         -> RunBlueprint -> UDMF TEXTMAP + manifest
```

The IWAD family is part of the effective input because Ultimate Doom and Doom
II have different actors. Reproducibility is guaranteed within the same engine
implementation and game-data context: identical input produces byte-identical
UDMF and manifest artifacts. It is not promised across generator algorithm
revisions; an old shared seed may intentionally produce a new run in a later
release.

Savegames preserve a stronger contract than seed reproducibility. `info.json`
records all eight recipe fields, while `procmap.json` stores the exact generated
UDMF. The loader validates and stages that document before normal world
deserialization, then copies the archived recipe back to the CVars. Older
procedural saves missing the four style fields receive the neutral value `1`.
Consequently, later generator changes cannot alter the base map underneath a
saved world, and conflicting ambient menu settings cannot select another map.

## 4. Pipeline overview

```text
Configuration and seed
         |
         v
Pure recipe hash -> RunBlueprint + briefing + manifest plan
         |
         v
Randomized spanning-tree substrate
         |
         v
Critical path + key limbs + side limbs + lock edges
         |
         v
Landmark expansion + same-stage circulation loops
         |
         v
Semantic cell-to-room composition
         |
         v
Room graph analysis + materials + dimensions + lighting
         |
         v
Encounters + weapon milestones + recovery + secrets
         |
         v
Closed chambers + corridors + doors + things
         |
         v
UDMF serialization -> MapData -> normal node builder/runtime
```

Each arrow represents a loss of freedom. Once a lock edge is planned, later
passes may decorate or geometrically realize it but may not move it to a
different progression stage. Once cells are composed into a room, later passes
may vary that room's silhouette but may not merge it through an unrelated lock.
This monotonic refinement sharply reduces the number of global repairs needed.

## 5. Deterministic random process

The generator owns an `FRandom`, which derives from the engine's SFMT random
implementation. `SetSeed` initializes it from the requested 32-bit seed.
Parameterless calls return the low eight bits of a newly generated 32-bit value;
modular selection uses `GenRand32() % n`. No wall-clock value is consulted by
the generation passes. Wall-clock entropy appears only in the explicit
“Randomize Seed” user action, which writes a new seed before generation.

Determinism depends on a stable order of decisions. The implementation therefore
uses row-major grid scans, ordered room arrays, fixed direction arrays, and
deterministic tie-breaking with the seeded stream. It does not iterate an
unordered hash container to make generation decisions. The map loader also
re-seeds immediately before `Generate`, preventing an earlier diagnostic or
ZScript call from advancing the stream used for the actual loaded map.

`RunBlueprint` is intentionally not drawn from that stream. A pure 32-bit hash
of the complete recipe feeds independent channels for profile, orientation,
key permutation, motifs, arsenal track, finale, and bounded pacing variation.
Inspecting the profile or writing a manifest therefore cannot shift any later
layout RNG draw. The blueprint is automatic: there is no profile CVar, menu
selector, or compatibility path that recreates the pre-blueprint rhythm.

The determinism regression performs three generations:

```text
H(seed = 424242) == H(seed = 424242)
H(seed = 424242) != H(seed = 424243)
```

where `H` is SHA-256 over the complete emitted UDMF byte sequence. The first
equality proves repeatability for the reference case; the inequality prevents a
degenerate test in which the seed is ignored.

## 6. Mission graph construction

### 6.1 Canvas

For size `S` in `[1,160]`, define

```text
R = max(0, S - 40)
W = 8 + 2S - R cells
H = 7 + S + R  cells.
```

and each cell is a logical 384-map-unit module. Serialization offsets rows and
columns by 0 or 16 units, producing adjacent center gaps of 368, 384, or 400
units without changing planner topology. The outer one-cell frame is never used,
so the logical working set is `(W - 2)(H - 2)`. Through size 40, the rectangular
aspect ratio and blueprint-selected cardinal direction favor broad,
directional footprints. Beyond 40, each new size step transfers one column into
a row: size 160 is 208×287, has far more capacity than the former 168×87 strip,
and retains broad coordinate margins. Difficulty changes landmark budgets, not canvas dimensions
or target route length; the finale reserves its footprint before secondary
arenas consume nearby empty cells.

### 6.2 Randomized spanning-tree substrate

The start coordinate is `(1, sy)`, where `sy` is randomly selected inside the
border. A depth-first traversal visits every interior cell. At each step, every
unvisited cardinal neighbor receives

```text
score = U[0,99]
      + forward-direction bias selected by the RunBlueprint
      - backward-direction bias selected by the RunBlueprint
      -  5 if adjacent to the north/south interior border.
```

The best candidate becomes the next child; when no candidate exists, the
traversal backtracks. The pass records `parent` and `depth` but does not yet
emit any level cell. This distinction is essential: the spanning tree is a
private route reservoir, not the final map.

### 6.3 Exit and critical path

The desired route length is

```text
L_target = (9 + 4S) × bounded blueprint route-length factor.
```

Every visited cell of depth at least seven is scored as an exit candidate:

```text
exitScore = 28x - 8|depth - L_target|
          + 100 if x >= W - 3
          +  18 if |y - sy| >= H/3.
```

Thus the exit tends to lie at the opposite edge selected by the blueprint, near
the desired path length, and often in a different perpendicular band from the
start. Following parent pointers back to
the start produces the critical path. Generation fails rather than emitting a
weak map if no sufficiently long path exists, if a parent chain is incomplete,
or if the result contains fewer than eight cells.

Every critical-path cell receives a monotonically increasing `pathRank` and is
connected bidirectionally to its predecessor. `pathRank` is the fundamental
progress coordinate used by later passes; physical BFS distance is computed
separately after room composition.

### 6.4 Optional limb growth

Branches grow from a known critical-path anchor. At each branch step, unused
cardinal neighbors are evaluated using openness and local contact:

```text
branchScore = U[0,30]
            + 7 * openNeighbors
            - 6 * keptNeighbors
            - 80 * unintendedMainPathContacts
            + 5 if on an interior border.
```

The heavy penalty for touching the critical path anywhere except the current
anchor prevents a branch from silently reconnecting beyond a future gate. Each
accepted branch cell inherits the anchor's `pathRank` and records a one-based
`branchDepth`.

### 6.5 Keys and locks

The requested number of keys depends on size and realized route length:

| Condition | Keys |
|---|---:|
| size at least 5 and path length at least 18 | 3 |
| size at least 3 and path length at least 13 | 2 |
| otherwise | 1 |

The blueprint's hash-shuffled permutation of blue, red, and yellow supplies the
key order. For key index `k` out of `K`, the initial gate rank is approximately

```text
gateRank(k) = clamp(|P|(k + 1)/(K + 1), 3, |P| - 2).
```

Its branch anchor is selected several ranks before the gate. Up to eight nearby
anchors are attempted, and the key is placed at the tip of a limb of roughly
`2 + floor(S/2)` cells. The tip becomes a key arena.

The lock is not a property of every wall around a “locked room.” It belongs to
the single directed boundary between critical-path ranks `gateRank - 1` and
`gateRank`. The later geometry pass therefore emits exactly two usable door
faces for each required key, not a ring of redundant locked doors.

The construction establishes three ordering facts:

1. a key anchor rank is lower than its gate rank;
2. the key limb is attached on the pre-gate side;
3. branch growth cannot touch the critical path elsewhere.

Consequently, the player can obtain the key before reaching its door, and the
key branch cannot itself become an unintended bypass.

After landmark expansion and loop insertion, the generator materializes a
`lockStage` on every retained cell and audits the final coarse connection set.
Equal-stage edges may not own a lock. A cross-stage edge must advance exactly
one stage, be owned by the later cell's directed keyed boundary, match the
planned gate rank and key type, and be the only crossing of that cut. Room
composition refuses to merge cells from different stages, and UDMF emission
repeats the stage/lock equivalence before it creates any normal portal or door.
This turns progression safety from a construction assumption into a checked
pre- and post-composition invariant.

### 6.6 Side branches and landmarks

The generator requests a bounded profile-scaled set of general side branches,
distributed across the critical path with seeded jitter and early/broad/late
profile bias. Anchors adjacent to a key branch are shifted when possible.
Branch lengths scale from one cell to a small size-dependent limb; selected
deep limbs become arenas.

Selected progression beats are then expanded into multi-cell landmarks:

- the start becomes a hub;
- profile-selected safe ranks become another hub and an arena;
- every key tip becomes an arena;
- the exit becomes the largest arena.

Landmark expansion greedily adds unused cells adjacent to the growing cluster.
It rewards multiple cluster contacts and penalizes Manhattan distance from the
landmark center, producing compact shapes rather than narrow tendrils. Added
cells inherit the semantic role and progression rank of the landmark.

The exit receives a boss only at difficulty 5, or at difficulty 4 on size 4 or
larger.
This separates “final encounter” from “boss monster” and avoids forcing a boss
into every generated map.

### 6.7 Same-stage circulation loops

Let `G = {g0, ..., gK-1}` be the sorted gate ranks. A room rank `r` belongs to
stage

```text
stage(r) = |{g in G : r >= g}|.
```

The balanced-layout baseline budget is `2 + S + floor(S/2)` before theme and
layout scaling. Missing east/south adjacencies are considered in three passes.
The first pass accepts only rank gaps of at least three, prioritizing routes that
fold back to an earlier region; later passes can spend remaining budget on local
circulation. The maximum accepted gap grows from seven with map size. A
connection can be opened only if both cells have the same stage and passes a
seeded layout-dependent probability. Stage equality remains the decisive
progression invariant: loops can improve reuse, alternate routes, and
cross-views inside a completed stage, but cannot connect the pre-key side of a
gate to the post-key side.

### 6.8 Blueprint pacing, motifs, and districts

After topology has a safe progression skeleton, the blueprint assigns bounded
room beats: opening, approach, key objective, recovery, set piece, finale, and
optional. Geometry-qualified rooms receive static encounter cards from
Breather, Skirmish, Crossfire, Pincer, Ambush, Cache Challenge, Holding Line,
and Set Piece. A cadence pass forbids more than two high-pressure cards in a
row and places recovery or an armory/cache choice after major holds and set
pieces. This is a planning annotation consumed by ordinary UDMF emission, not
a runtime director or monster-spawn system.

The same plan chooses two distinct feature motifs per normal map and three at
size 5+: watercourse, vertical pressure, remote reveal, shrine secrets, and
sightline reconnaissance. Candidate rooms are hash-ranked before geometry is
emitted; an infeasible candidate falls back deterministically without
consuming layout RNG. Two to four macro stages then select requested and safely
realized route shapes (`spine`, `fork_rejoin`, `ring`, `switchback`, or
`courtyard_spokes`), landmark archetypes, district roles, and vertical-route
intents. Each non-flat stage is anchored on the critical route, so the planned
vertical beat cannot disappear into an optional branch. At normal size and
above, Gentle plans a profile-derived stair hall, terrace overlook, or bridge
approach; Varied plans positive and negative doglegs; and Dramatic combines
those doglegs with a scenic form, including a 48–64-unit rise. Two or three
theme-local districts select compatible materials, light accents, and props
around the route, giving a theme internal location changes without introducing
non-IWAD assets.

## 7. Semantic room composition

The mission graph operates on cells, but Doom spaces should read as rooms,
halls, and courtyards rather than as a chain of identical boxes. `MergeRooms`
groups cells into `RoomInfo` records in two priority passes: special cells first,
ordinary cells second. Special cells include starts, exits, bosses, keys, and
locks; giving them first claim preserves their intended landmark footprints.

### 7.1 Compatibility predicate

A candidate cell may join a seed room only if their roles and progression are
compatible. Important constraints include:

- locked cells merge only with an equivalent lock type;
- ordinary cells do not absorb a special cell;
- key, exit, boss, and start cells merge only into their own landmark rank;
- ordinary ranks differ by at most one;
- main-path and branch cells cross-merge only for an explicit hub/arena at the
  same rank;
- deep-branch cells may not differ by more than one branch-depth level;
- arena/hub differences are tolerated only inside one landmark rank.

This is a semantic flood fill, not a rectangular partition.

### 7.2 Spatial class, target size, and shape family

Target cell count depends on role, map size, and—for combat landmarks—difficulty.
Locks remain one cell. Landmark growth uses `2 + floor(S/4)` as its base;
starts, keys, hubs, arenas, and finales add role-specific cells, bounded seeded
variation, and difficulty growth. This keeps important spaces broad without
making every ordinary destination resemble an arena.

Ordinary rooms sample an authored four-band distribution:

| Spatial class | Realized cells | Typical function |
|---|---:|---|
| connector | 1 | compressed transition, utility room, or closet |
| small | 2 | intimate encounter or short bay |
| medium | 3–6 | ordinary combat/traversal room |
| major | 7+ | hall, court, hub, arena, or set piece |

Main-route seeds deliberately retain approximately 20% one-cell connectors and
25% two-cell rooms, while roughly 22% target seven or more cells. Branches use a
denser distribution but still admit occasional large destinations. Realized
class is recomputed after constrained growth, and major semantic landmarks are
never downgraded below medium.

Each target also selects a compact, horizontal, vertical, or compound/bent
shape family. Compact growth rewards shared neighbors and balanced bounds.
Horizontal and vertical growth prefer one-cell-thick axial expansion and
penalize filling the gallery into another rectangle. Compound growth rewards a
turn followed by branching, yielding L, T, cross, and stepped footprints.
Candidates must still match progression and route semantics; a small seeded
term resolves otherwise equivalent choices.

After composition, adjacent cells assigned to the same room are opened into
continuous floor space even if landmark expansion had not created an explicit
mission-graph edge. This is safe because the semantic compatibility predicate
has already established that they represent one room.

## 8. Room-graph analysis and spatial coherence

`ApplyCoherence` collapses cell connections into a room adjacency graph. It
collects semantic flags, the minimum progression rank, maximum branch depth,
and the start room. A breadth-first traversal computes `distFromStart` in room
edges. Rooms with one or fewer adjacent rooms become dead ends unless they are
the start or exit; main-path rooms with at least three neighbors become hubs.

Two independent progress measures are retained:

- `progressionRank` describes intended order on the mission path;
- `distFromStart` describes realized graph distance after merges and loops.

The visual pass primarily uses graph distance, while weapon scheduling uses
ordered main-route rooms and progression rank. This prevents a local circulation
loop from destroying the authored progression cadence.

## 9. Visual grammar

### 9.1 Progression phases and palettes

For maximum BFS room distance `D`, room phase is

```text
phase = clamp(floor(4 * distFromStart / (D + 1)), 0, 3).
```

Techbase, Hell, Industrial, Gothic, and Corrupted Tech each select a
recipe-hashed material family per district. A family contains coordinated wall,
floor, ceiling, trim, corridor, stair, and landmark materials; bounded
room-local variants keep its language coherent without repeating one room
finish everywhere. Corrupted Tech begins with clean technology, crosses through
structural/computer and vine/marble language, and ends in hot infernal hybrids.
District boundaries reserve a true architectural return or trim seam, so a
material never changes arbitrarily across a flat wall.

Techbase families pair computer/support language with airlocks, command courts,
and reactor wells. Industrial families pair brown metal and machinery with
loading bays, refinery switchbacks, and foundry bastions. Hell combines stone,
marble, vine, wood, and hot surfaces around blood chapels, ritual pits, and
chasm bridges. Gothic combines marble, wood, stone, candelabra, and tall-torch
composition around gatehouses, naves, apses, and cloisters. Corrupted Tech
uses staged tech/infernal hybrids around containment halls and breach terraces.
All materials are compatible with Ultimate Doom; Doom II-only prop variation is
emitted only where the IWAD can be identified safely.

### 9.2 Theme-owned architecture and lighting

Themes also alter physical composition. Techbase favors airlocks, command
courts, reactor wells, clean angled bays, and same-stage circulation. Hell
favors blood chapels, broken wedges, ritual pits, chasm bridges, and open
combat. Industrial uses loading bays, refinery switchbacks, service doglegs,
and foundry bastions. Gothic uses gatehouses, naves, apses, cloisters, throne
courts, and cathedral clearance. Corrupted Tech begins with containment halls
and develops breach terraces, fractured chambers, and hell-core finales.

Each phase selects a theme-specific RGB light color. Techbase is cool blue-white,
Hell warm red-orange, Industrial desaturated amber, Gothic cool violet, and
Corrupted Tech crosses from blue through gray into hot red. Interior fade colors
are equally restrained; outdoor sectors keep clear sky visibility.

### 9.3 Deterministic room identity

Each room computes a stable style hash from its ID, bounds, cell count,
progression rank, and branch depth:

```text
style = |37 id + 17 minX + 29 maxY + 13 cells
          + 7 progressionRank + 19 branchDepth|.
```

The hash selects a dimension profile, footprint grammar, district material
family, connection profiles, and elevation roles. It does not consume the
shared RNG. Geometry identity therefore remains stable even when a later random
encounter decision changes its number of draws.

### 9.4 Dimension and corner profiles

Base half-width/half-height profiles span narrow axial connectors through broad
combat modules:

```text
(88,160), (160,88), (104,136), (136,104),
(120,176), (176,120), (136,152), (152,136),
(144,168), (168,144), (160,160), (176,176).
```

Spatial class clamps these profiles: connectors keep one narrow axis, small
rooms remain compact, and major rooms receive at least 320×320-unit modules.
The selected profile follows a multi-cell room's dominant axis. Arenas and exits
remain broad; hubs and keys receive minimum combat-capable dimensions; protected
starts, locks, keys, and exits retain their door and actor clearance contracts.

Eligible rectangular composed rooms emit one shared-sector exterior envelope
with no surviving cell-face walls. Other merged rooms use requested asymmetric
octagon, tapered bay, apse, stepped compound, courtyard cut, or fractured-wedge
grammar on their safe clipped shell. Portal-bearing spans remain straight, and
their lanes are reserved before the rest of the contour is cut. Per-face
expansion, asymmetric corner offsets, concave courts, and non-45-degree
shoulders create distinct silhouettes without weakening clearance; any contour
that cannot fit safely truthfully falls back to a compact shell.

Each inter-room edge also selects one real aperture/depth profile: Narrow
96×48 for deep optional branches; Standard 128×64 for required travel, locks,
and stairs; Gallery 176×96 for routine main-route and landmark travel; and
Grand 224×128 for arenas, major landmarks, and finales. Narrow is forbidden on
mandatory, keyed, and stair routes.

### 9.5 Vertical composition

The terrain pass assigns a hash-planned elevation field over the room graph;
it does not derive floor height from distance modulo a fixed cadence. Floors
remain multiples of eight. Gentle preserves one meaningful required stair beat,
Varied preserves an ascent and descent, and Dramatic targets a 128–192-unit
main-route highland/basin at size 3–4 or a 192–320-unit one at size 5+. A
reachable optional district targets the opposite extreme on large Dramatic
runs. The target is kept separately from realized floors so a constrained
recipe can retry a safe scenic chain rather than make an otherwise playable
map fail. Room clearances vary by role:

| Role | Typical clear height |
|---|---:|
| compact ordinary room | 144–176 |
| general room | 160–208 |
| hub | 192–224 |
| arena | 240–272 |
| exit/boss landmark | 288–320 |

The start is fixed at floor 0 and ceiling 192. Components that require a moving
door—start staging, keyed stage cuts, and key-shrine thresholds—share one floor,
as do start/key/exit pads and required switches. Every individual connection
changes by at most 64 units and the UDMF emitter converts it into spatially
ordered 8-unit stair sectors. A failed scenic chain retries another eligible
route and then drops the optional feature rather than failing generation.

### 9.6 Lighting and outdoors

Base light begins at 192 and falls by eight per phase. Side rooms and deep
branches darken; hubs, arenas, starts, keys, and exits brighten. Values are
quantized to multiples of eight and clamped to 160–208. The lower bound is a
deliberate readability policy rather than an engine limit.

Every map makes the exit and at least one additional combat landmark outdoor.
Larger maps select more multi-cell arenas, hubs, and broad main-route rooms.
The budget begins from a size-scaled baseline and is then adjusted by the
outdoors setting and theme. Outdoor sectors use at least light 192. Validation
requires at least two sky sectors and a sky courtyard spanning
at least 400 units on one axis, preventing a token sky closet from satisfying
the open-area contract.

### 9.7 Semantic detail

Starts and hubs may contain a centered platform raised by 8 units. Arenas, key
shrines, and exits place the same final 16-unit elevation behind two concentric
8-unit sectors, turning a single curb into a legible stair dais. Indoor hubs may
receive a shallow ceiling coffer; sky landmarks preserve the sky ceiling. The
exit is a larger 144-unit, level-224 `GATE1` pad bounded by four `EXITDOOR`
edges and an outer `STEP1` tier, giving its invisible walk trigger an explicit
visual language.

Decoration is dense enough to author room identity while remaining semantic:

- tech landmarks use lamps in Doom II and shared pillars/columns in Ultimate
  Doom;
- Hell and Gothic key rooms use key-colored torches or a gold candelabra when
  a safe wall bay exists; otherwise their matching keyed-door trim preserves
  the color cue without placing a solid prop in the reserved route;
- Hell exits use an evil eye, with a non-solid hanging infernal fallback when
  every solid wall bay would violate a reserved route;
- Hell outdoor rooms use torch trees, with the same clearance-safe non-solid
  fallback;
- secret rooms use reward-readable props;
- Industrial rooms add heavy lamps and occasional machinery barrels;
- Corrupted rooms cross from tech lamps to infernal torches;
- sufficiently populated combat rooms may contain one or two corpses.

Ordinary rooms attempt one to three props and landmarks attempt four to eight
across twelve wall/corner bays. Solid props are tested against every previously
emitted gameplay thing with a 40-unit clearance. They are also rejected from a
112-unit-deep approach rectangle around every traversable portal, operable door,
lift, and full stair route, with 28 units of aperture margin. Shallow landmark
tiers use a 40-unit depth and 12-unit margin so shrine markers can remain beside,
but never on, their steps. Non-solid corpses use a
smaller clearance. This makes decoration visible without allowing it to block
combat circulation or doorway approaches.

## 10. Encounter model

### 10.1 Per-room pressure

The start has zero enemies. For an ordinary room, initial pressure is

```text
pressure = floor((difficulty - 1)/2)
         + [phase >= 2]
         + [difficulty >= 4 and main path and phase > 0]
         - deepBranchRelief
         + U[0,1].
```

Difficulty 2 receives a sparse deterministic extra point so it remains
distinguishable from difficulty 1. Counts are then bounded by semantic role:

| Room role | Encounter cap/range |
|---|---:|
| ordinary | 1–3 |
| early ordinary room | at most 2 |
| dead-end reward | reduced by difficulty |
| hub | 2–4 |
| arena or key room | 2–5 |
| locked transition | 1–4 |
| exit | 2–5 |
| boss support | capped at 1–3 |

The purpose of the cap is not merely performance. Doom difficulty grows
nonlinearly with monster composition and room geometry, so bounding local count
prevents a random room from consuming the entire map's pressure budget.

Monster tier is

```text
tier = clamp(1 + phase
               + [difficulty >= 4]
               + [boss room and difficulty >= 5], 1, 5).
```

Heavy rosters therefore arrive primarily through progression and only receive a
difficulty acceleration in the upper two settings. Single-cell rooms cap at
tier 2 and two-cell rooms cap at tier 3, preventing large bodies from appearing
inside connector-scale geometry.

### 10.2 Coherent monster families

Rooms select a family from room ID, progression rank, and branch depth. Doom II
families include early infantry/demons, middle infantry/demons/flyers, and late
bruiser/heavy/air groups. Within one room, enemy-index jitter varies individual
actors without mixing the whole bestiary indiscriminately. Arch-Viles are
excluded from random placement.

Ultimate Doom uses separate early, middle, and late arrays containing only
actors available in that IWAD. Boss selection is also IWAD-aware. Doom II uses
an easy Baron fallback through difficulty 3, a medium Baron/Hell Knight choice
at difficulty 4, and a Cyberdemon at difficulty 5 only when the composed finale
contains at least eight cells. Ultimate Doom never substitutes the Doom II-only
Hell Knight. The Spider Mastermind is excluded because its 128-unit radius still
needs a dedicated footprint proof beyond generic spawning in a 384-unit cell.

### 10.3 Boss policy

A boss is a separate thing in the exit landmark and does not replace all
ordinary enemies. Bosses occur only for difficulty 5 or large difficulty-4
maps. Arena, shrine, and finale cell budgets expand with difficulty, and a heavy
boss requires an eight-cell finale; otherwise selection falls back to the
medium roster. Support counts are capped. This avoids the common procedural
failure in which a heavyweight boss occupies a closet or combines with an
unrestricted ordinary encounter roll.

### 10.4 Manual caches, fluids, and elevated ranged pressure

Every map selects at least one optional supply reveal. A one-sided,
player-use `SW1COMP` or `SW1GARG` panel invokes `Door_Open` on a
uniquely tagged closed cache slab. No `Door_Open` linedef has `playercross`:
collecting a key and ordinary route traversal can never reveal a chamber or
start a surprise encounter automatically. Key progression remains exclusively
on the normal keyed `Door_Raise` crossings. Ambushes are still available, but
as geometry-qualified static encounter cards placed in ordinary rooms rather
than as trigger closets.

Pre-emission descriptors reserve a compatible feature cell or perimeter face
for every cache. The family selector cycles among a freestanding clipped
pavilion, a framed wall-aligned alcove backed 12 units from an exposed wall,
and a false-wall chamber extruded into a uniquely reserved, verified empty
in-bounds neighboring coarse cell. The three moving slabs are 80, 64, and 96
units wide. False-wall infeasibility falls back deterministically rather
than consuming route or progression space. The same descriptor selects a
prominent, subtly framed, or room-texture-matched hidden cue; hidden faces carry
the automap-secret flag until their manual switch opens them. Selected switches
may move to a nearby room in the same lock stage. Multiple viable caches cycle
their families while preserving ammunition and health rewards.

Liquid is selected as macro architecture, not late decoration. Before reveals,
perches, and lifts consume optional space, the emitter reserves the strongest
compatible noncritical room as the primary liquid host. One optional room of at
least three cells is preferentially flooded: its ordinary floor becomes harmless
water or blood, while a dry chamfered island retains every actor, pickup, and
reward. Constrained main-route fallbacks are also harmless and preserve their
64-unit portals.

Additional descriptors form central, trench, paired, and irregular reservoirs;
broad flooded grottos; and straight, staggered, or right-angle watercourses that
span multiple cells. Uneven multi-segment banks keep 80 units of intended dry
circulation (validated with a 64-unit minimum), and rivers provide either a
64-unit causeway or an entirely dry bypass. Starts, keys, exits, bosses, locks,
secrets, triggers, reveals, perches, lifts, height transitions, and mandatory
passages are excluded. Standard maps target three macro liquid systems when
feasible at size 5 or larger, and size-20 or larger maps target four; a broader liquid budget scales
with size, detail, outdoors, and theme.

Every liquid sector is lowered only 8 units for harmless water or blood and 16
units for nukage or lava, and no initial thing is placed in it. Theme and
progression select only IWAD-common animated flat sequences. Nukage serializes 5
Slime damage every 32 tics; lava serializes 5 Fire damage every 16 tics with
radiation-suit leakage and terrain effects. Water and blood remain purely
architectural.

Open arenas are preferred for ranged positions, with sufficiently tall hubs and
broad route rooms as fallbacks. A descriptor chooses a 112-unit square stair
platform, 120-unit chamfered turret, or 96×144 wall-backed balcony, with a
straight, offset, or dogleg approach. The top rises 48 or 64 units while
preserving at least 80 units of headroom. Every route uses exact 16-unit risers:
two intermediate sectors at the lower rise and three at the higher. Exposed
retaining sides use `blockmonsters` rather than `blocking`, but the outer entry,
every riser, and the platform connection remain open to both players and
monsters. Feature cells are removed from ordinary reward, enemy, decoration,
and fluid placement to prevent overlap with the authored geometry.

Finally, adjacent same-stage rooms that are intentionally not connected may
receive a raised sightline window. Its sill begins 48 units above the higher
floor, its lintel ends 24 units below the lower ceiling, and the aperture retains
at least 64 units of vertical clearance. The two-sided opening blocks monsters,
so it adds previews, crossfire, and route comprehension without introducing a
new progression edge.

## 11. Weapon and resource economy

### 11.1 Guaranteed milestones

Main-route rooms are sorted by progression rank, and side rooms form a
separate ordered list. Weapon assignment searches outward from a preferred
milestone if that room already owns another weapon. Every track retains the
forward start shotgun; the blueprint then selects a Ballistic, Demolition, or
Energy emphasis, moving the early chaingun, mid-route rocket, late plasma, and
optional armory opportunities inside validated availability bounds. Doom II can
still add its super shotgun and the largest hard maps can still reserve an
optional BFG branch; Ultimate Doom omits unsupported things without leaving a
broken type. The forward shotgun gives immediate agency and is geometrically
validated against player angle and distance.

### 11.2 Phase-aware ammunition

Weapon rooms receive their weapon's ammunition family. The ledger does not
plan an ammunition family before a compatible weapon is already available;
track emphasis changes which family appears most often. Major fights—five or
more enemies, arenas, key rooms, or exits—upgrade small ammunition to a box or
cell pack:

```text
shells -> shell box
clip   -> bullet box
rocket -> rocket box
cell   -> cell pack.
```

Ammo is guaranteed for weapon rooms, major fights, and every difficulty 4–5
room with at least three enemies; it appears on 60% of other main-route rooms.
Major encounters emit a second pack.

### 11.3 Recovery and rewards

Major fights and rewards receive health; a deterministic threat-and-recovery
ledger prevents three consecutive dry critical-path rooms, forbids excessive
high-pressure streaks, and reserves recovery or a meaningful choice before the
finale. Dry rooms may instead carry small health-bonus trails.
Keys and bosses receive armor, while deep dead ends have a 40% armor chance.
Boss rooms use the strongest armor type in the current table.

The generator reserves `max(2, 1 + floor(S/3))` deep optional rooms as survival
caches. These favor dead ends and contain two medikits, health bonuses, two
large ammunition packs, and periodic armor. One or more deep optional dead ends
become real engine-counted secrets by setting the room sector to the canonical
`SECRET_MASK` (`0x0400`) required by the ZDoom UDMF namespace. Raw Doom special
9 is intentionally not used because this namespace does not translate it.
A secret gets a wall-aligned hidden door, ammunition, a stim/med recovery
bundle, and armor even when weapon scheduling selected a different branch.

Secret artifacts are staged by mission scale: the shallowest secret receives a
backpack and the deepest receives partial invisibility; sizes 4, 5, and 8 add
berserk, a soulsphere, and the computer map. Size-12 infernal themes may add
light amplification, size-12 high-difficulty maps add invulnerability, and
size-20 high-difficulty Doom II maps add a megasphere. Switch caches use the
same progression vocabulary at their current lock stage. Thus the rarest
artifacts communicate optional depth instead of appearing as arbitrary room
clutter.

## 12. UDMF geometry synthesis

### 12.1 Intermediate records

The emitter accumulates typed build records for vertices, sectors, sidedefs,
linedefs, things, and connection references. Only after all geometry and things
exist are these records serialized. This makes reference indices explicit and
allows later features such as door-face scaling to modify a sidedef before text
output.

Vertices are quantized to 0.001 map units and deduplicated through a hash map. A sector
stores heights, textures, light, an optional special and UDMF ID, plus optional
`damageamount`, `damageinterval`, `damagetype`, `leakiness`, and
`damageterraineffect` fields. A connection reference can mark an ordinary portal,
door, stair, lift, or nontraversable sightline window.
A sidedef stores top, middle, and bottom materials plus their alignment
transforms. A linedef stores side indices, activation/monster-blocking flags,
special, lock number, and five arguments. Thing records can mark closet actors
as deaf ambushers.

### 12.2 Coordinate system and chamber boundaries

Cell centers are calculated relative to the center of the retained-cell bounds.
If `L` and `U` are the minimum and maximum retained indices on an axis:

```text
layoutCenter = (L + U + 1) / 2
axisShift(seed, cell) in {0, 16}
centerShift = (axisShift(seed, L) + axisShift(seed, U)) / 2
world = ((cell + 0.5) - layoutCenter) * 384
      + axisShift(seed, cell) - centerShift.
```

This centers sparse extreme layouts even when their randomized route occupies
only one side of the allocation canvas.

Each eligible rectangular compound emits one clockwise shared-sector exterior
boundary; the UDMF proof rejects any line left on a former same-room cell face.
Complex or feature-host rooms use the conservative clockwise clipped-shell
boundary and serialize their realized fallback grammar. Clockwise winding
ensures the front sidedef faces inward. Inter-room connections reserve
their selected profile's full aperture/depth before contour cuts; absent
connections remain one-sided walls. Asymmetric bays, apses, concave courts,
unequal corners, and selected non-45-degree shoulders produce theme-owned
silhouettes while a bounded shell gap prevents adjacent rooms from colliding at
the shorter 368-unit cadence.

Every exposed wall is emitted through `AddWall`, which enforces:

- a one-sided linedef;
- `blocking = true`;
- a real `texturemiddle`;
- `dontpegbottom = true`;
- an active-IWAD logical texture metric lookup with a role-safe fallback;
- a continuous world or architectural-run phase selected by the wall role;
- independent safe transforms for top, middle, and bottom material bands.

These constraints eliminate hall-of-mirrors failures from missing middle
textures and stop wall motifs from restarting at every split segment or
slipping vertically when floors and ceilings change. Isolated trim and diagonal
detail intentionally use a centered native-size phase; stock doors and switches
retain their separate exact-fit transform contracts.

`AddWall` also indexes unordered vertex pairs. Two opposite one-sided faces in
the same sector describe an internal chamber/corridor seam, so they collapse to
one nonblocking two-sided partition with no middle texture. A second geometric
linedef is never serialized. This invariant matters at size 80: the formerly
failing Gothic seed accumulated hundreds of coincident junction lines, causing
the GL node builder to synthesize hole subsectors and leaving black floor and
ceiling polygons in the rendered view.

### 12.3 Explicit corridor sectors

Connections between different rooms are not represented by an ambiguous shared
grid edge. Equal-height joins receive a corridor sector sized by their
Narrow/Standard/Gallery/Grand profile. Unequal joins reserve a multi-flight
chain and divide every individual 8–64-unit connection into one sector per
8-unit rise or descent. The first stair sector differs from the source room by
eight units and the last matches the destination room; every intermediate
two-sided riser is player- and monster-open. Corridor side walls step outward
behind architectural returns at each room portal. This recess creates a
physical material boundary, so support or jamb textures never begin midway
through a flat chamber wall. Ceiling is the minimum adjacent ceiling,
raised to preserve at least 72 units of clearance when necessary. Light is the
average of adjacent room light within 160–208.

Doorless joins use aperture half-widths based on role:

- ordinary: 56;
- hub: 64;
- arena: 72;
- deep branch: 44.

The aperture is clamped to the smaller adjacent room dimension minus the larger
corner cut. Same-room connections use a 112-unit aperture half-width, or 128
units for hubs and arenas, clamped by the chamber corner profile.

### 12.4 Functional recessed doors

A door replaces the corridor sector with a closed sector whose floor and ceiling
start at the same height. The moving slab is 16 units deep. Each side receives
a distinct lowered approach sector whose ceiling equals the selected door's
native height. These approaches make the lintel physical, contain the moving
face below the adjacent room ceiling, and ensure that the slab never consumes
the entire connector depth. Static jambs fill the remaining corridor span. Two
one-sided track walls bound the slab, and two two-sided faces operate the door.

Door faces use UDMF special 12 (`Door_Raise`) with:

```text
arg0 = 0      operate on the back sector
arg1 = 16     speed
arg2 = 150    delay
playeruse = true
repeatspecial = true.
```

The front side is the room; the back side is the initially closed door sector.
Locked faces carry lock number 1, 2, or 3 and use red, blue, or yellow track
textures. The same color extends across four static approach-jamb segments, so
each keyed doorway presents at least six colored border surfaces. Ordinary
tracks use `DOORTRAK`. Track linedefs are one-sided, bottom-pegged walls so the
tracks remain stationary while the sector ceiling moves.

Door art preserves stock IWAD dimensions independently of the physical
aperture. Every graph manual/keyed/secret door retains a Standard-or-wider
(at least 128×64) passage rather than allowing compact art to narrow travel:

| family | native size |
|---|---:|
| `DOOR1`, `DOOR3` | 64×72 |
| `BIGDOOR1` | 128×96 |
| `BIGDOOR6` | 128×112 |
| other selected `BIGDOOR` and `MARBFAC` faces | 128×128 |
| Doom II `SPCDOOR1`–`SPCDOOR4` | 64×128 |

Locked doors remain 128×128 and use their key-colored `BIGDOOR` face. Techbase,
Industrial, Hell, Gothic, and Corrupted Tech select different ordinary subsets;
the Doom II-only `SPCDOOR` family is never emitted for Ultimate Doom.
For native width `tw` and emitted face width `w`, the horizontal crop is only
needed when `w < tw`; wider physical slabs retain unit horizontal art scale:

```text
offsetX = round(max(0, (tw - w)/2)),
```

which centers the recognizable motif rather than cropping only one edge. For a
door with native texture height `th` and visible vertical span `h`, both face
sidedefs use

```text
scaley_top = min(1, th/h).
```

The ordinary face width equals `tw`, so compact motifs end at their jamb rather
than overlapping the wall shoulders. Secret doors use a 64-unit hidden panel of
the adjacent 128-unit wall material and the same physical slab, while setting
the linedef secret flag.

Door selection is bounded. Locks and secrets always request a door; the start
is closed off as a safe staging area; requested reward/deep-transition doors
and a small fraction of arena transitions consume a normal-door budget of
`2 + S`; and at most one door is created per room pair.

### 12.5 Landmark sectors and exit

Multi-cell starts and hubs may receive a centered raised platform sector.
Arenas, key shrines, and exits use two 8-unit tiers whose four boundaries are
two-sided, traversable, top- and bottom-pegged step edges. Secret rooms mark
their base sector with `SECRET_MASK` (`special = 1024`), and reward placement
avoids nested landmark footprints so tangible supplies remain in that counted
sector. The exit platform uses `GATE1`, `EXITDOOR`,
light 224, and a single interior two-sided linedef with special 243
(`Exit_Normal`) and explicit `playercross` activation.

Reveal descriptors emit one of three bounded topologies. Pavilions use opposing
clipped-corner loops around a void moat, wall alcoves use a rectangular framed
inset, and false-wall chambers split an existing perimeter wall before extending
a solid shell into a verified empty neighboring grid cell. A thin closed door
sector bridges each opening. Tags 1500–1999 identify manual switch caches;
`Door_Open` special 11 targets those IDs at speed 16. Switch-use lines are
one-sided and occupy an exact centered 64-unit segment with the
64×128 `SW1COMP` or `SW1GARG` texture. Their zero origin, unit horizontal
scale, and `scaley_mid = 128 / wallHeight` show one switch motif in each axis;
they explicitly omit `playercross`. Hidden moving faces inherit the room wall
texture and mark the exterior line secret; framed variants use progressively
stronger accent materials. Raised platform sectors use IDs 2000–2999. Their
square, chamfered, or wall-backed perimeters join two or three untagged stair
sectors, and every 16-unit stair transition remains monster-open.

Liquid sectors use `FWATER1`, `BLOOD1`, `NUKAGE1`, or `LAVA1`. The latter two
serialize ZDoom UDMF `damageamount`, `damageinterval`, `damagetype`, `leakiness`,
and `damageterraineffect` where appropriate. Central, trench, paired, irregular,
and flooded-room reservoirs coexist with straight, staggered, and bent
watercourses. Each form is shallow: dry banks, an island, or a 64-unit causeway
preserve traversal instead of relying on swimmable deep water.

Optional lift sectors use IDs 3000–3999. A lift begins 32 units above its room,
owns an 80-unit square footprint and at least 64 units of raised-state headroom,
and exposes special 62 (`Plat_DownWaitUpStay`) with use/repeat activation on all
four faces. Each target is selected outside landmark anchors, reveals, perches,
keys, starts, exits, locks, bosses, and secrets. Serialized geometry retains at
least 96 units between the platform and room boundary, so it can lower for its
center reward without ever becoming the only route through the room.

### 12.6 Thing placement

Critical things snap to known cell centers. The player faces the first connected
cardinal direction. The start shotgun is placed 32 units forward. Keys occupy
the authored key cell, and the boss occupies the exit cell.

Rewards use a 17-slot center/cardinal/diagonal pattern distributed across room
cells; cells occupied by reveals, perches, fluids, lifts, or multi-cell
landmarks are excluded. This prevents dense secret bundles from stacking multiple artifacts
at one coordinate or hiding them inside unrelated feature sectors.
Enemies use a twelve-offset pattern distributed by progression rank and room
cell count. Every offset is clamped to

```text
safeX = max(32, halfWidth  - cornerCut - 20)
safeY = max(32, halfHeight - cornerCut - 20),
```

so narrow and heavily chamfered profiles cannot push an actor outside the
chamber. Enemies face the center of their assigned cell. Decoration uses the
related 18-unit clearance margin and a collision check against all emitted
things.

## 13. Serialization

The output begins with `namespace = "zdoom"` and serializes vertices, sectors,
sidedefs, linedefs, then things. Every thing is enabled for skills 1–5 and for
single-player, cooperative, and deathmatch flags; procedural launch itself is
currently restricted to single-player because generation and network-session
coordination are separate concerns.

The emitter returns success only if at least one vertex, sector, and linedef
exists. The loader additionally rejects an empty string. Semantic and geometric
validity is established by the external validator and real map load described
below.

## 14. Correctness invariants

The pipeline is organized around the following invariants.

### 14.1 Progression invariants

1. There is exactly one critical path from start rank 0 to the chosen exit in
   the selected spanning-tree substrate.
2. Each required key is on a limb anchored before its gate rank.
3. The locked boundary is exactly the critical-path edge entering its gate
   rank.
4. Branch growth cannot reconnect to a non-anchor critical-path cell.
5. Added circulation edges connect only equal lock stages.
6. Room composition does not merge different lock stages, incompatible locks,
   or special landmarks.
7. Every cross-stage connection is the sole keyed crossing of its planned cut;
   removing keyed door sectors leaves no normal-door or open-portal bypass.

### 14.2 Geometry invariants

1. Every one-sided boundary is blocking and textured.
2. No two-sided line pretends to be a solid blocking wall.
3. All sidedef sector indices and linedef vertex/side indices are valid.
4. Every sector is referenced and the sector-adjacency graph is connected.
5. Every door sector starts closed, has two faces separated by 16 units, and
   has exactly two stationary track walls plus two contained approach/lintel
   sectors.
6. Each door face uses the correct special, activation flags, lock, texture
   native width/height, crop, and vertical scale.
7. There is exactly one player start and one player-cross exit trigger.
8. Every generated map has a readable room-scale sky landmark and at least one
   `SECRET_MASK` room behind a hidden door, with a tangible reward in every
   counted secret.
9. Meaningful gameplay sectors occupy small, medium, and large area bands; the
   90th percentile is at least twice the median and the largest is at least five
   times the median. Room-scale sectors include several dimensions, a clearly
   elongated form, compound silhouettes, and a size-scaled non-45-degree edge
   budget.
10. A Cyberdemon remains at least 144 units from the nearest solid wall, and the
   Spider Mastermind is never emitted by the coarse-cell boss policy.
11. Every serialized usable, one-sided switch targets an existing initially
    closed cache-sector ID with valid `Door_Open` arguments. Every `Door_Open`
    is `playeruse` and never `playercross` activated; maps may omit this
    optional cache when its clearance proof cannot be retained.
12. Every switch owns one exact 64-unit panel with a single fitted 64×128 motif,
    and every reveal owns the 64-, 80-, or 96-unit door and bounded topology of
    its selected alcove, pavilion, or false-wall family.
13. Every reveal preserves full pavilion circulation, a wall-alcove front
    approach and exposed backing wall, or a uniquely reserved empty exterior
    cell, together with headroom, actor containment, and trigger targeting.
    Multi-reveal maps vary family, cue prominence, and entrance axis. Every
    manual cache retains both ammunition and health rewards after shaping.
14. Every liquid uses an IWAD-common animated flat, contains no initial thing,
    and is lowered by only 8 or 16 units. Reservoir banks target 80 units and
    validate at 64; watercourses retain a 64-unit causeway or dry bypass;
    flooded rooms retain a dry island. Hazardous damage fields exactly match
    nukage or lava semantics. The dominant liquid feature is macro-scale and
    liquid area occupies a size-scaled share of the playable floor plan.
15. At least one square, chamfered, or wall-backed ranged platform rises 48 or
    64 units above an adjacent room, contains a ranged actor, and reaches that
    room through a complete sequence of 16-unit sectors with no
    `blockmonsters` flag on the access route.
16. At least one 80-unit lift rises exactly 32 units, owns four valid use/repeat
    action edges, has 64 units of headroom, contains a reward, and retains a
    96-unit bypass.
17. Ordinary traversable sector boundaries, including raised-platform stairs,
    have at least 56 units of headroom and no floor step above 24 units;
    retaining sides, closed doors, and lifts are validated separately.
18. The exit trigger lies on a `GATE1` pad with four `EXITDOOR` borders and two
    complete 8-unit stair tiers, every present key color has at least six
    matching doorway-border segments, and every map contains at least two
    open-sky sectors.
19. Collinear one-sided segments in the same sector never change material at a
    shared point unless a fitted switch occupies the split; ordinary material
    transitions require a corner, recess, portal, jamb, step, or platform seam.
20. Every solid decoration remains outside the serialized approach rectangles
    of every traversable portal, operable door, lift, stair route, and shallow
    landmark tier.
21. No two serialized linedefs occupy the same geometric segment; every sector
    boundary vertex has one incoming and one outgoing edge, and every boundary
    loop closes with nonzero signed area.
22. Playable floors span at least 96 units and contain at least eight distinct
    levels; full-width 8-unit route risers meet a size-scaled minimum count.
23. Size-5 and larger maps contain a raised same-stage sightline window with at
    least 64 units of opening height, monster-blocking aperture edges, and no
    new traversable progression connection.

### 14.3 Economy and compatibility invariants

1. The start room has no encounter and supplies a forward shotgun.
2. Random placement never emits an Arch-Vile.
3. Monster counts remain inside size-scaled lower and upper bounds.
4. Ammunition occurs at least once per five ordinary monsters, direct recovery
   at least once per four monsters, and health/bonus/armor support at least once
   per two monsters in the structural metric.
5. Standard and larger maps contain at least two useful weapon milestones.
6. Doom II maps contain a super shotgun; Ultimate Doom maps contain no Doom
   II-only monster, weapon, or lamp actor.
7. Difficulty pressure for the reference seed is nondecreasing from 1 to 5.
8. At fixed seed and size, the finale-room floor area grows strictly at each
   difficulty step.
9. Decorative things meet the scale- and detail-dependent minimum
   `4 + size × (detail + 1)`; explicit route stairs, windows, banks, and trim
   sectors do not each require a prop.
10. The automatic blueprint contains a valid profile, cardinal orientation,
    key permutation, distinct motif set, arsenal track, finale, two-to-four
    macro stages, and a room-level plan whose high-pressure cards have threat
    budgets, optional armories contain an optional weapon, and finale approach
    retains a reserve. Every retained encounter card carries post-emission
    geometry/capacity/static-placement evidence and a required manual switch
    action when it is a switch cache. Stages expose requested/realized shape, landmark
    archetype, district role, vertical intent, planned `vertical_rise`, realized
    `realized_vertical_rise`, and gate timing; every
    non-flat stage has a matching main-route vertical anchor.
11. On the ordered main path, no more than two high-pressure cards are
    consecutive; Holding Line and Set Piece cards are followed by recovery or
    a meaningful weapon/reward choice.

## 15. Validation methodology

The primary regression driver is `test_procgen.sh`. It launches the actual
engine headlessly, requests a UDMF dump, parses the serialized document, and
also enters `PROCMAP` through the normal runtime loader. This is stronger than a
test that reads only internal room objects: serialization mistakes, unknown
textures, node-builder failures, and map-loader integration errors remain
observable.

`test_procgen.sh replayability` is intentionally sequential. Its curated
recipe corpus runs each recipe twice for UDMF and twice for the schema-1
manifest in fresh processes, requires byte-identical pairs, then checks the
manifest's planned beats/cards/motifs/arsenal/ammo/reward/finale,
footprint/material/elevation/connector metadata, card-feasibility evidence,
and additive post-emission accessibility- and visual-proof contracts. The determinism gate also dumps
one recipe twice through the same live generator singleton, ensuring a
feasibility fallback cannot mutate cached recipe planning for the next call.
It also compares paired difficulty-1 and difficulty-5 manifests to ensure the
critical-path threat curve rises, and derives a sector-graph component for
each player-visible signature. Across
the corpus it requires all five profiles, four orientations, all five themes,
three arsenal tracks, four finales, five motifs, all eight encounter cards, five requested
and realized macro shapes, seven landmark archetypes, six district roles, and
the implemented stair-hall, dogleg, terrace-overlook, and bridge-approach
vertical intents, plus visual footprint/material/connection diversity from
the required current schema-1 fields, and two distinct
player-visible structural/beat
signatures per profile.

### 15.1 Structural parser

An embedded Python parser extracts every UDMF block and verifies:

- reference ranges and nonzero lines;
- boundary winding consequences, middle textures, blocking, and pegging;
- finite, positive sidedef transforms using active-IWAD texture metrics and
  intentional alignment groups, plus exact single-copy switch-panel
  dimensions/scales;
- door topology, motion semantics, keyed tracks, Standard-plus physical slab
  width, 64/128-unit native art width, 72/96/112/128-unit lintel height, fitted
  art, contained approaches, and slab depth;
- lock-cut topology reconstructed with every keyed door sector removed, proving
  that normal doors and open portals do not reconnect either gate approach;
- manual switch-cache targets, use-only activation (never player-cross),
  closed slabs, cache rewards, and family-specific 64-, 80-, or 96-unit moving
  faces;
- topology-based reveal validation for clipped pavilions, framed wall alcoves,
  and false-wall chambers, including loop closure, reserved exterior cells,
  approach clearance, cue diversity, actor containment, and cache contents;
- ranged-platform footprint and elevation across square, chamfered, and
  wall-backed families, plus straight, offset, and dogleg player- and
  monster-open 16-unit stair paths to room level;
- animated liquid availability in both IWADs, exact harmless/hazardous fields,
  actor-free sectors, dry banks/islands/causeways, floor-plan share, natural
  multi-segment shores, and macro scale relative to ordinary sectors;
- three sector-area bands, median/90th-percentile/largest-area ratios, diverse
  room dimensions and aspect ratios, compound/non-45-degree silhouettes, and
  irregular 368/384/400-unit module cadence;
- raised monster-blocking sightline windows between distinct same-stage rooms;
- lift dimensions, height, headroom, action semantics, reward, and 96-unit bypass;
- ordinary traversal headroom and step height;
- graph-planned terrain range, level critical thresholds, 8-unit stair chains,
  and no individual walking transition above 64 units;
- exit activation/material language, complete stair tiers, and absence of
  obsolete specials;
- keyed doorway-border color coverage;
- sky size/light and global minimum light;
- canonical `SECRET_MASK` sector values, rejection of untranslated special 9,
  secret-door reachability, tangible per-secret supplies, and powerup placement
  inside a counted secret;
- a substantial diagonal-line ratio;
- at least eight distinct one-sided boundary lengths and six clear heights;
- start shotgun position;
- 160-unit player-start wall clearance and 144-unit Cyberdemon wall clearance;
- decoration clearance, accelerated by line and thing spatial indices;
- heavyweight boss clearance;
- sector connectivity and reference coverage;
- coincident-linedef rejection plus directed per-sector boundary reconstruction,
  including exact degree, loop closure, and nonzero-area checks.

Shell-level checks add key/lock cardinality, size-scaled sector/thing/monster
budgets, direct and bonus recovery ratios, decoration density, weapon milestones,
five-theme semiotics, texture diversity, guarded coordinate limits, player-radius
clearance around critical navigation anchors and manual switches, and IWAD actor
compatibility. The symbolic key-state solver traverses serialized sectors with
an inventory of blue/red/yellow keys, proving start → keys → matching gates →
exit independently of the lock-cut check. For size 3–160, the validator requires
at least eight non-track wall textures, eight floor textures, and six non-sky
ceilings.

### 15.2 Representative matrix

The historical representative matrix below records the July 2026 size-80
release candidate. The current `maxsettings` and runtime-load gates additionally
cover the supported size-160 maximum, while the sequential replayability corpus
covers every automatic blueprint dimension:

| Seed | Theme | Difficulty | Size | Sectors | Things | Monsters | Decorations | Locks | Keys |
|---:|---|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | techbase | 2 | 1 | 135 | 189 | 40 | 69 | 2 | 1 |
| 42 | hell | 3 | 2 | 144 | 227 | 53 | 81 | 2 | 1 |
| 99 | industrial | 3 | 3 | 199 | 325 | 61 | 122 | 4 | 2 |
| 123 | gothic | 4 | 4 | 232 | 401 | 108 | 144 | 4 | 2 |
| 999 | corrupted | 5 | 5 | 290 | 524 | 161 | 168 | 6 | 3 |
| 20260713 | hell | 5 | 20 | 1,056 | 1,631 | 530 | 482 | 6 | 3 |
| 8080 | industrial | 3 | 80 | 5,231 | 6,458 | 1,288 | 2,326 | 6 | 3 |

All seven documents passed the complete structural validator on 2026-07-18.
The fixed determinism case produces byte-identical output on repeated runs with
SHA-256
`eb02baefb951a8785eb40137709b0347b0982d06f49e3b8182a33cb31068912c`;
a neighboring seed produces different output.

### 15.3 Difficulty experiment

Holding seed 2024, techbase theme, and size 3 constant produces:

| Difficulty | Monsters | Ammo pickups | Health + armor pickups | Finale area |
|---:|---:|---:|---:|---:|
| 1 | 48 | 37 | 62 | 573,544 |
| 2 | 55 | 39 | 67 | 859,192 |
| 3 | 65 | 35 | 67 | 1,123,048 |
| 4 | 94 | 40 | 69 | 1,416,408 |
| 5 | 97 | 40 | 68 | 1,710,880 |

The pressure curve is monotonic, and actual emitted finale floor area increases
at every difficulty step. The raw pickup count understates late support because
major fights upgrade individual pickups to boxes and cell packs. This experiment
demonstrates the intended count/space curve; it does not claim equal completion
rates for players of different skill.

### 15.4 Runtime and compatibility tests

Runtime tests enter size-1 Techbase, size-3 Hell, size-5 Industrial, size-20
Gothic, and maximum size-160 Corrupted Tech maps through `+map PROCMAP`, require the `PROCMAP` level banner,
and reject generation, texture, map, connection, and node-builder errors.
An additional fixed regression runs seed `1771465796` at size 80 through all
five themes. It validates the serialized geometry and passage-clear decoration
contract, requires five distinct authored theme outputs, and enters every result
through the real node builder. Developer diagnostics reject both `Unclosed
loop` and `Adding dummy subsector`, so a document that parses but still needs a
renderer-hole repair cannot pass. The seed exercises both the large-map switch
host proof and the formerly black Gothic four-way junction directly. A separate
suite applies the same structural and node diagnostics to five unrelated
size-80 maps, including the maximum positive signed seed.
Separate Ultimate Doom cases exercise all five themes at difficulty 3–5,
explicitly reject every known Doom II-only actor in the generator vocabulary,
and load every theme through the node builder to verify its shared switch,
exit, keyed-border, material, and prop vocabulary.

The menu regression reads the packed `MENUDEF`, verifies every setup control,
checks native reinsertion into mod-replaced main menus, validates persistent
defaults and random seed generation, and enters a randomized Hell map through
the user-facing command. It also verifies that oversized replacement list menus
retain mouse-wheel, page, home/end, and selection-follow scrolling.

The settings regression holds seed, theme, difficulty, and size constant while
changing one style input at a time. It requires Exploratory to emit more
topology than Directed, Dramatic to exceed Gentle elevation range, Lavish to
add both interactive tagged structures and props over Sparse, and Open-Air to
emit more sky sectors than Enclosed. It then runtime-loads all-low and all-high
recipes with developer node diagnostics. A separate identical-recipe theme
matrix requires five distinct documents and checks authored differences in
outdoor cadence, lift machinery, average clearance, mixed wall vocabulary, and
multi-color lighting. Finally, `maxsettings` applies Exploratory, Dramatic,
Lavish, and Open-Air simultaneously at size 160 and runs both the complete
serialized audit and real developer-level node construction. The historical
size-80 output metrics remain a useful baseline, but are not the current
maximum-size claim.

The fixed-seed feature matrix proves all eight fluid profiles (central, trench,
paired, irregular, flooded-room, straight, staggered, and bend), harmless and
hazardous mixes, broad grottos, long watercourses, all reveal families and cue
levels, and every perch/approach family. A ten-minute interactive Doom II plus
Brutal Doom soak at developer level 3 exercised movement, firing, switches, and
the automap with zero successful `GetCrosshair` lifecycle notices. A 30-second
developer-level-4 control produced 149 starts and 149 finishes, confirming that
deep tracing remains available while ordinary developer diagnostics stay quiet.

## 16. Complexity and performance

Let `C = (W - 2)(H - 2)` be interior canvas cells, `R` composed rooms, `V`
emitted vertices, `L` linedefs, and `T` things.

- Spanning-tree traversal, exit selection, final grid cleanup, adjacency
  extraction, and room BFS are `O(C)`.
- Branch and landmark budgets are bounded by size 1–160 in the shipping interface;
  generalized growth is linear in accepted cells times four cardinal neighbors.
- Room composition scans each growing room frontier repeatedly. With bounded
  target sizes it is effectively linear in `C`; without those bounds its
  conservative worst case is quadratic.
- UDMF emission is linear in cells and connections; quantized vertex
  deduplication uses a hash map with expected `O(1)` insertion and lookup.
- Decoration collision checks scan prior things and can approach `O(T^2)`.
- Serialization is `O(V + R + L + T)` in record count and output size.
- The external validator indexes sector lines and buckets things spatially,
  avoiding a full line/thing scan for every sector during large-map audits.

The largest supported canvas has 58,710 interior cells at size 160, although
only the selected route, branches, and landmarks are emitted. Retained-cell
centering and extreme reflow keep authored geometry inside the generator's
guarded coordinate envelope. Hash-based vertex lookup keeps this practical;
spatial bucketing for thing clearances is the clearest remaining optimization.

Memory use is linear in the canvas, room graph, intermediate UDMF records, and
final string. Generation is synchronous during map opening; it does not retain a
second parsed map after ownership transfers to `MapData`.

## 17. Failure handling and security posture

Generation clears its previous state and error string at the start of every
call. It fails explicitly on an unusable route, incomplete parent chain,
missing key branch, an inaccessible required anchor or reserved approach,
empty UDMF, or empty core geometry. The
map factory logs the error and returns `nullptr`, allowing the normal engine
path to reject the map. Optional switch caches, ranged perches, and lifts are
instead omitted when their clearance or geometry proof cannot be satisfied.

Unlike a user-supplied UDMF, generator text is produced from fixed format
strings, bounded numeric inputs, fixed texture vocabularies, and internal actor
tables. Theme input influences table choice but is never interpolated as raw
UDMF syntax. This substantially limits injection risk. Nevertheless, the
generated text deliberately passes through the normal parser and node builder,
which provides the same structural checks used for external maps.

The singleton is synchronous and is not designed for concurrent generation.
Network games are rejected by the launch command because deterministic map text
alone does not implement peer negotiation, content verification, or synchronized
new-game lifecycle.

## 18. Limitations

The present system has deliberate boundaries:

- The planner embedding is cardinal and grid-based. Uneven module cadence,
  asymmetric per-face profiles, compound L/T growth, non-45-degree shoulders,
  windows, and landmarks obscure that substrate, but arbitrary-angle room graphs
  and vertically overlapping rooms are not synthesized.
- Five visual themes have dedicated phase tables and architectural rules built
  from IWAD-safe material vocabularies. Additional themes still need coherent
  geometry, lighting, transitions, landmarks, and semantic props rather than
  merely substituting random textures.
- Encounter balance uses counts and tiered families, not a formal estimate of
  hit points, damage exposure, infighting opportunity, or player inventory
  simulation.
- Multiplayer launch is unsupported.
- Generated things enable every skill flag because generation difficulty is
  applied while constructing the map; one serialized map is not a five-skill
  remix.
- Geometry validation proves topological and budget properties, but automated
  tests do not replace human evaluation of sightlines, combat rhythm, or visual
  composition.
- Fresh-map determinism is version-scoped. Algorithm or table changes may
  intentionally change the map produced by an old seed. Existing savegames
  archive the exact UDMF and therefore retain their original base geometry.
- Fluids are shallow classic-Doom sectors with banks, islands, and causeways;
  deep-water transfer heights, swimming, waterfalls, and stacked 3D volumes are
  outside the current compatibility contract.
- Decoration clearance still has quadratic worst-case behavior, which remains
  practical at the guarded size-160 bound but is the next scale bottleneck.

## 19. Future work

Promising extensions preserve the staged architecture rather than collapsing
it:

1. Extend the serialized-sector symbolic key-state solver into automated play
   traces that operate switches, collect weapons, and complete exits.
2. Generalize the five authored themes into data-driven packages containing
   palettes, semantic props, monster-family policies, and landmark templates
   with automatic IWAD capability checks.
3. Estimate encounter cost from monster hit points, projectile pressure,
   available cover, and supplied weapon damage rather than count alone.
4. Add spatial buckets for actor/decor clearance before expanding beyond the
   size-160 interface ceiling or changing the canvas aspect ratio.
5. Add visibility and crossfire metrics after node construction, feeding a
   bounded repair pass that can adjust portals or encounter anchors without
   changing progression.
6. Support deterministic cooperative generation through server-authored
   settings, map checksums, and explicit peer synchronization.
7. Store a generator schema/version beside shared seeds so older generation
   semantics can be reproduced intentionally.
8. Add automated play traces for reachability, key acquisition, door use,
   weapon pickup, and exit completion.

## 20. Reproduction

Build BiasedDoom and run the complete generator checks:

```bash
cmake --build build --config Release
./test_procgen.sh validate
./test_procgen.sh determinism
./test_procgen.sh replayability
./test_procgen.sh features
./test_procgen.sh doors
./test_procgen.sh rewards
./test_procgen.sh balance
./test_procgen.sh doom1
./test_procgen.sh load
./test_procgen.sh menu
./test_procgen.sh music
./test_procgen.sh settings
./test_procgen.sh themes
./test_procgen.sh extreme
./test_procgen.sh huge
./test_procgen.sh maxsettings
```

Inspect one document directly:

```bash
./build/biaseddoom -iwad /path/to/doom2.wad \
	+dumpprocudmf 42 hell 3 3 2 2 2 2 +quit
less /tmp/procmap_test.udmf

./build/biaseddoom -iwad /path/to/doom2.wad \
	+dumpprocmanifest 42 hell 3 3 2 2 2 2 +quit
less /tmp/procmap_manifest.json
```

Start the same map through the normal loader:

```bash
./build/biaseddoom -iwad /path/to/doom2.wad \
  +procgen_seed 42 +procgen_theme hell \
	+procgen_difficulty 3 +procgen_size 3 \
	+procgen_layout 2 +procgen_verticality 2 \
	+procgen_detail 2 +procgen_outdoors 2 +map PROCMAP
```

## 21. Implementation map

| Source | Responsibility |
|---|---|
| `src/common/maps/procgen.h` | cell/room state, `RunBlueprint`, and generator interface |
| `src/common/maps/procgen.cpp` | CVars, UDMF/manifest console commands, and in-memory `MapData` factory |
| `src/g_game.cpp` | exact procedural-map save archive and staged restoration |
| `src/m_misc.cpp` | final-frame screenshot request processing |
| `src/common/maps/procgen/procgen_core.cpp` | recipe hash, RunBlueprint, mission graph, embedding, branches, keys, locks, loops, landmarks |
| `src/common/maps/procgen/procgen_rooms.cpp` | room composition, graph analysis, visual grammar, pacing, economy, secrets |
| `src/common/maps/procgen/procgen_udmf.cpp` | sectors, chambers, corridors, doors, things, UDMF serialization |
| `src/common/maps/procgen/procgen_internal.h` | grid directions and shared actor tables |
| `src/p_openmap.cpp` | procedural-name interception in the normal map loader |
| `src/playsim/procgen_zscript.cpp` | native ZScript bridge |
| `wadsrc/static/zscript/procgen/procgen.zs` | public ZScript declarations |
| `wadsrc/static/menudef.txt` | player-facing generator configuration |
| `src/common/menu/menudef.cpp` | reinsertion into mod-replaced main menus |
| `test_procgen.sh` | serialized geometry, key-state, replayability manifest, balance, compatibility, menu, and runtime tests |

## 22. Conclusion

BiasedDoom's procedural generator treats a playable Doom map as a sequence of
contracts rather than a single random geometry problem. Progression is authored
first, embedded second, spatially composed third, paced fourth, and serialized
last. Locks and loops are constrained while the graph is still explicit;
materials and encounters operate on semantic rooms; macro liquids, sightline
windows, reveals, and perches reserve compatible space before emission; and UDMF
geometry is built from closed, testable primitives. The result is a generator
whose maps vary in route, sector scale, compound silhouette, height, material,
liquid geography, encounter, landmark architecture, and now player-visible run
identity while retaining
deterministic reproduction, key/lock safety, renderer-valid walls, functional
doors, bounded difficulty, and normal engine compatibility.

The architecture does not eliminate the need for human playtesting. It does,
however, move a large class of failures—unreachable exits, bypassed locks,
invisible walls, malformed doors, missing resources, unsupported actors, and
non-reproducible seeds—from subjective testing into construction rules and
executable validation. That separation is the principal result of the work.
