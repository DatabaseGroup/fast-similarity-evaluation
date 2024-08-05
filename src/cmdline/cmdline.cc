#include <boost/program_options.hpp>
#include <variant>

#include "../data/parser.hh"
#include "../indexing/index.hh"
#include "../join/blockslice.hh"
#include "../join/timeslice.hh"
#include "../statistics/join_statistics.hh"
#include "../timing/join_timing.hh"
#include "../util/git_sha.hh"

struct Config {
  std::string input_file;
  int64_t read_file_until{};
  std::string datatype;
  std::string similarity;
  double threshold{};
  int64_t batch_count{};
  int64_t reduction_cache_size{};
  int64_t probing_signatures_cache_size{};
  std::string label;
  std::string mode;
  std::vector<std::string> excluded_algorithms;
  std::vector<std::string> excluded_reductions;
};

bool process_program_options(int argc, char** argv, Config& config) {
  namespace po = boost::program_options;

  po::options_description optdesc{"DESCRIPTION"};
  optdesc.add_options()("input-file,f", po::value(&config.input_file)->required(), "Specify input file")(
    "datatype,d", po::value(&config.datatype)->required(), "Specify datatype (set, string, tree)")(
    "similarity,s", po::value(&config.similarity)->required(), "Specify similarity measure")(
    "threshold,t", po::value(&config.threshold)->required(), "Threshold")(
    "batch-count,b", po::value(&config.batch_count)->default_value(20), "Number of batches to split the data into")(
    "label,l", po::value(&config.label), "label for the run (printed in json)")(
    "exclude-algorithm,x", po::value(&config.excluded_algorithms)->multitoken(), "Excluded algorithms")(
    "exclude-reduction,y", po::value(&config.excluded_reductions)->multitoken(), "Excluded reductions")(
    "probe-cache-size,p",
    po::value(&config.probing_signatures_cache_size)->default_value(20),
    "Probing signatures cache size")(
    "reduction-cache-size,r", po::value(&config.reduction_cache_size)->default_value(20), "Reduction Cache Size")(
    "read-until,u",
    po::value(&config.read_file_until)->default_value(std::numeric_limits<int64_t>::max()),
    "Read the first X lines of the input")(
    "mode,m", po::value(&config.mode)->default_value("block"), "Mode of interleaving: block, time-static");

  const std::string exec_name(argv[0]);

  try {
    po::variables_map vm;
    po::command_line_parser parser(argc, argv);
    po::store(parser.options(optdesc).run(), vm);

    po::notify(vm);  // update variables map
  } catch (const std::exception& e) {
    std::cerr << "Error: " << e.what() << "\n";
    std::cerr << optdesc << "\n";
    return false;
  }

  return true;
}

nlohmann::json get_build_info() {
  nlohmann::json json;

#ifdef STATISTICS
  json["type"] = "statistics";
#else
  json["type"] = "timing";
#endif
  json["commit_sha1"] = util::GIT_SHA;

  return json;
}

nlohmann::json getISOCurrentTimestamp() {
  nlohmann::json time;
  absl::Time t1 = absl::Now();
  time["$date"] = absl::FormatTime("%Y-%m-%d%ET%H:%M:%SZ", t1, absl::UTCTimeZone());
  return time;
}

nlohmann::json get_metadata(Config& config) {
  nlohmann::json json;

  json["build"] = get_build_info();
  json["datatype"] = config.datatype;
  json["date"] = getISOCurrentTimestamp();
  json["dataset"] = std::filesystem::path(config.input_file).filename();
  json["similarity"] = config.similarity;
  json["threshold"] = config.threshold;

  json["label"] = config.label;
  json["mode"] = config.mode;
  if (config.mode == "block") {
    json["batch_count"] = config.batch_count;
    json["reduction_cache_size"] = config.reduction_cache_size;
    json["probing_signatures_cache_size"] = config.probing_signatures_cache_size;
  }

  return json;
}

std::pair<similarity::SimilarityId, similarity::Similarity> resolve_similarity(const std::string& sim_str,
                                                                               const double threshold,
                                                                               data::Dataset& dataset) {
  similarity::SimilarityId sim_id;
  similarity::Similarity sim;

  // this could be replaced by a hashtable, but who cares?
  if (sim_str == "sed") {
    sim = std::make_unique<similarity::StringEditDistance>(threshold);
    sim_id = similarity::SimilarityId::STRING_EDIT_DISTANCE;
  } else if (sim_str == "hd") {
    sim = std::make_unique<similarity::HammingDistance>(threshold);
    sim_id = similarity::SimilarityId::HAMMING_DISTANCE;
  } else if (sim_str == "ted") {
    auto& trees = std::get<types::Trees>(dataset.data);
    sim = std::make_unique<similarity::TreeEditDistance>(threshold, trees.meta.label_dict, trees.meta.cost_model);
    sim_id = similarity::SimilarityId::TREE_EDIT_DISTANCE;
  } else if (sim_str == "jaccard") {
    sim = std::make_unique<similarity::JaccardSimilarity>(threshold);
    sim_id = similarity::SimilarityId::JACCARD;
  } else if (sim_str == "jaro") {
    sim = std::make_unique<similarity::JaroSimilarity>(threshold);
    sim_id = similarity::SimilarityId::JARO_STRING;
  } else {
    throw std::invalid_argument("Similarity \"" + sim_str + "\" unknown.");
  }
  return {sim_id, std::move(sim)};
}

