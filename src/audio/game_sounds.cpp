#include "audio/game_sounds.h"

#include <json/json.hpp>
#include <windows.h>

#include <fstream>

namespace audio {

namespace {

constexpr int kFormat = 1;  // bump when the dump's layout changes

// Compiled in memory by `java BlockSoundDump.java` (JEP 330); needs only the client jar + libraries.
constexpr const char* kDumpSource = R"JAVA(
import com.google.gson.*;
import java.io.*;
import java.nio.charset.StandardCharsets;
import java.util.*;
import java.util.jar.*;
import net.minecraft.SharedConstants;
import net.minecraft.core.registries.BuiltInRegistries;
import net.minecraft.server.Bootstrap;
import net.minecraft.world.level.block.Block;
import net.minecraft.world.level.block.SoundType;
import net.minecraft.world.level.block.state.BlockState;
import net.minecraft.world.level.block.state.properties.Property;

public class BlockSoundDump {
    static <T extends Comparable<T>> String value(BlockState s, Property<T> p) { return p.getName(s.getValue(p)); }

    static String key(String name, BlockState s) {
        List<String> props = new ArrayList<>();
        for (Property<?> p : s.getProperties()) props.add(p.getName() + "=" + value(s, p));
        if (props.isEmpty()) return name;
        Collections.sort(props);
        return name + "[" + String.join(",", props) + "]";
    }

    static JsonElement read(JarFile jar, String path) throws IOException {
        JarEntry e = jar.getJarEntry(path);
        if (e == null) return null;
        try (Reader r = new InputStreamReader(jar.getInputStream(e), StandardCharsets.UTF_8)) { return JsonParser.parseReader(r); }
    }

    static void resolveTag(JarFile jar, String tag, Set<String> out, Set<String> seen) throws IOException {
        if (!seen.add(tag)) return;
        String id = tag.startsWith("minecraft:") ? tag.substring(10) : tag;
        JsonElement j = read(jar, "data/minecraft/tags/block/" + id + ".json");
        if (j == null) return;
        for (JsonElement v : j.getAsJsonObject().getAsJsonArray("values")) {
            String s = v.isJsonObject() ? v.getAsJsonObject().get("id").getAsString() : v.getAsString();
            if (s.startsWith("#")) resolveTag(jar, s.substring(1), out, seen);
            else out.add(s.contains(":") ? s : "minecraft:" + s);
        }
    }

    public static void main(String[] args) throws Exception {
        SharedConstants.tryDetectVersion();
        Bootstrap.bootStrap();
        JsonObject root = new JsonObject();
        root.addProperty("format", Integer.parseInt(args[1]));
        JsonArray types = new JsonArray();
        Map<SoundType, Integer> index = new IdentityHashMap<>();
        JsonObject blocks = new JsonObject(), states = new JsonObject();
        for (Block block : BuiltInRegistries.BLOCK) {
            String name = BuiltInRegistries.BLOCK.getKey(block).toString();
            for (BlockState state : block.getStateDefinition().getPossibleStates()) {
                SoundType t = state.getSoundType();
                if (index.containsKey(t)) continue;
                index.put(t, types.size());
                JsonObject o = new JsonObject();
                o.addProperty("break", t.getBreakSound().location().toString());
                o.addProperty("step", t.getStepSound().location().toString());
                o.addProperty("place", t.getPlaceSound().location().toString());
                o.addProperty("hit", t.getHitSound().location().toString());
                o.addProperty("fall", t.getFallSound().location().toString());
                o.addProperty("volume", t.getVolume());
                o.addProperty("pitch", t.getPitch());
                types.add(o);
            }
            int def = index.get(block.defaultBlockState().getSoundType());
            blocks.addProperty(name, def);
            for (BlockState state : block.getStateDefinition().getPossibleStates()) {
                int i = index.get(state.getSoundType());
                if (i != def) states.addProperty(key(name, state), i);
            }
        }
        root.add("types", types);
        root.add("blocks", blocks);
        root.add("states", states);

        File jarFile = new File(Block.class.getProtectionDomain().getCodeSource().getLocation().toURI());
        try (JarFile jar = new JarFile(jarFile)) {
            JsonObject tags = new JsonObject();
            for (String tag : new String[] {"inside_step_sound_blocks", "combination_step_sound_blocks", "crystal_sound_blocks", "climbable"}) {
                Set<String> members = new TreeSet<>();
                resolveTag(jar, tag, members, new HashSet<>());
                JsonArray a = new JsonArray();
                for (String m : members) a.add(m);
                tags.add(tag, a);
            }
            root.add("tags", tags);
            JsonObject dims = new JsonObject(), biomes = new JsonObject();
            for (Enumeration<JarEntry> en = jar.entries(); en.hasMoreElements();) {
                String n = en.nextElement().getName();
                JsonObject target = n.startsWith("data/minecraft/dimension_type/") ? dims
                                  : n.startsWith("data/minecraft/worldgen/biome/") ? biomes : null;
                if (target == null || !n.endsWith(".json")) continue;
                JsonElement j = read(jar, n);
                JsonObject attrs = j != null && j.isJsonObject() && j.getAsJsonObject().has("attributes")
                    ? j.getAsJsonObject().getAsJsonObject("attributes") : new JsonObject();
                JsonObject audio = new JsonObject();
                for (String a : new String[] {"minecraft:audio/background_music", "minecraft:audio/ambient_sounds", "minecraft:audio/music_volume"})
                    if (attrs.has(a)) audio.add(a.substring(16), attrs.get(a));
                target.add("minecraft:" + n.substring(n.lastIndexOf('/') + 1, n.length() - 5), audio);
            }
            root.add("dimension_types", dims);
            root.add("biomes", biomes);
        }
        try (Writer w = new OutputStreamWriter(new FileOutputStream(args[0]), StandardCharsets.UTF_8)) {
            new GsonBuilder().setPrettyPrinting().disableHtmlEscaping().create().toJson(root, w);
        }
        System.exit(0);
    }
}
)JAVA";

nlohmann::json readJson(const fs::path& p) {
    std::vector<u8> bytes;
    if (!readFile(p, bytes)) return nullptr;
    return nlohmann::json::parse(bytes.begin(), bytes.end(), nullptr, false);
}

// The launcher's Java runtime for this version (same search as GameReports).
fs::path findJava(const fs::path& mc, const std::string& component) {
    std::vector<fs::path> roots = {mc / "runtime"};
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
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, workDir.c_str(), &si, &pi))
        return false;
    const bool finished = WaitForSingleObject(pi.hProcess, timeoutMs) == WAIT_OBJECT_0;
    DWORD code = 1;
    if (finished) GetExitCodeProcess(pi.hProcess, &code);
    else TerminateProcess(pi.hProcess, 1);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return finished && code == 0;
}

