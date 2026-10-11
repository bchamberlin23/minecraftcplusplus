// OverworldBiomeBuilder (default overworld preset) and Climate.RTree.
#include "worldgen/climate_tree.h"

#include <algorithm>
#include <cmath>
#include <string_view>

namespace wg::climate {

namespace {

Param span(float min, float max) { return {quantize(min), quantize(max)}; }
Param span(Param min, Param max) { return {min.min, max.max}; }
Param point(float v) { return span(v, v); }

// Biome keys, as in Biomes.java.
#define MC(x) "minecraft:" #x
const char* const THE_NULL = nullptr;

class OverworldBuilder {
public:
    std::vector<Entry> out;

    void addBiomes() {
        addOffCoastBiomes();
        addInlandBiomes();
        addUndergroundBiomes();
    }

private:
    const Param FULL_RANGE = span(-1.0f, 1.0f);
    const Param temperatures[5] = {span(-1.0f, -0.45f), span(-0.45f, -0.15f), span(-0.15f, 0.2f), span(0.2f, 0.55f),
                                   span(0.55f, 1.0f)};
    const Param humidities[5] = {span(-1.0f, -0.35f), span(-0.35f, -0.1f), span(-0.1f, 0.1f), span(0.1f, 0.3f),
                                 span(0.3f, 1.0f)};
    const Param erosions[7] = {span(-1.0f, -0.78f),    span(-0.78f, -0.375f), span(-0.375f, -0.2225f),
                               span(-0.2225f, 0.05f),  span(0.05f, 0.45f),    span(0.45f, 0.55f),
                               span(0.55f, 1.0f)};
    const Param FROZEN_RANGE = temperatures[0];
    const Param UNFROZEN_RANGE = span(temperatures[1], temperatures[4]);
    const Param mushroomFieldsContinentalness = span(-1.2f, -1.05f);
    const Param deepOceanContinentalness = span(-1.05f, -0.455f);
    const Param oceanContinentalness = span(-0.455f, -0.19f);
    const Param coastContinentalness = span(-0.19f, -0.11f);
    const Param inlandContinentalness = span(-0.11f, 0.55f);
    const Param nearInlandContinentalness = span(-0.11f, 0.03f);
    const Param midInlandContinentalness = span(0.03f, 0.3f);
    const Param farInlandContinentalness = span(0.3f, 1.0f);

    const char* const OCEANS[2][5] = {
        {MC(deep_frozen_ocean), MC(deep_cold_ocean), MC(deep_ocean), MC(deep_lukewarm_ocean), MC(warm_ocean)},
        {MC(frozen_ocean), MC(cold_ocean), MC(ocean), MC(lukewarm_ocean), MC(warm_ocean)}};
    const char* const MIDDLE_BIOMES[5][5] = {
        {MC(snowy_plains), MC(snowy_plains), MC(snowy_plains), MC(snowy_taiga), MC(taiga)},
        {MC(plains), MC(plains), MC(forest), MC(taiga), MC(old_growth_spruce_taiga)},
        {MC(flower_forest), MC(plains), MC(forest), MC(birch_forest), MC(dark_forest)},
        {MC(savanna), MC(savanna), MC(forest), MC(jungle), MC(jungle)},
        {MC(desert), MC(desert), MC(desert), MC(desert), MC(desert)}};
    const char* const MIDDLE_BIOMES_VARIANT[5][5] = {
        {MC(ice_spikes), THE_NULL, MC(snowy_taiga), THE_NULL, THE_NULL},
        {MC(dappled_forest), THE_NULL, THE_NULL, THE_NULL, MC(old_growth_pine_taiga)},
        {MC(sunflower_plains), THE_NULL, THE_NULL, MC(old_growth_birch_forest), THE_NULL},
        {THE_NULL, THE_NULL, MC(plains), MC(sparse_jungle), MC(bamboo_jungle)},
        {THE_NULL, THE_NULL, THE_NULL, THE_NULL, THE_NULL}};
    const char* const PLATEAU_BIOMES[5][5] = {
        {MC(snowy_plains), MC(snowy_plains), MC(snowy_plains), MC(snowy_taiga), MC(snowy_taiga)},
        {MC(meadow), MC(meadow), MC(forest), MC(taiga), MC(old_growth_spruce_taiga)},
        {MC(meadow), MC(meadow), MC(meadow), MC(meadow), MC(pale_garden)},
        {MC(savanna_plateau), MC(savanna_plateau), MC(forest), MC(forest), MC(jungle)},
        {MC(badlands), MC(badlands), MC(badlands), MC(wooded_badlands), MC(wooded_badlands)}};
    const char* const PLATEAU_BIOMES_VARIANT[5][5] = {
        {MC(ice_spikes), THE_NULL, THE_NULL, THE_NULL, THE_NULL},
        {MC(cherry_grove), THE_NULL, MC(meadow), MC(meadow), MC(old_growth_pine_taiga)},
        {MC(cherry_grove), MC(cherry_grove), MC(forest), MC(birch_forest), THE_NULL},
        {THE_NULL, THE_NULL, THE_NULL, THE_NULL, THE_NULL},
        {MC(eroded_badlands), MC(eroded_badlands), THE_NULL, THE_NULL, THE_NULL}};
    const char* const SHATTERED_BIOMES[5][5] = {
        {MC(windswept_gravelly_hills), MC(windswept_gravelly_hills), MC(windswept_hills), MC(windswept_forest),
         MC(windswept_forest)},
        {MC(windswept_gravelly_hills), MC(windswept_gravelly_hills), MC(windswept_hills), MC(windswept_forest),
         MC(windswept_forest)},
        {MC(windswept_hills), MC(windswept_hills), MC(windswept_hills), MC(windswept_forest), MC(windswept_forest)},
        {THE_NULL, THE_NULL, THE_NULL, THE_NULL, THE_NULL},
        {THE_NULL, THE_NULL, THE_NULL, THE_NULL, THE_NULL}};

