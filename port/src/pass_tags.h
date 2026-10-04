/*
 * pass_tags -- étiquettes de passes de rendu (fork).
 *
 * Observe les passes de rendu du moteur SSX par trois hooks posés via
 * recomp_lookup_manual(), et les transforme en événements consommés par des
 * « sinks » : le sink JOURNAL (rien n'est écrit dans le pushbuffer) et le
 * sink EMIT (marqueurs NOP dans le flux).
 *
 * ── Modes (variable d'environnement XBOX_PASS_TAGS, lue une fois au démarrage)
 *
 *   absent, "0", "off"  OFF (défaut). recomp_lookup_manual() ne renvoie AUCUN
 *                       des hooks : le chemin d'origine est strictement intact,
 *                       le seul coût est un test d'entier dans lookup_manual.
 *   "log"               JOURNAL. Les 3 hooks sont actifs, les événements vont
 *                       dans _local/logs/pass_tags_<AAAAMMJJ_HHMMSS>.log.
 *   "emit"              ÉMISSION. Les hooks écrivent des marqueurs
 *                       NOP dans le pushbuffer (sink EMIT ci-dessous) ; le
 *                       traducteur les lit (nv2a_pgraph_d3d11.c, phases).
 *                       Aucun effet visuel. Bilan sur stderr toutes les
 *                       ~600 frames ("[PASS-TAGS] emit : ...").
 *   "log+emit"          les deux (aussi "emit+log").
 *
 * Réglages du journal (facultatifs) :
 *   XBOX_PASS_TAGS_DIR     dossier du journal. Défaut : le dossier « _local »
 *                          le plus haut en remontant depuis l'exe, + /logs ;
 *                          sinon le dossier de l'exe.
 *   XBOX_PASS_TAGS_FULL    nombre de premières frames journalisées en entier
 *                          (défaut 300).
 *   XBOX_PASS_TAGS_EVERY   ensuite une frame sur N (défaut 60, ~1/s), plus
 *                          toute frame dont la séquence est nouvelle (signature
 *                          absente des 16 dernières signatures journalisées).
 *   XBOX_PASS_TAGS_MAXMB   plafond de taille du fichier (défaut 64 Mo). Au-delà,
 *                          plus de détail, seuls les bilans continuent.
 *
 * ── Les hooks (conventions vérifiées dans les octets de la XBE)
 *
 *   H1 0x00105CE0 SceneRenderer_RenderFrame      thiscall, 0 arg, ret
 *        -> FRAME_BEGIN (extra = reflet prévu 0/1) ... FRAME_END
 *   H2 0x000FAAE0 GfxContext_ApplyStateBlock     thiscall, 1 arg, ret 4
 *        -> GROUP (vue, groupe, ortho, handler) quand le triplet change ;
 *           GROUP_END (extra = nb d'enregistrements) quand il se termine.
 *           Filtre : seul le site de la boucle d'enregistrements de
 *           SceneView_RenderPass est retenu (rec = blk-4 doit être un
 *           enregistrement valide de la vue courante).
 *   H3 0x000FE940 GfxContext_SetOrthographicViewAndApply  thiscall, 7 args, ret 0x1C
 *        -> ORTHO (gauche, haut, largeur, hauteur, near, far, flag) quand les
 *           arguments changent. Remis à zéro par H1, comme le triplet de H2.
 *
 *   Règles communes à tout hook (impératives) :
 *     1. lire TOUS les arguments avant d'appeler l'original (il dépile) ;
 *     2. appeler l'original exactement une fois, avec g_ecx intact ;
 *     3. ne pas modifier g_eax / g_edx / g_ecx après l'appel (ils sont
 *        sauvegardés et restaurés autour des sinks par précaution) ;
 *     4. un seul thread « propriétaire » est journalisé : celui qui entre dans
 *        H1 (le jeu rend depuis deux threads successifs, démarrage puis boucle
 *        principale). Les appels d'un autre thread sont comptés et passent
 *        tout droit vers l'original. L'état est sous verrou (jamais tenu
 *        pendant l'appel de l'original).
 *   Contrôle de pile : à l'entrée g_esp pointe sur l'adresse de retour fictive ;
 *   après l'original il doit valoir entrée + 4 + octets dépilés. Tout écart est
 *   compté (« esp_bad » dans les bilans du journal).
 *
 * ── Ajouter un hook
 *   1. Vérifier la convention dans les OCTETS (jamais l'en-tête « CC: » de gen/,
 *      faux par défaut) et que la fonction n'est atteinte que par des appels
 *      indirects (sinon lookup_manual ne la voit pas).
 *   2. Écrire hook_XXXXXXXX() dans pass_tags.c sur le modèle de hook_FE940 :
 *      lire les arguments, pt_dispatch() un événement, appeler sub_XXXXXXXX(),
 *      vérifier g_esp.
 *   3. L'ajouter dans pass_tags_lookup() ; au besoin un nouveau PT_EV_*.
 *
 * ── Ajouter un sink
 *   Remplir un pass_tags_sink (nom, event, flush) et l'enregistrer par
 *   pass_tags_add_sink() pendant pass_tags_init(). event() est appelé sur le
 *   thread du jeu, dans le hook : il doit être court et ne pas toucher aux
 *   registres guest (sauf le sink EMIT, voir ci-dessous).
 *
 * ── Sink EMIT (pass_tags.c emit_event)
 *   Pour FRAME_BEGIN, GROUP et FRAME_END (pas ORTHO ni GROUP_END), seulement
 *   sur le thread propriétaire et dans une frame H1, écrire dans le
 *   pushbuffer `0x00040100, tag` (NV097_NO_OPERATION, 1 paramètre) :
 *     ctx = MEM32(0x001776C0) ; refuser si ctx == 0 ou MEM32(ctx+0xC) & 4
 *     (enregistrement de pushbuffer) ; appeler sub_0016B920(ctx) (stdcall,
 *     ret 4, garantit wp < limite avec 0x200 de marge) ; wp = g_eax ;
 *     MEM32(wp) = 0x00040100 ; MEM32(wp+4) = tag ; MEM32(ctx) = wp + 8 ;
 *     sauvegarder/restaurer g_eax, g_ecx, g_edx autour.
 *   Les événements GROUP/ORTHO/FRAME_BEGIN sont livrés AVANT l'original (le
 *   marqueur précède donc les commandes de la passe), FRAME_END après.
 *   Le traducteur (nv2a_pgraph_d3d11.c, case NV097_NO_OPERATION) en tire la
 *   phase de rendu courante, état absolu donc tolérant aux pertes : voir le
 *   commentaire « Pass phases » du traducteur. Les frames rendues par le
 *   second thread (chargements) n'ont pas de marqueurs.
 *
 * ── Format du tag 32 bits (paramètre du NOP)
 *
 *   31            16 15 14 13 12      10 9        5 4       0
 *   +---------------+-----+--+----------+----------+---------+
 *   | magic 0x5358  |type |o | vue (3)  | groupe(5)| cpt (5) |   GROUP / ORTHO
 *   +---------------+-----+--+----------+----------+---------+
 *   | magic 0x5358  |type |r |  n° de frame modulo 8192 (13) |   FRAME_BEGIN / END
 *   +---------------+-----+--+-------------------------------+
 *
 *   magic  0x5358 (« SX ») dans les 16 bits hauts.
 *   type   0 FRAME_BEGIN, 1 FRAME_END, 2 GROUP, 3 ORTHO.
 *   o      1 si la vue est orthographique (FOV view+0x80C == 0.0) ; toujours 1
 *          pour ORTHO.
 *   r      FRAME_BEGIN : reflet prévu ; FRAME_END : 0.
 *   vue    index de vue 0..5 ; 7 = aucune / inconnue.
 *   groupe groupe de passe 0..0x17 (rec+0xC >> 27) ; 0 pour ORTHO.
 *   cpt    rang de l'événement dans la frame modulo 32 (détecte pertes et
 *          désordre dans le flux).
 *   GROUP_END n'a pas de tag (pass_tags_tag() renvoie 0).
 */
