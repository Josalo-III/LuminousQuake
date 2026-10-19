/*
===========================================================================
Copyright (C) 1999-2005 Id Software, Inc.

This file is part of Quake III Arena source code.

Quake III Arena source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

Quake III Arena source code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Quake III Arena source code; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/

/* SDL key translation/capture adapted for NetQuake. See SOURCE_PROVENANCE.md. */
#include <stdarg.h>
#include "quakedef.h"
#include "sdl_backend.h"
#ifdef __APPLE__
#include "mac_launch.h"
#endif

static qboolean initialized, focused, quit_requested, demo_active, mouse_active, suppress_text;
static int pressed[SDL_SCANCODE_COUNT], held[256];
static float mouse_x, mouse_y, previous_x, previous_y;
static cvar_t in_mouse = {"in_mouse", "1", true};
static cvar_t in_nograb = {"in_nograb", "0", true};
static cvar_t m_filter = {"m_filter", "0", true};
extern kbutton_t in_strafe, in_mlook;

qboolean LQ_HasFocus(void) { return focused; }
qboolean LQ_QuitRequested(void) { return quit_requested; }
void LQ_ResetQuit(void) { quit_requested = false; }
void LQ_SetDemo(qboolean active) { demo_active = active; }

static int TranslateKey(const SDL_KeyboardEvent *event)
{
    SDL_Keycode symbol = event->key;
    /* Preserve number-row bindings across keyboard layouts, as ioquake3 does. */
    if (event->scancode >= SDL_SCANCODE_1 && event->scancode <= SDL_SCANCODE_0)
        return event->scancode == SDL_SCANCODE_0 ? '0' : '1' + event->scancode - SDL_SCANCODE_1;
    /* SDL3 keycodes use uppercase Latin letters; Quake bindings use lowercase. */
    if (symbol >= SDLK_A && symbol <= SDLK_Z) return 'a' + symbol - SDLK_A;
    if (symbol >= SDLK_SPACE && symbol < SDLK_DELETE) return (int)symbol;
    switch (symbol) {
    case SDLK_RETURN: case SDLK_KP_ENTER: return K_ENTER;
    case SDLK_ESCAPE: return K_ESCAPE;
    case SDLK_TAB: return K_TAB;
    case SDLK_BACKSPACE: return K_BACKSPACE;
    case SDLK_UP: return K_UPARROW; case SDLK_DOWN: return K_DOWNARROW;
    case SDLK_LEFT: return K_LEFTARROW; case SDLK_RIGHT: return K_RIGHTARROW;
    case SDLK_LALT: case SDLK_RALT: return K_ALT;
    case SDLK_LCTRL: case SDLK_RCTRL: return K_CTRL;
    case SDLK_LSHIFT: case SDLK_RSHIFT: return K_SHIFT;
    case SDLK_F1: return K_F1; case SDLK_F2: return K_F2; case SDLK_F3: return K_F3;
    case SDLK_F4: return K_F4; case SDLK_F5: return K_F5; case SDLK_F6: return K_F6;
    case SDLK_F7: return K_F7; case SDLK_F8: return K_F8; case SDLK_F9: return K_F9;
    case SDLK_F10: return K_F10; case SDLK_F11: return K_F11; case SDLK_F12: return K_F12;
    case SDLK_INSERT: return K_INS; case SDLK_DELETE: return K_DEL;
    case SDLK_PAGEDOWN: return K_PGDN; case SDLK_PAGEUP: return K_PGUP;
    case SDLK_HOME: return K_HOME; case SDLK_END: return K_END;
    case SDLK_PAUSE: return K_PAUSE;
    default: return 0;
    }
}
void IN_ClearStates(void)
{
    int key;
    for (key = 0; key < 256; ++key)
        if (held[key]) Key_Event(key, false);
    memset(held, 0, sizeof(held)); memset(pressed, 0, sizeof(pressed));
    mouse_x = mouse_y = previous_x = previous_y = 0;
    Key_ClearStates();
}
/* Diagnostics for vertical look failing until a click: capture changes,
 * window events, and for 30 s after capture one sample a second of the
 * cursor's screen position (it should not move while captured). Written to
 * ~/Library/Logs/LuminousQuake/input.log, replaced each launch. */