    void add(Param t, Param h, Param c, Param e, Param d, Param w, float offset, const char* biome) {
        const i64 o = quantize(offset);
        out.push_back({{t, h, c, e, d, w, {o, o}}, biome});
    }
    void addSurfaceBiome(Param t, Param h, Param c, Param e, Param w, float offset, const char* biome) {
        add(t, h, c, e, point(0.0f), w, offset, biome);
        add(t, h, c, e, point(1.0f), w, offset, biome);
    }
    void addUndergroundBiome(Param t, Param h, Param c, Param e, Param w, float offset, const char* biome) {
        add(t, h, c, e, span(0.2f, 0.9f), w, offset, biome);
    }
    void addBottomBiome(Param t, Param h, Param c, Param e, Param w, float offset, const char* biome) {
        add(t, h, c, e, point(1.1f), w, offset, biome);
    }

    void addOffCoastBiomes() {
        addSurfaceBiome(FULL_RANGE, FULL_RANGE, mushroomFieldsContinentalness, FULL_RANGE, FULL_RANGE, 0.0f,
                        MC(mushroom_fields));
        for (int ti = 0; ti < 5; ++ti) {
            const Param t = temperatures[ti];
            addSurfaceBiome(t, FULL_RANGE, deepOceanContinentalness, FULL_RANGE, FULL_RANGE, 0.0f, OCEANS[0][ti]);
            addSurfaceBiome(t, FULL_RANGE, oceanContinentalness, FULL_RANGE, FULL_RANGE, 0.0f, OCEANS[1][ti]);
        }
    }

    void addInlandBiomes() {
        addMidSlice(span(-1.0f, -0.93333334f));
        addHighSlice(span(-0.93333334f, -0.7666667f));
        addPeaks(span(-0.7666667f, -0.56666666f));
        addHighSlice(span(-0.56666666f, -0.4f));
        addMidSlice(span(-0.4f, -0.26666668f));
        addLowSlice(span(-0.26666668f, -0.05f));
        addValleys(span(-0.05f, 0.05f));
        addLowSlice(span(0.05f, 0.26666668f));
        addMidSlice(span(0.26666668f, 0.4f));
        addHighSlice(span(0.4f, 0.56666666f));
        addPeaks(span(0.56666666f, 0.7666667f));
        addHighSlice(span(0.7666667f, 0.93333334f));
        addMidSlice(span(0.93333334f, 1.0f));
    }

