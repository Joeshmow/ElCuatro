#include <M5Unified.h>
#include <NimBLEDevice.h>
#include <Wire.h>
#include <Adafruit_seesaw.h>

// ─── State Definition ────────────────────────────────────────────────────────
enum GameState {
    START_SCREEN,
    CHARACTER_SELECT,
    MAP_SELECT,
    COUNTDOWN,
    RACE,
    ROUND_RESULT,
    CHECK_FIRST_TO_7,
    FINAL_RESULT,
    UPLOAD_SCORE,
    BACK_TO_MENU
};

// ─── Vehicle Data ─────────────────────────────────────────────────────────────
struct Vehicle {
    const char* name;
    const char* tagline;
    int accel;
    int handling;
    int turning;
    uint16_t color;
};

const Vehicle vehicles[3] = {
    { "KART",  "All-Rounder",  3, 3, 3, TFT_RED   },
    { "MOTO",  "Speed Demon",  5, 5, 2, TFT_BLUE  },
    { "BUGGY", "Corner King",  2, 3, 5, TFT_GREEN }
};
const int NUM_VEHICLES = 3;

// ─── Map Data ─────────────────────────────────────────────────────────────────
// All tracks are defined in a 640x480 world space.
// The camera centres on the player so only a 320x240 window is visible.
struct Map {
    const char* name;
    const char* difficulty;
    uint16_t    diffColor;
    uint16_t    bgColor;      // screen background fill (grass / city / space)
    float       startX;       // player spawn in world coords
    float       startY;
};

const Map maps[3] = {
    { "OVAL",         "EASY",   TFT_GREEN,  TFT_GREEN,     170, 110 },
    { "CITY CIRCUIT", "MEDIUM", TFT_YELLOW, TFT_DARKGREEN, 85,  165 },
    { "RAINBOW ROAD", "HARD",   TFT_RED,    TFT_BLACK,     85,  100 }
};
const int NUM_MAPS = 3;

// ─── Global Game Data ─────────────────────────────────────────────────────────
GameState currentState    = START_SCREEN;
GameState previousState   = BACK_TO_MENU;

int  playerWins        = 0;
int  opponentWins      = 0;
int  selectedCharacter = 0;
int  selectedMap       = 0;
int  previewCharacter  = 0;
int  previewMap        = 0;
int  countdownValue    = 3;
unsigned long countdownTimer = 0;
unsigned long lastTouchTime  = 0;
bool screenNeedsRedraw = true;
bool raceNeedsRedraw   = true;
bool bleConnected      = false;

// ─── Player World Position (camera anchor) ────────────────────────────────────
float playerWorldX = 320.0f;
float playerWorldY =  85.0f;

// ─── Forward Declarations ─────────────────────────────────────────────────────
void changeState(GameState next);
void handleCurrentState();

void drawStartScreen();
void drawCharacterSelect();
void drawMapSelect();
void drawCountdown();
void drawRace();
void drawRoundResult();
void drawFinalResult();
void drawUploadScore();
void drawBackToMenu();

void handleStartScreenTouch();
void handleCharacterSelectTouch();
void handleMapSelectTouch();
void handleRaceTouch();

void drawVehicle(int type, int cx, int cy);
void drawKart(int cx, int cy, uint16_t col);
void drawMoto(int cx, int cy, uint16_t col);
void drawBuggy(int cx, int cy, uint16_t col);
void drawStatRow(int x, int y, const char* label, int value);
void drawArrowButton(int cx, int cy, bool pointRight);

// Camera-space world draw helpers
void wRect(int wx, int wy, int ww, int wh, uint16_t col);
void wRRect(int wx, int wy, int ww, int wh, int r, uint16_t col);
void wLine(int wx1, int wy1, int wx2, int wy2, uint16_t col);
void wFEllipse(int wx, int wy, int rx, int ry, uint16_t col);
void wEllipse(int wx, int wy, int rx, int ry, uint16_t col);

void drawTrackWorld(int mapIdx);
void drawMapPreviewBox(int mapIdx, int bx, int by, int bw, int bh);
void drawRaceHUD();
void resetPlayerForMap();

