#pragma once
// log.h — tickfeat_live 统一日志(Quill 高性能异步:热路径仅拷贝参数,格式化/IO 交后台线程)。
// 全局 logger + 一次性初始化;main 启动时调 init_logging() 一次,其余文件用 LOG_*(g_log, ...)。

#include "quill/Backend.h"
#include "quill/Frontend.h"
#include "quill/LogMacros.h"
#include "quill/Logger.h"
#include "quill/sinks/ConsoleSink.h"

namespace tflive {

inline quill::Logger* g_log = nullptr;

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

}  // namespace tflive
