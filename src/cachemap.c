#include "cachemap.h"
#include "zncache.h"

#include "assert.h"
#include "glib.h"
#include "glibconfig.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <znutil.h>

void
zn_cachemap_init(struct zn_cachemap *map, const int num_zones, gint *active_readers_arr) {
    g_mutex_init(&map->cache_map_mutex);

    map->zone_map = g_hash_table_new(g_direct_hash, g_direct_equal);
    assert(map->zone_map);

    map->data_map = g_new(GHashTable *, num_zones);
    assert(map->data_map);

    // Zone → Data ID
    for (int i = 0; i < num_zones; i++) {
        map->data_map[i] = g_hash_table_new(g_direct_hash, g_direct_equal); // Chunk -> data ID
        assert(map->data_map[i]);
    }

    map->active_readers = active_readers_arr;
}

static void
free_cond_var(GCond *cond) {
    g_cond_clear(cond);
}

struct zone_map_result
zn_cachemap_find(struct zn_cachemap *map, const uint32_t data_id) {
    assert(map);

    g_mutex_lock(&map->cache_map_mutex);

    // Loop for spurious wakeups
    while (true) {

        // We found an entry
        if (g_hash_table_contains(map->zone_map, GINT_TO_POINTER(data_id))) {

            struct zone_map_result *lookup =
                g_hash_table_lookup(map->zone_map, GINT_TO_POINTER(data_id));

	    switch (lookup->type) {
            case RESULT_LOC:
                g_atomic_int_inc(&map->active_readers[lookup->location.zone]);
                g_mutex_unlock(&map->cache_map_mutex);
                return *lookup;                
            case RESULT_COND:
                g_cond_wait(&lookup->write_finished, &map->cache_map_mutex);
                break;
            case RESULT_EMPTY:
                lookup->type = RESULT_COND;
                g_mutex_unlock(&map->cache_map_mutex);
		return *lookup;
                break;
            default:
                assert(FALSE);
            }

        } else { // The thread needs to write an entry.
            struct zone_map_result *wait_cond = g_new0(struct zone_map_result, 1);
            wait_cond->type = RESULT_COND;
            g_cond_init(&wait_cond->write_finished);
            g_hash_table_insert(map->zone_map, GINT_TO_POINTER(data_id), wait_cond);
            g_mutex_unlock(&map->cache_map_mutex);
            return *wait_cond;
        }
    };
}

void
zn_cachemap_insert(struct zn_cachemap *map, const uint32_t data_id, struct zn_pair location) {
    assert(map);

    g_mutex_lock(&map->cache_map_mutex);

    dbg_print_g_hash_table("map->data_map[location.zone]", map->data_map[location.zone], PRINT_G_HASH_TABLE_GINT);

    // It must contain an entry if the thread called zn_cachemap_find beforehand
    assert(g_hash_table_contains(map->zone_map, GUINT_TO_POINTER(data_id)));

    struct zone_map_result *result = g_hash_table_lookup(map->zone_map, GUINT_TO_POINTER(data_id));
    assert(result->type == RESULT_COND);

    result->location = location; // Does this mutate the entry in the hash table?
    result->type = RESULT_LOC;
    assert(map->data_map[location.zone]);
    g_hash_table_insert(map->data_map[location.zone], GUINT_TO_POINTER(location.chunk_offset), GINT_TO_POINTER(data_id));
    g_cond_broadcast(&result->write_finished);            // Wake up threads waiting for it

    dbg_print_g_hash_table("map->data_map[location.zone]", map->data_map[location.zone], PRINT_G_HASH_TABLE_GINT);

    g_mutex_unlock(&map->cache_map_mutex);
}

void
zn_cachemap_clear_chunk(struct zn_cachemap *map, struct zn_pair *location) {
    assert(map);

    g_mutex_lock(&map->cache_map_mutex);

    dbg_print_g_hash_table("map->data_map[location.zone] before", map->data_map[location->zone], PRINT_G_HASH_TABLE_GINT);

    dbg_printf("Looking up zone=%u, chunk=%u\n", location->zone, location->chunk_offset);
    int data_id = GPOINTER_TO_INT(g_hash_table_lookup(map->data_map[location->zone], GUINT_TO_POINTER(location->chunk_offset)));

    dbg_printf("Got data_id=%d\n", data_id);

    assert(g_hash_table_contains(map->zone_map, GINT_TO_POINTER(data_id)));
    struct zone_map_result *res = g_hash_table_lookup(map->zone_map, GINT_TO_POINTER(data_id));
    assert(res->type == RESULT_LOC);
    assert(res->value.location.zone == location->zone);
    assert(res->value.location.chunk_offset == location->chunk_offset);

    // Erase the entry and free the zone_map_result memory
    g_hash_table_remove(map->zone_map, GINT_TO_POINTER(data_id));
    g_free(res);

    g_hash_table_remove(map->data_map[location->zone], GUINT_TO_POINTER(location->chunk_offset));

    dbg_print_g_hash_table("map->data_map[location.zone] after", map->data_map[location->zone], PRINT_G_HASH_TABLE_GINT);

    g_mutex_unlock(&map->cache_map_mutex);
}

void
zn_cachemap_clear_zone(struct zn_cachemap *map, uint32_t zone) {
    assert(map);

    g_mutex_lock(&map->cache_map_mutex);

    GHashTableIter iter;
    gpointer key, value;

    g_hash_table_iter_init(&iter, map->data_map[zone]);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        int data_id = GPOINTER_TO_INT(value);
        assert(g_hash_table_contains(map->zone_map, GINT_TO_POINTER(data_id)));
        struct zone_map_result *res = g_hash_table_lookup(map->zone_map, GINT_TO_POINTER(data_id));
        assert(res->type == RESULT_LOC);
        assert(res->value.location.zone == zone);

        // Erase the entry and free the zone_map_result memory
        g_hash_table_remove(map->zone_map, GINT_TO_POINTER(data_id));
        g_free(res);
    }

    g_hash_table_remove_all(map->data_map[zone]);

    g_mutex_unlock(&map->cache_map_mutex);
}

void
zn_cachemap_fail(struct zn_cachemap *map, const uint32_t id) {
    g_mutex_lock(&map->cache_map_mutex);

    assert(g_hash_table_contains(map->zone_map, GINT_TO_POINTER(id)));
    struct zone_map_result *entry = g_hash_table_lookup(map->zone_map, GINT_TO_POINTER(id));
    assert(entry->type == RESULT_COND);
    g_cond_broadcast(&entry->write_finished);            // Wake up threads waiting for it
    entry->type = RESULT_EMPTY;
    g_mutex_unlock(&map->cache_map_mutex);
}
