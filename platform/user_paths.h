/* Installation/user-data policy. GPL-2.0-or-later. */
#ifndef LQ_USER_PATHS_H
#define LQ_USER_PATHS_H
void LQ_InitUserDirectory(const char *game_directory);
void LQ_ReadOnlyFiles(void);
const char *LQ_WriteDirectory(void);
int LQ_UserFile(const char *name, int *handle, FILE **file);
int LQ_UserPath(char *out, size_t size, const char *name);
#endif