    void addPeaks(Param w) {
        for (int ti = 0; ti < 5; ++ti) {
            const Param t = temperatures[ti];
            for (int hi = 0; hi < 5; ++hi) {
                const Param h = humidities[hi];
                const char* middle = pickMiddleBiome(ti, hi, w);
                const char* middleOrBadlands = pickMiddleBiomeOrBadlandsIfHot(ti, hi, w);
                const char* middleOrBadlandsOrSlope = pickMiddleBiomeOrBadlandsIfHotOrSlopeIfCold(ti, hi, w);
                const char* plateau = pickPlateauBiome(ti, hi, w);
                const char* shattered = pickShatteredBiome(ti, hi, w);
                const char* shatteredOrSavanna = maybePickWindsweptSavannaBiome(ti, hi, w, shattered);
                const char* peak = pickPeakBiome(ti, hi, w);
                addSurfaceBiome(t, h, span(coastContinentalness, farInlandContinentalness), erosions[0], w, 0.0f, peak);
                addSurfaceBiome(t, h, span(coastContinentalness, nearInlandContinentalness), erosions[1], w, 0.0f,
                                middleOrBadlandsOrSlope);
                addSurfaceBiome(t, h, span(midInlandContinentalness, farInlandContinentalness), erosions[1], w, 0.0f,
                                peak);
                addSurfaceBiome(t, h, span(coastContinentalness, nearInlandContinentalness),
                                span(erosions[2], erosions[3]), w, 0.0f, middle);
                addSurfaceBiome(t, h, span(midInlandContinentalness, farInlandContinentalness), erosions[2], w, 0.0f,
                                plateau);
                addSurfaceBiome(t, h, midInlandContinentalness, erosions[3], w, 0.0f, middleOrBadlands);
                addSurfaceBiome(t, h, farInlandContinentalness, erosions[3], w, 0.0f, plateau);
                addSurfaceBiome(t, h, span(coastContinentalness, farInlandContinentalness), erosions[4], w, 0.0f,
                                middle);
                addSurfaceBiome(t, h, span(coastContinentalness, nearInlandContinentalness), erosions[5], w, 0.0f,
                                shatteredOrSavanna);
                addSurfaceBiome(t, h, span(midInlandContinentalness, farInlandContinentalness), erosions[5], w, 0.0f,
                                shattered);
                addSurfaceBiome(t, h, span(coastContinentalness, farInlandContinentalness), erosions[6], w, 0.0f,
                                middle);
            }
        }
    }

    void addHighSlice(Param w) {
        for (int ti = 0; ti < 5; ++ti) {
            const Param t = temperatures[ti];
            for (int hi = 0; hi < 5; ++hi) {
                const Param h = humidities[hi];
                const char* middle = pickMiddleBiome(ti, hi, w);
                const char* middleOrBadlands = pickMiddleBiomeOrBadlandsIfHot(ti, hi, w);
                const char* middleOrBadlandsOrSlope = pickMiddleBiomeOrBadlandsIfHotOrSlopeIfCold(ti, hi, w);
                const char* plateau = pickPlateauBiome(ti, hi, w);
                const char* shattered = pickShatteredBiome(ti, hi, w);
                const char* middleOrSavanna = maybePickWindsweptSavannaBiome(ti, hi, w, middle);
                const char* slope = pickSlopeBiome(ti, hi, w);
                const char* peak = pickPeakBiome(ti, hi, w);
                addSurfaceBiome(t, h, coastContinentalness, span(erosions[0], erosions[1]), w, 0.0f, middle);
                addSurfaceBiome(t, h, nearInlandContinentalness, erosions[0], w, 0.0f, slope);
                addSurfaceBiome(t, h, span(midInlandContinentalness, farInlandContinentalness), erosions[0], w, 0.0f,
                                peak);
                addSurfaceBiome(t, h, nearInlandContinentalness, erosions[1], w, 0.0f, middleOrBadlandsOrSlope);
                addSurfaceBiome(t, h, span(midInlandContinentalness, farInlandContinentalness), erosions[1], w, 0.0f,
                                slope);
                addSurfaceBiome(t, h, span(coastContinentalness, nearInlandContinentalness),
                                span(erosions[2], erosions[3]), w, 0.0f, middle);
                addSurfaceBiome(t, h, span(midInlandContinentalness, farInlandContinentalness), erosions[2], w, 0.0f,
                                plateau);
                addSurfaceBiome(t, h, midInlandContinentalness, erosions[3], w, 0.0f, middleOrBadlands);
                addSurfaceBiome(t, h, farInlandContinentalness, erosions[3], w, 0.0f, plateau);
                addSurfaceBiome(t, h, span(coastContinentalness, farInlandContinentalness), erosions[4], w, 0.0f,
                                middle);
                addSurfaceBiome(t, h, span(coastContinentalness, nearInlandContinentalness), erosions[5], w, 0.0f,
                                middleOrSavanna);
                addSurfaceBiome(t, h, span(midInlandContinentalness, farInlandContinentalness), erosions[5], w, 0.0f,
                                shattered);
                addSurfaceBiome(t, h, span(coastContinentalness, farInlandContinentalness), erosions[6], w, 0.0f,
                                middle);
            }
        }
    }

