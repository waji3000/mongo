/**
 *    Copyright (C) 2020-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>
#include <fmt/format.h>
// IWYU pragma: no_include "cxxabi.h"
#include <cstdint>
#include <ostream>
#include <string>
#include <system_error>
#include <vector>

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/timestamp.h"
#include "mongo/client/connection_string.h"
#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/client.h"
#include "mongo/db/collection_crud/collection_write_path.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/curop.h"
#include "mongo/db/database_name.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/op_observer/op_observer.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/client_cursor/cursor_id.h"
#include "mongo/db/query/client_cursor/kill_cursors_gen.h"
#include "mongo/db/query/getmore_command_gen.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/optime_with.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/storage_interface_impl.h"
#include "mongo/db/repl/wait_for_majority_service.h"
#include "mongo/db/s/metrics/sharding_data_transform_instance_metrics.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/s/resharding/resharding_metrics.h"
#include "mongo/db/s/resharding/resharding_oplog_fetcher.h"
#include "mongo/db/s/resharding/resharding_oplog_fetcher_progress_gen.h"
#include "mongo/db/s/resharding/resharding_util.h"
#include "mongo/db/s/shard_server_test_fixture.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session/logical_session_cache.h"
#include "mongo/db/session/logical_session_cache_noop.h"
#include "mongo/db/session/session_catalog_mongod.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/tenant_id.h"
#include "mongo/db/transaction_resources.h"
#include "mongo/executor/network_test_env.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/executor/thread_pool_task_executor_test_fixture.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/logv2/log.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/catalog/sharding_catalog_client_mock.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/sharding_task_executor.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/out_of_line_executor.h"
#include "mongo/util/str.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace {

repl::MutableOplogEntry makeOplog(const NamespaceString& nss,
                                  const UUID& uuid,
                                  const repl::OpTypeEnum& opType,
                                  const BSONObj& oField,
                                  const BSONObj& o2Field,
                                  const ReshardingDonorOplogId& oplogId) {
    repl::MutableOplogEntry oplogEntry;
    oplogEntry.setNss(nss);
    oplogEntry.setUuid(uuid);
    oplogEntry.setOpType(opType);
    oplogEntry.setObject(oField);

    if (!o2Field.isEmpty()) {
        oplogEntry.setObject2(o2Field);
    }

    oplogEntry.setOpTime({{}, {}});
    oplogEntry.setWallClockTime({});
    oplogEntry.set_id(Value(oplogId.toBSON()));

    return oplogEntry;
}

/**
 * RAII type for operating at a timestamp. Will remove any timestamping when the object destructs.
 */
class OneOffRead {
public:
    OneOffRead(OperationContext* opCtx, const Timestamp& ts, bool waitForOplog = false)
        : _opCtx(opCtx) {
        shard_role_details::getRecoveryUnit(_opCtx)->abandonSnapshot();
        if (waitForOplog) {
            auto storageEngine = opCtx->getServiceContext()->getStorageEngine();
            LocalOplogInfo* oplogInfo = LocalOplogInfo::get(opCtx);

            // Oplog should be available in this test.
            invariant(oplogInfo);
            storageEngine->waitForAllEarlierOplogWritesToBeVisible(opCtx,
                                                                   oplogInfo->getRecordStore());
        }
        if (ts.isNull()) {
            shard_role_details::getRecoveryUnit(_opCtx)->setTimestampReadSource(
                RecoveryUnit::ReadSource::kNoTimestamp);
        } else {
            shard_role_details::getRecoveryUnit(_opCtx)->setTimestampReadSource(
                RecoveryUnit::ReadSource::kProvided, ts);
        }
    }

    ~OneOffRead() {
        shard_role_details::getRecoveryUnit(_opCtx)->abandonSnapshot();
        shard_role_details::getRecoveryUnit(_opCtx)->setTimestampReadSource(
            RecoveryUnit::ReadSource::kNoTimestamp);
    }

private:
    OperationContext* _opCtx;
};

class ReshardingOplogFetcherTest : public ShardServerTestFixture {
public:
    void setUp() override {
        ShardServerTestFixture::setUp();
        _opCtx = operationContext();
        _svcCtx = _opCtx->getServiceContext();

        for (const auto& shardId : kTwoShardIdList) {
            auto shardTargeter = RemoteCommandTargeterMock::get(
                uassertStatusOK(shardRegistry()->getShard(operationContext(), shardId))
                    ->getTargeter());
            shardTargeter->setFindHostReturnValue(makeHostAndPort(shardId));
        }

        WaitForMajorityService::get(getServiceContext()).startup(getServiceContext());

        // onStepUp() relies on the storage interface to create the config.transactions table.
        repl::StorageInterface::set(getServiceContext(),
                                    std::make_unique<repl::StorageInterfaceImpl>());
        auto mongoDSessionCatalog = MongoDSessionCatalog::get(operationContext());
        mongoDSessionCatalog->onStepUp(operationContext());
        LogicalSessionCache::set(getServiceContext(), std::make_unique<LogicalSessionCacheNoop>());

        resetResharding();
        // In practice, the progress collection is created by ReshardingDataReplication before
        // creating the ReshardingOplogFetchers.
        create(NamespaceString::kReshardingFetcherProgressNamespace);
    }

    void tearDown() override {
        WaitForMajorityService::get(getServiceContext()).shutDown();
        ShardServerTestFixture::tearDown();
    }

    void resetResharding() {
        _reshardingUUID = UUID::gen();
        _metrics = ReshardingMetrics::makeInstance(_reshardingUUID,
                                                   kShardKey,
                                                   NamespaceString::kEmpty,
                                                   ReshardingMetrics::Role::kRecipient,
                                                   getServiceContext()->getFastClockSource()->now(),
                                                   getServiceContext());
        _fetchTimestamp = queryOplog(BSONObj())["ts"].timestamp();
        _donorShard = kTwoShardIdList[0];
        _destinationShard = kTwoShardIdList[1];
    }

    auto makeFetcherEnv() {
        return std::make_unique<ReshardingOplogFetcher::Env>(_svcCtx, _metrics.get());
    }

    auto makeExecutor() {
        ThreadPool::Options threadPoolOpts;
        threadPoolOpts.maxThreads = 100;
        threadPoolOpts.threadNamePrefix = "ReshardingOplogFetcherTest-";
        threadPoolOpts.poolName = "ReshardingOplogFetcherTestThreadPool";
        return executor::ThreadPoolTaskExecutor::create(
            std::make_unique<ThreadPool>(threadPoolOpts),
            std::make_unique<executor::NetworkInterfaceMock>());
    }

    /**
     * Override the CatalogClient to make CatalogClient::getAllShards automatically return the
     * expected shards. We cannot mock the network responses for the ShardRegistry reload, since the
     * ShardRegistry reload is done over DBClient, not the NetworkInterface, and there is no
     * DBClientMock analogous to the NetworkInterfaceMock.
     */
    std::unique_ptr<ShardingCatalogClient> makeShardingCatalogClient() override {

        class StaticCatalogClient final : public ShardingCatalogClientMock {
        public:
            StaticCatalogClient(std::vector<ShardId> shardIds) : _shardIds(std::move(shardIds)) {}

            StatusWith<repl::OpTimeWith<std::vector<ShardType>>> getAllShards(
                OperationContext* opCtx,
                repl::ReadConcernLevel readConcern,
                bool excludeDraining) override {
                std::vector<ShardType> shardTypes;
                for (const auto& shardId : _shardIds) {
                    const ConnectionString cs = ConnectionString::forReplicaSet(
                        shardId.toString(), {makeHostAndPort(shardId)});
                    ShardType sType;
                    sType.setName(cs.getSetName());
                    sType.setHost(cs.toString());
                    shardTypes.push_back(std::move(sType));
                };
                return repl::OpTimeWith<std::vector<ShardType>>(shardTypes);
            }

        private:
            const std::vector<ShardId> _shardIds;
        };

        return std::make_unique<StaticCatalogClient>(kTwoShardIdList);
    }

    void insertDocument(const CollectionPtr& coll, const InsertStatement& stmt) {
        // Insert some documents.
        OpDebug* const nullOpDebug = nullptr;
        const bool fromMigrate = false;
        ASSERT_OK(
            collection_internal::insertDocument(_opCtx, coll, stmt, nullOpDebug, fromMigrate));
    }

    BSONObj queryCollection(NamespaceString nss, const BSONObj& query) {
        BSONObj ret;
        const auto coll = acquireCollection(
            _opCtx,
            CollectionAcquisitionRequest(nss,
                                         PlacementConcern(boost::none, ShardVersion::UNSHARDED()),
                                         repl::ReadConcernArgs::get(_opCtx),
                                         AcquisitionPrerequisites::kRead),
            MODE_IS);
        ASSERT_TRUE(Helpers::findOne(_opCtx, coll.getCollectionPtr(), query, ret))
            << "Query: " << query;
        return ret;
    }

    BSONObj getLast(NamespaceString nss) {
        BSONObj ret;
        Helpers::getLast(_opCtx, nss, ret);
        return ret;
    }

    BSONObj queryOplog(const BSONObj& query) {
        OneOffRead oor(_opCtx, Timestamp::min(), true);
        return queryCollection(NamespaceString::kRsOplogNamespace, query);
    }

    repl::OpTime getLastApplied() {
        return repl::ReplicationCoordinator::get(_opCtx)->getMyLastAppliedOpTime();
    }

