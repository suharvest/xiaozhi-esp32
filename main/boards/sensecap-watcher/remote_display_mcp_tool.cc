#include "remote_display_mcp_tool.h"
#include <esp_log.h>
#include <cJSON.h>

static const char* TAG = "RemoteDisplayMcpTool";

void RemoteDisplayMcpTool::Initialize() {
    auto& mcp_server = McpServer::GetInstance();

    // 注册语音友好的投屏控制工具
    mcp_server.AddTool("self.screen_cast",
        "Control screen casting to external display (投屏控制).\n"
        "Actions:\n"
        "  - start: Start screen casting (auto-discover or use saved config)\n"
        "  - stop: Stop screen casting\n"
        "  - status: Show connection status\n"
        "  - discover: Search for available display devices\n"
        "  - select: Select a discovered device by name or number\n"
        "  - set_ip_suffix: Set server IP using only the last segment (0-255)\n"
        "\n"
        "Examples:\n"
        "  User says: '打开投屏' -> action='start'\n"
        "  User says: '关闭投屏' -> action='stop'\n"
        "  User says: '投屏到客厅显示器' -> action='select', device='客厅显示器'\n"
        "  User says: '投屏服务器地址最后一位是100' -> action='set_ip_suffix', suffix=100",
        PropertyList({
            Property("action", kPropertyTypeString),
            Property("device", kPropertyTypeString, std::string("")),  // 设备名称或编号
            Property("suffix", kPropertyTypeInteger, 0)  // IP 最后一段（0-255）
        }),
        [this](const PropertyList& properties) -> ReturnValue {
            return HandleScreenCast(properties);
        });

    // 保留原有的详细配置工具
    mcp_server.AddTool("self.config_remote_display",
        "Configure remote display settings for Raspberry Pi external screen.\n"
        "Actions:\n"
        "  - status: Show current configuration and connection status\n"
        "  - enable: Enable remote display feature\n"
        "  - disable: Disable remote display feature\n"
        "  - set_url: Set the WebSocket server URL (e.g. ws://192.168.1.100:8765)\n"
        "  - connect: Connect to the remote display server\n"
        "  - disconnect: Disconnect from the server",
        PropertyList({
            Property("action", kPropertyTypeString),
            Property("url", kPropertyTypeString, std::string(""))  // Optional, only for set_url action
        }),
        [this](const PropertyList& properties) -> ReturnValue {
            return HandleConfigRemoteDisplay(properties);
        });

    ESP_LOGI(TAG, "RemoteDisplayMcpTool initialized");
}

ReturnValue RemoteDisplayMcpTool::HandleConfigRemoteDisplay(const PropertyList& properties) {
    auto action = properties["action"].value<std::string>();
    auto* remote = RemoteDisplay::GetInstance();

    if (action == "status") {
        auto config = RemoteDisplay::LoadConfig();
        cJSON* result = cJSON_CreateObject();
        cJSON_AddBoolToObject(result, "enabled", config.enabled);
        cJSON_AddStringToObject(result, "url", config.server_url.c_str());
        cJSON_AddNumberToObject(result, "timeout_ms", config.timeout_ms);
        cJSON_AddBoolToObject(result, "connected", remote->IsRunning());
        return result;

    } else if (action == "enable") {
        auto config = RemoteDisplay::LoadConfig();
        config.enabled = true;
        RemoteDisplay::SaveConfig(config);
        ESP_LOGI(TAG, "Remote display enabled");
        return std::string("Remote display enabled. Use 'connect' action to connect.");

    } else if (action == "disable") {
        auto config = RemoteDisplay::LoadConfig();
        config.enabled = false;
        RemoteDisplay::SaveConfig(config);
        remote->Stop();
        ESP_LOGI(TAG, "Remote display disabled");
        return std::string("Remote display disabled and disconnected.");

    } else if (action == "set_url") {
        auto url = properties["url"].value<std::string>();
        if (url.empty()) {
            throw std::runtime_error("URL is required for set_url action");
        }
        auto config = RemoteDisplay::LoadConfig();
        config.server_url = url;
        RemoteDisplay::SaveConfig(config);
        ESP_LOGI(TAG, "Remote display URL set to: %s", url.c_str());
        return std::string("Server URL set to: " + url);

    } else if (action == "connect") {
        if (remote->IsRunning()) {
            return std::string("Already connected to remote display server.");
        }
        if (remote->StartWithConfig()) {
            ESP_LOGI(TAG, "Connected to remote display server");
            return std::string("Successfully connected to remote display server.");
        } else {
            return std::string("Failed to connect. Check URL and network settings.");
        }

    } else if (action == "disconnect") {
        remote->Stop();
        ESP_LOGI(TAG, "Disconnected from remote display server");
        return std::string("Disconnected from remote display server.");
    }

    throw std::runtime_error("Unknown action: " + action + ". Valid actions: status, enable, disable, set_url, connect, disconnect");
}