    void addMidSlice(Param w) {
        addSurfaceBiome(FULL_RANGE, FULL_RANGE, coastContinentalness, span(erosions[0], erosions[2]), w, 0.0f,
                        MC(stony_shore));
        addSurfaceBiome(span(temperatures[1], temperatures[2]), FULL_RANGE,
                        span(nearInlandContinentalness, farInlandContinentalness), erosions[6], w, 0.0f, MC(swamp));
        addSurfaceBiome(span(temperatures[3], temperatures[4]), FULL_RANGE,
                        span(nearInlandContinentalness, farInlandContinentalness), erosions[6], w, 0.0f,
                        MC(mangrove_swamp));
        for (int ti = 0; ti < 5; ++ti) {
            const Param t = temperatures[ti];
            for (int hi = 0; hi < 5; ++hi) {
                const Param h = humidities[hi];
                const char* middle = pickMiddleBiome(ti, hi, w);
                const char* middleOrBadlands = pickMiddleBiomeOrBadlandsIfHot(ti, hi, w);
                const char* middleOrBadlandsOrSlope = pickMiddleBiomeOrBadlandsIfHotOrSlopeIfCold(ti, hi, w);
                const char* shattered = pickShatteredBiome(ti, hi, w);
                const char* plateau = pickPlateauBiome(ti, hi, w);
                const char* beach = pickBeachBiome(ti, hi);
                const char* middleOrSavanna = maybePickWindsweptSavannaBiome(ti, hi, w, middle);
                const char* shatteredCoast = pickShatteredCoastBiome(ti, hi, w);
                const char* slope = pickSlopeBiome(ti, hi, w);
                addSurfaceBiome(t, h, span(nearInlandContinentalness, farInlandContinentalness), erosions[0], w, 0.0f,
                                slope);
                addSurfaceBiome(t, h, span(nearInlandContinentalness, midInlandContinentalness), erosions[1], w, 0.0f,
                                middleOrBadlandsOrSlope);
                addSurfaceBiome(t, h, farInlandContinentalness, erosions[1], w, 0.0f, ti == 0 ? slope : plateau);
                addSurfaceBiome(t, h, nearInlandContinentalness, erosions[2], w, 0.0f, middle);
                addSurfaceBiome(t, h, midInlandContinentalness, erosions[2], w, 0.0f, middleOrBadlands);
                addSurfaceBiome(t, h, farInlandContinentalness, erosions[2], w, 0.0f, plateau);
                addSurfaceBiome(t, h, span(coastContinentalness, nearInlandContinentalness), erosions[3], w, 0.0f,
                                middle);
                addSurfaceBiome(t, h, span(midInlandContinentalness, farInlandContinentalness), erosions[3], w, 0.0f,
                                middleOrBadlands);
                if (w.max < 0) {
                    addSurfaceBiome(t, h, coastContinentalness, erosions[4], w, 0.0f, beach);
                    addSurfaceBiome(t, h, span(nearInlandContinentalness, farInlandContinentalness), erosions[4], w,
                                    0.0f, middle);
                } else {
                    addSurfaceBiome(t, h, span(coastContinentalness, farInlandContinentalness), erosions[4], w, 0.0f,
                                    middle);
                }
                addSurfaceBiome(t, h, coastContinentalness, erosions[5], w, 0.0f, shatteredCoast);
                addSurfaceBiome(t, h, nearInlandContinentalness, erosions[5], w, 0.0f, middleOrSavanna);
                addSurfaceBiome(t, h, span(midInlandContinentalness, farInlandContinentalness), erosions[5], w, 0.0f,
                                shattered);
                addSurfaceBiome(t, h, coastContinentalness, erosions[6], w, 0.0f, w.max < 0 ? beach : middle);
                if (ti == 0)
                    addSurfaceBiome(t, h, span(nearInlandContinentalness, farInlandContinentalness), erosions[6], w,
                                    0.0f, middle);
            }
        }
    }