#ifndef PASS_TAGS_H
#define PASS_TAGS_H

#include <stdint.h>

#define PASS_TAG_MAGIC        0x5358u
#define PASS_TAG_IS(p)        (((uint32_t)(p) >> 16) == PASS_TAG_MAGIC)
#define PASS_TAG_TYPE(p)      (((uint32_t)(p) >> 14) & 3u)
#define PASS_TAG_VIEW_NONE    7u

enum pass_tags_mode {
    PASS_TAGS_OFF  = 0,
    PASS_TAGS_LOG  = 1,
    PASS_TAGS_EMIT = 2          /* emit ou log+emit */
};

enum pass_tag_type {
    PT_EV_FRAME_BEGIN = 0,
    PT_EV_FRAME_END   = 1,
    PT_EV_GROUP       = 2,
    PT_EV_ORTHO       = 3,
    PT_EV_GROUP_END   = 4       /* journal seulement, pas de tag */
};

typedef struct pass_tag_event {
    uint32_t seq_frame;         /* n° de frame (incrémenté à chaque H1) */
    uint32_t seq_event;         /* rang de l'événement dans la frame */
    uint32_t type;              /* enum pass_tag_type */
    uint32_t view;              /* 0..5, PASS_TAG_VIEW_NONE sinon */
    uint32_t group;             /* groupe de passe 0..0x17 */
    uint32_t ortho;             /* vue ortho (FOV == 0) */
    uint32_t handler_va;        /* GROUP : handler [[rec]] du 1er enregistrement */
    uint32_t extra;             /* FRAME_BEGIN : reflet ; GROUP_END : nb d'enr. ;
                                 * FRAME_END : nb d'enr. retenus dans la frame ;
                                 * ORTHO : flag (7e argument) */
    float    ortho_args[6];     /* ORTHO : gauche, haut, largeur, hauteur, near, far */
    uint32_t thread;            /* id Windows du thread propriétaire */
} pass_tag_event;

