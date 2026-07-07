#include "raylib.h"

//----------------------------------------------------------------------------------
// Global Variables Definition (local to this module)
//----------------------------------------------------------------------------------
static const int screenWidth = 720;
static const int screenHeight = 720;

static RenderTexture2D target = {0}; // Render texture to render our game
static int frameCounter = 0;

static Entity *entities = NULL; // Entity pool, one calloc at startup, packed array
static int entityCount = 0;     // Live entities in the pool
static int mouseHexQ = 0;       // Axial column under the mouse this frame
static int mouseHexR = 0;       // Axial row under the mouse this frame
static bool uiWantsMouse = false; // True when ImGui is using the mouse; board ignores clicks

// TODO: Define global variables here, recommended to make them static
