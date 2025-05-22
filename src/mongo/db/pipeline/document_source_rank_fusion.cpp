/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/db/pipeline/document_source_rank_fusion.h"

#include <algorithm>
#include <boost/smart_ptr/intrusive_ptr.hpp>
#include <fmt/format.h>
#include <fmt/ranges.h>

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/json.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_add_fields.h"
#include "mongo/db/pipeline/document_source_geo_near.h"
#include "mongo/db/pipeline/document_source_group.h"
#include "mongo/db/pipeline/document_source_hybrid_scoring_util.h"
#include "mongo/db/pipeline/document_source_limit.h"
#include "mongo/db/pipeline/document_source_project.h"
#include "mongo/db/pipeline/document_source_replace_root.h"
#include "mongo/db/pipeline/document_source_set_metadata.h"
#include "mongo/db/pipeline/document_source_set_window_fields.h"
#include "mongo/db/pipeline/document_source_sort.h"
#include "mongo/db/pipeline/document_source_union_with.h"
#include "mongo/db/pipeline/document_source_unwind.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/pipeline/lite_parsed_pipeline.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/search/document_source_search.h"
#include "mongo/db/pipeline/search/document_source_vector_search.h"
#include "mongo/db/pipeline/search/search_helper.h"
#include "mongo/db/query/allowed_contexts.h"
#include "mongo/logv2/log.h"
#include "mongo/util/string_map.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {

REGISTER_DOCUMENT_SOURCE_WITH_FEATURE_FLAG(rankFusion,
                                           DocumentSourceRankFusion::LiteParsed::parse,
                                           DocumentSourceRankFusion::createFromBson,
                                           AllowedWithApiStrict::kNeverInVersion1,
                                           &feature_flags::gFeatureFlagRankFusionBasic);

