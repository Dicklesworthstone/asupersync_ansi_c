/*
 * test_metamorphic.c — metamorphic relations M1 to M4 on the C interpreter
 * (bd-9kll.12.1).
 *
 * A metamorphic relation needs no Rust capture: it takes a scenario, applies
 * a transform that must not change what the scenario means, runs both
 * through asx_conformance_run and compares the projections (trace, snapshot
 * and observations, docs/CANONICAL_VOCABULARY_V2.md). A violation is a C
 * runtime that lets something other than a task's own program decide its
 * outcome: an arena index, a spawn count, a slot reused.
 *
 *   M1  Permuting the spawn order of mutually independent top-level tasks
 *       keeps the whole projection: every task's outcome, every obligation's
 *       resolution, every observation and the Foata-canonical trace.
 *   M2  Inserting an independent no-op task (a task with an empty program)
 *       changes no other entity: the projection without the no-op's own
 *       task, region and events is the original.
 *   M3  A region with budget b opened inside one with budget a is the one
 *       region with budget meet(a, b): generated scenarios, a task spawned
 *       into each form, same outcome, observations and dispatch order.
 *   M4  A weaker cancel request (kind User) after a stronger one changes
 *       nothing: the reasons strengthen monotonically and the cleanup
 *       budgets meet. Corpus scenarios whose driver script cancels, with
 *       the weaker request inserted after each; the converse (a stronger
 *       one after a weaker) must be seen to change the reason.
 *   M5 (a profile substitution keeps the digest) is `make profile-parity`.
 *
 * Which tasks are independent is read from the scenario text alone. A
 * name a task's program mentions that is declared at the top level (a
 * channel, a sync primitive, a region, another task, a finalizer) is shared
 * with whoever else mentions it, and a name introduced by a step's "as" is
 * shared with every other task that mentions it without introducing it; a
 * region someone names, cancels or finalizes is shared by its tasks. A task
 * that shares nothing with any other is isolated. The analysis is
 * conservative: a pair it cannot prove disjoint is dependent, and M1
 * permutes only isolated tasks among their own positions.
 *
 * M1, M2 and M4 hold only where the scenario's outcome does not depend on
 * how the lab breaks ties: adding or reordering tasks legitimately changes
 * which of two contenders the seeded pick serves first. A scenario whose
 * semantic digest changes with the seed alone is skipped as schedule
 * sensitive (counted, never passed), and so is one the C runtime does not
 * run at all or one its profile forbids. M4 is also out of scope where a
 * scenario sets a deadline or a quota: a budget cancel a task raises itself
 * lives in its Cx ahead of its record, a second request folds it in, and
 * the extra poll spends quota. The test fails closed on a corpus that
 * exercises none of the relations.
 *
 * The negative controls: the comparator must see a planted difference in
 * each projection part, the converse of M4 must be seen, and
 * tests/gates/test_metamorphic_mutants.sh builds this test against one
 * runtime bug per relation (an order dependence for M1 and M2, a budget
 * meet where the child replaces the parent for M3, a cancel where the last
 * request wins for M4) and expects the relation to report it.
 *
 * Run from the repository root; with paths as arguments it checks those
 * scenarios instead of every .json file of tests/conformance/scenarios_v2.
 * Setting ASX_METAMORPHIC_VERBOSE prints every skip with its reason.
 *
 * `--fuzz [FILE|-]` checks scenarios read as JSON lines instead (make
 * fuzz-metamorphic: twin_run generate --shape meta --jsonl): until every
 * relation has ASX_METAMORPHIC_CASES compared cases (10000), each `mm-m3-*`
 * line an M3 case whose budgets are read back, the rest through M1, M2 and
 * M4. The first violation of a relation is reduced by ddmin and written to
 * ASX_METAMORPHIC_OUT as a scenario. ASX_METAMORPHIC_SEEDS sets the seeds a
 * baseline is probed with (default 32).
 *
 * SPDX-License-Identifier: MIT
 */

#define _POSIX_C_SOURCE 200809L

#include "../../test_harness.h"
#include "conformance/interpreter.h"

#include <asx/asx_config.h>

#include <dirent.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#define MM_DIR "tests/conformance/scenarios_v2"
#define MM_MAX_SCENARIOS 1024u
#define MM_PATH_CAP 256u
#define MM_FILE_CAP (256u * 1024u)
#define MM_MAX_TASKS 64u
#define MM_MAX_REGIONS 64u
#define MM_MAX_NAMES 512u
/* Seeds a scenario is probed with. A rare flip needs many: with 16 the
 * corpus still hid one (race-deadline-wrapper-cx-reason-001), with 32 it
 * does not. */
#ifndef MM_SEEDS
#define MM_SEEDS 32u
#endif
#define MM_MAX_EVENTS 16384u
#define MM_POOL_CAP (4u * 1024u * 1024u)
#define MM_CANON_CAP (1024u * 1024u)
#define MM_NOOP_TASK "t.mm.noop"
#define MM_NOOP_REGION "r.mm.noop"
/* The no-op spends a few lab steps of its own. */
#define MM_NOOP_STEPS 16u
/* Compared variants the corpus must yield: fewer means the relation was
 * exercised too little to trust (most of the corpus is skipped when it is
 * schedule sensitive, does not fit the resource class or shares names).
 * The default class compares 30 M1 and 390 M2 variants (scratch run of
 * 786b3a6); these leave room for the corpus to move. */
#define MM_M1_MIN 15u
#define MM_M2_MIN 100u
#define MM_M4_MIN 10u

static asx_json_doc g_scn; /* the scenario under test, as read */
static asx_json_doc g_var; /* the variant built from it */
static asx_json_doc g_a;   /* the baseline projection */
static asx_json_doc g_b;   /* the variant's projection */
static char g_file[MM_FILE_CAP];
static char g_err[512];
static char g_why[320];
static char g_paths[MM_MAX_SCENARIOS][MM_PATH_CAP];
static uint32_t g_n_paths;
static int g_verbose;
/* While a violation is being minimized the relations run over candidate
 * reductions and report nothing. */
static int g_quiet;
/* The seeds a baseline is probed with: MM_SEEDS unless ASX_METAMORPHIC_SEEDS
 * (a --fuzz run, whose shapes are built not to need 32) says otherwise. */
static uint32_t g_seeds = MM_SEEDS;

static void mm_say(const char *fmt, ...) {
    va_list ap;
    if (g_quiet) return;
    va_start(ap, fmt);
    (void)vfprintf(stderr, fmt, ap);
    va_end(ap);
}

/* ---- reading the corpus ------------------------------------------------ */

static int cmp_paths(const void *a, const void *b) {
    return strcmp((const char *)a, (const char *)b);
}

static int add_path(const char *path) {
    if (g_n_paths >= MM_MAX_SCENARIOS || strlen(path) >= MM_PATH_CAP) return 0;
    memcpy(g_paths[g_n_paths], path, strlen(path) + 1u);
    g_n_paths++;
    return 1;
}

/* The scenarios named on the command line, else every .json under MM_DIR,
 * in name order. Returns 0 when there are none. */
static int collect_corpus(int argc, char **argv) {
    DIR *dir;
    const struct dirent *ent;
    if (argc > 1) {
        int i;
        for (i = 1; i < argc; i++) {
            if (!add_path(argv[i])) return 0;
        }
        return 1;
    }
    dir = opendir(MM_DIR);
    if (dir == NULL) return 0;
    for (ent = readdir(dir); ent != NULL; ent = readdir(dir)) {
        size_t len = strlen(ent->d_name);
        char path[MM_PATH_CAP];
        if (len < 6u || strcmp(&ent->d_name[len - 5u], ".json") != 0) continue;
        if (snprintf(path, sizeof(path), "%s/%s", MM_DIR, ent->d_name) >= (int)sizeof(path) ||
            !add_path(path)) {
            (void)closedir(dir);
            return 0;
        }
    }
    (void)closedir(dir);
    if (g_n_paths > 1u) qsort(g_paths, g_n_paths, MM_PATH_CAP, cmp_paths);
    return g_n_paths > 0u;
}

static int load_scenario(const char *path, asx_json_doc *doc, uint32_t *root) {
    FILE *f = fopen(path, "rb");
    size_t n;
    if (f == NULL) return 0;
    n = fread(g_file, 1, sizeof(g_file), f);
    (void)fclose(f);
    if (n == 0u || n == sizeof(g_file)) return 0;
    asx_json_doc_init(doc);
    return asx_json_parse(doc, g_file, n, root) == ASX_OK;
}

/* ---- name sets and footprints ------------------------------------------ */

typedef struct {
    const char *s[MM_MAX_NAMES];
    uint32_t n;
    int full;
} mm_set;

static int set_has(const mm_set *set, const char *x) {
    uint32_t i;
    for (i = 0; i < set->n; i++) {
        if (strcmp(set->s[i], x) == 0) return 1;
    }
    return 0;
}

static void set_add(mm_set *set, const char *x) {
    if (set_has(set, x)) return;
    if (set->n >= MM_MAX_NAMES) {
        set->full = 1;
        return;
    }
    set->s[set->n++] = x;
}

static int sets_meet(const mm_set *a, const mm_set *b) {
    uint32_t i;
    for (i = 0; i < a->n; i++) {
        if (set_has(b, a->s[i])) return 1;
    }
    return 0;
}

typedef struct {
    const mm_set *top;   /* names declared at the top level */
    const mm_set *allas; /* every "as" of the scenario (second pass) */
    const mm_set *own;   /* the "as" names this program introduces (second pass) */
    mm_set *mention;     /* top names the program mentions */
    mm_set *intro;       /* "as" names the program introduces (first pass) */
    mm_set *foreign;     /* scenario "as" names it mentions without introducing */
} mm_walk;

/* Every string value under `node` but a step's "op". A member named "as"
 * introduces a name; any other string is a mention. */
static void walk_names(const asx_json_doc *d, uint32_t node, const mm_walk *w) {
    asx_json_type ty = asx_json_type_of(d, node);
    uint32_t i;
    if (ty == ASX_JSON_STRING) {
        const char *s = asx_json_string(d, node);
        if (s == NULL) return;
        if (w->mention != NULL && set_has(w->top, s)) set_add(w->mention, s);
        if (w->foreign != NULL && set_has(w->allas, s) && !set_has(w->own, s)) {
            set_add(w->foreign, s);
        }
        return;
    }
    if (ty != ASX_JSON_ARRAY && ty != ASX_JSON_OBJECT) return;
    for (i = 0; i < asx_json_count(d, node); i++) {
        uint32_t m = asx_json_item(d, node, i);
        const char *key = ty == ASX_JSON_OBJECT ? asx_json_key(d, m) : NULL;
        if (key != NULL && strcmp(key, "as") == 0) {
            const char *name = asx_json_string(d, m);
            if (name != NULL && w->intro != NULL) set_add(w->intro, name);
            continue;
        }
        if (key != NULL && strcmp(key, "op") == 0 && asx_json_type_of(d, m) == ASX_JSON_STRING) {
            continue;
        }
        walk_names(d, m, w);
    }
}

static int has_op(const asx_json_doc *d, uint32_t node, const char *op) {
    asx_json_type ty = asx_json_type_of(d, node);
    uint32_t i;
    if (ty != ASX_JSON_ARRAY && ty != ASX_JSON_OBJECT) return 0;
    for (i = 0; i < asx_json_count(d, node); i++) {
        uint32_t m = asx_json_item(d, node, i);
        const char *key = ty == ASX_JSON_OBJECT ? asx_json_key(d, m) : NULL;
        if (key != NULL && strcmp(key, "op") == 0 && asx_json_type_of(d, m) == ASX_JSON_STRING) {
            if (strcmp(asx_json_string(d, m), op) == 0) return 1;
            continue;
        }
        if (has_op(d, m, op)) return 1;
    }
    return 0;
}

/* Whether any object under `node` has a member named `key`. */
static int has_key(const asx_json_doc *d, uint32_t node, const char *key) {
    asx_json_type ty = asx_json_type_of(d, node);
    uint32_t i;
    if (ty != ASX_JSON_ARRAY && ty != ASX_JSON_OBJECT) return 0;
    for (i = 0; i < asx_json_count(d, node); i++) {
        uint32_t m = asx_json_item(d, node, i);
        const char *k = ty == ASX_JSON_OBJECT ? asx_json_key(d, m) : NULL;
        if (k != NULL && strcmp(k, key) == 0) return 1;
        if (has_key(d, m, key)) return 1;
    }
    return 0;
}

/* Whether the scenario sets a budget with a deadline or a quota anywhere
 * (a task's, a region's, a spawn's, an open_region's). */
static int has_budget_limit(const asx_json_doc *d, uint32_t sc) {
    uint32_t tasks = asx_json_get(d, sc, "tasks");
    uint32_t regions = asx_json_get(d, sc, "regions");
    return has_key(d, tasks, "deadline_ns") || has_key(d, tasks, "poll_quota") ||
           has_key(d, tasks, "cost_quota") || has_key(d, regions, "deadline_ns") ||
           has_key(d, regions, "poll_quota") || has_key(d, regions, "cost_quota");
}

typedef struct {
    uint32_t n_tasks;
    uint32_t n_iso;
    uint32_t iso[MM_MAX_TASKS]; /* indices into "tasks", ascending */
    int uses_region_limits;
    const char *why; /* non-NULL: the scenario cannot be analysed */
} mm_info;

