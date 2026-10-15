// Shader pack pipeline: one frame (see pack_pipeline.h for the order), the PackSink for extension passes, and the
// Iris standard uniforms.
#include "render/packs/pack_programs.h"

#include "assets/assets.h"
#include "core/math.h"
#include "render/packs/pack_api.h"
#include "render/shading.h"
#include "render/upscalers/upscalers.h"

#include <algorithm>
#include <cmath>
#include <set>

using namespace packs;

namespace {

constexpr D3D12_RESOURCE_STATES kSrv = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
constexpr D3D12_RESOURCE_STATES kRt = D3D12_RESOURCE_STATE_RENDER_TARGET;
constexpr D3D12_RESOURCE_STATES kUav = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
constexpr D3D12_RESOURCE_STATES kDepthWrite = D3D12_RESOURCE_STATE_DEPTH_WRITE;
constexpr D3D12_RESOURCE_STATES kDepthRead = D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
constexpr u32 kCommandBytes = 28;
constexpr float kNear = 0.05f;

// Column-major 4x4 helpers (GL conventions: m[c * 4 + r]).
struct M4 {
    float m[16] = {};
    static M4 identity() {
        M4 r;
        r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1;
        return r;
    }
    float& at(int r, int c) { return m[c * 4 + r]; }
    float at(int r, int c) const { return m[c * 4 + r]; }
};
M4 mul(const M4& a, const M4& b) {
    M4 r;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) {
            float s = 0;
            for (int k = 0; k < 4; ++k) s += a.at(i, k) * b.at(k, j);
            r.at(i, j) = s;
        }
    return r;
}
void mulVec(const M4& a, const float v[4], float out[4]) {
    for (int i = 0; i < 4; ++i) out[i] = a.at(i, 0) * v[0] + a.at(i, 1) * v[1] + a.at(i, 2) * v[2] + a.at(i, 3) * v[3];
}
M4 inverse(const M4& a) {
    const float* m = a.m;
    float inv[16];
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
    float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    M4 r;
    if (det == 0) return M4::identity();
    for (int i = 0; i < 16; ++i) r.m[i] = inv[i] / det;
    return r;
}
M4 rotX(float deg) {
    const float a = deg * 0.0174532925f, c = std::cos(a), s = std::sin(a);
    M4 r = M4::identity();
    r.at(1, 1) = c, r.at(1, 2) = -s, r.at(2, 1) = s, r.at(2, 2) = c;
    return r;
}
M4 rotY(float deg) {
    const float a = deg * 0.0174532925f, c = std::cos(a), s = std::sin(a);
    M4 r = M4::identity();
    r.at(0, 0) = c, r.at(0, 2) = s, r.at(2, 0) = -s, r.at(2, 2) = c;
    return r;
}
M4 rotZ(float deg) {
    const float a = deg * 0.0174532925f, c = std::cos(a), s = std::sin(a);
    M4 r = M4::identity();
    r.at(0, 0) = c, r.at(0, 1) = -s, r.at(1, 0) = s, r.at(1, 1) = c;
    return r;
}
M4 translate(float x, float y, float z) {
    M4 r = M4::identity();
    r.at(0, 3) = x, r.at(1, 3) = y, r.at(2, 3) = z;
    return r;
}
M4 ortho(float l, float r, float b, float t, float n, float f) {
    M4 m = M4::identity();
    m.at(0, 0) = 2 / (r - l), m.at(1, 1) = 2 / (t - b), m.at(2, 2) = -2 / (f - n);
    m.at(0, 3) = -(r + l) / (r - l), m.at(1, 3) = -(t + b) / (t - b), m.at(2, 3) = -(f + n) / (f - n);
    return m;
}
M4 perspective(float fovY, float aspect, float n, float f) {
    M4 m;
    const float k = 1.0f / std::tan(fovY * 0.5f);
    m.at(0, 0) = k / aspect, m.at(1, 1) = k;
    m.at(2, 2) = (f + n) / (n - f), m.at(2, 3) = 2 * f * n / (n - f);
    m.at(3, 2) = -1;
    return m;
}
void mat3Of(const M4& a, float out[9]) {
    for (int c = 0; c < 3; ++c)
        for (int r = 0; r < 3; ++r) out[c * 3 + r] = a.at(r, c);
}
float fracf(float x) { return x - std::floor(x); }
float jmod(float a, float b) { return std::fmod(a, b); }  // Java % on floats: sign of the dividend

// Inverse of the game's celestial angle curve: day fraction (0 = noon) -> time of day ticks (6000 = noon).
double ticksFromSkyAngle(double angle) {
    double lo = 0, hi = 1;
    for (int i = 0; i < 40; ++i) {
        double d = (lo + hi) * 0.5;
        double e = 0.5 - std::cos(d * 3.14159265358979) / 2.0;
        double a = (d * 2.0 + e) / 3.0;
        if (a < angle) lo = d;
        else hi = d;
    }
    double d = (lo + hi) * 0.5;  // frac(time / 24000 - 0.25)
    return std::fmod((d + 0.25) * 24000.0, 24000.0);
}

// McppFrame (adapter.hlsli).
struct FrameConstants {
    i32 camBlock[4];
    float camFrac[4];
    float depth[4];
    float atlas[4];
    float screen[4];
    u32 misc[4];
    float fade[4];  // adapter.hlsli mcppFade
};

// Iris render stages per role.
RenderStage stageOf(PackRole role) {
    switch (role) {
        case PackRole::SkyBasic: return RenderStage::Sky;
        case PackRole::SkyTextured: return RenderStage::Sun;
        case PackRole::Clouds: return RenderStage::Clouds;
        case PackRole::Weather: return RenderStage::RainSnow;
        case PackRole::Entities: case PackRole::EntitiesTranslucent: case PackRole::EntitiesGlowing:
        case PackRole::Lightning: case PackRole::ShadowEntities: return RenderStage::Entities;
        case PackRole::Block: case PackRole::BlockTranslucent: case PackRole::ShadowBlock: return RenderStage::BlockEntities;
        case PackRole::Particles: case PackRole::ParticlesTranslucent: return RenderStage::Particles;
        case PackRole::Hand: return RenderStage::HandSolid;
        case PackRole::HandTranslucent: return RenderStage::HandTranslucent;
        case PackRole::DamagedBlock: return RenderStage::Destroy;
        case PackRole::Line: return RenderStage::Outline;
        default: return RenderStage::None;
    }
}
Gb gbOf(PackRole role) {
    switch (role) {
        case PackRole::SkyBasic: return Gb::SkyBasic;
        case PackRole::SkyTextured: return Gb::SkyTextured;
        case PackRole::Clouds: return Gb::Clouds;
        case PackRole::Weather: return Gb::Weather;
        case PackRole::Entities: return Gb::Entities;
        case PackRole::EntitiesTranslucent: return Gb::EntitiesTrans;
        case PackRole::EntitiesGlowing: return Gb::EntitiesGlowing;
        case PackRole::Block: return Gb::Block;
        case PackRole::BlockTranslucent: return Gb::BlockTrans;
        case PackRole::BeaconBeam: return Gb::BeaconBeam;
        case PackRole::Particles: return Gb::Particles;
        case PackRole::ParticlesTranslucent: return Gb::ParticlesTrans;
        case PackRole::Hand: return Gb::Hand;
        case PackRole::HandTranslucent: return Gb::HandWater;
        case PackRole::ArmorGlint: return Gb::ArmorGlint;
        case PackRole::SpiderEyes: return Gb::SpiderEyes;
        case PackRole::Lightning: return Gb::Lightning;
        case PackRole::Line: return Gb::Line;
        case PackRole::Textured: return Gb::Textured;
        case PackRole::DamagedBlock: return Gb::DamagedBlock;
        case PackRole::ShadowEntities: return Gb::ShadowEntities;
        case PackRole::ShadowBlock: return Gb::ShadowBlock;
        default: return Gb::Basic;
    }
}

}  // namespace

// ==== frame recorder =====================================================================================================

struct Renderer::Packs::Frame {
    Packs& k;
    Renderer& r;
    Pipe& p;
    ID3D12GraphicsCommandList* list;
    const FrameInput& in;
    std::vector<D3D12_RESOURCE_BARRIER> barriers;
    // Per-draw uniform values the frame sets (renderStage, entityId...).
    float vanillaFog[4] = {};

    Frame(Packs& packs, Pipe& pipe, ID3D12GraphicsCommandList* l, const FrameInput& input)
        : k(packs), r(packs.r), p(pipe), list(l), in(input) {}

