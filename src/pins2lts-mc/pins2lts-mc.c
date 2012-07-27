#include <config.h>
#include <assert.h>
#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <popt.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>

#include <atomics.h>
#include <cctables.h>
#include <dbs-ll.h>
#include <dfs-stack.h>
#include <dm/bitvector.h>
#include <fast_hash.h>
#include <is-balloc.h>
#include <lmap.h>
#include <lb2.h>
#include <ltsmin-tl.h>
#include <hre/user.h>
#include <spec-greybox.h>
#include <statistics.h>
#include <stats.h>
#include <trace.h>
#include <treedbs-ll.h>
#include <unix.h>
#include <zobrist.h>

static inline size_t min (size_t a, size_t b) {
    return a < b ? a : b;
}

#define                     MAX_STRATEGIES 5
typedef int                *state_data_t;
static const state_data_t   state_data_dummy;
static const size_t         SLOT_SIZE = sizeof(*state_data_dummy);
typedef int        *raw_data_t;
typedef struct state_info_s {
    state_data_t        data;
    tree_t              tree;
    ref_t               ref;
    hash64_t            hash64;
    lattice_t           lattice;
    lm_loc_t            loc;
} state_info_t;

typedef enum { UseGreyBox, UseBlackBox } box_t;

typedef enum {
    HashTable   = 1,
    TreeTable   = 2,
    ClearyTree  = 4,
    Tree        = TreeTable | ClearyTree
} db_type_t;

typedef enum {
    Strat_None   = 0,
    Strat_SBFS   = 1,
    Strat_BFS    = 2,
    Strat_DFS    = 4,
    Strat_NDFS   = 8,
    Strat_NNDFS  = 16,
    Strat_LNDFS  = 32,
    Strat_ENDFS  = 64,
    Strat_CNDFS  = 128,
    Strat_TA     = 256,
    Strat_TA_SBFS= Strat_SBFS | Strat_TA,
    Strat_TA_BFS = Strat_BFS | Strat_TA,
    Strat_TA_DFS = Strat_DFS | Strat_TA,
    Strat_2Stacks= Strat_BFS | Strat_SBFS | Strat_CNDFS | Strat_ENDFS,
    Strat_LTLG   = Strat_LNDFS | Strat_ENDFS | Strat_CNDFS,
    Strat_LTL    = Strat_NDFS | Strat_NNDFS | Strat_LTLG,
    Strat_Reach  = Strat_BFS | Strat_SBFS | Strat_DFS
} strategy_t;

/* permute_get_transitions is a replacement for GBgetTransitionsLong
 * TODO: move this to permute.c
 */
#define                     TODO_MAX 20

typedef enum {
    Perm_None,      /* normal group order */
    Perm_Shift,     /* shifted group order (lazy impl., thus cheap) */
    Perm_Shift_All, /* eq. to Perm_Shift, but non-lazy */
    Perm_Sort,      /* order on the state index in the DB */
    Perm_Random,    /* generate a random fixed permutation */
    Perm_RR,        /* more random */
    Perm_SR,        /* sort according to a random fixed permutation */
    Perm_Otf,       /* on-the-fly calculation of a random perm for num_succ */
    Perm_Dynamic,   /* generate a dynamic permutation based on color feedback */
    Perm_Unknown    /* not set yet */
} permutation_perm_t;

typedef enum ta_update_e {
    TA_UPDATE_NONE = 0,
    TA_UPDATE_WAITING = 1,
    TA_UPDATE_PASSED = 2,
} ta_update_e_t;

typedef struct permute_todo_s {
    state_info_t        si;
    transition_info_t   ti;
    int                 seen;
} permute_todo_t;

typedef void            (*perm_cb_f)(void *context, state_info_t *dst,
                                     transition_info_t *ti, int seen);

typedef struct permute_s {
    void               *ctx;    /* GB context */
    int               **rand;   /* random permutations */
    int                *pad;    /* scratch pad for otf and dynamic permutation */
    perm_cb_f           real_cb;            /* GB callback */
    state_info_t       *state;              /* the source state */
    double              shift;              /* distance in group-based shift */
    uint32_t            shiftorder;         /* shift projected to ref range*/
    int                 start_group;        /* fixed index of group-based shift*/
    int                 start_group_index;  /* recorded index higher than start*/
    permute_todo_t     *todos;  /* records states that require late permutation */
    int                *tosort; /* indices of todos */
    size_t              nstored;/* number of states stored in to-do */
    size_t              trans;  /* number of transition groups */
    permutation_perm_t  permutation;        /* kind of permuation */
    model_t             model;  /* GB model */
} permute_t;

/**
 * Create a permuter.
 * arguments:
 * permutation: see permutation_perm_t
 * shift: distance between shifts
 */
extern permute_t       *permute_create (permutation_perm_t permutation, model_t model,
                                        size_t workers, size_t trans, int worker_index);
extern void             permute_free (permute_t *perm);
extern int              permute_trans (permute_t *perm, state_info_t *state,
                                       perm_cb_f cb, void *ctx);

static char            *files[2];
static int              dbs_size = 0;
static int              refs = 1;
static int              no_red_perm = 0;
static int              all_red = 1;
static box_t            call_mode = UseBlackBox;
static size_t           max = SIZE_MAX;
static size_t           ratio = 2;
static size_t           W = -1;
static lb2_t           *lb2 = NULL;
static void            *dbs = NULL;
static lm_t            *lmap = NULL;
static dbs_stats_f      statistics;
static dbs_get_f        get;
static dbs_get_sat_f    get_sat_bit;
static dbs_unset_sat_f  unset_sat_bit;
static dbs_try_set_sat_f try_set_sat_bit;
static dbs_inc_sat_bits_f   inc_sat_bits;
static dbs_dec_sat_bits_f   dec_sat_bits;
static dbs_get_sat_bits_f   get_sat_bits;
static char            *state_repr = "tree";
static db_type_t        db_type = TreeTable;
#ifdef OPAAL
static char            *arg_strategy = "sbfs";
#else
static char            *arg_strategy = "bfs";
#endif
static strategy_t       strategy[MAX_STRATEGIES] = {Strat_BFS, Strat_None, Strat_None, Strat_None, Strat_None};
//static char            *arg_lb = "combined";
static char            *arg_perm = "unknown";
static permutation_perm_t permutation = Perm_Unknown;
static permutation_perm_t permutation_red = Perm_Unknown;
static char*            trc_output = NULL;
static int              dlk_detect = 0;
static char            *act_detect = NULL;
static char            *inv_detect = NULL;
static int              assert_detect = 0;
static int              no_exit = 0;
static int              act_index = -1;
static int              act_type = -1;
static ltsmin_expr_t    inv_expr = NULL;
static size_t           G = 1;
static size_t           H = 1000;
static int              ZOBRIST = 0;
static int              LATTICE_BLOCK_SIZE = (1UL<<CACHE_LINE) / sizeof(lattice_t);
static int              UPDATE = TA_UPDATE_WAITING;
static int              BACKOFF = 0;
static int              STDEV = 0;
static ref_t           *parent_ref = NULL;
static state_data_t     initial_data;
static state_info_t     initial_state;
static int              count_bits = 0;
static int              global_bits = 0;
static int              local_bits = 0;
static int              count_mask;
static size_t           max_level_size = 0;


static si_map_entry strategies[] = {
    {"bfs",     Strat_BFS},
    {"dfs",     Strat_DFS},
    {"sbfs",    Strat_SBFS},
#ifndef OPAAL
    {"ndfs",    Strat_NDFS},
    {"nndfs",   Strat_NNDFS},
    {"lndfs",   Strat_LNDFS},
    {"endfs",   Strat_ENDFS},
    {"cndfs",   Strat_CNDFS},
#endif
    {NULL, 0}
};

static si_map_entry permutations[] = {
    {"shift",   Perm_Shift},
    {"shiftall",Perm_Shift_All},
    {"sort",    Perm_Sort},
    {"otf",     Perm_Otf},
    {"random",  Perm_Random},
    {"rr",      Perm_RR},
    {"sr",      Perm_SR},
    {"none",    Perm_None},
    {"dynamic", Perm_Dynamic},
    {"unknown", Perm_Unknown},
    {NULL, 0}
};

static si_map_entry db_types[] = {
    {"table",   HashTable},
    {"tree",    TreeTable},
    {"cleary-tree", ClearyTree},
    {NULL, 0}
};

static void
state_db_popt (poptContext con, enum poptCallbackReason reason,
               const struct poptOption *opt, const char *arg, void *data)
{
    int                 res;
    switch (reason) {
    case POPT_CALLBACK_REASON_PRE:
        break;
    case POPT_CALLBACK_REASON_POST:
        res = linear_search (db_types, state_repr);
        if (res < 0)
            Abort ("unknown vector storage mode type %s", state_repr);
        db_type = res;
        int i = 0, begin = 0, end = 0;
        char *strat = strdup (arg_strategy);
        char last;
        do {
            if (i > 0 && Strat_ENDFS != strategy[i-1])
                Abort ("Only ENDFS supports recursive repair procedures.");
            while (',' != arg_strategy[end] && '\0' != arg_strategy[end]) ++end;
            last = strat[end];
            strat[end] = '\0';
            res = linear_search (strategies, &strat[begin]);
            if (res < 0)
                Abort ("unknown search strategy %s", &strat[begin]);
            strategy[i++] = res;
            end += 1;
            begin = end;
        } while ('\0' != last && i < MAX_STRATEGIES);
        free (strat);
        if (Strat_ENDFS == strategy[i-1]) {
            if (MAX_STRATEGIES == i)
                Abort ("Open-ended recursion in ENDFS repair strategies.");
            Warning (info, "Defaulting to NNDFS as ENDFS repair procedure.");
            strategy[i] = Strat_NNDFS;
        }
        res = linear_search (permutations, arg_perm);
        if (res < 0)
            Abort ("unknown permutation method %s", arg_perm);
        permutation = res;
        return;
    case POPT_CALLBACK_REASON_OPTION:
        break;
    }
    Abort ("unexpected call to state_db_popt");
    (void)con; (void)opt; (void)arg; (void)data;
}

static void
exit_ltsmin (int sig)
{
    if ( !lb2_stop(lb2) ) {
        Abort ("UNGRACEFUL EXIT");
    } else {
        Warning(info, "PREMATURE EXIT (caught signal: %d)", sig);
    }
}

static struct poptOption options[] = {
    {NULL, 0, POPT_ARG_CALLBACK | POPT_CBFLAG_POST | POPT_CBFLAG_SKIPOPTION,
     (void *)state_db_popt, 0, NULL, NULL},
    {"state", 0, POPT_ARG_STRING | POPT_ARGFLAG_SHOW_DEFAULT, &state_repr, 0,
      "select the data structure for storing states. Beware for Cleary tree: size <= 28 + 2 * ratio.", "<tree|table|cleary-tree>"},
    {"size", 's', POPT_ARG_INT | POPT_ARGFLAG_SHOW_DEFAULT, &dbs_size, 0,
     "log2 size of the state store", NULL},
#ifdef OPAAL
    {"lattice-blocks", 'l', POPT_ARG_INT | POPT_ARGFLAG_SHOW_DEFAULT, &LATTICE_BLOCK_SIZE, 0,
      "Size of blocks preallocated for lattices (> 1). "
         "Small blocks save memory when most states few lattices (< 4). "
         "Larger blocks save memory in case a few states have many lattices. "
         "For the best performance set this to: cache line size (usually 64) divided by lattice size of 8 byte.", NULL},
    {"update", 'u', POPT_ARG_INT | POPT_ARGFLAG_SHOW_DEFAULT, &UPDATE,
      0,"cover update strategy: 0 = simple, 1 = update waiting, 2 = update passed (may break traces)", NULL},
    {"backoff", 'b', POPT_ARG_VAL, &BACKOFF, 1, "Back-off algorithm for TA exploration", NULL},
    {"stdev", 0, POPT_ARG_VAL, &STDEV, 1, "Calculate the standard deviation of lattice ratio", NULL},
    {"strategy", 0, POPT_ARG_STRING | POPT_ARGFLAG_SHOW_DEFAULT,
     &arg_strategy, 0, "select the search strategy", "<sbfs|bfs|dfs>"},
#else
    {"strategy", 0, POPT_ARG_STRING | POPT_ARGFLAG_SHOW_DEFAULT,
     &arg_strategy, 0, "select the search strategy", "<bfs|sbfs|dfs|cndfs|lndfs|endfs|endfs,lndfs|endfs,endfs,nndfs|ndfs|nndfs>"},
    {"no-red-perm", 0, POPT_ARG_VAL, &no_red_perm, 1, "turn off transition permutation for the red search", NULL},
    {"nar", 1, POPT_ARG_VAL, &all_red, 0, "turn off red coloring in the blue search (NNDFS/MCNDFS)", NULL},
    {"grey", 0, POPT_ARG_VAL, &call_mode, UseGreyBox, "make use of GetTransitionsLong calls", NULL},
    {"max", 0, POPT_ARG_LONGLONG | POPT_ARGFLAG_SHOW_DEFAULT, &max, 0, "maximum search depth", "<int>"},
#endif
    {"perm", 'p', POPT_ARG_STRING | POPT_ARGFLAG_SHOW_DEFAULT,
     &arg_perm, 0, "select the transition permutation method",
     "<dynamic|random|rr|sort|sr|shift|shiftall|otf|none>"},
    {"gran", 'g', POPT_ARG_LONGLONG | POPT_ARGFLAG_SHOW_DEFAULT, &G, 0,
     "subproblem granularity ( T( work(P,g) )=min( T(P), g ) )", NULL},
    {"handoff", 0, POPT_ARG_LONGLONG | POPT_ARGFLAG_SHOW_DEFAULT, &H, 0,
     "maximum balancing handoff (handoff=min(max, stack_size/2))", NULL},
    {"zobrist", 'z', POPT_ARG_INT | POPT_ARGFLAG_SHOW_DEFAULT, &ZOBRIST, 0,
     "log2 size of zobrist random table (6 or 8 is good enough; 0 is no zobrist)", NULL},
    {"noref", 0, POPT_ARG_VAL, &refs, 0, "store full states on the stack/queue instead of references (faster)", NULL},
    {"ratio", 0, POPT_ARG_LONGLONG | POPT_ARGFLAG_SHOW_DEFAULT, &ratio, 0, "log2 tree root to leaf ratio", "<int>"},
    {"deadlock", 'd', POPT_ARG_VAL, &dlk_detect, 1, "detect deadlocks", NULL },
    {"action", 'a', POPT_ARG_STRING, &act_detect, 0, "detect error action", NULL },
    {"invariant", 'i', POPT_ARG_STRING, &inv_detect, 0, "detect invariant violations", NULL },
#ifdef SPINJA
    {"assert", 0, POPT_ARG_VAL, &assert_detect, 1, "detect assertion errors (SpinJa). Same as --action=assert", NULL },
#endif
    {"no-exit", 'n', POPT_ARG_VAL, &no_exit, 1, "no exit on error, just count (for error counters use -v)", NULL },
    {"trace", 0, POPT_ARG_STRING, &trc_output, 0, "file to write trace to", "<lts output>" },
    SPEC_POPT_OPTIONS,
    {NULL, 0, POPT_ARG_INCLUDE_TABLE, greybox_options, 0, "Greybox options", NULL},
    POPT_TABLEEND
};

