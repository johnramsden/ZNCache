// For pread
#define _XOPEN_SOURCE 500
#include <stdint.h>
#include <unistd.h>

#include "znutil.h"
#include "zncache.h"
#include "znprofiler.h"

#include <stdlib.h>
#include <assert.h>
#include <linux/fs.h>

#include "libzbd/zbd.h"
#include <inttypes.h>

#define ZN_DIRECT_ALIGNMENT 4096

void
zn_fg_evict(struct zn_cache *cache) {
    uint32_t free_zones = zsm_get_num_free_zones(&cache->zone_state);
    if (cache->eviction_policy.type == ZN_EVICT_PROMOTE_ZONE) {
        for (uint32_t i = 0; i < EVICT_LOW_THRESH_ZONES - free_zones; i++) {
            int zone =
                cache->eviction_policy.do_evict(cache->eviction_policy.data);
            if (zone == -1) {
                dbg_printf("No zones to evict%s", "\n");
                break;
            }

            zn_cachemap_clear_zone(&cache->cache_map, zone);
            while (cache->active_readers[zone] > 0) {
                g_thread_yield();
            }

            // We can assume that no threads will create entries to the zone in the cache map,
            // because it is full.
            int ret = zsm_evict(&cache->zone_state, zone);
            if (ret != 0) {
                assert(!"Issue occurred with evicting zones\n");
            }
        }
    } else if (cache->eviction_policy.type == ZN_EVICT_CHUNK) {
        (void)cache->eviction_policy.do_evict(cache->eviction_policy.data);
    } else {
        assert(!"NYI");
    }
}

static int
zn_get_wp(int fd, uint32_t zone_id, ssize_t zone_cap, unsigned long long *wp) {
    struct zbd_zone zone[MAX_ZONES_USED];

    unsigned int nr_zones = MAX_ZONES_USED;
    int ret = zbd_report_zones(fd, 0, MAX_ZONES_USED*zone_cap, ZBD_RO_ALL, zone, &nr_zones);

    if (ret != 0 || nr_zones == 0) {
        return -1;
    }

    *wp = zone[zone_id].wp;
    return 0;
}


unsigned char *
zn_cache_get(struct zn_cache *cache, const uint32_t id, unsigned char *random_buffer) {
    unsigned char *data = NULL;

    // PROFILE
    struct timespec total_start_time, total_end_time;
    TIME_NOW(&total_start_time);

    struct zone_map_result result = zn_cachemap_find(&cache->cache_map, id);
    assert(result.type != RESULT_EMPTY);

    // Found the entry, read it from disk, update eviction, and decrement reader.
    if (result.type == RESULT_LOC) {
        struct timespec start_time, end_time;
        TIME_NOW(&start_time);
        unsigned char *data = zn_read_from_disk(cache, &result.location);
        TIME_NOW(&end_time);
        double t = TIME_DIFFERENCE_NSEC(start_time, end_time);
        ZN_PROFILER_UPDATE(cache->profiler, ZN_PROFILER_METRIC_READ_LATENCY, t);
        ZN_PROFILER_PRINTF(cache->profiler, "READLATENCY_EVERY,%f\n", t);

        cache->eviction_policy.update_policy(cache->eviction_policy.data, result.location,
                                             ZN_READ);

        // Sadly, we have to remember to decrement the reader count here
        g_atomic_int_dec_and_test(&cache->active_readers[result.location.zone]);

        g_mutex_lock(&cache->ratio.lock);
        cache->ratio.hits++;
        g_mutex_unlock(&cache->ratio.lock);

        TIME_NOW(&total_end_time);
        t = TIME_DIFFERENCE_NSEC(total_start_time, total_end_time);
        ZN_PROFILER_PRINTF(cache->profiler, "CACHEHITLATENCY_EVERY,%f\n", t);
        ZN_PROFILER_UPDATE(cache->profiler, ZN_PROFILER_METRIC_HIT_LATENCY, t);
        ZN_PROFILER_UPDATE(cache->profiler, ZN_PROFILER_METRIC_CACHE_HIT_THROUGHPUT, cache->chunk_sz);

        return data;
    } else { // result.type == RESULT_COND

        // Repeatedly attempt to get an active zone. This function can fail when there all active
        // zones are writing, so put this into a while loop.
        struct zn_pair location;
	int attempts = 0;
        while (true) {

            enum zsm_get_active_zone_error ret = zsm_get_active_zone(&cache->zone_state, &location);

            if (ret == ZSM_GET_ACTIVE_ZONE_RETRY) {
                attempts++;
                g_thread_yield();
            } else if (ret == ZSM_GET_ACTIVE_ZONE_ERROR) {
                goto UNDO_MAP;
            } else if (ret == ZSM_GET_ACTIVE_ZONE_EVICT) {
                zn_fg_evict(cache);
            } else {
                break;
            }
        }

        // Emulates pulling in data from a remote source by filling in a cache entry with random
        // bytes
        data = zn_gen_write_buffer(cache, id, random_buffer);

        // Write buffer to disk, 4kb blocks at a time
        unsigned long long wp =
            CHUNK_POINTER(cache->zone_size, cache->chunk_sz, location.chunk_offset, location.zone);

        int ret;
        if (cache->backend == ZE_BACKEND_ZNS) {
            unsigned long long wp_bytes;
            ret = zn_get_wp(cache->fd, location.zone, cache->zone_cap, &wp_bytes);
            if (ret != 0 || wp_bytes != wp) {
                fprintf(stderr, "Error (%d): wp_bytes (%llu) != wp (%llu), zone=%u, chunk=%u\n", ret, wp_bytes, wp, location.zone, location.chunk_offset);
                assert(!"wp_bytes != wp");
            }
        }

        struct timespec start_time, end_time;
        TIME_NOW(&start_time);
        ret = zn_write_out(cache->fd, cache->chunk_sz, data, ZN_WRITE_GRANULARITY, wp, cache->backend, location.zone, cache->zone_cap);
        TIME_NOW(&end_time);
        double t = TIME_DIFFERENCE_NSEC(start_time, end_time);
        ZN_PROFILER_UPDATE(cache->profiler, ZN_PROFILER_METRIC_WRITE_LATENCY, t);
        ZN_PROFILER_PRINTF(cache->profiler, "WRITELATENCY_EVERY,%f\n", t);

        if (ret != 0) {
            dbg_printf("Couldn't write to fd at wp=%llu, zone=%u, chunk=%u\n", wp, location.chunk_offset, location.zone);
            goto UNDO_ZONE_GET;
        }

        g_mutex_lock(&cache->ratio.lock);
        cache->ratio.misses++;
        g_mutex_unlock(&cache->ratio.lock);

        // Update metadata
        zsm_return_active_zone(&cache->zone_state, &location);

        cache->eviction_policy.update_policy(cache->eviction_policy.data, location, ZN_WRITE);

        zn_cachemap_insert(&cache->cache_map, id, location);

        TIME_NOW(&total_end_time);
        t = TIME_DIFFERENCE_NSEC(total_start_time, total_end_time);
        ZN_PROFILER_PRINTF(cache->profiler, "CACHEMISSLATENCY_EVERY,%f\n", t);
        ZN_PROFILER_UPDATE(cache->profiler, ZN_PROFILER_METRIC_MISS_LATENCY, t);
        ZN_PROFILER_UPDATE(cache->profiler, ZN_PROFILER_METRIC_CACHE_MISS_THROUGHPUT, cache->chunk_sz);

        return data;

    UNDO_ZONE_GET:
        zsm_failed_to_write(&cache->zone_state, location);
    UNDO_MAP:
        zn_cachemap_fail(&cache->cache_map, id);

        return NULL;
    }
}

