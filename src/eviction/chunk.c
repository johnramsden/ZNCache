#include "eviction_policy.h"
#include "eviction_policy_chunk.h"
#include "znutil.h"
#include "minheap.h"
#include "zone_state_manager.h"

#include <assert.h>
#include <stdint.h>
#include <stdbool.h> // Cortes
#include <stdlib.h>
#include <glib.h>
#include <glibconfig.h>

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

    GList *node;
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

        // If node->data == NULL, the zone is not in the LRU queue. This
        // means that the zone is either not full, or has been removed
        // by the eviction thread while the read occurred. Don't do
        // anything
    }



    dbg_printf("State after chunk update%s", "\n");
    dbg_print_g_queue("lru_queue (zone,chunk,id,in_use)", &p->lru_queue, PRINT_G_QUEUE_ZN_PAIR);
    dbg_print_g_hash_table("chunk_to_lru_map (id,zone,chunk,in_use)", p->chunk_to_lru_map, PRINT_G_HASH_TABLE_ZN_PAIR_NODE);

    g_mutex_unlock(&p->policy_mutex);
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

        // Naive?
        for (uint32_t i = 0; i < p->cache->max_zone_chunks; i++) {
            if (!old_zone->chunks[i].in_use) {
                continue;
            }

            struct zn_pair new_location;
            enum zsm_get_active_zone_error ret = zsm_get_active_zone(&p->cache->zone_state, &new_location);
            if (ret != ZSM_GET_ACTIVE_ZONE_SUCCESS) {
                assert(!"TODO");
                // TODO: ???
            }

            // Read the chunk from the old zone
            unsigned char *data = zn_read_from_disk(p->cache, &old_zone->chunks[i]);
            assert(data);

            // Write the chunk to the new zone
            unsigned long long wp = CHUNK_POINTER(p->cache->zone_size, p->cache->chunk_sz,
                                                  new_location.chunk_offset, new_location.zone);
            if (zn_write_out(p->cache->fd, p->cache->chunk_sz, data, p->cache->chunk_sz, wp) != 0) {
                assert(!"Failed to write chunk to new zone");
            }

            // Update the cache map
            zn_cachemap_insert(&p->cache->cache_map, old_zone->chunks[i].id, new_location); // Add new mapping

            // Update the eviction policy metadata
            old_zone->chunks[i].in_use = false;
            old_zone->chunks_in_use--;

            // Update the new zone's metadata
            struct eviction_policy_chunk_zone *new_zone = &p->zone_pool[new_location.zone];
            new_zone->chunks[new_location.chunk_offset] = old_zone->chunks[i];
            new_zone->chunks[new_location.chunk_offset].in_use = true;
            new_zone->chunks_in_use++;

            // Update the LRU queue
            g_queue_push_tail(&p->lru_queue, &new_zone->chunks[new_location.chunk_offset]);

            // Free the data buffer
            free(data);
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
