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

    /* Scrollbar interaction state */
    bool sb_dragging;
    int sb_drag_mouse_y;
    int sb_thumb_grab_y;
    int sb_drag_start_offset;

    /* Mouse selection state */
    bool sel_active;
    bool sel_dragging;
    int sel_start_row;
    int sel_start_col;
    int sel_end_row;
    int sel_end_col;

    /* X11 Clipboard / Selection Atoms */
    Atom clipboard_atom;
    Atom utf8_string;
    Atom targets_atom;
    Atom sel_prop_atom;
    char *clipboard_text;
    char *primary_text;
} GUI;

typedef struct {
    int sb_x;
    int sb_y;
    int sb_w;
    int sb_h;
    int thumb_y;
    int thumb_h;
    int max_scroll;
    int visible_rows;
    int total_rows;
} ScrollbarGeom;

bool gui_get_scrollbar_geom(const GUI *gui, const Tab *tab, ScrollbarGeom *geom);
GUI *gui_create(Shell *shell, int width, int height);
void gui_destroy(GUI *gui);
void gui_run_event_loop(GUI *gui);
void gui_redraw(GUI *gui);

char *gui_get_selected_text(const GUI *gui, const Tab *tab);
void gui_copy_selection(GUI *gui, Tab *tab);
void gui_request_paste(GUI *gui);

#endif /* TERMINATOR_GUI_H */
