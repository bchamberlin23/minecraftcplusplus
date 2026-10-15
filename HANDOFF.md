# Handoff — October 14, 2026

This covers the state at the end of the October 14 sessions. `CONTINUE.md` keeps the long-running notes; this file
lists what changed today and what is still open.

## State

- Branch `master`. Every commit is local; nothing is pushed (pushing was never authorized).
- `build\Lead` (the owner's build) was rebuilt at 23:20 and includes everything below. Test builds go to
  `build\Lead2`.
- The working tree is clean apart from `docs/audit-26.3/` and `tools/audit_26_3.py`. Those are untracked and not
  ours; leave them alone.

## Done today (latest first)

| Commit | What |
|---|---|
| `79528dd` | **World lock fairness.** The main thread's exclusive locks (`World::WriteLock`) announce themselves, and background readers (`yieldToWriters`) hold back meanwhile. While holding the lock, readers run above normal priority (`LockPriority` in `terrain.cpp`). Before this, chunk inserts waited 15–120 ms. |
| `e126b9d` | **LOD swaps without a cross-fade.** Full detail ↔ coarse, any two levels, swap at once (`Renderer::applyCutChange`). Chunk Fade only applies to terrain appearing from, or going into, nothing. |
| `f5ffccc` | **Shader-pack clouds are cached.** The pack vertices are rebuilt only when the cloud mesh or its colour changes. The drift goes through `PackDraw::translate`, and the vertices stay on the GPU via `PackDraw::gpuVertices`. Converting every face every frame cost 5.3 ms. |
| `222c104` | **Detail streaming.** Coarse region rebuilds run on their own thread (`Terrain::coarseMain`), and `Jobs::parallelFor` accepts several callers at once, serving the newest batch first. An arriving chunk queues its own sections (`Terrain::fresh_`) instead of forcing a whole rescan. |
| `2aed124` | **Console.** When double-clicked, the game releases the console Windows opened for it alone and logs to `%LOCALAPPDATA%\minecraftCplusplus\logs\latest.log`. |
| `fc6cf3e` | **Pause.** While paused, the entity world and the local visuals (hand, swings, particles) stop ticking. |
| `de15364` | **Joining a world.** The hotbar starts empty until the server's inventory arrives; the viewer's default blocks no longer flash. |

Result while flying in spectator with BSL in `--play`: **54 → ~290 fps**. That figure comes from interleaved
old/new runs; the machine was busy at the time, so only the relative change is meaningful.

## Open items

1. **Pause shaking while moving** (owner report). The fix in `fc6cf3e` was never confirmed: scripted runs could not
   reproduce the shaking with either the old or the new build. Ask the owner whether it still happens. If it does,
   look at the TAA/DLSS jitter and the motion vectors on paused frames.
2. **Coarse LOD visible close to the camera** while chunks have not arrived yet (server or local generator). Meshing
   keeps up now, so what remains is chunk arrival plus the generated-chunk apply, about 0.7 ms a frame on the main
   thread. Inserts and lighting could move off the main thread, but only once every main-thread world read takes
   the shared lock.
3. **Far LOD fade-in speckle at the horizon.** This is Chunk Fade's white-noise dither on terrain that is genuinely
   new, smeared by the upscaler. Consider fog instead, or no fade beyond the render distance.
4. **Remaining main-thread costs while flying:**
   - render CPU about 1.5 ms (`ent::StagePass` 0.4–1.9 ms)
   - terrain about 0.9 ms
   - generated-chunk apply about 0.7 ms
   - p99 frame about 12 ms
5. **Older items, still pending:**
   - #4: built-in shaders look low quality (not reproduced)
   - #8: lighting pops on far terrain (probably relates to the coarse/detail lighting difference)
   - a missing item model (`select` without a fallback)
   - the experimental minecart camera

## How to test (scratch dir `%TEMP%\mcpp-work`)

- **Play-mode flight:**
  - Use `saves/FlyBase`, a Hills copy with `allowCommands = 1` set via `nbtlib`. Copy it to `saves/FlTest` for each run.
  - Run with `--play --world FlTest --saves-dir <scratch>/saves --profile`.
  - Script: `/gamemode spectator`, `wheel 1` ×10, `key LCONTROL down`, `key w down`.
  - The `profile: main thread` line splits the frame by phase. `MCPP_STREAM_PROFILE=1` logs the nearest unmeshed
    section and the queue sizes.
- **Crash dumps:** `python dump.py <dmp> <build dir>` symbolizes the faulting address and scans the stack using
  dbghelp and the PDB; there is no cdb on this machine.
- **Viewer benchmark and visual comparison:** `prof.sh <build> <preset> <tag>` and
  `cmpcheck.sh <tag> <cam> <shaders>`.

## Rules to keep

- Only Haiku subagents, and only for exploration; the main agent writes all code.
- One game instance at a time (`mkdir %TEMP%\mcpp-game.lock`). Use a scratch `--saves-dir` and never write to the
  real `.minecraft`.
- After each run:
  - restore `options.txt` from `mcpp-work/backup`
  - delete `command_history.txt`
  - never touch `friends\` or the title panorama cache
- Rebuild `build\Lead` only when the game is not running.
- **The owner trains models and plays other games on this machine.** Check the load before benchmarking, and compare
  builds interleaved, never against older numbers. Build at low priority while they are gaming
  (`cmd /c start "" /low /wait /b powershell -File build.ps1 -Dir Lead`).
