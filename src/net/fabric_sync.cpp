#include "net/fabric_sync.h"

#include "net/protocol.h"

#include <json/json.hpp>

#include <algorithm>

namespace net {

namespace {

// BlockState.toString(): "Block{ns:name}[k=v,...]" -> "ns:name[k=v,...]" with the properties sorted (our state keys).
std::string stateKey(std::string_view s) {
    if (!s.starts_with("Block{")) return std::string(s);
    const size_t close = s.find('}');
    if (close == std::string_view::npos) return {};
    std::string key(s.substr(6, close - 6));
    std::string_view rest = s.substr(close + 1);
    if (rest.size() < 2 || rest.front() != '[' || rest.back() != ']') return key;
    rest = rest.substr(1, rest.size() - 2);
    std::vector<std::string_view> props;
    while (!rest.empty()) {
        const size_t comma = rest.find(',');
        props.push_back(rest.substr(0, comma));
        if (comma == std::string_view::npos) break;
        rest.remove_prefix(comma + 1);
    }
    std::sort(props.begin(), props.end());
    key += '[';
    for (size_t i = 0; i < props.size(); ++i) key += (i ? "," : "") + std::string(props[i]);
    return key + ']';
}

std::string withNamespace(std::string_view ns) { return ns.empty() ? "minecraft" : std::string(ns); }

}  // namespace

bool FabricSync::loadDump(const fs::path& file) {
    std::vector<u8> bytes;
    if (!readFile(file, bytes)) return false;
    const nlohmann::json j = nlohmann::json::parse(bytes.begin(), bytes.end(), nullptr, false);
    if (!j.is_object() || !j.contains("blocks") || !j.contains("table")) return false;
    blocks_.clear(), table_.clear();
    for (const auto& b : j["blocks"]) {
        Block block;
        block.id = b.value("id", "");
        for (const auto& s : b.value("states", nlohmann::json::array()))
            if (s.is_string()) block.states.push_back(stateKey(s.get<std::string>()));
        blocks_.push_back(std::move(block));
    }
    for (const auto& s : j["table"]) table_.push_back(s.is_string() ? stateKey(s.get<std::string>()) : std::string());
    return !table_.empty();
}

// RegistrySyncPayload.read: registries grouped by namespace, ids grouped by namespace in bulks of consecutive raw ids.
bool FabricSync::parseSync(Reader& r) {
    const i32 regNamespaces = r.varInt();
    for (i32 i = 0; i < regNamespaces && r.ok; ++i) {
        const std::string regNs = withNamespace(r.str());
        const i32 regs = r.varInt();
        for (i32 k = 0; k < regs && r.ok; ++k) {
            const std::string registry = regNs + ":" + std::string(r.str());
            r.u8v();  // attributes (optional flag)
            auto& ids = synced_[registry];
            const i32 idNamespaces = r.varInt();
            int lastBulkLast = 0;
            for (i32 n = 0; n < idNamespaces && r.ok; ++n) {
                const std::string idNs = withNamespace(r.str());
                const i32 bulks = r.varInt();
                for (i32 b = 0; b < bulks && r.ok; ++b) {
                    const i32 startDiff = r.varInt(), size = r.varInt();
                    int raw = lastBulkLast + startDiff - 1;
                    for (i32 m = 0; m < size && r.ok; ++m) ids[idNs + ":" + std::string(r.str())] = ++raw;
                    lastBulkLast = raw;
                }
            }
        }
    }
    return r.ok;
}

std::vector<std::string> FabricSync::stateTable() const {
    // Synced block ids: blocks in the server's current raw id order, each with its states (the order Fabric rebuilds
    // the state ids in). Without them the dumped table is the server's.
    const auto it = synced_.find("minecraft:block");
    if (it == synced_.end() || blocks_.empty()) return table_;
    std::vector<std::pair<int, const Block*>> order;
    for (const Block& b : blocks_) {
        const auto raw = it->second.find(b.id);
        if (raw == it->second.end()) return table_;  // not the same block set: trust the dump
        order.push_back({raw->second, &b});
    }
    std::sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<std::string> out;
    out.reserve(table_.size());
    for (const auto& [raw, b] : order) out.insert(out.end(), b->states.begin(), b->states.end());
    return out;
}

size_t FabricSync::moddedBlocks() const {
    size_t n = 0;
    if (const auto it = synced_.find("minecraft:block"); it != synced_.end()) {
        for (const auto& [id, raw] : it->second) n += !id.starts_with("minecraft:");
        return n;
    }
    for (const Block& b : blocks_) n += !b.id.starts_with("minecraft:");
    return n;
}

}  // namespace net
