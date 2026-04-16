#include <M5Unified.h>
#include <NimBLEDevice.h>
#include <Wire.h>
#include <Adafruit_seesaw.h>
#include <Tween.h>
#include <math.h>

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
// ─── Track Point Data ────────────────────────────────────────────────────────
struct TrackPt {
    int x;
    int y;
};

// ── OVAL: 20-point ellipse  cx=640,cy=480, rx=380,ry=240 ─────────────────────
// Points computed as: x = 640 + 380cos(i*2π/20), y = 480 + 240sin(i*2π/20)
const TrackPt OVAL_PTS[20] = {
    {1020,480},{1002,554},{948,622},{864,674},{758,708},
    {640,720},{522,708},{416,674},{332,622},{278,554},
    {260,480},{278,406},{332,338},{416,286},{522,252},
    {640,240},{758,252},{864,286},{948,338},{1002,406}
};
const int OVAL_COUNT = 20;
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

const Map maps[4] = {
    { "OVAL",         "EASY",   TFT_GREEN,  TFT_GREEN,     240, 480 },
    { "CITY CIRCUIT", "MEDIUM", TFT_YELLOW, TFT_DARKGREEN, 470, 340 },
    { "RAINBOW ROAD", "HARD",   TFT_RED,    TFT_BLACK,     130, 220 },
    { "POINT OVAL",   "EASY",   TFT_GREEN,  TFT_DARKGREEN, 420, 480 }
};
const int NUM_MAPS = 4;

// ─── World Boundaries (for collision) ─────────────────────────────────────────
const float WORLD_WIDTH = 1280.0f;
const float WORLD_HEIGHT = 960.0f;
const float WALL_MARGIN = 30.0f;  // Distance to keep vehicle from edge

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
unsigned long lastRaceUpdateMs = 0;
unsigned long raceStartMs = 0;
unsigned long raceElapsedMs = 0;
bool finalResultInputUnlocked = false;
bool devComboLatched = false;

struct LeaderboardEntry {
    char playerName[16];
    char mapName[24];
    uint8_t lapsCompleted;
    unsigned long timeMs;
    uint8_t vehicleIndex;
};

LeaderboardEntry latestRaceResult = { "YOU", "", 0, 0, 0 };
bool leaderboardSyncQueued = false;

Adafruit_seesaw gamepad;
bool gamepadConnected = false;

const uint8_t GAMEPAD_I2C_ADDR = 0x50;
const uint8_t BUTTON_X = 6;
const uint8_t BUTTON_Y = 2;
const uint8_t BUTTON_A = 5;
const uint8_t BUTTON_B = 1;
const uint8_t JOYSTICK_X_PIN = 14;
const int JOYSTICK_CENTER = 512;
const int JOYSTICK_DEADZONE = 90;
const uint32_t GAMEPAD_BUTTON_MASK =
    (1UL << BUTTON_X) |
    (1UL << BUTTON_Y) |
    (1UL << BUTTON_A) |
    (1UL << BUTTON_B);

// ─── Player World Position (camera anchor) ────────────────────────────────────
float playerWorldX = 320.0f;
float playerWorldY =  85.0f;
float playerHeadingRad = 0.0f;

const int TOTAL_LAPS = 3;
const int NUM_CHECKPOINTS = 4;
int currentLap = 1;
const int CHECKPOINT_SEQUENCE[8] = { 0, 1, 2, 3, 3, 2, 1, 0 };
int checkpointSequencePos = 0;
bool wasInCheckpoint[NUM_CHECKPOINTS] = { false, false, false, false };

Tween::Timeline controlTween;
float smoothedSteer = 0.0f;
float smoothedThrottle = 0.0f;
float targetSteer = 0.0f;

// ─── Sprite Rendering (Double Buffering for Smooth Animation) ─────────────────
M5Canvas frameSprite(&M5.Display);
bool useSpriteRenderer = false;

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
void updateRaceMotion();
float getVehicleSpeedPxPerSecond();
void initGamepadQT();
void readGamepadInput(float& steer, float& throttleScale, bool& driftOn);
void retargetControlTween(float steer);
void handleDevCombo();
bool isInCheckpointZone(int mapIdx, int checkpointIdx, float x, float y);
void updateLapProgress();
void captureRaceResultForLeaderboard();
void formatRaceTime(unsigned long elapsedMs, char* buffer, size_t bufferSize);

void initBLE();
void stopBLE();
void initWiFi();
void uploadScoreToGCP();
void syncLeaderboardToGCP();
void syncRaceStateBLE();

// ─── Setup ────────────────────────────────────────────────────────────────────
void setup() {
    auto cfg = M5.config();
    M5.begin(cfg);
    M5.Display.setTextSize(2);
    M5.Display.fillScreen(TFT_BLACK);
    Serial.begin(115200);
    initGamepadQT();
    
    // Initialize sprite renderer for smooth animation
    frameSprite.setColorDepth(16);
    useSpriteRenderer = (frameSprite.createSprite(320, 240) != nullptr);
    
    changeState(START_SCREEN);
}

