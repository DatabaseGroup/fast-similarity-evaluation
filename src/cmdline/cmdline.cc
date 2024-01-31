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
  int64_t block_size{};
  std::string label;
};

bool process_program_options(int argc, char** argv, Config& config) {
  namespace po = boost::program_options;

  po::options_description optdesc{"DESCRIPTION"};
  optdesc.add_options()("input-file,f", po::value(&config.input_file)->required(), "Specify input file")(
    "datatype,d", po::value(&config.datatype)->required(), "Specify datatype (set, string, tree)")(
    "similarity,s", po::value(&config.similarity)->required(), "Specify similarity measure")(
    "threshold,t", po::value(&config.threshold)->required(), "Threshold")(
    "block-size,b", po::value(&config.block_size)->default_value(10000), "Block size")(
    "label,l", po::value(&config.label), "label for the run (printed in json)");

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
  json["blocksize"] = config.block_size;
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
    // todo
  }

  return {data_id, std::move(dataset)};
}

int main(int argc, char** argv) {
  Config config;

  if (!process_program_options(argc, argv, config)) {
    exit(-1);
  }

  auto [similarity_id, similarity] = resolve_similarity(config.similarity, config.threshold);
  auto [data_id, dataset] = resolve_data(config.datatype, config.input_file);

  ontology::StandardReductionGraph graph;
  auto plan_result = graph.enumerate_plans(data_id, similarity_id);
  auto& plans = plan_result.first;

  statistics::JoinStatistics statistics;
  timing::JoinTiming timing;
  timing.join_time.start();
  join::interleave_plans(dataset, similarity, plans, config.block_size, statistics);
  timing.join_time.stop();

  nlohmann::json result;
  result["meta"] = get_metadata(config);
  result["statistics"] = statistics.to_json();
  result["timing"] = timing.to_json();

  std::cout << result.dump(4) << std::endl;

  return 0;
}
