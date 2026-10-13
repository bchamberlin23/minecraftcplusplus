#include "entity/entity_assets.h"

#include "assets/game_reports.h"
#include "assets/asset_source.h"
#include "core/jobs.h"

#include <json/json.hpp>
#include <stb/stb_image.h>

#include <windows.h>

#include <algorithm>
#include <fstream>

using nlohmann::json;

namespace ent {
namespace {

// Bumped whenever the dump format changes, so old caches are regenerated.
constexpr const char* kDumpFile = "entities_v2.json";
constexpr const char* kAnimFile = "animations_v1.json";

// Runs against the client jar: bakes every LayerDefinition (exactly what the game renders) and lists every
// entity type's dimensions, update interval and synched-data ids. Launched as a single-file source program.
constexpr const char* kDumpSource = R"JAVA(
import java.io.*;
import java.lang.reflect.*;
import java.nio.charset.StandardCharsets;
import java.util.*;
import net.minecraft.client.model.geom.*;
import net.minecraft.client.model.geom.builders.LayerDefinition;

public class EntityDump {
    static Field cubesF, childrenF;
    static void f(StringBuilder sb, float v) { if (!Float.isFinite(v)) sb.append(0); else if (v == (int) v) sb.append((int) v); else sb.append(v); }

    @SuppressWarnings("unchecked")
    static void part(StringBuilder sb, ModelPart p) throws Exception {
        PartPose pose = p.getInitialPose();
        sb.append("{\"p\":[");
        float[] pv = {pose.x(), pose.y(), pose.z(), pose.xRot(), pose.yRot(), pose.zRot(), pose.xScale(), pose.yScale(), pose.zScale()};
        for (int i = 0; i < pv.length; i++) { if (i > 0) sb.append(','); f(sb, pv[i]); }
        sb.append("],\"q\":[");
        boolean first = true;
        for (ModelPart.Cube c : (List<ModelPart.Cube>) cubesF.get(p))
            for (ModelPart.Polygon q : c.polygons) {
                if (!first) sb.append(',');
                first = false;
                sb.append('[');
                for (int i = 0; i < 4; i++) {
                    ModelPart.Vertex v = q.vertices()[i];
                    f(sb, v.x()); sb.append(','); f(sb, v.y()); sb.append(','); f(sb, v.z()); sb.append(',');
                    f(sb, v.u()); sb.append(','); f(sb, v.v()); sb.append(',');
                }
                f(sb, q.normal().x()); sb.append(','); f(sb, q.normal().y()); sb.append(','); f(sb, q.normal().z());
                sb.append(']');
            }
        sb.append("],\"c\":{");
        first = true;
        for (Map.Entry<String, ModelPart> e : ((Map<String, ModelPart>) childrenF.get(p)).entrySet()) {
            if (!first) sb.append(',');
            first = false;
            sb.append('"').append(e.getKey()).append("\":");
            part(sb, e.getValue());
        }
        sb.append("}}");
    }