// ─── Frame Rate Limiter ───────────────────────────────────────────────────────
static unsigned long lastFrameMs = 0;
const unsigned long FRAME_TIME_MS = 16;  // ~60 FPS (1000/60 ≈ 16.67ms)

void limitFrameRate() {
    unsigned long now = millis();
    unsigned long elapsed = now - lastFrameMs;
    if (elapsed < FRAME_TIME_MS) {
        delayMicroseconds((FRAME_TIME_MS - elapsed) * 1000);
    }
    lastFrameMs = millis();
}

// ─── Main Loop ────────────────────────────────────────────────────────────────
void loop() {
    M5.update();
    if (currentState != previousState) {
        screenNeedsRedraw = true;
        previousState = currentState;
    }
    handleCurrentState();
    limitFrameRate();
}

// ─── State Machine ────────────────────────────────────────────────────────────
void changeState(GameState next) {
    Serial.printf("State: %d -> %d\n", currentState, next);
    currentState = next;
    screenNeedsRedraw = true;
    if (next == FINAL_RESULT) {
        finalResultInputUnlocked = false;
        devComboLatched = false;
    }
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
            handleDevCombo();
            updateRaceMotion();
            if (currentLap > TOTAL_LAPS) {
                captureRaceResultForLeaderboard();
                changeState(FINAL_RESULT);
                break;
            }
            syncRaceStateBLE();
            handleRaceTouch();
            // Always redraw during race for smooth animation
            drawRace();
            screenNeedsRedraw = false;
            raceNeedsRedraw = false;
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

        case FINAL_RESULT: {
            if (screenNeedsRedraw) {
                drawFinalResult();
                syncLeaderboardToGCP();
                screenNeedsRedraw = false;
            }
            if (!finalResultInputUnlocked) {
                bool physicalButtonsClear = !M5.BtnA.isPressed() && !M5.BtnC.isPressed();
                bool gamepadButtonsClear = true;
                if (gamepadConnected) {
                    uint32_t buttons = gamepad.digitalReadBulk(GAMEPAD_BUTTON_MASK);
                    gamepadButtonsClear = (buttons == GAMEPAD_BUTTON_MASK);
                }
                if (physicalButtonsClear && gamepadButtonsClear) {
                    finalResultInputUnlocked = true;
                }
                break;
            }

            bool playAgainPressed = M5.BtnA.wasClicked();
            bool menuPressed = M5.BtnC.wasClicked();
            if (gamepadConnected) {
                uint32_t buttons = gamepad.digitalReadBulk(GAMEPAD_BUTTON_MASK);
                playAgainPressed = playAgainPressed || !(buttons & (1UL << BUTTON_A));
                menuPressed = menuPressed || !(buttons & (1UL << BUTTON_Y));
            }

            if (playAgainPressed) {
                playerWins = 0;
                opponentWins = 0;
                changeState(COUNTDOWN);
            } else if (menuPressed) {
                playerWins = 0;
                opponentWins = 0;
                changeState(BACK_TO_MENU);
            }
            break;
        }

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
// World objects are offset relative to the player and then slightly zoomed in
// so the map fills more of the screen while the camera still follows the car.
// The world is rotated by the inverse heading so turning rotates the map view.
// =============================================================================

const float CAMERA_ZOOM = 1.18f;
const float CAMERA_BEHIND_DIST = 60.0f;  // Distance behind vehicle for camera

void worldToScreen(float wx, float wy, int& sx, int& sy) {
    // Camera follows from behind the vehicle
    float cos_h = cosf(playerHeadingRad);
    float sin_h = sinf(playerHeadingRad);
    
    // Camera offset: behind the vehicle (opposite direction of travel)
    float camX = playerWorldX - sin_h * CAMERA_BEHIND_DIST;
    float camY = playerWorldY + cos_h * CAMERA_BEHIND_DIST;
    
    float dx = wx - camX;
    float dy = wy - camY;

    sx = 160 + (int)(dx * CAMERA_ZOOM);
    sy = 120 + (int)(dy * CAMERA_ZOOM);
}

// Draw a world-space filled rect, automatically offset by camera
void wRect(int wx, int wy, int ww, int wh, uint16_t col) {
    int sx = 0;
    int sy = 0;
    worldToScreen((float)wx, (float)wy, sx, sy);
    auto& disp = useSpriteRenderer ? (M5GFX&)frameSprite : (M5GFX&)M5.Display;
    disp.fillRect(sx, sy, max(1, (int)(ww * CAMERA_ZOOM)), max(1, (int)(wh * CAMERA_ZOOM)), col);
}

// Draw a world-space filled rounded rect, offset by camera
void wRRect(int wx, int wy, int ww, int wh, int r, uint16_t col) {
    int sx = 0;
    int sy = 0;
    worldToScreen((float)wx, (float)wy, sx, sy);
    auto& disp = useSpriteRenderer ? (M5GFX&)frameSprite : (M5GFX&)M5.Display;
    disp.fillRoundRect(sx, sy, max(1, (int)(ww * CAMERA_ZOOM)), max(1, (int)(wh * CAMERA_ZOOM)), max(1, (int)(r * CAMERA_ZOOM)), col);
}

// Draw a world-space line, offset by camera
void wLine(int wx1, int wy1, int wx2, int wy2, uint16_t col) {
    int sx1 = 0;
    int sy1 = 0;
    int sx2 = 0;
    int sy2 = 0;
    worldToScreen((float)wx1, (float)wy1, sx1, sy1);
    worldToScreen((float)wx2, (float)wy2, sx2, sy2);
    auto& disp = useSpriteRenderer ? (M5GFX&)frameSprite : (M5GFX&)M5.Display;
    disp.drawLine(sx1, sy1, sx2, sy2, col);
}

// Draw a world-space filled ellipse, offset by camera
void wFEllipse(int wx, int wy, int rx, int ry, uint16_t col) {
    int sx = 0;
    int sy = 0;
    worldToScreen((float)wx, (float)wy, sx, sy);
    auto& disp = useSpriteRenderer ? (M5GFX&)frameSprite : (M5GFX&)M5.Display;
    disp.fillEllipse(sx, sy, max(1, (int)(rx * CAMERA_ZOOM)), max(1, (int)(ry * CAMERA_ZOOM)), col);
}

// Draw a world-space ellipse outline, offset by camera
void wEllipse(int wx, int wy, int rx, int ry, uint16_t col) {
    int sx = 0;
    int sy = 0;
    worldToScreen((float)wx, (float)wy, sx, sy);
    auto& disp = useSpriteRenderer ? (M5GFX&)frameSprite : (M5GFX&)M5.Display;
    disp.drawEllipse(sx, sy, max(1, (int)(rx * CAMERA_ZOOM)), max(1, (int)(ry * CAMERA_ZOOM)), col);
}

// =============================================================================
// TRACK WORLD GEOMETRY
// World space = 640 x 480.  Tracks are drawn with wRect/wRRect so the camera
// automatically shows the window around the player.
// =============================================================================

void drawTrackWorld(int mapIdx) {
    switch (mapIdx) {

        // ── OVAL ── simple oval ring, track width ~80px (scaled 2x for 1280x960 world) ──
        case 0:
            // Build a clean oval ring with ellipses to avoid round-rect artifacts.
            wFEllipse(640, 480, 500, 340, TFT_DARKGREY);
            wFEllipse(640, 480, 340, 180, TFT_GREEN);
            wEllipse(640, 480, 500, 340, TFT_WHITE);
            wEllipse(640, 480, 340, 180, TFT_WHITE);

            // Four vertical checkpoint guides spanning the drivable grey ring.
            wRect(312, 180, 16, 160, TFT_CYAN);
            wRect(312, 620, 16, 160, TFT_CYAN);
            wRect(472, 180, 16, 160, TFT_CYAN);
            wRect(472, 620, 16, 160, TFT_CYAN);
            wRect(792, 180, 16, 160, TFT_CYAN);
            wRect(792, 620, 16, 160, TFT_CYAN);
            wRect(952, 180, 16, 160, TFT_CYAN);
            wRect(952, 620, 16, 160, TFT_CYAN);
            break;

        // ── CITY CIRCUIT ── cross-shaped road with centre lines ───────────────
        case 1:
            // Horizontal road
            wRect(120, 280, 1040, 160, TFT_DARKGREY);
            // Vertical road
            wRect(520, 120, 160, 720, TFT_DARKGREY);
            // Intersection box (slightly lighter)
            wRect(520, 280, 160, 160, 0x6B4D);
            // Pavements (lighter strips along road edges)
            wRect(120, 272, 1040, 16, 0x9CD3);
            wRect(120, 432, 1040, 16, 0x9CD3);
            wRect(512, 120, 16, 720, 0x9CD3);
            wRect(672, 120, 16, 720, 0x9CD3);

            // Four vertical checkpoint guides spanning top-to-bottom grey road.
            wRect(540, 120, 16, 720, TFT_CYAN);
            wRect(572, 120, 16, 720, TFT_CYAN);
            wRect(612, 120, 16, 720, TFT_CYAN);
            wRect(644, 120, 16, 720, TFT_CYAN);
            break;

        // ── RAINBOW ROAD ── colourful rectangular loop ────────────────────────
        case 2: {
            uint16_t c0 = M5.Display.color565( 80, 80, 220); // blue  – top
            uint16_t c1 = M5.Display.color565(220, 80, 80);  // red   – right
            uint16_t c2 = M5.Display.color565( 80,200, 80);  // green – bottom
            uint16_t c3 = M5.Display.color565(220,180, 40);  // amber – left
            // Four coloured road segments forming a loop (scaled 2x)
            wRect(120, 150, 1040, 150, c0); // top
            wRect(1010, 150, 150, 660, c1); // right
            wRect(120, 660, 1040, 150, c2); // bottom
            wRect(120, 150, 150, 660, c3); // left
            // Black interior cutout
            wRect(270, 300, 740, 360, TFT_BLACK);

            // Four vertical checkpoint guides spanning top-to-bottom road sections.
            wRect(182, 150, 16, 660, TFT_CYAN);
            wRect(402, 150, 16, 150, TFT_CYAN);
            wRect(402, 660, 16, 150, TFT_CYAN);
            wRect(857, 150, 16, 150, TFT_CYAN);
            wRect(857, 660, 16, 150, TFT_CYAN);
            wRect(1077, 150, 16, 660, TFT_CYAN);
            break;
        }

        // ── POINT OVAL ── 20-point ellipse track ────────────────────────────────
        case 3:
            // Draw track segments connecting the 20 points
            for (int i = 0; i < OVAL_COUNT; i++) {
                int next = (i + 1) % OVAL_COUNT;
                int x1 = OVAL_PTS[i].x;
                int y1 = OVAL_PTS[i].y;
                int x2 = OVAL_PTS[next].x;
                int y2 = OVAL_PTS[next].y;
                wLine(x1, y1, x2, y2, TFT_DARKGREY);
                wLine(x1, y1, x2, y2, TFT_DARKGREY);
            }
            // Draw inner track line for lane guidance
            for (int i = 0; i < OVAL_COUNT; i++) {
                int next = (i + 1) % OVAL_COUNT;
                int x1 = OVAL_PTS[i].x;
                int y1 = OVAL_PTS[i].y;
                int x2 = OVAL_PTS[next].x;
                int y2 = OVAL_PTS[next].y;
                wLine(x1, y1, x2, y2, TFT_WHITE);
            }

            // Four vertical checkpoint guides spanning top-to-bottom of the oval area.
            wRect(468, 240, 16, 120, TFT_CYAN);
            wRect(468, 600, 16, 120, TFT_CYAN);
            wRect(548, 240, 16, 120, TFT_CYAN);
            wRect(548, 600, 16, 120, TFT_CYAN);
            wRect(708, 240, 16, 120, TFT_CYAN);
            wRect(708, 600, 16, 120, TFT_CYAN);
            wRect(788, 240, 16, 120, TFT_CYAN);
            wRect(788, 600, 16, 120, TFT_CYAN);
            break;
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
    playerHeadingRad = 1.5707963f;
    currentLap = 1;
    checkpointSequencePos = 0;
    for (int i = 0; i < NUM_CHECKPOINTS; i++) {
        wasInCheckpoint[i] = false;
    }
    lastRaceUpdateMs = 0;
    raceStartMs = millis();
    raceElapsedMs = 0;
    raceNeedsRedraw = true;
}

void formatRaceTime(unsigned long elapsedMs, char* buffer, size_t bufferSize) {
    unsigned long totalTenths = elapsedMs / 100;
    unsigned long minutes = totalTenths / 600;
    unsigned long seconds = (totalTenths / 10) % 60;
    unsigned long tenths = totalTenths % 10;
    snprintf(buffer, bufferSize, "%lu:%02lu.%lu", minutes, seconds, tenths);
}

void captureRaceResultForLeaderboard() {
    latestRaceResult.lapsCompleted = (currentLap > TOTAL_LAPS) ? TOTAL_LAPS : (uint8_t)currentLap;
    latestRaceResult.timeMs = raceElapsedMs;
    latestRaceResult.vehicleIndex = (uint8_t)selectedCharacter;
    strncpy(latestRaceResult.playerName, "YOU", sizeof(latestRaceResult.playerName) - 1);
    latestRaceResult.playerName[sizeof(latestRaceResult.playerName) - 1] = '\0';
    strncpy(latestRaceResult.mapName, maps[selectedMap].name, sizeof(latestRaceResult.mapName) - 1);
    latestRaceResult.mapName[sizeof(latestRaceResult.mapName) - 1] = '\0';
    leaderboardSyncQueued = true;
}

bool inVerticalGateSpan(float x, float y, float gateX, float gateWidth, float topY1, float topY2, float bottomY1, float bottomY2) {
    bool inX = x >= gateX && x <= gateX + gateWidth;
    bool inTop = y >= topY1 && y <= topY2;
    bool inBottom = y >= bottomY1 && y <= bottomY2;
    return inX && (inTop || inBottom);
}

bool isInCheckpointZone(int mapIdx, int checkpointIdx, float x, float y) {
    float zx = 0.0f;
    float zy = 0.0f;
    float zw = 0.0f;
    float zh = 0.0f;

    switch (mapIdx) {
        case 0: // OVAL
            if      (checkpointIdx == 0) return inVerticalGateSpan(x, y, 312.0f, 16.0f, 180.0f, 340.0f, 620.0f, 780.0f);
            else if (checkpointIdx == 1) return inVerticalGateSpan(x, y, 472.0f, 16.0f, 180.0f, 340.0f, 620.0f, 780.0f);
            else if (checkpointIdx == 2) return inVerticalGateSpan(x, y, 792.0f, 16.0f, 180.0f, 340.0f, 620.0f, 780.0f);
            else                         return inVerticalGateSpan(x, y, 952.0f, 16.0f, 180.0f, 340.0f, 620.0f, 780.0f);
            break;
        case 1: // CITY CIRCUIT
            if      (checkpointIdx == 0) { zx = 540.0f; zy = 120.0f; zw = 16.0f; zh = 720.0f; }
            else if (checkpointIdx == 1) { zx = 572.0f; zy = 120.0f; zw = 16.0f; zh = 720.0f; }
            else if (checkpointIdx == 2) { zx = 612.0f; zy = 120.0f; zw = 16.0f; zh = 720.0f; }
            else                         { zx = 644.0f; zy = 120.0f; zw = 16.0f; zh = 720.0f; }
            break;
        case 2: // RAINBOW ROAD
            if      (checkpointIdx == 0) return inVerticalGateSpan(x, y, 182.0f, 16.0f, 150.0f, 810.0f, 0.0f, 0.0f);
            else if (checkpointIdx == 1) return inVerticalGateSpan(x, y, 402.0f, 16.0f, 150.0f, 300.0f, 660.0f, 810.0f);
            else if (checkpointIdx == 2) return inVerticalGateSpan(x, y, 857.0f, 16.0f, 150.0f, 300.0f, 660.0f, 810.0f);
            else                         return inVerticalGateSpan(x, y, 1077.0f, 16.0f, 150.0f, 810.0f, 0.0f, 0.0f);
            break;
        case 3: // POINT OVAL
        default:
            if      (checkpointIdx == 0) return inVerticalGateSpan(x, y, 468.0f, 16.0f, 240.0f, 360.0f, 600.0f, 720.0f);
            else if (checkpointIdx == 1) return inVerticalGateSpan(x, y, 548.0f, 16.0f, 240.0f, 360.0f, 600.0f, 720.0f);
            else if (checkpointIdx == 2) return inVerticalGateSpan(x, y, 708.0f, 16.0f, 240.0f, 360.0f, 600.0f, 720.0f);
            else                         return inVerticalGateSpan(x, y, 788.0f, 16.0f, 240.0f, 360.0f, 600.0f, 720.0f);
            break;
    }

    return (x >= zx && x <= zx + zw && y >= zy && y <= zy + zh);
}

void updateLapProgress() {
    for (int i = 0; i < NUM_CHECKPOINTS; i++) {
        bool inGate = isInCheckpointZone(selectedMap, i, playerWorldX, playerWorldY);

        if (i == CHECKPOINT_SEQUENCE[checkpointSequencePos] && inGate && !wasInCheckpoint[i]) {
            checkpointSequencePos++;
            if (checkpointSequencePos >= 8) {
                currentLap++;
                checkpointSequencePos = 0;
            }
        }

        wasInCheckpoint[i] = inGate;
    }
}

void initGamepadQT() {
    gamepadConnected = gamepad.begin(GAMEPAD_I2C_ADDR);
    if (!gamepadConnected) {
        Serial.println("Gamepad QT not detected on I2C");
        return;
    }

    gamepad.pinModeBulk(GAMEPAD_BUTTON_MASK, INPUT_PULLUP);
    controlTween.clear();
    // Only smooth steering input; throttle is immediate for responsive acceleration control
    controlTween.add(smoothedSteer).init(0.0f);
    controlTween.start();
    Serial.println("Gamepad QT connected");
}

void retargetControlTween(float steer) {
    targetSteer = steer;

    controlTween.clear();
    controlTween.add(smoothedSteer)
        .init(smoothedSteer)
        .then<Ease::Sine>(targetSteer, 85);
    controlTween.start();
}

void readGamepadInput(float& steer, float& throttleScale, bool& driftOn) {
    steer = 0.0f;
    throttleScale = 0.0f;
    driftOn = false;

    if (!gamepadConnected) return;

    uint32_t buttons = gamepad.digitalReadBulk(GAMEPAD_BUTTON_MASK);
    bool boostPressed = !(buttons & (1UL << BUTTON_X));
    bool acceleratePressed = !(buttons & (1UL << BUTTON_A));
    driftOn = !(buttons & (1UL << BUTTON_B));

    int joyX = (int)gamepad.analogRead(JOYSTICK_X_PIN);
    int delta = joyX - JOYSTICK_CENTER;
    if (abs(delta) > JOYSTICK_DEADZONE) {
        steer = (float)delta / (float)(JOYSTICK_CENTER - JOYSTICK_DEADZONE);
        if (steer > 1.0f) steer = 1.0f;
        if (steer < -1.0f) steer = -1.0f;
    }

    if (acceleratePressed) throttleScale = 1.0f;
    if (boostPressed) throttleScale += 0.45f;
}

float getVehicleSpeedPxPerSecond() {
    const Vehicle& vehicle = vehicles[selectedCharacter];
    return 42.0f + (vehicle.accel * 18.0f) + (vehicle.handling * 4.0f);
}

void updateRaceMotion() {
    unsigned long now = millis();
    if (lastRaceUpdateMs == 0) {
        lastRaceUpdateMs = now;
        return;
    }

    float elapsedSeconds = (now - lastRaceUpdateMs) / 1000.0f;
    lastRaceUpdateMs = now;

    float steer = 0.0f;
    float throttleScale = 0.0f;
    bool driftOn = false;
    readGamepadInput(steer, throttleScale, driftOn);

    // Smooth steering input only; throttle is immediate
    if (fabsf(steer - targetSteer) > 0.02f) {
        retargetControlTween(steer);
    }
    controlTween.update();
    
    // Apply throttle directly without smoothing for immediate response
    smoothedThrottle = throttleScale;

    if (raceStartMs != 0) {
        raceElapsedMs = millis() - raceStartMs;
    }

    const Vehicle& vehicle = vehicles[selectedCharacter];
    
    // Only apply steering when accelerating (throttle > 0)
    // This prevents sideways drift and ensures pure pivot rotation
    if (smoothedThrottle > 0.0f) {
        float turnRate = 1.50f + (vehicle.turning * 0.50f);
        if (driftOn) turnRate *= 1.25f;
        playerHeadingRad += smoothedSteer * turnRate * elapsedSeconds;
    }

    const float PI_F = 3.1415926f;
    while (playerHeadingRad > PI_F)  playerHeadingRad -= 2.0f * PI_F;
    while (playerHeadingRad < -PI_F) playerHeadingRad += 2.0f * PI_F;

    float distance = getVehicleSpeedPxPerSecond() * smoothedThrottle * elapsedSeconds;
    if (driftOn) distance *= 0.8f;
    playerWorldX += sinf(playerHeadingRad) * distance;
    playerWorldY -= cosf(playerHeadingRad) * distance;

    // Wall collision: clamp to boundaries
    if (playerWorldX < WALL_MARGIN) playerWorldX = WALL_MARGIN;
    if (playerWorldX > WORLD_WIDTH - WALL_MARGIN) playerWorldX = WORLD_WIDTH - WALL_MARGIN;
    if (playerWorldY < WALL_MARGIN) playerWorldY = WALL_MARGIN;
    if (playerWorldY > WORLD_HEIGHT - WALL_MARGIN) playerWorldY = WORLD_HEIGHT - WALL_MARGIN;

    updateLapProgress();

    raceNeedsRedraw = true;
}

void handleDevCombo() {
    if (currentState != RACE) return;
    if (!M5.BtnC.isPressed()) {
        devComboLatched = false;
        return;
    }
    if (devComboLatched) return;

    devComboLatched = true;
    currentLap = TOTAL_LAPS;
    checkpointSequencePos = 0;
    for (int i = 0; i < NUM_CHECKPOINTS; i++) {
        wasInCheckpoint[i] = false;
    }
    updateLapProgress();
}

void drawRaceHUD() {
    auto& disp = useSpriteRenderer ? (M5GFX&)frameSprite : (M5GFX&)M5.Display;
    
    // Small black bar top-left: lap counter
    disp.fillRect(0, 0, 80, 18, TFT_BLACK);
    disp.setTextColor(TFT_WHITE, TFT_BLACK);
    char lapBuf[16];
    snprintf(lapBuf, sizeof(lapBuf), "LAP %d/%d", currentLap, TOTAL_LAPS);
    disp.drawString(lapBuf, 4, 2, 1);

    // Race timer top-right
    disp.fillRect(240, 0, 80, 18, TFT_BLACK);
    char timerBuf[16];
    formatRaceTime(raceElapsedMs, timerBuf, sizeof(timerBuf));
    disp.drawString(timerBuf, 244, 2, 1);

    // Vehicle name badge bottom-left
    disp.fillRect(0, 224, 90, 16, TFT_BLACK);
    disp.setTextColor(vehicles[selectedCharacter].color, TFT_BLACK);
    disp.drawString(vehicles[selectedCharacter].name, 4, 225, 1);
}

void drawRace() {
    // Render to sprite if available for smooth double-buffering, otherwise direct to display
    if (useSpriteRenderer) {
        // 1. Fill screen with background colour for this map
        frameSprite.fillScreen(maps[selectedMap].bgColor);

        // 2. Draw all track geometry offset by camera — world scrolls, player stays centred
        drawTrackWorld(selectedMap);

        // 3. Player vehicle is ALWAYS drawn at exact screen centre
        drawVehicle(selectedCharacter, 160, 120);

        // 4. HUD drawn last so it sits on top of everything
        drawRaceHUD();

        // Push sprite to display for atomic frame update
        frameSprite.pushSprite(0, 0);
    } else {
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
// VEHICLE DRAWING  (with rotation support during race)
// =============================================================================

void drawVehicle(int type, int cx, int cy) {
    // During race, draw rotated vehicle; otherwise use detailed sprite
    if (currentState == RACE) {
        auto& disp = useSpriteRenderer ? (M5GFX&)frameSprite : (M5GFX&)M5.Display;
        float cos_a = cosf(playerHeadingRad);
        float sin_a = sinf(playerHeadingRad);
        uint16_t col = vehicles[type].color;
        
        // Draw rotated triangle shape representing vehicle
        int x1 = cx + (int)(sin_a * 16);
        int y1 = cy - (int)(cos_a * 16);
        int x2 = cx - (int)(sin_a * 10) + (int)(cos_a * 12);
        int y2 = cy + (int)(cos_a * 10) + (int)(sin_a * 12);
        int x3 = cx - (int)(sin_a * 10) - (int)(cos_a * 12);
        int y3 = cy + (int)(cos_a * 10) - (int)(sin_a * 12);
        
        disp.fillTriangle(x1, y1, x2, y2, x3, y3, col);
        
        // Draw contrasting interior to show direction
        disp.fillCircle(cx, cy, 4, TFT_WHITE);
        
        // Draw yellow arrow in front showing acceleration direction
        // Draw yellow arrow showing steering direction (real-time joystick preview)
        // Arrow angle reflects where player is steering toward
        float arrowHeading = playerHeadingRad;
        if (smoothedSteer != 0.0f) {
            // Show immediate joystick steering input with arrow angle
            // Full stick deflection = ~45 degree preview
            arrowHeading += smoothedSteer * 0.785f;  // PI/4 radians
        }
        
        float arrow_cos = cosf(arrowHeading);
        float arrow_sin = sinf(arrowHeading);
        
        int arrowLen = 20;
        int arrowTipX = x1 + (int)(arrow_sin * arrowLen);
        int arrowTipY = y1 - (int)(arrow_cos * arrowLen);
        
        // Arrow shaft from front point to tip
        disp.drawLine(x1, y1, arrowTipX, arrowTipY, TFT_YELLOW);
        
        // Arrow head (small triangular chevron pointing forward)
        int arrowHeadSize = 4;
        int arrowHeadX1 = arrowTipX - (int)(arrow_sin * arrowHeadSize) - (int)(arrow_cos * arrowHeadSize);
        int arrowHeadY1 = arrowTipY + (int)(arrow_cos * arrowHeadSize) - (int)(arrow_sin * arrowHeadSize);
        int arrowHeadX2 = arrowTipX - (int)(arrow_sin * arrowHeadSize) + (int)(arrow_cos * arrowHeadSize);
        int arrowHeadY2 = arrowTipY + (int)(arrow_cos * arrowHeadSize) + (int)(arrow_sin * arrowHeadSize);
        
        disp.drawLine(arrowTipX, arrowTipY, arrowHeadX1, arrowHeadY1, TFT_YELLOW);
        disp.drawLine(arrowTipX, arrowTipY, arrowHeadX2, arrowHeadY2, TFT_YELLOW);
    } else {
        // Character select screen: draw detailed vehicle
        auto& disp = (useSpriteRenderer && currentState == RACE) ? (M5GFX&)frameSprite : (M5GFX&)M5.Display;
        
        switch (type) {
            case 0: drawKart (cx, cy, vehicles[0].color); break;
            case 1: drawMoto (cx, cy, vehicles[1].color); break;
            case 2: drawBuggy(cx, cy, vehicles[2].color); break;
        }
    }
}

void drawKart(int cx, int cy, uint16_t col) {
    // Only use sprite renderer during race; always use M5.Display for other screens
    auto& disp = (useSpriteRenderer && currentState == RACE) ? (M5GFX&)frameSprite : (M5GFX&)M5.Display;
    uint16_t wc = TFT_DARKGREY;
    disp.fillRoundRect(cx - 44, cy - 30, 14, 20, 3, wc);
    disp.fillRoundRect(cx + 30, cy - 30, 14, 20, 3, wc);
    disp.fillRoundRect(cx - 44, cy + 14, 14, 20, 3, wc);
    disp.fillRoundRect(cx + 30, cy + 14, 14, 20, 3, wc);
    disp.fillRect(cx - 30, cy + 30, 60,  5, wc);
    disp.fillRect(cx - 26, cy + 33,  8, 10, wc);
    disp.fillRect(cx + 18, cy + 33,  8, 10, wc);
    disp.fillRoundRect(cx - 30, cy - 24, 60, 52, 6, col);
    disp.fillRoundRect(cx - 14, cy - 16, 28, 28, 5, 0x2104);
    disp.fillTriangle(cx - 18, cy - 24, cx + 18, cy - 24, cx, cy - 36, col);
    disp.fillCircle(cx, cy - 4, 10, TFT_WHITE);
    disp.fillCircle(cx, cy - 4,  7, TFT_YELLOW);
    disp.fillRect(cx - 32, cy + 4, 4, 8, TFT_ORANGE);
    disp.fillRect(cx + 28, cy + 4, 4, 8, TFT_ORANGE);
}

void drawMoto(int cx, int cy, uint16_t col) {
    auto& disp = (useSpriteRenderer && currentState == RACE) ? (M5GFX&)frameSprite : (M5GFX&)M5.Display;
    uint16_t wc = TFT_DARKGREY, rim = 0x6B4D;
    disp.fillRoundRect(cx - 10, cy - 46, 20, 26, 5, wc);
    disp.fillRoundRect(cx - 10, cy + 22, 20, 26, 5, wc);
    disp.fillRoundRect(cx -  5, cy - 43, 10, 20, 3, rim);
    disp.fillRoundRect(cx -  5, cy + 25, 10, 20, 3, rim);
    disp.fillRoundRect(cx - 11, cy - 22, 22, 46, 4, col);
    disp.fillRect(cx - 24, cy - 18, 48,  5, 0xC618);
    disp.fillRect(cx - 24, cy - 26,  5,  8, 0xC618);
    disp.fillRect(cx + 19, cy - 26,  5,  8, 0xC618);
    disp.fillTriangle(cx - 8, cy - 22, cx + 8, cy - 22, cx, cy - 34, col);
    disp.drawLine(cx - 5, cy - 25, cx + 5, cy - 25, TFT_CYAN);
    disp.fillCircle(cx, cy - 8, 11, TFT_WHITE);
    disp.fillEllipse(cx, cy - 6,  7,  5, TFT_CYAN);
    disp.fillRect(cx + 11, cy + 6, 4, 18, TFT_ORANGE);
    disp.fillCircle(cx + 13, cy + 24, 3, TFT_YELLOW);
}

void drawBuggy(int cx, int cy, uint16_t col) {
    auto& disp = (useSpriteRenderer && currentState == RACE) ? (M5GFX&)frameSprite : (M5GFX&)M5.Display;
    uint16_t wc = 0x4208;
    disp.fillRoundRect(cx - 50, cy - 32, 18, 24, 4, wc);
    disp.fillRoundRect(cx + 32, cy - 32, 18, 24, 4, wc);
    disp.fillRoundRect(cx - 50, cy + 12, 18, 24, 4, wc);
    disp.fillRoundRect(cx + 32, cy + 12, 18, 24, 4, wc);
    int wx[4] = { cx-50, cx+32, cx-50, cx+32 };
    int wy[4] = { cy-32, cy-32, cy+12, cy+12 };
    for (int i = 0; i < 4; i++) {
        disp.drawLine(wx[i]+5, wy[i]+4, wx[i]+5,  wy[i]+20, TFT_DARKGREY);
        disp.drawLine(wx[i]+11,wy[i]+4, wx[i]+11, wy[i]+20, TFT_DARKGREY);
    }
    disp.drawLine(cx-28, cy-34, cx-28, cy-18, 0xC618);
    disp.drawLine(cx+28, cy-34, cx+28, cy-18, 0xC618);
    disp.drawLine(cx-28, cy-34, cx+28, cy-34, 0xC618);
    disp.fillRoundRect(cx - 32, cy - 26, 64, 56, 5, col);
    disp.fillRect(cx - 24, cy - 22, 48, 14, 0x03EF);
    disp.drawLine(cx - 24, cy - 22, cx + 24, cy - 22, TFT_CYAN);
    disp.fillCircle(cx, cy - 4, 10, TFT_WHITE);
    disp.fillEllipse(cx, cy - 2,  7,  4, 0x03EF);
    disp.fillRect(cx - 10, cy + 14,  8, 4, 0x2104);
    disp.fillRect(cx +  2, cy + 14,  8, 4, 0x2104);
    disp.fillRect(cx - 26, cy + 26, 52, 4, TFT_RED);
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
    M5.Display.setTextColor(TFT_CYAN, TFT_BLACK);
    M5.Display.drawCentreString("END GAME", 160, 14, 2);
    M5.Display.setTextColor(TFT_YELLOW, TFT_BLACK);
    M5.Display.drawCentreString("LEADERBOARD", 160, 36, 2);

    M5.Display.drawRect(22, 64, 276, 120, TFT_DARKGREY);
    M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
    M5.Display.drawString("1. YOU", 34, 78, 2);
    M5.Display.drawString("2. CPU_A", 34, 108, 2);
    M5.Display.drawString("3. CPU_B", 34, 138, 2);

    M5.Display.setTextColor(TFT_GREEN, TFT_BLACK);
    M5.Display.drawString("3/3 LAPS", 208, 78, 2);
    M5.Display.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    M5.Display.drawString("2/3 LAPS", 208, 108, 2);
    M5.Display.drawString("1/3 LAPS", 208, 138, 2);

    M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
    M5.Display.drawCentreString("BtnA: Play Again", 160, 196, 1);
    M5.Display.drawCentreString("BtnC (Y): Menu", 160, 208, 1);
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
void syncLeaderboardToGCP() {
    if (!leaderboardSyncQueued) return;

    // Groundwork for future cross-user sync:
    // - latestRaceResult contains the finished race snapshot
    // - this stub is where an HTTPS POST to GCP can serialize that snapshot
    // - keep the upload path separate so local gameplay stays offline-safe
    leaderboardSyncQueued = false;
}
void syncRaceStateBLE() { /* TODO */ }