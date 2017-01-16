//
// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE.md file in the project root for full license information.
//

#include "CNTKLibrary.h"
#include "Common.h"

using namespace CNTK;

namespace Microsoft { namespace MSR { namespace CNTK { namespace Test {

// Mock communicator to simulate MPI run
class MockCommunicator : public DistributedCommunicator
{
private:
    std::unordered_set<DistributedWorkerDescriptor> m_workers;
    DistributedWorkerDescriptor m_self;

public:
    virtual const std::unordered_set<DistributedWorkerDescriptor>& Workers() const override
    {
        return m_workers;
    }

    virtual const DistributedWorkerDescriptor& CurrentWorker() const override
    {
        return m_self;
    }

    virtual DistributedCommunicatorPtr SubGroup(const std::unordered_set<DistributedWorkerDescriptor>&) const override
    {
        return nullptr;
    }

    virtual void Concatenate(
        const std::vector<ValuePtr>&,
        std::vector<ValuePtr>&,
        const std::unordered_set<DistributedWorkerDescriptor>&) override
    {}

    virtual void Concatenate(
        const std::vector<NDArrayViewPtr>&,
        std::vector<NDArrayViewPtr>&,
        const std::unordered_set<DistributedWorkerDescriptor>&) override
    {}

    virtual void Gather(
        const Dictionary&,
        std::vector<DictionaryPtr>&,
        const std::unordered_set<DistributedWorkerDescriptor>&) override
    {}

    virtual void AggregateInPlace(
        const std::vector<NDArrayViewPtr>&,
        const std::unordered_set<DistributedWorkerDescriptor>&) override
    {}

    virtual void Aggregate(
        const std::vector<NDArrayViewPtr>&,
        std::vector<NDArrayViewPtr>&,
        const std::unordered_set<DistributedWorkerDescriptor>&) override
    {}
    
    virtual void Barrier() override
    {}

    MockCommunicator(size_t numWorkers)
    {
        for (size_t i = 0; i < numWorkers; i++)
        {
            DistributedWorkerDescriptor desc;
            desc.m_hostId = L"MockCommunicator";
            desc.m_globalRank = i;

            m_workers.insert(desc);
        }
        MockRank(0);
    }

