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
#include <Fonts/FreeMono9pt7b.h>
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
#define READ_CHAR_W       11         // exact xAdvance for FreeMono9pt7b
#define READ_CHAR_H       16         // full cell height including descenders
#define READ_LINE_SPACING 1          // tighter — mono reads cleanly
#define READ_CHARS_PER_LINE  ((SCREEN_H - MARGIN_X * 2) / READ_CHAR_W)    // exact: ~26
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
  "...Return to book"
};
const int OVERLAY_COUNT = 5;
int overlayIndex = 0;

// ─── Settings menu ───────────────────────────────────────────────────────────
const char* SETTINGS_ITEMS[] = {
  "Bluetooth",
  "Delete a book",
  "Delete all books",
  "Reset all progress",
  "...Return to Menu"
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
// All events now fire on button RELEASE so two long-press durations work correctly

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
  void onConnect(BLEServer*)    override { }
  void onDisconnect(BLEServer*) override { BLEDevice::startAdvertising(); }
};

// ============================================================
//  SETUP
// ============================================================
void setup() {
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
//  BUTTON LOGIC  -- all events fire on button RELEASE
//  Hold duration is measured at the moment of release so
//  BTN_LONG and BTN_XLONG are clearly distinguished.
// ============================================================
BtnEvent readButton() {
  int reading = digitalRead(BTN_PIN);
  BtnEvent result = BTN_NONE;
  unsigned long now = millis();

  // Falling edge: record press start
  if (reading == LOW && btnLastState == HIGH) {
    btnPressTime = now;
  }

  // Rising edge: decide event based on how long it was held
  if (reading == HIGH && btnLastState == LOW) {
    unsigned long held = now - btnPressTime;
    if (held >= XLONG_PRESS_MS) {
      result = BTN_XLONG;
      pendingClicks = 0;
    } else if (held >= LONG_PRESS_MS) {
      result = BTN_LONG;
      pendingClicks = 0;
    } else {
      pendingClicks++;
      btnReleaseTime = now;
    }
  }

  // After double-click window expires, emit single or double
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
        // FIX 3: Rebuild page index before continuing so pageOffsets[] is
        // always valid — loadMeta() restores currentBook/currentPage but
        // does not populate the in-RAM page index, causing out-of-bounds
        // reads (and a crash) if we skip buildPageIndex() here.
        if (currentBook >= 0) {
          buildPageIndex(currentBook);
          if (totalPages == 0) {
            drawMessage("Error", "Could not load book.");
            delay(1500);
            drawMenu(true);
            break;
          }
          // Clamp page in case the book was re-uploaded at a different length
          if (currentPage >= totalPages) currentPage = 0;
          appState = STATE_READING;
          drawPage(currentPage, true);
        } else {
          drawMessage("No book open", "Open Library to\npick a book.");
          delay(1500);
          drawMenu(true);
        }
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
  if (e == BTN_SINGLE) { libIndex = (libIndex + 1) % (bookCount + 1); drawLibrary(); }
  else if (e == BTN_DOUBLE) { libIndex = (libIndex - 1 + bookCount + 1) % (bookCount + 1); drawLibrary(); }
  else if (e == BTN_LONG) {
    if (bookCount == 0 || libIndex == bookCount) {
      // "Back" row selected, or no books
      appState = STATE_MENU; drawMenu(true); return;
    }
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
        // FIX 1 & 2: appState was never set here, so the device stayed in
        // STATE_OVERLAY after drawPage() returned. This caused the overlay
        // to reappear on the next button press AND made "Continue Reading"
        // from the main menu re-enter the overlay instead of the book.
        appState = STATE_READING;
        drawPage(currentPage, true);
        break;
    }
  }
}

void handleChapterSelectInput(BtnEvent e) {
  int totalItems = chapterCount + 1; // +1 for Back
  if (e == BTN_SINGLE) { chapterIndex = (chapterIndex + 1) % totalItems; drawChapterSelect(false); }
  else if (e == BTN_DOUBLE) { chapterIndex = (chapterIndex - 1 + totalItems) % totalItems; drawChapterSelect(false); }
  else if (e == BTN_LONG) {
    if (chapterIndex == chapterCount) {
      // Back to overlay
      overlayIndex = 0;
      appState = STATE_OVERLAY;
      drawOverlay(true);
      return;
    }
    currentPage = chapters[chapterIndex].page;
    books[currentBook].lastPage = currentPage;
    saveMeta();
    appState = STATE_READING;
    drawPage(currentPage, true);
  }
}

