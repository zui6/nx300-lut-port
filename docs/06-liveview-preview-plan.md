# 取景实时预览 LUT：GOT 槽方案（已定位，待实现）

> 分支：`experimental`　稳定版（照片 LUT）见 `main` 的 `docs/05-photo-lut-working.md`

---

## 一、结论先行

取景预览**不需要移植**上游那套 120 KB 的 SmartFilter 桥接。
它用的是**同一个 `ipcs_3d_lut` 函数**，只是通过 **`libcapture` 自己的 GOT 槽**调用。
我们之前只改了 `libiq` 里的槽，所以只有拍照生效。

**方案：多打一个 GOT 槽。**

| 模块 | 符号 | GOT 槽 | 状态 |
|---|---|---|---|
| `libiq-fw-slpcam.so` | `ipcs_3d_lut` | `base + 0x3c1aa8` | ✅ 已打（拍照生效） |
| `libcapture-fw-slpcam.so` | `ipcs_3d_lut` | **`base + 0x490468`** | ⬜ **待打（取景）** |

---

## 二、证据（NX300 1.45，libcapture sha256 `c8daff58…`）

### 2.1 导入确认

`libcapture-fw-slpcam.so` 的动态符号表里，`ipcs_3d_lut` 是**未定义（导入）符号**，索引 328。

### 2.2 GOT 槽

`.rel.plt` 里对应的重定位：

```
r_offset = 0x490468   type = 22 (R_ARM_JUMP_SLOT)   reloc_index = 2721
所属节   = .got (0x48d9d8 .. 0x4957c0)
```

### 2.3 PLT 桩

```
0x0013abdc  add ip, pc, 0x300000
0x0013abe0  add ip, ip, 0x55000
0x0013abe4  ldr pc, [ip, 0x884]!
```

验算：`(0x13abdc+8) + 0x300000 + 0x55000 + 0x884 = 0x490468` ✅ 与 GOT 槽一致。

### 2.4 唯一调用点

对全库 `.text` 做 BL 目标扫描，指向 `0x13abdc` 的**只有 1 处**：

```
0x15b650   in CDDWrapperIPCS_B2Y::SetIQParam_3D_LUT(stIPCS_B2Y_IQParam*) + 0x74
```

即 `CDDWrapperIPCS_B2Y::SetIQParam_3D_LUT`（`0x15b5dc`，`0x54` 字节）
负责把 3D LUT 表写进硬件 —— 这就是**取景侧的写表入口**。

### 2.5 顺带确认的相关符号

| 符号 | 地址 | 说明 |
|---|---|---|
| `CLiveviewNodeSmartFilter::smartFilter` | `0x1d928c` (0x860) | 逐帧效果分发，switch 0..9 |
| `CLiveviewNodeSmartFilter::Execute` | `0x1d8c0c` | 虚表入口 |
| `CLiveviewNodeSmartFilter::selectiveColor` | `0x1d9aec` (0x70c) | |
| `_ZTV24CLiveviewNodeSmartFilter` | `0x3c9678` (0x60) | 虚表 |
| `ADE_SmartFilter_YccProcess`（导入） | GOT `0x48ed44` | ADE 包装器 |
| `CLiveviewNodeDisplayAdjust::Execute` | `0x1cb7b8` | 上游"观察钩子"用的入口 |

`smartFilter` 里只调用 `ADE_SmartFilter_YccProcess`，**不**直接调 `ipcs_3d_lut`——
说明取景 LUT 是在别处经 `SetIQParam_3D_LUT` 配置的，与逐帧效果分发解耦。

---

## 三、blob 侧必须同步修的一个坑（**已完成**）

`nx_lut_ram_ipcs` 原本只按 `tid` 查上下文，而上下文由 `nx_lut_ram_update` 创建、
用完立刻释放（`s->tid=0`）。`SetIQParam_3D_LUT` 在**另一个线程**上运行，
那个线程永远没有上下文，于是走 `unmatched()` 分支直接转发原厂表 ——
**即使 GOT 槽打上也不会生效**。

已在 `blob/lut_ram_blob.c` 修好：没有上下文时按同一套
`__sync_bool_compare_and_swap` 逻辑绑定一个，调用 `selection()` 初始化，
并**保留到进程结束**（`selection()` 有按文件戳的缓存，不是每帧文件 I/O）。
同时补了 `enabled` 检查。

这部分不涉及配置结构体，`NX_LUT_RAM_ABI` 不变。

---

## 四、loader 侧待实现（下一步）

`build/lut_live_loader.c` 目前只认 `libiq` / `libudd` / `libc` 三个已认证映射，
需要增加第四个：`libcapture`。

