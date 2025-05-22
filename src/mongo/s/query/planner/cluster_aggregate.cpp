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


#include <absl/container/node_hash_set.h>
#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/smart_ptr.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>
#include <cstdint>
#include <fmt/format.h>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <boost/optional/optional.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/crypto/fle_field_schema_gen.h"
#include "mongo/db/basic_types.h"
#include "mongo/db/basic_types_gen.h"
#include "mongo/db/catalog/collection_uuid_mismatch_info.h"
#include "mongo/db/curop.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/fle_crud.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/pipeline/aggregation_request_helper.h"
#include "mongo/db/pipeline/document_source_internal_unpack_bucket.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_context_diagnostic_printer.h"
#include "mongo/db/pipeline/lite_parsed_pipeline.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/process_interface/mongos_process_interface.h"
#include "mongo/db/pipeline/search/document_source_search.h"
#include "mongo/db/pipeline/search/search_helper.h"
#include "mongo/db/pipeline/search/search_helper_bson_obj.h"
#include "mongo/db/pipeline/sharded_agg_helpers.h"
#include "mongo/db/pipeline/visitors/document_source_visitor_docs_needed_bounds.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/explain_common.h"
#include "mongo/db/query/explain_options.h"
#include "mongo/db/query/query_settings/query_settings_service.h"
#include "mongo/db/query/query_shape/agg_cmd_shape.h"
#include "mongo/db/query/query_stats/agg_key.h"
#include "mongo/db/query/query_stats/key.h"
#include "mongo/db/query/query_stats/query_stats.h"
#include "mongo/db/query/shard_key_diagnostic_printer.h"
#include "mongo/db/query/tailable_mode_gen.h"
#include "mongo/db/query/timeseries/timeseries_rewrites.h"
#include "mongo/db/raw_data_operation.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/server_options.h"
#include "mongo/db/shard_id.h"
#include "mongo/db/timeseries/timeseries_gen.h"
#include "mongo/db/timeseries/timeseries_options.h"
#include "mongo/db/version_context.h"
#include "mongo/db/views/resolved_view.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/chunk_manager.h"
#include "mongo/s/client/num_hosts_targeted_metrics.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/database_version.h"
#include "mongo/s/grid.h"
#include "mongo/s/query/exec/cluster_cursor_manager.h"
#include "mongo/s/query/exec/collect_query_stats_mongos.h"
#include "mongo/s/query/planner/cluster_aggregate.h"
#include "mongo/s/query/planner/cluster_aggregation_planner.h"
#include "mongo/s/transaction_router.h"
#include "mongo/s/type_collection_common_types_gen.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/net/socket_utils.h"
#include "mongo/util/str.h"
#include "mongo/util/string_map.h"
#include "mongo/util/uuid.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {

constexpr unsigned ClusterAggregate::kMaxViewRetries;
using sharded_agg_helpers::PipelineDataSource;

