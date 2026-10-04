/*
 * d3d8_gpuprof.h -- temps GPU par passe d'une image.
 *
 *   XBOX_GPUPROF=1             résumé [GPU] toutes les 2 s sur stderr + bilan à la sortie
 *   XBOX_GPUPROF_CSV=<chemin>  en plus, une ligne par image
 *
 * Chaque draw, clear et étape du post est encadré par deux requêtes
 * timestamp, émises l'une à la suite de l'autre sans travail CPU entre les
 * deux : leur écart est le temps d'exécution GPU de ce travail seul, même
 * quand le GPU attend le CPU entre deux draws (ce que la mesure « GPU » de
 * XBOX_PERF, du Present au Present, compte au contraire). Les écarts sont
 * sommés par catégorie ; « span » = du premier au dernier horodatage de
 * l'image (temps occupé + trous). Relu sans attendre, NF images plus tard.
 *
 * Inerte par défaut : sans XBOX_GPUPROF, g_gpuprof_on vaut 0 et chaque point
 * de mesure se réduit à un test de cette variable. Thread du pump seulement.
 * Coût CPU notable quand actif (~2 requêtes par draw) : ne pas mesurer les
 * FPS avec.
 */
#ifndef D3D8_GPUPROF_H
#define D3D8_GPUPROF_H

#ifdef __cplusplus
extern "C" {
#endif

enum {
    GP_G0, GP_G1, GP_G2, GP_G3, GP_G4, GP_G5, GP_G6,    /* draws 3D par groupe */
    GP_PERSP7,          /* draws groupes >= 7 en perspective (brume, lens flare) */
    GP_HUD,             /* draws groupes >= 7 ortho (HUD / menus) */
    GP_OTHER,           /* draws sans phase (pas de marqueurs, vidéos, 2e thread) */
    GP_CLEAR,           /* clears du jeu */
    GP_POST_SPLIT,      /* FRAME_END : résolution MSAA + chaîne post + réécriture */
    GP_POST_PRESENT,    /* chaîne post au present (sans split) */
    GP_PREVFRAME,       /* copie de l'image précédente (chrome « Master ») */
    GP_RESOLVE,         /* résolution MSAA au present */
    GP_BLIT,            /* copie / blit (+ gamma DAC) vers la swap chain */
    GP_SOFTSHADOW,      /* ombres douces: masque stencil, flou, application */
    GP_N
};

extern int g_gpuprof_on;

void gpuprof_init(void);
/* Traducteur : phase courante (PGRAPH_PHASE_*) et groupe du dernier marqueur. */
void gpuprof_set_phase(int phase, int group);
int  gpuprof_draw_cat(void);            /* catégorie d'un draw du jeu maintenant */
void gpuprof_begin(int cat);
void gpuprof_end(void);
void gpuprof_draw_begin(void);          /* draws du jeu (mode 1 : par suite, 2 : par draw) */
void gpuprof_draw_end(void);
void gpuprof_count_flush(void);         /* GetData qui a pu vider le tampon de commandes */
void gpuprof_note(unsigned bits);       /* colonne « note » de l'image (1 = stencil SMAA, 2 = écriture directe) */
void gpuprof_frame(void);               /* juste avant le Present DXGI */

#ifdef __cplusplus
}
#endif
#endif
