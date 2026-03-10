/*
 * Mini InkBook — E-Ink BLE Reader for VisionMaster e290 (Heltec)
 * Board: ESP32-S3 / VisionMaster e290
 *
 * Button scheme:
 *   Menus / Library / Bookmarks / Settings:
 *     1 click  = next item (down)
 *     2 clicks = previous item (up)
 *     hold     = select / confirm
 *     3s hold  = power off
 *
 *   Reading:
 *     1 click  = next page
 *     2 clicks = previous page
 *     hold     = open in-book overlay menu
 *     3s hold  = back to main menu
 *
 *   Overlay (while reading):
 *     1 click  = next option (down)
 *     2 clicks = previous option (up)
 *     hold     = confirm / select
 *     Options: Chapter Select | Bookmark | Start from beginning |
 *              Return to Main Menu | Back to book
 *
 *   Chapter Select:
 *     Chapters detected automatically from === Chapter Name === markers.
 *     1 click = next chapter, 2 clicks = previous, hold = jump to chapter.
 *
 *   Info screen:
 *     1 click  = scroll down
 *     2 clicks = scroll up
 *     hold     = back to main menu
 */

#include <Arduino.h>
#include <Adafruit_GFX.h>
#include <Fonts/FreeSerif9pt7b.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <FS.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <heltec-eink-modules.h>

// ─── Display ────────────────────────────────────────────────────────────────
EInkDisplay_VisionMasterE290 display;

// ─── Layout (landscape 296 × 128) ───────────────────────────────────────────
#define SCREEN_W        128
#define SCREEN_H        296

// Menu / UI font (textSize 1)
#define CHAR_W          6
#define CHAR_H          8
#define MARGIN_X        4
#define MARGIN_Y        4
#define CONTENT_W       (SCREEN_H - MARGIN_X * 2)
#define CHARS_PER_LINE  (CONTENT_W / CHAR_W)           // ~48
#define LINES_PER_PAGE  ((SCREEN_W - MARGIN_Y*2 - CHAR_H - 6) / (CHAR_H + 1))

// Reading font (FreeSerif9pt7b at textSize 1) — approx 6×14 px per glyph
// Using pixel-width of 7 for safe word-wrap (some glyphs are wider)
#define READ_CHAR_W       7
#define READ_CHAR_H       14
#define READ_LINE_SPACING 3
#define READ_CHARS_PER_LINE  ((SCREEN_H - MARGIN_X * 2) / READ_CHAR_W)        // ~40
#define READ_LINES_PER_PAGE  ((SCREEN_W - MARGIN_Y*2 - CHAR_H - 10) / (READ_CHAR_H + READ_LINE_SPACING))

// ─── Fast-refresh ────────────────────────────────────────────────────────────
// Full refresh triggered only on long-press confirms (forceFull=true)

// ─── Button ──────────────────────────────────────────────────────────────────
#define BTN_PIN           21
#define LONG_PRESS_MS     700
#define XLONG_PRESS_MS    3000
#define DOUBLE_MS         350

// ─── BLE UUIDs ───────────────────────────────────────────────────────────────
#define SERVICE_UUID     "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define UPLOAD_CHAR_UUID "beb5483e-36e1-4688-b7f5-ea07361b26a8"
#define LIST_CHAR_UUID   "beb5483e-36e1-4688-b7f5-ea07361b26a9"
#define DELETE_CHAR_UUID "beb5483e-36e1-4688-b7f5-ea07361b26aa"

#define BOOKS_DIR        "/books"
#define META_FILE        "/meta.json"
#define MAX_BOOKS        20
#define CHUNK_TIMEOUT_MS 3000

// ─── Bookmarks ───────────────────────────────────────────────────────────────
#define MAX_BOOKMARKS 50
struct Bookmark { int bookIdx; int page; };
Bookmark bookmarks[MAX_BOOKMARKS];
int bookmarkCount = 0;
int bmIndex       = 0;

// ─── Chapters ────────────────────────────────────────────────────────────────
#define MAX_CHAPTERS 100
struct Chapter { String name; int page; };
Chapter chapters[MAX_CHAPTERS];
int chapterCount = 0;
int chapterIndex = 0;

// ─── App States ──────────────────────────────────────────────────────────────
enum AppState {
  STATE_MENU,
  STATE_LIBRARY,
  STATE_READING,
  STATE_OVERLAY,
  STATE_CHAPTER_SELECT,
  STATE_BOOKMARKS,
  STATE_SETTINGS,
  STATE_SETTINGS_DELETE_BOOK,
  STATE_INFO
};
AppState appState    = STATE_MENU;
AppState returnState = STATE_MENU;

// ─── Main menu ───────────────────────────────────────────────────────────────
const char* MENU_ITEMS[] = {
  "Continue Reading",
  "Library",
  "Bookmarks",
  "Settings",
  "Info",
  "Power Off"
};
const int MENU_COUNT = 6;
int  menuIndex  = 0;
bool bleEnabled = false;

// ─── Library ─────────────────────────────────────────────────────────────────
struct BookMeta {
  String title;
  String filename;
  int    totalPages;
  int    lastPage;
};
BookMeta books[MAX_BOOKS];
int bookCount = 0;
int libIndex  = 0;

// ─── Reading ─────────────────────────────────────────────────────────────────
int  currentBook = -1;
int  currentPage = 0;
#define MAX_PAGES 2000
uint32_t pageOffsets[MAX_PAGES];
int      totalPages = 0;

// ─── Overlay menu ────────────────────────────────────────────────────────────
const char* OVERLAY_ITEMS[] = {
  "Chapter Select",
  "Bookmark this page",
  "Start from beginning",
  "Return to Main Menu",
  "Back to book"
};
const int OVERLAY_COUNT = 5;
int overlayIndex = 0;

// ─── Settings menu ───────────────────────────────────────────────────────────
const char* SETTINGS_ITEMS[] = {
  "Bluetooth",
  "Delete a book",
  "Delete all books",
  "Reset all progress",
  "Back"
};
const int SETTINGS_COUNT = 5;
int settingsIndex   = 0;
int deleteBookIndex = 0;
int infoScrollOffset = 0;

