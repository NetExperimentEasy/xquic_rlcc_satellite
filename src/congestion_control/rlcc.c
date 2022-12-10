/**
 * @copyright Copyright (c) 2022, jinyaoliu 
 */
#include "src/congestion_control/rlcc.h"
#include <math.h>
#include "src/common/xqc_time.h"
#include <xquic/xquic.h>
#include <xquic/xquic_typedef.h>
#include "src/congestion_control/xqc_sample.h"
#include "pthread.h"
#include <time.h>
#include <hiredis/hiredis.h>  

#define XQC_RLCC_MSS               	1460
#define MONITOR_INTERVAL           	100
#define XQC_RLCC_INIT_WIN          	(32 * XQC_RLCC_MSS)
#define XQC_RLCC_MIN_WINDOW        	(4 * XQC_RLCC_MSS )
#define CWND_GAIN					1.2
#define XQC_RLCC_INF             	0x7fffffff
#define SAMPLE_INTERVAL				100000		// 100ms
// #define SAMPLE_INTERVAL				1000000		// 1000ms
#define PROBE_INTERVAL				2000000		// 2s

const float xqc_rlcc_init_pacing_gain = 2.885;
const uint32_t max_xqc = 2147483647;

static pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t mutex_lock = PTHREAD_MUTEX_INITIALIZER;

static void 
xqc_rlcc_cac_pacing_rate_by_cwnd(xqc_rlcc_t *rlcc)
{	
    rlcc->pacing_rate = (rlcc->cwnd / (rlcc->srtt ? rlcc->srtt : 1000)) * (uint64_t)MSEC2SEC;
	// rlcc->pacing_rate = xqc_max(rlcc->pacing_rate, XQC_RLCC_MSS);
	rlcc->pacing_rate = xqc_clamp(rlcc->pacing_rate, XQC_RLCC_MSS, max_xqc);
	return;
}

static void 
xqc_rlcc_cac_cwnd_by_pacing_rate(xqc_rlcc_t *rlcc)
{	
    rlcc->cwnd = CWND_GAIN * (rlcc->pacing_rate/(uint64_t)MSEC2SEC) * (rlcc->srtt ? rlcc->srtt : 1000);
	// rlcc->cwnd = xqc_max(rlcc->cwnd, XQC_RLCC_MIN_WINDOW);
	rlcc->cwnd = xqc_clamp(rlcc->cwnd, XQC_RLCC_MIN_WINDOW, max_xqc);
	return;
}

static void
getRedisConn(xqc_rlcc_t* rlcc)
{
	rlcc->redis_conn_listener = redisConnect(rlcc->redis_host, rlcc->redis_port);
	rlcc->redis_conn_publisher = redisConnect(rlcc->redis_host, rlcc->redis_port);

	if(!rlcc->redis_conn_listener || !rlcc->redis_conn_publisher){
		printf("redisConnect error\n");
	}else if(rlcc->redis_conn_listener->err){
		printf("connection error:%s\n", rlcc->redis_conn_listener->errstr);
		redisFree(rlcc->redis_conn_listener);
	}else if(rlcc->redis_conn_publisher->err){
		printf("connection error:%s\n", rlcc->redis_conn_publisher->errstr);
		redisFree(rlcc->redis_conn_publisher);
	}

	return;
}

static void
pushState(redisContext* conn, u_int32_t key, char* value)
{	
	/* publish state */
	redisReply* reply;

 	reply = redisCommand(conn, "PUBLISH rlccstate_%d %s", key, value);

    if (reply!=NULL) freeReplyObject(reply);

	return;
}

