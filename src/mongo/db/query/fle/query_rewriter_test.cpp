/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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


#include <memory>
#include <set>
#include <typeindex>
#include <vector>

#include "query_rewriter.h"
#include <absl/container/node_hash_map.h>
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/json.h"
#include "mongo/crypto/fle_crypto_types.h"
#include "mongo/crypto/fle_field_schema_gen.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/fle/encrypted_predicate.h"
#include "mongo/db/query/fle/encrypted_predicate_test_fixtures.h"
#include "mongo/db/query/fle/query_rewriter_interface.h"
#include "mongo/db/query/fle/server_rewrite_helper.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/overloaded_visitor.h"  // IWYU pragma: keep

namespace mongo {
namespace {

/*
 *  The server rewrite itself is only responsible for traversing agg and MatchExpressions and
 *  executing whatever rewrites are registered. For unit testing, we will only verify that this
 *  traversal and rewrite is happening properly using a mock predicate rewriter that rewrites any
 *  equality with an object with the key `encrypt` to a $gt operator. Unit tests for the actual
 *  rewrites while mocking out tag generation are located in the test file for each encrypted
 *  predicate type. Full end-to-end testing happens in jstests. This organization ensures that we
 *  don't write redundant tests that each index type is properly rewritten under different
 *  circumstances, when the same exact code is called for each index type.
 */

class MockPredicateRewriter : public fle::EncryptedPredicate {
public:
    MockPredicateRewriter(const fle::QueryRewriterInterface* rewriter)
        : EncryptedPredicate(rewriter) {}

protected:
    bool isPayload(const BSONElement& elt) const override {
        if (!elt.isABSONObj()) {
            return false;
        }
        return elt.Obj().hasField("encrypt"_sd);
    }
    bool isPayload(const Value& v) const override {
        if (!v.isObject()) {
            return false;
        }
        return !v.getDocument().getField("encrypt").missing();
    }

    std::vector<PrfBlock> generateTags(fle::BSONValue) const override {
        // In some cases, we may have an empty nss, which implies that the query rewriter was
        // instantiated for an unencrypted collection. This can only happen in an aggregate command
        // when FLE2 queries are using along with unencrypted collections in a $lookup.
        // In the unlikely event that a schema was not provided for a collection that required
        // predicate rewrites, we expect that an error will be thrown during tag generation as
        // QueryRewriter asserts this condition when getESCNss() is called.
        auto escNss = _rewriter->getESCNss();
        ASSERT_TRUE(!escNss.isEmpty());
        return {};
    };

    // Encrypted values will be rewritten from $eq to $gt. This is an arbitrary decision just to
    // make sure that the rewrite works properly.
    std::unique_ptr<MatchExpression> rewriteToTagDisjunction(MatchExpression* expr) const override {
        invariant(expr->matchType() == MatchExpression::EQ);
        auto eqMatch = static_cast<EqualityMatchExpression*>(expr);
        if (!isPayload(eqMatch->getData())) {
            return nullptr;
        }
        // We would like to closely simulate the calls that are made when rewriteToTagDisjunction is
        // called, which includes a call to generateTags(). Our mock always returns an empty tags
        // array.
        auto tags = generateTags(eqMatch->getData());
        ASSERT_TRUE(tags.empty());
        return std::make_unique<GTMatchExpression>(eqMatch->path(),
                                                   eqMatch->getData().Obj().firstElement());
    };

    std::unique_ptr<Expression> rewriteToTagDisjunction(Expression* expr) const override {
        auto eqMatch = dynamic_cast<ExpressionCompare*>(expr);
        invariant(eqMatch);
        // Only operate over equality comparisons.
        if (eqMatch->getOp() != ExpressionCompare::EQ) {
            return nullptr;
        }
        auto payload = dynamic_cast<ExpressionConstant*>(eqMatch->getOperandList()[1].get());
        // If the comparison doesn't hold a constant, then don't rewrite.
        if (!payload) {
            return nullptr;
        }

        // If the constant is not considered a payload, then don't rewrite.
        if (!isPayload(payload->getValue())) {
            return nullptr;
        }
        auto cmp = std::make_unique<ExpressionCompare>(eqMatch->getExpressionContext(),
                                                       ExpressionCompare::GT);
        cmp->addOperand(eqMatch->getOperandList()[0]);
        cmp->addOperand(
            ExpressionConstant::create(eqMatch->getExpressionContext(),
                                       payload->getValue().getDocument().getField("encrypt")));
        return cmp;
    }

    std::unique_ptr<MatchExpression> rewriteToRuntimeComparison(
        MatchExpression* expr) const override {
        return nullptr;
    }

    std::unique_ptr<Expression> rewriteToRuntimeComparison(Expression* expr) const override {
        return nullptr;
    }

private:
    // This method is not used in mock implementations of the EncryptedPredicate since isPayload(),
    // which normally calls encryptedBinDataType(), is overridden to look for plain objects rather
    // than BinData. Since this method is pure virtual on the superclass and needs to be
    // implemented, it is set to kPlaceholder (0).
    EncryptedBinDataType encryptedBinDataType() const override {
        return EncryptedBinDataType::kPlaceholder;
    }
};

// A second mock rewrite which replaces documents with the key "foo" into $lt operations. We need
// two different rewrites that are registered on the same operator to verify that all rewrites are
// iterated through.
class OtherMockPredicateRewriter : public fle::EncryptedPredicate {
public:
    OtherMockPredicateRewriter(const fle::QueryRewriterInterface* rewriter)
        : EncryptedPredicate(rewriter) {}

protected:
    bool isPayload(const BSONElement& elt) const override {
        if (!elt.isABSONObj()) {
            return false;
        }
        return elt.Obj().hasField("foo"_sd);
    }
    bool isPayload(const Value& v) const override {
        if (!v.isObject()) {
            return false;
        }
        return !v.getDocument().getField("foo").missing();
    }

    std::vector<PrfBlock> generateTags(fle::BSONValue payload) const override {
        return {};
    };

    // Encrypted values will be rewritten from $eq to $lt. This is an arbitrary decision just to
    // make sure that the rewrite works properly.
    std::unique_ptr<MatchExpression> rewriteToTagDisjunction(MatchExpression* expr) const override {
        invariant(expr->matchType() == MatchExpression::EQ);
        auto eqMatch = static_cast<EqualityMatchExpression*>(expr);
        if (!isPayload(eqMatch->getData())) {
            return nullptr;
        }
        return std::make_unique<LTMatchExpression>(eqMatch->path(),
                                                   eqMatch->getData().Obj().firstElement());
    };

    std::unique_ptr<Expression> rewriteToTagDisjunction(Expression* expr) const override {
        auto eqMatch = dynamic_cast<ExpressionCompare*>(expr);
        invariant(eqMatch);
        if (eqMatch->getOp() != ExpressionCompare::EQ) {
            return nullptr;
        }
        auto payload = dynamic_cast<ExpressionConstant*>(eqMatch->getOperandList()[1].get());
        if (!payload) {
            return nullptr;
        }

        if (!isPayload(payload->getValue())) {
            return nullptr;
        }
        auto cmp = std::make_unique<ExpressionCompare>(eqMatch->getExpressionContext(),
                                                       ExpressionCompare::LT);
        cmp->addOperand(eqMatch->getOperandList()[0]);
        cmp->addOperand(ExpressionConstant::create(
            eqMatch->getExpressionContext(), payload->getValue().getDocument().getField("foo")));
        return cmp;
    }

