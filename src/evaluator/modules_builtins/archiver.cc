#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

#include "AsyncBridge.hpp"
#include "Scheduler.hpp"
#include "SwaziError.hpp"
#include "builtins.hpp"
#include "evaluator.hpp"

namespace fs = std::filesystem;

// Check for zlib availability
#ifdef __has_include
#if __has_include(<zlib.h>)
#include <zlib.h>
#define HAVE_ZLIB 1
#endif
#endif

// Utility to convert Value to string
static std::string value_to_string_simple(const Value& v) {
    if (std::holds_alternative<std::string>(v)) return std::get<std::string>(v);
    if (std::holds_alternative<double>(v)) {
        std::ostringstream ss;
        ss << std::get<double>(v);
        return ss.str();
    }
    return "";
}

// Helper to read entire file into buffer
static std::vector<uint8_t> read_file_bytes(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) throw std::runtime_error("Failed to open file: " + path);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)),
        std::istreambuf_iterator<char>());
}

// Helper to write buffer to file
static void write_file_bytes(const std::string& path, const std::vector<uint8_t>& data) {
    std::ofstream out(path, std::ios::binary);
    if (!out.is_open()) throw std::runtime_error("Failed to write file: " + path);
    out.write(reinterpret_cast<const char*>(data.data()), data.size());
}

