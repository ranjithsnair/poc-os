#!/usr/bin/env python3
"""
Prepares a freshly cloned mlibc/ checkout to build against PoC-OS:
copies our own toolchain/mlibc-sysdeps-pocos/ in as mlibc/sysdeps/pocos
(mlibc's build dispatches sysdeps directories by name, so it has to live
at that exact path inside the checkout), and patches mlibc's top-level
meson.build to recognize host_machine.system() == 'pocos' and pull it in
-- mirroring the existing `elif host_machine.system() == 'demo':
subdir('sysdeps/demo')` block already there for mlibc's own reference
port.

Safe to re-run: the sysdeps copy is always refreshed (so editing
toolchain/mlibc-sysdeps-pocos/ and re-running picks up the changes), and
the meson.build patch is only applied once (idempotent).

Usage: setup_mlibc.py <mlibc_checkout_dir> <our_sysdeps_source_dir>
"""
import shutil
import subprocess
import sys
from pathlib import Path

ANCHOR = "elif host_machine.system() == 'demo'\n"
PATCH = (
    "elif host_machine.system() == 'pocos'\n"
    "\tsubdir('sysdeps/pocos')\n"
)

# Tags sysdeps.cpp implements for real -- kept in sync by hand (it's a
# short, rarely-changing list); everything else mlibc's posix-enabled
# build might reference gets a STUB() body instead. See sysdeps.cpp's
# own doc comment for why this list is deliberately not "everything the
# kernel could support".
IMPLEMENTED_TAGS = [
    "LibcPanic", "LibcLog", "Isatty", "Write", "TcbSet", "AnonAllocate",
    "AnonFree", "Seek", "Exit", "Close", "FutexWake", "FutexWait", "Read",
    "Open", "VmMap", "VmUnmap", "ClockGet", "Stat", "FutexTid",
    # Phase 3 additions -- process control, signals, cwd, termios.
    "Fork", "Waitpid", "Execve", "GetPid", "GetTid", "GetUid", "GetEuid",
    "GetGid", "GetEgid", "Kill", "Dup2", "Pipe", "GetCwd", "Chdir", "Mkdir",
    "Sigaction", "Sigprocmask", "Ioctl", "Tcgetattr", "Tcsetattr", "Tcdrain",
    "Tcflush", "Tcflow", "Tcsendbreak", "Fcntl", "Ttyname", "Fchdir",
    "Fsync", "Chmod", "GetHostname", "GetPpid", "Sysconf", "SetPgid",
    "GetGroups", "Pselect",
    # Dynamic linking (mlibc/options/rtld) -- see toolchain/mlibc-sysdeps-
    # pocos/sysdeps.cpp's Sysdeps<VmProtect> and elf.c's PT_INTERP support.
    "VmProtect",
]


