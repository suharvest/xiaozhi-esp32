# 本地声纹 Gateway 设计备忘

## 背景

多数用户使用云端小智 Server，设备侧无法修改云端部署和配置。目标是在不改云端 Server 的前提下，为本地业务系统增加类似人脸识别的 session 级声纹校验能力。

## 结论

最低成本方案是在设备侧复用已有 wake-word Opus 上行数据，旁路发送给本地 voiceprint gateway。gateway 调用本地声纹后端，例如 seeed-local-voice 的 CAM++ 能力，识别当前说话人并写入本地 session 状态。warehouse system 或其他本地 MCP 服务再从 gateway 读取 session 级身份结果。

这个方案不需要重新采集音频，不需要新增 AFE/VAD，不需要第二次 Opus 编码，也不依赖云端 Server 回传声纹结果。

## 现有设备路径

当前固件启用 `CONFIG_SEND_WAKE_WORD_DATA` 时，唤醒后会把约 2 秒 wake-word PCM 编码成 Opus，并作为会话第一段音频发送给 Server。

关键路径：

- `main/audio/wake_words/afe_wake_word.cc`
  - `StoreWakeWordData()` 保留约 2 秒 PCM。
  - `EncodeWakeWordData()` 将 PCM 编成 Opus packet。
  - `GetWakeWordOpus()` 逐个弹出已编码 Opus packet。
- `main/audio/audio_service.cc`
  - `PopWakeWordPacket()` 将 Opus packet 包装成 `AudioStreamPacket`。
- `main/application.cc`
  - `Application::ContinueWakeWordInvoke()` 在唤醒后循环调用 `PopWakeWordPacket()`，再调用 `protocol_->SendAudio()` 发给小智 Server。

建议插入点：

```cpp
while (auto packet = audio_service_.PopWakeWordPacket()) {
    voiceprint_gateway->Send(packet->payload);
    protocol_->SendAudio(std::move(packet));
}
```

实际实现时应避免阻塞主链路。gateway 发送失败、队列满或本地服务不可用时，直接丢弃旁路数据，不能影响 `protocol_->SendAudio()`。

## 资源成本

已有常驻成本：

- wake-word 检测、AFE 和相关模型常驻。
- wake-word PCM 缓存约 2 秒，16 kHz、16-bit、mono，约 64-68 KB。
- AFE wake-word Opus 编码任务栈为 `4096 * 6 = 24 KB`，分配在 PSRAM。

旁路新增成本：

- 不新增采音链路。
- 不新增 Opus encoder。
- 若使用 UDP 或已建立 TCP 长连接，额外峰值内存主要是一个 Opus packet 的复制 buffer 和很小的发送队列。
- 额外 CPU 主要是 memcpy 和 socket send。
- 额外网络流量只发生在唤醒后发送 wake-word Opus 的短窗口内。

不建议：

- 不要缓存完整 2 秒音频后再发，避免额外 heap 峰值。
- 不要使用 HTTP multipart 或 TLS 短连接作为设备到 gateway 的第一版协议，连接和 TLS buffer 对 ESP32-S3 太重。
- 不要在设备侧调用声纹模型。声纹推理放在 local gateway 或 seeed-local-voice 后端。

## Gateway 职责

本地 gateway 负责：

1. 接收设备发来的 wake-word Opus packet。
2. 按会话或唤醒事件聚合短音频。
3. 解码或转码为 CAM++ 后端需要的音频格式。
4. 调用本地声纹识别接口。
5. 将识别结果写入本地 session 状态。
6. 暴露本地查询接口给 warehouse system 或 MCP 服务。

建议 session 状态字段：

```json
{
  "device_id": "...",
  "session_id": "...",
  "speaker_id": "...",
  "speaker_name": "...",
  "confidence": 0.0,
  "source": "voiceprint",
  "updated_at": 0,
  "expires_at": 0
}
```

如果设备侧拿不到云端 session id，可以用本地 wake event id 或时间窗口生成本地 session id，再由业务侧用设备 id 和最近有效窗口做校验。

## 与人脸 session 校验的关系

SenseCAP Watcher 的人脸方案是设备本地识别并在会话级冻结身份；声纹方案可以保持同样的业务语义，但身份来源变成本地 gateway。

推荐融合策略：

- 人脸识别成功时优先使用人脸身份。
- 声纹识别成功时作为无脸或背对设备场景的补充。
- 两者冲突时由业务策略决定：拒绝、降权、或要求二次确认。

## 后续实现步骤

1. 在设备侧增加一个轻量 `VoiceprintGatewayClient`，只负责非阻塞发送 Opus packet。
2. 协议先用 UDP 或长连接 TCP，packet 带 `device_id`、`event_id`、`seq`、`is_last` 和 payload。
3. gateway 聚合 packet 并调用 seeed-local-voice CAM++。
4. warehouse system 从 gateway 查询 `device_id` 当前有效 speaker。
5. 加入丢包、超时、队列满、gateway 不可用的降级逻辑。

## 待实测指标

需要在真实唤醒事件上记录：

- wake-word Opus packet 数。
- wake-word Opus 总字节数。
- Opus 编码耗时。
- 旁路发送耗时。
- 发送前后 free heap 和 minimum free heap。

这些指标决定第一版队列大小和超时时间。