// ─── BLE upload ──────────────────────────────────────────────────────────────
String        uploadBuffer  = "";
String        uploadName    = "";
bool          uploadActive  = false;
unsigned long lastChunkTime = 0;

BLEServer*         pServer     = nullptr;
BLECharacteristic* pUploadChar = nullptr;
BLECharacteristic* pListChar   = nullptr;
BLECharacteristic* pDeleteChar = nullptr;
bool bleServerRunning = false;

// ─── Button state ─────────────────────────────────────────────────────────────
enum BtnEvent { BTN_NONE, BTN_SINGLE, BTN_DOUBLE, BTN_LONG, BTN_XLONG };
int           btnLastState   = HIGH;
unsigned long btnPressTime   = 0;
unsigned long btnReleaseTime = 0;
int           pendingClicks  = 0;
bool          longFired      = false;
bool          xlongFired     = false;

// ============================================================
//  FORWARD DECLARATIONS
// ============================================================
void doUpdate(bool forceFull = false);

void drawSleepQuote();
void handleChapterSelectInput(BtnEvent e);

void drawCenteredTitle(const char* title);
void drawMenu(bool full = false);
void drawLibrary(bool full = false);
void drawPage(int page, bool full = false);
void drawOverlay(bool full = false);
void drawBookmarks(bool full = false);
void drawSettings(bool full = false);
void drawSettingsDeleteBook(bool full = false);
void drawChapterSelect(bool full = false);
void drawInfo();
void drawMessage(const char* title, const char* body);

BtnEvent readButton();
void handleMenuInput(BtnEvent e);
void handleLibraryInput(BtnEvent e);
void handleReadingInput(BtnEvent e);
void handleOverlayInput(BtnEvent e);
void handleBookmarksInput(BtnEvent e);
void handleSettingsInput(BtnEvent e);
void handleSettingsDeleteBookInput(BtnEvent e);
void handleInfoInput(BtnEvent e);

void addBookmark(int bookIdx, int page);
void deleteBookmark(int bmIdx);
void deleteAllBookmarks();

void deleteBook(int bookIdx);
void deleteAllBooks();
void resetAllProgress();

void buildPageIndex(int bookIdx);
String getPageText(int bookIdx, int page);

// Two wrap variants: one for menu font, one for reading font
int wrapText(const String& text, String lines[], int maxLines, int charsPerLine);

void loadMeta();
void saveMeta();

void startBLE();
void stopBLE();
void processUploadBuffer();

void goToSleep();

// ============================================================
//  SMART UPDATE
//  forceFull=true  → full refresh (used after long-press confirms)
//  forceFull=false → fast refresh (navigation, page turns)
// ============================================================
void doUpdate(bool forceFull) {
  if (forceFull) {
    display.fastmodeOff();
    display.update();
    display.fastmodeOn();
  } else {
    display.update();
  }
}

// ============================================================
//  WORD-WRAP  (charsPerLine is passed in so reading/menu can differ)
// ============================================================
int wrapText(const String& text, String lines[], int maxLines, int charsPerLine) {
  int lineCount = 0, pos = 0, len = text.length();
  while (pos < len && lineCount < maxLines) {
    if (pos > 0 && text[pos] == ' ') pos++;
    if (pos >= len) break;
    if (text[pos] == '\n') { lines[lineCount++] = ""; pos++; continue; }
    if (pos + charsPerLine >= len) {
      String chunk = text.substring(pos, len);
      chunk.replace("\n", " ");
      lines[lineCount++] = chunk;
      break;
    }
    int nlPos = -1;
    for (int i = pos; i < pos + charsPerLine && i < len; i++)
      if (text[i] == '\n') { nlPos = i; break; }
    if (nlPos != -1) { lines[lineCount++] = text.substring(pos, nlPos); pos = nlPos + 1; continue; }
    int breakAt = -1;
    for (int i = pos + charsPerLine; i > pos; i--)
      if (text[i] == ' ') { breakAt = i; break; }
    if (breakAt == -1) { lines[lineCount++] = text.substring(pos, pos + charsPerLine); pos += charsPerLine; }
    else               { lines[lineCount++] = text.substring(pos, breakAt); pos = breakAt + 1; }
  }
  return lineCount;
}

// ============================================================
//  BLE CALLBACKS
// ============================================================
class UploadCallback : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* c) override {
    String val = c->getValue().c_str();
    if (!val.length()) return;
    if (!uploadActive) {
      int sep = val.indexOf('|');
      if (sep != -1) { uploadName = val.substring(0, sep); uploadBuffer = val.substring(sep+1); uploadActive = true; }
    } else uploadBuffer += val;
    lastChunkTime = millis();
  }
};
class ListCallback : public BLECharacteristicCallbacks {
  void onRead(BLECharacteristic* c) override {
    String list = "";
    for (int i = 0; i < bookCount; i++) { if (i) list += ","; list += books[i].title; }
    c->setValue(list.c_str());
  }
};
class DeleteCallback : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* c) override {
    String title = c->getValue().c_str();
    for (int i = 0; i < bookCount; i++) {
      if (books[i].title == title || books[i].filename == title) { deleteBook(i); return; }
    }
  }
};
class ServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer*)    override { Serial.println("BLE connected"); }
  void onDisconnect(BLEServer*) override { BLEDevice::startAdvertising(); }
};

// ============================================================
//  SETUP
// ============================================================
void setup() {
  Serial.begin(115200);
  pinMode(BTN_PIN, INPUT_PULLUP);
  esp_sleep_enable_ext0_wakeup((gpio_num_t)BTN_PIN, 0);

  if (!LittleFS.begin(true)) { LittleFS.format(); LittleFS.begin(); }
  if (!LittleFS.exists(BOOKS_DIR)) LittleFS.mkdir(BOOKS_DIR);
  loadMeta();

  display.setRotation(1);
  display.setTextWrap(false);
  display.fastmodeOff();
  display.update();         // full refresh on wake/boot
  display.fastmodeOn();
  drawMenu();
}

