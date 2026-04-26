#include "ShoppingListActivity.h"

#include <ArduinoJson.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include <algorithm>
#include <cstdio>
#include <unordered_set>

#include "MappedInputManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "tandoor/TandoorCredentialStore.h"

namespace {
constexpr char CACHE_FILE[] = "/.crosspoint/shopping_list.json";
}  // namespace

std::string ShoppingListActivity::cacheMissingMessage() { return "Missing /.crosspoint/shopping_list.json"; }

std::string ShoppingListActivity::wifiRequiredMessage() {
  return "WiFi required. No cache at /.crosspoint/shopping_list.json";
}

std::string ShoppingListActivity::fetchFailedNoCacheMessage(const char* fetchError) {
  std::string msg = fetchError ? fetchError : "Shopping list fetch failed";
  msg += ". No cache at /.crosspoint/shopping_list.json";
  return msg;
}

bool ShoppingListActivity::containsId(const std::vector<int>& ids, int id) {
  return std::find(ids.begin(), ids.end(), id) != ids.end();
}

void ShoppingListActivity::onEnter() {
  Activity::onEnter();

  if (!TANDOOR_STORE.hasCredentials()) {
    state = State::ERROR;
    errorMessage =
        TANDOOR_STORE.getConfigError().empty() ? "Missing /.crosspoint/tandoor.json" : TANDOOR_STORE.getConfigError();
    requestUpdate();
    return;
  }

  // Try loading from cache first for instant display
  if (loadCacheFromSd()) {
    buildDisplayRows();
    state = State::DISPLAYING;
    userActive = true;  // User just opened list — prevent sleep while browsing
    requestUpdate();
    return;
  }

  // No cache — need WiFi to fetch
  state = State::WIFI_SELECTION;
  requestUpdate();

  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiComplete(!result.isCancelled); });
}

void ShoppingListActivity::onExit() {
  // Save current state to cache before leaving
  if (!items.empty()) {
    saveCacheToSd();
  }
  popupUntilMs = 0;
  popupText.clear();
  items.clear();
  displayRows.clear();
  WiFi.mode(WIFI_OFF);
  Activity::onExit();
}

void ShoppingListActivity::onWifiComplete(bool connected) {
  if (connected) {
    state = State::FETCHING;
    requestUpdateAndWait();
    fetchList();
  } else {
    WiFi.disconnect();
    WiFi.mode(WIFI_OFF);

    // Try cache as fallback
    if (loadCacheFromSd()) {
      buildDisplayRows();
      state = State::DISPLAYING;
    } else {
      state = State::ERROR;
      errorMessage = wifiRequiredMessage();
    }
    requestUpdate();
  }
}

void ShoppingListActivity::fetchList() {
  const std::vector<ShoppingListItem> localItems = items;
  std::vector<int> pendingIds;
  pendingIds.reserve(localItems.size());
  for (const auto& item : localItems) {
    if (item.checked && item.pendingSync) {
      pendingIds.push_back(item.id);
    }
  }

  std::vector<int> syncedIds;
  std::vector<int> remainingPendingIds;

  if (!pendingIds.empty()) {
    auto bulkErr = TandoorClient::bulkSetChecked(pendingIds, true);
    if (bulkErr == TandoorClient::OK) {
      syncedIds = pendingIds;
    } else if (bulkErr == TandoorClient::AUTH_FAILED || bulkErr == TandoorClient::NETWORK_ERROR) {
      remainingPendingIds = pendingIds;
    } else {
      LOG_DBG("SHOP", "Bulk sync unavailable, trying individual");
      syncedIds.reserve(pendingIds.size());
      remainingPendingIds.reserve(pendingIds.size());
      for (int itemId : pendingIds) {
        auto itemErr = TandoorClient::setItemChecked(itemId, true);
        if (itemErr == TandoorClient::OK || itemErr == TandoorClient::NOT_FOUND) {
          syncedIds.push_back(itemId);
        } else {
          LOG_ERR("SHOP", "Sync failed for item %d: %s", itemId, TandoorClient::errorString(itemErr));
          remainingPendingIds.push_back(itemId);
        }
      }
    }
  }

  if (!syncedIds.empty()) {
    items.erase(std::remove_if(items.begin(), items.end(),
                               [&syncedIds](const ShoppingListItem& item) {
                                 return item.checked && item.pendingSync && containsId(syncedIds, item.id);
                               }),
                items.end());
    saveCacheToSd();
    vTaskDelay(pdMS_TO_TICKS(500));
  }

  std::vector<ShoppingListItem> fetchedItems;
  TandoorClient::Error fetchErr = TandoorClient::fetchShoppingList(fetchedItems);
  if (fetchErr != TandoorClient::OK) {
    WiFi.disconnect();
    WiFi.mode(WIFI_OFF);
    if (loadCacheFromSd()) {
      buildDisplayRows();
      state = State::DISPLAYING;
      popupText = std::string(TandoorClient::errorString(fetchErr)) + " — showing cached list";
      popupUntilMs = millis() + POPUP_DURATION_MS * 2;
      LOG_DBG("SHOP", "Fetch failed (%s), using cache", TandoorClient::errorString(fetchErr));
    } else {
      state = State::ERROR;
      errorMessage = fetchFailedNoCacheMessage(TandoorClient::errorString(fetchErr));
    }
    requestUpdate();
    return;
  }

  mergeFetchedItems(std::move(fetchedItems), localItems, remainingPendingIds, syncedIds);
  saveCacheToSd();
  buildDisplayRows();
  selectorIndex = 0;
  state = State::DISPLAYING;
  userActive = false;  // Allow auto-sleep after refresh — user can put device down
  popupText = "Refresh successful";
  popupUntilMs = millis() + POPUP_DURATION_MS;

  // Turn off WiFi to save power while shopping
  WiFi.disconnect();
  WiFi.mode(WIFI_OFF);

  requestUpdate();
}

