#include "theme.h"
#include "util.h"
#include <stdio.h>
#include <string.h>

static const char *neon_aurora_hex[COLOR_COUNT] = {
    [COLOR_BG]             = "#1A1930",
    [COLOR_SURFACE_SEC]     = "#25233D",
    [COLOR_SURFACE_ELEV]    = "#302C4D",
    [COLOR_FG_MAIN]         = "#F0EFFA",
    [COLOR_FG_SEC]          = "#A6ADC8",
    [COLOR_PRIMARY_VIOLET]  = "#7C3AED",
    [COLOR_ELECTRIC_CYAN]   = "#22D3EE",
    [COLOR_NEON_PINK]       = "#F472B6",
    [COLOR_SOFT_GREEN]      = "#A6E3A1",
    [COLOR_WARM_ORANGE]     = "#FAB387",
    [COLOR_ERROR_RED]       = "#F38BA8",
    [COLOR_BORDER]          = "#484064",
    [COLOR_SELECTION]       = "#454064",
    [COLOR_CURSOR]          = "#C4B5FD"
};

static unsigned long allocate_hex_color(Display *dpy, int screen, const char *hex) {
    if (!dpy || !hex || hex[0] != '#' || strlen(hex) < 7) {
        return 0;
    }

    unsigned int r = 0, g = 0, b = 0;
    if (sscanf(hex + 1, "%02x%02x%02x", &r, &g, &b) != 3) {
        return 0;
    }

    Colormap cmap = DefaultColormap(dpy, screen);
    XColor xcolor;
    xcolor.red = (unsigned short)(r * 257);
    xcolor.green = (unsigned short)(g * 257);
    xcolor.blue = (unsigned short)(b * 257);
    xcolor.flags = DoRed | DoGreen | DoBlue;

    if (XAllocColor(dpy, cmap, &xcolor)) {
        return xcolor.pixel;
    }

    /* Fallback: Direct 24-bit TrueColor packed RGB */
    return (unsigned long)((r << 16) | (g << 8) | b);
}

static const char *preferred_fonts[] = {
    "-*-jetbrains mono-medium-r-*-*-14-*-*-*-m-*-*-*",
    "-*-dejavu sans mono-medium-r-*-*-14-*-*-*-m-*-*-*",
    "-*-liberation mono-medium-r-*-*-14-*-*-*-m-*-*-*",
    "-*-ubuntu mono-medium-r-*-*-14-*-*-*-m-*-*-*",
    "-misc-fixed-medium-r-normal--14-130-75-75-c-70-iso8859-1",
    "9x15",
    "fixed"
};

bool theme_init(Display *dpy, int screen, Theme *theme) {
    if (!theme) return false;

    for (int i = 0; i < COLOR_COUNT; i++) {
        theme->hex_values[i] = neon_aurora_hex[i];
        if (dpy) {
            theme->colors[i] = allocate_hex_color(dpy, screen, neon_aurora_hex[i]);
        } else {
            theme->colors[i] = 0;
        }
    }

    theme->top_bar_height = 36;
    theme->status_bar_height = 24;
    theme->padding_x = 14;
    theme->padding_y = 10;

    if (!dpy) {
        theme->font = NULL;
        theme->font_width = 8;
        theme->font_height = 16;
        theme->font_ascent = 12;
        theme->font_descent = 4;
        return true;
    }

    theme->font = NULL;
    for (size_t i = 0; i < ARRAY_SIZE(preferred_fonts); i++) {
        theme->font = XLoadQueryFont(dpy, preferred_fonts[i]);
        if (theme->font) {
            break;
        }
    }

    if (!theme->font) {
        fprintf(stderr, "terminator: warning: failed to load preferred fonts, attempting 'fixed'\n");
        theme->font = XLoadQueryFont(dpy, "fixed");
    }

    if (theme->font) {
        theme->font_ascent = theme->font->ascent;
        theme->font_descent = theme->font->descent;
        theme->font_height = theme->font_ascent + theme->font_descent;
        theme->font_width = XTextWidth(theme->font, "W", 1);
        if (theme->font_width <= 0) {
            theme->font_width = theme->font->max_bounds.width;
        }
        if (theme->font_width <= 0) {
            theme->font_width = 8;
        }
    } else {
        fprintf(stderr, "terminator: error: cannot load any X11 font\n");
        theme->font_width = 8;
        theme->font_height = 16;
        theme->font_ascent = 12;
        theme->font_descent = 4;
    }

    return true;
}

void theme_cleanup(Display *dpy, Theme *theme) {
    if (!theme) return;
    if (dpy && theme->font) {
        XFreeFont(dpy, theme->font);
        theme->font = NULL;
    }
}

unsigned long theme_color(const Theme *theme, ThemeColorIndex idx) {
    if (!theme || idx < 0 || idx >= COLOR_COUNT) return 0;
    return theme->colors[idx];
}
