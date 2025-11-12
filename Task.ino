#include <TFT_eSPI.h>
#include <SPI.h>
#include <FS.h>
#include <SPIFFS.h>
#include <ArduinoJson.h>

// ------------------------------------------------------
// HARDWARE / SCREEN CONFIG
// ------------------------------------------------------
#define TASKS_FILE "/tasks.json"

const int MAX_TASKS = 64;

// Screen layout for 4" 480x320, rotation 1
const int SCREEN_W = 480;
const int SCREEN_H = 320;
const int TOP_BAR_H = 32;
const int ARROW_W = 56;  // right column for scroll arrows

// 5 visible bubbles (spec), fixed height
const int VISIBLE_TASKS = 5;
const int BUBBLE_H = (SCREEN_H - TOP_BAR_H) / VISIBLE_TASKS;

// Backlight pin
#define TFT_BL 27  // from you

// ------------------------------------------------------
// COLORS
// ------------------------------------------------------
#define COLOR_BG 0x0842          // dark greenish background
#define COLOR_TOPBAR 0x0245      // dark green bar
#define COLOR_BUBBLE 0x03A3      // green bubble fill
#define COLOR_BUBBLE_OUT 0x0660  // darker green border
#define COLOR_TEXT_MAIN TFT_WHITE
#define COLOR_TEXT_MUTED 0xC618  // grey
#define COLOR_DETAILS_BG 0x0208  // dark overlay
#define COLOR_NOTES_BOX 0x0325   // dark green box

// Status colors (match PC as close as 16-bit allows)
#define COLOR_IN_PROGRESS 0x07E0  // bright green
#define COLOR_PAUSED 0xFEC0       // gold-ish
#define COLOR_WAITING_ON 0x4E7F   // sky blue-ish
#define COLOR_DONE 0x0464         // deep green
#define COLOR_READY_SHIP 0xA254   // purple-ish

// ------------------------------------------------------
// DATA MODEL
// ------------------------------------------------------
struct Task {
  String title;
  String month;
  int day;
  String timeStr;  // e.g. "3:45 PM"
  String status;   // "", "In Progress", etc.
  bool eod;        // End-of-day flag (priority 1)
  String notes;    // from PC
};

Task tasks[MAX_TASKS];
int taskCount = 0;

// ------------------------------------------------------
// UI STATE
// ------------------------------------------------------
TFT_eSPI tft = TFT_eSPI();

// scroll/list
int topIndex = 0;        // first visible task index
int selectedIndex = -1;  // for move mode

bool editMode = true;  // true = Edit, false = Move
bool screenOn = true;
bool backlightOn = true;

// popup state
bool detailsOpen = false;
int detailsTaskIndex = -1;
int notesScrollPx = 0;

bool statusPopupOpen = false;
int statusTaskIndex = -1;

// time sync (minutes since midnight based on PC)
bool timeSynced = false;
int baseMinutesOfDay = 0;
unsigned long baseMillis = 0;

// cached top-bar time string to avoid redraw if unchanged
String lastTimeBarStr;

// auto-off after 5:10 PM
bool autoOffTriggered = false;

// top-bar mode button rect
int modeBtnX = 0, modeBtnY = 0, modeBtnW = 80, modeBtnH = 22;

// serial line buffer
String incomingLine;

// ------------------------------------------------------
// FORWARD DECLARATIONS
// ------------------------------------------------------
void drawTopBar();
void drawTasks();
void drawScrollArrows();
void drawTaskBubble(int taskIndex, int slotIndex);
void handleTouch(uint16_t x, uint16_t y);
void handleDetailsTouch(uint16_t x, uint16_t y);
void handleStatusPopupTouch(uint16_t x, uint16_t y);
String getTopBarTimeString();
void saveTasks();
void loadTasks();
void sendAllTasksJson();
void addTaskFromJson(JsonObject obj);
void editTaskFromJson(JsonObject obj);
void deleteTaskByIndex(int idx);
void clearAllTasks();
uint16_t statusColor(const String &s);
void wakeScreenIfNeeded();

// ======================================================
// HELPERS
// ======================================================
const char *getJsonString(JsonObject obj, const char *key, const char *def = "") {
  if (!obj.containsKey(key)) return def;
  JsonVariant v = obj[key];
  if (v.isNull()) return def;
  if (v.is<const char *>()) {
    return v.as<const char *>();
  }
  return def;
}

int getJsonInt(JsonObject obj, const char *key, int def = 0) {
  if (!obj.containsKey(key)) return def;
  JsonVariant v = obj[key];
  if (v.isNull()) return def;
  if (v.is<int>()) return v.as<int>();
  if (v.is<long>()) return (int)v.as<long>();
  return def;
}

// ======================================================
// STORAGE (SPIFFS)
// ======================================================
bool tasksEqualForDup(const Task &a, const Task &b) {
  return a.title == b.title && a.month == b.month && a.day == b.day && a.timeStr == b.timeStr && a.eod == b.eod;
}