    void addLowSlice(Param w) {
        addSurfaceBiome(FULL_RANGE, FULL_RANGE, coastContinentalness, span(erosions[0], erosions[2]), w, 0.0f,
                        MC(stony_shore));
        addSurfaceBiome(span(temperatures[1], temperatures[2]), FULL_RANGE,
                        span(nearInlandContinentalness, farInlandContinentalness), erosions[6], w, 0.0f, MC(swamp));
        addSurfaceBiome(span(temperatures[3], temperatures[4]), FULL_RANGE,
                        span(nearInlandContinentalness, farInlandContinentalness), erosions[6], w, 0.0f,
                        MC(mangrove_swamp));
        for (int ti = 0; ti < 5; ++ti) {
            const Param t = temperatures[ti];
            for (int hi = 0; hi < 5; ++hi) {
                const Param h = humidities[hi];
                const char* middle = pickMiddleBiome(ti, hi, w);
                const char* middleOrBadlands = pickMiddleBiomeOrBadlandsIfHot(ti, hi, w);
                const char* middleOrBadlandsOrSlope = pickMiddleBiomeOrBadlandsIfHotOrSlopeIfCold(ti, hi, w);
                const char* beach = pickBeachBiome(ti, hi);
                const char* middleOrSavanna = maybePickWindsweptSavannaBiome(ti, hi, w, middle);
                const char* shatteredCoast = pickShatteredCoastBiome(ti, hi, w);
                addSurfaceBiome(t, h, nearInlandContinentalness, span(erosions[0], erosions[1]), w, 0.0f,
                                middleOrBadlands);
                addSurfaceBiome(t, h, span(midInlandContinentalness, farInlandContinentalness),
                                span(erosions[0], erosions[1]), w, 0.0f, middleOrBadlandsOrSlope);
                addSurfaceBiome(t, h, nearInlandContinentalness, span(erosions[2], erosions[3]), w, 0.0f, middle);
                addSurfaceBiome(t, h, span(midInlandContinentalness, farInlandContinentalness),
                                span(erosions[2], erosions[3]), w, 0.0f, middleOrBadlands);
                addSurfaceBiome(t, h, coastContinentalness, span(erosions[3], erosions[4]), w, 0.0f, beach);
                addSurfaceBiome(t, h, span(nearInlandContinentalness, farInlandContinentalness), erosions[4], w, 0.0f,
                                middle);
                addSurfaceBiome(t, h, coastContinentalness, erosions[5], w, 0.0f, shatteredCoast);
                addSurfaceBiome(t, h, nearInlandContinentalness, erosions[5], w, 0.0f, middleOrSavanna);
                addSurfaceBiome(t, h, span(midInlandContinentalness, farInlandContinentalness), erosions[5], w, 0.0f,
                                middle);
                addSurfaceBiome(t, h, coastContinentalness, erosions[6], w, 0.0f, beach);
                if (ti == 0)
                    addSurfaceBiome(t, h, span(nearInlandContinentalness, farInlandContinentalness), erosions[6], w,
                                    0.0f, middle);
            }
        }
    }