namespace {

// Description that gets set as part of $rankFusion's scoreDetails metadata.
static const std::string rankFusionScoreDetailsDescription =
    "value output by reciprocal rank fusion algorithm, computed as sum of (weight * (1 / "
    "(60 + rank))) across input pipelines from which this document is output, from:";

/**
 * Checks that the input pipeline is a valid ranked pipeline. This means it is either one of
 * $search, $vectorSearch, $geoNear, $rankFusion, $scoreFusion (which have ordered output) or has an
 * explicit $sort stage. A ranked pipeline must also be a 'selection pipeline', which means no stage
 * can modify the documents in any way. Only stages that retrieve, limit, or order documents are
 * allowed.
 */
static void rankFusionPipelineValidator(const Pipeline& pipeline) {
    // Note that we don't check for $rankFusion and $scoreFusion explicitly because it will be
    // desugared by this point.
    static const std::set<StringData> implicitlyOrderedStages{
        DocumentSourceVectorSearch::kStageName,
        DocumentSourceSearch::kStageName,
        DocumentSourceGeoNear::kStageName};
    const auto& sources = pipeline.getSources();

    static const std::string rankPipelineMsg =
        "All subpipelines to the $rankFusion stage must begin with one of $search, "
        "$vectorSearch, $geoNear, $scoreFusion, $rankFusion or have a custom $sort in "
        "the pipeline.";
    uassert(9834300,
            str::stream() << "$rankFusion input pipeline cannot be empty. " << rankPipelineMsg,
            !sources.empty());

    auto firstStageName = sources.front()->getSourceName();
    auto isRankedPipeline = implicitlyOrderedStages.contains(firstStageName) ||
        std::any_of(sources.begin(), sources.end(), [](auto& stage) {
                                return stage->getSourceName() == DocumentSourceSort::kStageName;
                            });
    uassert(9191100, rankPipelineMsg, isRankedPipeline);

    for (const auto& stage : sources) {
        if (stage->getSourceName() == DocumentSourceGeoNear::kStageName) {
            uassert(9191101,
                    str::stream() << "$geoNear can be used in a $rankFusion subpipeline but not "
                                     "when includeLocs or distanceField is specified because they "
                                     "modify the documents by adding an output field. Only stages "
                                     "that retrieve, limit, or order documents are allowed.",
                    stage->constraints().noFieldModifications);
        } else if (stage->getSourceName() == DocumentSourceSearch::kStageName) {
            uassert(
                9191102,
                str::stream()
                    << "$search can be used in a $rankFusion subpipeline but not when "
                       "returnStoredSource is set to true because it modifies the output fields. "
                       "Only stages that retrieve, limit, or order documents are allowed.",
                stage->constraints().noFieldModifications);
        } else {
            uassert(9191103,
                    str::stream() << stage->getSourceName()
                                  << " is not allowed in a $rankFusion subpipeline because it "
                                     "modifies the documents or transforms their fields. Only "
                                     "stages that retrieve, limit, or order documents are allowed.",
                    stage->constraints().noFieldModifications);
        }
    }
}

auto makeSureSortKeyIsOutput(const auto& stageList) {
    DocumentSourceSort* rightMostSort = nullptr;
    for (auto&& stage : stageList) {
        if (auto sortStage = dynamic_cast<DocumentSourceSort*>(stage.get())) {
            rightMostSort = sortStage;
        }
    }
    if (rightMostSort)
        rightMostSort->pleaseOutputSortKeyMetadata();
}

boost::intrusive_ptr<DocumentSource> setWindowFields(const auto& expCtx,
                                                     const std::string& rankFieldName) {
    // TODO SERVER-98562 We shouldn't need to provide this sort, since it's not used other than to
    // pass the parse-time validation checks.
    const SortPattern dummySortPattern{BSON("order" << 1), expCtx};
    return make_intrusive<DocumentSourceInternalSetWindowFields>(
        expCtx,
        boost::none,  // partitionBy
        dummySortPattern,
        std::vector<WindowFunctionStatement>{WindowFunctionStatement{
            rankFieldName,
            window_function::Expression::parse(
                BSON("$rank" << BSONObj()), dummySortPattern, expCtx.get())}},
        internalDocumentSourceSetWindowFieldsMaxMemoryBytes.load(),
        SbeCompatibility::notCompatible);
}

/**
 * Builds and returns an $addFields stage, like the following:
 * {$addFields:
 *     {prefix_score:
 *         {multiply:
 *             [{$divide: [1, {$add: [rank, rankConstant]}]}]
 *         },
 *     }
 * }
 */
boost::intrusive_ptr<DocumentSource> addScoreField(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const StringData prefix,
    const int rankConstant,
    const double weight) {
    const std::string score = fmt::format("{}_score", prefix);
    const std::string rankPath = fmt::format("${}_rank", prefix);
    const std::string scorePath = fmt::format("${}", score);

    BSONObjBuilder bob;
    {
        BSONObjBuilder addFieldsBob(bob.subobjStart("$addFields"_sd));
        {
            BSONObjBuilder scoreField(addFieldsBob.subobjStart(score));
            {
                BSONArrayBuilder multiplyArray(scoreField.subarrayStart("$multiply"_sd));
                // RRF Score = weight * (1 / (rank + rank constant)).
                multiplyArray.append(
                    BSON("$divide"
                         << BSON_ARRAY(1 << BSON("$add" << BSON_ARRAY(rankPath << rankConstant)))));
                multiplyArray.append(weight);
            }
        }
    }

    const auto spec = bob.obj();
    return DocumentSourceAddFields::createFromBson(spec.firstElement(), expCtx);
}

/**
 * Builds and returns a $replaceRoot stage: {$replaceWith: {docs: "$$ROOT"}}.
 * This has the effect of storing the unmodified user's document in the path '$docs'.
 */
boost::intrusive_ptr<DocumentSource> nestUserDocs(const auto& expCtx) {
    return DocumentSourceReplaceRoot::createFromBson(
        BSON("$replaceWith" << BSON("docs" << "$$ROOT")).firstElement(), expCtx);
}

std::list<boost::intrusive_ptr<DocumentSource>> buildFirstPipelineStages(
    const StringData prefixOne,
    const int rankConstant,
    const double weight,
    const bool includeScoreDetails,
    const bool inputGeneratesScore,
    const bool inputGeneratesScoreDetails,
    const boost::intrusive_ptr<ExpressionContext>& expCtx) {

    std::list<boost::intrusive_ptr<DocumentSource>> outputStages = {
        nestUserDocs(expCtx),
        setWindowFields(expCtx, fmt::format("{}_rank", prefixOne)),
        addScoreField(expCtx, prefixOne, rankConstant, weight),
    };
    if (includeScoreDetails) {
        outputStages.push_back(hybrid_scoring_util::score_details::addScoreDetails(
            expCtx, prefixOne, inputGeneratesScore, inputGeneratesScoreDetails));
    }
    return outputStages;
}

BSONObj groupEachScore(
    const std::map<std::string, std::unique_ptr<Pipeline, PipelineDeleter>>& pipelines,
    const bool includeScoreDetails) {
    // For each sub-pipeline, build the following obj:
    // name_score: {$max: {ifNull: ["$name_score", 0]}}
    // If scoreDetails is enabled, build:
    // name_rank: {$max: {ifNull: ["$name_rank", 0]}}
    // name_scoreDetails: {$mergeObjects: $name_scoreDetails}
    BSONObjBuilder bob;
    {
        BSONObjBuilder groupBob(bob.subobjStart("$group"_sd));
        groupBob.append("_id", "$docs._id");
        groupBob.append("docs", BSON("$first" << "$docs"));

        for (auto it = pipelines.begin(); it != pipelines.end(); it++) {
            const auto& pipelineName = it->first;
            const std::string scoreName = fmt::format("{}_score", pipelineName);
            groupBob.append(
                scoreName,
                BSON("$max" << BSON("$ifNull" << BSON_ARRAY(fmt::format("${}", scoreName) << 0))));
            // We only need to preserve the rank if we're calculating score details.
            if (includeScoreDetails) {
                const std::string rankName = fmt::format("{}_rank", pipelineName);
                groupBob.append(rankName,
                                BSON("$max" << BSON("$ifNull" << BSON_ARRAY(
                                                        fmt::format("${}", rankName) << 0))));
                const auto& [scoreDetailsName, scoreDetailsBson] =
                    hybrid_scoring_util::score_details::constructScoreDetailsForGrouping(
                        pipelineName);
                groupBob.append(scoreDetailsName, scoreDetailsBson);
            }
        }
        groupBob.done();
    }
    bob.done();
    return bob.obj();
}

BSONObj calculateFinalScore(
    const std::map<std::string, std::unique_ptr<Pipeline, PipelineDeleter>>& inputs) {
    // Generate a $add object with an array of all the fields containing a score for a given
    // pipeline.
    const auto& allInputs = [&] {
        BSONObjBuilder addBob;

        BSONArrayBuilder addArrBuilder(addBob.subarrayStart("$add"_sd));
        for (auto it = inputs.begin(); it != inputs.end(); it++) {
            StringBuilder sb;
            sb << "$" << it->first << "_score";
            addArrBuilder.append(sb.str());
        }
        addArrBuilder.done();
        return addBob.obj();
    };
    return BSON("$addFields" << BSON("score" << allInputs()));
}

boost::intrusive_ptr<DocumentSource> calculateFinalScoreMetadata(
    const auto& expCtx,
    const std::map<std::string, std::unique_ptr<Pipeline, PipelineDeleter>>& inputs) {
    // Generate an array of all the fields containing a score for a given pipeline.
    Expression::ExpressionVector allInputScores;
    for (auto it = inputs.begin(); it != inputs.end(); it++) {
        allInputScores.push_back(ExpressionFieldPath::createPathFromString(
            expCtx.get(), it->first + "_score", expCtx->variablesParseState));
    }

    // Return a $setMetadata stage that sets score to an $add object that takes the generated array
    // of each pipeline's score fieldpaths as an input.
    // Ex: {"$setMetadata": {"score": {"$add": ["$pipeline_name_score"]}}},
    return DocumentSourceSetMetadata::create(
        expCtx,
        make_intrusive<ExpressionAdd>(expCtx.get(), std::move(allInputScores)),
        DocumentMetadataFields::MetaType::kScore);
}

boost::intrusive_ptr<DocumentSource> buildUnionWithPipeline(
    const std::string& prefix,
    const int rankConstant,
    const double weight,
    std::unique_ptr<Pipeline, PipelineDeleter> oneInputPipeline,
    const bool includeScoreDetails,
    const bool inputGeneratesScore,
    const bool inputGeneratesScoreDetails,
    const boost::intrusive_ptr<ExpressionContext>& expCtx) {

    makeSureSortKeyIsOutput(oneInputPipeline->getSources());
    oneInputPipeline->pushBack(nestUserDocs(expCtx));
    oneInputPipeline->pushBack(setWindowFields(expCtx, fmt::format("{}_rank", prefix)));
    oneInputPipeline->pushBack(addScoreField(expCtx, prefix, rankConstant, weight));
    if (includeScoreDetails) {
        oneInputPipeline->pushBack(hybrid_scoring_util::score_details::addScoreDetails(
            expCtx, prefix, inputGeneratesScore, inputGeneratesScoreDetails));
    }
    std::vector<BSONObj> bsonPipeline = oneInputPipeline->serializeToBson();

    auto collName = expCtx->getNamespaceString().coll();

    BSONObj inputToUnionWith =
        BSON("$unionWith" << BSON("coll" << collName << "pipeline" << bsonPipeline));
    return DocumentSourceUnionWith::createFromBson(inputToUnionWith.firstElement(), expCtx);
}

/**
 * Constuct the scoreDetails metadata object. Looks like the following:
 * { "$setMetadata": {"scoreDetails": {"value": "$score", "description":
 * {"scoreDetailsDescription..."}, "details": "$calculatedScoreDetails"}}},
 */
boost::intrusive_ptr<DocumentSource> constructScoreDetailsMetadata(
    const std::string& scoreDetailsDescription,
    const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    boost::intrusive_ptr<DocumentSource> setScoreDetails = DocumentSourceSetMetadata::create(
        expCtx,
        Expression::parseObject(expCtx.get(),
                                BSON("value" << "$score"
                                             << "description" << scoreDetailsDescription
                                             << "details"
                                             << "$calculatedScoreDetails"),
                                expCtx->variablesParseState),
        DocumentMetadataFields::kScoreDetails);
    return setScoreDetails;
}

std::list<boost::intrusive_ptr<DocumentSource>> buildScoreAndMergeStages(
    const std::map<std::string, std::unique_ptr<Pipeline, PipelineDeleter>>& inputPipelines,
    const StringMap<double>& weights,
    const bool includeScoreDetails,
    const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    auto group = DocumentSourceGroup::createFromBson(
        groupEachScore(inputPipelines, includeScoreDetails).firstElement(), expCtx);
    auto addFields = DocumentSourceAddFields::createFromBson(
        calculateFinalScore(inputPipelines).firstElement(), expCtx);

    // Note that the scoreDetails fields go here in the pipeline. We create them below to be able
    // to return them immediately once all stages are generated.
    const SortPattern sortingPattern{BSON("score" << -1 << "_id" << 1), expCtx};
    auto sort = DocumentSourceSort::create(expCtx, sortingPattern);

    auto restoreUserDocs =
        DocumentSourceReplaceRoot::create(expCtx,
                                          ExpressionFieldPath::createPathFromString(
                                              expCtx.get(), "docs", expCtx->variablesParseState),
                                          "documents",
                                          SbeCompatibility::noRequirements);

    if (includeScoreDetails) {
        boost::intrusive_ptr<DocumentSource> addFieldsDetails =
            hybrid_scoring_util::score_details::constructCalculatedFinalScoreDetails(
                inputPipelines, weights, true, expCtx);
        auto setScoreDetails =
            constructScoreDetailsMetadata(rankFusionScoreDetailsDescription, expCtx);
        return {group, addFields, addFieldsDetails, setScoreDetails, sort, restoreUserDocs};
    }
    // TODO SERVER-85426: Remove this check once all feature flags have been removed.
    if (feature_flags::gFeatureFlagRankFusionFull.isEnabledUseLastLTSFCVWhenUninitialized(
            serverGlobalParams.featureCompatibility.acquireFCVSnapshot())) {
        auto setScore = calculateFinalScoreMetadata(expCtx, inputPipelines);
        return {group, addFields, setScore, sort, restoreUserDocs};
    }
    return {group, addFields, sort, restoreUserDocs};
}

}  // namespace

