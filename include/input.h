#ifndef TERMINATOR_INPUT_H
#define TERMINATOR_INPUT_H

#include "common.h"
#include "shell.h"
#include <X11/Xlib.h>

/* Handle X11 KeyPress event */
void input_handle_key_event(XKeyEvent *event, Shell *shell, bool *needs_redraw);

/* Handle X11 ButtonPress event (e.g. mouse clicks on tabs or mouse wheel scrolling) */
void input_handle_button_event(XButtonEvent *event, Shell *shell, int win_width, int win_height, bool *needs_redraw);

#endif /* TERMINATOR_INPUT_H */