    std::unique_ptr<MatchExpression> rewriteToRuntimeComparison(
        MatchExpression* expr) const override {
        return nullptr;
    }

    std::unique_ptr<Expression> rewriteToRuntimeComparison(Expression* expr) const override {
        return nullptr;
    }

private:
    EncryptedBinDataType encryptedBinDataType() const override {
        return EncryptedBinDataType::kPlaceholder;
    }
};

// A mock rewriter for text search predicates which will rewrite the string 'original' to
// 'rewritten' as an arbitrary choice to show rewrites work properly.
class EncTextSearchPredicateRewriter : public fle::EncryptedPredicate {
public:
    EncTextSearchPredicateRewriter(const fle::QueryRewriterInterface* rewriter)
        : EncryptedPredicate(rewriter) {}

    static inline const std::string kPayloadText{"original"};
    static inline const std::string kRewrittenText{"rewritten"};

protected:
    bool isPayload(const BSONElement& elt) const override {
        tasserted(10184100,
                  "Encrypted text search predicates are only supported as aggregation expressions, "
                  "so we will only check isPayload() on a Value.");
    }

    bool isPayload(const Value& v) const override {
        // For this test we are only considering the string which equals payloadText as a payload.
        return v.getString() == kPayloadText;
    }

    std::vector<PrfBlock> generateTags(fle::BSONValue payload) const override {
        return {};
    };

    std::unique_ptr<MatchExpression> rewriteToTagDisjunction(MatchExpression* expr) const override {
        tasserted(
            10184101,
            "Encrypted text search predicates are only supported as aggregation expressions.");
    };

    std::unique_ptr<Expression> rewriteToTagDisjunction(Expression* expr) const override {
        if (auto encStrStartsWithExpr = dynamic_cast<ExpressionEncStrStartsWith*>(expr);
            encStrStartsWithExpr) {
            if (!isPayload(encStrStartsWithExpr->getText().getValue())) {
                return nullptr;
            }

            auto expCtx = encStrStartsWithExpr->getExpressionContext();
            auto textExpr = make_intrusive<ExpressionConstant>(expCtx, Value(kRewrittenText));

            // Return a new ExpressionEncStrStartsWith with the same input, and rewritten prefix
            // string.
            return std::make_unique<ExpressionEncStrStartsWith>(
                expCtx, std::move(encStrStartsWithExpr->getChildren()[0]), std::move(textExpr));
        } else if (auto encStrEndsWithExpr = dynamic_cast<ExpressionEncStrEndsWith*>(expr);
                   encStrEndsWithExpr) {
            if (!isPayload(encStrEndsWithExpr->getText().getValue())) {
                return nullptr;
            }

            auto expCtx = encStrEndsWithExpr->getExpressionContext();
            auto textExpr = make_intrusive<ExpressionConstant>(expCtx, Value(kRewrittenText));

            // Return a new ExpressionEncStrEndsWithExpr with the same input, and rewritten suffix
            // string.
            return std::make_unique<ExpressionEncStrEndsWith>(
                expCtx, std::move(encStrEndsWithExpr->getChildren()[0]), std::move(textExpr));
        } else if (auto encStrContainsExpr = dynamic_cast<ExpressionEncStrContains*>(expr);
                   encStrContainsExpr) {
            if (!isPayload(encStrContainsExpr->getText().getValue())) {
                return nullptr;
            }

            auto expCtx = encStrContainsExpr->getExpressionContext();
            auto textExpr = make_intrusive<ExpressionConstant>(expCtx, Value(kRewrittenText));

            // Return a new ExpressionEncStrContainsExpr with the same input, and rewritten
            // substring string.
            return std::make_unique<ExpressionEncStrContains>(
                expCtx, std::move(encStrContainsExpr->getChildren()[0]), std::move(textExpr));
        } else if (auto encStrNormalizedEqExpr = dynamic_cast<ExpressionEncStrNormalizedEq*>(expr);
                   encStrNormalizedEqExpr) {
            if (!isPayload(encStrNormalizedEqExpr->getText().getValue())) {
                return nullptr;
            }

            auto expCtx = encStrNormalizedEqExpr->getExpressionContext();
            auto textExpr = make_intrusive<ExpressionConstant>(expCtx, Value(kRewrittenText));

            // Return a new ExpressionEncStrNormalizedEqExpr with the same input, and rewritten
            // target string.
            return std::make_unique<ExpressionEncStrNormalizedEq>(
                expCtx, std::move(encStrNormalizedEqExpr->getChildren()[0]), std::move(textExpr));
        }
        MONGO_UNREACHABLE_TASSERT(10184102);
    }

    std::unique_ptr<MatchExpression> rewriteToRuntimeComparison(
        MatchExpression* expr) const override {
        tasserted(
            10184103,
            "Encrypted text search predicates are only supported as aggregation expressions.");
    }

    std::unique_ptr<Expression> rewriteToRuntimeComparison(Expression* expr) const override {
        return nullptr;
    }

private:
    EncryptedBinDataType encryptedBinDataType() const override {
        return EncryptedBinDataType::kPlaceholder;
    }
};

// Define the mock match and agg rewrite maps to be used by the unit tests.
static const fle::MatchTypeToRewriteMap matchRewriteMap{
    {MatchExpression::EQ,
     {[](auto* rewriter, auto* expr) { return MockPredicateRewriter{rewriter}.rewrite(expr); },
      [](auto* rewriter, auto* expr) {
          return OtherMockPredicateRewriter{rewriter}.rewrite(expr);
      }}}};

static const fle::ExpressionToRewriteMap aggRewriteMap{
    {typeid(ExpressionCompare),
     {[](auto* rewriter, auto* expr) { return MockPredicateRewriter{rewriter}.rewrite(expr); },
      [](auto* rewriter, auto* expr) {
          return OtherMockPredicateRewriter{rewriter}.rewrite(expr);
      }}},
    {typeid(ExpressionEncStrStartsWith), {[](auto* rewriter, auto* expr) {
         return EncTextSearchPredicateRewriter{rewriter}.rewrite(expr);
     }}},
    {typeid(ExpressionEncStrEndsWith), {[](auto* rewriter, auto* expr) {
         return EncTextSearchPredicateRewriter{rewriter}.rewrite(expr);
     }}},
    {typeid(ExpressionEncStrContains), {[](auto* rewriter, auto* expr) {
         return EncTextSearchPredicateRewriter{rewriter}.rewrite(expr);
     }}},
    {typeid(ExpressionEncStrNormalizedEq), {[](auto* rewriter, auto* expr) {
         return EncTextSearchPredicateRewriter{rewriter}.rewrite(expr);
     }}}};


class MockQueryRewriter : public fle::QueryRewriter {
public:
    MockQueryRewriter(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                      const NamespaceString& mockNss,
                      const std::map<NamespaceString, NamespaceString>& escMap)
        : fle::QueryRewriter(expCtx, mockNss, aggRewriteMap, matchRewriteMap, escMap) {}

    BSONObj rewriteMatchExpressionForTest(const BSONObj& obj) {
        auto res = rewriteMatchExpression(obj);
        return res ? res.value() : obj;
    }

    BSONObj rewriteAggExpressionForTest(const BSONObj& obj) {
        tassert(9775503, "Invalid expression context", getExpressionContext());
        auto expr = Expression::parseExpression(
            getExpressionContext(), obj, getExpressionContext()->variablesParseState);
        auto result = rewriteExpression(expr.get());
        return result ? result->serialize().getDocument().toBson()
                      : expr->serialize().getDocument().toBson();
    }

