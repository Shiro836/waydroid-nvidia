/* SPDX-License-Identifier: MIT
 * Scripted stand-in for the nv_video ABI (nv_video.h) so the decoder core's state machine
 * can be tested on the host without a GPU. It models the documented behaviour only:
 * display-order output, hidden frames + show_existing (VP9/AV1), POC reordering
 * (H.264/HEVC), a bounded number of unreleased pictures (-> NV_VIDEO_E_AGAIN),
 * keyframe-after-flush, END_OF_STREAM drain, copy_out validation, device loss.
 *
 * Stub unit format (8 bytes): 'N', flags, w lo, w hi, h lo, h hi, poc, bit depth
 *   flags bit0 key, bit1 show, bit2 show_existing (shows the oldest hidden frame), bit3 corrupt
 */
#include "nv_video_stub.h"

#include <algorithm>
#include <cstring>
#include <deque>
#include <string>
#include <unistd.h>
#include <vector>

StubControl g_stub;

static thread_local std::string t_err;
static void set_err(const std::string &e) { t_err = e; }

struct NvVideoDevice { int dummy; };
struct NvVideoSession {
    NvVideoSessionInfo info;
    std::deque<NvVideoPicture> ready, hidden;
    std::vector<NvVideoPicture> reorder;
    unsigned outstanding = 0;
    uint64_t next_id = 1;
    bool need_key = true;
};

