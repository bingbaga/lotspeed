## LotSpeed TCP 拥塞控制原理与优化分析

### 1. 模块定位与整体流程
- LotSpeed 以可加载内核模块形式注册 `tcp_congestion_ops`，在 `init`/`release`/`cong_control`/`set_state` 等回调中接管拥塞控制。
- 加入连接时保存私有状态 `struct lotspeed`，包含目标速率、实际估计带宽、RTT 统计、增益、Turbo 标志等。
- 主循环在 `lotspeed_cong_control()` → `lotspeed_cong_control_impl()` 执行：
  1. 根据 ACK 派生的 `rate_sample` 估算瞬时带宽与 RTT；
  2. 调用 `lotspeed_adapt_rate()` 根据带宽/RTT/丢包调节目标速率与增益；
  3. 用核心公式 `cwnd = rate * RTT / MSS * gain` 计算期望 cwnd，配合慢启动/探测逻辑与 min/max clamp；
  4. 设置新的 `snd_cwnd` 和 `sk_pacing_rate` (约 1.2× target rate)。

### 2. 关键状态机与时序

```
 Client                                   Server (LotSpeed TCP stack)
   | SYN --------------------------------------> |
   |                                            | lotspeed_init():
   |                                            |   - set ssthresh / enable pacing
   |                                            |   - ca state reset, ss_mode = true
   |<-- SYN/ACK --------------------------------|
   | ACK -------------------------------------->|
   |====--> Data Flight =======================>| lotspeed_cong_control():
   |                                            |   for each ACK:
   |                                            |     rs = rate_sample
   |                                            |     update RTT min/actual bw
   |                                            |     adapt target_rate & cwnd_gain
   |                                            |     compute cwnd, pacing
   |                                            |     apply turbo/adaptive rules
   |                                            |<-- sends data paced at 1.2×rate
   |<== DupACK / ECE (if loss/ECN) ==           | lotspeed_set_state():
   |                                            |   adjust gain / ssthresh
   | FIN -------------------------------------->|
   |                                            | lotspeed_release():
   |                                            |   stats aggregation, ca reset
```

- 时序要点：`probe_cnt` 每 100 个 RTT 触发一次正向探测 (+10% cwnd)；丢包事件进入 `TCP_CA_Loss`/`TCP_CA_Recovery` 路径，Turbo 模式则忽略降速。

### 3. TCP 加速实现机制
- **速率驱动的 cwnd 计算**：通过用户指定的 `lotserver_rate` 和增益 `lotserver_gain`，直接将吞吐目标映射为窗口大小，绕过传统 AIMD 的缓慢加速。
- **分层自适应**：`lotserver_adaptive` 打开后对 `rate_sample` 做反馈，若实际带宽低于 0.5×目标且有丢包则降速，否则逐步恢复至预设速率。
- **RTT 膨胀保护**：如果 `srtt` 超出最小 RTT 的 1.5×，且非 Turbo 模式，则下调 `cwnd_gain`，抑制排队延迟。
- **Turbo/Burst 支持**：Turbo 模式把 `ssthresh` 设为无穷大且忽略 loss event；同时 pacing 速率设为 1.2×，允许一定突发性提高链路利用率。
- **可调 min/max cwnd**：允许针对高 BDP 链路预留窗口，避免 Linux 默认 clamp 限制。

### 4. 优化空间与建议
- **更细粒度的带宽估计**：当前 `actual_rate` 直接取 `delivered/interval`，缺少 filter；可引入 EMA 或 BBR 式 windowed max 减少抖动。
- **RTT 膨胀阈值**：固定 1.5× 可能对高噪声链路过敏；可按 `rtt_min`/方差自适应，或引入 ECN 信号优先级。
- **探测间隔**：`probe_cnt >= 100` 相当于 100×RTT 才探测一次，对高速低 RTT 场景过慢，可根据 BDP、自适应增减。
- **Turbo 风险控制**：忽略所有拥塞信号可能触发交换机保护，可增加“软 Turbo”模式：只忽略单次 loss，持续丢包仍降速。
- **统计导出**：目前仅 `pr_info` 打印，建议通过 `netlink` 或 `debugfs` 导出指标，便于用户态监控/自适应策略。
- **多队列 NIC 协同**：可参考 BBR v3，将 pacing rate 与 GSO size 结合，减少 CPU。当前仅设置 `sk_pacing_rate`，未处理 TSO sizing。

### 5. 结论
LotSpeed 通过速率驱动的窗口计算 + 自适应增益，实现类似旧 LotServer 的“高初速” TCP 加速。关键优势是部署简单、参数直观；同时仍可在 RTT、丢包事件中温和回退。进一步强化带宽/RTT 估计、探测策略和可观测性，有望在高 BDP/多租户网络中取得更稳定的收益。
