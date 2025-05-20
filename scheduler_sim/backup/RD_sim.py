#%%
import random
from Path import Path

def run_simulation_rd():
    # general variables
    # pacing_rate in Mbps, send buffer in bytes, current_buff in bytes, linger_time in ms
    path1 = Path(pacing_rate=30, send_buff_size=3_000_000)
    path2 = Path(pacing_rate=30, send_buff_size=3_000_000)
    paths = [path1, path2]
    traffic_rate = 2  # Mbytes per second
    max_sched_per_ms = 20
    sim_time = 20  # in seconds
    max_burst = 64000  # in bytes

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

    # # Final stats
    # print("Simulation done")
    # print(f"Path1 sent: {path1.traffic_sent / 1048576:.2f} MB, selected {path1.selection_cnt}")
    # print(f"Path2 sent: {path2.traffic_sent / 1048576:.2f} MB, selected {path2.selection_cnt}")

    # p1_traffic_trace = [traffic_history1[0]]
    # p2_traffic_trace = [traffic_history2[0]]

    # p1_traffic_trace += [curr - prev for prev, curr in zip(traffic_history1, traffic_history1[1:])]
    # p2_traffic_trace += [curr - prev for prev, curr in zip(traffic_history2, traffic_history2[1:])]

    # import matplotlib.pyplot as plt
    # time_axis = [i * 100 for i in range(len(p1_traffic_trace))]
    # plt.figure(figsize=(10, 5))
    # plt.plot(time_axis, p1_traffic_trace, label="Path 1")
    # plt.plot(time_axis, p2_traffic_trace, label="Path 2")
    # plt.xlabel("Time (ms)")
    # plt.ylabel("Traffic Sent (Bytes per 100 ms)")
    # plt.title("Per-Path Traffic Over Time")
    # plt.legend()
    # plt.grid(True)
    # plt.tight_layout()
    # plt.show()

    return path1.traffic_sent / 1048576, path2.traffic_sent / 1048576

p1, p2= [], []

for i in range(100):
    print(f"{i}-th instance done.")
    tmp1, tmp2 = run_simulation()
    p1.append(tmp1)
    p2.append(tmp2)
