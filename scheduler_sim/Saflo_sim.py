#%%
import random
from Path import Path

# general variables
# pacing_rate in Mbps, send buffer in bytes, current_buff in bytes, linger_time in ms
path1 = Path(pacing_rate=15, send_buff_size=3_000_000)
path2 = Path(pacing_rate=20, send_buff_size=3_000_000)
paths = [path1, path2]
traffic_rate = 2  # Mbytes per second
max_sched_per_ms = 20
sim_time = 20  # in seconds
max_burst = 64000  # in bytes

# subflow manager variables
operation_interval = 2 # in seconds
max_p, min_p = 0.7, 0.3

traffic_to_send = 0  # in bytes
wmem1_history = []
wmem2_history = []
time = 0  # in ms

def subflow_manager():
    total_lt = sum(p.linger_time for p in paths)

    if total_lt == 0:
        for p in paths:
            p.enable = random.random() < 0.5
        return

    for p in paths:
        prob = 1 - (p.linger_time / total_lt)
        prob = max(min_p, min(max_p, prob))
        p.enable = random.random() < prob


# Simulation loop
end = sim_time * 1000
while time < end:
    if time % 1000 == 0:
        traffic_to_send += 1048576 * traffic_rate  # add traffic per second
    sched_count = 0

    # Run subflow manager every operation_interval (s)
    if time % (operation_interval*1000) == 0:
        subflow_manager()
    
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
    wmem1_history.append(path1.wmem)
    wmem2_history.append(path2.wmem)

    # time proceed
    time += 1
    print(f"{time}/{end}")

# Final stats
print("Simulation done")
print(f"Path1 sent: {path1.traffic_sent / 1048576:.2f} MB, selected {path1.selection_cnt}")
print(f"Path2 sent: {path2.traffic_sent / 1048576:.2f} MB, selected {path2.selection_cnt}")


import matplotlib.pyplot as plt
plt.figure(figsize=(10, 5))
plt.plot(wmem1_history, label="Path 1 wmem")
plt.plot(wmem2_history, label="Path 2 wmem")
plt.xlabel("Time (ms)")
plt.ylabel("Send buffer occupancy (bytes)")
plt.title("wmem over time")
plt.legend()
plt.grid(True)
plt.tight_layout()
plt.show()
