/* Classic GPU frame adaptation and scaled UI coverage. GPL-2.0-or-later. */
#ifndef LQ_RENDER_BRIDGE_H
#define LQ_RENDER_BRIDGE_H
void LQ_RenderBegin(void);
void LQ_RenderAfterCommands(void);
int LQ_RenderBridgeActive(void);
void LQ_RenderMapChanged(void);
int LQ_RenderGPUView(void);
int LQ_RenderGPUParticle(const float origin[3], int color, int emissive, unsigned int seed);
void LQ_RenderSpark(const float origin[3], int color);
void LQ_RenderUIRegion(int x, int y, int width, int height, const unsigned char *pixels, int stride, int transparent);
int LQ_RenderUIPainted(int x, int y);
const unsigned char *LQ_RenderUICoverage(void);
void LQ_UIBegin(void);
void LQ_UIEnd(void);
int LQ_UIScale(void);
int LQ_UIActive(void);
void LQ_UIShutdown(void);
#endif