    boost::intrusive_ptr<ExpressionContextForTest> createExpressionContext() {
        boost::intrusive_ptr<ExpressionContextForTest> expCtx(
            new ExpressionContextForTest(_opCtx, NamespaceString::kRsOplogNamespace));
        expCtx->setResolvedNamespace(NamespaceString::kRsOplogNamespace,
                                     {NamespaceString::kRsOplogNamespace, {}});
        return expCtx;
    }

    int itcount(NamespaceString nss, BSONObj filter = BSONObj()) {
        OneOffRead oof(_opCtx, Timestamp::min(), nss.isOplog());

        DBDirectClient client(_opCtx);
        FindCommandRequest findRequest{nss};
        findRequest.setFilter(filter);
        auto cursor = client.find(std::move(findRequest));
        int ret = 0;
        while (cursor->more()) {
            cursor->next();
            ++ret;
        }

        return ret;
    }

    void create(NamespaceString nss) {
        writeConflictRetry(_opCtx, "create", nss, [&] {
            AllowLockAcquisitionOnTimestampedUnitOfWork allowLockAcquisition(
                shard_role_details::getLocker(_opCtx));
            AutoGetDb autoDb(_opCtx, nss.dbName(), LockMode::MODE_X);
            WriteUnitOfWork wunit(_opCtx);
            if (shard_role_details::getRecoveryUnit(_opCtx)->getCommitTimestamp().isNull()) {
                ASSERT_OK(
                    shard_role_details::getRecoveryUnit(_opCtx)->setTimestamp(Timestamp(1, 1)));
            }

            OperationShardingState::ScopedAllowImplicitCollectionCreate_UNSAFE
                unsafeCreateCollection(_opCtx);
            auto db = autoDb.ensureDbExists(_opCtx);
            ASSERT(db->createCollection(_opCtx, nss)) << nss.toStringForErrorMsg();
            wunit.commit();
        });
    }

    BSONObj makeMockAggregateResponse(Timestamp postBatchResumeToken,
                                      BSONArray oplogEntries,
                                      CursorId cursorId = 0) {
        return BSON("cursor" << BSON("firstBatch"
                                     << oplogEntries << "postBatchResumeToken"
                                     << BSON("ts" << postBatchResumeToken) << "id" << cursorId
                                     << "ns"
                                     << NamespaceString::kRsOplogNamespace.toString_forTest()));
    };

    BSONObj makeFinalNoopOplogEntry(const NamespaceString& nss,
                                    const UUID& collectionUUID,
                                    Timestamp postBatchResumeToken) {
        return makeOplog(nss,
                         collectionUUID,
                         repl::OpTypeEnum::kNoop,
                         BSONObj(),
                         BSON("type" << resharding::kReshardFinalOpLogType << "reshardingUUID"
                                     << _reshardingUUID),
                         ReshardingDonorOplogId(postBatchResumeToken, postBatchResumeToken))
            .toBSON();
    }

    template <typename T>
    T requestPassthroughHandler(executor::NetworkTestEnv::FutureHandle<T>& future,
                                int maxBatches = -1,
                                boost::optional<BSONObj> mockResponse = boost::none) {

        int maxNumRequests = 1000;  // No unittests would request more than this?
        if (maxBatches > -1) {
            // The fetcher will send a `killCursors` after the last `getMore`.
            maxNumRequests = maxBatches + 1;
        }

        bool hasMore = true;
        for (int batchNum = 0; hasMore && batchNum < maxNumRequests; ++batchNum) {
            onCommand([&](const executor::RemoteCommandRequest& request) -> StatusWith<BSONObj> {
                if (mockResponse) {
                    hasMore = false;
                    return StatusWith<BSONObj>(mockResponse.get());
                } else {
                    DBDirectClient client(cc().getOperationContext());
                    BSONObj result;
                    bool res = client.runCommand(request.dbname, request.cmdObj, result);
                    if (res == false || result.hasField("cursorsKilled") ||
                        result["cursor"]["id"].Long() == 0) {
                        hasMore = false;
                    }

                    return result;
                }
            });
        }

        return future.timed_get(Seconds(5));
    }

    // Generates the following oplog entries with `destinedRecipient` field attached:
    // - `numInsertOplogEntriesBeforeFinalOplogEntry` insert oplog entries.
    // - one no-op oplog entry indicating that fetching is complete and resharding should move to
    //   the next stage.
    // - `numNoopOplogEntriesAfterFinalOplogEntry` no-op oplog entries. The fetcher should discard
    //   all of these oplog entries.
    void setupBasic(NamespaceString outputCollectionNss,
                    NamespaceString dataCollectionNss,
                    ShardId destinedRecipient,
                    int numInsertOplogEntriesBeforeFinalOplogEntry = 5,
                    int approxInsertOplogEntrySizeBytes = 1,
                    int numNoopOplogEntriesAfterFinalOplogEntry = 0) {
        create(outputCollectionNss);
        create(dataCollectionNss);
        _fetchTimestamp = repl::StorageInterface::get(_svcCtx)->getLatestOplogTimestamp(_opCtx);

        {
            AutoGetCollection dataColl(_opCtx, dataCollectionNss, LockMode::MODE_IX);

            // Set a failpoint to tack a `destinedRecipient` onto oplog entries.
            setGlobalFailPoint("addDestinedRecipient",
                               BSON("mode"
                                    << "alwaysOn"
                                    << "data"
                                    << BSON("destinedRecipient" << destinedRecipient.toString())));

            // Generate insert oplog entries by inserting documents.
            {
                for (std::int32_t num = 0; num < numInsertOplogEntriesBeforeFinalOplogEntry;
                     ++num) {
                    WriteUnitOfWork wuow(_opCtx);
                    insertDocument(
                        dataColl.getCollection(),
                        InsertStatement(
                            BSON("_id" << num << std::string(approxInsertOplogEntrySizeBytes, 'a')
                                       << num)));
                    wuow.commit();
                }
            }

            // Generate an noop entry indicating that fetching is complete.
            {
                WriteUnitOfWork wuow(_opCtx);
                _opCtx->getServiceContext()->getOpObserver()->onInternalOpMessage(
                    _opCtx,
                    dataColl.getCollection()->ns(),
                    dataColl.getCollection()->uuid(),
                    BSON(
                        "msg" << fmt::format("Writes to {} are temporarily blocked for resharding.",
                                             dataColl.getCollection()->ns().toString_forTest())),
                    BSON("type" << resharding::kReshardFinalOpLogType << "reshardingUUID"
                                << _reshardingUUID),
                    boost::none,
                    boost::none,
                    boost::none,
                    boost::none);
                wuow.commit();
            }

            // Generate noop oplog entries.
            {
                for (std::int32_t num = 0; num < numNoopOplogEntriesAfterFinalOplogEntry; ++num) {
                    WriteUnitOfWork wuow(_opCtx);
                    _opCtx->getServiceContext()->getOpObserver()->onInternalOpMessage(
                        _opCtx,
                        dataColl.getCollection()->ns(),
                        dataColl.getCollection()->uuid(),
                        BSON("msg" << "other noop"),
                        boost::none /* o2 */,
                        boost::none,
                        boost::none,
                        boost::none,
                        boost::none);
                    wuow.commit();
                }
            }
        }

        repl::StorageInterface::get(_opCtx)->waitForAllEarlierOplogWritesToBeVisible(_opCtx);

        // Disable the failpoint.
        setGlobalFailPoint("addDestinedRecipient", BSON("mode" << "off"));
    }

    void assertUsedApplyOpsToBatchInsert(NamespaceString nss, int numApplyOpsOplogEntries) {
        ASSERT_EQ(0,
                  itcount(NamespaceString::kRsOplogNamespace,
                          BSON("op" << "i"
                                    << "ns" << nss.ns_forTest())));
        ASSERT_EQ(numApplyOpsOplogEntries,
                  itcount(NamespaceString::kRsOplogNamespace,
                          BSON("o.applyOps.op" << "i"
                                               << "o.applyOps.ns" << nss.ns_forTest())));
    }

    long long currentOpFetchedCount() const {
        auto curOp = _metrics->reportForCurrentOp();
        return curOp["oplogEntriesFetched"_sd].Long();
    }

    long long persistedFetchedCount(OperationContext* opCtx) const {
        DBDirectClient client(opCtx);
        auto sourceId = ReshardingSourceId{_reshardingUUID, _donorShard};
        auto doc = client.findOne(
            NamespaceString::kReshardingFetcherProgressNamespace,
            BSON(ReshardingOplogApplierProgress::kOplogSourceIdFieldName << sourceId.toBSON()));
        if (doc.isEmpty()) {
            return 0;
        }
        return doc[ReshardingOplogFetcherProgress::kNumEntriesFetchedFieldName].Long();
    }

