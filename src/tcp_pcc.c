/* PCC alegro:
 * This is draft of PCC v1, also known as PCC alegro.
 * Main issues are:
 * 	Lack of order (not everything is documented, lots of debug print, some
 * 	magic numbers, etc.)
 * 	Doesn't work too well when the delay is low and the rate is high.
 */ 

#include <linux/module.h>
#include <net/tcp.h>
#include <linux/random.h>

/* Ignore the first and last packets of every interval because they might
 * contain data of the previous / next interval. */
static const u32 pcc_ignore_packets = 5;

/* Interval should have a minimal number of segs, otherwise, it is not possible
 * to calculate stats about it. This length include two ignore part, one in the
 * beginning and one in the end. It limits the interval data to at least 20 segs,
 * which gives the ability to notice 5% loss */
static const u32 pcc_interval_min_segs = 40;

/* Epsilon is between 1% to 5%, and in granularity of 1% */
static const u32 pcc_epsilon_max = 5;
static const u32 pcc_epsilon_min = 1;
static const u32 pcc_epsilon_part = 100;

/* PCC have 4 intervals, 2 for higher rate and 2 for lower rate */
static const u32 pcc_intervals = 4;

/* PCC minimum rate is 1Kbps */
static const u64 pcc_rate_minimum = 1024;

/* PCC allows for 5% loss before drastic utility decreasment */
static const u32 pcc_loss_margin = 5;
/* PCC utility function const: alpha parameter for the sigmoid function */
static const u32 pcc_alpha = 100;

/* PCC utility function rounding factor. The utility function is rounded up to 
 * 1/pcc_rounding_factor */
static const u32 pcc_rounding_factor = 1000;

/* Limit the input sent to calculate sigmoid on, to avoid long calculation that
 * will result in zero */
static const u32 pcc_max_loss = 10;

static const s32 pcc_slow_start_threshold = 750;
static const s32 pcc_slow_start_threshold_base = 1000;

enum PCC_DECISION {
	PCC_RATE_UP,
	PCC_RATE_DOWN,
	PCC_RATE_STAY,
};

enum PCC_MODE {
	PCC_SLOW_START, 
	PCC_DECISION_MAKING,
	PCC_RATE_ADJUSMENT,
	PCC_LOSS, /* When tcp is in loss state, its stats can't be trusted */
};

/* Contains the statistics from one "experiment" interval */
struct pcc_interval {
	u64 rate;

	u32 segs_sent_start;
	u32 segs_sent_end;

	s64 utility;
	u32 lost;
	u32 delivered;
};

static int id = 0;
struct pcc_data {
	struct pcc_interval *intervals;
	struct pcc_interval *single_interval;
	int send_index;
	int recive_index;

	enum PCC_MODE mode;
	u64 rate;
	u64 last_rate;
	u32 epsilon;
	bool wait_mode;

	enum PCC_DECISION last_decision;
	u32 lost_base;
	u32 delivered_base;

	// debug helpers
	int id;
	int decisions_count;

	u32 segs_sent;
	u32 packets_counted;
	u32 double_counted;

};

/*********************
 * Getters / Setters *
 * ******************/
static u32 pcc_get_rtt(struct tcp_sock *tp)
{
        /* Get initial RTT - as measured by SYN -> SYN-ACK.
         * If information does not exist - use 1ms as a "LAN RTT".
         */
        if (tp->srtt_us) {
                return max(tp->srtt_us >> 3, 1U);
        } else {
                return USEC_PER_MSEC;
        }
}

/* Initialize cwnd to support current pacing rate (but not less then 4 packets)
 */
static void pcc_set_cwnd(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
        u64 cwnd = sk->sk_pacing_rate;

        cwnd *= pcc_get_rtt(tcp_sk(sk));
	cwnd /= tp->mss_cache;
        cwnd /= USEC_PER_SEC;
        cwnd *= 2;
	cwnd = max(4ULL, cwnd);
        cwnd = min((u32)cwnd, tp->snd_cwnd_clamp); /* apply cap */	
        tp->snd_cwnd = cwnd;
}


/* was the pcc struct fully inited */
static bool pcc_valid(struct pcc_data *pcc)
{
	return (pcc && pcc->intervals);
}

/******************
 * Intervals init *
 * ****************/