void ShoppingListActivity::mergeFetchedItems(std::vector<ShoppingListItem>&& fetchedItems,
                                             const std::vector<ShoppingListItem>& localItems,
                                             const std::vector<int>& pendingIds,
                                             const std::vector<int>& syncedIds) {
  items.clear();
  items.reserve(fetchedItems.size() + pendingIds.size());
  std::vector<int> fetchedIds;
  std::vector<int> serverCheckedIds;
  fetchedIds.reserve(fetchedItems.size());
  serverCheckedIds.reserve(fetchedItems.size());

  for (auto& fetched : fetchedItems) {
    fetchedIds.push_back(fetched.id);
    if (fetched.checked || containsId(syncedIds, fetched.id)) {
      serverCheckedIds.push_back(fetched.id);
      continue;
    }

    fetched.pendingSync = false;
    if (containsId(pendingIds, fetched.id)) {
      fetched.checked = true;
      fetched.pendingSync = true;
    } else {
      fetched.checked = false;
    }
    items.push_back(std::move(fetched));
  }

  for (const auto& local : localItems) {
    if (!(local.checked && local.pendingSync) || !containsId(pendingIds, local.id)) {
      continue;
    }
    if (containsId(serverCheckedIds, local.id)) {
      continue;
    }

    const auto exists = std::find_if(items.begin(), items.end(),
                                     [itemId = local.id](const ShoppingListItem& item) { return item.id == itemId; });
    if (exists == items.end() && !containsId(fetchedIds, local.id)) {
      items.push_back(local);
    }
  }

  std::sort(items.begin(), items.end(), [](const ShoppingListItem& a, const ShoppingListItem& b) {
    if (a.checked != b.checked) return !a.checked;
    if (a.category != b.category) return a.category < b.category;
    return a.foodName < b.foodName;
  });

  std::unordered_set<int> seenIds;
  seenIds.reserve(items.size());
  items.erase(std::remove_if(items.begin(), items.end(),
                             [&seenIds](const ShoppingListItem& item) { return !seenIds.insert(item.id).second; }),
              items.end());
}

void ShoppingListActivity::buildDisplayRows() {
  displayRows.clear();
  if (items.empty()) return;

  // Reserve estimated size: items + category headers
  displayRows.reserve(items.size() + 16);

  std::string lastCategory;
  for (size_t i = 0; i < items.size();) {
    const auto& item = items[i];
    // Insert category header when category changes
    if (item.category != lastCategory) {
      DisplayRow header;
      header.type = DisplayRow::CATEGORY_HEADER;
      header.headerText = item.category.empty() ? "Other" : item.category;
      displayRows.push_back(std::move(header));
      lastCategory = item.category;
    }

    std::vector<size_t> groupedIndices;
    groupedIndices.push_back(i);
    size_t j = i + 1;
    while (j < items.size() && items[j].category == item.category && items[j].foodName == item.foodName &&
           items[j].unitName == item.unitName && items[j].amount == item.amount && items[j].checked == item.checked) {
      groupedIndices.push_back(j);
      j++;
    }

    DisplayRow row;
    row.type = DisplayRow::ITEM;
    row.checked = item.checked;
    row.itemIndices = groupedIndices;
    row.lineText = buildGroupedLineText(groupedIndices);
    displayRows.push_back(std::move(row));
    i = j;
  }
}

