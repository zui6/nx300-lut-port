# 取景实时预览 LUT：现状与移植计划

> 分支：`experimental`
> 稳定版（照片 LUT）见 `main` 分支的 [docs/05-photo-lut-working.md](../docs/05-photo-lut-working.md)

---

## 一、为什么预览不带 LUT（已实测确认）

`main` 分支那套注入改写了 `libiq-fw-slpcam.so` 的两处：

- vtable：`_ZTV12CIpcs3DLutD4` 的 `Update` 槽
- GOT：`ipcs_3d_lut` 的 JUMP_SLOT

这条链路**只覆盖拍照 JPEG**。取景走的是另一条独立管线：

| 路径 | 库 | 入口 |
|---|---|---|
| 拍照 JPEG | `libiq-fw-slpcam.so` | `ipcs_3d_lut`（已接管 ✅） |
| 取景显示 / 快速预览 | `libcapture-fw-slpcam.so` | SmartFilter 节点的 YCC 处理链 |

原厂在取景侧有**独立的 SmartFilter 节点、处理线程、缓冲区状态和完成回调**。
ADE 的 YCC 色彩包装器带 `RGB3D_LUT` 开关，最终驱动 IPCS 三维表硬件开关。

### 实测否定的捷径

曾经假设「把相机智能滤镜切到‘微型’，取景就会启用 3D LUT 硬件，从而显示我们的表」。
**实测：无变化。** 切换智能滤镜确实触发了 `Update`（探针 `/mnt/mmc/lut-hook-entered`
有更新），LUT 表也确认被替换（trace `forwarded_ptrs` 指向 blob 内），
但取景画面没有任何变化，也没有出现原厂的「微型」效果。

结论：**取景侧必须单独挂钩，不能靠现有注入顺带覆盖。**

---

## 二、上游实现（NX300M 1.15）的架构

以下来自 `nx300m-mod` 的 `docs/SMARTFILTER_LUT_PREVIEW.md`。

### 单槽方案

- 只占用**一个** 原厂 SmartFilter 槽位：对应 **Miniature（微型）**
  - liveview 模式 `0` → ADE 效果 `10` → 成片内部效果枚举 `1`
- `GetEffectMode` 实际读对象 `+0x48`，不是 UserData 编码 `0x390001`
- 原厂节点分派 `0…14`，关闭值 `15`
- 其他槽位仍交还原厂

### 组件

| 文件 | 大小 | 作用 |
|---|---|---|
| `runtime/smartfilter_loader.c` | 72 KB | 替换 Capture 库的 **5 个 PLT 数据槽** |
| `runtime/smartfilter_ram.c` | 40 KB | RAM 模块（`smartfilter_ram.ld` 零地址链接） |
| `runtime/smartfilter_lut_bridge.c` | 8 KB | `nx_sf_preview` / `nx_sf_effect` / `nx_sf_select` |
| `runtime/smartfilter_ipcs.c` | 8 KB | 原厂 API 适配器：四路 B2Y 信号量、ADE 初始化、三通道写表、YCC 处理、cache 交接、IQ 复位 |
| `smartfilter-preview.sh` / `smartfilter-live-preview.sh` | — | 部署与控制 |

### 调用链

```
固定原厂取景模式
  → nx_sf_preview   建立本线程预览上下文
  → 原厂 smartFilter 构造 ADE_Image、注册 flush
  → nx_sf_effect     读取已暂存的 LUT
      → 取资源 → 处理 → 恢复参数 → 释放
  → 原厂工作线程释放帧标志、执行完成回调

MOD 选择风格/强度
  → nx_sf_select    复制、混合到 RAM，更新 selected_generation
  → 下次成功处理后更新 applied_generation
```

### 关键约束

- 帧路径**不分配内存、不访问文件、不启 Shell、不拟合颜色**
- 一份 RAM 配置含风格 ID、强度、代次、`3×730×uint16` 预览表
- 每通道 729 节点 + 零 padding，节点范围 `0…1023`
- 后端 busy / 处理失败时：先恢复参数、释放事务，再填透传输出
- 拍照请求先阻止新预览进入，在途帧返回后协调者取得拍照门

### 另一个模块：`CLiveviewNodeDisplayAdjust`

`docs/LUT_LIVEVIEW.md` 记录了一个**观察用**钩子（不改像素）：

