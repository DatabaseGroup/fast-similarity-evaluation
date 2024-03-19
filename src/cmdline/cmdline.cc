#include <boost/program_options.hpp>
#include <variant>

#include "../data/parser.hh"
#include "../indexing/index.hh"
#include "../join/plan_execution.hh"
#include "../statistics/join_statistics.hh"
#include "../util/git_sha.hh"
#include "../timing/join_timing.hh"

struct Config {
  std::string input_file;
  std::string datatype;
  std::string similarity;
  double threshold{};
  int64_t batch_count{};
  std::string label;
  std::vector<std::string> excluded_algorithms;
};

bool process_program_options(int argc, char** argv, Config& config) {
  namespace po = boost::program_options;

  po::options_description optdesc{"DESCRIPTION"};
  optdesc.add_options()("input-file,f", po::value(&config.input_file)->required(), "Specify input file")(
    "datatype,d", po::value(&config.datatype)->required(), "Specify datatype (set, string, tree)")(
    "similarity,s", po::value(&config.similarity)->required(), "Specify similarity measure")(
    "threshold,t", po::value(&config.threshold)->required(), "Threshold")(
    "batch-count,b", po::value(&config.batch_count)->default_value(20), "Number of batches to split the data into")(
    "label,l", po::value(&config.label), "label for the run (printed in json)")("exclude,x", po::value(&config.excluded_algorithms)->multitoken(), "Excluded algorithms");

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

  json["date"] = getISOCurrentTimestamp();
  json["build"] = get_build_info();
  json["dataset"] = std::filesystem::path(config.input_file).filename();
  json["similarity"] = config.similarity;
  json["threshold"] = config.threshold;
  json["batch_count"] = config.batch_count;
  json["datatype"] = config.datatype;
  json["label"] = config.label;

  return json;
}

std::pair<similarity::SimilarityId, similarity::Similarity> resolve_similarity(const std::string& sim_str,
                                                                               const double threshold) {
  similarity::SimilarityId sim_id;
  similarity::Similarity sim;

  // this could be replaced by a hashtable, but who cares?
  if (sim_str == "sed") {
    sim = std::make_unique<similarity::SEDSimilarity>(threshold);
    sim_id = similarity::SimilarityId::STRING_EDIT_DISTANCE;
  } /*else if (sim_str == "jaccard") {  // todo fix jaccard
    sim = std::make_unique<similarity::JaccardSimilarity>(threshold);
    sim_id = similarity::SimilarityId::STRING_EDIT_DISTANCE;
  }*/
  return {sim_id, std::move(sim)};
}

std::pair<types::DatatypeId, data::Dataset> resolve_data(const std::string& data_str, const std::string& filepath) {
  types::DatatypeId data_id;
  data::Dataset dataset;

  // this could be replaced by a hashtable, but who cares?
  if (data_str == "set") {
    // todo
  } else if (data_str == "string") {
    data_id = types::DatatypeId::STRING;
    data::StringParser string_parser;
    dataset = string_parser.parse(filepath);
  } else if (data_str == "tree") {
    data_id = types::DatatypeId::TREE;
    data::TreeParser tree_parser;
    dataset = tree_parser.parse(filepath);
  }

  return {data_id, std::move(dataset)};
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
  std::for_each(alg_list.begin(), alg_list.end(), [&](auto& str) {
    res.push_back(join::string_to_algorithm(str));
  });
  return res;
}

int main(int argc, char** argv) {
  Config config;

  if (!process_program_options(argc, argv, config)) {
    exit(-1);
  }

  auto [similarity_id, similarity] = resolve_similarity(config.similarity, config.threshold);
  auto [data_id, dataset] = resolve_data(config.datatype, config.input_file);

  ontology::StandardReductionGraph graph;
  ontology::PlannerConfiguration plan_config;
  plan_config.excluded_algorithms = find_excluded_algorithms(config.excluded_algorithms);
  auto plans = graph.enumerate_plans(data_id, similarity_id, plan_config);

  std::vector<statistics::LocalJoinStatistics> statistics = setup_statistics(plans);
  timing::JoinTiming timing;

  // cache size has to be at least 1
  join::PlanExecutor executor(config.batch_count, join::PlanExecutor::get_allpairs_batches(config.batch_count) + 1);
  timing.join_time.start();
  executor.execute_plans(dataset, similarity, plans, statistics);
  timing.join_time.stop();

  nlohmann::json result;
  result["meta"] = get_metadata(config);

  nlohmann::json local_statistics;
  for (size_t i = 0; i < plans.size(); ++i) {
    local_statistics[plans[i].to_string()] = statistics[i].to_json();
  }
  statistics::JoinStatistics global_statistics = sum_statistics(statistics);

  result["local_statistics"] = local_statistics;
  result["global_statistics"] = global_statistics.to_json();
  result["timing"] = timing.to_json();

  std::cout << result.dump(4) << std::endl;

  return 0;
}
