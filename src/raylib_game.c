/*******************************************************************************************
 *
 *   raylib gamejam template
 *
 *   Code licensed under an unmodified zlib/libpng license, which is an OSI-certified,
 *   BSD-like license that allows static linking with closed source software
 *
 *   Copyright (c) 2022-2026 Ramon Santamaria (@raysan5)
 *
 ********************************************************************************************/

#include "raylib.h"
#include "raymath.h" // Required for: MatrixLookAt() (camera-perpendicular billboard)
#include "rlgl.h"    // Required for: textured quad of the hover reticle

#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#include "cimgui.h"  // Dear ImGui C bindings
#include "rlImGui.h" // raylib backend for Dear ImGui

#if defined(PLATFORM_WEB)
#include <emscripten/emscripten.h> // Emscripten library
#endif

#include <stdio.h>  // Required for: printf()
#include <stdlib.h> // Required for: calloc(), free(), abs()
#include <string.h> // Required for: memset()
#include <math.h>   // Required for: sqrtf(), roundf(), fabsf()

//----------------------------------------------------------------------------------
// Defines and Macros
//----------------------------------------------------------------------------------
// Simple log system to avoid printf() calls if required
// NOTE: Avoiding those calls, also avoids const strings memory usage
#define SUPPORT_LOG_INFO
#if defined(SUPPORT_LOG_INFO)
#define LOG(...) printf(__VA_ARGS__)
#else
#define LOG(...)
#endif

#define MAX_ENTITIES 1024 // Entity pool capacity, allocated once at startup
#define GRID_RADIUS 4     // Hex board radius in cells around the center
#define HEX_SIZE 1.0f     // Hex circumradius (center to vertex) in world units

// 2.5D: tiles are hexagonal prisms standing on the y=0 plane
#define HEX_TILE_HEIGHT 0.2f       // Resting tile thickness
#define HEX_TILE_HEIGHT_HOVER 0.4f // Tile thickness while hovered

#define FIREBALL_CHARGE_NEEDED 2 // Connected lever pulls before a fireball detonates
#define FIREBALL_AOE_RADIUS 1    // Detonation reach in tiles

#define CRYSTAL_GIFT_PERIOD 2 // Turns between the center crystal's card gifts
#define ENEMY_SPAWN_PERIOD 3  // Turns between enemy reinforcements

#define HEX_CELL_OFFBOARD 999 // Axial coord that can never be on the board

// Cards: blank rectangles fanned along the bottom edge until placed on the board
#define CARD_WIDTH 84.0f
#define CARD_HEIGHT 120.0f
#define CARD_FAN_STEP 100.0f  // Horizontal distance between fan slots
#define CARD_FAN_BOTTOM 96.0f // Fan center height above the bottom screen edge
#define CARD_FAN_ARC 10.0f    // Extra drop per slot away from center, fakes a fanned hand

#define SHAKE_DURATION 0.3f   // Screen shake length after a card placement
#define IMPACT_DURATION 0.45f // Impact ring/dust lifetime at the placed tile
#define IMPACT_LIGHT_FRAMES 8 // Frames in the light-burst sheet (baked from itch light_007.mov)

// Player: a mage sprite dragged from tile to tile
#define PLAYER_FRAMES 8   // Idle frames in the player sheet (top row of the mage sheet)
#define PLAYER_FRAME_W 64 // Source frame size in pixels
#define PLAYER_FRAME_H 32

// Card models: pairs of <name>_card.glb / <name>_hex.glb exported from the
// sandbox prototype (sandbox/export_glb.py) into resources/models
#define MAX_CARD_MODELS 8
#define CARD_MODEL_H 2.6f // The exported card is 2.6 world units tall
#define HEX_MODEL_R 1.05f // The exported hex form's outer radius

//----------------------------------------------------------------------------------
// Types and Structures Definition
//----------------------------------------------------------------------------------
typedef enum
{
    SCREEN_LOGO = 0,
    SCREEN_TITLE,
    SCREEN_GAMEPLAY,
    SCREEN_ENDING
} GameScreen;

// Tag that marks an entity's behavior in the update/draw switches
typedef enum EntityKind
{
    ENTITY_NONE = 0,   // Free slot, skipped everywhere
    ENTITY_HEX_CELL,   // One clickable cell of the hex board
    ENTITY_HEALTH_BAR, // Row of heart icons in the screen-space UI layer
    ENTITY_CARD,
    ENTITY_PLAYER,
    ENTITY_ENEMY,
    ENTITY_PICKUP, // A card gift from the center crystal, collected by standing on it
    NUM_ENTITY_KINDS
} EntityKind;

typedef enum
{
    ENEMY_INTENT_NONE = 0, // Staying put (boxed in)
    ENEMY_INTENT_MOVE,     // Will walk to the intent tile
    ENEMY_INTENT_STRIKE,   // Will smash the placed card on the intent tile
    ENEMY_INTENT_ATTACK,   // No card in range: will hit the player on the intent tile for 2 hearts
} EnemyIntent;

typedef enum
{
    CARD_NONE = 0,
    CARD_LEYLINE,  // Conduit: its tile joins the leyline network
    CARD_FIREBALL, // AOE spell (logic TBD)
    CARD_HEX,      // Curse spell (logic TBD)
    CARD_WARD,     // Protection spell (logic TBD)
    NUM_CARD_KINDS
} CardKind;

// Card levels come from 2048-style merging only: placing a card onto a
// placed card of the SAME kind and level combines them into the next level
typedef enum
{
    CARD_LVL_1 = 1,
    CARD_LVL_2,
    CARD_LVL_3,
    CARD_LVL_4, // Exodia piece: flies to the hand as an inert trophy; all four kinds at 4 wins
    NUM_CARD_LEVELS
} CardLevel;

// Hand label + deal tint per kind (index by CardKind)
static const char *cardKindNames[NUM_CARD_KINDS] = {"", "ley", "fire", "hex", "ward"};
// Exodia theming: a lvl 4 card stops being a spell and becomes a sealed
// piece of the hex king (index by the kind that merged into it)
static const char *exodiaPieceNames[NUM_CARD_KINDS] = {
    "",
    "the hex king's head",      // leyline
    "the hex king's right arm", // fireball
    "the hex king's left arm",  // hex
    "the hex king's legs",      // ward
};
static const char *exodiaPieceShort[NUM_CARD_KINDS] = {"", "king's head", "right arm", "left arm", "king's legs"};
// Hover tooltip per kind, copied into the card's tooltip buffer at deal
static const char *cardKindTooltips[NUM_CARD_KINDS] = {
    "",
    "conduit: its tile joins the leyline network",
    "leyline-charged: detonates at 2 charges, hits all within 1 tile",
    "curse trap: the enemy that strikes it fights for you 1/2/3 turns",
    "baits enemy strikes, absorbs the hit for no heart, rests 1 turn after",
};
static const Color cardKindTints[NUM_CARD_KINDS] = {
    {255, 255, 255, 255}, // CARD_NONE
    {140, 225, 255, 255}, // leyline: conduit cyan
    {255, 130, 45, 255},  // fireball: fire orange
    {185, 100, 235, 255}, // hex: curse violet
    {255, 215, 120, 255}, // ward: warding gold
};
// Committed turns a played card stays on the field before returning to the
// hand (index by CardKind); a merge refreshes the clock
static const int cardKindLifetimes[NUM_CARD_KINDS] = {0, 3, 2, 2, 3};

// Mode Of Card
typedef enum CardMode
{
    NOT_A_CARD = 0,
    CARD_HEX_FORM,
    CARD_REC_FORM,
} CardMode;

// Material Effect
typedef enum MaterialEffect
{
    EFFECT_NONE = 0,
    GEM,
    INVERSE_GEM,
    NUM_EFFECTS,
} MaterialEffect;

// Fat struct: every possible member any entity could have, in one struct.
// An entity is initialized by tagging it with its kind; the update/draw
// switches on kind decide which members mean anything for it.
typedef struct Entity
{
    EntityKind kind; // Behavior tag, ENTITY_NONE = free slot

    // Spatial members (world space, board lies on the y=0 plane;
    // UI kinds read position.x/.y as screen pixels instead)
    Vector3 position; // World position (hex cell: base center on the board plane)
    Vector3 velocity; // World units per frame, for anything that moves
    float radius;     // World radius (hex cell: circumradius)

    // Hex board members (axial coordinates, pointy-top on the XZ plane)
    // https://www.redblobgames.com/grids/hexagons/
    int q; // Axial column, 0 at board center, range [-GRID_RADIUS, GRID_RADIUS]
    int r; // Axial row, 0 at board center, |q + r| <= GRID_RADIUS

    bool isLeyline; // Hex cell: a placed leyline card made this tile conduct

    // Gameplay members
    int value;     // Merge value, 0 means empty
    bool hovered;  // True while the mouse ray hits this entity (refreshed every frame)
    bool selected; // True while grabbed (cards and the player)
    int health;    // Current health shown by a health bar
    int maxHealth; // Total hearts a health bar draws

    // Visual members
    Color tint;       // Base fill color
    float alpha;      // UI fade 0..1 (card: rectangle hides while ghosting on the board)
    int frameCreated; // frameCounter value when this entity was spawned

    // Parrent Hex cell
    int parent;

    // Which card/hex model this entity wears (index into the model arrays);
    // cards get it at spawn, cells copy it from the card placed on them
    int modelIndex;

    // Card hover press tilt, eased toward the cursor (-1..1 per axis)
    Vector2 pressTilt;

    // Mode of Card
    CardMode cardMode;

    // Kind of Card: tags the logic of the card
    CardKind cardKind;

    // Card level, raised only by merging (see CardLevel). Cells remember the
    // level of the card placed on them so merges can match against it
    CardLevel cardLevel;

    // Hex cell: procedurally generated crystal shell worn while a card sits
    // on the tile (built once per cell at grid spawn, unloaded at shutdown)
    Mesh cardShellMesh;

    // Hex cell: an enemy telegraphed a strike on this card. Set/cleared only
    // by EnemyAIUpdate; the shell draws with the danger look while true
    bool inDanger;

    // Hex cell, fireball emplacement: charges banked so far (+1 per
    // committed turn while a live leyline taps the tile); detonates at
    // FIREBALL_CHARGE_NEEDED with a radius-1 AOE
    int fireballCharge;

    // Hex cell: committed turns a spent lvl 1 fireball husk keeps blocking
    // this tile (cardKind/cardLevel stay set so the husk can hand the card
    // back when it cools); 0 = no husk.
    // Card: committed turns this card is DEAD in the hand (ungrabbable,
    // grayed) -- a used ward comes home exhausted for one turn
    int huskTurns;

    // Enemy Shader params

    float enemyPulseSpeed;
    float enemyBreatheAmp;
    float enemyBreatheSpeed;
    int enemyMoveRange; // Tiles walked toward the player per committed turn

    // The enemy's telegraphed plan for the next lever pull, recomputed by
    // EnemyAIUpdate on every board change and read verbatim by DrawIntent
    int enemyIntentKind; // ENEMY_INTENT_*
    int enemyIntentQ;    // Target tile of the plan
    int enemyIntentR;

    // Enemy: committed turns left fighting for the player after striking a
    // hex card (0 = normal). While set the AI hunts other enemies instead of
    // cards/player and the outline hull renders curse-purple
    int hexedTurns;

    bool IsDraggable;

    // Movement Range
    int moveRange;

    // Hex cell: committed turns the placed card has left on the field. Set
    // from the kind's lifetime when the card is played (a merge refreshes
    // it), counted down by the turn pass, card returns to the hand at 0
    int turnLifetime;

    // Tooltip text shown while hovering this entity; empty string = no tooltip
    char tooltip[128];

} Entity;

// Hover reticle: a pixel-art frame that chases the hovered tile on a spring.
// One persistent object, integrated with dt each frame (never reset by draw order)
typedef struct HoverReticle
{
    Vector3 position; // World position on the board plane (y is recomputed every frame)
    Vector3 velocity; // World units per second, integrated by the spring
    float scale;      // Current half-size of the quad in world units
    float spinAngle;  // Radians, integrated from angular velocity (not derived from clock time)
    float bobPhase;   // Radians, integrated for the float bob
    bool active;      // False while nothing is hovered; the next hover re-snaps in place
} HoverReticle;

// One-shot impact effect at the tile where a card just landed
typedef struct ImpactFx
{
    Vector3 position; // Tile base center on the board plane
    Color tint;       // Card color driving the ring and dust
    float age;        // Seconds since the placement
    bool active;      // False once the effect has played out
} ImpactFx;

//----------------------------------------------------------------------------------
// Global Variables Definition (local to this module)
//----------------------------------------------------------------------------------
static const int screenWidth = 720;
static const int screenHeight = 720;

static RenderTexture2D target = {0}; // Render texture to render our game
static int frameCounter = 0;

static Texture2D heartTexture = {0};       // Heart icon for health bars (resources/heart_icon_32x32.png)
static Texture2D reticleTexture = {0};     // Hover reticle frame (resources/highlight_slot_26x26.png)
static Texture2D impactLightTexture = {0}; // Light-burst sheet, 8 frames in a row, white on alpha
                                           // (resources/impact_light_8x256.png)
static Texture2D playerTexture = {0};      // Player idle sheet, 8 frames of 64x32 in a row
                                           // (resources/player_mage_8x64x32.png)
static Texture2D tileArtTexture = {0};     // Tile-top art atlas: 8 hex-masked cutout variants
                                           // in a row (resources/tile_clouds_variants_8x256.png)
#define TILE_ART_VARIANTS 8
static Texture2D leverTrackTexture = {0}; // Turn lever track (resources/lever_track_128x16.png)
static Texture2D leverFillTexture = {0};  // Turn lever pull fill (resources/lever_fill_120x8.png)
static Texture2D leverKnobTexture = {0};  // Turn lever grab point (resources/lever_knob_16x18.png)

static bool leverWantsMouse = false;     // True when the turn button owns the mouse; board ignores it
static Vector2 leverKnobScreenPos = {0}; // Button center this frame; the 3D gem crystal draws here
                                         // (set during the button step, so it lags one frame like ImGui)

static HoverReticle reticle = {0}; // Persistent hover-effect state, updated in DrawHoverdHexEffect()

static ImpactFx impact = {0};      // Impact ring/dust of the last card placement
static float shakeTimeLeft = 0.0f; // Screen shake seconds remaining (kicked by a card placement)

// Card/hex models loaded from resources/models; 0 loaded = the card draw
// falls back to the old flat rectangles and cylinder ghost
static Model cardModels[MAX_CARD_MODELS] = {0};
static Model hexModels[MAX_CARD_MODELS] = {0};
static int cardModelCount = 0;
// Model pinning: every card kind wears one specific export, matched by
// filename substring during the model scan (-1 = not found, random fallback)
static int kindModelIndex[NUM_CARD_KINDS] = {-1, -1, -1, -1, -1};
static const char *kindModelTags[NUM_CARD_KINDS] = {
    "",       // CARD_NONE
    "Icon85", // leyline: blue sparkles
    "Icon16", // fireball: flames
    "Icon38", // hex: the heart card
    "Icon29", // ward: the shield card
};

// Foil shine for the card models: a diagonal iridescent band swept across the
// face, drawn as an additive second pass. The fragment shader lives here as a
// string; the mask is each mesh texture's own alpha.
#if defined(PLATFORM_WEB)
static const char *shineFS =
    "#version 100\n"
    "precision mediump float;\n"
    "varying vec2 fragTexCoord;\n"
    "uniform sampler2D texture0;\n"
    "uniform float time;\n"
    "uniform float phase;\n"
    "void main()\n"
    "{\n"
    "    vec4 tex = texture2D(texture0, fragTexCoord);\n"
    "    float mask = step(0.01, tex.a);\n"
    "    float sweep = fract(time*0.22 + phase);\n"
    "    float d = fragTexCoord.x*0.6 + fragTexCoord.y*0.4 - (sweep*2.2 - 0.6);\n"
    "    float band = smoothstep(0.16, 0.0, abs(d));\n"
    "    vec3 rainbow = 0.5 + 0.5*cos(6.2831*(d*2.0 + vec3(0.0, 0.33, 0.67)));\n"
    "    gl_FragColor = vec4(mix(vec3(1.0), rainbow, 0.6), band*0.4*mask);\n"
    "}\n";
#else
static const char *shineFS =
    "#version 330\n"
    "in vec2 fragTexCoord;\n"
    "uniform sampler2D texture0;\n"
    "uniform float time;\n"
    "uniform float phase;\n"
    "out vec4 finalColor;\n"
    "void main()\n"
    "{\n"
    "    vec4 tex = texture(texture0, fragTexCoord);\n"
    "    float mask = step(0.01, tex.a);\n"
    "    float sweep = fract(time*0.22 + phase);\n"
    "    float d = fragTexCoord.x*0.6 + fragTexCoord.y*0.4 - (sweep*2.2 - 0.6);\n"
    "    float band = smoothstep(0.16, 0.0, abs(d));\n"
    "    vec3 rainbow = 0.5 + 0.5*cos(6.2831*(d*2.0 + vec3(0.0, 0.33, 0.67)));\n"
    "    finalColor = vec4(mix(vec3(1.0), rainbow, 0.6), band*0.4*mask);\n"
    "}\n";
#endif
static Shader shineShader = {0};
static int shineTimeLoc = -1;
static int shinePhaseLoc = -1;

// Aurora background: procedural curtains drawn as a fullscreen quad behind
// the board. Three noise-driven layers, each a wavy baseline feathering
// upward, modulated by vertical rays; green at the base, violet up high.
#if defined(PLATFORM_WEB)
static const char *auroraFS =
    "#version 100\n"
    "precision mediump float;\n"
    "varying vec2 fragTexCoord;\n"
    "uniform float time;\n"
    "uniform float intensity;\n"
    "float hash(float n) { return fract(sin(n)*43758.5453); }\n"
    "float noise(vec2 p)\n"
    "{\n"
    "    vec2 i = floor(p);\n"
    "    vec2 f = fract(p);\n"
    "    f = f*f*(3.0 - 2.0*f);\n"
    "    float a = hash(i.x + i.y*57.0);\n"
    "    float b = hash(i.x + 1.0 + i.y*57.0);\n"
    "    float c = hash(i.x + (i.y + 1.0)*57.0);\n"
    "    float d = hash(i.x + 1.0 + (i.y + 1.0)*57.0);\n"
    "    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);\n"
    "}\n"
    "void main()\n"
    "{\n"
    "    vec2 uv = vec2(fragTexCoord.x, 1.0 - fragTexCoord.y);\n"
    "    vec3 col = vec3(0.0);\n"
    "    for (int i = 0; i < 3; i++)\n"
    "    {\n"
    "        float fi = float(i);\n"
    "        float wave = noise(vec2(uv.x*3.0 + fi*7.3 + time*(0.05 + fi*0.03), time*0.1 + fi*3.1));\n"
    "        float base = 0.35 + 0.18*fi + (wave - 0.5)*0.35;\n"
    "        float d = uv.y - base;\n"
    "        float curtain = exp(-max(d, 0.0)*(7.0 - fi*1.5))*exp(-max(-d, 0.0)*30.0);\n"
    "        float rays = 0.55 + 0.45*noise(vec2(uv.x*26.0 + fi*13.0, time*(0.35 + 0.1*fi)));\n"
    "        vec3 tint = mix(vec3(0.10, 0.95, 0.45), vec3(0.45, 0.25, 0.95), clamp(d*2.2 + fi*0.2, 0.0, 1.0));\n"
    "        col += tint*curtain*rays;\n"
    "    }\n"
    "    gl_FragColor = vec4(col*intensity, 1.0);\n"
    "}\n";
#else
static const char *auroraFS =
    "#version 330\n"
    "in vec2 fragTexCoord;\n"
    "uniform float time;\n"
    "uniform float intensity;\n"
    "out vec4 finalColor;\n"
    "float hash(float n) { return fract(sin(n)*43758.5453); }\n"
    "float noise(vec2 p)\n"
    "{\n"
    "    vec2 i = floor(p);\n"
    "    vec2 f = fract(p);\n"
    "    f = f*f*(3.0 - 2.0*f);\n"
    "    float a = hash(i.x + i.y*57.0);\n"
    "    float b = hash(i.x + 1.0 + i.y*57.0);\n"
    "    float c = hash(i.x + (i.y + 1.0)*57.0);\n"
    "    float d = hash(i.x + 1.0 + (i.y + 1.0)*57.0);\n"
    "    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);\n"
    "}\n"
    "void main()\n"
    "{\n"
    "    vec2 uv = vec2(fragTexCoord.x, 1.0 - fragTexCoord.y);\n"
    "    vec3 col = vec3(0.0);\n"
    "    for (int i = 0; i < 3; i++)\n"
    "    {\n"
    "        float fi = float(i);\n"
    "        float wave = noise(vec2(uv.x*3.0 + fi*7.3 + time*(0.05 + fi*0.03), time*0.1 + fi*3.1));\n"
    "        float base = 0.35 + 0.18*fi + (wave - 0.5)*0.35;\n"
    "        float d = uv.y - base;\n"
    "        float curtain = exp(-max(d, 0.0)*(7.0 - fi*1.5))*exp(-max(-d, 0.0)*30.0);\n"
    "        float rays = 0.55 + 0.45*noise(vec2(uv.x*26.0 + fi*13.0, time*(0.35 + 0.1*fi)));\n"
    "        vec3 tint = mix(vec3(0.10, 0.95, 0.45), vec3(0.45, 0.25, 0.95), clamp(d*2.2 + fi*0.2, 0.0, 1.0));\n"
    "        col += tint*curtain*rays;\n"
    "    }\n"
    "    finalColor = vec4(col*intensity, 1.0);\n"
    "}\n";
#endif
static Shader auroraShader = {0};
static int auroraTimeLoc = -1;
static int auroraIntensityLoc = -1;
static float auroraIntensity = 0.9f; // Sky brightness, debug tweakable
static Texture2D whiteTexture = {0}; // 1x1 white, the canvas for fullscreen shader quads

// Water: an endless plane under the board that mirrors the sky. Wave noise
// perturbs the normal, the eye ray reflects off it, and the reflected ray is
// intersected with a virtual sky plane running the same aurora curtain math
// (and the same time/intensity), so the water reflects the sky it sits under.
// Fresnel fades the mirror into deep teal looking straight down; crest glints
// on top. Uses gemVS for world position/normal.
#if defined(PLATFORM_WEB)
static const char *waterFS =
    "#version 100\n"
    "precision mediump float;\n"
    "varying vec3 fragPosition;\n"
    "varying vec3 fragNormal;\n"
    "uniform vec3 viewPos;\n"
    "uniform float time;\n"
    "uniform float intensity;\n"
    "uniform float waveAmp;\n"
    "uniform float waveScale;\n"
    "float hash(float n) { return fract(sin(n)*43758.5453); }\n"
    "float noise(vec2 p)\n"
    "{\n"
    "    vec2 i = floor(p);\n"
    "    vec2 f = fract(p);\n"
    "    f = f*f*(3.0 - 2.0*f);\n"
    "    float a = hash(i.x + i.y*57.0);\n"
    "    float b = hash(i.x + 1.0 + i.y*57.0);\n"
    "    float c = hash(i.x + (i.y + 1.0)*57.0);\n"
    "    float d = hash(i.x + 1.0 + (i.y + 1.0)*57.0);\n"
    "    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);\n"
    "}\n"
    "vec3 aurora(vec2 uv)\n"
    "{\n"
    "    vec3 col = vec3(0.0);\n"
    "    for (int i = 0; i < 3; i++)\n"
    "    {\n"
    "        float fi = float(i);\n"
    "        float wave = noise(vec2(uv.x*3.0 + fi*7.3 + time*(0.05 + fi*0.03), time*0.1 + fi*3.1));\n"
    "        float base = 0.35 + 0.18*fi + (wave - 0.5)*0.35;\n"
    "        float d = uv.y - base;\n"
    "        float curtain = exp(-max(d, 0.0)*(7.0 - fi*1.5))*exp(-max(-d, 0.0)*30.0);\n"
    "        float rays = 0.55 + 0.45*noise(vec2(uv.x*26.0 + fi*13.0, time*(0.35 + 0.1*fi)));\n"
    "        vec3 tint = mix(vec3(0.10, 0.95, 0.45), vec3(0.45, 0.25, 0.95), clamp(d*2.2 + fi*0.2, 0.0, 1.0));\n"
    "        col += tint*curtain*rays;\n"
    "    }\n"
    "    return col*intensity;\n"
    "}\n"
    "vec3 skyColor(vec3 org, vec3 dir)\n"
    "{\n"
    "    float up = clamp(dir.y, 0.0, 1.0);\n"
    "    vec3 col = mix(vec3(0.06, 0.06, 0.10), vec3(0.01, 0.01, 0.03), up);\n"
    "    vec2 sp = dir.xz/(dir.y + 0.25);\n"
    "    float star = hash(floor(sp.x*60.0) + floor(sp.y*60.0)*91.7);\n"
    "    col += vec3(smoothstep(0.995, 1.0, star))*up;\n"
    "    if (dir.z < -0.001)\n"
    "    {\n"
    "        float tt = (-30.0 - org.z)/dir.z;\n"
    "        vec3 hit = org + dir*tt;\n"
    "        vec2 uv = vec2((hit.x + 40.0)/80.0, hit.y/26.0);\n"
    "        if (tt > 0.0 && uv.x > 0.0 && uv.x < 1.0 && uv.y > 0.0 && uv.y < 1.0) col += aurora(uv);\n"
    "    }\n"
    "    return col;\n"
    "}\n"
    "float waveHeight(vec2 xz)\n"
    "{\n"
    "    return noise(xz*waveScale + vec2(time*0.35, time*0.22)) + 0.5*noise(xz*waveScale*2.7 - vec2(time*0.28, time*0.4));\n"
    "}\n"
    "void main()\n"
    "{\n"
    "    vec2 xz = fragPosition.xz;\n"
    "    float e = 0.18;\n"
    "    float hC = waveHeight(xz);\n"
    "    float hX = waveHeight(xz + vec2(e, 0.0));\n"
    "    float hZ = waveHeight(xz + vec2(0.0, e));\n"
    "    vec3 N = normalize(vec3(-(hX - hC)/e*waveAmp, 1.0, -(hZ - hC)/e*waveAmp));\n"
    "    vec3 I = normalize(fragPosition - viewPos);\n"
    "    vec3 R = reflect(I, N);\n"
    "    R.y = abs(R.y);\n"
    "    vec3 sky = skyColor(fragPosition, R);\n"
    "    vec3 deep = vec3(0.02, 0.07, 0.09);\n"
    "    float fres = pow(1.0 - max(dot(-I, N), 0.0), 3.0);\n"
    "    vec3 col = mix(deep, sky, 0.25 + 0.75*fres);\n"
    "    float glint = pow(max(dot(R, normalize(vec3(0.3, 0.5, -1.0))), 0.0), 60.0);\n"
    "    col += vec3(0.7, 0.9, 1.0)*glint*0.6;\n"
    "    gl_FragColor = vec4(col, 1.0);\n"
    "}\n";
