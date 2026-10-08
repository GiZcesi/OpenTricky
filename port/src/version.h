/*
 * version.h -- the version of OpenTricky: one number for both executables
 * (the game, SSX Tricky.exe, and the launcher, OpenTricky.exe), their
 * VERSIONINFO (version_info.rc), the About page, crash reports and the
 * update check. Change it here only, when a release is made.
 *
 * Plain preprocessor definitions: included by C files and by the resource
 * compiler alike.
 */
#ifndef OT_VERSION_H
#define OT_VERSION_H

#define OT_VERSION_MAJOR 0
#define OT_VERSION_MINOR 1
#define OT_VERSION_PATCH 1
/* "" for a release; "-dev" for every other build. Only the release tools
 * build with OT_RELEASE (cmake -DOT_RELEASE=ON, see port/CMakeLists.txt). */
#ifdef OT_RELEASE
#define OT_VERSION_SUFFIX ""
#else
#define OT_VERSION_SUFFIX "-dev"
#endif

#define OT_STR_(x) #x
#define OT_STR(x) OT_STR_(x)
/* "0.1.1" and "0.1.1-dev" */
#define OT_VERSION_NUMBER OT_STR(OT_VERSION_MAJOR) "." OT_STR(OT_VERSION_MINOR) "." OT_STR(OT_VERSION_PATCH)
#define OT_VERSION        OT_VERSION_NUMBER OT_VERSION_SUFFIX

#define OT_PRODUCT_NAME   "OpenTricky"
#define OT_COPYRIGHT      "OpenTricky. Not affiliated with Electronic Arts."

#endif /* OT_VERSION_H */
