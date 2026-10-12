#pragma once
// Bidirectional text like the game's FormattedBidiReorder (ICU Bidi + ArabicShaping(LETTERS_SHAPE)) without ICU:
// Arabic letters take their contextual presentation forms (lam-alef ligatures included), then the Unicode
// Bidirectional Algorithm (UAX #9: paragraph level, weak and neutral types, implicit levels, reordering, mirrored
// glyphs in right-to-left runs; explicit embeddings and isolates are treated as neutrals) puts the code points in
// visual order. Each code point keeps an attached value (the caller's style index) through shaping and reordering.
#include "core/common.h"

#include <vector>

namespace ui::bidi {

struct Char {
    u32 cp;
    u32 tag;  // caller data (kept with the code point)
};

// true if `cp` can change the visual order or shape (Hebrew, Arabic, other right-to-left scripts, marks).
bool needsBidi(u32 cp);
// ArabicShaping(LETTERS_SHAPE): contextual forms in logical order (lam + alef become one ligature code point).
void shapeArabic(std::vector<Char>& text);
// Logical -> visual order of one line. defaultRtl: the paragraph direction when no strong character decides it
// (ICU's DEFAULT_RTL for right-to-left languages, DEFAULT_LTR otherwise). Shapes first when needed.
void reorder(std::vector<Char>& text, bool defaultRtl);

}  // namespace ui::bidi