/* TODO: move to color.c
 * NNDFS state colors are encoded using one bitset, where two consecutive bits
 * describe four colors: (thus state 0 uses bit 0 and 1, state 1, 2 and 3, ..)
 * Colors:
 * While: 0 0) first generated by next state call
 * Blue:  0 1) A state that has finished its blue search and has not yet been reached
 *             in a red search
 * Pink:  1 0) A state that has been considered in both the blue and the red search
 * Cyan:  1 1) A state whose blue search has not been terminated
 *
 * In MC-NDFS, Pink states are the ones on the stack of dfs_red
 *
 */
typedef struct {
  int nn;
} nndfs_color_t;

enum { WHITE=0, BLUE=1, PINK=2, CYAN=3 };
#define NNCOLOR(c) (nndfs_color_t){ .nn = (c) }
#define NNWHITE    NNCOLOR(WHITE) // value: 00
#define NNBLUE     NNCOLOR(BLUE)  // value: 01
#define NNPINK     NNCOLOR(PINK)  // value: 10
#define NNCYAN     NNCOLOR(CYAN)  // value: 11

static inline nndfs_color_t
nn_get_color (bitvector_t *set, ref_t ref)
{ return (nndfs_color_t){ .nn = bitvector_get2 (set, ref<<1) };  }

static inline int
nn_set_color (bitvector_t *set, ref_t ref, nndfs_color_t color)
{ return bitvector_isset_or_set2 (set, ref<<1, color.nn); }

static inline int
nn_color_eq (const nndfs_color_t a, const nndfs_color_t b)
{ return a.nn == b.nn; };

/* NDFS uses two colors which are independent of each other.
 * Blue: bit 0) A state that has finished its blue search and has not yet been reached
 *             in a red search
 * Red:  bit 1) A state that has been considered in both the blue and the red search
 */
typedef struct {
  int n;
} ndfs_color_t;

enum { IBLUE=0, IRED=1 };
#define NCOLOR(c)  (ndfs_color_t){ .n = (c) }
#define NBLUE      NCOLOR(IBLUE) // bit 0
#define NRED       NCOLOR(IRED)  // bit 1

static inline int
ndfs_has_color (bitvector_t *set, ref_t ref, ndfs_color_t color)
{ return bitvector_is_set (set, (ref<<1)|color.n); }

static inline int
ndfs_try_color (bitvector_t *set, ref_t ref, ndfs_color_t color)
{ return bitvector_isset_or_set (set, (ref<<1)|color.n); }

/* Global state colors are encoded in the state database as independent bits.
 * All threads are sensitive too them.
 *
 * Colors:
 * Green: bit 0) A state that globally is does not have to be considered in
 *               the blue search anymore.
 * Yellow:bit 1) A state that globally is does not have to be considered in
 *               the Red search anymore.
 */
typedef struct {
  int g;
} global_color_t;

enum { RED=0, GREEN=1, DANGEROUS=2 };
#define GCOLOR(c)  (global_color_t){ .g = (c) }
#define GRED       GCOLOR(RED)      // bit 0
#define GGREEN     GCOLOR(GREEN)    // bit 1
#define GDANGEROUS GCOLOR(DANGEROUS)// bit 2

static inline int
global_has_color (ref_t ref, global_color_t color, int rec_bits)
{
    return get_sat_bit (dbs, ref, rec_bits+count_bits+color.g);
}

static inline int //RED and BLUE are independent
global_try_color (ref_t ref, global_color_t color, int rec_bits)
{
    return try_set_sat_bit (dbs, ref, rec_bits+count_bits+color.g);
}

static inline uint32_t
inc_wip (ref_t ref)
{
    return inc_sat_bits (dbs, ref) & count_mask;
}

static inline uint32_t
dec_wip (ref_t ref)
{
    return dec_sat_bits (dbs, ref) & count_mask;
}

static inline uint32_t
get_wip (ref_t ref)
{
    return get_sat_bits (dbs, ref) & count_mask;
}

typedef struct counter_s {
    double              runtime;        // measured exploration time
    size_t              visited;        // counter: visited states
    size_t              explored;       // counter: explored states
    size_t              allred;         // counter: allred states
    size_t              trans;          // counter: transitions
    size_t              level_max;      // counter: (BFS) level / (DFS) max level
    size_t              level_cur;      // counter: current (DFS) level
    size_t              threshold;      // report threshold
    size_t              waits;          // number of waits for WIP
    size_t              bogus_red;      // number of bogus red colorings
    size_t              rec;            // recursive ndfss
    size_t              splits;         // Splits by LB
    size_t              transfer;       // load transfered by LB
    size_t              deadlocks;      // deadlock count
    size_t              violations;     // invariant violation count
    size_t              errors;         // assertion error count
    size_t              exit;           // recursive ndfss
    rt_timer_t          timer;
    double              time;
    size_t              deletes;        // lattice deletes
    size_t              updates;        // lattice updates
    size_t              inserts;        // lattice inserts
    size_t              delayed;        // lattice backoff delays
    statistics_t        lattice_ratio;  // On-the-fly calc of stdev/mean of #lat
} counter_t;

typedef struct thread_ctx_s wctx_t;

struct thread_ctx_s {
    strategy_t          strategy;
    size_t              id;             // thread id (0..NUM_THREADS)
    stream_t            out;            // raw file output stream
    model_t             model;          // Greybox model
    state_data_t        store;          // temporary state storage1
    state_data_t        store2;         // temporary state storage2
    state_info_t        state;          // currently explored state
    state_info_t       *successor;      // current successor state
    dfs_stack_t         stack;          // Successor stack (for BFS and DFS)
    dfs_stack_t         in_stack;       // Input stack (for BFS)
    dfs_stack_t         out_stack;      // Output stack (for BFS)
    bitvector_t         color_map;      // Local NDFS coloring of states (ref-based)
    isb_allocator_t     group_stack;    // last explored group per frame (grey)
    counter_t           counters;       // reachability/NDFS_blue counters
    counter_t           red;            // NDFS_red counters
    ref_t               seed;           // current NDFS seed
    permute_t          *permute;        // transition permutor
    bitvector_t         all_red;        // all_red gaiser/Schwoon
    wctx_t             *rec_ctx;        // ctx for Evangelista's ndfs_p
    int                 rec_bits;       // bit depth of recursive ndfs
    ref_t               work;           // ENDFS work for loadbalancer
    int                 done;           // ENDFS done for loadbalancer
    lm_loc_t            last;           // TA last tombstone location
    dfs_stack_t         backoff;        // Backoff stack (for TA)
    rt_timer_t          timer;
    stats_t            *stats;
};

/* predecessor --(transition_info)--> successor */
typedef int         (*find_or_put_f)(state_info_t *successor,
                                     transition_info_t *ti,
                                     state_info_t *predecessor,
                                     state_data_t store);

static const ref_t DUMMY_IDX = SIZE_MAX;

extern size_t state_info_size ();
extern size_t state_info_int_size ();
extern void state_info_create_empty (state_info_t *state);
extern void state_info_create (state_info_t *state, state_data_t data,
                               tree_t tree, ref_t ref);
extern void state_info_serialize (state_info_t *state, raw_data_t data);
extern void state_info_deserialize (state_info_t *state, raw_data_t data,
                                    raw_data_t store);
extern int state_info_initialize (state_info_t *state, state_data_t data,
                                  transition_info_t *ti, state_info_t *src,
                                  wctx_t *ctx);
static find_or_put_f find_or_put;
static size_t       D; // size of state in explicit state DB
static size_t       N; // size of entire state
static size_t       K; // number of groups
static size_t       MAX_SUCC; // max succ. count to expand at once
static size_t       threshold;
static wctx_t     **contexts;
static zobrist_t    zobrist = NULL;

static void
add_results (counter_t *res, counter_t *cnt)
{
    res->runtime += cnt->runtime;
    res->visited += cnt->visited;
    res->explored += cnt->explored;
    res->allred += cnt->allred;
    res->trans += cnt->trans;
    res->level_max += cnt->level_max;
    res->waits += cnt->waits;
    res->rec += cnt->rec;
    res->bogus_red += cnt->bogus_red;
    res->splits += cnt->splits;
    res->transfer += cnt->transfer;
    res->deadlocks += cnt->deadlocks;
    res->violations += cnt->violations;
    res->errors += cnt->errors;
    res->exit += cnt->exit;
    res->time += RTrealTime (cnt->timer);
    res->updates += cnt->updates;
    res->inserts += cnt->inserts;
    res->deletes += cnt->deletes;
    res->delayed += cnt->delayed;
}

void
add_stats(stats_t *res, stats_t *stat)
{
    res->elts += stat->elts;
    res->nodes += stat->nodes;
    res->tests += stat->tests;
    res->misses += stat->misses;
    res->rehashes += stat->rehashes;
}

static void
ctx_add_counters (wctx_t *ctx, counter_t *cnt, counter_t *red, stats_t *stats)
{
    if (NULL != ctx->rec_ctx)                   // recursion
        ctx_add_counters (ctx->rec_ctx, cnt+1, red+1, NULL);
    if (ctx == contexts[ctx->id])               // top level
        add_stats (stats, ctx->stats);
    add_results(cnt, &ctx->counters);
    add_results(red, &ctx->red);
}

static inline void
wait_seed (wctx_t *ctx, ref_t seed)
{
    int didwait = 0;
    while (get_wip(seed) > 0 && !lb2_is_stopped(lb2)) { didwait = 1; } //wait
    if (didwait) {
        ctx->red.waits++;
    }
}

static inline void
increase_level (counter_t *cnt)
{
    cnt->level_cur++;
    if (cnt->level_cur > cnt->level_max) {
        cnt->level_max = cnt->level_cur;
    }
}

static inline void
set_all_red (wctx_t *ctx, state_info_t *state)
{
    if (global_try_color(state->ref, GRED, ctx->rec_bits)) {
        ctx->counters.allred++;
        if ( GBbuchiIsAccepting(ctx->model, state->data) )
            ctx->red.visited++; /* count accepting states */
    } else {
        ctx->red.allred++;
    }
}

static inline void
set_red (wctx_t *ctx, state_info_t *state)
{
    if (global_try_color(state->ref, GRED, ctx->rec_bits)) {
        ctx->red.explored++;
        if ( GBbuchiIsAccepting(ctx->model, get(dbs, state->ref, ctx->store2)) )
            ctx->red.visited++; /* count accepting states */
    } else {
        ctx->red.bogus_red++;
    }
}

static model_t      model = NULL;

static              model_t
get_model (int first)
{
    int                 start_index = 0;
    cct_cont_t         *map = cct_create_cont (start_index);
    model_t             model = GBcreateBase ();
    GBsetChunkMethods (model, (newmap_t)cct_create_vt, map,
                       HREgreyboxI2C, HREgreyboxC2I, HREgreyboxCount);
    if (first)
        GBloadFileShared (model, files[0]);
    GBloadFile (model, files[0], &model);
    return model;
}

/* Magic number for the largest stack i've encountered.
 * Allocated as bits in a bitvector, but addressed in a stack-wise fashion
 */
static const size_t MAX_STACK = 100000000;

static int num_global_bits (strategy_t s) {
    assert (GRED.g == 0);
    assert (GGREEN.g == 1);
    assert (GDANGEROUS.g == 2);
    return (Strat_ENDFS  & s ? 3 :
           (Strat_CNDFS  & s ? 2 :
           ((Strat_LNDFS | Strat_TA) & s ? 1 : 0)));
}

wctx_t *
wctx_create (size_t id, int depth, wctx_t *shared)
{
    assert (NULL == 0);
    wctx_t             *ctx = RTalignZero (CACHE_LINE_SIZE, sizeof (wctx_t));
    ctx->id = id;
    ctx->strategy = strategy[depth];
    if (NULL == shared)
        ctx->model = 0 == id ? model : get_model (0);
    else
        ctx->model = shared->model;
    state_info_create_empty (&ctx->state);
    ctx->store = RTalignZero (CACHE_LINE_SIZE, SLOT_SIZE * N * 2);
    ctx->store2 = RTalignZero (CACHE_LINE_SIZE, SLOT_SIZE * N * 2);
    ctx->stack = dfs_stack_create (state_info_int_size());
    ctx->out_stack = ctx->in_stack = ctx->stack;
    if (strategy[depth] & Strat_2Stacks)
        ctx->in_stack = dfs_stack_create (state_info_int_size());
    if (strategy[depth] & (Strat_CNDFS)) //third stack for accepting states
        ctx->out_stack = dfs_stack_create (state_info_int_size());
    if (strategy[depth] & (Strat_TA))
        ctx->backoff = dfs_stack_create (state_info_int_size());
    //allocate two bits for NDFS colorings
    if (strategy[depth] & Strat_LTL) {
        size_t local_bits = 2;
        int res = bitvector_create (&ctx->color_map, local_bits<<dbs_size);
        if (-1 == res) Abort ("Failure to allocate color bitvector.");
        if ((Strat_NNDFS | Strat_LNDFS | Strat_CNDFS | Strat_ENDFS) & strategy[depth]) {
            res = bitvector_create (&ctx->all_red, MAX_STACK);
            if (-1 == res) Abort ("Failure to allocate all-red bitvector.");
        }
    } else if (UseGreyBox == call_mode && Strat_DFS == strategy[depth]) {
        ctx->group_stack = isba_create (1);
    }
    ctx->counters.threshold = ctx->red.threshold = threshold;
    ctx->permute = permute_create (permutation, ctx->model, W, K, id);
    ctx->rec_bits = (depth ? shared->rec_bits + num_global_bits(strategy[depth-1]) : 0) ;
    ctx->rec_ctx = NULL;
    ctx->red.timer = RTcreateTimer ();
    ctx->counters.timer = RTcreateTimer ();
    statistics_init (&ctx->counters.lattice_ratio);
    ctx->red.time = 0;
    if (Strat_None != strategy[depth+1])
        ctx->rec_ctx = wctx_create (id, depth+1, ctx);
    return ctx;
}

