# NX300 LUT 移植 —— 第三阶段（源码版）

> 时间：2026-11-22 01:55
> 状态：**认证链 + blob 全通，卡在 ptrace 远程 syscall（ABI 不匹配）**

---

## 一、本轮重大进展

### 1.1 拿到作者源码
`YeJianbo/nx300m-mod`（main 分支，1658 文件）已克隆到 `repo/nx300m-mod/`。

关键文件：
- `runtime/lut_live_loader.c` —— loader（528 行）
- `runtime/lut_ram_blob.c` / `.h` —— RAM blob
- `runtime/live_loader_core.h` —— ptrace 注入核心（313 行）
- `runtime/lut_live_stock.h` —— **自动生成**的指纹常量（原仓库不含）

### 1.2 NX300 常量表（已从实机 ELF 提取）

| 常量 | NX300M 1.15 | **NX300 1.45** |
|---|---|---|
| `_ZN17CIpcParameterNode6UpdateEPv` | 0x350dc0 | **0x350a88** |
| `_ZN17CIpcParameterNode5ResetEv` | 0x350e0c | **0x350ad4** |
| `ipcs_3d_lut`（UDD） | 0x955c8 | **0x955c8**（相同） |
| `_ZTV12CIpcs3DLutD4`（vtable） | — | **0x3bed00** |
| vtable_slot（= vtable+8） | 0x3bfd08 | **0x3bed08** |
| `ipcs_3d_lut` GOT 槽 | 0x3c2ab4 | **0x3c1aa8** |
| `.plt`（PLT0 resolver） | 0x573a8 | **0x57300** |
| `_ZN12CIpcs3DLutD47m_sRLutE`（factory red） | — | **0x36aec8** |
| libc `syscall` 内 svc 地址 | — | **0xc12dc** |

文件指纹：

| 文件 | size | sha256 |
|---|---|---|
| di-camera-app-nx300 | 3,217,720 | `05fe6c9e…8f78` |
| libiq-fw-slpcam.so | 3,993,368 | `430b7df1…4b76` |
| libudd.so.3.1 | 1,018,360 | `694fc7e7…dfba` |
| libc-2.13.so | 1,197,388 | `a3605b6e…bb13`（与 NX300M 相同） |

生成脚本：`tmp/gen_stock.py` → 产出 `work/nx300build/lut_live_stock.h`

### 1.3 源码级修改（`work/nx300build/`）
- `lut_live_loader.c`：替换 4 组硬编码地址
  - `0x3c2ab4`→`0x3c1aa8`（8 处）
  - `0x3bfd08`→`0x3bed08`（12 处）
  - `0x350dc0`→`0x350a88`（12 处）
  - `0x350e0c`→`0x350ad4`（5 处）
- `live_loader_core.h`：`liveview_ready_snapshot()` 重写为**仅线程判定**
  （NX300 无 `0x4c9c5c`/`0x4c9f04` 这套 bss 链；`videosrc=1, udd_intr_handle=2` 已满足）
- 新增 `crt0_arm.S`（原仓库缺 `work/reference/.../crt0_arm.S`）

### 1.4 GitHub Actions 云端编译成功
- 仓库：https://github.com/zui6/nx300-lut-loader-build（private）
- workflow：`.github/workflows/build.yml`，`ubuntu-latest` + `apt install gcc-arm-linux-gnueabi`
- run id `44931451815`，**success**
- 产物：`lut_live_loader.arm` 71,700 B，sha256 `b20ea547…50aa`
- ELF：`type=EXEC mach=ARM entry=0x176a4 interp=/lib/ld-linux.so.3` ✓

---

## 二、实机验证结果

### 2.1 `status` —— **全部通过，EXIT=0**

```
maps_read:true  old_module_conflict:false
stock_iq_sha256:true   stock_udd_sha256:true
dynamic_abi:true       code_bytes:true
memory_errno:0
iq_bias=0xB4511000  udd_bias=0xB58E1000
slot_update = iq_bias+0x350A88 = Update 函数
slot_lut    = iq_bias+0x57300  = 未解析 PLT0
```

