#pragma once

#include <string>
#include <vector>

#include "activities/Activity.h"
#include "caldav/CalDavClient.h"
#include "util/ButtonNavigator.h"

/**
 * CalendarActivity displays a month grid calendar fetched from a CalDAV server.
 *
 * Month view: grid with day numbers, dots on days with events, cursor navigation.
 * Day view: press Confirm on a day to see that day's events as a list.
 *
 * Navigation:
 *   Month view: Up/Down/Left/Right move cursor, PageFwd/PageBack switch months,
 *               Confirm opens day detail, Back goes home, long-press Back refreshes.
 *   Day view:   Up/Down scroll events, Back returns to month view.
 */
class CalendarActivity final : public Activity {
  enum class State {
    WIFI_SELECTION,
    FETCHING,
    MONTH_VIEW,
    DAY_VIEW,
    TIME_PICKER,
    ERROR,
  };

  struct PendingEvent {
    int year, month, day, hour, minute;
  };

  ButtonNavigator buttonNavigator;
  State state = State::WIFI_SELECTION;
  std::vector<CalendarEvent> events;
  std::string errorMessage;
  bool userActive = false;

  // Month grid state — all set by initTodayDate() in onEnter()
  int viewYear = 0;
  int viewMonth = 1;
  int cursorDay = 1;
  int todayYear = 0, todayMonth = 0, todayDay = 0;

  // Day detail state
  size_t dayDetailIndex = 0;                    // scroll position in day events list
  std::vector<const CalendarEvent*> dayEvents;  // pointers into events vector

  // Time picker state (for on-the-go event creation)
  int pickerHour = 12;
  int pickerMinute = 0;
  int pickerField = 0;  // 0=hour, 1=minute

  // Pending events (created offline, synced on next refresh)
  std::vector<PendingEvent> pendingEvents;

  // Helpers
  void onWifiComplete(bool connected);
  void fetchCalendar();
  void triggerRefresh();
  bool saveCacheToSd() const;
  bool loadCacheFromSd();
  bool savePendingToSd() const;
  bool loadPendingFromSd();
  void syncPendingEvents();

  void initTodayDate();
  int daysInMonth(int year, int month) const;
  int dayOfWeek(int year, int month, int day) const;  // 0=Mon, 6=Sun
  bool hasEventsOnDay(int year, int month, int day) const;
  void collectDayEvents(int year, int month, int day);
  void clampCursor();
  void changeMonth(int delta);

  // Rendering
  void renderMonthGrid();
  void renderDayDetail();
  void renderTimePicker();

 public:
  explicit CalendarActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Calendar", renderer, mappedInput) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return userActive; }
};
