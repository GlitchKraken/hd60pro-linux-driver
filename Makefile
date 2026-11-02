#
# Linux driver for Elgato Game Capture HD60 Pro (YUAN MZ0380).
#
# Builds mz0380.ko. This driver is written for the HD60 Pro (MZ0380) and
# shares no source with any other capture driver; see README's Provenance
# section for what was consulted during reverse engineering and what was not.
#

mz0380-objs := \
	src/mz0380-cards.o \
	src/mz0380-pci.o \
	src/mz0380-params-base.o \
	src/mz0380-params-signal.o \
	src/mz0380-params-windows.o \
	src/mz0380-core.o \
	src/mz0380-snapshot.o \
	src/mz0380-proc-diagnostics.o \
	src/mz0380-proc-debug.o \
	src/mz0380-proc.o \
	src/mz0380-controls-read.o \
	src/mz0380-controls-write.o \
	src/mz0380-mailbox.o \
	src/mz0380-video.o \
	src/mz0380-video-state.o \
	src/mz0380-vb2.o \
	src/mz0380-no-signal.o \
	src/mz0380-signal.o \
	src/mz0380-mst3367.o \
	src/mz0380-mst3367-debug.o \
	src/mz0380-mst3367-signal.o \
	src/mz0380-mst3367-bitbang.o \
	src/mz0380-hdmitx.o \
	src/mz0380-dma.o \
	src/mz0380-dma-extent.o \
	src/mz0380-dma-stream.o \
	src/mz0380-dma-drain.o \
	src/mz0380-dma-nosg.o \
	src/mz0380-fw.o \
	src/mz0380-audio.o


ccflags-y += -I$(src)/src

obj-m += mz0380.o

#
# Kernel feature probes (kbuild pass only - $(srctree) is the kernel tree).
#
# Probe the headers instead of testing LINUX_VERSION_CODE. The exact release
# that dropped a symbol is rarely worth guessing, and a wrong guess produces a
# module that builds but misbehaves, which is far more expensive to diagnose
# than a build error.
#
ifneq ($(KERNELRELEASE),)

# vb2_ops->wait_prepare/wait_finish and the vb2_ops_wait_* helpers exist up to
# 6.x. vb2 core took the q->lock handling over and they were removed; present
# in 6.18, gone in 7.2.
MZ0380_HAVE_VB2_WAIT_OPS := $(shell grep -sqw vb2_ops_wait_prepare \
	$(srctree)/include/media/videobuf2-v4l2.h && echo 1)
ifeq ($(MZ0380_HAVE_VB2_WAIT_OPS),1)
ccflags-y += -DMZ0380_HAVE_VB2_WAIT_OPS
endif

# Linux 7.2 split the old system_wq into explicit percpu/default-unbound
# queues and warns whenever new work is queued on the compatibility alias.
# Probe the declaration so the same source keeps building on older kernels.
MZ0380_HAVE_SYSTEM_DFL_WQ := $(shell grep -sqw system_dfl_wq \
	$(srctree)/include/linux/workqueue.h && echo 1)
ifeq ($(MZ0380_HAVE_SYSTEM_DFL_WQ),1)
ccflags-y += -DMZ0380_HAVE_SYSTEM_DFL_WQ
endif

endif

.PHONY: all all-kernels kernels kcheck clean objclean distclean fw-install \
        load load-streaming unload tarball list probe \
        capture capture-h264 capture-audio \
        install uninstall dkms-install dkms-uninstall

TARFILES = Makefile *.md *.sh src scripts

#
# Which kernel to build against.
#
# Defaults to the running kernel, which is the only one whose module can
# actually be insmod'd, so the scripts' plain `make` stays correct. Override
# to cross-build for another installed kernel:
#
#     make KVER=6.18.42-1-cachyos-lts
#     make all-kernels          # every installed kernel that has headers
#
KVER     ?= $(shell uname -r)
KVERSION  = $(KVER)
KBUILD    = /lib/modules/$(KVER)/build

