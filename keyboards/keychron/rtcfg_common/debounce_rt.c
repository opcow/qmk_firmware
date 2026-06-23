/* Runtime-selectable debounce.
 *
 * QMK selects the debounce algorithm at compile time (DEBOUNCE_TYPE) and compiles
 * exactly one file from quantum/debounce. To switch at runtime we compile several
 * algorithms here, each with uniquely-prefixed symbols, behind a dispatcher that
 * QMK sees as the single custom debounce()/debounce_init()/debounce_free()
 * (rules.mk: DEBOUNCE_TYPE = custom).
 *
 * Each algorithm is a copy of the matching stock file with the compile-time
 * DEBOUNCE constant replaced by rtcfg_debounce_time() (rtcfg_common.h). The active
 * method comes from rtcfg_debounce_method():
 *   0 none, 1 sym_defer_g (default), 2 sym_eager_pk, 3 asym_eager_defer_pk
 */
#include "debounce.h"
#include "timer.h"
#include "rtcfg_common.h"
#include <stdlib.h>
#include <string.h>

#define ROW_SHIFTER      ((matrix_row_t)1)
#define DEBOUNCE_ELAPSED 0

// ---------- method 0: none ----------
static void none_init(uint8_t num_rows) {}
static void none_free(void) {}
static bool none_run(matrix_row_t raw[], matrix_row_t cooked[], uint8_t num_rows, bool changed) {
    bool cooked_changed = false;
    if (changed) {
        size_t matrix_size = num_rows * sizeof(matrix_row_t);
        if (memcmp(cooked, raw, matrix_size) != 0) {
            memcpy(cooked, raw, matrix_size);
            cooked_changed = true;
        }
    }
    return cooked_changed;
}

// ---------- method 1: sym_defer_g (symmetric, defer, global) ----------
static bool         sdg_debouncing = false;
static fast_timer_t sdg_time;

static void sdg_init(uint8_t num_rows) { sdg_debouncing = false; }
static void sdg_free(void) {}
static bool sdg_run(matrix_row_t raw[], matrix_row_t cooked[], uint8_t num_rows, bool changed) {
    bool cooked_changed = false;
    if (changed) {
        sdg_debouncing = true;
        sdg_time       = timer_read_fast();
    } else if (sdg_debouncing && timer_elapsed_fast(sdg_time) >= rtcfg_debounce_time()) {
        size_t matrix_size = num_rows * sizeof(matrix_row_t);
        if (memcmp(cooked, raw, matrix_size) != 0) {
            memcpy(cooked, raw, matrix_size);
            cooked_changed = true;
        }
        sdg_debouncing = false;
    }
    return cooked_changed;
}

// ---------- method 2: sym_eager_pk (symmetric, eager, per-key) ----------
typedef uint8_t       sepk_counter_t;
static sepk_counter_t *sepk_counters;
static fast_timer_t    sepk_last_time;
static bool            sepk_counters_need_update;
static bool            sepk_matrix_need_update;
static bool            sepk_cooked_changed;

static void sepk_update_counters(uint8_t num_rows, uint8_t elapsed_time);
static void sepk_transfer(matrix_row_t raw[], matrix_row_t cooked[], uint8_t num_rows);