### 4.1 要改的点

| 位置 | 现状 | 需要 |
|---|---|---|
| 认证表 | `iq` / `udd` / `libc` 三个 `admitted_image` | 加 `cap`：sha256 + 文件大小 |
| L268 | `refresh_admitted_image×3` | 加 `refresh_admitted_image(&cap,&cap_map,&cap_file)` |
| L275 | `code_check` 四处 | 加一处：`cap.bias+0x15b5dc` 对应的代码指纹 |
| L277 | `slot_a` / `slot_b` | 加 `slot_c = cap.bias + 0x490468` |
| L278-279 | `cover` + inode + 读值 | 加 `mc = cover(slot_c,4,1,0)`、`mc->inode==cap.first->inode`、读 `c` |
| L279 之后 | `a`/`b` 必须等于原厂 | 加 `c == udd.bias + 0x955c8` 校验 |
| commit | poke `slot_a` / `slot_b` | 加 poke `slot_c = region + h.ipcs_entry` |
| restore / known / existing | 只检查两个槽 | **三个槽都要**检查与还原 |
| 状态输出 | `slot_update` / `slot_lut` | 加 `slot_capture_lut` |

### 4.2 注意事项

- **三个槽指向同一个 hook 入口**（`region + h.ipcs_entry`），不需要第二份代码
- 还原时必须把 `slot_c` 写回 `udd.bias + 0x955c8`
- `libcapture` 指纹：sha256 `c8daff589b585ed72842db24ad1a073e3028ab5a28f82f21f9bf5bea059030f2`，
  大小 `4,791,652`；相机上文件名是 **`libcapture-fw-slpcam-nx300.so`**
  （本地 `bin\camlibs\libcapture-fw-slpcam.so` 是同一份）
- 建议顺序：先写 libcapture（取景），再写 libiq（拍照），最后统一回读校验
- 每个槽 patch 前必须确认当前值仍是 `udd.bias+0x955c8`，否则说明有别的注入在跑，应拒绝

### 4.3 验证方法

1. 装好后进取景，看画面是否变黑白
2. trace 里应出现**两个不同的 `update_tid`**：一个来自 libiq 的 `Update`，
   一个来自 `SetIQParam_3D_LUT` 所在线程
3. 若取景变了但照片没变 → libiq 槽被漏；反之 → libcapture 槽没打上

---

## 五、与上游 SmartFilter 方案的关系

上游 `nx300m-mod` 走"占用一个原厂 SmartFilter 槽位（Miniature）+ YCC→IPCS→YCC 桥接"，
需要替换 Capture 库的 5 个 PLT 槽和 ~120 KB 的 RAM 模块。

**本方案更轻**：直接复用原厂已有的 3D LUT 写表路径，只换表内容。
代价是不受"效果选择"控制 —— 只要相机写 3D LUT 表，就一定是我们的表。
若日后需要"多风格 + 强度混合"，blob 的 `selection()` 已支持（代次/强度/混合），
无需换架构。

---

## 六、复现这些结论的命令

```powershell
# 导入确认
rizin -N -q -e scr.color=0 -c "is~ipcs_3d_lut" libcapture-fw-slpcam.so
# PLT 桩
rizin -N -q -e scr.color=0 -c "s sym.imp.ipcs_3d_lut; pd 4" libcapture-fw-slpcam.so
# GOT 槽：解析 .rel.plt 中 sym_idx==328 的 r_offset
# 调用点：扫描 .text 中 BL 目标 == 0x13abdc 的指令
```

---

## 七、一次踩坑记录（避免重复）

排查过程中曾手工用 `dd` 往 `/proc/<pid>/mem` 写 vtable/GOT 槽位来"恢复原厂值"，
但**地址用了缓存的 `iq_bias`**（每次开机不同），结果写到错误偏移，
破坏了相机进程内存（重启可恢复，非永久损伤）。

**规则：任何槽位地址都必须从当次 `status` 现取 `iq_bias` / `udd_bias`，
不要用记忆里的值。**

---

# 附：SmartFilter 桥接移植规格（NX300 1.45）

> 2026-11-27 晚补。上游实现全部来自 `repo/nx300m-mod/`（`tools/build_smartfilter_preview.py`
> 是权威规格，`runtime/smartfilter_lut_bridge.c` / `smartfilter_ipcs.c` 是实现）。

## 一、上游到底怎么做的

**实时预览的像素运算不是软件算的**，是让原厂 IPCS 硬件做 YCC→YCC 的 3D LUT：

