// main.cpp — The Great Cryptid Road Trip
// Multiplayer 2D brawler client: Raylib + Emscripten + Socket.io
// Features: gamepad input, client-side prediction, 4 cryptid characters

#include <raylib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Emscripten Socket.io bindings (declared in socketio.js library)
// ---------------------------------------------------------------------------
#ifdef __EMSCRIPTEN__
#include <emscripten.h>

extern "C" {
    void SocketIO_Init(const char* url);
    void SocketIO_SendInput(int left, int right, int jump);
    void SocketIO_Disconnect();
    void SocketIO_SetOnConnect(void (*cb)());
    void SocketIO_SetOnAssigned(void (*cb)(const char*, int));
    void SocketIO_SetOnPlayerJoined(void (*cb)(const char*, int, float, float));
    void SocketIO_SetOnPlayerLeft(void (*cb)(const char*));
    void SocketIO_SetOnState(void (*cb)(const char*));
}
#endif

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------
#define SCREEN_W        1280
#define SCREEN_H        720
#define PLAYER_SIZE     32.0f
#define MOVE_SPEED      250.0f
#define JUMP_VEL       -450.0f
#define GRAVITY        1200.0f
#define GROUND_Y       600.0f
#define MAX_PLAYERS    4
#define PREDICTION_ALPHA 0.5f  // Server reconciliation blend factor

// ---------------------------------------------------------------------------
// Cryptid definitions — expandable for RPG class system
// ---------------------------------------------------------------------------
typedef struct {
    const char* id;
    const char* label;
    Color color;
} CryptidDef;

static const CryptidDef CRYPTIDS[MAX_PLAYERS] = {
    { "bigfoot",      "Bigfoot",      BROWN       },
    { "mothman",      "Mothman",      PURPLE      },
    { "jersey_devil", "Jersey Devil", RED         },
    { "chupacabra",   "Chupacabra",   DARKGREEN   },
};

// ---------------------------------------------------------------------------
// Player state — modular for future RPG stats, inventory, etc.
// ---------------------------------------------------------------------------
typedef struct {
    int active;              // Is this player slot occupied?
    char socketId[64];       // Server-assigned ID
    int cryptidIndex;        // 0-3 index into CRYPTIDS

    // Physics state
    float x, y;              // Current position
    float vx, vy;            // Velocity
    int onGround;            // Grounded flag

    // Client-side prediction: server authoritative state
    float serverX, serverY;  // Last received server position
    float predError;         // Accumulated prediction error
} Player;

// ---------------------------------------------------------------------------
// Game state — global, expandable for scenes (hub, battle, etc.)
// ---------------------------------------------------------------------------
typedef struct {
    Player players[MAX_PLAYERS];
    int myIndex;             // -1 = not yet assigned
    int connected;
    char serverUrl[256];
} GameState;

static GameState g_game;

// ---------------------------------------------------------------------------
// Simple JSON field extractor (avoids heavy JSON lib in WASM)
// ---------------------------------------------------------------------------
static float json_get_float(const char* json, const char* key) {
    char search[128];
    snprintf(search, sizeof(search), "\"%s\":", key);
    const char* pos = strstr(json, search);
    if (!pos) return 0.0f;
    pos += strlen(search);
    while (*pos == ' ') pos++;
    return (float)atof(pos);
}

// ---------------------------------------------------------------------------
// Socket.io callbacks
// ---------------------------------------------------------------------------
static void on_connect(void) {
    g_game.connected = 1;
    TraceLog(LOG_INFO, "[NET] Connected to server");
}

static void on_assigned(const char* sid, int idx) {
    strncpy(g_game.players[idx].socketId, sid, 63);
    g_game.players[idx].socketId[63] = '\0';
    g_game.players[idx].cryptidIndex = idx;
    g_game.players[idx].active = 1;
    g_game.myIndex = idx;
    TraceLog(LOG_INFO, "[NET] Assigned as %s (index %d)",
             CRYPTIDS[idx].label, idx);
}

static void on_player_joined(const char* sid, int idx, float x, float y) {
    if (idx >= 0 && idx < MAX_PLAYERS) {
        strncpy(g_game.players[idx].socketId, sid, 63);
        g_game.players[idx].socketId[63] = '\0';
        g_game.players[idx].cryptidIndex = idx;
        g_game.players[idx].active = 1;
        g_game.players[idx].x = x;
        g_game.players[idx].y = y;
        g_game.players[idx].serverX = x;
        g_game.players[idx].serverY = y;
        TraceLog(LOG_INFO, "[NET] %s joined", CRYPTIDS[idx].label);
    }
}