#ifdef HAVE_ZLIB
// Gzip compress using deflate
static std::vector<uint8_t> gzip_compress(const std::vector<uint8_t>& input, int level) {
    z_stream stream{};
    stream.zalloc = Z_NULL;
    stream.zfree = Z_NULL;
    stream.opaque = Z_NULL;

    // Use deflateInit2 with gzip wrapper (windowBits + 16)
    if (deflateInit2(&stream, level, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        throw std::runtime_error("deflateInit2 failed");
    }

    stream.avail_in = input.size();
    stream.next_in = const_cast<Bytef*>(input.data());

    std::vector<uint8_t> output;
    output.reserve(input.size() / 2);  // estimate

    std::vector<uint8_t> temp(32768);
    do {
        stream.avail_out = temp.size();
        stream.next_out = temp.data();

        deflate(&stream, Z_FINISH);

        size_t have = temp.size() - stream.avail_out;
        output.insert(output.end(), temp.begin(), temp.begin() + have);
    } while (stream.avail_out == 0);

    deflateEnd(&stream);
    return output;
}

// Gzip decompress
static std::vector<uint8_t> gzip_decompress(const std::vector<uint8_t>& input) {
    z_stream stream{};
    stream.zalloc = Z_NULL;
    stream.zfree = Z_NULL;
    stream.opaque = Z_NULL;

    // Use inflateInit2 with gzip wrapper
    if (inflateInit2(&stream, 15 + 16) != Z_OK) {
        throw std::runtime_error("inflateInit2 failed");
    }

    stream.avail_in = input.size();
    stream.next_in = const_cast<Bytef*>(input.data());

    std::vector<uint8_t> output;
    std::vector<uint8_t> temp(32768);

    int ret;
    do {
        stream.avail_out = temp.size();
        stream.next_out = temp.data();

        ret = inflate(&stream, Z_NO_FLUSH);
        if (ret != Z_OK && ret != Z_STREAM_END) {
            inflateEnd(&stream);
            throw std::runtime_error("inflate failed");
        }

        size_t have = temp.size() - stream.avail_out;
        output.insert(output.end(), temp.begin(), temp.begin() + have);
    } while (ret != Z_STREAM_END);

    inflateEnd(&stream);
    return output;
}

// Raw deflate compress
static std::vector<uint8_t> deflate_compress(const std::vector<uint8_t>& input, int level) {
    z_stream stream{};
    stream.zalloc = Z_NULL;
    stream.zfree = Z_NULL;
    stream.opaque = Z_NULL;

    if (deflateInit(&stream, level) != Z_OK) {
        throw std::runtime_error("deflateInit failed");
    }

    stream.avail_in = input.size();
    stream.next_in = const_cast<Bytef*>(input.data());

    std::vector<uint8_t> output;
    std::vector<uint8_t> temp(32768);

    do {
        stream.avail_out = temp.size();
        stream.next_out = temp.data();
        deflate(&stream, Z_FINISH);
        size_t have = temp.size() - stream.avail_out;
        output.insert(output.end(), temp.begin(), temp.begin() + have);
    } while (stream.avail_out == 0);

    deflateEnd(&stream);
    return output;
}

// Raw deflate decompress
static std::vector<uint8_t> deflate_decompress(const std::vector<uint8_t>& input) {
    z_stream stream{};
    stream.zalloc = Z_NULL;
    stream.zfree = Z_NULL;
    stream.opaque = Z_NULL;

    if (inflateInit(&stream) != Z_OK) {
        throw std::runtime_error("inflateInit failed");
    }

    stream.avail_in = input.size();
    stream.next_in = const_cast<Bytef*>(input.data());

    std::vector<uint8_t> output;
    std::vector<uint8_t> temp(32768);

    int ret;
    do {
        stream.avail_out = temp.size();
        stream.next_out = temp.data();
        ret = inflate(&stream, Z_NO_FLUSH);
        if (ret != Z_OK && ret != Z_STREAM_END) {
            inflateEnd(&stream);
            throw std::runtime_error("inflate failed");
        }
        size_t have = temp.size() - stream.avail_out;
        output.insert(output.end(), temp.begin(), temp.begin() + have);
    } while (ret != Z_STREAM_END);

    inflateEnd(&stream);
    return output;
}
#endif

// Simple TAR implementation (no compression, just archiving)
namespace tar {
struct Header {
    char name[100];
    char mode[8];
    char uid[8];
    char gid[8];
    char size[12];
    char mtime[12];
    char checksum[8];
    char typeflag;
    char linkname[100];
    char magic[6];
    char version[2];
    char uname[32];
    char gname[32];
    char devmajor[8];
    char devminor[8];
    char prefix[155];
    char pad[12];
};

struct FileMetadata {
    std::string name;
    uint32_t mode = 0644;
    uint32_t uid = 0;
    uint32_t gid = 0;
    time_t mtime = 0;
};

static_assert(sizeof(Header) == 512, "TAR header must be 512 bytes");

static void write_octal(char* dest, size_t len, uint64_t value) {
    std::memset(dest, '0', len);
    dest[len - 1] = '\0';
    for (int i = len - 2; i >= 0 && value > 0; --i) {
        dest[i] = '0' + (value & 7);
        value >>= 3;
    }
}

static uint64_t read_octal(const char* src, size_t len) {
    uint64_t result = 0;
    for (size_t i = 0; i < len && src[i] >= '0' && src[i] <= '7'; ++i) {
        result = (result << 3) | (src[i] - '0');
    }
    return result;
}

static void calculate_checksum(Header& hdr) {
    std::memset(hdr.checksum, ' ', 8);
    uint64_t sum = 0;
    const uint8_t* ptr = reinterpret_cast<const uint8_t*>(&hdr);
    for (size_t i = 0; i < sizeof(Header); ++i) {
        sum += ptr[i];
    }
    write_octal(hdr.checksum, 7, sum);
    hdr.checksum[7] = ' ';
}

static std::vector<uint8_t> create(const std::vector<std::pair<std::string, std::vector<uint8_t>>>& files) {
    std::vector<uint8_t> archive;

    for (const auto& [name, data] : files) {
        Header hdr{};
        std::memset(&hdr, 0, sizeof(hdr));

        std::strncpy(hdr.name, name.c_str(), 99);
        write_octal(hdr.mode, 7, 0644);
        write_octal(hdr.size, 11, data.size());
        write_octal(hdr.mtime, 11, std::time(nullptr));
        hdr.typeflag = '0';  // regular file
        std::strcpy(hdr.magic, "ustar");
        std::strcpy(hdr.version, "00");

        calculate_checksum(hdr);

        // Write header
        const uint8_t* hdr_bytes = reinterpret_cast<const uint8_t*>(&hdr);
        archive.insert(archive.end(), hdr_bytes, hdr_bytes + sizeof(hdr));

        // Write data
        archive.insert(archive.end(), data.begin(), data.end());

        // Pad to 512-byte boundary
        size_t padding = (512 - (data.size() % 512)) % 512;
        archive.insert(archive.end(), padding, 0);
    }

    // End-of-archive marker (two zero blocks)
    archive.insert(archive.end(), 1024, 0);

    return archive;
}

static std::vector<std::pair<std::string, std::vector<uint8_t>>> extract(const std::vector<uint8_t>& archive) {
    std::vector<std::pair<std::string, std::vector<uint8_t>>> files;
    size_t pos = 0;

    while (pos + 512 <= archive.size()) {
        const Header* hdr = reinterpret_cast<const Header*>(&archive[pos]);

        // Check for end-of-archive
        if (hdr->name[0] == '\0') break;

        std::string name(hdr->name);
        uint64_t size = read_octal(hdr->size, 11);

        pos += 512;

        if (pos + size > archive.size()) break;

        std::vector<uint8_t> data(archive.begin() + pos, archive.begin() + pos + size);
        files.emplace_back(name, std::move(data));

        // Skip to next 512-byte boundary
        pos += size;
        size_t padding = (512 - (size % 512)) % 512;
        pos += padding;
    }

    return files;
}

static void write_header(std::ostream& out, const FileMetadata& meta, size_t size) {
    Header hdr{};
    std::memset(&hdr, 0, sizeof(hdr));

    std::strncpy(hdr.name, meta.name.c_str(), 99);
    write_octal(hdr.mode, 7, meta.mode);
    write_octal(hdr.uid, 7, meta.uid);
    write_octal(hdr.gid, 7, meta.gid);
    write_octal(hdr.size, 11, size);
    write_octal(hdr.mtime, 11, meta.mtime > 0 ? meta.mtime : std::time(nullptr));
    hdr.typeflag = '0';
    std::strcpy(hdr.magic, "ustar");
    std::strcpy(hdr.version, "00");

    calculate_checksum(hdr);

    out.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
}

static void write_padding(std::ostream& out, size_t size) {
    size_t padding = (512 - (size % 512)) % 512;
    std::vector<uint8_t> zeros(padding, 0);
    if (padding > 0) {
        out.write(reinterpret_cast<const char*>(zeros.data()), padding);
    }
}
}  // namespace tar

std::shared_ptr<ObjectValue> make_archiver_exports(EnvPtr env) {
    auto obj = std::make_shared<ObjectValue>();
    Token tok{};
    tok.loc = TokenLocation("<archiver>", 0, 0, 0);

#ifdef HAVE_ZLIB
    // archiver.gzip(input_path, output_path, level=6) -> bool
    {
        auto fn = [](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
            if (args.size() < 2) {
                throw SwaziError("TypeError", "gzip requires input and output paths", token.loc);
            }

            std::string input = value_to_string_simple(args[0]);
            std::string output = value_to_string_simple(args[1]);
            int level = 6;
            if (args.size() >= 3 && std::holds_alternative<double>(args[2])) {
                level = static_cast<int>(std::get<double>(args[2]));
                level = std::clamp(level, 1, 9);
            }

            auto data = read_file_bytes(input);
            auto compressed = gzip_compress(data, level);
            write_file_bytes(output, compressed);

            return Value{true};
        };
        auto fn_val = std::make_shared<FunctionValue>("archiver.gzip", fn, env, tok);
        obj->properties["gzip"] = {Value{fn_val}, false, false, false, tok};
    }

    // archiver.gunzip(input_path, output_path) -> bool
    {
        auto fn = [](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
            if (args.size() < 2) {
                throw SwaziError("TypeError", "gunzip requires input and output paths", token.loc);
            }

            std::string input = value_to_string_simple(args[0]);
            std::string output = value_to_string_simple(args[1]);

            auto compressed = read_file_bytes(input);
            auto decompressed = gzip_decompress(compressed);
            write_file_bytes(output, decompressed);

            return Value{true};
        };
        auto fn_val = std::make_shared<FunctionValue>("archiver.gunzip", fn, env, tok);
        obj->properties["gunzip"] = {Value{fn_val}, false, false, false, tok};
    }

    // archiver.createGzipCompressor(output_path, level=6) -> Compressor object
    {
        auto fn = [env](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
            if (args.empty()) {
                throw SwaziError("TypeError", "createGzipCompressor requires output path", token.loc);
            }

            std::string output_path = value_to_string_simple(args[0]);
            int level = 6;
            if (args.size() >= 2 && std::holds_alternative<double>(args[1])) {
                level = static_cast<int>(std::get<double>(args[1]));
                level = std::clamp(level, 1, 9);
            }

            // Create state holder
            struct GzipCompressorState {
                z_stream stream;
                std::ofstream output;
                std::vector<uint8_t> temp_buffer;
                bool initialized = false;
                bool finalized = false;

                // Progress tracking
                size_t bytes_read = 0;     // Total uncompressed bytes input
                size_t bytes_written = 0;  // Total compressed bytes output
                size_t chunks_processed = 0;
                int compression_level = 6;

                ~GzipCompressorState() {
                    if (initialized && !finalized) {
                        deflateEnd(&stream);
                    }
                }
            };

            auto state = std::make_shared<GzipCompressorState>();
            state->temp_buffer.resize(32768);
            state->compression_level = level;
            state->output.open(output_path, std::ios::binary);

            if (!state->output.is_open()) {
                throw SwaziError("IOError", "Failed to open output file: " + output_path, token.loc);
            }

            // Initialize zlib stream
            state->stream = z_stream{};
            state->stream.zalloc = Z_NULL;
            state->stream.zfree = Z_NULL;
            state->stream.opaque = Z_NULL;

            if (deflateInit2(&state->stream, level, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
                throw SwaziError("CryptoError", "deflateInit2 failed", token.loc);
            }
            state->initialized = true;

            auto obj = std::make_shared<ObjectValue>();

            // update(chunk) method - compresses and writes chunk
            auto update_fn = [state](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
                if (state->finalized) {
                    throw SwaziError("IOError", "Compressor already finalized", token.loc);
                }
                if (args.empty()) {
                    throw SwaziError("TypeError", "update requires data argument", token.loc);
                }

                std::vector<uint8_t> chunk;
                if (std::holds_alternative<BufferPtr>(args[0])) {
                    chunk = std::get<BufferPtr>(args[0])->data;
                } else if (std::holds_alternative<std::string>(args[0])) {
                    std::string str = std::get<std::string>(args[0]);
                    chunk.assign(str.begin(), str.end());
                } else {
                    throw SwaziError("TypeError", "data must be Buffer or string", token.loc);
                }

                state->bytes_read += chunk.size();
                state->stream.avail_in = chunk.size();
                state->stream.next_in = chunk.data();

                do {
                    state->stream.avail_out = state->temp_buffer.size();
                    state->stream.next_out = state->temp_buffer.data();

                    deflate(&state->stream, Z_NO_FLUSH);

                    size_t have = state->temp_buffer.size() - state->stream.avail_out;
                    if (have > 0) {
                        state->output.write(reinterpret_cast<const char*>(state->temp_buffer.data()), have);
                        state->bytes_written += have;
                    }
                } while (state->stream.avail_out == 0);

                state->chunks_processed++;
                return std::monostate{};
            };

            // finalize() method - flushes and closes stream
            auto finalize_fn = [state](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
                if (state->finalized) {
                    throw SwaziError("IOError", "Compressor already finalized", token.loc);
                }

                // Flush remaining data
                state->stream.avail_in = 0;
                state->stream.next_in = nullptr;

                int ret;
                do {
                    state->stream.avail_out = state->temp_buffer.size();
                    state->stream.next_out = state->temp_buffer.data();

                    ret = deflate(&state->stream, Z_FINISH);

                    size_t have = state->temp_buffer.size() - state->stream.avail_out;
                    if (have > 0) {
                        state->output.write(reinterpret_cast<const char*>(state->temp_buffer.data()), have);
                        state->bytes_written += have;
                    }
                } while (ret != Z_STREAM_END);

                deflateEnd(&state->stream);
                state->output.close();
                state->finalized = true;

                return std::monostate{};
            };

            // getBytesRead() -> number (uncompressed input)
            auto bytes_read_fn = [state](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
                return Value{static_cast<double>(state->bytes_read)};
            };

            // getBytesWritten() -> number (compressed output)
            auto bytes_written_fn = [state](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
                return Value{static_cast<double>(state->bytes_written)};
            };

            // getCompressionRatio() -> number (percentage saved)
            auto ratio_fn = [state](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
                if (state->bytes_read == 0) return Value{0.0};
                double ratio = 1.0 - (static_cast<double>(state->bytes_written) / static_cast<double>(state->bytes_read));
                return Value{ratio * 100.0};
            };

            // getChunksProcessed() -> number
            auto chunks_fn = [state](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
                return Value{static_cast<double>(state->chunks_processed)};
            };

            // isFinalized() -> bool
            auto finalized_fn = [state](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
                return Value{state->finalized};
            };

            // getCompressionLevel() -> number
            auto level_fn = [state](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
                return Value{static_cast<double>(state->compression_level)};
            };

            Token tok;
            tok.type = TokenType::IDENTIFIER;
            tok.loc = TokenLocation("<archiver>", 0, 0, 0);

            obj->properties["update"] = PropertyDescriptor{
                std::make_shared<FunctionValue>("update", update_fn, env, tok),
                false, false, false, tok};
            obj->properties["finalize"] = PropertyDescriptor{
                std::make_shared<FunctionValue>("finalize", finalize_fn, env, tok),
                false, false, false, tok};
            obj->properties["getBytesRead"] = PropertyDescriptor{
                std::make_shared<FunctionValue>("getBytesRead", bytes_read_fn, env, tok),
                false, false, false, tok};
            obj->properties["getBytesWritten"] = PropertyDescriptor{
                std::make_shared<FunctionValue>("getBytesWritten", bytes_written_fn, env, tok),
                false, false, false, tok};
            obj->properties["getCompressionRatio"] = PropertyDescriptor{
                std::make_shared<FunctionValue>("getCompressionRatio", ratio_fn, env, tok),
                false, false, false, tok};
            obj->properties["getChunksProcessed"] = PropertyDescriptor{
                std::make_shared<FunctionValue>("getChunksProcessed", chunks_fn, env, tok),
                false, false, false, tok};
            obj->properties["isFinalized"] = PropertyDescriptor{
                std::make_shared<FunctionValue>("isFinalized", finalized_fn, env, tok),
                false, false, false, tok};
            obj->properties["getCompressionLevel"] = PropertyDescriptor{
                std::make_shared<FunctionValue>("getCompressionLevel", level_fn, env, tok),
                false, false, false, tok};

            return Value{obj};
        };
        auto fn_val = std::make_shared<FunctionValue>("archiver.createGzipCompressor", fn, env, tok);
        obj->properties["createGzipCompressor"] = {Value{fn_val}, false, false, false, tok};
    }

    // archiver.createGzipDecompressor(input_path, output_path) -> Decompressor object
    {
        auto fn = [env](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
            if (args.size() < 2) {
                throw SwaziError("TypeError", "createGzipDecompressor requires input and output paths", token.loc);
            }

            std::string input_path = value_to_string_simple(args[0]);
            std::string output_path = value_to_string_simple(args[1]);

            struct GzipDecompressorState {
                z_stream stream;
                std::ifstream input;
                std::ofstream output;
                std::vector<uint8_t> in_buffer;
                std::vector<uint8_t> out_buffer;
                bool initialized = false;
                bool finalized = false;

                // Progress tracking
                size_t bytes_read = 0;
                size_t bytes_written = 0;
                size_t total_input_size = 0;
                size_t chunks_processed = 0;

                ~GzipDecompressorState() {
                    if (initialized && !finalized) {
                        inflateEnd(&stream);
                    }
                }
            };

            auto state = std::make_shared<GzipDecompressorState>();
            state->in_buffer.resize(32768);
            state->out_buffer.resize(32768);

            state->input.open(input_path, std::ios::binary);
            state->output.open(output_path, std::ios::binary);

            if (!state->input.is_open()) {
                throw SwaziError("IOError", "Failed to open input file: " + input_path, token.loc);
            }
            if (!state->output.is_open()) {
                throw SwaziError("IOError", "Failed to open output file: " + output_path, token.loc);
            }

            // Get total input size
            state->input.seekg(0, std::ios::end);
            state->total_input_size = state->input.tellg();
            state->input.seekg(0, std::ios::beg);

            // Initialize zlib stream
            state->stream = z_stream{};
            state->stream.zalloc = Z_NULL;
            state->stream.zfree = Z_NULL;
            state->stream.opaque = Z_NULL;

            if (inflateInit2(&state->stream, 15 + 16) != Z_OK) {
                throw SwaziError("ArchiverError", "inflateInit2 failed", token.loc);
            }
            state->initialized = true;

            auto obj = std::make_shared<ObjectValue>();

            // process() method - reads input, decompresses, writes output
            auto process_fn = [state](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
                if (state->finalized) {
                    return Value{false};
                }

                // Read chunk from input
                state->input.read(reinterpret_cast<char*>(state->in_buffer.data()), state->in_buffer.size());
                std::streamsize bytes_read = state->input.gcount();

                if (bytes_read == 0) {
                    // End of input
                    inflateEnd(&state->stream);
                    state->input.close();
                    state->output.close();
                    state->finalized = true;
                    return Value{false};
                }

                state->bytes_read += bytes_read;
                state->stream.avail_in = bytes_read;
                state->stream.next_in = state->in_buffer.data();

                int ret;
                do {
                    state->stream.avail_out = state->out_buffer.size();
                    state->stream.next_out = state->out_buffer.data();

                    ret = inflate(&state->stream, Z_NO_FLUSH);

                    if (ret != Z_OK && ret != Z_STREAM_END) {
                        throw SwaziError("ArchiverError", "Decompression failed", token.loc);
                    }

                    size_t have = state->out_buffer.size() - state->stream.avail_out;
                    if (have > 0) {
                        state->output.write(reinterpret_cast<const char*>(state->out_buffer.data()), have);
                        state->bytes_written += have;
                    }

                    if (ret == Z_STREAM_END) {
                        inflateEnd(&state->stream);
                        state->input.close();
                        state->output.close();
                        state->finalized = true;
                        return Value{false};
                    }
                } while (state->stream.avail_out == 0);

                state->chunks_processed++;
                return Value{true};  // More data to process
            };

            // getBytesRead() -> number
            auto bytes_read_fn = [state](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
                return Value{static_cast<double>(state->bytes_read)};
            };

            // getBytesWritten() -> number
            auto bytes_written_fn = [state](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
                return Value{static_cast<double>(state->bytes_written)};
            };

            // getTotalSize() -> number
            auto total_size_fn = [state](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
                return Value{static_cast<double>(state->total_input_size)};
            };

            // getProgress() -> number (0.0 to 1.0)
            auto progress_fn = [state](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
                if (state->total_input_size == 0) return Value{0.0};
                return Value{static_cast<double>(state->bytes_read) / static_cast<double>(state->total_input_size)};
            };

            // getChunksProcessed() -> number
            auto chunks_fn = [state](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
                return Value{static_cast<double>(state->chunks_processed)};
            };

            // isFinalized() -> bool
            auto finalized_fn = [state](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
                return Value{state->finalized};
            };

            Token tok;
            tok.type = TokenType::IDENTIFIER;
            tok.loc = TokenLocation("<archiver>", 0, 0, 0);

            obj->properties["process"] = PropertyDescriptor{
                std::make_shared<FunctionValue>("process", process_fn, env, tok),
                false, false, false, tok};
            obj->properties["getBytesRead"] = PropertyDescriptor{
                std::make_shared<FunctionValue>("getBytesRead", bytes_read_fn, env, tok),
                false, false, false, tok};
            obj->properties["getBytesWritten"] = PropertyDescriptor{
                std::make_shared<FunctionValue>("getBytesWritten", bytes_written_fn, env, tok),
                false, false, false, tok};
            obj->properties["getTotalSize"] = PropertyDescriptor{
                std::make_shared<FunctionValue>("getTotalSize", total_size_fn, env, tok),
                false, false, false, tok};
            obj->properties["getProgress"] = PropertyDescriptor{
                std::make_shared<FunctionValue>("getProgress", progress_fn, env, tok),
                false, false, false, tok};
            obj->properties["getChunksProcessed"] = PropertyDescriptor{
                std::make_shared<FunctionValue>("getChunksProcessed", chunks_fn, env, tok),
                false, false, false, tok};
            obj->properties["isFinalized"] = PropertyDescriptor{
                std::make_shared<FunctionValue>("isFinalized", finalized_fn, env, tok),
                false, false, false, tok};

            return Value{obj};
        };
        auto fn_val = std::make_shared<FunctionValue>("archiver.createGzipDecompressor", fn, env, tok);
        obj->properties["createGzipDecompressor"] = {Value{fn_val}, false, false, false, tok};
    }

    // archiver.gzipBuffer(buffer, level=6) -> Buffer
    {
        auto fn = [](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
            if (args.empty()) {
                throw SwaziError("TypeError", "gzipBuffer requires input buffer or string", token.loc);
            }

            std::vector<uint8_t> input;
            if (std::holds_alternative<BufferPtr>(args[0])) {
                input = std::get<BufferPtr>(args[0])->data;
            } else if (std::holds_alternative<std::string>(args[0])) {
                std::string s = std::get<std::string>(args[0]);
                input.assign(s.begin(), s.end());
            } else {
                throw SwaziError("TypeError", "gzipBuffer requires Buffer or string", token.loc);
            }

            int level = 6;
            if (args.size() >= 2 && std::holds_alternative<double>(args[1])) {
                level = static_cast<int>(std::get<double>(args[1]));
                level = std::clamp(level, 1, 9);
            }

            auto compressed = gzip_compress(input, level);

            auto buf = std::make_shared<BufferValue>();
            buf->data = std::move(compressed);
            buf->encoding = "binary";

            return Value{buf};
        };
        auto fn_val = std::make_shared<FunctionValue>("archiver.gzipBuffer", fn, env, tok);
        obj->properties["gzipBuffer"] = {Value{fn_val}, false, false, false, tok};
    }

    // archiver.gunzipBuffer(buffer) -> Buffer
    {
        auto fn = [](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
            if (args.empty()) {
                throw SwaziError("TypeError", "gunzipBuffer requires input buffer or string", token.loc);
            }

            std::vector<uint8_t> input;
            if (std::holds_alternative<BufferPtr>(args[0])) {
                input = std::get<BufferPtr>(args[0])->data;
            } else if (std::holds_alternative<std::string>(args[0])) {
                std::string s = std::get<std::string>(args[0]);
                input.assign(s.begin(), s.end());
            } else {
                throw SwaziError("TypeError", "gunzipBuffer requires Buffer or string", token.loc);
            }

            auto decompressed = gzip_decompress(input);

            auto buf = std::make_shared<BufferValue>();
            buf->data = std::move(decompressed);
            buf->encoding = "binary";

            return Value{buf};
        };
        auto fn_val = std::make_shared<FunctionValue>("archiver.gunzipBuffer", fn, env, tok);
        obj->properties["gunzipBuffer"] = {Value{fn_val}, false, false, false, tok};
    }

    // archiver.deflate(buffer, level=6) -> Buffer
    {
        auto fn = [](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
            if (args.empty()) {
                throw SwaziError("TypeError", "deflate requires input buffer", token.loc);
            }

            std::vector<uint8_t> input;
            if (std::holds_alternative<BufferPtr>(args[0])) {
                input = std::get<BufferPtr>(args[0])->data;
            } else if (std::holds_alternative<std::string>(args[0])) {
                std::string s = std::get<std::string>(args[0]);
                input.assign(s.begin(), s.end());
            } else {
                throw SwaziError("TypeError", "deflate requires Buffer or string", token.loc);
            }

            int level = 6;
            if (args.size() >= 2 && std::holds_alternative<double>(args[1])) {
                level = static_cast<int>(std::get<double>(args[1]));
                level = std::clamp(level, 1, 9);
            }

            auto compressed = deflate_compress(input, level);

            auto buf = std::make_shared<BufferValue>();
            buf->data = std::move(compressed);
            buf->encoding = "binary";

            return Value{buf};
        };
        auto fn_val = std::make_shared<FunctionValue>("archiver.deflate", fn, env, tok);
        obj->properties["deflate"] = {Value{fn_val}, false, false, false, tok};
    }

    // archiver.inflate(buffer) -> Buffer
    {
        auto fn = [](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
            if (args.empty()) {
                throw SwaziError("TypeError", "inflate requires input buffer", token.loc);
            }

            std::vector<uint8_t> input;
            if (std::holds_alternative<BufferPtr>(args[0])) {
                input = std::get<BufferPtr>(args[0])->data;
            } else if (std::holds_alternative<std::string>(args[0])) {
                std::string s = std::get<std::string>(args[0]);
                input.assign(s.begin(), s.end());
            } else {
                throw SwaziError("TypeError", "inflate requires Buffer or string", token.loc);
            }

            auto decompressed = deflate_decompress(input);

            auto buf = std::make_shared<BufferValue>();
            buf->data = std::move(decompressed);
            buf->encoding = "binary";

            return Value{buf};
        };
        auto fn_val = std::make_shared<FunctionValue>("archiver.inflate", fn, env, tok);
        obj->properties["inflate"] = {Value{fn_val}, false, false, false, tok};
    }
#else
    // Stubs when zlib not available
    auto stub_fn = [](const std::vector<Value>&, EnvPtr, const Token& token) -> Value {
        throw SwaziError("NotImplementedError",
            "This archiver function requires zlib. Build with zlib support.",
            token.loc);
    };
    auto stub_val = std::make_shared<FunctionValue>("archiver.stub", stub_fn, env, tok);
    obj->properties["gzip"] = {Value{stub_val}, false, false, false, tok};
    obj->properties["gunzip"] = {Value{stub_val}, false, false, false, tok};
    obj->properties["deflate"] = {Value{stub_val}, false, false, false, tok};
    obj->properties["inflate"] = {Value{stub_val}, false, false, false, tok};
    obj->properties["gzipBuffer"] = {Value{stub_val}, false, false, false, tok};
    obj->properties["gunzipBuffer"] = {Value{stub_val}, false, false, false, tok};
#endif

    // archiver.tar(files_array, output_path) -> bool
    // files_array: [{name: "file1.txt", content: buffer_or_string}, ...]
    {
        auto fn = [](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
            if (args.size() < 2 || !std::holds_alternative<ArrayPtr>(args[0])) {
                throw SwaziError("TypeError",
                    "tar requires array of files and output path",
                    token.loc);
            }

            ArrayPtr files_arr = std::get<ArrayPtr>(args[0]);
            std::string output = value_to_string_simple(args[1]);

            std::vector<std::pair<std::string, std::vector<uint8_t>>> files;

            for (const auto& elem : files_arr->elements) {
                if (!std::holds_alternative<ObjectPtr>(elem)) continue;

                ObjectPtr file_obj = std::get<ObjectPtr>(elem);

                auto name_it = file_obj->properties.find("name");
                auto data_it = file_obj->properties.find("content");

                if (name_it == file_obj->properties.end() ||
                    data_it == file_obj->properties.end()) {
                    continue;
                }

                std::string name = value_to_string_simple(name_it->second.value);

                std::vector<uint8_t> data;
                if (std::holds_alternative<BufferPtr>(data_it->second.value)) {
                    data = std::get<BufferPtr>(data_it->second.value)->data;
                } else if (std::holds_alternative<std::string>(data_it->second.value)) {
                    std::string s = std::get<std::string>(data_it->second.value);
                    data.assign(s.begin(), s.end());
                }

                files.emplace_back(name, std::move(data));
            }

            auto archive = tar::create(files);
            write_file_bytes(output, archive);

            return Value{true};
        };
        auto fn_val = std::make_shared<FunctionValue>("archiver.tar", fn, env, tok);
        obj->properties["tar"] = {Value{fn_val}, false, false, false, tok};
    }

    // archiver.untar(input_path) -> array of {name, data}
    {
        auto fn = [](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
            if (args.empty()) {
                throw SwaziError("TypeError", "untar requires input path", token.loc);
            }

            std::string input = value_to_string_simple(args[0]);
            auto archive = read_file_bytes(input);
            auto files = tar::extract(archive);

            auto result = std::make_shared<ArrayValue>();

            for (const auto& [name, data] : files) {
                auto file_obj = std::make_shared<ObjectValue>();

                file_obj->properties["name"] = {Value{name}, false, false, false, Token()};

                auto buf = std::make_shared<BufferValue>();
                buf->data = data;
                buf->encoding = "binary";
                file_obj->properties["content"] = {Value{buf}, false, false, false, Token()};

                result->elements.push_back(Value{file_obj});
            }

            return Value{result};
        };
        auto fn_val = std::make_shared<FunctionValue>("archiver.untar", fn, env, tok);
        obj->properties["untar"] = {Value{fn_val}, false, false, false, tok};
    }

    // archiver.tarBuffer(files_array) -> Buffer (in-memory tar creation)
    {
        auto fn = [](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
            if (args.empty() || !std::holds_alternative<ArrayPtr>(args[0])) {
                throw SwaziError("TypeError", "tarBuffer requires array of files", token.loc);
            }

            ArrayPtr files_arr = std::get<ArrayPtr>(args[0]);
            std::vector<std::pair<std::string, std::vector<uint8_t>>> files;

            for (const auto& elem : files_arr->elements) {
                if (!std::holds_alternative<ObjectPtr>(elem)) continue;
                ObjectPtr file_obj = std::get<ObjectPtr>(elem);

                auto name_it = file_obj->properties.find("name");
                auto data_it = file_obj->properties.find("content");
                if (name_it == file_obj->properties.end() ||
                    data_it == file_obj->properties.end()) continue;

                std::string name = value_to_string_simple(name_it->second.value);
                std::vector<uint8_t> data;
                if (std::holds_alternative<BufferPtr>(data_it->second.value)) {
                    data = std::get<BufferPtr>(data_it->second.value)->data;
                } else if (std::holds_alternative<std::string>(data_it->second.value)) {
                    std::string s = std::get<std::string>(data_it->second.value);
                    data.assign(s.begin(), s.end());
                }
                files.emplace_back(name, std::move(data));
            }

            auto archive = tar::create(files);
            auto buf = std::make_shared<BufferValue>();
            buf->data = std::move(archive);
            buf->encoding = "binary";
            return Value{buf};
        };
        auto fn_val = std::make_shared<FunctionValue>("archiver.tarBuffer", fn, env, tok);
        obj->properties["tarBuffer"] = {Value{fn_val}, false, false, false, tok};
    }

    // archiver.untarBuffer(buffer) -> array of {name, data}
    {
        auto fn = [](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
            if (args.empty()) {
                throw SwaziError("TypeError", "untarBuffer requires input buffer", token.loc);
            }

            std::vector<uint8_t> archive;
            if (std::holds_alternative<BufferPtr>(args[0])) {
                archive = std::get<BufferPtr>(args[0])->data;
            } else if (std::holds_alternative<std::string>(args[0])) {
                std::string s = std::get<std::string>(args[0]);
                archive.assign(s.begin(), s.end());
            } else {
                throw SwaziError("TypeError", "untarBuffer requires Buffer or string", token.loc);
            }

            auto files = tar::extract(archive);
            auto result = std::make_shared<ArrayValue>();

            for (const auto& [name, data] : files) {
                auto file_obj = std::make_shared<ObjectValue>();
                file_obj->properties["name"] = {Value{name}, false, false, false, Token()};

                auto buf = std::make_shared<BufferValue>();
                buf->data = data;
                buf->encoding = "binary";
                file_obj->properties["content"] = {Value{buf}, false, false, false, Token()};

                result->elements.push_back(Value{file_obj});
            }
            return Value{result};
        };
        auto fn_val = std::make_shared<FunctionValue>("archiver.untarBuffer", fn, env, tok);
        obj->properties["untarBuffer"] = {Value{fn_val}, false, false, false, tok};
    }

    // archiver.createTar(output_path) -> TarWriter object
    {
        auto fn = [env](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
            if (args.empty()) {
                throw SwaziError("TypeError", "createTar requires output path", token.loc);
            }

            std::string output_path = value_to_string_simple(args[0]);

            struct TarWriterState {
                std::ofstream output;
                bool finalized = false;
            };

            auto state = std::make_shared<TarWriterState>();
            state->output.open(output_path, std::ios::binary);

            if (!state->output.is_open()) {
                throw SwaziError("IOError", "Failed to open output file: " + output_path, token.loc);
            }

            auto obj = std::make_shared<ObjectValue>();

            // addFile(name, file_path, options?) method
            auto add_file_fn = [state](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
                if (state->finalized) {
                    throw SwaziError("IOError", "TarWriter already finalized", token.loc);
                }
                if (args.size() < 2) {
                    throw SwaziError("TypeError", "addFile requires name and file path", token.loc);
                }

                std::string name = value_to_string_simple(args[0]);
                std::string file_path = value_to_string_simple(args[1]);

                tar::FileMetadata meta;
                meta.name = name;

                // Parse options if provided
                if (args.size() >= 3 && std::holds_alternative<ObjectPtr>(args[2])) {
                    ObjectPtr opts = std::get<ObjectPtr>(args[2]);

                    auto mode_it = opts->properties.find("mode");
                    if (mode_it != opts->properties.end() && std::holds_alternative<double>(mode_it->second.value)) {
                        meta.mode = static_cast<uint32_t>(std::get<double>(mode_it->second.value));
                    }

                    auto uid_it = opts->properties.find("uid");
                    if (uid_it != opts->properties.end() && std::holds_alternative<double>(uid_it->second.value)) {
                        meta.uid = static_cast<uint32_t>(std::get<double>(uid_it->second.value));
                    }

                    auto gid_it = opts->properties.find("gid");
                    if (gid_it != opts->properties.end() && std::holds_alternative<double>(gid_it->second.value)) {
                        meta.gid = static_cast<uint32_t>(std::get<double>(gid_it->second.value));
                    }

                    auto mtime_it = opts->properties.find("mtime");
                    if (mtime_it != opts->properties.end() && std::holds_alternative<double>(mtime_it->second.value)) {
                        meta.mtime = static_cast<time_t>(std::get<double>(mtime_it->second.value));
                    }
                }

                // Get file size
                std::ifstream file(file_path, std::ios::binary | std::ios::ate);
                if (!file.is_open()) {
                    throw SwaziError("IOError", "Failed to open file: " + file_path, token.loc);
                }
                std::streamsize file_size = file.tellg();
                file.seekg(0, std::ios::beg);

                // Write header
                tar::write_header(state->output, meta, file_size);

                // Stream file content
                std::vector<char> buffer(32768);
                while (file) {
                    file.read(buffer.data(), buffer.size());
                    std::streamsize bytes_read = file.gcount();
                    if (bytes_read > 0) {
                        state->output.write(buffer.data(), bytes_read);
                    }
                }

                // Write padding
                tar::write_padding(state->output, file_size);

                return std::monostate{};
            };

            // addBuffer(name, buffer, options?) method
            auto add_buffer_fn = [state](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
                if (state->finalized) {
                    throw SwaziError("IOError", "TarWriter already finalized", token.loc);
                }
                if (args.size() < 2) {
                    throw SwaziError("TypeError", "addBuffer requires name and buffer", token.loc);
                }

                std::string name = value_to_string_simple(args[0]);

                std::vector<uint8_t> data;
                if (std::holds_alternative<BufferPtr>(args[1])) {
                    data = std::get<BufferPtr>(args[1])->data;
                } else if (std::holds_alternative<std::string>(args[1])) {
                    std::string str = std::get<std::string>(args[1]);
                    data.assign(str.begin(), str.end());
                } else {
                    throw SwaziError("TypeError", "buffer must be Buffer or string", token.loc);
                }

                tar::FileMetadata meta;
                meta.name = name;

                // Parse options if provided
                if (args.size() >= 3 && std::holds_alternative<ObjectPtr>(args[2])) {
                    ObjectPtr opts = std::get<ObjectPtr>(args[2]);

                    auto mode_it = opts->properties.find("mode");
                    if (mode_it != opts->properties.end() && std::holds_alternative<double>(mode_it->second.value)) {
                        meta.mode = static_cast<uint32_t>(std::get<double>(mode_it->second.value));
                    }

                    auto uid_it = opts->properties.find("uid");
                    if (uid_it != opts->properties.end() && std::holds_alternative<double>(uid_it->second.value)) {
                        meta.uid = static_cast<uint32_t>(std::get<double>(uid_it->second.value));
                    }

                    auto gid_it = opts->properties.find("gid");
                    if (gid_it != opts->properties.end() && std::holds_alternative<double>(gid_it->second.value)) {
                        meta.gid = static_cast<uint32_t>(std::get<double>(gid_it->second.value));
                    }

                    auto mtime_it = opts->properties.find("mtime");
                    if (mtime_it != opts->properties.end() && std::holds_alternative<double>(mtime_it->second.value)) {
                        meta.mtime = static_cast<time_t>(std::get<double>(mtime_it->second.value));
                    }
                }

                // Write header
                tar::write_header(state->output, meta, data.size());

                // Write data
                state->output.write(reinterpret_cast<const char*>(data.data()), data.size());

                // Write padding
                tar::write_padding(state->output, data.size());

                return std::monostate{};
            };

            // finalize() method
            auto finalize_fn = [state](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
                if (state->finalized) {
                    throw SwaziError("IOError", "TarWriter already finalized", token.loc);
                }

                // Write end-of-archive marker (two zero blocks)
                std::vector<uint8_t> zeros(1024, 0);
                state->output.write(reinterpret_cast<const char*>(zeros.data()), 1024);
                state->output.close();
                state->finalized = true;

                return std::monostate{};
            };

            Token tok;
            tok.type = TokenType::IDENTIFIER;
            tok.loc = TokenLocation("<archiver>", 0, 0, 0);

            obj->properties["addFile"] = PropertyDescriptor{
                std::make_shared<FunctionValue>("addFile", add_file_fn, env, tok),
                false, false, false, tok};
            obj->properties["addBuffer"] = PropertyDescriptor{
                std::make_shared<FunctionValue>("addBuffer", add_buffer_fn, env, tok),
                false, false, false, tok};
            obj->properties["finalize"] = PropertyDescriptor{
                std::make_shared<FunctionValue>("finalize", finalize_fn, env, tok),
                false, false, false, tok};

            return Value{obj};
        };
        auto fn_val = std::make_shared<FunctionValue>("archiver.createTar", fn, env, tok);
        obj->properties["createTar"] = {Value{fn_val}, false, false, false, tok};
    }

#ifdef HAVE_ZLIB
    // archiver.compress(buffer, options?) -> Buffer
    // Unified compression with options: {algorithm: "gzip"|"deflate", level: 1-9}
    {
        auto fn = [](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
            if (args.empty()) {
                throw SwaziError("TypeError", "compress requires input buffer or string", token.loc);
            }

            std::vector<uint8_t> input;
            if (std::holds_alternative<BufferPtr>(args[0])) {
                input = std::get<BufferPtr>(args[0])->data;
            } else if (std::holds_alternative<std::string>(args[0])) {
                std::string s = std::get<std::string>(args[0]);
                input.assign(s.begin(), s.end());
            } else {
                throw SwaziError("TypeError", "compress requires Buffer or string", token.loc);
            }

            std::string algorithm = "gzip";
            int level = 6;

            if (args.size() >= 2 && std::holds_alternative<ObjectPtr>(args[1])) {
                ObjectPtr opts = std::get<ObjectPtr>(args[1]);

                auto algo_it = opts->properties.find("algorithm");
                if (algo_it != opts->properties.end()) {
                    algorithm = value_to_string_simple(algo_it->second.value);
                }

                auto level_it = opts->properties.find("level");
                if (level_it != opts->properties.end() &&
                    std::holds_alternative<double>(level_it->second.value)) {
                    level = static_cast<int>(std::get<double>(level_it->second.value));
                    level = std::clamp(level, 1, 9);
                }
            }

            std::vector<uint8_t> compressed;
            if (algorithm == "gzip") {
                compressed = gzip_compress(input, level);
            } else if (algorithm == "deflate") {
                compressed = deflate_compress(input, level);
            } else {
                throw SwaziError("ValueError",
                    "Unknown algorithm: " + algorithm + " (use 'gzip' or 'deflate')",
                    token.loc);
            }

            auto buf = std::make_shared<BufferValue>();
            buf->data = std::move(compressed);
            buf->encoding = "binary";
            return Value{buf};
        };
        auto fn_val = std::make_shared<FunctionValue>("archiver.compress", fn, env, tok);
        obj->properties["compress"] = {Value{fn_val}, false, false, false, tok};
    }

    // archiver.decompress(buffer, options?) -> Buffer
    // Unified decompression with options: {algorithm: "gzip"|"deflate"}
    {
        auto fn = [](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
            if (args.empty()) {
                throw SwaziError("TypeError", "decompress requires input buffer", token.loc);
            }

            std::vector<uint8_t> input;
            if (std::holds_alternative<BufferPtr>(args[0])) {
                input = std::get<BufferPtr>(args[0])->data;
            } else if (std::holds_alternative<std::string>(args[0])) {
                std::string s = std::get<std::string>(args[0]);
                input.assign(s.begin(), s.end());
            } else {
                throw SwaziError("TypeError", "decompress requires Buffer or string", token.loc);
            }

            std::string algorithm = "gzip";

            if (args.size() >= 2 && std::holds_alternative<ObjectPtr>(args[1])) {
                ObjectPtr opts = std::get<ObjectPtr>(args[1]);
                auto algo_it = opts->properties.find("algorithm");
                if (algo_it != opts->properties.end()) {
                    algorithm = value_to_string_simple(algo_it->second.value);
                }
            }

            std::vector<uint8_t> decompressed;
            if (algorithm == "gzip") {
                decompressed = gzip_decompress(input);
            } else if (algorithm == "deflate") {
                decompressed = deflate_decompress(input);
            } else {
                throw SwaziError("ValueError",
                    "Unknown algorithm: " + algorithm + " (use 'gzip' or 'deflate')",
                    token.loc);
            }

            auto buf = std::make_shared<BufferValue>();
            buf->data = std::move(decompressed);
            buf->encoding = "binary";
            return Value{buf};
        };
        auto fn_val = std::make_shared<FunctionValue>("archiver.decompress", fn, env, tok);
        obj->properties["decompress"] = {Value{fn_val}, false, false, false, tok};
    }

    // archiver.getCompressionRatio(original, compressed) -> number
    {
        auto fn = [](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
            if (args.size() < 2) {
                throw SwaziError("TypeError", "getCompressionRatio requires original and compressed sizes", token.loc);
            }

            size_t original_size = 0;
            size_t compressed_size = 0;

            // Get original size
            if (std::holds_alternative<BufferPtr>(args[0])) {
                original_size = std::get<BufferPtr>(args[0])->data.size();
            } else if (std::holds_alternative<double>(args[0])) {
                original_size = static_cast<size_t>(std::get<double>(args[0]));
            } else if (std::holds_alternative<std::string>(args[0])) {
                original_size = std::get<std::string>(args[0]).size();
            }

            // Get compressed size
            if (std::holds_alternative<BufferPtr>(args[1])) {
                compressed_size = std::get<BufferPtr>(args[1])->data.size();
            } else if (std::holds_alternative<double>(args[1])) {
                compressed_size = static_cast<size_t>(std::get<double>(args[1]));
            } else if (std::holds_alternative<std::string>(args[1])) {
                compressed_size = std::get<std::string>(args[1]).size();
            }

            if (original_size == 0) return Value{0.0};

            double ratio = 1.0 - (static_cast<double>(compressed_size) / static_cast<double>(original_size));
            return Value{ratio * 100.0};  // return as percentage
        };
        auto fn_val = std::make_shared<FunctionValue>("archiver.getCompressionRatio", fn, env, tok);
        obj->properties["getCompressionRatio"] = {Value{fn_val}, false, false, false, tok};
    }
#else
    obj->properties["compress"] = {Value{stub_val}, false, false, false, tok};
    obj->properties["decompress"] = {Value{stub_val}, false, false, false, tok};
    obj->properties["getCompressionRatio"] = {Value{stub_val}, false, false, false, tok};
#endif

    // archiver.listTar(input_path) -> array of {name, size, mode, mtime}
    // List tar contents without extracting
    {
        auto fn = [](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
            if (args.empty()) {
                throw SwaziError("TypeError", "listTar requires input path or buffer", token.loc);
            }

            std::vector<uint8_t> archive;
            if (std::holds_alternative<std::string>(args[0])) {
                archive = read_file_bytes(value_to_string_simple(args[0]));
            } else if (std::holds_alternative<BufferPtr>(args[0])) {
                archive = std::get<BufferPtr>(args[0])->data;
            } else {
                throw SwaziError("TypeError", "listTar requires path string or Buffer", token.loc);
            }

            auto result = std::make_shared<ArrayValue>();
            size_t pos = 0;

            while (pos + 512 <= archive.size()) {
                const tar::Header* hdr = reinterpret_cast<const tar::Header*>(&archive[pos]);
                if (hdr->name[0] == '\0') break;

                auto file_obj = std::make_shared<ObjectValue>();
                file_obj->properties["name"] = {Value{std::string(hdr->name)}, false, false, false, Token()};
                file_obj->properties["size"] = {Value{static_cast<double>(tar::read_octal(hdr->size, 11))}, false, false, false, Token()};
                file_obj->properties["mode"] = {Value{static_cast<double>(tar::read_octal(hdr->mode, 7))}, false, false, false, Token()};
                file_obj->properties["mtime"] = {Value{static_cast<double>(tar::read_octal(hdr->mtime, 11))}, false, false, false, Token()};

                result->elements.push_back(Value{file_obj});

                uint64_t size = tar::read_octal(hdr->size, 11);
                pos += 512 + size;
                size_t padding = (512 - (size % 512)) % 512;
                pos += padding;
            }

            return Value{result};
        };
        auto fn_val = std::make_shared<FunctionValue>("archiver.listTar", fn, env, tok);
        obj->properties["listTar"] = {Value{fn_val}, false, false, false, tok};
    }

    // archiver.extractTarFile(tar_path, file_name, output_path?, options?) -> Buffer or Extractor object
    {
        auto fn = [env](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
            if (args.size() < 2) {
                throw SwaziError("TypeError", "extractTarFile requires tar path and file name", token.loc);
            }

            std::string tar_path;
            std::vector<uint8_t> tar_data;
            bool from_buffer = false;

            if (std::holds_alternative<std::string>(args[0])) {
                tar_path = value_to_string_simple(args[0]);
            } else if (std::holds_alternative<BufferPtr>(args[0])) {
                tar_data = std::get<BufferPtr>(args[0])->data;
                from_buffer = true;
            } else {
                throw SwaziError("TypeError", "extractTarFile requires path string or Buffer", token.loc);
            }

            std::string target_name = value_to_string_simple(args[1]);

            // Check if output path provided (3rd argument)
            bool has_output_path = args.size() >= 3 && std::holds_alternative<std::string>(args[2]);
            std::string output_path = has_output_path ? value_to_string_simple(args[2]) : "";

            // Check for streaming option (4th argument)
            bool streaming = false;
            if (args.size() >= 4 && std::holds_alternative<ObjectPtr>(args[3])) {
                ObjectPtr opts = std::get<ObjectPtr>(args[3]);
                auto stream_it = opts->properties.find("streaming");
                if (stream_it != opts->properties.end() && std::holds_alternative<bool>(stream_it->second.value)) {
                    streaming = std::get<bool>(stream_it->second.value);
                }
            }

            // If no output path, use old behavior (return buffer)
            if (!has_output_path) {
                if (from_buffer) {
                    auto files = tar::extract(tar_data);
                    for (const auto& [name, data] : files) {
                        if (name == target_name) {
                            auto buf = std::make_shared<BufferValue>();
                            buf->data = data;
                            buf->encoding = "binary";
                            return Value{buf};
                        }
                    }
                } else {
                    auto archive = read_file_bytes(tar_path);
                    auto files = tar::extract(archive);
                    for (const auto& [name, data] : files) {
                        if (name == target_name) {
                            auto buf = std::make_shared<BufferValue>();
                            buf->data = data;
                            buf->encoding = "binary";
                            return Value{buf};
                        }
                    }
                }
                return Value{std::monostate{}};
            }

            // If streaming not requested, extract directly to file
            if (!streaming) {
                if (from_buffer) {
                    auto files = tar::extract(tar_data);
                    for (const auto& [name, data] : files) {
                        if (name == target_name) {
                            write_file_bytes(output_path, data);
                            return Value{true};
                        }
                    }
                } else {
                    auto archive = read_file_bytes(tar_path);
                    auto files = tar::extract(archive);
                    for (const auto& [name, data] : files) {
                        if (name == target_name) {
                            write_file_bytes(output_path, data);
                            return Value{true};
                        }
                    }
                }
                return Value{false};
            }

            // Streaming extraction - create extractor object
            struct TarExtractorState {
                std::ifstream tar_file;
                std::ofstream output_file;
                std::string target_name;
                bool file_found = false;
                bool finalized = false;
                bool from_buffer = false;

                // File info
                size_t file_size = 0;
                size_t bytes_extracted = 0;
                size_t current_pos = 0;

                // Streaming state
                std::vector<uint8_t> buffer;
                size_t bytes_remaining = 0;
            };

            auto state = std::make_shared<TarExtractorState>();
            state->target_name = target_name;
            state->from_buffer = from_buffer;
            state->buffer.resize(32768);

            if (!from_buffer) {
                state->tar_file.open(tar_path, std::ios::binary);
                if (!state->tar_file.is_open()) {
                    throw SwaziError("IOError", "Failed to open TAR file: " + tar_path, token.loc);
                }
            } else {
                throw SwaziError("NotImplementedError",
                    "Streaming extraction from buffer not yet supported (use file path)", token.loc);
            }

            state->output_file.open(output_path, std::ios::binary);
            if (!state->output_file.is_open()) {
                throw SwaziError("IOError", "Failed to open output file: " + output_path, token.loc);
            }

            // Find the target file in TAR
            bool found = false;
            while (state->tar_file) {
                tar::Header hdr{};
                state->tar_file.read(reinterpret_cast<char*>(&hdr), sizeof(hdr));

                if (state->tar_file.gcount() < sizeof(hdr) || hdr.name[0] == '\0') {
                    break;
                }

                std::string name(hdr.name);
                uint64_t size = tar::read_octal(hdr.size, 11);

                if (name == target_name) {
                    state->file_found = true;
                    state->file_size = size;
                    state->bytes_remaining = size;
                    found = true;
                    break;
                }

                // Skip this file
                size_t skip = size;
                size_t padding = (512 - (size % 512)) % 512;
                skip += padding;
                state->tar_file.seekg(skip, std::ios::cur);
            }

            if (!found) {
                state->tar_file.close();
                state->output_file.close();
                throw SwaziError("ValueError", "File not found in TAR: " + target_name, token.loc);
            }

            auto obj = std::make_shared<ObjectValue>();

            // process() method - extract chunk by chunk
            auto process_fn = [state](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
                if (state->finalized) {
                    return Value{false};
                }

                if (state->bytes_remaining == 0) {
                    state->output_file.close();
                    state->tar_file.close();
                    state->finalized = true;
                    return Value{false};
                }

                // Read chunk
                size_t to_read = std::min(state->buffer.size(), state->bytes_remaining);
                state->tar_file.read(reinterpret_cast<char*>(state->buffer.data()), to_read);
                std::streamsize bytes_read = state->tar_file.gcount();

                if (bytes_read <= 0) {
                    state->output_file.close();
                    state->tar_file.close();
                    state->finalized = true;
                    return Value{false};
                }

                // Write to output
                state->output_file.write(reinterpret_cast<const char*>(state->buffer.data()), bytes_read);
                state->bytes_extracted += bytes_read;
                state->bytes_remaining -= bytes_read;

                return Value{state->bytes_remaining > 0};
            };

            // getFileSize() -> number
            auto file_size_fn = [state](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
                return Value{static_cast<double>(state->file_size)};
            };

            // getBytesExtracted() -> number
            auto bytes_extracted_fn = [state](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
                return Value{static_cast<double>(state->bytes_extracted)};
            };

            // getBytesRemaining() -> number
            auto bytes_remaining_fn = [state](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
                return Value{static_cast<double>(state->bytes_remaining)};
            };

            // getProgress() -> number (0.0 to 1.0)
            auto progress_fn = [state](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
                if (state->file_size == 0) return Value{1.0};
                return Value{static_cast<double>(state->bytes_extracted) / static_cast<double>(state->file_size)};
            };

            // isFinalized() -> bool
            auto finalized_fn = [state](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
                return Value{state->finalized};
            };

            // getFileName() -> string
            auto filename_fn = [state](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
                return Value{state->target_name};
            };

            Token tok;
            tok.type = TokenType::IDENTIFIER;
            tok.loc = TokenLocation("<archiver>", 0, 0, 0);

            obj->properties["process"] = PropertyDescriptor{
                std::make_shared<FunctionValue>("process", process_fn, env, tok),
                false, false, false, tok};
            obj->properties["getFileSize"] = PropertyDescriptor{
                std::make_shared<FunctionValue>("getFileSize", file_size_fn, env, tok),
                false, false, false, tok};
            obj->properties["getBytesExtracted"] = PropertyDescriptor{
                std::make_shared<FunctionValue>("getBytesExtracted", bytes_extracted_fn, env, tok),
                false, false, false, tok};
            obj->properties["getBytesRemaining"] = PropertyDescriptor{
                std::make_shared<FunctionValue>("getBytesRemaining", bytes_remaining_fn, env, tok),
                false, false, false, tok};
            obj->properties["getProgress"] = PropertyDescriptor{
                std::make_shared<FunctionValue>("getProgress", progress_fn, env, tok),
                false, false, false, tok};
            obj->properties["isFinalized"] = PropertyDescriptor{
                std::make_shared<FunctionValue>("isFinalized", finalized_fn, env, tok),
                false, false, false, tok};
            obj->properties["getFileName"] = PropertyDescriptor{
                std::make_shared<FunctionValue>("getFileName", filename_fn, env, tok),
                false, false, false, tok};

            return Value{obj};
        };
        auto fn_val = std::make_shared<FunctionValue>("archiver.extractTarFile", fn, env, tok);
        obj->properties["extractTarFile"] = {Value{fn_val}, false, false, false, tok};
    }

    return obj;
}