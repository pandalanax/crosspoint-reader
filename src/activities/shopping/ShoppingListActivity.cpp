#include "ShoppingListActivity.h"

#include <ArduinoJson.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include <algorithm>
#include <cstdio>

#include "MappedInputManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "tandoor/TandoorCredentialStore.h"

namespace {
constexpr char CACHE_FILE[] = "/.crosspoint/shopping_list.json";
}  // namespace

std::string ShoppingListActivity::cacheMissingMessage() {
  return "Missing /.crosspoint/shopping_list.json";
}

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
    errorMessage = TANDOOR_STORE.getConfigError().empty() ? "Missing /.crosspoint/tandoor.json"
                                                          : TANDOOR_STORE.getConfigError();
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

  std::vector<int> remainingPendingIds;
  remainingPendingIds.reserve(pendingIds.size());
  for (int itemId : pendingIds) {
    const auto err = TandoorClient::setItemChecked(itemId, true);
    if (err == TandoorClient::OK || err == TandoorClient::NOT_FOUND) {
      continue;
    }
    LOG_ERR("SHOP", "Sync failed for item %d: %s", itemId, TandoorClient::errorString(err));
    remainingPendingIds.push_back(itemId);
  }

  std::vector<ShoppingListItem> fetchedItems;
  TandoorClient::Error err = TandoorClient::fetchShoppingList(fetchedItems);
  if (err != TandoorClient::OK) {
    // Try cache as fallback
    if (loadCacheFromSd()) {
      buildDisplayRows();
      state = State::DISPLAYING;
      LOG_DBG("SHOP", "Fetch failed (%s), using cache", TandoorClient::errorString(err));
    } else {
      state = State::ERROR;
      errorMessage = fetchFailedNoCacheMessage(TandoorClient::errorString(err));
    }
    requestUpdate();
    return;
  }

  mergeFetchedItems(std::move(fetchedItems), localItems, remainingPendingIds);
  saveCacheToSd();
  buildDisplayRows();
  selectorIndex = 0;
  state = State::DISPLAYING;
  userActive = false;  // Allow auto-sleep after refresh — user can put device down

  // Turn off WiFi to save power while shopping
  WiFi.disconnect();
  WiFi.mode(WIFI_OFF);

  requestUpdate();
}

void ShoppingListActivity::mergeFetchedItems(std::vector<ShoppingListItem>&& fetchedItems,
                                             const std::vector<ShoppingListItem>& localItems,
                                             const std::vector<int>& pendingIds) {
  items.clear();
  items.reserve(fetchedItems.size() + pendingIds.size());
  std::vector<int> fetchedIds;
  std::vector<int> serverCheckedIds;
  fetchedIds.reserve(fetchedItems.size());
  serverCheckedIds.reserve(fetchedItems.size());

  for (auto& fetched : fetchedItems) {
    fetchedIds.push_back(fetched.id);
    if (fetched.checked) {
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
}

void ShoppingListActivity::buildDisplayRows() {
  displayRows.clear();
  if (items.empty()) return;

  // Reserve estimated size: items + category headers
  displayRows.reserve(items.size() + 16);

  std::string lastCategory;
  for (size_t i = 0; i < items.size(); i++) {
    const auto& item = items[i];
    // Insert category header when category changes
    if (item.category != lastCategory) {
      DisplayRow header;
      header.type = DisplayRow::CATEGORY_HEADER;
      header.itemIndex = 0;
      header.headerText = item.category.empty() ? "Other" : item.category;
      displayRows.push_back(std::move(header));
      lastCategory = item.category;
    }
    DisplayRow row;
    row.type = DisplayRow::ITEM;
    row.itemIndex = i;
    displayRows.push_back(std::move(row));
  }
}

void ShoppingListActivity::toggleCurrentItem() {
  if (displayRows.empty()) return;
  const auto& row = displayRows[selectorIndex];
  if (row.type != DisplayRow::ITEM) return;

  auto& item = items[row.itemIndex];
  item.checked = !item.checked;
  item.pendingSync = item.checked;
  userActive = true;

  // Local toggle stays visible until a later refresh confirms sync.
  saveCacheToSd();
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
                   if (row.type == DisplayRow::CATEGORY_HEADER) {
                     return "-- " + row.headerText + " --";
                   }
                   const auto& item = items[row.itemIndex];
                   char amountBuf[16];
                   if (item.amount == static_cast<int>(item.amount)) {
                     snprintf(amountBuf, sizeof(amountBuf), "%d", static_cast<int>(item.amount));
                   } else {
                     snprintf(amountBuf, sizeof(amountBuf), "%.1f", item.amount);
                   }
                   std::string line = amountBuf;
                   if (!item.unitName.empty()) {
                     line += " " + item.unitName;
                   }
                   line += " " + item.foodName;
                   return line;
                 });
  }

  // Draw strikethrough lines over checked items
  if (!displayRows.empty()) {
    const int rowHeight = metrics.listRowHeight;
    const int pageItems = listRect.height / rowHeight;
    const int pageStart = static_cast<int>(selectorIndex) / pageItems * pageItems;
    const int rowCount = static_cast<int>(displayRows.size());
    const int textH = renderer.getLineHeight(UI_10_FONT_ID);
    const int padX = metrics.contentSidePadding;

    for (int i = pageStart; i < rowCount && i < pageStart + pageItems; i++) {
      const auto& row = displayRows[i];
      if (row.type != DisplayRow::ITEM) continue;
      if (!items[row.itemIndex].checked) continue;

      int itemY = listRect.y + (i % pageItems) * rowHeight;
      int lineY = itemY + textH / 2;
      bool inverted = (i == static_cast<int>(selectorIndex));
      renderer.drawLine(padX, lineY, pageWidth - padX * 2, lineY, !inverted);
    }
  }

  if (state == State::WIFI_SELECTION) {
    GUI.drawPopup(renderer, "Connecting to WiFi...");
  } else if (state == State::FETCHING) {
    GUI.drawPopup(renderer, "Fetching shopping list...");
  } else if (state == State::ERROR) {
    GUI.drawPopup(renderer, errorMessage.c_str());
  }

  // Button hints — long-press Back refreshes
  const auto labels = mappedInput.mapLabels("Back/Refresh", tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
