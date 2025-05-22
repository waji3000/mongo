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


#include <boost/optional.hpp>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

#include "mongo/bson/bsontypes.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/curop_failpoint_helpers.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/pipeline/document_source_cursor.h"
#include "mongo/db/pipeline/document_source_limit.h"
#include "mongo/db/pipeline/initialize_auto_get_helper.h"
#include "mongo/db/query/collection_index_usage_tracker_decoration.h"
#include "mongo/db/query/explain.h"
#include "mongo/db/query/explain_options.h"
#include "mongo/db/query/find_common.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/s/resharding/resume_token_gen.h"
#include "mongo/util/decorable.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/serialization_context.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {

MONGO_FAIL_POINT_DEFINE(hangBeforeDocumentSourceCursorLoadBatch);

ALLOCATE_DOCUMENT_SOURCE_ID(cursor, DocumentSourceCursor::id);

using boost::intrusive_ptr;
using std::string;

const char* DocumentSourceCursor::getSourceName() const {
    return kStageName.data();
}

bool DocumentSourceCursor::Batch::isEmpty() const {
    switch (_type) {
        case CursorType::kRegular:
            return _batchOfDocs.empty();
        case CursorType::kEmptyDocuments:
            return !_count;
    }
    MONGO_UNREACHABLE;
}

void DocumentSourceCursor::Batch::enqueue(Document&& doc, boost::optional<BSONObj> resumeToken) {
    switch (_type) {
        case CursorType::kRegular: {
            invariant(doc.isOwned());
            _batchOfDocs.push_back(std::move(doc));
            _memUsageBytes += _batchOfDocs.back().getApproximateSize();
            if (resumeToken) {
                _resumeTokens.push_back(*resumeToken);
                dassert(_resumeTokens.size() == _batchOfDocs.size());
            }
            break;
        }
        case CursorType::kEmptyDocuments: {
            ++_count;
            break;
        }
    }
}

Document DocumentSourceCursor::Batch::dequeue() {
    invariant(!isEmpty());
    switch (_type) {
        case CursorType::kRegular: {
            Document out = std::move(_batchOfDocs.front());
            _batchOfDocs.pop_front();
            if (_batchOfDocs.empty()) {
                _memUsageBytes = 0;
            }
            if (!_resumeTokens.empty()) {
                _resumeTokens.pop_front();
                dassert(_resumeTokens.size() == _batchOfDocs.size());
            }
            return out;
        }
        case CursorType::kEmptyDocuments: {
            --_count;
            return Document{};
        }
    }
    MONGO_UNREACHABLE;
}

void DocumentSourceCursor::Batch::clear() {
    _batchOfDocs.clear();
    _count = 0;
    _memUsageBytes = 0;
}

DocumentSource::GetNextResult DocumentSourceCursor::doGetNext() {
    if (_currentBatch.isEmpty()) {
        loadBatch();
    }

    // If we are tracking the oplog timestamp, update our cached latest optime.
    if (_resumeTrackingType == ResumeTrackingType::kOplog && _exec)
        _updateOplogTimestamp();
    else if (_resumeTrackingType == ResumeTrackingType::kNonOplog && _exec)
        _updateNonOplogResumeToken();

    if (_currentBatch.isEmpty()) {
        _currentBatch.clear();
        return GetNextResult::makeEOF();
    }

    return _currentBatch.dequeue();
}

bool DocumentSourceCursor::pullDataFromExecutor(OperationContext* opCtx) {
    PlanExecutor::ExecState state;
    Document resultObj;

    while ((state = _exec->getNextDocument(&resultObj, nullptr)) == PlanExecutor::ADVANCED) {
        boost::optional<BSONObj> resumeToken;
        if (_resumeTrackingType == ResumeTrackingType::kNonOplog)
            resumeToken = _exec->getPostBatchResumeToken();
        _currentBatch.enqueue(transformDoc(std::move(resultObj)), std::move(resumeToken));

        // As long as we're waiting for inserts, we shouldn't do any batching at this level we
        // need the whole pipeline to see each document to see if we should stop waiting.
        bool batchCountFull = _batchSizeCount != 0 && _currentBatch.count() >= _batchSizeCount;
        if (batchCountFull || _currentBatch.memUsageBytes() > _batchSizeBytes ||
            awaitDataState(opCtx).shouldWaitForInserts) {
            // Double the size for next batch when batch is full.
            if (batchCountFull && overflow::mul(_batchSizeCount, 2, &_batchSizeCount)) {
                _batchSizeCount = 0;  // Go unlimited if we overflow.
            }
            // Return false indicating the executor should not be destroyed.
            return false;
        }
    }

    tassert(10271304, "Expected PlanExecutor to be EOF", state == PlanExecutor::IS_EOF);

    // Keep the inner PlanExecutor alive if the cursor is tailable, since more results may
    // become available in the future, or if we are tracking the latest oplog resume inforation,
    // since we will need to retrieve the resume information the executor observed before
    // hitting EOF.
    if (_resumeTrackingType != ResumeTrackingType::kNone || pExpCtx->isTailableAwaitData()) {
        return false;
    }

    // Return true indicating the executor should be destroyed.
    return true;
}


