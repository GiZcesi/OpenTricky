/*
 * np_ghost -- ghost local rejoué depuis les commandes enregistrées (fork).
 *
 * Fait courir, dans un slot IA, un rider piloté par les commandes du Player
 * d'une course précédente, lues dans un journal np_cmdlog (.npcl). Aucun
 * réseau : c'est l'étape 2b du multijoueur.
 *
 * ── Interrupteur (variables d'environnement, lues une fois au démarrage)
 *
 *   XBOX_GHOST absent      OFF (défaut) : aucun des hooks n'est installé par
 *                          lui (les hooks de commande ne le sont que si
 *                          XBOX_NETLOG=1).
 *   XBOX_GHOST=<f.npcl>    ghost actif pour la PREMIÈRE course de la session.
 *   XBOX_GHOST_SLOT=k      index (race+0xC4[], = index de roster) du slot IA
 *                          piloté. Défaut : l'index du Player enregistré (même
 *                          place sur la grille).
 *   XBOX_GHOST_HUMAN=k     index de roster donné au joueur humain. Défaut : le
 *                          plus loin du ghost sur la grille (0 ou n-1).
 *   XBOX_GHOST_FORCE=none  ne force aucune condition d'égalité (ablation).
 *   XBOX_GHOST_HUMAN_REPLAY=1  le Player humain rejoue lui aussi la commande
 *                          et le +0x15C de l'IA enregistrée à son index : plus
 *                          aucun rider ne diffère de l'enregistrement (test de
 *                          reproduction de la course entière).
 *   À combiner avec XBOX_NETLOG=1 pour journaliser la course du ghost et la
 *   comparer (port/tools/np_ghost_cmp.py).
 *
 * ── Format lu : .npcl v2 ou v3 (port/src/netplay/np_cmdlog.h)
 *   En-tête : piste, mode, graines, roster. Enregistrements du Player
 *   (kind 0) : commande par race+0x18 et état de course. v3 : états RNG au
 *   début de chaque état de course (kind 2/3) -- sans eux (v2), l'état RNG
 *   n'est pas forcé.
 *
 * ── Hooks (tous atteints seulement par des tables -> lookup_manual les voit)
 *   0x00048C40  commande IA (thiscall, 1 arg, ret 4), via np_cmdlog.c : pour
 *               le slot ghost, le générateur IA N'EST PAS appelé ; le hook
 *               écrit la commande enregistrée et dépile comme `ret 4`. Les
 *               autres slots appellent l'original. rider+0x15C forcé à 1.0
 *               (valeur constante d'un Player).
 *   0x0005BEB0  commande Player : seulement le point « avant » (RNG).
 *   0x000AC9B0  InGameState_LoadLevel (thiscall, 0 arg, ret ; table 0x19A500)
 *               : AVANT l'original, roster 0x1DE900 (n x 0x98) recopié du
 *               .npcl, entrée du ghost passée IA (+0x7D = -1), entrée de
 *               l'humain passée au port 0 (+0x7D = 0) ; [0x1DEC9C] (graine lue
 *               en 0xACA97) recopiée.
 *   0x0002E040  Race_ResetPlayerRoster (thiscall, ret 8 ; table 0x19A580) :
 *               AVANT l'original, [0x1DEC98] (graine du RNG « course », lue en
 *               0x2E17A) recopiée.
 *   RNG : au premier appel de commande du premier tick de l'état 3 (compte à
 *   rebours), avant l'original, les deux états RNG (0x1FAD70, 0x1FAD88)
 *   reçoivent ceux du .npcl au même point (v3 seulement).
 *
 * ── Alignement des ticks
 *   Le survol d'intro (état 1) n'a pas forcément la même durée : le tick
 *   d'origine est r_start[état] + (race+0x18 - g_start[état]), où r_start et
 *   g_start sont les premiers race+0x18 de chaque état dans la course
 *   enregistrée et dans la course en cours.
 *
 * ── Fin et repli
 *   Pas de commande enregistrée pour le tick (fin de l'enregistrement, état
 *   absent) -> repli sur le générateur IA pour ce tick. Après l'état 5
 *   (EndRace), une relecture (race+0x18 qui recule) ou une nouvelle course :
 *   ghost terminé, le slot redevient une IA normale.
 *
 * ── Corrections d'état -- toutes OFF sans XBOX_GHOST_STATE
 *   Course d'origine enregistrée avec XBOX_NETLOG=1 XBOX_NETLOG_STATE=1 :
 *   un .npst à côté du .npcl (sous-objet Rider du Player, 0x5A00 o par tick).
 *   XBOX_GHOST_STATE=<f.npst>  active les corrections du ghost : au point de
 *                          la demande de commande (np_ghost_take), les plages
 *                          choisies de l'instantané du tick d'origine sont
 *                          recopiées dans le ghost.
 *   XBOX_GHOST_CORR_HZ=20  fréquence (ticks d'origine multiples de 60/HZ ;
 *                          0 = seulement sur seuil). Défaut 20.
 *   XBOX_GHOST_CORR_THR=20 correction aussi dès que l'écart de position
 *                          dépasse ce seuil (unités ; contact). 0 = off.
 *   XBOX_GHOST_CORR_SET    plages : "rec" (défaut) = +0x170..+0x1BF
 *                          (position, vitesse, avancement, quaternion,
 *                          direction) + +0x454 (mode de physique) + +0x458
 *                          (état RiderEvent) ; "body", "pos", "kin",
 *                          "kinrot", "kinrot458", ou "off:len,..." (hexa).
 *                          NE PAS écrire +0x1C0..+0x3FF : ghost bloqué en
 *                          chute.
 *   XBOX_GHOST_CORR_STATE=4  état de course où corriger (4 = Race ; 0 = tous).
 *   XBOX_GHOST_CORR_SAMEEV=1 ne corriger que si +0x458 du ghost = origine
 *                          (défaut 0 : plus mauvais).
 *   XBOX_GHOST_CORR_JUMP=39.4 seuil d'un « saut visible » dans le bilan (1 m).
 *   Bilan [GHOST] corrections : appliquées, sur seuil, sauts par état +0x458.
 *
 * ── Limites connues
 *   La poussée de départ des Players est lue directement sur la manette
 *   (0x2D65B) : le ghost ne la reçoit pas. L'IA sautée ne met plus à
 *   jour sa difficulté (0x481A0) ni ses états internes ; au repli elle repart
 *   de cet état figé. Le joueur humain prend le personnage de l'entrée de
 *   roster qu'on lui donne.
 */
#ifndef NP_GHOST_H
#define NP_GHOST_H

#include <stdint.h>

extern int g_np_ghost_on;

void np_ghost_init(void);
void (*np_ghost_lookup(uint32_t xbox_va))(void);
/* Appelé par les hooks de commande (np_cmdlog.c), avant l'original. */
void np_ghost_before(void);
/* 1 si l'appel est celui du slot ghost et que la commande a été écrite. */
int  np_ghost_take(uint32_t rider, uint32_t pcmd);
/* XBOX_GHOST_HUMAN_REPLAY : appelé par le hook Player après l'original. */
void np_ghost_player_after(uint32_t rider, uint32_t pcmd);

#endif /* NP_GHOST_H */
