/*
 * timer_wheel.c — hierarchical timer wheel, a port of Rust's TimerWheel
 *
 * Rust (asupersync 5e60b1c, src/time/wheel.rs) keeps timers in four levels
 * of 256 slots (1 ms, 256 ms, 65.536 s and 4.66 h per slot), a ready list
 * of timers whose tick has come, and a deadline-ordered overflow heap for
 * timers beyond the wheel's range. What fires, and in which order, is a
 * property of that structure rather than of a sort:
 *
 *   - a timer lands in a slot by its deadline's tick, not its exact
 *     deadline, so timers in the same 1 ms tick fire in the order they
 *     reached the slot (registration order, or cascade order for timers
 *     that came down from a coarser level);
 *   - a timer whose tick has already come goes straight to the ready list,
 *     behind the timers already there;
 *   - collecting walks the ready list in order and wakes each due entry.
 *
 * The arena is fixed (no allocation); slots and ready/overflow storage are
 * intrusive lists and a heap over arena indices. The functions below follow
 * the Rust functions of the same name, cited at each one.
 *
 * Differences that cannot change what fires or in which order:
 *   - a cancelled timer leaves its slot at once (Rust leaves it until the
 *     slot is visited, then discards it);
 *   - coalescing (CoalescingConfig) is off by default in Rust and not
 *     modelled here;
 *   - the overflow heap breaks equal deadlines by registration sequence,
 *     which orders them as Rust's generation does.
 *
 * SPDX-License-Identifier: MIT
 */

#include <asx/asx_config.h>
#define ASX_INTERNAL_TRACE_FAMILY_ACCESS 1
#include <asx/runtime/trace.h>
#undef ASX_INTERNAL_TRACE_FAMILY_ACCESS
#define ASX_INTERNAL_TIMER_WHEEL_ACCESS 1
#include <asx/time/timer_wheel.h>
#include <string.h>

/* -------------------------------------------------------------------
 * Geometry (wheel.rs:46-57)
 * ------------------------------------------------------------------- */

#define TW_LEVELS 4u
#define TW_SLOTS 256u
#define TW_BITMAP_WORDS (TW_SLOTS / 64u)
#define TW_TICK_NS ((uint64_t)1000000u) /* level 0 resolution: 1 ms */
#define TW_NIL 0xFFFFu

/* Arena indices are 16-bit. */
#if (ASX_MAX_TIMERS) > 65534
#error "ASX_MAX_TIMERS must be at most 65534"
#endif
#if (2 * (ASX_MAX_TASKS)) > 65534
#error "twice ASX_MAX_TASKS must be at most 65534 (the runtime wheel holds two timers per task)"
#endif

#define TW_APP_CAPACITY ((uint32_t)(ASX_MAX_TIMERS))
#define TW_RUNTIME_CAPACITY (2u * (uint32_t)(ASX_MAX_TASKS))

/* Where a timer is stored. */
#define TW_FREE 0u
#define TW_READY 1u
#define TW_BUCKET 2u
#define TW_OVERFLOW 3u

/* -------------------------------------------------------------------
 * Wheel state
 * ------------------------------------------------------------------- */

typedef struct {
    /* When this timer fires (clamped to the maximum duration). */
    asx_time deadline;
    /* Opaque callback data. */
    void *waker_data;
    /* Registration order: the overflow heap's tie-break. */
    uint64_t seq;
    /* For stale-handle detection. */
    uint32_t generation;
    /* List links; `next` is also the free-list link. */
    uint16_t prev;
    uint16_t next;
    /* Position in the overflow heap. */
    uint16_t heap_pos;
    /* TW_FREE, TW_READY, TW_BUCKET or TW_OVERFLOW; for TW_BUCKET, the
     * wheel level and the slot within it. */
    uint8_t where;
    uint8_t level;
    uint8_t slot;
} tw_entry;

typedef struct {
    uint16_t head;
    uint16_t tail;
} tw_list;

struct asx_timer_wheel {
    tw_entry *entries;
    uint16_t *overflow; /* binary min-heap of entry indices by (deadline, seq) */
    uint32_t capacity;
    uint32_t slot_count; /* arena high-water mark */
    uint32_t free_head;  /* LIFO list of released slots, as Rust's slab */
    uint32_t active_count;
    uint32_t overflow_len;
    uint64_t next_seq;
    uint64_t current_tick; /* current time in 1 ms ticks */
    uint32_t cursor[TW_LEVELS];
    uint64_t occupied[TW_LEVELS][TW_BITMAP_WORDS];
    tw_list bucket[TW_LEVELS][TW_SLOTS];
    tw_list ready;
    uint64_t max_timer_duration_ns; /* longer registrations are clamped (wheel.rs:98-104) */
    uint64_t max_wheel_duration_ns; /* longer ones wait in the overflow heap */
    int traced;                     /* emit TIMER_SET / TIMER_FIRE / TIMER_CANCEL */
};

