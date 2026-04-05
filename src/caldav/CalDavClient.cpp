#include "CalDavClient.h"

#include <HTTPClient.h>
#include <Logging.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_random.h>

#include <algorithm>
#include <cstring>
#include <ctime>

#include "CalDavCredentialStore.h"

namespace {

constexpr int HTTP_TIMEOUT_MS = 15000;
constexpr int MAX_RETRIES = 2;
constexpr int RETRY_DELAY_MS = 1000;
constexpr int MAX_RESPONSE_SIZE = 128 * 1024;  // 128KB — calendars can be larger than shopping lists
constexpr int MAX_SUMMARY_LEN = 128;
constexpr int MAX_LOCATION_LEN = 128;
constexpr int MAX_EVENTS = 500;

// Base64 encode a string into a stack buffer (max 256 chars output)
void base64Encode(const std::string& input, char* out, int outSize) {
  static constexpr char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  int outIdx = 0;
  int inLen = input.length();
  const uint8_t* in = reinterpret_cast<const uint8_t*>(input.c_str());
  int maxOut = outSize - 1;
  for (int i = 0; i < inLen && outIdx < maxOut - 3; i += 3) {
    uint32_t n = static_cast<uint32_t>(in[i]) << 16;
    if (i + 1 < inLen) n |= static_cast<uint32_t>(in[i + 1]) << 8;
    if (i + 2 < inLen) n |= static_cast<uint32_t>(in[i + 2]);
    out[outIdx++] = b64[(n >> 18) & 0x3F];
    out[outIdx++] = b64[(n >> 12) & 0x3F];
    out[outIdx++] = (i + 1 < inLen) ? b64[(n >> 6) & 0x3F] : '=';
    out[outIdx++] = (i + 2 < inLen) ? b64[n & 0x3F] : '=';
  }
  out[outIdx] = '\0';
}

std::string buildAuthHeader() {
  std::string authStr = CALDAV_STORE.getUsername() + ":" + CALDAV_STORE.getPassword();
  char authB64[256];
  base64Encode(authStr, authB64, sizeof(authB64));
  return std::string("Basic ") + authB64;
}

void beginAuthRequest(HTTPClient& http, std::unique_ptr<WiFiClientSecure>& secureClient, WiFiClient& plainClient,
                      const std::string& url) {
  bool isHttps = url.rfind("https://", 0) == 0;
  if (isHttps) {
    secureClient.reset(new WiFiClientSecure);
    secureClient->setInsecure();
    http.begin(*secureClient, url.c_str());
  } else {
    http.begin(plainClient, url.c_str());
  }
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.addHeader("Authorization", buildAuthHeader().c_str());
}

bool isWifiConnected() { return WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0); }

bool isTransientError(int httpCode) {
  return httpCode < 0 || httpCode == 500 || httpCode == 502 || httpCode == 503 || httpCode == 504;
}

// Parse "YYYYMMDD" or "YYYYMMDDTHHmmSS" from iCal DTSTART/DTEND values.
// Returns true on success.
bool parseICalDate(const char* val, int& year, int& month, int& day, int& hour, int& minute, bool& allDay) {
  // Strip VALUE=DATE: or VALUE=DATE-TIME: prefixes or TZID=...: prefixes
  const char* colon = strchr(val, ':');
  if (colon) val = colon + 1;

  int len = strlen(val);
  if (len < 8) return false;

  // Parse YYYYMMDD
  char buf[5];
  strncpy(buf, val, 4);
  buf[4] = '\0';
  year = atoi(buf);
  strncpy(buf, val + 4, 2);
  buf[2] = '\0';
  month = atoi(buf);
  strncpy(buf, val + 6, 2);
  buf[2] = '\0';
  day = atoi(buf);

  if (len >= 15 && val[8] == 'T') {
    // YYYYMMDDTHHmmSS
    strncpy(buf, val + 9, 2);
    buf[2] = '\0';
    hour = atoi(buf);
    strncpy(buf, val + 11, 2);
    buf[2] = '\0';
    minute = atoi(buf);
    allDay = false;
  } else {
    hour = 0;
    minute = 0;
    allDay = true;
  }

  return year > 0 && month >= 1 && month <= 12 && day >= 1 && day <= 31;
}

