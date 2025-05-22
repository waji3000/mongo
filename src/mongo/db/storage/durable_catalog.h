/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#pragma once

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <boost/optional/optional.hpp>

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/record_id.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/bson_collection_catalog_entry.h"
#include "mongo/db/storage/durable_catalog_entry.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/sorted_data_interface.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/string_map.h"
#include "mongo/util/uuid.h"

namespace mongo {

class KVEngine;

/**
 * An interface to modify the on-disk catalog metadata.
 */
class DurableCatalog final {
    DurableCatalog(const DurableCatalog&) = delete;
    DurableCatalog& operator=(const DurableCatalog&) = delete;
    DurableCatalog(DurableCatalog&&) = delete;
    DurableCatalog& operator=(DurableCatalog&&) = delete;

public:
    /**
     * `Entry` ties together the common identifiers of a single `_mdb_catalog` document.
     */
    struct EntryIdentifier {
        EntryIdentifier() {}
        EntryIdentifier(RecordId catalogId, std::string ident, NamespaceString nss)
            : catalogId(std::move(catalogId)), ident(std::move(ident)), nss(std::move(nss)) {}
        RecordId catalogId;
        std::string ident;
        NamespaceString nss;
    };

    DurableCatalog(RecordStore* rs,
                   bool directoryPerDb,
                   bool directoryForIndexes,
                   KVEngine* engine);
    DurableCatalog() = delete;


    static DurableCatalog* get(OperationContext* opCtx) {
        return opCtx->getServiceContext()->getStorageEngine()->getDurableCatalog();
    }

    void init(OperationContext* opCtx);

    std::vector<EntryIdentifier> getAllCatalogEntries(OperationContext* opCtx) const;

    /**
     * Scans the persisted catalog until an entry is found matching 'nss'.
     */
    boost::optional<DurableCatalogEntry> scanForCatalogEntryByNss(OperationContext* opCtx,
                                                                  const NamespaceString& nss) const;

    /**
     * Scans the persisted catalog until an entry is found matching 'uuid'.
     */
    boost::optional<DurableCatalogEntry> scanForCatalogEntryByUUID(OperationContext* opCtx,
                                                                   const UUID& uuid) const;

    EntryIdentifier getEntry(const RecordId& catalogId) const;

    /**
     * First tries to return the in-memory entry. If not found, e.g. when collection is dropped
     * after the provided timestamp, loads the entry from the persisted catalog at the provided
     * timestamp.
     */
    NamespaceString getNSSFromCatalog(OperationContext* opCtx, const RecordId& catalogId) const;

    std::string getIndexIdent(OperationContext* opCtx,
                              const RecordId& id,
                              StringData idxName) const;

    std::vector<std::string> getIndexIdents(OperationContext* opCtx, const RecordId& id) const;

    /**
     * Get a raw catalog entry for catalogId as BSON.
     */
    BSONObj getCatalogEntry(OperationContext* opCtx, const RecordId& catalogId) const {
        auto cursor = _rs->getCursor(opCtx);
        return _findEntry(*cursor, catalogId).getOwned();
    }

    /**
     * Parses the catalog entry object at `catalogId` to common types. Returns boost::none if it
     * doesn't exist or if the entry is the feature document.
     */
    boost::optional<DurableCatalogEntry> getParsedCatalogEntry(OperationContext* opCtx,
                                                               const RecordId& catalogId) const;

    /**
     * Helper which constructs a DurableCatalogEntry given 'catalogId' and 'obj'.
     */
    boost::optional<DurableCatalogEntry> parseCatalogEntry(const RecordId& catalogId,
                                                           const BSONObj& obj) const;

    /**
     * Updates the catalog entry for the collection 'nss' with the fields specified in 'md'. If
     * 'md.indexes' contains a new index entry, then this method generates a new index ident and
     * adds it to the catalog entry.
     */
    void putMetaData(OperationContext* opCtx,
                     const RecordId& id,
                     BSONCollectionCatalogEntry::MetaData& md);

    std::vector<std::string> getAllIdents(OperationContext* opCtx) const;

    RecordStore* getRecordStore() {
        return _rs;
    }

    /**
     * Create an entry in the catalog for an orphaned collection found in the
     * storage engine. Return the generated ns of the collection.
     * Note that this function does not recreate the _id index on the for non-clustered collections
     * because it does not have access to index catalog.
     */
    StatusWith<std::string> newOrphanedIdent(OperationContext* opCtx,
                                             std::string ident,
                                             const CollectionOptions& optionsWithUUID);