static void pcc_setup_decision_making_intervals(struct pcc_data *pcc)
{
	u64 rate_low, rate_high;
	char rand;
	int i;

	get_random_bytes(&rand, 1);
	rate_high = pcc->rate * (pcc_epsilon_part + pcc->epsilon);
	rate_low = pcc->rate * (pcc_epsilon_part - pcc->epsilon);

	rate_high /= pcc_epsilon_part;
	rate_low /= pcc_epsilon_part;

	for (i = 0; i < pcc_intervals; i += 2) {
		if ((rand >> (i / 2)) & 1) {
			pcc->intervals[i].rate = rate_low;
			pcc->intervals[i + 1].rate = rate_high;
		} else {
			pcc->intervals[i].rate = rate_high;
			pcc->intervals[i + 1].rate = rate_low;
		}

		pcc->intervals[i].segs_sent_start = 0;
		pcc->intervals[i + 1].segs_sent_start = 0;
	}
}

static void pcc_setup_intervals(struct pcc_data *pcc)
{
	switch (pcc->mode) {
	case PCC_DECISION_MAKING:
		pcc_setup_decision_making_intervals(pcc);
		break;
	case PCC_RATE_ADJUSMENT:
	case PCC_SLOW_START:
		pcc->single_interval->rate = pcc->rate;
	default:
		break;
	}
	pcc->send_index = 0;
	pcc->recive_index = 0;
	pcc->wait_mode = false;
}

static void pcc_start_interval(struct sock *sk, struct pcc_data *pcc)
{
	struct pcc_interval *interval;
	u64 rate = pcc->rate;

	if (!pcc->wait_mode) {
		interval = &pcc->intervals[pcc->send_index];
		interval->segs_sent_end = 0;
		interval->lost = 0;
		interval->delivered = 0;
		interval->segs_sent_start = tcp_sk(sk)->data_segs_out;
		interval->segs_sent_start = max(interval->segs_sent_start, 1U);
		rate = interval->rate;
	}

	rate = min(rate, (u64)sk->sk_max_pacing_rate);
	rate = max(rate, pcc_rate_minimum);
	sk->sk_pacing_rate = rate;
	pcc_set_cwnd(sk);
}

/************************
 * Utility and decisions *
 * **********************/
/* get x = number * pcc_rounding_factor, return 
 * (e ^ number) * pcc_rounding_factor
 */
static u32 pcc_exp(s32 x)
{
	s64 temp = pcc_rounding_factor;
	s64 exp = pcc_rounding_factor;
	int i;

	for (i = 1; temp != 0; i++) {
		temp *= x;
		temp /= i;
		temp /= pcc_rounding_factor;
		exp += temp;
	}
	return exp;
}

static void pcc_calc_utility(struct pcc_interval *interval)
{
	s64 loss_ratio, delivered, lost, rate, util;

	lost = interval->lost;
	delivered = interval->delivered;
	rate = interval->rate;
	if (!(lost+ delivered)) {
		interval->utility = S64_MIN;
		return;
	}

	/* loss rate = lost packets / all packets counted *100 * FACTOR_1 */
	loss_ratio = (lost * pcc_rounding_factor * pcc_alpha) /
		     (lost+ delivered);
	
	/* util = delivered rate / (1 + e^(100*loss_rate)) - lost_ratio * rate
	 */
	util = loss_ratio- (pcc_loss_margin * pcc_rounding_factor);
	if (util < pcc_max_loss*pcc_rounding_factor)
		util = (rate * pcc_rounding_factor) /
		       (pcc_exp(util) + pcc_rounding_factor);
	else
		util = 0;

	/* util *= goodput */
	util *= (pcc_rounding_factor * pcc_alpha) - loss_ratio;
	util /= pcc_rounding_factor * pcc_alpha;
	/* util -= "wasted rate" */
	util -= (rate * loss_ratio) / (pcc_alpha * pcc_rounding_factor);

	printk(KERN_INFO
		"rate %lld sent %u delv %lld lost %lld util %lld \n",
		 rate, interval->segs_sent_end - interval->segs_sent_start,
		 delivered, lost, util);
	interval->utility = util;
}

static void pcc_increase_epsilon(struct pcc_data *pcc)
{
	if (pcc->epsilon < pcc_epsilon_max)
		pcc->epsilon++;
}

