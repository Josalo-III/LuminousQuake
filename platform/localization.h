/* GPL-2.0-or-later. Optional rerelease text compatibility. */
#ifndef LQ_LOCALIZATION_H
#define LQ_LOCALIZATION_H
void LQ_LocalizationInit(const char *basedir);
void LQ_LocalizationParse(char *text);
const char *LQ_Localize(const char *text);
int LQ_LocalizeArgs(int argc, const char *const *argv, char *output, unsigned size);
#endif
