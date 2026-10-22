/* The translated game's entry points, so the port links without port/src/recomp/gen/. */
#include <stdint.h>
#include <stddef.h>
typedef void (*recomp_func_t)(void);
void xbe_entry_point(void) {}
size_t recomp_dispatch_init(void) { return 0; }
recomp_func_t recomp_lookup(uint32_t va) { (void)va; return 0; }
/* labels the generated code stamps and the front-end menu state it defines */
volatile unsigned g_last_loc, g_main_loc;
int g_fe_cursor;
int fe_menu_get_pb_state(void) { return 0; }
volatile uint32_t g_icall_trace[16];
volatile uint32_t g_icall_trace_idx;
