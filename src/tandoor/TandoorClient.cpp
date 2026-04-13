#include "TandoorClient.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Logging.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#include <algorithm>

#include "TandoorCredentialStore.h"

namespace {

constexpr int HTTP_TIMEOUT_MS = 10000;        // 10s per request
constexpr int MAX_RETRIES = 2;                // Retry once on transient failure
constexpr int RETRY_DELAY_MS = 1000;          // 1s between retries
constexpr int MAX_PAGES = 10;                 // Safety bound on pagination
constexpr int MAX_RESPONSE_SIZE = 64 * 1024;  // 64KB max response per page

bool isHttpsUrl(const std::string& url) { return url.rfind("https://", 0) == 0; }

bool isWifiConnected() { return WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0); }

bool isTransientError(int httpCode) {
  return httpCode < 0 || httpCode == 500 || httpCode == 502 || httpCode == 503 || httpCode == 504;
}

void addAuthHeaders(HTTPClient& http) {
  http.addHeader("Authorization", ("Bearer " + TANDOOR_STORE.getApiToken()).c_str());
  http.addHeader("Accept", "application/json");
}

/**
 * Helper to begin an HTTP request with optional HTTPS support.
 * Caller owns the secureClient and must keep it alive until http.end().
 */
void beginRequest(HTTPClient& http, std::unique_ptr<WiFiClientSecure>& secureClient, WiFiClient& plainClient,
                  const std::string& url) {
  if (isHttpsUrl(url)) {
    secureClient.reset(new WiFiClientSecure);
    secureClient->setInsecure();
    http.begin(*secureClient, url.c_str());
  } else {
    http.begin(plainClient, url.c_str());
  }
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  addAuthHeaders(http);
}

/**
 * Perform a GET request with retry on transient failures.
 * Returns the HTTP status code (negative = connection error).
 * On success, responseBody is populated.
 */
int getWithRetry(const std::string& url, String& responseBody) {
  int lastCode = -1;

  for (int attempt = 0; attempt <= MAX_RETRIES; attempt++) {
    if (attempt > 0) {
      if (!isWifiConnected()) {
        LOG_ERR("TDR", "WiFi lost during retry");
        return -1;
      }
      LOG_DBG("TDR", "Retry %d/%d after %dms", attempt, MAX_RETRIES, RETRY_DELAY_MS);
      vTaskDelay(pdMS_TO_TICKS(RETRY_DELAY_MS));
    }

    HTTPClient http;
    std::unique_ptr<WiFiClientSecure> secureClient;
    WiFiClient plainClient;
    beginRequest(http, secureClient, plainClient, url);

    lastCode = http.GET();

    if (lastCode == 200) {
      int len = http.getSize();
      if (len > MAX_RESPONSE_SIZE) {
        LOG_ERR("TDR", "Response too large: %d bytes", len);
        http.end();
        return -1;
      }
      responseBody = http.getString();
      http.end();
      return 200;
    }

    http.end();

    // Don't retry auth failures or client errors — they won't change
    if (!isTransientError(lastCode)) {
      return lastCode;
    }

    LOG_DBG("TDR", "Transient error: %d", lastCode);
  }

  return lastCode;
}

}  // namespace

