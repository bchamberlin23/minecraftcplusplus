#pragma once
// Video settings shared by the renderer's vendor SDK layer (render/upscalers/upscalers.h), the Video Settings screen
// and options.txt: shader preset, temporal upscaler (built-in TAA, NVIDIA DLSS, AMD FSR, Intel XeSS), quality mode,
// sharpness, frame generation, low-latency mode, VSync and the frame rate limit. No D3D dependencies.
#include "core/common.h"

#include <string>
#include <string_view>

enum class UpscalerKind : u8 { Taa, Dlss, Fsr, Xess, Count };
// Superset of the vendors' quality modes; each vendor offers a subset (see Upscalers::qualityModes).
enum class UpscaleQuality : u8 { Native, UltraQualityPlus, UltraQuality, Quality, Balanced, Performance, UltraPerformance, Count };
enum class FrameGenKind : u8 { Off, Dlss, Fsr, Xess, Count };
// NVIDIA Reflex (Off / On / On + Boost), AMD Anti-Lag 2 (Off / On), Intel XeLL (Off / On).
enum class LowLatencyMode : u8 { Off, On, Boost, Count };

struct VideoSettings {
    u8 shaders = 3;  // ShaderPreset (render/renderer.h): 0 off .. 4 ultra, 5 path traced
    u8 pathTracing = 2;  // PathTraceQuality (render/renderer.h) with Shaders: Path Traced: 0 low .. 3 ultra
    UpscalerKind upscaler = UpscalerKind::Taa;
    UpscaleQuality quality = UpscaleQuality::Native;
    float sharpness = 0.35f;              // 0..1, the pipeline's sharpening after the upscaler
    FrameGenKind frameGen = FrameGenKind::Off;
    u32 frameGenMultiplier = 2;           // presented frames per rendered frame (DLSS Multi Frame Generation: 2..4)
    LowLatencyMode lowLatency = LowLatencyMode::On;
    bool vsync = false;
    u32 fpsLimit = 0;                     // 0 = unlimited
    float renderScaleOverride = 0;        // --render-scale for the built-in TAA (not saved; 0 = from the quality mode)

    bool operator==(const VideoSettings&) const = default;

    static const char* upscalerKey(UpscalerKind k) {
        static const char* n[] = {"taa", "dlss", "fsr", "xess"};
        return n[int(k)];
    }
    static bool parseUpscaler(std::string_view s, UpscalerKind& out) {
        for (int i = 0; i < int(UpscalerKind::Count); ++i)
            if (s == upscalerKey(UpscalerKind(i))) return out = UpscalerKind(i), true;
        if (s == "off" || s == "native") return out = UpscalerKind::Taa, true;
        return false;
    }
    static const char* qualityKey(UpscaleQuality q) {
        static const char* n[] = {"native", "ultraqualityplus", "ultraquality", "quality", "balanced", "performance",
                                  "ultraperformance"};
        return n[int(q)];
    }
    static bool parseQuality(std::string_view s, UpscaleQuality& out) {
        for (int i = 0; i < int(UpscaleQuality::Count); ++i)
            if (s == qualityKey(UpscaleQuality(i))) return out = UpscaleQuality(i), true;
        if (s == "dlaa" || s == "aa" || s == "nativeaa") return out = UpscaleQuality::Native, true;
        if (s == "q") return out = UpscaleQuality::Quality, true;
        if (s == "b") return out = UpscaleQuality::Balanced, true;
        if (s == "p") return out = UpscaleQuality::Performance, true;
        if (s == "up") return out = UpscaleQuality::UltraPerformance, true;
        return false;
    }
    // off | dlss | dlss2x | dlss3x | dlss4x | fsr | xess
    std::string frameGenKey() const {
        switch (frameGen) {
            case FrameGenKind::Dlss: return "dlss" + std::to_string(frameGenMultiplier) + "x";
            case FrameGenKind::Fsr: return "fsr";
            case FrameGenKind::Xess: return "xess";
            default: return "off";
        }
    }
    bool parseFrameGen(std::string_view s) {
        if (s == "off") return frameGen = FrameGenKind::Off, true;
        if (s == "fsr") return frameGen = FrameGenKind::Fsr, frameGenMultiplier = 2, true;
        if (s == "xess") return frameGen = FrameGenKind::Xess, frameGenMultiplier = 2, true;
        if (s == "dlss") return frameGen = FrameGenKind::Dlss, frameGenMultiplier = 2, true;
        if (s.size() == 6 && s.starts_with("dlss") && s[5] == 'x' && s[4] >= '2' && s[4] <= '6')
            return frameGen = FrameGenKind::Dlss, frameGenMultiplier = u32(s[4] - '0'), true;
        return false;
    }
    static const char* pathTracingKey(u8 q) {
        static const char* n[] = {"low", "medium", "high", "ultra"};
        return n[q < 4 ? q : 2];
    }
    static bool parsePathTracing(std::string_view s, u8& out) {
        for (u8 i = 0; i < 4; ++i)
            if (s == pathTracingKey(i)) return out = i, true;
        return false;
    }
    static const char* lowLatencyKey(LowLatencyMode m) {
        static const char* n[] = {"off", "on", "boost"};
        return n[int(m)];
    }
    static bool parseLowLatency(std::string_view s, LowLatencyMode& out) {
        for (int i = 0; i < int(LowLatencyMode::Count); ++i)
            if (s == lowLatencyKey(LowLatencyMode(i))) return out = LowLatencyMode(i), true;
        return false;
    }
};