bool isDuplicateTask(const Task &t) {
  for (int i = 0; i < taskCount; i++) {
    if (tasksEqualForDup(tasks[i], t)) {
      return true;
    }
  }
  return false;
}

void saveTasks() {
  DynamicJsonDocument doc(12288);
  JsonArray arr = doc.createNestedArray("tasks");

  for (int i = 0; i < taskCount; i++) {
    JsonObject o = arr.createNestedObject();
    o["title"] = tasks[i].title;
    o["month"] = tasks[i].month;
    o["day"] = tasks[i].day;
    o["time"] = tasks[i].timeStr;
    o["priority"] = tasks[i].eod ? 1 : 0;
    o["status"] = tasks[i].status;
    o["notes"] = tasks[i].notes;
  }

  fs::File f = SPIFFS.open(TASKS_FILE, FILE_WRITE);
  if (!f) return;
  serializeJson(doc, f);
  f.close();
}

void loadTasks() {
  taskCount = 0;
  if (!SPIFFS.exists(TASKS_FILE)) return;

  fs::File f = SPIFFS.open(TASKS_FILE, FILE_READ);
  if (!f) return;

  DynamicJsonDocument doc(12288);
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) return;

  JsonArray arr = doc["tasks"].as<JsonArray>();
  for (JsonObject o : arr) {
    if (taskCount >= MAX_TASKS) break;

    Task t;
    t.title = String(getJsonString(o, "title", "Untitled"));
    t.month = String(getJsonString(o, "month", ""));
    t.day = getJsonInt(o, "day", 0);
    t.timeStr = String(getJsonString(o, "time", ""));
    int pr = getJsonInt(o, "priority", 0);
    t.eod = (pr == 1);
    t.status = String(getJsonString(o, "status", ""));
    t.notes = String(getJsonString(o, "notes", ""));

    if (t.status == "NULL") t.status = "";
    if (!isDuplicateTask(t)) {
      tasks[taskCount++] = t;
    }
  }
}

// ======================================================
// TIME
// ======================================================
int getCurrentMinutesOfDay() {
  if (!timeSynced) return -1;
  unsigned long elapsed = (millis() - baseMillis) / 60000UL;
  int mins = baseMinutesOfDay + (int)elapsed;
  mins %= (24 * 60);
  if (mins < 0) mins += 24 * 60;
  return mins;
}

String getTopBarTimeString() {
  if (!timeSynced) return "Sync Time";

  int nowMin = getCurrentMinutesOfDay();
  if (nowMin < 0) return "Sync Time";

  int hour = (nowMin / 60) % 24;
  int minute = nowMin % 60;

  // 12-hour display
  int dispHour = hour % 12;
  if (dispHour == 0) dispHour = 12;
  String ampm = (hour < 12) ? "AM" : "PM";

  String timePart = String(dispHour) + ":";
  if (minute < 10) timePart += "0";
  timePart += String(minute) + " " + ampm;

  int target = 17 * 60;  // 5 PM
  int diff = target - nowMin;

  if (diff <= 0) {
    return timePart + "  Work Done";
  }

  int h = diff / 60;
  int m = diff % 60;
  String s = timePart + "  ";
  if (h > 0) {
    s += String(h) + "h ";
  }
  s += String(m) + "m to 5PM";
  return s;
}

// ======================================================
// STATUS COLOR
// ======================================================
uint16_t statusColor(const String &sIn) {
  String s = sIn;
  s.trim();
  if (s == "In Progress") return COLOR_IN_PROGRESS;
  if (s == "Paused") return COLOR_PAUSED;
  if (s == "Waiting On" || s == "Waiting on") return COLOR_WAITING_ON;
  if (s == "Done") return COLOR_DONE;
  if (s == "Ready to Ship") return COLOR_READY_SHIP;
  return COLOR_TEXT_MAIN;
}

// ======================================================
// SERIAL / JSON – SYNC WITH PC
// ======================================================
void sendAllTasksJson() {
  DynamicJsonDocument doc(12288);
  JsonArray arr = doc.createNestedArray("tasks");
  for (int i = 0; i < taskCount; i++) {
    JsonObject o = arr.createNestedObject();
    o["title"] = tasks[i].title;
    o["month"] = tasks[i].month;
    o["day"] = tasks[i].day;
    o["time"] = tasks[i].timeStr;
    o["priority"] = tasks[i].eod ? 1 : 0;
    o["status"] = tasks[i].status;
    o["notes"] = tasks[i].notes;
  }
  serializeJson(doc, Serial);
  Serial.println();
}

void sendMoveToPC(int src, int dst) {
  DynamicJsonDocument doc(256);
  doc["cmd"] = "MOVE_TASK";
  doc["src"] = src;
  doc["dst"] = dst;
  serializeJson(doc, Serial);
  Serial.println();
}

