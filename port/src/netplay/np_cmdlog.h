/*
 * np_cmdlog -- journal des commandes et de l'état des riders (fork).
 *
 * Première brique du multijoueur, SANS réseau : à chaque tick de
 * course, le mot de commande 32 bits de chaque rider et son état clé sont
 * écrits dans un fichier binaire rejouable. Les hooks ne changent rien :
 * ils appellent toujours l'original et lisent après.
 *
 * ── Interrupteur (variable d'environnement, lue une fois au démarrage)
 *
 *   XBOX_NETLOG absent / "0"  OFF (défaut). recomp_lookup_manual() ne renvoie
 *                             aucun des hooks : chemin d'origine intact.
 *   XBOX_NETLOG=1             JOURNAL. Un fichier par course :
 *                             <_local>/netlog/cmdlog_<AAAAMMJJ_HHMMSS>_<n>.npcl
 *   XBOX_NETLOG_DIR           dossier de sortie (défaut : le « _local » le plus
 *                             haut en remontant depuis l'exe, + \netlog ; sinon
 *                             le dossier de l'exe).
 *
 * ── Les hooks (conventions vérifiées dans les octets)
 *
 *   La commande est demandée une fois par rider et par tick par
 *   Rider_UpdatePhysicsState (0x36990), site 0x36F7F :
 *       push &cmd ; ecx = rider ; call [ [rider] + 0x24 ]   (RECOMP_ICALL_SAFE)
 *   puis consommée par 0x31640 (dispatch sur rider+0x458, voir plus bas).
 *
 *   0x0005BEB0  commande du Player (manette)   slot +0x24 de la vtable 0x189ED0
 *   0x00048C40  commande des OtherRider (IA)   slot +0x24 de la vtable 0x188D50
 *
 *   Les deux : thiscall, 1 argument (pointeur vers le mot de commande, 4
 *   octets, mis à 0 puis rempli par l'original), ret 4. Ce sont des THUNKS
 *   d'ajustement (`sub ecx,[ecx-4]` puis jmp 0x5AA30 / 0x48AB0) : le ecx
 *   d'ENTRÉE (celui que lit le hook) est le pointeur du sous-objet Rider
 *   AVANT ajustement, c'est-à-dire exactement la valeur rangée dans
 *   race+0xC4[i] (vérifié au runtime : esi au site d'appel = ces
 *   entrées). Seules références : leurs vtables -> lookup_manual voit
 *   100 % des appels.
 *   Règles : lire ecx et l'argument AVANT l'original (il dépile 4 octets) ;
 *   après l'original, lire le mot ; g_eax/g_ecx/g_edx restaurés autour du
 *   journal ; pile contrôlée (entrée + 4 + 4, sinon compteur esp_bad).
 *
 * ── Mesuré
 *   Thread : tous les appels arrivent sur UN thread, celui de RenderFrame
 *     pendant la course (vérifié avec XBOX_PASS_TAGS=log en parallèle) : l'update
 *     physique et le rendu sont séquentiels sur le thread du jeu.
 *   Tick : 1 appel par rider et par tick ; race+0x18 avance de 1 par tick (FAIT).
 *   U2 : le jeu RATTRAPE. La simulation tient 60 ticks/s de temps réel quand
 *     l'affichage ralentit (plusieurs ticks entre deux Present, jamais un
 *     Present sans tick) ; sous très forte charge (16 Present/s) elle tombe à
 *     ~52 ticks/s (jusqu'à 10 ticks par Present observés).

 * ── Offsets utilisés (sous-objet Rider sauf mention)
 *
 *   objet course  race = [[0x1E3C7C]+0x72C]+0x1C                     FAIT
 *     race+0x88   nombre de riders                                   FAIT (runtime 6)
 *     race+0xC4[] pointeurs des sous-objets Rider                    FAIT
 *     race+0x7C / +0x80  nombre de Players / d'IA                    FAIT (runtime 1/5)
 *     race+0x18   compteur de frames de course                       DÉDUIT (vérifié par l'outil)
 *     race+0x1C   état de course (4 = Race)                          DÉDUIT
 *   roster 0x1DE900, pas 0x98, nombre [0x1DE8FC]                     FAIT
 *   graines RNG : [0x1DEC98] (par course), [0x1DEC9C] (chargement)   FAIT
 *   piste [0x1DEC90], mode de jeu [0x1DEC94]                         DÉDUIT (notes upstream)
 *   rider+0x170..+0x178  position                                    FAIT (notes + runtime)
 *   rider+0x180..+0x188  vitesse                                     FAIT (notes)
 *   rider+0x458  état / mode du rider : clé du switch des deux
 *                générateurs de commande ET de 0x31640              FAIT
 *   rider+0x15C  facteur de vitesse (écrit par l'IA)                 FAIT écriture / DÉDUIT rôle
 *   clé de 0x31640 : lue exactement comme 0x31640 la lit, m = [rider+0x58E0],
 *     key = [m + [[m+0x30]+4] + 0x488]. CORRECTION (une première lecture disait
 *     « mode physique rider+0x488 ») : l'offset 0x488 est relatif à
 *     l'objet complet (base virtuelle à +0x30), donc la clé est
 *     rider+0x458 ; le drapeau NPCL_F_MODE_IS_458 le vérifie à chaque
 *     enregistrement (100 % des 116 280 enregistrements d'une course mesurée).
 *
 * ── Format du fichier .npcl (petit-boutiste, tout en uint32 sauf mention)
 *
 *   En-tête :
 *     magic 'NPCL' (0x4C43504E), version (1), taille de l'en-tête en octets,
 *     taille d'un enregistrement (56), piste, mode de jeu, graine 0x1DEC98,
 *     graine 0x1DEC9C, adresse de l'objet course, nombre de riders n (<= 16),
 *     nombre de Players, nombre d'IA, n pointeurs de sous-objets Rider
 *     (race+0xC4[]), nombre d'entrées de roster r (<= 16), r x 0x98 octets
 *     bruts du roster, 2 dwords de réserve (0). Taille = 4 x (15 + n) + 0x98 x r.
 *   Puis des enregistrements de 56 octets (struct npcl_record ci-dessous),
 *   un par appel de commande, dans l'ordre des appels.
 *   v3 : à chaque changement de race+0x1C, au premier appel de commande du
 *   tick et AVANT l'original, deux enregistrements « RNG » : kind
 *   NPCL_KIND_RNG_A / NPCL_KIND_RNG_B, idx 0xFE, cmd = race_state, et les 6
 *   dwords de l'état RNG dans pos[0..2] puis vel[0..2] (bits bruts). Les
 *   outils doivent ignorer idx 0xFE dans les statistiques par rider.
 *
 *   tick : compteur propre au journal, +1 quand un rider déjà vu dans le tick
 *   courant redemande sa commande (donc un tick = un passage de
 *   Rider_UpdatePhysicsState sur les riders). present : d3d8_PresentSeq() au
 *   moment de l'appel (sert à la mesure U2). Nouveau fichier quand l'objet
 *   course change ou que race+0x18 recule.
 *
 *   Outil : port/tools/np_cmdlog_dump.py (CSV + PNG des trajectoires + U2).
 */
