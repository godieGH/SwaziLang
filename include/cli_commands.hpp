#ifndef CLI_COMMANDS_HPP
#define CLI_COMMANDS_HPP

#include <optional>
#include <string>
#include <vector>

namespace swazi {
namespace cli {

// Structure to hold parsed swazi.json data
struct ProjectConfig {
    std::string name;
    std::string version;
    std::string entry;
    std::string type;  // "project" | "package" | "library"
    std::string description;
    std::string author;
    std::string license;

    struct Engine {
        std::string swazi;
    };
    Engine engines;

    std::vector<std::string> keywords;

    // We'll add more fields later (exports, vendor, dependencies, etc.)

    bool is_valid = false;
};

// Command result structure
struct CommandResult {
    int exit_code;
    std::string message;
};

// Version bump types
enum class BumpType {
    PATCH,
    MINOR,
    MAJOR
};

// Main command dispatcher
CommandResult execute_command(const std::vector<std::string>& args);

// Individual command implementations
CommandResult cmd_init(const std::vector<std::string>& args);
CommandResult cmd_project(const std::vector<std::string>& args);
CommandResult cmd_vendor(const std::vector<std::string>& args);
CommandResult cmd_cache(const std::vector<std::string>& args);
CommandResult cmd_start(const std::vector<std::string>& args);
CommandResult cmd_run(const std::vector<std::string>& args);
CommandResult cmd_publish(const std::vector<std::string>& args);
CommandResult cmd_install(const std::vector<std::string>& args);
CommandResult cmd_fmt(const std::vector<std::string>& args);

// Helper functions
std::optional<ProjectConfig> find_and_parse_swazi_json(const std::string& start_dir = ".");
std::optional<ProjectConfig> parse_swazi_json(const std::string& filepath);
std::string get_project_root(const std::string& start_dir = ".");
int count_swazi_files(const std::string& dir);
std::vector<std::string> list_swazi_files(const std::string& dir);

// Version management
std::string bump_version(const std::string& version, BumpType bump_type);
bool update_swazi_json_version(const std::string& filepath, const std::string& new_version);
bool is_git_repository(const std::string& dir);
bool git_commit_and_tag(const std::string& dir, const std::string& version);

}  // namespace cli
}  // namespace swazi

#endif  // CLI_COMMANDS_HPP