#pragma once
// gconf/shm/v2/names.h — v2 SHM 段名(bybit-linear set)。

namespace gconf::shm::v2 {

// 交易规范
inline constexpr const char *OKX_SWAP_SYMINFO = "/shm_okx_swap_syminfo_v2";
inline constexpr const char *OKX_SPOT_SYMINFO = "/shm_okx_spot_syminfo_v2";
inline constexpr const char *BN_SWAP_SYMINFO = "/shm_bn_swap_syminfo_v2";
inline constexpr const char *BN_SPOT_SYMINFO = "/shm_bn_spot_syminfo_v2";

// BOOK_TICK
inline constexpr const char *BN_SWAP_BOOK_TICK = "/shm_bn_swap_book_tick_v2";
inline constexpr const char *BN_SPOT_BOOK_TICK = "/shm_bn_spot_book_tick_v2";
inline constexpr const char *OKX_SWAP_BOOK_TICK = "/shm_okx_swap_book_tick_v2";
inline constexpr const char *OKX_SPOT_BOOK_TICK = "/shm_okx_spot_book_tick_v2";
inline constexpr const char *BYBIT_LIN_BOOK_TICK = "/shm_bybit_lin_book_tick_v2";

// DEPTH (多档盘口 DepthBoard, 本地维护订单簿发 top-N)
inline constexpr const char *BN_SWAP_DEPTH = "/shm_bn_swap_depth_v2";
inline constexpr const char *BN_SPOT_DEPTH = "/shm_bn_spot_depth_v2";
inline constexpr const char *OKX_SWAP_DEPTH = "/shm_okx_swap_depth_v2";
inline constexpr const char *OKX_SPOT_DEPTH = "/shm_okx_spot_depth_v2";

// TRADE (逐笔成交广播环 TradeRing = BcastRing<TradeEntry>)
inline constexpr const char *BN_SWAP_TRADE = "/shm_bn_swap_trade_v2";
inline constexpr const char *BN_SPOT_TRADE = "/shm_bn_spot_trade_v2";
inline constexpr const char *OKX_SWAP_TRADE = "/shm_okx_swap_trade_v2";
inline constexpr const char *OKX_SPOT_TRADE = "/shm_okx_spot_trade_v2";

inline constexpr const char *BN_SWAP_PRIVATE_EVENT_RING = "/shm_bn_swap_private_event_ring_v2";
inline constexpr const char *BN_SWAP_ORDER_EVENT_RING = "/shm_bn_swap_order_event_ring_v2";
inline constexpr const char *BN_SPOT_PRIVATE_EVENT_RING = "/shm_bn_spot_private_event_ring_v2";
inline constexpr const char *BN_SPOT_ORDER_EVENT_RING = "/shm_bn_spot_order_event_ring_v2";

inline constexpr const char *OKX_SWAP_PRIVATE_EVENT_RING = "/shm_okx_swap_private_event_ring_v2";
inline constexpr const char *OKX_SPOT_PRIVATE_EVENT_RING = "/shm_okx_spot_private_event_ring_v2";
inline constexpr const char *OKX_SWAP_ORDER_EVENT_RING = "/shm_okx_swap_order_event_ring_v2";
inline constexpr const char *OKX_SPOT_ORDER_EVENT_RING = "/shm_okx_spot_order_event_ring_v2";

// Bybit V5 Linear
inline constexpr const char *BYBIT_LIN_SYMINFO = "/shm_bybit_lin_syminfo_v2";
inline constexpr const char *BYBIT_LIN_PRIVATE_EVENT_RING = "/shm_bybit_lin_private_event_ring_v2";
inline constexpr const char *BYBIT_LIN_ORDER_EVENT_RING = "/shm_bybit_lin_order_event_ring_v2";

inline constexpr const char *PRIVATE_REQ_RING = "/shm_private_req_ring_v2";

// FACTOR (下游因子板 FactorBoard = tickfeat_live 的 f0-f9 + mid + pdiff, per-LID)
inline constexpr const char *F0F9_FACTOR = "/shm_f0f9_v2";
} // namespace gconf::shm::v2
