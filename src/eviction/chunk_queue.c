#include "chunk_queue.h"
#include "glib.h"
#include "minheap.h"
#include "znbackend.h"
#include "znutil.h"

#include <assert.h>
#include <stdint.h>

int
zn_cq_init_chunk_queue(struct chunk_queue *cq, uint32_t nr_zones,
                       uint32_t max_zone_chunks) {

    cq->max_zone_chunks = max_zone_chunks;

    cq->chunk_to_lru_map = g_hash_table_new(g_direct_hash, g_direct_equal);

    cq->zone_pool = g_new(struct eviction_policy_chunk_zone, nr_zones);
    if (!cq->zone_pool) {
        return 1;
    }

    for (uint32_t z = 0; z < nr_zones; z++) {
        cq->zone_pool[z].chunks_in_use = 0;
        cq->zone_pool[z].filled = false;
        cq->zone_pool[z].chunks = g_new(struct zn_pair, max_zone_chunks);
        assert(cq->zone_pool[z].chunks);
        for (uint32_t c = 0; c < max_zone_chunks; c++) {
            cq->zone_pool[z].chunks[c].chunk_offset = 0;
            cq->zone_pool[z].chunks[c].in_use = false;
            assert(g_hash_table_insert(cq->chunk_to_lru_map,
				       &cq->zone_pool[z].chunks[c],
				       NULL
				       ));
        }
    }

    cq->invalid_pqueue = zn_minheap_init(nr_zones);
    if (!cq->invalid_pqueue) {
        return 1;
    }

    g_queue_init(&cq->lru_queue);

    return 0;
}

int
zn_cq_add_chunk_to_lru(struct chunk_queue *cq, struct zn_pair chunk) {
    assert(cq);

    struct zn_pair * chunk_key = &cq->zone_pool[chunk.zone].chunks[chunk.chunk_offset];
    struct eviction_policy_chunk_zone * zone_pool_entry = &cq->zone_pool[chunk.zone];

    assert(!chunk_key->in_use);
    *chunk_key = chunk;
    chunk_key->in_use = true;
    // v--- Need to update here on SSD incase invalidated then re-written
    zone_pool_entry->chunks_in_use++;

    // Update the lru queue
    g_queue_push_tail(&cq->lru_queue, chunk_key);

    // Update the hash table to point towards the LRU queue entry
    GList *node = g_queue_peek_tail_link(&cq->lru_queue);
    g_hash_table_insert(cq->chunk_to_lru_map, chunk_key, node);

    // We only add zones to the minheap when they are full.
    if (chunk.chunk_offset == cq->max_zone_chunks - 1) {
        dbg_printf("Adding %p (zone=%u) to pqueue\n", (void *)chunk_key, chunk.zone);
        zone_pool_entry->pqueue_entry =
            zn_minheap_insert(cq->invalid_pqueue, zone_pool_entry, zone_pool_entry->chunks_in_use);
        assert(zone_pool_entry->pqueue_entry);
        assert(zone_pool_entry->chunks_in_use == cq->max_zone_chunks);
        zone_pool_entry->filled = true;
    }

    return 0;
}

int
zn_cq_update_chunk_in_lru(struct chunk_queue *cq, struct zn_pair chunk) {
    assert(cq);

    GList *node = NULL;
    struct zn_pair* chunk_key = &cq->zone_pool[chunk.zone].chunks[chunk.chunk_offset];

    assert(g_hash_table_lookup_extended(cq->chunk_to_lru_map, chunk_key, NULL, (gpointer *) &node));

    if (node) {
	gpointer data = node->data;
	g_queue_delete_link(&cq->lru_queue, node);
	g_queue_push_tail(&cq->lru_queue, data);
	GList *new_node = g_queue_peek_tail_link(&cq->lru_queue);
	g_hash_table_replace(cq->chunk_to_lru_map, chunk_key, new_node);
    }

    // If node->data == NULL, the chunk is not in the LRU queue. This
    // is not necessarily an error because we may have invalidated it
    // just before this.

    return 0;
}

int
zn_cq_invalidate_latest_chunk(struct chunk_queue* cq, struct zn_pair *out) {
    assert(cq);
    assert(out);

    // Remove from queue and hash table
    out = g_queue_pop_head(&cq->lru_queue);
    g_hash_table_replace(cq->chunk_to_lru_map, out, NULL);

    // Invalidate chunk, update zone_pool info
    cq->zone_pool[out->zone].chunks[out->chunk_offset].in_use = false;
    cq->zone_pool[out->zone].chunks_in_use--;

    // Update priority
    zn_minheap_update_by_entry(cq->invalid_pqueue,
			       cq->zone_pool[out->zone].pqueue_entry,
			       cq->zone_pool[out->zone].chunks_in_use);
    return 0;
}

int
zn_cq_zone_dequeue(struct chunk_queue *cq, uint32_t *out_zone,
                   struct zn_pair** valid_chunks, uint32_t *valid_length) {
    assert(cq);
    assert(out_zone);
    assert(valid_chunks);
    assert(valid_length);

    // Get the zone to be freed
    struct zn_minheap_entry *ent = zn_minheap_extract_min(cq->invalid_pqueue);
    if (!ent) {
        return 1;
    }
    struct eviction_policy_chunk_zone * old_zone = ent->data;
    assert(old_zone);

    *out_zone = old_zone->zone_id;
    *valid_chunks = malloc(sizeof(struct zn_pair) * cq->max_zone_chunks);
    valid_length = 0;

    // Remove all chunks in the zone from the LRU queue and the hash table
    // Additionally create the list of valid chunks
    for (uint32_t i = 0; i < cq->max_zone_chunks; i++) {
        struct zn_pair *chunk_key = &old_zone->chunks[i];

        GList *node = g_hash_table_lookup(cq->chunk_to_lru_map, chunk_key);
        if (node) {
            g_queue_delete_link(&cq->lru_queue, node);

	    (*valid_chunks)[*valid_length] = *chunk_key;
	    *valid_length += 1;
        }

        g_hash_table_replace(cq->chunk_to_lru_map, chunk_key, NULL);

	chunk_key->id = 0;
	chunk_key->in_use = false;
    }
    
    // Update the entry on the zone itself
    old_zone->pqueue_entry = NULL;
    old_zone->chunks_in_use = 0;
    old_zone->filled = false;

    return 0;
}

uint32_t
zn_cq_lru_len(struct chunk_queue * cq) {
    return g_queue_get_length(&cq->lru_queue);
}