void initBLE();
void stopBLE();
void initWiFi();
void uploadScoreToGCP();
void syncRaceStateBLE();

// ─── Setup ────────────────────────────────────────────────────────────────────
void setup() {
    auto cfg = M5.config();
    M5.begin(cfg);
    M5.Display.setTextSize(2);
    M5.Display.fillScreen(TFT_BLACK);
    Serial.begin(115200);
    changeState(START_SCREEN);
}

// ─── Main Loop ────────────────────────────────────────────────────────────────
void loop() {
    M5.update();
    if (currentState != previousState) {
        screenNeedsRedraw = true;
        previousState = currentState;
    }
    handleCurrentState();
}

// ─── State Machine ────────────────────────────────────────────────────────────
void changeState(GameState next) {
    Serial.printf("State: %d -> %d\n", currentState, next);
    currentState = next;
    screenNeedsRedraw = true;
}

void handleCurrentState() {
    switch (currentState) {

        case START_SCREEN:
            if (screenNeedsRedraw) { drawStartScreen(); screenNeedsRedraw = false; }
            handleStartScreenTouch();
            break;

        case CHARACTER_SELECT:
            if (screenNeedsRedraw) {
                previewCharacter = selectedCharacter;
                drawCharacterSelect();
                screenNeedsRedraw = false;
            }
            handleCharacterSelectTouch();
            break;

        case MAP_SELECT:
            if (screenNeedsRedraw) {
                previewMap = selectedMap;
                drawMapSelect();
                screenNeedsRedraw = false;
            }
            handleMapSelectTouch();
            break;

        case COUNTDOWN:
            if (screenNeedsRedraw) {
                countdownValue = 3;
                countdownTimer = millis();
                resetPlayerForMap();
                drawCountdown();
                screenNeedsRedraw = false;
            }
            if (millis() - countdownTimer >= 1000) {
                countdownTimer = millis();
                countdownValue--;
                drawCountdown();
                if (countdownValue <= 0) changeState(RACE);
            }
            break;

        // RACE redraws every frame — player moves, world scrolls
        case RACE:
            syncRaceStateBLE();
            handleRaceTouch();
            if (screenNeedsRedraw || raceNeedsRedraw) {
                drawRace();
                screenNeedsRedraw = false;
                raceNeedsRedraw = false;
            }
            break;

        case ROUND_RESULT:
            if (screenNeedsRedraw) {
                drawRoundResult();
                screenNeedsRedraw = false;
            }
            if (millis() - countdownTimer >= 3000) changeState(CHECK_FIRST_TO_7);
            break;

        case CHECK_FIRST_TO_7:
            if (playerWins >= 7 || opponentWins >= 7) changeState(FINAL_RESULT);
            else                                       changeState(COUNTDOWN);
            break;

        case FINAL_RESULT:
            if (screenNeedsRedraw) {
                countdownTimer = millis();
                drawFinalResult();
                screenNeedsRedraw = false;
            }
            if (millis() - countdownTimer >= 2000) changeState(UPLOAD_SCORE);
            break;

        case UPLOAD_SCORE:
            if (screenNeedsRedraw) {
                drawUploadScore();
                screenNeedsRedraw = false;
                stopBLE();
                initWiFi();
                uploadScoreToGCP();
            }
            break;

        case BACK_TO_MENU:
            if (screenNeedsRedraw) { drawBackToMenu(); screenNeedsRedraw = false; }
            playerWins = 0; opponentWins = 0;
            selectedCharacter = 0; selectedMap = 0;
            bleConnected = false;
            delay(1500);
            changeState(START_SCREEN);
            break;
    }
}

// =============================================================================
// CAMERA HELPERS
// The player is always drawn at screen centre (160, 120).
// Every world object is offset by -(playerWorld - screenCentre).
// =============================================================================

// Camera top-left corner in world space
inline int camX() { return (int)playerWorldX - 160; }
inline int camY() { return (int)playerWorldY - 120; }

// Draw a world-space filled rect, automatically offset by camera
void wRect(int wx, int wy, int ww, int wh, uint16_t col) {
    M5.Display.fillRect(wx - camX(), wy - camY(), ww, wh, col);
}