static enum PCC_DECISION
pcc_get_decision(struct pcc_data *pcc, u64 new_rate)
{
	if (pcc->rate == new_rate)
		return PCC_RATE_STAY;

	return pcc->rate < new_rate ? PCC_RATE_UP : PCC_RATE_DOWN;
}

static void
pcc_change_epsilon_after_dicision(struct pcc_data *pcc, u64 new_rate)
{
	enum PCC_DECISION decision = pcc_get_decision(pcc, new_rate);

	if (decision == pcc->last_decision)
		pcc_increase_epsilon(pcc);
	else
		pcc->epsilon = pcc_epsilon_min;

	pcc->last_decision = decision;
}

static u64 pcc_decide_rate(struct pcc_data *pcc)
{
	bool run_1_res, run_2_res, did_agree;

	run_1_res = pcc->intervals[0].utility > pcc->intervals[1].utility;
	run_2_res = pcc->intervals[2].utility > pcc->intervals[3].utility;

	/* did_agree: was the 2 sets of intervals with the same result */
	did_agree = !((run_1_res == run_2_res) ^ 
		    (pcc->intervals[0].rate == pcc->intervals[2].rate));
	if (did_agree)
		pcc->mode = PCC_RATE_ADJUSMENT;

	if (did_agree)
		return run_1_res ? pcc->intervals[0].rate :
				   pcc->intervals[1].rate;
	else
		return pcc->rate;

}

static void pcc_decide(struct pcc_data *pcc, struct tcp_sock *tsk)
{
	u64 new_rate;
	int i;

	for (i = 0; i < pcc_intervals; i++ )
		pcc_calc_utility(&pcc->intervals[i]);

	new_rate = pcc_decide_rate(pcc);
	pcc_change_epsilon_after_dicision(pcc, new_rate);

	if (new_rate != pcc->rate)
		printk(KERN_INFO "%d decide: on new rate %d %lld (%d) %d\n",
		       pcc->id, pcc->rate < new_rate, new_rate,
		       pcc->decisions_count, pcc_get_rtt(tsk));
	else
		printk(KERN_INFO "%d decide: stay %lld (%d) %d\n", pcc->id,
			pcc->rate, pcc->decisions_count, pcc_get_rtt(tsk));

	if (pcc->mode == PCC_RATE_ADJUSMENT) {
		pcc->last_rate = new_rate;
		pcc->rate = new_rate;
		new_rate *= pcc->epsilon;
		new_rate /= pcc_epsilon_part;
		if (pcc->last_decision == PCC_RATE_DOWN)
			pcc->rate -= new_rate;
		else
			pcc->rate += new_rate;
	}
	pcc->decisions_count+=4;
}


static void pcc_decide_rate_adjusment(struct pcc_data *pcc)
{
	struct pcc_interval *interval = &pcc->intervals[0];
	s64 prev, extra_rate;

	prev = interval->utility;
	pcc_calc_utility(interval);

	printk(KERN_INFO "%d: adj mode: rate %lld utility %lld (%d)\n",
		pcc->id, interval->rate, interval->utility, pcc->decisions_count);
	if (prev < interval->utility) {
		pcc_increase_epsilon(pcc);
		extra_rate = (pcc->rate * pcc->epsilon) / pcc_epsilon_part;
		pcc->last_rate = pcc->rate;
		if (pcc->last_decision == PCC_RATE_UP)
			pcc->rate += extra_rate;
		else
			pcc->rate -= extra_rate;
	} else {
		pcc->rate = pcc->last_rate;
		pcc->epsilon = pcc_epsilon_min;
		pcc->mode = PCC_DECISION_MAKING;
		printk(KERN_INFO "%d: adj mode ended\n", pcc->id);
	}
	pcc->decisions_count++;
}

