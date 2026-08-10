import sys
import os
import pandas as pd
import matplotlib.pyplot as plt
from datetime import datetime


def load_trace(filepath):
    data = {
        "time": [],
        "bytes": [],
    }

    time_format = "%H:%M:%S.%f"

    with open(filepath, "r") as file:
        for line in file:
            try:
                info_arr = line.strip().split(",")

                general_info = info_arr[0].split()
                pkt_len = info_arr[-1].split()

                if pkt_len[0] != "length":
                    continue

                length = int(pkt_len[1])

                # Ignore packets without payload.
                if length <= 0:
                    continue

                timestamp = datetime.strptime(
                    general_info[0],
                    time_format
                )

            except (IndexError, ValueError):
                continue

            data["time"].append(timestamp)
            data["bytes"].append(length)

    df = pd.DataFrame(data)

    if not df.empty:
        # Convert timestamps to elapsed seconds.
        start_time = df["time"].iloc[0]
        df["elapsed"] = (
            df["time"] - start_time
        ).dt.total_seconds()

    return df


def main():
    if len(sys.argv) not in (2, 3):
        print("Usage:")
        print("  python3 01-plot_trace.py <trace-file> [output-file]")
        print()
        print("Example:")
        print("  python3 01-plot_trace.py data0_0.log")
        print("  python3 01-plot_trace.py data0_0.log trace.png")
        sys.exit(1)

    filepath = sys.argv[1]

    if len(sys.argv) == 3:
        output_file = sys.argv[2]
    else:
        output_file = os.path.splitext(filepath)[0] + ".png"

    df = load_trace(filepath)

    if df.empty:
        print(f"[ERROR] No valid traffic data found in {filepath}")
        sys.exit(1)

    print(f"[INFO] Trace         : {filepath}")
    print(f"[INFO] Packets       : {len(df)}")
    print(f"[INFO] Total bytes   : {df['bytes'].sum()}")
    print(f"[INFO] Duration      : {df['elapsed'].iloc[-1]:.3f} seconds")

    # --------------------------------------------------------
    # Plot
    # --------------------------------------------------------

    plt.figure(figsize=(10, 4))

    plt.plot(
        df["elapsed"],
        df["bytes"],
        linewidth=1
    )

    plt.title("Traffic Trace")
    plt.xlabel("Time (seconds)")
    plt.ylabel("Packet Payload (bytes)")

    plt.grid(True)
    plt.tight_layout()

    plt.savefig(
        output_file,
        dpi=300,
        bbox_inches="tight"
    )

    plt.close()

    print(f"[PASS] Plot saved to: {output_file}")


if __name__ == "__main__":
    main()