std::pair<types::DatatypeId, data::Dataset> resolve_data(const std::string& data_str,
                                                         const std::string& filepath,
                                                         const int64_t until_line_number) {
  types::DatatypeId data_id;

  // this could be replaced by a hashtable, but who cares?
  if (data_str == "set") {
    data_id = types::DatatypeId::SET;
    data::SetParser set_parser;
    return {data_id, set_parser.parse_until(filepath, until_line_number)};
  } else if (data_str == "string") {
    data_id = types::DatatypeId::STRING;
    data::StringParser string_parser;
    return {data_id, string_parser.parse_until(filepath, until_line_number)};
  } else if (data_str == "tree") {
    data_id = types::DatatypeId::TREE;
    data::TreeParser tree_parser;
    return {data_id, tree_parser.parse_until(filepath, until_line_number)};
  }

  throw std::invalid_argument("Data type \"" + data_str + "\" unknown.");
}

nlohmann::json plan_to_json(ontology::QueryPlan& plan) {
  nlohmann::json plan_json;

  plan_json["algorithm"] = join::algorithm_to_string(plan.algorithm_id);

  std::vector<std::string> reduction_steps;
  std::for_each(plan.steps.begin(), plan.steps.end(), [&](ontology::ReductionStep& step) {
    reduction_steps.emplace_back(step.reduction.get().get_label());
  });

  plan_json["reduction"] = reduction_steps;

  return plan_json;
}

std::vector<statistics::LocalJoinStatistics> setup_statistics(std::vector<ontology::QueryPlan>& plans) {
  std::vector<statistics::LocalJoinStatistics> statistics;
  for (auto& plan : plans) {
    statistics.emplace_back(plan_to_json(plan));
  }

  return statistics;
}

statistics::JoinStatistics sum_statistics(std::vector<statistics::LocalJoinStatistics>& statistics) {
  return std::reduce(statistics.begin(), statistics.end(), statistics::JoinStatistics());
}

std::vector<join::AlgorithmId> find_excluded_algorithms(std::vector<std::string>& alg_list) {
  std::vector<join::AlgorithmId> res;
  std::for_each(alg_list.begin(), alg_list.end(), [&](auto& str) { res.push_back(join::string_to_algorithm(str)); });
  return res;
}

int main(int argc, char** argv) {
  Config config;

  if (!process_program_options(argc, argv, config)) {
    exit(-1);
  }

  auto [data_id, dataset] = resolve_data(config.datatype, config.input_file, config.read_file_until);
  auto [similarity_id, similarity] = resolve_similarity(config.similarity, config.threshold, dataset);

  ontology::StandardReductionGraph graph;
  ontology::PlannerConfiguration plan_config;
  plan_config.excluded_algorithms = find_excluded_algorithms(config.excluded_algorithms);
  plan_config.excluded_reductions = config.excluded_reductions;
  auto plans = graph.enumerate_plans(data_id, similarity_id, plan_config);

  std::vector<statistics::LocalJoinStatistics> statistics = setup_statistics(plans);
  std::unique_ptr<timing::JoinTiming> timing;

  nlohmann::json result;
  result["meta"] = get_metadata(config);
  result["dataset_statistics"] = dataset.statistics->to_json();

  if (config.mode == "block") {
    // cache size has to be at least 1
    config.reduction_cache_size = std::max(INT64_C(1), config.reduction_cache_size);
    config.probing_signatures_cache_size = std::max(INT64_C(1), config.probing_signatures_cache_size);
    // batch count is always at most the size of the dataset
    config.batch_count = std::min(dataset.statistics->count, config.batch_count);

    join::PlanExecutor executor(config.batch_count, config.reduction_cache_size, config.probing_signatures_cache_size);
    timing = std::make_unique<timing::JoinTiming>();
    timing->join_time.start();
    executor.execute_plans(dataset, similarity, plans, statistics);
    timing->join_time.stop();
  } else {
    timing::TimeStaticJoinTiming tsj_timing;;
    join::execute_timeslice_prebuilt(dataset, similarity, plans, tsj_timing, statistics);
    timing = std::make_unique<timing::TimeStaticJoinTiming>(std::move(tsj_timing));
  }

  nlohmann::json local_statistics;
  for (size_t i = 0; i < plans.size(); ++i) {
    local_statistics[plans[i].to_string()] = statistics[i].to_json();
  }
  statistics::JoinStatistics global_statistics = sum_statistics(statistics);

  result["local_statistics"] = local_statistics;
  result["global_statistics"] = global_statistics.to_json();
  result["timing"] = timing->to_json();

  std::cout << result.dump(4) << std::endl;

  return 0;
}