```
nx_sf_preview(node,image,data,mode)   钩 smartFilter，
    按线程建上下文、抢"帧门"，然后调原厂函数
        ↓ 原厂函数内部会"应用效果"，那一步也被钩
nx_sf_effect(effect,image)
    mode != slot_mode → 走原厂
    否则：
      begin   按升序 trywait 4 个 B2Y 信号量；备份 6 个 ADE 开关
      render  取几何(宽/高/stride/采样必须 YCC420)、取 Y/UV 输入输出平面地址
              ipcs_open() → ADE_Ipcs_param_init()
              只打开 ADE_IP_switch_RGB3D_LUT，其余关闭
              ipcs_3d_lut(c, table+c*730) × 3 通道
              FlushDCache(输入 Y/UV)
              ADE_Ipcs_Ycc_correct_Param_SET(image)     ← 真正的 IPCS 运算
              FlushDCache(输出 Y/UV)
      restore ipcs_init() → IQ_IPC_Initialize(0) → IQ_IPC_Process(0)
              还原 6 个 ADE 开关 → ipcs_close()
      end     逆序 sem_post ×4
```

- 表格式：3 通道 × 729 节点（9³）+ 每通道 1 个零 padding，`u16`，取值 0..1023
- 强度混合：`(identity*(100-s) + value*s + 50)/100`
- **不需要在机身选滤镜**：它抢在"应用效果"之前，只看 `mode == slot_mode`（0 = 普通取景）
- JPEG 复用已有的 `ipcs_3d_lut` 替换；`restore` 里的 `IQ_IPC_Process(0)` 就是把新色彩推给 JPEG
- RAW 不碰：只处理 YCC 平面
- 拍照让路：`capture_pending` 标志让下一帧立刻回退原厂

## 二、NX300 上的 7 个钩子（已全部定位，GOT 槽）

| # | 符号 | 类型 | NX300 GOT 槽 |
|---|---|---|---|
| 1 | `_ZN24CLiveviewNodeSmartFilter11smartFilterEP9CCapImagePv22E_LIVEVIEW_SMARTFILTER` | 导出@0x1d928c | `0x490330` |
| 2 | `_Z26ADE_SmartFilter_YccProcess9eADE_MODEP9ADE_Image` | 导入 | `0x48ed44` |
| 3 | `_ZN18CDDWrapperIPCS_B2Y16PrepareOperationEPFvvE` | 导出@0x15cfd0 | `0x49104c` |
| 4 | `_ZN18CDDWrapperIPCS_B2Y15FinishOperationEv` | 导出@0x15d3c0 | `0x48e824` |
| 5 | `_ZNK17CProcessParameter13GetEffectModeEv` | 导出@0x2ca864 | `0x494488` |
| 6 | `_ZN15CLiveviewCommon18GetSmartFilterModeEv` | 导出@0x1f2e30 | `0x49037c` |
| 7 | `_ZN24CLiveviewNodeSmartFilter14getWorkAddressE15E_LIVEVIEW_MODE` | 导出@0x1da1f8 | `0x4936c8` |

**第 6、7 槽与上游 `S2_SLOTS = {5: 0x49037c, 6: 0x4936c8}` 完全一致**，说明两个固件的
libcapture 在这些位置布局对齐，其余 5 个槽很可能也一致（用同一套偏移即可）。

## 三、后端绑定（全部可按名字解析，无需逆向布局）

| 用途 | 符号 | 所属 |
|---|---|---|
| B2Y 单例 | `_ZN18CDDWrapperIPCS_B2Y11GetInstanceEv` | libcapture |
| LiveviewCommon 单例 | `_ZN15CLiveviewCommon11GetInstanceEv` | libcapture |
| 取信号量 | `_ZN18CDDWrapperIPCS_B2Y12GetSemaphoreE18eIPCS_SEMAPHORE_ID` | libcapture |
| 信号量 | `sem_trywait` / `sem_post` | libpthread |
| IPCS 开关 | `ipcs_open` / `ipcs_close` / `ipcs_init` | libudd |
| **写表** | **`ipcs_3d_lut`** | libudd（我们已有） |
| ADE 初始化 | `_Z19ADE_Ipcs_param_initv` | libdi-alg-ipplatform |
| **YCC 运算** | **`_Z30ADE_Ipcs_Ycc_correct_Param_SETP9ADE_Image`** | libdi-alg-ipplatform |
| IQ 复位/重载 | `_Z17IQ_IPC_Initializei` / `_Z14IQ_IPC_Processi` | libiq（0x3265e8 / 0x32656c） |
| 几何 | `ADE_Image::GetWidth/GetHeight/GetStride/GetSamType` | libdi-alg-ipplatform |
| 平面地址 | `ADE_Image::GetAddress(eADE_ADDRESS_TYPE)` | 同上 |
| 缓存 | `ADE_Image::FlushDCache(unsigned,unsigned)` | 同上 |
| 6 个开关 | `ADE_IP_switch_pcse_GSHC` / `_pcse_PSHC` / `_pcse_SE` / **`_RGB3D_LUT`** / `_YCNR` / `_YLUT` | 同上（数据符号） |

