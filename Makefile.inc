# -*- Makefile -*-
# Copyright (C) 2004 Christophe Varoqui, <christophe.varoqui@opensvc.com>
#

#
# Uncomment to disable libdmmp support
# ENABLE_LIBDMMP = 0
#
# Uncomment to disable dmevents polling support
# ENABLE_DMEVENTS_POLL = 0
#
# Readline library to use, libedit, libreadline, or empty
# Caution: Using libreadline may make the multipathd binary undistributable,
# see https://github.com/opensvc/multipath-tools/issues/36
READLINE :=

# List of scsi device handler modules to load on boot, e.g.
# SCSI_DH_MODULES_PRELOAD := scsi_dh_alua scsi_dh_rdac
SCSI_DH_MODULES_PRELOAD :=

EXTRAVERSION := $(shell rev=$$(git rev-parse --short=7 HEAD 2>/dev/null); echo $${rev:+-g$$rev})

# PKG_CONFIG must be read from the environment to enable compilation
# in Debian multiarch setups
PKG_CONFIG	?= pkg-config

ifeq ($(TOPDIR),)
	TOPDIR	= ..
endif
ifneq ($(CREATE_CONFIG),1)
include $(TOPDIR)/config.mk
endif

# Paths. All these can be overridden on the "make" command line.
prefix		:=
# Prefix for binaries
exec_prefix	:= $(prefix)
# Prefix for non-essential libraries (libdmmp)
usr_prefix	:= $(if $(prefix),$(prefix),/usr)
# Prefix for configuration files (multipath.conf)
etc_prefix	:= $(prefix)
# Where to install systemd-related files. systemd is usually installed under /usr
# Note: systemd installations with "split-usr=true" use separate "prefixdir" and
# "rootprefixdir". Our systemd_prefix corresponds to "prefixdir".
# In this case, override only unitdir, libudevdir and sys_execprefix below
# to use systemd's "rootprefixdir" instead of $(systemd_prefix)
systemd_prefix  := /usr
# Prefix for binaries that are owned by other packages (including systemd)
sys_execprefix  := /usr

# Make sure all prefix variables end in "/"
append-slash = $(1)$(if $(filter %/,$(1)),,/)
override prefix          := $(call append-slash,$(prefix))
override exec_prefix     := $(call append-slash,$(exec_prefix))
override usr_prefix      := $(call append-slash,$(usr_prefix))
override etc_prefix      := $(call append-slash,$(etc_prefix))
override systemd_prefix  := $(call append-slash,$(systemd_prefix))
override sys_execprefix  := $(call append-slash,$(sys_execprefix))

unitdir		:= $(systemd_prefix)lib/systemd/system
tmpfilesdir	:= $(systemd_prefix)lib/tmpfiles.d
modulesloaddir	:= $(systemd_prefix)lib/modules-load.d
libudevdir	:= $(systemd_prefix)lib/udev
udevrulesdir	:= $(libudevdir)/rules.d
bindir		:= $(exec_prefix)sbin
mandir		:= $(usr_prefix)share/man
LIB		:= $(if $(shell test -d /lib64 && echo 1),lib64,lib)
syslibdir	:= $(prefix)$(LIB)
usrlibdir	:= $(usr_prefix)$(LIB)
includedir	:= $(usr_prefix)include
pkgconfdir	:= $(usrlibdir)/pkgconfig
plugindir       := $(prefix)$(LIB)/multipath
configdir       := $(etc_prefix)etc/multipath/conf.d
configfile      := $(etc_prefix)etc/multipath.conf
statedir        := $(etc_prefix)etc/multipath
runtimedir      := $(if $(shell test -L /var/run -o ! -d /var/run && echo 1),/run,/var/run)
devmapper_incdir := $(or $(shell $(PKG_CONFIG) --variable=includedir devmapper),/usr/include)
libudev_incdir	:= $(or $(shell $(PKG_CONFIG) --variable=includedir libudev),/usr/include)
kernel_incdir	:= /usr/include
sysdir_bin      := $(sys_execprefix)bin

ifeq ($(V),)
Q		:= @
# make's "Entering directory" messages are confusing in parallel mode
#MAKEFLAGS	= --no-print-directory
endif

GZIP_PROG	:= gzip -9 -c
RM		:= rm -f
LN		:= ln -sf
INSTALL_PROGRAM	:= install

ORIG_CPPFLAGS    := $(CPPFLAGS)
ORIG_CFLAGS      := $(CFLAGS)
ORIG_LDFLAGS     := $(LDFLAGS)