void
wctx_free (wctx_t *ctx, int depth)
{
    RTfree (ctx->store);
    RTfree (ctx->store2);
    dfs_stack_destroy (ctx->out_stack);
    if (strategy[depth] & Strat_LTL) {  
        bitvector_free (&ctx->color_map);
        if ((Strat_NNDFS | Strat_LNDFS | Strat_CNDFS | Strat_ENDFS) & strategy[depth]) {
            bitvector_free (&ctx->all_red);
        }
    }
    if (strategy[depth] & (Strat_TA))
        dfs_stack_destroy (ctx->backoff);
    if (NULL != ctx->group_stack)
        isba_destroy (ctx->group_stack);
    //if (strategy[depth] & (Strat_BFS | Strat_ENDFS | Strat_CNDFS))
    //    dfs_stack_destroy (ctx->in_stack);
    if (strategy[depth] & (Strat_CNDFS))
        dfs_stack_destroy (ctx->stack); 
    if (strategy[depth] ==  (Strat_2Stacks))
        dfs_stack_destroy (ctx->in_stack);
    if (NULL != ctx->permute)
        permute_free (ctx->permute);
    if ( NULL != ctx->rec_ctx )
        wctx_free (ctx->rec_ctx, depth+1);
    RTfree (ctx);
}

static hash64_t
z_rehash (const void *v, int b, hash64_t seed)
{
    return zobrist_rehash (zobrist, seed);
    (void)b; (void)v;
}

static int
find_or_put_zobrist (state_info_t *state, transition_info_t *ti,
                     state_info_t *pred, state_data_t store)
{
    state->hash64 = zobrist_hash_dm (zobrist, state->data, pred->data,
                                     pred->hash64, ti->group);
    return DBSLLlookup_hash (dbs, state->data, &state->ref, &state->hash64);
    (void) store;
}

static int
find_or_put_dbs (state_info_t *state, transition_info_t *ti,
                 state_info_t *predecessor, state_data_t store)
{
    return DBSLLlookup_hash (dbs, state->data, &state->ref, NULL);
    (void) predecessor; (void) store; (void) ti;
}

static int
find_or_put_tree (state_info_t *state, transition_info_t *ti,
                  state_info_t *pred, state_data_t store)
{
    int                 ret;
    ret = TreeDBSLLlookup_dm (dbs, state->data, pred->tree, store, ti->group);
    state->tree = store;
    state->ref = TreeDBSLLindex (dbs, state->tree);
    return ret;
}

void
init_globals ()
{
#ifdef OPAAL
    strategy[0] |= Strat_TA;
#endif
    W = HREpeers(HREglobal());
    model = get_model (1);
    if (Perm_Unknown == permutation) //default permutation depends on strategy
        permutation = strategy[0] & Strat_Reach ? Perm_None : Perm_Dynamic;
    if (Perm_None != permutation) {
         if (call_mode == UseGreyBox)
            Abort ("Greybox not supported with state permutation.");
        refs = 1; //The permuter works with references only!
    }
    if (strategy[0] & Strat_LTL) {
        if (call_mode == UseGreyBox)
            Warning(info, "Greybox not supported with strategy NDFS, ignored.");
        threshold = 100000;
        permutation_red = no_red_perm ? Perm_None : permutation;
    } else {
        threshold = 100000 / W;
    }
    if (!(Strat_Reach & strategy[0]) && (assert_detect || dlk_detect || act_detect || inv_detect))
        Abort ("Verification of safety properties works only with reachability algorithms.");
    Warning (info, "Using %d cores (lb: SRP)", W/*, key_search(lb_methods, lb_method)*/);
    Warning (info, "loading model from %s", files[0]);
    lts_type_t          ltstype = GBgetLTStype (model);
    int                 state_labels = lts_type_get_state_label_count (ltstype);
    int                 edge_labels = lts_type_get_edge_label_count (ltstype);
    Warning (info, "There are %d state labels and %d edge labels",
             state_labels, edge_labels);
    matrix_t           *m = GBgetDMInfo (model);
    N = lts_type_get_state_length (ltstype);
    D = (strategy[0] & Strat_TA ? N - 2 : N);
    K = dm_nrows (m);
    Warning (info, "State length is %d, there are %d groups", N, K);
    assert (GRED.g == 0);
    if (assert_detect)
        act_detect = "assert";
    if (act_detect) {
        for (int i = 0; i < edge_labels; i++) {
            char *name = lts_type_get_edge_label_name(ltstype, i);
            if (0 == strcmp(name, act_detect)) {
                act_index = i;
                act_type = lts_type_get_edge_label_typeno(ltstype, i);
            }
        }
        if (-1 == act_index) Warning (info, "Cannot find action '%s', no such edge label is defined!", act_detect);
    }
    if (inv_detect)
        inv_expr = pred_parse_file (model, inv_detect);
    if (0 == dbs_size) {
        size_t el_size = (db_type != HashTable ? 3 : D) * SLOT_SIZE; // over estimation for cleary
        size_t map_el_size = (Strat_TA & strategy[0] ? sizeof(lattice_t) : 0);
        size_t db_el_size = (RTmemSize() / 3) / (el_size + map_el_size);
        dbs_size = (int) (log(db_el_size) / log(2));
        dbs_size = dbs_size > DB_SIZE_MAX ? DB_SIZE_MAX : dbs_size;
    }
    MAX_SUCC = ( Strat_DFS == strategy[0] ? 1 : SIZE_MAX );  /* for --grey: */
    if (trc_output && !(strategy[0] & Strat_LTL))
        parent_ref = RTmalloc (sizeof(ref_t[1UL<<dbs_size]));
    count_bits = ceil ( log(W+1) / log(2) );
    count_mask = (1<<count_bits) - 1;
    int i = 0;
    while (Strat_None != strategy[i] && i < MAX_STRATEGIES) {
        global_bits += num_global_bits(strategy[i]);
        local_bits += (Strat_LTL & strategy[i++] ? 2 : 0);
    }
    count_bits = (Strat_LNDFS == strategy[i-1] ? count_bits : 0);
    if (Strat_TA & strategy[0])
        lmap = lm_create (W, 1UL<<dbs_size, LATTICE_BLOCK_SIZE);
    int                 cleary = 0;
    int                 indexing = NULL != trc_output || ((Strat_TA | Strat_LTLG) & strategy[0]);
    switch (db_type) {
    case HashTable:
        if (ZOBRIST) {
            zobrist = zobrist_create (D, ZOBRIST, m);
            find_or_put = find_or_put_zobrist;
            dbs = DBSLLcreate_sized (D, dbs_size, (hash64_f)z_rehash, global_bits + count_bits);
        } else {
            find_or_put = find_or_put_dbs;
            dbs = DBSLLcreate_sized (D, dbs_size, (hash64_f)MurmurHash64, global_bits + count_bits);
        }
        statistics = (dbs_stats_f) DBSLLstats;
        get = (dbs_get_f) DBSLLget;
        get_sat_bit = (dbs_get_sat_f) DBSLLget_sat_bit;
        unset_sat_bit = (dbs_unset_sat_f) DBSLLunset_sat_bit;
        try_set_sat_bit = (dbs_try_set_sat_f) DBSLLtry_set_sat_bit;
        inc_sat_bits = (dbs_inc_sat_bits_f) DBSLLinc_sat_bits;
        dec_sat_bits = (dbs_dec_sat_bits_f) DBSLLdec_sat_bits;
        get_sat_bits = (dbs_get_sat_bits_f) DBSLLget_sat_bits;
        Warning (info, "Using a hash table with 2^%d elements", dbs_size);
        break;
    case ClearyTree:
        cleary = 1;
        if (indexing) Abort ("Cleary tree not supported in combination with error trails or the MCNDFS algorithms.");
    case TreeTable:
        if (ZOBRIST)
            Abort ("Zobrist and treedbs is not implemented");
        statistics = (dbs_stats_f) TreeDBSLLstats;
        get = (dbs_get_f) TreeDBSLLget;
        find_or_put = find_or_put_tree;
        dbs = TreeDBSLLcreate_dm (D, dbs_size, ratio,  m, global_bits + count_bits, cleary, indexing);
        unset_sat_bit = (dbs_unset_sat_f) TreeDBSLLunset_sat_bit;
        get_sat_bit = (dbs_get_sat_f) TreeDBSLLget_sat_bit;
        try_set_sat_bit = (dbs_try_set_sat_f) TreeDBSLLtry_set_sat_bit;
        inc_sat_bits = (dbs_inc_sat_bits_f) TreeDBSLLinc_sat_bits;
        dec_sat_bits = (dbs_dec_sat_bits_f) TreeDBSLLdec_sat_bits;
        get_sat_bits = (dbs_get_sat_bits_f) TreeDBSLLget_sat_bits;
        Warning (info, "Using a%s tree table with 2^%d elements", indexing ? "" : " non-indexing", dbs_size);
        break;
    case Tree: default: Abort ("Unknown state storage type: %d.", db_type);
    }
    Warning (info, "Global bits: %d, count bits: %d, local bits: %d.",
             global_bits, count_bits, local_bits);
    contexts = RTmalloc (sizeof (wctx_t *[W]));
    initial_data = RTmalloc (SLOT_SIZE * N);
    GBgetInitialState (model, initial_data);
    lb2 = lb2_create_max (W, G, H);
    (void) signal (SIGINT, exit_ltsmin);
}

void
deinit_globals ()
{
    if (HashTable & db_type)
        DBSLLfree (dbs);
    else //TreeDBSLL
        TreeDBSLLfree (dbs);
    RTfree (initial_data);
    if (NULL != lmap)
        lm_free (lmap);
    lb2_destroy(lb2);
    for (size_t i = 0; i < W; i++)
        wctx_free (contexts[i], 0);
    RTfree (contexts);
}

static inline void
print_state_space_total (char *name, counter_t *cnt)
{
    Warning (info, "%s%zu levels %zu states %zu transitions",
             name, cnt->level_max, cnt->explored, cnt->trans);
}

static inline void
maybe_report (counter_t *cnt, char *msg, size_t *threshold)
{
    if (!log_active(info) || cnt->explored < *threshold)
        return;
    if (!cas (threshold, *threshold, *threshold << 1))
        return;
    if (W == 1 || (strategy[0] & Strat_LTL))
        print_state_space_total (msg, cnt);
    else
        Warning (info, "%s%zu levels ~%zu states ~%zu transitions", msg,
                 cnt->level_max, W * cnt->explored,  W * cnt->trans);
}

static inline void
ndfs_maybe_report (char *prefix, counter_t *cnt)
{
    maybe_report (cnt, prefix, &threshold);
}

static void
print_totals (counter_t *ar_reach, counter_t *ar_red, int d, size_t db_elts)
{
    counter_t          *reach = ar_reach;
    counter_t          *red = ar_red;
    reach->explored /= W;
    reach->trans /= W;
    red->trans /= W;
    if ( 0 == (Strat_LTLG & strategy[d]) ) {
        red->visited /= W;
        red->explored /= W;
    }
    Warning (info, "%s_%d (%s/%s) stats:", key_search(strategies, strategy[d]), d+1,
             key_search(permutations, permutation), key_search(permutations, permutation_red));
    Warning (info, "blue states: %zu (%.2f%%), transitions: %zu (per worker)",
             reach->explored, ((double)reach->explored/db_elts)*100, reach->trans);
    Warning (info, "red states: %zu (%.2f%%), bogus: %zu  (%.2f%%), transitions: %zu, waits: %zu (%.2f sec)",
             red->explored, ((double)red->explored/db_elts)*100, red->bogus_red,
             ((double)red->bogus_red/db_elts), red->trans, red->waits, red->time);
    if  ( all_red && (strategy[d] & (Strat_LNDFS | Strat_NNDFS | Strat_CNDFS  | Strat_ENDFS)) )
        Warning (info, "all-red states: %zu (%.2f%%), bogus %zu (%.2f%%)",
             reach->allred, ((double)reach->allred/db_elts)*100,
             red->allred, ((double)red->allred/db_elts)*100);
    if (Strat_None != strategy[d+1]) {
        print_totals (ar_reach + 1, ar_red + 1, d+1, db_elts);
    }
}

