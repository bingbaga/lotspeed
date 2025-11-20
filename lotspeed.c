// lotspeed.c  ——  2025 年的"锐速"复活版 v2.0
// Author: uk0 @ 2025-11-19 17:06:58
// 致敬经典 LotServer/ServerSpeeder，为新时代而生

#include <linux/module.h>
#include <linux/version.h>
#include <net/tcp.h>
#include <linux/math64.h>
#include <linux/moduleparam.h>
#include <linux/jiffies.h>
#include <linux/ktime.h>
#include <linux/kernel.h>
#include <linux/timer.h>

// 版本兼容性检测 - 修正版本判断逻辑
// 根据实际测试：6.8.0 使用旧API，6.17+ 使用新API
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6,17,0)
#define KERNEL_6_17_PLUS 1
    #define NEW_CONG_CONTROL_API 1
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(6,9,0) && LINUX_VERSION_CODE < KERNEL_VERSION(6,17,0)
// 6.9 - 6.16 使用新API (需要进一步测试确认)
    #define NEW_CONG_CONTROL_API 1
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(5,19,0) && LINUX_VERSION_CODE < KERNEL_VERSION(6,8,0)
// 5.19 - 6.7.x 使用新API
    #define NEW_CONG_CONTROL_API 1
#else
// 6.8.0 - 6.8.x 以及更早版本使用旧API
#define OLD_CONG_CONTROL_API 1
#endif

// 滤波与探测常量
#define LOTSPEED_BW_EMA_SHIFT        3     // 1/8 EMA
#define LOTSPEED_BW_DECAY_SHIFT      3     // 窗口最大值衰减 1/8
#define LOTSPEED_BW_WINDOW_MS        200   // 200ms 窗口更新
#define LOTSPEED_PROBE_BASE          10000 // BDP 自适应探测基数
#define LOTSPEED_PROBE_MIN           8
#define LOTSPEED_PROBE_MAX           80
#define LOTSPEED_MIN_GAIN            10
#define LOTSPEED_TURBO_IGNORE_SPAN   3

// 可调参数（通过 sysfs 动态修改）
static unsigned long lotserver_rate = 125000000ULL;   // 默认 1Gbps
static unsigned int lotserver_gain = 15;              // 1.5x 默认增益
static unsigned int lotserver_min_cwnd = 50;          // 最小拥塞窗口
static unsigned int lotserver_max_cwnd = 10000;       // 最大拥塞窗口
static bool lotserver_adaptive = true;                // 自适应模式
static bool lotserver_turbo = false;                  // 涡轮模式
static bool lotserver_soft_turbo = true;              // 软涡轮（丢包预算）
static unsigned int lotserver_soft_turbo_budget = 2;  // 可忽略的连续丢包数
static bool lotserver_verbose = false;                // 详细日志模式
static bool force_unload = false;

static inline u8 lotspeed_get_turbo_budget(void)
{
    return lotserver_soft_turbo ?
           (u8)clamp_t(unsigned int, lotserver_soft_turbo_budget, 1U, 8U) : 0;
}

static inline void lotspeed_reset_turbo_budget(struct lotspeed *ca)
{
    if (ca) {
        ca->turbo_budget = lotspeed_get_turbo_budget();
        ca->turbo_ignore_ref = 0;
    }
}

static inline void lotspeed_consume_turbo_ignore(struct lotspeed *ca)
{
    if (ca && ca->turbo_ignore_ref > 0)
        ca->turbo_ignore_ref--;
}

static inline bool lotspeed_turbo_ignore_active(const struct lotspeed *ca)
{
    return ca && ca->turbo_ignore_ref > 0;
}

static bool lotspeed_turbo_should_ignore(struct lotspeed *ca, const char *reason)
{
    if (!ca)
        return lotserver_turbo;

    ca->turbo_ignore_ref = 0;

    if (!lotserver_turbo)
        return false;

    if (!lotserver_soft_turbo) {
        ca->turbo_ignore_ref = LOTSPEED_TURBO_IGNORE_SPAN;
        return true;
    }

    if (!ca->turbo_budget)
        return false;

    ca->turbo_budget--;
    ca->turbo_ignore_ref = LOTSPEED_TURBO_IGNORE_SPAN;
    if (lotserver_verbose) {
        pr_info("lotspeed: soft turbo ignoring %s (budget=%u)\n",
                reason, ca->turbo_budget);
    }

    return true;
}

