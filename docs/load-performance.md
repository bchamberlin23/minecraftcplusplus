# World loading, saving, and dimension transitions

Audit and changes: October 11, 2026.

The main avoidable startup cost was loading every saved overworld chunk and building its coarse meshes before showing a frame. Singleplayer now preloads a circle of `detail + 16` chunks around the saved player. The official server supplies authoritative chunks as the player travels; distant terrain continues to use the existing background generation. The viewer keeps its whole-save default, and `--radius 0` explicitly restores whole-save preloading in singleplayer.

Creating/upgrading a world now overlaps the JVM's preparation with independent asset loaders. Once the server is ready, protocol setup/login can also run during GPU initialization and initial terrain construction. A new world whose chunks are still only in server memory can start with empty client storage.

Coarse terrain construction skips entirely empty vertical layers and fills uniform sections directly. It produces the same node counts and geometry budgets as the previous whole-save path in the comparison world.

Dimension changes now clear world storage and terrain meshes in bulk. Region objects remain alive because background jobs hold their addresses. Every detail, coarse, and far job carries a dimension epoch; results from an earlier dimension are discarded. This also removes old block entities and biomes. Logging into an existing save in the Nether or End clears the overworld disk preview immediately.

The network event queue discards obsolete terrain updates preceding a respawn packet while preserving control-event order and teleport acknowledgements. Cheap chunks can drain up to 32 per frame instead of eight, still within the existing 4 ms application budget. New logs report dimension-clear time and the delay from receiving a respawn packet to the player's destination chunk becoming resident.

Saving keeps unchanged compressed payloads as spans into the input region buffer. It allocates owned payloads only for edited chunks and reserves the output's exact sector space before repacking. Compression settings, backups, atomic replacement, and disk flushing are preserved. Successfully written regions clear their dirty flags; failed regions remain dirty for retry. The C++ saver is used for direct local edits; ordinary singleplayer saves are performed by the official server. On quit, local terrain workers stop accepting work so they compete less with the server's save.

## Validation

- Release build with MSVC, Ninja, and the existing renderer/shader changes in this workspace.
- `--world-io-test`: scratch-only checks for bulk clear, stable region addresses, consumer notifications, block entities/biomes, mixed palettes, light, negative coordinates, unchanged region payloads, backups, and deliberately blocked atomic writes followed by retry.
- Read-only comparisons against `New World (1)`, including before/after screenshots at the same camera, time, and weather.
- A separate scratch world created with seed 711; scripted Overworld → Nether → End → Overworld at 30 fps, then Save and Quit to Title. The real save files were used only by the read-only viewer.
- Saving while in the Nether and reopening directly there: the client cleared its overworld preview in 2.56 ms and the player chunk became resident 99.9 ms after login. Screenshots also checked the Nether roof and End terrain. A final creation run confirmed the local login completed before the first-frame startup log, exercising the GPU/login overlap.

Representative observations (32 CPU threads, RTX 5070 Ti, 960×540, shaders off):

| Work | Before | After |
| --- | ---: | ---: |
| Preloaded chunks in the comparison world, detail 8 | 21,159 | 1,793 at radius 24 |
| Preloaded block sections | 200,739 | 16,227 |
| Region data read | 213 MiB | 47 MiB |
| Initial coarse meshes | 71,384 | 5,939 |
| Synthetic world-storage unload: 1,024 columns × 24 sections, final regression run | 46.60 ms, repeated removal/compaction | 7.60 ms, bulk clear |

The scripted travel test measured complete client dimension clearing at 4.85 ms into the Nether, 2.14 ms into the End, and 0.57 ms returning to the Overworld. It discarded 134 obsolete terrain events before the Nether transition. Destination player chunks became resident 917.5 ms, 327.8 ms, and 65.5 ms after their respawn packets, respectively. These are individual observations, not guarantees for total portal travel or complete terrain visibility; server generation and sending are included in destination readiness.

Several unrelated builds and game instances were active during timing runs. End-to-end startup samples varied substantially, so they should not be treated as a controlled percentage speedup. The reductions in loaded data/mesh counts and the regression checks are the stronger evidence.

## Remaining large opportunities

