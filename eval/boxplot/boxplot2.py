#!/usr/bin/env python3

import argparse
import random
import matplotlib
import numpy as np
import pandas as pd
import matplotlib.pyplot as plt
from pandas.core.base import np
import matplotlib.patches as mpatches

# This is pretty specific to how the final plot looks, so just change the script if needed
def main():
    parser = argparse.ArgumentParser(
        description="Generate side-by-side box plots from multiple CSV files."
    )
    parser.add_argument(
        "csv_files",
        help="Comma-delimited string with file paths (e.g. 'file1.csv,file2.csv')."
    )
    parser.add_argument(
        "chunk_size",
        help="Comma-delimited string with numbers (e.g. 512,64,512,64) that represents the chunk size"
    )
    parser.add_argument(
        "workload_type",
        help="Lower-caes comma-delimited string of labels corresponding to the type of workload"
             "(e.g. 'zipfian,sequential,zipfian')."
    )
    parser.add_argument(
        "working_set_ratio",
        help="Comma-delimited string of working set ratios"
             "(e.g. '10,2,10,2')."
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
    # The plots will be ordered in the same sequence that the csv files are ordered.
    csv_files_list = [path.strip() for path in args.csv_files.split(',')]
    chunk_size_list = args.chunk_size.split(',')
    workload_type_list = args.workload_type.split(',')
    working_set_ratio_list = args.working_set_ratio.split(',')
    colours = []
    for i in workload_type_list:
        match i:
            case "zipfian":
                colours.append("pink")
            case "random":
                colours.append("lightblue")
            case "sequential":
                colours.append("lightgreen")
    
    hatches = []
    for i in working_set_ratio_list:
        match i:
            case "2":
                hatches.append("o")
            case "10":
                hatches.append("//")

    font = {'family' : 'Serif',
            'weight' : 'bold',
            'size'   : 30}

    matplotlib.rc('font', **font)

    # Read each CSV and extract the first column
    all_data = []
    for file_path in csv_files_list:
        df = pd.read_csv(file_path)
        first_col = df.iloc[:, 0]
        all_data.append(first_col)

    # Set the locations of the bar plots on the graph
    # This assumes that ZNS and SSDs are next to each other in the csv file list.
    # So they are set to be spaced closer to each other here.
    positions = [0 for _ in range(len(csv_files_list))]
    label_pos = []
    for i in range(0, len(csv_files_list), 2):
        # This sets pairs
        positions[i] = i
        positions[i+1] = i+0.7
        label_pos.append(i + 0.35)

    # Create the side-by-side box plot
    bplot = plt.boxplot(all_data,
                        widths=0.7,
                        medianprops=dict(linewidth=3, color='black'),
                        patch_artist=True,
                        positions=positions)
    
    
    plt.xticks(label_pos, chunk_size_list)

    legends = [mpatches.Patch(facecolor='pink', hatch='//', label='Zipfian 1:10'),
               mpatches.Patch(facecolor='pink', hatch='o', label='Zipfian 1:2'),
               mpatches.Patch(facecolor='lightblue', hatch='//', label='Random 1:10'),
               mpatches.Patch(facecolor='lightblue', hatch='o', label='Random 1:2'),
               mpatches.Patch(facecolor='lightgreen', hatch='//', label='Sequential 1:10'),
               mpatches.Patch(facecolor='lightgreen', hatch='o', label='Sequential 1:2'),]
    plt.legend(ncols=3, handles=legends, bbox_to_anchor=(1, -0.05))

    for patch, color, hatch in zip(bplot['boxes'], colours, hatches):
        patch.set(linewidth=2)
        patch.set_facecolor(color)
        patch.set_hatch(hatch)

    # Need to figure out how to properly resize everything

    plt.title(args.title)
    plt.ylabel(args.yaxis)
    plt.figure(figsize=(6, 4))
    plt.draw()
    plt.savefig("Fig1.png", dpi=100, bbox_inches="tight")
    plt.show()

if __name__ == "__main__":
    main()
