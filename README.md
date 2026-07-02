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

## 依赖(外部,非 vendored)
- **tick_feat 引擎**:`../cpp/src`(`tick_feat.hpp` + `live/streaming_engine.hpp` + `event.hpp`)
- **gconf 段契约**:`../mdreplay/gconf/include`(官方 `DepthBoard`/`TradeRing`/`BookTickBoard`,submodule `jt_dev3`)
- **spdlog**:系统 pkg-config

> 需上述两处以**同级目录**存在(`cpp/` 与 `mdreplay/` 是本仓的 sibling)。将来可改submodule。

## 构建 / 运行
```bash
xmake build                      # tickfeat_live + tickfeat_dump
./tickfeat_live <okx_depth> <okx_trade> <bn_depth> <out_seg> [cpu] [poll_us] [csv]
./tickfeat_dump <out_seg> <lid>  # 参照消费者:读回某 LID 的 f0-f9
```

## 结构(POD + 自由函数 + 函数即目录)
| 文件 | 一件事 |
|---|---|
| `config.h` | `HostConfig`(POD)+ 解析 |
| `convert.h` | 定点 mantissa → 引擎单位(纯函数)|
| `shm_in.h` | `Inputs`(POD)+ attach 段 |
| `feed.h` | `Pending`/`FeedState`(POD)+ 轮询收集 / 全局 ts 序喂 |
| `emit.h` | 引擎结算秒 → 写 `FactorBoard` + valid_mask |
| `factor_board.h` | 输出段(POD,复用 gconf seqlock 框架)|
| `main.cpp` | 编排 + 循环 + 信号 + 周期观测(吞吐/每拍延迟)|
| `dump.cpp` | 参照消费者 |
