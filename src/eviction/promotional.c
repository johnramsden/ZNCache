#include "eviction_policy.h"
#include "eviction_policy_promotional.h"
#include "glib.h"
#include "glibconfig.h"
#include "znutil.h"

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>

void
zn_policy_promotional_update(policy_data_t _policy, struct zn_pair location,
                             enum zn_io_type io_type) {
    struct zn_policy_promotional *policy = _policy;
    assert(policy);

    g_mutex_lock(&policy->policy_mutex);
    assert(policy->zone_to_lru_map);

    gpointer zone_ptr = GUINT_TO_POINTER(location.zone);

    dbg_printf("State before promotional update%s", "\n");

    dbg_print_g_queue("lru_queue", &policy->lru_queue, PRINT_G_QUEUE_GINT);
    dbg_print_g_hash_table("zone_to_lru_map", policy->zone_to_lru_map, PRINT_G_HASH_TABLE_PROM_LRU_NODE);

    // We only add zones to the LRU when they are full.
    if (io_type == ZN_WRITE && location.chunk_offset == policy->zone_max_chunks-1) {
        g_queue_push_tail(&policy->lru_queue, zone_ptr);
        GList *node = g_queue_peek_tail_link(&policy->lru_queue);
        g_hash_table_insert(policy->zone_to_lru_map, zone_ptr, node);
    } else if (io_type == ZN_READ) {

        GList *node = g_hash_table_lookup(policy->zone_to_lru_map, zone_ptr);
        if (node) {
            gpointer data = node->data;
            g_queue_delete_link(&policy->lru_queue, node);
            g_queue_push_tail(&policy->lru_queue, data);
            // Replace in map, pointer invalid after destroying link
            GList *new_node = g_queue_peek_tail_link(&policy->lru_queue);
            g_hash_table_replace(policy->zone_to_lru_map, zone_ptr, new_node);
        }

        // If lru_loc == NULL, the zone is not in the LRU queue. This
        // means that the zone is either not full, or has been removed
        // by the eviction thread while the read occurred. Don't do
        // anything
    }

    dbg_printf("State after promotional update%s", "\n");
    dbg_print_g_queue("lru_queue", &policy->lru_queue, PRINT_G_QUEUE_GINT);
    dbg_print_g_hash_table("zone_to_lru_map", policy->zone_to_lru_map, PRINT_G_HASH_TABLE_PROM_LRU_NODE);

    g_mutex_unlock(&policy->policy_mutex);
}

int
zn_policy_promotional_get_zone_to_evict(policy_data_t policy) {
    struct zn_policy_promotional *promote_policy = policy;

    gboolean locked_by_us = g_mutex_trylock(&promote_policy->policy_mutex);
    if (!locked_by_us) {
        return -1;
    }

    dbg_print_g_queue("lru_queue", &promote_policy->lru_queue, PRINT_G_QUEUE_GINT);

    if (g_queue_get_length(&promote_policy->lru_queue) == 0) {
		g_mutex_unlock(&promote_policy->policy_mutex);
        return -1;
    }

    // Remove from LRU and hash map
    uint32_t zone_id = GPOINTER_TO_UINT(g_queue_pop_head(&promote_policy->lru_queue));
    g_hash_table_replace(promote_policy->zone_to_lru_map, GUINT_TO_POINTER(zone_id), NULL);
    dbg_printf("Evicted zone=%u\n", zone_id);

    g_mutex_unlock(&promote_policy->policy_mutex);
    return zone_id;
}