    void addValleys(Param w) {
        const Param e01 = span(erosions[0], erosions[1]);
        addSurfaceBiome(FROZEN_RANGE, FULL_RANGE, coastContinentalness, e01, w, 0.0f,
                        w.max < 0 ? MC(stony_shore) : MC(frozen_river));
        addSurfaceBiome(UNFROZEN_RANGE, FULL_RANGE, coastContinentalness, e01, w, 0.0f,
                        w.max < 0 ? MC(stony_shore) : MC(river));
        addSurfaceBiome(FROZEN_RANGE, FULL_RANGE, nearInlandContinentalness, e01, w, 0.0f, MC(frozen_river));
        addSurfaceBiome(UNFROZEN_RANGE, FULL_RANGE, nearInlandContinentalness, e01, w, 0.0f, MC(river));
        addSurfaceBiome(FROZEN_RANGE, FULL_RANGE, span(coastContinentalness, farInlandContinentalness),
                        span(erosions[2], erosions[5]), w, 0.0f, MC(frozen_river));
        addSurfaceBiome(UNFROZEN_RANGE, FULL_RANGE, span(coastContinentalness, farInlandContinentalness),
                        span(erosions[2], erosions[5]), w, 0.0f, MC(river));
        addSurfaceBiome(FROZEN_RANGE, FULL_RANGE, coastContinentalness, erosions[6], w, 0.0f, MC(frozen_river));
        addSurfaceBiome(UNFROZEN_RANGE, FULL_RANGE, coastContinentalness, erosions[6], w, 0.0f, MC(river));
        addSurfaceBiome(span(temperatures[1], temperatures[2]), FULL_RANGE,
                        span(inlandContinentalness, farInlandContinentalness), erosions[6], w, 0.0f, MC(swamp));
        addSurfaceBiome(span(temperatures[3], temperatures[4]), FULL_RANGE,
                        span(inlandContinentalness, farInlandContinentalness), erosions[6], w, 0.0f,
                        MC(mangrove_swamp));
        addSurfaceBiome(FROZEN_RANGE, FULL_RANGE, span(inlandContinentalness, farInlandContinentalness), erosions[6],
                        w, 0.0f, MC(frozen_river));
        for (int ti = 0; ti < 5; ++ti) {
            const Param t = temperatures[ti];
            for (int hi = 0; hi < 5; ++hi) {
                addSurfaceBiome(t, humidities[hi], span(midInlandContinentalness, farInlandContinentalness), e01, w,
                                0.0f, pickMiddleBiomeOrBadlandsIfHot(ti, hi, w));
            }
        }
    }

    void addUndergroundBiomes() {
        addUndergroundBiome(FULL_RANGE, FULL_RANGE, span(0.8f, 1.0f), FULL_RANGE, FULL_RANGE, 0.0f,
                            MC(dripstone_caves));
        addUndergroundBiome(FULL_RANGE, span(0.7f, 1.0f), FULL_RANGE, FULL_RANGE, FULL_RANGE, 0.0f, MC(lush_caves));
        addUndergroundBiome(FULL_RANGE, FULL_RANGE, span(coastContinentalness, inlandContinentalness),
                            span(erosions[5], erosions[6]), span(-1.1f, -0.85f), 0.0f, MC(sulfur_caves));
        addBottomBiome(FULL_RANGE, FULL_RANGE, FULL_RANGE, span(erosions[0], erosions[1]), FULL_RANGE, 0.0f,
                       MC(deep_dark));
    }

