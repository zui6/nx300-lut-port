# 07 崩溃根因与交接（2026-11-28 15:4x）

> 目的：让接手的 agent 在不知道前面 1.5M token 发生过什么的情况下，能直接继续。
> 前一份交接是 `docs/06-liveview-preview-plan.md` 附十一；**本文推翻其中一条结论**，见第一节。

---

## 一、结论：相机重启的根因是构建，不是钩子逻辑

### 现象

在 `CLiveviewNodeDisplayAdjust::Execute` 的虚表钩子里，只要加任何东西（哪怕只计数），相机就在 apply 之后 1~2 分钟重启。上一份交接据此推断"唯一安全的是只计数+转发"、"调 getter 会挂"。

### 真相

**blob 用 zig 一步式构建时，生成的是绝对寻址代码。**

`tools/build_lut_ram_blob.py` 原本把源码直接喂给 `zig cc ... -o blob.elf`。zig 的驱动按**链接模式**决定 PIC：静态链接时它把 `-fPIC` 丢掉，于是 `&contexts[SLOTS]` 被编译成常量：

```asm
movw ip, #0x70c4
movt ip, #0
add  ip, ip, #0xa0
add  lr, ip, #0x14000     ; lr = 0x1B164 —— 链接期绝对地址
ldr  r4, [lr, #0x80]      ; 读 0x1B1E4
```

blob 被 loader 映射在 `region`（实测 `0x83xxxxxx`），那条 `ldr` 取的是低地址未映射区 → 预览线程第一帧就死 → 内核看门狗约 1~2 分钟后重启整机（所以表现为"apply 之后过一会儿重启"，而不是"立刻崩"）。

旧 clang 构建的同一函数是**位置无关**的：

```asm
ldr ip, [pc, #0x78]
add ip, pc, ip            ; 基址来自 PC
```

### 修法（已落进代码）

`tools/build_lut_ram_blob.py` 改成两步：

1. 每个 `.c` 用 `zig cc ... -fPIC -c` 单独编成 `.o`
2. `zig ld.lld -T runtime/lut_ram_blob.ld ... a.o b.o -o lut_ram_blob.elf`

链接命令里没有 C 源码，驱动没有机会重新按非 PIC 编译。clang 路径保持原样（一步式）。

**验证**：改完后同一个"只计数+转发"钩子跑 **2299 帧 / 50.0 fps / 进程存活**。

### 后果：旧结论要重新评估

"往钩子里加 getter 就重启"很可能**全部**是同一个构建 bug 的表现——当时任何 zig 一步式构建的 blob 都会在第一帧崩。所以：

- `CCapImage` getter 调用**值得重试**；
- 交接里"已排除"的路线（尤其依赖 blob 内任何全局变量的）都应重新审视。

---

## 二、当前代码状态（未提交的地方要小心）

### `work\nx300blob`（分支 main，工作区脏）

| 文件 | 改动 | 状态 |
|---|---|---|
| `tools/build_lut_ram_blob.py` | 两步式 PIC 构建（核心修复） | **未提交** |
| `runtime/lut_ram_blob.c` | `nx_lut_ram_display`：计数 + 一次性 dump `CCapImage` 头 24 字 + 转发；新增 `nx_lut_dbg[64]`（`.bss`，**必须 hidden**） | **未提交** |
| `runtime/lut_ram_blob.ld` | header 第 13 字改为占位符，由构建脚本回填 tag | 未提交 |

`nx_lut_ram_setw` 已退化为空桩（loader 不再安装 setWidth 钩子），但符号必须保留：header 第 17 字是 loader/blob 的 ABI。

### `work\nx300build`（分支 main，已提交并推送）

- `4a60f94` loader 删掉 setWidth GOT 钩子，只留 DisplayAdjust 虚表钩子（`backend[40]`=槽位，`backend[42]`=原厂 Execute）
- `ecc51c5` CI 产物（72,776 B，sha256 `10f9e9f3…`）
- `05c2041` `build_loader_local.ps1`（zig 自检，产物 `*.zigtest.arm`，**绝不上机**）
- **未提交**：`build_loader_remote.ps1`（远程构建，见第五节）、`.gitignore`

### `work\x300-lut-port`（分支 experimental）