// ADD_TASK from PC, with duplicate protection
void addTaskFromJson(JsonObject obj) {
  if (taskCount >= MAX_TASKS) return;

  Task t;
  t.title = String(getJsonString(obj, "title", "Untitled"));
  t.month = String(getJsonString(obj, "month", ""));
  t.day = getJsonInt(obj, "day", 0);
  t.timeStr = String(getJsonString(obj, "time", ""));
  int pr = getJsonInt(obj, "priority", 0);
  t.eod = (pr == 1);
  t.status = String(getJsonString(obj, "status", ""));
  t.notes = String(getJsonString(obj, "notes", ""));

  if (t.status == "NULL") t.status = "";

  if (isDuplicateTask(t)) {
    // notify PC that we skipped a duplicate
    DynamicJsonDocument ev(256);
    ev["event"] = "duplicate_skipped";
    ev["title"] = t.title;
    serializeJson(ev, Serial);
    Serial.println();
    return;
  }

  tasks[taskCount++] = t;
  saveTasks();
  drawTasks();
  drawTopBar();
}

// EDIT_TASK from PC
void editTaskFromJson(JsonObject obj) {
  int idx = getJsonInt(obj, "id", -1);
  if (idx < 0 || idx >= taskCount) return;

  Task &t = tasks[idx];

  if (obj.containsKey("title") && !obj["title"].isNull()) {
    t.title = String(getJsonString(obj, "title", t.title.c_str()));
  }
  if (obj.containsKey("month") && !obj["month"].isNull()) {
    t.month = String(getJsonString(obj, "month", t.month.c_str()));
  }
  if (obj.containsKey("day") && !obj["day"].isNull()) {
    t.day = getJsonInt(obj, "day", t.day);
  }
  if (obj.containsKey("time") && !obj["time"].isNull()) {
    t.timeStr = String(getJsonString(obj, "time", t.timeStr.c_str()));
  }
  if (obj.containsKey("priority") && !obj["priority"].isNull()) {
    int pr = getJsonInt(obj, "priority", t.eod ? 1 : 0);
    t.eod = (pr == 1);
  }
  if (obj.containsKey("status") && !obj["status"].isNull()) {
    t.status = String(getJsonString(obj, "status", t.status.c_str()));
    if (t.status == "NULL") t.status = "";
  }
  if (obj.containsKey("notes") && !obj["notes"].isNull()) {
    t.notes = String(getJsonString(obj, "notes", t.notes.c_str()));
  }

  saveTasks();
  drawTasks();
  drawTopBar();
}

void deleteTaskByIndex(int idx) {
  if (idx < 0 || idx >= taskCount) return;
  for (int i = idx; i < taskCount - 1; i++) {
    tasks[i] = tasks[i + 1];
  }
  taskCount--;
  if (topIndex > 0 && topIndex >= taskCount) {
    topIndex = max(0, taskCount - VISIBLE_TASKS);
  }
  if (selectedIndex >= taskCount) selectedIndex = -1;
  saveTasks();
  drawTasks();
  drawTopBar();
}

void clearAllTasks() {
  taskCount = 0;
  topIndex = 0;
  selectedIndex = -1;
  saveTasks();
  drawTasks();
  drawTopBar();
  sendAllTasksJson();
}

// MOVE_TASK from PC (reorder)
void moveTaskFromPc(int src, int dst) {
  if (src < 0 || src >= taskCount || dst < 0 || dst >= taskCount) return;
  if (src == dst) return;

  Task tmp = tasks[src];
  if (src < dst) {
    for (int i = src; i < dst; i++) tasks[i] = tasks[i + 1];
  } else {
    for (int i = src; i > dst; i--) tasks[i] = tasks[i - 1];
  }
  tasks[dst] = tmp;

  saveTasks();
  drawTasks();
  drawTopBar();
}

// Board-initiated move (tap source then target)
void moveTaskToIndexFromBoard(int src, int dst) {
  if (src < 0 || src >= taskCount || dst < 0 || dst >= taskCount) return;
  if (src == dst) return;

  Task tmp = tasks[src];
  if (src < dst) {
    for (int i = src; i < dst; i++) tasks[i] = tasks[i + 1];
  } else {
    for (int i = src; i > dst; i--) tasks[i] = tasks[i - 1];
  }
  tasks[dst] = tmp;
  saveTasks();

  // notify PC
  sendMoveToPC(src, dst);

  // keep selected on moved
  selectedIndex = dst;

  drawTasks();
  drawTopBar();
}

// ------------------- SCREEN POWER -------------------
void backlightOnFn() {
  digitalWrite(TFT_BL, HIGH);
  backlightOn = true;
}
void backlightOffFn() {
  digitalWrite(TFT_BL, LOW);
  backlightOn = false;
}

