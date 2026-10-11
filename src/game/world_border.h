#pragma once
// The client's world border (WorldBorder with its Static / MovingBorderExtent), kept in step with the server's
// border packets and ticked with the level (ClientLevel.tick). Read by the player's collisions, block targeting,
// the border wall (sky/) and the warning vignette (ui/).
#include <algorithm>
#include <cmath>

struct WorldBorder {
    enum class Status { Growing, Shrinking, Stationary };

    double centerX = 0, centerZ = 0;
    int absoluteMaxSize = 29999984;
    int warningTime = 15, warningBlocks = 5;

    void setCenter(double x, double z) { centerX = x, centerZ = z; }
    void setSize(double s) {  // StaticBorderExtent
        moving_ = false;
        size_ = previous_ = to_ = s;
    }
    void lerpSizeBetween(double from, double to, long long ticks) {
        if (from == to) return setSize(to);
        moving_ = true;
        from_ = from, to_ = to;
        duration_ = double(ticks), progress_ = ticks;
        size_ = previous_ = calculate();
    }
    void tick() {  // WorldBorder.tick -> MovingBorderExtent.update
        if (!moving_) return;
        --progress_;
        previous_ = size_;
        size_ = calculate();
        if (progress_ <= 0) setSize(to_);
    }

    double size() const { return size_; }
    double lerpTarget() const { return to_; }
    double lerpSpeed() const { return moving_ ? std::abs(from_ - to_) / duration_ : 0.0; }
    Status status() const { return !moving_ ? Status::Stationary : to_ < from_ ? Status::Shrinking : Status::Growing; }
    // BorderStatus colours (the wall's tint).
    unsigned color() const {
        const Status s = status();
        return s == Status::Growing ? 0x40FF80u : s == Status::Shrinking ? 0xFF3030u : 0x20A0FFu;
    }

    // Edges at a partial tick (the static extent ignores it).
    double minX(float a = 0) const { return clampEdge(centerX - lerped(a) / 2.0); }
    double maxX(float a = 0) const { return clampEdge(centerX + lerped(a) / 2.0); }
    double minZ(float a = 0) const { return clampEdge(centerZ - lerped(a) / 2.0); }
    double maxZ(float a = 0) const { return clampEdge(centerZ + lerped(a) / 2.0); }

    bool isWithinBounds(double x, double z, double margin = 0) const {
        return x >= minX() - margin && x < maxX() + margin && z >= minZ() - margin && z < maxZ() + margin;
    }
    bool blockWithinBounds(int x, int z) const { return isWithinBounds(double(x), double(z)); }  // BlockPos
    double distanceToBorder(double x, double z) const {
        return std::min({x - minX(), maxX() - x, z - minZ(), maxZ() - z});
    }
    // isInsideCloseToBorder: an entity at (x, z) moving through a box of this size collides with the border.
    bool insideCloseToBorder(double x, double z, double boxX, double boxZ) const {
        const double bbMax = std::max(std::max(std::abs(boxX), std::abs(boxZ)), 1.0);
        return distanceToBorder(x, z) < bbMax * 2.0 && isWithinBounds(x, z, bbMax);
    }

private:
    double lerped(float a) const { return moving_ ? previous_ + (size_ - previous_) * double(a) : size_; }
    double clampEdge(double v) const { return std::clamp(v, -double(absoluteMaxSize), double(absoluteMaxSize)); }
    double calculate() const {
        const double progress = (duration_ - double(progress_)) / duration_;
        return progress < 1.0 ? from_ + progress * (to_ - from_) : to_;
    }

    bool moving_ = false;
    double size_ = 5.999997e7f, previous_ = 5.999997e7f, from_ = 5.999997e7f, to_ = 5.999997e7f;
    double duration_ = 0;
    long long progress_ = 0;
};
