/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/pipeline/document_source_coll_stats.h"
#include "mongo/db/pipeline/document_source_index_stats.h"
#include "mongo/db/pipeline/document_source_internal_convert_bucket_index_stats.h"
#include "mongo/db/pipeline/document_source_internal_unpack_bucket.h"
#include "mongo/db/query/timeseries/timeseries_rewrites.h"
#include "mongo/db/query/timeseries/timeseries_rewrites_mocks.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {
TypeCollectionTimeseriesFields createTimeseriesFields(
    const StringData timeField,
    const boost::optional<StringData>& metaField,
    const boost::optional<std::int32_t>& bucketMaxSpanSeconds,
    const bool assumeNoMixedSchemaData,
    const bool timeseriesBucketsAreFixed) {
    auto timeseriesOptions = TimeseriesOptions{};
    timeseriesOptions.setTimeField(timeField);
    timeseriesOptions.setMetaField(metaField);
    timeseriesOptions.setBucketMaxSpanSeconds(bucketMaxSpanSeconds);
    auto timeseriesFields = TypeCollectionTimeseriesFields{};
    timeseriesFields.setTimeseriesOptions({timeseriesOptions});
    timeseriesFields.setTimeseriesBucketsMayHaveMixedSchemaData(!assumeNoMixedSchemaData);
    timeseriesFields.setTimeseriesBucketingParametersHaveChanged(!timeseriesBucketsAreFixed);
    return timeseriesFields;
}

TEST(TimeseriesRewritesTest, EmptyPipelineRewriteTest) {
    const auto timeseriesFields = createTimeseriesFields("time"_sd, {}, {}, {}, {});
    const auto rewrittenPipeline = timeseries::rewritePipelineForTimeseriesCollection(
        {}, timeseriesFields, timeseriesFields.getTimeseriesOptions());

    ASSERT_EQ(rewrittenPipeline.size(), 1);
    ASSERT_EQ(rewrittenPipeline.front().firstElementFieldName(),
              DocumentSourceInternalUnpackBucket::kStageNameInternal);
}

TEST(TimeseriesRewritesTest, InternalUnpackBucketRewriteTest) {
    const auto timeseriesFields = createTimeseriesFields("time"_sd, {}, {}, {}, {});
    const auto originalPipeline = std::vector{BSON("$match" << BSON("a" << "1"))};
    const auto rewrittenPipeline = timeseries::rewritePipelineForTimeseriesCollection(
        originalPipeline, timeseriesFields, timeseriesFields.getTimeseriesOptions());

    ASSERT_EQ(rewrittenPipeline.size(), originalPipeline.size() + 1);
    ASSERT_EQ(rewrittenPipeline.front().firstElementFieldName(),
              DocumentSourceInternalUnpackBucket::kStageNameInternal);
}

TEST(TimeseriesRewritesTest, RouterRoleRequestRewriteTest) {
    const auto originalPipeline = std::vector{BSON("$match" << BSON("a" << "1"))};
    auto request = AggregateCommandRequest{
        NamespaceString::createNamespaceString_forTest("TestViewlessTimeseries"_sd),
        originalPipeline};
    const auto timeseriesFields = createTimeseriesFields("time"_sd, {}, {}, {}, {});

    timeseries::rewriteRequestPipelineAndHintForTimeseriesCollection(
        request, timeseriesFields, timeseriesFields.getTimeseriesOptions());

    const auto rewrittenPipeline = request.getPipeline();
    ASSERT_EQ(rewrittenPipeline.size(), originalPipeline.size() + 1);
    ASSERT_EQ(rewrittenPipeline.front().firstElementFieldName(),
              DocumentSourceInternalUnpackBucket::kStageNameInternal);
}

TEST(TimeseriesRewritesTest, ShardRoleRequestRewriteTest) {
    const auto originalPipeline = std::vector{BSON("$match" << BSON("a" << "1"))};
    auto request = AggregateCommandRequest{
        NamespaceString::createNamespaceString_forTest("TestViewlessTimeseries"_sd),
        originalPipeline};
    auto timeseriesOptions = TimeseriesOptions{};
    timeseriesOptions.setTimeField("time"_sd);
    auto collection = TimeseriesRewritesCollectionMock(
        timeseriesOptions, timeseries::MixedSchemaBucketsState::Invalid, boost::none, true, true);

    timeseries::rewriteRequestPipelineAndHintForTimeseriesCollection(
        request, collection, timeseriesOptions);

    const auto rewrittenPipeline = request.getPipeline();
    ASSERT_EQ(rewrittenPipeline.size(), originalPipeline.size() + 1);
    ASSERT_EQ(rewrittenPipeline.front().firstElementFieldName(),
              DocumentSourceInternalUnpackBucket::kStageNameInternal);
}

TEST(TimeseriesRewritesTest, InsertIndexStatsConversionStage) {
    const auto timeseriesFields = createTimeseriesFields("time"_sd, {"food"_sd}, {}, {}, {});
    const auto indexStatsStage = BSON("$indexStats" << BSON("a" << "1"));
    const auto matchStage = BSON("$match" << BSON("b" << 2));
    const auto originalPipeline = std::vector{indexStatsStage, matchStage};
    const auto rewrittenPipeline = timeseries::rewritePipelineForTimeseriesCollection(
        originalPipeline, timeseriesFields, timeseriesFields.getTimeseriesOptions());

    ASSERT_EQ(rewrittenPipeline.size(), originalPipeline.size() + 1);
    ASSERT_BSONOBJ_EQ(rewrittenPipeline[0], indexStatsStage);
    ASSERT_BSONOBJ_EQ(rewrittenPipeline[1],
                      BSON("$_internalConvertBucketIndexStats" << BSON("timeField" << "time"
                                                                                   << "metaField"
                                                                                   << "food")));
    ASSERT_BSONOBJ_EQ(rewrittenPipeline[2], matchStage);
}

TEST(TimeseriesRewritesTest, DontInsertUnpackStageWhenCollStatsPresent) {
    const auto timeseriesFields = createTimeseriesFields("time"_sd, {"food"_sd}, {}, {}, {});
    const auto collStatsStage = BSON("$collStats" << BSON("a" << "1"));
    const auto matchStage = BSON("$match" << BSON("b" << 2));
    const auto originalPipeline = std::vector{collStatsStage, matchStage};
    const auto rewrittenPipeline = timeseries::rewritePipelineForTimeseriesCollection(
        originalPipeline, timeseriesFields, timeseriesFields.getTimeseriesOptions());

    ASSERT_EQ(rewrittenPipeline.size(), originalPipeline.size());
    ASSERT_BSONOBJ_EQ(rewrittenPipeline[0], collStatsStage);
    ASSERT_BSONOBJ_EQ(rewrittenPipeline[1], matchStage);
}
}  // namespace
}  // namespace mongo