void DocumentSourceCursor::loadBatch() {
    if (!_exec || _exec->isDisposed()) {
        // No more documents.
        return;
    }

    auto opCtx = pExpCtx->getOperationContext();
    CurOpFailpointHelpers::waitWhileFailPointEnabled(
        &hangBeforeDocumentSourceCursorLoadBatch,
        opCtx,
        "hangBeforeDocumentSourceCursorLoadBatch",
        []() {
            LOGV2(20895,
                  "Hanging aggregation due to 'hangBeforeDocumentSourceCursorLoadBatch' failpoint");
        },
        _exec->nss());

    tassert(5565800,
            "Expected PlanExecutor to use an external lock policy",
            _exec->lockPolicy() == PlanExecutor::LockPolicy::kLockExternally);

    // Acquire catalog resources and ensure they are released at the end of this block.
    _catalogResourceHandle->acquire(opCtx, *_exec);
    ON_BLOCK_EXIT([&]() { _catalogResourceHandle->release(); });

    _catalogResourceHandle->checkCanServeReads(opCtx, *_exec);
    RestoreContext restoreContext(nullptr);

    try {
        // As soon as we call restoreState(), the executor may hold onto storage engine
        // resources. This includes cases where restoreState() throws an exception. We must
        // guarantee that if an exception is thrown, the executor is cleaned up, along with
        // references to storage engine resources, before the catalog resources are released.  This
        // is done in the 'catch' block below.
        _exec->restoreState(restoreContext);

        const bool shouldDestroyExec = pullDataFromExecutor(opCtx);

        recordPlanSummaryStats();

        // At any given time only one operation can own the entirety of resources used by a
        // multi-document transaction. As we can perform a remote call during the query
        // execution we will check in the session to avoid deadlocks. If we don't release the
        // storage engine resources used here then we could have two operations interacting with
        // resources of a session at the same time. This will leave the plan in the saved state
        // as a side-effect.
        _exec->releaseAllAcquiredResources();

        if (!shouldDestroyExec) {
            return;
        }
    } catch (...) {
        // Record error details before re-throwing the exception.
        _execStatus = exceptionToStatus().withContext("Error in $cursor stage");

        // '_exec' must be cleaned up before the catalog resources are freed. Since '_exec' is a
        // member variable, and the catalog resources are maintained via a ScopeGuard within this
        // function, by default, the catalog resources will be released first. In order to get
        // around this, we dispose of '_exec' here.
        doDispose();

        throw;
    }

    // If we got here, there won't be any more documents and we no longer need our PlanExecutor, so
    // destroy it, but leave our current batch intact.
    cleanupExecutor();
}

void DocumentSourceCursor::_updateOplogTimestamp() {
    // If we are about to return a result, set our oplog timestamp to the optime of that result.
    if (!_currentBatch.isEmpty()) {
        const auto& ts = _currentBatch.peekFront().getField(repl::OpTime::kTimestampFieldName);
        invariant(ts.getType() == BSONType::bsonTimestamp);
        _latestOplogTimestamp = ts.getTimestamp();
        return;
    }

    // If we have no more results to return, advance to the latest oplog timestamp.
    _latestOplogTimestamp = _exec->getLatestOplogTimestamp();
}