    const char* pickMiddleBiome(int ti, int hi, Param w) const {
        if (w.max < 0) return MIDDLE_BIOMES[ti][hi];
        const char* variant = MIDDLE_BIOMES_VARIANT[ti][hi];
        return variant ? variant : MIDDLE_BIOMES[ti][hi];
    }
    const char* pickMiddleBiomeOrBadlandsIfHot(int ti, int hi, Param w) const {
        return ti == 4 ? pickBadlandsBiome(hi, w) : pickMiddleBiome(ti, hi, w);
    }
    const char* pickMiddleBiomeOrBadlandsIfHotOrSlopeIfCold(int ti, int hi, Param w) const {
        return ti == 0 ? pickSlopeBiome(ti, hi, w) : pickMiddleBiomeOrBadlandsIfHot(ti, hi, w);
    }
    const char* maybePickWindsweptSavannaBiome(int ti, int hi, Param w, const char* underlying) const {
        return ti > 1 && hi < 4 && w.max >= 0 ? MC(windswept_savanna) : underlying;
    }
    const char* pickShatteredCoastBiome(int ti, int hi, Param w) const {
        const char* beachOrMiddle = w.max >= 0 ? pickMiddleBiome(ti, hi, w) : pickBeachBiome(ti, hi);
        return maybePickWindsweptSavannaBiome(ti, hi, w, beachOrMiddle);
    }
    const char* pickBeachBiome(int ti, int) const {
        if (ti == 0) return MC(snowy_beach);
        return ti == 4 ? MC(desert) : MC(beach);
    }
    const char* pickBadlandsBiome(int hi, Param w) const {
        if (hi < 2) return w.max < 0 ? MC(badlands) : MC(eroded_badlands);
        return hi < 3 ? MC(badlands) : MC(wooded_badlands);
    }
    const char* pickPlateauBiome(int ti, int hi, Param w) const {
        if (w.max >= 0 && PLATEAU_BIOMES_VARIANT[ti][hi]) return PLATEAU_BIOMES_VARIANT[ti][hi];
        return PLATEAU_BIOMES[ti][hi];
    }
    const char* pickPeakBiome(int ti, int hi, Param w) const {
        if (ti <= 2) return w.max < 0 ? MC(jagged_peaks) : MC(frozen_peaks);
        return ti == 3 ? MC(stony_peaks) : pickBadlandsBiome(hi, w);
    }
    const char* pickSlopeBiome(int ti, int hi, Param w) const {
        if (ti >= 3) return pickPlateauBiome(ti, hi, w);
        return hi <= 1 ? MC(snowy_slopes) : MC(grove);
    }
    const char* pickShatteredBiome(int ti, int hi, Param w) const {
        const char* biome = SHATTERED_BIOMES[ti][hi];
        return biome ? biome : pickMiddleBiome(ti, hi, w);
    }
};
#undef MC

}  // namespace

std::vector<Entry> overworldEntries() {
    OverworldBuilder b;
    b.addBiomes();
    return std::move(b.out);
}

// ---- RTree construction (Climate.RTree.build) ------------------------------------------------
// The game sorts with List.sort (stable), so std::stable_sort reproduces its orders exactly.

struct TreeBuilder {
    static constexpr int kChildrenPerNode = 19;
    Tree& t;

    struct Bounds {
        i64 lo[kDims], hi[kDims];
    };
    struct Bucket {
        std::vector<int> children;
        Bounds b;
    };

    Bounds boundsOf(const std::vector<int>& children) const {
        Bounds r;
        for (int d = 0; d < kDims; ++d) r.lo[d] = INT64_MAX, r.hi[d] = INT64_MIN;
        for (int c : children) {
            const Tree::Node& n = t.nodes_[c];
            for (int d = 0; d < kDims; ++d) r.lo[d] = std::min(r.lo[d], n.lo[d]), r.hi[d] = std::max(r.hi[d], n.hi[d]);
        }
        return r;
    }

    // RTree.sort's comparator: centre of `dimension`, then the following dimensions cyclically.
    static bool less(const i64* alo, const i64* ahi, const i64* blo, const i64* bhi, int dimension, bool absolute) {
        for (int i = 0; i < kDims; ++i) {
            const int d = (dimension + i) % kDims;
            i64 ca = (alo[d] + ahi[d]) / 2, cb = (blo[d] + bhi[d]) / 2;
            if (absolute) ca = ca < 0 ? -ca : ca, cb = cb < 0 ? -cb : cb;
            if (ca != cb) return ca < cb;
        }
        return false;
    }

    int makeSubTree(const std::vector<int>& children) {
        Tree::Node n;
        const Bounds b = boundsOf(children);
        std::copy(b.lo, b.lo + kDims, n.lo);
        std::copy(b.hi, b.hi + kDims, n.hi);
        n.first = int(t.children_.size());
        n.count = int(children.size());
        t.children_.insert(t.children_.end(), children.begin(), children.end());
        t.nodes_.push_back(n);
        return int(t.nodes_.size()) - 1;
    }

    std::vector<Bucket> bucketize(const std::vector<int>& nodes) const {
        std::vector<Bucket> buckets;
        std::vector<int> children;
        const int expected = int(std::pow(double(kChildrenPerNode),
                                          std::floor(std::log(double(nodes.size()) - 0.01) /
                                                     std::log(double(kChildrenPerNode)))));
        for (int c : nodes) {
            children.push_back(c);
            if (int(children.size()) >= expected) {
                buckets.push_back({children, boundsOf(children)});
                children.clear();
            }
        }
        if (!children.empty()) buckets.push_back({children, boundsOf(children)});
        return buckets;
    }

