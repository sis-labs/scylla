/*
 * Copyright (C) 2016 ScyllaDB
 */

/*
 * This file is part of Scylla.
 *
 * Scylla is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Scylla is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Scylla.  If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <vector>
#include <chrono>

#include "sstables.hh"
#include "compaction.hh"
#include "database.hh"
#include "compaction_strategy.hh"
#include "schema.hh"
#include "cql3/statements/property_definitions.hh"
#include "leveled_manifest.hh"
#include "sstable_set.hh"
#include "compatible_ring_position.hh"
#include <boost/range/algorithm/find.hpp>
#include <boost/icl/interval_map.hpp>
#include "date_tiered_compaction_strategy.hh"

logging::logger date_tiered_manifest::logger = logging::logger("DateTieredCompactionStrategy");

namespace sstables {

extern logging::logger logger;

class sstable_set_impl {
public:
    virtual ~sstable_set_impl() {}
    virtual std::unique_ptr<sstable_set_impl> clone() const = 0;
    virtual std::vector<shared_sstable> select(const query::partition_range& range) const = 0;
    virtual void insert(shared_sstable sst) = 0;
    virtual void erase(shared_sstable sst) = 0;
};

sstable_set::sstable_set(std::unique_ptr<sstable_set_impl> impl, lw_shared_ptr<sstable_list> all)
        : _impl(std::move(impl))
        , _all(std::move(all)) {
}

sstable_set::sstable_set(const sstable_set& x)
        : _impl(x._impl->clone())
        , _all(make_lw_shared(sstable_list(*x._all))) {
}

sstable_set::sstable_set(sstable_set&&) noexcept = default;

sstable_set&
sstable_set::operator=(const sstable_set& x) {
    if (this != &x) {
        auto tmp = sstable_set(x);
        *this = std::move(tmp);
    }
    return *this;
}

sstable_set&
sstable_set::operator=(sstable_set&&) noexcept = default;

std::vector<shared_sstable>
sstable_set::select(const query::partition_range& range) const {
    return _impl->select(range);
}

void
sstable_set::insert(shared_sstable sst) {
    _impl->insert(sst);
    try {
        _all->insert(sst);
    } catch (...) {
        _impl->erase(sst);
        throw;
    }
}

void
sstable_set::erase(shared_sstable sst) {
    _impl->erase(sst);
    _all->erase(sst);
}

sstable_set::~sstable_set() = default;

// default sstable_set, not specialized for anything
class bag_sstable_set : public sstable_set_impl {
    // erasing is slow, but select() is fast
    std::vector<shared_sstable> _sstables;
public:
    virtual std::unique_ptr<sstable_set_impl> clone() const override {
        return std::make_unique<bag_sstable_set>(*this);
    }
    virtual std::vector<shared_sstable> select(const query::partition_range& range) const override {
        return _sstables;
    }
    virtual void insert(shared_sstable sst) override {
        _sstables.push_back(std::move(sst));
    }
    virtual void erase(shared_sstable sst) override {
        _sstables.erase(boost::find(_sstables, sst));
    }
};

// specialized when sstables are partitioned in the token range space
// e.g. leveled compaction strategy
class partitioned_sstable_set : public sstable_set_impl {
    using value_set = std::unordered_set<shared_sstable>;
    using interval_map_type = boost::icl::interval_map<compatible_ring_position, value_set>;
    using interval_type = interval_map_type::interval_type;
    using map_iterator = interval_map_type::const_iterator;
private:
    schema_ptr _schema;
    interval_map_type _sstables;
private:
    interval_type make_interval(const query::partition_range& range) const {
        return interval_type::closed(
                compatible_ring_position(*_schema, range.start()->value()),
                compatible_ring_position(*_schema, range.end()->value()));
    }
    interval_type singular(const dht::ring_position& rp) const {
        auto crp = compatible_ring_position(*_schema, rp);
        return interval_type::closed(crp, crp);
    }
    std::pair<map_iterator, map_iterator> query(const query::partition_range& range) const {
        if (range.start() && range.end()) {
            return _sstables.equal_range(make_interval(range));
        }
        else if (range.start() && !range.end()) {
            auto start = singular(range.start()->value());
            return { _sstables.lower_bound(start), _sstables.end() };
        } else if (!range.start() && range.end()) {
            auto end = singular(range.end()->value());
            return { _sstables.begin(), _sstables.upper_bound(end) };
        } else {
            return { _sstables.begin(), _sstables.end() };
        }
    }
public:
    explicit partitioned_sstable_set(schema_ptr schema)
            : _schema(std::move(schema)) {
    }
    virtual std::unique_ptr<sstable_set_impl> clone() const override {
        return std::make_unique<partitioned_sstable_set>(*this);
    }
    virtual std::vector<shared_sstable> select(const query::partition_range& range) const override {
        auto ipair = query(range);
        auto b = std::move(ipair.first);
        auto e = std::move(ipair.second);
        value_set result;
        while (b != e) {
            boost::copy(b++->second, std::inserter(result, result.end()));
        }
        return std::vector<shared_sstable>(result.begin(), result.end());
    }
    virtual void insert(shared_sstable sst) override {
        auto first = sst->get_first_decorated_key().token();
        auto last = sst->get_last_decorated_key().token();
        using bound = query::partition_range::bound;
        _sstables.add({
                make_interval(
                        query::partition_range(
                                bound(dht::ring_position::starting_at(first)),
                                bound(dht::ring_position::ending_at(last)))),
                value_set({sst})});
    }
    virtual void erase(shared_sstable sst) override {
        auto first = sst->get_first_decorated_key().token();
        auto last = sst->get_last_decorated_key().token();
        using bound = query::partition_range::bound;
        _sstables.subtract({
                make_interval(
                        query::partition_range(
                                bound(dht::ring_position::starting_at(first)),
                                bound(dht::ring_position::ending_at(last)))),
                value_set({sst})});
    }
};

class compaction_strategy_impl {
protected:
    bool _use_clustering_key_filter = false;
public:
    virtual ~compaction_strategy_impl() {}
    virtual compaction_descriptor get_sstables_for_compaction(column_family& cfs, std::vector<sstables::shared_sstable> candidates) = 0;
    virtual compaction_strategy_type type() const = 0;
    virtual bool parallel_compaction() const {
        return true;
    }
    virtual int64_t estimated_pending_compactions(column_family& cf) const = 0;
    virtual std::unique_ptr<sstable_set_impl> make_sstable_set(schema_ptr schema) const {
        return std::make_unique<bag_sstable_set>();
    }
    bool use_clustering_key_filter() const {
        return _use_clustering_key_filter;
    }
};

//
// Null compaction strategy is the default compaction strategy.
// As the name implies, it does nothing.
//
class null_compaction_strategy : public compaction_strategy_impl {
public:
    virtual compaction_descriptor get_sstables_for_compaction(column_family& cfs, std::vector<sstables::shared_sstable> candidates) override {
        return sstables::compaction_descriptor();
    }

    virtual int64_t estimated_pending_compactions(column_family& cf) const override {
        return 0;
    }

    virtual compaction_strategy_type type() const {
        return compaction_strategy_type::null;
    }
};

//
// Major compaction strategy is about compacting all available sstables into one.
//
class major_compaction_strategy : public compaction_strategy_impl {
    static constexpr size_t min_compact_threshold = 2;
public:
    virtual compaction_descriptor get_sstables_for_compaction(column_family& cfs, std::vector<sstables::shared_sstable> candidates) override {
        // At least, two sstables must be available for compaction to take place.
        if (cfs.sstables_count() < min_compact_threshold) {
            return sstables::compaction_descriptor();
        }
        return sstables::compaction_descriptor(std::move(candidates));
    }

    virtual int64_t estimated_pending_compactions(column_family& cf) const override {
        return (cf.sstables_count() < min_compact_threshold) ? 0 : 1;
    }

    virtual compaction_strategy_type type() const {
        return compaction_strategy_type::major;
    }
};

class size_tiered_compaction_strategy_options {
    static constexpr uint64_t DEFAULT_MIN_SSTABLE_SIZE = 50L * 1024L * 1024L;
    static constexpr double DEFAULT_BUCKET_LOW = 0.5;
    static constexpr double DEFAULT_BUCKET_HIGH = 1.5;
    static constexpr double DEFAULT_COLD_READS_TO_OMIT = 0.05;
    const sstring MIN_SSTABLE_SIZE_KEY = "min_sstable_size";
    const sstring BUCKET_LOW_KEY = "bucket_low";
    const sstring BUCKET_HIGH_KEY = "bucket_high";
    const sstring COLD_READS_TO_OMIT_KEY = "cold_reads_to_omit";

    uint64_t min_sstable_size = DEFAULT_MIN_SSTABLE_SIZE;
    double bucket_low = DEFAULT_BUCKET_LOW;
    double bucket_high = DEFAULT_BUCKET_HIGH;
    double cold_reads_to_omit =  DEFAULT_COLD_READS_TO_OMIT;
public:
    static std::experimental::optional<sstring> get_value(const std::map<sstring, sstring>& options, const sstring& name) {
        auto it = options.find(name);
        if (it == options.end()) {
            return std::experimental::nullopt;
        }
        return it->second;
    }

    size_tiered_compaction_strategy_options(const std::map<sstring, sstring>& options) {
        using namespace cql3::statements;

        auto tmp_value = get_value(options, MIN_SSTABLE_SIZE_KEY);
        min_sstable_size = property_definitions::to_long(MIN_SSTABLE_SIZE_KEY, tmp_value, DEFAULT_MIN_SSTABLE_SIZE);

        tmp_value = get_value(options, BUCKET_LOW_KEY);
        bucket_low = property_definitions::to_double(BUCKET_LOW_KEY, tmp_value, DEFAULT_BUCKET_LOW);

        tmp_value = get_value(options, BUCKET_HIGH_KEY);
        bucket_high = property_definitions::to_double(BUCKET_HIGH_KEY, tmp_value, DEFAULT_BUCKET_HIGH);

        tmp_value = get_value(options, COLD_READS_TO_OMIT_KEY);
        cold_reads_to_omit = property_definitions::to_double(COLD_READS_TO_OMIT_KEY, tmp_value, DEFAULT_COLD_READS_TO_OMIT);
    }

    size_tiered_compaction_strategy_options() {
        min_sstable_size = DEFAULT_MIN_SSTABLE_SIZE;
        bucket_low = DEFAULT_BUCKET_LOW;
        bucket_high = DEFAULT_BUCKET_HIGH;
        cold_reads_to_omit = DEFAULT_COLD_READS_TO_OMIT;
    }

    // FIXME: convert java code below.
#if 0
    public static Map<String, String> validateOptions(Map<String, String> options, Map<String, String> uncheckedOptions) throws ConfigurationException
    {
        String optionValue = options.get(MIN_SSTABLE_SIZE_KEY);
        try
        {
            long minSSTableSize = optionValue == null ? DEFAULT_MIN_SSTABLE_SIZE : Long.parseLong(optionValue);
            if (minSSTableSize < 0)
            {
                throw new ConfigurationException(String.format("%s must be non negative: %d", MIN_SSTABLE_SIZE_KEY, minSSTableSize));
            }
        }
        catch (NumberFormatException e)
        {
            throw new ConfigurationException(String.format("%s is not a parsable int (base10) for %s", optionValue, MIN_SSTABLE_SIZE_KEY), e);
        }

        double bucketLow = parseDouble(options, BUCKET_LOW_KEY, DEFAULT_BUCKET_LOW);
        double bucketHigh = parseDouble(options, BUCKET_HIGH_KEY, DEFAULT_BUCKET_HIGH);
        if (bucketHigh <= bucketLow)
        {
            throw new ConfigurationException(String.format("%s value (%s) is less than or equal to the %s value (%s)",
                                                           BUCKET_HIGH_KEY, bucketHigh, BUCKET_LOW_KEY, bucketLow));
        }

        double maxColdReadsRatio = parseDouble(options, COLD_READS_TO_OMIT_KEY, DEFAULT_COLD_READS_TO_OMIT);
        if (maxColdReadsRatio < 0.0 || maxColdReadsRatio > 1.0)
        {
            throw new ConfigurationException(String.format("%s value (%s) should be between between 0.0 and 1.0",
                                                           COLD_READS_TO_OMIT_KEY, optionValue));
        }

        uncheckedOptions.remove(MIN_SSTABLE_SIZE_KEY);
        uncheckedOptions.remove(BUCKET_LOW_KEY);
        uncheckedOptions.remove(BUCKET_HIGH_KEY);
        uncheckedOptions.remove(COLD_READS_TO_OMIT_KEY);

        return uncheckedOptions;
    }
#endif
    friend class size_tiered_compaction_strategy;
};

class size_tiered_compaction_strategy : public compaction_strategy_impl {
    size_tiered_compaction_strategy_options _options;

    // Return a list of pair of shared_sstable and its respective size.
    std::vector<std::pair<sstables::shared_sstable, uint64_t>> create_sstable_and_length_pairs(const std::vector<sstables::shared_sstable>& sstables) const;

    // Group files of similar size into buckets.
    std::vector<std::vector<sstables::shared_sstable>> get_buckets(const std::vector<sstables::shared_sstable>& sstables, unsigned max_threshold) const;

    // Maybe return a bucket of sstables to compact
    std::vector<sstables::shared_sstable>
    most_interesting_bucket(std::vector<std::vector<sstables::shared_sstable>> buckets, unsigned min_threshold, unsigned max_threshold);

    // Return the average size of a given list of sstables.
    uint64_t avg_size(std::vector<sstables::shared_sstable>& sstables) {
        assert(sstables.size() > 0); // this should never fail
        uint64_t n = 0;

        for (auto& sstable : sstables) {
            // FIXME: Switch to sstable->bytes_on_disk() afterwards. That's what C* uses.
            n += sstable->data_size();
        }

        return n / sstables.size();
    }
public:
    size_tiered_compaction_strategy() = default;
    size_tiered_compaction_strategy(const std::map<sstring, sstring>& options) :
        _options(options) {}

    virtual compaction_descriptor get_sstables_for_compaction(column_family& cfs, std::vector<sstables::shared_sstable> candidates) override;

    virtual int64_t estimated_pending_compactions(column_family& cf) const override;

    friend std::vector<sstables::shared_sstable> size_tiered_most_interesting_bucket(lw_shared_ptr<sstable_list>);
    friend std::vector<sstables::shared_sstable> size_tiered_most_interesting_bucket(const std::list<sstables::shared_sstable>&);

    virtual compaction_strategy_type type() const {
        return compaction_strategy_type::size_tiered;
    }
};

std::vector<std::pair<sstables::shared_sstable, uint64_t>>
size_tiered_compaction_strategy::create_sstable_and_length_pairs(const std::vector<sstables::shared_sstable>& sstables) const {

    std::vector<std::pair<sstables::shared_sstable, uint64_t>> sstable_length_pairs;
    sstable_length_pairs.reserve(sstables.size());

    for(auto& sstable : sstables) {
        auto sstable_size = sstable->data_size();
        assert(sstable_size != 0);

        sstable_length_pairs.emplace_back(sstable, sstable_size);
    }

    return sstable_length_pairs;
}

std::vector<std::vector<sstables::shared_sstable>>
size_tiered_compaction_strategy::get_buckets(const std::vector<sstables::shared_sstable>& sstables, unsigned max_threshold) const {
    // sstables sorted by size of its data file.
    auto sorted_sstables = create_sstable_and_length_pairs(sstables);

    std::sort(sorted_sstables.begin(), sorted_sstables.end(), [] (auto& i, auto& j) {
        return i.second < j.second;
    });

    std::map<size_t, std::vector<sstables::shared_sstable>> buckets;

    bool found;
    for (auto& pair : sorted_sstables) {
        found = false;
        size_t size = pair.second;

        // look for a bucket containing similar-sized files:
        // group in the same bucket if it's w/in 50% of the average for this bucket,
        // or this file and the bucket are all considered "small" (less than `minSSTableSize`)
        for (auto& entry : buckets) {
            std::vector<sstables::shared_sstable> bucket = entry.second;
            size_t old_average_size = entry.first;

            if (((size > (old_average_size * _options.bucket_low) && size < (old_average_size * _options.bucket_high))
                || (size < _options.min_sstable_size && old_average_size < _options.min_sstable_size))
                && (bucket.size() < max_threshold))
            {
                size_t total_size = bucket.size() * old_average_size;
                size_t new_average_size = (total_size + size) / (bucket.size() + 1);

                bucket.push_back(pair.first);
                buckets.erase(old_average_size);
                buckets.insert({ new_average_size, std::move(bucket) });

                found = true;
                break;
            }
        }

        // no similar bucket found; put it in a new one
        if (!found) {
            std::vector<sstables::shared_sstable> new_bucket;
            new_bucket.push_back(pair.first);
            buckets.insert({ size, std::move(new_bucket) });
        }
    }

    std::vector<std::vector<sstables::shared_sstable>> bucket_list;
    bucket_list.reserve(buckets.size());

    for (auto& entry : buckets) {
        bucket_list.push_back(std::move(entry.second));
    }

    return bucket_list;
}

std::vector<sstables::shared_sstable>
size_tiered_compaction_strategy::most_interesting_bucket(std::vector<std::vector<sstables::shared_sstable>> buckets,
        unsigned min_threshold, unsigned max_threshold)
{
    std::vector<std::pair<std::vector<sstables::shared_sstable>, uint64_t>> pruned_buckets_and_hotness;
    pruned_buckets_and_hotness.reserve(buckets.size());

    // FIXME: add support to get hotness for each bucket.

    for (auto& bucket : buckets) {
        // FIXME: the coldest sstables will be trimmed to meet the threshold, so we must add support to this feature
        // by converting SizeTieredCompactionStrategy::trimToThresholdWithHotness.
        // By the time being, we will only compact buckets that meet the threshold.
        if (bucket.size() >= min_threshold && bucket.size() <= max_threshold) {
            auto avg = avg_size(bucket);
            pruned_buckets_and_hotness.push_back({ std::move(bucket), avg });
        }
    }

    if (pruned_buckets_and_hotness.empty()) {
        return std::vector<sstables::shared_sstable>();
    }

    // NOTE: Compacting smallest sstables first, located at the beginning of the sorted vector.
    auto& min = *std::min_element(pruned_buckets_and_hotness.begin(), pruned_buckets_and_hotness.end(), [] (auto& i, auto& j) {
        // FIXME: ignoring hotness by the time being.

        return i.second < j.second;
    });
    auto hottest = std::move(min.first);

    return hottest;
}

compaction_descriptor size_tiered_compaction_strategy::get_sstables_for_compaction(column_family& cfs, std::vector<sstables::shared_sstable> candidates) {
    // make local copies so they can't be changed out from under us mid-method
    int min_threshold = cfs.schema()->min_compaction_threshold();
    int max_threshold = cfs.schema()->max_compaction_threshold();

    // TODO: Add support to filter cold sstables (for reference: SizeTieredCompactionStrategy::filterColdSSTables).

    auto buckets = get_buckets(candidates, max_threshold);

    std::vector<sstables::shared_sstable> most_interesting = most_interesting_bucket(std::move(buckets), min_threshold, max_threshold);
    if (most_interesting.empty()) {
        // nothing to do
        return sstables::compaction_descriptor();
    }

    return sstables::compaction_descriptor(std::move(most_interesting));
}

int64_t size_tiered_compaction_strategy::estimated_pending_compactions(column_family& cf) const {
    int min_threshold = cf.schema()->min_compaction_threshold();
    int max_threshold = cf.schema()->max_compaction_threshold();
    std::vector<sstables::shared_sstable> sstables;
    int64_t n = 0;

    sstables.reserve(cf.sstables_count());
    for (auto& entry : *cf.get_sstables()) {
        sstables.push_back(entry);
    }

    for (auto& bucket : get_buckets(sstables, max_threshold)) {
        if (bucket.size() >= size_t(min_threshold)) {
            n += std::ceil(double(bucket.size()) / max_threshold);
        }
    }
    return n;
}

std::vector<sstables::shared_sstable> size_tiered_most_interesting_bucket(lw_shared_ptr<sstable_list> candidates) {
    size_tiered_compaction_strategy cs;

    std::vector<sstables::shared_sstable> sstables;
    sstables.reserve(candidates->size());
    for (auto& entry : *candidates) {
        sstables.push_back(entry);
    }

    auto buckets = cs.get_buckets(sstables, DEFAULT_MAX_COMPACTION_THRESHOLD);

    std::vector<sstables::shared_sstable> most_interesting = cs.most_interesting_bucket(std::move(buckets),
        DEFAULT_MIN_COMPACTION_THRESHOLD, DEFAULT_MAX_COMPACTION_THRESHOLD);

    return most_interesting;
}

std::vector<sstables::shared_sstable>
size_tiered_most_interesting_bucket(const std::list<sstables::shared_sstable>& candidates) {
    size_tiered_compaction_strategy cs;

    std::vector<sstables::shared_sstable> sstables(candidates.begin(), candidates.end());

    auto buckets = cs.get_buckets(sstables, DEFAULT_MAX_COMPACTION_THRESHOLD);

    std::vector<sstables::shared_sstable> most_interesting = cs.most_interesting_bucket(std::move(buckets),
        DEFAULT_MIN_COMPACTION_THRESHOLD, DEFAULT_MAX_COMPACTION_THRESHOLD);

    return most_interesting;
}

class leveled_compaction_strategy : public compaction_strategy_impl {
    static constexpr int32_t DEFAULT_MAX_SSTABLE_SIZE_IN_MB = 160;
    const sstring SSTABLE_SIZE_OPTION = "sstable_size_in_mb";

    int32_t _max_sstable_size_in_mb = DEFAULT_MAX_SSTABLE_SIZE_IN_MB;
public:
    leveled_compaction_strategy(const std::map<sstring, sstring>& options) {
        using namespace cql3::statements;

        auto tmp_value = size_tiered_compaction_strategy_options::get_value(options, SSTABLE_SIZE_OPTION);
        _max_sstable_size_in_mb = property_definitions::to_int(SSTABLE_SIZE_OPTION, tmp_value, DEFAULT_MAX_SSTABLE_SIZE_IN_MB);
        if (_max_sstable_size_in_mb >= 1000) {
            logger.warn("Max sstable size of {}MB is configured; having a unit of compaction this large is probably a bad idea",
                _max_sstable_size_in_mb);
        } else if (_max_sstable_size_in_mb < 50) {
            logger.warn("Max sstable size of {}MB is configured. Testing done for CASSANDRA-5727 indicates that performance improves up to 160MB",
                _max_sstable_size_in_mb);
        }
    }

    virtual compaction_descriptor get_sstables_for_compaction(column_family& cfs, std::vector<sstables::shared_sstable> candidates) override;

    virtual int64_t estimated_pending_compactions(column_family& cf) const override;

    virtual bool parallel_compaction() const override {
        return false;
    }

    virtual compaction_strategy_type type() const {
        return compaction_strategy_type::leveled;
    }
    virtual std::unique_ptr<sstable_set_impl> make_sstable_set(schema_ptr schema) const override {
        return std::make_unique<partitioned_sstable_set>(std::move(schema));
    }
};

compaction_descriptor leveled_compaction_strategy::get_sstables_for_compaction(column_family& cfs, std::vector<sstables::shared_sstable> candidates) {
    // NOTE: leveled_manifest creation may be slightly expensive, so later on,
    // we may want to store it in the strategy itself. However, the sstable
    // lists managed by the manifest may become outdated. For example, one
    // sstable in it may be marked for deletion after compacted.
    // Currently, we create a new manifest whenever it's time for compaction.
    leveled_manifest manifest = leveled_manifest::create(cfs, candidates, _max_sstable_size_in_mb);
    auto candidate = manifest.get_compaction_candidates();

    if (candidate.sstables.empty()) {
        return sstables::compaction_descriptor();
    }

    logger.debug("leveled: Compacting {} out of {} sstables", candidate.sstables.size(), cfs.get_sstables()->size());

    return std::move(candidate);
}

int64_t leveled_compaction_strategy::estimated_pending_compactions(column_family& cf) const {
    std::vector<sstables::shared_sstable> sstables;
    sstables.reserve(cf.sstables_count());
    for (auto& entry : *cf.get_sstables()) {
        sstables.push_back(entry);
    }
    leveled_manifest manifest = leveled_manifest::create(cf, sstables, _max_sstable_size_in_mb);
    return manifest.get_estimated_tasks();
}

class date_tiered_compaction_strategy : public compaction_strategy_impl {
    date_tiered_manifest _manifest;
public:
    date_tiered_compaction_strategy(const std::map<sstring, sstring>& options)
        : _manifest(options)
    {
        _use_clustering_key_filter = true;
    }

    virtual compaction_descriptor get_sstables_for_compaction(column_family& cfs, std::vector<sstables::shared_sstable> candidates) override {
        auto gc_before = gc_clock::now() - cfs.schema()->gc_grace_seconds();
        auto sstables = _manifest.get_next_sstables(cfs, candidates, gc_before);
        logger.debug("datetiered: Compacting {} out of {} sstables", sstables.size(), candidates.size());
        if (sstables.empty()) {
            return sstables::compaction_descriptor();
        }
        return sstables::compaction_descriptor(std::move(sstables));
    }

    virtual int64_t estimated_pending_compactions(column_family& cf) const override {
        return _manifest.get_estimated_tasks(cf);
    }

    virtual compaction_strategy_type type() const {
        return compaction_strategy_type::date_tiered;
    }
};

compaction_strategy::compaction_strategy(::shared_ptr<compaction_strategy_impl> impl)
    : _compaction_strategy_impl(std::move(impl)) {}
compaction_strategy::compaction_strategy() = default;
compaction_strategy::~compaction_strategy() = default;
compaction_strategy::compaction_strategy(const compaction_strategy&) = default;
compaction_strategy::compaction_strategy(compaction_strategy&&) = default;
compaction_strategy& compaction_strategy::operator=(compaction_strategy&&) = default;

compaction_strategy_type compaction_strategy::type() const {
    return _compaction_strategy_impl->type();
}

compaction_descriptor compaction_strategy::get_sstables_for_compaction(column_family& cfs, std::vector<sstables::shared_sstable> candidates) {
    return _compaction_strategy_impl->get_sstables_for_compaction(cfs, std::move(candidates));
}

bool compaction_strategy::parallel_compaction() const {
    return _compaction_strategy_impl->parallel_compaction();
}

int64_t compaction_strategy::estimated_pending_compactions(column_family& cf) const {
    return _compaction_strategy_impl->estimated_pending_compactions(cf);
}

bool compaction_strategy::use_clustering_key_filter() const {
    return _compaction_strategy_impl->use_clustering_key_filter();
}

sstable_set
compaction_strategy::make_sstable_set(schema_ptr schema) const {
    return sstable_set(
            _compaction_strategy_impl->make_sstable_set(std::move(schema)),
            make_lw_shared<sstable_list>());
}

compaction_strategy make_compaction_strategy(compaction_strategy_type strategy, const std::map<sstring, sstring>& options) {
    ::shared_ptr<compaction_strategy_impl> impl;

    switch(strategy) {
    case compaction_strategy_type::null:
        impl = make_shared<null_compaction_strategy>(null_compaction_strategy());
        break;
    case compaction_strategy_type::major:
        impl = make_shared<major_compaction_strategy>(major_compaction_strategy());
        break;
    case compaction_strategy_type::size_tiered:
        impl = make_shared<size_tiered_compaction_strategy>(size_tiered_compaction_strategy(options));
        break;
    case compaction_strategy_type::leveled:
        impl = make_shared<leveled_compaction_strategy>(leveled_compaction_strategy(options));
        break;
    case compaction_strategy_type::date_tiered:
        impl = make_shared<date_tiered_compaction_strategy>(date_tiered_compaction_strategy(options));
        break;
    default:
        throw std::runtime_error("strategy not supported");
    }

    return compaction_strategy(std::move(impl));
}

}
