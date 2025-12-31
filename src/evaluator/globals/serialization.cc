#include <cstring>
#include <unordered_map>
#include <vector>

#include "SwaziError.hpp"
#include "builtins.hpp"
#include "evaluator.hpp"

#if defined(HAVE_LIBSODIUM)
#include <sodium.h>
#endif

// Serialization format version
static const uint8_t SWAZI_SERIALIZE_VERSION = 1;

// Type tags for serialization
enum class SerializeType : uint8_t {
    NULL_TYPE = 0x00,
    BOOL_TRUE = 0x01,
    BOOL_FALSE = 0x02,
    NUMBER = 0x03,
    STRING = 0x04,
    ARRAY = 0x05,
    OBJECT = 0x06,
    BUFFER = 0x07,
    DATETIME = 0x08,
    RANGE = 0x09,
    HOLE = 0x0A,
    REFERENCE = 0x0B,  // For circular references
    REGEX = 0x0C
};

// Helper to write bytes
class ByteWriter {
   public:
    std::vector<uint8_t> data;

    void write_u8(uint8_t val) {
        data.push_back(val);
    }

    void write_u16(uint16_t val) {
        data.push_back(static_cast<uint8_t>(val & 0xFF));
        data.push_back(static_cast<uint8_t>((val >> 8) & 0xFF));
    }

    void write_u32(uint32_t val) {
        data.push_back(static_cast<uint8_t>(val & 0xFF));
        data.push_back(static_cast<uint8_t>((val >> 8) & 0xFF));
        data.push_back(static_cast<uint8_t>((val >> 16) & 0xFF));
        data.push_back(static_cast<uint8_t>((val >> 24) & 0xFF));
    }

    void write_u64(uint64_t val) {
        for (int i = 0; i < 8; i++) {
            data.push_back(static_cast<uint8_t>((val >> (i * 8)) & 0xFF));
        }
    }

    void write_double(double val) {
        uint64_t bits;
        std::memcpy(&bits, &val, sizeof(double));
        write_u64(bits);
    }

    void write_string(const std::string& str) {
        write_u32(static_cast<uint32_t>(str.size()));
        data.insert(data.end(), str.begin(), str.end());
    }

    void write_bytes(const std::vector<uint8_t>& bytes) {
        write_u32(static_cast<uint32_t>(bytes.size()));
        data.insert(data.end(), bytes.begin(), bytes.end());
    }
};

// Helper to read bytes
class ByteReader {
   public:
    const uint8_t* data;
    size_t size;
    size_t pos = 0;

    ByteReader(const uint8_t* d, size_t s) : data(d), size(s) {}

    void check_available(size_t n, const Token& token) {
        if (pos + n > size) {
            throw SwaziError("DeserializeError", "Unexpected end of data", token.loc);
        }
    }

    uint8_t read_u8(const Token& token) {
        check_available(1, token);
        return data[pos++];
    }

    uint16_t read_u16(const Token& token) {
        check_available(2, token);
        uint16_t val = data[pos] | (data[pos + 1] << 8);
        pos += 2;
        return val;
    }

    uint32_t read_u32(const Token& token) {
        check_available(4, token);
        uint32_t val = data[pos] | (data[pos + 1] << 8) | (data[pos + 2] << 16) | (data[pos + 3] << 24);
        pos += 4;
        return val;
    }

    uint64_t read_u64(const Token& token) {
        check_available(8, token);
        uint64_t val = 0;
        for (int i = 0; i < 8; i++) {
            val |= static_cast<uint64_t>(data[pos++]) << (i * 8);
        }
        return val;
    }

    double read_double(const Token& token) {
        uint64_t bits = read_u64(token);
        double val;
        std::memcpy(&val, &bits, sizeof(double));
        return val;
    }

    std::string read_string(const Token& token) {
        uint32_t len = read_u32(token);
        if (len > 10 * 1024 * 1024) {  // 10MB string limit
            throw SwaziError("DeserializeError", "String too large", token.loc);
        }
        check_available(len, token);
        std::string str(reinterpret_cast<const char*>(data + pos), len);
        pos += len;
        return str;
    }