void DocumentSourceCursor::_updateNonOplogResumeToken() {
    // If we are about to return a result, set our resume token to the one for that result.
    if (!_currentBatch.isEmpty()) {
        _latestNonOplogResumeToken = _currentBatch.peekFrontResumeToken();
        return;
    }

    // If we have no more results to return, advance to the latest executor resume token.
    _latestNonOplogResumeToken = _exec->getPostBatchResumeToken();
}

void DocumentSourceCursor::recordPlanSummaryStats() {
    invariant(_exec);
    _exec->getPlanExplainer().getSummaryStats(&_stats.planSummaryStats);
}

Value DocumentSourceCursor::serialize(const SerializationOptions& opts) const {
    // We never parse a DocumentSourceCursor, so we only serialize for explain. Since it's never
    // part of user input, there's no need to compute its query shape.
    if (!opts.isSerializingForExplain() || opts.isSerializingForQueryStats()) {
        return Value();
    }

    invariant(_exec);

    uassert(50660,
            "Mismatch between verbosity passed to serialize() and expression context verbosity",
            opts.verbosity == pExpCtx->getExplain());

    MutableDocument out;

    BSONObjBuilder explainStatsBuilder;

    {
        auto opCtx = pExpCtx->getOperationContext();
        auto secondaryNssList = _exec->getSecondaryNamespaces();
        boost::optional<AutoGetCollectionForReadMaybeLockFree> readLock = boost::none;
        auto initAutoGetFn = [&]() {
            readLock.emplace(pExpCtx->getOperationContext(),
                             _exec->nss(),
                             AutoGetCollection::Options{}.secondaryNssOrUUIDs(
                                 secondaryNssList.cbegin(), secondaryNssList.cend()));
        };
        bool isAnySecondaryCollectionNotLocal =
            initializeAutoGet(opCtx, _exec->nss(), secondaryNssList, initAutoGetFn);
        tassert(8322003,
                "Should have initialized AutoGet* after calling 'initializeAutoGet'",
                readLock.has_value());
        MultipleCollectionAccessor collections(opCtx,
                                               &readLock->getCollection(),
                                               readLock->getNss(),
                                               readLock->isAnySecondaryNamespaceAView() ||
                                                   isAnySecondaryCollectionNotLocal,
                                               secondaryNssList);
        Explain::explainStages(
            _exec.get(),
            collections,
            opts.verbosity.value(),
            _execStatus,
            _winningPlanTrialStats,
            BSONObj(),
            SerializationContext::stateCommandReply(pExpCtx->getSerializationContext()),
            BSONObj(),
            &explainStatsBuilder);
    }

    BSONObj explainStats = explainStatsBuilder.obj();
    invariant(explainStats["queryPlanner"]);
    out["queryPlanner"] = Value(explainStats["queryPlanner"]);

    if (opts.verbosity.value() >= ExplainOptions::Verbosity::kExecStats) {
        invariant(explainStats["executionStats"]);
        out["executionStats"] = Value(explainStats["executionStats"]);
    }

    return Value(DOC(getSourceName() << out.freezeToValue()));
}

void DocumentSourceCursor::detachFromOperationContext() {
    // Only detach the underlying executor if it hasn't been detached already.
    if (_exec && _exec->getOpCtx()) {
        _exec->detachFromOperationContext();
    }
}

void DocumentSourceCursor::reattachToOperationContext(OperationContext* opCtx) {
    if (_exec) {
        _exec->reattachToOperationContext(opCtx);
    }
}

void DocumentSourceCursor::doDispose() {
    _currentBatch.clear();
    if (!_exec || _exec->isDisposed()) {
        // We've already properly disposed of our PlanExecutor.
        return;
    }
    cleanupExecutor();
}

void DocumentSourceCursor::cleanupExecutor() {
    invariant(_exec);
    _exec->dispose(pExpCtx->getOperationContext());

    // Not freeing _exec if we're in explain mode since it will be used in serialize() to gather
    // execution stats.
    if (!pExpCtx->getExplain()) {
        _exec.reset();
    }
}

BSONObj DocumentSourceCursor::getPostBatchResumeToken() const {
    if (_resumeTrackingType == ResumeTrackingType::kOplog) {
        return ResumeTokenOplogTimestamp{getLatestOplogTimestamp()}.toBSON();
    } else if (_resumeTrackingType == ResumeTrackingType::kNonOplog) {
        return _latestNonOplogResumeToken;
    }
    return BSONObj{};
}

