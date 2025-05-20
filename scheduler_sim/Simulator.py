#%%
import random
from Path import Path
import pandas as pd

# General variables
# pacing_rate in Mbps, send buffer in bytes, current_buff in bytes, linger_time in ms
path1_pacing_rate = 30
path2_pacing_rate = 30
buff_size=3_000_000
traffic_rate = 2  # Mbytes per second
max_sched_per_ms = 20
sim_time = 20  # in seconds
max_burst = 64000  # in bytes

# Saflo subflow manager variables
operation_interval = 2 # in seconds
max_p, min_p = 0.7, 0.3

def run_simulation_rd():
    path1 = Path(pacing_rate=path1_pacing_rate, send_buff_size=buff_size)
    path2 = Path(pacing_rate=path2_pacing_rate, send_buff_size=buff_size)
    paths = [path1, path2]
    traffic_to_send = 0  # in bytes
    traffic_history1 = []
    traffic_history2 = []
    time = 0  # in ms
    end = sim_time * 1000
    # Run
    while time < end:
        if time % 1000 == 0:
            traffic_to_send += 1048576 * traffic_rate  # add traffic per second
        sched_count = 0
        # allocate traffic
        while traffic_to_send > 0 and sched_count < max_sched_per_ms:
            burst = min(max_burst, traffic_to_send)
            try_path = path1 if random.random() < 0.5 else path2

            if try_path.allocate(burst):
                try_path.update_linger_time(try_path.expected_linger_time(burst))
                traffic_to_send -= burst
                sched_count += 1
        # send traffic
        for p in paths:
            p.send()
        # time proceed
        time += 1
        if time%100 == 0:
            traffic_history1.append(path1.traffic_sent)
            traffic_history2.append(path2.traffic_sent)

        #print(f"{time}/{end}")
    return path1.traffic_sent / 1048576, path2.traffic_sent / 1048576

def run_simulation_blest():
    path1 = Path(pacing_rate=path1_pacing_rate, send_buff_size=buff_size)
    path2 = Path(pacing_rate=path2_pacing_rate, send_buff_size=buff_size)
    paths = [path1, path2]

    traffic_to_send = 0  # in bytes
    traffic_history1 = []
    traffic_history2 = []
    time = 0  # in ms
    end = sim_time * 1000
    #Run
    while time < end:
        if time % 1000 == 0:
            traffic_to_send += 1048576 * traffic_rate  # add 1 MB per second
        sched_count = 0        
        # allocate traffic
        while traffic_to_send > 0 and sched_count < max_sched_per_ms:
            sched_count += 1  
            burst = min(max_burst, traffic_to_send)
            # Find the path with minimum expected linger time
            try_path = path1
            final_lt = try_path.expected_linger_time(burst)
            for p in paths:
                lt = p.expected_linger_time(burst)
                if lt < final_lt:
                    try_path = p
                    final_lt = lt
            # Allocate and update
            if try_path.allocate(burst):
                traffic_to_send -= burst
                try_path.update_linger_time(burst)
        # send traffic
        for p in paths:
            p.send()
        # time proceed
        time += 1
        if time%100 == 0:
            traffic_history1.append(path1.traffic_sent)
            traffic_history2.append(path2.traffic_sent)
    #     print(f"{time}/{end}")
    return path1.traffic_sent / 1048576, path2.traffic_sent / 1048576

def saflo_subflow_manager(paths):
    total_lt = sum(p.linger_time for p in paths)

    if total_lt == 0:
        for p in paths:
            p.enable = random.random() < 0.5
        return

    for p in paths:
        prob = 1 - (p.linger_time / total_lt)
        prob = max(min_p, min(max_p, prob))
        p.enable = random.random() < prob

def run_simulation_saflo():
    path1 = Path(pacing_rate=path1_pacing_rate, send_buff_size=buff_size)
    path2 = Path(pacing_rate=path2_pacing_rate, send_buff_size=buff_size)
    paths = [path1, path2]
    traffic_to_send = 0  # in bytes
    traffic_history1 = []
    traffic_history2 = []
    time = 0  # in ms
    end = sim_time * 1000
    # RUN
    while time < end:
        if time % 1000 == 0:
            traffic_to_send += 1048576 * traffic_rate  # add traffic per second
        sched_count = 0
        # Run subflow manager every operation_interval (s)
        if time % (operation_interval*1000) == 0:
            saflo_subflow_manager(paths)
        # Allocate traffic
        while traffic_to_send > 0 and sched_count < max_sched_per_ms:
            sched_count += 1  
            burst = min(max_burst, traffic_to_send)

            # find the path with minimum expected linger time AMONG enabled paths
            candidates = [p for p in paths if p.enable] or paths
            try_path = min(candidates, key=lambda p: p.expected_linger_time(burst))

            # allocate and update
            if try_path.allocate(burst):
                traffic_to_send -= burst
                try_path.update_linger_time(burst)
        # Send traffic
        for p in paths:
            p.send()
        # time proceed
        time += 1
        if time%100 == 0:
            traffic_history1.append(path1.traffic_sent)
            traffic_history2.append(path2.traffic_sent)
    #     print(f"{time}/{end}")
    return path1.traffic_sent / 1048576, path2.traffic_sent / 1048576
# %%
# Store results as a dictionary: {scheduler_name: {path1: [], path2: []}}
results = {
    "saflo": {"path1": [], "path2": []},
    "blest": {"path1": [], "path2": []},
    "rd": {"path1": [], "path2": []},
}