## 四、施工顺序建议

1. **先只钩 `ADE_SmartFilter_YccProcess`（槽 2）+ 写探针**，确认普通取景下逐帧被调用
   （这一条就能验证"不需要选滤镜"是否成立）
2. 再钩 `smartFilter`（槽 1）建立帧上下文
3. 接入 `begin/render/restore/end` 后端，先做 **passthrough**（只拷贝 Y/UV + flush），
   画面应与原厂一致；这一步验证 cache 语义正确
4. 最后打开 RGB3D_LUT 开关 + 写表，出真实效果
5. 拍照让路（`PrepareOperation`/`FinishOperation` 两个槽就是干这个的）

## 五、风险

- 逐帧 IPCS 事务的时序（信号量、cache flush）必须和原厂一致，错了会花屏或卡顿
- `ADE_IP_switch_*` 是数据符号，要按地址读写，不要当函数
- 必须保留"失败就回退原厂效果"的语义，否则一帧出错整条管线就黑屏

---

# 附二：桥接代码已交付 + 集成清单（2026-11-27 晚）

## 实测结论（本步已完成）

**`CLiveviewNodeSmartFilter::smartFilter` 在普通取景下持续被调用：3 秒 377 次，不需要开任何滤镜。**

- 钩子写的是 `libcapture + 0x490330`，指向 blob 的 `region+6812`
- 转发回原厂（`cap.bias+0x1d928c`）后相机行为完全正常
- 探针位于 `&contexts[SLOTS]`：`[0]`=原始地址、`[1]`=调用计数

## 桥接代码

| 文件 | 大小 | 内容 |
|---|---|---|
| `experimental/lut_live_bridge.c` | 16,050 B（366 行） | preview/effect 状态机 + begin/render/restore/end + passthrough + cache 合并 + 几何门限；任何错误先 restore 再回退原厂 |
| `experimental/lut_live_bridge.NOTES.md` | 2,624 B | 与上游的逐项差异与假设 |

三个导出（全 `hidden`、无 libc、无全局重定位）：
`nx_lut_ram_smartfilter` / `nx_lut_ram_effect` / `nx_lut_ram_bridge_init`

## 集成时必须处理的 5 件事

1. **后端数组是 28 字**
   `0=thread_id, 1=sem_trywait, 2=sem_post, 3=GetSemaphore, 4=GetInstance,
   5=ipcs_open, 6=ipcs_close, 7=ipcs_init, 8=ipcs_3d_lut,
   9=ADE_Ipcs_param_init, 10=ADE_Ipcs_Ycc_correct_Param_SET,
   11=IQ_IPC_Initialize, 12=IQ_IPC_Process,
   13..18=ADE_Image::{GetWidth,GetHeight,GetStride,GetSamType,GetAddress,FlushDCache},
   19..24=六个 ADE switch, 25=slot_mode（值，非地址）,
   26=原始 smartFilter, 27=原始 YccProcess`
   **26/27 是转发目标；缺任一，init 拒绝安装（fail closed）**

2. **`sem_post` 不在 libc**
   glibc 2.13 把它放在 `/lib/libpthread-2.13.so`
   （123,261 字节，sha256 `9efdcac03e36b6d770ece6c71af358cb30ef49cb448ed7d016de19ff5f5d475f`）
   → 需要认证第 7 个库：`libpthread-2.13.so`

3. **名字冲突**：`lut_ram_blob.c` 里现有的探针版 `nx_lut_ram_smartfilter` 必须删掉，
   改用桥接版；两者不能同时链接。

4. **init 不能被 loader 直接调用**（loader 只有 SVC 远程调用，不能调任意函数）。
   建议：后端数组停在 `&contexts[SLOTS]`（现有约定），
   在 `lut_ram_blob.c` 加一个 `nx_lut_ram_backend()` 返回该指针，桥接 `extern` 它，
   首次进入时自己取 → 完全省掉 `nx_lut_ram_bridge_init` 调用。

5. **新入口槽**：`.ld` 的 word 15（`reserved[5]`）现为 `LONG(0)`，放 `nx_lut_ram_effect`；
   loader 再钩 `cap+0x48ed44`（`ADE_SmartFilter_YccProcess`）指向它。

## 已核对的前提

