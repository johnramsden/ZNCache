#include "eviction_policy.h"

#include "eviction_policy_promotional.h"
#include "eviction_policy_chunk.h"

#include <assert.h>
#include <glib.h>
#include <stdio.h>

#ifdef UNUSED
/**
 * Hash function for a zn_pair
 *
 * @param key Hashable zn_pair
 * @return Hashed zn_pair
 */
static guint
zn_pair_hash(gconstpointer key) {
    const struct zn_pair *p = key;

    guint hash_z = g_int_hash(&p->zone);
    guint hash_c = g_int_hash(&p->chunk_offset);

    /* Combine hashes via boost hash_combine method:
     * https://www.boost.org/doc/libs/1_43_0/doc/html/hash/reference.html#boost.hash_combine
     * seed ^= hash_value(v) + 0x9e3779b9 + (seed << 6) + (seed >> 2)
     */
    hash_z ^= hash_c + 0x9e3779b9 + (hash_z << 6) + (hash_z >> 2);

    return hash_z;
}

/**
 * Check for zn_pair equality
 *
 * @param a zn_pair 1
 * @param b  zn_pair 2
 * @return True if equal, else false
 */
static gboolean
zn_pair_equal(gconstpointer a, gconstpointer b) {
    const struct zn_pair *p1 = a;
    const struct zn_pair *p2 = b;
    return (p1->zone == p2->zone) && (p1->chunk_offset == p2->chunk_offset);
}
#endif

void
zn_evict_policy_init(struct zn_evict_policy *policy, enum zn_evict_policy_type type,
                     uint32_t zone_max_chunks, uint32_t nr_zones) {

    switch (type) {
        case ZN_EVICT_PROMOTE_ZONE: {
            struct zn_policy_promotional *data = malloc(sizeof(struct zn_policy_promotional));
            g_mutex_init(&data->policy_mutex);
            data->zone_to_lru_map = g_hash_table_new(g_direct_hash, g_direct_equal);
            assert(data->zone_to_lru_map);

            data->zone_max_chunks = zone_max_chunks;

            assert(data->zone_to_lru_map);
            g_queue_init(&data->lru_queue);

            *policy = (struct zn_evict_policy) {
                .type = ZN_EVICT_PROMOTE_ZONE,
                .data = data,
                .update_policy = zn_policy_promotional_update,
                .get_zone_to_evict = zn_policy_promotional_get_zone_to_evict,
            };
            break;
        }


        case ZN_EVICT_CHUNK: {
            struct zn_policy_chunk *data = malloc(sizeof(struct zn_policy_chunk));
            assert(data);

            // zn_pair to lru_map
            data->chunk_to_lru_map = g_hash_table_new(
                g_direct_hash, g_direct_equal
            );

            // Setup backing pool where zones marked not in use
            data->zone_pool = g_new(struct eviction_policy_chunk_zone, nr_zones);
            assert(data->zone_pool);
            for (uint32_t z = 0; z < nr_zones; z++) {
                data->zone_pool[z].chunks_in_use = 0;
                data->zone_pool[z].filled = false;
                data->zone_pool[z].chunks = g_new(struct zn_pair, zone_max_chunks);
                assert(data->zone_pool[z].chunks);
                for (uint32_t c = 0; c < zone_max_chunks; c++) {
                    data->zone_pool[z].chunks[c].chunk_offset = 0;
                    data->zone_pool[z].chunks[c].in_use = false;
                    assert(g_hash_table_insert(
                        data->chunk_to_lru_map,
                        &data->zone_pool[z].chunks[c],
                        NULL
                    ));
                }
            }

            data->invalid_pqueue = zn_minheap_init(nr_zones);
            assert(data->invalid_pqueue);

            g_mutex_init(&data->policy_mutex);

            data->zone_max_chunks = zone_max_chunks;
            assert(data->chunk_to_lru_map);

            g_queue_init(&data->lru_queue);

            *policy = (struct zn_evict_policy) {
                .type = ZN_EVICT_CHUNK,
                .data = data,
                .update_policy = zn_policy_chunk_update,
                .get_zone_to_evict = zn_policy_chunk_get_chunk_to_evict,
            };
            break;
        }

        case ZN_EVICT_ZONE: {
            fprintf(stderr, "NYI\n");
            exit(1);
        }
    }
}