void setScreenOn(bool on) {
  if (on) {
    screenOn = true;
    autoOffTriggered = false;
    backlightOnFn();
    tft.fillScreen(COLOR_BG);
    drawTopBar();
    drawTasks();
  } else {
    screenOn = false;
    backlightOffFn();
    tft.fillScreen(TFT_BLACK);
  }
}

void wakeScreenIfNeeded() {
  if (!screenOn || !backlightOn) {
    setScreenOn(true);
  }
}

// ------------------- SERIAL COMMAND -------------------
void processIncomingLine(const String &line) {
  wakeScreenIfNeeded();  // wake on any serial activity

  DynamicJsonDocument doc(1024);
  DeserializationError err = deserializeJson(doc, line);
  if (err) {
    // ignore invalid JSON
    return;
  }
  JsonObject obj = doc.as<JsonObject>();
  const char *cmdC = getJsonString(obj, "cmd", "");
  if (!cmdC || cmdC[0] == '\0') return;
  String cmd = cmdC;

  if (cmd == "ADD_TASK") {
    addTaskFromJson(obj);
  } else if (cmd == "EDIT_TASK") {
    editTaskFromJson(obj);
  } else if (cmd == "DELETE_TASK") {
    int idx = getJsonInt(obj, "id", -1);
    deleteTaskByIndex(idx);
  } else if (cmd == "CLEAR_ALL") {
    clearAllTasks();
  } else if (cmd == "LIST_TASKS") {
    sendAllTasksJson();
  } else if (cmd == "MOVE_TASK") {
    int src = getJsonInt(obj, "src", -1);
    int dst = getJsonInt(obj, "dst", -1);
    moveTaskFromPc(src, dst);
  } else if (cmd == "SET_TIME") {
    int h = getJsonInt(obj, "hour", 0);
    int m = getJsonInt(obj, "minute", 0);
    baseMinutesOfDay = h * 60 + m;
    baseMillis = millis();
    timeSynced = true;
    autoOffTriggered = false;
    drawTopBar();
  } else if (cmd == "TIME") {
    long epoch = 0;
    if (obj.containsKey("epoch") && !obj["epoch"].isNull()) {
      epoch = obj["epoch"].as<long>();
    }
    if (epoch > 0) {
      // Convert epoch seconds to local time (adjust for your timezone offset)
      // Example: EST (UTC-5) → offset = -5 * 3600
      const long TZ_OFFSET = -5 * 3600;  // adjust if you’re not in EST
      time_t localT = epoch + TZ_OFFSET;

      int hours = (localT / 3600) % 24;
      int mins  = (localT / 60) % 60;

      baseMinutesOfDay = hours * 60 + mins;
      baseMillis = millis();
      timeSynced = true;
      autoOffTriggered = false;
      drawTopBar();
    }
  } else if (cmd == "SCREEN") {
    const char *state = getJsonString(obj, "state", "ON");
    if (strcmp(state, "OFF") == 0) {
      setScreenOn(false);
    } else {
      setScreenOn(true);
    }
  }
}

// ======================================================
// UI DRAWING
// ======================================================
void drawTopBar() {
  if (!screenOn) return;

  tft.fillRect(0, 0, SCREEN_W, TOP_BAR_H, COLOR_TOPBAR);
  tft.setTextFont(2);
  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(COLOR_TEXT_MAIN, COLOR_TOPBAR);

  // Left: number of tasks
  String left = "Tasks: " + String(taskCount);
  tft.drawString(left, 6, TOP_BAR_H / 2);

  // Center: mode button (Edit / Move)
  modeBtnW = 80;
  modeBtnH = 22;
  modeBtnX = (SCREEN_W / 2) - (modeBtnW / 2);
  modeBtnY = (TOP_BAR_H / 2) - (modeBtnH / 2);

  uint16_t modeColor = editMode ? 0x05A3 : 0x0443;  // slightly different greens
  tft.fillRoundRect(modeBtnX, modeBtnY, modeBtnW, modeBtnH, 5, modeColor);
  tft.drawRoundRect(modeBtnX, modeBtnY, modeBtnW, modeBtnH, 5, COLOR_BUBBLE_OUT);

  tft.setTextDatum(CC_DATUM);
  tft.setTextColor(COLOR_TEXT_MAIN, modeColor);
  tft.drawString(editMode ? "Edit" : "Move", modeBtnX + modeBtnW / 2, modeBtnY + modeBtnH / 2);

  // Right: time + countdown
  String timeStr = getTopBarTimeString();
  lastTimeBarStr = timeStr;
  tft.setTextDatum(MR_DATUM);
  tft.setTextColor(COLOR_TEXT_MAIN, COLOR_TOPBAR);
  tft.drawString(timeStr, SCREEN_W - 4, TOP_BAR_H / 2);
}

