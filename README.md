# tickfeat_live

实时 f0–f9 因子 host:**轮询** gconf shm 行情段 → per-symbol `tick_feat` 流式引擎 → 每秒结算
f0–f9 写 `FactorBoard` 段。跑完(段无新数据后手动/信号)即退。

## 是什么 / 不是什么
- **是**:一个忠实的**生产轮询消费者**——像 factor_calc_v2/msig 一样轮询 latest-wins 段。
  轮询是生产消费模型,host 照做,不追离线全序 bit-exact(跨所 f8/f9 有 ~1e-3bps 残差,如实标 valid_mask)。
- **不是**:因子算法本身。f0–f9 的数值逻辑一行没重写,直接复用 `tick_feat` 流式引擎。

## 数据流
```
OKX DepthBoard(5+档 book)┐
OKX TradeRing(成交)      ┼─ 轮询/drain → 换算 → per-LID StreamingFeatureEngine → FactorBoard
BN  DepthBoard(取 L0 mid)┘
```

## 依赖
- **gconf 段契约**:`gconf/`(**vendored**,jt_dev3 权威版)。含 `DepthBoard`/`TradeRing`/`BookTickBoard`
  等行情段,以及本项目的输出段 **`factor_board.h`(`gconf::shm::v2::FactorBoard`)** 与段名 `shm_names.h::F0F9_FACTOR = /shm_f0f9_v2`。
- **tick_feat 引擎**(外部,非 vendored):`../cpp/src`(`tick_feat.hpp` + `live/streaming_engine.hpp` + `event.hpp`)。
- **Quill**:高性能异步日志(header-only;监控走日志,见 `log.h`)。引擎 `tick_feat.hpp` 仍链 spdlog。

> `../cpp/` 需以同级目录存在。gconf 已收进仓库(`gconf/include`),仓库自洽。

## 构建 / 运行
```bash
xmake build                      # tickfeat_live + tickfeat_dump
./tickfeat_live <okx_depth> <okx_trade> <bn_depth> <out_seg> [cpu] [poll_us]
./tickfeat_dump <out_seg> <lid>  # 参照消费者:读回某 LID 的 f0-f9 + mid + pdiff
# 例(云机真实段 → 因子段 F0F9_FACTOR):
./tickfeat_live /shm_okx_swap_depth_v2 /shm_okx_swap_trade_v2 /shm_bn_swap_depth_v2 /shm_f0f9_v2 15 200
```

## 结构(POD + 自由函数 + 函数即目录)
| 文件 | 一件事 |
|---|---|
| `config.h` | `HostConfig`(POD)+ 解析 |
| `convert.h` | 定点 mantissa → 引擎单位(纯函数)|
| `shm_in.h` | `Inputs`(POD)+ attach 段 |
| `feed.h` | `Pending`/`FeedState`(POD)+ 轮询收集 / 全局 ts 序喂 |
| `emit.h` | 引擎结算秒 → 写 `FactorBoard` + valid_mask;逐秒查「每币每秒必更新」→ 漏秒 WARN |
| `log.h` | Quill 全局 logger + 初始化(监控/观测走日志)|
| `main.cpp` | 编排 + 循环 + 信号 + 周期观测(吞吐/每拍延迟/漏秒)|
| `dump.cpp` | 参照消费者(读回 f0-f9 + mid + pdiff)|
| `gconf/…/factor_board.h` | 输出段契约(已迁入 vendored gconf,`gconf::shm::v2::FactorBoard`)|