static s64 pcc_adjust_utility(s64 utility, u32 rate, bool threshold)
{
	if (utility == S64_MIN)
		return S64_MIN;

	return utility * (threshold ? pcc_slow_start_threshold_base :
					pcc_slow_start_threshold) / rate;
}
static void pcc_decide_slow_start(struct pcc_data *pcc, struct tcp_sock *tsk)
{
	struct pcc_interval *interval = pcc->single_interval;
	s64 adjust_utility, prev_adjust_utility;
	u64 extra_rate;

	prev_adjust_utility = pcc_adjust_utility(interval->utility,
						 pcc->last_rate,
						 interval->utility < 0);
	pcc_calc_utility(interval);
	adjust_utility = pcc_adjust_utility(interval->utility, pcc->rate,
					interval->utility < 0);

	printk(KERN_INFO "%d: start mode: rate %lld utility %lld (%lld, %lld)\n",
		pcc->id, pcc->rate, interval->utility,
		prev_adjust_utility, adjust_utility);
	if (adjust_utility > prev_adjust_utility) {
		pcc->last_rate = pcc->rate;
		extra_rate = pcc->intervals[0].delivered * tsk->mss_cache;
		extra_rate = min(extra_rate, pcc->rate);
		pcc->rate += extra_rate;
	} else {
		pcc->rate = pcc->last_rate;
		pcc->mode = PCC_DECISION_MAKING;
		printk(KERN_INFO "%d: start mode ended\n", pcc->id);
	}
}

/**************************
 * intervals & sample:
 * was started, was ended,
 * find interval per sample
 * ************************/
bool send_interval_ended(struct pcc_interval *interval, struct tcp_sock *tsk,
			 struct pcc_data *pcc)
{
	int segs_sent = tsk->data_segs_out - interval->segs_sent_start;

	if (pcc->mode != PCC_DECISION_MAKING)
		segs_sent += pcc_ignore_packets;

	if (segs_sent < pcc_interval_min_segs)
		return false;

	if (pcc->packets_counted > interval->segs_sent_start ) {
		interval->segs_sent_end = tsk->data_segs_out;
		return true;
	}
	return false;
}

bool recive_interval_ended(struct pcc_interval *interval,
			   struct tcp_sock *tsk, struct pcc_data *pcc)
{
	return interval->segs_sent_end &&
	       interval->segs_sent_end - pcc_ignore_packets <
							pcc->packets_counted;
}

static void start_next_send_interval(struct sock *sk, struct pcc_data *pcc)
{
	pcc->send_index++;
	if (pcc->send_index == pcc_intervals ||
	    pcc->mode != PCC_DECISION_MAKING)
		pcc->wait_mode = true;

	pcc_start_interval(sk, pcc);
}

static void
pcc_update_interval(struct pcc_interval *interval,  struct pcc_data *pcc,
		    struct sock *sk, const struct rate_sample *rs)
{
	interval->lost += tcp_sk(sk)->lost - pcc->lost_base;
	interval->delivered += tcp_sk(sk)->delivered - pcc->delivered_base;
}

static void pcc_process_sample(struct sock *sk, const struct rate_sample *rs)
{
	struct pcc_data *pcc = inet_csk_ca(sk);
	struct tcp_sock *tsk = tcp_sk(sk);
	struct pcc_interval *interval;
	int index;
	u32 before;

	if (!pcc_valid(pcc))
		return;

	pcc_set_cwnd(sk);
	if (pcc->mode == PCC_LOSS)
		goto end;

	if (!pcc->wait_mode) {
		if (send_interval_ended(&pcc->intervals[pcc->send_index], tsk,
					pcc))
			start_next_send_interval(sk, pcc);
	}

	index = pcc->recive_index;
	interval = &pcc->intervals[index];
	before = pcc->packets_counted;
	pcc->packets_counted =  tsk->delivered + tsk->lost -
				pcc->double_counted;

	if (!interval->segs_sent_start)
		goto end;

	if (before > pcc_ignore_packets + interval->segs_sent_start)
		pcc_update_interval(interval, pcc, sk, rs);
	
	if (recive_interval_ended(interval, tsk, pcc)) {
		pcc->recive_index++;
		switch (pcc->mode) {
		case PCC_SLOW_START:
			pcc_decide_slow_start(pcc, tsk);
			break;
		case PCC_RATE_ADJUSMENT:
			pcc_decide_rate_adjusment(pcc);
			break;
		case PCC_DECISION_MAKING:
			if (pcc->recive_index == pcc_intervals)
				pcc_decide(pcc, tsk);
			else
				goto end;
		default:
			break;
		}
		pcc_setup_intervals(pcc);
		pcc_start_interval(sk, pcc);

	}

end:
	pcc->lost_base = tsk->lost;
	pcc->delivered_base = tsk->delivered;
}