// Draw a world-space filled rounded rect, offset by camera
void wRRect(int wx, int wy, int ww, int wh, int r, uint16_t col) {
    M5.Display.fillRoundRect(wx - camX(), wy - camY(), ww, wh, r, col);
}

// Draw a world-space line, offset by camera
void wLine(int wx1, int wy1, int wx2, int wy2, uint16_t col) {
    M5.Display.drawLine(wx1 - camX(), wy1 - camY(), wx2 - camX(), wy2 - camY(), col);
}

// Draw a world-space filled ellipse, offset by camera
void wFEllipse(int wx, int wy, int rx, int ry, uint16_t col) {
    M5.Display.fillEllipse(wx - camX(), wy - camY(), rx, ry, col);
}

// Draw a world-space ellipse outline, offset by camera
void wEllipse(int wx, int wy, int rx, int ry, uint16_t col) {
    M5.Display.drawEllipse(wx - camX(), wy - camY(), rx, ry, col);
}

// =============================================================================
// TRACK WORLD GEOMETRY
// World space = 640 x 480.  Tracks are drawn with wRect/wRRect so the camera
// automatically shows the window around the player.
// =============================================================================

void drawTrackWorld(int mapIdx) {
    switch (mapIdx) {

        // ── OVAL ── simple oval ring, track width ~80px ──────────────────────
        case 0:
            // Build a clean oval ring with ellipses to avoid round-rect artifacts.
            wFEllipse(320, 240, 250, 170, TFT_DARKGREY);
            wFEllipse(320, 240, 170,  90, TFT_GREEN);
            wEllipse(320, 240, 250, 170, TFT_WHITE);
            wEllipse(320, 240, 170,  90, TFT_WHITE);

            // Dashed lane markers on top/bottom straights.
            for (int i = 0; i < 7; i++) {
                int dx = 160 + i * 48;
                wRect(dx, 114, 22, 3, TFT_WHITE);
                wRect(dx, 364, 22, 3, TFT_WHITE);
            }

            // Red/white kerb strips on top and bottom straights.
            for (int i = 0; i < 16; i++) {
                uint16_t kerb = (i % 2 == 0) ? TFT_RED : TFT_WHITE;
                wRect(116 + i * 26, 104, 16, 6, kerb);
                wRect(116 + i * 26, 370, 16, 6, kerb);
            }

            // Vertical start/finish checkerboard near top-left straight.
            for (int r = 0; r < 6; r++) {
                for (int c = 0; c < 4; c++) {
                    uint16_t tile = ((r + c) % 2 == 0) ? TFT_WHITE : TFT_BLACK;
                    wRect(146 + c * 5, 108 + r * 5, 5, 5, tile);
                }
            }
            break;

        // ── CITY CIRCUIT ── cross-shaped road with centre lines ───────────────
        case 1:
            // Horizontal road
            wRect(60,  140, 520, 80, TFT_DARKGREY);
            // Vertical road
            wRect(260,  60,  80, 360, TFT_DARKGREY);
            // Intersection box (slightly lighter)
            wRect(260, 140, 80, 80, 0x6B4D);
            // Centre dashes – horizontal
            for (int i = 0; i < 6; i++)
                wRect(65 + i * 88, 175, 50, 8, TFT_WHITE);
            // Centre dashes – vertical
            for (int i = 0; i < 5; i++)
                wRect(291, 65 + i * 72, 8, 44, TFT_WHITE);
            // Pavements (lighter strips along road edges)
            wRect(60, 136, 520,  8, 0x9CD3);
            wRect(60, 216, 520,  8, 0x9CD3);
            wRect(256,  60,   8, 360, 0x9CD3);
            wRect(336,  60,   8, 360, 0x9CD3);
            // Start/finish line
            wRect(60, 152, 20, 48, TFT_WHITE);
            break;

        // ── RAINBOW ROAD ── colourful rectangular loop ────────────────────────
        case 2: {
            uint16_t c0 = M5.Display.color565( 80, 80, 220); // blue  – top
            uint16_t c1 = M5.Display.color565(220, 80, 80);  // red   – right
            uint16_t c2 = M5.Display.color565( 80,200, 80);  // green – bottom
            uint16_t c3 = M5.Display.color565(220,180, 40);  // amber – left
            // Four coloured road segments forming a loop
            wRect( 60,  75, 520, 75, c0); // top
            wRect(505,  75,  75, 330, c1); // right
            wRect( 60, 330, 520, 75, c2); // bottom
            wRect( 60,  75,  75, 330, c3); // left
            // Black interior cutout
            wRect(135, 150, 370, 180, TFT_BLACK);
            // Sparkling edge lines (white dashes along each segment)
            for (int i = 0; i < 8; i++) {
                wRect( 75 + i * 62, 76,  40, 4, TFT_WHITE);   // top dashes
                wRect( 75 + i * 62, 141, 40, 4, TFT_WHITE);
                wRect( 75 + i * 62, 330, 40, 4, TFT_WHITE);   // bottom dashes
                wRect( 75 + i * 62, 394, 40, 4, TFT_WHITE);
            }
            for (int i = 0; i < 5; i++) {
                wRect( 60, 90 + i * 58, 4, 40, TFT_WHITE);    // left dashes
                wRect(134, 90 + i * 58, 4, 40, TFT_WHITE);
                wRect(506, 90 + i * 58, 4, 40, TFT_WHITE);    // right dashes
                wRect(580, 90 + i * 58, 4, 40, TFT_WHITE);
            }
            // Start/finish
            wRect(60,  92, 20, 58, TFT_WHITE);
            break;
        }
    }
}

