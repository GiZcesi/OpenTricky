/*
 * fps_cap -- cadence d'images host au-delà de 60 (fork).
 *
 * Le jeu fait ses ticks logiques à 60 Hz (timer logiciel 0xB26B0) et, dans sa
 * boucle principale 0xAA1A0, « N ticks puis 1 rendu » : jamais de rendu sans
 * tick. Quand aucun tick n'est dû, il attend l'événement de frame
 * (XBoxExecutionMan_WaitForFrameEvent 0xB2750, appelé en 0xAA296).
 *
 * Ce module remplace cette attente : tant qu'aucun tick n'est dû, il rappelle
 * le rendu de l'état courant (state->vt+0x18) au rythme du plafond host. Le
 * rendu sans tick reçoit dt = 0 (0xAB610 : [state+0x4C] = ticks depuis le
 * dernier rendu) et ne touche pas l'état logique (mesuré ici aussi).
 *
 * Interpolation (XBOX_FPS_INTERP=1 par défaut quand le cap != 60) :
 * chaque rendu d'InGameState (normal ou en plus) montre lerp(tick N-1, tick N,
 * alpha), alpha = temps depuis le début du tick N / 16,67 ms. Affichage
 * décalé d'un tick (+16,7 ms de latence) ; pas d'extrapolation.
 *   caméra : bloc de vue InGameState+0xB0 (+i×0x80, vues < [+0x298]) :
 *            position linéaire, rotation de la matrice de vue par slerp
 *            (déterminant -1 géré), projection +0x100/+0x104 linéaire ;
 *   riders : position +0x170 et pose +0x48B0 (21 matrices 4×4 en
 *            coordonnées MONDE) : translation linéaire, rotation slerp.
 *   Copie des états au tick (hook 0xAD4A0) ; avant un rendu, la mémoire doit
 *   être identique à la copie du tick N, sinon rien n'est écrit ; après le
 *   rendu, la copie du tick N est réécrite (au bit près). Riders entiers
 *   remis tels qu'avant après un rendu en plus. Téléportation (> 400 unités
 *   en un tick) ou coupe caméra (> 60°) : état N (doublon).
 *   Reste au tick : HUD, particules, foule, décor animé, dérivés de pose
 *   calculés par le rendu (+0x4DF0.., suivent la pose interpolée).
 *   XBOX_FPS_INTERP=0      doublons (sans interpolation)
 *   XBOX_FPS_INTERP_LOG=N[@t]  N rendus de course journalisés ([INTERP]) à partir du tick de course t
 *   XBOX_FPS_INTERP_SYNTH=1    (test, avec XBOX_FPS_CAP_DUP) alpha = k / (DUP + 1)
 *   XBOX_FPS_INTERP_DUMP=t     (enquête) vide caméra / rider 0 sur 4 ticks
 *   XBOX_FPS_CAP_CHECK=3       (test) rendu en plus refait sans interpolation :
 *                              mots écrits par le rendu qui dépendent de l'interpolation
 *   XBOX_FPS_CAP_STALL=s:ms    (test) une saccade simulée de ms dans un rendu normal à s secondes
 *
 * Cadence: la durée moyenne d'un rendu est mesurée
 * sur les rendus normaux (InGameState 0xAB610, menus 0x7CBD0) et en plus ;
 * les durées > 50 ms (chargements) sont ignorées. Avant, une seule saccade
 * figeait la moyenne et bloquait les images en plus jusqu'à la fin.
 *
 *   XBOX_FPS_CAP=60 (défaut) | 120 | 144 | 240 | N (60..1000) | 0 (sans limite)
 *       60 ou absent : hook non installé, comportement d'origine strict.
 *   XBOX_FPS_CAP_LOG=1     statistiques toutes les ~2 s ([FPSCAP])
 *   XBOX_FPS_CAP_NOSKIP=1  (test) rendre en plus même si le tick suivant en est retardé
 *   XBOX_FPS_CAP_CHECK=2   témoin : même attente sans rendu (attribue les écarts aux autres threads)
 *   XBOX_FPS_CAP_CHECK=1   instrument d'état : empreinte avant / après chaque
 *                          rendu en plus (RNG, course, riders entiers,
 *                          AudioSystem), diff de pages mémoire échantillonné
 *
 * Fidélité : états RNG A/B sauvegardés et restaurés autour de chaque rendu en
 * plus (le rendu tire le RNG A pendant le survol d'intro) ; liste blanche des
 * rendus (course 0xAB610, menus 0x7CBD0) ; pas pendant le saut [état+0x6C].
 *
 * Règles :
 *   - un tick dû passe toujours avant (attente de l'événement avec délai,
 *     sondage à 0 juste avant chaque rendu) ;
 *   - pas de rendu en plus qui finirait après le tick suivant attendu
 *     (estimation : durée moyenne d'un rendu) : les ticks ne sont pas retardés ;
 *   - jamais plus d'un rendu en retard (pas de rattrapage côté rendu) ;
 *   - seulement sur l'appel de 0xAA296 (edi = 0, esi = Application,
 *     ecx = [esi+0x2C]) ; l'attente de démarrage 0xAA1D0 (edi = 3) garde
 *     l'original.
 */
#ifndef FPS_CAP_H
#define FPS_CAP_H

extern int g_fps_cap_on;

void fps_cap_init(void);
void (*fps_cap_lookup(unsigned int xbox_va))(void);

#endif
