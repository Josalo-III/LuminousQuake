/* Temporary CPU actor/UI bridge for the playable GPU world. GPL-2.0-or-later. */
#ifndef LQ_RENDER_BRIDGE_H
#define LQ_RENDER_BRIDGE_H
void LQ_RenderBegin(void);
void LQ_RenderAfterCommands(void);
int LQ_RenderBridgeActive(void);
void LQ_RenderMapChanged(void);
void LQ_RenderWorldDone(void);
void LQ_RenderSceneDone(void);
void LQ_RenderSurface(int x, int y, int count, int preserve);
void LQ_UIBegin(void);
void LQ_UIEnd(void);
int LQ_UIScale(void);
int LQ_UIActive(void);
void LQ_UIShutdown(void);
#endif
