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

#include "mongo/db/query/stage_builder/sbe/gen_expression.h"

#include <absl/container/flat_hash_set.h>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/smart_ptr.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>
// IWYU pragma: no_include "ext/alloc_traits.h"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <map>
#include <numeric>
#include <stack>
#include <string>
#include <utility>
#include <vector>

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/docval_to_sbeval.h"
#include "mongo/db/exec/sbe/expressions/runtime_environment.h"
#include "mongo/db/exec/sbe/util/pcre.h"
#include "mongo/db/exec/sbe/values/arith_common.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/exec/sbe/vm/vm_datetime.h"
#include "mongo/db/pipeline/accumulator.h"
#include "mongo/db/pipeline/accumulator_multi.h"
#include "mongo/db/pipeline/accumulator_percentile.h"
#include "mongo/db/pipeline/expression_from_accumulator_quantile.h"
#include "mongo/db/pipeline/expression_visitor.h"
#include "mongo/db/pipeline/expression_walker.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/algebra/polyvalue.h"
#include "mongo/db/query/bson_typemask.h"
#include "mongo/db/query/datetime/date_time_support.h"
#include "mongo/db/query/expression_walker.h"
#include "mongo/db/query/stage_builder/sbe/abt/comparison_op.h"
#include "mongo/db/query/stage_builder/sbe/abt/syntax/expr.h"
#include "mongo/db/query/stage_builder/sbe/abt/syntax/syntax.h"
#include "mongo/db/query/stage_builder/sbe/abt_holder_impl.h"
#include "mongo/db/query/stage_builder/sbe/builder.h"
#include "mongo/db/query/stage_builder/sbe/gen_abt_helpers.h"
#include "mongo/db/query/stage_builder/sbe/gen_helpers.h"
#include "mongo/db/query/stage_builder/sbe/sbexpr_helpers.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"


namespace mongo::stage_builder {
namespace {
size_t kArgumentCountForBinaryTree = 100;

inline abt::ProjectionName makeLocalVariableName(sbe::FrameId frameId, sbe::value::SlotId slotId) {
    return getABTLocalVariableName(frameId, slotId);
}

struct ExpressionVisitorContext {
    struct VarsFrame {
        VarsFrame(const std::vector<Variables::Id>& variableIds,
                  sbe::value::FrameIdGenerator* frameIdGenerator)
            : currentBindingIndex(0) {
            bindings.reserve(variableIds.size());
            for (const auto& variableId : variableIds) {
                bindings.push_back({variableId, frameIdGenerator->generate(), SbExpr{}});
            }
        }

        struct Binding {
            Variables::Id variableId;
            sbe::FrameId frameId;
            SbExpr expr;
        };

        std::vector<Binding> bindings;
        size_t currentBindingIndex;
    };

    ExpressionVisitorContext(StageBuilderState& state,
                             boost::optional<SbSlot> rootSlot,
                             const PlanStageSlots& slots)
        : state(state), rootSlot(std::move(rootSlot)), slots(&slots) {}

    void ensureArity(size_t arity) {
        invariant(exprStack.size() >= arity);
    }

    void pushExpr(SbExpr expr) {
        exprStack.emplace_back(std::move(expr));
    }

    abt::ABT popABTExpr() {
        tassert(6987504, "tried to pop from empty SbExpr stack", !exprStack.empty());

        auto expr = std::move(exprStack.back());
        exprStack.pop_back();
        return unwrap(expr.extractABT());
    }

    SbExpr popExpr() {
        tassert(7261700, "tried to pop from empty SbExpr stack", !exprStack.empty());

        auto expr = std::move(exprStack.back());
        exprStack.pop_back();
        return expr;
    }

    SbExpr done() {
        tassert(6987501, "expected exactly one SbExpr on the stack", exprStack.size() == 1);
        return popExpr();
    }

    StageBuilderState& state;

    std::vector<SbExpr> exprStack;

    boost::optional<SbSlot> rootSlot;

    // The lexical environment for the expression being traversed. A variable reference takes the
    // form "$$variable_name" in MQL's concrete syntax and gets transformed into a numeric
    // identifier (Variables::Id) in the AST. During this translation, we directly translate any
    // such variable to an SBE frame id using this mapping.
    std::map<Variables::Id, sbe::FrameId> environment;
    std::stack<VarsFrame> varsFrameStack;

    const PlanStageSlots* slots;
};

/**
 * For the given MatchExpression 'expr', generates a path traversal SBE plan stage sub-tree
 * implementing the comparison expression.
 */
SbExpr generateTraverseHelper(SbExpr inputExpr,
                              const FieldPath& fp,
                              size_t level,
                              StageBuilderState& state,
                              boost::optional<SbSlot> topLevelFieldSlot = boost::none) {
    using namespace std::literals;

    SbExprBuilder b(state);
    invariant(level < fp.getPathLength());

    tassert(6950802,
            "Expected an input expression or top level field",
            !inputExpr.isNull() || topLevelFieldSlot.has_value());

    // Generate an expression to read a sub-field at the current nested level.
    auto fieldName = b.makeStrConstant(fp.getFieldName(level));
    auto fieldExpr = topLevelFieldSlot
        ? b.makeVariable(*topLevelFieldSlot)
        : b.makeFunction("getField"_sd, std::move(inputExpr), std::move(fieldName));

    if (level == fp.getPathLength() - 1) {
        // For the last level, we can just return the field slot without the need for a
        // traverse stage.
        return fieldExpr;
    }

    // Generate nested traversal.
    auto lambdaFrameId = state.frameId();
    auto lambdaParam = b.makeVariable(lambdaFrameId, 0);

    auto resultExpr = generateTraverseHelper(std::move(lambdaParam), fp, level + 1, state);

    auto lambdaExpr = b.makeLocalLambda(lambdaFrameId, std::move(resultExpr));

    // Generate the traverse stage for the current nested level.
    return b.makeFunction(
        "traverseP"_sd, std::move(fieldExpr), std::move(lambdaExpr), b.makeInt32Constant(1));
}

SbExpr generateTraverse(SbExpr inputExpr,
                        bool expectsDocumentInputOnly,
                        const FieldPath& fp,
                        StageBuilderState& state,
                        boost::optional<SbSlot> topLevelFieldSlot = boost::none) {
    SbExprBuilder b(state);
    size_t level = 0;

    if (expectsDocumentInputOnly) {
        // When we know for sure that 'inputExpr' will be a document and _not_ an array (such as
        // when accessing a field on the root document), we can generate a simpler expression.
        return generateTraverseHelper(std::move(inputExpr), fp, level, state, topLevelFieldSlot);
    } else {
        tassert(6950803, "Expected an input expression", !inputExpr.isNull());
        // The general case: the value in the 'inputExpr' may be an array that will require
        // traversal.
        auto lambdaFrameId = state.frameId();
        auto lambdaParam = b.makeVariable(lambdaFrameId, 0);

        auto resultExpr = generateTraverseHelper(std::move(lambdaParam), fp, level, state);

        auto lambdaExpr = b.makeLocalLambda(lambdaFrameId, std::move(resultExpr));

        return b.makeFunction(
            "traverseP"_sd, std::move(inputExpr), std::move(lambdaExpr), b.makeInt32Constant(1));
    }
}

/**
 * Generates an EExpression that converts the input to upper or lower case.
 */
void generateStringCaseConversionExpression(ExpressionVisitorContext* _context,
                                            const std::string& caseConversionFunction) {
    uint32_t typeMask = (getBSONTypeMask(sbe::value::TypeTags::StringSmall) |
                         getBSONTypeMask(sbe::value::TypeTags::StringBig) |
                         getBSONTypeMask(sbe::value::TypeTags::bsonString) |
                         getBSONTypeMask(sbe::value::TypeTags::bsonSymbol) |
                         getBSONTypeMask(sbe::value::TypeTags::NumberInt32) |
                         getBSONTypeMask(sbe::value::TypeTags::NumberInt64) |
                         getBSONTypeMask(sbe::value::TypeTags::NumberDouble) |
                         getBSONTypeMask(sbe::value::TypeTags::NumberDecimal) |
                         getBSONTypeMask(sbe::value::TypeTags::Date) |
                         getBSONTypeMask(sbe::value::TypeTags::Timestamp));

    auto str = _context->popABTExpr();
    auto varStr = makeLocalVariableName(_context->state.frameId(), 0);

    auto totalCaseConversionExpr = buildABTMultiBranchConditionalFromCaseValuePairs(
        {ABTCaseValuePair{generateABTNullMissingOrUndefined(varStr), makeABTConstant(""_sd)},
         ABTCaseValuePair{
             makeABTFunction("typeMatch"_sd, makeVariable(varStr), abt::Constant::int32(typeMask)),
             makeABTFunction(caseConversionFunction,
                             makeABTFunction("coerceToString"_sd, makeVariable(varStr)))}},
        makeABTFail(ErrorCodes::Error{7158200},
                    str::stream() << "$" << caseConversionFunction
                                  << " input type is not supported"));

    _context->pushExpr(
        wrap(abt::make<abt::Let>(varStr, std::move(str), std::move(totalCaseConversionExpr))));
}

class ExpressionPreVisitor final : public ExpressionConstVisitor {
public:
    ExpressionPreVisitor(ExpressionVisitorContext* context) : _context{context} {}

    void visit(const ExpressionConstant* expr) final {}
    void visit(const ExpressionAbs* expr) final {}
    void visit(const ExpressionAdd* expr) final {}
    void visit(const ExpressionAllElementsTrue* expr) final {}
    void visit(const ExpressionAnd* expr) final {}
    void visit(const ExpressionAnyElementTrue* expr) final {}
    void visit(const ExpressionArray* expr) final {}
    void visit(const ExpressionArrayElemAt* expr) final {}
    void visit(const ExpressionBitAnd* expr) final {}
    void visit(const ExpressionBitOr* expr) final {}
    void visit(const ExpressionBitXor* expr) final {}
    void visit(const ExpressionBitNot* expr) final {}
    void visit(const ExpressionFirst* expr) final {}
    void visit(const ExpressionLast* expr) final {}
    void visit(const ExpressionObjectToArray* expr) final {}
    void visit(const ExpressionArrayToObject* expr) final {}
    void visit(const ExpressionBsonSize* expr) final {}
    void visit(const ExpressionCeil* expr) final {}
    void visit(const ExpressionCompare* expr) final {}
    void visit(const ExpressionConcat* expr) final {}
    void visit(const ExpressionConcatArrays* expr) final {}
    void visit(const ExpressionCond* expr) final {}
    void visit(const ExpressionDateDiff* expr) final {}
    void visit(const ExpressionDateFromString* expr) final {}
    void visit(const ExpressionDateFromParts* expr) final {}
    void visit(const ExpressionDateToParts* expr) final {}
    void visit(const ExpressionDateToString* expr) final {}
    void visit(const ExpressionDateTrunc* expr) final {}
    void visit(const ExpressionDivide* expr) final {}
    void visit(const ExpressionExp* expr) final {}
    void visit(const ExpressionFieldPath* expr) final {}
    void visit(const ExpressionFilter* expr) final {}
    void visit(const ExpressionFloor* expr) final {}
    void visit(const ExpressionIfNull* expr) final {}
    void visit(const ExpressionIn* expr) final {}
    void visit(const ExpressionIndexOfArray* expr) final {}
    void visit(const ExpressionIndexOfBytes* expr) final {}
    void visit(const ExpressionIndexOfCP* expr) final {}
    void visit(const ExpressionIsNumber* expr) final {}
    void visit(const ExpressionLet* expr) final {
        _context->varsFrameStack.push(ExpressionVisitorContext::VarsFrame{
            expr->getOrderedVariableIds(), _context->state.frameIdGenerator});
    }
    void visit(const ExpressionLn* expr) final {}
    void visit(const ExpressionLog* expr) final {}
    void visit(const ExpressionLog10* expr) final {}
    void visit(const ExpressionInternalFLEBetween* expr) final {}
    void visit(const ExpressionInternalFLEEqual* expr) final {}
    void visit(const ExpressionEncStrStartsWith* expr) final {}
    void visit(const ExpressionEncStrEndsWith* expr) final {}
    void visit(const ExpressionEncStrContains* expr) final {}
    void visit(const ExpressionEncStrNormalizedEq* expr) final {}
    void visit(const ExpressionInternalRawSortKey* expr) final {}
    void visit(const ExpressionMap* expr) final {}
    void visit(const ExpressionMeta* expr) final {}
    void visit(const ExpressionMod* expr) final {}
    void visit(const ExpressionMultiply* expr) final {}
    void visit(const ExpressionNot* expr) final {}
    void visit(const ExpressionObject* expr) final {}
    void visit(const ExpressionOr* expr) final {}
    void visit(const ExpressionPow* expr) final {}
    void visit(const ExpressionRange* expr) final {}
    void visit(const ExpressionReduce* expr) final {}
    void visit(const ExpressionReplaceOne* expr) final {}
    void visit(const ExpressionReplaceAll* expr) final {}
    void visit(const ExpressionSetDifference* expr) final {}
    void visit(const ExpressionSetEquals* expr) final {}
    void visit(const ExpressionSetIntersection* expr) final {}
    void visit(const ExpressionSetIsSubset* expr) final {}
    void visit(const ExpressionSetUnion* expr) final {}
    void visit(const ExpressionSize* expr) final {}
    void visit(const ExpressionReverseArray* expr) final {}
    void visit(const ExpressionSortArray* expr) final {}
    void visit(const ExpressionSlice* expr) final {}
    void visit(const ExpressionIsArray* expr) final {}
    void visit(const ExpressionInternalFindAllValuesAtPath* expr) final {}
    void visit(const ExpressionRound* expr) final {}
    void visit(const ExpressionSplit* expr) final {}
    void visit(const ExpressionSqrt* expr) final {}
    void visit(const ExpressionStrcasecmp* expr) final {}
    void visit(const ExpressionSubstrBytes* expr) final {}
    void visit(const ExpressionSubstrCP* expr) final {}
    void visit(const ExpressionStrLenBytes* expr) final {}
    void visit(const ExpressionBinarySize* expr) final {}
    void visit(const ExpressionStrLenCP* expr) final {}
    void visit(const ExpressionSubtract* expr) final {}
    void visit(const ExpressionSwitch* expr) final {}
    void visit(const ExpressionTestApiVersion* expr) final {}
    void visit(const ExpressionToLower* expr) final {}
    void visit(const ExpressionToUpper* expr) final {}
    void visit(const ExpressionTrim* expr) final {}
    void visit(const ExpressionTrunc* expr) final {}
    void visit(const ExpressionType* expr) final {}
    void visit(const ExpressionZip* expr) final {}
    void visit(const ExpressionConvert* expr) final {}
    void visit(const ExpressionRegexFind* expr) final {}
    void visit(const ExpressionRegexFindAll* expr) final {}
    void visit(const ExpressionRegexMatch* expr) final {}
    void visit(const ExpressionCosine* expr) final {}
    void visit(const ExpressionSine* expr) final {}
    void visit(const ExpressionTangent* expr) final {}
    void visit(const ExpressionArcCosine* expr) final {}
    void visit(const ExpressionArcSine* expr) final {}
    void visit(const ExpressionArcTangent* expr) final {}
    void visit(const ExpressionArcTangent2* expr) final {}
    void visit(const ExpressionHyperbolicArcTangent* expr) final {}
    void visit(const ExpressionHyperbolicArcCosine* expr) final {}
    void visit(const ExpressionHyperbolicArcSine* expr) final {}
    void visit(const ExpressionHyperbolicTangent* expr) final {}
    void visit(const ExpressionHyperbolicCosine* expr) final {}
    void visit(const ExpressionHyperbolicSine* expr) final {}
    void visit(const ExpressionDegreesToRadians* expr) final {}
    void visit(const ExpressionRadiansToDegrees* expr) final {}
    void visit(const ExpressionDayOfMonth* expr) final {}
    void visit(const ExpressionDayOfWeek* expr) final {}
    void visit(const ExpressionDayOfYear* expr) final {}
    void visit(const ExpressionHour* expr) final {}
    void visit(const ExpressionMillisecond* expr) final {}
    void visit(const ExpressionMinute* expr) final {}
    void visit(const ExpressionMonth* expr) final {}
    void visit(const ExpressionSecond* expr) final {}
    void visit(const ExpressionWeek* expr) final {}
    void visit(const ExpressionIsoWeekYear* expr) final {}
    void visit(const ExpressionIsoDayOfWeek* expr) final {}
    void visit(const ExpressionIsoWeek* expr) final {}
    void visit(const ExpressionYear* expr) final {}
    void visit(const ExpressionFromAccumulator<AccumulatorAvg>* expr) final {}
    void visit(const ExpressionFromAccumulator<AccumulatorMax>* expr) final {}
    void visit(const ExpressionFromAccumulator<AccumulatorMin>* expr) final {}
    void visit(const ExpressionFromAccumulatorN<AccumulatorFirstN>* expr) final {}
    void visit(const ExpressionFromAccumulatorN<AccumulatorLastN>* expr) final {}
    void visit(const ExpressionFromAccumulatorN<AccumulatorMaxN>* expr) final {}
    void visit(const ExpressionFromAccumulatorN<AccumulatorMinN>* expr) final {}
    void visit(const ExpressionFromAccumulatorQuantile<AccumulatorMedian>* expr) final {}
    void visit(const ExpressionFromAccumulatorQuantile<AccumulatorPercentile>* expr) final {}
    void visit(const ExpressionFromAccumulator<AccumulatorStdDevPop>* expr) final {}
    void visit(const ExpressionFromAccumulator<AccumulatorStdDevSamp>* expr) final {}
    void visit(const ExpressionFromAccumulator<AccumulatorSum>* expr) final {}
    void visit(const ExpressionFromAccumulator<AccumulatorMergeObjects>* expr) final {}
    void visit(const ExpressionTests::Testable* expr) final {}
    void visit(const ExpressionInternalJsEmit* expr) final {}
    void visit(const ExpressionInternalFindSlice* expr) final {}
    void visit(const ExpressionInternalFindPositional* expr) final {}
    void visit(const ExpressionInternalFindElemMatch* expr) final {}
    void visit(const ExpressionFunction* expr) final {}
    void visit(const ExpressionRandom* expr) final {}
    void visit(const ExpressionCurrentDate* expr) final {}
    void visit(const ExpressionToHashedIndexKey* expr) final {}
    void visit(const ExpressionDateAdd* expr) final {}
    void visit(const ExpressionDateSubtract* expr) final {}
    void visit(const ExpressionGetField* expr) final {}
    void visit(const ExpressionSetField* expr) final {}
    void visit(const ExpressionTsSecond* expr) final {}
    void visit(const ExpressionTsIncrement* expr) final {}
    void visit(const ExpressionInternalOwningShard* expr) final {}
    void visit(const ExpressionInternalIndexKey* expr) final {}
    void visit(const ExpressionInternalKeyStringValue* expr) final {}
    void visit(const ExpressionUUID* expr) final {}

private:
    ExpressionVisitorContext* _context;
};

class ExpressionInVisitor final : public ExpressionConstVisitor {
public:
    ExpressionInVisitor(ExpressionVisitorContext* context) : _context{context} {}

    void visit(const ExpressionConstant* expr) final {}
    void visit(const ExpressionAbs* expr) final {}
    void visit(const ExpressionAdd* expr) final {}
    void visit(const ExpressionAllElementsTrue* expr) final {}
    void visit(const ExpressionAnd* expr) final {}
    void visit(const ExpressionAnyElementTrue* expr) final {}
    void visit(const ExpressionArray* expr) final {}
    void visit(const ExpressionArrayElemAt* expr) final {}
    void visit(const ExpressionBitAnd* expr) final {}
    void visit(const ExpressionBitOr* expr) final {}
    void visit(const ExpressionBitXor* expr) final {}
    void visit(const ExpressionBitNot* expr) final {}
    void visit(const ExpressionFirst* expr) final {}
    void visit(const ExpressionLast* expr) final {}
    void visit(const ExpressionObjectToArray* expr) final {}
    void visit(const ExpressionArrayToObject* expr) final {}
    void visit(const ExpressionBsonSize* expr) final {}
    void visit(const ExpressionCeil* expr) final {}
    void visit(const ExpressionCompare* expr) final {}
    void visit(const ExpressionConcat* expr) final {}
    void visit(const ExpressionConcatArrays* expr) final {}
    void visit(const ExpressionCond* expr) final {}
    void visit(const ExpressionDateDiff* expr) final {}
    void visit(const ExpressionDateFromString* expr) final {}
    void visit(const ExpressionDateFromParts* expr) final {}
    void visit(const ExpressionDateToParts* expr) final {}
    void visit(const ExpressionDateToString* expr) final {}
    void visit(const ExpressionDateTrunc*) final {}
    void visit(const ExpressionDivide* expr) final {}
    void visit(const ExpressionExp* expr) final {}
    void visit(const ExpressionFieldPath* expr) final {}
    void visit(const ExpressionFilter* expr) final {}
    void visit(const ExpressionFloor* expr) final {}
    void visit(const ExpressionIfNull* expr) final {}
    void visit(const ExpressionIn* expr) final {}
    void visit(const ExpressionIndexOfArray* expr) final {}
    void visit(const ExpressionIndexOfBytes* expr) final {}
    void visit(const ExpressionIndexOfCP* expr) final {}
    void visit(const ExpressionIsNumber* expr) final {}
    void visit(const ExpressionLet* expr) final {
        // This visitor fires after each variable definition in a $let expression. The top of the
        // _context's expression stack will be an expression defining the variable initializer. We
        // use a separate frame stack ('varsFrameStack') to keep track of which variable we are
        // visiting, so we can appropriately bind the initializer.
        invariant(!_context->varsFrameStack.empty());
        auto& currentFrame = _context->varsFrameStack.top();
        size_t& currentBindingIndex = currentFrame.currentBindingIndex;
        invariant(currentBindingIndex < currentFrame.bindings.size());

        auto& currentBinding = currentFrame.bindings[currentBindingIndex++];
        currentBinding.expr = _context->popExpr();

        // Second, we bind this variables AST-level name (with type Variable::Id) to the frame that
        // will be used for compilation and execution. Once this "stage builder" finishes, these
        // Variable::Id bindings will no longer be relevant.
        invariant(_context->environment.find(currentBinding.variableId) ==
                  _context->environment.end());
        _context->environment.emplace(currentBinding.variableId, currentBinding.frameId);
    }
    void visit(const ExpressionLn* expr) final {}
    void visit(const ExpressionLog* expr) final {}
    void visit(const ExpressionLog10* expr) final {}
    void visit(const ExpressionInternalFLEBetween* expr) final {}
    void visit(const ExpressionInternalFLEEqual* expr) final {}
    void visit(const ExpressionEncStrStartsWith* expr) final {}
    void visit(const ExpressionEncStrEndsWith* expr) final {}
    void visit(const ExpressionEncStrContains* expr) final {}
    void visit(const ExpressionEncStrNormalizedEq* expr) final {}
    void visit(const ExpressionInternalRawSortKey* expr) final {}
    void visit(const ExpressionMap* expr) final {}
    void visit(const ExpressionMeta* expr) final {}
    void visit(const ExpressionMod* expr) final {}
    void visit(const ExpressionMultiply* expr) final {}
    void visit(const ExpressionNot* expr) final {}
    void visit(const ExpressionObject* expr) final {}
    void visit(const ExpressionOr* expr) final {}
    void visit(const ExpressionPow* expr) final {}
    void visit(const ExpressionRange* expr) final {}
    void visit(const ExpressionReduce* expr) final {}
    void visit(const ExpressionReplaceOne* expr) final {}
    void visit(const ExpressionReplaceAll* expr) final {}
    void visit(const ExpressionSetDifference* expr) final {}
    void visit(const ExpressionSetEquals* expr) final {}
    void visit(const ExpressionSetIntersection* expr) final {}
    void visit(const ExpressionSetIsSubset* expr) final {}
    void visit(const ExpressionSetUnion* expr) final {}
    void visit(const ExpressionSize* expr) final {}
    void visit(const ExpressionReverseArray* expr) final {}
    void visit(const ExpressionSortArray* expr) final {}
    void visit(const ExpressionSlice* expr) final {}
    void visit(const ExpressionIsArray* expr) final {}
    void visit(const ExpressionInternalFindAllValuesAtPath* expr) final {}
    void visit(const ExpressionRound* expr) final {}
    void visit(const ExpressionSplit* expr) final {}
    void visit(const ExpressionSqrt* expr) final {}
    void visit(const ExpressionStrcasecmp* expr) final {}
    void visit(const ExpressionSubstrBytes* expr) final {}
    void visit(const ExpressionSubstrCP* expr) final {}
    void visit(const ExpressionStrLenBytes* expr) final {}
    void visit(const ExpressionBinarySize* expr) final {}
    void visit(const ExpressionStrLenCP* expr) final {}
    void visit(const ExpressionSubtract* expr) final {}
    void visit(const ExpressionSwitch* expr) final {}
    void visit(const ExpressionTestApiVersion* expr) final {}
    void visit(const ExpressionToLower* expr) final {}
    void visit(const ExpressionToUpper* expr) final {}
    void visit(const ExpressionTrim* expr) final {}
    void visit(const ExpressionTrunc* expr) final {}
    void visit(const ExpressionType* expr) final {}
    void visit(const ExpressionZip* expr) final {}
    void visit(const ExpressionConvert* expr) final {}
    void visit(const ExpressionRegexFind* expr) final {}
    void visit(const ExpressionRegexFindAll* expr) final {}
    void visit(const ExpressionRegexMatch* expr) final {}
    void visit(const ExpressionCosine* expr) final {}
    void visit(const ExpressionSine* expr) final {}
    void visit(const ExpressionTangent* expr) final {}
    void visit(const ExpressionArcCosine* expr) final {}
    void visit(const ExpressionArcSine* expr) final {}
    void visit(const ExpressionArcTangent* expr) final {}
    void visit(const ExpressionArcTangent2* expr) final {}
    void visit(const ExpressionHyperbolicArcTangent* expr) final {}
    void visit(const ExpressionHyperbolicArcCosine* expr) final {}
    void visit(const ExpressionHyperbolicArcSine* expr) final {}
    void visit(const ExpressionHyperbolicTangent* expr) final {}
    void visit(const ExpressionHyperbolicCosine* expr) final {}
    void visit(const ExpressionHyperbolicSine* expr) final {}
    void visit(const ExpressionDegreesToRadians* expr) final {}
    void visit(const ExpressionRadiansToDegrees* expr) final {}
    void visit(const ExpressionDayOfMonth* expr) final {}
    void visit(const ExpressionDayOfWeek* expr) final {}
    void visit(const ExpressionDayOfYear* expr) final {}
    void visit(const ExpressionHour* expr) final {}
    void visit(const ExpressionMillisecond* expr) final {}
    void visit(const ExpressionMinute* expr) final {}
    void visit(const ExpressionMonth* expr) final {}
    void visit(const ExpressionSecond* expr) final {}
    void visit(const ExpressionWeek* expr) final {}
    void visit(const ExpressionIsoWeekYear* expr) final {}
    void visit(const ExpressionIsoDayOfWeek* expr) final {}
    void visit(const ExpressionIsoWeek* expr) final {}
    void visit(const ExpressionYear* expr) final {}
    void visit(const ExpressionFromAccumulator<AccumulatorAvg>* expr) final {}
    void visit(const ExpressionFromAccumulator<AccumulatorMax>* expr) final {}
    void visit(const ExpressionFromAccumulator<AccumulatorMin>* expr) final {}
    void visit(const ExpressionFromAccumulatorN<AccumulatorFirstN>* expr) final {}
    void visit(const ExpressionFromAccumulatorN<AccumulatorLastN>* expr) final {}
    void visit(const ExpressionFromAccumulatorN<AccumulatorMaxN>* expr) final {}
    void visit(const ExpressionFromAccumulatorN<AccumulatorMinN>* expr) final {}
    void visit(const ExpressionFromAccumulatorQuantile<AccumulatorMedian>* expr) final {}
    void visit(const ExpressionFromAccumulatorQuantile<AccumulatorPercentile>* expr) final {}
    void visit(const ExpressionFromAccumulator<AccumulatorStdDevPop>* expr) final {}
    void visit(const ExpressionFromAccumulator<AccumulatorStdDevSamp>* expr) final {}
    void visit(const ExpressionFromAccumulator<AccumulatorSum>* expr) final {}
    void visit(const ExpressionFromAccumulator<AccumulatorMergeObjects>* expr) final {}
    void visit(const ExpressionTests::Testable* expr) final {}
    void visit(const ExpressionInternalJsEmit* expr) final {}
    void visit(const ExpressionInternalFindSlice* expr) final {}
    void visit(const ExpressionInternalFindPositional* expr) final {}
    void visit(const ExpressionInternalFindElemMatch* expr) final {}
    void visit(const ExpressionFunction* expr) final {}
    void visit(const ExpressionRandom* expr) final {}
    void visit(const ExpressionCurrentDate* expr) final {}
    void visit(const ExpressionToHashedIndexKey* expr) final {}
    void visit(const ExpressionDateAdd* expr) final {}
    void visit(const ExpressionDateSubtract* expr) final {}
    void visit(const ExpressionGetField* expr) final {}
    void visit(const ExpressionSetField* expr) final {}
    void visit(const ExpressionTsSecond* expr) final {}
    void visit(const ExpressionTsIncrement* expr) final {}
    void visit(const ExpressionInternalOwningShard* expr) final {}
    void visit(const ExpressionInternalIndexKey* expr) final {}
    void visit(const ExpressionInternalKeyStringValue* expr) final {}
    void visit(const ExpressionUUID* expr) final {}

private:
    ExpressionVisitorContext* _context;
};


struct DoubleBound {
    DoubleBound(double b, bool isInclusive) : bound(b), inclusive(isInclusive) {}

