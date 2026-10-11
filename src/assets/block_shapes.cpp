#include "assets/block_shapes.h"

#include "assets/game_reports.h"

#include <cstdlib>
#include <sstream>

namespace {

constexpr int kFormat = 1;

// Run with the game's code on the class path; writes "shapes" then "states" sections as plain text:
//   S <count>, then one line per shape: <boxes> x0 y0 z0 x1 y1 z1 ...
//   T <count>, then one line per state: <state key> <outline shape> <collision shape>
constexpr const char* kDumpSource = R"JAVA(
import java.io.*;
import java.nio.charset.StandardCharsets;
import java.util.*;
import net.minecraft.SharedConstants;
import net.minecraft.core.BlockPos;
import net.minecraft.core.registries.BuiltInRegistries;
import net.minecraft.server.Bootstrap;
import net.minecraft.world.level.EmptyBlockGetter;
import net.minecraft.world.level.block.Block;
import net.minecraft.world.level.block.state.BlockState;
import net.minecraft.world.level.block.state.properties.Property;
import net.minecraft.world.phys.AABB;
import net.minecraft.world.phys.Vec3;
import net.minecraft.world.phys.shapes.VoxelShape;

public class BlockShapeDump {
    static <T extends Comparable<T>> String value(BlockState s, Property<T> p) { return p.getName(s.getValue(p)); }

    static String key(String name, BlockState s) {
        List<String> props = new ArrayList<>();
        for (Property<?> p : s.getProperties()) props.add(p.getName() + "=" + value(s, p));
        if (props.isEmpty()) return name;
        Collections.sort(props);
        return name + "[" + String.join(",", props) + "]";
    }

    static final Map<String, Integer> index = new HashMap<>();
    static final List<String> shapes = new ArrayList<>();

    static int add(VoxelShape shape, Vec3 offset) {
        StringBuilder sb = new StringBuilder();
        List<AABB> boxes = shape.toAabbs();
        sb.append(boxes.size());
        for (AABB b : boxes)
            for (double v : new double[] {b.minX - offset.x, b.minY - offset.y, b.minZ - offset.z, b.maxX - offset.x, b.maxY - offset.y, b.maxZ - offset.z})
                sb.append(' ').append((float) v);
        return index.computeIfAbsent(sb.toString(), k -> { shapes.add(k); return shapes.size() - 1; });
    }

    public static void main(String[] args) throws Exception {
        SharedConstants.tryDetectVersion();
        Bootstrap.bootStrap();
        StringBuilder states = new StringBuilder();
        int count = 0;
        for (Block block : BuiltInRegistries.BLOCK) {
            String name = BuiltInRegistries.BLOCK.getKey(block).toString();
            for (BlockState s : block.getStateDefinition().getPossibleStates()) {
                Vec3 offset = s.getOffset(BlockPos.ZERO);
                int outline = add(s.getShape(EmptyBlockGetter.INSTANCE, BlockPos.ZERO), offset);
                int collision = add(s.getCollisionShape(EmptyBlockGetter.INSTANCE, BlockPos.ZERO), offset);
                states.append(key(name, s)).append(' ').append(outline).append(' ').append(collision).append('\n');
                ++count;
            }
        }
        try (Writer w = new OutputStreamWriter(new FileOutputStream(args[0]), StandardCharsets.UTF_8)) {
            w.write(args[1] + "\nS " + shapes.size() + "\n");
            for (String s : shapes) w.write(s + "\n");
            w.write("T " + count + "\n");
            w.write(states.toString());
        }
        System.exit(0);
    }
}
)JAVA";

}  // namespace

bool BlockShapes::load(const fs::path& minecraftDir, const std::string& version, const fs::path& cacheDir) {
    const fs::path dir = cacheDir / version, file = dir / "block_shapes.txt";
    std::vector<u8> bytes;
    auto valid = [&] {
        return readFile(file, bytes) && bytes.size() > 4 && std::atoi(reinterpret_cast<const char*>(bytes.data())) == kFormat;
    };
    if (!valid()) {
        LOG("extracting block shapes with the game's code (one time, ~5 s)...");
        const fs::path tmp = dir / "block_shapes.tmp";
        if (!runGameProgram(minecraftDir, version, "BlockShapeDump", kDumpSource,
                            {tmp.wstring(), std::to_wstring(kFormat)}, dir)) {
            LOG("warning: block shapes unavailable; selection and collision boxes come from the block models");
            return false;
        }
        std::error_code ec;
        fs::rename(tmp, file, ec);
        if (!valid()) return false;
    }
    std::istringstream in(std::string(bytes.begin(), bytes.end()));
    int format = 0;
    std::string tag;
    size_t count = 0;
    in >> format >> tag >> count;
    shapes_.resize(count);
    for (auto& shape : shapes_) {
        size_t boxes = 0;
        in >> boxes;
        shape.resize(boxes);
        for (Box& b : shape) in >> b.min[0] >> b.min[1] >> b.min[2] >> b.max[0] >> b.max[1] >> b.max[2];
    }
    in >> tag >> count;
    states_.reserve(count);
    std::string key;
    Entry e{};
    while (in >> key >> e.outline >> e.collision)
        if (e.outline < shapes_.size() && e.collision < shapes_.size()) states_.emplace(key, e);
    return !states_.empty();
}

const BlockShapes::Entry* BlockShapes::find(std::string_view fullStateKey) const {
    auto it = states_.find(std::string(fullStateKey));
    return it == states_.end() ? nullptr : &it->second;
}