    CancelableOperationContextFactory makeCancelableOpCtx() {
        auto cancelableOpCtxExecutor = std::make_shared<ThreadPool>([] {
            ThreadPool::Options options;
            options.poolName = "TestReshardOplogFetcherCancelableOpCtxPool";
            options.minThreads = 1;
            options.maxThreads = 1;
            return options;
        }());

        return CancelableOperationContextFactory(operationContext()->getCancellationToken(),
                                                 cancelableOpCtxExecutor);
    }

protected:
    void testFetcherBasic(const NamespaceString& outputCollectionNss,
                          const NamespaceString& dataCollectionNss,
                          bool storeProgress,
                          boost::optional<int> initialAggregateBatchSize,
                          int expectedNumFetchedOplogEntries,
                          int expectedNumApplyOpsOplogEntries) {
        AutoGetCollection dataColl(_opCtx, dataCollectionNss, LockMode::MODE_IX);
        auto fetcherJob = launchAsync([&, this] {
            ThreadClient tc("RefetchRunner", _svcCtx->getService(), Client::noSession());
            ReshardingOplogFetcher fetcher(makeFetcherEnv(),
                                           _reshardingUUID,
                                           dataColl->uuid(),
                                           {_fetchTimestamp, _fetchTimestamp},
                                           _donorShard,
                                           _destinationShard,
                                           outputCollectionNss,
                                           storeProgress);
            fetcher.useReadConcernForTest(false);
            if (initialAggregateBatchSize) {
                fetcher.setInitialBatchSizeForTest(*initialAggregateBatchSize);
            }

            auto factory = makeCancelableOpCtx();
            fetcher.iterate(&cc(), factory);
        });

        requestPassthroughHandler(fetcherJob);

        ASSERT_EQ(expectedNumFetchedOplogEntries, itcount(outputCollectionNss));
        ASSERT_EQ(expectedNumFetchedOplogEntries, currentOpFetchedCount())
            << " Verify currentOp metrics";
        ASSERT_EQ(storeProgress ? expectedNumFetchedOplogEntries : 0, persistedFetchedCount(_opCtx))
            << " Verify persisted metrics";
        assertUsedApplyOpsToBatchInsert(outputCollectionNss, expectedNumApplyOpsOplogEntries);
    }

    void assertAggregateReadPreference(const executor::RemoteCommandRequest& request,
                                       const ReadPreferenceSetting& expectedReadPref) {
        auto parsedRequest = AggregateCommandRequest::parse(
            IDLParserContext("ReshardingOplogFetcherTest"),
            request.cmdObj.addFields(BSON("$db" << request.dbname.toString_forTest())));
        ASSERT_BSONOBJ_EQ(*parsedRequest.getUnwrappedReadPref(),
                          BSON("$readPreference" << expectedReadPref.toInnerBSON()));
    }

    void assertGetMoreCursorId(const executor::RemoteCommandRequest& request,
                               CursorId expectedCursorId) {
        auto parsedRequest = GetMoreCommandRequest::parse(
            IDLParserContext("ReshardingOplogFetcherTest"),
            request.cmdObj.addFields(BSON("$db" << request.dbname.toString_forTest())));
        ASSERT_EQ(parsedRequest.getCommandParameter(), expectedCursorId);
    }

    const std::vector<ShardId> kTwoShardIdList{{"s1"}, {"s2"}};
    const BSONObj kShardKey = BSON("skey" << 1);

    OperationContext* _opCtx;
    ServiceContext* _svcCtx;

    // To be reset per test case.
    UUID _reshardingUUID = UUID::gen();
    std::unique_ptr<ReshardingMetrics> _metrics;
    Timestamp _fetchTimestamp;
    ShardId _donorShard;
    ShardId _destinationShard;

private:
    // Set the sleep to 0 to speed up the tests.
    RAIIServerParameterControllerForTest _sleepMillisBeforeCriticalSection{
        "reshardingOplogFetcherSleepMillisBeforeCriticalSection", 0};
    RAIIServerParameterControllerForTest _sleepMillisDuringCriticalSection{
        "reshardingOplogFetcherSleepMillisDuringCriticalSection", 0};

    static HostAndPort makeHostAndPort(const ShardId& shardId) {
        return HostAndPort(str::stream() << shardId << ":123");
    }
};

TEST_F(ReshardingOplogFetcherTest, TestBasicSingleApplyOps) {
    for (bool storeProgress : {false, true}) {
        LOGV2(9480001, "Running case", "storeProgress"_attr = storeProgress);

        const NamespaceString outputCollectionNss = NamespaceString::createNamespaceString_forTest(
            "dbtests.outputCollection" + std::to_string(storeProgress));
        const NamespaceString dataCollectionNss = NamespaceString::createNamespaceString_forTest(
            "dbtests.runFetchIteration" + std::to_string(storeProgress));

        auto numInsertOplogEntries = 5;
        auto initialAggregateBatchSize = 2;
        // Add 1 to account for the sentinel final noop oplog entry.
        auto numFetchedOplogEntries = numInsertOplogEntries + 1;
        // The oplog entries come in 2 separate aggregate batches, each requires an applyOps oplog
        // entry.
        auto numApplyOpsOplogEntries = 2;

        setupBasic(
            outputCollectionNss, dataCollectionNss, _destinationShard, numInsertOplogEntries);
        testFetcherBasic(outputCollectionNss,
                         dataCollectionNss,
                         storeProgress,
                         initialAggregateBatchSize,
                         numFetchedOplogEntries,
                         numApplyOpsOplogEntries);

        resetResharding();
    }
}

TEST_F(ReshardingOplogFetcherTest, TestBasicMultipleApplyOps_BatchLimitOperations) {
    for (bool storeProgress : {false, true}) {
        LOGV2(9480002, "Running case", "storeProgress"_attr = storeProgress);

        const NamespaceString outputCollectionNss = NamespaceString::createNamespaceString_forTest(
            "dbtests.outputCollection" + std::to_string(storeProgress));
        const NamespaceString dataCollectionNss = NamespaceString::createNamespaceString_forTest(
            "dbtests.runFetchIteration" + std::to_string(storeProgress));

        auto batchLimitOperations = 5;
        RAIIServerParameterControllerForTest featureFlagController(
            "reshardingOplogFetcherInsertBatchLimitOperations", batchLimitOperations);
        auto numInsertOplogEntries = 8;
        auto initialAggregateBatchSize = boost::none;
        // Add 1 to account for the sentinel final noop oplog entry.
        auto numFetchedOplogEntries = numInsertOplogEntries + 1;
        // The oplog entries come in one aggregate batch. However, each applyOps oplog entry can
        // only have 'batchLimitOperations' oplog entries.
        auto numApplyOpsOplogEntries =
            std::ceil((double)numFetchedOplogEntries / batchLimitOperations);

        setupBasic(
            outputCollectionNss, dataCollectionNss, _destinationShard, numInsertOplogEntries);
        testFetcherBasic(outputCollectionNss,
                         dataCollectionNss,
                         storeProgress,
                         initialAggregateBatchSize,
                         numFetchedOplogEntries,
                         numApplyOpsOplogEntries);

        resetResharding();
    }
}

TEST_F(ReshardingOplogFetcherTest, TestBasicMultipleApplyOps_BatchLimitBytes) {
    for (bool storeProgress : {false, true}) {
        LOGV2(9480003, "Running case", "storeProgress"_attr = storeProgress);

        const NamespaceString outputCollectionNss = NamespaceString::createNamespaceString_forTest(
            "dbtests.outputCollection" + std::to_string(storeProgress));
        const NamespaceString dataCollectionNss = NamespaceString::createNamespaceString_forTest(
            "dbtests.runFetchIteration" + std::to_string(storeProgress));

        auto batchLimitBytes = 10 * 1024;
        RAIIServerParameterControllerForTest featureFlagController(
            "reshardingOplogFetcherInsertBatchLimitBytes", batchLimitBytes);
        auto numInsertOplogEntries = 8;
        auto approxInsertOplogEntrySizeBytes = 3 * 1024;
        auto initialAggregateBatchSize = boost::none;
        // Add 1 to account for the sentinel final noop oplog entry.
        auto numFetchedOplogEntries = numInsertOplogEntries + 1;
        // The oplog entries come in one aggregate batch. However, each applyOps oplog entry can
        // only have 'batchLimitBytes'.
        auto numApplyOpsOplogEntries = std::ceil((double)approxInsertOplogEntrySizeBytes *
                                                 numInsertOplogEntries / batchLimitBytes);

        setupBasic(outputCollectionNss,
                   dataCollectionNss,
                   _destinationShard,
                   numInsertOplogEntries,
                   approxInsertOplogEntrySizeBytes);
        testFetcherBasic(outputCollectionNss,
                         dataCollectionNss,
                         storeProgress,
                         initialAggregateBatchSize,
                         numFetchedOplogEntries,
                         numApplyOpsOplogEntries);

        resetResharding();
    }
}

TEST_F(ReshardingOplogFetcherTest,
       TestBasicMultipleApplyOps_SingleOplogEntrySizeExceedsBatchLimitBytes) {
    for (bool storeProgress : {false, true}) {
        LOGV2(9480004, "Running case", "storeProgress"_attr = storeProgress);

        const NamespaceString outputCollectionNss = NamespaceString::createNamespaceString_forTest(
            "dbtests.outputCollection" + std::to_string(storeProgress));
        const NamespaceString dataCollectionNss = NamespaceString::createNamespaceString_forTest(
            "dbtests.runFetchIteration" + std::to_string(storeProgress));

        auto batchLimitBytes = 1 * 1024;
        RAIIServerParameterControllerForTest featureFlagController(
            "reshardingOplogFetcherInsertBatchLimitBytes", batchLimitBytes);
        auto numInsertOplogEntries = 2;
        auto approxInsertOplogEntrySizeBytes = 3 * 1024;
        auto initialAggregateBatchSize = boost::none;
        // Add 1 to account for the sentinel final noop oplog entry.
        auto numFetchedOplogEntries = numInsertOplogEntries + 1;
        // The oplog entries come in one aggregate batch. However, the size of each insert oplog
        // entry exceeds the 'batchLimitBytes'. They should still get inserted successfully but each
        // should require a separate applyOps oplog entry.
        auto numApplyOpsOplogEntries = numFetchedOplogEntries;

        setupBasic(outputCollectionNss,
                   dataCollectionNss,
                   _destinationShard,
                   numInsertOplogEntries,
                   approxInsertOplogEntrySizeBytes);
        testFetcherBasic(outputCollectionNss,
                         dataCollectionNss,
                         storeProgress,
                         initialAggregateBatchSize,
                         numFetchedOplogEntries,
                         numApplyOpsOplogEntries);

        resetResharding();
    }
}