MusicDef parseMusic(const nlohmann::json& j, const SoundBank& bank) {
    MusicDef m;
    if (!j.is_object()) return m;
    m.event = bank.eventId(j.value("sound", ""));
    m.minDelay = j.value("min_delay", 12000);
    m.maxDelay = j.value("max_delay", 24000);
    m.replace = j.value("replace_current_music", false);
    return m;
}

AudioEnvironment parseEnvironment(const nlohmann::json& j, const SoundBank& bank) {
    AudioEnvironment env;
    if (!j.is_object()) return env;
    if (j.contains("background_music")) {
        const auto& m = j["background_music"];
        env.hasMusic = true;
        if (m.contains("default")) env.music.normal = parseMusic(m["default"], bank);
        if (m.contains("creative")) env.music.creative = parseMusic(m["creative"], bank);
        if (m.contains("underwater")) env.music.underwater = parseMusic(m["underwater"], bank);
    }
    if (j.contains("ambient_sounds")) {
        const auto& a = j["ambient_sounds"];
        env.hasAmbient = true;
        if (a.contains("loop") && a["loop"].is_string()) env.ambient.loop = bank.eventId(a["loop"].get<std::string>());
        if (a.contains("additions")) {
            auto add = [&](const nlohmann::json& x) {
                if (x.is_object())
                    env.ambient.additions.push_back({bank.eventId(x.value("sound", "")), x.value("tick_chance", 0.0)});
            };
            if (a["additions"].is_array())
                for (const auto& x : a["additions"]) add(x);
            else
                add(a["additions"]);
        }
        if (a.contains("mood") && a["mood"].is_object()) {
            const auto& m = a["mood"];
            env.ambient.hasMood = true;
            env.ambient.moodEvent = bank.eventId(m.value("sound", ""));
            env.ambient.moodTickDelay = m.value("tick_delay", 6000);
            env.ambient.moodExtent = m.value("block_search_extent", 8);
            env.ambient.moodOffset = m.value("offset", 2.0);
        }
    }
    if (j.contains("music_volume") && j["music_volume"].is_number()) {
        env.hasMusicVolume = true;
        env.musicVolume = j["music_volume"].get<float>();
    }
    return env;
}

}  // namespace

