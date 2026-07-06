#pragma once
// log.h — tickfeat_live 统一日志(Quill 高性能异步:热路径仅拷贝参数,格式化/IO 交后台线程)。
// 全局 logger + 一次性初始化;main 启动时调 init_logging() 一次,其余文件用 LOG_*(g_log, ...)。

#include <string>

#include "quill/Backend.h"
#include "quill/Frontend.h"
#include "quill/LogMacros.h"
#include "quill/Logger.h"
#include "quill/sinks/ConsoleSink.h"
#include "quill/sinks/FileSink.h"

namespace tflive {

inline quill::Logger* g_log  = nullptr;
inline quill::Logger* g_dump = nullptr;   // 可选:每条结算行落 CSV(对拍/复现);nullptr=关闭

// 起后台线程 + 建 console logger(时间戳到毫秒,与原 spdlog 版式一致)。
inline void init_logging() {
  quill::BackendOptions bopts;
  bopts.check_printable_char = {};   // 关非 ASCII 转义:允许中文日志(默认会把含字符串参数行的中文转义成 \xXX)
  quill::Backend::start(bopts);
  g_log = quill::Frontend::create_or_get_logger(
      "root", quill::Frontend::create_or_get_sink<quill::ConsoleSink>("console"),
      quill::PatternFormatterOptions{"[%(time)] [%(log_level)] %(message)",
                                     "%Y-%m-%d %H:%M:%S.%Qms"});
}

// 建因子落盘 logger:独立 FileSink(截断)+ 裸消息 pattern(无时间戳/级别 → 干净 CSV)。
// 异步:热路径仅拷贝参数,格式化/IO 交后台线程 → 不影响 poll/feed/emit 延迟。首行写表头。
inline void init_dump_logger(const std::string& path) {
  quill::FileSinkConfig fcfg;
  fcfg.set_open_mode('w');   // 截断重建(幂等)
  auto sink = quill::Frontend::create_or_get_sink<quill::FileSink>(path, fcfg);  // sink_name 即文件名
  g_dump = quill::Frontend::create_or_get_logger("dump", std::move(sink),
                                                 quill::PatternFormatterOptions{"%(message)"});
  LOG_INFO(g_dump, "lid,ts_us,f0,f1,f2,f3,f4,f5,f6,f7,f8,f9,mid,pdiff,"
                   "ob_uid_lo,ob_uid_hi,bn_uid_lo,bn_uid_hi,tr_ns_lo,tr_ns_hi,tr_id_lo,tr_id_hi");
}

}  // namespace tflive
