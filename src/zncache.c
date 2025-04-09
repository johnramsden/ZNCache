// For pread
#include <bits/posix1_lim.h>
#define _XOPEN_SOURCE 500
#define _GNU_SOURCE
#include <string.h>
#include "zncache.h"

#include "znprofiler.h"
#include "eviction_policy.h"
#include "libzbd/zbd.h"
#include "znutil.h"
#include "zone_state_manager.h"

#include <assert.h>
#include <ctype.h>
#include <fcntl.h>
#include <glib.h>
#include <getopt.h>
#include <linux/fs.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <unistd.h>

// No evict

uint32_t simple_workload[] = {
    1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 1, 2, 3, 4, 5, 6, 7, 8,
    9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19,
    20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32,
    33, 34, 35, 36, 37, 38, 39, 40,
	21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40};

unsigned char *RANDOM_DATA = NULL;

/**
 * @struct zn_thread_data
 * @brief Holds thread-specific data for interacting with the cache.
 *
 * This structure associates a thread with a specific cache instance
 * and provides a unique thread identifier.
 */
struct zn_thread_data {
    struct zn_cache *cache; /**< Pointer to the cache instance associated with this thread. */
    uint32_t tid;           /**< Unique identifier for the thread. */
    bool *done;              /**< Marks completed */
    uint32_t nr_threads;    /**< Total threads */
    uint32_t *nr_threads_completed;    /**< Total threads done */
    GMainLoop *loop;        /**< Main loop */
    GMutex *thread_counter_lock;
};


/**
 * Eviction thread
 *
 * @param user_data thread_data
 * @return
 */
gpointer
evict_task(gpointer user_data) {
    struct zn_thread_data *thread_data = user_data;
    struct zn_cache *cache = thread_data->cache;

    printf("Evict task started by thread %p\n", (void *) g_thread_self());

    while (true) {
        if (*thread_data->done) {
            break;
        }

        uint32_t free_zones = zsm_get_num_free_zones(&cache->zone_state);
        if (free_zones > EVICT_HIGH_THRESH_ZONES) {
            g_usleep(EVICT_INTERVAL_US);
            continue;
        }

        assert(EVICT_LOW_THRESH_ZONES - free_zones > 0);

        zn_fg_evict(cache);
    }

    printf("Evict task completed by thread %p\n", (void *) g_thread_self());

    return NULL;
}

