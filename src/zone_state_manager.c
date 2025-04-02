#include "zone_state_manager.h"

#include "assert.h"
#include "glib.h"
#include "libzbd/zbd.h"
#include "zncache.h"
#include "znutil.h"

#include <stdint.h>
#include <stdlib.h>

/**
 * @brief Close a zone
 *
 * @param cache cache Pointer to the `zn_cache` structure, caller is responsible for locking
 * @param zone_id Zone to close
 *
 * @return Returns 0 on success and -1 otherwise.
 */
static int
close_zone(struct zone_state_manager *state, struct zn_zone *zone) {
    if (zone->state == ZN_ZONE_FULL) {
        return 0;
    }

    unsigned long long wp = CHUNK_POINTER(state->zone_size, state->chunk_size, 0, zone->zone_id);
    dbg_printf("Closing zone %u, zone pointer %llu\n", zone->zone_id, wp);
    zbd_set_log_level(ZBD_LOG_ERROR);

    // FOR DEBUGGING ZONE STATE
    // struct zbd_zone zone;
    // unsigned int nr_zones;
    // if (zbd_report_zones(cache->fd, wp, 1, ZBD_RO_ALL, &zone, &nr_zones) == 0) {
    //     printf("Zone state before close: %u\n", zone.cond == ZBD_ZONE_COND_FULL);
    // }

	int ret = 0;
    if (state->backend_type == ZE_BACKEND_ZNS) {
		// NOTE: FULL ZONES ARE NOT ACTIVE
		ret = zbd_finish_zones(state->fd, wp, state->zone_cap);
		if (ret != 0) {
			dbg_printf("Failed to close zone %u\n", zone->zone_id);
			return ret;
		}
	}

    // EXPLICIT CLOSE FAILS ON NULLBLK, TODO: TEST ON REAL DEV ON CORTES
    // ret = zbd_close_zones(cache->fd, wp, cache->zone_cap);
    // if (ret != 0) {
    //     return ret;
    // }
    zone->state = ZN_ZONE_FULL;
    zone->chunk_offset = 0;

    return ret;
}

/**
 * @brief Reset a zone
 *
 * @param state Pointer to the `zone_state_manager` structure, caller is responsible for locking
 * @param zone Zone to reset
 *
 * @return Returns 0 on success and -1 otherwise.
 */
static int
reset_zone(struct zone_state_manager *state, struct zn_zone *zone) {
    if (zone->state == ZN_ZONE_FREE) {
        dbg_printf("Zone %u already closed\n", zone->zone_id);
        return 0;
    }

    unsigned long long wp = CHUNK_POINTER(state->zone_size, state->chunk_size, 0, zone->zone_id);
    dbg_printf("Resetting zone %u, zone pointer %llu\n", zone->zone_id, wp);
    zbd_set_log_level(ZBD_LOG_ERROR);

    int ret = 0;
    if (state->backend_type == ZE_BACKEND_ZNS) {
		// NOTE: FULL ZONES ARE NOT ACTIVE
		ret = zbd_reset_zones(state->fd, wp, state->zone_cap);
		if (ret != 0) {
			dbg_printf("Failed to close zone %u\n", zone->zone_id);
			return ret;
		}
    }

    zone->state = ZN_ZONE_FREE;
    zone->chunk_offset = 0;
    g_queue_push_tail(state->free, zone);

    return ret;
}

/**
 * @brief Opens the free zone
 *
 * @param state the zone state
 * @param zone_id Zone to open
 *
 * @note assumes that the lock is held
 *
 * @return Returns 0 on success and -1 otherwise.
 */
static int
open_zone(struct zone_state_manager *state, struct zn_zone *zone) {
    assert(state);
    assert(zone);
    assert(zone->state == ZN_ZONE_FREE);

    if (g_queue_get_length(state->active) + state->writes_occurring >= state->max_nr_active_zones) {
        return -1;
    }

	if (state->backend_type == ZE_BACKEND_ZNS) {
        dbg_printf("chunk_offset=%u, zone=%u\n", zone->chunk_offset, zone->zone_id);
		unsigned long long wp = CHUNK_POINTER(state->zone_size, state->chunk_size, 0, zone->zone_id);
		dbg_printf("Opening zone %u, zone pointer %llu\n", zone->zone_id, wp);

		int ret = zbd_open_zones(state->fd, wp, 1);
		if (ret != 0) {
			return ret;
		}
    }

    zone->state = ZN_ZONE_ACTIVE;
    zone->chunk_offset = 0;
    g_queue_push_tail(state->active, zone);

    return 0;
}

