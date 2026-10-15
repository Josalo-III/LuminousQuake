/* Original installations are read-only; writable state belongs to this engine.
 * -userdir selects a root containing separate id1/mod profiles. GPL-2.0-or-later. */
#include "quakedef.h"
#include "user_paths.h"
static char user_directory[MAX_OSPATH];
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
void LQ_InitUserDirectory(const char *game_directory)
{
    char root[MAX_OSPATH], path[MAX_OSPATH];
    const char *game = strrchr(game_directory, '/');
    int option = COM_CheckParm("-userdir");
    user_directory[0] = 0;
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
    snprintf(user_directory, sizeof(user_directory), "%s/%s", root, game);
    snprintf(path, sizeof(path), "%s/config.cfg", user_directory);
    COM_CreatePath(path);
    Sys_Printf("LuminousQuake user data: %s\n", user_directory);
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
