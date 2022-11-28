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
  
#include <hiredis/hiredis.h>  

#define XQC_RLCC_MSS               	1460
#define MONITOR_INTERVAL           	100
#define XQC_RLCC_INIT_WIN          	(32 * XQC_RLCC_MSS)
#define XQC_RLCC_MIN_WINDOW        	(4 * XQC_RLCC_MSS )
#define cwnd_gain					2
#define XQC_RLCC_INF             	0x7fffffff

const float xqc_rlcc_init_pacing_gain = 2.885;

static void 
xqc_rlcc_init_pacing_rate(xqc_rlcc_t *rlcc, xqc_sample_t *sampler)
{
    uint64_t bandwidth;
    bandwidth = rlcc->cwnd * (uint64_t)MSEC2SEC
        / (sampler->srtt ? sampler->srtt : 1000);
    rlcc->pacing_rate = bandwidth;
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
}

static void
pushState(redisContext* conn, u_int32_t key, char* value)
{	
	/* publish state */
	redisReply* reply;

 	reply = redisCommand(conn, "PUBLISH rlccstate_%d %s", key, value);

    if (reply!=NULL) freeReplyObject(reply);
}

static void
getResultFromReply(redisReply *reply, xqc_rlcc_t* rlcc)
{	
	int i;
	// uint32_t add_cwnd;
	float add_cwnd;
	if(reply->type == REDIS_REPLY_ARRAY) {
		// printf("%s\n", reply->element[2]->str);
		sscanf(reply->element[2]->str, "%f,%d", &add_cwnd, &rlcc->pacing_rate);
		// rlcc->cwnd += add_cwnd * XQC_RLCC_MSS;		// 改成加减动作
		rlcc->cwnd *= add_cwnd;						// 倍率乘性动作
		if(rlcc->cwnd < 4*XQC_RLCC_MSS){
			rlcc->cwnd = 4*XQC_RLCC_MSS;		// 保障最基本的吞吐
		}
		// printf("cwnd is %d, pacing_rate is %d\n", rlcc->cwnd, rlcc->pacing_rate);
	}
}

static void
subscribe(redisContext* conn, xqc_rlcc_t* rlcc)
{
	rlcc->reply = NULL;
    int redis_err = 0;

	// char key[10] = {0};
    // sprintf(key, "%d", rlcc->rlcc_path_flag);

	if ((rlcc->reply = redisCommand(conn, "SUBSCRIBE rlccaction_%d", rlcc->rlcc_path_flag)) == NULL) {
        printf("Failed to Subscibe)\n");
        redisFree(conn);
    } else {
        freeReplyObject(rlcc->reply);
	}
}

static void
getAction(redisContext* conn, xqc_rlcc_t* rlcc, void *reply, u_int32_t key)
{	
	int redis_err = 0;
	
	if((redis_err = redisGetReply(conn, &reply)) == REDIS_OK) {
		getResultFromReply((redisReply *)reply, rlcc);
		// rlcc->cwnd *= XQC_RLCC_MSS;		// 改成加减动作
		freeReplyObject(reply);
	}

	// /* 两个动作只有一个被设置的时候 */
	// if(rlcc->cwnd==0 && rlcc->pacing_rate!=0){
	// 	/* cacu cwnd by pacing_rate */
	// 	rlcc->cwnd = cwnd_gain * rlcc->throughput * rlcc->min_rtt ;
	// }

	// if(rlcc->pacing_rate==0 && rlcc->cwnd!=0){
	// 	/* cacu pacing_rate by cwnd */
		
	// }
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
	rlcc->last_delivered = 0;
	rlcc->throughput = 0;	/* 带宽的计算要按cubic的来 */
	rlcc->prior_cwnd = rlcc->cwnd;
	rlcc->min_rtt = rlcc->rtt;
	rlcc->recovery_start_time = 0;

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

}