void
zsm_init(struct zone_state_manager *state, const uint32_t num_zones, const int fd,
         const uint64_t zone_cap, const uint64_t zone_size, const size_t chunk_size,
         const uint32_t max_nr_active_zones,
         const enum zn_backend backend_type) {
    assert(state);
    state->fd = fd;
    state->zone_cap = zone_cap;
    state->zone_size = zone_size;
    state->chunk_size = chunk_size;
    state->max_zone_chunks = zone_cap / chunk_size;
    state->max_nr_active_zones = max_nr_active_zones;
    state->writes_occurring = 0;
    state->num_zones = num_zones;
    state->backend_type = backend_type;

    g_mutex_init(&state->state_mutex);

    state->active = g_queue_new();
    assert(state->active);

    state->free = g_queue_new();
    state->state = calloc(num_zones, sizeof(struct zn_zone));
    assert(state->free);
    assert(state->state);
    for (uint32_t i = 0; i < num_zones; i++) {
        GQueue *queue = g_queue_new();
        assert(queue);
        state->state[i] = (struct zn_zone) {
            .state = ZN_ZONE_FREE,
            .zone_id = i,
            .chunk_offset = 0,
            .invalid = queue
        };
        g_queue_push_tail(state->free, &state->state[i]);
    }
}

enum zsm_get_active_zone_error
zsm_get_active_zone(struct zone_state_manager *state, struct zn_pair *pair) {
    assert(state);
    assert(pair);

    g_mutex_lock(&state->state_mutex);

    uint32_t active_queue_size = g_queue_get_length(state->active);
    uint32_t writer_size = state->writes_occurring;
    uint32_t free_queue_size = g_queue_get_length(state->free);

    // Perform foreground eviction
    if ((active_queue_size + writer_size) == 0 && free_queue_size == 0) {
        g_mutex_unlock(&state->state_mutex);
        return ZSM_GET_ACTIVE_ZONE_EVICT;
    }

    assert((active_queue_size + writer_size) > 0 || free_queue_size > 0);

    // No active zones that we can use
    if (active_queue_size == 0) {
        uint32_t active_zones = g_queue_get_length(state->active) + state->writes_occurring;

        // Open a new zone if we can
        if (active_zones < state->max_nr_active_zones && free_queue_size > 0) {

            struct zn_zone *new_zone = g_queue_pop_head(state->free);
            assert(new_zone->state == ZN_ZONE_FREE);

            int ret = open_zone(state, new_zone);
            if (ret) {
                dbg_printf("Failed to open zone: %d with error: %d\n", new_zone->zone_id, ret);
                g_mutex_unlock(&state->state_mutex);
                return ZSM_GET_ACTIVE_ZONE_ERROR;
            }

        } else {
            // The thread needs to wait for a free zone
            g_mutex_unlock(&state->state_mutex);
            return ZSM_GET_ACTIVE_ZONE_RETRY;
        }
    }

    // Get an active zone
    dbg_print_g_queue("active queue (zone,chunk,state)", state->active, PRINT_G_QUEUE_ZN_ZONE);
    struct zn_zone *active_pair = g_queue_pop_head(state->active);
    assert(active_pair->state == ZN_ZONE_ACTIVE);

    *pair = (struct zn_pair) {
        .zone = active_pair->zone_id,
        .chunk_offset = active_pair->chunk_offset
    };

    active_pair->state = ZN_ZONE_WRITE_OCCURING;
    state->writes_occurring++;

    g_mutex_unlock(&state->state_mutex);
    return ZSM_GET_ACTIVE_ZONE_SUCCESS;
}

#ifdef TODO
// TODO
GArray
zsm_get_active_zone_batch(int chunks) {
    (void) chunks;
}
#endif

void
zsm_evict_and_write(struct zone_state_manager *state, uint32_t zone_id, uint32_t count) {
    assert(state);

    // Reset and open the zone.

    g_mutex_lock(&state->state_mutex);

    struct zn_zone *zone = &state->state[zone_id];
    assert(zone->state == ZN_ZONE_FULL);

    int ret = reset_zone(state, zone);
    assert(ret == 0);
    assert(g_queue_pop_tail(state->free) == zone);

    ret = open_zone(state, zone);
    assert(ret == 0);
    assert(g_queue_pop_tail(state->active) == zone);

    zone->state = ZN_ZONE_WRITE_OCCURING;
    zone->zone_id = zone_id;
    zone->chunk_offset = count - 1; // The last index that is active
    g_queue_clear(zone->invalid);

    state->writes_occurring++;

    g_mutex_unlock(&state->state_mutex);
}

