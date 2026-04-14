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

// ─── Global Game Data ─────────────────────────────────────────────────────────
GameState currentState = START_SCREEN;
GameState previousState = BACK_TO_MENU; // force first draw

int playerWins   = 0;
int opponentWins = 0;
int selectedCharacter = 0;  // 0–4
int selectedMap       = 0;  // 0–2
int countdownValue    = 3;
unsigned long countdownTimer = 0;
bool screenNeedsRedraw = true;
bool bleConnected = false;

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

void initBLE();
void stopBLE();
void initWiFi();
void uploadScoreToGCP();
void syncRaceStateBLE();

// ─── Setup ────────────────────────────────────────────────────────────────────
void setup() {
    M5.begin();
    M5.Lcd.setTextSize(2);
    M5.Lcd.fillScreen(BLACK);
    Serial.begin(115200);

    changeState(START_SCREEN);
}

// ─── Main Loop ────────────────────────────────────────────────────────────────
void loop() {
    M5.update(); // updates touch + buttons

    // Redraw screen only when state changes
    if (currentState != previousState) {
        screenNeedsRedraw = true;
        previousState = currentState;
    }

    handleCurrentState();
}

// ─── State Machine ────────────────────────────────────────────────────────────
void changeState(GameState next) {
    Serial.printf("State: %d → %d\n", currentState, next);
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
            if (screenNeedsRedraw) { drawCharacterSelect(); screenNeedsRedraw = false; }
            handleCharacterSelectTouch();
            break;

        case MAP_SELECT:
            if (screenNeedsRedraw) { drawMapSelect(); screenNeedsRedraw = false; }
            handleMapSelectTouch();
            break;

        case COUNTDOWN:
            if (screenNeedsRedraw) {
                countdownValue = 3;
                countdownTimer = millis();
                drawCountdown();
                screenNeedsRedraw = false;
            }
            // Tick down every 1 second
            if (millis() - countdownTimer >= 1000) {
                countdownTimer = millis();
                countdownValue--;
                drawCountdown();
                if (countdownValue <= 0) {
                    changeState(RACE);
                }
            }
            break;

        case RACE:
            if (screenNeedsRedraw) { drawRace(); screenNeedsRedraw = false; }
            syncRaceStateBLE();     // send/receive position over BLE
            handleRaceTouch();      // item button, etc.
            // TODO: detect race finish (lap completion logic goes here)
            // if (raceFinished) changeState(ROUND_RESULT);
            break;

        case ROUND_RESULT:
            if (screenNeedsRedraw) {
                // TODO: increment winner's score
                // playerWins++ or opponentWins++
                drawRoundResult();
                screenNeedsRedraw = false;
            }
            // Auto-advance after 3 seconds
            if (millis() - countdownTimer >= 3000) {
                changeState(CHECK_FIRST_TO_7);
            }
            break;

        case CHECK_FIRST_TO_7:
            if (playerWins >= 7 || opponentWins >= 7) {
                changeState(FINAL_RESULT);
            } else {
                changeState(COUNTDOWN); // play another round
            }
            break;

        case FINAL_RESULT:
            if (screenNeedsRedraw) {
                countdownTimer = millis(); // reuse as delay timer
                drawFinalResult();
                screenNeedsRedraw = false;
            }
            // Auto-advance to score upload after 2 seconds
            if (millis() - countdownTimer >= 2000) {
                changeState(UPLOAD_SCORE);
            }
            break;

        case UPLOAD_SCORE:
            if (screenNeedsRedraw) {
                drawUploadScore();
                screenNeedsRedraw = false;
                stopBLE();          // IMPORTANT: stop BLE before starting WiFi
                initWiFi();
                uploadScoreToGCP();
            }
            // uploadScoreToGCP() should call changeState(BACK_TO_MENU) when done
            break;

        case BACK_TO_MENU:
            if (screenNeedsRedraw) { drawBackToMenu(); screenNeedsRedraw = false; }
            // Reset all scores and selections
            playerWins = 0;
            opponentWins = 0;
            selectedCharacter = 0;
            selectedMap = 0;
            bleConnected = false;
            delay(1500);
            changeState(START_SCREEN);
            break;
    }
}

// ─── Screen Draw Stubs ────────────────────────────────────────────────────────
void drawStartScreen() {
    M5.Lcd.fillScreen(BLACK);
    M5.Lcd.setTextColor(WHITE);
    M5.Lcd.drawCentreString("KART RACER", 160, 60, 4);
    // TODO: draw "1 Player" and "2 Player" touch buttons
    M5.Lcd.drawCentreString("Touch to Start", 160, 180, 2);
}

