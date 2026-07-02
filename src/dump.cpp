// dump.cpp — 独立进程按 FactorBoard 契约读回某 LID 的 f0-f9(验证下游可消费)。
// 用法:tickfeat_dump <out_seg> <lid>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "factor_board.h"

int main(int argc, char** argv) {
  if (argc < 3) { std::fprintf(stderr, "用法: %s <out_seg> <lid>\n", argv[0]); return 2; }
  const int lid = std::atoi(argv[2]);
  const int fd = ::shm_open(argv[1], O_RDONLY, 0);
  if (fd < 0) { std::fprintf(stderr, "shm_open %s: %s\n", argv[1], std::strerror(errno)); return 1; }
  auto* board = static_cast<const tflive::FactorBoard*>(
      ::mmap(nullptr, sizeof(tflive::FactorBoard), PROT_READ, MAP_SHARED, fd, 0));
  ::close(fd);
  if (board == MAP_FAILED) { std::fprintf(stderr, "mmap 失败\n"); return 1; }

  tflive::FactorSlot s;
  for (int retry = 0; retry < 8; ++retry)
    if (tflive::factor_read(board->slot[lid], s)) break;

  std::printf("lid=%d ts_ns=%lld vmask=0x%03x\n", lid, static_cast<long long>(s.ts_ns), s.valid_mask);
  const char* names[tflive::kNumFactors] = {"f0 imb1", "f1 imb5", "f2 spread", "f3 svol_ratio", "f4 vpin",
                                            "f5 trend60", "f6 trend300", "f7 rv300", "f8 pm_2h", "f9 pm_12h"};
  for (int i = 0; i < tflive::kNumFactors; ++i)
    std::printf("  %-14s = %+.6f   [%s]\n", names[i], s.f[i], (s.valid_mask & (1u << i)) ? "valid" : "warmup");
  std::printf("  mid_price      = %.6f\n", s.mid);
  std::printf("  pdiff(bps)     = %+.6f   [%s]\n", s.pdiff, (s.valid_mask & (1u << 11)) ? "valid" : "no-bn");
  return 0;
}