    std::vector<uint8_t> read_bytes(const Token& token) {
        uint32_t len = read_u32(token);
        if (len > 100 * 1024 * 1024) {  // 100MB buffer limit
            throw SwaziError("DeserializeError", "Buffer too large", token.loc);
        }
        check_available(len, token);
        std::vector<uint8_t> bytes(data + pos, data + pos + len);
        pos += len;
        return bytes;
    }
};

// Serialization context for tracking references
struct SerializeContext {
    std::unordered_map<const void*, uint32_t> object_refs;
    std::unordered_map<const void*, uint32_t> array_refs;
    uint32_t next_id = 0;

    bool has_object_ref(const void* ptr) const {
        return object_refs.find(ptr) != object_refs.end();
    }

    bool has_array_ref(const void* ptr) const {
        return array_refs.find(ptr) != array_refs.end();
    }

    uint32_t get_object_ref(const void* ptr) const {
        auto it = object_refs.find(ptr);
        return it != object_refs.end() ? it->second : 0;
    }

    uint32_t get_array_ref(const void* ptr) const {
        auto it = array_refs.find(ptr);
        return it != array_refs.end() ? it->second : 0;
    }

    uint32_t add_object_ref(const void* ptr) {
        uint32_t id = next_id++;
        object_refs[ptr] = id;
        return id;
    }

    uint32_t add_array_ref(const void* ptr) {
        uint32_t id = next_id++;
        array_refs[ptr] = id;
        return id;
    }
};

// Deserialization context for tracking references
struct DeserializeContext {
    std::unordered_map<uint32_t, Value> refs;

    void add_ref(uint32_t id, const Value& val) {
        refs[id] = val;
    }

    Value get_ref(uint32_t id, const Token& token) {
        auto it = refs.find(id);
        if (it == refs.end()) {
            throw SwaziError("DeserializeError", "Invalid reference ID: " + std::to_string(id), token.loc);
        }
        return it->second;
    }
};

// Forward declarations
static void serialize_value(const Value& val, ByteWriter& writer, SerializeContext& ctx,
    FunctionPtr replacer, Evaluator* evaluator, EnvPtr callEnv, const Token& token);
static Value deserialize_value(ByteReader& reader, DeserializeContext& ctx,
    FunctionPtr reviver, Evaluator* evaluator, EnvPtr callEnv, const Token& token);

// Apply replacer function if provided
static std::pair<bool, Value> apply_replacer(const std::string& key, const Value& value,
    FunctionPtr replacer, Evaluator* evaluator,
    EnvPtr callEnv, const Token& token) {
    if (!replacer || !evaluator) {
        return {true, value};
    }

    try {
        std::vector<Value> args = {Value{key}, value};
        Value result = evaluator->invoke_function(replacer, args, callEnv, token);

        // If replacer returns undefined, remove the value
        if (std::holds_alternative<std::monostate>(result)) {
            return {false, Value{}};
        }

        return {true, result};
    } catch (const std::exception& e) {
        throw SwaziError("SerializeError",
            "Replacer function error for key '" + key + "': " + e.what(), token.loc);
    }
}

