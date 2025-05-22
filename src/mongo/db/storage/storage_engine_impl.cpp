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


#include "mongo/db/storage/storage_engine_impl.h"

#include <absl/container/node_hash_map.h>
#include <absl/meta/type_traits.h>
#include <algorithm>
#include <boost/container/vector.hpp>
#include <boost/move/utility_core.hpp>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/db/admission/execution_admission_context.h"
#include "mongo/db/catalog/clustered_collection_util.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/catalog/collection_record_store_options.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/client.h"
#include "mongo/db/index/multikey_paths.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/storage/deferred_drop_record_store.h"
#include "mongo/db/storage/durable_catalog.h"
#include "mongo/db/storage/durable_history_pin.h"
#include "mongo/db/storage/ident.h"
#include "mongo/db/storage/kv/kv_engine.h"
#include "mongo/db/storage/record_data.h"
#include "mongo/db/storage/recovery_unit_noop.h"
#include "mongo/db/storage/spill_table.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/db/storage/storage_repair_observer.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/transaction_resources.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/compiler.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/str.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage


#define LOGV2_FOR_RECOVERY(ID, DLEVEL, MESSAGE, ...) \
    LOGV2_DEBUG_OPTIONS(ID, DLEVEL, {logv2::LogComponent::kStorageRecovery}, MESSAGE, ##__VA_ARGS__)

namespace mongo {

using std::string;
using std::vector;

MONGO_FAIL_POINT_DEFINE(failToParseResumeIndexInfo);
MONGO_FAIL_POINT_DEFINE(pauseTimestampMonitor);

namespace {
const std::string kCatalogInfo = DatabaseName::kMdbCatalog.db(omitTenant).toString();
const NamespaceString kCatalogInfoNamespace = NamespaceString(DatabaseName::kMdbCatalog);
const auto kCatalogLogLevel = logv2::LogSeverity::Debug(2);
const auto kResumableIndexIdentStem = "resumable-index-build-"_sd;

// Returns true if the ident refers to a resumable index build table.
bool isResumableIndexBuildIdent(StringData ident) {
    return ident::isInternalIdent(ident, kResumableIndexIdentStem);
}

// Idents corresponding to resumable index build tables are encoded as an 'internal' table and
// tagged 'kResumableIndexIdentStem'.
// Generates an ident to unique identify a new resumable index build table.
std::string generateNewResumableIndexBuildIdent() {
    const auto resumableIndexIdent = ident::generateNewInternalIdent(kResumableIndexIdentStem);
    invariant(isResumableIndexBuildIdent(resumableIndexIdent));
    return resumableIndexIdent;
}
}  // namespace

StorageEngineImpl::StorageEngineImpl(OperationContext* opCtx,
                                     std::unique_ptr<KVEngine> engine,
                                     std::unique_ptr<KVEngine> spillKVEngine,
                                     StorageEngineOptions options)
    : _engine(std::move(engine)),
      _spillKVEngine(std::move(spillKVEngine)),
      _options(std::move(options)),
      _dropPendingIdentReaper(_engine.get()),
      _minOfCheckpointAndOldestTimestampListener(
          TimestampMonitor::TimestampType::kMinOfCheckpointAndOldest,
          [this](OperationContext* opCtx, Timestamp timestamp) {
              _onMinOfCheckpointAndOldestTimestampChanged(opCtx, timestamp);
          }),
      _supportsCappedCollections(_engine->supportsCappedCollections()) {
    uassert(28601,
            "Storage engine does not support --directoryperdb",
            !(_options.directoryPerDB && !_engine->supportsDirectoryPerDB()));

    // Replace the noop recovery unit for the startup operation context now that the storage engine
    // has been initialized. This is needed because at the time of startup, when the operation
    // context gets created, the storage engine initialization has not yet begun and so it gets
    // assigned a noop recovery unit. See the StorageClientObserver class.
    auto prevRecoveryUnit = shard_role_details::releaseRecoveryUnit(opCtx);
    invariant(prevRecoveryUnit->isNoop());
    shard_role_details::setRecoveryUnit(
        opCtx, _engine->newRecoveryUnit(), WriteUnitOfWork::RecoveryUnitState::kNotInUnitOfWork);
    // If we throw in this constructor, make sure to destroy the RecoveryUnit instance created above
    // before '_engine' is destroyed.
    ScopeGuard recoveryUnitResetGuard([&] {
        shard_role_details::setRecoveryUnit(opCtx,
                                            std::move(prevRecoveryUnit),
                                            WriteUnitOfWork::RecoveryUnitState::kNotInUnitOfWork);
    });

    // If we are loading the catalog after an unclean shutdown, it's possible that there are
    // collections in the catalog that are unknown to the storage engine. We should attempt to
    // recover these orphaned idents.
    // Allowing locking in write mode as reinitializeStorageEngine will be called while holding the
    // global lock in exclusive mode.
    invariant(!shard_role_details::getLocker(opCtx)->isLocked() ||
              shard_role_details::getLocker(opCtx)->isW());
    Lock::GlobalWrite globalLk(opCtx);
    loadDurableCatalog(opCtx,
                       _options.lockFileCreatedByUncleanShutdown ? LastShutdownState::kUnclean
                                                                 : LastShutdownState::kClean);

    // We can dismiss recoveryUnitResetGuard now.
    recoveryUnitResetGuard.dismiss();
}

void StorageEngineImpl::loadDurableCatalog(OperationContext* opCtx,
                                           LastShutdownState lastShutdownState) {
    bool catalogExists =
        _engine->hasIdent(*shard_role_details::getRecoveryUnit(opCtx), kCatalogInfo);
    if (_options.forRepair && catalogExists) {
        auto repairObserver = StorageRepairObserver::get(getGlobalServiceContext());
        invariant(repairObserver->isIncomplete());

        LOGV2(22246, "Repairing catalog metadata");
        Status status =
            _engine->repairIdent(*shard_role_details::getRecoveryUnit(opCtx), kCatalogInfo);

        if (status.code() == ErrorCodes::DataModifiedByRepair) {
            LOGV2_WARNING(22264, "Catalog data modified by repair", "error"_attr = status);
            repairObserver->invalidatingModification(str::stream() << "DurableCatalog repaired: "
                                                                   << status.reason());
        } else {
            fassertNoTrace(50926, status);
        }
    }

    // The '_mdb_' catalog is generated and retrieved with a default 'RecordStore' configuration.
    // This maintains current and earlier behavior of a MongoD.
    const auto catalogRecordStoreOpts = RecordStore::Options{};
    if (!catalogExists) {
        WriteUnitOfWork uow(opCtx);

        auto status =
            _engine->createRecordStore(kCatalogInfoNamespace, kCatalogInfo, catalogRecordStoreOpts);

        // BadValue is usually caused by invalid configuration string.
        // We still fassert() but without a stack trace.
        if (status.code() == ErrorCodes::BadValue) {
            fassertFailedNoTrace(28562);
        }
        fassert(28520, status);
        uow.commit();
    }

    _catalogRecordStore = _engine->getRecordStore(
        opCtx, kCatalogInfoNamespace, kCatalogInfo, catalogRecordStoreOpts, boost::none /* uuid */);

    if (shouldLog(::mongo::logv2::LogComponent::kStorageRecovery, kCatalogLogLevel)) {
        LOGV2_FOR_RECOVERY(4615631, kCatalogLogLevel.toInt(), "loadDurableCatalog:");
        _dumpCatalog(opCtx);
    }

    LOGV2(9529901,
          "Initializing durable catalog",
          "numRecords"_attr = _catalogRecordStore->numRecords());
    _catalog.reset(new DurableCatalog(_catalogRecordStore.get(),
                                      _options.directoryPerDB,
                                      _options.directoryForIndexes,
                                      _engine.get()));
    _catalog->init(opCtx);

    LOGV2(9529902, "Retrieving all idents from storage engine");
    std::vector<std::string> identsKnownToStorageEngine =
        _engine->getAllIdents(*shard_role_details::getRecoveryUnit(opCtx));
    std::sort(identsKnownToStorageEngine.begin(), identsKnownToStorageEngine.end());

    std::vector<DurableCatalog::EntryIdentifier> catalogEntries =
        _catalog->getAllCatalogEntries(opCtx);

    // Perform a read on the catalog at the `oldestTimestamp` and record the record stores (via
    // their catalogId) that existed.
    std::set<RecordId> existedAtOldestTs;
    if (!_engine->getOldestTimestamp().isNull()) {
        ReadSourceScope snapshotScope(
            opCtx, RecoveryUnit::ReadSource::kProvided, _engine->getOldestTimestamp());
        auto entriesAtOldest = _catalog->getAllCatalogEntries(opCtx);
        LOGV2_FOR_RECOVERY(5380110,
                           kCatalogLogLevel.toInt(),
                           "Catalog entries at the oldest timestamp",
                           "oldestTimestamp"_attr = _engine->getOldestTimestamp());
        for (const auto& entry : entriesAtOldest) {
            existedAtOldestTs.insert(entry.catalogId);
            LOGV2_FOR_RECOVERY(5380109,
                               kCatalogLogLevel.toInt(),
                               "Historical entry",
                               "catalogId"_attr = entry.catalogId,
                               "ident"_attr = entry.ident,
                               logAttrs(entry.nss));
        }
    }

    if (_options.forRepair) {
        // It's possible that there are collection files on disk that are unknown to the catalog. In
        // a repair context, if we can't find an ident in the catalog, we generate a catalog entry
        // 'local.orphan.xxxxx' for it. However, in a nonrepair context, the orphaned idents
        // will be dropped in reconcileCatalogAndIdents().
        for (const auto& ident : identsKnownToStorageEngine) {
            if (ident::isCollectionIdent(ident)) {
                bool isOrphan = !std::any_of(catalogEntries.begin(),
                                             catalogEntries.end(),
                                             [this, &ident](DurableCatalog::EntryIdentifier entry) {
                                                 return entry.ident == ident;
                                             });
                if (isOrphan) {
                    // If the catalog does not have information about this
                    // collection, we create an new entry for it.
                    WriteUnitOfWork wuow(opCtx);

                    auto keyFormat =
                        _engine->getKeyFormat(*shard_role_details::getRecoveryUnit(opCtx), ident);
                    bool isClustered = keyFormat == KeyFormat::String;
                    CollectionOptions optionsWithUUID;
                    optionsWithUUID.uuid.emplace(UUID::gen());
                    if (isClustered) {
                        optionsWithUUID.clusteredIndex =
                            clustered_util::makeDefaultClusteredIdIndex();
                    }

                    StatusWith<std::string> statusWithNs =
                        _catalog->newOrphanedIdent(opCtx, ident, optionsWithUUID);

                    if (statusWithNs.isOK()) {
                        wuow.commit();
                        auto orphanCollNs = statusWithNs.getValue();
                        LOGV2(22247,
                              "Successfully created an entry in the catalog for orphaned "
                              "collection",
                              "namespace"_attr = orphanCollNs,
                              "options"_attr = optionsWithUUID);

                        if (!isClustered) {
                            // The _id index is already implicitly created on collections clustered
                            // by _id.
                            LOGV2_WARNING(22265,
                                          "Collection does not have an _id index. Please manually "
                                          "build the index",
                                          "namespace"_attr = orphanCollNs);
                        }
                        StorageRepairObserver::get(getGlobalServiceContext())
                            ->benignModification(str::stream() << "Orphan collection created: "
                                                               << statusWithNs.getValue());

                    } else {
                        // Log an error message if we cannot create the entry.
                        // reconcileCatalogAndIdents() will later drop this ident.
                        LOGV2_ERROR(
                            22268,
                            "Cannot create an entry in the catalog for orphaned ident. Restarting "
                            "the server will remove this ident",
                            "ident"_attr = ident,
                            "error"_attr = statusWithNs.getStatus());
                    }
                }
            }
        }
    }

    const auto loadingFromUncleanShutdownOrRepair =
        lastShutdownState == LastShutdownState::kUnclean || _options.forRepair;

    LOGV2(9529903,
          "Initializing all collections in durable catalog",
          "numEntries"_attr = catalogEntries.size());
    for (DurableCatalog::EntryIdentifier entry : catalogEntries) {
        if (_options.forRestore) {
            // When restoring a subset of user collections from a backup, the collections not
            // restored are in the catalog but are unknown to the storage engine. The catalog
            // entries for these collections will be removed.
            const auto collectionIdent = entry.ident;
            bool restoredIdent = std::binary_search(identsKnownToStorageEngine.begin(),
                                                    identsKnownToStorageEngine.end(),
                                                    collectionIdent);

            if (!restoredIdent) {
                LOGV2(6260800,
                      "Removing catalog entry for collection not restored",
                      logAttrs(entry.nss),
                      "ident"_attr = collectionIdent);

                WriteUnitOfWork wuow(opCtx);
                fassert(6260801, _catalog->_removeEntry(opCtx, entry.catalogId));
                wuow.commit();

                continue;
            }

            // A collection being restored needs to also restore all of its indexes.
            _checkForIndexFiles(opCtx, entry, identsKnownToStorageEngine);
        }

        if (loadingFromUncleanShutdownOrRepair) {
            // If we are loading the catalog after an unclean shutdown or during repair, it's
            // possible that there are collections in the catalog that are unknown to the storage
            // engine. If we can't find a table in the list of storage engine idents, either
            // attempt to recover the ident or drop it.
            const auto collectionIdent = entry.ident;
            bool orphan = !std::binary_search(identsKnownToStorageEngine.begin(),
                                              identsKnownToStorageEngine.end(),
                                              collectionIdent);
            // If the storage engine is missing a collection and is unable to create a new record
            // store, drop it from the catalog and skip initializing it by continuing past the
            // following logic.
            if (orphan) {
                auto status =
                    _recoverOrphanedCollection(opCtx, entry.catalogId, entry.nss, collectionIdent);
                if (!status.isOK()) {
                    LOGV2_WARNING(22266,
                                  "Failed to recover orphaned data file for collection",
                                  logAttrs(entry.nss),
                                  "error"_attr = status);
                    WriteUnitOfWork wuow(opCtx);
                    fassert(50716, _catalog->_removeEntry(opCtx, entry.catalogId));

                    if (_options.forRepair) {
                        StorageRepairObserver::get(getGlobalServiceContext())
                            ->invalidatingModification(
                                str::stream() << "Collection " << entry.nss.toStringForErrorMsg()
                                              << " dropped: " << status.reason());
                    }
                    wuow.commit();
                    continue;
                }
            }
        }

        if (!entry.nss.isReplicated() &&
            !std::binary_search(identsKnownToStorageEngine.begin(),
                                identsKnownToStorageEngine.end(),
                                entry.ident)) {
            // All collection drops are non-transactional and unreplicated collections are dropped
            // immediately as they do not use two-phase drops. It's possible to run into a situation
            // where there are collections in the catalog that are unknown to the storage engine
            // after restoring from backed up data files. See SERVER-55552.
            WriteUnitOfWork wuow(opCtx);
            fassert(5555200, _catalog->_removeEntry(opCtx, entry.catalogId));
            wuow.commit();

            LOGV2_INFO(5555201,
                       "Removed unknown unreplicated collection from the catalog",
                       "catalogId"_attr = entry.catalogId,
                       logAttrs(entry.nss),
                       "ident"_attr = entry.ident);
            continue;
        }

        if (entry.nss.isOrphanCollection()) {
            LOGV2(22248, "Orphaned collection found", logAttrs(entry.nss));
        }
    }

    shard_role_details::getRecoveryUnit(opCtx)->abandonSnapshot();
}

void StorageEngineImpl::closeDurableCatalog(OperationContext* opCtx) {
    dassert(shard_role_details::getLocker(opCtx)->isLocked());
    if (shouldLog(::mongo::logv2::LogComponent::kStorageRecovery, kCatalogLogLevel)) {
        LOGV2_FOR_RECOVERY(4615632, kCatalogLogLevel.toInt(), "closeDurableCatalog:");
        _dumpCatalog(opCtx);
    }

    _catalog.reset();
    _catalogRecordStore.reset();
}

Status StorageEngineImpl::_recoverOrphanedCollection(OperationContext* opCtx,
                                                     RecordId catalogId,
                                                     const NamespaceString& collectionName,
                                                     StringData collectionIdent) {
    if (!_options.forRepair) {
        return {ErrorCodes::IllegalOperation, "Orphan recovery only supported in repair"};
    }
    LOGV2(22249,
          "Storage engine is missing collection from its metadata. Attempting to locate and "
          "recover the data",
          logAttrs(collectionName),
          "ident"_attr = collectionIdent);

    WriteUnitOfWork wuow(opCtx);
    const auto catalogEntry = _catalog->getParsedCatalogEntry(opCtx, catalogId);
    const auto md = catalogEntry->metadata;
    const auto recordStoreOptions = getRecordStoreOptions(collectionName, md->options);
    Status status =
        _engine->recoverOrphanedIdent(collectionName, collectionIdent, recordStoreOptions);

    bool dataModified = status.code() == ErrorCodes::DataModifiedByRepair;
    if (!status.isOK() && !dataModified) {
        return status;
    }
    if (dataModified) {
        StorageRepairObserver::get(getGlobalServiceContext())
            ->invalidatingModification(str::stream()
                                       << "Collection " << collectionName.toStringForErrorMsg()
                                       << " recovered: " << status.reason());
    }
    wuow.commit();
    return Status::OK();
}

void StorageEngineImpl::_checkForIndexFiles(
    OperationContext* opCtx,
    const DurableCatalog::EntryIdentifier& entry,
    std::vector<std::string>& identsKnownToStorageEngine) const {
    std::vector<std::string> indexIdents = _catalog->getIndexIdents(opCtx, entry.catalogId);
    for (const std::string& indexIdent : indexIdents) {
        bool restoredIndexIdent = std::binary_search(
            identsKnownToStorageEngine.begin(), identsKnownToStorageEngine.end(), indexIdent);

        if (restoredIndexIdent) {
            continue;
        }

        LOGV2_FATAL_NOTRACE(6261000,
                            "Collection is missing an index file",
                            logAttrs(entry.nss),
                            "collectionIdent"_attr = entry.ident,
                            "missingIndexIdent"_attr = indexIdent);
    }
}

bool StorageEngineImpl::_handleInternalIdent(OperationContext* opCtx,
                                             const std::string& ident,
                                             LastShutdownState lastShutdownState,
                                             ReconcileResult* reconcileResult,
                                             std::set<std::string>* internalIdentsToKeep,
                                             std::set<std::string>* allInternalIdents) {
    if (!ident::isInternalIdent(ident)) {
        return false;
    }

    allInternalIdents->insert(ident);

    // When starting up after an unclean shutdown, we do not attempt to recover any state from the
    // internal idents. Thus, we drop them in this case.
    if (lastShutdownState == LastShutdownState::kUnclean) {
        return true;
    }

    if (!isResumableIndexBuildIdent(ident)) {
        return false;
    }

    // When starting up after a clean shutdown and resumable index builds are supported, find the
    // internal idents that contain the relevant information to resume each index build and recover
    // the state.
    auto rs = _engine->getRecordStore(
        opCtx, NamespaceString::kEmpty, ident, RecordStore::Options{}, boost::none /* uuid */);

    auto cursor = rs->getCursor(opCtx);
    auto record = cursor->next();
    if (record) {
        auto doc = record.value().data.toBson();

        // Parse the documents here so that we can restart the build if the document doesn't
        // contain all the necessary information to be able to resume building the index.
        ResumeIndexInfo resumeInfo;
        try {
            if (MONGO_unlikely(failToParseResumeIndexInfo.shouldFail())) {
                uasserted(ErrorCodes::FailPointEnabled,
                          "failToParseResumeIndexInfo fail point is enabled");
            }

            resumeInfo = ResumeIndexInfo::parse(IDLParserContext("ResumeIndexInfo"), doc);
        } catch (const DBException& e) {
            LOGV2(4916300, "Failed to parse resumable index info", "error"_attr = e.toStatus());

            // Ignore the error so that we can restart the index build instead of resume it. We
            // should drop the internal ident if we failed to parse.
            return true;
        }

        LOGV2(4916301,
              "Found unfinished index build to resume",
              "buildUUID"_attr = resumeInfo.getBuildUUID(),
              "collectionUUID"_attr = resumeInfo.getCollectionUUID(),
              "phase"_attr = IndexBuildPhase_serializer(resumeInfo.getPhase()));

        // Keep the tables that are needed to rebuild this index.
        // Note: the table that stores the rebuild metadata itself (i.e. |ident|) isn't kept.
        for (const mongo::IndexStateInfo& idx : resumeInfo.getIndexes()) {
            internalIdentsToKeep->insert(idx.getSideWritesTable().toString());
            if (idx.getDuplicateKeyTrackerTable()) {
                internalIdentsToKeep->insert(idx.getDuplicateKeyTrackerTable()->toString());
            }
            if (idx.getSkippedRecordTrackerTable()) {
                internalIdentsToKeep->insert(idx.getSkippedRecordTrackerTable()->toString());
            }
        }

        reconcileResult->indexBuildsToResume.push_back(std::move(resumeInfo));

        return true;
    }
    return false;
}

/**
 * This method reconciles differences between idents the KVEngine is aware of and the
 * DurableCatalog. There are three differences to consider:
 *
 * First, a KVEngine may know of an ident that the DurableCatalog does not. This method will drop
 * the ident from the KVEngine.
 *
 * Second, a DurableCatalog may have a collection ident that the KVEngine does not. This is an
 * illegal state and this method fasserts.
 *
 * Third, a DurableCatalog may have an index ident that the KVEngine does not. This method will
 * rebuild the index.
 */
StatusWith<StorageEngine::ReconcileResult> StorageEngineImpl::reconcileCatalogAndIdents(
    OperationContext* opCtx, Timestamp stableTs, LastShutdownState lastShutdownState) {
    // Gather all tables known to the storage engine and drop those that aren't cross-referenced
    // in the _mdb_catalog. This can happen for two reasons.
    //
    // First, collection creation and deletion happen in two steps. First the storage engine
    // creates/deletes the table, followed by the change to the _mdb_catalog. It's not assumed a
    // storage engine can make these steps atomic.
    //
    // Second, a replica set node in 3.6+ on supported storage engines will only persist "stable"
    // data to disk. That is data which replication guarantees won't be rolled back. The
    // _mdb_catalog will reflect the "stable" set of collections/indexes. However, it's not
    // expected for a storage engine's ability to persist stable data to extend to "stable
    // tables".
    std::set<std::string> engineIdents;
    {
        std::vector<std::string> vec =
            _engine->getAllIdents(*shard_role_details::getRecoveryUnit(opCtx));
        engineIdents.insert(vec.begin(), vec.end());
        engineIdents.erase(kCatalogInfo);
    }

    LOGV2_FOR_RECOVERY(4615633, 2, "Reconciling collection and index idents.");
    std::set<std::string> catalogIdents;
    {
        std::vector<std::string> vec = _catalog->getAllIdents(opCtx);
        catalogIdents.insert(vec.begin(), vec.end());
    }
    std::set<std::string> internalIdentsToKeep;
    std::set<std::string> allInternalIdents;

    auto dropPendingIdents = _dropPendingIdentReaper.getAllIdentNames();

    // Drop all idents in the storage engine that are not known to the catalog. This can happen in
    // the case of a collection or index creation being rolled back.
    StorageEngine::ReconcileResult reconcileResult;
    for (const auto& it : engineIdents) {
        if (catalogIdents.find(it) != catalogIdents.end()) {
            continue;
        }

        if (_handleInternalIdent(opCtx,
                                 it,
                                 lastShutdownState,
                                 &reconcileResult,
                                 &internalIdentsToKeep,
                                 &allInternalIdents)) {
            continue;
        }

        if (!ident::isCollectionOrIndexIdent(it)) {
            // Only indexes and collections are candidates for dropping when the storage engine's
            // metadata does not align with the catalog metadata.
            continue;
        }

        // In repair context, any orphaned collection idents from the engine should already be
        // recovered in the catalog in loadDurableCatalog().
        invariant(!(ident::isCollectionIdent(it) && _options.forRepair));

        // Leave drop-pending idents alone.
        // These idents have to be retained as long as the corresponding drops are not part of a
        // checkpoint.
        if (dropPendingIdents.find(it) != dropPendingIdents.cend()) {
            LOGV2(22250,
                  "Not removing ident for uncheckpointed collection or index drop",
                  "ident"_attr = it);
            continue;
        }

        const auto& toRemove = it;
        const Timestamp identDropTs = stableTs;
        LOGV2_PROD_ONLY(
            22251, "Dropping unknown ident", "ident"_attr = toRemove, "ts"_attr = identDropTs);
        if (!identDropTs.isNull()) {
            addDropPendingIdent(identDropTs, std::make_shared<Ident>(toRemove), /*onDrop=*/nullptr);
        } else {
            WriteUnitOfWork wuow(opCtx);
            Status status = _engine->dropIdent(shard_role_details::getRecoveryUnit(opCtx),
                                               toRemove,
                                               ident::isCollectionIdent(toRemove));
            if (!status.isOK()) {
                // A concurrent operation, such as a checkpoint could be holding an open data handle
                // on the ident. Handoff the ident drop to the ident reaper to retry later.
                addDropPendingIdent(
                    identDropTs, std::make_shared<Ident>(toRemove), /*onDrop=*/nullptr);
            }
            wuow.commit();
        }
    }

    // Scan all collections in the catalog and make sure their ident is known to the storage
    // engine. An omission here is fatal. A missing ident could mean a collection drop was rolled
    // back. Note that startup already attempts to open tables; this should only catch errors in
    // other contexts such as `recoverToStableTimestamp`.
    std::vector<DurableCatalog::EntryIdentifier> catalogEntries =
        _catalog->getAllCatalogEntries(opCtx);
    if (!_options.forRepair) {
        for (const DurableCatalog::EntryIdentifier& entry : catalogEntries) {
            if (engineIdents.find(entry.ident) == engineIdents.end()) {
                return {ErrorCodes::UnrecoverableRollbackError,
                        str::stream()
                            << "Expected collection does not exist. Collection: "
                            << entry.nss.toStringForErrorMsg() << " Ident: " << entry.ident};
            }
        }
    }

    // Scan all indexes and return those in the catalog where the storage engine does not have the
    // corresponding ident. The caller is expected to rebuild these indexes.
    //
    // Also, remove unfinished builds except those that were background index builds started on a
    // secondary.
    for (const DurableCatalog::EntryIdentifier& entry : catalogEntries) {
        const auto catalogEntry = _catalog->getParsedCatalogEntry(opCtx, entry.catalogId);
        auto md = catalogEntry->metadata;

        // Batch up the indexes to remove them from `metaData` outside of the iterator.
        std::vector<std::string> indexesToDrop;
        for (const auto& indexMetaData : md->indexes) {
            auto indexName = indexMetaData.nameStringData();
            auto indexIdent = _catalog->getIndexIdent(opCtx, entry.catalogId, indexName);

            // Warn in case of incorrect "multikeyPath" information in catalog documents. This is
            // the result of a concurrency bug which has since been fixed, but may persist in
            // certain catalog documents. See https://jira.mongodb.org/browse/SERVER-43074
            const bool hasMultiKeyPaths =
                std::any_of(indexMetaData.multikeyPaths.begin(),
                            indexMetaData.multikeyPaths.end(),
                            [](auto& pathSet) { return pathSet.size() > 0; });
            if (!indexMetaData.multikey && hasMultiKeyPaths) {
                LOGV2_WARNING(
                    22267,
                    "The 'multikey' field for index was false with non-empty 'multikeyPaths'. This "
                    "indicates corruption of the catalog. Consider either dropping and recreating "
                    "the index, or rerunning with the --repair option. See "
                    "http://dochub.mongodb.org/core/repair for more information",
                    "index"_attr = indexName,
                    logAttrs(md->nss));
            }

            if (!engineIdents.count(indexIdent)) {
                // There are certain cases where the catalog entry may reference an index ident
                // which is no longer present. One example of this is when an unclean shutdown
                // occurs before a checkpoint is taken during startup recovery. Since we drop the
                // index ident without a timestamp when restarting the index build for startup
                // recovery, the subsequent startup recovery can see the now-dropped ident
                // referenced by the old index catalog entry.
                LOGV2(6386500,
                      "Index catalog entry ident not found",
                      "ident"_attr = indexIdent,
                      "entry"_attr = indexMetaData.spec,
                      logAttrs(md->nss));
            }

            // Any index build with a UUID is an unfinished two-phase build and must be restarted.
            // There are no special cases to handle on primaries or secondaries. An index build may
            // be associated with multiple indexes. We should only restart an index build if we
            // aren't going to resume it.
            if (indexMetaData.buildUUID) {
                invariant(!indexMetaData.ready);

                auto collUUID = md->options.uuid;
                invariant(collUUID);
                auto buildUUID = *indexMetaData.buildUUID;

                LOGV2(22253,
                      "Found index from unfinished build",
                      logAttrs(md->nss),
                      "uuid"_attr = *collUUID,
                      "index"_attr = indexName,
                      "buildUUID"_attr = buildUUID);

                // Insert in the map if a build has not already been registered.
                auto existingIt = reconcileResult.indexBuildsToRestart.find(buildUUID);
                if (existingIt == reconcileResult.indexBuildsToRestart.end()) {
                    reconcileResult.indexBuildsToRestart.insert(
                        {buildUUID, IndexBuildDetails(*collUUID)});
                    existingIt = reconcileResult.indexBuildsToRestart.find(buildUUID);
                }

                existingIt->second.indexSpecs.emplace_back(indexMetaData.spec);
                continue;
            }

            // The last anomaly is when the index build did not complete. This implies the index
            // build was on:
            // (1) a standalone and the `createIndexes` command never successfully returned, or
            // (2) an initial syncing node bulk building indexes during a collection clone.
            // In both cases the index entry in the catalog should be dropped.
            if (!indexMetaData.ready) {
                LOGV2(22256,
                      "Dropping unfinished index",
                      logAttrs(md->nss),
                      "index"_attr = indexName);
                // Ensure the `ident` is dropped while we have the `indexIdent` value.
                Status status = _engine->dropIdent(shard_role_details::getRecoveryUnit(opCtx),
                                                   indexIdent,
                                                   /*identHasSizeInfo=*/false);
                if (!status.isOK()) {
                    // A concurrent operation, such as a checkpoint could be holding an open data
                    // handle on the ident. Handoff the ident drop to the ident reaper to retry
                    // later.
                    addDropPendingIdent(
                        Timestamp::min(), std::make_shared<Ident>(indexIdent), /*onDrop=*/nullptr);
                }
                indexesToDrop.push_back(indexName.toString());
                continue;
            }
        }

        for (auto&& indexName : indexesToDrop) {
            invariant(md->eraseIndex(indexName),
                      str::stream() << "Index is missing. Collection: "
                                    << md->nss.toStringForErrorMsg() << " Index: " << indexName);
        }
        if (indexesToDrop.size() > 0) {
            WriteUnitOfWork wuow(opCtx);
            CollectionWriter writer{opCtx, entry.nss};
            auto collection = writer.getWritableCollection(opCtx);
            invariant(collection->getCatalogId() == entry.catalogId);
            collection->replaceMetadata(opCtx, std::move(md));
            wuow.commit();
        }
    }

    // Drop any internal ident that we won't need.
    for (auto&& temp : allInternalIdents) {
        if (internalIdentsToKeep.contains(temp)) {
            continue;
        }
        LOGV2(22257, "Dropping internal ident", "ident"_attr = temp);
        WriteUnitOfWork wuow(opCtx);
        Status status = _engine->dropIdent(
            shard_role_details::getRecoveryUnit(opCtx), temp, ident::isCollectionIdent(temp));
        if (!status.isOK()) {
            // A concurrent operation, such as a checkpoint could be holding an open data handle on
            // the ident. Handoff the ident drop to the ident reaper to retry later.
            addDropPendingIdent(
                Timestamp::min(), std::make_shared<Ident>(temp), /*onDrop=*/nullptr);
        }
        wuow.commit();
    }

    return reconcileResult;
}

std::string StorageEngineImpl::getFilesystemPathForDb(const DatabaseName& dbName) const {
    if (_options.directoryPerDB) {
        return storageGlobalParams.dbpath + '/' + ident::createDBNamePathComponent(dbName);
    } else {
        return storageGlobalParams.dbpath;
    }
}

void StorageEngineImpl::cleanShutdown(ServiceContext* svcCtx) {
    _timestampMonitor.reset();

    _catalog.reset();
    _catalogRecordStore.reset();

    _engine->cleanShutdown();
    // intentionally not deleting _engine
    if (_spillKVEngine) {
        _spillKVEngine->cleanShutdown();
    }
}

StorageEngineImpl::~StorageEngineImpl() {}

void StorageEngineImpl::startTimestampMonitor(
    std::initializer_list<TimestampMonitor::TimestampListener*> listeners) {
    // Unless explicitly disabled, all storage engines should create a TimestampMonitor for
    // drop-pending internal idents, even if they do not support pending drops for collections
    // and indexes.
    _timestampMonitor = std::make_unique<TimestampMonitor>(
        _engine.get(), getGlobalServiceContext()->getPeriodicRunner());

    _timestampMonitor->addListener(&_minOfCheckpointAndOldestTimestampListener);

    // Caller must provide listener for cleanup of CollectionCatalog when oldest timestamp advances.
    invariant(!std::empty(listeners));
    for (auto listener : listeners) {
        _timestampMonitor->addListener(listener);
    }
}

void StorageEngineImpl::stopTimestampMonitor() {
    _listeners = _timestampMonitor->getListeners();
    _timestampMonitor.reset();
}

void StorageEngineImpl::restartTimestampMonitor() {
    _timestampMonitor = std::make_unique<TimestampMonitor>(
        _engine.get(), getGlobalServiceContext()->getPeriodicRunner());

    invariant(!std::empty(_listeners));
    for (auto listener : _listeners) {
        _timestampMonitor->addListener(listener);
    }
    _listeners.clear();
}

void StorageEngineImpl::notifyStorageStartupRecoveryComplete() {
    _engine->notifyStorageStartupRecoveryComplete();
}

void StorageEngineImpl::notifyReplStartupRecoveryComplete(RecoveryUnit& ru) {
    _engine->notifyReplStartupRecoveryComplete(ru);
}

void StorageEngineImpl::setInStandaloneMode(bool inStandaloneMode) {
    _engine->setInStandaloneMode(inStandaloneMode);
}

std::unique_ptr<RecoveryUnit> StorageEngineImpl::newRecoveryUnit() {
    if (!_engine) {
        // shutdown
        return nullptr;
    }
    return _engine->newRecoveryUnit();
}

void StorageEngineImpl::flushAllFiles(OperationContext* opCtx, bool callerHoldsReadLock) {
    _engine->flushAllFiles(opCtx, callerHoldsReadLock);
}

Status StorageEngineImpl::beginBackup() {
    // We should not proceed if we are already in backup mode
    if (_inBackupMode)
        return Status(ErrorCodes::BadValue, "Already in Backup Mode");
    Status status = _engine->beginBackup();
    if (status.isOK())
        _inBackupMode = true;
    return status;
}

void StorageEngineImpl::endBackup() {
    // We should never reach here if we aren't already in backup mode
    invariant(_inBackupMode);
    _engine->endBackup();
    _inBackupMode = false;
}

Timestamp StorageEngineImpl::getBackupCheckpointTimestamp() {
    return _engine->getBackupCheckpointTimestamp();
}

Status StorageEngineImpl::disableIncrementalBackup() {
    LOGV2(9538600, "Disabling incremental backup");
    return _engine->disableIncrementalBackup();
}

StatusWith<std::unique_ptr<StorageEngine::StreamingCursor>>
StorageEngineImpl::beginNonBlockingBackup(const StorageEngine::BackupOptions& options) {
    return _engine->beginNonBlockingBackup(options);
}

void StorageEngineImpl::endNonBlockingBackup() {
    return _engine->endNonBlockingBackup();
}

StatusWith<std::deque<std::string>> StorageEngineImpl::extendBackupCursor() {
    return _engine->extendBackupCursor();
}

bool StorageEngineImpl::supportsCheckpoints() const {
    return _engine->supportsCheckpoints();
}

bool StorageEngineImpl::isEphemeral() const {
    return _engine->isEphemeral();
}

SnapshotManager* StorageEngineImpl::getSnapshotManager() const {
    return _engine->getSnapshotManager();
}

Status StorageEngineImpl::repairRecordStore(OperationContext* opCtx,
                                            RecordId catalogId,
                                            const NamespaceString& nss) {
    auto repairObserver = StorageRepairObserver::get(getGlobalServiceContext());
    invariant(repairObserver->isIncomplete());

    Status status = _engine->repairIdent(*shard_role_details::getRecoveryUnit(opCtx),
                                         _catalog->getEntry(catalogId).ident);
    bool dataModified = status.code() == ErrorCodes::DataModifiedByRepair;
    if (!status.isOK() && !dataModified) {
        return status;
    }

    if (dataModified) {
        repairObserver->invalidatingModification(
            str::stream() << "Collection " << nss.toStringForErrorMsg() << ": " << status.reason());
    }

    return status;
}

std::unique_ptr<SpillTable> StorageEngineImpl::makeSpillTable(OperationContext* opCtx,
                                                              KeyFormat keyFormat) {
    invariant(_spillKVEngine);
    std::unique_ptr<RecordStore> rs = _spillKVEngine->makeTemporaryRecordStore(
        opCtx, ident::generateNewInternalIdent(), keyFormat);
    LOGV2_DEBUG(10380301, 1, "Created spill table", "ident"_attr = rs->getIdent());
    return std::make_unique<SpillTable>(std::move(rs));
}

std::unique_ptr<TemporaryRecordStore> StorageEngineImpl::makeTemporaryRecordStore(
    OperationContext* opCtx, KeyFormat keyFormat) {
    std::unique_ptr<RecordStore> rs =
        _engine->makeTemporaryRecordStore(opCtx, ident::generateNewInternalIdent(), keyFormat);
    LOGV2_DEBUG(22258, 1, "Created temporary record store", "ident"_attr = rs->getIdent());
    return std::make_unique<DeferredDropRecordStore>(std::move(rs), this);
}

std::unique_ptr<TemporaryRecordStore>
StorageEngineImpl::makeTemporaryRecordStoreForResumableIndexBuild(OperationContext* opCtx,
                                                                  KeyFormat keyFormat) {
    std::unique_ptr<RecordStore> rs =
        _engine->makeTemporaryRecordStore(opCtx, generateNewResumableIndexBuildIdent(), keyFormat);
    LOGV2_DEBUG(4921500,
                1,
                "Created temporary record store for resumable index build",
                "ident"_attr = rs->getIdent());
    return std::make_unique<DeferredDropRecordStore>(std::move(rs), this);
}

std::unique_ptr<TemporaryRecordStore> StorageEngineImpl::makeTemporaryRecordStoreFromExistingIdent(
    OperationContext* opCtx, StringData ident, KeyFormat keyFormat) {
    auto rs = _engine->getTemporaryRecordStore(opCtx, ident, keyFormat);
    return std::make_unique<DeferredDropRecordStore>(std::move(rs), this);
}

void StorageEngineImpl::setJournalListener(JournalListener* jl) {
    _engine->setJournalListener(jl);
}

void StorageEngineImpl::setStableTimestamp(Timestamp stableTimestamp, bool force) {
    _engine->setStableTimestamp(stableTimestamp, force);
}

Timestamp StorageEngineImpl::getStableTimestamp() const {
    return _engine->getStableTimestamp();
}

void StorageEngineImpl::setInitialDataTimestamp(Timestamp initialDataTimestamp) {
    _engine->setInitialDataTimestamp(initialDataTimestamp);
}

Timestamp StorageEngineImpl::getInitialDataTimestamp() const {
    return _engine->getInitialDataTimestamp();
}

void StorageEngineImpl::setOldestTimestampFromStable() {
    _engine->setOldestTimestampFromStable();
}

void StorageEngineImpl::setOldestTimestamp(Timestamp newOldestTimestamp, bool force) {
    _engine->setOldestTimestamp(newOldestTimestamp, force);
}

Timestamp StorageEngineImpl::getOldestTimestamp() const {
    return _engine->getOldestTimestamp();
};

void StorageEngineImpl::setOldestActiveTransactionTimestampCallback(
    StorageEngine::OldestActiveTransactionTimestampCallback callback) {
    _engine->setOldestActiveTransactionTimestampCallback(callback);
}

bool StorageEngineImpl::supportsRecoverToStableTimestamp() const {
    return _engine->supportsRecoverToStableTimestamp();
}

bool StorageEngineImpl::supportsRecoveryTimestamp() const {
    return _engine->supportsRecoveryTimestamp();
}

StatusWith<Timestamp> StorageEngineImpl::recoverToStableTimestamp(OperationContext* opCtx) {
    invariant(shard_role_details::getLocker(opCtx)->isW());

    // SERVER-58311: Reset the recovery unit to unposition storage engine cursors. This allows WT to
    // assert it has sole access when performing rollback_to_stable().
    shard_role_details::replaceRecoveryUnit(opCtx);

    StatusWith<Timestamp> swTimestamp = _engine->recoverToStableTimestamp(*opCtx);
    if (!swTimestamp.isOK()) {
        return swTimestamp;
    }

    DurableHistoryRegistry::get(opCtx)->reconcilePins(opCtx);

    LOGV2(22259,
          "recoverToStableTimestamp successful",
          "stableTimestamp"_attr = swTimestamp.getValue());
    return {swTimestamp.getValue()};
}

boost::optional<Timestamp> StorageEngineImpl::getRecoveryTimestamp() const {
    return _engine->getRecoveryTimestamp();
}

boost::optional<Timestamp> StorageEngineImpl::getLastStableRecoveryTimestamp() const {
    return _engine->getLastStableRecoveryTimestamp();
}

bool StorageEngineImpl::supportsReadConcernSnapshot() const {
    return _engine->supportsReadConcernSnapshot();
}

void StorageEngineImpl::clearDropPendingState(OperationContext* opCtx) {
    _dropPendingIdentReaper.clearDropPendingState(opCtx);
}

Timestamp StorageEngineImpl::getAllDurableTimestamp() const {
    return _engine->getAllDurableTimestamp();
}

boost::optional<Timestamp> StorageEngineImpl::getOplogNeededForCrashRecovery() const {
    return _engine->getOplogNeededForCrashRecovery();
}

Timestamp StorageEngineImpl::getPinnedOplog() const {
    return _engine->getPinnedOplog();
}

void StorageEngineImpl::_dumpCatalog(OperationContext* opCtx) {
    auto catalogRs = _catalogRecordStore.get();
    auto cursor = catalogRs->getCursor(opCtx);
    boost::optional<Record> rec = cursor->next();
    stdx::unordered_set<std::string> nsMap;
    while (rec) {
        // This should only be called by a parent that's done an appropriate `shouldLog` check. Do
        // not duplicate the log level policy.
        LOGV2_FOR_RECOVERY(4615634,
                           kCatalogLogLevel.toInt(),
                           "Catalog entry",
                           "catalogId"_attr = rec->id,
                           "value"_attr = rec->data.toBson());
        auto valueBson = rec->data.toBson();
        if (valueBson.hasField("md")) {
            std::string ns = valueBson.getField("md").Obj().getField("ns").String();
            invariant(!nsMap.count(ns), str::stream() << "Found duplicate namespace: " << ns);
            nsMap.insert(ns);
        }
        rec = cursor->next();
    }
    shard_role_details::getRecoveryUnit(opCtx)->abandonSnapshot();
}

void StorageEngineImpl::addDropPendingIdent(
    const std::variant<Timestamp, StorageEngine::CheckpointIteration>& dropTime,
    std::shared_ptr<Ident> ident,
    DropIdentCallback&& onDrop) {
    _dropPendingIdentReaper.addDropPendingIdent(dropTime, ident, std::move(onDrop));
}

std::shared_ptr<Ident> StorageEngineImpl::markIdentInUse(StringData ident) {
    return _dropPendingIdentReaper.markIdentInUse(ident);
}

void StorageEngineImpl::checkpoint() {
    _engine->checkpoint();
}

StorageEngine::CheckpointIteration StorageEngineImpl::getCheckpointIteration() const {
    return _engine->getCheckpointIteration();
}

bool StorageEngineImpl::hasDataBeenCheckpointed(
    StorageEngine::CheckpointIteration checkpointIteration) const {
    return _engine->hasDataBeenCheckpointed(checkpointIteration);
}

void StorageEngineImpl::_onMinOfCheckpointAndOldestTimestampChanged(OperationContext* opCtx,
                                                                    const Timestamp& timestamp) {
    if (_dropPendingIdentReaper.hasExpiredIdents(timestamp)) {
        LOGV2(22260,
              "Removing drop-pending idents with drop timestamps before timestamp",
              "timestamp"_attr = timestamp);

        _dropPendingIdentReaper.dropIdentsOlderThan(opCtx, timestamp);
    } else {
        LOGV2_DEBUG(8097401,
                    1,
                    "No drop-pending idents have expired",
                    "timestamp"_attr = timestamp,
                    "pendingIdentsCount"_attr = _dropPendingIdentReaper.getNumIdents());
    }
}

StorageEngineImpl::TimestampMonitor::TimestampMonitor(KVEngine* engine, PeriodicRunner* runner)
    : _engine(engine), _periodicRunner(runner) {
    _startup();
}

StorageEngineImpl::TimestampMonitor::~TimestampMonitor() {
    LOGV2(22261, "Timestamp monitor shutting down");
}

void StorageEngineImpl::TimestampMonitor::_startup() {
    invariant(!_running);

    LOGV2(22262, "Timestamp monitor starting");
    PeriodicRunner::PeriodicJob job(
        "TimestampMonitor",
        [&](Client* client) {
            if (MONGO_unlikely(pauseTimestampMonitor.shouldFail())) {
                LOGV2(6321800,
                      "Pausing the timestamp monitor due to the pauseTimestampMonitor fail point");
                pauseTimestampMonitor.pauseWhileSet();
            }

            {
                stdx::lock_guard<stdx::mutex> lock(_monitorMutex);
                if (_listeners.empty()) {
                    return;
                }
            }

            try {
                auto uniqueOpCtx = client->makeOperationContext();
                auto opCtx = uniqueOpCtx.get();

                auto backupCursorHooks = BackupCursorHooks::get(opCtx->getServiceContext());
                if (backupCursorHooks->isBackupCursorOpen()) {
                    LOGV2_DEBUG(9810500, 1, "Backup in progress, skipping table drops.");
                    return;
                }

                // The TimestampMonitor is an important background cleanup task for the storage
                // engine and needs to be able to make progress to free up resources.
                ScopedAdmissionPriority<ExecutionAdmissionContext> immediatePriority(
                    opCtx, AdmissionContext::Priority::kExempt);

                // The checkpoint timestamp is not cached in mongod and needs to be fetched with
                // a call into WiredTiger, all the other timestamps are cached in mongod.
                Timestamp checkpoint = _engine->getCheckpointTimestamp();
                Timestamp oldest = _engine->getOldestTimestamp();
                Timestamp stable = _engine->getStableTimestamp();
                Timestamp minOfCheckpointAndOldest =
                    (checkpoint.isNull() || (checkpoint > oldest)) ? oldest : checkpoint;

                {
                    stdx::lock_guard<stdx::mutex> lock(_monitorMutex);
                    for (const auto& listener : _listeners) {
                        if (listener->getType() == TimestampType::kCheckpoint) {
                            listener->notify(opCtx, checkpoint);
                        } else if (listener->getType() == TimestampType::kOldest) {
                            listener->notify(opCtx, oldest);
                        } else if (listener->getType() == TimestampType::kStable) {
                            listener->notify(opCtx, stable);
                        } else if (listener->getType() ==
                                   TimestampType::kMinOfCheckpointAndOldest) {
                            listener->notify(opCtx, minOfCheckpointAndOldest);
                        } else if (stable == Timestamp::min()) {
                            // Special case notification of all listeners when writes do not
                            // have timestamps. This handles standalone mode and storage engines
                            // that don't support timestamps.
                            listener->notify(opCtx, Timestamp::min());
                        }
                    }
                }

            } catch (const ExceptionFor<ErrorCodes::Interrupted>&) {
                LOGV2(6183600, "Timestamp monitor got interrupted, retrying");
                return;
            } catch (const ExceptionFor<ErrorCodes::InterruptedDueToReplStateChange>&) {
                LOGV2(6183601,
                      "Timestamp monitor got interrupted due to repl state change, retrying");
                return;
            } catch (const ExceptionFor<ErrorCodes::InterruptedAtShutdown>& ex) {
                if (_shuttingDown) {
                    return;
                }
                _shuttingDown = true;
                LOGV2(22263, "Timestamp monitor is stopping", "error"_attr = ex);
            } catch (const ExceptionFor<ErrorCategory::CancellationError>&) {
                return;
            } catch (const DBException& ex) {
                // Logs and rethrows the exceptions of other types.
                LOGV2_ERROR(5802500, "Timestamp monitor threw an exception", "error"_attr = ex);
                throw;
            }
        },
        Seconds(1),
        true /*isKillableByStepdown*/);

    _job = _periodicRunner->makeJob(std::move(job));
    _job.start();
    _running = true;
}

void StorageEngineImpl::TimestampMonitor::addListener(TimestampListener* listener) {
    stdx::lock_guard<stdx::mutex> lock(_monitorMutex);
    if (std::find(_listeners.begin(), _listeners.end(), listener) != _listeners.end()) {
        bool listenerAlreadyRegistered = true;
        invariant(!listenerAlreadyRegistered);
    }
    _listeners.push_back(listener);
}

void StorageEngineImpl::TimestampMonitor::removeListener(TimestampListener* listener) {
    stdx::lock_guard<stdx::mutex> lock(_monitorMutex);
    if (auto it = std::find(_listeners.begin(), _listeners.end(), listener);
        it != _listeners.end()) {
        _listeners.erase(it);
    }
}

std::vector<StorageEngineImpl::TimestampMonitor::TimestampListener*>
StorageEngineImpl::TimestampMonitor::getListeners() {
    return _listeners;
}

StatusWith<Timestamp> StorageEngineImpl::pinOldestTimestamp(
    RecoveryUnit& ru,
    const std::string& requestingServiceName,
    Timestamp requestedTimestamp,
    bool roundUpIfTooOld) {
    return _engine->pinOldestTimestamp(
        ru, requestingServiceName, requestedTimestamp, roundUpIfTooOld);
}

void StorageEngineImpl::unpinOldestTimestamp(const std::string& requestingServiceName) {
    _engine->unpinOldestTimestamp(requestingServiceName);
}

void StorageEngineImpl::setPinnedOplogTimestamp(const Timestamp& pinnedTimestamp) {
    _engine->setPinnedOplogTimestamp(pinnedTimestamp);
}

Status StorageEngineImpl::oplogDiskLocRegister(OperationContext* opCtx,
                                               RecordStore* oplogRecordStore,
                                               const Timestamp& opTime,
                                               bool orderedCommit) {
    // Callers should be updating visibility as part of a write operation. We want to ensure that
    // we never get here while holding an uninterruptible, read-ticketed lock. That would indicate
    // that we are operating with the wrong global lock semantics, and either hold too weak a lock
    // (e.g. IS) or that we upgraded in a way we shouldn't (e.g. IS -> IX).
    invariant(!shard_role_details::getLocker(opCtx)->hasReadTicket() ||
              !opCtx->uninterruptibleLocksRequested_DO_NOT_USE());  // NOLINT

    return _engine->oplogDiskLocRegister(
        *shard_role_details::getRecoveryUnit(opCtx), oplogRecordStore, opTime, orderedCommit);
}

void StorageEngineImpl::waitForAllEarlierOplogWritesToBeVisible(
    OperationContext* opCtx, RecordStore* oplogRecordStore) const {
    // Callers are waiting for other operations to finish updating visibility. We want to ensure
    // that we never get here while holding an uninterruptible, write-ticketed lock. That could
    // indicate we are holding a stronger lock than we need to, and that we could actually
    // contribute to ticket-exhaustion. That could prevent the write we are waiting on from
    // acquiring the lock it needs to update the oplog visibility.
    invariant(!shard_role_details::getLocker(opCtx)->hasWriteTicket() ||
              !opCtx->uninterruptibleLocksRequested_DO_NOT_USE());  // NOLINT

    // Make sure that callers do not hold an active snapshot so it will be able to see the oplog
    // entries it waited for afterwards.
    if (shard_role_details::getRecoveryUnit(opCtx)->isActive()) {
        shard_role_details::getLocker(opCtx)->dump();
        invariant(!shard_role_details::getRecoveryUnit(opCtx)->isActive(),
                  str::stream() << "Unexpected open storage txn. RecoveryUnit state: "
                                << RecoveryUnit::toString(
                                       shard_role_details::getRecoveryUnit(opCtx)->getState())
                                << ", inMultiDocumentTransaction:"
                                << (opCtx->inMultiDocumentTransaction() ? "true" : "false"));
    }

    _engine->waitForAllEarlierOplogWritesToBeVisible(opCtx, oplogRecordStore);
}

bool StorageEngineImpl::waitUntilDurable(OperationContext* opCtx) {
    // Don't block while holding a lock unless we are the only active operation in the system.
    auto locker = shard_role_details::getLocker(opCtx);
    invariant(!locker->isLocked() || locker->isW());
    return _engine->waitUntilDurable(opCtx);
}

bool StorageEngineImpl::waitUntilUnjournaledWritesDurable(OperationContext* opCtx,
                                                          bool stableCheckpoint) {
    // Don't block while holding a lock unless we are the only active operation in the system.
    auto locker = shard_role_details::getLocker(opCtx);
    invariant(!locker->isLocked() || locker->isW());
    return _engine->waitUntilUnjournaledWritesDurable(opCtx, stableCheckpoint);
}

DurableCatalog* StorageEngineImpl::getDurableCatalog() {
    return _catalog.get();
}

const DurableCatalog* StorageEngineImpl::getDurableCatalog() const {
    return _catalog.get();
}

BSONObj StorageEngineImpl::getSanitizedStorageOptionsForSecondaryReplication(
    const BSONObj& options) const {
    return _engine->getSanitizedStorageOptionsForSecondaryReplication(options);
}

void StorageEngineImpl::dump() const {
    _engine->dump();
}

Status StorageEngineImpl::autoCompact(RecoveryUnit& ru, const AutoCompactOptions& options) {
    return _engine->autoCompact(ru, options);
}

bool StorageEngineImpl::underCachePressure() {
    return _engine->underCachePressure();
};

size_t StorageEngineImpl::getCacheSizeMB() {
    return _engine->getCacheSizeMB();
}

bool StorageEngineImpl::hasOngoingLiveRestore() {
    return _engine->hasOngoingLiveRestore();
}

}  // namespace mongo