// ============================================================
//  LOOP
// ============================================================
void loop() {
  if (uploadActive && millis() - lastChunkTime > CHUNK_TIMEOUT_MS)
    processUploadBuffer();

  BtnEvent e = readButton();
  if (e == BTN_NONE) return;

  switch (appState) {
    case STATE_MENU:                 handleMenuInput(e);              break;
    case STATE_LIBRARY:              handleLibraryInput(e);           break;
    case STATE_READING:              handleReadingInput(e);           break;
    case STATE_OVERLAY:              handleOverlayInput(e);           break;
    case STATE_CHAPTER_SELECT:       handleChapterSelectInput(e);     break;
    case STATE_BOOKMARKS:            handleBookmarksInput(e);         break;
    case STATE_SETTINGS:             handleSettingsInput(e);          break;
    case STATE_SETTINGS_DELETE_BOOK: handleSettingsDeleteBookInput(e);break;
    case STATE_INFO:                 handleInfoInput(e);              break;
  }
}

// ============================================================
//  BUTTON LOGIC
// ============================================================
BtnEvent readButton() {
  int reading = digitalRead(BTN_PIN);
  BtnEvent result = BTN_NONE;
  unsigned long now = millis();
  if (reading == LOW  && btnLastState == HIGH)              { btnPressTime = now; longFired = false; xlongFired = false; }
  if (reading == LOW  && !xlongFired && now - btnPressTime >= XLONG_PRESS_MS) { xlongFired = true; longFired = true; result = BTN_XLONG; }
  else if (reading == LOW && !longFired && now - btnPressTime >= LONG_PRESS_MS) { longFired = true; result = BTN_LONG; }
  if (reading == HIGH && btnLastState == LOW && !longFired) { pendingClicks++; btnReleaseTime = now; }
  if (pendingClicks > 0 && reading == HIGH && now - btnReleaseTime > DOUBLE_MS) {
    result = (pendingClicks >= 2) ? BTN_DOUBLE : BTN_SINGLE;
    pendingClicks = 0;
  }
  btnLastState = reading;
  return result;
}

// ============================================================
//  INPUT HANDLERS
//
//  NEW SCHEME:
//    Menus:   1 click = next/down  |  2 clicks = prev/up  |  hold = select
//    Reading: 1 click = next page  |  2 clicks = prev page |  hold = overlay
//    Overlay: 1 click = next/down  |  2 clicks = prev/up  |  hold = confirm
// ============================================================

void handleMenuInput(BtnEvent e) {
  if (e == BTN_SINGLE) { menuIndex = (menuIndex + 1) % MENU_COUNT; drawMenu(); return; }
  if (e == BTN_DOUBLE) { menuIndex = (menuIndex - 1 + MENU_COUNT) % MENU_COUNT; drawMenu(); return; }
  if (e == BTN_XLONG) {
    drawSleepQuote();
    goToSleep();
    return;
  }
  if (e == BTN_LONG) {
    switch (menuIndex) {
      case 0: // Continue Reading
        if (currentBook >= 0) { appState = STATE_READING; drawPage(currentPage, true); }
        else { drawMessage("No book open", "Open Library to\npick a book."); delay(1500); drawMenu(true); }
        break;
      case 1: libIndex = 0; appState = STATE_LIBRARY;   drawLibrary(true);   break;
      case 2: bmIndex  = 0; appState = STATE_BOOKMARKS; drawBookmarks(true); break;
      case 3: settingsIndex = 0; appState = STATE_SETTINGS; drawSettings(true); break;
      case 4: appState = STATE_INFO; infoScrollOffset = 0; drawInfo(); break;
      case 5: drawSleepQuote(); delay(800); goToSleep(); break;
    }
  }
}

void handleLibraryInput(BtnEvent e) {
  if (bookCount == 0) { appState = STATE_MENU; drawMenu(true); return; }
  if (e == BTN_SINGLE) { libIndex = (libIndex + 1) % bookCount; drawLibrary(); }
  else if (e == BTN_DOUBLE) { libIndex = (libIndex - 1 + bookCount) % bookCount; drawLibrary(); }
  else if (e == BTN_LONG) {
    currentBook = libIndex;
    currentPage = books[currentBook].lastPage;
    buildPageIndex(currentBook);
    appState = STATE_READING;
    drawPage(currentPage, true);
  }
}

void handleReadingInput(BtnEvent e) {
  if (e == BTN_XLONG) {
    appState = STATE_MENU;
    drawMenu(true);
    return;
  }
  if (e == BTN_SINGLE) {
    if (currentPage < totalPages - 1) {
      currentPage++; books[currentBook].lastPage = currentPage; saveMeta(); drawPage(currentPage);
    }
  } else if (e == BTN_DOUBLE) {
    if (currentPage > 0) {
      currentPage--; books[currentBook].lastPage = currentPage; saveMeta(); drawPage(currentPage);
    }
  } else if (e == BTN_LONG) {
    overlayIndex = 0;
    appState = STATE_OVERLAY;
    drawOverlay(true);
  }
}

void handleOverlayInput(BtnEvent e) {
  if (e == BTN_SINGLE) { overlayIndex = (overlayIndex + 1) % OVERLAY_COUNT; drawOverlay(); }
  else if (e == BTN_DOUBLE) { overlayIndex = (overlayIndex - 1 + OVERLAY_COUNT) % OVERLAY_COUNT; drawOverlay(); }
  else if (e == BTN_LONG) {
    switch (overlayIndex) {
      case 0: // Chapter Select
        if (chapterCount == 0) {
          drawMessage("No Chapters", "No === markers\nfound in this book.");
          delay(1500);
          drawOverlay(true);
        } else {
          chapterIndex = 0;
          appState = STATE_CHAPTER_SELECT;
          drawChapterSelect(true);
        }
        break;
      case 1: // Bookmark this page
        addBookmark(currentBook, currentPage);
        drawMessage("Bookmarked!", books[currentBook].title.substring(0,20).c_str());
        delay(1200);
        appState = STATE_READING;
        drawPage(currentPage, true);
        break;
      case 2: // Start from beginning
        currentPage = 0;
        books[currentBook].lastPage = 0;
        saveMeta();
        appState = STATE_READING;
        drawPage(0, true);
        break;
      case 3: // Return to Main Menu
        appState = STATE_MENU;
        drawMenu(true);
        break;
      case 4: // Back to book
        appState = STATE_READING;
        drawPage(currentPage, true);
        break;
    }
  }
}