static void
print_statistics (counter_t *ar_reach, counter_t *ar_red, rt_timer_t timer,
                  stats_t *stats)
{
    counter_t          *reach = ar_reach;
    counter_t          *red = ar_red;
    double              mem1, mem2, mem3=0, mem4, compr, fill, leafs;
    float               tot = RTrealTime (timer);
    size_t              db_elts = stats->elts;
    size_t              db_nodes = stats->nodes;
    db_nodes = db_nodes == 0 ? db_elts : db_nodes;
    double              el_size =
       db_type & Tree ? (db_type==ClearyTree?1:2) + (2.0 / (1UL<<ratio)) : D+.5;
    size_t              s = state_info_size();
    size_t              max_load = Strat_NDFS & strategy[0] ?
                                   reach->level_max+red->level_max :
            (Strat_SBFS & strategy[0] ? max_level_size : lb2_max_load(lb2));
    mem1 = ((double)(s * max_load)) / (1 << 20);
    size_t lattices = reach->inserts - reach->updates;
    if (Strat_LTL & strategy[0]) {
        RTprintTimer (info, timer, "Total exploration time");
        Warning (info, "");
        Warning (info, "State space has %zu states, %zu are accepting", db_elts,
                 red->visited);
        print_totals (ar_reach, ar_red, 0, db_elts);
        mem3 = ((double)(((((size_t)local_bits)<<dbs_size))/8*W)) / (1UL<<20);
        Warning (info, "");
        Warning (info, "Total memory used for local state coloring: %.1fMB", mem3);
    } else {
        statistics_t state_stats; statistics_init (&state_stats);
        statistics_t trans_stats; statistics_init (&trans_stats);
        for (size_t i = 0; i< W; i++) {
            statistics_record (&state_stats, contexts[i]->counters.explored);
            statistics_record (&trans_stats, contexts[i]->counters.trans);
        }
        if (W > 1)
            Warning (info, "mean standard work distribution: %.1f%% (states) %.1f%% (transitions)",
                     (100 * statistics_stdev(&state_stats) / statistics_mean(&state_stats)),
                     (100 * statistics_stdev(&trans_stats) / statistics_mean(&trans_stats)));
        Warning (info, "");
        print_state_space_total ("State space has ", reach);
        RTprintTimer (info, timer, "Total exploration time");
        double time = RTrealTime (timer);
        Warning(info, "States per second: %.0f, Transitions per second: %.0f",
                ar_reach->explored/time, ar_reach->trans/time);
        Warning(info, "");
        if (Strat_TA & strategy[0]) {
            if (STDEV) {
                statistics_t stats; statistics_init (&stats);
                for (size_t i = 0; i< W; i++) {
                    statistics_t *s = &contexts[i]->counters.lattice_ratio;
                    statistics_union (&stats, &stats, s);
                }
                Warning (info, "Lattice ratio: %.2f standard deviation: %.3f", ((double)lattices/db_elts), statistics_stdev(&stats));
            }
            size_t alloc = lm_allocated (lmap);
            mem3 = ((double)(sizeof(lattice_t[alloc + db_elts]))) / (1<<20);
            double lm = ((double)(sizeof(lattice_t[alloc + (1UL<<dbs_size)]))) / (1<<20);
            double redundancy = (((double)(db_elts + alloc)) / lattices - 1) * 100;
            Warning (info, "Lattice map: %.1fMB (~%.1fMB paged-in) overhead: %.2f%%, allocated: %zu", mem3, lm, redundancy, alloc);
        }
    }

    Warning (info, "Queue width: %zuB, total height: %zu, memory: %.2fMB",
             s, max_load, mem1);
    mem2 = ((double)(1UL << (dbs_size)) / (1<<20)) * SLOT_SIZE * el_size;
    mem4 = ((double)(db_nodes * SLOT_SIZE * el_size)) / (1<<20);
    fill = (double)((db_elts * 100) / (1UL << dbs_size));
    if (db_type & Tree) {
        compr = (double)(db_nodes * el_size) / ((D+1) * db_elts) * 100;
        leafs = (double)(((db_nodes - db_elts) * 100) / (1UL << (dbs_size-ratio)));
        Warning (info, "Tree memory: %.1fMB, compr.: %.1f%%, fill (roots/leafs): "
                "%.1f%%/%.1f%%", mem4, compr, fill, leafs);
    } else {
        Warning (info, "Table memory: %.1fMB, fill ratio: %.1f%%", mem4, fill);
    }
    Warning (info, "Est. total memory use: %.1fMB (~%.1fMB paged-in)",
             mem1 + mem4 + mem3, mem1 + mem2 + mem3);
    Warning (infoLong, "Internal statistics:\n\n"
             "Algorithm:\nWork time: %.2f sec\nUser time: %.2f sec\nExplored: %zu\n"
                 "Transitions: %zu\nDeadlocks: %zu\nInvariant violations: %zu\n"
                 "Error actions: %zu\nWaits: %zu\nRec. calls: %zu\n\n"
             "Database:\nElements: %zu\nNodes: %zu\nMisses: %zu\nEq. tests: %zu\nRehashes: %zu\n\n"
             "Memory:\nQueue: %.1f MB\nDB: %.1f MB\nDB alloc.: %.1f MB\nColors: %.1f MB\n\n"
             "Load balancer:\nSplits: %zu\nLoad transfer: %zu\n\n"
             "Lattice MAP:\nRatio: %.2f\nInserts: %zu\nUpdates: %zu\nDeletes: %zu\nDelayed: %zu",
             tot, reach->runtime, reach->explored, reach->trans, reach->deadlocks,
                    reach->violations, reach->errors, red->waits, reach->rec,
             db_elts, db_nodes, stats->misses, stats->tests, stats->rehashes,
             mem1, mem4, mem2, mem3,
             reach->splits, reach->transfer,
             ((double)lattices/db_elts), reach->inserts, reach->updates,
                     reach->deletes, reach->delayed);
}

static void
print_thread_statistics (wctx_t *ctx)
{
    char                name[128];
    char               *format = "[%zu%s] saw in %.3f sec ";
    if (W < 4) {
    if (Strat_Reach & strategy[0]) {
        snprintf (name, sizeof name, format, ctx->id, "", ctx->counters.runtime);
        print_state_space_total (name, &ctx->counters);
    } else if (Strat_LTL & strategy[0]) {
        snprintf (name, sizeof name, format, ctx->id, " B", ctx->counters.runtime);
        print_state_space_total (name, &ctx->counters);
        snprintf (name, sizeof name, format, ctx->id, " R", ctx->counters.runtime);
        print_state_space_total (name, &ctx->red);
    }}
}

/** Fisher / Yates GenRandPerm*/
static void
randperm (int *perm, int n, uint32_t seed)
{
    srandom (seed);
    for (int i=0; i<n; i++)
        perm[i] = i;
    for (int i=0; i<n; i++) {
        int                 j = random()%(n-i)+i;
        int                 t = perm[j];
        perm[j] = perm[i];
        perm[i] = t;
    }
}

static int
sort_cmp (const void *a, const void *b, void *arg)
{
    permute_t          *perm = (permute_t *) arg;
    const permute_todo_t     *A = &perm->todos[*((int*)a)];
    const permute_todo_t     *B = &perm->todos[*((int*)b)];
    return A->si.ref - B->si.ref + perm->shiftorder;
}

static const int            RR_ARRAY_SIZE = 16;

static int
rr_cmp (const void *a, const void *b, void *arg)
{
    permute_t          *perm = (permute_t *) arg;
    int                *rand = *perm->rand;
    ref_t               A = perm->todos[*((int*)a)].si.ref;
    ref_t               B = perm->todos[*((int*)b)].si.ref;
    return ((((1UL<<dbs_size)-1)&rand[A & ((1<<RR_ARRAY_SIZE)-1)])^A) -
           ((((1UL<<dbs_size)-1)&rand[B & ((1<<RR_ARRAY_SIZE)-1)])^B);
}

static int
rand_cmp (const void *a, const void *b, void *arg)
{
    permute_t          *perm = (permute_t *) arg;
    int                *rand = *perm->rand;
    const permute_todo_t     *A = &perm->todos[*((int*)a)];
    const permute_todo_t     *B = &perm->todos[*((int*)b)];
    return rand[A->ti.group] - rand[B->ti.group];
}

static int
dyn_cmp (const void *a, const void *b, void *arg)
{
    permute_t          *perm = (permute_t *) arg;
    wctx_t             *ctx = perm->ctx;
    int                *rand = *perm->rand;
    const permute_todo_t     *A = &perm->todos[*((int*)a)];
    const permute_todo_t     *B = &perm->todos[*((int*)b)];
  
    if (!(Strat_LTL & strategy[0]) || A->seen != B->seen) {
        return B->seen - A->seen;
    } else {
        int Awhite = nn_color_eq(nn_get_color(&ctx->color_map, A->si.ref), NNWHITE);
        int Bwhite = nn_color_eq(nn_get_color(&ctx->color_map, B->si.ref), NNWHITE);
        int Aval = ((A->seen) << 1) | Awhite;
        int Bval = ((B->seen) << 1) | Bwhite;
        if (Aval == Bval)
            return rand[A->ti.group] - rand[B->ti.group];
        return Bval - Aval;
    }
}

static inline void
perm_todo (permute_t *perm, state_data_t dst, transition_info_t *ti)
{
    assert (perm->nstored < perm->trans+TODO_MAX);
    permute_todo_t *next = perm->todos + perm->nstored;
    perm->tosort[perm->nstored] = perm->nstored;
    next->seen = state_info_initialize (&next->si, dst, ti, perm->state, perm->ctx);
    next->si.data = (raw_data_t) -2; // we won't copy these around, since they
    next->si.tree = (raw_data_t) -2; // are is stored in the DB and we have a reference
    next->ti.group = ti->group;
    next->ti.labels = ti->labels;
    perm->nstored++;
}


static char *
full_msg(int ret)
{
    return (DB_FULL == ret ? "hash table" : (DB_ROOTS_FULL == ret ? "tree roots table" : "tree leafs table"));
}

static inline void
perm_do (permute_t *perm, int i)
{
    permute_todo_t *todo = perm->todos + i;
    if (todo->seen < 0)
        if (lb2_stop(lb2)) Warning (info, "Error: %s full! Change -s/--ratio.", full_msg(todo->seen));
    perm->real_cb (perm->ctx, &todo->si, &todo->ti, todo->seen);
}

static inline void
perm_do_all (permute_t *perm)
{
    for (size_t i = 0; i < perm->nstored; i++)
        perm_do (perm, perm->tosort[i]);
}

permute_t *
permute_create (permutation_perm_t permutation, model_t model,
                size_t workers, size_t trans, int worker_index)
{
    permute_t          *perm = RTalign (CACHE_LINE_SIZE, sizeof(permute_t));
    perm->todos = RTalign (CACHE_LINE_SIZE, sizeof(permute_todo_t[trans+TODO_MAX]));
    perm->tosort = RTalign (CACHE_LINE_SIZE, sizeof(int[trans+TODO_MAX]));
    perm->shift = ((double)trans)/workers;
    perm->shiftorder = (1UL<<dbs_size) / workers * worker_index;
    perm->start_group = perm->shift * worker_index;
    perm->trans = trans;
    perm->model = model;
    perm->permutation = permutation;
    if (Perm_Otf == perm->permutation)
        perm->pad = RTalign (CACHE_LINE_SIZE, sizeof(int[trans+TODO_MAX]));
    if (Perm_Random == perm->permutation) {
        perm->rand = RTalignZero (CACHE_LINE_SIZE, sizeof(int*[trans+TODO_MAX]));
        for (size_t i = 1; i < perm->trans+TODO_MAX; i++) {
            perm->rand[i] = RTalign (CACHE_LINE_SIZE, sizeof(int[ i ]));
            randperm (perm->rand[i], i, perm->shiftorder);
        }
    }
    if (Perm_RR == perm->permutation) {
        perm->rand = RTalignZero (CACHE_LINE_SIZE, sizeof(int*));
        perm->rand[0] = RTalign (CACHE_LINE_SIZE, sizeof(int[1<<RR_ARRAY_SIZE]));
        srandom (time(NULL) + 9876432*worker_index);
        for (int i =0; i < (1<<RR_ARRAY_SIZE); i++)
            perm->rand[0][i] = random();
    }
    if (Perm_SR == perm->permutation || Perm_Dynamic == perm->permutation) {
        perm->rand = RTalignZero (CACHE_LINE_SIZE, sizeof(int*));
        perm->rand[0] = RTalign (CACHE_LINE_SIZE, sizeof(int[trans+TODO_MAX]));
        randperm (perm->rand[0], trans+TODO_MAX, (time(NULL) + 9876*worker_index));
    }
    return perm;
}

void
permute_set_model (permute_t *perm, model_t model)
{
    perm->model = model;
}

void
permute_free (permute_t *perm)
{
    RTfree (perm->todos);
    RTfree (perm->tosort);
    if (Perm_Otf == perm->permutation)
        RTfree (perm->pad);
    if (Perm_Random == perm->permutation) {
        for (size_t i = 0; i < perm->trans+TODO_MAX; i++)
            RTfree (perm->rand[i]);
        RTfree (perm->rand);
    }
    if (((Perm_SR | Perm_RR) & perm->permutation) || Perm_Dynamic == perm->permutation) {
        RTfree (perm->rand[0]);
        RTfree (perm->rand);
    }
    RTfree (perm);
}

static void
permute_one (void *arg, transition_info_t *ti, state_data_t dst)
{
    permute_t          *perm = (permute_t*) arg;
    state_info_t        successor;
    int                 seen;
    switch (perm->permutation) {
    case Perm_Shift:
        if (ti->group < perm->start_group) {
            perm_todo (perm, dst, ti);
            break;
        }
    case Perm_None:
        seen = state_info_initialize (&successor, dst, ti, perm->state, perm->ctx);
        if (seen < 0)
            if (lb2_stop(lb2)) Warning (info, "Error: %s full! Change -s/--ratio.", full_msg(seen));
        perm->real_cb (perm->ctx, &successor, ti, seen);
        break;
    case Perm_Shift_All:
        if (0 == perm->start_group_index && ti->group >= perm->start_group)
            perm->start_group_index = perm->nstored;
    case Perm_Dynamic:
    case Perm_Random:
    case Perm_SR:
    case Perm_RR:
    case Perm_Otf:
    case Perm_Sort:
        perm_todo (perm, dst, ti);
        break;
    default:
        Abort ("Unknown permutation!");
    }
}

int
permute_trans (permute_t *perm, state_info_t *state, perm_cb_f cb, void *ctx)
{
    perm->ctx = ctx;
    perm->real_cb = cb;
    perm->state = state;
    perm->nstored = perm->start_group_index = 0;
    int v[N];
    int count;
    if ((Strat_TA & strategy[0]) && (refs || (Tree & db_type))) {
        memcpy (v, state->data, D<<2);
        ((lattice_t*)(v + D))[0] = state->lattice;
        count = GBgetTransitionsAll (perm->model, v, permute_one, perm);
    } else {
        count = GBgetTransitionsAll (perm->model, state->data, permute_one, perm);
    }
    switch (perm->permutation) {
    case Perm_Otf:
        randperm (perm->pad, perm->nstored, state->ref + perm->shiftorder);
        for (size_t i = 0; i < perm->nstored; i++)
            perm_do (perm, perm->pad[i]);
        break;
    case Perm_Random:
        for (size_t i = 0; i < perm->nstored; i++)
            perm_do (perm, perm->rand[perm->nstored][i]);
        break;
    case Perm_Dynamic:
        qsortr (perm->tosort, perm->nstored, sizeof(int), dyn_cmp, perm);
        perm_do_all (perm);
        break;
    case Perm_RR:
        qsortr (perm->tosort, perm->nstored, sizeof(int), rr_cmp, perm);
        perm_do_all (perm);
        break;
    case Perm_SR:
        qsortr (perm->tosort, perm->nstored, sizeof(int), rand_cmp, perm);
        perm_do_all (perm);
        break;
    case Perm_Sort:
        qsortr (perm->tosort, perm->nstored, sizeof(int), sort_cmp, perm);
        perm_do_all (perm);
        break;
    case Perm_Shift:
        perm_do_all (perm);
        break;
    case Perm_Shift_All:
        for (size_t i = 0; i < perm->nstored; i++) {
            size_t j = (perm->start_group_index + i);
            j = j < perm->nstored ? j : 0;
            perm_do (perm, j);
        }
        break;
    case Perm_None:
        break;
    default:
        Abort ("Unknown permutation!");
    }
    return count;
}

