#include "CalendarActivity.h"

#include <ArduinoJson.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>

#include "MappedInputManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "caldav/CalDavCredentialStore.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr char CACHE_FILE[] = "/.crosspoint/calendar_cache.json";
constexpr char PENDING_FILE[] = "/.crosspoint/calendar_pending.json";

constexpr const char* MONTH_NAMES[] = {"",    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                       "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

constexpr const char* DAY_HEADERS[] = {"Mo", "Tu", "We", "Th", "Fr", "Sa", "Su"};
}  // namespace

std::string CalendarActivity::cacheMissingMessage() {
  return "Missing /.crosspoint/calendar_cache.json";
}

std::string CalendarActivity::wifiRequiredMessage() {
  return "WiFi required. No cache at /.crosspoint/calendar_cache.json";
}

std::string CalendarActivity::fetchFailedNoCacheMessage(const char* fetchError) {
  std::string msg = fetchError ? fetchError : "Calendar fetch failed";
  msg += ". No cache at /.crosspoint/calendar_cache.json";
  return msg;
}

// ---- Date helpers ----

void CalendarActivity::initTodayDate() {
  struct tm now;
  time_t nowEpoch = time(nullptr);
  localtime_r(&nowEpoch, &now);
  todayYear = now.tm_year + 1900;
  todayMonth = now.tm_mon + 1;
  todayDay = now.tm_mday;
}

int CalendarActivity::daysInMonth(int year, int month) const {
  static constexpr int dim[] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (month == 2 && (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0))) return 29;
  return dim[month];
}

// Returns 0=Monday .. 6=Sunday (ISO weekday)
int CalendarActivity::dayOfWeek(int year, int month, int day) const {
  // Tomohiko Sakamoto's algorithm (returns 0=Sun..6=Sat)
  static constexpr int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
  int y = year;
  if (month < 3) y--;
  int dow = (y + y / 4 - y / 100 + y / 400 + t[month - 1] + day) % 7;
  // Convert: 0=Sun -> 6, 1=Mon -> 0, 2=Tue -> 1, ...
  return (dow + 6) % 7;
}

bool CalendarActivity::hasEventsOnDay(int year, int month, int day) const {
  for (const auto& ev : events) {
    if (ev.startYear == year && ev.startMonth == month && ev.startDay == day) return true;
  }
  return false;
}

void CalendarActivity::collectDayEvents(int year, int month, int day) {
  dayEvents.clear();
  for (const auto& ev : events) {
    if (ev.startYear == year && ev.startMonth == month && ev.startDay == day) {
      dayEvents.push_back(&ev);
    }
  }
}

void CalendarActivity::clampCursor() {
  int maxDay = daysInMonth(viewYear, viewMonth);
  if (cursorDay > maxDay) cursorDay = maxDay;
  if (cursorDay < 1) cursorDay = 1;
}

void CalendarActivity::changeMonth(int delta) {
  viewMonth += delta;
  while (viewMonth > 12) {
    viewMonth -= 12;
    viewYear++;
  }
  while (viewMonth < 1) {
    viewMonth += 12;
    viewYear--;
  }
  clampCursor();
}

// ---- Activity lifecycle ----

void CalendarActivity::onEnter() {
  Activity::onEnter();
  initTodayDate();
  viewYear = todayYear;
  viewMonth = todayMonth;
  cursorDay = todayDay;

  loadPendingFromSd();

  if (!CALDAV_STORE.hasCredentials()) {
    state = State::ERROR;
    errorMessage = CALDAV_STORE.getConfigError().empty() ? "Missing /.crosspoint/caldav.json"
                                                         : CALDAV_STORE.getConfigError();
    requestUpdate();
    return;
  }

  if (loadCacheFromSd()) {
    state = State::MONTH_VIEW;
    userActive = true;
    requestUpdate();
    return;
  }

  state = State::WIFI_SELECTION;
  requestUpdate();
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiComplete(!result.isCancelled); });
}

void CalendarActivity::onExit() {
  if (!events.empty()) saveCacheToSd();
  if (!pendingEvents.empty()) savePendingToSd();
  events.clear();
  dayEvents.clear();
  pendingEvents.clear();
  WiFi.mode(WIFI_OFF);
  Activity::onExit();
}