    static fle::QueryRewriter getQueryRewriterWithMockedMaps(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const NamespaceString& nss,
        const std::map<NamespaceString, NamespaceString>& escMap) {
        // Workaround for protected fle::QueryRewriter constructor. Slices the mocked object,
        // leaving us with the copied base class with mocked maps.
        return MockQueryRewriter(expCtx, nss, escMap);
    }

private:
    fle::TagMap _tags;
    std::set<StringData> _encryptedFields;
};

class FLEServerRewriteTest : public unittest::Test {
public:
    FLEServerRewriteTest() : _mock(nullptr) {}

    void setUp() override {
        fle::registerEncTextSearchExpressions();
        _mock = std::make_unique<MockQueryRewriter>(_expCtx, _mockNss, _mockEscMap);
    }

    void tearDown() override {}

protected:
    std::unique_ptr<MockQueryRewriter> _mock;
    boost::intrusive_ptr<ExpressionContext> _expCtx{new ExpressionContextForTest()};
    NamespaceString _mockNss = NamespaceString::createNamespaceString_forTest("test.mock"_sd);
    std::map<NamespaceString, NamespaceString> _mockEscMap{{_mockNss, _mockNss}};
};

#define ASSERT_MATCH_EXPRESSION_REWRITE(input, expected)                 \
    auto actual = _mock->rewriteMatchExpressionForTest(fromjson(input)); \
    ASSERT_BSONOBJ_EQ(actual, fromjson(expected));

#define TEST_FLE_REWRITE_MATCH(name, input, expected)      \
    TEST_F(FLEServerRewriteTest, name##_MatchExpression) { \
        ASSERT_MATCH_EXPRESSION_REWRITE(input, expected);  \
    }

#define ASSERT_AGG_EXPRESSION_REWRITE(input, expected)                 \
    auto actual = _mock->rewriteAggExpressionForTest(fromjson(input)); \
    ASSERT_BSONOBJ_EQ(actual, fromjson(expected));

#define TEST_FLE_REWRITE_AGG(name, input, expected)      \
    TEST_F(FLEServerRewriteTest, name##_AggExpression) { \
        ASSERT_AGG_EXPRESSION_REWRITE(input, expected);  \
    }

TEST_FLE_REWRITE_MATCH(TopLevel_DottedPath,
                       "{'user.ssn': {$eq: {encrypt: 2}}}",
                       "{'user.ssn': {$gt: 2}}");

TEST_FLE_REWRITE_AGG(TopLevel_DottedPath,
                     "{$eq: ['$user.ssn', {$const: {encrypt: 2}}]}",
                     "{$gt: ['$user.ssn', {$const: 2}]}");

TEST_FLE_REWRITE_MATCH(TopLevel_Conjunction_BothEncrypted,
                       "{$and: [{ssn: {encrypt: 2}}, {age: {encrypt: 4}}]}",
                       "{$and: [{ssn: {$gt: 2}}, {age: {$gt: 4}}]}");

TEST_FLE_REWRITE_AGG(
    TopLevel_Conjunction_BothEncrypted,
    "{$and: [{$eq: ['$user.ssn', {$const: {encrypt: 2}}]}, {$eq: ['$age', {$const: {encrypt: "
    "4}}]}]}",
    "{$and: [{$gt: ['$user.ssn', {$const: 2}]}, {$gt: ['$age', {$const: 4}]}]}");

TEST_FLE_REWRITE_MATCH(TopLevel_Conjunction_PartlyEncrypted,
                       "{$and: [{ssn: {encrypt: 2}}, {age: {plain: 4}}]}",
                       "{$and: [{ssn: {$gt: 2}}, {age: {$eq: {plain: 4}}}]}");

TEST_FLE_REWRITE_AGG(
    TopLevel_Conjunction_PartlyEncrypted,
    "{$and: [{$eq: ['$user.ssn', {$const: {encrypt: 2}}]}, {$eq: ['$age', {$const: {plain: 4}}]}]}",
    "{$and: [{$gt: ['$user.ssn', {$const: 2}]}, {$eq: ['$age', {$const: {plain: 4}}]}]}");

TEST_FLE_REWRITE_MATCH(TopLevel_Conjunction_PartlyEncryptedWithUnregisteredOperator,
                       "{$and: [{ssn: {encrypt: 2}}, {age: {$lt: {encrypt: 4}}}]}",
                       "{$and: [{ssn: {$gt: 2}}, {age: {$lt: {encrypt: 4}}}]}");

TEST_FLE_REWRITE_AGG(
    TopLevel_Conjunction_PartlyEncryptedWithUnregisteredOperator,
    "{$and: [{$eq: ['$user.ssn', {$const: {encrypt: 2}}]}, {$lt: ['$age', {$const: {encrypt: "
    "4}}]}]}",
    "{$and: [{$gt: ['$user.ssn', {$const: 2}]}, {$lt: ['$age', {$const: {encrypt: "
    "4}}]}]}");

TEST_FLE_REWRITE_MATCH(TopLevel_Encrypted_Nested_Unecrypted,
                       "{$and: [{ssn: {encrypt: 2}}, {user: {region: 'US'}}]}",
                       "{$and: [{ssn: {$gt: 2}}, {user: {$eq: {region: 'US'}}}]}");

TEST_FLE_REWRITE_MATCH(TopLevel_Not,
                       "{ssn: {$not: {$eq: {encrypt: 5}}}}",
                       "{ssn: {$not: {$gt: 5}}}");

TEST_FLE_REWRITE_AGG(TopLevel_Not,
                     "{$not: [{$eq: ['$ssn', {$const: {encrypt: 2}}]}]}",
                     "{$not: [{$gt: ['$ssn', {$const: 2}]}]}")

TEST_FLE_REWRITE_MATCH(TopLevel_Neq, "{ssn: {$ne: {encrypt: 5}}}", "{ssn: {$not: {$gt: 5}}}");

TEST_FLE_REWRITE_MATCH(
    NestedConjunction,
    "{$and: [{$and: [{ssn: {encrypt: 2}}, {other: 'field'}]}, {otherSsn: {encrypt: 3}}]}",
    "{$and: [{$and: [{ssn: {$gt: 2}}, {other: {$eq: 'field'}}]}, {otherSsn: {$gt: 3}}]}");

TEST_FLE_REWRITE_AGG(NestedConjunction,
                     "{$and: [{$and: [{$eq: ['$ssn', {$const: {encrypt: 2}}]},{$eq: ['$other', "
                     "'field']}]},{$eq: ['$age',{$const: {encrypt: 4}}]}]}",
                     "{$and: [{$and: [{$gt: ['$ssn', {$const: 2}]},{$eq: ['$other', "
                     "{$const: 'field'}]}]},{$gt: ['$age',{$const: 4}]}]}");

TEST_FLE_REWRITE_MATCH(TopLevel_Nor,
                       "{$nor: [{ssn: {encrypt: 5}}, {other: {$eq: 'field'}}]}",
                       "{$nor: [{ssn: {$gt: 5}}, {other: {$eq: 'field'}}]}");

TEST_FLE_REWRITE_MATCH(TopLevel_Or,
                       "{$or: [{ssn: {encrypt: 5}}, {other: {$eq: 'field'}}]}",
                       "{$or: [{ssn: {$gt: 5}}, {other: {$eq: 'field'}}]}");

TEST_FLE_REWRITE_AGG(
    TopLevel_Or,
    "{$or: [{$eq: ['$ssn', {$const: {encrypt: 2}}]}, {$eq: ['$ssn', {$const: {encrypt: 4}}]}]}",
    "{$or: [{$gt: ['$ssn', {$const: 2}]}, {$gt: ['$ssn', {$const: 4}]}]}")

TEST_FLE_REWRITE_AGG(Basic_FleEncStrStartsWith,
                     "{$encStrStartsWith: {input: '$ssn', prefix: 'original'}}",
                     "{$encStrStartsWith: {input: '$ssn', prefix: {$const: 'rewritten'}}}")

TEST_FLE_REWRITE_AGG(
    Nested_FleEncStrStartsWith_FullyRewrite,
    "{$or: [{$encStrStartsWith: {input: '$ssn', prefix: 'original'}}, {$encStrStartsWith: {input: "
    "'$otherSsn', prefix: 'original'}}]}",
    "{$or: [{$encStrStartsWith: {input: '$ssn', prefix: {$const: 'rewritten'}}}, "
    "{$encStrStartsWith: {input: '$otherSsn', prefix: {$const: 'rewritten'}}}]}")

TEST_FLE_REWRITE_AGG(
    Nested_FleEncStrStartsWith_PartiallyRewrite,
    "{$and: [{$encStrStartsWith: {input: '$ssn', prefix: 'original'}}, {$encStrStartsWith: {input: "
    "'$otherSsn', prefix: 'other'}}]}",
    "{$and: [{$encStrStartsWith: {input: '$ssn', prefix: {$const: 'rewritten'}}}, "
    "{$encStrStartsWith: {input: '$otherSsn', prefix: {$const: 'other'}}}]}")

TEST_FLE_REWRITE_MATCH(
    Match_FleEncStrStartsWith_Expr,
    "{$expr: {$encStrStartsWith: {input: '$ssn', prefix: 'original'}}}",
    "{$expr: {$encStrStartsWith: {input: '$ssn', prefix: {$const: 'rewritten'}}}}");

TEST_FLE_REWRITE_MATCH(
    Match_Nested_FleEncStrStartsWith_Expr,
    "{$and: [{$expr: {$encStrStartsWith: {input: '$ssn', prefix: 'original'}}}, {$expr: "
    "{$encStrStartsWith: {input: "
    "'$otherSsn', prefix: 'other'}}}]}",
    "{$and: [{$expr: {$encStrStartsWith: {input: '$ssn', prefix: {$const: 'rewritten'}}}}, "
    "{$expr: {$encStrStartsWith: {input: '$otherSsn', prefix: {$const: 'other'}}}}]}")

TEST_FLE_REWRITE_AGG(Basic_FleEncStrEndsWith,
                     "{$encStrEndsWith: {input: '$ssn', suffix: 'original'}}",
                     "{$encStrEndsWith: {input: '$ssn', suffix: {$const: 'rewritten'}}}")

TEST_FLE_REWRITE_AGG(
    Nested_FleEncStrEndsWith_FullyRewrite,
    "{$or: [{$encStrEndsWith: {input: '$ssn', suffix: 'original'}}, {$encStrEndsWith: {input: "
    "'$otherSsn', suffix: 'original'}}]}",
    "{$or: [{$encStrEndsWith: {input: '$ssn', suffix: {$const: 'rewritten'}}}, "
    "{$encStrEndsWith: {input: '$otherSsn', suffix: {$const: 'rewritten'}}}]}")

TEST_FLE_REWRITE_AGG(
    Nested_FleEncStrEndsWith_PartiallyRewrite,
    "{$and: [{$encStrEndsWith: {input: '$ssn', suffix: 'original'}}, {$encStrEndsWith: {input: "
    "'$otherSsn', suffix: 'other'}}]}",
    "{$and: [{$encStrEndsWith: {input: '$ssn', suffix: {$const: 'rewritten'}}}, "
    "{$encStrEndsWith: {input: '$otherSsn', suffix: {$const: 'other'}}}]}")

TEST_FLE_REWRITE_MATCH(
    Match_FleEncStrEndsWith_Expr,
    "{$expr: {$encStrEndsWith: {input: '$ssn', suffix: 'original'}}}",
    "{$expr: {$encStrEndsWith: {input: '$ssn', suffix: {$const: 'rewritten'}}}}");

TEST_FLE_REWRITE_MATCH(
    Match_Nested_FleEncStrEndsWith_Expr,
    "{$and: [{$expr: {$encStrEndsWith: {input: '$ssn', suffix: 'original'}}}, {$expr: "
    "{$encStrEndsWith: {input: "
    "'$otherSsn', suffix: 'other'}}}]}",
    "{$and: [{$expr: {$encStrEndsWith: {input: '$ssn', suffix: {$const: 'rewritten'}}}}, "
    "{$expr: {$encStrEndsWith: {input: '$otherSsn', suffix: {$const: 'other'}}}}]}")

TEST_FLE_REWRITE_AGG(Basic_FleEncStrContains,
                     "{$encStrContains: {input: '$ssn', substring: 'original'}}",
                     "{$encStrContains: {input: '$ssn', substring: {$const: 'rewritten'}}}")

TEST_FLE_REWRITE_AGG(
    Nested_FleEncStrContains_FullyRewrite,
    "{$or: [{$encStrContains: {input: '$ssn', substring: 'original'}}, {$encStrContains: {input: "
    "'$otherSsn', substring: 'original'}}]}",
    "{$or: [{$encStrContains: {input: '$ssn', substring: {$const: 'rewritten'}}}, "
    "{$encStrContains: {input: '$otherSsn', substring: {$const: 'rewritten'}}}]}")

TEST_FLE_REWRITE_AGG(
    Nested_FleEncStrContains_PartiallyRewrite,
    "{$and: [{$encStrContains: {input: '$ssn', substring: 'original'}}, {$encStrContains: {input: "
    "'$otherSsn', substring: 'other'}}]}",
    "{$and: [{$encStrContains: {input: '$ssn', substring: {$const: 'rewritten'}}}, "
    "{$encStrContains: {input: '$otherSsn', substring: {$const: 'other'}}}]}")

TEST_FLE_REWRITE_MATCH(
    Match_FleEncStrContains_Expr,
    "{$expr: {$encStrContains: {input: '$ssn', substring: 'original'}}}",
    "{$expr: {$encStrContains: {input: '$ssn', substring: {$const: 'rewritten'}}}}");

TEST_FLE_REWRITE_MATCH(
    Match_Nested_FleEncStrContains_Expr,
    "{$and: [{$expr: {$encStrContains: {input: '$ssn', substring: 'original'}}}, {$expr: "
    "{$encStrContains: {input: "
    "'$otherSsn', substring: 'other'}}}]}",
    "{$and: [{$expr: {$encStrContains: {input: '$ssn', substring: {$const: 'rewritten'}}}}, "
    "{$expr: {$encStrContains: {input: '$otherSsn', substring: {$const: 'other'}}}}]}")

TEST_FLE_REWRITE_AGG(Basic_FleEncStrNormalizedEq,
                     "{$encStrNormalizedEq: {input: '$ssn', string: 'original'}}",
                     "{$encStrNormalizedEq: {input: '$ssn', string: {$const: 'rewritten'}}}")

TEST_FLE_REWRITE_AGG(
    Nested_FleEncStrNormalizedEq_FullyRewrite,
    "{$or: [{$encStrNormalizedEq: {input: '$ssn', string: 'original'}}, {$encStrNormalizedEq: "
    "{input: "
    "'$otherSsn', string: 'original'}}]}",
    "{$or: [{$encStrNormalizedEq: {input: '$ssn', string: {$const: 'rewritten'}}}, "
    "{$encStrNormalizedEq: {input: '$otherSsn', string: {$const: 'rewritten'}}}]}")

TEST_FLE_REWRITE_AGG(
    Nested_FleEncStrNormalizedEq_PartiallyRewrite,
    "{$and: [{$encStrNormalizedEq: {input: '$ssn', string: 'original'}}, {$encStrNormalizedEq: "
    "{input: "
    "'$otherSsn', string: 'other'}}]}",
    "{$and: [{$encStrNormalizedEq: {input: '$ssn', string: {$const: 'rewritten'}}}, "
    "{$encStrNormalizedEq: {input: '$otherSsn', string: {$const: 'other'}}}]}")

TEST_FLE_REWRITE_MATCH(
    Match_FleEncStrNormalizedEq_Expr,
    "{$expr: {$encStrNormalizedEq: {input: '$ssn', string: 'original'}}}",
    "{$expr: {$encStrNormalizedEq: {input: '$ssn', string: {$const: 'rewritten'}}}}");

TEST_FLE_REWRITE_MATCH(
    Match_Nested_FleEncStrNormalizedEq_Expr,
    "{$and: [{$expr: {$encStrNormalizedEq: {input: '$ssn', string: 'original'}}}, {$expr: "
    "{$encStrNormalizedEq: {input: "
    "'$otherSsn', string: 'other'}}}]}",
    "{$and: [{$expr: {$encStrNormalizedEq: {input: '$ssn', string: {$const: 'rewritten'}}}}, "
    "{$expr: {$encStrNormalizedEq: {input: '$otherSsn', string: {$const: 'other'}}}}]}")

TEST_FLE_REWRITE_MATCH(
    Match_FleEncStrCombined_Exprs_FullyRewrite,
    "{$or: [{$expr: {$encStrStartsWith: {input: '$ssn', prefix: 'original'}}}, {$expr: "
    "{$encStrEndsWith: {input: "
    "'$otherSsn', suffix: 'original'}}}, {$expr: {$encStrContains: {input: '$ssn', substring: "
    "'original'}}}, {$expr: {$encStrNormalizedEq: {input: '$ssn', string: 'original'}}}]}",
    "{$or: [{$expr: {$encStrStartsWith: {input: '$ssn', prefix: {$const: 'rewritten'}}}}, "
    "{$expr: {$encStrEndsWith: {input: '$otherSsn', suffix: {$const: 'rewritten'}}}}, {$expr: "
    "{$encStrContains: {input: '$ssn', substring: {$const: 'rewritten'}}}}, {$expr: "
    "{$encStrNormalizedEq: {input: '$ssn', string: {$const: 'rewritten'}}}}]}")

TEST_FLE_REWRITE_MATCH(
    Match_FleEncStrCombined_Exprs_PartiallyRewrite,
    "{$and: [{$expr: {$encStrStartsWith: {input: '$ssn', prefix: 'original'}}}, {$expr: "
    "{$encStrEndsWith: {input: "
    "'$otherSsn', suffix: 'other'}}}, {$expr: {$encStrContains: {input: '$ssn', substring: "
    "'original'}}}, {$expr: {$encStrNormalizedEq: {input: '$ssn', string: "
    "'other'}}}]}",
    "{$and: [{$expr: {$encStrStartsWith: {input: '$ssn', prefix: {$const: 'rewritten'}}}}, "
    "{$expr: {$encStrEndsWith: {input: '$otherSsn', suffix: {$const: 'other'}}}}, {$expr: "
    "{$encStrContains: {input: '$ssn', substring: {$const: 'rewritten'}}}}, {$expr: "
    "{$encStrNormalizedEq: {input: '$ssn', string: {$const: 'other'}}}}]}")

// Test that the rewriter will work from any rewrite registered to an expression. The test rewriter
// has two rewrites registered on $eq.

TEST_FLE_REWRITE_MATCH(OtherRewrite_Basic, "{'ssn': {$eq: {foo: 2}}}", "{'ssn': {$lt: 2}}");

TEST_FLE_REWRITE_AGG(OtherRewrite_Basic,
                     "{$eq: ['$user.ssn', {$const: {foo: 2}}]}",
                     "{$lt: ['$user.ssn', {$const: 2}]}");

TEST_FLE_REWRITE_MATCH(OtherRewrite_Conjunction_BothEncrypted,
                       "{$and: [{ssn: {encrypt: 2}}, {age: {foo: 4}}]}",
                       "{$and: [{ssn: {$gt: 2}}, {age: {$lt: 4}}]}");

TEST_FLE_REWRITE_AGG(
    OtherRewrite_Conjunction_BothEncrypted,
    "{$and: [{$eq: ['$user.ssn', {$const: {encrypt: 2}}]}, {$eq: ['$age', {$const: {foo: "
    "4}}]}]}",
    "{$and: [{$gt: ['$user.ssn', {$const: 2}]}, {$lt: ['$age', {$const: 4}]}]}");

class MockPipelineRewrite : public fle::PipelineRewrite {
public:
    MockPipelineRewrite(const NamespaceString& nss,
                        const EncryptionInformation& encryptInfo,
                        std::unique_ptr<Pipeline, PipelineDeleter> toRewrite)
        : PipelineRewrite(nss, encryptInfo, std::move(toRewrite)) {}