    public static void main(String[] args) throws Exception {
        cubesF = ModelPart.class.getDeclaredField("cubes");
        childrenF = ModelPart.class.getDeclaredField("children");
        cubesF.setAccessible(true);
        childrenF.setAccessible(true);
        StringBuilder sb = new StringBuilder();
        sb.append("{\"layers\":{");
        boolean first = true;
        for (Map.Entry<ModelLayerLocation, LayerDefinition> e : LayerDefinitions.createRoots().entrySet()) {
            ModelPart root;
            try { root = e.getValue().bakeRoot(); } catch (Throwable t) { continue; }
            if (!first) sb.append(',');
            first = false;
            sb.append('"').append(e.getKey().toString()).append("\":");
            part(sb, root);
        }
        sb.append("},\"types\":{");
        net.minecraft.SharedConstants.tryDetectVersion();
        net.minecraft.server.Bootstrap.bootStrap();
        Map<Object, Class<?>> classes = new HashMap<>();
        for (Field fd : Class.forName("net.minecraft.world.entity.EntityTypes").getDeclaredFields()) {
            if (!Modifier.isStatic(fd.getModifiers()) || !(fd.getGenericType() instanceof ParameterizedType pt)) continue;
            if (pt.getActualTypeArguments().length == 1 && pt.getActualTypeArguments()[0] instanceof Class<?> c) {
                fd.setAccessible(true);
                classes.put(fd.get(null), c);
            }
        }
        first = true;
        for (net.minecraft.world.entity.EntityType<?> t : net.minecraft.core.registries.BuiltInRegistries.ENTITY_TYPE) {
            if (!first) sb.append(',');
            first = false;
            net.minecraft.world.entity.EntityDimensions d = t.getDimensions();
            sb.append('"').append(net.minecraft.world.entity.EntityType.getKey(t)).append("\":[");
            f(sb, d.width()); sb.append(','); f(sb, d.height()); sb.append(','); f(sb, d.eyeHeight());
            sb.append(',').append(t.updateInterval()).append(",{");
            boolean firstField = true;
            for (Class<?> c = classes.get(t); c != null && c != Object.class; c = c.getSuperclass())
                for (Field fd : c.getDeclaredFields()) {
                    if (!Modifier.isStatic(fd.getModifiers()) || fd.getType() != net.minecraft.network.syncher.EntityDataAccessor.class) continue;
                    fd.setAccessible(true);
                    Object a = fd.get(null);
                    if (a == null) continue;
                    if (!firstField) sb.append(',');
                    firstField = false;
                    sb.append('"').append(c.getSimpleName()).append('.').append(fd.getName()).append("\":")
                      .append(((net.minecraft.network.syncher.EntityDataAccessor<?>) a).id());
                }
            sb.append("}]");
        }
        sb.append("}}\n");
        File out = new File(args[0]);
        File tmp = new File(args[0] + ".tmp");
        try (Writer w = new OutputStreamWriter(new FileOutputStream(tmp), StandardCharsets.UTF_8)) { w.write(sb.toString()); }
        out.delete();
        if (!tmp.renameTo(out)) throw new IOException("rename failed");
    }
}
)JAVA";

// Runs against the client jar: every keyframe AnimationDefinition (net.minecraft.client.animation.definitions) as
// baked keyframes (radians, model-space positions, scale offsets), for KeyframeAnimation.
constexpr const char* kAnimDumpSource = R"JAVA(
import java.io.*;
import java.lang.reflect.*;
import java.nio.charset.StandardCharsets;
import java.util.*;
import java.util.zip.*;
import net.minecraft.client.animation.*;
import org.joml.Vector3fc;

public class AnimDump {
    static void f(StringBuilder sb, float v) { if (!Float.isFinite(v)) sb.append(0); else if (v == (int) v) sb.append((int) v); else sb.append(v); }
    static void v(StringBuilder sb, Vector3fc x) { sb.append(','); f(sb, x.x()); sb.append(','); f(sb, x.y()); sb.append(','); f(sb, x.z()); }

    public static void main(String[] args) throws Exception {
        StringBuilder sb = new StringBuilder("{\"anims\":{");
        boolean first = true;
        List<String> names = new ArrayList<>();
        try (ZipFile zip = new ZipFile(args[1])) {
            for (Enumeration<? extends ZipEntry> e = zip.entries(); e.hasMoreElements();) {
                String n = e.nextElement().getName();
                if (n.startsWith("net/minecraft/client/animation/definitions/") && n.endsWith(".class") && !n.contains("$"))
                    names.add(n.substring(0, n.length() - 6).replace('/', '.'));
            }
        }
        Collections.sort(names);
        for (String cn : names) {
            Class<?> c = Class.forName(cn);
            for (Field fd : c.getDeclaredFields()) {
                if (!Modifier.isStatic(fd.getModifiers()) || fd.getType() != AnimationDefinition.class) continue;
                fd.setAccessible(true);
                AnimationDefinition d = (AnimationDefinition) fd.get(null);
                if (!first) sb.append(',');
                first = false;
                sb.append('"').append(c.getSimpleName()).append('.').append(fd.getName()).append("\":[");
                f(sb, d.lengthInSeconds());
                sb.append(',').append(d.looping() ? 1 : 0).append(",[");
                boolean firstChannel = true;
                for (Map.Entry<String, List<AnimationChannel>> b : new TreeMap<>(d.boneAnimations()).entrySet())
                    for (AnimationChannel ch : b.getValue()) {
                        if (!firstChannel) sb.append(',');
                        firstChannel = false;
                        int target = ch.target() == AnimationChannel.Targets.POSITION ? 0 : ch.target() == AnimationChannel.Targets.ROTATION ? 1 : 2;
                        sb.append("[\"").append(b.getKey()).append("\",").append(target).append(",[");
                        boolean firstKey = true;
                        for (Keyframe k : ch.keyframes()) {
                            if (!firstKey) sb.append(',');
                            firstKey = false;
                            sb.append('[');
                            f(sb, k.timestamp());
                            sb.append(',').append(k.interpolation() == AnimationChannel.Interpolations.CATMULLROM ? 1 : 0);
                            v(sb, k.preTarget());
                            v(sb, k.postTarget());
                            sb.append(']');
                        }
                        sb.append("]]");
                    }
                sb.append("]]");
            }
        }
        sb.append("}}\n");
        File out = new File(args[0]);
        File tmp = new File(args[0] + ".tmp");
        try (Writer w = new OutputStreamWriter(new FileOutputStream(tmp), StandardCharsets.UTF_8)) { w.write(sb.toString()); }
        out.delete();
        if (!tmp.renameTo(out)) throw new IOException("rename failed");
    }
}
)JAVA";

