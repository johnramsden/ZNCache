project('ZNWorkload', 'c',
        version : '1.0.0',
        default_options : [
            'warning_level=3',
            'c_std=c17',
            # To test ASAN
            #                'b_sanitize=address'
        ])

# define zbd dep
zbd_lib = declare_dependency(
    include_directories: include_directories('vendor/lib/include'),
    link_args: ['-L../vendor/lib/lib', '-lzbd', '-Wl,-rpath,' + meson.current_source_dir() / 'vendor/lib/lib']
)

# Read options
verify_enabled = get_option('verify')
debug_enabled = get_option('debugging')
debugsymbols_enabled = get_option('debugsymbols')
BLOCK_ZONE_CAPACITY = get_option('BLOCK_ZONE_CAPACITY')
READ_SLEEP_US = get_option('READ_SLEEP_US')
PROFILER_PRINT_EVERY = get_option('PROFILER_PRINT_EVERY')
PROFILING_INTERVAL_SEC = get_option('PROFILING_INTERVAL_SEC')
EVICTION_POLICY = get_option('EVICTION_POLICY')
EVICT_HIGH_THRESH_ZONES = get_option('EVICT_HIGH_THRESH_ZONES')
EVICT_LOW_THRESH_ZONES = get_option('EVICT_LOW_THRESH_ZONES')
EVICT_HIGH_THRESH_CHUNKS = get_option('EVICT_HIGH_THRESH_CHUNKS')
EVICT_LOW_THRESH_CHUNKS = get_option('EVICT_LOW_THRESH_CHUNKS')
EVICT_INTERVAL_US = get_option('EVICT_INTERVAL_US')
MAX_ZONES_USED = get_option('MAX_ZONES_USED')

# Conditional compiler flags
cflags = [
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

if PROFILER_PRINT_EVERY
    cflags += ['-DZN_PROFILER_PRINT_EVERY']
endif

if verify_enabled
    cflags += ['-DVERIFY']
endif

if debug_enabled
    cflags += ['-DDEBUG', '-g']
elif debugsymbols_enabled
    cflags += ['-g']
endif

# Print options for debugging purposes
message('Verify mode: ' + verify_enabled.to_string())
message('Debug mode: ' + debug_enabled.to_string())

# Define the include directory
inc_dir = include_directories('include')

# Add the src subdirectory
subdir('src')
subdir('tests')
