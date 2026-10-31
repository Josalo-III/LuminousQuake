/* Native Finder startup; platform-specific UI stays outside engine code. */
#ifndef LQ_MAC_LAUNCH_H
#define LQ_MAC_LAUNCH_H
int LQ_MacInstallation(int argc, char **argv, char *path, size_t size);
void LQ_MacError(const char *message);
int LQ_MacFinderLaunch(void);
/* 1 when the app is active and this SDL window is key, 0 when not, -1 unknown. */
struct SDL_Window;
int LQ_MacWindowKey(struct SDL_Window *window);
#endif
