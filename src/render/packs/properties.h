#pragma once
// Java .properties files of shader packs (shaders.properties, block.properties, lang files, the per-pack settings
// file): key = value / key: value / key value, '#' and '!' comments, backslash line continuation and escapes.
// Iris runs the C preprocessor over shaders.properties and the id maps first (option macros in #if); callers pass
// preprocessed text.
#include "core/common.h"

#include <unordered_map>

namespace packs {

struct Properties {
    std::vector<std::pair<std::string, std::string>> entries;  // in file order (later entries override earlier ones)
    std::unordered_map<std::string, size_t> index;             // key -> last entry

    void parse(std::string_view text);
    const std::string* get(const std::string& key) const;
    std::string value(const std::string& key, const std::string& fallback = "") const;
    bool boolean(const std::string& key, bool fallback) const;
};

std::string trim(std::string_view s);
std::vector<std::string> splitWhitespace(std::string_view s);

}  // namespace packs
