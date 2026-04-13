#include "TandoorCredentialStore.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>
#include <ObfuscationUtils.h>

namespace {
constexpr char TANDOOR_SETTINGS_FILE[] = "/.crosspoint/tandoor.json";
}

// Initialize the static instance
TandoorCredentialStore TandoorCredentialStore::instance;

void TandoorCredentialStore::setServerUrl(const std::string& url) { serverUrl = url; }

void TandoorCredentialStore::setApiToken(const std::string& token) { apiToken = token; }

bool TandoorCredentialStore::hasCredentials() const { return !serverUrl.empty() && !apiToken.empty(); }

std::string TandoorCredentialStore::getApiBaseUrl() const {
  if (serverUrl.empty()) return "";
  std::string url = serverUrl;
  // Strip trailing slash
  while (!url.empty() && url.back() == '/') url.pop_back();
  return url + "/api";
}

bool TandoorCredentialStore::saveToFile() const {
  Storage.mkdir("/.crosspoint");
  return JsonSettingsIO::saveTandoor(*this, TANDOOR_SETTINGS_FILE);
}

bool TandoorCredentialStore::loadFromFile() {
  serverUrl.clear();
  apiToken.clear();

  if (!Storage.exists(TANDOOR_SETTINGS_FILE)) {
    configError = "Missing /.crosspoint/tandoor.json";
    return false;
  }

  String json = Storage.readFile(TANDOOR_SETTINGS_FILE);
  if (json.isEmpty()) {
    configError = "Empty /.crosspoint/tandoor.json";
    return false;
  }

  return JsonSettingsIO::loadTandoor(*this, json.c_str());
}

// ---- JsonSettingsIO for Tandoor ----

bool JsonSettingsIO::saveTandoor(const TandoorCredentialStore& store, const char* path) {
  JsonDocument doc;
  doc["serverUrl"] = store.serverUrl;
  doc["apiToken_obf"] = obfuscation::obfuscateToBase64(store.apiToken);

  String json;
  serializeJson(doc, json);
  return Storage.writeFile(path, json);
}

bool JsonSettingsIO::loadTandoor(TandoorCredentialStore& store, const char* json) {
  JsonDocument doc;
  auto error = deserializeJson(doc, json);
  if (error) {
    store.configError = "Invalid /.crosspoint/tandoor.json";
    LOG_ERR("TDR", "Invalid /.crosspoint/tandoor.json: %s", error.c_str());
    return false;
  }

  store.serverUrl = doc["serverUrl"] | std::string("");
  bool ok = false;
  store.apiToken = obfuscation::deobfuscateFromBase64(doc["apiToken_obf"] | "", &ok);
  if (!ok || store.apiToken.empty()) {
    // Fallback to plain text (first-time setup or migration)
    store.apiToken = doc["apiToken"] | std::string("");
  }

  if (store.serverUrl.empty()) {
    store.configError = "Missing serverUrl in /.crosspoint/tandoor.json";
    return false;
  }
  if (store.apiToken.empty()) {
    store.configError = "Missing apiToken in /.crosspoint/tandoor.json";
    return false;
  }

  store.configError.clear();
  LOG_DBG("TDR", "Loaded Tandoor credentials for: %s", store.serverUrl.c_str());
  return true;
}