    ~MockPipelineRewrite() override {};

protected:
    fle::QueryRewriter getQueryRewriterForEsc(FLETagQueryInterface* queryImpl) override {
        return MockQueryRewriter::getQueryRewriterWithMockedMaps(expCtx, nssEsc, _escMap);
    }
};

static inline void assertPipelinesSame(const std::vector<BSONObj>& expectedPipeline,
                                       const std::vector<BSONObj>& actualPipeline) {
    ASSERT_EQUALS(expectedPipeline.size(), actualPipeline.size());

    auto flags =
        BSONObj::ComparisonRules::kIgnoreFieldOrder | BSONObj::ComparisonRules::kConsiderFieldName;
    for (size_t i = 0; i < actualPipeline.size(); ++i) {
        ASSERT_TRUE(actualPipeline[i].woCompare(expectedPipeline[i], {}, flags) == 0);
    }
}

class FLEServerRewritePipelineTest : public unittest::Test {
public:
    static constexpr auto kSingleEncryptionSchemaEncryptionInfo = R"({
                    "type": 1,
                    "schema":{
                        "test.coll_a": {
                            "escCollection": "enxcol_.coll_a.esc",
                            "ecocCollection": "enxcol_.coll_a.ecoc",
                            "fields": [
                                {
                                    "keyId": {
                                        "$uuid": "12345678-1234-9876-1234-123456789012"
                                    },
                                    "path": "ssn",
                                    "bsonType": "string",
                                    "queries": {
                                        "queryType": "equality"
                                    }
                                },
                                {
                                    "keyId": {
                                        "$uuid": "12345678-1234-9876-1234-123456789013"
                                    },
                                    "path": "age",
                                    "bsonType": "int",
                                    "queries": {
                                        "queryType": "equality"
                                    }
                                }
                            ]
                        }
                    }
                })";
    static constexpr auto kTwoEncryptionSchemaEncryptionInfo = R"({
                    "type": 1,
                    "schema":{
                        "test.coll_a": {
                            "escCollection": "enxcol_.coll_a.esc",
                            "ecocCollection": "enxcol_.coll_a.ecoc",
                            "fields": [
                                {
                                    "keyId": {
                                        "$uuid": "12345678-1234-9876-1234-123456789012"
                                    },
                                    "path": "ssn",
                                    "bsonType": "string",
                                    "queries": {
                                        "queryType": "equality"
                                    }
                                },
                                {
                                    "keyId": {
                                        "$uuid": "12345678-1234-9876-1234-123456789013"
                                    },
                                    "path": "age",
                                    "bsonType": "int",
                                    "queries": {
                                        "queryType": "equality"
                                    }
                                }
                            ]
                        },
                        "test.coll_b": {
                            "escCollection": "enxcol_.coll_b.esc",
                            "ecocCollection": "enxcol_.coll_b.ecoc",
                            "fields": [
                                {
                                    "keyId": {
                                        "$uuid": "12345678-1234-9876-1234-123456789014"
                                    },
                                    "path": "b_ssn",
                                    "bsonType": "string",
                                    "queries": {
                                        "queryType": "equality"
                                    }
                                },
                                {
                                    "keyId": {
                                        "$uuid": "12345678-1234-9876-1234-123456789015"
                                    },
                                    "path": "b_age",
                                    "bsonType": "int",
                                    "queries": {
                                        "queryType": "equality"
                                    }
                                }
                            ]
                        }
                    }
                })";
    static constexpr auto kThreeEncryptionSchemaEncryptionInfo = R"({
                    "type": 1,
                    "schema":{
                        "test.coll_a": {
                            "escCollection": "enxcol_.coll_a.esc",
                            "ecocCollection": "enxcol_.coll_a.ecoc",
                            "fields": [
                                {
                                    "keyId": {
                                        "$uuid": "12345678-1234-9876-1234-123456789012"
                                    },
                                    "path": "ssn",
                                    "bsonType": "string",
                                    "queries": {
                                        "queryType": "equality"
                                    }
                                },
                                {
                                    "keyId": {
                                        "$uuid": "12345678-1234-9876-1234-123456789013"
                                    },
                                    "path": "age",
                                    "bsonType": "int",
                                    "queries": {
                                        "queryType": "equality"
                                    }
                                }
                            ]
                        },
                        "test.coll_b": {
                            "escCollection": "enxcol_.coll_b.esc",
                            "ecocCollection": "enxcol_.coll_b.ecoc",
                            "fields": [
                                {
                                    "keyId": {
                                        "$uuid": "12345678-1234-9876-1234-123456789014"
                                    },
                                    "path": "b_ssn",
                                    "bsonType": "string",
                                    "queries": {
                                        "queryType": "equality"
                                    }
                                },
                                {
                                    "keyId": {
                                        "$uuid": "12345678-1234-9876-1234-123456789015"
                                    },
                                    "path": "b_age",
                                    "bsonType": "int",
                                    "queries": {
                                        "queryType": "equality"
                                    }
                                }
                            ]
                        },
                        "test.coll_c": {
                            "escCollection": "enxcol_.coll_c.esc",
                            "ecocCollection": "enxcol_.coll_c.ecoc",
                            "fields": [
                                {
                                    "keyId": {
                                        "$uuid": "12345678-1234-9876-1234-123456789016"
                                    },
                                    "path": "c_ssn",
                                    "bsonType": "string",
                                    "queries": {
                                        "queryType": "equality"
                                    }
                                },
                                {
                                    "keyId": {
                                        "$uuid": "12345678-1234-9876-1234-123456789017"
                                    },
                                    "path": "c_age",
                                    "bsonType": "int",
                                    "queries": {
                                        "queryType": "equality"
                                    }
                                }
                            ]
                        }
                    }
                })";

    // This schema map does not contain the schema for the primary collection used in this test.
    static constexpr auto kSingleEncryptionSchemaEncryptionCollD = R"({
        "type": 1,
        "schema":{
            "test.coll_d": {
                "escCollection": "enxcol_.coll_d.esc",
                "ecocCollection": "enxcol_.coll_d.ecoc",
                "fields": [
                    {
                        "keyId": {
                            "$uuid": "12345678-1234-9876-1234-123456789012"
                        },
                        "path": "d_ssn",
                        "bsonType": "string",
                        "queries": {
                            "queryType": "equality"
                        }
                    },
                    {
                        "keyId": {
                            "$uuid": "12345678-1234-9876-1234-123456789013"
                        },
                        "path": "d_age",
                        "bsonType": "int",
                        "queries": {
                            "queryType": "equality"
                        }
                    }
                ]
            }
        }
    })";

    void setResolvedNamespacesForTest(const std::vector<NamespaceString>& additionalNs) {
        ResolvedNamespaceMap resolvedNs;
        resolvedNs.insert_or_assign(_primaryNss, {_primaryNss, std::vector<BSONObj>{}});
        for (auto&& ns : additionalNs) {
            resolvedNs.insert_or_assign(ns, {ns, std::vector<BSONObj>{}});
        }
        _expCtx->setResolvedNamespaces(std::move(resolvedNs));
    }

    auto jsonToPipeline(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                        const NamespaceString& nss,
                        StringData jsonArray) {
        const auto inputBson = fromjson("{pipeline: " + jsonArray + "}");

        ASSERT_EQUALS(inputBson["pipeline"].type(), BSONType::Array);
        auto rawPipeline = parsePipelineFromBSON(inputBson["pipeline"]);
        expCtx->setNamespaceString(nss);
        return Pipeline::parse(rawPipeline, expCtx);
    }

    void assertExpectedPipeline(const Pipeline& rewrittenPipeline,
                                const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                const NamespaceString& nss,
                                StringData expectedPipelineJsonArray) {
        SerializationOptions opts{.serializeForFLE2 = true};
        auto serializedRewrittenPipeline = rewrittenPipeline.serializeToBson(opts);
        auto serializedExpectedPipeline =
            jsonToPipeline(expCtx, nss, expectedPipelineJsonArray)->serializeToBson(opts);
        assertPipelinesSame(serializedExpectedPipeline, serializedRewrittenPipeline);
    }

