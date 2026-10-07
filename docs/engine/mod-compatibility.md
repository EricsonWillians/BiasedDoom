# Classic-Mod Compatibility

BiasedDoom keeps the established Doom mod loading model: WAD/PK3 resources,
DECORATE, ZScript, ACS, DeHackEd/BEX patches, and the usual command-line load
order remain the compatibility baseline. This guide describes the current
DeHackEd-family behavior that matters most for large weapon packs. It does not
turn every third-party mod into a supported engine contract; a mod may still
depend on another package, a specific IWAD, or behavior outside this engine's
compatibility surface.

## DeHackEd, BEX, MBF21, and DSDHacked

Traditional `.deh` and `.bex` files can be supplied through their normal
command-line/configuration paths, and a resource archive can provide an
embedded `DEHACKED` lump. Later patches still take precedence when they change
the same game data, so preserve the mod author's documented load order.

MBF21-capable mods commonly use **DECOHack**, which emits the extended
**DSDHacked** dialect. BiasedDoom identifies that dialect from `Doom version =
2021` in the patch header. Its `[SPRITES]` table may refer to sparse numeric
IDs instead of the compact sequence used by older DeHackEd files; `8000` is a
common DECOHack starting point.

Current builds resolve those DSDHacked sprite entries through a sparse mapping.
They do not resize the legacy dense sprite-name table to the highest supplied
number. This is important for both compatibility and robustness: a valid
weapon sprite at `8000` or another signed 32-bit ID is accepted, while one
unreasonably large number cannot force a giant allocation.

The sparse mapping has a deliberate limit of **262,144 distinct extended
sprite IDs per loaded patch set**. Replacing an existing ID remains valid when
the budget is full. Values outside the signed 32-bit range, malformed numbers,
or new entries beyond the budget are rejected with a console warning. There is
no CVar to lift the limit; it is a memory and hostile-input safety boundary,
not a gameplay restriction.

## What This Fixes—and What It Does Not

The previous guarded dense-extension path was suitable for ordinary legacy
patches but could reject otherwise valid sparse DSDHacked weapon entries. The
visible symptom was an invisible weapon sprite or an "out of range" diagnostic
even though the mod supplied the named art. The sparse path fixes that specific
class of issue for DECOHack/MBF21-style content.

It does not guarantee that every MBF21-labelled mod will run unchanged. A mod
can still fail because it is missing an asset or a dependency, is loaded in the
wrong order, expects a different game/IWAD, replaces non-DeHackEd engine code,
or uses a feature outside the engine's supported behavior. Treat the mod's own
documentation as authoritative for its required IWAD and companion packages.

## Diagnosing A Missing Weapon Sprite

Launch the smallest reproduction with stdout and a log:

```text
biaseddoom -config deh-test.ini -iwad doom2 -file weapon-mod.pk3 -stdout +logfile deh-test.log
```

Then check the log in this order:

1. Confirm the expected `Adding dehacked patch ...` and `Patch installed`
   messages appear.
2. Search for `Sprite number ... out of range.`. On a current build this means
   the input was malformed, outside the signed 32-bit range, or exceeded the
   safety budget—not that a normal `8000`-series DECOHack ID is unsupported.
3. Search for `Frame ... Sprite ... is undefined`. That means the patch mapped
   the frame but the named four-character sprite was not available from the
   loaded resource set.
4. Reproduce with the mod's required packages in its documented order, then
   add optional gameplay/visual packages one at a time.

For a source build, maintainers can exercise the parser and sparse mapping
without redistributing a third-party mod:

```bash
./tools/test-dehacked-extended-sprites.sh --iwad /path/to/DOOM2.WAD
```

The fixture covers the conventional `8000` range, a valid `INT_MAX` mapping,
more than 65,536 sparse entries, and safe rejection above `INT_MAX`.

## Related Compatibility Notes

- A game compatibility profile may intentionally disable MBF21 behavior for a
  map that requires older semantics. Do not change compatibility settings just
  to solve a missing sprite; sprite parsing and map compatibility are separate
  concerns.
- BiasedDoom's procedural generator is a separate Doom/Ultimate Doom and Doom
  II feature. Its game-data restrictions do not limit normal maps or
  DeHackEd/DSDHacked weapon mods loaded with other supported games.
- For a crash, hang, or a mod that behaves differently after its sprites load,
  use the reduction procedure in the [root troubleshooting guide](../../TROUBLESHOOTING.md)
  and attach the smallest reproducing command line and log.
