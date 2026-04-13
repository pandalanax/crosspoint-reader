#pragma once
#include <string>

class TandoorCredentialStore;
namespace JsonSettingsIO {
bool saveTandoor(const TandoorCredentialStore& store, const char* path);
bool loadTandoor(TandoorCredentialStore& store, const char* json);
}  // namespace JsonSettingsIO

/**
 * Singleton class for storing Tandoor Recipes API credentials on the SD card.
 * Stores the server URL and API token needed to access the shopping list API.
 */
class TandoorCredentialStore {
 private:
  static TandoorCredentialStore instance;
  std::string serverUrl;  // e.g. "https://recipes.example.com"
  std::string apiToken;   // Tandoor API token
  std::string configError;

  // Private constructor for singleton
  TandoorCredentialStore() = default;

  friend bool JsonSettingsIO::saveTandoor(const TandoorCredentialStore&, const char*);
  friend bool JsonSettingsIO::loadTandoor(TandoorCredentialStore&, const char*);

 public:
  // Delete copy constructor and assignment
  TandoorCredentialStore(const TandoorCredentialStore&) = delete;
  TandoorCredentialStore& operator=(const TandoorCredentialStore&) = delete;

  static TandoorCredentialStore& getInstance() { return instance; }

  bool saveToFile() const;
  bool loadFromFile();

  void setServerUrl(const std::string& url);
  const std::string& getServerUrl() const { return serverUrl; }

  void setApiToken(const std::string& token);
  const std::string& getApiToken() const { return apiToken; }

  bool hasCredentials() const;
  const std::string& getConfigError() const { return configError; }

  // Get base API URL (e.g. "https://recipes.example.com/api")
  std::string getApiBaseUrl() const;
};

#define TANDOOR_STORE TandoorCredentialStore::getInstance()
