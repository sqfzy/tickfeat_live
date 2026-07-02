// main.cpp — tickfeat_live:轮询 gconf shm(OKX DepthBoard+TradeRing / BN DepthBoard)→
// per-LID tick_feat 流式引擎 → 每秒结算 f0-f9 写 FactorBoard。跑完(段无新数据后手动/信号)即退。
//
// 编排即目录:parse → attach → 建引擎 → 循环{收一拍 → 按序喂 → 结算写出 → 观测} → 收尾。

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <thread>

#include <pthread.h>
#include <spdlog/spdlog.h>

#include "config.h"
#include "shm_in.h"
#include "feed.h"
#include "emit.h"

namespace {

std::atomic<bool> g_stop{false};
void on_signal(int) { g_stop.store(true, std::memory_order_relaxed); }
void install_signals() { std::signal(SIGINT, on_signal); std::signal(SIGTERM, on_signal); }

void pin_cpu(int cpu) {
  if (cpu < 0) return;
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(cpu, &set);
  if (pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0) spdlog::info("绑核 cpu {}", cpu);
  else spdlog::warn("绑核 cpu {} 失败", cpu);
}

using clk = std::chrono::steady_clock;

// 周期观测:吞吐 ev/s + 每拍处理延迟峰值(collect+feed+emit)。
struct Progress {
  std::uint64_t events = 0;
  std::uint64_t max_tick_us = 0;
  clk::time_point last_log = clk::now();
};

void observe(Progress& pr, std::size_t tick_events, std::uint64_t tick_us, const tflive::HostConfig& cfg) {
  pr.events += tick_events;
  if (tick_us > pr.max_tick_us) pr.max_tick_us = tick_us;
  const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(clk::now() - pr.last_log).count();
  if (elapsed < cfg.progress_sec) return;
  spdlog::info("progress: {} 事件 / {}s(峰值处理延迟 {}µs/拍)", pr.events, elapsed, pr.max_tick_us);
  pr.events = 0;
  pr.max_tick_us = 0;
  pr.last_log = clk::now();
}

void log_summary(const tflive::FeedState& fst, const tflive::EmitState& est) {
  std::size_t rows = 0, syms = 0;
  for (std::size_t r : est.last_rows) { rows += r; if (r > 0) ++syms; }
  spdlog::info("stopped: 出因子 {} 币 / {} 行", syms, rows);
  if (fst.lapped) spdlog::warn("trade 环绕圈丢 {} 条(轮询跟不上成交)", fst.lapped);
}

void run_replay(const tflive::HostConfig& cfg, tflive::Inputs& in,
                tflive::EngineSet& eng, tflive::FactorBoard* out) {
  tflive::FeedState fst = tflive::make_feed_state();
  tflive::EmitState est = tflive::make_emit_state();
  std::FILE* csv = cfg.csv.empty() ? nullptr : std::fopen(cfg.csv.c_str(), "w");
  if (csv) std::fprintf(csv, "lid,ts_us,f0,f1,f2,f3,f4,f5,f6,f7,f8,f9,mid,pdiff\n");

  Progress pr;
  spdlog::info("running: {} 币, poll {}µs", static_cast<int>(gconf::sym::N_SYMS), cfg.poll_us);
  while (!g_stop.load(std::memory_order_relaxed)) {
    const auto t0 = clk::now();
    const std::size_t ev = tflive::collect_tick(in, fst);
    tflive::feed_in_order(eng, fst);
    tflive::emit_settled(eng, out, est, csv);
    const auto us = std::chrono::duration_cast<std::chrono::microseconds>(clk::now() - t0).count();
    observe(pr, ev, static_cast<std::uint64_t>(us), cfg);
    std::this_thread::sleep_for(std::chrono::microseconds(cfg.poll_us));
  }
  if (csv) std::fclose(csv);
  log_summary(fst, est);
}

}  // namespace

int main(int argc, char** argv) {
  spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v");
  install_signals();

  const tflive::HostConfig cfg = tflive::parse_args(argc, argv);
  pin_cpu(cfg.cpu);

  tflive::Inputs in;
  if (!tflive::attach_inputs(cfg, in)) return 1;
  tflive::FactorBoard* out = tflive::create_output(cfg);
  if (!out) return 1;

  tflive::EngineSet eng = tflive::make_engines();
  run_replay(cfg, in, eng, out);
  return 0;
}