// =============================================================================
// MAP PREVIEW  —  scaled-down version of the same track geometry.
// bx/by = top-left of the preview box in screen coords, bw/bh = box size.
// World is 640x480; we scale to bw x bh.
// =============================================================================

void drawMapPreviewBox(int mapIdx, int bx, int by, int bw, int bh) {
    float sx = bw / 640.0f;
    float sy = bh / 480.0f;

    // Helper macros (local to this function via lambdas if compiler supports,
    // otherwise inline — using a small local function approach):
    #define PR(wx, wy, ww, wh, col) \
        M5.Display.fillRect(bx + (int)((wx)*sx), by + (int)((wy)*sy), \
                            max(1,(int)((ww)*sx)), max(1,(int)((wh)*sy)), col)
    #define PRR(wx, wy, ww, wh, r, col) \
        M5.Display.fillRoundRect(bx + (int)((wx)*sx), by + (int)((wy)*sy), \
                                 max(2,(int)((ww)*sx)), max(2,(int)((wh)*sy)), r, col)
    #define PE(wx, wy, rx, ry, col) \
        M5.Display.fillEllipse(bx + (int)((wx)*sx), by + (int)((wy)*sy), \
                               max(1,(int)((rx)*sx)), max(1,(int)((ry)*sy)), col)
    #define PEO(wx, wy, rx, ry, col) \
        M5.Display.drawEllipse(bx + (int)((wx)*sx), by + (int)((wy)*sy), \
                               max(1,(int)((rx)*sx)), max(1,(int)((ry)*sy)), col)

    switch (mapIdx) {
        case 0:
            M5.Display.fillRect(bx, by, bw, bh, TFT_GREEN);
            PE(320, 240, 250, 170, TFT_DARKGREY);
            PE(320, 240, 170,  90, TFT_GREEN);
            PEO(320, 240, 250, 170, TFT_WHITE);
            PEO(320, 240, 170,  90, TFT_WHITE);
            PR(146, 108, 20, 30, TFT_WHITE);
            break;
        case 1:
            M5.Display.fillRect(bx, by, bw, bh, TFT_DARKGREEN);
            PR(60, 140, 520, 80, TFT_DARKGREY);
            PR(260, 60,  80,360, TFT_DARKGREY);
            PR(260,140,  80, 80, 0x6B4D);
            PR(60, 136, 520,  8, 0x9CD3);
            PR(60, 216, 520,  8, 0x9CD3);
            PR(256, 60,   8,360, 0x9CD3);
            PR(336, 60,   8,360, 0x9CD3);
            PR(60, 152,  20, 48, TFT_WHITE);
            break;
        case 2: {
            uint16_t c0 = M5.Display.color565( 80, 80,220);
            uint16_t c1 = M5.Display.color565(220, 80, 80);
            uint16_t c2 = M5.Display.color565( 80,200, 80);
            uint16_t c3 = M5.Display.color565(220,180, 40);
            M5.Display.fillRect(bx, by, bw, bh, TFT_BLACK);
            PR( 60,  75, 520, 75, c0);
            PR(505,  75,  75,330, c1);
            PR( 60, 330, 520, 75, c2);
            PR( 60,  75,  75,330, c3);
            PR(135, 150, 370,180, TFT_BLACK);
            PR( 60,  92,  20, 58, TFT_WHITE);
            break;
        }
    }

    #undef PR
    #undef PRR
    #undef PE
    #undef PEO
}