// 参数变更回调 - 速率
static int param_set_rate(const char *val, const struct kernel_param *kp)
{
    unsigned long old_val = lotserver_rate;
    int ret = param_set_ulong(val, kp);

    if (ret == 0 && old_val != lotserver_rate && lotserver_verbose) {
        unsigned long gbps_int = lotserver_rate / 125000000;
        unsigned long gbps_frac = (lotserver_rate % 125000000) * 100 / 125000000;
        pr_info("lotspeed: [uk0@2025-11-19 17:06:58] rate changed: %lu -> %lu (%lu.%02lu Gbps)\n",
                old_val, lotserver_rate, gbps_int, gbps_frac);
    }
    return ret;
}

// 参数变更回调 - 增益
static int param_set_gain(const char *val, const struct kernel_param *kp)
{
    unsigned int old_val = lotserver_gain;
    int ret = param_set_uint(val, kp);

    if (ret == 0 && old_val != lotserver_gain && lotserver_verbose) {
        unsigned int gain_int = lotserver_gain / 10;
        unsigned int gain_frac = lotserver_gain % 10;
        pr_info("lotspeed: [uk0@2025-11-19 17:06:58] gain changed: %u -> %u (%u.%ux)\n",
                old_val, lotserver_gain, gain_int, gain_frac);
    }
    return ret;
}

// 参数变更回调 - 最小窗口
static int param_set_min_cwnd(const char *val, const struct kernel_param *kp)
{
    unsigned int old_val = lotserver_min_cwnd;
    int ret = param_set_uint(val, kp);

    if (ret == 0 && old_val != lotserver_min_cwnd && lotserver_verbose) {
        pr_info("lotspeed: [uk0@2025-11-19 17:06:58] min_cwnd changed: %u -> %u\n",
                old_val, lotserver_min_cwnd);
    }
    return ret;
}

// 参数变更回调 - 最大窗口
static int param_set_max_cwnd(const char *val, const struct kernel_param *kp)
{
    unsigned int old_val = lotserver_max_cwnd;
    int ret = param_set_uint(val, kp);

    if (ret == 0 && old_val != lotserver_max_cwnd && lotserver_verbose) {
        pr_info("lotspeed: [uk0@2025-11-19 17:06:58] max_cwnd changed: %u -> %u\n",
                old_val, lotserver_max_cwnd);
    }
    return ret;
}

// 参数变更回调 - 自适应模式
static int param_set_adaptive(const char *val, const struct kernel_param *kp)
{
    bool old_val = lotserver_adaptive;
    int ret = param_set_bool(val, kp);

    if (ret == 0 && old_val != lotserver_adaptive && lotserver_verbose) {
        pr_info("lotspeed: [uk0@2025-11-19 17:06:58] adaptive mode: %s -> %s\n",
                old_val ? "ON" : "OFF", lotserver_adaptive ? "ON" : "OFF");
    }
    return ret;
}

// 参数变更回调 - 涡轮模式
static int param_set_turbo(const char *val, const struct kernel_param *kp)
{
    bool old_val = lotserver_turbo;
    int ret = param_set_bool(val, kp);

    if (ret == 0 && old_val != lotserver_turbo && lotserver_verbose) {
        if (lotserver_turbo) {
            pr_info("lotspeed: [uk0@2025-11-19 17:06:58] ⚡⚡⚡ TURBO MODE ACTIVATED ⚡⚡⚡\n");
            pr_info("lotspeed: WARNING: Ignoring ALL congestion signals!\n");
        } else {
            pr_info("lotspeed: [uk0@2025-11-19 17:06:58] Turbo mode DEACTIVATED\n");
        }
    }
    return ret;
}

// 自定义参数操作
static const struct kernel_param_ops param_ops_rate = {
        .set = param_set_rate,
        .get = param_get_ulong,
};

static const struct kernel_param_ops param_ops_gain = {
        .set = param_set_gain,
        .get = param_get_uint,
};

static const struct kernel_param_ops param_ops_min_cwnd = {
        .set = param_set_min_cwnd,
        .get = param_get_uint,
};

static const struct kernel_param_ops param_ops_max_cwnd = {
        .set = param_set_max_cwnd,
        .get = param_get_uint,
};

static const struct kernel_param_ops param_ops_adaptive = {
        .set = param_set_adaptive,
        .get = param_get_bool,
};

static const struct kernel_param_ops param_ops_turbo = {
        .set = param_set_turbo,
        .get = param_get_bool,
};

// 注册参数
module_param(force_unload, bool, 0644);
MODULE_PARM_DESC(force_unload, "Force unload module ignoring references");

