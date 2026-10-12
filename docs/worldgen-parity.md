# Native world generation parity (WORLD-06)

How closely the native generator (src/worldgen) matches the official 26.3 server, measured with `--gentest`
against worlds the server generated. Run it as:

```
minecraftCplusplus.exe --world <world dir> --gentest <world dir> 0          # overworld (GENTEST_DIM picks another)
GENTEST_STRUCTURES=starts minecraftCplusplus.exe --world <dir> --gentest <dir> 0   # structure starts / references
```

`--world` makes the test use that world's own data packs (otherwise the default world's packs are loaded).

What is compared:

- **Terrain**: chunks the server left at `minecraft:terrain` / `minecraft:biomes` (the edge of the generated area),
  block by block and biome by biome, against our noise, surface and carvers (with the structures' terrain
  adaptation, as the starts exist before the noise).
- **Decoration**: every `minecraft:full` chunk, after decorating it and its neighbours (features and structure
  pieces in the game's steps) and the structures' post-processing.
- **Structures**: starts, pieces and references of every saved chunk.

## Results (2026-10-11, master)

| World | Terrain (blocks differing) | Biomes | Decoration (blocks differing) | Structures |
|---|---|---|---|---|
| seed 12345 overworld | 148,744 / 28.3M (0.53%) | 0 / 897k | 2.88M / 468M (0.61%) | 55 / 55 starts, pieces identical; 5,625 / 5,625 references |
| seed 12345 nether | 70,516 / 18.9M (0.37%) | 0 / 598k | 2.53M / 312M (0.81%) | |
| seed 12345 end | 0 / 21.4M | 0 / 663k | 67 / 323M | |
| seed 777 overworld | 189,550 / 40.6M (0.47%) | 0 / 1.29M | 1.24M / 402M (0.31%) | 69 / 69 starts, pieces identical; 5,339 / 5,339 references |
| seed 5 overworld | 124,888 / 28.5M (0.44%) | 0 / 903k | 1.64M / 475M (0.34%) | |

What remains:

- **Terrain-status chunks** include blocks their already-decorated neighbours placed into them (ore blobs, the
  edges of structure pieces, trees); those show as differences although our terrain itself matches.
- **Decoration order**: the game decorates chunks in loading order, which is not reproducible; changing our order
  (`GENTEST_ORDER=reverse`, `zmajor`) moves the result by up to a quarter (2.9M to 3.7M on seed 12345), so part of
  the remainder is overlap order.
- **Feature positions**: a set of features land elsewhere than the server's in a fraction of chunks (coal ore, deep
  dark sculk patches, lush caves moss / clay pools, dark oak trees); the differences are symmetric (the same
  features, shifted), spread over chunk interiors, while other ores, blobs and most vegetation match. The feature
  order (FeatureSorter) and the per-chunk biome set follow the game's code; the cause is open.

Far terrain uses this generator only beyond the server's view distance, and the viewer for chunks the save does
not have; biomes, terrain shape and structures, what shows at a distance, match.
