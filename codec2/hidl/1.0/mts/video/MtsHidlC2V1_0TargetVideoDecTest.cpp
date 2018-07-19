/*
 * Copyright (C) 2018 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

//#define LOG_NDEBUG 0
#define LOG_TAG "codec2_hidl_hal_video_dec_test"

#include <android-base/logging.h>
#include <gtest/gtest.h>
#include <stdio.h>
#include <fstream>

#include <codec2/hidl/client.h>
#include <C2AllocatorIon.h>
#include <C2Config.h>
#include <C2Debug.h>
#include <C2Buffer.h>
#include <C2BufferPriv.h>

using android::C2AllocatorIon;

#include <VtsHalHidlTargetTestBase.h>
#include "media_c2_video_hidl_test_common.h"
#include "media_c2_hidl_test_common.h"

struct FrameInfo {
    int bytesCount;
    uint32_t flags;
    int64_t timestamp;
};

class LinearBuffer : public C2Buffer {
   public:
    explicit LinearBuffer(const std::shared_ptr<C2LinearBlock>& block)
        : C2Buffer(
              {block->share(block->offset(), block->size(), ::C2Fence())}) {}
};

static ComponentTestEnvironment* gEnv = nullptr;

namespace {

class Codec2VideoDecHidlTest : public ::testing::VtsHalHidlTargetTestBase {
   private:
    typedef ::testing::VtsHalHidlTargetTestBase Super;

   public:
    ::std::string getTestCaseInfo() const override {
        return ::std::string() +
                "Component: " + gEnv->getComponent().c_str() + " | " +
                "Instance: " + gEnv->getInstance().c_str() + " | " +
                "Res: " + gEnv->getRes().c_str();
    }

    // google.codec2 Video test setup
    virtual void SetUp() override {
        Super::SetUp();
        mDisableTest = false;
        ALOGV("Codec2VideoDecHidlTest SetUp");
        mClient = android::Codec2Client::CreateFromService(
            gEnv->getInstance().c_str());
        ASSERT_NE(mClient, nullptr);
        mListener.reset(new CodecListener(
            [this](std::list<std::unique_ptr<C2Work>>& workItems) {
                handleWorkDone(workItems);
            }));
        ASSERT_NE(mListener, nullptr);
        for (int i = 0; i < MAX_INPUT_BUFFERS; ++i) {
            mWorkQueue.emplace_back(new C2Work);
        }
        mClient->createComponent(gEnv->getComponent().c_str(), mListener,
                                 &mComponent);
        ASSERT_NE(mComponent, nullptr);

        std::shared_ptr<C2AllocatorStore> store =
            android::GetCodec2PlatformAllocatorStore();
        CHECK_EQ(store->fetchAllocator(C2AllocatorStore::DEFAULT_LINEAR,
                                       &mLinearAllocator),
                 C2_OK);
        mLinearPool = std::make_shared<C2PooledBlockPool>(mLinearAllocator,
                                                          mBlockPoolId++);
        ASSERT_NE(mLinearPool, nullptr);

        mCompName = unknown_comp;
        struct StringToName {
            const char* Name;
            standardComp CompName;
        };

        const StringToName kStringToName[] = {
            {"h263", h263}, {"avc", avc}, {"mpeg2", mpeg2}, {"mpeg4", mpeg4},
            {"hevc", hevc}, {"vp8", vp8}, {"vp9", vp9},
        };

        const size_t kNumStringToName =
            sizeof(kStringToName) / sizeof(kStringToName[0]);

        std::string substring;
        std::string comp;
        substring = std::string(gEnv->getComponent());
        /* TODO: better approach to find the component */
        /* "c2.android." => 11th position */
        size_t pos = 11;
        size_t len = substring.find(".decoder", pos);
        comp = substring.substr(pos, len - pos);

        for (size_t i = 0; i < kNumStringToName; ++i) {
            if (!strcasecmp(comp.c_str(), kStringToName[i].Name)) {
                mCompName = kStringToName[i].CompName;
                break;
            }
        }
        mEos = false;
        mFramesReceived = 0;
        if (mCompName == unknown_comp) mDisableTest = true;
        if (mDisableTest) std::cout << "[   WARN   ] Test Disabled \n";
    }

    virtual void TearDown() override {
        if (mComponent != nullptr) {
            if (::testing::Test::HasFatalFailure()) return;
            mComponent->release();
            mComponent = nullptr;
        }
        Super::TearDown();
    }

    // callback function to process onWorkDone received by Listener
    void handleWorkDone(std::list<std::unique_ptr<C2Work>>& workItems) {
        for (std::unique_ptr<C2Work>& work : workItems) {
            // handle configuration changes in work done
            if (!work->worklets.empty() &&
                (work->worklets.front()->output.configUpdate.size() != 0)) {
                ALOGV("Config Update");
                std::vector<std::unique_ptr<C2Param>> updates =
                    std::move(work->worklets.front()->output.configUpdate);
                std::vector<C2Param*> configParam;
                std::vector<std::unique_ptr<C2SettingResult>> failures;
                for (size_t i = 0; i < updates.size(); ++i) {
                    C2Param* param = updates[i].get();
                    if (param->index() ==
                        C2VideoSizeStreamInfo::output::PARAM_TYPE) {
                        ALOGV("Received C2VideoSizeStreamInfo");
                        configParam.push_back(param);
                    }
                }
                mComponent->config(configParam, C2_DONT_BLOCK, &failures);
                ASSERT_EQ(failures.size(), 0u);
            }
            mFramesReceived++;
            mEos = (work->worklets.front()->output.flags &
                    C2FrameData::FLAG_END_OF_STREAM) != 0;
            work->input.buffers.clear();
            work->worklets.clear();
            typedef std::unique_lock<std::mutex> ULock;
            ULock l(mQueueLock);
            mWorkQueue.push_back(std::move(work));
            mQueueCondition.notify_all();
        }
    }

    enum standardComp {
        h263,
        avc,
        mpeg2,
        mpeg4,
        hevc,
        vp8,
        vp9,
        unknown_comp,
    };

    bool mEos;
    bool mDisableTest;
    standardComp mCompName;
    uint32_t mFramesReceived;
    C2BlockPool::local_id_t mBlockPoolId;
    std::shared_ptr<C2BlockPool> mLinearPool;
    std::shared_ptr<C2Allocator> mLinearAllocator;

    std::mutex mQueueLock;
    std::condition_variable mQueueCondition;
    std::list<std::unique_ptr<C2Work>> mWorkQueue;

    std::shared_ptr<android::Codec2Client> mClient;
    std::shared_ptr<android::Codec2Client::Listener> mListener;
    std::shared_ptr<android::Codec2Client::Component> mComponent;

   protected:
    static void description(const std::string& description) {
        RecordProperty("description", description);
    }
};