namespace {
// Ticks for server-side Javascript deprecation log messages.
Rarely _samplerAccumulatorJs, _samplerFunctionJs;

// "Resolve" involved namespaces into a map. We won't try to execute anything on a mongos, but we
// still have to populate this map so that any $lookups, etc. will be able to have a resolved view
// definition. It's okay that this is incorrect, we will repopulate the real namespace map on the
// mongod. Note that this function must be called before forwarding an aggregation command on an
// unsharded collection, in order to verify that the involved namespaces are allowed to be sharded.
auto resolveInvolvedNamespaces(const stdx::unordered_set<NamespaceString>& involvedNamespaces) {
    ResolvedNamespaceMap resolvedNamespaces;
    for (auto&& nss : involvedNamespaces) {
        resolvedNamespaces.try_emplace(nss, nss, std::vector<BSONObj>{});
    }
    return resolvedNamespaces;
}

Document serializeForPassthrough(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                 const AggregateCommandRequest& request,
                                 const NamespaceString& executionNs) {
    auto req = request;

    // Reset all generic arguments besides those needed for the aggregation itself.
    // Other generic arguments that need to be set like txnNumber, lsid, etc. will be attached
    // later.
    // TODO: SERVER-90827 Only reset arguments not suitable for passing through to shards.
    auto maxTimeMS = req.getMaxTimeMS();
    auto readConcern = req.getReadConcern();
    auto writeConcern = req.getWriteConcern();
    auto rawData = req.getRawData();
    req.setGenericArguments({});
    req.setMaxTimeMS(maxTimeMS);
    req.setReadConcern(std::move(readConcern));
    req.setWriteConcern(std::move(writeConcern));
    req.setRawData(rawData);
    aggregation_request_helper::addQuerySettingsToRequest(req, expCtx);

    auto cmdObj =
        isRawDataOperation(expCtx->getOperationContext()) && req.getNamespace() != executionNs
        ? rewriteCommandForRawDataOperation<AggregateCommandRequest>(req.toBSON(),
                                                                     executionNs.coll())
        : req.toBSON();

    return Document(cmdObj);
}

// Build an appropriate ExpressionContext for the pipeline. This helper instantiates an appropriate
// collator, creates a MongoProcessInterface for use by the pipeline's stages, and sets the
// collection UUID if provided.
boost::intrusive_ptr<ExpressionContext> makeExpressionContext(
    OperationContext* opCtx,
    const AggregateCommandRequest& request,
    const boost::optional<CollectionRoutingInfo>& cri,
    const NamespaceString& executionNs,
    BSONObj collationObj,
    boost::optional<UUID> uuid,
    ResolvedNamespaceMap resolvedNamespaces,
    bool hasChangeStream,
    boost::optional<ExplainOptions::Verbosity> verbosity) {

    std::unique_ptr<CollatorInterface> collation;
    if (!collationObj.isEmpty()) {
        // This will be null if attempting to build an interface for the simple collator.
        collation = uassertStatusOK(
            CollatorFactoryInterface::get(opCtx->getServiceContext())->makeFromBSON(collationObj));
    }

    // Create the expression context, and set 'inRouter' to true. We explicitly do *not* set
    // mergeCtx->tempDir.
    const bool canBeRejected = query_settings::canPipelineBeRejected(request.getPipeline());
    auto mergeCtx = ExpressionContextBuilder{}
                        .fromRequest(opCtx, request)
                        .explain(verbosity)
                        .collator(std::move(collation))
                        .mongoProcessInterface(std::make_shared<MongosProcessInterface>(
                            Grid::get(opCtx)->getExecutorPool()->getArbitraryExecutor()))
                        .resolvedNamespace(std::move(resolvedNamespaces))
                        .mayDbProfile(true)
                        .inRouter(true)
                        .collUUID(uuid)
                        .canBeRejected(canBeRejected)
                        .build();

    if (!(cri && cri->hasRoutingTable()) && collationObj.isEmpty()) {
        mergeCtx->setIgnoreCollator();
    }

    // Serialize the 'AggregateCommandRequest' and save it so that the original command can be
    // reconstructed for dispatch to a new shard, which is sometimes necessary for change streams
    // pipelines.
    if (hasChangeStream) {
        mergeCtx->setOriginalAggregateCommand(
            serializeForPassthrough(mergeCtx, request, executionNs).toBson());
    }

    return mergeCtx;
}

void appendEmptyResultSetWithStatus(OperationContext* opCtx,
                                    const NamespaceString& nss,
                                    Status status,
                                    BSONObjBuilder* result) {
    // Rewrite ShardNotFound as NamespaceNotFound so that appendEmptyResultSet swallows it.
    if (status == ErrorCodes::ShardNotFound) {
        status = {ErrorCodes::NamespaceNotFound, status.reason()};
    }
    collectQueryStatsMongos(opCtx, std::move(CurOp::get(opCtx)->debug().queryStatsInfo.key));
    appendEmptyResultSet(opCtx, *result, status, nss);
}

void updateHostsTargetedMetrics(OperationContext* opCtx,
                                const NamespaceString& executionNss,
                                const boost::optional<CollectionRoutingInfo>& cri,
                                const stdx::unordered_set<NamespaceString>& involvedNamespaces) {
    if (!cri)
        return;

    // Create a set of ShardIds that own a chunk belonging to any of the collections involved in
    // this pipeline. This will be used to determine whether the pipeline targeted all of the shards
    // that own chunks for any collection involved or not.
    //
    // Note that this will only take into account collections that are sharded. If the namespace is
    // an unsharded collection we will not track which shard owns it here. This is to preserve
    // semantics of the tracked host metrics since unsharded collections are tracked separately on
    // its own value.
    std::set<ShardId> shardsOwningChunks = [&]() {
        std::set<ShardId> shardsIds;

        if (cri->isSharded()) {
            std::set<ShardId> shardIdsForNs;
            // Note: It is fine to use 'getAllShardIds_UNSAFE_NotPointInTime' here because the
            // result is only used to update stats.
            cri->getChunkManager().getAllShardIds_UNSAFE_NotPointInTime(&shardIdsForNs);
            for (const auto& shardId : shardIdsForNs) {
                shardsIds.insert(shardId);
            }
        }

        for (const auto& nss : involvedNamespaces) {
            if (nss == executionNss)
                continue;

            const auto resolvedNsCri =
                uassertStatusOK(getCollectionRoutingInfoForTxnCmd(opCtx, nss));
            if (resolvedNsCri.isSharded()) {
                std::set<ShardId> shardIdsForNs;
                // Note: It is fine to use 'getAllShardIds_UNSAFE_NotPointInTime' here because the
                // result is only used to update stats.
                resolvedNsCri.getChunkManager().getAllShardIds_UNSAFE_NotPointInTime(
                    &shardIdsForNs);
                for (const auto& shardId : shardIdsForNs) {
                    shardsIds.insert(shardId);
                }
            }
        }

        return shardsIds;
    }();

    auto nShardsTargeted = CurOp::get(opCtx)->debug().nShards;
    if (nShardsTargeted > 0) {
        // shardsOwningChunks will only contain something if we targeted a sharded collection in the
        // pipeline.
        bool hasTargetedShardedCollection = !shardsOwningChunks.empty();
        auto targetType = NumHostsTargetedMetrics::get(opCtx).parseTargetType(
            opCtx, nShardsTargeted, shardsOwningChunks.size(), hasTargetedShardedCollection);
        NumHostsTargetedMetrics::get(opCtx).addNumHostsTargeted(
            NumHostsTargetedMetrics::QueryType::kAggregateCmd, targetType);
    }
}

/**
 * Performs validations related to API versioning, time-series stages, and general command
 * validation.
 * Throws UserAssertion if any of the validations fails
 *     - validation of API versioning on each stage on the pipeline
 *     - validation of API versioning on 'AggregateCommandRequest' request
 *     - validation of time-series related stages
 *     - validation of command parameters
 */
void performValidationChecks(const OperationContext* opCtx,
                             const AggregateCommandRequest& request,
                             const LiteParsedPipeline& liteParsedPipeline) {
    liteParsedPipeline.validate(opCtx);
    aggregation_request_helper::validateRequestForAPIVersion(opCtx, request);
    aggregation_request_helper::validateRequestFromClusterQueryWithoutShardKey(request);

    uassert(51028, "Cannot specify exchange option to a router", !request.getExchange());
    uassert(51143,
            "Cannot specify runtime constants option to a router",
            !request.getLegacyRuntimeConstants());
    uassert(51089,
            str::stream() << "Internal parameter(s) ["
                          << AggregateCommandRequest::kNeedsMergeFieldName << ", "
                          << AggregateCommandRequest::kFromRouterFieldName
                          << "] cannot be set to 'true' when sent to router",
            !request.getNeedsMerge() && !aggregation_request_helper::getFromRouter(request));
    uassert(ErrorCodes::BadValue,
            "Aggregate queries on router may not request or provide a resume token",
            !request.getRequestResumeToken() && !request.getResumeAfter() && !request.getStartAt());
}

/**
 * Appends the give 'bucketMaxSpanSeconds' attribute to the given object buidler for a timeseries
 * unpack stage. 'tsFields' will contain values from the catalog if they were found. Otherwise, we
 * will use the value produced by the kickback exception.
 */
void appendBucketMaxSpan(BSONObjBuilder& bob,
                         const TypeCollectionTimeseriesFields* tsFields,
                         BSONElement oldMaxSpanSeconds) {
    if (tsFields) {
        int32_t bucketSpan = 0;
        auto maxSpanSeconds = tsFields->getBucketMaxSpanSeconds();
        auto granularity = tsFields->getGranularity();
        if (maxSpanSeconds) {
            bucketSpan = *maxSpanSeconds;
        } else {
            bucketSpan = timeseries::getMaxSpanSecondsFromGranularity(
                granularity.get_value_or(BucketGranularityEnum::Seconds));
        }
        bob.append(DocumentSourceInternalUnpackBucket::kBucketMaxSpanSeconds, bucketSpan);
    } else {
        bob.append(oldMaxSpanSeconds);
    }
}

/**
 * Rebuilds the pipeline and possibly updating the granularity value for the 'bucketMaxSpanSeconds'
 * field in the $_internalUnpackBucket stage, since it may be out of date if a 'collMod' operation
 * changed it after the DB primary threw the kickback exception. Also removes the
 * 'usesExtendedRange' field altogether, since an accurate value for this can only be known on the
 * mongod for the respective shard.
 */
std::vector<BSONObj> patchPipelineForTimeSeriesQuery(
    const std::vector<BSONObj>& pipeline, const TypeCollectionTimeseriesFields* tsFields) {

    std::vector<BSONObj> newPipeline;
    for (auto& stage : pipeline) {
        if (stage.firstElementFieldNameStringData() ==
                DocumentSourceInternalUnpackBucket::kStageNameInternal ||
            stage.firstElementFieldNameStringData() ==
                DocumentSourceInternalUnpackBucket::kStageNameExternal) {
            BSONObjBuilder newOptions;
            for (auto& elem : stage.firstElement().Obj()) {
                if (elem.fieldNameStringData() ==
                    DocumentSourceInternalUnpackBucket::kBucketMaxSpanSeconds) {
                    appendBucketMaxSpan(newOptions, tsFields, elem);
                } else if (elem.fieldNameStringData() ==
                           DocumentSourceInternalUnpackBucket::kUsesExtendedRange) {
                    // Omit so that target shard can amend with correct value.
                } else {
                    newOptions.append(elem);
                }
            }
            newPipeline.push_back(
                BSON(stage.firstElementFieldNameStringData() << newOptions.obj()));
            continue;
        }
        newPipeline.push_back(stage);
    }
    return newPipeline;
}

/**
 * Builds an expCtx with which to parse the request's pipeline, then parses the pipeline and
 * registers the pre-optimized pipeline with query stats collection.
 */
std::unique_ptr<Pipeline, PipelineDeleter> parsePipelineAndRegisterQueryStats(
    OperationContext* opCtx,
    const stdx::unordered_set<NamespaceString>& involvedNamespaces,
    const ClusterAggregate::Namespaces& nsStruct,
    AggregateCommandRequest& request,
    boost::optional<CollectionRoutingInfo> cri,
    bool hasChangeStream,
    bool shouldDoFLERewrite,
    bool requiresCollationForParsingUnshardedAggregate,
    boost::optional<ResolvedView> resolvedView,
    boost::optional<ExplainOptions::Verbosity> verbosity) {
    // Populate the collation. If this is a change stream, take the user-defined collation if one
    // exists, or an empty BSONObj otherwise. Change streams never inherit the collection's default
    // collation, and since collectionless aggregations generally run on the 'admin'
    // database, the standard logic would attempt to resolve its non-existent UUID and
    // collation by sending a specious 'listCollections' command to the config servers.
    auto collationObj = hasChangeStream
        ? request.getCollation().value_or(BSONObj())
        : cluster_aggregation_planner::getCollation(opCtx,
                                                    cri,
                                                    nsStruct.executionNss,
                                                    request.getCollation().value_or(BSONObj()),
                                                    requiresCollationForParsingUnshardedAggregate);

    // Build an ExpressionContext for the pipeline. This instantiates an appropriate collator,
    // includes all involved namespaces, and creates a shared MongoProcessInterface for use by
    // the pipeline's stages.
    boost::intrusive_ptr<ExpressionContext> expCtx =
        makeExpressionContext(opCtx,
                              request,
                              cri,
                              nsStruct.executionNss,
                              collationObj,
                              boost::none /* uuid */,
                              resolveInvolvedNamespaces(involvedNamespaces),
                              hasChangeStream,
                              verbosity);

    if (resolvedView) {
        // If applicable, ensure that the resolved namespace is added to the resolvedNamespaces map
        // on the expCtx before calling Pipeline::parse(). This is necessary for search on views as
        // Pipeline::parse() will first check if a view exists directly on the stage specification
        // and if none is found, will then check for the view using the expCtx. As such, it's
        // necessary to add the resolved namespace to the expCtx prior to any call to
        // Pipeline::parse().
        search_helpers::checkAndSetViewOnExpCtx(
            expCtx, request.getPipeline(), *resolvedView, nsStruct.requestedNss);
    }

    auto pipeline = Pipeline::parse(request.getPipeline(), expCtx);

    // Perform the query settings lookup and attach it to 'expCtx'.
    // In case query settings have already been looked up (in case the agg request is
    // running over a view) we avoid performing query settings lookup.
    query_shape::DeferredQueryShape deferredShape{[&]() {
        return shape_helpers::tryMakeShape<query_shape::AggCmdShape>(
            request, nsStruct.executionNss, involvedNamespaces, *pipeline, expCtx);
    }};
    expCtx->setQuerySettingsIfNotPresent(std::move(request.getQuerySettings()).value_or_eval([&] {
        return query_settings::lookupQuerySettingsWithRejectionCheckOnRouter(
            expCtx, deferredShape, nsStruct.executionNss);
    }));

    // Skip query stats recording for queryable encryption queries.
    if (!shouldDoFLERewrite) {
        query_stats::registerRequest(
            opCtx,
            nsStruct.executionNss,
            [&]() {
                uassertStatusOKWithContext(deferredShape->getStatus(),
                                           "Failed to compute query shape");
                return std::make_unique<query_stats::AggKey>(expCtx,
                                                             request,
                                                             std::move(deferredShape->getValue()),
                                                             std::move(involvedNamespaces));
            },
            hasChangeStream);
    }
    return pipeline;
}

void rewritePipelineIfTimeseries(OperationContext* const opCtx,
                                 AggregateCommandRequest& request,
                                 const CollectionRoutingInfo& cri) {
    if (timeseries::isEligibleForViewlessTimeseriesRewritesInRouter(opCtx, cri)) {
        // TypeCollectionTimeseriesFields encapsulates TimeseriesOptions.
        const auto timeseriesFields = cri.getChunkManager().getTimeseriesFields();
        tassert(9949202,
                "Expected getTimeseriesFields() to return a meaningful value",
                timeseriesFields.has_value());
        if (timeseriesFields.has_value()) {
            timeseries::rewriteRequestPipelineAndHintForTimeseriesCollection(
                request, *timeseriesFields, timeseriesFields->getTimeseriesOptions());
            // The query has been rewritten to something that should run directly against the bucket
            // collection. Set this flag to indicate to the shard that the query's target has
            // changed to avoid incorrect rewrites in the shard role.
            isRawDataOperation(opCtx) = true;
        }
    }
}

}  // namespace

