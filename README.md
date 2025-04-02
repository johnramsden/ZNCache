# ZNWorkload

Test workloads for ZNS

## Setup

Clone repo:

```shell
git clone https://github.com/johnramsden/ZNWorkload.git --recursive
```

Compile dependencies:

```shell
./build-deps.sh
```

Compile project:

```shell
meson setup buildDir
meson compile -C buildDir
```

There are various variables that can be set: (defaults in `meson_options.txt`)

* `debugging`: Enables debug output (default true)
* `debugsymbols`: Enables debug symbols
* `verify`: Enables correctness verification (default true)
* `BLOCK_ZONE_CAPACITY`: Sets SSD zone size (default 1077MiB 1129316352)
* `READ_SLEEP_US`: Read delay to simulate remote data (default 40430us)
* `PROFILING_INTERVAL_SEC`: Interval to print metrics on (averaged) (default 10)
* `PROFILER_PRINT_EVERY`: Print metrics on every call, not just at interval (default true)
* `EVICT_HIGH_THRESH_ZONES`: High water mark for zone eviction
* `EVICT_LOW_THRESH_ZONES`: Low water mark for zone eviction
* `EVICT_HIGH_THRESH_CHUNKS`: High water mark for chunk eviction
* `EVICT_LOW_THRESH_CHUNKS`: Low water mark for chunk eviction
* `EVICT_INTERVAL_US`: Sleep time between evictions (us) (default 100,000, or 0.1s)
* `EVICTION_POLICY`: (`ZN_EVICT_PROMOTE_ZONE`, `ZN_EVICT_CHUNK`) Eviction policy, default `ZN_EVICT_PROMOTE_ZONE`
* `MAX_ZONES_USED`: Set maximum zones to use (default 0 means all)

To modify these:

```shell
meson setup --reconfigure buildDir -Dverify=true -Ddebugging=true -DBLOCK_ZONE_CAPACITY=1048576
meson compile -C buildDir
```

## Testing and Development

Create an emulated ZNS device via `scripts/nullblk-zones.sh` with:

* sector size: 4096B
* zone size: 32MiB
* conventional zones (non seq writes): 0
* sequential zones: 100

```shell
./scripts/nullblk.sh 4096 32 0 100 "zns" # or ssd
```

```
Created /dev/nullb0
```

To destroy:

```shell
./scripts/nullblk-zoned-delete.sh 0 # Replace 0 with ID if different
```

# Workloads

For detailed experiment reproduction, see [WORKLOADS](docs/WORKLOADS.md)

### Min-workload

For mini-test:

* 14 zones
* 8chunks per zone
* `2*14=28 is capacity`
* Start evict at 2 zones free: `24` entries
* On evict, evict 4 zones: `20` entries remain

ZNS:

```shell
./scripts/nullblk.sh 4096 1 0 14 "zns"
```

On SSD:

```shell
./scripts/nullblk.sh 4096 1 0 14 "ssd"
```

```shell
./zncache /dev/nullb0 524288 2
```

This means 2chunks to fill a zone: `1024*1024/2`

### Documentation

Run `doxygen`:

```shell
doxygen Doxyfile
```

Open `docs/html/index.html` in a browser.

### Formatting

Run `clang-format`:

```shell
clang-format -i src/ze_cache.c
```