TEST_F(ReshardingOplogFetcherTest, TestBasicMultipleApplyOps_FinalOplogEntry) {
    for (bool storeProgress : {false, true}) {
        LOGV2(9678001, "Running case", "storeProgress"_attr = storeProgress);

        const NamespaceString outputCollectionNss = NamespaceString::createNamespaceString_forTest(
            "dbtests.outputCollection" + std::to_string(storeProgress));
        const NamespaceString dataCollectionNss = NamespaceString::createNamespaceString_forTest(
            "dbtests.runFetchIteration" + std::to_string(storeProgress));

        auto batchLimitOperations = 5;
        RAIIServerParameterControllerForTest featureFlagController(
            "reshardingOplogFetcherInsertBatchLimitOperations", batchLimitOperations);
        auto numInsertOplogEntriesBeforeFinal = 8;
        auto approxInsertOplogEntrySizeBytes = 1;
        auto numNoopOplogEntriesAfterFinal = 3;
        auto initialAggregateBatchSize = boost::none;
        // Add 1 to account for the sentinel final noop oplog entry. The oplog entries after the
        // final oplog entries should be discarded.
        auto numFetchedOplogEntries = numInsertOplogEntriesBeforeFinal + 1;
        // The oplog entries come in one aggregate batch. However, each applyOps oplog entry can
        // only have 'batchLimitOperations' oplog entries.
        auto numApplyOpsOplogEntries =
            std::ceil((double)numFetchedOplogEntries / batchLimitOperations);

        setupBasic(outputCollectionNss,
                   dataCollectionNss,
                   _destinationShard,
                   numInsertOplogEntriesBeforeFinal,
                   approxInsertOplogEntrySizeBytes,
                   numNoopOplogEntriesAfterFinal);
        testFetcherBasic(outputCollectionNss,
                         dataCollectionNss,
                         storeProgress,
                         initialAggregateBatchSize,
                         numFetchedOplogEntries,
                         numApplyOpsOplogEntries);

        resetResharding();
    }
}

TEST_F(ReshardingOplogFetcherTest, TestTrackLastSeen) {
    const NamespaceString outputCollectionNss =
        NamespaceString::createNamespaceString_forTest("dbtests.outputCollection");
    const NamespaceString dataCollectionNss =
        NamespaceString::createNamespaceString_forTest("dbtests.runFetchIteration");

    setupBasic(outputCollectionNss, dataCollectionNss, _destinationShard);

    AutoGetCollection dataColl(_opCtx, dataCollectionNss, LockMode::MODE_IX);

    const int maxBatches = 1;
    auto fetcherJob = launchAsync([&, this] {
        ThreadClient tc("RefetcherRunner", _svcCtx->getService(), Client::noSession());

        ReshardingOplogFetcher fetcher(makeFetcherEnv(),
                                       _reshardingUUID,
                                       dataColl->uuid(),
                                       {_fetchTimestamp, _fetchTimestamp},
                                       _donorShard,
                                       _destinationShard,
                                       outputCollectionNss,
                                       true /* storeProgress */);
        fetcher.useReadConcernForTest(false);
        fetcher.setInitialBatchSizeForTest(2);
        fetcher.setMaxBatchesForTest(maxBatches);

        auto factory = makeCancelableOpCtx();
        fetcher.iterate(&cc(), factory);
        return fetcher.getLastSeenTimestamp();
    });

    ReshardingDonorOplogId lastSeen = requestPassthroughHandler(fetcherJob, maxBatches);

    ASSERT_EQ(2, itcount(outputCollectionNss));
    ASSERT_EQ(2, currentOpFetchedCount()) << " Verify reported metrics";
    ASSERT_EQ(2, persistedFetchedCount(_opCtx)) << " Verify persisted metrics";
    // Assert the lastSeen value has been bumped from the original `_fetchTimestamp`.
    ASSERT_GT(lastSeen.getTs(), _fetchTimestamp);
    assertUsedApplyOpsToBatchInsert(outputCollectionNss, 1 /* numApplyOpsOplogEntries */);
}

TEST_F(ReshardingOplogFetcherTest, TestFallingOffOplog) {
    const NamespaceString outputCollectionNss =
        NamespaceString::createNamespaceString_forTest("dbtests.outputCollection");
    const NamespaceString dataCollectionNss =
        NamespaceString::createNamespaceString_forTest("dbtests.runFetchIteration");

    setupBasic(outputCollectionNss, dataCollectionNss, _destinationShard);

    AutoGetCollection dataColl(_opCtx, dataCollectionNss, LockMode::MODE_IX);

    auto fetcherJob = launchAsync([&, this] {
        ThreadClient tc("RefetcherRunner", _svcCtx->getService(), Client::noSession());

        const Timestamp doesNotExist(1, 1);
        ReshardingOplogFetcher fetcher(makeFetcherEnv(),
                                       _reshardingUUID,
                                       dataColl->uuid(),
                                       {doesNotExist, doesNotExist},
                                       _donorShard,
                                       _destinationShard,
                                       outputCollectionNss,
                                       true /* storeProgress*/);
        fetcher.useReadConcernForTest(false);

        // Status has a private default constructor so we wrap it in a boost::optional to placate
        // the Windows compiler.
        try {
            auto factory = makeCancelableOpCtx();
            fetcher.iterate(&cc(), factory);
            // Test failure case.
            return boost::optional<Status>(Status::OK());
        } catch (...) {
            return boost::optional<Status>(exceptionToStatus());
        }
    });

    auto fetcherStatus = requestPassthroughHandler(fetcherJob);

    ASSERT_EQ(0, itcount(outputCollectionNss));
    ASSERT_EQ(ErrorCodes::OplogQueryMinTsMissing, fetcherStatus->code());
    ASSERT_EQ(0, currentOpFetchedCount()) << " Verify currentOp metrics";
    assertUsedApplyOpsToBatchInsert(outputCollectionNss, 0 /* numApplyOpsOplogEntries */);
}

TEST_F(ReshardingOplogFetcherTest, TestAwaitInsert) {
    const NamespaceString outputCollectionNss =
        NamespaceString::createNamespaceString_forTest("dbtests.outputCollection");
    const NamespaceString dataCollectionNss =
        NamespaceString::createNamespaceString_forTest("dbtests.runFetchIteration");

    create(outputCollectionNss);
    create(dataCollectionNss);
    _fetchTimestamp = repl::StorageInterface::get(_svcCtx)->getLatestOplogTimestamp(_opCtx);

    const auto& collectionUUID = [&] {
        AutoGetCollection dataColl(_opCtx, dataCollectionNss, LockMode::MODE_IX);
        return dataColl->uuid();
    }();

    ReshardingDonorOplogId startAt{_fetchTimestamp, _fetchTimestamp};
    ReshardingOplogFetcher fetcher(makeFetcherEnv(),
                                   _reshardingUUID,
                                   collectionUUID,
                                   startAt,
                                   _donorShard,
                                   _destinationShard,
                                   outputCollectionNss,
                                   true /* storeProgress */);

    // The ReshardingOplogFetcher hasn't inserted a record yet so awaitInsert(startAt) won't be
    // immediately ready.
    auto hasSeenStartAtFuture = fetcher.awaitInsert(startAt);
    ASSERT_FALSE(hasSeenStartAtFuture.isReady());

    // Because no writes have happened to the data collection, the `hasSeenStartAtFuture` will still
    // not be ready.
    auto fetcherJob = launchAsync([&, this] {
        ThreadClient tc("RunnerForFetcher", _svcCtx->getService(), Client::noSession());
        fetcher.useReadConcernForTest(false);
        fetcher.setInitialBatchSizeForTest(2);
        auto factory = makeCancelableOpCtx();
        return fetcher.iterate(&cc(), factory);
    });

    ASSERT_TRUE(requestPassthroughHandler(fetcherJob));
    ASSERT_FALSE(hasSeenStartAtFuture.isReady());

    // Insert a document into the data collection and have it generate an oplog entry with a
    // "destinedRecipient" field.
    auto dataWriteTimestamp = [&] {
        FailPointEnableBlock fp("addDestinedRecipient",
                                BSON("destinedRecipient" << _destinationShard.toString()));

        {
            AutoGetCollection dataColl(_opCtx, dataCollectionNss, LockMode::MODE_IX);
            WriteUnitOfWork wuow(_opCtx);
            insertDocument(dataColl.getCollection(), InsertStatement(BSON("_id" << 1 << "a" << 1)));
            wuow.commit();
        }

        repl::StorageInterface::get(_opCtx)->waitForAllEarlierOplogWritesToBeVisible(_opCtx);
        return repl::StorageInterface::get(_svcCtx)->getLatestOplogTimestamp(_opCtx);
    }();

    fetcherJob = launchAsync([&, this] {
        ThreadClient tc("RunnerForFetcher", _svcCtx->getService(), Client::noSession());
        fetcher.useReadConcernForTest(false);
        fetcher.setInitialBatchSizeForTest(2);
        auto factory = makeCancelableOpCtx();
        return fetcher.iterate(&cc(), factory);
    });
    ASSERT_TRUE(requestPassthroughHandler(fetcherJob));
    ASSERT_TRUE(hasSeenStartAtFuture.isReady());

    // Asking for `startAt` again would return an immediately ready future.
    ASSERT_TRUE(fetcher.awaitInsert(startAt).isReady());

    // However, asking for `dataWriteTimestamp` wouldn't become ready until the next record is
    // inserted into the output collection.
    ASSERT_FALSE(fetcher.awaitInsert({dataWriteTimestamp, dataWriteTimestamp}).isReady());
}