protected:
    boost::intrusive_ptr<ExpressionContext> _expCtx{new ExpressionContextForTest()};
    NamespaceString _primaryNss = NamespaceString::createNamespaceString_forTest("test.coll_a"_sd);
};

#define TEST_FLE_REWRITE_PIPELINE(name,                                                        \
                                  input,                                                       \
                                  expected,                                                    \
                                  additionalNamespaces,                                        \
                                  encryptionInformation,                                       \
                                  enableMultiSchemaFeatureFlag)                                \
    TEST_F(FLEServerRewritePipelineTest, name##_PipelineRewrite) {                             \
        RAIIServerParameterControllerForTest _scopedFeature{                                   \
            "featureFlagLookupEncryptionSchemasFLE", enableMultiSchemaFeatureFlag};            \
        setResolvedNamespacesForTest(additionalNamespaces);                                    \
        auto pipeline = jsonToPipeline(_expCtx, _primaryNss, input);                           \
        auto pipelineRewrite =                                                                 \
            MockPipelineRewrite(_primaryNss,                                                   \
                                EncryptionInformation::parse(IDLParserContext("root"),         \
                                                             fromjson(encryptionInformation)), \
                                std::move(pipeline));                                          \
        pipelineRewrite.doRewrite(nullptr);                                                    \
        auto rewrittenPipeline = pipelineRewrite.getPipeline();                                \
        ASSERT(rewrittenPipeline);                                                             \
        assertExpectedPipeline(*rewrittenPipeline, _expCtx, _primaryNss, expected);            \
    }

