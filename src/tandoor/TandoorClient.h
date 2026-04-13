#pragma once
#include <cstdint>
#include <string>
#include <vector>

/**
 * A single shopping list entry from Tandoor Recipes.
 */
struct ShoppingListItem {
  int id;
  std::string foodName;
  std::string category;  // supermarket_category name (e.g. "Rewe", "DM")
  std::string unitName;  // nullable — may be empty
  float amount;
  bool checked;
  bool pendingSync = false;
};

/**
 * HTTP client for Tandoor Recipes shopping list API.
 *
 * API Endpoints:
 *   GET  /api/shopping-list-entry/?format=json&page_size=200  - Get all entries
 *   PATCH /api/shopping-list-entry/{id}/?format=json          - Update an entry
 *
 * Authentication:
 *   Authorization: Bearer <api_token>
 */
class TandoorClient {
 public:
  enum Error { OK = 0, NO_CREDENTIALS, NETWORK_ERROR, AUTH_FAILED, NOT_FOUND, SERVER_ERROR, JSON_ERROR };

  /**
   * Fetch all shopping list entries (unchecked first, then checked).
   * Items are grouped by supermarket category.
   * @param outItems Output: the shopping list items
   * @return OK on success, error code on failure
   */
  static Error fetchShoppingList(std::vector<ShoppingListItem>& outItems);

  /**
   * Toggle the checked state of a shopping list entry.
   * @param itemId The entry ID
   * @param checked The new checked state
   * @return OK on success, error code on failure
   */
  static Error setItemChecked(int itemId, bool checked);

  static const char* errorString(Error error);
};
