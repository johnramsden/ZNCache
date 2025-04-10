#pragma once

#include <stddef.h>

#include "znbackend.h"

// Forward declare zn_cache to avoid cyclic dependency
struct zn_cache;

#include <stdint.h>

/**
 * @enum zn_io_type
 * @brief Defines the type of IO done
 */
enum zn_io_type {
    ZN_READ = 0,
    ZN_WRITE = 1,
};

/**
 * @enum zn_eviction_policy
 * @brief Defines eviction policies
 */
enum zn_evict_policy_type {
    ZN_EVICT_ZONE = 0,         /**< Zone granularity eviction. */
    ZN_EVICT_PROMOTE_ZONE = 1, /**< Zone granularity eviction with promotion. */
    ZN_EVICT_CHUNK = 2,        /**< Chunk granularity eviction. */
};

/** Policy specific data */
typedef void *policy_data_t;

/** A generic policy update function */
typedef void (*update_policy_t)(policy_data_t policy, struct zn_pair location,
                                enum zn_io_type io_type);

/** A generic eviction function informed by the policy */
typedef int (*do_evict)(policy_data_t policy);

/** @struct zn_evict_policy
    @brief generic policy type
 */
struct zn_evict_policy {
    enum zn_evict_policy_type type; /**< Eviction policy. */
    policy_data_t data;             /**< Opaque data handle */
    update_policy_t update_policy;  /**< Called when policy needs to be updated */
    do_evict
        do_evict;  /**< Called when eviction thread needs to evict something */
};

/** @brief Sets up the data structure for the selected eviction policy.
 */
void
zn_evict_policy_init(struct zn_evict_policy *policy, enum zn_evict_policy_type type, struct zn_cache *cache);

/** @brief Get LRU size
 */
size_t
zn_evict_policy_get_cache_size(struct zn_evict_policy *policy);
