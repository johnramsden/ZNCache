#pragma once

#include "glib.h"
#include "stdbool.h"
#include "cachemap.h"
#include "znbackend.h"

#include <stdint.h>

/**
 * @enum zn_zone_condition
 * @brief Defines possible conditions of a cache zone.
 *
 * Zones transition between these states based on their usage and availability.
 */
enum zn_zone_condition {
    ZN_ZONE_FREE = 0,   /**< The zone is available for new allocations. */
    ZN_ZONE_FULL = 1,   /**< The zone is completely occupied and cannot accept new data. */
    ZN_ZONE_ACTIVE = 2, /**< The zone is currently in use and may still have space for new data. */
    ZN_ZONE_WRITE_OCCURING = 3, /**< The zone is currently being written to. */
};

/**
 * @enum zsm_get_active_zone_error
 * @brief Error types for zsm_get_active_zone
 */
enum zsm_get_active_zone_error {
    ZSM_GET_ACTIVE_ZONE_SUCCESS = 0,  /**< Success */
    ZSM_GET_ACTIVE_ZONE_RETRY = 2,    /**< Thread needs to retry later */
    ZSM_GET_ACTIVE_ZONE_ERROR = 3,    /**< Error occurred */
    ZSM_GET_ACTIVE_ZONE_EVICT = 4     /**< Thread needs to evict */
};

/**
 * @struct zn_zone
 * @brief Stores the state of a zone.
 */
struct zn_zone {
    enum zn_zone_condition state;
    uint32_t zone_id;
    uint32_t chunk_offset;
    GQueue *invalid; /**< Invalidated chunks, used after filled on SSD */
};

/**
 * @struct zone_state_manager
 * @brief Stores the state of all zones on a ZNS SSD.
 */
struct zone_state_manager {
    GMutex state_mutex; /**< The lock protecting this data structure */
    GQueue *active;     /**< The queue of zones that are currently active. Stores pointers to zn_zones. */
    GQueue *free;       /**< The queue of zones that are free. Stores pointers to zn_zones. */
    struct zn_zone *state; /**< An array that stores the state of each zone, and acts as the backing
    memory for the active and free queues. */
    int writes_occurring;  /**< The current number of writes occuring on active zones */

    // Information about the cache
    int fd;                       /**< File descriptor of the SSD */
    uint64_t zone_cap;            /**< Maximum storage capacity per zone in bytes. */
    uint64_t zone_size;           /**< Storage size per zone in bytes. */
    size_t chunk_size;            /**< Size of each chunk in bytes. */
    uint32_t max_nr_active_zones; /**< Maximum number of zones that can be active at once. */
    uint64_t max_zone_chunks;     /**< Maximum amount of chunks that a zone can store */
    uint32_t num_zones;           /**< Number of zones */
	enum zn_backend backend_type; /**< The type of backend */
};

/**
 * @brief Performs setup for the zone_state subsystem.
 *
 * @param[out] state Pointer to the `zone_state_manager` structure to be initialized.
 * @param[in]  num_zones Number of zones on the disk
 * @param[in]  fd file descriptor of the disk
 * @param[in]  zone_cap capacity of the zone in bytes
 * @param[in]  zone_size size of the zone in bytes
 * @param[in]  chunk_size size of the chunk in bytes
 * @param[in]  backend_type the type of SSD that is backing the zones
 *
 */
void
zsm_init(struct zone_state_manager *state, const uint32_t num_zones, const int fd,
         const uint64_t zone_cap, const uint64_t zone_size, const size_t chunk_size,
         const uint32_t max_nr_active_zones,
         const enum zn_backend backend_type);

/** @brief Returns a new chunk that a thread can write to
 *  @param[in]  state zone_state data structure
 *  @param[out] pair the new location to write to
 *  @return 0 on success, 1 indicates that the thread needs to retry later, and -1 on failure
 *  Implementation notes:
 *  - Gets an active zone if it can, otherwise get from the free list (and move it to the active
 * list)
 *  - Increment the corresponding chunk pointer to point to the next free zone
 *  - If chunk pointer reaches the end, move zone to full list
 */
enum zsm_get_active_zone_error
zsm_get_active_zone(struct zone_state_manager *state, struct zn_pair *pair);

/** @brief Not yet thought out well, but a function for host-side gc (when we need to relocate a
 * number of chunks)
 *  @param chunks specifies how many chunks we need
 *  @return which chunks can be written to, in the form of a dynamic array
 *  Implementation notes:
 *  - Updates the zone state accordingly
 */
GArray
zsm_get_active_zone_batch(int chunks);


/** @brief Returns the active zone after it's written to.
 *  @param pair the chunk that was written to.
 */
int
zsm_return_active_zone(struct zone_state_manager *state, struct zn_pair *pair);

/** @brief Moves full zones to the free zone to make them available again
 *  @param zone_to_free the zone to make free again
 *  Implementation notes
 *  - Should be the one to perform the freeing operation
 *  - Does not manage zone eviction policy
 *  @return 0 if no error, -1 otherwise
 */
int
zsm_evict(struct zone_state_manager *state, int zone_to_free);

/** @brief Resets the zone pointer, making it writeable again from the start. Once the thread is finished updating the zone, it should call zsm_return_active_zone.
 *  @param zone_id the zone to make free again
 *  @param count the number of chunks that are still valid.
 */
void
zsm_evict_and_write(struct zone_state_manager *state, uint32_t zone_id, uint32_t count);

void
zsm_failed_to_write(struct zone_state_manager *state, struct zn_pair pair);

/** @brief Returns the active zone count */
uint32_t
zsm_get_num_active_zones(struct zone_state_manager *state);

/** @brief Returns the free zone count */
uint32_t
zsm_get_num_free_zones(struct zone_state_manager *state);

/** @brief Returns the full zone count */
uint32_t
zsm_get_num_full_zones(struct zone_state_manager *state);

/** @brief Mark a chunk as invalid */
void
zsm_mark_chunk_invalid(struct zone_state_manager *state, struct zn_pair *location);

/** @brief Returns invalid chunks in a zone */
uint32_t
zsm_get_num_invalid_chunks(struct zone_state_manager *state, uint32_t zone);
