#pragma once

#include "minheap.h"
#include "eviction_policy.h"
#include "glib.h"

#include "cachemap.h"
#include "zone_state_manager.h"

#include <stdint.h>

struct eviction_policy_chunk_zone {
    uint32_t zone_id;
    struct zn_pair *chunks; /**< Pool of chunks, backing for lru */
    uint32_t chunks_in_use;
    bool filled;
    struct zn_minheap_entry * pqueue_entry; /**< Entry in invalid_pqueue */
};

struct zn_policy_chunk {
    // Tail is the end of the queue, head is the least recently used
    GQueue lru_queue;             /**< Least Recently Used (LRU) queue of chunks for eviction. */
    GHashTable *chunk_to_lru_map; /**< Hash table mapping chunks to locations in the LRU queue. */
    GMutex policy_mutex;          /**< LRU lock */

    struct zn_minheap * invalid_pqueue; /**< Priority queue keeping track of invalid zones. Stores as data: ptrs to eviction_policy_chunk_zones */

    struct eviction_policy_chunk_zone *zone_pool; /**< Pool of zones, backing for minheap */

    struct zn_cache *cache; /**< Shared pointer to cache (not owned by policy) */
    uint32_t total_chunks;   /**< Number of chunks on disk */

    unsigned char *chunk_buf; /**< Buffer for use during GC */
};

/** @brief Updates the chunk LRU policy
 */
void
zn_policy_chunk_update(policy_data_t policy, struct zn_pair location,
                             enum zn_io_type io_type);

/** @brief Gets a chunk to evict.
    @returns the 0 on evict, 1 if no evict.
 */
int
zn_policy_chunk_evict(policy_data_t policy);
