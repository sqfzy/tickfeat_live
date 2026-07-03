-- tickfeat_live — 实时 f0-f9 因子 host:轮询 gconf shm → tick_feat 流式引擎 → FactorBoard。
-- 独立工程:复用 ../cpp/src(tick_feat 引擎)+ ../mdreplay/gconf(官方段,submodule)+ spdlog。
set_project("tickfeat_live")
set_languages("c++23")
add_rules("mode.release", "mode.debug")

-- Quill(高性能异步日志,header-only);float 收缩关闭以求可复现(同 tick_feat 引擎口径)。
add_requires("quill")
add_cxxflags("-ffp-contract=off")

local ENGINE = {"../cpp/src", "../cpp/src/live"}
local GCONF  = "gconf/include"   -- vendored gconf(jt_dev3, 含 factor_board.h + 段名 shm_names.h)

target("tickfeat_live")
    set_kind("binary")
    add_files("src/main.cpp")
    add_includedirs("src", ENGINE[1], ENGINE[2], GCONF)
    add_packages("quill")
    add_syslinks("rt", "pthread")

target("tickfeat_dump")  -- 参照消费者(另进程读回验证)
    set_kind("binary")
    add_files("src/dump.cpp")
    add_includedirs("src", GCONF)
    add_syslinks("rt")