    int build(std::vector<int> children) {
        if (children.size() == 1) return children[0];
        if (children.size() <= size_t(kChildrenPerNode)) {
            auto magnitude = [&](int c) {
                const Tree::Node& n = t.nodes_[c];
                i64 total = 0;
                for (int d = 0; d < kDims; ++d) {
                    const i64 centre = (n.lo[d] + n.hi[d]) / 2;
                    total += centre < 0 ? -centre : centre;
                }
                return total;
            };
            std::stable_sort(children.begin(), children.end(),
                             [&](int a, int b) { return magnitude(a) < magnitude(b); });
            return makeSubTree(children);
        }
        i64 minCost = INT64_MAX;
        int minDimension = -1;
        std::vector<Bucket> minBuckets;
        for (int d = 0; d < kDims; ++d) {
            std::stable_sort(children.begin(), children.end(), [&](int a, int b) {
                const Tree::Node &na = t.nodes_[a], &nb = t.nodes_[b];
                return less(na.lo, na.hi, nb.lo, nb.hi, d, false);
            });
            std::vector<Bucket> buckets = bucketize(children);
            i64 totalCost = 0;
            for (const Bucket& bucket : buckets)
                for (int k = 0; k < kDims; ++k) totalCost += std::abs(bucket.b.hi[k] - bucket.b.lo[k]);
            if (minCost > totalCost) {
                minCost = totalCost;
                minDimension = d;
                minBuckets = std::move(buckets);
            }
        }
        std::stable_sort(minBuckets.begin(), minBuckets.end(), [&](const Bucket& a, const Bucket& b) {
            return less(a.b.lo, a.b.hi, b.b.lo, b.b.hi, minDimension, true);
        });
        std::vector<int> built;
        for (Bucket& bucket : minBuckets) built.push_back(build(std::move(bucket.children)));
        return makeSubTree(built);
    }
};

Tree::Tree(const std::vector<Entry>& entries) {
    std::vector<int> leaves;
    for (const Entry& e : entries) {
        Node n;
        for (int d = 0; d < kDims; ++d) n.lo[d] = e.space[d].min, n.hi[d] = e.space[d].max;
        const auto it = std::find_if(names_.begin(), names_.end(),
                                     [&](const char* s) { return std::string_view(s) == e.biome; });
        n.biome = int(it - names_.begin());
        if (it == names_.end()) names_.push_back(e.biome);
        leaves.push_back(int(nodes_.size()));
        nodes_.push_back(n);
    }
    TreeBuilder builder{*this};
    root_ = builder.build(std::move(leaves));
}

// Sum of squared per-dimension distances (RTree.Node.distance), with Java's wrapping long math.
i64 Tree::distance(int node, const i64* target) const {
    const Node& n = nodes_[node];
    u64 sum = 0;
    for (int d = 0; d < kDims; ++d) {
        const i64 above = i64(u64(target[d]) - u64(n.hi[d]));
        const i64 below = i64(u64(n.lo[d]) - u64(target[d]));
        const i64 dist = above > 0 ? above : (below > 0 ? below : 0);
        sum += u64(dist) * u64(dist);
    }
    return i64(sum);
}

// RTree.SubTree.search: depth-first in child order, pruning children whose bounds are not strictly
// closer than the best so far. Seeding with the previous result means ties go to that leaf.
int Tree::searchNode(int node, const i64* target, int candidate) const {
    const Node& n = nodes_[node];
    if (n.count == 0) return node;
    i64 minDistance = candidate < 0 ? INT64_MAX : distance(candidate, target);
    int closest = candidate;
    for (int i = 0; i < n.count; ++i) {
        const int child = children_[n.first + i];
        const i64 childDistance = distance(child, target);
        if (minDistance > childDistance) {
            const int leaf = searchNode(child, target, closest);
            const i64 leafDistance = child == leaf ? childDistance : distance(leaf, target);
            if (minDistance > leafDistance) {
                minDistance = leafDistance;
                closest = leaf;
            }
        }
    }
    return closest;
}

int Tree::search(const i64 target[kDims], int& hint) const {
    hint = searchNode(root_, target, hint);
    return nodes_[hint].biome;
}

const Tree& overworldTree() {
    static const Tree tree(overworldEntries());
    return tree;
}

}  // namespace wg::climate
