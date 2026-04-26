#include "CalDavClient.h"

#include <HTTPClient.h>
#include <Logging.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_random.h>
#include <esp_sntp.h>

#include <algorithm>
#include <cstring>
#include <ctime>
#include <vector>

#include "CalDavCredentialStore.h"

namespace {

constexpr int HTTP_TIMEOUT_MS = 15000;
constexpr int MAX_RETRIES = 2;
constexpr int RETRY_DELAY_MS = 1000;
constexpr int MAX_RESPONSE_SIZE = 512 * 1024;
constexpr int MAX_SUMMARY_LEN = 128;
constexpr int MAX_LOCATION_LEN = 128;
constexpr int MAX_EVENTS = 1000;

struct ICalDateTime {
  int year = 0;
  int month = 0;
  int day = 0;
  int hour = 0;
  int minute = 0;
  bool allDay = false;
};

struct ParsedEvent {
  CalendarEvent event = {};
  std::string uid;
  std::string rrule;
  bool hasDtStart = false;
  bool hasDtEnd = false;
  bool isException = false;
  ICalDateTime recurrenceId = {};
};

enum class Frequency { NONE, DAILY, WEEKLY, MONTHLY, YEARLY };

struct RecurrenceRule {
  Frequency freq = Frequency::NONE;
  int interval = 1;
  int count = 0;
  bool hasUntil = false;
  ICalDateTime until = {};
  int byMonth = 0;
  int bySetPos = 0;
  std::vector<int> byDays;
};

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

void ensureNtpTime() {
  time_t now = time(nullptr);
  if (now > 1700000000) return;  // Already have a reasonable time (post-2023)

  LOG_DBG("CAL", "RTC not set, syncing NTP...");
  if (esp_sntp_enabled()) esp_sntp_stop();
  esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
  esp_sntp_setservername(0, "pool.ntp.org");
  esp_sntp_init();

  for (int retry = 0; retry < 50; retry++) {  // 5s max
    vTaskDelay(pdMS_TO_TICKS(100));
    if (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
      LOG_DBG("CAL", "NTP synced");
      return;
    }
  }
  LOG_ERR("CAL", "NTP sync timeout");
}

bool isTransientError(int httpCode) {
  return httpCode < 0 || httpCode == 500 || httpCode == 502 || httpCode == 503 || httpCode == 504;
}

std::string normalizeCalendarCollectionUrl(const std::string& rawUrl) {
  if (rawUrl.empty()) return rawUrl;
  if (rawUrl.back() == '/') return rawUrl;
  return rawUrl + "/";
}

std::string extractBaseOrigin(const std::string& url) {
  const size_t schemeEnd = url.find("://");
  if (schemeEnd == std::string::npos) return "";
  const size_t hostStart = schemeEnd + 3;
  const size_t pathStart = url.find('/', hostStart);
  if (pathStart == std::string::npos) return url;
  return url.substr(0, pathStart);
}

std::string absoluteUrlForHref(const std::string& collectionUrl, const std::string& href) {
  if (href.rfind("http://", 0) == 0 || href.rfind("https://", 0) == 0) return href;
  const std::string baseOrigin = extractBaseOrigin(collectionUrl);
  if (!href.empty() && href.front() == '/') return baseOrigin + href;
  return normalizeCalendarCollectionUrl(collectionUrl) + href;
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

void daysToDate(int days, int& year, int& month, int& day) {
  int a = days + 32044;
  int b = (4 * a + 3) / 146097;
  int c = a - (146097 * b) / 4;
  int d = (4 * c + 3) / 1461;
  int e = c - (1461 * d) / 4;
  int m = (5 * e + 2) / 153;
  day = e - (153 * m + 2) / 5 + 1;
  month = m + 3 - 12 * (m / 10);
  year = 100 * b + d - 4800 + m / 10;
}

bool isLeapYear(int year) { return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0); }

int daysInMonth(int year, int month) {
  static constexpr int dim[] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (month == 2 && isLeapYear(year)) return 29;
  return dim[month];
}

void addDays(int& year, int& month, int& day, int delta) {
  int days = dateToDays(year, month, day) + delta;
  daysToDate(days, year, month, day);
}

void addMinutes(int& year, int& month, int& day, int& hour, int& minute, int deltaMinutes) {
  int totalMinutes = hour * 60 + minute + deltaMinutes;
  while (totalMinutes < 0) {
    totalMinutes += 24 * 60;
    addDays(year, month, day, -1);
  }
  while (totalMinutes >= 24 * 60) {
    totalMinutes -= 24 * 60;
    addDays(year, month, day, 1);
  }
  hour = totalMinutes / 60;
  minute = totalMinutes % 60;
}

int dayOfWeekMonday0(int year, int month, int day) {
  static constexpr int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
  int y = year;
  if (month < 3) y--;
  int dow = (y + y / 4 - y / 100 + y / 400 + t[month - 1] + day) % 7;
  return (dow + 6) % 7;
}

int lastSundayOfMonth(int year, int month) {
  const int lastDay = daysInMonth(year, month);
  const int dow = dayOfWeekMonday0(year, month, lastDay);
  return lastDay - ((dow + 1) % 7);
}

bool isEuropeBrusselsDstUtc(int year, int month, int day, int hour, int minute) {
  const int marchSwitchDay = lastSundayOfMonth(year, 3);
  const int octoberSwitchDay = lastSundayOfMonth(year, 10);

  if (month < 3 || month > 10) return false;
  if (month > 3 && month < 10) return true;

  const int minutes = hour * 60 + minute;
  if (month == 3) {
    if (day > marchSwitchDay) return true;
    if (day < marchSwitchDay) return false;
    return minutes >= 60;
  }

  if (day < octoberSwitchDay) return true;
  if (day > octoberSwitchDay) return false;
  return minutes < 60;
}

bool parseWeekdayToken(const std::string& token, int& weekday) {
  if (token == "MO")
    weekday = 0;
  else if (token == "TU")
    weekday = 1;
  else if (token == "WE")
    weekday = 2;
  else if (token == "TH")
    weekday = 3;
  else if (token == "FR")
    weekday = 4;
  else if (token == "SA")
    weekday = 5;
  else if (token == "SU")
    weekday = 6;
  else
    return false;
  return true;
}

bool parsePropertyValue(const char* val, ICalDateTime& out) {
  const char* colon = strchr(val, ':');
  if (colon) val = colon + 1;

  // Find length up to next newline or null terminator (val may point into a larger buffer)
  int len = 0;
  while (val[len] != '\0' && val[len] != '\n' && val[len] != '\r') len++;
  if (len < 8) return false;

  char buf[5];
  strncpy(buf, val, 4);
  buf[4] = '\0';
  out.year = atoi(buf);
  strncpy(buf, val + 4, 2);
  buf[2] = '\0';
  out.month = atoi(buf);
  strncpy(buf, val + 6, 2);
  buf[2] = '\0';
  out.day = atoi(buf);

  out.hour = 0;
  out.minute = 0;
  out.allDay = true;

  if (len >= 13 && val[8] == 'T') {
    strncpy(buf, val + 9, 2);
    buf[2] = '\0';
    out.hour = atoi(buf);
    strncpy(buf, val + 11, 2);
    buf[2] = '\0';
    out.minute = atoi(buf);
    out.allDay = false;

    if (val[len - 1] == 'Z') {
      const int offsetMinutes = isEuropeBrusselsDstUtc(out.year, out.month, out.day, out.hour, out.minute) ? 120 : 60;
      addMinutes(out.year, out.month, out.day, out.hour, out.minute, offsetMinutes);
    }
  }

  return out.year > 0 && out.month >= 1 && out.month <= 12 && out.day >= 1 && out.day <= 31;
}

std::string extractPropertyValue(const char* line, int lineLen, int propertyNameLen, int maxLen) {
  if (lineLen <= propertyNameLen) return "";
  if (line[propertyNameLen] == ':') {
    return std::string(line + propertyNameLen + 1, std::min(lineLen - propertyNameLen - 1, maxLen));
  }

  const char* colonPos = static_cast<const char*>(memchr(line, ':', lineLen));
  if (!colonPos) return "";

  const int valueStart = static_cast<int>(colonPos - line) + 1;
  return std::string(line + valueStart, std::min(lineLen - valueStart, maxLen));
}

void normalizeEventRange(ParsedEvent& parsed) {
  if (!parsed.hasDtStart) return;

  if (!parsed.hasDtEnd) {
    parsed.event.endYear = parsed.event.startYear;
    parsed.event.endMonth = parsed.event.startMonth;
    parsed.event.endDay = parsed.event.startDay;
    parsed.event.endHour = parsed.event.startHour;
    parsed.event.endMinute = parsed.event.startMinute;
    return;
  }

  if (parsed.event.allDay) {
    addDays(parsed.event.endYear, parsed.event.endMonth, parsed.event.endDay, -1);
    parsed.event.endHour = 23;
    parsed.event.endMinute = 59;
    return;
  }

  if (parsed.event.endHour == 0 && parsed.event.endMinute == 0 &&
      dateToDays(parsed.event.endYear, parsed.event.endMonth, parsed.event.endDay) >
          dateToDays(parsed.event.startYear, parsed.event.startMonth, parsed.event.startDay)) {
    addDays(parsed.event.endYear, parsed.event.endMonth, parsed.event.endDay, -1);
  }
}

bool parseRRule(const std::string& rruleString, RecurrenceRule& rule) {
  size_t start = 0;
  while (start < rruleString.size()) {
    size_t end = rruleString.find(';', start);
    if (end == std::string::npos) end = rruleString.size();
    const std::string part = rruleString.substr(start, end - start);
    size_t eq = part.find('=');
    if (eq != std::string::npos) {
      const std::string key = part.substr(0, eq);
      const std::string value = part.substr(eq + 1);
      if (key == "FREQ") {
        if (value == "DAILY")
          rule.freq = Frequency::DAILY;
        else if (value == "WEEKLY")
          rule.freq = Frequency::WEEKLY;
        else if (value == "MONTHLY")
          rule.freq = Frequency::MONTHLY;
        else if (value == "YEARLY")
          rule.freq = Frequency::YEARLY;
      } else if (key == "INTERVAL") {
        const int parsed = atoi(value.c_str());
        rule.interval = parsed > 0 ? parsed : 1;
      } else if (key == "COUNT") {
        rule.count = atoi(value.c_str());
      } else if (key == "UNTIL") {
        rule.hasUntil = parsePropertyValue(value.c_str(), rule.until);
      } else if (key == "BYMONTH") {
        rule.byMonth = atoi(value.c_str());
      } else if (key == "BYSETPOS") {
        rule.bySetPos = atoi(value.c_str());
      } else if (key == "BYDAY") {
        size_t dayStart = 0;
        while (dayStart < value.size()) {
          size_t dayEnd = value.find(',', dayStart);
          if (dayEnd == std::string::npos) dayEnd = value.size();
          int weekday = 0;
          if (parseWeekdayToken(value.substr(dayStart, dayEnd - dayStart), weekday)) {
            rule.byDays.push_back(weekday);
          }
          dayStart = dayEnd + 1;
        }
      }
    }
    start = end + 1;
  }

  return rule.freq != Frequency::NONE;
}

std::string occurrenceKey(const std::string& uid, int year, int month, int day, int hour, int minute, bool allDay) {
  char buffer[96];
  snprintf(buffer, sizeof(buffer), "%s|%04d%02d%02d|%02d%02d|%d", uid.c_str(), year, month, day, hour, minute, allDay);
  return buffer;
}

bool intersectsWindow(const CalendarEvent& event, int startDays, int endDaysExclusive) {
  const int eventStart = dateToDays(event.startYear, event.startMonth, event.startDay);
  const int eventEnd = dateToDays(event.endYear, event.endMonth, event.endDay);
  return eventStart < endDaysExclusive && eventEnd >= startDays;
}

int nthWeekdayOfMonth(int year, int month, int weekday, int nth) {
  if (nth == 0) return 0;
  if (nth > 0) {
    int day = 1;
    while (dayOfWeekMonday0(year, month, day) != weekday) day++;
    day += (nth - 1) * 7;
    return day <= daysInMonth(year, month) ? day : 0;
  }

  int day = daysInMonth(year, month);
  while (dayOfWeekMonday0(year, month, day) != weekday) day--;
  day += (nth + 1) * 7;
  return day >= 1 ? day : 0;
}

void appendOccurrence(std::vector<CalendarEvent>& outEvents, const CalendarEvent& baseEvent, const std::string& uid,
                      int year, int month, int day, const std::vector<std::string>& overrides, int startDays,
                      int endDaysExclusive) {
  const std::string key =
      occurrenceKey(uid, year, month, day, baseEvent.startHour, baseEvent.startMinute, baseEvent.allDay);
  if (std::find(overrides.begin(), overrides.end(), key) != overrides.end()) return;
  if (static_cast<int>(outEvents.size()) >= MAX_EVENTS) return;

  CalendarEvent occurrence = baseEvent;
  const int deltaDays =
      dateToDays(year, month, day) - dateToDays(baseEvent.startYear, baseEvent.startMonth, baseEvent.startDay);
  occurrence.startYear = year;
  occurrence.startMonth = month;
  occurrence.startDay = day;
  occurrence.endYear = baseEvent.endYear;
  occurrence.endMonth = baseEvent.endMonth;
  occurrence.endDay = baseEvent.endDay;
  addDays(occurrence.endYear, occurrence.endMonth, occurrence.endDay, deltaDays);

  if (intersectsWindow(occurrence, startDays, endDaysExclusive)) {
    outEvents.push_back(std::move(occurrence));
  }
}

void expandRecurringEvent(const ParsedEvent& parsed, const std::vector<std::string>& overrides, int startDays,
                          int endDaysExclusive, std::vector<CalendarEvent>& outEvents) {
  RecurrenceRule rule;
  if (!parseRRule(parsed.rrule, rule)) return;

  const int dtStartDays = dateToDays(parsed.event.startYear, parsed.event.startMonth, parsed.event.startDay);
  const int untilDays = rule.hasUntil ? dateToDays(rule.until.year, rule.until.month, rule.until.day) : 0;
  int produced = 0;

  auto shouldStop = [&](int year, int month, int day) {
    if (rule.count > 0 && produced >= rule.count) return true;
    if (rule.hasUntil && dateToDays(year, month, day) > untilDays) return true;
    return false;
  };

  if (rule.freq == Frequency::YEARLY) {
    for (int year = parsed.event.startYear; year <= parsed.event.startYear + 20; year += rule.interval) {
      int month = rule.byMonth > 0 ? rule.byMonth : parsed.event.startMonth;
      int day = parsed.event.startDay;
      if (month == parsed.event.startMonth && year == parsed.event.startYear) {
        if (!shouldStop(year, month, day)) {
          appendOccurrence(outEvents, parsed.event, parsed.uid, year, month, day, overrides, startDays,
                           endDaysExclusive);
          produced++;
        }
        continue;
      }
      if (day > daysInMonth(year, month) || shouldStop(year, month, day)) break;
      appendOccurrence(outEvents, parsed.event, parsed.uid, year, month, day, overrides, startDays, endDaysExclusive);
      produced++;
      if (dateToDays(year, month, day) >= endDaysExclusive) break;
    }
    return;
  }

  if (rule.freq == Frequency::MONTHLY && !rule.byDays.empty() && rule.bySetPos != 0) {
    int year = parsed.event.startYear;
    int month = parsed.event.startMonth;
    while (true) {
      int day = nthWeekdayOfMonth(year, month, rule.byDays.front(), rule.bySetPos);
      if (day > 0 && dateToDays(year, month, day) >= dtStartDays) {
        if (shouldStop(year, month, day)) break;
        appendOccurrence(outEvents, parsed.event, parsed.uid, year, month, day, overrides, startDays, endDaysExclusive);
        produced++;
        if (dateToDays(year, month, day) >= endDaysExclusive) break;
      }

      month += rule.interval;
      while (month > 12) {
        month -= 12;
        year++;
      }
      if (year > parsed.event.startYear + 10) break;
    }
    return;
  }

  if (rule.freq == Frequency::WEEKLY) {
    std::vector<int> weekdays = rule.byDays;
    if (weekdays.empty()) {
      weekdays.push_back(dayOfWeekMonday0(parsed.event.startYear, parsed.event.startMonth, parsed.event.startDay));
    }

    const int baseWeekStart =
        dtStartDays - dayOfWeekMonday0(parsed.event.startYear, parsed.event.startMonth, parsed.event.startDay);
    for (int weekStart = baseWeekStart; weekStart < endDaysExclusive + 7; weekStart += rule.interval * 7) {
      for (int weekday : weekdays) {
        int occurrenceDays = weekStart + weekday;
        if (occurrenceDays < dtStartDays) continue;
        int year, month, day;
        daysToDate(occurrenceDays, year, month, day);
        if (shouldStop(year, month, day)) return;
        appendOccurrence(outEvents, parsed.event, parsed.uid, year, month, day, overrides, startDays, endDaysExclusive);
        produced++;
        if (rule.count > 0 && produced >= rule.count) return;
      }
    }
    return;
  }

  if (rule.freq == Frequency::DAILY) {
    int year = parsed.event.startYear;
    int month = parsed.event.startMonth;
    int day = parsed.event.startDay;
    while (!shouldStop(year, month, day) && dateToDays(year, month, day) < endDaysExclusive) {
      appendOccurrence(outEvents, parsed.event, parsed.uid, year, month, day, overrides, startDays, endDaysExclusive);
      produced++;
      addDays(year, month, day, rule.interval);
    }
    return;
  }

  if (rule.freq == Frequency::MONTHLY) {
    int year = parsed.event.startYear;
    int month = parsed.event.startMonth;
    const int originalDay = parsed.event.startDay;
    while (true) {
      if (originalDay <= daysInMonth(year, month)) {
        if (shouldStop(year, month, originalDay)) break;
        appendOccurrence(outEvents, parsed.event, parsed.uid, year, month, originalDay, overrides, startDays,
                         endDaysExclusive);
        produced++;
        if (dateToDays(year, month, originalDay) >= endDaysExclusive) break;
      }

      month += rule.interval;
      while (month > 12) {
        month -= 12;
        year++;
      }
      if (year > parsed.event.startYear + 10) break;
    }
  }
}

std::string buildCalendarQueryXml(int startDays, int endDaysExclusive) {
  int startYear, startMonth, startDay;
  int endYear, endMonth, endDay;
  daysToDate(startDays - 1, startYear, startMonth, startDay);
  daysToDate(endDaysExclusive + 1, endYear, endMonth, endDay);

  char request[512];
  snprintf(request, sizeof(request),
           "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
           "<c:calendar-query xmlns:d=\"DAV:\" xmlns:c=\"urn:ietf:params:xml:ns:caldav\">"
           "<d:prop>"
           "<d:getetag/>"
           "<c:calendar-data>"
           "<c:expand start=\"%04d%02d%02dT000000Z\" end=\"%04d%02d%02dT000000Z\"/>"
           "</c:calendar-data>"
           "</d:prop>"
           "<c:filter>"
           "<c:comp-filter name=\"VCALENDAR\">"
           "<c:comp-filter name=\"VEVENT\">"
           "<c:time-range start=\"%04d%02d%02dT000000Z\" end=\"%04d%02d%02dT000000Z\"/>"
           "</c:comp-filter>"
           "</c:comp-filter>"
           "</c:filter>"
           "</c:calendar-query>",
           startYear, startMonth, startDay, endYear, endMonth, endDay, startYear, startMonth, startDay, endYear,
           endMonth, endDay);
  return request;
}

void parseCalendarDataFragment(const char* rawData, int rawLen, std::vector<ParsedEvent>& parsedEvents) {
  if (!rawData || rawLen <= 0) return;

  // Copy into a mutable String for in-place unfolding
  String unfolded;
  if (!unfolded.reserve(rawLen)) {
    LOG_ERR("CAL", "Failed to allocate %d bytes for iCal fragment", rawLen);
    return;
  }
  unfolded.concat(rawData, rawLen);
  unfoldICalLines(unfolded);

  ParsedEvent current = {};
  bool inEvent = false;
  int lineStart = 0;
  const int bodyLen = unfolded.length();
  const char* body = unfolded.c_str();

  for (int i = 0; i <= bodyLen; i++) {
    if (i < bodyLen && body[i] != '\n') continue;

    int lineEnd = i;
    if (lineEnd > lineStart && body[lineEnd - 1] == '\r') lineEnd--;
    const int lineLen = lineEnd - lineStart;

    if (lineLen >= 12 && strncmp(body + lineStart, "BEGIN:VEVENT", 12) == 0) {
      inEvent = true;
      current = {};
    } else if (inEvent && lineLen >= 10 && strncmp(body + lineStart, "END:VEVENT", 10) == 0) {
      inEvent = false;
      if (current.hasDtStart && static_cast<int>(parsedEvents.size()) < MAX_EVENTS * 2) {
        normalizeEventRange(current);
        parsedEvents.push_back(std::move(current));
      }
    } else if (inEvent) {
      if (lineLen > 7 && strncmp(body + lineStart, "SUMMARY", 7) == 0) {
        current.event.summary = extractPropertyValue(body + lineStart, lineLen, 7, MAX_SUMMARY_LEN);
      } else if (lineLen > 8 && strncmp(body + lineStart, "LOCATION", 8) == 0) {
        current.event.location = extractPropertyValue(body + lineStart, lineLen, 8, MAX_LOCATION_LEN);
      } else if (lineLen > 7 && strncmp(body + lineStart, "DTSTART", 7) == 0) {
        ICalDateTime dtStart;
        if (parsePropertyValue(body + lineStart + 7, dtStart)) {
          current.event.startYear = dtStart.year;
          current.event.startMonth = dtStart.month;
          current.event.startDay = dtStart.day;
          current.event.startHour = dtStart.hour;
          current.event.startMinute = dtStart.minute;
          current.event.allDay = dtStart.allDay;
          current.hasDtStart = true;
        }
      } else if (lineLen > 5 && strncmp(body + lineStart, "DTEND", 5) == 0) {
        ICalDateTime dtEnd;
        if (parsePropertyValue(body + lineStart + 5, dtEnd)) {
          current.event.endYear = dtEnd.year;
          current.event.endMonth = dtEnd.month;
          current.event.endDay = dtEnd.day;
          current.event.endHour = dtEnd.hour;
          current.event.endMinute = dtEnd.minute;
          current.hasDtEnd = true;
        }
      } else if (lineLen >= 4 && strncmp(body + lineStart, "UID:", 4) == 0) {
        current.uid = extractPropertyValue(body + lineStart, lineLen, 3, 96);
      }
    }

    lineStart = i + 1;
  }
}

// Check if the tag name (between < and >) ends with the given suffix.
// Works with raw C pointers to avoid Arduino String heap allocations.
bool tagEndsWith(const char* tagStart, int tagLen, const char* suffix, int suffixLen) {
  if (tagLen < suffixLen) return false;
  return memcmp(tagStart + tagLen - suffixLen, suffix, suffixLen) == 0;
}

bool parseCalendarQueryResponse(const String& responseBody, std::vector<ParsedEvent>& parsedEvents) {
  const char* data = responseBody.c_str();
  const int dataLen = responseBody.length();
  if (dataLen == 0) {
    LOG_ERR("CAL", "Empty response body");
    return false;
  }
  LOG_DBG("CAL", "Parsing %d byte response", dataLen);

  static constexpr char CALENDAR_DATA[] = "calendar-data";
  static constexpr int CD_LEN = 13;  // strlen("calendar-data")

  int pos = 0;
  while (pos < dataLen) {
    // Find next '<'
    const char* openAngle = static_cast<const char*>(memchr(data + pos, '<', dataLen - pos));
    if (!openAngle) break;
    int openTagStart = static_cast<int>(openAngle - data);

    // Find matching '>'
    const char* closeAngle = static_cast<const char*>(memchr(data + openTagStart, '>', dataLen - openTagStart));
    if (!closeAngle) break;
    int openTagEnd = static_cast<int>(closeAngle - data);

    // Tag content is between '<' and '>'
    const char* tagContent = data + openTagStart + 1;
    int tagLen = openTagEnd - openTagStart - 1;

    // Check if this tag ends with "calendar-data" (handles any namespace prefix)
    if (!tagEndsWith(tagContent, tagLen, CALENDAR_DATA, CD_LEN)) {
      pos = openTagEnd + 1;
      continue;
    }

    // Skip closing tags (start with '/')
    if (tagLen > 0 && tagContent[0] == '/') {
      pos = openTagEnd + 1;
      continue;
    }

    // Found an opening calendar-data tag. Find the matching close tag.
    const char* contentStart = data + openTagEnd + 1;
    int remaining = dataLen - (openTagEnd + 1);

    // Search for "</...calendar-data>"
    const char* searchPtr = contentStart;
    int searchLen = remaining;
    int closeTagStartPos = -1;
    int closeTagEndPos = -1;

    while (searchLen > 0) {
      const char* lt = static_cast<const char*>(memchr(searchPtr, '<', searchLen));
      if (!lt) break;
      int ltPos = static_cast<int>(lt - data);

      // Check if it's a closing tag starting with "</"
      if (ltPos + 1 < dataLen && data[ltPos + 1] == '/') {
        const char* gt = static_cast<const char*>(memchr(lt, '>', dataLen - ltPos));
        if (!gt) break;
        int gtPos = static_cast<int>(gt - data);

        // Check if this closing tag ends with "calendar-data"
        const char* closeTagContent = data + ltPos + 2;  // skip "</"
        int closeTagLen = gtPos - ltPos - 2;
        if (tagEndsWith(closeTagContent, closeTagLen, CALENDAR_DATA, CD_LEN)) {
          closeTagStartPos = ltPos;
          closeTagEndPos = gtPos;
          break;
        }

        searchPtr = gt + 1;
        searchLen = dataLen - (gtPos + 1);
      } else {
        searchPtr = lt + 1;
        searchLen = dataLen - (ltPos + 1);
      }
    }

    if (closeTagStartPos < 0) break;

    // Pass raw pointer range directly — no intermediate String allocation needed.
    int fragmentLen = closeTagStartPos - (openTagEnd + 1);
    parseCalendarDataFragment(contentStart, fragmentLen, parsedEvents);

    pos = closeTagEndPos + 1;
  }

  LOG_DBG("CAL", "Found %zu events in response", parsedEvents.size());
  return !parsedEvents.empty();
}

void collectResponseHrefs(const String& responseBody, std::vector<std::string>& hrefs) {
  const char* data = responseBody.c_str();
  const int dataLen = responseBody.length();
  const char* pos = data;

  while (pos < data + dataLen) {
    const char* openTag = strstr(pos, "<href>");
    if (!openTag) break;
    const char* contentStart = openTag + 6;
    const char* closeTag = strstr(contentStart, "</href>");
    if (!closeTag) break;

    // Trim whitespace from href content
    while (contentStart < closeTag && (*contentStart == ' ' || *contentStart == '\t' || *contentStart == '\n' ||
                                       *contentStart == '\r'))
      contentStart++;
    const char* contentEnd = closeTag;
    while (contentEnd > contentStart && (contentEnd[-1] == ' ' || contentEnd[-1] == '\t' || contentEnd[-1] == '\n' ||
                                         contentEnd[-1] == '\r'))
      contentEnd--;

    if (contentEnd > contentStart) {
      hrefs.emplace_back(contentStart, contentEnd - contentStart);
    }
    pos = closeTag + 7;
  }
}

int getCalendarObject(const std::string& url, String& responseBody) {
  int lastCode = -1;

  for (int attempt = 0; attempt <= MAX_RETRIES; attempt++) {
    if (attempt > 0) {
      if (!isWifiConnected()) return -1;
      vTaskDelay(pdMS_TO_TICKS(RETRY_DELAY_MS));
    }

    HTTPClient http;
    std::unique_ptr<WiFiClientSecure> secureClient;
    WiFiClient plainClient;
    beginAuthRequest(http, secureClient, plainClient, url);

    lastCode = http.GET();
    if (lastCode == 200) {
      const int len = http.getSize();
      if (len > MAX_RESPONSE_SIZE) {
        http.end();
        return -1;
      }
      responseBody = http.getString();
      http.end();
      return 200;
    }

    http.end();
    if (!isTransientError(lastCode)) return lastCode;
  }

  return lastCode;
}

bool fetchEventsViaHrefFallback(const std::string& collectionUrl, const String& responseBody, int startDays, int endDays,
                                std::vector<CalendarEvent>& outEvents) {
  std::vector<std::string> hrefs;
  collectResponseHrefs(responseBody, hrefs);
  if (hrefs.empty()) return false;

  std::vector<ParsedEvent> parsedEvents;
  parsedEvents.reserve(hrefs.size());

  for (const auto& href : hrefs) {
    String icalBody;
    if (getCalendarObject(absoluteUrlForHref(collectionUrl, href), icalBody) != 200) continue;
    parseCalendarDataFragment(icalBody.c_str(), icalBody.length(), parsedEvents);
  }

  for (const auto& parsed : parsedEvents) {
    if (static_cast<int>(outEvents.size()) >= MAX_EVENTS) break;
    if (intersectsWindow(parsed.event, startDays, endDays)) {
      outEvents.push_back(parsed.event);
    }
  }

  std::sort(outEvents.begin(), outEvents.end());
  return !outEvents.empty();
}

int propfindCollectionMembers(const std::string& url, String& responseBody) {
  static constexpr char propfindXml[] =
      "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
      "<d:propfind xmlns:d=\"DAV:\">"
      "<d:prop><d:getetag/></d:prop>"
      "</d:propfind>";

  int lastCode = -1;
  for (int attempt = 0; attempt <= MAX_RETRIES; attempt++) {
    if (attempt > 0) {
      if (!isWifiConnected()) return -1;
      vTaskDelay(pdMS_TO_TICKS(RETRY_DELAY_MS));
    }

    HTTPClient http;
    std::unique_ptr<WiFiClientSecure> secureClient;
    WiFiClient plainClient;
    beginAuthRequest(http, secureClient, plainClient, url);
    http.addHeader("Content-Type", "application/xml; charset=utf-8");
    http.addHeader("Depth", "1");

    lastCode = http.sendRequest("PROPFIND", reinterpret_cast<uint8_t*>(const_cast<char*>(propfindXml)),
                                strlen(propfindXml));
    if (lastCode == 207) {
      const int len = http.getSize();
      if (len > MAX_RESPONSE_SIZE) {
        http.end();
        return -1;
      }
      responseBody = http.getString();
      http.end();
      return 207;
    }

    http.end();
    if (!isTransientError(lastCode)) return lastCode;
  }

  return lastCode;
}

bool fetchEventsViaPropfindFallback(const std::string& collectionUrl, int startDays, int endDays,
                                    std::vector<CalendarEvent>& outEvents) {
  String propfindBody;
  if (propfindCollectionMembers(collectionUrl, propfindBody) != 207) return false;

  std::vector<std::string> hrefs;
  collectResponseHrefs(propfindBody, hrefs);
  if (hrefs.empty()) return false;

  std::vector<ParsedEvent> parsedEvents;
  parsedEvents.reserve(hrefs.size());

  for (const auto& href : hrefs) {
    if (href.empty() || href.back() == '/') continue;
    if (href.find(".ics") == std::string::npos) continue;

    String icalBody;
    if (getCalendarObject(absoluteUrlForHref(collectionUrl, href), icalBody) != 200) continue;
    parseCalendarDataFragment(icalBody.c_str(), icalBody.length(), parsedEvents);
  }

  for (const auto& parsed : parsedEvents) {
    if (static_cast<int>(outEvents.size()) >= MAX_EVENTS) break;
    if (intersectsWindow(parsed.event, startDays, endDays)) {
      outEvents.push_back(parsed.event);
    }
  }

  std::sort(outEvents.begin(), outEvents.end());
  return !outEvents.empty();
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
    LOG_DBG("CAL", "%s",
            CALDAV_STORE.getConfigError().empty() ? "Missing /.crosspoint/caldav.json"
                                                  : CALDAV_STORE.getConfigError().c_str());
    return NO_CREDENTIALS;
  }

