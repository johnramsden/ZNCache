#ifndef ZNCACHE_H
#define ZNCACHE_H

#include <stdbool.h> // Needed on old C (actions, cortes)
#include <stdint.h>
#include <glib.h>
#include <libzbd/zbd.h>

#include "cachemap.h"
#include "zone_state_manager.h"
#include "eviction_policy.h"
#include "znbackend.h"
#include "znprofiler.h"

#define PRINT_THRESH_PERCENT 1

#define MICROSECS_PER_SECOND 1000000
// #define EVICT_SLEEP_US ((long) (EVICT_SLEEP_SECS * MICROSECS_PER_SECOND)) // Compile-time
// #define ZE_READ_SLEEP_US ((long) (0.25 * MICROSECS_PER_SECOND)) // Compile-time

#define MAX_OPEN_ZONES 14

#define ZN_WRITE_GRANULARITY 1048576 // 1MB

/**
 * @struct zn_reader
 * @brief Manages concurrent read operations within the cache.
 *
 * The reader structure tracks query execution and workload distribution,
 * ensuring thread-safe access to cached data.
 */
struct zn_reader {
    GMutex lock;             /**< Mutex to synchronize access to the reader state. */
    uint64_t workload_index; /**< Index of the workload associated with the reader. */
    uint32_t* workload_buffer;
    uint64_t workload_max;
    uint64_t thresh_perc; /**< The next percentage to report numbers at */
};

struct zn_cache_hitratio {
    GMutex lock;
    uint64_t hits;
    uint64_t misses;
};

/**
 * @struct zn_cache
 * @brief Represents a cache system that manages data storage in predefined zones.
 *
 * This structure is responsible for managing zones, including tracking active,
 * free, and recently used zones. It supports parallel insertion, LRU eviction,
 * and mapping of data IDs to specific zones and offsets.
 */
struct zn_cache {
    enum zn_backend backend;      /**< SSD backend. */
    int fd;                       /**< File descriptor for associated disk. */
    uint32_t max_nr_active_zones; /**< Maximum number of zones that can be active at once. */
    uint32_t nr_zones;            /**< Total number of zones availible. */
    uint64_t max_zone_chunks;     /**< Maximum number of chunks a zone can hold. */
    size_t chunk_sz;              /**< Size of each chunk in bytes. */
    uint64_t zone_cap;            /**< Maximum storage capacity per zone in bytes. */
    uint64_t zone_size;           /**< Storage size per zone in bytes. */

    struct zn_cachemap cache_map;
    struct zn_evict_policy eviction_policy;
    struct zone_state_manager zone_state;
    struct zn_reader reader; /**< Reader structure for tracking workload location. */
    gint *active_readers;    /**< Owning reference of the list of active readers per zone */

    struct zn_cache_hitratio ratio;

    struct zn_profiler * profiler; /**< Stores metrics */
};

/**
 * @brief Execute eviction in foreground
 *
 * @param cache Pointer to the `zn_cache` structure.
 */
void
zn_fg_evict(struct zn_cache *cache);

/**
 * @brief Get data from cache
 *
 * Gets data from cache if present, otherwise pulls from emulated remote
 *
 * @param cache Pointer to the `zn_cache` structure.
 * @param id Cache item ID to get
 * @param random_buffer Buffer used for read simulation
 * @returns Buffer of data recieved or NULL on error (callee is responsible for freeing)
 */
unsigned char *
zn_cache_get(struct zn_cache *cache, const uint32_t id, unsigned char *random_buffer);

/**
 * @brief Initializes a `zn_cache` structure with the given parameters.
 *
 * This function sets up the cache by initializing its fields, creating the required
 * data structures (hash table, queues, state array), and setting up synchronization
 * mechanisms. It also verifies the integrity of the initialized cache.
 *
 * @param cache Pointer to the `zn_cache` structure to initialize.
 * @param info Pointer to `zbd_info` providing zone details.
 * @param chunk_sz The size of each chunk in bytes.
 * @param zone_cap The maximum capacity per zone in bytes.
 * @param fd File descriptor associated with the disk
 * @param eviction_policy Eviction policy used
 */
void
zn_init_cache(struct zn_cache *cache, struct zbd_info *info, size_t chunk_sz, uint64_t zone_cap,
              int fd, enum zn_evict_policy_type policy, enum zn_backend backend, uint32_t* workload_buffer,
              uint64_t workload_max, char *metrics_file);

/**
 * @brief Destroys and cleans up a `zn_cache` structure.
 *
 * This function frees all dynamically allocated resources associated with the cache,
 * including hash tables, queues, and state arrays. It also closes the file descriptor
 * and clears associated mutexes to ensure proper cleanup.
 *
 * @param cache Pointer to the `zn_cache` structure to be destroyed.
 *
 * @note After calling this function, the `zn_cache` structure should not be used
 *       unless it is reinitialized.
 * @note The function assumes that `zam` is properly initialized before being passed.
 */
void
zn_destroy_cache(struct zn_cache *cache);

/**
 * @brief Read a chunk from disk
 *
 * @param cache Pointer to the `zn_cache` structure
 * @param zone_pair Chunk, zone pair
 * @return Buffer read from disk, to be freed by caller
 */
unsigned char *
zn_read_from_disk(struct zn_cache *cache, struct zn_pair *zone_pair);

/**
 * @brief Write buffer to disk
 *
 * @param to_write Total size of write
 * @param buffer   Buffer to write to disk
 * @param fd       Disk file descriptor
 * @param write_size Granularity for each write
 * @return int     Non-zero on error
 *
 * @note Be careful write size is not too large otherwise you can get errors
 */
int
zn_write_out(int fd, size_t const to_write, const unsigned char *buffer, ssize_t write_size,
             unsigned long long wp_start, enum zn_backend backend);

/**
 * Allocate a buffer prefixed by `zone_id`, with the rest being `RANDOM_DATA`
 * Simulates remote read with ZE_READ_SLEEP_US
 *
 * @param cache Pointer to the `zn_cache` structure.
 * @param zone_id ID to write to first 4 bytes
 * @return Allocated buffer or NULL, caller is responsible for free
 */
unsigned char *
zn_gen_write_buffer(struct zn_cache *cache, uint32_t zone_id, unsigned char *buffer);

/**
 * Validate contents of cache read
 *
 * @param cache Pointer to the `zn_cache` structure.
 * @param data Data to validate against RANDOM_DATA
 * @param id Identifier that should be in first 4 bytes
 * @return Non-zero on error
 */
int
zn_validate_read(struct zn_cache *cache, unsigned char *data, uint32_t id, unsigned char *compare_buffer);

/**
 * Get the cache hitratio
 *
 * @param cache Pointer to the `zn_cache` structure.
 * @return Cache hit ratio
 */
double
zn_cache_get_hit_ratio(struct zn_cache * cache);

#endif // ZNCACHE_H