static mm_set g_top;
static mm_set g_allas;
static mm_set g_gment;
static mm_set g_ment[MM_MAX_TASKS];
static mm_set g_intro[MM_MAX_TASKS];
static mm_set g_foreign[MM_MAX_TASKS];

static void add_declared(const asx_json_doc *d, uint32_t list) {
    uint32_t i;
    for (i = 0; i < asx_json_count(d, list); i++) {
        const char *name = asx_json_get_string(d, asx_json_item(d, list, i), "name");
        if (name != NULL) set_add(&g_top, name);
    }
}

/* Which top-level tasks of scenario `sc` share nothing with any other. */
static void analyze(const asx_json_doc *d, uint32_t sc, mm_info *info) {
    uint32_t tasks = asx_json_get(d, sc, "tasks");
    uint32_t regions = asx_json_get(d, sc, "regions");
    uint32_t script = asx_json_get(d, sc, "script");
    uint32_t n = asx_json_count(d, tasks);
    uint32_t nr = asx_json_count(d, regions);
    int touched[MM_MAX_REGIONS];
    uint32_t i;
    uint32_t j;
    int changed;
    mm_walk w;

    memset(info, 0, sizeof(*info));
    if (n == 0u || n > MM_MAX_TASKS || nr > MM_MAX_REGIONS) {
        info->why = "no tasks, or more tasks or regions than the analysis holds";
        return;
    }
    info->n_tasks = n;
    memset(&g_top, 0, sizeof(g_top));
    memset(&g_allas, 0, sizeof(g_allas));
    memset(&g_gment, 0, sizeof(g_gment));
    add_declared(d, regions);
    add_declared(d, asx_json_get(d, sc, "channels"));
    add_declared(d, asx_json_get(d, sc, "sync"));
    add_declared(d, tasks);
    for (i = 0; i < nr; i++) {
        add_declared(d, asx_json_get(d, asx_json_item(d, regions, i), "finalizers"));
    }
    info->uses_region_limits = has_op(d, tasks, "region_limits");

    /* Pass 1: what each task mentions and introduces. */
    for (i = 0; i < n; i++) {
        uint32_t t = asx_json_item(d, tasks, i);
        const char *name = asx_json_get_string(d, t, "name");
        memset(&g_ment[i], 0, sizeof(g_ment[i]));
        memset(&g_intro[i], 0, sizeof(g_intro[i]));
        memset(&g_foreign[i], 0, sizeof(g_foreign[i]));
        memset(&w, 0, sizeof(w));
        w.top = &g_top;
        w.mention = &g_ment[i];
        w.intro = &g_intro[i];
        walk_names(d, asx_json_get(d, t, "program"), &w);
        if (name != NULL) set_add(&g_ment[i], name);
    }
    for (i = 0; i < n; i++) {
        for (j = 0; j < g_intro[i].n; j++) set_add(&g_allas, g_intro[i].s[j]);
    }
    /* Region finalizers and the driver script act on regions and tasks by
     * name: what they mention is touched. */
    for (i = 0; i < nr; i++) {
        uint32_t fin = asx_json_get(d, asx_json_item(d, regions, i), "finalizers");
        memset(&w, 0, sizeof(w));
        w.top = &g_top;
        w.mention = &g_gment;
        walk_names(d, fin, &w);
    }
    memset(&w, 0, sizeof(w));
    w.top = &g_top;
    w.mention = &g_gment;
    walk_names(d, script, &w);

    /* Pass 2: the introduced names each task mentions that are not its own. */
    for (i = 0; i < n; i++) {
        memset(&w, 0, sizeof(w));
        w.top = &g_top;
        w.allas = &g_allas;
        w.own = &g_intro[i];
        w.foreign = &g_foreign[i];
        walk_names(d, asx_json_get(d, asx_json_item(d, tasks, i), "program"), &w);
    }

    /* A region is touched when it has finalizers or anything names it, and
     * so is every region below it: its tasks then share the region. */
    for (i = 0; i < nr; i++) {
        uint32_t r = asx_json_item(d, regions, i);
        const char *name = asx_json_get_string(d, r, "name");
        touched[i] = asx_json_count(d, asx_json_get(d, r, "finalizers")) > 0u ||
                     (name != NULL && set_has(&g_gment, name));
        for (j = 0; j < n && !touched[i]; j++) {
            if (name != NULL && set_has(&g_ment[j], name)) touched[i] = 1;
        }
    }
    do {
        changed = 0;
        for (i = 0; i < nr; i++) {
            const char *parent = asx_json_get_string(d, asx_json_item(d, regions, i), "parent");
            for (j = 0; parent != NULL && j < nr && !touched[i]; j++) {
                const char *pn = asx_json_get_string(d, asx_json_item(d, regions, j), "name");
                if (pn != NULL && strcmp(pn, parent) == 0 && touched[j]) {
                    touched[i] = 1;
                    changed = 1;
                }
            }
        }
    } while (changed);
    for (i = 0; i < n; i++) {
        const char *region = asx_json_get_string(d, asx_json_item(d, tasks, i), "region");
        for (j = 0; region != NULL && j < nr; j++) {
            const char *rn = asx_json_get_string(d, asx_json_item(d, regions, j), "name");
            if (rn != NULL && strcmp(rn, region) == 0 && touched[j]) set_add(&g_ment[i], region);
        }
    }

    for (i = 0; i < n; i++) {
        if (g_ment[i].full || g_intro[i].full || g_foreign[i].full) {
            info->why = "a program mentions more names than the analysis holds";
            return;
        }
    }
    if (g_top.full || g_allas.full || g_gment.full) {
        info->why = "the scenario declares more names than the analysis holds";
        return;
    }
    for (i = 0; i < n; i++) {
        int isolated = 1;
        for (j = 0; j < n && isolated; j++) {
            if (j == i) continue;
            if (sets_meet(&g_ment[i], &g_ment[j]) || sets_meet(&g_foreign[i], &g_intro[j]) ||
                sets_meet(&g_foreign[j], &g_intro[i])) {
                isolated = 0;
            }
        }
        if (isolated) info->iso[info->n_iso++] = i;
    }
}

/* ---- variants ---------------------------------------------------------- */

typedef struct {
    const char *key;
    uint32_t node;
} mm_override;

/* A copy of object `obj` of `src` in `dst`; the members named in `over`
 * take the given nodes of `dst` instead (and are added when missing). */
static uint32_t copy_with(asx_json_doc *dst, const asx_json_doc *src, uint32_t obj,
                          const mm_override *over, uint32_t n_over) {
    uint32_t out = asx_json_new_object(dst);
    uint32_t i;
    uint32_t k;
    for (i = 0; i < asx_json_count(src, obj); i++) {
        uint32_t m = asx_json_item(src, obj, i);
        const char *key = asx_json_key(src, m);
        uint32_t val = ASX_JSON_NONE;
        if (key == NULL) continue;
        for (k = 0; k < n_over; k++) {
            if (strcmp(key, over[k].key) == 0) val = over[k].node;
        }
        if (val == ASX_JSON_NONE) val = asx_json_copy(dst, src, m);
        asx_json_set(dst, out, key, val);
    }
    for (k = 0; k < n_over; k++) {
        if (asx_json_get(src, obj, over[k].key) == ASX_JSON_NONE) {
            asx_json_set(dst, out, over[k].key, over[k].node);
        }
    }
    return out;
}

static int variant_ok(uint32_t root) { return root != ASX_JSON_NONE && asx_json_doc_ok(&g_var); }

static int build_seed_variant(const asx_json_doc *d, uint32_t sc, uint64_t seed,
                              uint32_t *out_root) {
    mm_override over[1];
    asx_json_doc_init(&g_var);
    over[0].key = "seed";
    over[0].node = asx_json_new_u64(&g_var, seed);
    *out_root = copy_with(&g_var, d, sc, over, 1u);
    return variant_ok(*out_root);
}

/* The scenario with "tasks" in the order perm[0..n-1] (indices into it). */
static int build_permuted(const asx_json_doc *d, uint32_t sc, const uint32_t *perm, uint32_t n,
                          uint32_t *out_root) {
    uint32_t src_tasks = asx_json_get(d, sc, "tasks");
    mm_override over[1];
    uint32_t arr;
    uint32_t i;
    asx_json_doc_init(&g_var);
    arr = asx_json_new_array(&g_var);
    for (i = 0; i < n; i++) {
        asx_json_push(&g_var, arr, asx_json_copy(&g_var, d, asx_json_item(d, src_tasks, perm[i])));
    }
    over[0].key = "tasks";
    over[0].node = arr;
    *out_root = copy_with(&g_var, d, sc, over, 1u);
    return variant_ok(*out_root);
}

/* The scenario plus a task with an empty program: first or last in
 * "tasks", in a region of its own (child of the root) or in the region of
 * the first task. */
static int build_with_noop(const asx_json_doc *d, uint32_t sc, int front, int fresh_region,
                           uint32_t *out_root) {
    uint32_t src_tasks = asx_json_get(d, sc, "tasks");
    uint32_t src_regions = asx_json_get(d, sc, "regions");
    uint32_t src_lab = asx_json_get(d, sc, "lab");
    mm_override over[3];
    uint32_t n_over = 0;
    uint32_t n = asx_json_count(d, src_tasks);
    const char *region = MM_NOOP_REGION;
    uint64_t steps = 0;
    uint32_t arr;
    uint32_t noop;
    uint32_t i;

    asx_json_doc_init(&g_var);
    if (!fresh_region) {
        region = asx_json_get_string(d, asx_json_item(d, src_tasks, 0), "region");
        if (region == NULL) return 0;
    }
    noop = asx_json_new_object(&g_var);
    asx_json_set(&g_var, noop, "name", asx_json_new_string(&g_var, MM_NOOP_TASK));
    asx_json_set(&g_var, noop, "region", asx_json_new_string(&g_var, region));
    asx_json_set(&g_var, noop, "budget", asx_json_new_null(&g_var));
    asx_json_set(&g_var, noop, "program", asx_json_new_array(&g_var));
    arr = asx_json_new_array(&g_var);
    if (front) asx_json_push(&g_var, arr, noop);
    for (i = 0; i < n; i++) {
        asx_json_push(&g_var, arr, asx_json_copy(&g_var, d, asx_json_item(d, src_tasks, i)));
    }
    if (!front) asx_json_push(&g_var, arr, noop);
    over[n_over].key = "tasks";
    over[n_over++].node = arr;
    if (fresh_region) {
        uint32_t regs = asx_json_new_array(&g_var);
        uint32_t r = asx_json_new_object(&g_var);
        for (i = 0; i < asx_json_count(d, src_regions); i++) {
            asx_json_push(&g_var, regs, asx_json_copy(&g_var, d, asx_json_item(d, src_regions, i)));
        }
        asx_json_set(&g_var, r, "name", asx_json_new_string(&g_var, MM_NOOP_REGION));
        asx_json_set(&g_var, r, "parent", asx_json_new_string(&g_var, "root"));
        asx_json_set(&g_var, r, "budget", asx_json_new_null(&g_var));
        asx_json_push(&g_var, regs, r);
        over[n_over].key = "regions";
        over[n_over++].node = regs;
    }
    if (src_lab != ASX_JSON_NONE &&
        asx_json_u64(d, asx_json_get(d, src_lab, "max_steps"), &steps)) {
        mm_override lab[1];
        lab[0].key = "max_steps";
        lab[0].node = asx_json_new_u64(&g_var, steps + MM_NOOP_STEPS);
        over[n_over].key = "lab";
        over[n_over++].node = copy_with(&g_var, d, src_lab, lab, 1u);
    }
    *out_root = copy_with(&g_var, d, sc, over, n_over);
    return variant_ok(*out_root);
}

/* ---- running and comparing --------------------------------------------- */

static asx_status run_scn(const asx_json_doc *doc, uint32_t root, asx_json_doc *out,
                          uint32_t *out_root) {
    asx_json_doc_init(out);
    g_err[0] = '\0';
    return asx_conformance_run(doc, root, out, out_root, g_err, sizeof(g_err));
}

static char g_canon[MM_CANON_CAP];
static char g_canon2[MM_CANON_CAP];

static const char *canon_into(const asx_json_doc *d, uint32_t node, char *buf) {
    asx_json_out out;
    asx_json_out_init(&out, buf, MM_CANON_CAP);
    if (asx_json_write_canonical(d, node, &out) != ASX_OK) return NULL;
    if (asx_json_out_finish(&out) != ASX_OK) return NULL;
    return buf;
}

/* 1 when the two nodes have the same canonical bytes. */
static int same_node(const asx_json_doc *da, uint32_t a, const asx_json_doc *db, uint32_t b) {
    const char *x;
    const char *y;
    if (a == ASX_JSON_NONE || b == ASX_JSON_NONE) return a == b;
    x = canon_into(da, a, g_canon);
    y = canon_into(db, b, g_canon2);
    return x != NULL && y != NULL && strcmp(x, y) == 0;
}

static char g_pool_a[MM_POOL_CAP];
static char g_pool_b[MM_POOL_CAP];
static const char *g_ev_a[MM_MAX_EVENTS];
static const char *g_ev_b[MM_MAX_EVENTS];

