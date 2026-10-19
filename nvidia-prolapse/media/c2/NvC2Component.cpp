/* SPDX-License-Identifier: Apache-2.0
 * Codec2 store + decoder components over the nvc2 core (see NvC2Component.h).
 *
 * Output path: every decoded picture is copied by the GPU copy engine
 * (nv_video_copy_out) into a gralloc block fetched from the framework's output pool
 * (BufferQueue when decoding to a Surface): HAL_PIXEL_FORMAT_YCBCR_420_888 -> NV12 or
 * HAL_PIXEL_FORMAT_YCBCR_P010 -> P010, LINEAR (minigbm gbm_mesa R8 carrier layout,
 * nvidia-prolapse/gralloc/minigbm/0004). Exception: an output pool on the plain gralloc
 * allocator (no Surface: the client maps every block on the CPU, ByteBuffer/Image mode) gets
 * 8-bit pictures as HAL_PIXEL_FORMAT_YV12, written planar (see Component::Pool::fetch).
 * The copy's sync_file is waited for here before
 * the work is returned. Not verified: whether the image's (LineageOS 20) HIDL Codec2
 * client/C2Fence code carries a sync_file fence across the HAL; until that is checked the
 * copy fence stays inside the component (copy_out of a 4K frame measured ~0.5 ms).
 */
#define LOG_TAG "nvc2"
#include "NvC2Component.h"

#include <C2AllocatorGralloc.h>
#include <C2PlatformSupport.h>
#include <SimpleC2Interface.h>
#include <cros_gralloc_handle.h>
#include <log/log.h>
#include <media/stagefright/foundation/MediaDefs.h>
#include <system/graphics.h>

#include <cerrno>
#include <cstring>
#include <poll.h>
#include <sys/system_properties.h>
#include <unistd.h>

