#pragma once
// Signs and books (signs_books.cpp): the sign editor (open_sign_editor -> sign_update), book and quill editing and
// signing (edit_book), written book reading (open_book) and the lectern's book (menu "lectern").
#include "ui/modules.h"

namespace ui {

// A use of the item in `hand` (0 main, 1 off hand): a book and quill opens BookEditScreen like WritableBookItem.use
// on the client. true when it did.
bool openWritableBook(UiState& s, int hand);

}  // namespace ui
