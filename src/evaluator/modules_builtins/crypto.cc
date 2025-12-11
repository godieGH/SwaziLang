#include "SwaziError.hpp"
#include "builtins.hpp"
#include "evaluator.hpp"

#if defined(HAVE_LIBSODIUM)
#include <sodium.h>
#else
#error "crypto module requires libsodium"
#endif

#include <algorithm>
#include <cstring>

// ============= STATEFUL CRYPTO STATE HOLDERS =============

struct HashState {
    std::string algorithm;
    crypto_hash_sha256_state sha256_state;
    crypto_hash_sha512_state sha512_state;
    crypto_generichash_state blake2b_state;
    std::vector<uint8_t> key;
    bool finalized = false;
};

struct HmacState {
    std::string algorithm;
    crypto_auth_hmacsha256_state sha256_state;
    crypto_auth_hmacsha512_state sha512_state;
    bool finalized = false;
};

struct SecretBoxEncryptState {
    std::vector<uint8_t> key;
    std::vector<uint8_t> nonce;
    crypto_secretstream_xchacha20poly1305_state state;
    bool initialized = false;
    bool finalized = false;
};

struct SecretBoxDecryptState {
    std::vector<uint8_t> key;
    std::vector<uint8_t> nonce;
    crypto_secretstream_xchacha20poly1305_state state;
    bool initialized = false;
    bool finalized = false;
};

struct SignState {
    std::string algorithm;  // empty string means raw mode
    std::vector<uint8_t> secret_key;
    crypto_hash_sha512_state sha512_state;
    crypto_hash_sha256_state sha256_state;
    std::vector<uint8_t> accumulated_data;
    bool finalized = false;
};
// Helper: convert Value to string
static std::string value_to_string_simple_crypto(const Value& v) {
    if (std::holds_alternative<std::string>(v)) return std::get<std::string>(v);
    if (std::holds_alternative<double>(v)) {
        std::ostringstream ss;
        ss << std::get<double>(v);
        return ss.str();
    }
    if (std::holds_alternative<bool>(v)) return std::get<bool>(v) ? "true" : "false";
    return std::string();
}

// Helper: get algorithm constant from string
static int get_hash_algorithm(const std::string& algo, const Token& token) {
    if (algo == "sha256") return 0;  // we'll use crypto_hash_sha256
    if (algo == "sha512") return 1;  // crypto_hash_sha512
    throw SwaziError("CryptoError", "Unknown hash algorithm: " + algo + ". Supported: sha256, sha512", token.loc);
}

