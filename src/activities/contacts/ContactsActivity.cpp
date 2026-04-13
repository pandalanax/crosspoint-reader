#include "ContactsActivity.h"

#include <ArduinoJson.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>

#include <cstdio>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr char CONTACTS_FILE[] = "/.crosspoint/contacts.json";
constexpr int MAX_CONTACTS = 1000;
constexpr int MAX_STRING_LEN = 256;
}  // namespace

// ---- Data loading ----

bool ContactsActivity::loadFromSd() {
  if (!Storage.exists(CONTACTS_FILE)) {
    errorMessage = "Missing /.crosspoint/contacts.json";
    return false;
  }

  String json = Storage.readFile(CONTACTS_FILE);
  if (json.isEmpty()) {
    errorMessage = "Empty /.crosspoint/contacts.json";
    return false;
  }

  JsonDocument doc;
  auto error = deserializeJson(doc, json);
  if (error) {
    LOG_ERR("CONTACTS", "Parse failed: %s", error.c_str());
    errorMessage = "Invalid /.crosspoint/contacts.json";
    return false;
  }

  JsonArray arr = doc.as<JsonArray>();
  if (arr.isNull()) {
    errorMessage = "Expected JSON array in /.crosspoint/contacts.json";
    return false;
  }

  contacts.clear();
  contacts.reserve(std::min(static_cast<int>(arr.size()), MAX_CONTACTS));

  for (JsonObject obj : arr) {
    if (static_cast<int>(contacts.size()) >= MAX_CONTACTS) break;

    const char* name = obj["n"] | "";
    if (name[0] == '\0') continue;

    Contact c;
    c.name.assign(name, std::min(strlen(name), static_cast<size_t>(MAX_STRING_LEN)));

    // Join phone array into semicolon-delimited string
    JsonArray phones = obj["p"].as<JsonArray>();
    if (!phones.isNull()) {
      for (JsonVariant v : phones) {
        const char* p = v.as<const char*>();
        if (!p) continue;
        if (!c.phones.empty()) c.phones += ';';
        c.phones.append(p, std::min(strlen(p), static_cast<size_t>(64)));
      }
    }

    // Join email array
    JsonArray emails = obj["e"].as<JsonArray>();
    if (!emails.isNull()) {
      for (JsonVariant v : emails) {
        const char* e = v.as<const char*>();
        if (!e) continue;
        if (!c.emails.empty()) c.emails += ';';
        c.emails.append(e, std::min(strlen(e), static_cast<size_t>(128)));
      }
    }

    const char* org = obj["o"] | "";
    if (org[0] != '\0') {
      c.org.assign(org, std::min(strlen(org), static_cast<size_t>(MAX_STRING_LEN)));
    }

    contacts.push_back(std::move(c));
  }

  LOG_DBG("CONTACTS", "Loaded %zu contacts", contacts.size());
  if (contacts.empty()) {
    errorMessage = "No valid contacts in /.crosspoint/contacts.json";
    return false;
  }
  return !contacts.empty();
}

void ContactsActivity::buildDisplayRows() {
  displayRows.clear();
  displayRows.reserve(contacts.size() + 27);
  letterStartIndex.fill(-1);

  char lastLetter = '\0';

  for (size_t i = 0; i < contacts.size(); i++) {
    char first = contacts[i].name[0];
    char upper = (first >= 'a' && first <= 'z') ? (first - 32) : first;
    char letter;
    int letterIdx;

    if (upper >= 'A' && upper <= 'Z') {
      letter = upper;
      letterIdx = upper - 'A';
    } else {
      letter = '#';
      letterIdx = 26;
    }

    if (letter != lastLetter) {
      if (letterStartIndex[letterIdx] < 0) {
        letterStartIndex[letterIdx] = static_cast<int>(displayRows.size());
      }
      DisplayRow header;
      header.type = DisplayRow::LETTER_HEADER;
      header.contactIndex = 0;
      header.letter = letter;
      displayRows.push_back(header);
      lastLetter = letter;
    }

    DisplayRow row;
    row.type = DisplayRow::CONTACT;
    row.contactIndex = i;
    row.letter = letter;
    displayRows.push_back(row);
  }
}