void drawScrollArrows() {
  if (!screenOn) return;

  int colX = SCREEN_W - ARROW_W;
  tft.fillRect(colX, TOP_BAR_H, ARROW_W, SCREEN_H - TOP_BAR_H, COLOR_BG);

  // Up arrow
  int upCx = colX + ARROW_W / 2;
  int upCy = TOP_BAR_H + 20;
  tft.fillTriangle(
    upCx, upCy - 10,
    upCx - 10, upCy + 6,
    upCx + 10, upCy + 6,
    COLOR_BUBBLE_OUT);

  // Down arrow
  int dnCx = colX + ARROW_W / 2;
  int dnCy = SCREEN_H - 20;
  tft.fillTriangle(
    dnCx, dnCy + 10,
    dnCx - 10, dnCy - 6,
    dnCx + 10, dnCy - 6,
    COLOR_BUBBLE_OUT);
}

void drawTaskBubble(int taskIndex, int slotIndex) {
  if (!screenOn) return;
  if (taskIndex < 0 || taskIndex >= taskCount) return;

  int x = 8;
  int y = TOP_BAR_H + slotIndex * BUBBLE_H + 4;
  int w = SCREEN_W - ARROW_W - 16;
  int h = BUBBLE_H - 8;

  Task &t = tasks[taskIndex];

  // Bubble background
  tft.fillRoundRect(x, y, w, h, 8, COLOR_BUBBLE);

  // Border: white if status blank, else green
  uint16_t borderColor = t.status.length() ? COLOR_BUBBLE_OUT : TFT_WHITE;
  tft.drawRoundRect(x, y, w, h, 8, borderColor);

  // Left status indicator bar
  uint16_t sc = t.status.length() ? statusColor(t.status) : borderColor;
  tft.fillRect(x + 2, y + 4, 4, h - 8, sc);

  // Selection in Move mode
  bool selected = (!editMode && (taskIndex == selectedIndex));
  if (selected) {
    tft.drawRoundRect(x - 1, y - 1, w + 2, h + 2, 9, TFT_YELLOW);
  }

  // Title
  tft.setTextDatum(TL_DATUM);
  tft.setTextFont(2);
  tft.setTextColor(COLOR_TEXT_MAIN, COLOR_BUBBLE);
  String title = t.title;
  if (title.length() > 26) {
    title = title.substring(0, 23) + "...";
  }
  tft.drawString(title, x + 10, y + 4);

  // Date / time / EOD (simple line)
  String dateStr = "";
  if (t.month.length() > 0 && t.month != "None" && t.day > 0) {
    dateStr = t.month + " " + String(t.day);
  }
  String line2 = "";
  if (dateStr.length() && t.timeStr.length()) {
    line2 = dateStr + " " + t.timeStr;
  } else if (dateStr.length()) {
    line2 = dateStr;
  } else if (t.timeStr.length()) {
    line2 = t.timeStr;
  }
  if (t.eod) {
    if (line2.length()) line2 += "  EOD";
    else line2 = "EOD";
  }

  tft.setTextColor(COLOR_TEXT_MUTED, COLOR_BUBBLE);
  tft.drawString(line2, x + 10, y + 24);

  // Status text
  if (t.status.length()) {
    tft.setTextColor(statusColor(t.status), COLOR_BUBBLE);
    tft.drawString(t.status, x + 10, y + 44);
  }

  // (D) and (E) buttons – big enough to press
  int btnW = 40;
  int btnH = 26;
  int spacing = 4;
  int eX = x + w - 6 - btnW;
  int dX = eX - spacing - btnW;
  int btnY = y + h - btnH - 6;

  // (D) – Details
  tft.fillRoundRect(dX, btnY, btnW, btnH, 4, COLOR_BG);
  tft.drawRoundRect(dX, btnY, btnW, btnH, 4, COLOR_BUBBLE_OUT);
  tft.setTextDatum(CC_DATUM);
  tft.setTextColor(COLOR_TEXT_MAIN, COLOR_BG);
  tft.drawString("(D)", dX + btnW / 2, btnY + btnH / 2);

  // (E) – Status
  tft.fillRoundRect(eX, btnY, btnW, btnH, 4, COLOR_BG);
  tft.drawRoundRect(eX, btnY, btnW, btnH, 4, COLOR_BUBBLE_OUT);
  tft.drawString("(E)", eX + btnW / 2, btnY + btnH / 2);
}

void drawTasks() {
  if (!screenOn) return;

  // Clear area except arrow column and top bar
  tft.fillRect(0, TOP_BAR_H, SCREEN_W - ARROW_W, SCREEN_H - TOP_BAR_H, COLOR_BG);

  for (int i = 0; i < VISIBLE_TASKS; i++) {
    int idx = topIndex + i;
    if (idx < taskCount) {
      drawTaskBubble(idx, i);
    }
  }

  drawScrollArrows();
}