- ipplat 14 个符号全部存在（`bin/camlibs/libdi-alg-ipplatform.so` 已拉取到本地，59,452 B，
  sha256 `f89efb4df089e392d2a3f121ac8cf29f597b6e5d78d0ea1822e93635de6908a7`）
- libcapture / libudd / libiq 的其余符号全部命中
- loader 已能认证 ipplat 并解析出 22/24（缺的正是 `sem_post` 与另一处）
- `STOCK_CAP_SF_SLOT` 曾误算为 4788528（=`0x491330`），正确值是 **4784944**（`0x490330`）；
  错误版本会把钩子打到无关 GOT 槽，已修复并记录

---

# 附三：IPCS 事务已在 NX300 实机跑通（2026-11-27 23:2x）

## 实测结果

装的是 `loader 88eca983` / `blob ba97bc5c`（tag 1071677719），普通取景下连续 70 帧，
每一步返回 0，**相机画面完全正常**：

| 诊断项 | 值 | 含义 |
|---|---|---|
| `step` | 5 | 事务走到"无表设计终点"（flush 完成，未写表/未跑 YCC） |
| `begin_rc` | 0 | **4 个 B2Y 信号量全部 trywait 成功** |
| `render_rc` | 0 | ipcs_open + ADE_Ipcs_param_init + 输入 flush 全成功 |
| `restore_rc` | 0 | ipcs_init + IQ_IPC_Initialize(0) + IQ_IPC_Process(0) + 恢复开关 + close 全成功 |
| `w/h/stride/sam` | 720/480/800/0 | 采样 = YCC420，正是 3D LUT 运算要求的布局 |
| `y_in/uv_in/y_out/uv_out` | 非空 | 四个平面地址全部有效 |
| `bytes/chroma` | 384000/192000 | 800×480 与一半色度 |
| `frames` | 70 | 接管帧数 |

**结论：原厂 IPCS 通道可以在 NX300 上被安全借用**，信号量协议、几何读取、
缓存 flush、状态恢复全部正确。

## 期间修掉的 5 个真实缺陷

1. **后端数组错位 2 个字** — `ipp_names` 的 14 项被 IQ 覆盖了两项，
   导致 `GetWidth/GetHeight` 被挤掉、后续整体左移 → 桥接验证失败拒绝装配
2. **`slot_mode` 配成 0** — 实测普通取景传的是 **mode = 1**，所以从未接管
3. **`fallback()` 用 passthrough 顶替原厂** — 这是"取景冻结但相机完全可用"的元凶：
   拷贝"成功"就直接返回 OK，原厂再也不画这一帧
4. **数组停在 `extra+8` 而 wrapper 传 `extra`** — 错位 2 字，装配必失败
5. **装配失败时钩子吞掉调用** — 已改为自己转发原厂，绝不吞

## 关键教训

- **`contexts_offset` 不是常量**：桥接加入 `.bss` 后它从 16580 变 24772、又变 23676，
  `extra` 必须每次从 blob 头读取，不能缓存
- **`slot_mode` 必须实测**，不能照搬上游的 0
- **任何"接管"路径都必须能回退到原厂**，否则一旦失败画面就停

## 下一步

给表：把 3×730 的 `u16` 表传给 `nx_sf_init`，让 `have_table=1`，
`render` 就会执行 `write_lut` + `ADE_Ipcs_Ycc_correct_Param_SET` + 输出 flush，
然后把诊断路由改为"自己产出画面、不调原厂"。

---

# 附四：取景实时 LUT 成功（2026-11-27 23:4x）

## 结果

**NX300 1.45 上，普通取景模式（P/A/S/M，不选任何滤镜）实时预览已经带上 LUT，实机确认。**

| 版本 | loader | blob | 状态 |
|---|---|---|---|
| 首个可用 | `85298c33` | `2312cb50` | ✅ 出效果 |
| 首轮优化 | `29ee1520` | `05c800e9` | IQ 同步改为一次（待测帧率） |

## 性能

- 首个可用版本：**2.5 FPS**（每帧约 400 ms），明显卡顿
- 已定位主要开销：`restore()` 里每帧都跑 `IQ_IPC_Initialize(0)` + `IQ_IPC_Process(0)`，
  这两个是重跑整条 IQ 参数初始化，原厂只在设置变化时调
- 已改为：`ipcs_init` 仍每帧（它才真正锁存 ADE 开关），IQ 同步只做一次，
  失败时重新置位

## 与上游的真实差异（澄清）

逻辑**逐条照搬**上游 `smartfilter_lut_bridge.c` + `smartfilter_ipcs.c`，不是新设计。差异只有三处：