void drawCharacterSelect() {
    M5.Lcd.fillScreen(BLACK);
    M5.Lcd.drawCentreString("Pick Your Kart", 160, 20, 2);
    // TODO: draw 4-5 kart buttons across the bottom
    //       highlight selectedCharacter
}

void drawMapSelect() {
    M5.Lcd.fillScreen(BLACK);
    M5.Lcd.drawCentreString("Pick a Track", 160, 20, 2);
    // TODO: draw 3 map buttons (Map 1 / Map 2 / Map 3)
}

void drawCountdown() {
    M5.Lcd.fillScreen(BLACK);
    if (countdownValue > 0) {
        M5.Lcd.drawCentreString(String(countdownValue), 160, 80, 7);
    } else {
        M5.Lcd.drawCentreString("GO!", 160, 80, 7);
    }
}

void drawRace() {
    M5.Lcd.fillScreen(DARKGREEN);
    // TODO: draw track, kart sprites, HUD (lap count, item slot, position)
    M5.Lcd.drawCentreString("RACE", 160, 10, 2);
}

void drawRoundResult() {
    M5.Lcd.fillScreen(BLACK);
    // TODO: show who won this round + current win totals
    M5.Lcd.drawCentreString("Round Over!", 160, 60, 3);
    M5.Lcd.setCursor(20, 130);
    M5.Lcd.printf("You: %d  Opp: %d", playerWins, opponentWins);
    countdownTimer = millis(); // start delay for auto-advance
}

void drawFinalResult() {
    M5.Lcd.fillScreen(BLACK);
    M5.Lcd.drawCentreString("GAME OVER", 160, 50, 3);
    // TODO: display final winner, total wins
    if (playerWins >= 7) {
        M5.Lcd.drawCentreString("YOU WIN!", 160, 110, 4);
    } else {
        M5.Lcd.drawCentreString("YOU LOSE", 160, 110, 4);
    }
}

void drawUploadScore() {
    M5.Lcd.fillScreen(BLACK);
    M5.Lcd.drawCentreString("Uploading score...", 160, 100, 2);
}

void drawBackToMenu() {
    M5.Lcd.fillScreen(BLACK);
    M5.Lcd.drawCentreString("Returning to menu...", 160, 100, 2);
}

// ─── Touch Handlers (Stub) ────────────────────────────────────────────────────
void handleStartScreenTouch() {
    auto touch = M5.Touch.getDetail();
    if (touch.state == m5::touch_state_t::touch) { // screen was touched
        changeState(CHARACTER_SELECT);
    }
}

void handleCharacterSelectTouch() {
    auto touch = M5.Touch.getDetail();
    if (touch.state == m5::touch_state_t::touch) {
        // TODO: determine which kart button was touched
        // selectedCharacter = ...;
        changeState(MAP_SELECT);
    }
}

void handleMapSelectTouch() {
    auto touch = M5.Touch.getDetail();
    if (touch.state == m5::touch_state_t::touch) {
        // TODO: determine which map button was touched
        // selectedMap = ...;
        changeState(COUNTDOWN);
    }
}

void handleRaceTouch() {
    auto touch = M5.Touch.getDetail();
    if (touch.state == m5::touch_state_t::touch) {
        // TODO: check if touch is on the item button region
        // if (touchInRect(touch, itemBtnX, itemBtnY, itemBtnW, itemBtnH)) useItem();
    }
}

// ─── BLE / WiFi / GCP Stubs ───────────────────────────────────────────────────
void initBLE() {
    // TODO: initialize BLE server or client
    // Reference your lab 3 UUID setup here
}

void stopBLE() {
    // TODO: BLE.end() or equivalent
    // Must be called BEFORE initWiFi() — ESP32 can't run both
}

void initWiFi() {
    // TODO: WiFi.begin(ssid, password)
    // Wait for connection with timeout
}

void uploadScoreToGCP() {
    // TODO: HTTP POST to your Cloud Function endpoint
    // Body: JSON with playerWins, opponentWins, selectedCharacter, etc.
    // On success: changeState(BACK_TO_MENU)
    // On failure: show error, then changeState(BACK_TO_MENU) anyway
    changeState(BACK_TO_MENU); // placeholder until implemented
}

void syncRaceStateBLE() {
    // TODO: send local kart position/lap over BLE
    // TODO: receive opponent position/lap from BLE
    // If connection drops: changeState(START_SCREEN) or show error
}