static void
getResultFromReply(redisReply *reply, xqc_rlcc_t* rlcc)
{	
	float cwnd_rate;
	float pacing_rate_rate;
	if(reply->type == REDIS_REPLY_ARRAY) {

		// printf("before cwnd is %d, pacing_rate is %d\n", rlcc->cwnd, rlcc->pacing_rate);

		// cwnd_rate : (0, +INF), pacing_rate_rate : (0, +INF); if value is 0, means that set it auto
		sscanf(reply->element[2]->str, "%f,%f", &cwnd_rate, &pacing_rate_rate);

		if(cwnd_rate!=0){
			rlcc->cwnd *= cwnd_rate;
			if(rlcc->cwnd < XQC_RLCC_MIN_WINDOW){
				rlcc->cwnd = XQC_RLCC_MIN_WINDOW;		// base cwnd
			}
		}

		if(pacing_rate_rate!=0){		// use pacing
			rlcc->pacing_rate *= pacing_rate_rate;
			// TODO: base pacing rate needed
		}
		
		if(cwnd_rate==0){
			xqc_rlcc_cac_cwnd_by_pacing_rate(rlcc);
		}

		if(pacing_rate_rate==0){					// use cwnd update pacing_rate
			xqc_rlcc_cac_pacing_rate_by_cwnd(rlcc);
		}

		// printf("after cwnd is %d, pacing_rate is %d\n", rlcc->cwnd, rlcc->pacing_rate);
	}

	return;
}

static void
subscribe(redisContext* conn, xqc_rlcc_t* rlcc)
{
	rlcc->reply = NULL;
    int redis_err = 0;

	if ((rlcc->reply = redisCommand(conn, "SUBSCRIBE rlccaction_%d", rlcc->rlcc_path_flag)) == NULL) {
        printf("Failed to Subscibe)\n");
        redisFree(conn);
    } else {
        freeReplyObject(rlcc->reply);
	}

	return;
}

void*
getActionT(void* arg)
{	
	int redis_err = 0;
	xqc_rlcc_t* rlcc = (xqc_rlcc_t*)arg;
	void* reply = rlcc->reply;
	while(1){
		pthread_mutex_lock(&mutex_lock);
        pthread_cond_wait(&cond, &mutex_lock);
		if((redis_err = redisGetReply(rlcc->redis_conn_listener, &reply)) == REDIS_OK) {
			getResultFromReply((redisReply *)reply, rlcc);
			freeReplyObject(reply);
		}
		pthread_mutex_unlock(&mutex_lock);
	}

}

static void
xqc_rlcc_init(void *cong_ctl, xqc_send_ctl_t *ctl_ctx, xqc_cc_params_t cc_params)
{	
	xqc_rlcc_t *rlcc = (xqc_rlcc_t *)(cong_ctl);
	memset(rlcc, 0, sizeof(*rlcc));

	rlcc->cwnd = XQC_RLCC_INIT_WIN;
    rlcc->timestamp = xqc_monotonic_timestamp();
	rlcc->rtt = XQC_RLCC_INF;
	rlcc->srtt = XQC_RLCC_INF;
	rlcc->rlcc_lost = 0;
	rlcc->delivery_rate = 0;
	rlcc->soft_delivery_rate = 0;
	rlcc->prior_cwnd = rlcc->cwnd;
	rlcc->min_rtt = rlcc->rtt;
	rlcc->is_slow_start = XQC_FALSE;
	rlcc->in_recovery = XQC_FALSE;

	xqc_rlcc_cac_pacing_rate_by_cwnd(rlcc);
	rlcc->prior_pacing_rate = rlcc->pacing_rate;

	if (cc_params.customize_on) {
        rlcc->rlcc_path_flag = cc_params.rlcc_path_flag;	// 客户端指定flag标识流
		rlcc->redis_host = cc_params.redis_host;
		rlcc->redis_port = cc_params.redis_port;
    }
		
	getRedisConn(rlcc);

	if(rlcc->rlcc_path_flag){
		pushState(rlcc->redis_conn_publisher, rlcc->rlcc_path_flag, "state:init");
		subscribe(rlcc->redis_conn_listener, rlcc);
	}else{
		redisReply* error = redisCommand(rlcc->redis_conn_publisher, "SET error rlcc_path_flag is null");
		freeReplyObject(error);
	}

	// another thread to get action from redis by cond signal
	pthread_t tid;
    pthread_create(&tid, NULL, getActionT, (void*)rlcc);
    pthread_detach(tid);

	return;
}

