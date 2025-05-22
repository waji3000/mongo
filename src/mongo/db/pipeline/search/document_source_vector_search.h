/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_limit.h"
#include "mongo/db/query/search/internal_search_mongot_remote_spec_gen.h"
#include "mongo/executor/task_executor_cursor.h"

namespace mongo {

/**
 * A class to retrieve vector search results from a mongot process.
 */
class DocumentSourceVectorSearch : public DocumentSource {
public:
    const BSONObj kSortSpec = BSON("$vectorSearchScore" << -1);
    static constexpr StringData kStageName = "$vectorSearch"_sd;
    static constexpr StringData kLimitFieldName = "limit"_sd;
    static constexpr StringData kFilterFieldName = "filter"_sd;
    static constexpr StringData kIndexFieldName = "index"_sd;
    static constexpr StringData kNumCandidatesFieldName = "numCandidates"_sd;

    DocumentSourceVectorSearch(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                               std::shared_ptr<executor::TaskExecutor> taskExecutor,
                               BSONObj originalSpec,
                               boost::optional<SearchQueryViewSpec> view = boost::none);

    static std::list<boost::intrusive_ptr<DocumentSource>> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

    std::list<boost::intrusive_ptr<DocumentSource>> desugar();

    const char* getSourceName() const override {
        return kStageName.data();
    }

    static const Id& id;

    Id getId() const override {
        return id;
    }

    boost::optional<DistributedPlanLogic> distributedPlanLogic() override {
        DistributedPlanLogic logic;
        logic.shardsStage = this;
        if (_limit) {
            logic.mergingStages = {DocumentSourceLimit::create(pExpCtx, *_limit)};
        }
        logic.mergeSortPattern = kSortSpec;
        return logic;
    }

    void addVariableRefs(std::set<Variables::Id>* refs) const final {}

    DepsTracker::State getDependencies(DepsTracker* deps) const final {
        // This stage doesn't currently support tracking field dependencies since mongot is
        // responsible for determining what fields to return. We do need to track metadata
        // dependencies though, so downstream stages know they are allowed to access
        // "vectorSearchScore" metadata.
        // TODO SERVER-101100 Implement logic for dependency analysis.

        deps->setMetadataAvailable(DocumentMetadataFields::kVectorSearchScore);
        return DepsTracker::State::NOT_SUPPORTED;
    }

    boost::intrusive_ptr<DocumentSource> clone(
        const boost::intrusive_ptr<ExpressionContext>& newExpCtx) const override {
        auto expCtx = newExpCtx ? newExpCtx : pExpCtx;
        return make_intrusive<DocumentSourceVectorSearch>(
            expCtx, _taskExecutor, _originalSpec.copy(), _view);
    }

    StageConstraints constraints(Pipeline::SplitState pipeState) const final {
        StageConstraints constraints(StreamType::kStreaming,
                                     PositionRequirement::kFirst,
                                     HostTypeRequirement::kAnyShard,
                                     DiskUseRequirement::kNoDiskUse,
                                     FacetRequirement::kNotAllowed,
                                     TransactionRequirement::kNotAllowed,
                                     LookupRequirement::kNotAllowed,
                                     UnionRequirement::kNotAllowed,
                                     ChangeStreamRequirement::kDenylist);
        // TODO: SERVER-85426 The constraint should now always be UnionRequirement::kAllowed.
        // TODO: BACKPORT-22945 (8.0) Ensure that using this feature inside a view definition is not
        // permitted.
        if (enableUnionWithVectorSearch.load()) {
            constraints.unionRequirement = UnionRequirement::kAllowed;
        }
        constraints.requiresInputDocSource = false;
        constraints.noFieldModifications = true;
        return constraints;
    };

protected:
    Value serialize(const SerializationOptions& opts) const override;

    Pipeline::SourceContainer::iterator doOptimizeAt(Pipeline::SourceContainer::iterator itr,
                                                     Pipeline::SourceContainer* container) override;

private:
    // Get the next record from mongot. This will establish the mongot cursor on the first call.
    GetNextResult doGetNext() final;

    boost::optional<BSONObj> getNext();

    DocumentSource::GetNextResult getNextAfterSetup();

    // Initialize metrics related to the $vectorSearch stage on the OpDebug object.
    void initializeOpDebugVectorSearchMetrics();

    /**
     * Attempts a pipeline optimization that removes a $sort stage that comes after the output of
     * of mongot, if the resulting documents from mongot are sorted by the same criteria as the
     * $sort ('vectorSearchScore').
     *
     * Also, this optimization only applies to cases where the $sort comes directly after this
     * stage.
     * TODO SERVER-96068 generalize this optimization to cases where any number of stages that
     * preserve sort order come between this stage and the sort.
     *
     * Returns a pair of the iterator to return to the optimizer, and a bool of whether or not the
     * optimization was successful. If optimization was successful, the container will be modified
     * appropriately.
     */
    std::pair<Pipeline::SourceContainer::iterator, bool> _attemptSortAfterVectorSearchOptimization(
        Pipeline::SourceContainer::iterator itr, Pipeline::SourceContainer* container);

    std::unique_ptr<MatchExpression> _filterExpr;

    std::shared_ptr<executor::TaskExecutor> _taskExecutor;

    std::unique_ptr<executor::TaskExecutorCursor> _cursor;

    // Store the cursorId. We need to store it on the document source because the id on the
    // TaskExecutorCursor will be set to zero after the final getMore after the cursor is
    // exhausted.
    boost::optional<CursorId> _cursorId{boost::none};

    // Limit value for the pipeline as a whole. This is not the limit that we send to mongot,
    // rather, it is used when adding the $limit stage to the merging pipeline in a sharded cluster.
    // This allows us to limit the documents that are returned from the shards as much as possible
    // without adding complicated rules for pipeline splitting.
    // The limit that we send to mongot is received and stored on the '_request' object above.
    boost::optional<long long> _limit;

    // Keep track of the original request BSONObj's extra fields in case there were fields mongod
    // doesn't know about that mongot will need later.
    BSONObj _originalSpec;

    // If applicable, hold the view information.
    boost::optional<SearchQueryViewSpec> _view;
};
}  // namespace mongo
