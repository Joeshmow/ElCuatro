#include <M5Unified.h>
#include <NimBLEDevice.h>
#include <Wire.h>
#include <Adafruit_seesaw.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <math.h>

static const char* LEADERBOARD_PLAYER_NAME = "CBU";
static const char* WIFI_SSID = "CBU-LANCERS";
static const char* WIFI_PASSWORD = "L@ncerN@tion";
static const char* GCP_LEADERBOARD_URL = "https://leaderboardpusher-734706803358.europe-west1.run.app";
static const char* GCP_API_BEARER_TOKEN = "";
static const bool GCP_ALLOW_INSECURE_TLS = true;

static bool blePendingConnect = false;
static NimBLEAdvertisedDevice* blePendingDevice = nullptr;  

// ─── State Definition ────────────────────────────────────────────────────────
enum GameState {
    START_SCREEN,
    CHARACTER_SELECT,
    CONNECTION_WAIT,
    MAP_SELECT,
    COUNTDOWN,
    RACE,
    NAME_SELECT,
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

static const char* BLE_SERVICE_UUID = "62d15720-755e-42cf-ab5b-4f4e2d08dcb7";
static const char* BLE_RACE_CHAR_UUID = "95e6a553-9308-4ce2-b803-95f9e0388dd0";
static const uint8_t BLE_PACKET_VERSION = 1;
static const uint8_t BLE_ROLE_FLAG_HOST = 0x01;
static const uint8_t BLE_ROLE_FLAG_RACE_FINISHED = 0x02;

struct RaceStatePacket {
    uint8_t version;
    uint8_t roleFlags;
    uint8_t mapIndex;
    uint8_t vehicleIndex;
    uint8_t lap;
    uint8_t checkpointSeqPos;
    float worldX;
    float worldY;
    float headingRad;
    uint32_t elapsedMs;
    uint64_t nodeId;
};

NimBLEServer* bleServer = nullptr;
NimBLECharacteristic* bleRaceCharacteristic = nullptr;
NimBLEClient* bleClient = nullptr;
NimBLERemoteCharacteristic* bleRemoteRaceCharacteristic = nullptr;
bool bleInitialized = false;
bool bleServerPeerConnected = false;
bool bleClientPeerConnected = false;
unsigned long lastBleSyncMs = 0;
String bleLocalAddress = "";
uint64_t bleLocalNodeId = 0;
uint64_t blePeerNodeId = 0;
bool bleIsHost = true;
bool bleRoleResolved = false;
bool bleTransportHostMode = true;
bool bleRoleChosen = false;
unsigned long bleTransportModeStartedMs = 0;
unsigned long bleTransportRetryMs = 0;
unsigned long bleLastPeerSeenMs = 0;

#define BLE_FORCE_ROLE_CLIENT

#if defined(BLE_FORCE_ROLE_HOST)
static const int kForcedBleRole = 1;
#elif defined(BLE_FORCE_ROLE_CLIENT)
static const int kForcedBleRole = -1;
#else
static const int kForcedBleRole = 0;
#endif

float opponentWorldX = 0.0f;
float opponentWorldY = 0.0f;
float opponentHeadingRad = 0.0f;
uint8_t opponentVehicleIndex = 0;
uint8_t opponentMapIndex = 0;
uint8_t opponentLap = 1;
unsigned long opponentRaceElapsedMs = 0;
unsigned long opponentLastUpdateMs = 0;
bool opponentStateValid = false;

const int PLAYER_NAME_LENGTH = 3;
char currentPlayerName[PLAYER_NAME_LENGTH + 1] = "YOU";
char nameEditBuffer[PLAYER_NAME_LENGTH + 1] = "YOU";
int nameEditIndex = 0;

struct LeaderboardEntry {
    char playerName[16];
    char mapName[24];
    uint8_t lapsCompleted;
    unsigned long totalTimeMs;
    unsigned long lap1Ms;
    unsigned long lap2Ms;
    unsigned long lap3Ms;
    uint8_t vehicleIndex;
};

LeaderboardEntry latestRaceResult = { "YOU", "", 0, 0, 0, 0, 0, 0 };
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
const int MAX_CHECKPOINTS = 8;
const int CHECKPOINT_SEQUENCE_LEN = 4;
const int OVAL_CHECKPOINT_SEQUENCE_LEN = 8;
int currentLap = 1;
const int CHECKPOINT_SEQUENCE[CHECKPOINT_SEQUENCE_LEN] = { 0, 1, 2, 3 };
const int OVAL_CHECKPOINT_SEQUENCE[OVAL_CHECKPOINT_SEQUENCE_LEN] = { 0, 1, 2, 3, 4, 5, 6, 7 };
int checkpointSequencePos = 0;
bool wasInCheckpoint[MAX_CHECKPOINTS] = { false, false, false, false, false, false, false, false };
unsigned long lapSplitMs[TOTAL_LAPS] = { 0, 0, 0 };
unsigned long lapStartElapsedMs = 0;

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
void drawConnectionWait();
void drawMapSelect();
void drawCountdown();
void drawRace();
void drawNameSelect();
void drawRoundResult();
void drawFinalResult();
void drawUploadScore();
void drawBackToMenu();

void handleStartScreenTouch();
void handleCharacterSelectTouch();
void handleConnectionWaitTouch();
void handleMapSelectTouch();
void handleRaceTouch();
void handleNameSelectInput();

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
void drawActiveCheckpointGuide(int mapIdx);
void drawMapPreviewBox(int mapIdx, int bx, int by, int bw, int bh);
void drawOpponentVehicleWorld();
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
void updateBleAuthorityRole(uint64_t peerNodeId);
bool getBleLinkConnected();
bool getBleDataLinkReady();
void applyBleTransportMode(bool hostMode);
void refreshBleTransportMode();
void setBleRole(bool hostMode);
void captureRaceResultForLeaderboard();
void formatRaceTime(unsigned long elapsedMs, char* buffer, size_t bufferSize);

static uint16_t blend565(uint16_t fg, uint16_t bg, uint8_t mix) {
    uint8_t fgR = (fg >> 11) & 0x1F;
    uint8_t fgG = (fg >> 5) & 0x3F;
    uint8_t fgB = fg & 0x1F;

    uint8_t bgR = (bg >> 11) & 0x1F;
    uint8_t bgG = (bg >> 5) & 0x3F;
    uint8_t bgB = bg & 0x1F;

    uint8_t invMix = 255 - mix;
    uint8_t outR = (uint8_t)((fgR * mix + bgR * invMix) / 255);
    uint8_t outG = (uint8_t)((fgG * mix + bgG * invMix) / 255);
    uint8_t outB = (uint8_t)((fgB * mix + bgB * invMix) / 255);

    return (uint16_t)((outR << 11) | (outG << 5) | outB);
}

void initBLE();
void stopBLE();
void initWiFi();
void uploadScoreToGCP();
void syncLeaderboardToGCP();
void syncRaceStateBLE();
void handleIncomingRacePacket(const uint8_t* data, size_t len);
void attemptBleClientConnect(NimBLEAdvertisedDevice* device);

int charSetIndex(char c);

// ─── Setup ────────────────────────────────────────────────────────────────────
void setup() {
    auto cfg = M5.config();
    M5.begin(cfg);
    M5.Display.setTextSize(2);
    M5.Display.fillScreen(TFT_BLACK);
    Serial.begin(115200);
    initGamepadQT();
    initBLE();

    if (kForcedBleRole != 0) {
        setBleRole(kForcedBleRole > 0);
    }

    if (strlen(LEADERBOARD_PLAYER_NAME) > 0) {
        strncpy(currentPlayerName, LEADERBOARD_PLAYER_NAME, sizeof(currentPlayerName) - 1);
        currentPlayerName[sizeof(currentPlayerName) - 1] = '\0';
    }
    currentPlayerName[PLAYER_NAME_LENGTH] = '\0';

    for (int i = 0; i < PLAYER_NAME_LENGTH; i++) {
        if (currentPlayerName[i] == '\0') {
            currentPlayerName[i] = 'A';
        }
    }
    currentPlayerName[PLAYER_NAME_LENGTH] = '\0';
    
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
    refreshBleTransportMode();

     if (blePendingConnect && blePendingDevice != nullptr) {
        blePendingConnect = false;
        attemptBleClientConnect(blePendingDevice);
        blePendingDevice = nullptr;
    }

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
    } else if (next == NAME_SELECT) {
        strncpy(nameEditBuffer, currentPlayerName, sizeof(nameEditBuffer) - 1);
        nameEditBuffer[sizeof(nameEditBuffer) - 1] = '\0';
        nameEditBuffer[PLAYER_NAME_LENGTH] = '\0';
        for (int i = 0; i < PLAYER_NAME_LENGTH; i++) {
            if (nameEditBuffer[i] == '\0') {
                nameEditBuffer[i] = 'A';
            }
        }
        nameEditBuffer[PLAYER_NAME_LENGTH] = '\0';
        nameEditIndex = 0;
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

        case CONNECTION_WAIT:
            syncRaceStateBLE();
            if (screenNeedsRedraw) {
                drawConnectionWait();
                screenNeedsRedraw = false;
            }
            handleConnectionWaitTouch();
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

        case NAME_SELECT:
            if (screenNeedsRedraw) {
                drawNameSelect();
                screenNeedsRedraw = false;
            }
            handleNameSelectInput();
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
            bool editNamePressed = M5.BtnB.wasClicked();
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
            } else if (editNamePressed) {
                changeState(NAME_SELECT);
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
            if (leaderboardSyncQueued) {
                initWiFi();
                syncLeaderboardToGCP();
            }
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

void drawActiveCheckpointGuide(int mapIdx) {
    const bool splitOvalCheckpoints = (mapIdx == 0 || mapIdx == 3);
    const int sequenceLen = splitOvalCheckpoints ? OVAL_CHECKPOINT_SEQUENCE_LEN : CHECKPOINT_SEQUENCE_LEN;
    const int* sequence = splitOvalCheckpoints ? OVAL_CHECKPOINT_SEQUENCE : CHECKPOINT_SEQUENCE;

    int activeSequencePos = checkpointSequencePos;
    if (activeSequencePos < 0 || activeSequencePos >= sequenceLen) {
        activeSequencePos = 0;
    }
    int checkpointIdx = sequence[activeSequencePos];

    switch (mapIdx) {
        case 0: // OVAL
            if (checkpointIdx == 0) {
                wRect(312, 180, 16, 160, TFT_CYAN);
            } else if (checkpointIdx == 1) {
                wRect(472, 180, 16, 160, TFT_CYAN);
            } else if (checkpointIdx == 2) {
                wRect(792, 180, 16, 160, TFT_CYAN);
            } else if (checkpointIdx == 3) {
                wRect(952, 180, 16, 160, TFT_CYAN);
            } else if (checkpointIdx == 4) {
                wRect(952, 620, 16, 160, TFT_CYAN);
            } else if (checkpointIdx == 5) {
                wRect(792, 620, 16, 160, TFT_CYAN);
            } else if (checkpointIdx == 6) {
                wRect(472, 620, 16, 160, TFT_CYAN);
            } else {
                wRect(312, 620, 16, 160, TFT_CYAN);
            }
            break;

        case 1: // CITY CIRCUIT
            if (checkpointIdx == 0) {
                wRect(540, 120, 16, 720, TFT_CYAN);
            } else if (checkpointIdx == 1) {
                wRect(572, 120, 16, 720, TFT_CYAN);
            } else if (checkpointIdx == 2) {
                wRect(612, 120, 16, 720, TFT_CYAN);
            } else {
                wRect(644, 120, 16, 720, TFT_CYAN);
            }
            break;

        case 2: // RAINBOW ROAD
            if (checkpointIdx == 0) {
                wRect(182, 150, 16, 660, TFT_CYAN);
            } else if (checkpointIdx == 1) {
                wRect(402, 150, 16, 150, TFT_CYAN);
                wRect(402, 660, 16, 150, TFT_CYAN);
            } else if (checkpointIdx == 2) {
                wRect(857, 150, 16, 150, TFT_CYAN);
                wRect(857, 660, 16, 150, TFT_CYAN);
            } else {
                wRect(1077, 150, 16, 660, TFT_CYAN);
            }
            break;

        case 3: // POINT OVAL
        default:
            if (checkpointIdx == 0) {
                wRect(468, 240, 16, 120, TFT_CYAN);
            } else if (checkpointIdx == 1) {
                wRect(548, 240, 16, 120, TFT_CYAN);
            } else if (checkpointIdx == 2) {
                wRect(708, 240, 16, 120, TFT_CYAN);
            } else if (checkpointIdx == 3) {
                wRect(788, 240, 16, 120, TFT_CYAN);
            } else if (checkpointIdx == 4) {
                wRect(788, 600, 16, 120, TFT_CYAN);
            } else if (checkpointIdx == 5) {
                wRect(708, 600, 16, 120, TFT_CYAN);
            } else if (checkpointIdx == 6) {
                wRect(548, 600, 16, 120, TFT_CYAN);
            } else {
                wRect(468, 600, 16, 120, TFT_CYAN);
            }
            break;
    }
}

void drawTrackWorld(int mapIdx) {
    switch (mapIdx) {

        // ── OVAL ── simple oval ring, track width ~80px (scaled 2x for 1280x960 world) ──
        case 0:
            // Build a clean oval ring with ellipses to avoid round-rect artifacts.
            wFEllipse(640, 480, 500, 340, TFT_DARKGREY);
            wFEllipse(640, 480, 340, 180, TFT_GREEN);
            wEllipse(640, 480, 500, 340, TFT_WHITE);
            wEllipse(640, 480, 340, 180, TFT_WHITE);

            drawActiveCheckpointGuide(mapIdx);
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

            drawActiveCheckpointGuide(mapIdx);
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

            drawActiveCheckpointGuide(mapIdx);
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

            drawActiveCheckpointGuide(mapIdx);
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
    for (int i = 0; i < MAX_CHECKPOINTS; i++) {
        wasInCheckpoint[i] = false;
    }
    lastRaceUpdateMs = 0;
    raceStartMs = millis();
    raceElapsedMs = 0;
    lapStartElapsedMs = 0;
    for (int i = 0; i < TOTAL_LAPS; i++) {
        lapSplitMs[i] = 0;
    }
    opponentStateValid = false;
    opponentLastUpdateMs = 0;
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
    latestRaceResult.totalTimeMs = raceElapsedMs;
    latestRaceResult.lap1Ms = lapSplitMs[0];
    latestRaceResult.lap2Ms = lapSplitMs[1];
    latestRaceResult.lap3Ms = lapSplitMs[2];
    latestRaceResult.vehicleIndex = (uint8_t)selectedCharacter;
    if (strlen(currentPlayerName) > 0) {
        strncpy(latestRaceResult.playerName, currentPlayerName, sizeof(latestRaceResult.playerName) - 1);
    } else if (strlen(LEADERBOARD_PLAYER_NAME) > 0) {
        strncpy(latestRaceResult.playerName, LEADERBOARD_PLAYER_NAME, sizeof(latestRaceResult.playerName) - 1);
    } else {
        strncpy(latestRaceResult.playerName, "YOU", sizeof(latestRaceResult.playerName) - 1);
    }
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
            if      (checkpointIdx == 0) return (x >= 312.0f && x <= 328.0f && y >= 180.0f && y <= 340.0f);
            else if (checkpointIdx == 1) return (x >= 472.0f && x <= 488.0f && y >= 180.0f && y <= 340.0f);
            else if (checkpointIdx == 2) return (x >= 792.0f && x <= 808.0f && y >= 180.0f && y <= 340.0f);
            else if (checkpointIdx == 3) return (x >= 952.0f && x <= 968.0f && y >= 180.0f && y <= 340.0f);
            else if (checkpointIdx == 4) return (x >= 952.0f && x <= 968.0f && y >= 620.0f && y <= 780.0f);
            else if (checkpointIdx == 5) return (x >= 792.0f && x <= 808.0f && y >= 620.0f && y <= 780.0f);
            else if (checkpointIdx == 6) return (x >= 472.0f && x <= 488.0f && y >= 620.0f && y <= 780.0f);
            else                         return (x >= 312.0f && x <= 328.0f && y >= 620.0f && y <= 780.0f);
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
            if      (checkpointIdx == 0) return (x >= 468.0f && x <= 484.0f && y >= 240.0f && y <= 360.0f);
            else if (checkpointIdx == 1) return (x >= 548.0f && x <= 564.0f && y >= 240.0f && y <= 360.0f);
            else if (checkpointIdx == 2) return (x >= 708.0f && x <= 724.0f && y >= 240.0f && y <= 360.0f);
            else if (checkpointIdx == 3) return (x >= 788.0f && x <= 804.0f && y >= 240.0f && y <= 360.0f);
            else if (checkpointIdx == 4) return (x >= 788.0f && x <= 804.0f && y >= 600.0f && y <= 720.0f);
            else if (checkpointIdx == 5) return (x >= 708.0f && x <= 724.0f && y >= 600.0f && y <= 720.0f);
            else if (checkpointIdx == 6) return (x >= 548.0f && x <= 564.0f && y >= 600.0f && y <= 720.0f);
            else                         return (x >= 468.0f && x <= 484.0f && y >= 600.0f && y <= 720.0f);
            break;
    }

    return (x >= zx && x <= zx + zw && y >= zy && y <= zy + zh);
}

void updateLapProgress() {
    const bool splitOvalCheckpoints = (selectedMap == 0 || selectedMap == 3);
    const int sequenceLen = splitOvalCheckpoints ? OVAL_CHECKPOINT_SEQUENCE_LEN : CHECKPOINT_SEQUENCE_LEN;
    const int checkpointCount = splitOvalCheckpoints ? MAX_CHECKPOINTS : NUM_CHECKPOINTS;
    const int* sequence = splitOvalCheckpoints ? OVAL_CHECKPOINT_SEQUENCE : CHECKPOINT_SEQUENCE;

    for (int i = 0; i < checkpointCount; i++) {
        bool inGate = isInCheckpointZone(selectedMap, i, playerWorldX, playerWorldY);

        if (i == sequence[checkpointSequencePos] && inGate && !wasInCheckpoint[i]) {
            checkpointSequencePos++;
            if (checkpointSequencePos >= sequenceLen) {
                if (currentLap >= 1 && currentLap <= TOTAL_LAPS) {
                    lapSplitMs[currentLap - 1] = raceElapsedMs - lapStartElapsedMs;
                    lapStartElapsedMs = raceElapsedMs;
                }
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
    smoothedSteer = 0.0f;
    targetSteer = 0.0f;
    Serial.println("Gamepad QT connected");
}

void retargetControlTween(float steer) {
    targetSteer = steer;
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
    smoothedSteer += (targetSteer - smoothedSteer) * 0.22f;
    
    // Apply throttle directly without smoothing for immediate response
    smoothedThrottle = throttleScale;

    if (raceStartMs != 0 && (!bleConnected || !bleRoleResolved || bleIsHost)) {
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

    if (!bleConnected || !bleRoleResolved || bleIsHost) {
        updateLapProgress();
    }

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
    for (int i = 0; i < MAX_CHECKPOINTS; i++) {
        wasInCheckpoint[i] = false;
    }
    for (int i = 0; i < TOTAL_LAPS; i++) {
        lapSplitMs[i] = 0;
    }
    lapStartElapsedMs = raceElapsedMs;
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

    // BLE link and authority role status centered at the top.
    disp.fillRect(108, 0, 44, 18, TFT_BLACK);
    disp.setTextColor(bleConnected ? TFT_GREEN : TFT_RED, TFT_BLACK);
    const char* roleTag = bleIsHost ? "H" : (bleRoleResolved ? "C" : "?");
    char p2Buf[10];
    if (bleConnected) snprintf(p2Buf, sizeof(p2Buf), "P2 %s", roleTag);
    else snprintf(p2Buf, sizeof(p2Buf), "P2 OFF");
    disp.drawString(p2Buf, 110, 2, 1);

    // Vehicle name badge bottom-left
    disp.fillRect(0, 224, 90, 16, TFT_BLACK);
    disp.setTextColor(vehicles[selectedCharacter].color, TFT_BLACK);
    disp.drawString(vehicles[selectedCharacter].name, 4, 225, 1);

    // Always-on mini-map so both karts' movement is visible even when one is off-camera.
    const int miniX = 256;
    const int miniY = 204;
    const int miniW = 60;
    const int miniH = 34;
    disp.fillRect(miniX, miniY, miniW, miniH, TFT_BLACK);
    disp.drawRect(miniX, miniY, miniW, miniH, TFT_DARKGREY);

    auto worldToMiniX = [&](float wx) {
        float n = wx / (float)WORLD_WIDTH;
        if (n < 0.0f) n = 0.0f;
        if (n > 1.0f) n = 1.0f;
        return miniX + 2 + (int)(n * (float)(miniW - 5));
    };
    auto worldToMiniY = [&](float wy) {
        float n = wy / (float)WORLD_HEIGHT;
        if (n < 0.0f) n = 0.0f;
        if (n > 1.0f) n = 1.0f;
        return miniY + 2 + (int)(n * (float)(miniH - 5));
    };

    int selfMiniX = worldToMiniX(playerWorldX);
    int selfMiniY = worldToMiniY(playerWorldY);
    disp.fillCircle(selfMiniX, selfMiniY, 2, TFT_CYAN);

    const bool opponentFresh = opponentStateValid && (millis() - opponentLastUpdateMs <= 1500);
    if (opponentFresh) {
        int oppMiniX = worldToMiniX(opponentWorldX);
        int oppMiniY = worldToMiniY(opponentWorldY);
        disp.fillCircle(oppMiniX, oppMiniY, 2, TFT_YELLOW);
    }
}

void drawOpponentVehicleWorld() {
    if (!opponentStateValid) return;
    if (opponentMapIndex != (uint8_t)selectedMap) return;
    if (millis() - opponentLastUpdateMs > 1500) return;

    int sx = 0;
    int sy = 0;
    worldToScreen(opponentWorldX, opponentWorldY, sx, sy);
    if (sx < -30 || sx > 350 || sy < -30 || sy > 270) return;

    auto& disp = useSpriteRenderer ? (M5GFX&)frameSprite : (M5GFX&)M5.Display;
    float cos_a = cosf(opponentHeadingRad);
    float sin_a = sinf(opponentHeadingRad);
    uint16_t col = vehicles[opponentVehicleIndex % NUM_VEHICLES].color;
    uint16_t ghostCol = col;
    uint16_t edgeCol = col;

    int x1 = sx + (int)(sin_a * 14);
    int y1 = sy - (int)(cos_a * 14);
    int x2 = sx - (int)(sin_a * 9) + (int)(cos_a * 10);
    int y2 = sy + (int)(cos_a * 9) + (int)(sin_a * 10);
    int x3 = sx - (int)(sin_a * 9) - (int)(cos_a * 10);
    int y3 = sy + (int)(cos_a * 9) - (int)(sin_a * 10);

    disp.fillTriangle(x1, y1, x2, y2, x3, y3, ghostCol);
    disp.drawLine(x1, y1, x2, y2, edgeCol);
    disp.drawLine(x2, y2, x3, y3, edgeCol);
    disp.drawLine(x3, y3, x1, y1, edgeCol);
    disp.fillCircle(sx, sy, 2, edgeCol);
}

void drawRace() {
    // Render to sprite if available for smooth double-buffering, otherwise direct to display
    if (useSpriteRenderer) {
        // 1. Fill screen with background colour for this map
        frameSprite.fillScreen(maps[selectedMap].bgColor);

        // 2. Draw all track geometry offset by camera — world scrolls, player stays centred
        drawTrackWorld(selectedMap);

        // Remote player kart in world-space, if a peer is connected.
        drawOpponentVehicleWorld();

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

        // Remote player kart in world-space, if a peer is connected.
        drawOpponentVehicleWorld();

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
        changeState(CONNECTION_WAIT);
    }
}

void drawConnectionWait() {
    bool transportConnected = getBleLinkConnected();
    bool dataLinked = getBleDataLinkReady();

    M5.Display.fillScreen(TFT_BLACK);
    M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
    M5.Display.drawCentreString("PAIRING CHECK", 160, 34, 2);
    M5.Display.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    M5.Display.drawCentreString("Waiting for data link", 160, 70, 1);

    const char* roleTag = bleTransportHostMode ? "HOST" : "CLIENT";
    M5.Display.setTextColor(TFT_CYAN, TFT_BLACK);
    M5.Display.drawCentreString(roleTag, 160, 94, 2);

    if (dataLinked) {
        M5.Display.fillRoundRect(44, 126, 232, 50, 8, TFT_DARKGREEN);
        M5.Display.drawRoundRect(44, 126, 232, 50, 8, TFT_GREEN);
        M5.Display.setTextColor(TFT_GREENYELLOW, TFT_DARKGREEN);
        M5.Display.drawCentreString("LINKED", 160, 142, 2);

        M5.Display.setTextColor(TFT_YELLOW, TFT_BLACK);
        M5.Display.drawCentreString("Tap or BtnB to continue", 160, 206, 1);
    } else {
        M5.Display.fillRoundRect(44, 126, 232, 50, 8, 0x2104);
        M5.Display.drawRoundRect(44, 126, 232, 50, 8, TFT_RED);
        M5.Display.setTextColor(TFT_RED, 0x2104);
        M5.Display.drawCentreString("NOT LINKED", 160, 142, 2);

        M5.Display.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
        M5.Display.drawCentreString("Keep both devices here", 160, 188, 1);
        M5.Display.drawCentreString("Wait for packet exchange", 160, 206, 1);
    }

    M5.Display.setTextColor(TFT_DARKGREY, TFT_BLACK);
    char dbgBuf[28];
    unsigned long seenAge = (bleLastPeerSeenMs == 0) ? 9999 : (millis() - bleLastPeerSeenMs);
    snprintf(dbgBuf, sizeof(dbgBuf), "T%d D%d S%lu", transportConnected ? 1 : 0, dataLinked ? 1 : 0, seenAge);
    M5.Display.drawCentreString(dbgBuf, 160, 224, 1);
}

void handleConnectionWaitTouch() {
    static unsigned long lastConnectionUiRefreshMs = 0;
    static bool lastShownConnected = false;

    getBleLinkConnected();
    const bool connectedNow = getBleDataLinkReady();
    unsigned long now = millis();
    if (connectedNow != lastShownConnected || (now - lastConnectionUiRefreshMs) > 400) {
        drawConnectionWait();
        lastShownConnected = connectedNow;
        lastConnectionUiRefreshMs = now;
    }

    if (!connectedNow) return;

    auto touch = M5.Touch.getDetail();
    if (touch.state == m5::touch_state_t::touch_begin) {
        changeState(MAP_SELECT);
        return;
    }

    if (M5.BtnB.wasClicked()) {
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
    M5.Display.drawCentreString("KART RACER", 160, 48, 4);
    if (kForcedBleRole == 0) {
        M5.Display.drawCentreString("Tap LEFT or BtnA = HOST", 160, 142, 2);
        M5.Display.drawCentreString("Tap RIGHT or BtnC = CLIENT", 160, 164, 2);
    } else {
        M5.Display.drawCentreString("Role is fixed by firmware", 160, 142, 2);
        M5.Display.drawCentreString("Tap screen to continue", 160, 164, 2);
    }
    if (bleRoleChosen) {
        M5.Display.setTextColor(TFT_GREEN, TFT_BLACK);
        M5.Display.drawCentreString(bleTransportHostMode ? "MODE: HOST" : "MODE: CLIENT", 160, 194, 2);
    } else {
        M5.Display.setTextColor(TFT_YELLOW, TFT_BLACK);
        M5.Display.drawCentreString("Select multiplayer mode", 160, 194, 2);
    }
}

void drawCountdown() {
    M5.Display.fillScreen(TFT_BLACK);
    M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
    if (countdownValue > 0) M5.Display.drawCentreString(String(countdownValue), 160, 80, 7);
    else                    M5.Display.drawCentreString("GO!", 160, 80, 7);
}

void drawNameSelect() {
    M5.Display.fillScreen(TFT_BLACK);
    M5.Display.setTextColor(TFT_CYAN, TFT_BLACK);
    M5.Display.drawCentreString("SELECT NAME", 160, 20, 2);

    const int boxY = 88;
    for (int i = 0; i < 3; i++) {
        int boxX = 100 + i * 40;
        uint16_t border = (i == nameEditIndex) ? TFT_YELLOW : TFT_DARKGREY;
        M5.Display.drawRect(boxX, boxY, 28, 36, border);
        char ch[2] = { nameEditBuffer[i], '\0' };
        M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
        M5.Display.drawCentreString(ch, boxX + 14, boxY + 8, 4);
    }

    M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
    M5.Display.drawCentreString("BtnA: Prev  BtnC: Next", 160, 160, 1);
    M5.Display.drawCentreString("BtnB: Next letter / Done", 160, 174, 1);
    M5.Display.drawCentreString("3 letters only", 160, 200, 1);
}

void handleNameSelectInput() {
    const char charset[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    const int charsetLen = (int)strlen(charset);

    bool changed = false;

    if (M5.BtnA.wasClicked()) {
        int pos = charSetIndex(nameEditBuffer[nameEditIndex]);
        pos = (pos - 1 + charsetLen) % charsetLen;
        nameEditBuffer[nameEditIndex] = charset[pos];
        changed = true;
    }

    if (M5.BtnC.wasClicked()) {
        int pos = charSetIndex(nameEditBuffer[nameEditIndex]);
        pos = (pos + 1) % charsetLen;
        nameEditBuffer[nameEditIndex] = charset[pos];
        changed = true;
    }

    if (M5.BtnB.wasClicked()) {
        if (nameEditIndex < PLAYER_NAME_LENGTH - 1) {
            nameEditIndex++;
            changed = true;
        } else {
            strncpy(currentPlayerName, nameEditBuffer, sizeof(currentPlayerName) - 1);
            currentPlayerName[sizeof(currentPlayerName) - 1] = '\0';
            currentPlayerName[PLAYER_NAME_LENGTH] = '\0';

            // Update already-finished race record so the next sync sends the new name.
            strncpy(latestRaceResult.playerName, currentPlayerName, sizeof(latestRaceResult.playerName) - 1);
            latestRaceResult.playerName[sizeof(latestRaceResult.playerName) - 1] = '\0';
            leaderboardSyncQueued = true;

            changeState(FINAL_RESULT);
            return;
        }
    }

    if (changed) {
        drawNameSelect();
    }
}

int charSetIndex(char c) {
    const char charset[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_";
    for (int i = 0; charset[i] != '\0'; i++) {
        if (charset[i] == c) return i;
    }
    return 0;
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
    char totalTimeBuf[16];
    if (latestRaceResult.totalTimeMs > 0) {
        formatRaceTime(latestRaceResult.totalTimeMs, totalTimeBuf, sizeof(totalTimeBuf));
    } else {
        strncpy(totalTimeBuf, "--:--.-", sizeof(totalTimeBuf) - 1);
        totalTimeBuf[sizeof(totalTimeBuf) - 1] = '\0';
    }

    char line1[28];
    snprintf(line1, sizeof(line1), "1. %s", latestRaceResult.playerName);

    M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
    M5.Display.drawString(line1, 34, 78, 2);
    M5.Display.drawString("2. --------", 34, 108, 2);
    M5.Display.drawString("3. --------", 34, 138, 2);

    M5.Display.setTextColor(TFT_GREEN, TFT_BLACK);
    M5.Display.drawString(totalTimeBuf, 208, 78, 2);
    M5.Display.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    M5.Display.drawString("--:--.-", 208, 108, 2);
    M5.Display.drawString("--:--.-", 208, 138, 2);

    M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
    M5.Display.drawCentreString("BtnA: Play Again", 160, 196, 1);
    M5.Display.drawCentreString("BtnB: Edit Name", 160, 208, 1);
    M5.Display.drawCentreString("BtnC (Y): Menu", 160, 220, 1);
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

void setBleRole(bool hostMode) {
    bleTransportHostMode = hostMode;
    bleRoleChosen = true;
    applyBleTransportMode(bleTransportHostMode);
}

void handleStartScreenTouch() {
    auto touch = M5.Touch.getDetail();

    if (kForcedBleRole != 0) {
        if (touch.state == m5::touch_state_t::touch_begin || M5.BtnA.wasClicked() || M5.BtnC.wasClicked()) {
            changeState(CHARACTER_SELECT);
        }
        return;
    }

    if (touch.state == m5::touch_state_t::touch_begin) {
        if (touch.x < 160) {
            setBleRole(true);
        } else {
            setBleRole(false);
        }
        changeState(CHARACTER_SELECT);
        return;
    }

    if (M5.BtnA.wasClicked()) {
        setBleRole(true);
        changeState(CHARACTER_SELECT);
    } else if (M5.BtnC.wasClicked()) {
        setBleRole(false);
        changeState(CHARACTER_SELECT);
    }
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
void updateBleAuthorityRole(uint64_t peerNodeId) {
    if (peerNodeId == 0 || peerNodeId == bleLocalNodeId) return;

    blePeerNodeId = peerNodeId;
    bleIsHost = (bleLocalNodeId < blePeerNodeId);
    bleRoleResolved = true;
}

bool getBleLinkConnected() {
    bool serverConnectedNow = false;
    bool clientConnectedNow = false;

    if (bleServer != nullptr) {
        serverConnectedNow = (bleServer->getConnectedCount() > 0);
    }

    if (bleClient != nullptr) {
        clientConnectedNow = bleClient->isConnected();
    }

    // Client-side fallback: remote characteristic can remain valid when isConnected is transient.
    if (!clientConnectedNow && bleRemoteRaceCharacteristic != nullptr) {
        clientConnectedNow = true;
    }

    const unsigned long now = millis();
    const bool recentlySeenPeer = (bleLastPeerSeenMs != 0) && (now - bleLastPeerSeenMs <= 3000);

    bleServerPeerConnected = serverConnectedNow;
    bleClientPeerConnected = clientConnectedNow;

    if (!clientConnectedNow && !recentlySeenPeer) {
        bleRemoteRaceCharacteristic = nullptr;
    }

    bleConnected = bleServerPeerConnected || bleClientPeerConnected || recentlySeenPeer;
    return bleConnected;
}

bool getBleDataLinkReady() {
    const unsigned long now = millis();
    return (bleLastPeerSeenMs != 0) && (now - bleLastPeerSeenMs <= 1500);
}

void applyBleTransportMode(bool hostMode) {
    bleTransportHostMode = hostMode;
    bleTransportModeStartedMs = millis();

    NimBLEScan* scan = NimBLEDevice::getScan();
    NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();

    if (scan != nullptr) {
        scan->stop();
    }
    if (advertising != nullptr) {
        advertising->stop();
    }

    if (hostMode) {
        if (advertising != nullptr) {
            advertising->addServiceUUID(BLE_SERVICE_UUID);
            advertising->start();
        }
    } else {
        if (scan != nullptr) {
            scan->start(0, false, true);
        }
    }
}

void refreshBleTransportMode() {
    if (!bleInitialized) return;

    // If client mode and not connected, restart scan to find and reconnect to host.
    if (!bleTransportHostMode && currentState == CONNECTION_WAIT) {
        if (bleClient != nullptr && !bleClient->isConnected()) {
            NimBLEScan* scan = NimBLEDevice::getScan();
            if (scan != nullptr && !scan->isScanning()) {
                bleRemoteRaceCharacteristic = nullptr;
                bleClientPeerConnected = false;
                scan->start(0, false, true);
            }
        }
    }

    bool linkConnected = getBleLinkConnected();
    if (linkConnected) return;
    if (bleConnected) return;
    if (bleTransportRetryMs == 0) return;
}

void handleIncomingRacePacket(const uint8_t* data, size_t len) {
    if (data == nullptr || len < sizeof(RaceStatePacket)) return;

    RaceStatePacket packet;
    memcpy(&packet, data, sizeof(RaceStatePacket));
    if (packet.version != BLE_PACKET_VERSION) return;

    bleLastPeerSeenMs = millis();
    bleConnected = true;

    updateBleAuthorityRole(packet.nodeId);

    const bool senderIsHost = (packet.roleFlags & BLE_ROLE_FLAG_HOST) != 0;
    const bool senderRaceFinished = (packet.roleFlags & BLE_ROLE_FLAG_RACE_FINISHED) != 0;

    if (senderIsHost && !bleIsHost && packet.mapIndex < NUM_MAPS) {
        selectedMap = packet.mapIndex;
        previewMap = packet.mapIndex;
    }

    opponentWorldX = packet.worldX;
    opponentWorldY = packet.worldY;
    opponentHeadingRad = packet.headingRad;
    opponentVehicleIndex = packet.vehicleIndex;
    opponentMapIndex = packet.mapIndex;
    opponentLap = packet.lap;
    opponentRaceElapsedMs = packet.elapsedMs;
    opponentLastUpdateMs = millis();
    opponentStateValid = true;

    if (senderIsHost && !bleIsHost && packet.mapIndex == (uint8_t)selectedMap) {
        const bool splitOvalCheckpoints = (selectedMap == 0 || selectedMap == 3);
        const int sequenceLen = splitOvalCheckpoints ? OVAL_CHECKPOINT_SEQUENCE_LEN : CHECKPOINT_SEQUENCE_LEN;

        int remoteLap = (int)packet.lap;
        if (remoteLap < 1) remoteLap = 1;
        if (remoteLap > TOTAL_LAPS + 1) remoteLap = TOTAL_LAPS + 1;

        currentLap = remoteLap;
        checkpointSequencePos = (int)packet.checkpointSeqPos;
        if (checkpointSequencePos < 0) checkpointSequencePos = 0;
        if (checkpointSequencePos >= sequenceLen) checkpointSequencePos = sequenceLen - 1;
        raceElapsedMs = packet.elapsedMs;

        if (senderRaceFinished) {
        currentLap = TOTAL_LAPS + 1;
        checkpointSequencePos = 0;
        }
    }
}

class BleRaceCharCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& connInfo) override {
        Serial.printf("[BLE] onWrite callback: received packet\n");
        const std::string payload = c->getValue();
        Serial.printf("[BLE] onWrite payload size: %d\n", payload.size());
        bleLastPeerSeenMs = millis();
        bleConnected = true;
        handleIncomingRacePacket((const uint8_t*)payload.data(), payload.size());
        (void)connInfo;
    }
};

class BleServerCallbacks : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer* s, NimBLEConnInfo& i) override {
        Serial.printf("[BLE] Server: Client connected!\n");
        bleServerPeerConnected = true;
        bleConnected = true;
        bleLastPeerSeenMs = millis();
        (void)s;
        (void)i;
    }

    void onDisconnect(NimBLEServer* s, NimBLEConnInfo& i, int r) override {
        Serial.printf("[BLE] Server: Client disconnected (reason: %d)\n", r);
        bleServerPeerConnected = false;
        bleConnected = bleClientPeerConnected;
        if (!bleConnected) {
            blePeerNodeId = 0;
            bleRoleResolved = false;
            bleIsHost = true;
            applyBleTransportMode(bleTransportHostMode);
        }
        (void)s;
        (void)i;
    }
};

class BleClientCallbacks : public NimBLEClientCallbacks {
    void onConnect(NimBLEClient* c) override {
        bleClientPeerConnected = true;
        bleConnected = true;
        bleLastPeerSeenMs = millis();
        (void)c;
    }

    void onDisconnect(NimBLEClient* c, int reason) override {
        bleClientPeerConnected = false;
        bleRemoteRaceCharacteristic = nullptr;
        bleConnected = bleServerPeerConnected;
        if (!bleConnected) {
            blePeerNodeId = 0;
            bleRoleResolved = false;
            bleIsHost = true;
            applyBleTransportMode(bleTransportHostMode);
        }
        (void)c;
        (void)reason;
    }
};

static BleRaceCharCallbacks bleRaceCharCallbacks;
static BleServerCallbacks bleServerCallbacks;
static BleClientCallbacks bleClientCallbacks;

static void onBleRaceNotify(NimBLERemoteCharacteristic* c, uint8_t* data, size_t len, bool isNotify) {
    Serial.printf("[BLE] onBleRaceNotify: received %d bytes\n", len);
    bleLastPeerSeenMs = millis();
    bleConnected = true;
    handleIncomingRacePacket(data, len);
    (void)c;
    (void)isNotify;
}

class BleScanCallbacks : public NimBLEScanCallbacks {
    void onResult(const NimBLEAdvertisedDevice* advertisedDevice) override {

        if (advertisedDevice == nullptr) return;
        if (blePendingConnect) return;  // already have one queued
        if (bleRemoteRaceCharacteristic != nullptr) return;
        if (bleClient != nullptr && bleClient->isConnected()) return;
        if (!advertisedDevice->isAdvertisingService(NimBLEUUID(BLE_SERVICE_UUID))) return;

        String peerAddress = String(advertisedDevice->getAddress().toString().c_str());
        if (peerAddress == bleLocalAddress) return;

        Serial.printf("[BLE] Found host: %s — queuing connect\n", peerAddress.c_str());
        blePendingDevice = const_cast<NimBLEAdvertisedDevice*>(advertisedDevice);
        blePendingConnect = true;
        NimBLEDevice::getScan()->stop();
    }
};

static BleScanCallbacks bleScanCallbacks;

void initBLE() {
        if (bleInitialized) return;

        NimBLEDevice::init("M5-KART");
        bleLocalAddress = String(NimBLEDevice::getAddress().toString().c_str());
    bleLocalNodeId = ESP.getEfuseMac();
    blePeerNodeId = 0;
    bleIsHost = true;
    bleRoleResolved = false;
    bleTransportHostMode = true;
    bleRoleChosen = false;
    bleTransportRetryMs = 0;

        bleServer = NimBLEDevice::createServer();
        bleServer->setCallbacks(&bleServerCallbacks);

        NimBLEService* service = bleServer->createService(BLE_SERVICE_UUID);
        bleRaceCharacteristic = service->createCharacteristic(
                BLE_RACE_CHAR_UUID,
                NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR | NIMBLE_PROPERTY::NOTIFY
        );
        bleRaceCharacteristic->setCallbacks(&bleRaceCharCallbacks);
        RaceStatePacket initPacket = {};
        initPacket.version = BLE_PACKET_VERSION;
        bleRaceCharacteristic->setValue((uint8_t*)&initPacket, sizeof(initPacket));
        service->start();

        NimBLEScan* scan = NimBLEDevice::getScan();
        scan->setScanCallbacks(&bleScanCallbacks, false);
        scan->setInterval(45);
        scan->setWindow(15);
        scan->setActiveScan(true);

        applyBleTransportMode(bleTransportHostMode);

        bleInitialized = true;
        bleConnected = false;
}

void stopBLE() {
        if (!bleInitialized) return;

        NimBLEDevice::getScan()->stop();
        NimBLEDevice::getAdvertising()->stop();
        if (bleClient != nullptr && bleClient->isConnected()) {
                bleClient->disconnect();
        }

        bleRemoteRaceCharacteristic = nullptr;
        bleServerPeerConnected = false;
        bleClientPeerConnected = false;
        bleConnected = false;
        blePeerNodeId = 0;
        bleRoleResolved = false;
        bleIsHost = true;
        bleTransportHostMode = true;
        bleTransportModeStartedMs = 0;
        bleTransportRetryMs = 0;
        bleLastPeerSeenMs = 0;
}
void initWiFi() {
    if (strlen(WIFI_SSID) == 0 || strlen(WIFI_PASSWORD) == 0) {
        Serial.println("WiFi credentials missing in include/secrets.h");
        return;
    }

    if (WiFi.status() == WL_CONNECTED) {
        return;
    }

    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    Serial.print("Connecting to WiFi");
    const unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 12000) {
        delay(250);
        Serial.print(".");
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
        Serial.print("WiFi connected. IP: ");
        Serial.println(WiFi.localIP());
    } else {
        Serial.println("WiFi connection timed out");
    }
}
void uploadScoreToGCP() { changeState(BACK_TO_MENU); }
void syncLeaderboardToGCP() {
    Serial.println("=== syncLeaderboardToGCP called ===");
    Serial.printf("queued=%d  url_len=%d  wifi=%d\n",
        leaderboardSyncQueued, strlen(GCP_LEADERBOARD_URL), WiFi.status());

    if (!leaderboardSyncQueued) return;
    if (strlen(GCP_LEADERBOARD_URL) == 0) return;

    // MUST stop BLE before starting WiFi on ESP32
    stopBLE();
    delay(100);  // give radio stack time to wind down

    M5.Display.fillRect(0, 0, 320, 16, TFT_BLACK);
    M5.Display.setTextColor(TFT_YELLOW, TFT_BLACK);
    M5.Display.drawString("Connecting WiFi...", 4, 2, 1);

    if (WiFi.status() != WL_CONNECTED) initWiFi();

    if (WiFi.status() != WL_CONNECTED) {
        M5.Display.fillRect(0, 0, 320, 16, TFT_BLACK);
        M5.Display.setTextColor(TFT_RED, TFT_BLACK);
        M5.Display.drawString("WiFi FAILED", 4, 2, 1);
        Serial.println("Skipping: WiFi unavailable");
        return;
    }

    M5.Display.fillRect(0, 0, 320, 16, TFT_BLACK);
    M5.Display.setTextColor(TFT_CYAN, TFT_BLACK);
    M5.Display.drawString("Uploading...", 4, 2, 1);

    char payload[512];
    snprintf(payload, sizeof(payload),
        "{\"name\":\"%s\",\"score\":%lu,\"playerName\":\"%s\","
        "\"map\":\"%s\",\"lapsCompleted\":%u,\"totalTimeMs\":%lu,"
        "\"lap1Ms\":%lu,\"lap2Ms\":%lu,\"lap3Ms\":%lu,\"vehicleIndex\":%u}",
        latestRaceResult.playerName,
        latestRaceResult.totalTimeMs,
        latestRaceResult.playerName,
        latestRaceResult.mapName,
        latestRaceResult.lapsCompleted,
        latestRaceResult.totalTimeMs,
        latestRaceResult.lap1Ms,
        latestRaceResult.lap2Ms,
        latestRaceResult.lap3Ms,
        latestRaceResult.vehicleIndex
    );

    Serial.printf("Payload: %s\n", payload);

    WiFiClientSecure tlsClient;
    if (GCP_ALLOW_INSECURE_TLS) tlsClient.setInsecure();

    HTTPClient https;
    if (!https.begin(tlsClient, GCP_LEADERBOARD_URL)) {
        M5.Display.fillRect(0, 0, 320, 16, TFT_BLACK);
        M5.Display.setTextColor(TFT_RED, TFT_BLACK);
        M5.Display.drawString("HTTPS begin FAILED", 4, 2, 1);
        Serial.println("HTTPS begin failed");
        return;
    }

    https.addHeader("Content-Type", "application/json");
    if (strlen(GCP_API_BEARER_TOKEN) > 0) {
        char authHeader[220];
        snprintf(authHeader, sizeof(authHeader), "Bearer %s", GCP_API_BEARER_TOKEN);
        https.addHeader("Authorization", authHeader);
    }

    int code = https.POST((uint8_t*)payload, strlen(payload));
    String response = https.getString();
    https.end();

    Serial.printf("HTTP code: %d\n", code);
    Serial.println(response);

    // Show result on screen
    M5.Display.fillRect(0, 0, 320, 16, TFT_BLACK);
    if (code >= 200 && code < 300) {
        M5.Display.setTextColor(TFT_GREEN, TFT_BLACK);
        M5.Display.drawString("Score uploaded!", 4, 2, 1);
        leaderboardSyncQueued = false;
    } else {
        char errBuf[32];
        snprintf(errBuf, sizeof(errBuf), "Upload failed: %d", code);
        M5.Display.setTextColor(TFT_RED, TFT_BLACK);
        M5.Display.drawString(errBuf, 4, 2, 1);
    }
    delay(1500);
}
void syncRaceStateBLE() {
    if (!bleInitialized) return;

    unsigned long now = millis();
    if (now - lastBleSyncMs < 80) return;
    lastBleSyncMs = now;

    RaceStatePacket packet = {};
    packet.version = BLE_PACKET_VERSION;
    packet.roleFlags = 0;
    if (bleIsHost) packet.roleFlags |= BLE_ROLE_FLAG_HOST;
    if (currentLap > TOTAL_LAPS) packet.roleFlags |= BLE_ROLE_FLAG_RACE_FINISHED;
    packet.mapIndex = (uint8_t)selectedMap;
    packet.vehicleIndex = (uint8_t)selectedCharacter;
    packet.lap = (uint8_t)currentLap;
    packet.checkpointSeqPos = (uint8_t)checkpointSequencePos;
    packet.worldX = playerWorldX;
    packet.worldY = playerWorldY;
    packet.headingRad = playerHeadingRad;
    packet.elapsedMs = (uint32_t)raceElapsedMs;
    packet.nodeId = bleLocalNodeId;

    if (bleRaceCharacteristic != nullptr && bleServerPeerConnected) {
        Serial.printf("[BLE] HOST: Sending notify (peer connected)\n");
        bleRaceCharacteristic->setValue((uint8_t*)&packet, sizeof(packet));
        bleRaceCharacteristic->notify();
    }

    if (bleRemoteRaceCharacteristic != nullptr && bleClient != nullptr && bleClient->isConnected()) {
        Serial.printf("[BLE] CLIENT: Sending writeValue (isConnected=%d)\n", bleClient->isConnected() ? 1 : 0);
        bleRemoteRaceCharacteristic->writeValue((uint8_t*)&packet, sizeof(packet), false);
    }

    bleConnected = bleServerPeerConnected || bleClientPeerConnected;
}

void attemptBleClientConnect(NimBLEAdvertisedDevice* device) {
    if (bleClient == nullptr) {
        bleClient = NimBLEDevice::createClient();
        bleClient->setClientCallbacks(&bleClientCallbacks, false);
    }

    Serial.println("[BLE] Attempting connect...");
    if (!bleClient->connect(device)) {
        Serial.println("[BLE] Connect failed, restarting scan");
        if (currentState == CONNECTION_WAIT) {
            NimBLEDevice::getScan()->start(0, false, true);
        }
        return;
    }

    NimBLERemoteService* service = bleClient->getService(BLE_SERVICE_UUID);
    if (service == nullptr) {
        Serial.println("[BLE] Service not found");
        bleClient->disconnect();
        return;
    }

    bleRemoteRaceCharacteristic = service->getCharacteristic(BLE_RACE_CHAR_UUID);
    if (bleRemoteRaceCharacteristic == nullptr) {
        Serial.println("[BLE] Characteristic not found");
        bleClient->disconnect();
        return;
    }

    if (bleRemoteRaceCharacteristic->canNotify()) {
        bleRemoteRaceCharacteristic->subscribe(true, onBleRaceNotify);
    }

    bleClientPeerConnected = true;
    bleConnected = true;
    bleLastPeerSeenMs = millis();
    Serial.println("[BLE] CLIENT CONNECTED and subscribed");
}