// Task function. The function that each thread runs. This will simulate servicing cache requests.
// @param data
void
task_function(gpointer data, gpointer user_data) {
    (void) user_data;
    struct zn_thread_data *thread_data = data;

    printf("Task %d started by thread %p\n", thread_data->tid, (void *) g_thread_self());

    // Handles any cache read requests
    while (true) {
        g_mutex_lock(&thread_data->cache->reader.lock);

        uint32_t data_id = 0;

		// When we exceed Query number, go next workload
		// The query that we're on

		// We finished reading all the workloads
		if (thread_data->cache->reader.workload_index >= thread_data->cache->reader.workload_max) {
			g_mutex_unlock(&thread_data->cache->reader.lock);
			break;
		}

		// Increment the query index
        bool print = false;
        uint64_t wi = thread_data->cache->reader.workload_index++;
        uint64_t percent = (wi * 100L) / thread_data->cache->reader.workload_max;

        if (percent >= thread_data->cache->reader.thresh_perc) {
	    thread_data->cache->reader.thresh_perc += PRINT_THRESH_PERCENT;
            print = true; // Print while unlocked
        }
		g_mutex_unlock(&thread_data->cache->reader.lock);

        if (print) {
            printf("[%d]:\t(%lu%%)\tze_cache_get(workload[%lu]=%d)\n", thread_data->tid, percent, wi,
		   thread_data->cache->reader.workload_buffer[wi]);
            fflush(stdout);
        }

        dbg_printf("[%d]: ze_cache_get(workload[%d]=%d)\n", thread_data->tid, wi,
               thread_data->cache->reader.workload_buffer[wi]);

		data_id = thread_data->cache->reader.workload_buffer[wi];

        // Find the data in the cache
        // PROFILE START
        struct timespec start_time, end_time;
        TIME_NOW(&start_time);
        unsigned char *data = zn_cache_get(thread_data->cache, data_id, RANDOM_DATA);
        if (data == NULL) {
            dbg_printf("ERROR: Couldn't get data for data_id=%u\n", data_id);
            return;
        }
        TIME_NOW(&end_time);
        double t = TIME_DIFFERENCE_NSEC(start_time, end_time);
        ZN_PROFILER_UPDATE(thread_data->cache->profiler, ZN_PROFILER_METRIC_GET_LATENCY, t);
        ZN_PROFILER_PRINTF(thread_data->cache->profiler, "GETLATENCY_EVERY,%f\n", t);
        // PROFILE END

#ifdef VERIFY
        assert(zn_validate_read(thread_data->cache, data, data_id, RANDOM_DATA) == 0);
#endif
        free(data);

        // PROFILE METRICS
        // Throughput
        ZN_PROFILER_UPDATE(thread_data->cache->profiler, ZN_PROFILER_METRIC_CACHE_THROUGHPUT, thread_data->cache->chunk_sz);
        // Update cache size
        ZN_PROFILER_SET(
            thread_data->cache->profiler,
            ZN_PROFILER_METRIC_CACHE_USED_MIB,
            BYTES_TO_MIB(zn_evict_policy_get_cache_size(&thread_data->cache->eviction_policy))
        );
        // Update free zones
        ZN_PROFILER_SET(
            thread_data->cache->profiler,
            ZN_PROFILER_METRIC_CACHE_FREE_ZONES,
            zsm_get_num_free_zones(&thread_data->cache->zone_state)
        );
        // Update cache hitratio
        double hr = zn_cache_get_hit_ratio(thread_data->cache);
        ZN_PROFILER_SET(
            thread_data->cache->profiler,
            ZN_PROFILER_METRIC_CACHE_HITRATIO,
            hr
        );
        // Show thread still active
        ZN_PROFILER_PRINTF(thread_data->cache->profiler, "THREADID_EVERY,%d\n", thread_data->tid);
        dbg_printf("Hitratio: %f\n", hr);
    }
    printf("Task %d finished by thread %p\n", thread_data->tid, (void *) g_thread_self());

    g_mutex_lock(thread_data->thread_counter_lock);
    (*thread_data->nr_threads_completed)++;
    if (*thread_data->nr_threads_completed == thread_data->nr_threads) {
        g_main_loop_quit(thread_data->loop);
        *thread_data->done = true;
    }
    g_mutex_unlock(thread_data->thread_counter_lock);
}

/**
 * Profiling task triggerred periodically
 *
 * @param user_data Profiler
 * @return TRUE
 */
static gboolean
profiling_task(gpointer user_data) {
    struct zn_profiler *zp = user_data;

    zn_profiler_write_all_and_reset(zp);

    // Return TRUE to keep firing
    return TRUE;
}

static void
usage(FILE * file, char *progname) {
    fprintf(file,
            "Usage: %s <DEVICE> <CHUNK_SZ> <THREADS> [-w workload_file] [-i iterations] [-m metrics_file ] [ -h]\n",
            progname);
}

/**
 * Read an exact workload amount
 *
 * @param fd File to read from
 * @param buffer Buffer to populate
 * @param size Exact size to read
 * @return Non-zero on error
 */
int
read_workload(int fd, uint32_t *buffer, size_t size) {
    size_t total_bytes_read = 0;

    while (total_bytes_read < size) {
        errno = 0;
        ssize_t bytes_read = read(fd, buffer + total_bytes_read, size - total_bytes_read);
        if (bytes_read < 0) {
            if (errno == EINTR) {
                // Interrupted
                continue;
            }
            fprintf(stderr, "Couldn't read the file: '%s'\n", strerror(errno));
            return 1;
        }
        if (bytes_read == 0) {
            break;
        }
        total_bytes_read += bytes_read;
    }

    if (total_bytes_read != size) {
        fprintf(stderr, "Couldn't read the file fully, read %zu bytes out of %zu\n", total_bytes_read, size);
        return 1;
    }

    return 0;
}

