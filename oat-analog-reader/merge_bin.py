# merge_bin.py — PlatformIO post-build hook.
# -----------------------------------------------------------------------------
# Produces ONE merged factory image per chip at out/firmware-<mcu>.bin, flashable
# at offset 0x0 by ESP Web Tools. It uses the env's OWN FLASH_EXTRA_IMAGES
# (bootloader, partitions, boot_app0 — each already at its correct per-chip
# offset) plus ESP32_APP_OFFSET, so the offsets are never hand-maintained. This
# is why classic-ESP32 (bootloader @ 0x1000) and the native-USB chips
# (bootloader @ 0x0) both come out right with no special-casing.
Import("env")  # noqa: F821  (injected by PlatformIO)
import os


def _flash_freq(board):
    raw = str(board.get("build.f_flash", "40000000L")).replace("L", "")
    return {"80000000": "80m", "40000000": "40m",
            "26000000": "26m", "20000000": "20m"}.get(raw, "40m")


def after_build(source, target, env):
    board = env.BoardConfig()
    mcu = board.get("build.mcu", "esp32")
    build_dir = env.subst("$BUILD_DIR")
    out_dir = os.path.join(env.subst("$PROJECT_DIR"), "out")
    os.makedirs(out_dir, exist_ok=True)
    merged = os.path.join(out_dir, "firmware-%s.bin" % mcu)

    # (offset, image) pairs the env flashes, in order: bootloader / partitions /
    # boot_app0 from FLASH_EXTRA_IMAGES, then the app at ESP32_APP_OFFSET.
    images = [(str(off), env.subst(img)) for off, img in env.get("FLASH_EXTRA_IMAGES", [])]
    images.append((env.subst("$ESP32_APP_OFFSET"),
                   os.path.join(build_dir, "firmware.bin")))

    cmd = [env.subst("$PYTHONEXE"), "-m", "esptool", "--chip", mcu, "merge_bin",
           "-o", merged,
           "--flash_mode", board.get("build.flash_mode", "dio"),
           "--flash_freq", _flash_freq(board),
           "--flash_size", board.get("upload.flash_size", "4MB")]
    for off, img in images:
        cmd += [off, img]

    env.Execute(env.VerboseAction(
        " ".join('"%s"' % c if " " in c else c for c in cmd),
        "Merging factory image -> %s" % os.path.relpath(merged)))


env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", after_build)  # noqa: F821
