#include "cachemap.h"
#include "eviction_policy.h"
#include "eviction_policy_chunk.h"
#include "znbackend.h"
#include "zncache.h"
#include "znutil.h"
#include "minheap.h"
#include "zone_state_manager.h"

#include <assert.h>
#include <stdint.h>
#include <stdbool.h> // Cortes
#include <stdlib.h>
#include <glib.h>
#include <glibconfig.h>
#include <string.h>

void
zn_policy_chunk_update(policy_data_t _policy, struct zn_pair location,
                             enum zn_io_type io_type) {
    struct zn_policy_chunk *p = _policy;
    assert(p);

    g_mutex_lock(&p->policy_mutex);
    assert(p->chunk_to_lru_map);

    dbg_printf("State before chunk update%s", "\n");

    dbg_print_g_queue("lru_queue (zone,chunk,id,in_use)", &p->lru_queue, PRINT_G_QUEUE_ZN_PAIR);
    dbg_print_g_hash_table("chunk_to_lru_map (id,zone,chunk,in_use)", p->chunk_to_lru_map, PRINT_G_HASH_TABLE_ZN_PAIR_NODE);

    struct eviction_policy_chunk_zone * zpc = &p->zone_pool[location.zone];
    struct zn_pair * zp = &zpc->chunks[location.chunk_offset];

    GList *node = NULL;
    // Should always be present (might be NULL)
    assert(g_hash_table_lookup_extended(p->chunk_to_lru_map, zp, NULL, (gpointer *)&node));

    if (io_type == ZN_WRITE) {
        assert(!zp->in_use);
        zp->chunk_offset = location.chunk_offset;
        zp->zone = location.zone;
        zp->id = location.id;
        zp->in_use = true;
        zpc->chunks_in_use++; // Need to update here on SSD incase invalidated then re-written
        zpc->zone_id = location.zone;
        g_queue_push_tail(&p->lru_queue, zp);
        GList *node = g_queue_peek_tail_link(&p->lru_queue);
        g_hash_table_insert(p->chunk_to_lru_map, zp, node);

        if (location.chunk_offset == p->cache->max_zone_chunks-1) {
            // We only add zones to the minheap when they are full.
            dbg_printf("Adding %p (zone=%u) to pqueue\n", (void *)zp, location.zone);
            zpc->pqueue_entry = zn_minheap_insert(p->invalid_pqueue, zpc, zpc->chunks_in_use);
            assert(zpc->pqueue_entry);
            zpc->filled = true;
        }
    } else if (io_type == ZN_READ) {

        if (node) {
	    gpointer data = node->data;
	    g_queue_delete_link(&p->lru_queue, node);
	    g_queue_push_tail(&p->lru_queue, data);
	    GList *new_node = g_queue_peek_tail_link(&p->lru_queue);
	    g_hash_table_replace(p->chunk_to_lru_map, zp, new_node);
        }

        // If node->data == NULL, the chunk is not in the LRU queue.
    }



    dbg_printf("State after chunk update%s", "\n");
    dbg_print_g_queue("lru_queue (zone,chunk,id,in_use)", &p->lru_queue, PRINT_G_QUEUE_ZN_PAIR);
    dbg_print_g_hash_table("chunk_to_lru_map (id,zone,chunk,in_use)", p->chunk_to_lru_map, PRINT_G_HASH_TABLE_ZN_PAIR_NODE);

    g_mutex_unlock(&p->policy_mutex);
}

static gint
sort_locs(gconstpointer a, gconstpointer b, gpointer user_data) {
    (void) user_data;
    const struct zn_pair * loc_a = a;
    const struct zn_pair * loc_b = b;
    if (loc_a->chunk_offset < loc_b->chunk_offset) {
        return -1;
    } else {
	return 1;   
    }
}

/** @brief This function takes a full zone containing invalid chunks,
    and compacts it in-place so that only valid chunks remain.
 *
 * At a high level, this function copies all data from the zone,
   removes all invalid chunks, and makes the zone available again as
   an active zone.
 */