static Uint64 capture_log_until, capture_log_next;
static void InputLog(const char *format, ...)
{
    static FILE *file; static int tried;
    if (!tried) {
        const char *home = getenv("HOME"); char path[1024];
        tried = 1;
        if (home && snprintf(path, sizeof(path), "%s/Library/Logs/LuminousQuake/input.log", home) < (int)sizeof(path))
            file = fopen(path, "w");
    }
    if (!file) return;
    va_list args; va_start(args, format);
    fprintf(file, "%8.3f ", SDL_GetTicks() / 1000.0); vfprintf(file, format, args); fputc('\n', file);
    va_end(args); fflush(file);
}
static void CaptureSample(void)
{
    SDL_Window *window = LQ_SDLWindow();
    Uint64 now = SDL_GetTicks();
    if (!mouse_active || now > capture_log_until || now < capture_log_next) return;
    capture_log_next = now + 1000;
    float x, y; SDL_GetGlobalMouseState(&x, &y);
    InputLog("sample: cursor %.0f,%.0f relative %d keyboard-focus %d mouse-focus %d pitch %.1f",
        x, y, window && SDL_GetWindowRelativeMouseMode(window), SDL_GetKeyboardFocus() == window,
        SDL_GetMouseFocus() == window, cl.viewangles[PITCH]);
}
void LQ_UpdateCapture(void)
{
    SDL_Window *window = LQ_SDLWindow();
    if (window) {
        qboolean text_wanted = initialized && focused &&
            (key_dest == key_console || key_dest == key_message || key_dest == key_menu);
        if (text_wanted && !SDL_TextInputActive(window)) SDL_StartTextInput(window);
        else if (!text_wanted && SDL_TextInputActive(window)) SDL_StopTextInput(window);
    }
    qboolean wanted = initialized && window && focused && in_mouse.value && !in_nograb.value &&
        !COM_CheckParm("-nomouse") && key_dest == key_game && !cl.paused &&
        (demo_active || (cls.state == ca_connected && !con_forcedup));
#ifdef __APPLE__
    if (wanted && LQ_MacSettleWindowMove(window))
        Con_Printf("Mouse capture: a window move had no click to end it; relative mode applied.\n"),
        InputLog("window move without a click ended; relative mode applied");
#endif
    if (wanted == mouse_active) return;
    if (wanted) {
        if (!SDL_SetWindowRelativeMouseMode(window, true)) {
            /* Dummy/headless video drivers need not implement capture. */
            return;
        }
        SDL_SetWindowMouseGrab(window, true);
    } else {
        if (window) {
            SDL_SetWindowRelativeMouseMode(window, false);
            SDL_SetWindowMouseGrab(window, false);
        }
    }
    mouse_active = wanted;
    InputLog("capture %s: relative %d keyboard-focus %d mouse-focus %d fullscreen %d", wanted ? "on" : "off",
        window && SDL_GetWindowRelativeMouseMode(window), SDL_GetKeyboardFocus() == window,
        SDL_GetMouseFocus() == window, window && (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) != 0);
    if (wanted) capture_log_until = SDL_GetTicks() + 30000, capture_log_next = 0;
    mouse_x = mouse_y = previous_x = previous_y = 0;
    SDL_GetRelativeMouseState(NULL, NULL);
}
/* Drop relative mode so the next LQ_UpdateCapture applies it afresh. */
static void ReleaseCapture(void)
{
    InputLog("release capture after a window event");
    SDL_Window *window = LQ_SDLWindow();
    if (!mouse_active || !window) return;
    SDL_SetWindowRelativeMouseMode(window, false);
    SDL_SetWindowMouseGrab(window, false);
    mouse_active = false;
}
void LQ_SetFocus(qboolean value)
{
    focused = value;
    if (!focused) IN_ClearStates();
    LQ_AudioFocus(focused);
    LQ_UpdateCapture();
}
void IN_Init(void)
{
    if (initialized) return;
    Cvar_RegisterVariable(&in_mouse); Cvar_RegisterVariable(&in_nograb); Cvar_RegisterVariable(&m_filter);
    /* Minimal defaults also work with packs that omit default.cfg. Later
     * quake.rc/config.cfg commands retain their normal override precedence. */
    Key_SetBinding('w', "+forward"); Key_SetBinding('s', "+back");
    Key_SetBinding('a', "+moveleft"); Key_SetBinding('d', "+moveright");
    Key_SetBinding(K_UPARROW, "+forward"); Key_SetBinding(K_DOWNARROW, "+back");
    Key_SetBinding(K_LEFTARROW, "+left"); Key_SetBinding(K_RIGHTARROW, "+right");
    Key_SetBinding(K_MOUSE1, "+attack"); Key_SetBinding(K_SPACE, "+jump");
    Key_SetBinding(K_SHIFT, "+speed"); Key_SetBinding('`', "toggleconsole");
    Key_SetBinding(K_F11, "vid_togglefullscreen");
    Key_SetBinding('1', "impulse 1"); Key_SetBinding('2', "impulse 2");
    Key_SetBinding('3', "impulse 3"); Key_SetBinding('4', "impulse 4");
    Key_SetBinding('5', "impulse 5"); Key_SetBinding('6', "impulse 6");
    Key_SetBinding('7', "impulse 7"); Key_SetBinding('8', "impulse 8");
    initialized = true;
    key_text_input = true;
}
void IN_Shutdown(void)
{
    if (!initialized) return;
    IN_ClearStates(); initialized = false; key_text_input = false;
    LQ_UpdateCapture();
}
void IN_Commands(void) { LQ_UpdateCapture(); }