void handleChapterSelectInput(BtnEvent e) {
  if (e == BTN_SINGLE) { chapterIndex = (chapterIndex + 1) % chapterCount; drawChapterSelect(false); }
  else if (e == BTN_DOUBLE) { chapterIndex = (chapterIndex - 1 + chapterCount) % chapterCount; drawChapterSelect(false); }
  else if (e == BTN_LONG) {
    currentPage = chapters[chapterIndex].page;
    books[currentBook].lastPage = currentPage;
    saveMeta();
    appState = STATE_READING;
    drawPage(currentPage, true);
  }
}

void handleBookmarksInput(BtnEvent e) {
  if (bookmarkCount == 0) { appState = STATE_MENU; drawMenu(true); return; }
  if (e == BTN_SINGLE) { bmIndex = (bmIndex + 1) % bookmarkCount; drawBookmarks(); }
  else if (e == BTN_DOUBLE) { bmIndex = (bmIndex - 1 + bookmarkCount) % bookmarkCount; drawBookmarks(); }
  else if (e == BTN_LONG) {
    int bi = bookmarks[bmIndex].bookIdx;
    int pg = bookmarks[bmIndex].page;
    if (bi >= 0 && bi < bookCount) {
      currentBook = bi;
      currentPage = pg;
      books[currentBook].lastPage = pg;
      buildPageIndex(currentBook);
      saveMeta();
      appState = STATE_READING;
      drawPage(currentPage, true);
    }
  }
}

void handleSettingsInput(BtnEvent e) {
  if (e == BTN_SINGLE) { settingsIndex = (settingsIndex + 1) % SETTINGS_COUNT; drawSettings(); }
  else if (e == BTN_DOUBLE) { settingsIndex = (settingsIndex - 1 + SETTINGS_COUNT) % SETTINGS_COUNT; drawSettings(); }
  else if (e == BTN_LONG) {
    switch (settingsIndex) {
      case 0: // Bluetooth toggle
        bleEnabled = !bleEnabled;
        bleEnabled ? startBLE() : stopBLE();
        drawSettings(true);
        break;
      case 1: // Delete a book
        if (bookCount == 0) {
          drawMessage("No books", "Library is empty.");
          delay(1200); drawSettings(true);
        } else {
          deleteBookIndex = 0;
          appState = STATE_SETTINGS_DELETE_BOOK;
          drawSettingsDeleteBook(true);
        }
        break;
      case 2: // Delete all books
        deleteAllBooks();
        drawMessage("Done", "All books deleted.");
        delay(1200); drawSettings(true);
        break;
      case 3: // Reset all progress
        resetAllProgress();
        drawMessage("Done", "All progress reset.");
        delay(1200); drawSettings(true);
        break;
      case 4: // Back
        appState = STATE_MENU; drawMenu(true);
        break;
    }
  }
}

void handleSettingsDeleteBookInput(BtnEvent e) {
  if (e == BTN_SINGLE) { deleteBookIndex = (deleteBookIndex + 1) % bookCount; drawSettingsDeleteBook(); }
  else if (e == BTN_DOUBLE) { deleteBookIndex = (deleteBookIndex - 1 + bookCount) % bookCount; drawSettingsDeleteBook(); }
  else if (e == BTN_LONG) {
    String title = books[deleteBookIndex].title;
    deleteBook(deleteBookIndex);
    drawMessage("Deleted", title.substring(0, 24).c_str());
    delay(1200);
    if (bookCount == 0) { appState = STATE_SETTINGS; drawSettings(true); }
    else { deleteBookIndex = 0; drawSettingsDeleteBook(true); }
  }
}

// ─── Info line count (used in handleInfoInput and drawInfo) ─────────────────
#define INFO_LINE_COUNT 30

void handleInfoInput(BtnEvent e) {
  int visibleLines = (SCREEN_W - MARGIN_Y*2 - CHAR_H - 8) / (CHAR_H + 1);
  int maxScroll = max(0, INFO_LINE_COUNT - visibleLines);
  if (e == BTN_SINGLE) {
    if (infoScrollOffset < maxScroll) { infoScrollOffset++; drawInfo(); }
  } else if (e == BTN_DOUBLE) {
    if (infoScrollOffset > 0) { infoScrollOffset--; drawInfo(); }
  } else if (e == BTN_LONG || e == BTN_XLONG) {
    infoScrollOffset = 0;
    appState = STATE_MENU;
    drawMenu(true);
  }
}

// ============================================================
//  BOOKMARK OPERATIONS
// ============================================================
void addBookmark(int bookIdx, int page) {
  for (int i = 0; i < bookmarkCount; i++)
    if (bookmarks[i].bookIdx == bookIdx && bookmarks[i].page == page) return;
  if (bookmarkCount >= MAX_BOOKMARKS) return;
  bookmarks[bookmarkCount++] = { bookIdx, page };
  saveMeta();
}

void deleteBookmark(int bmIdx) {
  if (bmIdx < 0 || bmIdx >= bookmarkCount) return;
  for (int i = bmIdx; i < bookmarkCount - 1; i++) bookmarks[i] = bookmarks[i+1];
  bookmarkCount--;
  saveMeta();
}

void deleteAllBookmarks() {
  bookmarkCount = 0;
  saveMeta();
}