/**
 * Algo's state representation and serialization / deserialization
 * TODO: move this to state_info.c
 */

void
state_info_create_empty (state_info_t *state)
{
    state->tree = NULL;
    state->data = NULL;
    state->ref = DUMMY_IDX;
}

void
state_info_create (state_info_t *state, state_data_t data, tree_t tree,
                   ref_t ref)
{
    state->data = data;
    state->tree = tree;
    state->ref = ref;
}

size_t
state_info_int_size ()
{
    return (state_info_size () + 3) / 4;
}

size_t
state_info_size ()
{
    size_t              ref_size = sizeof (ref_t);
    size_t              data_size = SLOT_SIZE * (HashTable & db_type ? D : 2*D);
    size_t              state_info_size = refs ? ref_size : data_size;
    if (!refs && (HashTable & db_type))
        state_info_size += ref_size;
    if (ZOBRIST)
        state_info_size += sizeof (hash64_t);
    if (Strat_TA & strategy[0])
        state_info_size += sizeof (lattice_t) + sizeof (lm_loc_t);
    return state_info_size;
}

/**
 * Next-state function output --> algorithm
 */
int
state_info_initialize (state_info_t *state, state_data_t data,
                       transition_info_t *ti, state_info_t *src, wctx_t *ctx)
{
    state->data = data;
    if (Strat_TA & strategy[0]) {
        state->lattice = *(lattice_t*)(data + D);
        state->loc = LM_NULL_LOC;
    }
    return find_or_put (state, ti, src, ctx->store2);
}

/**
 * From stack/queue --> algorithm
 */
void
state_info_serialize (state_info_t *state, raw_data_t data)
{
    assert (state->ref != (1 | (1ULL<<32)));
    if (ZOBRIST) {
        ((uint64_t*)data)[0] = state->hash64;
        data += 2;
    }
    if (refs) {
        ((ref_t*)data)[0] = state->ref;
        data += 2;
    } else if (HashTable & db_type) {
        ((ref_t*)data)[0] = state->ref;
        data += 2;
        memcpy (data, state->data, (SLOT_SIZE * D));
        data += D;
    } else { // Tree
        memcpy (data, state->tree, (2 * SLOT_SIZE * D));
        data += D<<1;
    }
    if (Strat_TA & strategy[0]) {
        ((lattice_t*)data)[0] = state->lattice;
        data += 2;
        ((lm_loc_t*)data)[0] = state->loc;
    }
}

/**
 * From stack/queue --> algorithm
 */
void
state_info_deserialize (state_info_t *state, raw_data_t data, state_data_t store)
{
    if (ZOBRIST) {
        state->hash64 = ((hash64_t*)data)[0];
        data += 2;
    }
    if (refs) {
        state->ref  = ((ref_t*)data)[0];
        data += 2;
        state->data = get (dbs, state->ref, store);
        if (Tree & db_type) {
            state->tree = state->data;
            state->data = TreeDBSLLdata (dbs, state->data);
        }
    } else {
        if (HashTable & db_type) {
            state->ref  = ((ref_t*)data)[0];
            data += 2;
            state->data = data;
            data += D;
        } else { // Tree
            state->tree = data;
            state->data = TreeDBSLLdata (dbs, data);
            state->ref  = TreeDBSLLindex (dbs, data);
            data += D<<1;
        }
    }
    if (Strat_TA & strategy[0]) {
        state->lattice = ((lattice_t*)data)[0];
        data += 2;
        state->loc = ((lm_loc_t*)data)[0];
    }
}

void
state_info_deserialize_cheap (state_info_t *state, raw_data_t data)
{
    assert (refs);
    if (ZOBRIST) {
        state->hash64 = ((hash64_t*)data)[0];
        data += 2;
    }
    state->ref  = ((ref_t*)data)[0];
    data += 2;
    if (Strat_TA & strategy[0]) {
        state->lattice = ((lattice_t*)data)[0];
        data += 2;
        state->loc = ((lm_loc_t*)data)[0];
    }
}

static void *
get_state (ref_t ref, void *arg)
{
    wctx_t             *ctx = (wctx_t *) arg;
    raw_data_t          state = get (dbs, ref, ctx->store2);
    return Tree & db_type ? TreeDBSLLdata(dbs, state) : state;
}

static void
find_dfs_stack_trace (wctx_t *ctx, dfs_stack_t stack, ref_t *trace, size_t level)
{
    // gather trace
    state_info_t        state;
    assert (level - 1 == dfs_stack_nframes (ctx->stack));
    for (int i = dfs_stack_nframes (ctx->stack)-1; i >= 0; i--) {
        dfs_stack_leave (stack);
        raw_data_t          data = dfs_stack_pop (stack);
        state_info_deserialize (&state, data, ctx->store);
        trace[i] = state.ref;
    }
}

static void
ndfs_report_cycle (wctx_t *ctx, state_info_t *cycle_closing_state)
{
    /* Stop other workers, exit if some other worker was first here */
    if ( !lb2_stop(lb2) )
        return;
    size_t              level = dfs_stack_nframes (ctx->stack) + 1;
    Warning (info, "Accepting cycle FOUND at depth %zu!", level);
    if (trc_output) {
        ref_t              *trace = RTmalloc(sizeof(ref_t) * level);
        /* Write last state to stack to close cycle */
        trace[level-1] = cycle_closing_state->ref;
        find_dfs_stack_trace (ctx, ctx->stack, trace, level);
        trc_env_t          *trace_env = trc_create (ctx->model, get_state,
                                                    trace[0], ctx);
        trc_write_trace (trace_env, trc_output, trace, level);
        RTfree (trace);
    }
    Warning (info,"Exiting now!");
}

static void
handle_error_trace (wctx_t *ctx)
{
    size_t              level = ctx->counters.level_cur;
    if (trc_output) {
        ref_t               start_ref = initial_state.ref;
        trc_env_t  *trace_env = trc_create (ctx->model, get_state, start_ref, ctx);
        trc_find_and_write (trace_env, trc_output, ctx->state.ref, level, parent_ref);
    }
    Warning (info, "Exiting now!");
}

/*
 * NDFS algorithm by Courcoubetis et al.
 */

/* ndfs_handle and ndfs_explore_state can be used by blue and red search */
static void
ndfs_handle_red (void *arg, state_info_t *successor, transition_info_t *ti, int seen)
{
    wctx_t             *ctx = (wctx_t *) arg;
    if ( successor->ref == ctx->seed )
        /* Found cycle back to the seed */
        ndfs_report_cycle (ctx, successor);
    if ( !ndfs_has_color(&ctx->color_map, successor->ref, NRED) ) {
        raw_data_t stack_loc = dfs_stack_push (ctx->stack, NULL);
        state_info_serialize (successor, stack_loc);
    }
    (void) ti; (void) seen;
}

static void
ndfs_handle_blue (void *arg, state_info_t *successor, transition_info_t *ti, int seen)
{
    wctx_t             *ctx = (wctx_t *) arg;
    if ( !ndfs_has_color(&ctx->color_map, successor->ref, NBLUE) ) {
        raw_data_t stack_loc = dfs_stack_push (ctx->stack, NULL);
        state_info_serialize (successor, stack_loc);
    }
    (void) ti; (void) seen;
}

static inline void
ndfs_explore_state_red (wctx_t *ctx)
{
    counter_t *cnt = &ctx->red;
    dfs_stack_enter (ctx->stack);
    increase_level (cnt);
    ctx->permute->permutation = permutation_red;
    cnt->trans += permute_trans (ctx->permute, &ctx->state, ndfs_handle_red, ctx);
    cnt->explored++;
    ndfs_maybe_report ("[R] ", cnt);
}

static inline void
ndfs_explore_state_blue (wctx_t *ctx)
{
    counter_t *cnt = &ctx->counters;
    dfs_stack_enter (ctx->stack);
    increase_level (cnt);
    ctx->permute->permutation = permutation;
    cnt->trans += permute_trans (ctx->permute, &ctx->state, ndfs_handle_blue, ctx);
    cnt->explored++;
    ndfs_maybe_report ("[B] ", cnt);
}

/* NDFS dfs_red */
static void
ndfs_red (wctx_t *ctx, ref_t seed)
{
    ctx->seed = seed;
    ctx->red.visited++; //count accepting states
    while ( !lb2_is_stopped(lb2) ) {
        raw_data_t          state_data = dfs_stack_top (ctx->stack);
        if (NULL != state_data) {
            state_info_deserialize (&ctx->state, state_data, ctx->store);
            if ( ndfs_try_color(&ctx->color_map, ctx->state.ref, NRED) ) {
                if (seed == ctx->state.ref)
                    break;
                dfs_stack_pop (ctx->stack);
            } else
                ndfs_explore_state_red (ctx);
        } else { //backtrack
            dfs_stack_leave (ctx->stack);
            ctx->red.level_cur--;
            state_data = dfs_stack_top (ctx->stack);
            state_info_deserialize_cheap (&ctx->state, state_data);
            /* exit search if backtrack hits seed, leave stack the way it was */
            if (seed == ctx->state.ref)
                break;
            dfs_stack_pop (ctx->stack);
        }
    }
}

/* NDFS dfs_blue */
void
ndfs_blue (wctx_t *ctx)
{
    while ( !lb2_is_stopped(lb2) ) {
        raw_data_t          state_data = dfs_stack_top (ctx->stack);
        if (NULL != state_data) {
            state_info_deserialize (&ctx->state, state_data, ctx->store);
            if ( ndfs_try_color(&ctx->color_map, ctx->state.ref, NBLUE) ) {
                dfs_stack_pop (ctx->stack);
            } else
                ndfs_explore_state_blue (ctx);
        } else { //backtrack
            if (0 == dfs_stack_nframes (ctx->stack))
                return;
            dfs_stack_leave (ctx->stack);
            ctx->counters.level_cur--;
            /* call red DFS for accepting states */
            state_data = dfs_stack_top (ctx->stack);
            state_info_deserialize (&ctx->state, state_data, ctx->store);
            if ( GBbuchiIsAccepting(ctx->model, ctx->state.data) )
                ndfs_red (ctx, ctx->state.ref);
            dfs_stack_pop (ctx->stack);
        }
    }
}

/*
 * New NDFS algorithm by Schwoon/Esparza/Gaiser
 */

static void
nndfs_red_handle (void *arg, state_info_t *successor, transition_info_t *ti,
                  int seen)
{
    wctx_t             *ctx = (wctx_t *) arg;
    nndfs_color_t color = nn_get_color(&ctx->color_map, successor->ref);
    if ( nn_color_eq(color, NNCYAN) ) {
        /* Found cycle back to the stack */
        ndfs_report_cycle(ctx, successor);
    } else if ( nn_color_eq(color, NNBLUE) && (ctx->strategy != Strat_LNDFS ||
            !global_has_color(ctx->state.ref, GRED, ctx->rec_bits)) ) {
        raw_data_t stack_loc = dfs_stack_push (ctx->stack, NULL);
        state_info_serialize (successor, stack_loc);
    }
    (void) ti; (void) seen;
}

static void
nndfs_blue_handle (void *arg, state_info_t *successor, transition_info_t *ti,
                   int seen)
{
    wctx_t             *ctx = (wctx_t *) arg;
    nndfs_color_t color = nn_get_color (&ctx->color_map, successor->ref);
    /**
     * The following lines bear little resemblance to the algorithms in the
     * respective papers (NNDFS / LNDFS), but we must store all non-red states
     * on the stack here, in order to calculate all-red correctly later.
     */
    if ( nn_color_eq(color, NNCYAN) &&
            (GBbuchiIsAccepting(ctx->model, ctx->state.data) ||
             GBbuchiIsAccepting(ctx->model, get(dbs, successor->ref, ctx->store2))) ) {
        /* Found cycle in blue search */
        ndfs_report_cycle(ctx, successor);
    } else if ((ctx->strategy == Strat_LNDFS && !global_has_color(ctx->state.ref, GRED, ctx->rec_bits)) ||
               (ctx->strategy != Strat_LNDFS && !nn_color_eq(color, NNPINK))) {
        raw_data_t stack_loc = dfs_stack_push (ctx->stack, NULL);
        state_info_serialize (successor, stack_loc);
    }
    (void) ti; (void) seen;
}

static inline void
nndfs_explore_state_red (wctx_t *ctx)
{
    counter_t *cnt = &ctx->red;
    dfs_stack_enter (ctx->stack);
    increase_level (cnt);
    ctx->permute->permutation = permutation_red;
    cnt->trans += permute_trans (ctx->permute, &ctx->state, nndfs_red_handle, ctx);
    ndfs_maybe_report ("[R] ", cnt);
}

static inline void
nndfs_explore_state_blue (wctx_t *ctx)
{
    counter_t *cnt = &ctx->counters;
    dfs_stack_enter (ctx->stack);
    increase_level (cnt);
    ctx->permute->permutation = permutation;
    cnt->trans += permute_trans (ctx->permute, &ctx->state, nndfs_blue_handle, ctx);
    cnt->explored++;
    ndfs_maybe_report ("[B] ", cnt);
}

/* NNDFS dfs_red */
static void
nndfs_red (wctx_t *ctx, ref_t seed)
{
    ctx->red.visited++; //count accepting states
    nndfs_explore_state_red (ctx);
    while ( !lb2_is_stopped(lb2) ) {
        raw_data_t          state_data = dfs_stack_top (ctx->stack);
        if (NULL != state_data) {
            state_info_deserialize (&ctx->state, state_data, ctx->store);
            nndfs_color_t color = nn_get_color (&ctx->color_map, ctx->state.ref);
            if ( nn_color_eq(color, NNBLUE) ) {
                nn_set_color (&ctx->color_map, ctx->state.ref, NNPINK);
                nndfs_explore_state_red (ctx);
                ctx->red.explored++;
            } else {
                if (seed == ctx->state.ref)
                    break;
                dfs_stack_pop (ctx->stack);
            }
        } else { //backtrack
            dfs_stack_leave (ctx->stack);
            ctx->red.level_cur--;
            state_data = dfs_stack_top (ctx->stack);
            state_info_deserialize_cheap (&ctx->state, state_data);
            /* exit search if backtrack hits seed, leave stack the way it was */
            if (seed == ctx->state.ref)
                break;
            dfs_stack_pop (ctx->stack);
        }
    }
}

