#include "render/packs/properties.h"

namespace packs {

std::string trim(std::string_view s) {
    while (!s.empty() && std::isspace(u8(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(u8(s.back()))) s.remove_suffix(1);
    return std::string(s);
}

std::vector<std::string> splitWhitespace(std::string_view s) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && std::isspace(u8(s[i]))) ++i;
        size_t j = i;
        while (j < s.size() && !std::isspace(u8(s[j]))) ++j;
        if (j > i) out.emplace_back(s.substr(i, j - i));
        i = j;
    }
    return out;
}

namespace {
std::string unescape(std::string_view s) {
    std::string r;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '\\' || i + 1 >= s.size()) {
            r += s[i];
            continue;
        }
        char c = s[++i];
        switch (c) {
            case 'n': r += '\n'; break;
            case 't': r += '\t'; break;
            case 'r': r += '\r'; break;
            case 'f': r += '\f'; break;
            case 'u':
                if (i + 4 < s.size()) {
                    unsigned cp = unsigned(std::strtoul(std::string(s.substr(i + 1, 4)).c_str(), nullptr, 16));
                    i += 4;
                    if (cp < 0x80) r += char(cp);
                    else if (cp < 0x800) r += char(0xC0 | (cp >> 6)), r += char(0x80 | (cp & 63));
                    else r += char(0xE0 | (cp >> 12)), r += char(0x80 | ((cp >> 6) & 63)), r += char(0x80 | (cp & 63));
                }
                break;
            default: r += c;
        }
    }
    return r;
}
}  // namespace

void Properties::parse(std::string_view text) {
    // Logical lines (continuations joined).
    std::vector<std::string> lines;
    std::string cur;
    bool cont = false;
    size_t i = 0;
    while (i <= text.size()) {
        size_t j = text.find('\n', i);
        if (j == std::string_view::npos) j = text.size();
        std::string_view l = text.substr(i, j - i);
        if (!l.empty() && l.back() == '\r') l.remove_suffix(1);
        if (cont) {
            while (!l.empty() && (l.front() == ' ' || l.front() == '\t')) l.remove_prefix(1);
        } else {
            std::string_view t = l;
            while (!t.empty() && (t.front() == ' ' || t.front() == '\t')) t.remove_prefix(1);
            if (t.empty() || t.front() == '#' || t.front() == '!') {
                i = j + 1;
                if (j == text.size()) break;
                continue;
            }
        }
        // Odd number of trailing backslashes: continuation.
        size_t bs = 0;
        while (bs < l.size() && l[l.size() - 1 - bs] == '\\') ++bs;
        if (bs % 2 == 1) {
            cur += l.substr(0, l.size() - 1);
            cont = true;
        } else {
            cur += l;
            lines.push_back(std::move(cur));
            cur.clear();
            cont = false;
        }
        if (j == text.size()) break;
        i = j + 1;
    }
    if (!cur.empty()) lines.push_back(std::move(cur));
    for (const std::string& line : lines) {
        std::string_view l = line;
        while (!l.empty() && std::isspace(u8(l.front()))) l.remove_prefix(1);
        size_t k = 0;
        while (k < l.size()) {
            if (l[k] == '\\') {
                k += 2;
                continue;
            }
            if (l[k] == '=' || l[k] == ':' || l[k] == ' ' || l[k] == '\t') break;
            ++k;
        }
        std::string key = unescape(l.substr(0, std::min(k, l.size())));
        std::string_view rest = k < l.size() ? l.substr(k) : std::string_view();
        while (!rest.empty() && (rest.front() == ' ' || rest.front() == '\t')) rest.remove_prefix(1);
        if (!rest.empty() && (rest.front() == '=' || rest.front() == ':')) rest.remove_prefix(1);
        while (!rest.empty() && (rest.front() == ' ' || rest.front() == '\t')) rest.remove_prefix(1);
        std::string value = trim(unescape(rest));
        if (key.empty()) continue;
        index[key] = entries.size();
        entries.emplace_back(std::move(key), std::move(value));
    }
}

const std::string* Properties::get(const std::string& key) const {
    auto it = index.find(key);
    return it == index.end() ? nullptr : &entries[it->second].second;
}

std::string Properties::value(const std::string& key, const std::string& fallback) const {
    const std::string* v = get(key);
    return v ? *v : fallback;
}

bool Properties::boolean(const std::string& key, bool fallback) const {
    const std::string* v = get(key);
    if (!v) return fallback;
    if (*v == "true" || *v == "1" || *v == "on") return true;
    if (*v == "false" || *v == "0" || *v == "off") return false;
    return fallback;
}

}  // namespace packs