// ============================================================
//  BOOK / PROGRESS OPERATIONS
// ============================================================
void deleteBook(int bookIdx) {
  if (bookIdx < 0 || bookIdx >= bookCount) return;
  LittleFS.remove(String(BOOKS_DIR) + "/" + books[bookIdx].filename);
  for (int i = bookmarkCount - 1; i >= 0; i--)
    if (bookmarks[i].bookIdx == bookIdx) deleteBookmark(i);
  for (int i = 0; i < bookmarkCount; i++)
    if (bookmarks[i].bookIdx > bookIdx) bookmarks[i].bookIdx--;
  for (int i = bookIdx; i < bookCount - 1; i++) books[i] = books[i+1];
  bookCount--;
  if (currentBook == bookIdx) currentBook = -1;
  else if (currentBook > bookIdx) currentBook--;
  saveMeta();
}

void deleteAllBooks() {
  File dir = LittleFS.open(BOOKS_DIR);
  if (dir) {
    File f = dir.openNextFile();
    while (f) {
      String path = String(BOOKS_DIR) + "/" + f.name();
      f.close();
      LittleFS.remove(path);
      f = dir.openNextFile();
    }
    dir.close();
  }
  bookCount     = 0;
  bookmarkCount = 0;
  currentBook   = -1;
  currentPage   = 0;
  saveMeta();
}

void resetAllProgress() {
  for (int i = 0; i < bookCount; i++) books[i].lastPage = 0;
  deleteAllBookmarks();
  currentPage = 0;
  saveMeta();
}

// ============================================================
//  DISPLAY HELPERS  (menus always use textSize 1)
// ============================================================
void drawCenteredTitle(const char* title) {
  int len    = strlen(title);
  int totalX = len * CHAR_W;
  int x      = (SCREEN_H - totalX) / 2;
  if (x < 0) x = 0;
  display.setTextSize(1);
  display.setCursor(x, MARGIN_Y);
  display.print(title);
  display.drawLine(0, MARGIN_Y + CHAR_H + 2, SCREEN_H, MARGIN_Y + CHAR_H + 2, BLACK);
}

void drawRow(int x, int y, const String& label, bool selected) {
  display.setTextSize(1);
  display.setCursor(x, y);
  display.print(selected ? "> " : "  ");
  display.print(label);
}

// ─── MAIN MENU ───────────────────────────────────────────────────────────────
void drawMenu(bool full) {
  display.clearMemory();
  display.setTextSize(1);
  display.setTextColor(BLACK);

  drawCenteredTitle("Mini InkBook");
  if (bleEnabled) {
    display.setCursor(SCREEN_H - CHAR_W * 5 - MARGIN_X, MARGIN_Y);
    display.print("[BLE]");
  }

  int y = MARGIN_Y + CHAR_H + 6;
  for (int i = 0; i < MENU_COUNT; i++) {
    String label;
    if (i == 0) {
      label = "Continue: ";
      label += (currentBook >= 0) ? books[currentBook].title.substring(0, 18) : "---";
    } else {
      label = MENU_ITEMS[i];
    }
    drawRow(MARGIN_X, y, label, i == menuIndex);
    y += CHAR_H + 2;
  }
  doUpdate(full);
}

// ─── LIBRARY ─────────────────────────────────────────────────────────────────
#define LIB_VISIBLE 9
void drawLibrary(bool full) {
  display.clearMemory();
  display.setTextSize(1);
  display.setTextColor(BLACK);
  drawCenteredTitle("Library");

  if (bookCount == 0) {
    display.setCursor(MARGIN_X, MARGIN_Y + CHAR_H + 6);
    display.print("No books yet.");
    display.setCursor(MARGIN_X, MARGIN_Y + CHAR_H + 6 + CHAR_H + 2);
    display.print("Enable BLE to send .txt files.");
    doUpdate(full); return;
  }

  int windowStart = (libIndex / LIB_VISIBLE) * LIB_VISIBLE;
  int y = MARGIN_Y + CHAR_H + 6;
  for (int i = windowStart; i < min(windowStart + LIB_VISIBLE, bookCount); i++) {
    char pg[8]; sprintf(pg, "[%d]", books[i].lastPage + 1);
    int maxT = CHARS_PER_LINE - 2 - (int)strlen(pg) - 1;
    String label = books[i].title.substring(0, maxT) + " " + pg;
    drawRow(MARGIN_X, y, label, i == libIndex);
    y += CHAR_H + 2;
  }
  if (bookCount > LIB_VISIBLE) {
    char pos[10]; sprintf(pos, "%d/%d", libIndex + 1, bookCount);
    display.setCursor(SCREEN_H - strlen(pos) * CHAR_W - MARGIN_X, MARGIN_Y);
    display.print(pos);
  }
  doUpdate(full);
}

// ─── READING  (FreeSerif9pt7b for comfortable reading) ───────────────────────
void drawPage(int page, bool full) {
  if (currentBook < 0 || page >= totalPages) return;
  display.clearMemory();
  display.setTextColor(BLACK);

  // ── Header: title + page counter at small size ──
  display.setFont(nullptr);
  display.setTextSize(1);
  String header = books[currentBook].title.substring(0, 22);
  char pg[14]; sprintf(pg, " %d/%d", page + 1, totalPages);
  header += pg;
  display.setCursor(MARGIN_X, MARGIN_Y);
  display.print(header);
  display.drawLine(0, MARGIN_Y + CHAR_H + 2, SCREEN_H, MARGIN_Y + CHAR_H + 2, BLACK);

  // Bookmark indicator
  for (int i = 0; i < bookmarkCount; i++) {
    if (bookmarks[i].bookIdx == currentBook && bookmarks[i].page == page) {
      display.setCursor(SCREEN_H - CHAR_W * 3 - MARGIN_X, MARGIN_Y);
      display.print("[B]");
      break;
    }
  }

  // ── Body text with serif font at textSize 1 ──
  display.setFont(&FreeSerif9pt7b);
  display.setTextSize(1);
  String rawText = getPageText(currentBook, page);
  String lines[READ_LINES_PER_PAGE];
  int lineCount = wrapText(rawText, lines, READ_LINES_PER_PAGE, READ_CHARS_PER_LINE);

  // For proportional fonts, y cursor is the baseline
  int y = MARGIN_Y + CHAR_H + 6 + READ_CHAR_H;
  for (int i = 0; i < lineCount; i++) {
    display.setCursor(MARGIN_X, y);
    display.print(lines[i]);
    y += READ_CHAR_H + READ_LINE_SPACING;
  }

  // Restore default font/size for any subsequent UI
  display.setFont(nullptr);
  display.setTextSize(1);
  doUpdate(full);
}