- 库：`libcapture-fw-slpcam-nx300.so`（NX300M 的名字）
- 函数：`CLiveviewNodeDisplayAdjust::Execute(CCapImage *, void *, unsigned)` @ `0x278fc0`
- 虚表：`_ZTV26CLiveviewNodeDisplayAdjust` @ `0x39c890`，目标入口 `0x39c8ac`
- 原厂函数使用节点参数 `self+0x38…0x4c`
- 帧路径无 Shell / 文件 / 内存分配 / 固定等待

这个钩子**只做观察**，不应用 LUT。若要走「在显示前改像素」的路子，
需要自己实现 YCC→YCC 的逐像素 LUT，成本和风险都更高。

---

## 三、移植到 NX300 1.45 需要做的事

### 第 1 步：定位 NX300 的地址（纯静态，不需上机）

在 `libcapture-fw-slpcam.so`（NX300 版，**4,731,xxx 字节左右**）里找：

1. `SmartFilter` 相关符号与 PLT 槽（对应 mod 替换的那 5 个）
2. `GetEffectMode` 读取的 `+0x48` 偏移是否一致
3. ADE YCC 包装器的 `RGB3D_LUT` 开关位置
4. （若走另一条路）`CLiveviewNodeDisplayAdjust::Execute` 的 NX300 地址与虚表

建议工具：
- `readelf -Ws` / `nm -D` 找符号
- 我们已有的 ELF 解析脚本（`tmp/plt3.py` 思路）解 `.rel.plt` + PLT 桩
- 注意：**这个库的 `.plt` 不是标准 12 字节布局**，需要按 `add ip,pc,#imm` /
  `add ip,ip,#imm` / `ldr pc,[ip,#imm]!` 三条指令 + ARM 立即数 `ROR` 解码来还原

### 第 2 步：重编译 RAM 模块

`smartfilter_ram.c` 里的硬编码地址要换成 NX300 的。构建脚本
`tools/build_smartfilter_preview.py` 已有完整校验（header、尺寸、无重定位），
可直接复用其 `clang --target=arm-linux-gnueabi` 工具链。

### 第 3 步：上机验证

- 相机智能滤镜必须切到 **微型**
- 用 `smartfilter-preview.sh` 部署与启停
- 观察 `nx_sf_preview` / `nx_sf_effect` 是否被调用（先用观察版，不改像素）

---

## 四、风险与注意

1. **这是另一个独立工程**，工作量与已完成的部分相当，需要多轮上机
2. 取景是**每帧**路径，任何错误都会立刻表现为卡顿或黑屏
3. 帧路径**不能做文件 I/O**——这也是为什么本分支的诊断版 blob
   用了「每帧写 SD」的探针，**绝不能用于带 SmartFilter 的长时间取景**
4. 该链路涉及 IPCS 硬件表与 DMA cache 交接，出错可能不只是软件层面
5. 建议：**先做纯观察版**（只记录，不改像素），确认调用点和帧率后再改像素

---

## 五、本分支内容

| 路径 | 说明 |
|---|---|
| `experimental/make-diagnostic-blob.py` | 把完整 blob 源码改造成「探针+直接转发」诊断版 |
| `docs/06-liveview-preview-plan.md` | 本文 |

### 诊断版 blob 的用途

把两个钩子改成「写一个探针文件，然后立刻原样转发原厂」：

- `nx_lut_ram_update` → 写 `/mnt/mmc/lut-probe-update`，转发
- `nx_lut_ram_ipcs` → 写 `/mnt/mmc/lut-probe-ipcs`，转发

**用来回答一个问题：钩子到底有没有被调用。** 与 LUT 逻辑完全解耦，
所以相机行为保持原厂，可以安全地反复测试。

> ⚠️ `ipcs` 钩子如果每帧写文件（尤其带 `fsync`），会严重拖慢相机并磨损 SD。
> 只适合短时间验证，测完立刻恢复原厂槽位或重启。

### 一个操作教训

**不要手工用 `dd` 改 `/proc/<pid>/mem` 里的 vtable/GOT 槽位。**
这些地址依赖当次开机的 `iq_bias`（每次开机可能不同），
写错偏移会破坏相机进程内存。让 loader 的 `restore` 或重启来处理。
