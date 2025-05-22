/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/db/storage/wiredtiger/wiredtiger_recovery_unit.h"

#include <cstring>
#include <string>
#include <utility>
#include <wiredtiger.h>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/base/checked_cast.h"
#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/base/initializer.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/client.h"
#include "mongo/db/global_settings.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/record_id.h"
#include "mongo/db/repl/repl_set_member_in_standalone_mode.h"
#include "mongo/db/repl/repl_settings.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/execution_context.h"
#include "mongo/db/storage/key_format.h"
#include "mongo/db/storage/record_data.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/recovery_unit_test_harness.h"
#include "mongo/db/storage/snapshot_manager.h"
#include "mongo/db/storage/storage_parameters_gen.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_cursor_helpers.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_kv_engine.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_record_store.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_util.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/transaction_resources.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/temp_dir.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/clock_source_mock.h"
#include "mongo/util/str.h"

namespace mongo {
namespace {

class WiredTigerRecoveryUnitHarnessHelper final : public RecoveryUnitHarnessHelper {
public:
    WiredTigerRecoveryUnitHarnessHelper() : _dbpath("wt_test") {
        WiredTigerKVEngineBase::WiredTigerConfig wtConfig = getWiredTigerConfigFromStartupOptions();
        wtConfig.cacheSizeMB = 1;

        // Use a replica set so that writes to replicated collections are not journaled and thus
        // retain their timestamps.
        repl::ReplSettings replSettings;
        replSettings.setReplSetString("rs");
        setGlobalReplSettings(replSettings);
        repl::ReplicationCoordinator::set(getGlobalServiceContext(),
                                          std::make_unique<repl::ReplicationCoordinatorMock>(
                                              getGlobalServiceContext(), replSettings));

        _engine = std::make_unique<WiredTigerKVEngine>(
            std::string{kWiredTigerEngineName},
            _dbpath.path(),
            &_cs,
            std::move(wtConfig),
            false,
            false,
            getGlobalReplSettings().isReplSet(),
            repl::ReplSettings::shouldRecoverFromOplogAsStandalone(),
            getReplSetMemberInStandaloneMode(getGlobalServiceContext()));

        _engine->notifyStorageStartupRecoveryComplete();
    }

    ~WiredTigerRecoveryUnitHarnessHelper() override {}

    std::unique_ptr<RecoveryUnit> newRecoveryUnit() final {
        return std::unique_ptr<RecoveryUnit>(_engine->newRecoveryUnit());
    }

    std::unique_ptr<RecordStore> createRecordStore(OperationContext* opCtx,
                                                   const std::string& ns) final {
        std::string ident = ns;
        NamespaceString nss = NamespaceString::createNamespaceString_forTest(ns);
        const auto res = _engine->createRecordStore(nss, ident, RecordStore::Options{});
        return _engine->getRecordStore(opCtx, nss, ident, RecordStore::Options{}, UUID::gen());
    }

    WiredTigerKVEngine* getEngine() {
        return _engine.get();
    }

    ClockSourceMock* getClockSourceMock() {
        return &_cs;
    }

private:
    unittest::TempDir _dbpath;
    ClockSourceMock _cs;
    std::unique_ptr<WiredTigerKVEngine> _engine;
};

std::unique_ptr<RecoveryUnitHarnessHelper> makeWTRUHarnessHelper() {
    return std::make_unique<WiredTigerRecoveryUnitHarnessHelper>();
}

MONGO_INITIALIZER(RegisterHarnessFactory)(InitializerContext* const) {
    mongo::registerRecoveryUnitHarnessHelperFactory(makeWTRUHarnessHelper);
}

class WiredTigerRecoveryUnitTestFixture : public unittest::Test {
public:
    typedef std::pair<ServiceContext::UniqueClient, ServiceContext::UniqueOperationContext>
        ClientAndCtx;

    ClientAndCtx makeClientAndOpCtx(RecoveryUnitHarnessHelper* harnessHelper,
                                    const std::string& clientName) {
        auto sc = harnessHelper->serviceContext();
        auto client = sc->getService()->makeClient(clientName);
        auto opCtx = client->makeOperationContext();
        shard_role_details::setRecoveryUnit(opCtx.get(),
                                            harnessHelper->newRecoveryUnit(),
                                            WriteUnitOfWork::RecoveryUnitState::kNotInUnitOfWork);
        return std::make_pair(std::move(client), std::move(opCtx));
    }

    void getCursor(WiredTigerRecoveryUnit* ru, WT_CURSOR** cursor) {
        WiredTigerSession* session = ru->getSession();
        invariantWTOK(session->create(wt_uri, wt_config), *session);
        invariantWTOK(session->open_cursor(wt_uri, nullptr, nullptr, cursor), *session);
    }

    void setUp() override {
        harnessHelper = std::make_unique<WiredTigerRecoveryUnitHarnessHelper>();
        clientAndCtx1 = makeClientAndOpCtx(harnessHelper.get(), "writer");
        clientAndCtx2 = makeClientAndOpCtx(harnessHelper.get(), "reader");
        ru1 = checked_cast<WiredTigerRecoveryUnit*>(
            shard_role_details::getRecoveryUnit(clientAndCtx1.second.get()));
        ru1->setOperationContext(clientAndCtx1.second.get());
        ru2 = checked_cast<WiredTigerRecoveryUnit*>(
            shard_role_details::getRecoveryUnit(clientAndCtx2.second.get()));
        ru2->setOperationContext(clientAndCtx2.second.get());
        snapshotManager = static_cast<WiredTigerSnapshotManager*>(
            harnessHelper->getEngine()->getSnapshotManager());
    }