#ifndef NP_CMDLOG_H
#define NP_CMDLOG_H

#include <stdint.h>

#define NPCL_MAGIC        0x4C43504Eu   /* "NPCL" */
#define NPCL_VERSION      3u   /* 2 : phys_mode = clé exacte de 0x31640 ; 3 : événements RNG */
#define NPCL_MAX_RIDERS   16u
#define NPCL_ROSTER_STRIDE 0x98u

/* kind */
#define NPCL_KIND_PLAYER  0u
#define NPCL_KIND_AI      1u
#define NPCL_KIND_RNG_A   2u   /* v3 : état RNG global (0x1FAD70) */
#define NPCL_KIND_RNG_B   3u   /* v3 : état RNG « course » (0x1FAD88) */

#define NPCL_RNG_A_VA     0x001FAD70u   /* 6 dwords, semé à l'horloge au boot */
#define NPCL_RNG_B_VA     0x001FAD88u   /* 6 dwords, re-semé par course depuis [0x1DEC98] */
/* flags */
#define NPCL_F_MODE_IS_458 0x01u        /* la clé de 0x31640 est bien rider+0x458 */
#define NPCL_F_IDX_UNKNOWN 0x02u        /* rider absent de race+0xC4[] */

#pragma pack(push, 1)
typedef struct {
    uint32_t tick;          /* compteur du journal (voir en-tête) */
    uint32_t race_frame;    /* race+0x18 */
    uint32_t present;       /* d3d8_PresentSeq() */
    uint8_t  idx;           /* index dans race+0xC4[], 0xFF si inconnu */
    uint8_t  kind;          /* NPCL_KIND_* (selon le hook) */
    uint8_t  flags;         /* NPCL_F_* */
    uint8_t  race_state;    /* race+0x1C (octet bas) */
    uint32_t cmd;           /* mot de commande après l'original */
    float    pos[3];        /* rider+0x170 */
    float    vel[3];        /* rider+0x180 */
    uint32_t ev_state;      /* rider+0x458 */
    float    speed_factor;  /* rider+0x15C */
    uint32_t phys_mode;     /* clé du switch de 0x31640 (= rider+0x458) */
} npcl_record;

/* Instantanés d'état du Player, fichier .npst à côté du .npcl
 * (XBOX_NETLOG_STATE=1). En-tête : magic 'NPST', version 1, taille d'un
 * bloc (NPST_BLOCK), index du Player dans race+0xC4[]. Puis, à chaque tick
 * où le Player demande sa commande (même point que les enregistrements
 * .npcl, après l'original) : npst_record puis NPST_BLOCK octets bruts du
 * sous-objet Rider, à partir de son offset 0. Sert aux corrections d'état
 * du ghost (np_ghost.h, XBOX_GHOST_STATE) et à l'inventaire des champs. */
#define NPST_MAGIC   0x5453504Eu   /* "NPST" */
#define NPST_VERSION 1u
#define NPST_BLOCK   0x5A00u
typedef struct {
    uint32_t race_frame;    /* race+0x18 */
    uint32_t race_state;    /* race+0x1C */
} npst_record;
#pragma pack(pop)

typedef char npcl_record_size_check[sizeof(npcl_record) == 56 ? 1 : -1];

extern int g_np_cmdlog_on;

void np_cmdlog_init(void);
void (*np_cmdlog_lookup(uint32_t xbox_va))(void);

#endif /* NP_CMDLOG_H */