def main():
    if len(sys.argv) != 3:
        print(f"usage: {sys.argv[0]} <mlibc_checkout_dir> <our_sysdeps_source_dir>", file=sys.stderr)
        sys.exit(1)
    mlibc_dir = Path(sys.argv[1])
    sysdeps_src = Path(sys.argv[2])

    dest = mlibc_dir / "sysdeps" / "pocos"
    if dest.exists():
        shutil.rmtree(dest)
    shutil.copytree(sysdeps_src, dest, symlinks=True)
    print(f"copied {sysdeps_src} -> {dest}")

    generated_dir = dest / "generated"
    generated_dir.mkdir()
    (generated_dir / "include" / "mlibc").mkdir(parents=True)
    gen_script = Path(__file__).parent / "gen_mlibc_stubs.py"
    subprocess.run(
        [sys.executable, str(gen_script), str(mlibc_dir), ",".join(IMPLEMENTED_TAGS),
         str(generated_dir / "stubs.cpp"),
         str(generated_dir / "include" / "mlibc" / "generated-tags.hpp")],
        check=True,
    )

    meson_build = mlibc_dir / "meson.build"
    text = meson_build.read_text()
    if "host_machine.system() == 'pocos'" in text:
        print("meson.build already patched for 'pocos'")
    elif ANCHOR not in text:
        print(f"error: couldn't find anchor line {ANCHOR!r} in {meson_build} "
              "-- mlibc's meson.build layout may have changed upstream", file=sys.stderr)
        sys.exit(1)
    else:
        text = text.replace(ANCHOR, PATCH + ANCHOR, 1)
        meson_build.write_text(text)
        print(f"patched {meson_build} to recognize system 'pocos'")

    # mlibc's own bits/types.h already knows int_fastN_t/uint_fastN_t sizes
    # are compiler-specific ("Unfortunately, GCC and Clang disagree about
    # fast types") and skips its self-consistency static_assert for Clang
    # for exactly that reason -- but libc.a itself is built with Clang
    # (see toolchain/pocos.cross-file's doc comment) while our actual cross
    # GCC's own libgcc build later includes these same headers, tripping
    # the *same* known disagreement from the GCC side, which the upstream
    # guard doesn't account for. Broadening the skip to GCC too doesn't
    # weaken anything Clang's side wasn't already exempted from.
    types_h = mlibc_dir / "options/internal/include/bits/types.h"
    types_text = types_h.read_text()
    old_guard = "#ifndef __clang__\n\t__MLIBC_CHECK_TYPE(__mlibc_int_fast8,"
    new_guard = "#if !defined(__clang__) && !defined(__GNUC__)\n\t__MLIBC_CHECK_TYPE(__mlibc_int_fast8,"
    if new_guard in types_text:
        print("bits/types.h already patched to also skip GCC")
    elif old_guard not in types_text:
        print(f"error: couldn't find the fast-type-check guard in {types_h} "
              "-- mlibc's bits/types.h layout may have changed upstream", file=sys.stderr)
        sys.exit(1)
    else:
        types_h.write_text(types_text.replace(old_guard, new_guard, 1))
        print(f"patched {types_h} to also skip the fast-type check for GCC")

    # mlibc's ifuncs_supported detection (meson.build) is a pure *compile*
    # check -- it only verifies the C++ compiler accepts
    # __attribute__((ifunc(...))) syntax, not that anything will ever
    # *process* the resulting R_X86_64_IRELATIVE relocations at runtime.
    # On a real OS that's the dynamic linker's job; PoC-OS has none (no
    # PT_INTERP, no dynamic linker at all -- see elf.h's own doc comment),
    # so an ifunc's GOT slot is never patched and calling it jumps to
    # whatever the linker left there statically, silently corrupting the
    # call instead of crashing outright. Found via mlibc/options/ansi/
    # x86_64/strcmp.cpp's CPU-dispatching ifunc: strcmp("PS1","PS1") was
    # returning -1 (unequal) for identical strings, which cascaded into
    # every hash-table lookup in anything linking against libc.a (bash's
    # own variable table, in this case) silently failing. Forcing this to
    # false makes mlibc fall back to its plain portable implementations
    # everywhere ifuncs would otherwise have been used.
    meson_text = meson_build.read_text()
    old_ifunc = "ifuncs_supported = false\n"
    if "ifuncs_supported = false  # pocos:" in meson_text:
        print("meson.build already patched to force-disable ifuncs")
    elif old_ifunc not in meson_text:
        print(f"error: couldn't find 'ifuncs_supported = false' default in {meson_build} "
              "-- mlibc's meson.build layout may have changed upstream", file=sys.stderr)
        sys.exit(1)
    else:
        meson_text = meson_text.replace(
            "ifuncs_supported = cpp_compiler.compiles(ifunc_check, name: 'C++ compiler supports ifunc')",
            "ifuncs_supported = false  # pocos: no dynamic linker to process IRELATIVE relocations -- see tools/setup_mlibc.py",
            1,
        )
        meson_build.write_text(meson_text)
        print(f"patched {meson_build} to force-disable ifuncs_supported")


if __name__ == "__main__":
    main()