namespace nvc2 {

using namespace ::android;

namespace {

constexpr size_t kMinInputBufferSize = 2 * 1024 * 1024;
constexpr uint64_t kGrallocUsageVideoDecoder = 1ull << 22; /* BufferUsage::VIDEO_DECODER */
constexpr int kCopyFenceTimeoutMs = 2000;
/* minigbm cros_gralloc/cros_gralloc_helpers.h:35 (pinned 16844ba) */
constexpr uint32_t kCrosGrallocMagic = 0xABCDDCBA;

constexpr uint32_t align_up(uint32_t v, uint32_t a) { return (v + a - 1) / a * a; }

/* Size-implied level ceilings (MaxPicSize/MaxLumaPs/MaxFS of each spec); nv_video caps
 * carry sizes and bit depths, and max_level only where the backend knows it. */
bool fits(const NvVideoCaps &c, uint64_t max_samples) { return (uint64_t)c.max_width * c.max_height >= max_samples; }

std::vector<uint32_t> vp9_levels(const NvVideoCaps &c)
{
    std::vector<uint32_t> l = {C2Config::LEVEL_VP9_1, C2Config::LEVEL_VP9_1_1, C2Config::LEVEL_VP9_2,
                               C2Config::LEVEL_VP9_2_1, C2Config::LEVEL_VP9_3, C2Config::LEVEL_VP9_3_1,
                               C2Config::LEVEL_VP9_4, C2Config::LEVEL_VP9_4_1};
    if (fits(c, 8912896))
        l.insert(l.end(), {C2Config::LEVEL_VP9_5, C2Config::LEVEL_VP9_5_1, C2Config::LEVEL_VP9_5_2});
    if (fits(c, 35651584))
        l.insert(l.end(), {C2Config::LEVEL_VP9_6, C2Config::LEVEL_VP9_6_1, C2Config::LEVEL_VP9_6_2});
    return l;
}

std::vector<uint32_t> av1_levels(const NvVideoCaps &c)
{
    std::vector<uint32_t> l = {C2Config::LEVEL_AV1_2, C2Config::LEVEL_AV1_2_1, C2Config::LEVEL_AV1_2_2,
                               C2Config::LEVEL_AV1_2_3, C2Config::LEVEL_AV1_3, C2Config::LEVEL_AV1_3_1,
                               C2Config::LEVEL_AV1_3_2, C2Config::LEVEL_AV1_3_3, C2Config::LEVEL_AV1_4,
                               C2Config::LEVEL_AV1_4_1, C2Config::LEVEL_AV1_4_2, C2Config::LEVEL_AV1_4_3};
    if (fits(c, 8912896))
        l.insert(l.end(), {C2Config::LEVEL_AV1_5, C2Config::LEVEL_AV1_5_1, C2Config::LEVEL_AV1_5_2,
                           C2Config::LEVEL_AV1_5_3});
    if (fits(c, 35651584))
        l.insert(l.end(), {C2Config::LEVEL_AV1_6, C2Config::LEVEL_AV1_6_1, C2Config::LEVEL_AV1_6_2,
                           C2Config::LEVEL_AV1_6_3});
    return l;
}

std::vector<uint32_t> hevc_levels(const NvVideoCaps &c)
{
    std::vector<uint32_t> l = {C2Config::LEVEL_HEVC_MAIN_1, C2Config::LEVEL_HEVC_MAIN_2,
                               C2Config::LEVEL_HEVC_MAIN_2_1, C2Config::LEVEL_HEVC_MAIN_3,
                               C2Config::LEVEL_HEVC_MAIN_3_1, C2Config::LEVEL_HEVC_MAIN_4,
                               C2Config::LEVEL_HEVC_MAIN_4_1, C2Config::LEVEL_HEVC_HIGH_4,
                               C2Config::LEVEL_HEVC_HIGH_4_1};
    if (fits(c, 8912896))
        l.insert(l.end(), {C2Config::LEVEL_HEVC_MAIN_5, C2Config::LEVEL_HEVC_MAIN_5_1, C2Config::LEVEL_HEVC_MAIN_5_2,
                           C2Config::LEVEL_HEVC_HIGH_5, C2Config::LEVEL_HEVC_HIGH_5_1,
                           C2Config::LEVEL_HEVC_HIGH_5_2});
    if (fits(c, 35651584))
        l.insert(l.end(), {C2Config::LEVEL_HEVC_MAIN_6, C2Config::LEVEL_HEVC_MAIN_6_1, C2Config::LEVEL_HEVC_MAIN_6_2,
                           C2Config::LEVEL_HEVC_HIGH_6, C2Config::LEVEL_HEVC_HIGH_6_1,
                           C2Config::LEVEL_HEVC_HIGH_6_2});
    return l;
}

std::vector<uint32_t> avc_levels(const NvVideoCaps &c)
{
    std::vector<uint32_t> l = {C2Config::LEVEL_AVC_1, C2Config::LEVEL_AVC_1B, C2Config::LEVEL_AVC_1_1,
                               C2Config::LEVEL_AVC_1_2, C2Config::LEVEL_AVC_1_3, C2Config::LEVEL_AVC_2,
                               C2Config::LEVEL_AVC_2_1, C2Config::LEVEL_AVC_2_2, C2Config::LEVEL_AVC_3,
                               C2Config::LEVEL_AVC_3_1, C2Config::LEVEL_AVC_3_2, C2Config::LEVEL_AVC_4,
                               C2Config::LEVEL_AVC_4_1, C2Config::LEVEL_AVC_4_2};
    /* MaxFS: 5.x 36864 MBs (4096x2304), 6.x 139264 MBs */
    if (fits(c, 36864ull * 256))
        l.insert(l.end(), {C2Config::LEVEL_AVC_5, C2Config::LEVEL_AVC_5_1, C2Config::LEVEL_AVC_5_2});
    if (fits(c, 139264ull * 256))
        l.insert(l.end(), {C2Config::LEVEL_AVC_6, C2Config::LEVEL_AVC_6_1, C2Config::LEVEL_AVC_6_2});
    return l;
}

bool ten_bit(const NvVideoCaps &c) { return c.bit_depth_mask & (1u << 10); }

/* ISO/IEC 23091-2 code points -> C2Color (the subset the decoders emit) */
C2Color::primaries_t map_primaries(uint32_t v)
{
    switch (v) {
    case 1: return C2Color::PRIMARIES_BT709;
    case 4: return C2Color::PRIMARIES_BT470_M;
    case 5: return C2Color::PRIMARIES_BT601_625;
    case 6: case 7: return C2Color::PRIMARIES_BT601_525;
    case 9: return C2Color::PRIMARIES_BT2020;
    default: return C2Color::PRIMARIES_UNSPECIFIED;
    }
}
C2Color::transfer_t map_transfer(uint32_t v)
{
    switch (v) {
    case 1: case 6: case 14: case 15: return C2Color::TRANSFER_170M;
    case 8: return C2Color::TRANSFER_LINEAR;
    case 13: return C2Color::TRANSFER_SRGB;
    case 16: return C2Color::TRANSFER_ST2084;
    case 18: return C2Color::TRANSFER_HLG;
    default: return C2Color::TRANSFER_UNSPECIFIED;
    }
}
C2Color::matrix_t map_matrix(uint32_t v)
{
    switch (v) {
    case 1: return C2Color::MATRIX_BT709;
    case 5: case 6: return C2Color::MATRIX_BT601;
    case 9: return C2Color::MATRIX_BT2020;
    case 10: return C2Color::MATRIX_BT2020_CONSTANT;
    default: return C2Color::MATRIX_UNSPECIFIED;
    }
}

} // namespace

/* ------------------------------------------------------------------ interface */

class IntfImpl : public SimpleInterface<void>::BaseParams {
public:
    IntfImpl(const std::shared_ptr<C2ReflectorHelper> &helper, const CodecEntry &e)
        : SimpleInterface<void>::BaseParams(helper, e.name, C2Component::KIND_DECODER, C2Component::DOMAIN_VIDEO,
                                            e.mime)
    {
        noPrivateBuffers();
        noInputReferences();
        noOutputReferences();
        noInputLatency();
        noTimeStretch();

        const bool reorders = e.codec == NV_VIDEO_CODEC_H264 || e.codec == NV_VIDEO_CODEC_HEVC;
        /* works the component may hold before returning them: the reorder window for
         * H.264/HEVC (DPB, max 16), one hidden frame for VP9/AV1 */
        addParameter(DefineParam(mActualOutputDelay, C2_PARAMKEY_OUTPUT_DELAY)
                         .withDefault(new C2PortActualDelayTuning::output(reorders ? 16u : 2u))
                         .withFields({C2F(mActualOutputDelay, value).inRange(0, 16)})
                         .withSetter(Setter<decltype(*mActualOutputDelay)>::StrictValueWithNoDeps)
                         .build());

        addParameter(DefineParam(mAttrib, C2_PARAMKEY_COMPONENT_ATTRIBUTES)
                         .withConstValue(new C2ComponentAttributesSetting(C2Component::ATTRIB_IS_TEMPORAL))
                         .build());

        const uint32_t minw = (e.caps.min_width + 1) & ~1u, minh = (e.caps.min_height + 1) & ~1u;
        const uint32_t maxw = e.caps.max_width & ~1u, maxh = e.caps.max_height & ~1u;
        addParameter(DefineParam(mSize, C2_PARAMKEY_PICTURE_SIZE)
                         .withDefault(new C2StreamPictureSizeInfo::output(0u, 320, 240))
                         .withFields({C2F(mSize, width).inRange(minw, maxw, 2),
                                      C2F(mSize, height).inRange(minh, maxh, 2)})
                         .withSetter(SizeSetter)
                         .build());
        addParameter(DefineParam(mMaxSize, C2_PARAMKEY_MAX_PICTURE_SIZE)
                         .withDefault(new C2StreamMaxPictureSizeTuning::output(0u, 320, 240))
                         .withFields({C2F(mMaxSize, width).inRange(minw, maxw, 2),
                                      C2F(mMaxSize, height).inRange(minh, maxh, 2)})
                         .withSetter(MaxPictureSizeSetter, mSize)
                         .build());
        addParameter(DefineParam(mMaxInputSize, C2_PARAMKEY_INPUT_MAX_BUFFER_SIZE)
                         .withDefault(new C2StreamMaxBufferSizeInfo::input(0u, kMinInputBufferSize))
                         .withFields({C2F(mMaxInputSize, value).any()})
                         .calculatedAs(MaxInputSizeSetter, mMaxSize)
                         .build());

        std::vector<uint32_t> profiles, levels;
        uint32_t defProfile = 0, defLevel = 0;
        switch (e.codec) {
        case NV_VIDEO_CODEC_VP9:
            profiles = {C2Config::PROFILE_VP9_0};
            if (ten_bit(e.caps))
                profiles.push_back(C2Config::PROFILE_VP9_2);
            levels = vp9_levels(e.caps);
            defProfile = C2Config::PROFILE_VP9_0;
            break;
        case NV_VIDEO_CODEC_AV1:
            profiles = {C2Config::PROFILE_AV1_0}; /* Main: 8- and 10-bit 4:2:0 */
            levels = av1_levels(e.caps);
            defProfile = C2Config::PROFILE_AV1_0;
            break;
        case NV_VIDEO_CODEC_HEVC:
            profiles = {C2Config::PROFILE_HEVC_MAIN, C2Config::PROFILE_HEVC_MAIN_STILL};
            if (ten_bit(e.caps))
                profiles.push_back(C2Config::PROFILE_HEVC_MAIN_10);
            levels = hevc_levels(e.caps);
            defProfile = C2Config::PROFILE_HEVC_MAIN;
            break;
        default:
            profiles = {C2Config::PROFILE_AVC_CONSTRAINED_BASELINE, C2Config::PROFILE_AVC_BASELINE,
                        C2Config::PROFILE_AVC_MAIN, C2Config::PROFILE_AVC_CONSTRAINED_HIGH,
                        C2Config::PROFILE_AVC_PROGRESSIVE_HIGH, C2Config::PROFILE_AVC_HIGH};
            levels = avc_levels(e.caps);
            defProfile = C2Config::PROFILE_AVC_HIGH;
            break;
        }
        defLevel = levels.back();
        addParameter(DefineParam(mProfileLevel, C2_PARAMKEY_PROFILE_LEVEL)
                         .withDefault(new C2StreamProfileLevelInfo::input(0u, (C2Config::profile_t)defProfile,
                                                                          (C2Config::level_t)defLevel))
                         .withFields({C2F(mProfileLevel, profile).oneOf(profiles),
                                      C2F(mProfileLevel, level).oneOf(levels)})
                         .withSetter(ProfileLevelSetter)
                         .build());

        std::shared_ptr<C2StreamColorInfo::output> colorInfo = C2StreamColorInfo::output::AllocShared(
            {C2ChromaOffsetStruct::ITU_YUV_420_0()}, 0u, 8u, C2Color::YUV_420);
        helper->addStructDescriptors<C2ChromaOffsetStruct>();
        addParameter(DefineParam(mColorInfo, C2_PARAMKEY_CODED_COLOR_INFO).withConstValue(colorInfo).build());

        addParameter(DefineParam(mDefaultColorAspects, C2_PARAMKEY_DEFAULT_COLOR_ASPECTS)
                         .withDefault(new C2StreamColorAspectsTuning::output(
                             0u, C2Color::RANGE_UNSPECIFIED, C2Color::PRIMARIES_UNSPECIFIED,
                             C2Color::TRANSFER_UNSPECIFIED, C2Color::MATRIX_UNSPECIFIED))
                         .withFields({C2F(mDefaultColorAspects, range)
                                          .inRange(C2Color::RANGE_UNSPECIFIED, C2Color::RANGE_OTHER),
                                      C2F(mDefaultColorAspects, primaries)
                                          .inRange(C2Color::PRIMARIES_UNSPECIFIED, C2Color::PRIMARIES_OTHER),
                                      C2F(mDefaultColorAspects, transfer)
                                          .inRange(C2Color::TRANSFER_UNSPECIFIED, C2Color::TRANSFER_OTHER),
                                      C2F(mDefaultColorAspects, matrix)
                                          .inRange(C2Color::MATRIX_UNSPECIFIED, C2Color::MATRIX_OTHER)})
                         .withSetter(DefaultColorAspectsSetter)
                         .build());

        /* IMPLEMENTATION_DEFINED is listed so a Surface configuration is visible; the
         * component picks NV12 or P010 from the stream's bit depth either way. */
        std::vector<uint32_t> pixelFormats = {HAL_PIXEL_FORMAT_YCBCR_420_888};
        if (ten_bit(e.caps))
            pixelFormats.push_back(HAL_PIXEL_FORMAT_YCBCR_P010);
        pixelFormats.push_back(HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED);
        addParameter(DefineParam(mPixelFormat, C2_PARAMKEY_PIXEL_FORMAT)
                         .withDefault(new C2StreamPixelFormatInfo::output(0u, HAL_PIXEL_FORMAT_YCBCR_420_888))
                         .withFields({C2F(mPixelFormat, value).oneOf(pixelFormats)})
                         .withSetter((Setter<decltype(*mPixelFormat)>::StrictValueWithNoDeps))
                         .build());
    }

