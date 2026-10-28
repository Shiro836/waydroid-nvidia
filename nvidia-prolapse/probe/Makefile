# Host build of the compute probe and the rtld-audit helper.
# The bionic (NDK) build of vkcompute lands here in P1.
CC ?= gcc
CFLAGS ?= -O1 -g -Wall
all: vkcompute vkcaps vkexts eglgles libldaudit.so
compute.spv: compute.comp
	glslc -fshader-stage=compute -o $@ $<
compute_spv.h: compute.spv
	python3 -I -c 'import sys;d=open(sys.argv[1],"rb").read();print("static const unsigned char compute_spv[%d] __attribute__((aligned(4))) = {%s};"%(len(d),",".join(str(b) for b in d)))' $< > $@
# HAL_INC: pinned AOSP hwvulkan.h/hardware.h copies, for the --hal bootstrap
# (the driver repository's hal/include: ../../tools/driver_source.py, or DRIVER=<checkout>)
DRIVER  ?= $(shell python3 $(CURDIR)/../../tools/driver_source.py path)
HAL_INC := -I$(DRIVER)/hal/include/android
vkcompute: vkcompute.c compute_spv.h
	$(CC) $(CFLAGS) $(HAL_INC) -o $@ vkcompute.c -ldl
# vkexts: which device extensions vkCreateDevice accepts (--all/--each/--bisect/--with)
vkexts: vkexts.c
	$(CC) $(CFLAGS) $(HAL_INC) -Wextra -o $@ vkexts.c -ldl
# vkcaps: external memory/semaphore/fence + DRM modifier capability and round-trip probe
vkcaps: vkcaps.c
	$(CC) $(CFLAGS) -Wextra -o $@ vkcaps.c -lvulkan -ldl
libldaudit.so: ldaudit.c
	$(CC) $(CFLAGS) -shared -fPIC -o $@ $<
clean:
	rm -f vkcompute vkcaps vkexts nvexts eglgles eglgles-android eglgles-android32 libldaudit.so compute.spv compute_spv.h nvprobe appsandbox nvprobe32 nvexts32
	rm -rf glesapp/out hwuiapp/out
	rm -rf gen
.PHONY: all clean

# --- bionic (NDK) build of the same probe: this is nvprobe, the P1 in-guest binary ---
NDK ?= /opt/android-ndk
NDK_CC ?= $(NDK)/toolchains/llvm/prebuilt/linux-x86_64/bin/x86_64-linux-android33-clang
nvprobe: vkcompute.c compute_spv.h
	$(NDK_CC) -O1 -g -Wall -fPIE -pie $(HAL_INC) -o $@ vkcompute.c -ldl

nvexts: vkexts.c
	$(NDK_CC) -O1 -g -Wall -Wextra -fPIE -pie $(HAL_INC) -o $@ vkexts.c -ldl

# 32-bit x86 (i686) builds of the same probes: a 32-bit process loads /vendor_extra/lib (P6).
# appsandbox stays 64-bit: bionic installs the same dual-arch app filter (x86_64 primary, x86
# secondary) in both zygotes (seccomp_policy.cpp), and the kernel applies its x86 half to the
# 32-bit program appsandbox execs.
NDK_CC32 ?= $(NDK)/toolchains/llvm/prebuilt/linux-x86_64/bin/i686-linux-android33-clang
nvprobe32: vkcompute.c compute_spv.h
	$(NDK_CC32) -O1 -g -Wall -fPIE -pie $(HAL_INC) -o $@ vkcompute.c -ldl
nvexts32: vkexts.c
	$(NDK_CC32) -O1 -g -Wall -Wextra -fPIE -pie $(HAL_INC) -o $@ vkexts.c -ldl