TEST_FLE_REWRITE_PIPELINE(Match,
                          "[{$match: {$and: [{ssn: {encrypt: 2}}, {age: {encrypt: 4}}]}}]",
                          "[{$match: {$and: [{ssn: {$gt: 2}}, {age: {$gt: 4}}]}}]",
                          {},
                          FLEServerRewritePipelineTest::kSingleEncryptionSchemaEncryptionInfo,
                          true);

TEST_FLE_REWRITE_PIPELINE(
    ProjectWithMatch,
    "[{$project: {foo: '$ssn'}},{$match: {$and: [{foo: {encrypt: 2}}, {age: {encrypt: 4}}]}}]",
    "[{$project: {foo: '$ssn'}},{$match: {$and: [{foo: {$gt: 2}}, {age: {$gt: 4}}]}}]",
    {},
    FLEServerRewritePipelineTest::kSingleEncryptionSchemaEncryptionInfo,
    true);

TEST_FLE_REWRITE_PIPELINE(GraphLookup,
                          "[{ $graphLookup: {  \
                                from: \"coll_a\",\
                                as: \"selfGraph\",\
                                connectToField: \"name\",\
                                connectFromField: \"reportsTo\", \
                                startWith: \"$reportsTo\", \
                                restrictSearchWithMatch: { \
                                     \"ssn\" : { encrypt : 1234 } }}}]",
                          "[{ $graphLookup: { \
                                from : \"coll_a\", \
                                as : \"selfGraph\",\
                                connectToField : \"name\", \
                                connectFromField : \"reportsTo\", \
                                startWith : \"$reportsTo\", \
                                restrictSearchWithMatch : { \
                                    \"ssn\" : { $gt : 1234 } }}}]",
                          {},
                          FLEServerRewritePipelineTest::kSingleEncryptionSchemaEncryptionInfo,
                          true);

