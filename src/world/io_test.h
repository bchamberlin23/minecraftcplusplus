#pragma once

class BlockRegistry;

// Scratch-only regression checks and CPU timings for world saving and dimension unload.
int runWorldIoTest(BlockRegistry& registry, int dataVersion);