typedef struct pass_tags_sink {
    const char *name;
    void (*event)(const pass_tag_event *ev);
    void (*flush)(void);        /* facultatif, appelé ~ toutes les 60 frames */
} pass_tags_sink;

/* Tag 32 bits d'un événement, 0 s'il n'en a pas (GROUP_END). */
static inline uint32_t pass_tags_tag(const pass_tag_event *ev)
{
    uint32_t t = PASS_TAG_MAGIC << 16;
    switch (ev->type) {
    case PT_EV_FRAME_BEGIN:
    case PT_EV_FRAME_END:
        return t | (ev->type << 14)
                 | ((ev->type == PT_EV_FRAME_BEGIN && ev->extra) ? 1u << 13 : 0u)
                 | (ev->seq_frame & 0x1FFFu);
    case PT_EV_GROUP:
    case PT_EV_ORTHO:
        return t | (ev->type << 14) | ((ev->ortho & 1u) << 13)
                 | ((ev->view & 7u) << 10) | ((ev->group & 0x1Fu) << 5)
                 | (ev->seq_event & 0x1Fu);
    default:
        return 0;
    }
}

/* Lit XBOX_PASS_TAGS et prépare les sinks. À appeler une fois, au démarrage,
 * avant que le jeu ne tourne. */
void pass_tags_init(void);

/* Mode actif. Lu par recomp_lookup_manual() : en OFF les hooks n'existent pas. */
extern int g_pass_tags_mode;

/* Hook pour cette VA, ou NULL. N'appeler que si g_pass_tags_mode != OFF. */
void (*pass_tags_lookup(uint32_t xbox_va))(void);

/* Enregistre un sink (au plus 4). Renvoie 0 si la table est pleine. */
int pass_tags_add_sink(const pass_tags_sink *s);

#endif /* PASS_TAGS_H */
