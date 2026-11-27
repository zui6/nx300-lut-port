# experimental 分支

这个分支是**实验与未完成工作**的归档。稳定可用的照片 LUT 在 `main` 分支。

| 内容 | 说明 |
|---|---|
| [docs/06-liveview-preview-plan.md](../docs/06-liveview-preview-plan.md) | 取景实时预览为什么缺失、上游架构、移植到 NX300 的步骤与风险 |
| [experimental/make-diagnostic-blob.py](make-diagnostic-blob.py) | 把完整 blob 改造成「只写探针 + 直接转发」的诊断版，用于判断钩子是否被调用 |

## 这里的结论都经过实机验证

- 取景预览**不走** `libiq` 的 `ipcs_3d_lut`，必须另挂 `libcapture-fw-slpcam.so` 的 SmartFilter 链
- 把相机智能滤镜切到「微型」**不能**让预览显示本 LUT（实测无变化）
- 帧路径不能做文件 I/O；诊断版的每帧探针只适合短时间验证

## 未做

- NX300 版 `libcapture-fw-slpcam.so` 的地址定位（5 个 PLT 槽、`+0x48` 效果模式、ADE `RGB3D_LUT`）
- `smartfilter_ram.c` / `smartfilter_loader.c` 的 NX300 重编译
- 观察版桥接（先只记录、不改像素）
