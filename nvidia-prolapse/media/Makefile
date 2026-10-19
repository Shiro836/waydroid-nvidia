# nvidia-prolapse/media — NVDEC hardware decoding for Android (Codec2 vendor HAL)
#
#   make test     -> host unit tests of the decoder core against the scripted nv_video stub
#   make xmlgen   -> out/host/nvc2-xmlgen (links the GNU build of the nv_video backend: NV_VIDEO_LIB)
#   make xmlgen-android -> out/android/nvc2-xmlgen (bionic, for the release gate)
#   make stubgen  -> out/host/nvc2-xmlgen-stub (same tool on the stub; for the xml unit test)
#   make realtest -> core on the real GPU (see the rule)
#   make android  -> out/android/libnvc2_core.a (NDK r29, x86_64, API 33, platform libc++ ABI std::__1)
#   make android-c2 -> out/android/c2obj/*.o (Codec2 component, compile check)
#
# nv_video.h is the backend's fixed ABI (driver-source/native/video/nv_video.h).

NDK      ?= ../../build/hwcomposer-work/ndk/android-ndk-r29
NDK_BIN  := $(NDK)/toolchains/llvm/prebuilt/linux-x86_64/bin
NDK_CXX  ?= $(NDK_BIN)/x86_64-linux-android33-clang++
NDK_AR   ?= $(NDK_BIN)/llvm-ar
HOST_CXX ?= g++
NV_VIDEO_INC ?= ../driver-source/native/video
NV_VIDEO_LIB ?=

WARN   := -Wall -Wextra -Werror
CXXFLAGS_COMMON := -std=c++17 -O2 -g $(WARN) -Icore -I$(NV_VIDEO_INC)

CORE := core/nvc2_decoder.cpp core/nvc2_caps.cpp
HDRS := core/nvc2_decoder.h core/nvc2_caps.h $(NV_VIDEO_INC)/nv_video.h

all: test

out/host/core_test: $(CORE) $(HDRS) test/core_test.cpp test/nv_video_stub.cpp test/nv_video_stub.h Makefile
	@mkdir -p $(@D)
	$(HOST_CXX) $(CXXFLAGS_COMMON) -fsanitize=address,undefined -Itest -o $@ $(CORE) test/core_test.cpp test/nv_video_stub.cpp

out/host/xml_test: $(CORE) $(HDRS) tools/nvc2_xmlgen.cpp test/xml_test.cpp test/nv_video_stub.cpp Makefile
	@mkdir -p $(@D)
	$(HOST_CXX) $(CXXFLAGS_COMMON) -fsanitize=address,undefined -Itest -DNVC2_XMLGEN_NO_MAIN -o $@ $(CORE) tools/nvc2_xmlgen.cpp test/xml_test.cpp test/nv_video_stub.cpp

test: out/host/core_test out/host/xml_test
	./out/host/core_test
	./out/host/xml_test
	python3 -I test/validate_xml.py out/host/media_codecs_c2.xml
	python3 -I test/validate_xml.py --no-decoders out/host/media_codecs_c2.none.xml

xmlgen: out/host/nvc2-xmlgen
out/host/nvc2-xmlgen: $(CORE) $(HDRS) tools/nvc2_xmlgen.cpp Makefile
	@test -n "$(NV_VIDEO_LIB)" || { echo "NV_VIDEO_LIB= the GNU build of the nv_video backend (libnv_bionic_native GNU)"; exit 1; }
	@mkdir -p $(@D)
	$(HOST_CXX) $(CXXFLAGS_COMMON) -o $@ $(CORE) tools/nvc2_xmlgen.cpp $(NV_VIDEO_LIB)

# Bionic build of the same tool for the release's install-time gate (packaging/native/build-gate.py):
# run through the image's linker64 against the shipped libnv_bionic_native.so, no compiler on the user's host.
#   make xmlgen-android NV_VIDEO_ANDROID_LIB="-L<dir of the x86_64-linux-android release lib> -lnv_bionic_native"
xmlgen-android: out/android/nvc2-xmlgen
out/android/nvc2-xmlgen: $(CORE) $(HDRS) tools/nvc2_xmlgen.cpp Makefile
	@test -n "$(NV_VIDEO_ANDROID_LIB)" || { echo "NV_VIDEO_ANDROID_LIB= the x86_64 Android build of the nv_video backend"; exit 1; }
	@mkdir -p $(@D)
	$(NDK_CXX) $(CXXFLAGS_COMMON) -static-libstdc++ -o $@ $(CORE) tools/nvc2_xmlgen.cpp $(NV_VIDEO_ANDROID_LIB)

