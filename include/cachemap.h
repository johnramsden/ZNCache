#pragma once

#include "znbackend.h"
#include "glib.h"
#include <stdint.h>

/**
 * @struct zn_cachemap
 *
 * @brief A map for finding where data is stored on the disk based on the data ID.
 * Keeps track of two things:
 *  1. Data ID → (Zone ID, chunk pointer)
 *  2. Zone ID → Data ID
 */
struct zn_cachemap {
    GMutex cache_map_mutex;
    GHashTable *zone_map;
    GHashTable **data_map;  /**< Zone ID → GHashTable (chunk -> Data ID) */
    gint *active_readers;   /**< Non-owning reference to the number of currently active readers per zone. */
};

void
zn_cachemap_init(struct zn_cachemap *map, const int num_zones, gint *active_readers_arr);

/**
 * @struct zone_map_result
 * @brief The returned result from a search of the data ID in the cache map
 * This is a type with two possible values:
 * 1. It contains a zn_pair, which represents the location on disk
     where the data can be found
 * 2. It contains a condition variable, which new threads will wait on
        as this thread is tasked with writing the data to disk. The
        thread should signal once it is finished.
 */
struct zone_map_result {
    union {
        struct zn_pair location;
        GCond *write_finished;
    } value;

    enum { RESULT_LOC = 0, RESULT_COND = 1 } type;
};

/** @brief Finds the data in the zone if it exists, otherwise returns additional information for
    writing to a zone
 *  @param data_id the element to find
 *  @return result indicating where to find the data
 *  If there doesn't exist the data id on disk, the cache will instead return:
 *  - A condition variable with a message indicating that the thread
 *       needs to write and then signal this later
 *	- When a reader requests to read, we need to increment the active
 *       reader count on behalf of them
 *
 * This function should sleep on a condition variable when it finds it
 *      in the cache (indicating that a thread is currently writing the
 *      data to disk). When it is woken up, it should try again to see
 *      if the data exists in the cache map.
 */
struct zone_map_result
zn_cachemap_find(struct zn_cachemap *map, const uint32_t data_id);

/** @brief Begin compaction of a zone. This temporarily renders all chunks in that zone unavailable
    until the compaction operation is finished, after which zn_cachemap_compact_end should be
called.
 *  @param[in] map the cachemap
 *  @param[in] zone_id the zone that will be compacted
 *  @param[out] data_ids an out pointer for an array. This outputs the list of data IDs that were associated with the zone.
 *  @param[out] locations an out pointer for an array. This outputs the list of locations (most importantly are the chunk locations) that were associated with the zone.
 *  @param[out] count an out pointer for the number of chunks that are still valid in the zone.
 */
void
zn_cachemap_compact_begin(struct zn_cachemap *map, const uint32_t zone_id, uint32_t **data_ids,
                          struct zn_pair **locations, uint32_t *count);

/** @brief Finish compaction of a zone. This temporarily renders all chunks in that zone unavailable
    until the compaction operation is finished, after which zn_cachemap_compact_end should be
called.
 *  @param[in] map the cachemap
 *  @param[in] zone_id the zone that was compacted
 *  @param[in] data_ids An array that stores the list of data IDs that were associated with the zone.
 *  @param[in] locations An array that stores the list of locations that were associated with the zone.
 *  @param[in] count the number of chunks that were still valid in the zone.
 */
void
zn_cachemap_compact_end(struct zn_cachemap *map, const uint32_t zone_id, const uint32_t* data_ids, struct zn_pair* locations, uint32_t count);

/** @brief Inserts a new mapping into the data structure. Called by
 * the thread when it's finished writing to the zone.
 *
 * @param data_id id of the data to be inserted
 * @param location the location on disk where the data lives
 * @return void
 *
 * Implementation notes:
 * - Additionally inserts the mapping into the Zone ID → Data ID map
 * - We could abstract away the cond variable logic from the thread
 *     here by signalling when the thread calls this function.
 */
void
zn_cachemap_insert(struct zn_cachemap *map, const uint32_t data_id, struct zn_pair location);

/** @brief Clears all entries of a zone in the mapping. Called by eviction threads.
 * @param zone the zone
   to clear
 * @return void
 * Implementation notes:
 *   - Additionally clears the Zone ID → Data ID map
 */
void
zn_cachemap_clear_chunk(struct zn_cachemap *map, struct zn_pair *location);

/** @brief Clears all entries of a zone in the mapping. Called by eviction threads.
 * @param zone the zone
   to clear
 * @return void
 * Implementation notes:
 *   - Additionally clears the Zone ID → Data ID map
 */
void
zn_cachemap_clear_zone(struct zn_cachemap *map, uint32_t zone);

void
zn_cachemap_fail(struct zn_cachemap *map, const uint32_t id);
