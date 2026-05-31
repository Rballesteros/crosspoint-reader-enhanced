"""
PlatformIO pre-build script: apply CrossPoint's JPEGDEC patches via `git apply`.

The upstream JPEGDEC pin still has the wild-pointer + DC-write bugs in
JPEGDecodeMCU_P that surface when EIGHT_BIT_GRAYSCALE decodes a 3-component
progressive JPEG (each Y MCU drags two MCU_SKIP calls behind it for Cb/Cr).
The patches in `scripts/jpegdec_patches/` carry the fix; this script applies
each one against the libdep working tree.

Idempotency by reset-then-apply: the patched file(s) are restored to the pinned
pristine state (`git checkout`) and the series is applied fresh on every run.
This avoids fragile "already applied?" detection -- the patches have overlapping
hunk context (so they can't be reverse-checked one at a time) and the pre-script
runs more than once per build.

Patches live in `scripts/jpegdec_patches/` as one-commit-per-fix files
(see the file headers for context). Applied in lexical order.
"""

Import("env")  # noqa: F821 (SCons-injected global)
import os
import subprocess
import sys


PATCH_DIR = os.path.join(env["PROJECT_DIR"], "scripts", "jpegdec_patches")  # noqa: F821


def patch_jpegdec(env):
    libdeps_dir = os.path.join(env["PROJECT_DIR"], ".pio", "libdeps")
    if not os.path.isdir(libdeps_dir):
        return
    patches = _patch_files()
    for env_dir in os.listdir(libdeps_dir):
        jpeg_dir = os.path.join(libdeps_dir, env_dir, "JPEGDEC")
        if not os.path.isdir(os.path.join(jpeg_dir, ".git")):
            continue
        _apply_all(jpeg_dir, patches)


def _patch_files():
    if not os.path.isdir(PATCH_DIR):
        raise RuntimeError(
            "JPEGDEC patches missing -- aborting build (expected directory %s)"
            % PATCH_DIR
        )
    patches = sorted(
        os.path.join(PATCH_DIR, name)
        for name in os.listdir(PATCH_DIR)
        if name.endswith(".patch")
    )
    if not patches:
        raise RuntimeError(
            "JPEGDEC patches missing -- aborting build (no .patch files in %s)"
            % PATCH_DIR
        )
    return patches


def _apply_all(jpeg_dir, patches):
    names = ", ".join(os.path.basename(p) for p in patches)
    targets = _patch_targets(patches)
    # Restore the patched files to the pinned pristine state, then apply fresh.
    subprocess.run(["git", "checkout", "--"] + targets, cwd=jpeg_dir, check=True)
    result = subprocess.run(
        ["git", "apply"] + patches, cwd=jpeg_dir, capture_output=True, text=True
    )
    if result.returncode != 0:
        # The libdep source has diverged from what the patches expect.
        sys.stderr.write(
            "ERROR: JPEGDEC patches do not apply cleanly:\n%s%s\n"
            % (result.stdout, result.stderr)
        )
        raise SystemExit(1)
    print("Applied JPEGDEC patches: %s" % names)


def _patch_targets(patches):
    """Files touched by the patches (parsed from their `+++ b/<path>` headers)."""
    targets = []
    for patch in patches:
        with open(patch, encoding="utf-8") as fh:
            for line in fh:
                if line.startswith("+++ b/"):
                    path = line[len("+++ b/"):].strip()
                    if path and path not in targets:
                        targets.append(path)
    return targets


patch_jpegdec(env)  # noqa: F821