#else
static const char *waterFS =
    "#version 330\n"
    "in vec3 fragPosition;\n"
    "in vec3 fragNormal;\n"
    "uniform vec3 viewPos;\n"
    "uniform float time;\n"
    "uniform float intensity;\n"
    "uniform float waveAmp;\n"
    "uniform float waveScale;\n"
    "out vec4 finalColor;\n"
    "float hash(float n) { return fract(sin(n)*43758.5453); }\n"
    "float noise(vec2 p)\n"
    "{\n"
    "    vec2 i = floor(p);\n"
    "    vec2 f = fract(p);\n"
    "    f = f*f*(3.0 - 2.0*f);\n"
    "    float a = hash(i.x + i.y*57.0);\n"
    "    float b = hash(i.x + 1.0 + i.y*57.0);\n"
    "    float c = hash(i.x + (i.y + 1.0)*57.0);\n"
    "    float d = hash(i.x + 1.0 + (i.y + 1.0)*57.0);\n"
    "    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);\n"
    "}\n"
    "vec3 aurora(vec2 uv)\n"
    "{\n"
    "    vec3 col = vec3(0.0);\n"
    "    for (int i = 0; i < 3; i++)\n"
    "    {\n"
    "        float fi = float(i);\n"
    "        float wave = noise(vec2(uv.x*3.0 + fi*7.3 + time*(0.05 + fi*0.03), time*0.1 + fi*3.1));\n"
    "        float base = 0.35 + 0.18*fi + (wave - 0.5)*0.35;\n"
    "        float d = uv.y - base;\n"
    "        float curtain = exp(-max(d, 0.0)*(7.0 - fi*1.5))*exp(-max(-d, 0.0)*30.0);\n"
    "        float rays = 0.55 + 0.45*noise(vec2(uv.x*26.0 + fi*13.0, time*(0.35 + 0.1*fi)));\n"
    "        vec3 tint = mix(vec3(0.10, 0.95, 0.45), vec3(0.45, 0.25, 0.95), clamp(d*2.2 + fi*0.2, 0.0, 1.0));\n"
    "        col += tint*curtain*rays;\n"
    "    }\n"
    "    return col*intensity;\n"
    "}\n"
    "vec3 skyColor(vec3 org, vec3 dir)\n"
    "{\n"
    "    float up = clamp(dir.y, 0.0, 1.0);\n"
    "    vec3 col = mix(vec3(0.06, 0.06, 0.10), vec3(0.01, 0.01, 0.03), up);\n"
    "    vec2 sp = dir.xz/(dir.y + 0.25);\n"
    "    float star = hash(floor(sp.x*60.0) + floor(sp.y*60.0)*91.7);\n"
    "    col += vec3(smoothstep(0.995, 1.0, star))*up;\n"
    "    if (dir.z < -0.001)\n"
    "    {\n"
    "        float tt = (-30.0 - org.z)/dir.z;\n"
    "        vec3 hit = org + dir*tt;\n"
    "        vec2 uv = vec2((hit.x + 40.0)/80.0, hit.y/26.0);\n"
    "        if (tt > 0.0 && uv.x > 0.0 && uv.x < 1.0 && uv.y > 0.0 && uv.y < 1.0) col += aurora(uv);\n"
    "    }\n"
    "    return col;\n"
    "}\n"
    "float waveHeight(vec2 xz)\n"
    "{\n"
    "    return noise(xz*waveScale + vec2(time*0.35, time*0.22)) + 0.5*noise(xz*waveScale*2.7 - vec2(time*0.28, time*0.4));\n"
    "}\n"
    "void main()\n"
    "{\n"
    "    vec2 xz = fragPosition.xz;\n"
    "    float e = 0.18;\n"
    "    float hC = waveHeight(xz);\n"
    "    float hX = waveHeight(xz + vec2(e, 0.0));\n"
    "    float hZ = waveHeight(xz + vec2(0.0, e));\n"
    "    vec3 N = normalize(vec3(-(hX - hC)/e*waveAmp, 1.0, -(hZ - hC)/e*waveAmp));\n"
    "    vec3 I = normalize(fragPosition - viewPos);\n"
    "    vec3 R = reflect(I, N);\n"
    "    R.y = abs(R.y);\n"
    "    vec3 sky = skyColor(fragPosition, R);\n"
    "    vec3 deep = vec3(0.02, 0.07, 0.09);\n"
    "    float fres = pow(1.0 - max(dot(-I, N), 0.0), 3.0);\n"
    "    vec3 col = mix(deep, sky, 0.25 + 0.75*fres);\n"
    "    float glint = pow(max(dot(R, normalize(vec3(0.3, 0.5, -1.0))), 0.0), 60.0);\n"
    "    col += vec3(0.7, 0.9, 1.0)*glint*0.6;\n"
    "    finalColor = vec4(col, 1.0);\n"
    "}\n";
#endif
static Shader waterShader = {0};
static int waterViewPosLoc = -1;
static int waterTimeLoc = -1;
static int waterIntensityLoc = -1;
static int waterWaveAmpLoc = -1;
static int waterWaveScaleLoc = -1;
static float waterWaveAmp = 0.35f;  // Wave steepness; 0 = flat mirror
static float waterWaveScale = 1.4f; // Ripple frequency
#define WATER_LEVEL (-0.05f)

// Enemy: the architect demo's icosphere, spiked and breathing entirely in
// the vertex shader (stable per-vertex hash picks the spikes, lengths pulse
// on time, the body scales on a breath sine, spin is a yaw uniform). One
// fragment shader draws the whole look: near-black body with a pastel
// fresnel rim, plus the mesh-defining lines computed from barycentric coords
// (each unwelded corner's id rides the vertex color alpha as 0/0.5/1 and
// interpolates into barycentrics; RGB is the pastel palette). fwidth keeps
// the lines a constant pixel width, and unlike glPolygonMode wireframe this
// works on GLES/WebGL too.
#if defined(PLATFORM_WEB)
static const char *enemyVS =
    "#version 100\n"
    "attribute vec3 vertexPosition;\n"
    "attribute vec3 vertexNormal;\n"
    "attribute vec4 vertexColor;\n"
    "uniform mat4 mvp;\n"
    "uniform mat4 matModel;\n"
    "uniform float time;\n"
    "uniform float yaw;\n"
    "uniform float spikeLen;\n"
    "uniform float spikeDensity;\n"
    "uniform float pulseSpeed;\n"
    "uniform float breatheAmp;\n"
    "uniform float breatheSpeed;\n"
    "uniform float expand;\n"
    "varying vec4 fragColor;\n"
    "varying vec3 fragNormal;\n"
    "varying vec3 fragWorldPos;\n"
    "varying vec3 fragBary;\n"
    "float hash(vec3 p) { return fract(sin(dot(p, vec3(12.9898, 78.233, 37.719)))*43758.5453); }\n"
    "void main()\n"
    "{\n"
    "    float h = hash(vertexPosition);\n"
    "    float mask = smoothstep(1.0 - spikeDensity, 1.0, h);\n"
    "    float wob = 0.7 + 0.3*sin(time*pulseSpeed + h*6.2831);\n"
    "    float breath = 1.0 + breatheAmp*sin(time*breatheSpeed);\n"
    "    vec3 p = (vertexPosition + vertexNormal*(spikeLen*mask*wob + expand))*breath;\n"
    "    float c = cos(yaw); float s = sin(yaw);\n"
    "    mat3 rot = mat3(c, 0.0, s, 0.0, 1.0, 0.0, -s, 0.0, c);\n"
    "    p = rot*p;\n"
    "    float aa = vertexColor.a;\n"
    "    fragBary = vec3(1.0 - step(0.25, aa), step(0.25, aa)*(1.0 - step(0.75, aa)), step(0.75, aa));\n"
    "    fragColor = vec4(vertexColor.rgb, 1.0);\n"
    "    fragNormal = rot*vertexNormal;\n"
    "    fragWorldPos = vec3(matModel*vec4(p, 1.0));\n"
    "    gl_Position = mvp*vec4(p, 1.0);\n"
    "}\n";
static const char *enemyFS =
    "#version 100\n"
    "#extension GL_OES_standard_derivatives : enable\n"
    "precision mediump float;\n"
    "varying vec4 fragColor;\n"
    "varying vec3 fragNormal;\n"
    "varying vec3 fragWorldPos;\n"
    "varying vec3 fragBary;\n"
    "uniform vec3 viewPos;\n"
    "uniform float rim;\n"
    "uniform float lineWidth;\n"
    "void main()\n"
    "{\n"
    "    vec3 d = fwidth(fragBary);\n"
    "    vec3 a3 = smoothstep(vec3(0.0), d*lineWidth, fragBary);\n"
    "    float edge = 1.0 - min(min(a3.x, a3.y), a3.z);\n"
    "    vec3 V = normalize(viewPos - fragWorldPos);\n"
    "    float fres = pow(1.0 - clamp(dot(V, normalize(fragNormal)), 0.0, 1.0), 2.5);\n"
    "    vec3 body = mix(vec3(0.05, 0.05, 0.09), fragColor.rgb, fres*rim);\n"
    "    gl_FragColor = vec4(mix(body, fragColor.rgb, edge), 1.0);\n"
    "}\n";
// L4D-style outline: inverted hull (front faces culled, expand > 0 in the
// shared VS), flat danger-red pulsing slowly
static const char *enemyOutlineFS =
    "#version 100\n"
    "precision mediump float;\n"
    "varying vec4 fragColor;\n"
    "varying vec3 fragNormal;\n"
    "varying vec3 fragWorldPos;\n"
    "varying vec3 fragBary;\n"
    // highp to match the VS's implicit precision: WebGL refuses to link
    // programs whose shared uniforms differ in precision between stages
    "uniform highp float time;\n"
    "uniform float alpha;\n"
    "uniform float hexed;\n"
    "void main()\n"
    "{\n"
    "    float pulse = 0.85 + 0.15*sin(time*2.6);\n"
    "    vec3 hull = mix(vec3(1.0, 0.08, 0.06), vec3(0.72, 0.39, 0.92), hexed);\n"
    "    gl_FragColor = vec4(hull*pulse, alpha);\n"
    "}\n";
#else
static const char *enemyVS =
    "#version 330\n"
    "in vec3 vertexPosition;\n"
    "in vec3 vertexNormal;\n"
    "in vec4 vertexColor;\n"
    "uniform mat4 mvp;\n"
    "uniform mat4 matModel;\n"
    "uniform float time;\n"
    "uniform float yaw;\n"
    "uniform float spikeLen;\n"
    "uniform float spikeDensity;\n"
    "uniform float pulseSpeed;\n"
    "uniform float breatheAmp;\n"
    "uniform float breatheSpeed;\n"
    "uniform float expand;\n"
    "out vec4 fragColor;\n"
    "out vec3 fragNormal;\n"
    "out vec3 fragWorldPos;\n"
    "out vec3 fragBary;\n"
    "float hash(vec3 p) { return fract(sin(dot(p, vec3(12.9898, 78.233, 37.719)))*43758.5453); }\n"
    "void main()\n"
    "{\n"
    "    float h = hash(vertexPosition);\n"
    "    float mask = smoothstep(1.0 - spikeDensity, 1.0, h);\n"
    "    float wob = 0.7 + 0.3*sin(time*pulseSpeed + h*6.2831);\n"
    "    float breath = 1.0 + breatheAmp*sin(time*breatheSpeed);\n"
    "    vec3 p = (vertexPosition + vertexNormal*(spikeLen*mask*wob + expand))*breath;\n"
    "    float c = cos(yaw); float s = sin(yaw);\n"
    "    mat3 rot = mat3(c, 0.0, s, 0.0, 1.0, 0.0, -s, 0.0, c);\n"
    "    p = rot*p;\n"
    "    float aa = vertexColor.a;\n"
    "    fragBary = vec3(1.0 - step(0.25, aa), step(0.25, aa)*(1.0 - step(0.75, aa)), step(0.75, aa));\n"
    "    fragColor = vec4(vertexColor.rgb, 1.0);\n"
    "    fragNormal = rot*vertexNormal;\n"
    "    fragWorldPos = vec3(matModel*vec4(p, 1.0));\n"
    "    gl_Position = mvp*vec4(p, 1.0);\n"
    "}\n";
static const char *enemyFS =
    "#version 330\n"
    "in vec4 fragColor;\n"
    "in vec3 fragNormal;\n"
    "in vec3 fragWorldPos;\n"
    "in vec3 fragBary;\n"
    "uniform vec3 viewPos;\n"
    "uniform float rim;\n"
    "uniform float lineWidth;\n"
    "out vec4 finalColor;\n"
    "void main()\n"
    "{\n"
    "    vec3 d = fwidth(fragBary);\n"
    "    vec3 a3 = smoothstep(vec3(0.0), d*lineWidth, fragBary);\n"
    "    float edge = 1.0 - min(min(a3.x, a3.y), a3.z);\n"
    "    vec3 V = normalize(viewPos - fragWorldPos);\n"
    "    float fres = pow(1.0 - clamp(dot(V, normalize(fragNormal)), 0.0, 1.0), 2.5);\n"
    "    vec3 body = mix(vec3(0.05, 0.05, 0.09), fragColor.rgb, fres*rim);\n"
    "    finalColor = vec4(mix(body, fragColor.rgb, edge), 1.0);\n"
    "}\n";
static const char *enemyOutlineFS =
    "#version 330\n"
    "in vec4 fragColor;\n"
    "in vec3 fragNormal;\n"
    "in vec3 fragWorldPos;\n"
    "in vec3 fragBary;\n"
    "uniform float time;\n"
    "uniform float alpha;\n"
    "uniform float hexed;\n"
    "out vec4 finalColor;\n"
    "void main()\n"
    "{\n"
    "    float pulse = 0.85 + 0.15*sin(time*2.6);\n"
    "    vec3 hull = mix(vec3(1.0, 0.08, 0.06), vec3(0.72, 0.39, 0.92), hexed);\n"
    "    finalColor = vec4(hull*pulse, alpha);\n"
    "}\n";
#endif

typedef struct EnemyShaderLocs
{
    int time;
    int yaw;
    int spikeLen;
    int spikeDensity;
    int pulseSpeed;
    int breatheAmp;
    int breatheSpeed;
    int viewPos;
    int rim;
    int lineWidth;
    int expand;
    int alpha;
    int hexed;
} EnemyShaderLocs;

static Shader enemyShader = {0};
static Shader enemyOutlineShader = {0};
static EnemyShaderLocs enemyLocs = {0};
static EnemyShaderLocs enemyOutlineLocs = {0};
static Model enemyModel = {0};
static bool enemyModelLoaded = false;
static float enemySize = 0.55f;         // Body radius, world units
static float enemySpikeLen = 0.35f;     // Spike reach past the body
static float enemySpikeDensity = 0.45f; // Fraction of vertices that spike
static float enemyRim = 0.8f;           // Pastel fresnel bleed on the fill
static float enemyLineWidth = 2.0f;     // Wire line width, screen px
static float enemyOutline = 0.05f;      // L4D red hull thickness, 0 = off

// Card shell: the translucent crystal skin every placed card wears, drawn as
// the cell's generated hex-frustum mesh. One fragment shader does both
// looks: cardValue drives a level color ramp (1 cyan -> 2 violet -> 3 gold)
// with a fresnel rim carrying most of the alpha, and the danger uniform
// crossfades the whole skin to a red pulse when EnemyAIUpdate telegraphs a
// strike on this card. Alpha blended over the gem, so both keep showing
#if defined(PLATFORM_WEB)
static const char *cardShellVS =
    "#version 100\n"
    "attribute vec3 vertexPosition;\n"
    "attribute vec3 vertexNormal;\n"
    "uniform mat4 mvp;\n"
    "uniform mat4 matModel;\n"
    "varying vec3 fragNormal;\n"
    "varying vec3 fragWorldPos;\n"
    "void main()\n"
    "{\n"
    "    fragNormal = normalize(vec3(matModel*vec4(vertexNormal, 0.0)));\n"
    "    fragWorldPos = vec3(matModel*vec4(vertexPosition, 1.0));\n"
    "    gl_Position = mvp*vec4(vertexPosition, 1.0);\n"
    "}\n";
static const char *cardShellFS =
    "#version 100\n"
    "precision mediump float;\n"
    "varying vec3 fragNormal;\n"
    "varying vec3 fragWorldPos;\n"
    "uniform vec3 viewPos;\n"
    "uniform float cardValue;\n"
    "uniform float danger;\n"
    "uniform float time;\n"
    "void main()\n"
    "{\n"
    "    vec3 lvl1 = vec3(0.30, 0.75, 0.95);\n"
    "    vec3 lvl2 = vec3(0.65, 0.40, 0.95);\n"
    "    vec3 lvl3 = vec3(1.00, 0.80, 0.30);\n"
    "    float t = clamp((cardValue - 1.0)*0.5, 0.0, 1.0);\n"
    "    vec3 base = mix(mix(lvl1, lvl2, min(t*2.0, 1.0)), lvl3, max(t*2.0 - 1.0, 0.0));\n"
    "    vec3 V = normalize(viewPos - fragWorldPos);\n"
    "    float fres = pow(1.0 - clamp(dot(V, normalize(fragNormal)), 0.0, 1.0), 2.0);\n"
    "    float alpha = 0.15 + 0.5*fres;\n"
    "    float pulse = 0.5 + 0.5*sin(time*6.0);\n"
    "    base = mix(base, vec3(1.0, 0.10, 0.07), danger);\n"
    "    alpha = mix(alpha, 0.25 + 0.55*pulse, danger);\n"
    "    gl_FragColor = vec4(base, alpha);\n"
    "}\n";
#else
static const char *cardShellVS =
    "#version 330\n"
    "in vec3 vertexPosition;\n"
    "in vec3 vertexNormal;\n"
    "uniform mat4 mvp;\n"
    "uniform mat4 matModel;\n"
    "out vec3 fragNormal;\n"
    "out vec3 fragWorldPos;\n"
    "void main()\n"
    "{\n"
    "    fragNormal = normalize(vec3(matModel*vec4(vertexNormal, 0.0)));\n"
    "    fragWorldPos = vec3(matModel*vec4(vertexPosition, 1.0));\n"
    "    gl_Position = mvp*vec4(vertexPosition, 1.0);\n"
    "}\n";
static const char *cardShellFS =
    "#version 330\n"
    "in vec3 fragNormal;\n"
    "in vec3 fragWorldPos;\n"
    "uniform vec3 viewPos;\n"
    "uniform float cardValue;\n"
    "uniform float danger;\n"
    "uniform float time;\n"
    "out vec4 finalColor;\n"
    "void main()\n"
    "{\n"
    "    vec3 lvl1 = vec3(0.30, 0.75, 0.95);\n"
    "    vec3 lvl2 = vec3(0.65, 0.40, 0.95);\n"
    "    vec3 lvl3 = vec3(1.00, 0.80, 0.30);\n"
    "    float t = clamp((cardValue - 1.0)*0.5, 0.0, 1.0);\n"
    "    vec3 base = mix(mix(lvl1, lvl2, min(t*2.0, 1.0)), lvl3, max(t*2.0 - 1.0, 0.0));\n"
    "    vec3 V = normalize(viewPos - fragWorldPos);\n"
    "    float fres = pow(1.0 - clamp(dot(V, normalize(fragNormal)), 0.0, 1.0), 2.0);\n"
    "    float alpha = 0.15 + 0.5*fres;\n"
    "    float pulse = 0.5 + 0.5*sin(time*6.0);\n"
    "    base = mix(base, vec3(1.0, 0.10, 0.07), danger);\n"
    "    alpha = mix(alpha, 0.25 + 0.55*pulse, danger);\n"
    "    finalColor = vec4(base, alpha);\n"
    "}\n";
#endif

static Shader cardShellShader = {0};
static Material cardShellMaterial = {0};
static int cardShellViewPosLoc = -1;
static int cardShellValueLoc = -1;
static int cardShellDangerLoc = -1;
static int cardShellTimeLoc = -1;
static float cardShellRadius = 0.98f; // Shell outer radius, fraction of HEX_SIZE
static float cardShellHeight = 0.6f;  // Shell height above the tile top

// Sound effects: tiny synthesized blips, no audio assets. Each SFX is a sine
// sweep with a percussive decay, optionally crossfaded with white noise for
// hits, rendered once into a Sound at startup by SynthSound
static Sound sndPickup = {0}; // Grabbing a card or the player
static Sound sndPlace = {0};  // Committing a card/player onto a tile
static Sound sndMerge = {0};  // 2048 merge: rising chime
static Sound sndReturn = {0}; // A card's field lifetime expiring
static Sound sndHit = {0};    // Enemy smashes a card / hits the player
static Sound sndLever = {0};  // The turn lever firing
static Sound sndBoom = {0};   // Fireball detonation

static Sound SynthSound(float startFreq, float endFreq, float duration, float noiseMix, float volume)
{
    const int sampleRate = 22050;
    int frameCount = (int)(duration * (float)sampleRate);
    short *samples = (short *)MemAlloc((unsigned int)((size_t)frameCount * sizeof(short)));

    float phase = 0.0f;
    unsigned int rng = 0x9E3779B9u;
    for (int i = 0; i < frameCount; i++)
    {
        float t = (float)i / (float)frameCount;
        float freq = startFreq + (endFreq - startFreq) * t;
        phase += 2.0f * PI * freq / (float)sampleRate;
        rng = rng * 1664525u + 1013904223u; // LCG white noise
        float noise = ((float)(rng >> 16) / 32767.5f) - 1.0f;
        float env = expf(-5.0f * t) * (1.0f - t);                            // Percussive decay, clickless end
        float attack = fminf(1.0f, (float)i / (0.002f * (float)sampleRate)); // ~2ms ramp, clickless start
        float s = (sinf(phase) * (1.0f - noiseMix) + noise * noiseMix) * env * attack * volume;
        if (s > 1.0f) s = 1.0f;
        if (s < -1.0f) s = -1.0f;
        samples[i] = (short)(s * 32767.0f);
    }

    Wave wave = {.frameCount = (unsigned int)frameCount, .sampleRate = sampleRate, .sampleSize = 16, .channels = 1, .data = samples};
    Sound sound = LoadSoundFromWave(wave); // Copies the samples into the audio buffer
    MemFree(samples);
    return sound;
}

// Leyline tweakables + the layer the network renders on (see the leyline
// helpers further down for the color/arc math)
static RenderTexture2D leylineTexture = {0};
static float leyHue = 0.52f;  // Film center on the color wheel, 0..1
static float leyIrid = 0.35f; // Hue spread along a beam, 0 = flat color
static float leyInk = 2.0f;   // Pen outline width in screen px, 0 = off
static float leyArc = 0.22f;  // Beam arc height
static float leyWidth = 3.0f; // Beam line width in px

static EnemyShaderLocs GetEnemyShaderLocs(Shader shader)
{
    EnemyShaderLocs locs = {
        .time = GetShaderLocation(shader, "time"),
        .yaw = GetShaderLocation(shader, "yaw"),
        .spikeLen = GetShaderLocation(shader, "spikeLen"),
        .spikeDensity = GetShaderLocation(shader, "spikeDensity"),
        .pulseSpeed = GetShaderLocation(shader, "pulseSpeed"),
        .breatheAmp = GetShaderLocation(shader, "breatheAmp"),
        .breatheSpeed = GetShaderLocation(shader, "breatheSpeed"),
        .viewPos = GetShaderLocation(shader, "viewPos"),
        .rim = GetShaderLocation(shader, "rim"),
        .lineWidth = GetShaderLocation(shader, "lineWidth"),
        .expand = GetShaderLocation(shader, "expand"),
        .alpha = GetShaderLocation(shader, "alpha"),
        .hexed = GetShaderLocation(shader, "hexed"),
    };
    return locs;
}

// Gem materials: any mesh drawn with gemMaterial or inverseGemMaterial renders
// as screen-space refractive crystal -- the shader bends the view ray against
// the mesh normals and samples the scene texture (the aurora sky), with
// chromatic dispersion and a fresnel rim. The inverse variant transmits the
// negative. Same FS for both; invertColors is baked per shader at load.
#if defined(PLATFORM_WEB)
static const char *gemVS =
    "#version 100\n"
    "attribute vec3 vertexPosition;\n"
    "attribute vec3 vertexNormal;\n"
    "uniform mat4 mvp;\n"
    "uniform mat4 matModel;\n"
    "uniform mat4 matNormal;\n"
    "varying vec3 fragPosition;\n"
    "varying vec3 fragNormal;\n"
    "void main()\n"
    "{\n"
    "    fragPosition = vec3(matModel*vec4(vertexPosition, 1.0));\n"
    "    fragNormal = normalize(vec3(matNormal*vec4(vertexNormal, 0.0)));\n"
    "    gl_Position = mvp*vec4(vertexPosition, 1.0);\n"
    "}\n";
static const char *gemFS =
    "#version 100\n"
    "precision mediump float;\n"
    "varying vec3 fragPosition;\n"
    "varying vec3 fragNormal;\n"
    "uniform sampler2D texture0;\n"
    "uniform vec3 viewPos;\n"
    "uniform vec3 camRight;\n"
    "uniform vec3 camUp;\n"
    "uniform vec2 resolution;\n"
    "uniform float ior;\n"
    "uniform float strength;\n"
    "uniform float chromatic;\n"
    "uniform float milkiness;\n"
    "uniform int invertColors;\n"
    "vec3 refrSample(vec2 baseUV, vec3 I, vec3 N, float eta)\n"
    "{\n"
    "    vec3 R = refract(I, N, eta);\n"
    "    vec2 off = vec2(dot(R, camRight), dot(R, camUp))*strength;\n"
    "    return texture2D(texture0, clamp(baseUV + off, vec2(0.002), vec2(0.998))).rgb;\n"
    "}\n"
    "void main()\n"
    "{\n"
    "    vec3 N = normalize(fragNormal);\n"
    "    vec3 I = normalize(fragPosition - viewPos);\n"
    "    if (dot(N, I) > 0.0) N = -N;\n"
    "    vec2 baseUV = gl_FragCoord.xy/resolution;\n"
    "    float eta = 1.0/ior;\n"
    "    vec3 col;\n"
    "    col.r = refrSample(baseUV, I, N, eta*(1.0 - chromatic)).r;\n"
    "    col.g = refrSample(baseUV, I, N, eta).g;\n"
    "    col.b = refrSample(baseUV, I, N, eta*(1.0 + chromatic)).b;\n"
    "    if (invertColors == 1) col = vec3(1.0) - col;\n"
    "    col = mix(col, vec3(0.82, 0.86, 0.95), milkiness);\n"
    "    float fres = pow(1.0 - max(dot(-I, N), 0.0), 4.0);\n"
    "    col += vec3(0.85, 0.9, 1.0)*fres*0.5;\n"
    "    gl_FragColor = vec4(col, 1.0);\n"
    "}\n";
#else
static const char *gemVS =
    "#version 330\n"
    "in vec3 vertexPosition;\n"
    "in vec3 vertexNormal;\n"
    "uniform mat4 mvp;\n"
    "uniform mat4 matModel;\n"
    "uniform mat4 matNormal;\n"
    "out vec3 fragPosition;\n"
    "out vec3 fragNormal;\n"
    "void main()\n"
    "{\n"
    "    fragPosition = vec3(matModel*vec4(vertexPosition, 1.0));\n"
    "    fragNormal = normalize(vec3(matNormal*vec4(vertexNormal, 0.0)));\n"
    "    gl_Position = mvp*vec4(vertexPosition, 1.0);\n"
    "}\n";
static const char *gemFS =
    "#version 330\n"
    "in vec3 fragPosition;\n"
    "in vec3 fragNormal;\n"
    "uniform sampler2D texture0;\n"
    "uniform vec3 viewPos;\n"
    "uniform vec3 camRight;\n"
    "uniform vec3 camUp;\n"
    "uniform vec2 resolution;\n"
    "uniform float ior;\n"
    "uniform float strength;\n"
    "uniform float chromatic;\n"
    "uniform float milkiness;\n"
    "uniform int invertColors;\n"
    "out vec4 finalColor;\n"
    "vec3 refrSample(vec2 baseUV, vec3 I, vec3 N, float eta)\n"
    "{\n"
    "    vec3 R = refract(I, N, eta);\n"
    "    vec2 off = vec2(dot(R, camRight), dot(R, camUp))*strength;\n"
    "    return texture(texture0, clamp(baseUV + off, vec2(0.002), vec2(0.998))).rgb;\n"
    "}\n"
    "void main()\n"
    "{\n"
    "    vec3 N = normalize(fragNormal);\n"
    "    vec3 I = normalize(fragPosition - viewPos);\n"
    "    if (dot(N, I) > 0.0) N = -N;\n"
    "    vec2 baseUV = gl_FragCoord.xy/resolution;\n"
    "    float eta = 1.0/ior;\n"
    "    vec3 col;\n"
    "    col.r = refrSample(baseUV, I, N, eta*(1.0 - chromatic)).r;\n"
    "    col.g = refrSample(baseUV, I, N, eta).g;\n"
    "    col.b = refrSample(baseUV, I, N, eta*(1.0 + chromatic)).b;\n"
    "    if (invertColors == 1) col = vec3(1.0) - col;\n"
    "    col = mix(col, vec3(0.82, 0.86, 0.95), milkiness);\n"
    "    float fres = pow(1.0 - max(dot(-I, N), 0.0), 4.0);\n"
    "    col += vec3(0.85, 0.9, 1.0)*fres*0.5;\n"
    "    finalColor = vec4(col, 1.0);\n"
    "}\n";
#endif

// Cached uniform locations for one gem shader instance
typedef struct GemShaderLocs
{
    int viewPos;
    int camRight;
    int camUp;
    int resolution;
    int ior;
    int strength;
    int chromatic;
    int milkiness;
} GemShaderLocs;

static Shader gemShader = {0};        // invertColors = 0
static Shader inverseGemShader = {0}; // invertColors = 1
static GemShaderLocs gemLocs = {0};
static GemShaderLocs inverseGemLocs = {0};
static Material gemMaterial = {0};         // Draw any mesh with this = crystal
static Material inverseGemMaterial = {0};  // ... = crystal transmitting the negative
static Mesh hexPrismMesh = {0};            // 6-slice cylinder: the tile prism, with normals
static RenderTexture2D sceneTexture = {0}; // What the tile gems refract (the aurora sky)
static RenderTexture2D worldTexture = {0}; // The rendered world; what the air shards refract

// Gem optics tweakers, exposed in the hex debug ImGui window
static float gemIor = 1.45f;
static float gemStrength = 0.35f;
static float gemChromatic = 0.05f;
static float gemMilkiness = 0.15f;      // Inverse gem (tile) milkiness, 0 = clear
static float leverGemMilkiness = 0.35f; // Turn button crystal milkiness at rest
static float leverGemMilkyNow = 0.35f;  // Eased milkiness this frame: clouds up while hovered

static float tileArtScale = 1.0f; // Tile-top art size, relative to the tile (debug tweakable)

static float mouseSwayAmount = 1.0f; // Camera lean toward the cursor, 0 = off (debug tweakable)

// Air particles, ported from the python refraction demo: crystal shards drawn
// with the gem material (they refract the aurora sky) and flat ReFantazio-ish
// triangles that bloom in a post pass. Spawned in the default camera's view,
// with a fifth of them big foreground pieces floating over the board.
#define MAX_AIR_SHARDS 40
#define MAX_AIR_TRIS 90
#define SHARD_MODEL_COUNT 3