Returning to title still starts a separate game process. In the captured run it took 9.03 seconds after launching that process; its graphics-provider initialization alone logged 3.465 seconds, followed by 1.871 seconds for initial coarse terrain. Reusing initialized assets/device state across title/world changes, or initializing unused graphics providers on demand, could remove more of this fixed cost. Those require a broader lifecycle change and separate testing of each upscaler, frame-generation provider, and shader-pack path.

The official JVM still owns first-time Nether/End generation, world upgrades, player synchronization, and normal singleplayer saving. Those costs must be measured separately from client teardown. The new readiness logs provide that separation; this change does not claim to optimize the JVM's region writer.

Until server chunks arrive, terrain outside the disk preview comes from the seed-based generator, so distant player-built structures can appear later. Use `--radius 0` when the complete saved preview matters more than startup work.

## Reproduce

```powershell
./build.ps1 -Dir Lead2
./build/Lead2/minecraftCplusplus.exe --world-io-test

$env:MCPP_LOG_TIME = '1'
./build/Lead2/minecraftCplusplus.exe --world 'New World (1)' --readonly --no-gen --detail 8 --radius 0 --shaders off --size 960x540 --seconds 1
./build/Lead2/minecraftCplusplus.exe --world 'New World (1)' --readonly --no-gen --detail 8 --radius 24 --shaders off --size 960x540 --seconds 1
```

Logs and screenshots from this session are in `build/load-audit/` (ignored build artifacts). The `minecraftCplusplus-before-load.exe` copy beside the tested executable preserves the original binary for comparisons.

## Smooth outward chunk loading

The follow-up change makes ordinary world entry prepare detailed meshes only within 64 blocks before the first frame. The remaining detail streams outward over later frames; coarse terrain stays available underneath it. Benchmarks, the existing gameplay self-test, and the title panorama retain their complete initial detail preparation.

Meshing dispatches at most 256 sections at a time and publishes each group of 64 as it finishes. Completed meshes are sorted by distance to the current camera, including after a teleport. Per-frame uploads have a 64-result limit, a 4 MiB byte budget, and a 1.5 ms soft time budget; one indivisible upload can exceed a byte/time budget. Backpressure bounds completed work waiting for presentation. Repeated neighbour/light refresh requests are coalesced, and new nearby detail receives most of each dispatch batch during chunk bursts.

Generated chunks are selected nearest to the current player rather than popped in reverse completion order. The main-thread generation backlog is also sorted nearest first. Completed far regions are applied nearest first. Network control/block events keep their protocol order; presentation prioritizes the chunks already available locally.

The additional outward visual fade was implemented and tested in the original working checkout. It extends renderer/shader changes that were already uncommitted when this task began, so that part is held separately until its foundation is committed or inclusion is authorized. This commit publishes the independent loading, saving, and nearest-first streaming changes without taking that other work. The authored extension is preserved in [pending-outward-fade.patch](pending-outward-fade.patch); apply it after the renderer foundation is available. It adds only this task's changes to the renderer and world-entry call sites.

At detail 16/radius 32 in the same saved comparison world, the first frame began with 163 nearby detailed sections instead of 11,250. A nine-second run at 30 fps finished with all 11,250 detailed sections resident and zero pending. The first five seconds of upload tracing advanced from 64 to 289 blocks away, never processed more than 64 results in a frame, and measured at most 1.21 MiB/0.38 ms per upload pass. Screenshots at 0.25, 0.6, 1.2, and 4.2 seconds showed nearby terrain filling before the farther landscape. These timings exclude sorting/cut construction and are observations on this machine, not universal frame-time guarantees.

Use `MCPP_STREAM_PROFILE=1` to log each detail upload pass. The follow-up build is isolated in `build/LoadSmooth/` so concurrent builds cannot replace the executable being tested.

The earlier isolated integration run, including the still-uncommitted renderer foundation, also passed direct Nether login, Nether → End → Overworld travel, Save and Quit to Title, and the save/clear regression suite. Each destination restarted the outward reveal at its new player position. A read-only run with missing nearby saved chunks exercised the generated-chunk delivery path. Tests used a separate copy named `load-smooth-test.exe` to avoid interfering with other game tests on the machine.

Before publishing, the independent changes were rebuilt in a clean checkout without any of the other uncommitted renderer/shader/audit files. The standalone save/clear suite passed, and a read-only detail-8/radius-24 viewer run reached 3,616 resident sections with zero pending after two seconds. The pending fade patch was separately checked against a scratch copy of its required foundation.