// Serialize a single value
static void serialize_value(const Value& val, ByteWriter& writer, SerializeContext& ctx,
    FunctionPtr replacer, Evaluator* evaluator, EnvPtr callEnv, const Token& token) {
    // Handle null/undefined
    if (std::holds_alternative<std::monostate>(val)) {
        writer.write_u8(static_cast<uint8_t>(SerializeType::NULL_TYPE));
        return;
    }

    // Handle boolean
    if (std::holds_alternative<bool>(val)) {
        bool b = std::get<bool>(val);
        writer.write_u8(static_cast<uint8_t>(b ? SerializeType::BOOL_TRUE : SerializeType::BOOL_FALSE));
        return;
    }

    // Handle number
    if (std::holds_alternative<double>(val)) {
        writer.write_u8(static_cast<uint8_t>(SerializeType::NUMBER));
        writer.write_double(std::get<double>(val));
        return;
    }

    // Handle string
    if (std::holds_alternative<std::string>(val)) {
        writer.write_u8(static_cast<uint8_t>(SerializeType::STRING));
        writer.write_string(std::get<std::string>(val));
        return;
    }

    // Handle HoleValue
    if (std::holds_alternative<HoleValue>(val)) {
        writer.write_u8(static_cast<uint8_t>(SerializeType::HOLE));
        return;
    }

    // Handle Buffer
    if (std::holds_alternative<BufferPtr>(val)) {
        BufferPtr buf = std::get<BufferPtr>(val);
        if (!buf) {
            throw SwaziError("SerializeError", "Cannot serialize null Buffer", token.loc);
        }
        writer.write_u8(static_cast<uint8_t>(SerializeType::BUFFER));
        writer.write_string(buf->encoding);
        writer.write_bytes(buf->data);
        return;
    }

    // Handle DateTime
    if (std::holds_alternative<DateTimePtr>(val)) {
        DateTimePtr dt = std::get<DateTimePtr>(val);
        if (!dt) {
            throw SwaziError("SerializeError", "Cannot serialize null DateTime", token.loc);
        }
        writer.write_u8(static_cast<uint8_t>(SerializeType::DATETIME));
        writer.write_u64(dt->epochNanoseconds);
        writer.write_u32(static_cast<uint32_t>(dt->year));
        writer.write_u8(static_cast<uint8_t>(dt->month));
        writer.write_u8(static_cast<uint8_t>(dt->day));
        writer.write_u8(static_cast<uint8_t>(dt->hour));
        writer.write_u8(static_cast<uint8_t>(dt->minute));
        writer.write_u8(static_cast<uint8_t>(dt->second));
        writer.write_u32(dt->fractionalNanoseconds);
        writer.write_u8(static_cast<uint8_t>(dt->precision));
        writer.write_u32(static_cast<uint32_t>(dt->tzOffsetSeconds));
        writer.write_u8(dt->isUTC ? 1 : 0);
        writer.write_string(dt->literalText);
        return;
    }

    // Handle Range
    if (std::holds_alternative<RangePtr>(val)) {
        RangePtr range = std::get<RangePtr>(val);
        if (!range) {
            throw SwaziError("SerializeError", "Cannot serialize null Range", token.loc);
        }
        writer.write_u8(static_cast<uint8_t>(SerializeType::RANGE));
        writer.write_u32(static_cast<uint32_t>(range->start));
        writer.write_u32(static_cast<uint32_t>(range->end));
        writer.write_u32(static_cast<uint32_t>(range->step));
        writer.write_u32(static_cast<uint32_t>(range->cur));
        writer.write_u8(range->inclusive ? 1 : 0);
        writer.write_u8(range->increasing ? 1 : 0);
        return;
    }

    // Handle Array
    if (std::holds_alternative<ArrayPtr>(val)) {
        ArrayPtr arr = std::get<ArrayPtr>(val);
        if (!arr) {
            throw SwaziError("SerializeError", "Cannot serialize null Array", token.loc);
        }

        const void* ptr = arr.get();

        // Check for circular reference
        if (ctx.has_array_ref(ptr)) {
            writer.write_u8(static_cast<uint8_t>(SerializeType::REFERENCE));
            writer.write_u32(ctx.get_array_ref(ptr));
            return;
        }

        // Add to reference map
        uint32_t ref_id = ctx.add_array_ref(ptr);

        writer.write_u8(static_cast<uint8_t>(SerializeType::ARRAY));
        writer.write_u32(ref_id);
        writer.write_u32(static_cast<uint32_t>(arr->elements.size()));

        for (size_t i = 0; i < arr->elements.size(); ++i) {
            Value elem = arr->elements[i];

            // Apply replacer if provided
            if (replacer && evaluator) {
                auto result = apply_replacer(std::to_string(i), elem, replacer, evaluator, callEnv, token);
                if (!result.first) {
                    // Replacer removed this element - serialize as null
                    writer.write_u8(static_cast<uint8_t>(SerializeType::NULL_TYPE));
                    continue;
                }
                elem = result.second;
            }

            serialize_value(elem, writer, ctx, replacer, evaluator, callEnv, token);
        }
        return;
    }

    // Handle Object
    if (std::holds_alternative<ObjectPtr>(val)) {
        ObjectPtr obj = std::get<ObjectPtr>(val);
        if (!obj) {
            throw SwaziError("SerializeError", "Cannot serialize null Object", token.loc);
        }

        const void* ptr = obj.get();

        // Check for circular reference
        if (ctx.has_object_ref(ptr)) {
            writer.write_u8(static_cast<uint8_t>(SerializeType::REFERENCE));
            writer.write_u32(ctx.get_object_ref(ptr));
            return;
        }

        // Check for unsupported object types
        if (obj->is_env_proxy) {
            throw SwaziError("SerializeError", "Cannot serialize environment proxy objects", token.loc);
        }

        // Add to reference map
        uint32_t ref_id = ctx.add_object_ref(ptr);

        writer.write_u8(static_cast<uint8_t>(SerializeType::OBJECT));
        writer.write_u32(ref_id);

        // Count serializable properties (filter out functions and other unsupported types)
        std::vector<std::pair<std::string, Value>> serializable_props;
        for (const auto& kv : obj->properties) {
            const Value& prop_val = kv.second.value;

            // Skip functions, classes, promises, generators, files, proxies
            if (std::holds_alternative<FunctionPtr>(prop_val) ||
                std::holds_alternative<ClassPtr>(prop_val) ||
                std::holds_alternative<PromisePtr>(prop_val) ||
                std::holds_alternative<GeneratorPtr>(prop_val) ||
                std::holds_alternative<FilePtr>(prop_val) ||
                std::holds_alternative<ProxyPtr>(prop_val)) {
                continue;
            }

            serializable_props.push_back({kv.first, prop_val});
        }

        writer.write_u32(static_cast<uint32_t>(serializable_props.size()));

        for (const auto& kv : serializable_props) {
            writer.write_string(kv.first);

            Value prop_val = kv.second;

            // Apply replacer if provided
            if (replacer && evaluator) {
                auto result = apply_replacer(kv.first, prop_val, replacer, evaluator, callEnv, token);
                if (!result.first) {
                    // Replacer removed this property - serialize as null
                    writer.write_u8(static_cast<uint8_t>(SerializeType::NULL_TYPE));
                    continue;
                }
                prop_val = result.second;
            }

            serialize_value(prop_val, writer, ctx, replacer, evaluator, callEnv, token);
        }
        return;
    }

    // Handle Regex
    if (std::holds_alternative<RegexPtr>(val)) {
        RegexPtr regex = std::get<RegexPtr>(val);
        if (!regex) {
            throw SwaziError("SerializeError", "Cannot serialize null Regex", token.loc);
        }
        writer.write_u8(static_cast<uint8_t>(SerializeType::REGEX));
        writer.write_string(regex->pattern);
        writer.write_string(regex->flags);
        return;
    }

    // Unsupported types
    std::string type_name;
    if (std::holds_alternative<FunctionPtr>(val))
        type_name = "Function";
    else if (std::holds_alternative<ClassPtr>(val))
        type_name = "Class";
    else if (std::holds_alternative<PromisePtr>(val))
        type_name = "Promise";
    else if (std::holds_alternative<GeneratorPtr>(val))
        type_name = "Generator";
    else if (std::holds_alternative<FilePtr>(val))
        type_name = "File";
    else if (std::holds_alternative<ProxyPtr>(val))
        type_name = "Proxy";
    else if (std::holds_alternative<MapStoragePtr>(val))
        type_name = "Map";
    else
        type_name = "Unknown";

    throw SwaziError("SerializeError",
        "Cannot serialize type: " + type_name + ". Only primitives, Arrays, plain Objects, Buffers, DateTime, Ranges, and Holes are supported.",
        token.loc);
}

