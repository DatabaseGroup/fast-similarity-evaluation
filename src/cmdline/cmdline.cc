#include <boost/program_options.hpp>
#include <variant>
#include <iostream>
#include <sys/ioctl.h>

#include "../data/parser.hh"
#include "../indexing/index.hh"
#include "../join/blockslice.hh"
#include "../join/timeslice_dynamic.hh"
#include "../join/timeslice_static.hh"
#include "../statistics/join_statistics.hh"
#include "../timing/join_timing.hh"
#include "../util/git_sha.hh"
#include "../util/hw_cache.hh"

struct Config {
  std::string input_file;
  bool shuffle{false};
  bool warmup{false};
  bool warmup_flush_hwcache{false};
  int64_t read_file_until{};
  std::string datatype;
  std::string similarity;
  double threshold{};
  int64_t batch_count{};
  int64_t reduction_cache_size{};
  int64_t probing_signatures_cache_size{};
  double timeslice{};
  std::string label;
  std::string mode;
  std::vector<std::string> excluded_algorithms;
  std::vector<std::string> excluded_reductions;
  std::vector<std::string> additional_reductions;
};

uint16_t get_terminal_width() {
  winsize ws{};
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col != 0) {
    return ws.ws_col;
  }
  // fallback
  return 80;
}

