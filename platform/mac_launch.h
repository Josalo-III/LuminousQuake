/* Native Finder startup; platform-specific UI stays outside engine code. */
#ifndef LQ_MAC_LAUNCH_H
#define LQ_MAC_LAUNCH_H
int LQ_MacInstallation(int argc, char **argv, char *path, size_t size);
void LQ_MacError(const char *message);
int LQ_MacFinderLaunch(void);
/* SDL (3.4, Cocoa) marks a window as moving when macOS repositions it, as on
 * entering fullscreen, and ends that only on a left-button release; until then
 * it leaves relative mouse mode off while reporting it on, so the cursor stays
 * free and vertical look stops at the menu bar and Dock. Ends such a move when
 * no button is down. Returns 1 if it ended one. */
struct SDL_Window;
int LQ_MacSettleWindowMove(struct SDL_Window *window);
#endif