/* NNDFS dfs_blue */
void
nndfs_blue (wctx_t *ctx)
{
    while ( !lb2_is_stopped(lb2) ) {
        raw_data_t          state_data = dfs_stack_top (ctx->stack);
        if (NULL != state_data) {
            state_info_deserialize (&ctx->state, state_data, ctx->store);
            nndfs_color_t color = nn_get_color (&ctx->color_map, ctx->state.ref);
            if ( nn_color_eq(color, NNWHITE) ) {
                bitvector_set ( &ctx->all_red, ctx->counters.level_cur );
                nn_set_color (&ctx->color_map, ctx->state.ref, NNCYAN);
                nndfs_explore_state_blue (ctx);
            } else {
                if ( ctx->counters.level_cur != 0 && !nn_color_eq(color, NNPINK) )
                    bitvector_unset ( &ctx->all_red, ctx->counters.level_cur - 1);
                dfs_stack_pop (ctx->stack);
            }
        } else { //backtrack
            if (0 == dfs_stack_nframes (ctx->stack))
                return;
            dfs_stack_leave (ctx->stack);
            ctx->counters.level_cur--;
            state_data = dfs_stack_top (ctx->stack);
            state_info_deserialize (&ctx->state, state_data, ctx->store);
            if ( all_red && bitvector_is_set(&ctx->all_red, ctx->counters.level_cur) ) {
                /* exit if backtrack hits seed, leave stack the way it was */
                nn_set_color (&ctx->color_map, ctx->state.ref, NNPINK);
                ctx->counters.allred++;
                if ( GBbuchiIsAccepting(ctx->model, ctx->state.data) )
                    ctx->red.visited++;
            } else if ( GBbuchiIsAccepting(ctx->model, ctx->state.data) ) {
                /* call red DFS for accepting states */
                nndfs_red (ctx, ctx->state.ref);
                nn_set_color (&ctx->color_map, ctx->state.ref, NNPINK);
            } else {
                if (ctx->counters.level_cur > 0)
                    bitvector_unset (&ctx->all_red, ctx->counters.level_cur - 1);
                nn_set_color (&ctx->color_map, ctx->state.ref, NNBLUE);
            }
            dfs_stack_pop (ctx->stack);
        }
    }
}

/*
 * LNDFS by Laarman/Langerak/vdPol/Weber/Wijs (originally MCNDFS)
 *
 *  @incollection {springerlink:10.1007/978-3-642-24372-1_23,
       author = {Laarman, Alfons and Langerak, Rom and van de Pol, Jaco and Weber, Michael and Wijs, Anton},
       affiliation = {Formal Methods and Tools, University of Twente, The Netherlands},
       title = {{Multi-core Nested Depth-First Search}}},
       booktitle = {Automated Technology for Verification and Analysis},
       series = {Lecture Notes in Computer Science},
       editor = {Bultan, Tevfik and Hsiung, Pao-Ann},
       publisher = {Springer Berlin / Heidelberg},
       isbn = {978-3-642-24371-4},
       keyword = {Computer Science},
       pages = {321-335},
       volume = {6996},
       url = {http://dx.doi.org/10.1007/978-3-642-24372-1_23},
       note = {10.1007/978-3-642-24372-1_23},
       year = {2011}
    }
 */

/* LNDFS dfs_red */
static void
lndfs_red (wctx_t *ctx, ref_t seed)
{
    inc_wip (seed);
    while ( !lb2_is_stopped(lb2) ) {
        raw_data_t          state_data = dfs_stack_top (ctx->stack);
        if (NULL != state_data) {
            state_info_deserialize (&ctx->state, state_data, ctx->store);
            nndfs_color_t color = nn_get_color (&ctx->color_map, ctx->state.ref);
            if ( !nn_color_eq(color, NNPINK) &&
                 !global_has_color(ctx->state.ref, GRED, ctx->rec_bits) ) {
                nn_set_color (&ctx->color_map, ctx->state.ref, NNPINK);
                nndfs_explore_state_red (ctx);
            } else {
                if (seed == ctx->state.ref)
                    break;
                dfs_stack_pop (ctx->stack);
            }
        } else { //backtrack
            dfs_stack_leave (ctx->stack);
            ctx->red.level_cur--;
            state_data = dfs_stack_top (ctx->stack);
            state_info_deserialize_cheap (&ctx->state, state_data);
            if (seed == ctx->state.ref) {
                /* exit if backtrack hits seed, leave stack the way it was */
                dec_wip (seed);
                wait_seed (ctx, seed);
                if ( global_try_color(ctx->state.ref, GRED, ctx->rec_bits) )
                    ctx->red.visited++; //count accepting states
                return;
            }
            set_red (ctx, &ctx->state);
            dfs_stack_pop (ctx->stack);
        }
    }
    //halted by the load balancer
    dec_wip (seed);
}

/* LNDFS dfs_blue */
void
lndfs_blue (wctx_t *ctx)
{
    while ( !lb2_is_stopped(lb2) ) {
        raw_data_t          state_data = dfs_stack_top (ctx->stack);
        if (NULL != state_data) {
            state_info_deserialize (&ctx->state, state_data, ctx->store);
            nndfs_color_t color = nn_get_color (&ctx->color_map, ctx->state.ref);
            if ( nn_color_eq(color, NNWHITE) &&
                 !global_has_color(ctx->state.ref, GRED, ctx->rec_bits) ) {
                bitvector_set (&ctx->all_red, ctx->counters.level_cur);
                nn_set_color (&ctx->color_map, ctx->state.ref, NNCYAN);
                nndfs_explore_state_blue (ctx);
            } else {
                if ( ctx->counters.level_cur != 0 && !global_has_color(ctx->state.ref, GRED, ctx->rec_bits) )
                    bitvector_unset (&ctx->all_red, ctx->counters.level_cur - 1);
                dfs_stack_pop (ctx->stack);
            }
        } else { //backtrack
            if (0 == dfs_stack_nframes (ctx->stack))
                return;
            dfs_stack_leave (ctx->stack);
            ctx->counters.level_cur--;
            state_data = dfs_stack_top (ctx->stack);
            state_info_deserialize (&ctx->state, state_data, ctx->store);
            if ( all_red && bitvector_is_set(&ctx->all_red, ctx->counters.level_cur) ) {
                /* all successors are red */
                wait_seed (ctx, ctx->state.ref);
                set_all_red (ctx, &ctx->state);
            } else if ( GBbuchiIsAccepting(ctx->model, ctx->state.data) ) {
                /* call red DFS for accepting states */
                lndfs_red (ctx, ctx->state.ref);
            } else if (ctx->counters.level_cur > 0 &&
                       !global_has_color(ctx->state.ref, GRED, ctx->rec_bits)) {
                /* unset the all-red flag (only for non-initial nodes) */
                bitvector_unset (&ctx->all_red, ctx->counters.level_cur - 1);
            }
            nn_set_color (&ctx->color_map, ctx->state.ref, NNBLUE);
            dfs_stack_pop (ctx->stack);
        }
    }
}

extern void rec_ndfs_call (wctx_t *ctx, ref_t state);

/**
 * o Parallel NDFS algorithm by Evangelista/Pettruci/Youcef (ENDFS)
 * o Improved (Combination) NDFS algorithm (CNDFS).
     <Submitted to ATVA 2012>
 * o Combination of ENDFS and LNDFS (NMCNDFS)
     @inproceedings{pdmc11,
       month = {July},
       official_url = {http://dx.doi.org/10.4204/EPTCS.72.2},
       issn = {2075-2180},
       author = {A. W. {Laarman} and J. C. {van de Pol}},
       series = {Electronic Proceedings in Theoretical Computer Science},
       editor = {J. {Barnat} and K. {Heljanko}},
       title = {{Variations on Multi-Core Nested Depth-First Search}},
       address = {USA},
       publisher = {EPTCS},
       id_number = {10.4204/EPTCS.72.2},
       howpublished = {http://eprints.eemcs.utwente.nl/20618/},
       volume = {72},
       location = {Snowbird, Utah},
       booktitle = {Proceedings of the 10th International Workshop on Parallel and Distributed Methods in verifiCation, PDMC 2011, Snowbird, Utah},
       year = {2011},
       pages = {13--28}
      }
 */
static void
endfs_lb (wctx_t *ctx)
{
    atomic_write (&ctx->done, 1);
    size_t workers[W];
    int idle_count = W-1;
    for (size_t i = 0; i<((size_t)W); i++)
        workers[i] = (i==ctx->id ? 0 : 1);
    while (0 != idle_count)
    for (size_t i=0; i<W; i++) {
        if (0==workers[i])
            continue;
        if (1 == atomic_read(&(contexts[i]->done))) {
            workers[i] = 0;
            idle_count--;
            continue;
        }
        ref_t work = atomic_read (&contexts[i]->work);
        if (SIZE_MAX == work)
            continue;
        rec_ndfs_call (ctx, work);
    }
}

static void
endfs_handle_dangerous (wctx_t *ctx)
{
    while ( dfs_stack_size(ctx->in_stack) ) {
        raw_data_t state_data = dfs_stack_pop (ctx->in_stack);
        state_info_deserialize_cheap (&ctx->state, state_data);
        if ( !global_has_color(ctx->state.ref, GDANGEROUS, ctx->rec_bits) &&
              ctx->state.ref != ctx->seed )
            if (global_try_color(ctx->state.ref, GRED, ctx->rec_bits))
                ctx->red.explored++;
    }
    if (global_try_color(ctx->seed, GRED, ctx->rec_bits)) {
        ctx->red.explored++;
        ctx->red.visited++;
    }
    if ( global_has_color(ctx->seed, GDANGEROUS, ctx->rec_bits) ) {
        rec_ndfs_call (ctx, ctx->seed);
    }
}

static void
cndfs_handle_nonseed_accepting (wctx_t *ctx)
{
    size_t nonred, accs;
    nonred = accs = dfs_stack_size(ctx->out_stack);
    if (nonred) {
        ctx->red.waits++;
        ctx->counters.rec += accs;
    }
    if (nonred) {
        RTstartTimer (ctx->red.timer);
        while ( nonred && !lb2_is_stopped(lb2) ) {
            nonred = 0;
            for (size_t i = 0; i < accs; i++) {
                raw_data_t state_data = dfs_stack_peek (ctx->out_stack, i);
                state_info_deserialize_cheap (&ctx->state, state_data);
                if (!global_has_color(ctx->state.ref, GRED, ctx->rec_bits))
                    nonred++;
            }
        }
        RTstopTimer (ctx->red.timer);
    }
    for (size_t i = 0; i < accs; i++)
        dfs_stack_pop (ctx->out_stack);
    while ( dfs_stack_size(ctx->in_stack) ) {
        raw_data_t state_data = dfs_stack_pop (ctx->in_stack);
        state_info_deserialize_cheap (&ctx->state, state_data);
        if (global_try_color(ctx->state.ref, GRED, ctx->rec_bits))
            ctx->red.explored++;
    }
}

static void
endfs_handle_red (void *arg, state_info_t *successor, transition_info_t *ti, int seen)
{
    wctx_t             *ctx = (wctx_t *) arg;
    /* Find cycle back to the seed */
    nndfs_color_t color = nn_get_color (&ctx->color_map, successor->ref);
    if ( nn_color_eq(color, NNCYAN) )
        ndfs_report_cycle (ctx, successor);
    /* Mark states dangerous if necessary */
    if ( Strat_ENDFS == ctx->strategy &&
         GBbuchiIsAccepting(ctx->model, get(dbs, successor->ref, ctx->store2)) &&
         !global_has_color(successor->ref, GRED, ctx->rec_bits) )
        global_try_color(successor->ref, GDANGEROUS, ctx->rec_bits);
    if ( !nn_color_eq(color, NNPINK) &&
         !global_has_color(successor->ref, GRED, ctx->rec_bits) ) {
        raw_data_t stack_loc = dfs_stack_push (ctx->stack, NULL);
        state_info_serialize (successor, stack_loc);
    }
    (void) ti; (void) seen;
}

static void
endfs_handle_blue (void *arg, state_info_t *successor, transition_info_t *ti, int seen)
{
    wctx_t             *ctx = (wctx_t *) arg;
    nndfs_color_t color = nn_get_color (&ctx->color_map, successor->ref);
    /**
     * The following lines bear little resemblance to the algorithms in the
     * respective papers (Evangelista et al./ Laarman et al.), but we must
     * store all non-red states on the stack in order to calculate
     * all-red correctly later. Red states are also stored as optimization.
     */
    if ( nn_color_eq(color, NNCYAN) &&
         (GBbuchiIsAccepting(ctx->model, ctx->state.data) ||
         GBbuchiIsAccepting(ctx->model, get(dbs, successor->ref, ctx->store2))) ) {
        /* Found cycle in blue search */
        ndfs_report_cycle(ctx, successor);
    } else if ( all_red || (!nn_color_eq(color, NNCYAN) && !nn_color_eq(color, NNBLUE) &&
                            !global_has_color(successor->ref, GGREEN, ctx->rec_bits)) ) {
        raw_data_t stack_loc = dfs_stack_push (ctx->stack, NULL);
        state_info_serialize (successor, stack_loc);
    }
    (void) ti; (void) seen;
}

static inline void
endfs_explore_state_red (wctx_t *ctx, counter_t *cnt)
{
    dfs_stack_enter (ctx->stack);
    increase_level (cnt);
    ctx->permute->permutation = permutation_red;
    cnt->trans += permute_trans (ctx->permute, &ctx->state, endfs_handle_red, ctx);
    ndfs_maybe_report ("[R] ", cnt);
}