static int cmp_str(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

/* The canonical text of every event of the trace (layers flattened),
 * without those that contain `skip0`, `skip1` or `skip2`, sorted. 0 on
 * overflow. */
static int flatten_trace(const asx_json_doc *d, uint32_t trace, char *pool, const char **ev,
                         const char *skip0, const char *skip1, const char *skip2, uint32_t *out_n) {
    uint32_t used = 0;
    uint32_t n = 0;
    uint32_t l;
    uint32_t e;
    for (l = 0; l < asx_json_count(d, trace); l++) {
        uint32_t layer = asx_json_item(d, trace, l);
        for (e = 0; e < asx_json_count(d, layer); e++) {
            const char *txt = canon_into(d, asx_json_item(d, layer, e), g_canon);
            size_t len;
            if (txt == NULL) return 0;
            if (skip0 != NULL && strstr(txt, skip0) != NULL) continue;
            if (skip1 != NULL && strstr(txt, skip1) != NULL) continue;
            if (skip2 != NULL && strstr(txt, skip2) != NULL) continue;
            len = strlen(txt) + 1u;
            if (n >= MM_MAX_EVENTS || len > MM_POOL_CAP - used) return 0;
            memcpy(pool + used, txt, len);
            ev[n++] = pool + used;
            used += (uint32_t)len;
        }
    }
    if (n > 1u) qsort(ev, n, sizeof(ev[0]), cmp_str);
    *out_n = n;
    return 1;
}

/* The events of the two traces as sorted multisets: `skip0` leaves events
 * out of both, `skip1` and `skip2` out of the variant's only. */
static const char *trace_difference(const asx_json_doc *da, uint32_t ta, const asx_json_doc *db,
                                    uint32_t tb, const char *skip0, const char *skip1,
                                    const char *skip2) {
    uint32_t na = 0;
    uint32_t nb = 0;
    uint32_t i;
    if (!flatten_trace(da, ta, g_pool_a, g_ev_a, skip0, NULL, NULL, &na) ||
        !flatten_trace(db, tb, g_pool_b, g_ev_b, skip0, skip1, skip2, &nb)) {
        return "trace: too large for the comparison pools";
    }
    if (na != nb) {
        (void)snprintf(g_why, sizeof(g_why), "trace: %u events, the variant has %u", (unsigned)na,
                       (unsigned)nb);
        return g_why;
    }
    for (i = 0; i < na; i++) {
        if (strcmp(g_ev_a[i], g_ev_b[i]) != 0) {
            (void)snprintf(g_why, sizeof(g_why), "trace: event %.120s, the variant has %.120s",
                           g_ev_a[i], g_ev_b[i]);
            return g_why;
        }
    }
    return NULL;
}

/* What a comparison allows the variant to differ in. All NULL: nothing (M1). */
typedef struct {
    const char *noop_task;   /* M2: the variant holds this task more, and it completed */
    const char *noop_region; /* M2: and this region more */
    const char *skip_event;  /* M4: events whose canonical text holds this, in both traces */
} mm_cmp;

/* How the variant's projection `rb` differs from the baseline's `ra`, or
 * NULL when it does not. With nothing allowed the two must be identical
 * (M1). With a no-op named, the variant may hold exactly that task (and
 * region, when named) and the events naming them, and nothing else may
 * differ; the no-op itself must have completed (M2). */
static const char *mm_compare(const asx_json_doc *da, uint32_t ra, const asx_json_doc *db,
                              uint32_t rb, const mm_cmp *cmp) {
    const char *noop_task = cmp->noop_task;
    const char *noop_region = cmp->noop_region;
    uint32_t sa = asx_json_get(da, ra, "snapshot");
    uint32_t sb = asx_json_get(db, rb, "snapshot");
    uint32_t oa = asx_json_get(da, ra, "observations");
    uint32_t ob = asx_json_get(db, rb, "observations");
    uint32_t ta = asx_json_get(da, ra, "trace");
    uint32_t tb = asx_json_get(db, rb, "trace");
    char skip_task[96];
    char skip_region[96];
    uint32_t i;
    uint32_t j;

    if (sa == ASX_JSON_NONE || sb == ASX_JSON_NONE || oa == ASX_JSON_NONE || ob == ASX_JSON_NONE ||
        ta == ASX_JSON_NONE || tb == ASX_JSON_NONE) {
        return "a projection lacks its trace, snapshot or observations";
    }
    if (!same_node(da, oa, db, ob)) return "observations";
    if (asx_json_count(da, sa) != asx_json_count(db, sb)) return "snapshot: different members";
    for (i = 0; i < asx_json_count(da, sa); i++) {
        uint32_t ma = asx_json_item(da, sa, i);
        const char *key = asx_json_key(da, ma);
        uint32_t mb = key != NULL ? asx_json_get(db, sb, key) : ASX_JSON_NONE;
        int grows;
        if (mb == ASX_JSON_NONE) return "snapshot: a member is missing";
        grows = (noop_task != NULL && strcmp(key, "tasks") == 0) ||
                (noop_region != NULL && strcmp(key, "regions") == 0);
        if (!grows) {
            if (!same_node(da, ma, db, mb)) {
                (void)snprintf(g_why, sizeof(g_why), "snapshot.%s", key);
                return g_why;
            }
            continue;
        }
        if (asx_json_count(db, mb) != asx_json_count(da, ma) + 1u) {
            (void)snprintf(g_why, sizeof(g_why), "snapshot.%s: not exactly one entity more", key);
            return g_why;
        }
        for (j = 0; j < asx_json_count(da, ma); j++) {
            uint32_t ea = asx_json_item(da, ma, j);
            const char *name = asx_json_key(da, ea);
            uint32_t eb = name != NULL ? asx_json_get(db, mb, name) : ASX_JSON_NONE;
            if (eb == ASX_JSON_NONE || !same_node(da, ea, db, eb)) {
                (void)snprintf(g_why, sizeof(g_why), "snapshot.%s.%.100s", key,
                               name != NULL ? name : "?");
                return g_why;
            }
        }
    }
    if (noop_task != NULL) {
        uint32_t nt = asx_json_get(db, asx_json_get(db, sb, "tasks"), noop_task);
        const char *state = nt != ASX_JSON_NONE ? asx_json_get_string(db, nt, "state") : NULL;
        /* Its outcome may be a cancel the scenario itself requested for
         * its region before the no-op first ran; completing is the claim. */
        if (state == NULL || strcmp(state, "Completed") != 0) {
            return "the no-op task did not complete";
        }
    }
    skip_task[0] = '\0';
    skip_region[0] = '\0';
    if (noop_task != NULL) (void)snprintf(skip_task, sizeof(skip_task), "\"%s\"", noop_task);
    if (noop_region != NULL) {
        (void)snprintf(skip_region, sizeof(skip_region), "\"%s\"", noop_region);
    }
    return trace_difference(da, ta, db, tb, cmp->skip_event, noop_task != NULL ? skip_task : NULL,
                            noop_region != NULL ? skip_region : NULL);
}

static const char *mm_difference(const asx_json_doc *da, uint32_t ra, const asx_json_doc *db,
                                 uint32_t rb, const char *noop_task, const char *noop_region) {
    mm_cmp cmp;
    memset(&cmp, 0, sizeof(cmp));
    cmp.noop_task = noop_task;
    cmp.noop_region = noop_region;
    return mm_compare(da, ra, db, rb, &cmp);
}

/* ---- one scenario ------------------------------------------------------ */

typedef struct {
    uint32_t scenarios;
    uint32_t load_failures;
    uint32_t skipped_forbidden;
    uint32_t skipped_baseline;
    uint32_t skipped_sensitive;
    uint32_t skipped_analysis;
    uint32_t m1_applied;
    uint32_t m1_variants;
    uint32_t m1_violations;
    uint32_t m2_applied;
    uint32_t m2_variants;
    uint32_t m2_capacity; /* variants skipped: a capacity limit, not compared */
    uint32_t m2_violations;
    uint32_t m3_pairs;
    uint32_t m3_violations;
    uint32_t m4_applied;
    uint32_t m4_variants;
    uint32_t m4_violations;
} mm_report;

static uint64_t alt_seed(uint64_t seed, uint32_t k) {
    uint64_t s = seed ^ (0x9E3779B97F4A7C15ULL * (uint64_t)(k + 1u));
    return s != 0u ? s : 1u;
}

static void note_skip(const char *label, const char *what, const char *detail) {
    if (g_verbose) {
        fprintf(stderr, "  skip %s: %s%s%s\n", label, what, detail != NULL ? ": " : "",
                detail != NULL ? detail : "");
    }
}

static char g_sens[160];

/* Whether the baseline in g_a keeps its semantic digest under other seeds.
 * Otherwise g_sens says which seed and which part of the projection first
 * differed: the scenario's outcome is decided by a seeded tie-break (two
 * equal-priority picks, two timers due together), so adding or reordering
 * a task legitimately changes it. */
static int seed_insensitive(const asx_json_doc *d, uint32_t sc, uint64_t seed, uint32_t ra,
                            const char *base_digest) {
    uint32_t k;
    g_sens[0] = '\0';
    for (k = 0; k < g_seeds; k++) {
        uint32_t vr = 0;
        uint32_t rb = 0;
        const char *digest;
        const char *diff;
        if (!build_seed_variant(d, sc, alt_seed(seed, k), &vr)) {
            (void)snprintf(g_sens, sizeof(g_sens), "seed %u: the variant could not be built",
                           (unsigned)k);
            return 0;
        }
        if (run_scn(&g_var, vr, &g_b, &rb) != ASX_OK) {
            (void)snprintf(g_sens, sizeof(g_sens), "seed %u: the run fails: %.80s", (unsigned)k,
                           g_err);
            return 0;
        }
        digest = asx_json_get_string(&g_b, rb, "semantic_digest");
        if (digest != NULL && strcmp(digest, base_digest) == 0) continue;
        diff = mm_difference(&g_a, ra, &g_b, rb, NULL, NULL);
        (void)snprintf(g_sens, sizeof(g_sens), "seed %u: %.120s", (unsigned)k,
                       diff != NULL ? diff : "semantic digest");
        return 0;
    }
    return 1;
}

/* With ASX_METAMORPHIC_VERBOSE, the dispatch order, observations and
 * snapshot of the baseline in g_a and the variant in g_b, for a
 * violation. */
static void dump_parts(uint32_t ra, uint32_t rb) {
    static const char *const parts[] = {"dispatches", "observations", "snapshot"};
    size_t i;
    if (!g_verbose) return;
    for (i = 0; i < sizeof(parts) / sizeof(parts[0]); i++) {
        const char *x = canon_into(&g_a, asx_json_get(&g_a, ra, parts[i]), g_canon);
        const char *y = canon_into(&g_b, asx_json_get(&g_b, rb, parts[i]), g_canon2);
        fprintf(stderr, "    %s\n      base:    %.1600s\n      variant: %.1600s\n", parts[i],
                x != NULL ? x : "?", y != NULL ? y : "?");
    }
}

static void m1_variant(const char *label, const asx_json_doc *d, uint32_t sc, uint32_t ra,
                       const uint32_t *perm, uint32_t n, const char *how, mm_report *rep) {
    uint32_t vr = 0;
    uint32_t rb = 0;
    const char *diff;
    rep->m1_variants++;
    if (!build_permuted(d, sc, perm, n, &vr)) {
        rep->m1_violations++;
        mm_say("  M1 VIOLATION %s (%s): the variant could not be built\n", label, how);
        return;
    }
    if (run_scn(&g_var, vr, &g_b, &rb) != ASX_OK) {
        rep->m1_violations++;
        mm_say("  M1 VIOLATION %s (%s): the permuted scenario does not run: %s\n", label, how,
               g_err);
        return;
    }
    diff = mm_difference(&g_a, ra, &g_b, rb, NULL, NULL);
    if (diff != NULL) {
        rep->m1_violations++;
        mm_say("  M1 VIOLATION %s (%s): %s\n", label, how, diff);
        dump_parts(ra, rb);
    }
}

/* 1 when the variant's observations hold a capacity refusal that the
 * baseline's do not. */
static int capacity_effect(const asx_json_doc *da, uint32_t ra, const asx_json_doc *db,
                           uint32_t rb) {
    static const char *const names[] = {"ASX_E_RESOURCE_EXHAUSTED"};
    const char *a = canon_into(da, asx_json_get(da, ra, "observations"), g_canon);
    const char *b = canon_into(db, asx_json_get(db, rb, "observations"), g_canon2);
    size_t i;
    if (a == NULL || b == NULL) return 1;
    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (strstr(b, names[i]) != NULL && strstr(a, names[i]) == NULL) return 1;
    }
    return 0;
}

static void m2_variant(const char *label, const asx_json_doc *d, uint32_t sc, uint32_t ra,
                       int front, int fresh_region, mm_report *rep) {
    uint32_t vr = 0;
    uint32_t rb = 0;
    const char *diff;
    const char *how = fresh_region
                          ? (front ? "first, own region" : "last, own region")
                          : (front ? "first, first task's region" : "last, first task's region");
    /* The no-op uses a task, and a region of its own, from arenas of fixed
     * size. A scenario at the edge of a capacity (a resource-plane limit,
     * not semantics) may then not run, or have a spawn or region refused
     * that the baseline got: those variants are skipped and counted, not
     * compared. */
    if (!build_with_noop(d, sc, front, fresh_region, &vr)) {
        rep->m2_capacity++;
        note_skip(label, "the variant does not fit the document pools", how);
        return;
    }
    if (run_scn(&g_var, vr, &g_b, &rb) != ASX_OK) {
        rep->m2_capacity++;
        note_skip(label, "the variant with a no-op does not run", g_err);
        return;
    }
    if (capacity_effect(&g_a, ra, &g_b, rb)) {
        rep->m2_capacity++;
        note_skip(label, "the no-op reached a capacity limit", how);
        return;
    }
    rep->m2_variants++;
    diff = mm_difference(&g_a, ra, &g_b, rb, MM_NOOP_TASK, fresh_region ? MM_NOOP_REGION : NULL);
    if (diff != NULL) {
        rep->m2_violations++;
        mm_say("  M2 VIOLATION %s (%s): %s\n", label, how, diff);
        dump_parts(ra, rb);
    }
}