void validateComponent(
    const std::shared_ptr<android::Codec2Client::Component>& component,
    Codec2VideoDecHidlTest::standardComp compName, bool& disableTest) {
    // Validate its a C2 Component
    if (component->getName().find("c2") == std::string::npos) {
        ALOGE("Not a c2 component");
        disableTest = true;
        return;
    }

    // Validate its not an encoder and the component to be tested is video
    if (component->getName().find("encoder") != std::string::npos) {
        ALOGE("Expected Decoder, given Encoder");
        disableTest = true;
        return;
    }
    std::vector<std::unique_ptr<C2Param>> queried;
    c2_status_t c2err =
        component->query({}, {C2PortMediaTypeSetting::input::PARAM_TYPE},
                         C2_DONT_BLOCK, &queried);
    if (c2err != C2_OK && queried.size() == 0) {
        ALOGE("Query media type failed => %d", c2err);
    } else {
        std::string inputDomain =
            ((C2StreamMediaTypeSetting::input*)queried[0].get())->m.value;
        if (inputDomain.find("video/") == std::string::npos) {
            ALOGE("Expected Video Component");
            disableTest = true;
            return;
        }
    }

    // Validates component name
    if (compName == Codec2VideoDecHidlTest::unknown_comp) {
        ALOGE("Component InValid");
        disableTest = true;
        return;
    }
    ALOGV("Component Valid");
}