static void
xqc_rlcc_on_ack(void *cong_ctl, xqc_sample_t *sampler)
{	
	xqc_rlcc_t *rlcc = (xqc_rlcc_t *)(cong_ctl);

	/*	sampler
	 *  prior_delivered : uint64_t : 当前确认的数据包发送前的交付数, 存疑
	 *	interval : xqc_usec_t : 两次采样的间隔时间，稍大于约1rtt的时间
	 *  delivered : uint32_t : 采样区间内的交付数
	 *  acked : uint32_t : 最新一次被ack的数据的大小
	 *  bytes_inflight : uint32_t : 发送未收到确认的数据量
	 *  prior_inflight : uint32_t : 处理此ack前的inflight
	 *  rtt : xqc_usec_t : 采样区间测得的rtt
	 *  is_app_limited : uint32_t : 
	 *  loss : uint32_t : 目测是周期内的丢包数
	 *  total_acked : uint64_t : 总acked数
	 *  srtt : xqc_usec_t
	 *  delivery_rate : uint32_t : (uint64_t)(1e6 * sampler->delivered / sampler->interval);
	 *  prior_lost : uint32_t : bbr2用于判断丢包是否过快, 此包发送前的丢包数  但是这个丢包数很诡异，应该是减去了已经重传的包
	 *  tx_in_flight ： 此包发送时的inflight（包括此包）
	 *  lost_pkts : uint32 : bbr2用于判断丢包是否过快，目前为止的丢包总数-此包发送前的丢包数
	 */

	 /*
	  *起步的rtt较大，前期rtt基本很小，但是导致srtt计算结果前200ms一直很大迟迟不能降下来，srtt变化慢 但是能反应相对稳定的反应变化趋势（必须结合rtt的变化才准确）
	  */

	printf("debug:pd:%ld, i:%ld, d:%d, a:%d, bi:%d, pi:%d, r:%ld, ial:%d, l:%d, ta:%ld, s:%ld, dr:%d, pl:%d, lp:%d\n",
		sampler->prior_delivered,
		sampler->interval,
		sampler->delivered,
		sampler->acked,
		sampler->bytes_inflight,
		sampler->prior_inflight,
		sampler->rtt,
		sampler->is_app_limited,
		sampler->loss,
		sampler->total_acked,
		sampler->srtt,
		sampler->delivery_rate,
		sampler->prior_lost,
		sampler->lost_pkts
	);

	xqc_usec_t current_time = xqc_monotonic_timestamp();

	// update min rtt
	if (rlcc->min_rtt == 0 || sampler->rtt < rlcc->min_rtt) {
        rlcc->min_rtt = sampler->rtt;
		rlcc->min_rtt_timestamp = sampler->now;  // min_rtt_timestamp use sampler now
    }
	
	// probeRTT : simplely force update min_rtt every 2s
	if (rlcc->min_rtt_timestamp + PROBE_INTERVAL < current_time){
		rlcc->min_rtt = rlcc->rtt;
		rlcc->min_rtt_timestamp = current_time;
	}

	if(rlcc->soft_delivery_rate == 0){
		rlcc->soft_delivery_rate=sampler->delivery_rate;
	}else{
		rlcc->soft_delivery_rate -= rlcc->soft_delivery_rate >> 3;
		rlcc->soft_delivery_rate += sampler->delivery_rate >> 3;
	}

	int plan = 1;

	if (plan == 1)
	{
	// plan1. 100ms固定间隔单步状态(取最近一次ack的sampler结果)
	if(rlcc->timestamp + SAMPLE_INTERVAL <= current_time){	// 100000 100ms交互一次
		
		rlcc->timestamp = current_time; // 更新时间戳

		if(rlcc->rlcc_path_flag){
			char value[500] = {0};
			// sprintf(value, "cwnd:%ld;pacing_rate:%ld;rtt:%ld;min_rtt:%ld;srtt:%ld;inflight:%ld;rlcc_lost:%d;lost:%d;is_app_limited:%d;delivery_rate:%d;soft_delivery_rate:%d",
			// rlcc->cwnd,
			// rlcc->pacing_rate,
			// sampler->rtt,
			// rlcc->min_rtt,
			// sampler->srtt, 
			// sampler->bytes_inflight, 
			// rlcc->rlcc_lost,
			// sampler->lost_pkts,
			// sampler->is_app_limited,
			// sampler->delivery_rate,
			// rlcc->soft_delivery_rate);
			sprintf(value, "%d;%d;%ld;%ld;%ld;%d;%d;%d;%d;%d;%d",
				(rlcc->cwnd>>10),
				(rlcc->pacing_rate>>10),
				sampler->rtt,
				rlcc->min_rtt,
				sampler->srtt,
				sampler->bytes_inflight, 
				rlcc->rlcc_lost,
				sampler->lost_pkts,
				sampler->is_app_limited,
				sampler->delivery_rate,
				rlcc->soft_delivery_rate); // delivery_rate 不作为状态，作为单独的奖励计算使用
			pushState(rlcc->redis_conn_publisher, rlcc->rlcc_path_flag , value);
			pthread_mutex_lock(&mutex_lock);
			// send signal
			pthread_cond_signal(&cond);
			pthread_mutex_unlock(&mutex_lock);

		}else{
			redisReply* error = redisCommand(rlcc->redis_conn_publisher, "SET error rlcc_path_flag is null");
			freeReplyObject(error);
		}

	}
	}

	if (plan == 2)
	{
	// plan2. 双rtt 半rtt状态
	// xqc_usec_t time_interval;

	if (rlcc->sample_start > current_time){
		// 开始采样前持续更新到最新的sampler
		rlcc->rtt = sampler->rtt;
		rlcc->srtt = sampler->srtt;
		rlcc->inflight = sampler->bytes_inflight;
		rlcc->lost = sampler->lost_pkts;
		rlcc->delivery_rate = sampler->delivery_rate;
	}

	if (rlcc->sample_start <= current_time){
		// sample
		rlcc->rtt -= rlcc->rtt >> 2;
		rlcc->rtt += sampler->rtt >> 2;

		rlcc->srtt -= rlcc->srtt >> 2;
		rlcc->srtt += sampler->srtt >> 2;

		rlcc->inflight -= rlcc->inflight >> 2;
		rlcc->inflight += sampler->bytes_inflight >> 2;

		printf("rlcc lost %d, >>2 %d",rlcc->lost, rlcc->lost >> 2);
		rlcc->lost -= rlcc->lost >> 2;				// 验证下 这里 0 右移会不会溢出
		rlcc->lost += sampler->lost_pkts >> 2;
		printf("rlcc_lost after %d", rlcc->lost);

		rlcc->delivery_rate -= rlcc->delivery_rate >> 2;
		rlcc->delivery_rate += sampler->delivery_rate >> 2;

	}

	if (rlcc->sample_stop<current_time){
		// stop sample and send signal

		rlcc->timestamp = current_time;
		rlcc->sample_start = current_time + sampler->srtt;
		rlcc->sample_stop = rlcc->sample_start + xqc_min(rlcc->min_rtt, SAMPLE_INTERVAL);
	
		if(rlcc->rlcc_path_flag){
			char value[500] = {0};
			// sprintf(value, "cwnd:%ld;pacing_rate:%ld;rtt:%ld;min_rtt:%ld;srtt:%ld;inflight:%ld;rlcc_lost:%d;lost:%d;is_app_limited:%d;delivery_rate:%d;soft_delivery_rate:%d",
			// rlcc->cwnd,
			// rlcc->pacing_rate,
			// sampler->rtt,
			// rlcc->min_rtt,
			// sampler->srtt, 
			// sampler->bytes_inflight, 
			// rlcc->rlcc_lost,
			// sampler->lost_pkts,
			// sampler->is_app_limited,
			// sampler->delivery_rate
			// rlcc->soft_delivery_rate);
			sprintf(value, "%d;%d;%ld;%ld;%ld;%d;%d;%d;%d;%d;%d",
				(rlcc->cwnd>>10),
				(rlcc->pacing_rate>>10),
				rlcc->rtt,
				rlcc->min_rtt,
				rlcc->srtt,
				rlcc->inflight, 
				rlcc->rlcc_lost,
				rlcc->lost,
				sampler->is_app_limited,
				rlcc->delivery_rate,
				rlcc->soft_delivery_rate); // delivery_rate 不作为状态，作为单独的奖励计算使用
			pushState(rlcc->redis_conn_publisher, rlcc->rlcc_path_flag , value);
			pthread_mutex_lock(&mutex_lock);
			// send signal
			pthread_cond_signal(&cond);
			pthread_mutex_unlock(&mutex_lock);

		}else{
			redisReply* error = redisCommand(rlcc->redis_conn_publisher, "SET error rlcc_path_flag is null");
			freeReplyObject(error);
		}
	}
	}
	return;
}

