#include "world/io_test.h"

#include "assets/assets.h"
#include "world/save.h"
#include "world/world.h"

#include <algorithm>
#include <stdexcept>

namespace {
void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}

u32 be32(const u8* p) { return (u32(p[0]) << 24) | (u32(p[1]) << 16) | (u32(p[2]) << 8) | p[3]; }

std::vector<u8> payload(const fs::path& path, int index) {
    std::vector<u8> bytes;
    require(readFile(path, bytes) && bytes.size() >= 8192, "read region");
    const size_t pos = size_t(be32(bytes.data() + index * 4) >> 8) * 4096;
    require(pos >= 8192 && pos + 5 <= bytes.size(), "region location");
    const size_t len = size_t(be32(bytes.data() + pos)) + 4;
    require(pos + len <= bytes.size(), "region payload length");
    return {bytes.begin() + pos, bytes.begin() + pos + len};
}

// A deliberately mixed palette and non-uniform light exercise storage compaction and round trips.
std::vector<World::PackedSection> sections(u16 stone, int count) {
    std::vector<World::PackedSection> result(size_t(count), World::PackedSection{});
    for (auto& s : result) {
        s.palette = {stone, 0};
        s.bits = 4;
        s.words.assign(256, 0x0101010101010101ull);
        s.skyData.resize(2048);
        for (size_t i = 0; i < s.skyData.size(); ++i) s.skyData[i] = u8(i);
        s.block = World::kLightUniform | 7;
        s.hasBiomes = true;
        std::fill(std::begin(s.biomes), std::end(s.biomes), World::biomeId("minecraft:plains"));
    }
    return result;
}
}  // namespace

int runWorldIoTest(BlockRegistry& registry, int dataVersion) {
    const fs::path scratch = fs::temp_directory_path() /
        ("mcpp-world-io-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()));
    // Only this freshly created test directory is removed; no user's save is opened for writing.
    struct Cleanup {
        fs::path path;
        ~Cleanup() { std::error_code ec; fs::remove_all(path, ec); }
    } cleanup{scratch};
    try {
        fs::create_directories(scratch / "region");
        const u16 stone = registry.idFor("minecraft:stone"), gold = registry.idFor("minecraft:gold_block");
        const auto packed = sections(stone, 24);
        World world;
        world.initEmpty(-4, 19);
        world.trackBlockChanges(true);
        auto fill = [&] {
            for (int cz = 0; cz < 32; ++cz)
                for (int cx = 0; cx < 32; ++cx) world.replaceChunk(cx, cz, -4, packed);
        };
        fill();
        Timer timer;
        for (int cz = 0; cz < 32; ++cz)
            for (int cx = 0; cx < 32; ++cx) world.removeChunk(cx, cz);
        const double oldMs = timer.ms();
        fill();
        const World::Region* stable = world.regions()[0].get();
        world.setChunkBlockEntities(0, 0, {{0, 0, 0, "test"}});
        world.markChunkDirty(0, 0);
        std::vector<u64> changes;
        world.takeBlockChanges(changes);
        timer = Timer();
        world.clearChunks();
        const double clearMs = timer.ms();
        require(world.regions()[0].get() == stable, "clear retains region addresses");
        require(!world.hasChunk(0, 0) && !world.hasChunk(31, 31), "clear removes every column");
        require(stable->sections.empty() && stable->palettes.empty() && stable->words.empty() &&
                stable->lightArrays.empty(), "clear frees block/light storage");
        require(!world.blockEntities(0, 0) && world.dirtyChunks().empty(), "clear removes entities and dirty state");
        require(world.biomeAt(0, 0) == World::kUnknownBiome, "clear removes old biomes");
        world.takeBlockChanges(changes);
        require(changes.size() == 1024, "clear reports all removed columns to consumers");
        LOG("world io: 1024 columns x 24 sections: individual unload %.2f ms, bulk unload %.2f ms", oldMs, clearMs);

        const auto small = sections(stone, 2);
        for (auto [cx, cz] : {std::pair{0, 0}, std::pair{1, 0}, std::pair{32, 0}, std::pair{-1, -1}}) {
            world.replaceChunk(cx, cz, 0, small);
            world.markChunkDirty(cx, cz);
        }
        WorldSaver saver(world, registry, scratch);
        require(saver.lock(), "scratch session lock");
        saver.enableNewChunks([](int, int, int) { return "minecraft:plains"; }, dataVersion);
        SaveStats st = saver.save(nullptr);
        require(st.ok && st.chunks == 4 && st.regions == 3 && world.dirtyChunks().empty(), "initial save");
        const auto untouched = payload(scratch / "region/r.0.0.mca", 1);
        require(world.setBlock(0, 0, 0, gold) && world.setBlock(512, 0, 0, gold), "edits");
        // Deny access to one atomic-write temporary file to exercise partial save failure.
        const fs::path blocked = scratch / "region/r.1.0.mca.mcpp-tmp";
        HANDLE blocker = CreateFileW(blocked.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                     FILE_ATTRIBUTE_NORMAL, nullptr);
        require(blocker != INVALID_HANDLE_VALUE, "failure fixture");
        st = saver.save(nullptr);
        CloseHandle(blocker);
        require(!st.ok && st.chunks == 1 && st.regions == 1, "partial failure is reported");
        require(world.dirtyChunks().size() == 1 && world.dirtyChunks().count(World::chunkKey(32, 0)),
                "only unsuccessful region remains dirty");
        require(payload(scratch / "region/r.0.0.mca", 1) == untouched, "untouched payload survives repacking byte for byte");
        require(fs::exists(scratch / "minecraftCplusplus-backup/r.0.0.mca"), "original region backed up");
        st = saver.save(nullptr);
        require(st.ok && st.chunks == 1 && st.regions == 1 && world.dirtyChunks().empty(), "failed save retries successfully");
        World loaded;
        LoadStats stats;
        require(loaded.load(scratch, registry, 0, 0, 0, stats) && stats.chunks == 4, "reload saved chunks");
        require(loaded.block(0, 0, 0) == gold && loaded.block(512, 0, 0) == gold && loaded.block(-15, 0, -16) == stone,
                "saved block edits and negative coordinates round trip");
        require(loaded.light(1, 0, 0).block4(0, 0, 0) == 7 && loaded.light(1, 0, 0).sky4(3, 0, 0) == 0,
                "light round trip");
        LOG("world io: PASS (bulk clear, stable pointers, notifications, palettes/light, unchanged payload, backup, failure/retry)");
        return 0;
    } catch (const std::exception& e) {
        LOG("world io: FAIL: %s", e.what());
        return 1;
    }
}