TEST_FLE_REWRITE_PIPELINE(GeoNear,
                          "[{ $geoNear: { \
                                key: \"location\", \
                                near : {type : {$const : \"Point\"}, \
                                coordinates : [ {$const : -73.99279}, {$const : 40.719296} ]}, \
                                distanceField : \"dist.calculated\", \
                                maxDistance : 10, \
                                minDistance : 2, \
                                query : {ssn : {encrypt : 1234}}, \
                                spherical : true, \
                                includeLocs : \"dist.location\"} \
                                }]",
                          "[{ $geoNear: { \
                                key: \"location\", \
                                near : {type : {$const : \"Point\"}, \
                                coordinates : [ {$const : -73.99279}, {$const : 40.719296} ]}, \
                                distanceField : \"dist.calculated\", \
                                maxDistance : 10, \
                                minDistance : 2, \
                                query : {ssn : {\"$gt\" : 1234}}, \
                                spherical : true, \
                                includeLocs : \"dist.location\"} \
                                }]",
                          {},
                          FLEServerRewritePipelineTest::kSingleEncryptionSchemaEncryptionInfo,
                          true);

// Note, for DocumentSourceLookup testing, we purposely provide an empty "let", because the
// implementation for the rewrites relies on pipeline::serialize() which will add the superfluous
// empty let variables.
TEST_FLE_REWRITE_PIPELINE(LookupSinglyNestedMatch,
                          R"([{ $lookup: {
                                from: "coll_b",
                                localField: "foo",
                                foreignField: "b_foo",
                                as: "docs",
                                let: {},
                                pipeline: [
                                    {$match:
                                        {$and: [{b_ssn: {encrypt: 2}}, {b_age: {encrypt: 4}}]}
                                    }]}}])",
                          R"([{ $lookup: {
                                from: "coll_b",
                                localField: "foo",
                                foreignField: "b_foo",
                                as: "docs",
                                let: {},
                                pipeline: [
                                    {$match:
                                        {$and: [{b_ssn: {$gt: 2}}, {b_age: {$gt: 4}}]}
                                    }]}}])",
                          {NamespaceString::createNamespaceString_forTest("test.coll_b"_sd)},
                          FLEServerRewritePipelineTest::kTwoEncryptionSchemaEncryptionInfo,
                          true);

TEST_FLE_REWRITE_PIPELINE(LookupDoublyNestedMatch,
                          R"([{ $lookup: {
                                from: "coll_b",
                                localField: "foo",
                                foreignField: "b_foo",
                                as: "docs",
                                let: {},
                                pipeline: [
                                    { $lookup: {
                                        from: "coll_c",
                                        localField: "b_foo",
                                        foreignField: "c_foo",
                                        as: "inner_docs",
                                        let: {},
                                        pipeline: [
                                            {$match:
                                                {$and: [{c_ssn: {encrypt: 2}}, {c_age: {encrypt: 4}}]}
                                            }
                                            ]}},
                                    {$match:
                                        {$and: [{b_ssn: {encrypt: 2}}, {b_age: {encrypt: 4}}]}
                                    }]}}])",
                          R"([{ $lookup: {
                                from: "coll_b",
                                localField: "foo",
                                foreignField: "b_foo",
                                as: "docs",
                                let: {},
                                pipeline: [
                                    { $lookup: {
                                        from: "coll_c",
                                        localField: "b_foo",
                                        foreignField: "c_foo",
                                        as: "inner_docs",
                                        let: {},
                                        pipeline: [
                                            {$match:
                                                {$and: [{c_ssn: {$gt: 2}}, {c_age: {$gt: 4}}]}
                                            }
                                            ]}},
                                    {$match:
                                        {$and: [{b_ssn: {$gt: 2}}, {b_age: {$gt: 4}}]}
                                    }]}}])",
                          std::vector<NamespaceString>(
                              {NamespaceString::createNamespaceString_forTest("test.coll_b"_sd),
                               NamespaceString::createNamespaceString_forTest("test.coll_c"_sd)}),
                          FLEServerRewritePipelineTest::kThreeEncryptionSchemaEncryptionInfo,
                          true);

// Test that no rewrites take place when feature flag is disabled.
TEST_FLE_REWRITE_PIPELINE(LookupSinglyNestedMatch_FeatureFlagDisabled,
                          R"([{ $lookup: {
                                from: "coll_b",
                                localField: "foo",
                                foreignField: "b_foo",
                                as: "docs",
                                let: {},
                                pipeline: [
                                    {$match:
                                        {$and: [{b_ssn: {encrypt: 2}}, {b_age: {encrypt: 4}}]}
                                    }]}}])",
                          R"([{ $lookup: {
                                from: "coll_b",
                                localField: "foo",
                                foreignField: "b_foo",
                                as: "docs",
                                let: {},
                                pipeline: [
                                    {$match:
                                        {$and: [{b_ssn: {encrypt: 2}}, {b_age: {encrypt: 4}}]}
                                    }]}}])",
                          {NamespaceString::createNamespaceString_forTest("test.coll_b"_sd)},
                          FLEServerRewritePipelineTest::kTwoEncryptionSchemaEncryptionInfo,
                          false);

