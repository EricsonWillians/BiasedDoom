# Python Scripting Performance Guide

This page is for mod authors whose Python scripts stutter, get budget-disabled,
or trip `SCRIPT WARNING` budget messages. It explains the cost model, how to
measure it, and the three rules that fix almost every slow script.

## The C-crossing cost model

Python runs **synchronously on the engine thread**. Every `bd.*` call, every
handle property read or write, and every event dispatch crosses the
CPython/C++ boundary. One crossing is cheap; thousands per tic are not. The
canonical slow pattern is a per-tic Python loop that touches every actor —
each iteration pays at least one crossing, so a 500-actor scan costs 500+
crossings per tic, before your own logic runs.

The engine enforces a whole-tic wall-clock budget, configured by CVars
(see `src/python/python_runtime.cpp`):

| CVar | Default | Meaning |
|------|---------|---------|
| `py_tick_budget_ms` | `3` | Wall-clock milliseconds all Python callbacks and scheduled tasks may consume per tic. `0` disables enforcement and warnings. |
| `py_tick_hard_budget` | `true` | When the budget is consumed, skip remaining Python callables until the next tic. |
| `py_tick_overrun_limit` | `3` | Disable a callback (or cancel a repeating task) after this many consecutive individual overruns. `0` disables repeat-offender removal. |
| `py_max_tasks` | `4096` | Upper bound on the scheduled-task queue. |

A single callable cannot be interrupted mid-flight: it may exceed the budget
once, the runtime logs the overrun, and later work in that tic is skipped.
`py_reload` re-enables budget-disabled callbacks. When Python is not opted
in there is no cost at all — CPython is never initialized.

## Measuring: `bd.profile()` and `bd.reset_profile()`

`bd.profile()` returns per-callback and per-task timing plus the current
budget settings; `bd.reset_profile()` clears the counters (it does **not**
re-enable disabled callbacks). A typical result:

```python
{
    "callbacks": [
        {
            "event": "tick", "source": "my_mod.main:on_tick",
            "calls": 3500, "total_us": 412300, "max_us": 980,
            "budget_skips": 12, "budget_overruns": 2,
            "every": 1, "priority": 0,
            "failed": 0, "budget_disabled": 0,
        },
        # ...
    ],
    "tasks": [
        {
            "id": 7, "source": "my_mod.main:spawn_wave",
            "due_tick": 1050, "repeat": 35, "calls": 100,
            "total_us": 81200, "max_us": 640,
            "budget_skips": 0, "budget_overruns": 0, "map_local": 1,
        },
    ],
    "tick_budget_ms": 3, "hard_budget": True, "overrun_limit": 3,
    "budget_overruns": 2, "budget_skips": 12,
}
```

Reading it:

- **`total_us / calls`** is the average cost per invocation. For a `tick`
  callback at 35 Hz, every 28.5 µs of average cost consumes 1% of the
  default 3 ms budget; a callback averaging ~1 ms eats a third of it alone.
- **`max_us`** shows the worst tic — spikes here are what players feel.
- **`budget_skips`** counts tics where earlier work had already exhausted the
  budget and this callable never ran. **`budget_overruns`** counts times the
  callable itself blew the budget; reaching `py_tick_overrun_limit`
  consecutive overruns sets **`budget_disabled`** and the callback stops
  firing.
- A nonzero **`failed`** means the callback raised and was permanently
  skipped until `py_reload`.

Profile release builds too — debug interpreters are significantly slower.

## Rule 1: Push filters down into C

Filtering with `bd.actor_refs` keyword arguments happens inside the native
thinker scan — the discarded actors never cross into Python.

**Before** — 500 crossings per tic, all in Python:

```python
def on_tick(event):
    for a in bd.actors(limit=100000):            # full-level snapshot scan
        if a["class_name"] == "DoomImp" and a["alive"]:
            dx, dy = a["x"] - px, a["y"] - py
            if dx * dx + dy * dy <= 512 * 512:
                burn(a)
```

**After** — two crossings, natives do the walking:

```python
def on_tick(event):
    imps = bd.actor_refs(class_name="DoomImp", sphere=(px, py, 512))
    bd.apply_actor_batch([("damage", imp, 2) for imp in imps])
```

`class_name` matches derived classes unless `subclasses=False`;
`sphere=(x, y, r)` is a 2D radius, and the optional `z=` kwarg adds a
vertical band. `bd.sector_at(x, y)` + `bd.actors_in_sector(sector)` cover
room-scoped queries without scanning the map.

## Rule 2: Batch mutations and reads

- **`bd.apply_actor_batch(operations)`** applies many mutations in one
  crossing and returns the number applied. Operations whose `Actor` handle
  went stale since the batch was built are skipped (a rate-limited warning
  reports how many); any *other* invalid operation aborts the batch at the
  first failure — validate generated batches before submitting them.
- **`bd.actor_field_batch(refs, fields)`** reads whitelisted fields
  (`health`, `x`, `y`, `z`, `angle`, `pitch`, `roll`, `speed`, `alpha`,
  `tid`, `class_name`, `alive`, `is_player`, `is_monster`, `special`,
  `damage_factor`) for many actors in one crossing, returning one tuple per
  ref. Stale handles yield all-`None` tuples instead of raising.

## Rule 3: Throttle hot callbacks

- **`bd.on(..., every=N)`** dispatches only every Nth matching event —
  free throttling for high-frequency events.
- **`bd.schedule(cb, repeat=35)`** turns per-tic bookkeeping into a
  once-a-second task under the same budget rules.
- **Modulo gating** (`if event["level_time"] % bd.TICRATE != 0: return`)
  remains the simplest manual throttle.

## Budget checklist

- [ ] No full-level scans in `tick` — filters pushed into `bd.actor_refs`.
- [ ] Bulk mutations go through `apply_actor_batch`; bulk reads through
      `actor_field_batch`.
- [ ] Hot callbacks throttled with `every=`, `bd.schedule`, or modulo gating.
- [ ] No blocking disk, network, or subprocess work in any callback.
- [ ] `bd.profile()` shows `budget_skips == 0` and no `budget_disabled`
      callbacks under realistic load.
- [ ] Profiled a release build, not only debug.