module_param_cb(lotserver_rate, &param_ops_rate, &lotserver_rate, 0644);
MODULE_PARM_DESC(lotserver_rate, "Target rate in bytes/sec (default 1Gbps)");

module_param_cb(lotserver_gain, &param_ops_gain, &lotserver_gain, 0644);
MODULE_PARM_DESC(lotserver_gain, "Gain multiplier x10 (30 = 3.0x)");

module_param_cb(lotserver_min_cwnd, &param_ops_min_cwnd, &lotserver_min_cwnd, 0644);
MODULE_PARM_DESC(lotserver_min_cwnd, "Minimum congestion window");

module_param_cb(lotserver_max_cwnd, &param_ops_max_cwnd, &lotserver_max_cwnd, 0644);
MODULE_PARM_DESC(lotserver_max_cwnd, "Maximum congestion window");

module_param_cb(lotserver_adaptive, &param_ops_adaptive, &lotserver_adaptive, 0644);
MODULE_PARM_DESC(lotserver_adaptive, "Enable adaptive rate control");

module_param_cb(lotserver_turbo, &param_ops_turbo, &lotserver_turbo, 0644);
MODULE_PARM_DESC(lotserver_turbo, "Turbo mode - ignore all congestion signals");

module_param(lotserver_verbose, bool, 0644);
MODULE_PARM_DESC(lotserver_verbose, "Enable verbose logging");

module_param(lotserver_soft_turbo, bool, 0644);
MODULE_PARM_DESC(lotserver_soft_turbo, "Soft turbo - allow limited loss ignoring before backing off");

module_param(lotserver_soft_turbo_budget, uint, 0644);
MODULE_PARM_DESC(lotserver_soft_turbo_budget, "Number of consecutive losses Turbo mode may ignore");

// 统计信息
static atomic_t active_connections = ATOMIC_INIT(0);
static atomic64_t total_bytes_sent = ATOMIC64_INIT(0);
static atomic_t total_losses = ATOMIC_INIT(0);
static atomic_t module_ref_count = ATOMIC_INIT(0);

struct lotspeed {
    u64 target_rate;
    u64 actual_rate;
    u64 bw_window_max;
    u64 last_update;
    u64 bytes_sent;     // 添加字节统计
    u64 start_time;     // 连接开始时间
    u32 cwnd_gain;
    u32 loss_count;
    u32 rtt_min;
    u32 rtt_cnt;
    u32 bw_window_stamp;
    u32 rtt_ema;
    u32 rtt_var;
    u32 probe_cnt;
    bool ss_mode;
    u8 turbo_budget;
    u8 turbo_ignore_ref;
    u8 reserved;
};

static struct tcp_congestion_ops lotspeed_ops;

// 初始化连接
static void lotspeed_init(struct sock *sk)
{
    struct tcp_sock *tp = tcp_sk(sk);
    struct lotspeed *ca = inet_csk_ca(sk);
    memset(ca, 0, sizeof(*ca));

    // 初始化状态
    tp->snd_ssthresh = lotserver_turbo ? TCP_INFINITE_SSTHRESH : tp->snd_cwnd * 2;
    ca->target_rate = lotserver_rate;
    ca->actual_rate = 0;
    ca->cwnd_gain = lotserver_gain;
    ca->loss_count = 0;
    ca->rtt_min = 0;
    ca->rtt_cnt = 0;
    ca->last_update = tcp_jiffies32;
    ca->ss_mode = true;
    ca->probe_cnt = 0;
    ca->bytes_sent = 0;
    ca->start_time = ktime_get_real_seconds();
    ca->bw_window_stamp = tcp_jiffies32;
    lotspeed_reset_turbo_budget(ca);

    // 强制开启 pacing
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 13, 0)
    cmpxchg(&sk->sk_pacing_status, SK_PACING_NONE, SK_PACING_NEEDED);
#endif

    atomic_inc(&active_connections);
    atomic_inc(&module_ref_count);

    if (lotserver_verbose) {
        unsigned long gbps_int = ca->target_rate / 125000000;
        unsigned long gbps_frac = (ca->target_rate % 125000000) * 100 / 125000000;
        unsigned int gain_int = ca->cwnd_gain / 10;
        unsigned int gain_frac = ca->cwnd_gain % 10;

        pr_info("lotspeed: [uk0@2025-11-19 17:06:58] NEW connection #%d | rate=%lu.%02lu Gbps | gain=%u.%ux | mode=%s\n",
                atomic_read(&active_connections),
                gbps_int, gbps_frac,
                gain_int, gain_frac,
                lotserver_turbo ? "TURBO" : (lotserver_adaptive ? "adaptive" : "fixed"));
    }
}