/* Application-facing wheel (asx_timer_wheel_global) and the scheduler's own
 * (asx_timer_wheel_runtime): one implementation, two instances. The runtime
 * never collects the application wheel, and the scheduler's timers must not
 * be visible to application code. */
static tw_entry g_app_entries[ASX_MAX_TIMERS];
static uint16_t g_app_overflow[ASX_MAX_TIMERS];
static asx_timer_wheel g_app_wheel;
static int g_app_ready = 0;

static tw_entry g_runtime_entries[2u * (ASX_MAX_TASKS)];
static uint16_t g_runtime_overflow[2u * (ASX_MAX_TASKS)];
static asx_timer_wheel g_runtime_wheel;
static int g_runtime_ready = 0;

static uint32_t tw_next_generation(uint32_t current) {
    current++;
    if (current == 0u) { current = 1u; }
    return current;
}

static uint64_t tw_trace_entity_id(const asx_timer_handle *handle) {
    return ((uint64_t)handle->slot << 32) | (uint64_t)handle->generation;
}

/* Resolution of a level: 1 ms * 256^level (wheel.rs:49-55). */
static uint64_t tw_level_res_ns(uint32_t level) { return TW_TICK_NS << (8u * level); }

/* Range of a level: 256 slots of its resolution. */
static uint64_t tw_level_range_ns(uint32_t level) { return tw_level_res_ns(level) << 8; }

/* Wheel time, aligned to the resolution (wheel.rs:554-556). */
static uint64_t tw_current_time(const asx_timer_wheel *w) {
    if (w->current_tick > UINT64_MAX / TW_TICK_NS) return UINT64_MAX;
    return w->current_tick * TW_TICK_NS;
}

/* Deadlines farther than this wait in the overflow heap, clamped to the
 * physical range (wheel.rs:1091-1097). */
static uint64_t tw_max_range_ns(const asx_timer_wheel *w) {
    uint64_t physical = tw_level_range_ns(TW_LEVELS - 1u);
    return w->max_wheel_duration_ns < physical ? w->max_wheel_duration_ns : physical;
}

static uint64_t tw_sat_sub(uint64_t a, uint64_t b) { return a > b ? a - b : 0u; }

static uint64_t tw_sat_add(uint64_t a, uint64_t b) {
    return a > UINT64_MAX - b ? UINT64_MAX : a + b;
}

/* Index of the lowest set bit of x (x != 0). */
static uint32_t tw_ctz64(uint64_t x) {
    uint32_t n = 0u;
    if ((x & 0xFFFFFFFFu) == 0u) {
        n += 32u;
        x >>= 32;
    }
    if ((x & 0xFFFFu) == 0u) {
        n += 16u;
        x >>= 16;
    }
    if ((x & 0xFFu) == 0u) {
        n += 8u;
        x >>= 8;
    }
    if ((x & 0xFu) == 0u) {
        n += 4u;
        x >>= 4;
    }
    if ((x & 0x3u) == 0u) {
        n += 2u;
        x >>= 2;
    }
    if ((x & 0x1u) == 0u) { n += 1u; }
    return n;
}

/* -------------------------------------------------------------------
 * Lists: ready list, slot buckets
 * ------------------------------------------------------------------- */

static void tw_list_clear(tw_list *l) {
    l->head = (uint16_t)TW_NIL;
    l->tail = (uint16_t)TW_NIL;
}

static void tw_list_push(asx_timer_wheel *w, tw_list *l, uint32_t i) {
    tw_entry *e = &w->entries[i];
    e->next = (uint16_t)TW_NIL;
    e->prev = l->tail;
    if ((uint32_t)l->tail != TW_NIL) {
        w->entries[l->tail].next = (uint16_t)i;
    } else {
        l->head = (uint16_t)i;
    }
    l->tail = (uint16_t)i;
}

static void tw_list_unlink(asx_timer_wheel *w, tw_list *l, uint32_t i) {
    tw_entry *e = &w->entries[i];
    if ((uint32_t)e->prev != TW_NIL) {
        w->entries[e->prev].next = e->next;
    } else {
        l->head = e->next;
    }
    if ((uint32_t)e->next != TW_NIL) {
        w->entries[e->next].prev = e->prev;
    } else {
        l->tail = e->prev;
    }
    e->prev = (uint16_t)TW_NIL;
    e->next = (uint16_t)TW_NIL;
}

