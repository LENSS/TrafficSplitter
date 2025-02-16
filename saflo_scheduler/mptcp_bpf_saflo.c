// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2023, SUSE. */

#include "mptcp_bpf.h"
#include <bpf/bpf_tracing.h>
#include <bpf/bpf_helpers.h>
#include <limits.h>

char _license[] SEC("license") = "GPL";

#define MPTCP_SEND_BURST_SIZE	65428

#define min(a, b) ((a) < (b) ? (a) : (b))

struct bpf_subflow_send_info {
	u8 subflow_id;
	u64 linger_time;
};

struct saflo_key_t{
	u32 token;
	s16 local_id;
	u8 remote_id;
};

struct saflo_data_t{
	u32 avg_pacing;
	u32 wmem;
	u64 linger_time;
	bool enabled;
	bool safe;
};

struct detection_key_t{
	u32 token;
	u64 timestamp_ns; 
};

struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 1024);  // Maximum number of entries
    __type(key, struct saflo_key_t);
    __type(value, struct saflo_data_t);
	__uint(pinning, LIBBPF_PIN_BY_NAME); //Not sure HMM
} saflo_map SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 1024);  // Maximum number of entries
    __type(key, struct detection_key_t);
    __type(value, u32);
	__uint(pinning, LIBBPF_PIN_BY_NAME); //Not sure HMM
} detection_map SEC(".maps");

extern bool mptcp_subflow_active(struct mptcp_subflow_context *subflow) __ksym;
extern void mptcp_set_timeout(struct sock *sk) __ksym;
extern __u64 mptcp_wnd_end(const struct mptcp_sock *msk) __ksym;
extern bool tcp_stream_memory_free(const struct sock *sk, int wake) __ksym;
extern bool bpf_mptcp_subflow_queues_empty(struct sock *sk) __ksym;
extern void mptcp_pm_subflow_chk_stale(const struct mptcp_sock *msk, struct sock *ssk) __ksym;

#define SSK_MODE_ACTIVE	0
#define SSK_MODE_BACKUP	1
#define SSK_MODE_MAX	2

static __always_inline __u64 div_u64(__u64 dividend, __u32 divisor)
{
	return dividend / divisor;
}

static __always_inline bool tcp_write_queue_empty(struct sock *sk)
{
	const struct tcp_sock *tp = bpf_skc_to_tcp_sock(sk);

	return tp ? tp->write_seq == tp->snd_nxt : true;
}

static __always_inline bool tcp_rtx_and_write_queues_empty(struct sock *sk)
{
	return bpf_mptcp_subflow_queues_empty(sk) && tcp_write_queue_empty(sk);
}

static __always_inline bool __sk_stream_memory_free(const struct sock *sk, int wake)
{
	if (sk->sk_wmem_queued >= sk->sk_sndbuf)
		return false;

	return tcp_stream_memory_free(sk, wake);
}

static __always_inline bool sk_stream_memory_free(const struct sock *sk)
{
	return __sk_stream_memory_free(sk, 0);
}

SEC("struct_ops")
void BPF_PROG(mptcp_sched_saflo_init, struct mptcp_sock *msk)
{
}

SEC("struct_ops")
void BPF_PROG(mptcp_sched_saflo_release, struct mptcp_sock *msk)
{
}

static int bpf_saflo_get_send(struct mptcp_sock *msk,
			      struct mptcp_sched_data *data)
{
	struct bpf_subflow_send_info send_info[SSK_MODE_MAX];
	struct mptcp_subflow_context *subflow;
	struct sock *sk = (struct sock *)msk;
	__u32 pace, burst, wmem; 
	int i, nr_active = 0;
	__u64 linger_time;
	struct sock *ssk;
	struct detection_key_t detection_key;

	/* pick the subflow with the lower wmem/wspace ratio */
	for (i = 0; i < SSK_MODE_MAX; ++i) {
		send_info[i].subflow_id = MPTCP_SUBFLOWS_MAX;
		send_info[i].linger_time = -1;
	}

