#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "tandoor/TandoorClient.h"
#include "util/ButtonNavigator.h"

/**
 * ShoppingListActivity fetches the Tandoor Recipes shopping list over WiFi,
 * caches it locally, and displays a checklist the user can navigate with
 * buttons.
 *
 * Flow:
 *   1. Connect to WiFi (reuses WifiSelectionActivity)
 *   2. Fetch shopping list from Tandoor API
 *   3. Cache to SD card for offline use, disconnect WiFi
 *   4. Display checklist — navigate with up/down, confirm to check off
 *   5. Long-press Back to refresh (reconnects WiFi, re-fetches)
 *
 * Sleep behavior:
 *   - After a refresh completes, auto-sleep is allowed (normal timeout)
 *   - Any button press (navigate/check) marks the user as active → prevents sleep
 *   - This lets you refresh, put it down, and it sleeps on its own
 */
class ShoppingListActivity final : public Activity {
  enum class State {
    WIFI_SELECTION,
    FETCHING,
    DISPLAYING,
    ERROR,
  };

  ButtonNavigator buttonNavigator;
  State state = State::WIFI_SELECTION;
  size_t selectorIndex = 0;
  std::vector<ShoppingListItem> items;
  std::string errorMessage;
  unsigned long popupUntilMs = 0;
  std::string popupText;
  static constexpr unsigned long POPUP_DURATION_MS = 2000;

  // When true, the user is actively browsing the list (prevent auto-sleep).
  // Set to false after a refresh completes so the device can sleep.
  // Set to true again on any navigation or check-off action.
  bool userActive = false;

  // Category header tracking for rendering
  struct DisplayRow {
    enum Type { CATEGORY_HEADER, ITEM };
    Type type;
    std::vector<size_t> itemIndices;  // Indices into items vector (for ITEM type)
    std::string headerText;  // Category name (for CATEGORY_HEADER type)
    std::string lineText;    // Precomputed display text (for ITEM type)
    bool checked = false;
  };
  std::vector<DisplayRow> displayRows;

  void buildDisplayRows();
  void onWifiComplete(bool connected);
  void fetchList();
  void triggerRefresh();
  void toggleCurrentItem();
  std::string buildGroupedLineText(const std::vector<size_t>& itemIndices) const;
  bool saveCacheToSd() const;
  bool loadCacheFromSd();
  void mergeFetchedItems(std::vector<ShoppingListItem>&& fetchedItems, const std::vector<ShoppingListItem>& localItems,
                         const std::vector<int>& pendingIds, const std::vector<int>& syncedIds);
  static bool containsId(const std::vector<int>& ids, int id);
  static std::string cacheMissingMessage();
  static std::string wifiRequiredMessage();
  static std::string fetchFailedNoCacheMessage(const char* fetchError);

 public:
  explicit ShoppingListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("ShoppingList", renderer, mappedInput) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return userActive; }
};