# Real GPU: the core on the GNU nv_video backend, VP9 IVF vs an ffmpeg NV12 reference
#   make realtest NV_VIDEO_LIB="-L<dir> -lnv_bionic_native -Wl,-rpath,<dir>" IVF=in.ivf REF=ref.nv12 W=854 H=480
realtest: out/host/real_vp9_test
	./out/host/real_vp9_test $(IVF) $(REF) $(W) $(H) $(POOL)
out/host/real_vp9_test: core/nvc2_decoder.cpp $(HDRS) test/real_vp9_test.cpp Makefile
	@test -n "$(NV_VIDEO_LIB)" || { echo "NV_VIDEO_LIB= the GNU build of the nv_video backend"; exit 1; }
	@mkdir -p $(@D)
	$(HOST_CXX) $(CXXFLAGS_COMMON) -o $@ core/nvc2_decoder.cpp test/real_vp9_test.cpp $(NV_VIDEO_LIB)

android: out/android/libnvc2_core.a
out/android/libnvc2_core.a: $(CORE) $(HDRS) Makefile
	@mkdir -p $(@D)/obj
	$(NDK_CXX) $(CXXFLAGS_COMMON) -isystem $(R)/hwcomposer/libcxx-override -fPIC -c core/nvc2_decoder.cpp -o $(@D)/obj/nvc2_decoder.o
	$(NDK_CXX) $(CXXFLAGS_COMMON) -isystem $(R)/hwcomposer/libcxx-override -fPIC -c core/nvc2_caps.cpp -o $(@D)/obj/nvc2_caps.o
	$(NDK_AR) rcs $@ $(@D)/obj/nvc2_decoder.o $(@D)/obj/nvc2_caps.o

# Codec2 front, compile check (link needs the vendor Codec2 libraries built by vndk/build.sh).
# Platform libc++ ABI (std::__1) via hwcomposer/libcxx-override, as the hwcomposer build.
# -Wno-invalid-specialization / -fno-exceptions: AOSP's C2Enum.h/C2ParamDef.h vs clang 21
# (Soong builds with -fno-exceptions; its older clang accepted the std::underlying_type specialisation).
R      := ../..
HWDEPS := $(R)/build/hwcomposer-work/deps
AV     ?= $(R)/build/media-c2/av
C2_INC := -isystem $(R)/hwcomposer/libcxx-override -Icore -I$(NV_VIDEO_INC) \
          -isystem $(AV)/media/codec2/core/include -isystem $(AV)/media/codec2/vndk/include \
          -isystem $(AV)/media/codec2/components/base/include -isystem $(AV)/media/libstagefright/foundation/include \
          -isystem $(R)/build/minigbm/cros_gralloc -isystem $(R)/build/minigbm \
          -isystem $(HWDEPS)/aosp_system_core/libcutils/include -isystem $(HWDEPS)/aosp_system_core/libutils/include \
          -isystem $(HWDEPS)/aosp_system_core/libsystem/include -isystem $(HWDEPS)/android_system_logging/liblog/include \
          -isystem $(HWDEPS)/aosp_frameworks_native/libs/nativewindow/include \
          -isystem $(HWDEPS)/aosp_frameworks_native/libs/nativebase/include \
          -isystem $(HWDEPS)/aosp_frameworks_native/libs/arect/include -isystem $(HWDEPS)/aosp_libbase/include \
          -isystem $(HWDEPS)/aosp_libhardware/include
C2_FLAGS := -std=c++17 -O2 -g -fPIC -fno-exceptions -D__ANDROID_VNDK__ -DANDROID -Wall -Wextra -Werror \
            -Wno-unused-parameter -Wno-invalid-specialization $(C2_INC)
android-c2: out/android/c2obj/NvC2Component.o out/android/c2obj/SimpleC2Interface.o
out/android/c2obj/NvC2Component.o: c2/NvC2Component.cpp c2/NvC2Component.h $(HDRS) Makefile
	@mkdir -p $(@D)
	$(NDK_CXX) $(C2_FLAGS) -c $< -o $@
out/android/c2obj/SimpleC2Interface.o: $(AV)/media/codec2/components/base/SimpleC2Interface.cpp Makefile
	@mkdir -p $(@D)
	$(NDK_CXX) $(C2_FLAGS) -Wno-error -c $< -o $@

clean:
	rm -rf out

.PHONY: all test xmlgen xmlgen-android realtest android android-c2 clean
