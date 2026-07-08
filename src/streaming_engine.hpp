// streaming_engine.hpp — compute_day 的「事件流增量执行」形式
//
// 把离线批处理改写成: 事件到达 → 当前秒桶累积 → 跨秒结算一行 f0-f9。
// 滚动窗口用 cumsum 增量 append(非 deque running-sum), 与离线 np.cumsum 同序;
// 桶内成交用 Kahan(同 pandas); as-of 复用 prev_index —— 三者保证与离线逐位一致。
//
// ⚠️ 三个口径在流式下的落地:
//  1. 只对「有 OB 的秒」产行(同离线 grid=bucket_last_mid 的秒); 纯成交秒(无 OB)整秒丢弃、其成交也丢
//     (同离线 grid_mid.merge(grp, how='left') 丢掉 trade-only 秒)。
//  2. mid=桶末 OB; imb/spread=ts≤秒起点的最后 OB(boundary_ob, 不同快照):
//     进入新秒时 boundary=latest_ob(ts<新秒); 桶内遇 ts==秒起点 的 OB 再刷新(匹配离线 prev_index side=right)。
//  3. 多源 as-of: 跨秒先结算 BN 桶(append bn 桶末 mid)再 OKX 桶, 保证 okx 秒 s 的 pdiff 用到 bn 秒 s。
#pragma once

#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

#include "event.hpp"
#include "tick_feat.hpp"

namespace tick_feat::live {

class StreamingFeatureEngine {
public:
    StreamingFeatureEngine() = default;

    // OKX DepthBoard(5 档): 只供 imb5 的 as-of 盘口(mid/imb1/spread 已改用 booktick, 见 feed_okx_booktick)。
    void feed_okx_ob(const ObEvent& ev) {
        advance_to(ev.ts);
        latest_ob_ = ev; have_latest_ob_ = true;
        ob_cur_.push_back(ev);   // 源 raw:留存本秒消费的 OB 约简事件(imb5 用)
        if (ev.ts == cur_sec_) { boundary_ob_ = ev; have_boundary_ob_ = true; }  // ts≤秒起点 含 ts==秒起点
    }

    // OKX booktick(BBO): mid(桶末)/imb1/spread(as-of) 的源。行由它驱动(has_bt_this_sec_)。
    void feed_okx_booktick(const OkxBookTickEvent& ev) {
        advance_to(ev.ts);
        latest_bt_ = ev; have_latest_bt_ = true;
        bt_bucket_last_mid_ = ob_mid_scaled(ev.bid_px0, ev.ask_px0);   // 桶末 mid
        bt_bucket_last_bid_ = ev.bid_px0; bt_bucket_last_ask_ = ev.ask_px0;  // 桶末整数价(factor_calc_v2 int 口径 pdiff_v2 用)
        has_bt_this_sec_ = true;
        bt_cur_.push_back(ev);   // 源 raw
        if (ev.ts == cur_sec_) { boundary_bt_ = ev; have_boundary_bt_ = true; }  // 秒首 as-of
    }

    void feed_okx_trade(const TradeEvent& ev) {
        advance_to(ev.ts);
        const double usd  = ev.amount * static_cast<double>(ev.price_scaled) / PX_SCALE;
        const double sign = (ev.side == 0) ? 1.0 : -1.0;
        kahan_add(sv_, sv_comp_, sign * usd);
        kahan_add(av_, av_comp_, usd);
        tr_cur_.push_back(ev);   // 源 raw:留存本秒成交事件
    }

    void feed_bn_mid(const BnMidEvent& ev) {
        advance_to(ev.ts);
        bn_bucket_last_mid_  = ob_mid_scaled(ev.bid_px0, ev.ask_px0);
        bn_bucket_last_bid_ = ev.bid_px0; bn_bucket_last_ask_ = ev.ask_px0;  // 桶末整数价(pdiff_v2 as-of 用)
        bn_has_mid_this_sec_ = true;
        bn_cur_.push_back(ev);   // 源 raw:留存本秒 BN 事件
    }

    void finish() {                                  // 结算最后未闭合的秒
        if (started_) { settle_bn_sec(); settle_okx_sec(); }
    }

    const Features& features() const { return out_; }

    // 瞬时 pdiff 侧向量(与 out_ 逐行对齐; 无 bn as-of 的秒=NaN)。只读附加量, 不进 Features,
    // 不改 f0-f9+mid 这 11 列参照 —— 离线 bit-exact 对拍只比那 11 列, 不受影响。
    const std::vector<double>& pdiff_series() const { return pdiff_; }