// Deserialize a single value
static Value deserialize_value(ByteReader& reader, DeserializeContext& ctx,
    FunctionPtr reviver, Evaluator* evaluator, EnvPtr callEnv, const Token& token) {
    uint8_t type_tag = reader.read_u8(token);

    switch (static_cast<SerializeType>(type_tag)) {
        case SerializeType::NULL_TYPE:
            return std::monostate{};

        case SerializeType::BOOL_TRUE:
            return true;

        case SerializeType::BOOL_FALSE:
            return false;

        case SerializeType::NUMBER:
            return reader.read_double(token);

        case SerializeType::STRING:
            return reader.read_string(token);

        case SerializeType::HOLE:
            return HoleValue{};

        case SerializeType::BUFFER: {
            auto buf = std::make_shared<BufferValue>();
            buf->encoding = reader.read_string(token);
            buf->data = reader.read_bytes(token);
            return Value{buf};
        }

        case SerializeType::DATETIME: {
            auto dt = std::make_shared<DateTimeValue>();
            dt->epochNanoseconds = reader.read_u64(token);
            dt->year = static_cast<int>(reader.read_u32(token));
            dt->month = static_cast<int>(reader.read_u8(token));
            dt->day = static_cast<int>(reader.read_u8(token));
            dt->hour = static_cast<int>(reader.read_u8(token));
            dt->minute = static_cast<int>(reader.read_u8(token));
            dt->second = static_cast<int>(reader.read_u8(token));
            dt->fractionalNanoseconds = reader.read_u32(token);
            dt->precision = static_cast<DateTimePrecision>(reader.read_u8(token));
            dt->tzOffsetSeconds = static_cast<int32_t>(reader.read_u32(token));
            dt->isUTC = reader.read_u8(token) != 0;
            dt->literalText = reader.read_string(token);
            return Value{dt};
        }

        case SerializeType::RANGE: {
            auto range = std::make_shared<RangeValue>(0, 0);
            range->start = static_cast<int>(reader.read_u32(token));
            range->end = static_cast<int>(reader.read_u32(token));
            range->step = static_cast<size_t>(reader.read_u32(token));
            range->cur = static_cast<int>(reader.read_u32(token));
            range->inclusive = reader.read_u8(token) != 0;
            range->increasing = reader.read_u8(token) != 0;
            return Value{range};
        }

        case SerializeType::ARRAY: {
            uint32_t ref_id = reader.read_u32(token);
            uint32_t length = reader.read_u32(token);

            auto arr = std::make_shared<ArrayValue>();
            arr->elements.reserve(length);

            // Add to reference map before deserializing elements (for circular refs)
            ctx.add_ref(ref_id, Value{arr});

            for (uint32_t i = 0; i < length; ++i) {
                Value elem = deserialize_value(reader, ctx, reviver, evaluator, callEnv, token);

                // Apply reviver if provided
                if (reviver && evaluator) {
                    try {
                        std::vector<Value> args = {Value{std::to_string(i)}, elem};
                        elem = evaluator->invoke_function(reviver, args, callEnv, token);
                    } catch (const std::exception& e) {
                        throw SwaziError("DeserializeError",
                            "Reviver function error at index " + std::to_string(i) + ": " + e.what(),
                            token.loc);
                    }
                }

                arr->elements.push_back(elem);
            }

            return Value{arr};
        }

        case SerializeType::OBJECT: {
            uint32_t ref_id = reader.read_u32(token);
            uint32_t prop_count = reader.read_u32(token);

            auto obj = std::make_shared<ObjectValue>();

            // Add to reference map before deserializing properties (for circular refs)
            ctx.add_ref(ref_id, Value{obj});

            for (uint32_t i = 0; i < prop_count; ++i) {
                std::string key = reader.read_string(token);
                Value prop_val = deserialize_value(reader, ctx, reviver, evaluator, callEnv, token);

                // Apply reviver if provided
                if (reviver && evaluator) {
                    try {
                        std::vector<Value> args = {Value{key}, prop_val};
                        prop_val = evaluator->invoke_function(reviver, args, callEnv, token);
                    } catch (const std::exception& e) {
                        throw SwaziError("DeserializeError",
                            "Reviver function error for key '" + key + "': " + e.what(),
                            token.loc);
                    }
                }

                PropertyDescriptor pd;
                pd.value = prop_val;
                pd.is_private = false;
                pd.is_readonly = false;
                pd.is_locked = false;
                pd.token = Token{};
                obj->properties[key] = std::move(pd);
            }

            return Value{obj};
        }

        case SerializeType::REFERENCE: {
            uint32_t ref_id = reader.read_u32(token);
            return ctx.get_ref(ref_id, token);
        }

        case SerializeType::REGEX: {
            std::string pattern = reader.read_string(token);
            std::string flags = reader.read_string(token);
            auto regex = std::make_shared<RegexValue>(pattern, flags);
            return Value{regex};
        }

        default:
            throw SwaziError("DeserializeError",
                "Unknown type tag: " + std::to_string(type_tag), token.loc);
    }
}