	for (i = 0; i < data->subflows && i < MPTCP_SUBFLOWS_MAX; i++) {
		bool backup;
		struct saflo_key_t key = {};
		struct saflo_data_t *value, new_value = {};

		subflow = bpf_mptcp_subflow_ctx_by_pos(data, i);
		if (!subflow)
			break;

		backup = subflow->backup || subflow->request_bkup;

		// Look up eBPF map entity or create it if not exists. 
		key.token = subflow->token;
		key.local_id = subflow->local_id;
		key.remote_id = subflow->remote_id;

		value = bpf_map_lookup_elem(&saflo_map, &key);
		if (!value) {
			// Handle case where the key is not found
			new_value.avg_pacing = 0;
			new_value.wmem = 0;
			new_value.linger_time = 0;
			new_value.enabled = 1;
			new_value.safe = 1;
			// Insert the new key-value pair into the map
			if (bpf_map_update_elem(&saflo_map, &key, &new_value, BPF_ANY) != 0) {
            	bpf_printk("Failed to insert new entry\n");
        	}
			value = &new_value;
		}

		ssk = mptcp_subflow_tcp_sock(subflow);
		if (!mptcp_subflow_active(subflow))
			continue;

		nr_active += !backup;
		pace = subflow->avg_pacing_rate;
		if (!pace) {
			/* init pacing rate from socket */
			subflow->avg_pacing_rate = ssk->sk_pacing_rate;
			pace = subflow->avg_pacing_rate;
			if (!pace)
				continue;
		}

		linger_time = div_u64((__u64)ssk->sk_wmem_queued << 32, pace);
		if (linger_time < send_info[backup].linger_time && value->enabled == 1 && value->safe == 1) {
			send_info[backup].subflow_id = i;
			send_info[backup].linger_time = linger_time;
		}
		
		new_value = *value;
		new_value.avg_pacing = pace;
		new_value.wmem = ssk->sk_wmem_queued;
		new_value.linger_time = linger_time;

		if (bpf_map_update_elem(&saflo_map, &key, &new_value, BPF_ANY) != 0) {
			bpf_printk("Failed to update entry\n");
		}
	}
	mptcp_set_timeout(sk);

	/* pick the best backup if no other subflow is active */
	if (!nr_active)
		send_info[SSK_MODE_ACTIVE].subflow_id = send_info[SSK_MODE_BACKUP].subflow_id;

	subflow = bpf_mptcp_subflow_ctx_by_pos(data, send_info[SSK_MODE_ACTIVE].subflow_id);
	if (!subflow)
		return -1;
	ssk = mptcp_subflow_tcp_sock(subflow);
	if (!ssk || !sk_stream_memory_free(ssk))
		return -1;

	burst = min(MPTCP_SEND_BURST_SIZE, mptcp_wnd_end(msk) - msk->snd_nxt);
	wmem = ssk->sk_wmem_queued;
	if (!burst)
		goto out;

	subflow->avg_pacing_rate = div_u64((__u64)subflow->avg_pacing_rate * wmem +
					   ssk->sk_pacing_rate * burst,
					   burst + wmem);
	msk->snd_burst = burst;

	// Update detection map for traffic tracking
	detection_key.token = subflow->token;
	detection_key.timestamp_ns = bpf_ktime_get_ns();
	//bpf_printk("burst_info_t: %u, %llu, %d", burst_track.token, burst_track.timestamp_ns, burst_track.burst);
	if (bpf_map_update_elem(&detection_map, &detection_key, &burst, BPF_ANY) != 0) {
            	bpf_printk("Failed to insert new entry\n");
	}

out:
	mptcp_subflow_set_scheduled(subflow, true);
	return 0;
}

static int bpf_saflo_get_retrans(struct mptcp_sock *msk,
				 struct mptcp_sched_data *data)
{
	int backup = MPTCP_SUBFLOWS_MAX, pick = MPTCP_SUBFLOWS_MAX, subflow_id;
	struct mptcp_subflow_context *subflow;
	int min_stale_count = INT_MAX;
	struct sock *ssk;

	for (int i = 0; i < data->subflows && i < MPTCP_SUBFLOWS_MAX; i++) {
		subflow = bpf_mptcp_subflow_ctx_by_pos(data, i);
		if (!subflow)
			break;

		if (!mptcp_subflow_active(subflow))
			continue;

		ssk = mptcp_subflow_tcp_sock(subflow);
		/* still data outstanding at TCP level? skip this */
		if (!tcp_rtx_and_write_queues_empty(ssk)) {
			mptcp_pm_subflow_chk_stale(msk, ssk);
			min_stale_count = min(min_stale_count, subflow->stale_count);
			continue;
		}

		if (subflow->backup || subflow->request_bkup) {
			if (backup == MPTCP_SUBFLOWS_MAX)
				backup = i;
			continue;
		}

		if (pick == MPTCP_SUBFLOWS_MAX)
			pick = i;
	}

	if (pick < MPTCP_SUBFLOWS_MAX) {
		subflow_id = pick;
		bpf_printk("retrans selected subflow: %d", subflow_id);
		goto out;
	}
	subflow_id = min_stale_count > 1 ? backup : MPTCP_SUBFLOWS_MAX;

out:
	subflow = bpf_mptcp_subflow_ctx_by_pos(data, subflow_id);
	if (!subflow)
		return -1;
	mptcp_subflow_set_scheduled(subflow, true);
	return 0;
}

SEC("struct_ops")
int BPF_PROG(bpf_saflo_get_subflow, struct mptcp_sock *msk,
	     struct mptcp_sched_data *data)
{
	if (data->reinject)
		return bpf_saflo_get_retrans(msk, data);
	return bpf_saflo_get_send(msk, data);
}

SEC(".struct_ops")
struct mptcp_sched_ops saflo = {
	.init		= (void *)mptcp_sched_saflo_init,
	.release	= (void *)mptcp_sched_saflo_release,
	.get_subflow	= (void *)bpf_saflo_get_subflow,
	.name		= "bpf_saflo",
};