int ContactsActivity::findNextLetterIndex(int fromRow, int direction) {
  if (displayRows.empty()) return -1;

  // Find which letter group we're currently in
  char currentLetter = displayRows[fromRow].letter;

  // Scan in direction for a different letter header
  int idx = fromRow + direction;
  int total = static_cast<int>(displayRows.size());

  while (idx >= 0 && idx < total) {
    if (displayRows[idx].type == DisplayRow::LETTER_HEADER && displayRows[idx].letter != currentLetter) {
      return idx;
    }
    idx += direction;
  }

  return -1;  // At boundary
}

// ---- Activity lifecycle ----

void ContactsActivity::onEnter() {
  Activity::onEnter();

  if (loadFromSd()) {
    buildDisplayRows();
    state = State::LIST;
    userActive = true;
  } else {
    state = State::ERROR;
  }

  requestUpdate();
}

void ContactsActivity::onExit() {
  contacts.clear();
  displayRows.clear();
  Activity::onExit();
}

// ---- Input handling ----

void ContactsActivity::loop() {
  if (state == State::DETAIL) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      state = State::LIST;
      userActive = true;
      requestUpdate();
      return;
    }
    // Scroll detail if needed (future: for contacts with many fields)
    return;
  }

  if (state != State::LIST) return;

  const int rowCount = static_cast<int>(displayRows.size());
  if (rowCount == 0) return;

  // Back: go home
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onGoHome();
    return;
  }

  // Confirm: open detail
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (displayRows[selectorIndex].type == DisplayRow::CONTACT) {
      detailScrollIndex = 0;
      state = State::DETAIL;
      userActive = true;
      requestUpdate();
    }
    return;
  }

  // PageForward: jump to next letter
  if (mappedInput.wasReleased(MappedInputManager::Button::PageForward)) {
    int next = findNextLetterIndex(static_cast<int>(selectorIndex), 1);
    if (next >= 0) {
      // Land on first contact after the header
      selectorIndex = (next + 1 < rowCount && displayRows[next + 1].type == DisplayRow::CONTACT)
                          ? static_cast<size_t>(next + 1)
                          : static_cast<size_t>(next);
      userActive = true;
      requestUpdate();
    }
    return;
  }

  // PageBack: jump to previous letter
  if (mappedInput.wasReleased(MappedInputManager::Button::PageBack)) {
    int prev = findNextLetterIndex(static_cast<int>(selectorIndex), -1);
    if (prev >= 0) {
      selectorIndex = (prev + 1 < rowCount && displayRows[prev + 1].type == DisplayRow::CONTACT)
                          ? static_cast<size_t>(prev + 1)
                          : static_cast<size_t>(prev);
      userActive = true;
      requestUpdate();
    }
    return;
  }

  // Up/Down navigation — skip letter headers
  const int pageItems = UITheme::getInstance().getNumberOfItemsPerPage(renderer, true, false, true, false);

  auto skipToContact = [this, rowCount](int idx, int direction) {
    for (int i = 0; i < rowCount; i++) {
      if (idx < 0) idx = rowCount - 1;
      if (idx >= rowCount) idx = 0;
      if (displayRows[idx].type == DisplayRow::CONTACT) return idx;
      idx += direction;
    }
    return idx;
  };

  buttonNavigator.onNextRelease([this, rowCount, skipToContact] {
    int next = ButtonNavigator::nextIndex(static_cast<int>(selectorIndex), rowCount);
    selectorIndex = skipToContact(next, 1);
    userActive = true;
    requestUpdate();
  });

  buttonNavigator.onPreviousRelease([this, rowCount, skipToContact] {
    int prev = ButtonNavigator::previousIndex(static_cast<int>(selectorIndex), rowCount);
    selectorIndex = skipToContact(prev, -1);
    userActive = true;
    requestUpdate();
  });

  buttonNavigator.onNextContinuous([this, rowCount, pageItems, skipToContact] {
    int next = ButtonNavigator::nextPageIndex(static_cast<int>(selectorIndex), rowCount, pageItems);
    selectorIndex = skipToContact(next, 1);
    userActive = true;
    requestUpdate();
  });

  buttonNavigator.onPreviousContinuous([this, rowCount, pageItems, skipToContact] {
    int prev = ButtonNavigator::previousPageIndex(static_cast<int>(selectorIndex), rowCount, pageItems);
    selectorIndex = skipToContact(prev, -1);
    userActive = true;
    requestUpdate();
  });
}