typedef struct AirParticle
{
    Vector3 base; // Rest position in the air
    Vector3 off;  // Eased mouse-dodge offset
    Vector3 axis; // Tumble axis
    float phase;  // Per-particle time offset
    float bob;    // Bob amplitude
    float speed;  // Bob speed
    float spin;   // Tumble rate, degrees/second
    float scale;  // World size
    int variant;  // Which shard model (shards) / palette color (triangles)
} AirParticle;

static AirParticle airShards[MAX_AIR_SHARDS] = {0};
static AirParticle airTris[MAX_AIR_TRIS] = {0};
static Model shardModels[SHARD_MODEL_COUNT] = {0};
static int shardModelCount = 0;

// Selective bloom for the triangles: they render alone into a half-res glow
// buffer, get a separable gaussian blur, and composite additively on top
static RenderTexture2D glowTexture = {0};
static RenderTexture2D blurTexture = {0};
static Shader blurShader = {0};
static int blurDirLoc = -1;

#if defined(PLATFORM_WEB)
static const char *blurFS =
    "#version 100\n"
    "precision mediump float;\n"
    "varying vec2 fragTexCoord;\n"
    "varying vec4 fragColor;\n"
    "uniform sampler2D texture0;\n"
    "uniform vec2 dir;\n"
    "void main()\n"
    "{\n"
    "    vec3 c = texture2D(texture0, fragTexCoord).rgb*0.227027;\n"
    "    c += texture2D(texture0, fragTexCoord + dir*1.0).rgb*0.194594;\n"
    "    c += texture2D(texture0, fragTexCoord - dir*1.0).rgb*0.194594;\n"
    "    c += texture2D(texture0, fragTexCoord + dir*2.0).rgb*0.121621;\n"
    "    c += texture2D(texture0, fragTexCoord - dir*2.0).rgb*0.121621;\n"
    "    c += texture2D(texture0, fragTexCoord + dir*3.0).rgb*0.054054;\n"
    "    c += texture2D(texture0, fragTexCoord - dir*3.0).rgb*0.054054;\n"
    "    c += texture2D(texture0, fragTexCoord + dir*4.0).rgb*0.016216;\n"
    "    c += texture2D(texture0, fragTexCoord - dir*4.0).rgb*0.016216;\n"
    "    gl_FragColor = vec4(c, 1.0)*fragColor;\n"
    "}\n";
#else
static const char *blurFS =
    "#version 330\n"
    "in vec2 fragTexCoord;\n"
    "in vec4 fragColor;\n"
    "uniform sampler2D texture0;\n"
    "uniform vec2 dir;\n"
    "out vec4 finalColor;\n"
    "void main()\n"
    "{\n"
    "    float w[5] = float[](0.227027, 0.194594, 0.121621, 0.054054, 0.016216);\n"
    "    vec3 c = texture(texture0, fragTexCoord).rgb*w[0];\n"
    "    for (int i = 1; i < 5; i++)\n"
    "    {\n"
    "        c += texture(texture0, fragTexCoord + dir*float(i)).rgb*w[i];\n"
    "        c += texture(texture0, fragTexCoord - dir*float(i)).rgb*w[i];\n"
    "    }\n"
    "    finalColor = vec4(c, 1.0)*fragColor;\n"
    "}\n";
#endif

// The big crystal hovering at board center, straight from the python demo
static Model centerGemModel = {0};
static bool centerGemLoaded = false;
static float centerGemScale = 1.2f; // Debug tweakable

// Air particle tweakers, exposed in the hex debug ImGui window
static float airShardCount = 24.0f; // How many shards are active
static float airTriCount = 18.0f;   // How many glow triangles are active
static float airParticleScale = 1.0f;
static float triGlowStrength = 0.8f;

// Impact light-burst tweakers, exposed in the hex debug ImGui window
static float impactLightSize = 1.7f;   // Half-size of the burst quad, in hex sizes
static float impactLightHeight = 0.3f; // Quad height above the tile base
static float impactAnimSpeed = 1.0f;   // Playback speed of the impact animation

// Card model tweakers, exposed in the hex debug ImGui window
static float cardModelScale = 0.8f;    // Hand card model scale
static float hexModelScale = 0.95f;    // Hex form scale, relative to a tile
static float cardPressTiltDeg = 17.0f; // Max hover press tilt on hand cards

// Size tweakers, exposed in the hex debug ImGui window
static float playerSpriteSize = 1.4f; // Player billboard height in world units
static float cardScale = 1.0f;        // Card rectangle scale (fan spacing follows)
static float reticleScale = 0.85f;    // Reticle half-size at rest, frames one tile
static float leverScale = 2.0f;       // Turn lever art scale (track is 128px long at 1x)

//----------------------------------------------------------------------------------
// Tweak persistence: every debug-tweakable global lives in this table, loads
// from resources/tweaks.json at startup, and the ImGui save button writes it
// back -- to the runtime copy AND to src/resources, so a rebuild's resource
// copy does not clobber tuned values (and the file stays committable).
//----------------------------------------------------------------------------------
#define TWEAKS_FILE "resources/tweaks.json"
#define TWEAKS_SOURCE_FILE "../../src/resources/tweaks.json"

typedef struct Tweak
{
    const char *name;
    float *value;
} Tweak;

static const Tweak tweaks[] = {
    {"impact_size", &impactLightSize},
    {"impact_height", &impactLightHeight},
    {"impact_anim_speed", &impactAnimSpeed},
    {"card_model_scale", &cardModelScale},
    {"hex_model_scale", &hexModelScale},
    {"press_tilt_deg", &cardPressTiltDeg},
    {"player_size", &playerSpriteSize},
    {"card_scale", &cardScale},
    {"reticle_scale", &reticleScale},
    {"lever_scale", &leverScale},
    {"aurora_intensity", &auroraIntensity},
    {"water_wave_amp", &waterWaveAmp},
    {"water_wave_scale", &waterWaveScale},
    {"enemy_size", &enemySize},
    {"enemy_spike_len", &enemySpikeLen},
    {"enemy_spike_density", &enemySpikeDensity},
    {"enemy_rim", &enemyRim},
    {"enemy_line_width", &enemyLineWidth},
    {"enemy_outline", &enemyOutline},
    {"ley_hue", &leyHue},
    {"ley_irid", &leyIrid},
    {"ley_ink", &leyInk},
    {"ley_arc", &leyArc},
    {"ley_width", &leyWidth},
    {"gem_ior", &gemIor},
    {"gem_strength", &gemStrength},
    {"gem_chromatic", &gemChromatic},
    {"gem_milkiness", &gemMilkiness},
    {"lever_gem_milkiness", &leverGemMilkiness},
    {"tile_art_scale", &tileArtScale},
    {"mouse_sway", &mouseSwayAmount},
    {"air_shards", &airShardCount},
    {"air_tris", &airTriCount},
    {"air_particle_scale", &airParticleScale},
    {"tri_glow", &triGlowStrength},
    {"center_gem_scale", &centerGemScale},
};
#define TWEAK_COUNT (int)(sizeof(tweaks) / sizeof(tweaks[0]))

// Populate the tweakable globals from the JSON; keys missing from the file
// keep their compiled defaults
static void LoadTweaks(void)
{
    char *text = LoadFileText(TWEAKS_FILE);
    if (text == NULL) return;

    for (int i = 0; i < TWEAK_COUNT; i++)
    {
        const char *at = strstr(text, TextFormat("\"%s\"", tweaks[i].name));
        if (at == NULL) continue;
        const char *colon = strchr(at, ':');
        if (colon != NULL) *tweaks[i].value = strtof(colon + 1, NULL);
    }
    UnloadFileText(text);
    LOG("INFO: TWEAKS: loaded %s\n", TWEAKS_FILE);
}

// Write the current tweak values back out (runtime copy + source copy)
static void SaveTweaks(void)
{
    char json[2048] = {0};
    int length = snprintf(json, sizeof(json), "{\n");
    for (int i = 0; i < TWEAK_COUNT; i++)
    {
        length += snprintf(json + length, sizeof(json) - (size_t)length, "    \"%s\": %.4f%s\n",
                           tweaks[i].name, *tweaks[i].value, (i < TWEAK_COUNT - 1) ? "," : "");
    }
    snprintf(json + length, sizeof(json) - (size_t)length, "}\n");

    SaveFileText(TWEAKS_FILE, json);
    SaveFileText(TWEAKS_SOURCE_FILE, json); // Keep the versioned copy in sync (no-op on web)
    LOG("INFO: TWEAKS: saved %s\n", TWEAKS_FILE);
}

static Entity *entities = NULL;  // Entity pool, one calloc at startup, packed array
static int entityCount = 0;      // Live entities in the pool
static int mouseHexQ = 0;        // Axial column under the mouse this frame
static int mouseHexR = 0;        // Axial row under the mouse this frame
static float mouseBoardX = 0.0f; // Board-plane point under the mouse this frame
static float mouseBoardZ = 0.0f;
static bool mouseOnPlane = false; // False when the mouse ray misses the board plane
static bool uiWantsMouse = false; // True when ImGui is using the mouse; board ignores clicks

static int gameTurn = 0;
static int lastCommittedTurn = 0; // gameTurn the turn-commit pass last ran for
static int playerMovedTurn = -1;  // gameTurn of the player's last committed step

// Game flow: the board simulates (and animates) in every state, but input
// only reaches it while playing; start and game over draw as modal overlays
typedef enum
{
    GAME_STATE_START = 0,
    GAME_STATE_PLAYING,
    GAME_STATE_OVER,
    GAME_STATE_WIN, // All four exodia pieces assembled
} GameState;
static GameState gameState = GAME_STATE_START;

// 2.5D camera: tilted orthographic view down at the board plane (y = 0);
// no perspective foreshortening, so tiles read as a flat iso board
static Camera3D camera = {
    .position = {0.0f, 14.0f, 12.0f},
    .target = {0.0f, 0.0f, 0.0f},
    .up = {0.0f, 1.0f, 0.0f},
    .fovy = 16.0f, // Orthographic: world-space height of the view, sized to fit the board
    .projection = CAMERA_ORTHOGRAPHIC,
};

//----------------------------------------------------------------------------------
// Module Functions Definition
//----------------------------------------------------------------------------------
// Take a free slot from the entity pool, zeroed and tagged with kind
static Entity *EntitySpawn(EntityKind kind)
{
    if (entityCount >= MAX_ENTITIES)
    {
        LOG("WARNING: ENTITY: pool full, cannot spawn kind %d\n", kind);
        return NULL;
    }

    Entity *entity = &entities[entityCount];
    entityCount++;

    memset(entity, 0, sizeof(Entity));
    entity->kind = kind;
    entity->frameCreated = frameCounter;
    return entity;
}

// Remove an entity: swap the last live entity back into its slot (order is not preserved)
static void EntityDespawn(int index)
{
    if ((index < 0) || (index >= entityCount)) return;

    entities[index] = entities[entityCount - 1];
    entities[entityCount - 1].kind = ENTITY_NONE;
    entityCount--;
}

// Convert axial coordinates to a world position on the board plane (pointy-top layout)
static Vector3 HexAxialToWorld(int q, int r)
{
    float sqrt3 = sqrtf(3.0f);
    Vector3 world = {0};
    world.x = HEX_SIZE * (sqrt3 * (float)q + sqrt3 / 2.0f * (float)r);
    world.y = 0.0f;
    world.z = HEX_SIZE * (3.0f / 2.0f) * (float)r;
    return world;
}

// Axial hex distance in tiles between two cells
static int HexDistance(int q1, int r1, int q2, int r2)
{
    int dq = q1 - q2;
    int dr = r1 - r2;
    return (abs(dq) + abs(dr) + abs(dq + dr)) / 2;
}

// True when the axial cell exists on the board (same rule SpawnHexGrid uses)
static bool HexOnBoard(int q, int r)
{
    return (abs(q) <= GRID_RADIUS) && (abs(r) <= GRID_RADIUS) && (abs(q + r) <= GRID_RADIUS);
}

// The center crystal owns its tile: solid to the player, enemies, and card
// placement alike (its gifts drop on the surrounding tiles)
static bool HexTileSolid(int q, int r)
{
    return (q == 0) && (r == 0);
}

// Leyline network, indexed [q + GRID_RADIUS][r + GRID_RADIUS]. ONLY leyline
// tiles conduct (plus the player's tile as the source); placed spell cards
// are endpoints that tap an adjacent live tile but never extend the chain.
// The leyline layer draws from this and the fireball logic charges from it,
// so the picture and the rules cannot disagree
enum
{
    LEY_W = 2 * GRID_RADIUS + 1
};

static void ComputeLeylineNetwork(const Entity *player, bool conductive[LEY_W][LEY_W], bool live[LEY_W][LEY_W])
{
    static const int dirs[6][2] = {{1, 0}, {1, -1}, {0, -1}, {-1, 0}, {-1, 1}, {0, 1}};

    memset(conductive, 0, sizeof(bool) * LEY_W * LEY_W);
    memset(live, 0, sizeof(bool) * LEY_W * LEY_W);

    for (int i = 0; i < entityCount; i++)
    {
        const Entity *cell = &entities[i];
        if ((cell->kind != ENTITY_HEX_CELL) || !cell->isLeyline) continue;
        conductive[cell->q + GRID_RADIUS][cell->r + GRID_RADIUS] = true;
    }
    if (player == NULL) return;
    conductive[player->q + GRID_RADIUS][player->r + GRID_RADIUS] = true;

    // flood from the player: everything reachable is energized
    int stackQ[LEY_W * LEY_W];
    int stackR[LEY_W * LEY_W];
    int top = 0;
    live[player->q + GRID_RADIUS][player->r + GRID_RADIUS] = true;
    stackQ[top] = player->q;
    stackR[top] = player->r;
    top++;
    while (top > 0)
    {
        top--;
        int q = stackQ[top];
        int r = stackR[top];
        for (int d = 0; d < 6; d++)
        {
            int nq = q + dirs[d][0];
            int nr = r + dirs[d][1];
            if (!HexOnBoard(nq, nr)) continue;
            if (!conductive[nq + GRID_RADIUS][nr + GRID_RADIUS]) continue;
            if (live[nq + GRID_RADIUS][nr + GRID_RADIUS]) continue;
            live[nq + GRID_RADIUS][nr + GRID_RADIUS] = true;
            stackQ[top] = nq;
            stackR[top] = nr;
            top++;
        }
    }
}

// True when any neighbour of (q, r) is a live conducting tile: how a placed
// spell card taps the network by adjacency
static bool LeylineTapped(const bool live[LEY_W][LEY_W], int q, int r)
{
    static const int dirs[6][2] = {{1, 0}, {1, -1}, {0, -1}, {-1, 0}, {-1, 1}, {0, 1}};
    for (int d = 0; d < 6; d++)
    {
        int nq = q + dirs[d][0];
        int nr = r + dirs[d][1];
        if (!HexOnBoard(nq, nr)) continue;
        if (live[nq + GRID_RADIUS][nr + GRID_RADIUS]) return true;
    }
    return false;
}

// Convert a screen point to the world position where hand cards stand: along
// the pick ray, on the plane through the camera target. The 2D card passes
// keep working in screen pixels; only the drawing lifts them into the world.
static Vector3 ScreenToHandPlane(Vector2 screenPos)
{
    Ray ray = GetScreenToWorldRay(screenPos, camera);
    float t = Vector3Distance(camera.position, camera.target);
    return Vector3Add(ray.position, Vector3Scale(ray.direction, t));
}

// Resolve one gem shader's uniform locations (called once at load)
static GemShaderLocs GetGemShaderLocs(Shader shader)
{
    GemShaderLocs locs = {
        .viewPos = GetShaderLocation(shader, "viewPos"),
        .camRight = GetShaderLocation(shader, "camRight"),
        .camUp = GetShaderLocation(shader, "camUp"),
        .resolution = GetShaderLocation(shader, "resolution"),
        .ior = GetShaderLocation(shader, "ior"),
        .strength = GetShaderLocation(shader, "strength"),
        .chromatic = GetShaderLocation(shader, "chromatic"),
        .milkiness = GetShaderLocation(shader, "milkiness"),
    };
    return locs;
}

// Push this frame's camera basis + optics into one gem shader
static void UpdateGemShader(Shader shader, const GemShaderLocs *locs, float milkiness)
{
    Vector3 forward = Vector3Normalize(Vector3Subtract(camera.target, camera.position));
    Vector3 right = Vector3Normalize(Vector3CrossProduct(forward, camera.up));
    Vector3 up = Vector3CrossProduct(right, forward);
    float resolution[2] = {(float)screenWidth, (float)screenHeight};

    SetShaderValue(shader, locs->viewPos, &camera.position, SHADER_UNIFORM_VEC3);
    SetShaderValue(shader, locs->camRight, &right, SHADER_UNIFORM_VEC3);
    SetShaderValue(shader, locs->camUp, &up, SHADER_UNIFORM_VEC3);
    SetShaderValue(shader, locs->resolution, resolution, SHADER_UNIFORM_VEC2);
    SetShaderValue(shader, locs->ior, &gemIor, SHADER_UNIFORM_FLOAT);
    SetShaderValue(shader, locs->strength, &gemStrength, SHADER_UNIFORM_FLOAT);
    SetShaderValue(shader, locs->chromatic, &gemChromatic, SHADER_UNIFORM_FLOAT);
    SetShaderValue(shader, locs->milkiness, &milkiness, SHADER_UNIFORM_FLOAT);
}

// ReFantazio-ish triangle palette: royal blues and cyans with white/gold
static const Color airTriColors[4] = {
    {70, 130, 255, 255}, {130, 220, 255, 255}, {255, 255, 255, 255}, {255, 215, 130, 255}};

// Spawn one air particle in the default camera's view; a fifth become big
// foreground pieces hung between the camera and the board
// Low-discrepancy sequence (van der Corput in the given base): successive
// indices land far apart, so particles cover the view evenly instead of
// clumping the way plain uniform randoms do
static float Halton(int index, int base)
{
    float f = 1.0f;
    float r = 0.0f;
    while (index > 0)
    {
        f /= (float)base;
        r += f * (float)(index % base);
        index /= base;
    }
    return r;
}

static AirParticle SpawnAirParticle(bool isTriangle, int seqIndex)
{
    Vector3 fwd = Vector3Normalize(Vector3Subtract(camera.target, camera.position));
    Vector3 right = Vector3Normalize(Vector3CrossProduct(fwd, camera.up));
    Vector3 up = Vector3CrossProduct(right, fwd);
    float halfView = camera.fovy / 2.0f;

    AirParticle p = {0};
    bool foreground = (GetRandomValue(0, 99) < 20);
    // Halton bases 2/3 spread the frustum placement evenly; a little random
    // jitter on top keeps it from reading as a lattice
    float u = -0.85f + Halton(seqIndex + 1, 2) * 1.70f + (float)GetRandomValue(-6, 6) / 100.0f;
    float v = -0.65f + Halton(seqIndex + 1, 3) * 1.45f + (float)GetRandomValue(-6, 6) / 100.0f;
    float depth = foreground ? (float)GetRandomValue(5, 9) : (float)GetRandomValue(12, 26);

    p.base = Vector3Add(camera.position, Vector3Scale(fwd, depth));
    p.base = Vector3Add(p.base, Vector3Scale(right, u * halfView * 0.95f));
    p.base = Vector3Add(p.base, Vector3Scale(up, v * halfView * 0.9f));
    if (p.base.y < 0.5f) p.base.y = 0.5f + (float)GetRandomValue(0, 150) / 100.0f;

    p.axis = Vector3Normalize((Vector3){(float)GetRandomValue(-100, 100) / 100.0f,
                                        (float)GetRandomValue(-100, 100) / 100.0f + 0.01f,
                                        (float)GetRandomValue(-100, 100) / 100.0f});
    p.phase = (float)GetRandomValue(0, 628) / 100.0f;
    p.bob = 0.1f + (float)GetRandomValue(0, 40) / 100.0f;
    p.speed = 0.3f + (float)GetRandomValue(0, 90) / 100.0f;
    p.spin = 15.0f + (float)GetRandomValue(0, 65);
    if (isTriangle)
    {
        p.scale = (foreground ? 0.28f : 0.10f) + (float)GetRandomValue(0, 12) / 100.0f;
        p.variant = GetRandomValue(0, 3);
    }
    else
    {
        p.scale = (foreground ? 0.45f : 0.10f) + (float)GetRandomValue(0, 20) / 100.0f;
        p.variant = GetRandomValue(0, SHARD_MODEL_COUNT - 1);
    }
    return p;
}

// Air particle mouse dodge: near the cursor's board point they ease away,
// then drift back (same feel as the python demo)
static void UpdateAirParticles(AirParticle *parts, int count, float dt)
{
    if (!mouseOnPlane) return;
    Vector3 mouseWorld = {mouseBoardX, 0.8f, mouseBoardZ};
    const float radius = 1.6f;
    for (int i = 0; i < count; i++)
    {
        Vector3 d = Vector3Subtract(Vector3Add(parts[i].base, parts[i].off), mouseWorld);
        float dist = Vector3Length(d);
        Vector3 target = {0};
        if ((dist > 0.001f) && (dist < radius))
        {
            float push = (radius - dist) / radius * 0.5f;
            target = Vector3Scale(d, push / dist);
        }
        float ease = 1.0f - expf(-6.0f * dt);
        parts[i].off = Vector3Add(parts[i].off, Vector3Scale(Vector3Subtract(target, parts[i].off), ease));
    }
}

// Current world position of one air particle (bob + drift + dodge)
static Vector3 AirParticlePosition(const AirParticle *p, float t)
{
    Vector3 pos = p->base;
    pos.x += sinf(t * 0.22f + p->phase) * 0.35f + p->off.x;
    pos.y += sinf(t * p->speed + p->phase) * p->bob + p->off.y;
    pos.z += cosf(t * 0.18f + p->phase * 1.4f) * 0.3f + p->off.z;
    return pos;
}

// The flat glow triangles, drawn wherever the current render pass points
// (the 3D scene for the sharp pass, the glow buffer for the bloom mask)
static void DrawAirTriangles(float t)
{
    rlDisableBackfaceCulling();
    for (int i = 0; i < (int)airTriCount && i < MAX_AIR_TRIS; i++)
    {
        const AirParticle *p = &airTris[i];
        Vector3 pos = AirParticlePosition(p, t);
        float s = p->scale * airParticleScale;

        rlPushMatrix();
        rlTranslatef(pos.x, pos.y, pos.z);
        rlRotatef(t * p->spin + p->phase * 57.3f, p->axis.x, p->axis.y, p->axis.z);
        rlBegin(RL_TRIANGLES);
        Color c = airTriColors[p->variant % 4];
        rlColor4ub(c.r, c.g, c.b, c.a);
        rlVertex3f(0.0f, s, 0.0f);
        rlVertex3f(-s * 0.87f, -s * 0.5f, 0.0f);
        rlVertex3f(s * 0.87f, -s * 0.5f, 0.0f);
        rlEnd();
        rlPopMatrix();
    }
    rlEnableBackfaceCulling();
}

// CRPG camera controls (Baldur's Gate style): WASD pans along the board
// plane, right-drag orbits the target, mouse wheel zooms (orthographic
// camera, so fovy is the zoom)
static void UpdateGameCamera(void)
{
    float dt = GetFrameTime();

    // tweakables
    const float panSpeed = 10.0f;   // world units / second
    const float orbitSens = 0.005f; // radians / pixel of mouse drag

    // WASD pan: slide camera + target together across the ground plane
    Vector3 forward = Vector3Subtract(camera.target, camera.position);
    forward.y = 0.0f;
    forward = Vector3Normalize(forward);
    Vector3 right = Vector3Normalize(Vector3CrossProduct(forward, (Vector3){0.0f, 1.0f, 0.0f}));

    Vector3 move = {0};
    if (IsKeyDown(KEY_W)) move = Vector3Add(move, forward);
    if (IsKeyDown(KEY_S)) move = Vector3Subtract(move, forward);
    if (IsKeyDown(KEY_D)) move = Vector3Add(move, right);
    if (IsKeyDown(KEY_A)) move = Vector3Subtract(move, right);
    if (Vector3Length(move) > 0.0f)
    {
        move = Vector3Scale(Vector3Normalize(move), panSpeed * dt);
        camera.position = Vector3Add(camera.position, move);
        camera.target = Vector3Add(camera.target, move);
    }

    // Right-drag orbit: rotate around the target (yaw + clamped pitch)
    if (IsMouseButtonDown(MOUSE_BUTTON_RIGHT) && !uiWantsMouse)
    {
        Vector2 d = GetMouseDelta();
        Vector3 offset = Vector3Subtract(camera.position, camera.target);
        float radius = Vector3Length(offset);

        float yaw = atan2f(offset.x, offset.z) - d.x * orbitSens;
        float pitch = asinf(offset.y / radius) - d.y * orbitSens;

        // clamp pitch: stay above the board, below straight-down (no pole flip)
        if (pitch < 0.25f) pitch = 0.25f;
        if (pitch > 1.45f) pitch = 1.45f;

        offset.x = radius * cosf(pitch) * sinf(yaw);
        offset.y = radius * sinf(pitch);
        offset.z = radius * cosf(pitch) * cosf(yaw);
        camera.position = Vector3Add(camera.target, offset);
    }

    // Wheel zoom: fovy is the visible world height on this ortho camera
    float wheel = GetMouseWheelMove();
    if ((wheel != 0.0f) && !uiWantsMouse)
    {
        camera.fovy *= 1.0f - wheel * 0.08f;
        if (camera.fovy < 6.0f) camera.fovy = 6.0f;
        if (camera.fovy > 30.0f) camera.fovy = 30.0f;
    }

    // Mouse sway: the camera leans a touch toward the cursor. Eased, and
    // applied as an incremental orbit (only the frame's sway delta), so it
    // rides on top of the controls above without fighting or drifting.
    static Vector2 sway = {0};
    Vector2 mouse = GetMousePosition();
    Vector2 swayTarget = {
        ((mouse.x / (float)screenWidth) * 2.0f - 1.0f) * 0.035f * mouseSwayAmount,
        ((mouse.y / (float)screenHeight) * 2.0f - 1.0f) * 0.022f * mouseSwayAmount,
    };
    float swayDeltaYaw = (swayTarget.x - sway.x) * 0.05f;
    float swayDeltaPitch = (swayTarget.y - sway.y) * 0.05f;
    sway.x += swayDeltaYaw;
    sway.y += swayDeltaPitch;

    Vector3 swayOffset = Vector3Subtract(camera.position, camera.target);
    float swayRadius = Vector3Length(swayOffset);
    float swayYaw = atan2f(swayOffset.x, swayOffset.z) - swayDeltaYaw;
    float swayPitch = asinf(swayOffset.y / swayRadius) - swayDeltaPitch;
    if (swayPitch < 0.2f) swayPitch = 0.2f;
    if (swayPitch > 1.5f) swayPitch = 1.5f;
    swayOffset.x = swayRadius * cosf(swayPitch) * sinf(swayYaw);
    swayOffset.y = swayRadius * sinf(swayPitch);
    swayOffset.z = swayRadius * cosf(swayPitch) * cosf(swayYaw);
    camera.position = Vector3Add(camera.target, swayOffset);
}

// Convert a point on the board plane to axial coordinates, rounded to the nearest cell
static void HexWorldToAxial(float x, float z, int *q, int *r)
{
    float sqrt3 = sqrtf(3.0f);
    float px = x / HEX_SIZE;
    float pz = z / HEX_SIZE;
    float qf = sqrt3 / 3.0f * px - 1.0f / 3.0f * pz;
    float rf = 2.0f / 3.0f * pz;

    // Cube rounding: round each cube coord, then fix the one that drifted most
    float cx = qf;
    float cz = rf;
    float cy = -cx - cz;
    float rx = roundf(cx);
    float ry = roundf(cy);
    float rz = roundf(cz);
    float dx = fabsf(rx - cx);
    float dy = fabsf(ry - cy);
    float dz = fabsf(rz - cz);

    if ((dx > dy) && (dx > dz))
        rx = -ry - rz;
    else if (dy > dz)
        ry = -rx - rz;
    else
        rz = -rx - ry;

    *q = (int)rx;
    *r = (int)rz;
}

static void AdvanceTurn(void)
{
}

// Screen text with a black border, readable over any backdrop: an 8-way
// black ring (thicker for big type) under the colored fill
static void DrawTextOutlined(const char *text, int x, int y, int size, Color color)
{
    int o = (size >= 40) ? 3 : 2;
    for (int oy = -o; oy <= o; oy += o)
    {
        for (int ox = -o; ox <= o; ox += o)
        {
            if ((ox != 0) || (oy != 0)) DrawText(text, x + ox, y + oy, size, Fade(BLACK, (float)color.a / 255.0f));
        }
    }
    DrawText(text, x, y, size, color);
}