本文档所在仓库；`docs/06-liveview-preview-plan.md` 是上一份完整技术档案（附五~附十一）。

---

## 三、相机与注入的现状

- 相机：`192.168.1.142`，telnet 23，root shell（`nx300:/#`）。目标进程 `di-camera-app-nx300`，pid **265**。
- 用户换过电池，**内存镜像已清空**；最后一次 `tmp\cam_exec.py` 被中断，相机应在正常运行，钩子状态未知。
- 注入全在内存，**重启即清除**。
- 载荷目录：`/mnt/mmc/nx-t/`（`lut.arm` = loader，`blob.bin` = blob）
- worker 日志：`/mnt/mmc/nx300m-lut-worker-<pid>.log`（apply 的 stderr + 最终 JSON 都在这里，**不在终端**）
- 规则：**只动 SD 卡**，不写固件/系统分区。

### apply 被拒（rc=7）怎么办

```
Existing RAM image, identity, root or code differs; validated restore required before upgrade.
EXIT rc=7 stage=existing-RAM-image-admission errno=2
```

内存里已有镜像且与新 blob 不符。两条路：

1. `./lut.arm restore --pid 265 --blob blob.bin --root /mnt/mmc/nx300m-mod`
   要求 `blob.bin` **就是驻留内存的那一份**（逐字节匹配）。所以：**先 restore，再覆盖 blob.bin，最后 apply**，顺序不能反。
2. 重启相机（最省事，内存清空）。

`restore-known` 同样需要 `--blob`。没有 `--force`。

---

## 四、下一步（按顺序）

1. **跑 `tmp\cam_exec.py`**：restore → 推新 blob → apply → 读计数器与 dump（脚本已写好，见第五节）。
2. **解 `CCapImage` 头**。期望（旧记录）：`word[2]`=stride 800、`word[4]`=720、`word[5]`=480、`word[8]`=3。24 个字全打出来后按实际值定位。
3. **从头里找 buffer 指针** → 结合 `CCapBuffer::getVirtAddress` / `getGWidth` / `getOffsetX` / `getOffsetY` 推平面地址：
   `Y = offX + offY*gWidth`，`UV = offX + (offY/2)*gWidth`。
   如果 getter 能安全调用（现在很可能可以），直接调比推布局更省事。
4. **构造 `ADE_Image`**：
   - `sADEImgSize` 12 B = {width, height, stride}
   - `sADEYccAddr` 20 B ×2（输入/输出），`[+0x04]/[+0x08]/[+0x0C]/[+0x10]` = Y 基址/行尾/UV 基址/行尾
   - `sADEPixelType` 12 B = {0,1,0}
   - 调用序：ctor `0x1d9a3c` → `RegFlushFunc` `0x1d9a54` → `ADE_SmartFilter_YccProcess` `0x1d9a64`
5. 接已跑通 70 帧的 IPCS 事务。**底线：任何一步失败都转发原厂，绝不吞调用。**

### 新构建的地址（会随每次构建变化，务必从产物里重新取）

| 项 | 值（tag 516503513 那次） |
|---|---|
| `nx_lut_dbg` | `0x1bd88`（从 ELF `.symtab` 读，别硬编码） |
| `contexts_offset` / `stride` / `count` | 24772 / 9120 / 9 |
| `extra` 基址（`&contexts[SLOTS]`） | `24772 + 9*9120 = 106852` |
| 帧计数器 `w[32]` | `region + 106852 + 128` |
| "已 dump" 标志 `w[33]` | `region + 106852 + 132` |
| `w[42]` | 原厂 Execute（loader 填，**不要覆盖**） |
| `region` | 每次 apply 不同，从 worker 日志的 JSON 读 |

---

## 五、工具

### 远程 loader 构建（8 秒，替代 CI）

```powershell
cd D:\workbuddy\2026-10-16-19-57-08\work\nx300build
powershell -NoProfile -File .\build_loader_remote.ps1 -Out ..\..\tmp\scratch\x.arm        # 实验
powershell -NoProfile -File .\build_loader_remote.ps1 -MatchCi -Out ...                   # 对齐 CI
```

