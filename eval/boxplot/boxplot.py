#!/usr/bin/env python3

import argparse
import pandas as pd
import matplotlib.pyplot as plt


def main():
    parser = argparse.ArgumentParser(
        description="Generate side-by-side box plots from multiple CSV files."
    )
    parser.add_argument(
        "csv_files",
        help="Comma-delimited string with file paths (e.g. 'file1.csv,file2.csv')."
    )
    parser.add_argument(
        "labels",
        help="Comma-delimited string of labels corresponding to each file "
             "(e.g. 'Label1,Label2')."
    )
    parser.add_argument(
        "yaxis",
        help="yaxis labels."
    )
    parser.add_argument(
        "title",
        help="Plot title."
    )

    args = parser.parse_args()

    # Split the comma-delimited strings into lists
    csv_files_list = [path.strip() for path in args.csv_files.split(',')]
    labels_list = [lbl.strip() for lbl in args.labels.split(',')]

    all_data = []

    # Read each CSV and extract the first column
    for file_path in csv_files_list:
        df = pd.read_csv(file_path)
        first_col = df.iloc[:, 0]
        all_data.append(first_col)

    # Create the side-by-side box plot
    plt.boxplot(all_data, labels=labels_list)
    plt.title(args.title)
    plt.ylabel(args.yaxis)
    plt.figure(figsize=(6, 4))
    plt.show()

    # Print summary statistics for each dataset
    for lbl, data in zip(labels_list, all_data):
        print(f"\n=== Statistics for {lbl} ===")
        print(data.describe())  # prints count, mean, std, min, 25%, 50%, 75%, max
        iqr = data.quantile(0.75) - data.quantile(0.25)
        print(f"IQR: {iqr}")


if __name__ == "__main__":
    main()
