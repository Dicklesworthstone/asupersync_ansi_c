/*
 * test_metamorphic.c — metamorphic relations M1 and M2 on the C interpreter
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
 * Both relations hold only where the scenario's outcome does not depend on
 * how the lab breaks ties: adding or reordering tasks legitimately changes
 * which of two contenders the seeded pick serves first. A scenario whose
 * semantic digest changes with the seed alone is skipped as schedule
 * sensitive (counted, never passed), and so is one the C runtime does not
 * run at all or one its profile forbids. The test fails closed on a
 * corpus that exercises neither relation.
 *
 * The negative controls: the comparator must see a planted difference in
 * each projection part, and tests/gates/test_metamorphic_mutants.sh builds
 * this test against a runtime with an order-dependent bug and expects both
 * relations to report it.
 *
 * Run from the repository root; with paths as arguments it checks those
 * scenarios instead of every .json file of tests/conformance/scenarios_v2.
 * Setting ASX_METAMORPHIC_VERBOSE prints every skip with its reason.
 *
 * SPDX-License-Identifier: MIT
 */

#define _POSIX_C_SOURCE 200809L

#include "../../test_harness.h"
#include "conformance/interpreter.h"

#include <asx/asx_config.h>

#include <dirent.h>
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
 * without those that contain `skip1` or `skip2`, sorted. 0 on overflow. */
static int flatten_trace(const asx_json_doc *d, uint32_t trace, char *pool, const char **ev,
                         const char *skip1, const char *skip2, uint32_t *out_n) {
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

static const char *trace_difference(const asx_json_doc *da, uint32_t ta, const asx_json_doc *db,
                                    uint32_t tb, const char *skip1, const char *skip2) {
    uint32_t na = 0;
    uint32_t nb = 0;
    uint32_t i;
    if (!flatten_trace(da, ta, g_pool_a, g_ev_a, NULL, NULL, &na) ||
        !flatten_trace(db, tb, g_pool_b, g_ev_b, skip1, skip2, &nb)) {
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

/* How the variant's projection `rb` differs from the baseline's `ra`, or
 * NULL when it does not. With no no-op named the two must be identical
 * (M1). With one, the variant may hold exactly that task (and region, when
 * `noop_region` is set) and the events naming them, and nothing else may
 * differ, the no-op itself must have completed ok (M2). */
static const char *mm_difference(const asx_json_doc *da, uint32_t ra, const asx_json_doc *db,
                                 uint32_t rb, const char *noop_task, const char *noop_region) {
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
    return trace_difference(da, ta, db, tb, noop_task != NULL ? skip_task : NULL,
                            noop_region != NULL ? skip_region : NULL);
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
    for (k = 0; k < MM_SEEDS; k++) {
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
        fprintf(stderr, "  M1 VIOLATION %s (%s): the variant could not be built\n", label, how);
        return;
    }
    if (run_scn(&g_var, vr, &g_b, &rb) != ASX_OK) {
        rep->m1_violations++;
        fprintf(stderr, "  M1 VIOLATION %s (%s): the permuted scenario does not run: %s\n", label,
                how, g_err);
        return;
    }
    diff = mm_difference(&g_a, ra, &g_b, rb, NULL, NULL);
    if (diff != NULL) {
        rep->m1_violations++;
        fprintf(stderr, "  M1 VIOLATION %s (%s): %s\n", label, how, diff);
        dump_parts(ra, rb);
    }
}

/* 1 when the variant's observations hold a capacity refusal that the
 * baseline's do not. */
static int capacity_effect(const asx_json_doc *da, uint32_t ra, const asx_json_doc *db,
                           uint32_t rb) {
    static const char *const names[] = {"ASX_E_REGION_AT_CAPACITY", "ASX_E_RESOURCE_EXHAUSTED"};
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
        fprintf(stderr, "  M2 VIOLATION %s (%s): %s\n", label, how, diff);
        dump_parts(ra, rb);
    }
}

/* Classify scenario `sc` of `d` and, where it qualifies, check M1 and M2. */
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

/* Fails closed: a corpus that skips almost everything proves nothing. Only
 * the default resource class has room for the larger scenarios. */
TEST(the_corpus_exercises_both_relations) {
    sweep();
    ASSERT_TRUE(g_rep.m1_variants >= MM_M1_MIN);
    ASSERT_TRUE(g_rep.m2_variants >= MM_M2_MIN);
}

int main(int argc, char **argv) {
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
    RUN_TEST(m1_permuting_independent_tasks_keeps_the_projection);
    RUN_TEST(m2_a_no_op_task_changes_no_other_entity);
    RUN_TEST_IF(ASX_MAX_TASKS >= 64, the_corpus_exercises_both_relations,
                "this resource class is too small to run most of the corpus");
    TEST_REPORT();
    return test_failures;
}