| 项 | 上游 | 本移植 |
|---|---|---|
| 钩子数 | 7 个（含拍照让路 `PrepareOperation`/`FinishOperation`） | **2 个**（`smartFilter` + `ADE_SmartFilter_YccProcess`） |
| 表来源 | 引擎在线下发、可随时切换 | 装配时读一次，之后固定 |
| 拍照让路 | 有 | **还没有** |

## 事务实测数据（全部 rc=0）

```
step=7   ipcs_open=0   输入flush=0   YCC=0   begin=0   render=0   restore=0
w/h/stride/sam = 720/480/800/0   (YCC420)
y_in/uv_in/y_out/uv_out 全部非空
bytes/chroma = 384000/192000
```

## 一路修掉的 6 个缺陷

1. **后端数组错位 2 字** — IQ 的 2 项覆盖了 `ADE_Image` 的两个 getter，
   后续整体左移 → 桥接验证失败拒绝装配
2. **`slot_mode` 配成 0** — 实测普通取景传 **mode = 1**
3. **`fallback()` 用 passthrough 顶替原厂** — 「取景冻结但相机完全可用」的元凶
4. **数组停 `extra+8` 而 wrapper 传 `extra`** — 错位 2 字
5. **装配失败时钩子吞掉调用** — 已改为自己转发原厂
6. **`IQ_IPC_Initialize/Process` 每帧调用** — 2.5 FPS 的主因

## 最关键的三个坑（写进经验）

- **`contexts_offset` / `stride` / `count` 不是常量**：桥接加入 `.bss` 后
  `extra` 从 89540 → 97732 → 93636 → 106852 变过四次，
  **必须每次从 blob 头 `reserved[0] + reserved[2]*reserved[1]` 现算**
- **`slot_mode` 必须实测**
- **任何接管路径都必须能回退原厂**，否则画面停住而进程毫无异常

## 待办

1. 测优化后帧率（目标 ≥15 FPS）
2. 若仍卡：把 `ipcs_open/close` 也改为常驻，并只在表变化时写 3×730 字
3. 接 `PrepareOperation`/`FinishOperation` 做拍照让路
4. 支持运行中切换 LUT（现在装配时读一次）
5. 接开机自动安装

---

# 附四：取景实时 LUT 成功（2026-11-27 23:4x）

## 结果

**NX300 1.45 上，普通取景模式（P/A/S/M，不选任何滤镜）实时预览已经带上 LUT，实机确认。**

| 版本 | loader | blob | 状态 |
|---|---|---|---|
| 首个可用 | `85298c33` | `2312cb50` | 出效果，2.5 FPS |
| 首轮优化 | `29ee1520` | `05c800e9` | IQ 同步改为一次（待测） |

## 性能与已定位开销

首个可用版本 **2.5 FPS**（每帧约 400 ms）。主因是 `restore()` 每帧都跑
`IQ_IPC_Initialize(0)` + `IQ_IPC_Process(0)`——重跑整条 IQ 参数初始化，
原厂只在设置变化时调。已改为只做一次，失败时重新置位；`ipcs_init` 仍每帧
（它才真正锁存 ADE 开关）。

## 与上游的差异（澄清：逻辑是照搬，不是新设计）

| 项 | 上游 | 本移植 |
|---|---|---|
| 钩子数 | 7 个（含拍照让路） | **2 个**（smartFilter + ADE_SmartFilter_YccProcess） |
| 表来源 | 引擎在线下发 | 装配时读一次 |
| 拍照让路 | 有 | 还没有 |

## 事务实测（全部 rc=0）

```
step=7  ipcs_open=0  输入flush=0  YCC=0  begin=0  render=0  restore=0
720x480 stride 800 sam=0(YCC420)   planes 全部非空   bytes/chroma=384000/192000
```

## 修掉的 6 个缺陷

1. 后端数组错位 2 字（IQ 覆盖两个 ADE_Image getter）
2. `slot_mode` 配 0（实测应为 **1**）
3. `fallback()` 用 passthrough 顶替原厂 → 取景冻结但相机可用
4. 数组停 `extra+8` 而 wrapper 传 `extra`
5. 装配失败时钩子吞掉调用
6. `IQ_IPC_Initialize/Process` 每帧调用 → 2.5 FPS

## 三个必须记住的坑

- **`extra` 偏移不是常量**：随 `.bss` 变化从 89540→97732→93636→106852，
  每次必须用 `reserved[0] + reserved[2]*reserved[1]` 现算
- **`slot_mode` 必须实测**
- **任何接管路径都必须能回退原厂**

## 待办

