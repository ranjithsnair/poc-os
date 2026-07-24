#
# Sample makefile for bash loadable builtin development
#
# Copyright (C) 2015-2022 Free Software Foundation, Inc.     

#   This program is free software: you can redistribute it and/or modify
#   it under the terms of the GNU General Public License as published by
#   the Free Software Foundation, either version 3 of the License, or
#   (at your option) any later version.
#
#   This program is distributed in the hope that it will be useful,
#   but WITHOUT ANY WARRANTY; without even the implied warranty of
#   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#   GNU General Public License for more details.
#
#   You should have received a copy of the GNU General Public License
#   along with this program.  If not, see <http://www.gnu.org/licenses/>.
#

PACKAGE = bash
VERSION = 5.2-release

PACKAGE_NAME = bash
PACKAGE_VERSION = 5.2-release

# Include some boilerplate Gnu makefile definitions.
prefix = /usr/local

exec_prefix = ${prefix}
bindir = ${exec_prefix}/bin
libdir = ${exec_prefix}/lib
infodir = ${datarootdir}/info
includedir = ${prefix}/include

datarootdir = ${prefix}/share

loadablesdir = ${libdir}/bash
headersdir = $(includedir)/$(PACKAGE_NAME)

topdir = ../../../src/bash-5.2
BUILD_DIR = /Users/ranjith/Git/lucy-os/toolchain/build-bash
srcdir = ../../../src/bash-5.2/examples/loadables
VPATH = ../../../src/bash-5.2/examples/loadables

# Support an alternate destination root directory for package building
DESTDIR =

INSTALL = /usr/bin/install -c
INSTALL_PROGRAM = ${INSTALL}
INSTALL_SCRIPT = ${INSTALL}
INSTALL_DATA = ${INSTALL} -m 644
INSTALLMODE= -m 0755


CC = x86_64-elf-gcc
RM = rm -f

SHELL = /bin/sh

host_os = elf
host_cpu = x86_64
host_vendor = pc

CFLAGS = -ffreestanding -fno-pic -fno-pie -m64 -O2 -Wno-implicit-function-declaration -DNEED_EXTERN_PC
LOCAL_CFLAGS = 
DEFS = -DHAVE_CONFIG_H
LOCAL_DEFS = -DSHELL

CPPFLAGS = 

BASHINCDIR = ${topdir}/include

SUPPORT_SRC = $(topdir)/support/

LIBBUILD = ${BUILD_DIR}/lib

INTL_LIBSRC = ${topdir}/lib/intl
INTL_BUILDDIR = ${LIBBUILD}/intl
INTL_INC = 
LIBINTL_H = 

CCFLAGS = $(DEFS) $(LOCAL_DEFS) $(LOCAL_CFLAGS) $(CFLAGS)

#
# These values are generated for configure by ${topdir}/support/shobj-conf.
# If your system is not supported by that script, but includes facilities for
# dynamic loading of shared objects, please update the script and send the
# changes to bash-maintainers@gnu.org.
#
SHOBJ_CC = x86_64-elf-gcc
SHOBJ_CFLAGS = -fpic
SHOBJ_LD = ${CC}
SHOBJ_LDFLAGS = -shared -nostdlib -static -T /Users/ranjith/Git/lucy-os/userland/linker.ld /Users/ranjith/Git/lucy-os/toolchain/sysroot/usr/lib/crt1.o /Users/ranjith/Git/lucy-os/toolchain/sysroot/usr/lib/crti.o /Users/ranjith/Git/lucy-os/toolchain/cross/lib/gcc/x86_64-elf/14.2.0/crtbegin.o -L./lib/termcap
SHOBJ_XLDFLAGS = 
SHOBJ_LIBS = 
SHOBJ_STATUS = supported

INC = -I$(headersdir) -I$(headersdir)/include -I$(headersdir)/builtins

.c.o:
	$(SHOBJ_CC) $(SHOBJ_CFLAGS) $(CCFLAGS) $(INC) -c -o $@ $<