SYSTEMD_CPPFLAGS := $(if $(SYSTEMD),-DUSE_SYSTEMD=$(SYSTEMD))
SYSTEMD_LIBDEPS := $(if $(SYSTEMD),$(if $(shell test $(SYSTEMD) -gt 209 && echo 1),-lsystemd,-lsystemd-daemon))
MODPROBE_UNIT := $(shell test "0$(SYSTEMD)" -lt 245 2>/dev/null || \
			echo "modprobe@dm_multipath.service")

OPTFLAGS	:= -O2 -g $(STACKPROT) --param=ssp-buffer-size=4

# Set WARN_ONLY=1 to avoid compilation erroring out due to warnings. Useful during development.
WARN_ONLY       :=
ERROR           := $(if $(WARN_ONLY),,error=)
WERROR          := $(if $(WARN_ONLY),,-Werror)
WARNFLAGS	:= $(WERROR) -Wall -Wextra -Wformat=2 $(WFORMATOVERFLOW) -W$(ERROR)implicit-int \
		  -W$(ERROR)implicit-function-declaration -W$(ERROR)format-security \
		  $(WNOCLOBBERED) -W$(ERROR)cast-qual $(ERROR_DISCARDED_QUALIFIERS) $(W_URCU_TYPE_LIMITS)

CPPFLAGS	:= $(FORTIFY_OPT) $(CPPFLAGS) $(D_URCU_VERSION) \
		   -D_FILE_OFFSET_BITS=64 \
		   -DBIN_DIR=\"$(bindir)\" -DMULTIPATH_DIR=\"$(TGTDIR)$(plugindir)\" \
		   -DRUNTIME_DIR=\"$(runtimedir)\" -DCONFIG_DIR=\"$(TGTDIR)$(configdir)\" \
		   -DDEFAULT_CONFIGFILE=\"$(TGTDIR)$(configfile)\" -DSTATE_DIR=\"$(TGTDIR)$(statedir)\" \
		   -DEXTRAVERSION=\"$(EXTRAVERSION)\" -MMD -MP
CFLAGS		:= -std=$(C_STD) $(CFLAGS) $(OPTFLAGS) $(WARNFLAGS) -pipe \
		   -fexceptions
BIN_CFLAGS	:= -fPIE -DPIE
LIB_CFLAGS	:= -fPIC
SHARED_FLAGS	:= -shared
LDFLAGS		:= $(LDFLAGS) -Wl,-z,relro -Wl,-z,now -Wl,-z,defs
BIN_LDFLAGS	:= -pie

# Source code directories. Don't modify.

multipathdir	:= $(TOPDIR)/libmultipath
daemondir	:= $(TOPDIR)/multipathd
mpathutildir	:= $(TOPDIR)/libmpathutil
mpathpersistdir	:= $(TOPDIR)/libmpathpersist
mpathcmddir	:= $(TOPDIR)/libmpathcmd
mpathvaliddir	:= $(TOPDIR)/libmpathvalid
thirdpartydir	:= $(TOPDIR)/third-party
libdmmpdir	:= $(TOPDIR)/libdmmp
nvmedir		:= $(TOPDIR)/libmultipath/nvme

# Common code for libraries - library Makefiles just set DEVLIB
# SONAME defaults to 0 (we use version scripts)
SONAME := 0
LIBS = $(DEVLIB).$(SONAME)
VERSION_SCRIPT = $(DEVLIB:%.so=%.version)
NV_VERSION_SCRIPT = $(DEVLIB:%.so=%-nv.version)

%.o:	%.c
	@echo building $@ because of $?
	$(Q)$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

%.abi:  %.so.0
	$(Q)abidw $< >$@

%.abi:  %.so
	$(Q)abidw $< >$@

%-nv.version:	%.version
	@echo creating $@ from $<
	@printf 'NOVERSION {\nglobal:\n' >$@
	@grep -P '^[ \t]+[a-zA-Z_][a-zA-Z0-9_]*;' $< >>$@
	@printf 'local:\n\t*;\n};\n' >>$@

%:	%.in
	@echo creating $@
	$(Q)sed -e 's:@CONFIGFILE@:'$(TGTDIR)$(configfile)':g' \
		-e 's:@CONFIGDIR@:'$(TGTDIR)$(configdir)':g' \
		-e 's:@STATE_DIR@:'$(TGTDIR)$(statedir)':g' \
		-e 's:@BINDIR@:'$(TGTDIR)$(bindir)':g' \
		-e 's:@SYSDIR_BIN@:'$(sysdir_bin)': g' \
		-e 's:@RUNTIME_DIR@:'$(runtimedir)':g' \
		-e 's/@MODPROBE_UNIT@/'$(MODPROBE_UNIT)'/g' \
		$< >$@