// 释放连接
static void lotspeed_release(struct sock *sk)
{
    struct lotspeed *ca = inet_csk_ca(sk);
    u64 duration;

    // 添加空指针检查
    if (!ca) {
        pr_warn("lotspeed: [uk0@2025-11-19 17:06:58] release called with NULL ca\n");
        atomic_dec(&active_connections);
        return;
    }

    // 安全获取 duration
    if (ca->start_time > 0) {
        duration = ktime_get_real_seconds() - ca->start_time;
    } else {
        duration = 0;
    }

    atomic_dec(&active_connections);

    // 只有在有数据时才更新统计
    if (ca->bytes_sent > 0) {
        atomic64_add(ca->bytes_sent, &total_bytes_sent);
    }
    if (ca->loss_count > 0) {
        atomic_add(ca->loss_count, &total_losses);
    }

    if (lotserver_verbose) {
        pr_info("lotspeed: [uk0@2025-11-19 17:06:58] connection released, active=%d\n",
                atomic_read(&active_connections));
    }

    // 清理 ca 结构
    memset(ca, 0, sizeof(struct lotspeed));
}

// 更新 RTT 统计
static void lotspeed_update_rtt(struct sock *sk)
{
    struct lotspeed *ca = inet_csk_ca(sk);
    struct tcp_sock *tp = tcp_sk(sk);
    u32 rtt_us = tp->srtt_us >> 3;
    s32 delta;
    u32 abs_delta;

    if (!rtt_us || rtt_us == 0)
        return;

    // 记录最小 RTT 作为基准
    if (!ca->rtt_min || rtt_us < ca->rtt_min) {
        ca->rtt_min = rtt_us;
        if (lotserver_verbose && ca->rtt_cnt > 100) {
            pr_debug("lotspeed: new min RTT: %u us\n", ca->rtt_min);
        }
    }

    ca->rtt_cnt++;

    if (!ca->rtt_ema) {
        ca->rtt_ema = rtt_us;
        ca->rtt_var = rtt_us >> 3;
        return;
    }

    delta = (s32)rtt_us - (s32)ca->rtt_ema;
    ca->rtt_ema += delta >> 3;

    abs_delta = delta < 0 ? -delta : delta;
    ca->rtt_var += ((s32)abs_delta - (s32)ca->rtt_var) >> 2;
}

