#pragma once
#include <stdint.h>
#include "glib.h"

/** This per-zone data structure provides the backing for minheap.
 */
struct eviction_policy_chunk_zone {
    uint32_t zone_id;
    struct zn_pair *chunks; /**< Pool of chunks, backing for lru */
    uint32_t chunks_in_use;
    bool filled;
    struct zn_minheap_entry * pqueue_entry; /**< Entry in invalid_pqueue */
} __attribute__((aligned(32)));


/** This data structure stores the bookkeeping required to keep track
    of valid/invalid chunks and provides operations to dequeue zones
    for eviction. */
struct chunk_queue {
    struct zn_minheap * invalid_pqueue; /**< Priority queue keeping track of invalid zones. Stores as data: ptrs to eviction_policy_chunk_zones */
    GHashTable *chunk_to_lru_map; /**< Hash table mapping chunks to locations in the LRU queue. */
    // Tail is the end of the queue, head is the least recently used
    GQueue lru_queue;             /**< Least Recently Used (LRU) queue of chunks for eviction. */
    struct eviction_policy_chunk_zone *zone_pool; /**< Pool of zones, backing for minheap */

    uint32_t total_chunks;   /**< Number of chunks on disk */
    uint32_t max_zone_chunks;   /**< Number of chunks on disk */
} __attribute__((aligned(64)));

/** @brief Initializes the chunk queue.
    @param[out] chunk_queue the data structure to be initialized
    @param[in] nr_zones the number of zones in the cache
    @param[in] max_zone_chunks the number of chunks in a zone
    @returns 0 on success, 1 on failure
 */
int
zn_cq_init_chunk_queue(struct chunk_queue *, uint32_t nr_zones,
                       uint32_t max_zone_chunks);

/** @brief Adds a chunk to the LRU queue.
    @param[in] chunk_queue the data structure
    @param[in] zn_pair the chunk to be added
    @returns 0 on success, 1 on failure
 */
int
zn_cq_add_chunk_to_lru(struct chunk_queue*, struct zn_pair);

/** @brief Updates a chunk already in the chunk queue.
    @param[out] chunk_queue the data structure
    @param[in] zn_pair the chunk to be updated
    @returns 0 on success, 1 on failure
 */
int
zn_cq_update_chunk_in_lru(struct chunk_queue *cq, struct zn_pair chunk);

/** @brief Invalidates the latest chunk in the data structure and returns it.
    @param[in] chunk_queue the data structure
    @param[out] out the out parameter to store the chunk
    @returns 0 on success, 1 on failure.
 */
int
zn_cq_invalidate_latest_chunk(struct chunk_queue*, struct zn_pair* out);

/** @brief Returns the most invalidated zone.
    @param[in] chunk_queue the data structure
    @param[out] out_zone the out parameter to store the zone that should be invalidated
    @param[out] valid_chunks the list of chunks that are still valid in the zone and need to be moved
    @param[out] valid_length the length of the valid chunk list
    @returns 0 on success, 1 on failure.
 */
__attribute__((__warn_unused_result__))
int
zn_cq_zone_dequeue(struct chunk_queue *, uint32_t *out_zone,
                   struct zn_pair** valid_chunks, uint32_t *valid_length);

/** @brief Returns the length of the LRU queue
 */
uint32_t
zn_cq_lru_len(struct chunk_queue*);
