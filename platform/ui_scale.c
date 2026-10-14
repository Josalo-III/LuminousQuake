/* Scale the original 2D drawing canvas in whole-pixel powers of two.
 * The 3D view/depth buffers retain their full render resolution.
 * GPL-2.0-or-later. */
#include "quakedef.h"
#include "render_bridge.h"
static viddef_t saved;
static byte *canvas, *background;
static size_t allocated;
static int active, scale = 1;
static float console_current, console_lines;
static int status_lines;
extern int con_notifylines;
int LQ_UIScale(void)
{
    if (active) return scale;
    int factor = 1;
    while (vid.width / (factor * 2) >= 320 && vid.height / (factor * 2) >= 200) factor *= 2;
    return factor;
}
int LQ_UIActive(void) { return active; }
void LQ_UIShutdown(void)
{
    free(canvas); free(background); canvas = background = NULL;
    allocated = 0; active = 0; scale = 1;
}
void LQ_UIBegin(void)
{
    if (active) return;
    scale = 1;
    while (vid.width / (scale * 2) >= 320 && vid.height / (scale * 2) >= 200) scale *= 2;
    if (scale == 1) return;
    saved = vid;
    unsigned width = vid.width / scale, height = vid.height / scale;
    size_t count = (size_t)width * height;
    if (allocated != count) {
        free(canvas); free(background);
        canvas = malloc(count); background = malloc(count);
        if (!canvas || !background) Sys_Error("Cannot allocate scaled UI canvas");
        allocated = count;
    }
    for (unsigned y = 0; y < height; ++y) for (unsigned x = 0; x < width; ++x)
        canvas[(size_t)y * width + x] = saved.buffer[(size_t)(y * scale) * saved.rowbytes + x * scale];
    memcpy(background, canvas, count);
    vid.buffer = vid.conbuffer = canvas;
    vid.width = vid.conwidth = vid.rowbytes = vid.conrowbytes = width;
    vid.height = vid.conheight = height;
    console_current = scr_con_current; console_lines = scr_conlines;
    scr_con_current /= scale; scr_conlines /= scale;
    status_lines = sb_lines; sb_lines /= scale;
    con_notifylines /= scale;
    active = 1;
    Sbar_Changed();
}
void LQ_UIEnd(void)
{
    if (!active) return;
    unsigned width = vid.width, height = vid.height;
    unsigned console_height = (unsigned)scr_con_current;
    unsigned hud_height = (unsigned)sb_lines;
    vid = saved;
    scr_con_current = console_current; scr_conlines = console_lines;
    sb_lines = status_lines;
    con_notifylines *= scale;
    /* Preserve full-resolution world pixels wherever the UI did not draw. */
    for (unsigned y = 0; y < height; ++y) for (unsigned x = 0; x < width; ++x) {
        size_t i = (size_t)y * width + x;
        if (canvas[i] == background[i] && y >= console_height &&
            y < height - hud_height && key_dest != key_menu) continue;
        for (int row = 0; row < scale; ++row)
            memset(vid.buffer + (size_t)(y * scale + row) * vid.rowbytes + x * scale, canvas[i], scale);
    }
    active = 0;
}