/* M4's transform. The scenario's "script" with, after each driver
 * cancel_task / cancel_region, a second request for the same target at the
 * same instant: of kind User after one of a stronger kind (`weaker`), or of
 * kind Shutdown after one of kind User (the converse, which must change
 * something). *n_added is how many were inserted. */
static int build_cancel_duplicates(const asx_json_doc *d, uint32_t sc, int weaker,
                                   uint32_t *out_root, uint32_t *n_added) {
    uint32_t script = asx_json_get(d, sc, "script");
    mm_override over[1];
    uint32_t arr;
    uint32_t i;
    *n_added = 0;
    asx_json_doc_init(&g_var);
    arr = asx_json_new_array(&g_var);
    for (i = 0; i < asx_json_count(d, script); i++) {
        uint32_t entry = asx_json_item(d, script, i);
        uint32_t op = asx_json_get(d, entry, "op");
        const char *name = asx_json_get_string(d, op, "op");
        const char *kind = asx_json_get_string(d, op, "kind");
        asx_json_push(&g_var, arr, asx_json_copy(&g_var, d, entry));
        if (name == NULL || kind == NULL) continue;
        if (strcmp(name, "cancel_task") != 0 && strcmp(name, "cancel_region") != 0) continue;
        if (weaker ? strcmp(kind, "User") == 0 : strcmp(kind, "User") != 0) continue;
        {
            uint32_t dup = asx_json_copy(&g_var, d, entry);
            uint32_t dup_op = asx_json_get(&g_var, dup, "op");
            asx_json_set(&g_var, dup_op, "kind",
                         asx_json_new_string(&g_var, weaker ? "User" : "Shutdown"));
            asx_json_push(&g_var, arr, dup);
            (*n_added)++;
        }
    }
    over[0].key = "script";
    over[0].node = arr;
    *out_root = copy_with(&g_var, d, sc, over, 1u);
    return variant_ok(*out_root);
}

/* The events a repeated region cancel emits again (vocabulary section 3:
 * region.cancelled on every call, for the whole subtree). */
#define MM_REGION_CANCELLED "\"k\":\"region.cancelled\""

/* M4: a weaker cancel request after a stronger one changes nothing: the
 * reasons strengthen monotonically and the cleanup budgets meet. Only the
 * region.cancelled events of the repeated request are new. */
static void m4_check(const char *label, const asx_json_doc *d, uint32_t sc, uint32_t ra,
                     mm_report *rep) {
    uint32_t vr = 0;
    uint32_t rb = 0;
    uint32_t added = 0;
    mm_cmp cmp;
    const char *diff;
    /* A budget cancel the task raises itself (a spent poll quota, a passed
     * deadline) lives in its Cx ahead of its record, and a second request
     * folds the Cx's reason into the request (record/task.rs:716-722, mirrored
     * by asx_task_cancel_reason_internal): the record then takes it, however
     * weak the request. Such a scenario is out of M4's scope; it is also one
     * where the repeated request's extra poll spends quota. */
    if (has_budget_limit(d, sc)) {
        note_skip(label, "M4 does not apply: the scenario sets a deadline or quota", NULL);
        return;
    }
    if (!build_cancel_duplicates(d, sc, 1, &vr, &added)) {
        rep->m4_violations++;
        mm_say("  M4 VIOLATION %s: the variant could not be built\n", label);
        return;
    }
    if (added == 0u) return;
    rep->m4_applied++;
    if (run_scn(&g_var, vr, &g_b, &rb) != ASX_OK) {
        rep->m4_violations++;
        mm_say("  M4 VIOLATION %s: the scenario with a weaker cancel does not run: %s\n", label,
               g_err);
        return;
    }
    rep->m4_variants++;
    memset(&cmp, 0, sizeof(cmp));
    cmp.skip_event = MM_REGION_CANCELLED;
    diff = mm_compare(&g_a, ra, &g_b, rb, &cmp);
    if (diff != NULL) {
        rep->m4_violations++;
        mm_say("  M4 VIOLATION %s (%u weaker cancel(s) added): %s\n", label, (unsigned)added, diff);
        dump_parts(ra, rb);
    }
}

/* Classify scenario `sc` of `d` and, where it qualifies, check M1, M2 and
 * M4. */
static void check_scenario(const char *label, const asx_json_doc *d, uint32_t sc, mm_report *rep) {
    uint64_t seed = 0;
    uint32_t ra = 0;
    const char *digest;
    const char *name = NULL;
    mm_info info;
    asx_status st;
    int forbidden;

    rep->scenarios++;
    forbidden = asx_conformance_requirement_forbidden(d, sc, asx_profile_active(), &name);
    if (forbidden != 0) {
        rep->skipped_forbidden++;
        note_skip(label, forbidden > 0 ? "the profile forbids it" : "unknown requirement", name);
        return;
    }
    st = run_scn(d, sc, &g_a, &ra);
    if (st != ASX_OK) {
        rep->skipped_baseline++;
        note_skip(label, "the C runtime does not run it", g_err);
        return;
    }
    digest = asx_json_get_string(&g_a, ra, "semantic_digest");
    (void)asx_json_u64(d, asx_json_get(d, sc, "seed"), &seed);
    if (digest == NULL) {
        rep->skipped_baseline++;
        note_skip(label, "no semantic digest", NULL);
        return;
    }
    /* seed_insensitive reuses g_b; g_a (the baseline) stays. */
    if (!seed_insensitive(d, sc, seed, ra, digest)) {
        rep->skipped_sensitive++;
        note_skip(label, "schedule sensitive, a seeded tie-break decides it", g_sens);
        return;
    }
    analyze(d, sc, &info);
    if (info.why != NULL) {
        rep->skipped_analysis++;
        note_skip(label, info.why, NULL);
        return;
    }

    if (info.n_iso >= 2u) {
        uint32_t perm[MM_MAX_TASKS];
        uint32_t k;
        rep->m1_applied++;
        for (k = 0; k < info.n_tasks; k++) perm[k] = k;
        for (k = 0; k < info.n_iso; k++) perm[info.iso[k]] = info.iso[info.n_iso - 1u - k];
        m1_variant(label, d, sc, ra, perm, info.n_tasks, "isolated tasks reversed", rep);
        if (info.n_iso > 2u) {
            for (k = 0; k < info.n_tasks; k++) perm[k] = k;
            for (k = 0; k < info.n_iso; k++) perm[info.iso[k]] = info.iso[(k + 1u) % info.n_iso];
            m1_variant(label, d, sc, ra, perm, info.n_tasks, "isolated tasks rotated", rep);
        }
    }

    m4_check(label, d, sc, ra, rep);

    /* region_limits caps a region's tasks or children, which a no-op uses
     * up: the limit would legitimately refuse something else. */
    if (info.uses_region_limits) {
        note_skip(label, "M2 does not apply: it sets region limits", NULL);
        return;
    }
    rep->m2_applied++;
    m2_variant(label, d, sc, ra, 1, 1, rep);
    m2_variant(label, d, sc, ra, 0, 0, rep);
}

static void print_report(const char *what, const mm_report *r) {
    fprintf(stderr,
            "  %s: %u scenarios; skipped %u (profile) %u (not run) %u (schedule sensitive) %u "
            "(analysis); M1 %u scenarios %u variants %u violations; M2 %u scenarios %u variants "
            "(%u skipped at a capacity limit) %u violations\n",
            what, (unsigned)r->scenarios, (unsigned)r->skipped_forbidden,
            (unsigned)r->skipped_baseline, (unsigned)r->skipped_sensitive,
            (unsigned)r->skipped_analysis, (unsigned)r->m1_applied, (unsigned)r->m1_variants,
            (unsigned)r->m1_violations, (unsigned)r->m2_applied, (unsigned)r->m2_variants,
            (unsigned)r->m2_capacity, (unsigned)r->m2_violations);
    fprintf(stderr,
            "  %s: M3 %u budget pairs %u violations; M4 %u scenarios %u variants %u violations\n",
            what, (unsigned)r->m3_pairs, (unsigned)r->m3_violations, (unsigned)r->m4_applied,
            (unsigned)r->m4_variants, (unsigned)r->m4_violations);
}

/* ---- M3: budgets meet through nested regions -------------------------- */

/* A budget as the DSL writes it (docs/SCENARIO_DSL_V2.md, Budgets): the
 * fields present, a priority that meets by maximum, or null (INFINITE). */
typedef struct {
    int infinite;
    int has_deadline;
    uint64_t deadline;
    int has_poll;
    uint64_t poll;
    int has_cost;
    uint64_t cost;
    unsigned priority;
} mm_budget;

/* The edge budgets of the unit-level M3 (test_budget_obligation.c,
 * nested_region_budgets_equal_their_meet) but ZERO: INFINITE, NEW, quotas,
 * deadlines, priorities. A zero poll quota would stop the helper tasks
 * that open the regions from running at all. */
static const mm_budget g_budgets[] = {
    {1, 0, 0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0, 0, 128},   {0, 0, 0, 1, 7, 0, 0, 128},
    {0, 1, 0, 0, 0, 0, 0, 3}, {0, 1, 500, 0, 0, 1, 9, 250}, {0, 1, 1000, 1, 3, 0, 0, 128},
};
#define MM_BUDGETS (sizeof(g_budgets) / sizeof(g_budgets[0]))

/* The budgets of the regions above the innermost one: deadlines and
 * priorities only. A task spawned into a region has its quotas clamped to
 * its spawner's remaining quotas (cx.rs:4547-4586), and the spawner of the
 * innermost region's task sits in the outer ones, one poll further in the
 * nested form than in the single one: a quota out there would differ for
 * that reason alone. */
static const mm_budget g_outer_budgets[] = {
    {1, 0, 0, 0, 0, 0, 0, 0},     {0, 0, 0, 0, 0, 0, 0, 128},    {0, 1, 0, 0, 0, 0, 0, 3},
    {0, 1, 500, 0, 0, 0, 0, 250}, {0, 1, 1000, 0, 0, 0, 0, 128},
};
#define MM_OUTER_BUDGETS (sizeof(g_outer_budgets) / sizeof(g_outer_budgets[0]))

static uint64_t u64_min(uint64_t a, uint64_t b) { return a < b ? a : b; }

/* meet: the earlier deadline, the smaller quotas, the higher priority; a
 * field only one side constrains stays; two nulls stay null. */
static mm_budget budget_meet(const mm_budget *x, const mm_budget *y) {
    mm_budget m;
    memset(&m, 0, sizeof(m));
    if (x->infinite && y->infinite) {
        m.infinite = 1;
        return m;
    }
    m.has_deadline = x->has_deadline || y->has_deadline;
    m.deadline = (x->has_deadline && y->has_deadline) ? u64_min(x->deadline, y->deadline)
                 : x->has_deadline                    ? x->deadline
                                                      : y->deadline;
    m.has_poll = x->has_poll || y->has_poll;
    m.poll = (x->has_poll && y->has_poll) ? u64_min(x->poll, y->poll)
             : x->has_poll                ? x->poll
                                          : y->poll;
    m.has_cost = x->has_cost || y->has_cost;
    m.cost = (x->has_cost && y->has_cost) ? u64_min(x->cost, y->cost)
             : x->has_cost                ? x->cost
                                          : y->cost;
    m.priority = x->priority > y->priority ? x->priority : y->priority;
    return m;
}

static void budget_json(const mm_budget *b, char *out, size_t cap) {
    size_t n;
    if (b->infinite) {
        (void)snprintf(out, cap, "null");
        return;
    }
    n = (size_t)snprintf(out, cap, "{\"priority\":%u", b->priority);
    if (b->has_deadline && n < cap) {
        n += (size_t)snprintf(out + n, cap - n, ",\"deadline_ns\":%llu",
                              (unsigned long long)b->deadline);
    }
    if (b->has_poll && n < cap) {
        n +=
            (size_t)snprintf(out + n, cap - n, ",\"poll_quota\":%llu", (unsigned long long)b->poll);
    }
    if (b->has_cost && n < cap) {
        n +=
            (size_t)snprintf(out + n, cap - n, ",\"cost_quota\":%llu", (unsigned long long)b->cost);
    }
    if (n < cap) (void)snprintf(out + n, cap - n, "}");
}

/* t.g polls a checkpoint between yields, then sleeps past 500 and
 * checkpoints again: a poll quota or a deadline of its region's budget ends
 * it early (cancelled, Deadline, PollQuota), and its priority (the
 * region's) against t.peer's puts it before or after t.peer in the
 * dispatch order. */
