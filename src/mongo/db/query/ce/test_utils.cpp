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

#include "mongo/db/query/ce/test_utils.h"

#include "mongo/db/exec/docval_to_sbeval.h"
#include "mongo/db/query/ce/histogram_estimation_impl.h"
#include "mongo/db/query/stats/value_utils.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo::ce {
namespace value = sbe::value;

stats::ScalarHistogram createHistogram(const std::vector<BucketData>& data) {
    value::Array bounds;
    std::vector<stats::Bucket> buckets;

    double cumulativeFreq = 0.0;
    double cumulativeNDV = 0.0;

    // Create a value vector & sort it.
    std::vector<stats::SBEValue> values;
    for (size_t i = 0; i < data.size(); i++) {
        const auto& item = data[i];
        const auto [tag, val] = sbe::value::makeValue(item._v);
        values.emplace_back(tag, val);
    }
    sortValueVector(values);

    for (size_t i = 0; i < values.size(); i++) {
        const auto& val = values[i];
        const auto [tag, value] = copyValue(val.getTag(), val.getValue());
        bounds.push_back(tag, value);

        const auto& item = data[i];
        cumulativeFreq += item._equalFreq + item._rangeFreq;
        cumulativeNDV += item._ndv + 1.0;
        buckets.emplace_back(
            item._equalFreq, item._rangeFreq, cumulativeFreq, item._ndv, cumulativeNDV);
    }
    return stats::ScalarHistogram::make(std::move(bounds), std::move(buckets));
}

double estimateCardinalityScalarHistogramInteger(const stats::ScalarHistogram& hist,
                                                 const int v,
                                                 const EstimationType type) {
    const auto [tag, val] =
        std::make_pair(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(v));
    return estimateCardinality(hist, tag, val, type).card;
};
}  // namespace mongo::ce