// Turn button: the crystal on the right edge commits the turn on click. It
// stays a crystal -- the spinning 3D gem draws at leverKnobScreenPos in its
// own pass -- and hovering it clouds the crystal up (milkiness eases toward
// milky, back to the tweakable base when the mouse leaves)
static void TurnLever(void)
{
    const float cx = 650.0f; // Button center, screen px
    const float cy = 276.0f;
    float hitRadius = 26.0f * leverScale;

    float dt = GetFrameTime();
    if (dt > 0.05f) dt = 0.05f;

    Vector2 mouse = GetMousePosition();
    bool hovered = CheckCollisionPointCircle(mouse, (Vector2){cx, cy}, hitRadius);
    leverWantsMouse = hovered;

    if (hovered && !uiWantsMouse && IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
    {
        gameTurn++;
        shakeTimeLeft = SHAKE_DURATION;
        PlaySound(sndLever);
        LOG("INFO: TURN: button pressed, turn is now %d\n", gameTurn);
    }

    // The hover read: the crystal clouds up, eased both ways
    float milkyTarget = hovered ? (leverGemMilkiness + 0.5f) : leverGemMilkiness;
    if (milkyTarget > 1.0f) milkyTarget = 1.0f;
    leverGemMilkyNow += (milkyTarget - leverGemMilkyNow) * (1.0f - expf(-12.0f * dt));

    // The crystal itself is the spinning 3D gem drawn in its own pass after
    // this one; the flat coin art only shows when the gem model is missing
    leverKnobScreenPos = (Vector2){cx, cy};
    if (!centerGemLoaded)
    {
        DrawTexturePro(leverKnobTexture, (Rectangle){0, 0, 16, 18},
                       (Rectangle){cx, cy, 24.0f * leverScale, 27.0f * leverScale},
                       (Vector2){12.0f * leverScale, 13.5f * leverScale}, 0.0f, WHITE);
    }

    // "next turn" label: yellow with a black outline so it reads on any sky
    DrawTextOutlined("next turn", (int)cx - MeasureText("next turn", 22) / 2, (int)(cy + hitRadius) + 8, 22, (Color){255, 220, 60, 255});
    const char *turnText = TextFormat("turn %d", gameTurn);
    DrawTextOutlined(turnText, (int)cx - MeasureText(turnText, 20) / 2, (int)(cy + hitRadius) + 36, 20, LIGHTGRAY);
}

// Spawn one ENTITY_HEX_CELL per cell of the hexagonal board
// Appends one triangle (positions + one hard face normal for all three
// corners) to a mesh being built; *at is the running vertex index
static void HexMeshPushTri(Mesh *mesh, int *at, Vector3 a, Vector3 b, Vector3 c, Vector3 n)
{
    Vector3 corners[3] = {a, b, c};
    for (int v = 0; v < 3; v++)
    {
        mesh->vertices[(*at) * 3 + 0] = corners[v].x;
        mesh->vertices[(*at) * 3 + 1] = corners[v].y;
        mesh->vertices[(*at) * 3 + 2] = corners[v].z;
        mesh->normals[(*at) * 3 + 0] = n.x;
        mesh->normals[(*at) * 3 + 1] = n.y;
        mesh->normals[(*at) * 3 + 2] = n.z;
        (*at)++;
    }
}

// Generates the card shell: a pointy-top hexagonal frustum (tapered sides,
// flat top, open bottom) sitting on y=0, unwelded so every face keeps a hard
// normal for the shell shader's fresnel. Every cell builds its own at grid
// spawn and carries it in the fat struct
static Mesh GenHexCardMesh(float radius, float height)
{
    const int sides = 6;
    const float topScale = 0.78f; // Top hex narrower than the base

    Mesh mesh = {0};
    mesh.triangleCount = sides * 3; // 2 per side quad + 1 top fan slice
    mesh.vertexCount = mesh.triangleCount * 3;
    mesh.vertices = (float *)MemAlloc((unsigned int)(mesh.vertexCount * 3 * sizeof(float)));
    mesh.normals = (float *)MemAlloc((unsigned int)(mesh.vertexCount * 3 * sizeof(float)));

    int at = 0;
    for (int k = 0; k < sides; k++)
    {
        // Same corner angles as the board hexes (pointy-top on XZ)
        float a0 = DEG2RAD * (60.0f * (float)k + 30.0f);
        float a1 = DEG2RAD * (60.0f * (float)(k + 1) + 30.0f);
        Vector3 b0 = {cosf(a0) * radius, 0.0f, sinf(a0) * radius};
        Vector3 b1 = {cosf(a1) * radius, 0.0f, sinf(a1) * radius};
        Vector3 t0 = {cosf(a0) * radius * topScale, height, sinf(a0) * radius * topScale};
        Vector3 t1 = {cosf(a1) * radius * topScale, height, sinf(a1) * radius * topScale};

        // Side normal from the face itself, flipped outward if the winding
        // put it inward (robust against angle-direction mistakes)
        Vector3 n = Vector3Normalize(Vector3CrossProduct(Vector3Subtract(t0, b0), Vector3Subtract(b1, b0)));
        Vector3 outward = {cosf((a0 + a1) / 2.0f), 0.0f, sinf((a0 + a1) / 2.0f)};
        if (Vector3DotProduct(n, outward) < 0.0f) n = Vector3Negate(n);

        HexMeshPushTri(&mesh, &at, b0, t0, t1, n);
        HexMeshPushTri(&mesh, &at, b0, t1, b1, n);
        HexMeshPushTri(&mesh, &at, (Vector3){0.0f, height, 0.0f}, t0, t1, (Vector3){0.0f, 1.0f, 0.0f});
    }

    UploadMesh(&mesh, false);
    return mesh;
}

static void SpawnHexGrid(void)
{
    for (int q = -GRID_RADIUS; q <= GRID_RADIUS; q++)
    {
        for (int r = -GRID_RADIUS; r <= GRID_RADIUS; r++)
        {
            if (abs(q + r) > GRID_RADIUS) continue;

            Entity *cell = EntitySpawn(ENTITY_HEX_CELL);
            if (cell == NULL) return;

            cell->q = q;
            cell->r = r;
            cell->position = HexAxialToWorld(q, r);
            cell->radius = HEX_SIZE;
            cell->tint = LIGHTGRAY;
            cell->cardShellMesh = GenHexCardMesh(HEX_SIZE * cardShellRadius, cardShellHeight);
        }
    }
}

// Reticle step: integrate the hover reticle's spring/scale/spin toward the
// hovered tile (NULL hides it; the next hover re-snaps in place). Movement is
// an under-damped spring (overshoots and rebounds, not a lerp), scale follows
// spring speed, spin follows 1/scale^2 like a skater pulling their arms in.
static void UpdateHoverReticle(const Entity *hoveredCell)
{
    const float stiffness = 90.0f;         // Spring pull toward the tile center (1/s^2)
    const float damping = 6.0f;            // Well below critical (2*sqrtf(90) ~= 19): magnetic overshoot
    float scaleRest = reticleScale;        // Half-size at rest, frames one tile (debug tweakable)
    float scaleMax = reticleScale + 0.75f; // Half-size cap while moving fast
    const float scalePerSpeed = 0.12f;     // Extra half-size per world-unit/s of spring speed
    const float shrinkRate = 16.0f;        // Fast: snaps down onto the tile
    const float growRate = 5.0f;           // Slow: swells out smoothly when it starts moving
    const float spinFactor = 4.5f;         // Angular velocity = spinFactor/scale^2 (rad/s)
    const float floatHeight = 0.25f;       // Ride height above the tile top
    const float bobAmplitude = 0.06f;      // Slow sine bob around the ride height
    const float bobSpeed = 3.0f;           // Bob phase speed (rad/s)

    if (hoveredCell == NULL)
    {
        reticle.active = false; // Next hover re-snaps in place instead of flying across the board
        return;
    }

    float dt = GetFrameTime();
    if (dt > 0.05f) dt = 0.05f; // Clamp frame hitches so the spring cannot explode

    Vector3 target = hoveredCell->position;

    if (!reticle.active)
    {
        // Fresh hover: appear big over the tile and let the shrink snap it down
        reticle.position = target;
        reticle.velocity = (Vector3){0};
        reticle.scale = scaleMax;
        reticle.active = true;
    }

    // Under-damped spring toward the tile center, on the board plane (XZ)
    reticle.velocity.x += ((target.x - reticle.position.x) * stiffness - reticle.velocity.x * damping) * dt;
    reticle.velocity.z += ((target.z - reticle.position.z) * stiffness - reticle.velocity.z * damping) * dt;
    reticle.position.x += reticle.velocity.x * dt;
    reticle.position.z += reticle.velocity.z * dt;

    // Scale follows spring speed: big while moving fast, small at rest,
    // shrinking much faster than it grows
    float speed = sqrtf(reticle.velocity.x * reticle.velocity.x + reticle.velocity.z * reticle.velocity.z);
    float scaleTarget = scaleRest + speed * scalePerSpeed;
    if (scaleTarget > scaleMax) scaleTarget = scaleMax;
    float scaleRate = (scaleTarget < reticle.scale) ? shrinkRate : growRate;
    reticle.scale += (scaleTarget - reticle.scale) * (1.0f - expf(-scaleRate * dt));

    // Spin: drifts lazily while big, whips around fast once shrunk
    reticle.spinAngle += (spinFactor / (reticle.scale * reticle.scale)) * dt;
    if (reticle.spinAngle > 2.0f * PI) reticle.spinAngle -= 2.0f * PI;

    // Float above whatever height the tile is currently drawn at
    reticle.bobPhase += bobSpeed * dt;
    if (reticle.bobPhase > 2.0f * PI) reticle.bobPhase -= 2.0f * PI;
    reticle.position.y = HEX_TILE_HEIGHT_HOVER + floatHeight + sinf(reticle.bobPhase) * bobAmplitude;
}

// Reticle draw: a textured quad spinning flat on the board plane; in true 3D
// the camera projection replaces the "iso squash" of a 2D layout
static void DrawHoverReticle(void)
{
    if (!reticle.active) return;

    rlPushMatrix();
    rlTranslatef(reticle.position.x, reticle.position.y, reticle.position.z);
    rlRotatef(reticle.spinAngle * RAD2DEG, 0.0f, 1.0f, 0.0f);

    float s = reticle.scale;
    rlSetTexture(reticleTexture.id);
    rlBegin(RL_QUADS);
    rlColor4ub(255, 255, 255, 255);
    rlNormal3f(0.0f, 1.0f, 0.0f); // Counter-clockwise seen from above, so the face points up
    rlTexCoord2f(0.0f, 0.0f);
    rlVertex3f(-s, 0.0f, -s);
    rlTexCoord2f(0.0f, 1.0f);
    rlVertex3f(-s, 0.0f, s);
    rlTexCoord2f(1.0f, 1.0f);
    rlVertex3f(s, 0.0f, s);
    rlTexCoord2f(1.0f, 0.0f);
    rlVertex3f(s, 0.0f, -s);
    rlEnd();
    rlSetTexture(0);

    rlPopMatrix();
}

// Impact step: age out the ring/dust of the last card placement
static void UpdateImpactEffect(void)
{
    if (!impact.active) return;

    float dt = GetFrameTime();
    if (dt > 0.05f) dt = 0.05f;
    impact.age += dt * impactAnimSpeed;
    if (impact.age >= IMPACT_DURATION) impact.active = false;
}

// Impact draw: expanding hex rings + dust chips where a card just landed
// (called inside BeginMode3D)
static void DrawImpactEffect(void)
{
    if (!impact.active) return;

    float t = impact.age / IMPACT_DURATION;      // 0 -> 1 over the effect
    float ease = 1.0f - (1.0f - t) * (1.0f - t); // Ease-out: bursts fast, settles soft
    float fade = 1.0f - t;

    // Expanding hex rings hugging the board plane
    float ringRadius = HEX_SIZE * (0.7f + 1.2f * ease);
    DrawCylinderWires(impact.position, ringRadius, ringRadius, 0.05f, 6, Fade(impact.tint, fade));
    DrawCylinderWires(impact.position, ringRadius * 0.8f, ringRadius * 0.8f, 0.05f, 6, Fade(WHITE, fade * 0.7f));

    // Light burst: one frame of the sheet (baked from the itch light_007.mov),
    // played flat on the board over the effect's lifetime, tinted by the placed
    // card's color. Plain alpha blend: additive glow saturates to invisible
    // against this near-white scene
    int frame = (int)(t * (float)IMPACT_LIGHT_FRAMES);
    if (frame >= IMPACT_LIGHT_FRAMES) frame = IMPACT_LIGHT_FRAMES - 1;
    float u0 = (float)frame / (float)IMPACT_LIGHT_FRAMES;
    float u1 = (float)(frame + 1) / (float)IMPACT_LIGHT_FRAMES;

    float half = HEX_SIZE * impactLightSize;              // Burst spills over the tile's neighbours
    float lightY = impact.position.y + impactLightHeight; // Clears the resting tile top

    rlSetTexture(impactLightTexture.id);
    rlBegin(RL_QUADS);
    rlColor4ub(impact.tint.r, impact.tint.g, impact.tint.b, 255);
    rlNormal3f(0.0f, 1.0f, 0.0f); // Counter-clockwise seen from above, so the face points up
    rlTexCoord2f(u0, 0.0f);
    rlVertex3f(impact.position.x - half, lightY, impact.position.z - half);
    rlTexCoord2f(u0, 1.0f);
    rlVertex3f(impact.position.x - half, lightY, impact.position.z + half);
    rlTexCoord2f(u1, 1.0f);
    rlVertex3f(impact.position.x + half, lightY, impact.position.z + half);
    rlTexCoord2f(u1, 0.0f);
    rlVertex3f(impact.position.x + half, lightY, impact.position.z - half);
    rlEnd();
    rlSetTexture(0);

    // Dust: six chips thrown outward on a small hop, shrinking as they fade
    for (int i = 0; i < 6; i++)
    {
        float angle = ((float)i + 0.5f) * (2.0f * PI / 6.0f);
        float dist = HEX_SIZE * (0.4f + 1.0f * ease);
        float hop = 0.6f * ease * (1.0f - ease) * 4.0f; // Parabola: up then back down
        Vector3 dust = {
            impact.position.x + cosf(angle) * dist,
            impact.position.y + 0.1f + hop,
            impact.position.z + sinf(angle) * dist,
        };
        float dustSize = 0.12f * fade;
        DrawCube(dust, dustSize, dustSize, dustSize, Fade(impact.tint, fade));
    }
}

// Deal one card into the hand fan, cycling a small test palette
// Spawn one enemy on tile (q, r). Everything instance-specific lives on the
// entity: home tile, pulse/breathe rates (offset per spawn so a crowd never
// throbs in sync), and whether it can be grabbed
static void SpawnEnemy(int q, int r)
{
    static int spawned = 0;

    Entity *enemy = EntitySpawn(ENTITY_ENEMY);
    if (enemy == NULL) return;

    enemy->q = q;
    enemy->r = r;
    enemy->position = HexAxialToWorld(q, r);
    enemy->enemyPulseSpeed = 2.2f + 0.3f * (float)(spawned % 3);
    enemy->enemyBreatheAmp = 0.06f;
    enemy->enemyBreatheSpeed = 1.6f + 0.2f * (float)(spawned % 4);
    enemy->enemyMoveRange = 1;
    enemy->IsDraggable = true;
    spawned++;
}

// Lift a placed card off its tile and back into the hand, kind and level
// intact, springing into the fan from the tile it left. Shared by the
// default turn return and by enemies striking cards off the board. Returns
// the hand card (NULL if the pool is full) so callers can mark it exhausted
static Entity *ReturnPlacedCard(Entity *cell)
{
    Entity *back = EntitySpawn(ENTITY_CARD);
    if (back != NULL)
    {
        back->cardKind = cell->cardKind;
        back->cardLevel = cell->cardLevel;
        back->value = (int)cell->cardLevel;
        back->tint = cardKindTints[cell->cardKind];
        back->cardMode = CARD_REC_FORM;
        back->modelIndex = cell->modelIndex;
        if (back->cardLevel >= CARD_LVL_4)
            snprintf(back->tooltip, sizeof(back->tooltip), "%s -- an exodia piece! collect all four to win",
                     exodiaPieceNames[back->cardKind]);
        else
            snprintf(back->tooltip, sizeof(back->tooltip), "%s -- stays %d turns on the field",
                     cardKindTooltips[back->cardKind], cardKindLifetimes[back->cardKind]);
        Vector2 lift = GetWorldToScreen(cell->position, camera);
        back->position = (Vector3){lift.x, lift.y, 0.0f};
    }

    cell->value = 0;
    cell->cardKind = CARD_NONE;
    cell->cardLevel = 0;
    cell->isLeyline = false;
    cell->inDanger = false; // No card, nothing to strike
    cell->turnLifetime = 0;
    cell->fireballCharge = 0;
    cell->huskTurns = 0;
    return back;
}

// Every slain enemy sheds a card: a pickup left on the tile it died on,
// collected like the crystal's gifts by standing on it. Enemies die to a
// single hit, so every kill site pairs this with the despawn
static void SpawnEnemyDrop(int q, int r)
{
    Entity *drop = EntitySpawn(ENTITY_PICKUP);
    if (drop == NULL) return;
    drop->q = q;
    drop->r = r;
    drop->position = HexAxialToWorld(q, r);
    drop->cardKind = (CardKind)GetRandomValue(CARD_LEYLINE, CARD_WARD);
    drop->tint = cardKindTints[drop->cardKind];
    drop->modelIndex = (kindModelIndex[drop->cardKind] >= 0) ? kindModelIndex[drop->cardKind] : 0;
    LOG("INFO: PICKUP: slain enemy dropped a %s card at (q=%d, r=%d)\n", cardKindNames[drop->cardKind], q, r);
}

// Fireball detonation: a radius-FIREBALL_AOE_RADIUS blast that destroys
// every enemy in reach. The spent emplacement follows the level rule: lvl 1
// leaves a husk blocking the tile for one committed turn before the card
// returns to the hand, lvl 2+ returns immediately
static void DetonateFireball(Entity *cell)
{
    LOG("INFO: FIREBALL: lvl %d detonated at (q=%d, r=%d)\n", (int)cell->cardLevel, cell->q, cell->r);
    impact = (ImpactFx){.position = cell->position, .tint = (Color){255, 160, 70, 255}, .age = 0.0f, .active = true};
    shakeTimeLeft = SHAKE_DURATION;
    PlaySound(sndBoom);

    // Backwards so the swap-back despawn only moves already-visited slots
    for (int i = entityCount - 1; i >= 0; i--)
    {
        const Entity *enemy = &entities[i];
        if (enemy->kind != ENTITY_ENEMY) continue;
        if (HexDistance(cell->q, cell->r, enemy->q, enemy->r) > FIREBALL_AOE_RADIUS) continue;
        LOG("INFO: FIREBALL: enemy at (q=%d, r=%d) destroyed\n", enemy->q, enemy->r);
        SpawnEnemyDrop(enemy->q, enemy->r); // the drop appends at the tail, then the despawn swaps it into this slot
        EntityDespawn(i);
    }

    if (cell->cardLevel >= CARD_LVL_2)
    {
        ReturnPlacedCard(cell); // spent, straight back to the hand
    }
    else
    {
        // lvl 1: dead on the field for a turn. Kind/level stay on the cell
        // so the cooled husk can hand the card back; value drops to 0 so the
        // husk is no card (unstrikeable, unmergeable, no gem)
        cell->value = 0;
        cell->fireballCharge = 0;
        cell->inDanger = false;
        cell->turnLifetime = 0;
        cell->huskTurns = 1;
    }
}

static void SpawnCardKind(CardKind kind)
{
    Entity *card = EntitySpawn(ENTITY_CARD);
    if (card == NULL) return;

    // Every deal is level 1; higher levels exist only through merging on the board
    card->cardKind = kind;
    card->cardLevel = CARD_LVL_1;
    card->value = (int)card->cardLevel;
    card->tint = cardKindTints[card->cardKind];
    card->cardMode = CARD_REC_FORM;
    card->modelIndex = (cardModelCount > 0) ? GetRandomValue(0, cardModelCount - 1) : 0;
    if (kindModelIndex[card->cardKind] >= 0) card->modelIndex = kindModelIndex[card->cardKind];
    snprintf(card->tooltip, sizeof(card->tooltip), "%s -- stays %d turns on the field",
             cardKindTooltips[card->cardKind], cardKindLifetimes[card->cardKind]);
    // Starts below the screen edge and springs up into its fan slot
    card->position = (Vector3){(float)screenWidth / 2.0f, (float)screenHeight + CARD_HEIGHT, 0.0f};
}

// A random-kind deal (the debug button; the starting hand is fixed)
static void SpawnCard(void)
{
    SpawnCardKind((CardKind)GetRandomValue(CARD_LEYLINE, CARD_WARD));
}

// Update and draw frame: a flat sequence of system steps, each its own pass
// over the entity list
// Leylines: conduit cards mark tiles; iridescent arcs connect adjacent
// conducting tiles and energize when the network reaches the player. The
// whole network renders alone on leylineTexture, and the composite stamps
// that layer's silhouette in ink around itself -- a screen-space pen outline
// on its own layer, then the color art on top.
// Iridescent film color: hue slides with distance along the beam and drifts
// with time, with a soft shimmer wave riding on top
static Color LeylineIridColor(float along, float time)
{
    float h = leyHue + leyIrid * (0.55f * along + 0.18f * sinf(time * 1.4f + along * 6.2831f) + 0.08f * time);
    h -= floorf(h);
    return ColorFromHSV(h * 360.0f, 0.62f, 1.0f);
}

static Color LeylineDimColor(void)
{
    return ColorFromHSV(leyHue * 360.0f, 0.25f, 0.45f);
}

// Point at tt along the raised arc between two tile centers
static Vector3 LeylineBezier(Vector3 a, Vector3 b, float tt)
{
    float mx = (a.x + b.x) / 2.0f;
    float mz = (a.z + b.z) / 2.0f;
    Vector3 out = {
        (1 - tt) * (1 - tt) * a.x + 2 * (1 - tt) * tt * mx + tt * tt * b.x,
        HEX_TILE_HEIGHT + 0.08f + 4.0f * leyArc * tt * (1 - tt),
        (1 - tt) * (1 - tt) * a.z + 2 * (1 - tt) * tt * mz + tt * tt * b.z,
    };
    return out;
}

// One player mage billboard, perpendicular to the camera. Shared by the
// world pass and the over-the-crystal redraw
static void DrawPlayerSprite(const Entity *p)
{
    int frame = (frameCounter / 8) % PLAYER_FRAMES; // ~7.5 fps idle at 60 fps
    Rectangle src = {(float)(frame * PLAYER_FRAME_W), 0.0f, (float)PLAYER_FRAME_W, (float)PLAYER_FRAME_H};

    float spriteH = playerSpriteSize;                                          // World height of the billboard
    float spriteW = spriteH * ((float)PLAYER_FRAME_W / (float)PLAYER_FRAME_H); // Frames are 2:1, square pixels
    float ground = (p->selected && reticle.active) ? reticle.position.y : HEX_TILE_HEIGHT;
    Vector3 pos = {p->position.x, ground + spriteH / 2.0f, p->position.z};

    // Perpendicular to the camera: use the view-space up axis instead of
    // world Y, so the tilted camera does not foreshorten the sprite
    Matrix matView = MatrixLookAt(camera.position, camera.target, camera.up);
    Vector3 camUp = {matView.m1, matView.m5, matView.m9};
    DrawBillboardPro(camera, playerTexture, src, pos, camUp, (Vector2){spriteW, spriteH},
                     (Vector2){spriteW / 2.0f, spriteH / 2.0f}, 0.0f, WHITE);
}

// One enemy instance: red outline hulls + spiky body, everything per-instance
// read from the entity. Expects the frame's shared enemy uniforms (time,
// spikes, view) already set. Shared by the world pass and the redraw
static void DrawEnemyInstance(const Entity *enemy, float airTime)
{
    // Per-instance motion from the entity: spawn frame gives each one its own
    // phase, pulse/breathe rates ride the struct. Mesh draws flush
    // immediately, so per-instance uniforms are safe
    float phase = (float)(enemy->frameCreated % 628) * 0.01f;
    float enemyYaw = airTime * 0.35f + phase;
    Vector3 enemyPos = enemy->position;
    enemyPos.y = HEX_TILE_HEIGHT + enemySize + 0.5f + sinf(airTime * 1.3f + phase) * 0.08f;
    if (enemy->selected) enemyPos.y += 0.25f; // Lifted while grabbed

    // L4D outline: inverted hulls under the body -- a solid ring plus a
    // fainter, fatter additive glow ring, tracking the spikes. Danger red
    // normally, curse purple while this enemy is hexed
    if (enemyOutline > 0.002f)
    {
        float glowExpand = enemyOutline * 2.6f;
        float solidAlpha = 1.0f;
        float glowAlpha = 0.28f;
        float hexed = (enemy->hexedTurns > 0) ? 1.0f : 0.0f;
        SetShaderValue(enemyOutlineShader, enemyOutlineLocs.hexed, &hexed, SHADER_UNIFORM_FLOAT);
        SetShaderValue(enemyOutlineShader, enemyOutlineLocs.yaw, &enemyYaw, SHADER_UNIFORM_FLOAT);
        SetShaderValue(enemyOutlineShader, enemyOutlineLocs.pulseSpeed, &enemy->enemyPulseSpeed, SHADER_UNIFORM_FLOAT);
        SetShaderValue(enemyOutlineShader, enemyOutlineLocs.breatheAmp, &enemy->enemyBreatheAmp, SHADER_UNIFORM_FLOAT);
        SetShaderValue(enemyOutlineShader, enemyOutlineLocs.breatheSpeed, &enemy->enemyBreatheSpeed, SHADER_UNIFORM_FLOAT);
        enemyModel.materials[0].shader = enemyOutlineShader;
        // Flush queued batch geometry (tile art quads etc.) before flipping
        // the cull face, or it renders front-culled and disappears when the
        // blend mode change flushes it
        rlDrawRenderBatchActive();
        rlSetCullFace(RL_CULL_FACE_FRONT);
        SetShaderValue(enemyOutlineShader, enemyOutlineLocs.expand, &enemyOutline, SHADER_UNIFORM_FLOAT);
        SetShaderValue(enemyOutlineShader, enemyOutlineLocs.alpha, &solidAlpha, SHADER_UNIFORM_FLOAT);
        DrawModel(enemyModel, enemyPos, enemySize, WHITE);
        BeginBlendMode(BLEND_ADDITIVE);
        SetShaderValue(enemyOutlineShader, enemyOutlineLocs.expand, &glowExpand, SHADER_UNIFORM_FLOAT);
        SetShaderValue(enemyOutlineShader, enemyOutlineLocs.alpha, &glowAlpha, SHADER_UNIFORM_FLOAT);
        DrawModel(enemyModel, enemyPos, enemySize, WHITE);
        EndBlendMode();
        rlSetCullFace(RL_CULL_FACE_BACK);
        enemyModel.materials[0].shader = enemyShader;
    }

    SetShaderValue(enemyShader, enemyLocs.yaw, &enemyYaw, SHADER_UNIFORM_FLOAT);
    SetShaderValue(enemyShader, enemyLocs.pulseSpeed, &enemy->enemyPulseSpeed, SHADER_UNIFORM_FLOAT);
    SetShaderValue(enemyShader, enemyLocs.breatheAmp, &enemy->enemyBreatheAmp, SHADER_UNIFORM_FLOAT);
    SetShaderValue(enemyShader, enemyLocs.breatheSpeed, &enemy->enemyBreatheSpeed, SHADER_UNIFORM_FLOAT);
    DrawModel(enemyModel, enemyPos, enemySize, WHITE);
}

// The placed card this enemy would strike on the next turn, or NULL. Wards
// bait: any ward in attack range takes priority over every other card, so a
// well-placed ward soaks the hit. The turn pass and the intent display both
// call this, so the telegraph always matches the action
static Entity *EnemyStrikeTarget(const Entity *enemy)
{
    Entity *firstCard = NULL;
    for (int j = 0; j < entityCount; j++)
    {
        Entity *cell = &entities[j];
        if ((cell->kind != ENTITY_HEX_CELL) || (cell->value == 0)) continue;
        if (HexDistance(enemy->q, enemy->r, cell->q, cell->r) > 1) continue;
        if (cell->cardKind == CARD_WARD) return cell;
        if (firstCard == NULL) firstCard = cell;
    }
    return firstCard;
}

// Where this enemy would stand after its chase steps toward the target: a
// BFS distance field flooded from the target over walkable tiles (the
// crystal's solid tile and tiles held by other enemies block), walked
// downhill up to moveRange steps. Routing, not greed: the enemy goes AROUND
// the crystal instead of stalling behind it
static void EnemyChaseDestination(const Entity *enemy, const Entity *player, int *outQ, int *outR)
{
    static const int dirs[6][2] = {{1, 0}, {1, -1}, {0, -1}, {-1, 0}, {-1, 1}, {0, 1}};

    bool blocked[LEY_W][LEY_W] = {0};
    for (int j = 0; j < entityCount; j++)
    {
        const Entity *other = &entities[j];
        if ((other->kind != ENTITY_ENEMY) || (other == enemy)) continue;
        if (HexOnBoard(other->q, other->r)) blocked[other->q + GRID_RADIUS][other->r + GRID_RADIUS] = true;
    }

    // Flood from the target: field = walking distance to it, -1 unreachable
    int field[LEY_W][LEY_W];
    memset(field, -1, sizeof(field));
    int queueQ[LEY_W * LEY_W];
    int queueR[LEY_W * LEY_W];
    int head = 0;
    int tail = 0;
    field[player->q + GRID_RADIUS][player->r + GRID_RADIUS] = 0;
    queueQ[tail] = player->q;
    queueR[tail] = player->r;
    tail++;
    while (head < tail)
    {
        int q = queueQ[head];
        int r = queueR[head];
        head++;
        for (int d = 0; d < 6; d++)
        {
            int nq = q + dirs[d][0];
            int nr = r + dirs[d][1];
            if (!HexOnBoard(nq, nr) || HexTileSolid(nq, nr)) continue;
            if (blocked[nq + GRID_RADIUS][nr + GRID_RADIUS]) continue;
            if (field[nq + GRID_RADIUS][nr + GRID_RADIUS] >= 0) continue;
            field[nq + GRID_RADIUS][nr + GRID_RADIUS] = field[q + GRID_RADIUS][r + GRID_RADIUS] + 1;
            queueQ[tail] = nq;
            queueR[tail] = nr;
            tail++;
        }
    }

    // Walk the field downhill, stopping once adjacent to the target
    int q = enemy->q;
    int r = enemy->r;
    for (int step = 0; step < enemy->enemyMoveRange; step++)
    {
        if (HexDistance(q, r, player->q, player->r) <= 1) break;

        int bestQ = q;
        int bestR = r;
        int bestField = field[q + GRID_RADIUS][r + GRID_RADIUS];
        if (bestField < 0) break; // sealed off entirely: hold
        for (int d = 0; d < 6; d++)
        {
            int nq = q + dirs[d][0];
            int nr = r + dirs[d][1];
            if (!HexOnBoard(nq, nr) || HexTileSolid(nq, nr)) continue;
            if (blocked[nq + GRID_RADIUS][nr + GRID_RADIUS]) continue;
            int f = field[nq + GRID_RADIUS][nr + GRID_RADIUS];
            if ((f >= 0) && (f < bestField))
            {
                bestField = f;
                bestQ = nq;
                bestR = nr;
            }
        }
        if ((bestQ == q) && (bestR == r)) break; // boxed in
        q = bestQ;
        r = bestR;
    }
    *outQ = q;
    *outR = r;
}

// EXECUTES each enemy's stored intent verbatim on a committed turn (strike
// the telegraphed card / walk to the telegraphed tile). Runs only from the
// turn-commit pass; EnemyAIUpdate re-plans right after
static void EnemyAIExecuteIntents(void)
{
    for (int i = 0; i < entityCount; i++)
    {
        Entity *enemy = &entities[i];
        if ((enemy->kind != ENTITY_ENEMY) || enemy->selected) continue;

        if (enemy->enemyIntentKind == ENEMY_INTENT_STRIKE)
        {
            for (int j = 0; j < entityCount; j++)
            {
                Entity *cell = &entities[j];
                if ((cell->kind != ENTITY_HEX_CELL) || (cell->value == 0)) continue;
                if ((cell->q != enemy->enemyIntentQ) || (cell->r != enemy->enemyIntentR)) continue;

                LOG("INFO: ENEMY: struck %s lvl %d at (q=%d, r=%d)\n",
                    cardKindNames[cell->cardKind], (int)cell->cardLevel, cell->q, cell->r);
                impact = (ImpactFx){.position = cell->position, .tint = (Color){235, 70, 60, 255}, .age = 0.0f, .active = true};
                shakeTimeLeft = SHAKE_DURATION;
                PlaySound(sndHit);
                bool warded = (cell->cardKind == CARD_WARD); // Wards soak the hit for free
                bool hexTrap = (cell->cardKind == CARD_HEX); // Hexes curse the striker
                if (hexTrap)
                {
                    // +1 because this turn's wear-off pass (right after the
                    // execute) eats a stack before the curse ever fights
                    enemy->hexedTurns = (int)cell->cardLevel + 1;
                    LOG("INFO: HEX: enemy at (q=%d, r=%d) cursed, fights for you %d turns\n",
                        enemy->q, enemy->r, (int)cell->cardLevel);
                }
                Entity *back = ReturnPlacedCard(cell); // forced back to the hand...
                if (warded && (back != NULL)) back->huskTurns = 1; // used wards come home exhausted for a turn
                for (int h = 0; !warded && !hexTrap && (h < entityCount); h++)
                {
                    if ((entities[h].kind == ENTITY_HEALTH_BAR) && (entities[h].health > 0)) entities[h].health--;
                } // ...an unwarded, unhexed forced return costs the player a heart
                break;
            }
        }
        else if (enemy->enemyIntentKind == ENEMY_INTENT_ATTACK)
        {
            if (enemy->hexedTurns > 0)
            {
                // Hexed: the attack lands on the enemy standing on the
                // intent tile (whiffs if it stepped away this same commit)
                for (int j = entityCount - 1; j >= 0; j--)
                {
                    Entity *foe = &entities[j];
                    if ((foe->kind != ENTITY_ENEMY) || (foe == enemy) || (foe->hexedTurns > 0)) continue;
                    if ((foe->q != enemy->enemyIntentQ) || (foe->r != enemy->enemyIntentR)) continue;

                    LOG("INFO: HEX: cursed enemy destroyed foe at (q=%d, r=%d)\n", foe->q, foe->r);
                    impact = (ImpactFx){.position = foe->position, .tint = (Color){185, 100, 235, 255}, .age = 0.0f, .active = true};
                    shakeTimeLeft = SHAKE_DURATION;
                    PlaySound(sndHit);
                    SpawnEnemyDrop(foe->q, foe->r);
                    EntityDespawn(j); // swap-back: only already-visited or skippable slots move
                    break;
                }
            }
            else
            {
                // Every board change re-plans, so the intent tile still holds
                // the player: land the hit for 2 hearts
                for (int j = 0; j < entityCount; j++)
                {
                    Entity *hit = &entities[j];
                    if (hit->kind == ENTITY_PLAYER)
                    {
                        impact = (ImpactFx){.position = hit->position, .tint = (Color){235, 70, 60, 255}, .age = 0.0f, .active = true};
                    }
                    if (hit->kind != ENTITY_HEALTH_BAR) continue;
                    hit->health -= 2;
                    if (hit->health < 0) hit->health = 0;
                }
                shakeTimeLeft = SHAKE_DURATION;
                PlaySound(sndHit);
                LOG("INFO: ENEMY: hit the player for 2 at (q=%d, r=%d)\n", enemy->enemyIntentQ, enemy->enemyIntentR);
            }
        }
        else if (enemy->enemyIntentKind == ENEMY_INTENT_MOVE)
        {
            bool taken = false;
            for (int j = 0; j < entityCount; j++)
            {
                const Entity *other = &entities[j];
                if ((other->kind != ENTITY_ENEMY) || (other == enemy)) continue;
                if ((other->q == enemy->enemyIntentQ) && (other->r == enemy->enemyIntentR)) taken = true;
            }
            if (!taken)
            {
                enemy->q = enemy->enemyIntentQ;
                enemy->r = enemy->enemyIntentR;
                LOG("INFO: ENEMY: chased to (q=%d, r=%d)\n", enemy->q, enemy->r);
            }
        }
    }
}

// The enemy decision pass: recomputes every telegraph from the live board.
// Event-driven, not per-frame -- call it whenever the board changes in a way
// the enemies care about: a card is played, the player moves, and a new turn
// has started (after intents execute and placed cards return). The intent
// display only reads the stored fields -- it cannot disagree with what
// EnemyAIExecuteIntents will do
static void EnemyAIUpdate(void)
{
    Entity *player = NULL;
    for (int i = 0; i < entityCount; i++)
    {
        if (entities[i].kind == ENTITY_PLAYER)
        {
            player = &entities[i];
            break;
        }
    }

    // Every re-plan starts from a clean board: only cards a strike is
    // telegraphed on below wear the danger shell
    for (int i = 0; i < entityCount; i++)
    {
        if (entities[i].kind == ENTITY_HEX_CELL) entities[i].inDanger = false;
    }

    for (int i = 0; i < entityCount; i++)
    {
        Entity *enemy = &entities[i];
        if (enemy->kind != ENTITY_ENEMY) continue;

        enemy->enemyIntentKind = ENEMY_INTENT_NONE;

        // Hexed: fights for the player -- hunts the nearest unhexed enemy
        // (attack when adjacent, chase otherwise), ignoring cards and the
        // player entirely until the curse wears off
        if (enemy->hexedTurns > 0)
        {
            Entity *prey = NULL;
            int preyDist = 0;
            for (int j = 0; j < entityCount; j++)
            {
                Entity *other = &entities[j];
                if ((other->kind != ENTITY_ENEMY) || (other == enemy) || (other->hexedTurns > 0)) continue;
                int dist = HexDistance(enemy->q, enemy->r, other->q, other->r);
                if ((prey == NULL) || (dist < preyDist))
                {
                    prey = other;
                    preyDist = dist;
                }
            }
            if (prey == NULL) continue; // no foes left: hold

            if (preyDist <= 1)
            {
                enemy->enemyIntentKind = ENEMY_INTENT_ATTACK;
                enemy->enemyIntentQ = prey->q;
                enemy->enemyIntentR = prey->r;
            }
            else
            {
                int destQ, destR;
                EnemyChaseDestination(enemy, prey, &destQ, &destR);
                if ((destQ != enemy->q) || (destR != enemy->r))
                {
                    enemy->enemyIntentKind = ENEMY_INTENT_MOVE;
                    enemy->enemyIntentQ = destQ;
                    enemy->enemyIntentR = destR;
                }
            }
            continue;
        }

        Entity *strike = EnemyStrikeTarget(enemy);
        if (strike != NULL)
        {
            enemy->enemyIntentKind = ENEMY_INTENT_STRIKE;
            enemy->enemyIntentQ = strike->q;
            enemy->enemyIntentR = strike->r;
            strike->inDanger = true;
            continue;
        }

        if (player == NULL) continue;

        // No card to smash: the player himself is fair game when in reach
        if (HexDistance(enemy->q, enemy->r, player->q, player->r) <= 1)
        {
            enemy->enemyIntentKind = ENEMY_INTENT_ATTACK;
            enemy->enemyIntentQ = player->q;
            enemy->enemyIntentR = player->r;
            continue;
        }

        int destQ, destR;
        EnemyChaseDestination(enemy, player, &destQ, &destR);
        if ((destQ != enemy->q) || (destR != enemy->r))
        {
            enemy->enemyIntentKind = ENEMY_INTENT_MOVE;
            enemy->enemyIntentQ = destQ;
            enemy->enemyIntentR = destR;
        }
    }
}

// Fresh run: sweep every non-cell entity, wipe the board state off the
// cells (their generated shell meshes survive), and rebuild the starting
// cast. Used at boot and by the game over screen's restart
static void ResetGame(void)
{
    for (int i = entityCount - 1; i >= 0; i--)
    {
        if (entities[i].kind != ENTITY_HEX_CELL) EntityDespawn(i);
    }
    for (int i = 0; i < entityCount; i++)
    {
        Entity *cell = &entities[i];
        cell->value = 0;
        cell->cardKind = CARD_NONE;
        cell->cardLevel = 0;
        cell->isLeyline = false;
        cell->inDanger = false;
        cell->turnLifetime = 0;
        cell->fireballCharge = 0;
        cell->huskTurns = 0;
        cell->hovered = false;
    }
    gameTurn = 0;
    lastCommittedTurn = 0;
    playerMovedTurn = -1;

    Entity *healthBar = EntitySpawn(ENTITY_HEALTH_BAR);
    if (healthBar != NULL)
    {
        healthBar->position = (Vector3){24.0f, 64.0f, 0.0f}; // Screen pixels for UI kinds
        healthBar->health = 3;
        healthBar->maxHealth = 5;
    }

    // Starting hand: always two leylines, a hex, a ward, and a fireball
    SpawnCardKind(CARD_LEYLINE);
    SpawnCardKind(CARD_LEYLINE);
    SpawnCardKind(CARD_HEX);
    SpawnCardKind(CARD_WARD);
    SpawnCardKind(CARD_FIREBALL);

    Entity *playerSpawn = EntitySpawn(ENTITY_PLAYER);
    if (playerSpawn != NULL)
    {
        playerSpawn->q = 0; // Starts beside the crystal (the center tile is solid)
        playerSpawn->r = 1;
        playerSpawn->position = HexAxialToWorld(0, 1);
        playerSpawn->moveRange = 1; // Tiles the player may walk per committed turn
    }

    SpawnEnemy(2, -2);
    EnemyAIUpdate(); // first plan: the telegraph exists before any input event
}

// enemy intent -- the telegraph for the next lever pull, drawing only the
// stored intent fields EnemyAIUpdate computed so it can never lie. Strike: a
// red dashed arc to the doomed card, a pulsing ring, the raised wash hex,
// and a bobbing "!" shout. Move: a solid red arch from the enemy's tile to
// the tile it will walk to, plus a steady ring there. Call it LAST in the
// scene, with nothing drawn after: the 3D part runs depth-off so no crystal,
// card, or particle can cover the warning
static void DrawIntent(float airTime)
{
    BeginMode3D(camera);
    rlDisableDepthTest();
    for (int i = 0; i < entityCount; i++)
    {
        const Entity *enemy = &entities[i];
        if ((enemy->kind != ENTITY_ENEMY) || enemy->selected) continue;

        if (enemy->enemyIntentKind == ENEMY_INTENT_NONE) continue;
        bool willStrike = (enemy->enemyIntentKind == ENEMY_INTENT_STRIKE);
        bool willAttack = (enemy->enemyIntentKind == ENEMY_INTENT_ATTACK);
        Vector3 to = HexAxialToWorld(enemy->enemyIntentQ, enemy->enemyIntentR);
        Color intentColor;
        if (willStrike || willAttack)
        {
            float pulse = 0.6f + 0.4f * sinf(airTime * 5.0f);
            intentColor = (Color){250, 45, 35, (unsigned char)(195 + 60 * pulse)};
        }
        else
        {
            intentColor = (Color){235, 70, 60, 220}; // steady red; hits pulse
        }

        // dashed arc from the enemy to the intended tile, built from
        // camera-facing ribbon quads: glLineWidth is capped near 1px on core
        // GL, so thickness has to be real geometry
        Vector3 from = enemy->position;
        Vector3 camFwd = Vector3Normalize(Vector3Subtract(camera.target, camera.position));
        const float ribbonHalf = 0.07f;
        float ringR = HEX_SIZE * (0.72f + 0.05f * sinf(airTime * 5.0f));
        const float bandHalf = 0.07f;
        const int intentSteps = 14;
        rlDisableBackfaceCulling();

        // strike target: a filled red hex RAISED above the doomed card (the
        // gem slab sits on the tile and swallows anything drawn at tile
        // level), bobbing and pulsing so it is hard to miss
        if (willStrike)
        {
            float washPulse = 0.5f + 0.5f * sinf(airTime * 6.0f);
            float washY = HEX_TILE_HEIGHT + 0.65f + 0.06f * sinf(airTime * 3.0f);
            rlBegin(RL_TRIANGLES);
            rlColor4ub(250, 25, 20, (unsigned char)(150 + 90 * washPulse));
            for (int k = 0; k < 6; k++)
            {
                float a0 = DEG2RAD * (60.0f * (float)k + 30.0f);
                float a1 = DEG2RAD * (60.0f * (float)(k + 1) + 30.0f);
                float rr = HEX_SIZE * 0.96f;
                rlVertex3f(to.x, washY, to.z);
                rlVertex3f(to.x + cosf(a0) * rr, washY, to.z + sinf(a0) * rr);
                rlVertex3f(to.x + cosf(a1) * rr, washY, to.z + sinf(a1) * rr);
            }
            rlEnd();
        }

        // move: a SOLID red arch from the enemy's tile to the tile it will
        // walk to on the next lever pull -- fat cylinder segments along the
        // bezier (real geometry, reads from any camera angle), with the same
        // dark underlay treatment so it pops on bright tiles
        if (enemy->enemyIntentKind == ENEMY_INTENT_MOVE)
        {
            Vector3 tileFrom = HexAxialToWorld(enemy->q, enemy->r);
            const int archSteps = 16;
            for (int pass = 0; pass < 2; pass++)
            {
                float rad = (pass == 0) ? 0.15f : 0.10f;
                Color pc = (pass == 0) ? (Color){16, 10, 14, 235} : intentColor;
                Vector3 prev = {tileFrom.x, HEX_TILE_HEIGHT, tileFrom.z};
                for (int k = 1; k <= archSteps; k++)
                {
                    float t = (float)k / archSteps;
                    float mx = (tileFrom.x + to.x) / 2.0f;
                    float mz = (tileFrom.z + to.z) / 2.0f;
                    Vector3 p = {(1 - t) * (1 - t) * tileFrom.x + 2 * (1 - t) * t * mx + t * t * to.x,
                                 HEX_TILE_HEIGHT + 3.2f * t * (1.0f - t),
                                 (1 - t) * (1 - t) * tileFrom.z + 2 * (1 - t) * t * mz + t * t * to.z};
                    DrawCylinderEx(prev, p, rad, rad, 6, pc);
                    prev = p;
                }
            }
        }

        // two passes: a fat near-black underlay, then the red stroke on top,
        // so the warning pops on bright tiles instead of washing out
        for (int pass = 0; pass < 2; pass++)
        {
            float half = (pass == 0) ? ribbonHalf + 0.04f : ribbonHalf;
            Color pc = (pass == 0) ? (Color){16, 10, 14, 235} : intentColor;

            rlBegin(RL_QUADS);
            rlColor4ub(pc.r, pc.g, pc.b, pc.a);
            for (int k = 0; (willStrike || willAttack) && (k < intentSteps); k += 2) // dashes: strikes and player hits, moves get the solid arch
            {
                float t0 = (float)k / intentSteps;
                float t1 = (float)(k + 1) / intentSteps;
                float mx = (from.x + to.x) / 2.0f;
                float mz = (from.z + to.z) / 2.0f;
                Vector3 p0 = {(1 - t0) * (1 - t0) * from.x + 2 * (1 - t0) * t0 * mx + t0 * t0 * to.x,
                              HEX_TILE_HEIGHT + 0.35f + 1.6f * t0 * (1.0f - t0),
                              (1 - t0) * (1 - t0) * from.z + 2 * (1 - t0) * t0 * mz + t0 * t0 * to.z};
                Vector3 p1 = {(1 - t1) * (1 - t1) * from.x + 2 * (1 - t1) * t1 * mx + t1 * t1 * to.x,
                              HEX_TILE_HEIGHT + 0.35f + 1.6f * t1 * (1.0f - t1),
                              (1 - t1) * (1 - t1) * from.z + 2 * (1 - t1) * t1 * mz + t1 * t1 * to.z};
                Vector3 dir = Vector3Normalize(Vector3Subtract(p1, p0));
                Vector3 side = Vector3Scale(Vector3Normalize(Vector3CrossProduct(dir, camFwd)), half);
                rlVertex3f(p0.x - side.x, p0.y - side.y, p0.z - side.z);
                rlVertex3f(p0.x + side.x, p0.y + side.y, p0.z + side.z);
                rlVertex3f(p1.x + side.x, p1.y + side.y, p1.z + side.z);
                rlVertex3f(p1.x - side.x, p1.y - side.y, p1.z - side.z);
            }
            // the target ring: a flat hex band, same underlay treatment
            for (int k = 0; k < 6; k++)
            {
                float a0 = DEG2RAD * (60.0f * (float)k + 30.0f);
                float a1 = DEG2RAD * (60.0f * (float)(k + 1) + 30.0f);
                float ri = ringR - half;
                float ro = ringR + half;
                rlVertex3f(to.x + cosf(a0) * ri, HEX_TILE_HEIGHT + 0.04f, to.z + sinf(a0) * ri);
                rlVertex3f(to.x + cosf(a0) * ro, HEX_TILE_HEIGHT + 0.04f, to.z + sinf(a0) * ro);
                rlVertex3f(to.x + cosf(a1) * ro, HEX_TILE_HEIGHT + 0.04f, to.z + sinf(a1) * ro);
                rlVertex3f(to.x + cosf(a1) * ri, HEX_TILE_HEIGHT + 0.04f, to.z + sinf(a1) * ri);
            }
            rlEnd();
        }
        rlEnableBackfaceCulling();
        (void)bandHalf;
    }
    // Flush while the depth test is still off: the batch otherwise renders
    // at EndMode3D with the test re-enabled and dies against the gem depths
    rlDrawRenderBatchActive();
    rlEnableDepthTest();
    EndMode3D();

    // strike intent shouts: a bobbing red "!" over any enemy about to hit a
    // card or the player
    for (int i = 0; i < entityCount; i++)
    {
        const Entity *enemy = &entities[i];
        if ((enemy->kind != ENTITY_ENEMY) || enemy->selected) continue;
        if ((enemy->enemyIntentKind != ENEMY_INTENT_STRIKE) && (enemy->enemyIntentKind != ENEMY_INTENT_ATTACK)) continue;
        Vector3 above = enemy->position;
        above.y = HEX_TILE_HEIGHT + enemySize * 2.0f + 1.9f;
        Vector2 shout = GetWorldToScreen(above, camera);
        float bob = sinf(airTime * 4.0f) * 4.0f;
        const int shoutSize = 90;
        int shoutX = (int)shout.x - MeasureText("!", shoutSize) / 2;
        DrawTextOutlined("!", shoutX, (int)(shout.y + bob), shoutSize, (Color){235, 70, 60, 255});
    }
}

static void UpdateDrawFrame(void)
{
    // Update
    //----------------------------------------------------------------------------------
    frameCounter++;

    // When ImGui wants the mouse (hovering/dragging a UI window), the board
    // must not see clicks; WantCaptureMouse lags one frame, which is fine
    uiWantsMouse = igGetIO_Nil()->WantCaptureMouse;

    // Modal screens: while not playing the world still animates as the
    // backdrop, but claiming the mouse here freezes every board interaction
    // (grabs, drops, the turn button). The overlays late in the draw pass
    // read the clicks and flip the state
    if (gameState != GAME_STATE_PLAYING) uiWantsMouse = true;

    // Losing the last heart ends the run
    if (gameState == GAME_STATE_PLAYING)
    {
        for (int i = 0; i < entityCount; i++)
        {
            if ((entities[i].kind != ENTITY_HEALTH_BAR) || (entities[i].health > 0)) continue;
            gameState = GAME_STATE_OVER;
            LOG("INFO: GAME: over at turn %d\n", gameTurn);
        }
    }

    // Exodia: holding a lvl 4 piece of every kind wins the run
    if (gameState == GAME_STATE_PLAYING)
    {
        bool pieceOfKind[NUM_CARD_KINDS] = {0};
        for (int i = 0; i < entityCount; i++)
        {
            const Entity *card = &entities[i];
            if ((card->kind != ENTITY_CARD) || (card->cardLevel < CARD_LVL_4)) continue;
            pieceOfKind[card->cardKind] = true;
        }
        bool all = true;
        for (int k = CARD_LEYLINE; k < NUM_CARD_KINDS; k++)
        {
            if (!pieceOfKind[k]) all = false;
        }
        if (all)
        {
            gameState = GAME_STATE_WIN;
            LOG("INFO: GAME: exodia assembled, won at turn %d\n", gameTurn);
        }
    }

    //
    // camera -- WASD pan, right-drag orbit, wheel zoom
    //

    UpdateGameCamera();

    // Pick the cell under the mouse: cast a ray through the cursor and
    // intersect it with the board plane (y = 0)
    mouseHexQ = HEX_CELL_OFFBOARD;
    mouseHexR = HEX_CELL_OFFBOARD;
    mouseOnPlane = false;
    Ray mouseRay = GetScreenToWorldRay(GetMousePosition(), camera);
    if (fabsf(mouseRay.direction.y) > 0.0001f)
    {
        float t = -mouseRay.position.y / mouseRay.direction.y;
        if (t > 0.0f)
        {
            mouseBoardX = mouseRay.position.x + mouseRay.direction.x * t;
            mouseBoardZ = mouseRay.position.z + mouseRay.direction.z * t;
            mouseOnPlane = true;
            HexWorldToAxial(mouseBoardX, mouseBoardZ, &mouseHexQ, &mouseHexR);
        }
    }

    float dt = GetFrameTime();
    if (dt > 0.05f) dt = 0.05f; // Clamp frame hitches so the springs cannot explode
    Vector2 mouse = GetMousePosition();

    //
    // find the player -- later steps need his tile to behave around him
    //

    Entity *player = NULL;
    for (int i = 0; i < entityCount; i++)
    {
        if (entities[i].kind == ENTITY_PLAYER)
        {
            player = &entities[i];
            break;
        }
    }

    //
    // hex cell hover -- remember the hovered cell for the card, player,
    // reticle, and debug steps below
    //

    Entity *hoveredCell = NULL;
    for (int i = 0; i < entityCount; i++)
    {
        Entity *cell = &entities[i];
        if (cell->kind != ENTITY_HEX_CELL) continue;

        cell->hovered = ((cell->q == mouseHexQ) && (cell->r == mouseHexR) && !uiWantsMouse && !leverWantsMouse);
        if (cell->hovered) hoveredCell = cell;
    }

    //
    // player grab -- a press on the player's tile picks him up
    //

    if ((player != NULL) && (hoveredCell != NULL) && !uiWantsMouse && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && (player->q == hoveredCell->q) && (player->r == hoveredCell->r))
    {
        player->selected = true;
        PlaySound(sndPickup);
    }

    //
    // enemy grab -- a press on a draggable enemy's tile picks it up, unless
    // the player is standing there too (he wins the grab)
    //

    for (int i = 0; i < entityCount; i++)
    {
        Entity *enemy = &entities[i];
        if ((enemy->kind != ENTITY_ENEMY) || !enemy->IsDraggable) continue;
        if ((hoveredCell == NULL) || uiWantsMouse || !IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) continue;
        if ((enemy->q != hoveredCell->q) || (enemy->r != hoveredCell->r)) continue;
        if ((player != NULL) && player->selected) continue;

        enemy->selected = true;
    }

    //
    // card grab -- a press picks up the card under the cursor
    //

    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !uiWantsMouse)
    {
        for (int i = 0; i < entityCount; i++)
        {
            Entity *card = &entities[i];
            if (card->kind != ENTITY_CARD) continue;
            if (card->huskTurns > 0) continue;            // Dead in the hand: ungrabbable until it wakes
            if (card->cardLevel >= CARD_LVL_4) continue; // Exodia pieces are inert trophies

            Rectangle rect = {card->position.x - CARD_WIDTH * cardScale / 2.0f, card->position.y - CARD_HEIGHT * cardScale / 2.0f,
                              CARD_WIDTH * cardScale, CARD_HEIGHT * cardScale};
            if (CheckCollisionPointRec(mouse, rect))
            {
                card->selected = true;
                PlaySound(sndPickup);
                break; // Only one card can be grabbed
            }
        }
    }

    //
    // card mode -- a grabbed card ghosts over the board, is a rectangle elsewhere
    //

    for (int i = 0; i < entityCount; i++)
    {
        Entity *card = &entities[i];
        if (card->kind != ENTITY_CARD) continue;
        card->cardMode = (card->selected && (hoveredCell != NULL)) ? CARD_HEX_FORM : CARD_REC_FORM;
    }

    //
    // card drop -- release commits the grabbed card onto an empty tile with a
    // screen shake + impact; anywhere else it springs home
    //

    if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT))
    {
        for (int i = 0; i < entityCount; i++)
        {
            Entity *card = &entities[i];
            if ((card->kind != ENTITY_CARD) || !card->selected) continue;

            card->selected = false;
            if (hoveredCell != NULL)
            {
                // 2048 rule: dropping onto a placed card of the SAME kind
                // and SAME level merges them into one of the next level
                bool canMerge = (hoveredCell->value > 0) && (hoveredCell->cardKind == card->cardKind) && (hoveredCell->cardLevel == card->cardLevel) && (hoveredCell->cardLevel < CARD_LVL_4);
                if (canMerge)
                {
                    hoveredCell->cardLevel++;
                    hoveredCell->value = (int)hoveredCell->cardLevel;
                    hoveredCell->turnLifetime = cardKindLifetimes[hoveredCell->cardKind]; // merge refreshes the clock
                    hoveredCell->fireballCharge = 0;                                      // merged fireballs charge from scratch
                    impact = (ImpactFx){.position = hoveredCell->position, .tint = card->tint, .age = 0.0f, .active = true};
                    LOG("INFO: CARD: merged %s to lvl %d at (q=%d, r=%d)\n",
                        cardKindNames[card->cardKind], (int)hoveredCell->cardLevel, hoveredCell->q, hoveredCell->r);
                    PlaySound(sndMerge);
                    EntityDespawn(i);
                    // Lvl 4 = an exodia piece: it leaves the board at once and
                    // lives in the hand as an inert trophy
                    if (hoveredCell->cardLevel >= CARD_LVL_4)
                    {
                        LOG("INFO: EXODIA: %s assembled!\n", exodiaPieceNames[hoveredCell->cardKind]);
                        ReturnPlacedCard(hoveredCell);
                    }
                    EnemyAIUpdate(); // card played: re-plan
                }
                else if ((hoveredCell->value == 0) && (hoveredCell->huskTurns == 0) &&
                         !HexTileSolid(hoveredCell->q, hoveredCell->r)) // husks and the crystal tile block placement
                {
                    // The ghost commits: the tile takes the card's kind,
                    // level, color, and hex model
                    hoveredCell->cardKind = card->cardKind;
                    hoveredCell->cardLevel = card->cardLevel;
                    hoveredCell->value = (int)card->cardLevel;
                    hoveredCell->turnLifetime = cardKindLifetimes[card->cardKind];
                    hoveredCell->fireballCharge = 0;
                    hoveredCell->tint = card->tint;
                    hoveredCell->modelIndex = card->modelIndex;
                    hoveredCell->isLeyline = hoveredCell->isLeyline || (card->cardKind == CARD_LEYLINE);
                    impact = (ImpactFx){.position = hoveredCell->position, .tint = card->tint, .age = 0.0f, .active = true};
                    LOG("INFO: CARD: placed %s lvl %d at (q=%d, r=%d)\n",
                        cardKindNames[card->cardKind], (int)card->cardLevel, hoveredCell->q, hoveredCell->r);
                    PlaySound(sndPlace);
                    EntityDespawn(i);
                    EnemyAIUpdate(); // card played: re-plan
                }
            }
            break; // Only one card can be grabbed
        }
    }

    //
    // card motion -- hover lift, springs toward fan slots in pool order (or the
    // cursor while dragged), rectangle fade while ghosting
    //

    int handCount = 0;
    for (int i = 0; i < entityCount; i++)
    {
        if (entities[i].kind == ENTITY_CARD) handCount++;
    }

    int slot = 0;
    for (int i = 0; i < entityCount; i++)
    {
        Entity *card = &entities[i];
        if (card->kind != ENTITY_CARD) continue;

        // Fan slot centered on the bottom edge, outer cards sit lower
        float fanIndex = (float)slot - (float)(handCount - 1) / 2.0f;
        slot++;
        Vector2 springTarget = {
            (float)screenWidth / 2.0f + fanIndex * CARD_FAN_STEP * cardScale,
            (float)screenHeight - CARD_FAN_BOTTOM + fabsf(fanIndex) * CARD_FAN_ARC,
        };

        Rectangle rect = {card->position.x - CARD_WIDTH * cardScale / 2.0f, card->position.y - CARD_HEIGHT * cardScale / 2.0f,
                          CARD_WIDTH * cardScale, CARD_HEIGHT * cardScale};
        card->hovered = (!uiWantsMouse && CheckCollisionPointRec(mouse, rect));
        if (card->hovered && !card->selected) springTarget.y -= 14.0f; // Hovered card lifts out of the fan

        // Ease toward the slot rather than sitting rigidly; the same
        // under-damped spring makes released cards bounce back home
        float stiffness = 260.0f; // Under-damped with damping 18 (critical ~32)
        float damping = 18.0f;
        if (card->selected && (card->cardMode == CARD_REC_FORM))
        {
            springTarget = mouse; // Dragged off-board: ride tight on the cursor
            stiffness = 900.0f;
            damping = 55.0f;
        }
        card->velocity.x += ((springTarget.x - card->position.x) * stiffness - card->velocity.x * damping) * dt;
        card->velocity.y += ((springTarget.y - card->position.y) * stiffness - card->velocity.y * damping) * dt;
        card->position.x += card->velocity.x * dt;
        card->position.y += card->velocity.y * dt;

        // Rectangle hides while ghosting on the board, fades back in when released
        float alphaTarget = (card->selected && (card->cardMode == CARD_HEX_FORM)) ? 0.0f : 1.0f;
        float fadeRate = (alphaTarget < card->alpha) ? 18.0f : 8.0f; // Hide fast, reappear soft
        card->alpha += (alphaTarget - card->alpha) * (1.0f - expf(-fadeRate * dt));

        // Press tilt: the hovered spot of the card model sinks under the
        // cursor (Balatro-style), eased both ways
        Vector2 tiltTarget = {0};
        if (card->hovered && !card->selected)
        {
            tiltTarget.x = (mouse.x - card->position.x) / (CARD_WIDTH * cardScale / 2.0f);
            tiltTarget.y = (mouse.y - card->position.y) / (CARD_HEIGHT * cardScale / 2.0f);
        }
        card->pressTilt.x += (tiltTarget.x - card->pressTilt.x) * 0.12f;
        card->pressTilt.y += (tiltTarget.y - card->pressTilt.y) * 0.12f;
    }

    //
    // air particles -- dodge softly away from the cursor's board point
    //

    UpdateAirParticles(airShards, (int)airShardCount < MAX_AIR_SHARDS ? (int)airShardCount : MAX_AIR_SHARDS, dt);
    UpdateAirParticles(airTris, (int)airTriCount < MAX_AIR_TRIS ? (int)airTriCount : MAX_AIR_TRIS, dt);

    //
    // effect state -- reticle chases the hovered tile, impact ages out
    //

    UpdateHoverReticle(hoveredCell);
    UpdateImpactEffect();

    //
    // player drag/drop -- dragged he stands on the reticle (runs after the
    // reticle step so he matches its sprung position exactly); released he
    // takes the hovered cell, or springs back home if dropped off-board
    //

    if (player != NULL)
    {
        if (player->selected && IsMouseButtonReleased(MOUSE_BUTTON_LEFT))
        {
            player->selected = false;
            if (hoveredCell != NULL)
            {
                // One step of moveRange tiles per committed turn; anything
                // else springs him home (dropping back on his own tile is
                // free and spends nothing)
                int stepDist = HexDistance(player->q, player->r, hoveredCell->q, hoveredCell->r);
                if ((stepDist > 0) && (stepDist <= player->moveRange) && (playerMovedTurn != gameTurn) &&
                    !HexTileSolid(hoveredCell->q, hoveredCell->r))
                {
                    playerMovedTurn = gameTurn;
                    player->q = hoveredCell->q;
                    player->r = hoveredCell->r;
                    LOG("INFO: PLAYER: placed at (q=%d, r=%d)\n", player->q, player->r);
                    PlaySound(sndPlace);
                    EnemyAIUpdate(); // player moved: re-plan
                }
                else if (stepDist > 0)
                {
                    LOG("INFO: PLAYER: move refused (dist %d, moved this turn: %s)\n",
                        stepDist, (playerMovedTurn == gameTurn) ? "yes" : "no");
                }
            }
        }

        if (player->selected && reticle.active)
        {
            // Ride the reticle: it already springs tile to tile, the player
            // just stands on it (velocity zeroed so the drop doesn't fling him)
            player->position.x = reticle.position.x;
            player->position.z = reticle.position.z;
            player->velocity = (Vector3){0};
        }
        else
        {
            // Off the reticle (idle, or dragged off-board): spring on the board
            // plane toward home or the cursor (under-damped, critical ~41)
            Vector3 home = HexAxialToWorld(player->q, player->r);
            float targetX = home.x;
            float targetZ = home.z;
            if (player->selected && mouseOnPlane)
            {
                targetX = mouseBoardX;
                targetZ = mouseBoardZ;
            }

            float stiffness = 420.0f;
            float damping = 24.0f;
            player->velocity.x += ((targetX - player->position.x) * stiffness - player->velocity.x * damping) * dt;
            player->velocity.z += ((targetZ - player->position.z) * stiffness - player->velocity.z * damping) * dt;
            player->position.x += player->velocity.x * dt;
            player->position.z += player->velocity.z * dt;
        }
    }

    //
    // pickup collection -- standing on a crystal gift adds its card to the
    // hand
    //

    if (player != NULL)
    {
        for (int i = entityCount - 1; i >= 0; i--) // backwards: despawn swaps from the tail
        {
            const Entity *gift = &entities[i];
            if (gift->kind != ENTITY_PICKUP) continue;
            if ((gift->q != player->q) || (gift->r != player->r)) continue;

            CardKind kind = gift->cardKind;
            LOG("INFO: PICKUP: collected the crystal's %s card\n", cardKindNames[kind]);
            PlaySound(sndPickup);
            EntityDespawn(i);
            SpawnCardKind(kind);
        }
    }

    //
    // enemy drag/drop -- same behavior as the player: grabbed it rides the
    // reticle, released it takes the hovered cell (or springs back home)
    //

    for (int i = 0; i < entityCount; i++)
    {
        Entity *enemy = &entities[i];
        if (enemy->kind != ENTITY_ENEMY) continue;

        if (enemy->selected && IsMouseButtonReleased(MOUSE_BUTTON_LEFT))
        {
            enemy->selected = false;
            if ((hoveredCell != NULL) && !HexTileSolid(hoveredCell->q, hoveredCell->r))
            {
                enemy->q = hoveredCell->q;
                enemy->r = hoveredCell->r;
                LOG("INFO: ENEMY: placed at (q=%d, r=%d)\n", enemy->q, enemy->r);
            }
        }

        if (enemy->selected && reticle.active)
        {
            enemy->position.x = reticle.position.x;
            enemy->position.z = reticle.position.z;
            enemy->velocity = (Vector3){0};
        }
        else
        {
            Vector3 home = HexAxialToWorld(enemy->q, enemy->r);
            float targetX = home.x;
            float targetZ = home.z;
            if (enemy->selected && mouseOnPlane)
            {
                targetX = mouseBoardX;
                targetZ = mouseBoardZ;
            }

            float stiffness = 420.0f;
            float damping = 24.0f;
            enemy->velocity.x += ((targetX - enemy->position.x) * stiffness - enemy->velocity.x * damping) * dt;
            enemy->velocity.z += ((targetZ - enemy->position.z) * stiffness - enemy->velocity.z * damping) * dt;
            enemy->position.x += enemy->velocity.x * dt;
            enemy->position.z += enemy->velocity.z * dt;
        }
    }

    //
    // fireball no-cast-time -- lvl 3 skips the charge cycle entirely: it
    // detonates the instant a live leyline chain taps its tile, checked
    // every frame so placing the connecting leyline (or stepping the player
    // into the chain) sets it off immediately
    //

    {
        bool anyLvl3 = false;
        for (int i = 0; i < entityCount; i++)
        {
            const Entity *cell = &entities[i];
            if ((cell->kind == ENTITY_HEX_CELL) && (cell->value > 0) && (cell->cardKind == CARD_FIREBALL) && (cell->cardLevel >= CARD_LVL_3)) anyLvl3 = true;
        }
        if (anyLvl3)
        {
            bool conductive[LEY_W][LEY_W];
            bool live[LEY_W][LEY_W];
            ComputeLeylineNetwork(player, conductive, live);
            bool boomed = false;
            for (int i = 0; i < entityCount; i++)
            {
                Entity *cell = &entities[i];
                if ((cell->kind != ENTITY_HEX_CELL) || (cell->value == 0) || (cell->cardKind != CARD_FIREBALL) || (cell->cardLevel < CARD_LVL_3)) continue;
                if (!LeylineTapped(live, cell->q, cell->r)) continue;
                DetonateFireball(cell);
                boomed = true;
            }
            if (boomed) EnemyAIUpdate(); // enemies died / the card left: re-plan
        }
    }

    //
    // turn commit -- when the lever fires: exhausted hand cards wake, husks
    // cool, connected fireballs bank a charge and detonate at full (BEFORE
    // any enemy acts), THEN enemies execute their telegraphed intents, hex
    // curses tick down, every placed card burns one turn of field lifetime,
    // the timed spawns fire (crystal gift every 5 turns, enemy reinforcement
    // every 7), and finally the AI re-plans against the new board
    //

    {
        if (gameTurn != lastCommittedTurn)
        {
            lastCommittedTurn = gameTurn;

            // exhausted hand cards wake up FIRST, so a ward used during this
            // commit's execute below stays dead for one full turn
            for (int i = 0; i < entityCount; i++)
            {
                Entity *card = &entities[i];
                if ((card->kind != ENTITY_CARD) || (card->huskTurns == 0)) continue;
                card->huskTurns--;
                if (card->huskTurns == 0) LOG("INFO: CARD: %s awake in the hand again\n", cardKindNames[card->cardKind]);
            }

            // spent fireball husks finish cooling: the card comes home
            // (before charging, so a husk born this turn survives one full turn)
            int cellCount = entityCount; // spawns below append to the pool
            for (int i = 0; i < cellCount; i++)
            {
                Entity *cell = &entities[i];
                if ((cell->kind != ENTITY_HEX_CELL) || (cell->huskTurns == 0)) continue;

                cell->huskTurns--;
                if (cell->huskTurns > 0) continue;
                LOG("INFO: FIREBALL: husk at (q=%d, r=%d) cooled, card returned to hand\n", cell->q, cell->r);
                PlaySound(sndReturn);
                ReturnPlacedCard(cell);
            }

            // fireballs resolve BEFORE the enemies act: a full-charge blast
            // kills the enemy that was about to smash it, and an enemy
            // moving toward the blast zone is judged where it stands NOW --
            // it walks into the crater, not into the explosion
            {
                bool conductive[LEY_W][LEY_W];
                bool live[LEY_W][LEY_W];
                ComputeLeylineNetwork(player, conductive, live);
                for (int i = 0; i < cellCount; i++)
                {
                    Entity *cell = &entities[i];
                    if ((cell->kind != ENTITY_HEX_CELL) || (cell->value == 0) || (cell->cardKind != CARD_FIREBALL)) continue;
                    if (!LeylineTapped(live, cell->q, cell->r)) continue; // disconnected: no charge

                    cell->fireballCharge++;
                    LOG("INFO: FIREBALL: charged %d/%d at (q=%d, r=%d)\n",
                        cell->fireballCharge, FIREBALL_CHARGE_NEEDED, cell->q, cell->r);
                    if (cell->fireballCharge >= FIREBALL_CHARGE_NEEDED) DetonateFireball(cell);
                }
            }

            EnemyAIExecuteIntents();

            // hex curses wear off one stack per committed turn (fresh hexes
            // were set with +1 so this tick never eats their first fight)
            for (int i = 0; i < entityCount; i++)
            {
                Entity *cursed = &entities[i];
                if ((cursed->kind != ENTITY_ENEMY) || (cursed->hexedTurns == 0)) continue;
                cursed->hexedTurns--;
                if (cursed->hexedTurns == 0) LOG("INFO: HEX: curse wore off enemy at (q=%d, r=%d)\n", cursed->q, cursed->r);
            }

            // every remaining placed card burns one turn of field lifetime
            for (int i = 0; i < cellCount; i++)
            {
                Entity *cell = &entities[i];
                if ((cell->kind != ENTITY_HEX_CELL) || (cell->value == 0)) continue;

                cell->turnLifetime--;
                if (cell->turnLifetime > 0) continue;

                LOG("INFO: CARD: %s lvl %d expired, returned to hand from (q=%d, r=%d)\n",
                    cardKindNames[cell->cardKind], (int)cell->cardLevel, cell->q, cell->r);
                PlaySound(sndReturn);
                ReturnPlacedCard(cell);
            }

            // every 5th turn the center crystal drops a card gift on a free
            // tile AROUND itself -- its own tile is solid (one gift at a
            // time: uncollected gifts don't stack)
            if ((gameTurn % CRYSTAL_GIFT_PERIOD) == 0)
            {
                static const int giftDirs[6][2] = {{1, 0}, {1, -1}, {0, -1}, {-1, 0}, {-1, 1}, {0, 1}};
                bool taken = false;
                for (int i = 0; i < entityCount; i++)
                {
                    if (entities[i].kind == ENTITY_PICKUP) taken = true;
                }
                int start = GetRandomValue(0, 5);
                for (int d = 0; !taken && (d < 6); d++)
                {
                    int gq = giftDirs[(start + d) % 6][0];
                    int gr = giftDirs[(start + d) % 6][1];

                    bool tileFree = true;
                    for (int i = 0; i < entityCount; i++)
                    {
                        const Entity *other = &entities[i];
                        if (((other->kind == ENTITY_ENEMY) || (other->kind == ENTITY_PLAYER)) && (other->q == gq) && (other->r == gr)) tileFree = false;
                        if ((other->kind == ENTITY_HEX_CELL) && (other->q == gq) && (other->r == gr) &&
                            ((other->value > 0) || (other->huskTurns > 0))) tileFree = false;
                    }
                    if (!tileFree) continue;

                    Entity *gift = EntitySpawn(ENTITY_PICKUP);
                    if (gift != NULL)
                    {
                        gift->q = gq;
                        gift->r = gr;
                        gift->position = HexAxialToWorld(gq, gr);
                        gift->cardKind = (CardKind)GetRandomValue(CARD_LEYLINE, CARD_WARD);
                        gift->tint = cardKindTints[gift->cardKind];
                        gift->modelIndex = (kindModelIndex[gift->cardKind] >= 0) ? kindModelIndex[gift->cardKind] : 0;
                        LOG("INFO: PICKUP: crystal dropped a %s card at (q=%d, r=%d)\n", cardKindNames[gift->cardKind], gq, gr);
                    }
                    break;
                }
            }

            // every 7th turn another enemy crawls onto the board, on a free
            // tile kept off the player's back
            if ((gameTurn % ENEMY_SPAWN_PERIOD) == 0)
            {
                for (int attempt = 0; attempt < 40; attempt++)
                {
                    int q = GetRandomValue(-GRID_RADIUS, GRID_RADIUS);
                    int r = GetRandomValue(-GRID_RADIUS, GRID_RADIUS);
                    if (!HexOnBoard(q, r) || HexTileSolid(q, r)) continue;
                    if ((player != NULL) && (HexDistance(q, r, player->q, player->r) < 3)) continue;

                    bool tileFree = true;
                    for (int i = 0; i < entityCount; i++)
                    {
                        const Entity *other = &entities[i];
                        if ((other->kind == ENTITY_ENEMY) && (other->q == q) && (other->r == r)) tileFree = false;
                        if ((other->kind == ENTITY_HEX_CELL) && (other->q == q) && (other->r == r) &&
                            ((other->value > 0) || (other->huskTurns > 0))) tileFree = false;
                    }
                    if (!tileFree) continue;

                    LOG("INFO: ENEMY: reinforcement spawned at (q=%d, r=%d)\n", q, r);
                    SpawnEnemy(q, r);
                    break;
                }
            }

            EnemyAIUpdate(); // new turn started: re-plan
        }
    }
    //----------------------------------------------------------------------------------

    // Draw
    //----------------------------------------------------------------------------------
    // Render game screen to a texture,
    // it could be useful for scaling or further shader postprocessing
    //
    // aurora sky -- rendered into the scene texture: it is both the visible
    // background and what the gem materials refract
    //

    BeginTextureMode(sceneTexture);
    float auroraTime = (float)GetTime();
    SetShaderValue(auroraShader, auroraTimeLoc, &auroraTime, SHADER_UNIFORM_FLOAT);
    SetShaderValue(auroraShader, auroraIntensityLoc, &auroraIntensity, SHADER_UNIFORM_FLOAT);
    BeginShaderMode(auroraShader);
    DrawTexturePro(whiteTexture, (Rectangle){0, 0, 1, 1},
                   (Rectangle){0, 0, (float)screenWidth, (float)screenHeight},
                   (Vector2){0, 0}, 0.0f, WHITE);
    EndShaderMode();
    EndTextureMode();

    // This frame's camera basis + optics for both gem shaders
    UpdateGemShader(gemShader, &gemLocs, 0.0f);
    UpdateGemShader(inverseGemShader, &inverseGemLocs, gemMilkiness);

    //
    // triangle glow mask -- the triangles alone, half res, on black; whatever
    // lands here blooms in the composite step (no lights involved)
    //

    float airTime = (float)GetTime();
    BeginTextureMode(glowTexture);
    ClearBackground(BLACK);
    BeginMode3D(camera);
    DrawAirTriangles(airTime);
    EndMode3D();
    // Lever knob gem halo: a soft icy disc at the knob (half-res screen
    // coords); the blur legs turn it into the gem's glow
    if (centerGemLoaded)
    {
        float haloR = 20.0f * leverScale * (0.9f + 0.1f * sinf(airTime * 2.2f)); // Gentle pulse
        Vector2 haloAt = {leverKnobScreenPos.x / 2.0f, leverKnobScreenPos.y / 2.0f};
        DrawCircleGradient(haloAt, haloR, (Color){120, 170, 255, 255}, BLANK);
        DrawCircleGradient(haloAt, haloR * 0.45f, (Color){235, 245, 255, 255}, BLANK); // Hot core
    }
    EndTextureMode();

    // Blur, horizontal leg into the scratch buffer
    float gw = (float)glowTexture.texture.width;
    float gh = (float)glowTexture.texture.height;
    float dirH[2] = {1.6f / gw, 0.0f};
    BeginTextureMode(blurTexture);
    ClearBackground(BLACK);
    SetShaderValue(blurShader, blurDirLoc, dirH, SHADER_UNIFORM_VEC2);
    BeginShaderMode(blurShader);
    DrawTexturePro(glowTexture.texture, (Rectangle){0, 0, gw, -gh},
                   (Rectangle){0, 0, gw, gh}, (Vector2){0, 0}, 0.0f, WHITE);
    EndShaderMode();
    EndTextureMode();

    // The world pass renders into its own texture so the air shards can
    // refract the finished board, not just the sky
    BeginTextureMode(worldTexture);
    ClearBackground(BLACK);

    // The sky as the visible background
    DrawTexturePro(sceneTexture.texture,
                   (Rectangle){0, 0, (float)screenWidth, -(float)screenHeight},
                   (Rectangle){0, 0, (float)screenWidth, (float)screenHeight},
                   (Vector2){0, 0}, 0.0f, WHITE);

    // 2.5D: the world is 3D, viewed through a tilted orthographic camera
    BeginMode3D(camera);

    //
    // water -- an endless plane under the board mirroring the aurora sky;
    // wave noise bends the reflection, fresnel fades it into deep teal
    //

    {
        float waterCamPos[3] = {camera.position.x, camera.position.y, camera.position.z};
        SetShaderValue(waterShader, waterViewPosLoc, waterCamPos, SHADER_UNIFORM_VEC3);
        SetShaderValue(waterShader, waterTimeLoc, &auroraTime, SHADER_UNIFORM_FLOAT);
        SetShaderValue(waterShader, waterIntensityLoc, &auroraIntensity, SHADER_UNIFORM_FLOAT);
        SetShaderValue(waterShader, waterWaveAmpLoc, &waterWaveAmp, SHADER_UNIFORM_FLOAT);
        SetShaderValue(waterShader, waterWaveScaleLoc, &waterWaveScale, SHADER_UNIFORM_FLOAT);
        BeginShaderMode(waterShader);
        rlBegin(RL_QUADS);
        rlColor4ub(255, 255, 255, 255);
        rlNormal3f(0.0f, 1.0f, 0.0f);
        rlVertex3f(-60.0f, WATER_LEVEL, -60.0f);
        rlVertex3f(-60.0f, WATER_LEVEL, 60.0f);
        rlVertex3f(60.0f, WATER_LEVEL, 60.0f);
        rlVertex3f(60.0f, WATER_LEVEL, -60.0f);
        rlEnd();
        rlDrawRenderBatchActive();
        EndShaderMode();
    }

    //
    // board tiles -- raised prisms, hover reads as a raised tile
    //

    { // Card shell per-frame uniforms; cardValue/danger are set per cell below
        float shellCamPos[3] = {camera.position.x, camera.position.y, camera.position.z};
        SetShaderValue(cardShellShader, cardShellViewPosLoc, shellCamPos, SHADER_UNIFORM_VEC3);
        SetShaderValue(cardShellShader, cardShellTimeLoc, &auroraTime, SHADER_UNIFORM_FLOAT);
    }

    // Fireball AOE preview heat, per tile: every tile a placed fireball will
    // hit glows ember-orange, rising as the charge banks (gentle pulse)
    float fireballHeat[LEY_W][LEY_W] = {0};
    {
        static const int aoeDirs[6][2] = {{1, 0}, {1, -1}, {0, -1}, {-1, 0}, {-1, 1}, {0, 1}};
        for (int i = 0; i < entityCount; i++)
        {
            const Entity *fb = &entities[i];
            if ((fb->kind != ENTITY_HEX_CELL) || (fb->value == 0) || (fb->cardKind != CARD_FIREBALL)) continue;
            float heat = (0.35f + 0.65f * (float)fb->fireballCharge / FIREBALL_CHARGE_NEEDED) * (0.75f + 0.25f * sinf(auroraTime * 3.0f + (float)fb->q));
            for (int d = -1; d < 6; d++) // -1 = the fireball's own tile
            {
                int nq = fb->q + ((d < 0) ? 0 : aoeDirs[d][0]);
                int nr = fb->r + ((d < 0) ? 0 : aoeDirs[d][1]);
                if (!HexOnBoard(nq, nr)) continue;
                if (heat > fireballHeat[nq + GRID_RADIUS][nr + GRID_RADIUS]) fireballHeat[nq + GRID_RADIUS][nr + GRID_RADIUS] = heat;
            }
        }
    }

    for (int i = 0; i < entityCount; i++)
    {
        const Entity *cell = &entities[i];
        if (cell->kind != ENTITY_HEX_CELL) continue;

        float drawSize = cell->radius * 0.92f; // Small gap between neighbour cells

        float height = cell->hovered ? HEX_TILE_HEIGHT_HOVER : HEX_TILE_HEIGHT;

        // Every tile is an inverse gem: the prism mesh drawn with the inverse
        // gem material refracts the aurora sky as its negative. GenMeshCylinder
        // already places a wall vertex on -Z, so it matches DrawCylinder's
        // pointy-top layout with no extra rotation.
        Matrix tileTransform = MatrixMultiply(
            MatrixScale(drawSize, height, drawSize),
            MatrixTranslate(cell->position.x, cell->position.y, cell->position.z));
        DrawMesh(hexPrismMesh, inverseGemMaterial, tileTransform);

        // Tile art on the prism top: a plain textured quad, deliberately NOT
        // drawn with the gem material -- the art obscures the effect and the
        // effect leaves the art alone. The art hex is flat-top, the board is
        // pointy-top, hence the 90-degree yaw; art vertex-to-vertex = 2R.
        // Each tile picks one atlas variant by a stable hash of its axial
        // coords, so the board reads varied instead of wallpapered
        int variant = ((cell->q * 5 + cell->r * 9) % TILE_ART_VARIANTS + TILE_ART_VARIANTS) % TILE_ART_VARIANTS;
        float u0 = (float)variant / TILE_ART_VARIANTS;
        float u1 = (float)(variant + 1) / TILE_ART_VARIANTS;

        float artHalf = drawSize * tileArtScale;
        rlPushMatrix();
        rlTranslatef(cell->position.x, height + 0.005f, cell->position.z);
        rlRotatef(90.0f, 0.0f, 1.0f, 0.0f);
        rlSetTexture(tileArtTexture.id);
        rlBegin(RL_QUADS);
        rlColor4ub(255, 255, 255, 255);
        rlNormal3f(0.0f, 1.0f, 0.0f);
        rlTexCoord2f(u0, 0.0f);
        rlVertex3f(-artHalf, 0.0f, -artHalf);
        rlTexCoord2f(u0, 1.0f);
        rlVertex3f(-artHalf, 0.0f, artHalf);
        rlTexCoord2f(u1, 1.0f);
        rlVertex3f(artHalf, 0.0f, artHalf);
        rlTexCoord2f(u1, 0.0f);
        rlVertex3f(artHalf, 0.0f, -artHalf);
        rlEnd();
        rlSetTexture(0);
        rlPopMatrix();

        // Hover/placed color reads as a translucent wash over the gem
        if (cell->hovered)
            DrawCylinder(cell->position, drawSize, drawSize, height + 0.01f, 6, Fade(SKYBLUE, 0.35f));
        else if (cell->value > 0)
            DrawCylinder(cell->position, drawSize, drawSize, height + 0.01f, 6, Fade(cell->tint, 0.30f));
        DrawCylinderWires(cell->position, drawSize, drawSize, height, 6, DARKGRAY);

        // Fireball blast preview: in-range tiles wear an ember wash
        float heat = fireballHeat[cell->q + GRID_RADIUS][cell->r + GRID_RADIUS];
        if (heat > 0.0f)
            DrawCylinder(cell->position, drawSize, drawSize, height + 0.012f, 6, Fade((Color){255, 120, 50, 255}, 0.30f * heat));

        // Spent fireball husk: charred mounds and a breathing ember squat on
        // the tile until the cooldown hands the card back
        if (cell->huskTurns > 0)
        {
            DrawSphere((Vector3){cell->position.x, height + 0.10f, cell->position.z}, 0.24f, (Color){28, 24, 26, 255});
            DrawSphere((Vector3){cell->position.x + 0.14f, height + 0.07f, cell->position.z - 0.08f}, 0.13f, (Color){24, 20, 22, 255});
            float ember = 0.5f + 0.5f * sinf(auroraTime * 2.2f + (float)cell->q);
            DrawSphere((Vector3){cell->position.x - 0.08f, height + 0.14f, cell->position.z + 0.1f}, 0.045f,
                       (Color){255, (unsigned char)(90 + 60 * ember), 30, (unsigned char)(120 + 100 * ember)});
        }

        // Placed card: the cell wears its hex-form model on the prism top
        if ((cell->value > 0) && (cardModelCount > 0))
        {
            rlDisableBackfaceCulling();
            rlPushMatrix();
            rlTranslatef(cell->position.x, height + 0.01f, cell->position.z);
            // Merged cards read bigger: +18% per level above 1
            float levelScale = 1.0f + 0.18f * (float)(cell->cardLevel - CARD_LVL_1);
            DrawModel(hexModels[cell->modelIndex % cardModelCount], (Vector3){0},
                      hexModelScale * HEX_SIZE / HEX_MODEL_R * levelScale, WHITE);
            rlPopMatrix();

            // The generated shell over the gem, worn ONLY while a strike is
            // telegraphed on this card: the pulsing red danger skin
            if (cell->inDanger)
            {
                float shellValue = (float)cell->cardLevel;
                float shellDanger = 1.0f;
                SetShaderValue(cardShellShader, cardShellValueLoc, &shellValue, SHADER_UNIFORM_FLOAT);
                SetShaderValue(cardShellShader, cardShellDangerLoc, &shellDanger, SHADER_UNIFORM_FLOAT);
                Matrix shellTransform = MatrixMultiply(
                    MatrixScale(levelScale, 1.0f, levelScale),
                    MatrixTranslate(cell->position.x, height + 0.01f, cell->position.z));
                DrawMesh(cell->cardShellMesh, cardShellMaterial, shellTransform);
            }
            rlEnableBackfaceCulling();
        }

        // Fireball emplacement: one golden ember orbits per banked charge
        if ((cell->value > 0) && (cell->cardKind == CARD_FIREBALL))
        {
            for (int k = 0; k < cell->fireballCharge; k++)
            {
                float ang = auroraTime * 1.8f + (float)k * (2.0f * PI / (float)FIREBALL_CHARGE_NEEDED);
                Vector3 ep = {cell->position.x + cosf(ang) * 0.55f,
                              height + 0.32f + sinf(auroraTime * 2.6f + (float)k) * 0.06f,
                              cell->position.z + sinf(ang) * 0.55f};
                DrawSphere(ep, 0.07f, (Color){255, 230, 150, 230});
                DrawSphere(ep, 0.13f, (Color){255, 120, 40, 80});
            }
        }
    }

    //
    // card pickups -- the center crystal's gifts: the card's hex form
    // spinning and bobbing over the tile until the player steps on it
    //

    for (int i = 0; i < entityCount; i++)
    {
        const Entity *gift = &entities[i];
        if (gift->kind != ENTITY_PICKUP) continue;

        DrawCylinder(gift->position, HEX_SIZE * 0.5f, HEX_SIZE * 0.5f, HEX_TILE_HEIGHT + 0.015f, 6,
                     Fade(gift->tint, 0.25f + 0.1f * sinf(auroraTime * 3.0f)));
        if (cardModelCount > 0)
        {
            rlDisableBackfaceCulling();
            rlPushMatrix();
            rlTranslatef(gift->position.x, HEX_TILE_HEIGHT + 0.55f + sinf(auroraTime * 2.0f) * 0.1f, gift->position.z);
            rlRotatef(auroraTime * 40.0f, 0.0f, 1.0f, 0.0f);
            DrawModel(hexModels[gift->modelIndex % cardModelCount], (Vector3){0},
                      hexModelScale * HEX_SIZE / HEX_MODEL_R * 0.6f, WHITE);
            rlPopMatrix();
            rlEnableBackfaceCulling();
        }
    }

    //
    // landing ghost -- the grabbed card projected onto the hovered tile, riding
    // the reticle's sprung position rather than the raw cursor
    //

    if (hoveredCell != NULL)
    {
        for (int i = 0; i < entityCount; i++)
        {
            const Entity *card = &entities[i];
            if ((card->kind != ENTITY_CARD) || !card->selected || (card->cardMode != CARD_HEX_FORM)) continue;

            bool ghostMerge = (hoveredCell->value > 0) && (hoveredCell->cardKind == card->cardKind) && (hoveredCell->cardLevel == card->cardLevel) && (hoveredCell->cardLevel < CARD_LVL_4);
            bool ghostValid = ((hoveredCell->value == 0) && (hoveredCell->huskTurns == 0) &&
                               !HexTileSolid(hoveredCell->q, hoveredCell->r)) ||
                              ghostMerge;
            if (cardModelCount > 0)
            {
                // Translucent hex-form model, red-tinted over an invalid target
                Color ghost = ghostValid ? Fade(WHITE, 0.55f) : Fade(RED, 0.55f);
                rlDisableBackfaceCulling();
                rlPushMatrix();
                rlTranslatef(reticle.position.x, reticle.position.y, reticle.position.z);
                DrawModel(hexModels[card->modelIndex % cardModelCount], (Vector3){0},
                          hexModelScale * HEX_SIZE / HEX_MODEL_R, ghost);
                rlPopMatrix();
                rlEnableBackfaceCulling();
            }
            else
            {
                // Fallback: transparent hex fill + outline in the card's color
                Color ghost = ghostValid ? card->tint : RED; // Red = invalid target
                float size = HEX_SIZE * 0.92f;
                DrawCylinder(reticle.position, size, size, 0.08f, 6, Fade(ghost, 0.35f));
                DrawCylinderWires(reticle.position, size, size, 0.08f, 6, Fade(ghost, 0.85f));
            }
            break; // Only one card can be grabbed
        }
    }

    //
    // enemy -- the architect demo's spiky icosphere hovering over its tile:
    // VS spikes + breath, one FS pass drawing the dark body, pastel fresnel
    // rim, and the pastel mesh lines from barycentric coords (WebGL-safe)
    //

    if (enemyModelLoaded)
    {
        float enemyCamPos[3] = {camera.position.x, camera.position.y, camera.position.z};
        float enemyZero = 0.0f;
        SetShaderValue(enemyShader, enemyLocs.time, &airTime, SHADER_UNIFORM_FLOAT);
        SetShaderValue(enemyShader, enemyLocs.spikeLen, &enemySpikeLen, SHADER_UNIFORM_FLOAT);
        SetShaderValue(enemyShader, enemyLocs.spikeDensity, &enemySpikeDensity, SHADER_UNIFORM_FLOAT);
        SetShaderValue(enemyShader, enemyLocs.viewPos, enemyCamPos, SHADER_UNIFORM_VEC3);
        SetShaderValue(enemyShader, enemyLocs.rim, &enemyRim, SHADER_UNIFORM_FLOAT);
        SetShaderValue(enemyShader, enemyLocs.lineWidth, &enemyLineWidth, SHADER_UNIFORM_FLOAT);
        SetShaderValue(enemyShader, enemyLocs.expand, &enemyZero, SHADER_UNIFORM_FLOAT);
        SetShaderValue(enemyOutlineShader, enemyOutlineLocs.time, &airTime, SHADER_UNIFORM_FLOAT);
        SetShaderValue(enemyOutlineShader, enemyOutlineLocs.spikeLen, &enemySpikeLen, SHADER_UNIFORM_FLOAT);
        SetShaderValue(enemyOutlineShader, enemyOutlineLocs.spikeDensity, &enemySpikeDensity, SHADER_UNIFORM_FLOAT);

        for (int i = 0; i < entityCount; i++)
        {
            const Entity *enemy = &entities[i];
            if (enemy->kind != ENTITY_ENEMY) continue;
            DrawEnemyInstance(enemy, airTime);
        }
    }

    //
    // player -- billboarded mage sprite standing on his tile, idle animation
    // from the sheet's top row; stands on the reticle while dragged
    //

    for (int i = 0; i < entityCount; i++)
    {
        const Entity *p = &entities[i];
        if (p->kind != ENTITY_PLAYER) continue;
        DrawPlayerSprite(p);
    }

    DrawHoverReticle(); // Drawn late in 3D so its alpha blends over the tiles
    DrawImpactEffect();
    EndMode3D();
    EndTextureMode();

    //
    // leyline layer -- the conduit network alone on a transparent layer:
    // iridescent arcs between adjacent conducting tiles, spinning runes on
    // leyline tiles, pulses flowing where the network reaches the player.
    // The composite below stamps this layer's silhouette as a pen outline
    //

    {
        static const int leyDirs[6][2] = {{1, 0}, {1, -1}, {0, -1}, {-1, 0}, {-1, 1}, {0, 1}};
        bool conductive[LEY_W][LEY_W];
        bool endpoint[LEY_W][LEY_W] = {0};
        bool live[LEY_W][LEY_W];
        ComputeLeylineNetwork(player, conductive, live);

        // placed non-leyline cards are endpoints: they tap into an adjacent
        // live leyline but never extend the chain themselves
        for (int i = 0; i < entityCount; i++)
        {
            const Entity *cell = &entities[i];
            if (cell->kind != ENTITY_HEX_CELL) continue;
            if ((cell->value > 0) && !cell->isLeyline) endpoint[cell->q + GRID_RADIUS][cell->r + GRID_RADIUS] = true;
        }

        BeginTextureMode(leylineTexture);
        ClearBackground(BLANK);
        BeginMode3D(camera);

        // runes -- a spinning flat diamond on every leyline tile
        for (int i = 0; i < entityCount; i++)
        {
            const Entity *cell = &entities[i];
            if ((cell->kind != ENTITY_HEX_CELL) || !cell->isLeyline) continue;
            bool lit = live[cell->q + GRID_RADIUS][cell->r + GRID_RADIUS];
            Color rc = lit ? LeylineIridColor((float)cell->q * 0.31f + (float)cell->r * 0.17f, airTime) : LeylineDimColor();
            float rs = 0.26f + (lit ? 0.04f * sinf(airTime * 2.0f + (float)(cell->q + cell->r)) : 0.0f);
            rlPushMatrix();
            rlTranslatef(cell->position.x, HEX_TILE_HEIGHT + 0.02f, cell->position.z);
            rlRotatef(airTime * (lit ? 30.0f : 6.0f), 0.0f, 1.0f, 0.0f);
            rlBegin(RL_TRIANGLES);
            rlColor4ub(rc.r, rc.g, rc.b, 255);
            for (int k = 0; k < 4; k++)
            {
                float a0 = DEG2RAD * 90.0f * (float)k;
                float a1 = DEG2RAD * 90.0f * (float)(k + 1);
                rlVertex3f(0.0f, 0.0f, 0.0f);
                rlVertex3f(cosf(a0) * rs, 0.0f, sinf(a0) * rs);
                rlVertex3f(cosf(a1) * rs, 0.0f, sinf(a1) * rs);
            }
            rlEnd();
            rlPopMatrix();
        }

        // beams -- iridescent arcs between adjacent conducting tiles, drawn
        // as a color pass then an additive glow repeat for live spans
        for (int pass = 0; pass < 2; pass++)
        {
            if (pass == 1) BeginBlendMode(BLEND_ADDITIVE);
            for (int q = -GRID_RADIUS; q <= GRID_RADIUS; q++)
            {
                for (int r = -GRID_RADIUS; r <= GRID_RADIUS; r++)
                {
                    bool aCon = HexOnBoard(q, r) && conductive[q + GRID_RADIUS][r + GRID_RADIUS];
                    bool aEnd = HexOnBoard(q, r) && endpoint[q + GRID_RADIUS][r + GRID_RADIUS];
                    if (!aCon && !aEnd) continue;
                    for (int d = 0; d < 3; d++) // half the dirs = each pair once
                    {
                        int nq = q + leyDirs[d][0];
                        int nr = r + leyDirs[d][1];
                        if (!HexOnBoard(nq, nr)) continue;
                        bool bCon = conductive[nq + GRID_RADIUS][nr + GRID_RADIUS];
                        bool bEnd = endpoint[nq + GRID_RADIUS][nr + GRID_RADIUS];
                        bool lit;
                        if (aCon && bCon)
                        {
                            // chain span: live only when both ends reach the player
                            lit = live[q + GRID_RADIUS][r + GRID_RADIUS] && live[nq + GRID_RADIUS][nr + GRID_RADIUS];
                        }
                        else if (aCon && bEnd)
                        {
                            lit = live[q + GRID_RADIUS][r + GRID_RADIUS]; // tap into the endpoint
                        }
                        else if (aEnd && bCon)
                        {
                            lit = live[nq + GRID_RADIUS][nr + GRID_RADIUS];
                        }
                        else
                        {
                            continue; // endpoint-endpoint: spells never chain each other
                        }
                        if ((pass == 1) && !lit) continue;

                        Vector3 a = HexAxialToWorld(q, r);
                        Vector3 b = HexAxialToWorld(nq, nr);
                        float seed = (a.x + b.x) * 0.35f + (a.z + b.z) * 0.21f;
                        unsigned char alpha = (pass == 0) ? 255 : 150;
                        rlSetLineWidth((pass == 0) ? leyWidth : leyWidth * 2.6f);
                        rlBegin(RL_LINES);
                        const int steps = 12;
                        for (int k = 0; k < steps; k++)
                        {
                            Color c0 = lit ? LeylineIridColor((float)k / steps + seed, airTime) : LeylineDimColor();
                            Color c1 = lit ? LeylineIridColor((float)(k + 1) / steps + seed, airTime) : LeylineDimColor();
                            Vector3 p0 = LeylineBezier(a, b, (float)k / steps);
                            Vector3 p1 = LeylineBezier(a, b, (float)(k + 1) / steps);
                            rlColor4ub(c0.r, c0.g, c0.b, alpha);
                            rlVertex3f(p0.x, p0.y, p0.z);
                            rlColor4ub(c1.r, c1.g, c1.b, alpha);
                            rlVertex3f(p1.x, p1.y, p1.z);
                        }
                        rlEnd();

                        // pulses riding live beams, in the additive pass
                        if ((pass == 1) && lit)
                        {
                            for (int k = 0; k < 3; k++)
                            {
                                float pt = airTime * 0.9f + (float)k / 3.0f;
                                pt -= floorf(pt);
                                Vector3 pp = LeylineBezier(a, b, pt);
                                Color pc = LeylineIridColor(pt + seed, airTime);
                                DrawSphere(pp, 0.055f, (Color){pc.r, pc.g, pc.b, 230});
                                DrawSphere(pp, 0.10f, (Color){pc.r, pc.g, pc.b, 70});
                            }
                        }
                    }
                }
            }
            if (pass == 1) EndBlendMode();
        }
        rlSetLineWidth(1.0f);

        // spell endpoints get an activation ring when any neighbouring
        // leyline tile is live (adjacency taps them in; they never conduct)
        BeginBlendMode(BLEND_ADDITIVE);
        for (int i = 0; i < entityCount; i++)
        {
            const Entity *cell = &entities[i];
            if (cell->kind != ENTITY_HEX_CELL) continue;
            if (!endpoint[cell->q + GRID_RADIUS][cell->r + GRID_RADIUS]) continue;
            bool tapped = false;
            for (int d = 0; d < 6; d++)
            {
                int nq = cell->q + leyDirs[d][0];
                int nr = cell->r + leyDirs[d][1];
                if (HexOnBoard(nq, nr) && live[nq + GRID_RADIUS][nr + GRID_RADIUS]) tapped = true;
            }
            if (!tapped) continue;
            float ring = cell->radius * (0.62f + 0.06f * sinf(airTime * 3.0f));
            Color rc = LeylineIridColor((float)cell->q * 0.31f + (float)cell->r * 0.17f, airTime);
            DrawCylinder((Vector3){cell->position.x, HEX_TILE_HEIGHT + 0.03f, cell->position.z},
                         ring, ring, 0.02f, 24, (Color){rc.r, rc.g, rc.b, 70});
        }
        EndBlendMode();

        EndMode3D();
        EndTextureMode();
    }

    BeginTextureMode(target);
    ClearBackground(BLACK);

    // The finished world as this frame's base layer
    DrawTexturePro(worldTexture.texture,
                   (Rectangle){0, 0, (float)screenWidth, -(float)screenHeight},
                   (Rectangle){0, 0, (float)screenWidth, (float)screenHeight},
                   (Vector2){0, 0}, 0.0f, WHITE);

    // Leyline pen-outline composite: the network layer's silhouette stamped
    // in ink around itself (offset draws tinted black keep only the alpha
    // shape), then the color layer once on top -- outline and art never
    // share a layer, so the contour stays one continuous pen line
    {
        Rectangle leySrc = {0, 0, (float)screenWidth, -(float)screenHeight};
        static const int leyOff[8][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}, {-1, -1}, {-1, 1}, {1, -1}, {1, 1}};
        if (leyInk > 0.1f)
        {
            for (int k = 0; k < 8; k++)
            {
                DrawTexturePro(leylineTexture.texture, leySrc,
                               (Rectangle){(float)leyOff[k][0] * leyInk, (float)leyOff[k][1] * leyInk,
                                           (float)screenWidth, (float)screenHeight},
                               (Vector2){0, 0}, 0.0f, (Color){12, 10, 18, 255});
            }
        }
        DrawTexturePro(leylineTexture.texture, leySrc,
                       (Rectangle){0, 0, (float)screenWidth, (float)screenHeight},
                       (Vector2){0, 0}, 0.0f, WHITE);
    }

    BeginMode3D(camera);

    //
    // center crystal -- the demo's big quartz hovering over the middle of the
    // board, refracting the world behind it; slow turn + gentle bob
    //

    if (centerGemLoaded)
    {
        rlDisableBackfaceCulling();
        rlPushMatrix();
        rlTranslatef(0.0f, 2.4f + sinf(airTime * 0.8f) * 0.15f, 0.0f);
        rlRotatef(airTime * 12.0f, 0.0f, 1.0f, 0.0f);
        DrawModel(centerGemModel, (Vector3){0}, centerGemScale, WHITE);
        rlPopMatrix();
        rlEnableBackfaceCulling();
    }

    //
    // over-the-crystal redraw -- the gem is drawn on top of the blitted world,
    // so anything standing nearer the camera than it must draw again or it
    // reads as behind the glass. The gem's depth is in the buffer, so these
    // depth-tested redraws resolve the overlap per pixel at the silhouette
    //

    if (centerGemLoaded)
    {
        Vector3 crystalFwd = Vector3Normalize(Vector3Subtract(camera.target, camera.position));
        Vector3 crystalCenter = {0.0f, 2.4f + sinf(airTime * 0.8f) * 0.15f, 0.0f};
        float crystalDepth = Vector3DotProduct(Vector3Subtract(crystalCenter, camera.position), crystalFwd);

        for (int i = 0; i < entityCount; i++)
        {
            const Entity *p = &entities[i];
            if (p->kind != ENTITY_PLAYER) continue;
            if (Vector3DotProduct(Vector3Subtract(p->position, camera.position), crystalFwd) >= crystalDepth) continue;
            DrawPlayerSprite(p);
        }

        for (int i = 0; i < entityCount && enemyModelLoaded; i++)
        {
            const Entity *enemy = &entities[i];
            if (enemy->kind != ENTITY_ENEMY) continue;
            if (Vector3DotProduct(Vector3Subtract(enemy->position, camera.position), crystalFwd) >= crystalDepth) continue;
            DrawEnemyInstance(enemy, airTime);
        }
    }

    //
    // air particles -- gem shards refracting the rendered world beneath them,
    // and the sharp pass of the glow triangles
    //

    if (shardModelCount > 0)
    {
        rlDisableBackfaceCulling();
        for (int i = 0; i < (int)airShardCount && i < MAX_AIR_SHARDS; i++)
        {
            const AirParticle *p = &airShards[i];
            Vector3 pos = AirParticlePosition(p, airTime);
            rlPushMatrix();
            rlTranslatef(pos.x, pos.y, pos.z);
            rlRotatef(airTime * p->spin + p->phase * 57.3f, p->axis.x, p->axis.y, p->axis.z);
            DrawModel(shardModels[p->variant % shardModelCount], (Vector3){0},
                      p->scale * airParticleScale, WHITE);
            rlPopMatrix();
        }
        rlEnableBackfaceCulling();
    }
    DrawAirTriangles(airTime);

    //
    // hand cards -- glb card models standing at the fan positions (converted
    // from the screen-space springs), facing the camera like the player
    // sprite, with hover press tilt and an additive foil-shine pass. Drawn
    // last with depth testing off so the hand always sits on top of the board
    //

    if (cardModelCount > 0)
    {
        // Face the camera from any orbit: yaw around Y toward the camera,
        // then pitch back by its elevation
        Vector3 camOffset = Vector3Subtract(camera.position, camera.target);
        float yawDeg = RAD2DEG * atan2f(camOffset.x, camOffset.z);
        float elevDeg = RAD2DEG * atan2f(camOffset.y, sqrtf(camOffset.x * camOffset.x + camOffset.z * camOffset.z));
        float shineTime = (float)GetTime();
        rlDrawRenderBatchActive(); // Flush pending 3D quads before changing state
        rlDisableDepthTest();
        rlDisableBackfaceCulling();
        for (int i = 0; i < entityCount; i++)
        {
            Entity *card = &entities[i];
            if (card->kind != ENTITY_CARD) continue;
            if (card->selected && (card->cardMode == CARD_HEX_FORM)) continue; // Ghosting on the board
            if (card->alpha < 0.01f) continue;

            Vector3 pos = ScreenToHandPlane((Vector2){card->position.x, card->position.y});
            Model model = cardModels[card->modelIndex % cardModelCount];
            // Hand cards are screen-space UI: scale with the ortho zoom
            // (fovy, 16 = home) so they keep a constant on-screen size
            float scale = cardModelScale * cardScale * (camera.fovy / 16.0f);

            rlPushMatrix();
            rlTranslatef(pos.x, pos.y, pos.z);
            rlRotatef(yawDeg, 0.0f, 1.0f, 0.0f);   // Turn toward the camera's yaw
            rlRotatef(-elevDeg, 1.0f, 0.0f, 0.0f); // Then face it dead on
            rlRotatef(card->pressTilt.y * cardPressTiltDeg, 1.0f, 0.0f, 0.0f);
            rlRotatef(card->pressTilt.x * cardPressTiltDeg, 0.0f, 1.0f, 0.0f);

            // With depth testing off the model's mesh order is the paint
            // order; the exporter adds layers back-to-front, so this holds.
            // Dead-in-hand cards read as gray stone, and skip the foil shine
            bool cardDead = (card->huskTurns > 0);
            Color bodyTint = cardDead ? (Color){105, 105, 115, 255} : WHITE;
            DrawModel(model, (Vector3){0}, scale, Fade(bodyTint, card->alpha));

            // Foil shine: the same model again through the shine shader
            if (!cardDead)
            {
                float phase = (float)i * 0.19f;
                SetShaderValue(shineShader, shineTimeLoc, &shineTime, SHADER_UNIFORM_FLOAT);
                SetShaderValue(shineShader, shinePhaseLoc, &phase, SHADER_UNIFORM_FLOAT);
                Shader saved[MAX_CARD_MODELS * 2] = {0};
                for (int m = 0; m < model.materialCount && m < (int)(sizeof(saved) / sizeof(saved[0])); m++)
                {
                    saved[m] = model.materials[m].shader;
                    model.materials[m].shader = shineShader;
                }
                BeginBlendMode(BLEND_ADDITIVE);
                DrawModel(model, (Vector3){0}, scale, WHITE);
                EndBlendMode();
                for (int m = 0; m < model.materialCount && m < (int)(sizeof(saved) / sizeof(saved[0])); m++)
                {
                    model.materials[m].shader = saved[m];
                }
            }

            rlPopMatrix();
        }
        rlEnableBackfaceCulling();
        rlEnableDepthTest();
    }

    EndMode3D();

    //
    // triangle glow composite -- vertical blur leg, additive: the bloom lands
    // wherever the triangles are on screen
    //

    float dirV[2] = {0.0f, 1.6f / gh};
    SetShaderValue(blurShader, blurDirLoc, dirV, SHADER_UNIFORM_VEC2);
    BeginBlendMode(BLEND_ADDITIVE);
    BeginShaderMode(blurShader);
    DrawTexturePro(blurTexture.texture, (Rectangle){0, 0, gw, -gh},
                   (Rectangle){0, 0, (float)screenWidth, (float)screenHeight},
                   (Vector2){0, 0}, 0.0f, Fade(WHITE, triGlowStrength > 1.0f ? 1.0f : triGlowStrength));
    EndShaderMode();
    EndBlendMode();

    //
    // card rectangles -- fallback when no glb models are loaded; with models
    // the hand is drawn in the 3D pass and only the value overlays here
    //

    for (int i = 0; i < entityCount && cardModelCount > 0; i++)
    {
        const Entity *card = &entities[i];
        if ((card->kind != ENTITY_CARD) || (card->alpha < 0.01f)) continue;
        if (card->selected && (card->cardMode == CARD_HEX_FORM)) continue;

        bool isPiece = (card->cardLevel >= CARD_LVL_4);
        const char *valueText = (card->huskTurns > 0)  ? TextFormat("%s zzz", cardKindNames[card->cardKind])
                                : isPiece              ? TextFormat("%s", exodiaPieceShort[card->cardKind])
                                                       : TextFormat("%s %d", cardKindNames[card->cardKind], (int)card->cardLevel);
        Color valueColor = (card->huskTurns > 0) ? GRAY
                           : isPiece             ? (Color){255, 220, 60, 255}
                                                 : card->tint;
        int vx = (int)card->position.x - MeasureText(valueText, 24) / 2;
        int vy = (int)(card->position.y + CARD_HEIGHT * cardScale * 0.18f);
        DrawTextOutlined(valueText, vx, vy, 24, Fade(valueColor, card->alpha));
    }

    for (int i = 0; i < entityCount && cardModelCount == 0; i++)
    {
        const Entity *card = &entities[i];
        if ((card->kind != ENTITY_CARD) || (card->alpha < 0.01f)) continue;

        // Colored border behind a plain body, art comes later; tilt follows
        // the eased position so cards straighten as they slide
        float rotation = (card->position.x - (float)screenWidth / 2.0f) * 0.02f;
        Vector2 center = {card->position.x, card->position.y};

        Rectangle border = {center.x, center.y, CARD_WIDTH * cardScale + 6.0f, CARD_HEIGHT * cardScale + 6.0f};
        Rectangle body = {center.x, center.y, CARD_WIDTH * cardScale, CARD_HEIGHT * cardScale};
        DrawRectanglePro(border, (Vector2){border.width / 2.0f, border.height / 2.0f}, rotation, Fade(card->tint, card->alpha));
        DrawRectanglePro(body, (Vector2){body.width / 2.0f, body.height / 2.0f}, rotation, Fade(card->hovered ? WHITE : RAYWHITE, card->alpha));

        DrawText(TextFormat("%s %d", cardKindNames[card->cardKind], (int)card->cardLevel),
                 (int)center.x - 5, (int)center.y - 10, 20, Fade(DARKGRAY, card->alpha));
    }

    //
    // card tooltip -- the hovered card's tooltip buffer in a dark strip above
    // the fan; grabbed cards keep quiet
    //

    for (int i = 0; i < entityCount; i++)
    {
        const Entity *card = &entities[i];
        if ((card->kind != ENTITY_CARD) || !card->hovered || card->selected) continue;
        if (card->tooltip[0] == '\0') continue;

        const int tipSize = 20;
        int tw = MeasureText(card->tooltip, tipSize);
        int tx = (int)card->position.x - tw / 2;
        int ty = (int)(card->position.y - CARD_HEIGHT * cardScale / 2.0f) - tipSize - 16;
        // Keep the strip on screen when an edge card is hovered
        if (tx < 8) tx = 8;
        if (tx + tw > screenWidth - 8) tx = screenWidth - 8 - tw;
        DrawRectangle(tx - 8, ty - 6, tw + 16, tipSize + 12, Fade(BLACK, 0.72f));
        DrawText(card->tooltip, tx, ty, tipSize, RAYWHITE);
        break; // Fan cards overlap: only the frontmost hovered one speaks
    }

    //
    // placed card countdown -- turns left on the field, on the tile's lower
    // edge (white, flipping red on the last turn)
    //

    for (int i = 0; i < entityCount; i++)
    {
        const Entity *cell = &entities[i];
        if ((cell->kind != ENTITY_HEX_CELL) || (cell->value == 0)) continue;

        Vector3 edge = cell->position;
        edge.y = HEX_TILE_HEIGHT;
        edge.z += HEX_SIZE * 0.55f; // Toward the camera-facing edge of the tile
        Vector2 at = GetWorldToScreen(edge, camera);
        const char *turnsText = TextFormat("%d", cell->turnLifetime);
        const int turnsSize = 22;
        int tx2 = (int)at.x - MeasureText(turnsText, turnsSize) / 2;
        Color turnsColor = (cell->turnLifetime <= 1) ? (Color){235, 70, 60, 255} : RAYWHITE;
        DrawTextOutlined(turnsText, tx2, (int)at.y, turnsSize, turnsColor);
    }

    //
    // health bars -- rows of heart icons
    //

    for (int i = 0; i < entityCount; i++)
    {
        const Entity *bar = &entities[i];
        if (bar->kind != ENTITY_HEALTH_BAR) continue;

        float scale = 1.5f; // 32px pixel-art hearts drawn at 48px
        float step = (float)heartTexture.width * scale + 4.0f;

        for (int h = 0; h < bar->maxHealth; h++)
        {
            Vector2 pos = {bar->position.x + (float)h * step, bar->position.y};
            Color tint = (h < bar->health) ? WHITE : Fade(DARKGRAY, 0.4f); // Lost hearts are dimmed
            DrawTextureEx(heartTexture, pos, 0.0f, scale, tint);
        }
    }

    //
    // turn lever -- slide-to-commit widget, owns the turn change
    //

    TurnLever();

    //
    // lever knob gem -- the grab point rendered as a small spinning crystal
    // riding the knob's screen position, refracting the world behind it
    //

    if (centerGemLoaded)
    {
        // Constant screen size: world scale tracks the ortho zoom (fovy)
        float knobGemScale = 12.0f * leverScale * camera.fovy / (float)screenHeight;
        Vector3 knobWorld = ScreenToHandPlane(leverKnobScreenPos);
        BeginMode3D(camera);
        rlDisableBackfaceCulling();
        rlDisableDepthTest();
        rlPushMatrix();
        rlTranslatef(knobWorld.x, knobWorld.y, knobWorld.z);
        rlRotatef(airTime * 30.0f, 0.0f, 1.0f, 0.0f);
        // The button crystal gets its own milkiness (eased up while hovered);
        // the frame-start uniform update restores the shared gem shader for
        // everyone else next frame
        SetShaderValue(gemShader, gemLocs.milkiness, &leverGemMilkyNow, SHADER_UNIFORM_FLOAT);
        DrawModel(centerGemModel, (Vector3){0}, knobGemScale, WHITE);
        rlPopMatrix();
        rlEnableDepthTest();
        rlEnableBackfaceCulling();
        EndMode3D();

        // Bloom over the crystal itself: the half-res mask halo only survives
        // around the silhouette, so the shine on top is drawn here directly
        float shineR = 24.0f * leverScale * (0.9f + 0.1f * sinf(airTime * 2.2f));
        BeginBlendMode(BLEND_ADDITIVE);
        DrawCircleGradient(leverKnobScreenPos, shineR, (Color){70, 110, 200, 255}, BLANK);
        DrawCircleGradient(leverKnobScreenPos, shineR * 0.4f, (Color){120, 150, 220, 255}, BLANK);
        EndBlendMode();
    }

    DrawIntent(airTime);

    if (hoveredCell != NULL) DrawText(TextFormat("cell (q=%d, r=%d)", hoveredCell->q, hoveredCell->r), 24, screenHeight - 40, 20, LIGHTGRAY);

    if ((frameCounter / 20) % 2) DrawTextOutlined("hex merge time!", screenWidth / 2 - MeasureText("hex merge time!", 30) / 2, 28, 30, MAROON);

    // Spawn countdowns: turns until the crystal's next card gift and the
    // next enemy reinforcement (both fire on the lever pull they hit 0 on)
    {
        int nextCard = CRYSTAL_GIFT_PERIOD - (gameTurn % CRYSTAL_GIFT_PERIOD);
        int nextEnemy = ENEMY_SPAWN_PERIOD - (gameTurn % ENEMY_SPAWN_PERIOD);
        const char *spawnText = TextFormat("next card %d   next enemy %d", nextCard, nextEnemy);
        DrawTextOutlined(spawnText, screenWidth / 2 - MeasureText(spawnText, 20) / 2, 64, 20, LIGHTGRAY);
    }

    DrawRectangleLinesEx((Rectangle){0, 0, screenWidth, screenHeight}, 16, BLACK);

    //
    // modal screens -- start and game over: a dark veil over the live board
    // with outlined title text; a click flips the state (and the click that
    // dismisses them never reaches the board, uiWantsMouse is held all frame)
    //

    if (gameState != GAME_STATE_PLAYING)
    {
        DrawRectangle(0, 0, screenWidth, screenHeight, Fade(BLACK, 0.62f));

        const char *title = (gameState == GAME_STATE_START) ? "HEX MERGE"
                            : (gameState == GAME_STATE_WIN) ? "EXODIA!"
                                                            : "GAME OVER";
        Color titleColor = (gameState == GAME_STATE_OVER) ? (Color){235, 70, 60, 255} : (Color){255, 220, 60, 255};
        DrawTextOutlined(title, screenWidth / 2 - MeasureText(title, 64) / 2, screenHeight / 2 - 110, 64, titleColor);

        if (gameState == GAME_STATE_START)
        {
            const char *lines[] = {
                "place cards, merge equals to level them up",
                "leylines power your spells -- keep them connected",
                "click the crystal to end the turn",
            };
            for (int li = 0; li < 3; li++)
            {
                DrawTextOutlined(lines[li], screenWidth / 2 - MeasureText(lines[li], 20) / 2, screenHeight / 2 - 8 + li * 28, 20, RAYWHITE);
            }
        }
        else if (gameState == GAME_STATE_WIN)
        {
            const char *ranText = TextFormat("all four pieces assembled in %d turns", gameTurn);
            DrawTextOutlined(ranText, screenWidth / 2 - MeasureText(ranText, 24) / 2, screenHeight / 2 - 36, 24, RAYWHITE);

            // The four exodia pieces in a row, bobbing: gold-pipped mini
            // cards, one per kind (winning means all four are in the hand)
            const float pieceW = 74.0f;
            const float pieceH = 86.0f;
            const float pieceGap = 18.0f;
            float rowX = (float)screenWidth / 2.0f - (4.0f * pieceW + 3.0f * pieceGap) / 2.0f;
            for (int k = CARD_LEYLINE; k < NUM_CARD_KINDS; k++)
            {
                float bob = sinf((float)GetTime() * 2.0f + (float)k * 0.9f) * 3.0f;
                float px = rowX + (float)(k - CARD_LEYLINE) * (pieceW + pieceGap);
                float py = (float)screenHeight / 2.0f - 4.0f + bob;
                // Tinted border as a slightly larger rounded rect under the body
                DrawRectangleRounded((Rectangle){px - 3, py - 3, pieceW + 6, pieceH + 6}, 0.2f, 6, cardKindTints[k]);
                DrawRectangleRounded((Rectangle){px, py, pieceW, pieceH}, 0.2f, 6, (Color){24, 18, 28, 255});
                // Piece name in exodia gold, the kind that forged it beneath
                const char *pieceName = exodiaPieceShort[k];
                DrawTextOutlined(pieceName, (int)(px + pieceW / 2.0f) - MeasureText(pieceName, 16) / 2,
                                 (int)(py + 24), 16, (Color){255, 220, 60, 255});
                DrawTextOutlined(cardKindNames[k], (int)(px + pieceW / 2.0f) - MeasureText(cardKindNames[k], 16) / 2,
                                 (int)(py + 54), 16, cardKindTints[k]);
            }
        }
        else
        {
            const char *ranText = TextFormat("you lasted %d turns", gameTurn);
            DrawTextOutlined(ranText, screenWidth / 2 - MeasureText(ranText, 24) / 2, screenHeight / 2 - 4, 24, RAYWHITE);
        }

        // The pulsing prompt, and the click that acts on it
        const char *prompt = (gameState == GAME_STATE_START) ? "click to start"
                             : (gameState == GAME_STATE_WIN) ? "click to play again"
                                                             : "click to try again";
        float promptPulse = 0.65f + 0.35f * sinf((float)GetTime() * 3.0f);
        DrawTextOutlined(prompt, screenWidth / 2 - MeasureText(prompt, 26) / 2, screenHeight / 2 + 96, 26,
                         Fade((Color){255, 220, 60, 255}, promptPulse));

        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !igGetIO_Nil()->WantCaptureMouse)
        {
            if (gameState != GAME_STATE_START) ResetGame();
            gameState = GAME_STATE_PLAYING;
            PlaySound(sndLever);
        }
    }

    EndTextureMode();

    // Render to screen (main framebuffer)
    BeginDrawing();
    ClearBackground(BLACK);

    // Screen shake: eased offset + slight roll, kicked by a card placement
    float shakeAmp = 0.0f;
    if (shakeTimeLeft > 0.0f)
    {
        shakeTimeLeft -= GetFrameTime();
        if (shakeTimeLeft < 0.0f) shakeTimeLeft = 0.0f;
        float shakeT = shakeTimeLeft / SHAKE_DURATION;
        shakeAmp = shakeT * shakeT; // Eased decay over the shake duration
    }
    float wobble = (float)frameCounter;
    Vector2 shakeOffset = {sinf(wobble * 1.9f) * 9.0f * shakeAmp, cosf(wobble * 2.3f) * 9.0f * shakeAmp};
    float shakeRoll = sinf(wobble * 1.4f) * 1.5f * shakeAmp;

    // Draw render texture to screen, scaled if required (drawn about its
    // center so the shake roll pivots on the middle of the view)
    DrawTexturePro(target.texture, (Rectangle){0, 0, (float)target.texture.width, -(float)target.texture.height},
                   (Rectangle){(float)screenWidth / 2.0f + shakeOffset.x, (float)screenHeight / 2.0f + shakeOffset.y, (float)target.texture.width, (float)target.texture.height},
                   (Vector2){(float)target.texture.width / 2.0f, (float)target.texture.height / 2.0f}, shakeRoll, WHITE);

    // ImGui UI, drawn on top of the scaled game texture. The dev menu ships
    // hidden; the backquote key toggles it
    static bool devMenuOpen = false;
    if (IsKeyPressed(KEY_GRAVE)) devMenuOpen = !devMenuOpen;

    rlImGuiBegin();

    if (devMenuOpen)
    {
        igBegin("hex debug", NULL, 0);
#if !defined(BUILD_GIT_HASH)
#define BUILD_GIT_HASH "dev"
#endif
        // Which build is this? Commit at configure time + compile timestamp --
        // compare against git to tell whether a Pages deploy actually updated
        igText("build %s  (%s %s)", BUILD_GIT_HASH, __DATE__, __TIME__);
        igText("entities: %d/%d", entityCount, MAX_ENTITIES);
        igText("mouse cell: (q=%d, r=%d)", mouseHexQ, mouseHexR);
        for (int i = 0; i < entityCount; i++)
        {
            const Entity *enemy = &entities[i];
            if (enemy->kind != ENTITY_ENEMY) continue;
            const char *what = (enemy->enemyIntentKind == ENEMY_INTENT_STRIKE)   ? "STRIKE"
                               : (enemy->enemyIntentKind == ENEMY_INTENT_ATTACK) ? "ATTACK"
                               : (enemy->enemyIntentKind == ENEMY_INTENT_MOVE)   ? "move"
                                                                                 : "hold";
            igText("enemy (%d,%d)%s: %s -> (%d,%d)", enemy->q, enemy->r,
                   (enemy->hexedTurns > 0) ? TextFormat(" HEXED %d", enemy->hexedTurns) : "", what,
                   enemy->enemyIntentQ, enemy->enemyIntentR);
        }

        if (igButton("deal card", (ImVec2_c){0, 0})) SpawnCard();
        igSameLine(0.0f, 8.0f);
        if (igButton("test win screen", (ImVec2_c){0, 0})) gameState = GAME_STATE_WIN;

        if (igButton("save tweaks", (ImVec2_c){0, 0})) SaveTweaks();
        igSameLine(0.0f, 8.0f);
        if (igButton("reload tweaks", (ImVec2_c){0, 0})) LoadTweaks();

        // Impact burst tweakers; the button replays the effect at the board center
        igSliderFloat("impact size", &impactLightSize, 0.5f, 4.0f, "%.2f", 0);
        igSliderFloat("impact height", &impactLightHeight, 0.0f, 2.0f, "%.2f", 0);
        igSliderFloat("impact anim speed", &impactAnimSpeed, 0.1f, 3.0f, "%.2f", 0);

        // Size tweakers for the visual elements
        igSliderFloat("player size", &playerSpriteSize, 0.5f, 3.0f, "%.2f", 0);
        igSliderFloat("card size", &cardScale, 0.5f, 1.5f, "%.2f", 0);
        igSliderFloat("reticle size", &reticleScale, 0.4f, 1.6f, "%.2f", 0);
        igSliderFloat("lever scale", &leverScale, 1.0f, 4.0f, "%.2f", 0);
        igSliderFloat("aurora", &auroraIntensity, 0.0f, 2.0f, "%.2f", 0);
        igSliderFloat("water wave amp", &waterWaveAmp, 0.0f, 1.2f, "%.2f", 0);
        igSliderFloat("water wave scale", &waterWaveScale, 0.3f, 4.0f, "%.2f", 0);
        igSliderFloat("enemy size", &enemySize, 0.2f, 2.0f, "%.2f", 0);
        igSliderFloat("enemy spikes", &enemySpikeLen, 0.0f, 1.2f, "%.2f", 0);
        igSliderFloat("enemy spike density", &enemySpikeDensity, 0.05f, 1.0f, "%.2f", 0);
        igSliderFloat("enemy rim", &enemyRim, 0.0f, 1.5f, "%.2f", 0);
        igSliderFloat("enemy line width", &enemyLineWidth, 1.0f, 6.0f, "%.1f", 0);
        igSliderFloat("enemy red outline", &enemyOutline, 0.0f, 0.15f, "%.3f", 0);
        igSliderFloat("ley hue", &leyHue, 0.0f, 1.0f, "%.2f", 0);
        igSliderFloat("ley iridescence", &leyIrid, 0.0f, 1.0f, "%.2f", 0);
        igSliderFloat("ley ink", &leyInk, 0.0f, 5.0f, "%.1f", 0);
        igSliderFloat("ley arc", &leyArc, 0.0f, 0.8f, "%.2f", 0);
        igSliderFloat("ley width", &leyWidth, 1.0f, 8.0f, "%.1f", 0);

        // Gem tile optics
        igSliderFloat("gem ior", &gemIor, 1.0f, 2.4f, "%.2f", 0);
        igSliderFloat("gem strength", &gemStrength, 0.0f, 1.0f, "%.2f", 0);
        igSliderFloat("gem dispersion", &gemChromatic, 0.0f, 0.15f, "%.3f", 0);
        igSliderFloat("gem milkiness", &gemMilkiness, 0.0f, 1.0f, "%.2f", 0);
        igSliderFloat("lever gem milkiness", &leverGemMilkiness, 0.0f, 1.0f, "%.2f", 0);
        igSliderFloat("tile art size", &tileArtScale, 0.3f, 1.4f, "%.2f", 0);
        igSliderFloat("mouse sway", &mouseSwayAmount, 0.0f, 3.0f, "%.2f", 0);

        // Air particle tweakers
        igSliderFloat("air shards", &airShardCount, 0.0f, (float)MAX_AIR_SHARDS, "%.0f", 0);
        igSliderFloat("air triangles", &airTriCount, 0.0f, (float)MAX_AIR_TRIS, "%.0f", 0);
        igSliderFloat("air particle scale", &airParticleScale, 0.3f, 3.0f, "%.2f", 0);
        igSliderFloat("tri glow", &triGlowStrength, 0.0f, 1.0f, "%.2f", 0);
        igSliderFloat("center gem scale", &centerGemScale, 0.0f, 3.0f, "%.2f", 0);

        if (igButton("reset camera", (ImVec2_c){0, 0}))
        {
            camera.position = (Vector3){0.0f, 14.0f, 12.0f};
            camera.target = (Vector3){0.0f, 0.0f, 0.0f};
            camera.fovy = 16.0f;
        }

        // Card model tweakers
        igText("card models: %d", cardModelCount);
        igSliderFloat("card model scale", &cardModelScale, 0.3f, 2.0f, "%.2f", 0);
        igSliderFloat("hex model scale", &hexModelScale, 0.5f, 1.5f, "%.2f", 0);
        igSliderFloat("press tilt", &cardPressTiltDeg, 0.0f, 45.0f, "%.0f", 0);
        if (igButton("test impact", (ImVec2_c){0, 0}))
        {
            impact = (ImpactFx){.position = HexAxialToWorld(0, 0), .tint = (Color){255, 203, 0, 255}, .age = 0.0f, .active = true};
        }

        for (int i = 0; i < entityCount; i++)
        {
            if (entities[i].kind == ENTITY_HEALTH_BAR)
            {
                igSliderInt("health", &entities[i].health, 0, entities[i].maxHealth, "%d", 0);
                break;
            }
        }
        igEnd();
    }

    rlImGuiEnd();

    EndDrawing();

    // Debug: F10 dumps the frame to debug_shot.png next to the binary
    if (IsKeyPressed(KEY_F10)) TakeScreenshot("debug_shot.png");
    { // TEMP
        static int shotFrame = 0;
        if (++shotFrame == 95) TakeScreenshot("det_check.png");
    }
    //----------------------------------------------------------------------------------
}