TEST_F(ReshardingOplogFetcherTest, TestProgressMarkOplogInsert) {
    for (bool storeProgress : {false, true}) {
        LOGV2(9480005, "Running case", "storeProgress"_attr = storeProgress);

        const NamespaceString outputCollectionNss = NamespaceString::createNamespaceString_forTest(
            "dbtests.outputCollection" + std::to_string(storeProgress));
        const NamespaceString dataCollectionNss = NamespaceString::createNamespaceString_forTest(
            "dbtests.runFetchIteration" + std::to_string(storeProgress));

        create(outputCollectionNss);
        create(dataCollectionNss);

        const auto& collectionUUID = [&] {
            AutoGetCollection dataColl(_opCtx, dataCollectionNss, LockMode::MODE_IX);
            return dataColl->uuid();
        }();

        ReshardingDonorOplogId startAt{_fetchTimestamp, _fetchTimestamp};
        ReshardingOplogFetcher fetcher(makeFetcherEnv(),
                                       _reshardingUUID,
                                       collectionUUID,
                                       startAt,
                                       _donorShard,
                                       _destinationShard,
                                       outputCollectionNss,
                                       storeProgress);

        // A progressMarkOplog should not be inserted if the donor's cursor response has the same
        // timestamp as the initial startAt timestamp.
        auto postBatchResumeToken = startAt.getTs();
        auto oplogEntries = BSONArrayBuilder().arr();
        auto mockCursorResponse = makeMockAggregateResponse(postBatchResumeToken, oplogEntries);
        ASSERT_EQ(postBatchResumeToken, startAt.getTs());

        auto fetcherJob = launchAsync([&, this] {
            ThreadClient tc("RunnerForFetcher", _svcCtx->getService(), Client::noSession());
            auto factory = makeCancelableOpCtx();
            return fetcher.iterate(&cc(), factory);
        });
        ASSERT_TRUE(requestPassthroughHandler(fetcherJob, -1, mockCursorResponse));
        ASSERT_EQ(0, currentOpFetchedCount()) << " Verify currentOp metrics";
        ASSERT_EQ(0, persistedFetchedCount(_opCtx)) << " Verify persisted metrics";
        assertUsedApplyOpsToBatchInsert(outputCollectionNss, 0 /* numApplyOpsOplogEntries */);
        ASSERT_TRUE(getLast(outputCollectionNss).isEmpty());

        // A progressMarkOplog should be inserted if the donor's cursor response has an empty batch,
        // and a timestamp larger than the lastSeenTimestamp.
        postBatchResumeToken = _fetchTimestamp + 1;
        oplogEntries = BSONArrayBuilder().arr();
        mockCursorResponse = makeMockAggregateResponse(postBatchResumeToken, oplogEntries);
        ASSERT_GT(postBatchResumeToken, fetcher.getLastSeenTimestamp().getTs());

        fetcherJob = launchAsync([&, this] {
            ThreadClient tc("RunnerForFetcher", _svcCtx->getService(), Client::noSession());
            auto factory = makeCancelableOpCtx();
            return fetcher.iterate(&cc(), factory);
        });
        ASSERT_TRUE(requestPassthroughHandler(fetcherJob, -1, mockCursorResponse));
        ASSERT_EQ(1, currentOpFetchedCount()) << " Verify currentOp metrics";
        ASSERT_EQ(storeProgress ? 1 : 0, persistedFetchedCount(_opCtx))
            << " Verify persisted metrics";
        assertUsedApplyOpsToBatchInsert(outputCollectionNss, 1 /* numApplyOpsOplogEntries */);
        auto lastOplogInOuptut = getLast(outputCollectionNss);
        ASSERT_EQ(resharding::kReshardProgressMark,
                  lastOplogInOuptut.getObjectField("o2").getField("type").String());
        ASSERT_EQ(postBatchResumeToken,
                  lastOplogInOuptut.getObjectField("_id").getField("ts").timestamp());

        // A progressMarkOplog should not be inserted if the donor's cursor response has a non-empty
        // batch.
        postBatchResumeToken = _fetchTimestamp + 2;
        auto oplog = makeOplog(dataCollectionNss,
                               collectionUUID,
                               repl::OpTypeEnum::kInsert,
                               BSONObj(),
                               BSONObj(),
                               ReshardingDonorOplogId(postBatchResumeToken, postBatchResumeToken))
                         .toBSON();
        oplogEntries = BSON_ARRAY(oplog);
        mockCursorResponse = makeMockAggregateResponse(postBatchResumeToken, oplogEntries);

        fetcherJob = launchAsync([&, this] {
            ThreadClient tc("RunnerForFetcher", _svcCtx->getService(), Client::noSession());
            auto factory = makeCancelableOpCtx();
            return fetcher.iterate(&cc(), factory);
        });
        ASSERT_TRUE(requestPassthroughHandler(fetcherJob, -1, mockCursorResponse));
        ASSERT_EQ(2, currentOpFetchedCount()) << " Verify currentOp metrics";
        ASSERT_EQ(storeProgress ? 2 : 0, persistedFetchedCount(_opCtx))
            << " Verify persisted metrics";
        assertUsedApplyOpsToBatchInsert(outputCollectionNss, 2 /* numApplyOpsOplogEntries */);
        ASSERT_EQ(getLast(outputCollectionNss).woCompare(oplog), 0);

        // A progressMarkOplog should not be inserted if the donor's cursor response has the same
        // timestamp as the lastSeenTimestamp.
        oplogEntries = BSONArrayBuilder().arr();
        mockCursorResponse = makeMockAggregateResponse(postBatchResumeToken, oplogEntries);
        ASSERT_EQ(postBatchResumeToken, fetcher.getLastSeenTimestamp().getTs());

        fetcherJob = launchAsync([&, this] {
            ThreadClient tc("RunnerForFetcher", _svcCtx->getService(), Client::noSession());
            auto factory = makeCancelableOpCtx();
            return fetcher.iterate(&cc(), factory);
        });
        ASSERT_TRUE(requestPassthroughHandler(fetcherJob, -1, mockCursorResponse));
        ASSERT_EQ(2, currentOpFetchedCount()) << " Verify currentOp metrics";
        ASSERT_EQ(storeProgress ? 2 : 0, persistedFetchedCount(_opCtx))
            << " Verify persisted metrics";
        assertUsedApplyOpsToBatchInsert(outputCollectionNss, 2 /* numApplyOpsOplogEntries */);
        ASSERT_EQ(getLast(outputCollectionNss).woCompare(oplog), 0);

        resetResharding();
    }
}

