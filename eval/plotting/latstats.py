#!/usr/bin/env python3

import argparse
import csv
import numpy as np
from collections import defaultdict

ns_to_ms = 1_000_000

def compute_stats(file_path):
    values = []
    with open(file_path, newline='') as csvfile:
        reader = csv.reader(csvfile)
        for row in reader:
            if len(row) < 3:
                continue
            try:
                val = float(row[2]) / ns_to_ms
                values.append(val)
            except ValueError:
                continue  # Skip headers or invalid rows

    if not values:
        return None, None

    mean_val = np.mean(values)
    p99_val = np.percentile(values, 99)
    return mean_val, p99_val

sz_map = {
    "536870912": "512M",
    "268435456": "256M",
    "65536": "64K"
}

disk_map = {
    "nvme1n1p1": "Block",
    "nvme0n2": "ZNS"
}
dist_map = {
    "ZIPFIAN": "ZIPF",
    "UNIFORM": "UNIF"
}

def get_labels(labels):
    lab = []
    labels_split = labels.split('|')
    for l in labels_split:
        keywords = l.split(',')
        ssd = disk_map[keywords[-1].split("-")[0]]
        lab.append((ssd, f"{sz_map[keywords[0]]}-{dist_map[keywords[2]]}-{keywords[3].split("=")[1]}"))
    return lab

def main():
    parser = argparse.ArgumentParser(description='Compute mean and P99 of the third column in CSVs.')
    parser.add_argument('--files', required=True, help='|-separated list of CSV file paths.')
    parser.add_argument('--labels', required=True, help='|-separated list of labels for the files.')

    args = parser.parse_args()

    files = args.files.split('|')
    labels = get_labels(args.labels)

    if len(files) != len(labels):
        raise ValueError("The number of files and labels must be the same.")

    data = defaultdict(dict)
    for file, label in zip(files, labels):
        mean, p99 = compute_stats(file)
        data[label[1]][label[0]] = {"mean": mean, "p99": p99}

    for key, val in data.items():
        data[key]["Block"]["mean multiplier"] = data[key]["Block"]["mean"] / data[key]["ZNS"]["mean"]
        data[key]["ZNS"]["mean multiplier"] = 1
        data[key]["Block"]["p99 multiplier"] = data[key]["Block"]["p99"] / data[key]["ZNS"]["p99"]
        data[key]["ZNS"]["p99 multiplier"] = 1

    print("| Name | Mean (ms) | P99 (ms) |")
    print("|------|------|-----|")
    for key, val in data.items():
        for k,v in val.items():
            print(f" | {k}-{key} | {v['mean']:.2f} ({v['mean multiplier']:.2f}x) |{v['p99']:.2f} ({v['p99 multiplier']:.2f}x) |")

if __name__ == '__main__':
    main()