static void
zn_policy_compact_zone(struct zn_policy_chunk *p, struct eviction_policy_chunk_zone * old_zone) {
    unsigned char* buf = zn_read_from_disk_whole(p->cache, old_zone->zone_id, p->chunk_buf);
    assert(buf);

    // These arrays are allocated inside the function
    uint32_t* data_ids = NULL;
    struct zn_pair *locations = NULL;
    uint32_t count = 0;

    // Get information about the valid chunks
    zn_cachemap_compact_begin(&p->cache->cache_map, old_zone->zone_id, &data_ids, &locations, &count);
    
    // This is an annoying hack but we need to sort the locations and
    // data IDs by increasing chunk offsets. This is because we are
    // moving memory of chunks with later offsets into memory
    // locations of chunks with earlier offsets, so if we didn't sort,
    // we could be overwriting earlier chunks that appear later in the
    // locations array. Wirth sorting, on each iteration of the array
    // the new location is guaranteed to be smaller than the old
    // location so it will work properly
    for (uint32_t i = 0; i < count; i++) {
        locations[i].id = data_ids[i];
    }
    g_sort_array(locations, count, sizeof(struct zn_pair), sort_locs, NULL);

    // Wait for all readers to finish
    while (g_atomic_int_get(&p->cache->active_readers[old_zone->zone_id]) > 0) {}

    // Reset the current zone, making it writeable again
    zsm_evict_and_write(&p->cache->zone_state, old_zone->zone_id, count);

    struct eviction_policy_chunk_zone *zpc = &p->zone_pool[old_zone->zone_id];
    zpc->chunks_in_use = 0;
    zpc->filled = false;
    zpc->pqueue_entry = NULL; // We won't be full

    // Each iteration of this loop will write a chunk from the old
    // offset into the ith offset, compressing the data in the zone.
    for (uint32_t i = 0; i < count; i++) {
        // Read from the correct chunk offset indicated by the
	// variable location. Note that location is NOT the backing
	// memory for the LRU queue. This is just data about the old
	// chunk that was stored in the cachemap.
        struct zn_pair *old_chunk = &locations[i];

        // We use that data to get to the actual entry
        struct zn_pair *old_zpc_entry = &zpc->chunks[old_chunk->chunk_offset];
        struct zn_pair *new_zpc_entry = &zpc->chunks[i];
	
	assert(old_chunk->chunk_offset == old_zpc_entry->chunk_offset);
	assert(old_chunk->id == old_zpc_entry->id);
	assert(old_chunk->zone == old_zpc_entry->zone);
        
	unsigned char* read_ptr = &buf[p->cache->chunk_sz * old_chunk->chunk_offset];

	// Write to the ith sequential chunk
	unsigned long long wp =
	    CHUNK_POINTER(p->cache->zone_cap, p->cache->chunk_sz, i, old_chunk->zone);
	if (zn_write_out(p->cache->fd, p->cache->chunk_sz, read_ptr, ZN_WRITE_GRANULARITY, wp, p->cache->backend) != 0) {
            dbg_printf("Couldn't write to fd at wp=%llu\n", wp);
	}

	// Update the backing metadata for LRU queue. This involves
	// deleting the entry in the LRU queue and pointing its hash
	// table entry to NULL, then creating a new entry, adding it
	// to the queue, and updating the hashmap to point to the new
	// entry.

	// Create the new entry
	new_zpc_entry->chunk_offset = i;
	new_zpc_entry->id = old_chunk->id;
	new_zpc_entry->zone = old_chunk->zone;
	new_zpc_entry->in_use = true;

	// Find the old entry
	GList *node = NULL;
	// Should always be present and not NULL, because it
	// represented a valid location on disk
	assert(g_hash_table_lookup_extended(p->chunk_to_lru_map, old_zpc_entry, NULL, (gpointer *)&node));
	assert(node);

	// Delete the entry in the LRU queue and also its entry in the hashmap
	g_queue_delete_link(&p->lru_queue, node);
	g_hash_table_replace(p->chunk_to_lru_map, old_zpc_entry, NULL);

	// Add the new entry to the LRU queue and hash map
	g_queue_push_tail(&p->lru_queue, new_zpc_entry);
	GList *new_node = g_queue_peek_tail_link(&p->lru_queue);
	g_hash_table_insert(p->chunk_to_lru_map, new_zpc_entry, new_node);

	// Increment the chunks_in_use
	zpc->chunks_in_use += 1;
    }

    // Assign the rest to be free
    for (uint32_t i = count; i < p->cache->max_zone_chunks; i++) {
	zpc->chunks[i].in_use = false;
    }

    // Update the zsm. The latest pair that we wrote to was to the
    // (count - 1)th chunk (0 indexed), So indicate it as such in the
    // chunk_offset field.
    struct zn_pair end_pair = {
        .zone = old_zone->zone_id,
        .chunk_offset = count - 1,
    };
    int ret = zsm_return_active_zone(&p->cache->zone_state, &end_pair);
    assert(!ret);

    for (uint32_t i = 0; i < count; i++) {
        data_ids[i] = zpc->chunks[i].id;
    }

    // Finish the compaction, updating the cachemap
    zn_cachemap_compact_end(&p->cache->cache_map, old_zone->zone_id, data_ids,
                            zpc->chunks, count);

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
        struct zn_minheap_entry *ent = zn_minheap_extract_min(p->invalid_pqueue);
        if (!ent) {
            return;
        }

        struct eviction_policy_chunk_zone * old_zone = ent->data;
        assert(old_zone);

        dbg_printf("Found minheap_entry priority=%u, chunks_in_use=%u, zone=%u\n",
            ent->priority,  old_zone->chunks_in_use, old_zone->zone_id);
        dbg_printf("zone[%u] chunks:\n", old_zone->zone_id);
        dbg_print_zn_pair_list(old_zone->chunks, p->cache->max_zone_chunks);

        if (zsm_get_num_free_chunks(&p->cache->zone_state) < old_zone->chunks_in_use) {
            zn_policy_compact_zone(p, old_zone);
            return;
        }

        unsigned char *buf = zn_read_from_disk_whole(p->cache, old_zone->zone_id, p->chunk_buf);

        for (uint32_t i = 0; i < p->cache->max_zone_chunks; i++) {
            if (!old_zone->chunks[i].in_use) {
                continue;
            }

            struct zn_pair new_location;
            enum zsm_get_active_zone_error ret = zsm_get_active_zone(&p->cache->zone_state, &new_location);

            // Not enough zones available. We are just going to compact the old zone
            if (ret != ZSM_GET_ACTIVE_ZONE_SUCCESS) {
                // This needs to be updated
                zn_policy_compact_zone(p, old_zone);
                return;
            }

            // Read the chunk from the old zone
            unsigned char *data = &buf[i * p->cache->chunk_sz];

            // Write the chunk to the new zone
            unsigned long long wp = CHUNK_POINTER(p->cache->zone_size, p->cache->chunk_sz,
                                                  new_location.chunk_offset, new_location.zone);
            if (zn_write_out(p->cache->fd, p->cache->chunk_sz, data, ZN_WRITE_GRANULARITY, wp, p->cache->backend) != 0) {
                assert(!"Failed to write chunk to new zone");
            }

            // Update the cache map
            struct zn_pair old =
                zn_cachemap_atomic_replace(&p->cache->cache_map, old_zone->chunks[i].id,
                                           new_location); // Add new mapping
            assert(old.zone == old_zone->zone_id);
            assert(old.chunk_offset == i);
            assert(old.id == old_zone->chunks[i].id);
            assert(old.in_use == true);

            // Return the zone
            ret = zsm_return_active_zone(&p->cache->zone_state, &new_location);
            assert(!ret);

            // Update the eviction policy metadata
            old_zone->chunks[i].in_use = false;
            old_zone->chunks_in_use--;

            // Update the new zone's metadata
            struct eviction_policy_chunk_zone *new_zone = &p->zone_pool[new_location.zone];
            new_zone->chunks[new_location.chunk_offset].chunk_offset = new_location.chunk_offset;
            new_zone->chunks[new_location.chunk_offset].id = old.id;
            new_zone->chunks[new_location.chunk_offset].in_use = true;
	    new_zone->chunks[new_location.chunk_offset].zone = new_zone->zone_id;
            new_zone->chunks_in_use++;

	    // Find the old entry
	    GList *node = NULL;
	    // Should always be present and not NULL, because it
	    // represented a valid location on disk
	    assert(g_hash_table_lookup_extended(p->chunk_to_lru_map, &old_zone->chunks[old.chunk_offset], NULL, (gpointer *)&node));
	    assert(node);

	    // Delete the entry in the LRU queue and also its entry in the hashmap
	    g_queue_delete_link(&p->lru_queue, node);
	    g_hash_table_replace(p->chunk_to_lru_map, &old_zone->chunks[old.chunk_offset], NULL);

	    // Add the new entry to the LRU queue and hash map
	    g_queue_push_tail(&p->lru_queue, &new_zone->chunks[new_location.chunk_offset]);
	    GList *new_node = g_queue_peek_tail_link(&p->lru_queue);
	    g_hash_table_insert(p->chunk_to_lru_map, &new_zone->chunks[new_location.chunk_offset], new_node);

	    // Increment the chunks_in_use
	    new_zone->chunks_in_use += 1;;

        }

        zn_cachemap_clear_zone(&p->cache->cache_map, old_zone->zone_id);
        // Reset the old zone
        zsm_evict(&p->cache->zone_state, old_zone->zone_id);
        free_zones = zsm_get_num_free_zones(&p->cache->zone_state);
    }
}

