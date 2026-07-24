#!/usr/bin/env python3
"""
Generates a "cover everything else" sysdeps file for our mlibc port:
mlibc's options/posix/generic/*.cpp files are compiled unconditionally
once the 'posix' option is on (there's no finer-grained way to opt out of
individual POSIX functions), and each one that mentions a sysdep tag our
own sysdeps.hpp doesn't list fails to *compile* (not just link) with
"static assertion failed: Unimplemented sysdep called!" -- so every tag
mlibc might reference has to have *some* SysdepImpl, even if most of them
just abort at runtime if a real caller ever reaches them (STUB(), same
as sysdeps/demo's own approach for the handful it doesn't implement).

Reads mlibc/options/internal/include/mlibc/sysdep-signatures.hpp (the
authoritative list of every SYSDEP_FUNC/SYSDEP_FUNC_RET/
SYSDEP_FUNC_NORETURN declaration) directly, rather than hand-transcribing
~150 signatures, so this can never drift out of sync with whatever mlibc
version is actually checked out. Only ungated entries and ones under
`#if __MLIBC_POSIX_OPTION` or `#if MLIBC_BUILDING_RTLD` are included
(matching the options we actually enable -- see toolchain/pocos.cross-file
and toolchain/mlibc-sysdeps-pocos/meson.build); Linux/glibc/BSD/epoll/
timerfd/signalfd/eventfd/reboot/wrappers/riscv-only entries are skipped
since those options are all disabled for us.

Usage: gen_mlibc_stubs.py <mlibc_checkout_dir> <implemented_tags_csv> <output.cpp> <output_tags.hpp>
"""
import re
import sys
from pathlib import Path

INCLUDE_GATES = {None, "__MLIBC_POSIX_OPTION"}
# MLIBC_BUILDING_RTLD (guards VmReadahead) is deliberately excluded: it's
# only defined for the specific translation units mlibc's meson.build
# compiles into its internal "ld.a" (folded into libc.a for our static
# build, not a real separate ld.so -- see toolchain/mlibc-sysdeps-pocos/
# meson.build's doc comment), so a tag gated on it would need to appear
# in our base-class list *conditionally per translation unit*, which a
# plain #include'd base-class list can't express. Left out until/unless
# an actual build error shows something under options/rtld/ needs it.

LINE_RE = re.compile(
    r"^(SYSDEP_FUNC|SYSDEP_FUNC_RET|SYSDEP_FUNC_NORETURN)\((.*)\);\s*$"
)


def split_args(inner):
    """Splits a macro argument list on top-level commas (mirrors the
    parenthesis/angle-bracket nesting SYSDEP_FUNC's argument lists use,
    e.g. `struct sigevent *__restrict evp` or template-ish types)."""
    parts = []
    depth = 0
    current = ""
    for ch in inner:
        if ch in "(<":
            depth += 1
        elif ch in ")>":
            depth -= 1
        if ch == "," and depth == 0:
            parts.append(current.strip())
            current = ""
        else:
            current += ch
    if current.strip():
        parts.append(current.strip())
    return parts


def parse_signatures(path):
    entries = []  # (macro, rettype_or_None, tag, arg_types_str)
    gate_stack = []

    def current_gate():
        for g in reversed(gate_stack):
            if g is not None:
                return g
        return None

    with open(path) as f:
        for raw_line in f:
            line = raw_line.strip()
            if line.startswith("#ifndef") or line.startswith("#ifdef"):
                # Push a transparent (None) entry rather than ignoring
                # these outright -- sysdep-signatures.hpp uses "#ifndef"
                # for its own top-of-file include guard and "#ifdef
                # __riscv" nested inside other gates, and every #if-like
                # directive needs a matching push so the #endif count
                # stays balanced. current_gate() already skips None
                # entries when searching for the innermost *meaningful*
                # gate, so this correctly falls through to whatever real
                # gate (if any) encloses it instead of masking it.
                gate_stack.append(None)
                continue
            if line.startswith("#if "):
                m = re.match(r"#if\s+(?:defined\()?(\w+)\)?", line)
                gate_stack.append(m.group(1) if m else "UNKNOWN")
                continue
            if line.startswith("#elif"):
                gate_stack[-1] = "UNKNOWN"
                continue
            if line.startswith("#endif"):
                if gate_stack:
                    gate_stack.pop()
                continue

            m = LINE_RE.match(line)
            if not m:
                continue
            macro, args = m.group(1), m.group(2)
            gate = current_gate()
            if gate not in INCLUDE_GATES:
                continue

            parts = split_args(args)
            if macro == "SYSDEP_FUNC_RET":
                rettype, tag = parts[0], parts[1]
                arg_parts = parts[2:]
            else:
                rettype, tag = None, parts[0]
                arg_parts = parts[1:]
            entries.append((macro, rettype, tag, arg_parts))
    return entries


