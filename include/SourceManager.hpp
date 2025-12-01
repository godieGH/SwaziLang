#pragma once
#include <map>
#include <sstream>
#include <string>

class SourceManager {
   public:
    std::string filename;
    std::string source;
    std::map<int, std::string> lines;

    SourceManager(const std::string& fname, const std::string& src)
        : filename(fname), source(src) {
        build_line_map();
    }

    std::string get_line(int line_num) const {
        auto it = lines.find(line_num);
        return it != lines.end() ? it->second : "";
    }

    std::string format_error_context(int line, int col) const {
        std::stringstream ss;
        ss << " * " << line << " | ";
        std::string line_text = get_line(line);
        ss << line_text << "\n";
        ss << std::string(col + ss.str().size() - line_text.size(), ' ') << "^";
        return ss.str();
    }

   private:
    void build_line_map() {
        int line_num = 1;
        std::string current_line;

        for (char c : source) {
            if (c == '\n') {
                lines[line_num] = current_line;
                current_line.clear();
                line_num++;
            } else {
                current_line += c;
            }
        }
        if (!current_line.empty()) {
            lines[line_num] = current_line;
        }
    }
};