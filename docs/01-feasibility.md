# NX300 1.45 机内 LUT 可行性报告

> 生成时间：2026-11-21
> 分析对象：`lut_live_loader.arm`（NX300M 1.15 专用）vs 实机 NX300 1.45
> 方法：REA/Ghidra 反汇编 loader + 实机只读验证（root shell，未做任何写入）

---

## 一、结论速览

| 问题 | 答案 |
|---|---|
| rc=75 是什么？ | **不是"找不到原厂函数"**，是 `main()` 里 **Liveview admission 超时（12000 ms）** |
| 相机 LUT 机制存在吗？ | **完整存在**，所有 hook 落点都能定位 |
| 现有 loader 能改常量修好吗？ | **不能**，判据结构与目标模块层级不同 |
| 技术上能做吗？ | **能**，但要重写 loader（或由作者出 NX300 版） |
| 风险？ | 只动内存/SD 卡，崩溃可重启，**拔卡即回原厂** |

---

## 二、rc=75 的真实来源

`lut_live_loader.arm` 的 `main()`（0x10e18）apply 分支：

```c
lVar20 = now_ms();
/* deadline = now + 12000 ms */
LAB_00011324:                       /* 每 50ms 重试一轮 */
  open("/proc/<pid>/mem")
  pread64(fd, &v, 4, 0x4c9c5c);  if (v != 0x24c0b8) → 失败
  pread64(fd, &v, 4, 0x4c9c64);  /* 期望 0x4c9f04 或 0x4da0f0 */
  pread64(fd, &v, 4, 0x4c9f04);  if (v != 0x24c838) → 失败
  pread64(fd, &v, 4, 0x24c86c);  /* 期望 0x220de4 或 7 */
  liveview_comm_count(pid, "videosrc", &n1);
  liveview_comm_count(pid, "udd_intr_handle", &n2);
  ...
  nanosleep(50ms);

if (超时) {
    fprintf(stderr, "%s Liveview admission timed out after %u ms; no camera thread attached.\n",
            "RAM LUT", 12000);
    return 0x4b;                    /* == 75 == EX_TEMPFAIL */
}
```

**日志中的 12.7 秒耗时与此完全对应**（12 s 等待 + 进程开销）。

### 实机读数（NX300 1.45，pid=265）

| 地址 | 实际值 | loader 期望 | |
|---|---|---|---|
| `0x4c9c5c` | `0x00000000` | `0x24c0b8` | ✗ |
| `0x4c9c64` | `0x00000000` | `0x4c9f04`/`0x4da0f0` | ✗ |
| `0x4c9f04` | `0x00000000` | `0x24c838` | ✗ |
| `0x24c86c` | `0x0021e194` | `0x220de4`/`7` | ✗ |

**根因**：`di-camera-app` 在 NX300 上的映射只有

```
00008000-00295000 r-xp
0029c000-00322000 rw-p        ← rw 段只到 0x322000
```

loader 要读的 `0x4c9c5c` / `0x4c9f04` 在 NX300 上**根本没有映射**（返回 0）。

### 例外：线程条件其实满足

```
videosrc        × 1     ✓ loader 要求存在
udd_intr_handle × 2     ✓ loader 要求 > 1
```

即 NX300 的取景线程结构完全正常，只是 loader 查错了内存地址。

---

## 三、loader 的完整指纹锁（即使过了门控也过不去）

### 3.1 `dynamic_abi()` —— .rel.plt 精确指纹

```c
/* 遍历目标 ELF 的 .rel.plt */
if (r_offset == 0x3c2ab4 && r_type == R_ARM_JUMP_SLOT)
    要求 memcmp(name, "ipcs_3d_lut", 12) == 0，且全表恰好 1 个
if (r_offset == 0x3bfd08 && r_type == R_ARM_JUMP_SLOT)
    要求 memcmp(name, "_ZN17CIpcParameterNode6UpdateEPv", 32) == 0，且恰好 1 个
```

### 3.2 `admission_file_hash()` —— SHA256 白名单

### 3.3 `stock_writer_slot()` —— GOT 槽双态校验

```c
if (udd_base + 0x955c8 == slot) return 1;   /* 已解析 */
if (slot != iq_base + 0x573a8)   return 0;  /* 未解析：应指向 IQ PLT0 */
return code_check(...);                      /* 校验 PLT0 桩字节 */
```

### 3.4 实机对照总表