def arg_types_only(arg_parts):
    """Strips trailing parameter *names* off each argument, keeping just
    the type -- e.g. 'const char *pathname' -> 'const char *'. A stub
    definition (unlike a real implementation) never needs to reference
    its parameters by name."""
    types = []
    for arg in arg_parts:
        arg = arg.strip()
        # Peel off a trailing identifier (the parameter name), leaving
        # the type and any '*'/'&' immediately before it attached to the type.
        m = re.match(r"^(.*?)([A-Za-z_]\w*)(\[\])?$", arg)
        if not m:
            types.append(arg)
            continue
        prefix, _name, array = m.group(1), m.group(2), m.group(3)
        # If the "name" we peeled off is actually a type keyword (no
        # preceding type at all, e.g. a bare 'void'), keep it whole.
        if prefix.strip() == "" and not array:
            types.append(arg)
        else:
            types.append((prefix + (array or "")).strip())
    return ", ".join(types) if types else "void"


def main():
    if len(sys.argv) != 5:
        print(f"usage: {sys.argv[0]} <mlibc_dir> <implemented_tags_csv> <out.cpp> <out_tags.hpp>",
              file=sys.stderr)
        sys.exit(1)
    mlibc_dir = Path(sys.argv[1])
    implemented = set(t for t in sys.argv[2].split(",") if t)
    out_cpp = Path(sys.argv[3])
    out_tags = Path(sys.argv[4])

    sig_path = mlibc_dir / "options/internal/include/mlibc/sysdep-signatures.hpp"
    entries = parse_signatures(sig_path)

    seen = set()
    stub_lines = []
    tag_lines = []
    for macro, rettype, tag, arg_parts in entries:
        if tag in seen:
            continue
        seen.add(tag)
        tag_lines.append(tag)
        if tag in implemented:
            continue  # sysdeps.cpp defines this one for real

        types = arg_types_only(arg_parts)
        if macro == "SYSDEP_FUNC_NORETURN":
            stub_lines.append(
                f"[[noreturn]] void Sysdeps<{tag}>::operator()({types}) {{ STUB(); }}"
            )
        elif macro == "SYSDEP_FUNC_RET":
            stub_lines.append(
                f"{rettype} Sysdeps<{tag}>::operator()({types}) {{ STUB(); }}"
            )
        else:
            stub_lines.append(
                f"int Sysdeps<{tag}>::operator()({types}) {{ STUB(); }}"
            )

    out_cpp.write_text(
        "/* Generated by tools/gen_mlibc_stubs.py -- do not edit by hand.\n"
        " * Regenerate via the top-level Makefile's mlibc setup step.\n"
        " *\n"
        " * Every tag here compiles (satisfying whatever options/posix/generic/*.cpp\n"
        " * references it) but aborts at runtime if actually called -- see STUB()\n"
        " * below, same approach sysdeps/demo takes for the handful it stubs.\n"
        " * sysdeps.cpp implements the ones PoC-OS actually has real syscalls for;\n"
        " * this file exists so *compiling* mlibc doesn't require implementing the\n"
        " * other ~130 POSIX functions bash/gcc will likely never call. */\n"
        "#include <bits/ensure.h>\n"
        "#include <mlibc/all-sysdeps.hpp>\n\n"
        "#define STUB() ({ __ensure(!\"STUB function was called\"); __builtin_unreachable(); })\n\n"
        "namespace mlibc {\n\n"
        + "\n".join(stub_lines) + "\n"
        "\n} // namespace mlibc\n"
    )

    out_tags.write_text(
        "// Generated by tools/gen_mlibc_stubs.py -- do not edit by hand.\n"
        "// Every tag mlibc's posix-enabled build might reference (see sysdeps.cpp's\n"
        "// doc comment). Included from include/mlibc/sysdeps.hpp.\n"
        + ",\n".join(f"\t{t}" for t in tag_lines) + "\n"
    )
    print(f"{len(seen)} tags total, {len(seen) - len(implemented & seen)} stubbed, "
          f"{len(implemented & seen)} implemented for real")


if __name__ == "__main__":
    main()