# Map scheduler name to its simulation function
simulation_funcs = {
    "saflo": run_simulation_saflo,
    "blest": run_simulation_blest,
    "rd": run_simulation_rd,
}

# Run all simulations
num_runs = 5000
for name, sim_func in simulation_funcs.items():
    for i in range(num_runs):
        tmp1, tmp2 = sim_func()
        results[name]["path1"].append(tmp1)
        results[name]["path2"].append(tmp2)
        print(f"[{name}] {i+1}/{num_runs} instance done.")

df_list = []
for name, data in results.items():
    for i in range(num_runs):
        df_list.append({"scheduler": name, "path": "Path1", "traffic_sent": data["path1"][i]})
        df_list.append({"scheduler": name, "path": "Path2", "traffic_sent": data["path2"][i]})

df = pd.DataFrame(df_list)

# %%
import seaborn as sns
import matplotlib.pyplot as plt

plt.figure(figsize=(10, 6))
sns.boxplot(x="path", y="traffic_sent", hue="scheduler", data=df)
plt.ylabel("Traffic Sent (MB)")
plt.title("📊 Traffic Distribution per Path (100 Instances)")
plt.grid(True)
plt.tight_layout()
plt.show()
# %%
import numpy as np

plt.figure(figsize=(10, 6))
for (scheduler, path), group in df.groupby(["scheduler", "path"]):
    sorted_vals = np.sort(group["traffic_sent"].values)
    cdf = np.arange(1, len(sorted_vals) + 1) / len(sorted_vals)
    plt.plot(sorted_vals, cdf, label=f"{scheduler} - {path}")

plt.xlabel("Traffic Sent (MB)")
plt.ylabel("CDF")
plt.title("📈 CDF of Traffic Sent per Path Across 100 Runs")
plt.legend()
plt.grid(True)
plt.tight_layout()
plt.show()
#%%
import seaborn as sns
import matplotlib.pyplot as plt

plt.figure(figsize=(10, 6))
sns.ecdfplot(data=df, x="traffic_sent", hue="scheduler", stat="proportion")
plt.title("Smoothed CDF (ECDF) of Traffic Sent")
plt.xlabel("Traffic Sent (MB)")
plt.grid(True)
plt.tight_layout()
plt.show()
# %%
agg = df.groupby(["scheduler", "path"])["traffic_sent"].agg(["mean", "std"]).reset_index()

import numpy as np

# Plot with error bars
plt.figure(figsize=(10, 6))
for path in df["path"].unique():
    subset = agg[agg["path"] == path]
    x = np.arange(len(subset))
    plt.errorbar(x, subset["mean"], yerr=subset["std"], fmt='o', capsize=5, label=path)

plt.xticks(np.arange(len(subset)), subset["scheduler"])
plt.ylabel("Traffic Sent (MB)")
plt.title("📈 Mean ± Std of Traffic Sent per Path and Scheduler")
plt.legend()
plt.grid(True)
plt.tight_layout()
plt.show()

# %%
import seaborn as sns
import matplotlib.pyplot as plt
g = sns.displot(
    data=df,
    x="traffic_sent",
    hue="path",
    col="scheduler",
    bins=20,
    kind="hist",
    multiple="layer",
    facet_kws={"sharex": True, "sharey": True}
)
g.set_axis_labels("Traffic Sent (MB)", "Frequency")
g.fig.suptitle("📊 Histogram per Scheduler", y=1.03)
plt.tight_layout()
plt.show()


# %%
plt.figure(figsize=(10, 6))
sns.histplot(data=df[df["path"] == "Path1"], x="traffic_sent", hue="scheduler", bins=20, multiple="layer")
plt.title("📊 Histogram of Path1 Traffic Sent Across Schedulers")
plt.xlabel("Traffic Sent (MB)")
plt.ylabel("Frequency")
plt.grid(True)
plt.tight_layout()
plt.show()

# %%
import numpy as np
# Assuming 2 entries (Path1, Path2) per run for each scheduler
df["instance"] = df.groupby(["scheduler"]).cumcount() // 2

# Compute CV = std / mean per instance
cv_df = df.groupby(["scheduler", "instance"])["traffic_sent"].agg(["std", "mean"]).reset_index()
cv_df["cv"] = cv_df["std"] / cv_df["mean"]

plt.figure(figsize=(10, 6))
sns.boxplot(data=cv_df, x="scheduler", y="cv")
plt.ylabel("Coefficient of Variation (CV)")
plt.title("📊 Variability of Traffic Distribution per Run (Higher = More Random)")
plt.grid(True)
plt.tight_layout()
plt.show()

#%%
def binary_entropy(p):
    if p in [0, 1]:
        return 0.0
    return -p * np.log2(p) - (1 - p) * np.log2(1 - p)

entropy_records = []

for (scheduler, instance), group in df.groupby(["scheduler", "instance"]):
    p1 = group[group["path"] == "Path1"]["traffic_sent"].values[0]
    p2 = group[group["path"] == "Path2"]["traffic_sent"].values[0]
    total = p1 + p2
    if total == 0:
        entropy = 0.0
    else:
        p = p1 / total
        entropy = binary_entropy(p)
    entropy_records.append({"scheduler": scheduler, "instance": instance, "entropy": entropy})

entropy_df = pd.DataFrame(entropy_records)
plt.figure(figsize=(10, 6))
sns.boxplot(data=entropy_df, x="scheduler", y="entropy")
plt.ylabel("Entropy (bits)")
plt.title("📈 Entropy of Traffic Distribution per Run (Max = 1.0 for 50/50)")
plt.grid(True)
plt.tight_layout()
plt.show()
# %%