# Installed kernels that can actually be built against, newest first.
KVERS_BUILDABLE = $(shell for d in /lib/modules/*/; do \
	k=$${d%/}; k=$${k##*/}; \
	[ -e "$$d/build/Makefile" ] && echo "$$k"; \
	done | sort -rV)

#
# Toolchain auto-detect.
# CachyOS / Arch / Fedora ship a clang-built kernel; gcc cannot compile
# modules against it (rejects -mretpoline-external-thunk etc).
# If clang + ld.lld are present, use them; override on CLI to force.
#
ifeq ($(shell command -v clang >/dev/null && command -v ld.lld >/dev/null && echo yes),yes)
KBUILD_FLAGS ?= CC=clang LD=ld.lld
else
KBUILD_FLAGS ?=
endif

#
# M=$(CURDIR), never M=$(PWD). PWD is an environment variable, and sudo's
# env_reset drops it - so `sudo make install` passed kbuild an empty M=, which
# it reads as "build the kernel itself" and answers with a syncconfig failure
# about arch/arm/crypto/Kconfig. The same command works unprivileged, because
# there the shell had exported PWD. CURDIR is set by make itself and survives
# both.
#
all: kcheck
	$(MAKE) -C $(KBUILD) M=$(CURDIR) $(KBUILD_FLAGS) modules

#
# Fail with a usable message instead of kbuild's bare "No such file or
# directory". A running kernel whose package has been upgraded away leaves no
# /lib/modules entry at all, so nothing out-of-tree can be built OR loaded on
# that boot - the only fixes are a reboot into an installed kernel or a
# downgrade, and the error has to say so.
#
kcheck:
	@if [ ! -e "$(KBUILD)/Makefile" ]; then \
		echo "ERROR: no kernel build tree for '$(KVER)'"; \
		echo "       ($(KBUILD) is missing)"; \
		if [ "$(KVER)" = "$$(uname -r)" ]; then \
			echo; \
			echo "  The RUNNING kernel has no /lib/modules entry - its package was"; \
			echo "  upgraded or removed. No out-of-tree module can be built or loaded"; \
			echo "  on this boot. Reboot into one of the kernels below, or reinstall"; \
			echo "  the running kernel's headers."; \
		fi; \
		echo; \
		echo "  buildable kernels: $(KVERS_BUILDABLE)"; \
		echo "  cross-build with:  make KVER=<version>"; \
		exit 1; \
	fi

#
# Build for every installed kernel that has headers, keeping one
# $(KO_DIR)/mz0380-<kver>.ko per kernel.
#
# The copies live in a subdirectory, and the loop uses objclean rather than
# `make clean`: kbuild's clean deletes *.ko RECURSIVELY under M=, so both
# keeping the copies alongside and keeping them in $(KO_DIR) lose the previous
# kernel's module to the next kernel's clean.
#
# The running kernel is built LAST so the plain mz0380.ko that the load scripts
# insmod is the one matching this boot.
#
KO_DIR = ko

all-kernels:
	@set -e; \
	mkdir -p $(KO_DIR); \
	running="$$(uname -r)"; \
	others=""; mine=""; \
	for k in $(KVERS_BUILDABLE); do \
		if [ "$$k" = "$$running" ]; then mine="$$k"; else others="$$others $$k"; fi; \
	done; \
	[ -n "$$others$$mine" ] || { echo "no buildable kernel found"; exit 1; }; \
	for k in $$others $$mine; do \
		echo "=== $$k ==="; \
		$(MAKE) --no-print-directory objclean >/dev/null; \
		$(MAKE) --no-print-directory KVER=$$k all; \
		cp mz0380.ko $(KO_DIR)/mz0380-$$k.ko; \
		echo "    -> $(KO_DIR)/mz0380-$$k.ko"; \
	done; \
	if [ -z "$$mine" ]; then \
		rm -f mz0380.ko; \
		echo; \
		echo "NOTE: running kernel '$$running' has no build tree, so mz0380.ko was"; \
		echo "      removed - a module with the wrong vermagic only fails at insmod"; \
		echo "      with 'Invalid module format', which reads like a driver bug."; \
		echo "      Reboot into a built kernel and run make; the per-kernel copies"; \
		echo "      in $(KO_DIR)/ are kept either way."; \
	fi