# --- eglgles: NVIDIA's own EGL + GLES without libglvnd (P7 gate G7a) ---
# Our own minimal glvnd vendor host calls libEGL_nvidia's __egl_Main directly. The vendor ABI header
# (include/glvnd/libeglabi.h) and the Khronos EGL/GLES headers come from a pinned libglvnd clone, the
# version the host's libglvnd package is built from (1.7.0).
GLVND_URL    ?= https://gitlab.freedesktop.org/glvnd/libglvnd.git
GLVND_COMMIT ?= faa23f21fc677af5792825dc30cb1ccef4bf33a6
GLVND_DIR    ?= ../../build/libglvnd
$(GLVND_DIR)/.pinned:
	test -d $(GLVND_DIR)/.git || git clone -q $(GLVND_URL) $(GLVND_DIR)
	git -C $(GLVND_DIR) checkout -q $(GLVND_COMMIT)
	@test "$$(git -C $(GLVND_DIR) rev-parse HEAD)" = "$(GLVND_COMMIT)" || { echo "libglvnd checkout is not at $(GLVND_COMMIT)" >&2; exit 1; }
	touch $@
GLVND_INC = -I$(GLVND_DIR)/include
eglgles: eglgles.c $(GLVND_DIR)/.pinned
	$(CC) $(CFLAGS) -Wextra $(GLVND_INC) -o $@ eglgles.c -ldl
eglgles-android: eglgles.c $(GLVND_DIR)/.pinned
	$(NDK_CC) -O1 -g -Wall -Wextra -fPIE -pie $(GLVND_INC) -o $@ eglgles.c -ldl
eglgles-android32: eglgles.c $(GLVND_DIR)/.pinned
	$(NDK_CC32) -O1 -g -Wall -Wextra -fPIE -pie $(GLVND_INC) -o $@ eglgles.c -ldl

# --- appsandbox: run a command as an app uid under bionic's APP seccomp filter (P2 gate) ---
# The filter is generated from bionic's own policy sources at a pinned android13-release
# commit with bionic's own generator; nothing here is hand-written policy. Kernel syscall
# numbers come from the NDK sysroot's asm/unistd headers, preprocessed per arch exactly the
# way bionic's build does (libc/seccomp/gen_syscall_nrs*.cpp).
BIONIC_URL    ?= https://android.googlesource.com/platform/bionic
BIONIC_COMMIT ?= 0b981d1ec055efaef86d8a5d402cf1b3167ba10b
BIONIC_DIR    ?= gen/bionic
NDK_CLANG ?= $(NDK)/toolchains/llvm/prebuilt/linux-x86_64/bin/clang
NDK_CXX   ?= $(NDK)/toolchains/llvm/prebuilt/linux-x86_64/bin/x86_64-linux-android33-clang++
SECCOMP_ARCHES := arm arm64 x86 x86_64
SECCOMP_TARGET_arm    := armv7a-linux-androideabi33
SECCOMP_TARGET_arm64  := aarch64-linux-android33
SECCOMP_TARGET_x86    := i686-linux-android33
SECCOMP_TARGET_x86_64 := x86_64-linux-android33
SECCOMP_NRS_SRC_arm    := gen_syscall_nrs.cpp
SECCOMP_NRS_SRC_arm64  := gen_syscall_nrs.cpp
SECCOMP_NRS_SRC_x86    := gen_syscall_nrs_x86.cpp
SECCOMP_NRS_SRC_x86_64 := gen_syscall_nrs_x86_64.cpp
SECCOMP_LISTS := SYSCALLS.TXT SECCOMP_ALLOWLIST_COMMON.TXT SECCOMP_ALLOWLIST_APP.TXT \
                 SECCOMP_BLOCKLIST_COMMON.TXT SECCOMP_BLOCKLIST_APP.TXT SECCOMP_PRIORITY.TXT

$(BIONIC_DIR)/.pinned:
	rm -rf $(BIONIC_DIR); mkdir -p $(dir $(BIONIC_DIR))
	git clone -q --filter=blob:none --no-checkout $(BIONIC_URL) $(BIONIC_DIR)
	cd $(BIONIC_DIR) && git sparse-checkout init --no-cone && \
	    git sparse-checkout set /libc/SYSCALLS.TXT /libc/SECCOMP_*.TXT /libc/seccomp/ /libc/tools/genseccomp.py /libc/tools/gensyscalls.py && \
	    git checkout -q $(BIONIC_COMMIT)
	@test "$$(git -C $(BIONIC_DIR) rev-parse HEAD)" = "$(BIONIC_COMMIT)" || { echo "bionic checkout is not at $(BIONIC_COMMIT)" >&2; exit 1; }
	touch $@