// =============================================================================
// MAP SELECT  —  same cycle-through UX as character select
// =============================================================================

void drawMapSelect() {
    M5.Display.fillScreen(TFT_BLACK);

    // Title
    M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
    M5.Display.drawCentreString("SELECT TRACK", 160, 6, 2);

    // Position dots
    for (int i = 0; i < NUM_MAPS; i++) {
        uint16_t col = (i == previewMap) ? TFT_WHITE : TFT_DARKGREY;
        M5.Display.fillCircle(148 + i * 12, 26, 4, col);
    }

    // Large preview box: 220 x 140, centred horizontally
    const int px = 50, py = 36, pw = 220, ph = 140;
    M5.Display.drawRect(px - 1, py - 1, pw + 2, ph + 2, TFT_DARKGREY);
    drawMapPreviewBox(previewMap, px, py, pw, ph);

    // Arrows at left/right midpoints of preview box
    drawArrowButton(24,       py + ph / 2, false);
    drawArrowButton(296,      py + ph / 2, true);

    // Track name
    M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
    M5.Display.drawCentreString(maps[previewMap].name, 160, 186, 3);

    // Difficulty badge
    M5.Display.setTextColor(maps[previewMap].diffColor, TFT_BLACK);
    M5.Display.drawCentreString(maps[previewMap].difficulty, 160, 214, 2);

    // Confirm hint
    M5.Display.setTextColor(TFT_YELLOW, TFT_BLACK);
    M5.Display.drawCentreString("TAP TRACK TO SELECT", 160, 232, 1);
}

void handleMapSelectTouch() {
    auto touch = M5.Touch.getDetail();
    if (touch.state != m5::touch_state_t::touch_begin) return;
    if (millis() - lastTouchTime < 200) return;
    lastTouchTime = millis();

    int tx = touch.x;
    int ty = touch.y;
    const int py = 36, ph = 140;

    // Left arrow
    if (tx < 55 && ty > py && ty < py + ph) {
        previewMap = (previewMap - 1 + NUM_MAPS) % NUM_MAPS;
        drawMapSelect();
        return;
    }
    // Right arrow
    if (tx > 265 && ty > py && ty < py + ph) {
        previewMap = (previewMap + 1) % NUM_MAPS;
        drawMapSelect();
        return;
    }
    // Centre tap: confirm
    if (tx >= 50 && tx <= 270 && ty >= py && ty <= py + ph) {
        selectedMap = previewMap;
        M5.Display.fillRect(50, py, 220, ph, TFT_WHITE);
        delay(80);
        drawMapSelect();
        delay(120);
        changeState(COUNTDOWN);
    }
}

// =============================================================================
// RACE  —  camera follows player; vehicle is always at screen centre
// =============================================================================

void resetPlayerForMap() {
    playerWorldX = maps[selectedMap].startX;
    playerWorldY = maps[selectedMap].startY;
    raceNeedsRedraw = true;
}

void drawRaceHUD() {
    // Small black bar top-left: lap counter
    M5.Display.fillRect(0, 0, 80, 18, TFT_BLACK);
    M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
    M5.Display.drawString("LAP 1/3", 4, 2, 1);

    // Win counter top-right
    M5.Display.fillRect(240, 0, 80, 18, TFT_BLACK);
    char buf[16];
    snprintf(buf, sizeof(buf), "%d - %d", playerWins, opponentWins);
    M5.Display.drawString(buf, 244, 2, 1);

    // Vehicle name badge bottom-left
    M5.Display.fillRect(0, 224, 90, 16, TFT_BLACK);
    M5.Display.setTextColor(vehicles[selectedCharacter].color, TFT_BLACK);
    M5.Display.drawString(vehicles[selectedCharacter].name, 4, 225, 1);
}

