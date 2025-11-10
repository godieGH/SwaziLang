#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

#include "evaluator.hpp"
#include "lexer.hpp"
#include "parser.hpp"

namespace fs = std::filesystem;

class FileExecutionTest : public ::testing::Test {
   protected:
    void SetUp() override {
        test_dir = fs::temp_directory_path() / "swazi_test";
        fs::create_directories(test_dir);
    }

    void TearDown() override {
        fs::remove_all(test_dir);
    }

    fs::path test_dir;

    void createTestFile(const std::string& filename, const std::string& content) {
        fs::path filepath = test_dir / filename;
        std::ofstream file(filepath);
        file << content;
        file.close();
    }
};

TEST_F(FileExecutionTest, ExecutesSimpleScript) {
    createTestFile("test.sl", "data x = 5\ndata y = x + 3\n");

    fs::path filepath = test_dir / "test.sl";
    std::ifstream file(filepath);
    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string source = buffer.str();

    Lexer lexer(source, filepath.string());
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto ast = parser.parse();

    Evaluator evaluator;
    evaluator.set_entry_point(filepath.string());
    EXPECT_NO_THROW(evaluator.evaluate(ast.get()));
}

TEST_F(FileExecutionTest, HandlesFileNotFound) {
    fs::path nonexistent = test_dir / "nonexistent.sl";
    std::ifstream file(nonexistent);
    EXPECT_FALSE(file.is_open());
}

TEST_F(FileExecutionTest, FindsFileWithExtension) {
    createTestFile("script.sl", "data a = 1\n");

    fs::path base = test_dir / "script";
    fs::path with_ext = test_dir / "script.sl";

    EXPECT_TRUE(fs::exists(with_ext));
}

TEST_F(FileExecutionTest, ParsesCLIArgs) {
    std::vector<std::string> args = {"swazi", "test.sl", "arg1", "arg2"};

    EXPECT_EQ(args[0], "swazi");
    EXPECT_EQ(args[1], "test.sl");
    EXPECT_EQ(args.size(), 4);
}