json readJson(const fs::path& p) {
    std::vector<u8> bytes;
    if (!readFile(p, bytes)) return nullptr;
    return json::parse(bytes.begin(), bytes.end(), nullptr, false);
}

fs::path findJava(const fs::path& minecraftDir, const std::string& component) {
    std::vector<fs::path> roots = {minecraftDir / "runtime"};
    if (const wchar_t* local = _wgetenv(L"LOCALAPPDATA"))
        roots.push_back(fs::path(local) / "Packages" / "Microsoft.4297127D64EC6_8wekyb3d8bbwe" / "LocalCache" / "Local" /
                        "runtime");
    if (const wchar_t* pf = _wgetenv(L"ProgramFiles(x86)")) roots.push_back(fs::path(pf) / "Minecraft Launcher" / "runtime");
    std::error_code ec;
    for (const auto& root : roots) {
        const fs::path java = root / component / "windows-x64" / component / "bin" / "java.exe";
        if (fs::exists(java, ec)) return java;
    }
    return {};
}

bool runHidden(const std::wstring& commandLine, const fs::path& workDir, DWORD timeoutMs) {
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::wstring cmd = commandLine;
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, workDir.c_str(), &si,
                        &pi))
        return false;
    const bool finished = WaitForSingleObject(pi.hProcess, timeoutMs) == WAIT_OBJECT_0;
    DWORD code = 1;
    if (finished) GetExitCodeProcess(pi.hProcess, &code);
    else TerminateProcess(pi.hProcess, 1);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return finished && code == 0;
}

// Runs a dump program (kDumpSource / kAnimDumpSource) with the client jar and its libraries on the class path.
bool generateDump(const fs::path& minecraftDir, const std::string& version, const fs::path& jar, const fs::path& out,
                  const char* sourceName = "EntityDump.java", const char* sourceText = kDumpSource) {
    const fs::path versionDir = minecraftDir / "versions" / version;
    const json v = readJson(versionDir / (version + ".json"));
    if (!v.is_object()) return false;
    const std::string component = v.contains("javaVersion") ? v["javaVersion"].value("component", "") : "";
    const fs::path java = findJava(minecraftDir, component);
    if (java.empty()) {
        LOG("entities: the game's Java runtime (%s) was not found; launch the game once from the launcher",
            component.c_str());
        return false;
    }
    std::wstring cp = jar.wstring();
    for (const auto& lib : v.value("libraries", json::array())) {
        bool allowed = true;
        if (lib.contains("rules")) {
            allowed = false;
            for (const auto& rule : lib["rules"]) {
                const bool matches = !rule.contains("os") || rule["os"].value("name", "") == "windows";
                if (matches) allowed = rule.value("action", "") == "allow";
            }
        }
        if (!allowed || !lib.contains("downloads") || !lib["downloads"].contains("artifact")) continue;
        cp += L";" + (minecraftDir / "libraries" / lib["downloads"]["artifact"].value("path", "")).wstring();
    }
    const fs::path dir = out.parent_path();
    const fs::path source = dir / sourceName;
    {
        std::ofstream f(source, std::ios::binary);
        f << sourceText;
        if (!f) return false;
    }
    LOG("entities: extracting entity models from the game (one time, ~5 s)...");
    // Single-file source launch: needs only java.exe from the game's runtime, no separate compiler step.
    const std::wstring cmd = L"\"" + java.wstring() + L"\" -cp \"" + cp + L"\" \"" + source.wstring() + L"\" \"" +
                             out.wstring() + L"\" \"" + jar.wstring() + L"\"";
    return runHidden(cmd, dir, 300000);
}