void drawRace() {
    M5.Display.startWrite();

    // 1. Fill screen with background colour for this map (cheap base layer)
    M5.Display.fillScreen(maps[selectedMap].bgColor);

    // 2. Draw all track geometry offset by camera — world scrolls, player stays centred
    drawTrackWorld(selectedMap);

    // 3. Player vehicle is ALWAYS drawn at exact screen centre
    drawVehicle(selectedCharacter, 160, 120);

    // 4. HUD drawn last so it sits on top of everything
    drawRaceHUD();

    M5.Display.endWrite();
}

// =============================================================================
// CHARACTER SELECT  (unchanged from previous version)
// =============================================================================

void drawCharacterSelect() {
    M5.Display.fillScreen(TFT_BLACK);
    M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
    M5.Display.drawCentreString("CHOOSE YOUR RIDE", 160, 6, 2);
    for (int i = 0; i < NUM_VEHICLES; i++) {
        uint16_t col = (i == previewCharacter) ? TFT_WHITE : TFT_DARKGREY;
        M5.Display.fillCircle(148 + i * 12, 26, 4, col);
    }
    drawArrowButton(24,  98, false);
    drawArrowButton(296, 98, true);
    drawVehicle(previewCharacter, 160, 93);
    M5.Display.setTextColor(vehicles[previewCharacter].color, TFT_BLACK);
    M5.Display.drawCentreString(vehicles[previewCharacter].name, 160, 158, 4);
    M5.Display.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    M5.Display.drawCentreString(vehicles[previewCharacter].tagline, 160, 180, 1);
    drawStatRow(62, 194, "ACCEL   ", vehicles[previewCharacter].accel);
    drawStatRow(62, 207, "HANDLING", vehicles[previewCharacter].handling);
    drawStatRow(62, 220, "TURNING ", vehicles[previewCharacter].turning);
    M5.Display.setTextColor(TFT_YELLOW, TFT_BLACK);
    M5.Display.drawCentreString("TAP VEHICLE TO SELECT", 160, 233, 1);
}

void drawStatRow(int x, int y, const char* label, int value) {
    M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
    M5.Display.drawString(label, x, y, 1);
    for (int i = 0; i < 5; i++) {
        uint16_t col = (i < value) ? TFT_GREEN : 0x2104;
        M5.Display.fillRect(x + 66 + i * 16, y, 12, 9, col);
        M5.Display.drawRect(x + 66 + i * 16, y, 12, 9, TFT_DARKGREY);
    }
}

void drawArrowButton(int cx, int cy, bool pointRight) {
    M5.Display.fillRoundRect(cx - 20, cy - 22, 40, 44, 8, 0x2104);
    M5.Display.drawRoundRect(cx - 20, cy - 22, 40, 44, 8, TFT_DARKGREY);
    if (pointRight)
        M5.Display.fillTriangle(cx - 8, cy - 12, cx + 12, cy, cx - 8, cy + 12, TFT_WHITE);
    else
        M5.Display.fillTriangle(cx + 8, cy - 12, cx - 12, cy, cx + 8, cy + 12, TFT_WHITE);
}

void handleCharacterSelectTouch() {
    auto touch = M5.Touch.getDetail();
    if (touch.state != m5::touch_state_t::touch_begin) return;
    if (millis() - lastTouchTime < 200) return;
    lastTouchTime = millis();
    int tx = touch.x, ty = touch.y;
    if (tx < 55 && ty > 50 && ty < 150) {
        previewCharacter = (previewCharacter - 1 + NUM_VEHICLES) % NUM_VEHICLES;
        drawCharacterSelect(); return;
    }
    if (tx > 265 && ty > 50 && ty < 150) {
        previewCharacter = (previewCharacter + 1) % NUM_VEHICLES;
        drawCharacterSelect(); return;
    }
    if (tx > 55 && tx < 265 && ty > 45 && ty < 155) {
        selectedCharacter = previewCharacter;
        M5.Display.fillRoundRect(55, 45, 210, 115, 8, TFT_WHITE);
        delay(80);
        drawCharacterSelect();
        delay(120);
        changeState(MAP_SELECT);
    }
}