static int tw_is_occupied(const asx_timer_wheel *w, uint32_t level, uint32_t slot) {
    return (w->occupied[level][slot / 64u] & ((uint64_t)1u << (slot % 64u))) != 0u;
}

static void tw_set_occupied(asx_timer_wheel *w, uint32_t level, uint32_t slot) {
    w->occupied[level][slot / 64u] |= (uint64_t)1u << (slot % 64u);
}

static void tw_clear_occupied(asx_timer_wheel *w, uint32_t level, uint32_t slot) {
    w->occupied[level][slot / 64u] &= ~((uint64_t)1u << (slot % 64u));
}

static void tw_ready_push(asx_timer_wheel *w, uint32_t i) {
    w->entries[i].where = TW_READY;
    tw_list_push(w, &w->ready, i);
}

static void tw_bucket_push(asx_timer_wheel *w, uint32_t level, uint32_t slot, uint32_t i) {
    tw_entry *e = &w->entries[i];
    e->where = TW_BUCKET;
    e->level = (uint8_t)level;
    e->slot = (uint8_t)slot;
    tw_list_push(w, &w->bucket[level][slot], i);
    tw_set_occupied(w, level, slot);
}

/* Detach a slot's list (`std::mem::take`), leaving the slot empty. */
static tw_list tw_bucket_take(asx_timer_wheel *w, uint32_t level, uint32_t slot) {
    tw_list taken = w->bucket[level][slot];
    tw_list_clear(&w->bucket[level][slot]);
    tw_clear_occupied(w, level, slot);
    return taken;
}

/* -------------------------------------------------------------------
 * Overflow heap, ordered by (deadline, registration sequence)
 * (wheel.rs:276-294)
 * ------------------------------------------------------------------- */

static int tw_ovf_less(const asx_timer_wheel *w, uint32_t a, uint32_t b) {
    const tw_entry *ea = &w->entries[a];
    const tw_entry *eb = &w->entries[b];
    if (ea->deadline != eb->deadline) return ea->deadline < eb->deadline;
    return ea->seq < eb->seq;
}

static void tw_ovf_place(asx_timer_wheel *w, uint32_t pos, uint32_t i) {
    w->overflow[pos] = (uint16_t)i;
    w->entries[i].heap_pos = (uint16_t)pos;
}

static void tw_ovf_sift_up(asx_timer_wheel *w, uint32_t pos) {
    uint32_t i = w->overflow[pos];
    while (pos > 0u) {
        uint32_t parent = (pos - 1u) / 2u;
        ASX_CHECKPOINT_WAIVER("bounded: heap depth is log2(capacity)");
        if (!tw_ovf_less(w, i, w->overflow[parent])) break;
        tw_ovf_place(w, pos, w->overflow[parent]);
        pos = parent;
    }
    tw_ovf_place(w, pos, i);
}

static void tw_ovf_sift_down(asx_timer_wheel *w, uint32_t pos) {
    uint32_t i = w->overflow[pos];
    for (;;) {
        uint32_t left = 2u * pos + 1u;
        uint32_t best;
        ASX_CHECKPOINT_WAIVER("bounded: heap depth is log2(capacity)");
        if (left >= w->overflow_len) break;
        best = left;
        if (left + 1u < w->overflow_len &&
            tw_ovf_less(w, w->overflow[left + 1u], w->overflow[left])) {
            best = left + 1u;
        }
        if (!tw_ovf_less(w, w->overflow[best], i)) break;
        tw_ovf_place(w, pos, w->overflow[best]);
        pos = best;
    }
    tw_ovf_place(w, pos, i);
}

static void tw_ovf_push(asx_timer_wheel *w, uint32_t i) {
    w->entries[i].where = TW_OVERFLOW;
    w->entries[i].prev = (uint16_t)TW_NIL;
    w->entries[i].next = (uint16_t)TW_NIL;
    tw_ovf_place(w, w->overflow_len, i);
    w->overflow_len++;
    tw_ovf_sift_up(w, w->overflow_len - 1u);
}

static void tw_ovf_remove_at(asx_timer_wheel *w, uint32_t pos) {
    w->overflow_len--;
    if (pos >= w->overflow_len) return; /* removed the last entry */
    tw_ovf_place(w, pos, w->overflow[w->overflow_len]);
    if (pos > 0u && tw_ovf_less(w, w->overflow[pos], w->overflow[(pos - 1u) / 2u])) {
        tw_ovf_sift_up(w, pos);
    } else {
        tw_ovf_sift_down(w, pos);
    }
}

/* -------------------------------------------------------------------
 * Placement (wheel.rs:710-755)
 * ------------------------------------------------------------------- */

