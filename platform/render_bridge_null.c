/* Headless/viewer hooks do not need a gameplay compositor. GPL-2.0-or-later. */
#include "render_bridge.h"
int LQ_RenderGPUView(void) { return 0; }
int LQ_RenderGPUParticle(const float origin[3], int color) { (void)origin; (void)color; return 0; }
void LQ_RenderUIRegion(int x, int y, int width, int height, const unsigned char *pixels, int stride, int transparent)
{ (void)x; (void)y; (void)width; (void)height; (void)pixels; (void)stride; (void)transparent; }
int LQ_RenderUIPainted(int x, int y) { (void)x; (void)y; return 0; }
void LQ_RenderBegin(void) {}
void LQ_RenderAfterCommands(void) {}
int LQ_RenderBridgeActive(void) { return 0; }
void LQ_RenderMapChanged(void) {}
void LQ_UIBegin(void) {}
void LQ_UIEnd(void) {}
int LQ_UIScale(void) { return 1; }
int LQ_UIActive(void) { return 0; }
void LQ_UIShutdown(void) {}
