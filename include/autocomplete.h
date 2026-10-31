#ifndef TERMINATOR_AUTOCOMPLETE_H
#define TERMINATOR_AUTOCOMPLETE_H

#include "common.h"
#include "terminal.h"

/* Attempt file-name auto-completion on tab->input at tab->cursor.
 * Returns true if input was completed or ambiguous matches were displayed. */
bool autocomplete_handle_tab(Tab *tab);

/* Handle user typing a choice number (1..N) when ambiguous matches are displayed.
 * Returns true if completed, false if invalid number. */
bool autocomplete_handle_choice(Tab *tab, int choice);

/* Cancel any pending completion choice state without modifying input. */
void autocomplete_cancel(Tab *tab);

#endif /* TERMINATOR_AUTOCOMPLETE_H */
