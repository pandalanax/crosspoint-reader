#pragma once
#include <string>

class CalDavCredentialStore;
namespace JsonSettingsIO {
bool saveCalDav(const CalDavCredentialStore& store, const char* path);
bool loadCalDav(CalDavCredentialStore& store, const char* json);
}  // namespace JsonSettingsIO

/**
 * Singleton class for storing CalDAV credentials on the SD card.
 * Stores the calendar URL, username, and password needed to fetch events.
 */
class CalDavCredentialStore {
 private:
  static CalDavCredentialStore instance;
  std::string calendarUrl;  // Full .ics URL, e.g. "https://cal.example.com/user/calendar-id/"
  std::string username;
  std::string password;
  std::string configError;

  CalDavCredentialStore() = default;

  friend bool JsonSettingsIO::saveCalDav(const CalDavCredentialStore&, const char*);
  friend bool JsonSettingsIO::loadCalDav(CalDavCredentialStore&, const char*);

 public:
  CalDavCredentialStore(const CalDavCredentialStore&) = delete;
  CalDavCredentialStore& operator=(const CalDavCredentialStore&) = delete;

  static CalDavCredentialStore& getInstance() { return instance; }

  bool saveToFile() const;
  bool loadFromFile();

  void setCalendarUrl(const std::string& url);
  const std::string& getCalendarUrl() const { return calendarUrl; }

  void setUsername(const std::string& user);
  const std::string& getUsername() const { return username; }

  void setPassword(const std::string& pass);
  const std::string& getPassword() const { return password; }

  bool hasCredentials() const;
  const std::string& getConfigError() const { return configError; }
};

#define CALDAV_STORE CalDavCredentialStore::getInstance()