#define MM_M3_PROGRAM                                                                              \
    "[{\"op\":\"checkpoint\",\"on_cancel\":\"return\"},{\"op\":\"yield\"},"                        \
    "{\"op\":\"checkpoint\",\"on_cancel\":\"return\"},{\"op\":\"yield\"},"                         \
    "{\"op\":\"checkpoint\",\"on_cancel\":\"return\"},{\"op\":\"yield\"},"                         \
    "{\"op\":\"checkpoint\",\"on_cancel\":\"return\"},{\"op\":\"yield\"},"                         \
    "{\"op\":\"checkpoint\",\"on_cancel\":\"return\"},{\"op\":\"sleep\",\"ns\":600},"              \
    "{\"op\":\"checkpoint\",\"on_cancel\":\"return\"},{\"op\":\"trace\",\"message\":\"x end\"}]"

/* The scenario text. Budgets bind a task only through the region it is
 * spawned into by another task (`child.cx().spawn`, DSL 3.2): t.0 opens
 * r.1 with budget `top` and spawns t.c into it, and t.c reaches t.g's
 * region r.3 either in two steps, r.2 with `a` (spawning t.d) and r.3 with
 * `b` inside it (`nested`), or in one, r.3 with `met`. t.g runs the
 * program above in r.3; t.peer is a top-level task of priority 100. */
static int m3_text(char *buf, size_t cap, const char *top, const char *a, const char *b,
                   const char *met, int nested) {
    char chain[1400];
    if (nested) {
        (void)snprintf(
            chain, sizeof(chain),
            "{\"op\":\"open_region\",\"as\":\"r.2\",\"budget\":%s},"
            "{\"op\":\"spawn\",\"as\":\"t.d\",\"region\":\"r.2\",\"program\":["
            "{\"op\":\"open_region\",\"as\":\"r.3\",\"budget\":%s},"
            "{\"op\":\"spawn\",\"as\":\"t.g\",\"region\":\"r.3\",\"program\":" MM_M3_PROGRAM "},"
            "{\"op\":\"join\",\"task\":\"t.g\"}]},{\"op\":\"join\",\"task\":\"t.d\"}",
            a, b);
    } else {
        (void)snprintf(
            chain, sizeof(chain),
            "{\"op\":\"open_region\",\"as\":\"r.3\",\"budget\":%s},"
            "{\"op\":\"spawn\",\"as\":\"t.g\",\"region\":\"r.3\",\"program\":" MM_M3_PROGRAM "},"
            "{\"op\":\"join\",\"task\":\"t.g\"}",
            met);
    }
    return snprintf(buf, cap,
                    "{\"schema\":\"asx.scenario.v2\",\"id\":\"mm-m3-001\",\"description\":\"d\","
                    "\"unit\":\"budget\",\"rules\":[\"inv.determinism.replayable\"],\"seed\":42,"
                    "\"lab\":{\"max_steps\":4000},"
                    "\"regions\":[{\"name\":\"r.main\",\"parent\":\"root\",\"budget\":null}],"
                    "\"channels\":[],\"sync\":[],"
                    "\"tasks\":[{\"name\":\"t.0\",\"region\":\"r.main\",\"budget\":null,"
                    "\"program\":[{\"op\":\"open_region\",\"as\":\"r.1\",\"budget\":%s},"
                    "{\"op\":\"spawn\",\"as\":\"t.c\",\"region\":\"r.1\",\"program\":[%s]},"
                    "{\"op\":\"join\",\"task\":\"t.c\"}]},"
                    "{\"name\":\"t.peer\",\"region\":\"r.main\",\"budget\":{\"priority\":100},"
                    "\"program\":[{\"op\":\"yield\"},{\"op\":\"yield\"},{\"op\":\"yield\"}]}],"
                    "\"script\":[],\"expect\":{\"kind\":\"ok\"}}",
                    top, chain);
}

/* Run one scenario text through g_scn / g_a (as the nested form) or g_var /
 * g_b (as the single one). 0 with g_why set when it does not run. */
static int m3_run(const char *text, asx_json_doc *doc, asx_json_doc *out, uint32_t *rout) {
    uint32_t sc = 0;
    asx_json_doc_init(doc);
    if (asx_json_parse(doc, text, strlen(text), &sc) != ASX_OK) {
        (void)snprintf(g_why, sizeof(g_why), "the scenario text does not parse");
        return 0;
    }
    if (run_scn(doc, sc, out, rout) != ASX_OK) {
        (void)snprintf(g_why, sizeof(g_why), "does not run: %.200s", g_err);
        return 0;
    }
    return 1;
}

/* The observations of task `name`, canonical, one per line (their step
 * numbers are the task's own, so they are the same in both forms). 0 when
 * `cap` is too small. */
static int obs_text(const asx_json_doc *d, uint32_t obs, const char *name, char *out, size_t cap) {
    size_t used = 0;
    uint32_t i;
    out[0] = '\0';
    for (i = 0; i < asx_json_count(d, obs); i++) {
        uint32_t o = asx_json_item(d, obs, i);
        const char *task = asx_json_get_string(d, o, "task");
        const char *txt;
        size_t len;
        if (task == NULL || strcmp(task, name) != 0) continue;
        txt = canon_into(d, o, g_canon);
        if (txt == NULL) return 0;
        len = strlen(txt);
        if (len + 2u > cap - used) return 0;
        memcpy(out + used, txt, len);
        used += len;
        out[used++] = '\n';
        out[used] = '\0';
    }
    return 1;
}

/* The dispatches of t.g and t.peer as "<ns> <task> <lane>", in order: their
 * step numbers count the helper tasks too, which the two forms differ in. */
static int dispatch_text(const asx_json_doc *d, uint32_t disp, char *out, size_t cap) {
    size_t used = 0;
    uint32_t i;
    out[0] = '\0';
    for (i = 0; i < asx_json_count(d, disp); i++) {
        const char *s = asx_json_string(d, asx_json_item(d, disp, i));
        const char *at;
        size_t len;
        if (s == NULL || (strstr(s, " t.g ") == NULL && strstr(s, " t.peer ") == NULL)) continue;
        at = strchr(s, '@');
        if (at == NULL) return 0;
        len = strlen(at + 1);
        if (len + 2u > cap - used) return 0;
        memcpy(out + used, at + 1, len);
        used += len;
        out[used++] = '\n';
        out[used] = '\0';
    }
    return 1;
}

/* What M3 compares: t.g's and t.peer's snapshot entries, t.g's
 * observations and t.g's place among t.peer's dispatches. The helper tasks
 * and regions the nesting adds are not compared. */
static const char *m3_compare(const asx_json_doc *da, uint32_t ra, const asx_json_doc *db,
                              uint32_t rb) {
    static char xa[4096];
    static char xb[4096];
    uint32_t ta = asx_json_get(da, asx_json_get(da, ra, "snapshot"), "tasks");
    uint32_t tb = asx_json_get(db, asx_json_get(db, rb, "snapshot"), "tasks");
    static const char *const names[] = {"t.g", "t.peer"};
    size_t i;
    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (!same_node(da, asx_json_get(da, ta, names[i]), db, asx_json_get(db, tb, names[i]))) {
            (void)snprintf(g_why, sizeof(g_why), "snapshot.tasks.%s", names[i]);
            return g_why;
        }
    }
    if (!obs_text(da, asx_json_get(da, ra, "observations"), "t.g", xa, sizeof(xa)) ||
        !obs_text(db, asx_json_get(db, rb, "observations"), "t.g", xb, sizeof(xb))) {
        return "observations of t.g: too long to compare";
    }
    if (strcmp(xa, xb) != 0) return "observations of t.g";
    if (!dispatch_text(da, asx_json_get(da, ra, "dispatches"), xa, sizeof(xa)) ||
        !dispatch_text(db, asx_json_get(db, rb, "dispatches"), xb, sizeof(xb))) {
        return "dispatches: too long to compare";
    }
    if (strcmp(xa, xb) != 0) return "the dispatch order of t.g and t.peer";
    return NULL;
}

/* M3: a region with budget b opened inside one with budget a is the budget
 * of one region opened with meet(a, b) (Rust's region table meets a child's
 * budget with its parent's, runtime/region_table.rs:314): the task spawned
 * into it has the same effective budget, so the same outcome, observations
 * and place in the dispatch order. Both forms sit under a common ancestor
 * with budget `top`: a meet that is not associative, or that lets the
 * child replace the parent, then comes out different, which a parent with
 * no budget would absorb. */
/* One M3 case: the nested form of budgets (top, a, b) against the single
 * region with meet(a, b). 1 when the relation is violated. */
static int m3_case(const mm_budget *top, const mm_budget *a, const mm_budget *b, mm_report *rep) {
    mm_budget met = budget_meet(a, b);
    char jt[128];
    char ja[128];
    char jb[128];
    char jm[128];
    char text[4096];
    char what[600];
    uint32_t ra = 0;
    uint32_t rb = 0;
    const char *diff;
    budget_json(top, jt, sizeof(jt));
    budget_json(a, ja, sizeof(ja));
    budget_json(b, jb, sizeof(jb));
    budget_json(&met, jm, sizeof(jm));
    (void)snprintf(what, sizeof(what), "top %s, nested %s then %s, single %s", jt, ja, jb, jm);
    rep->m3_pairs++;

    (void)m3_text(text, sizeof(text), jt, ja, jb, jm, 1);
    if (!m3_run(text, &g_scn, &g_a, &ra)) {
        rep->m3_violations++;
        mm_say("  M3 VIOLATION (%s): nested %s\n", what, g_why);
        return 1;
    }
    (void)m3_text(text, sizeof(text), jt, ja, jb, jm, 0);
    if (!m3_run(text, &g_var, &g_b, &rb)) {
        rep->m3_violations++;
        mm_say("  M3 VIOLATION (%s): single %s\n", what, g_why);
        return 1;
    }
    diff = m3_compare(&g_a, ra, &g_b, rb);
    if (diff != NULL) {
        rep->m3_violations++;
        mm_say("  M3 VIOLATION (%s): %s\n", what, diff);
        dump_parts(ra, rb);
        return 1;
    }
    return 0;
}

static void m3_check(mm_report *rep) {
    uint32_t t;
    uint32_t i;
    uint32_t j;
    for (t = 0; t < MM_OUTER_BUDGETS; t++) {
        for (i = 0; i < MM_OUTER_BUDGETS; i++) {
            for (j = 0; j < MM_BUDGETS; j++) {
                (void)m3_case(&g_outer_budgets[t], &g_outer_budgets[i], &g_budgets[j], rep);
            }
        }
    }
}

/* ---- the corpus sweep, run once ---------------------------------------- */

static mm_report g_rep;
static int g_swept;

static void sweep(void) {
    uint32_t i;
    if (g_swept) return;
    g_swept = 1;
    memset(&g_rep, 0, sizeof(g_rep));
    for (i = 0; i < g_n_paths; i++) {
        uint32_t sc = 0;
        if (!load_scenario(g_paths[i], &g_scn, &sc)) {
            g_rep.load_failures++;
            fprintf(stderr, "  cannot read %s\n", g_paths[i]);
            continue;
        }
        check_scenario(g_paths[i], &g_scn, sc, &g_rep);
    }
    print_report("corpus", &g_rep);
}

/* ---- --fuzz: generated scenarios, and the smallest that still violates ---- */

#define MM_REL_COUNT 4u
#define MM_FUZZ_CASES 10000u
/* Elements of one scenario a reduction can delete, and evaluations it may
 * spend (each runs the relations over a candidate). */
#define MM_DEL_CAP 4096u
#define MM_DDMIN_EVALS 4000u

static const char *const g_rel_name[MM_REL_COUNT] = {"M1", "M2", "M3", "M4"};

static asx_json_doc g_cand; /* a candidate reduction */
static asx_json_doc g_best; /* the smallest reproduction so far */
static uint8_t g_drop[MM_DEL_CAP];

static uint32_t rel_violations(const mm_report *r, uint32_t rel) {
    switch (rel) {
    case 0u: return r->m1_violations;
    case 1u: return r->m2_violations;
    case 2u: return r->m3_violations;
    default: return r->m4_violations;
    }
}

static uint32_t rel_cases(const mm_report *r, uint32_t rel) {
    switch (rel) {
    case 0u: return r->m1_variants;
    case 1u: return r->m2_variants;
    case 2u: return r->m3_pairs;
    default: return r->m4_variants;
    }
}

/* Arrays a reduction may delete elements of: the same as twin_run's
 * minimizer (tools/twin_run/src/minimize.rs). */
static int reducible_key(const char *key) {
    static const char *const keys[] = {"tasks", "script",  "regions", "channels",
                                       "sync",  "program", "steps",   "members"};
    size_t i;
    for (i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        if (strcmp(key, keys[i]) == 0) return 1;
    }
    return 0;
}

/* A copy of `node` of `src` in `dst` without the deletable elements that
 * `drop` marks, numbered in preorder: a dropped element's descendants are
 * still counted, so a number means the same element in every candidate. An
 * array's elements are deletable when `reducible`, which an object's members
 * set by their key; the arrays under "members" are programs, whose own
 * elements are deletable too (`members`). With `dst` NULL it only counts:
 * *idx ends as the number of deletable elements. */