    // factor_calc_v2 口径旁路(与 out_ 逐行对齐): pdiff_v2=(bn-okx)/okx×1e9(桶末); mean_v2=其 12h 均值(≥3h valid, 否则 0)。
    const std::vector<double>& pdiff_v2_series() const { return pdiff_v2_; }
    const std::vector<double>& mean_v2_series()  const { return mean_v2_; }

    // 源 raw 旁路(与 out_ 逐行对齐):每行=该秒引擎实际消费的原始事件(OB 约简快照/成交/BN)。
    // 逐字留存, 喂回引擎即逐位复现该行因子 —— 取代 id lo/hi 摘要。emit dump 后置空释放(非 const)。
    struct RawBucket { std::vector<OkxBookTickEvent> bt; std::vector<ObEvent> ob; std::vector<TradeEvent> tr; std::vector<BnMidEvent> bn; };
    std::vector<RawBucket>& raw_series() { return raw_; }

private:
    // watermark 推进: 跨秒时先结算 cur_sec(先 bn 后 okx), 再开启新秒。
    void advance_to(int64_t ts) {
        const int64_t new_sec = to_bucket(ts);
        if (new_sec == cur_sec_) return;             // 同秒, 继续累积
        if (started_) { settle_bn_sec(); settle_okx_sec(); }
        cur_sec_ = new_sec; started_ = true;
        has_bt_this_sec_ = false;
        sv_ = sv_comp_ = av_ = av_comp_ = 0.0;
        boundary_ob_ = latest_ob_; have_boundary_ob_ = have_latest_ob_;  // imb5 as-of=进入新秒时最新 depth
        boundary_bt_ = latest_bt_; have_boundary_bt_ = have_latest_bt_;  // mid/imb1/spread as-of=最新 booktick
        bn_has_mid_this_sec_ = false;
        // 不清事件缓冲: 结算秒由 settle_okx_sec 的 std::move 清空; 未结算秒(无 booktick)的 ob/bn
        // carry forward, 累积进下一结算行的 raw —— 否则 imb5(ob as-of)/pdiff(bn as-of)的 as-of 事件
        // 落在未结算秒时会丢, 自复算重建不出。离线按各事件自身 ts 重新分桶, 不会跨秒串入。
    }

    // 更新 [lo,hi] 区间(空时 lo=MAX/hi=MIN, 首值即两端);纯附加, 与数值无关。
    static void span_min_max(std::int64_t& lo, std::int64_t& hi, std::int64_t v) {
        if (v < lo) lo = v;
        if (v > hi) hi = v;
    }
    void settle_bn_sec() {
        if (!bn_has_mid_this_sec_) return;
        bn_ts_.push_back(cur_sec_);
        bn_mid_.push_back(bn_bucket_last_mid_);
        bn_bid_.push_back(bn_bucket_last_bid_); bn_ask_.push_back(bn_bucket_last_ask_);  // as-of int mid 用
    }