Role roleFor(std::string_view n) {
    static const std::pair<std::string_view, Role> kRoles[] = {
        {"head", Head}, {"hat", Hat}, {"body", Body}, {"right_arm", RightArm}, {"left_arm", LeftArm},
        {"right_leg", RightLeg}, {"left_leg", LeftLeg}, {"right_hind_leg", RightHindLeg},
        {"left_hind_leg", LeftHindLeg}, {"right_front_leg", RightFrontLeg}, {"left_front_leg", LeftFrontLeg},
        {"right_middle_hind_leg", RightMiddleHindLeg}, {"left_middle_hind_leg", LeftMiddleHindLeg},
        {"right_middle_front_leg", RightMiddleFrontLeg}, {"left_middle_front_leg", LeftMiddleFrontLeg},
        {"right_wing", RightWing}, {"left_wing", LeftWing}, {"jacket", Jacket}, {"right_sleeve", RightSleeve},
        {"left_sleeve", LeftSleeve}, {"right_pants", RightPants}, {"left_pants", LeftPants},
        {"base_plate", BasePlate}, {"right_body_stick", RightBodyStick}, {"left_body_stick", LeftBodyStick},
        {"shoulder_stick", ShoulderStick}, {"tail", Tail}, {"right_foot", RightFoot}, {"left_foot", LeftFoot},
    };
    for (const auto& [name, role] : kRoles)
        if (name == n) return role;
    return RoleNone;
}

std::string stripNs(std::string_view s) {
    const size_t c = s.find(':');
    return std::string(c == std::string_view::npos ? s : s.substr(c + 1));
}

}  // namespace

int TypeInfo::dataId(std::string_view field) const {
    for (const auto& [k, v] : data) {
        const size_t dot = k.rfind('.');
        if (std::string_view(k).substr(dot + 1) == field) return v;
    }
    return -1;
}

