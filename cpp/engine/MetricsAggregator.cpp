#include "MetricsAggregator.hpp"

#include "EngineTunables.hpp"

#include <regex>

namespace reactnativecesium {

std::string MetricsAggregator::stripHtmlToPlain(const std::string& html) {
  if (html.empty()) return "";
  // The regexes are static so we pay the (very real) cost of compilation only
  // once per process. std::regex matches behaviour previously implemented twice
  // (NSRegularExpression on iOS, std::regex on Android) — both bridges now hit
  // this path so we cannot drift.
  static const std::regex tagRx("<[^>]+>");
  static const std::regex wsRx("\\s+");
  std::string plain = std::regex_replace(html, tagRx, " ");
  plain = std::regex_replace(plain, wsRx, " ");
  size_t start = plain.find_first_not_of(" \t\n\r");
  size_t end   = plain.find_last_not_of(" \t\n\r");
  if (start == std::string::npos) return "";
  return plain.substr(start, end - start + 1);
}

std::optional<MetricsSnapshot>
MetricsAggregator::tick(double dt, const FrameResult& frame, bool tlsConfigured) {
  const double instFps = (dt > 1e-6) ? (1.0 / dt) : 0.0;
  fpsEma_ = (fpsEma_ <= 1e-6) ? instFps : (fpsEma_ * 0.85 + instFps * 0.15);

  if (++metricsTick_ < tunables::kMetricsEmitEveryFrames) {
    return std::nullopt;
  }
  metricsTick_ = 0;

  latest_.fps                = fpsEma_;
  latest_.tilesRendered      = frame.tilesRendered;
  latest_.tilesLoading       = frame.tilesLoading;
  latest_.tilesVisited       = frame.tilesVisited;
  latest_.ionTokenConfigured = frame.ionTokenConfigured;
  latest_.tilesetReady       = frame.tilesetActive;
  latest_.tlsConfigured      = tlsConfigured;

  if (!frame.creditHtmlLines.empty()) {
    std::string joined;
    joined.reserve(256);
    for (const auto& html : frame.creditHtmlLines) {
      if (!joined.empty()) joined += "|";
      joined += html;
    }
    if (joined != lastCreditHtmlJoined_) {
      lastCreditHtmlJoined_ = std::move(joined);
      std::string credits;
      credits.reserve(256);
      for (const auto& html : frame.creditHtmlLines) {
        if (html.find("Error: Invalid Credit") != std::string::npos) continue;
        std::string chunk = stripHtmlToPlain(html);
        if (chunk.empty()) continue;
        if (!credits.empty()) credits += " \xC2\xB7 "; // " · "
        credits += chunk;
      }
      latest_.creditsPlainText = std::move(credits);
    }
  } else {
    lastCreditHtmlJoined_.clear();
    latest_.creditsPlainText.clear();
  }

  return latest_;
}

} // namespace reactnativecesium
