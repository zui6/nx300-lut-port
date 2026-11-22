# NX300 机内 LUT 移植（NX300M mod → NX300 1.45）

把 [YeJianbo/nx300m-mod](https://github.com/YeJianbo/nx300m-mod) 的机内 3D LUT
后端从 **Samsung NX300M 固件 1.15** 移植到 **NX300 固件 1.45**。

> 本仓库只包含**移植工作本身**：源码改动、构建配置、编译产物、分析与报告。
> 不含任何 Samsung 固件、库或 LUT 数据文件。

---

## 现状（2026-11-22）

| 环节 | 状态 |
|---|---|
| 原厂模块指纹（libiq / libudd / libc / 主程序） | ✅ 通过 |
| `dynamic_abi` / `code_bytes` 校验 | ✅ 通过 |
| RAM blob admission | ✅ 通过 |
| ptrace attach + 远程 `mmap2` | ✅ 通过 |
| 写 GOT / vtable | ⚠️ **执行后相机崩溃重启** |
| LUT 实际生效 | ❌ 未验证 |

**相机没有变砖**：所有改动只落在 SD 卡、`/tmp`（tmpfs）和运行时内存，
崩溃后重启即恢复；拔掉 SD 卡完全回到原厂状态。

---

## 关键发现

1. **`rc=75` 不是"找不到原厂函数"**
   loader 的 `main()` 里有一段 **Liveview admission（12 秒超时）**，
   它读取 NX300M 专属的 `.bss` 地址（`0x4c9c5c` / `0x4c9c64` / `0x4c9f04`）。
   NX300 的 `di-camera-app` 根本没有这些映射，读到 0，于是空转 12 秒返回 75。

2. **文件大小本身也是指纹**
   | 文件 | NX300M 期望 | NX300 实际 |
   |---|---|---|
   | `libiq-fw-slpcam.so` | 3,997,488 | 3,993,368（差 4,120）|
   | `di-camera-app-nx300` | 3,231,956 | 3,217,720（差 14,236）|

3. **`ipcs_3d_lut` 的库内偏移完全相同**（`0x955c8`），
   说明两个固件共享同一套 UDD 实现。

4. **相机有 POSIX 定时器**，`PTRACE_SYSCALL` 之后可能先收到**实时信号 42**，
   而不是 syscall-entry trap。原 loader 只接受 `SIGTRAP|0x80`，必须重试。

---

## NX300 常量表

| 符号 | NX300M 1.15 | **NX300 1.45** |
|---|---|---|
| `_ZN17CIpcParameterNode6UpdateEPv` | `0x350dc0` | **`0x350a88`** |
| `_ZN17CIpcParameterNode5ResetEv` | `0x350e0c` | **`0x350ad4`** |
| `ipcs_3d_lut`（libudd） | `0x955c8` | **`0x955c8`** |
| `_ZTV12CIpcs3DLutD4`（vtable） | — | **`0x3bed00`** |
| vtable slot（`+8`） | `0x3bfd08` | **`0x3bed08`** |
| `ipcs_3d_lut` GOT 槽 | `0x3c2ab4` | **`0x3c1aa8`** |
| `.plt`（PLT0 resolver） | `0x573a8` | **`0x57300`** |
| `_ZN12CIpcs3DLutD47m_sRLutE` | — | **`0x36aec8`** |
| libc 内 `svc` 地址 | — | **`0xc12dc`** |

由 `tools/gen_stock.py` 从原始 ELF 自动生成 `build/lut_live_stock.h`。

---

## 目录

```
build/
  lut_live_loader.c              # 改写为 NX300 地址的 loader
  live_loader_core.h             # 含两处修复：liveview 判定、syscall_stop
  lut_live_stock.h               # 自动生成的指纹常量
  crt0_arm.S                     # 自写 ARM 入口（原仓库缺此文件）
  lut_ram_blob.h                 # blob 结构定义
  lut_ram_blob_build.h           # blob 构建 ID
  smartfilter_admission_cache.h
  build.yml                      # GitHub Actions 工作流（含 ABI 探针）
  lut_live_loader.arm            # 编译产物（源码级移植版）
  lut_live_loader_binarypatch.arm# 另一条路线：直接 patch 原二进制的产物
docs/
  01-feasibility.md              # 机理分析：rc=75 真因、指纹锁全景
  02-binary-patch.md             # 二进制补丁路线（16 处 / 227 字节）
  03-source-port.md              # 源码级移植
  04-injection.md                # ptrace 注入打通与当前卡点
tools/
  gen_stock.py                   # 从 ELF 生成 lut_live_stock.h
  make_binary_patch.py           # 生成二进制补丁版 loader
```

---

## 构建

需要 ARM 交叉工具链（`arm-linux-gnueabi-gcc`）与相机上的 `libc-2.13.so`：

```bash
arm-linux-gnueabi-gcc \
  -std=gnu99 -O2 -Wall -Wextra \
  -march=armv7-a -mfloat-abi=soft \
  -D_TIME_BITS=32 -D_FILE_OFFSET_BITS=64 \
  -fno-stack-protector -fno-pic -no-pie \
  -nostdlib -nostartfiles -Wl,-e,_start \
  -Wl,--dynamic-linker=/lib/ld-linux.so.3 \
  -Wl,-z,defs -I. \
  lut_live_loader.c crt0_arm.S ./libc-2.13.so -lgcc \
  -o lut_live_loader.arm
```

`libc-2.13.so` 需自行从相机 `/lib/libc-2.13.so` 提取（sha256
`a3605b6ea309ef9fd5db3c30f85482a1fca4b38393e9c98a3be8a2bad284bb13`）。

`build/build.yml` 给出了 GitHub Actions 上可复现的构建方式，
并含一个用 qemu 实测 `struct timespec` / `syscall(263)` ABI 的探针。

---

## 下一步

崩溃说明「写进去了但写错了」。建议二分定位：

1. 只 mmap、**不改槽**的 dry-run —— 确认注入本身不伤相机
2. 只改 GOT（`ipcs_3d_lut` 槽），不动 vtable
3. 再动 vtable

最可疑的是第 3 步：我们沿用了 NX300M 的 `_ZTV12CIpcs3DLutD4 + 8` 取 `Update`，
但 NX300 的虚表布局未必相同，需要核对实际内容。

---

## 安全边界

- **绝不动系统分区**（`/usr` `/etc` `/lib` `/bin`）：本工作全程零写入
- 写入位置仅：SD 卡 `/mnt/mmc`、tmpfs `/tmp` 与 `/var/run`、目标进程运行时内存
- 崩溃/黑屏：重启即恢复；拔 SD 卡完全回原厂

---

## 致谢

移植目标与全部设计归功于 [YeJianbo/nx300m-mod](https://github.com/YeJianbo/nx300m-mod) 的作者。
本仓库只是把它推到另一台相机上。