bool GameSounds::generate(const fs::path& mc, const std::string& version, const fs::path& out) {
    const fs::path versionDir = mc / "versions" / version;
    const nlohmann::json v = readJson(versionDir / (version + ".json"));
    if (!v.is_object()) return false;
    const std::string component = v.contains("javaVersion") ? v["javaVersion"].value("component", "") : "";
    const fs::path java = findJava(mc, component);
    if (java.empty()) {
        LOG("audio: the game's Java runtime (%s) was not found; block sounds unavailable", component.c_str());
        return false;
    }
    std::wstring cp = (versionDir / (version + ".jar")).wstring();
    for (const auto& lib : v.value("libraries", nlohmann::json::array())) {
        bool allowed = true;
        if (lib.contains("rules")) {
            allowed = false;
            for (const auto& rule : lib["rules"]) {
                const bool matches = !rule.contains("os") || rule["os"].value("name", "") == "windows";
                if (matches) allowed = rule.value("action", "") == "allow";
            }
        }
        if (!allowed || !lib.contains("downloads") || !lib["downloads"].contains("artifact")) continue;
        cp += L";" + (mc / "libraries" / lib["downloads"]["artifact"].value("path", "")).wstring();
    }
    std::error_code ec;
    fs::create_directories(out.parent_path(), ec);
    const fs::path source = out.parent_path() / "BlockSoundDump.java";
    {
        std::ofstream f(source, std::ios::binary);
        f << kDumpSource;
        if (!f) return false;
    }
    const fs::path tmp = out.parent_path() / "block_sounds.tmp";
    LOG("audio: extracting block sound types with the game's code (one time, ~4 s)...");
    const std::wstring cmd = L"\"" + java.wstring() + L"\" -cp \"" + cp + L"\" \"" + source.wstring() + L"\" \"" +
                             tmp.wstring() + L"\" " + std::to_wstring(kFormat);
    if (!runHidden(cmd, out.parent_path(), 180000)) {
        LOG("audio: block sound extraction failed");
        return false;
    }
    fs::rename(tmp, out, ec);
    return !ec;
}

bool GameSounds::load(const fs::path& mc, const std::string& version, const fs::path& cacheDir, const SoundBank& bank) {
    const fs::path file = cacheDir / version / "block_sounds.json";
    nlohmann::json j = readJson(file);
    if (!j.is_object() || j.value("format", 0) != kFormat) {
        if (!generate(mc, version, file)) return false;
        j = readJson(file);
        if (!j.is_object()) return false;
    }
    for (const auto& t : j.value("types", nlohmann::json::array())) {
        BlockSoundType s;
        s.breakSound = bank.eventId(t.value("break", ""));
        s.step = bank.eventId(t.value("step", ""));
        s.place = bank.eventId(t.value("place", ""));
        s.hit = bank.eventId(t.value("hit", ""));
        s.fall = bank.eventId(t.value("fall", ""));
        s.volume = t.value("volume", 1.0f);
        s.pitch = t.value("pitch", 1.0f);
        types_.push_back(s);
    }
    if (types_.empty()) return false;
    for (const auto& b : j["blocks"].items()) blocks_.emplace(b.key(), b.value().get<int>());
    for (const auto& b : j["states"].items()) states_.emplace(b.key(), b.value().get<int>());
    const std::pair<const char*, u8> tagNames[] = {{"inside_step_sound_blocks", TagInsideStep},
                                                   {"combination_step_sound_blocks", TagCombinationStep},
                                                   {"crystal_sound_blocks", TagCrystal},
                                                   {"climbable", TagClimbable}};
    for (const auto& [name, bit] : tagNames)
        if (j["tags"].contains(name))
            for (const auto& b : j["tags"][name]) tags_[b.get<std::string>()] |= bit;
    for (const auto& d : j["dimension_types"].items()) dimensions_.emplace(d.key(), parseEnvironment(d.value(), bank));
    for (const auto& b : j["biomes"].items()) biomes_.emplace(b.key(), parseEnvironment(b.value(), bank));
    return true;
}

int GameSounds::typeFor(std::string_view key) const {
    if (!states_.empty())
        if (auto it = states_.find(key); it != states_.end()) return it->second;
    auto it = blocks_.find(key.substr(0, key.find('[')));
    return it == blocks_.end() ? 0 : it->second;
}

u8 GameSounds::tags(std::string_view blockName) const {
    auto it = tags_.find(blockName.substr(0, blockName.find('[')));
    return it == tags_.end() ? 0 : it->second;
}

const AudioEnvironment* GameSounds::dimension(std::string_view name) const {
    auto it = dimensions_.find(name);
    return it == dimensions_.end() ? nullptr : &it->second;
}

const AudioEnvironment* GameSounds::biome(std::string_view name) const {
    auto it = biomes_.find(name);
    return it == biomes_.end() ? nullptr : &it->second;
}

}  // namespace audio
