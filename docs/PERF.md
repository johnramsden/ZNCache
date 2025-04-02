# PERF

Compile with: `-g`

Profile with `perf`:

```shell
sudo perf record -F 99 -g -- ./buildDir/src/zncache
```

`run_workloads.sh -p` will automatically profile.

Generate flamegraph:

```
perf script -i INPUT.perf > out.perf
stackcollapse-perf.pl out.perf > out.folded
./flamegraph.pl out.folded > flamegraph.svg
```

## Symbols

Add dbgsyms for better perf data [Ubuntu wiki](https://documentation.ubuntu.com/server/reference/debugging/debug-symbol-packages/index.html)

update and install:

```
sudo apt-get update
sudo apt-get install linux-image-$(uname -r)-dbgsym
```
