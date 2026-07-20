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

    // OKX DepthBoard(5 档): mid(桶末)/imb1/imb5/spread(as-of) 的源。行由它驱动(has_ob_this_sec_)。
    void feed_okx_ob(const ObEvent& ev) {
        advance_to(ev.ts);
        latest_ob_ = ev; have_latest_ob_ = true;
        bucket_last_mid_ = ob_mid_scaled(ev.bid_px0, ev.ask_px0);   // 桶末 mid
        ob_bucket_last_bid_ = ev.bid_px0; ob_bucket_last_ask_ = ev.ask_px0;  // 桶末整数价(factor_calc_v2 int 口径 pdiff_v2 用)
        has_ob_this_sec_ = true;
        ob_cur_.push_back(ev);   // 源 raw:留存本秒消费的 OB 约简事件(喂回引擎即复现)
        if (ev.ts == cur_sec_) { boundary_ob_ = ev; have_boundary_ob_ = true; }  // ts≤秒起点 含 ts==秒起点
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
        if (started_) { settle_bn_sec(); settle_okx_sec(); finalize_minute(); }  // 补最后未闭合分钟(rv_60m)
    }

    const Features& features() const { return out_; }

    // 瞬时 pdiff 侧向量(与 out_ 逐行对齐; 无 bn as-of 的秒=NaN)。只读附加量, 不进 Features,
    // 不改 f0-f9+mid 这 11 列参照 —— 离线 bit-exact 对拍只比那 11 列, 不受影响。
    const std::vector<double>& pdiff_series() const { return pdiff_; }

    // factor_calc_v2 口径旁路(与 out_ 逐行对齐): pdiff_v2=(bn-okx)/okx×1e9(桶末); mean_v2=其 12h 均值(≥3h valid, 否则 0)。
    const std::vector<double>& pdiff_v2_series() const { return pdiff_v2_; }
    const std::vector<double>& mean_v2_series()  const { return mean_v2_; }

    // CAP/volgate 旁路(与 out_ 逐行对齐):
    //  trade_notional_ = 最近 300s Σ|amount|·price(未乘 ctVal; = cs_avol_ 的 300s 窗, 同 f3/f4 口径)。
    //  rv60m_          = 分钟均价收益 60min 滚动样本 std×√1440; 暖机<61 个完整分钟=NaN(fail-closed)。
    const std::vector<double>& trade_notional_series() const { return trade_notional_; }
    const std::vector<double>& rv60m_series()          const { return rv60m_; }

    // 源 raw 旁路(与 out_ 逐行对齐):每行=该秒引擎实际消费的原始事件(OB 约简快照/成交/BN)。
    // 逐字留存, 喂回引擎即逐位复现该行因子 —— 取代 id lo/hi 摘要。emit dump 后置空释放(非 const)。
    struct RawBucket { std::vector<ObEvent> ob; std::vector<TradeEvent> tr; std::vector<BnMidEvent> bn; };
    std::vector<RawBucket>& raw_series() { return raw_; }

    // 调试: 暴露 cumsum 与秒网格, 供逐值对照离线 np.cumsum(定位 cs_avol 的 1 ulp)。
    const std::vector<double>&  dbg_cs_avol() const { return cs_avol_; }
    const std::vector<double>&  dbg_cs_svol() const { return cs_svol_; }
    const std::vector<int64_t>& dbg_ts_sec()  const { return ts_sec_; }
    std::size_t                 dbg_rebase_count() const { return rebase_count_; }   // cs rebase 触发次数
    const std::vector<double>&  dbg_av_series() const { return av_series_; }         // 每秒精确 av 增量(真值参照用)
    const std::vector<double>&  dbg_sv_series() const { return sv_series_; }