  if (!isWifiConnected()) {
    LOG_ERR("CAL", "WiFi not connected");
    return NETWORK_ERROR;
  }

  const std::string url = normalizeCalendarCollectionUrl(CALDAV_STORE.getCalendarUrl());
  LOG_DBG("CAL", "Fetching calendar: %s", url.c_str());

  int lastCode = -1;

  // Ensure RTC has a valid time before computing the query window
  ensureNtpTime();

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

  std::vector<ParsedEvent> parsedEvents;
  parsedEvents.reserve(128);
  outEvents.clear();
  outEvents.reserve(64);

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
    http.addHeader("Content-Type", "application/xml; charset=utf-8");
    http.addHeader("Depth", "1");

    const std::string queryXml = buildCalendarQueryXml(startDays, endDays);
    lastCode =
        http.sendRequest("REPORT", reinterpret_cast<uint8_t*>(const_cast<char*>(queryXml.c_str())), queryXml.length());

    if (lastCode == 207) {
      int len = http.getSize();
      if (len > MAX_RESPONSE_SIZE) {
        LOG_ERR("CAL", "Response too large: %d bytes", len);
        http.end();
        return SERVER_ERROR;
      }
      String responseBody = http.getString();
      LOG_DBG("CAL", "Got %d bytes (Content-Length: %d)", responseBody.length(), len);
      if (responseBody.length() == 0) {
        LOG_ERR("CAL", "Failed to read response body (allocation failure?)");
        http.end();
        return PARSE_ERROR;
      }
      if (!parseCalendarQueryResponse(responseBody, parsedEvents)) {
        http.end();
        LOG_ERR("CAL", "No VEVENT data in calendar-query response; trying href fallbacks");
        if (fetchEventsViaHrefFallback(url, responseBody, startDays, endDays, outEvents)) {
          LOG_DBG("CAL", "Parsed %zu events via REPORT href fallback", outEvents.size());
          return OK;
        }
        if (fetchEventsViaPropfindFallback(url, startDays, endDays, outEvents)) {
          LOG_DBG("CAL", "Parsed %zu events via PROPFIND fallback", outEvents.size());
          return OK;
        }
        return PARSE_ERROR;
      }

      http.end();
      break;
    }

    http.end();

    if (lastCode == 401) return AUTH_FAILED;
    if (!isTransientError(lastCode)) {
      LOG_ERR("CAL", "Calendar query failed: %d", lastCode);
      return (lastCode < 0) ? NETWORK_ERROR : SERVER_ERROR;
    }
    LOG_DBG("CAL", "Transient error: %d", lastCode);
  }

  if (lastCode != 207) {
    LOG_ERR("CAL", "All retries failed: %d", lastCode);
    return (lastCode < 0) ? NETWORK_ERROR : SERVER_ERROR;
  }

  for (const auto& parsed : parsedEvents) {
    if (static_cast<int>(outEvents.size()) >= MAX_EVENTS) break;
    if (intersectsWindow(parsed.event, startDays, endDays)) {
      outEvents.push_back(parsed.event);
    }
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
  std::string putUrl = normalizeCalendarCollectionUrl(CALDAV_STORE.getCalendarUrl());
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
      return CALDAV_STORE.getConfigError().empty() ? "Missing /.crosspoint/caldav.json"
                                                   : CALDAV_STORE.getConfigError().c_str();
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
