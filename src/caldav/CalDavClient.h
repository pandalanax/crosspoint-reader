#pragma once
#include <cstdint>
#include <string>
#include <vector>

/**
 * A single calendar event parsed from iCalendar data.
 */
struct CalendarEvent {
  std::string summary;
  std::string location;
  int startYear, startMonth, startDay;
  int startHour, startMinute;
  int endYear, endMonth, endDay;
  int endHour, endMinute;
  bool allDay;

  // For display sorting
  bool operator<(const CalendarEvent& other) const;
};

/**
 * HTTP client for CalDAV calendar access.
 *
 * Fetches a full .ics file from a Radicale (or any CalDAV) server,
 * parses VEVENT blocks, and returns events within a date range.
 *
 * Authentication: HTTP Basic Auth
 */
class CalDavClient {
 public:
  enum Error { OK = 0, NO_CREDENTIALS, NETWORK_ERROR, AUTH_FAILED, SERVER_ERROR, PARSE_ERROR };

  /**
   * Fetch calendar events within a date range.
   * @param daysBack  How many days before today to include
   * @param daysForward  How many days after today to include
   * Skips recurring events (RRULE) in v1.
   */
  static Error fetchEvents(std::vector<CalendarEvent>& outEvents, int daysBack = 30, int daysForward = 120);

  /**
   * Upload a new event to the CalDAV server via PUT.
   * Generates a unique UID and creates a minimal VCALENDAR.
   */
  static Error putEvent(int year, int month, int day, int hour, int minute, const char* summary = "Meeting");

  static const char* errorString(Error error);
};
