import tensorflow as tf
import pandas as pd
import random
import os
import sys
from datetime import datetime


CLIENT_IP = "192.168.10.10"

NUM_WEBSITES = 10
ITERATIONS = 20

TARGET_SIZE = 5100
FEATURE_SIZE = 5000


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


def preprocess(filepath, web_num, nOfLabel):
    data = {
        "time": [],
        "length": [],
    }

    print(f"[INFO] Reading: {filepath}")

    time_format = "%H:%M:%S.%f"

    with open(filepath, "r") as file:
        for line in file:

            # ------------------------------------------------
            # Parse tcpdump line
            # ------------------------------------------------

            try:
                info_arr = line.strip().split(",")

                general_info = info_arr[0].split()
                pkt_len = info_arr[-1].split()

                if pkt_len[0] != "length":
                    continue

                length = int(pkt_len[1])

            except (IndexError, ValueError):
                continue

            # Ignore packets without payload
            if length <= 0:
                continue

            # ------------------------------------------------
            # Extract timestamp and traffic direction
            # ------------------------------------------------

            try:
                time_stamp = datetime.strptime(
                    general_info[0],
                    time_format
                )

                source = general_info[2]

                if source.startswith(CLIENT_IP + "."):
                    direction = 1       # uplink
                else:
                    direction = 0       # downlink

            except (IndexError, ValueError):
                continue

            # ------------------------------------------------
            # Convert packet size into 1500-byte chunks
            # ------------------------------------------------

            num_of_chunks = length / 1500

            if direction == 0:
                value = 1
            else:
                value = -1

            while num_of_chunks > 0.03:
                data["length"].append(value)
                data["time"].append(time_stamp)

                num_of_chunks -= 1

    df = pd.DataFrame(data)

    if df.empty:
        raise ValueError(f"No valid traffic data found in {filepath}")

    df.set_index("time", inplace=True)

    # --------------------------------------------------------
    # Pad / truncate trace
    # --------------------------------------------------------

    if len(df) < TARGET_SIZE:

        num_to_pad = TARGET_SIZE - len(df)

        padding = pd.DataFrame({
            "length": [0] * num_to_pad
        })

        # Timestamp values are not used by DF after this point,
        # so reuse the final timestamp for padded entries.
        padding.index = [df.index[-1]] * num_to_pad
        padding.index.name = "time"

        df = pd.concat([df, padding])

    elif len(df) > TARGET_SIZE:

        df = df.iloc[:TARGET_SIZE]

    # --------------------------------------------------------
    # Generate feature and label
    # --------------------------------------------------------

    feature = df["length"].iloc[:FEATURE_SIZE].to_numpy()

    label = [0.0] * nOfLabel
    label[web_num] = 1.0

    return feature, label


def main():
    if len(sys.argv) != 3:
        print("Usage:")
        print("  python3 03-preprocess_WF_DF.py <trace-directory> <output-directory>")
        print()
        print("Example:")
        print(
            "  python3 03-preprocess_WF_DF.py "
            "trafficsplitter-web-traces trafficsplitter-web-tfrecord"
        )
        sys.exit(1)

    trace_dir = os.path.abspath(sys.argv[1])
    output_dir = os.path.abspath(sys.argv[2])

    os.makedirs(output_dir, exist_ok=True)

    print("============================================================")
    print(" Website Fingerprinting Preprocessing")
    print("============================================================")
    print(f"[INFO] Trace directory : {trace_dir}")
    print(f"[INFO] Output directory: {output_dir}")
    print(f"[INFO] Websites        : {NUM_WEBSITES}")
    print(f"[INFO] Traces/site     : {ITERATIONS}")
    print(f"[INFO] Feature size    : {FEATURE_SIZE}")
    print()

    # --------------------------------------------------------
    # Generate one TFRecord for each trace iteration
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

        for web_idx in range(NUM_WEBSITES):

            filename = f"data{web_idx}_{trace_idx}.log"
            filepath = os.path.join(trace_dir, filename)

            if not os.path.isfile(filepath):
                raise FileNotFoundError(
                    f"Trace file not found: {filepath}"
                )

            feature, label = preprocess(
                filepath,
                web_num=web_idx,
                nOfLabel=NUM_WEBSITES,
            )

            features.append(feature)
            labels.append(label)

        features, labels = shuffle_lists_together(
            features,
            labels
        )

        output_file = os.path.join(
            output_dir,
            f"df_preprocessed_{NUM_WEBSITES}_{trace_idx}.tfrecord",
        )

        with tf.io.TFRecordWriter(output_file) as writer:

            for feature, label in zip(features, labels):

                example = serialize_example(
                    feature,
                    label
                )

                writer.write(example)

        print(f"[PASS] Saved: {output_file}")

    print()
    print("============================================================")
    print(" Preprocessing completed."
    )
    print("============================================================")


if __name__ == "__main__":
    main()