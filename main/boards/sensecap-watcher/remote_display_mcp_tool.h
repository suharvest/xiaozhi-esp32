#ifndef REMOTE_DISPLAY_MCP_TOOL_H
#define REMOTE_DISPLAY_MCP_TOOL_H

#include "mcp_server.h"
#include "remote_display.h"

// 远程显示 MCP 工具 - 允许通过语音/AI交互配置远程显示
class RemoteDisplayMcpTool {
public:
    RemoteDisplayMcpTool() = default;

    // 初始化工具，注册到 MCP 服务器
    void Initialize();

private:
    // MCP 工具回调 - 语音友好的投屏控制
    ReturnValue HandleScreenCast(const PropertyList& properties);

    // MCP 工具回调 - 详细配置（保持兼容）
    ReturnValue HandleConfigRemoteDisplay(const PropertyList& properties);

    // 缓存发现的设备列表（供多设备选择使用）
    std::vector<DiscoveredDisplay> cached_displays_;
};

#endif // REMOTE_DISPLAY_MCP_TOOL_H