    void settle_okx_sec() {
        if (!has_bt_this_sec_) return;               // 行由 OKX booktick 驱动(mid 来源)
        const int64_t q   = cur_sec_;
        const double  mid = bt_bucket_last_mid_;      // 桶末 mid ← booktick BBO

        double imb1 = 0.0, imb5 = 0.0, spread = 0.0;
        if (have_boundary_bt_) {                      // imb1/spread ← booktick as-of BBO(L0 量/价)
            const ImbSpread is = imb_spread_from_levels(
                static_cast<double>(boundary_bt_.bid_px0) / PX_SCALE,
                static_cast<double>(boundary_bt_.ask_px0) / PX_SCALE,
                boundary_bt_.bid_sz0 / PX_SCALE, boundary_bt_.ask_sz0 / PX_SCALE,
                0.0, 0.0);                            // 5-sum 不用(imb5 另算)
            imb1 = is.imb1; spread = is.spread;
        }
        if (have_boundary_ob_) {                      // imb5 ← depth as-of(5 档和), 口径同 imb_spread_from_levels
            double bsz5 = 0.0, asz5 = 0.0;
            for (int l = 0; l < 5; ++l) {
                bsz5 += boundary_ob_.bid_sz[l] / PX_SCALE;
                asz5 += boundary_ob_.ask_sz[l] / PX_SCALE;
            }
            const double d5 = bsz5 + asz5;
            if (d5 > 0.0) imb5 = (bsz5 - asz5) / d5;
        }

        // log return(1s 网格): log(mid/prev_mid); 首秒/prev≤0 → 0
        const double lr = (have_prev_mid_ && prev_mid_ > 0.0) ? std::log(mid / prev_mid_) : 0.0;

        // append 网格 + cumsum 增量(顺序累加, 同 np.cumsum)
        ts_sec_.push_back(q); mid_.push_back(mid);
        imb1_.push_back(imb1); imb5_.push_back(imb5); spread_.push_back(spread);
        cs_svol_.push_back((cs_svol_.empty() ? 0.0 : cs_svol_.back()) + sv_);
        cs_avol_.push_back((cs_avol_.empty() ? 0.0 : cs_avol_.back()) + av_);
        cs_lr2_.push_back((cs_lr2_.empty() ? 0.0 : cs_lr2_.back()) + lr * lr);

        // pdiff: bn 桶末 mid as-of(ts≤q 的最后 bn 秒)
        double pdiff = 0.0, pcnt = 0.0; std::int64_t pdiff_v2 = 0;
        const std::int64_t bi = prev_index(bn_ts_, q);
        if (bi >= 0) {
            const double bn_m = bn_mid_[static_cast<std::size_t>(bi)];
            if (bn_m > 0.0 && std::isfinite(mid) && mid > 0.0) {
                pdiff = (mid - bn_m) / bn_m * 1e4; pcnt = 1.0;
                // factor_calc_v2 整数口径 verbatim: mid=(bid+ask)/2(int); pdiff=(bn_mid-okx_mid)*1e9/okx_mid(int64 向零截断)
                const std::int64_t okx_mid_i = (bt_bucket_last_bid_ + bt_bucket_last_ask_) / 2;
                const std::int64_t bn_mid_i  = (bn_bid_[static_cast<std::size_t>(bi)] + bn_ask_[static_cast<std::size_t>(bi)]) / 2;
                if (okx_mid_i > 0 && bn_mid_i > 0)
                    pdiff_v2 = (bn_mid_i - okx_mid_i) * 1000000000LL / okx_mid_i;
            }
        }
        cs_pdiff_.push_back((cs_pdiff_.empty() ? 0.0 : cs_pdiff_.back()) + pdiff);
        cs_pcnt_.push_back((cs_pcnt_.empty() ? 0.0 : cs_pcnt_.back()) + pcnt);
        // cs_pdiff_v2_ 存整数 pdiff_v2 的累加(整数 <2^53 → double 精确, rolling 差仍精确整数)
        cs_pdiff_v2_.push_back((cs_pdiff_v2_.empty() ? 0.0 : cs_pdiff_v2_.back()) + static_cast<double>(pdiff_v2));
        pdiff_.push_back(pcnt > 0.0 ? pdiff : std::numeric_limits<double>::quiet_NaN());  // 瞬时值旁路(cs_pdiff_ 仍 +0 保 pm 不变)
        pdiff_v2_.push_back(pcnt > 0.0 ? static_cast<double>(pdiff_v2) : std::numeric_limits<double>::quiet_NaN());

        // 源 raw:留存本秒消费的原始事件(has_ob 恒真→ob 非空;bn/trade 该秒无则空)。move 走, reset 清缓冲。
        raw_.push_back(RawBucket{std::move(bt_cur_), std::move(ob_cur_), std::move(tr_cur_), std::move(bn_cur_)});

        prev_mid_ = mid; have_prev_mid_ = true;

        emit_features(q, mid, imb1, imb5, spread);
    }

    // 逐秒 f0-f9(与 compute_day 逐秒部分逐字一致: 同 rolling_sum_asof / grid_value_asof)
    void emit_features(int64_t q, double mid_now, double imb1, double imb5, double spread) {
        const double svol60  = rolling_sum_asof(cs_svol_, ts_sec_, q, 60);
        const double avol60  = rolling_sum_asof(cs_avol_, ts_sec_, q, 60);
        const double svol10  = rolling_sum_asof(cs_svol_, ts_sec_, q, 10);
        const double avol10  = rolling_sum_asof(cs_avol_, ts_sec_, q, 10);
        const double lr2_300 = rolling_sum_asof(cs_lr2_, ts_sec_, q, 300);
        const double mid_60  = grid_value_asof(ts_sec_, mid_, q - 60  * GRID_US);
        const double mid_300 = grid_value_asof(ts_sec_, mid_, q - 300 * GRID_US);

        const double svol_ratio = (avol60 > 1e-9) ? svol60 / avol60 : 0.0;
        const double vpin       = (avol10 > 1e-9) ? std::abs(svol10) / avol10 : -1.0;
        const double trend60    = (mid_now > 0) ? (mid_now - mid_60)  / mid_now * 1e4 : 0.0;
        const double trend300   = (mid_now > 0) ? (mid_now - mid_300) / mid_now * 1e4 : 0.0;
        const double rv_300s    = std::sqrt(std::max(lr2_300, 0.0)) * 1e4;

        const double pm_2h_s  = rolling_sum_asof(cs_pdiff_, ts_sec_, q, 7200);
        const double pm_2h_n  = rolling_sum_asof(cs_pcnt_,  ts_sec_, q, 7200);
        const double pm_2h    = (pm_2h_n  > 0) ? pm_2h_s  / pm_2h_n  : 0.0;
        const double pm_12h_s = rolling_sum_asof(cs_pdiff_, ts_sec_, q, 43200);
        const double pm_12h_n = rolling_sum_asof(cs_pcnt_,  ts_sec_, q, 43200);
        const double pm_12h   = (pm_12h_n > 0) ? pm_12h_s / pm_12h_n : 0.0;

        // factor_calc_v2 口径 mean: pdiff_v2 的 12h 均值; ≥3h(10800s)有效样本才出, 否则 0(同 v2 mean_min_s)。
        // v2 TimeRing.avg verbatim: avg_out=(int64)(sum/count); mv2_s 是整数 pdiff_v2 的精确和。
        const double mv2_s   = rolling_sum_asof(cs_pdiff_v2_, ts_sec_, q, 43200);
        const double mean_v2 = (pm_12h_n >= 10800.0 && pm_12h_n > 0.0)
                                 ? static_cast<double>(static_cast<std::int64_t>(mv2_s / pm_12h_n)) : 0.0;
        mean_v2_.push_back(mean_v2);

        out_.ts_us.push_back(q);
        out_.f0.push_back(imb1);  out_.f1.push_back(imb5);  out_.f2.push_back(spread);
        out_.f3.push_back(svol_ratio); out_.f4.push_back(vpin);
        out_.f5.push_back(trend60); out_.f6.push_back(trend300); out_.f7.push_back(rv_300s);
        out_.f8.push_back(pm_2h); out_.f9.push_back(pm_12h);
        out_.mid_price.push_back(mid_now);
    }