static void tw_insert_entry_at(asx_timer_wheel *w, uint32_t i, uint64_t current) {
    const tw_entry *e = &w->entries[i];
    uint64_t delta;
    uint32_t level;

    if (e->deadline <= current) {
        tw_ready_push(w, i);
        return;
    }
    delta = e->deadline - current;

    /* Beyond the wheel's configured range: overflow. */
    if (delta >= tw_max_range_ns(w)) {
        tw_ovf_push(w, i);
        return;
    }

    /* The finest level whose range holds the delta. */
    for (level = 0u; level < TW_LEVELS; level++) {
        ASX_CHECKPOINT_WAIVER("bounded: TW_LEVELS iterations");
        if (delta < tw_level_range_ns(level)) {
            uint64_t res = tw_level_res_ns(level);
            uint64_t tick = e->deadline / res;
            /* Level 0: a deadline within the current millisecond is ready,
             * its slot has already been visited. */
            if (level == 0u && tick <= current / res) {
                tw_ready_push(w, i);
                return;
            }
            tw_bucket_push(w, level, (uint32_t)(tick % TW_SLOTS), i);
            return;
        }
    }
    tw_ovf_push(w, i);
}

static void tw_insert_entry(asx_timer_wheel *w, uint32_t i) {
    tw_insert_entry_at(w, i, tw_current_time(w));
}

/* -------------------------------------------------------------------
 * Advancing (wheel.rs:756-967)
 * ------------------------------------------------------------------- */

static void tw_realign_cursors(asx_timer_wheel *w) {
    uint64_t now_ns = tw_current_time(w);
    uint32_t level;
    for (level = 0u; level < TW_LEVELS; level++) {
        ASX_CHECKPOINT_WAIVER("bounded: TW_LEVELS iterations");
        w->cursor[level] = (uint32_t)((now_ns / tw_level_res_ns(level)) % TW_SLOTS);
    }
}

/* Distance in slots from the level's cursor to its next occupied slot
 * (1..256), 0 when the level is empty (WheelLevel::next_occupied_distance,
 * wheel.rs:347-385). */
static uint32_t tw_next_occupied_distance(const asx_timer_wheel *w, uint32_t level) {
    const uint64_t *occ = w->occupied[level];
    uint32_t cursor = w->cursor[level];
    uint32_t pos;

    if ((occ[0] | occ[1] | occ[2] | occ[3]) == 0u) return 0u;

    pos = cursor + 1u;
    while (pos < TW_SLOTS) {
        uint32_t word = pos / 64u;
        uint32_t bit = pos % 64u;
        uint64_t masked = occ[word] >> bit;
        ASX_CHECKPOINT_WAIVER("bounded: at most one pass over the bitmap words");
        if (masked != 0u) {
            uint32_t found = pos + tw_ctz64(masked);
            if (found < TW_SLOTS) return found - cursor;
        }
        pos = (word + 1u) * 64u;
    }

    pos = 0u;
    while (pos <= cursor) {
        uint32_t word = pos / 64u;
        uint32_t bit = pos % 64u;
        uint64_t masked = occ[word] >> bit;
        ASX_CHECKPOINT_WAIVER("bounded: at most one pass over the bitmap words");
        if (masked != 0u) {
            uint32_t found = pos + tw_ctz64(masked);
            if (found <= cursor) return TW_SLOTS - cursor + found;
        }
        pos = (word + 1u) * 64u;
    }
    return 0u;
}

/* The first tick before `limit` at which something happens: a slot is
 * visited or an overflow timer comes into range (wheel.rs:805-840). */
static uint64_t tw_next_skip_tick(const asx_timer_wheel *w, uint64_t limit) {
    uint64_t next_tick = limit;
    uint64_t r_i = 1u;
    uint32_t level;

    for (level = 0u; level < TW_LEVELS; level++) {
        uint32_t dist = tw_next_occupied_distance(w, level);
        ASX_CHECKPOINT_WAIVER("bounded: TW_LEVELS iterations");
        if (dist != 0u) {
            uint64_t current_base = w->current_tick - (w->current_tick % r_i);
            uint64_t hit_tick = current_base + (uint64_t)dist * r_i;
            if (hit_tick <= w->current_tick) hit_tick += (uint64_t)TW_SLOTS * r_i;
            if (hit_tick < next_tick) next_tick = hit_tick;
        }
        r_i *= TW_SLOTS;
    }

    if (w->overflow_len > 0u) {
        const tw_entry *e = &w->entries[w->overflow[0]];
        uint64_t min_enter_ns = tw_sat_sub(e->deadline, tw_max_range_ns(w));
        uint64_t min_enter_tick = min_enter_ns / TW_TICK_NS;
        if (min_enter_tick < next_tick) {
            if (min_enter_tick > w->current_tick) {
                next_tick = min_enter_tick;
            } else {
                return w->current_tick;
            }
        }
    }
    return next_tick;
}

