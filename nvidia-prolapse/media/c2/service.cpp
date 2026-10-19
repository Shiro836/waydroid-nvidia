/* SPDX-License-Identifier: Apache-2.0
 * android.hardware.media.c2@1.2-service-nvprolapse — vendor Codec2 HAL service exposing
 * c2.nvprolapse.<codec>.decoder for every codec nv_video_query_caps() reports.
 * Follows frameworks/av media/codec2/hidl/services/vendor.cpp (android-13).
 *
 * Start-up checks, all fatal with a named reason (init restarts us, logcat says why):
 *  - the NVIDIA device opens and the backend ABI matches,
 *  - the media_codecs_c2.xml the framework will read lists exactly the live caps
 *    (a stale file would advertise codecs/sizes this GPU does not decode).
 * A GPU without an NVDEC class this decoder drives (before Turing) is not a failure: the setup
 * then binds a listing without codecs (nvc2-xmlgen exit 3), runs this service once (oneshot) and
 * declares no Codec2 HAL; the service checks that listing, logs the reason once and exits 0.
 */
#define LOG_TAG "nvc2-service"
#include <C2Component.h>
#include <binder/ProcessState.h>
#include <codec2/hidl/1.2/ComponentStore.h>
#include <hidl/HidlTransportSupport.h>
#include <hwminijail/HardwareMinijail.h>
#include <log/log.h>

#include <csignal>
#include <cstdio>
#include <string>

#include "NvC2Component.h"

namespace {
constexpr char kSeccompPolicy[] = "/odm/etc/seccomp_policy/android.hardware.media.c2@1.2-nvprolapse.policy";
constexpr char kCodecsXml[] = "/odm/etc/media_codecs_c2.xml";
} // namespace

int main()
{
    using namespace ::android;
    signal(SIGPIPE, SIG_IGN);
    hardware::SetupMinijail(kSeccompPolicy);

    /* stdio, not <fstream>: the image's libc++ lacks the r29 headers' basic_filebuf symbols */
    std::string have;
    bool readable = false;
    if (FILE *f = fopen(kCodecsXml, "re")) {
        char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, f)) > 0)
            have.append(buf, n);
        readable = !ferror(f);
        fclose(f);
    }
    std::string err, no_nvdec_gpu;
    std::shared_ptr<nvc2::Store> impl = nvc2::Store::Create(&err, &no_nvdec_gpu);
    if (!impl && !no_nvdec_gpu.empty()) {
        const std::string want = nvc2::no_decoder_xml(no_nvdec_gpu.c_str());
        if (!readable || have != want) {
            ALOGE("%s does not match this GPU (%s); re-run sudo waydroid-nvidia-setup on the host (expected:\n%s)",
                  kCodecsXml, nvc2::no_decoder_reason(no_nvdec_gpu.c_str()).c_str(), want.c_str());
            return 1;
        }
        ALOGI("%s", nvc2::no_decoder_reason(no_nvdec_gpu.c_str()).c_str());
        return 0;
    }
    if (!impl) {
        ALOGE("no NVDEC store: %s", err.c_str());
        return 1;
    }
    if (!readable || have != nvc2::media_codecs_xml(impl->entries())) {
        ALOGE("%s does not match this GPU's decode caps; regenerate it on the host with nvc2-xmlgen "
              "(expected:\n%s)",
              kCodecsXml, nvc2::media_codecs_xml(impl->entries()).c_str());
        return 1;
    }

    ProcessState::initWithDriver("/dev/vndbinder");
    ProcessState::self()->startThreadPool();
    hardware::configureRpcThreadpool(8, true /* callerWillJoin */);

    using namespace ::android::hardware::media::c2::V1_2;
    sp<IComponentStore> store = new utils::ComponentStore(impl);
    if (store->registerAsService("default") != OK) {
        ALOGE("cannot register IComponentStore/default (VINTF manifest fragment missing?)");
        return 1;
    }
    ALOGI("IComponentStore/default registered, %zu decoder(s)", impl->entries().size());
    hardware::joinRpcThreadpool();
    return 0;
}
