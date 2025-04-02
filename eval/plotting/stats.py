#!/usr/bin/env python3
import argparse
import csv
import statistics
import math
import numpy as np

def main():
    parser = argparse.ArgumentParser(
        description="Process a CSV file with exactly three columns: milliseconds, title, and data. " \
                    "Uses the middle column (index 1) as the title, calculates total run time (in minutes) " \
                    "from the first column's last entry, and computes statistics on the data (last column)."
    )
    parser.add_argument("csv_file", help="Path to the CSV file.")
    args = parser.parse_args()

    rows = []
    with open(args.csv_file, newline='') as csvfile:
        reader = csv.reader(csvfile)
        for row in reader:
            if len(row) != 3:
                print(f"Skipping row with unexpected number of columns: {row}")
                continue
            rows.append(row)

    if not rows:
        print("No valid data found in the CSV file.")
        return

    # Use the middle column (index 1) of the first row as the title.
    title = rows[0][1]

    # Calculate total run time in minutes from the very last row's first column (milliseconds).
    try:
        last_ms = float(rows[-1][0])
    except ValueError:
        print("Error: Could not convert the first column of the last row to a number.")
        return
    total_runtime_minutes = last_ms / 60000.0

    # Process the data from the last column (index 2)
    data = []
    for row in rows:
        try:
            value = float(row[2])
            data.append(value)
        except ValueError:
            print(f"Skipping row due to invalid data in the last column: {row}")
            continue

    if not data:
        print("No valid data found in the last column.")
        return

    # Compute arithmetic mean.
    mean_val = statistics.mean(data)

    # Compute geometric mean using only positive data values.
    positive_data = [x for x in data if x > 0]
    if positive_data:
        geo_mean = math.exp(sum(math.log(x) for x in positive_data) / len(positive_data))
    else:
        geo_mean = float('nan')

    # Compute standard deviation (sample standard deviation).
    std_dev = statistics.stdev(data) if len(data) > 1 else 0.0

    # Compute the 99th percentile (P99) of the data.
    p99 = np.percentile(data, 99)

    # Print the results.
    print(f"Title: {title}")
    print(f"Total Run Time: {total_runtime_minutes:.2f} minutes")
    print(f"Mean: {mean_val:.4f}")
    print(f"Geometric Mean: {geo_mean:.4f}")
    print(f"Standard Deviation: {std_dev:.4f}")
    print(f"P99: {p99:.4f}")

if __name__ == '__main__':
    main()
