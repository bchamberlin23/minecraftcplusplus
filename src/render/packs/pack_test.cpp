// --pack-test <pack.zip | folder> [--dim <dimension id>] [--dump <dir>] [--no-dxc] [--set NAME=VALUE ...]
// Loads a shader pack, translates every program (GLSL -> SPIR-V -> HLSL) and compiles each stage with DXC through
// SPIRV-Cross's own entry point; prints a per-program report. Used to check translator coverage without a window.
#include "render/packs/pack.h"

#include <fstream>

namespace packs {

int runPackTest(int argc, wchar_t** argv) {
    fs::path pack, dump;
    LoadOptions opt;
    bool dxc = true;
    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        auto next = [&]() -> std::wstring { return i + 1 < argc ? argv[++i] : L""; };
        if (a == L"--pack-test") pack = next();
        else if (a == L"--dim") opt.dimension = fs::path(next()).string();
        else if (a == L"--dump") dump = next();
        else if (a == L"--no-dxc") dxc = false;
        else if (a == L"--profile-name") opt.profile = fs::path(next()).string();
        else if (a == L"--set") {
            std::string kv = fs::path(next()).string();
            size_t eq = kv.find('=');
            if (eq != std::string::npos) opt.optionValues[kv.substr(0, eq)] = kv.substr(eq + 1);
        }
    }
    PackDefinition def;
    std::string error;
    opt.failOnErrors = false;  // report every failure
    if (!def.load(pack, opt, error)) {
        LOG("pack-test: %s", error.c_str());
        return 1;
    }
    LOG("pack-test: %s: folder '%s', %zu options, %u stages translated, %u failed, %.0f ms", def.source.name().c_str(),
        def.dimensionDir.c_str(), def.options.decls.size(), def.stagesCompiled, def.stagesFailed, def.loadMs);
    for (const std::string& w : def.warnings) LOG("  warning: %s", w.c_str());
    if (!dump.empty()) fs::create_directories(dump);
    int failures = 0, dxcFailures = 0;
    Timer dxcTimer;
    auto report = [&](const Program& p) {
        std::string db;
        for (int b : p.drawBuffers) db += std::to_string(b) + " ";
        LOG("  %-32s %s%s%s%s drawbuffers [%s]%s", p.name.c_str(), p.vs.present ? "vs " : "", p.fs.present ? "fs " : "",
            p.cs.present ? "cs " : "", p.enabled ? "" : "(disabled) ", db.c_str(), p.ok() ? "" : " NOT OK");
        for (const Stage* st : {&p.vs, &p.fs, &p.cs}) {
            if (!st->present) continue;
            if (!dump.empty()) {
                std::string base = st->file;
                for (char& c : base)
                    if (c == '/') c = '_';
                std::ofstream(dump / (base + ".glsl")) << st->glsl;
                std::ofstream(dump / (base + ".hlsl")) << st->hlsl.hlsl;
            }
            if (!st->error.empty()) {
                ++failures;
                LOG("    %s: %s", st->file.c_str(), st->error.substr(0, 1500).c_str());
                continue;
            }
            if (!dxc || !p.enabled) continue;
            const bool cs = st == &p.cs;
            DxilResult r;
            const char* profile = cs ? "cs_6_0" : st == &p.vs ? "vs_6_0" : "ps_6_0";
            if (!compileHlsl(st->hlsl.hlsl, cs ? "mcpp_cs" : "mcpp_spirv_entry", profile, {}, r)) {
                ++dxcFailures;
                LOG("    %s: DXC: %s", st->file.c_str(), r.error.substr(0, 1500).c_str());
            }
        }
    };
    for (size_t g = 0; g < size_t(Gb::Count); ++g)
        if (def.gbuffers[g]) report(*def.gbuffers[g]);
    for (auto* list : {&def.setup, &def.begin, &def.shadowcomp, &def.prepare, &def.deferred, &def.composite})
        for (const Program& p : *list) {
            if (p.fs.present || p.vs.present) report(p);
            if (auto it = def.computes.find(p.name); it != def.computes.end())
                for (const Program& c : it->second) report(c);
        }
    if (def.final_) report(*def.final_);
    LOG("pack-test: %d translation failures, %d DXC failures (DXC %.0f ms)", failures, dxcFailures, dxcTimer.ms());
    const auto& t = def.settings.targets;
    for (int i = 0; i < 16; ++i)
        if (t[size_t(i)].format != TexFormat::RGBA8 || !t[size_t(i)].clear || t[size_t(i)].size[0] != 1)
            LOG("  colortex%d %s clear=%d size %.2f x %.2f%s", i, texFormatName(t[size_t(i)].format), t[size_t(i)].clear,
                t[size_t(i)].size[0], t[size_t(i)].size[1], t[size_t(i)].sizeRelative ? " (relative)" : "");
    LOG("  shadow %d px, distance %.0f, sunPathRotation %.1f, hw filtering %d/%d; %zu custom textures, %zu images, "
        "%zu uniforms", def.settings.shadow.resolution, def.settings.shadow.distance, def.settings.sunPathRotation,
        def.settings.shadow.hardwareFiltering[0], def.settings.shadow.hardwareFiltering[1], def.textures.size(),
        def.images.size(), def.uniforms.size());
    return failures || dxcFailures ? 2 : 0;
}

}  // namespace packs
