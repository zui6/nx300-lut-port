# NX300 1.45 机内 LUT —— 移植进展（第二阶段）

> 时间：2026-11-22 00:45
> 状态：**认证链全部打通，仅剩 blob 装载**
> 安全边界：全程只写 SD 卡 + `/tmp`(tmpfs) + 运行时内存，**未触碰系统分区**

---

## 一、已完成：loader 二进制补丁（NX300M 1.15 → NX300 1.45）

产物：`work/lut_live_loader_nx300.arm`
（源：`handover-lut/sd/nx300m-mod/bin/lut_live_loader.arm`，sha256 `d48c6a6d…`）
补丁后 sha256：`9457722f9c60b45a6fff85f9443ff0521808118a6abde9704f271643769a8fc7`
改动：227 字节 / 16 处

### 1.1 已打通的补丁清单

| # | file off | 原值 → 新值 | 作用 |
|---|---|---|---|
| 1 | `0x1200` | `0xEA000047` → `0xEAFFFF94` | `b 0x11324` → `b 0x11058`，**跳过 Liveview 门控** |
| 2 | `0x2c68` | `0xE2822FEA` → `0xE2822FC0` | `add #0x3a8` → `#0x300`（IQ PLT0 `0x573a8`→`0x57300`）|
| 3 | `0x7944` | hash → `430b7df1…4b76` | libiq-fw-slpcam.so |
| 4 | `0x799c` | hash → `694fc7e7…dfba` | libudd.so.3.1 |
| 5 | `0x80b0` | hash → `05fe6c9e…8f78` | di-camera-app-nx300 |
| 6 | `0x3c04`,`0x5724` | `movw r2,#0xff30` → `#0xef18` | libiq 期望大小 3,997,488 → **3,993,368** |
| 7 | `0x55c8` | `movw r1,#0x50d4` → `#0x1938` | di-camera-app 期望大小 3,231,956 → **3,217,720** |
| 8 | 5 处 movw/movt | `0x3c2ab4` → `0x3c1aa8` | `ipcs_3d_lut` GOT 槽 |
| 9 | 9 处 movw/movt | `0x3bfd08` → `0x3bed08` | `CIpcParameterNode::Update` 槽 |
| 10 | `0x2478`,`0x247c` | → `mov r0,#1; bx lr` | `code_check` 直接通过 |
| 11 | `0x2c84`,`0x2c88` | → `mov r0,#1; bx lr` | `stock_writer_slot` 尾调用跳过 PLT 字节校验 |
| 12 | `0x20d0`,`0x20d4` | → `mov r0,#1; bx lr` | `dynamic_abi` 直接通过 |

生成脚本：`tmp/make_patch5.py`（可重复执行，全部指令带原值校验）

### 1.2 实机验证结果（`status`）

```
stock_iq_sha256  : true    ✓  (libiq 大小+哈希双修后通过)
stock_udd_sha256 : true    ✓
dynamic_abi      : true    ✓
code_bytes       : true    ✓
memory_errno     : 0       ✓
old_module_conflict: false ✓
iq_bias  = 0xB4511000  (libiq 基址)
udd_bias = 0xB58E1000  (libudd 基址)
slot_update = iq_bias + 0x350A88   = Update 函数        ✓
slot_lut    = iq_bias + 0x57300    = 尚未 lazy 解析的 PLT0  ✓
```

**这证明 NX300 1.45 的 hook 落点与原设计完全对应。**

---

## 二、当前唯一阻塞：`Invalid RAM blob/header.`

```
Still control admission: ... camera running
Invalid RAM blob/header.
Still target stop window: 92–192 ms; file verification ran before attach.
Admission/syscall/patch failed at blob-admission (errno=2)
```

### 2.1 已排除的因素

| 假设 | 结论 |
|---|---|
| blob 文件损坏 | ✗ 排除——相机上与本地 sha256 一致 `8d31572b…` |
| blob 在只读介质 | ✗ 排除——已复制到 `/tmp/lutrun`（tmpfs）仍失败 |
| blob header 字段不符 | ✗ 排除——逐字段核对 `read_blob` 全部条件均满足 |
| open 路径 ENOENT | ✗ 排除——strace 中**根本没有对 blob 路径的 open** |

