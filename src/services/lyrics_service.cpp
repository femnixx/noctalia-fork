#include "services/lyrics_service.h"

#include <chrono>
#include <fstream>
#include <filesystem>
#include <regex>
#include <sstream>
#include <iomanip>
#include <glib.h>
#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include "core/log.h"

namespace noctalia::services {

constexpr Logger kLog("lyrics");

// HTTP response writer for libcurl
size_t writeCallback(void* contents, size_t size, size_t nmemb, std::string* response) {
  size_t totalSize = size * nmemb;
  response->append(static_cast<char*>(contents), totalSize);
  return totalSize;
}

// Fetch URL with libcurl
std::optional<std::string> fetchUrl(const std::string& url) {
  CURL* curl = curl_easy_init();
  if (!curl) {
    kLog.error("LyricsService: Failed to initialize curl");
    return std::nullopt;
  }

  std::string response;
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "Noctalia/1.0");

  CURLcode res = curl_easy_perform(curl);
  long httpCode = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
  curl_easy_cleanup(curl);

  if (res != CURLE_OK) {
    kLog.error("LyricsService: curl error: {}", curl_easy_strerror(res));
    return std::nullopt;
  }

  if (httpCode != 200) {
    kLog.debug("LyricsService: HTTP {} for {}", httpCode, url);
    return std::nullopt;
  }

  return response;
}

// Generate MD5 hash using GLib
std::string md5Hex(const std::string& input) {
  GChecksum* checksum = g_checksum_new(G_CHECKSUM_MD5);
  if (!checksum) return "";
  g_checksum_update(checksum, reinterpret_cast<const guchar*>(input.c_str()), static_cast<gssize>(input.size()));
  const gchar* value = g_checksum_get_string(checksum);
  std::string result = value ? value : "";
  g_checksum_free(checksum);
  return result;
}

LyricsService::LyricsService() {
  // Default cache directory
  const char* xdgCache = std::getenv("XDG_CACHE_HOME");
  if (xdgCache && *xdgCache) {
    m_cacheDir = std::string(xdgCache) + "/noctalia/lyrics";
  } else {
    const char* home = std::getenv("HOME");
    if (home && *home) {
      m_cacheDir = std::string(home) + "/.cache/noctalia/lyrics";
    } else {
      m_cacheDir = "/tmp/noctalia/lyrics";
    }
  }
  std::filesystem::create_directories(m_cacheDir);
}

LyricsService::~LyricsService() = default;

void LyricsService::setCacheDir(const std::string& dir) {
  m_cacheDir = dir;
  std::filesystem::create_directories(m_cacheDir);
}

std::string LyricsService::makeCacheKey(const std::string& artist,
                                         const std::string& title,
                                         int durationSec) const {
  // Normalize: lowercase, trim, remove special chars
  auto normalize = [](const std::string& s) {
    std::string result;
    result.reserve(s.size());
    for (char c : s) {
      if (std::isalnum(static_cast<unsigned char>(c)) || c == ' ') {
        result += std::tolower(static_cast<unsigned char>(c));
      }
    }
    // Collapse multiple spaces
    std::string collapsed;
    bool lastSpace = false;
    for (char c : result) {
      if (c == ' ') {
        if (!lastSpace) collapsed += ' ';
        lastSpace = true;
      } else {
        collapsed += c;
        lastSpace = false;
      }
    }
    return collapsed;
  };

  std::string normalized = normalize(artist) + "|" + normalize(title) + "|" + std::to_string(durationSec);

  // MD5 hash for filename using GLib
  return md5Hex(normalized);
}

std::optional<LyricsResult> LyricsService::loadFromCache(const std::string& cacheKey) const {
  std::filesystem::path cachePath = std::filesystem::path(m_cacheDir) / (cacheKey + ".json");
  if (!std::filesystem::exists(cachePath)) {
    return std::nullopt;
  }

  std::ifstream file(cachePath);
  if (!file) {
    return std::nullopt;
  }

  try {
    nlohmann::json json;
    file >> json;

    LyricsResult result;
    result.isSynced = json.value("synced", false);
    result.instrumental = json.value("instrumental", false);

    for (const auto& lineJson : json["lines"]) {
      LyricLine line;
      line.timestampUs = lineJson.value("timestampUs", -1);
      line.text = lineJson.value("text", "");
      result.lines.push_back(std::move(line));
    }

    // Check cache age (30 days TTL)
    auto fileTime = std::filesystem::last_write_time(cachePath);
    auto now = std::filesystem::file_time_type::clock::now();
    if (now - fileTime > std::chrono::days(30)) {
      kLog.debug("LyricsService: Cache expired for {}", cacheKey);
      return std::nullopt;
    }

    kLog.debug("LyricsService: Cache hit for {}", cacheKey);
    return result;
  } catch (const std::exception& e) {
    kLog.error("LyricsService: Failed to parse cache {}: {}", cacheKey, e.what());
    return std::nullopt;
  }
}

