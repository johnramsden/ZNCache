#ifndef PROFILING_H
#define PROFILING_H

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <glib.h>
#include <time.h>

#define METRICS_BUFFER_SIZE (1u << 12)  // 4096
#define PROFILING_HEADERS "TIMESTAMP,METRIC,VALUE"

#define SINCE_PROFILER_BEGAN(p, now) (TIME_DIFFERENCE_MILLISEC((p->started_ts), (now)))

enum zn_profiler_type {
    ZN_PROFILER_AVG = 0,
    ZN_PROFILER_SET = 1,
    ZN_PROFILER_OVER_TIME = 2,
};

struct zn_profiler_metrics {
    uint32_t count;
    double value;
    enum zn_profiler_type type;
};

#define PROFILING_METRICS 11 // Keep in sync with enum, zn_profiler_metric_names, and zn_profiler_metric_types
enum zn_profiler_tag {
    ZN_PROFILER_METRIC_GET_LATENCY = 0,
    ZN_PROFILER_METRIC_CACHE_USED_MIB = 1,
    ZN_PROFILER_METRIC_CACHE_HITRATIO = 2,
    ZN_PROFILER_METRIC_READ_LATENCY = 3,
    ZN_PROFILER_METRIC_WRITE_LATENCY = 4,
    ZN_PROFILER_METRIC_CACHE_FREE_ZONES = 5,
    ZN_PROFILER_METRIC_HIT_LATENCY = 6,
    ZN_PROFILER_METRIC_MISS_LATENCY= 7,
    ZN_PROFILER_METRIC_CACHE_THROUGHPUT = 8,
    ZN_PROFILER_METRIC_CACHE_HIT_THROUGHPUT = 9,
    ZN_PROFILER_METRIC_CACHE_MISS_THROUGHPUT = 10,
};

// (in znprofiler.c)
extern char *zn_profiler_metric_names[PROFILING_METRICS];
extern enum zn_profiler_type zn_profiler_metric_types[PROFILING_METRICS];

struct zn_profiler {
    FILE *fp;
    char buffer[METRICS_BUFFER_SIZE];
    struct zn_profiler_metrics metrics[PROFILING_METRICS];
    bool realtime;
    GMutex lock;
    struct timespec started_ts;
};

/**
 * Initialize the profiler
 *
 * @param filename File to output metrics to
 * @return Profiler or NULL on error
 */
struct zn_profiler *
zn_profiler_init(const char *filename);

/**
 * Close and flush profiler
 * @param zp Profiler
 */
void
zn_profiler_close(struct zn_profiler *zp);

/**
 * Write metrics to profiler
 *
* LOCKED BY CALLEE
 */
void
zn_profiler_write(struct zn_profiler *zp, const char *format, ...);

/**
 * Write all metrics out and reset counters
 *
* LOCKED BY CALLEE
 */
void
zn_profiler_write_all_and_reset(struct zn_profiler *zp);

/**
 * @brief Increments the specified metric's total value and usage count.
 *
 * This function adds the provided @p value to the selected metric's
 * cumulative total in the profiler and increments the metric's usage count
 * by one.
 *
 * LOCKED BY CALLEE
 *
 * @param[in,out] zp      Pointer to the profiler structure managing the metrics.
 * @param[in]     metric  Enum identifier of the metric to update.
 * @param[in]     value   The amount to add to the metric's current total.
 */
void
zn_profiler_update_metric(struct zn_profiler *zp, enum zn_profiler_tag metric, double value);

/**
 * @brief Sets a metric value
 *
 * LOCKED BY CALLEE
 *
 * @param[in,out] zp      Pointer to the profiler structure managing the metrics.
 * @param[in]     metric  Enum identifier of the metric to update.
 * @param[in]     value   The amount to set to the metric's current value.
 */
void
zn_profiler_set_metric(struct zn_profiler *zp, enum zn_profiler_tag metric, double value);

/**
* Calls zn_profiler_update_metric if zp not NULL
*/
#define ZN_PROFILER_UPDATE(zp, metric, value)    \
    do {                                    \
        if ((zp) != NULL) {                \
            zn_profiler_update_metric((zp), (metric), (value));     \
        }                                   \
    } while (0)

/**
* Calls zn_profiler_set_metric if zp not NULL
*/
#define ZN_PROFILER_SET(zp, metric, value)    \
    do {                                    \
        if ((zp) != NULL) {                \
            zn_profiler_set_metric((zp), (metric), (value));     \
        }                                   \
    } while (0)

/**
* Print metric if profiler on and ZN_PROFILER_EVERY=true
*/
#ifdef ZN_PROFILER_PRINT_EVERY
#define ZN_PROFILER_PRINTF(zp, M, ...)    \
    do {                                    \
        if ((zp) != NULL) {                \
            struct timespec ts;           \
            TIME_NOW(&ts);                \
            fprintf(zp->fp, "%f," M, SINCE_PROFILER_BEGAN(zp, ts), ##__VA_ARGS__);     \
        }                                   \
    } while (0)
#else
#define ZN_PROFILER_PRINTF(...)
#endif

/**
 * @brief Resets the specified metric's value and usage count to zero.
 *
 * This function sets both the cumulative value and usage count for the
 * given metric to zero, clearing any collected data.
 *
 * LOCKED BY **CALLEE**
 *
 * @param[in,out] zp      Pointer to the profiler structure managing the metrics.
 * @param[in]     metric  Enum identifier of the metric to reset.
 */
void
zn_profiler_reset_metric(struct zn_profiler *zp, enum zn_profiler_tag metric);


#endif //PROFILING_H
