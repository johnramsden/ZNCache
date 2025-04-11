#!/bin/bash
# shellcheck disable=SC2154

set -e

# Get the directory in which this script is located:
SCRIPT_DIR="$(cd "$(dirname "$0")" || exit 1; pwd)"

usage() {
    printf "Usage: %s [-p] DEVICE WORKLOAD_DIR NR_THREADS\n" "$(basename "$0")"
    exit 1
}

# Default value for the profile flag
profile=false

# Parse options. The ":p" means that -p is a valid flag.
while getopts ":p" opt; do
    case $opt in
        p)
            profile=true
            ;;
        \?)
            echo "Invalid option: -$OPTARG" >&2
            usage
            ;;
    esac
done

# Remove the parsed options from the positional parameters
shift $((OPTIND - 1))

# Check that exactly 3 positional arguments remain
if [ "$#" -ne 3 ]; then
    echo "Illegal number of parameters $#, should be 3"
    usage
fi

device="$1"
directory="$2"
threads="$3"

echo "Device: $device"
echo "Workload Directory: $directory"
echo "Threads: $threads"
echo "Logging to: ./logs"
echo "Profile flag: $profile"

ret=0

# Loop through all files in the directory
for file in "$directory"/*.bin; do
    # Check if it's a file (not a directory)
    if ! [ -f "$file" ]; then
        echo "ERROR: no such file '$file', skipping"
        continue
    fi
    filename=$(basename ${file})
    # Split the filename into an array using comma as the delimiter
    IFS=, read -ra params <<< "${filename%.*}"

    # Iterate over each key-value pair
    for param in "${params[@]}"; do
        # Extract the key and value by removing the longest matching suffix and prefix respectively
        key="${param%%=*}"
        value="${param#*=}"
        # Declare the variable globally in the script
        declare -g "$key"="$value"
    done

    runfile="./logs/$(date '+%Y-%m-%d_%H:%M:%S')-run"
    # Now you can access the variables
    {
        echo "Chunk Size: $chunk_size"
        echo "Latency: $latency"
        echo "Distribution Type: $distr"
        echo "Working Set Ratio: $ratio"
        echo "Zone Size: $zone_size"
        echo "Iterations: $iterations"
        echo "Number of Zones: $n_zones"
        echo "Total Chunks: $chunks"
        echo "High Water: $evict_high"
        echo "Low water: $evict_low"
        echo "Eviction $eviction"
        echo "Device $device"
    } | tee "$runfile"

    echo >> "$runfile"

    # shellcheck disable=SC2024
    echo "Running $runfile"

    # debugsymbols=true to profile
    meson setup --reconfigure buildDir -Dverify=false \
                                       -Ddebugging=false \
                                       -Ddebugsymbols="$profile" \
                                       -DREAD_SLEEP_US="$latency" \
                                       -DEVICT_HIGH_THRESH_ZONES="$evict_high" \
                                       -DEVICT_LOW_THRESH_ZONES="$evict_low" \
                                       -DEVICT_HIGH_THRESH_CHUNKS="$evict_high" \
                                       -DEVICT_LOW_THRESH_CHUNKS="$evict_low" \
                                       -DEVICTION_POLICY="$eviction" \
                                       -DPROFILING_INTERVAL_SEC="60" \
                                       -DMAX_ZONES_USED="$n_zones" \
                                       -DASSERTS=false \
                                        >/dev/null
    meson compile -C buildDir >/dev/null

    if [ "$device" = "/dev/nvme1n1p1" ]; then
        echo "Pre-conditioning SSD"
        "${SCRIPT_DIR}/precondition-nvme1n1.sh" "${n_zones}"
    fi

    # shellcheck disable=SC2024
    if $profile; then
        sudo perf record -F 99 -g -o "$runfile.perf" -- ./buildDir/src/zncache "$1" "$chunk_size" "$threads" -w "$file" -i "$iterations" -m "$runfile.profile.csv" >> "$runfile"
        ret=$?
    else
        sudo ./buildDir/src/zncache "$1" "$chunk_size" "$threads" -w "$file" -i "$iterations" -m "$runfile.profile.csv" >> "$runfile"
        ret=$?
    fi
    if [ $ret -ne 0 ]; then
        echo "Run FAILED!"
        ret=1
    else
        tail -n3 "$runfile"
    fi
done

exit $ret
