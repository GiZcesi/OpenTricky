/*
 * xbox_perf.h -- profil par zones d'une image.
 *
 *   XBOX_PERF=1            résumé [PERF] toutes les 2 s sur stderr + bilan à la sortie
 *   XBOX_PERF_CSV=<chemin> en plus, une ligne par image (thread du jeu : G, pump : P)
 *
 * Inerte par défaut : sans XBOX_PERF, g_perf_on vaut 0 et chaque point de
 * mesure se réduit à un test de cette variable.
 *
 * Thread du jeu (hooks port/src/perf_hooks.c) : tick (vt+0x14), rendu
 * (vt+0x18), attente de l'événement de frame (0xB2750), et le temps passé
 * dans les appels noyau pendant chacune de ces zones (dispatcher de
 * kernel_bridge.c). Pump : traduction du pushbuffer (nv2a_live_pb_tick),
 * host_present (post + blit + Present DXGI), Present DXGI seul, et une
 * durée GPU (requêtes timestamp entre deux Presents). Compteurs par image :
 * draws, méthodes, sommets, programmes décodés, compilations, envois de
 * textures, signatures de textures.
 */
#ifndef XBOX_PERF_H
#define XBOX_PERF_H

#ifdef __cplusplus
extern "C" {
#endif

extern int g_perf_on;

enum {
    /* thread du jeu */
    PZ_TICK, PZ_RENDER, PZ_FWAIT,
    /* pump */
    PZ_PB, PZ_PRESENT, PZ_DXGI,
    /* pump, dans d3d8_nv2a_draw_program_gpu : envois sommets+indices, constantes, états+IA/VS/PS, DrawIndexed */
    PZ_DUP, PZ_DCB, PZ_DSTATE, PZ_DDRAW,
    /* pump, traduction: textures (signature + envoi), clé + recherche du PS, décodage VS,
     * draw_program entier pour les draws CPU (particules) et GPU */
    PZ_TEX, PZ_PSH, PZ_VSDEC, PZ_PROGCPU, PZ_PROGGPU,
    PZ_N
};

enum {
    PC_DECODE, PC_COMPILE, PC_TEXUP, PC_TEXSIG, PC_KCALL,
    PC_CPUVTX, PC_CPUDRAW,              /* sommets / draws transformés sur CPU */
    PC_D3DSET, PC_D3DSKIP,              /* appels d'état D3D11 émis / sautés par le miroir */
    PC_N
};

void   perf_init(void);
double perf_now(void);                  /* ms (QPC) */

/* Thread du jeu : zones imbriquables (la zone courante reçoit le temps noyau). */
int    perf_zone_enter(int zone);       /* renvoie la zone précédente */
void   perf_zone_leave(int zone, int prev, double ms);
void   perf_kernel(unsigned ordinal, double ms);   /* depuis le dispatcher noyau */
void   perf_game_frame_end(int nticks);

/* Pump */
void   perf_add(int zone, double ms);
void   perf_count(int c, unsigned n);
void   perf_present_done(double gpu_ms_prev);   /* fin de host_present */

#ifdef __cplusplus
}
#endif
#endif