define SECCOMP_NRS_RULE
gen/libseccomp_gen_syscall_nrs_$(1): $(BIONIC_DIR)/.pinned
	$(NDK_CLANG) --target=$(SECCOMP_TARGET_$(1)) -E -dD -o $$@ $(BIONIC_DIR)/libc/seccomp/$(SECCOMP_NRS_SRC_$(1))
endef
$(foreach a,$(SECCOMP_ARCHES),$(eval $(call SECCOMP_NRS_RULE,$(a))))

gen/x86_64_app_policy.cpp gen/x86_app_policy.cpp: $(addprefix gen/libseccomp_gen_syscall_nrs_,$(SECCOMP_ARCHES))
	PYTHONPATH=$(BIONIC_DIR)/libc/tools python3 -s -P $(BIONIC_DIR)/libc/tools/genseccomp.py \
	    --name-modifier app --out-dir gen $(addprefix $(BIONIC_DIR)/libc/,$(SECCOMP_LISTS)) $^
	@grep -q "x86_64_app_filter\[\]" gen/x86_64_app_policy.cpp && grep -q "x86_app_filter\[\]" gen/x86_app_policy.cpp

# The generated files include "seccomp/seccomp_bpfs.h": mirror bionic's include layout.
gen/seccomp/seccomp_bpfs.h: $(BIONIC_DIR)/.pinned
	mkdir -p gen/seccomp && cp $(BIONIC_DIR)/libc/seccomp/seccomp_bpfs.h $@

appsandbox: appsandbox.cpp gen/x86_64_app_policy.cpp gen/x86_app_policy.cpp gen/seccomp/seccomp_bpfs.h
	$(NDK_CXX) -std=c++17 -O1 -g -Wall -Werror -fPIE -pie -static-libstdc++ -Igen -Igen/seccomp \
	    -o $@ appsandbox.cpp gen/x86_64_app_policy.cpp gen/x86_app_policy.cpp
	@$(NDK)/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-readelf -d $@ | grep -q "NEEDED.*libc++_shared" && { echo "appsandbox must not need libc++_shared" >&2; exit 1; } || true

# --- glesapp: a NativeActivity GLES2 app (glesapp/main.c), built for one ABI per APK ---
#   make glesapp-x86.apk     the 32-bit x86 build: a 32-bit GLES process (ANGLE x86 -> 32-bit HAL -> i386 ICD)
#   make glesapp-x86_64.apk  the same app as a 64-bit process, for A/B
#   make glesapp-armeabi-v7a.apk  an ARM32 build (x86 image: runs through the ARM translator)
# No Java: the manifest has hasCode=false and names android.app.NativeActivity. Signed with a throwaway
# debug key made here (glesapp/out/debug.jks); it is a test app, never shipped.
SDK        ?= $(HOME)/Android/Sdk
BUILDTOOLS ?= $(SDK)/build-tools/36.0.0
ANDROID_JAR ?= $(SDK)/platforms/android-33/android.jar
GLESAPP_TRIPLE_x86    := i686-linux-android33
GLESAPP_TRIPLE_x86_64 := x86_64-linux-android33
# ARM32 build: on an x86 image it runs through the ARM translator (houdini's 32-bit path), whose
# guest calls into the x86 system EGL/GLES libraries reach the same 32-bit x86 driver stack
GLESAPP_TRIPLE_armeabi-v7a := armv7a-linux-androideabi33
glesapp/out/%/lib/libglesapp.so: glesapp/main.c
	@mkdir -p $(dir $@)
	$(NDK)/toolchains/llvm/prebuilt/linux-x86_64/bin/$(GLESAPP_TRIPLE_$*)-clang -O2 -g -Wall -Wextra -Werror -fPIC -shared \
	    -fvisibility=hidden -Wl,-z,defs -o $@ glesapp/main.c -landroid -lEGL -lGLESv2 -llog -lm
