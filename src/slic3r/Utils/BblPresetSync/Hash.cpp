#include "Hash.hpp"

#include <openssl/evp.h>
#include <openssl/md5.h>

#include <cstdio>

namespace Slic3r::BblPresetSync {

std::string md5_hex(const std::string& data)
{
    unsigned char digest[MD5_DIGEST_LENGTH];

    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    EVP_DigestInit(ctx, EVP_md5());
    EVP_DigestUpdate(ctx, data.c_str(), data.length());
    EVP_DigestFinal(ctx, digest, nullptr);
    EVP_MD_CTX_free(ctx);

    char hex[MD5_DIGEST_LENGTH * 2 + 1];
    for (int i = 0; i < MD5_DIGEST_LENGTH; ++i)
        std::snprintf(hex + (i * 2), 3, "%02x", digest[i]);
    hex[MD5_DIGEST_LENGTH * 2] = '\0';
    return std::string(hex);
}

const std::set<std::string>& hash_ignored_keys()
{
    static const std::set<std::string> keys = {"updated_time", "update_time", "user_id", "setting_id", "version",
                                               "name",         "type",        "public",  "code",       "nickname"};
    return keys;
}

std::string upload_hash(const ValueMap& values)
{
    const std::set<std::string>& ignored = hash_ignored_keys();
    std::string                  text;
    for (const auto& [key, value] : values) {
        if (ignored.count(key)) continue;
        if (key == "base_id" && value.empty()) continue;
        text += key;
        text += '=';
        text += value;
        text += '\n';
    }
    return md5_hex(text);
}

} // namespace Slic3r::BblPresetSync