    static C2R SizeSetter(bool, const C2P<C2StreamPictureSizeInfo::output> &oldMe,
                          C2P<C2StreamPictureSizeInfo::output> &me)
    {
        C2R res = C2R::Ok();
        if (!me.F(me.v.width).supportsAtAll(me.v.width)) {
            res = res.plus(C2SettingResultBuilder::BadValue(me.F(me.v.width)));
            me.set().width = oldMe.v.width;
        }
        if (!me.F(me.v.height).supportsAtAll(me.v.height)) {
            res = res.plus(C2SettingResultBuilder::BadValue(me.F(me.v.height)));
            me.set().height = oldMe.v.height;
        }
        return res;
    }
    static C2R MaxPictureSizeSetter(bool, C2P<C2StreamMaxPictureSizeTuning::output> &me,
                                    const C2P<C2StreamPictureSizeInfo::output> &size)
    {
        me.set().width = c2_max(me.v.width, size.v.width);
        me.set().height = c2_max(me.v.height, size.v.height);
        C2R res = C2R::Ok();
        if (!me.F(me.v.width).supportsAtAll(me.v.width))
            res = res.plus(C2SettingResultBuilder::BadValue(me.F(me.v.width)));
        if (!me.F(me.v.height).supportsAtAll(me.v.height))
            res = res.plus(C2SettingResultBuilder::BadValue(me.F(me.v.height)));
        return res;
    }
    static C2R MaxInputSizeSetter(bool, C2P<C2StreamMaxBufferSizeInfo::input> &me,
                                  const C2P<C2StreamMaxPictureSizeTuning::output> &maxSize)
    {
        /* compression ratio >= 2 (C2SoftVpxDec) */
        me.set().value = c2_max((size_t)(((maxSize.v.width + 63) / 64) * ((maxSize.v.height + 63) / 64) * 3072),
                                kMinInputBufferSize);
        return C2R::Ok();
    }
    static C2R DefaultColorAspectsSetter(bool, C2P<C2StreamColorAspectsTuning::output> &me)
    {
        if (me.v.range > C2Color::RANGE_OTHER)
            me.set().range = C2Color::RANGE_OTHER;
        if (me.v.primaries > C2Color::PRIMARIES_OTHER)
            me.set().primaries = C2Color::PRIMARIES_OTHER;
        if (me.v.transfer > C2Color::TRANSFER_OTHER)
            me.set().transfer = C2Color::TRANSFER_OTHER;
        if (me.v.matrix > C2Color::MATRIX_OTHER)
            me.set().matrix = C2Color::MATRIX_OTHER;
        return C2R::Ok();
    }
    static C2R ProfileLevelSetter(bool, C2P<C2StreamProfileLevelInfo::input> &me)
    {
        /* profile/level arrive from the container; unknown values are not an error, the
         * bitstream decides (UNSUPPORTED from nv_video names any real gap) */
        (void)me;
        return C2R::Ok();
    }

