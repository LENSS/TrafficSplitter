#%%
import random
from Path import Path

# pacing_rate in Mbps, send buffer in bytes, current_buff in bytes, linger_time in ms
path1 = Path(pacing_rate=15, send_buff_size=3_000_000)
path2 = Path(pacing_rate=20, send_buff_size=3_000_000)
paths = [path1, path2]
traffic_rate = 2  # Mbytes per second
max_sched_per_ms = 20
sim_time = 20  # in seconds
max_burst = 64000  # in bytes


traffic_to_send = 0  # in bytes
wmem1_history = []
wmem2_history = []
time = 0  # in ms

# Simulation loop
end = sim_time * 1000
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
# %%