- 服务器：`zui@192.168.1.119`，密码 `zhongyon32`，`sudo` 同密码；目录 `/home/zui/nx300build`；`arm-linux-gnueabi-gcc 12.2.0`
- **默认 `-Out` 会覆盖 git 跟踪的 CI 产物**，实验一定带 `-Out`
- Debian 的 gcc 默认不开 `_FORTIFY_SOURCE`，且页对齐 4 KiB（Ubuntu 是 64 KiB），所以逐字 flags 的产物是 52,152 B；`-MatchCi` 补 fortify 后 72,784 B，动态导入与 CI 完全一致
- **字节完全相同不可能**：连续两次同 flags 构建差 6 字节，全在 `.strtab`（gcc 临时文件名）

### blob 本地构建（1 秒）

```powershell
cd D:\workbuddy\2026-10-16-19-57-08\work\nx300blob
$env:NX_LUT_CC = "D:\AndroidSdk\zig\zig-windows-x86_64-0.13.0\zig.exe"
$env:PYTHONPATH = "D:\tools\pylib"
python -X utf8 tools\build_lut_ram_blob.py --output-dir out-local
```

### 反汇编（不装 Ghidra 也能看）

```powershell
$env:PYTHONPATH="D:\tools\pylib"
python tmp\disasm_blob.py work\nx300blob\out-local\lut_ram_blob.elf nx_lut_ram_display
python tmp\disasm_bin.py  work\nx300blob\out-local\lut_ram_blob.bin 16 64   # header 第 16 字 = display 入口
```

capstone、paramiko 已装在 `D:\tools\pylib`。改 blob 后**一定**先反汇编确认没有 `movw/movt` 绝对地址。

### 相机侧脚本

| 脚本 | 用途 |
|---|---|
| `tmp\camcmd.py '<cmd>'` | 跑一条**短**命令并打印输出 |
| `tmp\cam_exec.py` | restore → 推 blob → apply → 读计数与 dump（中途被中断，需重跑） |
| `tmp\step2_dump.py` | 同上但更早的版本，可参考 |
| `tmp\sample_count.py <region> [n] [period]` | 只采样帧计数器 |

---

## 六、这次新踩的坑（务必知道）

1. **相机的行编辑器会吞字符。**
   超过 ~70 字符的命令行会被 busybox 的 lineedit 破坏（插入空格、丢字符），表现为"命令没执行"或参数解析失败。之前所有"apply 没反应"都是这个原因。
   **解法**：`nc -l -p 9995 | sh` 把脚本从 socket 直接喂给 `sh`，绕开 tty（`tmp\cam_exec.py` 已实现）。heredoc 也会被吞，别用。

2. **PIC + `visibility("default")` = 走 GOT = 崩。**
   blob 没有动态链接，GOT 永远不会被填。所有要在 blob 里访问的全局符号必须 `hidden`（或 `static`），否则编译成 GOT 间接访问，第一次用就跳飞。`nx_lut_dbg` 就是踩了这个坑。

3. **驻留镜像会阻塞再 apply**（rc=7），且 `restore` 要求 blob 文件与驻留镜像逐字节一致 —— 所以顺序是 **restore → 覆盖 blob.bin → apply**。

4. **worker 日志才是 apply 的输出。** 终端只看得到最终 JSON；`/mnt/mmc/nx300m-lut-worker-<pid>.log` 里才有 STAGE 行和失败原因。

5. **相机偶尔从路由器掉线**，表现为连接重置，极易误判成崩溃。脚本要能重连重试。

---

## 七、已完成 / 未完成

**已完成（实机验证）**

- 拍照 JPEG 带 LUT（main 分支，稳定）
- 微型滤镜模式下取景 LUT（约 30 FPS）
- 一键注入 / 恢复，幂等
- **普通模式 DisplayAdjust 虚表钩子：2299 帧 / 50.0 fps / 进程存活**（本文第一节）
- 相机重启根因定位 + 构建修复

**未完成**

- `CCapImage` 头 dump（代码已写好，`cam_exec.py` 被中断，未取到数据）
- 平面地址推导
- `ADE_Image` 构造 + IPCS 事务接进普通模式
- 交接文档 `docs/06` 附十一里"getter 会崩"的结论需要用新构建重测后修正