void CalendarActivity::onWifiComplete(bool connected) {
  if (connected) {
    state = State::FETCHING;
    requestUpdateAndWait();
    fetchCalendar();
  } else {
    WiFi.disconnect();
    WiFi.mode(WIFI_OFF);
    if (loadCacheFromSd()) {
      state = State::MONTH_VIEW;
    } else {
      state = State::ERROR;
      errorMessage = wifiRequiredMessage();
    }
    requestUpdate();
  }
}

void CalendarActivity::fetchCalendar() {
  syncPendingEvents();
  CalDavClient::Error err = CalDavClient::fetchEvents(events, 30, 120);
  if (err != CalDavClient::OK) {
    if (loadCacheFromSd()) {
      state = State::MONTH_VIEW;
      LOG_DBG("CAL", "Fetch failed (%s), using cache", CalDavClient::errorString(err));
    } else {
      state = State::ERROR;
      errorMessage = fetchFailedNoCacheMessage(CalDavClient::errorString(err));
    }
    requestUpdate();
    return;
  }

  saveCacheToSd();
  state = State::MONTH_VIEW;
  userActive = false;

  WiFi.disconnect();
  WiFi.mode(WIFI_OFF);
  requestUpdate();
}

void CalendarActivity::triggerRefresh() {
  state = State::WIFI_SELECTION;
  requestUpdateAndWait();
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiComplete(!result.isCancelled); });
}

// ---- Cache ----

bool CalendarActivity::saveCacheToSd() const {
  Storage.mkdir("/.crosspoint");

  JsonDocument doc;
  JsonArray arr = doc["events"].to<JsonArray>();
  for (const auto& ev : events) {
    JsonObject obj = arr.add<JsonObject>();
    obj["s"] = ev.summary;
    obj["l"] = ev.location;
    obj["y"] = ev.startYear;
    obj["m"] = ev.startMonth;
    obj["d"] = ev.startDay;
    obj["h"] = ev.startHour;
    obj["n"] = ev.startMinute;
    obj["a"] = ev.allDay;
  }

  String json;
  serializeJson(doc, json);
  bool ok = Storage.writeFile(CACHE_FILE, json);
  if (ok) LOG_DBG("CAL", "Saved %zu events to cache", events.size());
  return ok;
}

bool CalendarActivity::loadCacheFromSd() {
  if (!Storage.exists(CACHE_FILE)) {
    errorMessage = cacheMissingMessage();
    return false;
  }

  String json = Storage.readFile(CACHE_FILE);
  if (json.isEmpty()) {
    errorMessage = "Empty /.crosspoint/calendar_cache.json";
    return false;
  }

  JsonDocument doc;
  auto error = deserializeJson(doc, json);
  if (error) {
    LOG_ERR("CAL", "Cache parse failed: %s", error.c_str());
    errorMessage = "Invalid /.crosspoint/calendar_cache.json";
    return false;
  }

  events.clear();
  JsonArray arr = doc["events"].as<JsonArray>();
  events.reserve(arr.size());

  for (JsonObject obj : arr) {
    CalendarEvent ev;
    ev.summary = obj["s"] | "";
    ev.location = obj["l"] | "";
    ev.startYear = obj["y"] | 0;
    ev.startMonth = obj["m"] | 0;
    ev.startDay = obj["d"] | 0;
    ev.startHour = obj["h"] | 0;
    ev.startMinute = obj["n"] | 0;
    ev.allDay = obj["a"] | false;
    events.push_back(std::move(ev));
  }

  LOG_DBG("CAL", "Loaded %zu events from cache", events.size());
  if (events.empty()) {
    errorMessage = "No events in /.crosspoint/calendar_cache.json";
    return false;
  }
  return !events.empty();
}

// ---- Pending events ----

bool CalendarActivity::savePendingToSd() const {
  Storage.mkdir("/.crosspoint");
  JsonDocument doc;
  JsonArray arr = doc["pending"].to<JsonArray>();
  for (const auto& pe : pendingEvents) {
    JsonObject obj = arr.add<JsonObject>();
    obj["y"] = pe.year;
    obj["m"] = pe.month;
    obj["d"] = pe.day;
    obj["h"] = pe.hour;
    obj["n"] = pe.minute;
  }
  String json;
  serializeJson(doc, json);
  bool ok = Storage.writeFile(PENDING_FILE, json);
  if (ok) LOG_DBG("CAL", "Saved %zu pending events", pendingEvents.size());
  return ok;
}