static void 
xqc_rlcc_on_lost(void *cong_ctl, xqc_usec_t lost_sent_time)
{	
	xqc_rlcc_t *rlcc = (xqc_rlcc_t *)(cong_ctl);
	xqc_usec_t current_time = xqc_monotonic_timestamp();
	
	int plan = 1;

	if (plan == 1)
	{
	// plan1. 100ms固定间隔单步状态
	if(rlcc->timestamp + SAMPLE_INTERVAL <= current_time){	
		rlcc->rlcc_lost++;
	}else{
		rlcc->rlcc_lost = 0;
	}
	}

	if (plan == 2)
	{
	// plan2. 双rtt 统计最近间隔内的丢包情况状态
	if (rlcc->sample_start <= current_time){
		rlcc->rlcc_lost++;
	}

	if (rlcc->sample_stop < current_time){
		rlcc->rlcc_lost = 0;
	}
	}

	return;
}

static uint64_t 
xqc_rlcc_get_cwnd(void *cong_ctl)
{	
	xqc_rlcc_t *rlcc = (xqc_rlcc_t *)(cong_ctl);
	return rlcc->cwnd;
}

static void 
xqc_rlcc_reset_cwnd(void *cong_ctl)
{	
	xqc_rlcc_t *rlcc = (xqc_rlcc_t *)(cong_ctl);
    rlcc->cwnd = XQC_RLCC_MIN_WINDOW;
	return;
}

