#define _GNU_SOURCE
#include <assert.h>
#include <libzbd/zbd.h>
#include <stdbool.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/fs.h>

#include "eviction_policy.h"
#include "znutil.h"
#include "zncache.h"

#define ACTIVE_READERS 1
#define CHUNK_SIZE 524288
#define NUM_DEV 1
#define WORKLOAD_SZ 28

unsigned char *RANDOM_DATA = NULL;

char *devices[NUM_DEV] = {
    "/dev/nullb0", // ZNS
    // "/dev/nullb1" // Conv
};

uint32_t workload[WORKLOAD_SZ] = {
    1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28 // ZONES FULL, FG GC
};

int
setup_dev(char *device, struct zn_cache *cfg) {

    struct zbd_info info = {0};
    uint64_t zone_capacity = 0;
    int fd;
    enum zn_backend backend = zbd_device_is_zoned(device) ? ZE_BACKEND_ZNS : ZE_BACKEND_BLOCK;
    if (backend == ZE_BACKEND_ZNS) {
        fd = zbd_open(device, O_RDWR | O_DIRECT | O_SYNC, &info);
        if (fd < 0) {
            fprintf(stderr, "Error opening device: %s\n", device);
            return fd;
        }

        int ret = zbd_reset_zones(fd, 0, 0);
        if (ret != 0) {
            fprintf(stderr, "Couldn't reset zones\n");
            return -1;
        }

        ret = zone_cap(fd, &zone_capacity);
        if (ret != 0) {
            fprintf(stderr, "Couldn't report zone info\n");
            return ret;
        }
    } else {
        fd = open(device, O_RDWR);
        if (fd < 0) {
            fprintf(stderr, "Error opening device: %s\n", device);
            return fd;
        }

        uint64_t size = 0;
        if (ioctl(fd, BLKGETSIZE64, &size) == -1) {
            fprintf(stderr, "Error: Couldn't get block size: %s\n", device);
            return -1;
        }

        if (size < BLOCK_ZONE_CAPACITY) {
            fprintf(stderr, "Error: The size of the disk is smaller than a single zone!\n");
            return -1;
        }
        info.nr_zones = ((long) size / BLOCK_ZONE_CAPACITY);
        info.max_nr_active_zones = 0;

        zone_capacity = BLOCK_ZONE_CAPACITY;
    }

	zn_init_cache(cfg, &info, CHUNK_SIZE, zone_capacity,
              fd, ZN_EVICT_CHUNK, backend, workload,
              WORKLOAD_SZ, NULL);

    return 0;
}

int
test_evict(struct zn_cache *cfg) {
    int failures = 0;
    for (uint32_t wi = 0; wi < WORKLOAD_SZ; wi++) {
        uint32_t data_id = workload[wi];
        unsigned char *data = zn_cache_get(cfg, data_id, RANDOM_DATA);
        if (data == NULL || zn_validate_read(cfg, data, data_id, RANDOM_DATA) != 0) {
            printf("TEST FAILED: Wrong data returned for workload[%u]=%u\n", wi, workload[wi]);
            return 1;
        }
    }

    // Now we are preloaded, check:
    uint32_t free_zones = zsm_get_num_free_zones(&cfg->zone_state);
    if (free_zones != 0) {
        printf("TEST FAILED: Free zones %u, expected 0\n", free_zones);
        failures++;
    }
    uint32_t full_zones = zsm_get_num_full_zones(&cfg->zone_state);
    if (full_zones != cfg->nr_zones) {
        printf("TEST FAILED: Full zones %u, expected %u\n", full_zones, cfg->nr_zones);
        failures++;
    }
    for (uint32_t z = 0; z < cfg->nr_zones; z++) {
        uint32_t invalid_chunks = zsm_get_num_invalid_chunks(&cfg->zone_state, z);
        if (invalid_chunks != 0) {
            printf("TEST FAILED: Invalid chunks=%u for zone %u, expected 0\n", invalid_chunks, z);
            failures++;
        }
    }

    // First GC
    uint32_t data_id = 29;
    unsigned char *data = zn_cache_get(cfg, data_id, RANDOM_DATA);
    if (data == NULL || zn_validate_read(cfg, data, data_id, RANDOM_DATA) != 0) {
        printf("TEST FAILED: Wrong data returned for id=%u\n", data_id);
        failures++;
    }

    // 3 because 14-4, add 1 chunk, 3 free
    free_zones = zsm_get_num_free_zones(&cfg->zone_state);
    uint32_t expect = EVICT_LOW_THRESH_ZONES-1;
    if (free_zones != expect) {
        printf("TEST FAILED: Free zones=%u, expected %u\n", free_zones, expect);
        failures++;
    }

    // Evicted 4
    full_zones = zsm_get_num_full_zones(&cfg->zone_state);
    expect = cfg->nr_zones-EVICT_LOW_THRESH_ZONES;
    if (full_zones != expect) {
        printf("TEST FAILED: Full zones=%u, expected %u\n", full_zones, expect);
        failures++;
    }

    // TODO: How many invalid? This seems wrong? If gc'd shouldn't still be valid?
    // uint32_t invalid_chunks = 0;
    // for (uint32_t z = 0; z < cfg->nr_zones; z++) {
    //     invalid_chunks += zsm_get_num_invalid_chunks(&cfg->zone_state, z);
    // }
    // if (invalid_chunks != EVICT_LOW_THRESH_CHUNKS) {
    //     printf("TEST FAILED: Invalid chunks %u != %u\n", invalid_chunks, EVICT_LOW_THRESH_CHUNKS);
    //     failures++;
    // }

    // Check correct data remains, should have evicted first


    return failures;
}


int main(void) {
    int failures = 0;

    RANDOM_DATA = generate_random_buffer(CHUNK_SIZE);
    if (RANDOM_DATA == NULL) {
        return 1;
    }

    struct zn_cache cfg[NUM_DEV];
    for (int i = 0; i < NUM_DEV; i++) {
        if (setup_dev(devices[i], &cfg[i]) != 0) {
            fprintf(stderr, "Error: Couldn't setup device %s\n", devices[i]);
            return 1;
        }
    }

    for (int i = 0; i < NUM_DEV; i++) {
        int f = test_evict(&cfg[i]);
        if (f == 0) {
            printf("TESTs PASSED for %s\n", devices[i]);
        } else {
            printf("TESTs FAILED (%u) for %s\n", f, devices[i]);
        }
        failures += f;
    }

    return failures;
}