    std::unique_ptr<WiredTigerRecoveryUnitHarnessHelper> harnessHelper;
    ClientAndCtx clientAndCtx1, clientAndCtx2;
    WiredTigerRecoveryUnit *ru1, *ru2;
    WiredTigerSnapshotManager* snapshotManager;

private:
    const char* wt_uri = "table:prepare_transaction";
    const char* wt_config = "key_format=S,value_format=S,log=(enabled=false)";
};

TEST_F(WiredTigerRecoveryUnitTestFixture, SetReadSource) {
    ru1->setTimestampReadSource(RecoveryUnit::ReadSource::kProvided, Timestamp(1, 1));
    ASSERT_EQ(RecoveryUnit::ReadSource::kProvided, ru1->getTimestampReadSource());
    ASSERT_EQ(Timestamp(1, 1), ru1->getPointInTimeReadTimestamp());
}

TEST_F(WiredTigerRecoveryUnitTestFixture, NoOverlapReadSource) {
    OperationContext* opCtx1 = clientAndCtx1.second.get();
    OperationContext* opCtx2 = clientAndCtx2.second.get();

    std::unique_ptr<RecordStore> rs(harnessHelper->createRecordStore(opCtx1, "a.b"));

    const std::string str = str::stream() << "test";
    const Timestamp ts1{1, 1};
    const Timestamp ts2{1, 2};
    const Timestamp ts3{1, 2};

    RecordId rid1;
    {
        StorageWriteTransaction txn(*ru1);
        StatusWith<RecordId> res = rs->insertRecord(opCtx1, str.c_str(), str.size() + 1, ts1);
        ASSERT_OK(res);
        txn.commit();
        rid1 = res.getValue();
        snapshotManager->setLastApplied(ts1);
    }

    // Read without a timestamp. The write should be visible.
    ASSERT_EQ(ru1->getTimestampReadSource(), RecoveryUnit::ReadSource::kNoTimestamp);
    RecordData unused;
    ASSERT_TRUE(rs->findRecord(opCtx1, rid1, &unused));

    // Read with kNoOverlap. The write should be visible.
    ru1->abandonSnapshot();
    ru1->setTimestampReadSource(RecoveryUnit::ReadSource::kNoOverlap);
    ASSERT_TRUE(rs->findRecord(opCtx1, rid1, &unused));

    RecordId rid2, rid3;
    {
        // Start, but do not commit a transaction with opCtx2. This sets a timestamp at ts2, which
        // creates a hole. kNoOverlap, which is a function of all_durable, will only be able to read
        // at the time immediately before.
        StorageWriteTransaction txn(*ru2);
        StatusWith<RecordId> res =
            rs->insertRecord(opCtx2, str.c_str(), str.size() + 1, Timestamp());
        ASSERT_OK(ru2->setTimestamp(ts2));
        ASSERT_OK(res);
        rid2 = res.getValue();

        // While holding open a transaction with opCtx2, perform an insert at ts3 with opCtx1. This
        // creates a "hole".
        {
            StorageWriteTransaction txn(*ru1);
            StatusWith<RecordId> res = rs->insertRecord(opCtx1, str.c_str(), str.size() + 1, ts3);
            ASSERT_OK(res);
            txn.commit();
            rid3 = res.getValue();
            snapshotManager->setLastApplied(ts3);
        }

        // Read without a timestamp, and we should see the first and third records.
        ru1->abandonSnapshot();
        ru1->setTimestampReadSource(RecoveryUnit::ReadSource::kNoTimestamp);
        ASSERT_TRUE(rs->findRecord(opCtx1, rid1, &unused));
        ASSERT_FALSE(rs->findRecord(opCtx1, rid2, &unused));
        ASSERT_TRUE(rs->findRecord(opCtx1, rid3, &unused));

        // Now read at kNoOverlap. Since the transaction at ts2 has not committed, all_durable is
        // held back to ts1. LastApplied has advanced to ts3, but because kNoOverlap is the minimum,
        // we should only see one record.
        ru1->abandonSnapshot();
        ru1->setTimestampReadSource(RecoveryUnit::ReadSource::kNoOverlap);
        ASSERT_TRUE(rs->findRecord(opCtx1, rid1, &unused));
        ASSERT_FALSE(rs->findRecord(opCtx1, rid2, &unused));
        ASSERT_FALSE(rs->findRecord(opCtx1, rid3, &unused));

        txn.commit();
    }

    // Now that the hole has been closed, kNoOverlap should see all 3 records.
    ru1->abandonSnapshot();
    ru1->setTimestampReadSource(RecoveryUnit::ReadSource::kNoOverlap);
    ASSERT_TRUE(rs->findRecord(opCtx1, rid1, &unused));
    ASSERT_TRUE(rs->findRecord(opCtx1, rid2, &unused));
    ASSERT_TRUE(rs->findRecord(opCtx1, rid3, &unused));
}

TEST_F(WiredTigerRecoveryUnitTestFixture,
       LocalReadOnADocumentBeingPreparedWithoutIgnoringPreparedTriggersPrepareConflict) {
    // Prepare but don't commit a transaction
    ru1->beginUnitOfWork(clientAndCtx1.second->readOnly());
    WT_CURSOR* cursor;
    getCursor(ru1, &cursor);
    cursor->set_key(cursor, "key");
    cursor->set_value(cursor, "value");
    invariantWTOK(wiredTigerCursorInsert(*ru1, cursor), cursor->session);
    ru1->setPrepareTimestamp({1, 1});
    ru1->prepareUnitOfWork();

    // The transaction read default enforces prepare conflicts and triggers a WT_PREPARE_CONFLICT.
    ru2->beginUnitOfWork(clientAndCtx2.second->readOnly());
    getCursor(ru2, &cursor);
    cursor->set_key(cursor, "key");
    int ret = cursor->search(cursor);
    ASSERT_EQ(WT_PREPARE_CONFLICT, ret);

    ru1->abortUnitOfWork();
    ru2->abortUnitOfWork();
}

TEST_F(WiredTigerRecoveryUnitTestFixture,
       LocalReadsOnADocumentBeingPreparedDontTriggerPrepareConflict) {
    // Prepare but don't commit a transaction
    ru1->beginUnitOfWork(clientAndCtx1.second->readOnly());
    WT_CURSOR* cursor;
    getCursor(ru1, &cursor);
    cursor->set_key(cursor, "key");
    cursor->set_value(cursor, "value");
    invariantWTOK(wiredTigerCursorInsert(*ru1, cursor), cursor->session);
    ru1->setPrepareTimestamp({1, 1});
    ru1->prepareUnitOfWork();

    // A transaction that chooses to ignore prepare conflicts does not see the record instead of
    // returning a prepare conflict.
    ru2->beginUnitOfWork(clientAndCtx2.second->readOnly());
    ru2->setPrepareConflictBehavior(PrepareConflictBehavior::kIgnoreConflicts);
    getCursor(ru2, &cursor);
    cursor->set_key(cursor, "key");
    int ret = cursor->search(cursor);
    ASSERT_EQ(WT_NOTFOUND, ret);

    ru1->abortUnitOfWork();
    ru2->abortUnitOfWork();
}

TEST_F(WiredTigerRecoveryUnitTestFixture, WriteAllowedWhileIgnorePrepareFalse) {
    // Prepare but don't commit a transaction
    ru1->beginUnitOfWork(clientAndCtx1.second->readOnly());
    WT_CURSOR* cursor;
    getCursor(ru1, &cursor);
    cursor->set_key(cursor, "key1");
    cursor->set_value(cursor, "value1");
    invariantWTOK(wiredTigerCursorInsert(*ru1, cursor), cursor->session);
    ru1->setPrepareTimestamp({1, 1});
    ru1->prepareUnitOfWork();

    // A transaction that chooses to ignore prepare conflicts with kIgnoreConflictsAllowWrites does
    // not see the record
    ru2->beginUnitOfWork(clientAndCtx2.second->readOnly());
    ru2->setPrepareConflictBehavior(PrepareConflictBehavior::kIgnoreConflictsAllowWrites);

    // The prepared write is not visible.
    getCursor(ru2, &cursor);
    cursor->set_key(cursor, "key1");
    ASSERT_EQ(WT_NOTFOUND, cursor->search(cursor));

    getCursor(ru2, &cursor);
    cursor->set_key(cursor, "key2");
    cursor->set_value(cursor, "value2");

    // The write is allowed.
    invariantWTOK(wiredTigerCursorInsert(*ru1, cursor), cursor->session);

    ru1->abortUnitOfWork();
    ru2->abortUnitOfWork();
}

TEST_F(WiredTigerRecoveryUnitTestFixture, WriteOnADocumentBeingPreparedTriggersWTRollback) {
    // Prepare but don't commit a transaction
    ru1->beginUnitOfWork(clientAndCtx1.second->readOnly());
    WT_CURSOR* cursor;
    getCursor(ru1, &cursor);
    cursor->set_key(cursor, "key");
    cursor->set_value(cursor, "value");
    invariantWTOK(wiredTigerCursorInsert(*ru1, cursor), cursor->session);
    ru1->setPrepareTimestamp({1, 1});
    ru1->prepareUnitOfWork();

    // Another transaction with write triggers WT_ROLLBACK
    ru2->beginUnitOfWork(clientAndCtx2.second->readOnly());
    getCursor(ru2, &cursor);
    cursor->set_key(cursor, "key");
    cursor->set_value(cursor, "value2");
    int ret = wiredTigerCursorInsert(*ru1, cursor);
    ASSERT_EQ(WT_ROLLBACK, ret);

    ru1->abortUnitOfWork();
    ru2->abortUnitOfWork();
}

DEATH_TEST_REGEX_F(WiredTigerRecoveryUnitTestFixture,
                   PrepareTimestampOlderThanStableTimestamp,
                   "prepare timestamp .* is not newer than the stable timestamp") {
    ru1->beginUnitOfWork(clientAndCtx1.second->readOnly());
    harnessHelper->getEngine()->setStableTimestamp({2, 1}, false);
    ru1->setPrepareTimestamp({1, 1});
    // It is illegal to set the prepare timestamp older than the stable timestamp.
    ru1->prepareUnitOfWork();
}

DEATH_TEST_REGEX_F(WiredTigerRecoveryUnitTestFixture,
                   CommitTimestampOlderThanPrepareTimestamp,
                   "commit timestamp .* is less than the prepare timestamp") {
    ru1->beginUnitOfWork(clientAndCtx1.second->readOnly());
    ru1->setDurableTimestamp({4, 1});  // Newer than the prepare timestamp.
    harnessHelper->getEngine()->setStableTimestamp({2, 1}, false);
    ru1->setPrepareTimestamp({3, 1});  // Newer than the stable timestamp.
    ru1->prepareUnitOfWork();
    ru1->setCommitTimestamp({1, 1});
    // It is illegal to set the commit timestamp older than the prepare timestamp.
    ru1->commitUnitOfWork();
}

TEST_F(WiredTigerRecoveryUnitTestFixture, RoundUpPreparedTimestamps) {
    ru1->beginUnitOfWork(clientAndCtx1.second->readOnly());
    RecoveryUnit::OpenSnapshotOptions roundUp{.roundUpPreparedTimestamps = true};
    ru1->preallocateSnapshot(roundUp);
    ru1->setDurableTimestamp({4, 1});
    harnessHelper->getEngine()->setStableTimestamp({3, 1}, false);
    // Check setting a prepared transaction timestamp earlier than the
    // stable timestamp is valid with roundUpPreparedTimestamps option.
    ru1->setPrepareTimestamp({2, 1});
    ru1->prepareUnitOfWork();
    // Check setting a commit timestamp earlier than the prepared transaction
    // timestamp is valid with roundUpPreparedTimestamps option.
    ru1->setCommitTimestamp({1, 1});
    ru1->commitUnitOfWork();
}

TEST_F(WiredTigerRecoveryUnitTestFixture,
       ChangeIsPassedEmptyLastTimestampSetOnCommitWithNoTimestamp) {
    boost::optional<Timestamp> commitTs = boost::none;

    {
        StorageWriteTransaction txn(*ru1);
        ru1->onCommit([&](OperationContext*, boost::optional<Timestamp> commitTime) {
            commitTs = commitTime;
        });
        txn.commit();
    }
    ASSERT(!commitTs);
}

TEST_F(WiredTigerRecoveryUnitTestFixture, ChangeIsPassedLastTimestampSetOnCommit) {
    boost::optional<Timestamp> commitTs = boost::none;

    Timestamp ts1(5, 5);
    Timestamp ts2(6, 6);
    {
        StorageWriteTransaction txn(*ru1);
        ru1->onCommit([&](OperationContext*, boost::optional<Timestamp> commitTime) {
            commitTs = commitTime;
        });
        ASSERT_OK(ru1->setTimestamp(ts1));
        ASSERT(!commitTs);
        ASSERT_OK(ru1->setTimestamp(ts2));
        ASSERT(!commitTs);
        ASSERT_OK(ru1->setTimestamp(ts1));
        ASSERT(!commitTs);
        txn.commit();
        ASSERT_EQ(*commitTs, ts1);
    }
    ASSERT_EQ(*commitTs, ts1);
}

TEST_F(WiredTigerRecoveryUnitTestFixture, ChangeIsNotPassedLastTimestampSetOnAbort) {
    boost::optional<Timestamp> commitTs = boost::none;

    Timestamp ts1(5, 5);
    {
        StorageWriteTransaction txn(*ru1);
        ru1->onCommit([&](OperationContext*, boost::optional<Timestamp> commitTime) {
            commitTs = commitTime;
        });
        ASSERT_OK(ru1->setTimestamp(ts1));
        ASSERT(!commitTs);
    }
    ASSERT(!commitTs);
}

TEST_F(WiredTigerRecoveryUnitTestFixture, ChangeIsPassedCommitTimestamp) {
    boost::optional<Timestamp> commitTs = boost::none;
    Timestamp ts1(5, 5);

    ru1->setCommitTimestamp(ts1);
    ASSERT(!commitTs);

    {
        StorageWriteTransaction txn(*ru1);
        ru1->onCommit([&](OperationContext*, boost::optional<Timestamp> commitTime) {
            commitTs = commitTime;
        });
        ASSERT(!commitTs);
        txn.commit();
        ASSERT_EQ(*commitTs, ts1);
    }
    ASSERT_EQ(*commitTs, ts1);
}

TEST_F(WiredTigerRecoveryUnitTestFixture, ChangeIsNotPassedCommitTimestampIfCleared) {
    boost::optional<Timestamp> commitTs = boost::none;

    Timestamp ts1(5, 5);

    ru1->setCommitTimestamp(ts1);
    ASSERT(!commitTs);
    ru1->clearCommitTimestamp();
    ASSERT(!commitTs);

    {
        StorageWriteTransaction txn(*ru1);
        ru1->onCommit([&](OperationContext*, boost::optional<Timestamp> commitTime) {
            commitTs = commitTime;
        });
        ASSERT(!commitTs);
        txn.commit();
    }
    ASSERT(!commitTs);
}

TEST_F(WiredTigerRecoveryUnitTestFixture, ChangeIsPassedNewestCommitTimestamp) {
    boost::optional<Timestamp> commitTs = boost::none;

    Timestamp ts1(5, 5);
    Timestamp ts2(6, 6);

    ru1->setCommitTimestamp(ts2);
    ASSERT(!commitTs);
    ru1->clearCommitTimestamp();
    ASSERT(!commitTs);
    ru1->setCommitTimestamp(ts1);
    ASSERT(!commitTs);

    {
        StorageWriteTransaction txn(*ru1);
        ru1->onCommit([&](OperationContext*, boost::optional<Timestamp> commitTime) {
            commitTs = commitTime;
        });
        ASSERT(!commitTs);
        txn.commit();
        ASSERT_EQ(*commitTs, ts1);
    }
    ASSERT_EQ(*commitTs, ts1);
}

TEST_F(WiredTigerRecoveryUnitTestFixture, ChangeIsNotPassedCommitTimestampOnAbort) {
    boost::optional<Timestamp> commitTs = boost::none;

    Timestamp ts1(5, 5);

    ru1->setCommitTimestamp(ts1);
    ASSERT(!commitTs);

    {
        StorageWriteTransaction txn(*ru1);
        ru1->onCommit([&](OperationContext*, boost::optional<Timestamp> commitTime) {
            commitTs = commitTime;
        });
        ASSERT(!commitTs);
    }
    ASSERT(!commitTs);
}

TEST_F(WiredTigerRecoveryUnitTestFixture, CommitTimestampBeforeSetTimestampOnCommit) {
    boost::optional<Timestamp> commitTs = boost::none;

    Timestamp ts1(5, 5);
    Timestamp ts2(6, 6);

    ru1->setCommitTimestamp(ts2);
    ASSERT(!commitTs);

    {
        StorageWriteTransaction txn(*ru1);
        ru1->onCommit([&](OperationContext*, boost::optional<Timestamp> commitTime) {
            commitTs = commitTime;
        });
        ASSERT(!commitTs);
        txn.commit();
        ASSERT_EQ(*commitTs, ts2);
    }
    ASSERT_EQ(*commitTs, ts2);
    ru1->clearCommitTimestamp();

    {
        StorageWriteTransaction txn(*ru1);
        ru1->onCommit([&](OperationContext*, boost::optional<Timestamp> commitTime) {
            commitTs = commitTime;
        });
        ASSERT_OK(ru1->setTimestamp(ts1));
        ASSERT_EQ(*commitTs, ts2);
        txn.commit();
        ASSERT_EQ(*commitTs, ts1);
    }
    ASSERT_EQ(*commitTs, ts1);
}

TEST_F(WiredTigerRecoveryUnitTestFixture, CommitTimestampAfterSetTimestampOnCommit) {
    boost::optional<Timestamp> commitTs = boost::none;

    Timestamp ts1(5, 5);
    Timestamp ts2(6, 6);

    {
        StorageWriteTransaction txn(*ru1);
        ru1->onCommit([&](OperationContext*, boost::optional<Timestamp> commitTime) {
            commitTs = commitTime;
        });
        ASSERT(!commitTs);
        ASSERT_OK(ru1->setTimestamp(ts2));
        ASSERT(!commitTs);
        txn.commit();
        ASSERT_EQ(*commitTs, ts2);
    }
    ASSERT_EQ(*commitTs, ts2);

    ru1->setCommitTimestamp(ts1);
    ASSERT_EQ(*commitTs, ts2);

    {
        StorageWriteTransaction txn(*ru1);
        ru1->onCommit([&](OperationContext*, boost::optional<Timestamp> commitTime) {
            commitTs = commitTime;
        });
        ASSERT_EQ(*commitTs, ts2);
        txn.commit();
        ASSERT_EQ(*commitTs, ts1);
    }
    ASSERT_EQ(*commitTs, ts1);
}

TEST_F(WiredTigerRecoveryUnitTestFixture, CommitTimestampBeforeSetTimestampOnAbort) {
    boost::optional<Timestamp> commitTs = boost::none;

    Timestamp ts1(5, 5);
    Timestamp ts2(6, 6);

    ru1->setCommitTimestamp(ts2);
    ASSERT(!commitTs);

    {
        StorageWriteTransaction txn(*ru1);
        ru1->onCommit([&](OperationContext*, boost::optional<Timestamp> commitTime) {
            commitTs = commitTime;
        });
        ASSERT(!commitTs);
    }
    ASSERT(!commitTs);
    ru1->clearCommitTimestamp();

    {
        StorageWriteTransaction txn(*ru1);
        ru1->onCommit([&](OperationContext*, boost::optional<Timestamp> commitTime) {
            commitTs = commitTime;
        });
        ASSERT_OK(ru1->setTimestamp(ts1));
        ASSERT(!commitTs);
    }
    ASSERT(!commitTs);
}

TEST_F(WiredTigerRecoveryUnitTestFixture, CommitTimestampAfterSetTimestampOnAbort) {
    boost::optional<Timestamp> commitTs = boost::none;

    Timestamp ts1(5, 5);
    Timestamp ts2(6, 6);

    {
        StorageWriteTransaction txn(*ru1);
        ru1->onCommit([&](OperationContext*, boost::optional<Timestamp> commitTime) {
            commitTs = commitTime;
        });
        ASSERT(!commitTs);
        ASSERT_OK(ru1->setTimestamp(ts2));
        ASSERT(!commitTs);
    }
    ASSERT(!commitTs);

    ru1->setCommitTimestamp(ts1);
    ASSERT(!commitTs);

    {
        StorageWriteTransaction txn(*ru1);
        ru1->onCommit([&](OperationContext*, boost::optional<Timestamp> commitTime) {
            commitTs = commitTime;
        });
        ASSERT(!commitTs);
    }
    ASSERT(!commitTs);
}

TEST_F(WiredTigerRecoveryUnitTestFixture, CheckpointCursorsAreNotCached) {
    auto opCtx = clientAndCtx1.second.get();

    std::unique_ptr<RecordStore> rs(
        harnessHelper->createRecordStore(opCtx, "test.checkpoint_cached"));
    auto uri = static_cast<WiredTigerRecordStore*>(rs.get())->getURI();

    WiredTigerKVEngine* engine = harnessHelper->getEngine();

    // Insert a record.
    StorageWriteTransaction txn(*ru1);
    StatusWith<RecordId> s = rs->insertRecord(opCtx, "data", 4, Timestamp());
    ASSERT_TRUE(s.isOK());
    ASSERT_EQUALS(1, rs->numRecords());
    txn.commit();

    // Test 1: A normal read should create a new cursor and release it into the session cache.

    // Close all cached cursors to establish a 'before' state.
    ru1->getSession()->closeAllCursors(uri);
    int cachedCursorsBefore = ru1->getSession()->cachedCursors();

    RecordData rd;
    ASSERT_TRUE(rs->findRecord(opCtx, s.getValue(), &rd));

    // A cursor should have been checked out and released into the cache.
    ASSERT_GT(ru1->getSession()->cachedCursors(), cachedCursorsBefore);
    // All opened cursors are returned.
    ASSERT_EQ(0, ru1->getSession()->cursorsOut());

    ru1->abandonSnapshot();

    // Force a checkpoint.
    engine->flushAllFiles(opCtx, /*callerHoldsReadLock*/ false);

    // Test 2: Checkpoint cursors are not expected to be cached, they
    // should be immediately closed when destructed.
    ru1->setTimestampReadSource(WiredTigerRecoveryUnit::ReadSource::kCheckpoint);

    // Close any cached cursors to establish a new 'before' state.
    ru1->getSession()->closeAllCursors(uri);
    cachedCursorsBefore = ru1->getSession()->cachedCursors();

    // Will search the checkpoint cursor for the record, then release the checkpoint cursor.
    ASSERT_TRUE(rs->findRecord(opCtx, s.getValue(), &rd));

    // No new cursors should have been released into the cache, with the exception of a metadata
    // cursor that is opened to determine if the table is LSM. Metadata cursors are cached.
    ASSERT_EQ(ru1->getSession()->cachedCursors(), cachedCursorsBefore + 1);

    // All opened cursors are closed.
    ASSERT_EQ(0, ru1->getSession()->cursorsOut());

    ASSERT_EQ(ru1->getTimestampReadSource(), WiredTigerRecoveryUnit::ReadSource::kCheckpoint);
}

TEST_F(WiredTigerRecoveryUnitTestFixture, StorageStatsSubsequentTransactions) {
    auto opCtx = clientAndCtx1.second.get();

    std::unique_ptr<RecordStore> rs(harnessHelper->createRecordStore(opCtx, "test.storage_stats"));
    auto uri = static_cast<WiredTigerRecordStore*>(rs.get())->getURI();

    // Insert a record.
    StorageWriteTransaction txn1(*ru1);
    StatusWith<RecordId> s = rs->insertRecord(opCtx, "rec1", 4, Timestamp());
    ASSERT_TRUE(s.isOK());
    ASSERT_EQUALS(1, rs->numRecords());

    // Checking the storage stats
    auto storageStats = ru1->computeOperationStatisticsSinceLastCall();
    WiredTigerStats* wtStats = dynamic_cast<WiredTigerStats*>(storageStats.get());
    ASSERT_TRUE(wtStats != nullptr);

    // txnDirtyBytes should be greater than zero since there is uncommitted data on the transaction
    ASSERT_GT(wtStats->txnBytesDirty(), 0);

    txn1.commit();

    // A new transaction will reset stats
    StorageWriteTransaction txn2(*ru1);
    // The transaction won't actually start until the session is accessed
    ru1->getSession();

    storageStats = ru1->computeOperationStatisticsSinceLastCall();
    wtStats = dynamic_cast<WiredTigerStats*>(storageStats.get());
    ASSERT_TRUE(wtStats != nullptr);

    // txnDirtyBytes should be zero since transaction was just restarted
    ASSERT_EQUALS(wtStats->txnBytesDirty(), 0);
    txn2.abort();
}

TEST_F(WiredTigerRecoveryUnitTestFixture, ReadOnceCursorsCached) {
    auto opCtx = clientAndCtx1.second.get();

    std::unique_ptr<RecordStore> rs(harnessHelper->createRecordStore(opCtx, "test.read_once"));
    auto uri = static_cast<WiredTigerRecordStore*>(rs.get())->getURI();

    // Insert a record.
    StorageWriteTransaction txn(*ru1);
    StatusWith<RecordId> s = rs->insertRecord(opCtx, "data", 4, Timestamp());
    ASSERT_TRUE(s.isOK());
    ASSERT_EQUALS(1, rs->numRecords());
    txn.commit();

    // Test 1: A normal read should create a new cursor and release it into the session cache.

    // Close all cached cursors to establish a 'before' state.
    ru1->getSession()->closeAllCursors(uri);
    int cachedCursorsBefore = ru1->getSession()->cachedCursors();

    RecordData rd;
    ASSERT_TRUE(rs->findRecord(opCtx, s.getValue(), &rd));

    // A cursor should have been checked out and released into the cache.
    ASSERT_GT(ru1->getSession()->cachedCursors(), cachedCursorsBefore);
    // All opened cursors are returned.
    ASSERT_EQ(0, ru1->getSession()->cursorsOut());

    ru1->abandonSnapshot();

    // Test 2: A read-once operation should create a new cursor because it has a different
    // configuration. This will be released into the cache.

    ru1->setReadOnce(true);

    // Close any cached cursors to establish a new 'before' state.
    ru1->getSession()->closeAllCursors(uri);
    cachedCursorsBefore = ru1->getSession()->cachedCursors();

    // The subsequent read operation will create a new read_once cursor and release into the cache.
    ASSERT_TRUE(rs->findRecord(opCtx, s.getValue(), &rd));

    // A new cursor should have been released into the cache.
    ASSERT_GT(ru1->getSession()->cachedCursors(), cachedCursorsBefore);
    // All opened cursors are closed.
    ASSERT_EQ(0, ru1->getSession()->cursorsOut());

    ASSERT(ru1->getReadOnce());
}

TEST_F(WiredTigerRecoveryUnitTestFixture, CheckpointCursorNotChanged) {
    auto opCtx1 = clientAndCtx1.second.get();
    auto opCtx2 = clientAndCtx2.second.get();

    std::unique_ptr<RecordStore> rs(
        harnessHelper->createRecordStore(opCtx1, "test.checkpoint_stable"));

    WiredTigerKVEngine* engine = harnessHelper->getEngine();

    // Insert a record.
    RecordId rid1;
    {
        StorageWriteTransaction txn(*ru1);
        StatusWith<RecordId> s1 = rs->insertRecord(opCtx1, "data", 4, Timestamp());
        ASSERT_TRUE(s1.isOK());
        ASSERT_EQUALS(1, rs->numRecords());
        rid1 = s1.getValue();
        txn.commit();
    }
    // Force a checkpoint.
    engine->flushAllFiles(opCtx1, /*callerHoldsReadLock*/ false);

    // Test 1: Open a checkpoint cursor and ensure it has the first record.
    ru2->setTimestampReadSource(WiredTigerRecoveryUnit::ReadSource::kCheckpoint);
    auto originalCheckpointCursor = rs->getCursor(opCtx2, true);
    ASSERT(originalCheckpointCursor->seekExact(rid1));

    // Insert a new record.
    RecordId rid2;
    {
        StorageWriteTransaction txn(*ru1);
        StatusWith<RecordId> s2 = rs->insertRecord(opCtx1, "data_2", 6, Timestamp());
        ASSERT_TRUE(s2.isOK());
        ASSERT_EQUALS(2, rs->numRecords());
        rid2 = s2.getValue();
        txn.commit();
    }

    // Test 2: New record does not appear in original checkpoint cursor.
    ASSERT(!originalCheckpointCursor->seekExact(rid2));
    ASSERT(originalCheckpointCursor->seekExact(rid1));

    // Test 3: New record does not appear in new checkpoint cursor since no new checkpoint was
    // created.
    ru1->setTimestampReadSource(WiredTigerRecoveryUnit::ReadSource::kCheckpoint);
    auto checkpointCursor = rs->getCursor(opCtx1, true);
    ASSERT(!checkpointCursor->seekExact(rid2));

    // Force a checkpoint.
    engine->flushAllFiles(opCtx1, /*callerHoldsReadLock*/ false);

    // Test 4: Old and new record should appear in new checkpoint cursor. Only old record
    // should appear in the original checkpoint cursor
    ru1->setTimestampReadSource(WiredTigerRecoveryUnit::ReadSource::kCheckpoint);
    auto newCheckpointCursor = rs->getCursor(opCtx1, true);
    ASSERT(newCheckpointCursor->seekExact(rid1));
    ASSERT(newCheckpointCursor->seekExact(rid2));
    ASSERT(originalCheckpointCursor->seekExact(rid1));
    ASSERT(!originalCheckpointCursor->seekExact(rid2));
}

TEST_F(WiredTigerRecoveryUnitTestFixture, CheckpointCursorGetId) {
    auto opCtx1 = clientAndCtx1.second.get();
    auto opCtx2 = clientAndCtx2.second.get();

    std::unique_ptr<RecordStore> rs(harnessHelper->createRecordStore(opCtx1, "test.checkpoint_id"));

    WiredTigerKVEngine* engine = harnessHelper->getEngine();

    // Force a checkpoint.
    engine->flushAllFiles(opCtx1, /*callerHoldsReadLock*/ false);

    // Open a checkpoint cursor and check its id.
    ru2->setTimestampReadSource(WiredTigerRecoveryUnit::ReadSource::kCheckpoint);
    auto originalCheckpointCursor = rs->getCursor(opCtx2, true);
    auto firstCheckpointId = originalCheckpointCursor->getCheckpointId();
    ASSERT(firstCheckpointId > 0);

    // Insert a record and force a checkpoint.
    RecordId rid1;
    {
        StorageWriteTransaction txn(*ru1);
        StatusWith<RecordId> s1 = rs->insertRecord(opCtx1, "data", 4, Timestamp());
        ASSERT_TRUE(s1.isOK());
        ASSERT_EQUALS(1, rs->numRecords());
        rid1 = s1.getValue();
        txn.commit();
    }
    engine->flushAllFiles(opCtx1, /*callerHoldsReadLock*/ false);

    // Open another checkpoint cursor and check its new id.
    ru1->setTimestampReadSource(WiredTigerRecoveryUnit::ReadSource::kCheckpoint);
    auto newCheckpointCursor = rs->getCursor(opCtx1, true);
    ASSERT(newCheckpointCursor->getCheckpointId() > firstCheckpointId);
}

TEST_F(WiredTigerRecoveryUnitTestFixture, CommitWithDurableTimestamp) {
    Timestamp ts1(3, 3);
    Timestamp ts2(5, 5);

    ru1->setCommitTimestamp(ts1);
    ru1->setDurableTimestamp(ts2);
    auto durableTs = ru1->getDurableTimestamp();
    ASSERT_EQ(ts2, durableTs);

    {
        StorageWriteTransaction txn(*ru1);
        txn.commit();
    }
}

TEST_F(WiredTigerRecoveryUnitTestFixture, CommitWithoutDurableTimestamp) {
    Timestamp ts1(5, 5);
    ru1->setCommitTimestamp(ts1);

    {
        StorageWriteTransaction txn(*ru1);
        txn.commit();
    }
}

TEST_F(WiredTigerRecoveryUnitTestFixture, MultiTimestampConstraintsInternalState) {
    Timestamp ts1(1, 1);
    Timestamp ts2(2, 2);

    OperationContext* opCtx = clientAndCtx1.second.get();
    ru1->beginUnitOfWork(opCtx->readOnly());

    // Perform an non timestamped write.
    WT_CURSOR* cursor;
    getCursor(ru1, &cursor);
    cursor->set_key(cursor, "key");
    cursor->set_value(cursor, "value");
    invariantWTOK(wiredTigerCursorInsert(*ru1, cursor), cursor->session);

    // Perform a write at ts1.
    cursor->set_key(cursor, "key2");
    cursor->set_value(cursor, "value");
    ASSERT_OK(ru1->setTimestamp(ts1));
    invariantWTOK(wiredTigerCursorInsert(*ru1, cursor), cursor->session);

    // Setting the timestamp again to the same value should not fail.
    ASSERT_OK(ru1->setTimestamp(ts1));

    // Committing the unit of work should reset the internal state for the multi timestamp
    // constraint checks.
    ru1->commitUnitOfWork();
    ru1->beginUnitOfWork(opCtx->readOnly());

    // Perform a write at ts2.
    cursor->set_key(cursor, "key3");
    cursor->set_value(cursor, "value");
    ASSERT_OK(ru1->setTimestamp(ts2));
    invariantWTOK(wiredTigerCursorInsert(*ru1, cursor), cursor->session);

    ru1->commitUnitOfWork();
}

TEST_F(WiredTigerRecoveryUnitTestFixture, AbandonSnapshotAbortMode) {
    ru1->setAbandonSnapshotMode(RecoveryUnit::AbandonSnapshotMode::kAbort);

    OperationContext* opCtx = clientAndCtx1.second.get();
    const char* const key = "key";

    {
        ru1->beginUnitOfWork(opCtx->readOnly());

        WT_CURSOR* cursor;
        getCursor(ru1, &cursor);
        cursor->set_key(cursor, key);
        cursor->set_value(cursor, "value");
        invariantWTOK(wiredTigerCursorInsert(*ru1, cursor), cursor->session);

        ru1->commitUnitOfWork();
    }

    // Create a cursor. We will check that once positioned, the cursor is reset by a call to
    // abandonSnapshot() on the associated RecoveryUnit.
    WT_CURSOR* cursor;
    getCursor(ru1, &cursor);
    cursor->set_key(cursor, key);
    ASSERT_EQ(0, cursor->search(cursor));

    ru1->abandonSnapshot();

    // The WT transaction should have been aborted and the cursor reset.

    // Advancing to the "next" record now that the cursor has been reset should give us the first
    // record again.
    ASSERT_EQ(0, cursor->next(cursor));

    const char* returnedKey = nullptr;
    ASSERT_EQ(0, cursor->get_key(cursor, &returnedKey));
    ASSERT_EQ(0, strncmp(key, returnedKey, strlen(key)));
}

// Validate the return of mdb_handle_general when killing an opCtx with the RU configured to cancel
// cache eviction. While there is gating in place, ensure that the gating fully disables the
// feature
TEST_F(WiredTigerRecoveryUnitTestFixture, OptionalEvictionCanBeInterrupted) {
    for (bool enableFeature : {false, true}) {
        RAIIServerParameterControllerForTest featureFlag{"featureFlagStorageEngineInterruptibility",
                                                         enableFeature};
        auto clientAndCtx =
            makeClientAndOpCtx(harnessHelper.get(), "test" + std::to_string(enableFeature));
        OperationContext* opCtx = clientAndCtx.second.get();
        auto ru = WiredTigerRecoveryUnit::get(shard_role_details::getRecoveryUnit(opCtx));

        WiredTigerEventHandler eventHandler;
        WT_SESSION* session = ru->getSessionNoTxn()->with([](WT_SESSION* arg) { return arg; });
        ASSERT_EQ(0,
                  eventHandler.getWtEventHandler()->handle_general(eventHandler.getWtEventHandler(),
                                                                   ru->getConnection()->conn(),
                                                                   session,
                                                                   WT_EVENT_EVICTION,
                                                                   nullptr));

        opCtx->markKilled(ErrorCodes::Interrupted);
        ASSERT_EQ(
            enableFeature,
            (bool)eventHandler.getWtEventHandler()->handle_general(eventHandler.getWtEventHandler(),
                                                                   ru->getConnection()->conn(),
                                                                   session,
                                                                   WT_EVENT_EVICTION,
                                                                   nullptr));

        if (enableFeature) {
            ASSERT_EQ(WiredTigerUtil::getCancelledCacheMetric_forTest(), 1);
        } else {
            ASSERT_EQ(WiredTigerUtil::getCancelledCacheMetric_forTest(), 0);
        }
    }
}

class SnapshotTestDecoration {
public:
    void hit() {
        _hits++;
    }