int
main(int argc, char **argv) {
    zbd_set_log_level(ZBD_LOG_DEBUG);

    if (geteuid() != 0) {
        fprintf(stderr, "Please run as root\n");
        return -1;
    }

    if (argc < 4 || argc > 11) {
        usage(stderr, argv[0]);
        return -1;
    }

    char *device = argv[1];

    size_t chunk_sz = strtoul(argv[2], NULL, 10);
    int32_t nr_threads = strtol(argv[3], NULL, 10);
    int32_t nr_eviction_threads = 1;

    char *metrics_file = NULL;
    char *workload_file = NULL;
    uint64_t workload_max = UINT64_MAX;
    uint32_t *workload_buffer;

    int c;
    opterr = 0;
    optind = 4;
    while ((c = getopt(argc, argv, "w:i:m:h")) != -1) {
        switch (c) {
            case 'w':
                workload_file = optarg;
            break;
            case 'i':
                workload_max = strtol(optarg, NULL, 10);
            break;
            case 'm':
                metrics_file = optarg;
            break;
            case 'h':
                usage(stdout, argv[0]);
                exit(EXIT_SUCCESS);
            case '?':
                if (isprint(optopt)) {
                    fprintf(stderr, "Unknown option `-%c'.\n", optopt);
                } else {
                    fprintf(stderr, "Unknown option character `\\x%x'.\n", optopt);
                }
                usage(stderr, argv[0]);
                return -1;
            default:
                usage(stderr, argv[0]);
                exit(-1);
        }
    }

    enum zn_backend device_type = zbd_device_is_zoned(device) ? ZE_BACKEND_ZNS : ZE_BACKEND_BLOCK;

    if (workload_file != NULL) {
        if (workload_max == UINT64_MAX) {
            fprintf(stderr, "'iterations' must be set if 'workload_file' is set\n");
            return 1;
        }
        int workload_fd = open(workload_file, O_RDONLY);
        if (workload_fd == -1) {
            fprintf(stderr, "Couldn't read workload file %s\n", workload_file);
            return 1;
        }

        size_t workload_sz = workload_max * sizeof(uint32_t);

        workload_buffer = malloc(workload_sz);
        assert(workload_buffer != NULL);

        if (read_workload(workload_fd, workload_buffer, workload_sz) != 0) {
            return 1;
        }
    } else {
        workload_max = sizeof(simple_workload) / sizeof(uint32_t);
        workload_buffer = simple_workload;
    }

    printf("Running with configuration:\n"
           "\tDevice name: %s\n"
           "\tDevice type: %s\n"
           "\tChunk size: %lu\n"
           "\tBLOCK_ZONE_CAPACITY: %u\n"
           "\tWorker threads: %u\n"
           "\tEviction threads: %u\n"
           "\tWorkload file: %s\n"
           "\tMetrics file: %s\n",
           device, (device_type == ZE_BACKEND_ZNS) ? "ZNS" : "Block", chunk_sz,
           BLOCK_ZONE_CAPACITY, nr_threads, nr_eviction_threads,
           workload_file != NULL ? workload_file : "Simple generator",
           metrics_file != NULL ? metrics_file : "NO");

#ifdef DEBUG
    printf("\tDEBUG=on\n");
#endif
#ifdef VERIFY
    printf("\tVERIFY=on\n");
#endif

    struct zbd_info info = {0};
    int fd;
    if (device_type == ZE_BACKEND_ZNS) {
        fd = zbd_open(device, O_RDWR | O_DIRECT | O_SYNC, &info);
    } else {
        fd = open(device, O_RDWR | O_DIRECT | O_SYNC);

        uint64_t size = 0;
        if (ioctl(fd, BLKGETSIZE64, &size) == -1) {
            fprintf(stderr, "Couldn't get block size: %s\n", device);
            return -1;
        }

        if (size < BLOCK_ZONE_CAPACITY) {
            assert(!"The size of the disk is smaller than a single zone!");
        }
        info.nr_zones = ((long) size / BLOCK_ZONE_CAPACITY);
        info.max_nr_active_zones = 0;
    }

    if (fd < 0) {
        fprintf(stderr, "Error opening device: %s\n", device);
        return fd;
    }

    uint64_t zone_capacity = 0;
    if (device_type == ZE_BACKEND_ZNS) {
        if (MAX_ZONES_USED != 0) {
            info.nr_zones = MAX_ZONES_USED;
        }
        int ret = zbd_reset_zones(fd, 0, 0);
        if (ret != 0) {
            fprintf(stderr, "Couldn't reset zones\n");
            return -1;
        }

        ret = zone_cap(fd, &zone_capacity);
        if (ret != 0) {
            fprintf(stderr, "Couldn't report zone info\n");
            return ret;
        }
    } else {
        zone_capacity = BLOCK_ZONE_CAPACITY;
        info.zone_size = BLOCK_ZONE_CAPACITY;
    }

    RANDOM_DATA = generate_random_buffer(chunk_sz);
    if (RANDOM_DATA == NULL) {
        nomem();
    }

    struct zn_cache cache = {0};
    zn_init_cache(&cache, &info, chunk_sz, zone_capacity, fd, EVICTION_POLICY, device_type, workload_buffer, workload_max, metrics_file);

    GError *error = NULL;
    // Create a thread pool with a maximum of nr_threads
    GThreadPool *pool = g_thread_pool_new(task_function, NULL, nr_threads, FALSE, &error);
    if (error) {
        fprintf(stderr, "Error creating thread pool: %s\n", error->message);
        return 1;
    }

    struct timespec start_time, end_time;

    // Push tasks to the thread pool
    struct zn_thread_data *thread_data = g_new(struct zn_thread_data, nr_threads);
    assert(thread_data != NULL);
    // Setup mainloop to iterate context for profiling triggers
    GMainLoop *loop = g_main_loop_new(NULL, FALSE);

    if (cache.profiler != NULL && !cache.profiler->realtime) {
        g_timeout_add_seconds(PROFILING_INTERVAL_SEC, profiling_task, cache.profiler);
    }

    GMutex lock;
    g_mutex_init(&lock);
    uint32_t nr_threads_completed = 0;
    bool done = false;

    TIME_NOW(&start_time);
    for (int i = 0; i < nr_threads; i++) {
        thread_data[i].tid = i;
        thread_data[i].cache = &cache;
        thread_data[i].nr_threads = nr_threads;
        thread_data[i].nr_threads_completed = &nr_threads_completed;
        thread_data[i].thread_counter_lock = &lock;
        thread_data[i].loop = loop;
        thread_data[i].done = &done;

        g_thread_pool_push(pool, &thread_data[i], &error);
        if (error) {
            fprintf(stderr, "Error pushing task: %s\n", error->message);
            return 1;
        }
    }

    // Setup eviction thread
    struct zn_thread_data eviction_thread_data = {.tid = 0, .cache = &cache, .done = &done};

    GThread *thread = g_thread_new("evict-thread", evict_task, &eviction_thread_data);

    g_main_loop_run(thread_data->loop);

    // Wait for tasks to finish and free the thread pool
    g_thread_pool_free(pool, FALSE, TRUE);

    g_thread_join(thread);

    TIME_NOW(&end_time);

    printf("Total runtime: %0.2fs (%0.2fms)\n", TIME_DIFFERENCE_SEC(start_time, end_time),
               TIME_DIFFERENCE_MILLISEC(start_time, end_time));

    // Cleanup
    g_main_loop_unref(loop);
    zn_destroy_cache(&cache);
    g_free(thread_data);
    free(RANDOM_DATA);
    return 0;
}