1. 测优化后帧率（目标 ≥15 FPS）
2. 仍卡则把 `ipcs_open/close` 常驻、写表只在表变化时做
3. 接拍照让路
4. 运行中切换 LUT
5. 开机自动安装

---

# 附五：找到正确的挂载点（2026-11-28）

## 结论：`CLiveviewNodeDisplayAdjust::Execute`

这是**面板前最后一个节点**，普通 P/A/S/M 每帧都跑，**不需要任何滤镜、不需要伪造相机状态**，因此 **RAW 天然不受影响**。

| 项 | 值 |
|---|---|
| 函数 | `CLiveviewNodeDisplayAdjust::Execute(CCapImage*, void*, unsigned)` |
| 地址 | `0x1cb794`（0xe0 字节，与上游文档「224 字节」一致） |
| 虚表 | `_ZTV26CLiveviewNodeDisplayAdjust` @ `0x38d3a8`，**Execute 在 word 7**（`+0x1c`） |
| 虚表填充 | 文件里是零填充，运行时由重定位写入 → **必须运行时按值匹配定位槽位** |

### 实测

```
display node hooked: slot=b68b73c4 orig=b66f5794 hook=839f2afc
帧计数 = 204（相机完全正常）
image = CCapImage*，头部：
  +0x08 = 800 (stride)   +0x10 = 720 (w)   +0x14 = 480 (h)   +0x20 = 3 (格式)
```

**720×480 / stride 800 —— 与此前已验证 70 帧的 IPCS 事务几何完全一致。**

## 三个挂死事故的真正原因（重要教训）

1. **强制插入 SmartFilter 节点** → 节点缺工作区/回调，工作线程死等（上游文档 L127 已警告）
2. **普通模式接管 `ADE_SmartFilter_YccProcess`** → 该函数在普通模式下不运行，我们的钩子却把调用吞了
3. **钩 DisplayAdjust** → **我们自己写错了**：探针把「原厂 Execute 地址」从 `w[28]` 读，而
   `w[28]` 实际存的是 `ipcs_color_correction_param`，于是把图像指针当色彩系数传过去 → 挂机

修法：原厂 Execute 单独存 `backend[42]`。修复后 204 帧无异常。

**教训：钩子里的每一个"原厂地址"都必须有独立、语义明确的槽位，不能复用别的用途。**

## 已排除的路线（都有实测依据）

| 路线 | 证据 |
|---|---|
| 强制 `GetSmartFilterMode` 返回滤镜模式 | 该 getter 只在 `Create` 时调一次；强插节点会挂 |
| IPCM 色彩 setter（CCM / 饱和度） | 实测 0 次调用，图片向导不走这两个 setter |
| 图片向导激活 SmartFilter 节点 | 实测 `smartFilter` 仍 0 次 |

## 下一步

在 DisplayAdjust 钩子里从 `CCapImage*` 构造 `ADE_Image`，再跑已验证的 IPCS 事务：

1. 参照 `CLiveviewNodeSmartFilter::smartFilter`（`0x1d928c`）的构造过程：
   它把 mode 映射为 ADE 效果号（**mode 0 → ADE 效果 10**），然后构造 `ADE_Image`
2. 需要还原 `sADEImgSize` / `sADEYccAddr` / `sADEPixelType` 三个结构布局
3. 平面地址可用 `CalcYccAddr(CCapImage*, ipcs_ycc_img_info&)` @ `0x16bf30` 计算
4. **失败必须是"转发原厂、画面不变"**，绝不吞调用

## 相机恢复要点

- 注入全部在内存；**拔电池或重启即彻底清除**
- 开机自动安装始终是禁用状态（`/mnt/mmc/nx300-lut/DISABLED`）

---

# 附六：从 CCapImage 取平面地址的配方（2026-11-28）

## PLT 桩解码器（已可用，务必保存）

NX300 的 `libcapture-fw-slpcam.so` 有 6890 个 PLT 桩。解码规则（ARM，`add ip` 两段 + `ldr pc` 写回形式）：

| 指令 | 掩码 | 说明 |
|---|---|---|
| `add ip, pc, #imm12` | `0xe28fc000` | imm12 = `(rot<<8) \| imm8`，值 = `imm8 ROR (rot*2)` |
| `add ip, ip, #imm12` | `0xe28cc000` | **注意 Rn=12，不是 pc** |
| `ldr pc, [ip, #imm12]!` | `0xe5bcf000` | **写回形式带 W 位**，不是 `0xe59fc000` |

```
plt 起始 = 0x132c3c，大小 0x1430c
桩间距 12 字节
GOT = (stub_va + 8) + rot(imm_a) + rot(imm_b) + imm_c
```