// ─── OVERLAY (in-book quick menu) ────────────────────────────────────────────
void drawOverlay(bool full) {
  display.clearMemory();
  display.setTextSize(1);
  display.setTextColor(BLACK);

  drawCenteredTitle("Quick Menu");

  int y = MARGIN_Y + CHAR_H + 6;
  for (int i = 0; i < OVERLAY_COUNT; i++) {
    drawRow(MARGIN_X, y, OVERLAY_ITEMS[i], i == overlayIndex);
    y += CHAR_H + 4;
  }

  display.setCursor(MARGIN_X, SCREEN_W - CHAR_H - 2);
  char ctx[40];
  sprintf(ctx, "p.%d  %s", currentPage + 1, books[currentBook].title.substring(0,16).c_str());
  display.print(ctx);

  doUpdate(full);
}

// ─── CHAPTER SELECT ──────────────────────────────────────────────────────────
#define CH_VISIBLE 9
void drawChapterSelect(bool full) {
  display.clearMemory();
  display.setTextSize(1);
  display.setTextColor(BLACK);
  drawCenteredTitle("Chapters");

  int windowStart = (chapterIndex / CH_VISIBLE) * CH_VISIBLE;
  int y = MARGIN_Y + CHAR_H + 6;
  for (int i = windowStart; i < min(windowStart + CH_VISIBLE, chapterCount); i++) {
    char pgBuf[8]; sprintf(pgBuf, " p.%d", chapters[i].page + 1);
    int maxName = CHARS_PER_LINE - 2 - (int)strlen(pgBuf);
    String label = chapters[i].name.substring(0, maxName) + pgBuf;
    drawRow(MARGIN_X, y, label, i == chapterIndex);
    y += CHAR_H + 2;
  }
  if (chapterCount > CH_VISIBLE) {
    char pos[10]; sprintf(pos, "%d/%d", chapterIndex + 1, chapterCount);
    display.setCursor(SCREEN_H - strlen(pos) * CHAR_W - MARGIN_X, MARGIN_Y);
    display.print(pos);
  }
  doUpdate(full);
}

// ─── BOOKMARKS ───────────────────────────────────────────────────────────────
#define BM_VISIBLE 9
void drawBookmarks(bool full) {
  display.clearMemory();
  display.setTextSize(1);
  display.setTextColor(BLACK);
  drawCenteredTitle("Bookmarks");

  if (bookmarkCount == 0) {
    display.setCursor(MARGIN_X, MARGIN_Y + CHAR_H + 6);
    display.print("No bookmarks yet.");
    display.setCursor(MARGIN_X, MARGIN_Y + CHAR_H + 6 + CHAR_H + 2);
    display.print("Open Quick Menu while reading.");
    doUpdate(full); return;
  }

  int windowStart = (bmIndex / BM_VISIBLE) * BM_VISIBLE;
  int y = MARGIN_Y + CHAR_H + 6;
  for (int i = windowStart; i < min(windowStart + BM_VISIBLE, bookmarkCount); i++) {
    int bi = bookmarks[i].bookIdx;
    int pg = bookmarks[i].page;
    String title = (bi >= 0 && bi < bookCount) ? books[bi].title.substring(0, 22) : "?";
    char label[48]; sprintf(label, "%s p.%d", title.c_str(), pg + 1);
    drawRow(MARGIN_X, y, String(label), i == bmIndex);
    y += CHAR_H + 2;
  }
  if (bookmarkCount > BM_VISIBLE) {
    char pos[10]; sprintf(pos, "%d/%d", bmIndex + 1, bookmarkCount);
    display.setCursor(SCREEN_H - strlen(pos) * CHAR_W - MARGIN_X, MARGIN_Y);
    display.print(pos);
  }
  doUpdate(full);
}

// ─── SETTINGS ────────────────────────────────────────────────────────────────
void drawSettings(bool full) {
  display.clearMemory();
  display.setTextSize(1);
  display.setTextColor(BLACK);
  drawCenteredTitle("Settings");

  int y = MARGIN_Y + CHAR_H + 6;
  for (int i = 0; i < SETTINGS_COUNT; i++) {
    String label;
    if (i == 0) label = bleEnabled ? "Bluetooth: ON" : "Bluetooth: OFF";
    else        label = SETTINGS_ITEMS[i];
    drawRow(MARGIN_X, y, label, i == settingsIndex);
    y += CHAR_H + 4;
  }
  doUpdate(full);
}

// ─── SETTINGS: DELETE A BOOK ─────────────────────────────────────────────────
void drawSettingsDeleteBook(bool full) {
  display.clearMemory();
  display.setTextSize(1);
  display.setTextColor(BLACK);
  drawCenteredTitle("Delete Book");

  int windowStart = (deleteBookIndex / LIB_VISIBLE) * LIB_VISIBLE;
  int y = MARGIN_Y + CHAR_H + 6;
  for (int i = windowStart; i < min(windowStart + LIB_VISIBLE, bookCount); i++) {
    drawRow(MARGIN_X, y, books[i].title.substring(0, CHARS_PER_LINE - 4), i == deleteBookIndex);
    y += CHAR_H + 2;
  }
  if (bookCount > LIB_VISIBLE) {
    char pos[10]; sprintf(pos, "%d/%d", deleteBookIndex + 1, bookCount);
    display.setCursor(SCREEN_H - strlen(pos) * CHAR_W - MARGIN_X, MARGIN_Y);
    display.print(pos);
  }
  display.setCursor(MARGIN_X, SCREEN_W - CHAR_H - 2);
  display.print("hold=confirm delete  2x=cancel");
  doUpdate(full);
}

