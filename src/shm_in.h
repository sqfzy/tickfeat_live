#pragma once
// shm_in.h — 连上游 gconf shm 段(只读)+ 建输出段。Inputs 是三段指针的 POD 束。
//
// 上游是「生产网关同款 latest-wins 段」:OKX/BN 多档 book = DepthBoard,OKX 成交 = TradeRing。
// 轮询消费(见 feed.h)—— 这是生产消费模型,host 忠实照做。

#include <cstddef>
#include <cstdio>
#include <cstring>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <spdlog/spdlog.h>
#include <gconf/shm/v2/depth_board.h>
#include <gconf/shm/v2/trade.h>

#include "config.h"
#include "factor_board.h"

namespace tflive {

namespace v2 = gconf::shm::v2;

// mmap 一个 shm 段;create=false 只读连既有,true 建段(O_TRUNC 清零)。失败返 nullptr(已打日志)。
template <class Board>
[[nodiscard]] Board* map_segment(const char* name, bool create) {
  const int flags = create ? (O_CREAT | O_RDWR | O_TRUNC) : O_RDONLY;
  const int fd = ::shm_open(name, flags, 0660);
  if (fd < 0) { spdlog::error("shm_open {} 失败: {}", name, std::strerror(errno)); return nullptr; }
  if (create && ::ftruncate(fd, static_cast<off_t>(sizeof(Board))) != 0) {
    spdlog::error("ftruncate {} 失败", name); ::close(fd); return nullptr;
  }
  const int prot = create ? (PROT_READ | PROT_WRITE) : PROT_READ;
  void* base = ::mmap(nullptr, sizeof(Board), prot, MAP_SHARED, fd, 0);
  ::close(fd);
  if (base == MAP_FAILED) { spdlog::error("mmap {} 失败", name); return nullptr; }
  return reinterpret_cast<Board*>(base);
}

// 上游三段(只读)。
struct Inputs {
  const v2::DepthBoard* okx_ob = nullptr;   // OKX 多档 book
  v2::TradeRing*        okx_tr = nullptr;    // OKX 成交(drain 需非 const)
  const v2::DepthBoard* bn_ob  = nullptr;    // BN 多档 book(取 L0)
};

// 连三段;任一失败返 false(启动期错误,main 转非零退出)。
[[nodiscard]] inline bool attach_inputs(const HostConfig& cfg, Inputs& in) {
  in.okx_ob = map_segment<const v2::DepthBoard>(cfg.okx_depth.c_str(), false);
  in.okx_tr = map_segment<v2::TradeRing>(cfg.okx_trade.c_str(), false);
  in.bn_ob  = map_segment<const v2::DepthBoard>(cfg.bn_depth.c_str(), false);
  if (!in.okx_ob || !in.okx_tr || !in.bn_ob) return false;
  spdlog::info("attach: okx_ob={} okx_tr={} bn_ob={}", cfg.okx_depth, cfg.okx_trade, cfg.bn_depth);
  return true;
}

// 建输出 FactorBoard 段(create,清零 + 写段头)。失败返 nullptr。
[[nodiscard]] inline FactorBoard* create_output(const HostConfig& cfg) {
  ::shm_unlink(cfg.out_seg.c_str());
  FactorBoard* out = map_segment<FactorBoard>(cfg.out_seg.c_str(), true);
  if (!out) return nullptr;
  v2::seg_init(out->hdr, v2::SegKind::Board, sizeof(FactorSlot), gconf::sym::N_SYMS, kFactorSchemaHash, 0);
  spdlog::info("output: {} (FactorBoard, {} 槽)", cfg.out_seg, static_cast<int>(gconf::sym::N_SYMS));
  return out;
}

}  // namespace tflive