static inline void
endfs_explore_state_blue (wctx_t *ctx, counter_t *cnt)
{
    dfs_stack_enter (ctx->stack);
    increase_level (cnt);
    ctx->permute->permutation = permutation;
    cnt->trans += permute_trans (ctx->permute, &ctx->state, endfs_handle_blue, ctx);
    cnt->explored++;
    ndfs_maybe_report ("[B] ", cnt);
}

/* ENDFS dfs_red */
static void
endfs_red (wctx_t *ctx, ref_t seed)
{
    size_t              seed_level = dfs_stack_nframes (ctx->stack);
    while ( !lb2_is_stopped(lb2) ) {
        raw_data_t          state_data = dfs_stack_top (ctx->stack);
        if (NULL != state_data) {
            state_info_deserialize (&ctx->state, state_data, ctx->store);
            nndfs_color_t color = nn_get_color (&ctx->color_map, ctx->state.ref);
            if ( !nn_color_eq(color, NNPINK) &&
                 !global_has_color(ctx->state.ref, GRED, ctx->rec_bits) ) {
                nn_set_color (&ctx->color_map, ctx->state.ref, NNPINK);
                dfs_stack_push (ctx->in_stack, state_data);
                if ( Strat_CNDFS == strategy[0] && ctx->state.ref != seed && 
                     GBbuchiIsAccepting(ctx->model, ctx->state.data) )
                    dfs_stack_push (ctx->out_stack, state_data);
                endfs_explore_state_red (ctx, &ctx->red);
            } else {
                if (seed_level == dfs_stack_nframes (ctx->stack))
                    break;
                dfs_stack_pop (ctx->stack);
            }
        } else { //backtrack
            dfs_stack_leave (ctx->stack);
            ctx->red.level_cur--;
            /* exit search if backtrack hits seed, leave stack the way it was */
            if (seed_level == dfs_stack_nframes(ctx->stack))
                break;
            dfs_stack_pop (ctx->stack);
        }
    }
}

void // just for checking correctness of all-red implementation. Unused.
check (void *arg, state_info_t *successor, transition_info_t *ti, int seen)
{
    wctx_t *ctx=arg;
    assert (global_has_color(successor->ref, GRED, ctx->rec_bits) );
    (void) ti; (void) seen;
}

/* ENDFS dfs_blue */
void
endfs_blue (wctx_t *ctx)
{
    ctx->done = 0;
    while ( !lb2_is_stopped(lb2) ) {
        raw_data_t          state_data = dfs_stack_top (ctx->stack);
        if (NULL != state_data) {
            state_info_deserialize (&ctx->state, state_data, ctx->store);
            nndfs_color_t color = nn_get_color (&ctx->color_map, ctx->state.ref);
            if ( !nn_color_eq(color, NNCYAN) && !nn_color_eq(color, NNBLUE) &&
                 !global_has_color(ctx->state.ref, GGREEN, ctx->rec_bits) ) {
                if (all_red)
                    bitvector_set (&ctx->all_red, ctx->counters.level_cur);
                nn_set_color (&ctx->color_map, ctx->state.ref, NNCYAN);
                endfs_explore_state_blue (ctx, &ctx->counters);
            } else {
                if ( all_red && ctx->counters.level_cur != 0 && !global_has_color(ctx->state.ref, GRED, ctx->rec_bits) )
                    bitvector_unset (&ctx->all_red, ctx->counters.level_cur - 1);
                dfs_stack_pop (ctx->stack);
            }
        } else { //backtrack
            if (0 == dfs_stack_nframes(ctx->stack))
                break;
            dfs_stack_leave (ctx->stack);
            ctx->counters.level_cur--;
            /* call red DFS for accepting states */
            state_data = dfs_stack_top (ctx->stack);
            state_info_deserialize (&ctx->state, state_data, ctx->store);
            /* Mark state GGREEN on backtrack */
            global_try_color (ctx->state.ref, GGREEN, ctx->rec_bits);
            nn_set_color (&ctx->color_map, ctx->state.ref, NNBLUE);
            if ( all_red && bitvector_is_set(&ctx->all_red, ctx->counters.level_cur) ) {
                /* all successors are red */
                //permute_trans (ctx->permute, &ctx->state, check, ctx); 
                set_all_red (ctx, &ctx->state);
            } else if ( GBbuchiIsAccepting(ctx->model, ctx->state.data) ) {
                ref_t           seed = ctx->state.ref;
                ctx->seed = ctx->work = seed;
                endfs_red (ctx, seed);
                if (Strat_ENDFS == ctx->strategy)
                    endfs_handle_dangerous (ctx);
                else
                    cndfs_handle_nonseed_accepting (ctx);
                ctx->work = SIZE_MAX;
            } else if (all_red && ctx->counters.level_cur > 0 &&
                       !global_has_color(ctx->state.ref, GRED, ctx->rec_bits)) { 
                /* unset the all-red flag (only for non-initial nodes) */
                bitvector_unset (&ctx->all_red, ctx->counters.level_cur - 1);
            }
            dfs_stack_pop (ctx->stack);
        }
    }
    if ( Strat_ENDFS == ctx->strategy &&            // if ENDFS,
         ctx == contexts[ctx->id] &&                // if top-level ENDFS, and
         (Strat_LTLG & ctx->rec_ctx->strategy) )    // if rec strategy uses global bits (global pruning)
        endfs_lb (ctx);                             // then do simple load balancing
}

void
rec_ndfs_call (wctx_t *ctx, ref_t state)
{
    dfs_stack_push (ctx->rec_ctx->stack, (int*)&state);
    ctx->counters.rec++;
    switch (ctx->rec_ctx->strategy) {
    case Strat_ENDFS:
       endfs_blue (ctx->rec_ctx); break;
    case Strat_LNDFS:
       lndfs_blue (ctx->rec_ctx); break;
    case Strat_NNDFS:
       nndfs_blue (ctx->rec_ctx); break;
    case Strat_NDFS:
       ndfs_blue (ctx->rec_ctx); break;
    default:
       Abort ("Invalid recursive strategy.");
    }
}

/*
 * Reachability algorithms
 *  @incollection {springerlink:10.1007/978-3-642-20398-5_40,
     author = {Laarman, Alfons and van de Pol, Jaco and Weber, Michael},
     affiliation = {Formal Methods and Tools, University of Twente, The Netherlands},
     title = {{Multi-Core LTSmin: Marrying Modularity and Scalability}},
     booktitle = {NASA Formal Methods},
     series = {Lecture Notes in Computer Science},
     editor = {Bobaru, Mihaela and Havelund, Klaus and Holzmann, Gerard and Joshi, Rajeev},
     publisher = {Springer Berlin / Heidelberg},
     isbn = {978-3-642-20397-8},
     keyword = {Computer Science},
     pages = {506-511},
     volume = {6617},
     url = {http://dx.doi.org/10.1007/978-3-642-20398-5_40},
     note = {10.1007/978-3-642-20398-5_40},
     year = {2011}
   }
 */
size_t
split_bfs (void *arg_src, void *arg_tgt, size_t handoff)
{
    wctx_t             *source = arg_src;
    wctx_t             *target = arg_tgt;
    dfs_stack_t         source_stack = source->in_stack;
    size_t              in_size = dfs_stack_size (source_stack);
    if (in_size < 2) {
        in_size = dfs_stack_size (source->out_stack);
        source_stack = source->out_stack;
    }
    if (BACKOFF && (Strat_TA & strategy[0]) && in_size < handoff) {
        in_size = dfs_stack_frame_size (source->backoff);
        for (size_t i = 0; i < in_size; i++)
            dfs_stack_push (source_stack, dfs_stack_pop(source->backoff));
    }
    handoff = min (in_size >> 1 , handoff);
    for (size_t i = 0; i < handoff; i++) {
        state_data_t        one = dfs_stack_pop (source_stack);
        assert (NULL != one);
        dfs_stack_push (target->stack, one);
    }
    source->counters.splits++;
    source->counters.transfer += handoff;
    return handoff;
}

size_t
split_sbfs (void *arg_src, void *arg_tgt, size_t handoff)
{
    wctx_t             *source = arg_src;
    wctx_t             *target = arg_tgt;
    size_t              in_size = dfs_stack_size (source->in_stack);
    if (BACKOFF && (Strat_TA & strategy[0]) && in_size < handoff) {
        in_size = dfs_stack_frame_size (source->backoff);
        for (size_t i = 0; i < in_size; i++)
            dfs_stack_push (source->in_stack, dfs_stack_pop(source->backoff));
    }
    handoff = min (in_size >> 1 , handoff);
    for (size_t i = 0; i < handoff; i++) {
        state_data_t        one = dfs_stack_pop (source->in_stack);
        assert (NULL != one);
        dfs_stack_push (target->in_stack, one);
    }
    source->counters.splits++;
    source->counters.transfer += handoff;
    return handoff;
}

size_t
split_dfs (void *arg_src, void *arg_tgt, size_t handoff)
{
    wctx_t             *source = arg_src;
    wctx_t             *target = arg_tgt;
    size_t              in_size = dfs_stack_size (source->stack);
    if (BACKOFF && (Strat_TA & strategy[0]) && in_size < handoff) {
        in_size = dfs_stack_frame_size (source->backoff);
        for (size_t i = 0; i < in_size; i++)
            dfs_stack_push (source->in_stack, dfs_stack_pop(source->backoff));
    }
    handoff = min (in_size >> 1, handoff);
    for (size_t i = 0; i < handoff; i++) {
        state_data_t        one = dfs_stack_top (source->stack);
        if (!one) {
            if (UseGreyBox == call_mode) {
                int *next_index = isba_pop_int (source->group_stack);
                isba_push_int (target->group_stack, next_index);
            }
            dfs_stack_leave (source->stack);
            source->counters.level_cur--;
            one = dfs_stack_pop (source->stack);
            dfs_stack_push (target->stack, one);
            dfs_stack_enter (target->stack);
            target->counters.level_cur++;
        } else {
            dfs_stack_push (target->stack, one);
            dfs_stack_pop (source->stack);
        }
    }
    source->counters.splits++;
    source->counters.transfer += handoff;
    return handoff;
}

static inline size_t
bfs_load (wctx_t *ctx)
{
    return dfs_stack_frame_size(ctx->in_stack) + dfs_stack_frame_size(ctx->out_stack);
}

static inline int
valid_end_state(wctx_t *ctx, raw_data_t state)
{
#if defined(SPINJA)
    return GBbuchiIsAccepting(ctx->model, state);
#endif
    return false;
    (void) ctx; (void) state;
}

static inline void
deadlock_detect (wctx_t *ctx, int count)
{
    if (count > 0 || valid_end_state(ctx, ctx->state.data)) return;
    ctx->counters.deadlocks++; // counting is costless
    if (dlk_detect && (!no_exit || trc_output) && lb2_stop(lb2)) {
        Warning (info, "");
        Warning (info, "Deadlock found in state at depth %zu!", ctx->counters.level_cur);
        Warning (info, "");
        if (trc_output) handle_error_trace (ctx);
    }
}

static inline void
invariant_detect (wctx_t *ctx, raw_data_t state)
{
    if ( !inv_expr || eval_predicate(inv_expr, NULL, state) ) return;
    ctx->counters.violations++;
    if ((!no_exit || trc_output) && lb2_stop(lb2)) {
        Warning (info, "");
        Warning (info, "Invariant violation (%s) found at depth %zu!", inv_detect, ctx->counters.level_cur);
        Warning (info, "");
        if (trc_output) handle_error_trace (ctx);
    }
}

static inline void
action_detect (wctx_t *ctx, transition_info_t *ti, ref_t last)
{
    if (-1 == act_index || NULL == ti->labels || 0 == ti->labels[act_index]) return;
    ctx->counters.errors++;
    if ((!no_exit || trc_output) && lb2_stop(lb2)) {
        ctx->state.ref = last; // TODO: include the action in the trace
        chunk c = GBchunkGet (ctx->model, act_type, ti->labels[act_index]);
        char value[4096];
        chunk2string(c, 4096, value);
        Warning (info, "");
        Warning (info, "Error action %s with value %s found at depth %zu!", act_detect, value, ctx->counters.level_cur);
        Warning (info, "");
        if (trc_output) handle_error_trace (ctx);
    }
}

static void
reach_handle (void *arg, state_info_t *successor, transition_info_t *ti,
              int seen)
{
    wctx_t             *ctx = (wctx_t *) arg;
    if (!seen) {
        raw_data_t stack_loc = dfs_stack_push (ctx->stack, NULL);
        state_info_serialize (successor, stack_loc);
        if (trc_output)
            parent_ref[successor->ref] = ctx->state.ref;
        ctx->counters.visited++;
    }
    action_detect (ctx, ti, successor->ref);
    ctx->counters.trans++;
    (void) ti;
}

static void
reach_handle_wrap (void *arg, transition_info_t *ti, state_data_t data)
{
    wctx_t             *ctx = (wctx_t *) arg;
    state_info_t        successor;
    int                 seen;
    seen = state_info_initialize (&successor, data, ti, &ctx->state, ctx);
    if (seen < 0)
        if (lb2_stop(lb2)) Warning (info, "Error: %s full! Change -s or --ratio.", full_msg(seen));
    reach_handle (arg, &successor, ti, seen);
}

static inline size_t
explore_state (wctx_t *ctx, raw_data_t state, int next_index)
{
    size_t              count = 0;
    size_t              i = K;
    state_info_deserialize (&ctx->state, state, ctx->store);
    if (0 == next_index) { // first (grey) call with this state
        invariant_detect (ctx, ctx->state.data);
        if (ctx->counters.level_cur >= max) return K;
    }
    if ( UseBlackBox == call_mode )
        count = permute_trans (ctx->permute, &ctx->state, reach_handle, ctx);
    else // UseGreyBox
        for (i = next_index; i<K && count<MAX_SUCC; i++)
            count += GBgetTransitionsLong (ctx->model, i, ctx->state.data,
                                           reach_handle_wrap, ctx);
    if (0 == next_index) // last (grey) call with this state
        deadlock_detect (ctx, count);
    maybe_report (&ctx->counters, "", &threshold);
    return i;
}

