/* The app's main executable: a signed Mach-O that hands over to Contents/Resources/launcher.sh (a shell script cannot be the notarized main
 * executable of a hardened app in every case, and a tiny stub keeps the bundle uniform). */
#include <mach-o/dyld.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    char exe[PATH_MAX], real[PATH_MAX], script[PATH_MAX + 64], *slash;
    uint32_t n = sizeof exe;
    char **args;
    int i;
    if (_NSGetExecutablePath(exe, &n) != 0 || !realpath(exe, real)) return 1;
    slash = strrchr(real, '/');
    if (!slash) return 1;
    *slash = 0;                                                  /* .../Contents/MacOS */
    snprintf(script, sizeof script, "%s/../Resources/launcher.sh", real);
    args = (char **)calloc((size_t)argc + 2, sizeof *args);
    if (!args) return 1;
    args[0] = (char *)"bash"; args[1] = script;
    for (i = 1; i < argc; i++) args[i + 1] = argv[i];
    execv("/bin/bash", args);
    perror("SSX Tricky: cannot start the launcher");
    return 1;
}
