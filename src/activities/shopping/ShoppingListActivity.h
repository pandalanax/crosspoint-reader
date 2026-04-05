#pragma once

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

  // When true, the user is actively browsing the list (prevent auto-sleep).
  // Set to false after a refresh completes so the device can sleep.
  // Set to true again on any navigation or check-off action.
  bool userActive = false;

  // Category header tracking for rendering
  struct DisplayRow {
    enum Type { CATEGORY_HEADER, ITEM };
    Type type;
    size_t itemIndex;        // Index into items vector (for ITEM type)
    std::string headerText;  // Category name (for CATEGORY_HEADER type)
  };
  std::vector<DisplayRow> displayRows;

  void buildDisplayRows();
  void onWifiComplete(bool connected);
  void fetchList();
  void triggerRefresh();
  void toggleCurrentItem();
  bool saveCacheToSd() const;
  bool loadCacheFromSd();

 public:
  explicit ShoppingListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("ShoppingList", renderer, mappedInput) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return userActive; }
};