static int MouseKey(Uint8 button)
{
    switch (button) {
    case SDL_BUTTON_LEFT: return K_MOUSE1;
    case SDL_BUTTON_RIGHT: return K_MOUSE2;
    case SDL_BUTTON_MIDDLE: return K_MOUSE3;
    case SDL_BUTTON_X1: return K_AUX1;
    case SDL_BUTTON_X2: return K_AUX2;
    default: return 0;
    }
}
static void Button(int key, qboolean down)
{
    if (!key) return;
    if (down) {
        if (!held[key]++) Key_Event(key, true);
    } else if (held[key] && !--held[key]) Key_Event(key, false);
}
void LQ_HandleEvent(const SDL_Event *event)
{
    int key;
    if (event->type >= SDL_EVENT_WINDOW_FIRST && event->type <= SDL_EVENT_WINDOW_LAST &&
        event->type != SDL_EVENT_WINDOW_EXPOSED)
        InputLog("window event 0x%x", (unsigned)event->type);
    switch (event->type) {
    case SDL_EVENT_QUIT: quit_requested = true; break;
    case SDL_EVENT_WINDOW_ENTER_FULLSCREEN: case SDL_EVENT_WINDOW_LEAVE_FULLSCREEN:
    case SDL_EVENT_WINDOW_SHOWN: case SDL_EVENT_WINDOW_DISPLAY_CHANGED:
        /* macOS animates into and out of a fullscreen Space. Relative mode
         * set during that animation can leave the cursor free: it stops at
         * the screen's top or bottom edge, so vertical look dies while
         * horizontal still works, until a click. Capture again afterwards. */
        if (event->window.windowID == SDL_GetWindowID(LQ_SDLWindow())) ReleaseCapture();
        break;
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
    case SDL_EVENT_WINDOW_FOCUS_LOST:
    case SDL_EVENT_WINDOW_MINIMIZED:
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
    case SDL_EVENT_WINDOW_EXPOSED:
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        if (event->window.windowID != SDL_GetWindowID(LQ_SDLWindow())) break;
        switch (event->type) {
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED: quit_requested = true; break;
        case SDL_EVENT_WINDOW_FOCUS_LOST: case SDL_EVENT_WINDOW_MINIMIZED: LQ_SetFocus(false); break;
        case SDL_EVENT_WINDOW_FOCUS_GAINED: ReleaseCapture(); LQ_SetFocus(true); break;
        case SDL_EVENT_WINDOW_EXPOSED: case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED: VID_Update(NULL); break;
        default: break;
        }
        break;
    case SDL_EVENT_KEY_DOWN:
        if (!focused) break;
        if ((event->key.mod & SDL_KMOD_ALT) && event->key.key == SDLK_RETURN) {
            if (!event->key.repeat) Cbuf_AddText("vid_togglefullscreen\n");
            suppress_text = true;
            break;
        }
#ifdef __APPLE__
        if ((event->key.mod & SDL_KMOD_GUI) && event->key.key == SDLK_Q) {
            quit_requested = true; break;
        }
#endif
        if (demo_active && event->key.key == SDLK_ESCAPE) { quit_requested = true; break; }
        key = TranslateKey(&event->key);
        suppress_text = key && keybindings[key] && !strcmp(keybindings[key], "toggleconsole");
        if (event->key.scancode < 0 || event->key.scancode >= SDL_SCANCODE_COUNT) break;
        if (!pressed[event->key.scancode]) {
            pressed[event->key.scancode] = key;
            Button(key, true);
        } else if (event->key.repeat && (key == K_BACKSPACE || key == K_DEL)) Key_Event(key, true);
        break;
    case SDL_EVENT_KEY_UP:
        if (event->key.scancode < 0 || event->key.scancode >= SDL_SCANCODE_COUNT) break;
        key = pressed[event->key.scancode];
        pressed[event->key.scancode] = 0;
        Button(key, false);
        break;
    case SDL_EVENT_TEXT_INPUT:
        if (focused && !suppress_text) {
            const unsigned char *text = (const unsigned char *)event->text.text;
            while (*text) Key_CharEvent(*text++);
        }
        suppress_text = false;
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_BUTTON_UP:
        if (focused) Button(MouseKey(event->button.button), event->type == SDL_EVENT_MOUSE_BUTTON_DOWN);
        break;
    case SDL_EVENT_MOUSE_WHEEL:
        if (focused && event->wheel.y) {
            int direction = event->wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -1 : 1;
            key = event->wheel.y * direction > 0 ? K_MWHEELUP : K_MWHEELDOWN;
            Key_Event(key, true); Key_Event(key, false);
        }
        break;
    case SDL_EVENT_MOUSE_MOTION:
        if (mouse_active) { mouse_x += event->motion.xrel; mouse_y += event->motion.yrel; }
        break;
    default: break;
    }
}
void LQ_PumpEvents(void)
{
    SDL_Event event;
    if (!LQ_SDLWindow()) return;
    LQ_UpdateCapture();
    while (SDL_PollEvent(&event)) LQ_HandleEvent(&event);
    LQ_UpdateCapture();
    CaptureSample();
}
void IN_Move(usercmd_t *cmd)
{
    float x = mouse_x, y = mouse_y;
    qboolean mouselook = freelook.value || (in_mlook.state & 1);
    mouse_x = mouse_y = 0;
    if (!mouse_active) return;
    if (m_filter.value) { x = (x + previous_x) * 0.5f; y = (y + previous_y) * 0.5f; }
    previous_x = x; previous_y = y;
    x *= sensitivity.value; y *= sensitivity.value;
    if ((in_strafe.state & 1) || (lookstrafe.value && mouselook))
        cmd->sidemove += m_side.value * x;
    else cl.viewangles[YAW] -= m_yaw.value * x;
    if (mouselook) V_StopPitchDrift();
    if (mouselook && !(in_strafe.state & 1)) {
        cl.viewangles[PITCH] += m_pitch.value * y;
        if (cl.viewangles[PITCH] > 80) cl.viewangles[PITCH] = 80;
        if (cl.viewangles[PITCH] < -70) cl.viewangles[PITCH] = -70;
    } else if ((in_strafe.state & 1) && noclip_anglehack) cmd->upmove -= m_forward.value * y;
    else cmd->forwardmove -= m_forward.value * y;
}
/* Exercise the same Quake delta calculation when a dummy SDL driver cannot
 * acquire relative mode. This does not claim to test OS mouse capture. */
void LQ_TestMouseDelta(int x, int y, usercmd_t *cmd)
{
    qboolean captured = mouse_active;
    mouse_active = true; mouse_x = x; mouse_y = y; previous_x = previous_y = 0;
    IN_Move(cmd);
    mouse_active = captured;
}