// Main serialize function
std::shared_ptr<ObjectValue> make_serialization_exports(EnvPtr env, Evaluator* evaluator) {
    auto obj = std::make_shared<ObjectValue>();

    // swazi.serialize(value, replacer?) -> Buffer
    {
        auto fn = [evaluator](const std::vector<Value>& args, EnvPtr callEnv, const Token& token) -> Value {
            if (args.empty()) {
                throw SwaziError("TypeError", "swazi.serialize requires a value argument", token.loc);
            }

            Value val = args[0];
            FunctionPtr replacer = nullptr;

            // Check for optional replacer function
            if (args.size() >= 2 && std::holds_alternative<FunctionPtr>(args[1])) {
                replacer = std::get<FunctionPtr>(args[1]);
            }

            ByteWriter writer;
            SerializeContext ctx;

            // Write header
            writer.write_u8(SWAZI_SERIALIZE_VERSION);

            try {
                serialize_value(val, writer, ctx, replacer, evaluator, callEnv, token);
            } catch (const SwaziError&) {
                throw;
            } catch (const std::exception& e) {
                throw SwaziError("SerializeError", e.what(), token.loc);
            }

            // Create buffer with serialized data
            auto buf = std::make_shared<BufferValue>();
            buf->data = std::move(writer.data);
            buf->encoding = "binary";

#if defined(HAVE_LIBSODIUM)
            // Add integrity check (hash of the data)
            std::vector<uint8_t> hash(crypto_hash_sha256_BYTES);
            crypto_hash_sha256(hash.data(), buf->data.data(), buf->data.size());
            buf->data.insert(buf->data.end(), hash.begin(), hash.end());
#endif

            return Value{buf};
        };

        Token tok;
        tok.type = TokenType::IDENTIFIER;
        tok.loc = TokenLocation("<serialization>", 0, 0, 0);
        auto fn_value = std::make_shared<FunctionValue>("swazi.serialize", fn, env, tok);
        obj->properties["serialize"] = PropertyDescriptor{fn_value, false, false, false, tok};
    }

    // swazi.deserialize(buffer, reviver?) -> Value
    {
        auto fn = [evaluator](const std::vector<Value>& args, EnvPtr callEnv, const Token& token) -> Value {
            if (args.empty()) {
                throw SwaziError("TypeError", "swazi.deserialize requires a Buffer argument", token.loc);
            }

            if (!std::holds_alternative<BufferPtr>(args[0])) {
                throw SwaziError("TypeError", "swazi.deserialize requires a Buffer", token.loc);
            }

            BufferPtr buf = std::get<BufferPtr>(args[0]);
            if (!buf) {
                throw SwaziError("TypeError", "Cannot deserialize null Buffer", token.loc);
            }

            FunctionPtr reviver = nullptr;

            // Check for optional reviver function
            if (args.size() >= 2 && std::holds_alternative<FunctionPtr>(args[1])) {
                reviver = std::get<FunctionPtr>(args[1]);
            }

            std::vector<uint8_t> data = buf->data;

#if defined(HAVE_LIBSODIUM)
            // Verify integrity check
            if (data.size() < crypto_hash_sha256_BYTES) {
                throw SwaziError("DeserializeError", "Buffer too small to contain valid serialized data", token.loc);
            }

            size_t data_size = data.size() - crypto_hash_sha256_BYTES;
            std::vector<uint8_t> stored_hash(data.begin() + data_size, data.end());
            data.resize(data_size);

            std::vector<uint8_t> computed_hash(crypto_hash_sha256_BYTES);
            crypto_hash_sha256(computed_hash.data(), data.data(), data.size());

            if (sodium_memcmp(stored_hash.data(), computed_hash.data(), crypto_hash_sha256_BYTES) != 0) {
                throw SwaziError("DeserializeError", "Data integrity check failed - corrupted or tampered data", token.loc);
            }
#endif

            if (data.empty()) {
                throw SwaziError("DeserializeError", "Empty buffer", token.loc);
            }

            ByteReader reader(data.data(), data.size());
            DeserializeContext ctx;

            // Read and verify version
            uint8_t version = reader.read_u8(token);
            if (version != SWAZI_SERIALIZE_VERSION) {
                throw SwaziError("DeserializeError",
                    "Unsupported serialization version: " + std::to_string(version), token.loc);
            }

            try {
                Value result = deserialize_value(reader, ctx, reviver, evaluator, callEnv, token);

                // Verify all data was consumed
                if (reader.pos != reader.size) {
                    throw SwaziError("DeserializeError",
                        "Unexpected data at end of buffer (pos=" + std::to_string(reader.pos) +
                            ", size=" + std::to_string(reader.size) + ")",
                        token.loc);
                }

                return result;
            } catch (const SwaziError&) {
                throw;
            } catch (const std::exception& e) {
                throw SwaziError("DeserializeError", e.what(), token.loc);
            }
        };

        Token tok;
        tok.type = TokenType::IDENTIFIER;
        tok.loc = TokenLocation("<serialization>", 0, 0, 0);
        auto fn_value = std::make_shared<FunctionValue>("swazi.deserialize", fn, env, tok);
        obj->properties["deserialize"] = PropertyDescriptor{fn_value, false, false, false, tok};
    }

    // swazi.serialize.version -> number (current serialization format version)
    {
        obj->properties["version"] = PropertyDescriptor{
            Value{static_cast<double>(SWAZI_SERIALIZE_VERSION)},
            false, false, true, Token{}};
    }

    // swazi.serialize.clone(value) -> cloned value (deep clone using serialize/deserialize)
    {
        auto fn = [evaluator](const std::vector<Value>& args, EnvPtr callEnv, const Token& token) -> Value {
            if (args.empty()) {
                throw SwaziError("TypeError", "swazi.serialize.clone requires a value argument", token.loc);
            }

            // Serialize then immediately deserialize for a deep clone
            ByteWriter writer;
            SerializeContext ctx;
            writer.write_u8(SWAZI_SERIALIZE_VERSION);

            try {
                serialize_value(args[0], writer, ctx, nullptr, nullptr, callEnv, token);
            } catch (const std::exception& e) {
                throw SwaziError("SerializeError", e.what(), token.loc);
            }

            ByteReader reader(writer.data.data(), writer.data.size());
            DeserializeContext dctx;

            reader.read_u8(token);  // skip version

            try {
                return deserialize_value(reader, dctx, nullptr, nullptr, callEnv, token);
            } catch (const std::exception& e) {
                throw SwaziError("DeserializeError", e.what(), token.loc);
            }
        };

        Token tok;
        tok.type = TokenType::IDENTIFIER;
        tok.loc = TokenLocation("<serialization>", 0, 0, 0);
        auto fn_value = std::make_shared<FunctionValue>("swazi.serialize.clone", fn, env, tok);
        obj->properties["clone"] = PropertyDescriptor{fn_value, false, false, false, tok};
    }

    // swazi.serialize.equals(a, b) -> bool (structural equality check)
    {
        auto fn = [evaluator](const std::vector<Value>& args, EnvPtr callEnv, const Token& token) -> Value {
            if (args.size() < 2) {
                throw SwaziError("TypeError", "swazi.serialize.equals requires two arguments", token.loc);
            }

            // Serialize both values
            ByteWriter writer1, writer2;
            SerializeContext ctx1, ctx2;

            writer1.write_u8(SWAZI_SERIALIZE_VERSION);
            writer2.write_u8(SWAZI_SERIALIZE_VERSION);

            try {
                serialize_value(args[0], writer1, ctx1, nullptr, nullptr, callEnv, token);
                serialize_value(args[1], writer2, ctx2, nullptr, nullptr, callEnv, token);
            } catch (const std::exception&) {
                // If either fails to serialize, they're not equal
                return Value{false};
            }

            // Compare serialized bytes
            if (writer1.data.size() != writer2.data.size()) {
                return Value{false};
            }

            for (size_t i = 0; i < writer1.data.size(); ++i) {
                if (writer1.data[i] != writer2.data[i]) {
                    return Value{false};
                }
            }

            return Value{true};
        };

        Token tok;
        tok.type = TokenType::IDENTIFIER;
        tok.loc = TokenLocation("<serialization>", 0, 0, 0);
        auto fn_value = std::make_shared<FunctionValue>("swazi.serialize.equals", fn, env, tok);
        obj->properties["equals"] = PropertyDescriptor{fn_value, false, false, false, tok};
    }

    return obj;
}