ReturnValue RemoteDisplayMcpTool::HandleScreenCast(const PropertyList& properties) {
    auto action = properties["action"].value<std::string>();
    auto* remote = RemoteDisplay::GetInstance();

    if (action == "start") {
        // 1. 如果已连接，直接返回
        if (remote->IsRunning()) {
            return std::string("投屏已开启");
        }

        // 2. 尝试使用已保存的配置（1秒超时，快速失败）
        auto config = RemoteDisplay::LoadConfig();
        if (!config.server_url.empty()) {
            if (remote->Start(config.server_url, 1000)) {
                return std::string("投屏已开启，已连接到 " + config.server_url);
            }
        }

        // 3. 尝试 mDNS 自动发现（500ms超时）
        auto displays = remote->DiscoverDisplays(500);

        if (displays.empty()) {
            // 未发现设备，提示用户手动输入
            return std::string("未发现投屏设备。请告诉我服务器 IP 地址的最后一段数字(0-255)，"
                              "例如服务器 IP 是 192.168.1.100，请说 一百");
        } else if (displays.size() == 1) {
            // 发现唯一设备，自动连接
            auto& d = displays[0];
            std::string url = "ws://" + d.ip + ":" + std::to_string(d.port);
            if (remote->Start(url)) {
                // 保存配置
                config.server_url = url;
                config.enabled = true;
                RemoteDisplay::SaveConfig(config);
                return std::string("投屏已开启，已连接到 " + d.name);
            }
            return std::string("连接失败，请检查网络");
        } else {
            // 发现多个设备，返回列表供选择
            cJSON* result = cJSON_CreateObject();
            cJSON_AddStringToObject(result, "message", "发现多个投屏设备，请选择：");
            cJSON* list = cJSON_CreateArray();
            for (size_t i = 0; i < displays.size(); i++) {
                cJSON* item = cJSON_CreateObject();
                cJSON_AddNumberToObject(item, "index", i + 1);
                cJSON_AddStringToObject(item, "name", displays[i].name.c_str());
                cJSON_AddStringToObject(item, "ip", displays[i].ip.c_str());
                cJSON_AddItemToArray(list, item);
            }
            cJSON_AddItemToObject(result, "devices", list);

            // 缓存发现结果，供后续 select 使用
            cached_displays_ = displays;
            return result;
        }

    } else if (action == "stop") {
        remote->Stop();
        return std::string("投屏已关闭");

    } else if (action == "status") {
        auto config = RemoteDisplay::LoadConfig();
        if (remote->IsRunning()) {
            return std::string("投屏已开启，连接到 " + config.server_url);
        } else {
            return std::string("投屏未开启");
        }

    } else if (action == "discover") {
        auto displays = remote->DiscoverDisplays(500);

        if (displays.empty()) {
            return std::string("未发现投屏设备");
        }

        cJSON* result = cJSON_CreateObject();
        cJSON_AddStringToObject(result, "message", "发现以下投屏设备：");
        cJSON* list = cJSON_CreateArray();
        for (size_t i = 0; i < displays.size(); i++) {
            cJSON* item = cJSON_CreateObject();
            cJSON_AddNumberToObject(item, "index", i + 1);
            cJSON_AddStringToObject(item, "name", displays[i].name.c_str());
            cJSON_AddStringToObject(item, "ip", displays[i].ip.c_str());
            cJSON_AddNumberToObject(item, "port", displays[i].port);
            cJSON_AddItemToArray(list, item);
        }
        cJSON_AddItemToObject(result, "devices", list);

        // 缓存发现结果
        cached_displays_ = displays;
        return result;

    } else if (action == "select") {
        auto device = properties["device"].value<std::string>();
        if (device.empty()) {
            return std::string("请指定设备名称或编号");
        }

        // 如果缓存为空，先执行发现
        if (cached_displays_.empty()) {
            cached_displays_ = remote->DiscoverDisplays(500);
        }

        if (cached_displays_.empty()) {
            return std::string("未发现投屏设备，请先确保服务器已启动");
        }

        // 查找设备
        DiscoveredDisplay* target = nullptr;

        // 尝试按编号查找
        try {
            int index = std::stoi(device);
            if (index >= 1 && index <= static_cast<int>(cached_displays_.size())) {
                target = &cached_displays_[index - 1];
            }
        } catch (...) {
            // 不是数字，尝试按名称查找
            for (auto& d : cached_displays_) {
                if (d.name == device || d.name.find(device) != std::string::npos) {
                    target = &d;
                    break;
                }
            }
        }

        if (!target) {
            return std::string("未找到设备: " + device);
        }

        // 连接到选定设备
        std::string url = "ws://" + target->ip + ":" + std::to_string(target->port);
        if (remote->Start(url)) {
            // 保存配置
            auto config = RemoteDisplay::LoadConfig();
            config.server_url = url;
            config.enabled = true;
            RemoteDisplay::SaveConfig(config);
            return std::string("投屏已开启，已连接到 " + target->name);
        }
        return std::string("连接失败，请检查网络");

    } else if (action == "set_ip_suffix") {
        auto suffix = properties["suffix"].value<int>();
        if (suffix < 0 || suffix > 255) {
            return std::string("IP 地址最后一段应该在 0 到 255 之间");
        }
        if (remote->ConnectWithIPSuffix(suffix)) {
            // 保存配置
            auto config = RemoteDisplay::LoadConfig();
            config.server_url = "ws://" + remote->GetIPPrefix() + std::to_string(suffix) + ":8765";
            config.enabled = true;
            RemoteDisplay::SaveConfig(config);
            return std::string("投屏已开启");
        }
        return std::string("连接失败，请确认服务器已启动");
    }

    throw std::runtime_error("未知操作: " + action);
}
