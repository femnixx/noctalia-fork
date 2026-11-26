#pragma once

#include <string>
#include <vector>
#include <optional>
#include <chrono>
#include <functional>

namespace noctalia::services {

struct LyricLine {
  std::int64_t timestampUs;  // -1 for plain (unsynced) lyrics
  std::string text;
};

struct LyricsResult {
  std::vector<LyricLine> lines;
  bool isSynced = false;  // true if timestamps are present
  bool instrumental = false;
};

class LyricsService {
public:
  LyricsService();
  ~LyricsService();

  // Non-copyable, movable
  LyricsService(const LyricsService&) = delete;
  LyricsService& operator=(const LyricsService&) = delete;
  LyricsService(LyricsService&&) = default;
  LyricsService& operator=(LyricsService&&) = default;

  // Fetch lyrics for a track
  // artist: track artist name
  // title: track title
  // durationSec: track duration in seconds (for LRCLIB)
  // callback: called with result (or empty on failure)
  void fetchLyrics(const std::string& artist,
                   const std::string& title,
                   int durationSec,
                   std::function<void(std::optional<LyricsResult>)> callback);

  // Cancel any pending fetch
  void cancel();

  // Set cache directory
  void setCacheDir(const std::string& dir);

private:
  std::string m_cacheDir;
  std::string m_pendingCacheKey;
  bool m_fetching = false;

  // Generate cache key from artist + title + duration
  std::string makeCacheKey(const std::string& artist, const std::string& title, int durationSec) const;

  // Try to load from cache
  std::optional<LyricsResult> loadFromCache(const std::string& cacheKey) const;

  // Save to cache
  void saveToCache(const std::string& cacheKey, const LyricsResult& result) const;

  // Parse LRC format into LyricLine vector
  static std::vector<LyricLine> parseLRC(const std::string& lrcText);

  // Parse plain lyrics (no timestamps)
  static std::vector<LyricLine> parsePlainLyrics(const std::string& text);
};

} // namespace noctalia::services