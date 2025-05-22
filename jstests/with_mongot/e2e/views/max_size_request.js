/**
 * This test builds a pipeline nearing the BSON size limit and attempts to create search indexes
 * with parameters that would put the request over the limit. This is to ensure that our search
 * index interface correctly catches and returns such errors.
 * @tags: [ featureFlagMongotIndexedViews, requires_fcv_81 ]
 */
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";
import {createSearchIndex, dropSearchIndex} from "jstests/libs/search.js";
import {assertViewAppliedCorrectly} from "jstests/with_mongot/e2e_lib/explain_utils.js";

const testDb = db.getSiblingDB(jsTestName());
const coll = testDb.underlyingSourceCollection;
coll.drop();

const bulk = coll.initializeUnorderedBulkOp();
bulk.insert({_id: "foo", category: "test"});
assert.commandWorked(bulk.execute());

const parentName = "underlyingSourceCollection";

// 15.9 MB target size for the pipeline (just under 16 MB BSON limit).
const targetSize = 15 * 1024 * 1024 + 1024 * 200;

// 100 KB string to append to the pipeline.
const largeString = "a".repeat(1024 * 100);

const fieldNamePrefix = "large_field_";
let fieldNum = 0;

let pipeline = [];

for (let currentSize = 0; currentSize < targetSize;) {
    // Add large stage to pipeline.
    const stage = {$addFields: {[`${fieldNamePrefix}${fieldNum}`]: largeString}};
    pipeline.push(stage);

    // Increment variables.
    fieldNum++;
    currentSize += JSON.stringify(stage).length;
}

// At this point, the pipeline is ~15.9 MB. There is an additional 800 KB needed to overflow BSON
// (16 MB is actually 16.7MB).

const viewName = "viewName";

assert.commandWorked(testDb.createView(viewName, parentName, pipeline));

const view = testDb[viewName];

// Should be able to create a regular search index with this pipeline as it's still under the BSON
// limit.
createSearchIndex(view, {name: "viewNameIndex", definition: {"mappings": {"dynamic": true}}});

// Expect failures when the mongot request goes over the BSON limit.
// Both of these asserts add a minimum of 800MB to the request.
assert.throwsWithCode(
    () => createSearchIndex(
        view,
        {name: "tooLargeNameIndex".repeat(1024 * 52), definition: {"mappings": {"dynamic": true}}}),
    ErrorCodes.BSONObjectTooLarge);

assert.throwsWithCode(
    () => createSearchIndex(view, {
        name: "tooLargeDefinitionIndex",
        definition: {
            "mappings": {"dynamic": true},
            "fields": {"large_metadata": {"type": "string", "meta": "b".repeat(1024 * 800)}}
        }
    }),
    ErrorCodes.BSONObjectTooLarge);

// Ensure that a simple query can be ran on the successful search index even after the expected
// failures above.
const normalSearchQuery = {
    $search: {index: "viewNameIndex", text: {query: "test", path: "category"}}
};

// We can only assert that the view is applied correctly for the single_node suite and single_shard
// suite (numberOfShardsForCollection() will return 1 in a single node environment). This is because
// the sharded_cluster suite will omit the explain output for each shard in this test because the
// view definition is too large (but still small enough to run queries on).
if (FixtureHelpers.numberOfShardsForCollection(coll) == 1) {
    const explain = assert.commandWorked(view.explain().aggregate([normalSearchQuery]));
    assertViewAppliedCorrectly(explain, [normalSearchQuery], pipeline);
}

const results = view.aggregate([normalSearchQuery]).toArray();
assert(results.length == 1);

dropSearchIndex(view, {name: "viewNameIndex"});