| 判据 | NX300M 1.15 期望 | NX300 1.45 实测 | 结果 |
|---|---|---|---|
| `di-camera-app` sha256 | `e820df7c…` | `05fe6c9e…` | ✗ |
| IQ 库文件名 | `libiq-fw-slpcam.so` | `libiq-fw-slpcam.so` | ✓ |
| `libiq-fw-slpcam.so` sha256 | `43641c84…` | `430b7df1…` | ✗ |
| `libudd.so.3.1` sha256 | `4c2d05ba…` | `694fc7e7…` | ✗ |
| `libc-2.13.so` sha256 | `a3605b6e…` | `a3605b6e…` | **✓** |
| `ipcs_3d_lut` 函数内偏移 | `0x955c8` | `0x955c8` | **✓** |
| `ipcs_3d_lut` GOT 槽 | `0x3c2ab4` | `0x3c1aa8` | ✗ |
| `CIpcParameterNode::Update` 槽 | `0x3bfd08` | `0x3bed08`（虚表项） | ✗ |
| IQ `.plt` 起始（PLT0） | `0x573a8` | `0x57300` | ✗ |
| Liveview bss 门控 | 4 地址 + 魔数 | 无映射 | ✗ |

---

## 四、决定性好消息：NX300 的 LUT 机制完整

### 4.1 `libiq-fw-slpcam.so`（NX300 版，430b7df1，3,993,368 B）含完整 `CIpcParameterNode`

```
dynsym 4960 项，其中：
  _ZTV17CIpcParameterNode            vtable  @ 0x3bf0c0  size 32
  _ZN17CIpcParameterNode6UpdateEPv   Update  @ 0x350a88  size 76
  _ZN17CIpcParameterNodeC1Ev / C2Ev / D1Ev / D2Ev
  _ZN17CIpcParameterNode8SetParamEj
  _ZN17CIpcParameterNode11SetSubIndexERjij
  _ZN17CIpcParameterNode11GetSubIndexEji
  _ZN17CIpcParameterNode12MakeSubIndexEiz
  _ZN17CIpcParameterNode12GetMakerNoteEPi
  _ZN17CIpcParameterNode15SetPreviousNodeEPS_
  _ZN17CIpcParameterNode11SetNextNodeEPS_
  _ZN17CIpcParameterNode5ResetEv
  _ZN17CIpcParameterNode8GetIndexEPv
  _ZN17CIpcParameterNode14GetSubIndexMaxEi
  _ZN17CIpcParameterNode12DebugProcessEiPPc
```

**19 个 ABS32 虚表项指向 `Update`**（.rel.dyn `0x3bed08`…`0x3bf0c8`）。

### 4.2 实机运行时验证（pid=265）

| 项目 | 地址 | 实测值 | 解读 |
|---|---|---|---|
| libiq 基址 | `0xb4472000` | — | `.plt` @ `0x57300`，`.got` @ `0x3bf1e8` |
| `CIpcParameterNode` 虚表项 | `0xb4830d08` | `0xb47c2a88` | = `libiq + 0x350a88` = **`Update` 函数实体** ✓ |
| `ipcs_3d_lut` GOT 槽 | `0xb4833aa8` | `0xb44c9300` | = `libiq + 0x57300` = **自身 PLT0（尚未 lazy 解析）** ✓ |
| `Update` 首指令 | `libiq+0x350a88` | `00 30 90 e5` = `ldr r3,[r0]` | 正常 ARM 函数 ✓ |
| `ipcs_3d_lut` 首指令 | `libudd+0x955c8` | `10 48 2d e9` = `push {r4,fp,lr}` | 函数实体存在 ✓ |

**关键**：`libiq` 的 rw 段 `0xb4830000-0xb4849000` 覆盖虚表项和 GOT 槽 → **两者都可写**。

### 4.3 libudd 侧完整

```
ipcs_3d_lut              0x955c8  (240 B)   ← 与 NX300M 完全同偏移
ipcs_hl_3d_lut_onoff     0x61bd0  (128 B)
SetManualLut             0xa49fc  (264 B)   ← 灰度 LUT 生成器，非 3D
RW_LUT / BW_LUT / GW_LUT 0x127d58 / 0x127d7c / 0x127d88
```

libudd 内含 `"IPCS DD has not been opened yet"` 检查，说明 `ipcs_3d_lut` 有前置条件。

---

## 五、为什么不能"只改常量"

差值看似规律，但结构不同：

```
0x3c2ab4 → 0x3c1aa8   Δ 0x100C
0x3bfd08 → 0x3bed08   Δ 0x1000
0x573a8  → 0x57300    Δ 0xA8
0x955c8  → 0x955c8    Δ 0
```