static void pcc_init(struct sock *sk)
{
	struct pcc_data *pcc = inet_csk_ca(sk);

	pcc->intervals = kzalloc(sizeof(struct pcc_interval) *pcc_intervals,
				 GFP_KERNEL);
	if (!pcc->intervals) {
		printk(KERN_INFO "init fails\n");
		return;
	}

	pcc->single_interval = &pcc->intervals[0];
	id++;
	pcc->id = id;
	pcc->epsilon = pcc_epsilon_min;
	pcc->rate = pcc_rate_minimum*512;
	pcc->last_rate = pcc_rate_minimum*512;
	tcp_sk(sk)->snd_ssthresh = TCP_INFINITE_SSTHRESH;
	pcc->mode = PCC_SLOW_START;
	pcc->intervals[0].utility = S64_MIN;

	pcc_setup_intervals(pcc);
	pcc_start_interval(sk,pcc);
	cmpxchg(&sk->sk_pacing_status, SK_PACING_NONE, SK_PACING_NEEDED);
}

static void pcc_release(struct sock *sk)
{
	struct pcc_data *pcc = inet_csk_ca(sk);

	kfree(pcc->intervals);
}

/* PCC does not need to undo the cwnd since it does not
 * always reduce cwnd on losses (see pcc_main()). Keep it for now.
 */
static u32 pcc_undo_cwnd(struct sock *sk)
{
        return tcp_sk(sk)->snd_cwnd;
}

static u32 pcc_ssthresh(struct sock *sk)
{
        return TCP_INFINITE_SSTHRESH; /* PCC does not use ssthresh */
}

static void pcc_set_state(struct sock *sk, u8 new_state)
{
	struct pcc_data *pcc = inet_csk_ca(sk);
	s32 double_counted;

	if (!pcc_valid(pcc))
		return;

	if (pcc->mode == PCC_LOSS && new_state != 4) {
		double_counted = tcp_sk(sk)->delivered + tcp_sk(sk)->lost+
			tcp_packets_in_flight(tcp_sk(sk));
		double_counted -= tcp_sk(sk)->data_segs_out;
		double_counted -= pcc->double_counted;
		pcc->double_counted+= double_counted;
		printk(KERN_INFO "%d loss ended: double_counted %d\n",
		       pcc->id, double_counted);

		pcc->mode = PCC_DECISION_MAKING;
		pcc_setup_intervals(pcc);
		pcc_start_interval(sk, pcc);
	} else if (pcc->mode != PCC_LOSS && new_state  == 4) {
		printk(KERN_INFO "%d loss: started\n", pcc->id);
		pcc->mode = PCC_LOSS;
		pcc->wait_mode = true;
		pcc_start_interval(sk, pcc);
	} else {
		pcc_set_cwnd(sk);
	}
}

static void pcc_cong_avoid(struct sock *sk, u32 ack, u32 acked)
{
}

static void pcc_pkts_acked(struct sock *sk, const struct ack_sample *acks)
{
}
static void pcc_ack_event(struct sock *sk, u32 flags)
{

}


static void pcc_cwnd_event(struct sock *sk, enum tcp_ca_event event)
{
}

static struct tcp_congestion_ops tcp_pcc_cong_ops __read_mostly = {
        .flags = TCP_CONG_NON_RESTRICTED,
        .name = "pcc",
        .owner = THIS_MODULE,
        .init = pcc_init,
        .release        = pcc_release,
        .cong_control = pcc_process_sample,
        /* Keep the windows static */
        .undo_cwnd = pcc_undo_cwnd,
        /* Slow start threshold will not exist */
        .ssthresh = pcc_ssthresh,
	.set_state	= pcc_set_state,
	.cong_avoid = pcc_cong_avoid,
	.pkts_acked = pcc_pkts_acked,
	.in_ack_event = pcc_ack_event,
	.cwnd_event	= pcc_cwnd_event,
};

/* Kernel module section */

static int __init pcc_register(void)
{
        BUILD_BUG_ON(sizeof(struct pcc_data) > ICSK_CA_PRIV_SIZE);
	printk(KERN_INFO "pcc init reg\n");
        return tcp_register_congestion_control(&tcp_pcc_cong_ops);
}

static void __exit pcc_unregister(void)
{
        tcp_unregister_congestion_control(&tcp_pcc_cong_ops);
}

module_init(pcc_register);
module_exit(pcc_unregister);

MODULE_AUTHOR("Nogah Frankel <nogah.frankel@gmail.com>");
MODULE_LICENSE("Dual BSD/GPL");
MODULE_DESCRIPTION("TCP PCC (Performance-oriented Congestion Control)");