static void on_player_left(const char* sid) {
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (strcmp(g_game.players[i].socketId, sid) == 0) {
            g_game.players[i].active = 0;
            TraceLog(LOG_INFO, "[NET] %s left", CRYPTIDS[i].label);
            break;
        }
    }
}

static void on_state(const char* json) {
    // Parse server state JSON: {"socketId1":{"x":100,"y":200},...}
    // Walk through all active players and reconcile
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (!g_game.players[i].active) continue;

        // Build search key from socketId
        char keySearch[128];
        snprintf(keySearch, sizeof(keySearch), "\"%s\":", g_game.players[i].socketId);
        const char* objStart = strstr(json, keySearch);
        if (!objStart) continue;

        // Extract x and y from the nested object
        objStart += strlen(keySearch);
        float sx = json_get_float(objStart, "x");
        float sy = json_get_float(objStart, "y");

        g_game.players[i].serverX = sx;
        g_game.players[i].serverY = sy;

        // Client-side prediction reconciliation:
        // Blend toward server position to correct drift
        float errX = sx - g_game.players[i].x;
        float errY = sy - g_game.players[i].y;
        g_game.players[i].predError += errX * errX + errY * errY;

        // Only correct if error is significant (prevents jitter)
        float dist = sqrtf(errX * errX + errY * errY);
        if (dist > 5.0f) {
            g_game.players[i].x += errX * PREDICTION_ALPHA;
            g_game.players[i].y += errY * PREDICTION_ALPHA;
        } else {
            // Snap to server for small corrections
            g_game.players[i].x = sx;
            g_game.players[i].y = sy;
        }
    }
}

// ---------------------------------------------------------------------------
// Initialize game state
// ---------------------------------------------------------------------------
static void GameInit(void) {
    memset(&g_game, 0, sizeof(g_game));
    g_game.myIndex = -1;
    g_game.connected = 0;

    // Determine server URL (env var or default)
    const char* url = getenv("CRYPTID_SERVER");
    if (!url) url = "http://localhost:3000";
    strncpy(g_game.serverUrl, url, 255);

#ifdef __EMSCRIPTEN__
    // Initialize Socket.io connection
    SocketIO_Init(g_game.serverUrl);

    // Register callbacks
    SocketIO_SetOnConnect(on_connect);
    SocketIO_SetOnAssigned(on_assigned);
    SocketIO_SetOnPlayerJoined(on_player_joined);
    SocketIO_SetOnPlayerLeft(on_player_left);
    SocketIO_SetOnState(on_state);
#endif
}

// ---------------------------------------------------------------------------
// Process input: keyboard + gamepad
// ---------------------------------------------------------------------------
typedef struct {
    int left, right, jump;
} PlayerInput;

static PlayerInput ProcessInput(void) {
    PlayerInput input = {0, 0, 0};

    // Keyboard fallback (WASD / arrows)
    if (IsKeyDown(KEY_LEFT)  || IsKeyDown(KEY_A)) input.left  = 1;
    if (IsKeyDown(KEY_RIGHT) || IsKeyDown(KEY_D)) input.right = 1;
    if (IsKeyDown(KEY_UP)    || IsKeyDown(KEY_W) || IsKeyDown(KEY_SPACE)) input.jump = 1;

    // Gamepad input (Raylib native)
    for (int pad = 0; pad < 4; pad++) {
        if (IsGamepadAvailable(pad)) {
            float axisH = GetGamepadAxisMovement(pad, GAMEPAD_AXIS_LEFT_X);
            float axisV = GetGamepadAxisMovement(pad, GAMEPAD_AXIS_LEFT_Y);

            if (axisH < -0.3f) input.left  = 1;
            if (axisH >  0.3f) input.right = 1;

            // Jump on face button A (or right trigger)
            if (IsGamepadButtonDown(pad, GAMEPAD_BUTTON_RIGHT_FACE_DOWN) ||
                GetGamepadAxisMovement(pad, GAMEPAD_AXIS_RIGHT_TRIGGER) > 0.3f) {
                input.jump = 1;
            }
        }
    }

    return input;
}

