#ifndef TERMINATOR_INPUT_H
#define TERMINATOR_INPUT_H

#include "common.h"
#include "shell.h"
#include <X11/Xlib.h>

struct GUI;

/* Handle X11 KeyPress event */
void input_handle_key_event(XKeyEvent *event, Shell *shell, bool *needs_redraw);

/* Handle X11 ButtonPress, ButtonRelease, and Motion events */
void input_handle_button_press(XButtonEvent *event, struct GUI *gui);
void input_handle_button_release(XButtonEvent *event, struct GUI *gui);
void input_handle_motion(XMotionEvent *event, struct GUI *gui);

#endif /* TERMINATOR_INPUT_H */