bool CalendarActivity::loadPendingFromSd() {
  if (!Storage.exists(PENDING_FILE)) return false;
  String json = Storage.readFile(PENDING_FILE);
  if (json.isEmpty()) {
    LOG_ERR("CAL", "Empty /.crosspoint/calendar_pending.json");
    return false;
  }

  JsonDocument doc;
  if (deserializeJson(doc, json)) {
    LOG_ERR("CAL", "Invalid /.crosspoint/calendar_pending.json");
    return false;
  }

  pendingEvents.clear();
  JsonArray arr = doc["pending"].as<JsonArray>();
  pendingEvents.reserve(arr.size());
  for (JsonObject obj : arr) {
    PendingEvent pe;
    pe.year = obj["y"] | 0;
    pe.month = obj["m"] | 0;
    pe.day = obj["d"] | 0;
    pe.hour = obj["h"] | 0;
    pe.minute = obj["n"] | 0;
    // Skip malformed entries
    if (pe.year < 2020 || pe.month < 1 || pe.month > 12 || pe.day < 1 || pe.day > 31 || pe.hour > 23 || pe.minute > 59)
      continue;
    pendingEvents.push_back(pe);
  }
  LOG_DBG("CAL", "Loaded %zu pending events", pendingEvents.size());
  return !pendingEvents.empty();
}

void CalendarActivity::syncPendingEvents() {
  if (pendingEvents.empty()) return;
  LOG_DBG("CAL", "Syncing %zu pending events", pendingEvents.size());

  std::vector<PendingEvent> failed;
  for (const auto& pe : pendingEvents) {
    auto err = CalDavClient::putEvent(pe.year, pe.month, pe.day, pe.hour, pe.minute);
    if (err == CalDavClient::OK) {
      LOG_DBG("CAL", "Synced: %04d-%02d-%02d %02d:%02d", pe.year, pe.month, pe.day, pe.hour, pe.minute);
    } else {
      LOG_ERR("CAL", "Sync failed (%s): %04d-%02d-%02d", CalDavClient::errorString(err), pe.year, pe.month, pe.day);
      failed.push_back(pe);
    }
  }

  pendingEvents = std::move(failed);
  savePendingToSd();
  if (pendingEvents.empty()) {
    Storage.remove(PENDING_FILE);
  } else {
    LOG_ERR("CAL", "%zu pending events remain in /.crosspoint/calendar_pending.json", pendingEvents.size());
  }
}

// ---- Input handling ----