// ---------------------------------------------------------------------------
// Update local player physics (client-side prediction)
// ---------------------------------------------------------------------------
static void UpdateLocalPlayer(float dt, PlayerInput input) {
    if (g_game.myIndex < 0) return;

    Player* p = &g_game.players[g_game.myIndex];
    if (!p->active) return;

    // Apply input to velocity
    p->vx = 0.0f;
    if (input.left)  p->vx = -MOVE_SPEED;
    if (input.right) p->vx =  MOVE_SPEED;

    // Jump (only if grounded)
    if (input.jump && p->onGround) {
        p->vy = JUMP_VEL;
        p->onGround = 0;
    }

    // Gravity
    p->vy += GRAVITY * dt;

    // Integrate
    p->x += p->vx * dt;
    p->y += p->vy * dt;

    // World bounds
    if (p->x < 0) p->x = 0;
    if (p.x > SCREEN_W - PLAYER_SIZE) p->x = SCREEN_W - PLAYER_SIZE;

    // Ground collision
    if (p->y >= GROUND_Y - PLAYER_SIZE) {
        p->y = GROUND_Y - PLAYER_SIZE;
        p->vy = 0;
        p->onGround = 1;
    }

    // Send input to server (authoritative)
#ifdef __EMSCRIPTEN__
    SocketIO_SendInput(input.left, input.right, input.jump);
#endif
}

// ---------------------------------------------------------------------------
// Render
// ---------------------------------------------------------------------------
static void Render(void) {
    BeginDrawing();
    ClearBackground(RAYWHITE);

    // Title bar
    DrawRectangle(0, 0, SCREEN_W, 48, BLACK);
    DrawText("THE GREAT CRYPTID ROAD TRIP", 20, 12, 24, WHITE);

    // Connection status
    if (!g_game.connected) {
        DrawText("Connecting...", SCREEN_W / 2 - 60, SCREEN_H / 2, 20, GRAY);
    }

    // Ground line
    DrawRectangle(0, (int)GROUND_Y, SCREEN_W, SCREEN_H - (int)GROUND_Y, DARKGRAY);
    DrawLine(0, (int)GROUND_Y, SCREEN_W, (int)GROUND_Y, BLACK);

    // Draw all active players
    for (int i = 0; i < MAX_PLAYERS; i++) {
        Player* p = &g_game.players[i];
        if (!p->active) continue;

        const CryptidDef* def = &CRYPTIDS[p->cryptidIndex];

        // Player square
        DrawRectangle((int)p->x, (int)p->y, (int)PLAYER_SIZE, (int)PLAYER_SIZE, def->color);

        // Outline for local player
        if (i == g_game.myIndex) {
            DrawRectangleLines((int)p->x - 2, (int)p->y - 2,
                              (int)PLAYER_SIZE + 4, (int)PLAYER_SIZE + 4, YELLOW);
        }

        // Label
        DrawText(def->label, (int)p->x, (int)p->y - 18, 12, BLACK);

        // Debug: show prediction error
        if (i == g_game.myIndex) {
            char debug[64];
            snprintf(debug, sizeof(debug), "err: %.1f", p->predError);
            DrawText(debug, (int)p->x, (int)p->y + PLAYER_SIZE + 4, 10, DARKGRAY);
        }
    }

    // Controls help
    DrawText("Gamepad: Left Stick + A Button  |  Keyboard: WASD/Arrows + Space",
             20, SCREEN_H - 24, 14, DARKGRAY);

    EndDrawing();
}

// ---------------------------------------------------------------------------
// Main loop (Emscripten-compatible)
// ---------------------------------------------------------------------------
static void MainLoop(void) {
    float dt = GetFrameTime();
    PlayerInput input = ProcessInput();
    UpdateLocalPlayer(dt, input);
    Render();
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------
int main(void) {
    // Window setup
    InitWindow(SCREEN_W, SCREEN_H, "The Great Cryptid Road Trip");
    SetTargetFPS(60);

    // Gamepad setup
    InitGamepadMappings();

    // Initialize game state and network
    GameInit();

#ifdef __EMSCRIPTEN__
    // Emscripten main loop (non-blocking)
    emscripten_set_main_loop(MainLoop, 0, 1);
#else
    // Desktop fallback for local testing
    while (!WindowShouldClose()) {
        MainLoop();
    }
#endif

    // Cleanup
#ifdef __EMSCRIPTEN__
    SocketIO_Disconnect();
#endif
    CloseWindow();
    return 0;
}
