// LN("..."): English menu text kept in a table or array and shown later, through L() / L_line() or an option row
// (runtime/ui/i18n.h), which looks it up then. The mark changes nothing; tools/menu_strings.py collects these, the
// L() / L_line() literals and the option rows' literals as the text to translate.
// L("..."): the text in the menus' language now (runtime/ui/i18n.mm), from C as well: for text shown outside the
// menus (the crash dialog, the platform's message boxes). Before the settings are read it follows the system language.
#pragma once
#define LN(s) s
#ifdef __cplusplus
extern "C" {
#endif
const char *L(const char *en);
#ifdef __cplusplus
}
#endif