static int tw_can_fast_advance_one_tick(const asx_timer_wheel *w, uint64_t target_tick) {
    uint32_t next_cursor = (w->cursor[0] + 1u) % TW_SLOTS;
    if (next_cursor == 0u || tw_is_occupied(w, 0u, next_cursor)) return 0;

    if (w->overflow_len > 0u) {
        const tw_entry *e = &w->entries[w->overflow[0]];
        uint64_t min_enter_ns = tw_sat_sub(e->deadline, tw_max_range_ns(w));
        if (min_enter_ns / TW_TICK_NS <= target_tick) return 0;
    }
    return 1;
}

/* A slot of a coarser level comes due: each of its timers is placed again,
 * in slot order, so it lands in a finer level (or the ready list). When the
 * level's cursor wraps, the next level cascades too (wheel.rs:865-888). */
static void tw_cascade(asx_timer_wheel *w, uint32_t level) {
    while (level < TW_LEVELS) {
        uint32_t cursor;
        tw_list taken;
        uint32_t i;
        uint32_t nxt;

        ASX_CHECKPOINT_WAIVER("bounded: TW_LEVELS iterations");
        w->cursor[level] = (w->cursor[level] + 1u) % TW_SLOTS;
        cursor = w->cursor[level];
        taken = tw_bucket_take(w, level, cursor);
        for (i = taken.head; i != TW_NIL; i = nxt) {
            ASX_CHECKPOINT_WAIVER("bounded: a slot holds at most the arena's timers");
            nxt = w->entries[i].next;
            tw_insert_entry(w, i);
        }
        if (cursor != 0u) break;
        level++;
    }
}

/* A level-0 slot comes due: its timers are due or in the current
 * millisecond; either way they wait in the ready list (wheel.rs:889-902). */
static void tw_collect_bucket(asx_timer_wheel *w, tw_list taken) {
    uint64_t now = tw_current_time(w);
    uint32_t i;
    uint32_t nxt;
    for (i = taken.head; i != TW_NIL; i = nxt) {
        ASX_CHECKPOINT_WAIVER("bounded: a slot holds at most the arena's timers");
        nxt = w->entries[i].next;
        if (w->entries[i].deadline <= now) {
            tw_ready_push(w, i);
        } else {
            tw_insert_entry(w, i);
        }
    }
}

static void tw_tick_level0(asx_timer_wheel *w) {
    uint32_t cursor;
    tw_list taken;

    w->cursor[0] = (w->cursor[0] + 1u) % TW_SLOTS;
    cursor = w->cursor[0];
    taken = tw_bucket_take(w, 0u, cursor);
    tw_collect_bucket(w, taken);
    if (cursor == 0u) tw_cascade(w, 1u);
}

/* Overflow timers that have come into range join the wheel (wheel.rs:903-920). */
static void tw_refill_overflow(asx_timer_wheel *w) {
    uint64_t current = tw_current_time(w);
    uint64_t max_range = tw_max_range_ns(w);

    while (w->overflow_len > 0u) {
        uint32_t i = w->overflow[0];
        uint64_t delta = tw_sat_sub(w->entries[i].deadline, current);
        ASX_CHECKPOINT_WAIVER("bounded: each iteration removes one overflow entry");
        /* A due entry always leaves overflow, even with a zero range. */
        if (delta < max_range || delta == 0u) {
            tw_ovf_remove_at(w, 0u);
            tw_insert_entry(w, i);
        } else {
            break;
        }
    }
}

static void tw_advance_to(asx_timer_wheel *w, uint64_t target_tick) {
    if (w->active_count == 0u) {
        w->current_tick = target_tick;
        tw_realign_cursors(w);
        return;
    }

    while (w->current_tick < target_tick) {
        uint64_t next_tick;
        ASX_CHECKPOINT_WAIVER("bounded: each iteration advances at least one tick toward target");

        if (target_tick == w->current_tick + 1u && tw_can_fast_advance_one_tick(w, target_tick)) {
            w->current_tick = target_tick;
            w->cursor[0] = (w->cursor[0] + 1u) % TW_SLOTS;
            continue;
        }

        /* Skip ticks on which nothing happens. */
        next_tick = tw_next_skip_tick(w, target_tick);
        if (next_tick > w->current_tick + 1u) {
            w->current_tick = next_tick - 1u;
            tw_realign_cursors(w);
        }

        w->current_tick++;
        tw_tick_level0(w);
        tw_refill_overflow(w);
    }
}

