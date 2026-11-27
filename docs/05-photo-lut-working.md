# NX300 机内 LUT：稳定版（照片已可用）

> 状态：**照片 JPEG 带 LUT 已经实机验证通过**（2026-11-27）
> 取景实时预览尚未实现，见 [限制](#已知限制)。

---

## 一、成果

| 环节 | 状态 | 证据 |
|---|---|---|
| 原厂模块指纹校验 | ✅ | `status` → `stock_iq_sha256/stock_udd_sha256: true` |
| 动态 ABI / 代码指纹 | ✅ | `dynamic_abi/code_bytes: true` |
| RAM blob 准入 | ✅ | `blob-admission` 通过 |
| ptrace 注入（71 线程） | ✅ | `stop window ≈ 0.3 s` |
| vtable 改写 | ✅ | 内存实测 `slot_update == region+64` |
| GOT 改写 | ✅ | 内存实测 `slot_b == region+ipcs_entry` |
| 相机调用 C++ 虚函数钩子 | ✅ | trace `update_tid=286` |
| 相机调用 `ipcs_3d_lut` GOT 钩子 | ✅ | trace `write_calls=3` |
| **LUT 表替换生效** | ✅ | trace `forwarded_ptrs` 指向 blob 内的表 |
| **拍照出片带 LUT** | ✅ | 实机拍摄为黑白 |

---

## 二、产物

| 文件 | 大小 | sha256 |
|---|---|---|
| `build/lut_live_loader.arm` | 72,120 | `d93d1028ba0cdcfbbd082530ad4c239127a17528ed27e505c51befd1f76fcfaf` |
| `blob/lut_ram_blob.bin` | 102,400 | `9c37587709923a3f6a3d640ed6dc7907f7128e0ccd6fcb279521f4299a6dad7f` |
| `build/lut_ram_blob_build.h` | — | tag `3723920501` |

**三件套必须成套使用**：loader 内置 `NX_LUT_BLOB_BUILD` 校验，与 blob header 的
`reserved[3]` 不一致会报 `Invalid RAM blob/header`。

---

## 三、使用步骤

前提：`nx300m-mod` 的文件在 `/mnt/mmc/nx300m-mod/`（`looks/` 里有 `table.d4lut`）。
mod 本身可以处于 `DISABLED` 状态——本条链路不依赖 mod 的引擎。

```sh
# 1) 上传（相机监听，PC 发送）
#    相机端: nc -l -p 9200 > /tmp/lut.arm
#    PC  端: nc 192.168.1.142 9200 < lut_live_loader.arm
#    同样方式传 blob 到 /tmp/blob.bin

# 2) 准备
chmod 755 /tmp/lut.arm
mkdir -p /tmp/lutrun
cp /tmp/blob.bin /tmp/lutrun/lut_ram_blob.bin

# 3) 清掉可能残留的 trace 开关（逐帧 fsync 会拖慢相机）
rm -f /mnt/mmc/lut-trace-enabled

# 4) 注入
cd /tmp/lutrun && /tmp/lut.arm apply --pid 265 \
    --blob /tmp/lutrun/lut_ram_blob.bin --root /mnt/mmc/nx300m-mod

# 期望输出末尾：
#   EXIT rc=0 stage=slot-commit errno=0
#   {"command":"apply",...,"hooks_installed":true,...,"ready":true,...}

# 5) 选择要应用的 LUT
printf '1 /mnt/mmc/nx300m-mod/looks/nx300-hard-mono/table.d4lut 100\n' \
    > /var/run/nx300m-lut-selection
```

然后**拍摄一张照片**，回放查看即为 LUT 效果。

### 选择文件格式（关键）

```
<代次> <表文件绝对路径> [强度0-100]
```

- 路径**必须指向 `table.d4lut` 文件本身**，不能只写目录
- 路径必须以 `<root>/looks/` 开头，只允许 `a-z 0-9 / _ - .`
- 代次变化会强制重新加载
- 省略强度默认 100

可用风格目录（`/mnt/mmc/nx300m-mod/looks/`）：
`nx-original-clearblue`、`nx-original-coolnight`、`nx-original-softcolor`、
`nx-original-sunlight`、`nx300-adjust-base`、`nx300-hard-mono`

### 触发 Update

`CIpcParameterNode::Update` 不是每帧调用，只在参数真正变化时触发。可靠触发方式：

- 切换照片风格 / 图片导向 / 智能滤镜
- **进回放再返回取景**（最稳，会重建取景链）

改完选择文件后需要触发一次 Update 才会被读到。

---

## 四、验证证据

注入成功后，打开 trace 可看到（`touch /mnt/mmc/lut-trace-enabled`）：

```json
{"stage":"update-return","update_tid":286,
 "writer_tids":[286,286,286],"channel_order":[0,1,2],"write_calls":3,
 "source_branch":"factory",
 "source_ptrs":[3028143816,3028145276,3028146736],
 "forwarded_ptrs":[2208269048,2208270508,2208271968],
 "generation":1,"strength":100,
 "selection_reads":2,"selection_cache_hit":true,
 "table_reads":1,"table_cache_hit":true,
 "selection_path":"/mnt/mmc/nx300m-mod/looks/nx300-hard-mono/table.d4lut"}
```

判读要点：

| 字段 | 含义 | 正常值 |
|---|---|---|
| `writer_tids` | 三通道写入线程 | 与 `update_tid` 相同 |
| `channel_order` | 通道顺序 | `[0,1,2]` |
| `source_ptrs` | 原厂表地址 | 三个相差 1460 字节 |
| `forwarded_ptrs` | **实际转发给原厂的表** | **应与 `source_ptrs` 不同**（指向 blob 内） |
| `table_reads` | 表文件读取次数 | ≥1 |
| `selection_path` | 解析出的路径 | 指向 `.../table.d4lut` |

**`forwarded_ptrs == source_ptrs` 是最常见的失败特征**：说明仍在转发原厂表，
通常是选择路径写错（只写到目录、或表大小不等于 4380 字节）。

---

## 五、这一版修掉的四个致命问题

### 1. `%llu` 在本机 ABI 上是坏的（最关键的坑）

用 GCC 14 编译、链接相机自带的 glibc 2.13，`printf` 家族处理 `%llu` 时会
**错位读取后续可变参数**。后果：

- `publish_markers()` 里的 `%llu` 后面紧跟 `%s`，字符串指针变成野指针，
  `snprintf` 段错误 → **worker 在写 marker 前当场死掉**
- 表现：所有注入阶段都打出来了，却永远没有 `EXIT`、永远没有 marker、
  LUT 引擎永远不激活
- 同样原因让 `status` 输出里 `state` 显示成空格、`tracer_pid` 显示成 83（真实是 `S` / `0`）

**修复**：新增 `u64_text()`，把所有 `%llu` 换成 `%s`；marker 读取端改用
`fscanf("%23s") + strtoull`。`sscanf` 的 `%llu` 不受影响，无需改动。

### 2. `detach_all()` 会重放挂起信号

`PTRACE_DETACH` 的第 4 个参数是要投递给目标的信号。相机进程有 POSIX 定时器，
线程可能以**实时信号 42** 停止；原代码把 `stops[i].signal` 原样传下去，
等于在 detach 时向相机投递 42 —— 实时信号默认动作是**终止进程**。

**修复**：detach 一律传 0。

### 3. worker 日志写在 tmpfs，重启即丢

原路径 `/var/run/nx300m-lut-worker-N.log`，相机一重启就没了，导致长期拿不到崩溃现场。

**修复**：优先写 `/mnt/mmc/`，并加 `O_SYNC`（exFAT 的 `write()` 只进页缓存）。

### 4. `syscall(263, CLOCK_MONOTONIC, ...)` 在本机不可靠

无论 variadic `syscall()`、`clock_gettime()`（glibc 2.13 在 librt 里、`-nostdlib` 链不到）
还是内联 `svc`，取到的时间戳都异常，导致 supervisor deadline 立刻过期、
在 71 个线程仍被 ptrace 停住时 kill 掉 worker。

**修复**：改用 `gettimeofday()`（固定参数、在 libc 里、不需要 librt）。

---

## 六、已知限制

### 1. 取景 / 拍照后快速预览**不带 LUT**

这是**架构决定**，不是 bug：

| 路径 | 库 | 本方案是否覆盖 |
|---|---|---|
| 拍照 JPEG | `libiq-fw-slpcam.so` → `ipcs_3d_lut` | ✅ |
| 取景显示 / 快速预览 | `libcapture-fw-slpcam.so` → SmartFilter YCC 链 | ❌ |

原厂取景是一条**独立的 SmartFilter 节点 + YCC 处理链**，需要另外在
`libcapture-fw-slpcam.so` 里挂桥接。mod 的上游实现见
`docs/SMARTFILTER_LUT_PREVIEW.md`（5 个 PLT 槽 + YCC→IPCS→YCC 桥）。
把相机智能滤镜切到「微型」也**不能**让预览显示本 LUT（实测无变化）。

### 2. 一次性生效，不持久

LUT 只存在于运行时内存。**相机重启后需要重新 apply**。

要日常使用，需要把 apply 接到开机流程（`/mnt/mmc/autoexec.sh`）；
注意每次开机 `iq_bias` / `udd_bias` 都可能不同，loader 会自动重新探测，无需改配置。

### 3. 看门狗

注入期间会短暂冻结全部线程约 0.3–0.6 秒。实测在**看门狗开启**时，
相机可能在注入后重启（看门狗未及时被喂）。

- 若遇到注入后重启：关闭相机看门狗，或确认看门狗超时足够长
- 修复 `%llu` 之前观察到的频繁重启，部分来自 worker 崩溃的连带效应；
  该修复后稳定性明显改善，但仍未在「看门狗开启」条件下做完整回归

### 4. `apply` 会被"来历不明的残留钩子"拒绝

若槽位里已有非原厂值而 `/var/run/nx300m-lut-live-265.state` 不存在，loader 会安全拒绝：

```
Unrecognized resident hooks without a valid live state: ...; refusing replacement.
```

这是**正确的保护**。解决办法：重启相机让内存恢复原厂，然后重新 apply。

**不要**手工用 `dd` 去改 vtable/GOT 槽位——地址依赖当次开机的 `iq_bias`，
写错会破坏相机进程内存（重启可恢复，但会浪费一轮）。

---

## 七、排错表

| 现象 | 原因 | 处理 |
|---|---|---|
| `Invalid RAM blob/header` | loader 与 blob 的 build tag 不匹配 | 换成同一套三件套 |
| `EXIT rc=3 stage=blob-admission` + `Unrecognized resident hooks` | 槽位有残留钩子 | 重启相机后重试 |
| `EXIT rc=0` 但照片无 LUT | 选择文件路径/大小不对 | 确认指向 `table.d4lut` 且大小为 4380 |
| trace 里 `forwarded_ptrs == source_ptrs` | 表没被加载 | 同上；看 `table_reads` 是否为 0 |
| trace 没有新文件 | `Update` 没被触发 | 进回放再返回取景 |
| 相机注入后重启 | 看门狗超时 | 关闭看门狗 |
| 相机完全无响应 | 极少见 | 拔电池重启，内存改动全部消失 |

---

## 八、安全边界

- 只写 **SD 卡**、`/tmp`、`/var/run`（tmpfs）和**目标进程运行时内存**
- **从不写入** `/usr` `/etc` `/lib` `/boot` 或任何固件分区
- 所有改动**重启即完全恢复**；拔掉 SD 卡即回原厂
- 相机进程被 ptrace 期间若断电，最坏是黑屏，拔电池重启即可，**不会变砖**
