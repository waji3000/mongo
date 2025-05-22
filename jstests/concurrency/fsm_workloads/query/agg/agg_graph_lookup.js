/**
 * agg_graph_lookup.js
 *
 * Runs a $graphLookup aggregation simultaneously with updates.
 *
 * @tags: [
 *   requires_getmore,
 *   uses_getmore_outside_of_transaction,
 * ]
 */
import {interruptedQueryErrors} from "jstests/concurrency/fsm_libs/assert.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {TxnUtil} from "jstests/libs/txns/txn_util.js";

export const $config = (function() {
    const data = {numDocs: 1000};

    const states = (function() {
        function query(db, collName) {
            if (this.shouldSkipTest) {
                return;
            }
            const limitAmount = 20;
            const startingId = Random.randInt(this.numDocs - limitAmount);

            function getQueryResults() {
                let arr = null;
                try {
                    const cursor = db[collName]
                          .aggregate([
                              {$match: {_id: {$gt: startingId}}},
                              {
                                  $graphLookup: {
                                      from: collName,
                                      startWith: "$to",
                                      connectToField: "_id",
                                      connectFromField: "to",
                                      maxDepth: 10,
                                      as: "out",
                                  }
                              },
                              {$limit: limitAmount}
                          ], {cursor: {batchSize: limitAmount + 1}});

                    arr = cursor.toArray();
                } catch (e) {
                    // When running with stepdowns or with balancer, we expect to sometimes see
                    // the query killed.
                    const isExpectedError =
                        (TestData.runningWithShardStepdowns || TestData.runningWithBalancer) &&
                        interruptedQueryErrors.includes(e.code);
                    if (!isExpectedError) {
                        throw e;
                    }
                }

                return arr;
            }

            const res = getQueryResults();
            if (res) {
                assert.eq(res.length, limitAmount);
            }
        }

        function update(db, collName) {
            if (this.shouldSkipTest) {
                return;
            }
            const index = Random.randInt(this.numDocs + 1);
            const update = Random.randInt(this.numDocs + 1);
            const res = db[collName].update({_id: index}, {$set: {to: update}});
            assert.commandWorked(res);
        }

        return {query, update};
    })();

    const transitions = {query: {query: 0.5, update: 0.5}, update: {query: 0.5, update: 0.5}};

    function setup(db, collName, cluster) {
        // TODO SERVER-88936: Remove this field and associated checks once the flag is active on
        // last-lts.
        this.shouldSkipTest = TestData.runInsideTransaction && cluster.isSharded() &&
            !FeatureFlagUtil.isPresentAndEnabled(db.getMongo(), 'AllowAdditionalParticipants');
        if (this.shouldSkipTest) {
            return;
        }

        // Load example data.
        const bulk = db[collName].initializeUnorderedBulkOp();
        for (let i = 0; i < this.numDocs; ++i) {
            bulk.insert({_id: i, to: i + 1});
        }
        const res = bulk.execute();
        assert.commandWorked(res);
        assert.eq(this.numDocs, res.nInserted);
        assert.eq(this.numDocs, db[collName].find().itcount());
    }

    return {
        threadCount: 10,
        iterations: 100,
        states: states,
        startState: 'query',
        transitions: transitions,
        data: data,
        setup: setup,
    };
})();
