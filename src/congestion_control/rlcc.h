/**
 * @copyright Copyright (c) 2022, jinyaoliu 
 */

#ifndef _RLCC_CC_H_INCLUDED_
#define _RLCC_CC_H_INCLUDED_

#include <xquic/xquic_typedef.h>
#include <xquic/xquic.h>
#include "src/transport/xqc_send_ctl.h"
#include "src/transport/xqc_packet_out.h"
#include <hiredis/hiredis.h>  

#define MSEC2SEC 1000000

typedef struct xqc_rlcc_s {
    uint32_t                cwnd;
    /* Current pacing rate */
    uint32_t                pacing_rate;
    redisContext*           redis_conn_listener;
    redisContext*           redis_conn_publisher;
    void*                   reply; // subscribe

    /* sample state */
    xqc_usec_t              timestamp;
    xqc_usec_t              rtt;
    uint64_t                lost;
    xqc_usec_t              srtt;
    uint32_t                bandwidth;
    uint32_t                last_delivered;
    uint64_t                inflight;
    uint32_t                prior_cwnd;
    xqc_usec_t              min_rtt;
    xqc_usec_t              min_rtt_timestamp;

    uint32_t                in_recovery;

    uint32_t                rlcc_path_flag;
    xqc_usec_t              recovery_start_time;
} xqc_rlcc_t;

extern const xqc_cong_ctrl_callback_t xqc_rlcc_cb;

#endif /* _XQC_RLCC_H_INCLUDED_ */