// ─── INFO ────────────────────────────────────────────────────────────────────
static const char* INFO_LINES[] = {
  "MENUS",
  "  1 click  = next item (down)",
  "  2 clicks = previous item (up)",
  "  hold     = select / confirm",
  "  3s hold  = power off",
  "",
  "READING",
  "  1 click  = next page",
  "  2 clicks = previous page",
  "  hold     = open quick menu",
  "  3s hold  = back to main menu",
  "",
  "QUICK MENU",
  "  1 click  = next option",
  "  2 clicks = previous option",
  "  hold     = confirm option",
  "  Options:",
  "    Chapter Select",
  "    Bookmark this page",
  "    Start from beginning",
  "    Return to Main Menu",
  "    Back to book",
  "",
  "SETTINGS",
  "  Bluetooth toggle is here",
  "  Delete books, reset progress",
  "",
  "INFO (this screen)",
  "  1 click  = scroll down",
  "  2 clicks = scroll up",
  "  hold     = back to main menu"
};
static const int INFO_LINE_COUNT_CHECK = 30; // matches #define INFO_LINE_COUNT above

void drawInfo() {
  display.clearMemory();
  display.setFont(nullptr);
  display.setTextSize(1);
  display.setTextColor(BLACK);
  drawCenteredTitle("Button Guide");

  int visibleLines = (SCREEN_W - MARGIN_Y*2 - CHAR_H - 8) / (CHAR_H + 1);
  int y = MARGIN_Y + CHAR_H + 6;
  for (int i = infoScrollOffset; i < INFO_LINE_COUNT && i < infoScrollOffset + visibleLines; i++) {
    display.setCursor(MARGIN_X, y);
    display.print(INFO_LINES[i]);
    y += CHAR_H + 1;
  }
  // Scroll indicator
  if (INFO_LINE_COUNT > visibleLines) {
    char pos[12]; sprintf(pos, "%d/%d", infoScrollOffset + 1, INFO_LINE_COUNT - visibleLines + 1);
    display.setCursor(SCREEN_H - strlen(pos)*CHAR_W - MARGIN_X, MARGIN_Y);
    display.print(pos);
  }
  doUpdate(true);
}

// ─── MESSAGE ─────────────────────────────────────────────────────────────────
void drawMessage(const char* title, const char* body) {
  display.clearMemory();
  display.setTextSize(1);
  display.setTextColor(BLACK);
  drawCenteredTitle(title);
  display.setCursor(MARGIN_X, MARGIN_Y + CHAR_H + 6);
  display.print(body);
  doUpdate(true);
}

// ============================================================
//  PAGE INDEX BUILDER
//  Uses READ_CHARS_PER_LINE / READ_LINES_PER_PAGE since the
//  reading view uses textSize 2.
// ============================================================
void buildPageIndex(int bookIdx) {
  totalPages   = 0;
  chapterCount = 0;
  if (bookIdx < 0 || bookIdx >= bookCount) return;

  String path = String(BOOKS_DIR) + "/" + books[bookIdx].filename;
  File f = LittleFS.open(path, "r");
  if (!f) return;

  pageOffsets[0] = 0;
  totalPages     = 1;

  int    lineCount = 0, lineChars = 0;
  String lineBuf   = "";

  while (f.available() && totalPages < MAX_PAGES) {
    char ch = f.read();

    if (ch == '\n') {
      String trimmed = lineBuf;
      trimmed.trim();
      if (trimmed.startsWith("===") && trimmed.endsWith("===") && trimmed.length() > 6) {
        String name = trimmed.substring(3, trimmed.length() - 3);
        name.trim();
        if (name.length() > 0 && chapterCount < MAX_CHAPTERS)
          chapters[chapterCount++] = { name, totalPages - 1 };
      }
      lineBuf   = "";
    } else {
      lineBuf += ch;
    }

    // Page-break accounting using READING font dimensions
    if (ch == '\n') {
      lineChars = 0;
      lineCount++;
    } else {
      lineChars++;
      if (lineChars >= READ_CHARS_PER_LINE) {
        lineChars = 0;
        lineCount++;
      }
    }
    if (lineCount >= READ_LINES_PER_PAGE) {
      pageOffsets[totalPages++] = f.position();
      lineCount = lineChars = 0;
    }
  }

  f.close();
  books[bookIdx].totalPages = totalPages;
  saveMeta();
  Serial.printf("Book '%s': %d pages, %d chapters\n",
                books[bookIdx].title.c_str(), totalPages, chapterCount);
}

String getPageText(int bookIdx, int page) {
  if (bookIdx < 0 || page >= totalPages) return "";
  String path = String(BOOKS_DIR) + "/" + books[bookIdx].filename;
  File f = LittleFS.open(path, "r");
  if (!f) return "";
  f.seek(pageOffsets[page]);
  // Read enough for one reading-font page with a small buffer
  const int readAhead = READ_CHARS_PER_LINE * (READ_LINES_PER_PAGE + 4);
  String text = ""; text.reserve(readAhead);
  int count = 0;
  while (f.available() && count++ < readAhead) text += (char)f.read();
  f.close();
  return text;
}