// 自适应速率调整
static void lotspeed_adapt_rate(struct sock *sk, const struct rate_sample *rs)
{
    struct lotspeed *ca = inet_csk_ca(sk);
    struct tcp_sock *tp = tcp_sk(sk);
    u64 sample_bw = 0;
    u64 filtered_bw;
    u32 rtt_us = tp->srtt_us >> 3;
    u32 min_rtt = ca->rtt_min ? ca->rtt_min : rtt_us;
    bool ecn = rs && rs->is_ece;
    u32 mss = tp->mss_cache ? tp->mss_cache : 1460;
    u32 window_deadline;

    if (!lotserver_adaptive)
        goto rtt_check;

    // 计算实际带宽（瞬时值）
    if (rs && rs->delivered > 0 && rs->interval_us > 0) {
        sample_bw = (u64)rs->delivered * USEC_PER_SEC;
        do_div(sample_bw, rs->interval_us);

        // 发送字节统计
        ca->bytes_sent += (u64)rs->delivered * mss;

        // 指数滑动平均，抑制抖动
        if (!ca->actual_rate) {
            ca->actual_rate = sample_bw;
        } else {
            ca->actual_rate -= ca->actual_rate >> LOTSPEED_BW_EMA_SHIFT;
            ca->actual_rate += sample_bw >> LOTSPEED_BW_EMA_SHIFT;
        }

        // BBR 风格窗口最大值，探测更高带宽
        if (!ca->bw_window_max || sample_bw >= ca->bw_window_max) {
            ca->bw_window_max = sample_bw;
            ca->bw_window_stamp = tcp_jiffies32;
        } else {
            window_deadline = ca->bw_window_stamp +
                    (u32)msecs_to_jiffies(LOTSPEED_BW_WINDOW_MS);
            if (time_after32(tcp_jiffies32, window_deadline)) {
                ca->bw_window_max -= ca->bw_window_max >> LOTSPEED_BW_DECAY_SHIFT;
                if (ca->bw_window_max < ca->actual_rate)
                    ca->bw_window_max = ca->actual_rate;
                ca->bw_window_stamp = tcp_jiffies32;
            }
        }
    }

    filtered_bw = ca->actual_rate ? ca->actual_rate : sample_bw;

    if (filtered_bw) {
        // 如果实际速率远低于目标且存在丢包，快速降速
        if (filtered_bw < ca->target_rate / 2 && ca->loss_count > 0) {
            ca->target_rate = max_t(u64, filtered_bw * 15 / 10, lotserver_rate / 4);
            ca->cwnd_gain = max_t(u32, ca->cwnd_gain - 5, LOTSPEED_MIN_GAIN);
            if (lotserver_verbose) {
                unsigned long gbps_int = ca->target_rate / 125000000;
                unsigned long gbps_frac = (ca->target_rate % 125000000) * 100 / 125000000;
                unsigned int gain_int = ca->cwnd_gain / 10;
                unsigned int gain_frac = ca->cwnd_gain % 10;
                pr_info("lotspeed: adapt DOWN: rate=%lu.%02lu Gbps, gain=%u.%ux\n",
                        gbps_int, gbps_frac, gain_int, gain_frac);
            }
        }
        // 表现良好，逐步提升到窗口最大值
        else if (ca->loss_count == 0 &&
                 filtered_bw > ca->target_rate * 8 / 10) {
            u64 desired = ca->bw_window_max ?
                          min(ca->bw_window_max, lotserver_rate) :
                          lotserver_rate;
            u64 step = max_t(u64, ca->target_rate >> 3, mss * 8ULL);
            ca->target_rate = min_t(u64, ca->target_rate + step, desired);
            ca->cwnd_gain = min_t(u32, ca->cwnd_gain + 1, lotserver_gain);
        }
    }

rtt_check:
    // RTT 膨胀检测：阈值 = minRTT + max(minRTT/3, 1.5~2×方差)
    if (min_rtt && rtt_us) {
        u32 var = ca->rtt_var ? ca->rtt_var : min_rtt >> 3;
        u32 tolerance = min_rtt / 3;
        u32 var_term = (var * (ecn ? 3 : 4)) >> 1;
        u32 threshold = min_rtt + max(tolerance, var_term);

        if (!lotserver_turbo && rtt_us > threshold) {
            ca->cwnd_gain = max_t(u32, ca->cwnd_gain - 2, LOTSPEED_MIN_GAIN);
        } else if (ca->cwnd_gain < lotserver_gain) {
            ca->cwnd_gain++;
        }
    }
}

static u32 lotspeed_probe_threshold(const struct lotspeed *ca, u32 target_cwnd, u32 rtt_us)
{
    u32 denom = max(50u, target_cwnd);
    u32 cwnd_term = clamp_t(u32, LOTSPEED_PROBE_BASE / denom, 4u, 60u);
    u32 min_rtt = ca->rtt_min ? ca->rtt_min : (rtt_us ? rtt_us : 1000);
    u32 min_rtt_ms = max(1u, DIV_ROUND_UP(min_rtt, 1000));
    u32 rtt_term = clamp_t(u32, 50u / min_rtt_ms, 4u, 20u);

    return clamp_t(u32, cwnd_term + rtt_term,
                   LOTSPEED_PROBE_MIN, LOTSPEED_PROBE_MAX);
}