// Unfold iCal line continuations: lines starting with a space or tab are continuations
// of the previous line. We process in-place.
void unfoldICalLines(String& ical) {
  int writePos = 0;
  int len = ical.length();
  const char* data = ical.c_str();

  for (int i = 0; i < len; i++) {
    if (data[i] == '\r') continue;  // Skip CR
    if (data[i] == '\n' && i + 1 < len && (data[i + 1] == ' ' || data[i + 1] == '\t')) {
      i++;  // Skip the newline + whitespace (continuation)
      continue;
    }
    ical[writePos++] = data[i];
  }
  ical.remove(writePos);
}

// Convert a date to "days since epoch" for simple comparison
int dateToDays(int year, int month, int day) {
  // Simple Julian Day Number approximation — good enough for date range comparison
  int a = (14 - month) / 12;
  int y = year + 4800 - a;
  int m = month + 12 * a - 3;
  return day + (153 * m + 2) / 5 + 365 * y + y / 4 - y / 100 + y / 400 - 32045;
}

}  // namespace

bool CalendarEvent::operator<(const CalendarEvent& other) const {
  if (startYear != other.startYear) return startYear < other.startYear;
  if (startMonth != other.startMonth) return startMonth < other.startMonth;
  if (startDay != other.startDay) return startDay < other.startDay;
  if (allDay != other.allDay) return allDay;  // All-day events first
  if (startHour != other.startHour) return startHour < other.startHour;
  return startMinute < other.startMinute;
}