// ------------- DETAILS POPUP (with notes scroll) -------------
void drawDetailsPopup(int idx) {
  if (!screenOn) return;
  if (idx < 0 || idx >= taskCount) return;

  detailsOpen = true;
  detailsTaskIndex = idx;

  // dark overlay
  tft.fillRect(0, 0, SCREEN_W, SCREEN_H, COLOR_DETAILS_BG);

  int px = 20;
  int py = 20;
  int pw = SCREEN_W - 40;
  int ph = SCREEN_H - 40;

  tft.fillRoundRect(px, py, pw, ph, 8, COLOR_BG);
  tft.drawRoundRect(px, py, pw, ph, 8, COLOR_BUBBLE_OUT);

  Task &t = tasks[idx];

  tft.setTextFont(2);
  tft.setTextColor(COLOR_TEXT_MAIN, COLOR_BG);
  tft.setTextDatum(TC_DATUM);
  tft.drawString("Task Details", px + pw / 2, py + 8);

  tft.setTextDatum(TL_DATUM);
  int textX = px + 8;
  int textY = py + 32;

  // Title
  tft.drawString("Title: " + t.title, textX, textY);
  textY += 18;

  // Date
  String dateStr = "";
  if (t.month.length() > 0 && t.month != "None" && t.day > 0) {
    dateStr = t.month + " " + String(t.day);
  }
  tft.drawString("Date: " + dateStr, textX, textY);
  textY += 18;

  // Time
  tft.drawString("Time: " + t.timeStr, textX, textY);
  textY += 18;

  // Status
  tft.setTextColor(statusColor(t.status), COLOR_BG);
  tft.drawString("Status: " + t.status, textX, textY);
  tft.setTextColor(COLOR_TEXT_MAIN, COLOR_BG);
  textY += 18;

  // EOD
  tft.drawString(String("EOD: ") + (t.eod ? "Yes" : "No"), textX, textY);
  textY += 22;

  // Notes label
  tft.drawString("Notes:", textX, textY);
  textY += 14;

  // Notes box
  int notesX = px + 8;
  int notesY = textY;
  int notesW = pw - 16;
  int notesH = ph - (notesY - py) - 50;  // leave space for Close button

  tft.fillRect(notesX, notesY, notesW, notesH, COLOR_NOTES_BOX);
  tft.drawRect(notesX, notesY, notesW, notesH, COLOR_BUBBLE_OUT);

  // arrows inside notes box (right edge)
  int upAx = notesX + notesW - 14;
  int upAy = notesY + 10;
  tft.fillTriangle(
    upAx, upAy - 6,
    upAx - 6, upAy + 4,
    upAx + 6, upAy + 4,
    COLOR_BUBBLE_OUT);
  int dnAx = notesX + notesW - 14;
  int dnAy = notesY + notesH - 10;
  tft.fillTriangle(
    dnAx, dnAy + 6,
    dnAx - 6, dnAy - 4,
    dnAx + 6, dnAy - 4,
    COLOR_BUBBLE_OUT);

  // draw notes text with vertical scroll
  tft.setTextFont(2);
  tft.setTextColor(COLOR_TEXT_MAIN, COLOR_NOTES_BOX);
  tft.setTextDatum(TL_DATUM);

  String notes = t.notes;
  if (notes == "NULL") notes = "";

  int textStartY = notesY + 2 - notesScrollPx;
  int cursorX = notesX + 4;
  int cursorY = textStartY;

  String line;
  for (unsigned int i = 0; i <= notes.length(); i++) {
    char c = (i < notes.length()) ? notes[i] : '\n';
    if (c == '\n' || line.length() >= 36 || i == notes.length()) {
      if (line.length()) {
        if (cursorY > notesY - 20 && cursorY < notesY + notesH + 20) {
          tft.drawString(line, cursorX, cursorY);
        }
        cursorY += 16;
        line = "";
      }
      if (c != '\n' && i < notes.length()) {
        line += c;
      }
    } else {
      line += c;
    }
  }

  // Close button
  int cx = px + pw / 2 - 40;
  int cy = py + ph - 30;
  int cw = 80;
  int ch = 24;
  tft.fillRoundRect(cx, cy, cw, ch, 5, COLOR_BUBBLE);
  tft.drawRoundRect(cx, cy, cw, ch, 5, COLOR_BUBBLE_OUT);
  tft.setTextDatum(CC_DATUM);
  tft.setTextColor(COLOR_TEXT_MAIN, COLOR_BUBBLE);
  tft.drawString("Close", cx + cw / 2, cy + ch / 2);
}