static uint32_t del_copy(asx_json_doc *dst, const asx_json_doc *src, uint32_t node, int reducible,
                         int members, const uint8_t *drop, uint32_t *idx) {
    asx_json_type ty = asx_json_type_of(src, node);
    uint32_t out = ASX_JSON_NONE;
    uint32_t i;
    if (ty == ASX_JSON_OBJECT) {
        if (dst != NULL) out = asx_json_new_object(dst);
        for (i = 0; i < asx_json_count(src, node); i++) {
            uint32_t m = asx_json_item(src, node, i);
            const char *key = asx_json_key(src, m);
            uint32_t child;
            if (key == NULL) continue;
            child =
                del_copy(dst, src, m, reducible_key(key), strcmp(key, "members") == 0, drop, idx);
            if (dst != NULL) asx_json_set(dst, out, key, child);
        }
        return out;
    }
    if (ty == ASX_JSON_ARRAY) {
        if (dst != NULL) out = asx_json_new_array(dst);
        for (i = 0; i < asx_json_count(src, node); i++) {
            uint32_t e = asx_json_item(src, node, i);
            int dropped = 0;
            uint32_t child;
            if (reducible) {
                uint32_t me = (*idx)++;
                dropped = drop != NULL && me < MM_DEL_CAP && drop[me];
            }
            child = del_copy(dropped ? NULL : dst, src, e, members, 0, drop, idx);
            if (dst != NULL && !dropped) asx_json_push(dst, out, child);
        }
        return out;
    }
    return dst != NULL ? asx_json_copy(dst, src, node) : ASX_JSON_NONE;
}

typedef int (*mm_pred)(const asx_json_doc *d, uint32_t sc, void *ctx);

/* Delta debugging (ddmin) of scenario `sc` of `src` into g_best: chunks of
 * deletable elements, halving down to single ones, repeated at that size
 * until none can go, so the result is 1-minimal for deletions. `pred` says
 * whether a candidate still reproduces. *best_root is the result's root in
 * g_best; *evals is how many candidates were tried. 0 when `src` cannot be
 * copied. */
static int ddmin(const asx_json_doc *src, uint32_t sc, mm_pred pred, void *ctx, uint32_t *best_root,
                 uint32_t *evals) {
    uint32_t n = 0;
    uint32_t chunk;
    *evals = 0;
    asx_json_doc_init(&g_best);
    *best_root = asx_json_copy(&g_best, src, sc);
    if (*best_root == ASX_JSON_NONE || !asx_json_doc_ok(&g_best)) return 0;
    (void)del_copy(NULL, &g_best, *best_root, 0, 0, NULL, &n);
    if (n > MM_DEL_CAP) n = MM_DEL_CAP;
    chunk = n;
    while (chunk >= 1u && n > 0u && *evals < MM_DDMIN_EVALS) {
        int changed = 0;
        uint32_t start = 0;
        while (start < n && *evals < MM_DDMIN_EVALS) {
            uint32_t end = start + chunk < n ? start + chunk : n;
            uint32_t k;
            uint32_t idx = 0;
            uint32_t cr;
            memset(g_drop, 0, sizeof(g_drop));
            for (k = start; k < end; k++) g_drop[k] = 1;
            asx_json_doc_init(&g_cand);
            cr = del_copy(&g_cand, &g_best, *best_root, 0, 0, g_drop, &idx);
            (*evals)++;
            if (cr != ASX_JSON_NONE && asx_json_doc_ok(&g_cand) && pred(&g_cand, cr, ctx)) {
                asx_json_doc_init(&g_best);
                *best_root = asx_json_copy(&g_best, &g_cand, cr);
                n = 0;
                (void)del_copy(NULL, &g_best, *best_root, 0, 0, NULL, &n);
                if (n > MM_DEL_CAP) n = MM_DEL_CAP;
                changed = 1;
            } else {
                start = end;
            }
        }
        if (chunk == 1u) {
            if (!changed) break;
        } else {
            chunk = (chunk + 1u) / 2u;
        }
    }
    return 1;
}

/* Whether relation `*ctx` (0 to 3 for M1, M2, M3 and M4) is violated by
 * the scenario: it must still run, be schedule insensitive and be one the
 * relation applies to, as the original was. */
static int pred_relation(const asx_json_doc *d, uint32_t sc, void *ctx) {
    mm_report rep;
    int saved = g_quiet;
    uint32_t rel = *(const uint32_t *)ctx;
    memset(&rep, 0, sizeof(rep));
    g_quiet = 1;
    check_scenario("candidate", d, sc, &rep);
    g_quiet = saved;
    return rel_violations(&rep, rel) > 0u;
}

static uint32_t count_deletable(const asx_json_doc *d, uint32_t root) {
    uint32_t n = 0;
    (void)del_copy(NULL, d, root, 0, 0, NULL, &n);
    return n;
}

static int write_file(const char *dir, const char *name, const char *text) {
    char path[512];
    FILE *f;
    if (snprintf(path, sizeof(path), "%s/%s.json", dir, name) >= (int)sizeof(path)) return 0;
    f = fopen(path, "wb");
    if (f == NULL) return 0;
    (void)fputs(text, f);
    (void)fputc('\n', f);
    return fclose(f) == 0;
}

/* The reduction of scenario `sc` of `d`, which violates relation `rel`, as a
 * scenario file in `dir`: the fixture candidate. */
static void minimize_and_write(const char *dir, uint32_t ordinal, uint32_t rel,
                               const asx_json_doc *d, uint32_t sc) {
    uint32_t root = 0;
    uint32_t evals = 0;
    uint32_t before = count_deletable(d, sc);
    uint32_t after;
    uint32_t id_node;
    uint32_t desc_node;
    uint32_t out;
    const char *orig = asx_json_get_string(d, sc, "id");
    char id[96];
    char desc[400];
    const char *text;
    mm_override over[2];
    int saved = g_quiet;
    g_quiet = 1;
    if (!ddmin(d, sc, pred_relation, &rel, &root, &evals)) {
        g_quiet = saved;
        fprintf(stderr, "  %s: the violating scenario could not be copied to minimize it\n",
                g_rel_name[rel]);
        return;
    }
    g_quiet = saved;
    after = count_deletable(&g_best, root);
    (void)snprintf(id, sizeof(id), "mm-%c%c-min-%03u-001", 'm' + 0, g_rel_name[rel][1],
                   (unsigned)ordinal);
    (void)snprintf(desc, sizeof(desc),
                   "%s violated: minimized by ddmin from %s (%u deletable elements to %u in %u "
                   "tries; bd-9kll.12.1). Run test_metamorphic on this file to see it.",
                   g_rel_name[rel], orig != NULL ? orig : "?", (unsigned)before, (unsigned)after,
                   (unsigned)evals);
    asx_json_doc_init(&g_cand);
    id_node = asx_json_new_string(&g_cand, id);
    desc_node = asx_json_new_string(&g_cand, desc);
    over[0].key = "id";
    over[0].node = id_node;
    over[1].key = "description";
    over[1].node = desc_node;
    out = copy_with(&g_cand, &g_best, root, over, 2u);
    text = canon_into(&g_cand, out, g_canon);
    if (text == NULL || !write_file(dir, id, text)) {
        fprintf(stderr, "  %s: cannot write the minimized scenario to %s\n", g_rel_name[rel], dir);
        return;
    }
    fprintf(stderr, "  %s: minimized %s from %u to %u deletable elements (%u tries): %s/%s.json\n",
            g_rel_name[rel], orig != NULL ? orig : "?", (unsigned)before, (unsigned)after,
            (unsigned)evals, dir, id);
}

/* Read the three budgets back from an M3 case (generate --shape meta): the
 * budgets of the open_region steps that name r.1, r.2 and r.3. A missing
 * budget is null. 0 when a region is not opened. */
static int find_open_region(const asx_json_doc *d, uint32_t node, const char *name,
                            uint32_t *budget) {
    asx_json_type ty = asx_json_type_of(d, node);
    uint32_t i;
    if (ty == ASX_JSON_OBJECT) {
        const char *op = asx_json_get_string(d, node, "op");
        const char *as = asx_json_get_string(d, node, "as");
        if (op != NULL && as != NULL && strcmp(op, "open_region") == 0 && strcmp(as, name) == 0) {
            *budget = asx_json_get(d, node, "budget");
            return 1;
        }
    }
    if (ty != ASX_JSON_ARRAY && ty != ASX_JSON_OBJECT) return 0;
    for (i = 0; i < asx_json_count(d, node); i++) {
        if (find_open_region(d, asx_json_item(d, node, i), name, budget)) return 1;
    }
    return 0;
}

static void budget_read(const asx_json_doc *d, uint32_t node, mm_budget *b) {
    uint64_t v = 0;
    memset(b, 0, sizeof(*b));
    if (node == ASX_JSON_NONE || asx_json_is_null(d, node)) {
        b->infinite = 1;
        return;
    }
    b->priority = 128u;
    if (asx_json_u64(d, asx_json_get(d, node, "priority"), &v)) b->priority = (unsigned)v;
    if (asx_json_u64(d, asx_json_get(d, node, "deadline_ns"), &v)) {
        b->has_deadline = 1;
        b->deadline = v;
    }
    if (asx_json_u64(d, asx_json_get(d, node, "poll_quota"), &v)) {
        b->has_poll = 1;
        b->poll = v;
    }
    if (asx_json_u64(d, asx_json_get(d, node, "cost_quota"), &v)) {
        b->has_cost = 1;
        b->cost = v;
    }
}

static int m3_read(const asx_json_doc *d, uint32_t sc, mm_budget *top, mm_budget *a, mm_budget *b) {
    uint32_t tasks = asx_json_get(d, sc, "tasks");
    uint32_t nt = 0;
    uint32_t na = 0;
    uint32_t nb = 0;
    if (!find_open_region(d, tasks, "r.1", &nt) || !find_open_region(d, tasks, "r.2", &na) ||
        !find_open_region(d, tasks, "r.3", &nb)) {
        return 0;
    }
    budget_read(d, nt, top);
    budget_read(d, na, a);
    budget_read(d, nb, b);
    return 1;
}

static int m3_violates(const mm_budget *t, const mm_budget *a, const mm_budget *b) {
    mm_report rep;
    int saved = g_quiet;
    memset(&rep, 0, sizeof(rep));
    g_quiet = 1;
    (void)m3_case(t, a, b, &rep);
    g_quiet = saved;
    return rep.m3_violations > 0u;
}

/* Reduce the three budgets of a violating M3 case: each to null, else
 * field by field, while it still violates. */
static void m3_minimize(mm_budget *t, mm_budget *a, mm_budget *b) {
    mm_budget *all[3];
    int changed;
    all[0] = t;
    all[1] = a;
    all[2] = b;
    do {
        uint32_t i;
        changed = 0;
        for (i = 0; i < 3u; i++) {
            mm_budget *x = all[i];
            mm_budget keep = *x;
            mm_budget none;
            memset(&none, 0, sizeof(none));
            none.infinite = 1;
            if (!x->infinite) {
                *x = none;
                if (m3_violates(t, a, b)) {
                    changed = 1;
                    continue;
                }
                *x = keep;
            }
            if (x->has_deadline) {
                x->has_deadline = 0;
                x->deadline = 0;
                if (m3_violates(t, a, b)) {
                    changed = 1;
                } else {
                    x->has_deadline = 1;
                    x->deadline = keep.deadline;
                }
            }
            if (x->has_poll) {
                x->has_poll = 0;
                x->poll = 0;
                if (m3_violates(t, a, b)) {
                    changed = 1;
                } else {
                    x->has_poll = 1;
                    x->poll = keep.poll;
                }
            }
            if (x->has_cost) {
                x->has_cost = 0;
                x->cost = 0;
                if (m3_violates(t, a, b)) {
                    changed = 1;
                } else {
                    x->has_cost = 1;
                    x->cost = keep.cost;
                }
            }
            if (!x->infinite && x->priority != 128u) {
                unsigned p = x->priority;
                x->priority = 128u;
                if (m3_violates(t, a, b)) {
                    changed = 1;
                } else {
                    x->priority = p;
                }
            }
        }
    } while (changed);
}

/* The two forms of a reduced M3 case, as scenario files in `dir`. */
static void m3_write(const char *dir, uint32_t ordinal, const mm_budget *t, const mm_budget *a,
                     const mm_budget *b) {
    mm_budget met = budget_meet(a, b);
    char jt[128];
    char ja[128];
    char jb[128];
    char jm[128];
    char text[4096];
    char name[96];
    int nested;
    budget_json(t, jt, sizeof(jt));
    budget_json(a, ja, sizeof(ja));
    budget_json(b, jb, sizeof(jb));
    budget_json(&met, jm, sizeof(jm));
    for (nested = 1; nested >= 0; nested--) {
        (void)m3_text(text, sizeof(text), jt, ja, jb, jm, nested);
        (void)snprintf(name, sizeof(name), "mm-m3-min-%03u-%s-001", (unsigned)ordinal,
                       nested ? "nested" : "single");
        if (!write_file(dir, name, text)) {
            fprintf(stderr, "  M3: cannot write %s/%s.json\n", dir, name);
        }
    }
    fprintf(stderr,
            "  M3: minimized to top %s, nested %s then %s, single %s: %s/mm-m3-min-%03u-*\n", jt,
            ja, jb, jm, dir, (unsigned)ordinal);
}

/* `--fuzz [FILE|-]`: scenarios, one canonical JSON per line (twin_run
 * generate --shape meta --jsonl), checked until each relation has
 * ASX_METAMORPHIC_CASES (default 10000) compared cases. The scenarios named
 * `mm-m3-*` are M3 cases (their three budgets are read back); the rest go
 * through M1, M2 and M4. The first violation of a relation is reduced by
 * ddmin and written to ASX_METAMORPHIC_OUT (default .). Exit 0 when every
 * relation held over its cases, 1 on a violation, 2 when the stream ended
 * short of the cases (a run that checked nothing is no proof). */