// number of elementary streams per component
#define STREAM_COUNT 2
// LookUpTable of clips and metadata for component testing
void GetURLForComponent(Codec2VideoDecHidlTest::standardComp comp, char* mURL,
                        char* info, size_t streamIndex = 1) {
    struct CompToURL {
        Codec2VideoDecHidlTest::standardComp comp;
        const char mURL[STREAM_COUNT][512];
        const char info[STREAM_COUNT][512];
    };
    ASSERT_TRUE(streamIndex < STREAM_COUNT);

    static const CompToURL kCompToURL[] = {
        {Codec2VideoDecHidlTest::standardComp::avc,
         {"bbb_avc_176x144_300kbps_60fps.h264",
          "bbb_avc_640x360_768kbps_30fps.h264"},
         {"bbb_avc_176x144_300kbps_60fps.info",
          "bbb_avc_640x360_768kbps_30fps.info"}},
        {Codec2VideoDecHidlTest::standardComp::hevc,
         {"bbb_hevc_176x144_176kbps_60fps.hevc",
          "bbb_hevc_640x360_1600kbps_30fps.hevc"},
         {"bbb_hevc_176x144_176kbps_60fps.info",
          "bbb_hevc_640x360_1600kbps_30fps.info"}},
        {Codec2VideoDecHidlTest::standardComp::mpeg2,
         {"bbb_mpeg2_176x144_105kbps_25fps.m2v",
          "bbb_mpeg2_352x288_1mbps_60fps.m2v"},
         {"bbb_mpeg2_176x144_105kbps_25fps.info",
          "bbb_mpeg2_352x288_1mbps_60fps.info"}},
        {Codec2VideoDecHidlTest::standardComp::h263,
         {"", "bbb_h263_352x288_300kbps_12fps.h263"},
         {"", "bbb_h263_352x288_300kbps_12fps.info"}},
        {Codec2VideoDecHidlTest::standardComp::mpeg4,
         {"", "bbb_mpeg4_352x288_512kbps_30fps.m4v"},
         {"", "bbb_mpeg4_352x288_512kbps_30fps.info"}},
        {Codec2VideoDecHidlTest::standardComp::vp8,
         {"bbb_vp8_176x144_240kbps_60fps.vp8",
          "bbb_vp8_640x360_2mbps_30fps.vp8"},
         {"bbb_vp8_176x144_240kbps_60fps.info",
          "bbb_vp8_640x360_2mbps_30fps.info"}},
        {Codec2VideoDecHidlTest::standardComp::vp9,
         {"bbb_vp9_176x144_285kbps_60fps.vp9",
          "bbb_vp9_640x360_1600kbps_30fps.vp9"},
         {"bbb_vp9_176x144_285kbps_60fps.info",
          "bbb_vp9_640x360_1600kbps_30fps.info"}},
    };

    for (size_t i = 0; i < sizeof(kCompToURL) / sizeof(kCompToURL[0]); ++i) {
        if (kCompToURL[i].comp == comp) {
            strcat(mURL, kCompToURL[i].mURL[streamIndex]);
            strcat(info, kCompToURL[i].info[streamIndex]);
            return;
        }
    }
}

void decodeNFrames(const std::shared_ptr<android::Codec2Client::Component> &component,
                   std::mutex &queueLock, std::condition_variable &queueCondition,
                   std::list<std::unique_ptr<C2Work>> &workQueue,
                   std::shared_ptr<C2Allocator> &linearAllocator,
                   std::shared_ptr<C2BlockPool> &linearPool,
                   C2BlockPool::local_id_t blockPoolId,
                   std::ifstream& eleStream,
                   android::Vector<FrameInfo>* Info,
                   int offset, int range, bool signalEOS = true) {
    typedef std::unique_lock<std::mutex> ULock;
    int frameID = 0;
    linearPool =
        std::make_shared<C2PooledBlockPool>(linearAllocator, blockPoolId++);
    component->start();
    int maxRetry = 0;
    while (1) {
        if (frameID == (int)Info->size() || frameID == (offset + range)) break;
        uint32_t flags = 0;
        std::unique_ptr<C2Work> work;
        // Prepare C2Work
        while (!work && (maxRetry < MAX_RETRY)) {
            ULock l(queueLock);
            if (!workQueue.empty()) {
                work.swap(workQueue.front());
                workQueue.pop_front();
            } else {
                queueCondition.wait_for(l, TIME_OUT);
                maxRetry++;
            }
        }
        if (!work && (maxRetry >= MAX_RETRY)) {
            ASSERT_TRUE(false) << "Wait for generating C2Work exceeded timeout";
        }
        int64_t timestamp = (*Info)[frameID].timestamp;
        if ((*Info)[frameID].flags) flags = (1 << ((*Info)[frameID].flags - 1));
        if (signalEOS && ((frameID == (int)Info->size() - 1) ||
                          (frameID == (offset + range - 1))))
            flags |= C2FrameData::FLAG_END_OF_STREAM;

        work->input.flags = (C2FrameData::flags_t)flags;
        work->input.ordinal.timestamp = timestamp;
        work->input.ordinal.frameIndex = frameID;

        int size = (*Info)[frameID].bytesCount;
        char* data = (char*)malloc(size);

        eleStream.read(data, size);
        ASSERT_EQ(eleStream.gcount(), size);

        std::shared_ptr<C2LinearBlock> block;
        ASSERT_EQ(C2_OK,
                  linearPool->fetchLinearBlock(
                      size, {C2MemoryUsage::CPU_READ, C2MemoryUsage::CPU_WRITE},
                      &block));
        ASSERT_TRUE(block);

        // Write View
        C2WriteView view = block->map().get();
        if (view.error() != C2_OK) {
            fprintf(stderr, "C2LinearBlock::map() failed : %d", view.error());
            break;
        }
        ASSERT_EQ((size_t)size, view.capacity());
        ASSERT_EQ(0u, view.offset());
        ASSERT_EQ((size_t)size, view.size());

        memcpy(view.base(), data, size);

        work->input.buffers.clear();
        work->input.buffers.emplace_back(new LinearBuffer(block));
        work->worklets.clear();
        work->worklets.emplace_back(new C2Worklet);
        free(data);

        std::list<std::unique_ptr<C2Work>> items;
        items.push_back(std::move(work));

        // DO THE DECODING
        ASSERT_EQ(component->queue(&items), C2_OK);
        ALOGV("Frame #%d size = %d queued", frameID, size);
        frameID++;
        maxRetry = 0;
    }
}

