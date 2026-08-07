#%%
import pandas as pd
import tensorflow as tf
import numpy as np
from multiprocessing import shared_memory, Lock
import time as te
import subprocess
import re

MAX_TOKENS = 1000
def extract_tokens_as_int():
    command = "ss --mptcp -i | grep token"
    tokens = []
    try:
        # Run the command and capture its output
        process = subprocess.Popen(command, shell=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        stdout, stderr = process.communicate()

        if process.returncode != 0:
            print(f"Command failed with error: {stderr.strip()}")
            return None

        # Process each line of the command output
        for line in stdout.splitlines():
            # Find "token:" in the line
            match = re.search(r'token:([0-9a-fA-F]+)', line)
            if match:
                token_str = match.group(1)  # Extract the hexadecimal token
                try:
                    # Convert the token to an unsigned integer (base 16)
                    token_value = int(token_str, 16)
                    tokens.append(token_value)
                    
                    # Limit the number of tokens to MAX_TOKENS
                    if len(tokens) >= MAX_TOKENS:
                        break
                except ValueError:
                    print(f"Failed to convert token '{token_str}' to uint32.")
                    continue

    except Exception as e:
        print(f"An error occurred: {e}")
        return None

    return tokens

# Function to write to shared memory
def write_to_shared_memory(data, lock):
    with lock:  # Ensure only one process writes at a time
        for i, value in enumerate(data):
            buffer[i] = value
        #print(f"Written to shared memory: {data}")

# Function to read from shared memory
def read_from_shared_memory(lock):
    with lock:  # Ensure consistent reads
        data = buffer[:1000]
        #print(f"Read from shared memory: {data.tolist()}")
    return data.tolist()

# Delete the old shm if it exists and create the new shm
shm_name = "shm_detect"  # Shared memory name
shm_size = 1000  # Number of integers
shm_size = shm_size * np.dtype(np.uint32).itemsize
try:
    existing_shm = shared_memory.SharedMemory(name=shm_name)
    existing_shm.unlink()  # Unlink removes the shared memory block
    existing_shm.close()
    print(f"Existing shared memory '{shm_name}' deleted.")
except FileNotFoundError:
    print(f"No existing shared memory found with name '{shm_name}'.")
shm = shared_memory.SharedMemory(name=shm_name, create=True, size=shm_size)
lock = Lock()

# Load the detector modules
interpreter_main = tf.lite.Interpreter(model_path="detector_10s_main.tflite")
interpreter_main.allocate_tensors()
input_details_main = interpreter_main.get_input_details()
output_details_main = interpreter_main.get_output_details()

interpreter_sub = tf.lite.Interpreter(model_path="detector_10s_sub.tflite")
interpreter_sub.allocate_tensors()
input_details_sub = interpreter_sub.get_input_details()
output_details_sub = interpreter_sub.get_output_details()

# Main operation start
while(True):
    ####################################################################
    # Detection (Inference)
    ####################################################################
    # Open the file and organize data with token
    raw_data = {}
    try:
        with open("detection.log", "r") as file:
            for line in file:
                info = line.split(" ")
                token = int(info[0].split(":")[-1])
                time = int(info[1].split(":")[-1])
                burst = int(info[2].split(":")[-1].strip())

                if raw_data.get(token) is None:
                    raw_data[token] = {}
                    raw_data[token]["time"] = []
                    raw_data[token]["burst"] = []
                raw_data[token]["time"].append(time)
                raw_data[token]["burst"].append(burst)
    except:
        print("There is no detection log file. Wait 2s and try again.")
        te.sleep(2)
        continue
    
    # Iterate tokens and get the detection results.
    malicious_token_main = []
    malicious_token_sub = []
    tokens = extract_tokens_as_int()
    for t in raw_data:
        if tokens == None:
            print("There are no MPTCP connections. Wait 2s and try again.")
            te.sleep(2)
            continue
        # Skip the closed tokens
        if t not in tokens:
            continue
        features = []
        results_main = []
        results_sub = []
        # Reframing 
        df = pd.DataFrame(raw_data[t])
        # Convert kernel ns to datetime (ignoring rows with 0 timestamps)
        df['time'] = pd.to_datetime(df['time'], unit='ns', errors='coerce')
        df.set_index('time', inplace=True)
        df_resampled = df.resample('10ms').sum()
        df_resampled['burst'] = df_resampled['burst'].fillna(0)
        df_resampled.reset_index(inplace=True)
        if len(df_resampled["burst"]) < 1000:
            continue
        df_resampled['burst'] = df_resampled['burst'].clip(upper=5000000)

        # Slice data into features 
        moving_window = 100
        sec_in_sf = 100 # 1 value = 10ms -> 100 value = 1s  
        how_long = 10 # This represents time length of one data feature. If it is 15, then one feature includes 15 seconds of traffic info. 
        for idx in range(0, len(df_resampled), moving_window):
            # Check if there is an enough number of datapoints for one feature        
            if idx + sec_in_sf*how_long > len(df_resampled):
                #print(sec_in_sf*how_long)
                #print(len(df_resampled))
                break
            # Get feature
            features.append(df_resampled["burst"][idx:idx+(sec_in_sf*how_long)])

        # Feed features into the detector module. 
        for each in features:
            # Convert the pandas.Series to a numpy array and preprocess
            input_array = each.to_numpy().astype(np.float32)  # Convert to float32
            input_array = np.expand_dims(input_array, axis=-1)  # Add a channel dimension
            input_array = np.expand_dims(input_array, axis=0)   # Add a batch dimension
            
            # Ensure the input matches the model's expected shape
            if input_array.shape != tuple(input_details_main[0]['shape']):
                raise ValueError(f"Input shape {input_array.shape} does not match model input shape {input_details_main[0]['shape']}")

            # Set the input tensor
            interpreter_main.set_tensor(input_details_main[0]['index'], input_array)
            interpreter_sub.set_tensor(input_details_sub[0]['index'], input_array)

            # Run inference
            interpreter_main.invoke()
            interpreter_sub.invoke()

            # Get the output tensor
            output_data_main = interpreter_main.get_tensor(output_details_main[0]['index'])
            output_data_sub = interpreter_sub.get_tensor(output_details_sub[0]['index'])

            # Append the result to the results list
            results_main.append(output_data_main)
            results_sub.append(output_data_main)
        
        if sum(results_main)/len(features) >= 0.5:
            None
            #malicious_token_main.append(t)
        if sum(results_sub)/len(features) >= 0.5:
            None
            #malicious_token_sub.append(t)

    #%%
    ####################################################################
    # Update shared memory
    ####################################################################
    # Create a NumPy array backed by shared memory
    buffer = np.ndarray((1000,), dtype=np.uint32, buffer=shm.buf)

    # Read shm and update the new malicious token.
    data = read_from_shared_memory(lock)
    tokens = extract_tokens_as_int()
    if tokens == None:
        print("There are no MPTCP connections. Wait 2s and try again.")
        te.sleep(2)
        continue

    for ex in data:
        # ex = token in the shared memory
        if ex == 0:
            break
        elif ex not in tokens:
            # closed token
            continue
        elif ex not in malicious_token_main:
            malicious_token_main.append(ex)

    if len(malicious_token_main) > 0:
        for ex in data:
            # ex = token in the shared memory
            if ex == 0:
                break
            elif ex not in tokens:
                # closed token
                continue
            elif ex not in malicious_token_sub:
                malicious_token_sub.append(ex)

    malicious_token_total = malicious_token_main+malicious_token_sub 

    s = ""
    write_to_shared_memory(malicious_token_total, lock)
    for each in malicious_token_total:
        if each == 0:
            break
        s += f"{each} "        
    print(f"Malicious tokens: {s}")
        
    te.sleep(5)
    # shm.close()
    # shm.unlink()