void CalendarActivity::loop() {
  if (state == State::TIME_PICKER) {
    // Back: cancel, return to month view
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      state = State::MONTH_VIEW;
      userActive = true;
      requestUpdate();
      return;
    }

    // Confirm: save pending event (cap at 50 to prevent unbounded growth)
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (pendingEvents.size() >= 50) {
        LOG_ERR("CAL", "Too many pending events, dropping oldest");
        pendingEvents.erase(pendingEvents.begin());
      }
      PendingEvent pe{viewYear, viewMonth, cursorDay, pickerHour, pickerMinute};
      pendingEvents.push_back(pe);
      savePendingToSd();

      // Also add to local events list for immediate display
      CalendarEvent ev;
      ev.summary = "Meeting";
      ev.startYear = pe.year;
      ev.startMonth = pe.month;
      ev.startDay = pe.day;
      ev.startHour = pe.hour;
      ev.startMinute = pe.minute;
      ev.allDay = false;
      events.push_back(std::move(ev));
      std::sort(events.begin(), events.end());
      saveCacheToSd();

      state = State::MONTH_VIEW;
      userActive = true;
      requestUpdate();
      return;
    }

    // Left/Right: switch between hour and minute fields
    if (mappedInput.wasReleased(MappedInputManager::Button::Left) ||
        mappedInput.wasReleased(MappedInputManager::Button::Right)) {
      pickerField = 1 - pickerField;
      userActive = true;
      requestUpdate();
      return;
    }

    // Up/Down: increment/decrement selected field
    buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Up}, [this] {
      if (pickerField == 0) {
        pickerHour = (pickerHour + 1) % 24;
      } else {
        pickerMinute = (pickerMinute + 5) % 60;
      }
      userActive = true;
      requestUpdate();
    });
    buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Down}, [this] {
      if (pickerField == 0) {
        pickerHour = (pickerHour + 23) % 24;
      } else {
        pickerMinute = (pickerMinute + 55) % 60;
      }
      userActive = true;
      requestUpdate();
    });
    return;
  }

  if (state == State::DAY_VIEW) {
    // Back returns to month view
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      state = State::MONTH_VIEW;
      userActive = true;
      requestUpdate();
      return;
    }

    // Scroll through day's events
    int eventCount = static_cast<int>(dayEvents.size());
    if (eventCount > 1) {
      buttonNavigator.onNextRelease([this, eventCount] {
        dayDetailIndex = ButtonNavigator::nextIndex(static_cast<int>(dayDetailIndex), eventCount);
        userActive = true;
        requestUpdate();
      });
      buttonNavigator.onPreviousRelease([this, eventCount] {
        dayDetailIndex = ButtonNavigator::previousIndex(static_cast<int>(dayDetailIndex), eventCount);
        userActive = true;
        requestUpdate();
      });
    }
    return;
  }

  if (state != State::MONTH_VIEW) return;

  // Back: short = home, long = refresh
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (mappedInput.getHeldTime() >= 1000) {
      triggerRefresh();
    } else {
      onGoHome();
    }
    return;
  }

  // Confirm: long press = add event, short press = day detail
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (mappedInput.getHeldTime() >= 1000) {
      // Long press: open time picker for new event
      pickerHour = 12;
      pickerMinute = 0;
      pickerField = 0;
      state = State::TIME_PICKER;
      userActive = true;
      requestUpdate();
    } else {
      // Short press: show day events
      collectDayEvents(viewYear, viewMonth, cursorDay);
      if (!dayEvents.empty()) {
        dayDetailIndex = 0;
        state = State::DAY_VIEW;
        userActive = true;
        requestUpdate();
      }
    }
    return;
  }

  // Month navigation with side buttons (PageForward/PageBack)
  if (mappedInput.wasReleased(MappedInputManager::Button::PageForward)) {
    changeMonth(1);
    userActive = true;
    requestUpdate();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::PageBack)) {
    changeMonth(-1);
    userActive = true;
    requestUpdate();
    return;
  }

  // Grid cursor navigation
  int maxDay = daysInMonth(viewYear, viewMonth);

  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Right}, [this, maxDay] {
    if (cursorDay < maxDay) {
      cursorDay++;
    } else {
      changeMonth(1);
      cursorDay = 1;
    }
    userActive = true;
    requestUpdate();
  });

  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Left}, [this] {
    if (cursorDay > 1) {
      cursorDay--;
    } else {
      changeMonth(-1);
      cursorDay = daysInMonth(viewYear, viewMonth);
    }
    userActive = true;
    requestUpdate();
  });

  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Down}, [this, maxDay] {
    if (cursorDay + 7 <= maxDay) {
      cursorDay += 7;
    } else if (cursorDay + 7 > maxDay) {
      changeMonth(1);
      cursorDay = std::min(cursorDay + 7 - maxDay, daysInMonth(viewYear, viewMonth));
    }
    userActive = true;
    requestUpdate();
  });

  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Up}, [this] {
    if (cursorDay - 7 >= 1) {
      cursorDay -= 7;
    } else {
      int prevMonthDays = daysInMonth(viewMonth == 1 ? viewYear - 1 : viewYear, viewMonth == 1 ? 12 : viewMonth - 1);
      changeMonth(-1);
      cursorDay = std::min(prevMonthDays + (cursorDay - 7), daysInMonth(viewYear, viewMonth));
    }
    userActive = true;
    requestUpdate();
  });
}

// ---- Rendering ----

