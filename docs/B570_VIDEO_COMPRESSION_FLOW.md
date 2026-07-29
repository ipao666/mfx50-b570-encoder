# B570 视频压缩流程说明

## 实时交通流压缩流程

```text
摄像头 RTSP / GB28181 流
  -> 工程侧 demux
  -> H.264/H.265 access unit
  -> 45 路调度
  -> B570 oneVPL 硬解
  -> video-memory surface
  -> 低频场景特征和 ROI 分析
  -> profile 选择：base_q36b48 / day_guard_q34 / risk_q35
  -> ROI delta QP 保护
       车辆/运动目标：中等保护
       车牌/车辆核心：强保护
       标牌/文字：小面积弱保护
       树叶/天空/路面纹理：默认不低 QP 保护
  -> HEVC CQP 编码
       QPI/QPP/QPB
       B 帧，DIST=4
       GOP_SIZE=300
       AdaptiveI:on
  -> H.265 elementary stream
  -> 工程侧 mux 成 MP4/TS/FLV 或继续推流
```

## 为什么不建议每路先转完整 YUV

Raw NV12 1080p30 单路约 93 MB/s，还没算额外拷贝和调度开销。45 路会变成每秒数 GB 的内存流量，再叠加算法分析和编码提交，容易拖垮 CPU/内存带宽。

对于 B570 45 路目标，推荐路径是：

- demux 编码 access unit；
- 解码和编码尽量留在 oneVPL 内；
- surface 尽量保留在显存；
- ROI 按受控间隔分析；
- 避免每帧 CPU 全分辨率 MBQP。

## 当前 B570 运行示例控制项

封装包内的 B570 example 使用：

- B570 硬解硬编；
- video-memory surface；
- oneVPL 多 session 异步调度；
- HEVC CQP;
- IPB frame structure with `DIST=4`;
- `GOP_SIZE=300`;
- `AdaptiveI:on`;
- `ScenarioInfo 6`, `ContentInfo 3`, `AdaptiveCQM:on`;
- 交通监控 ROI delta QP；
- 车牌 ROI 扩张和保护；
- ROI 分析结果复用间隔 30；
- 默认关闭全帧 MBQP，保证吞吐。

## 离线 MP4 压缩流程

```text
输入 H.264 MP4 文件
  -> ffmpeg demux 成 AnnexB H.264
  -> 运行同一套 B570 运行示例
  -> 写出 H.265 elementary stream
  -> ffmpeg 再 mux 回 MP4
```

这样能让离线视频压缩行为尽量接近实时摄像头流。
