import os
import sys
import random
import numpy as np
import tensorflow as tf
from tensorflow.keras import layers, models
from tensorflow.keras.optimizers import Adamax
from tensorflow.keras.callbacks import ModelCheckpoint, EarlyStopping


# ============================================================
# AE configuration
# ============================================================

INPUT_LENGTH = 5000
NUM_CLASSES = 10
NUM_SHARDS = 20

NUM_FOLDS = 5
EPOCHS = 40
BATCH_SIZE = 16
LEARNING_RATE = 0.002

SEED = 42


# ============================================================
# CPU configuration
# ============================================================

# The AE VM is expected to have 3 CPU cores.
tf.config.threading.set_intra_op_parallelism_threads(3)
tf.config.threading.set_inter_op_parallelism_threads(1)

random.seed(SEED)
np.random.seed(SEED)
tf.random.set_seed(SEED)


# ============================================================
# Lightweight DF-style 1D CNN
# ============================================================

def cnn_model_1d(input_length=INPUT_LENGTH, num_classes=NUM_CLASSES):
    """
    Lightweight DF-style CNN for the scaled-down AE WF task.

    Compared with the original model:
      - classes: 93 -> 10
      - convolution filters: 32/64/128/256 -> 16/32/64/128
      - dense layers: 512/512 -> 256/128
      - keeps the original 4-block convolutional structure
    """

    inputs = layers.Input(
        shape=(input_length, 1),
        name="input"
    )

    # --------------------------------------------------------
    # Convolution block 1
    # --------------------------------------------------------

    x = layers.Conv1D(
        filters=16,
        kernel_size=8,
        padding="same",
        activation=None,
        use_bias=True,
        name="conv1"
    )(inputs)

    x = layers.BatchNormalization(
        name="bn1"
    )(x)

    x = layers.ELU(
        name="elu1"
    )(x)

    x = layers.Dropout(
        0.1,
        name="dropout1"
    )(x)

    x = layers.MaxPooling1D(
        pool_size=4,
        name="pool1"
    )(x)

    # --------------------------------------------------------
    # Convolution block 2
    # --------------------------------------------------------

    x = layers.Conv1D(
        filters=32,
        kernel_size=8,
        padding="same",
        activation=None,
        use_bias=True,
        name="conv2"
    )(x)

    x = layers.BatchNormalization(
        name="bn2"
    )(x)

    x = layers.ELU(
        name="elu2"
    )(x)

    x = layers.Dropout(
        0.1,
        name="dropout2"
    )(x)

    x = layers.MaxPooling1D(
        pool_size=4,
        name="pool2"
    )(x)

    # --------------------------------------------------------
    # Convolution block 3
    # --------------------------------------------------------

    x = layers.Conv1D(
        filters=64,
        kernel_size=8,
        padding="same",
        activation=None,
        use_bias=True,
        name="conv3"
    )(x)

    x = layers.BatchNormalization(
        name="bn3"
    )(x)

    x = layers.ReLU(
        name="relu3"
    )(x)

    x = layers.Dropout(
        0.1,
        name="dropout3"
    )(x)

    x = layers.MaxPooling1D(
        pool_size=4,
        name="pool3"
    )(x)

    # --------------------------------------------------------
    # Convolution block 4
    # --------------------------------------------------------

    x = layers.Conv1D(
        filters=128,
        kernel_size=8,
        padding="same",
        activation=None,
        use_bias=True,
        name="conv4"
    )(x)

    x = layers.BatchNormalization(
        name="bn4"
    )(x)

    x = layers.ReLU(
        name="relu4"
    )(x)

    x = layers.Dropout(
        0.1,
        name="dropout4"
    )(x)

    x = layers.MaxPooling1D(
        pool_size=4,
        name="pool4"
    )(x)

    # --------------------------------------------------------
    # Dense layers
    # --------------------------------------------------------

    x = layers.Flatten(
        name="flatten"
    )(x)

    x = layers.Dense(
        256,
        activation="relu",
        name="dense1"
    )(x)

    x = layers.Dropout(
        0.5,
        name="dense_dropout1"
    )(x)

    x = layers.Dense(
        128,
        activation="relu",
        name="dense2"
    )(x)

    x = layers.Dropout(
        0.4,
        name="dense_dropout2"
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
            buffer_size=256,
            seed=SEED,
            reshuffle_each_iteration=True
        )

    ds = ds.batch(batch_size)

    # Avoid aggressive AUTOTUNE on the 3-CPU AE VM.
    ds = ds.prefetch(1)

    return ds


# ============================================================
# Closed-world 5-fold evaluation
# ============================================================

def closedworld_eval_for_5fold(
    tfrecord_dir,
    output_dir="wf-models"
):
    os.makedirs(output_dir, exist_ok=True)

    all_tfrecord_files = [
        os.path.join(
            tfrecord_dir,
            f"df_preprocessed_{NUM_CLASSES}_{i}.tfrecord"
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
    print(" Scaled-Down WF CNN Evaluation")
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

    idx = np.arange(
        len(all_tfrecord_files)
    )

    rng = np.random.default_rng(
        SEED
    )

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

        model = cnn_model_1d()

        model.compile(
            optimizer=Adamax(
                learning_rate=LEARNING_RATE
            ),
            loss="categorical_crossentropy",
            metrics=[
                "accuracy",
                tf.keras.metrics.TopKCategoricalAccuracy(
                    k=2,
                    name="top_2_accuracy"
                ),
                tf.keras.metrics.TopKCategoricalAccuracy(
                    k=3,
                    name="top_3_accuracy"
                ),
            ],
        )

        if k == 0:
            model.summary()

        ckpt_path = os.path.join(
            output_dir,
            f"wf_ae_fold{k + 1}.keras"
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
            "  python3 05-web-fingerprinting-DF.py "
            "<tfrecord-directory> "
            "[output-directory]"
        )
        print()
        print("Example:")
        print(
            "  python3 05-web-fingerprinting-DF.py "
            "trafficsplitter-web-tfrecord "
            "trafficsplitter-wf-models"
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
            "wf-models"
        )

    closedworld_eval_for_5fold(
        tfrecord_dir,
        output_dir
    )


if __name__ == "__main__":
    main()
