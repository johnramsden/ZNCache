#!/usr/bin/env python3
import argparse
import statistics
import numpy as np
from matplotlib.ticker import ScalarFormatter

import csv
import matplotlib.pyplot as plt
import pandas as pd
from matplotlib import rcParams

# Increase all font sizes by 4 points from their defaults
rcParams.update({key: rcParams[key] + 4 for key in rcParams if "size" in key and isinstance(rcParams[key], (int, float))})

def main():
    parser = argparse.ArgumentParser(
        description="Scatter plot CSV data. Uses the second column as the default y-axis label and plot title."
    )
    parser.add_argument("data_files", help="Comma-delimited paths to the CSV file(s).")
    parser.add_argument(
        "--labels",
        help="Comma-delimited list of labels for the data sets. If not provided, defaults to the value in the second column of each file.",
        default=None
    )
    parser.add_argument(
        "--yaxis",
        help="Label for the y-axis. Defaults to the value in the second column of the first file.",
        default=None
    )
    parser.add_argument(
        "--title",
        help="Title for the plot. Defaults to the value in the second column of the first file.",
        default=None
    )
    parser.add_argument(
        "--output",
        help="Output file.",
        default=None
    )
    parser.add_argument(
        "--yunits",
        help="yaxis units (MiB, GiB, B).",
        choices=['MiB', 'GiB', 'B'],
        default="B"
    )
    parser.add_argument(
        "--inunits",
        help="Input units (MiB, GiB, B).",
        choices=['MiB', 'GiB', 'B'],
        default="B"
    )
    parser.add_argument(
        "--type",
        help="Plot type.",
        choices=['scatter', 'line'],
        default="scatter"
    )
    parser.add_argument(
        "--regression",
        action="store_true",
        help="Add regression line to plot."
    )
    parser.add_argument(
        "--skipzero",
        help="Skip zeros.",
        action='store_true'
    )
    parser.add_argument(
        "--overlay-threads",
        help="Optional: comma-separated list of alternating begin,end csvs to overlay eviction intervals. Example: b1.csv,e1.csv,b2.csv,e2.csv",
        default=None
    )

    args = parser.parse_args()

    files = [df.strip() for df in args.data_files.split(',')]
    if args.labels is not None:
        label_list = [lbl.strip() for lbl in args.labels.split(',')]
        if len(label_list) != len(files):
            print("Error: The number of labels provided does not match the number of data files.")
            return
    else:
        label_list = [None] * len(files)

    plt.figure(figsize=(12, 4))
    overall_default_label = None

    for idx, file in enumerate(files):
        x_vals = []
        y_vals = []
        default_label = None

        with open(file, 'r') as f:
            reader = csv.reader(f)
            for row in reader:
                if default_label is None:
                    default_label = row[1]
                try:
                    x_val = float(row[0]) / 60000.0
                    y_val = float(row[2])

                    if args.inunits == "MiB":
                        y_val *= (1024 * 1024)
                    elif args.inunits == "GiB":
                        y_val *= (1024 * 1024 * 1024)

                    if args.yunits == "MiB":
                        y_val /= (1024 * 1024)
                    elif args.yunits == "GiB":
                        y_val /= (1024 * 1024 * 1024)

                    if args.skipzero and y_val == 0:
                        continue

                except ValueError as e:
                    print(f"Skipping row {row} in file {file}: {e}")
                    continue
                x_vals.append(x_val)
                y_vals.append(y_val)

        label = label_list[idx] if label_list[idx] is not None else default_label

        if overall_default_label is None:
            overall_default_label = default_label

        print(f"File: {file} Average: {statistics.fmean(y_vals)}")
        if args.type == "scatter":
            plt.scatter(x_vals, y_vals, label=label, s=1)
        else:
            plt.plot(x_vals, y_vals, label=label)

        if args.regression:
            x_arr = np.array(x_vals)
            y_arr = np.array(y_vals)
            slope, intercept = np.polyfit(x_arr, y_arr, 1)
            y_fit = slope * x_arr + intercept
            plt.plot(x_arr, y_fit, color='red')

    # Optional overlay eviction threads
    if args.overlay_threads:
        thread_csvs = [p.strip() for p in args.overlay_threads.split(',')]
        if len(thread_csvs) % 2 != 0:
            print("Error: overlay-threads must have an even number of files (begin,end pairs)")
        else:
            for i in range(0, len(thread_csvs), 2):
                begin_file = thread_csvs[i]
                end_file = thread_csvs[i + 1]
                thread_id = f"thread-{i//2}"

                begin_df = pd.read_csv(begin_file, header=None, names=['timestamp', 'name', 'id'])
                end_df = pd.read_csv(end_file, header=None, names=['timestamp', 'name', 'id'])

                begin_df['timestamp'] = begin_df['timestamp'] / 60000
                end_df['timestamp'] = end_df['timestamp'] / 60000

                merged = pd.merge(begin_df, end_df, on='id', suffixes=('_begin', '_end'))
                merged.sort_values('timestamp_begin', inplace=True)

                for _, row in merged.iterrows():
                    plt.plot(
                        [row['timestamp_begin'], row['timestamp_end']],
                        [0, 0],  # Plot as a line at y=0 (or some fixed dummy value)
                        linewidth=1,
                        linestyle='--',
                        color='gray',
                        alpha=0.4,
                        zorder=0,
                    )
    plt.ticklabel_format(style='plain', axis='y')

    plt.xlabel("Time (minutes)")
    y_label = args.yaxis if args.yaxis is not None else overall_default_label
    plt.ylabel(y_label)
    plot_title = args.title
    if plot_title is not None:
        plt.title(plot_title)
    plt.legend()

    out = f"data/{plot_title}.png"
    if args.output is not None:
        out = args.output
    plt.savefig(out, bbox_inches='tight', pad_inches=0)

    print(f"Saved plot to {out}")
    # plt.show()

if __name__ == '__main__':
    main()