void LyricsService::saveToCache(const std::string& cacheKey, const LyricsResult& result) const {
  std::filesystem::path cachePath = std::filesystem::path(m_cacheDir) / (cacheKey + ".json");

  nlohmann::json json;
  json["synced"] = result.isSynced;
  json["instrumental"] = result.instrumental;
  json["cachedAt"] = std::chrono::duration_cast<std::chrono::seconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();

  for (const auto& line : result.lines) {
    nlohmann::json lineJson;
    lineJson["timestampUs"] = line.timestampUs;
    lineJson["text"] = line.text;
    json["lines"].push_back(lineJson);
  }

  std::ofstream file(cachePath);
  if (file) {
    file << json.dump(2);
    kLog.debug("LyricsService: Cached lyrics for {}", cacheKey);
  }
}

std::vector<LyricLine> LyricsService::parseLRC(const std::string& lrcText) {
  std::vector<LyricLine> lines;
  std::regex timestampRegex(R"(\[(\d{2}):(\d{2})\.(\d{2,3})\](.*))");

  std::istringstream stream(lrcText);
  std::string line;
  while (std::getline(stream, line)) {
    std::smatch match;
    if (std::regex_match(line, match, timestampRegex)) {
      int minutes = std::stoi(match[1]);
      int seconds = std::stoi(match[2]);
      int centiseconds = std::stoi(match[3]);
      std::string text = match[4];

      // Convert to microseconds
      std::int64_t timestampUs = (minutes * 60 + seconds) * 1000000LL + centiseconds * 10000LL;

      if (!text.empty()) {
        lines.push_back({timestampUs, text});
      }
    }
  }

  // Sort by timestamp
  std::sort(lines.begin(), lines.end(),
            [](const LyricLine& a, const LyricLine& b) {
              return a.timestampUs < b.timestampUs;
            });

  return lines;
}

std::vector<LyricLine> LyricsService::parsePlainLyrics(const std::string& text) {
  std::vector<LyricLine> lines;
  std::istringstream stream(text);
  std::string line;
  while (std::getline(stream, line)) {
    if (!line.empty()) {
      lines.push_back({-1, line});  // -1 = no timestamp
    }
  }
  return lines;
}

void LyricsService::fetchLyrics(const std::string& artist,
                                const std::string& title,
                                int durationSec,
                                std::function<void(std::optional<LyricsResult>)> callback) {
  if (m_fetching) {
    kLog.warn("LyricsService: Fetch already in progress, ignoring new request");
    callback(std::nullopt);
    return;
  }

  m_fetching = true;
  m_pendingCacheKey = makeCacheKey(artist, title, durationSec);

  // Check cache first
  if (auto cached = loadFromCache(m_pendingCacheKey)) {
    kLog.debug("LyricsService: Serving from cache");
    m_fetching = false;
    callback(*cached);
    return;
  }

  // Build LRCLIB API URL
  std::ostringstream urlStream;
  urlStream << "https://lrclib.net/api/get?"
            << "artist=" << curl_easy_escape(nullptr, artist.c_str(), static_cast<int>(artist.size()))
            << "&track=" << curl_easy_escape(nullptr, title.c_str(), static_cast<int>(title.size()))
            << "&duration=" << durationSec;

  std::string url = urlStream.str();
  kLog.debug("LyricsService: Fetching lyrics from LRCLIB: {}", url);

  // Fetch in background (simplified - in production use thread pool)
  auto response = fetchUrl(url);
  if (!response) {
    m_fetching = false;
    callback(std::nullopt);
    return;
  }

  try {
    nlohmann::json json = nlohmann::json::parse(*response);

    LyricsResult result;
    result.instrumental = json.value("instrumental", false);

    if (json.contains("syncedLyrics") && !json["syncedLyrics"].is_null() && !json["syncedLyrics"].get<std::string>().empty()) {
      result.isSynced = true;
      result.lines = parseLRC(json["syncedLyrics"].get<std::string>());
    } else if (json.contains("plainLyrics") && !json["plainLyrics"].is_null() && !json["plainLyrics"].get<std::string>().empty()) {
      result.isSynced = false;
      result.lines = parsePlainLyrics(json["plainLyrics"].get<std::string>());
    }

    // Cache the result
    saveToCache(m_pendingCacheKey, result);

    m_fetching = false;
    callback(result);
  } catch (const std::exception& e) {
    kLog.error("LyricsService: Failed to parse LRCLIB response: {}", e.what());
    m_fetching = false;
    callback(std::nullopt);
  }
}

void LyricsService::cancel() {
  m_fetching = false;
  m_pendingCacheKey.clear();
}

} // namespace noctalia::services