// =============================================================================
// VEHICLE DRAWING  (unchanged)
// =============================================================================

void drawVehicle(int type, int cx, int cy) {
    switch (type) {
        case 0: drawKart (cx, cy, vehicles[0].color); break;
        case 1: drawMoto (cx, cy, vehicles[1].color); break;
        case 2: drawBuggy(cx, cy, vehicles[2].color); break;
    }
}

void drawKart(int cx, int cy, uint16_t col) {
    uint16_t wc = TFT_DARKGREY;
    M5.Display.fillRoundRect(cx - 44, cy - 30, 14, 20, 3, wc);
    M5.Display.fillRoundRect(cx + 30, cy - 30, 14, 20, 3, wc);
    M5.Display.fillRoundRect(cx - 44, cy + 14, 14, 20, 3, wc);
    M5.Display.fillRoundRect(cx + 30, cy + 14, 14, 20, 3, wc);
    M5.Display.fillRect(cx - 30, cy + 30, 60,  5, wc);
    M5.Display.fillRect(cx - 26, cy + 33,  8, 10, wc);
    M5.Display.fillRect(cx + 18, cy + 33,  8, 10, wc);
    M5.Display.fillRoundRect(cx - 30, cy - 24, 60, 52, 6, col);
    M5.Display.fillRoundRect(cx - 14, cy - 16, 28, 28, 5, 0x2104);
    M5.Display.fillTriangle(cx - 18, cy - 24, cx + 18, cy - 24, cx, cy - 36, col);
    M5.Display.fillCircle(cx, cy - 4, 10, TFT_WHITE);
    M5.Display.fillCircle(cx, cy - 4,  7, TFT_YELLOW);
    M5.Display.fillRect(cx - 32, cy + 4, 4, 8, TFT_ORANGE);
    M5.Display.fillRect(cx + 28, cy + 4, 4, 8, TFT_ORANGE);
}

void drawMoto(int cx, int cy, uint16_t col) {
    uint16_t wc = TFT_DARKGREY, rim = 0x6B4D;
    M5.Display.fillRoundRect(cx - 10, cy - 46, 20, 26, 5, wc);
    M5.Display.fillRoundRect(cx - 10, cy + 22, 20, 26, 5, wc);
    M5.Display.fillRoundRect(cx -  5, cy - 43, 10, 20, 3, rim);
    M5.Display.fillRoundRect(cx -  5, cy + 25, 10, 20, 3, rim);
    M5.Display.fillRoundRect(cx - 11, cy - 22, 22, 46, 4, col);
    M5.Display.fillRect(cx - 24, cy - 18, 48,  5, 0xC618);
    M5.Display.fillRect(cx - 24, cy - 26,  5,  8, 0xC618);
    M5.Display.fillRect(cx + 19, cy - 26,  5,  8, 0xC618);
    M5.Display.fillTriangle(cx - 8, cy - 22, cx + 8, cy - 22, cx, cy - 34, col);
    M5.Display.drawLine(cx - 5, cy - 25, cx + 5, cy - 25, TFT_CYAN);
    M5.Display.fillCircle(cx, cy - 8, 11, TFT_WHITE);
    M5.Display.fillEllipse(cx, cy - 6,  7,  5, TFT_CYAN);
    M5.Display.fillRect(cx + 11, cy + 6, 4, 18, TFT_ORANGE);
    M5.Display.fillCircle(cx + 13, cy + 24, 3, TFT_YELLOW);
}

