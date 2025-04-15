#include "cachemap.h"
#include "chunk_queue.h"
#include "eviction_policy.h"
#include "eviction_policy_chunk.h"
#include "znbackend.h"
#include "zncache.h"
#include "znutil.h"
#include "zone_state_manager.h"

#include <assert.h>
#include <stdint.h>
#include <stdbool.h> // Cortes
#include <stdlib.h>
#include <glib.h>
#include <glibconfig.h>
#include <string.h>

// ========== Chunk helper functions ==========

/** @brief Copies old data from the chunk buffer to the new location. Updates metadata.

 *  This function assumes that it can use p->chunk_buf as-is.
 */
static void
zn_policy_write_to_zone(struct zn_policy_chunk* p, struct zn_pair old, struct zn_pair new) {
    // Copy data to new zone
    unsigned char *data = &p->chunk_buf[old.chunk_offset * p->cache->chunk_sz];
    unsigned long long wp = CHUNK_POINTER(p->cache->zone_size, p->cache->chunk_sz,
                                          new.chunk_offset, new.zone);
    if (zn_write_out(p->cache->fd, p->cache->chunk_sz, data, ZN_WRITE_GRANULARITY, wp, p->cache->backend) != 0) {
        assert(!"Failed to write chunk to new zone");
    }

    // Update metadata
    new.id = old.id;
    new.in_use = true;
    int ret = zn_cq_add_chunk_to_lru(&p->chunk_queue, new);
    assert(!ret);
}

/** @brief Compacts the zone so that only valid chunks remain.

 *  Assumes that p->chunk_buf is already filled with the correct data.
 */
static void
zn_policy_compact_zone(struct zn_policy_chunk *p, uint32_t old_zone,
                       struct zn_pair *valid_chunks, uint32_t valid_length) {

    // Reset the old zone, making it writeable again
    while (g_atomic_int_get(&p->cache->active_readers[old_zone]) > 0) {}
    zsm_evict_and_write(&p->cache->zone_state, old_zone);

    // Each iteration of this loop will write a chunk from the old
    // offset into the ith offset, compressing the data in the zone.
    for (uint32_t i = 0; i < valid_length; i++) {
        struct zn_pair old_chunk = valid_chunks[i];
        struct zn_pair new_chunk = old_chunk;
        new_chunk.chunk_offset = i;
        new_chunk.in_use = true;
	
	zn_policy_write_to_zone(p, old_chunk, new_chunk);
	zn_cachemap_insert(&p->cache->cache_map, new_chunk.id, new_chunk);
    }

    struct zn_pair new_location = {
        .chunk_offset = valid_length - 1,
        .zone = old_zone,
    };
    int ret = zsm_return_active_zone(&p->cache->zone_state, &new_location);
    assert(!ret);
}

static void
zn_policy_chunk_gc(policy_data_t policy) {
    // TODO: If later separated from evict, lock here
    struct zn_policy_chunk *p = policy;

    uint32_t free_zones = zsm_get_num_free_zones(&p->cache->zone_state);
    if (free_zones > EVICT_HIGH_THRESH_ZONES) {
        return;
    }

    while (free_zones < EVICT_LOW_THRESH_ZONES) {
        uint32_t old_zone = 0;
        struct zn_pair* valid_chunks = NULL;
        uint32_t valid_length = 0;
        int no_zone = zn_cq_zone_dequeue(&p->chunk_queue, &old_zone,
                                         &valid_chunks, &valid_length);
        if (no_zone) {
            return;
        }

	for (uint32_t i = 0; i < valid_length; i++) {
	    zn_cachemap_invalidate(&p->cache->cache_map, valid_chunks[i].id, valid_chunks[i]);
	}

        zn_read_from_disk_whole(p->cache, old_zone, p->chunk_buf);
        for (uint32_t i = 0; i < valid_length; i++) {
	    struct zn_pair old_location = valid_chunks[i];
            struct zn_pair new_location;
            enum zsm_get_active_zone_error ret = zsm_get_active_zone(&p->cache->zone_state,
                                                                     &new_location);

            // Not enough zones available. We are just going to compact the old zone
            if (ret != ZSM_GET_ACTIVE_ZONE_SUCCESS) {
                zn_policy_compact_zone(p, old_zone,
                                       valid_chunks + i,
                                       valid_length - i);
                goto FREE_NEXT;
            }

            zn_policy_write_to_zone(p, old_location, new_location);
            ret = zsm_return_active_zone(&p->cache->zone_state, &new_location);
	    assert(!ret);
	    zn_cachemap_insert(&p->cache->cache_map, new_location.id, new_location);
        }

        // This just checks to make sure that the zone is empty
        zn_cachemap_clear_zone(&p->cache->cache_map, old_zone);

        // Reset the old zone
	while (g_atomic_int_get(&p->cache->active_readers[old_zone]) > 0) {}
        zsm_evict(&p->cache->zone_state, (int)old_zone);

    FREE_NEXT:
        free_zones = zsm_get_num_free_zones(&p->cache->zone_state);
	free(valid_chunks);
    }
}

