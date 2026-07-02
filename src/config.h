#pragma once
// config.h — HostConfig(POD)+ 命令行解析。位置参数,轻量(不过度设计)。

#include <cstdio>
#include <cstdlib>
#include <string>

namespace tflive {

// 运行配置。段名随部署而变 → 参数化;poll_us/cpu/progress 随运维而变 → 参数化。
struct HostConfig {
  std::string okx_depth;   // OKX 多档 book 段(gconf DepthBoard)
  std::string okx_trade;   // OKX 成交段(gconf TradeRing)
  std::string bn_depth;    // BN 多档 book 段(取 L0 当 mid)
  std::string out_seg;     // 输出 FactorBoard 段名
  int         cpu = -1;    // 绑核(-1=不绑)
  int         poll_us = 200;
  int         progress_sec = 5;
  std::string csv;         // 可选:每结算秒逐行落 CSV(对拍/复现用)
};

inline void print_usage(const char* argv0) {
  std::fprintf(stderr,
      "用法: %s <okx_depth_seg> <okx_trade_seg> <bn_depth_seg> <out_seg> [cpu] [poll_us] [csv]\n"
      "  轮询 gconf shm(OKX DepthBoard+TradeRing / BN DepthBoard)→ tick_feat 引擎 → 写 FactorBoard。\n"
      "  cpu: 绑核(缺省 -1 不绑); poll_us: 轮询间隔(缺省 200); csv: 逐秒落盘路径(可选,对拍用)。\n",
      argv0);
}

// 解析位置参数;不足 4 个 → 打用法后退出(启动期错误,不半带病启动)。
[[nodiscard]] inline HostConfig parse_args(int argc, char** argv) {
  if (argc < 5) { print_usage(argv[0]); std::exit(2); }
  HostConfig cfg;
  cfg.okx_depth = argv[1];
  cfg.okx_trade = argv[2];
  cfg.bn_depth  = argv[3];
  cfg.out_seg   = argv[4];
  if (argc > 5) cfg.cpu     = std::atoi(argv[5]);
  if (argc > 6) cfg.poll_us = std::atoi(argv[6]);
  if (argc > 7) cfg.csv     = argv[7];
  return cfg;
}

}  // namespace tflive
