# NX300 LUT 移植 —— 第四阶段（注入已打通）

> 时间：2026-11-22 02:22
> 状态：**认证链 + blob + ptrace 注入全部打通；apply 会导致相机重启（未变砖）**

---

## 一、本轮突破

### 1.1 关键修复：`syscall_stop` 容忍 POSIX 定时器信号

**现象**（修复前）：
```
Remote syscall 192 failed: original pc=b55a71ec r0=00000012 r7=142
   entry pc=00000000 r0=00000000 r7=0 ...
Admission/syscall/patch failed at mmap2
```

`entry pc=0` 说明 `syscall_stop()` 返回 false（没等到 `SIGTRAP|0x80`）。

**根因**：相机的 `di-camera-app` 驱动 POSIX 定时器，`PTRACE_SYSCALL`
之后第一个停下来的原因可能是实时信号 **42**（`SIGRTMIN+10`），
而不是 syscall-entry trap。原实现只接受 `SIGTRAP|0x80`，于是直接失败。

**修复**（`live_loader_core.h` 的 `syscall_stop`）：
```c
for(unsigned attempt=0;attempt<8;attempt++){
    if(ptrace(PTRACE_SYSCALL,s->tid,NULL,NULL)<0){s->stopped=1;return 0;}
    if(!wait_stop(s,1500))break;
    if(s->signal==(SIGTRAP|0x80))return 1;   /* 只认 syscall trap，其余继续 */
}
```

### 1.2 ABI 已彻底排除

GitHub Actions 里加了 qemu 实测探针：
```
time_t=4  timespec=8
clock_gettime tv_sec=31 tv_nsec=97527664
syscall263 ret=0 tv_sec=31 tv_nsec=97849716
ABI OK
```
**`struct timespec` / variadic `syscall(263)` 均正常**，ABI 不是问题。

### 1.3 修复后的效果

修复版 `apply` 执行后 **相机重启** —— 这恰恰证明：
- ptrace 附加成功
- 远程 `mmap2` 成功
- **真的写到了相机进程内存**

之前（修复前）根本走不到这一步。属于功能性突破，但 hook 尚未正确，
导致 `di-camera-app` 崩溃（重启即恢复，**未变砖**）。

---

## 二、当前完整状态

| 阶段 | 状态 |
|---|---|
| 源码获取 | ✅ `repo/nx300m-mod/`（main，1658 文件）|
| NX300 常量表提取 | ✅ 全部对应 |
| 源码级地址改写 | ✅ 4 组共 37 处 |
| Liveview 门控 | ✅ 改为仅线程判定 |
| `crt0_arm.S` | ✅ 自写（原仓库缺失）|
| 云端编译 | ✅ GitHub Actions，产物可复现 |
| `status` 全部校验 | ✅ 全 true，EXIT=0 |
| `blob` admission | ✅ 通过 |
| ptrace attach | ✅ 通过 |
| 远程 `mmap2` | ✅ 通过（修复 `syscall_stop` 后）|
| **写入 GOT / vtable** | ⚠️ 执行后相机崩溃 |
| LUT 实际生效 | ❌ 未验证 |

---

## 三、崩溃的可能原因（下一步排查）

1. **vtable 槽选择错误**
   `_ZTV12CIpcs3DLutD4 = 0x3bed00`，取 `+8 = 0x3bed08`。
   但 NX300 的 vtable 有 32 字节（8 项），第 3 项才是 `Update`？
   需核对 NX300 虚表的实际布局（`Update` 是第 2 项还是第 3 项）。

2. **`stock_update` / `stock_reset` 字节指纹**
   `code_check` 已通过（status 全 true），说明指纹对。
   但 blob 内部使用的是**配置传入的地址**，需确认配置字段正确。

3. **blob 的 LUT 处理逻辑**
   `lut_ram_blob.c` 是 NX300M 的代码，虽然无硬编码地址，
   但对 `CIpcParameterNode` 的调用约定可能依赖 NX300M 的类布局。

4. **ptrace 恢复不完整**
   `remote_syscall` 结束后要求逐字恢复寄存器与 VFP；若恢复失败，
   相机线程状态损坏 → 崩溃。日志会有 `RECOVERY_PENDING`。

**下一步建议**：先做一个**只 mmap、不改槽**的 dry-run 版本，
确认 ptrace 注入本身不伤相机；再单独测试改 GOT（不动 vtable）；
最后再动 vtable。逐步定位。

---

## 四、产物清单（已归档到 `handover-lut/build-nx300/`）

| 文件 | 说明 |
|---|---|
| `lut_live_loader.arm` | 编译产物，71,700 B，sha256 `211c419f…7a86` |
| `lut_live_loader.c` | 改写好 NX300 地址的源码 |
| `live_loader_core.h` | 含 liveview 与 syscall_stop 两处修复 |
| `lut_live_stock.h` | 自动生成的 NX300 指纹常量 |
| `crt0_arm.S` | 自写启动代码 |
| `build.yml` | GitHub Actions 工作流（含 ABI 探针）|

上游仓库：https://github.com/zui6/nx300-lut-loader-build （private）

---

## 五、安全边界（全程遵守）

- 系统分区 `/usr` `/etc` `/lib` `/bin`：**零写入**
- 上传目标仅 `/tmp`（tmpfs）与 `/mnt/mmc`（SD 卡）
- 两次相机重启均为 apply 后崩溃所致，**重启即恢复，未变砖**
- 最坏情况：拔 SD 卡完全回原厂

---

## 六、REA 工具

```
healthy: True
registration:dsh → aligned (C:\Users\30128\.dsh\cordis.patch.yml)
Ghidra 12.1.4
```
已补 `npm_config_cache` → D 盘。需完全退出并重启 DSH 生效。
