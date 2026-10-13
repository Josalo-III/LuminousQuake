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
#include "quakedef.h"
#include "sdl_backend.h"

static qboolean initialized, focused, quit_requested, demo_active, mouse_active, suppress_text;
static int pressed[SDL_NUM_SCANCODES], held[256];
static float mouse_x, mouse_y, previous_x, previous_y;
static cvar_t in_mouse = {"in_mouse", "1", true};
static cvar_t in_nograb = {"in_nograb", "0", true};
static cvar_t m_filter = {"m_filter", "0", true};
extern kbutton_t in_strafe, in_mlook;

qboolean LQ_HasFocus(void) { return focused; }
qboolean LQ_QuitRequested(void) { return quit_requested; }
void LQ_ResetQuit(void) { quit_requested = false; }
void LQ_SetDemo(qboolean active) { demo_active = active; }

static int TranslateKey(const SDL_Keysym *keysym)
{
    SDL_Keycode symbol = keysym->sym;
    /* Preserve number-row bindings across keyboard layouts, as ioquake3 does. */
    if (keysym->scancode >= SDL_SCANCODE_1 && keysym->scancode <= SDL_SCANCODE_0)
        return keysym->scancode == SDL_SCANCODE_0 ? '0' : '1' + keysym->scancode - SDL_SCANCODE_1;
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
void LQ_UpdateCapture(void)
{
    SDL_Window *window = LQ_SDLWindow();
    qboolean wanted = initialized && window && focused && in_mouse.value && !in_nograb.value &&
        !COM_CheckParm("-nomouse") && key_dest == key_game && !cl.paused &&
        (demo_active || (cls.state == ca_connected && !con_forcedup));
    if (wanted == mouse_active) return;
    if (wanted) {
        if (SDL_SetRelativeMouseMode(SDL_TRUE)) {
            /* Dummy/headless video drivers need not implement capture. */
            return;
        }
        SDL_SetWindowGrab(window, SDL_TRUE);
    } else {
        SDL_SetRelativeMouseMode(SDL_FALSE);
        if (window) SDL_SetWindowGrab(window, SDL_FALSE);
    }
    mouse_active = wanted;
    mouse_x = mouse_y = previous_x = previous_y = 0;
    SDL_GetRelativeMouseState(NULL, NULL);
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
    Cbuf_AddText("+mlook\n");
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
    switch (event->type) {
    case SDL_QUIT: quit_requested = true; break;
    case SDL_WINDOWEVENT:
        if (event->window.windowID != SDL_GetWindowID(LQ_SDLWindow())) break;
        switch (event->window.event) {
        case SDL_WINDOWEVENT_CLOSE: quit_requested = true; break;
        case SDL_WINDOWEVENT_FOCUS_LOST: case SDL_WINDOWEVENT_MINIMIZED: LQ_SetFocus(false); break;
        case SDL_WINDOWEVENT_FOCUS_GAINED: LQ_SetFocus(true); break;
        case SDL_WINDOWEVENT_EXPOSED: case SDL_WINDOWEVENT_SIZE_CHANGED: VID_Update(NULL); break;
        default: break;
        }
        break;
    case SDL_KEYDOWN:
        if (!focused) break;
        if ((event->key.keysym.mod & KMOD_ALT) && event->key.keysym.sym == SDLK_RETURN) {
            if (!event->key.repeat) Cbuf_AddText("vid_togglefullscreen\n");
            suppress_text = true;
            break;
        }
#ifdef __APPLE__
        if ((event->key.keysym.mod & KMOD_GUI) && event->key.keysym.sym == SDLK_q) {
            quit_requested = true; break;
        }
#endif
        if (demo_active && event->key.keysym.sym == SDLK_ESCAPE) { quit_requested = true; break; }
        key = TranslateKey(&event->key.keysym);
        suppress_text = key && keybindings[key] && !strcmp(keybindings[key], "toggleconsole");
        if (event->key.keysym.scancode >= SDL_NUM_SCANCODES) break;
        if (!pressed[event->key.keysym.scancode]) {
            pressed[event->key.keysym.scancode] = key;
            Button(key, true);
        } else if (event->key.repeat && (key == K_BACKSPACE || key == K_DEL)) Key_Event(key, true);
        break;
    case SDL_KEYUP:
        if (event->key.keysym.scancode >= SDL_NUM_SCANCODES) break;
        key = pressed[event->key.keysym.scancode];
        pressed[event->key.keysym.scancode] = 0;
        Button(key, false);
        break;
    case SDL_TEXTINPUT:
        if (focused && !suppress_text) {
            const unsigned char *text = (const unsigned char *)event->text.text;
            while (*text) Key_CharEvent(*text++);
        }
        suppress_text = false;
        break;
    case SDL_MOUSEBUTTONDOWN: case SDL_MOUSEBUTTONUP:
        if (focused) Button(MouseKey(event->button.button), event->type == SDL_MOUSEBUTTONDOWN);
        break;
    case SDL_MOUSEWHEEL:
        if (focused && event->wheel.y) {
            int direction = event->wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -1 : 1;
            key = event->wheel.y * direction > 0 ? K_MWHEELUP : K_MWHEELDOWN;
            Key_Event(key, true); Key_Event(key, false);
        }
        break;
    case SDL_MOUSEMOTION:
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
}
void IN_Move(usercmd_t *cmd)
{
    float x = mouse_x, y = mouse_y;
    mouse_x = mouse_y = 0;
    if (!mouse_active) return;
    if (m_filter.value) { x = (x + previous_x) * 0.5f; y = (y + previous_y) * 0.5f; }
    previous_x = x; previous_y = y;
    x *= sensitivity.value; y *= sensitivity.value;
    if ((in_strafe.state & 1) || (lookstrafe.value && (in_mlook.state & 1)))
        cmd->sidemove += m_side.value * x;
    else cl.viewangles[YAW] -= m_yaw.value * x;
    if (in_mlook.state & 1) V_StopPitchDrift();
    if ((in_mlook.state & 1) && !(in_strafe.state & 1)) {
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