static void sepk_init(uint8_t num_rows) {
    sepk_counters_need_update = false;
    sepk_matrix_need_update   = false;
    sepk_counters = (sepk_counter_t *)malloc(num_rows * MATRIX_COLS * sizeof(sepk_counter_t));
    for (int i = 0; i < num_rows * MATRIX_COLS; i++) sepk_counters[i] = DEBOUNCE_ELAPSED;
}
static void sepk_free(void) {
    free(sepk_counters);
    sepk_counters = NULL;
}
static bool sepk_run(matrix_row_t raw[], matrix_row_t cooked[], uint8_t num_rows, bool changed) {
    bool updated_last   = false;
    sepk_cooked_changed = false;

    if (sepk_counters_need_update) {
        fast_timer_t now          = timer_read_fast();
        fast_timer_t elapsed_time = TIMER_DIFF_FAST(now, sepk_last_time);
        sepk_last_time            = now;
        updated_last              = true;
        if (elapsed_time > UINT8_MAX) elapsed_time = UINT8_MAX;
        if (elapsed_time > 0) sepk_update_counters(num_rows, elapsed_time);
    }
    if (changed || sepk_matrix_need_update) {
        if (!updated_last) sepk_last_time = timer_read_fast();
        sepk_transfer(raw, cooked, num_rows);
    }
    return sepk_cooked_changed;
}
static void sepk_update_counters(uint8_t num_rows, uint8_t elapsed_time) {
    sepk_counters_need_update = false;
    sepk_matrix_need_update   = false;
    sepk_counter_t *p = sepk_counters;
    for (uint8_t row = 0; row < num_rows; row++) {
        for (uint8_t col = 0; col < MATRIX_COLS; col++) {
            if (*p != DEBOUNCE_ELAPSED) {
                if (*p <= elapsed_time) {
                    *p                      = DEBOUNCE_ELAPSED;
                    sepk_matrix_need_update = true;
                } else {
                    *p -= elapsed_time;
                    sepk_counters_need_update = true;
                }
            }
            p++;
        }
    }
}
static void sepk_transfer(matrix_row_t raw[], matrix_row_t cooked[], uint8_t num_rows) {
    sepk_matrix_need_update = false;
    uint8_t          db = rtcfg_debounce_time();
    sepk_counter_t  *p  = sepk_counters;
    for (uint8_t row = 0; row < num_rows; row++) {
        matrix_row_t delta        = raw[row] ^ cooked[row];
        matrix_row_t existing_row = cooked[row];
        for (uint8_t col = 0; col < MATRIX_COLS; col++) {
            matrix_row_t col_mask = (ROW_SHIFTER << col);
            if (delta & col_mask) {
                if (*p == DEBOUNCE_ELAPSED) {
                    *p                        = db;
                    sepk_counters_need_update = true;
                    existing_row ^= col_mask;
                    sepk_cooked_changed = true;
                }
            }
            p++;
        }
        cooked[row] = existing_row;
    }
}

// ---------- method 3: asym_eager_defer_pk (eager press, defer release, per-key) ----------
typedef struct {
    bool    pressed : 1;
    uint8_t time : 7;
} aedpk_counter_t;
static aedpk_counter_t *aedpk_counters;
static fast_timer_t     aedpk_last_time;
static bool             aedpk_counters_need_update;
static bool             aedpk_matrix_need_update;
static bool             aedpk_cooked_changed;

static void aedpk_update(matrix_row_t raw[], matrix_row_t cooked[], uint8_t num_rows, uint8_t elapsed_time);
static void aedpk_transfer(matrix_row_t raw[], matrix_row_t cooked[], uint8_t num_rows);