TEST_FLE_REWRITE_PIPELINE(LookupDoublyNestedMatch_FeatureFlagDisabled,
                          R"([{ $lookup: {
                                from: "coll_b",
                                localField: "foo",
                                foreignField: "b_foo",
                                as: "docs",
                                let: {},
                                pipeline: [
                                    { $lookup: {
                                        from: "coll_c",
                                        localField: "b_foo",
                                        foreignField: "c_foo",
                                        as: "inner_docs",
                                        let: {},
                                        pipeline: [
                                            {$match:
                                                {$and: [{c_ssn: {encrypt: 2}}, {c_age: {encrypt: 4}}]}
                                            }
                                            ]}},
                                    {$match:
                                        {$and: [{b_ssn: {encrypt: 2}}, {b_age: {encrypt: 4}}]}
                                    }]}}])",
                          R"([{ $lookup: {
                                from: "coll_b",
                                localField: "foo",
                                foreignField: "b_foo",
                                as: "docs",
                                let: {},
                                pipeline: [
                                    { $lookup: {
                                        from: "coll_c",
                                        localField: "b_foo",
                                        foreignField: "c_foo",
                                        as: "inner_docs",
                                        let: {},
                                        pipeline: [
                                            {$match:
                                                {$and: [{c_ssn: {encrypt: 2}}, {c_age: {encrypt: 4}}]}
                                            }
                                            ]}},
                                    {$match:
                                        {$and: [{b_ssn: {encrypt: 2}}, {b_age: {encrypt: 4}}]}
                                    }]}}])",
                          std::vector<NamespaceString>(
                              {NamespaceString::createNamespaceString_forTest("test.coll_b"_sd),
                               NamespaceString::createNamespaceString_forTest("test.coll_c"_sd)}),
                          FLEServerRewritePipelineTest::kThreeEncryptionSchemaEncryptionInfo,
                          false);

TEST_F(FLEServerRewritePipelineTest, MissingEscPrimaryCollectionFails_PipelineRewrite) {
    RAIIServerParameterControllerForTest _scopedFeature{"featureFlagLookupEncryptionSchemasFLE",
                                                        true};
    setResolvedNamespacesForTest({});
    auto pipeline = jsonToPipeline(
        _expCtx, _primaryNss, "[{$match: {$and: [{ssn: {encrypt: 2}}, {age: {encrypt: 4}}]}}]");
    auto pipelineRewrite = MockPipelineRewrite(
        _primaryNss,
        EncryptionInformation::parse(IDLParserContext("root"),
                                     fromjson(kSingleEncryptionSchemaEncryptionCollD)),
        std::move(pipeline));
    ASSERT_THROWS_CODE(pipelineRewrite.doRewrite(nullptr), AssertionException, 10026006);
}

TEST_F(FLEServerRewritePipelineTest, MissingEscForeignCollectionFails_PipelineRewrite) {
    RAIIServerParameterControllerForTest _scopedFeature{"featureFlagLookupEncryptionSchemasFLE",
                                                        true};

    const auto foreignNss = NamespaceString::createNamespaceString_forTest("test.coll_d"_sd);
    setResolvedNamespacesForTest({foreignNss});
    auto pipeline = jsonToPipeline(_expCtx, _primaryNss, R"([{ $lookup: {
                                                                            from: "coll_d",
                                                                            localField: "foo",
                                                                            foreignField: "d_foo",
                                                                            as: "docs",
                                                                            let: {},
                                                                            pipeline: [
                                                                                {$match:
                                                                                    {$and: [{d_ssn: {encrypt: 2}}, {d_age: {encrypt: 4}}]}
                                                                                }]}}])");
    auto pipelineRewrite = MockPipelineRewrite(
        _primaryNss,
        EncryptionInformation::parse(IDLParserContext("root"),
                                     fromjson(kSingleEncryptionSchemaEncryptionInfo)),
        std::move(pipeline));
    ASSERT_THROWS_CODE(pipelineRewrite.doRewrite(nullptr), AssertionException, 10026006);
}

TEST_FLE_REWRITE_PIPELINE(LookupDoublyNestedMatchMissingUnencryptedForeignCollection,
                          R"([{ $lookup: {
                                from: "coll_d",
                                localField: "foo",
                                foreignField: "d_foo",
                                as: "docs",
                                let: {},
                                pipeline: [
                                    { $lookup: {
                                        from: "coll_b",
                                        localField: "d_foo",
                                        foreignField: "b_foo",
                                        as: "inner_docs",
                                        let: {},
                                        pipeline: [
                                            {$match:
                                                {$and: [{b_ssn: {encrypt: 2}}, {b_age: {encrypt:
                        4}}]}
                                            }
                                            ]}},
                                    {$match:
                                        {$and: [{d_ssn: 2}, {d_age: 4}]}
                                    }]}}])",
                          R"([{ $lookup: {
                                from: "coll_d",
                                localField: "foo",
                                foreignField: "d_foo",
                                as: "docs",
                                let: {},
                                pipeline: [
                                    { $lookup: {
                                        from: "coll_b",
                                        localField: "d_foo",
                                        foreignField: "b_foo",
                                        as: "inner_docs",
                                        let: {},
                                        pipeline: [
                                            {$match:
                                                {$and: [{b_ssn: {$gt: 2}}, {b_age: {$gt: 4}}]}
                                            }
                                            ]}},
                                    {$match:
                                        {$and: [{d_ssn: 2}, {d_age: 4}]}
                                    }]}}])",
                          std::vector<NamespaceString>(
                              {NamespaceString::createNamespaceString_forTest("test.coll_b"_sd),
                               NamespaceString::createNamespaceString_forTest("test.coll_d"_sd)}),
                          FLEServerRewritePipelineTest::kTwoEncryptionSchemaEncryptionInfo,
                          true);

TEST_FLE_REWRITE_PIPELINE(LookupDoublyNestedMatchMissingUnencryptedPrimarySchema,
                          R"([{ $lookup: {
                                from: "coll_e",
                                localField: "foo",
                                foreignField: "e_foo",
                                as: "docs",
                                let: {},
                                pipeline: [
                                    { $lookup: {
                                        from: "coll_d",
                                        localField: "e_foo",
                                        foreignField: "d_foo",
                                        as: "inner_docs",
                                        let: {},
                                        pipeline: [
                                            {$match:
                                                {$and: [{d_ssn: {encrypt: 2}}, {d_age: {encrypt:4}}]}
                                            }
                                            ]}},
                                    {$match:
                                        {$and: [{e_ssn: 2}, {e_age: 4}]}
                                    }]}},
                            {$match:
                                {$and: [{ssn: 2}, {age: 4}]}
                            }])",
                          R"([{ $lookup: {
                                from: "coll_e",
                                localField: "foo",
                                foreignField: "e_foo",
                                as: "docs",
                                let: {},
                                pipeline: [
                                    { $lookup: {
                                        from: "coll_d",
                                        localField: "e_foo",
                                        foreignField: "d_foo",
                                        as: "inner_docs",
                                        let: {},
                                        pipeline: [
                                            {$match:
                                                {$and: [{d_ssn: {$gt: 2}}, {d_age: {$gt: 4}}]}
                                            }
                                            ]}},
                                    {$match:
                                        {$and: [{e_ssn: 2}, {e_age: 4}]}
                                    }]}},
                                    {$match:
                                        {$and: [{ssn: 2}, {age: 4}]}
                                    }])",
                          std::vector<NamespaceString>(
                              {NamespaceString::createNamespaceString_forTest("test.coll_d"_sd),
                               NamespaceString::createNamespaceString_forTest("test.coll_e"_sd)}),
                          FLEServerRewritePipelineTest::kSingleEncryptionSchemaEncryptionCollD,
                          true);

}  // namespace
}  // namespace mongo
