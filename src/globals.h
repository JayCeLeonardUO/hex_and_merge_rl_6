#include "raylib.h"

//----------------------------------------------------------------------------------
// Global Variables Definition (local to this module)
//----------------------------------------------------------------------------------
static const int screenWidth = 720;
static const int screenHeight = 720;

static RenderTexture2D target = {0}; // Render texture to render our game
static int frameCounter = 0;

static Texture2D heartTexture = {0}; // Heart icon for health bars (resources/heart_icon_32x32.png)

static Entity *entities = NULL;   // Entity pool, one calloc at startup, packed array
static int entityCount = 0;       // Live entities in the pool
static int mouseHexQ = 0;         // Axial column under the mouse this frame
static int mouseHexR = 0;         // Axial row under the mouse this frame
static bool uiWantsMouse = false; // True when ImGui is using the mouse; board ignores clicks

// 2.5D camera: tilted perspective looking down at the board plane (y = 0)
static Camera3D camera = {
    .position = {0.0f, 14.0f, 12.0f},
    .target = {0.0f, 0.0f, 0.0f},
    .up = {0.0f, 1.0f, 0.0f},
    .fovy = 45.0f,
    .projection = CAMERA_PERSPECTIVE,
};

// TODO: Define global variables here, recommended to make them static
