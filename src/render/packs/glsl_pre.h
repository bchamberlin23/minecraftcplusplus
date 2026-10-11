#pragma once
// GLSL source preparation for shader packs, before translation: #include expansion (OptiFine rules: "/path" from the
// shaders root, otherwise relative to the including file), option values applied to #define / const lines, and a
// C-style preprocessor that resolves #if / #ifdef / #elif / #else / #endif but keeps comments (OptiFine / Iris read
// DRAWBUFFERS / RENDERTARGETS and const directives from the active text) and leaves #define / #undef in place for
// the GLSL compiler. The same expression evaluator serves shaders.properties conditions.
#include "core/common.h"

#include <map>
#include <optional>
#include <unordered_map>

namespace packs {

class PackSource;

struct SourceLine {
    std::string text;
    u16 file = 0;  // index into IncludeResult::files
    u32 line = 0;  // 1-based line in that file
};
struct Included {
    std::vector<SourceLine> lines;
    std::vector<std::string> files;  // relative paths, [0] = the root file
    std::string error;
};
// Reads `file` (shaders-relative) and expands includes recursively. Missing includes are an error.
bool expandIncludes(const PackSource& src, const std::string& file, Included& out);
// Same for in-memory text (e.g. shaders.properties, which supports #include too in Iris).
bool expandIncludesText(const PackSource& src, const std::string& name, const std::string& text, Included& out);

struct Macro {
    bool function = false;
    std::vector<std::string> params;
    std::string body;
};
using MacroTable = std::unordered_map<std::string, Macro>;

// Preprocessor value (C semantics: integers stay integers).
struct PpValue {
    bool isFloat = false;
    i64 i = 0;
    double f = 0;
    double num() const { return isFloat ? f : double(i); }
    bool truthy() const { return isFloat ? f != 0 : i != 0; }
};
// Evaluates a #if-style expression (defined(), macros, C operators); undefined identifiers are 0.
std::optional<PpValue> evalPp(std::string_view expr, const MacroTable& macros, std::string* error = nullptr);
// Object-like macro expansion of a token sequence (for const directive values like "SHADOW_RES").
std::string expandMacros(std::string_view text, const MacroTable& macros);

struct Preprocessed {
    std::string text;               // active lines joined with '\n' (conditional directives removed)
    std::vector<u32> lineOrigin;    // per output line: index into the input lines
    std::string error;
};
// `macros` holds the predefined macros and receives the final macro state.
bool preprocess(const std::vector<SourceLine>& in, MacroTable& macros, Preprocessed& out);
// Same for plain text (one SourceLine per line).
bool preprocessText(const std::string& text, MacroTable& macros, Preprocessed& out);

// "file:line" of an output line of preprocess().
std::string describeLine(const Included& inc, const Preprocessed& pp, u32 outputLine);

// ---- option declarations (OptiFine "shader options") -------------------------------------------------------
// #define NAME VALUE // [a b c] comment   -> value option (allowed values from the bracket list)
// #define NAME // comment / //#define NAME -> boolean option (on / off)
// const <type> NAME = VALUE; // [a b c]   -> const option (shadowMapResolution, sunPathRotation, ...)
struct OptionDecl {
    std::string name;
    std::string value;                 // default
    std::vector<std::string> allowed;  // empty for booleans
    bool boolean = false, isConst = false;
    std::string comment;               // text after the value list
};
// Scans raw (include-expanded, not preprocessed) lines for option declarations; first declaration wins.
void scanOptions(const std::vector<SourceLine>& lines, std::map<std::string, OptionDecl>& out);
// Rewrites option lines to the given values (name -> value; booleans "true" / "false").
void applyOptions(std::vector<SourceLine>& lines, const std::map<std::string, OptionDecl>& decls,
                  const std::unordered_map<std::string, std::string>& values);

// Identifiers referenced by #ifdef / #ifndef / #if defined / #elif (boolean options must be referenced).
void collectConditionNames(const std::vector<SourceLine>& lines, std::unordered_map<std::string, int>& out);

}  // namespace packs
