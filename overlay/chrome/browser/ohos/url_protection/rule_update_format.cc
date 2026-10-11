// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license in LICENSE.
#include "chrome/browser/ohos/url_protection/rule_update_format.h"

#include <algorithm>
#include <cmath>
#include "base/base64.h"
#include "base/json/json_reader.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "chrome/browser/ohos/url_protection/url_protection.h"
#include "crypto/hash.h"
#include "third_party/boringssl/src/include/openssl/curve25519.h"
#include "url/origin.h"

namespace chrome::ohos::url_protection {
namespace {
bool Endpoint(const GURL& url) {
  return IsPublicHttpUrl(url) && url.SchemeIs("https") && !url.has_query() &&
         !url.has_ref();
}
bool Hex(std::string_view value, size_t size) {
  return value.size() == size && std::ranges::all_of(value, [](char c) {
           return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
         });
}
}  // namespace
UpdateConfig ParseUpdateConfig(std::string_view json) {
  UpdateConfig config;
  if (json.empty())
    return config;
  config.status = "invalid_config";
  if (json.size() > kMaxManifestBytes)
    return config;
  auto root = base::JSONReader::ReadDict(json, base::JSON_PARSE_RFC);
  if (!root || root->FindInt("format") != 1)
    return config;
  auto* endpoint = root->FindString("manifestUrl");
  auto* keys = root->FindDict("keys");
  auto minimum = root->FindInt("minimumSequence");
  if (!endpoint || !keys || keys->empty() || keys->size() > 4 || !minimum ||
      *minimum < 0)
    return config;
  config.manifest_url = GURL(*endpoint);
  if (!Endpoint(config.manifest_url))
    return config;
  for (const auto [id, key] : *keys) {
    std::string decoded;
    if (id.empty() || id.size() > 32 || !key.is_string() ||
        !std::ranges::all_of(id,
                             [](char c) {
                               return base::IsAsciiAlphaNumeric(c) ||
                                      c == '_' || c == '-';
                             }) ||
        !base::Base64Decode(key.GetString(), &decoded) ||
        decoded.size() != ED25519_PUBLIC_KEY_LEN)
      return config;
    config.keys.emplace(id, std::move(decoded));
  }
  config.minimum_sequence = *minimum;
  config.status = "ready";
  return config;
}
std::optional<RuleManifest> VerifyRuleManifest(std::string_view json,
                                               const UpdateConfig& config,
                                               int minimum_sequence,
                                               base::Time now) {
  if (!config.valid() || json.size() > kMaxManifestBytes)
    return std::nullopt;
  auto envelope = base::JSONReader::ReadDict(json, base::JSON_PARSE_RFC);
  if (!envelope || envelope->FindInt("format") != 1)
    return std::nullopt;
  auto* id = envelope->FindString("keyId");
  auto* encoded = envelope->FindString("signed");
  auto* signature = envelope->FindString("signature");
  std::string message, decoded_signature;
  if (!id || !encoded || !signature || !config.keys.contains(*id) ||
      !base::Base64Decode(*encoded, &message) || message.size() > 8192 ||
      !base::Base64Decode(*signature, &decoded_signature) ||
      decoded_signature.size() != ED25519_SIGNATURE_LEN ||
      ED25519_verify(
          reinterpret_cast<const uint8_t*>(message.data()), message.size(),
          reinterpret_cast<const uint8_t*>(decoded_signature.data()),
          reinterpret_cast<const uint8_t*>(config.keys.at(*id).data())) != 1)
    return std::nullopt;
  auto root = base::JSONReader::ReadDict(message, base::JSON_PARSE_RFC);
  if (!root || root->FindInt("format") != 1)
    return std::nullopt;
  auto sequence = root->FindInt("sequence");
  auto size = root->FindInt("size");
  auto count = root->FindInt("count");
  auto generated = root->FindDouble("generatedAt");
  auto expires = root->FindDouble("expiresAt");
  auto* url = root->FindString("rulesUrl");
  auto* digest = root->FindString("sha256");
  auto* version = root->FindString("version");
  if (!sequence || *sequence < 1 ||
      *sequence < std::max(minimum_sequence, config.minimum_sequence) ||
      !size || *size <= 0 || *size > static_cast<int>(kMaxRuleBytes) ||
      !count || *count < 1 || *count > 200000 || !generated || !expires ||
      !std::isfinite(*generated) || !std::isfinite(*expires) || !url ||
      !digest || !Hex(*digest, 64) || !version || !Hex(*version, 40))
    return std::nullopt;
  RuleManifest manifest;
  manifest.sequence = *sequence;
  manifest.size = *size;
  manifest.count = *count;
  manifest.rules_url = GURL(*url);
  manifest.sha256 = *digest;
  manifest.version = *version;
  manifest.generated = base::Time::FromMillisecondsSinceUnixEpoch(*generated);
  manifest.expires = base::Time::FromMillisecondsSinceUnixEpoch(*expires);
  if (!Endpoint(manifest.rules_url) ||
      url::Origin::Create(manifest.rules_url) !=
          url::Origin::Create(config.manifest_url) ||
      manifest.generated > now || manifest.expires <= now ||
      manifest.expires <= manifest.generated ||
      manifest.expires - manifest.generated > base::Days(7))
    return std::nullopt;
  return manifest;
}
std::optional<LocalRules> VerifyRulePayload(std::string_view payload,
                                            const RuleManifest& manifest,
                                            base::Time now) {
  if (payload.size() != static_cast<size_t>(manifest.size) ||
      base::ToLowerASCII(base::HexEncode(crypto::hash::Sha256(payload))) !=
          manifest.sha256)
    return std::nullopt;
  auto root = base::JSONReader::ReadDict(payload, base::JSON_PARSE_RFC);
  if (!root || !root->FindList("urls") ||
      root->FindList("urls")->size() != static_cast<size_t>(manifest.count))
    return std::nullopt;
  auto rules = ParseLocalRules(payload, now);
  if (!rules.IsUsable(now) || rules.version != manifest.version ||
      rules.generated != manifest.generated ||
      rules.expires != manifest.expires)
    return std::nullopt;
  return rules;
}
bool AcceptRuleSequence(const RuleManifest& manifest,
                        int installed_sequence,
                        std::string_view installed_digest) {
  return manifest.sequence > installed_sequence ||
         (manifest.sequence == installed_sequence &&
          !installed_digest.empty() && manifest.sha256 == installed_digest);
}
std::string EncodeRuleCache(std::string_view manifest,
                            std::string_view payload) {
  return base::NumberToString(manifest.size()) + "\n" + std::string(manifest) +
         std::string(payload);
}
bool DecodeRuleCache(std::string_view cache,
                     std::string& manifest,
                     std::string& payload) {
  const auto separator = cache.find('\n');
  size_t length = 0;
  if (separator == std::string_view::npos || separator > 6 ||
      !base::StringToSizeT(cache.substr(0, separator), &length) || !length ||
      length > kMaxManifestBytes || cache.size() <= separator + 1 + length ||
      cache.size() - separator - 1 - length > kMaxRuleBytes)
    return false;
  manifest = cache.substr(separator + 1, length);
  payload = cache.substr(separator + 1 + length);
  return true;
}
}  // namespace chrome::ohos::url_protection
