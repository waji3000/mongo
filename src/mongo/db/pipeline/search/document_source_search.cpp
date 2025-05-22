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

#include "mongo/db/pipeline/search/document_source_search.h"

#include "mongo/db/exec/document_value/document_metadata_fields.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_internal_shard_filter.h"
#include "mongo/db/pipeline/document_source_limit.h"
#include "mongo/db/pipeline/document_source_replace_root.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/search/document_source_internal_search_id_lookup.h"
#include "mongo/db/pipeline/search/document_source_internal_search_mongot_remote.h"
#include "mongo/db/pipeline/search/lite_parsed_search.h"
#include "mongo/db/pipeline/skip_and_limit.h"
#include "mongo/db/query/search/manage_search_index_request_gen.h"
#include "mongo/db/query/search/mongot_cursor.h"
#include "mongo/db/views/resolved_view.h"
#include "mongo/platform/compiler.h"
#include <boost/optional/optional.hpp>

namespace mongo {

using boost::intrusive_ptr;
using std::list;

namespace {
/** Helper written in a particular redundant way to work around a GCC false-positive warning. */
StringData removePrefixWorkaround(StringData key, StringData pre) {
    MONGO_COMPILER_DIAGNOSTIC_PUSH
    MONGO_COMPILER_DIAGNOSTIC_IGNORED_TRANSITIONAL("-Warray-bounds")
    if (!key.starts_with(pre))
        return key;
    key.remove_prefix(pre.size());
    MONGO_COMPILER_DIAGNOSTIC_POP
    return key;
}
}  // namespace

REGISTER_DOCUMENT_SOURCE(search,
                         LiteParsedSearchStage::parse,
                         DocumentSourceSearch::createFromBson,
                         AllowedWithApiStrict::kNeverInVersion1);

ALLOCATE_DOCUMENT_SOURCE_ID(search, DocumentSourceSearch::id)

// $searchBeta is supported as an alias for $search for compatibility with applications that used
// search during its beta period.
REGISTER_DOCUMENT_SOURCE(searchBeta,
                         LiteParsedSearchStage::parse,
                         DocumentSourceSearch::createFromBson,
                         AllowedWithApiStrict::kNeverInVersion1);

const char* DocumentSourceSearch::getSourceName() const {
    return kStageName.data();
}

Value DocumentSourceSearch::serialize(const SerializationOptions& opts) const {
    // If we aren't serializing for query stats or explain, serialize the full spec.
    // If we are in a router, serialize the full spec.
    // Otherwise, just serialize the mongotQuery.
    if ((!opts.isSerializingForQueryStats() && !opts.isSerializingForExplain()) ||
        pExpCtx->getInRouter()) {
        MutableDocument spec{Document(_spec.toBSON())};
        if (_view) {
            spec["view"] = Value(_view->toBSON());
        }
        return Value(Document{{getSourceName(), spec.freezeToValue()}});
    }
    return Value(DOC(getSourceName() << opts.serializeLiteral(_spec.getMongotQuery())));
}

intrusive_ptr<DocumentSource> DocumentSourceSearch::createFromBson(
    BSONElement elem, const intrusive_ptr<ExpressionContext>& expCtx) {
    mongot_cursor::throwIfNotRunningWithMongotHostConfigured(expCtx);

    uassert(ErrorCodes::FailedToParse,
            str::stream() << "$search value must be an object. Found: " << typeName(elem.type()),
            elem.type() == BSONType::Object);
    auto specObj = elem.embeddedObject();
    // If kMongotQueryFieldName is present, this is the case that we re-create the
    // DocumentSource from a serialized DocumentSourceSearch that was originally parsed on a
    // router.
    // We need to make sure that the mongotQuery BSONObj in the InternalSearchMongotRemoteSpec is
    // owned so that it persists safely to GetMores. Since the IDL type is object_owned, using the
    // parse() function will make sure it's owned. Manually constructing the spec does _not_ ensure
    // the owned is owned, which is why we call specObj.getOwned().
    InternalSearchMongotRemoteSpec spec =
        specObj.hasField(InternalSearchMongotRemoteSpec::kMongotQueryFieldName)
        ? InternalSearchMongotRemoteSpec::parse(IDLParserContext(kStageName), specObj)
        : InternalSearchMongotRemoteSpec(specObj.getOwned());

    auto view = search_helpers::getViewFromBSONObj(expCtx, specObj);

    return make_intrusive<DocumentSourceSearch>(expCtx, std::move(spec), view);
}

std::list<intrusive_ptr<DocumentSource>> DocumentSourceSearch::desugar() {
    if (_view) {
        // This function will throw if the view violates validation rules for supporting
        // mongot-indexed views.
        search_helpers::validateViewPipeline(*_view);
    }

    auto executor =
        executor::getMongotTaskExecutor(pExpCtx->getOperationContext()->getServiceContext());
    std::list<intrusive_ptr<DocumentSource>> desugaredPipeline;
    // 'getBoolField' returns false if the field is not present.
    bool storedSource = _spec.getMongotQuery().getBoolField(mongot_cursor::kReturnStoredSourceArg);

    auto spec =
        InternalSearchMongotRemoteSpec::parseOwned(IDLParserContext(kStageName), _spec.toBSON());
    // Pass the limit in when there is no idLookup stage, and use the limit for mongotDocsRequested.
    // TODO: SERVER-76591 Remove special limit in favor of regular sharded limit optimization.
    spec.setMongotDocsRequested(spec.getLimit());
    if (!storedSource) {
        spec.setLimit(boost::none);
    }
    // Remove mergingPipeline info since it is not useful for
    // DocumentSourceInternalSearchMongotRemote.
    spec.setMergingPipeline(boost::none);

    // Extract the viewPipeline to share with DocumentSourceInternalSearchIdLookUp as it is required
    // for mongot-indexed views.
    boost::optional<std::vector<BSONObj>> viewPipeline;
    if (_view) {
        viewPipeline = boost::make_optional(_view->getEffectivePipeline());
    }

    auto mongoTRemoteStage = make_intrusive<DocumentSourceInternalSearchMongotRemote>(
        std::move(spec), pExpCtx, executor, _view);
    desugaredPipeline.push_back(mongoTRemoteStage);

    // If 'returnStoredSource' is true, we don't want to do idLookup. Instead, promote the fields in
    // 'storedSource' to root.
    if (storedSource) {
        // {$replaceRoot: {newRoot: {$ifNull: ["$storedSource", "$$ROOT"]}}
        // 'storedSource' is not always present in the document from mongot. If it's present, use it
        // as the root. Otherwise keep the original document.
        BSONObj replaceRootSpec =
            BSON("$replaceRoot" << BSON(
                     "newRoot" << BSON(
                         "$ifNull" << BSON_ARRAY("$" + kProtocolStoredFieldsName << "$$ROOT"))));
        desugaredPipeline.push_back(
            DocumentSourceReplaceRoot::createFromBson(replaceRootSpec.firstElement(), pExpCtx));
        // Note: intentionally not including a shard filtering operator here. The isolation
        // semantics are already weaker here so this was deemed OK. Potentially part of that
        // conversation: the documents are not guaranteed to have the shard key, and we don't have
        // an idLookup to go get it.
        return desugaredPipeline;
    } else {
        auto shardFilterer = DocumentSourceInternalShardFilter::buildIfNecessary(pExpCtx);
        // idLookup must always be immediately after the $mongotRemote stage, which is always first
        // in the pipeline.
        auto idLookupStage = make_intrusive<DocumentSourceInternalSearchIdLookUp>(
            pExpCtx,
            _spec.getLimit().value_or(0),
            buildExecShardFilterPolicy(shardFilterer),
            viewPipeline);
        desugaredPipeline.insert(std::next(desugaredPipeline.begin()), idLookupStage);
        // In this case, connect the shared search state of these two stages of the pipeline
        // for batch size tuning.
        mongoTRemoteStage->setSearchIdLookupMetrics(idLookupStage->getSearchIdLookupMetrics());
        if (shardFilterer) {
            desugaredPipeline.push_back(std::move(shardFilterer));
        }
    }

    return desugaredPipeline;
}

StageConstraints DocumentSourceSearch::constraints(Pipeline::SplitState pipeState) const {
    auto constraints = DocumentSourceInternalSearchMongotRemote::getSearchDefaultConstraints();
    if (!isStoredSource()) {
        constraints.noFieldModifications = true;
    }
    return constraints;
}
bool checkRequiresSearchSequenceToken(Pipeline::SourceContainer::iterator itr,
                                      Pipeline::SourceContainer* container) {
    DepsTracker deps;
    while (itr != container->end()) {
        auto nextStage = itr->get();
        nextStage->getDependencies(&deps);
        ++itr;
    }
    return deps.getNeedsMetadata(DocumentMetadataFields::kSearchSequenceToken);
}

Pipeline::SourceContainer::iterator DocumentSourceSearch::doOptimizeAt(
    Pipeline::SourceContainer::iterator itr, Pipeline::SourceContainer* container) {
    // In the case where the query has an extractable limit, we send that limit to mongot as a guide
    // for the number of documents mongot should return (rather than the default batchsize).
    // Move past the current stage ($search).
    auto stageItr = std::next(itr);
    // Only attempt to get the limit or requiresSearchSequenceToken from the query if there are
    // further stages in the pipeline.
    if (stageItr != container->end()) {
        // Calculate the extracted limit without modifying the rest of the pipeline.
        boost::optional<long long> limit = getUserLimit(stageItr, container);
        if (limit.has_value()) {
            _spec.setLimit((int64_t)*limit);
        }
        if (!_spec.getRequiresSearchSequenceToken()) {
            _spec.setRequiresSearchSequenceToken(checkRequiresSearchSequenceToken(itr, container));
        }
    }

    // If this $search stage was parsed from a spec sent from a router, the current pipeline may be
    // missing a reference to $$SEARCH_META that is only in the merging pipeline, so we shouldn't
    // compute requiresSearchMetaCursor based on the current pipeline.
    if (!_spec.getMetadataMergeProtocolVersion().has_value()) {
        // Determine whether the pipeline references the $$SEARCH_META variable. We won't insert a
        // $setVariableFromSubPipeline stage until we split the pipeline (see
        // distributedPlanLogic()), but at that point we don't have access to the full pipeline to
        // know whether we need it.
        _spec.setRequiresSearchMetaCursor(
            std::any_of(std::next(itr), container->end(), [](const auto& itr) {
                return search_helpers::hasReferenceToSearchMeta(*itr);
            }));
    }

    return std::next(itr);
}

void DocumentSourceSearch::validateSortSpec(boost::optional<BSONObj> sortSpec) {
    if (sortSpec) {
        // Verify that sortSpec do not contain dots after '$searchSortValues', as we expect it
        // to only contain top-level fields (no nested objects).
        for (auto&& k : *sortSpec) {
            auto key = k.fieldNameStringData();
            key = removePrefixWorkaround(key, mongot_cursor::kSearchSortValuesFieldPrefix);
            tassert(7320404,
                    fmt::format("planShardedSearch returned sortSpec with key containing a dot: {}",
                                key),
                    key.find('.', 0) == std::string::npos);
        }
    }
}

boost::optional<DocumentSource::DistributedPlanLogic> DocumentSourceSearch::distributedPlanLogic() {
    // If 'searchReturnEofImmediately' is set, we return this stage as is because we don't expect to
    // return any results. More precisely, we wish to avoid calling 'planShardedSearch' when no
    // mongot is set up.
    if (MONGO_unlikely(searchReturnEofImmediately.shouldFail())) {
        return DistributedPlanLogic{this, nullptr, boost::none};
    }
    if (!_spec.getMetadataMergeProtocolVersion().has_value()) {
        // Issue a planShardedSearch call to mongot if we have not yet done so, and validate the
        // sortSpec in response. Note that this is done for unsharded collections as well as sharded
        // ones because the two collection types are treated the same in the context of shard
        // targeting.
        search_helpers::planShardedSearch(pExpCtx, &_spec);
        validateSortSpec(_spec.getSortSpec());
    }

    // Construct the DistributedPlanLogic for sharded planning based on the information returned
    // from mongot.
    DistributedPlanLogic logic;
    logic.shardsStage = this;
    if (_spec.getMergingPipeline() && _spec.getRequiresSearchMetaCursor()) {
        logic.mergingStages = {DocumentSourceSetVariableFromSubPipeline::create(
            pExpCtx,
            Pipeline::parse(*_spec.getMergingPipeline(), pExpCtx),
            Variables::kSearchMetaId)};
    }

    logic.mergeSortPattern = _spec.getSortSpec().has_value() ? _spec.getSortSpec()->getOwned()
                                                             : mongot_cursor::kSortSpec;

    logic.needsSplit = false;
    logic.canMovePast = canMovePastDuringSplit;

    return logic;
}

DepsTracker::State DocumentSourceSearch::getDependencies(DepsTracker* deps) const {
    // This stage doesn't currently support tracking field dependencies since mongot is
    // responsible for determining what fields to return. We do need to track metadata
    // dependencies though, so downstream stages know they are allowed to access "searchScore"
    // metadata.
    // TODO SERVER-101100 Implement logic for dependency analysis.

    deps->setMetadataAvailable(DocumentMetadataFields::kSearchScore);
    if (hasScoreDetails()) {
        deps->setMetadataAvailable(DocumentMetadataFields::kSearchScoreDetails);
    }
    return DepsTracker::State::NOT_SUPPORTED;
}

bool DocumentSourceSearch::canMovePastDuringSplit(const DocumentSource& ds) {
    // Check if next stage uses the variable.
    return !search_helpers::hasReferenceToSearchMeta(ds) &&
        ds.constraints().preservesOrderAndMetadata;
}

}  // namespace mongo
