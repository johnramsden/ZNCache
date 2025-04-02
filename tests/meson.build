project_tests = [
    'minheap', 'minheap_concurrent', 'chunk_eviction'
]

test_cflags = [
    '-DBLOCK_ZONE_CAPACITY=' + BLOCK_ZONE_CAPACITY.to_string(),
    '-DZN_READ_SLEEP_US=' + READ_SLEEP_US.to_string(),
    '-DPROFILING_INTERVAL_SEC=' + PROFILING_INTERVAL_SEC.to_string(),
    '-DEVICTION_POLICY=' + EVICTION_POLICY,
    '-DEVICT_HIGH_THRESH_ZONES=' + EVICT_HIGH_THRESH_ZONES.to_string(),
    '-DEVICT_LOW_THRESH_ZONES=' + EVICT_LOW_THRESH_ZONES.to_string(),
    '-DEVICT_HIGH_THRESH_CHUNKS=' + EVICT_HIGH_THRESH_CHUNKS.to_string(),
    '-DEVICT_LOW_THRESH_CHUNKS=' + EVICT_LOW_THRESH_CHUNKS.to_string(),
    '-DEVICT_INTERVAL_US=' + EVICT_INTERVAL_US.to_string(),
    '-DMAX_ZONES_USED=' + MAX_ZONES_USED.to_string(),
    '-D_POSIX_C_SOURCE=199309L', # CLOCK_MONO
]

foreach test_name : project_tests
    src = files(
        meson.project_source_root() + '/src/cache.c',
        meson.project_source_root() + '/src/znutil.c',
        meson.project_source_root() + '/src/cachemap.c',
        meson.project_source_root() + '/src/znprofiler.c',
        meson.project_source_root() + '/src/zone_state_manager.c',
        meson.project_source_root() + '/src/eviction_policy.c',
        meson.project_source_root() + '/src/minheap.c',
        meson.project_source_root() + '/src/eviction/promotional.c',
        meson.project_source_root() + '/src/eviction/chunk.c',
        test_name + '.c'
    )
    test_exe = executable(test_name, src,
                          include_directories : inc_dir,
                          c_args : test_cflags,
                          dependencies : [ zbd_lib, dependency('glib-2.0') ],
                          install: true)
    test(test_name, test_exe)
endforeach