TEST_F(ReshardingOplogFetcherTest, TestStartAtUpdatedWithProgressMarkOplogTs) {
    for (bool storeProgress : {false, true}) {
        LOGV2(9480006, "Running case", "storeProgress"_attr = storeProgress);

        const NamespaceString outputCollectionNss = NamespaceString::createNamespaceString_forTest(
            "dbtests.outputCollection" + std::to_string(storeProgress));
        const NamespaceString dataCollectionNss = NamespaceString::createNamespaceString_forTest(
            "dbtests.runFetchIteration" + std::to_string(storeProgress));
        const NamespaceString otherCollection = NamespaceString::createNamespaceString_forTest(
            "dbtests.collectionNotBeingResharded" + std::to_string(storeProgress));

        create(outputCollectionNss);
        create(dataCollectionNss);
        create(otherCollection);
        _fetchTimestamp = repl::StorageInterface::get(_svcCtx)->getLatestOplogTimestamp(_opCtx);

        const auto& collectionUUID = [&] {
            AutoGetCollection dataColl(_opCtx, dataCollectionNss, LockMode::MODE_IX);
            return dataColl->uuid();
        }();

        ReshardingDonorOplogId startAt{_fetchTimestamp, _fetchTimestamp};
        ReshardingOplogFetcher fetcher(makeFetcherEnv(),
                                       _reshardingUUID,
                                       collectionUUID,
                                       startAt,
                                       _donorShard,
                                       _destinationShard,
                                       outputCollectionNss,
                                       storeProgress);

        // Insert a document into the data collection and have it generate an oplog entry with a
        // "destinedRecipient" field.
        auto writeToDataCollectionTs = [&] {
            FailPointEnableBlock fp("addDestinedRecipient",
                                    BSON("destinedRecipient" << _destinationShard.toString()));

            {
                AutoGetCollection dataColl(_opCtx, dataCollectionNss, LockMode::MODE_IX);
                WriteUnitOfWork wuow(_opCtx);
                insertDocument(dataColl.getCollection(),
                               InsertStatement(BSON("_id" << 1 << "a" << 1)));
                wuow.commit();
            }

            repl::StorageInterface::get(_opCtx)->waitForAllEarlierOplogWritesToBeVisible(_opCtx);
            return repl::StorageInterface::get(_svcCtx)->getLatestOplogTimestamp(_opCtx);
        }();

        auto fetcherJob = launchAsync([&, this] {
            ThreadClient tc("RunnerForFetcher", _svcCtx->getService(), Client::noSession());
            fetcher.useReadConcernForTest(false);
            fetcher.setInitialBatchSizeForTest(2);
            auto factory = makeCancelableOpCtx();
            return fetcher.iterate(&cc(), factory);
        });
        ASSERT_TRUE(requestPassthroughHandler(fetcherJob));

        // The fetcher's lastSeenTimestamp should be equal to `writeToDataCollectionTs`.
        ASSERT_TRUE(fetcher.getLastSeenTimestamp().getClusterTime() == writeToDataCollectionTs);
        ASSERT_TRUE(fetcher.getLastSeenTimestamp().getTs() == writeToDataCollectionTs);
        ASSERT_EQ(1, currentOpFetchedCount()) << " Verify currentOp metrics";
        ASSERT_EQ(storeProgress ? 1 : 0, persistedFetchedCount(_opCtx))
            << " Verify persisted metrics";
        assertUsedApplyOpsToBatchInsert(outputCollectionNss, 1 /* numApplyOpsOplogEntries */);

        // Now, insert a document into a different collection that is not involved in resharding.
        auto writeToOtherCollectionTs = [&] {
            {
                AutoGetCollection dataColl(_opCtx, otherCollection, LockMode::MODE_IX);
                WriteUnitOfWork wuow(_opCtx);
                insertDocument(dataColl.getCollection(),
                               InsertStatement(BSON("_id" << 1 << "a" << 1)));
                wuow.commit();
            }

            repl::StorageInterface::get(_opCtx)->waitForAllEarlierOplogWritesToBeVisible(_opCtx);
            return repl::StorageInterface::get(_svcCtx)->getLatestOplogTimestamp(_opCtx);
        }();

        fetcherJob = launchAsync([&, this] {
            ThreadClient tc("RunnerForFetcher", _svcCtx->getService(), Client::noSession());
            fetcher.useReadConcernForTest(false);
            fetcher.setInitialBatchSizeForTest(2);
            auto factory = makeCancelableOpCtx();
            return fetcher.iterate(&cc(), factory);
        });
        ASSERT_TRUE(requestPassthroughHandler(fetcherJob));

        // The fetcher's lastSeenTimestamp should now be equal to `writeToOtherCollectionTs`
        // because the lastSeenTimestamp will be updated with the latest oplog timestamp from the
        // donor's cursor response.
        ASSERT_TRUE(fetcher.getLastSeenTimestamp().getClusterTime() == writeToOtherCollectionTs);
        ASSERT_TRUE(fetcher.getLastSeenTimestamp().getTs() == writeToOtherCollectionTs);
        ASSERT_EQ(2, currentOpFetchedCount()) << " Verify currentOp metrics";
        ASSERT_EQ(storeProgress ? 2 : 0, persistedFetchedCount(_opCtx))
            << " Verify persisted metrics";
        assertUsedApplyOpsToBatchInsert(outputCollectionNss, 2 /* numApplyOpsOplogEntries */);

        // The last document returned by ReshardingDonorOplogIterator::getNextBatch() would be
        // `writeToDataCollectionTs`, but ReshardingOplogFetcher would have inserted a doc with
        // `writeToOtherCollectionTs` after this so `awaitInsert` should be immediately ready when
        // passed `writeToDataCollectionTs`.
        ASSERT_TRUE(
            fetcher.awaitInsert({writeToDataCollectionTs, writeToDataCollectionTs}).isReady());

        // `awaitInsert` should not be ready if passed `writeToOtherCollectionTs`.
        ASSERT_FALSE(
            fetcher.awaitInsert({writeToOtherCollectionTs, writeToOtherCollectionTs}).isReady());

        resetResharding();
    }
}

TEST_F(ReshardingOplogFetcherTest, RetriesOnRemoteInterruptionError) {
    const NamespaceString outputCollectionNss =
        NamespaceString::createNamespaceString_forTest("dbtests.outputCollection");
    const NamespaceString dataCollectionNss =
        NamespaceString::createNamespaceString_forTest("dbtests.runFetchIteration");

    create(outputCollectionNss);
    create(dataCollectionNss);
    _fetchTimestamp = repl::StorageInterface::get(_svcCtx)->getLatestOplogTimestamp(_opCtx);

    const auto& collectionUUID = [&] {
        AutoGetCollection dataColl(_opCtx, dataCollectionNss, LockMode::MODE_IX);
        return dataColl->uuid();
    }();

    auto fetcherJob = launchAsync([&, this] {
        ThreadClient tc("RunnerForFetcher", _svcCtx->getService(), Client::noSession());

        ReshardingDonorOplogId startAt{_fetchTimestamp, _fetchTimestamp};
        ReshardingOplogFetcher fetcher(makeFetcherEnv(),
                                       _reshardingUUID,
                                       collectionUUID,
                                       startAt,
                                       _donorShard,
                                       _destinationShard,
                                       outputCollectionNss,
                                       true /* storeProgress */);
        fetcher.useReadConcernForTest(false);
        fetcher.setInitialBatchSizeForTest(2);

        auto factory = makeCancelableOpCtx();
        return fetcher.iterate(&cc(), factory);
    });

    onCommand([&](const executor::RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        // Simulate the remote donor shard stepping down or transitioning into rollback.
        return {ErrorCodes::InterruptedDueToReplStateChange, "operation was interrupted"};
    });

    auto moreToCome = fetcherJob.timed_get(Seconds(5));
    ASSERT_TRUE(moreToCome);
}

TEST_F(ReshardingOplogFetcherTest, RetriesOnNetworkTimeoutError) {
    const NamespaceString outputCollectionNss =
        NamespaceString::createNamespaceString_forTest("dbtests.outputCollection");
    const NamespaceString dataCollectionNss =
        NamespaceString::createNamespaceString_forTest("dbtests.runFetchIteration");

    create(outputCollectionNss);
    create(dataCollectionNss);
    _fetchTimestamp = repl::StorageInterface::get(_svcCtx)->getLatestOplogTimestamp(_opCtx);

    const auto& collectionUUID = [&] {
        AutoGetCollection dataColl(_opCtx, dataCollectionNss, LockMode::MODE_IX);
        return dataColl->uuid();
    }();

    auto fetcherJob = launchAsync([&, this] {
        ThreadClient tc("RunnerForFetcher", _svcCtx->getService(), Client::noSession());

        ReshardingDonorOplogId startAt{_fetchTimestamp, _fetchTimestamp};
        ReshardingOplogFetcher fetcher(makeFetcherEnv(),
                                       _reshardingUUID,
                                       collectionUUID,
                                       startAt,
                                       _donorShard,
                                       _destinationShard,
                                       outputCollectionNss,
                                       true /* storeProgress */);

        auto factory = makeCancelableOpCtx();
        return fetcher.iterate(&cc(), factory);
    });

    onCommand([&](const executor::RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        // Inject network timeout error.
        return {ErrorCodes::NetworkInterfaceExceededTimeLimit, "exceeded network time limit"};
    });

    auto moreToCome = fetcherJob.timed_get(Seconds(5));
    ASSERT_TRUE(moreToCome);
}

TEST_F(ReshardingOplogFetcherTest, ImmediatelyDoneWhenFinalOpHasAlreadyBeenFetched) {
    const NamespaceString outputCollectionNss =
        NamespaceString::createNamespaceString_forTest("dbtests.outputCollection");
    const NamespaceString dataCollectionNss =
        NamespaceString::createNamespaceString_forTest("dbtests.runFetchIteration");

    create(outputCollectionNss);
    create(dataCollectionNss);
    _fetchTimestamp = repl::StorageInterface::get(_svcCtx)->getLatestOplogTimestamp(_opCtx);

    const auto& collectionUUID = [&] {
        AutoGetCollection dataColl(_opCtx, dataCollectionNss, LockMode::MODE_IX);
        return dataColl->uuid();
    }();

    ReshardingOplogFetcher fetcher(makeFetcherEnv(),
                                   _reshardingUUID,
                                   collectionUUID,
                                   ReshardingOplogFetcher::kFinalOpAlreadyFetched,
                                   _donorShard,
                                   _destinationShard,
                                   outputCollectionNss,
                                   true /* storeProgress */);

    auto future = fetcher.schedule(nullptr, CancellationToken::uncancelable());

    ASSERT_TRUE(future.isReady());
    ASSERT_OK(future.getNoThrow());
}

