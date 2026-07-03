#pragma once
// emit.h — 把各引擎新结算的秒行写进 FactorBoard(latest 写槽);可选逐行落 CSV(对拍/复现)。

#include <cmath>
#include <cstdint>
#include <vector>

#include "log.h"
#include "tick_feat.hpp"   // tick_feat::Features

#include "feed.h"          // EngineSet
#include "factor_board.h"

namespace tflive {

// per-LID 已写出的行数 + 首行 ts(算暖机 span 用)+ 上一行 ts(查每秒必更新)。
struct EmitState {
  std::vector<std::size_t>  last_rows;
  std::vector<std::int64_t> first_row_ts;
  std::vector<std::int64_t> last_emit_ts;   // 每 LID 上一行的秒(µs);相邻 !=1s → 漏秒
  std::uint64_t             gap_events = 0;  // 漏秒事件累计(监控:某币未每秒更新)
};

inline EmitState make_emit_state() {
  EmitState s;
  s.last_rows.assign(gconf::sym::N_SYMS, 0);
  s.first_row_ts.assign(gconf::sym::N_SYMS, 0);
  s.last_emit_ts.assign(gconf::sym::N_SYMS, 0);
  return s;
}

// 历史时间跨度(秒)→ valid_mask:各因子按暖机窗口标可信(精确有效性由消费者结合 ts 老化)。
inline std::uint16_t valid_mask_of(std::int64_t span_s) {
  std::uint16_t m = (1u << 0) | (1u << 1) | (1u << 2) | (1u << 10);  // f0,f1,f2,mid 立即
  if (span_s >= 60)    m |= (1u << 3);              // svol_ratio
  if (span_s >= 10)    m |= (1u << 4);              // vpin
  if (span_s >= 60)    m |= (1u << 5);              // trend60
  if (span_s >= 300)   m |= (1u << 6) | (1u << 7);  // trend300 / rv300
  if (span_s >= 7200)  m |= (1u << 8);              // pm_2h
  if (span_s >= 43200) m |= (1u << 9);              // pm_12h
  return m;
}

// 写该 LID 的最新一行到段(µs→ns)。pdiff 为该行瞬时基差(NaN=无 bn as-of),finite 则置 valid bit11。
inline void write_latest(FactorBoard* out, int lid, const tick_feat::Features& fe,
                         double pdiff, std::int64_t first_ts) {
  const std::size_t i = fe.rows() - 1;
  const std::int64_t span_s = (fe.ts_us[i] - first_ts) / 1'000'000;
  std::uint16_t mask = valid_mask_of(span_s);
  if (std::isfinite(pdiff)) mask |= (1u << 11);   // pdiff→bit11
  const double f10[kNumFactors] = {fe.f0[i], fe.f1[i], fe.f2[i], fe.f3[i], fe.f4[i],
                                   fe.f5[i], fe.f6[i], fe.f7[i], fe.f8[i], fe.f9[i]};
  factor_write(out->slot[lid], fe.ts_us[i] * 1000, static_cast<std::uint16_t>(lid),
               mask, f10, fe.mid_price[i], pdiff);
}

// 各引擎有新结算秒 → 写段;逐新秒查「每币每秒必更新」,相邻秒差 !=1s 即漏秒(WARN)。返回本拍写出行数。
inline std::size_t emit_settled(EngineSet& eng, FactorBoard* out, EmitState& es) {
  std::size_t emitted = 0;
  for (int lid = 0; lid < gconf::sym::N_SYMS; ++lid) {
    const auto& fe = eng[lid].features();
    const auto& pd = eng[lid].pdiff_series();   // 瞬时 pdiff 旁路, 与 fe 逐行对齐
    const std::size_t r = fe.rows();
    if (r <= es.last_rows[lid]) continue;
    if (es.last_rows[lid] == 0) {
      es.first_row_ts[lid] = fe.ts_us.front();
      LOG_DEBUG(g_log, "首因子 lid={} ts_us={}", lid, fe.ts_us.front());
    }
    for (std::size_t k = es.last_rows[lid]; k < r; ++k) {   // 监控:每币每秒 10 因子必更新一次
      if (es.last_emit_ts[lid] != 0) {
        const std::int64_t d = fe.ts_us[k] - es.last_emit_ts[lid];
        if (d != 1'000'000) {
          ++es.gap_events;
          LOG_WARNING(g_log, "漏秒 lid={} {}→{}(缺{}秒, 该币未每秒更新)",
                      lid, es.last_emit_ts[lid], fe.ts_us[k], d / 1'000'000 - 1);
        }
      }
      es.last_emit_ts[lid] = fe.ts_us[k];
    }
    emitted += r - es.last_rows[lid];
    es.last_rows[lid] = r;
    write_latest(out, lid, fe, pd[r - 1], es.first_row_ts[lid]);
  }
  return emitted;
}

}  // namespace tflive