std::string ShoppingListActivity::buildGroupedLineText(const std::vector<size_t>& itemIndices) const {
  if (itemIndices.empty()) return "";

  const auto& first = items[itemIndices.front()];
  const size_t groupCount = itemIndices.size();
  float displayAmount = 0.0f;
  if (first.amount > 0.0f) {
    displayAmount = first.amount * static_cast<float>(groupCount);
  } else {
    displayAmount = static_cast<float>(groupCount);
  }

  char amountBuf[16];
  if (displayAmount == static_cast<int>(displayAmount)) {
    snprintf(amountBuf, sizeof(amountBuf), "%d", static_cast<int>(displayAmount));
  } else {
    snprintf(amountBuf, sizeof(amountBuf), "%.1f", displayAmount);
  }

  std::string line = amountBuf;
  if (!first.unitName.empty()) {
    line += " ";
    line += first.unitName;
  }
  line += " ";
  line += first.foodName;
  return line;
}

void ShoppingListActivity::toggleCurrentItem() {
  if (displayRows.empty()) return;
  const auto& row = displayRows[selectorIndex];
  if (row.type != DisplayRow::ITEM) return;

  const bool newChecked = !row.checked;
  for (size_t itemIndex : row.itemIndices) {
    items[itemIndex].checked = newChecked;
    items[itemIndex].pendingSync = newChecked;
  }
  userActive = true;

  // Local toggle stays visible until a later refresh confirms sync.
  saveCacheToSd();
  buildDisplayRows();
  requestUpdate();
}

void ShoppingListActivity::triggerRefresh() {
  state = State::WIFI_SELECTION;
  requestUpdateAndWait();

  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiComplete(!result.isCancelled); });
}

bool ShoppingListActivity::saveCacheToSd() const {
  Storage.mkdir("/.crosspoint");

  JsonDocument doc;
  JsonArray arr = doc["items"].to<JsonArray>();
  for (const auto& item : items) {
    JsonObject obj = arr.add<JsonObject>();
    obj["id"] = item.id;
    obj["food"] = item.foodName;
    obj["category"] = item.category;
    obj["unit"] = item.unitName;
    obj["amount"] = item.amount;
    obj["checked"] = item.checked;
    obj["pendingSync"] = item.pendingSync;
  }

  String json;
  serializeJson(doc, json);
  bool ok = Storage.writeFile(CACHE_FILE, json);
  if (ok) {
    LOG_DBG("SHOP", "Saved %zu items to cache", items.size());
  } else {
    LOG_ERR("SHOP", "Failed to save cache");
  }
  return ok;
}

bool ShoppingListActivity::loadCacheFromSd() {
  if (!Storage.exists(CACHE_FILE)) {
    errorMessage = cacheMissingMessage();
    return false;
  }

  String json = Storage.readFile(CACHE_FILE);
  if (json.isEmpty()) {
    errorMessage = "Empty /.crosspoint/shopping_list.json";
    return false;
  }

  JsonDocument doc;
  auto error = deserializeJson(doc, json);
  if (error) {
    LOG_ERR("SHOP", "Cache parse failed: %s", error.c_str());
    errorMessage = "Invalid /.crosspoint/shopping_list.json";
    return false;
  }

  items.clear();
  JsonArray arr = doc["items"].as<JsonArray>();
  items.reserve(arr.size());

  for (JsonObject obj : arr) {
    ShoppingListItem item;
    item.id = obj["id"] | 0;
    item.foodName = obj["food"] | "";
    item.category = obj["category"] | "";
    item.unitName = obj["unit"] | "";
    item.amount = obj["amount"] | 0.0f;
    item.checked = obj["checked"] | false;
    item.pendingSync = obj["pendingSync"].is<bool>() ? static_cast<bool>(obj["pendingSync"]) : item.checked;
    items.push_back(std::move(item));
  }

  LOG_DBG("SHOP", "Loaded %zu items from cache", items.size());
  if (items.empty()) {
    errorMessage = "No items in /.crosspoint/shopping_list.json";
    return false;
  }
  return !items.empty();
}