// 核心拥塞控制逻辑实现（内部函数）
static void lotspeed_cong_control_impl(struct sock *sk, const struct rate_sample *rs)
{
    struct tcp_sock *tp = tcp_sk(sk);
    struct lotspeed *ca = inet_csk_ca(sk);
    u64 rate;
    u32 cwnd;
    u32 rtt_us = tp->srtt_us >> 3;
    u32 mss = tp->mss_cache;
    u32 target_cwnd;
    u32 probe_threshold;

    // 默认值处理
    if (!rtt_us) rtt_us = 1000;   // 1ms 默认
    if (!mss) mss = 1460;          // 标准以太网 MSS

    // 更新 RTT 统计
    lotspeed_update_rtt(sk);

    // 自适应调整
    lotspeed_adapt_rate(sk, rs);

    // 选择速率
    rate = ca->target_rate;

    // 核心公式：CWND = (rate × RTT) / MSS × gain
    target_cwnd = div64_u64(rate * (u64)rtt_us, (u64)mss * 1000000);
    target_cwnd = div_u64(target_cwnd * ca->cwnd_gain, 10);

    probe_threshold = lotspeed_probe_threshold(ca,
                                               max_t(u32, target_cwnd, 1),
                                               rtt_us);

    // 慢启动阶段特殊处理
    if (ca->ss_mode && tp->snd_cwnd < tp->snd_ssthresh) {
        // 指数增长
        cwnd = tp->snd_cwnd * 2;
        if (cwnd >= target_cwnd) {
            ca->ss_mode = false;
            cwnd = target_cwnd;
        }
    } else {
        // 正常阶段
        cwnd = target_cwnd;

        // 周期性探测更高速率
        ca->probe_cnt++;
        if (ca->probe_cnt >= probe_threshold) {
            cwnd = cwnd * 11 / 10;   // 探测 +10%
            ca->probe_cnt = 0;
        }
    }

    // 应用安全限制
    cwnd = max_t(u32, cwnd, lotserver_min_cwnd);
    cwnd = min_t(u32, cwnd, lotserver_max_cwnd);
    cwnd = min_t(u32, cwnd, tp->snd_cwnd_clamp);

    // 设置拥塞窗口和 pacing 速率
    tp->snd_cwnd = cwnd;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 13, 0)
    // 改进: 给予 20% 的 Overhead 空间，防止 Pacing 限制了 TCP 本身的突发能力
    // 许多网卡需要小规模的突发来维持高吞吐
    u64 pacing = rate + (rate >> 2); // Rate * 1.25
    sk->sk_pacing_rate = pacing;
#endif

    // 定期状态输出
    if (lotserver_verbose && ca->rtt_cnt > 0 && ca->rtt_cnt % 1000 == 0) {
        unsigned long gbps_int = rate / 125000000;
        unsigned long gbps_frac = (rate % 125000000) * 100 / 125000000;
        unsigned int gain_int = ca->cwnd_gain / 10;
        unsigned int gain_frac = ca->cwnd_gain % 10;

        pr_info("lotspeed: [uk0] STATUS: cwnd=%u | rate=%lu.%02lu Gbps | RTT=%u us | gain=%u.%ux | losses=%u\n",
                cwnd, gbps_int, gbps_frac, rtt_us, gain_int, gain_frac, ca->loss_count);
    }
}

// 主拥塞控制函数 - 兼容不同内核版本
#ifdef NEW_CONG_CONTROL_API
// 新版本内核 (5.19-6.7.x, 6.9+, 6.17+)
static void lotspeed_cong_control(struct sock *sk, u32 ack, int flag,
                                  const struct rate_sample *rs)
{
    // 新版本可以使用 ack 和 flag 参数进行更精细的控制
    #ifdef KERNEL_6_17_PLUS
    // 6.17+ 内核的特殊处理
    if (flag & CA_ACK_ECE && lotserver_verbose) {
        pr_debug("lotspeed: [6.17+] ECN echo received, ack=%u\n", ack);
    }
    #endif

    // 调用实际的拥塞控制逻辑
    lotspeed_cong_control_impl(sk, rs);
}
#else
// 旧版本内核 (5.18 及以下, 6.8.0-6.8.x)
static void lotspeed_cong_control(struct sock *sk, const struct rate_sample *rs)
{
    // 直接调用实际的拥塞控制逻辑
    lotspeed_cong_control_impl(sk, rs);
}
#endif

// 处理状态变化
static void lotspeed_set_state(struct sock *sk, u8 new_state)
{
    struct lotspeed *ca = inet_csk_ca(sk);
    struct tcp_sock *tp = tcp_sk(sk);

    switch (new_state) {
        case TCP_CA_Loss:
            // 涡轮模式根据软/硬策略选择是否忽略
            if (lotspeed_turbo_should_ignore(ca, "loss")) {
                lotspeed_consume_turbo_ignore(ca);
                tp->snd_ssthresh = TCP_INFINITE_SSTHRESH;
                return;
            }
            // 记录丢包
            ca->loss_count++;
            ca->cwnd_gain = max_t(u32, ca->cwnd_gain * 8 / 10, LOTSPEED_MIN_GAIN);

            if (lotserver_verbose && (ca->loss_count == 1 || ca->loss_count % 10 == 0)) {
                unsigned int gain_int = ca->cwnd_gain / 10;
                unsigned int gain_frac = ca->cwnd_gain % 10;
                pr_info("lotspeed: LOSS #%u detected, gain reduced to %u.%ux\n",
                        ca->loss_count, gain_int, gain_frac);
            }
            break;

        case TCP_CA_Recovery:
            // 进入恢复阶段
            if (!lotserver_turbo) {
                ca->cwnd_gain = max_t(u32, ca->cwnd_gain * 9 / 10, 15);
            }
            break;

        case TCP_CA_Open:
            // 恢复正常
            ca->ss_mode = false;
            lotspeed_reset_turbo_budget(ca);
            break;

        default:
            break;
    }
}

