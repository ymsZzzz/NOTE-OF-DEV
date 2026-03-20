# Factory与OTA分区切换实践

**Tags:** #ESP32S3 #ESP_IDF #Factory #OTA #分区表
**来源:** `note_of_dev/NOTE.md`（已归档）
**关联:** [[20260320_ESP32S3_factory分区后idf默认烧录失败]]

## 关键约束

- 工厂固件与正式固件的 `menuconfig` 关键配置应保持一致。
- 分区表必须严格一致，否则分区切换 API 行为不可靠。

## 启动与切换流程

1. 首次烧录不写 `otadata`，让 bootloader 默认从 `factory` 启动。
2. 工厂流程结束后，调用 API 切换到 `ota0` 正式固件。
3. 正式固件首次启动需尽早做分区校验并标记可用，防止断电后回滚到 `factory`。

## 工程注意事项

引入 `factory` 分区后，默认 IDF 烧录地址可能导致镜像放错分区或尺寸不匹配，通常需要自定义烧录地址与流程。