private:
    // watermark 推进: 跨秒时先结算 cur_sec(先 bn 后 okx), 再开启新秒。
    void advance_to(int64_t ts) {
        const int64_t new_sec = to_bucket(ts);
        if (new_sec == cur_sec_) return;             // 同秒, 继续累积
        if (started_) { settle_bn_sec(); settle_okx_sec(); }
        cur_sec_ = new_sec; started_ = true;
        has_ob_this_sec_ = false;
        sv_ = sv_comp_ = av_ = av_comp_ = 0.0;
        boundary_ob_ = latest_ob_; have_boundary_ob_ = have_latest_ob_;  // 秒首 as-of=进入新秒时最新 depth
        bn_has_mid_this_sec_ = false;
        // 不清事件缓冲: 结算秒由 settle_okx_sec 的 std::move 清空; 未结算秒(无 OB)的 tr/bn
        // carry forward, 累积进下一结算行的 raw —— 否则 pdiff(bn as-of)的 as-of 事件
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
        if (!has_ob_this_sec_) return;               // 只对有 OB 的秒产行
        const int64_t q   = cur_sec_;
        const double  mid = bucket_last_mid_;         // 桶末 mid

        // imb/spread: 秒首 as-of(boundary_ob); 无则 0(同离线 prev_index<0)
        double imb1 = 0.0, imb5 = 0.0, spread = 0.0;
        if (have_boundary_ob_) {
            double bsz5 = 0.0, asz5 = 0.0;
            for (int l = 0; l < 5; ++l) {            // 逐项 /PX_SCALE 再累加(同离线 extract_ob_snapshots)
                bsz5 += boundary_ob_.bid_sz[l] / PX_SCALE;
                asz5 += boundary_ob_.ask_sz[l] / PX_SCALE;
            }
            const ImbSpread is = imb_spread_from_levels(
                static_cast<double>(boundary_ob_.bid_px0) / PX_SCALE,
                static_cast<double>(boundary_ob_.ask_px0) / PX_SCALE,
                boundary_ob_.bid_sz[0] / PX_SCALE, boundary_ob_.ask_sz[0] / PX_SCALE,
                bsz5, asz5);
            imb1 = is.imb1; imb5 = is.imb5; spread = is.spread;
        }

        // log return(1s 网格): log(mid/prev_mid); 首秒/prev≤0 → 0
        const double lr = (have_prev_mid_ && prev_mid_ > 0.0) ? std::log(mid / prev_mid_) : 0.0;

        // append 网格 + cumsum 增量(顺序累加, 同 np.cumsum)
        ts_sec_.push_back(q); mid_.push_back(mid);
        imb1_.push_back(imb1); imb5_.push_back(imb5); spread_.push_back(spread);
        cs_svol_.push_back((cs_svol_.empty() ? 0.0 : cs_svol_.back()) + sv_);
        cs_avol_.push_back((cs_avol_.empty() ? 0.0 : cs_avol_.back()) + av_);
        cs_lr2_.push_back((cs_lr2_.empty() ? 0.0 : cs_lr2_.back()) + lr * lr);
        av_series_.push_back(av_); sv_series_.push_back(sv_);   // 每秒精确增量, 供 cs rebase 重累
        maybe_rebase_cs();                                      // 量级超阈则 rebase cs_avol_/cs_svol_(防长跑相减退化)

        // pdiff: bn 桶末 mid as-of(ts≤q 的最后 bn 秒)
        double pdiff = 0.0, pcnt = 0.0; std::int64_t pdiff_v2 = 0;
        const std::int64_t bi = prev_index(bn_ts_, q);
        if (bi >= 0) {
            const double bn_m = bn_mid_[static_cast<std::size_t>(bi)];
            if (bn_m > 0.0 && std::isfinite(mid) && mid > 0.0) {
                pdiff = (mid - bn_m) / bn_m * 1e4; pcnt = 1.0;
                // factor_calc_v2 整数口径 verbatim: mid=(bid+ask)/2(int); pdiff=(bn_mid-okx_mid)*1e9/okx_mid(int64 向零截断)
                const std::int64_t okx_mid_i = (ob_bucket_last_bid_ + ob_bucket_last_ask_) / 2;  // 桶末 OB 整数价
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
        raw_.push_back(RawBucket{std::move(ob_cur_), std::move(tr_cur_), std::move(bn_cur_)});

        prev_mid_ = mid; have_prev_mid_ = true;

        // rv_60m 分钟聚合: 跨分钟先结算上一分钟(finalize_minute), 再把本秒 mid 累进当前分钟均价。
        // 顺序 f64 累加(同内联参照的 Python 顺序求和), mmean = Σmid/cnt。缺失分钟自然跳过(无 settle 秒→无累积)。
        const int64_t m = q / MINUTE_US;
        if (!have_minute_) { cur_minute_id_ = m; minute_sum_ = 0.0; minute_cnt_ = 0; have_minute_ = true; }
        if (m != cur_minute_id_) {                    // 进入新分钟 → 结算上一分钟(可能跨多分钟, 中间空分钟无行)
            finalize_minute();
            cur_minute_id_ = m; minute_sum_ = 0.0; minute_cnt_ = 0;
        }
        minute_sum_ += mid; ++minute_cnt_;

        emit_features(q, mid, imb1, imb5, spread);
    }

    // 结算 cur_minute_id_ 分钟: mmean=Σmid/cnt → 相邻分钟 log return → 60-行样本 std×√1440。
    // returns 序列从第 2 个完整分钟起(首分钟无 diff); rv 需 60 个 return(n≥60), 否则该分钟 NaN。
    // 逐字对齐内联 numpy 参照: var=(Σx²−Σx²/60)/59 (ddof=1), 固定 60-行窗(按行非按墙钟)。
    void finalize_minute() {
        if (!have_minute_ || minute_cnt_ <= 0) return;
        const double mmean = minute_sum_ / static_cast<double>(minute_cnt_);
        if (have_prev_mmean_) {
            const double r = (prev_mmean_ > 0.0 && mmean > 0.0) ? std::log(mmean / prev_mmean_) : 0.0;
            minute_id_grid_.push_back(cur_minute_id_);
            cs_r_.push_back((cs_r_.empty()  ? 0.0 : cs_r_.back())  + r);
            cs_r2_.push_back((cs_r2_.empty() ? 0.0 : cs_r2_.back()) + r * r);
            const std::size_t n = cs_r_.size();       // 已积累的 return 数
            double rv = std::numeric_limits<double>::quiet_NaN();
            if (n >= 60) {                            // 末 60 个 return: 索引 [n-60, n-1]
                const double sx  = cs_r_[n - 1]  - (n >= 61 ? cs_r_[n - 61]  : 0.0);
                const double sxx = cs_r2_[n - 1] - (n >= 61 ? cs_r2_[n - 61] : 0.0);
                const double var = (sxx - sx * sx / 60.0) / 59.0;   // ddof=1
                rv = std::sqrt(std::max(var, 0.0)) * std::sqrt(1440.0);
            }
            rv60_grid_.push_back(rv);
        }
        prev_mmean_ = mmean; have_prev_mmean_ = true;
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
        // v2 逐字复刻其浮点序列: TimeRing.avg 返回 avg=(double)sum/count/1e9, 写出取 (int64)(avg*1e9)。
        // 这个 /1e9 再 *1e9 的往返不可省 —— 偶发 1 ULP 舍入会翻转截断, 略去则与 v2 差 1(1e-9)。
        const double mv2_s   = rolling_sum_asof(cs_pdiff_v2_, ts_sec_, q, 43200);
        const double mean_v2 = (pm_12h_n >= 10800.0 && pm_12h_n > 0.0)
                                 ? static_cast<double>(static_cast<std::int64_t>((mv2_s / pm_12h_n / 1e9) * 1e9)) : 0.0;
        mean_v2_.push_back(mean_v2);

        // V300: cs_avol_ 的 300s 窗(= Σ|amount|·price, 同 f3/f4 abs_vol 口径; 未乘 ctVal)。
        trade_notional_.push_back(rolling_sum_asof(cs_avol_, ts_sec_, q, 300));
        // rv_60m: as-of 到「最近一个已完整结束的分钟」(completed=q 所在分钟−1); 无则 NaN(暖机 fail-closed)。
        const int64_t completed_min = q / MINUTE_US - 1;
        const std::int64_t rmi = prev_index(minute_id_grid_, completed_min);
        rv60m_.push_back(rmi >= 0 ? rv60_grid_[static_cast<std::size_t>(rmi)]
                                  : std::numeric_limits<double>::quiet_NaN());

        out_.ts_us.push_back(q);
        out_.f0.push_back(imb1);  out_.f1.push_back(imb5);  out_.f2.push_back(spread);
        out_.f3.push_back(svol_ratio); out_.f4.push_back(vpin);
        out_.f5.push_back(trend60); out_.f6.push_back(trend300); out_.f7.push_back(rv_300s);
        out_.f8.push_back(pm_2h); out_.f9.push_back(pm_12h);
        out_.mid_price.push_back(mid_now);
    }

    // cs_avol_ 恒正累加, 长跑量级涨到 1e11 → rolling_sum_asof 的 cs[q]-cs[q-60] 大数相减精度退化
    // (f3/f4; 见 dbg_rebase.py 数值复现)。触发式 rebase: 量级超阈值时, 把最近 REBASE_KEEP 个条目的
    // cs 从 0 用【精确增量】重累, 拉回小量级 → 相减恢复精确。只碰 cs_avol_/cs_svol_(仅 f3/f4 用, 窗 60s);
    // 重累是常数平移(cs[i]-=cs[base-1]), 保留窗内所有差值; 老条目(<base)永不被 60s 窗读到, 无副作用。
    // 阈值 2e10 ≫ --verify 2h 窗的 ~1e9 → 短验证不触发, 与批处理 naive cumsum 保持逐位一致;
    // 仅生产连跑 >1 天(cs>2e10)才触发, 与全序列 naive 离线差 ~ulp(2e10)≈3.8e-10(<1e-9, 且无人跑多天 naive 离线)。
    void maybe_rebase_cs() {
        if (cs_avol_.back() <= REBASE_THRESHOLD) return;
        const std::size_t n = cs_avol_.size();
        if (n <= REBASE_KEEP) return;
        const std::size_t base = n - REBASE_KEEP;               // 起点; [base,n) 从 0 重累
        double a = 0.0, s = 0.0;
        for (std::size_t i = base; i < n; ++i) {                // 精确增量顺序累加(同 np.cumsum, 但从近端 0 起 → 量级小)
            a += av_series_[i]; s += sv_series_[i];
            cs_avol_[i] = a; cs_svol_[i] = s;
        }
        ++rebase_count_;
    }

    // ── OKX 当前秒桶 ──
    int64_t cur_sec_         = std::numeric_limits<int64_t>::min();
    bool    started_         = false;
    bool    has_ob_this_sec_ = false;
    double  bucket_last_mid_ = 0.0;
    std::int64_t ob_bucket_last_bid_ = 0, ob_bucket_last_ask_ = 0;   // 桶末整数价(factor_calc_v2 int pdiff_v2)
    double  sv_ = 0.0, sv_comp_ = 0.0, av_ = 0.0, av_comp_ = 0.0;
    bool    have_latest_ob_   = false; ObEvent latest_ob_{};
    bool    have_boundary_ob_ = false; ObEvent boundary_ob_{};

    // ── OKX 历史(append-only) ──
    std::vector<int64_t> ts_sec_;
    std::vector<double>  mid_, imb1_, imb5_, spread_;
    std::vector<double>  cs_svol_, cs_avol_, cs_lr2_;
    // cs rebase(防 cs_avol_/cs_svol_ 长跑相减退化): 存每秒精确增量, 量级超阈时最近 REBASE_KEEP 条从 0 重累。
    std::vector<double>  av_series_, sv_series_;
    std::size_t          rebase_count_ = 0;
    static constexpr double      REBASE_THRESHOLD = 2e10;   // cs_avol 超此值触发(约 1 天量级); ≫ --verify 窗
    static constexpr std::size_t REBASE_KEEP      = 360;    // 重累保留的条目数(>300s 窗 for V300 + 余量; 每条≥1s → ≥360s)
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

    // ── CAP/volgate 旁路(与 out_ 逐行对齐) ──
    std::vector<double> trade_notional_;   // V300 = cs_avol_ 的 300s 窗(未乘 ctVal)
    std::vector<double> rv60m_;            // 分钟均价收益 60min 样本 std×√1440; 暖机=NaN

    // ── rv_60m 分钟聚合层(秒网格之外的独立分钟网格) ──
    static constexpr int64_t MINUTE_US = 60 * GRID_US;   // 1 自然分钟(µs)
    int64_t cur_minute_id_ = 0;            // 当前累积中的分钟 id(= ts/MINUTE_US)
    double  minute_sum_    = 0.0;          // 当前分钟内 mid 顺序累加
    int64_t minute_cnt_    = 0;            // 当前分钟内 settle 秒数
    bool    have_minute_   = false;
    double  prev_mmean_    = 0.0;          // 上一完整分钟均价(算 log return)
    bool    have_prev_mmean_ = false;
    std::vector<int64_t> minute_id_grid_;  // 每个完整分钟(有 return 者)的 minute_id, 单调; as-of 用
    std::vector<double>  cs_r_, cs_r2_;    // 分钟 log return 的 Σr / Σr²(固定 60-行窗样本方差)
    std::vector<double>  rv60_grid_;       // 与 minute_id_grid_ 逐行对齐: 该分钟的 rv_60m(n<60=NaN)

    // ── 源 raw:当前秒事件缓冲(settle move 走, 未结算秒 carry-forward)+ 逐行历史(emit dump 后置空释放) ──
    std::vector<ObEvent>    ob_cur_;
    std::vector<TradeEvent> tr_cur_;
    std::vector<BnMidEvent> bn_cur_;
    std::vector<RawBucket>  raw_;    // 与 out_ 逐行对齐

    Features out_;
};

} // namespace tick_feat::live