std::shared_ptr<ObjectValue> make_crypto_exports(EnvPtr env) {
    // Initialize libsodium (idempotent, safe to call multiple times)
    if (sodium_init() < 0) {
        throw std::runtime_error("Failed to initialize libsodium");
    }

    auto obj = std::make_shared<ObjectValue>();

    // ============= CONSTANTS =============

    // Export key sizes as constants
    obj->properties["HASH_SHA256_BYTES"] = PropertyDescriptor{
        Value{static_cast<double>(crypto_hash_sha256_BYTES)},
        false, false, true, Token()};
    obj->properties["HASH_SHA512_BYTES"] = PropertyDescriptor{
        Value{static_cast<double>(crypto_hash_sha512_BYTES)},
        false, false, true, Token()};
    obj->properties["HASH_BLAKE2B_BYTES"] = PropertyDescriptor{
        Value{static_cast<double>(crypto_generichash_BYTES)},
        false, false, true, Token()};
    obj->properties["SECRETBOX_KEYBYTES"] = PropertyDescriptor{
        Value{static_cast<double>(crypto_secretbox_KEYBYTES)},
        false, false, true, Token()};
    obj->properties["SECRETBOX_NONCEBYTES"] = PropertyDescriptor{
        Value{static_cast<double>(crypto_secretbox_NONCEBYTES)},
        false, false, true, Token()};
    obj->properties["SECRETBOX_MACBYTES"] = PropertyDescriptor{
        Value{static_cast<double>(crypto_secretbox_MACBYTES)},
        false, false, true, Token()};
    obj->properties["BOX_PUBLICKEYBYTES"] = PropertyDescriptor{
        Value{static_cast<double>(crypto_box_PUBLICKEYBYTES)},
        false, false, true, Token()};
    obj->properties["BOX_SECRETKEYBYTES"] = PropertyDescriptor{
        Value{static_cast<double>(crypto_box_SECRETKEYBYTES)},
        false, false, true, Token()};
    obj->properties["BOX_NONCEBYTES"] = PropertyDescriptor{
        Value{static_cast<double>(crypto_box_NONCEBYTES)},
        false, false, true, Token()};
    obj->properties["SIGN_PUBLICKEYBYTES"] = PropertyDescriptor{
        Value{static_cast<double>(crypto_sign_PUBLICKEYBYTES)},
        false, false, true, Token()};
    obj->properties["SIGN_SECRETKEYBYTES"] = PropertyDescriptor{
        Value{static_cast<double>(crypto_sign_SECRETKEYBYTES)},
        false, false, true, Token()};
    obj->properties["SIGN_BYTES"] = PropertyDescriptor{
        Value{static_cast<double>(crypto_sign_BYTES)},
        false, false, true, Token()};

    // ============= HASHING =============

    // crypto.hash(algorithm, data) -> Buffer
    {
        auto fn = [](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
            if (args.size() < 2) {
                throw SwaziError("TypeError", "crypto.hash requires (algorithm, data)", token.loc);
            }

            std::string algo = value_to_string_simple_crypto(args[0]);

            // Convert data to bytes
            std::vector<uint8_t> data;
            if (std::holds_alternative<BufferPtr>(args[1])) {
                data = std::get<BufferPtr>(args[1])->data;
            } else if (std::holds_alternative<std::string>(args[1])) {
                std::string str = std::get<std::string>(args[1]);
                data.assign(str.begin(), str.end());
            } else {
                throw SwaziError("TypeError", "data must be Buffer or string", token.loc);
            }

            auto result = std::make_shared<BufferValue>();

            if (algo == "sha256") {
                result->data.resize(crypto_hash_sha256_BYTES);
                crypto_hash_sha256(result->data.data(), data.data(), data.size());
            } else if (algo == "sha512") {
                result->data.resize(crypto_hash_sha512_BYTES);
                crypto_hash_sha512(result->data.data(), data.data(), data.size());
            } else if (algo == "blake2b") {
                result->data.resize(crypto_generichash_BYTES);  // 32 bytes
                crypto_generichash(result->data.data(), crypto_generichash_BYTES,
                    data.data(), data.size(),
                    nullptr, 0);  // no key
            } else {
                throw SwaziError("CryptoError",
                    "Unknown algorithm: " + algo + ". Supported: sha256, sha512 and blake2b only", token.loc);
            }

            result->encoding = "binary";
            return Value{result};
        };

        Token tok;
        tok.type = TokenType::IDENTIFIER;
        tok.loc = TokenLocation("<crypto>", 0, 0, 0);
        auto fn_value = std::make_shared<FunctionValue>("crypto.hash", fn, env, tok);
        obj->properties["hash"] = PropertyDescriptor{fn_value, false, false, false, tok};
    }

    // crypto.createHash(algorithm, optionalKey) -> HashState object
    {
        auto fn = [env](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
            if (args.empty()) {
                throw SwaziError("TypeError", "crypto.createHash requires algorithm", token.loc);
            }

            std::string algo = value_to_string_simple_crypto(args[0]);

            auto state = std::make_shared<HashState>();
            state->algorithm = algo;

            // Check for optional key (second argument)
            std::vector<uint8_t> key;
            if (args.size() >= 2) {
                if (std::holds_alternative<BufferPtr>(args[1])) {
                    key = std::get<BufferPtr>(args[1])->data;
                } else if (std::holds_alternative<std::string>(args[1])) {
                    std::string str = std::get<std::string>(args[1]);
                    key.assign(str.begin(), str.end());
                } else if (!std::holds_alternative<std::monostate>(args[1])) {
                    throw SwaziError("TypeError", "key must be Buffer or string", token.loc);
                }
            }

            if (algo == "sha256") {
                if (!key.empty()) {
                    throw SwaziError("CryptoError", "sha256 does not support keyed hashing", token.loc);
                }
                crypto_hash_sha256_init(&state->sha256_state);
            } else if (algo == "sha512") {
                if (!key.empty()) {
                    throw SwaziError("CryptoError", "sha512 does not support keyed hashing", token.loc);
                }
                crypto_hash_sha512_init(&state->sha512_state);
            } else if (algo == "blake2b") {
                state->key = key;  // Store key for streaming operations
                crypto_generichash_init(&state->blake2b_state,
                    key.empty() ? nullptr : key.data(),
                    key.empty() ? 0 : key.size(),
                    crypto_generichash_BYTES);
            } else {
                throw SwaziError("CryptoError",
                    "Unknown algorithm: " + algo + ". Supported: sha256, sha512, blake2b", token.loc);
            }

            // Create object with methods
            auto obj = std::make_shared<ObjectValue>();

            // update(data) method
            auto update_fn = [state](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
                if (state->finalized) {
                    throw SwaziError("CryptoError", "Hash already finalized", token.loc);
                }
                if (args.empty()) {
                    throw SwaziError("TypeError", "update requires data argument", token.loc);
                }

                std::vector<uint8_t> data;
                if (std::holds_alternative<BufferPtr>(args[0])) {
                    data = std::get<BufferPtr>(args[0])->data;
                } else if (std::holds_alternative<std::string>(args[0])) {
                    std::string str = std::get<std::string>(args[0]);
                    data.assign(str.begin(), str.end());
                } else {
                    throw SwaziError("TypeError", "data must be Buffer or string", token.loc);
                }

                if (state->algorithm == "sha256") {
                    crypto_hash_sha256_update(&state->sha256_state, data.data(), data.size());
                } else if (state->algorithm == "sha512") {
                    crypto_hash_sha512_update(&state->sha512_state, data.data(), data.size());
                } else if (state->algorithm == "blake2b") {
                    crypto_generichash_update(&state->blake2b_state, data.data(), data.size());
                }

                return std::monostate{};
            };

            // finalize() method
            auto finalize_fn = [state](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
                if (state->finalized) {
                    throw SwaziError("CryptoError", "Hash already finalized", token.loc);
                }

                auto result = std::make_shared<BufferValue>();

                if (state->algorithm == "sha256") {
                    result->data.resize(crypto_hash_sha256_BYTES);
                    crypto_hash_sha256_final(&state->sha256_state, result->data.data());
                } else if (state->algorithm == "sha512") {
                    result->data.resize(crypto_hash_sha512_BYTES);
                    crypto_hash_sha512_final(&state->sha512_state, result->data.data());
                } else if (state->algorithm == "blake2b") {
                    result->data.resize(crypto_generichash_BYTES);
                    crypto_generichash_final(&state->blake2b_state, result->data.data(),
                        crypto_generichash_BYTES);
                }

                state->finalized = true;
                result->encoding = "binary";
                return Value{result};
            };

            Token tok;
            tok.type = TokenType::IDENTIFIER;
            tok.loc = TokenLocation("<crypto>", 0, 0, 0);

            obj->properties["update"] = PropertyDescriptor{
                std::make_shared<FunctionValue>("update", update_fn, env, tok),
                false, false, false, tok};
            obj->properties["finalize"] = PropertyDescriptor{
                std::make_shared<FunctionValue>("finalize", finalize_fn, env, tok),
                false, false, false, tok};

            return Value{obj};
        };

        Token tok;
        tok.type = TokenType::IDENTIFIER;
        tok.loc = TokenLocation("<crypto>", 0, 0, 0);
        auto fn_value = std::make_shared<FunctionValue>("crypto.createHash", fn, env, tok);
        obj->properties["createHash"] = PropertyDescriptor{fn_value, false, false, false, tok};
    }
    // ============= HMAC =============

    // crypto.hmac(algorithm, key, data) -> Buffer
    {
        auto fn = [](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
            if (args.size() < 3) {
                throw SwaziError("TypeError", "crypto.hmac requires (algorithm, key, data)", token.loc);
            }

            std::string algo = value_to_string_simple_crypto(args[0]);

            // Get key
            std::vector<uint8_t> key;
            if (std::holds_alternative<BufferPtr>(args[1])) {
                key = std::get<BufferPtr>(args[1])->data;
            } else if (std::holds_alternative<std::string>(args[1])) {
                std::string str = std::get<std::string>(args[1]);
                key.assign(str.begin(), str.end());
            } else {
                throw SwaziError("TypeError", "key must be Buffer or string", token.loc);
            }

            // Get data
            std::vector<uint8_t> data;
            if (std::holds_alternative<BufferPtr>(args[2])) {
                data = std::get<BufferPtr>(args[2])->data;
            } else if (std::holds_alternative<std::string>(args[2])) {
                std::string str = std::get<std::string>(args[2]);
                data.assign(str.begin(), str.end());
            } else {
                throw SwaziError("TypeError", "data must be Buffer or string", token.loc);
            }

            auto result = std::make_shared<BufferValue>();

            if (algo == "sha256") {
                result->data.resize(crypto_auth_hmacsha256_BYTES);
                crypto_auth_hmacsha256(result->data.data(), data.data(), data.size(), key.data());
            } else if (algo == "sha512") {
                result->data.resize(crypto_auth_hmacsha512_BYTES);
                crypto_auth_hmacsha512(result->data.data(), data.data(), data.size(), key.data());
            } else {
                throw SwaziError("CryptoError",
                    "Unknown algorithm: " + algo + ". Supported: sha256, sha512", token.loc);
            }

            result->encoding = "binary";
            return Value{result};
        };

        Token tok;
        tok.type = TokenType::IDENTIFIER;
        tok.loc = TokenLocation("<crypto>", 0, 0, 0);
        auto fn_value = std::make_shared<FunctionValue>("crypto.hmac", fn, env, tok);
        obj->properties["hmac"] = PropertyDescriptor{fn_value, false, false, false, tok};
    }

    // crypto.createHmac(algorithm, key) -> HmacState object
    {
        auto fn = [env](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
            if (args.size() < 2) {
                throw SwaziError("TypeError", "crypto.createHmac requires (algorithm, key)", token.loc);
            }

            std::string algo = value_to_string_simple_crypto(args[0]);

            std::vector<uint8_t> key;
            if (std::holds_alternative<BufferPtr>(args[1])) {
                key = std::get<BufferPtr>(args[1])->data;
            } else if (std::holds_alternative<std::string>(args[1])) {
                std::string str = std::get<std::string>(args[1]);
                key.assign(str.begin(), str.end());
            } else {
                throw SwaziError("TypeError", "key must be Buffer or string", token.loc);
            }

            auto state = std::make_shared<HmacState>();
            state->algorithm = algo;

            if (algo == "sha256") {
                crypto_auth_hmacsha256_init(&state->sha256_state, key.data(), key.size());
            } else if (algo == "sha512") {
                crypto_auth_hmacsha512_init(&state->sha512_state, key.data(), key.size());
            } else {
                throw SwaziError("CryptoError",
                    "Unknown algorithm: " + algo + ". Supported: sha256, sha512", token.loc);
            }

            auto obj = std::make_shared<ObjectValue>();

            // update(data) method
            auto update_fn = [state](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
                if (state->finalized) {
                    throw SwaziError("CryptoError", "HMAC already finalized", token.loc);
                }
                if (args.empty()) {
                    throw SwaziError("TypeError", "update requires data argument", token.loc);
                }

                std::vector<uint8_t> data;
                if (std::holds_alternative<BufferPtr>(args[0])) {
                    data = std::get<BufferPtr>(args[0])->data;
                } else if (std::holds_alternative<std::string>(args[0])) {
                    std::string str = std::get<std::string>(args[0]);
                    data.assign(str.begin(), str.end());
                } else {
                    throw SwaziError("TypeError", "data must be Buffer or string", token.loc);
                }

                if (state->algorithm == "sha256") {
                    crypto_auth_hmacsha256_update(&state->sha256_state, data.data(), data.size());
                } else {
                    crypto_auth_hmacsha512_update(&state->sha512_state, data.data(), data.size());
                }

                return std::monostate{};
            };

            // finalize() method
            auto finalize_fn = [state](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
                if (state->finalized) {
                    throw SwaziError("CryptoError", "HMAC already finalized", token.loc);
                }

                auto result = std::make_shared<BufferValue>();

                if (state->algorithm == "sha256") {
                    result->data.resize(crypto_auth_hmacsha256_BYTES);
                    crypto_auth_hmacsha256_final(&state->sha256_state, result->data.data());
                } else {
                    result->data.resize(crypto_auth_hmacsha512_BYTES);
                    crypto_auth_hmacsha512_final(&state->sha512_state, result->data.data());
                }

                state->finalized = true;
                result->encoding = "binary";
                return Value{result};
            };

            Token tok;
            tok.type = TokenType::IDENTIFIER;
            tok.loc = TokenLocation("<crypto>", 0, 0, 0);

            obj->properties["update"] = PropertyDescriptor{
                std::make_shared<FunctionValue>("update", update_fn, env, tok),
                false, false, false, tok};
            obj->properties["finalize"] = PropertyDescriptor{
                std::make_shared<FunctionValue>("finalize", finalize_fn, env, tok),
                false, false, false, tok};

            return Value{obj};
        };

        Token tok;
        tok.type = TokenType::IDENTIFIER;
        tok.loc = TokenLocation("<crypto>", 0, 0, 0);
        auto fn_value = std::make_shared<FunctionValue>("crypto.createHmac", fn, env, tok);
        obj->properties["createHmac"] = PropertyDescriptor{fn_value, false, false, false, tok};
    }

    // ============= RANDOM =============

    // crypto.randomBytes(length) -> Buffer
    {
        auto fn = [](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
            if (args.empty() || !std::holds_alternative<double>(args[0])) {
                throw SwaziError("TypeError", "crypto.randomBytes requires length", token.loc);
            }

            size_t len = static_cast<size_t>(std::get<double>(args[0]));
            if (len > 1024 * 1024) {
                throw SwaziError("RangeError", "randomBytes length too large (max 1MB)", token.loc);
            }

            auto result = std::make_shared<BufferValue>();
            result->data.resize(len);
            randombytes_buf(result->data.data(), len);
            result->encoding = "binary";

            return Value{result};
        };

        Token tok;
        tok.type = TokenType::IDENTIFIER;
        tok.loc = TokenLocation("<crypto>", 0, 0, 0);
        auto fn_value = std::make_shared<FunctionValue>("crypto.randomBytes", fn, env, tok);
        obj->properties["randomBytes"] = PropertyDescriptor{fn_value, false, false, false, tok};
    }

    // crypto.randomInt(min, max) -> number
    {
        auto fn = [](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
            if (args.size() < 2 || !std::holds_alternative<double>(args[0]) ||
                !std::holds_alternative<double>(args[1])) {
                throw SwaziError("TypeError", "crypto.randomInt requires (min, max)", token.loc);
            }

            int64_t min = static_cast<int64_t>(std::get<double>(args[0]));
            int64_t max = static_cast<int64_t>(std::get<double>(args[1]));

            if (min >= max) {
                throw SwaziError("RangeError", "min must be less than max", token.loc);
            }

            uint64_t range = static_cast<uint64_t>(max - min);
            uint64_t random_val = randombytes_uniform(static_cast<uint32_t>(range));

            return Value{static_cast<double>(min + random_val)};
        };

        Token tok;
        tok.type = TokenType::IDENTIFIER;
        tok.loc = TokenLocation("<crypto>", 0, 0, 0);
        auto fn_value = std::make_shared<FunctionValue>("crypto.randomInt", fn, env, tok);
        obj->properties["randomInt"] = PropertyDescriptor{fn_value, false, false, false, tok};
    }

    // ============= SYMMETRIC ENCRYPTION (Secret Box - XSalsa20-Poly1305) =============

    // crypto.secretbox.encrypt(key, nonce, data) -> Buffer (includes MAC)
    {
        auto fn = [](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
            if (args.size() < 3) {
                throw SwaziError("TypeError",
                    "crypto.secretbox.encrypt requires (key, nonce, data)", token.loc);
            }

            // Get key (must be exactly 32 bytes)
            BufferPtr key_buf;
            if (std::holds_alternative<BufferPtr>(args[0])) {
                key_buf = std::get<BufferPtr>(args[0]);
            } else {
                throw SwaziError("TypeError", "key must be Buffer", token.loc);
            }

            if (key_buf->data.size() != crypto_secretbox_KEYBYTES) {
                throw SwaziError("CryptoError",
                    "key must be exactly " + std::to_string(crypto_secretbox_KEYBYTES) + " bytes",
                    token.loc);
            }

            // Get nonce (must be exactly 24 bytes)
            BufferPtr nonce_buf;
            if (std::holds_alternative<BufferPtr>(args[1])) {
                nonce_buf = std::get<BufferPtr>(args[1]);
            } else {
                throw SwaziError("TypeError", "nonce must be Buffer", token.loc);
            }

            if (nonce_buf->data.size() != crypto_secretbox_NONCEBYTES) {
                throw SwaziError("CryptoError",
                    "nonce must be exactly " + std::to_string(crypto_secretbox_NONCEBYTES) + " bytes",
                    token.loc);
            }

            // Get plaintext
            std::vector<uint8_t> plaintext;
            if (std::holds_alternative<BufferPtr>(args[2])) {
                plaintext = std::get<BufferPtr>(args[2])->data;
            } else if (std::holds_alternative<std::string>(args[2])) {
                std::string str = std::get<std::string>(args[2]);
                plaintext.assign(str.begin(), str.end());
            } else {
                throw SwaziError("TypeError", "data must be Buffer or string", token.loc);
            }

            // Encrypt
            auto result = std::make_shared<BufferValue>();
            result->data.resize(crypto_secretbox_MACBYTES + plaintext.size());

            int ret = crypto_secretbox_easy(
                result->data.data(),
                plaintext.data(),
                plaintext.size(),
                nonce_buf->data.data(),
                key_buf->data.data());

            if (ret != 0) {
                throw SwaziError("CryptoError", "Encryption failed", token.loc);
            }

            result->encoding = "binary";
            return Value{result};
        };

        Token tok;
        tok.type = TokenType::IDENTIFIER;
        tok.loc = TokenLocation("<crypto>", 0, 0, 0);
        auto fn_value = std::make_shared<FunctionValue>("crypto.secretbox.encrypt", fn, env, tok);

        auto secretbox_obj = std::make_shared<ObjectValue>();
        secretbox_obj->properties["encrypt"] = PropertyDescriptor{fn_value, false, false, false, tok};

        // crypto.secretbox.decrypt(key, nonce, ciphertext) -> Buffer
        auto fn_decrypt = [](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
            if (args.size() < 3) {
                throw SwaziError("TypeError",
                    "crypto.secretbox.decrypt requires (key, nonce, ciphertext)", token.loc);
            }

            // Get key
            BufferPtr key_buf;
            if (std::holds_alternative<BufferPtr>(args[0])) {
                key_buf = std::get<BufferPtr>(args[0]);
            } else {
                throw SwaziError("TypeError", "key must be Buffer", token.loc);
            }

            if (key_buf->data.size() != crypto_secretbox_KEYBYTES) {
                throw SwaziError("CryptoError",
                    "key must be exactly " + std::to_string(crypto_secretbox_KEYBYTES) + " bytes",
                    token.loc);
            }

            // Get nonce
            BufferPtr nonce_buf;
            if (std::holds_alternative<BufferPtr>(args[1])) {
                nonce_buf = std::get<BufferPtr>(args[1]);
            } else {
                throw SwaziError("TypeError", "nonce must be Buffer", token.loc);
            }

            if (nonce_buf->data.size() != crypto_secretbox_NONCEBYTES) {
                throw SwaziError("CryptoError",
                    "nonce must be exactly " + std::to_string(crypto_secretbox_NONCEBYTES) + " bytes",
                    token.loc);
            }

            // Get ciphertext
            BufferPtr cipher_buf;
            if (std::holds_alternative<BufferPtr>(args[2])) {
                cipher_buf = std::get<BufferPtr>(args[2]);
            } else {
                throw SwaziError("TypeError", "ciphertext must be Buffer", token.loc);
            }

            if (cipher_buf->data.size() < crypto_secretbox_MACBYTES) {
                throw SwaziError("CryptoError", "ciphertext too short", token.loc);
            }

            // Decrypt
            auto result = std::make_shared<BufferValue>();
            result->data.resize(cipher_buf->data.size() - crypto_secretbox_MACBYTES);

            int ret = crypto_secretbox_open_easy(
                result->data.data(),
                cipher_buf->data.data(),
                cipher_buf->data.size(),
                nonce_buf->data.data(),
                key_buf->data.data());

            if (ret != 0) {
                throw SwaziError("CryptoError", "Decryption failed (authentication error)", token.loc);
            }

            result->encoding = "binary";
            return Value{result};
        };

        auto fn_decrypt_value = std::make_shared<FunctionValue>("crypto.secretbox.decrypt",
            fn_decrypt, env, tok);
        secretbox_obj->properties["decrypt"] = PropertyDescriptor{fn_decrypt_value, false, false, false, tok};

        // crypto.secretbox.createEncryptor(key) -> Encryptor object
        auto fn_create_encryptor = [env](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
            if (args.empty()) {
                throw SwaziError("TypeError", "createEncryptor requires key", token.loc);
            }

            BufferPtr key_buf;
            if (std::holds_alternative<BufferPtr>(args[0])) {
                key_buf = std::get<BufferPtr>(args[0]);
            } else {
                throw SwaziError("TypeError", "key must be Buffer", token.loc);
            }

            if (key_buf->data.size() != crypto_secretstream_xchacha20poly1305_KEYBYTES) {
                throw SwaziError("CryptoError",
                    "key must be exactly " + std::to_string(crypto_secretstream_xchacha20poly1305_KEYBYTES) + " bytes",
                    token.loc);
            }

            auto state = std::make_shared<SecretBoxEncryptState>();
            state->key = key_buf->data;

            auto obj = std::make_shared<ObjectValue>();

            // init() -> header (must be called first, returns header to prepend to ciphertext)
            auto init_fn = [state](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
                if (state->initialized) {
                    throw SwaziError("CryptoError", "Encryptor already initialized", token.loc);
                }

                auto header = std::make_shared<BufferValue>();
                header->data.resize(crypto_secretstream_xchacha20poly1305_HEADERBYTES);

                crypto_secretstream_xchacha20poly1305_init_push(
                    &state->state,
                    header->data.data(),
                    state->key.data());

                state->initialized = true;
                header->encoding = "binary";
                return Value{header};
            };

            // update(chunk) -> encrypted chunk
            auto update_fn = [state](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
                if (!state->initialized) {
                    throw SwaziError("CryptoError", "Encryptor not initialized (call init() first)", token.loc);
                }
                if (state->finalized) {
                    throw SwaziError("CryptoError", "Encryptor already finalized", token.loc);
                }
                if (args.empty()) {
                    throw SwaziError("TypeError", "update requires data argument", token.loc);
                }

                std::vector<uint8_t> plaintext;
                if (std::holds_alternative<BufferPtr>(args[0])) {
                    plaintext = std::get<BufferPtr>(args[0])->data;
                } else if (std::holds_alternative<std::string>(args[0])) {
                    std::string str = std::get<std::string>(args[0]);
                    plaintext.assign(str.begin(), str.end());
                } else {
                    throw SwaziError("TypeError", "data must be Buffer or string", token.loc);
                }

                auto result = std::make_shared<BufferValue>();
                result->data.resize(plaintext.size() + crypto_secretstream_xchacha20poly1305_ABYTES);

                unsigned long long ciphertext_len;
                crypto_secretstream_xchacha20poly1305_push(
                    &state->state,
                    result->data.data(),
                    &ciphertext_len,
                    plaintext.data(),
                    plaintext.size(),
                    nullptr, 0,
                    0);  // tag = 0 (message continues)

                result->data.resize(ciphertext_len);
                result->encoding = "binary";
                return Value{result};
            };

            // finalize() -> final encrypted chunk with tag
            auto finalize_fn = [state](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
                if (!state->initialized) {
                    throw SwaziError("CryptoError", "Encryptor not initialized", token.loc);
                }
                if (state->finalized) {
                    throw SwaziError("CryptoError", "Encryptor already finalized", token.loc);
                }

                auto result = std::make_shared<BufferValue>();
                result->data.resize(crypto_secretstream_xchacha20poly1305_ABYTES);

                unsigned long long ciphertext_len;
                crypto_secretstream_xchacha20poly1305_push(
                    &state->state,
                    result->data.data(),
                    &ciphertext_len,
                    nullptr, 0,
                    nullptr, 0,
                    crypto_secretstream_xchacha20poly1305_TAG_FINAL);

                result->data.resize(ciphertext_len);
                state->finalized = true;
                result->encoding = "binary";
                return Value{result};
            };

            Token tok;
            tok.type = TokenType::IDENTIFIER;
            tok.loc = TokenLocation("<crypto>", 0, 0, 0);

            obj->properties["init"] = PropertyDescriptor{
                std::make_shared<FunctionValue>("init", init_fn, env, tok),
                false, false, false, tok};
            obj->properties["update"] = PropertyDescriptor{
                std::make_shared<FunctionValue>("update", update_fn, env, tok),
                false, false, false, tok};
            obj->properties["finalize"] = PropertyDescriptor{
                std::make_shared<FunctionValue>("finalize", finalize_fn, env, tok),
                false, false, false, tok};

            return Value{obj};
        };
        auto fn_create_encryptor_value = std::make_shared<FunctionValue>(
            "crypto.secretbox.createEncryptor", fn_create_encryptor, env, tok);
        secretbox_obj->properties["createEncryptor"] = PropertyDescriptor{
            fn_create_encryptor_value, false, false, false, tok};

        // crypto.secretbox.createDecryptor(key, header) -> Decryptor object
        auto fn_create_decryptor = [env](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
            if (args.size() < 2) {
                throw SwaziError("TypeError", "createDecryptor requires (key, header)", token.loc);
            }

            BufferPtr key_buf;
            if (std::holds_alternative<BufferPtr>(args[0])) {
                key_buf = std::get<BufferPtr>(args[0]);
            } else {
                throw SwaziError("TypeError", "key must be Buffer", token.loc);
            }

            if (key_buf->data.size() != crypto_secretstream_xchacha20poly1305_KEYBYTES) {
                throw SwaziError("CryptoError",
                    "key must be exactly " + std::to_string(crypto_secretstream_xchacha20poly1305_KEYBYTES) + " bytes",
                    token.loc);
            }

            BufferPtr header_buf;
            if (std::holds_alternative<BufferPtr>(args[1])) {
                header_buf = std::get<BufferPtr>(args[1]);
            } else {
                throw SwaziError("TypeError", "header must be Buffer", token.loc);
            }

            if (header_buf->data.size() != crypto_secretstream_xchacha20poly1305_HEADERBYTES) {
                throw SwaziError("CryptoError",
                    "header must be exactly " + std::to_string(crypto_secretstream_xchacha20poly1305_HEADERBYTES) + " bytes",
                    token.loc);
            }

            auto state = std::make_shared<SecretBoxDecryptState>();
            state->key = key_buf->data;

            if (crypto_secretstream_xchacha20poly1305_init_pull(
                    &state->state,
                    header_buf->data.data(),
                    state->key.data()) != 0) {
                throw SwaziError("CryptoError", "Invalid header", token.loc);
            }

            state->initialized = true;

            auto obj = std::make_shared<ObjectValue>();

            // update(chunk) -> decrypted chunk
            auto update_fn = [state](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
                if (state->finalized) {
                    throw SwaziError("CryptoError", "Decryptor already finalized", token.loc);
                }
                if (args.empty()) {
                    throw SwaziError("TypeError", "update requires data argument", token.loc);
                }

                BufferPtr cipher_buf;
                if (std::holds_alternative<BufferPtr>(args[0])) {
                    cipher_buf = std::get<BufferPtr>(args[0]);
                } else {
                    throw SwaziError("TypeError", "data must be Buffer", token.loc);
                }

                auto result = std::make_shared<BufferValue>();
                result->data.resize(cipher_buf->data.size());

                unsigned long long plaintext_len;
                unsigned char tag;

                if (crypto_secretstream_xchacha20poly1305_pull(
                        &state->state,
                        result->data.data(),
                        &plaintext_len,
                        &tag,
                        cipher_buf->data.data(),
                        cipher_buf->data.size(),
                        nullptr, 0) != 0) {
                    throw SwaziError("CryptoError", "Decryption failed (authentication error)", token.loc);
                }

                result->data.resize(plaintext_len);

                if (tag == crypto_secretstream_xchacha20poly1305_TAG_FINAL) {
                    state->finalized = true;
                }

                result->encoding = "binary";
                return Value{result};
            };

            Token tok;
            tok.type = TokenType::IDENTIFIER;
            tok.loc = TokenLocation("<crypto>", 0, 0, 0);

            obj->properties["update"] = PropertyDescriptor{
                std::make_shared<FunctionValue>("update", update_fn, env, tok),
                false, false, false, tok};

            return Value{obj};
        };

        auto fn_create_decryptor_value = std::make_shared<FunctionValue>(
            "crypto.secretbox.createDecryptor", fn_create_decryptor, env, tok);
        secretbox_obj->properties["createDecryptor"] = PropertyDescriptor{
            fn_create_decryptor_value, false, false, false, tok};
        obj->properties["secretbox"] = PropertyDescriptor{Value{secretbox_obj}, false, false, true, tok};
    }

    // ============= ASYMMETRIC ENCRYPTION (Box - X25519-XSalsa20-Poly1305) =============

    // crypto.box.keypair() -> { publicKey: Buffer, secretKey: Buffer }
    {
        auto fn = [](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
            auto result = std::make_shared<ObjectValue>();

            auto pk = std::make_shared<BufferValue>();
            pk->data.resize(crypto_box_PUBLICKEYBYTES);

            auto sk = std::make_shared<BufferValue>();
            sk->data.resize(crypto_box_SECRETKEYBYTES);

            crypto_box_keypair(pk->data.data(), sk->data.data());

            pk->encoding = "binary";
            sk->encoding = "binary";

            result->properties["publicKey"] = PropertyDescriptor{Value{pk}, false, false, true, Token()};
            result->properties["secretKey"] = PropertyDescriptor{Value{sk}, false, false, true, Token()};

            return Value{result};
        };

        Token tok;
        tok.type = TokenType::IDENTIFIER;
        tok.loc = TokenLocation("<crypto>", 0, 0, 0);
        auto fn_value = std::make_shared<FunctionValue>("crypto.box.keypair", fn, env, tok);

        auto box_obj = std::make_shared<ObjectValue>();
        box_obj->properties["keypair"] = PropertyDescriptor{fn_value, false, false, false, tok};

        // crypto.box.encrypt(theirPublicKey, mySecretKey, nonce, data) -> Buffer
        auto fn_encrypt = [](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
            if (args.size() < 4) {
                throw SwaziError("TypeError",
                    "crypto.box.encrypt requires (theirPublicKey, mySecretKey, nonce, data)",
                    token.loc);
            }

            // Validate and get public key
            BufferPtr pk_buf;
            if (std::holds_alternative<BufferPtr>(args[0])) {
                pk_buf = std::get<BufferPtr>(args[0]);
                if (pk_buf->data.size() != crypto_box_PUBLICKEYBYTES) {
                    throw SwaziError("CryptoError", "publicKey must be 32 bytes", token.loc);
                }
            } else {
                throw SwaziError("TypeError", "publicKey must be Buffer", token.loc);
            }

            // Validate and get secret key
            BufferPtr sk_buf;
            if (std::holds_alternative<BufferPtr>(args[1])) {
                sk_buf = std::get<BufferPtr>(args[1]);
                if (sk_buf->data.size() != crypto_box_SECRETKEYBYTES) {
                    throw SwaziError("CryptoError", "secretKey must be 32 bytes", token.loc);
                }
            } else {
                throw SwaziError("TypeError", "secretKey must be Buffer", token.loc);
            }

            // Validate and get nonce
            BufferPtr nonce_buf;
            if (std::holds_alternative<BufferPtr>(args[2])) {
                nonce_buf = std::get<BufferPtr>(args[2]);
                if (nonce_buf->data.size() != crypto_box_NONCEBYTES) {
                    throw SwaziError("CryptoError", "nonce must be 24 bytes", token.loc);
                }
            } else {
                throw SwaziError("TypeError", "nonce must be Buffer", token.loc);
            }

            // Get plaintext
            std::vector<uint8_t> plaintext;
            if (std::holds_alternative<BufferPtr>(args[3])) {
                plaintext = std::get<BufferPtr>(args[3])->data;
            } else if (std::holds_alternative<std::string>(args[3])) {
                std::string str = std::get<std::string>(args[3]);
                plaintext.assign(str.begin(), str.end());
            } else {
                throw SwaziError("TypeError", "data must be Buffer or string", token.loc);
            }

            // Encrypt
            auto result = std::make_shared<BufferValue>();
            result->data.resize(crypto_box_MACBYTES + plaintext.size());

            int ret = crypto_box_easy(
                result->data.data(),
                plaintext.data(),
                plaintext.size(),
                nonce_buf->data.data(),
                pk_buf->data.data(),
                sk_buf->data.data());

            if (ret != 0) {
                throw SwaziError("CryptoError", "Encryption failed", token.loc);
            }

            result->encoding = "binary";
            return Value{result};
        };

        auto fn_encrypt_value = std::make_shared<FunctionValue>("crypto.box.encrypt",
            fn_encrypt, env, tok);
        box_obj->properties["encrypt"] = PropertyDescriptor{fn_encrypt_value, false, false, false, tok};

        // crypto.box.decrypt(theirPublicKey, mySecretKey, nonce, ciphertext) -> Buffer
        auto fn_decrypt = [](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
            if (args.size() < 4) {
                throw SwaziError("TypeError",
                    "crypto.box.decrypt requires (theirPublicKey, mySecretKey, nonce, ciphertext)",
                    token.loc);
            }

            BufferPtr pk_buf = std::get<BufferPtr>(args[0]);
            BufferPtr sk_buf = std::get<BufferPtr>(args[1]);
            BufferPtr nonce_buf = std::get<BufferPtr>(args[2]);
            BufferPtr cipher_buf = std::get<BufferPtr>(args[3]);

            if (cipher_buf->data.size() < crypto_box_MACBYTES) {
                throw SwaziError("CryptoError", "ciphertext too short", token.loc);
            }

            auto result = std::make_shared<BufferValue>();
            result->data.resize(cipher_buf->data.size() - crypto_box_MACBYTES);

            int ret = crypto_box_open_easy(
                result->data.data(),
                cipher_buf->data.data(),
                cipher_buf->data.size(),
                nonce_buf->data.data(),
                pk_buf->data.data(),
                sk_buf->data.data());

            if (ret != 0) {
                throw SwaziError("CryptoError", "Decryption failed (authentication error)", token.loc);
            }

            result->encoding = "binary";
            return Value{result};
        };

        auto fn_decrypt_value = std::make_shared<FunctionValue>("crypto.box.decrypt",
            fn_decrypt, env, tok);
        box_obj->properties["decrypt"] = PropertyDescriptor{fn_decrypt_value, false, false, false, tok};

        obj->properties["box"] = PropertyDescriptor{Value{box_obj}, false, false, true, tok};
    }

    // ============= DIGITAL SIGNATURES (Ed25519) =============

    // crypto.sign.keypair() -> { publicKey: Buffer, secretKey: Buffer }
    {
        auto fn = [](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
            auto result = std::make_shared<ObjectValue>();

            auto pk = std::make_shared<BufferValue>();
            pk->data.resize(crypto_sign_PUBLICKEYBYTES);

            auto sk = std::make_shared<BufferValue>();
            sk->data.resize(crypto_sign_SECRETKEYBYTES);

            crypto_sign_keypair(pk->data.data(), sk->data.data());

            pk->encoding = "binary";
            sk->encoding = "binary";

            result->properties["publicKey"] = PropertyDescriptor{Value{pk}, false, false, true, Token()};
            result->properties["secretKey"] = PropertyDescriptor{Value{sk}, false, false, true, Token()};

            return Value{result};
        };

        Token tok;
        tok.type = TokenType::IDENTIFIER;
        tok.loc = TokenLocation("<crypto>", 0, 0, 0);
        auto fn_value = std::make_shared<FunctionValue>("crypto.sign.keypair", fn, env, tok);

        auto sign_obj = std::make_shared<ObjectValue>();
        sign_obj->properties["keypair"] = PropertyDescriptor{fn_value, false, false, false, tok};

        // crypto.sign.sign(secretKey, message) -> Buffer (detached signature)
        auto fn_sign = [](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
            if (args.size() < 2) {
                throw SwaziError("TypeError", "crypto.sign.sign requires (secretKey, message)", token.loc);
            }

            BufferPtr sk_buf;
            if (std::holds_alternative<BufferPtr>(args[0])) {
                sk_buf = std::get<BufferPtr>(args[0]);
                if (sk_buf->data.size() != crypto_sign_SECRETKEYBYTES) {
                    throw SwaziError("CryptoError", "secretKey must be 64 bytes", token.loc);
                }
            } else {
                throw SwaziError("TypeError", "secretKey must be Buffer", token.loc);
            }

            std::vector<uint8_t> message;
            if (std::holds_alternative<BufferPtr>(args[1])) {
                message = std::get<BufferPtr>(args[1])->data;
            } else if (std::holds_alternative<std::string>(args[1])) {
                std::string str = std::get<std::string>(args[1]);
                message.assign(str.begin(), str.end());
            } else {
                throw SwaziError("TypeError", "message must be Buffer or string", token.loc);
            }

            auto result = std::make_shared<BufferValue>();
            result->data.resize(crypto_sign_BYTES);

            crypto_sign_detached(
                result->data.data(),
                nullptr,
                message.data(),
                message.size(),
                sk_buf->data.data());

            result->encoding = "binary";
            return Value{result};
        };

        auto fn_sign_value = std::make_shared<FunctionValue>("crypto.sign.sign", fn_sign, env, tok);
        sign_obj->properties["sign"] = PropertyDescriptor{fn_sign_value, false, false, false, tok};

        // crypto.sign.verify(publicKey, signature, message) -> bool
        auto fn_verify = [](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
            if (args.size() < 3) {
                throw SwaziError("TypeError",
                    "crypto.sign.verify requires (publicKey, signature, message)", token.loc);
            }

            BufferPtr pk_buf;
            if (std::holds_alternative<BufferPtr>(args[0])) {
                pk_buf = std::get<BufferPtr>(args[0]);
                if (pk_buf->data.size() != crypto_sign_PUBLICKEYBYTES) {
                    throw SwaziError("CryptoError", "publicKey must be 32 bytes", token.loc);
                }
            } else {
                throw SwaziError("TypeError", "publicKey must be Buffer", token.loc);
            }

            BufferPtr sig_buf;
            if (std::holds_alternative<BufferPtr>(args[1])) {
                sig_buf = std::get<BufferPtr>(args[1]);
                if (sig_buf->data.size() != crypto_sign_BYTES) {
                    throw SwaziError("CryptoError", "signature must be 64 bytes", token.loc);
                }
            } else {
                throw SwaziError("TypeError", "signature must be Buffer", token.loc);
            }

            std::vector<uint8_t> message;
            if (std::holds_alternative<BufferPtr>(args[2])) {
                message = std::get<BufferPtr>(args[2])->data;
            } else if (std::holds_alternative<std::string>(args[2])) {
                std::string str = std::get<std::string>(args[2]);
                message.assign(str.begin(), str.end());
            } else {
                throw SwaziError("TypeError", "message must be Buffer or string", token.loc);
            }

            int ret = crypto_sign_verify_detached(
                sig_buf->data.data(),
                message.data(),
                message.size(),
                pk_buf->data.data());

            return Value{ret == 0};  // 0 = valid, -1 = invalid
        };

        auto fn_verify_value = std::make_shared<FunctionValue>("crypto.sign.verify", fn_verify, env, tok);
        sign_obj->properties["verify"] = PropertyDescriptor{fn_verify_value, false, false, false, tok};

        // crypto.sign.createSigner(secretKey) -> Signer object (hash-then-sign pattern)
        auto fn_create_signer = [env](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
            if (args.empty()) {
                throw SwaziError("TypeError", "createSigner requires secretKey", token.loc);
            }

            BufferPtr sk_buf;
            if (std::holds_alternative<BufferPtr>(args[0])) {
                sk_buf = std::get<BufferPtr>(args[0]);
                if (sk_buf->data.size() != crypto_sign_SECRETKEYBYTES) {
                    throw SwaziError("CryptoError", "secretKey must be 64 bytes", token.loc);
                }
            } else {
                throw SwaziError("TypeError", "secretKey must be Buffer", token.loc);
            }

            // Check for optional algorithm parameter (second argument)
            std::string algorithm;
            if (args.size() >= 2) {
                if (std::holds_alternative<std::string>(args[1])) {
                    algorithm = std::get<std::string>(args[1]);
                    if (algorithm != "sha256" && algorithm != "sha512") {
                        throw SwaziError("CryptoError",
                            "Unsupported algorithm: " + algorithm + ". Supported: sha256, sha512",
                            token.loc);
                    }
                } else if (!std::holds_alternative<std::monostate>(args[1])) {
                    throw SwaziError("TypeError", "algorithm must be string", token.loc);
                }
            }

            auto state = std::make_shared<SignState>();
            state->secret_key = sk_buf->data;
            state->algorithm = algorithm;

            // Initialize hash state if algorithm provided
            if (algorithm == "sha512") {
                crypto_hash_sha512_init(&state->sha512_state);
            } else if (algorithm == "sha256") {
                crypto_hash_sha256_init(&state->sha256_state);
            }
            // else: raw mode (will buffer in accumulated_data)

            auto obj = std::make_shared<ObjectValue>();

            // update(data) method
            auto update_fn = [state](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
                if (state->finalized) {
                    throw SwaziError("CryptoError", "Signer already finalized", token.loc);
                }
                if (args.empty()) {
                    throw SwaziError("TypeError", "update requires data argument", token.loc);
                }

                std::vector<uint8_t> data;
                if (std::holds_alternative<BufferPtr>(args[0])) {
                    data = std::get<BufferPtr>(args[0])->data;
                } else if (std::holds_alternative<std::string>(args[0])) {
                    std::string str = std::get<std::string>(args[0]);
                    data.assign(str.begin(), str.end());
                } else {
                    throw SwaziError("TypeError", "data must be Buffer or string", token.loc);
                }

                if (state->algorithm == "sha512") {
                    crypto_hash_sha512_update(&state->sha512_state, data.data(), data.size());
                } else if (state->algorithm == "sha256") {
                    crypto_hash_sha256_update(&state->sha256_state, data.data(), data.size());
                } else {
                    // Raw mode: accumulate in memory
                    state->accumulated_data.insert(
                        state->accumulated_data.end(),
                        data.begin(),
                        data.end());
                }

                return std::monostate{};
            };

            // finalize() method - returns signature
            auto finalize_fn = [state](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
                if (state->finalized) {
                    throw SwaziError("CryptoError", "Signer already finalized", token.loc);
                }

                std::vector<uint8_t> data_to_sign;

                if (state->algorithm == "sha512") {
                    data_to_sign.resize(crypto_hash_sha512_BYTES);
                    crypto_hash_sha512_final(&state->sha512_state, data_to_sign.data());
                } else if (state->algorithm == "sha256") {
                    data_to_sign.resize(crypto_hash_sha256_BYTES);
                    crypto_hash_sha256_final(&state->sha256_state, data_to_sign.data());
                } else {
                    // Raw mode: sign accumulated data directly
                    data_to_sign = std::move(state->accumulated_data);
                }

                auto result = std::make_shared<BufferValue>();
                result->data.resize(crypto_sign_BYTES);

                crypto_sign_detached(
                    result->data.data(),
                    nullptr,
                    data_to_sign.data(),
                    data_to_sign.size(),
                    state->secret_key.data());

                state->finalized = true;
                result->encoding = "binary";
                return Value{result};
            };

            Token tok;
            tok.type = TokenType::IDENTIFIER;
            tok.loc = TokenLocation("<crypto>", 0, 0, 0);

            obj->properties["update"] = PropertyDescriptor{
                std::make_shared<FunctionValue>("update", update_fn, env, tok),
                false, false, false, tok};
            obj->properties["finalize"] = PropertyDescriptor{
                std::make_shared<FunctionValue>("finalize", finalize_fn, env, tok),
                false, false, false, tok};

            return Value{obj};
        };

        auto fn_create_signer_value = std::make_shared<FunctionValue>(
            "crypto.sign.createSigner", fn_create_signer, env, tok);
        sign_obj->properties["createSigner"] = PropertyDescriptor{
            fn_create_signer_value, false, false, false, tok};

        // crypto.sign.createVerifier(publicKey, signature) -> Verifier object
        auto fn_create_verifier = [env](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
            if (args.size() < 2) {
                throw SwaziError("TypeError", "createVerifier requires (publicKey, signature)", token.loc);
            }

            BufferPtr pk_buf;
            if (std::holds_alternative<BufferPtr>(args[0])) {
                pk_buf = std::get<BufferPtr>(args[0]);
                if (pk_buf->data.size() != crypto_sign_PUBLICKEYBYTES) {
                    throw SwaziError("CryptoError", "publicKey must be 32 bytes", token.loc);
                }
            } else {
                throw SwaziError("TypeError", "publicKey must be Buffer", token.loc);
            }

            BufferPtr sig_buf;
            if (std::holds_alternative<BufferPtr>(args[1])) {
                sig_buf = std::get<BufferPtr>(args[1]);
                if (sig_buf->data.size() != crypto_sign_BYTES) {
                    throw SwaziError("CryptoError", "signature must be 64 bytes", token.loc);
                }
            } else {
                throw SwaziError("TypeError", "signature must be Buffer", token.loc);
            }

            // Check for optional algorithm parameter (third argument)
            std::string algorithm;
            if (args.size() >= 3) {
                if (std::holds_alternative<std::string>(args[2])) {
                    algorithm = std::get<std::string>(args[2]);
                    if (algorithm != "sha256" && algorithm != "sha512") {
                        throw SwaziError("CryptoError",
                            "Unsupported algorithm: " + algorithm + ". Supported: sha256, sha512",
                            token.loc);
                    }
                } else if (!std::holds_alternative<std::monostate>(args[2])) {
                    throw SwaziError("TypeError", "algorithm must be string", token.loc);
                }
            }

            struct VerifyState {
                std::string algorithm;
                std::vector<uint8_t> public_key;
                std::vector<uint8_t> signature;
                crypto_hash_sha512_state sha512_state;
                crypto_hash_sha256_state sha256_state;
                std::vector<uint8_t> accumulated_data;
                bool finalized = false;
            };

            auto state = std::make_shared<VerifyState>();
            state->public_key = pk_buf->data;
            state->signature = sig_buf->data;
            state->algorithm = algorithm;

            // Initialize hash state if algorithm provided
            if (algorithm == "sha512") {
                crypto_hash_sha512_init(&state->sha512_state);
            } else if (algorithm == "sha256") {
                crypto_hash_sha256_init(&state->sha256_state);
            }
            // else: raw mode (will buffer in accumulated_data)

            auto obj = std::make_shared<ObjectValue>();

            // update(data) method
            auto update_fn = [state](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
                if (state->finalized) {
                    throw SwaziError("CryptoError", "Verifier already finalized", token.loc);
                }
                if (args.empty()) {
                    throw SwaziError("TypeError", "update requires data argument", token.loc);
                }

                std::vector<uint8_t> data;
                if (std::holds_alternative<BufferPtr>(args[0])) {
                    data = std::get<BufferPtr>(args[0])->data;
                } else if (std::holds_alternative<std::string>(args[0])) {
                    std::string str = std::get<std::string>(args[0]);
                    data.assign(str.begin(), str.end());
                } else {
                    throw SwaziError("TypeError", "data must be Buffer or string", token.loc);
                }

                if (state->algorithm == "sha512") {
                    crypto_hash_sha512_update(&state->sha512_state, data.data(), data.size());
                } else if (state->algorithm == "sha256") {
                    crypto_hash_sha256_update(&state->sha256_state, data.data(), data.size());
                } else {
                    // Raw mode: accumulate in memory
                    state->accumulated_data.insert(
                        state->accumulated_data.end(),
                        data.begin(),
                        data.end());
                }

                return std::monostate{};
            };

            // finalize() method - returns true/false
            auto finalize_fn = [state](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
                if (state->finalized) {
                    throw SwaziError("CryptoError", "Verifier already finalized", token.loc);
                }

                std::vector<uint8_t> data_to_verify;

                if (state->algorithm == "sha512") {
                    data_to_verify.resize(crypto_hash_sha512_BYTES);
                    crypto_hash_sha512_final(&state->sha512_state, data_to_verify.data());
                } else if (state->algorithm == "sha256") {
                    data_to_verify.resize(crypto_hash_sha256_BYTES);
                    crypto_hash_sha256_final(&state->sha256_state, data_to_verify.data());
                } else {
                    // Raw mode: verify against accumulated data directly
                    data_to_verify = std::move(state->accumulated_data);
                }

                int ret = crypto_sign_verify_detached(
                    state->signature.data(),
                    data_to_verify.data(),
                    data_to_verify.size(),
                    state->public_key.data());

                state->finalized = true;
                return Value{ret == 0};  // 0 = valid, -1 = invalid
            };

            Token tok;
            tok.type = TokenType::IDENTIFIER;
            tok.loc = TokenLocation("<crypto>", 0, 0, 0);

            obj->properties["update"] = PropertyDescriptor{
                std::make_shared<FunctionValue>("update", update_fn, env, tok),
                false, false, false, tok};
            obj->properties["finalize"] = PropertyDescriptor{
                std::make_shared<FunctionValue>("finalize", finalize_fn, env, tok),
                false, false, false, tok};

            return Value{obj};
        };

        auto fn_create_verifier_value = std::make_shared<FunctionValue>(
            "crypto.sign.createVerifier", fn_create_verifier, env, tok);
        sign_obj->properties["createVerifier"] = PropertyDescriptor{
            fn_create_verifier_value, false, false, false, tok};

        obj->properties["sign"] = PropertyDescriptor{Value{sign_obj}, false, false, true, tok};
    }

    // ============= KEY DERIVATION =============

    // crypto.kdf(password, salt, opsLimit, memLimit, keyLength) -> Buffer
    {
        auto fn = [](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
            if (args.size() < 5) {
                throw SwaziError("TypeError",
                    "crypto.kdf requires (password, salt, opsLimit, memLimit, keyLength)", token.loc);
            }

            // Get password
            std::vector<uint8_t> password;
            if (std::holds_alternative<BufferPtr>(args[0])) {
                password = std::get<BufferPtr>(args[0])->data;
            } else if (std::holds_alternative<std::string>(args[0])) {
                std::string str = std::get<std::string>(args[0]);
                password.assign(str.begin(), str.end());
            } else {
                throw SwaziError("TypeError", "password must be Buffer or string", token.loc);
            }

            // Get salt (must be exactly 16 bytes)
            BufferPtr salt_buf;
            if (std::holds_alternative<BufferPtr>(args[1])) {
                salt_buf = std::get<BufferPtr>(args[1]);
                if (salt_buf->data.size() != crypto_pwhash_SALTBYTES) {
                    throw SwaziError("CryptoError",
                        "salt must be exactly " + std::to_string(crypto_pwhash_SALTBYTES) + " bytes",
                        token.loc);
                }
            } else {
                throw SwaziError("TypeError", "salt must be Buffer", token.loc);
            }

            // Get opsLimit
            if (!std::holds_alternative<double>(args[2])) {
                throw SwaziError("TypeError", "opsLimit must be number", token.loc);
            }
            unsigned long long opsLimit = static_cast<unsigned long long>(std::get<double>(args[2]));

            // Get memLimit
            if (!std::holds_alternative<double>(args[3])) {
                throw SwaziError("TypeError", "memLimit must be number", token.loc);
            }
            size_t memLimit = static_cast<size_t>(std::get<double>(args[3]));

            // Get keyLength
            if (!std::holds_alternative<double>(args[4])) {
                throw SwaziError("TypeError", "keyLength must be number", token.loc);
            }
            size_t keyLength = static_cast<size_t>(std::get<double>(args[4]));

            if (keyLength > 1024) {
                throw SwaziError("RangeError", "keyLength too large (max 1024)", token.loc);
            }

            auto result = std::make_shared<BufferValue>();
            result->data.resize(keyLength);

            int ret = crypto_pwhash(
                result->data.data(),
                keyLength,
                reinterpret_cast<const char*>(password.data()),
                password.size(),
                salt_buf->data.data(),
                opsLimit,
                memLimit,
                crypto_pwhash_ALG_DEFAULT);

            if (ret != 0) {
                throw SwaziError("CryptoError", "Key derivation failed (out of memory?)", token.loc);
            }

            result->encoding = "binary";
            return Value{result};
        };

        Token tok;
        tok.type = TokenType::IDENTIFIER;
        tok.loc = TokenLocation("<crypto>", 0, 0, 0);
        auto fn_value = std::make_shared<FunctionValue>("crypto.kdf", fn, env, tok);
        obj->properties["kdf"] = PropertyDescriptor{fn_value, false, false, false, tok};
    }

    // Export KDF constants for recommended parameters
    obj->properties["KDF_OPSLIMIT_INTERACTIVE"] = PropertyDescriptor{
        Value{static_cast<double>(crypto_pwhash_OPSLIMIT_INTERACTIVE)},
        false, false, true, Token()};
    obj->properties["KDF_MEMLIMIT_INTERACTIVE"] = PropertyDescriptor{
        Value{static_cast<double>(crypto_pwhash_MEMLIMIT_INTERACTIVE)},
        false, false, true, Token()};
    obj->properties["KDF_OPSLIMIT_MODERATE"] = PropertyDescriptor{
        Value{static_cast<double>(crypto_pwhash_OPSLIMIT_MODERATE)},
        false, false, true, Token()};
    obj->properties["KDF_MEMLIMIT_MODERATE"] = PropertyDescriptor{
        Value{static_cast<double>(crypto_pwhash_MEMLIMIT_MODERATE)},
        false, false, true, Token()};
    obj->properties["KDF_OPSLIMIT_SENSITIVE"] = PropertyDescriptor{
        Value{static_cast<double>(crypto_pwhash_OPSLIMIT_SENSITIVE)},
        false, false, true, Token()};
    obj->properties["KDF_MEMLIMIT_SENSITIVE"] = PropertyDescriptor{
        Value{static_cast<double>(crypto_pwhash_MEMLIMIT_SENSITIVE)},
        false, false, true, Token()};

    // ============= CONSTANT-TIME COMPARISON =============

    // crypto.timingSafeEqual(a, b) -> bool
    {
        auto fn = [](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
            if (args.size() < 2) {
                throw SwaziError("TypeError", "crypto.timingSafeEqual requires (a, b)", token.loc);
            }

            BufferPtr a_buf;
            if (std::holds_alternative<BufferPtr>(args[0])) {
                a_buf = std::get<BufferPtr>(args[0]);
            } else {
                throw SwaziError("TypeError", "first argument must be Buffer", token.loc);
            }

            BufferPtr b_buf;
            if (std::holds_alternative<BufferPtr>(args[1])) {
                b_buf = std::get<BufferPtr>(args[1]);
            } else {
                throw SwaziError("TypeError", "second argument must be Buffer", token.loc);
            }

            if (a_buf->data.size() != b_buf->data.size()) {
                return Value{false};
            }

            int ret = sodium_memcmp(a_buf->data.data(), b_buf->data.data(), a_buf->data.size());
            return Value{ret == 0};
        };

        Token tok;
        tok.type = TokenType::IDENTIFIER;
        tok.loc = TokenLocation("<crypto>", 0, 0, 0);
        auto fn_value = std::make_shared<FunctionValue>("crypto.timingSafeEqual", fn, env, tok);
        obj->properties["timingSafeEqual"] = PropertyDescriptor{fn_value, false, false, false, tok};
    }

    // ============= MEMORY UTILITIES =============

    // crypto.memzero(buffer) -> void (securely wipe buffer)
    {
        auto fn = [](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
            if (args.empty()) {
                throw SwaziError("TypeError", "crypto.memzero requires buffer argument", token.loc);
            }

            if (std::holds_alternative<BufferPtr>(args[0])) {
                BufferPtr buf = std::get<BufferPtr>(args[0]);
                sodium_memzero(buf->data.data(), buf->data.size());
            } else {
                throw SwaziError("TypeError", "argument must be Buffer", token.loc);
            }

            return std::monostate{};
        };

        Token tok;
        tok.type = TokenType::IDENTIFIER;
        tok.loc = TokenLocation("<crypto>", 0, 0, 0);
        auto fn_value = std::make_shared<FunctionValue>("crypto.memzero", fn, env, tok);
        obj->properties["memzero"] = PropertyDescriptor{fn_value, false, false, false, tok};
    }

    // ============= UUID GENERATION =============

    // crypto.randomUUID() -> string (UUID v4)
    {
        auto fn = [](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
            // Generate 16 random bytes
            uint8_t bytes[16];
            randombytes_buf(bytes, 16);

            // Set version (4) and variant bits according to RFC 4122
            bytes[6] = (bytes[6] & 0x0f) | 0x40;  // Version 4
            bytes[8] = (bytes[8] & 0x3f) | 0x80;  // Variant 10

            // Format as UUID string: xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx
            char uuid[37];  // 32 hex chars + 4 dashes + null terminator
            snprintf(uuid, sizeof(uuid),
                "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                bytes[0], bytes[1], bytes[2], bytes[3],
                bytes[4], bytes[5],
                bytes[6], bytes[7],
                bytes[8], bytes[9],
                bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);

            return Value{std::string(uuid)};
        };

        Token tok;
        tok.type = TokenType::IDENTIFIER;
        tok.loc = TokenLocation("<crypto>", 0, 0, 0);
        auto fn_value = std::make_shared<FunctionValue>("crypto.randomUUID", fn, env, tok);
        obj->properties["randomUUID"] = PropertyDescriptor{fn_value, false, false, false, tok};
    }

    // crypto.uuidToBytes(uuid) -> Buffer (convert UUID string to 16 bytes)
    {
        auto fn = [](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
            if (args.empty() || !std::holds_alternative<std::string>(args[0])) {
                throw SwaziError("TypeError", "crypto.uuidToBytes requires UUID string", token.loc);
            }

            std::string uuid = std::get<std::string>(args[0]);

            // Remove dashes
            std::string hex;
            for (char c : uuid) {
                if (c != '-') hex += c;
            }

            if (hex.length() != 32) {
                throw SwaziError("ValueError", "Invalid UUID format (expected 32 hex chars)", token.loc);
            }

            // Convert hex to bytes
            auto result = std::make_shared<BufferValue>();
            result->data.resize(16);

            for (size_t i = 0; i < 16; ++i) {
                std::string byte_str = hex.substr(i * 2, 2);
                result->data[i] = static_cast<uint8_t>(std::stoul(byte_str, nullptr, 16));
            }

            result->encoding = "binary";
            return Value{result};
        };

        Token tok;
        tok.type = TokenType::IDENTIFIER;
        tok.loc = TokenLocation("<crypto>", 0, 0, 0);
        auto fn_value = std::make_shared<FunctionValue>("crypto.uuidToBytes", fn, env, tok);
        obj->properties["uuidToBytes"] = PropertyDescriptor{fn_value, false, false, false, tok};
    }

    // crypto.bytesToUUID(buffer) -> string (convert 16 bytes to UUID string)
    {
        auto fn = [](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
            if (args.empty() || !std::holds_alternative<BufferPtr>(args[0])) {
                throw SwaziError("TypeError", "crypto.bytesToUUID requires Buffer", token.loc);
            }

            BufferPtr buf = std::get<BufferPtr>(args[0]);
            if (buf->data.size() != 16) {
                throw SwaziError("ValueError", "Buffer must be exactly 16 bytes", token.loc);
            }

            // Format as UUID string
            char uuid[37];
            const uint8_t* bytes = buf->data.data();
            snprintf(uuid, sizeof(uuid),
                "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                bytes[0], bytes[1], bytes[2], bytes[3],
                bytes[4], bytes[5],
                bytes[6], bytes[7],
                bytes[8], bytes[9],
                bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);

            return Value{std::string(uuid)};
        };

        Token tok;
        tok.type = TokenType::IDENTIFIER;
        tok.loc = TokenLocation("<crypto>", 0, 0, 0);
        auto fn_value = std::make_shared<FunctionValue>("crypto.bytesToUUID", fn, env, tok);
        obj->properties["bytesToUUID"] = PropertyDescriptor{fn_value, false, false, false, tok};
    }

    return obj;
}