void
zn_init_cache(struct zn_cache *cache, struct zbd_info *info, size_t chunk_sz, uint64_t zone_cap,
              int fd, enum zn_evict_policy_type policy, enum zn_backend backend, uint32_t* workload_buffer,
              uint64_t workload_max, char *metrics_file) {
    cache->fd = fd;
    cache->chunk_sz = chunk_sz;
    cache->nr_zones = info->nr_zones;
    cache->zone_cap = zone_cap;
    cache->max_nr_active_zones =
        info->max_nr_active_zones == 0 ? MAX_OPEN_ZONES : info->max_nr_active_zones;
    cache->zone_cap = zone_cap;
    cache->zone_size = info->zone_size;
    cache->max_zone_chunks = zone_cap / chunk_sz;
    cache->backend = backend;
    cache->active_readers = calloc(cache->nr_zones, sizeof(gint));
    cache->reader.workload_buffer = workload_buffer;
    cache->reader.workload_max = workload_max;

#ifdef DEBUG
    printf("Initialized cache:\n");
    printf("\tchunk_sz=%lu\n", cache->chunk_sz);
    printf("\tnr_zones=%u\n", cache->nr_zones);
    printf("\tzone_cap=%" PRIu64 "\n", cache->zone_cap);
    printf("\tmax_zone_chunks=%" PRIu64 "\n", cache->max_zone_chunks);
    printf("\tmax_nr_active_zones=%u\n", cache->max_nr_active_zones);
#endif

    // Set up the data structures
    zn_cachemap_init(&cache->cache_map, cache->nr_zones, cache->active_readers);
    zn_evict_policy_init(&cache->eviction_policy, policy, cache);
    zsm_init(&cache->zone_state, cache->nr_zones, fd, zone_cap, cache->zone_size, chunk_sz,
             cache->max_nr_active_zones, cache->backend);

    cache->ratio.hits = 0;
    cache->ratio.misses = 0;
    g_mutex_init(&cache->ratio.lock);

    cache->profiler = NULL;
    if (metrics_file != NULL) {
        cache->profiler = zn_profiler_init(metrics_file);
        assert(cache->profiler != NULL);
    }

    g_mutex_init(&cache->reader.lock);
    cache->reader.workload_index = 0;
    cache->reader.thresh_perc = 0;

    /* VERIFY_ZE_CACHE(cache); */
}

