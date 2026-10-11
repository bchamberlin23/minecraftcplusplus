#pragma once
// Private to climate.cpp / overworld_biomes.cpp: Climate.java's quantized parameter space, the
// multi-noise RTree and the overworld parameter table (OverworldBiomeBuilder).
#include "core/common.h"

#include <vector>

namespace wg::climate {

constexpr int kDims = 7;  // temperature, humidity, continentalness, erosion, depth, weirdness, offset

// Climate.quantizeCoord: (long) (coord * 10000.0F), with Java's saturating float -> long cast.
inline i64 quantize(float coord) {
    const float v = coord * 10000.0f;
    if (v != v) return 0;
    if (v >= 9.2233720368547758e18f) return INT64_MAX;
    if (v <= -9.2233720368547758e18f) return INT64_MIN;
    return i64(v);
}

struct Param {
    i64 min, max;
};

struct Entry {
    Param space[kDims];  // ParameterPoint.parameterSpace(): offset is the point [offset, offset]
    const char* biome;   // "minecraft:..." (string literal)
};

// OverworldBiomeBuilder.addBiomes for the default overworld preset, in the game's order.
std::vector<Entry> overworldEntries();

// Climate.RTree with 19 children per node, flattened. Leaves keep the index of their entry's biome
// name in names().
class Tree {
public:
    explicit Tree(const std::vector<Entry>& entries);

    // RTree.search: `hint` is the previous result on this thread (RTree.lastResult), -1 for none;
    // it is updated to the new result leaf. Returns the biome index.
    int search(const i64 target[kDims], int& hint) const;
    const std::vector<const char*>& names() const { return names_; }

private:
    struct Node {
        i64 lo[kDims], hi[kDims];
        int first = 0, count = 0;  // children in children_; count 0 = leaf
        int biome = -1;
    };
    int searchNode(int node, const i64* target, int candidate) const;
    i64 distance(int node, const i64* target) const;

    std::vector<Node> nodes_;
    std::vector<int> children_;
    std::vector<const char*> names_;
    int root_ = 0;
    friend struct TreeBuilder;
};

const Tree& overworldTree();  // built once, shared (like the game's per-preset ParameterList)

}  // namespace wg::climate