bool EntityAssets::parseModels(const std::vector<u8>& bytes) {
    const json root = json::parse(bytes.begin(), bytes.end(), nullptr, false);
    if (!root.is_object() || !root.contains("layers")) return false;
    for (const auto& [name, layer] : root["layers"].items()) {
        Model m;
        m.name = name;
        std::fill(std::begin(m.roles), std::end(m.roles), i16(-1));
        // Pre-order walk; parents always precede their children.
        struct Item {
            const json* node;
            int parent;
            std::string name;
        };
        std::vector<Item> stack = {{&layer, -1, ""}};
        std::vector<int> depth;
        while (!stack.empty()) {
            Item it = std::move(stack.back());
            stack.pop_back();
            const json& n = *it.node;
            ModelPart p;
            p.parent = it.parent;
            p.role = roleFor(it.name);
            p.name = it.name;
            const auto& pv = n["p"];
            float* pose = &p.pose.x;
            for (int i = 0; i < 9 && i < int(pv.size()); ++i) pose[i] = pv[size_t(i)].get<float>();
            p.quadFirst = u32(quads_.size());
            const int index = int(m.parts.size());
            for (const auto& q : n["q"]) {
                if (q.size() < 23) continue;
                GpuQuad g{};
                for (int v = 0; v < 4; ++v) {
                    for (int a = 0; a < 3; ++a) g.pos[v][a] = q[size_t(v * 5 + a)].get<float>();
                    g.uv[v][0] = q[size_t(v * 5 + 3)].get<float>();
                    g.uv[v][1] = q[size_t(v * 5 + 4)].get<float>();
                }
                for (int a = 0; a < 3; ++a) g.normal[a] = q[size_t(20 + a)].get<float>();
                g.part = u32(index);
                quads_.push_back(g);
            }
            p.quadCount = u32(quads_.size()) - p.quadFirst;
            m.quadCount += p.quadCount;
            if (p.role != RoleNone && m.roles[p.role] < 0) m.roles[p.role] = i16(index);
            m.parts.push_back(p);
            depth.push_back(it.parent >= 0 ? depth[size_t(it.parent)] + 1 : 0);
            for (const auto& [childName, child] : n["c"].items()) stack.push_back({&child, index, childName});
        }
        m.baby = name.find("_baby#") != std::string::npos;
        m.partIndex["root"] = 0;
        for (size_t i = 1; i < m.parts.size(); ++i) {
            auto [it, added] = m.partIndex.emplace(m.parts[i].name, i16(i));
            if (!added && depth[i] < depth[size_t(it->second)]) it->second = i16(i);
        }
        modelIndex_[name] = int(models_.size());
        models_.push_back(std::move(m));
    }
    if (root.contains("types"))
        for (const auto& [name, t] : root["types"].items()) {
            if (!t.is_array() || t.size() < 5) continue;
            TypeInfo ti;
            ti.name = name;
            ti.width = t[0].get<float>(), ti.height = t[1].get<float>(), ti.eye = t[2].get<float>();
            ti.updateInterval = t[3].get<int>();
            for (const auto& [field, id] : t[4].items()) ti.data[field] = id.get<int>();
            typeIndex_[name] = -1 - int(types_.size());  // resolved to protocol ids in load()
            types_.push_back(std::move(ti));
        }
    return !models_.empty();
}

bool EntityAssets::parseAnimations(const std::vector<u8>& bytes) {
    const json root = json::parse(bytes.begin(), bytes.end(), nullptr, false);
    if (!root.is_object() || !root.contains("anims")) return false;
    for (const auto& [name, a] : root["anims"].items()) {
        if (!a.is_array() || a.size() < 3) continue;
        KeyAnimation& k = animations_[name];
        k.length = a[0].get<float>();
        k.looping = a[1].get<int>() != 0;
        for (const auto& c : a[2]) {
            KeyChannel ch;
            ch.bone = c[0].get<std::string>();
            ch.target = u8(c[1].get<int>());
            for (const auto& f : c[2]) {
                Keyframe kf{};
                kf.t = f[0].get<float>();
                kf.catmullRom = u8(f[1].get<int>());
                for (int i = 0; i < 3; ++i) kf.pre[i] = f[size_t(2 + i)].get<float>(), kf.post[i] = f[size_t(5 + i)].get<float>();
                ch.keys.push_back(kf);
            }
            if (!ch.keys.empty()) k.channels.push_back(std::move(ch));
        }
    }
    return !animations_.empty();
}

