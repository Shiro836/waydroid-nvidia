/* SPDX-License-Identifier: Apache-2.0
 * Codec2 front of the nvc2 decoder: component store + one decoder component per codec
 * that nv_video_query_caps() reports as supported. Built against pinned AOSP
 * frameworks/av (android-13 era) Codec2 headers; the parameter set follows the AOSP
 * software decoders (C2SoftVpxDec/C2SoftHevcDec IntfImpl, SimpleInterface BaseParams).
 */
#ifndef NVC2_C2_COMPONENT_H
#define NVC2_C2_COMPONENT_H

#include <C2Component.h>
#include <C2Config.h>
#include <cutils/native_handle.h>
#include <util/C2InterfaceHelper.h>

#include <condition_variable>
#include <deque>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

#include "nvc2_caps.h"
#include "nvc2_decoder.h"

namespace nvc2 {

class Intf;

/* System property that routes decoding to this store or to the image's software codecs. */
inline constexpr char kRouteProp[] = "persist.waydroid.nvidia_video_decode";
bool decode_routed_here();

class Store : public C2ComponentStore {
public:
    /* Opens the device and queries caps. A failure is fatal for the service (no store,
     * no silent empty list): *error names it. A GPU without an NVDEC class this decoder
     * drives is no failure: no store, *error empty, *no_nvdec_gpu = its name. */
    static std::shared_ptr<Store> Create(std::string *error, std::string *no_nvdec_gpu);
    ~Store() override;

    C2String getName() const override { return "nvprolapse"; }
    c2_status_t createComponent(C2String name, std::shared_ptr<C2Component> *const component) override;
    c2_status_t createInterface(C2String name, std::shared_ptr<C2ComponentInterface> *const interface) override;
    std::vector<std::shared_ptr<const C2Component::Traits>> listComponents() override;
    c2_status_t copyBuffer(std::shared_ptr<C2GraphicBuffer>, std::shared_ptr<C2GraphicBuffer>) override
    {
        return C2_OMITTED;
    }
    c2_status_t query_sm(const std::vector<C2Param *> &stackParams,
                         const std::vector<C2Param::Index> &heapParamIndices,
                         std::vector<std::unique_ptr<C2Param>> *const heapParams) const override;
    c2_status_t config_sm(const std::vector<C2Param *> &params,
                          std::vector<std::unique_ptr<C2SettingResult>> *const failures) override;
    std::shared_ptr<C2ParamReflector> getParamReflector() const override { return mReflector; }
    c2_status_t querySupportedParams_nb(std::vector<std::shared_ptr<C2ParamDescriptor>> *const params) const override;
    c2_status_t querySupportedValues_sm(std::vector<C2FieldSupportedValuesQuery> &fields) const override;

    const std::vector<CodecEntry> &entries() const { return mEntries; }

private:
    Store() = default;
    const CodecEntry *find(const C2String &name) const;

    NvVideoDevice *mDevice = nullptr;
    std::vector<CodecEntry> mEntries;
    std::shared_ptr<C2ReflectorHelper> mReflector;
    std::atomic<c2_node_id_t> mNextId{1};
};

class Component : public C2Component, public std::enable_shared_from_this<Component> {
public:
    Component(NvVideoDevice *dev, const CodecEntry &entry, c2_node_id_t id,
              const std::shared_ptr<C2ReflectorHelper> &reflector);
    ~Component() override;

    c2_status_t setListener_vb(const std::shared_ptr<Listener> &listener, c2_blocking_t mayBlock) override;
    c2_status_t queue_nb(std::list<std::unique_ptr<C2Work>> *const items) override;
    c2_status_t announce_nb(const std::vector<C2WorkOutline> &) override { return C2_OMITTED; }
    c2_status_t flush_sm(flush_mode_t mode, std::list<std::unique_ptr<C2Work>> *const flushedWork) override;
    c2_status_t drain_nb(drain_mode_t mode) override;
    c2_status_t start() override;
    c2_status_t stop() override;
    c2_status_t reset() override;
    c2_status_t release() override;
    std::shared_ptr<C2ComponentInterface> intf() override;

private:
    class Pool;
    void loop();
    bool process(std::unique_ptr<C2Work> &work, std::unique_lock<std::mutex> &lk);
    void deliver(std::vector<Output> &outs, std::list<std::unique_ptr<C2Work>> *done);
    void notify(std::list<std::unique_ptr<C2Work>> &done, std::unique_lock<std::mutex> &lk);
    bool ensure_decoder(std::string *error);
    void fatal(const std::string &why);
    void drop_block(uint64_t cookie);

    NvVideoDevice *mDevice;
    CodecEntry mEntry;
    std::shared_ptr<Intf> mIntf;

    std::mutex mLock;
    std::condition_variable mCv;
    std::thread mThread;
    bool mRunning = false, mQuit = false, mError = false;
    std::shared_ptr<Listener> mListener;
    std::deque<std::unique_ptr<C2Work>> mQueue;          /* not yet given to the decoder */
    std::map<uint64_t, std::unique_ptr<C2Work>> mPending; /* given, waiting for a picture */
    std::unique_ptr<Decoder> mDecoder;
    std::vector<uint8_t> mCsd;
    std::shared_ptr<C2BlockPool> mOutputPool;
    std::map<uint64_t, std::shared_ptr<C2GraphicBlock>> mBlocks; /* cookie -> fetched block */
    std::map<uint64_t, native_handle_t *> mHandles;             /* cookie -> unwrapped gralloc handle */
    uint64_t mNextCookie = 1;
};

} // namespace nvc2

#endif
