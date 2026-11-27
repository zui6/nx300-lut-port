#!/usr/bin/env python3
"""把完整 LUT blob 改造成「探针 + 直接转发」诊断版。

用途：回答「钩子到底有没有被相机调用」这一个问题，与 LUT 逻辑完全解耦。
两个钩子都改成「先写一个探针文件，然后立刻原样转发原厂函数」，
后面的 LUT 逻辑变成不可达代码，相机行为保持原厂。

用法：
    python make-diagnostic-blob.py <lut_ram_blob.c 路径>

改造后需要重新跑 tools/build_lut_ram_blob.py 生成新 blob（build tag 会变），
并同步更新 loader 的 lut_ram_blob_build.h 后重编译 loader。

注意：ipcs 钩子每帧都会跑，探针写文件（含 fsync）会严重拖慢相机并磨损 SD，
只适合短时间验证。测完请重启相机或恢复原厂槽位。
"""
import sys
from pathlib import Path

PROBE_UPDATE = '"/mnt/mmc/lut-probe-update","u\\n",2'
PROBE_IPCS = '"/mnt/mmc/lut-probe-ipcs","i\\n",2'


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    path = Path(sys.argv[1])
    src = path.read_text(encoding="utf-8")
    original = src

    # --- nx_lut_ram_update：探针 + 直接转发 ---
    anchor_u = "void nx_lut_ram_update(void *self,void *params) {"
    if anchor_u not in src:
        print("找不到 nx_lut_ram_update 定义")
        return 1
    head_u, rest_u = src.split(anchor_u, 1)
    # 定位函数体里 original 变量声明所在行
    decl_u = "    void (*original)(void *,void *)=(void *)nx_lut_ram_config.original_update;"
    if decl_u not in rest_u:
        print("找不到 update 里的 original 声明")
        return 1
    after_decl = rest_u.split(decl_u, 1)[1]
    rest_u = (
        "\n    /* DIAGNOSTIC BUILD: 只写探针，然后直接转发原厂 */\n"
        f"    write_file({PROBE_UPDATE});\n"
        f"{decl_u}\n"
        "    original(self,params);return;\n"
        + after_decl
    )
    src = head_u + anchor_u + rest_u

    # --- nx_lut_ram_ipcs：探针 + 直接转发 ---
    anchor_i = "void nx_lut_ram_ipcs(unsigned channel,const u16 *source) {"
    if anchor_i not in src:
        print("找不到 nx_lut_ram_ipcs 定义")
        return 1
    head_i, rest_i = src.split(anchor_i, 1)
    decl_i = "    void (*original)(unsigned,const u16 *)=(void *)nx_lut_ram_config.original_ipcs;"
    if decl_i not in rest_i:
        print("找不到 ipcs 里的 original 声明")
        return 1
    after_decl_i = rest_i.split(decl_i, 1)[1]
    rest_i = (
        "\n    /* DIAGNOSTIC BUILD: 只写探针，然后直接转发原厂 */\n"
        f"    write_file({PROBE_IPCS});\n"
        f"{decl_i}\n"
        "    original(channel,source);return;\n"
        + after_decl_i
    )
    src = head_i + anchor_i + rest_i

    if src == original:
        print("没有发生改动（可能已经是诊断版）")
        return 1
    path.write_text(src, encoding="utf-8", newline="\n")
    print(f"已改造: {path}")
    print("  下一步:")
    print("    1) 跑 tools/build_lut_ram_blob.py 生成新 blob（注意新的 build tag）")
    print("    2) 把新 runtime/lut_ram_blob_build.h 同步到 loader 构建目录")
    print("    3) 重编译 loader 并成套使用")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
