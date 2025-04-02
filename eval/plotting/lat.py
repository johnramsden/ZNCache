#!/usr/bin/env python3
import argparse
import statistics
import numpy as np
import csv
import matplotlib.pyplot as plt

def main():
    parser = argparse.ArgumentParser(
        description="Scatter plot CSV data. Uses the second column as the default y-axis label and plot title."
    )
    # Accept one or more CSV file paths as a comma-delimited string.
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
        "--regression",
        action="store_true",
        help="Add regression line to plot."
    )
    parser.add_argument(
        "--type",
        help="Plot type.",
        choices=['scatter', 'line'],
        default="scatter"
    )
    args = parser.parse_args()

    # Split the comma-delimited input file paths and strip extra spaces.
    files = [f.strip() for f in args.data_files.split(',')]
    # If labels are provided, split them as well.
    if args.labels is not None:
        label_list = [lbl.strip() for lbl in args.labels.split(',')]
        if len(label_list) != len(files):
            print("Error: The number of labels provided does not match the number of data files.")
            return
    else:
        # Create a placeholder list so that we later use each file's default label.
        label_list = [None] * len(files)

    overall_default_label = None  # Will hold the default label from the first file if needed.

    plt.figure(figsize=(12, 6))

    for idx, file in enumerate(files):
        x_vals = []
        y_vals = []
        default_label = None

        # Read the CSV file and extract data.
        with open(file, 'r') as f:
            reader = csv.reader(f)
            for row in reader:
                # Set the default label from the second column of the first row.
                if default_label is None:
                    default_label = row[1]
                try:
                    # Convert time from ms to minutes.
                    x_val = float(row[0]) / 60000.0
                    # Scale y-value by dividing by 1,000,000.
                    y_val = float(row[2]) / 1_000_000
                    if y_val == 0.0:
                        continue
                except ValueError as e:
                    print(f"Skipping row {row} in file {file}: {e}")
                    continue
                x_vals.append(x_val)
                y_vals.append(y_val)

        # For the first file, use its default label for overall y-axis label and plot title if not provided.
        if overall_default_label is None:
            overall_default_label = default_label

        # Determine the label for this dataset.
        label = label_list[idx] if label_list[idx] is not None else default_label

        print(f"File: {file} Average: {statistics.fmean(y_vals)}")
        # Plot the data.
        if args.type == "scatter":
            plt.scatter(x_vals, y_vals, label=label, s=1)
        else:
            plt.plot(x_vals, y_vals, label=label)

        # Optionally, add a regression line for this dataset.
        if args.regression:
            x_arr = np.array(x_vals)
            y_arr = np.array(y_vals)
            slope, intercept = np.polyfit(x_arr, y_arr, 1)
            y_fit = slope * x_arr + intercept
            plt.plot(x_arr, y_fit, color='red')

    # Disable scientific notation on the y-axis.
    plt.ticklabel_format(style='plain', axis='y')
    plt.xlabel("Time (minutes)")
    # Use the yaxis flag if provided; otherwise use the default label from the first file.
    y_label = args.yaxis if args.yaxis is not None else overall_default_label
    plt.ylabel(y_label)
    # Similarly, use the title flag if provided; otherwise use the default label from the first file.
    plot_title = args.title if args.title is not None else overall_default_label
    plt.title(plot_title)
    plt.legend()

    out = f"data/{plot_title}.png"
    if args.output is not None:
        out = args.output
    plt.savefig(out)
    # plt.show()  # Uncomment to display the plot interactively.

if __name__ == '__main__':
    main()
