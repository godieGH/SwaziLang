#include "cli_commands.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <nlohmann/json.hpp>
#include <regex>
#include <sstream>

#include "evaluator.hpp"
#include "lexer.hpp"
#include "parser.hpp"

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace swazi {
namespace cli {

// Parse swazi.json properly with nlohmann/json
std::optional<ProjectConfig> parse_swazi_json(const std::string& filepath) {
    std::ifstream file(filepath);
    if (!file.is_open()) {
        return std::nullopt;
    }

    try {
        json j = json::parse(file);

        ProjectConfig config;
        config.is_valid = true;

        // Extract basic fields with defaults
        config.name = j.value("name", "");
        config.version = j.value("version", "");
        config.entry = j.value("entry", "");
        config.type = j.value("type", "");
        config.description = j.value("description", "");
        config.author = j.value("author", "");
        config.license = j.value("license", "");

        // Extract engines.swazi
        if (j.contains("engines") && j["engines"].is_object()) {
            config.engines.swazi = j["engines"].value("swazi", "");
        }

        // Extract keywords
        if (j.contains("keywords") && j["keywords"].is_array()) {
            for (const auto& keyword : j["keywords"]) {
                if (keyword.is_string()) {
                    config.keywords.push_back(keyword.get<std::string>());
                }
            }
        }

        return config;

    } catch (const json::parse_error& e) {
        std::cerr << "JSON parse error in " << filepath << ": " << e.what() << std::endl;
        return std::nullopt;
    } catch (const json::exception& e) {
        std::cerr << "JSON error in " << filepath << ": " << e.what() << std::endl;
        return std::nullopt;
    }
}

std::string get_project_root(const std::string& start_dir) {
    fs::path current = fs::absolute(start_dir);

    while (true) {
        fs::path config_path = current / "swazi.json";
        if (fs::exists(config_path)) {
            return current.string();
        }

        if (!current.has_parent_path() || current == current.parent_path()) {
            break;
        }
        current = current.parent_path();
    }

    return "";
}

std::optional<ProjectConfig> find_and_parse_swazi_json(const std::string& start_dir) {
    std::string root = get_project_root(start_dir);
    if (root.empty()) {
        return std::nullopt;
    }

    return parse_swazi_json(root + "/swazi.json");
}

int count_swazi_files(const std::string& dir) {
    int count = 0;
    try {
        for (const auto& entry : fs::recursive_directory_iterator(dir)) {
            if (entry.is_regular_file()) {
                std::string ext = entry.path().extension().string();
                if (ext == ".sl" || ext == ".swz") {
                    count++;
                }
            }
        }
    } catch (const std::exception& e) {
        // Silently ignore errors (like permission denied)
    }
    return count;
}

std::vector<std::string> list_swazi_files(const std::string& dir) {
    std::vector<std::string> files;
    try {
        for (const auto& entry : fs::recursive_directory_iterator(dir)) {
            if (entry.is_regular_file()) {
                std::string ext = entry.path().extension().string();
                if (ext == ".sl" || ext == ".swz") {
                    files.push_back(entry.path().string());
                }
            }
        }
    } catch (const std::exception& e) {
        // Silently ignore errors
    }
    std::sort(files.begin(), files.end());
    return files;
}

CommandResult execute_command(const std::vector<std::string>& args) {
    if (args.empty()) {
        return {1, "No command specified"};
    }

    std::string command = args[0];
    std::vector<std::string> sub_args(args.begin() + 1, args.end());

    if (command == "init") {
        return cmd_init(sub_args);
    } else if (command == "project") {
        return cmd_project(sub_args);
    } else if (command == "vendor") {
        return cmd_vendor(sub_args);
    } else if (command == "cache") {
        return cmd_cache(sub_args);
    } else if (command == "start") {
        return cmd_start(sub_args);
    } else if (command == "run") {
        return cmd_run(sub_args);
    } else if (command == "publish") {
        return cmd_publish(sub_args);
    } else if (command == "install") {
        return cmd_install(sub_args);
    } else {
        return {1, "Unknown command: " + command};
    }
}

std::string bump_version(const std::string& version, BumpType bump_type) {
    // Parse version string (expects format: major.minor.patch)
    std::regex version_regex(R"((\d+)\.(\d+)\.(\d+).*)");
    std::smatch matches;

    if (!std::regex_match(version, matches, version_regex)) {
        return "";  // Invalid version format
    }

    int major = std::stoi(matches[1].str());
    int minor = std::stoi(matches[2].str());
    int patch = std::stoi(matches[3].str());

    switch (bump_type) {
        case BumpType::MAJOR:
            major++;
            minor = 0;
            patch = 0;
            break;
        case BumpType::MINOR:
            minor++;
            patch = 0;
            break;
        case BumpType::PATCH:
            patch++;
            break;
    }

    return std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch);
}

bool update_swazi_json_version(const std::string& filepath, const std::string& new_version) {
    // Read the current JSON
    std::ifstream file(filepath);
    if (!file.is_open()) {
        return false;
    }

    json j;
    try {
        j = json::parse(file);
    } catch (...) {
        file.close();
        return false;
    }
    file.close();

    // Update version
    j["version"] = new_version;

    // Write back
    std::ofstream out_file(filepath);
    if (!out_file.is_open()) {
        return false;
    }

    out_file << j.dump(2) << std::endl;
    out_file.close();

    return true;
}

bool is_git_repository(const std::string& dir) {
    fs::path git_dir = fs::path(dir) / ".git";
    return fs::exists(git_dir);
}

bool git_commit_and_tag(const std::string& dir, const std::string& version) {
    // Change to project directory
    fs::path old_path = fs::current_path();

    try {
        fs::current_path(dir);

        // Stage swazi.json
        int result = std::system("git add swazi.json");
        if (result != 0) {
            fs::current_path(old_path);
            return false;
        }

        // Commit
        std::string commit_cmd = "git commit -m \"chore: bump version to " + version + "\"";
        result = std::system(commit_cmd.c_str());
        if (result != 0) {
            fs::current_path(old_path);
            return false;
        }

        // Create tag
        std::string tag_cmd = "git tag v" + version;
        result = std::system(tag_cmd.c_str());

        fs::current_path(old_path);
        return result == 0;

    } catch (...) {
        fs::current_path(old_path);
        return false;
    }
}

CommandResult cmd_init(const std::vector<std::string>& args) {
    std::cout << "Initializing Swazi project...\n\n";

    // Check if swazi.json already exists
    if (fs::exists("swazi.json")) {
        std::cout << "swazi.json already exists in current directory.\n";
        std::cout << "Do you want to overwrite it? (y/N): ";
        std::string response;
        std::getline(std::cin, response);
        if (response != "y" && response != "Y") {
            return {0, "Aborted."};
        }
    }

    // Check for flags
    bool create_vendor = false;
    std::string type = "project";

    for (const auto& arg : args) {
        if (arg == "--vendor") {
            create_vendor = true;
        } else if (arg.rfind("--type=", 0) == 0) {
            type = arg.substr(7);
        }
    }

    // Prompt for project details
    std::string name, version, entry, engine, vendor_prompt, description;

    std::cout << "Project name: ";
    std::getline(std::cin, name);
    if (name.empty()) name = "my-swazi-project";

    std::cout << "Version (1.0.0): ";
    std::getline(std::cin, version);
    if (version.empty()) version = "1.0.0";

    std::cout << "Description: ";
    std::getline(std::cin, description);

    std::cout << "Entry point (index.sl): ";
    std::getline(std::cin, entry);
    if (entry.empty()) entry = "index.sl";

    if (!create_vendor) {
        std::cout << "Project type (project/package/library) [" << type << "]: ";
        std::string type_input;
        std::getline(std::cin, type_input);
        if (!type_input.empty()) type = type_input;
    }

    // Generate default engine version based on current SWAZI_VERSION
    std::string default_engine = ">=2.11.0 <=" + std::string(SWAZI_VERSION);
    std::cout << "Swazi engine version (" << default_engine << "): ";
    std::getline(std::cin, engine);
    if (engine.empty()) engine = default_engine;

    if (!create_vendor) {
        std::cout << "Create vendor directory? (y/N): ";
        std::getline(std::cin, vendor_prompt);
        create_vendor = (vendor_prompt == "y" || vendor_prompt == "Y");
    }

    // Create swazi.json using nlohmann/json
    json config = {
        {"name", name},
        {"version", version},
        {"type", type},
        {"entry", entry},
        {"engines", {{"swazi", engine}}}};

    if (!description.empty()) {
        config["description"] = description;
    }

    if (create_vendor) {
        config["vendor"] = json::object();
    }

    // Write to file with pretty printing
    std::ofstream config_file("swazi.json");
    if (!config_file.is_open()) {
        return {1, "Failed to create swazi.json"};
    }

    config_file << config.dump(2) << std::endl;
    config_file.close();

    std::cout << "\n✓ Created swazi.json\n";

    // Create vendor directory if requested
    if (create_vendor) {
        try {
            fs::create_directory("vendor");
            std::cout << "✓ Created vendor/ directory\n";
        } catch (const std::exception& e) {
            std::cout << "⚠ Warning: Failed to create vendor/ directory: " << e.what() << "\n";
        }
    }

    // Create entry file if it doesn't exist
    if (!fs::exists(entry)) {
        std::ofstream entry_file(entry);
        if (entry_file.is_open()) {
            entry_file << "/**\n";
            entry_file << " * name: " << name << "\n";
            entry_file << " * version: " << version << "\n";
            if (!description.empty()) {
                entry_file << " * description: " << description << "\n";
            }
            entry_file << " */\n\n";
            entry_file << "chapisha(\"Karibu to Swazi!\");\n";
            entry_file.close();
            std::cout << "✓ Created " << entry << "\n";
        }
    }

    std::cout << "\n✓ Project initialized successfully!\n";
    std::cout << "\nNext steps:\n";
    std::cout << "  1. Edit " << entry << " to start coding\n";
    std::cout << "  2. Run 'swazi start' to execute your project\n";

    return {0, ""};
}

CommandResult cmd_project(const std::vector<std::string>& args) {
    if (args.empty()) {
        return {1, "Usage: swazi project [info|version|entry|type|files|filecount]"};
    }

    std::string subcommand = args[0];
    auto config_opt = find_and_parse_swazi_json(".");

    if (subcommand == "info") {
        std::cout << "Project Information\n";
        std::cout << "===================\n\n";

        if (config_opt.has_value()) {
            auto config = config_opt.value();
            std::cout << "Name:        " << (config.name.empty() ? "(not set)" : config.name) << "\n";
            std::cout << "Version:     " << (config.version.empty() ? "(not set)" : config.version) << "\n";
            std::cout << "Type:        " << (config.type.empty() ? "(not set)" : config.type) << "\n";
            std::cout << "Entry:       " << (config.entry.empty() ? "(not set)" : config.entry) << "\n";
            std::cout << "Description: " << (config.description.empty() ? "(not set)" : config.description) << "\n";
            std::cout << "Author:      " << (config.author.empty() ? "(not set)" : config.author) << "\n";
            std::cout << "License:     " << (config.license.empty() ? "(not set)" : config.license) << "\n";
            std::cout << "Engine:      " << (config.engines.swazi.empty() ? "(not set)" : config.engines.swazi) << "\n";

            if (!config.keywords.empty()) {
                std::cout << "Keywords:    ";
                for (size_t i = 0; i < config.keywords.size(); i++) {
                    std::cout << config.keywords[i];
                    if (i < config.keywords.size() - 1) std::cout << ", ";
                }
                std::cout << "\n";
            }

            std::string root = get_project_root(".");
            int file_count = count_swazi_files(root);
            std::cout << "\nFile Count:  " << file_count << " .sl/.swz files\n";
            std::cout << "Root:        " << root << "\n";
        } else {
            std::cout << "Name:        (not set)\n";
            std::cout << "Version:     (not set)\n";
            std::cout << "Type:        (not set)\n";
            std::cout << "Entry:       (not set)\n";
            std::cout << "Description: (not set)\n";
            std::cout << "Author:      (not set)\n";
            std::cout << "License:     (not set)\n";
            std::cout << "Engine:      (not set)\n";
            std::cout << "Keywords:    (not set)\n";

            std::string current = fs::current_path().string();
            int file_count = count_swazi_files(current);
            std::cout << "\nFile Count:  " << file_count << " .sl/.swz files\n";
            std::cout << "Root:        " << current << " (no swazi.json found)\n";
        }

    } else if (subcommand == "version") {
        // Check for bump flag
        BumpType bump_type;
        bool should_bump = false;

        for (size_t i = 1; i < args.size(); i++) {
            std::string arg = args[i];
            if (arg == "--bump" || arg == "-b") {
                if (i + 1 < args.size()) {
                    std::string bump_str = args[i + 1];
                    if (bump_str == "patch") {
                        bump_type = BumpType::PATCH;
                        should_bump = true;
                    } else if (bump_str == "minor") {
                        bump_type = BumpType::MINOR;
                        should_bump = true;
                    } else if (bump_str == "major") {
                        bump_type = BumpType::MAJOR;
                        should_bump = true;
                    } else {
                        return {1, "Invalid bump type. Use: patch, minor, or major"};
                    }
                    break;
                }
            } else if (arg.rfind("--bump=", 0) == 0 || arg.rfind("-b=", 0) == 0) {
                std::string bump_str = arg.substr(arg.find('=') + 1);
                if (bump_str == "patch") {
                    bump_type = BumpType::PATCH;
                    should_bump = true;
                } else if (bump_str == "minor") {
                    bump_type = BumpType::MINOR;
                    should_bump = true;
                } else if (bump_str == "major") {
                    bump_type = BumpType::MAJOR;
                    should_bump = true;
                } else {
                    return {1, "Invalid bump type. Use: patch, minor, or major"};
                }
                break;
            }
        }

        if (should_bump) {
            if (!config_opt.has_value()) {
                return {1, "Error: No swazi.json found"};
            }

            auto config = config_opt.value();
            std::string current_version = config.version;

            if (current_version.empty()) {
                return {1, "Error: No version found in swazi.json"};
            }

            std::string new_version = bump_version(current_version, bump_type);
            if (new_version.empty()) {
                return {1, "Error: Invalid version format in swazi.json"};
            }

            std::string root = get_project_root(".");
            std::string config_path = root + "/swazi.json";

            if (!update_swazi_json_version(config_path, new_version)) {
                return {1, "Error: Failed to update swazi.json"};
            }

            std::cout << "Version bumped: " << current_version << " → " << new_version << "\n";

            // Check if this is a git repository
            if (is_git_repository(root)) {
                std::cout << "Git repository detected. Creating commit and tag...\n";
                if (git_commit_and_tag(root, new_version)) {
                    std::cout << "✓ Created commit and tag v" << new_version << "\n";
                } else {
                    std::cout << "⚠ Warning: Failed to create git commit/tag. Please commit manually.\n";
                }
            }

            return {0, ""};
        } else {
            // Just display version
            if (config_opt.has_value()) {
                std::string ver = config_opt.value().version;
                std::cout << (ver.empty() ? "(not set)" : ver) << "\n";
            } else {
                std::cout << "(not set)\n";
            }
        }

    } else if (subcommand == "entry") {
        if (config_opt.has_value()) {
            std::string ent = config_opt.value().entry;
            std::cout << (ent.empty() ? "(not set)" : ent) << "\n";
        } else {
            std::cout << "(not set)\n";
        }

    } else if (subcommand == "type") {
        if (config_opt.has_value()) {
            std::string typ = config_opt.value().type;
            std::cout << (typ.empty() ? "(not set)" : typ) << "\n";
        } else {
            std::cout << "(not set)\n";
        }

    } else if (subcommand == "name") {
        if (config_opt.has_value()) {
            std::string nam = config_opt.value().name;
            std::cout << (nam.empty() ? "(not set)" : nam) << "\n";
        } else {
            std::cout << "(not set)\n";
        }

    } else if (subcommand == "description") {
        if (config_opt.has_value()) {
            std::string desc = config_opt.value().description;
            std::cout << (desc.empty() ? "(not set)" : desc) << "\n";
        } else {
            std::cout << "(not set)\n";
        }

    } else if (subcommand == "author") {
        if (config_opt.has_value()) {
            std::string auth = config_opt.value().author;
            std::cout << (auth.empty() ? "(not set)" : auth) << "\n";
        } else {
            std::cout << "(not set)\n";
        }

    } else if (subcommand == "license") {
        if (config_opt.has_value()) {
            std::string lic = config_opt.value().license;
            std::cout << (lic.empty() ? "(not set)" : lic) << "\n";
        } else {
            std::cout << "(not set)\n";
        }

    } else if (subcommand == "engine") {
        if (config_opt.has_value()) {
            std::string eng = config_opt.value().engines.swazi;
            std::cout << (eng.empty() ? "(not set)" : eng) << "\n";
        } else {
            std::cout << "(not set)\n";
        }

    } else if (subcommand == "keywords") {
        if (config_opt.has_value()) {
            auto keywords = config_opt.value().keywords;
            if (keywords.empty()) {
                std::cout << "(not set)\n";
            } else {
                for (size_t i = 0; i < keywords.size(); i++) {
                    std::cout << keywords[i];
                    if (i < keywords.size() - 1) std::cout << ", ";
                }
                std::cout << "\n";
            }
        } else {
            std::cout << "(not set)\n";
        }

    } else if (subcommand == "filecount") {
        std::string root = get_project_root(".");
        if (root.empty()) {
            root = fs::current_path().string();
        }
        int file_count = count_swazi_files(root);
        std::cout << file_count << "\n";

    } else if (subcommand == "files") {
        std::string root = get_project_root(".");
        if (root.empty()) {
            root = fs::current_path().string();
        }
        auto files = list_swazi_files(root);

        if (files.empty()) {
            std::cout << "No .sl or .swz files found.\n";
        } else {
            std::cout << "Swazi Files (" << files.size() << "):\n";
            std::cout << "===================\n";
            for (const auto& file : files) {
                // Make path relative to root
                fs::path rel = fs::relative(file, root);
                std::cout << "  " << rel.string() << "\n";
            }
        }

    } else {
        return {1, "Unknown subcommand: " + subcommand + "\n" + "Available: info, version, entry, type, name, description, author, license, engine, keywords, files, filecount"};
    }

    return {0, ""};
}

CommandResult cmd_vendor(const std::vector<std::string>& args) {
    if (args.empty()) {
        std::cout << "Usage: swazi vendor [init|list|clear|add|remove]\n";
        return {1, ""};
    }

    std::string subcommand = args[0];

    if (subcommand == "init") {
        std::cout << "vendor init (stub)\n";
        return {0, "Not yet implemented"};
    } else if (subcommand == "list") {
        std::cout << "vendor list (stub)\n";
        return {0, "Not yet implemented"};
    } else if (subcommand == "clear") {
        std::cout << "vendor clear (stub)\n";
        return {0, "Not yet implemented"};
    } else if (subcommand == "add") {
        std::cout << "vendor add (stub)\n";
        return {0, "Not yet implemented"};
    } else if (subcommand == "remove") {
        std::cout << "vendor remove (stub)\n";
        return {0, "Not yet implemented"};
    } else {
        return {1, "Unknown vendor subcommand: " + subcommand};
    }
}

CommandResult cmd_cache(const std::vector<std::string>& args) {
    if (args.empty()) {
        // Show cache location
        std::string home = std::getenv("HOME") ? std::getenv("HOME") : "";
        if (home.empty()) {
            home = std::getenv("USERPROFILE") ? std::getenv("USERPROFILE") : "";
        }

        if (!home.empty()) {
            fs::path cache_path = fs::path(home) / ".swazi" / "cache";
            std::cout << "Cache location: " << cache_path.string() << "\n";

            if (fs::exists(cache_path)) {
                std::cout << "Cache exists: Yes\n";
                // Could add size calculation here
            } else {
                std::cout << "Cache exists: No\n";
            }
        } else {
            std::cout << "Could not determine cache location\n";
        }

        return {0, ""};
    }

    std::string subcommand = args[0];

    if (subcommand == "list") {
        std::cout << "cache list (stub)\n";
        return {0, "Not yet implemented"};
    } else if (subcommand == "clear") {
        std::cout << "cache clear (stub)\n";
        return {0, "Not yet implemented"};
    } else if (subcommand == "remove") {
        std::cout << "cache remove (stub)\n";
        return {0, "Not yet implemented"};
    } else if (subcommand == "add") {
        std::cout << "cache add (stub)\n";
        return {0, "Not yet implemented"};
    } else {
        return {1, "Unknown cache subcommand: " + subcommand};
    }
}

CommandResult cmd_start(const std::vector<std::string>& args) {
    // Find swazi.json
    auto config_opt = find_and_parse_swazi_json(".");

    if (!config_opt.has_value()) {
        std::cerr << "Error: No swazi.json found in current directory or parent directories.\n";
        std::cerr << "Run 'swazi init' to create a new project.\n";
        return {1, ""};
    }

    auto config = config_opt.value();

    if (config.entry.empty()) {
        std::cerr << "Error: No entry point specified in swazi.json.\n";
        return {1, ""};
    }

    // Get project root
    std::string root = get_project_root(".");
    if (root.empty()) {
        std::cerr << "Error: Could not determine project root.\n";
        return {1, ""};
    }

    // Build full path to entry file
    fs::path entry_path = fs::path(root) / config.entry;

    // Check if entry file exists
    if (!fs::exists(entry_path)) {
        std::cerr << "Error: Entry point '" << config.entry << "' not found at: " << entry_path.string() << "\n";
        return {1, ""};
    }

    // Read the entry file
    std::ifstream file(entry_path);
    if (!file.is_open()) {
        std::cerr << "Error: Could not open entry file: " << entry_path.string() << "\n";
        return {1, ""};
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string source_code = buffer.str();
    file.close();

    // Ensure source ends with newline
    if (source_code.empty() || source_code.back() != '\n') {
        source_code.push_back('\n');
    }

    std::cout << "Running project: " << config.name << " (v" << config.version << ")\n";
    std::cout << "Entry point: " << config.entry << "\n";
    std::cout << "-----------------------------------\n\n";

    try {
        // You'll need to include these headers at the top if not already included
        // #include "lexer.hpp"
        // #include "parser.hpp"
        // #include "evaluator.hpp"
        // These should already be available in your project

        SourceManager src_mgr(entry_path.string(), source_code);
        Lexer lexer(source_code, entry_path.string(), &src_mgr);
        std::vector<Token> tokens = lexer.tokenize();

        Parser parser(tokens);
        std::unique_ptr<ProgramNode> ast = parser.parse();

        Evaluator evaluator;

        // Build CLI args for the script (project name as argv[0], then any additional args)
        std::vector<std::string> cli_args;
        cli_args.push_back(config.name);

        // Add any additional arguments passed to 'swazi start'
        for (const auto& arg : args) {
            cli_args.push_back(arg);
        }

        evaluator.set_cli_args(cli_args);
        evaluator.set_entry_point(entry_path.string());
        evaluator.evaluate(ast.get());

        return {0, ""};

    } catch (const std::exception& e) {
        std::cerr << "\n-----------------------------------\n";
        std::cerr << "Runtime error: " << e.what() << "\n";
        return {1, ""};
    } catch (...) {
        std::cerr << "\n-----------------------------------\n";
        std::cerr << "Unknown fatal error occurred\n";
        return {1, ""};
    }
}

CommandResult cmd_run(const std::vector<std::string>& args) {
    std::cout << "run <script-name> command (stub)\n";
    std::cout << "This will run script from swazi.json\n";
    return {0, "Not yet implemented"};
}

CommandResult cmd_publish(const std::vector<std::string>& args) {
    std::cout << "publish command (stub)\n";
    std::cout << "This will package and publish your project to the Swazi registry.\n";
    return {0, "Not yet implemented"};
}

CommandResult cmd_install(const std::vector<std::string>& args) {
    std::cout << "install command (stub)\n";
    std::cout << "This will install dependencies from swazi.json.\n";
    return {0, "Not yet implemented"};
}

}  // namespace cli
}  // namespace swazi