bool process_program_options(int argc, char** argv, Config& config) {
  namespace po = boost::program_options;

  po::options_description optdesc{"USAGE", get_terminal_width()};
  optdesc.add_options()("input-file,f", po::value(&config.input_file)->required(), "Specify input file")(
    "shuffle,h", po::bool_switch(&config.shuffle)->default_value(false), "Shuffle the dataset after parsing")(
    "warmup,w",
    po::bool_switch(&config.warmup)->default_value(false),
    "Perform warmup by filling caches before execution (only affects time-dynamic)")(
    "flush-hwcache",
    po::bool_switch(&config.warmup_flush_hwcache)->default_value(false),
    "Try to flush hardware (data-)caches between warmup and execution (only affects time-dynamic)")(
    "datatype,d", po::value(&config.datatype)->required(), "Specify datatype (set, string, tree)")(
    "similarity,s", po::value(&config.similarity)->required(), "Specify similarity function (jaccard, sed, ted, jaro)")(
    "threshold,t", po::value(&config.threshold)->required(), "Threshold of the similarity join")(
    "batch-count,b", po::value(&config.batch_count)->default_value(20), "Number of batches to split the data into, only affects block mode")(
    "label,l", po::value(&config.label), "Label for the run (printed in json)")(
    "exclude-algorithm,x", po::value(&config.excluded_algorithms)->multitoken(), "Excluded algorithms in the reduction graph")(
    "exclude-reduction,y", po::value(&config.excluded_reductions)->multitoken(), "Excluded reductions in the reduction graph")(
    "probe-cache-size,p",
    po::value(&config.probing_signatures_cache_size)->default_value(20),
    "Probing signatures cache size, only affects block mode")(
    "reduction-cache-size,r", po::value(&config.reduction_cache_size)->default_value(20), "Reduction Cache Size, only affects block mode")(
    "read-until,u",
    po::value(&config.read_file_until)->default_value(std::numeric_limits<int64_t>::max()),
    "Read the first X lines of the input, skipping the rest")(
    "mode,m", po::value(&config.mode)->default_value("block"), "Mode of interleaving: block, time-static, time-dynamic")(
    "time-slice,i", po::value(&config.timeslice)->default_value(0.3), "Timeslice in seconds")(
    "additional-reductions,a",
    po::value(&config.additional_reductions)->multitoken(),
    "Enable optional reductions. Currently supported: qX enables X-grams");

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
  json["dataset_shuffled"] = config.shuffle;
  json["dataset_truncated"] = config.read_file_until;
  json["similarity"] = config.similarity;
  json["threshold"] = config.threshold;

  json["label"] = config.label;
  json["mode"] = config.mode;
  if (config.mode == "block") {
    json["batch_count"] = config.batch_count;
    json["reduction_cache_size"] = config.reduction_cache_size;
    json["probing_signatures_cache_size"] = config.probing_signatures_cache_size;
  } else {
    json["timeslice"] = config.timeslice;
    json["warmup"] = config.warmup;
    json["warmup_flush_hwcache"] = config.warmup_flush_hwcache;
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
                                                         const int64_t until_line_number,
                                                         bool shuffle) {
  types::DatatypeId data_id;
  data::Dataset dataset;

  // this could be replaced by a hashtable, but who cares?
  if (data_str == "set") {
    data_id = types::DatatypeId::SET;
    data::SetParser set_parser;
    dataset = set_parser.parse_until(filepath, until_line_number);
  } else if (data_str == "string") {
    data_id = types::DatatypeId::STRING;
    data::StringParser string_parser;
    dataset = string_parser.parse_until(filepath, until_line_number);
  } else if (data_str == "tree") {
    data_id = types::DatatypeId::TREE;
    data::TreeParser tree_parser;
    dataset = tree_parser.parse_until(filepath, until_line_number);
  } else {
    throw std::invalid_argument("Data type \"" + data_str + "\" unknown.");
  }

  if (shuffle) {
    std::visit(
      [](auto& datameta) {
        std::mt19937 prng(std::random_device{}());
        std::shuffle(datameta.data.begin(), datameta.data.end(), prng);

        for (size_t i = 0; i < datameta.data.size(); ++i) {
          datameta.data[i].id = i;
        }
      },
      dataset.data);
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

template <class Statistics>
std::vector<Statistics> setup_statistics(std::vector<ontology::QueryPlan>& plans) {
  std::vector<Statistics> statistics;
  for (auto& plan : plans) {
    statistics.emplace_back(plan_to_json(plan));
    statistics.back().step_verifications.resize(plan.steps.size());
  }

  return statistics;
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

  auto [data_id, dataset] = resolve_data(config.datatype, config.input_file, config.read_file_until, config.shuffle);
  auto [similarity_id, similarity] = resolve_similarity(config.similarity, config.threshold, dataset);

  ontology::StandardReductionGraph graph{config.additional_reductions};
  ontology::PlannerConfiguration plan_config;
  plan_config.excluded_algorithms = find_excluded_algorithms(config.excluded_algorithms);
  plan_config.excluded_reductions = config.excluded_reductions;
  auto plans = graph.enumerate_plans(data_id, similarity_id, plan_config);

  std::unique_ptr<timing::JoinTiming> timing;
  std::unique_ptr<statistics::GlobalJoinStatistics> global_statistics;
  statistics::LocalStatistics local_statistics;

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

    using StatClass = statistics::LocalBlockSliceStatistics;
    auto lls = setup_statistics<StatClass>(plans);
    global_statistics = std::make_unique<statistics::GlobalBlockSliceStatistics>();
    timing = std::make_unique<timing::JoinTiming>();
    timing->join_time.start();
    executor.execute_plans(dataset, similarity, plans, lls);
    timing->join_time.stop();
    std::for_each(
      lls.begin(), lls.end(), [&](auto& s) { local_statistics.emplace_back(std::make_unique<StatClass>(s)); });
  } else if (config.mode == "time-static") {
    using StatClass = statistics::LocalTimeSliceStatistics;
    timing::TimeStaticJoinTiming tsj_timing;

    auto lls = setup_statistics<StatClass>(plans);
    global_statistics = std::make_unique<statistics::GlobalTimeSliceStatistics>();
    join::execute_timeslice_prebuilt(dataset, similarity, plans, config.timeslice, tsj_timing, lls);
    timing = std::make_unique<timing::TimeStaticJoinTiming>(std::move(tsj_timing));
    std::for_each(
      lls.begin(), lls.end(), [&](auto& s) { local_statistics.emplace_back(std::make_unique<StatClass>(s)); });
  } else {
    using StatClass = statistics::LocalDynamicTimeSliceStatistics;
    timing::TimeDynamicJoinTiming tdj_timing;

    // TJoin does not support the required filter configs and updates
    std::erase_if(plans, [](ontology::QueryPlan& p) { return p.algorithm_id == join::TJOIN; });

    auto lls = setup_statistics<StatClass>(plans);
    global_statistics = std::make_unique<statistics::GlobalDynamicTimeSliceStatistics>();
    join::timeslice::DynamicTimeslicing dts(plans, dataset.statistics->count, config.timeslice);
    if (config.warmup) {
      auto temp_lls = setup_statistics<StatClass>(plans);
      timing::TimeDynamicJoinTiming temp_timing;
      dts.execute_join(dataset, similarity, plans, temp_timing, temp_lls);

      if (config.warmup_flush_hwcache) {
        util::try_flush_cache();
      }
    }
    dts.execute_join(dataset, similarity, plans, tdj_timing, lls);
    timing = std::make_unique<timing::TimeDynamicJoinTiming>(std::move(tdj_timing));
    std::for_each(
      lls.begin(), lls.end(), [&](auto& s) { local_statistics.emplace_back(std::make_unique<StatClass>(s)); });
  }

  nlohmann::json lsjson;
  for (size_t i = 0; i < plans.size(); ++i) {
    lsjson[plans[i].to_string()] = local_statistics[i]->to_json();
  }
  statistics::merge_local_statistics(local_statistics, global_statistics);

  result["local_statistics"] = lsjson;
  result["global_statistics"] = global_statistics->to_json();
  result["timing"] = timing->to_json();

  std::cout << result.dump(4) << std::endl;

  return 0;
}