void
dfs_grey (wctx_t *ctx)
{
    uint32_t            next_index = 0;
    while (lb2_balance(lb2, ctx->id, dfs_stack_size(ctx->stack), split_dfs)) {
        raw_data_t          state_data = dfs_stack_top (ctx->stack);
        if (NULL == state_data) {
            if (0 == dfs_stack_nframes (ctx->stack))
                return;
            dfs_stack_leave (ctx->stack);
            ctx->counters.level_cur--;
            next_index = isba_pop_int (ctx->group_stack)[0];
            continue;
        }
        if (next_index == K) {
            ctx->counters.explored++;
            dfs_stack_pop (ctx->stack);
        } else {
            dfs_stack_enter (ctx->stack);
            increase_level (&ctx->counters);
            next_index = explore_state (ctx, state_data, next_index);
            isba_push_int (ctx->group_stack, (int *)&next_index);
        }
        next_index = 0;
    }
}

void
dfs (wctx_t *ctx)
{
    while (lb2_balance(lb2, ctx->id, dfs_stack_size(ctx->stack), split_dfs)) {
        raw_data_t          state_data = dfs_stack_top (ctx->stack);
        if (NULL != state_data) {
            dfs_stack_enter (ctx->stack);
            increase_level (&ctx->counters);
            explore_state (ctx, state_data, 0);
            ctx->counters.explored++;
        } else {
            if (0 == dfs_stack_nframes (ctx->stack))
                return;
            dfs_stack_leave (ctx->stack);
            ctx->counters.level_cur--;
            dfs_stack_pop (ctx->stack);
        }
    }
}

void
bfs (wctx_t *ctx)
{
    while (lb2_balance(lb2, ctx->id, bfs_load(ctx), split_bfs)) {
        raw_data_t          state_data = dfs_stack_pop (ctx->in_stack);
        if (NULL != state_data) {
            explore_state (ctx, state_data, 0);
            ctx->counters.explored++;
        } else {
            if (0 == dfs_stack_frame_size (ctx->out_stack))
                return;
            dfs_stack_t     old = ctx->out_stack;
            ctx->stack = ctx->out_stack = ctx->in_stack;
            ctx->in_stack = old;
            increase_level (&ctx->counters);
        }
    }
}

void
sbfs (wctx_t *ctx)
{
    size_t              total = 0;
    size_t              out_size;
    do {
        while (lb2_balance(lb2, ctx->id, dfs_stack_frame_size(ctx->in_stack), split_sbfs)) {
            raw_data_t          state_data = dfs_stack_pop (ctx->in_stack);
            if (NULL != state_data) {
                explore_state (ctx, state_data, 0);
                ctx->counters.explored++;
            }
        }
        out_size = lb2_reduce (dfs_stack_frame_size(ctx->out_stack), W);
        if (0 == ctx->id && out_size > max_level_size) max_level_size = out_size;
        lb2_reinit (lb2, ctx->id);
        increase_level (&ctx->counters);
        if (0 == ctx->id) {
            total += out_size;
            Warning(infoLong, "BFS level %zu has %zu states %zu total", ctx->counters.level_cur, out_size, total);
        }
        dfs_stack_t     old = ctx->out_stack;
        ctx->stack = ctx->out_stack = ctx->in_stack;
        ctx->in_stack = old;
    } while (out_size > 0 && !lb2_is_stopped(lb2));
}

/**
 * Multi-core reachability algorithm for timed automata.
 * <Submitted to FORMATS 2012>
 */

typedef enum ta_set_e {
    TA_WAITING = 0,
    TA_PASSED  = 1,
} ta_set_e_t;

static inline int
backoff_or_lock (wctx_t *ctx, state_info_t *state) {
    if (BACKOFF) {
        if (!lm_try_lock(lmap, state->ref)) {
            raw_data_t stack_loc = dfs_stack_push (ctx->backoff, NULL);
            state_info_serialize (state, stack_loc);
            ctx->counters.delayed++;
            return 1;
        }
    } else {
        lm_lock (lmap, state->ref);
    }
    return 0;
}

lm_cb_t
ta_covered (void *arg, lattice_t l, lm_status_t status, lm_loc_t loc)
{
    wctx_t         *ctx = (wctx_t*) arg;
    int *succ_l = (int*)&ctx->successor->lattice;
    if (GBisCoveredByShort(ctx->model, succ_l, (int*)&l) ) {
        ctx->done = 1;
        return LM_CB_STOP; //A l' : (E (s,l)eL : l>=l')=>(A (s,l)eL : l>=l')
    } else if (TA_UPDATE_NONE != UPDATE &&
            (TA_UPDATE_PASSED == UPDATE || TA_WAITING == (ta_set_e_t)status) &&
            GBisCoveredByShort(ctx->model, (int*)&l, succ_l)) {
        lm_delete (lmap, loc);
        ctx->last = (LM_NULL_LOC == ctx->last ? loc : ctx->last);
        ctx->counters.deletes++;
    }
    ctx->work++;
    return LM_CB_NEXT;
}

static void
ta_handle (void *arg, state_info_t *successor, transition_info_t *ti, int seen)
{
    wctx_t         *ctx = (wctx_t*) arg;
    ctx->done = 0;
    ctx->work = 0;
    ctx->last = LM_NULL_LOC;
    ctx->successor = successor;
    if (backoff_or_lock(ctx, successor))
        return;
    lm_loc_t last = lm_iterate (lmap, successor->ref, ta_covered, ctx);
    if (!ctx->done) {
        last = (LM_NULL_LOC == ctx->last ? last : ctx->last);
        ctx->counters.inserts++;
        successor->loc = lm_insert_from (lmap, successor->ref,
                                        successor->lattice, TA_WAITING, &last);
        lm_unlock (lmap, successor->ref);
        if (STDEV) { // quite costly: flops
            if (ctx->work > 0)
                statistics_unrecord (&ctx->counters.lattice_ratio, ctx->work);
            statistics_record (&ctx->counters.lattice_ratio, ctx->work+1);
        }
        raw_data_t stack_loc = dfs_stack_push (ctx->stack, NULL);
        state_info_serialize (successor, stack_loc);
        if (trc_output)
            parent_ref[successor->ref] = ctx->state.ref; // TODO: for backoffs!
        ctx->counters.updates += LM_NULL_LOC != ctx->last;
        ctx->counters.visited++;
    } else {
       lm_unlock (lmap, successor->ref);
    }
    action_detect (ctx, ti, successor->ref);
    ctx->counters.trans++;
    (void) ti; (void) seen;
}

static inline int
is_waiting (wctx_t *ctx, raw_data_t state_data)
{
    state_info_deserialize (&ctx->state, state_data, ctx->store);
    if (LM_NULL_LOC == ctx->state.loc) {
        ta_handle (ctx, &ctx->state, NULL, 0);
        return !ctx->done;
    }
    if (TA_UPDATE_NONE == UPDATE)
        return 1; // we don't need to update the global waiting info
    if (backoff_or_lock(ctx, &ctx->state))
        return 0; // pretend the state is already in the waiting set
    int ret_val = 0;
    lattice_t lattice = lm_get (lmap, ctx->state.loc);
    if (ctx->state.lattice == lattice && // maybe changed or LATTICE_NULL!
            TA_WAITING == (ta_set_e_t)lm_get_status (lmap, ctx->state.loc)) {
        lm_set_status (lmap, ctx->state.loc, TA_PASSED);
        ret_val = 1;
    } else {
        ret_val = 0;
    }
    lm_unlock (lmap, ctx->state.ref);
    return ret_val;
}

static inline void
ta_explore_state (wctx_t *ctx)
{
    int                 count = 0;
    invariant_detect (ctx, ctx->state.data);
    count = permute_trans (ctx->permute, &ctx->state, ta_handle, ctx);
    deadlock_detect (ctx, count);
    maybe_report (&ctx->counters, "", &threshold);
    ctx->counters.explored++;
}

static inline size_t
ta_load (wctx_t *ctx, dfs_stack_t stack)
{
    return dfs_stack_frame_size(stack) +
        (BACKOFF ? dfs_stack_frame_size(ctx->backoff) : 0);
}

static inline size_t
ta_bfs_load (wctx_t *ctx)
{
    return dfs_stack_frame_size(ctx->in_stack) + dfs_stack_frame_size(ctx->out_stack) +
        (BACKOFF ? dfs_stack_frame_size(ctx->backoff) : 0);
}

void
ta_dfs (wctx_t *ctx)
{
    while (lb2_balance(lb2, ctx->id, ta_load(ctx, ctx->stack), split_dfs)) {
        raw_data_t          state_data = dfs_stack_top (ctx->stack);
        if (NULL != state_data) {
            if (is_waiting(ctx, state_data)) {
                dfs_stack_enter (ctx->stack);
                increase_level (&ctx->counters);
                ta_explore_state (ctx);
            } else {
                dfs_stack_pop (ctx->stack);
            }
        } else {
            if (0 == dfs_stack_size (ctx->stack)) {
                if (0 == dfs_stack_frame_size (ctx->backoff)) {
                    return;
                } else {
                    while (0 != dfs_stack_frame_size (ctx->backoff))
                        dfs_stack_push (ctx->stack, dfs_stack_pop(ctx->backoff));
                    continue;
                }
            }
            dfs_stack_leave (ctx->stack);
            ctx->counters.level_cur--;
            dfs_stack_pop (ctx->stack);
        }
    }
}

void
ta_bfs (wctx_t *ctx)
{
    while (lb2_balance(lb2, ctx->id, ta_bfs_load(ctx), split_bfs)) {
        raw_data_t          state_data = dfs_stack_pop (ctx->in_stack);
        if (NULL != state_data) {
            if (is_waiting(ctx, state_data)) {
                ta_explore_state (ctx);
            }
        } else {
            if (0 == dfs_stack_frame_size (ctx->out_stack)) {
                if (0 == dfs_stack_frame_size (ctx->backoff)) {
                    return;
                } else {
                    while (0 != dfs_stack_frame_size (ctx->backoff))
                        dfs_stack_push (ctx->in_stack, dfs_stack_pop(ctx->backoff));
                    continue;
                }
            }
            dfs_stack_t     old = ctx->out_stack;
            ctx->stack = ctx->out_stack = ctx->in_stack;
            ctx->in_stack = old;
            increase_level (&ctx->counters);
        }
    }
}

void
ta_bfs_strict (wctx_t *ctx)
{
    size_t out_size;
    do {
        while (lb2_balance(lb2, ctx->id, ta_load(ctx, ctx->in_stack), split_sbfs)) {
            raw_data_t          state_data = dfs_stack_pop (ctx->in_stack);
            if (NULL != state_data) {
                if (is_waiting(ctx, state_data)) {
                    ta_explore_state (ctx);
                }
            } else {
                while (0 != dfs_stack_frame_size (ctx->backoff)) {
                    dfs_stack_push (ctx->in_stack, dfs_stack_pop(ctx->backoff));
                }
            }
        }
        out_size = lb2_reduce (dfs_stack_frame_size(ctx->out_stack), W);
        if (0 == ctx->id && out_size > max_level_size) max_level_size = out_size;
        lb2_reinit (lb2, ctx->id);
        increase_level (&ctx->counters);
        if (0 == ctx->id && log_active(infoLong))
            Warning(info, "BFS level %zu has %zu states", ctx->counters.level_cur, out_size);
        dfs_stack_t     old = ctx->out_stack;
        ctx->stack = ctx->out_stack = ctx->in_stack;
        ctx->in_stack = old;
    } while (out_size > 0 && !lb2_is_stopped(lb2));
}

/* explore is started for each thread (worker) */
static void
explore (size_t id)
{
    wctx_t             *ctx = wctx_create (id, 0, NULL);// after global init
    ctx->timer = RTcreateTimer ();
    contexts[id] = ctx;
    transition_info_t   ti = GB_NO_TRANSITION;
    if (0 == ctx->id)                                   // before alg. start:
        state_info_initialize (&initial_state, initial_data, &ti, &ctx->state, ctx);
    lb2_local_init (lb2, ctx->id, ctx);                 // BARRIER

    if ( Strat_LTL & strategy[0] )
        ndfs_handle_blue (ctx, &initial_state, &ti, 0);
    else if (0 == ctx->id) { // only w1 receives load, as it is propagated later
        if ( Strat_TA & strategy[0] )
            ta_handle (ctx, &initial_state, &ti, 0);
        else
            reach_handle (ctx, &initial_state, &ti, 0);
    }
    ctx->counters.trans = 0; //reset trans count
    RTstartTimer (ctx->timer);
    switch (strategy[0]) {
    case Strat_TA_SBFS: ta_bfs_strict (ctx); break;
    case Strat_TA_BFS:  ta_bfs (ctx); break;
    case Strat_TA_DFS:  ta_dfs (ctx); break;
    case Strat_SBFS:    sbfs (ctx); break;
    case Strat_BFS:     bfs (ctx); break;
    case Strat_DFS:
        if (UseGreyBox == call_mode) dfs_grey (ctx); else dfs (ctx); break;
    case Strat_NDFS:    ndfs_blue (ctx); break;
    case Strat_NNDFS:   nndfs_blue (ctx); break;
    case Strat_LNDFS:   lndfs_blue (ctx); break;
    case Strat_CNDFS:
    case Strat_ENDFS:   endfs_blue (ctx); break;
    default: Abort ("Unknown or front-end incompatible strategy (%d).", strategy[0]);
    }
    RTstopTimer (ctx->timer);
    ctx->counters.runtime = RTrealTime (ctx->timer);
    print_thread_statistics (ctx);
    ctx->stats = statistics (dbs);
}

int
main (int argc, char *argv[])
{
    /* Init structures */
    HREinitBegin(argv[0]);
    HREaddOptions(options,"Perform a parallel reachability analysis of <model>\n\nOptions");
    HREenableThreads(1);
    HREinitStart(&argc,&argv,1,1,files,"<model>");      // spawns threads!

    size_t              id = HREme (HREglobal());

    if (0 == id) init_globals ();                       // global init

    lb2_barrier (HREpeers(HREglobal()));                // BARRIER

    explore (id);

    lb2_barrier (HREpeers(HREglobal()));                // BARRIER

    if (0 != id) return EXIT_SUCCESS;                   // local exit

    counter_t          *reach = RTmallocZero (sizeof(counter_t[MAX_STRATEGIES]));
    counter_t          *red = RTmallocZero (sizeof(counter_t[MAX_STRATEGIES]));
    stats_t            *stats = RTmallocZero (sizeof (stats_t));
    for (size_t i = 0; i < W; i++)
        ctx_add_counters (contexts[i], reach, red, stats);
    if (log_active(info))
        print_statistics (reach, red, contexts[0]->timer, stats);
    deinit_globals ();
}                                                       // global exit