    void MockRank(size_t rank)
    {
        m_self.m_hostId = L"MockCommunicator";
        m_self.m_globalRank = rank;
    }
};

MinibatchSourcePtr TextFormatMinibatchSource(const std::wstring& dataFilePath, const std::vector<StreamConfiguration>& streamConfigs, size_t epochSize, bool randomize, size_t distributedAfterSampleCount, size_t numWorkers, size_t workerRank, size_t chunkSizeInBytes)
{
    ::CNTK::Dictionary minibatchSourceConfiguration;
    minibatchSourceConfiguration[L"epochSize"] = epochSize;

    if (randomize)
        minibatchSourceConfiguration[L"randomize"] = true;
    else
        minibatchSourceConfiguration[L"randomize"] = false;

    ::CNTK::Dictionary deserializerConfiguration;
    deserializerConfiguration[L"type"] = L"CNTKTextFormatDeserializer";
    deserializerConfiguration[L"file"] = dataFilePath;

    ::CNTK::Dictionary inputStreamsConfig;
    for (auto streamConfig : streamConfigs)
    {
        std::wstring streamName = streamConfig.m_streamName;
        size_t streamDim = streamConfig.m_dim;
        bool isSparse = streamConfig.m_isSparse;
        std::wstring streamAlias = streamConfig.m_streamAlias;

        ::CNTK::Dictionary inputStreamConfig;
        inputStreamConfig[L"dim"] = streamDim;
        inputStreamConfig[L"format"] = isSparse ? L"sparse" : L"dense";
        if (!streamAlias.empty())
            inputStreamConfig[L"alias"] = streamAlias;

        inputStreamsConfig[streamName] = inputStreamConfig;
    }

    deserializerConfiguration[L"input"] = inputStreamsConfig;
    deserializerConfiguration[L"chunkSizeInBytes"] = chunkSizeInBytes;
    minibatchSourceConfiguration[L"deserializers"] = std::vector<::CNTK::DictionaryValue>({ deserializerConfiguration });
    minibatchSourceConfiguration[L"distributedAfterSampleCount"] = distributedAfterSampleCount;
    minibatchSourceConfiguration[L"numWorkers"] = numWorkers;
    minibatchSourceConfiguration[L"workerRank"] = workerRank;
    return CreateCompositeMinibatchSource(minibatchSourceConfiguration);
}

void TestMinibatchSourceWarmStart(size_t numMBs, size_t minibatchSize, size_t warmStartSamples, bool randomize, size_t chunkSizeInBytes, bool expectNoData = false)
{
    const size_t inputDim = 2;
    const size_t numOutputClasses = 2;
    auto featureStreamName = L"features";
    auto labelsStreamName = L"labels";
    const size_t numWorkers = 2;

    auto minibatchSource = TextFormatMinibatchSource(
        L"SimpleDataTrain_cntk_text.txt",
        { { featureStreamName, inputDim }, { labelsStreamName, numOutputClasses } },
        MinibatchSource::InfinitelyRepeat,
        randomize,
        warmStartSamples,
        numWorkers,
        0,
        chunkSizeInBytes);

    auto minibatchSource2 = TextFormatMinibatchSource(
        L"SimpleDataTrain_cntk_text.txt",
        { { featureStreamName, inputDim }, { labelsStreamName, numOutputClasses } },
        MinibatchSource::InfinitelyRepeat,
        randomize,
        warmStartSamples,
        numWorkers,
        1,
        chunkSizeInBytes);

    auto featureStreamInfo = minibatchSource->StreamInfo(featureStreamName);
    auto labelStreamInfo = minibatchSource->StreamInfo(labelsStreamName);

    auto featureStreamInfo2 = minibatchSource2->StreamInfo(featureStreamName);
    auto labelStreamInfo2 = minibatchSource2->StreamInfo(labelsStreamName);

    size_t totalSamples = 0;
    for (size_t i = 0; i < numMBs; ++i)
    {
        bool distributed = minibatchSource->IsDistributed();
        bool distributed2 = minibatchSource2->IsDistributed();
        if (distributed != (totalSamples >= warmStartSamples) || distributed != distributed2)
        {
            ReportFailure("TestMinibatchSourceWarmStart failed in distributed state: expected %d, actual %d",
                totalSamples >= warmStartSamples, distributed);
        }

        auto minibatchData = minibatchSource->GetNextMinibatch(minibatchSize);
        auto minibatchData2 = minibatchSource2->GetNextMinibatch(minibatchSize);

        size_t expectedNumSamples = minibatchSize;
        size_t numSamples = minibatchData[featureStreamInfo].m_numSamples;

        if (expectNoData && distributed2)
        {
            if (numSamples != expectedNumSamples/2 || !minibatchData2.empty())
                ReportFailure("TestMinibatchSourceWarmStart failed in sample count: expected %lu, distributed %d (0:%lu)", expectedNumSamples, distributed, numSamples);
        }
        else
        {
            size_t numSamples2 = minibatchData2[featureStreamInfo].m_numSamples;
            if (numSamples != numSamples2)
                ReportFailure("TestMinibatchSourceWarmStart failed in sample count: expected %lu, distributed %d (0:%lu, 1:%lu)", expectedNumSamples, distributed, numSamples, numSamples2);
        }

        totalSamples += expectedNumSamples;
    }
}

BOOST_AUTO_TEST_SUITE(MinibatchSourceSuite)

BOOST_AUTO_TEST_CASE(NoRandomizedMinibatchSourceWarmStart)
{
    TestMinibatchSourceWarmStart(10, 64, 128, false, 1024);
    TestMinibatchSourceWarmStart(10, 64, 0, false, 1024);
    TestMinibatchSourceWarmStart(10, 64, 100, false, 1024);
}

BOOST_AUTO_TEST_CASE(NoRandomizedMinibatchSourceWithSingleChunk)
{
    size_t chunk32MB = 1024 * 1024 * 32;
    TestMinibatchSourceWarmStart(10, 64, 128, false, chunk32MB);
    TestMinibatchSourceWarmStart(10, 64, 0, false, chunk32MB);
    TestMinibatchSourceWarmStart(10, 64, 100, false, chunk32MB);
}

BOOST_AUTO_TEST_CASE(RandomizedMinibatchSourceWithSmallChunks)
{
    TestMinibatchSourceWarmStart(10, 64, 0, true, 1024);
    TestMinibatchSourceWarmStart(10, 64, 128, true, 1024);
}

BOOST_AUTO_TEST_CASE(RandomizedMinibatchSourceWithNoData)
{
    size_t chunk32MB = 1024 * 1024 * 32;
    bool expectNoData = true;
    TestMinibatchSourceWarmStart(10, 64, 0, true, chunk32MB, expectNoData);
    TestMinibatchSourceWarmStart(10, 64, 128, true, chunk32MB, expectNoData);
}

BOOST_AUTO_TEST_SUITE_END()

}}}}