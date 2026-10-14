/* Headless/viewer hooks do not need a gameplay compositor. GPL-2.0-or-later. */
#include "render_bridge.h"
void LQ_RenderBegin(void) {}
void LQ_RenderAfterCommands(void) {}
int LQ_RenderBridgeActive(void) { return 0; }
void LQ_RenderMapChanged(void) {}
void LQ_RenderWorldDone(void) {}
void LQ_RenderSceneDone(void) {}
void LQ_RenderSurface(int x, int y, int count, int preserve)
{ (void)x; (void)y; (void)count; (void)preserve; }
