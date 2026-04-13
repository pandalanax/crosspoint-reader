#pragma once

#include <array>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

/**
 * ContactsActivity displays a contact list loaded from a JSON file on SD card.
 *
 * The JSON is generated from a vCard file by scripts/vcf_to_contacts_json.py
 * and placed at /.crosspoint/contacts.json on the SD card.
 *
 * List view: alphabetical scroll with letter headers, PageFwd/PageBack to jump letters.
 * Detail view: press Confirm to see phone numbers, email, org.
 */
class ContactsActivity final : public Activity {
  enum class State { LIST, DETAIL, ERROR };

  struct Contact {
    std::string name;
    std::string phones;  // semicolon-delimited
    std::string emails;  // semicolon-delimited
    std::string org;
  };

  struct DisplayRow {
    enum Type { LETTER_HEADER, CONTACT };
    Type type;
    size_t contactIndex;
    char letter;  // for LETTER_HEADER
  };

  ButtonNavigator buttonNavigator;
  State state = State::ERROR;
  std::vector<Contact> contacts;
  std::vector<DisplayRow> displayRows;
  std::string errorMessage;
  size_t selectorIndex = 0;
  size_t detailScrollIndex = 0;
  bool userActive = false;

  // Index into displayRows for each letter A-Z (0-25), -1 if absent.
  // Entry 26 is for '#' (non-alpha).
  std::array<int, 27> letterStartIndex{};

  bool loadFromSd();
  void buildDisplayRows();
  int findNextLetterIndex(int fromRow, int direction);
  void renderList();
  void renderDetailPopup();

 public:
  explicit ContactsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Contacts", renderer, mappedInput) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return userActive; }
};