真正的阻塞不在数值，而在**层级**：

1. **NX300M：hook 在主程序** `di-camera-app`
   `ipcs_3d_lut` 与 `CIpcParameterNode::Update` 是主程序的 **JUMP_SLOT import**
2. **NX300：hook 必须在 `libiq` 库内**
   - `ipcs_3d_lut`：libiq 的 JUMP_SLOT @ `0x3c1aa8`
   - `CIpcParameterNode::Update`：libiq **自己定义**（`st_shndx=12`），只有虚表 ABS32，**没有 JUMP_SLOT**

因此：

- `dynamic_abi()` 扫描主程序 `.rel.plt` → NX300 主程序 977 条里**这两个符号一条都没有** → 直接返回 false
- `stock_writer_slot()` 的 `udd/iQ` 双态判据针对主程序 GOT 槽，NX300 目标槽在 libiq
- Liveview 门控 bss 布局差异过大（NX300 的 rw 段只到 `0x322000`）

**结论：loader 的判定链与目标模块是硬绑定的，二进制打补丁无法修复。**

---

## 六、可行路线

### 路线 A（推荐，最省力）：找作者要 NX300 版
作者 `YeJianbo/nx300m-mod` 有 loader 与 blob 源码。改 6 组常量 + 换 hook 模块即可出 NX300 版：

```
liveview 门控  → NX300 的 bss（需重新定位，或按线程条件放宽）
dynamic_abi    → 改为解析 libiq，槽位 0x3c1aa8
                 Update 改为读虚表项 0x3bed08，不要求 JUMP_SLOT
admission_hash → 三个新哈希
stock_writer   → iq_base + 0x57300
```

### 路线 B（自己实现）：重写注入器，复用 blob
`lut_ram_blob.bin` **不含任何硬编码目标地址**（已全表扫描确认），是位置无关、由 loader 传入 196 字节配置的代码。理论上可复用。

但要自己实现：
1. ptrace attach + 停驻全部线程
2. remote syscall（需在目标 `.text` 找 `svc 0` gadget）调 `mmap2` / `mprotect`
3. 写入 blob + 填配置结构
4. 改 libiq 虚表项 / GOT 槽
5. 崩溃保护与恢复

**缺 ARM 交叉编译工具链**（本机无，相机只有 perl/strace，且用户要求不在相机编译）。需要先在 D 盘装 `arm-linux-gnueabihf` 工具链。

### 路线 C：放弃 loader，走 LD_PRELOAD
需要改 `di-camera-app` 的启动环境 → 涉及系统分区，**违反安全红线，不建议**。

---

## 七、安全边界（已验证可恢复）

- 本轮**全程只读**：仅 `cat`/`grep`/`dd` 读取，未做任何写入
- 已从相机取回（只读下载）：`/usr/lib/libiq-fw-slpcam.so`、`/usr/bin/di-camera-app-nx300`
- 曾临时在 `web_root` 放过两个副本，**已删除**
- 相机 root shell 无需认证（telnet 23，无 yama），可在内存层做实验
- 任何内存改动：进程崩溃 → 重启恢复；**拔 SD 卡 → 完全回原厂**

---

## 八、复现用命令

```sh
# 相机（只读）
sha256sum /usr/bin/di-camera-app-nx300 /usr/lib/libiq-fw-slpcam.so \
          /usr/lib/libudd.so.3.1 /lib/libc-2.13.so
grep -E 'libiq|libudd' /proc/$(pidof di-camera-app-nx300)/maps
for t in /proc/$(pidof di-camera-app-nx300)/task/*/comm; do cat $t; done | sort | uniq -c

# 读门控地址（会返回 0，证明未映射）
dd if=/proc/265/mem bs=1 skip=$((0x4c9c5c)) count=4 2>/dev/null | hexdump -C

# 读 libiq 虚表项（应为 libiq+0x350a88）
dd if=/proc/265/mem bs=1 skip=$((0xb4472000+0x3bed08)) count=4 | hexdump -C
```

---

## 九、待办

- [ ] 决定路线 A / B
- [ ] 若走 B：在 D 盘安装 ARM 交叉工具链
- [ ] 若走 B：定位 NX300 的 Liveview bss 状态（或改为仅线程判定）
- [ ] 逆向 `lut_ram_blob.bin` 的 header/配置格式（196 B）
- [ ] 确认 `ipcs_3d_lut` 的参数格式（240 B 函数体）