    // ---- helpers ----
    void trans(ID3D12Resource* res, D3D12_RESOURCE_STATES& state, D3D12_RESOURCE_STATES to) {
        if (!res || state == to) return;
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = res;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = state;
        b.Transition.StateAfter = to;
        barriers.push_back(b);
        state = to;
    }
    void uavBarrier(ID3D12Resource* res) {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        b.UAV.pResource = res;
        barriers.push_back(b);
    }
    void flush() {
        if (!barriers.empty()) list->ResourceBarrier(UINT(barriers.size()), barriers.data());
        barriers.clear();
    }
    D3D12_CPU_DESCRIPTOR_HANDLE cpu(u32 i) const {
        return {p.cpuHeap->GetCPUDescriptorHandleForHeapStart().ptr + size_t(i) * p.srvStride};
    }
    D3D12_CPU_DESCRIPTOR_HANDLE rtv(u32 i) const {
        return {p.rtvHeap->GetCPUDescriptorHandleForHeapStart().ptr + size_t(i) * p.rtvStride};
    }
    D3D12_CPU_DESCRIPTOR_HANDLE dsv(u32 i) const {
        return {p.dsvHeap->GetCPUDescriptorHandleForHeapStart().ptr + size_t(i) * p.dsvStride};
    }
    // The scene depth (reversed Z, GL orientation): the renderer's, or the pack's own at the render size when scaled.
    D3D12_CPU_DESCRIPTOR_HANDLE sceneDsv() const {
        return p.ownDepth ? dsv(Pipe::kOwnDsv) : r.dsvHeap_->GetCPUDescriptorHandleForHeapStart();
    }
    D3D12_GPU_DESCRIPTOR_HANDLE samplerGpu(u32 i) const {
        return {p.samplerHeap->GetGPUDescriptorHandleForHeapStart().ptr + u64(i) * p.samplerStride};
    }
    // n descriptors in this frame's slice of the shader-visible heap.
    u32 gpuAlloc(u32 n) {
        if (p.gpuHead + n > Pipe::kGpuSlice) p.gpuHead = 0;  // overflow: wraps within the slice (should not happen)
        const u32 at = p.frameSlot * Pipe::kGpuSlice + p.gpuHead;
        p.gpuHead += n;
        return at;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE gpuCpu(u32 i) const {
        return {p.gpuHeap->GetCPUDescriptorHandleForHeapStart().ptr + size_t(i) * p.srvStride};
    }
    D3D12_GPU_DESCRIPTOR_HANDLE gpuGpu(u32 i) const {
        return {p.gpuHeap->GetGPUDescriptorHandleForHeapStart().ptr + u64(i) * p.srvStride};
    }
    D3D12_GPU_VIRTUAL_ADDRESS uploadAlloc(u64 size, u8** mapped) {
        size = (size + 255) & ~u64(255);
        if (p.uploadHead + size > Pipe::kUploadSlice) {
            static bool warned = false;
            if (!warned) LOG("shader pack: the per-frame upload slice (%llu MB) overflowed; geometry may flicker",
                             (unsigned long long)(Pipe::kUploadSlice >> 20));
            warned = true;
            p.uploadHead = 0;
        }
        const u64 at = u64(p.frameSlot) * Pipe::kUploadSlice + p.uploadHead;
        p.uploadHead += size;
        *mapped = p.uploadMapped + at;
        return p.upload->GetGPUVirtualAddress() + at;
    }

    // ---- uniforms ----
    void computeUniforms() {
        UniformStore& u = p.uniforms;
        const PackDefinition& def = *p.def;
        const CameraState& cam = *in.cam;
        const PackGameState& g = *k.game_;
        const SkyLighting& sky = r.skyLighting_;
        const double now = nowSeconds();
        p.frameTime = float(std::clamp(now - p.lastTime, 0.0, 1.0));
        p.lastTime = now;
        u.setFloat("frameTime", p.frameTime);
        u.setFloat("frameTimeCounter", float(std::fmod(now - p.startTime, 3600.0)));
        u.setInt("frameCounter", i32(p.frameNumber % 720720));

        // View: GL conventions (right, up, -forward), camera-relative world.
        const float cy = std::cos(cam.yaw), sy = std::sin(cam.yaw), cp = std::cos(cam.pitch), sp = std::sin(cam.pitch);
        const Vec3 fwd{-sy * cp, -sp, cy * cp};
        const Vec3 right{-cy, 0, -sy};  // from the yaw alone (fwd x up flips at +-90 degrees of pitch)
        const Vec3 up = cross(right, fwd);
        M4 view = M4::identity();
        view.at(0, 0) = right.x, view.at(0, 1) = right.y, view.at(0, 2) = right.z;
        view.at(1, 0) = up.x, view.at(1, 1) = up.y, view.at(1, 2) = up.z;
        view.at(2, 0) = -fwd.x, view.at(2, 1) = -fwd.y, view.at(2, 2) = -fwd.z;
        const float aspect = float(p.width) / float(std::max(p.height, 1u));
        const float farDist = std::max(in.viewDistance, 64.0f);
        const float depthFar = farDist * 4.0f;
        const M4 proj = perspective(cam.fovY, aspect, kNear, depthFar);
        u.setMat4("gbufferModelView", view.m);
        u.setMat4("gbufferModelViewInverse", inverse(view).m);
        u.setMat4("gbufferProjection", proj.m);
        u.setMat4("gbufferProjectionInverse", inverse(proj).m);
        if (!p.havePrev) {
            std::memcpy(p.prevView, view.m, 64);
            std::memcpy(p.prevProj, proj.m, 64);
            p.prevCam[0] = cam.x, p.prevCam[1] = cam.y, p.prevCam[2] = cam.z;
        }
        u.setMat4("gbufferPreviousModelView", p.prevView);
        u.setMat4("gbufferPreviousProjection", p.prevProj);
        auto setCam = [&](const char* name, const double c[3]) {
            u.setVec(name, float(c[0]), float(c[1]), float(c[2]), 0, 3);
            const double fl[3] = {std::floor(c[0]), std::floor(c[1]), std::floor(c[2])};
            u.setIvec(std::string(name) + "Int", i32(fl[0]), i32(fl[1]), i32(fl[2]), 0, 3);
            u.setVec(std::string(name) + "Fract", float(c[0] - fl[0]), float(c[1] - fl[1]), float(c[2] - fl[2]), 0, 3);
        };
        const double camPos[3] = {cam.x, cam.y, cam.z};
        setCam("cameraPosition", camPos);
        setCam("previousCameraPosition", p.prevCam);
        u.setVec("eyePosition", float(cam.x), float(cam.y), float(cam.z), 0, 3);
        u.setVec("relativeEyePosition", 0, 0, 0, 0, 3);
        u.setVec("playerLookVector", fwd.x, fwd.y, fwd.z, 0, 3);
        u.setVec("playerBodyVector", fwd.x, 0, fwd.z, 0, 3);
        u.setFloat("eyeAltitude", float(cam.y));
        std::memcpy(p.prevView, view.m, 64);
        std::memcpy(p.prevProj, proj.m, 64);
        p.prevCam[0] = cam.x, p.prevCam[1] = cam.y, p.prevCam[2] = cam.z;
        p.havePrev = true;

        // Time of day: the sky's celestial angle (0 = noon).
        double skyAngle = fracf(sky.sunAngle / 6.2831853f);
        i64 dayTime = g.worldTime >= 0 ? g.worldTime : i64(ticksFromSkyAngle(skyAngle));
        u.setInt("worldTime", i32(dayTime % 24000));
        u.setInt("worldDay", i32(dayTime / 24000));
        u.setInt("moonPhase", g.moonPhase);
        const float sunAngle = skyAngle < 0.75 ? float(skyAngle + 0.25) : float(skyAngle - 0.75);
        const bool isDay = sunAngle <= 0.5f;
        const float shadowAngle = isDay ? sunAngle : sunAngle - 0.5f;
        u.setFloat("sunAngle", sunAngle);
        u.setFloat("shadowAngle", shadowAngle);
        const float spr = def.settings.sunPathRotation;
        auto celestial = [&](float y, float out[3]) {
            M4 c = mul(mul(mul(view, rotY(-90.0f)), rotZ(spr)), rotX(float(skyAngle) * 360.0f));
            const float v[4] = {0, y, 0, 0};
            float o[4];
            mulVec(c, v, o);
            out[0] = o[0], out[1] = o[1], out[2] = o[2];
        };
        float sun[3], moon[3];
        celestial(100.0f, sun);
        celestial(-100.0f, moon);
        u.setVec("sunPosition", sun[0], sun[1], sun[2], 0, 3);
        u.setVec("moonPosition", moon[0], moon[1], moon[2], 0, 3);
        const float* light = isDay ? sun : moon;
        u.setVec("shadowLightPosition", light[0], light[1], light[2], 0, 3);
        {
            const float v[4] = {0, 100, 0, 0};
            float o[4];
            mulVec(view, v, o);
            u.setVec("upPosition", o[0], o[1], o[2], 0, 3);
        }

        // Shadow matrices (Iris ShadowMatrices).
        {
            const float dist = def.settings.shadow.distance;
            const float interval = def.settings.shadow.intervalSize;
            const float sa = shadowAngle < 0.25f ? shadowAngle + 0.75f : shadowAngle - 0.25f;
            M4 mv = mul(mul(mul(translate(0, 0, -100), rotX(90)), rotZ(sa * -360.0f)), rotX(spr));
            if (interval != 0) {
                float ox = jmod(float(cam.x), interval) - interval / 2, oy = jmod(float(cam.y), interval) - interval / 2,
                      oz = jmod(float(cam.z), interval) - interval / 2;
                mv = mul(mv, translate(ox, oy, oz));
            }
            const M4 sp = ortho(-dist, dist, -dist, dist, 0.05f, 256.0f);
            u.setMat4("shadowModelView", mv.m);
            u.setMat4("shadowModelViewInverse", inverse(mv).m);
            u.setMat4("shadowProjection", sp.m);
            u.setMat4("shadowProjectionInverse", inverse(sp).m);
            shadowMV = mv, shadowP = sp;
        }
        gbView = view, gbProj = proj;

        // Weather, eye, colours.
        u.setFloat("rainStrength", sky.rain);
        u.setFloat("thunderStrength", sky.thunder);
        {
            const float target = sky.rain;
            const float half = (target > p.wetness ? def.settings.wetnessHalflife : def.settings.drynessHalflife) / 20.0f;
            p.wetness += (target - p.wetness) * (1.0f - std::exp(-p.frameTime * 0.693f / std::max(half, 0.01f)));
            u.setFloat("wetness", p.wetness);
        }
        const int inWater = g.isEyeInWater >= 0 ? g.isEyeInWater : (sky.cameraInWater ? 1 : 0);
        u.setInt("isEyeInWater", inWater);
        u.setIvec("eyeBrightness", g.eyeBrightness[0], g.eyeBrightness[1]);
        {
            const float half = def.settings.eyeBrightnessHalflife / 20.0f;
            const float kk = 1.0f - std::exp(-p.frameTime * 0.693f / std::max(half, 0.01f));
            for (int c = 0; c < 2; ++c) p.eyeBrightnessSmooth[c] += (float(g.eyeBrightness[c]) - p.eyeBrightnessSmooth[c]) * kk;
            u.setIvec("eyeBrightnessSmooth", i32(p.eyeBrightnessSmooth[0] + 0.5f), i32(p.eyeBrightnessSmooth[1] + 0.5f));
        }
        u.setFloat("nightVision", g.nightVision);
        u.setFloat("blindness", g.blindness);
        u.setFloat("darknessFactor", g.darknessFactor);
        u.setFloat("darknessLightFactor", g.darknessLightFactor);
        u.setFloat("playerMood", g.playerMood);
        u.setFloat("screenBrightness", g.screenBrightness);
        u.setInt("hideGUI", g.hideGui ? 1 : 0);
        u.setVec("fogColor", r.fogColor_[0], r.fogColor_[1], r.fogColor_[2], 0, 3);
        u.setVec("skyColor", sky.skyColor[0], sky.skyColor[1], sky.skyColor[2], 0, 3);
        u.setFloat("viewWidth", float(p.width));
        u.setFloat("viewHeight", float(p.height));
        u.setFloat("aspectRatio", aspect);
        u.setFloat("near", kNear);
        u.setFloat("far", farDist);
        u.setFloat("fogStart", farDist * 0.6f);
        u.setFloat("fogEnd", farDist);
        u.setFloat("fogDensity", 0.0f);
        u.setInt("fogMode", 9729);  // GL_LINEAR
        u.setInt("fogShape", 1);
        u.setFloat("centerDepthSmooth", p.centerDepthSmooth);
        u.setIvec("atlasSize", i32(p.atlasW), i32(p.atlasH));
        u.setIvec("terrainTextureSize", i32(p.atlasW), i32(p.atlasH));
        u.setInt("terrainIconSize", kTexSize);
        const PackPlayerState& pl = *k.player_;
        u.setInt("heldItemId", pl.heldItem[0].empty() ? -1 : k.itemId(pl.heldItem[0]));
        u.setInt("heldItemId2", pl.heldItem[1].empty() ? -1 : k.itemId(pl.heldItem[1]));
        u.setInt("heldBlockLightValue", pl.heldLight[0]);
        u.setInt("heldBlockLightValue2", pl.heldLight[1]);
        u.setBool("isElytraFlying", pl.elytraFlying);
        u.setBool("heavyFog", pl.heavyFog);
        u.setInt("biome", biomeIndex(g.biome));
        u.setInt("biome_category", g.biomeCategory);
        u.setInt("biome_precipitation", g.precipitation);
        u.setFloat("temperature", g.temperature);
        u.setFloat("rainfall", g.rainfall);
        u.setBool("isRightHanded", g.rightHanded);
        u.setBool("is_sneaking", g.sneaking);
        u.setBool("is_sprinting", g.sprinting);
        u.setBool("is_hurt", g.hurt);
        u.setBool("is_invisible", g.invisible);
        u.setBool("is_burning", g.burning);
        u.setBool("is_on_ground", g.onGround);
        u.setBool("isSpectator", g.spectator);
        u.setFloat("currentPlayerHealth", g.health / std::max(g.maxHealth, 1.0f));
        u.setFloat("maxPlayerHealth", g.maxHealth);
        u.setFloat("currentPlayerHunger", g.food / 20.0f);
        u.setFloat("maxPlayerHunger", 20);
        u.setFloat("currentPlayerAir", g.air / std::max(g.maxAir, 1.0f));
        u.setFloat("maxPlayerAir", g.maxAir);
        u.setFloat("cloudHeight", 192.33f);
        u.setFloat("cloudTime", float(std::fmod(now - p.startTime, 3600.0)) * 20.0f);
        u.setInt("bedrockLevel", -64);
        u.setInt("heightLimit", 384);
        u.setFloat("endFlashIntensity", 0);
        u.setVec("endFlashPosition", 0, 1, 0, 0, 3);
        u.setVec("lightningBoltPosition", 0, -1000, 0, 0, 4);
        u.setInt("dhRenderDistance", 0);
        u.setInt("vxRenderDistance", 0);
        for (const auto& [name, v] : g.extra) u.setFloat(name, v);
        u.setFloat("velocity", 0);
        u.setInt("entityId", -1);
        u.setInt("blockEntityId", -1);
        u.setInt("currentRenderedItemId", -1);
        u.setVec("entityColor", 0, 0, 0, 0, 4);
        u.setInt("renderStage", 0);
        u.setFloat("alphaTestRef", 0.1f);
        u.setVec("chunkOffset", 0, 0, 0, 0, 3);

        p.custom_.evaluate(u, p.frameTime);
    }
    M4 gbView, gbProj, shadowMV, shadowP;

    // Built-in matrices of a pass kind: 0 gbuffers, 1 shadow, 2 full screen, 3 hand.
    M4 shadowModel = M4::identity();  // shadow casters drawn from last frame's vertices: the camera's motion
    void setPassMatrices(int kind, const float* model = nullptr) {
        UniformStore& u = p.uniforms;
        M4 mv = kind == 1 ? mul(shadowMV, shadowModel) : kind == 2 ? M4::identity() : gbView;
        if (model && (model[0] != 0 || model[1] != 0 || model[2] != 0)) mv = mul(mv, translate(model[0], model[1], model[2]));
        M4 pr = kind == 1 ? shadowP : kind == 2 ? ortho(0, 1, 0, 1, -1, 1) : gbProj;
        if (kind == 3) {  // hand: Iris's hand projection (70 degrees, depth squeezed by MC_HAND_DEPTH)
            M4 hp = perspective(70.0f * 0.0174532925f, float(p.width) / float(std::max(p.height, 1u)), kNear,
                                std::max(in.viewDistance, 64.0f) * 4.0f);
            M4 squeeze = M4::identity();
            squeeze.at(2, 2) = 0.125f;
            pr = mul(squeeze, hp);
        }
        const M4 mvp = mul(pr, mv), mvi = inverse(mv), pri = inverse(pr);
        u.setMat4("mcpp_ModelViewMatrix", mv.m);
        u.setMat4("mcpp_ProjectionMatrix", pr.m);
        u.setMat4("mcpp_ModelViewProjectionMatrix", mvp.m);
        u.setMat4("mcpp_ModelViewMatrixInverse", mvi.m);
        u.setMat4("mcpp_ProjectionMatrixInverse", pri.m);
        u.setMat4("mcpp_ModelViewProjectionMatrixInverse", inverse(mvp).m);
        M4 t = M4::identity();
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j) t.at(i, j) = mv.at(j, i);
        u.setMat4("mcpp_ModelViewMatrixTranspose", t.m);
        M4 it = M4::identity();
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j) it.at(i, j) = mvi.at(j, i);
        u.setMat4("mcpp_ModelViewMatrixInverseTranspose", it.m);
        float n3[9];
        mat3Of(mv, n3);  // rotation only: the normal matrix is the rotation itself
        u.setMat3("mcpp_NormalMatrix", n3);
        u.setMat3("normalMatrix", n3);
        u.setMat4("modelViewMatrix", mv.m);
        u.setMat4("modelViewMatrixInverse", mvi.m);
        u.setMat4("projectionMatrix", pr.m);
        u.setMat4("projectionMatrixInverse", pri.m);
        u.setMat4("textureMatrix", M4::identity().m);
        u.setMat4("mcpp_TextureMatrix", M4::identity().m);
        UValue& fog = u.at("mcpp_Fog.color");
        fog.count = 4;
        std::copy(r.fogColor_, r.fogColor_ + 4, fog.f);
        u.setFloat("mcpp_Fog.start", std::max(in.viewDistance, 64.0f) * 0.6f);
        u.setFloat("mcpp_Fog.end", std::max(in.viewDistance, 64.0f));
        u.setFloat("mcpp_Fog.density", 0.0f);
        u.setFloat("mcpp_Fog.scale", 1.0f / std::max(in.viewDistance * 0.4f, 1.0f));
    }

    // ---- the game's lightmap (shaders/lightmap.hlsli vanillaLight, from Renderer::lightmap()): 16 x 16, x = block
    // light, y = sky light ----
    void updateLightmap() {
        float lm[3][4];
        r.lightmap_.pack(lm);
        u8* dst = p.lightmapMapped + size_t(p.frameSlot) * 256 * 16;
        auto curve = [](float l) {
            const float f = std::clamp(l / 15.0f, 0.0f, 1.0f);
            return f / (4.0f - 3.0f * f);
        };
        for (int s = 0; s < 16; ++s)
            for (int b = 0; b < 16; ++b) {
                const float bf = float(b) / 15.0f, mixF = 0.9f * (2.0f * bf - 1.0f) * (2.0f * bf - 1.0f);
                float c[3];
                for (int k = 0; k < 3; ++k) {
                    c[k] = lm[0][k] + lm[1][k] * curve(float(s)) * lm[1][3];
                    c[k] += (lm[2][k] + (1.0f - lm[2][k]) * mixF) * curve(float(b)) * lm[2][3];
                    c[k] = std::clamp(c[k], 0.0f, 1.0f);
                }
                const float m = std::max(c[0], std::max(c[1], c[2])), inv = 1.0f - m;
                u8* px = dst + s * 256 + b * 4;
                for (int k = 0; k < 3; ++k) {
                    const float notGamma = m > 0 ? c[k] * ((1.0f - inv * inv * inv * inv) / m) : c[k];
                    px[k] = u8(std::clamp(c[k] + (notGamma - c[k]) * lm[0][3], 0.0f, 1.0f) * 255.0f + 0.5f);
                }
                px[3] = 255;
            }
        D3D12_RESOURCE_STATES st = kSrv;
        trans(p.lightmap.res.Get(), st, D3D12_RESOURCE_STATE_COPY_DEST);
        flush();
        D3D12_TEXTURE_COPY_LOCATION d{}, s{};
        d.pResource = p.lightmap.res.Get();
        d.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        s.pResource = p.lightmapUpload.Get();
        s.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        s.PlacedFootprint.Offset = u64(p.frameSlot) * 256 * 16;
        s.PlacedFootprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, 16, 16, 1, 256};
        list->CopyTextureRegion(&d, 0, 0, 0, &s, nullptr);
        trans(p.lightmap.res.Get(), st, kSrv);
        flush();
    }

    // ---- resources by reference ----
    struct View {
        u32 cpu;
        ID3D12Resource* res;
        D3D12_RESOURCE_STATES* state;
    };
    // Readable view of a resource; `writing` = colortex indices the current pass renders to (read their other copy).
    View srvOf(const ResRef& ref, const std::vector<int>* writing) {
        switch (ref.kind) {
            case Res::Color: {
                Target& t = p.color[ref.index];
                if (!t.tex[0]) return {p.white.srv, nullptr, nullptr};
                int cur = t.alt && p.flipped[ref.index] ? 1 : 0;
                if (writing && std::find(writing->begin(), writing->end(), int(ref.index)) != writing->end()) {
                    if (!t.alt) return {p.white.srv, nullptr, nullptr};
                    cur ^= 1;
                }
                return {t.srv[cur], t.tex[cur].Get(), &t.state[cur]};
            }
            case Res::Depth0: case Res::Depth1: case Res::Depth2: {
                const int i = ref.kind == Res::Depth0 ? 0 : ref.kind == Res::Depth1 ? 1 : 2;
                return {p.depthSrv[i], p.depthTex[i].Get(), &p.depthTexState[i]};
            }
            case Res::Shadow0: case Res::Shadow1: {
                const int i = ref.kind == Res::Shadow0 ? 0 : 1;
                if (!p.shadowDepth[i] || p.inShadow) return {p.depthFar.srv, nullptr, nullptr};  // no feedback
                return {p.shadowSrv[i], p.shadowDepth[i].Get(), &p.shadowState[i]};
            }
            case Res::ShadowColor: {
                Target& t = p.shadowColor[std::min<int>(ref.index, 1)];
                if (!t.tex[0] || p.inShadow) return {p.white.srv, nullptr, nullptr};
                return {t.srv[0], t.tex[0].Get(), &t.state[0]};
            }
            case Res::Noise: return {p.noise.srv, nullptr, nullptr};
            case Res::Lightmap: return {p.lightmap.srv, nullptr, nullptr};
            case Res::Normals: return {p.normals.srv, nullptr, nullptr};
            case Res::Specular: return {p.specular.srv, nullptr, nullptr};
            case Res::Atlas: return {p.atlas.srv, nullptr, nullptr};
            case Res::Custom: return {ref.index < p.custom.size() && p.custom[ref.index].srv ? p.custom[ref.index].srv : p.white.srv,
                                      nullptr, nullptr};
            case Res::Image: {
                Image& im = p.images[ref.index];
                return {im.srv, im.res.Get(), &im.state};
            }
            case Res::DepthFar: return {p.depthFar.srv, nullptr, nullptr};
            default: return {p.white.srv, nullptr, nullptr};
        }
    }
    View uavOf(const ResRef& ref) {
        switch (ref.kind) {
            case Res::Color: {
                Target& t = p.color[ref.index];
                if (!t.tex[0]) return {p.nullUav, nullptr, nullptr};
                const int cur = t.alt && p.flipped[ref.index] ? 1 : 0;
                return {t.uav[cur], t.tex[cur].Get(), &t.state[cur]};
            }
            case Res::ShadowColor: {
                Target& t = p.shadowColor[std::min<int>(ref.index, 1)];
                if (!t.tex[0]) return {p.nullUav, nullptr, nullptr};
                return {t.uav[0], t.tex[0].Get(), &t.state[0]};
            }
            case Res::Image: {
                Image& im = p.images[ref.index];
                return {im.uav, im.res.Get(), &im.state};
            }
            default: return {p.nullUav, nullptr, nullptr};
        }
    }

    // ---- binding ----
    D3D12_GPU_DESCRIPTOR_HANDLE textureTable(const StageBin& st, const std::vector<int>* writing) {
        const u32 n = u32(std::max<size_t>(st.textures.size(), 1));
        const u32 at = gpuAlloc(n);
        for (u32 i = 0; i < n; ++i) {
            View v = i < st.textures.size() ? srvOf(st.textures[i], writing) : View{p.nullSrv, nullptr, nullptr};
            if (v.res) trans(v.res, *v.state, kSrv);
            r.device_->CopyDescriptorsSimple(1, gpuCpu(at + i), cpu(v.cpu), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        }
        return gpuGpu(at);
    }
    D3D12_GPU_DESCRIPTOR_HANDLE imageTable(const StageBin& st) {
        const u32 at = gpuAlloc(kMaxImages);
        for (u32 i = 0; i < kMaxImages; ++i) {
            View v = i < st.images.size() ? uavOf(st.images[i]) : View{p.nullUav, nullptr, nullptr};
            if (v.res) trans(v.res, *v.state, kUav);
            r.device_->CopyDescriptorsSimple(1, gpuCpu(at + i), cpu(v.cpu), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        }
        return gpuGpu(at);
    }
    D3D12_GPU_DESCRIPTOR_HANDLE bufferTable() {
        const u32 at = gpuAlloc(kMaxBuffers);
        for (u32 i = 0; i < kMaxBuffers; ++i)
            r.device_->CopyDescriptorsSimple(1, gpuCpu(at + i), cpu(p.bufferUav[i]), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        return gpuGpu(at);
    }
    D3D12_GPU_DESCRIPTOR_HANDLE drawTextureTable(u32 gtextureCpu) {
        const u32 at = gpuAlloc(3);
        r.device_->CopyDescriptorsSimple(1, gpuCpu(at), cpu(gtextureCpu), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        r.device_->CopyDescriptorsSimple(1, gpuCpu(at + 1), cpu(p.normals.srv), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        r.device_->CopyDescriptorsSimple(1, gpuCpu(at + 2), cpu(p.specular.srv), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        return gpuGpu(at);
    }
    D3D12_GPU_VIRTUAL_ADDRESS uniformBlock(const StageBin& st) {
        if (!st.refl.uniformBytes) return 0;
        u8* dst;
        const D3D12_GPU_VIRTUAL_ADDRESS va = uploadAlloc(st.refl.uniformBytes, &dst);
        std::memset(dst, 0, st.refl.uniformBytes);
        writeUniforms(st.uniforms, dst);
        // gl_TextureMatrix[1]: the lightmap matrix (coordinate / 256 + 1/32).
        for (const UniformSlot& s : st.uniforms)
            if (s.member.name == "mcpp_TextureMatrix" && s.member.arraySize > 1) {
                float lm[16] = {1.0f / 256, 0, 0, 0, 0, 1.0f / 256, 0, 0, 0, 0, 1.0f / 256, 0, 1.0f / 32, 1.0f / 32, 1.0f / 32, 1};
                for (u32 c = 0; c < 4; ++c) std::memcpy(dst + s.member.offset + s.member.arrayStride + c * s.member.matrixStride, lm + c * 4, 16);
            }
        return va;
    }

    // Binds a graphics program's resources (both stages) for the next draws.
    void bindGraphics(const ProgramBin& b, const StageBin& vs, u32 gtextureCpu, const std::vector<int>* writing) {
        list->SetGraphicsRootSignature(p.root.Get());
        ID3D12DescriptorHeap* heaps[] = {p.gpuHeap.Get(), p.samplerHeap.Get()};
        list->SetDescriptorHeaps(2, heaps);
        list->SetGraphicsRootConstantBufferView(kRootFrame, p.frameCb);
        if (const auto va = uniformBlock(vs)) list->SetGraphicsRootConstantBufferView(kRootVsUniforms, va);
        if (const auto va = uniformBlock(b.ps)) list->SetGraphicsRootConstantBufferView(kRootPsUniforms, va);
        list->SetGraphicsRootDescriptorTable(kRootVsTextures, textureTable(vs, writing));
        list->SetGraphicsRootDescriptorTable(kRootPsTextures, textureTable(b.ps, writing));
        list->SetGraphicsRootDescriptorTable(kRootVsSamplers, samplerGpu(vs.samplerTable));
        list->SetGraphicsRootDescriptorTable(kRootPsSamplers, samplerGpu(b.ps.samplerTable));
        list->SetGraphicsRootDescriptorTable(kRootDrawTextures, drawTextureTable(gtextureCpu));
        list->SetGraphicsRootDescriptorTable(kRootDrawSamplers, samplerGpu(b.ps.drawSamplerTable));
        list->SetGraphicsRootDescriptorTable(kRootImages, imageTable(b.ps.images.empty() ? vs : b.ps));
        list->SetGraphicsRootDescriptorTable(kRootBuffers, bufferTable());
    }
    void alphaTest(const AlphaTest& at) {
        const u32 v[4] = {0, at.enabled ? u32(at.func) + 1 : 0u, 0, 0};
        u32 c[4];
        std::memcpy(c, v, 16);
        std::memcpy(&c[0], &at.ref, 4);
        list->SetGraphicsRoot32BitConstants(kRootCount, 4, c, 0);
    }

    // ---- PSOs ----
    // depth: 0 none, 1 test + write (reversed-Z), 2 test only, 3 shadow (LESS_EQUAL + write)
    ID3D12PipelineState* graphicsPso(u16 program, Adapter a, u8 blend, u8 depth, u8 cull) {
        const PsoKey key{program, u8(a), blend, depth, cull, 0};
        auto it = p.psos.find(key);
        if (it != p.psos.end()) return it->second.Get();
        ProgramBin& b = p.programs[program];
        const StageBin& vs = b.vs[size_t(a)];
        ComPtr<ID3D12PipelineState> pso;
        if (vs.present && b.ps.present) {
            const PackDefinition& def = *p.def;
            D3D12_GRAPHICS_PIPELINE_STATE_DESC d{};
            d.pRootSignature = p.root.Get();
            d.VS = {vs.code.data(), vs.code.size()};
            d.PS = {b.ps.code.data(), b.ps.code.size()};
            d.NumRenderTargets = UINT(b.drawBuffers.size());
            for (size_t i = 0; i < b.drawBuffers.size(); ++i) {
                const int db = b.drawBuffers[i];
                TexFormat f = b.final ? TexFormat::RGBA8 : b.shadow ? def.settings.shadow.colorFormat[std::clamp(db, 0, 1)]
                                                                    : def.settings.targets[size_t(std::clamp(db, 0, 15))].format;
                d.RTVFormats[i] = b.final ? DXGI_FORMAT_R8G8B8A8_UNORM : dxgiFormat(f);
                auto& rt = d.BlendState.RenderTarget[i];
                rt.RenderTargetWriteMask = b.psWritten[i] ? D3D12_COLOR_WRITE_ENABLE_ALL : 0;
                BlendMode bm = blend == 255 ? BlendMode{} : vanillaBlend(blend);
                if (b.src->blend) bm = *b.src->blend;
                if (auto bb = b.src->bufferBlend.find(db); bb != b.src->bufferBlend.end()) bm = bb->second;
                if (bm.enabled && !formatIsInt(f)) {
                    rt.BlendEnable = TRUE;
                    rt.SrcBlend = blendFactorD3D(bm.src, false), rt.DestBlend = blendFactorD3D(bm.dst, false);
                    rt.SrcBlendAlpha = blendFactorD3D(bm.srcA, true), rt.DestBlendAlpha = blendFactorD3D(bm.dstA, true);
                    rt.BlendOp = rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
                }
            }
            d.BlendState.IndependentBlendEnable = TRUE;
            d.SampleMask = UINT_MAX;
            d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
            d.RasterizerState.CullMode = cull ? D3D12_CULL_MODE_BACK : D3D12_CULL_MODE_NONE;
            // y flipped: the terrain adapters' clockwise (D3D) fronts become counter-clockwise; PackSink geometry
            // (Generic) keeps the game's counter-clockwise winding (entity models, items), which the flip makes clockwise.
            d.RasterizerState.FrontCounterClockwise = b.final || a == Adapter::Generic ? FALSE : TRUE;
            d.RasterizerState.DepthClipEnable = TRUE;
            d.DepthStencilState.DepthEnable = depth != 0;
            d.DepthStencilState.DepthWriteMask = depth == 1 || depth == 3 ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
            // depth 4: glint over the geometry drawn just before (vanilla EQUAL_DEPTH_TEST). Base and glint come from
            // different pack programs whose positions differ by a few ULPs, so instead of EQUAL: pushed slightly away
            // (reversed Z: smaller) and kept only where the stored depth is no farther, i.e. on that geometry (not on
            // what shows through its transparent texels).
            d.DepthStencilState.DepthFunc = depth == 3 || depth == 4 ? D3D12_COMPARISON_FUNC_LESS_EQUAL
                                                                     : D3D12_COMPARISON_FUNC_GREATER_EQUAL;
            if (depth == 4) d.RasterizerState.DepthBias = -64, d.RasterizerState.SlopeScaledDepthBias = -0.5f;
            d.DSVFormat = depth ? DXGI_FORMAT_D32_FLOAT : DXGI_FORMAT_UNKNOWN;
            d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            d.SampleDesc.Count = 1;
            if (FAILED(r.device_->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&pso)))) {
                LOG("error: shader pack: pipeline for %s failed", b.name.c_str());
                pso.Reset();
            }
        }
        p.psos[key] = pso;
        return pso.Get();
    }
    static D3D12_BLEND blendFactorD3D(BlendFactor f, bool alpha);
    ID3D12PipelineState* computePso(u16 program) {
        ProgramBin& b = p.programs[program];
        auto it = p.computePsos.find(b.name);
        if (it != p.computePsos.end()) return it->second.Get();
        ComPtr<ID3D12PipelineState> pso;
        if (b.cs.present) {
            D3D12_COMPUTE_PIPELINE_STATE_DESC d{};
            d.pRootSignature = p.computeRoot.Get();
            d.CS = {b.cs.code.data(), b.cs.code.size()};
            if (FAILED(r.device_->CreateComputePipelineState(&d, IID_PPV_ARGS(&pso)))) {
                LOG("error: shader pack: compute pipeline for %s failed", b.name.c_str());
                pso.Reset();
            }
        }
        p.computePsos[b.name] = pso;
        return pso.Get();
    }

    const ProgramBin* program(const Program* src, u16* index = nullptr) {
        if (!src) return nullptr;
        auto it = p.programIndex.find(src->name);
        if (it == p.programIndex.end()) return nullptr;
        if (index) *index = it->second;
        return &p.programs[it->second];
    }

    // Render targets of a gbuffers program (current flip state) + scene depth.
    void bindGbufferTargets(const ProgramBin& b, bool depth = true) {
        D3D12_CPU_DESCRIPTOR_HANDLE rts[8];
        for (size_t i = 0; i < b.drawBuffers.size(); ++i) {
            Target& t = p.color[b.drawBuffers[i] & 15];
            const int cur = t.alt && p.flipped[b.drawBuffers[i] & 15] ? 1 : 0;
            if (!t.tex[0]) {
                rts[i] = rtv(p.fallbackRtv);  // should not happen (all draw buffers are allocated)
                continue;
            }
            trans(t.tex[cur].Get(), t.state[cur], kRt);
            rts[i] = rtv(t.rtv[cur]);
        }
        const D3D12_CPU_DESCRIPTOR_HANDLE ds = sceneDsv();
        list->OMSetRenderTargets(UINT(b.drawBuffers.size()), rts, FALSE, depth ? &ds : nullptr);
        const D3D12_VIEWPORT vp{0, 0, float(p.width), float(p.height), 0, 1};
        const D3D12_RECT sc{0, 0, LONG(p.width), LONG(p.height)};
        list->RSSetViewports(1, &vp);
        list->RSSetScissorRects(1, &sc);
    }

    // ---- terrain (indirect draws from the renderer's cull) ----
    void terrain(Gb role, Adapter a, int seg, RenderStage stage, const AlphaTest& defAlpha, u8 blend, u8 depth, bool cull,
                 bool shadow = false) {
        if (!in.haveWorld) return;
        u16 index;
        const ProgramBin* b = program(p.def->resolve(role), &index);
        if (!b || !b->vs[size_t(a)].present) return;
        ID3D12PipelineState* pso = graphicsPso(index, a, blend, depth, cull);
        if (!pso) return;
        p.uniforms.setInt("renderStage", int(stage));
        p.uniforms.setFloat("alphaTestRef", defAlpha.enabled ? defAlpha.ref : 0.0f);
        if (!shadow) bindGbufferTargets(*b);
        bindGraphics(*b, b->vs[size_t(a)], p.atlas.srv, &b->drawBuffers);
        flush();
        list->SetPipelineState(pso);
        alphaTest(b->src->alphaTest ? *b->src->alphaTest : defAlpha);
        list->SetGraphicsRootShaderResourceView(kRootGeometry,
                                                (a == Adapter::Model ? r.modelQuads_ : r.cubeQuads_)->GetGPUVirtualAddress());
        list->SetGraphicsRootShaderResourceView(kRootSections, r.sections_->GetGPUVirtualAddress());
        list->SetGraphicsRootShaderResourceView(kRootColors, r.colors_->GetGPUVirtualAddress());
        list->SetGraphicsRootShaderResourceView(kRootRemap, r.remap_->GetGPUVirtualAddress() + u64(in.frameIndex) * kRemapCapacity * 4);
        list->SetGraphicsRootShaderResourceView(kRootLayers, p.layerTable ? p.layerTable->GetGPUVirtualAddress()
                                                                          : r.colors_->GetGPUVirtualAddress());
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        D3D12_INDEX_BUFFER_VIEW ibv{r.indexBuffer_->GetGPUVirtualAddress(), 65536 * 6 * 4, DXGI_FORMAT_R32_UINT};
        list->IASetIndexBuffer(&ibv);
        if (shadow)
            list->ExecuteIndirect(p.cmdSig.Get(), Pipe::kShadowSegCap, p.shadowArgs.Get(), u64(seg) * Pipe::kShadowSegCap * kCommandBytes,
                                  p.shadowCounts.Get(), u64(seg) * 4);
        else
            list->ExecuteIndirect(p.cmdSig.Get(), r.segCap_[seg], r.args_.Get(), u64(r.segBase_[seg]) * kCommandBytes,
                                  r.counts_.Get(), u64(seg) * 4);
    }

    // ---- full-screen / compute passes with flips ----
    void computeProgram(const Program& c) {
        u16 index;
        const ProgramBin* b = program(&c, &index);
        if (!b) return;
        ID3D12PipelineState* pso = computePso(index);
        if (!pso) return;
        ID3D12DescriptorHeap* heaps[] = {p.gpuHeap.Get(), p.samplerHeap.Get()};
        list->SetDescriptorHeaps(2, heaps);
        list->SetComputeRootSignature(p.computeRoot.Get());
        if (const auto va = uniformBlock(b->cs)) list->SetComputeRootConstantBufferView(kCUniforms, va);
        list->SetComputeRootDescriptorTable(kCTextures, textureTable(b->cs, nullptr));
        list->SetComputeRootDescriptorTable(kCSamplers, samplerGpu(b->cs.samplerTable));
        list->SetComputeRootDescriptorTable(kCImages, imageTable(b->cs));
        list->SetComputeRootDescriptorTable(kCBuffers, bufferTable());
        list->SetComputeRootDescriptorTable(kCDrawTextures, drawTextureTable(p.color[0].srv[0] ? p.color[0].srv[0] : p.white.srv));
        list->SetComputeRootDescriptorTable(kCDrawSamplers, samplerGpu(b->cs.drawSamplerTable));
        list->SetComputeRootConstantBufferView(kCFrame, p.frameCb);
        flush();
        list->SetPipelineState(pso);
        u32 groups[3] = {1, 1, 1};
        if (c.hasWorkGroups) {
            for (int i = 0; i < 3; ++i) groups[i] = u32(std::max(c.workGroups[i], 1));
        } else {
            const float sx = c.hasWorkGroupsRender ? c.workGroupsRender[0] : 1.0f, sy = c.hasWorkGroupsRender ? c.workGroupsRender[1] : 1.0f;
            groups[0] = u32(std::ceil(float(p.width) * sx / float(b->cs.refl.localSize[0])));
            groups[1] = u32(std::ceil(float(p.height) * sy / float(b->cs.refl.localSize[1])));
        }
        list->Dispatch(groups[0], groups[1], groups[2]);
        // Writes are visible to the next pass.
        for (const ResRef& im : b->cs.images) {
            View v = uavOf(im);
            if (v.res) uavBarrier(v.res);
        }
        uavBarrier(nullptr);
        flush();
    }

    void generateMips(Target& t, int version) {
        if (t.mipSrv[version].size() < 2) return;
        list->SetComputeRootSignature(p.utilRoot.Get());
        ID3D12DescriptorHeap* heaps[] = {p.gpuHeap.Get()};
        list->SetDescriptorHeaps(1, heaps);
        list->SetPipelineState(p.psoMip.Get());
        trans(t.tex[version].Get(), t.state[version], kUav);
        flush();
        u32 w = t.w, h = t.h;
        for (size_t m = 1; m < t.mipSrv[version].size(); ++m) {
            w = std::max(1u, w / 2), h = std::max(1u, h / 2);
            const u32 at = gpuAlloc(2);
            r.device_->CopyDescriptorsSimple(1, gpuCpu(at), cpu(t.mipUav[version][m - 1]), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            r.device_->CopyDescriptorsSimple(1, gpuCpu(at + 1), cpu(t.mipUav[version][m]), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            float consts[28] = {};
            consts[25] = float(w), consts[26] = float(h);
            u8* dst;
            const auto va = uploadAlloc(sizeof(consts) + 16, &dst);
            std::memcpy(dst, consts, sizeof(consts));
            list->SetComputeRootConstantBufferView(0, va);
            // The source mip is read through a UAV view (same resource state for all mips).
            list->SetComputeRootDescriptorTable(1, gpuGpu(at));
            list->SetComputeRootDescriptorTable(2, gpuGpu(at + 1));
            list->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
            uavBarrier(t.tex[version].Get());
            flush();
        }
    }

    // A composite-style pass: compute programs first, then the fragment program into the flipped targets.
    void fullscreenPass(const Program& prog, const std::vector<Program>* computes, bool final,
                        D3D12_CPU_DESCRIPTOR_HANDLE backbufferRtv) {
        if (computes)
            for (const Program& c : *computes)
                if (c.ok()) computeProgram(c);
        if (!prog.vs.present || !prog.ok()) return;
        u16 index;
        const ProgramBin* b = program(&prog, &index);
        if (!b) return;
        // Mipmaps requested by this program, on the current copies.
        for (int m : prog.mipmapped) {
            Target& t = p.color[m & 15];
            if (t.tex[0]) generateMips(t, t.alt && p.flipped[m & 15] ? 1 : 0);
        }
        ID3D12PipelineState* pso = graphicsPso(index, Adapter::Fullscreen, 255, 0, 0);
        if (!pso) return;
        setPassMatrices(2);
        // Targets: the other copy of each written buffer (flipped afterwards).
        std::vector<int> writes, inPlace;  // inPlace: written without a flip (their current copy is the target)
        D3D12_CPU_DESCRIPTOR_HANDLE rts[8];
        u32 vw = p.width, vh = p.height;
        if (final) {
            rts[0] = backbufferRtv;
        } else {
            for (size_t i = 0; i < b->drawBuffers.size(); ++i) {
                const int db = b->drawBuffers[i] & 15;
                Target& t = p.color[db];
                const bool noFlip = prog.flip.count(db) && !prog.flip.at(db);
                const int cur = t.alt && p.flipped[db] ? 1 : 0;
                const int dst = t.alt && !noFlip ? cur ^ 1 : cur;
                trans(t.tex[dst].Get(), t.state[dst], kRt);
                rts[i] = rtv(t.rtv[dst]);
                writes.push_back(db);
                if (dst == cur) inPlace.push_back(db);
                if (i == 0) vw = t.w, vh = t.h;
            }
        }
        if (prog.scale[0] > 0 && prog.scale[0] < 1) vw = u32(float(vw) * prog.scale[0]), vh = u32(float(vh) * prog.scale[0]);
        u32 gtex = p.white.srv;
        if (p.color[0].tex[0]) {  // composite programs: gtexture / tex = colortex0
            View v = srvOf({Res::Color, 0}, &inPlace);
            if (v.res) trans(v.res, *v.state, kSrv);
            gtex = v.cpu;
        }
        bindGraphics(*b, b->vs[size_t(Adapter::Fullscreen)], gtex, &inPlace);
        flush();
        list->OMSetRenderTargets(final ? 1 : UINT(b->drawBuffers.size()), rts, FALSE, nullptr);
        const D3D12_VIEWPORT vp{0, 0, float(vw), float(vh), 0, 1};
        const D3D12_RECT sc{0, 0, LONG(vw), LONG(vh)};
        list->RSSetViewports(1, &vp);
        list->RSSetScissorRects(1, &sc);
        list->SetPipelineState(pso);
        alphaTest(prog.alphaTest ? *prog.alphaTest : AlphaTest{});
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        list->DrawInstanced(3, 1, 0, 0);
        if (!final)
            for (int db : writes) {
                const bool noFlip = prog.flip.count(db) && !prog.flip.at(db);
                if (p.color[db].alt && !noFlip) p.flipped[db] = !p.flipped[db];
            }
        for (const auto& [db, f] : prog.flip)  // explicit flips of buffers the pass does not write
            if (f && std::find(writes.begin(), writes.end(), db) == writes.end() && p.color[db & 15].alt)
                p.flipped[db & 15] = !p.flipped[db & 15];
    }

    void passes(const std::vector<Program>& list_, D3D12_CPU_DESCRIPTOR_HANDLE bb) {
        for (const Program& prog : list_) {
            auto it = p.def->computes.find(prog.name);
            fullscreenPass(prog, it != p.def->computes.end() ? &it->second : nullptr, false, bb);
            if (prog.ok() || it != p.def->computes.end()) mark(prog.name.c_str());
        }
    }

    // ---- GPU timings ----
    void mark(const char* name) {
        if (!p.timeQueries) return;
        u32& n = p.markCount[p.frameSlot];
        if (n >= Pipe::kMaxMarks) return;
        list->EndQuery(p.timeQueries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, p.frameSlot * Pipe::kMaxMarks + n);
        p.markNames[p.frameSlot][n++] = name;
    }
    // The slot's previous frame (finished: the renderer waited for its fence before reusing the slot).
    void readTimings() {
        const u32 slot = p.frameSlot, n = p.markCount[slot];
        p.markCount[slot] = 0;
        if (!p.timeReadback || n < 2) return;
        D3D12_RANGE range{size_t(slot) * Pipe::kMaxMarks * 8, size_t(slot) * Pipe::kMaxMarks * 8 + size_t(n) * 8}, none{0, 0};
        u8* m;
        if (FAILED(p.timeReadback->Map(0, &range, reinterpret_cast<void**>(&m)))) return;
        u64 t[Pipe::kMaxMarks];
        std::memcpy(t, m + range.Begin, size_t(n) * 8);
        p.timeReadback->Unmap(0, &none);
        if (++p.timingFrames <= 30) return;  // start-up
        const double k = 1000.0 / double(r.timestampFreq_);
        constexpr size_t kHistory = 512;
        float total = 0;
        for (u32 i = 1; i < n; ++i) {
            const float ms = t[i] >= t[i - 1] ? float(double(t[i] - t[i - 1]) * k) : 0.0f;
            total += ms;
            const char* name = p.markNames[slot][i];
            auto it = std::find_if(p.timing.begin(), p.timing.end(), [&](const auto& e) { return e.first == name; });
            if (it == p.timing.end()) it = p.timing.insert(p.timing.end(), {name, {}});
            if (it->second.size() < kHistory) it->second.push_back(ms);
            else it->second[p.timingFrames % kHistory] = ms;
        }
        if (p.timingTotal.size() < kHistory) p.timingTotal.push_back(total);
        else p.timingTotal[p.timingFrames % kHistory] = total;
        // The renderer's per-pass stats (the debug overlay): the biggest groups.
        RenderStats& st = r.stats_;
        st.passCount = 0;
        for (u32 i = 1; i < n && st.passCount < RenderStats::kMaxPasses; ++i) {
            st.passNames[st.passCount] = p.markNames[slot][i];
            st.passMs[st.passCount++] = t[i] >= t[i - 1] ? float(double(t[i] - t[i - 1]) * k) : 0.0f;
        }
    }
    void resolveTimings() {
        const u32 slot = p.frameSlot, n = p.markCount[slot];
        if (!p.timeQueries || !n) return;
        list->ResolveQueryData(p.timeQueries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * Pipe::kMaxMarks, n,
                               p.timeReadback.Get(), u64(slot) * Pipe::kMaxMarks * 8);
    }

    // ---- depth copies ----
    D3D12_GPU_VIRTUAL_ADDRESS utilConstants(const float* shadowMat, u32 activeCount = 0, const float* camFrac = nullptr) {
        struct {
            float mat[16];
            i32 camBlock[4];
            float camFrac[4];
            float depth[4];
            u32 misc[4];
        } c{};
        if (shadowMat) std::memcpy(c.mat, shadowMat, 64);
        const CameraState& cam = *in.cam;
        const double bx = std::floor(cam.x), by = std::floor(cam.y), bz = std::floor(cam.z);
        c.camBlock[0] = i32(bx), c.camBlock[1] = i32(by), c.camBlock[2] = i32(bz);
        c.camFrac[0] = float(cam.x - bx), c.camFrac[1] = float(cam.y - by), c.camFrac[2] = float(cam.z - bz);
        if (camFrac) std::copy(camFrac, camFrac + 4, c.camFrac);
        const float F = std::max(in.viewDistance, 64.0f) * 4.0f;
        c.depth[0] = F / (F - kNear), c.depth[1] = float(p.width), c.depth[2] = float(p.height);
        c.misc[0] = activeCount, c.misc[1] = Pipe::kShadowSegCap;
        c.misc[2] = u32(p.def->settings.shadow.distance * std::max(p.def->settings.shadow.distanceRenderMul, 1.0f) + 16);
        u8* dst;
        const auto va = uploadAlloc(sizeof(c), &dst);
        std::memcpy(dst, &c, sizeof(c));
        return va;
    }
    D3D12_RESOURCE_STATES sceneDepthState = kDepthWrite;
    void depthCopy(int index) {
        trans(p.sceneDepth(r), sceneDepthState, kDepthRead);
        trans(p.depthTex[index].Get(), p.depthTexState[index], kUav);
        flush();
        list->SetComputeRootSignature(p.utilRoot.Get());
        ID3D12DescriptorHeap* heaps[] = {p.gpuHeap.Get()};
        list->SetDescriptorHeaps(1, heaps);
        list->SetComputeRootConstantBufferView(0, utilConstants(nullptr));
        const u32 at = gpuAlloc(2);
        r.device_->CopyDescriptorsSimple(1, gpuCpu(at), cpu(p.sceneDepthSrv), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        r.device_->CopyDescriptorsSimple(1, gpuCpu(at + 1), cpu(p.depthUav[index]), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        list->SetComputeRootDescriptorTable(1, gpuGpu(at));
        list->SetComputeRootDescriptorTable(2, gpuGpu(at + 1));
        list->SetPipelineState(p.psoDepthToGl.Get());
        list->Dispatch((p.width + 7) / 8, (p.height + 7) / 8, 1);
        trans(p.depthTex[index].Get(), p.depthTexState[index], kSrv);
        trans(p.sceneDepth(r), sceneDepthState, kDepthWrite);
        flush();
    }

    // ---- temporal upscaling (Pipe::scaled): DLSS / FSR / XeSS on the pack's final image ----
    // Start of the frame: this frame's sub-pixel jitter (Halton 2, 3 like the built-in pipeline; packs see it through
    // gl_Position, adapter.hlsli) and the camera the upscaler SDKs read (Upscalers::setCamera).
    Mat4 upVp;  // this frame's unjittered view-projection (camera-relative, reversed Z): motion vectors
    void upscaleCamera() {
        p.jitter[0] = p.jitter[1] = 0;
        if (!p.scaled || !r.upscaler_) return;
        const u32 phases = r.upscalers_->jitterPhases(p.width, p.outW);
        p.jitterPhase = (p.jitterPhase + 1) % phases;
        auto halton = [](u32 i, u32 b) {
            float f = 1, x = 0;
            for (; i > 0; i /= b) f /= float(b), x += f * float(i % b);
            return x;
        };
        p.jitter[0] = (halton(p.jitterPhase + 1, 2) - 0.5f) * 2.0f / float(p.width);
        p.jitter[1] = (halton(p.jitterPhase + 1, 3) - 0.5f) * 2.0f / float(p.height);
        const CameraState& cam = *in.cam;
        const float cy = std::cos(cam.yaw), sy = std::sin(cam.yaw), cp = std::cos(cam.pitch), sp = std::sin(cam.pitch);
        const Vec3 fwd{-sy * cp, -sp, cy * cp};
        const Vec3 right{-cy, 0, -sy};  // from the yaw alone (fwd x up flips at +-90 degrees of pitch)
        const Vec3 up = cross(right, fwd);
        const float aspect = float(p.outW) / float(std::max(p.outH, 1u));
        upscale::CameraFrame c;
        c.pos[0] = cam.x, c.pos[1] = cam.y, c.pos[2] = cam.z;
        const Vec3 basis[3] = {right, up, fwd};
        for (int i = 0; i < 3; ++i) c.right[i] = (&basis[0].x)[i], c.up[i] = (&basis[1].x)[i], c.fwd[i] = (&basis[2].x)[i];
        c.view = viewFromBasis(right, up, fwd);
        c.proj = perspectiveReverseZ(cam.fovY, aspect, kNear);
        c.fovY = cam.fovY, c.aspect = aspect, c.nearZ = kNear;
        c.renderSize[0] = p.width, c.renderSize[1] = p.height;
        c.outputSize[0] = p.outW, c.outputSize[1] = p.outH;
        c.jitterPx[0] = -p.jitter[0] * 0.5f * float(p.width);
        c.jitterPx[1] = p.jitter[1] * 0.5f * float(p.height);
        r.upscalers_->setCamera(c);
        upVp = c.proj * c.view;
    }
    // After final (drawn into upColor): upright depth, camera motion vectors, the upscaler into upOut, then upOut onto
    // the back buffer and the depth (output size, the D3D way up) into the renderer's for the overlays.
    void upscaleToBackbuffer(D3D12_CPU_DESCRIPTOR_HANDLE bb) {
        const CameraState& cam = *in.cam;
        // Upright depth (psoDepthFlip into upDepth).
        D3D12_RESOURCE_STATES upDepthState = kDepthRead;
        trans(p.sceneDepth(r), sceneDepthState, kDepthRead);
        trans(p.upDepth.Get(), upDepthState, kDepthWrite);
        flush();
        ID3D12DescriptorHeap* heaps[] = {p.gpuHeap.Get()};
        list->SetDescriptorHeaps(1, heaps);
        list->SetGraphicsRootSignature(p.utilRoot.Get());
        list->SetGraphicsRootConstantBufferView(0, utilConstants(nullptr));
        u32 at = gpuAlloc(1);
        r.device_->CopyDescriptorsSimple(1, gpuCpu(at), cpu(p.sceneDepthSrv), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        list->SetGraphicsRootDescriptorTable(1, gpuGpu(at));
        const D3D12_CPU_DESCRIPTOR_HANDLE ud = dsv(Pipe::kUpDepthDsv);
        list->OMSetRenderTargets(0, nullptr, FALSE, &ud);
        const D3D12_VIEWPORT vp{0, 0, float(p.width), float(p.height), 0, 1};
        const D3D12_RECT sc{0, 0, LONG(p.width), LONG(p.height)};
        list->RSSetViewports(1, &vp);
        list->RSSetScissorRects(1, &sc);
        list->SetPipelineState(p.psoDepthFlip.Get());
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        list->DrawInstanced(3, 1, 0, 0);
        trans(p.upDepth.Get(), upDepthState, kDepthRead);
        // Camera motion: previous clip from this frame's unjittered NDC + depth.
        D3D12_RESOURCE_STATES motionState = kSrv;
        trans(p.upMotion.Get(), motionState, kUav);
        flush();
        M4 cur, prev;
        std::memcpy(cur.m, upVp.m, 64);
        std::memcpy(prev.m, p.upHistory ? p.upPrevVp : upVp.m, 64);
        M4 move = M4::identity();
        if (p.upHistory)
            move.at(0, 3) = float(cam.x - p.upPrevCam[0]), move.at(1, 3) = float(cam.y - p.upPrevCam[1]),
            move.at(2, 3) = float(cam.z - p.upPrevCam[2]);
        const M4 toPrev = mul(mul(prev, move), inverse(cur));
        const float frac[4] = {p.jitter[0], p.jitter[1], 0, 0};
        list->SetComputeRootSignature(p.utilRoot.Get());
        list->SetComputeRootConstantBufferView(0, utilConstants(toPrev.m, 0, frac));
        at = gpuAlloc(2);
        r.device_->CopyDescriptorsSimple(1, gpuCpu(at), cpu(p.upDepthSrv), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        r.device_->CopyDescriptorsSimple(1, gpuCpu(at + 1), cpu(p.upMotionUav), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        list->SetComputeRootDescriptorTable(1, gpuGpu(at));
        list->SetComputeRootDescriptorTable(2, gpuGpu(at + 1));
        list->SetPipelineState(p.psoMotion.Get());
        list->Dispatch((p.width + 7) / 8, (p.height + 7) / 8, 1);
        trans(p.upMotion.Get(), motionState, kSrv);
        trans(p.fallbackLayer.Get(), p.fallbackState, kSrv);  // the reactive hint (what drew without the pack, upright)
        if (!p.exposureReady) {  // depthFar (1x1 R32F = 1) doubles as the exposure input
            D3D12_RESOURCE_STATES e = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            trans(p.depthFar.res.Get(), e, kSrv);
            p.exposureReady = true;
        }
        flush();
        std::memcpy(p.upPrevVp, upVp.m, 64);
        p.upPrevCam[0] = cam.x, p.upPrevCam[1] = cam.y, p.upPrevCam[2] = cam.z;
        mark("upscaler inputs");

        UpscaleInputs u;
        u.list = list;
        u.color = p.upColor.Get();
        u.depth = p.upDepth.Get();
        u.motion = p.upMotion.Get();
        u.exposure = p.depthFar.res.Get();
        u.reactive = p.fallbackLayer.Get();
        u.output = p.upOut.Get();
        u.jitter[0] = -p.jitter[0] * 0.5f * float(p.width);
        u.jitter[1] = p.jitter[1] * 0.5f * float(p.height);
        u.renderSize[0] = p.width, u.renderSize[1] = p.height;
        u.outputSize[0] = p.outW, u.outputSize[1] = p.outH;
        u.fovY = cam.fovY;
        u.aspect = float(p.outW) / float(std::max(p.outH, 1u));
        u.nearZ = kNear;
        u.frameTimeMs = p.frameTime * 1000.0f;
        u.reset = !p.upHistory;
        r.upscaler_->evaluate(u);
        p.upHistory = true;
        mark("upscale");

        // upOut -> back buffer; the depth -> the renderer's depth buffer (output size, untouched by the pack when scaled).
        D3D12_RESOURCE_STATES outState = kUav;
        trans(p.upOut.Get(), outState, kSrv);
        flush();
        list->SetDescriptorHeaps(1, heaps);
        list->SetGraphicsRootSignature(p.utilRoot.Get());
        const float ratio[4] = {0, 0, float(p.width) / float(p.outW), float(p.height) / float(p.outH)};
        list->SetGraphicsRootConstantBufferView(0, utilConstants(nullptr, 0, ratio));
        at = gpuAlloc(2);
        r.device_->CopyDescriptorsSimple(1, gpuCpu(at), cpu(p.upOutSrv), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        r.device_->CopyDescriptorsSimple(1, gpuCpu(at + 1), cpu(p.sceneDepthSrv), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        const D3D12_VIEWPORT ov{0, 0, float(p.outW), float(p.outH), 0, 1};
        const D3D12_RECT os{0, 0, LONG(p.outW), LONG(p.outH)};
        list->RSSetViewports(1, &ov);
        list->RSSetScissorRects(1, &os);
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        list->OMSetRenderTargets(1, &bb, FALSE, nullptr);
        list->SetGraphicsRootDescriptorTable(1, gpuGpu(at));
        list->SetPipelineState(p.psoUpCopy.Get());
        list->DrawInstanced(3, 1, 0, 0);
        const D3D12_CPU_DESCRIPTOR_HANDLE rd = r.dsvHeap_->GetCPUDescriptorHandleForHeapStart();
        list->OMSetRenderTargets(0, nullptr, FALSE, &rd);
        list->SetGraphicsRootDescriptorTable(1, gpuGpu(at + 1));
        list->SetPipelineState(p.psoDepthUp.Get());
        list->DrawInstanced(3, 1, 0, 0);
        trans(p.upOut.Get(), outState, kUav);
        flush();
        mark("upscaled copy");
    }

    // ---- centerDepthSmooth ----
    void readCenterDepth() {
        const u32 slot = p.frameSlot;
        if (!p.centerReadback || !p.centerPending[slot]) return;
        p.centerPending[slot] = false;
        D3D12_RANGE range{size_t(slot) * 512, size_t(slot) * 512 + 4}, none{0, 0};
        u8* m;
        if (FAILED(p.centerReadback->Map(0, &range, reinterpret_cast<void**>(&m)))) return;
        float d;
        std::memcpy(&d, m + range.Begin, 4);
        p.centerReadback->Unmap(0, &none);
        if (!(d >= 0.0f && d <= 1.0f)) return;
        if (!p.haveCenterDepth) {
            p.centerDepthSmooth = d, p.haveCenterDepth = true;
            return;
        }
        // Same exponential smoothing as eyeBrightnessSmooth (half-life in ticks).
        const float dt = float(std::clamp(nowSeconds() - p.lastTime, 0.0, 1.0));
        const float half = std::max(p.def->settings.centerDepthHalflife / 20.0f, 0.001f);
        p.centerDepthSmooth += (d - p.centerDepthSmooth) * (1.0f - std::exp(-dt * 0.693f / half));
    }
    void copyCenterDepth() {
        if (!p.centerReadback) {
            p.centerReadback = r.createBuffer(512 * Renderer::kFrames, D3D12_HEAP_TYPE_READBACK);
            if (!p.centerReadback) return;
        }
        trans(p.depthTex[0].Get(), p.depthTexState[0], D3D12_RESOURCE_STATE_COPY_SOURCE);
        flush();
        D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
        dst.pResource = p.centerReadback.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint.Offset = u64(p.frameSlot) * 512;
        dst.PlacedFootprint.Footprint = {DXGI_FORMAT_R32_FLOAT, 1, 1, 1, 256};
        src.pResource = p.depthTex[0].Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        const UINT cx = p.width / 2, cy = p.height / 2;
        const D3D12_BOX box{cx, cy, 0, cx + 1, cy + 1, 1};
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
        trans(p.depthTex[0].Get(), p.depthTexState[0], kSrv);
        flush();
        p.centerPending[p.frameSlot] = true;
    }

    // ---- passes without pack support: LDR layer (D3D orientation) merged into colortex0 ----
    void fallbackBegin(PassContext& pc) {
        // Depth the D3D way up for their depth test.
        trans(p.sceneDepth(r), sceneDepthState, kDepthRead);
        trans(p.fallbackLayer.Get(), p.fallbackState, kRt);
        flush();
        list->SetGraphicsRootSignature(p.utilRoot.Get());
        ID3D12DescriptorHeap* heaps[] = {p.gpuHeap.Get()};
        list->SetDescriptorHeaps(1, heaps);
        list->SetGraphicsRootConstantBufferView(0, utilConstants(nullptr));
        const u32 at = gpuAlloc(1);
        r.device_->CopyDescriptorsSimple(1, gpuCpu(at), cpu(p.sceneDepthSrv), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        list->SetGraphicsRootDescriptorTable(1, gpuGpu(at));
        const D3D12_CPU_DESCRIPTOR_HANDLE fd = dsv(p.flippedDsv);
        list->OMSetRenderTargets(0, nullptr, FALSE, &fd);
        const D3D12_VIEWPORT vp{0, 0, float(p.width), float(p.height), 0, 1};
        const D3D12_RECT sc{0, 0, LONG(p.width), LONG(p.height)};
        list->RSSetViewports(1, &vp);
        list->RSSetScissorRects(1, &sc);
        list->SetPipelineState(p.psoDepthFlip.Get());
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        list->DrawInstanced(3, 1, 0, 0);
        trans(p.sceneDepth(r), sceneDepthState, kDepthWrite);
        flush();
        const float clear[4] = {0, 0, 0, 0};
        const D3D12_CPU_DESCRIPTOR_HANDLE fl = rtv(p.fallbackRtv);
        list->ClearRenderTargetView(fl, clear, 0, nullptr);
        list->OMSetRenderTargets(1, &fl, FALSE, &fd);
        // Renderer bindings the passes expect.
        ID3D12DescriptorHeap* rh[] = {r.srvHeap_.Get()};
        list->SetDescriptorHeaps(1, rh);
        pc.shading = false;
        pc.width = p.width, pc.height = p.height;
    }
    void fallbackMerge() {
        trans(p.fallbackLayer.Get(), p.fallbackState, kSrv);
        Target& t = p.color[0];
        const int cur = t.alt && p.flipped[0] ? 1 : 0;
        trans(t.tex[cur].Get(), t.state[cur], kRt);
        flush();
        list->SetGraphicsRootSignature(p.utilRoot.Get());
        ID3D12DescriptorHeap* heaps[] = {p.gpuHeap.Get()};
        list->SetDescriptorHeaps(1, heaps);
        list->SetGraphicsRootConstantBufferView(0, utilConstants(nullptr));
        const u32 at = gpuAlloc(1);
        r.device_->CopyDescriptorsSimple(1, gpuCpu(at), cpu(p.fallbackSrv), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        list->SetGraphicsRootDescriptorTable(1, gpuGpu(at));
        const D3D12_CPU_DESCRIPTOR_HANDLE c0 = rtv(t.rtv[cur]);
        list->OMSetRenderTargets(1, &c0, FALSE, nullptr);
        list->SetPipelineState(p.psoMerge.Get());
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        list->DrawInstanced(3, 1, 0, 0);
    }
    void runStage(PassStage stage, PassContext& pc, bool merge) {
        if (r.passes_[int(stage)].empty()) return;
        fallbackBegin(pc);
        PassContext c = pc;
        c.pack = &k;
        k.inStage_ = true;
        r.runPasses(stage, c);
        k.inStage_ = false;
        k.casterStage_ = stage == PassStage::World;
        k.flushBatches();
        k.casterStage_ = false;  // the stage's pack geometry, batched by program and per-draw uniforms
        if (merge) fallbackMerge();
    }

    // ---- shadow pass ----
    void shadowPass() {
        const PackDefinition& def = *p.def;
        if (!p.shadowEnabled || !in.haveWorld || !p.shadowDepth[0]) return;
        // Cull against the shadow volume.
        const M4 m = mul(shadowP, shadowMV);
        D3D12_RESOURCE_STATES countsState = D3D12_RESOURCE_STATE_COMMON, argsState = D3D12_RESOURCE_STATE_COMMON;
        trans(p.shadowCounts.Get(), countsState, D3D12_RESOURCE_STATE_COPY_DEST);
        flush();
        list->CopyBufferRegion(p.shadowCounts.Get(), 0, r.zeros_.Get(), 0, 16);
        trans(p.shadowCounts.Get(), countsState, kUav);
        trans(p.shadowArgs.Get(), argsState, kUav);
        flush();
        list->SetComputeRootSignature(p.utilRoot.Get());
        list->SetComputeRootConstantBufferView(0, utilConstants(m.m, in.activeCount));
        list->SetComputeRootShaderResourceView(3, r.sections_->GetGPUVirtualAddress());
        list->SetComputeRootShaderResourceView(4, in.activeVa);
        list->SetComputeRootUnorderedAccessView(5, p.shadowCounts->GetGPUVirtualAddress());
        list->SetComputeRootUnorderedAccessView(6, p.shadowArgs->GetGPUVirtualAddress());
        list->SetPipelineState(p.psoShadowCull.Get());
        list->Dispatch((in.activeCount + 63) / 64, 1, 1);
        trans(p.shadowCounts.Get(), countsState, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
        trans(p.shadowArgs.Get(), argsState, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
        // Targets.
        trans(p.shadowDepth[0].Get(), p.shadowState[0], kDepthWrite);
        D3D12_CPU_DESCRIPTOR_HANDLE rts[2];
        for (int i = 0; i < 2; ++i) {
            Target& t = p.shadowColor[i];
            trans(t.tex[0].Get(), t.state[0], kRt);
            rts[i] = rtv(t.rtv[0]);
        }
        flush();
        list->ClearDepthStencilView(dsv(p.shadowDsv), D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
        for (int i = 0; i < 2; ++i)
            if (p.shadowColor[i].clearEnabled) list->ClearRenderTargetView(rts[i], p.shadowColor[i].clear.data(), 0, nullptr);
        setPassMatrices(1);
        p.inShadow = true;
        auto shadowTargets = [&](const ProgramBin& b) {
            D3D12_CPU_DESCRIPTOR_HANDLE t[8];
            for (size_t i = 0; i < b.drawBuffers.size(); ++i) t[i] = rts[std::clamp(b.drawBuffers[i], 0, 1)];
            const D3D12_CPU_DESCRIPTOR_HANDLE d = dsv(p.shadowDsv);
            list->OMSetRenderTargets(UINT(b.drawBuffers.size()), t, FALSE, &d);
            const D3D12_VIEWPORT vp{0, 0, float(p.shadowRes), float(p.shadowRes), 0, 1};
            const D3D12_RECT sc{0, 0, LONG(p.shadowRes), LONG(p.shadowRes)};
            list->RSSetViewports(1, &vp);
            list->RSSetScissorRects(1, &sc);
        };
        const AlphaTest cut{true, AlphaFunc::Greater, 0.1f};
        struct Seg {
            Gb role;
            Adapter a;
            int seg;
            RenderStage stage;
            bool alpha;
        };
        const Seg opaque[] = {{Gb::ShadowSolid, Adapter::Cube, 0, RenderStage::TerrainSolid, false},
                              {Gb::ShadowCutout, Adapter::Cube, 1, RenderStage::TerrainCutoutMipped, true},
                              {Gb::ShadowCutout, Adapter::Model, 3, RenderStage::TerrainCutout, true}};
        for (const Seg& s : opaque) {
            if (!def.settings.shadow.terrain) break;
            const ProgramBin* b = program(def.resolve(s.role));
            if (!b) continue;
            shadowTargets(*b);
            terrain(s.role, s.a, s.seg, s.stage, s.alpha ? cut : AlphaTest{}, u8(PackBlend::Opaque), 3, false, true);
        }
        // Entities and block entities (last frame's vertices, see Packs::casters_).
        if (k.casterPipe_ == &p && !k.casters_.empty()) {
            const CameraState& cam = *in.cam;
            shadowModel = translate(float(k.casterCam_[0] - cam.x), float(k.casterCam_[1] - cam.y), float(k.casterCam_[2] - cam.z));
            setPassMatrices(1);
            for (Renderer::Packs::Batch& c : k.casters_) {
                const bool block = c.draw.role == PackRole::Block || c.draw.role == PackRole::BlockTranslucent;
                if (block ? !def.settings.shadow.blockEntities : !def.settings.shadow.entities) continue;
                PackDraw d = c.draw;
                d.role = block ? PackRole::ShadowBlock : PackRole::ShadowEntities;
                const ProgramBin* b = program(def.resolve(gbOf(d.role)));
                if (!b) continue;
                shadowTargets(*b);
                k.drawRanges(d, c.ranges.data(), c.ranges.size());
            }
            shadowModel = M4::identity();
            setPassMatrices(1);
        }
        // shadowtex1: depth without translucent.
        trans(p.shadowDepth[0].Get(), p.shadowState[0], D3D12_RESOURCE_STATE_COPY_SOURCE);
        trans(p.shadowDepth[1].Get(), p.shadowState[1], D3D12_RESOURCE_STATE_COPY_DEST);
        flush();
        list->CopyResource(p.shadowDepth[1].Get(), p.shadowDepth[0].Get());
        trans(p.shadowDepth[1].Get(), p.shadowState[1], kSrv);
        trans(p.shadowDepth[0].Get(), p.shadowState[0], kDepthWrite);
        flush();
        if (def.settings.shadow.translucent) {
            const ProgramBin* b = program(def.resolve(Gb::ShadowWater));
            if (b) {
                shadowTargets(*b);
                terrain(Gb::ShadowWater, Adapter::Cube, 2, RenderStage::TerrainTranslucent, AlphaTest{},
                        u8(PackBlend::Opaque), 3, false, true);
            }
        }
        p.inShadow = false;
        trans(p.shadowDepth[0].Get(), p.shadowState[0], kSrv);
        for (int i = 0; i < 2; ++i) {
            Target& t = p.shadowColor[i];
            trans(t.tex[0].Get(), t.state[0], kSrv);
        }
        trans(p.shadowCounts.Get(), countsState, D3D12_RESOURCE_STATE_COMMON);
        trans(p.shadowArgs.Get(), argsState, D3D12_RESOURCE_STATE_COMMON);
        flush();
        for (int i = 0; i < 2; ++i)
            if (p.shadowColor[i].mipSrv[0].size() > 1) generateMips(p.shadowColor[i], 0);
    }

    // ---- built-in sky geometry for gbuffers_skybasic (the vanilla sky disc) ----
    void skyDisc() {
        const ProgramBin* b = program(p.def->resolve(Gb::SkyBasic));
        if (!b || !r.skyLighting_.overworld) return;
        std::vector<PackVertex> v;
        auto disc = [&](float y, u32 color) {
            const float g = (y > 0 ? 1.0f : -1.0f) * 512.0f;
            std::vector<PackVertex> ring;
            for (int i = -180; i <= 180; i += 45) {
                const float a = float(i) * 0.0174532925f;
                PackVertex pv{};
                pv.pos[0] = g * std::cos(a), pv.pos[1] = y, pv.pos[2] = 512.0f * std::sin(a);
                pv.color = color;
                pv.normal = packNormal(0, y > 0 ? -1.0f : 1.0f, 0);
                pv.light = 240u << 16;
                pv.entity = -1;
                ring.push_back(pv);
            }
            PackVertex center = ring[0];
            center.pos[0] = 0, center.pos[2] = 0;
            for (size_t i = 0; i + 1 < ring.size(); ++i) {  // triangle fan -> list
                v.push_back(center);
                v.push_back(ring[i]);
                v.push_back(ring[i + 1]);
            }
        };
        const float* sc = r.skyLighting_.skyColor;
        auto rgba = [](float rr, float gg, float bb, float aa) {
            auto c = [](float x) { return u32(std::clamp(x, 0.0f, 1.0f) * 255.0f + 0.5f); };
            return c(rr) | c(gg) << 8 | c(bb) << 16 | c(aa) << 24;
        };
        disc(16.0f, rgba(sc[0], sc[1], sc[2], 1));
        {  // Iris's horizon (HorizonRenderer): an octagonal wall from y -16 to 16 and a floor, so the sky has no gap
            const u32 c = rgba(sc[0], sc[1], sc[2], 1);
            const float rad = 512.0f;
            for (int i = 0; i < 8; ++i) {
                const float a0 = float(i) * 0.785398f, a1 = float(i + 1) * 0.785398f;
                const float x0 = rad * std::cos(a0), z0 = rad * std::sin(a0), x1 = rad * std::cos(a1), z1 = rad * std::sin(a1);
                const float quad[6][3] = {{x0, -16, z0}, {x1, -16, z1}, {x1, 16, z1}, {x0, -16, z0}, {x1, 16, z1}, {x0, 16, z0}};
                for (const auto& q : quad) {
                    PackVertex pv{};
                    pv.pos[0] = q[0], pv.pos[1] = q[1], pv.pos[2] = q[2];
                    pv.color = c;
                    pv.normal = packNormal(-std::cos(a0), 0, -std::sin(a0));
                    pv.light = 240u << 16;
                    pv.entity = -1;
                    v.push_back(pv);
                }
            }
        }
        if (in.cam->y >= 63.0) disc(-16.0f, rgba(sc[0], sc[1], sc[2], 1));  // horizon floor
        if (in.cam->y < 63.0) disc(-16.0f, rgba(0, 0, 0, 1));
        PackDraw d;
        d.role = PackRole::SkyBasic;
        d.renderStage = u8(RenderStage::Sky);
        d.depthTest = false, d.depthWrite = false, d.cull = false, d.quads = false;
        d.vertices = v.data();
        d.vertexCount = u32(v.size());
        k.draw(d);
    }

    void record(PassContext& pc, ID3D12Resource* backbuffer, D3D12_CPU_DESCRIPTOR_HANDLE backbufferRtv) {
        const PackDefinition& def = *p.def;
        p.frameSlot = in.frameIndex;
        p.gpuHead = 0;
        p.uploadHead = 0;
        ++p.frameNumber;
        p.list = list;
        k.frame_ = this;
        readTimings();
        mark("start");
        readCenterDepth();
        upscaleCamera();
        computeUniforms();
        updateLightmap();
        {  // McppFrame
            FrameConstants fc{};
            const CameraState& cam = *in.cam;
            const double bx = std::floor(cam.x), by = std::floor(cam.y), bz = std::floor(cam.z);
            fc.camBlock[0] = i32(bx), fc.camBlock[1] = i32(by), fc.camBlock[2] = i32(bz);
            fc.camFrac[0] = float(cam.x - bx), fc.camFrac[1] = float(cam.y - by), fc.camFrac[2] = float(cam.z - bz);
            const float F = std::max(in.viewDistance, 64.0f) * 4.0f;
            fc.depth[0] = kNear, fc.depth[1] = (F - kNear) / F, fc.depth[2] = F / (F - kNear), fc.depth[3] = float(p.height);
            fc.atlas[0] = float(kTexSize) / float(p.atlasW), fc.atlas[1] = float(kTexSize) / float(p.atlasH);
            fc.atlas[2] = float(p.atlasTilesPerRow), fc.atlas[3] = def.settings.separateAo ? 1.0f : 0.0f;
            fc.screen[0] = float(p.width), fc.screen[1] = float(p.height), fc.screen[2] = 1.0f / float(p.width),
            fc.screen[3] = 1.0f / float(p.height);
            fc.misc[0] = p.layerTableSize;
            std::memcpy(&fc.misc[1], p.jitter, 8);  // adapter.hlsli mcppClip: gbuffers jitter (upscaling)
            std::memcpy(fc.fade, in.fade, sizeof(fc.fade));
            u8* dst;
            p.frameCb = uploadAlloc(sizeof(fc), &dst);
            std::memcpy(dst, &fc, sizeof(fc));
        }
        for (bool& f : p.flipped) f = false;

        // ---- clears ----
        for (int i = 0; i < 16; ++i) {
            Target& t = p.color[i];
            if (!t.tex[0] || !t.clearEnabled) continue;
            float c[4] = {t.clear[0], t.clear[1], t.clear[2], t.clear[3]};
            if (i == 0 && !def.settings.targets[0].clearColor) c[0] = r.fogColor_[0], c[1] = r.fogColor_[1], c[2] = r.fogColor_[2], c[3] = 1;
            for (int v = 0; v < (t.alt ? 2 : 1); ++v) {
                trans(t.tex[v].Get(), t.state[v], kRt);
                flush();
                list->ClearRenderTargetView(rtv(t.rtv[v]), c, 0, nullptr);
            }
        }
        list->ClearDepthStencilView(sceneDsv(), D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0, 0, nullptr);
        sceneDepthState = kDepthWrite;

        mark("clears");
        // ---- begin, shadow, shadowcomp, prepare ----
        passes(def.begin, backbufferRtv);
        if (def.settings.prepareBeforeShadow) passes(def.prepare, backbufferRtv);
        shadowPass();
        mark("shadow map");
        passes(def.shadowcomp, backbufferRtv);
        if (!def.settings.prepareBeforeShadow) passes(def.prepare, backbufferRtv);

        // ---- gbuffers: sky, terrain, World stage ----
        p.phase = 0;
        setPassMatrices(0);
        skyDisc();
        runStage(PassStage::Sky, pc, false);  // the sky passes (and per-frame updates) run; their drawing is not used
        mark("gbuffers sky");
        setPassMatrices(0);
        const AlphaTest none{}, cut{true, AlphaFunc::Greater, 0.1f};
        terrain(Gb::TerrainSolid, Adapter::Cube, 0, RenderStage::TerrainSolid, none, u8(PackBlend::Opaque), 1, true);
        terrain(Gb::TerrainCutout, Adapter::Cube, 1, RenderStage::TerrainCutoutMipped, cut, u8(PackBlend::Opaque), 1, true);
        terrain(Gb::TerrainCutout, Adapter::Model, 3, RenderStage::TerrainCutout, cut, u8(PackBlend::Opaque), 1, true);
        mark("gbuffers terrain");
        runStage(PassStage::World, pc, true);
        mark("gbuffers entities+block entities");
        setPassMatrices(0);
        depthCopy(2);  // depthtex2: no hand
        k.drawHand(false);
        setPassMatrices(0);
        depthCopy(1);
        mark("gbuffers hand+depth copies");

        // ---- deferred ----
        passes(def.deferred, backbufferRtv);

        // ---- translucent ----
        p.phase = 1;
        setPassMatrices(0);
        // culled like the game's translucent terrain (fluids bring their backward faces)
        terrain(Gb::Water, Adapter::Cube, 2, RenderStage::TerrainTranslucent, none, u8(PackBlend::Translucent), 2, true);
        mark("gbuffers water");
        runStage(PassStage::AfterTranslucent, pc, true);
        k.drawHand(true);
        setPassMatrices(0);
        depthCopy(0);
        copyCenterDepth();
        mark("gbuffers particles+clouds+weather");

        // ---- composite, final ----
        passes(def.composite, backbufferRtv);
        const bool upscale = p.scaled && p.upColor && r.upscaler_;
        const D3D12_CPU_DESCRIPTOR_HANDLE finalRtv = upscale ? rtv(p.upColorRtv) : backbufferRtv;
        D3D12_RESOURCE_STATES upColorState = kSrv;
        if (upscale) {
            trans(p.upColor.Get(), upColorState, kRt);
            flush();
        }
        (void)backbuffer;
        static const int debugBuffer = [] {  // MCPP_PACK_BUFFER=N: show colortexN (16, 17: shadowcolor0/1) instead of final
            const char* v = std::getenv("MCPP_PACK_BUFFER");
            return v ? std::atoi(v) : -1;
        }();
        if (debugBuffer >= 0 && (debugBuffer < 16 ? p.color[debugBuffer].tex[0] : debugBuffer < 18 ? p.shadowColor[debugBuffer - 16].tex[0] : debugBuffer < 20 ? p.shadowDepth[debugBuffer - 18].Get() : nullptr)) {
            copyToBackbuffer(finalRtv, debugBuffer);
        } else if (def.final_ && def.final_->ok()) {
            auto it = def.computes.find("final");
            fullscreenPass(*def.final_, it != def.computes.end() ? &it->second : nullptr, true, finalRtv);
            mark("final");
        } else {
            copyToBackbuffer(finalRtv);
            mark("final (copy)");
        }
        if (upscale) {
            trans(p.upColor.Get(), upColorState, kSrv);
            flush();
            upscaleToBackbuffer(backbufferRtv);
        }

        if (p.frameNumber == 120) {  // MCPP_PACK_UNIFORMS=a,b,c: log those values (debugging)
            if (const char* names = std::getenv("MCPP_PACK_UNIFORMS")) {
                std::string list = names;
                for (size_t at = 0; at < list.size();) {
                    size_t end = list.find(',', at);
                    if (end == std::string::npos) end = list.size();
                    const std::string n = list.substr(at, end - at);
                    at = end + 1;
                    const UValue* v = p.uniforms.find(n);
                    if (!v) {
                        LOG("pack uniform %s: missing", n.c_str());
                    } else if (v->isInt) {
                        LOG("pack uniform %s = %d %d %d %d", n.c_str(), v->i[0], v->i[1], v->i[2], v->i[3]);
                    } else {
                        LOG("pack uniform %s = %g %g %g %g", n.c_str(), v->f[0], v->f[1], v->f[2], v->f[3]);
                    }
                }
            }
        }
        if (p.frameNumber == 2) {  // report uniforms the pack reads that nothing provides (they stay zero)
            std::set<std::string> missing;
            for (const ProgramBin& b : p.programs)
                for (const StageBin* st : {&b.vs[0], &b.vs[1], &b.vs[2], &b.vs[3], &b.ps, &b.cs})
                    for (const UniformSlot& u : st->uniforms)
                        if (!u.value->set && u.member.name.compare(0, 5, "mcpp_") != 0) missing.insert(u.member.name);
            for (const std::string& m : missing) {
                LOG("shader pack %s: uniform '%s' is not provided (zero)", p.name.c_str(), m.c_str());
                p.warnings.push_back("uniform not provided: " + m);
            }
        }

        // ---- persistent buffers start the next frame unflipped ----
        for (int i = 0; i < 16; ++i) {
            Target& t = p.color[i];
            if (!t.alt || !p.flipped[i] || t.clearEnabled) continue;
            trans(t.tex[1].Get(), t.state[1], D3D12_RESOURCE_STATE_COPY_SOURCE);
            trans(t.tex[0].Get(), t.state[0], D3D12_RESOURCE_STATE_COPY_DEST);
            flush();
            list->CopyResource(t.tex[0].Get(), t.tex[1].Get());
        }
        for (int i = 0; i < 16; ++i)
            for (int v = 0; v < 2; ++v)
                if (p.color[i].tex[v]) trans(p.color[i].tex[v].Get(), p.color[i].state[v], kRt);
        trans(p.sceneDepth(r), sceneDepthState, kDepthWrite);
        flush();
        mark("flip copies");
        resolveTimings();
        k.frame_ = nullptr;

        // Renderer state for the overlays / Hud passes that follow (back buffer + scene depth, renderer heap).
        ID3D12DescriptorHeap* rh[] = {r.srvHeap_.Get()};
        list->SetDescriptorHeaps(1, rh);
        const D3D12_CPU_DESCRIPTOR_HANDLE ds = r.dsvHeap_->GetCPUDescriptorHandleForHeapStart();  // upsampled when scaled
        list->OMSetRenderTargets(1, &backbufferRtv, FALSE, &ds);
        const D3D12_VIEWPORT vp{0, 0, float(r.width_), float(r.height_), 0, 1};
        const D3D12_RECT sc{0, 0, LONG(r.width_), LONG(r.height_)};
        list->RSSetViewports(1, &vp);
        list->RSSetScissorRects(1, &sc);
        pc.shading = false;
        pc.width = r.width_, pc.height = r.height_;
    }

    void copyToBackbuffer(D3D12_CPU_DESCRIPTOR_HANDLE bb, int buffer = 0) {
        // No final program: colortex0 as is (Iris's default final pass), flipped back to the D3D orientation.
        Target& t = buffer >= 16 ? p.shadowColor[std::min(buffer - 16, 1)] : p.color[buffer];  // debug: 16, 17 shadowcolor0/1
        const int cur = buffer < 16 && t.alt && p.flipped[buffer] ? 1 : 0;
        const bool depth = buffer >= 18;  // debug: 18, 19 shadowtex0/1
        if (depth) {
            trans(p.shadowDepth[buffer - 18].Get(), p.shadowState[buffer - 18], kSrv);
        } else {
            trans(t.tex[cur].Get(), t.state[cur], kSrv);
        }
        flush();
        list->SetGraphicsRootSignature(p.utilRoot.Get());
        ID3D12DescriptorHeap* heaps[] = {p.gpuHeap.Get()};
        list->SetDescriptorHeaps(1, heaps);
        list->SetGraphicsRootConstantBufferView(0, utilConstants(nullptr));
        const u32 at = gpuAlloc(1);
        r.device_->CopyDescriptorsSimple(1, gpuCpu(at), cpu(depth ? p.shadowSrv[buffer - 18] : t.srv[cur]),
                                         D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        list->SetGraphicsRootDescriptorTable(1, gpuGpu(at));
        list->OMSetRenderTargets(1, &bb, FALSE, nullptr);
        list->SetPipelineState(p.psoCopy.Get());
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        list->DrawInstanced(3, 1, 0, 0);
    }
};

D3D12_BLEND Renderer::Packs::Frame::blendFactorD3D(BlendFactor f, bool alpha) {
    switch (f) {
        case BlendFactor::Zero: return D3D12_BLEND_ZERO;
        case BlendFactor::One: return D3D12_BLEND_ONE;
        case BlendFactor::SrcColor: return alpha ? D3D12_BLEND_SRC_ALPHA : D3D12_BLEND_SRC_COLOR;
        case BlendFactor::OneMinusSrcColor: return alpha ? D3D12_BLEND_INV_SRC_ALPHA : D3D12_BLEND_INV_SRC_COLOR;
        case BlendFactor::DstColor: return alpha ? D3D12_BLEND_DEST_ALPHA : D3D12_BLEND_DEST_COLOR;
        case BlendFactor::OneMinusDstColor: return alpha ? D3D12_BLEND_INV_DEST_ALPHA : D3D12_BLEND_INV_DEST_COLOR;
        case BlendFactor::SrcAlpha: return D3D12_BLEND_SRC_ALPHA;
        case BlendFactor::OneMinusSrcAlpha: return D3D12_BLEND_INV_SRC_ALPHA;
        case BlendFactor::DstAlpha: return D3D12_BLEND_DEST_ALPHA;
        case BlendFactor::OneMinusDstAlpha: return D3D12_BLEND_INV_DEST_ALPHA;
        case BlendFactor::SrcAlphaSaturate: return alpha ? D3D12_BLEND_ONE : D3D12_BLEND_SRC_ALPHA_SAT;
    }
    return D3D12_BLEND_ONE;
}

// ==== entry points ========================================================================================================

void Renderer::Packs::record(ID3D12GraphicsCommandList* list, const FrameInput& in, PassContext& pc,
                             ID3D12Resource* backbuffer, D3D12_CPU_DESCRIPTOR_HANDLE backbufferRtv) {
    if (!pipe_) return;
    Frame f(*this, *pipe_, list, in);
    f.record(pc, backbuffer, backbufferRtv);
}

// PackSink: generic geometry from the extension passes, drawn with the role's program right away.
void Renderer::Packs::draw(const PackDraw& d) {
    if (!pipe_ || !frame_ || !d.vertexCount || (!d.vertices && !d.gpuVertices)) return;
    Frame& f = *frame_;
    // Vertices go straight into this frame's upload slice (unless already on the GPU); quads stay quads (shared quad
    // index buffer).
    const u32 count = d.quads ? d.vertexCount & ~3u : d.vertexCount;
    if (!count) return;
    D3D12_GPU_VIRTUAL_ADDRESS va = d.gpuVertices;
    if (!va) {
        u8* dst;
        va = f.uploadAlloc(u64(count) * sizeof(PackVertex), &dst);
        std::memcpy(dst, d.vertices, size_t(count) * sizeof(PackVertex));
    }
    const Batch::Range range{va, count, d.quads};
    if (inStage_) {  // inside an extension stage: batched until the stage ends (the passes keep their own bindings)
        for (Batch& bt : batches_) {
            const PackDraw& a = bt.draw;
            if (a.role == d.role && a.blend == d.blend && a.renderStage == d.renderStage && a.depthTest == d.depthTest &&
                a.depthWrite == d.depthWrite && a.depthEqual == d.depthEqual && a.hand == d.hand && a.cull == d.cull && a.blockAtlas == d.blockAtlas && a.texture == d.texture &&
                a.entityId == d.entityId && a.blockEntityId == d.blockEntityId && a.itemId == d.itemId &&
                std::memcmp(a.entityColor, d.entityColor, sizeof(a.entityColor)) == 0 &&
                std::memcmp(a.translate, d.translate, sizeof(a.translate)) == 0) {
                bt.ranges.push_back(range);
                return;
            }
        }
        batches_.push_back({d, {range}});
        return;
    }
    drawRanges(d, &range, 1);
}

void Renderer::Packs::drawRanges(const PackDraw& d, const Batch::Range* ranges, size_t rangeCount) {
    Frame& f = *frame_;
    Pipe& p = *pipe_;
    const bool shadow = d.role == PackRole::ShadowEntities || d.role == PackRole::ShadowBlock;
    if (shadow != p.inShadow) return;
    u16 index;
    const ProgramBin* b = f.program(p.def->resolve(gbOf(d.role)), &index);
    if (!b || !b->vs[size_t(Adapter::Generic)].present) return;
    const u8 depth = shadow ? 3 : !d.depthTest ? 0 : d.depthEqual ? 4 : d.depthWrite ? 1 : 2;
    ID3D12PipelineState* pso = f.graphicsPso(index, Adapter::Generic, u8(d.blend), depth, d.cull ? 1 : 0);
    if (!pso) return;
    UniformStore& u = p.uniforms;
    const RenderStage stage = d.renderStage ? RenderStage(d.renderStage) : stageOf(d.role);
    u.setInt("renderStage", int(stage));
    u.setInt("entityId", d.entityId);
    u.setInt("blockEntityId", d.blockEntityId);
    u.setInt("currentRenderedItemId", d.itemId);
    u.setVec("entityColor", d.entityColor[0], d.entityColor[1], d.entityColor[2], d.entityColor[3], 4);
    const bool hand = d.role == PackRole::Hand || d.role == PackRole::HandTranslucent || d.hand;
    if (!shadow) f.setPassMatrices(hand ? 3 : 0, d.translate);
    if (!shadow) f.bindGbufferTargets(*b, depth != 0);
    // gtexture: the draw's texture (a view in this frame's slice), the atlas, or white.
    u32 gtex = d.blockAtlas ? p.atlas.srv : p.white.srv;
    if (d.texture && !d.blockAtlas) {
        const u32 at = p.cpuCap - 1;  // scratch view: copied into this frame's descriptor slice right away
        D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Format = d.texture->GetDesc().Format;
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sv.Texture2D.MipLevels = d.texture->GetDesc().MipLevels;
        r.device_->CreateShaderResourceView(d.texture, &sv, f.cpu(at));
        gtex = at;
    }
    f.bindGraphics(*b, b->vs[size_t(Adapter::Generic)], gtex, &b->drawBuffers);
    f.flush();
    ID3D12GraphicsCommandList* list = f.list;
    list->SetPipelineState(pso);
    AlphaTest at{};
    if (d.blend == PackBlend::Opaque && d.role != PackRole::SkyBasic) at = {true, AlphaFunc::Greater, 0.1f};
    f.alphaTest(b->src->alphaTest ? *b->src->alphaTest : at);
    const u32 zero[2] = {0, 0};
    list->SetGraphicsRoot32BitConstants(kRootDraw, 2, zero, 0);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    const D3D12_INDEX_BUFFER_VIEW ibv{p.quadIndex->GetGPUVirtualAddress(), Pipe::kQuadIndexQuads * 6 * 4, DXGI_FORMAT_R32_UINT};
    list->IASetIndexBuffer(&ibv);
    for (size_t i = 0; i < rangeCount; ++i) {
        const Batch::Range& rg = ranges[i];
        if (rg.quads) {
            for (u32 first = 0; first < rg.count; first += Pipe::kQuadIndexQuads * 4) {  // one index buffer's worth
                const u32 n = std::min(rg.count - first, Pipe::kQuadIndexQuads * 4);
                list->SetGraphicsRootShaderResourceView(kRootGeometry, rg.va + u64(first) * sizeof(PackVertex));
                list->DrawIndexedInstanced(n / 4 * 6, 1, 0, 0, 0);
            }
        } else {
            list->SetGraphicsRootShaderResourceView(kRootGeometry, rg.va);
            list->DrawInstanced(rg.count, 1, 0, 0);
        }
    }
}

void Renderer::Packs::drawHand(bool translucent) {
    for (Batch& b : hand_)
        if ((b.draw.role != PackRole::Hand) == translucent) drawRanges(b.draw, b.ranges.data(), b.ranges.size());  // glint: last
    if (translucent) hand_.clear();
}

void Renderer::Packs::flushBatches() {
    std::vector<Batch> batches;
    batches.swap(batches_);
    for (Batch& b : batches) {
        if (b.draw.role == PackRole::Hand || b.draw.role == PackRole::HandTranslucent || b.draw.hand) {
            hand_.push_back(std::move(b));  // drawn where Iris draws the hand (drawHand)
            continue;
        }
        drawRanges(b.draw, b.ranges.data(), b.ranges.size());
    }
    if (casterStage_) {  // the World stage: next frame's shadow casters
        casters_.clear();
        for (Batch& b : batches) {
            const PackRole role = b.draw.role;
            if (role == PackRole::Entities || role == PackRole::EntitiesTranslucent || role == PackRole::Block ||
                role == PackRole::BlockTranslucent)
                casters_.push_back(std::move(b));
        }
        const CameraState& cam = *frame_->in.cam;
        casterCam_[0] = cam.x, casterCam_[1] = cam.y, casterCam_[2] = cam.z;
        casterPipe_ = pipe_.get();
    }
}

bool Renderer::Packs::atlasUv(u32 layer, float u, float v, float out[2]) const {
    if (!pipe_ || !pipe_->atlasW) return false;
    const Pipe& p = *pipe_;
    const u32 frame = layer < r.remapCpu_.size() ? r.remapCpu_[layer] : layer;
    const float tw = float(kTexSize) / float(p.atlasW), th = float(kTexSize) / float(p.atlasH);
    out[0] = (float(frame % p.atlasTilesPerRow) + std::clamp(u, 0.0f, 1.0f)) * tw;
    out[1] = (float(frame / p.atlasTilesPerRow) + std::clamp(v, 0.0f, 1.0f)) * th;
    return true;
}

int Renderer::Packs::entityId(std::string_view name) const {
    if (!pipe_) return -1;
    auto it = pipe_->entityIdByName.find(std::string(name));
    return it == pipe_->entityIdByName.end() ? -1 : it->second;
}

int Renderer::Packs::itemId(std::string_view name) const {
    if (!pipe_ || name.empty()) return -1;
    std::string n(name);
    if (n.find(':') == std::string::npos) n = "minecraft:" + n;
    auto it = pipe_->itemIdByName.find(n);
    return it == pipe_->itemIdByName.end() ? -1 : it->second;
}

int Renderer::Packs::blockId(u16 state) const {
    if (!pipe_ || state >= pipe_->blockIdByState.size()) return -1;
    return pipe_->blockIdByState[state];
}

u32 Renderer::Packs::renderSize(bool height) const {
    if (!pipe_) return height ? r.height_ : r.width_;
    return height ? pipe_->height : pipe_->width;
}

bool Renderer::Packs::frameGenUi() const {
    return pipe_ && pipe_->scaled && pipe_->upHistory && r.upscalers_->frameGenActive() && r.shading_ && r.shading_->ui_;
}

bool Renderer::Packs::frameGenInputs(ID3D12Resource*& depth, ID3D12Resource*& motion) const {
    if (!pipe_ || !pipe_->scaled || !pipe_->upHistory) return false;
    depth = pipe_->upDepth.Get(), motion = pipe_->upMotion.Get();
    return depth && motion;
}

ID3D12Resource* Renderer::Packs::glintTexture(bool item) const {
    return pipe_ ? pipe_->glint[item ? 1 : 0].res.Get() : nullptr;
}

bool Renderer::Packs::vanillaClouds() const { return pipe_ && pipe_->def->settings.clouds != "off"; }

// Median GPU ms per pass of a pack's frames (exit / pack change).
void Renderer::Packs::logTimings(const Pipe& p) {
    if (p.timingTotal.empty()) return;
    auto median = [](std::vector<float> v) {
        std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
        return v[v.size() / 2];
    };
    std::string line;
    char buf[160];
    for (const auto& [name, samples] : p.timing) {
        if (samples.empty()) continue;
        std::snprintf(buf, sizeof(buf), " %s %.3f |", name.c_str(), median(samples));
        line += buf;
    }
    LOG("shader pack %s: gpu ms per pass (median of %zu frames):%s total %.3f", p.name.c_str(), p.timingTotal.size(),
        line.c_str(), median(p.timingTotal));
}