Status ClusterAggregate::runAggregate(OperationContext* opCtx,
                                      const Namespaces& namespaces,
                                      AggregateCommandRequest& request,
                                      const PrivilegeVector& privileges,
                                      boost::optional<ExplainOptions::Verbosity> verbosity,
                                      BSONObjBuilder* result) {
    return runAggregate(opCtx, namespaces, request, {request}, privileges, verbosity, result);
}

Status ClusterAggregate::runAggregate(OperationContext* opCtx,
                                      const Namespaces& namespaces,
                                      AggregateCommandRequest& request,
                                      const LiteParsedPipeline& liteParsedPipeline,
                                      const PrivilegeVector& privileges,
                                      boost::optional<ExplainOptions::Verbosity> verbosity,
                                      BSONObjBuilder* result) {
    return runAggregate(opCtx,
                        namespaces,
                        request,
                        liteParsedPipeline,
                        privileges,
                        boost::none /*CollectionRoutingInfo*/,
                        boost::none /*ResolvedView*/,
                        verbosity,
                        result);
}

Status ClusterAggregate::runAggregate(OperationContext* opCtx,
                                      const Namespaces& namespaces,
                                      AggregateCommandRequest& request,
                                      const LiteParsedPipeline& liteParsedPipeline,
                                      const PrivilegeVector& privileges,
                                      boost::optional<CollectionRoutingInfo> cri,
                                      boost::optional<ResolvedView> resolvedView,
                                      boost::optional<ExplainOptions::Verbosity> verbosity,
                                      BSONObjBuilder* result) {
    // Perform some validations on the LiteParsedPipeline and request before continuing with the
    // aggregation command.
    performValidationChecks(opCtx, request, liteParsedPipeline);

    const auto isSharded = [](OperationContext* opCtx, const NamespaceString& nss) {
        auto criSW = getCollectionRoutingInfoForTxnCmd(opCtx, nss);

        // If the ns is not found we assume its unsharded. It might be implicitly created as
        // unsharded if this query does writes. An existing collection could also be concurrently
        // sharded in between here and lock acquisition elsewhere reguardless so shardedness still
        // needs to be checked after parsing too.
        if (criSW.getStatus().code() == ErrorCodes::NamespaceNotFound) {
            return false;
        }
        const auto& cri = uassertStatusOK(criSW);
        return cri.isSharded();
    };
    bool isExplain = request.getExplain().get_value_or(false);
    liteParsedPipeline.verifyIsSupported(opCtx, isSharded, isExplain);
    auto hasChangeStream = liteParsedPipeline.hasChangeStream();
    CurOp::get(opCtx)->debug().isChangeStreamQuery = hasChangeStream;
    const auto& involvedNamespaces = liteParsedPipeline.getInvolvedNamespaces();
    bool shouldDoFLERewrite = request.getEncryptionInformation().has_value();
    auto generatesOwnDataOnce = liteParsedPipeline.generatesOwnDataOnce();
    auto requiresCollationForParsingUnshardedAggregate =
        liteParsedPipeline.requiresCollationForParsingUnshardedAggregate();
    auto pipelineDataSource = hasChangeStream ? PipelineDataSource::kChangeStream
        : generatesOwnDataOnce                ? PipelineDataSource::kGeneratesOwnDataOnce
                                              : PipelineDataSource::kNormal;

    // If the routing table is not already taken by the higher level, fill it now.
    if (!cri && !generatesOwnDataOnce) {
        // If the routing table is valid, we obtain a reference to it. If the table is not valid,
        // then either the database does not exist, or there are no shards in the cluster. In the
        // latter case, we always return an empty cursor. In the former case, if the requested
        // aggregation is a $changeStream, we allow the operation to continue so that stream cursors
        // can be established on the given namespace before the database or collection is actually
        // created. If the database does not exist and this is not a $changeStream, then we return
        // an empty cursor.
        auto executionNsRoutingInfoStatus =
            sharded_agg_helpers::getExecutionNsRoutingInfo(opCtx, namespaces.executionNss);

        if (!executionNsRoutingInfoStatus.isOK()) {
            uassert(CollectionUUIDMismatchInfo(request.getDbName(),
                                               *request.getCollectionUUID(),
                                               request.getNamespace().coll().toString(),
                                               boost::none),
                    "Database does not exist",
                    executionNsRoutingInfoStatus != ErrorCodes::NamespaceNotFound ||
                        !request.getCollectionUUID());

            if (liteParsedPipeline.startsWithCollStats()) {
                uassertStatusOKWithContext(executionNsRoutingInfoStatus,
                                           "Unable to retrieve information for $collStats stage");
            }
        }

        if (executionNsRoutingInfoStatus.isOK()) {
            cri = executionNsRoutingInfoStatus.getValue();
        } else if (!(hasChangeStream &&
                     executionNsRoutingInfoStatus == ErrorCodes::NamespaceNotFound)) {
            // To achieve parity with mongod-style responses, parse and validate the query
            // even though the namespace is not found.
            try {
                auto pipeline = parsePipelineAndRegisterQueryStats(
                    opCtx,
                    involvedNamespaces,
                    namespaces,
                    request,
                    cri,
                    hasChangeStream,
                    shouldDoFLERewrite,
                    requiresCollationForParsingUnshardedAggregate,
                    resolvedView,
                    verbosity);
                pipeline->validateCommon(false);
            } catch (const ExceptionFor<ErrorCodes::NamespaceNotFound>& ex) {
                LOGV2_DEBUG(8396400,
                            4,
                            "Skipping query stats due to NamespaceNotFound",
                            "status"_attr = ex.toStatus());
                // ignore redundant NamespaceNotFound errors.
            }

            // if validation is ok, just return empty result
            appendEmptyResultSetWithStatus(
                opCtx, namespaces.requestedNss, executionNsRoutingInfoStatus.getStatus(), result);
            return Status::OK();
        }
    }

    // We should never acquire a routing table for a collectionless aggregate, as the first stage
    // generates its own data and has the pipeline has no shards part to target.
    tassert(10337900,
            "Cannot acquire a routing table for collectionless aggregate",
            !(cri && generatesOwnDataOnce));

    // This is used later on as well.
    const auto routingTableIsAvailable = cri && cri->hasRoutingTable();

    // If cri is valueful, then the database definitely exists and the cluster has shards. If the
    // routing table also exists, the collection exists and is tracked in the router role, so it is
    // appropriate to attempt the rewrite. If any of these conditions are false, then either cri is
    // none or the routing table is absent, and the rewrite will either be performed in the shard
    // role or the database is nonexistent or the cluster has no shards to execute the query anyway.
    if (routingTableIsAvailable) {
        rewritePipelineIfTimeseries(opCtx, request, *cri);
    }

    // Create an RAII object that prints the collection's shard key in the case of a tassert
    // or crash.
    ScopedDebugInfo shardKeyDiagnostics(
        "ShardKeyDiagnostics",
        diagnostic_printers::ShardKeyDiagnosticPrinter{
            (cri && cri->isSharded()) ? cri->getChunkManager().getShardKeyPattern().toBSON()
                                      : BSONObj()});

    // pipelineBuilder will be invoked within AggregationTargeter::make() if and only if it chooses
    // any policy other than "specific shard only".
    auto [pipeline, expCtx] = [&]() -> std::tuple<std::unique_ptr<Pipeline, PipelineDeleter>,
                                                  boost::intrusive_ptr<ExpressionContext>> {
        auto pipeline =
            parsePipelineAndRegisterQueryStats(opCtx,
                                               involvedNamespaces,
                                               namespaces,
                                               request,
                                               cri,
                                               hasChangeStream,
                                               shouldDoFLERewrite,
                                               requiresCollationForParsingUnshardedAggregate,
                                               resolvedView,
                                               verbosity);
        auto expCtx = pipeline->getContext();

        // If the aggregate command supports encrypted collections, do rewrites of the pipeline to
        // support querying against encrypted fields.
        if (shouldDoFLERewrite) {
            if (!request.getEncryptionInformation()->getCrudProcessed().value_or(false)) {
                pipeline = processFLEPipelineS(opCtx,
                                               namespaces.executionNss,
                                               request.getEncryptionInformation().value(),
                                               std::move(pipeline));
                request.getEncryptionInformation()->setCrudProcessed(true);
            }
            stdx::lock_guard<Client> lk(*opCtx->getClient());
            CurOp::get(opCtx)->setShouldOmitDiagnosticInformation(lk, true);
        }

        expCtx->initializeReferencedSystemVariables();

        // Optimize the pipeline if:
        // - We have a valid routing table.
        // - We know the collection's collation.
        // - We have a change stream.
        // - Need to do a FLE rewrite.
        // - It's a collectionless aggregation, and so a collection default collation cannot exist.
        // This is because the results of optimization may depend on knowing the collation.
        // TODO SERVER-81991: Determine whether this is necessary once all unsharded collections are
        // tracked as unsplittable collections in the sharding catalog.
        if (routingTableIsAvailable || requiresCollationForParsingUnshardedAggregate ||
            hasChangeStream || shouldDoFLERewrite ||
            expCtx->getNamespaceString().isCollectionlessAggregateNS()) {
            pipeline->optimizePipeline();

            // Validate the pipeline post-optimization.
            const bool alreadyOptimized = true;
            pipeline->validateCommon(alreadyOptimized);
        }

        // TODO SERVER-89546: Ideally extractDocsNeededBounds should be called internally within
        // DocumentSourceSearch.
        if (search_helpers::isSearchPipeline(pipeline.get())) {
            // We won't reach this if the whole pipeline passes through with the "specific shard"
            // policy. That's okay since the shard will have access to the entire pipeline to
            // extract accurate bounds.
            auto bounds = extractDocsNeededBounds(*pipeline.get());
            auto searchStage = dynamic_cast<mongo::DocumentSourceSearch*>(pipeline->peekFront());
            searchStage->setDocsNeededBounds(bounds);
        }
        return {std::move(pipeline), expCtx};
    }();

    // Create an RAII object that prints useful information about the ExpressionContext in the case
    // of a tassert or crash.
    ScopedDebugInfo expCtxDiagnostics("ExpCtxDiagnostics",
                                      diagnostic_printers::ExpressionContextPrinter{expCtx});

    auto targeter = cluster_aggregation_planner::AggregationTargeter::make(
        opCtx,
        std::move(pipeline),
        cri,
        pipelineDataSource,
        request.getPassthroughToShard().has_value());

    uassert(
        6487500,
        fmt::format("Cannot use {} with an aggregation that executes entirely on mongos",
                    AggregateCommandRequest::kCollectionUUIDFieldName),
        !request.getCollectionUUID() ||
            targeter.policy !=
                cluster_aggregation_planner::AggregationTargeter::TargetingPolicy::kMongosRequired);

    if (request.getExplain()) {
        explain_common::generateServerInfo(result);
        explain_common::generateServerParameters(expCtx, result);
    }

    // Here we modify the original 'request' object by copying the query settings from 'expCtx' into
    // it.
    //
    // In case when the original 'request' fails with the 'CommandOnShardedViewNotSupportedOnMongod'
    // exception, we retrieve the view definition and run the resolved/expanded request. The
    // resolved/expanded request must use the query settings matching the original request.
    //
    // By attaching the query settings to the original request object we can re-use the query
    // settings even though the original 'expCtx' object has been already destroyed.
    const auto& querySettings = expCtx->getQuerySettings();
    if (!query_settings::isDefault(querySettings)) {
        request.setQuerySettings(querySettings);
    }

    // Need to explicitly assign expCtx because lambdas can't capture structured bindings.
    auto status = [&](auto& expCtx) {
        bool requestQueryStatsFromRemotes = query_stats::shouldRequestRemoteMetrics(
            CurOp::get(expCtx->getOperationContext())->debug());
        try {
            switch (targeter.policy) {
                case cluster_aggregation_planner::AggregationTargeter::TargetingPolicy::
                    kMongosRequired: {
                    // If this is an explain write the explain output and return.
                    auto expCtx = targeter.pipeline->getContext();
                    if (expCtx->getExplain()) {
                        auto opts =
                            SerializationOptions{.verbosity = boost::make_optional(
                                                     ExplainOptions::Verbosity::kQueryPlanner)};
                        *result << "splitPipeline" << BSONNULL << "mongos"
                                << Document{{"host",
                                             prettyHostNameAndPort(expCtx->getOperationContext()
                                                                       ->getClient()
                                                                       ->getLocalPort())},
                                            {"stages", targeter.pipeline->writeExplainOps(opts)}};
                        return Status::OK();
                    }

                    return cluster_aggregation_planner::runPipelineOnMongoS(
                        namespaces,
                        request.getCursor().getBatchSize().value_or(
                            aggregation_request_helper::kDefaultBatchSize),
                        std::move(targeter.pipeline),
                        result,
                        privileges,
                        requestQueryStatsFromRemotes);
                }

                case cluster_aggregation_planner::AggregationTargeter::TargetingPolicy::kAnyShard: {
                    const bool eligibleForSampling = !request.getExplain();
                    return cluster_aggregation_planner::dispatchPipelineAndMerge(
                        opCtx,
                        std::move(targeter),
                        serializeForPassthrough(expCtx, request, namespaces.executionNss),
                        request.getCursor().getBatchSize().value_or(
                            aggregation_request_helper::kDefaultBatchSize),
                        namespaces,
                        privileges,
                        result,
                        pipelineDataSource,
                        eligibleForSampling,
                        requestQueryStatsFromRemotes);
                }
                case cluster_aggregation_planner::AggregationTargeter::TargetingPolicy::
                    kSpecificShardOnly: {
                    // Mark expCtx as tailable and await data so CCC behaves accordingly.
                    expCtx->setTailableMode(TailableModeEnum::kTailableAndAwaitData);
                    expCtx->setInRouter(false);

                    uassert(6273801,
                            "per shard cursor pipeline must contain $changeStream",
                            hasChangeStream);

                    ShardId shardId(std::string(request.getPassthroughToShard()->getShard()));

                    return cluster_aggregation_planner::runPipelineOnSpecificShardOnly(
                        expCtx,
                        namespaces,
                        expCtx->getExplain(),
                        serializeForPassthrough(expCtx, request, namespaces.executionNss),
                        privileges,
                        shardId,
                        result,
                        requestQueryStatsFromRemotes);
                }

                    MONGO_UNREACHABLE;
            }
            MONGO_UNREACHABLE;
        } catch (const DBException& dbe) {
            return dbe.toStatus();
        }
    }(expCtx);

    if (status.code() != ErrorCodes::CommandOnShardedViewNotSupportedOnMongod) {
        // Increment counters and set flags even in case of failed aggregate commands.
        // But for views on a sharded cluster, aggregation runs twice. First execution fails
        // because the namespace is a view, and then it is re-run with resolved view pipeline
        // and namespace.
        liteParsedPipeline.tickGlobalStageCounters();

        if (expCtx->getServerSideJsConfig().accumulator && _samplerAccumulatorJs.tick()) {
            LOGV2_WARNING(
                8996506,
                "$accumulator is deprecated. For more information, see "
                "https://www.mongodb.com/docs/manual/reference/operator/aggregation/accumulator/");
        }

        if (expCtx->getServerSideJsConfig().function && _samplerFunctionJs.tick()) {
            LOGV2_WARNING(
                8996507,
                "$function is deprecated. For more information, see "
                "https://www.mongodb.com/docs/manual/reference/operator/aggregation/function/");
        }
    }

    if (status.isOK()) {
        updateHostsTargetedMetrics(opCtx, namespaces.executionNss, cri, involvedNamespaces);
        if (expCtx->getExplain()) {
            explain_common::generateQueryShapeHash(expCtx->getOperationContext(), result);
            // Add 'command' object to explain output. If this command was done as passthrough, it
            // will already be there.
            if (targeter.policy !=
                cluster_aggregation_planner::AggregationTargeter::kSpecificShardOnly) {
                explain_common::appendIfRoom(
                    serializeForPassthrough(expCtx, request, namespaces.requestedNss).toBson(),
                    "command",
                    result);
            }
            collectQueryStatsMongos(opCtx,
                                    std::move(CurOp::get(opCtx)->debug().queryStatsInfo.key));
        }
    }

    return status;
}