bool EntityAssets::loadTextures(const fs::path& jar) {
    // The game's textures with the enabled resource packs over them.
    const AssetSource& src = AssetSource::client(jar);
    if (src.empty()) return false;
    struct Tex {
        std::string path;
        std::string key;
        std::vector<u8> rgba;
        int w = 0, h = 0;
    };
    std::vector<Tex> texs;
    for (const std::string& file : src.list("assets/minecraft/textures/", ".png")) {
        std::string_view n = file;
        constexpr std::string_view prefix = "assets/minecraft/textures/";
        n.remove_prefix(prefix.size());
        // misc/enchanted_glint_*: the armour glint, misc/shadow: entity shadows (entity_render.cpp); environment/end_sky:
        // the end portal effect (block entities).
        if (!n.starts_with("entity/") && !n.starts_with("item/") && !n.starts_with("misc/enchanted_glint") &&
            n != "misc/shadow.png" && n != "environment/end_sky.png")
            continue;
        // Large decorative sets the entity renderer does not use here (the plain shield is kept for held shields).
        // (Banner patterns, pot sherds, beacon beams and the end portal are kept: the block entity renderer uses them.)
        if (n.starts_with("entity/shield/") && !n.starts_with("entity/shield/shield_base")) continue;
        texs.push_back({file, std::string(n.substr(0, n.size() - 4))});
    }
    for (const Tex& t : texs) {  // villager hats (VillagerMetadataSection) from the textures' .mcmeta
        if (!t.key.starts_with("entity/villager/") && !t.key.starts_with("entity/zombie_villager/")) continue;
        std::vector<u8> meta;
        if (!src.read(t.path + ".mcmeta", meta)) continue;
        const json j = json::parse(meta.begin(), meta.end(), nullptr, false);
        if (!j.is_object() || !j.contains("villager") || !j["villager"].is_object()) continue;
        const std::string hat = j["villager"].value("hat", "none");
        villagerHats_[t.key] = hat == "full" ? 2 : hat == "partial" ? 1 : 0;
    }
    Jobs::parallelFor(texs.size(), [&](size_t i) {
        Tex& t = texs[i];
        std::vector<u8> bytes;
        if (!src.read(t.path, bytes)) return;
        int w, h, n;
        u8* img = stbi_load_from_memory(bytes.data(), int(bytes.size()), &w, &h, &n, 4);
        if (!img) return;
        if (t.key.starts_with("item/") && h > w) h = w;  // animated item textures: first frame
        t.w = w, t.h = h;
        t.rgba.assign(img, img + size_t(w) * h * 4);
        stbi_image_free(img);
    });
    // Shelf packing, tallest first. Index 0 is a white texel block for untextured geometry.
    std::vector<size_t> order;
    for (size_t i = 0; i < texs.size(); ++i)
        if (!texs[i].rgba.empty()) order.push_back(i);
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        return texs[a].h != texs[b].h ? texs[a].h > texs[b].h : texs[a].w > texs[b].w;
    });
    constexpr u32 kW = 4096, kGap = 2;
    struct Place {
        u32 x, y;
    };
    std::vector<Place> places(texs.size());
    u32 x = 4 + kGap, y = 0, shelf = 4;
    for (size_t i : order) {
        const Tex& t = texs[i];
        if (x + u32(t.w) > kW) x = 0, y += shelf + kGap, shelf = 0;
        places[i] = {x, y};
        x += u32(t.w) + kGap;
        shelf = std::max(shelf, u32(t.h));
    }
    atlasW_ = kW;
    atlasH_ = std::max(16u, (y + shelf + 3) & ~3u);
    constexpr u32 kSlotRows = 2;  // runtime textures: 2 rows of 64 x 64 slots (128 profile skins / capes)
    slotY_ = (atlasH_ + kGap + 3) & ~3u;
    atlasH_ = slotY_ + kSlotRows * 64;
    atlas_.assign(size_t(atlasW_) * atlasH_ * 4, 0);
    for (u32 yy = 0; yy < 4; ++yy)
        for (u32 xx = 0; xx < 4; ++xx) std::memset(&atlas_[(size_t(yy) * atlasW_ + xx) * 4], 255, 4);
    rects_.push_back({0.5f / float(atlasW_), 0.5f / float(atlasH_), 2.0f / float(atlasW_), 2.0f / float(atlasH_), 4, 4});
    texIndex_["white"] = 0;
    for (size_t i : order) {
        const Tex& t = texs[i];
        for (int row = 0; row < t.h; ++row)
            std::memcpy(&atlas_[(size_t(places[i].y + row) * atlasW_ + places[i].x) * 4], &t.rgba[size_t(row) * t.w * 4],
                        size_t(t.w) * 4);
        TexRect r;
        r.u0 = float(places[i].x) / float(atlasW_), r.v0 = float(places[i].y) / float(atlasH_);
        r.du = float(t.w) / float(atlasW_), r.dv = float(t.h) / float(atlasH_);
        r.w = t.w, r.h = t.h;
        texIndex_[t.key] = int(rects_.size());
        rects_.push_back(r);
    }
    slotFirst_ = int(rects_.size());
    slotCount_ = int((atlasH_ - slotY_) / 64 * (kW / 64));
    for (int i = 0; i < slotCount_; ++i) {
        TexRect r;
        r.u0 = float(u32(i) % (kW / 64) * 64) / float(atlasW_), r.v0 = float(slotY_ + u32(i) / (kW / 64) * 64) / float(atlasH_);
        r.du = 64.0f / float(atlasW_), r.dv = 64.0f / float(atlasH_);
        r.w = r.h = 64;
        rects_.push_back(r);
    }
    return true;
}