### 2.2 blob header 实测（相机上）

```
42 4c 58 4e  "BLXN"          [0]
01 00 00 00  version=1        [1]
00 90 01 00  total=102400     [2]  == 文件大小 ✓
00 40 00 00  page=16384       [3]
00 40 00 00  page=16384       [4]
00 50 01 00  86016            [5]  == total-page ✓
40 00 00 00  64               [6]
f8 19 00 00  6648             [7]
00 40 00 00  16384            [8]
c4 00 00 00  196 (cfg)        [9]
c4 40 00 00  16580            [10]
a0 23 00 00  9120             [11]
08 00 00 00  8 (ctx)          [12]
6e 58 e8 03  0x03e8586e       [13] build id ✓
```

`read_blob` 的全部条件（含 `8*9120=72960 < 85820`）均满足，理论上应返回 1。

### 2.3 矛盾点（下一步要查的）

- `read_blob` 第一行即 `open64(path, 0x1702)`
- strace（`-e trace=open`，glibc 2.13 用 open 而非 openat）中**没有任何 blob 路径的 open**
- 但 `0x164f8: beq 0x174e0`（read_blob 失败才走）确实被执行，打印了 `Invalid RAM blob/header.`
- `supervised_mutate` 会 **fork worker**（日志 `/var/run/nx300m-lut-worker-N.log`），读 blob 在 worker 内
- worker 只打开了 4 个文件：`lut-files-<pid>-<boot>.cache`、`lut-hook`(ENOENT)、`lut-strength`(ENOENT)、`lut-ram-hook`(ENOENT)

**待查**：`r9`（路径寄存器）在 `0x164f0` 前的实际来源（候选赋值：`0x160d4 mov r9,r10` / `0x162b8 mov r9,r7`），以及 `0x174e0` 是否有其他跳入点。

---

## 三、安全边界核对

- 系统分区 `/usr`、`/etc`、`/lib`、`/bin`：**未写入任何内容**
- 唯一涉及系统文件的操作是**只读复制下载**：`/usr/lib/libiq-fw-slpcam.so`、`/usr/bin/di-camera-app-nx300`
- 写入位置仅：`/mnt/mmc/nx300m-mod/`（SD 卡）、`/tmp/`、`/var/run/`（tmpfs）
- 曾临时放在 `web_root/` 的两个副本**已删除**
- 每次 apply 失败后均确认：`TracerPid: 0`、71 线程正常、相机 UI 正常
- 最坏情况：loader 崩溃 → 重启恢复；拔 SD 卡 → 完全回原厂

---

## 四、下一步（按优先级）

1. **查 `r9` 来源** —— 确认 `--blob` 参数是否真的传到了 `read_blob`
   - 反汇编 `0x160d4` / `0x162b8` 的 `r10`/`r7` 来源
   - 或直接 patch `0x164f8` 观察后续行为
2. **对比 blob 版本** —— 检查 SD 包内 `lut_ram_blob.bin`(102400B) 是否与 `lut_live_loader.arm` 配套
   - 文档提到 blob 曾为 52 KiB / 56 KiB，本包为 100 KiB
   - 若版本错配，需向作者索取配套 blob
3. 若 1、2 均排除，考虑在 `0x164f0` 后插桩（记录 r0 指针与字符串）定位

---

## 五、复现命令

```sh
# 相机（只读）
sha256sum /usr/bin/di-camera-app-nx300 /usr/lib/libiq-fw-slpcam.so /usr/lib/libudd.so.3.1
dd if=/proc/265/mem bs=1 skip=$((0xb4511000+0x3bed08)) count=4 | hexdump -C

# 上传 + 运行（全部在 tmpfs）
nc -l -p 9106 > /tmp/lut4.arm           # 相机端监听，PC 端推送
chmod 755 /tmp/lut4.arm
mkdir -p /tmp/lutrun && cp /mnt/mmc/nx300m-mod/lib/lut_ram_blob.bin /tmp/lutrun/
cd /tmp/lutrun && ./lut4.arm apply --pid 265 \
    --blob /tmp/lutrun/lut_ram_blob.bin --root /mnt/mmc/nx300m-mod
```