    /**
     * On success, returns the RecordId which identifies the new record store in the durable catalog
     * in addition to ownership of the new RecordStore.
     */
    StatusWith<std::pair<RecordId, std::unique_ptr<RecordStore>>> createCollection(
        OperationContext* opCtx,
        const NamespaceString& nss,
        const std::string& ident,
        const CollectionOptions& options);

    Status createIndex(OperationContext* opCtx,
                       const RecordId& catalogId,
                       const NamespaceString& nss,
                       const UUID& uuid,
                       const IndexConfig& indexConfig,
                       const boost::optional<BSONObj>& storageEngineIndexOptions);

    /**
     * Import a collection by inserting the given metadata into the durable catalog and instructing
     * the storage engine to import the corresponding idents. The metadata object should be a valid
     * catalog entry and contain the following fields:
     * "md": A document representing the BSONCollectionCatalogEntry::MetaData of the collection.
     * "idxIdent": A document containing {<index_name>: <index_ident>} pairs for all indexes.
     * "nss": NamespaceString of the collection being imported.
     * "ident": Ident of the collection file.
     *
     * On success, returns an ImportResult structure containing the RecordId which identifies the
     * new record store in the durable catalog, ownership of the new RecordStore and the UUID of the
     * collection imported.
     *
     * The collection must be locked in MODE_X when calling this function.
     */
    struct ImportResult {
        ImportResult(RecordId catalogId, std::unique_ptr<RecordStore> rs, UUID uuid)
            : catalogId(std::move(catalogId)), rs(std::move(rs)), uuid(uuid) {}
        RecordId catalogId;
        std::unique_ptr<RecordStore> rs;
        UUID uuid;
    };

    StatusWith<ImportResult> importCollection(OperationContext* opCtx,
                                              const NamespaceString& nss,
                                              const BSONObj& metadata,
                                              const BSONObj& storageMetadata,
                                              bool generateNewUUID,
                                              bool panicOnCorruptWtMetadata = true,
                                              bool repair = false);

    Status renameCollection(OperationContext* opCtx,
                            const RecordId& catalogId,
                            const NamespaceString& toNss,
                            BSONCollectionCatalogEntry::MetaData& md);

    /**
     * Deletes the persisted collection catalog entry identified by 'catalogId'.
     *
     * Expects (invariants) that all of the index catalog entries have been removed already via
     * removeIndex.
     */
    Status dropCollection(OperationContext* opCtx, const RecordId& catalogId);

    /**
     * Drops the provided ident and recreates it as empty for use in resuming an index build.
     */
    Status dropAndRecreateIndexIdentForResume(
        OperationContext* opCtx,
        const NamespaceString& nss,
        const UUID& uuid,
        StringData ident,
        const IndexConfig& indexConfig,
        const boost::optional<BSONObj>& storageEngineIndexOptions);

    void getReadyIndexes(OperationContext* opCtx, RecordId catalogId, StringSet* names) const;

    bool isIndexPresent(OperationContext* opCtx,
                        const RecordId& catalogId,
                        StringData indexName) const;

private:
    class AddIdentChange;

    friend class StorageEngineImpl;
    friend class DurableCatalogTest;
    friend class StorageEngineTest;

    /**
     * Finds the durable catalog entry using the provided RecordStore cursor.
     * The returned BSONObj is unowned and is only valid while the cursor is positioned.
     */
    BSONObj _findEntry(SeekableRecordCursor& cursor, const RecordId& catalogId) const;
    StatusWith<EntryIdentifier> _addEntry(OperationContext* opCtx,
                                          NamespaceString nss,
                                          const std::string& ident,
                                          const CollectionOptions& options);
    StatusWith<EntryIdentifier> _importEntry(OperationContext* opCtx,
                                             NamespaceString nss,
                                             const BSONObj& metadata);
    Status _removeEntry(OperationContext* opCtx, const RecordId& catalogId);

    std::shared_ptr<BSONCollectionCatalogEntry::MetaData> _parseMetaData(
        const BSONElement& mdElement) const;

    RecordStore* _rs;  // not owned
    const bool _directoryPerDb;
    const bool _directoryForIndexes;

    absl::flat_hash_map<RecordId, EntryIdentifier, RecordId::Hasher> _catalogIdToEntryMap;
    mutable stdx::mutex _catalogIdToEntryMapLock;

    KVEngine* const _engine;
};
}  // namespace mongo