glesapp/out/debug.jks:
	@mkdir -p $(dir $@)
	keytool -genkeypair -keystore $@ -storepass android -keypass android -alias glesapp -keyalg RSA -keysize 2048 \
	    -validity 3650 -dname "CN=glesapp test key" >/dev/null
glesapp-%.apk: glesapp/out/%/lib/libglesapp.so glesapp/AndroidManifest.xml glesapp/out/debug.jks
	rm -rf glesapp/out/$*/apk && mkdir -p glesapp/out/$*/apk/lib/$*
	$(BUILDTOOLS)/aapt2 link -o glesapp/out/$*/unsigned.apk --manifest glesapp/AndroidManifest.xml -I $(ANDROID_JAR)
	cp glesapp/out/$*/lib/libglesapp.so glesapp/out/$*/apk/lib/$*/
	cd glesapp/out/$*/apk && zip -q -r ../unsigned.apk lib
	$(BUILDTOOLS)/zipalign -f -p 4 glesapp/out/$*/unsigned.apk glesapp/out/$*/aligned.apk
	$(BUILDTOOLS)/apksigner sign --ks glesapp/out/debug.jks --ks-pass pass:android --key-pass pass:android \
	    --out $@ glesapp/out/$*/aligned.apk
	@echo "$@: $$(unzip -l $@ | grep -o 'lib/[^ ]*')"

# --- hwuiapp: a Java activity drawn by HWUI (skiavk), forced to one ABI by an empty native library ---
#   make hwuiapp-x86.apk   a 32-bit app process whose UI HWUI renders through the 32-bit Vulkan HAL
hwuiapp/out/classes.dex: hwuiapp/src/dev/nvprolapse/hwuiapp/MainActivity.java
	rm -rf hwuiapp/out/classes && mkdir -p hwuiapp/out/classes
	javac --release 8 -classpath $(ANDROID_JAR) -d hwuiapp/out/classes $< 2>&1 | grep -v "^warning: \[options\]" || true
	test -f hwuiapp/out/classes/dev/nvprolapse/hwuiapp/MainActivity.class
	$(BUILDTOOLS)/d8 --min-api 26 --lib $(ANDROID_JAR) --output hwuiapp/out $$(find hwuiapp/out/classes -name '*.class')
hwuiapp/out/%/libhwuiapp.so: hwuiapp/abi.c
	@mkdir -p $(dir $@)
	$(NDK)/toolchains/llvm/prebuilt/linux-x86_64/bin/$(GLESAPP_TRIPLE_$*)-clang -O2 -fPIC -shared -o $@ $<
hwuiapp-%.apk: hwuiapp/out/classes.dex hwuiapp/out/%/libhwuiapp.so hwuiapp/AndroidManifest.xml glesapp/out/debug.jks
	rm -rf hwuiapp/out/$*/apk && mkdir -p hwuiapp/out/$*/apk/lib/$*
	$(BUILDTOOLS)/aapt2 link -o hwuiapp/out/$*/unsigned.apk --manifest hwuiapp/AndroidManifest.xml -I $(ANDROID_JAR)
	cp hwuiapp/out/$*/libhwuiapp.so hwuiapp/out/$*/apk/lib/$*/ && cp hwuiapp/out/classes.dex hwuiapp/out/$*/apk/
	cd hwuiapp/out/$*/apk && zip -q -r ../unsigned.apk lib classes.dex
	$(BUILDTOOLS)/zipalign -f -p 4 hwuiapp/out/$*/unsigned.apk hwuiapp/out/$*/aligned.apk
	$(BUILDTOOLS)/apksigner sign --ks glesapp/out/debug.jks --ks-pass pass:android --key-pass pass:android \
	    --out $@ hwuiapp/out/$*/aligned.apk
	@echo "$@: $$(unzip -l $@ | grep -oE '(lib/[^ ]*|classes.dex)' | tr '\n' ' ')"
