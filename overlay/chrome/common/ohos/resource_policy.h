// Copyright 2026 The Chromium Authors. BSD-style license in LICENSE.
#ifndef CHROME_COMMON_OHOS_RESOURCE_POLICY_H_
#define CHROME_COMMON_OHOS_RESOURCE_POLICY_H_

#include <charconv>
#include <cstdint>
#include <string>
#include <string_view>

namespace chrome::ohos::resources {
inline std::string Lower(std::string_view value) {
  std::string result(value);
  for (char& c : result) {
    if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
  }
  return result;
}

// Response headers only. Never open/replay a request to classify its body.
inline std::string Classify(std::string_view path, std::string_view mime,
                            std::string_view disposition, int status,
                            std::string_view method) {
  if (method != "GET" || (status != 200 && status != 206)) return {};
  const auto type = Lower(mime.substr(0, mime.find(';')));
  const auto name = Lower(path);
  // Known stream fragments are not standalone downloads.
  if (name.ends_with(".m4s") || name.ends_with(".ts") ||
      type == "video/mp2t" || type == "video/iso.segment" ||
      type == "audio/iso.segment") return {};
  if (type == "application/vnd.apple.mpegurl" || type == "application/x-mpegurl" ||
      type == "audio/mpegurl" || type == "audio/x-mpegurl") return "hls";
  if (type == "application/dash+xml") return "dash";
  // An error/JSON document named movie.mp4 must not become a video candidate.
  if (type == "text/html" || type == "application/json") return {};
  if (name.ends_with(".m3u8")) return "hls";
  if (name.ends_with(".mpd")) return "dash";
  if (type.starts_with("video/")) return "video";
  if (type.starts_with("audio/")) return "audio";
  if (type.starts_with("image/")) return "image";
  const bool generic = type.empty() || type == "application/octet-stream";
  if (generic) {
    for (auto ext : {".mp4", ".webm", ".mov", ".mkv"})
      if (name.ends_with(ext)) return "video";
    for (auto ext : {".mp3", ".m4a", ".aac", ".ogg", ".flac", ".wav"})
      if (name.ends_with(ext)) return "audio";
    for (auto ext : {".png", ".jpg", ".jpeg", ".gif", ".webp", ".avif"})
      if (name.ends_with(ext)) return "image";
  }
  if (type == "application/pdf" || Lower(disposition).starts_with("attachment"))
    return "file";
  return {};
}

inline int64_t TotalBytes(int status, int64_t length, std::string_view range) {
  if (status == 200) return length >= 0 ? length : -1;
  if (status != 206 || !range.starts_with("bytes ")) return -1;
  const auto dash = range.find('-'), slash = range.find('/');
  if (dash == range.npos || slash == range.npos || dash <= 6 || slash <= dash + 1)
    return -1;
  auto parse = [](std::string_view text, int64_t& value) {
    auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    return error == std::errc() && end == text.data() + text.size() && value >= 0;
  };
  int64_t first, last, total;
  if (!parse(range.substr(6, dash - 6), first) ||
      !parse(range.substr(dash + 1, slash - dash - 1), last) ||
      !parse(range.substr(slash + 1), total) || first > last || last >= total)
    return -1;
  return total;
}
}  // namespace chrome::ohos::resources
#endif
