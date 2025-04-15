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

#define BACKOFF_US_START 100000
#define BACKOFF_RETRIES 5

void
zn_fg_evict(struct zn_cache *cache) {
    ZN_PROFILER_PRINTF(cache->profiler, "EVICTIONBEGIN_EVERY,%p\n", (void *) g_thread_self());
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
    ZN_PROFILER_PRINTF(cache->profiler, "EVICTIONEND_EVERY,%p\n", (void *) g_thread_self());
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

        struct timespec start_time, end_time;
        TIME_NOW(&start_time);
        int ret = zn_write_out(cache->fd, cache->chunk_sz, data, cache->io_size, wp);
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

    cache->io_size = MAX_IO == 0 ? cache->chunk_sz : MAX_IO;

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
    size_t chunk_sz = cache->chunk_sz;
    size_t max_io = cache->io_size;
    size_t align = ZN_DIRECT_ALIGNMENT;

    // Sanity checks
    if ((chunk_sz % align) != 0 || (max_io % align) != 0) {
        fprintf(stderr, "Error: Sizes must be aligned to %zu bytes for O_DIRECT\n", align);
        return NULL;
    }

    // Allocate aligned buffer
    unsigned char *data;
    if (posix_memalign((void **)&data, align, chunk_sz) != 0) {
        nomem();
    }
    // Calculate starting offset
    unsigned long long wp = CHUNK_POINTER(cache->zone_size, chunk_sz, zone_pair->chunk_offset, zone_pair->zone);
    if ((wp % align) != 0) {
        fprintf(stderr, "Error: Read offset (%llu) not aligned to %zu for O_DIRECT\n", wp, align);
        free(data);
        return NULL;
    }

    // Loop in max_io chunks
    size_t total_read = 0;
    while (total_read < chunk_sz) {
        size_t to_read = (chunk_sz - total_read > max_io) ? max_io : (chunk_sz - total_read);

        int attempts = 0;
        ssize_t r;
        while (true) {
            r = pread(cache->fd, data + total_read, to_read, wp + total_read);
            if (r == (ssize_t)to_read) {
                break; // success
            }

            if (++attempts >= BACKOFF_RETRIES) {
                fprintf(stderr, "Partial read at offset %llu (%zd/%zu): %s\n",
                        wp + total_read, r, to_read, strerror(errno));
                free(data);
                return NULL;
            }

            // exponential backoff: 100ms, 200ms, 400ms
            g_usleep(BACKOFF_US_START * (1 << (attempts - 1))); // g_usleep in microseconds
        }

        total_read += r;
    }

    return data;
}

#define BACKOFF_US_START 100000   // 100 ms in microseconds
#define BACKOFF_RETRIES 5         // number of retries

int
zn_write_out(int fd, size_t const to_write, const unsigned char *buffer, ssize_t write_size,
             unsigned long long wp_start) {
    ssize_t bytes_written;
    size_t total_written = 0;

    while (total_written < to_write) {
        ssize_t remaining = to_write - total_written;
        size_t chunk_size = (remaining < write_size) ? remaining : write_size;

        int attempts = 0;
        while (true) {
            errno = 0;
            bytes_written = pwrite(fd, buffer + total_written, chunk_size, wp_start + total_written);

            if (bytes_written == (ssize_t)chunk_size) {
                break; // success
            }

            if (++attempts >= BACKOFF_RETRIES) {
                fprintf(stderr, "Write failed at offset %llu (%zd/%zu): %s\n",
                        wp_start + total_written, bytes_written, chunk_size, strerror(errno));
                return -1;
            }

            // Exponential backoff: 100ms, 200ms, 400ms, ...
            g_usleep(BACKOFF_US_START * (1 << (attempts - 1)));
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