/* wheel.rs:502-507 */
static void tw_synchronize(asx_timer_wheel *w, asx_time now) {
    uint64_t target_tick = now / TW_TICK_NS;
    if (target_tick > w->current_tick) tw_advance_to(w, target_tick);
}

/* -------------------------------------------------------------------
 * Arena
 * ------------------------------------------------------------------- */

static void tw_release(asx_timer_wheel *w, uint32_t i) {
    tw_entry *e = &w->entries[i];
    e->where = TW_FREE;
    e->waker_data = NULL;
    e->prev = (uint16_t)TW_NIL;
    e->next = (uint16_t)w->free_head;
    w->free_head = i;
}

/* Take a free slot: the most recently released, else a new one (Rust's
 * slab hands the freed id back to the next insert). */
static int tw_alloc(asx_timer_wheel *w, uint32_t *out_idx) {
    if (w->free_head != TW_NIL) {
        uint32_t i = w->free_head;
        w->free_head = w->entries[i].next;
        *out_idx = i;
        return 1;
    }
    if (w->slot_count >= w->capacity) return 0;
    *out_idx = w->slot_count++;
    return 1;
}

static void tw_unlink(asx_timer_wheel *w, uint32_t i) {
    tw_entry *e = &w->entries[i];
    if (e->where == TW_READY) {
        tw_list_unlink(w, &w->ready, i);
    } else if (e->where == TW_BUCKET) {
        tw_list *l = &w->bucket[e->level][e->slot];
        tw_list_unlink(w, l, i);
        if ((uint32_t)l->head == TW_NIL) tw_clear_occupied(w, e->level, e->slot);
    } else if (e->where == TW_OVERFLOW) {
        tw_ovf_remove_at(w, e->heap_pos);
    }
}

/* -------------------------------------------------------------------
 * Setup / reset
 * ------------------------------------------------------------------- */

static void tw_reset_state(asx_timer_wheel *w) {
    uint32_t level;
    uint32_t slot;
    w->slot_count = 0u;
    w->free_head = TW_NIL;
    w->active_count = 0u;
    w->overflow_len = 0u;
    w->next_seq = 0u;
    w->current_tick = 0u;
    for (level = 0u; level < TW_LEVELS; level++) {
        ASX_CHECKPOINT_WAIVER("bounded: TW_LEVELS * TW_SLOTS iterations");
        w->cursor[level] = 0u;
        memset(w->occupied[level], 0, sizeof(w->occupied[level]));
        for (slot = 0u; slot < TW_SLOTS; slot++) {
            ASX_CHECKPOINT_WAIVER("bounded: TW_SLOTS constant");
            tw_list_clear(&w->bucket[level][slot]);
        }
    }
    tw_list_clear(&w->ready);
    w->max_timer_duration_ns = ASX_TIMER_MAX_DURATION_NS;
    w->max_wheel_duration_ns = ASX_TIMER_MAX_WHEEL_DURATION_NS;
}

static void tw_setup(asx_timer_wheel *w, tw_entry *entries, uint16_t *overflow, uint32_t capacity,
                     int traced) {
    memset(w, 0, sizeof(*w));
    w->entries = entries;
    w->overflow = overflow;
    w->capacity = capacity;
    w->traced = traced;
    memset(entries, 0, (size_t)capacity * sizeof(tw_entry));
    tw_reset_state(w);
}

asx_timer_wheel *asx_timer_wheel_global(void) {
    if (!g_app_ready) {
        tw_setup(&g_app_wheel, g_app_entries, g_app_overflow, TW_APP_CAPACITY, 1);
        g_app_ready = 1;
    }
    return &g_app_wheel;
}

asx_timer_wheel *asx_timer_wheel_runtime(void) {
    if (!g_runtime_ready) {
        tw_setup(&g_runtime_wheel, g_runtime_entries, g_runtime_overflow, TW_RUNTIME_CAPACITY, 0);
        g_runtime_ready = 1;
    }
    return &g_runtime_wheel;
}

void asx_timer_wheel_init(asx_timer_wheel *wheel) {
    if (wheel == NULL || wheel->entries == NULL) return;
    memset(wheel->entries, 0, (size_t)wheel->capacity * sizeof(tw_entry));
    tw_reset_state(wheel);
}

void asx_timer_wheel_reset(asx_timer_wheel *wheel) {
    uint32_t i;

    if (wheel == NULL || wheel->entries == NULL) return;

    /* Preserve stale-handle safety across reset epochs. */
    for (i = 0u; i < wheel->slot_count; i++) {
        tw_entry *e = &wheel->entries[i];
        ASX_CHECKPOINT_WAIVER("bounded: slot_count <= capacity");
        e->generation = tw_next_generation(e->generation);
        e->where = TW_FREE;
        e->waker_data = NULL;
        e->deadline = 0u;
        e->seq = 0u;
        e->prev = (uint16_t)TW_NIL;
        e->next = (uint16_t)TW_NIL;
    }
    tw_reset_state(wheel);
}

