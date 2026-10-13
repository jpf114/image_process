#pragma once
// TilingParams ↔ JSON（workflow / tiling 配置同构）。
//
// 用途：多景（目录输入）按景并行时，app 层把整套参数序列化为每景子任务 JSON，
// 交由子进程 `workflow --config <json> --only tiling` 执行——与 workflow 配置
// 走同一个解析入口，避免"命令行重建参数"遗漏字段导致行为漂移。
#include "ip/json_io.h"
#include "tiling/i_tiling.h"

#include <string>

namespace iptl {

// 序列化全部字段（含默认值，保证往返一致）
void tilingParamsToJson(const TilingParams& p, ip::Json& out);

// 反序列化：缺失键保留 p 的当前值；类型不符返回 false 并填充 err
bool tilingParamsFromJson(const ip::Json& in, TilingParams& p, std::string& err);

} // namespace iptl