void waitOnInputConsumption(std::mutex &queueLock,
                            std::condition_variable &queueCondition,
                            std::list<std::unique_ptr<C2Work>> &workQueue) {
    typedef std::unique_lock<std::mutex> ULock;
    uint32_t queueSize;
    int maxRetry = 0;
    {
        ULock l(queueLock);
        queueSize = workQueue.size();
    }
    while ((maxRetry < MAX_RETRY) && (queueSize < MAX_INPUT_BUFFERS)) {
        ULock l(queueLock);
        if (queueSize != workQueue.size()) {
            queueSize = workQueue.size();
            maxRetry = 0;
        } else {
            queueCondition.wait_for(l, TIME_OUT);
            maxRetry++;
        }
    }
}

TEST_F(Codec2VideoDecHidlTest, validateCompName) {
    if (mDisableTest) return;
    ALOGV("Checks if the given component is a valid video component");
    validateComponent(mComponent, mCompName, mDisableTest);
    ASSERT_EQ(mDisableTest, false);
}

// Bitstream Test
TEST_F(Codec2VideoDecHidlTest, DecodeTest) {
    description("Decodes input file");
    if (mDisableTest) return;

    char mURL[512], info[512];
    std::ifstream eleStream, eleInfo;

    strcpy(mURL, gEnv->getRes().c_str());
    strcpy(info, gEnv->getRes().c_str());
    GetURLForComponent(mCompName, mURL, info);

    eleInfo.open(info);
    ASSERT_EQ(eleInfo.is_open(), true) << mURL << " - file not found";
    android::Vector<FrameInfo> Info;
    int bytesCount = 0;
    uint32_t flags = 0;
    uint32_t timestamp = 0;
    while (1) {
        if (!(eleInfo >> bytesCount)) break;
        eleInfo >> flags;
        eleInfo >> timestamp;
        Info.push_back({bytesCount, flags, timestamp});
    }
    eleInfo.close();

    ALOGV("mURL : %s", mURL);
    eleStream.open(mURL, std::ifstream::binary);
    ASSERT_EQ(eleStream.is_open(), true);
    ASSERT_NO_FATAL_FAILURE(decodeNFrames(
        mComponent, mQueueLock, mQueueCondition, mWorkQueue, mLinearAllocator,
        mLinearPool, mBlockPoolId, eleStream, &Info, 0, (int)Info.size()));

    // blocking call to ensures application to Wait till all the inputs are
    // consumed
    if (!mEos) {
        ALOGV("Waiting for input consumption");
        ASSERT_NO_FATAL_FAILURE(
            waitOnInputConsumption(mQueueLock, mQueueCondition, mWorkQueue));
    }

    eleStream.close();
    if (mFramesReceived != Info.size()) {
        ALOGE("Input buffer count and Output buffer count mismatch");
        ALOGV("framesReceived : %d inputFrames : %zu", mFramesReceived,
              Info.size());
        ASSERT_TRUE(false);
    }
}

}  // anonymous namespace

// TODO : Video specific configuration Test
// TODO : Thumbnail Test
// TODO : Test EOS
// TODO : Flush Test
// TODO : Check timestamps deviation
// TODO : Adaptive Test
int main(int argc, char** argv) {
    gEnv = new ComponentTestEnvironment();
    ::testing::AddGlobalTestEnvironment(gEnv);
    ::testing::InitGoogleTest(&argc, argv);
    gEnv->init(&argc, argv);
    int status = gEnv->initFromOptions(argc, argv);
    if (status == 0) {
        int status = RUN_ALL_TESTS();
        LOG(INFO) << "C2 Test result = " << status;
    }
    return status;
}