static int fuzz_main(const char *src) {
    FILE *in = strcmp(src, "-") == 0 ? stdin : fopen(src, "rb");
    const char *env;
    const char *dir;
    uint32_t cases = MM_FUZZ_CASES;
    uint32_t minimized = 0;
    uint32_t seen[MM_REL_COUNT];
    uint32_t k;
    uint32_t lines = 0;
    mm_report rep;
    int violated = 0;
    int short_of = 0;
    if (in == NULL) {
        fprintf(stderr, "  fuzz: cannot open %s\n", src);
        return 2;
    }
    env = getenv("ASX_METAMORPHIC_CASES");
    if (env != NULL && atoi(env) > 0) cases = (uint32_t)atoi(env);
    dir = getenv("ASX_METAMORPHIC_OUT");
    if (dir == NULL || dir[0] == '\0') dir = ".";
    memset(&rep, 0, sizeof(rep));
    memset(seen, 0, sizeof(seen));
    while (fgets(g_file, sizeof(g_file), in) != NULL) {
        size_t len = strlen(g_file);
        uint32_t sc = 0;
        uint32_t before[MM_REL_COUNT];
        const char *id;
        int all = 1;
        if (len + 1u >= sizeof(g_file)) {
            fprintf(stderr, "  fuzz: a scenario line is longer than %u bytes\n",
                    (unsigned)sizeof(g_file));
            return 2;
        }
        asx_json_doc_init(&g_scn);
        if (len == 0u || asx_json_parse(&g_scn, g_file, len, &sc) != ASX_OK) {
            rep.load_failures++;
            fprintf(stderr, "  fuzz: line %u is not a scenario (%u bytes): %.80s\n",
                    (unsigned)(lines + rep.load_failures), (unsigned)len, g_file);
            continue;
        }
        lines++;
        id = asx_json_get_string(&g_scn, sc, "id");
        if (id == NULL) id = "?";
        for (k = 0; k < MM_REL_COUNT; k++) before[k] = rel_violations(&rep, k);
        if (strncmp(id, "mm-m3-", 6u) == 0) {
            mm_budget t;
            mm_budget a;
            mm_budget b;
            if (rep.m3_pairs < cases) {
                if (!m3_read(&g_scn, sc, &t, &a, &b)) {
                    rep.load_failures++;
                    fprintf(stderr, "  fuzz: %s does not hold the three nested regions\n", id);
                } else if (m3_case(&t, &a, &b, &rep) && seen[2] == 0u) {
                    seen[2] = 1u;
                    m3_minimize(&t, &a, &b);
                    m3_write(dir, ++minimized, &t, &a, &b);
                }
            }
        } else {
            check_scenario(id, &g_scn, sc, &rep);
            for (k = 0; k < MM_REL_COUNT; k++) {
                if (k == 2u || rel_violations(&rep, k) <= before[k] || seen[k] != 0u) continue;
                seen[k] = 1u;
                minimize_and_write(dir, ++minimized, k, &g_scn, sc);
            }
        }
        for (k = 0; k < MM_REL_COUNT; k++) {
            if (rel_cases(&rep, k) < cases) all = 0;
        }
        if (all) break;
    }
    if (in != stdin) (void)fclose(in);
    fprintf(stderr,
            "  fuzz: %u scenarios read; skipped %u (not run) %u (schedule sensitive) %u "
            "(analysis) %u (unreadable)\n",
            (unsigned)lines, (unsigned)rep.skipped_baseline, (unsigned)rep.skipped_sensitive,
            (unsigned)rep.skipped_analysis, (unsigned)rep.load_failures);
    for (k = 0; k < MM_REL_COUNT; k++) {
        fprintf(stderr, "  fuzz: %s %u cases, %u violation(s)%s\n", g_rel_name[k],
                (unsigned)rel_cases(&rep, k), (unsigned)rel_violations(&rep, k),
                rel_cases(&rep, k) < cases ? "  (SHORT of the cases asked for)" : "");
        if (rel_violations(&rep, k) > 0u) violated = 1;
        if (rel_cases(&rep, k) < cases) short_of = 1;
    }
    fprintf(stderr, "  fuzz: M2 skipped %u variant(s) at a capacity limit\n",
            (unsigned)rep.m2_capacity);
    if (violated) return 1;
    return short_of ? 2 : 0;
}

/* ---- tests ------------------------------------------------------------- */

/* Three tasks that share nothing, two of them introducing the same local
 * name: M1 and M2 must apply to it and hold. */
static const char g_inline[] =
    "{\"schema\":\"asx.scenario.v2\",\"id\":\"mm-inline-001\",\"description\":\"d\","
    "\"unit\":\"scheduler\",\"rules\":[\"inv.determinism.replayable\"],\"seed\":42,"
    "\"lab\":{\"max_steps\":600},"
    "\"regions\":[{\"name\":\"r.main\",\"parent\":\"root\",\"budget\":null}],"
    "\"channels\":[],\"sync\":[],"
    "\"tasks\":["
    "{\"name\":\"t.a\",\"region\":\"r.main\",\"budget\":null,\"program\":["
    "{\"op\":\"trace\",\"message\":\"a1\"},{\"op\":\"sleep\",\"ns\":50},"
    "{\"op\":\"reserve\",\"kind\":\"Lease\",\"as\":\"o\"},{\"op\":\"yield\"},"
    "{\"op\":\"commit\",\"obligation\":\"o\"}]},"
    "{\"name\":\"t.b\",\"region\":\"r.main\",\"budget\":null,\"program\":["
    "{\"op\":\"trace\",\"message\":\"b1\"},{\"op\":\"sleep\",\"ns\":20},"
    "{\"op\":\"reserve\",\"kind\":\"Ack\",\"as\":\"o\"},"
    "{\"op\":\"abort\",\"obligation\":\"o\",\"reason\":\"Explicit\"}]},"
    "{\"name\":\"t.c\",\"region\":\"r.main\",\"budget\":null,\"program\":["
    "{\"op\":\"yield\"},{\"op\":\"trace\",\"message\":\"c1\"}]}"
    "],\"script\":[],\"expect\":{\"kind\":\"ok\"}}";

TEST(an_inline_scenario_of_independent_tasks_satisfies_m1_and_m2) {
    mm_report rep;
    uint32_t sc = 0;
    mm_info info;
    int verbose;

    memset(&rep, 0, sizeof(rep));
    asx_json_doc_init(&g_scn);
    ASSERT_EQ(asx_json_parse(&g_scn, g_inline, strlen(g_inline), &sc), ASX_OK);
    analyze(&g_scn, sc, &info);
    ASSERT_TRUE(info.why == NULL);
    ASSERT_EQ(info.n_iso, 3u);
    verbose = g_verbose;
    g_verbose = 1;
    check_scenario("inline", &g_scn, sc, &rep);
    g_verbose = verbose;
    print_report("inline", &rep);
    ASSERT_EQ(rep.skipped_baseline + rep.skipped_sensitive + rep.skipped_analysis, 0u);
    ASSERT_EQ(rep.m1_applied, 1u);
    ASSERT_EQ(rep.m1_variants, 2u);
    ASSERT_EQ(rep.m2_applied, 1u);
    ASSERT_EQ(rep.m2_variants, 2u);
    ASSERT_EQ(rep.m2_capacity, 0u);
    ASSERT_EQ(rep.m1_violations + rep.m2_violations, 0u);
}

/* Tasks that mention one channel are not isolated, and the analysis says
 * so; a no-op that nobody names is isolated. */
TEST(the_analysis_keeps_tasks_that_share_a_name_in_place) {
    static const char text[] =
        "{\"schema\":\"asx.scenario.v2\",\"id\":\"mm-share-001\",\"description\":\"d\","
        "\"unit\":\"channel\",\"rules\":[\"inv.determinism.replayable\"],\"seed\":42,"
        "\"lab\":{\"max_steps\":600},"
        "\"regions\":[{\"name\":\"r.main\",\"parent\":\"root\",\"budget\":null}],"
        "\"channels\":[{\"name\":\"ch.q\",\"type\":\"mpsc\",\"capacity\":2,\"sender\":\"t.p\","
        "\"receiver\":\"t.c\"}],\"sync\":[],"
        "\"tasks\":["
        "{\"name\":\"t.p\",\"region\":\"r.main\",\"budget\":null,\"program\":["
        "{\"op\":\"send\",\"channel\":\"ch.q\",\"value\":1},"
        "{\"op\":\"close_sender\",\"channel\":\"ch.q\"}]},"
        "{\"name\":\"t.c\",\"region\":\"r.main\",\"budget\":null,\"program\":["
        "{\"op\":\"recv\",\"channel\":\"ch.q\"},{\"op\":\"recv\",\"channel\":\"ch.q\"}]},"
        "{\"name\":\"t.x\",\"region\":\"r.main\",\"budget\":null,\"program\":["
        "{\"op\":\"yield\"}]}"
        "],\"script\":[],\"expect\":{\"kind\":\"ok\"}}";
    uint32_t sc = 0;
    mm_info info;

    asx_json_doc_init(&g_scn);
    ASSERT_EQ(asx_json_parse(&g_scn, text, strlen(text), &sc), ASX_OK);
    analyze(&g_scn, sc, &info);
    ASSERT_TRUE(info.why == NULL);
    ASSERT_EQ(info.n_tasks, 3u);
    ASSERT_EQ(info.n_iso, 1u);
    ASSERT_EQ(info.iso[0], 2u); /* t.x only */
}

/* The comparator is not vacuous: a planted difference in each part of the
 * projection is reported, and an unchanged copy is not. */
TEST(the_comparator_reports_a_planted_difference_in_each_part) {
    uint32_t sc = 0;
    uint32_t ra = 0;
    uint32_t root;
    uint32_t snap;
    uint32_t obs;
    uint32_t trace;
    uint32_t layer;
    uint32_t extra;
    mm_override over[1];
    mm_override snap_over[1];

    asx_json_doc_init(&g_scn);
    ASSERT_EQ(asx_json_parse(&g_scn, g_inline, strlen(g_inline), &sc), ASX_OK);
    ASSERT_EQ(run_scn(&g_scn, sc, &g_a, &ra), ASX_OK);
    snap = asx_json_get(&g_a, ra, "snapshot");
    obs = asx_json_get(&g_a, ra, "observations");
    trace = asx_json_get(&g_a, ra, "trace");
    ASSERT_TRUE(snap != ASX_JSON_NONE && obs != ASX_JSON_NONE && trace != ASX_JSON_NONE);
    ASSERT_TRUE(asx_json_count(&g_a, trace) > 0u);

    /* An unchanged copy. */
    asx_json_doc_init(&g_b);
    root = copy_with(&g_b, &g_a, ra, NULL, 0u);
    ASSERT_TRUE(mm_difference(&g_a, ra, &g_b, root, NULL, NULL) == NULL);

    /* The snapshot's clock moved. */
    asx_json_doc_init(&g_b);
    snap_over[0].key = "now_ns";
    snap_over[0].node = asx_json_new_u64(&g_b, 987654321u);
    over[0].key = "snapshot";
    over[0].node = copy_with(&g_b, &g_a, snap, snap_over, 1u);
    root = copy_with(&g_b, &g_a, ra, over, 1u);
    ASSERT_TRUE(mm_difference(&g_a, ra, &g_b, root, NULL, NULL) != NULL);

    /* An observation more. */
    asx_json_doc_init(&g_b);
    extra = asx_json_new_array(&g_b);
    for (layer = 0; layer < asx_json_count(&g_a, obs); layer++) {
        asx_json_push(&g_b, extra, asx_json_copy(&g_b, &g_a, asx_json_item(&g_a, obs, layer)));
    }
    {
        uint32_t o = asx_json_new_object(&g_b);
        asx_json_set(&g_b, o, "op", asx_json_new_string(&g_b, "planted"));
        asx_json_push(&g_b, extra, o);
    }
    over[0].key = "observations";
    over[0].node = extra;
    root = copy_with(&g_b, &g_a, ra, over, 1u);
    ASSERT_TRUE(mm_difference(&g_a, ra, &g_b, root, NULL, NULL) != NULL);

    /* An event more. */
    asx_json_doc_init(&g_b);
    extra = asx_json_new_array(&g_b);
    for (layer = 0; layer < asx_json_count(&g_a, trace); layer++) {
        asx_json_push(&g_b, extra, asx_json_copy(&g_b, &g_a, asx_json_item(&g_a, trace, layer)));
    }
    {
        uint32_t ev = asx_json_new_object(&g_b);
        uint32_t lay = asx_json_new_array(&g_b);
        asx_json_set(&g_b, ev, "k", asx_json_new_string(&g_b, "user.trace"));
        asx_json_set(&g_b, ev, "message", asx_json_new_string(&g_b, "planted"));
        asx_json_push(&g_b, lay, ev);
        asx_json_push(&g_b, extra, lay);
    }
    over[0].key = "trace";
    over[0].node = extra;
    root = copy_with(&g_b, &g_a, ra, over, 1u);
    ASSERT_TRUE(mm_difference(&g_a, ra, &g_b, root, NULL, NULL) != NULL);
}

/* The no-op filter of M2: a real no-op variant passes, and the same
 * variant with an event of another entity added does not. */
