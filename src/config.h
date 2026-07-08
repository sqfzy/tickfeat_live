#pragma once
// config.h — HostConfig(POD)+ 命令行解析。位置参数,轻量(不过度设计)。

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace tflive {

// 运行配置。段名随部署而变 → 参数化;poll_us/cpu/progress 随运维而变 → 参数化。
struct HostConfig {
  std::string okx_depth;   // OKX 多档 book 段(gconf DepthBoard)
  std::string okx_trade;   // OKX 成交段(gconf TradeRing)
  std::string bn_booktick; // BN bookTicker 段(单档 BBO,取 mid)
  std::string out_seg;     // 输出 FactorBoard 段名
  int         cpu = -1;    // 绑核(-1=不绑)
  int         poll_us = 200;
  int         progress_sec = 5;
  std::string dump;        // --dump <csv>:每条结算行落盘(对拍/复现);空=不落盘
};

inline void print_usage(const char* argv0) {
  std::fprintf(stderr,
      "用法: %s <okx_depth_seg> <okx_trade_seg> <bn_booktick_seg> <out_seg> [cpu] [poll_us] [--dump <jsonl>]\n"
      "  轮询 gconf shm(OKX DepthBoard+TradeRing / BN BookTickBoard)→ tick_feat 引擎 → 写 FactorBoard。\n"
      "  注: mid/imb1/imb5/spread 全用 OKX DepthBoard(5 档)。\n"
      "  cpu: 绑核(缺省 -1 不绑); poll_us: 轮询间隔(缺省 200)。\n"
      "  --dump <csv>: 每条结算行落 CSV(Quill 异步,不影响热路径;逐位对拍/复现用,无漏秒)。\n"
      "  监控(每币每秒 10 因子必更新)走 Quill 日志:漏秒即 WARN,progress 汇总本周期漏秒事件数。\n",
      argv0);
}

// 解析:先分离 --dump 旗标,再按位置取 4 段(+可选 cpu/poll_us)。不足 4 个位置参 → 打用法后退出。
[[nodiscard]] inline HostConfig parse_args(int argc, char** argv) {
  HostConfig cfg;
  std::vector<std::string> pos;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--dump" && i + 1 < argc) cfg.dump = argv[++i];
    else pos.push_back(a);
  }
  if (pos.size() < 4) { print_usage(argv[0]); std::exit(2); }
  cfg.okx_depth   = pos[0];
  cfg.okx_trade   = pos[1];
  cfg.bn_booktick = pos[2];
  cfg.out_seg     = pos[3];
  if (pos.size() > 4) cfg.cpu     = std::atoi(pos[4].c_str());
  if (pos.size() > 5) cfg.poll_us = std::atoi(pos[5].c_str());
  return cfg;
}

}  // namespace tflive