// ------------- STATUS POPUP -------------
void drawStatusPopup(int idx) {
  if (!screenOn) return;
  if (idx < 0 || idx >= taskCount) return;

  statusPopupOpen = true;
  statusTaskIndex = idx;

  // dark overlay
  tft.fillRect(0, 0, SCREEN_W, SCREEN_H, COLOR_DETAILS_BG);

  int px = 60;
  int py = 40;
  int pw = SCREEN_W - 120;
  int ph = SCREEN_H - 80;

  tft.fillRoundRect(px, py, pw, ph, 8, COLOR_BG);
  tft.drawRoundRect(px, py, pw, ph, 8, COLOR_BUBBLE_OUT);

  tft.setTextFont(2);
  tft.setTextColor(COLOR_TEXT_MAIN, COLOR_BG);
  tft.setTextDatum(TC_DATUM);
  tft.drawString("Set Status", px + pw / 2, py + 8);

  const char *opts[] = {
    "In Progress",
    "Paused",
    "Waiting On",
    "Done",
    "Ready to Ship",
    "None"
  };
  const int optCount = 6;

  tft.setTextDatum(TL_DATUM);
  int rowY = py + 28;
  int rowH = 26;
  for (int i = 0; i < optCount; i++) {
    int ry = rowY + i * rowH;
    tft.drawRect(px + 8, ry, pw - 16, rowH - 2, COLOR_BUBBLE_OUT);
    tft.drawString(opts[i], px + 12, ry + 4);
  }

  // hint
  tft.setTextColor(COLOR_TEXT_MUTED, COLOR_BG);
  tft.drawString("Tap a status (None = clear)", px + 10, py + ph - 30);
}

// ======================================================
// TOUCH HANDLING
// ======================================================
void scrollUp() {
  if (topIndex > 0) {
    topIndex--;
    drawTasks();
  }
}

void scrollDown() {
  if (topIndex + VISIBLE_TASKS < taskCount) {
    topIndex++;
    drawTasks();
  }
}

void handleDetailsTouch(uint16_t x, uint16_t y) {
  if (!detailsOpen) return;

  int px = 20;
  int py = 20;
  int pw = SCREEN_W - 40;
  int ph = SCREEN_H - 40;

  int notesX = px + 8;
  int notesY = py + 32 + 18 * 4 + 22;  // from drawDetailsPopup layout
  int notesW = pw - 16;
  int notesH = ph - (notesY - py) - 50;

  // Up arrow area
  int upAx = notesX + notesW - 22;
  int upAy = notesY;
  int upAw = 24;
  int upAh = notesH / 2;

  // Down arrow area
  int dnAx = notesX + notesW - 22;
  int dnAy = notesY + notesH / 2;
  int dnAw = 24;
  int dnAh = notesH / 2;

  // Close button
  int cx = px + pw / 2 - 40;
  int cy = py + ph - 30;
  int cw = 80;
  int ch = 24;

  if (x >= upAx && x <= upAx + upAw && y >= upAy && y <= upAy + upAh) {
    // scroll up
    notesScrollPx -= 16;
    if (notesScrollPx < 0) notesScrollPx = 0;
    drawDetailsPopup(detailsTaskIndex);
    return;
  }
  if (x >= dnAx && x <= dnAx + dnAw && y >= dnAy && y <= dnAy + dnAh) {
    // scroll down
    notesScrollPx += 16;
    drawDetailsPopup(detailsTaskIndex);
    return;
  }
  if (x >= cx && x <= cx + cw && y >= cy && y <= cy + ch) {
    // close
    detailsOpen = false;
    notesScrollPx = 0;
    tft.fillScreen(COLOR_BG);
    drawTopBar();
    drawTasks();
    return;
  }
}

void handleStatusPopupTouch(uint16_t x, uint16_t y) {
  if (!statusPopupOpen) return;

  int px = 60;
  int py = 40;
  int pw = SCREEN_W - 120;
  int ph = SCREEN_H - 80;

  int rowY = py + 28;
  int rowH = 26;

  if (x < px + 8 || x > px + pw - 8) return;
  if (y < rowY || y > rowY + rowH * 6) return;

  int idx = (y - rowY) / rowH;  // 0..5

  const char *opts[] = {
    "In Progress",
    "Paused",
    "Waiting On",
    "Done",
    "Ready to Ship",
    "None"
  };

  if (idx >= 0 && idx < 6 && statusTaskIndex >= 0 && statusTaskIndex < taskCount) {
    if (strcmp(opts[idx], "None") == 0) {
      tasks[statusTaskIndex].status = "";
    } else {
      tasks[statusTaskIndex].status = opts[idx];
    }
    saveTasks();

    // notify PC of status change
    DynamicJsonDocument doc(256);
    doc["cmd"] = "EDIT_TASK";
    doc["id"] = statusTaskIndex;
    doc["status"] = tasks[statusTaskIndex].status;
    serializeJson(doc, Serial);
    Serial.println();
  }

  statusPopupOpen = false;
  tft.fillScreen(COLOR_BG);
  drawTopBar();
  drawTasks();
}

