#include <stdlib.h>
#include <assert.h>
#include "znprofiler.h"

#include <string.h>
#include <znutil.h>

char *zn_profiler_metric_names[PROFILING_METRICS] = {
    "GETLATENCY",
    "CACHE_USED_MIB",
    "HITRATIO",
};

enum zn_profiler_type zn_profiler_metric_types[PROFILING_METRICS] = {
    ZN_PROFILER_AVG, // GET
    ZN_PROFILER_SET, // Cache size
    ZN_PROFILER_SET, // Hitratio
};

struct zn_profiler *
zn_profiler_init(const char *filename) {
    assert(filename);

    struct zn_profiler *zp = malloc(sizeof(struct zn_profiler));
    if (zp == NULL) {
        return NULL;
    }

    zp->fp = fopen(filename, "w");
    if (zp->fp == NULL) {
        dbg_printf("Failed to open file %s\n", filename);
        free(zp);
        return NULL;
    }

    // Enable buffering
    setvbuf(zp->fp, zp->buffer, _IOFBF, METRICS_BUFFER_SIZE);

    g_mutex_init(&zp->lock);

    zn_profiler_write(zp, "%s\n", PROFILING_HEADERS);

    for (uint32_t i = 0; i < PROFILING_METRICS; i++) {
        zn_profiler_reset_metric(zp, i);
        zp->metrics[i].type = zn_profiler_metric_types[i];
    }

    return zp;
}

void
zn_profiler_close(struct zn_profiler *zp) {
    fflush(zp->fp);
    free(zp);
}

void
zn_profiler_write(struct zn_profiler *zp, const char *format, ...) {
    va_list args;
    va_start(args, format);

    g_mutex_lock(&zp->lock);
    vfprintf(zp->fp, format, args);
    g_mutex_unlock(&zp->lock);

    va_end(args);
}

void
zn_profiler_reset_metric(struct zn_profiler *zp, enum zn_profiler_tag metric) {
    zp->metrics[metric].value = 0;
    zp->metrics[metric].count = 0;
}

void
zn_profiler_write_all_and_reset(struct zn_profiler *zp) {
    for (uint32_t i = 0; i < PROFILING_METRICS; i++) {
        g_mutex_lock(&zp->lock);
        double val = zp->metrics[i].value;
        if (zp->metrics[i].type == ZN_PROFILER_AVG) {
            val = zp->metrics[i].value / zp->metrics[i].count;
        }
        fprintf(zp->fp, "%s,%f\n", zn_profiler_metric_names[i], val);
        zn_profiler_reset_metric(zp, i);
        g_mutex_unlock(&zp->lock);
    }
}

void
zn_profiler_update_metric(struct zn_profiler *zp, enum zn_profiler_tag metric, double value) {
    g_mutex_lock(&zp->lock);
    zp->metrics[metric].value+=value;
    zp->metrics[metric].count++;
    g_mutex_unlock(&zp->lock);
}

void
zn_profiler_set_metric(struct zn_profiler *zp, enum zn_profiler_tag metric, double value) {
    g_mutex_lock(&zp->lock);
    zp->metrics[metric].value = value;
    g_mutex_unlock(&zp->lock);
}

