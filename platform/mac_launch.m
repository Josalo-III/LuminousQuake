/* Finder launch and installation selection. GPL-2.0-or-later. */
#import <Cocoa/Cocoa.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <stdio.h>
#include <string.h>
#include "mac_launch.h"
static BOOL interactive;
int LQ_MacFinderLaunch(void) { return interactive; }
static NSString *Normalize(NSString *path)
{
    if ([[path lastPathComponent] caseInsensitiveCompare:@"id1"] == NSOrderedSame)
        path = [path stringByDeletingLastPathComponent];
    return [path stringByStandardizingPath];
}
static BOOL Valid(NSString *path)
{
    if (!path) return NO;
    NSString *pak = [Normalize(path) stringByAppendingPathComponent:@"id1/pak0.pak"];
    FILE *file = fopen([pak fileSystemRepresentation], "rb");
    if (!file) { /* DOS-era and GOG installs: PAK0.PAK on a case-sensitive volume */
        pak = [Normalize(path) stringByAppendingPathComponent:@"id1/PAK0.PAK"];
        file = fopen([pak fileSystemRepresentation], "rb");
    }
    if (!file) return NO;
    unsigned char header[12]; size_t count = fread(header, 1, sizeof(header), file);
    fclose(file);
    return count == sizeof(header) && !memcmp(header, "PACK", 4);
}
void LQ_MacError(const char *message)
{
    if (!interactive) return;
    @autoreleasepool {
        NSAlert *alert = [[NSAlert alloc] init];
        [alert setMessageText:@"LuminousQuake could not start"];
        [alert setInformativeText:[NSString stringWithUTF8String:message] ?: @"Unknown startup error"];
        [alert runModal];
    }
}
int LQ_MacInstallation(int argc, char **argv, char *path, size_t size)
{
    @autoreleasepool {
        NSBundle *bundle = [NSBundle mainBundle];
        BOOL app = [[[bundle bundlePath] pathExtension] isEqualToString:@"app"];
        BOOL choose = NO;
        for (int i = 1; i < argc; ++i) if (!strcmp(argv[i], "--choose-installation")) choose = YES;
        interactive = app && (argc == 1 || choose);
        for (int i = 1; i < argc; ++i)
            if (!strcmp(argv[i], "-basedir")) return 1; /* explicit CLI override */
        NSString *nearby = app ? [[bundle bundlePath] stringByDeletingLastPathComponent] :
            [[[NSString stringWithUTF8String:argv[0]] stringByStandardizingPath] stringByDeletingLastPathComponent];
        /* A copy dropped into a Quake folder plays that folder; otherwise use the
         * folder chosen last time. */
        NSString *candidate = nearby;
        if (!Valid(candidate)) candidate = [[NSUserDefaults standardUserDefaults] stringForKey:@"QuakeInstallation"];
        if (choose || (interactive && ([NSEvent modifierFlags] & NSEventModifierFlagOption))) candidate = nil;
        if (!Valid(candidate)) {
            if (!interactive) return 1; /* CLI discovery supplies a useful terminal error. */
            SDL_SetMainReady();
            if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) return -1;
            SDL_Window *prompt = SDL_CreateWindow("LuminousQuake", 640, 360, SDL_WINDOW_HIDDEN);
            [NSApp activateIgnoringOtherApps:YES];
            NSAlert *notice = [[NSAlert alloc] init];
            [notice setMessageText:@"Locate your Quake installation"];
            [notice setInformativeText:@"Choose the folder containing id1 and your original pak0.pak. LuminousQuake uses the existing game data without changing it."];
            [notice addButtonWithTitle:@"Choose Folder…"]; [notice addButtonWithTitle:@"Cancel"];
            if ([notice runModal] != NSAlertFirstButtonReturn) { SDL_DestroyWindow(prompt); return 0; }
            for (;;) {
                NSOpenPanel *panel = [NSOpenPanel openPanel];
                [panel setCanChooseDirectories:YES]; [panel setCanChooseFiles:NO];
                [panel setAllowsMultipleSelection:NO]; [panel setPrompt:@"Use Quake Folder"];
                if ([panel runModal] != NSModalResponseOK) { SDL_DestroyWindow(prompt); return 0; }
                candidate = Normalize([[panel URL] path]);
                if (Valid(candidate)) break;
                NSAlert *error = [[NSAlert alloc] init];
                [error setMessageText:@"Quake data was not found"];
                [error setInformativeText:@"Select an installation containing id1/pak0.pak, or select its id1 folder. A PAK with a valid header is required; pak1.pak is optional."];
                [error runModal];
            }
            SDL_DestroyWindow(prompt);
            [[NSUserDefaults standardUserDefaults] setObject:candidate forKey:@"QuakeInstallation"];
        }
        candidate = Normalize(candidate);
        if (snprintf(path, size, "%s", [candidate fileSystemRepresentation]) >= (int)size) return -1;
        return 1;
    }
}
int LQ_MacSettleWindowMove(SDL_Window *window)
{
    if (!window || ([NSEvent pressedMouseButtons] & 1)) return 0; /* a real drag ends on its own */
    NSWindow *nswindow = (__bridge NSWindow *)SDL_GetPointerProperty(SDL_GetWindowProperties(window),
        SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, NULL);
    id delegate = [nswindow delegate];
    SEL moving = NSSelectorFromString(@"isMoving"), finish = NSSelectorFromString(@"windowDidFinishMoving");
    /* SDL's own window listener; another SDL version without them is left alone. */
    if (![delegate respondsToSelector:moving] || ![delegate respondsToSelector:finish]) return 0;
    if (!((BOOL (*)(id, SEL))[delegate methodForSelector:moving])(delegate, moving)) return 0;
    ((void (*)(id, SEL))[delegate methodForSelector:finish])(delegate, finish);
    return 1;
}