    int getHits() {
        return _hits;
    }

private:
    int _hits = 0;
};

const RecoveryUnit::Snapshot::Decoration<SnapshotTestDecoration> getSnapshotDecoration =
    RecoveryUnit::Snapshot::declareDecoration<SnapshotTestDecoration>();

TEST_F(WiredTigerRecoveryUnitTestFixture, AbandonSnapshotChange) {
    ASSERT(ru1->getSession());

    getSnapshotDecoration(ru1->getSnapshot()).hit();
    ASSERT_EQ(1, getSnapshotDecoration(ru1->getSnapshot()).getHits());

    ru1->abandonSnapshot();

    // A snapshot is closed, reconstructing our decoration.
    ASSERT_EQ(0, getSnapshotDecoration(ru1->getSnapshot()).getHits());
}

TEST_F(WiredTigerRecoveryUnitTestFixture, CommitSnapshotChange) {
    ru1->beginUnitOfWork(/*readOnly=*/false);

    getSnapshotDecoration(ru1->getSnapshot()).hit();
    ASSERT_EQ(1, getSnapshotDecoration(ru1->getSnapshot()).getHits());

    ASSERT(ru1->getSession());

    ASSERT_EQ(1, getSnapshotDecoration(ru1->getSnapshot()).getHits());

    ru1->commitUnitOfWork();

    // A snapshot is closed, reconstructing our decoration.
    ASSERT_EQ(0, getSnapshotDecoration(ru1->getSnapshot()).getHits());
}

TEST_F(WiredTigerRecoveryUnitTestFixture, AbortSnapshotChange) {
    // A snapshot is already open from when the RU was constructed.
    ASSERT(ru1->getSession());
    getSnapshotDecoration(ru1->getSnapshot()).hit();
    ASSERT_EQ(1, getSnapshotDecoration(ru1->getSnapshot()).getHits());

    ru1->beginUnitOfWork(/*readOnly=*/false);
    ASSERT_EQ(1, getSnapshotDecoration(ru1->getSnapshot()).getHits());

    ru1->abortUnitOfWork();

    // A snapshot is closed, reconstructing our decoration.
    ASSERT_EQ(0, getSnapshotDecoration(ru1->getSnapshot()).getHits());
}

TEST_F(WiredTigerRecoveryUnitTestFixture, PreparedTransactionSkipsOptionalEviction) {
    RAIIServerParameterControllerForTest truncateFeatureFlag{
        "featureFlagStorageEngineInterruptibility", true};

    // A snapshot is already open from when the RU was constructed.
    ASSERT(ru1->getSession());
    ASSERT(!ru1->getNoEvictionAfterCommitOrRollback());

    // Abort WUOW.
    ru1->beginUnitOfWork(/*readOnly=*/false);
    ASSERT(!ru1->getNoEvictionAfterCommitOrRollback());

    ru1->setPrepareTimestamp({1, 1});
    ru1->prepareUnitOfWork();

    ASSERT(ru1->getNoEvictionAfterCommitOrRollback());

    ru1->abortUnitOfWork();
    ASSERT(!ru1->getNoEvictionAfterCommitOrRollback());

    // Commit WUOW.
    ru1->beginUnitOfWork(/*readOnly=*/false);
    ru1->setDurableTimestamp({1, 1});
    ASSERT(!ru1->getNoEvictionAfterCommitOrRollback());

    ru1->setPrepareTimestamp({1, 1});
    ru1->prepareUnitOfWork();

    ASSERT(ru1->getNoEvictionAfterCommitOrRollback());

    ru1->setCommitTimestamp({1, 1});
    ru1->commitUnitOfWork();
    ASSERT(!ru1->getNoEvictionAfterCommitOrRollback());
}

TEST_F(WiredTigerRecoveryUnitTestFixture, CacheSizeEstimatesTransactionBytes) {
    OperationContext* opCtx = clientAndCtx1.second.get();
    std::unique_ptr<RecordStore> rs(harnessHelper->createRecordStore(opCtx, "test.table"));

    // Check that if we haven't done anything, the txn holds no cache.
    StorageWriteTransaction txn(*ru1);
    ASSERT_EQ(ru1->getCacheDirtyBytes(), 0);

    // Write some data, now the txn is at least as large as the write.
    std::string data(1024, 'a');  // 1MB.
    ASSERT_OK(rs->insertRecord(opCtx, data.c_str(), data.size(), Timestamp(13, 37)));
    ASSERT_GTE(ru1->getCacheDirtyBytes(), data.size());

    // Rolling back returns us to zero.
    txn.abort();
    ASSERT_EQ(ru1->getCacheDirtyBytes(), 0);
}

DEATH_TEST_REGEX_F(WiredTigerRecoveryUnitTestFixture,
                   MultiTimestampConstraints,
                   "Fatal assertion.*4877100") {
    Timestamp ts1(1, 1);
    Timestamp ts2(2, 2);

    OperationContext* opCtx = clientAndCtx1.second.get();
    ru1->beginUnitOfWork(opCtx->readOnly());

    auto writeTest = [&]() {
        // Perform an non timestamped write.
        WT_CURSOR* cursor;
        getCursor(ru1, &cursor);
        cursor->set_key(cursor, "key");
        cursor->set_value(cursor, "value");
        invariantWTOK(wiredTigerCursorInsert(*ru1, cursor), cursor->session);

        // Perform a write at ts1.
        cursor->set_key(cursor, "key2");
        cursor->set_value(cursor, "value");
        ASSERT_OK(ru1->setTimestamp(ts1));
        invariantWTOK(wiredTigerCursorInsert(*ru1, cursor), cursor->session);

        // Setting the timestamp again to a different value should detect that we're trying to set
        // multiple timestamps with the first write being non timestamped.
        ASSERT_OK(ru1->setTimestamp(ts2));
        ru1->commitUnitOfWork();
    };

    try {
        writeTest();
    } catch (StorageUnavailableException const&) {
        // It's expected to get a WCE the first time we try this, due to the multi-timestamp
        // constraint. We'll try again and it will fassert and print out extra debug info.
    }
    writeTest();
}

DEATH_TEST_F(WiredTigerRecoveryUnitTestFixture,
             SetDurableTimestampTwice,
             "Trying to reset durable timestamp when it was already set.") {
    Timestamp ts1(3, 3);
    Timestamp ts2(5, 5);
    ru1->setDurableTimestamp(ts1);
    ru1->setDurableTimestamp(ts2);
}

DEATH_TEST_F(WiredTigerRecoveryUnitTestFixture,
             RollbackHandlerAbortsOnTxnOpen,
             "rollback handler reopened transaction") {
    ASSERT(ru1->getSession());
    {
        StorageWriteTransaction txn(*ru1);
        ru1->assertInActiveTxn();
        ru1->onRollback([&](OperationContext*) { ru1->getSession(); });
    }
}

DEATH_TEST_F(WiredTigerRecoveryUnitTestFixture,
             MayNotChangeReadSourceWhilePinned,
             "Cannot change ReadSource as it is pinned.") {

    ru1->pinReadSource();
    ru1->setTimestampReadSource(RecoveryUnit::ReadSource::kNoOverlap);
}

}  // namespace
}  // namespace mongo
