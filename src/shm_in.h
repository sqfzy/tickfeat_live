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

// 段头契约校验:逐字段比对本进程编译期常量。生产者(jt_dev3)独立编译,book25/trade 契约一旦漂移
// 必须在 attach 期响亮失败, 而非静默算出错因子。magic/version/kind/abi 不符=布局根本对不上→致命(返 false);
// schema_hash 漂移=尺寸都对只是字段描述串变→gconf 定为演进期可容忍,告警后继续(见 seg_header.h)。
[[nodiscard]] inline bool check_segment(const char* name, const v2::SegHeader& hdr, v2::SegKind kind,
                                        std::uint32_t entry_size, std::uint32_t capacity,
                                        std::uint64_t schema_hash) {
  const v2::SegError e = v2::seg_check(hdr, kind, entry_size, capacity, schema_hash);
  if (e == v2::SegError::Ok) return true;
  if (e == v2::SegError::SchemaDrift) {
    spdlog::warn("段 {} schema 漂移: {}(本端 hash={:#x}, 段内 hash={:#x})—— 演进期容忍, 继续但因子可能偏",
                 name, v2::seg_error_str(e), schema_hash, hdr.schema_hash);
    return true;
  }
  spdlog::error("段 {} 契约不符: {}(kind 期望={} 段内={}; entry_size 期望={} 段内={}; capacity 期望={} 段内={})",
                name, v2::seg_error_str(e), static_cast<int>(kind), static_cast<int>(hdr.kind),
                entry_size, hdr.entry_size, capacity, hdr.capacity);
  return false;
}

// 连三段;任一失败返 false(启动期错误,main 转非零退出)。mmap 成功后逐段跑 seg_check 卡契约。
[[nodiscard]] inline bool attach_inputs(const HostConfig& cfg, Inputs& in) {
  in.okx_ob = map_segment<const v2::DepthBoard>(cfg.okx_depth.c_str(), false);
  in.okx_tr = map_segment<v2::TradeRing>(cfg.okx_trade.c_str(), false);
  in.bn_ob  = map_segment<const v2::DepthBoard>(cfg.bn_depth.c_str(), false);
  if (!in.okx_ob || !in.okx_tr || !in.bn_ob) return false;
  const bool ok =
      check_segment(cfg.okx_depth.c_str(), in.okx_ob->hdr, v2::SegKind::Board,
                    sizeof(v2::DepthBoardSlot), gconf::sym::N_SYMS, v2::kDepthBoardSchemaHash) &&
      check_segment(cfg.okx_trade.c_str(), in.okx_tr->hdr, v2::SegKind::BcastRing,
                    sizeof(v2::TradeEntry), v2::TRADE_RING_CAP, v2::kTradeSchemaHash) &&
      check_segment(cfg.bn_depth.c_str(), in.bn_ob->hdr, v2::SegKind::Board,
                    sizeof(v2::DepthBoardSlot), gconf::sym::N_SYMS, v2::kDepthBoardSchemaHash);
  if (!ok) return false;
  spdlog::info("attach: okx_ob={} okx_tr={} bn_ob={} (契约校验通过)", cfg.okx_depth, cfg.okx_trade, cfg.bn_depth);
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
