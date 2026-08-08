// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2022, SUSE. */

#include "mptcp_bpf.h"
#include <bpf/bpf_tracing.h>

char _license[] SEC("license") = "GPL";

SEC("struct_ops")
void BPF_PROG(mptcp_sched_ran_init, struct mptcp_sock *msk)
{
}

SEC("struct_ops")
void BPF_PROG(mptcp_sched_ran_release, struct mptcp_sock *msk)
{
}

SEC("struct_ops")
int BPF_PROG(bpf_red_get_subflow, struct mptcp_sock *msk,
	     struct mptcp_sched_data *data)
{
    unsigned int random_value;
    random_value = bpf_get_prandom_u32();

	for (int i = 0; i < data->subflows && i < MPTCP_SUBFLOWS_MAX; i++) {
		if (!bpf_mptcp_subflow_ctx_by_pos(data, i))
			break;
        if ((random_value % data->subflows) == i) {
            mptcp_subflow_set_scheduled(bpf_mptcp_subflow_ctx_by_pos(data, i), true);
            return 0; // Successfully scheduled
        }
	}
	return 0;
}

SEC(".struct_ops")
struct mptcp_sched_ops red = {
	.init		= (void *)mptcp_sched_ran_init,
	.release	= (void *)mptcp_sched_ran_release,
	.get_subflow	= (void *)bpf_red_get_subflow,
	.name		= "bpf_ran",
};
