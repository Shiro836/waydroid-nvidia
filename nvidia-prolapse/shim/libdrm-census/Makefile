# glibc-shim mode: NVIDIA's own libraries load the driver's bionic libdrm.so.2 (built by the driver
# repository, tools/driver_source.py: <driver>/libdrm/out/libdrm.so.2, out32/ for i686). This checks that
# library for them: SONAME, bionic-only NEEDED, and every drm* name the blobs reference (the names of
# every supported driver branch, imports/<version>-<arch>.names, plus the loaded driver's own libraries
# found through the loader cache: ../tools/find-driver.py).
#   make [ARCH=x86]               check the x86_64 (or i686) library
#   NO_BLOB_CENSUS=REASON         skip the census of the host's NVIDIA libraries (a builder without the
#                                 NVIDIA driver); the reference lists are still checked
#   DRIVER=<checkout>             the driver checkout (default: tools/driver_source.py path)

NDK      ?= /opt/android-ndk
TC       := $(NDK)/toolchains/llvm/prebuilt/linux-x86_64
READELF  := $(TC)/bin/llvm-readelf
PYTHON   := python3 -I
ARCH     ?= x86_64

HERE     := $(patsubst %/,%,$(dir $(abspath $(lastword $(MAKEFILE_LIST)))))
TOP      := $(abspath $(HERE)/../../..)
DRIVER   ?= $(shell python3 $(TOP)/tools/driver_source.py path)
# the libdrm release the driver builds (its libdrm/Makefile pin)
VERSION  := $(shell sed -n 's/^VERSION *:= *//p' $(DRIVER)/libdrm/Makefile)
ifeq ($(ARCH),x86_64)
LIB      := $(DRIVER)/libdrm/out/libdrm.so.2
HOSTDRM  ?= /usr/lib/libdrm.so.2
else ifeq ($(ARCH),x86)
LIB      := $(DRIVER)/libdrm/out32/libdrm.so.2
HOSTDRM  ?= /usr/lib32/libdrm.so.2
else
$(error ARCH must be x86_64 or x86, not '$(ARCH)')
endif
ifeq ($(VERSION),)
$(error no libdrm VERSION in $(DRIVER)/libdrm/Makefile)
endif

REFERENCES := $(sort $(wildcard $(HERE)/imports/*-$(ARCH).names))
ifeq ($(REFERENCES),)
$(error no driver reference lists $(HERE)/imports/*-$(ARCH).names)
endif

.PHONY: check
check:
	@[ -f "$(LIB)" ] || { echo "libdrm-census: $(LIB) missing (build the driver: make -C $(DRIVER)/libdrm$(if $(filter x86,$(ARCH)), ARCH=x86))" >&2; exit 1; }
	@set -e; blobs=""; \
	if [ -z "$(NO_BLOB_CENSUS)" ]; then \
	  blobs=$$($(PYTHON) $(TOP)/nvidia-prolapse/shim/tools/find-driver.py --arch $(ARCH)) || { \
	    echo "libdrm-census: no usable host NVIDIA driver for $(ARCH) (above); a builder without it: make NO_BLOB_CENSUS=\"<reason>\"" >&2; exit 1; }; fi; \
	$(PYTHON) $(HERE)/check-libdrm.py --readelf $(READELF) --allow-needed libc.so,libm.so,libdl.so \
	    --arch $(ARCH) --host-libdrm $(HOSTDRM) --pinned-version $(VERSION) \
	    $(addprefix --reference ,$(REFERENCES)) $$(for b in $$blobs; do printf -- '--blob %s ' "$$b"; done) \
	    $(if $(NO_BLOB_CENSUS),--no-blob-census "$(NO_BLOB_CENSUS)") $(LIB)
