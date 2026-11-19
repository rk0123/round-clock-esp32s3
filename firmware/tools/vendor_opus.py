"""libopus 1.3.1 -> Arduino sketch src/ 子树（第二版，线性写法，便于排错）。"""
import os
import re
import shutil
import sys

SRC = r"C:\Users\rk\AppData\Local\Temp\opus-1.3.1"
DST = sys.argv[1] if len(sys.argv) > 1 else r"C:\Users\rk\WorkBuddy\weixue_esp32_v2.6.2\firmware\src\opus"

KEEP = ["src", "celt", "silk", "silk/fixed", "include"]
SKIP = {"opus_multistream.c", "opus_multistream_decoder.c", "opus_multistream_encoder.c",
        "opus_projection.c", "opus_projection_decoder.c", "opus_projection_encoder.c",
        "opus_demo.c", "opus_compare.c", "repacketizer_demo.c", "opus_custom_demo.c"}

CONFIG_H = '''/* 手写 config.h —— libopus 1.3.1 / ESP32-S3 定点构建 */
#pragma once
#define PACKAGE_VERSION "1.3.1"
#define OPUS_BUILD 1
#define FIXED_POINT 1
#define DISABLE_FLOAT_API 1
#define FLOAT_APPROX 1
#define VAR_ARRAYS 1
#define HAVE_LRINT 1
#define HAVE_LRINTF 1
#define HAVE_STDINT_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRING_H 1
#define HAVE_STRINGS_H 1
#define HAVE_INTTYPES_H 1
'''

BLOCK = re.compile(r'#ifdef HAVE_CONFIG_H[ \t]*\r?\n#include "config\.h"[ \t]*\r?\n#endif')
INC = re.compile(r'#include "([^"]+)"')
ORDER = ["src", "celt", "silk", "silk/fixed", "include"]

if os.path.isdir(DST):
    shutil.rmtree(DST)
for d in KEEP:
    os.makedirs(os.path.join(DST, *d.split("/")), exist_ok=True)
    s = os.path.join(SRC, *d.split("/"))
    for name in os.listdir(s):
        p = os.path.join(s, name)
        if os.path.isfile(p) and name.endswith((".c", ".h")) and name not in SKIP:
            shutil.copy2(p, os.path.join(DST, *d.split("/"), name))
open(os.path.join(DST, "src", "config.h"), "w", encoding="utf-8").write(CONFIG_H)

index = {}
for root, _, files in os.walk(DST):
    for name in files:
        index.setdefault(name, []).append(os.path.relpath(os.path.join(root, name), DST).replace("\\", "/"))

n_block = n_inc = 0
for root, _, files in os.walk(DST):
    for name in files:
        if not name.endswith((".c", ".h")):
            continue
        path = os.path.join(root, name)
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            text = f.read()
        out = []
        for line in text.replace("\r\n", "\n").split("\n"):
            out.append(line)
        text = "\n".join(out)
        orig = text

        target = os.path.relpath(os.path.join(DST, "src", "config.h"), root).replace("\\", "/")
        new_text, k = BLOCK.subn('#include "' + target + '"', text)
        n_block += k
        text = new_text

        def fix(m, root=root):
            head = m.group(1)
            if os.path.isfile(os.path.join(root, head)):
                return m.group(0)
            hits = index.get(os.path.basename(head))
            if not hits:
                return m.group(0)
            hits = sorted(hits, key=lambda h: ORDER.index(h.split("/")[0]) if h.split("/")[0] in ORDER else 99)
            return '#include "' + os.path.relpath(os.path.join(DST, *hits[0].split("/")), root).replace("\\", "/") + '"'

        text2, k2 = INC.subn(fix, text)
        n_inc += k2
        if name.endswith(".c"):
            head = '#include "%s"\n' % os.path.relpath(os.path.join(DST, "src", "config.h"), root).replace("\\", "/")
            if not text2.startswith(head):
                text2 = head + text2
        if text2 != orig:
            with open(path, "w", encoding="utf-8", newline="\n") as f:
                f.write(text2)
print("block=%d inc=%d" % (n_block, n_inc))

# ESP32-audioI2S 自带一个 opus 解码器（celt.cpp / silk.cpp），其中有 10 个同名的全局
# 查表；libopus 也定义了一份，链接期就是 multiple definition。
# 我们只用 libopus（编+解），所以给它这份统一加前缀，库那边一个字节都不用动。
RENAME = ["silk_NLSF_EXT_iCDF", "silk_Quantization_Offsets_Q10",
          "silk_stereo_only_code_mid_iCDF", "silk_stereo_pred_joint_iCDF",
          "silk_uniform3_iCDF", "silk_uniform4_iCDF", "silk_uniform5_iCDF",
          "silk_uniform6_iCDF", "silk_uniform8_iCDF", "tf_select_table"]
for root, _, files in os.walk(DST):
    for name in files:
        if not name.endswith((".c", ".h")):
            continue
        path = os.path.join(root, name)
        text = open(path, encoding="utf-8", errors="replace").read()
        new = text
        for sym in RENAME:
            new = re.sub(r"\b%s\b" % re.escape(sym), "rcz_" + sym, new)
        if new != text:
            open(path, "w", encoding="utf-8", newline="\n").write(new)