int EntityAssets::allocSlot() const { return slotsUsed_ < slotCount_ ? slotFirst_ + slotsUsed_++ : -1; }

void EntityAssets::slotOrigin(int tex, u32& x, u32& y) const {
    const u32 i = u32(tex - slotFirst_);
    x = i % (atlasW_ / 64) * 64, y = slotY_ + i / (atlasW_ / 64) * 64;
}

void EntityAssets::writeSlot(int tex, const u8* rgba, int w, int h) const {
    if (tex < slotFirst_ || tex >= slotFirst_ + slotCount_ || w <= 0 || h <= 0 || w > 64 || h > 64) return;
    u32 x, y;
    slotOrigin(tex, x, y);
    for (int row = 0; row < 64; ++row) {
        u8* dst = &atlas_[(size_t(y + u32(row)) * atlasW_ + x) * 4];
        if (row < h) std::memcpy(dst, rgba + size_t(row) * w * 4, size_t(w) * 4), std::memset(dst + w * 4, 0, size_t(64 - w) * 4);
        else std::memset(dst, 0, 64 * 4);
    }
    TexRect& r = rects_[size_t(tex)];
    r.du = float(w) / float(atlasW_), r.dv = float(h) / float(atlasH_), r.w = w, r.h = h;
    dirtySlots_.push_back(tex);
}

std::vector<int> EntityAssets::takeDirtySlots() const {
    std::vector<int> out;
    out.swap(dirtySlots_);
    return out;
}

bool EntityAssets::load(const fs::path& minecraftDir, const std::string& version, const fs::path& jar,
                        const fs::path& cacheDir, const GameReports& reports) {
    try {
        return loadImpl(minecraftDir, version, jar, cacheDir, reports);
    } catch (const std::exception& e) {
        LOG("entities: loading failed (%s); entities will not be drawn", e.what());
        *this = EntityAssets();
        return false;
    }
}