    static DoubleBound minInfinity() {
        return DoubleBound(-std::numeric_limits<double>::infinity(), false);
    }
    static DoubleBound plusInfinity() {
        return DoubleBound(std::numeric_limits<double>::infinity(), false);
    }
    static DoubleBound plusInfinityInclusive() {
        return DoubleBound(std::numeric_limits<double>::infinity(), true);
    }
    std::string printLowerBound() const {
        return str::stream() << (inclusive ? "[" : "(") << bound;
    }
    std::string printUpperBound() const {
        return str::stream() << bound << (inclusive ? "]" : ")");
    }
    double bound;
    bool inclusive;
};

class ExpressionPostVisitor final : public ExpressionConstVisitor {
public:
    ExpressionPostVisitor(ExpressionVisitorContext* context) : _context{context} {}

    enum class SetOperation {
        Difference,
        Intersection,
        Union,
        Equals,
        IsSubset,
    };

    void visit(const ExpressionConstant* expr) final {
        auto [tag, val] = sbe::value::makeValue(expr->getValue());
        pushABT(makeABTConstant(tag, val));
    }

    void visit(const ExpressionAbs* expr) final {
        auto inputName = makeLocalVariableName(_context->state.frameIdGenerator->generate(), 0);

        auto absExpr = buildABTMultiBranchConditionalFromCaseValuePairs(
            {ABTCaseValuePair{generateABTNullMissingOrUndefined(inputName), abt::Constant::null()},
             ABTCaseValuePair{
                 generateABTNonNumericCheck(inputName),
                 makeABTFail(ErrorCodes::Error{7157700}, "$abs only supports numeric types")},
             ABTCaseValuePair{
                 generateABTLongLongMinCheck(inputName),
                 makeABTFail(ErrorCodes::Error{7157701}, "can't take $abs of long long min")}},
            makeABTFunction("abs", makeVariable(inputName)));

        pushABT(
            abt::make<abt::Let>(std::move(inputName), _context->popABTExpr(), std::move(absExpr)));
    }

    void visit(const ExpressionAdd* expr) final {
        size_t arity = expr->getChildren().size();
        _context->ensureArity(arity);

        // Build a linear tree for a small number of children so that we can pre-validate all
        // arguments.
        if (arity < kArgumentCountForBinaryTree ||
            feature_flags::gFeatureFlagSbeUpgradeBinaryTrees.checkEnabled()) {
            visitFast(expr);
            return;
        }

        auto checkLeaf = [&](abt::ABT arg) {
            auto name = makeLocalVariableName(_context->state.frameId(), 0);
            auto var = makeVariable(name);
            auto checkedLeaf = buildABTMultiBranchConditional(
                ABTCaseValuePair{abt::make<abt::BinaryOp>(abt::Operations::Or,
                                                          makeABTFunction("isNumber", var),
                                                          makeABTFunction("isDate", var)),
                                 var},
                makeABTFail(ErrorCodes::Error{7315401},
                            "only numbers and dates are allowed in an $add expression"));
            return abt::make<abt::Let>(std::move(name), std::move(arg), std::move(checkedLeaf));
        };

        auto combineTwoTree = [&](abt::ABT left, abt::ABT right) {
            auto nameLeft = makeLocalVariableName(_context->state.frameId(), 0);
            auto nameRight = makeLocalVariableName(_context->state.frameId(), 0);
            auto varLeft = makeVariable(nameLeft);
            auto varRight = makeVariable(nameRight);

            auto addExpr = buildABTMultiBranchConditionalFromCaseValuePairs(
                {ABTCaseValuePair{
                     abt::make<abt::BinaryOp>(abt::Operations::Or,
                                              generateABTNullMissingOrUndefined(nameLeft),
                                              generateABTNullMissingOrUndefined(nameRight)),
                     abt::Constant::null()},
                 ABTCaseValuePair{abt::make<abt::BinaryOp>(abt::Operations::And,
                                                           makeABTFunction("isDate", varLeft),
                                                           makeABTFunction("isDate", varRight)),
                                  makeABTFail(ErrorCodes::Error{7315402},
                                              "only one date allowed in an $add expression")}},
                abt::make<abt::BinaryOp>(abt::Operations::Add, varLeft, varRight));
            return makeLet({std::move(nameLeft), std::move(nameRight)},
                           abt::makeSeq(std::move(left), std::move(right)),
                           std::move(addExpr));
        };

        abt::ABTVector leaves;
        leaves.reserve(arity);
        for (size_t idx = 0; idx < arity; ++idx) {
            leaves.emplace_back(checkLeaf(_context->popABTExpr()));
        }
        std::reverse(std::begin(leaves), std::end(leaves));

        pushABT(makeBalancedTree(combineTwoTree, std::move(leaves)));
    }

    void visitFast(const ExpressionAdd* expr) {
        size_t arity = expr->getChildren().size();
        _context->ensureArity(arity);

        if (arity == 0) {
            // Return a zero constant if the expression has no operand children.
            pushABT(abt::Constant::int32(0));
        } else {
            abt::ABTVector binds;
            abt::ProjectionNameVector names;
            abt::ABTVector variables;
            abt::ABTVector checkArgIsNull;
            abt::ABTVector checkArgHasValidType;
            binds.reserve(arity);
            names.reserve(arity);
            variables.reserve(arity);
            checkArgIsNull.reserve(arity);
            checkArgHasValidType.reserve(arity);

            for (size_t idx = 0; idx < arity; ++idx) {
                binds.push_back(_context->popABTExpr());
                auto name = makeLocalVariableName(_context->state.frameId(), 0);

                // Count the number of dates among children of this $add while verifying the types
                // so that we can later check that we have at most one date.
                checkArgHasValidType.emplace_back(buildABTMultiBranchConditionalFromCaseValuePairs(
                    {ABTCaseValuePair{makeABTFunction("isNumber", makeVariable(name)),
                                      abt::Constant::int32(0)},
                     ABTCaseValuePair{makeABTFunction("isDate", makeVariable(name)),
                                      abt::Constant::int32(1)}},
                    makeABTFail(ErrorCodes::Error{7157723},
                                "only numbers and dates are allowed in an $add expression")));

                checkArgIsNull.push_back(generateABTNullMissingOrUndefined(name));
                names.push_back(std::move(name));
                variables.emplace_back(makeVariable(names[idx]));
            }

            // At this point 'binds' vector contains arguments of $add expression in the reversed
            // order. We need to reverse it back to perform summation in the right order below.
            // Summation in different order can lead to different result because of accumulated
            // precision errors from floating point types.
            std::reverse(std::begin(binds), std::end(binds));

            auto checkNullAllArguments =
                makeBooleanOpTree(abt::Operations::Or, std::move(checkArgIsNull));

            auto checkValidTypeAndCountDates =
                makeNaryOp(abt::Operations::Add, std::move(checkArgHasValidType));

            auto addOp = makeNaryOp(abt::Operations::Add, std::move(variables));

            auto addExpr = buildABTMultiBranchConditionalFromCaseValuePairs(
                {ABTCaseValuePair{std::move(checkNullAllArguments), abt::Constant::null()},
                 ABTCaseValuePair{abt::make<abt::BinaryOp>(abt::Operations::Gt,
                                                           std::move(checkValidTypeAndCountDates),
                                                           abt::Constant::int32(1)),
                                  makeABTFail(ErrorCodes::Error{7157722},
                                              "only one date allowed in an $add expression")}},
                std::move(addOp));

            addExpr = makeLet(std::move(names), std::move(binds), std::move(addExpr));
            pushABT(std::move(addExpr));
        }
    }