void ShoppingListActivity::loop() {
  if (popupUntilMs != 0 && millis() >= popupUntilMs) {
    popupUntilMs = 0;
    requestUpdate();
  }

  if (state != State::DISPLAYING) return;

  const int rowCount = static_cast<int>(displayRows.size());
  if (rowCount == 0) return;

  const int pageItems = UITheme::getInstance().getNumberOfItemsPerPage(renderer, true, false, true, false);

  // Confirm toggles the checked state
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    toggleCurrentItem();
    return;
  }

  // Back: short press = home, long press (1s+) = refresh from Tandoor
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (mappedInput.getHeldTime() >= 1000) {
      triggerRefresh();
    } else {
      onGoHome();
    }
    return;
  }

  // Navigation — skip category headers when pressing up/down
  auto skipToItem = [this, rowCount](int idx, int direction) {
    for (int i = 0; i < rowCount; i++) {
      if (idx < 0) idx = rowCount - 1;
      if (idx >= rowCount) idx = 0;
      if (displayRows[idx].type == DisplayRow::ITEM) return idx;
      idx += direction;
    }
    return idx;  // Fallback — shouldn't happen unless all rows are headers
  };

  buttonNavigator.onNextRelease([this, rowCount, skipToItem] {
    int next = ButtonNavigator::nextIndex(static_cast<int>(selectorIndex), rowCount);
    selectorIndex = skipToItem(next, 1);
    userActive = true;
    requestUpdate();
  });

  buttonNavigator.onPreviousRelease([this, rowCount, skipToItem] {
    int prev = ButtonNavigator::previousIndex(static_cast<int>(selectorIndex), rowCount);
    selectorIndex = skipToItem(prev, -1);
    userActive = true;
    requestUpdate();
  });

  buttonNavigator.onNextContinuous([this, rowCount, pageItems, skipToItem] {
    int next = ButtonNavigator::nextPageIndex(static_cast<int>(selectorIndex), rowCount, pageItems);
    selectorIndex = skipToItem(next, 1);
    userActive = true;
    requestUpdate();
  });

  buttonNavigator.onPreviousContinuous([this, rowCount, pageItems, skipToItem] {
    int prev = ButtonNavigator::previousPageIndex(static_cast<int>(selectorIndex), rowCount, pageItems);
    selectorIndex = skipToItem(prev, -1);
    userActive = true;
    requestUpdate();
  });
}

void ShoppingListActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  // Header
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "Shopping List");

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;

  // State::DISPLAYING
  if (displayRows.empty() && state == State::DISPLAYING) {
    renderer.drawText(UI_12_FONT_ID, metrics.contentSidePadding, contentTop + 20, "Shopping list is empty");
  }

  const Rect listRect{0, contentTop, pageWidth, contentHeight};
  if (!displayRows.empty()) {
    GUI.drawList(renderer, listRect, static_cast<int>(displayRows.size()), static_cast<int>(selectorIndex),
                 [this](int index) -> std::string {
                   const auto& row = displayRows[index];
                   return row.type == DisplayRow::CATEGORY_HEADER ? "-- " + row.headerText + " --" : row.lineText;
                 });
  }

  // Checked items stay visible until sync, but render them as inverted rows so the state is obvious on e-ink.
  if (!displayRows.empty()) {
    const int rowHeight = metrics.listRowHeight;
    const int pageItems = listRect.height / rowHeight;
    const int pageStart = static_cast<int>(selectorIndex) / pageItems * pageItems;
    const int rowCount = static_cast<int>(displayRows.size());
    const int totalPages = (rowCount + pageItems - 1) / pageItems;
    const bool lyraStyle =
        SETTINGS.uiTheme == CrossPointSettings::UI_THEME::LYRA || SETTINGS.uiTheme == CrossPointSettings::UI_THEME::LYRA_3_COVERS;
    const int contentWidth =
        listRect.width - (lyraStyle && totalPages > 1 ? (metrics.scrollBarWidth + metrics.scrollBarRightOffset) : 5);
    const int rowTextX = lyraStyle ? listRect.x + metrics.contentSidePadding + 8 : listRect.x + metrics.contentSidePadding;
    const int rowTextYAdjust = lyraStyle ? 7 : 0;
    const int rowTextWidth = lyraStyle ? contentWidth - metrics.contentSidePadding * 2 - 16
                                       : contentWidth - metrics.contentSidePadding * 2;

    for (int i = pageStart; i < rowCount && i < pageStart + pageItems; i++) {
      const auto& row = displayRows[i];
      if (row.type != DisplayRow::ITEM) continue;
      if (!row.checked) continue;

      int itemY = listRect.y + (i % pageItems) * rowHeight;
      const bool selected = (i == static_cast<int>(selectorIndex));
      if (lyraStyle) {
        renderer.fillRoundedRect(metrics.contentSidePadding, itemY, contentWidth - metrics.contentSidePadding * 2,
                                 rowHeight, 6, Color::Black);
      } else {
        renderer.fillRect(0, itemY - 2, listRect.width, rowHeight, true);
      }

      std::string text = renderer.truncatedText(UI_10_FONT_ID, row.lineText.c_str(), rowTextWidth);
      renderer.drawText(UI_10_FONT_ID, rowTextX, itemY + rowTextYAdjust, text.c_str(), false);
    }
  }

  if (state == State::WIFI_SELECTION) {
    GUI.drawPopup(renderer, "Connecting to WiFi...");
  } else if (state == State::FETCHING) {
    GUI.drawPopup(renderer, "Fetching shopping list...");
  } else if (state == State::ERROR) {
    GUI.drawPopup(renderer, errorMessage.c_str());
  } else if (popupUntilMs != 0) {
    GUI.drawPopup(renderer, popupText.c_str());
  }

  // Button hints — long-press Back refreshes
  const auto labels = mappedInput.mapLabels("Back/Sync", tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