// ============================================================
//  UPLOAD PROCESSING
// ============================================================
void processUploadBuffer() {
  uploadActive = false;
  if (!uploadName.length() || !uploadBuffer.length()) return;

  String safeName = uploadName;
  for (int i = 0; i < (int)safeName.length(); i++) {
    char c = safeName[i];
    if (!isAlphaNumeric(c) && c != '.' && c != '-' && c != '_') safeName[i] = '_';
  }

  File f = LittleFS.open(String(BOOKS_DIR) + "/" + safeName, "w");
  if (!f) return;
  f.print(uploadBuffer); f.close();

  String title = safeName;
  int dot = title.lastIndexOf('.');
  if (dot > 0) title = title.substring(0, dot);
  title.replace("_", " ");

  bool found = false;
  for (int i = 0; i < bookCount; i++) {
    if (books[i].filename == safeName) { books[i].totalPages = 0; books[i].lastPage = 0; found = true; break; }
  }
  if (!found && bookCount < MAX_BOOKS) books[bookCount++] = { title, safeName, 0, 0 };

  saveMeta();
  uploadBuffer = ""; uploadName = "";
  drawMessage("Book received!", title.c_str());
  delay(2000);
  if (appState == STATE_MENU) drawMenu();
}

// ============================================================
//  META PERSISTENCE
// ============================================================
void loadMeta() {
  bookCount = 0; bookmarkCount = 0;
  if (!LittleFS.exists(META_FILE)) return;
  File f = LittleFS.open(META_FILE, "r");
  if (!f) return;
  JsonDocument doc;
  if (deserializeJson(doc, f) != DeserializationError::Ok) { f.close(); return; }
  f.close();

  currentBook = doc["currentBook"] | -1;
  currentPage = doc["currentPage"] | 0;

  for (JsonObject obj : doc["books"].as<JsonArray>()) {
    if (bookCount >= MAX_BOOKS) break;
    books[bookCount++] = {
      obj["title"].as<String>(), obj["file"].as<String>(),
      obj["pages"] | 0, obj["last"] | 0
    };
  }
  for (JsonObject obj : doc["bookmarks"].as<JsonArray>()) {
    if (bookmarkCount >= MAX_BOOKMARKS) break;
    bookmarks[bookmarkCount++] = { obj["book"] | 0, obj["page"] | 0 };
  }
}

void saveMeta() {
  JsonDocument doc;
  doc["currentBook"] = currentBook;
  doc["currentPage"] = currentPage;

  JsonArray arr = doc["books"].to<JsonArray>();
  for (int i = 0; i < bookCount; i++) {
    JsonObject o = arr.add<JsonObject>();
    o["title"] = books[i].title; o["file"]  = books[i].filename;
    o["pages"] = books[i].totalPages; o["last"] = books[i].lastPage;
  }
  JsonArray bma = doc["bookmarks"].to<JsonArray>();
  for (int i = 0; i < bookmarkCount; i++) {
    JsonObject o = bma.add<JsonObject>();
    o["book"] = bookmarks[i].bookIdx; o["page"] = bookmarks[i].page;
  }
  File f = LittleFS.open(META_FILE, "w");
  if (f) { serializeJson(doc, f); f.close(); }
}

// ============================================================
//  BLE
// ============================================================
void startBLE() {
  if (bleServerRunning) return;
  BLEDevice::init("MiniInkBook");
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new ServerCallbacks());
  BLEService* svc = pServer->createService(SERVICE_UUID);
  pUploadChar = svc->createCharacteristic(UPLOAD_CHAR_UUID,
    BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
  pUploadChar->setCallbacks(new UploadCallback());
  pListChar = svc->createCharacteristic(LIST_CHAR_UUID, BLECharacteristic::PROPERTY_READ);
  pListChar->setCallbacks(new ListCallback());
  pDeleteChar = svc->createCharacteristic(DELETE_CHAR_UUID, BLECharacteristic::PROPERTY_WRITE);
  pDeleteChar->setCallbacks(new DeleteCallback());
  svc->start();
  BLEAdvertising* adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(SERVICE_UUID); adv->setScanResponse(true);
  BLEDevice::startAdvertising();
  bleServerRunning = true;
}

void stopBLE() {
  if (!bleServerRunning) return;
  BLEDevice::stopAdvertising();
  if (pServer && pServer->getConnectedCount() > 0) pServer->disconnect(0);
  BLEDevice::deinit(true);
  bleServerRunning = false; pServer = nullptr;
}

// ============================================================
//  SLEEP QUOTE  (full refresh before deep sleep)
// ============================================================
void drawSleepQuote() {
  static const char* quotes[] = {
    "\"A reader lives a thousand lives before he dies.\" — George R.R. Martin",
    "\"Not all those who wander are lost.\" — J.R.R. Tolkien",
    "\"It is what you read when you don't have to that determines what you will be.\" — Oscar Wilde",
    "\"Libraries are the wardrobes of literature, whence men, properly informed, may bring forth something for ornament, much for curiosity, and more for use.\" — Dion Boucicault",
    "\"I have always imagined that Paradise will be a kind of library.\" — Jorge Luis Borges",
    "\"A library is not a luxury but one of the necessities of life.\" — Henry Ward Beecher",
    "\"Think before you speak. Read before you think.\" — Fran Lebowitz",
    "\"Until I feared I would lose it, I never loved to read. One does not love breathing.\" — Harper Lee",
    "\"The only thing you absolutely have to know is the location of the library.\" — Albert Einstein",
    "\"Today a reader, tomorrow a leader.\" — Margaret Fuller"
  };
  const int quoteCount = 10;
  int idx = (esp_random() % quoteCount);

  display.fastmodeOff();
  display.clearMemory();
  display.setFont(nullptr);
  display.setTextSize(1);
  display.setTextColor(BLACK);
  drawCenteredTitle("Good Night");

  // Word-wrap the quote into lines
  String lines[12];
  int lc = wrapText(String(quotes[idx]), lines, 12, CHARS_PER_LINE);
  int y = MARGIN_Y + CHAR_H + 8;
  for (int i = 0; i < lc; i++) {
    display.setCursor(MARGIN_X, y);
    display.print(lines[i]);
    y += CHAR_H + 2;
  }
  display.update();
}

// ============================================================
//  DEEP SLEEP
// ============================================================
void goToSleep() {
  if (bleServerRunning) stopBLE();
  Serial.flush();
  esp_deep_sleep_start();
}
