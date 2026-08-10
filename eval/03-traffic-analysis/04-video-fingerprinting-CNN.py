import os
import sys
import random
import numpy as np
import tensorflow as tf
from tensorflow.keras import layers, models
from tensorflow.keras.optimizers import Adam
from tensorflow.keras.callbacks import ModelCheckpoint, EarlyStopping


# ============================================================
# AE configuration
# ============================================================

INPUT_LENGTH = 600       # 60 seconds x 10 bins/second
NUM_CLASSES = 5          # 5 videos
NUM_SHARDS = 10          # 10 traces per video -> 10 TFRecord shards

NUM_FOLDS = 5
EPOCHS = 40
BATCH_SIZE = 8
LEARNING_RATE = 0.001

SEED = 42


# ============================================================
# CPU configuration
# ============================================================

# The AE VM is expected to have 3 CPU cores.
# Keep TensorFlow's thread usage modest and predictable.
tf.config.threading.set_intra_op_parallelism_threads(3)
tf.config.threading.set_inter_op_parallelism_threads(1)

random.seed(SEED)
np.random.seed(SEED)
tf.random.set_seed(SEED)


# ============================================================
# Lightweight VF CNN
# ============================================================

def vf_cnn_model(input_length=INPUT_LENGTH, num_classes=NUM_CLASSES):
    """
    Lightweight 1D CNN for the scaled-down AE video-fingerprinting task.

    Compared with the original model:
      - input length: 1200 -> 600
      - classes: 50 -> 5
      - Conv1D filters: 150 -> 32/64
      - removes five 1024-unit dense layers
      - uses GlobalAveragePooling1D
      - uses one small 64-unit dense layer
    """

    inputs = layers.Input(
        shape=(input_length, 1),
        name="input"
    )

    # Convolution block 1
    x = layers.Conv1D(
        filters=64,
        kernel_size=15,
        strides=1,
        padding="valid",
        use_bias=False,
        name="conv1"
    )(inputs)
    x = layers.BatchNormalization(name="bn1")(x)
    x = layers.LeakyReLU(name="relu1")(x)
    x = layers.AveragePooling1D(
        pool_size=4,
        name="pool1"
    )(x)
    x = layers.Dropout(
        rate=0.25,
        name="dropout1"
    )(x)

    # Convolution block 2
    x = layers.Conv1D(
        filters=64,
        kernel_size=15,
        strides=1,
        padding="valid",
        use_bias=False,
        name="conv2"
    )(x)
    x = layers.BatchNormalization(name="bn2")(x)
    x = layers.LeakyReLU(name="relu2")(x)
    x = layers.AveragePooling1D(
        pool_size=4,
        name="pool2"
    )(x)
    x = layers.Dropout(
        rate=0.25,
        name="dropout2"
    )(x)

    # Much lighter than Flatten + multiple 1024-unit Dense layers.
    x = layers.GlobalAveragePooling1D(
        name="global_average_pool"
    )(x)

    x = layers.Dense(
        128,
        activation="relu",
        name="dense"
    )(x)
    x = layers.Dropout(
        rate=0.30,
        name="dropout3"
    )(x)

    outputs = layers.Dense(
        num_classes,
        activation="softmax",
        name="output"
    )(x)

    return models.Model(
        inputs=inputs,
        outputs=outputs
    )


# ============================================================
# TFRecord parsing
# ============================================================

def _parse_function(proto):
    feature_description = {
        "feature": tf.io.FixedLenFeature(
            [INPUT_LENGTH],
            tf.float32
        ),
        "label": tf.io.FixedLenFeature(
            [NUM_CLASSES],
            tf.float32
        ),
    }

    parsed = tf.io.parse_single_example(
        proto,
        feature_description
    )

    # Conv1D expects (length, channels).
    feature = tf.expand_dims(
        parsed["feature"],
        axis=-1
    )

    label = parsed["label"]

    return feature, label


def make_ds(file_list, batch_size=BATCH_SIZE, training=False):
    ds = tf.data.TFRecordDataset(
        file_list,
        num_parallel_reads=1
    )

    ds = ds.map(
        _parse_function,
        num_parallel_calls=1
    )

    if training:
        ds = ds.shuffle(
            buffer_size=128,
            seed=SEED,
            reshuffle_each_iteration=True
        )

    ds = ds.batch(batch_size)

    # Small fixed prefetch is more appropriate for the 3-CPU AE VM
    # than aggressive AUTOTUNE parallelism.
    ds = ds.prefetch(1)

    return ds


# ============================================================
# Closed-world 5-fold evaluation
# ============================================================

