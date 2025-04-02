#!/bin/sh

# Check that we received exactly two arguments:
# 1. Comma-delimited directories.
# 2. Comma-delimited labels.
if [ "$#" -ne 2 ]; then
    echo "Usage: $0 comma_delimited_directories comma_delimited_labels"
    exit 1
fi

CSV_DIRS="$1"
LABELS="$2"

# Function to join a given filename to each directory.
join_paths() {
    file_name="$1"
    dirs="$2"
    result=""
    # Set IFS to comma for splitting the directories.
    IFS=','
    for d in $dirs; do
        # Trim any whitespace (if needed) and build the path.
        dir=$(echo "$d" | xargs)
        if [ -z "$result" ]; then
            result="${dir}/${file_name}"
        else
            result="${result},${dir}/${file_name}"
        fi
    done
    echo "$result"
}

# Each Python command now uses join_paths() to build a comma-delimited list of CSV file paths.
python bytes.py "$(join_paths "CACHE_USED_MIB.csv" "$CSV_DIRS")" \
    --yunits "GiB" \
    --inunits "MiB" \
    --title "Cache Used" \
    --type "line" \
    --yaxis "Cache Used (GiB)" \
    --labels "$LABELS"

python bytes.py "$(join_paths "CACHETHROUGHPUT.csv" "$CSV_DIRS")" \
    --yunits "GiB" \
    --inunits "B" \
    --title "Cache Throughput" \
    --type "line" \
    --yaxis "Cache Throughput (GiB/s)" \
    --labels "$LABELS"

python bytes.py "$(join_paths "CACHEMISSTHROUGHPUT.csv" "$CSV_DIRS")" \
    --yunits "GiB" \
    --inunits "B" \
    --title "Cache Miss Throughput" \
    --type "line" \
    --yaxis "Cache Miss Throughput (GiB/s)" \
    --labels "$LABELS"

python bytes.py "$(join_paths "CACHEHITTHROUGHPUT.csv" "$CSV_DIRS")" \
    --yunits "GiB" \
    --inunits "B" \
    --title "Cache Hit Throughput" \
    --type "line" \
    --yaxis "Cache Hit Throughput (GiB/s)" \
    --labels "$LABELS"

python lat.py "$(join_paths "CACHEHITLATENCY.csv" "$CSV_DIRS")" \
    --title "Cache Hit Latency" \
    --type "line" \
    --yaxis "Hit Latency (ms)" \
    --labels "$LABELS"

python lat.py "$(join_paths "CACHEMISSLATENCY.csv" "$CSV_DIRS")" \
    --title "Cache Miss Latency" \
    --type "line" \
    --yaxis "Miss Latency (ms)" \
    --labels "$LABELS"

python lat.py "$(join_paths "GETLATENCY.csv" "$CSV_DIRS")" \
    --title "Cache Get Latency" \
    --type "line" \
    --yaxis "Get Latency (ms)" \
    --labels "$LABELS"

python lat.py "$(join_paths "GETLATENCY_EVERY.csv" "$CSV_DIRS")" \
    --title "Cache Get Latency (Individual)" \
    --type "scatter" \
    --yaxis "Get Latency (ms)" \
    --labels "$LABELS"

python lat.py "$(join_paths "READLATENCY.csv" "$CSV_DIRS")" \
    --title "Cache Read Latency" \
    --type "line" \
    --yaxis "Read Latency (ms)" \
    --labels "$LABELS"

python lat.py "$(join_paths "READLATENCY_EVERY.csv" "$CSV_DIRS")" \
    --title "Cache Read Latency (Individual)" \
    --type "scatter" \
    --yaxis "Read Latency (ms)" \
    --labels "$LABELS"

python lat.py "$(join_paths "WRITELATENCY.csv" "$CSV_DIRS")" \
    --title "Cache Write Latency" \
    --type "line" \
    --yaxis "Write Latency (ms)" \
    --labels "$LABELS"

python lat.py "$(join_paths "WRITELATENCY_EVERY.csv" "$CSV_DIRS")" \
    --title "Cache Write Latency (Individual)" \
    --type "scatter" \
    --yaxis "Write Latency (ms)" \
    --labels "$LABELS"

python plot.py "$(join_paths "FREEZONES.csv" "$CSV_DIRS")" \
    --title "Free Zones" \
    --type "line" \
    --yaxis "Free Zones" \
    --labels "$LABELS"

python plot.py "$(join_paths "HITRATIO.csv" "$CSV_DIRS")" \
    --title "Hitratio" \
    --type "line" \
    --yaxis "Hit-ratio" \
    --labels "$LABELS"