TandoorClient::Error TandoorClient::fetchShoppingList(std::vector<ShoppingListItem>& outItems) {
  if (!TANDOOR_STORE.hasCredentials()) {
    LOG_DBG("TDR", "%s", TANDOOR_STORE.getConfigError().empty() ? "Missing /.crosspoint/tandoor.json"
                                                                 : TANDOOR_STORE.getConfigError().c_str());
    return NO_CREDENTIALS;
  }

  if (!isWifiConnected()) {
    LOG_ERR("TDR", "WiFi not connected");
    return NETWORK_ERROR;
  }

  outItems.clear();
  outItems.reserve(64);

  std::string url = TANDOOR_STORE.getApiBaseUrl() + "/shopping-list-entry/?format=json&page_size=200";
  int page = 0;

  while (!url.empty()) {
    if (++page > MAX_PAGES) {
      LOG_ERR("TDR", "Pagination limit reached (%d pages)", MAX_PAGES);
      break;  // Keep what we have rather than failing entirely
    }

    LOG_DBG("TDR", "Fetching page %d: %s", page, url.c_str());

    String responseBody;
    const int httpCode = getWithRetry(url, responseBody);

    if (httpCode == 401) return AUTH_FAILED;
    if (httpCode != 200) {
      LOG_ERR("TDR", "Fetch failed: %d", httpCode);
      // If we already have items from previous pages, keep them
      if (!outItems.empty()) {
        LOG_DBG("TDR", "Keeping %zu items from previous pages", outItems.size());
        break;
      }
      return (httpCode < 0) ? NETWORK_ERROR : SERVER_ERROR;
    }

    JsonDocument doc;
    const DeserializationError error = deserializeJson(doc, responseBody);
    if (error) {
      LOG_ERR("TDR", "JSON parse failed: %s", error.c_str());
      if (!outItems.empty()) break;  // Keep partial results
      return JSON_ERROR;
    }

    JsonArray results = doc["results"].as<JsonArray>();
    for (JsonObject entry : results) {
      ShoppingListItem item;
      item.id = entry["id"] | 0;
      item.checked = entry["checked"] | false;
      item.amount = entry["amount"] | 0.0f;

      JsonObject food = entry["food"].as<JsonObject>();
      if (food) {
        item.foodName = food["name"] | "";
        JsonObject cat = food["supermarket_category"].as<JsonObject>();
        if (cat) {
          item.category = cat["name"] | "";
        }
      }

      JsonObject unit = entry["unit"].as<JsonObject>();
      if (unit) {
        item.unitName = unit["name"] | "";
      }

      // Skip entries with no food name — malformed data
      if (item.foodName.empty()) continue;

      outItems.push_back(std::move(item));
    }

    const char* nextUrl = doc["next"] | static_cast<const char*>(nullptr);
    url = nextUrl ? std::string(nextUrl) : "";
  }

  // Sort: unchecked first, then by category
  std::sort(outItems.begin(), outItems.end(), [](const ShoppingListItem& a, const ShoppingListItem& b) {
    if (a.checked != b.checked) return !a.checked;
    return a.category < b.category;
  });

  LOG_DBG("TDR", "Fetched %zu shopping list items", outItems.size());
  return OK;
}

TandoorClient::Error TandoorClient::setItemChecked(int itemId, bool checked) {
  if (!TANDOOR_STORE.hasCredentials()) return NO_CREDENTIALS;
  if (!isWifiConnected()) return NETWORK_ERROR;

  char urlBuf[256];
  snprintf(urlBuf, sizeof(urlBuf), "%s/shopping-list-entry/%d/?format=json", TANDOOR_STORE.getApiBaseUrl().c_str(),
           itemId);
  std::string url(urlBuf);

  char body[32];
  snprintf(body, sizeof(body), "{\"checked\":%s}", checked ? "true" : "false");

  int lastCode = -1;
  for (int attempt = 0; attempt <= MAX_RETRIES; attempt++) {
    if (attempt > 0) {
      if (!isWifiConnected()) return NETWORK_ERROR;
      LOG_DBG("TDR", "PATCH retry %d/%d", attempt, MAX_RETRIES);
      vTaskDelay(pdMS_TO_TICKS(RETRY_DELAY_MS));
    }

    HTTPClient http;
    std::unique_ptr<WiFiClientSecure> secureClient;
    WiFiClient plainClient;
    beginRequest(http, secureClient, plainClient, url);
    http.addHeader("Content-Type", "application/json");

    lastCode = http.PATCH(reinterpret_cast<uint8_t*>(body), strlen(body));
    http.end();

    if (lastCode == 200) return OK;
    if (lastCode == 401) return AUTH_FAILED;
    if (lastCode == 404) return NOT_FOUND;
    if (!isTransientError(lastCode)) break;

    LOG_DBG("TDR", "PATCH transient error: %d", lastCode);
  }

  LOG_ERR("TDR", "PATCH failed: %d", lastCode);
  return (lastCode < 0) ? NETWORK_ERROR : SERVER_ERROR;
}

const char* TandoorClient::errorString(Error error) {
  switch (error) {
    case OK:
      return "Success";
    case NO_CREDENTIALS:
      return TANDOOR_STORE.getConfigError().empty() ? "Missing /.crosspoint/tandoor.json"
                                                    : TANDOOR_STORE.getConfigError().c_str();
    case NETWORK_ERROR:
      return "Network error";
    case AUTH_FAILED:
      return "Authentication failed";
    case NOT_FOUND:
      return "Item not found on server";
    case SERVER_ERROR:
      return "Server error";
    case JSON_ERROR:
      return "JSON parse error";
    default:
      return "Unknown error";
  }
}