DEATH_TEST_REGEX_F(ReshardingOplogFetcherTest,
                   CannotFetchMoreWhenFinalOpHasAlreadyBeenFetched,
                   "Invariant failure.*_startAt != kFinalOpAlreadyFetched") {
    const NamespaceString outputCollectionNss =
        NamespaceString::createNamespaceString_forTest("dbtests.outputCollection");
    const NamespaceString dataCollectionNss =
        NamespaceString::createNamespaceString_forTest("dbtests.runFetchIteration");

    create(outputCollectionNss);
    create(dataCollectionNss);
    _fetchTimestamp = repl::StorageInterface::get(_svcCtx)->getLatestOplogTimestamp(_opCtx);

    const auto& collectionUUID = [&] {
        AutoGetCollection dataColl(_opCtx, dataCollectionNss, LockMode::MODE_IX);
        return dataColl->uuid();
    }();

    auto fetcherJob = launchAsync([&, this] {
        ThreadClient tc("RunnerForFetcher", _svcCtx->getService(), Client::noSession());

        // We intentionally do not call fetcher.useReadConcernForTest(false) for this test case.
        ReshardingOplogFetcher fetcher(makeFetcherEnv(),
                                       _reshardingUUID,
                                       collectionUUID,
                                       ReshardingOplogFetcher::kFinalOpAlreadyFetched,
                                       _donorShard,
                                       _destinationShard,
                                       outputCollectionNss,
                                       true /* storeProgress */);
        fetcher.setInitialBatchSizeForTest(2);

        auto factory = makeCancelableOpCtx();
        return fetcher.iterate(&cc(), factory);
    });

    // Calling onCommand() leads to a more helpful "Expected death, found life" error when the
    // invariant failure isn't triggered.
    onCommand([&](const executor::RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        return {ErrorCodes::InternalError, "this error should never be observed"};
    });

    (void)fetcherJob.timed_get(Seconds(5));
}

TEST_F(ReshardingOplogFetcherTest, ReadPreferenceBeforeAfterCriticalSection_TargetPrimary) {
    // Not set the reshardingOplogFetcherTargetPrimaryDuringCriticalSection to test that the
    // default is true.
    const NamespaceString outputCollectionNss =
        NamespaceString::createNamespaceString_forTest("dbtests.outputCollection");
    const NamespaceString dataCollectionNss =
        NamespaceString::createNamespaceString_forTest("dbtests.runFetchIteration");

    create(outputCollectionNss);
    create(dataCollectionNss);

    const auto& collectionUUID = [&] {
        AutoGetCollection dataColl(_opCtx, dataCollectionNss, LockMode::MODE_IX);
        return dataColl->uuid();
    }();

    ReshardingOplogFetcher fetcher(makeFetcherEnv(),
                                   _reshardingUUID,
                                   collectionUUID,
                                   {_fetchTimestamp, _fetchTimestamp},
                                   _donorShard,
                                   _destinationShard,
                                   outputCollectionNss,
                                   true /* storeProgress */);
    auto executor = makeExecutor();
    executor->startup();
    auto fetcherFuture = fetcher.schedule(executor, CancellationToken::uncancelable());

    // Make the cursor for the the aggregate command below have a non-zero id to test that
    // onEnteringCriticalSection() interrupts the in-progress aggregation. So the fetcher should not
    // schedule a getMore command after this.
    auto cursorIdBeforeCriticalSection = 123;
    auto aggBeforeCriticalSectionFuture = launchAsync([&, this] {
        onCommand([&](const executor::RemoteCommandRequest& request) -> StatusWith<BSONObj> {
            auto expectedReadPref = ReadPreferenceSetting{
                ReadPreference::Nearest, ReadPreferenceSetting::kMinimalMaxStalenessValue};
            assertAggregateReadPreference(request, expectedReadPref);

            fetcher.onEnteringCriticalSection();

            auto postBatchResumeToken = _fetchTimestamp + 1;
            return makeMockAggregateResponse(
                postBatchResumeToken, {} /* oplogEntries */, cursorIdBeforeCriticalSection);
        });
    });

    aggBeforeCriticalSectionFuture.default_timed_get();

    // Depending on when the interrupt occurs, the fetcher may still try to kill the cursor after
    // the cancellation. In that case, schedule a response for the killCursor command.
    auto makeKillCursorResponse = [&](const executor::RemoteCommandRequest& request) {
        auto parsedRequest = KillCursorsCommandRequest::parse(
            IDLParserContext(_agent.getTestName()),
            request.cmdObj.addFields(BSON("$db" << request.dbname.toString_forTest())));

        ASSERT_EQ(parsedRequest.getNamespace().ns_forTest(),
                  NamespaceString::kRsOplogNamespace.toString_forTest());
        ASSERT_EQ(parsedRequest.getCursorIds().size(), 1U);
        ASSERT_EQ(parsedRequest.getCursorIds()[0], cursorIdBeforeCriticalSection);
        return BSONObj{};
    };

    // Make the cursor for the the aggregate command below have a non-zero id to test that the
    // fetcher does not schedule a getMore command after seeing the final oplog entry.
    auto cursorIdDuringCriticalSection = 456;
    auto makeAggResponse = [&](const executor::RemoteCommandRequest& request) {
        auto expectedReadPref = ReadPreferenceSetting{ReadPreference::PrimaryOnly};
        assertAggregateReadPreference(request, expectedReadPref);

        auto postBatchResumeToken = _fetchTimestamp + 2;
        auto oplogEntries = BSON_ARRAY(
            makeFinalNoopOplogEntry(dataCollectionNss, collectionUUID, postBatchResumeToken));
        return makeMockAggregateResponse(
            postBatchResumeToken, oplogEntries, cursorIdDuringCriticalSection);
    };

    bool scheduledAggResponse = false;
    onCommand([&](const executor::RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        auto cmdName = request.cmdObj.firstElementFieldName();
        if (cmdName == "killCursors"_sd) {
            return makeKillCursorResponse(request);
        } else if (cmdName == "aggregate"_sd) {
            scheduledAggResponse = true;
            return makeAggResponse(request);
        }
        return {ErrorCodes::InternalError,
                str::stream() << "Unexpected command request " << request.toString()};
    });
    if (!scheduledAggResponse) {
        onCommand([&](const executor::RemoteCommandRequest& request) -> StatusWith<BSONObj> {
            return makeAggResponse(request);
        });
    }

    ASSERT_OK(fetcherFuture.getNoThrow());
    executor->shutdown();
    executor->join();
}

TEST_F(ReshardingOplogFetcherTest, ReadPreferenceBeforeAfterCriticalSection_NotTargetPrimary) {
    RAIIServerParameterControllerForTest targetPrimaryDuringCriticalSection{
        "reshardingOplogFetcherTargetPrimaryDuringCriticalSection", false};

    const NamespaceString outputCollectionNss =
        NamespaceString::createNamespaceString_forTest("dbtests.outputCollection");
    const NamespaceString dataCollectionNss =
        NamespaceString::createNamespaceString_forTest("dbtests.runFetchIteration");

    create(outputCollectionNss);
    create(dataCollectionNss);

    const auto& collectionUUID = [&] {
        AutoGetCollection dataColl(_opCtx, dataCollectionNss, LockMode::MODE_IX);
        return dataColl->uuid();
    }();

    ReshardingOplogFetcher fetcher(makeFetcherEnv(),
                                   _reshardingUUID,
                                   collectionUUID,
                                   {_fetchTimestamp, _fetchTimestamp},
                                   _donorShard,
                                   _destinationShard,
                                   outputCollectionNss,
                                   true /* storeProgress */);
    auto executor = makeExecutor();
    executor->startup();
    auto fetcherFuture = fetcher.schedule(executor, CancellationToken::uncancelable());

    // Make the cursor for the the aggregate command below have a non-zero id to test that
    // onEnteringCriticalSection() does not interrupt the in-progress aggregation. The fetcher
    // should schedule a getMore command after this.
    auto cursorIdBeforeCriticalSection = 123;
    onCommand([&](const executor::RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        auto expectedReadPref = ReadPreferenceSetting{
            ReadPreference::Nearest, ReadPreferenceSetting::kMinimalMaxStalenessValue};
        assertAggregateReadPreference(request, expectedReadPref);

        fetcher.onEnteringCriticalSection();

        auto postBatchResumeToken = _fetchTimestamp + 1;
        return makeMockAggregateResponse(
            postBatchResumeToken, {} /* oplogEntries */, cursorIdBeforeCriticalSection);
    });

    onCommand([&](const executor::RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        assertGetMoreCursorId(request, cursorIdBeforeCriticalSection);
        auto postBatchResumeToken = _fetchTimestamp + 2;
        return makeMockAggregateResponse(
            postBatchResumeToken, {} /* oplogEntries */, cursorIdBeforeCriticalSection);
    });

    // The fetcher should kill the cursor after exhausting it.
    onCommand([&](const executor::RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        auto parsedRequest = KillCursorsCommandRequest::parse(
            IDLParserContext(_agent.getTestName()),
            request.cmdObj.addFields(BSON("$db" << request.dbname.toString_forTest())));

        ASSERT_EQ(parsedRequest.getNamespace().ns_forTest(),
                  NamespaceString::kRsOplogNamespace.toString_forTest());
        ASSERT_EQ(parsedRequest.getCursorIds().size(), 1U);
        ASSERT_EQ(parsedRequest.getCursorIds()[0], cursorIdBeforeCriticalSection);
        return BSONObj{};
    });

    // Make the cursor for the the aggregate command below have a non-zero id to test that the
    // fetcher does not schedule a getMore command after seeing the final oplog entry.
    auto cursorIdDuringCriticalSection = 456;
    onCommand([&](const executor::RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        auto expectedReadPref = ReadPreferenceSetting{
            ReadPreference::Nearest, ReadPreferenceSetting::kMinimalMaxStalenessValue};
        assertAggregateReadPreference(request, expectedReadPref);

        auto postBatchResumeToken = _fetchTimestamp + 2;
        auto oplogEntries = BSON_ARRAY(
            makeFinalNoopOplogEntry(dataCollectionNss, collectionUUID, postBatchResumeToken));
        return makeMockAggregateResponse(
            postBatchResumeToken, oplogEntries, cursorIdDuringCriticalSection);
    });

    ASSERT_OK(fetcherFuture.getNoThrow());
    executor->shutdown();
    executor->join();
}

