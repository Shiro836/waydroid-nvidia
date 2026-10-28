# NVIDIA's own EGL/GLES as an Android vendor EGL driver (P7). Two pieces, both Android-agnostic of Waydroid:
#   libEGL_nvnative.so, libGLESv1_CM_nvnative.so, libGLESv2_nvnative.so   the front (Android EGL loader <-> glvnd vendor)
#   libnvnative-egl-android.so + nvnative_egl_android.json               NVIDIA external platform for EGL_PLATFORM_ANDROID_KHR
#
#   make sources    fetch the pinned third-party commits below into $(SRC_ROOT) (depth 1)
#   make            host + android + android32 + check
#   make host       out/host/   glibc, ASan/UBSan, for test/host-window on the host GPU
#   make android    out/android/    NDK r30 API 33 x86_64 (guest /vendor_extra/lib64[/egl])
#   make android32  out/android32/  NDK r30 API 33 i686   (guest /vendor_extra/lib[/egl])
#   make check      the built artifacts: exports, SONAME, NEEDED, ELF class, no glibc references
#   make test       test/host-window on the host GPU (and the Y-orientation variant), see README.md
#   make front-fence-check / driver-check / stress    the D1 checks (README "D1", "Tests")
#
# Third-party sources are read from the git object store of pinned clones (never from a working tree), like the
# HAL's Mesa sources; nothing third-party is checked in:
#   egl-x11      v1.0.6-5 328a5e1b src/base/* (NVIDIA platform base, Apache-2.0), src/x11/driver-platform-surface.h
#                (main 2026-08-25; v1.0.6 + ec2d477 eglSetDamageRegionKHR -> EGL_BAD_MATCH on non-postable surfaces,
#                c83c2e8 eglSwapInterval with no current context -> the driver's EGL_BAD_CONTEXT: both dEQP-EGL fixes)
#   eglexternalplatform 1.2.1 cf9f1058  interface/*.h (MIT)
#   libglvnd     v1.7.0  faa23f21  include/glvnd/libeglabi.h, GLdispatchABI.h, GL/gl.h + glext.h it includes (MIT)
#   frameworks/native android13-release 2fe7eddf  opengl/libs/EGL/egl_entries.in (Apache-2.0) -> the front's export list

.DEFAULT_GOAL := all
NDK      ?= /opt/android-ndk
TC       := $(NDK)/toolchains/llvm/prebuilt/linux-x86_64
NDK_CC   := $(TC)/bin/x86_64-linux-android33-clang
NDK_CC32 := $(TC)/bin/i686-linux-android33-clang
NDK_CXX  := $(TC)/bin/x86_64-linux-android33-clang++
NDK_CXX32:= $(TC)/bin/i686-linux-android33-clang++
READELF  := $(TC)/bin/llvm-readelf
SYSROOT  := $(TC)/sysroot
HOSTCC   ?= gcc
PYTHON   := python3 -I

SRC_ROOT ?= ../../build/p7/src
EGLX11   ?= $(SRC_ROOT)/egl-x11
EGLX11_SHA := 328a5e1bf457c1467ae2fe91bc43b37827d58bb7
EPLAT    ?= $(SRC_ROOT)/eglexternalplatform
EPLAT_SHA := cf9f10589775c1b4d90f1df6f417f8cf1bed7ec6
GLVND    ?= $(SRC_ROOT)/libglvnd
GLVND_SHA := faa23f21fc677af5792825dc30cb1ccef4bf33a6
FWN      ?= $(SRC_ROOT)/fw-native
FWN_SHA  := 2fe7eddf185ecb63223eeadc58804f9bf6cbd426

# where `make sources` fetches each pinned commit from (depth 1, into $(SRC_ROOT); an existing clone is used when it
# has the commit)
SOURCES := $(EGLX11)=https://github.com/NVIDIA/egl-x11.git=$(EGLX11_SHA) \
           $(EPLAT)=https://github.com/NVIDIA/eglexternalplatform.git=$(EPLAT_SHA) \
           $(GLVND)=https://gitlab.freedesktop.org/glvnd/libglvnd.git=$(GLVND_SHA) \
           $(FWN)=https://android.googlesource.com/platform/frameworks/native=$(FWN_SHA)

