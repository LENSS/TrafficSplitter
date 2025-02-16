#%%
import pandas as pd
import matplotlib.pyplot as plt
raw_data = {}
# Open the file in read mode
with open("detection.log", "r") as file:
    for line in file:
        info = line.split(" ")
        token = int(info[0].split(":")[-1])
        time = int(info[1].split(":")[-1])
        burst = int(info[2].split(":")[-1].strip()

        if raw_data.get(token) is None:
            raw_data[token] = {}
            raw_data[token]["time"] = []
            raw_data[token]["burst"] = []
        raw_data[token]["time"].append(time)
        raw_data[token]["burst"].append(burst)
# %%

show_figure = False
if show_figure is True: 
    for t in raw_data:
        # print(raw_data[t])
        df = pd.DataFrame(raw_data[t])
        # Convert kernel ns to datetime (ignoring rows with 0 timestamps)
        df['time'] = pd.to_datetime(df['time'], unit='ns', errors='coerce')
        df.set_index('time', inplace=True)
        df_resampled = df.resample('10ms').sum()
        df_resampled['burst'] = df_resampled['burst'].fillna(0)
        df_resampled.reset_index(inplace=True)

        # Create a figure and axis
        plt.figure(figsize=(8, 6))  # Width and height in inches
        plt.plot(df_resampled['time'], df_resampled['burst'], marker='', label='')

        # Add labels and title
        plt.xlabel('Index')
        plt.ylabel('Values')
        plt.title(f'token={t}')
        plt.legend()

        # Show grid
        plt.grid()

        # Display the plot
        print(t)
        plt.show()

# %%
df = pd.DataFrame(raw_data[1661496411])
df['time'] = pd.to_datetime(df['time'], unit='ns', errors='coerce')
df.set_index('time', inplace=True)
df_resampled = df.resample('10ms').sum()
df_resampled['burst'] = df_resampled['burst'].fillna(0)
df_resampled.reset_index(inplace=True)

moving_window = 2
sec_in_sf = 100 # 1 value = 10ms -> 100 value = 1s  
how_long = 10 # This represents time length of one data feature. If it is 15, then one feature includes 15 seconds of traffic info. 
features = []
labels=[]
answer=1
for idx in range(0, len(df_resampled), moving_window):
    # Check if there is an enough number of datapoints for one feature        
    if idx + sec_in_sf*how_long > len(df_resampled):
        print(sec_in_sf*how_long)
        print(len(df_resampled))
        break
    # Get feature
    features.append(df_resampled["burst"][idx:idx+(sec_in_sf*how_long)])        
    #features.append(df["Byte"][idx:idx+(sec_in_sf*how_long)].to_numpy().reshape(sec_in_sf,how_long))        
    # Get label
    labels.append(answer)
#%%
import tensorflow as tf
import pandas as pd
import random
def shuffle_lists_together(list1, list2):
#Shuffles two lists, keeping the correspondence between the elements.
    if len(list1) != len(list2):
        raise ValueError("Both lists must have the same length")
    # Zip the lists together
    zipped_list = list(zip(list1, list2))
    # Shuffle the zipped list
    random.shuffle(zipped_list)
    # Unzip the shuffled list|
    list1_shuffled, list2_shuffled = zip(*zipped_list)
    # Convert the tuples back to lists, if necessary
    return list(list1_shuffled), list(list2_shuffled)

def float_feature(value):
    return tf.train.Feature(float_list=tf.train.FloatList(value=value))

def int64_feature(value):
    return tf.train.Feature(int64_list=tf.train.Int64List(value=[value]))

def serialize_example(feature, label):
    feature_dict = {
        'feature': float_feature(feature),
        'label': int64_feature(label),  # Use int64_feature for integer labels
    }
    # Create a Feature message using tf.train.Example.
    example_proto = tf.train.Example(features=tf.train.Features(feature=feature_dict))
    return example_proto.SerializeToString()

shuffled_features, shuffled_label = shuffle_lists_together(features, labels)
###################################################################
#PLEASE change the address below when you save the tfrecord file!!#
###################################################################
with tf.io.TFRecordWriter(f'./10s_dataset_attack'+".tfrecord") as writer:
    for feature, label in zip(shuffled_features, shuffled_label):
        example = serialize_example(feature, label)
        writer.write(example)

# %%
