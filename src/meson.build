#Get the sources

# DONT FORGET TO ADD NEW FILES TO TEST MESON
srcs = files(
    'zncache.c',
    'cache.c',
    'znutil.c',
    'cachemap.c',
    'znprofiler.c',
    'zone_state_manager.c',
    'eviction_policy.c',
    'minheap.c',
    'eviction/promotional.c',
    'eviction/chunk.c',
    'eviction/chunk_queue.c'
)

executable('zncache',
           srcs,
           include_directories : inc_dir,
           c_args : cflags,
           dependencies : [ zbd_lib, dependency('glib-2.0') ]
)