TEST(the_no_op_filter_hides_the_no_op_and_nothing_else) {
    uint32_t sc = 0;
    uint32_t ra = 0;
    uint32_t vr = 0;
    uint32_t rb = 0;
    uint32_t trace;
    uint32_t extra;
    uint32_t layer;
    uint32_t root;
    mm_override over[1];

    asx_json_doc_init(&g_scn);
    ASSERT_EQ(asx_json_parse(&g_scn, g_inline, strlen(g_inline), &sc), ASX_OK);
    ASSERT_EQ(run_scn(&g_scn, sc, &g_a, &ra), ASX_OK);
    ASSERT_TRUE(build_with_noop(&g_scn, sc, 1, 1, &vr));
    ASSERT_EQ(run_scn(&g_var, vr, &g_b, &rb), ASX_OK);
    /* Without the filter the variant differs (it has the no-op's entities);
     * with it, it does not. */
    ASSERT_TRUE(mm_difference(&g_a, ra, &g_b, rb, NULL, NULL) != NULL);
    ASSERT_TRUE(mm_difference(&g_a, ra, &g_b, rb, MM_NOOP_TASK, MM_NOOP_REGION) == NULL);

    trace = asx_json_get(&g_b, rb, "trace");
    asx_json_doc_init(&g_var);
    extra = asx_json_new_array(&g_var);
    for (layer = 0; layer < asx_json_count(&g_b, trace); layer++) {
        asx_json_push(&g_var, extra,
                      asx_json_copy(&g_var, &g_b, asx_json_item(&g_b, trace, layer)));
    }
    {
        uint32_t ev = asx_json_new_object(&g_var);
        uint32_t lay = asx_json_new_array(&g_var);
        asx_json_set(&g_var, ev, "k", asx_json_new_string(&g_var, "user.trace"));
        asx_json_set(&g_var, ev, "message", asx_json_new_string(&g_var, "planted"));
        asx_json_push(&g_var, lay, ev);
        asx_json_push(&g_var, extra, lay);
    }
    over[0].key = "trace";
    over[0].node = extra;
    root = copy_with(&g_var, &g_b, rb, over, 1u);
    ASSERT_TRUE(mm_difference(&g_a, ra, &g_var, root, MM_NOOP_TASK, MM_NOOP_REGION) != NULL);
}

TEST(m1_permuting_independent_tasks_keeps_the_projection) {
    sweep();
    ASSERT_EQ(g_rep.load_failures, 0u);
    ASSERT_EQ(g_rep.m1_violations, 0u);
}

TEST(m2_a_no_op_task_changes_no_other_entity) {
    sweep();
    ASSERT_EQ(g_rep.load_failures, 0u);
    ASSERT_EQ(g_rep.m2_violations, 0u);
}

TEST(m3_nested_region_budgets_equal_their_meet) {
    mm_report rep;
    memset(&rep, 0, sizeof(rep));
    m3_check(&rep);
    print_report("m3", &rep);
    ASSERT_EQ(rep.m3_pairs, (uint32_t)(MM_OUTER_BUDGETS * MM_OUTER_BUDGETS * MM_BUDGETS));
    ASSERT_EQ(rep.m3_violations, 0u);
}

TEST(m4_a_weaker_cancel_after_a_stronger_one_changes_nothing) {
    sweep();
    ASSERT_EQ(g_rep.load_failures, 0u);
    ASSERT_EQ(g_rep.m4_violations, 0u);
}

/* The converse of M4 is not a relation: a Shutdown cancel after a User one
 * strengthens the reason. At least one scenario of the corpus must show it,
 * through the same transform and comparator, so M4 is not holding because
 * a duplicate cancel is invisible to them. */
TEST(m4_a_stronger_cancel_after_a_weaker_one_is_seen) {
    uint32_t seen = 0;
    uint32_t i;
    for (i = 0; i < g_n_paths; i++) {
        uint32_t sc = 0;
        uint32_t ra = 0;
        uint32_t vr = 0;
        uint32_t rb = 0;
        uint32_t added = 0;
        const char *base;
        const char *var;
        int had_shutdown;
        if (!load_scenario(g_paths[i], &g_scn, &sc)) continue;
        if (run_scn(&g_scn, sc, &g_a, &ra) != ASX_OK) continue;
        base = canon_into(&g_a, asx_json_get(&g_a, ra, "snapshot"), g_canon);
        if (base == NULL) continue;
        had_shutdown = strstr(base, "\"kind\":\"Shutdown\"") != NULL;
        if (!build_cancel_duplicates(&g_scn, sc, 0, &vr, &added) || added == 0u) continue;
        if (run_scn(&g_var, vr, &g_b, &rb) != ASX_OK) continue;
        var = canon_into(&g_b, asx_json_get(&g_b, rb, "snapshot"), g_canon);
        if (var != NULL && !had_shutdown && strstr(var, "\"kind\":\"Shutdown\"") != NULL) seen++;
    }
    fprintf(stderr, "  M4 converse: %u scenario(s) end with a Shutdown reason they lacked\n",
            (unsigned)seen);
    ASSERT_TRUE(seen >= 1u);
}

/* Fails closed: a corpus that skips almost everything proves nothing. Only
 * the default resource class has room for the larger scenarios. */
TEST(the_corpus_exercises_the_relations) {
    sweep();
    ASSERT_TRUE(g_rep.m1_variants >= MM_M1_MIN);
    ASSERT_TRUE(g_rep.m2_variants >= MM_M2_MIN);
    ASSERT_TRUE(g_rep.m4_variants >= MM_M4_MIN);
}

/* ---- the fuzz mode's own parts ------------------------------------------ */

static int pred_has_needle(const asx_json_doc *d, uint32_t sc, void *ctx) {
    const char *text = canon_into(d, sc, g_canon);
    (void)ctx;
    return text != NULL && strstr(text, "\"needle\"") != NULL;
}

/* ddmin keeps what the predicate needs and deletes the rest: tasks, steps
 * (also inside a spawned program), regions, sync declarations and script
 * entries, down to the one step the predicate looks for. */
TEST(the_minimizer_deletes_everything_the_predicate_does_not_need) {
    static const char text[] =
        "{\"schema\":\"asx.scenario.v2\",\"id\":\"mm-ddmin-001\",\"description\":\"d\","
        "\"unit\":\"scheduler\",\"rules\":[\"inv.determinism.replayable\"],\"seed\":42,"
        "\"lab\":{\"max_steps\":600},"
        "\"regions\":[{\"name\":\"r.main\",\"parent\":\"root\",\"budget\":null},"
        "{\"name\":\"r.x\",\"parent\":\"r.main\",\"budget\":null}],"
        "\"channels\":[],\"sync\":[{\"name\":\"m\",\"type\":\"mutex\"}],"
        "\"tasks\":["
        "{\"name\":\"t.a\",\"region\":\"r.main\",\"budget\":null,\"program\":["
        "{\"op\":\"yield\"},{\"op\":\"trace\",\"message\":\"a1\"},{\"op\":\"sleep\",\"ns\":50}]},"
        "{\"name\":\"t.b\",\"region\":\"r.main\",\"budget\":null,\"program\":["
        "{\"op\":\"trace\",\"message\":\"b1\"},"
        "{\"op\":\"spawn\",\"as\":\"t.b.c\",\"program\":[{\"op\":\"yield\"},"
        "{\"op\":\"trace\",\"message\":\"needle\"},{\"op\":\"yield\"}]},"
        "{\"op\":\"join\",\"task\":\"t.b.c\"}]},"
        "{\"name\":\"t.c\",\"region\":\"r.x\",\"budget\":null,\"program\":["
        "{\"op\":\"checkpoint\"}]}],"
        "\"script\":[{\"at_ns\":10,\"op\":{\"op\":\"advance\",\"ns\":5}}],"
        "\"expect\":{\"kind\":\"ok\"}}";
    uint32_t sc = 0;
    uint32_t root = 0;
    uint32_t evals = 0;
    uint32_t tasks;
    uint32_t spawn;
    asx_json_doc_init(&g_scn);
    ASSERT_EQ(asx_json_parse(&g_scn, text, strlen(text), &sc), ASX_OK);
    ASSERT_TRUE(count_deletable(&g_scn, sc) > 10u);
    ASSERT_TRUE(ddmin(&g_scn, sc, pred_has_needle, NULL, &root, &evals));
    ASSERT_TRUE(evals > 0u);
    tasks = asx_json_get(&g_best, root, "tasks");
    ASSERT_EQ(asx_json_count(&g_best, tasks), 1u);
    ASSERT_EQ(asx_json_count(&g_best, asx_json_get(&g_best, root, "script")), 0u);
    ASSERT_EQ(asx_json_count(&g_best, asx_json_get(&g_best, root, "regions")), 0u);
    ASSERT_EQ(asx_json_count(&g_best, asx_json_get(&g_best, root, "sync")), 0u);
    spawn = asx_json_item(&g_best,
                          asx_json_get(&g_best, asx_json_item(&g_best, tasks, 0), "program"), 0);
    ASSERT_EQ(asx_json_count(&g_best,
                             asx_json_get(&g_best, asx_json_item(&g_best, tasks, 0), "program")),
              1u);
    ASSERT_EQ(asx_json_count(&g_best, asx_json_get(&g_best, spawn, "program")), 1u);
    /* 1-minimal: nothing is left to delete but what the predicate needs
     * (the task, its spawn, the one step). */
    ASSERT_EQ(count_deletable(&g_best, root), 3u);
}

/* The scenario `generate --shape meta` writes for an M3 case carries the
 * three budgets; the driver reads them back and builds both forms itself. */
TEST(the_budgets_of_an_m3_case_are_read_back_from_its_scenario) {
    static const char *const jt = "null";
    static const char *const ja = "{\"priority\":7,\"deadline_ns\":300}";
    static const char *const jb = "{\"priority\":200,\"poll_quota\":5,\"cost_quota\":9}";
    char text[4096];
    uint32_t sc = 0;
    mm_budget t;
    mm_budget a;
    mm_budget b;
    (void)m3_text(text, sizeof(text), jt, ja, jb, "null", 1);
    asx_json_doc_init(&g_scn);
    ASSERT_EQ(asx_json_parse(&g_scn, text, strlen(text), &sc), ASX_OK);
    ASSERT_TRUE(m3_read(&g_scn, sc, &t, &a, &b));
    ASSERT_TRUE(t.infinite);
    ASSERT_TRUE(!a.infinite && a.priority == 7u && a.has_deadline && a.deadline == 300u);
    ASSERT_TRUE(!a.has_poll && !a.has_cost);
    ASSERT_TRUE(b.priority == 200u && b.has_poll && b.poll == 5u && b.has_cost && b.cost == 9u);
    ASSERT_TRUE(!b.has_deadline);
    /* A scenario with no such regions is not an M3 case. */
    ASSERT_EQ(asx_json_parse(&g_scn, g_inline, strlen(g_inline), &sc), ASX_OK);
    ASSERT_TRUE(!m3_read(&g_scn, sc, &t, &a, &b));
}

int main(int argc, char **argv) {
    const char *seeds = getenv("ASX_METAMORPHIC_SEEDS");
    if (seeds != NULL && atoi(seeds) > 0) g_seeds = (uint32_t)atoi(seeds);
    if (argc >= 2 && strcmp(argv[1], "--fuzz") == 0) {
        return fuzz_main(argc >= 3 ? argv[2] : "-");
    }
    fprintf(stderr, "=== test_metamorphic ===\n");
    g_verbose = getenv("ASX_METAMORPHIC_VERBOSE") != NULL;
    if (!collect_corpus(argc, argv)) {
        fprintf(stderr, "  FAIL: no scenarios (run from the repository root: %s)\n", MM_DIR);
        return 1;
    }
    RUN_TEST(an_inline_scenario_of_independent_tasks_satisfies_m1_and_m2);
    RUN_TEST(the_analysis_keeps_tasks_that_share_a_name_in_place);
    RUN_TEST(the_comparator_reports_a_planted_difference_in_each_part);
    RUN_TEST(the_no_op_filter_hides_the_no_op_and_nothing_else);
    RUN_TEST(the_minimizer_deletes_everything_the_predicate_does_not_need);
    RUN_TEST(the_budgets_of_an_m3_case_are_read_back_from_its_scenario);
    RUN_TEST(m1_permuting_independent_tasks_keeps_the_projection);
    /* The no-op takes a task and a region, and M3's nested form four regions
     * and the root: in a class with fewer, a scenario at the edge has a
     * spawn or a region refused that the other form did not, which is the
     * class's limit and not a semantic difference. */
    RUN_TEST_IF(ASX_MAX_REGIONS >= 8 && ASX_MAX_TASKS >= 64,
                m2_a_no_op_task_changes_no_other_entity,
                "this resource class has too few regions or tasks for the added no-op");
    RUN_TEST_IF(ASX_MAX_REGIONS >= 6, m3_nested_region_budgets_equal_their_meet,
                "this resource class has too few regions for M3's nested form");
    RUN_TEST(m4_a_weaker_cancel_after_a_stronger_one_changes_nothing);
    RUN_TEST(m4_a_stronger_cancel_after_a_weaker_one_is_seen);
    RUN_TEST_IF(ASX_MAX_REGIONS >= 8 && ASX_MAX_TASKS >= 64, the_corpus_exercises_the_relations,
                "this resource class is too small to run most of the corpus");
    TEST_REPORT();
    return test_failures;
}
