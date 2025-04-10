import os
import sys
import shutil

# Map chunk sizes to labels
CHUNK_SIZE_MAP = {
    "536870912": "512M",
    "65536": "64K"
}

DEVICE_MAP = {
    "/dev/nvme0n2": "ZNS",
    "/dev/nvme1n1": "SSD",
    "/dev/nvme1n1p1": "SSD"
}

def parse_header_lines(filepath, num_lines=20):
    with open(filepath, 'r') as f:
        lines = [f.readline().strip() for _ in range(num_lines)]
    return lines

def extract_metadata(header_lines):
    data = {}
    for line in header_lines:
        if line.strip() == '':
            break
        elif line.startswith("Chunk Size:"):
            size = line.split(":")[1].strip()
            data["ChunkSize"] = CHUNK_SIZE_MAP.get(size, size)
        elif line.startswith("Distribution Type:"):
            data["DistType"] = line.split(":")[1].strip()
        elif line.startswith("Working Set Ratio:"):
            data["Ratio"] = line.split(":")[1].strip()
        elif line.startswith("Iterations:"):
            data["Iter"] = line.split(":")[1].strip()
        elif line.startswith("Number of Zones:"):
            data["Zones"] = line.split(":")[1].strip()
        elif line.startswith("Eviction "):
            data["Evict"] = line.split("Eviction")[1].strip()
        elif line.startswith("Device "):
            data["Device"] = DEVICE_MAP.get(line.split("Device ")[1].strip(), None)
    return data

def build_filename(metadata):
    parts = [
        metadata.get("ChunkSize", None),
        metadata.get("DistType", None),
        f"RATIO={metadata.get('Ratio')}" if "Ratio" in metadata else None,
        f"Iter={metadata.get('Iter')}" if "Iter" in metadata else None,
        f"Zones={metadata.get('Zones')}" if "Zones" in metadata else None,
        metadata.get("Evict", None),
        metadata.get("Device", None)
    ]
    return None if None in parts else "-".join(parts) + ".csv"

def process_directory(directory):
    if not os.path.isdir(directory):
        print(f"Error: '{directory}' is not a valid directory.")
        return

    out_dir = os.path.join(directory, "out")
    os.makedirs(out_dir, exist_ok=True)

    for filename in os.listdir(directory):
        filepath = os.path.join(directory, filename)

        # Skip anything ending in .csv, only handle base `*-run` files
        if filename.endswith(".csv") or not os.path.isfile(filepath):
            continue

        profile_file = filepath + ".profile.csv"
        if not os.path.exists(profile_file):
            print(f"Skipping {filename}: no matching .profile.csv file found.")
            continue

        header = parse_header_lines(filepath)
        metadata = extract_metadata(header)
        new_filename = build_filename(metadata)
        if new_filename is None:
            print(f"Skipping {filename}: no matching .profile.csv file found.")
            continue

        new_filepath = os.path.join(out_dir, new_filename)

        print(f"Copying:\n  {os.path.basename(profile_file)} → out/{new_filename}")
        shutil.copyfile(profile_file, new_filepath)

if __name__ == "__main__":
    if len(sys.argv) != 2:
        print("Usage: python rename_profiles.py <directory>")
        sys.exit(1)

    target_directory = sys.argv[1]
    process_directory(target_directory)