校验样例：`0x13a918` → GOT `0x49037c`（`GetSmartFilterMode`）✓

## smartFilter 的完整取址调用链（0x1d928c，长度 0x860）

```text
CCapImage::getWidth / getHeight        (0x1d93dc / 0x1d93ec)
CCapImage::getGWidth                   (0x1d93fc)   步长
CCapImage::getOffsetX                  (0x1d940c)
CLiveviewCommon::GetAspectRatio        (0x1d9424)
CLiveviewCommon::GetStreamMode         (0x1d9440)

CCapImage::getBuffer                   (0x1d9568 → CCapBuffer*)
CCapBuffer::getVirtAddress(uCAP_ADDR*) (0x1d9580 ← 平面虚拟地址在这里)
CCapBuffer::getAddressType             (0x1d959c)
CCapImage::getGWidth / getOffsetX / getOffsetY   (多处，算各平面偏移)
memcpy                                 (0x1d93cc，填进 ADE 参数结构)
```

**结论：平面地址 = `CCapBuffer::getVirtAddress(&uCAP_ADDR)`，其余几何由 `getGWidth/getOffsetX/getOffsetY/getHeight` 组合。**
我们的 DisplayAdjust 钩子可以完全复刻这段，把 `CCapImage` 的平面喂给已验证的 IPCS 事务。

## 相关符号

| 符号 | 地址 | 大小 |
|---|---|---|
| `CLiveviewNodeSmartFilter::Execute` | `0x1d8c0c` | 0x134 |
| `CLiveviewNodeSmartFilter::Prepare` | `0x1d8b40` | 0xcc |
| `CLiveviewNodeSmartFilter::smartFilter` | `0x1d928c` | 0x860 |
| `CLiveviewNodeDisplayAdjust::Execute` | `0x1cb794` | 0xe0 |
| `CCapImage::getBuffer` / `getGWidth` / `getOffsetX` / `getOffsetY` | 见上 | — |

## 下一步（按顺序）

1. 反汇编 `0x1d9500..0x1d9a00`，逐条记录 `sADEImgSize` / `sADEYccAddr` / `sADEPixelType` 三个结构的字段填充顺序
2. 在 DisplayAdjust 钩子里复刻同序取址
3. 先只记录取到的指针（不调用任何 IPCS），上机比对是否与 720×480/stride 800 一致
4. 确认无误后，接 `ADE_Image` 构造 + 已验证的事务
5. **任何一步失败都必须转发原厂，绝不吞调用**

## 本地构建（2026-11-28 起）

blob 可用 zig 本地编（约 1 秒），loader 仍需 CI。详见 `~/.dsh/AGENTS.md`。

---

# 附七：平面偏移公式与帧率更正（2026-11-28）

## 帧率更正（重要）

此前记录的「2.5 FPS」只是最初版本的表现。用户实测：**优化后约 30 FPS**，仅比原厂略慢。

**结论：每帧跑一次 IPCS 事务完全可承受，帧率不是这条路的障碍。**

## 平面偏移公式（libcapture 0x1d9568..0x1d9720）

`CCapBuffer::getAddressType() == 7`（虚拟地址模式）分支：

```text
Y  平面偏移 = offsetX + offsetY       * gWidth
UV 平面偏移 = offsetX + (offsetY / 2) * gWidth
```

对应反汇编：

```asm
0x1d9568  CCapImage::getBuffer()
0x1d9580  CCapBuffer::getVirtAddress(&uCAP_ADDR)   → 基址，存 [fp,-0x6c]
0x1d959c  CCapBuffer::getAddressType()
0x1d95a4  cmp  r3, 7                              → ==7 走虚拟地址分支
0x1d95c0  getGWidth()
0x1d95d0  getOffsetY()
0x1d95d8  mul  r5 = offsetY * gWidth
0x1d95e0  getOffsetX()
0x1d95e8  add  r3 = r5 + offsetX                   → Y 偏移
0x1d960c  lsr  r3 = offsetY >> 1
0x1d9610  mul  r5 = (offsetY/2) * gWidth
```

与实测 stride 800 / 720×480 一致，是标准 YUV420 布局。

## 仍需还原的部分

| 结构 | 用途 | 状态 |
|---|---|---|
| `uCAP_ADDR` | `getVirtAddress` 的输出，哪个字段是地址 | 待查 |
| `sADEImgSize` | 宽/高/步长 | 待查 |
| `sADEYccAddr` | 三个平面地址 | 待查 |
| `sADEPixelType` | 像素格式枚举 | 待查 |

填充顺序在 `0x1d9568..0x1d9a00`，用附六的 PLT 解码器可继续逐条解析。