CalDavClient::Error CalDavClient::fetchEvents(std::vector<CalendarEvent>& outEvents, int daysBack, int daysForward) {
  if (!CALDAV_STORE.hasCredentials()) {
    LOG_DBG("CAL", "No credentials configured");
    return NO_CREDENTIALS;
  }

  if (!isWifiConnected()) {
    LOG_ERR("CAL", "WiFi not connected");
    return NETWORK_ERROR;
  }

  const std::string& url = CALDAV_STORE.getCalendarUrl();
  LOG_DBG("CAL", "Fetching calendar: %s", url.c_str());

  String responseBody;
  int lastCode = -1;

  for (int attempt = 0; attempt <= MAX_RETRIES; attempt++) {
    if (attempt > 0) {
      if (!isWifiConnected()) return NETWORK_ERROR;
      LOG_DBG("CAL", "Retry %d/%d", attempt, MAX_RETRIES);
      vTaskDelay(pdMS_TO_TICKS(RETRY_DELAY_MS));
    }

    HTTPClient http;
    std::unique_ptr<WiFiClientSecure> secureClient;
    WiFiClient plainClient;
    beginAuthRequest(http, secureClient, plainClient, url);

    lastCode = http.GET();

    if (lastCode == 200) {
      int len = http.getSize();
      if (len > MAX_RESPONSE_SIZE) {
        LOG_ERR("CAL", "Response too large: %d bytes", len);
        http.end();
        return SERVER_ERROR;
      }
      responseBody = http.getString();
      http.end();
      break;
    }

    http.end();

    if (lastCode == 401) return AUTH_FAILED;
    if (!isTransientError(lastCode)) {
      LOG_ERR("CAL", "Fetch failed: %d", lastCode);
      return (lastCode < 0) ? NETWORK_ERROR : SERVER_ERROR;
    }
    LOG_DBG("CAL", "Transient error: %d", lastCode);
  }

  if (lastCode != 200) {
    LOG_ERR("CAL", "All retries failed: %d", lastCode);
    return (lastCode < 0) ? NETWORK_ERROR : SERVER_ERROR;
  }

  // Unfold continuation lines
  unfoldICalLines(responseBody);

  // Get current date from ESP32 RTC (set by SNTP or manual)
  struct tm now;
  time_t nowEpoch = time(nullptr);
  localtime_r(&nowEpoch, &now);
  int todayYear = now.tm_year + 1900;
  int todayMonth = now.tm_mon + 1;
  int todayDay = now.tm_mday;
  int todayDays = dateToDays(todayYear, todayMonth, todayDay);
  int startDays = todayDays - daysBack;
  int endDays = todayDays + daysForward;

  LOG_DBG("CAL", "Filtering events: %d days back, %d days forward", daysBack, daysForward);

  // Parse VEVENT blocks line by line
  outEvents.clear();
  outEvents.reserve(32);

  bool inEvent = false;
  bool hasRRule = false;
  CalendarEvent current = {};
  bool hasDtStart = false;

  int lineStart = 0;
  int bodyLen = responseBody.length();
  const char* body = responseBody.c_str();

  for (int i = 0; i <= bodyLen; i++) {
    if (i < bodyLen && body[i] != '\n') continue;

    // Process line [lineStart, i)
    int lineEnd = i;
    if (lineEnd > lineStart && body[lineEnd - 1] == '\r') lineEnd--;
    int lineLen = lineEnd - lineStart;

    if (lineLen >= 12 && strncmp(body + lineStart, "BEGIN:VEVENT", 12) == 0) {
      inEvent = true;
      hasRRule = false;
      hasDtStart = false;
      current = {};
    } else if (inEvent && lineLen >= 10 && strncmp(body + lineStart, "END:VEVENT", 10) == 0) {
      inEvent = false;

      // Skip recurring events in v1
      if (hasRRule) {
        lineStart = i + 1;
        continue;
      }

      if (hasDtStart) {
        int eventDays = dateToDays(current.startYear, current.startMonth, current.startDay);
        if (eventDays >= startDays && eventDays < endDays && static_cast<int>(outEvents.size()) < MAX_EVENTS) {
          outEvents.push_back(std::move(current));
        }
      }
    } else if (inEvent) {
      // Extract properties (truncate long values to prevent memory bloat)
      if (lineLen > 8 && strncmp(body + lineStart, "SUMMARY:", 8) == 0) {
        int len = std::min(lineLen - 8, MAX_SUMMARY_LEN);
        current.summary.assign(body + lineStart + 8, len);
      } else if (lineLen > 7 && strncmp(body + lineStart, "SUMMARY", 7) == 0) {
        // SUMMARY;LANGUAGE=...:value
        const char* colonPos = static_cast<const char*>(memchr(body + lineStart, ':', lineLen));
        if (colonPos) {
          int valStart = colonPos - body + 1;
          int len = std::min(lineEnd - valStart, MAX_SUMMARY_LEN);
          current.summary.assign(body + valStart, len);
        }
      } else if (lineLen > 9 && strncmp(body + lineStart, "LOCATION:", 9) == 0) {
        int len = std::min(lineLen - 9, MAX_LOCATION_LEN);
        current.location.assign(body + lineStart + 9, len);
      } else if (lineLen > 8 && strncmp(body + lineStart, "LOCATION", 8) == 0) {
        const char* colonPos = static_cast<const char*>(memchr(body + lineStart, ':', lineLen));
        if (colonPos) {
          int valStart = colonPos - body + 1;
          int len = std::min(lineEnd - valStart, MAX_LOCATION_LEN);
          current.location.assign(body + valStart, len);
        }
      } else if (lineLen > 7 && strncmp(body + lineStart, "DTSTART", 7) == 0) {
        // Could be DTSTART:20260405T090000Z or DTSTART;VALUE=DATE:20260405 or DTSTART;TZID=...:...
        const char* colonPos = static_cast<const char*>(memchr(body + lineStart, ':', lineLen));
        if (colonPos) {
          hasDtStart = parseICalDate(body + lineStart + 7, current.startYear, current.startMonth, current.startDay,
                                     current.startHour, current.startMinute, current.allDay);
        }
      } else if (lineLen >= 6 && strncmp(body + lineStart, "RRULE:", 6) == 0) {
        hasRRule = true;
      }
    }

    lineStart = i + 1;
  }

  std::sort(outEvents.begin(), outEvents.end());

  LOG_DBG("CAL", "Parsed %zu events in date range", outEvents.size());
  return OK;
}