    uint32_t bitDepthHint() const
    {
        switch (mProfileLevel->profile) {
        case C2Config::PROFILE_VP9_2:
        case C2Config::PROFILE_VP9_3:
        case C2Config::PROFILE_HEVC_MAIN_10:
            return 10;
        default:
            return mPixelFormat->value == HAL_PIXEL_FORMAT_YCBCR_P010 ? 10 : 8;
        }
    }
    std::pair<uint32_t, uint32_t> maxSize() const
    {
        return {c2_max(mMaxSize->width, mSize->width), c2_max(mMaxSize->height, mSize->height)};
    }

private:
    std::shared_ptr<C2StreamPictureSizeInfo::output> mSize;
    std::shared_ptr<C2StreamMaxPictureSizeTuning::output> mMaxSize;
    std::shared_ptr<C2StreamMaxBufferSizeInfo::input> mMaxInputSize;
    std::shared_ptr<C2StreamProfileLevelInfo::input> mProfileLevel;
    std::shared_ptr<C2StreamColorInfo::output> mColorInfo;
    std::shared_ptr<C2StreamColorAspectsTuning::output> mDefaultColorAspects;
    std::shared_ptr<C2StreamPixelFormatInfo::output> mPixelFormat;
    std::shared_ptr<C2ComponentAttributesSetting> mAttrib;
};

class Intf : public SimpleC2Interface<IntfImpl> {
public:
    Intf(const char *name, c2_node_id_t id, const std::shared_ptr<IntfImpl> &impl)
        : SimpleC2Interface<IntfImpl>(name, id, impl), mImpl(impl)
    {
    }
    const std::shared_ptr<IntfImpl> mImpl;
};

/* ------------------------------------------------------------------ output pool */

class Component::Pool : public OutputPool {
public:
    explicit Pool(Component *c) : mC(c) {}
    int fetch(uint32_t width, uint32_t height, uint32_t format, OutputBuffer *out) override
    {
        /*
         * Without a Surface (output pool on the plain gralloc allocator, not BUFFERQUEUE) the
         * client maps each block on the CPU (CCodec ByteBuffer/Image output). Waydroid's system
         * image does that for YCbCr_420_888 and YV12 blocks with a computed Android YV12 layout
         * instead of gralloc's (C2AllocationGralloc::map, waydroid/android_vendor_waydroid
         * waydroid-patches/base-patches-33/frameworks/av/0019-C2Gralloc-Drop-lockYCbCr-use.patch:
         * Y stride ALIGN(block width, 16), chroma stride ALIGN(y stride / 2, 16), Cr at
         * y stride * crop height, then Cb), so an NV12 block reads as stripes there. 8-bit
         * pictures for such a pool are YV12 blocks written in exactly that layout, which is
         * also gralloc's YV12 layout (system/core graphics.h HAL_PIXEL_FORMAT_YV12) and what
         * AOSP's software decoders fetch (frameworks/av C2SoftAvcDec.cpp:783-786 at
         * android-13.0.0_r75: YV12, width ALIGN128). Block width ALIGN(width, 128) keeps every
         * stride a 16-byte multiple; block height = the picture's display height (made even).
         */
        const bool cpu_client = mC->mOutputPool->getAllocatorId() == C2PlatformAllocatorStore::GRALLOC;
        const bool yv12 = cpu_client && format == NV_VIDEO_FORMAT_NV12;
        const uint32_t hal = format == NV_VIDEO_FORMAT_P010 ? HAL_PIXEL_FORMAT_YCBCR_P010
                             : yv12                          ? HAL_PIXEL_FORMAT_YV12
                                                             : HAL_PIXEL_FORMAT_YCBCR_420_888;
        const uint32_t block_w = yv12 ? align_up(width, 128) : (width + 1) & ~1u;
        std::shared_ptr<C2GraphicBlock> block;
        /*
         * Such a client reads every block on the CPU, so the block carries CPU_READ
         * (GRALLOC_USAGE_SW_READ_OFTEN, C2AllocatorGralloc.cpp C2AndroidMemoryUsage::asGrallocUsage),
         * as AOSP's software decoders fetch theirs ({CPU_READ, CPU_WRITE}, C2SoftAvcDec.cpp:784
         * at android-13.0.0_r75): gralloc then places it in CPU-cached memory (minigbm
         * vendor patch 0007). The GPU writes it, so no CPU_WRITE.
         */
        C2MemoryUsage usage = C2AndroidMemoryUsage::FromGrallocUsage(kGrallocUsageVideoDecoder);
        if (cpu_client)
            usage.expected |= C2MemoryUsage::CPU_READ;
        c2_status_t err = mC->mOutputPool->fetchGraphicBlock(block_w, (height + 1) & ~1u, hal, usage, &block);
        if (err == C2_BLOCKING || err == C2_TIMED_OUT)
            return -EAGAIN;
        if (err != C2_OK) {
            ALOGE("fetchGraphicBlock(%ux%u, format 0x%x) failed: %d", block_w, height, hal, err);
            return -EIO;
        }
        native_handle_t *nh = UnwrapNativeCodec2GrallocHandle(block->handle());
        if (!nh) {
            ALOGE("output block has no gralloc handle");
            return -EINVAL;
        }
        const auto *h = reinterpret_cast<const cros_gralloc_handle *>(nh);
        int rc = 0;
        if (h->magic != kCrosGrallocMagic || nh->numFds < 1 || (!yv12 && h->num_planes < 2)) {
            ALOGE("output block is not a %s minigbm buffer (magic 0x%x, planes %u): "
                  "the decoder needs minigbm gbm_mesa with nvidia-prolapse/gralloc",
                  yv12 ? "YV12" : "2-plane", h->magic, h->num_planes);
            rc = -EINVAL;
        } else if (yv12) {
            /* Android YV12: Y, then Cr, then Cb, both chroma planes at the chroma stride. */
            const uint32_t ys = align_up(h->width, 16), cs = align_up(ys / 2, 16);
            const uint64_t cr = (uint64_t)ys * h->height;
            const uint64_t cb = cr + (uint64_t)cs * (h->height / 2);
            const uint64_t end = cb + (uint64_t)cs * (h->height / 2);
            if (h->strides[0] != ys || end > h->total_size) {
                ALOGE("YV12 output block %ux%u: stride %u, %llu bytes; the Android YV12 layout "
                      "needs stride %u and %llu bytes",
                      h->width, h->height, h->strides[0], (unsigned long long)h->total_size, ys,
                      (unsigned long long)end);
                rc = -EINVAL;
            } else {
                out->target = NvVideoTarget{};
                out->target.dmabuf_fd[0] = h->fds[0];
                out->target.dmabuf_fd[1] = h->fds[0];
                out->target.cr_dmabuf_fd = h->fds[0];
                out->target.offset[0] = 0;
                out->target.offset[1] = cb;
                out->target.cr_offset = cr;
                out->target.pitch[0] = ys;
                out->target.pitch[1] = cs;
                out->target.cr_pitch = cs;
                out->target.modifier = 0; /* the R8 carrier is LINEAR */
                out->target.format = NV_VIDEO_FORMAT_YUV420P;
                out->target.width = h->width;
                out->target.height = h->height;
                out->target.acquire_fence_fd = -1; /* fetchGraphicBlock waited for the dequeue fence */
            }
        } else {
            out->target = NvVideoTarget{};
            /* fds stay open until the block is returned (we keep nh until then) */
            out->target.dmabuf_fd[0] = h->fds[0];
            out->target.dmabuf_fd[1] = h->num_planes > 1 && nh->numFds > 1 ? h->fds[1] : h->fds[0];
            out->target.offset[0] = h->offsets[0];
            out->target.offset[1] = h->offsets[1];
            out->target.pitch[0] = h->strides[0];
            out->target.pitch[1] = h->strides[1];
            out->target.modifier = h->format_modifier;
            out->target.format = format;
            out->target.width = h->width;
            out->target.height = h->height;
            out->target.acquire_fence_fd = -1; /* fetchGraphicBlock waited for the dequeue fence */
        }
        if (rc == 0) {
            out->cookie = mC->mNextCookie++;
            mC->mBlocks[out->cookie] = block;
            mC->mHandles[out->cookie] = nh;
            return 0;
        }
        /* UnwrapNativeCodec2GrallocHandle memcpy's the block's fds (C2AllocatorGralloc.cpp
         * UnwrapNativeHandle, no dup): they belong to the block, free only the handle struct. */
        native_handle_delete(nh);
        return rc;
    }
    void discard(const OutputBuffer &buf) override { mC->drop_block(buf.cookie); }

private:
    Component *mC;
};

void Component::drop_block(uint64_t cookie)
{
    mBlocks.erase(cookie);
    auto it = mHandles.find(cookie);
    if (it != mHandles.end()) {
        native_handle_delete(it->second); /* fds are the block's (unwrapped without dup) */
        mHandles.erase(it);
    }
}

/* ------------------------------------------------------------------ component */

Component::Component(NvVideoDevice *dev, const CodecEntry &entry, c2_node_id_t id,
                     const std::shared_ptr<C2ReflectorHelper> &reflector)
    : mDevice(dev), mEntry(entry),
      mIntf(std::make_shared<Intf>(entry.name, id, std::make_shared<IntfImpl>(reflector, entry)))
{
}

Component::~Component() { release(); }

std::shared_ptr<C2ComponentInterface> Component::intf() { return mIntf; }

c2_status_t Component::setListener_vb(const std::shared_ptr<Listener> &listener, c2_blocking_t)
{
    std::lock_guard<std::mutex> lk(mLock);
    if (mRunning && listener)
        return C2_BAD_STATE;
    mListener = listener;
    return C2_OK;
}

c2_status_t Component::start()
{
    std::lock_guard<std::mutex> lk(mLock);
    if (mRunning)
        return C2_BAD_STATE;
    mQuit = false;
    mError = false;
    mRunning = true;
    mThread = std::thread(&Component::loop, this);
    return C2_OK;
}

c2_status_t Component::stop()
{
    {
        std::lock_guard<std::mutex> lk(mLock);
        if (!mRunning)
            return C2_OK;
        mQuit = true;
    }
    mCv.notify_all();
    if (mThread.joinable())
        mThread.join();
    std::lock_guard<std::mutex> lk(mLock);
    mRunning = false;
    mQueue.clear();
    mPending.clear();
    mDecoder.reset();
    mCsd.clear();
    while (!mHandles.empty())
        drop_block(mHandles.begin()->first);
    mBlocks.clear();
    mOutputPool.reset();
    return C2_OK;
}

c2_status_t Component::reset()
{
    stop();
    std::lock_guard<std::mutex> lk(mLock);
    mListener.reset();
    return C2_OK;
}

c2_status_t Component::release() { return reset(); }

c2_status_t Component::queue_nb(std::list<std::unique_ptr<C2Work>> *const items)
{
    {
        std::lock_guard<std::mutex> lk(mLock);
        if (!mRunning)
            return C2_BAD_STATE;
        while (!items->empty()) {
            mQueue.push_back(std::move(items->front()));
            items->pop_front();
        }
    }
    mCv.notify_all();
    return C2_OK;
}

c2_status_t Component::drain_nb(drain_mode_t mode)
{
    /* CCodec drains with an END_OF_STREAM input work; a separate drain request carries
     * no frame index to return, so it is not offered. */
    (void)mode;
    return C2_OMITTED;
}

c2_status_t Component::flush_sm(flush_mode_t, std::list<std::unique_ptr<C2Work>> *const flushedWork)
{
    std::lock_guard<std::mutex> lk(mLock);
    while (!mQueue.empty()) {
        flushedWork->push_back(std::move(mQueue.front()));
        mQueue.pop_front();
    }
    if (mDecoder) {
        std::vector<Output> outs;
        if (mDecoder->flush(&outs) != Status::Ok)
            fatal(mDecoder->error());
        for (auto &o : outs) {
            auto it = mPending.find(o.index);
            if (it != mPending.end()) {
                flushedWork->push_back(std::move(it->second));
                mPending.erase(it);
            }
        }
    }
    for (auto &p : mPending)
        flushedWork->push_back(std::move(p.second));
    mPending.clear();
    return C2_OK;
}

void Component::fatal(const std::string &why)
{
    ALOGE("%s: %s", mEntry.name, why.c_str());
    mError = true;
}

bool Component::ensure_decoder(std::string *error)
{
    if (mDecoder)
        return true;
    Config cfg;
    cfg.codec = mEntry.codec;
    auto mx = mIntf->mImpl->maxSize();
    cfg.max_width = c2_max(mx.first, 16u);
    cfg.max_height = c2_max(mx.second, 16u);
    cfg.bit_depth = mIntf->mImpl->bitDepthHint();
    cfg.extra_output_slots = 0; /* pictures are copied out and released at once */
    if (mEntry.codec == NV_VIDEO_CODEC_AV1 || mEntry.codec == NV_VIDEO_CODEC_H264 ||
        mEntry.codec == NV_VIDEO_CODEC_HEVC)
        cfg.codec_config = mCsd; /* av1C / avcC / hvcC / Annex-B parameter sets */
    mDecoder.reset(Decoder::create(mDevice, cfg, error));
    return mDecoder != nullptr;
}

void Component::deliver(std::vector<Output> &outs, std::list<std::unique_ptr<C2Work>> *done)
{
    for (auto &o : outs) {
        auto it = mPending.find(o.index);
        if (it == mPending.end()) {
            ALOGE("%s: output for unknown frame index %llu", mEntry.name, (unsigned long long)o.index);
            if (o.release_fence >= 0)
                close(o.release_fence);
            continue;
        }
        std::unique_ptr<C2Work> work = std::move(it->second);
        mPending.erase(it);
        auto &wl = work->worklets.front();
        wl->output.flags = C2FrameData::flags_t(0);
        wl->output.buffers.clear();
        wl->output.ordinal = work->input.ordinal;
        work->result = C2_OK;
        work->workletsProcessed = 1u;
        if (!o.empty) {
            if (o.release_fence >= 0) {
                struct pollfd p = {o.release_fence, POLLIN, 0};
                int r;
                do
                    r = poll(&p, 1, kCopyFenceTimeoutMs);
                while (r < 0 && errno == EINTR);
                close(o.release_fence);
                if (r != 1) {
                    fatal("copy-out fence did not signal within " + std::to_string(kCopyFenceTimeoutMs) + " ms");
                    work->result = C2_CORRUPTED;
                }
            }
            auto bit = mBlocks.find(o.buffer.cookie);
            if (bit != mBlocks.end() && work->result == C2_OK) {
                const C2Rect crop(o.picture.crop_width, o.picture.crop_height);
                std::shared_ptr<C2Buffer> buf = C2Buffer::CreateGraphicBuffer(bit->second->share(crop, C2Fence()));
                auto aspects = std::make_shared<C2StreamColorAspectsInfo::output>(
                    0u, o.picture.full_range ? C2Color::RANGE_FULL : C2Color::RANGE_LIMITED,
                    map_primaries(o.picture.color_primaries), map_transfer(o.picture.transfer),
                    map_matrix(o.picture.matrix));
                buf->setInfo(aspects);
                wl->output.buffers.push_back(buf);
                if (o.size_changed)
                    wl->output.configUpdate.push_back(
                        C2Param::Copy(C2StreamPictureSizeInfo::output(0u, o.picture.crop_width,
                                                                      o.picture.crop_height)));
            }
            drop_block(o.buffer.cookie); /* the C2Buffer keeps its own reference */
        }
        if (o.corrupt)
            ALOGW("%s: frame %llu not decodable (%s); returned without a picture", mEntry.name,
                  (unsigned long long)o.index, nv_video_last_error());
        if (o.eos)
            wl->output.flags = C2FrameData::FLAG_END_OF_STREAM;
        done->push_back(std::move(work));
    }
}

/* Listener callbacks run without mLock: they cross binder, and queue_nb/flush_sm must not wait on them. */
void Component::notify(std::list<std::unique_ptr<C2Work>> &done, std::unique_lock<std::mutex> &lk)
{
    std::shared_ptr<Listener> l = mListener;
    if (done.empty() || !l)
        return;
    lk.unlock();
    l->onWorkDone_nb(shared_from_this(), std::move(done));
    lk.lock();
}

bool Component::process(std::unique_ptr<C2Work> &work, std::unique_lock<std::mutex> &lk)
{
    std::list<std::unique_ptr<C2Work>> done;
    const uint64_t index = work->input.ordinal.frameIndex.peeku();
    const bool eos = work->input.flags & C2FrameData::FLAG_END_OF_STREAM;
    const bool csd = work->input.flags & C2FrameData::FLAG_CODEC_CONFIG;
    const uint8_t *data = nullptr;
    size_t len = 0;
    std::unique_ptr<C2ReadView> view;
    if (!work->input.buffers.empty() && work->input.buffers[0]) {
        const auto &blocks = work->input.buffers[0]->data().linearBlocks();
        if (!blocks.empty()) {
            view = std::make_unique<C2ReadView>(blocks.front().map().get());
            if (view->error() != C2_OK) {
                fatal("cannot map input buffer");
                return true;
            }
            data = view->data();
            len = blocks.front().size();
        }
    }

    if (csd && !mDecoder) {
        mCsd.insert(mCsd.end(), data, data + len);
        work->result = C2_OK;
        work->workletsProcessed = 1u;
        work->worklets.front()->output.ordinal = work->input.ordinal;
        done.push_back(std::move(work));
    } else {
        std::string err;
        if (!ensure_decoder(&err)) {
            fatal(err);
            return true;
        }
        if (!mOutputPool) {
            C2BlockPool::local_id_t poolId = C2BlockPool::BASIC_GRAPHIC;
            std::vector<std::unique_ptr<C2Param>> heap;
            if (mIntf->query_vb({}, {C2PortBlockPoolsTuning::output::PARAM_TYPE}, C2_DONT_BLOCK, &heap) == C2_OK &&
                heap.size() == 1 && heap[0]) {
                auto *p = C2PortBlockPoolsTuning::output::From(heap[0].get());
                if (p && p->flexCount() > 0)
                    poolId = p->m.values[0];
            }
            c2_status_t e = GetCodec2BlockPool(poolId, shared_from_this(), &mOutputPool);
            if (e != C2_OK) {
                fatal("GetCodec2BlockPool(" + std::to_string(poolId) + ") failed: " + std::to_string(e));
                return true;
            }
        }
        Pool pool(this);
        std::vector<Output> outs;
        mPending[index] = std::move(work);
        Status st = mDecoder->queue(data, len, index, eos, csd, &pool, &outs);
        if (st == Status::Again) {
            /* not consumed: back to the queue front, retried once a buffer is free */
            work = std::move(mPending[index]);
            mPending.erase(index);
        }
        deliver(outs, &done);
        if (st == Status::Error) {
            fatal(mDecoder->error());
        }
        if (st == Status::Again) {
            notify(done, lk);
            return false;
        }
    }
    notify(done, lk);
    return true;
}

void Component::loop()
{
    std::unique_lock<std::mutex> lk(mLock);
    while (!mQuit) {
        if (mError) {
            if (std::shared_ptr<Listener> l = mListener) {
                lk.unlock();
                l->onError_nb(shared_from_this(), C2_CORRUPTED);
                lk.lock();
            }
            mCv.wait(lk, [this] { return mQuit; });
            break;
        }
        if (mQueue.empty()) {
            if (mDecoder && mOutputPool) {
                /* pictures held back by an empty output pool */
                Pool pool(this);
                std::vector<Output> outs;
                if (mDecoder->drain(&pool, &outs) == Status::Error)
                    fatal(mDecoder->error());
                std::list<std::unique_ptr<C2Work>> done;
                deliver(outs, &done);
                notify(done, lk);
            }
            mCv.wait_for(lk, std::chrono::milliseconds(4), [this] { return mQuit || !mQueue.empty(); });
            continue;
        }
        std::unique_ptr<C2Work> work = std::move(mQueue.front());
        mQueue.pop_front();
        if (!process(work, lk)) {
            mQueue.push_front(std::move(work));
            /* output pool exhausted: the consumer returns buffers asynchronously */
            mCv.wait_for(lk, std::chrono::milliseconds(4), [this] { return mQuit; });
        }
    }
}

/* ------------------------------------------------------------------ store */

std::shared_ptr<Store> Store::Create(std::string *error, std::string *no_nvdec_gpu)
{
    std::shared_ptr<Store> s(new Store());
    NvVideoDeviceInfo info{};
    int rc = nv_video_device_open(&s->mDevice, &info);
    if (rc != 0) {
        *error = std::string("nv_video_device_open failed (") + std::to_string(rc) + "): " + nv_video_last_error();
        return nullptr;
    }
    if (info.abi_version != NV_VIDEO_ABI_VERSION) {
        *error = "nv_video backend ABI " + std::to_string(info.abi_version) + ", this service was built for " +
                 std::to_string(NV_VIDEO_ABI_VERSION);
        return nullptr;
    }
    if (info.nvdec_class == 0) {
        *no_nvdec_gpu = info.name;
        return nullptr;
    }
    s->mEntries = supported_codecs(s->mDevice, error);
    if (!error->empty())
        return nullptr;
    if (s->mEntries.empty()) {
        *error = std::string("GPU ") + info.name + " supports none of VP9/AV1/HEVC/H.264 decode";
        return nullptr;
    }
    s->mReflector = std::make_shared<C2ReflectorHelper>();
    ALOGI("NVDEC on %s (class 0x%x, %u engine(s)): %zu decoder(s)", info.name, info.nvdec_class, info.nvdec_engines,
          s->mEntries.size());
    return s;
}

Store::~Store()
{
    if (mDevice)
        nv_video_device_close(mDevice);
}

/* Runtime route (read on every list/create, so no restage or rebuild is needed to switch):
 * persist.waydroid.nvidia_video_decode unset or "on" -> c2.nvprolapse.* are offered;
 * "off" -> this store offers nothing and the framework keeps the image's software codecs (OMX.google.*
 * stay ahead of c2.android.* via the ranks native mode sets), i.e. stock Waydroid's choice.
 * Any other value offers nothing and logs why. MediaCodecList is built once per app process, so a
 * change applies to processes started afterwards. Set with `waydroid prop set` or waydroid.cfg [properties]. */
bool decode_routed_here()
{
    char v[PROP_VALUE_MAX] = "";
    __system_property_get(kRouteProp, v);
    static std::mutex m;
    static std::string last = "\x01";
    std::lock_guard<std::mutex> l(m);
    const bool on = !strcmp(v, "") || !strcmp(v, "on");
    const bool off = !strcmp(v, "off");
    if (last != v) {
        last = v;
        if (on)
            ALOGI("%s=%s: hardware decode routed to c2.nvprolapse.*", kRouteProp, v[0] ? v : "(unset)");
        else if (off)
            ALOGI("%s=off: offering no components; apps use the image's software decoders", kRouteProp);
        else
            ALOGE("%s=\"%s\" is not on|off: offering no components until it is fixed", kRouteProp, v);
    }
    return on;
}

const CodecEntry *Store::find(const C2String &name) const
{
    if (!decode_routed_here())
        return nullptr;
    for (const auto &e : mEntries)
        if (name == e.name)
            return &e;
    return nullptr;
}

c2_status_t Store::createComponent(C2String name, std::shared_ptr<C2Component> *const component)
{
    const CodecEntry *e = find(name);
    if (!e)
        return C2_NOT_FOUND;
    *component = std::make_shared<Component>(mDevice, *e, mNextId++, mReflector);
    return C2_OK;
}

c2_status_t Store::createInterface(C2String name, std::shared_ptr<C2ComponentInterface> *const interface)
{
    const CodecEntry *e = find(name);
    if (!e)
        return C2_NOT_FOUND;
    *interface = std::make_shared<Intf>(e->name, mNextId++, std::make_shared<IntfImpl>(mReflector, *e));
    return C2_OK;
}

std::vector<std::shared_ptr<const C2Component::Traits>> Store::listComponents()
{
    std::vector<std::shared_ptr<const C2Component::Traits>> v;
    if (!decode_routed_here())
        return v;
    for (const auto &e : mEntries) {
        auto t = std::make_shared<C2Component::Traits>();
        t->name = e.name;
        t->domain = C2Component::DOMAIN_VIDEO;
        t->kind = C2Component::KIND_DECODER;
        t->rank = 0x10; /* ahead of the software decoders (0x200); xml rank= may override */
        t->mediaType = e.mime;
        t->owner = "default"; /* the client overrides it with the instance name */
        v.push_back(t);
    }
    return v;
}

c2_status_t Store::query_sm(const std::vector<C2Param *> &stackParams, const std::vector<C2Param::Index> &heapParamIndices,
                            std::vector<std::unique_ptr<C2Param>> *const) const
{
    return stackParams.empty() && heapParamIndices.empty() ? C2_OK : C2_BAD_INDEX;
}

c2_status_t Store::config_sm(const std::vector<C2Param *> &params, std::vector<std::unique_ptr<C2SettingResult>> *const)
{
    return params.empty() ? C2_OK : C2_BAD_INDEX;
}

c2_status_t Store::querySupportedParams_nb(std::vector<std::shared_ptr<C2ParamDescriptor>> *const) const
{
    return C2_OK;
}

c2_status_t Store::querySupportedValues_sm(std::vector<C2FieldSupportedValuesQuery> &fields) const
{
    return fields.empty() ? C2_OK : C2_BAD_INDEX;
}

} // namespace nvc2
