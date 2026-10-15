/* Scale the original 2D drawing canvas in whole-pixel powers of two.
 * The 3D view/depth buffers retain their full render resolution.
 * GPL-2.0-or-later. */
#include "quakedef.h"
#include "render_bridge.h"
#include "jobs.h"
static viddef_t saved;
static byte *canvas, *background;
static size_t allocated;
static int active, scale = 1;
static float console_current, console_lines;
static int status_lines;
extern int con_notifylines;
typedef struct {
    const byte *source, *background, *coverage;
    byte *destination;
    size_t source_stride, destination_stride;
    unsigned width, height, scale, console_height, hud_height;
    qboolean menu;
} lq_ui_rows;

static void DownsampleRows(void *opaque_context, size_t begin, size_t end)
{
    lq_ui_rows *context = opaque_context;
    for (size_t y = begin; y < end; ++y) for (unsigned x = 0; x < context->width; ++x)
        context->destination[y * context->destination_stride + x] =
            context->source[y * context->scale * context->source_stride + x * context->scale];
}

static void CompositeRows(void *opaque_context, size_t begin, size_t end)
{
    lq_ui_rows *context = opaque_context;
    for (size_t y = begin; y < end; ++y) for (unsigned x = 0; x < context->width; ++x) {
        size_t logical = y * context->width + x;
        size_t full = y * context->scale * context->destination_stride + x * context->scale;
        if (context->source[logical] == context->background[logical] && y >= context->console_height &&
            y < context->height - context->hud_height && !context->menu &&
            (!context->coverage || !context->coverage[full])) continue;
        for (unsigned row = 0; row < context->scale; ++row)
            memset(context->destination + full + row * context->destination_stride,
                context->source[logical], context->scale);
    }
}
int LQ_UIScale(void)
{
    if (active) return scale;
    int factor = 1;
    while (vid.width / (factor * 2) >= 400 && vid.height / (factor * 2) >= 300) factor *= 2;
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
    scale = LQ_UIScale();
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
    lq_ui_rows rows = {saved.buffer, NULL, NULL, canvas, (size_t)saved.rowbytes, width,
        width, height, (unsigned)scale, 0, 0, false};
    LQ_JobsRunRange(height, 16, DownsampleRows, &rows);
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
    lq_ui_rows rows = {canvas, background, LQ_RenderUICoverage(), vid.buffer, width,
        (size_t)vid.rowbytes, width, height, (unsigned)scale, console_height, hud_height,
        key_dest == key_menu};
    LQ_JobsRunRange(height, 16, CompositeRows, &rows);
    active = 0;
}
