#include "node_config_file.h"
#include "debug_utils-inl.h"
#include "simdjson.h"

#include <string>

namespace node {

std::optional<std::string_view> ConfigReader::GetDataFromArgs(
    const std::vector<std::string>& args) {
  constexpr std::string_view flag_path = "--experimental-config-file";
  constexpr std::string_view default_file =
      "--experimental-default-config-file";

  bool has_default_config_file = false;

  for (auto it = args.begin(); it != args.end(); ++it) {
    if (*it == flag_path) {
      // Case: "--experimental-config-file foo"
      if (auto next = std::next(it); next != args.end()) {
        return *next;
      }
    } else if (it->starts_with(flag_path)) {
      // Case: "--experimental-config-file=foo"
      if (it->size() > flag_path.size() && (*it)[flag_path.size()] == '=') {
        return std::string_view(*it).substr(flag_path.size() + 1);
      }
    } else if (*it == default_file || it->starts_with(default_file)) {
      has_default_config_file = true;
    }
  }

  if (has_default_config_file) {
    return "node.config.json";
  }

  return std::nullopt;
}

ParseResult ConfigReader::ParseNodeOptions(
    simdjson::ondemand::object* node_options_object) {
  auto env_options_map = options_parser::MapEnvOptionsFlagInputType();
  simdjson::ondemand::value ondemand_value;
  std::string_view key;

  for (auto field : *node_options_object) {
    if (field.unescaped_key().get(key) || field.value().get(ondemand_value)) {
      return ParseResult::InvalidContent;
    }

    // The key needs to match the CLI option
    std::string prefix = "--";
    auto it = env_options_map.find(prefix.append(key));
    if (it != env_options_map.end()) {
      switch (it->second) {
        case options_parser::OptionType::kBoolean: {
          bool result;
          if (ondemand_value.get_bool().get(result)) {
            FPrintF(stderr, "Invalid value for %s\n", it->first.c_str());
            return ParseResult::InvalidContent;
          }

          if (result) {
            // If the value is true, we need to set the flag
            node_options_.push_back(it->first);
          }

          break;
        }
        // String array can allow both string and array types
        case options_parser::OptionType::kStringList: {
          simdjson::ondemand::json_type field_type;
          if (ondemand_value.type().get(field_type)) {
            return ParseResult::InvalidContent;
          }
          switch (field_type) {
            case simdjson::ondemand::json_type::array: {
              std::vector<std::string> result;
              simdjson::ondemand::array raw_imports;
              if (ondemand_value.get_array().get(raw_imports)) {
                FPrintF(stderr, "Invalid value for %s\n", it->first.c_str());
                return ParseResult::InvalidContent;
              }
              for (auto raw_import : raw_imports) {
                std::string_view import;
                if (raw_import.get_string(import)) {
                  FPrintF(stderr, "Invalid value for %s\n", it->first.c_str());
                  return ParseResult::InvalidContent;
                }
                node_options_.push_back(it->first + "=" + std::string(import));
              }
              break;
            }
            case simdjson::ondemand::json_type::string: {
              std::string result;
              if (ondemand_value.get_string(result)) {
                FPrintF(stderr, "Invalid value for %s\n", it->first.c_str());
                return ParseResult::InvalidContent;
              }
              node_options_.push_back(it->first + "=" + result);
              break;
            }
            default:
              FPrintF(stderr, "Invalid value for %s\n", it->first.c_str());
              return ParseResult::InvalidContent;
          }
          break;
        }
        case options_parser::OptionType::kString: {
          std::string result;
          if (ondemand_value.get_string(result)) {
            FPrintF(stderr, "Invalid value for %s\n", it->first.c_str());
            return ParseResult::InvalidContent;
          }
          node_options_.push_back(it->first + "=" + result);
          break;
        }
        case options_parser::OptionType::kInteger: {
          int64_t result;
          if (ondemand_value.get_int64().get(result)) {
            FPrintF(stderr, "Invalid value for %s\n", it->first.c_str());
            return ParseResult::InvalidContent;
          }
          node_options_.push_back(it->first + "=" + std::to_string(result));
          break;
        }
        case options_parser::OptionType::kHostPort:
        case options_parser::OptionType::kUInteger: {
          uint64_t result;
          if (ondemand_value.get_uint64().get(result)) {
            FPrintF(stderr, "Invalid value for %s\n", it->first.c_str());
            return ParseResult::InvalidContent;
          }
          node_options_.push_back(it->first + "=" + std::to_string(result));
          break;
        }
        case options_parser::OptionType::kNoOp: {
          FPrintF(stderr,
                  "No-op flag %s is currently not supported\n",
                  it->first.c_str());
          return ParseResult::InvalidContent;
          break;
        }
        case options_parser::OptionType::kV8Option: {
          FPrintF(stderr,
                  "V8 flag %s is currently not supported\n",
                  it->first.c_str());
          return ParseResult::InvalidContent;
        }
        default:
          UNREACHABLE();
      }
    } else {
      FPrintF(stderr, "Unknown or not allowed option %s\n", key.data());
      return ParseResult::InvalidContent;
    }
  }
  return ParseResult::Valid;
}

ParseResult ConfigReader::ParseConfig(const std::string_view& config_path) {
  std::string file_content;
  // Read the configuration file
  int r = ReadFileSync(&file_content, config_path.data());
  if (r != 0) {
    const char* err = uv_strerror(r);
    FPrintF(
        stderr, "Cannot read configuration from %s: %s\n", config_path, err);
    return ParseResult::FileError;
  }

  // Parse the configuration file
  simdjson::ondemand::parser json_parser;
  simdjson::ondemand::document document;
  if (json_parser.iterate(file_content).get(document)) {
    FPrintF(stderr, "Can't parse %s\n", config_path.data());
    return ParseResult::InvalidContent;
  }

  simdjson::ondemand::object main_object;
  // If document is not an object, throw an error.
  if (auto root_error = document.get_object().get(main_object)) {
    if (root_error == simdjson::error_code::INCORRECT_TYPE) {
      FPrintF(stderr,
              "Root value unexpected not an object for %s\n\n",
              config_path.data());
    } else {
      FPrintF(stderr, "Can't parse %s\n", config_path.data());
    }
    return ParseResult::InvalidContent;
  }

  simdjson::ondemand::object node_options_object;
  //  If "nodeOptions" is an object, parse it
  if (auto node_options_error =
          main_object["nodeOptions"].get_object().get(node_options_object)) {
    if (node_options_error != simdjson::error_code::NO_SUCH_FIELD) {
      FPrintF(stderr,
              "\"nodeOptions\" value unexpected for %s\n\n",
              config_path.data());
      return ParseResult::InvalidContent;
    }
  } else {
    return ParseNodeOptions(&node_options_object);
  }

  return ParseResult::Valid;
}

std::string ConfigReader::AssignNodeOptions() {
  if (node_options_.empty()) {
    return "";
  } else {
    DCHECK_GT(node_options_.size(), 0);
    std::string acc;
    acc.reserve(node_options_.size() * 2);
    for (size_t i = 0; i < node_options_.size(); ++i) {
      // The space is necessary at the beginning of the string
      acc += " " + node_options_[i];
    }
    return acc;
  }
}

size_t ConfigReader::GetFlagsSize() {
  return node_options_.size();
}
}  // namespace node