// 丢包时的 ssthresh
static u32 lotspeed_ssthresh(struct sock *sk)
{
    struct lotspeed *ca = inet_csk_ca(sk);
    struct tcp_sock *tp = tcp_sk(sk);
    u32 thresh;

    // 硬涡轮：永不降速
    if (lotserver_turbo && !lotserver_soft_turbo) {
        return TCP_INFINITE_SSTHRESH;
    }

    if (lotserver_turbo && lotspeed_turbo_ignore_active(ca)) {
        lotspeed_consume_turbo_ignore(ca);
        return TCP_INFINITE_SSTHRESH;
    }

    // 温和降速
    ca->loss_count++;
    ca->cwnd_gain = max_t(u32, ca->cwnd_gain * 8 / 10, LOTSPEED_MIN_GAIN);

    thresh = max_t(u32, tp->snd_cwnd * 7 / 10, lotserver_min_cwnd);
    return thresh;
}

// 恢复拥塞窗口
static u32 lotspeed_undo_cwnd(struct sock *sk)
{
    struct tcp_sock *tp = tcp_sk(sk);
    struct lotspeed *ca = inet_csk_ca(sk);

    // 误判恢复，重置丢包计数
    ca->loss_count = 0;
    ca->ss_mode = false;

    return max(tp->snd_cwnd, tp->prior_cwnd);
}

// 处理拥塞事件
static void lotspeed_cwnd_event(struct sock *sk, enum tcp_ca_event event)
{
    struct lotspeed *ca = inet_csk_ca(sk);

    switch (event) {
        case CA_EVENT_LOSS:
            // 发生丢包
            if (lotserver_turbo && (!lotserver_soft_turbo || lotspeed_turbo_ignore_active(ca))) {
                lotspeed_consume_turbo_ignore(ca);
                break;
            }
            ca->loss_count++;
            if (!lotserver_turbo || lotserver_soft_turbo) {
                ca->cwnd_gain = max_t(u32, ca->cwnd_gain - 5, LOTSPEED_MIN_GAIN);
            }
            break;

        case CA_EVENT_TX_START:
            // 开始传输
            ca->ss_mode = true;
            ca->probe_cnt = 0;
            lotspeed_reset_turbo_budget(ca);
            break;

        case CA_EVENT_CWND_RESTART:
            // 重新开始
            ca->ss_mode = true;
            ca->loss_count = 0;
            ca->probe_cnt = 0;
            lotspeed_reset_turbo_budget(ca);
            break;

        default:
            // 其他事件忽略
            break;
    }
}

static struct tcp_congestion_ops lotspeed_ops __read_mostly = {
        .name           = "lotspeed",
        .owner          = THIS_MODULE,
        .init           = lotspeed_init,
        .release        = lotspeed_release,
        .cong_control   = lotspeed_cong_control,
        .set_state      = lotspeed_set_state,
        .ssthresh       = lotspeed_ssthresh,
        .undo_cwnd      = lotspeed_undo_cwnd,
        .cwnd_event     = lotspeed_cwnd_event,
        .flags          = TCP_CONG_NON_RESTRICTED,
};

// 辅助函数来格式化带边框的行
static void print_boxed_line(const char *prefix, const char *content)
{
    int prefix_len = strlen(prefix);
    int content_len = strlen(content);
    int total_len = prefix_len + content_len;
    int padding = 56 - total_len;  // 56 = 60 - 2个边框字符

    if (padding < 0) padding = 0;

    pr_info("║%s%s%*s║\n", prefix, content, padding, "");
}

