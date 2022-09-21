/**
 * @copyright Copyright (c) 2022, jinyaoliu 
 */
#include "src/congestion_control/rlcc.h"
#include <math.h>
#include "src/common/xqc_time.h"
#include <xquic/xquic.h>
#include <xquic/xquic_typedef.h>
#include "src/congestion_control/xqc_sample.h"
  
#include <hiredis/hiredis.h>  

#define XQC_RLCC_MSS               1460
#define MONITOR_INTERVAL           100
#define XQC_RLCC_INIT_WIN          (32 * XQC_RLCC_MSS)


const float xqc_rlcc_init_pacing_gain = 2.885;


static redisContext*
getRedisConn()
{
	redisContext* conn = redisConnect("0.0.0.0", 6379);  
    if(conn->err)   printf("connection error:%s\n", conn->errstr);
	return conn;
}

// 现在的问题 找不到 cong_ctl的结构体在哪里，应该是流的结构体
// 用四元组的源地址端口？ 用CID标识？  多路径用path_id标识？
// CID训练的时候 python如何实时获得该流的CID信息

static void
pushState(redisContext* conn, string key, string value)
{	
	/* 先操作再上锁 */
	redisReply* reply = redisCommand(conn, "HSET rlcc_9000 state %s", value);
	reply = redisCommand(conn, "HSET rlcc_9000 lock 0");  
    freeReplyObject(reply);
}

static void
getAction(redisContext* conn, xqc_rlcc_t* rlcc)
{	
	redisReply* lock;

    while(1){
        // 写入动作后，lock变为1，此时才可以读取动作
        lock = redisCommand(conn, "HGET rlcc_9000 lock"); 
        int lockvalue = atoi(lock->str);
        if(lockvalue==1){
            redisReply* action = redisCommand(conn, "HGET rlcc_9000 cwnd");
			rlcc->cwnd = atoi(action->str);
			redisReply* action = redisCommand(conn, "HGET rlcc_9000 pacing_rate");
			rlcc->pacing_rate = atoi(action->str);
            printf("current action is %d, pacing_rate is %d", rlcc->cwnd, rlcc->pacing_rate);
            freeReplyObject(action);
            break;
        }
    }
    
    freeReplyObject(lock); 

	/* 此处根据值进行进一步计算 */
	if(rlcc->cwnd==0){
		/* 根据pacing_rate计算窗口大小 */
		return;
	}

	if(rlcc->pacing_rate==0){
		/* 根据cwnd计算pacing_rate大小 */
		return;
	}
}

static void
xqc_rlcc_init(void *cong_ctl, xqc_sample_t *sampler, xqc_cc_params_t cc_params){
    xqc_rlcc_t *rlcc = (xqc_rlcc_t *)(cong_ctl);
	memset(rlcc, 0, sizeof(*rlcc));

	xqc_send_ctl_t *send_ctl = sampler->send_ctl;
	xqc_connection_t *ctl_conn = send_ctl->ctl_conn;
	
	/* 初始化rlcc参数 */
	rlcc->cwnd = XQC_CUBIC_INIT_WIN;
	/* rfc 9000 选择dcid标识流较为合适 */
	rlcc->original_dcid = ctl_conn->original_dcid;
    rlcc->time_stamp = xqc_monotonic_timestamp();
	rlcc->pacing_rate = xqc_rlcc_init_pacing_gain * bandwidth;
	rlcc->redis_conn = getRedisConn();
	rlcc->rtt = send_ctl->ctl_latest_rtt; /* 不知道初始化的时候是多少 */
	rlcc->srtt = send_ctl->ctl_srtt;
	rlcc->lost = 0;
	rlcc->bandwidth = 0;	/* 带宽的计算要按cubic的来 */
	rlcc->last_bandwidth = 0;
	rlcc->prior_cwnd = rlcc->cwnd;
	rlcc->min_rtt = rlcc->rtt;

	pushState(rlcc->redis_conn, "tom", "jerry");
}