void handleTouch(uint16_t x, uint16_t y) {
  if (!screenOn) return;

  // Popups first
  if (detailsOpen) {
    handleDetailsTouch(x, y);
    return;
  }
  if (statusPopupOpen) {
    handleStatusPopupTouch(x, y);
    return;
  }

  // Top bar: mode toggle
  if (y < TOP_BAR_H) {
    if (x >= modeBtnX && x <= modeBtnX + modeBtnW && y >= modeBtnY && y <= modeBtnY + modeBtnH) {
      editMode = !editMode;
      selectedIndex = -1;
      drawTopBar();
      drawTasks();
    }
    return;
  }

  // Scroll arrow column
  if (x >= SCREEN_W - ARROW_W) {
    int upTop = TOP_BAR_H;
    int upBottom = TOP_BAR_H + (SCREEN_H - TOP_BAR_H) / 2;
    if (y >= upTop && y <= upBottom) {
      // Up
      scrollUp();
    } else {
      // Down
      scrollDown();
    }
    return;
  }

  // Task bubbles
  if (y >= TOP_BAR_H) {
    int relY = y - TOP_BAR_H;
    int slot = relY / BUBBLE_H;
    if (slot < 0 || slot >= VISIBLE_TASKS) return;
    int idx = topIndex + slot;
    if (idx < 0 || idx >= taskCount) return;

    int bx = 8;
    int bw = SCREEN_W - ARROW_W - 16;
    int by = TOP_BAR_H + slot * BUBBLE_H + 4;
    int bh = BUBBLE_H - 8;

    // (D) and (E) button areas
    int btnW = 40;
    int btnH = 26;
    int spacing = 4;
    int eX = bx + bw - 6 - btnW;
    int dX = eX - spacing - btnW;
    int btnY = by + bh - btnH - 6;

    bool inD = (x >= dX && x <= dX + btnW && y >= btnY && y <= btnY + btnH);
    bool inE = (x >= eX && x <= eX + btnW && y >= btnY && y <= btnY + btnH);

    if (editMode) {
      // EDIT MODE:
      if (inD) {
        // Details popup (with notes)
        notesScrollPx = 0;
        drawDetailsPopup(idx);
        return;
      } else if (inE) {
        // Status popup
        drawStatusPopup(idx);
        return;
      } else {
        // tap body in EDIT mode: no-op (keeps UI behavior)
        selectedIndex = idx;
        drawTasks();
        return;
      }
    } else {
      // MOVE MODE:
      // D/E are ignored; tapping a bubble chooses source/target
      if (selectedIndex < 0) {
        // first tap: select source
        selectedIndex = idx;
        drawTasks();
      } else {
        // second tap: move selected to this index
        int src = selectedIndex;
        int dst = idx;
        selectedIndex = -1;
        moveTaskToIndexFromBoard(src, dst);
      }
      return;
    }
  }
}

// ======================================================
// SETUP / LOOP
// ======================================================
void setup() {
  Serial.begin(115200);
  delay(200);

  pinMode(TFT_BL, OUTPUT);
  backlightOnFn();

  if (!SPIFFS.begin(true)) {
    // SPIFFS init fail; continue with empty tasks
  }

  tft.init();
  tft.setRotation(1);
  tft.fillScreen(COLOR_BG);
  tft.setTextFont(2);

  loadTasks();
  drawTopBar();
  drawTasks();
}

void loop() {
  // Serial JSON (wakes screen on any activity)
  if (Serial.available() && (!screenOn || !backlightOn)) {
    wakeScreenIfNeeded();
  }
  while (Serial.available() > 0) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      if (incomingLine.length() > 0) {
        processIncomingLine(incomingLine);
        incomingLine = "";
      }
    } else {
      incomingLine += c;
      if (incomingLine.length() > 512) {
        incomingLine = "";
      }
    }
  }

  // Time bar: update once per second if string changed
  static unsigned long lastTimeUpdate = 0;
  if (millis() - lastTimeUpdate > 1000) {
    lastTimeUpdate = millis();
    String nowStr = getTopBarTimeString();
    if (nowStr != lastTimeBarStr) {
      drawTopBar();
    }
  }

  // Auto screen-off 10 minutes after 5 PM
  if (timeSynced && !autoOffTriggered) {
    int mins = getCurrentMinutesOfDay();
    if (mins >= (17 * 60 + 10)) {  // 5:10 PM
      autoOffTriggered = true;
      setScreenOn(false);
    }
  }

  // Touch
  uint16_t tx, ty;
  if (tft.getTouch(&tx, &ty)) {
    // fix inverted up/down (from your note)
    ty = SCREEN_H - ty;

    wakeScreenIfNeeded();
    handleTouch(tx, ty);

    // simple debounce
    delay(180);
    // wait for release
    uint16_t x2, y2;
    while (tft.getTouch(&x2, &y2)) {
      delay(20);
    }
  }
}