DocumentSourceCursor::~DocumentSourceCursor() {
    if (pExpCtx->getExplain()) {
        invariant(_exec->isDisposed());  // _exec should have at least been disposed.
    } else {
        invariant(!_exec);  // '_exec' should have been cleaned up via dispose() before destruction.
    }
}

DocumentSourceCursor::DocumentSourceCursor(
    const MultipleCollectionAccessor& collections,
    std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> exec,
    const boost::intrusive_ptr<CatalogResourceHandle>& catalogResourceHandle,
    const intrusive_ptr<ExpressionContext>& pCtx,
    CursorType cursorType,
    ResumeTrackingType resumeTrackingType)
    : DocumentSource(kStageName, pCtx),
      _currentBatch(cursorType),
      _catalogResourceHandle(catalogResourceHandle),
      _exec(std::move(exec)),
      _resumeTrackingType(resumeTrackingType),
      _queryFramework(_exec->getQueryFramework()) {
    // It is illegal for both 'kEmptyDocuments' to be set and _resumeTrackingType to be other than
    // 'kNone'.
    uassert(ErrorCodes::InvalidOptions,
            "The resumeToken is not compatible with this query",
            cursorType != CursorType::kEmptyDocuments ||
                resumeTrackingType == ResumeTrackingType::kNone);

    tassert(10240803,
            "Expected enclosed executor to use ShardRole",
            _exec->usesCollectionAcquisitions());

    // Later code in the DocumentSourceCursor lifecycle expects that '_exec' is in a saved state.
    _exec->saveState();

    auto&& explainer = _exec->getPlanExplainer();
    _planSummary = explainer.getPlanSummary();
    recordPlanSummaryStats();

    if (pExpCtx->getExplain()) {
        // It's safe to access the executor even if we don't have the collection lock since we're
        // just going to call getStats() on it.
        _winningPlanTrialStats = explainer.getWinningPlanTrialStats();
    }

    if (collections.hasMainCollection()) {
        const auto& coll = collections.getMainCollection();
        CollectionIndexUsageTrackerDecoration::recordCollectionIndexUsage(
            coll.get(),
            _stats.planSummaryStats.collectionScans,
            _stats.planSummaryStats.collectionScansNonTailable,
            _stats.planSummaryStats.indexesUsed);
    }
    for (auto& [nss, coll] : collections.getSecondaryCollections()) {
        if (coll) {
            PlanSummaryStats stats;
            explainer.getSecondarySummaryStats(nss, &stats);
            CollectionIndexUsageTrackerDecoration::recordCollectionIndexUsage(
                coll.get(),
                stats.collectionScans,
                stats.collectionScansNonTailable,
                stats.indexesUsed);
        }
    }

    initializeBatchSizeCounts();
    _batchSizeBytes = static_cast<size_t>(internalDocumentSourceCursorBatchSizeBytes.load());
}

void DocumentSourceCursor::initializeBatchSizeCounts() {
    // '0' means there's no limitation.
    _batchSizeCount = 0;
    if (auto cq = _exec->getCanonicalQuery()) {
        if (cq->getFindCommandRequest().getLimit().has_value()) {
            // $limit is pushed down into executor, skipping batch size count limitation.
            return;
        }
        for (const auto& ds : cq->cqPipeline()) {
            if (ds->getSourceName() == DocumentSourceLimit::kStageName) {
                // $limit is pushed down into executor, skipping batch size count limitation.
                return;
            }
        }
    }
    // No $limit is pushed down into executor, reading limit from knobs.
    _batchSizeCount = internalDocumentSourceCursorInitialBatchSize.load();
}

intrusive_ptr<DocumentSourceCursor> DocumentSourceCursor::create(
    const MultipleCollectionAccessor& collections,
    std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> exec,
    const boost::intrusive_ptr<CatalogResourceHandle>& catalogResourceHandle,
    const intrusive_ptr<ExpressionContext>& pExpCtx,
    CursorType cursorType,
    ResumeTrackingType resumeTrackingType) {
    intrusive_ptr<DocumentSourceCursor> source(new DocumentSourceCursor(collections,
                                                                        std::move(exec),
                                                                        catalogResourceHandle,
                                                                        pExpCtx,
                                                                        cursorType,
                                                                        resumeTrackingType));
    return source;
}
}  // namespace mongo