static void
xqc_rlcc_on_ack(void *cong_ctl, xqc_sample_t *sampler)
{
	xqc_rlcc_t *rlcc = (xqc_rlcc_t *)(cong_ctl);

	// pacing_rate, cwnd 由动作去改变

	/* 
	rlcc运行间隔采用xqc默认的采样间隔 sampler->interval
	interval 默认值是 XQC_DEFAULT_RECORD_INTERVAL  100000  100ms
	运行中 sampler->interval = xqc_max(sampler->ack_elapse, sampler->send_elapse);

	后续需要验证这个时间间隔变化 是否会影响马尔可夫性
	*/
	xqc_usec_t now = xqc_monotonic_timestamp();
	if(rlcc->time_stamp + ctl_info->record_interval <= now){
		rlcc->time_stamp = now;
		// 是每个运行的时间间隔更新一次统计值，执行一次动作，而不是每个ack
		rlcc->time_stamp = sampler->now;
		rlcc->rtt = sampler->rtt;
		rlcc->srtt = sampler->srtt;
		rlcc->lost = sampler->lost_pkts; /* bbrv2的间隔时间丢包数量 */
		rlcc->last_bandwidth = rlcc->bandwidth;
		rlcc->bandwidth = 1.0 * sampler->delivered / sampler->interval * MSEC2SEC;
		rlcc->prior_cwnd = rlcc->cwnd;
		// bbr的测最小rtt是建立在其他丢包算法减半退让的基础来实现的
		// min_rtt的测量是个问题？该怎么测量准确的min_rtt?
		rlcc->min_rtt = sampler->send_ctl->ctl_minrtt; //不确定这个min rtt在rtt增长环境中能否跟踪rtt变化

		pushState(rlcc->redis_conn, "tom", "jerry");

		/* 外部计算+写入redis需要一段时间 ； 获取动作按照redis里面的锁来*/
		/* 此处阻塞来获取动作 */
		getAction(rlcc->redis_conn, rlcc);

	}
	
	
}

static void 
xqc_rlcc_on_lost(void *cong_ctl, xqc_usec_t lost_sent_time)
{
    /* xqc_bbr 超过目标窗口后如果发生丢包，算法就不会增加窗口了 */
	/* rlcc这里因为要算法全托管给rl，所以这里不做操作 */
	return;
}

size_t
xqc_rlcc_size()
{
    return sizeof(xqc_rlcc_t);
}




const xqc_cong_ctrl_callback_t xqc_rlcc_cb = {
	.xqc_cong_ctl_size              = xqc_rlcc_size,
    // .xqc_cong_ctl_init           = xqc_rlcc_init,
	.xqc_cong_ctl_init_bbr          = xqc_rlcc_init,
    .xqc_cong_ctl_on_lost           = xqc_rlcc_on_lost,
    // .xqc_cong_ctl_on_ack         = xqc_rlcc_on_ack, //rlcc的入口
	.xqc_cong_ctl_bbr               = xqc_rlcc_on_ack,
	/* Callback when sending a packet, to determine if the packet can be sent */
	.xqc_cong_ctl_get_cwnd          = xqc_rlcc_get_cwnd,
	.xqc_cong_ctl_get_pacing_rate         = xqc_rlcc_get_pacing_rate,
	/* Callback when all packets are detected as lost within 1-RTT, reset the congestion window */
    .xqc_cong_ctl_reset_cwnd        = xqc_rlcc_reset_cwnd,
	/* If the connection is in slow start state; if rlcc use slow start rewrite this */
    // .xqc_cong_ctl_in_slow_start     = xqc_rlcc_in_slow_start,
	/* If the connection is in recovery state. */
    // .xqc_cong_ctl_in_recovery       = xqc_rlcc_in_recovery,
	.xqc_cong_ctl_get_bandwidth_estimate  = xqc_rlcc_get_bandwidth,
};