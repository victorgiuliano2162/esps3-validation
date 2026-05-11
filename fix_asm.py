Import("env")
import os
import subprocess

asm_dirs = [
    "lib/model_v5/src/edge-impulse-sdk/porting/espressif/ESP-NN/src/activation_functions",
    "lib/model_v5/src/edge-impulse-sdk/porting/espressif/ESP-NN/src/basic_math",
    "lib/model_v5/src/edge-impulse-sdk/porting/espressif/ESP-NN/src/common",
    "lib/model_v5/src/edge-impulse-sdk/porting/espressif/ESP-NN/src/convolution",
    "lib/model_v5/src/edge-impulse-sdk/porting/espressif/ESP-NN/src/fully_connected",
    "lib/model_v5/src/edge-impulse-sdk/porting/espressif/ESP-NN/src/pooling",
]

cc = env.subst("$CC")
flags = env.subst("$CCFLAGS $CPPFLAGS $_CPPDEFFLAGS $_CPPINCFLAGS")
build_dir = env.subst("$BUILD_DIR")

extra_objects = []

for d in asm_dirs:
    for f in os.listdir(d):
        if f.endswith(".S"):
            src = os.path.join(d, f)
            out_dir = os.path.join(build_dir, "asm_objs", d.replace("/", os.sep))
            os.makedirs(out_dir, exist_ok=True)
            out = os.path.join(out_dir, f + ".o")
            cmd = f'"{cc}" {flags} -c -o "{out}" "{src}"'
            print(f"[fix_asm] Compiling {f}")
            ret = subprocess.call(cmd, shell=True)
            if ret == 0:
                extra_objects.append(out)
            else:
                print(f"[fix_asm] FAILED: {f}")

env.Append(LINKFLAGS=[o for o in extra_objects])