static int __init lotspeed_module_init(void)
{
    unsigned long gbps_int, gbps_frac;
    unsigned int gain_int, gain_frac;
    char buffer[128];

    BUILD_BUG_ON(sizeof(struct lotspeed) > ICSK_CA_PRIV_SIZE);

    pr_info("╔════════════════════════════════════════════════════════╗\n");
    pr_info("║          LotSpeed v2.0 - 锐速复活版                    ║\n");

    // 动态生成时间和用户行
    snprintf(buffer, sizeof(buffer), "uk0 @ 2025-11-19 17:06:58");
    print_boxed_line("          Created by ", buffer);

    // 动态生成内核版本行
    snprintf(buffer, sizeof(buffer), "%u.%u.%u",
             LINUX_VERSION_CODE >> 16,
             (LINUX_VERSION_CODE >> 8) & 0xff,
             LINUX_VERSION_CODE & 0xff);
    print_boxed_line("          Kernel: ", buffer);

    // 显示API版本信息
#ifdef KERNEL_6_17_PLUS
    pr_info("║          API: NEW (6.17+ special)                      ║\n");
#elif defined(NEW_CONG_CONTROL_API)
    pr_info("║          API: NEW (5.19-6.7.x, 6.9+)                   ║\n");
#elif defined(OLD_CONG_CONTROL_API)
    pr_info("║          API: LEGACY (6.8.0-6.8.x and older)           ║\n");
#else
    pr_info("║          API: LEGACY (<5.19)                           ║\n");
#endif

    pr_info("╚════════════════════════════════════════════════════════╝\n");

    gbps_int = lotserver_rate / 125000000;
    gbps_frac = (lotserver_rate % 125000000) * 100 / 125000000;
    gain_int = lotserver_gain / 10;
    gain_frac = lotserver_gain % 10;

    pr_info("Initial Parameters:\n");
    pr_info("  Rate: %lu.%02lu Gbps\n", gbps_int, gbps_frac);
    pr_info("  Gain: %u.%ux\n", gain_int, gain_frac);
    pr_info("  Min/Max CWND: %u/%u\n", lotserver_min_cwnd, lotserver_max_cwnd);
    pr_info("  Adaptive: %s | Turbo: %s | Verbose: %s\n",
            lotserver_adaptive ? "ON" : "OFF",
            lotserver_turbo ? "ON" : "OFF",
            lotserver_verbose ? "ON" : "OFF");

    return tcp_register_congestion_control(&lotspeed_ops);
}

static void __exit lotspeed_module_exit(void)
{
    u64 total_bytes;
    u64 gb_sent, mb_sent;
    int active_conns;
    int retry_count = 0;

    pr_info("lotspeed: [uk0@2025-11-19 17:06:58] Beginning module unload\n");

    // 先注销算法，防止新连接使用
    tcp_unregister_congestion_control(&lotspeed_ops);
    pr_info("lotspeed: Unregistered from TCP stack\n");

    // 等待现有连接释放（最多等待5秒）
    while (atomic_read(&active_connections) > 0 && retry_count < 50) {
        pr_info("lotspeed: Waiting for %d connections to close (attempt %d/50)\n",
                atomic_read(&active_connections), retry_count + 1);
        msleep(100);  // 等待100ms
        retry_count++;
    }

    active_conns = atomic_read(&active_connections);

    if (active_conns > 0) {
        pr_err("lotspeed: WARNING - Force unloading with %d active connections!\n", active_conns);
        pr_err("lotspeed: This may cause system instability!\n");

        if (!force_unload) {
            pr_err("lotspeed: Refusing to unload. Set force_unload=1 to override\n");
            pr_err("lotspeed: echo 1 > /sys/module/lotspeed/parameters/force_unload\n");

            // 重新注册以保持稳定
            tcp_register_congestion_control(&lotspeed_ops);
            return;  // 拒绝卸载
        }
    }

    total_bytes = atomic64_read(&total_bytes_sent);
    gb_sent = total_bytes >> 30;
    mb_sent = (total_bytes >> 20) & 0x3FF;

    pr_info("╔════════════════════════════════════════════════════════╗\n");
    pr_info("║          LotSpeed v2.0 Unloaded                        ║\n");
    pr_info("║          Time: 2025-11-19 17:06:58                     ║\n");
    pr_info("║          User: uk0                                     ║\n");
    pr_info("║          Active Connections: %-26d║\n", active_conns);
    pr_info("║          Data Sent: %llu.%llu GB%*s║\n",
            gb_sent, mb_sent * 1000 / 1024,
            (int)(30 - snprintf(NULL, 0, "%llu.%llu GB", gb_sent, mb_sent * 1000 / 1024)), "");
    pr_info("╚════════════════════════════════════════════════════════╝\n");
}

module_init(lotspeed_module_init);
module_exit(lotspeed_module_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("uk0 <github.com/uk0>");
MODULE_VERSION("2.0");
MODULE_DESCRIPTION("LotSpeed v2.0 - Modern LotServer/ServerSpeeder replacement for 1G~40G networks");
MODULE_ALIAS("tcp_lotspeed");