int
zn_policy_chunk_evict(policy_data_t policy) {
    struct zn_policy_chunk *p = policy;

    gboolean locked_by_us = g_mutex_trylock(&p->policy_mutex);
    if (!locked_by_us) {
        return -1;
    }

    uint32_t in_lru = g_queue_get_length(&p->lru_queue);
    uint32_t free_chunks = p->total_chunks - in_lru;

    if ((in_lru == 0) || (free_chunks > EVICT_HIGH_THRESH_CHUNKS)) {
        g_mutex_unlock(&p->policy_mutex);
        return 1;
    }

    dbg_printf("State before chunk evict%s", "\n");
    dbg_print_g_queue("lru_queue (zone,chunk,id,in_use)", &p->lru_queue, PRINT_G_QUEUE_ZN_PAIR);
    dbg_print_g_hash_table("chunk_to_lru_map (id,zone,chunk,in_use)", p->chunk_to_lru_map, PRINT_G_HASH_TABLE_ZN_PAIR_NODE);
    uint32_t free_zones = zsm_get_num_free_zones(&p->cache->zone_state);
    (void)free_zones;

    dbg_printf("Free zones before evict=%u\n", free_zones);
    dbg_printf("Free chunks=%u, Chunks in lru=%u, EVICT_HIGH_THRESH_CHUNKS=%u\n",
           free_chunks, in_lru, EVICT_HIGH_THRESH_CHUNKS);

    uint32_t nr_evict = EVICT_LOW_THRESH_CHUNKS-free_chunks;

    dbg_printf("Evicting %u chunks\n", nr_evict);

    // We meet thresh for eviction - evict
    for (uint32_t i = 0; i < nr_evict; i++) {
        struct zn_pair * zp = g_queue_pop_head(&p->lru_queue);
        g_hash_table_replace(p->chunk_to_lru_map, zp, NULL);

        // Invalidate chunk
        p->zone_pool[zp->zone].chunks[zp->chunk_offset].in_use = false;
        p->zone_pool[zp->zone].chunks_in_use--;

        // Update priority
        zn_minheap_update_by_entry(
            p->invalid_pqueue,
            p->zone_pool[zp->zone].pqueue_entry,
            p->zone_pool[zp->zone].chunks_in_use
        );

        // Update ZSM, cachemap
        zsm_mark_chunk_invalid(&p->cache->zone_state, zp);
        zn_cachemap_clear_chunk(&p->cache->cache_map, zp);

        // TODO: SSD look at invalid (not here, on write)
    }

    dbg_printf("State after chunk evict%s\n", "");
    dbg_print_g_queue("lru_queue (zone,chunk,id,in_use)", &p->lru_queue, PRINT_G_QUEUE_ZN_PAIR);
    dbg_print_g_hash_table("chunk_to_lru_map (id,zone,chunk,in_use)", p->chunk_to_lru_map, PRINT_G_HASH_TABLE_ZN_PAIR_NODE);

    in_lru = g_queue_get_length(&p->lru_queue);
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
