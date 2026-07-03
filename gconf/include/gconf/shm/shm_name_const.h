#pragma once

namespace gconf::shm {

// 交易规范
inline constexpr const char *SHM_OKX_SWAP_SYMINFO = "/shm_okx_swap_syminfo";
inline constexpr const char *SHM_OKX_SPOT_SYMINFO = "/shm_okx_spot_syminfo";
inline constexpr const char *SHM_BN_SWAP_SYMINFO = "/shm_bn_swap_syminfo";
inline constexpr const char *SHM_BN_SPOT_SYMINFO = "/shm_bn_spot_syminfo";

// BOOK_TICK
inline constexpr const char *SHM_BN_SWAP_BOOK_TICK = "/shm_bn_swap_book_tick";
inline constexpr const char *SHM_BN_SPOT_BOOK_TICK = "/shm_bn_spot_book_tick";
inline constexpr const char *SHM_OKX_SWAP_BOOK_TICK = "/shm_okx_swap_book_tick";
inline constexpr const char *SHM_OKX_SPOT_BOOK_TICK = "/shm_okx_spot_book_tick";
// bybit book_tick 段名统一到 v2/shm_names.h::BYBIT_LIN_BOOK_TICK ("/shm_bybit_lin_book_tick_v2", 与 bn/okx v2 对齐)。
// 原遗留 SHM_BYBIT_LIN_BOOK_TICK("/shm_bybit_lin_book_tick" 无 _v2) 已删 — 零引用且与 app 实际段名冲突。

inline constexpr const char *SHM_BN_SWAP_PRIVATE_EVENT_RING = "/shm_bn_swap_private_event_ring";
inline constexpr const char *SHM_BN_SWAP_ORDER_EVENT_RING = "/shm_bn_swap_order_event_ring";
inline constexpr const char *SHM_BN_SPOT_PRIVATE_EVENT_RING = "/shm_bn_spot_private_event_ring";
inline constexpr const char *SHM_BN_SPOT_ORDER_EVENT_RING = "/shm_bn_spot_order_event_ring";

inline constexpr const char *SHM_OKX_SWAP_PRIVATE_EVENT_RING = "/shm_okx_swap_private_event_ring";
inline constexpr const char *SHM_OKX_SPOT_PRIVATE_EVENT_RING = "/shm_okx_spot_private_event_ring";
inline constexpr const char *SHM_OKX_SWAP_ORDER_EVENT_RING = "/shm_okx_swap_order_event_ring";
inline constexpr const char *SHM_OKX_SPOT_ORDER_EVENT_RING = "/shm_okx_spot_order_event_ring";

// Bybit V5 Linear
inline constexpr const char *SHM_BYBIT_LIN_SYMINFO = "/shm_bybit_lin_syminfo";
inline constexpr const char *SHM_BYBIT_LIN_PRIVATE_EVENT_RING = "/shm_bybit_lin_private_event_ring";
inline constexpr const char *SHM_BYBIT_LIN_ORDER_EVENT_RING = "/shm_bybit_lin_order_event_ring";

inline constexpr const char *SHM_PRIVATE_REQ_RING = "/shm_private_req_ring";
} // namespace gconf::shm
