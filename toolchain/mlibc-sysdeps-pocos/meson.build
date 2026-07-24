# Mirrors sysdeps/demo/meson.build's structure -- see mlibc's own
# porting guide (book/) for what each block does. rtld_sources aren't
# meaningfully used here since PoC-OS has no dynamic linker (elf.c only
# ever loads static ET_EXEC binaries), but mlibc's build still expects
# every sysdeps directory to define them.
sysdep_supported_options = {
	'posix': true,
}

# generated/stubs.cpp doesn't exist until tools/gen_mlibc_stubs.py has
# run (see tools/setup_mlibc.py, which does this right after copying this
# directory in) -- it's derived from mlibc's own sysdep-signatures.hpp,
# so it lives alongside the mlibc checkout rather than in our own
# persistent toolchain/mlibc-sysdeps-pocos/ source.
rtld_sources += files(
	'sysdeps.cpp',
	'syscall.cpp',
	'ioctl.cpp',
	'sigreturn_trampoline.S',
	'generated/stubs.cpp',
)
rtld_include_dirs += include_directories('include', 'generated/include')

libc_sources += files(
	'entry.cpp',
	'sysdeps.cpp',
	'syscall.cpp',
	'ioctl.cpp',
	'sigreturn_trampoline.S',
	'generated/stubs.cpp',
)
libc_include_dirs += include_directories('include', 'generated/include')

if not no_headers
	install_headers(
		'include/abi-bits/auxv.h',
		'include/abi-bits/blkcnt_t.h',
		'include/abi-bits/blksize_t.h',
		'include/abi-bits/clockid_t.h',
		'include/abi-bits/dev_t.h',
		'include/abi-bits/errno.h',
		'include/abi-bits/fcntl.h',
		'include/abi-bits/gid_t.h',
		'include/abi-bits/ino_t.h',
		'include/abi-bits/limits.h',
		'include/abi-bits/mode_t.h',
		'include/abi-bits/nlink_t.h',
		'include/abi-bits/pid_t.h',
		'include/abi-bits/seek-whence.h',
		'include/abi-bits/signal.h',
		'include/abi-bits/stat.h',
		'include/abi-bits/uid_t.h',
		'include/abi-bits/vm-flags.h',
		'include/abi-bits/sigevent.h',
		'include/abi-bits/sigval.h',
		'include/abi-bits/sa_family_t.h',
		'include/abi-bits/sockaddr_storage.h',
		'include/abi-bits/sig-limits.h',
		'include/abi-bits/suseconds_t.h',
		'include/abi-bits/access.h',
		'include/abi-bits/socklen_t.h',
		'include/abi-bits/socket.h',
		'include/abi-bits/poll.h',
		'include/abi-bits/resource.h',
		'include/abi-bits/in.h',
		'include/abi-bits/rlim_t.h',
		'include/abi-bits/utsname.h',
		'include/abi-bits/fd_set.h',
		'include/abi-bits/sem.h',
		'include/abi-bits/time.h',
		'include/abi-bits/ipc.h',
		'include/abi-bits/statvfs.h',
		'include/abi-bits/fsblkcnt_t.h',
		'include/abi-bits/fsfilcnt_t.h',
		'include/abi-bits/shm.h',
		'include/abi-bits/termios.h',
		'include/abi-bits/msg.h',
		'include/abi-bits/mqueue.h',
		'include/abi-bits/utmp-defines.h',
		'include/abi-bits/utmpx.h',
		'include/abi-bits/sigset_t.h',
		'include/abi-bits/wait.h',
		'include/abi-bits/aio.h',
		subdir: 'abi-bits',
		follow_symlinks: true
	)

	# Not an abi-bits file (mlibc doesn't define this hook for us at all --
	# see include/sys/ioctl.h's doc comment): bash's jobs.c unconditionally
	# expects a real <sys/ioctl.h> to exist, the way it does on any Unix.
	install_headers(
		'include/sys/ioctl.h',
		subdir: 'sys'
	)
endif

if not headers_only
	crt = custom_target('crt1',
		build_by_default: true,
		command: c_compiler.cmd_array() + ['-c', '-o', '@OUTPUT@', '@INPUT@'],
		input: 'crt1.S',
		output: 'crt1.o',
		install: true,
		install_dir: get_option('libdir')
	)
	# See crti.S's doc comment: GCC's crtbegin.o/crtend.o only contribute
	# the *middle* of _init/_fini and expect a real libc's crti.o/crtn.o
	# to supply the enclosing prologue/epilogue.
	crti = custom_target('crti',
		build_by_default: true,
		command: c_compiler.cmd_array() + ['-c', '-o', '@OUTPUT@', '@INPUT@'],
		input: 'crti.S',
		output: 'crti.o',
		install: true,
		install_dir: get_option('libdir')
	)
	crtn = custom_target('crtn',
		build_by_default: true,
		command: c_compiler.cmd_array() + ['-c', '-o', '@OUTPUT@', '@INPUT@'],
		input: 'crtn.S',
		output: 'crtn.o',
		install: true,
		install_dir: get_option('libdir')
	)
endif