kernels:
	@echo "running:   $$(uname -r)"
	@echo "buildable: $(KVERS_BUILDABLE)"
	@echo "default:   $(KVER)"

# Drop just enough build state to force a full rebuild against a different
# kernel, without kbuild's recursive walk. Top level only, and $(KO_DIR) is
# untouched by construction.
objclean:
	@rm -f *.o .*.o *.mod *.mod.c .*.cmd mz0380.ko \
		src/*.o src/.*.o src/.*.cmd \
		Module.symvers modules.order modules.builtin*
	@rm -rf .tmp_versions

# clean must work even when the running kernel has no build tree, otherwise
# the one command that unsticks a broken tree is itself unavailable.
clean:
	@if [ -e "$(KBUILD)/Makefile" ]; then \
		$(MAKE) -C $(KBUILD) M=$(CURDIR) $(KBUILD_FLAGS) clean; \
	else \
		echo "no build tree for '$(KVER)' - removing build products directly"; \
		rm -f *.o *.ko *.mod *.mod.c .*.cmd src/*.o src/.*.cmd \
			Module.symvers modules.order; \
		rm -rf .tmp_versions; \
	fi

# clean deliberately leaves $(KO_DIR) alone - all-kernels runs clean between
# kernels, so anything it removed there would not survive the next pass.
distclean: clean
	rm -rf $(KO_DIR)


#
# Firmware install. The MZ0380.HD.HEX blob ships with the Windows
# driver and is not redistributable here. Copy it from the extracted
# installer (or from a Windows install) into /lib/firmware/mz0380/.
#
FW_DIR ?= /lib/firmware/mz0380
# M170: this checked for MZ0380.HD.HEX - the blob the driver STOPPED USING when
# the upload path was deleted from the tree. load-streaming depends on it, so
# `make load-streaming` failed outright on a correctly-installed system, telling
# the user to go and extract a firmware image that nothing reads.
#
# The card boots its own flash. The only file the driver ever requests is the
# MZ0380.FW.TXT sidecar, and only to compare a version string and warn on a
# mismatch. It is optional: without it the driver says so and carries on.
fw-install:
	@if [ ! -f $(FW_DIR)/MZ0380.FW.TXT ]; then \
		echo "note: $(FW_DIR)/MZ0380.FW.TXT is absent."; \
		echo "      The driver does not need it to run - the card boots its"; \
		echo "      own flash image. It is a version sidecar: without it the"; \
		echo "      expected-vs-actual firmware check is simply skipped."; \
		echo "      Create it with the card's version if you want the check:"; \
		echo "          echo 01.11 | sudo tee $(FW_DIR)/MZ0380.FW.TXT"; \
	else \
		echo "firmware version sidecar: $(FW_DIR)/MZ0380.FW.TXT = $$(cat $(FW_DIR)/MZ0380.FW.TXT)"; \
	fi

#
# Load helpers.
#
load: all
	sudo dmesg -c >/dev/null
	sudo modprobe -a videodev videobuf2-v4l2 videobuf2-vmalloc \
		v4l2-dv-timings snd-pcm
	sudo insmod ./mz0380.ko procfs_verbosity=2

# M166/M167: the shipping defaults ARE the capture configuration, so this no
# longer has to spell them out. Kept because the name is in the README and in
# muscle memory; it is now a plain load with the dependencies pulled in.
load-streaming: all fw-install
	sudo modprobe -a videodev videobuf2-v4l2 videobuf2-vmalloc \
		v4l2-dv-timings snd-pcm
	sudo insmod ./mz0380.ko procfs_verbosity=2

unload:
	sudo rmmod mz0380 || true
	sync

tarball:
	tar zcf ../mz0380-dev-$(shell date +%Y%m%d-%H%M%S).tgz $(TARFILES)

#
# Smoke tests (require an actual capture + HDMI source).
#
list:
	v4l2-ctl --list-devices

probe:
	@echo "Use: sudo scripts/mz0380-m55-real-capture.sh 6 45"

# M130: the payload is planar I420, not an H.264 bitstream. The old name said
# h264 and the old argument asked for 6 frames from a card that delivers one.
capture: all
	sudo scripts/mz0380-m55-real-capture.sh 1 45

capture-h264: capture

capture-audio:
	@echo "ALSA PCM DMA is not implemented; enable_audio must remain disabled."
	@false

#
# Installation.
#
# `make install` is the quick path: drop the module where depmod can find it,
# install the modprobe.d file, and rebuild the dependency tables. It is tied to
# ONE kernel - the next kernel upgrade leaves it behind. For anything other
# than a quick test, use `make dkms-install`, which rebuilds automatically.
#
VERSION      = 0.1.0
MODDIR       = /lib/modules/$(KVER)/kernel/drivers/media/pci/mz0380
MODPROBE_DIR = /etc/modprobe.d
DKMS_SRC     = /usr/src/mz0380-$(VERSION)

install: all
	install -d $(DESTDIR)$(MODDIR)
	install -m 644 mz0380.ko $(DESTDIR)$(MODDIR)/
	install -d $(DESTDIR)$(MODPROBE_DIR)
	install -m 644 mz0380.modprobe.conf $(DESTDIR)$(MODPROBE_DIR)/mz0380.conf
	@[ -n "$(DESTDIR)" ] || depmod -a $(KVER)
	@echo
	@echo "installed for kernel $(KVER)."
	@echo "The card autoloads on its PCI ID - MODULE_DEVICE_TABLE is set - so a"
	@echo "reboot is enough. To load it now:  sudo modprobe mz0380"
	@echo
	@echo "This binds to ONE kernel. Use 'make dkms-install' to survive upgrades."

uninstall:
	rm -f $(DESTDIR)$(MODDIR)/mz0380.ko
	rm -f $(DESTDIR)$(MODPROBE_DIR)/mz0380.conf
	-rmdir $(DESTDIR)$(MODDIR) 2>/dev/null
	@[ -n "$(DESTDIR)" ] || depmod -a $(KVER)
	@echo "uninstalled from kernel $(KVER)."

#
# DKMS. Copies the tree to /usr/src so the module is rebuilt on kernel upgrade.
# The source list is explicit rather than a bare `cp -r .`: the working tree
# carries multi-megabyte RE material (re-dump/, windowsDriver/, reference/) that
# has no business in /usr/src.
#
dkms-install: dkms.conf
	@command -v dkms >/dev/null || { echo "dkms is not installed"; exit 1; }
	install -d $(DKMS_SRC)
	cp -r Makefile dkms.conf mz0380.modprobe.conf src $(DKMS_SRC)/
	dkms add     -m mz0380 -v $(VERSION)
	dkms build   -m mz0380 -v $(VERSION)
	dkms install -m mz0380 -v $(VERSION)
	install -d $(MODPROBE_DIR)
	install -m 644 mz0380.modprobe.conf $(MODPROBE_DIR)/mz0380.conf
	@echo
	@echo "DKMS install complete. The module now rebuilds on kernel upgrades."
	@echo "Load it with:  sudo modprobe mz0380"

dkms-uninstall:
	-dkms remove -m mz0380 -v $(VERSION) --all
	rm -rf $(DKMS_SRC)
	rm -f $(MODPROBE_DIR)/mz0380.conf
	@echo "DKMS package removed."
