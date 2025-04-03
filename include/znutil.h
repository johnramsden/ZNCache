#ifndef UTIL_H
#define UTIL_H

#define SEED 42

#include "libzbd/zbd.h"

#include "zncache.h"
#include <time.h> // clock macro

#include <glib.h>

/* Will only print messages (to stdout) when DEBUG is defined */
#ifdef DEBUG
#    define dbg_printf(M, ...) printf("%s: " M, __func__, ##__VA_ARGS__)
#else
#    define dbg_printf(...)
#endif

// Get write pointer from (zone, chunk)
#define CHUNK_POINTER(z_sz, c_sz, c_num, z_num)                                                    \
    (((uint64_t) (z_sz) * (uint64_t) (z_num)) + ((uint64_t) (c_num) * (uint64_t) (c_sz)))

enum print_g_queue_type {
    PRINT_G_QUEUE_GINT = 0,
    PRINT_G_QUEUE_ZN_ZONE = 1,
    PRINT_G_QUEUE_ZN_PAIR = 2,
};

enum print_g_hash_table_type {
    PRINT_G_HASH_TABLE_GINT = 0,
    PRINT_G_HASH_TABLE_PROM_LRU_NODE = 1,
    PRINT_G_HASH_TABLE_ZN_PAIR = 2,
    PRINT_G_HASH_TABLE_ZN_PAIR_NODE = 3,
    PRINT_G_HASH_TABLE_ZONE_MAP_RESULT = 4,
};

/**
 * @brief Prints all key-value pairs in a GHashTable.
 *
 * This function assumes that the GHashTable uses integer keys and values
 * stored as GINT_TO_POINTER. The keys are pointers to integers, and
 * the values are stored as integer pointers.
 *
 * @param hash_table A pointer to the GHashTable to print.
 */
void
print_g_hash_table(char *name, GHashTable *hash_table, enum print_g_hash_table_type type);

#ifdef DEBUG
#    define dbg_print_g_hash_table(name, hash_table, type) print_g_hash_table(name, hash_table, type)
#else
#    define dbg_print_g_hash_table(...)
#endif

/**
 * @brief Prints all elements in a GQueue.
 *
 * This function assumes that the GQueue stores integers using GINT_TO_POINTER.
 *
 * @param queue A pointer to the GQueue to print.
 */
void
print_g_queue(char *name, const GQueue *queue, enum print_g_queue_type type);

#ifdef DEBUG
#    define dbg_print_g_queue(name, queue, type) print_g_queue(name, queue, type)
#else
#    define dbg_print_g_queue(...)
#endif

/**
 * @brief Generates a buffer filled with random bytes.
 *
 * This function allocates a buffer of the specified size and fills it with
 * random values ranging from 0 to 255. The random number generator is seeded
 * with a predefined value (`SEED`), which may result in deterministic output
 * unless `SEED` is properly randomized elsewhere.
 *
 * @param size The number of bytes to allocate and populate with random values.
 * @return A pointer to the allocated buffer containing random bytes, or NULL
 *         if allocation fails or size is zero.
 *
 * @note The caller is responsible for freeing the allocated buffer.
 */
unsigned char *
generate_random_buffer(size_t size);

/**
 * @brief Exit if NOMEM
 */
void
nomem();

/**
 * @brief Prints information about a Zoned Block Device (ZBD).
 *
 * This function outputs detailed information about a given `zbd_info` structure,
 * including vendor details, sector counts, zone properties, and model type.
 *
 * @param info Pointer to a `struct zbd_info` containing ZBD details.
 *
 * @cite https://github.com/westerndigitalcorporation/libzbd/blob/master/include/libzbd/zbd.h
 */
void
print_zbd_info(struct zbd_info *info);

/**
 * @brief Get zone capacity
 *
 * @param[in] fd open zone file descriptor
 * @param[out] zone_cap zone capacity
 * @return non-zero on error
 */
int
zone_cap(int fd, uint64_t *zone_capacity);

void
print_zn_pair_list(struct zn_pair *list, uint32_t len);

#ifdef DEBUG
#    define dbg_print_zn_pair_list(list, len) print_zn_pair_list(list, len)
#else
#    define dbg_print_zn_pair_list(...)
#endif

// Timing

#define TIME_NOW(_t) (clock_gettime(CLOCK_MONOTONIC, (_t)))

#define TIME_DIFFERENCE_SEC(_start, _end)                                                          \
    ((_end.tv_sec + _end.tv_nsec / 1.0e9) - (_start.tv_sec + _start.tv_nsec / 1.0e9))

#define TIME_DIFFERENCE_MILLISEC(_start, _end)                                                     \
    ((_end.tv_nsec / 1.0e6 < _start.tv_nsec / 1.0e6)) ?                                            \
        ((_end.tv_sec - 1.0 - _start.tv_sec) * 1.0e3 + (_end.tv_nsec / 1.0e6) + 1.0e3 -            \
         (_start.tv_nsec / 1.0e6)) :                                                               \
        ((_end.tv_sec - _start.tv_sec) * 1.0e3 + (_end.tv_nsec / 1.0e6) -                          \
         (_start.tv_nsec / 1.0e6))

#define TIME_DIFFERENCE_NSEC(_start, _end)                                                         \
    ((_end.tv_nsec < _start.tv_nsec)) ?                                                            \
        ((_end.tv_sec - 1 - (_start.tv_sec)) * 1e9 + _end.tv_nsec + 1e9 - _start.tv_nsec) :        \
        ((_end.tv_sec - (_start.tv_sec)) * 1e9 + _end.tv_nsec - _start.tv_nsec)

#define BYTES_TO_MIB(bytes) ((double)(bytes) / (1024.0 * 1024.0))

#endif // UTIL_H