static void aedpk_init(uint8_t num_rows) {
    aedpk_counters_need_update = false;
    aedpk_matrix_need_update   = false;
    aedpk_counters = (aedpk_counter_t *)malloc(num_rows * MATRIX_COLS * sizeof(aedpk_counter_t));
    for (int i = 0; i < num_rows * MATRIX_COLS; i++) aedpk_counters[i].time = DEBOUNCE_ELAPSED;
}
static void aedpk_free(void) {
    free(aedpk_counters);
    aedpk_counters = NULL;
}
static bool aedpk_run(matrix_row_t raw[], matrix_row_t cooked[], uint8_t num_rows, bool changed) {
    bool updated_last    = false;
    aedpk_cooked_changed = false;

    if (aedpk_counters_need_update) {
        fast_timer_t now          = timer_read_fast();
        fast_timer_t elapsed_time = TIMER_DIFF_FAST(now, aedpk_last_time);
        aedpk_last_time           = now;
        updated_last              = true;
        if (elapsed_time > UINT8_MAX) elapsed_time = UINT8_MAX;
        if (elapsed_time > 0) aedpk_update(raw, cooked, num_rows, elapsed_time);
    }
    if (changed || aedpk_matrix_need_update) {
        if (!updated_last) aedpk_last_time = timer_read_fast();
        aedpk_transfer(raw, cooked, num_rows);
    }
    return aedpk_cooked_changed;
}
static void aedpk_update(matrix_row_t raw[], matrix_row_t cooked[], uint8_t num_rows, uint8_t elapsed_time) {
    aedpk_counter_t *p = aedpk_counters;
    aedpk_counters_need_update = false;
    aedpk_matrix_need_update   = false;
    for (uint8_t row = 0; row < num_rows; row++) {
        for (uint8_t col = 0; col < MATRIX_COLS; col++) {
            matrix_row_t col_mask = (ROW_SHIFTER << col);
            if (p->time != DEBOUNCE_ELAPSED) {
                if (p->time <= elapsed_time) {
                    p->time = DEBOUNCE_ELAPSED;
                    if (p->pressed) {
                        aedpk_matrix_need_update = true;  // key-down: eager
                    } else {
                        // key-up: defer
                        matrix_row_t cooked_next = (cooked[row] & ~col_mask) | (raw[row] & col_mask);
                        aedpk_cooked_changed |= cooked_next ^ cooked[row];
                        cooked[row] = cooked_next;
                    }
                } else {
                    p->time -= elapsed_time;
                    aedpk_counters_need_update = true;
                }
            }
            p++;
        }
    }
}
static void aedpk_transfer(matrix_row_t raw[], matrix_row_t cooked[], uint8_t num_rows) {
    aedpk_counter_t *p = aedpk_counters;
    aedpk_matrix_need_update = false;
    uint8_t db = rtcfg_debounce_time();
    if (db > 127) db = 127;  // time field is 7 bits
    for (uint8_t row = 0; row < num_rows; row++) {
        matrix_row_t delta = raw[row] ^ cooked[row];
        for (uint8_t col = 0; col < MATRIX_COLS; col++) {
            matrix_row_t col_mask = (ROW_SHIFTER << col);
            if (delta & col_mask) {
                if (p->time == DEBOUNCE_ELAPSED) {
                    p->pressed                 = (raw[row] & col_mask);
                    p->time                    = db;
                    aedpk_counters_need_update = true;
                    if (p->pressed) {  // key-down: eager
                        cooked[row] ^= col_mask;
                        aedpk_cooked_changed = true;
                    }
                }
            } else if (p->time != DEBOUNCE_ELAPSED) {
                if (!p->pressed) p->time = DEBOUNCE_ELAPSED;
            }
            p++;
        }
    }
}

// ---------- dispatcher ----------
typedef struct {
    void (*init)(uint8_t num_rows);
    void (*free_)(void);
    bool (*run)(matrix_row_t raw[], matrix_row_t cooked[], uint8_t num_rows, bool changed);
} db_algo_t;

#define DB_METHOD_COUNT 4
static const db_algo_t DB_ALGOS[DB_METHOD_COUNT] = {
    {none_init,  none_free,  none_run },   // 0 none
    {sdg_init,   sdg_free,   sdg_run  },   // 1 sym_defer_g (default)
    {sepk_init,  sepk_free,  sepk_run },   // 2 sym_eager_pk
    {aedpk_init, aedpk_free, aedpk_run},   // 3 asym_eager_defer_pk
};

static uint8_t db_num_rows = 0;
static uint8_t db_active   = 0xFF;  // no algorithm initialized yet

void debounce_init(uint8_t num_rows) {
    // Real per-method init is deferred to the first debounce() call, which runs
    // after keyboard_post_init_user() has loaded user_config from EEPROM.
    db_num_rows = num_rows;
    db_active   = 0xFF;
}

void debounce_free(void) {
    if (db_active < DB_METHOD_COUNT) DB_ALGOS[db_active].free_();
    db_active = 0xFF;
}

bool debounce(matrix_row_t raw[], matrix_row_t cooked[], uint8_t num_rows, bool changed) {
    // A zero debounce time means "no debounce" regardless of the selected method.
    uint8_t want = (rtcfg_debounce_time() == 0) ? 0 : rtcfg_debounce_method();
    if (want >= DB_METHOD_COUNT) want = 1;  // fall back to default

    if (want != db_active) {
        if (db_active < DB_METHOD_COUNT) DB_ALGOS[db_active].free_();
        DB_ALGOS[want].init(db_num_rows);
        db_active = want;
    }
    return DB_ALGOS[want].run(raw, cooked, num_rows, changed);
}