GEN := out/gen
BASE_SRCS := config-list.c platform-base.c platform-utils.c refcountobj.c
BASE_HDRS := config-list.h glvnd_list.h platform-base.h platform-impl.h platform-utils.h refcountobj.h
GEN_FILES := $(addprefix $(GEN)/eplbase/,$(BASE_SRCS) $(BASE_HDRS)) $(GEN)/eplbase/driver-platform-surface.h \
             $(GEN)/include/eglexternalplatform.h $(GEN)/include/eglexternalplatformversion.h \
             $(GEN)/include/glvnd/libeglabi.h $(GEN)/include/glvnd/GLdispatchABI.h $(GEN)/include/GL/gl.h \
             $(GEN)/include/GL/glext.h $(GEN)/front_entries.h

.PHONY: sources
sources:
	@set -e; for p in $(SOURCES); do d=$${p%%=*}; rest=$${p#*=}; url=$${rest%=*}; sha=$${rest##*=}; \
	    if git -C $$d cat-file -e $$sha^{commit} 2>/dev/null; then echo "sources: $$d has $$sha"; continue; fi; \
	    [ ! -e $$d ] || [ -d $$d/.git ] || { echo "sources: $$d exists and is not a git clone; move it aside" >&2; exit 1; }; \
	    mkdir -p $$d; git -C $$d init -q; \
	    python3 ../../tools/fetch.py retry -- git -C $$d fetch -q --depth 1 $$url $$sha; \
	    git -C $$d cat-file -e $$sha^{commit}; echo "sources: $$d at $$sha ($$url)"; done

# extract <file> of <repo>@<sha>; loud remedy when the pinned commit is not there
define EXTRACT
	@mkdir -p $(dir $@)
	@git -C $(1) cat-file -e $(2):$(3) 2>/dev/null || { echo "$(1) does not contain $(2):$(3). Remedy: make sources" \
	    "(fetches every pinned commit) or set $(4)=<a clone with $(2)>" >&2; exit 1; }
	git -C $(1) show $(2):$(3) > $@.tmp && mv $@.tmp $@
endef
$(GEN)/eplbase/driver-platform-surface.h:
	$(call EXTRACT,$(EGLX11),$(EGLX11_SHA),src/x11/driver-platform-surface.h,EGLX11)
$(GEN)/eplbase/%:
	$(call EXTRACT,$(EGLX11),$(EGLX11_SHA),src/base/$*,EGLX11)
$(GEN)/include/eglexternalplatform.h $(GEN)/include/eglexternalplatformversion.h:
	$(call EXTRACT,$(EPLAT),$(EPLAT_SHA),interface/$(@F),EPLAT)
$(GEN)/include/glvnd/%.h:
	$(call EXTRACT,$(GLVND),$(GLVND_SHA),include/glvnd/$*.h,GLVND)
$(GEN)/include/GL/%.h:
	$(call EXTRACT,$(GLVND),$(GLVND_SHA),include/GL/$*.h,GLVND)
$(GEN)/egl_entries.in:
	$(call EXTRACT,$(FWN),$(FWN_SHA),opengl/libs/EGL/egl_entries.in,FWN)
$(GEN)/front_entries.h: $(GEN)/egl_entries.in tools/gen-front-entries.py
	$(PYTHON) tools/gen-front-entries.py $< $@

# Path-independent output (the same bytes from any checkout path).
HERE       := $(patsubst %/,%,$(dir $(abspath $(lastword $(MAKEFILE_LIST)))))
PREFIX_MAP = -ffile-prefix-map=$(HERE)=nvidia-prolapse/egl -ffile-prefix-map=$(DRIVER)=nvidia-prolapse-driver
# the Vulkan HAL's pinned AOSP/minigbm headers and the gralloc backend live in the driver repository
# (../../tools/driver_source.py: build/nvidia-prolapse-src, or NVIDIA_PROLAPSE_SRC / DRIVER=<checkout>)
DRIVER     ?= $(shell python3 $(HERE)/../../tools/driver_source.py path)
HAL        := $(DRIVER)/hal
HAL_INC    := -I$(HAL)/include/android -I$(HAL)/include/minigbm
COMMON := -std=gnu11 -D_GNU_SOURCE -DHAVE_TYPEOF -fPIC -fvisibility=hidden -O2 -g $(PREFIX_MAP) -Wall -Wextra -Werror \
          -Wno-unused-parameter -Wno-missing-field-initializers -Wno-sign-compare \
          -Iinclude -Isrc -I$(GEN)/eplbase -I$(GEN)/include -I$(GEN) $(HAL_INC)
# the third-party platform base is compiled as-is (no warnings of ours enforced on it)
BASE_FLAGS := -std=gnu11 -D_GNU_SOURCE -DHAVE_TYPEOF -fPIC -fvisibility=hidden -O2 -g $(PREFIX_MAP) -w \
              -I$(GEN)/eplbase -I$(GEN)/include
LDSO := -shared -Wl,-z,defs -Wl,--no-undefined
PLAT_SRCS := src/platform.c src/window.c src/gralloc.c
HDRS := src/nvn.h include/nvn_android.h $(HAL)/include/minigbm/cros_gralloc_handle.h

# per-target toolchain settings
host_CC := $(HOSTCC)
host_CFLAGS := -fsanitize=address,undefined -fno-omit-frame-pointer -I/usr/include/libdrm
host_LIBS := -ldl -lpthread
android_CC := $(NDK_CC)
android_CFLAGS := -I$(SYSROOT)/usr/include/drm
android_LIBS := -llog -ldl
android32_CC := $(NDK_CC32)
android32_CFLAGS := -I$(SYSROOT)/usr/include/drm -D_FILE_OFFSET_BITS=64
android32_LIBS := -llog -ldl

LIBS := libEGL_nvnative.so libGLESv1_CM_nvnative.so libGLESv2_nvnative.so libnvnative-egl-android.so

define TARGET_RULES
out/$(1)/base/%.o: $(GEN)/eplbase/%.c $(GEN_FILES)
	@mkdir -p $$(@D)
	$$($(1)_CC) $$(BASE_FLAGS) $$($(1)_CFLAGS) -c -o $$@ $$<
out/$(1)/libnvnative-egl-android.so: $(PLAT_SRCS) $(HDRS) $(addprefix out/$(1)/base/,$(BASE_SRCS:.c=.o))
	@mkdir -p $$(@D)
	$$($(1)_CC) $$(COMMON) $$($(1)_CFLAGS) $$(LDSO) -Wl,-soname,libnvnative-egl-android.so -o $$@ $(PLAT_SRCS) \
	    $(addprefix out/$(1)/base/,$(BASE_SRCS:.c=.o)) $$($(1)_LIBS)
out/$(1)/libEGL_nvnative.so: src/front.c $(GEN_FILES)
	@mkdir -p $$(@D)
	$$($(1)_CC) $$(COMMON) $$($(1)_CFLAGS) $$(LDSO) -Wl,-soname,libEGL_nvnative.so -o $$@ src/front.c $$($(1)_LIBS)
out/$(1)/libGLESv1_CM_nvnative.so out/$(1)/libGLESv2_nvnative.so: src/gles_empty.c
	@mkdir -p $$(@D)
	$$($(1)_CC) $$(COMMON) $$($(1)_CFLAGS) $$(LDSO) -Wl,-soname,$$(@F) -o $$@ src/gles_empty.c
endef
$(eval $(call TARGET_RULES,host))
$(eval $(call TARGET_RULES,android))
$(eval $(call TARGET_RULES,android32))

# Y-orientation measurement variant of the platform (EGL_SURFACE_Y_INVERTED_NVX = EGL_FALSE), host only.
out/host-yfalse/libnvnative-egl-android.so: $(PLAT_SRCS) $(HDRS) $(addprefix out/host/base/,$(BASE_SRCS:.c=.o))
	@mkdir -p $(@D)
	$(HOSTCC) $(COMMON) $(host_CFLAGS) -DNVN_Y_INVERTED=EGL_FALSE $(LDSO) -Wl,-soname,libnvnative-egl-android.so \
	    -o $@ $(PLAT_SRCS) $(addprefix out/host/base/,$(BASE_SRCS:.c=.o)) $(host_LIBS)

# Stress build (not shipped, README "Tests"): skips the GPU wait on an already-signalled dequeue fence, so an early
# release fence is never masked by a wait; it made the D1 race of test/host-window ~10x more frequent in-process.
out/host-amplify/libnvnative-egl-android.so: $(PLAT_SRCS) $(HDRS) $(addprefix out/host/base/,$(BASE_SRCS:.c=.o))
	@mkdir -p $(@D)
	$(HOSTCC) $(COMMON) $(host_CFLAGS) -DNVN_TEST_SKIP_SIGNALLED_WAIT $(LDSO) -Wl,-soname,libnvnative-egl-android.so \
	    -o $@ $(PLAT_SRCS) $(addprefix out/host/base/,$(BASE_SRCS:.c=.o)) $(host_LIBS)

# ---- tests ----
GRALLOC := $(DRIVER)/gralloc
out/host/host-window: test/host-window.c include/nvn_android.h $(HAL)/include/minigbm/cros_gralloc_handle.h $(GEN_FILES)
	@mkdir -p $(@D)
	$(HOSTCC) -std=gnu11 -D_GNU_SOURCE -O1 -g $(PREFIX_MAP) -Wall -Wextra -Werror -Wno-unused-parameter \
	    $(host_CFLAGS) -Iinclude $(HAL_INC) -I$(GRALLOC)/include -o $@ test/host-window.c -ldl -lpthread
out/host/fence-probe out/host/fence-race out/host/front-api: out/host/%: test/%.c $(GEN_FILES)
	@mkdir -p $(@D)
	$(HOSTCC) -std=gnu11 -D_GNU_SOURCE -O1 -g $(PREFIX_MAP) -Wall -Wextra -Werror -Wno-unused-parameter \
	    $(host_CFLAGS) -Iinclude -I$(GRALLOC)/include -o $@ $< -ldl
# driver reproducer (default: stock libglvnd dlopened, nothing of ours; --front: through our front): FENCE-CONTRACT.md §3.3
out/host/fence-first-wait: test/fence-first-wait.c
	@mkdir -p $(@D)
	$(HOSTCC) -std=gnu11 -D_GNU_SOURCE -O1 -g $(PREFIX_MAP) -Wall -Wextra -Werror -Wno-unused-parameter \
	    $(host_CFLAGS) -o $@ $< -ldl -lpthread
$(GRALLOC)/out/host/libgbm_mesa_wrapper.so:
	$(MAKE) -C $(GRALLOC) host

# Guest test (SurfaceFlinger layer through libgui; ABI-fragile by design, see test/sf_client.h)
GUEST_CXXFLAGS := -std=c++17 -O1 -g $(PREFIX_MAP) -Wall -Wextra -Werror -Wno-unused-parameter -fno-exceptions \
                  -Iinclude $(HAL_INC) -static-libstdc++
SYSLIBS ?= $(GEN)/syslibs
# link-time copies of the guest's own /system libraries (never shipped), read from the image without mounting it
SYSTEM_IMG ?= /var/lib/waydroid/images/system.img
SYSLIB_NAMES := libgui.so libutils.so libbinder.so libui.so libnativewindow.so
$(SYSLIBS)/.stamp:
	@mkdir -p $(SYSLIBS)/lib $(SYSLIBS)/lib64
	@set -e; for a in lib lib64; do for l in $(SYSLIB_NAMES); do \
	    debugfs -R "dump /system/$$a/$$l $(SYSLIBS)/$$a/$$l" $(SYSTEM_IMG) >/dev/null 2>&1; \
	    [ -s $(SYSLIBS)/$$a/$$l ] || { echo "cannot read /system/$$a/$$l from $(SYSTEM_IMG) (set SYSTEM_IMG)" >&2; exit 1; }; \
	done; done; touch $@
out/android/eglwindow: test/eglwindow.cpp test/sf_client.h include/nvn_android.h $(SYSLIBS)/.stamp
	@mkdir -p $(@D)
	$(NDK_CXX) $(GUEST_CXXFLAGS) -o $@ test/eglwindow.cpp -L$(SYSLIBS)/lib64 -lgui -lutils -lbinder -lui \
	    -lnativewindow -ldl -llog -Wl,--allow-shlib-undefined
out/android32/eglwindow: test/eglwindow.cpp test/sf_client.h include/nvn_android.h $(SYSLIBS)/.stamp
	@mkdir -p $(@D)
	$(NDK_CXX32) $(GUEST_CXXFLAGS) -o $@ test/eglwindow.cpp -L$(SYSLIBS)/lib -lgui -lutils -lbinder -lui \
	    -lnativewindow -ldl -llog -Wl,--allow-shlib-undefined

.PHONY: all host android android32 check test guest clean driver-check front-fence-check stress
host: $(addprefix out/host/,$(LIBS)) out/host/host-window out/host/fence-probe out/host/fence-race out/host/fence-first-wait out/host/front-api \
      out/host-yfalse/libnvnative-egl-android.so
android: $(addprefix out/android/,$(LIBS))
android32: $(addprefix out/android32/,$(LIBS))
guest: out/android/eglwindow out/android32/eglwindow out/android/sfimage out/android32/sfimage
# sfimage: SurfaceFlinger RenderEngine's EGL/AHB sequence through Android's own libEGL (whatever ro.hardware.egl selects)
out/android/sfimage: test/sfimage.c
	@mkdir -p $(@D)
	$(NDK_CC) -O1 -g -Wall -Wextra -Werror -o $@ test/sfimage.c -lEGL -lGLESv3 -lnativewindow
out/android32/sfimage: test/sfimage.c
	@mkdir -p $(@D)
	$(NDK_CC32) -O1 -g -Wall -Wextra -Werror -o $@ test/sfimage.c -lEGL -lGLESv3 -lnativewindow
all: host android android32 check

# ---- artifact checks ----
ALLOWED_NEEDED := libc.so libdl.so liblog.so libm.so
check: android android32 $(GEN)/front_entries.h
	@$(PYTHON) tools/check-artifacts.py --readelf $(READELF) --entries $(GEN)/front_entries.h \
	    --allowed "$(ALLOWED_NEEDED)" out/android:ELF64 out/android32:ELF32

# The NVIDIA driver (eglcore) leaks a few dbus allocations on the host; nothing of ours is suppressed.
TEST_ENV := DISPLAY= WAYLAND_DISPLAY= LSAN_OPTIONS=suppressions=$(HERE)/test/lsan.supp
HW := ./out/host/host-window --gralloc $(GRALLOC)/out/host/libgbm_mesa_wrapper.so --front out/host/libEGL_nvnative.so
test: host $(GRALLOC)/out/host/libgbm_mesa_wrapper.so
	$(TEST_ENV) $(HW) --platform-lib out/host/libnvnative-egl-android.so
	$(TEST_ENV) $(HW) --platform-lib out/host/libnvnative-egl-android.so --consumer-process
	@echo "== Y-orientation measurement: EGL_SURFACE_Y_INVERTED_NVX = EGL_FALSE build (reports, does not assert)"
	$(TEST_ENV) $(HW) --platform-lib out/host-yfalse/libnvnative-egl-android.so --y-measure --frames 2
	$(TEST_ENV) ./out/host/fence-probe --front out/host/libEGL_nvnative.so --dmabuf $(GRALLOC)/out/host/libgbm_mesa_wrapper.so
	$(TEST_ENV) ./out/host/fence-race --front out/host/libEGL_nvnative.so --gralloc $(GRALLOC)/out/host/libgbm_mesa_wrapper.so --wait-cpu
	$(TEST_ENV) ./out/host/front-api --front out/host/libEGL_nvnative.so --platform-lib out/host/libnvnative-egl-android.so

# Every fence-first-wait sequence (test/fence-first-wait.c MODES[]).
FFW_MODES := first-wait warm warm-nodup warm-after-wait same-thread switch-ctx switch-after-wait rebind release-thread \
             main-warm-ctx two-waits client-wait-first khr-warm khr-fence khr-fence-warm migrate migrate-warm \
             khr-warm-nodup pbuffer-first warm-then-pbuffer second-display terminate-reinit reinit-noterm
DRIVER_CHECK_ROUNDS ?= 10
FRONT_FENCE_ROUNDS ?= 20
# driver-check: the driver's native-fence contract (FENCE-CONTRACT.md §1.2) through STOCK libglvnd, nothing of ours.
# A detector of the driver defect D1: it FAILS on 615.71.09 (24/24 early in first-wait, release-thread, main-warm-ctx,
# two-waits, client-wait-first, khr-fence, pbuffer-first; 0 in the others) and passes only on a driver that keeps it.
driver-check: out/host/fence-first-wait
	@fail=0; for m in $(FFW_MODES); do \
	    out=$$($(TEST_ENV) ./out/host/fence-first-wait --rounds $(DRIVER_CHECK_ROUNDS) --mode $$m \
	        $$([ $$m = client-wait-first ] && echo --a-iters 4000000) 2>&1); rc=$$?; \
	    echo "$$out" | grep "^fence-first-wait: mode"; [ $$rc -eq 0 ] || fail=1; done; exit $$fail
# front-fence-check: every sequence through OUR front on the Android platform display (the way an Android app or
# SurfaceFlinger reaches the driver). The front starts each thread's timeline in eglMakeCurrent (FENCE-CONTRACT.md §6a):
# must be 0 early in every mode. Then fence-race's consumer thread (its first native-fence operation is an eglWaitSync,
# the sequence that showed D1 in the platform harness), through the front.
front-fence-check: out/host/fence-first-wait out/host/fence-race out/host/libEGL_nvnative.so out/host/libnvnative-egl-android.so \
                   $(GRALLOC)/out/host/libgbm_mesa_wrapper.so
	@fail=0; for m in $(FFW_MODES); do \
	    out=$$($(TEST_ENV) ./out/host/fence-first-wait --front out/host/libEGL_nvnative.so \
	        --platform-lib out/host/libnvnative-egl-android.so --rounds $(FRONT_FENCE_ROUNDS) --mode $$m \
	        $$([ $$m = client-wait-first ] && echo --a-iters 4000000) 2>&1); rc=$$?; \
	    echo "$$out" | grep "^fence-first-wait: mode" || echo "$$out" | tail -5; [ $$rc -eq 0 ] || fail=1; done; \
	out=$$($(TEST_ENV) ./out/host/fence-race --front out/host/libEGL_nvnative.so \
	    --gralloc $(GRALLOC)/out/host/libgbm_mesa_wrapper.so --consumer-thread --iters 200000 2>&1); rc=$$?; \
	echo "$$out" | grep "^fence-race: \(release\|consumer read\)"; [ $$rc -eq 0 ] || fail=1; exit $$fail

# Host stress (README "Tests"): tools/host-window-stress.sh, pass bar 0 failed runs. STRESS_PLATFORM=amplify uses the
# investigation platform build (skips the GPU wait on an already-signalled release fence).
STRESS_RUNS ?= $(if $(filter amplify,$(STRESS_PLATFORM)),300,200)
STRESS_JOBS ?= 8
STRESS_CONSUMERS ?= gl-write gl-read vk-write vk-read cpu
STRESS_PLATFORM ?= normal
stress_plat = $(if $(filter amplify,$(STRESS_PLATFORM)),out/host-amplify,out/host)/libnvnative-egl-android.so
stress: host out/host-amplify/libnvnative-egl-android.so $(GRALLOC)/out/host/libgbm_mesa_wrapper.so
	$(MAKE) -C $(HAL) out/host/vulkan.nvidia.so
	@fail=0; for c in $(STRESS_CONSUMERS); do ./tools/host-window-stress.sh --platform-lib $(stress_plat) --consumer $$c \
	    --runs $(STRESS_RUNS) --jobs $(STRESS_JOBS) --process mix || fail=1; done; exit $$fail

clean:
	rm -rf out