std::unique_ptr<DocumentSourceRankFusion::LiteParsed> DocumentSourceRankFusion::LiteParsed::parse(
    const NamespaceString& nss, const BSONElement& spec, const LiteParserOptions& options) {
    uassert(ErrorCodes::FailedToParse,
            str::stream() << kStageName << " must take a nested object but found: " << spec,
            spec.type() == BSONType::Object);

    auto parsedSpec = RankFusionSpec::parse(IDLParserContext(kStageName), spec.embeddedObject());
    auto inputPipesObj = parsedSpec.getInput().getPipelines();

    // Ensure that all pipelines are valid ranked selection pipelines.
    std::vector<LiteParsedPipeline> liteParsedPipelines;
    for (const auto& elem : inputPipesObj) {
        liteParsedPipelines.emplace_back(nss, parsePipelineFromBSON(elem));
    }

    return std::make_unique<DocumentSourceRankFusion::LiteParsed>(
        spec.fieldName(), nss, std::move(liteParsedPipelines));
}

std::list<boost::intrusive_ptr<DocumentSource>> DocumentSourceRankFusion::createFromBson(
    BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& pExpCtx) {
    uassert(ErrorCodes::FailedToParse,
            str::stream() << "The " << kStageName
                          << " stage specification must be an object, found "
                          << typeName(elem.type()),
            elem.type() == BSONType::Object);

    // It is currently necessary to annotate on the ExpressionContext that this is a $rankFusion
    // query. Once desugaring happens, there's no way to identity from the (desugared) pipeline
    // alone that it came from $rankFusion. We need to know if it came from $rankFusion so we can
    // reject the query if it is run over a view.
    // TODO SERVER-101661 Remove this when $rankFusion is enabled on views.
    pExpCtx->setIsRankFusion();

    auto spec = RankFusionSpec::parse(IDLParserContext(kStageName), elem.embeddedObject());

    // It's important to use an ordered map here, so that we get stability in the desugaring =>
    // stability in the query shape.
    std::map<std::string, std::unique_ptr<Pipeline, PipelineDeleter>> inputPipelines;
    // Ensure that all pipelines are valid ranked selection pipelines.
    for (const auto& elem : spec.getInput().getPipelines()) {
        auto pipeline = Pipeline::parse(parsePipelineFromBSON(elem), pExpCtx);
        rankFusionPipelineValidator(*pipeline);

        auto inputName = elem.fieldName();
        uassertStatusOKWithContext(
            FieldPath::validateFieldName(inputName),
            "$rankFusion pipeline names must follow the naming rules of field path expressions.");
        uassert(
            9921000,
            str::stream() << "$rankFusion pipeline names must be unique, but found duplicate name '"
                          << inputName << "'.",
            !inputPipelines.contains(inputName));
        inputPipelines[inputName] = std::move(pipeline);
    }

    StringMap<double> weights;
    // If RankFusionCombinationSpec has no value (no weights specified), no work to do.
    const auto& combinationSpec = spec.getCombination();
    if (combinationSpec.has_value()) {
        weights = hybrid_scoring_util::validateWeights(
            combinationSpec->getWeights(), inputPipelines, "rankFusion");
    }

    // For now, the rankConstant is always 60.
    static const double rankConstant = 60;
    const bool includeScoreDetails = spec.getScoreDetails();
    // TODO SERVER-85426: Remove this check once all feature flags have been removed.
    if (includeScoreDetails) {
        auto isRankFusionFullEnabled =
            feature_flags::gFeatureFlagRankFusionFull.isEnabledUseLastLTSFCVWhenUninitialized(
                serverGlobalParams.featureCompatibility.acquireFCVSnapshot());
        uassert(ErrorCodes::QueryFeatureNotAllowed,
                "'featureFlagRankFusionFull' must be enabled to use scoreDetails",
                isRankFusionFullEnabled);
    }

    std::list<boost::intrusive_ptr<DocumentSource>> outputStages;
    for (auto it = inputPipelines.begin(); it != inputPipelines.end(); it++) {
        const auto& name = it->first;
        auto& pipeline = it->second;

        // Check if an explicit weight for this pipeline has been specified.
        // If not, the default is one.
        double pipelineWeight = hybrid_scoring_util::getPipelineWeight(weights, name);

        // We need to know if the pipeline generates "score" and "scoreDetails" metadata so we know
        // how to construct each pipeline's individual "scoreDetails" (see addScoreDetails()).
        const bool inputGeneratesScore =
            pipeline->generatesMetadataType(DocumentMetadataFields::kScore);
        const bool inputGeneratesScoreDetails =
            pipeline->generatesMetadataType(DocumentMetadataFields::kScoreDetails);

        if (outputStages.empty()) {
            // First pipeline.
            makeSureSortKeyIsOutput(pipeline->getSources());


            while (!pipeline->empty()) {
                outputStages.push_back(pipeline->popFront());
            }
            auto firstPipelineStages = buildFirstPipelineStages(name,
                                                                rankConstant,
                                                                pipelineWeight,
                                                                includeScoreDetails,
                                                                inputGeneratesScore,
                                                                inputGeneratesScoreDetails,
                                                                pExpCtx);
            for (const auto& stage : firstPipelineStages) {
                outputStages.push_back(std::move(stage));
            }
        } else {
            auto unionWithStage = buildUnionWithPipeline(name,
                                                         rankConstant,
                                                         pipelineWeight,
                                                         std::move(pipeline),
                                                         includeScoreDetails,
                                                         inputGeneratesScore,
                                                         inputGeneratesScoreDetails,
                                                         pExpCtx);
            outputStages.push_back(unionWithStage);
        }
    }

    // Build all remaining stages to perform the fusion.
    auto finalStages =
        buildScoreAndMergeStages(inputPipelines, weights, includeScoreDetails, pExpCtx);
    for (const auto& stage : finalStages) {
        outputStages.push_back(stage);
    }

    return outputStages;
}
}  // namespace mongo