size_t
xqc_rlcc_size()
{
    return sizeof(xqc_rlcc_t);
}

static uint32_t 
xqc_rlcc_get_pacing_rate(void *cong_ctl)
{	
	xqc_rlcc_t *rlcc = (xqc_rlcc_t *)(cong_ctl);
    return rlcc->pacing_rate;
}

static uint32_t 
xqc_rlcc_get_bandwidth(void *cong_ctl)
{	
	xqc_rlcc_t *rlcc = (xqc_rlcc_t *)(cong_ctl);
	return rlcc->delivery_rate;
}

static void 
xqc_rlcc_restart_from_idle(void *cong_ctl, uint64_t conn_delivered)
{	
	return;
}

static int
xqc_rlcc_in_recovery(void *cong) {
	xqc_rlcc_t *rlcc = (xqc_rlcc_t *)(cong);
    return rlcc->in_recovery;	// never in in recovery, all control by cc RL
}

int32_t
xqc_rlcc_in_slow_start(void *cong_ctl)
{	
    xqc_rlcc_t *rlcc = (xqc_rlcc_t *)(cong_ctl);
    return rlcc->is_slow_start;	// nerver in slow start, all control by cc RL
}

const xqc_cong_ctrl_callback_t xqc_rlcc_cb = {
    .xqc_cong_ctl_size              	= xqc_rlcc_size,
    .xqc_cong_ctl_init              	= xqc_rlcc_init,
    .xqc_cong_ctl_on_lost           	= xqc_rlcc_on_lost,
	.xqc_cong_ctl_on_ack_multiple_pkts 	= xqc_rlcc_on_ack,	// bind with change pacing rate
    // .xqc_cong_ctl_on_ack				= xqc_rlcc_on_ack,	// only change cwnd
    .xqc_cong_ctl_get_cwnd				= xqc_rlcc_get_cwnd,
    .xqc_cong_ctl_reset_cwnd			= xqc_rlcc_reset_cwnd,
    .xqc_cong_ctl_in_slow_start			= xqc_rlcc_in_slow_start,
    .xqc_cong_ctl_restart_from_idle		= xqc_rlcc_restart_from_idle,
    .xqc_cong_ctl_in_recovery      		= xqc_rlcc_in_recovery,
	.xqc_cong_ctl_get_pacing_rate		= xqc_rlcc_get_pacing_rate,
};