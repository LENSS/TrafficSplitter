class Path:
    def __init__(self, pacing_rate, send_buff_size):
        self.pacing_rate = pacing_rate  # in Mbps
        self.send_buff_size = send_buff_size  # in bytes
        self.wmem = 0
        self.linger_time = 0.0
        self.traffic_sent = 0
        self.selection_cnt = 0
        self.enable = True

    def can_allocate(self, burst):
        return self.send_buff_size - self.wmem >= burst

    def allocate(self, burst):
        if self.can_allocate(burst):
            self.wmem += burst
            self.selection_cnt += 1
            return True
        return False

    def send(self):
        bytes_per_ms = int(self.pacing_rate * 1_000_000 / 8 * 0.001)
        sent = min(bytes_per_ms, self.wmem)
        self.wmem -= sent
        self.traffic_sent += sent
        return sent

    def expected_linger_time(self, burst):
        return ((self.wmem + burst) * 8) / (self.pacing_rate * 1_000_000) * 1000  # ms

    def update_linger_time(self, linger_time=0):
        self.linger_time = linger_time