void handleBookmarksInput(BtnEvent e) {
  int totalItems = bookmarkCount + 1; // +1 for Back
  if (e == BTN_SINGLE) { bmIndex = (bmIndex + 1) % totalItems; drawBookmarks(); }
  else if (e == BTN_DOUBLE) { bmIndex = (bmIndex - 1 + totalItems) % totalItems; drawBookmarks(); }
  else if (e == BTN_LONG) {
    if (bmIndex == bookmarkCount) {
      // "Back" row
      appState = STATE_MENU; drawMenu(true); return;
    }
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

#define INFO_SCROLL_STEP 3

void handleInfoInput(BtnEvent e) {
  int visibleLines = (SCREEN_W - MARGIN_Y*2 - CHAR_H - 8) / (CHAR_H + 1);
  int maxScroll = max(0, INFO_LINE_COUNT - visibleLines);
  if (e == BTN_SINGLE) {
    infoScrollOffset = min(infoScrollOffset + INFO_SCROLL_STEP, maxScroll);
    drawInfo();
  } else if (e == BTN_DOUBLE) {
    infoScrollOffset = max(infoScrollOffset - INFO_SCROLL_STEP, 0);
    drawInfo();
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
  display.print(selected ? "* " : "  ");
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
#define LIB_VISIBLE 8
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
    // Back row
    int y = MARGIN_Y + CHAR_H + 6 + (CHAR_H + 2) * 3;
    drawRow(MARGIN_X, y, "...Return to Menu", libIndex == 0);
    doUpdate(full); return;
  }

  int totalItems = bookCount + 1; // +1 for Back
  int windowStart = (libIndex / LIB_VISIBLE) * LIB_VISIBLE;
  int y = MARGIN_Y + CHAR_H + 6;
  for (int i = windowStart; i < min(windowStart + LIB_VISIBLE, totalItems); i++) {
    if (i == bookCount) {
      drawRow(MARGIN_X, y, "...Return to Menu", i == libIndex);
    } else {
      char pg[8]; sprintf(pg, "[%d]", books[i].lastPage + 1);
      int maxT = CHARS_PER_LINE - 2 - (int)strlen(pg) - 1;
      String label = books[i].title.substring(0, maxT) + " " + pg;
      drawRow(MARGIN_X, y, label, i == libIndex);
    }
    y += CHAR_H + 2;
  }
  if (totalItems > LIB_VISIBLE) {
    char pos[10]; sprintf(pos, "%d/%d", libIndex + 1, totalItems);
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
  display.setFont(&FreeMono9pt7b);     // was &FreeSerif9pt7b
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
#define CH_VISIBLE 8
void drawChapterSelect(bool full) {
  display.clearMemory();
  display.setTextSize(1);
  display.setTextColor(BLACK);
  drawCenteredTitle("Chapters");

  int totalItems = chapterCount + 1; // +1 for Back
  int windowStart = (chapterIndex / CH_VISIBLE) * CH_VISIBLE;
  int y = MARGIN_Y + CHAR_H + 6;
  for (int i = windowStart; i < min(windowStart + CH_VISIBLE, totalItems); i++) {
    if (i == chapterCount) {
      drawRow(MARGIN_X, y, "...Return to Book", i == chapterIndex);
    } else {
      char pgBuf[8]; sprintf(pgBuf, " p.%d", chapters[i].page + 1);
      int maxName = CHARS_PER_LINE - 2 - (int)strlen(pgBuf);
      String label = chapters[i].name.substring(0, maxName) + pgBuf;
      drawRow(MARGIN_X, y, label, i == chapterIndex);
    }
    y += CHAR_H + 2;
  }
  if (totalItems > CH_VISIBLE) {
    char pos[10]; sprintf(pos, "%d/%d", chapterIndex + 1, totalItems);
    display.setCursor(SCREEN_H - strlen(pos) * CHAR_W - MARGIN_X, MARGIN_Y);
    display.print(pos);
  }
  doUpdate(full);
}

// ─── BOOKMARKS ───────────────────────────────────────────────────────────────
#define BM_VISIBLE 8
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
    int y = MARGIN_Y + CHAR_H + 6 + (CHAR_H + 2) * 3;
    drawRow(MARGIN_X, y, "...Return to Menu", bmIndex == 0);
    doUpdate(full); return;
  }

  int totalItems = bookmarkCount + 1; // +1 for Back
  int windowStart = (bmIndex / BM_VISIBLE) * BM_VISIBLE;
  int y = MARGIN_Y + CHAR_H + 6;
  for (int i = windowStart; i < min(windowStart + BM_VISIBLE, totalItems); i++) {
    if (i == bookmarkCount) {
      drawRow(MARGIN_X, y, "...Return to Menu", i == bmIndex);
    } else {
      int bi = bookmarks[i].bookIdx;
      int pg = bookmarks[i].page;
      String title = (bi >= 0 && bi < bookCount) ? books[bi].title.substring(0, 22) : "?";
      char label[48]; sprintf(label, "%s p.%d", title.c_str(), pg + 1);
      drawRow(MARGIN_X, y, String(label), i == bmIndex);
    }
    y += CHAR_H + 2;
  }
  if (totalItems > BM_VISIBLE) {
    char pos[10]; sprintf(pos, "%d/%d", bmIndex + 1, totalItems);
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
  "NAVIGATION"
  "1 Button Press = Page Forward / Down"
  "2 Button Presses = Page Back / Up"
  "Long Press = Select"
  "Extra Long Press (3 seconds) = Back to menu OR Power Off"
  "                       "
  "ADDING BOOKS"
  "Books can be added and removed via Bluetooth at:"
  "|https://samuelsowanick.github.io/mini-ink-book-connect/|"
  "                       "
  "Bluetooth must be enabled under 'Settings > Bluetooth'"
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
      yield(); // feed watchdog during long index builds
    }
  }

  f.close();
  books[bookIdx].totalPages = totalPages;
  saveMeta();
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
  "A reader lives a thousand lives before he dies. -- George R.R. Martin",
  "Not all readers are leaders, but all leaders are readers. -- Harry S. Truman",
  "Reading is to the mind what exercise is to the body. -- Joseph Addison",
  "The more that you read, the more things you will know. -- Dr. Seuss",
  "I do believe something very magical can happen when you read a good book. -- J.K. Rowling",
  "Reading is a discount ticket to everywhere. -- Mary Schmich",
  "Reading gives us someplace to go when we have to stay where we are. -- Mason Cooley",
  "Once you learn to read, you will be forever free. -- Frederick Douglass",
  "There is no friend as loyal as a book. -- Ernest Hemingway",
  "A book is a dream that you hold in your hands. -- Neil Gaiman",
  "Books are the mirrors of the soul. -- Virginia Woolf",
  "A good book has no ending. -- R.D. Cumming",
  "Books are a uniquely portable magic. -- Stephen King",
  "A book is a gift you can open again and again. -- Garrison Keillor",
  "Books are the quietest and most constant of friends. -- Charles W. Eliot",
  "A room without books is like a body without a soul. -- Marcus Tullius Cicero",
  "Books are lighthouses erected in the great sea of time. -- E.P. Whipple",
  "The books that the world calls immoral are books that show the world its own shame. -- Oscar Wilde",
  "A book must be the axe for the frozen sea within us. -- Franz Kafka",
  "Good friends, good books, and a sleepy conscience: this is the ideal life. -- Mark Twain",
  "Outside of a dog, a book is man's best friend. Inside of a dog, it's too dark to read. -- Groucho Marx",
  "A book lying idle on a shelf is wasted ammunition. -- Henry Miller",
  "A library is not a luxury but one of the necessities of life. -- Henry Ward Beecher",
  "Google can bring you back 100,000 answers. A librarian can bring you back the right one. -- Neil Gaiman",
  "Libraries are the thin red line between civilization and barbarism. -- Neil Gaiman",
  "A library is the delivery room for the birth of ideas. -- Norman Cousins",
  "The library is the temple of learning, and learning has liberated more people than all the wars in history. -- Carl T. Rowan",
  "Libraries store the energy that fuels the imagination. -- Sidney Sheldon",
  "A great library contains the diary of the human race. -- George Mercer Dawson",
  "The only thing that you absolutely have to know is the location of the library. -- Albert Einstein",
  "I have always imagined that Paradise will be a kind of library. -- Jorge Luis Borges",
  "My alma mater was books, a good library. -- Malcolm X",
  "Reading is the sole means by which we slip, involuntarily, often helplessly, into another's skin. -- Joyce Carol Oates",
  "That's the thing about books. They let you travel without moving your feet. -- Jhumpa Lahiri",
  "Books were my pass to personal freedom. -- Oprah Winfrey",
  "Fiction reveals truth that reality obscures. -- Ralph Waldo Emerson",
  "The book you don't read won't help. -- Jim Rohn",
  "I am not a speed reader. I am a speed understander. -- Isaac Asimov",
  "To read without reflecting is like eating without digesting. -- Edmund Burke",
  "Reading makes a full man; conference a ready man; and writing an exact man. -- Francis Bacon",
  "Show me a family of readers and I will show you the people who move the world. -- Napoleon Bonaparte",
  "In books I have traveled, not only to other worlds, but into my own. -- Anna Quindlen",
  "Books are the carriers of civilization. -- Barbara Tuchman",
  "Read a thousand books, and your words will flow like a river. -- Lisa See",
  "The reading of all good books is like conversation with the finest men of past centuries. -- Rene Descartes",
  "If there's a book that you want to read, but it hasn't been written yet, then you must write it. -- Toni Morrison",
  "There is no greater agony than bearing an untold story inside you. -- Maya Angelou",
  "A writer only begins a book. A reader finishes it. -- Samuel Johnson",
  "Every book you pick up has its own lesson or lessons. -- Stephen King",
  "The purpose of a writer is to keep civilization from destroying itself. -- Albert Camus",
  "Literature is a way of noticing the existence of other people. -- George Saunders",
  "A writer is a reader moved to emulation. -- Saul Bellow",
  "Literacy is a bridge from misery to hope. -- Kofi Annan",
  "One child, one teacher, one book, one pen can change the world. -- Malala Yousafzai",
  "The man who does not read good books has no advantage over the man who cannot read them. -- Mark Twain",
  "Literacy is not a luxury; it is a right and a responsibility. -- Hillary Clinton",
  "The more I read, the more I acquire, the more certain I am that I know nothing. -- Voltaire",
  "Children are made readers on the laps of their parents. -- Emilie Buchwald",
  "Librarians save lives: by handing the right book, at the right time, to a kid in need. -- Neil Gaiman",
  "To read is to voyage through time. -- Carl Sagan",
  "Until I feared I would lose it, I never loved to read. One does not love breathing. -- Harper Lee",
  "I kept always two books in my pocket: one to read, one to write in. -- Robert Louis Stevenson",
  "There is no such thing as a child who hates to read; there are only children who have not found the right book. -- Frank Serafini",
  "Literature is the art of discovering something extraordinary about ordinary people. -- Boris Pasternak",
  "Books are not made for furniture, but there is nothing else that so beautifully furnishes a house. -- Henry Ward Beecher",
  "Without words, without writing, and without books there would be no history, there could be no concept of humanity. -- Hermann Hesse",
  "Books are the plane, and the train, and the road. They are the destination, and the journey. -- Anna Quindlen",
  "In the library, I felt better, words you could trust and look at till you understood them. -- Jeanette Winterson",
  "Literature is the most agreeable way of ignoring life. -- Fernando Pessoa",
  "Stories are a communal currency of humanity. -- Tahir Shah",
  "That is part of the beauty of all literature. You discover that your longings are universal longings. -- F. Scott Fitzgerald",
  "We are all stories in the end. -- Steven Moffat",
  "A story has no beginning or end; arbitrarily one chooses that moment of experience from which to look back. -- Graham Greene",
  "All great literature is one of two stories: a man goes on a journey, or a stranger comes to town. -- Leo Tolstoy",
  "Literature is not only a mirror; it is also a map. -- Margaret Atwood",
  "Books let us into their souls and lay open to us the secrets of our own. -- William Hazlitt",
  "To add a library to a house is to give that house a soul. -- Cicero",
  "What a school thinks about its library is a measure of what it feels about education. -- Harold Howe",
  "Libraries allow children to ask questions about the world and find the answers. -- Julius Lester",
  "A book is a loaded gun in the house next door. -- Ray Bradbury",
  "Education is the most powerful weapon which you can use to change the world. -- Nelson Mandela",
  "The more you read, the more you know. The more you know, the smarter you grow. -- Robert Kiyosaki",
  "Books are the bees which carry the quickening pollen from one to another mind. -- James Russell Lowell",
  "Reading is an act of civilization; it is one of the greatest acts of civilization. -- Milton Glaser",
  "Think before you speak. Read before you think. -- Fran Lebowitz",
  "Today a reader, tomorrow a leader. -- Margaret Fuller",
  "I've been drunk for about a week now, and I thought it might sober me up to sit in a library. -- F. Scott Fitzgerald",
  "Libraries are the wardrobes of literature, whence men may bring forth something for ornament, much for curiosity, and more for use. -- Dion Boucicault",
  "Reading is the key that opens doors to many good things in life. -- Rupert Evans"
};
  
  const int quoteCount = 10;
  int idx = (esp_random() % quoteCount);

  display.fastmodeOff();
  display.clearMemory();
  display.setFont(nullptr);
  display.setTextSize(1);
  display.setTextColor(BLACK);

  // Build quoted string
  String quoted = String("\"") + quotes[idx] + "\"";

  // Word-wrap the quote
  String lines[14];
  int lc = wrapText(quoted, lines, 14, CHARS_PER_LINE);

  // Center the block vertically
  int blockH = lc * (CHAR_H + 2);
  int y = (SCREEN_W - blockH) / 2;
  if (y < MARGIN_Y) y = MARGIN_Y;

  for (int i = 0; i < lc; i++) {
    // Center each line horizontally
    int lineLen = lines[i].length();
    int x = (SCREEN_H - lineLen * CHAR_W) / 2;
    if (x < MARGIN_X) x = MARGIN_X;
    display.setCursor(x, y);
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
  esp_deep_sleep_start();
}
