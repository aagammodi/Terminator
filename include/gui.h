#ifndef TERMINATOR_GUI_H
#define TERMINATOR_GUI_H

#include "common.h"
#include "shell.h"
#include "theme.h"
#include <X11/Xlib.h>
#include <X11/Xutil.h>

typedef struct GUI {
    Display *dpy;
    int screen;
    Window win;
    Pixmap back_buffer;
    GC gc;
    Atom wm_delete_window;
    
    int width;
    int height;
    
    Theme theme;
    Shell *shell;
    
    bool needs_redraw;
} GUI;

GUI *gui_create(Shell *shell, int width, int height);
void gui_destroy(GUI *gui);
void gui_run_event_loop(GUI *gui);
void gui_redraw(GUI *gui);

#endif /* TERMINATOR_GUI_H */
