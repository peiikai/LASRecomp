// The iOS layer's own functions, shared between its files (runtime/platform/ios*.c / .m) and nothing else.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

const char *plat_ios_bundle_dir(void);  // the .app (flat: resources, the game framework)
const char *plat_ios_os_version(void);  // "26.0.1"

#ifdef __cplusplus
}
#endif
