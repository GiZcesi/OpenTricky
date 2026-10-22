/* Stand-ins for what the translated game defines, so the tests link without it. */
#include <stdint.h>

volatile unsigned g_last_loc, g_main_loc;
int g_fe_cursor;
int fe_menu_get_pb_state(void) { return 0; }

typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(uint32_t va)        { (void)va; return 0; }
recomp_func_t recomp_lookup_manual(uint32_t va) { (void)va; return 0; }
int g_lowacc_enabled;
void xbox_lowaccess_log() {}
