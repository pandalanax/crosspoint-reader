#include "CalDavCredentialStore.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>
#include <ObfuscationUtils.h>

namespace {
constexpr char CALDAV_SETTINGS_FILE[] = "/.crosspoint/caldav.json";
}

CalDavCredentialStore CalDavCredentialStore::instance;

void CalDavCredentialStore::setCalendarUrl(const std::string& url) { calendarUrl = url; }

void CalDavCredentialStore::setUsername(const std::string& user) { username = user; }

void CalDavCredentialStore::setPassword(const std::string& pass) { password = pass; }

bool CalDavCredentialStore::hasCredentials() const { return !calendarUrl.empty(); }

bool CalDavCredentialStore::saveToFile() const {
  Storage.mkdir("/.crosspoint");
  return JsonSettingsIO::saveCalDav(*this, CALDAV_SETTINGS_FILE);
}

bool CalDavCredentialStore::loadFromFile() {
  calendarUrl.clear();
  username.clear();
  password.clear();

  if (!Storage.exists(CALDAV_SETTINGS_FILE)) {
    configError = "Missing /.crosspoint/caldav.json";
    return false;
  }

  String json = Storage.readFile(CALDAV_SETTINGS_FILE);
  if (json.isEmpty()) {
    configError = "Empty /.crosspoint/caldav.json";
    return false;
  }

  return JsonSettingsIO::loadCalDav(*this, json.c_str());
}

// ---- JsonSettingsIO for CalDAV ----

bool JsonSettingsIO::saveCalDav(const CalDavCredentialStore& store, const char* path) {
  JsonDocument doc;
  doc["calendarUrl"] = store.calendarUrl;
  doc["username"] = store.username;
  doc["password_obf"] = obfuscation::obfuscateToBase64(store.password);

  String json;
  serializeJson(doc, json);
  return Storage.writeFile(path, json);
}

bool JsonSettingsIO::loadCalDav(CalDavCredentialStore& store, const char* json) {
  JsonDocument doc;
  auto error = deserializeJson(doc, json);
  if (error) {
    store.configError = "Invalid /.crosspoint/caldav.json";
    LOG_ERR("CAL", "Invalid /.crosspoint/caldav.json: %s", error.c_str());
    return false;
  }

  store.calendarUrl = doc["calendarUrl"] | std::string("");
  store.username = doc["username"] | std::string("");
  bool ok = false;
  store.password = obfuscation::deobfuscateFromBase64(doc["password_obf"] | "", &ok);
  if (!ok || store.password.empty()) {
    store.password = doc["password"] | std::string("");
  }

  if (store.calendarUrl.empty()) {
    store.configError = "Missing calendarUrl in /.crosspoint/caldav.json";
    return false;
  }
  if (store.username.empty()) {
    store.configError = "Missing username in /.crosspoint/caldav.json";
    return false;
  }
  if (store.password.empty()) {
    store.configError = "Missing password in /.crosspoint/caldav.json";
    return false;
  }

  store.configError.clear();
  LOG_DBG("CAL", "Loaded CalDAV credentials for: %s", store.calendarUrl.c_str());
  return true;
}