    void visit(const ExpressionAllElementsTrue* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(const ExpressionAnd* expr) final {
        visitMultiBranchLogicExpression(expr, abt::Operations::And);
    }
    void visit(const ExpressionAnyElementTrue* expr) final {
        auto arg = _context->popABTExpr();
        auto argName = makeLocalVariableName(_context->state.frameId(), 0);

        auto lambdaArgName = makeLocalVariableName(_context->state.frameId(), 0);
        auto lambdaBody =
            makeFillEmptyFalse(makeABTFunction("coerceToBool", makeVariable(lambdaArgName)));
        auto lambdaExpr =
            abt::make<abt::LambdaAbstraction>(std::move(lambdaArgName), std::move(lambdaBody));

        auto resultExpr = abt::make<abt::If>(
            makeFillEmptyFalse(makeABTFunction("isArray", makeVariable(argName))),
            makeABTFunction("traverseF",
                            makeVariable(argName),
                            std::move(lambdaExpr),
                            abt::Constant::boolean(false)),
            makeABTFail(ErrorCodes::Error{7158300}, "$anyElementTrue's argument must be an array"));

        pushABT(abt::make<abt::Let>(std::move(argName), std::move(arg), std::move(resultExpr)));
    }

    void visit(const ExpressionArray* expr) final {
        auto arity = expr->getChildren().size();
        _context->ensureArity(arity);

        if (arity == 0) {
            auto [emptyArrTag, emptyArrVal] = sbe::value::makeNewArray();
            pushABT(makeABTConstant(emptyArrTag, emptyArrVal));
            return;
        }

        std::vector<abt::ProjectionName> varNames;
        std::vector<abt::ABT> binds;
        for (size_t idx = 0; idx < arity; ++idx) {
            varNames.emplace_back(makeLocalVariableName(_context->state.frameId(), 0));
            binds.emplace_back(_context->popABTExpr());
        }
        std::reverse(std::begin(binds), std::end(binds));

        abt::ABTVector argVars;
        for (auto& name : varNames) {
            argVars.push_back(makeFillEmptyNull(makeVariable(name)));
        }

        auto arrayExpr = abt::make<abt::FunctionCall>("newArray", std::move(argVars));

        pushABT(makeLet(std::move(varNames), std::move(binds), std::move(arrayExpr)));
    }
    void visit(const ExpressionArrayElemAt* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(const ExpressionBitAnd* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(const ExpressionBitOr* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(const ExpressionBitXor* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(const ExpressionBitNot* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(const ExpressionFirst* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(const ExpressionLast* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(const ExpressionObjectToArray* expr) final {
        auto arg = _context->popABTExpr();
        auto argName = makeLocalVariableName(_context->state.frameId(), 0);

        // Create an expression to invoke the built-in function.
        auto funcCall = makeABTFunction("objectToArray"_sd, makeVariable(argName));
        auto funcName = makeLocalVariableName(_context->state.frameId(), 0);

        // Create validation checks when builtin returns nothing
        auto validationExpr = buildABTMultiBranchConditionalFromCaseValuePairs(
            {ABTCaseValuePair{makeABTFunction("exists"_sd, makeVariable(funcName)),
                              makeVariable(funcName)},
             ABTCaseValuePair{generateABTNullMissingOrUndefined(argName), abt::Constant::null()},
             ABTCaseValuePair{generateABTNonObjectCheck(argName),
                              makeABTFail(ErrorCodes::Error{5153215},
                                          "$objectToArray requires an object input")}},
            abt::Constant::nothing());

        pushABT(makeLet({std::move(argName), std::move(funcName)},
                        abt::makeSeq(std::move(arg), std::move(funcCall)),
                        std::move(validationExpr)));
    }
    void visit(const ExpressionArrayToObject* expr) final {
        auto arg = _context->popABTExpr();
        auto argName = makeLocalVariableName(_context->state.frameId(), 0);

        // Create an expression to invoke the built-in function.
        auto funcCall = makeABTFunction("arrayToObject"_sd, makeVariable(argName));
        auto funcName = makeLocalVariableName(_context->state.frameId(), 0);

        // Create validation checks when builtin returns nothing
        auto validationExpr = buildABTMultiBranchConditionalFromCaseValuePairs(
            {ABTCaseValuePair{makeABTFunction("exists"_sd, makeVariable(funcName)),
                              makeVariable(funcName)},
             ABTCaseValuePair{generateABTNullMissingOrUndefined(argName), abt::Constant::null()},
             ABTCaseValuePair{generateABTNonArrayCheck(argName),
                              makeABTFail(ErrorCodes::Error{5153200},
                                          "$arrayToObject requires an array input")}},
            abt::Constant::nothing());

        pushABT(makeLet({std::move(argName), std::move(funcName)},
                        abt::makeSeq(std::move(arg), std::move(funcCall)),
                        std::move(validationExpr)));
    }
    void visit(const ExpressionBsonSize* expr) final {
        // Build an expression which evaluates the size of a BSON document and validates the input
        // argument.
        // 1. If the argument is null or empty, return null.
        // 2. Else, if the argument is a BSON document, return its size.
        // 3. Else, raise an error.
        auto arg = _context->popABTExpr();
        auto argName = makeLocalVariableName(_context->state.frameId(), 0);

        auto bsonSizeExpr = buildABTMultiBranchConditionalFromCaseValuePairs(
            {ABTCaseValuePair{generateABTNullMissingOrUndefined(argName), abt::Constant::null()},
             ABTCaseValuePair{
                 generateABTNonObjectCheck(argName),
                 makeABTFail(ErrorCodes::Error{7158301}, "$bsonSize requires a document input")}},
            makeABTFunction("bsonSize", makeVariable(argName)));

        pushABT(abt::make<abt::Let>(std::move(argName), std::move(arg), std::move(bsonSizeExpr)));
    }

    void visit(const ExpressionCeil* expr) final {
        auto inputName = makeLocalVariableName(_context->state.frameIdGenerator->generate(), 0);

        auto ceilExpr = buildABTMultiBranchConditionalFromCaseValuePairs(
            {ABTCaseValuePair{generateABTNullMissingOrUndefined(inputName), abt::Constant::null()},
             ABTCaseValuePair{
                 generateABTNonNumericCheck(inputName),
                 makeABTFail(ErrorCodes::Error{7157702}, "$ceil only supports numeric types")}},
            makeABTFunction("ceil", makeVariable(inputName)));

        pushABT(
            abt::make<abt::Let>(std::move(inputName), _context->popABTExpr(), std::move(ceilExpr)));
    }
    void visit(const ExpressionCompare* expr) final {
        _context->ensureArity(2);

        auto rhs = _context->popExpr();
        auto lhs = _context->popExpr();

        _context->pushExpr(generateExpressionCompare(
            _context->state, expr->getOp(), std::move(lhs), std::move(rhs)));
    }

    void visit(const ExpressionConcat* expr) final {
        auto arity = expr->getChildren().size();
        _context->ensureArity(arity);

        // Concatenation of no strings is an empty string.
        if (arity == 0) {
            pushABT(makeABTConstant(""_sd));
            return;
        }

        std::vector<abt::ProjectionName> varNames;
        std::vector<abt::ABT> binds;
        for (size_t idx = 0; idx < arity; ++idx) {
            // ABT can bind a single variable at a time, so create a new frame for each
            // argument.
            varNames.emplace_back(makeLocalVariableName(_context->state.frameId(), 0));
            binds.emplace_back(_context->popABTExpr());
        }
        std::reverse(std::begin(binds), std::end(binds));

        abt::ABTVector checkNullArg;
        abt::ABTVector checkStringArg;
        abt::ABTVector argVars;
        for (auto& name : varNames) {
            checkNullArg.push_back(generateABTNullMissingOrUndefined(name));
            checkStringArg.push_back(makeABTFunction("isString"_sd, makeVariable(name)));
            argVars.push_back(makeVariable(name));
        }

        auto checkNullAnyArgument = makeBooleanOpTree(abt::Operations::Or, std::move(checkNullArg));

        auto checkStringAllArguments =
            makeBooleanOpTree(abt::Operations::And, std::move(checkStringArg));

        auto concatExpr = buildABTMultiBranchConditionalFromCaseValuePairs(
            {ABTCaseValuePair{std::move(checkNullAnyArgument), abt::Constant::null()},
             ABTCaseValuePair{std::move(checkStringAllArguments),
                              abt::make<abt::FunctionCall>("concat", std::move(argVars))}},
            makeABTFail(ErrorCodes::Error{7158201}, "$concat supports only strings"));

        pushABT(makeLet(std::move(varNames), std::move(binds), std::move(concatExpr)));
    }

    void visit(const ExpressionConcatArrays* expr) final {
        auto numChildren = expr->getChildren().size();
        _context->ensureArity(numChildren);

        // If there are no children, return an empty array.
        if (numChildren == 0) {
            pushABT(abt::Constant::emptyArray());
            return;
        }

        std::vector<abt::ABT> args;
        args.reserve(numChildren + 1);
        for (size_t i = 0; i < numChildren; ++i) {
            args.emplace_back(_context->popABTExpr());
        }
        std::reverse(args.begin(), args.end());

        std::vector<abt::ABT> argIsNullOrMissing;
        abt::ProjectionNameVector argNames;
        abt::ABTVector argVars;
        argIsNullOrMissing.reserve(numChildren);
        argNames.reserve(numChildren + 1);
        argVars.reserve(numChildren);
        for (size_t i = 0; i < numChildren; ++i) {
            argNames.emplace_back(makeLocalVariableName(_context->state.frameId(), 0));
            argVars.emplace_back(makeVariable(argNames.back()));
            argIsNullOrMissing.emplace_back(generateABTNullMissingOrUndefined(argNames.back()));
        }

        abt::ProjectionName resultName = makeLocalVariableName(_context->state.frameId(), 0);
        argNames.emplace_back(resultName);
        args.emplace_back(abt::make<abt::FunctionCall>("concatArrays", std::move(argVars)));

        pushABT(makeLet(
            std::move(argNames),
            std::move(args),
            buildABTMultiBranchConditionalFromCaseValuePairs(
                {ABTCaseValuePair{makeABTFunction("exists"_sd, makeVariable(resultName)),
                                  makeVariable(resultName)},
                 ABTCaseValuePair{
                     makeBooleanOpTree(abt::Operations::Or, std::move(argIsNullOrMissing)),
                     abt::Constant::null()}},
                makeABTFail(ErrorCodes::Error{7158000}, "$concatArrays only supports arrays"))));
    }

    void visit(const ExpressionCond* expr) final {
        visitConditionalExpression(expr);
    }
    void visit(const ExpressionDateDiff* expr) final {
        using namespace std::literals;

        const auto& children = expr->getChildren();
        invariant(children.size() == 5);

        auto startDateName = makeLocalVariableName(_context->state.frameId(), 0);
        auto endDateName = makeLocalVariableName(_context->state.frameId(), 0);
        auto unitName = makeLocalVariableName(_context->state.frameId(), 0);
        auto timezoneName = makeLocalVariableName(_context->state.frameId(), 0);
        auto startOfWeekName = makeLocalVariableName(_context->state.frameId(), 0);

        // An auxiliary boolean variable to hold a value of a common subexpression 'unit'=="week"
        // (string).
        auto unitIsWeekName = makeLocalVariableName(_context->state.frameId(), 0);

        auto startDateVar = makeVariable(startDateName);
        auto endDateVar = makeVariable(endDateName);
        auto unitVar = makeVariable(unitName);
        auto timezoneVar = makeVariable(timezoneName);
        auto startOfWeekVar = makeVariable(startOfWeekName);
        auto unitIsWeekVar = makeVariable(unitIsWeekName);

        // Get child expressions.
        boost::optional<abt::ABT> startOfWeekExpression;
        if (expr->isStartOfWeekSpecified()) {
            startOfWeekExpression = _context->popABTExpr();
        }
        auto timezoneExpression =
            expr->isTimezoneSpecified() ? _context->popABTExpr() : abt::Constant::str("UTC"_sd);
        auto unitExpression = _context->popABTExpr();
        auto endDateExpression = _context->popABTExpr();
        auto startDateExpression = _context->popABTExpr();

        auto timeZoneDBSlot = *_context->state.getTimeZoneDBSlot();
        auto timeZoneDBVar = makeABTVariable(timeZoneDBSlot);

        //  Set parameters for an invocation of built-in "dateDiff" function.
        abt::ABTVector arguments;
        arguments.push_back(timeZoneDBVar);
        arguments.push_back(startDateVar);
        arguments.push_back(endDateVar);
        arguments.push_back(unitVar);
        arguments.push_back(timezoneVar);
        if (expr->isStartOfWeekSpecified()) {
            // Parameter "startOfWeek" - if the time unit is the week, then pass value of parameter
            // "startOfWeek" of "$dateDiff" expression, otherwise pass a valid default value, since
            // "dateDiff" built-in function does not accept non-string type values for this
            // parameter.
            arguments.push_back(
                abt::make<abt::If>(unitIsWeekVar, startOfWeekVar, abt::Constant::str("sun"_sd)));
        }

        // Set bindings for the frame.
        abt::ABTVector bindings;
        abt::ProjectionNameVector bindingNames;
        bindingNames.push_back(startDateName);
        bindings.push_back(std::move(startDateExpression));
        bindingNames.push_back(endDateName);
        bindings.push_back(std::move(endDateExpression));
        bindingNames.push_back(unitName);
        bindings.push_back(std::move(unitExpression));
        bindingNames.push_back(timezoneName);
        bindings.push_back(std::move(timezoneExpression));
        if (expr->isStartOfWeekSpecified()) {
            bindingNames.push_back(startOfWeekName);
            bindings.push_back(std::move(*startOfWeekExpression));
            bindingNames.push_back(unitIsWeekName);
            bindings.push_back(generateABTIsEqualToStringCheck(unitVar, "week"_sd));
        }

        // Create an expression to invoke built-in "dateDiff" function.
        auto dateDiffFunctionCall = abt::make<abt::FunctionCall>("dateDiff", std::move(arguments));

        // Create expressions to check that each argument to "dateDiff" function exists, is not
        // null, and is of the correct type.
        std::vector<ABTCaseValuePair> inputValidationCases;

        // Return null if any of the parameters is either null or missing.
        inputValidationCases.push_back(generateABTReturnNullIfNullMissingOrUndefined(startDateVar));
        inputValidationCases.push_back(generateABTReturnNullIfNullMissingOrUndefined(endDateVar));
        inputValidationCases.push_back(generateABTReturnNullIfNullMissingOrUndefined(unitVar));
        inputValidationCases.push_back(generateABTReturnNullIfNullMissingOrUndefined(timezoneVar));
        if (expr->isStartOfWeekSpecified()) {
            inputValidationCases.emplace_back(
                abt::make<abt::BinaryOp>(abt::Operations::And,
                                         unitIsWeekVar,
                                         generateABTNullMissingOrUndefined(startOfWeekName)),
                abt::Constant::null());
        }

        // "timezone" parameter validation.
        inputValidationCases.emplace_back(
            generateABTNonStringCheck(timezoneName),
            makeABTFail(ErrorCodes::Error{7157919},
                        "$dateDiff parameter 'timezone' must be a string"));
        inputValidationCases.emplace_back(
            makeNot(makeABTFunction("isTimezone", timeZoneDBVar, timezoneVar)),
            makeABTFail(ErrorCodes::Error{7157920},
                        "$dateDiff parameter 'timezone' must be a valid timezone"));

        // "startDate" parameter validation.
        inputValidationCases.emplace_back(generateABTFailIfNotCoercibleToDate(
            startDateVar, ErrorCodes::Error{7157921}, "$dateDiff"_sd, "startDate"_sd));

        // "endDate" parameter validation.
        inputValidationCases.emplace_back(generateABTFailIfNotCoercibleToDate(
            endDateVar, ErrorCodes::Error{7157922}, "$dateDiff"_sd, "endDate"_sd));

        // "unit" parameter validation.
        inputValidationCases.emplace_back(
            generateABTNonStringCheck(unitName),
            makeABTFail(ErrorCodes::Error{7157923}, "$dateDiff parameter 'unit' must be a string"));
        inputValidationCases.emplace_back(
            makeNot(makeABTFunction("isTimeUnit", unitVar)),
            makeABTFail(ErrorCodes::Error{7157924},
                        "$dateDiff parameter 'unit' must be a valid time unit"));

        // "startOfWeek" parameter validation.
        if (expr->isStartOfWeekSpecified()) {
            // If 'timeUnit' value is equal to "week" then validate "startOfWeek" parameter.
            inputValidationCases.emplace_back(
                abt::make<abt::BinaryOp>(abt::Operations::And,
                                         unitIsWeekVar,
                                         generateABTNonStringCheck(startOfWeekName)),
                makeABTFail(ErrorCodes::Error{7157925},
                            "$dateDiff parameter 'startOfWeek' must be a string"));
            inputValidationCases.emplace_back(
                abt::make<abt::BinaryOp>(abt::Operations::And,
                                         unitIsWeekVar,
                                         makeNot(makeABTFunction("isDayOfWeek", startOfWeekVar))),
                makeABTFail(ErrorCodes::Error{7157926},
                            "$dateDiff parameter 'startOfWeek' must be a valid day of the week"));
        }

        auto dateDiffExpression = buildABTMultiBranchConditionalFromCaseValuePairs(
            std::move(inputValidationCases), std::move(dateDiffFunctionCall));

        pushABT(
            makeLet(std::move(bindingNames), std::move(bindings), std::move(dateDiffExpression)));
    }
    void visit(const ExpressionDateFromString* expr) final {
        const auto& children = expr->getChildren();
        invariant(children.size() == 5);
        _context->ensureArity(
            1 + (expr->isFormatSpecified() ? 1 : 0) + (expr->isTimezoneSpecified() ? 1 : 0) +
            (expr->isOnErrorSpecified() ? 1 : 0) + (expr->isOnNullSpecified() ? 1 : 0));

        // Get child expressions.
        auto onErrorExpression =
            expr->isOnErrorSpecified() ? _context->popABTExpr() : abt::Constant::null();

        auto onNullExpression =
            expr->isOnNullSpecified() ? _context->popABTExpr() : abt::Constant::null();

        auto formatExpression =
            expr->isFormatSpecified() ? _context->popABTExpr() : abt::Constant::null();
        auto formatName = makeLocalVariableName(_context->state.frameId(), 0);

        auto timezoneExpression =
            expr->isTimezoneSpecified() ? _context->popABTExpr() : abt::Constant::str("UTC"_sd);
        auto timezoneName = makeLocalVariableName(_context->state.frameId(), 0);

        auto dateStringExpression = _context->popABTExpr();
        auto dateStringName = makeLocalVariableName(_context->state.frameId(), 0);

        auto timeZoneDBSlot = *_context->state.getTimeZoneDBSlot();
        auto timeZoneDBName = getABTVariableName(timeZoneDBSlot);

        // Set parameters for an invocation of built-in "dateFromString" function.
        abt::ABTVector arguments;
        arguments.push_back(makeVariable(timeZoneDBName));
        // Set bindings for the frame.
        abt::ABTVector bindings;
        abt::ProjectionNameVector bindingNames;
        bindingNames.push_back(dateStringName);
        bindings.push_back(std::move(dateStringExpression));
        arguments.push_back(makeVariable(dateStringName));
        if (timezoneExpression.is<abt::Constant>()) {
            arguments.push_back(timezoneExpression);
        } else {
            bindingNames.push_back(timezoneName);
            bindings.push_back(timezoneExpression);
            arguments.push_back(makeVariable(timezoneName));
        }
        if (expr->isFormatSpecified()) {
            if (formatExpression.is<abt::Constant>()) {
                arguments.push_back(formatExpression);
            } else {
                bindingNames.push_back(formatName);
                bindings.push_back(formatExpression);
                arguments.push_back(makeVariable(formatName));
            }
        }

        // Create an expression to invoke built-in "dateFromString" function.
        std::string functionName =
            expr->isOnErrorSpecified() ? "dateFromStringNoThrow" : "dateFromString";
        auto dateFromStringFunctionCall =
            abt::make<abt::FunctionCall>(functionName, std::move(arguments));

        // Create expressions to check that each argument to "dateFromString" function exists, is
        // not null, and is of the correct type.
        std::vector<ABTCaseValuePair> inputValidationCases;

        // Return onNull if dateString is null or missing.
        inputValidationCases.push_back(
            {generateABTNullMissingOrUndefined(makeVariable(dateStringName)),
             std::move(onNullExpression)});

        // Create an expression to return Nothing if specified, or raise a conversion failure.
        // As long as onError is specified, a Nothing return will always be filled with onError.
        auto nonStringReturn = expr->isOnErrorSpecified()
            ? abt::Constant::nothing()
            : makeABTFail(ErrorCodes::ConversionFailure,
                          "$dateFromString requires that 'dateString' be a string");

        inputValidationCases.push_back(
            {generateABTNonStringCheck(makeVariable(dateStringName)), std::move(nonStringReturn)});

        if (expr->isTimezoneSpecified()) {
            if (timezoneExpression.is<abt::Constant>()) {
                // Return null if timezone is specified as either null or missing.
                inputValidationCases.push_back(
                    generateABTReturnNullIfNullMissingOrUndefined(timezoneExpression));
            } else {
                inputValidationCases.push_back(
                    generateABTReturnNullIfNullMissingOrUndefined(makeVariable(timezoneName)));
            }
        }

        if (expr->isFormatSpecified()) {
            // validate "format" parameter only if it has been specified.
            if (auto* formatExpressionConst = formatExpression.cast<abt::Constant>();
                formatExpressionConst) {
                inputValidationCases.push_back(
                    generateABTReturnNullIfNullMissingOrUndefined(formatExpression));
                auto [formatTag, formatVal] = formatExpressionConst->get();
                if (!sbe::value::isNullish(formatTag)) {
                    // We don't want to error on null.
                    uassert(4997802,
                            "$dateFromString requires that 'format' be a string",
                            sbe::value::isString(formatTag));
                    TimeZone::validateFromStringFormat(getStringView(formatTag, formatVal));
                }
            } else {
                inputValidationCases.push_back(
                    generateABTReturnNullIfNullMissingOrUndefined(makeVariable(formatName)));
                inputValidationCases.emplace_back(
                    generateABTNonStringCheck(makeVariable(formatName)),
                    makeABTFail(ErrorCodes::Error{4997803},
                                "$dateFromString requires that 'format' be a string"));
                inputValidationCases.emplace_back(
                    makeNot(makeABTFunction("validateFromStringFormat", makeVariable(formatName))),
                    // This should be unreachable. The validation function above will uassert on an
                    // invalid format string and then return true. It returns false on non-string
                    // input, but we already check for non-string format above.
                    abt::Constant::null());
            }
        }

        // "timezone" parameter validation.
        if (auto* timezoneExpressionConst = timezoneExpression.cast<abt::Constant>();
            timezoneExpressionConst) {
            auto [timezoneTag, timezoneVal] = timezoneExpressionConst->get();
            if (!sbe::value::isNullish(timezoneTag)) {
                // We don't want to error on null.
                uassert(4997805,
                        "$dateFromString parameter 'timezone' must be a string",
                        sbe::value::isString(timezoneTag));
                auto [timezoneDBTag, timezoneDBVal] =
                    _context->state.env->getAccessor(timeZoneDBSlot)->getViewOfValue();
                uassert(4997801,
                        "$dateFromString first argument must be a timezoneDB object",
                        timezoneDBTag == sbe::value::TypeTags::timeZoneDB);
                uassert(4997806,
                        "$dateFromString parameter 'timezone' must be a valid timezone",
                        sbe::vm::isValidTimezone(timezoneTag,
                                                 timezoneVal,
                                                 sbe::value::getTimeZoneDBView(timezoneDBVal)));
            }
        } else {
            inputValidationCases.emplace_back(
                generateABTNonStringCheck(makeVariable(timezoneName)),
                makeABTFail(ErrorCodes::Error{4997807},
                            "$dateFromString parameter 'timezone' must be a string"));
            inputValidationCases.emplace_back(
                makeNot(makeABTFunction(
                    "isTimezone", makeVariable(timeZoneDBName), makeVariable(timezoneName))),
                makeABTFail(ErrorCodes::Error{4997808},
                            "$dateFromString parameter 'timezone' must be a valid timezone"));
        }

        auto dateFromStringExpr = buildABTMultiBranchConditionalFromCaseValuePairs(
            std::move(inputValidationCases), std::move(dateFromStringFunctionCall));

        // If onError is specified, a Nothing return means that either dateString is not a string,
        // or the builtin dateFromStringNoThrow caught an error. We return onError in either case.
        if (expr->isOnErrorSpecified()) {
            dateFromStringExpr = abt::make<abt::BinaryOp>(abt::Operations::FillEmpty,
                                                          std::move(dateFromStringExpr),
                                                          std::move(onErrorExpression));
        }

        pushABT(
            makeLet(std::move(bindingNames), std::move(bindings), std::move(dateFromStringExpr)));
    }
    void visit(const ExpressionDateFromParts* expr) final {
        // This expression can carry null children depending on the set of fields provided,
        // to compute a date from parts so we only need to pop if a child exists.
        const auto& children = expr->getChildren();
        invariant(children.size() == 11);

        boost::optional<abt::ABT> eTimezone;
        if (children[10]) {
            eTimezone = _context->popABTExpr();
        }
        boost::optional<abt::ABT> eIsoDayOfWeek;
        if (children[9]) {
            eIsoDayOfWeek = _context->popABTExpr();
        }
        boost::optional<abt::ABT> eIsoWeek;
        if (children[8]) {
            eIsoWeek = _context->popABTExpr();
        }
        boost::optional<abt::ABT> eIsoWeekYear;
        if (children[7]) {
            eIsoWeekYear = _context->popABTExpr();
        }
        boost::optional<abt::ABT> eMillisecond;
        if (children[6]) {
            eMillisecond = _context->popABTExpr();
        }
        boost::optional<abt::ABT> eSecond;
        if (children[5]) {
            eSecond = _context->popABTExpr();
        }
        boost::optional<abt::ABT> eMinute;
        if (children[4]) {
            eMinute = _context->popABTExpr();
        }
        boost::optional<abt::ABT> eHour;
        if (children[3]) {
            eHour = _context->popABTExpr();
        }
        boost::optional<abt::ABT> eDay;
        if (children[2]) {
            eDay = _context->popABTExpr();
        }
        boost::optional<abt::ABT> eMonth;
        if (children[1]) {
            eMonth = _context->popABTExpr();
        }
        boost::optional<abt::ABT> eYear;
        if (children[0]) {
            eYear = _context->popABTExpr();
        }

        auto yearName = makeLocalVariableName(_context->state.frameId(), 0);
        auto monthName = makeLocalVariableName(_context->state.frameId(), 0);
        auto dayName = makeLocalVariableName(_context->state.frameId(), 0);
        auto hourName = makeLocalVariableName(_context->state.frameId(), 0);
        auto minName = makeLocalVariableName(_context->state.frameId(), 0);
        auto secName = makeLocalVariableName(_context->state.frameId(), 0);
        auto millisecName = makeLocalVariableName(_context->state.frameId(), 0);
        auto timeZoneName = makeLocalVariableName(_context->state.frameId(), 0);

        auto yearVar = makeVariable(yearName);
        auto monthVar = makeVariable(monthName);
        auto dayVar = makeVariable(dayName);
        auto hourVar = makeVariable(hourName);
        auto minVar = makeVariable(minName);
        auto secVar = makeVariable(secName);
        auto millisecVar = makeVariable(millisecName);
        auto timeZoneVar = makeVariable(timeZoneName);

        // Build a chain of nested bounds checks for each date part that is provided in the
        // expression. We elide the checks in the case that default values are used. These bound
        // checks are then used by folding over pairs of ite tests and else branches to implement
        // short-circuiting in the case that checks fail. To emulate the control flow of MQL for
        // this expression we interleave type conversion checks with time component bound checks.
        const auto minInt16 = std::numeric_limits<int16_t>::lowest();
        const auto maxInt16 = std::numeric_limits<int16_t>::max();

        // Constructs an expression that does a bound check of var over a closed interval [lower,
        // upper].
        auto boundedCheck = [](abt::ABT& var,
                               int16_t lower,
                               int16_t upper,
                               const std::string& varName) {
            str::stream errMsg;
            if (varName == "year" || varName == "isoWeekYear") {
                errMsg << "'" << varName << "'"
                       << " must evaluate to an integer in the range " << lower << " to " << upper;
            } else {
                errMsg << "'" << varName << "'"
                       << " must evaluate to a value in the range [" << lower << ", " << upper
                       << "]";
            }
            return ABTCaseValuePair{
                abt::make<abt::BinaryOp>(
                    abt::Operations::Or,
                    abt::make<abt::BinaryOp>(abt::Operations::Lt, var, abt::Constant::int32(lower)),
                    abt::make<abt::BinaryOp>(
                        abt::Operations::Gt, var, abt::Constant::int32(upper))),
                makeABTFail(ErrorCodes::Error{7157916}, errMsg)};
        };

        // Here we want to validate each field that is provided as input to the agg expression. To
        // do this we implement the following checks:
        // 1) Check if the value in a given slot null or missing.
        // 2) Check if the value in a given slot is an integral int64.
        auto fieldConversionBinding = [](abt::ABT& expr,
                                         sbe::value::FrameIdGenerator* frameIdGenerator,
                                         const std::string& varName) {
            auto outerName = makeLocalVariableName(frameIdGenerator->generate(), 0);
            auto outerVar = makeVariable(outerName);
            auto convertedFieldName = makeLocalVariableName(frameIdGenerator->generate(), 0);
            auto convertedFieldVar = makeVariable(convertedFieldName);

            return abt::make<abt::Let>(
                outerName,
                std::move(expr),
                abt::make<abt::If>(
                    abt::make<abt::BinaryOp>(abt::Operations::Or,
                                             makeNot(makeABTFunction("exists", outerVar)),
                                             makeABTFunction("isNull", outerVar)),
                    abt::Constant::null(),
                    abt::make<abt::Let>(
                        convertedFieldName,
                        makeABTFunction("convert",
                                        outerVar,
                                        abt::Constant::int32(static_cast<int32_t>(
                                            sbe::value::TypeTags::NumberInt64))),
                        abt::make<abt::If>(makeABTFunction("exists", convertedFieldVar),
                                           convertedFieldVar,
                                           makeABTFail(ErrorCodes::Error{7157917},
                                                       str::stream()
                                                           << "'" << varName << "'"
                                                           << " must evaluate to an integer")))));
        };

        // Build two vectors on the fly to elide bound and conversion for defaulted values.
        std::vector<ABTCaseValuePair>
            boundChecks;  // checks for lower and upper bounds of date fields.

        // Make a disjunction of null checks for each date part by over this vector. These checks
        // are necessary after the initial conversion computation because we need have the outer let
        // binding evaluate to null if any field is null.
        auto nullExprs = abt::ABTVector{generateABTNullMissingOrUndefined(timeZoneName),
                                        generateABTNullMissingOrUndefined(millisecName),
                                        generateABTNullMissingOrUndefined(secName),
                                        generateABTNullMissingOrUndefined(minName),
                                        generateABTNullMissingOrUndefined(hourName),
                                        generateABTNullMissingOrUndefined(dayName),
                                        generateABTNullMissingOrUndefined(monthName),
                                        generateABTNullMissingOrUndefined(yearName)};

        // The first "if" expression allows short-circuting of the null field case. If the nullish
        // checks pass, then we check the bounds of each field and invoke the builtins if all checks
        // pass.
        boundChecks.push_back(
            {makeBooleanOpTree(abt::Operations::Or, std::move(nullExprs)), abt::Constant::null()});

        // Operands is for the outer let bindings.
        abt::ABTVector operands;
        if (eIsoWeekYear) {
            boundChecks.push_back(boundedCheck(yearVar, 1, 9999, "isoWeekYear"));
            operands.push_back(fieldConversionBinding(
                *eIsoWeekYear, _context->state.frameIdGenerator, "isoWeekYear"));
            if (!eIsoWeek) {
                operands.push_back(abt::Constant::int32(1));
            } else {
                boundChecks.push_back(boundedCheck(monthVar, minInt16, maxInt16, "isoWeek"));
                operands.push_back(
                    fieldConversionBinding(*eIsoWeek, _context->state.frameIdGenerator, "isoWeek"));
            }
            if (!eIsoDayOfWeek) {
                operands.push_back(abt::Constant::int32(1));
            } else {
                boundChecks.push_back(boundedCheck(dayVar, minInt16, maxInt16, "isoDayOfWeek"));
                operands.push_back(fieldConversionBinding(
                    *eIsoDayOfWeek, _context->state.frameIdGenerator, "isoDayOfWeek"));
            }
        } else {
            // The regular year/month/day case.
            if (!eYear) {
                operands.push_back(abt::Constant::int32(1970));
            } else {
                boundChecks.push_back(boundedCheck(yearVar, 1, 9999, "year"));
                operands.push_back(
                    fieldConversionBinding(*eYear, _context->state.frameIdGenerator, "year"));
            }
            if (!eMonth) {
                operands.push_back(abt::Constant::int32(1));
            } else {
                boundChecks.push_back(boundedCheck(monthVar, minInt16, maxInt16, "month"));
                operands.push_back(
                    fieldConversionBinding(*eMonth, _context->state.frameIdGenerator, "month"));
            }
            if (!eDay) {
                operands.push_back(abt::Constant::int32(1));
            } else {
                boundChecks.push_back(boundedCheck(dayVar, minInt16, maxInt16, "day"));
                operands.push_back(
                    fieldConversionBinding(*eDay, _context->state.frameIdGenerator, "day"));
            }
        }
        if (!eHour) {
            operands.push_back(abt::Constant::int32(0));
        } else {
            boundChecks.push_back(boundedCheck(hourVar, minInt16, maxInt16, "hour"));
            operands.push_back(
                fieldConversionBinding(*eHour, _context->state.frameIdGenerator, "hour"));
        }
        if (!eMinute) {
            operands.push_back(abt::Constant::int32(0));
        } else {
            boundChecks.push_back(boundedCheck(minVar, minInt16, maxInt16, "minute"));
            operands.push_back(
                fieldConversionBinding(*eMinute, _context->state.frameIdGenerator, "minute"));
        }
        if (!eSecond) {
            operands.push_back(abt::Constant::int32(0));
        } else {
            // MQL doesn't place bound restrictions on the second field, because seconds carry over
            // to minutes and can be large ints such as 71,841,012 or even unix epochs.
            operands.push_back(
                fieldConversionBinding(*eSecond, _context->state.frameIdGenerator, "second"));
        }
        if (!eMillisecond) {
            operands.push_back(abt::Constant::int32(0));
        } else {
            // MQL doesn't enforce bound restrictions on millisecond fields because milliseconds
            // carry over to seconds.
            operands.push_back(fieldConversionBinding(
                *eMillisecond, _context->state.frameIdGenerator, "millisecond"));
        }
        if (!eTimezone) {
            operands.push_back(abt::Constant::str("UTC"));
        } else {
            // Validate that eTimezone is a string.
            auto timeZoneName = makeLocalVariableName(_context->state.frameId(), 0);
            auto timeZoneVar = makeVariable(timeZoneName);
            operands.push_back(abt::make<abt::Let>(
                timeZoneName,
                std::move(*eTimezone),
                abt::make<abt::If>(
                    makeABTFunction("isString", timeZoneVar),
                    timeZoneVar,
                    makeABTFail(ErrorCodes::Error{7157918},
                                str::stream() << "'timezone' must evaluate to a string"))));
        }

        // Invocation of the datePartsWeekYear and dateParts functions depend on a TimeZoneDatabase
        // for datetime computation. This global object is registered as an unowned value in the
        // runtime environment so we pass the corresponding slot to the datePartsWeekYear and
        // dateParts functions as a variable.
        auto timeZoneDBSlot = *_context->state.getTimeZoneDBSlot();
        auto computeDate = makeABTFunction(eIsoWeekYear ? "datePartsWeekYear" : "dateParts",
                                           makeABTVariable(timeZoneDBSlot),
                                           std::move(yearVar),
                                           std::move(monthVar),
                                           std::move(dayVar),
                                           std::move(hourVar),
                                           std::move(minVar),
                                           std::move(secVar),
                                           std::move(millisecVar),
                                           std::move(timeZoneVar));

        pushABT(makeLet({std::move(yearName),
                         std::move(monthName),
                         std::move(dayName),
                         std::move(hourName),
                         std::move(minName),
                         std::move(secName),
                         std::move(millisecName),
                         std::move(timeZoneName)},
                        std::move(operands),
                        buildABTMultiBranchConditionalFromCaseValuePairs(std::move(boundChecks),
                                                                         std::move(computeDate))));
    }

    void visit(const ExpressionDateToParts* expr) final {
        const auto& children = expr->getChildren();
        auto dateName = makeLocalVariableName(_context->state.frameId(), 0);
        auto timezoneName = makeLocalVariableName(_context->state.frameId(), 0);
        auto isoflagName = makeLocalVariableName(_context->state.frameId(), 0);

        auto dateVar = makeVariable(dateName);
        auto timezoneVar = makeVariable(timezoneName);
        auto isoflagVar = makeVariable(isoflagName);

        // Initialize arguments with values from stack or default values.
        auto isoflag = abt::Constant::boolean(false);
        if (children[2]) {
            isoflag = _context->popABTExpr();
        }
        auto timezone = abt::Constant::str("UTC");
        if (children[1]) {
            timezone = _context->popABTExpr();
        }
        if (!children[0]) {
            pushABT(makeABTFail(ErrorCodes::Error{7157911}, "$dateToParts must include a date"));
            return;
        }
        auto date = _context->popABTExpr();

        auto timeZoneDBSlot = *_context->state.getTimeZoneDBSlot();
        auto timeZoneDBVar = makeABTVariable(timeZoneDBSlot);

        auto isoTypeMask = getBSONTypeMask(sbe::value::TypeTags::Boolean);

        // Check that each argument exists, is not null, and is the correct type.
        auto totalDateToPartsFunc = buildABTMultiBranchConditionalFromCaseValuePairs(
            {ABTCaseValuePair{generateABTNullMissingOrUndefined(timezoneName),
                              abt::Constant::null()},
             ABTCaseValuePair{
                 makeNot(makeABTFunction("isString", timezoneVar)),
                 makeABTFail(ErrorCodes::Error{7157912}, "$dateToParts timezone must be a string")},
             ABTCaseValuePair{makeNot(makeABTFunction("isTimezone", timeZoneDBVar, timezoneVar)),
                              makeABTFail(ErrorCodes::Error{7157913},
                                          "$dateToParts timezone must be a valid timezone")},
             ABTCaseValuePair{generateABTNullMissingOrUndefined(isoflagName),
                              abt::Constant::null()},
             ABTCaseValuePair{
                 makeNot(
                     makeABTFunction("typeMatch", isoflagVar, abt::Constant::int32(isoTypeMask))),
                 makeABTFail(ErrorCodes::Error{7157914}, "$dateToParts iso8601 must be a boolean")},
             ABTCaseValuePair{generateABTNullMissingOrUndefined(dateName), abt::Constant::null()},
             ABTCaseValuePair{makeNot(makeABTFunction(
                                  "typeMatch", dateVar, abt::Constant::int32(dateTypeMask()))),
                              makeABTFail(ErrorCodes::Error{7157915},
                                          "$dateToParts date must have the format of a date")},
             // Determine whether to call dateToParts or isoDateToParts.
             ABTCaseValuePair{
                 abt::make<abt::BinaryOp>(
                     abt::Operations::Eq, isoflagVar, abt::Constant::boolean(false)),
                 makeABTFunction("dateToParts", timeZoneDBVar, dateVar, timezoneVar, isoflagVar)}},
            makeABTFunction("isoDateToParts", timeZoneDBVar, dateVar, timezoneVar, isoflagVar));

        pushABT(makeLet({std::move(dateName), std::move(timezoneName), std::move(isoflagName)},
                        abt::makeSeq(std::move(date), std::move(timezone), std::move(isoflag)),
                        std::move(totalDateToPartsFunc)));
    }

    void visit(const ExpressionDateToString* expr) final {
        const auto& children = expr->getChildren();
        invariant(children.size() == 4);
        _context->ensureArity(1 + (expr->isFormatSpecified() ? 1 : 0) +
                              (expr->isTimezoneSpecified() ? 1 : 0) +
                              (expr->isOnNullSpecified() ? 1 : 0));

        // Get child expressions.
        abt::ABT onNullExpression =
            expr->isOnNullSpecified() ? _context->popABTExpr() : abt::Constant::null();

        abt::ABT timezoneExpression =
            expr->isTimezoneSpecified() ? _context->popABTExpr() : abt::Constant::str("UTC"_sd);
        abt::ABT dateExpression = _context->popABTExpr();

        abt::ABT formatExpression = expr->isFormatSpecified()
            ? _context->popABTExpr()
            : abt::Constant::str(kIsoFormatStringZ);  // assumes UTC until disproven

        auto timeZoneDBSlot = *_context->state.getTimeZoneDBSlot();
        auto [timezoneDBTag, timezoneDBVal] =
            _context->state.env->getAccessor(timeZoneDBSlot)->getViewOfValue();
        uassert(4997900,
                "$dateToString first argument must be a timezoneDB object",
                timezoneDBTag == sbe::value::TypeTags::timeZoneDB);
        auto timezoneDB = sbe::value::getTimeZoneDBView(timezoneDBVal);

        // Local bind to hold the date, timezone and format expression results
        auto dateName = makeLocalVariableName(_context->state.frameId(), 0);
        auto timezoneName = makeLocalVariableName(_context->state.frameId(), 0);
        auto formatName = makeLocalVariableName(_context->state.frameId(), 0);
        auto dateToStringName = makeLocalVariableName(_context->state.frameId(), 0);

        // Create expressions to check that each argument to "dateToString" function exists, is not
        // null, and is of the correct type.
        std::vector<ABTCaseValuePair> inputValidationCases;
        // Return the evaluation of the function, if the result is correct.
        inputValidationCases.push_back(
            {makeABTFunction("exists"_sd, makeVariable(dateToStringName)),
             makeVariable(dateToStringName)});
        // Return onNull if date is null or missing.
        inputValidationCases.push_back({generateABTNullMissingOrUndefined(makeVariable(dateName)),
                                        std::move(onNullExpression)});
        // Return null if format or timezone is null or missing.
        inputValidationCases.push_back(
            generateABTReturnNullIfNullMissingOrUndefined(makeVariable(formatName)));
        inputValidationCases.push_back(
            generateABTReturnNullIfNullMissingOrUndefined(makeVariable(timezoneName)));

        // "date" parameter validation.
        inputValidationCases.emplace_back(generateABTFailIfNotCoercibleToDate(
            makeVariable(dateName), ErrorCodes::Error{4997901}, "$dateToString"_sd, "date"_sd));

        // "timezone" parameter validation.
        if (auto* timezoneExpressionConst = timezoneExpression.cast<abt::Constant>();
            timezoneExpressionConst) {
            auto [timezoneTag, timezoneVal] = timezoneExpressionConst->get();
            if (!sbe::value::isNullish(timezoneTag)) {
                // If the query did not specify a format string and a non-UTC timezone was
                // specified, the default format should not use a 'Z' suffix.
                if (!expr->isFormatSpecified() &&
                    !(sbe::vm::getTimezone(timezoneTag, timezoneVal, timezoneDB).isUtcZone())) {
                    formatExpression = abt::Constant::str(kIsoFormatStringNonZ);
                }

                // We don't want to error on null.
                uassert(4997905,
                        "$dateToString parameter 'timezone' must be a string",
                        sbe::value::isString(timezoneTag));
                uassert(4997906,
                        "$dateToString parameter 'timezone' must be a valid timezone",
                        sbe::vm::isValidTimezone(timezoneTag, timezoneVal, timezoneDB));
            }
        } else {
            inputValidationCases.emplace_back(
                generateABTNonStringCheck(makeVariable(timezoneName)),
                makeABTFail(ErrorCodes::Error{4997907},
                            "$dateToString parameter 'timezone' must be a string"));
            inputValidationCases.emplace_back(
                makeNot(makeABTFunction(
                    "isTimezone", makeABTVariable(timeZoneDBSlot), makeVariable(timezoneName))),
                makeABTFail(ErrorCodes::Error{4997908},
                            "$dateToString parameter 'timezone' must be a valid timezone"));
        }

        // "format" parameter validation.
        if (auto* formatExpressionConst = formatExpression.cast<abt::Constant>();
            formatExpressionConst) {
            auto [formatTag, formatVal] = formatExpressionConst->get();
            if (!sbe::value::isNullish(formatTag)) {
                // We don't want to return an error on null.
                uassert(4997902,
                        "$dateToString parameter 'format' must be a string",
                        sbe::value::isString(formatTag));
                TimeZone::validateToStringFormat(getStringView(formatTag, formatVal));
            }
        } else {
            inputValidationCases.emplace_back(
                generateABTNonStringCheck(makeVariable(formatName)),
                makeABTFail(ErrorCodes::Error{4997903},
                            "$dateToString parameter 'format' must be a string"));
            inputValidationCases.emplace_back(
                makeNot(makeABTFunction("isValidToStringFormat", makeVariable(formatName))),
                makeABTFail(ErrorCodes::Error{4997904},
                            "$dateToString parameter 'format' must be a valid format"));
        }

        // Set parameters for an invocation of built-in "dateToString" function.
        abt::ABTVector arguments;
        arguments.push_back(makeABTVariable(timeZoneDBSlot));
        arguments.push_back(makeVariable(dateName));
        arguments.push_back(makeVariable(formatName));
        arguments.push_back(makeVariable(timezoneName));

        // Create an expression to invoke built-in "dateToString" function.
        auto dateToStringFunctionCall =
            abt::make<abt::FunctionCall>("dateToString", std::move(arguments));

        pushABT(makeLet({std::move(dateName),
                         std::move(formatName),
                         std::move(timezoneName),
                         std::move(dateToStringName)},
                        abt::makeSeq(std::move(dateExpression),
                                     std::move(formatExpression),
                                     std::move(timezoneExpression),
                                     std::move(dateToStringFunctionCall)),
                        buildABTMultiBranchConditionalFromCaseValuePairs(
                            std::move(inputValidationCases), abt::Constant::nothing())));
    }
    void visit(const ExpressionDateTrunc* expr) final {
        const auto& children = expr->getChildren();
        invariant(children.size() == 5);
        _context->ensureArity(2 + (expr->isBinSizeSpecified() ? 1 : 0) +
                              (expr->isTimezoneSpecified() ? 1 : 0) +
                              (expr->isStartOfWeekSpecified() ? 1 : 0));

        // Get child expressions.
        auto startOfWeekExpression =
            expr->isStartOfWeekSpecified() ? _context->popABTExpr() : abt::Constant::str("sun"_sd);
        auto timezoneExpression =
            expr->isTimezoneSpecified() ? _context->popABTExpr() : abt::Constant::str("UTC"_sd);
        auto binSizeExpression =
            expr->isBinSizeSpecified() ? _context->popABTExpr() : abt::Constant::int64(1);
        auto unitExpression = _context->popABTExpr();
        auto dateExpression = _context->popABTExpr();

        auto timeZoneDBSlot = *_context->state.getTimeZoneDBSlot();
        auto timeZoneDBVar = makeABTVariable(timeZoneDBSlot);
        auto [timezoneDBTag, timezoneDBVal] =
            _context->state.env->getAccessor(timeZoneDBSlot)->getViewOfValue();
        tassert(7157927,
                "$dateTrunc first argument must be a timezoneDB object",
                timezoneDBTag == sbe::value::TypeTags::timeZoneDB);
        auto timezoneDB = sbe::value::getTimeZoneDBView(timezoneDBVal);

        // Local bind to hold the argument expression results
        auto dateName = makeLocalVariableName(_context->state.frameId(), 0);
        auto unitName = makeLocalVariableName(_context->state.frameId(), 0);
        auto binSizeName = makeLocalVariableName(_context->state.frameId(), 0);
        auto timezoneName = makeLocalVariableName(_context->state.frameId(), 0);
        auto startOfWeekName = makeLocalVariableName(_context->state.frameId(), 0);

        auto dateVar = makeVariable(dateName);
        auto unitVar = makeVariable(unitName);
        auto binSizeVar = makeVariable(binSizeName);
        auto timezoneVar = makeVariable(timezoneName);
        auto startOfWeekVar = makeVariable(startOfWeekName);

        // Set parameters for an invocation of built-in "dateTrunc" function.
        abt::ABTVector arguments;
        arguments.push_back(timeZoneDBVar);
        arguments.push_back(dateVar);
        arguments.push_back(unitVar);
        arguments.push_back(binSizeVar);
        arguments.push_back(timezoneVar);
        arguments.push_back(startOfWeekVar);

        // Create an expression to invoke built-in "dateTrunc" function.
        auto dateTruncFunctionCall =
            abt::make<abt::FunctionCall>("dateTrunc", std::move(arguments));
        auto dateTruncName = makeLocalVariableName(_context->state.frameId(), 0);
        auto dateTruncVar = makeVariable(dateTruncName);

        // Local bind to hold the unitIsWeek common subexpression
        auto unitIsWeekName = makeLocalVariableName(_context->state.frameId(), 0);
        auto unitIsWeekVar = makeVariable(unitIsWeekName);
        auto unitIsWeek = generateABTIsEqualToStringCheck(unitExpression, "week"_sd);

        // Create expressions to check that each argument to "dateTrunc" function exists, is not
        // null, and is of the correct type.
        std::vector<ABTCaseValuePair> inputValidationCases;

        // Return null if any of the parameters is either null or missing.
        inputValidationCases.push_back(generateABTReturnNullIfNullMissingOrUndefined(dateVar));
        inputValidationCases.push_back(
            generateABTReturnNullIfNullMissingOrUndefined(unitExpression));
        inputValidationCases.push_back(
            generateABTReturnNullIfNullMissingOrUndefined(binSizeExpression));
        inputValidationCases.push_back(
            generateABTReturnNullIfNullMissingOrUndefined(timezoneExpression));
        inputValidationCases.emplace_back(
            abt::make<abt::BinaryOp>(abt::Operations::And,
                                     unitIsWeekVar,
                                     generateABTNullMissingOrUndefined(startOfWeekVar)),
            abt::Constant::null());

        // "timezone" parameter validation.
        if (timezoneExpression.is<abt::Constant>()) {
            auto [timezoneTag, timezoneVal] = timezoneExpression.cast<abt::Constant>()->get();
            tassert(7157928,
                    "$dateTrunc parameter 'timezone' must be a string",
                    sbe::value::isString(timezoneTag));
            tassert(7157929,
                    "$dateTrunc parameter 'timezone' must be a valid timezone",
                    sbe::vm::isValidTimezone(timezoneTag, timezoneVal, timezoneDB));
        } else {
            inputValidationCases.emplace_back(
                generateABTNonStringCheck(timezoneVar),
                makeABTFail(ErrorCodes::Error{7157930},
                            "$dateTrunc parameter 'timezone' must be a string"));
            inputValidationCases.emplace_back(
                makeNot(makeABTFunction("isTimezone", timeZoneDBVar, timezoneVar)),
                makeABTFail(ErrorCodes::Error{7157931},
                            "$dateTrunc parameter 'timezone' must be a valid timezone"));
        }

        // "date" parameter validation.
        inputValidationCases.emplace_back(generateABTFailIfNotCoercibleToDate(
            dateVar, ErrorCodes::Error{7157932}, "$dateTrunc"_sd, "date"_sd));

        // "unit" parameter validation.
        if (unitExpression.is<abt::Constant>()) {
            auto [unitTag, unitVal] = unitExpression.cast<abt::Constant>()->get();
            tassert(7157933,
                    "$dateTrunc parameter 'unit' must be a string",
                    sbe::value::isString(unitTag));
            auto unitString = sbe::value::getStringView(unitTag, unitVal);
            tassert(7157934,
                    "$dateTrunc parameter 'unit' must be a valid time unit",
                    isValidTimeUnit(unitString));
        } else {
            inputValidationCases.emplace_back(
                generateABTNonStringCheck(unitVar),
                makeABTFail(ErrorCodes::Error{7157935},
                            "$dateTrunc parameter 'unit' must be a string"));
            inputValidationCases.emplace_back(
                makeNot(makeABTFunction("isTimeUnit", unitVar)),
                makeABTFail(ErrorCodes::Error{7157936},
                            "$dateTrunc parameter 'unit' must be a valid time unit"));
        }

        // "binSize" parameter validation.
        if (expr->isBinSizeSpecified()) {
            if (binSizeExpression.is<abt::Constant>()) {
                auto [binSizeTag, binSizeValue] = binSizeExpression.cast<abt::Constant>()->get();
                tassert(7157937,
                        "$dateTrunc parameter 'binSize' must be coercible to a positive 64-bit "
                        "integer",
                        sbe::value::isNumber(binSizeTag));
                auto [binSizeLongOwn, binSizeLongTag, binSizeLongValue] =
                    sbe::value::genericNumConvert(
                        binSizeTag, binSizeValue, sbe::value::TypeTags::NumberInt64);
                tassert(7157938,
                        "$dateTrunc parameter 'binSize' must be coercible to a positive 64-bit "
                        "integer",
                        binSizeLongTag != sbe::value::TypeTags::Nothing);
                auto binSize = sbe::value::bitcastTo<int64_t>(binSizeLongValue);
                tassert(7157939,
                        "$dateTrunc parameter 'binSize' must be coercible to a positive 64-bit "
                        "integer",
                        binSize > 0);
            } else {
                inputValidationCases.emplace_back(
                    makeNot(abt::make<abt::BinaryOp>(
                        abt::Operations::And,
                        abt::make<abt::BinaryOp>(
                            abt::Operations::And,
                            makeABTFunction("isNumber", binSizeVar),
                            makeABTFunction(
                                "exists",
                                makeABTFunction("convert",
                                                binSizeVar,
                                                abt::Constant::int32(static_cast<int32_t>(
                                                    sbe::value::TypeTags::NumberInt64))))),
                        generateABTPositiveCheck(binSizeVar))),
                    makeABTFail(
                        ErrorCodes::Error{7157940},
                        "$dateTrunc parameter 'binSize' must be coercible to a positive 64-bit "
                        "integer"));
            }
        }

        // "startOfWeek" parameter validation.
        if (expr->isStartOfWeekSpecified()) {
            if (startOfWeekExpression.is<abt::Constant>()) {
                auto [startOfWeekTag, startOfWeekVal] =
                    startOfWeekExpression.cast<abt::Constant>()->get();
                tassert(7157941,
                        "$dateTrunc parameter 'startOfWeek' must be a string",
                        sbe::value::isString(startOfWeekTag));
                auto startOfWeekString = sbe::value::getStringView(startOfWeekTag, startOfWeekVal);
                tassert(7157942,
                        "$dateTrunc parameter 'startOfWeek' must be a valid day of the week",
                        isValidDayOfWeek(startOfWeekString));
            } else {
                // If 'timeUnit' value is equal to "week" then validate "startOfWeek" parameter.
                inputValidationCases.emplace_back(
                    abt::make<abt::BinaryOp>(abt::Operations::And,
                                             unitIsWeekVar,
                                             generateABTNonStringCheck(startOfWeekVar)),
                    makeABTFail(ErrorCodes::Error{7157943},
                                "$dateTrunc parameter 'startOfWeek' must be a string"));
                inputValidationCases.emplace_back(
                    abt::make<abt::BinaryOp>(
                        abt::Operations::And,
                        unitIsWeekVar,
                        makeNot(makeABTFunction("isDayOfWeek", startOfWeekVar))),
                    makeABTFail(
                        ErrorCodes::Error{7157944},
                        "$dateTrunc parameter 'startOfWeek' must be a valid day of the week"));
            }
        }

        pushABT(makeLet(
            {std::move(dateName),
             std::move(unitName),
             std::move(binSizeName),
             std::move(timezoneName),
             std::move(startOfWeekName),
             std::move(dateTruncName)},
            abt::makeSeq(std::move(dateExpression),
                         std::move(unitExpression),
                         std::move(binSizeExpression),
                         std::move(timezoneExpression),
                         std::move(startOfWeekExpression),
                         std::move(dateTruncFunctionCall)),
            abt::make<abt::If>(makeABTFunction("exists", dateTruncVar),
                               dateTruncVar,
                               abt::make<abt::Let>(std::move(unitIsWeekName),
                                                   std::move(unitIsWeek),
                                                   buildABTMultiBranchConditionalFromCaseValuePairs(
                                                       std::move(inputValidationCases),
                                                       abt::Constant::nothing())))));
    }
    void visit(const ExpressionDivide* expr) final {
        _context->ensureArity(2);
        auto rhs = _context->popABTExpr();
        auto lhs = _context->popABTExpr();

        auto lhsName = makeLocalVariableName(_context->state.frameIdGenerator->generate(), 0);
        auto rhsName = makeLocalVariableName(_context->state.frameIdGenerator->generate(), 0);

        auto checkIsNumber =
            abt::make<abt::BinaryOp>(abt::Operations::And,
                                     makeABTFunction("isNumber", makeVariable(lhsName)),
                                     makeABTFunction("isNumber", makeVariable(rhsName)));

        auto checkIsNullOrMissing =
            abt::make<abt::BinaryOp>(abt::Operations::Or,
                                     generateABTNullMissingOrUndefined(lhsName),
                                     generateABTNullMissingOrUndefined(rhsName));

        auto divideExpr = buildABTMultiBranchConditionalFromCaseValuePairs(
            {ABTCaseValuePair{std::move(checkIsNullOrMissing), abt::Constant::null()},
             ABTCaseValuePair{std::move(checkIsNumber),
                              abt::make<abt::BinaryOp>(abt::Operations::Div,
                                                       makeVariable(lhsName),
                                                       makeVariable(rhsName))}},
            makeABTFail(ErrorCodes::Error{7157719}, "$divide only supports numeric types"));

        pushABT(makeLet({std::move(lhsName), std::move(rhsName)},
                        abt::makeSeq(std::move(lhs), std::move(rhs)),
                        std::move(divideExpr)));
    }
    void visit(const ExpressionExp* expr) final {
        auto inputName = makeLocalVariableName(_context->state.frameIdGenerator->generate(), 0);

        auto expExpr = buildABTMultiBranchConditionalFromCaseValuePairs(
            {ABTCaseValuePair{generateABTNullMissingOrUndefined(inputName), abt::Constant::null()},
             ABTCaseValuePair{
                 generateABTNonNumericCheck(inputName),
                 makeABTFail(ErrorCodes::Error{7157704}, "$exp only supports numeric types")}},
            makeABTFunction("exp", makeVariable(inputName)));

        pushABT(
            abt::make<abt::Let>(std::move(inputName), _context->popABTExpr(), std::move(expExpr)));
    }
    void visit(const ExpressionFieldPath* expr) final {
        _context->pushExpr(generateExpressionFieldPath(_context->state,
                                                       expr->getFieldPath(),
                                                       expr->getVariableId(),
                                                       _context->rootSlot,
                                                       *_context->slots,
                                                       &_context->environment));
    }
    void visit(const ExpressionFilter* expr) final {
        unsupportedExpression("$filter");
    }

    void visit(const ExpressionFloor* expr) final {
        auto inputName = makeLocalVariableName(_context->state.frameIdGenerator->generate(), 0);

        auto floorExpr = buildABTMultiBranchConditionalFromCaseValuePairs(
            {ABTCaseValuePair{generateABTNullMissingOrUndefined(inputName), abt::Constant::null()},
             ABTCaseValuePair{
                 generateABTNonNumericCheck(inputName),
                 makeABTFail(ErrorCodes::Error{7157703}, "$floor only supports numeric types")}},
            makeABTFunction("floor", makeVariable(inputName)));

        pushABT(abt::make<abt::Let>(
            std::move(inputName), _context->popABTExpr(), std::move(floorExpr)));
    }
    void visit(const ExpressionIfNull* expr) final {
        auto numChildren = expr->getChildren().size();
        invariant(numChildren >= 2);

        std::vector<abt::ABT> values;
        values.reserve(numChildren);
        for (size_t i = 0; i < numChildren; ++i) {
            values.emplace_back(_context->popABTExpr());
        }
        std::reverse(values.begin(), values.end());

        auto resultExpr = makeIfNullExpr(std::move(values), _context->state.frameIdGenerator);

        pushABT(std::move(resultExpr));
    }
    void visit(const ExpressionIn* expr) final {
        auto arrExpArg = _context->popABTExpr();
        auto expArg = _context->popABTExpr();
        auto expLocalVar = makeLocalVariableName(_context->state.frameId(), 0);
        auto arrLocalVar = makeLocalVariableName(_context->state.frameId(), 0);

        abt::ABTVector functionArgs{makeVariable(expLocalVar), makeVariable(arrLocalVar)};
        auto collatorSlot = _context->state.getCollatorSlot();
        if (collatorSlot) {
            functionArgs.emplace_back(makeABTVariable(*collatorSlot));
        }

        auto inExpr = abt::make<abt::If>(
            // Check that the arr argument is an array and is not missing.
            makeFillEmptyFalse(makeABTFunction("isArray", makeVariable(arrLocalVar))),
            (collatorSlot ? abt::make<abt::FunctionCall>("collIsMember", std::move(functionArgs))
                          : abt::make<abt::FunctionCall>("isMember", std::move(functionArgs))),
            makeABTFail(ErrorCodes::Error{5153700}, "$in requires an array as a second argument"));

        pushABT(makeLet({std::move(expLocalVar), std::move(arrLocalVar)},
                        abt::makeSeq(std::move(expArg), std::move(arrExpArg)),
                        std::move(inExpr)));
    }
    void visit(const ExpressionIndexOfArray* expr) final {
        unsupportedExpression(expr->getOpName());
    }

    void visit(const ExpressionIndexOfBytes* expr) final {
        visitIndexOfFunction(expr, _context, "indexOfBytes");
    }

    void visit(const ExpressionIndexOfCP* expr) final {
        visitIndexOfFunction(expr, _context, "indexOfCP");
    }
    void visit(const ExpressionIsNumber* expr) final {
        auto arg = _context->popABTExpr();
        auto varName = makeLocalVariableName(_context->state.frameId(), 0);
        auto exprIsNum = abt::make<abt::If>(makeABTFunction("exists", makeVariable(varName)),
                                            makeABTFunction("isNumber", makeVariable(varName)),
                                            abt::Constant::boolean(false));

        pushABT(abt::make<abt::Let>(varName, std::move(arg), std::move(exprIsNum)));
    }
    void visit(const ExpressionLet* expr) final {
        invariant(!_context->varsFrameStack.empty());
        // The evaluated result of the $let is the evaluated result of its "in" field, which is
        // already on top of the stack. The "infix" visitor has already popped the variable
        // initializers off the expression stack.
        _context->ensureArity(1);

        // We should have bound all the variables from this $let expression.
        auto& currentFrame = _context->varsFrameStack.top();
        invariant(currentFrame.currentBindingIndex == currentFrame.bindings.size());

        auto resultExpr = _context->popABTExpr();
        std::vector<abt::ProjectionName> varNames;
        std::vector<abt::ABT> bindings;
        for (auto& binding : currentFrame.bindings) {
            varNames.emplace_back(makeLocalVariableName(binding.frameId, 0));
            bindings.emplace_back(unwrap(binding.expr.extractABT()));
        }

        pushABT(makeLet(std::move(varNames), std::move(bindings), std::move(resultExpr)));

        // Pop the lexical frame for this $let and remove all its bindings, which are now out of
        // scope.
        for (const auto& binding : currentFrame.bindings) {
            _context->environment.erase(binding.variableId);
        }
        _context->varsFrameStack.pop();
    }

    void visit(const ExpressionLn* expr) final {
        auto inputName = makeLocalVariableName(_context->state.frameIdGenerator->generate(), 0);

        auto lnExpr = buildABTMultiBranchConditionalFromCaseValuePairs(
            {ABTCaseValuePair{generateABTNullMissingOrUndefined(inputName), abt::Constant::null()},
             ABTCaseValuePair{
                 generateABTNonNumericCheck(inputName),
                 makeABTFail(ErrorCodes::Error{7157705}, "$ln only supports numeric types")},
             // Note: In MQL, $ln on a NumberDecimal NaN historically evaluates to a NumberDouble
             // NaN.
             ABTCaseValuePair{generateABTNaNCheck(inputName),
                              makeABTFunction("convert",
                                              makeVariable(inputName),
                                              abt::Constant::int32(static_cast<int32_t>(
                                                  sbe::value::TypeTags::NumberDouble)))},
             ABTCaseValuePair{generateABTNonPositiveCheck(inputName),
                              makeABTFail(ErrorCodes::Error{7157706},
                                          "$ln's argument must be a positive number")}},
            makeABTFunction("ln", makeVariable(inputName)));

        pushABT(
            abt::make<abt::Let>(std::move(inputName), _context->popABTExpr(), std::move(lnExpr)));
    }
    void visit(const ExpressionLog* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(const ExpressionLog10* expr) final {
        auto inputName = makeLocalVariableName(_context->state.frameIdGenerator->generate(), 0);

        auto log10Expr = buildABTMultiBranchConditionalFromCaseValuePairs(
            {ABTCaseValuePair{generateABTNullMissingOrUndefined(inputName), abt::Constant::null()},
             ABTCaseValuePair{
                 generateABTNonNumericCheck(inputName),
                 makeABTFail(ErrorCodes::Error{7157707}, "$log10 only supports numeric types")},
             // Note: In MQL, $log10 on a NumberDecimal NaN historically evaluates to a NumberDouble
             // NaN.
             ABTCaseValuePair{generateABTNaNCheck(inputName),
                              makeABTFunction("convert",
                                              makeVariable(inputName),
                                              abt::Constant::int32(static_cast<int32_t>(
                                                  sbe::value::TypeTags::NumberDouble)))},
             ABTCaseValuePair{generateABTNonPositiveCheck(inputName),
                              makeABTFail(ErrorCodes::Error{7157708},
                                          "$log10's argument must be a positive number")}},
            makeABTFunction("log10", makeVariable(inputName)));

        pushABT(abt::make<abt::Let>(
            std::move(inputName), _context->popABTExpr(), std::move(log10Expr)));
    }
    void visit(const ExpressionInternalFLEBetween* expr) final {
        unsupportedExpression("$_internalFleBetween");
    }
    void visit(const ExpressionInternalFLEEqual* expr) final {
        unsupportedExpression("$_internalFleEq");
    }
    void visit(const ExpressionEncStrStartsWith* expr) final {
        unsupportedExpression("$encStrStartsWith");
    }
    void visit(const ExpressionEncStrEndsWith* expr) final {
        unsupportedExpression("$encStrEndsWith");
    }
    void visit(const ExpressionEncStrContains* expr) final {
        unsupportedExpression("$encStrContains");
    }
    void visit(const ExpressionEncStrNormalizedEq* expr) final {
        unsupportedExpression("$encStrNormalizedEq");
    }

    void visit(const ExpressionInternalRawSortKey* expr) final {
        unsupportedExpression(ExpressionInternalRawSortKey::kName.data());
    }
    void visit(const ExpressionMap* expr) final {
        unsupportedExpression("$map");
    }
    void visit(const ExpressionMeta* expr) final {
        auto pushMetadataABT = [this](boost::optional<sbe::value::SlotId> slot, uint32_t typeMask) {
            if (slot) {
                pushABT(abt::make<abt::If>(
                    makeFillEmptyTrue(makeABTFunction(
                        "typeMatch"_sd, makeABTVariable(*slot), abt::Constant::int32(typeMask))),
                    makeABTVariable(*slot),
                    makeABTFail(ErrorCodes::Error{8107800}, "Unexpected metadata type")));
            } else {
                pushABT(abt::Constant::nothing());
            }
        };
        switch (expr->getMetaType()) {
            case DocumentMetadataFields::MetaType::kSearchScore:
                pushMetadataABT(_context->state.data->metadataSlots.searchScoreSlot,
                                getBSONTypeMask(BSONType::NumberDouble) |
                                    getBSONTypeMask(BSONType::NumberLong));
                break;
            case DocumentMetadataFields::MetaType::kSearchHighlights:
                pushMetadataABT(_context->state.data->metadataSlots.searchHighlightsSlot,
                                getBSONTypeMask(BSONType::Array));
                break;
            case DocumentMetadataFields::MetaType::kSearchScoreDetails:
                pushMetadataABT(_context->state.data->metadataSlots.searchDetailsSlot,
                                getBSONTypeMask(BSONType::Object));
                break;
            case DocumentMetadataFields::MetaType::kSearchSequenceToken:
                pushMetadataABT(_context->state.data->metadataSlots.searchSequenceToken,
                                getBSONTypeMask(BSONType::String));
                break;
            default:
                unsupportedExpression("$meta");
        }
    }
    void visit(const ExpressionMod* expr) final {
        auto rhs = _context->popABTExpr();
        auto lhs = _context->popABTExpr();
        auto lhsName = makeLocalVariableName(_context->state.frameIdGenerator->generate(), 0);
        auto rhsName = makeLocalVariableName(_context->state.frameIdGenerator->generate(), 0);

        auto modExpr = buildABTMultiBranchConditionalFromCaseValuePairs(
            {ABTCaseValuePair{abt::make<abt::BinaryOp>(abt::Operations::Or,
                                                       generateABTNullMissingOrUndefined(lhsName),
                                                       generateABTNullMissingOrUndefined(rhsName)),
                              abt::Constant::null()},
             ABTCaseValuePair{
                 abt::make<abt::BinaryOp>(abt::Operations::Or,
                                          generateABTNonNumericCheck(lhsName),
                                          generateABTNonNumericCheck(rhsName)),
                 makeABTFail(ErrorCodes::Error{7157718}, "$mod only supports numeric types")}},
            makeABTFunction("mod", makeVariable(lhsName), makeVariable(rhsName)));

        pushABT(makeLet({std::move(lhsName), std::move(rhsName)},
                        abt::makeSeq(std::move(lhs), std::move(rhs)),
                        std::move(modExpr)));
    }
    void visit(const ExpressionMultiply* expr) final {
        auto arity = expr->getChildren().size();
        _context->ensureArity(arity);

        if (arity < kArgumentCountForBinaryTree ||
            feature_flags::gFeatureFlagSbeUpgradeBinaryTrees.checkEnabled()) {
            visitFast(expr);
            return;
        }

        auto checkLeaf = [&](abt::ABT arg) {
            auto name = makeLocalVariableName(_context->state.frameId(), 0);
            auto var = makeVariable(name);
            auto checkedLeaf = buildABTMultiBranchConditional(
                ABTCaseValuePair{makeABTFunction("isNumber", var), var},
                makeABTFail(ErrorCodes::Error{7315403},
                            "only numbers are allowed in an $multiply expression"));
            return abt::make<abt::Let>(std::move(name), std::move(arg), std::move(checkedLeaf));
        };

        auto combineTwoTree = [&](abt::ABT left, abt::ABT right) {
            auto nameLeft = makeLocalVariableName(_context->state.frameId(), 0);
            auto nameRight = makeLocalVariableName(_context->state.frameId(), 0);
            auto varLeft = makeVariable(nameLeft);
            auto varRight = makeVariable(nameRight);

            auto mulExpr = buildABTMultiBranchConditional(
                ABTCaseValuePair{
                    abt::make<abt::BinaryOp>(abt::Operations::Or,
                                             generateABTNullMissingOrUndefined(nameLeft),
                                             generateABTNullMissingOrUndefined(nameRight)),
                    abt::Constant::null()},
                abt::make<abt::BinaryOp>(
                    abt::Operations::Mult, std::move(varLeft), std::move(varRight)));
            return makeLet({std::move(nameLeft), std::move(nameRight)},
                           abt::makeSeq(std::move(left), std::move(right)),
                           std::move(mulExpr));
        };

        abt::ABTVector leaves;
        leaves.reserve(arity);
        for (size_t idx = 0; idx < arity; ++idx) {
            leaves.emplace_back(checkLeaf(_context->popABTExpr()));
        }
        std::reverse(std::begin(leaves), std::end(leaves));

        pushABT(makeBalancedTree(combineTwoTree, std::move(leaves)));
    }
    void visitFast(const ExpressionMultiply* expr) {
        auto arity = expr->getChildren().size();
        _context->ensureArity(arity);

        // Return multiplicative identity if the $multiply expression has no operands.
        if (arity == 0) {
            pushABT(abt::Constant::int32(1));
            return;
        }

        abt::ABTVector binds;
        abt::ProjectionNameVector names;
        abt::ABTVector checkExprsNull;
        abt::ABTVector checkExprsNumber;
        abt::ABTVector variables;
        binds.reserve(arity);
        names.reserve(arity);
        variables.reserve(arity);
        checkExprsNull.reserve(arity);
        checkExprsNumber.reserve(arity);
        for (size_t idx = 0; idx < arity; ++idx) {
            binds.push_back(_context->popABTExpr());
            auto currentName =
                makeLocalVariableName(_context->state.frameIdGenerator->generate(), 0);
            names.push_back(currentName);

            checkExprsNull.push_back(generateABTNullMissingOrUndefined(currentName));
            checkExprsNumber.push_back(makeABTFunction("isNumber", makeVariable(currentName)));
            variables.push_back(makeVariable(currentName));
        }

        // At this point 'binds' vector contains arguments of $multiply expression in the reversed
        // order. We need to reverse it back to perform multiplication in the right order below.
        // Multiplication in different order can lead to different result because of accumulated
        // precision errors from floating point types.
        std::reverse(std::begin(binds), std::end(binds));

        auto checkNullAnyArgument =
            makeBooleanOpTree(abt::Operations::Or, std::move(checkExprsNull));
        auto checkNumberAllArguments =
            makeBooleanOpTree(abt::Operations::And, std::move(checkExprsNumber));
        auto multiplication = makeNaryOp(abt::Operations::Mult, std::move(variables));

        auto multiplyExpr = buildABTMultiBranchConditionalFromCaseValuePairs(
            {ABTCaseValuePair{std::move(checkNullAnyArgument), abt::Constant::null()},
             ABTCaseValuePair{std::move(checkNumberAllArguments), std::move(multiplication)}},
            makeABTFail(ErrorCodes::Error{7157721},
                        "only numbers are allowed in an $multiply expression"));

        multiplyExpr = makeLet(std::move(names), std::move(binds), std::move(multiplyExpr));
        pushABT(std::move(multiplyExpr));
    }
    void visit(const ExpressionNot* expr) final {
        pushABT(
            makeNot(makeFillEmptyFalse(makeABTFunction("coerceToBool", _context->popABTExpr()))));
    }
    void visit(const ExpressionObject* expr) final {
        const auto& childExprs = expr->getChildExpressions();
        size_t childSize = childExprs.size();
        _context->ensureArity(childSize);

        // The expression argument for 'newObj' must be a sequence of a field name constant
        // expression and an expression for the value. So, we need 2 * childExprs.size() elements in
        // the expressions vector.
        abt::ABTVector exprs;
        exprs.reserve(childSize * 2);

        // We iterate over child expressions in reverse, because they will be popped from stack in
        // reverse order.
        for (auto rit = childExprs.rbegin(); rit != childExprs.rend(); ++rit) {
            exprs.push_back(_context->popABTExpr());
            exprs.push_back(abt::Constant::str(rit->first));
        }

        // Lastly we need to reverse it to get the correct order of arguments.
        std::reverse(exprs.begin(), exprs.end());

        pushABT(abt::make<abt::FunctionCall>("newObj", std::move(exprs)));
    }

    void visit(const ExpressionOr* expr) final {
        visitMultiBranchLogicExpression(expr, abt::Operations::Or);
    }
    void visit(const ExpressionPow* expr) final {
        _context->ensureArity(2);
        auto rhs = _context->popABTExpr();
        auto lhs = _context->popABTExpr();

        auto lhsName = makeLocalVariableName(_context->state.frameId(), 0);
        auto rhsName = makeLocalVariableName(_context->state.frameId(), 0);

        auto checkIsNotNumber = abt::make<abt::BinaryOp>(abt::Operations::Or,
                                                         generateABTNonNumericCheck(lhsName),
                                                         generateABTNonNumericCheck(rhsName));

        auto checkBaseIsZero = abt::make<abt::BinaryOp>(
            abt::Operations::Eq, makeVariable(lhsName), abt::Constant::int32(0));

        auto checkIsZeroAndNegative = abt::make<abt::BinaryOp>(
            abt::Operations::And, std::move(checkBaseIsZero), generateABTNegativeCheck(rhsName));

        auto checkIsNullOrMissing =
            abt::make<abt::BinaryOp>(abt::Operations::Or,
                                     generateABTNullMissingOrUndefined(lhsName),
                                     generateABTNullMissingOrUndefined(rhsName));

        // Create an expression to invoke built-in "pow" function
        auto powFunctionCall = makeABTFunction("pow", makeVariable(lhsName), makeVariable(rhsName));
        // Local bind to hold the result of the built-in "pow" function
        auto powResName = makeLocalVariableName(_context->state.frameId(), 0);
        auto powResVariable = makeVariable(powResName);

        // Return the result or check for issues if result is empty (Nothing)
        auto checkPowRes = abt::make<abt::BinaryOp>(
            abt::Operations::FillEmpty,
            std::move(powResVariable),
            buildABTMultiBranchConditionalFromCaseValuePairs(
                {ABTCaseValuePair{std::move(checkIsNullOrMissing), abt::Constant::null()},
                 ABTCaseValuePair{
                     std::move(checkIsNotNumber),
                     makeABTFail(ErrorCodes::Error{5154200}, "$pow only supports numeric types")},
                 ABTCaseValuePair{std::move(checkIsZeroAndNegative),
                                  makeABTFail(ErrorCodes::Error{5154201},
                                              "$pow cannot raise 0 to a negative exponent")}},
                abt::Constant::nothing()));


        pushABT(makeLet({std::move(lhsName), std::move(rhsName), std::move(powResName)},
                        abt::makeSeq(std::move(lhs), std::move(rhs), std::move(powFunctionCall)),
                        std::move(checkPowRes)));
    }
    void visit(const ExpressionRange* expr) final {
        auto startName = makeLocalVariableName(_context->state.frameIdGenerator->generate(), 0);
        auto endName = makeLocalVariableName(_context->state.frameIdGenerator->generate(), 0);
        auto stepName = makeLocalVariableName(_context->state.frameIdGenerator->generate(), 0);

        auto convertedStartName =
            makeLocalVariableName(_context->state.frameIdGenerator->generate(), 0);
        auto convertedEndName =
            makeLocalVariableName(_context->state.frameIdGenerator->generate(), 0);
        auto convertedStepName =
            makeLocalVariableName(_context->state.frameIdGenerator->generate(), 0);

        auto step =
            expr->getChildren().size() == 3 ? _context->popABTExpr() : abt::Constant::int32(1);
        auto end = _context->popABTExpr();
        auto start = _context->popABTExpr();

        auto rangeExpr = makeLet(
            {startName, endName, stepName},
            abt::makeSeq(std::move(start), std::move(end), std::move(step)),
            buildABTMultiBranchConditionalFromCaseValuePairs(
                {ABTCaseValuePair{generateABTNonNumericCheck(startName),
                                  makeABTFail(ErrorCodes::Error{7157711},
                                              "$range only supports numeric types for start")},
                 ABTCaseValuePair{generateABTNonNumericCheck(endName),
                                  makeABTFail(ErrorCodes::Error{7157712},
                                              "$range only supports numeric types for end")},
                 ABTCaseValuePair{generateABTNonNumericCheck(stepName),
                                  makeABTFail(ErrorCodes::Error{7157713},
                                              "$range only supports numeric types for step")}},
                makeLet(
                    {convertedStartName, convertedEndName, convertedStepName},
                    abt::makeSeq(makeABTFunction("convert",
                                                 makeVariable(startName),
                                                 abt::Constant::int32(static_cast<int32_t>(
                                                     sbe::value::TypeTags::NumberInt32))),
                                 makeABTFunction("convert",
                                                 makeVariable(endName),
                                                 abt::Constant::int32(static_cast<int32_t>(
                                                     sbe::value::TypeTags::NumberInt32))),
                                 makeABTFunction("convert",
                                                 makeVariable(stepName),
                                                 abt::Constant::int32(static_cast<int32_t>(
                                                     sbe::value::TypeTags::NumberInt32)))),
                    buildABTMultiBranchConditionalFromCaseValuePairs(
                        {ABTCaseValuePair{
                             makeNot(makeABTFunction("exists", makeVariable(convertedStartName))),
                             makeABTFail(ErrorCodes::Error{7157714},
                                         "$range start argument cannot be "
                                         "represented as a 32-bit integer")},
                         ABTCaseValuePair{
                             makeNot(makeABTFunction("exists", makeVariable(convertedEndName))),
                             makeABTFail(ErrorCodes::Error{7157715},
                                         "$range end argument cannot be represented "
                                         "as a 32-bit integer")},
                         ABTCaseValuePair{
                             makeNot(makeABTFunction("exists", makeVariable(convertedStepName))),
                             makeABTFail(ErrorCodes::Error{7157716},
                                         "$range step argument cannot be "
                                         "represented as a 32-bit integer")},
                         ABTCaseValuePair{abt::make<abt::BinaryOp>(abt::Operations::Eq,
                                                                   makeVariable(convertedStepName),
                                                                   abt::Constant::int32(0)),
                                          makeABTFail(ErrorCodes::Error{7157717},
                                                      "$range requires a non-zero step value")}},
                        makeABTFunction("newArrayFromRange",
                                        makeVariable(convertedStartName),
                                        makeVariable(convertedEndName),
                                        makeVariable(convertedStepName))))));

        pushABT(std::move(rangeExpr));
    }

    void visit(const ExpressionReduce* expr) final {
        unsupportedExpression("$reduce");
    }
    void visit(const ExpressionReplaceOne* expr) final {
        _context->ensureArity(3);

        auto replacementArg = _context->popABTExpr();
        auto findArg = _context->popABTExpr();
        auto inputArg = _context->popABTExpr();

        auto inputArgName = makeLocalVariableName(_context->state.frameId(), 0);
        auto findArgName = makeLocalVariableName(_context->state.frameId(), 0);
        auto replacementArgName = makeLocalVariableName(_context->state.frameId(), 0);

        auto inputArgNullName = makeLocalVariableName(_context->state.frameId(), 0);
        auto findArgNullName = makeLocalVariableName(_context->state.frameId(), 0);
        auto replacementArgNullName = makeLocalVariableName(_context->state.frameId(), 0);

        auto checkNull = abt::make<abt::BinaryOp>(
            abt::Operations::Or,
            abt::make<abt::BinaryOp>(
                abt::Operations::Or, makeVariable(inputArgNullName), makeVariable(findArgNullName)),
            makeVariable(replacementArgNullName));

        // Check if find string is empty, and if so return the the concatenation of the replacement
        // string and the input string, otherwise replace the first occurrence of the find string.
        auto isEmptyFindStr = abt::make<abt::BinaryOp>(
            abt::Operations::Eq, makeVariable(findArgName), abt::Constant::str(""_sd));

        auto generateTypeCheckCaseValuePair = [](abt::ProjectionName paramName,
                                                 abt::ProjectionName paramIsNullName,
                                                 StringData param) {
            return ABTCaseValuePair{
                makeNot(abt::make<abt::BinaryOp>(
                    abt::Operations::Or,
                    makeVariable(std::move(paramIsNullName)),
                    makeABTFunction("isString", makeVariable(std::move(paramName))))),
                makeABTFail(ErrorCodes::Error{7158302},
                            str::stream()
                                << "$replaceOne requires that '" << param << "' be a string")};
        };

        // Order here is important because we want to preserve the precedence of failures in MQL.
        auto replaceOneExpr = buildABTMultiBranchConditionalFromCaseValuePairs(
            {generateTypeCheckCaseValuePair(inputArgName, inputArgNullName, "input"),
             generateTypeCheckCaseValuePair(findArgName, findArgNullName, "find"),
             generateTypeCheckCaseValuePair(
                 replacementArgName, replacementArgNullName, "replacement"),
             ABTCaseValuePair{std::move(checkNull), abt::Constant::null()}},
            abt::make<abt::If>(std::move(isEmptyFindStr),
                               makeABTFunction("concat",
                                               makeVariable(replacementArgName),
                                               makeVariable(inputArgName)),
                               makeABTFunction("replaceOne",
                                               makeVariable(inputArgName),
                                               makeVariable(findArgName),
                                               makeVariable(replacementArgName))));

        pushABT(makeLet({replacementArgName,
                         findArgName,
                         inputArgName,
                         replacementArgNullName,
                         findArgNullName,
                         inputArgNullName},
                        abt::makeSeq(std::move(replacementArg),
                                     std::move(findArg),
                                     std::move(inputArg),
                                     generateABTNullMissingOrUndefined(replacementArgName),
                                     generateABTNullMissingOrUndefined(findArgName),
                                     generateABTNullMissingOrUndefined(inputArgName)),
                        std::move(replaceOneExpr)));
    }

    void visit(const ExpressionReplaceAll* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(const ExpressionSetDifference* expr) final {
        invariant(expr->getChildren().size() == 2);

        generateSetExpression(expr, SetOperation::Difference);
    }
    void visit(const ExpressionSetEquals* expr) final {
        invariant(expr->getChildren().size() >= 2);

        generateSetExpression(expr, SetOperation::Equals);
    }
    void visit(const ExpressionSetIntersection* expr) final {
        if (expr->getChildren().size() == 0) {
            auto [emptySetTag, emptySetValue] = sbe::value::makeNewArraySet();
            pushABT(makeABTConstant(emptySetTag, emptySetValue));
            return;
        }

        generateSetExpression(expr, SetOperation::Intersection);
    }
    void visit(const ExpressionSetIsSubset* expr) final {
        tassert(5154700,
                "$setIsSubset expects two expressions in the input",
                expr->getChildren().size() == 2);

        generateSetExpression(expr, SetOperation::IsSubset);
    }
    void visit(const ExpressionSetUnion* expr) final {
        if (expr->getChildren().size() == 0) {
            auto [emptySetTag, emptySetValue] = sbe::value::makeNewArraySet();
            pushABT(makeABTConstant(emptySetTag, emptySetValue));
            return;
        }

        generateSetExpression(expr, SetOperation::Union);
    }

    void visit(const ExpressionSize* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(const ExpressionReverseArray* expr) final {
        auto frameId = _context->state.frameId();
        auto arg = _context->popABTExpr();
        auto name = makeLocalVariableName(frameId, 0);
        auto var = makeVariable(name);

        auto argumentIsNotArray = makeNot(makeABTFunction("isArray", var));

        auto exprReverseArr = buildABTMultiBranchConditionalFromCaseValuePairs(
            {ABTCaseValuePair{generateABTNullMissingOrUndefined(name), abt::Constant::null()},
             ABTCaseValuePair{std::move(argumentIsNotArray),
                              makeABTFail(ErrorCodes::Error{7158002},
                                          "$reverseArray argument must be an array")}},
            makeABTFunction("reverseArray", std::move(var)));

        pushABT(abt::make<abt::Let>(std::move(name), std::move(arg), std::move(exprReverseArr)));
    }

    void visit(const ExpressionSortArray* expr) final {
        auto frameId = _context->state.frameId();
        auto arg = _context->popABTExpr();
        auto name = makeLocalVariableName(frameId, 0);
        auto var = makeVariable(name);

        auto [specTag, specVal] = makeValue(expr->getSortPattern());
        auto specConstant = makeABTConstant(specTag, specVal);

        auto argumentIsNotArray = makeNot(makeABTFunction("isArray", var));

        abt::ABTVector functionArgs{std::move(var), std::move(specConstant)};

        auto collatorSlot = _context->state.getCollatorSlot();
        if (collatorSlot) {
            functionArgs.emplace_back(makeABTVariable(*collatorSlot));
        }

        auto exprSortArr = buildABTMultiBranchConditionalFromCaseValuePairs(
            {ABTCaseValuePair{generateABTNullMissingOrUndefined(name), abt::Constant::null()},
             ABTCaseValuePair{std::move(argumentIsNotArray),
                              makeABTFail(ErrorCodes::Error{7158001},
                                          "$sortArray input argument must be an array")}},
            abt::make<abt::FunctionCall>("sortArray", std::move(functionArgs)));

        pushABT(abt::make<abt::Let>(std::move(name), std::move(arg), std::move(exprSortArr)));
    }

    void visit(const ExpressionSlice* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(const ExpressionIsArray* expr) final {
        pushABT(makeFillEmptyFalse(makeABTFunction("isArray", _context->popABTExpr())));
    }
    void visit(const ExpressionInternalFindAllValuesAtPath* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(const ExpressionRound* expr) final {
        visitRoundTruncExpression(expr);
    }
    void visit(const ExpressionSplit* expr) final {
        invariant(expr->getChildren().size() == 2);
        _context->ensureArity(2);

        auto [arrayWithEmptyStringTag, arrayWithEmptyStringVal] = sbe::value::makeNewArray();
        sbe::value::ValueGuard arrayWithEmptyStringGuard{arrayWithEmptyStringTag,
                                                         arrayWithEmptyStringVal};
        auto [emptyStrTag, emptyStrVal] = sbe::value::makeNewString("");
        sbe::value::getArrayView(arrayWithEmptyStringVal)->push_back(emptyStrTag, emptyStrVal);

        auto delimiter = _context->popABTExpr();
        auto stringExpression = _context->popABTExpr();

        auto varString = makeLocalVariableName(_context->state.frameId(), 0);
        auto varDelimiter = makeLocalVariableName(_context->state.frameId(), 0);
        auto emptyResult = makeABTConstant(arrayWithEmptyStringTag, arrayWithEmptyStringVal);
        arrayWithEmptyStringGuard.reset();

        // In order to maintain MQL semantics, first check both the string expression
        // (first agument), and delimiter string (second argument) for null, undefined, or
        // missing, and if either is nullish make the entire expression return null. Only
        // then make further validity checks against the input. Fail if the delimiter is an
        // empty string. Return [""] if the string expression is an empty string.
        auto totalSplitFunc = buildABTMultiBranchConditionalFromCaseValuePairs(
            {ABTCaseValuePair{
                 abt::make<abt::BinaryOp>(abt::Operations::Or,
                                          generateABTNullMissingOrUndefined(varString),
                                          generateABTNullMissingOrUndefined(varDelimiter)),
                 abt::Constant::null()},
             ABTCaseValuePair{makeNot(makeABTFunction("isString"_sd, makeVariable(varString))),
                              makeABTFail(ErrorCodes::Error{7158202},
                                          "$split string expression must be a string")},
             ABTCaseValuePair{
                 makeNot(makeABTFunction("isString"_sd, makeVariable(varDelimiter))),
                 makeABTFail(ErrorCodes::Error{7158203}, "$split delimiter must be a string")},
             ABTCaseValuePair{abt::make<abt::BinaryOp>(abt::Operations::Eq,
                                                       makeVariable(varDelimiter),
                                                       makeABTConstant(""_sd)),
                              makeABTFail(ErrorCodes::Error{7158204},
                                          "$split delimiter must not be an empty string")},
             ABTCaseValuePair{abt::make<abt::BinaryOp>(abt::Operations::Eq,
                                                       makeVariable(varString),
                                                       makeABTConstant(""_sd)),
                              std::move(emptyResult)}},
            makeABTFunction("split"_sd, makeVariable(varString), makeVariable(varDelimiter)));

        pushABT(makeLet({varString, varDelimiter},
                        abt::makeSeq(std::move(stringExpression), std::move(delimiter)),
                        std::move(totalSplitFunc)));
    }
    void visit(const ExpressionSqrt* expr) final {
        auto inputName = makeLocalVariableName(_context->state.frameIdGenerator->generate(), 0);

        auto sqrtExpr = buildABTMultiBranchConditionalFromCaseValuePairs(
            {ABTCaseValuePair{generateABTNullMissingOrUndefined(inputName), abt::Constant::null()},
             ABTCaseValuePair{
                 generateABTNonNumericCheck(inputName),
                 makeABTFail(ErrorCodes::Error{7157709}, "$sqrt only supports numeric types")},
             ABTCaseValuePair{generateABTNegativeCheck(inputName),
                              makeABTFail(ErrorCodes::Error{7157710},
                                          "$sqrt's argument must be greater than or equal to 0")}},
            makeABTFunction("sqrt", makeVariable(inputName)));

        pushABT(
            abt::make<abt::Let>(std::move(inputName), _context->popABTExpr(), std::move(sqrtExpr)));
    }
    void visit(const ExpressionStrcasecmp* expr) final {
        invariant(expr->getChildren().size() == 2);
        _context->ensureArity(2);

        SbExprBuilder b(_context->state);

        generateStringCaseConversionExpression(_context, "toUpper");
        SbExpr rhs = _context->popExpr();
        generateStringCaseConversionExpression(_context, "toUpper");
        SbExpr lhs = _context->popExpr();

        _context->pushExpr(b.makeBinaryOp(abt::Operations::Cmp3w, std::move(lhs), std::move(rhs)));
    }
    void visit(const ExpressionSubstrBytes* expr) final {
        invariant(expr->getChildren().size() == 3);
        _context->ensureArity(3);

        abt::ABT byteCount = _context->popABTExpr();
        abt::ABT startIndex = _context->popABTExpr();
        abt::ABT stringExpr = _context->popABTExpr();

        abt::ProjectionName stringExprName = makeLocalVariableName(_context->state.frameId(), 0);
        abt::ProjectionName startIndexName = makeLocalVariableName(_context->state.frameId(), 0);
        abt::ProjectionName byteCountName = makeLocalVariableName(_context->state.frameId(), 0);

        abt::ABTVector functionArgs;

        abt::ABT validStringExpr = buildABTMultiBranchConditionalFromCaseValuePairs(
            {ABTCaseValuePair{generateABTNullMissingOrUndefined(stringExprName),
                              makeABTConstant(""_sd)},
             ABTCaseValuePair{
                 makeFillEmptyTrue(makeABTFunction("coerceToString", makeVariable(stringExprName))),
                 makeABTFail(ErrorCodes::Error(5155608),
                             "$substrBytes: string expression could not be resolved to a string")}},
            makeABTFunction("coerceToString", makeVariable(stringExprName)));
        functionArgs.push_back(std::move(validStringExpr));

        abt::ABT validStartIndexExpr = abt::make<abt::If>(
            abt::make<abt::BinaryOp>(
                abt::Operations::Or,
                generateABTNullMissingOrUndefined(startIndexName),
                abt::make<abt::BinaryOp>(abt::Operations::Or,
                                         generateABTNonNumericCheck(startIndexName),
                                         abt::make<abt::BinaryOp>(abt::Operations::Lt,
                                                                  makeVariable(startIndexName),
                                                                  abt::Constant::int32(0)))),
            makeABTFail(ErrorCodes::Error{5155603},
                        "Starting index must be non-negative numeric type"),
            makeABTFunction(
                "convert",
                makeVariable(startIndexName),
                abt::Constant::int32(static_cast<int32_t>(sbe::value::TypeTags::NumberInt64))));
        functionArgs.push_back(std::move(validStartIndexExpr));

        abt::ABT validLengthExpr = abt::make<abt::If>(
            abt::make<abt::BinaryOp>(abt::Operations::Or,
                                     generateABTNullMissingOrUndefined(byteCountName),
                                     generateABTNonNumericCheck(byteCountName)),
            makeABTFail(ErrorCodes::Error{5155602}, "Length must be a numeric type"),
            makeABTFunction(
                "convert",
                makeVariable(byteCountName),
                abt::Constant::int32(static_cast<int32_t>(sbe::value::TypeTags::NumberInt64))));
        functionArgs.push_back(std::move(validLengthExpr));

        pushABT(makeLet(
            {std::move(byteCountName), std::move(startIndexName), std::move(stringExprName)},
            abt::makeSeq(std::move(byteCount), std::move(startIndex), std::move(stringExpr)),
            abt::make<abt::FunctionCall>("substrBytes", std::move(functionArgs))));
    }
    void visit(const ExpressionSubstrCP* expr) final {
        invariant(expr->getChildren().size() == 3);
        _context->ensureArity(3);

        abt::ABT len = _context->popABTExpr();
        abt::ABT startIndex = _context->popABTExpr();
        abt::ABT stringExpr = _context->popABTExpr();

        abt::ProjectionName stringExprName = makeLocalVariableName(_context->state.frameId(), 0);
        abt::ProjectionName startIndexName = makeLocalVariableName(_context->state.frameId(), 0);
        abt::ProjectionName lenName = makeLocalVariableName(_context->state.frameId(), 0);

        abt::ABTVector functionArgs;

        abt::ABT validStringExpr = buildABTMultiBranchConditionalFromCaseValuePairs(
            {ABTCaseValuePair{generateABTNullMissingOrUndefined(stringExprName),
                              makeABTConstant(""_sd)},
             ABTCaseValuePair{
                 makeFillEmptyTrue(makeABTFunction("coerceToString", makeVariable(stringExprName))),
                 makeABTFail(ErrorCodes::Error(5155708),
                             "$substrCP: string expression could not be resolved to a "
                             "string")}},
            makeABTFunction("coerceToString", makeVariable(stringExprName)));
        functionArgs.push_back(std::move(validStringExpr));

        abt::ABT validStartIndexExpr = buildABTMultiBranchConditionalFromCaseValuePairs(
            {ABTCaseValuePair{
                 generateABTNullishOrNotRepresentableInt32Check(startIndexName),
                 makeABTFail(ErrorCodes::Error{5155700},
                             "$substrCP: starting index must be numeric type representable as a "
                             "32-bit integral value")},
             ABTCaseValuePair{
                 abt::make<abt::BinaryOp>(
                     abt::Operations::Lt, makeVariable(startIndexName), abt::Constant::int32(0)),
                 makeABTFail(ErrorCodes::Error{5155701},
                             "$substrCP: starting index must be a non-negative integer")}},
            makeABTFunction(
                "convert",
                makeVariable(startIndexName),
                abt::Constant::int32(static_cast<int32_t>(sbe::value::TypeTags::NumberInt32))));
        functionArgs.push_back(std::move(validStartIndexExpr));

        abt::ABT validLengthExpr = buildABTMultiBranchConditionalFromCaseValuePairs(
            {ABTCaseValuePair{generateABTNullishOrNotRepresentableInt32Check(lenName),
                              makeABTFail(ErrorCodes::Error{5155702},
                                          "$substrCP: length must be numeric type representable as "
                                          "a 32-bit integral value")},
             ABTCaseValuePair{abt::make<abt::BinaryOp>(abt::Operations::Lt,
                                                       makeVariable(lenName),
                                                       abt::Constant::int32(0)),
                              makeABTFail(ErrorCodes::Error{5155703},
                                          "$substrCP: length must be a non-negative integer")}},
            makeABTFunction(
                "convert",
                makeVariable(lenName),
                abt::Constant::int32(static_cast<int32_t>(sbe::value::TypeTags::NumberInt32))));
        functionArgs.push_back(std::move(validLengthExpr));

        pushABT(makeLet({std::move(lenName), std::move(startIndexName), std::move(stringExprName)},
                        abt::makeSeq(std::move(len), std::move(startIndex), std::move(stringExpr)),
                        abt::make<abt::FunctionCall>("substrCP", std::move(functionArgs))));
    }
    void visit(const ExpressionStrLenBytes* expr) final {
        tassert(5155802, "expected 'expr' to have 1 child", expr->getChildren().size() == 1);
        _context->ensureArity(1);

        abt::ProjectionName strName = makeLocalVariableName(_context->state.frameId(), 0);
        abt::ABT strExpression = _context->popABTExpr();
        abt::ABT strVar = makeVariable(strName);

        auto strLenBytesExpr = abt::make<abt::If>(
            makeFillEmptyFalse(makeABTFunction("isString", strVar)),
            makeABTFunction("strLenBytes", strVar),
            makeABTFail(ErrorCodes::Error{5155800}, "$strLenBytes requires a string argument"));

        pushABT(abt::make<abt::Let>(
            std::move(strName), std::move(strExpression), std::move(strLenBytesExpr)));
    }
    void visit(const ExpressionBinarySize* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(const ExpressionStrLenCP* expr) final {
        tassert(5155902, "expected 'expr' to have 1 child", expr->getChildren().size() == 1);
        _context->ensureArity(1);

        abt::ProjectionName strName = makeLocalVariableName(_context->state.frameId(), 0);
        abt::ABT strExpression = _context->popABTExpr();
        abt::ABT strVar = makeVariable(strName);

        auto strLenCPExpr = abt::make<abt::If>(
            makeFillEmptyFalse(makeABTFunction("isString", strVar)),
            makeABTFunction("strLenCP", strVar),
            makeABTFail(ErrorCodes::Error{5155900}, "$strLenCP requires a string argument"));

        pushABT(abt::make<abt::Let>(
            std::move(strName), std::move(strExpression), std::move(strLenCPExpr)));
    }
    void visit(const ExpressionSubtract* expr) final {
        invariant(expr->getChildren().size() == 2);
        _context->ensureArity(2);

        auto rhs = _context->popABTExpr();
        auto lhs = _context->popABTExpr();

        auto lhsName = makeLocalVariableName(_context->state.frameIdGenerator->generate(), 0);
        auto rhsName = makeLocalVariableName(_context->state.frameIdGenerator->generate(), 0);

        auto checkNullArguments =
            abt::make<abt::BinaryOp>(abt::Operations::Or,
                                     generateABTNullMissingOrUndefined(lhsName),
                                     generateABTNullMissingOrUndefined(rhsName));

        auto checkArgumentTypes = makeNot(abt::make<abt::If>(
            makeABTFunction("isNumber", makeVariable(lhsName)),
            makeABTFunction("isNumber", makeVariable(rhsName)),
            abt::make<abt::BinaryOp>(
                abt::Operations::And,
                makeABTFunction("isDate", makeVariable(lhsName)),
                abt::make<abt::BinaryOp>(abt::Operations::Or,
                                         makeABTFunction("isNumber", makeVariable(rhsName)),
                                         makeABTFunction("isDate", makeVariable(rhsName))))));

        auto subtractOp = abt::make<abt::BinaryOp>(
            abt::Operations::Sub, makeVariable(lhsName), makeVariable(rhsName));
        auto subtractExpr = buildABTMultiBranchConditionalFromCaseValuePairs(
            {ABTCaseValuePair{std::move(checkNullArguments), abt::Constant::null()},
             ABTCaseValuePair{
                 std::move(checkArgumentTypes),
                 makeABTFail(
                     ErrorCodes::Error{7157720},
                     "Only numbers and dates are allowed in an $subtract expression. To "
                     "subtract a number from a date, the date must be the first argument.")}},
            std::move(subtractOp));

        pushABT(makeLet({std::move(lhsName), std::move(rhsName)},
                        abt::makeSeq(std::move(lhs), std::move(rhs)),
                        std::move(subtractExpr)));
    }
    void visit(const ExpressionSwitch* expr) final {
        visitConditionalExpression(expr);
    }
    void visit(const ExpressionTestApiVersion* expr) final {
        pushABT(abt::Constant::int32(1));
    }
    void visit(const ExpressionToLower* expr) final {
        generateStringCaseConversionExpression(_context, "toLower");
    }
    void visit(const ExpressionToUpper* expr) final {
        generateStringCaseConversionExpression(_context, "toUpper");
    }
    void visit(const ExpressionTrim* expr) final {
        tassert(5156301,
                "trim expressions must have spots in their children vector for 'input' and "
                "'chars' fields",
                expr->getChildren().size() == 2);
        auto numProvidedArgs = 1;
        if (expr->hasCharactersExpr())  // 'chars' is not null
            ++numProvidedArgs;
        _context->ensureArity(numProvidedArgs);
        auto isCharsProvided = numProvidedArgs == 2;

        auto inputName = makeLocalVariableName(_context->state.frameId(), 0);
        auto charsName = makeLocalVariableName(_context->state.frameId(), 0);

        auto charsString = isCharsProvided ? _context->popABTExpr() : abt::Constant::null();
        auto inputString = _context->popABTExpr();
        auto trimBuiltinName = expr->getTrimTypeString();

        auto checkCharsNullish = isCharsProvided ? generateABTNullMissingOrUndefined(charsName)
                                                 : abt::Constant::boolean(false);

        auto checkCharsNotString = isCharsProvided
            ? makeNot(makeABTFunction("isString"_sd, makeVariable(charsName)))
            : abt::Constant::boolean(false);


        /*
           Trim Functionality (invariant that 'input' has been provided, otherwise would've failed
           at parse time)

           if ('input' is nullish) {
                -> return null
           }
           else if ('input' is not a string) {
                ->  fail with error code 5156302
           }
           else if ('chars' is provided and nullish) {
                -> return null
           }
           else if ('chars' is provided but is not a string) {
                ->  fail with error code 5156303
           }
           else {
                -> make an ABT function for the correct $trim variant with 'input' and 'chars'
                   parameters
           }
        */
        auto trimFunc = buildABTMultiBranchConditionalFromCaseValuePairs(
            {ABTCaseValuePair{generateABTNullMissingOrUndefined(inputName), abt::Constant::null()},
             ABTCaseValuePair{
                 makeNot(makeABTFunction("isString"_sd, makeVariable(inputName))),
                 makeABTFail(ErrorCodes::Error{5156302},
                             "$" + trimBuiltinName + " input expression must be a string")},
             ABTCaseValuePair{std::move(checkCharsNullish), abt::Constant::null()},
             ABTCaseValuePair{std::move(checkCharsNotString),
                              makeABTFail(ErrorCodes::Error{5156303},
                                          "$" + trimBuiltinName +
                                              " chars expression must be a string if provided")}},
            makeABTFunction(trimBuiltinName, makeVariable(inputName), makeVariable(charsName)));

        pushABT(makeLet({std::move(inputName), std::move(charsName)},
                        abt::makeSeq(std::move(inputString), std::move(charsString)),
                        std::move(trimFunc)));
    }
    void visit(const ExpressionTrunc* expr) final {
        visitRoundTruncExpression(expr);
    }
    void visit(const ExpressionType* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(const ExpressionZip* expr) final {
        unsupportedExpression("$zip");
    }
    void visit(const ExpressionConvert* expr) final {
        unsupportedExpression("$convert");
    }
    void visit(const ExpressionRegexFind* expr) final {
        generateRegexExpression(expr, "regexFind");
    }
    void visit(const ExpressionRegexFindAll* expr) final {
        generateRegexExpression(expr, "regexFindAll");
    }
    void visit(const ExpressionRegexMatch* expr) final {
        generateRegexExpression(expr, "regexMatch");
    }
    void visit(const ExpressionCosine* expr) final {
        generateTrigonometricExpressionWithBounds(
            "cos", DoubleBound::minInfinity(), DoubleBound::plusInfinity());
    }
    void visit(const ExpressionSine* expr) final {
        generateTrigonometricExpressionWithBounds(
            "sin", DoubleBound::minInfinity(), DoubleBound::plusInfinity());
    }
    void visit(const ExpressionTangent* expr) final {
        generateTrigonometricExpressionWithBounds(
            "tan", DoubleBound::minInfinity(), DoubleBound::plusInfinity());
    }
    void visit(const ExpressionArcCosine* expr) final {
        generateTrigonometricExpressionWithBounds(
            "acos", DoubleBound(-1.0, true), DoubleBound(1.0, true));
    }
    void visit(const ExpressionArcSine* expr) final {
        generateTrigonometricExpressionWithBounds(
            "asin", DoubleBound(-1.0, true), DoubleBound(1.0, true));
    }
    void visit(const ExpressionArcTangent* expr) final {
        generateTrigonometricExpression("atan");
    }
    void visit(const ExpressionArcTangent2* expr) final {
        generateTrigonometricExpressionBinary("atan2");
    }
    void visit(const ExpressionHyperbolicArcTangent* expr) final {
        generateTrigonometricExpressionWithBounds(
            "atanh", DoubleBound(-1.0, true), DoubleBound(1.0, true));
    }
    void visit(const ExpressionHyperbolicArcCosine* expr) final {
        generateTrigonometricExpressionWithBounds(
            "acosh", DoubleBound(1.0, true), DoubleBound::plusInfinityInclusive());
    }
    void visit(const ExpressionHyperbolicArcSine* expr) final {
        generateTrigonometricExpression("asinh");
    }
    void visit(const ExpressionHyperbolicCosine* expr) final {
        generateTrigonometricExpression("cosh");
    }
    void visit(const ExpressionHyperbolicSine* expr) final {
        generateTrigonometricExpression("sinh");
    }
    void visit(const ExpressionHyperbolicTangent* expr) final {
        generateTrigonometricExpression("tanh");
    }
    void visit(const ExpressionDegreesToRadians* expr) final {
        generateTrigonometricExpression("degreesToRadians");
    }
    void visit(const ExpressionRadiansToDegrees* expr) final {
        generateTrigonometricExpression("radiansToDegrees");
    }
    void visit(const ExpressionDayOfMonth* expr) final {
        generateDateExpressionAcceptingTimeZone("dayOfMonth", expr);
    }
    void visit(const ExpressionDayOfWeek* expr) final {
        generateDateExpressionAcceptingTimeZone("dayOfWeek", expr);
    }
    void visit(const ExpressionDayOfYear* expr) final {
        generateDateExpressionAcceptingTimeZone("dayOfYear", expr);
    }
    void visit(const ExpressionHour* expr) final {
        generateDateExpressionAcceptingTimeZone("hour", expr);
    }
    void visit(const ExpressionMillisecond* expr) final {
        generateDateExpressionAcceptingTimeZone("millisecond", expr);
    }
    void visit(const ExpressionMinute* expr) final {
        generateDateExpressionAcceptingTimeZone("minute", expr);
    }
    void visit(const ExpressionMonth* expr) final {
        generateDateExpressionAcceptingTimeZone("month", expr);
    }
    void visit(const ExpressionSecond* expr) final {
        generateDateExpressionAcceptingTimeZone("second", expr);
    }
    void visit(const ExpressionWeek* expr) final {
        generateDateExpressionAcceptingTimeZone("week", expr);
    }
    void visit(const ExpressionIsoWeekYear* expr) final {
        generateDateExpressionAcceptingTimeZone("isoWeekYear", expr);
    }
    void visit(const ExpressionIsoDayOfWeek* expr) final {
        generateDateExpressionAcceptingTimeZone("isoDayOfWeek", expr);
    }
    void visit(const ExpressionIsoWeek* expr) final {
        generateDateExpressionAcceptingTimeZone("isoWeek", expr);
    }
    void visit(const ExpressionYear* expr) final {
        generateDateExpressionAcceptingTimeZone("year", expr);
    }
    void visit(const ExpressionFromAccumulator<AccumulatorAvg>* expr) final {
        size_t arity = expr->getChildren().size();
        _context->ensureArity(arity);
        if (arity == 0) {
            pushABT(abt::Constant::null());
        } else if (arity == 1) {
            abt::ABT singleInput = _context->popABTExpr();
            abt::ProjectionName singleInputName =
                makeLocalVariableName(_context->state.frameId(), 0);
            auto k = makeVariable(singleInputName);

            abt::ABT stdDevSampExpr = buildABTMultiBranchConditionalFromCaseValuePairs(
                {ABTCaseValuePair{generateABTNullMissingOrUndefined(singleInputName),
                                  abt::Constant::null()},
                 ABTCaseValuePair{makeABTFunction("isArray", makeVariable(singleInputName)),
                                  makeFillEmptyNull(makeABTFunction(
                                      "avgOfArray", makeVariable(singleInputName)))},
                 ABTCaseValuePair{makeABTFunction("isNumber", makeVariable(singleInputName)),
                                  makeVariable(singleInputName)}},
                abt::Constant::null());

            pushABT(abt::make<abt::Let>(
                std::move(singleInputName), std::move(singleInput), std::move(stdDevSampExpr)));
        } else {
            generateExpressionFromAccumulatorExpression(expr, _context, "avgOfArray");
        }
    }
    void visit(const ExpressionFromAccumulatorN<AccumulatorFirstN>* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(const ExpressionFromAccumulatorN<AccumulatorLastN>* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(const ExpressionFromAccumulator<AccumulatorMax>* expr) final {
        visitMaxMinFunction(expr, _context, "maxOfArray");
    }

    void visit(const ExpressionFromAccumulator<AccumulatorMin>* expr) final {
        visitMaxMinFunction(expr, _context, "minOfArray");
    }

    void visit(const ExpressionFromAccumulatorN<AccumulatorMaxN>* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(const ExpressionFromAccumulatorN<AccumulatorMinN>* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(const ExpressionFromAccumulatorQuantile<AccumulatorMedian>* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(const ExpressionFromAccumulatorQuantile<AccumulatorPercentile>* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(const ExpressionFromAccumulator<AccumulatorStdDevPop>* expr) final {
        size_t arity = expr->getChildren().size();
        _context->ensureArity(arity);

        if (arity == 0) {
            pushABT(abt::Constant::null());
        } else if (arity == 1) {
            abt::ABT singleInput = _context->popABTExpr();
            abt::ProjectionName singleInputName =
                makeLocalVariableName(_context->state.frameId(), 0);
            abt::ABT stdDevPopExpr = buildABTMultiBranchConditionalFromCaseValuePairs(
                {ABTCaseValuePair{generateABTNullMissingOrUndefined(singleInputName),
                                  abt::Constant::null()},
                 ABTCaseValuePair{makeABTFunction("isArray", makeVariable(singleInputName)),
                                  makeFillEmptyNull(
                                      makeABTFunction("stdDevPop", makeVariable(singleInputName)))},
                 ABTCaseValuePair{
                     makeABTFunction("isNumber", makeVariable(singleInputName)),
                     // Population standard deviation for a single numeric input is always 0.
                     abt::Constant::int32(0)}},
                abt::Constant::null());

            pushABT(abt::make<abt::Let>(
                std::move(singleInputName), std::move(singleInput), std::move(stdDevPopExpr)));
        } else {
            generateExpressionFromAccumulatorExpression(expr, _context, "stdDevPop");
        }
    }
    void visit(const ExpressionFromAccumulator<AccumulatorStdDevSamp>* expr) final {
        size_t arity = expr->getChildren().size();
        _context->ensureArity(arity);

        if (arity == 0) {
            pushABT(abt::Constant::null());
        } else if (arity == 1) {
            abt::ABT singleInput = _context->popABTExpr();
            abt::ProjectionName singleInputName =
                makeLocalVariableName(_context->state.frameId(), 0);
            abt::ABT stdDevSampExpr = buildABTMultiBranchConditionalFromCaseValuePairs(
                {ABTCaseValuePair{generateABTNullMissingOrUndefined(singleInputName),
                                  abt::Constant::null()},
                 ABTCaseValuePair{makeABTFunction("isArray", makeVariable(singleInputName)),
                                  makeFillEmptyNull(makeABTFunction(
                                      "stdDevSamp", makeVariable(singleInputName)))}},
                // Sample standard deviation is undefined for a single input.
                abt::Constant::null());

            pushABT(abt::make<abt::Let>(
                std::move(singleInputName), std::move(singleInput), std::move(stdDevSampExpr)));
        } else {
            generateExpressionFromAccumulatorExpression(expr, _context, "stdDevSamp");
        }
    }
    void visit(const ExpressionFromAccumulator<AccumulatorSum>* expr) final {
        size_t arity = expr->getChildren().size();
        _context->ensureArity(arity);
        if (arity == 0) {
            pushABT(abt::Constant::null());
        } else if (arity == 1) {
            abt::ABT singleInput = _context->popABTExpr();
            abt::ProjectionName singleInputName =
                makeLocalVariableName(_context->state.frameId(), 0);
            auto k = makeVariable(singleInputName);

            // $sum returns 0 if the operand is missing, undefined, or non-numeric.
            abt::ABT stdDevSampExpr = buildABTMultiBranchConditionalFromCaseValuePairs(
                {ABTCaseValuePair{generateABTNullMissingOrUndefined(singleInputName),
                                  abt::Constant::int32(0)},
                 ABTCaseValuePair{makeABTFunction("isArray", makeVariable(singleInputName)),
                                  makeFillEmptyNull(makeABTFunction(
                                      "sumOfArray", makeVariable(singleInputName)))},
                 ABTCaseValuePair{makeABTFunction("isNumber", makeVariable(singleInputName)),
                                  makeVariable(singleInputName)}},
                abt::Constant::int32(0));

            pushABT(abt::make<abt::Let>(
                std::move(singleInputName), std::move(singleInput), std::move(stdDevSampExpr)));
        } else {
            generateExpressionFromAccumulatorExpression(expr, _context, "sumOfArray");
        }
    }
    void visit(const ExpressionFromAccumulator<AccumulatorMergeObjects>* expr) final {
        unsupportedExpression(expr->getOpName());
    }
    void visit(const ExpressionTests::Testable* expr) final {
        unsupportedExpression("$test");
    }
    void visit(const ExpressionInternalJsEmit* expr) final {
        unsupportedExpression("$internalJsEmit");
    }
    void visit(const ExpressionInternalFindSlice* expr) final {
        unsupportedExpression("$internalFindSlice");
    }
    void visit(const ExpressionInternalFindPositional* expr) final {
        unsupportedExpression("$internalFindPositional");
    }
    void visit(const ExpressionInternalFindElemMatch* expr) final {
        unsupportedExpression("$internalFindElemMatch");
    }
    void visit(const ExpressionFunction* expr) final {
        unsupportedExpression("$function");
    }

    void visit(const ExpressionRandom* expr) final {
        uassert(
            5155201, "$rand does not currently accept arguments", expr->getChildren().size() == 0);
        auto expression = makeABTFunction("rand");
        pushABT(std::move(expression));
    }

    void visit(const ExpressionCurrentDate* expr) final {
        uassert(9940500,
                "$currentDate does not currently accept arguments",
                expr->getChildren().size() == 0);
        auto expression = makeABTFunction("currentDate");
        pushABT(std::move(expression));
    }

    void visit(const ExpressionToHashedIndexKey* expr) final {
        unsupportedExpression("$toHashedIndexKey");
    }

    void visit(const ExpressionDateAdd* expr) final {
        generateDateArithmeticsExpression(expr, "dateAdd");
    }

    void visit(const ExpressionDateSubtract* expr) final {
        generateDateArithmeticsExpression(expr, "dateSubtract");
    }

    void visit(const ExpressionGetField* expr) final {
        unsupportedExpression("$getField");
    }

    void visit(const ExpressionSetField* expr) final {
        unsupportedExpression("$setField");
    }

    void visit(const ExpressionUUID* expr) final {
        // TODO(SERVER-101161): Support $uuid in SBE.
        unsupportedExpression("$uuid");
    }

    void visit(const ExpressionTsSecond* expr) final {
        _context->ensureArity(1);

        auto frameId = _context->state.frameId();
        auto arg = _context->popABTExpr();
        auto name = makeLocalVariableName(frameId, 0);
        auto var = makeVariable(name);

        auto tsSecondExpr = buildABTMultiBranchConditionalFromCaseValuePairs(
            {ABTCaseValuePair{generateABTNullMissingOrUndefined(name), abt::Constant::null()},
             ABTCaseValuePair{generateABTNonTimestampCheck(name),
                              makeABTFail(ErrorCodes::Error{7157900},
                                          str::stream() << expr->getOpName()
                                                        << " expects argument of type timestamp")}},
            makeABTFunction("tsSecond", makeVariable(name)));
        pushABT(abt::make<abt::Let>(std::move(name), std::move(arg), std::move(tsSecondExpr)));
    }

    void visit(const ExpressionTsIncrement* expr) final {
        _context->ensureArity(1);

        auto frameId = _context->state.frameId();
        auto arg = _context->popABTExpr();
        auto name = makeLocalVariableName(frameId, 0);
        auto var = makeVariable(name);

        auto tsIncrementExpr = buildABTMultiBranchConditionalFromCaseValuePairs(
            {ABTCaseValuePair{generateABTNullMissingOrUndefined(name), abt::Constant::null()},
             ABTCaseValuePair{generateABTNonTimestampCheck(name),
                              makeABTFail(ErrorCodes::Error{7157901},
                                          str::stream() << expr->getOpName()
                                                        << " expects argument of type timestamp")}},
            makeABTFunction("tsIncrement", makeVariable(name)));
        pushABT(abt::make<abt::Let>(std::move(name), std::move(arg), std::move(tsIncrementExpr)));
    }

    void visit(const ExpressionInternalOwningShard* expr) final {
        unsupportedExpression("$_internalOwningShard");
    }

    void visit(const ExpressionInternalIndexKey* expr) final {
        unsupportedExpression("$_internalIndexKey");
    }

    void visit(const ExpressionInternalKeyStringValue* expr) final {
        unsupportedExpression(expr->getOpName());
    }

private:
    /**
     * Shared logic for $round and $trunc expressions
     */
    template <typename ExprType>
    void visitRoundTruncExpression(const ExprType* expr) {
        const std::string opName(expr->getOpName());
        invariant(opName == "$round" || opName == "$trunc");

        const auto& children = expr->getChildren();
        invariant(children.size() == 1 || children.size() == 2);
        const bool hasPlaceArg = (children.size() == 2);
        _context->ensureArity(children.size());

        auto inputNumName = makeLocalVariableName(_context->state.frameId(), 0);
        auto inputPlaceName = makeLocalVariableName(_context->state.frameId(), 0);

        // We always need to validate the number parameter, since it will always exist.
        std::vector<ABTCaseValuePair> inputValidationCases{
            generateABTReturnNullIfNullMissingOrUndefined(makeVariable(inputNumName)),
            ABTCaseValuePair{
                generateABTNonNumericCheck(inputNumName),
                makeABTFail(ErrorCodes::Error{5155300}, opName + " only supports numeric types")}};
        // Only add these cases if we have a "place" argument.
        if (hasPlaceArg) {
            inputValidationCases.emplace_back(
                generateABTReturnNullIfNullMissingOrUndefined(makeVariable(inputPlaceName)));
            inputValidationCases.emplace_back(generateInvalidRoundPlaceArgCheck(inputPlaceName),
                                              makeABTFail(ErrorCodes::Error{5155301},
                                                          opName +
                                                              " requires \"place\" argument to be "
                                                              "an integer between -20 and 100"));
        }

        abt::ABT abtExpr = buildABTMultiBranchConditionalFromCaseValuePairs(
            std::move(inputValidationCases),
            makeABTFunction((opName == "$round" ? "round"_sd : "trunc"_sd),
                            makeVariable(inputNumName),
                            makeVariable(inputPlaceName)));

        // "place" argument defaults to 0.
        abt::ABT placeABT = hasPlaceArg ? _context->popABTExpr() : abt::Constant::int32(0);
        abt::ABT inputABT = _context->popABTExpr();
        pushABT(makeLet({std::move(inputNumName), std::move(inputPlaceName)},
                        abt::makeSeq(std::move(inputABT), std::move(placeABT)),
                        std::move(abtExpr)));
    }

    /**
     * Shared logic for $and, $or. Converts each child into an EExpression that evaluates to Boolean
     * true or false, based on MQL rules for $and and $or branches, and then chains the branches
     * together using binary and/or EExpressions so that the result has MQL's short-circuit
     * semantics.
     */
    void visitMultiBranchLogicExpression(const Expression* expr, abt::Operations logicOp) {
        invariant(logicOp == abt::Operations::And || logicOp == abt::Operations::Or);

        size_t numChildren = expr->getChildren().size();
        if (numChildren == 0) {
            // Empty $and and $or always evaluate to their logical operator's identity value:
            // true and false, respectively.
            auto logicIdentityVal = (logicOp == abt::Operations::And);
            pushABT(abt::Constant::boolean(logicIdentityVal));
            return;
        }

        std::vector<abt::ABT> exprs;
        exprs.reserve(numChildren);
        for (size_t i = 0; i < numChildren; ++i) {
            exprs.emplace_back(
                makeFillEmptyFalse(makeABTFunction("coerceToBool", _context->popABTExpr())));
        }
        std::reverse(exprs.begin(), exprs.end());

        pushABT(makeBooleanOpTree(logicOp, std::move(exprs)));
    }

    /**
     * Handle $switch and $cond, which have different syntax but are structurally identical in the
     * AST.
     */
    void visitConditionalExpression(const Expression* expr) {
        // The default case is always the last child in the ExpressionSwitch. If it is unspecified
        // in the user's query, it is a nullptr. In ExpressionCond, the last child is the "else"
        // branch, and it is guaranteed not to be nullptr.
        auto defaultExpr = expr->getChildren().back() != nullptr
            ? _context->popABTExpr()
            : makeABTFail(ErrorCodes::Error{7158303},
                          "$switch could not find a matching branch for an "
                          "input, and no default was specified.");

        size_t numCases = expr->getChildren().size() / 2;
        std::vector<ABTCaseValuePair> cases;
        cases.reserve(numCases);

        for (size_t i = 0; i < numCases; ++i) {
            auto valueExpr = _context->popABTExpr();
            auto conditionExpr =
                makeFillEmptyFalse(makeABTFunction("coerceToBool", _context->popABTExpr()));
            cases.emplace_back(std::move(conditionExpr), std::move(valueExpr));
        }

        std::reverse(cases.begin(), cases.end());

        pushABT(buildABTMultiBranchConditionalFromCaseValuePairs(std::move(cases),
                                                                 std::move(defaultExpr)));
    }

    void generateDateExpressionAcceptingTimeZone(StringData exprName, const Expression* expr) {
        const auto& children = expr->getChildren();
        invariant(children.size() == 2);

        auto timezoneExpression =
            children[1] ? _context->popABTExpr() : abt::Constant::str("UTC"_sd);
        auto dateExpression = _context->popABTExpr();

        // Local bind to hold the date expression result
        auto dateName = makeLocalVariableName(_context->state.frameId(), 0);

        auto timeZoneDBSlot = *_context->state.getTimeZoneDBSlot();

        // Set parameters for an invocation of the built-in function.
        abt::ABTVector arguments;
        arguments.push_back(makeVariable(dateName));

        // Local bind to hold the timezone expression result
        auto timezoneName = makeLocalVariableName(_context->state.frameId(), 0);
        // Create a variable to hold the built-in function.
        auto funcName = makeLocalVariableName(_context->state.frameId(), 0);

        // Create expressions to check that each argument to the function exists, is not
        // null, and is of the correct type.
        std::vector<ABTCaseValuePair> inputValidationCases;
        // Return the evaluation of the function, if it exists.
        inputValidationCases.push_back(
            {makeABTFunction("exists"_sd, makeVariable(funcName)), makeVariable(funcName)});
        // Return null if any of the parameters is either null or missing.
        inputValidationCases.push_back(
            generateABTReturnNullIfNullMissingOrUndefined(makeVariable(dateName)));

        // "timezone" parameter validation.
        if (timezoneExpression.is<abt::Constant>()) {
            auto [timezoneTag, timezoneVal] = timezoneExpression.cast<abt::Constant>()->get();
            auto [timezoneDBTag, timezoneDBVal] =
                _context->state.env->getAccessor(timeZoneDBSlot)->getViewOfValue();
            auto timezoneDB = sbe::value::getTimeZoneDBView(timezoneDBVal);
            uassert(5157900,
                    str::stream() << "$" << exprName.toString()
                                  << " parameter 'timezone' must be a string",
                    sbe::value::isString(timezoneTag));
            uassert(5157901,
                    str::stream() << "$" << exprName.toString()
                                  << " parameter 'timezone' must be a valid timezone",
                    sbe::vm::isValidTimezone(timezoneTag, timezoneVal, timezoneDB));
            auto [timezoneObjTag, timezoneObjVal] = sbe::value::makeCopyTimeZone(
                sbe::vm::getTimezone(timezoneTag, timezoneVal, timezoneDB));
            auto timezoneConst = abt::make<abt::Constant>(timezoneObjTag, timezoneObjVal);
            arguments.push_back(std::move(timezoneConst));
        } else {
            inputValidationCases.push_back(
                generateABTReturnNullIfNullMissingOrUndefined(timezoneExpression));
            inputValidationCases.emplace_back(
                generateABTNonStringCheck(makeVariable(timezoneName)),
                makeABTFail(ErrorCodes::Error{5157902},
                            str::stream() << "$" << exprName.toString()
                                          << " parameter 'timezone' must be a string"));
            inputValidationCases.emplace_back(
                makeNot(makeABTFunction(
                    "isTimezone", makeABTVariable(timeZoneDBSlot), makeVariable(timezoneName))),
                makeABTFail(ErrorCodes::Error{5157903},
                            str::stream() << "$" << exprName.toString()
                                          << " parameter 'timezone' must be a valid timezone"));
            arguments.push_back(makeABTVariable(timeZoneDBSlot));
            arguments.push_back(makeVariable(timezoneName));
        }

        // "date" parameter validation.
        inputValidationCases.emplace_back(generateABTFailIfNotCoercibleToDate(
            makeVariable(dateName), ErrorCodes::Error{5157904}, exprName, "date"_sd));

        pushABT(makeLet(
            {std::move(dateName), std::move(timezoneName), funcName},
            abt::makeSeq(std::move(dateExpression),
                         std::move(timezoneExpression),
                         abt::make<abt::FunctionCall>(exprName.toString(), std::move(arguments))),
            buildABTMultiBranchConditionalFromCaseValuePairs(std::move(inputValidationCases),
                                                             abt::Constant::nothing())));
    }

    /**
     * Creates a CaseValuePair such that an exception is thrown if a value of the parameter denoted
     * by variable 'dateRef' is of a type that is not coercible to a date.
     *
     * dateRef - a variable corresponding to the parameter.
     * errorCode - error code of the type mismatch error.
     * expressionName - a name of an expression the parameter belongs to.
     * parameterName - a name of the parameter corresponding to variable 'dateRef'.
     */
    static ABTCaseValuePair generateABTFailIfNotCoercibleToDate(const abt::ABT& dateVar,
                                                                ErrorCodes::Error errorCode,
                                                                StringData expressionName,
                                                                StringData parameterName) {
        return {
            makeNot(makeABTFunction("typeMatch", dateVar, abt::Constant::int32(dateTypeMask()))),
            makeABTFail(errorCode,
                        str::stream() << expressionName << " parameter '" << parameterName
                                      << "' must be coercible to date")};
    }

    /**
     * Creates a CaseValuePair such that Null value is returned if a value of variable denoted by
     * 'variable' is null, missing, or undefined.
     */
    static ABTCaseValuePair generateABTReturnNullIfNullMissingOrUndefined(const abt::ABT& name) {
        return {generateABTNullMissingOrUndefined(name), abt::Constant::null()};
    }

    /**
     * Creates a boolean expression to check if 'variable' is equal to string 'string'.
     */
    static abt::ABT generateABTIsEqualToStringCheck(const abt::ABT& expr, StringData string) {
        return abt::make<abt::BinaryOp>(
            abt::Operations::And,
            makeABTFunction("isString", expr),
            abt::make<abt::BinaryOp>(abt::Operations::Eq, expr, abt::Constant::str(string)));
    }

    /**
     * Shared expression building logic for trignometric expressions to make sure the operand
     * is numeric and is not null.
     */
    void generateTrigonometricExpression(StringData exprName) {
        auto frameId = _context->state.frameId();
        auto arg = _context->popABTExpr();
        auto argName = makeLocalVariableName(frameId, 0);

        auto genericTrigonometricExpr = buildABTMultiBranchConditionalFromCaseValuePairs(
            {ABTCaseValuePair{generateABTNullMissingOrUndefined(argName), abt::Constant::null()},
             ABTCaseValuePair{makeABTFunction("isNumber", makeVariable(argName)),
                              makeABTFunction(exprName, makeVariable(argName))}},
            makeABTFail(ErrorCodes::Error{7157800},
                        str::stream()
                            << "$" << exprName.toString() << " supports only numeric types"));

        pushABT(abt::make<abt::Let>(
            std::move(argName), std::move(arg), std::move(genericTrigonometricExpr)));
    }

    /**
     * Shared expression building logic for binary trigonometric expressions to make sure the
     * operands are numeric and are not null.
     */
    void generateTrigonometricExpressionBinary(StringData exprName) {
        _context->ensureArity(2);
        auto rhs = _context->popABTExpr();
        auto lhs = _context->popABTExpr();
        auto lhsName = makeLocalVariableName(_context->state.frameId(), 0);
        auto rhsName = makeLocalVariableName(_context->state.frameId(), 0);
        auto lhsVariable = makeVariable(lhsName);
        auto rhsVariable = makeVariable(rhsName);

        auto checkNullOrMissing =
            abt::make<abt::BinaryOp>(abt::Operations::Or,
                                     generateABTNullMissingOrUndefined(lhsName),
                                     generateABTNullMissingOrUndefined(rhsName));

        auto checkIsNumber = abt::make<abt::BinaryOp>(abt::Operations::And,
                                                      makeABTFunction("isNumber", lhsVariable),
                                                      makeABTFunction("isNumber", rhsVariable));

        auto genericTrigonometricExpr = buildABTMultiBranchConditionalFromCaseValuePairs(
            {ABTCaseValuePair{std::move(checkNullOrMissing), abt::Constant::null()},
             ABTCaseValuePair{
                 std::move(checkIsNumber),
                 makeABTFunction(exprName, std::move(lhsVariable), std::move(rhsVariable))}},
            makeABTFail(ErrorCodes::Error{7157801},
                        str::stream() << "$" << exprName << " supports only numeric types"));


        pushABT(makeLet({std::move(lhsName), std::move(rhsName)},
                        abt::makeSeq(std::move(lhs), std::move(rhs)),
                        std::move(genericTrigonometricExpr)));
    }

    /**
     * Shared expression building logic for trignometric expressions with bounds for the valid
     * values of the argument.
     */
    void generateTrigonometricExpressionWithBounds(StringData exprName,
                                                   const DoubleBound& lowerBound,
                                                   const DoubleBound& upperBound) {
        auto frameId = _context->state.frameId();
        auto arg = _context->popABTExpr();
        auto argName = makeLocalVariableName(frameId, 0);
        auto variable = makeVariable(argName);
        abt::Operations lowerCmp =
            lowerBound.inclusive ? abt::Operations::Gte : abt::Operations::Gt;
        abt::Operations upperCmp =
            upperBound.inclusive ? abt::Operations::Lte : abt::Operations::Lt;
        auto checkBounds = abt::make<abt::BinaryOp>(
            abt::Operations::And,
            abt::make<abt::BinaryOp>(
                lowerCmp, variable, abt::Constant::fromDouble(lowerBound.bound)),
            abt::make<abt::BinaryOp>(
                upperCmp, variable, abt::Constant::fromDouble(upperBound.bound)));

        auto checkIsNumber = makeABTFunction("isNumber", variable);
        auto trigonometricExpr = makeABTFunction(exprName, variable);

        auto genericTrigonometricExpr = buildABTMultiBranchConditionalFromCaseValuePairs(
            {ABTCaseValuePair{generateABTNullMissingOrUndefined(argName), abt::Constant::null()},
             ABTCaseValuePair{makeNot(std::move(checkIsNumber)),
                              makeABTFail(ErrorCodes::Error{7157802},
                                          str::stream() << "$" << exprName.toString()
                                                        << " supports only numeric types")},
             ABTCaseValuePair{generateABTNaNCheck(argName), std::move(variable)},
             ABTCaseValuePair{std::move(checkBounds), std::move(trigonometricExpr)}},
            makeABTFail(ErrorCodes::Error{7157803},
                        str::stream() << "Cannot apply $" << exprName.toString()
                                      << ", value must be in " << lowerBound.printLowerBound()
                                      << ", " << upperBound.printUpperBound()));

        pushABT(abt::make<abt::Let>(
            std::move(argName), std::move(arg), std::move(genericTrigonometricExpr)));
    }

    /*
     * Generates an EExpression that returns an index for $indexOfBytes or $indexOfCP.
     */
    void visitIndexOfFunction(const Expression* expr,
                              ExpressionVisitorContext* _context,
                              const std::string& indexOfFunction) {
        const auto& children = expr->getChildren();
        auto operandSize = children.size() <= 3 ? 3 : 4;
        abt::ABTVector operands;
        operands.reserve(operandSize);

        auto strName = makeLocalVariableName(_context->state.frameId(), 0);
        auto substrName = makeLocalVariableName(_context->state.frameId(), 0);
        auto startIndexName = makeLocalVariableName(_context->state.frameId(), 0);
        boost::optional<abt::ProjectionName> endIndexName;

        // Get arguments from stack.
        std::vector<abt::ProjectionName> varNames;
        switch (children.size()) {
            case 2: {
                varNames.emplace_back(startIndexName);
                operands.emplace_back(abt::Constant::int64(0));
                varNames.emplace_back(substrName);
                operands.emplace_back(_context->popABTExpr());
                varNames.emplace_back(strName);
                operands.emplace_back(_context->popABTExpr());
                break;
            }
            case 3: {
                varNames.emplace_back(startIndexName);
                operands.emplace_back(_context->popABTExpr());
                varNames.emplace_back(substrName);
                operands.emplace_back(_context->popABTExpr());
                varNames.emplace_back(strName);
                operands.emplace_back(_context->popABTExpr());
                break;
            }
            case 4: {
                endIndexName.emplace(makeLocalVariableName(_context->state.frameId(), 0));
                varNames.emplace_back(*endIndexName);
                operands.emplace_back(_context->popABTExpr());
                varNames.emplace_back(startIndexName);
                operands.emplace_back(_context->popABTExpr());
                varNames.emplace_back(substrName);
                operands.emplace_back(_context->popABTExpr());
                varNames.emplace_back(strName);
                operands.emplace_back(_context->popABTExpr());
                break;
            }
            default:
                MONGO_UNREACHABLE;
        }

        // Add string and substring operands.
        abt::ABTVector functionArgs{makeVariable(strName), makeVariable(substrName)};

        // Add start index operand.
        auto checkValidStartIndex = buildABTMultiBranchConditionalFromCaseValuePairs(
            {ABTCaseValuePair{generateABTNullishOrNotRepresentableInt32Check(startIndexName),
                              makeABTFail(ErrorCodes::Error{7158003},
                                          str::stream()
                                              << "$" << indexOfFunction
                                              << " start index must resolve to a number")},
             ABTCaseValuePair{generateABTNegativeCheck(startIndexName),
                              makeABTFail(ErrorCodes::Error{7158004},
                                          str::stream() << "$" << indexOfFunction
                                                        << " start index must be positive")}},
            makeABTFunction(
                "convert",
                makeVariable(startIndexName),
                abt::Constant::int32(static_cast<int32_t>(sbe::value::TypeTags::NumberInt64))));
        functionArgs.push_back(std::move(checkValidStartIndex));

        // Add end index operand.
        if (endIndexName) {
            auto checkValidEndIndex = buildABTMultiBranchConditionalFromCaseValuePairs(
                {ABTCaseValuePair{generateABTNullishOrNotRepresentableInt32Check(*endIndexName),
                                  makeABTFail(ErrorCodes::Error{7158005},
                                              str::stream()
                                                  << "$" << indexOfFunction
                                                  << " end index must resolve to a number")},
                 ABTCaseValuePair{generateABTNegativeCheck(*endIndexName),
                                  makeABTFail(ErrorCodes::Error{7158006},
                                              str::stream() << "$" << indexOfFunction
                                                            << " end index must be positive")}},
                makeABTFunction(
                    "convert",
                    makeVariable(*endIndexName),
                    abt::Constant::int32(static_cast<int32_t>(sbe::value::TypeTags::NumberInt64))));
            functionArgs.push_back(std::move(checkValidEndIndex));
        }

        // Check if string or substring are null or missing before calling indexOfFunction.
        auto resultExpr = buildABTMultiBranchConditionalFromCaseValuePairs(
            {ABTCaseValuePair{generateABTNullMissingOrUndefined(strName), abt::Constant::null()},
             ABTCaseValuePair{generateABTNonStringCheck(strName),
                              makeABTFail(ErrorCodes::Error{7158007},
                                          str::stream()
                                              << "$" << indexOfFunction
                                              << " string must resolve to a string or null")},
             ABTCaseValuePair{generateABTNullMissingOrUndefined(substrName),
                              makeABTFail(ErrorCodes::Error{7158008},
                                          str::stream() << "$" << indexOfFunction
                                                        << " substring must resolve to a string")},
             ABTCaseValuePair{generateABTNonStringCheck(substrName),
                              makeABTFail(ErrorCodes::Error{7158009},
                                          str::stream() << "$" << indexOfFunction
                                                        << " substring must resolve to a string")}},
            abt::make<abt::FunctionCall>(indexOfFunction, std::move(functionArgs)));

        // Build local binding tree.
        pushABT(makeLet(std::move(varNames), std::move(operands), std::move(resultExpr)));
    }

    /*
     * Generates an EExpression that returns the maximum for $max and minimum for $min.
     */
    void visitMaxMinFunction(const Expression* expr,
                             ExpressionVisitorContext* _context,
                             const std::string& maxMinFunction) {
        size_t arity = expr->getChildren().size();
        _context->ensureArity(arity);

        if (arity == 0) {
            pushABT(abt::Constant::null());
        } else if (arity == 1) {
            abt::ABT singleInput = _context->popABTExpr();
            abt::ProjectionName singleInputName =
                makeLocalVariableName(_context->state.frameId(), 0);

            abt::ABT maxMinExpr = buildABTMultiBranchConditionalFromCaseValuePairs(
                {ABTCaseValuePair{generateABTNullMissingOrUndefined(singleInputName),
                                  abt::Constant::null()},
                 ABTCaseValuePair{
                     makeABTFunction("isArray", makeVariable(singleInputName)),
                     // In the case of a single argument, if the input is an array, $min or $max
                     // operates on the elements of array to return a single value.
                     makeFillEmptyNull(
                         makeABTFunction(maxMinFunction, makeVariable(singleInputName)))}},
                makeVariable(singleInputName));

            pushABT(abt::make<abt::Let>(
                std::move(singleInputName), std::move(singleInput), std::move(maxMinExpr)));
        } else {
            generateExpressionFromAccumulatorExpression(expr, _context, maxMinFunction);
        }
    }

    /*
     * Converts n > 1 children into an array and generates an EExpression for
     * ExpressionFromAccumulator expressions. Accepts an Expression, ExpressionVisitorContext, and
     * the name of a builtin function.
     */
    void generateExpressionFromAccumulatorExpression(const Expression* expr,
                                                     ExpressionVisitorContext* _context,
                                                     const std::string& functionCall) {
        size_t arity = expr->getChildren().size();
        std::vector<abt::ProjectionName> varNames;
        std::vector<abt::ABT> binds;
        for (size_t idx = 0; idx < arity; ++idx) {
            varNames.emplace_back(makeLocalVariableName(_context->state.frameId(), 0));
            binds.emplace_back(_context->popABTExpr());
        }

        std::reverse(std::begin(binds), std::end(binds));
        abt::ABTVector argVars;
        for (auto& name : varNames) {
            argVars.push_back(makeVariable(name));
        }

        // Take in all arguments and construct an array.
        auto arrayExpr = makeLet(std::move(varNames),
                                 std::move(binds),
                                 abt::make<abt::FunctionCall>("newArray", std::move(argVars)));

        pushABT(makeFillEmptyNull(makeABTFunction(functionCall, std::move(arrayExpr))));
    }

    /**
     * Generic logic for building set expressions: setUnion, setIntersection, etc.
     */
    void generateSetExpression(const Expression* expr, SetOperation setOp) {
        using namespace std::literals;

        size_t arity = expr->getChildren().size();
        _context->ensureArity(arity);

        abt::ABTVector args;
        abt::ProjectionNameVector argNames;
        abt::ABTVector variables;

        abt::ABTVector checkNulls;
        abt::ABTVector checkNotArrays;

        auto collatorSlot = _context->state.getCollatorSlot();

        args.reserve(arity);
        argNames.reserve(arity);
        variables.reserve(arity + (collatorSlot.has_value() ? 1 : 0));
        checkNulls.reserve(arity);
        checkNotArrays.reserve(arity);

        auto operatorNameSetFunctionNamePair =
            getSetOperatorAndFunctionNames(setOp, collatorSlot.has_value());
        auto operatorName = operatorNameSetFunctionNamePair.first;
        auto setFunctionName = operatorNameSetFunctionNamePair.second;
        if (collatorSlot) {
            variables.push_back(makeABTVariable(*collatorSlot));
        }

        for (size_t idx = 0; idx < arity; ++idx) {
            args.push_back(_context->popABTExpr());
            auto argName = makeLocalVariableName(_context->state.frameId(), 0);
            argNames.push_back(argName);
            variables.push_back(makeVariable(argName));

            checkNulls.push_back(generateABTNullMissingOrUndefined(argName));
            checkNotArrays.push_back(generateABTNonArrayCheck(std::move(argName)));
        }
        // Reverse the args array to preserve the original order of the arguments, since some set
        // operations, such as $setDifference, are not commutative.
        std::reverse(std::begin(args), std::end(args));

        auto checkNullAnyArgument = makeBooleanOpTree(abt::Operations::Or, std::move(checkNulls));
        auto checkNotArrayAnyArgument =
            makeBooleanOpTree(abt::Operations::Or, std::move(checkNotArrays));
        abt::ABT setExpr = [&]() -> abt::ABT {
            // To match classic engine semantics, $setEquals and $setIsSubset should throw an error
            // for any non-array arguments including null and missing values.
            if (setOp == SetOperation::Equals || setOp == SetOperation::IsSubset) {
                return makeIf(
                    makeFillEmptyTrue(std::move(checkNotArrayAnyArgument)),
                    makeABTFail(ErrorCodes::Error{7158100},
                                str::stream()
                                    << "All operands of $" << operatorName << " must be arrays."),
                    abt::make<abt::FunctionCall>(setFunctionName.toString(), std::move(variables)));
            } else {
                return buildABTMultiBranchConditionalFromCaseValuePairs(
                    {ABTCaseValuePair{std::move(checkNullAnyArgument), abt::Constant::null()},
                     ABTCaseValuePair{std::move(checkNotArrayAnyArgument),
                                      makeABTFail(ErrorCodes::Error{7158101},
                                                  str::stream()
                                                      << "All operands of $" << operatorName
                                                      << " must be arrays.")}},
                    abt::make<abt::FunctionCall>(setFunctionName.toString(), std::move(variables)));
            }
        }();

        setExpr = makeLet(std::move(argNames), std::move(args), std::move(setExpr));
        pushABT(std::move(setExpr));
    }

    std::pair<StringData, StringData> getSetOperatorAndFunctionNames(SetOperation setOp,
                                                                     bool hasCollator) const {
        switch (setOp) {
            case SetOperation::Difference:
                return std::make_pair("setDifference"_sd,
                                      hasCollator ? "collSetDifference"_sd : "setDifference"_sd);
            case SetOperation::Intersection:
                return std::make_pair("setIntersection"_sd,
                                      hasCollator ? "collSetIntersection"_sd
                                                  : "setIntersection"_sd);
            case SetOperation::Union:
                return std::make_pair("setUnion"_sd,
                                      hasCollator ? "collSetUnion"_sd : "setUnion"_sd);
            case SetOperation::Equals:
                return std::make_pair("setEquals"_sd,
                                      hasCollator ? "collSetEquals"_sd : "setEquals"_sd);
            case SetOperation::IsSubset:
                return std::make_pair("setIsSubset"_sd,
                                      hasCollator ? "collSetIsSubset"_sd : "setIsSubset"_sd);
        }
        MONGO_UNREACHABLE;
    }

    /**
     * Shared expression building logic for regex expressions.
     */
    void generateRegexExpression(const ExpressionRegex* expr, StringData exprName) {
        size_t arity = expr->hasOptions() ? 3 : 2;
        _context->ensureArity(arity);

        boost::optional<abt::ABT> options;
        if (expr->hasOptions()) {
            options = _context->popABTExpr();
        }
        auto pattern = _context->popABTExpr();
        auto input = _context->popABTExpr();

        auto inputVar = makeLocalVariableName(_context->state.frameId(), 0);
        auto patternVar = makeLocalVariableName(_context->state.frameId(), 0);

        auto generateRegexNullResponse = [exprName]() {
            if (exprName == "regexMatch"_sd) {
                return abt::Constant::boolean(false);
            } else if (exprName == "regexFindAll"_sd) {
                return abt::Constant::emptyArray();
            } else {
                return abt::Constant::null();
            }
        };

        auto makeError = [exprName](int errorCode, StringData message) {
            return makeABTFail(ErrorCodes::Error{errorCode},
                               str::stream() << "$" << exprName.toString() << ": " << message);
        };

        auto makeRegexFunctionCall = [&](abt::ABT compiledRegex) {
            auto resultVar = makeLocalVariableName(_context->state.frameId(), 0);
            return abt::make<abt::Let>(
                resultVar,
                makeABTFunction(exprName, std::move(compiledRegex), makeVariable(inputVar)),
                abt::make<abt::If>(
                    makeABTFunction("exists"_sd, makeVariable(resultVar)),
                    makeVariable(resultVar),
                    makeError(5073403, "error occurred while executing the regular expression")));
        };

        auto regexFunctionResult = [&]() {
            if (auto patternAndOptions = expr->getConstantPatternAndOptions(); patternAndOptions) {
                auto [pattern, options] = *patternAndOptions;
                if (!pattern) {
                    // Pattern is null, just generate null result.
                    return generateRegexNullResponse();
                }

                // Create the compiled Regex from constant pattern and options.
                auto [regexTag, regexVal] = sbe::makeNewPcreRegex(*pattern, options);
                auto compiledRegex = makeABTConstant(regexTag, regexVal);
                return makeRegexFunctionCall(std::move(compiledRegex));
            }

            // 'patternArgument' contains the following expression:
            //
            // if isString(pattern) {
            //     if hasNullBytes(pattern) {
            //         fail('pattern cannot have null bytes in it')
            //     } else {
            //         pattern
            //     }
            // } else if isBsonRegex(pattern) {
            //     getRegexPattern(pattern)
            // } else {
            //     fail('pattern must be either string or BSON RegEx')
            // }
            auto patternArgument = abt::make<abt::If>(
                makeABTFunction("isString"_sd, makeVariable(patternVar)),
                abt::make<abt::If>(
                    makeABTFunction("hasNullBytes"_sd, makeVariable(patternVar)),
                    makeError(5126602, "regex pattern must not have embedded null bytes"),
                    makeVariable(patternVar)),
                abt::make<abt::If>(
                    makeABTFunction("typeMatch"_sd,
                                    makeVariable(patternVar),
                                    abt::Constant::int32(getBSONTypeMask(BSONType::RegEx))),
                    makeABTFunction("getRegexPattern"_sd, makeVariable(patternVar)),
                    makeError(5126601,
                              "regex pattern must have either string or BSON RegEx type")));

            if (!options) {
                // If no options are passed to the expression, try to extract them from the
                // pattern.
                auto optionsArgument = abt::make<abt::If>(
                    makeABTFunction("typeMatch"_sd,
                                    makeVariable(patternVar),
                                    abt::Constant::int32(getBSONTypeMask(BSONType::RegEx))),
                    makeABTFunction("getRegexFlags"_sd, makeVariable(patternVar)),
                    makeABTConstant(""_sd));
                auto compiledRegex = makeABTFunction(
                    "regexCompile"_sd, std::move(patternArgument), std::move(optionsArgument));
                return abt::make<abt::If>(makeABTFunction("isNull"_sd, makeVariable(patternVar)),
                                          generateRegexNullResponse(),
                                          makeRegexFunctionCall(std::move(compiledRegex)));
            }

            // If there are options passed to the expression, we construct local bind with
            // options argument because it needs to be validated even when pattern is null.
            auto userOptionsVar = makeLocalVariableName(_context->state.frameId(), 0);
            auto optionsArgument = [&]() {
                // The code below generates the following expression:
                //
                // let stringOptions =
                //     if isString(options) {
                //         if hasNullBytes(options) {
                //             fail('options cannot have null bytes in it')
                //         } else {
                //             options
                //         }
                //     } else if isNull(options) {
                //         ''
                //     } else {
                //         fail('options must be either string or null')
                //     }
                // in
                //     if isBsonRegex(pattern) {
                //         let bsonOptions = getRegexFlags(pattern)
                //         in
                //             if stringOptions == "" {
                //                 bsonOptions
                //             } else if bsonOptions == "" {
                //                 stringOptions
                //             } else {
                //                 fail('multiple options specified')
                //             }
                //     } else {
                //         stringOptions
                //     }
                auto stringOptions = abt::make<abt::If>(
                    makeABTFunction("isString"_sd, makeVariable(userOptionsVar)),
                    abt::make<abt::If>(
                        makeABTFunction("hasNullBytes"_sd, makeVariable(userOptionsVar)),
                        makeError(5126604, "regex flags must not have embedded null bytes"),
                        makeVariable(userOptionsVar)),
                    abt::make<abt::If>(
                        makeABTFunction("isNull"_sd, makeVariable(userOptionsVar)),
                        makeABTConstant(""_sd),
                        makeError(5126603, "regex flags must have either string or null type")));

                auto generateIsEmptyString = [](const abt::ProjectionName& var) {
                    return abt::make<abt::BinaryOp>(
                        abt::Operations::Eq, makeVariable(var), makeABTConstant(""_sd));
                };

                auto stringVar = makeLocalVariableName(_context->state.frameId(), 0);
                auto bsonPatternVar = makeLocalVariableName(_context->state.frameId(), 0);
                return abt::make<abt::Let>(
                    stringVar,
                    std::move(stringOptions),
                    abt::make<abt::If>(
                        makeABTFunction("typeMatch"_sd,
                                        makeVariable(patternVar),
                                        abt::Constant::int32(getBSONTypeMask(BSONType::RegEx))),
                        abt::make<abt::Let>(
                            bsonPatternVar,
                            makeABTFunction("getRegexFlags", makeVariable(patternVar)),
                            buildABTMultiBranchConditionalFromCaseValuePairs(
                                {ABTCaseValuePair{generateIsEmptyString(stringVar),
                                                  makeVariable(bsonPatternVar)},
                                 ABTCaseValuePair{generateIsEmptyString(bsonPatternVar),
                                                  makeVariable(stringVar)}},
                                makeError(5126605,
                                          "regex options cannot be specified in both BSON "
                                          "RegEx and 'options' field"))),
                        makeVariable(stringVar)));
            }();

            auto optionsVar = makeLocalVariableName(_context->state.frameId(), 0);
            return makeLet(
                {userOptionsVar, optionsVar},
                abt::makeSeq(std::move(*options), std::move(optionsArgument)),
                abt::make<abt::If>(
                    makeABTFunction("isNull"_sd, makeVariable(patternVar)),
                    generateRegexNullResponse(),
                    makeRegexFunctionCall(makeABTFunction(
                        "regexCompile"_sd, makeVariable(patternVar), makeVariable(optionsVar)))));
        }();

        auto regexCall = buildABTMultiBranchConditionalFromCaseValuePairs(
            {ABTCaseValuePair{generateABTNullMissingOrUndefined(inputVar),
                              generateRegexNullResponse()},
             ABTCaseValuePair{makeNot(makeABTFunction("isString"_sd, makeVariable(inputVar))),
                              makeError(5073401, "input must be of type string")}},
            std::move(regexFunctionResult));

        pushABT(makeLet({inputVar, patternVar},
                        abt::makeSeq(std::move(input), std::move(pattern)),
                        std::move(regexCall)));
    }

    /**
     * Generic logic for building $dateAdd and $dateSubtract expressions.
     */
    void generateDateArithmeticsExpression(const ExpressionDateArithmetics* expr,
                                           const std::string& dateExprName) {
        const auto& children = expr->getChildren();
        auto arity = children.size();
        invariant(arity == 4);
        auto timezoneExpr = children[3] ? _context->popABTExpr() : abt::Constant::str("UTC"_sd);
        auto amountExpr = _context->popABTExpr();
        auto unitExpr = _context->popABTExpr();
        auto startDateExpr = _context->popABTExpr();

        auto startDateName = makeLocalVariableName(_context->state.frameId(), 0);
        auto unitName = makeLocalVariableName(_context->state.frameId(), 0);
        auto origAmountName = makeLocalVariableName(_context->state.frameId(), 0);
        auto tzName = makeLocalVariableName(_context->state.frameId(), 0);
        auto amountName = makeLocalVariableName(_context->state.frameId(), 0);

        auto convertedAmountInt64 = [&]() {
            if (dateExprName == "dateAdd") {
                return makeABTFunction(
                    "convert",
                    makeVariable(origAmountName),
                    abt::Constant::int32(static_cast<int32_t>(sbe::value::TypeTags::NumberInt64)));
            } else if (dateExprName == "dateSubtract") {
                return makeABTFunction(
                    "convert",
                    abt::make<abt::UnaryOp>(abt::Operations::Neg, makeVariable(origAmountName)),
                    abt::Constant::int32(static_cast<int32_t>(sbe::value::TypeTags::NumberInt64)));
            } else {
                MONGO_UNREACHABLE;
            }
        }();

        auto timeZoneDBSlot = *_context->state.getTimeZoneDBSlot();
        auto timeZoneDBVar = makeABTVariable(timeZoneDBSlot);

        abt::ABTVector checkNullArg;
        checkNullArg.push_back(generateABTNullMissingOrUndefined(startDateName));
        checkNullArg.push_back(generateABTNullMissingOrUndefined(unitName));
        checkNullArg.push_back(generateABTNullMissingOrUndefined(origAmountName));
        checkNullArg.push_back(generateABTNullMissingOrUndefined(tzName));

        auto checkNullAnyArgument = makeBooleanOpTree(abt::Operations::Or, std::move(checkNullArg));

        auto dateAddExpr = buildABTMultiBranchConditionalFromCaseValuePairs(
            {ABTCaseValuePair{std::move(checkNullAnyArgument), abt::Constant::null()},
             ABTCaseValuePair{generateABTNonStringCheck(tzName),
                              makeABTFail(ErrorCodes::Error{7157902},
                                          str::stream()
                                              << "$" << dateExprName
                                              << " expects timezone argument of type string")},
             ABTCaseValuePair{
                 makeNot(makeABTFunction("isTimezone", timeZoneDBVar, makeVariable(tzName))),
                 makeABTFail(ErrorCodes::Error{7157903},
                             str::stream() << "$" << dateExprName << " expects a valid timezone")},
             ABTCaseValuePair{
                 makeNot(makeABTFunction("typeMatch",
                                         makeVariable(startDateName),
                                         abt::Constant::int32(dateTypeMask()))),
                 makeABTFail(ErrorCodes::Error{7157904},
                             str::stream() << "$" << dateExprName
                                           << " must have startDate argument convertable to date")},
             ABTCaseValuePair{generateABTNonStringCheck(unitName),
                              makeABTFail(ErrorCodes::Error{7157905},
                                          str::stream()
                                              << "$" << dateExprName
                                              << " expects unit argument of type string")},
             ABTCaseValuePair{
                 makeNot(makeABTFunction("isTimeUnit", makeVariable(unitName))),
                 makeABTFail(ErrorCodes::Error{7157906},
                             str::stream() << "$" << dateExprName << " expects a valid time unit")},
             ABTCaseValuePair{makeNot(makeABTFunction("exists", makeVariable(amountName))),
                              makeABTFail(ErrorCodes::Error{7157907},
                                          str::stream() << "invalid $" << dateExprName
                                                        << " 'amount' argument value")}},
            makeABTFunction("dateAdd",
                            timeZoneDBVar,
                            makeVariable(startDateName),
                            makeVariable(unitName),
                            makeVariable(amountName),
                            makeVariable(tzName)));

        pushABT(makeLet({std::move(startDateName),
                         std::move(unitName),
                         std::move(origAmountName),
                         std::move(tzName),
                         std::move(amountName)},
                        abt::makeSeq(std::move(startDateExpr),
                                     std::move(unitExpr),
                                     std::move(amountExpr),
                                     std::move(timezoneExpr),
                                     std::move(convertedAmountInt64)),
                        std::move(dateAddExpr)));
    }


    void unsupportedExpression(const char* op) const {
        // We're guaranteed to not fire this assertion by implementing a mechanism in the upper
        // layer which directs the query to the classic engine when an unsupported expression
        // appears.
        tasserted(5182300, str::stream() << "Unsupported expression in SBE stage builder: " << op);
    }

private:
    void pushABT(abt::ABT abt) {
        _context->pushExpr(wrap(std::move(abt)));
    }

    ExpressionVisitorContext* _context;
};
}  // namespace

SbExpr generateExpression(StageBuilderState& state,
                          const Expression* expr,
                          boost::optional<SbSlot> rootSlot,
                          const PlanStageSlots& slots) {
    ExpressionVisitorContext context(state, std::move(rootSlot), slots);

    ExpressionPreVisitor preVisitor{&context};
    ExpressionInVisitor inVisitor{&context};
    ExpressionPostVisitor postVisitor{&context};
    ExpressionWalker walker{&preVisitor, &inVisitor, &postVisitor};
    expression_walker::walk<const Expression>(expr, &walker);

    return context.done();
}

SbExpr generateExpressionFieldPath(StageBuilderState& state,
                                   const FieldPath& fieldPath,
                                   boost::optional<Variables::Id> variableId,
                                   boost::optional<SbSlot> rootSlot,
                                   const PlanStageSlots& slots,
                                   std::map<Variables::Id, sbe::FrameId>* environment) {
    SbExprBuilder b(state);
    invariant(fieldPath.getPathLength() >= 1);

    boost::optional<SbSlot> topLevelFieldSlot;
    bool expectsDocumentInputOnly = false;
    auto fp = fieldPath.getPathLength() > 1 ? boost::make_optional(fieldPath.tail()) : boost::none;

    if (!variableId) {
        auto it = Variables::kBuiltinVarNameToId.find(fieldPath.front());

        if (it != Variables::kBuiltinVarNameToId.end()) {
            variableId.emplace(it->second);
        } else if (fieldPath.front() == "CURRENT"_sd) {
            variableId.emplace(Variables::kRootId);
        } else {
            tasserted(8859700,
                      str::stream() << "Expected variableId to be provided for user variable: '$$"
                                    << fieldPath.fullPath() << "'");
        }
    }

    auto varId = *variableId;

    SbExpr inputExpr;

    if (!Variables::isUserDefinedVariable(varId)) {
        if (varId == Variables::kRootId) {
            if (fp) {
                // Check if we already have a slot containing an expression corresponding
                // to 'expr'.
                auto fpe = std::make_pair(PlanStageSlots::kPathExpr, fp->fullPath());
                if (slots.has(fpe)) {
                    return SbExpr{slots.get(fpe)};
                }

                // Obtain a slot for the top-level field referred to by 'expr', if one
                // exists.
                auto topLevelField = std::make_pair(PlanStageSlots::kField, fp->front());
                topLevelFieldSlot = slots.getIfExists(topLevelField);
            }

            // Set inputExpr to refer to the root document.
            inputExpr = SbExpr{rootSlot};
            expectsDocumentInputOnly = true;
        } else if (varId == Variables::kRemoveId) {
            // For the field paths that begin with "$$REMOVE", we always produce Nothing,
            // so no traversal is necessary.
            return b.makeNothingConstant();
        } else {
            auto slot = state.getBuiltinVarSlot(varId);
            uassert(5611301,
                    str::stream() << "Builtin variable '$$" << fieldPath.fullPath()
                                  << "' (id=" << varId << ") is not available",
                    slot.has_value());

            inputExpr = SbExpr{SbSlot{*slot}};
        }
    } else {
        if (environment) {
            auto it = environment->find(varId);
            if (it != environment->end()) {
                inputExpr = b.makeVariable(it->second, 0);
            }
        }

        if (!inputExpr) {
            inputExpr = SbSlot{state.getGlobalVariableSlot(varId)};
        }
    }

    if (fieldPath.getPathLength() == 1) {
        tassert(6929400, "Expected a valid input expression", !inputExpr.isNull());

        // A solo variable reference (e.g.: "$$ROOT" or "$$myvar") that doesn't need any
        // traversal.
        return inputExpr;
    }

    tassert(6929401,
            "Expected a valid input expression or a valid field slot",
            !inputExpr.isNull() || topLevelFieldSlot.has_value());

    // Dereference a dotted path, which may contain arrays requiring implicit traversal.
    return generateTraverse(
        std::move(inputExpr), expectsDocumentInputOnly, *fp, state, topLevelFieldSlot);
}

SbExpr generateExpressionCompare(StageBuilderState& state,
                                 ExpressionCompare::CmpOp op,
                                 SbExpr lhs,
                                 SbExpr rhs) {
    SbExprBuilder b(state);

    auto frameId = state.frameIdGenerator->generate();
    auto lhsVar = SbLocalVar{frameId, 0};
    auto rhsVar = SbLocalVar{frameId, 1};

    auto binds = SbExpr::makeSeq(std::move(lhs), std::move(rhs));

    auto comparisonOperator = [op]() {
        switch (op) {
            case ExpressionCompare::CmpOp::EQ:
                return abt::Operations::Eq;
            case ExpressionCompare::CmpOp::NE:
                return abt::Operations::Neq;
            case ExpressionCompare::CmpOp::GT:
                return abt::Operations::Gt;
            case ExpressionCompare::CmpOp::GTE:
                return abt::Operations::Gte;
            case ExpressionCompare::CmpOp::LT:
                return abt::Operations::Lt;
            case ExpressionCompare::CmpOp::LTE:
                return abt::Operations::Lte;
            case ExpressionCompare::CmpOp::CMP:
                return abt::Operations::Cmp3w;
        }
        MONGO_UNREACHABLE;
    }();

    // We use the "cmp3w" primitive for every comparison, because it "type brackets" its
    // comparisons (for example, a number will always compare as less than a string). The
    // other comparison primitives are designed for comparing values of the same type.
    auto cmp3w = b.makeBinaryOp(abt::Operations::Cmp3w, lhsVar, rhsVar);

    auto cmp = (comparisonOperator == abt::Operations::Cmp3w)
        ? std::move(cmp3w)
        : b.makeBinaryOp(comparisonOperator, std::move(cmp3w), b.makeInt32Constant(0));

    // If either operand evaluates to "Nothing", then the entire operation expressed by
    // 'cmp' will also evaluate to "Nothing". MQL comparisons, however, treat "Nothing" as
    // if it is a value that is less than everything other than MinKey. (Notably, two
    // expressions that evaluate to "Nothing" are considered equal to each other.) We also
    // need to explicitly check for 'bsonUndefined' type because it is considered equal to
    // "Nothing" according to MQL semantics.
    auto generateExists = [&](SbLocalVar var) {
        auto undefinedTypeMask = static_cast<int32_t>(getBSONTypeMask(BSONType::Undefined));
        return b.makeBinaryOp(
            abt::Operations::And,
            b.makeFunction("exists"_sd, var),
            b.makeFunction("typeMatch"_sd, var, b.makeInt32Constant(~undefinedTypeMask)));
    };

    auto nothingFallbackCmp =
        b.makeBinaryOp(comparisonOperator, generateExists(lhsVar), generateExists(rhsVar));

    auto cmpWithFallback = b.makeFillEmpty(std::move(cmp), std::move(nothingFallbackCmp));

    return b.makeLet(frameId, std::move(binds), std::move(cmpWithFallback));
}
}  // namespace mongo::stage_builder
