#ifndef TERMINATOR_THEME_H
#define TERMINATOR_THEME_H

#include "common.h"
#include <X11/Xlib.h>

typedef enum {
    COLOR_BG = 0,
    COLOR_SURFACE_SEC,
    COLOR_SURFACE_ELEV,
    COLOR_FG_MAIN,
    COLOR_FG_SEC,
    COLOR_PRIMARY_VIOLET,
    COLOR_ELECTRIC_CYAN,
    COLOR_NEON_PINK,
    COLOR_SOFT_GREEN,
    COLOR_WARM_ORANGE,
    COLOR_ERROR_RED,
    COLOR_BORDER,
    COLOR_SELECTION,
    COLOR_CURSOR,
    COLOR_COUNT
} ThemeColorIndex;

typedef struct Theme {
    unsigned long colors[COLOR_COUNT];
    const char *hex_values[COLOR_COUNT];
    
    /* Font and metrics */
    XFontStruct *font;
    int font_width;
    int font_height;
    int font_ascent;
    int font_descent;

    /* Window dimensions and padding */
    int top_bar_height;
    int status_bar_height;
    int padding_x;
    int padding_y;
} Theme;

/* Initialize theme colors and font */
bool theme_init(Display *dpy, int screen, Theme *theme);

/* Clean up theme resources */
void theme_cleanup(Display *dpy, Theme *theme);

/* Helper to get pixel value by index */
unsigned long theme_color(const Theme *theme, ThemeColorIndex idx);

#endif /* TERMINATOR_THEME_H */