TEST_F(ReshardingOplogFetcherTest, OnEnteringCriticalSectionBeforeScheduling) {
    const NamespaceString outputCollectionNss =
        NamespaceString::createNamespaceString_forTest("dbtests.outputCollection");
    const NamespaceString dataCollectionNss =
        NamespaceString::createNamespaceString_forTest("dbtests.runFetchIteration");

    create(outputCollectionNss);
    create(dataCollectionNss);

    const auto& collectionUUID = [&] {
        AutoGetCollection dataColl(_opCtx, dataCollectionNss, LockMode::MODE_IX);
        return dataColl->uuid();
    }();

    ReshardingOplogFetcher fetcher(makeFetcherEnv(),
                                   _reshardingUUID,
                                   collectionUUID,
                                   {_fetchTimestamp, _fetchTimestamp},
                                   _donorShard,
                                   _destinationShard,
                                   outputCollectionNss,
                                   true /* storeProgress */);
    fetcher.onEnteringCriticalSection();

    auto executor = makeExecutor();
    executor->startup();
    auto fetcherFuture = fetcher.schedule(executor, CancellationToken::uncancelable());

    // Make the cursor for the the aggregate command below have a non-zero id to test that the
    // fetcher does not schedule a getMore command after seeing the final oplog entry.
    auto cursorIdDuringCriticalSection = 123;
    onCommand([&](const executor::RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        auto expectedReadPref = ReadPreferenceSetting{ReadPreference::PrimaryOnly};
        assertAggregateReadPreference(request, expectedReadPref);

        auto postBatchResumeToken = _fetchTimestamp + 1;
        auto oplogEntries = BSON_ARRAY(
            makeFinalNoopOplogEntry(dataCollectionNss, collectionUUID, postBatchResumeToken));
        return makeMockAggregateResponse(
            postBatchResumeToken, oplogEntries, cursorIdDuringCriticalSection);
    });

    ASSERT_OK(fetcherFuture.getNoThrow());
    executor->shutdown();
    executor->join();
}

TEST_F(ReshardingOplogFetcherTest, OnEnteringCriticalSectionMoreThanOnce) {
    const NamespaceString outputCollectionNss =
        NamespaceString::createNamespaceString_forTest("dbtests.outputCollection");
    const NamespaceString dataCollectionNss =
        NamespaceString::createNamespaceString_forTest("dbtests.runFetchIteration");

    create(outputCollectionNss);
    create(dataCollectionNss);

    const auto& collectionUUID = [&] {
        AutoGetCollection dataColl(_opCtx, dataCollectionNss, LockMode::MODE_IX);
        return dataColl->uuid();
    }();

    ReshardingOplogFetcher fetcher(makeFetcherEnv(),
                                   _reshardingUUID,
                                   collectionUUID,
                                   {_fetchTimestamp, _fetchTimestamp},
                                   _donorShard,
                                   _destinationShard,
                                   outputCollectionNss,
                                   true /* storeProgress */);
    auto executor = makeExecutor();
    executor->startup();
    auto fetcherFuture = fetcher.schedule(executor, CancellationToken::uncancelable());

    // Make the cursor for the the aggregate command below have id 0 to make the fetcher not
    // schedule a getMore command so that the test does not need to also schedule a getMore
    // response.
    auto cursorIdBeforeCriticalSection = 0;
    onCommand([&](const executor::RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        auto expectedReadPref = ReadPreferenceSetting{
            ReadPreference::Nearest, ReadPreferenceSetting::kMinimalMaxStalenessValue};
        assertAggregateReadPreference(request, expectedReadPref);

        fetcher.onEnteringCriticalSection();
        fetcher.onEnteringCriticalSection();

        auto postBatchResumeToken = _fetchTimestamp + 1;
        return makeMockAggregateResponse(
            postBatchResumeToken, {} /* oplogEntries */, cursorIdBeforeCriticalSection);
    });

    // Make the cursor for the the aggregate command below have a non-zero id to test that the
    // fetcher does not schedule a getMore command after seeing the final oplog entry.
    auto cursorIdDuringCriticalSection = 123;
    onCommand([&](const executor::RemoteCommandRequest& request) -> StatusWith<BSONObj> {
        auto expectedReadPref = ReadPreferenceSetting{ReadPreference::PrimaryOnly};
        assertAggregateReadPreference(request, expectedReadPref);

        auto postBatchResumeToken = _fetchTimestamp + 2;
        auto oplogEntries = BSON_ARRAY(
            makeFinalNoopOplogEntry(dataCollectionNss, collectionUUID, postBatchResumeToken));
        return makeMockAggregateResponse(
            postBatchResumeToken, oplogEntries, cursorIdDuringCriticalSection);
    });

    ASSERT_OK(fetcherFuture.getNoThrow());
    fetcher.onEnteringCriticalSection();
    executor->shutdown();
    executor->join();
}

TEST_F(ReshardingOplogFetcherTest, OnEnteringCriticalSectionAfterFetchingFinalOplogEntry) {
    for (bool targetPrimary : {false, true}) {
        LOGV2(10355403, "Running case", "targetPrimary"_attr = targetPrimary);

        RAIIServerParameterControllerForTest targetPrimaryDuringCriticalSection{
            "reshardingOplogFetcherTargetPrimaryDuringCriticalSection", targetPrimary};

        const NamespaceString outputCollectionNss = NamespaceString::createNamespaceString_forTest(
            "dbtests.outputCollection" + std::to_string(targetPrimary));
        const NamespaceString dataCollectionNss = NamespaceString::createNamespaceString_forTest(
            "dbtests.runFetchIteration" + std::to_string(targetPrimary));

        create(outputCollectionNss);
        create(dataCollectionNss);

        const auto& collectionUUID = [&] {
            AutoGetCollection dataColl(_opCtx, dataCollectionNss, LockMode::MODE_IX);
            return dataColl->uuid();
        }();

        ReshardingOplogFetcher fetcher(makeFetcherEnv(),
                                       _reshardingUUID,
                                       collectionUUID,
                                       {_fetchTimestamp, _fetchTimestamp},
                                       _donorShard,
                                       _destinationShard,
                                       outputCollectionNss,
                                       true /* storeProgress */);
        auto executor = makeExecutor();
        executor->startup();

        // Invoke onEnterCriticalSection() after the fetcher has consumed the final oplog entry.
        auto fp = globalFailPointRegistry().find("pauseReshardingOplogFetcherAfterConsuming");
        auto timesEnteredBefore = fp->setMode(FailPoint::alwaysOn);

        auto fetcherFuture = fetcher.schedule(executor, CancellationToken::uncancelable());

        auto cursorId = 123;
        auto aggBeforeCriticalSectionFuture = launchAsync([&, this] {
            onCommand([&](const executor::RemoteCommandRequest& request) -> StatusWith<BSONObj> {
                auto expectedReadPref = ReadPreferenceSetting{
                    ReadPreference::Nearest, ReadPreferenceSetting::kMinimalMaxStalenessValue};
                assertAggregateReadPreference(request, expectedReadPref);

                auto postBatchResumeToken = _fetchTimestamp + 1;
                auto oplogEntries = BSON_ARRAY(makeFinalNoopOplogEntry(
                    dataCollectionNss, collectionUUID, postBatchResumeToken));
                return makeMockAggregateResponse(postBatchResumeToken, oplogEntries, cursorId);
            });
        });

        fp->waitForTimesEntered(timesEnteredBefore + 1);

        fetcher.onEnteringCriticalSection();
        auto timesEnteredAfter = fp->setMode(FailPoint::off);
        ASSERT_EQ(timesEnteredAfter, timesEnteredBefore + 1);

        aggBeforeCriticalSectionFuture.default_timed_get();

        // Schedule a response for the killCursor command to prevent its request from interfering
        // with the next test case.
        onCommand([&](const executor::RemoteCommandRequest& request) -> StatusWith<BSONObj> {
            auto parsedRequest = KillCursorsCommandRequest::parse(
                IDLParserContext(_agent.getTestName()),
                request.cmdObj.addFields(BSON("$db" << request.dbname.toString_forTest())));

            ASSERT_EQ(parsedRequest.getNamespace().ns_forTest(),
                      NamespaceString::kRsOplogNamespace.toString_forTest());
            ASSERT_EQ(parsedRequest.getCursorIds().size(), 1U);
            ASSERT_EQ(parsedRequest.getCursorIds()[0], cursorId);
            return BSONObj{};
        });

        // The fetcher should not schedule another aggregate command. If it does, it would get stuck
        // waiting for the aggregate response which the test does not schedule.
        ASSERT_OK(fetcherFuture.getNoThrow());
        executor->shutdown();
        executor->join();

        resetResharding();
    }
}

}  // namespace
}  // namespace mongo
