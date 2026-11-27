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