def closedworld_eval_for_5fold(
    tfrecord_dir,
    output_dir="vf-models"
):
    os.makedirs(output_dir, exist_ok=True)

    all_tfrecord_files = [
        os.path.join(
            tfrecord_dir,
            f"bps_preprocessed_{NUM_CLASSES}_{i}.tfrecord"
        )
        for i in range(NUM_SHARDS)
    ]

    missing = [
        f for f in all_tfrecord_files
        if not os.path.isfile(f)
    ]

    if missing:
        print("[ERROR] Missing TFRecord files:")
        for f in missing:
            print(f"  {f}")
        sys.exit(1)

    print("============================================================")
    print(" Scaled-Down VF CNN Evaluation")
    print("============================================================")
    print(f"[INFO] TFRecord directory : {tfrecord_dir}")
    print(f"[INFO] Input length       : {INPUT_LENGTH}")
    print(f"[INFO] Number of classes  : {NUM_CLASSES}")
    print(f"[INFO] TFRecord shards    : {NUM_SHARDS}")
    print(f"[INFO] Folds              : {NUM_FOLDS}")
    print(f"[INFO] Epochs             : {EPOCHS}")
    print(f"[INFO] Batch size         : {BATCH_SIZE}")
    print(f"[INFO] Learning rate      : {LEARNING_RATE}")
    print()

    # Deterministic shard shuffle.
    idx = np.arange(len(all_tfrecord_files))
    rng = np.random.default_rng(SEED)
    rng.shuffle(idx)

    fold_indices = np.array_split(
        idx,
        NUM_FOLDS
    )

    fold_best = []

    for k in range(NUM_FOLDS):
        print()
        print(
            f"================= "
            f"Fold {k + 1}/{NUM_FOLDS} "
            f"================="
        )

        val_idx = fold_indices[k]

        train_idx = np.concatenate([
            fold_indices[j]
            for j in range(NUM_FOLDS)
            if j != k
        ])

        val_files = [
            all_tfrecord_files[i]
            for i in val_idx
        ]

        train_files = [
            all_tfrecord_files[i]
            for i in train_idx
        ]

        print(
            f"[INFO] Train shards: {len(train_files)} | "
            f"Validation shards: {len(val_files)}"
        )

        train_dataset = make_ds(
            train_files,
            training=True
        )

        val_dataset = make_ds(
            val_files,
            training=False
        )

        # Fresh model for every fold.
        model = vf_cnn_model()

        model.compile(
            optimizer=Adam(
                learning_rate=LEARNING_RATE
            ),
            loss="categorical_crossentropy",
            metrics=[
                "accuracy",
                tf.keras.metrics.TopKCategoricalAccuracy(
                    k=2,
                    name="top_2_accuracy"
                ),
            ],
        )

        if k == 0:
            model.summary()

        ckpt_path = os.path.join(
            output_dir,
            f"vf_ae_fold{k + 1}.keras"
        )

        checkpoint = ModelCheckpoint(
            filepath=ckpt_path,
            monitor="val_accuracy",
            mode="max",
            save_best_only=True,
            verbose=1
        )

        # early_stopping = EarlyStopping(
        #     monitor="val_accuracy",
        #     mode="max",
        #     patience=4,
        #     restore_best_weights=True,
        #     verbose=1
        # )

        history = model.fit(
            train_dataset,
            epochs=EPOCHS,
            validation_data=val_dataset,
            callbacks=[
                checkpoint,
                # early_stopping
            ],
            verbose=2
        )

        best_val = float(
            np.max(
                history.history.get(
                    "val_accuracy",
                    [0.0]
                )
            )
        )

        fold_best.append(best_val)

        print(
            f"[PASS] Fold {k + 1} best "
            f"validation accuracy: {best_val:.4f}"
        )

        # Release model memory before the next fold.
        tf.keras.backend.clear_session()

    print()
    print("============================================================")
    print(" 5-Fold Summary")
    print("============================================================")

    for i, acc in enumerate(
        fold_best,
        start=1
    ):
        print(
            f"Fold {i}: "
            f"{acc:.4f}"
        )

    mean_acc = float(
        np.mean(fold_best)
    )

    std_acc = float(
        np.std(fold_best)
    )

    print()
    print(
        f"Mean validation accuracy: "
        f"{mean_acc:.4f} "
        f"(+/- {std_acc:.4f})"
    )

    return fold_best


# ============================================================
# Main
# ============================================================

def main():
    if len(sys.argv) not in (2, 3):
        print("Usage:")
        print(
            "  python3 04-video-fingerprinting-CNN.py "
            "<tfrecord-directory> "
            "[output-directory]"
        )
        print()
        print("Example:")
        print(
            "  python3 04-video-fingerprinting-CNN.py "
            "trafficsplitter-video-tfrecord "
            "trafficsplitter-vf-models"
        )
        sys.exit(1)

    tfrecord_dir = os.path.abspath(
        sys.argv[1]
    )

    if len(sys.argv) == 3:
        output_dir = os.path.abspath(
            sys.argv[2]
        )
    else:
        output_dir = os.path.abspath(
            "vf-models"
        )

    closedworld_eval_for_5fold(
        tfrecord_dir,
        output_dir
    )


if __name__ == "__main__":
    main()