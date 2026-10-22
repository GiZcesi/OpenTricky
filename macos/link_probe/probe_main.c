/* Dummy translated-game entry points, so the runtime links without gen/. */
#include <stdint.h>
typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(uint32_t va)        { (void)va; return 0; }
recomp_func_t recomp_lookup_manual(uint32_t va) { (void)va; return 0; }
int main(void) { return 0; }