bool EntityAssets::loadImpl(const fs::path& minecraftDir, const std::string& version, const fs::path& jar,
                            const fs::path& cacheDir, const GameReports& reports) {
    Timer t;
    const fs::path dir = cacheDir / version;
    const fs::path file = dir / kDumpFile;
    std::error_code ec;
    fs::create_directories(dir, ec);
    std::vector<u8> bytes;
    if (!fs::exists(file, ec) && !generateDump(minecraftDir, version, jar, file)) {
        LOG("entities: could not extract the entity models (Java dump failed); entities will not be drawn");
        return false;
    }
    if (!readFile(file, bytes) || !parseModels(bytes)) {
        LOG("entities: %s is unreadable; delete it to regenerate", file.string().c_str());
        return false;
    }
    if (!loadTextures(jar)) return false;
    {  // keyframe animations (optional: without them those models keep their procedural poses)
        const fs::path animFile = dir / kAnimFile;
        std::vector<u8> animBytes;
        if (!fs::exists(animFile, ec)) generateDump(minecraftDir, version, jar, animFile, "AnimDump.java", kAnimDumpSource);
        if (!readFile(animFile, animBytes) || !parseAnimations(animBytes))
            LOG("entities: no keyframe animations (%s)", animFile.string().c_str());
    }

    // Protocol ids from the game's registry report.
    const json& reg = reports.registries();
    std::vector<TypeInfo> byId;
    if (reg.is_object() && reg.contains("minecraft:entity_type")) {
        const json entries = reg["minecraft:entity_type"].value("entries", json::object());
        for (const auto& [name, e] : entries.items()) {
            const int id = e.value("protocol_id", -1);
            if (id < 0) continue;
            if (size_t(id) >= byId.size()) byId.resize(size_t(id) + 1);
            auto it = typeIndex_.find(name);
            if (it != typeIndex_.end()) byId[size_t(id)] = types_[size_t(-1 - it->second)];
            else byId[size_t(id)].name = name;
        }
    }
    types_ = std::move(byId);
    typeIndex_.clear();
    for (size_t i = 0; i < types_.size(); ++i) typeIndex_[types_[i].name] = int(i);
    if (reg.is_object() && reg.contains("minecraft:item"))
        for (const json entries = reg["minecraft:item"].value("entries", json::object());
             const auto& [name, e] : entries.items()) {
            const int id = e.value("protocol_id", -1);
            if (id < 0) continue;
            if (size_t(id) >= items_.size()) items_.resize(size_t(id) + 1);
            items_[size_t(id)] = stripNs(name);
        }

    // Variant registries: entries are synced in registry order, which is sorted by identifier.
    // (The game's data with the world's data packs.)
    if (const AssetSource& data = AssetSource::data(jar); !data.empty()) {
        for (const char* r : {"cow_variant", "pig_variant", "chicken_variant", "wolf_variant", "cat_variant",
                              "frog_variant", "zombie_nautilus_variant", "painting_variant"}) {
            const std::string prefix = std::string("data/minecraft/") + r + "/";
            std::vector<Variant>& list = variants_[r];
            for (const std::string& file : data.list(prefix, ".json")) {
                std::vector<u8> bytes;
                if (!data.read(file, bytes)) continue;
                const json j = json::parse(bytes.begin(), bytes.end(), nullptr, false);
                if (!j.is_object()) continue;
                Variant v;
                v.name = file.substr(prefix.size(), file.size() - prefix.size() - 5);
                auto tex = [&](const json& o, const char* key) {
                    return o.is_object() && o.contains(key) && o[key].is_string() ? texture(stripNs(o[key].get<std::string>()))
                                                                                    : -1;
                };
                v.tex = tex(j, "asset_id");
                v.babyTex = tex(j, "baby_asset_id");
                v.model = j.value("model", "normal");
                if (j.contains("assets")) {
                    v.wild = tex(j["assets"], "wild"), v.tame = tex(j["assets"], "tame"), v.angry = tex(j["assets"], "angry");
                    v.tex = v.wild;
                }
                if (j.contains("baby_assets")) {
                    v.babyWild = tex(j["baby_assets"], "wild"), v.babyTame = tex(j["baby_assets"], "tame");
                    v.babyAngry = tex(j["baby_assets"], "angry");
                    v.babyTex = v.babyWild;
                }
                list.push_back(std::move(v));
            }
            std::sort(list.begin(), list.end(), [](const Variant& a, const Variant& b) { return a.name < b.name; });
        }
    }
    LOG("entities %6.1f ms  %zu model layers, %zu quads, %zu textures in a %ux%u atlas, %zu entity types, %zu animations",
        t.ms(), models_.size(), quads_.size(), rects_.size(), atlasW_, atlasH_, types_.size(), animations_.size());
    return true;
}

int EntityAssets::model(std::string_view layer) const {
    auto it = modelIndex_.find(layer.find(':') == std::string_view::npos ? "minecraft:" + std::string(layer)
                                                                          : std::string(layer));
    return it == modelIndex_.end() ? -1 : it->second;
}

int EntityAssets::texture(std::string_view path) const {
    auto it = texIndex_.find(std::string(path));
    return it == texIndex_.end() ? -1 : it->second;
}

int EntityAssets::itemTexture(std::string_view item) const { return texture("item/" + std::string(item)); }

int EntityAssets::villagerHat(std::string_view path) const {
    auto it = villagerHats_.find(std::string(path));
    return it == villagerHats_.end() ? 0 : it->second;
}

const TypeInfo* EntityAssets::type(int protocolId) const {
    return protocolId >= 0 && size_t(protocolId) < types_.size() ? &types_[size_t(protocolId)] : nullptr;
}

int EntityAssets::typeId(std::string_view name) const {
    auto it = typeIndex_.find(name.find(':') == std::string_view::npos ? "minecraft:" + std::string(name) : std::string(name));
    return it == typeIndex_.end() ? -1 : it->second;
}

const KeyAnimation* EntityAssets::animation(std::string_view name) const {
    auto it = animations_.find(std::string(name));
    return it == animations_.end() ? nullptr : &it->second;
}

const std::vector<Variant>& EntityAssets::variants(std::string_view registry) const {
    static const std::vector<Variant> kNone;
    auto it = variants_.find(std::string(registry));
    return it == variants_.end() ? kNone : it->second;
}

}  // namespace ent
