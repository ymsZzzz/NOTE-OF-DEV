# 20260320_ESP32S3_WebSocket_SSL重认证空指针崩溃

**Tags:** #BUG #ESP32S3 #WebSocket #SSL #空指针
**来源:** `note_of_dev/NOTE.md`（已归档）
**关联:** [[esp_websocket_client并发风险与规避]]

## 环境

- 平台：ESP32-S3
- 组件：`esp_websocket_client`
- 场景：并发接收与发送

## 现象

系统随机崩溃，定位在 `send` 接口底层 SSL 重认证调用路径，表现为空指针访问。

## 排查过程

1. 问题在并发场景下随机出现，单线程下不明显。
2. 崩溃堆栈落在 SSL 重认证相关路径。
3. 配置项对比后，锁定 websocket SSL 重认证功能为高风险路径。

## 根本原因

重认证路径在并发场景下存在不稳定行为，触发空指针访问。

## 解决方案

在 `menuconfig` 中关闭 websocket SSL 重认证配置项。

## 回归检查

关闭后并发收发场景稳定运行，未复现该空指针崩溃。