    // ── OKX 当前秒桶 ──
    int64_t cur_sec_         = std::numeric_limits<int64_t>::min();
    bool    started_         = false;
    double  sv_ = 0.0, sv_comp_ = 0.0, av_ = 0.0, av_comp_ = 0.0;
    // OKX booktick(BBO): 驱动行 + mid(桶末)/imb1/spread(as-of) 的源
    bool    has_bt_this_sec_    = false;
    double  bt_bucket_last_mid_ = 0.0;
    std::int64_t bt_bucket_last_bid_ = 0, bt_bucket_last_ask_ = 0;   // 桶末整数价(factor_calc_v2 int pdiff_v2)
    bool    have_latest_bt_   = false; OkxBookTickEvent latest_bt_{};
    bool    have_boundary_bt_ = false; OkxBookTickEvent boundary_bt_{};
    // OKX DepthBoard(5 档): 只供 imb5 的 as-of
    bool    have_latest_ob_   = false; ObEvent latest_ob_{};
    bool    have_boundary_ob_ = false; ObEvent boundary_ob_{};

    // ── OKX 历史(append-only) ──
    std::vector<int64_t> ts_sec_;
    std::vector<double>  mid_, imb1_, imb5_, spread_;
    std::vector<double>  cs_svol_, cs_avol_, cs_lr2_;
    double prev_mid_      = 0.0;
    bool   have_prev_mid_ = false;

    // ── BN 当前桶 + 历史 ──
    bool    bn_has_mid_this_sec_ = false;
    double  bn_bucket_last_mid_  = 0.0;
    std::int64_t bn_bucket_last_bid_ = 0, bn_bucket_last_ask_ = 0;   // 桶末整数价(pdiff_v2 as-of)
    std::vector<int64_t> bn_ts_;
    std::vector<double>  bn_mid_;
    std::vector<int64_t> bn_bid_, bn_ask_;   // 与 bn_ts_/bn_mid_ 逐秒对齐, as-of int mid 用

    // ── pdiff 历史(与 okx 网格对齐) ──
    std::vector<double> cs_pdiff_, cs_pcnt_;
    std::vector<double> pdiff_;   // 瞬时 pdiff 旁路(无 bn as-of=NaN); 供实时消费者读, 不进 Features
    // ── factor_calc_v2 口径旁路: pdiff_v2=(bn-okx)/okx×1e9(桶末); mean_v2=其 12h 均值(≥3h valid) ──
    std::vector<double> cs_pdiff_v2_;
    std::vector<double> pdiff_v2_, mean_v2_;   // 与 out_ 逐行对齐; 不进 f0-f9

    // ── 源 raw:当前秒事件缓冲(reset 每秒清 / settle move 走)+ 逐行历史(emit dump 后置空释放) ──
    std::vector<OkxBookTickEvent> bt_cur_;
    std::vector<ObEvent>    ob_cur_;
    std::vector<TradeEvent> tr_cur_;
    std::vector<BnMidEvent> bn_cur_;
    std::vector<RawBucket>  raw_;    // 与 out_ 逐行对齐

    Features out_;
};

} // namespace tick_feat::live