int
zsm_return_active_zone(struct zone_state_manager *state, struct zn_pair *pair) {
    assert(state);
    assert(pair);

    g_mutex_lock(&state->state_mutex);
    assert(g_queue_get_length(state->active) + state->writes_occurring <=
           state->max_nr_active_zones);

    struct zn_zone *zone = &state->state[pair->zone];
    assert(zone->state == ZN_ZONE_WRITE_OCCURING);
    assert(zone->chunk_offset == pair->chunk_offset);

    // Update the state of the chunk
    state->writes_occurring--;
    zone->chunk_offset++;
    if (zone->chunk_offset == state->max_zone_chunks) {
        int ret = close_zone(state, zone);
        if (ret != 0) {
            dbg_printf("An error occurred while closing zone %u\n", zone->zone_id);
            g_mutex_unlock(&state->state_mutex);
            return ret;
        }
    } else {
        zone->state = ZN_ZONE_ACTIVE;
        g_queue_push_tail(state->active, zone);
    }

    g_mutex_unlock(&state->state_mutex);
    return 0;
}

int
zsm_evict(struct zone_state_manager *state, int zone_to_free) {
    assert(state);

    g_mutex_lock(&state->state_mutex);

    struct zn_zone *zone = &state->state[zone_to_free];
    assert(zone->state == ZN_ZONE_FULL);
	
	// TODO: I think we need to free the invalid queue
    g_queue_clear(zone->invalid); // This should do it... Right?
    int ret = reset_zone(state, zone);
    if (!ret) {
        g_mutex_unlock(&state->state_mutex);
        return ret;
    }

    assert(zone->state == ZN_ZONE_FREE);

    g_queue_clear(state->state[zone_to_free].invalid);

    g_mutex_unlock(&state->state_mutex);
    return 0;
}

void
zsm_failed_to_write(struct zone_state_manager *state, struct zn_pair pair) {
    assert(state);

    g_mutex_lock(&state->state_mutex);
    assert(g_queue_get_length(state->active) + state->writes_occurring <= state->max_nr_active_zones);

    struct zn_zone *zone = &state->state[pair.zone];
    assert(zone->state == ZN_ZONE_WRITE_OCCURING);
    assert(zone->chunk_offset == pair.chunk_offset);
    assert(zone->chunk_offset < state->max_zone_chunks);

    // Update the state of the chunk
    state->writes_occurring--;
    zone->state = ZN_ZONE_ACTIVE;
    g_queue_push_tail(state->active, zone);

    g_mutex_unlock(&state->state_mutex);
}

uint32_t
zsm_get_num_active_zones(struct zone_state_manager *state) {
    g_mutex_lock(&state->state_mutex);
    uint32_t len = g_queue_get_length(state->active) + state->writes_occurring;
    g_mutex_unlock(&state->state_mutex);
    return len;
}

uint32_t
zsm_get_num_free_zones(struct zone_state_manager *state) {
    g_mutex_lock(&state->state_mutex);
    uint32_t len = g_queue_get_length(state->free);
    g_mutex_unlock(&state->state_mutex);
    return len;
}

uint32_t
zsm_get_num_full_zones(struct zone_state_manager *state) {

    g_mutex_lock(&state->state_mutex);
    uint32_t count = 0;
    for (uint32_t i = 0; i < state->num_zones; i++) {
        if (state->state[i].state == ZN_ZONE_FULL) {
            count++;
        }
    }
    g_mutex_unlock(&state->state_mutex);

    return count;
}

uint32_t
zsm_get_num_invalid_chunks(struct zone_state_manager *state, uint32_t zone) {
    g_mutex_lock(&state->state_mutex);
    uint32_t len = g_queue_get_length(state->state[zone].invalid);
    g_mutex_unlock(&state->state_mutex);
    return len;
}

void
zsm_mark_chunk_invalid(struct zone_state_manager *state, struct zn_pair *location) {
    g_mutex_lock(&state->state_mutex);
    struct zn_zone *zone = &state->state[location->zone];
    dbg_print_g_queue(
        "state[location.zone].state->invalid before mark",
        zone->invalid,
        PRINT_G_QUEUE_GINT
    );
    g_queue_push_tail(zone->invalid, GUINT_TO_POINTER(location->chunk_offset));
    dbg_print_g_queue(
        "state[location.zone].state->invalid after mark",
        zone->invalid,
        PRINT_G_QUEUE_GINT
    );
    g_mutex_unlock(&state->state_mutex);
}