Status ClusterAggregate::retryOnViewError(OperationContext* opCtx,
                                          const AggregateCommandRequest& request,
                                          const ResolvedView& resolvedView,
                                          const NamespaceString& requestedNss,
                                          const PrivilegeVector& privileges,
                                          boost::optional<ExplainOptions::Verbosity> verbosity,
                                          BSONObjBuilder* result,
                                          unsigned numberRetries) {
    if (numberRetries >= kMaxViewRetries) {
        return Status(ErrorCodes::InternalError,
                      "Failed to resolve view after max number of retries.");
    }

    auto resolvedAggRequest =
        resolvedView.asExpandedViewAggregation(VersionContext::getDecoration(opCtx), request);

    result->resetToEmpty();

    if (auto txnRouter = TransactionRouter::get(opCtx)) {
        txnRouter.onViewResolutionError(opCtx, requestedNss);
    }

    // We pass both the underlying collection namespace and the view namespace here. The
    // underlying collection namespace is used to execute the aggregation on mongoD. Any cursor
    // returned will be registered under the view namespace so that subsequent getMore and
    // killCursors calls against the view have access.
    Namespaces nsStruct;
    nsStruct.requestedNss = requestedNss;
    nsStruct.executionNss = resolvedView.getNamespace();

    // For a sharded time-series collection, the routing is based on both routing table and the
    // bucketMaxSpanSeconds value. We need to make sure we use the bucketMaxSpanSeconds of the same
    // version as the routing table, instead of the one attached in the view error. This way the
    // shard versioning check can correctly catch stale routing information.
    //
    // In addition, we should be sure to remove the 'usesExtendedRange' value from the unpack stage,
    // since the value on the target shard may be different.
    boost::optional<CollectionRoutingInfo> snapshotCri;
    if (nsStruct.executionNss.isTimeseriesBucketsCollection()) {
        const TypeCollectionTimeseriesFields* timeseriesFields =
            [&]() -> const TypeCollectionTimeseriesFields* {
            StatusWith<CollectionRoutingInfo> criSt =
                sharded_agg_helpers::getExecutionNsRoutingInfo(opCtx, nsStruct.executionNss);

            if (criSt.isOK()) {
                const CollectionRoutingInfo& cri = criSt.getValue();
                if (cri.isSharded()) {
                    snapshotCri = cri;
                    return cri.getChunkManager().getTimeseriesFields().get_ptr();
                }
            }
            return nullptr;
        }();

        const auto patchedPipeline =
            patchPipelineForTimeSeriesQuery(resolvedAggRequest.getPipeline(), timeseriesFields);
        resolvedAggRequest.setPipeline(patchedPipeline);
    }

    auto status = ClusterAggregate::runAggregate(opCtx,
                                                 nsStruct,
                                                 resolvedAggRequest,
                                                 {resolvedAggRequest},
                                                 privileges,
                                                 snapshotCri,
                                                 boost::make_optional(resolvedView),
                                                 verbosity,
                                                 result);

    // If the underlying namespace was changed to a view during retry, then re-run the aggregation
    // on the new resolved namespace.
    if (status.extraInfo<ResolvedView>()) {
        return ClusterAggregate::retryOnViewError(opCtx,
                                                  resolvedAggRequest,
                                                  *status.extraInfo<ResolvedView>(),
                                                  requestedNss,
                                                  privileges,
                                                  verbosity,
                                                  result,
                                                  numberRetries + 1);
    }

    return status;
}

}  // namespace mongo