// ========== Chunk public functions ==========
void
zn_policy_chunk_update(policy_data_t _policy, struct zn_pair location,
                             enum zn_io_type io_type) {
    struct zn_policy_chunk *p = _policy;
    assert(p);

    g_mutex_lock(&p->policy_mutex);

    dbg_printf("State before chunk update%s", "\n");

    dbg_print_g_queue("lru_queue (zone,chunk,id,in_use)", &p->chunk_queue.lru_queue, PRINT_G_QUEUE_ZN_PAIR);
    dbg_print_g_hash_table("chunk_to_lru_map (id,zone,chunk,in_use)", p->chunk_queue.chunk_to_lru_map, PRINT_G_HASH_TABLE_ZN_PAIR_NODE);

    if (io_type == ZN_WRITE) {
        zn_cq_add_chunk_to_lru(&p->chunk_queue, location);
    } else if (io_type == ZN_READ) {
	zn_cq_update_chunk_in_lru(&p->chunk_queue, location);
    }

    dbg_printf("State after chunk update%s", "\n");
    dbg_print_g_queue("lru_queue (zone,chunk,id,in_use)", &p->chunk_queue.lru_queue, PRINT_G_QUEUE_ZN_PAIR);
    dbg_print_g_hash_table("chunk_to_lru_map (id,zone,chunk,in_use)", p->chunk_queue.chunk_to_lru_map, PRINT_G_HASH_TABLE_ZN_PAIR_NODE);

    g_mutex_unlock(&p->policy_mutex);
}

int
zn_policy_chunk_evict(policy_data_t policy) {
    struct zn_policy_chunk *p = policy;

    gboolean locked_by_us = g_mutex_trylock(&p->policy_mutex);
    if (!locked_by_us) {
        return -1;
    }

    uint32_t in_lru = zn_cq_lru_len(&p->chunk_queue);
    uint32_t free_chunks = p->total_chunks - in_lru;

    if ((in_lru == 0) || (free_chunks > EVICT_HIGH_THRESH_CHUNKS)) {
        g_mutex_unlock(&p->policy_mutex);
        return 1;
    }

    uint32_t free_zones = zsm_get_num_free_zones(&p->cache->zone_state);

    uint32_t nr_evict = EVICT_LOW_THRESH_CHUNKS-free_chunks;

    dbg_printf("Evicting %u chunks\n", nr_evict);

    // We meet thresh for eviction - evict
    for (uint32_t i = 0; i < nr_evict; i++) {
        struct zn_pair chunk;
        zn_cq_invalidate_latest_chunk(&p->chunk_queue, &chunk);
        
        // Update ZSM, cachemap
        zsm_mark_chunk_invalid(&p->cache->zone_state, &chunk);
        zn_cachemap_clear_chunk(&p->cache->cache_map, &chunk);

        // TODO: SSD look at invalid (not here, on write)
    }

    in_lru = zn_cq_lru_len(&p->chunk_queue);
    free_chunks = p->total_chunks - in_lru;
    dbg_printf("Free chunks=%u, Chunks in lru=%u, EVICT_HIGH_THRESH_CHUNKS=%u\n",
               free_chunks, in_lru, EVICT_HIGH_THRESH_CHUNKS);

    // Do GC
    zn_policy_chunk_gc(p);

    free_zones = zsm_get_num_free_zones(&p->cache->zone_state);
    dbg_printf("Free zones after evict=%u\n", free_zones);

    g_mutex_unlock(&p->policy_mutex);

    return 0;
}
