// The iOS layer's own functions, shared between its files (runtime/platform/ios*.c / .m) and nothing else.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

const char *plat_ios_bundle_dir(void);  // the .app (flat: resources, the game framework, the embedded Python)
const char *plat_ios_os_version(void);  // "26.0.1"
// The embedded Python's self-test (LARECOMP_IOS_PYTEST=1 at startup, ios.c plat_init): ios_python.m
void plat_ios_python_selftest(void);

#ifdef __cplusplus
}
#endif