// ---- Rendering ----

void ContactsActivity::renderList() {
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  // Show current letter in header
  char headerBuf[32];
  if (!displayRows.empty()) {
    char letter = displayRows[selectorIndex].letter;
    snprintf(headerBuf, sizeof(headerBuf), "Contacts - %c", letter);
  } else {
    snprintf(headerBuf, sizeof(headerBuf), "Contacts");
  }
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, headerBuf);

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;

  if (displayRows.empty()) {
    renderer.drawText(UI_12_FONT_ID, metrics.contentSidePadding, contentTop + 20, "No contacts found");
  } else {
    GUI.drawList(renderer, Rect{0, contentTop, pageWidth, contentHeight}, static_cast<int>(displayRows.size()),
                 static_cast<int>(selectorIndex), [this](int index) -> std::string {
                   const auto& row = displayRows[index];
                   if (row.type == DisplayRow::LETTER_HEADER) {
                     char buf[8];
                     snprintf(buf, sizeof(buf), "-- %c --", row.letter);
                     return std::string(buf);
                   }
                   return contacts[row.contactIndex].name;
                 });
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), "Prev Ltr", "Next Ltr");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void ContactsActivity::renderDetail() {
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto& contact = contacts[displayRows[selectorIndex].contactIndex];

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, contact.name.c_str());

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int lineH = renderer.getLineHeight(UI_12_FONT_ID) + 4;
  const int padX = metrics.contentSidePadding;
  int y = contentTop + 8;

  // Phone numbers
  if (!contact.phones.empty()) {
    renderer.drawText(UI_10_FONT_ID, padX, y, "Phone:");
    y += lineH;

    // Split on semicolons and render each
    size_t start = 0;
    while (start < contact.phones.size()) {
      size_t end = contact.phones.find(';', start);
      if (end == std::string::npos) end = contact.phones.size();
      std::string phone = contact.phones.substr(start, end - start);
      renderer.drawText(UI_12_FONT_ID, padX + 12, y, phone.c_str());
      y += lineH;
      start = end + 1;
    }
    y += 4;
  }

  // Emails
  if (!contact.emails.empty()) {
    renderer.drawText(UI_10_FONT_ID, padX, y, "Email:");
    y += lineH;

    size_t start = 0;
    while (start < contact.emails.size()) {
      size_t end = contact.emails.find(';', start);
      if (end == std::string::npos) end = contact.emails.size();
      std::string email = contact.emails.substr(start, end - start);
      renderer.drawText(UI_12_FONT_ID, padX + 12, y, email.c_str());
      y += lineH;
      start = end + 1;
    }
    y += 4;
  }

  // Org
  if (!contact.org.empty()) {
    renderer.drawText(UI_10_FONT_ID, padX, y, "Org:");
    y += lineH;
    renderer.drawText(UI_12_FONT_ID, padX + 12, y, contact.org.c_str());
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void ContactsActivity::render(RenderLock&&) {
  renderer.clearScreen();

  if (state == State::ERROR) {
    const auto pageWidth = renderer.getScreenWidth();
    const auto& metrics = UITheme::getInstance().getMetrics();
    GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "Contacts");
    int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
    renderer.drawText(UI_12_FONT_ID, metrics.contentSidePadding, contentTop + 20, errorMessage.c_str());
    renderer.displayBuffer();
    return;
  }

  if (state == State::LIST) {
    renderList();
  } else if (state == State::DETAIL) {
    renderDetail();
  }

  renderer.displayBuffer();
}
