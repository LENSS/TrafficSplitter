import tensorflow as tf
import pandas as pd
import random
import os
import sys
from datetime import datetime, timedelta


CLIENT_IP = "192.168.10.10"

NUM_VIDEOS = 5
ITERATIONS = 10

BIN_SIZE = "0.1s"
FEATURE_SIZE = 600       # 60 seconds x 10 bins/second
MOVING_WINDOW = 20       # Move by 2 seconds (20 x 0.1s)


def shuffle_lists_together(list1, list2):
    if len(list1) != len(list2):
        raise ValueError("Both lists must have the same length")

    zipped_list = list(zip(list1, list2))
    random.shuffle(zipped_list)

    list1_shuffled, list2_shuffled = zip(*zipped_list)

    return list(list1_shuffled), list(list2_shuffled)


def float_feature(value):
    return tf.train.Feature(
        float_list=tf.train.FloatList(value=value)
    )


def serialize_example(feature, label):
    feature_dict = {
        "feature": float_feature(feature),
        "label": float_feature(label),
    }

    example_proto = tf.train.Example(
        features=tf.train.Features(feature=feature_dict)
    )

    return example_proto.SerializeToString()


def preprocess(filepath, video_num, nOfLabel):
    data = {
        "time": [],
        "dl_bytes": [],
    }

    print(f"[INFO] Reading: {filepath}")

    time_format = "%H:%M:%S.%f"

    current_date = datetime(2000, 1, 1)
    last_time = None

    # --------------------------------------------------------
    # Parse tcpdump trace
    # --------------------------------------------------------

    with open(filepath, "r") as file:
        for line in file:

            try:
                info_arr = line.strip().split(",")

                general_info = info_arr[0].split()
                pkt_len = info_arr[-1].split()

                if pkt_len[0] != "length":
                    continue

                length = int(pkt_len[1])

            except (IndexError, ValueError):
                continue

            try:
                # Parse timestamp.
                t = datetime.strptime(
                    general_info[0],
                    time_format
                )

                time_stamp = current_date.replace(
                    hour=t.hour,
                    minute=t.minute,
                    second=t.second,
                    microsecond=t.microsecond
                )

                # Handle midnight rollover if necessary.
                if last_time and time_stamp < last_time:
                    delta = last_time - time_stamp

                    if delta > timedelta(hours=1):
                        current_date += timedelta(days=1)

                        time_stamp = current_date.replace(
                            hour=t.hour,
                            minute=t.minute,
                            second=t.second,
                            microsecond=t.microsecond
                        )

                source = general_info[2]

                # Client -> Proxy = uplink
                # Proxy -> Client = downlink
                if source.startswith(CLIENT_IP + "."):
                    direction = 1
                else:
                    direction = 0

            except (IndexError, ValueError):
                continue

            # Store only downlink traffic.
            if direction == 0 and length > 0:
                data["dl_bytes"].append(length)
                data["time"].append(time_stamp)

            last_time = time_stamp

    df = pd.DataFrame(data)

    if df.empty:
        raise ValueError(
            f"No valid downlink traffic found in {filepath}"
        )

    df.set_index("time", inplace=True)

    # --------------------------------------------------------
    # Aggregate bytes into 0.1-second bins
    # --------------------------------------------------------

    df = df.resample(BIN_SIZE).sum()

    # --------------------------------------------------------
    # Normalize
    # --------------------------------------------------------

    max_bytes = df["dl_bytes"].max()

    if max_bytes > 0:
        df["dl_bytes"] = df["dl_bytes"] / max_bytes

    # --------------------------------------------------------
    # Pad only when the trace is shorter than one feature.
    # Keep the full trace otherwise so sliding windows can
    # generate multiple features.
    # --------------------------------------------------------

    if len(df) < FEATURE_SIZE:
        num_to_pad = FEATURE_SIZE - len(df)

        padding = pd.DataFrame({
            "dl_bytes": [0.0] * num_to_pad
        })

        df = pd.concat(
            [df.reset_index(drop=True), padding],
            ignore_index=True
        )
    else:
        df = df.reset_index(drop=True)

    # --------------------------------------------------------
    # Generate sliding-window features and labels
    # --------------------------------------------------------

    features = []
    labels = []

    raw_length = len(df)

    for idx in range(
        0,
        raw_length - FEATURE_SIZE + 1,
        MOVING_WINDOW
    ):
        feature = df["dl_bytes"].iloc[
            idx:idx + FEATURE_SIZE
        ].to_numpy()

        label = [0.0] * nOfLabel
        label[video_num] = 1.0

        features.append(feature)
        labels.append(label)

    print(
        f"[INFO] Generated {len(features)} features "
        f"from {os.path.basename(filepath)}"
    )

    return features, labels, df


def main():
    if len(sys.argv) != 3:
        print("Usage:")
        print(
            "  python3 02-preprocess_VF_bps.py "
            "<trace-directory> <output-directory>"
        )
        print()
        print("Example:")
        print(
            "  python3 02-preprocess_VF_bps.py "
            "trafficsplitter-video-traces "
            "trafficsplitter-video-tfrecord"
        )
        sys.exit(1)

    trace_dir = os.path.abspath(sys.argv[1])
    output_dir = os.path.abspath(sys.argv[2])

    os.makedirs(output_dir, exist_ok=True)

    print("============================================================")
    print(" Video Fingerprinting BPS Preprocessing")
    print("============================================================")
    print(f"[INFO] Trace directory : {trace_dir}")
    print(f"[INFO] Output directory: {output_dir}")
    print(f"[INFO] Videos          : {NUM_VIDEOS}")
    print(f"[INFO] Traces/video    : {ITERATIONS}")
    print(f"[INFO] Bin size        : {BIN_SIZE}")
    print(f"[INFO] Feature size    : {FEATURE_SIZE}")
    print(f"[INFO] Window step     : {MOVING_WINDOW} bins")
    print(
        f"[INFO] Window step     : "
        f"{MOVING_WINDOW * 0.1:.1f} seconds"
    )
    print()

    # --------------------------------------------------------
    # Generate one TFRecord per trace iteration
    # --------------------------------------------------------

    for trace_idx in range(ITERATIONS):

        features = []
        labels = []

        print()
        print("============================================================")
        print(
            f" Processing trace iteration "
            f"{trace_idx + 1}/{ITERATIONS}"
        )
        print("============================================================")

        for video_idx in range(NUM_VIDEOS):

            filename = f"data{video_idx}_{trace_idx}.log"
            filepath = os.path.join(trace_dir, filename)

            if not os.path.isfile(filepath):
                raise FileNotFoundError(
                    f"Trace file not found: {filepath}"
                )

            f, l, _ = preprocess(
                filepath,
                video_num=video_idx,
                nOfLabel=NUM_VIDEOS
            )

            features.extend(f)
            labels.extend(l)

        features, labels = shuffle_lists_together(
            features,
            labels
        )

        output_file = os.path.join(
            output_dir,
            f"bps_preprocessed_{NUM_VIDEOS}_{trace_idx}.tfrecord"
        )

        with tf.io.TFRecordWriter(output_file) as writer:

            for feature, label in zip(features, labels):

                example = serialize_example(
                    feature,
                    label
                )

                writer.write(example)

        print(
            f"[PASS] Saved: {output_file} "
            f"({len(features)} features)"
        )

    print()
    print("============================================================")
    print(" Video preprocessing completed.")
    print("============================================================")


if __name__ == "__main__":
    main()