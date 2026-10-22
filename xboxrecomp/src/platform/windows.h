/* windows.h -- non-Windows shim. On Windows the real SDK header is used (this
 * directory is only on the include path for non-Windows builds). Gives code
 * that says `#include <windows.h>` the NT type vocabulary and the generic
 * host primitives from win32_compat. */
#ifndef OT_SHIM_WINDOWS_H
#define OT_SHIM_WINDOWS_H
#include "xbox_winnt.h"
#endif