/* -------------------------------------------------------------------
 * Registration (wheel.rs:515-557)
 *
 * A deadline farther ahead than the maximum timer duration is clamped to
 * that duration past the wheel's time, as Rust's TimerWheel::register does:
 * the timer fires early and its owner re-registers if the true deadline has
 * not come.
 * ------------------------------------------------------------------- */

static void tw_arm(asx_timer_wheel *w, uint32_t idx, asx_time deadline, void *waker_data,
                   asx_timer_handle *out_handle) {
    tw_entry *e = &w->entries[idx];
    uint64_t current = tw_current_time(w);

    e->deadline = deadline;
    e->waker_data = waker_data;
    e->seq = w->next_seq++;
    e->generation = tw_next_generation(e->generation);
    w->active_count++;

    out_handle->slot = idx;
    out_handle->generation = e->generation;
    tw_insert_entry_at(w, idx, current);

    if (w->traced) {
        asx_trace_emit(ASX_TRACE_TIMER_SET, tw_trace_entity_id(out_handle), deadline);
    }
}

static asx_time tw_clamp_deadline(const asx_timer_wheel *w, asx_time deadline) {
    uint64_t current = tw_current_time(w);
    if (deadline > current && deadline - current > w->max_timer_duration_ns) {
        return tw_sat_add(current, w->max_timer_duration_ns);
    }
    return deadline;
}

asx_status asx_timer_register(asx_timer_wheel *wheel, asx_time deadline, void *waker_data,
                              asx_timer_handle *out_handle) {
    uint32_t idx;

    if (wheel == NULL || out_handle == NULL) return ASX_E_INVALID_ARGUMENT;
    if (!tw_alloc(wheel, &idx)) return ASX_E_RESOURCE_EXHAUSTED;

    tw_arm(wheel, idx, tw_clamp_deadline(wheel, deadline), waker_data, out_handle);
    return ASX_OK;
}

/* -------------------------------------------------------------------
 * Cancellation (wheel.rs:577-594)
 * ------------------------------------------------------------------- */

static int tw_handle_is_live(const asx_timer_wheel *w, const asx_timer_handle *handle) {
    const tw_entry *e;
    if (handle->slot >= w->slot_count) return 0;
    e = &w->entries[handle->slot];
    return e->where != TW_FREE && e->generation == handle->generation;
}

int asx_timer_cancel(asx_timer_wheel *wheel, const asx_timer_handle *handle) {
    const tw_entry *e;

    if (wheel == NULL || handle == NULL) return 0;
    if (!tw_handle_is_live(wheel, handle)) return 0;

    e = &wheel->entries[handle->slot];
    if (wheel->traced) {
        asx_trace_emit(ASX_TRACE_TIMER_CANCEL, tw_trace_entity_id(handle), e->deadline);
    }
    tw_unlink(wheel, handle->slot);
    tw_release(wheel, handle->slot);
    if (wheel->active_count > 0u) wheel->active_count--;
    return 1;
}

/* -------------------------------------------------------------------
 * Collection (wheel.rs:699-705, drain_ready 921-)
 *
 * Wakes due timers in ready-list order. A timer still ahead of `now`
 * (in the current millisecond) stays, in order; so does a due one beyond
 * the caller's capacity.
 * ------------------------------------------------------------------- */

static uint32_t tw_drain_ready(asx_timer_wheel *w, asx_time now, void **out_wakers,
                               uint32_t max_wakers) {
    tw_list taken;
    uint32_t count = 0u;
    uint32_t i;
    uint32_t nxt;

    if ((uint32_t)w->ready.head == TW_NIL) return 0u;

    taken = w->ready;
    tw_list_clear(&w->ready);

    for (i = taken.head; i != TW_NIL; i = nxt) {
        tw_entry *e = &w->entries[i];
        ASX_CHECKPOINT_WAIVER("bounded: the ready list holds at most the arena's timers");
        nxt = e->next;
        if (e->deadline > now) {
            tw_insert_entry(w, i);
        } else if (count >= max_wakers) {
            tw_ready_push(w, i);
        } else {
            asx_timer_handle fired;
            fired.slot = i;
            fired.generation = e->generation;
            out_wakers[count++] = e->waker_data;
            if (w->traced) {
                asx_trace_emit(ASX_TRACE_TIMER_FIRE, tw_trace_entity_id(&fired), e->deadline);
            }
            tw_release(w, i);
            if (w->active_count > 0u) w->active_count--;
        }
    }
    return count;
}

