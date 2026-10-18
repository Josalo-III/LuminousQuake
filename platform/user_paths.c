/* Original installations are read-only; writable state belongs to this engine.
 * Profiles live at <root>/<edition>/<game>: shareware, retail and rerelease
 * each keep their own id1 and mod settings, saves and demos. -userdir selects
 * the root. GPL-2.0-or-later. */
#include "quakedef.h"
#include "user_paths.h"
#include <sys/stat.h>
static char user_directory[MAX_OSPATH];
static const char *user_edition = "";
const char *LQ_Edition(void) { return user_edition; }
static int read_only;
void LQ_ReadOnlyFiles(void) { read_only = 1; }
static int SafeName(const char *name)
{
    return name && *name && strlen(name) < MAX_QPATH && !strstr(name, "..") &&
        !strchr(name, '/') && !strchr(name, '\\') && !strchr(name, ':');
}
const char *LQ_WriteDirectory(void) { return user_directory; }
int LQ_UserPath(char *out, size_t size, const char *name)
{
    if (!user_directory[0] || !SafeName(name)) return 0;
    return snprintf(out, size, "%s/%s", user_directory, name) < (int)size;
}
void LQ_InitUserDirectory(const char *game_directory, const char *edition)
{
    char root[MAX_OSPATH], path[MAX_OSPATH];
    const char *game = strrchr(game_directory, '/');
    int option = COM_CheckParm("-userdir");
    user_directory[0] = 0;
    user_edition = edition && *edition ? edition : "retail";
    /* Discovery diagnostics and the standalone viewer have no writable state. */
    if (read_only || COM_CheckParm("--check-data") || COM_CheckParm("--scene-info") ||
        COM_CheckParm("--light-analysis") ||
        COM_CheckParm("--gpu-test")) return;
    game = game ? game + 1 : game_directory;
    if (!SafeName(game)) Sys_Error("Invalid user-data game profile");
    if (option) {
        if (option + 1 >= com_argc || !*com_argv[option + 1]) Sys_Error("-userdir needs a directory");
        if (snprintf(root, sizeof(root), "%s", com_argv[option + 1]) >= (int)sizeof(root) - MAX_QPATH - 2)
            Sys_Error("User-data path too long");
    } else {
#ifdef _WIN32
        const char *home_directory = getenv("LOCALAPPDATA");
        const char *suffix = "/LuminousQuake";
#elif defined(__APPLE__)
        const char *home_directory = getenv("HOME");
        const char *suffix = "/Library/Application Support/LuminousQuake";
#else
        const char *home_directory = getenv("XDG_DATA_HOME");
        const char *suffix = "/LuminousQuake";
        if (!home_directory || !*home_directory) { home_directory = getenv("HOME"); suffix = "/.local/share/LuminousQuake"; }
#endif
        if (!home_directory || !*home_directory) Sys_Error("Cannot locate user data; use -userdir PATH");
        if (snprintf(root, sizeof(root), "%s%s", home_directory, suffix) >= (int)sizeof(root) - MAX_QPATH - 2)
            Sys_Error("User-data path too long; use -userdir PATH");
    }
    size_t length = strlen(root);
    while (length > 1 && (root[length - 1] == '/' || root[length - 1] == '\\')) root[--length] = 0;
    snprintf(user_directory, sizeof(user_directory), "%s/%s/%s", root, user_edition, game);
    /* Before October 2026 every release shared <root>/<game>. That profile was
     * mostly the rerelease's, so its first run takes it over. */
    if (!option && !strcmp(user_edition, "rerelease")) {
        char old[MAX_OSPATH];
        struct stat existing;
        snprintf(old, sizeof(old), "%s/%s", root, game);
        if (stat(user_directory, &existing) != 0 && stat(old, &existing) == 0 && S_ISDIR(existing.st_mode)) {
            snprintf(path, sizeof(path), "%s/%s/", root, user_edition);
            COM_CreatePath(path);
            if (rename(old, user_directory) == 0)
                Sys_Printf("Moved the shared profile %s to %s\n", old, user_directory);
        }
    }
    snprintf(path, sizeof(path), "%s/config.cfg", user_directory);
    COM_CreatePath(path);
    Sys_Printf("LuminousQuake edition: %s; user data: %s\n", user_edition, user_directory);
}
int LQ_UserFile(const char *name, int *handle, FILE **file)
{
    char path[MAX_OSPATH]; int opened, length;
    if (!LQ_UserPath(path, sizeof(path), name)) return -1;
    length = Sys_FileOpenRead(path, &opened);
    if (length < 0) return -1;
    if (handle) *handle = opened;
    else { Sys_FileClose(opened); *file = fopen(path, "rb"); if (!*file) return -1; }
    return length;
}
