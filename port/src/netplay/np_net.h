/*
 * np_net -- online multiplayer hooks.
 *
 * Online play is not part of this source tree: these hooks are empty, so the
 * compiler drops every call and every branch that tests them. The game runs
 * exactly as without them.
 */
#ifndef NP_NET_H
#define NP_NET_H

#include <stdint.h>

#define NP_NET_NAME_LEN 17

#define g_np_net_on 0
static inline void np_net_init(void) {}
static inline void (*np_net_lookup(uint32_t xbox_va))(void) { (void)xbox_va; return 0; }
static inline void np_net_before(void) {}
static inline int  np_net_take(uint32_t rider, uint32_t pcmd) { (void)rider; (void)pcmd; return 0; }
static inline void np_net_player_after(uint32_t rider, uint32_t pcmd) { (void)rider; (void)pcmd; }
static inline void np_net_ai_after(uint32_t rider, uint32_t pcmd) { (void)rider; (void)pcmd; }
static inline int  np_net_block_pause(void) { return 0; }
static inline void np_net_pre_2d600(void) {}
static inline void np_net_post_2d600(void) {}
static inline void np_net_at_783d0(uint32_t rider, uint32_t pressed) { (void)rider; (void)pressed; }
static inline void np_net_at_34da0(uint32_t rider, uint32_t other) { (void)rider; (void)other; }
static inline void np_net_pre_contact(uint32_t rider) { (void)rider; }
static inline void np_net_post_contact(uint32_t rider) { (void)rider; }
static inline int  np_net_knock_34da0(uint32_t victim, uint32_t aggressor) { (void)victim; (void)aggressor; return 0; }
static inline int  np_net_knock_126090(uint32_t aggressor, uint32_t victim, uint32_t flag) { (void)aggressor; (void)victim; (void)flag; return 0; }
static inline int  np_net_knock_125670(uint32_t victim, uint32_t aggressor) { (void)victim; (void)aggressor; return 0; }
static inline int  np_net_knock_335b0(uint32_t aggressor) { (void)aggressor; return 0; }
static inline int  np_net_knock_31b60(uint32_t victim) { (void)victim; return 0; }
static inline int  np_net_knock_113370(uint32_t aggressor, uint32_t victim) { (void)aggressor; (void)victim; return 0; }
static inline int  np_net_knock_125930(uint32_t victim) { (void)victim; return 0; }
static inline int  np_net_test_knock_pre(uint32_t victim, uint32_t other) { (void)victim; (void)other; return 0; }
static inline void np_net_test_knock_post(uint32_t victim, uint32_t other, uint32_t knocked) { (void)victim; (void)other; (void)knocked; }
static inline int  np_net_spect_press(void) { return 0; }
static inline void np_net_off(void) {}

/* name tags over the other riders, straight start: none */
#define g_np_tags_on 0
static inline void np_tags_init(void) {}
static inline void np_tags_frame(uint32_t frame) { (void)frame; }
static inline void np_tags_phase(int old_phase, int new_phase, unsigned tag) { (void)old_phase; (void)new_phase; (void)tag; }
static inline void np_tags_image_end(void) {}
static inline void np_tags_wait_line(const char *text) { (void)text; }
static inline int np_direct_block_menu(void) { return 0; }

#endif /* NP_NET_H */