uint32_t asx_timer_collect_expired(asx_timer_wheel *wheel, asx_time now, void **out_wakers,
                                   uint32_t max_wakers) {
    if (wheel == NULL) return 0u;

    tw_synchronize(wheel, now);
    if (out_wakers == NULL || max_wakers == 0u) return 0u;

    return tw_drain_ready(wheel, now, out_wakers, max_wakers);
}

/* -------------------------------------------------------------------
 * Update (cancel + re-register), as the Rust driver's update
 * (time/driver.rs:522-540)
 * ------------------------------------------------------------------- */

asx_status asx_timer_update(asx_timer_wheel *wheel, const asx_timer_handle *old_handle,
                            asx_time new_deadline, void *waker_data, asx_timer_handle *out_handle) {
    uint32_t idx;

    if (wheel == NULL || out_handle == NULL) return ASX_E_INVALID_ARGUMENT;
    if (old_handle == NULL) return asx_timer_register(wheel, new_deadline, waker_data, out_handle);

    /* A timer that fired, was cancelled or whose slot was reused is not
     * re-armed. */
    if (!tw_handle_is_live(wheel, old_handle)) return ASX_E_STALE_HANDLE;

    /* Cancel, then register into the freed slot under a new generation
     * (Rust's slab reuses the freed id), so a full wheel cannot refuse. The
     * new registration goes behind the timers already registered. */
    (void)asx_timer_cancel(wheel, old_handle);
    if (!tw_alloc(wheel, &idx)) return ASX_E_RESOURCE_EXHAUSTED;
    tw_arm(wheel, idx, tw_clamp_deadline(wheel, new_deadline), waker_data, out_handle);
    return ASX_OK;
}

/* -------------------------------------------------------------------
 * Queries
 * ------------------------------------------------------------------- */

uint32_t asx_timer_active_count(const asx_timer_wheel *wheel) {
    if (wheel == NULL) return 0u;
    return wheel->active_count;
}

void asx_timer_set_max_duration(asx_timer_wheel *wheel, uint64_t max_duration_ns) {
    if (wheel == NULL) return;
    wheel->max_timer_duration_ns = max_duration_ns;
}

void asx_timer_advance(asx_timer_wheel *wheel, asx_time now) {
    if (wheel == NULL) return;
    tw_synchronize(wheel, now);
}

/* The earliest live deadline, clamped to the wheel's time when a timer is
 * already due (TimerWheel::raw_next_deadline, wheel.rs:634-698). A level's
 * first occupied slot after its cursor holds that level's earliest
 * deadline; the minimum is taken across all levels because levels are not
 * deadline-ordered near cursor wraps. */
int asx_timer_next_deadline(const asx_timer_wheel *wheel, asx_time *out_deadline) {
    uint64_t current;
    uint64_t min_deadline = 0u;
    int have = 0;
    uint32_t level;
    uint32_t i;

    if (wheel == NULL || out_deadline == NULL) return 0;
    current = tw_current_time(wheel);

    for (i = wheel->ready.head; i != TW_NIL; i = wheel->entries[i].next) {
        ASX_CHECKPOINT_WAIVER("bounded: the ready list holds at most the arena's timers");
        if (wheel->entries[i].deadline <= current) {
            *out_deadline = current;
            return 1;
        }
        if (!have || wheel->entries[i].deadline < min_deadline) {
            min_deadline = wheel->entries[i].deadline;
            have = 1;
        }
    }
    if (have) {
        *out_deadline = min_deadline;
        return 1;
    }

    for (level = 0u; level < TW_LEVELS; level++) {
        uint32_t dist = tw_next_occupied_distance(wheel, level);
        ASX_CHECKPOINT_WAIVER("bounded: TW_LEVELS iterations");
        if (dist != 0u) {
            uint32_t slot = (wheel->cursor[level] + dist) % TW_SLOTS;
            for (i = wheel->bucket[level][slot].head; i != TW_NIL; i = wheel->entries[i].next) {
                ASX_CHECKPOINT_WAIVER("bounded: a slot holds at most the arena's timers");
                if (!have || wheel->entries[i].deadline < min_deadline) {
                    min_deadline = wheel->entries[i].deadline;
                    have = 1;
                }
            }
        }
    }

    if (wheel->overflow_len > 0u) {
        uint64_t d = wheel->entries[wheel->overflow[0]].deadline;
        if (!have || d < min_deadline) {
            min_deadline = d;
            have = 1;
        }
    }

    if (have) *out_deadline = min_deadline;
    return have;
}
