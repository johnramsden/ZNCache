#pragma once

#include "chunk_queue.h"
#include "eviction_policy.h"
#include "glib.h"

#include <stdint.h>

struct zn_policy_chunk {
    struct chunk_queue chunk_queue; /** Data structure for storing chunk eviction state */
    struct zn_cache *cache; /**< Shared pointer to cache (not owned by policy) */
    unsigned char *chunk_buf; /**< Buffer for use during GC */

    GMutex policy_mutex;          /**< LRU lock */
    uint32_t total_chunks;   /**< Number of chunks on disk */
} __attribute__((aligned(128)));

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