**这证明整张常量表与 NX300 实机完全对应。**

### 2.2 `apply` —— blob 通过，卡在 mmap2

```
Still control admission: ... camera running        ← 认证全过
Remote syscall 192 failed: original pc=b55a71ec r0=00000012 r7=142
   orig_r0=00000012; entry pc=00000000 r0=00000000 r7=0
   latest pc=00000000 r0=00000000 r7=0
Still target stop window: 455266533376 ms
Admission/syscall/patch failed at mmap2 (errno=2)
```

`192` = ARM `__NR_mmap2`。`entry` 全 0 说明 `syscall_stop()` 返回 false
（`PTRACE_SYSCALL` 后没等到 `SIGTRAP|0x80`），`check` 未被赋值。

### 2.3 已确认的 ABI 不匹配证据

```
elapsed_ms=8589934592          (= 2^33)
target stop window=455266533376 ms
```

`core_now_ms()` 用 `syscall(263, CLOCK_MONOTONIC, &ts)` + `struct timespec`。
数值异常说明 **编译头文件（Ubuntu GCC 的 glibc 头）与相机 `libc-2.13.so`
在结构体布局上不一致**。

作者原构建用 `/opt/nx300m-toolchain`（GCC 14 + 配套 2.13 头文件），
本机无 WSL、无该工具链，只能用 Ubuntu 的 `gcc-arm-linux-gnueabi`，
`-D_TIME_BITS=32 -D_FILE_OFFSET_BITS=64` 未能完全消除差异。

**同一原因很可能导致 ptrace 寄存器/等待状态解析异常（sig=42 而非 0x85）。**

---

## 三、两条路线的现状对比

| | 二进制 patch 版（SD 包预编译） | 源码编译版（GitHub Actions） |
|---|---|---|
| ABI | ✓ 正确（作者工具链） | ✗ 不匹配（时间/结构体） |
| 认证链 | ✓ 全通过 | ✓ 全通过 |
| blob admission | ✗ `Invalid RAM blob/header` | ✓ **通过** |
| mmap2 注入 | 未到达 | ✗ `syscall_stop` 失败 |

**互补关系**：二进制版 ABI 对但 blob 逻辑旧；源码版逻辑新但 ABI 错。

---

## 四、下一步（按可行性排序）

### 路线 1：找与 SD 包 blob 匹配的**旧版**源码
SD 包是 `preview.12`，GitHub main 是最新。若 checkout 到对应 tag/commit，
用其 `read_blob` 逻辑编译，可解决 blob 问题——但仍需 ABI 匹配。

### 路线 2：解决 ABI（推荐）
- 在 GitHub Actions 里用 **Ubuntu 20.04**（glibc 2.31 头，更接近 2.13）
- 或安装 `libc6-dev-armel-cross` 的 2.13 头
- 或用 `arm-linux-gnueabi-gcc` + 显式 `-isystem` 指向 2.13 头文件
- 验证手段：编译一个最小程序，在相机上打印 `sizeof(struct timespec)` 和
  `clock_gettime` 结果，与本地比对

### 路线 3：绕开 `struct timespec`
把 `core_now_ms()` / `now_ms()` 改为**不依赖结构体布局**的实现
（例如用 `gettimeofday` 的 8 字节手工解析，或直接用 `uptime` 读数）。
但 `PTRACE_GETREGS` 的失败可能另有原因，需单独验证。

---

## 五、安全边界（本轮）

- 系统分区 `/usr` `/etc` `/lib` **零写入**
- 所有上传仅到 `/tmp`（tmpfs）与 `/mnt/mmc`（SD 卡）
- 每次 apply 失败后确认：`TracerPid=0`、71 线程、相机 UI 正常
- 最坏情况重启即可，拔卡完全回原厂

---

## 六、REA 工具已重装

```
healthy: True
registration:dsh → aligned  (C:\Users\30128\.dsh\cordis.patch.yml)
Ghidra 12.1.4 (D:\workbuddy\...\tools\ghidra\ghidra_12.1.4_PUBLIC)
```
已补 `npm_config_cache` 指向 D 盘。**需完全退出并重启 DSH 后生效。**
