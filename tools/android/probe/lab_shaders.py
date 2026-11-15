#!/usr/bin/env python3
# lab_shaders.h for the probe's shader lab: lab_shaders.py <game's shader build folder> <dxc folder> <lab_write.hlsl> <output>.
import os, platform, re, subprocess, sys, tempfile

folder, dxc, write, output = sys.argv[1:5]
out = ""

def array(name, data):
    return f"static const unsigned char lab_{name}[] = {{" + ",".join(map(str, data)) + "};\n"

for name in ["IdleCS", "TextureDecodeCS", "FbWriteDepthCS", "FbWriteDepthCSMS", "FbWriteColorCS", "FbReadAnyFullCS", "FbReadAnyChangesCS"]:
    text = open(f"{folder}/{name}.hlsl.spirv.c").read()
    out += array(name, (int(n) & 255 for n in re.findall(r"-?\d+", re.search(r"=\s*\{(.*?)\}", text, re.S).group(1))))

arch = "arm64" if platform.machine() == "arm64" else "x64"
env = dict(os.environ, DYLD_LIBRARY_PATH=f"{dxc}/lib/{arch}")
with tempfile.TemporaryDirectory() as d:
    for variant in range(7):
        spv = f"{d}/v{variant}.spv"
        subprocess.run([f"{dxc}/bin/{arch}/dxc-macos", "-spirv", "-fspv-target-env=vulkan1.0", "-fvk-use-dx-layout", "-E", "CSMain", "-T", "cs_6_3",
                        "-D", f"VARIANT={variant}", write, "-Fo", spv], check=True, env=env)
        out += array(f"v{variant}", open(spv, "rb").read())
    # The palette decoder as it was, to compare with the game's.
    spv = f"{d}/old_decode.spv"
    subprocess.run([f"{dxc}/bin/{arch}/dxc-macos", "-spirv", "-fspv-target-env=vulkan1.0", "-fvk-use-dx-layout", "-E", "CSMain", "-T", "cs_6_3",
                    f"-I{dxc}/../..", f"-I{dxc}/../../shaders", "-D", "RT64_TLUT_SWITCH", f"{dxc}/../../shaders/TextureDecodeCS.hlsl", "-Fo", spv], check=True, env=env)
    out += array("OldDecode", open(spv, "rb").read())
open(output, "w").write(out)
