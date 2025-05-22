/**
+ * Tests releaseMemory in hash lookup.
+ *
 * @tags: [
 *   requires_fcv_82,
 *   # We modify the value of a query knob. setParameter is not persistent.
 *   does_not_support_stepdowns,
 *   # This test runs commands that are not allowed with security token: setParameter.
 *   not_allowed_with_signed_security_token,
 *   requires_getmore,
 *   uses_getmore_outside_of_transaction,
 *   assumes_read_preference_unchanged,
 *   does_not_support_transactions,
 *   # releaseMemory needs special permission
 *   assumes_superuser_permissions,
 *   # This test relies on query commands returning specific batch-sized responses.
 *   assumes_no_implicit_cursor_exhaustion,
 * ]
 */

import {assertArrayEq} from "jstests/aggregation/extras/utils.js";
import {DiscoverTopology} from "jstests/libs/discover_topology.js";
import {getAggPlanStages, getEngine} from "jstests/libs/query/analyze_plan.js";
import {accumulateServerStatusMetric} from "jstests/libs/release_memory_util.js";
import {setParameterOnAllHosts} from "jstests/noPassthrough/libs/server_parameter_helpers.js";

function getSpillCounter() {
    return accumulateServerStatusMetric(db, metrics => metrics.query.lookup.hashLookupSpills);
}

const memoryKnob = "internalQuerySlotBasedExecutionHashLookupApproxMemoryUseInBytesBeforeSpill";
const sbeIncreasedSpillingKnob = "internalQuerySlotBasedExecutionHashAggIncreasedSpilling";

function getServerParameter(knob) {
    return assert.commandWorked(db.adminCommand({getParameter: 1, [knob]: 1}))[knob];
}
function setServerParameter(knob, value) {
    setParameterOnAllHosts(DiscoverTopology.findNonConfigNodes(db.getMongo()), knob, value);
}

const memoryInitialValue = getServerParameter(memoryKnob);
const sbeIncreasedSpillingInitialValue = getServerParameter(sbeIncreasedSpillingKnob);

// HashLookup in SBE might use HashAgg. We want to control spilling. Disable increased spilling.
setServerParameter(sbeIncreasedSpillingKnob, "never");

const students = db[jsTestName() + "_students"];
students.drop();
const people = db[jsTestName() + "_people"];
people.drop();

const studentsDocs = [
    {sID: 22001, name: "Alex", year: 1, score: 4.0},
    {sID: 21001, name: "Bernie", year: 2, score: 3.7},
    {sID: 20010, name: "Chris", year: 3, score: 2.5},
    {sID: 22021, name: "Drew", year: 1, score: 3.2},
    {sID: 17301, name: "Harley", year: 6, score: 3.1},
    {sID: 21022, name: "Farmer", year: 1, score: 2.2},
    {sID: 20020, name: "George", year: 3, score: 2.8},
    {sID: 18020, name: "Harley", year: 5, score: 2.8},
];
const peopleDocs = [
    {pID: 1000, name: "Alex"},
    {pID: 1001, name: "Drew"},
    {pID: 1002, name: "Justin"},
    {pID: 1003, name: "Parker"},
];

assert.commandWorked(students.insertMany(studentsDocs));
assert.commandWorked(people.insertMany(peopleDocs));

const lookupWithPipeline = [{
        $lookup: {
            from: students.getName(),
            as: "matched",
            let: {l_name: "$name"},
            pipeline: [{$match: {$expr: {$eq: ["$$l_name", "name"]}}}]
        }
    }];

const lookupWithoutPipeline = [
    {$lookup: {from: students.getName(), localField: "name", foreignField: "name", as: "matched"}}
];

for (let {localColl, pipeline} of [{localColl: people, pipeline: lookupWithPipeline},
                                   {localColl: people, pipeline: lookupWithoutPipeline}]) {
    jsTest.log.info("Running pipeline: ", pipeline[0]);

    const explain = localColl.explain().aggregate(pipeline);
    if (getEngine(explain) === "classic") {
        jsTest.log.info("Skipping test, $lookup does not spill in classic", explain);
        continue;
    }
    const eqLookupNodes = getAggPlanStages(explain, "EQ_LOOKUP");
    if (eqLookupNodes.length === 0) {
        jsTest.log.info("Skipping test, $lookup has not been pushed to SBE", explain);
        continue;
    }

    // Get all the results to use as a reference. Set 'allowDiskUse' to false to disable
    // increased spilling in debug builds.
    const expectedResults = localColl.aggregate(pipeline, {"allowDiskUse": false}).toArray();
    {
        jsTest.log(`Running no spill in first batch`);
        setServerParameter(memoryKnob, 100 * 1024 * 1024);
        let initialSpillCount = getSpillCounter();

        // Retrieve the first batch without spilling.
        const cursor =
            localColl.aggregate(pipeline, {"allowDiskUse": true, cursor: {batchSize: 1}});
        const cursorId = cursor.getId();

        // Assert it did not spill during the first batch.
        const newSpillCount = getSpillCounter();
        assert.eq(initialSpillCount, newSpillCount);
        initialSpillCount = newSpillCount;

        // Release memory (i.e., spill)
        const releaseMemoryCmd = {releaseMemory: [cursorId]};
        jsTest.log.info("Running releaseMemory: ", releaseMemoryCmd);
        const releaseMemoryRes = db.runCommand(releaseMemoryCmd);
        assert.commandWorked(releaseMemoryRes);
        assert.eq(releaseMemoryRes.cursorsReleased, [cursorId], releaseMemoryRes);
        assert.lt(initialSpillCount, getSpillCounter());

        jsTest.log.info("Running getMore");
        const results = cursor.toArray();

        assertArrayEq({actual: results, expected: expectedResults});

        setServerParameter(memoryKnob, memoryInitialValue);
    }

    // Run query with increased spilling to spill while creating the first batch.
    {
        jsTest.log(`Running spill in first batch`);
        setServerParameter(memoryKnob, 1);
        let initialSpillCount = getSpillCounter();

        // Retrieve the first batch.
        const cursor =
            localColl.aggregate(pipeline, {"allowDiskUse": true, cursor: {batchSize: 1}});
        const cursorId = cursor.getId();

        // Assert it spilt during the first batch.
        const newSpillCount = getSpillCounter();
        assert.lt(initialSpillCount, newSpillCount);
        initialSpillCount = newSpillCount;

        // Release memory (i.e., spill)
        const releaseMemoryCmd = {releaseMemory: [cursorId]};
        jsTest.log.info("Running releaseMemory: ", releaseMemoryCmd);
        const releaseMemoryRes = db.runCommand(releaseMemoryCmd);
        assert.commandWorked(releaseMemoryRes);
        assert.eq(releaseMemoryRes.cursorsReleased, [cursorId], releaseMemoryRes);
        assert.eq(initialSpillCount, getSpillCounter());

        jsTest.log.info("Running getMore");
        const results = cursor.toArray();

        assertArrayEq({actual: results, expected: expectedResults});

        setServerParameter(memoryKnob, memoryInitialValue);
    }
}

setServerParameter(sbeIncreasedSpillingKnob, sbeIncreasedSpillingInitialValue);
