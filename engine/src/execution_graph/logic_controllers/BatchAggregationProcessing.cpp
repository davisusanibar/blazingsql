#include "BatchAggregationProcessing.h"
#include "taskflow/executor.h"
#include "utilities/CommonOperations.h"
#include <cudf/partitioning.hpp>

namespace ral {
namespace batch {

// BEGIN ComputeAggregateKernel

ComputeAggregateKernel::ComputeAggregateKernel(std::size_t kernel_id, const std::string & queryString, std::shared_ptr<Context> context, std::shared_ptr<ral::cache::graph> query_graph)
    : kernel{kernel_id, queryString, context, kernel_type::ComputeAggregateKernel} {
    this->query_graph = query_graph;
}

void ComputeAggregateKernel::do_process(std::vector< std::unique_ptr<ral::frame::BlazingTable> > inputs,
    std::shared_ptr<ral::cache::CacheMachine> output,
    cudaStream_t stream, const std::map<std::string, std::string>& args) {
    auto & input = inputs[0];

    std::unique_ptr<ral::frame::BlazingTable> columns;
    if(this->aggregation_types.size() == 0) {
        columns = ral::operators::compute_groupby_without_aggregations(
                input->toBlazingTableView(), this->group_column_indices);
    } else if (this->group_column_indices.size() == 0) {
        columns = ral::operators::compute_aggregations_without_groupby(
                input->toBlazingTableView(), aggregation_input_expressions, this->aggregation_types, aggregation_column_assigned_aliases);
    } else {
        columns = ral::operators::compute_aggregations_with_groupby(
            input->toBlazingTableView(), aggregation_input_expressions, this->aggregation_types, aggregation_column_assigned_aliases, group_column_indices);
    }

    output->addToCache(std::move(columns));
}

kstatus ComputeAggregateKernel::run() {
    CodeTimer timer;

    std::tie(this->group_column_indices, aggregation_input_expressions, this->aggregation_types,
        aggregation_column_assigned_aliases) = ral::operators::parseGroupByExpression(this->expression);

    std::unique_ptr <ral::cache::CacheData> cache_data = this->input_cache()->pullCacheData();
    while(cache_data != nullptr ){
        std::vector<std::unique_ptr <ral::cache::CacheData> > inputs;
        inputs.push_back(std::move(cache_data));

        ral::execution::executor::get_instance()->add_task(
                std::move(inputs),
                this->output_cache(),
                this);

        cache_data = this->input_cache()->pullCacheData();
    }

    if(logger != nullptr) {
        logger->debug("{query_id}|{step}|{substep}|{info}|{duration}|kernel_id|{kernel_id}||",
                                "query_id"_a=context->getContextToken(),
                                "step"_a=context->getQueryStep(),
                                "substep"_a=context->getQuerySubstep(),
                                "info"_a="Compute Aggregate Kernel tasks created",
                                "duration"_a=timer.elapsed_time(),
                                "kernel_id"_a=this->get_id());
    }

    std::unique_lock<std::mutex> lock(kernel_mutex);
    kernel_cv.wait(lock,[this]{
        return this->tasks.empty();
    });

    if(logger != nullptr) {
        logger->debug("{query_id}|{step}|{substep}|{info}|{duration}|kernel_id|{kernel_id}||",
                    "query_id"_a=context->getContextToken(),
                    "step"_a=context->getQueryStep(),
                    "substep"_a=context->getQuerySubstep(),
                    "info"_a="ComputeAggregate Kernel Completed",
                    "duration"_a=timer.elapsed_time(),
                    "kernel_id"_a=this->get_id());
    }
    return kstatus::proceed;
}

std::pair<bool, uint64_t> ComputeAggregateKernel::get_estimated_output_num_rows(){
    if(this->aggregation_types.size() > 0 && this->group_column_indices.size() == 0) { // aggregation without groupby
        return std::make_pair(true, 1);
    } else {
        std::pair<bool, uint64_t> total_in = this->query_graph->get_estimated_input_rows_to_kernel(this->kernel_id);
        if (total_in.first){
            double out_so_far = (double)this->output_.total_rows_added();
            double in_so_far = (double)this->input_.total_rows_added();
            if (in_so_far == 0) {
                return std::make_pair(false, 0);
            } else {
                return std::make_pair(true, (uint64_t)( ((double)total_in.second) *out_so_far/in_so_far) );
            }
        } else {
            return std::make_pair(false, 0);
        }
    }
}

// END ComputeAggregateKernel

// BEGIN DistributeAggregateKernel

DistributeAggregateKernel::DistributeAggregateKernel(std::size_t kernel_id, const std::string & queryString, std::shared_ptr<Context> context, std::shared_ptr<ral::cache::graph> query_graph)
    : distributing_kernel{kernel_id, queryString, context, kernel_type::DistributeAggregateKernel} {
    this->query_graph = query_graph;
    set_number_of_message_trackers(1); //default
}

void DistributeAggregateKernel::do_process(std::vector< std::unique_ptr<ral::frame::BlazingTable> > inputs,
    std::shared_ptr<ral::cache::CacheMachine> output,
    cudaStream_t stream, const std::map<std::string, std::string>& args) {
    auto & input = inputs[0];

    // num_partitions = context->getTotalNodes() will do for now, but may want a function to determine this in the future.
    // If we do partition into something other than the number of nodes, then we have to use part_ids and change up more of the logic
    int num_partitions = this->context->getTotalNodes();

    // If its an aggregation without group by we want to send all the results to the master node
    auto& self_node = ral::communication::CommunicationData::getInstance().getSelfNode();
    if (group_column_indices.size() == 0) {
        if(this->context->isMasterNode(self_node)) {
            bool added = this->output_.get_cache()->addToCache(std::move(input),"",false);
            if (added) {
                increment_node_count(self_node.id());
            }
        } else {
            if (!set_empty_part_for_non_master_node){ // we want to keep in the non-master nodes something, so that the cache is not empty
                std::unique_ptr<ral::frame::BlazingTable> empty =
                    ral::utilities::create_empty_table(input->toBlazingTableView());
                bool added = this->add_to_output_cache(std::move(empty), "", true);
                set_empty_part_for_non_master_node = true;
                if (added) {
                    increment_node_count(self_node.id());
                }
            }

            send_message(std::move(input),
                true, //specific_cache
                "", //cache_id
                {this->context->getMasterNode().id()}); //target_id
        }
    } else {
        CudfTableView batch_view = input->view();
        std::vector<CudfTableView> partitioned;
        std::unique_ptr<CudfTable> hashed_data; // Keep table alive in this scope
        if (batch_view.num_rows() > 0) {
            std::vector<cudf::size_type> hased_data_offsets;
            std::tie(hashed_data, hased_data_offsets) = cudf::hash_partition(input->view(), columns_to_hash, num_partitions);
            // the offsets returned by hash_partition will always start at 0, which is a value we want to ignore for cudf::split
            std::vector<cudf::size_type> split_indexes(hased_data_offsets.begin() + 1, hased_data_offsets.end());
            partitioned = cudf::split(hashed_data->view(), split_indexes);
        } else {
            //  copy empty view
            for (auto i = 0; i < num_partitions; i++) {
                partitioned.push_back(batch_view);
            }
        }

        std::vector<ral::frame::BlazingTableView> partitions;
        for(auto partition : partitioned) {
            partitions.push_back(ral::frame::BlazingTableView(partition, input->names()));
        }

        scatter(partitions,
            output.get(),
            "", //message_id_prefix
            "" //cache_id
        );
    }
}

kstatus DistributeAggregateKernel::run() {
    CodeTimer timer;

    std::tie(group_column_indices, aggregation_input_expressions, aggregation_types,
        aggregation_column_assigned_aliases) = ral::operators::parseGroupByExpression(this->expression);

    std::transform(group_column_indices.begin(), group_column_indices.end(), std::back_inserter(columns_to_hash), [](int index) { return (cudf::size_type)index; });

    std::unique_ptr <ral::cache::CacheData> cache_data = this->input_cache()->pullCacheData();
    while(cache_data != nullptr ){
        std::vector<std::unique_ptr <ral::cache::CacheData> > inputs;
        inputs.push_back(std::move(cache_data));

        ral::execution::executor::get_instance()->add_task(
                std::move(inputs),
                this->output_cache(),
                this);

        cache_data = this->input_cache()->pullCacheData();
    }

    if(logger != nullptr) {
        logger->debug("{query_id}|{step}|{substep}|{info}|{duration}|kernel_id|{kernel_id}||",
                                "query_id"_a=context->getContextToken(),
                                "step"_a=context->getQueryStep(),
                                "substep"_a=context->getQuerySubstep(),
                                "info"_a="DistributeAggregate Kernel tasks created",
                                "duration"_a=timer.elapsed_time(),
                                "kernel_id"_a=this->get_id());
    }

    std::unique_lock<std::mutex> lock(kernel_mutex);
    kernel_cv.wait(lock,[this]{
        return this->tasks.empty();
    });

    send_total_partition_counts(
        "", //message_prefix
        "" //cache_id
    );

    int total_count = get_total_partition_counts();
    this->output_cache()->wait_for_count(total_count);

    logger->debug("{query_id}|{step}|{substep}|{info}|{duration}|kernel_id|{kernel_id}||",
                "query_id"_a=context->getContextToken(),
                "step"_a=context->getQueryStep(),
                "substep"_a=context->getQuerySubstep(),
                "info"_a="DistributeAggregate Kernel Completed",
                "duration"_a=timer.elapsed_time(),
                "kernel_id"_a=this->get_id());

    return kstatus::proceed;
}

// END DistributeAggregateKernel

// BEGIN MergeAggregateKernel

MergeAggregateKernel::MergeAggregateKernel(std::size_t kernel_id, const std::string & queryString, std::shared_ptr<Context> context, std::shared_ptr<ral::cache::graph> query_graph)
    : kernel{kernel_id, queryString, context, kernel_type::MergeAggregateKernel} {
    this->query_graph = query_graph;
}

void MergeAggregateKernel::do_process(std::vector< std::unique_ptr<ral::frame::BlazingTable> > inputs,
    std::shared_ptr<ral::cache::CacheMachine> output,
    cudaStream_t stream, const std::map<std::string, std::string>& args) {
    CodeTimer eventTimer(false);

    std::vector< ral::frame::BlazingTableView > tableViewsToConcat;
    for (std::size_t i = 0; i < inputs.size(); i++){
        tableViewsToConcat.emplace_back(inputs[i]->toBlazingTableView());
    }

    eventTimer.start();
    if( ral::utilities::checkIfConcatenatingStringsWillOverflow(tableViewsToConcat)) {
        logger->warn("{query_id}|{step}|{substep}|{info}",
                        "query_id"_a=(context ? std::to_string(context->getContextToken()) : ""),
                        "step"_a=(context ? std::to_string(context->getQueryStep()) : ""),
                        "substep"_a=(context ? std::to_string(context->getQuerySubstep()) : ""),
                        "info"_a="In MergeAggregateKernel::run Concatenating Strings will overflow strings length");
    }
    auto concatenated = ral::utilities::concatTables(tableViewsToConcat);

    auto log_input_num_rows = concatenated ? concatenated->num_rows() : 0;
    auto log_input_num_bytes = concatenated ? concatenated->sizeInBytes() : 0;

    std::vector<int> group_column_indices;
    std::vector<std::string> aggregation_input_expressions, aggregation_column_assigned_aliases;
    std::vector<AggregateKind> aggregation_types;
    std::tie(group_column_indices, aggregation_input_expressions, aggregation_types,
        aggregation_column_assigned_aliases) = ral::operators::parseGroupByExpression(this->expression);

    std::vector<int> mod_group_column_indices;
    std::vector<std::string> mod_aggregation_input_expressions, mod_aggregation_column_assigned_aliases, merging_column_names;
    std::vector<AggregateKind> mod_aggregation_types;
    std::tie(mod_group_column_indices, mod_aggregation_input_expressions, mod_aggregation_types,
        mod_aggregation_column_assigned_aliases) = ral::operators::modGroupByParametersForMerge(
        group_column_indices, aggregation_types, concatenated->names());

    std::unique_ptr<ral::frame::BlazingTable> columns;
    if(aggregation_types.size() == 0) {
        columns = ral::operators::compute_groupby_without_aggregations(
                concatenated->toBlazingTableView(), mod_group_column_indices);
    } else if (group_column_indices.size() == 0) {
        // aggregations without groupby are only merged on the master node
        if( context->isMasterNode(ral::communication::CommunicationData::getInstance().getSelfNode()) ) {
            columns = ral::operators::compute_aggregations_without_groupby(
                    concatenated->toBlazingTableView(), mod_aggregation_input_expressions, mod_aggregation_types,
                    mod_aggregation_column_assigned_aliases);
        } else {
            // with aggregations without groupby the distribution phase should deposit an empty dataframe with the right schema into the cache, which is then output here
            columns = std::move(concatenated);
        }
    } else {
        columns = ral::operators::compute_aggregations_with_groupby(
                concatenated->toBlazingTableView(), mod_aggregation_input_expressions, mod_aggregation_types,
                mod_aggregation_column_assigned_aliases, mod_group_column_indices);
    }
    eventTimer.stop();

    auto log_output_num_rows = columns->num_rows();
    auto log_output_num_bytes = columns->sizeInBytes();

    events_logger->info("{ral_id}|{query_id}|{kernel_id}|{input_num_rows}|{input_num_bytes}|{output_num_rows}|{output_num_bytes}|{event_type}|{timestamp_begin}|{timestamp_end}",
                    "ral_id"_a=context->getNodeIndex(ral::communication::CommunicationData::getInstance().getSelfNode()),
                    "query_id"_a=context->getContextToken(),
                    "kernel_id"_a=this->get_id(),
                    "input_num_rows"_a=log_input_num_rows,
                    "input_num_bytes"_a=log_input_num_bytes,
                    "output_num_rows"_a=log_output_num_rows,
                    "output_num_bytes"_a=log_output_num_bytes,
                    "event_type"_a="compute",
                    "timestamp_begin"_a=eventTimer.start_time(),
                    "timestamp_end"_a=eventTimer.end_time());

    output->addToCache(std::move(columns));
}

kstatus MergeAggregateKernel::run() {
    CodeTimer timer;

    // This Kernel needs all of the input before it can do any output. So lets wait until all the input is available
    this->input_cache()->wait_until_finished();

    int batch_count=0;
    try {
        std::vector<std::unique_ptr <ral::cache::CacheData> > inputs;

        while(this->input_cache()->wait_for_next()){
            std::unique_ptr <ral::cache::CacheData> cache_data = this->input_cache()->pullCacheData();

            if(cache_data != nullptr){
                inputs.push_back(std::move(cache_data));
                batch_count++;
            }
        }

        ral::execution::executor::get_instance()->add_task(
                std::move(inputs),
                this->output_cache(),
                this);

        logger->debug("{query_id}|{step}|{substep}|{info}|{duration}|kernel_id|{kernel_id}||",
                                    "query_id"_a=context->getContextToken(),
                                    "step"_a=context->getQueryStep(),
                                    "substep"_a=context->getQuerySubstep(),
                                    "info"_a="Merge Aggregate Kernel tasks created",
                                    "duration"_a=timer.elapsed_time(),
                                    "kernel_id"_a=this->get_id());

        std::unique_lock<std::mutex> lock(kernel_mutex);
        kernel_cv.wait(lock,[this]{
            return this->tasks.empty();
        });
    } catch(const std::exception& e) {
        // TODO add retry here
        logger->error("{query_id}|{step}|{substep}|{info}|{duration}||||",
                    "query_id"_a=context->getContextToken(),
                    "step"_a=context->getQueryStep(),
                    "substep"_a=context->getQuerySubstep(),
                    "info"_a="In MergeAggregate kernel for {}. What: {}"_format(expression, e.what()),
                    "duration"_a="");
        throw;
    }

    logger->debug("{query_id}|{step}|{substep}|{info}|{duration}|kernel_id|{kernel_id}||",
                "query_id"_a=context->getContextToken(),
                "step"_a=context->getQueryStep(),
                "substep"_a=context->getQuerySubstep(),
                "info"_a="MergeAggregate Kernel Completed",
                "duration"_a=timer.elapsed_time(),
                "kernel_id"_a=this->get_id());

    return kstatus::proceed;
}

// END MergeAggregateKernel

} // namespace batch
} // namespace ral