void drawBuggy(int cx, int cy, uint16_t col) {
    uint16_t wc = 0x4208;
    M5.Display.fillRoundRect(cx - 50, cy - 32, 18, 24, 4, wc);
    M5.Display.fillRoundRect(cx + 32, cy - 32, 18, 24, 4, wc);
    M5.Display.fillRoundRect(cx - 50, cy + 12, 18, 24, 4, wc);
    M5.Display.fillRoundRect(cx + 32, cy + 12, 18, 24, 4, wc);
    int wx[4] = { cx-50, cx+32, cx-50, cx+32 };
    int wy[4] = { cy-32, cy-32, cy+12, cy+12 };
    for (int i = 0; i < 4; i++) {
        M5.Display.drawLine(wx[i]+5, wy[i]+4, wx[i]+5,  wy[i]+20, TFT_DARKGREY);
        M5.Display.drawLine(wx[i]+11,wy[i]+4, wx[i]+11, wy[i]+20, TFT_DARKGREY);
    }
    M5.Display.drawLine(cx-28, cy-34, cx-28, cy-18, 0xC618);
    M5.Display.drawLine(cx+28, cy-34, cx+28, cy-18, 0xC618);
    M5.Display.drawLine(cx-28, cy-34, cx+28, cy-34, 0xC618);
    M5.Display.fillRoundRect(cx - 32, cy - 26, 64, 56, 5, col);
    M5.Display.fillRect(cx - 24, cy - 22, 48, 14, 0x03EF);
    M5.Display.drawLine(cx - 24, cy - 22, cx + 24, cy - 22, TFT_CYAN);
    M5.Display.fillCircle(cx, cy - 4, 10, TFT_WHITE);
    M5.Display.fillEllipse(cx, cy - 2,  7,  4, 0x03EF);
    M5.Display.fillRect(cx - 10, cy + 14,  8, 4, 0x2104);
    M5.Display.fillRect(cx +  2, cy + 14,  8, 4, 0x2104);
    M5.Display.fillRect(cx - 26, cy + 26, 52, 4, TFT_RED);
}

// =============================================================================
// REMAINING SCREENS
// =============================================================================

void drawStartScreen() {
    M5.Display.fillScreen(TFT_BLACK);
    M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
    M5.Display.drawCentreString("KART RACER", 160, 60, 4);
    M5.Display.drawCentreString("Touch to Start", 160, 180, 2);
}

void drawCountdown() {
    M5.Display.fillScreen(TFT_BLACK);
    M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
    if (countdownValue > 0) M5.Display.drawCentreString(String(countdownValue), 160, 80, 7);
    else                    M5.Display.drawCentreString("GO!", 160, 80, 7);
}

void drawRoundResult() {
    M5.Display.fillScreen(TFT_BLACK);
    M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
    M5.Display.drawCentreString("Round Over!", 160, 60, 3);
    M5.Display.setCursor(20, 130);
    M5.Display.printf("You: %d  Opp: %d", playerWins, opponentWins);
    countdownTimer = millis();
}

void drawFinalResult() {
    M5.Display.fillScreen(TFT_BLACK);
    M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
    M5.Display.drawCentreString("GAME OVER", 160, 50, 3);
    if (playerWins >= 7) M5.Display.drawCentreString("YOU WIN!",  160, 110, 4);
    else                 M5.Display.drawCentreString("YOU LOSE",  160, 110, 4);
}

void drawUploadScore() {
    M5.Display.fillScreen(TFT_BLACK);
    M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
    M5.Display.drawCentreString("Uploading score...", 160, 100, 2);
}

void drawBackToMenu() {
    M5.Display.fillScreen(TFT_BLACK);
    M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
    M5.Display.drawCentreString("Returning to menu...", 160, 100, 2);
}

// =============================================================================
// REMAINING TOUCH HANDLERS
// =============================================================================

void handleStartScreenTouch() {
    auto touch = M5.Touch.getDetail();
    if (touch.state == m5::touch_state_t::touch_begin) changeState(CHARACTER_SELECT);
}

void handleRaceTouch() {
    auto touch = M5.Touch.getDetail();
    if (touch.state == m5::touch_state_t::touch_begin) {
        // TODO: item button region check
    }
}

// =============================================================================
// BLE / WiFi / GCP STUBS
// =============================================================================
void initBLE()          { /* TODO */ }
void stopBLE()          { /* TODO */ }
void initWiFi()         { /* TODO */ }
void uploadScoreToGCP() { changeState(BACK_TO_MENU); }
void syncRaceStateBLE() { /* TODO */ }