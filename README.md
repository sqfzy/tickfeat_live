# tickfeat_live

实时 f0–f9 因子 host:**轮询** gconf shm 行情段 → per-symbol `tick_feat` 流式引擎 → 每秒结算
f0–f9 写 `FactorBoard` 段。跑完(段无新数据后手动/信号)即退。

## 是什么 / 不是什么
- **是**:一个忠实的**生产轮询消费者**——像 factor_calc_v2/msig 一样轮询 latest-wins 段。
- **不是**:因子算法本身。f0–f9 的数值逻辑一行没重写,直接复用 `tick_feat` 流式引擎。

## 与离线参照的一致性
- **paced replay 下已验证 bit-exact**:经 `mdreplay` 定速回放(realtime=1.0, 无 CPU 争用)喂 shm,
  在线链路对离线黄金参照 `tick_feat_deliverable` 全 27 币 × **11 列(f0–f9 + mid)max|diff| = 0**;
  源血缘(`--dump` 的 6 列)抽查 0 不符。见 `test/verify_live.sh`。
- **real-time latest-wins 下有非确定残差**:真盘全速轮询是有损采样——高速/CPU 争用时会漏采桶末快照,
  致个别币个别秒残差(随时序漂移, 非算法问题)。这是轮询消费模型的固有边界, 非管线缺陷。
- `pdiff` 非参照列(离线只出 f0–f9 + mid), 它是 f8/f9 的原始输入 → 由 f8/f9 逐位一致间接背书。

## 数据流
```
OKX DepthBoard(5+档 book)┐
OKX TradeRing(成交)      ┼─ 轮询/drain → 换算 → per-LID StreamingFeatureEngine → FactorBoard
BN  BookTickBoard(BBO mid)┘                                      (可选 --dump <csv>:逐结算行 + 6 列源血缘)
```

## 依赖
- **gconf 段契约**:`gconf/`(**git submodule** → `hft-infra/gconf` @ jt_dev3)。含 `DepthBoard`/`TradeRing`/
  `BookTickBoard` 等行情段,以及本项目的输出段 **`factor_board.h`(`gconf::shm::v2::FactorBoard`)** 与段名
  `shm_names.h::TICKFEAT_FACTOR = /shm_tickfeat`。首次 clone 后 `git submodule update --init`。
- **tick_feat 引擎**(已 vendored 进 `src/`):`tick_feat.hpp` + `streaming_engine.hpp` + `event.hpp`
  (源自 `../cpp/src`, 改动须同步)。
- **Quill**:高性能异步日志(header-only;监控 + `--dump` 落盘走 Quill,见 `log.h`)。

> gconf 走 submodule(`gconf/include`),与生产者共用同一份权威契约。

## 构建 / 运行
```bash
git submodule update --init      # 拉 gconf submodule
xmake build                      # tickfeat_live + tickfeat_dump
./tickfeat_live <okx_depth> <okx_trade> <bn_booktick> <out_seg> [cpu] [poll_us] [--dump <csv>]
./tickfeat_dump <out_seg> <lid>  # 参照消费者:读回某 LID 的 f0-f9 + mid + pdiff
# 例(云机真实段 → 因子段 TICKFEAT_FACTOR):
./tickfeat_live /shm_okx_swap_depth_v2 /shm_okx_swap_trade_v2 /shm_bn_swap_book_tick_v2 /shm_tickfeat 15 200
# --dump <csv>:逐结算行落 CSV(Quill 异步, 不碰热路径)+ 6 列源血缘, 供对拍/复现/溯源。

bash test/verify_live.sh          # 端到端验证:回放 → --dump → 对拍(11 列 max|diff|=0) + 血缘核对
```

## 结构(POD + 自由函数 + 函数即目录)
| 文件 | 一件事 |
|---|---|
| `config.h` | `HostConfig`(POD)+ 解析 |
| `convert.h` | 定点 mantissa → 引擎单位(纯函数)|
| `shm_in.h` | `Inputs`(POD)+ attach 段 |
| `feed.h` | `Pending`/`FeedState`(POD)+ 轮询收集 / 全局 ts 序喂 |
| `emit.h` | 引擎结算秒 → 写 `FactorBoard` + valid_mask;`--dump` 时逐结算行落 CSV(14 因子列 + 6 血缘列);漏秒 WARN |
| `log.h` | Quill 全局 logger + `--dump` 的独立 FileSink logger(裸 CSV, 异步不碰热路径)|
| `streaming_engine.hpp` | `compute_day` 的事件流增量形式;当秒各段 id 区间 → 逐行 `src_` 血缘旁路(不进 11 列 Features)|
| `event.hpp` | 事件契约;`update_id`(OB/BN)/`exch_ns`(trade)惰性透传作源血缘键 |
| `main.cpp` | 编排 + 循环 + 信号 + 周期观测(吞吐/每拍延迟/漏秒)|
| `dump.cpp` | 参照消费者(读回 f0-f9 + mid + pdiff)|
| `test/verify_live.sh` | 端到端对拍脚手架(bit-exact + 血缘;+ `diff_factors.py`/`check_lineage.py`)|
| `gconf/…/factor_board.h` | 输出段契约(vendored gconf,`gconf::shm::v2::FactorBoard`)|
