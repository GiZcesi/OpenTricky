/*
 * ticktrace -- trace par tick de course : RNG et empreintes d'état (fork).
 *
 * But : un critère « bout en bout » pour l'uncap. Deux runs ne
 * se comparent pas image par image (non déterministes) ; on compare la
 * suite des TICKS : à chaque tick de course, l'état logique doit être le même.
 * Le premier tick où l'empreinte diffère, et le tirage RNG qui l'explique,
 * disent si un écart vient d'un état non capturé ou de la variance du jeu.
 *
 *   XBOX_TICKTRACE=1        une ligne par tick de course (InGameState vt+0x14 = 0xAD4A0)
 *   XBOX_TICKTRACE_RNG=1    + une ligne par tirage RNG A/B (hook 0x12A610) avec
 *                           phase (tick / hors tick / autre thread) et les 3
 *                           premières fonctions invitées appelantes
 *   XBOX_TICKTRACE_DIR=dir  dossier (défaut : le plus haut « _local » au-dessus
 *                           de l'exe, + \ticktrace)
 *   XBOX_TICKTRACE_EXTRA=va:len,...   régions absolues en plus dans l'empreinte
 *   XBOX_TICKTRACE_WATCH=base+off:len,...   « qui écrit quoi » : pour chaque mot,
 *                           nombre de ticks où il change PENDANT le tick et de
 *                           rendus (0xAB610) où il change PENDANT le rendu ;
 *                           base = st (InGameState [app+4]), cam ([st+0x2C]),
 *                           race, app, r0..r7 (riders [race+0xC4+4i])
 *                           ou VA absolue ; bilan « # watch » tous les 1 800 ticks
 *                           (8 régions, 64 Ko chacune au plus)
 *
 * Ligne T (après chaque tick) :
 *   T seq course_tick etat nA nB dA dB rngA rngB course cam nriders r0..r5 [xEXTRA]
 *     nA/nB = compteurs de tirages [0x1FAD84] / [0x1FAD9C] ([obj+0x14], +1 par
 *     tirage, 0x12A62C) ; dA/dB = tirages pendant ce tick ; empreintes FNV-1a 32 :
 *     rngA/rngB (6 mots), course (0x400 o), cam = vue 0 d'InGameState (matrice
 *     de vue +0xC0 et projection +0x100/+0x104), puis nriders et, par rider, la
 *     cinématique (position +0x170, vitesse +0x180, +0x458, +0x15C, jauge +0x1C).
 *     Les tirages faits dans le tick N sont les lignes R de seq N-1.
 * Ligne R (XBOX_TICKTRACE_RNG=1) :
 *   R seq A|B n phase appelant1 appelant2 appelant3
 *     phase : t = dans le tick de course, h = thread de la boucle hors tick
 *     (rendu, autres états), o = autre thread. Appelants : fonctions invitées
 *     (adresse de début) retrouvées depuis la pile host ; une fonction invitée
 *     incorporée (inline) par le compilateur peut manquer.
 *
 *   XBOX_TICKTRACE_READS=base+off:len   (enquête, lent) une région : un rendu sur
 *                           XBOX_TICKTRACE_READS_EVERY (30), ses pages host passent en
 *                           PAGE_NOACCESS ; chaque accès est noté (mot lu / écrit,
 *                           fonction invitée lectrice / écrivaine) ; bilan « # reads ».
 *                           XBOX_TICKTRACE_READS_PHASE=tick : trace les ticks au lieu
 *                           des rendus (qui écrit l'état pendant le tick)
 *
 * Inerte par défaut : sans XBOX_TICKTRACE, le hook du tick n'est pas installé ;
 * le hook RNG (appels directs routés par la passe 13) appelle l'original tout de
 * suite. Comparaison de deux traces : port/tools/ticktrace_cmp.py.
 */
#ifndef TICKTRACE_H
#define TICKTRACE_H

extern int g_ticktrace_on;

void ticktrace_init(void);
void (*ticktrace_lookup(unsigned int xbox_va))(void);

#endif
