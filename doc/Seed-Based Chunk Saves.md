# Seed-Based Chunk Saves

Cube-Craft worlds store generated terrain as a seed plus sparse edits:

```text
final chunk = generate(seed, worldgen version, chunk coordinate) + apply(diff)
```

Untouched chunks have no chunk file and use no disk space.

## Files

```text
media/map/<world>/
  world.meta
  world.cfg
  audit.log
  chunks/<x>_<y>_<z>.diff
```

`world.meta` records the seed, worldgen version, worldgen parameter hash, save
format version, entry chunk, and spawn. Existing saves reject incompatible
worldgen settings instead of silently changing their terrain.

Each `.diff` contains checksummed semantic edit records and, after compaction,
a sparse snapshot containing only octree nodes that differ from the generated
base. It never stores raw pointers or live octree memory.

## Editing and Saving

An edit changes the live chunk immediately and adds a revisioned operation to
that chunk's in-memory journal. Every ten seconds the active journal is swapped
into a flush buffer and written by a background thread, so the game thread does
not perform compression or disk I/O.

Diff frames contain their chunk coordinate, revision, timestamp, optional
player ID, operation, payload, and checksum. Supported operations include cube,
material, corner, volume, and blueprint edits.

## Loading

Loading generates the deterministic base chunk, replays its compacted sparse
snapshot and newer journal operations, canonicalizes the affected octree, and
checks the resulting chunk hash. Invalid or incomplete frames are detected and
valid earlier revisions can still be recovered.

## Compaction and History

Compaction regenerates the base, applies saved and pending edits, merges
identical octree children, and writes only differences. Overrides are removed
when terrain is restored to its generated state.

`audit.log` is separate from compacted chunk state, so undo, redo, and
player/time-based rollback history survives compaction and restart. Rollbacks
are committed as new revisions rather than deleting history.

Useful commands:

```text
/worldundo [count]
/worldredo [count]
/worldlog [player] [radius] [minutes]
/worldrevert player <name> [minutes]
/worldrevert area <x1 y1 z1> <x2 y2 z2> [minutes]
/worldrestore chunk <x y z> <revision>
/worlddiff stats [x y z]
/worlddiff compact [x y z|all]
/worlddiff verify [x y z|all]
```

In multiplayer, the server owns revisions and authoritative diffs. Normal
updates replicate semantic operations; a revision or hash mismatch requests
the authoritative compacted diff, with full chunk transfer reserved as a
compatibility fallback.
