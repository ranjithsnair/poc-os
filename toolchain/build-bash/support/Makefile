#
# Simple Makefile for the support programs.
#
# documentation support: man2html
# testing support: printenv recho zecho xcase
#
# bashbug.sh lives here (created by configure), but bashbug is created by
# the top-level makefile
#
# Currently only man2html is built
#
# Copyright (C) 1998-2021 Free Software Foundation, Inc.

#   This program is free software: you can redistribute it and/or modify
#   it under the terms of the GNU General Public License as published by
#   the Free Software Foundation, either version 3 of the License, or
#   (at your option) any later version.

#   This program is distributed in the hope that it will be useful,
#   but WITHOUT ANY WARRANTY; without even the implied warranty of
#   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#   GNU General Public License for more details.

#   You should have received a copy of the GNU General Public License
#   along with this program.  If not, see <http://www.gnu.org/licenses/>.

#
# Boilerplate
#
topdir = ../../src/bash-5.2
srcdir = ../../src/bash-5.2/support
VPATH = ../../src/bash-5.2/support
BUILD_DIR = /Users/ranjith/Git/lucy-os/toolchain/build-bash

RM	= rm -f
SHELL	= /bin/sh
CC = x86_64-elf-gcc
CC_FOR_BUILD = gcc

EXEEXT = 

#
# Compiler options:
#
PROFILE_FLAGS = 

CFLAGS = -ffreestanding -fno-pic -fno-pie -m64 -O2 -Wno-implicit-function-declaration -DNEED_EXTERN_PC
CFLAGS_FOR_BUILD = -g
BASE_CFLAGS_FOR_BUILD = -g
CPPFLAGS = 
CPPFLAGS_FOR_BUILD = 
LOCAL_CFLAGS = 
STYLE_CFLAGS = 
DEFS = -DHAVE_CONFIG_H
LOCAL_DEFS = -DSHELL

LIBS = -ldl -lc /Users/ranjith/Git/lucy-os/toolchain/cross/lib/gcc/x86_64-elf/14.2.0/crtend.o /Users/ranjith/Git/lucy-os/toolchain/sysroot/usr/lib/crtn.o
LIBS_FOR_BUILD = ${LIBS}		# XXX

LOCAL_LDFLAGS = 
LDFLAGS = -nostdlib -static -T /Users/ranjith/Git/lucy-os/userland/linker.ld /Users/ranjith/Git/lucy-os/toolchain/sysroot/usr/lib/crt1.o /Users/ranjith/Git/lucy-os/toolchain/sysroot/usr/lib/crti.o /Users/ranjith/Git/lucy-os/toolchain/cross/lib/gcc/x86_64-elf/14.2.0/crtbegin.o -L./lib/termcap $(LOCAL_LDFLAGS)
LDFLAGS_FOR_BUILD =  $(LOCAL_LDFLAGS) $(CFLAGS_FOR_BUILD)

INCLUDES = -I${BUILD_DIR} -I${topdir}

BASE_CCFLAGS = ${PROFILE_FLAGS} $(DEFS) $(LOCAL_DEFS) $(SYSTEM_FLAGS) \
	  ${INCLUDES} $(STYLE_CFLAGS) $(LOCAL_CFLAGS)

CCFLAGS = $(BASE_CCFLAGS) $(CPPFLAGS) $(CFLAGS)
CCFLAGS_FOR_BUILD = $(BASE_CCFLAGS) $(CPPFLAGS_FOR_BUILD) $(CFLAGS_FOR_BUILD)

SRC1 =	man2html.c
OBJ1 =	man2html.o

.c.o:
	$(RM) $@
	$(CC_FOR_BUILD) -c $(CCFLAGS_FOR_BUILD) $<

all: man2html$(EXEEXT)

man2html$(EXEEXT): $(OBJ1)
	$(CC_FOR_BUILD) $(LDFLAGS_FOR_BUILD) $(OBJ1) -o $@ ${LIBS_FOR_BUILD}

clean:
	$(RM) man2html$(EXEEXT) $(OBJ1)

distclean maintainer-clean mostlyclean: clean
	$(RM) bash.pc bashbug.sh

man2html.o: man2html.c
