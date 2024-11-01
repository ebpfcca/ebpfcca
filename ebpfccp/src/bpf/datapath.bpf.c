#include "datapath.bpf.h"
#include "vmlinux.h"
#include <bpf/bpf_core_read.h>
#include <bpf/bpf_tracing.h>

// Ring buffer to send signal events to user-land
struct {
  __uint(type, BPF_MAP_TYPE_RINGBUF);
  __uint(max_entries, sizeof(struct signal) * 1024);
} signals SEC(".maps");

void load_signal(struct sock *sk, const struct rate_sample *rs) {
  struct tcp_sock *tp = tcp_sk(sk);
  struct ccp *ca = inet_csk_ca(sk);

  struct signal *sig;

  // Reserve bytes in the ring buffer and get a pointer to the reserved space
  sig = bpf_ringbuf_reserve(&signals, sizeof(struct signal), 0);
  if (!sig)
    return;

  u64 rin = 0;  // send bandwidth in bytes per second
  u64 rout = 0; // recv bandwidth in bytes per second
  u64 ack_us = rs->rcv_interval_us;
  u64 snd_us = rs->snd_interval_us;

  if (ack_us != 0 && snd_us != 0) {
    rin = rout = (u64)rs->delivered * MTU * S_TO_US;
    do_div(rin, snd_us);
    do_div(rout, ack_us);
  }

  sig->bytes_acked = tp->bytes_acked - ca->last_bytes_acked;
  ca->last_bytes_acked = tp->bytes_acked;

  // Submit the reserved space to the ring buffer
  bpf_ringbuf_submit(sig, 0);
}

SEC("struct_ops")
void BPF_PROG(init, struct sock *sk) { return; }

SEC("struct_ops")
void BPF_PROG(cwnd_event, struct sock *sk, enum tcp_ca_event event) { return; }

SEC("struct_ops")
void BPF_PROG(cong_control, struct sock *sk, const struct rate_sample *rs) {
  load_signal(sk, rs);
}

SEC("struct_ops")
__u32 BPF_PROG(ssthresh, struct sock *sk) { return 0; }

SEC("struct_ops")
void BPF_PROG(set_state, struct sock *sk, __u8 new_state) { return; }

SEC("struct_ops")
void BPF_PROG(pckts_acked, struct sock *sk, const struct ack_sample *sample) {
  return;
}

SEC("struct_ops")
__u32 BPF_PROG(undo_cwnd, struct sock *sk) { return 0; }

SEC(".struct_ops")
struct tcp_congestion_ops ebpfccp = {
    .init = (void *)init,
    .ssthresh = (void *)ssthresh,
    .cong_control = (void *)cong_control,
    .set_state = (void *)set_state,
    .undo_cwnd = (void *)undo_cwnd,
    .cwnd_event = (void *)cwnd_event,
    .pkts_acked = (void *)pckts_acked,
    .get_info = NULL,
    .release = NULL,
    .name = "ebpfccp",
};