CalDavClient::Error CalDavClient::putEvent(int year, int month, int day, int hour, int minute, const char* summary) {
  if (!CALDAV_STORE.hasCredentials()) return NO_CREDENTIALS;
  if (!isWifiConnected()) return NETWORK_ERROR;

  // Generate a unique UID from random bytes
  char uid[48];
  uint32_t r1 = esp_random();
  uint32_t r2 = esp_random();
  uint32_t r3 = esp_random();
  snprintf(uid, sizeof(uid), "%08x-%08x-%08x@crosspoint", r1, r2, r3);

  // Sanitize summary: strip iCal-unsafe chars, truncate to 64 chars
  char safeSummary[65];
  int si = 0;
  for (int i = 0; summary[i] != '\0' && si < 64; i++) {
    char c = summary[i];
    if (c == '\n' || c == '\r') continue;             // No line breaks in SUMMARY
    if (c == '\\' || c == ';' || c == ',') continue;  // iCal special chars
    safeSummary[si++] = c;
  }
  safeSummary[si] = '\0';
  if (si == 0) strncpy(safeSummary, "Meeting", sizeof(safeSummary));

  // Compute DTEND: 1 hour after DTSTART, handle midnight wrap
  int endHour = hour + 1;
  int endDay = day, endMonth = month, endYear = year;
  if (endHour >= 24) {
    endHour = 0;
    endDay++;
    // Simple month-end wrap (good enough for event creation)
    static constexpr int dim[] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int maxDay = dim[endMonth];
    if (endMonth == 2 && (endYear % 4 == 0 && (endYear % 100 != 0 || endYear % 400 == 0))) maxDay = 29;
    if (endDay > maxDay) {
      endDay = 1;
      endMonth++;
      if (endMonth > 12) {
        endMonth = 1;
        endYear++;
      }
    }
  }

  // Build minimal VCALENDAR
  char icsBody[512];
  int written = snprintf(icsBody, sizeof(icsBody),
                         "BEGIN:VCALENDAR\r\n"
                         "VERSION:2.0\r\n"
                         "PRODID:-//CrossPoint//Reader//EN\r\n"
                         "BEGIN:VEVENT\r\n"
                         "UID:%s\r\n"
                         "DTSTART:%04d%02d%02dT%02d%02d00\r\n"
                         "DTEND:%04d%02d%02dT%02d%02d00\r\n"
                         "SUMMARY:%s\r\n"
                         "END:VEVENT\r\n"
                         "END:VCALENDAR\r\n",
                         uid, year, month, day, hour, minute, endYear, endMonth, endDay, endHour, minute, safeSummary);
  if (written < 0 || written >= static_cast<int>(sizeof(icsBody))) {
    LOG_ERR("CAL", "iCal body too large");
    return PARSE_ERROR;
  }

  // PUT to calendarUrl + uid.ics
  std::string putUrl = CALDAV_STORE.getCalendarUrl();
  if (!putUrl.empty() && putUrl.back() != '/') putUrl += '/';
  putUrl += uid;
  putUrl += ".ics";

  LOG_DBG("CAL", "PUT event to: %s", putUrl.c_str());

  int lastCode = -1;
  for (int attempt = 0; attempt <= MAX_RETRIES; attempt++) {
    if (attempt > 0) {
      if (!isWifiConnected()) return NETWORK_ERROR;
      vTaskDelay(pdMS_TO_TICKS(RETRY_DELAY_MS));
    }

    HTTPClient http;
    std::unique_ptr<WiFiClientSecure> secureClient;
    WiFiClient plainClient;
    beginAuthRequest(http, secureClient, plainClient, putUrl);
    http.addHeader("Content-Type", "text/calendar; charset=utf-8");
    http.addHeader("If-None-Match", "*");

    lastCode = http.PUT(reinterpret_cast<uint8_t*>(icsBody), strlen(icsBody));
    http.end();

    if (lastCode == 201 || lastCode == 204) {
      LOG_DBG("CAL", "Event created: %d", lastCode);
      return OK;
    }
    if (lastCode == 401) return AUTH_FAILED;
    if (!isTransientError(lastCode)) {
      LOG_ERR("CAL", "PUT failed: %d", lastCode);
      return (lastCode < 0) ? NETWORK_ERROR : SERVER_ERROR;
    }
  }

  LOG_ERR("CAL", "PUT retries exhausted: %d", lastCode);
  return (lastCode < 0) ? NETWORK_ERROR : SERVER_ERROR;
}

const char* CalDavClient::errorString(Error error) {
  switch (error) {
    case OK:
      return "Success";
    case NO_CREDENTIALS:
      return "No CalDAV credentials configured";
    case NETWORK_ERROR:
      return "Network error";
    case AUTH_FAILED:
      return "Authentication failed (check username/password)";
    case SERVER_ERROR:
      return "Server error";
    case PARSE_ERROR:
      return "Calendar parse error";
    default:
      return "Unknown error";
  }
}