//------------------------------------------------------------------------------------
// Program main entry point
//------------------------------------------------------------------------------------
int main(void)
{
#if !defined(_DEBUG)
    SetTraceLogLevel(LOG_NONE); // Disable raylib trace log messages
#endif

    // Initialization
    //--------------------------------------------------------------------------------------
    InitWindow(screenWidth, screenHeight, "raylib gamejam template");

    // All SFX are synthesized at startup, no audio assets (web: the browser
    // holds audio until the first input, then it just starts working)
    InitAudioDevice();
    sndPickup = SynthSound(500.0f, 750.0f, 0.07f, 0.0f, 0.35f);
    sndPlace = SynthSound(240.0f, 130.0f, 0.12f, 0.25f, 0.55f);
    sndMerge = SynthSound(520.0f, 1180.0f, 0.22f, 0.0f, 0.45f);
    sndReturn = SynthSound(700.0f, 280.0f, 0.16f, 0.0f, 0.35f);
    sndHit = SynthSound(150.0f, 55.0f, 0.24f, 0.55f, 0.8f);
    sndLever = SynthSound(320.0f, 70.0f, 0.14f, 0.35f, 0.55f);
    sndBoom = SynthSound(110.0f, 38.0f, 0.45f, 0.7f, 0.9f);

    // Render texture to draw, enables screen scaling
    // NOTE: If screen is scaled, mouse input should be scaled proportionally
    target = LoadRenderTexture(screenWidth, screenHeight);
    SetTextureFilter(target.texture, TEXTURE_FILTER_BILINEAR);

    rlImGuiSetup(true); // Dear ImGui with the dark theme

    // Load resources (desktop: resources/ sits next to the binary,
    // web: emscripten preloads it into the .data bundle)
    heartTexture = LoadTexture("resources/heart_icon_32x32.png");

    reticleTexture = LoadTexture("resources/highlight_slot_26x26.png");
    SetTextureFilter(reticleTexture, TEXTURE_FILTER_POINT); // Keep the pixel art crisp when scaled

    impactLightTexture = LoadTexture("resources/impact_light_8x256.png");
    SetTextureFilter(impactLightTexture, TEXTURE_FILTER_BILINEAR); // Smooth glow, not pixel art

    playerTexture = LoadTexture("resources/player_mage_8x64x32.png");
    SetTextureFilter(playerTexture, TEXTURE_FILTER_POINT); // Keep the pixel art crisp when scaled

    tileArtTexture = LoadTexture("resources/tile_clouds_variants_8x256.png");
    SetTextureFilter(tileArtTexture, TEXTURE_FILTER_BILINEAR); // Smooth vector art, not pixel art

    // Card models: every *_card.glb in resources/models with its *_hex.glb twin
    FilePathList modelFiles = LoadDirectoryFiles("resources/models");
    for (unsigned int i = 0; (i < modelFiles.count) && (cardModelCount < MAX_CARD_MODELS); i++)
    {
        const char *path = modelFiles.paths[i];
        int suffix = TextFindIndex(path, "_card.glb");
        if (suffix < 0) continue;

        char hexPath[512] = {0};
        TextCopy(hexPath, path);
        TextCopy(hexPath + suffix, "_hex.glb");
        if (!FileExists(hexPath))
        {
            LOG("WARNING: MODEL: %s has no matching _hex.glb\n", path);
            continue;
        }

        cardModels[cardModelCount] = LoadModel(path);
        hexModels[cardModelCount] = LoadModel(hexPath);
        for (int k = CARD_LEYLINE; k < NUM_CARD_KINDS; k++)
        {
            if (TextFindIndex(GetFileName(path), kindModelTags[k]) >= 0) kindModelIndex[k] = cardModelCount;
        }
        cardModelCount++;
        LOG("INFO: MODEL: loaded card/hex pair %s\n", path);
    }
    UnloadDirectoryFiles(modelFiles);

    // Foil shine shader, from the FS string in the globals
    shineShader = LoadShaderFromMemory(NULL, shineFS);
    shineTimeLoc = GetShaderLocation(shineShader, "time");
    shinePhaseLoc = GetShaderLocation(shineShader, "phase");

    // Aurora background shader + the 1x1 white canvas it draws on
    auroraShader = LoadShaderFromMemory(NULL, auroraFS);
    auroraTimeLoc = GetShaderLocation(auroraShader, "time");
    auroraIntensityLoc = GetShaderLocation(auroraShader, "intensity");

    waterShader = LoadShaderFromMemory(gemVS, waterFS);
    waterViewPosLoc = GetShaderLocation(waterShader, "viewPos");
    waterTimeLoc = GetShaderLocation(waterShader, "time");
    waterIntensityLoc = GetShaderLocation(waterShader, "intensity");
    waterWaveAmpLoc = GetShaderLocation(waterShader, "waveAmp");
    waterWaveScaleLoc = GetShaderLocation(waterShader, "waveScale");
    Image whiteImage = GenImageColor(1, 1, WHITE);
    whiteTexture = LoadTextureFromImage(whiteImage);
    UnloadImage(whiteImage);

    // Gem materials: two shader instances of the same source, one with the
    // color inversion baked on; both refract the aurora scene texture
    gemShader = LoadShaderFromMemory(gemVS, gemFS);
    inverseGemShader = LoadShaderFromMemory(gemVS, gemFS);
    gemLocs = GetGemShaderLocs(gemShader);
    inverseGemLocs = GetGemShaderLocs(inverseGemShader);
    int invertOff = 0;
    int invertOn = 1;
    SetShaderValue(gemShader, GetShaderLocation(gemShader, "invertColors"), &invertOff, SHADER_UNIFORM_INT);
    SetShaderValue(inverseGemShader, GetShaderLocation(inverseGemShader, "invertColors"), &invertOn, SHADER_UNIFORM_INT);

    sceneTexture = LoadRenderTexture(screenWidth, screenHeight);
    worldTexture = LoadRenderTexture(screenWidth, screenHeight);

    gemMaterial = LoadMaterialDefault();
    gemMaterial.shader = gemShader;
    gemMaterial.maps[MATERIAL_MAP_ALBEDO].texture = sceneTexture.texture;
    inverseGemMaterial = LoadMaterialDefault();
    inverseGemMaterial.shader = inverseGemShader;
    inverseGemMaterial.maps[MATERIAL_MAP_ALBEDO].texture = sceneTexture.texture;

    hexPrismMesh = GenMeshCylinder(1.0f, 1.0f, 6); // Unit hex prism, scaled per tile

    // Air particles: shard models become gem-material crystals refracting the
    // sky; triangles get the bloom buffers
    for (int i = 0; i < SHARD_MODEL_COUNT; i++)
    {
        const char *shardPath = TextFormat("resources/models/shard%d.glb", i);
        if (!FileExists(shardPath)) break;
        shardModels[shardModelCount] = LoadModel(shardPath);
        for (int m = 0; m < shardModels[shardModelCount].materialCount; m++)
        {
            shardModels[shardModelCount].materials[m].shader = gemShader;
            shardModels[shardModelCount].materials[m].maps[MATERIAL_MAP_ALBEDO].texture = worldTexture.texture;
        }
        shardModelCount++;
    }
    LOG("INFO: PARTICLES: %d shard models\n", shardModelCount);

    if (FileExists("resources/models/centergem.glb"))
    {
        centerGemModel = LoadModel("resources/models/centergem.glb");
        for (int m = 0; m < centerGemModel.materialCount; m++)
        {
            centerGemModel.materials[m].shader = gemShader;
            centerGemModel.materials[m].maps[MATERIAL_MAP_ALBEDO].texture = worldTexture.texture;
        }
        centerGemLoaded = true;
    }

    // Enemy: spiky icosphere from the architect demo (pastel vertex colors
    // baked into the glb), shaded by the enemy VS/FS pair in the globals
    if (FileExists("resources/models/enemy_ico.glb"))
    {
        enemyModel = LoadModel("resources/models/enemy_ico.glb");
        enemyShader = LoadShaderFromMemory(enemyVS, enemyFS);
        enemyOutlineShader = LoadShaderFromMemory(enemyVS, enemyOutlineFS);
        enemyLocs = GetEnemyShaderLocs(enemyShader);
        enemyOutlineLocs = GetEnemyShaderLocs(enemyOutlineShader);
        enemyModel.materials[0].shader = enemyShader;
        enemyModelLoaded = (enemyModel.meshCount > 0);
    }

    for (int i = 0; i < MAX_AIR_SHARDS; i++) airShards[i] = SpawnAirParticle(false, i);
    for (int i = 0; i < MAX_AIR_TRIS; i++) airTris[i] = SpawnAirParticle(true, MAX_AIR_SHARDS + i);

    leylineTexture = LoadRenderTexture(screenWidth, screenHeight);
    glowTexture = LoadRenderTexture(screenWidth / 2, screenHeight / 2);
    blurTexture = LoadRenderTexture(screenWidth / 2, screenHeight / 2);
    SetTextureFilter(glowTexture.texture, TEXTURE_FILTER_BILINEAR);
    SetTextureFilter(blurTexture.texture, TEXTURE_FILTER_BILINEAR);
    blurShader = LoadShaderFromMemory(NULL, blurFS);
    blurDirLoc = GetShaderLocation(blurShader, "dir");

    // Card shell shader: loaded before SpawnHexGrid generates the per-cell
    // shell meshes it draws
    cardShellShader = LoadShaderFromMemory(cardShellVS, cardShellFS);
    cardShellViewPosLoc = GetShaderLocation(cardShellShader, "viewPos");
    cardShellValueLoc = GetShaderLocation(cardShellShader, "cardValue");
    cardShellDangerLoc = GetShaderLocation(cardShellShader, "danger");
    cardShellTimeLoc = GetShaderLocation(cardShellShader, "time");
    cardShellMaterial = LoadMaterialDefault();
    cardShellMaterial.shader = cardShellShader;

    LoadTweaks(); // Tweakable globals populate from resources/tweaks.json

    leverTrackTexture = LoadTexture("resources/lever_track_128x16.png");
    leverFillTexture = LoadTexture("resources/lever_fill_120x8.png");
    leverKnobTexture = LoadTexture("resources/lever_knob_16x18.png");
    SetTextureFilter(leverTrackTexture, TEXTURE_FILTER_POINT);
    SetTextureFilter(leverFillTexture, TEXTURE_FILTER_POINT);
    SetTextureFilter(leverKnobTexture, TEXTURE_FILTER_POINT);

    // Entity pool: one allocation for the whole game, entities live in a
    // packed array with swap-back removal
    entities = (Entity *)calloc(MAX_ENTITIES, sizeof(Entity));
    entityCount = 0;

    SpawnHexGrid();
    ResetGame(); // Starting cast: hearts, hand, player, first enemy, first plan

#if defined(PLATFORM_WEB)
    emscripten_set_main_loop(UpdateDrawFrame, 60, 1);
#else
    SetTargetFPS(60); // Set our game frames-per-second
    //--------------------------------------------------------------------------------------

    // Main game loop
    while (!WindowShouldClose()) // Detect window close button
    {
        UpdateDrawFrame();
    }
#endif

    // De-Initialization
    //--------------------------------------------------------------------------------------
    rlImGuiShutdown();
    UnloadTexture(heartTexture);
    UnloadTexture(reticleTexture);
    UnloadTexture(impactLightTexture);
    UnloadTexture(playerTexture);
    UnloadTexture(tileArtTexture);
    UnloadTexture(leverTrackTexture);
    UnloadTexture(leverFillTexture);
    UnloadTexture(leverKnobTexture);
    for (int i = 0; i < cardModelCount; i++)
    {
        UnloadModel(cardModels[i]);
        UnloadModel(hexModels[i]);
    }
    UnloadShader(shineShader);
    UnloadShader(auroraShader);
    UnloadShader(waterShader);
    if (enemyModelLoaded)
    {
        UnloadModel(enemyModel);
        UnloadShader(enemyShader);
        UnloadShader(enemyOutlineShader);
    }
    UnloadShader(gemShader);
    UnloadShader(inverseGemShader);
    UnloadMesh(hexPrismMesh);
    UnloadRenderTexture(sceneTexture);
    UnloadRenderTexture(worldTexture);
    UnloadRenderTexture(leylineTexture);
    UnloadRenderTexture(glowTexture);
    UnloadRenderTexture(blurTexture);
    UnloadShader(blurShader);
    for (int i = 0; i < shardModelCount; i++) UnloadModel(shardModels[i]);
    if (centerGemLoaded) UnloadModel(centerGemModel);
    UnloadShader(cardShellShader);
    // Hex cells never despawn, so every cell still owns its shell mesh here
    for (int i = 0; i < entityCount; i++)
    {
        if (entities[i].kind == ENTITY_HEX_CELL) UnloadMesh(entities[i].cardShellMesh);
    }
    UnloadTexture(whiteTexture);
    UnloadRenderTexture(target);
    free(entities);

    UnloadSound(sndPickup);
    UnloadSound(sndPlace);
    UnloadSound(sndMerge);
    UnloadSound(sndReturn);
    UnloadSound(sndHit);
    UnloadSound(sndLever);
    UnloadSound(sndBoom);
    CloseAudioDevice();

    CloseWindow(); // Close window and OpenGL context
    //--------------------------------------------------------------------------------------

    return 0;
}