static void
xqc_rlcc_on_ack(void *cong_ctl, xqc_packet_out_t *po, xqc_usec_t now)
{	
	xqc_rlcc_t *rlcc = (xqc_rlcc_t *)(cong_ctl);

	/*
	对于统计有用的字段
	po -> po_largest_ack  	Largest Acknowledged in ACK frame
	po -> po_sent_time		发送时间
	po -> po_delivered		发这个包之前已经交付的数据总数
	po_delivered_time		发这个包之前的上一个ack时间
	po_first_sent_time		the time of first sent packet during current sample period
	po_is_app_limited		
	po_tx_in_flight			飞行数据包
	po_lost					丢包数： 累加值
	*/

	// ack time - send time
	rlcc->rtt = now - po->po_sent_time;
	// smooth rtt
	rlcc->srtt = 7 * rlcc->srtt / 8 + rlcc->rtt / 8;

	xqc_usec_t current_time = xqc_monotonic_timestamp();

	// update min rtt
	if (rlcc->min_rtt == 0 || rlcc->rtt < rlcc->min_rtt) {
        rlcc->min_rtt = rlcc->rtt;
		rlcc->min_rtt_timestamp = current_time;
    }	
	
	// probeRTT : probe机制并不好，后续考虑其他方法，这里就简单强制更新以下min_rtt
	if (rlcc->min_rtt_timestamp + 2000000 < current_time){
		rlcc->min_rtt = rlcc->rtt;
		rlcc->min_rtt_timestamp = current_time;
	}  // 2s不变则强制更新min_rtt

	if(rlcc->timestamp + 100000 <= current_time){	// 100000 100ms交互一次
		rlcc->timestamp = current_time;
		// rlcc->lost;   on_lost中统计最近100ms内lost的触发次数

		// 100ms内的带宽
		rlcc->throughput = (po -> po_delivered - rlcc->last_delivered)*10; //100ms = /0.1
		rlcc->last_delivered = po -> po_delivered;
		rlcc->prior_cwnd = rlcc->cwnd;
		rlcc->inflight = po->po_tx_in_flight;
		
		// 100ms内的丢包数
		rlcc->recent_lost = po->po_lost - rlcc->last_po_lost;
		rlcc->last_po_lost = po->po_lost;

		if(rlcc->rlcc_path_flag){
			char value[500] = {0};
			// sprintf(value, "throughput:%d;rtt:%ld;srtt:%ld;inflight:%ld;rlcclost:%d;recent_lost:%d;is_app_limited:%d",
			// 	rlcc->throughput,
			// 	rlcc->rtt, 
			// 	rlcc->srtt, 
			// 	rlcc->inflight, 
			// 	rlcc->rlcc_lost,
			// 	rlcc->recent_lost,
			// 	po->po_is_app_limited);
			sprintf(value, "%d;%ld;%ld;%ld;%d;%d;%d",
				rlcc->throughput,
				rlcc->rtt, 
				rlcc->srtt, 
				rlcc->inflight, 
				rlcc->rlcc_lost,
				rlcc->recent_lost,
				po->po_is_app_limited);
			pushState(rlcc->redis_conn_publisher, rlcc->rlcc_path_flag , value);
			getAction(rlcc->redis_conn_listener, rlcc, rlcc->reply, rlcc->rlcc_path_flag); //sub to get the first pub
			rlcc->cwnd += XQC_RLCC_MSS; // test if ok
		}else{
			redisReply* error = redisCommand(rlcc->redis_conn_publisher, "SET error rlcc_path_flag is null");
			freeReplyObject(error);
		}
	}
	
	if (po->po_sent_time > rlcc->recovery_start_time){  // quit recovery
		rlcc->in_recovery = 0;
	}

}

static void 
xqc_rlcc_on_lost(void *cong_ctl, xqc_usec_t lost_sent_time)
{	
	xqc_rlcc_t *rlcc = (xqc_rlcc_t *)(cong_ctl);
	xqc_usec_t current_time = xqc_monotonic_timestamp();
	
	if(rlcc->timestamp + 100000 <= current_time){	
		rlcc->rlcc_lost++;
	}else{
		rlcc->rlcc_lost = 0;
	}

	/* No reaction if already in a recovery period. */
    if (rlcc->in_recovery) {
        return;
    }

	rlcc->recovery_start_time = xqc_monotonic_timestamp();
	rlcc->in_recovery = 1;
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
	rlcc->recovery_start_time = 0;
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
    return rlcc->throughput;
}

static void 
xqc_rlcc_restart_from_idle(void *cong_ctl, uint64_t conn_delivered)
{	
	return;
}

static int
xqc_rlcc_in_recovery(void *cong) {
	xqc_rlcc_t *rlcc = (xqc_rlcc_t *)(cong);
    return rlcc->in_recovery;	// 这块可能有影响，后续需要观察
}

int32_t
xqc_rlcc_in_slow_start(void *cong_ctl)
{
    xqc_rlcc_t *rlcc = (xqc_rlcc_t *)(cong_ctl);
    return 0;	// nerver in slow start
}

const xqc_cong_ctrl_callback_t xqc_rlcc_cb = {
    .xqc_cong_ctl_size              = xqc_rlcc_size,
    .xqc_cong_ctl_init              = xqc_rlcc_init,
    .xqc_cong_ctl_on_lost           = xqc_rlcc_on_lost,
    .xqc_cong_ctl_on_ack            = xqc_rlcc_on_ack,
    .xqc_cong_ctl_get_cwnd          = xqc_rlcc_get_cwnd,
    .xqc_cong_ctl_reset_cwnd        = xqc_rlcc_reset_cwnd,
    .xqc_cong_ctl_in_slow_start     = xqc_rlcc_in_slow_start,
    .xqc_cong_ctl_restart_from_idle = xqc_rlcc_restart_from_idle,
    .xqc_cong_ctl_in_recovery       = xqc_rlcc_in_recovery,
};