void CalendarActivity::renderMonthGrid() {
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  // Header: "Apr 2026"
  char headerBuf[32];
  snprintf(headerBuf, sizeof(headerBuf), "%s %d", MONTH_NAMES[viewMonth], viewYear);
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, headerBuf);

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentBottom = pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing;
  const int contentHeight = contentBottom - contentTop;

  // Layout: 7 columns
  const int cellWidth = pageWidth / 7;
  const int dayHeaderHeight = 20;
  const int gridTop = contentTop + dayHeaderHeight + 4;
  const int gridHeight = contentHeight - dayHeaderHeight - 4;
  const int cellHeight = gridHeight / 6;  // Max 6 rows in a month

  // Day-of-week headers (Mo Tu We Th Fr Sa Su)
  for (int col = 0; col < 7; col++) {
    int x = col * cellWidth;
    int textW = renderer.getTextWidth(UI_10_FONT_ID, DAY_HEADERS[col]);
    renderer.drawText(UI_10_FONT_ID, x + (cellWidth - textW) / 2, contentTop, DAY_HEADERS[col]);
  }

  // Separator line under day headers
  renderer.fillRect(metrics.contentSidePadding, contentTop + dayHeaderHeight - 2,
                    pageWidth - 2 * metrics.contentSidePadding, 1, true);

  // Draw grid
  int firstDow = dayOfWeek(viewYear, viewMonth, 1);  // 0=Mon
  int maxDay = daysInMonth(viewYear, viewMonth);

  for (int day = 1; day <= maxDay; day++) {
    int cellIdx = (day - 1) + firstDow;
    int col = cellIdx % 7;
    int row = cellIdx / 7;

    int cellX = col * cellWidth;
    int cellY = gridTop + row * cellHeight;
    int cellCenterX = cellX + cellWidth / 2;
    int cellCenterY = cellY + cellHeight / 2;

    char dayStr[4];
    snprintf(dayStr, sizeof(dayStr), "%d", day);
    int textW = renderer.getTextWidth(UI_12_FONT_ID, dayStr);
    int textH = renderer.getLineHeight(UI_12_FONT_ID);
    int textX = cellCenterX - textW / 2;
    int textY = cellCenterY - textH / 2 - 4;  // Shift up to leave room for dot

    bool isToday = (viewYear == todayYear && viewMonth == todayMonth && day == todayDay);
    bool isSelected = (day == cursorDay);

    if (isToday) {
      // Filled circle for today
      int radius = std::min(cellWidth, cellHeight) / 2 - 4;
      // Draw filled circle as a series of horizontal lines
      for (int dy = -radius; dy <= radius; dy++) {
        int dx = static_cast<int>(sqrt(static_cast<float>(radius * radius - dy * dy)));
        renderer.fillRect(cellCenterX - dx, cellCenterY - 4 + dy, 2 * dx, 1, true);
      }
      renderer.drawText(UI_12_FONT_ID, textX, textY, dayStr, false);  // White text on black circle
    } else if (isSelected) {
      // Thick border rect for cursor
      int boxW = cellWidth - 8;
      int boxH = cellHeight - 6;
      int boxX = cellCenterX - boxW / 2;
      int boxY = cellCenterY - boxH / 2 - 2;
      renderer.drawRect(boxX, boxY, boxW, boxH, 2, true);
      renderer.drawText(UI_12_FONT_ID, textX, textY, dayStr);
    } else {
      renderer.drawText(UI_12_FONT_ID, textX, textY, dayStr);
    }

    // Event dot
    if (hasEventsOnDay(viewYear, viewMonth, day)) {
      int dotY = cellCenterY + textH / 2 - 2;
      int dotR = 3;
      if (isToday) {
        // White dot on black circle
        renderer.fillRect(cellCenterX - dotR, dotY - dotR, dotR * 2, dotR * 2, false);
      } else {
        renderer.fillRect(cellCenterX - dotR, dotY - dotR, dotR * 2, dotR * 2, true);
      }
    }
  }

  // Button hints
  const auto labels = mappedInput.mapLabels("Back/Refresh", "View/+Add", "Prev Mo", "Next Mo");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void CalendarActivity::renderDayDetail() {
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  // Header: "Tue Apr 5"
  static constexpr const char* DOW_NAMES[] = {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};
  char headerBuf[32];
  int dow = dayOfWeek(viewYear, viewMonth, cursorDay);
  snprintf(headerBuf, sizeof(headerBuf), "%s %s %d", DOW_NAMES[dow], MONTH_NAMES[viewMonth], cursorDay);
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, headerBuf);

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;

  if (dayEvents.empty()) {
    renderer.drawText(UI_12_FONT_ID, metrics.contentSidePadding, contentTop + 20, "No events");
  } else {
    GUI.drawList(renderer, Rect{0, contentTop, pageWidth, contentHeight}, static_cast<int>(dayEvents.size()),
                 static_cast<int>(dayDetailIndex), [this](int index) -> std::string {
                   const auto* ev = dayEvents[index];
                   if (ev->allDay) {
                     return "All day: " + ev->summary;
                   }
                   char timeBuf[8];
                   snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d ", ev->startHour, ev->startMinute);
                   std::string line = std::string(timeBuf) + ev->summary;
                   if (!ev->location.empty()) {
                     line += " @ " + ev->location;
                   }
                   return line;
                 });
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void CalendarActivity::renderTimePicker() {
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  // Header: "Add Meeting — Apr 5"
  char headerBuf[48];
  snprintf(headerBuf, sizeof(headerBuf), "Add Meeting - %s %d", MONTH_NAMES[viewMonth], cursorDay);
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, headerBuf);

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  int centerX = pageWidth / 2;
  int centerY = contentTop + (pageHeight - contentTop - metrics.buttonHintsHeight) / 2 - 20;

  // Draw time display: "HH : MM"
  char hourStr[4], minStr[4];
  snprintf(hourStr, sizeof(hourStr), "%02d", pickerHour);
  snprintf(minStr, sizeof(minStr), "%02d", pickerMinute);

  int hourW = renderer.getTextWidth(BOOKERLY_18_FONT_ID, hourStr);
  int minW = renderer.getTextWidth(BOOKERLY_18_FONT_ID, minStr);
  int colonW = renderer.getTextWidth(BOOKERLY_18_FONT_ID, ":");
  int totalW = hourW + colonW + minW + 24;  // 24px spacing

  int hourX = centerX - totalW / 2;
  int colonX = hourX + hourW + 8;
  int minX = colonX + colonW + 16;
  int textH = renderer.getLineHeight(BOOKERLY_18_FONT_ID);
  int textY = centerY - textH / 2;

  renderer.drawText(BOOKERLY_18_FONT_ID, hourX, textY, hourStr);
  renderer.drawText(BOOKERLY_18_FONT_ID, colonX, textY, ":");
  renderer.drawText(BOOKERLY_18_FONT_ID, minX, textY, minStr);

  // Underline the selected field
  int underY = textY + textH + 4;
  if (pickerField == 0) {
    renderer.fillRect(hourX - 2, underY, hourW + 4, 3, true);
  } else {
    renderer.fillRect(minX - 2, underY, minW + 4, 3, true);
  }

  // Arrows above/below selected field
  int arrowX = (pickerField == 0) ? hourX + hourW / 2 : minX + minW / 2;
  int arrowUpY = textY - 16;
  int arrowDownY = underY + 10;
  // Up arrow: small triangle
  for (int dy = 0; dy < 8; dy++) {
    renderer.fillRect(arrowX - dy, arrowUpY + dy, 2 * dy + 1, 1, true);
  }
  // Down arrow: small triangle
  for (int dy = 0; dy < 8; dy++) {
    renderer.fillRect(arrowX - dy, arrowDownY + 8 - dy, 2 * dy + 1, 1, true);
  }

  // Pending count indicator
  if (!pendingEvents.empty()) {
    char pendBuf[32];
    snprintf(pendBuf, sizeof(pendBuf), "%zu pending", pendingEvents.size());
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, pageHeight - metrics.buttonHintsHeight - 30, pendBuf);
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "Save", tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void CalendarActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto& metrics = UITheme::getInstance().getMetrics();

  if (state == State::FETCHING) {
    GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "Calendar");
    int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
    renderer.drawText(UI_12_FONT_ID, metrics.contentSidePadding, contentTop + 20, "Fetching calendar...");
    renderer.displayBuffer();
    return;
  }

  if (state == State::ERROR) {
    GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "Calendar");
    int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
    renderer.drawText(UI_12_FONT_ID, metrics.contentSidePadding, contentTop + 20, errorMessage.c_str());
    renderer.displayBuffer();
    return;
  }

  if (state == State::WIFI_SELECTION) {
    GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "Calendar");
    int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
    renderer.drawText(UI_12_FONT_ID, metrics.contentSidePadding, contentTop + 20, "Connecting to WiFi...");
    renderer.displayBuffer();
    return;
  }

  if (state == State::MONTH_VIEW) {
    renderMonthGrid();
  } else if (state == State::DAY_VIEW) {
    renderDayDetail();
  } else if (state == State::TIME_PICKER) {
    renderTimePicker();
  }

  renderer.displayBuffer();
}