extern "C" {

int nv_video_device_open(NvVideoDevice **out, NvVideoDeviceInfo *info)
{
    *out = new NvVideoDevice{};
    if (info) {
        memset(info, 0, sizeof(*info));
        info->abi_version = NV_VIDEO_ABI_VERSION;
        info->pci_vendor = 0x10de;
        info->nvdec_class = g_stub.nvdec_class;
        info->nvdec_engines = 1;
        strcpy(info->name, "stub");
    }
    return 0;
}
void nv_video_device_close(NvVideoDevice *d) { delete d; }

int nv_video_query_caps(NvVideoDevice *, uint32_t codec, NvVideoCaps *caps)
{
    if (codec < 1 || codec > 4) {
        set_err("bad codec");
        return NV_VIDEO_E_INVALID;
    }
    *caps = g_stub.caps[codec];
    caps->codec = codec;
    return 0;
}

int nv_video_session_create(NvVideoDevice *, const NvVideoSessionInfo *info, NvVideoSession **out)
{
    g_stub.sessions_created++;
    if (info->codec_config_len)
        g_stub.last_codec_config.assign(info->codec_config, info->codec_config + info->codec_config_len);
    auto *s = new NvVideoSession{};
    s->info = *info;
    *out = s;
    return 0;
}
void nv_video_session_destroy(NvVideoSession *s)
{
    g_stub.sessions_destroyed++;
    g_stub.outstanding_at_destroy = s->outstanding + (unsigned)s->ready.size();
    delete s;
}

static bool reorders(const NvVideoSession *s)
{
    return s->info.codec == NV_VIDEO_CODEC_H264 || s->info.codec == NV_VIDEO_CODEC_HEVC;
}

static void reorder_out(NvVideoSession *s, size_t keep)
{
    while (s->reorder.size() > keep) {
        auto it = std::min_element(s->reorder.begin(), s->reorder.end(),
                                   [](const NvVideoPicture &a, const NvVideoPicture &b) {
                                       return a.reserved[0] < b.reserved[0];
                                   });
        s->ready.push_back(*it);
        s->reorder.erase(it);
    }
}

int nv_video_decode(NvVideoSession *s, const uint8_t *d, size_t len, uint64_t ts, uint32_t flags)
{
    g_stub.decode_calls++;
    if (g_stub.device_lost) {
        set_err("stub: channel error");
        return NV_VIDEO_E_DEVICE_LOST;
    }
    if (len) {
        if (len < 8 || d[0] != 'N') {
            if (len >= 1 && d[0] == 'P') /* parameter set unit (codec config) */
                return 0;
            set_err("stub: not a stub unit");
            return NV_VIDEO_E_BITSTREAM;
        }
        const unsigned limit = s->info.extra_output_slots + g_stub.dpb_slots;
        if (s->outstanding + s->ready.size() + s->reorder.size() >= limit) {
            set_err("stub: no free slot");
            return NV_VIDEO_E_AGAIN;
        }
        const uint8_t fl = d[1];
        if (fl & 8) {
            set_err("stub: corrupt unit");
            return NV_VIDEO_E_BITSTREAM;
        }
        if (s->need_key && !(fl & 1)) {
            set_err("stub: first unit after flush is not a keyframe");
            return NV_VIDEO_E_BITSTREAM;
        }
        s->need_key = false;
        NvVideoPicture p{};
        p.id = s->next_id++;
        p.timestamp = ts;
        p.width = p.crop_width = d[2] | d[3] << 8;
        p.height = p.crop_height = d[4] | d[5] << 8;
        p.bit_depth = d[7] ? d[7] : 8;
        p.keyframe = fl & 1;
        p.reserved[0] = d[6];
        if (p.width > s->info.max_width || p.height > s->info.max_height) {
            set_err("stub: stream larger than the session");
            return NV_VIDEO_E_UNSUPPORTED;
        }
        if (reorders(s)) {
            s->reorder.push_back(p);
            reorder_out(s, g_stub.reorder_depth);
        } else if (fl & 4) {
            if (s->hidden.empty()) {
                set_err("stub: show_existing without a hidden frame");
                return NV_VIDEO_E_BITSTREAM;
            }
            NvVideoPicture h = s->hidden.front();
            s->hidden.pop_front();
            h.timestamp = ts;
            s->ready.push_back(h);
        } else if (fl & 2) {
            s->ready.push_back(p);
        } else {
            s->hidden.push_back(p);
        }
    }
    if (flags & NV_VIDEO_DECODE_END_OF_STREAM) {
        reorder_out(s, 0);
        s->hidden.clear();
    }
    return 0;
}

int nv_video_next_picture(NvVideoSession *s, NvVideoPicture *pic)
{
    if (g_stub.device_lost) {
        set_err("stub: channel error");
        return NV_VIDEO_E_DEVICE_LOST;
    }
    if (s->ready.empty())
        return 0;
    *pic = s->ready.front();
    s->ready.pop_front();
    s->outstanding++;
    return 1;
}

int nv_video_flush(NvVideoSession *s)
{
    s->ready.clear();
    s->hidden.clear();
    s->reorder.clear();
    s->need_key = true;
    return 0;
}

int nv_video_copy_out(NvVideoSession *s, uint64_t id, const NvVideoTarget *t, int *fence)
{
    g_stub.copies++;
    const bool p010 = t->format == NV_VIDEO_FORMAT_P010;
    const bool planar = t->format == NV_VIDEO_FORMAT_YUV420P;
    if (t->format != NV_VIDEO_FORMAT_NV12 && !p010 && !planar) {
        set_err("stub: bad target format");
        return NV_VIDEO_E_INVALID;
    }
    if (t->dmabuf_fd[0] < 0 || t->dmabuf_fd[1] < 0 || (planar && t->cr_dmabuf_fd < 0)) {
        set_err("stub: bad fd");
        return NV_VIDEO_E_INVALID;
    }
    (void)s;
    uint8_t mark = (uint8_t)id;
    if (pwrite(t->dmabuf_fd[0], &mark, 1, (off_t)t->offset[0]) != 1 ||
        pwrite(t->dmabuf_fd[1], &mark, 1, (off_t)t->offset[1]) != 1) {
        set_err("stub: write failed");
        return NV_VIDEO_E_INVALID;
    }
    g_stub.last_copy_format = t->format;
    *fence = g_stub.fence_fds ? dup(1) : -1;
    return 0;
}

int nv_video_picture_release(NvVideoSession *s, uint64_t)
{
    if (!s->outstanding) {
        set_err("stub: release without picture");
        return NV_VIDEO_E_INVALID;
    }
    s->outstanding--;
    return 0;
}

const char *nv_video_last_error(void) { return t_err.c_str(); }

} // extern "C"