void
zn_destroy_cache(struct zn_cache *cache) {
    (void) cache;
    if (cache->profiler != NULL) {
        zn_profiler_close(cache->profiler);
    }

    // TODO assert(!"Todo: clean up cache");

    /* g_hash_table_destroy(cache->zone_map); */
    /* g_free(cache->zone_state); */
    /* g_queue_free_full(cache->active_queue, g_free); */
    /* g_queue_free(cache->lru_queue); */

    /* // TODO: MISSING FREES */

    /* if(cache->backend == ZE_BACKEND_ZNS) { */
    /*     zbd_close(cache->fd); */
    /* } else { */
    /*     close(cache->fd); */
    /* } */

    /* g_mutex_clear(&cache->cache_lock); */
    /* g_mutex_clear(&cache->reader.lock); */
}

unsigned char *
zn_read_from_disk(struct zn_cache *cache, struct zn_pair *zone_pair) {
    unsigned char *data;

    // Ensure chunk size is aligned
    if ((cache->chunk_sz % ZN_DIRECT_ALIGNMENT) != 0) {
        fprintf(stderr, "Error: chunk_sz (%zu) must be multiple of %d for O_DIRECT\n",
                cache->chunk_sz, ZN_DIRECT_ALIGNMENT);
        return NULL;
    }

    // Allocate aligned buffer
    if (posix_memalign((void **)&data, ZN_DIRECT_ALIGNMENT, cache->chunk_sz) != 0) {
        nomem();
    }

    // Compute aligned offset
    unsigned long long wp =
        CHUNK_POINTER(cache->zone_size, cache->chunk_sz, zone_pair->chunk_offset, zone_pair->zone);

    if ((wp % ZN_DIRECT_ALIGNMENT) != 0) {
        fprintf(stderr, "Error: read offset (%llu) not aligned to %d for O_DIRECT\n",
                wp, ZN_DIRECT_ALIGNMENT);
        free(data);
        return NULL;
    }

    dbg_printf("[%u,%u] read from write pointer: %llu\n",
               zone_pair->zone, zone_pair->chunk_offset, wp);

    ssize_t b = pread(cache->fd, data, cache->chunk_sz, wp);
    if (b != (ssize_t)cache->chunk_sz) {
        fprintf(stderr, "Couldn't read from fd: %s\n", strerror(errno));
        free(data);
        return NULL;
    }

    return data;
}

#define MAX_RETRY 10

int
zn_write_out(int fd, size_t const to_write, const unsigned char *buffer, ssize_t write_size,
             unsigned long long wp_start, enum zn_backend backend, uint32_t zone_id, ssize_t zone_cap) {
    ssize_t bytes_written;
    size_t total_written = 0;

    uint32_t retrys = 0;

    errno = 0;
    while (total_written < to_write) {
        ssize_t remaining = to_write - total_written;
        size_t chunk_size = (remaining < write_size) ? remaining : write_size;

        bytes_written = pwrite(fd, buffer + total_written, chunk_size, wp_start + total_written);

        if (bytes_written <= 0 || (errno != 0)) {
            fprintf(stderr,"Error: %s\n", strerror(errno));
            fprintf(stderr, "Couldn't write to fd=%d at offset=%llu\n", fd, wp_start + total_written);
            if (backend == ZE_BACKEND_ZNS) {
                unsigned long long wp;
                int ret = zn_get_wp(fd, zone_id, zone_cap, &wp);
                if (ret != 0) {
                    assert(!"Error getting wp");
                }
                total_written = wp - wp_start;
            }
            retrys++;
            if (retrys > MAX_RETRY) {
                assert(!"Max retries exceeded");
            }
            errno = 0;
            continue;
        }

        total_written += bytes_written;
    }

    assert(total_written == to_write);

    return 0;
}

unsigned char *
zn_gen_write_buffer(struct zn_cache *cache, uint32_t zone_id, unsigned char *buffer) {
    unsigned char *data;

    if (posix_memalign((void **)&data, ZN_DIRECT_ALIGNMENT, cache->chunk_sz) != 0) {
        nomem();
    }

    memcpy(data, buffer, cache->chunk_sz);
    memcpy(data, &zone_id, sizeof(uint32_t));

    g_usleep(ZN_READ_SLEEP_US);

    return data;
}

int
zn_validate_read(struct zn_cache *cache, unsigned char *data, uint32_t id, unsigned char *compare_buffer) {
    uint32_t read_id;
    memcpy(&read_id, data, sizeof(uint32_t));
    if (read_id != id) {
        dbg_printf("Invalid read_id(%u)!=id(%u)\n", read_id, id);
        return -1;
    }
    // 4 bytes for int
    for (uint32_t i = sizeof(uint32_t); i < cache->chunk_sz; i++) {
        if (data[i] != compare_buffer[i]) {
            dbg_printf("data[%d]!=RANDOM_DATA[%d]\n", read_id, id);
            return -1;
        }
    }
    return 0;
}

double
zn_cache_get_hit_ratio(struct zn_cache * cache) {
    g_mutex_lock(&cache->ratio.lock);
    double num = cache->ratio.hits;
    double den = cache->ratio.misses + cache->ratio.hits;
    g_mutex_unlock(&cache->ratio.lock);
    if (den == 0) {
        return 0;
    }
    return num / den;
}
