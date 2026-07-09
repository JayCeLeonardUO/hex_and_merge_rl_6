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
#include "rlgl.h" // Required for: textured quad of the hover reticle

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
#define HEX_TILE_HEIGHT 0.2f          // Resting tile thickness
#define HEX_TILE_HEIGHT_HOVER 0.4f    // Tile thickness while hovered
#define HEX_TILE_HEIGHT_SELECTED 0.7f // Tile thickness while selected

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
    NUM_ENTITY_KINDS
} EntityKind;

// Mode Of Card

typedef enum CardMode
{
    NOT_A_CARD = 0,
    CARD_HEX_FORM,
    CARD_REC_FORM,
} CardMode;

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

    // Gameplay members
    int value;     // Merge value, 0 means empty
    bool hovered;  // True while the mouse ray hits this entity (refreshed every frame)
    bool selected; // True when toggled by a click
    int health;    // Current health shown by a health bar
    int maxHealth; // Total hearts a health bar draws

    // Visual members
    Color tint;       // Base fill color
    float alpha;      // UI fade 0..1 (card: rectangle hides while ghosting on the board)
    int frameCreated; // frameCounter value when this entity was spawned

    // Parrent Hex cell
    int parent;

    // Mode of Card
    CardMode cardMode;

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

static HoverReticle reticle = {0}; // Persistent hover-effect state, updated in DrawHoverdHexEffect()

static ImpactFx impact = {0};      // Impact ring/dust of the last card placement
static float shakeTimeLeft = 0.0f; // Screen shake seconds remaining (kicked by a card placement)

// Impact light-burst tweakers, exposed in the hex debug ImGui window
static float impactLightSize = 1.7f;   // Half-size of the burst quad, in hex sizes
static float impactLightHeight = 0.3f; // Quad height above the tile base

static Entity *entities = NULL;   // Entity pool, one calloc at startup, packed array
static int entityCount = 0;       // Live entities in the pool
static int mouseHexQ = 0;         // Axial column under the mouse this frame
static int mouseHexR = 0;         // Axial row under the mouse this frame
static bool uiWantsMouse = false; // True when ImGui is using the mouse; board ignores clicks

// 2.5D camera: tilted orthographic view down at the board plane (y = 0);
// no perspective foreshortening, so tiles read as a flat iso board
static Camera3D camera = {
    .position = {0.0f, 14.0f, 12.0f},
    .target = {0.0f, 0.0f, 0.0f},
    .up = {0.0f, 1.0f, 0.0f},
    .fovy = 16.0f, // Orthographic: world-space height of the view, sized to fit the board
    .projection = CAMERA_ORTHOGRAPHIC,
};

// TODO: Define global variables here, recommended to make them static

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

// Spawn one ENTITY_HEX_CELL per cell of the hexagonal board
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
        }
    }
}

// Reticle step: integrate the hover reticle's spring/scale/spin toward the
// hovered tile (NULL hides it; the next hover re-snaps in place). Movement is
// an under-damped spring (overshoots and rebounds, not a lerp), scale follows
// spring speed, spin follows 1/scale^2 like a skater pulling their arms in.
static void UpdateHoverReticle(const Entity *hoveredCell)
{
    const float stiffness = 90.0f;     // Spring pull toward the tile center (1/s^2)
    const float damping = 6.0f;        // Well below critical (2*sqrtf(90) ~= 19): magnetic overshoot
    const float scaleRest = 0.85f;     // Half-size at rest, frames one tile
    const float scaleMax = 1.6f;       // Half-size cap while moving fast
    const float scalePerSpeed = 0.12f; // Extra half-size per world-unit/s of spring speed
    const float shrinkRate = 16.0f;    // Fast: snaps down onto the tile
    const float growRate = 5.0f;       // Slow: swells out smoothly when it starts moving
    const float spinFactor = 4.5f;     // Angular velocity = spinFactor/scale^2 (rad/s)
    const float floatHeight = 0.25f;   // Ride height above the tile top
    const float bobAmplitude = 0.06f;  // Slow sine bob around the ride height
    const float bobSpeed = 3.0f;       // Bob phase speed (rad/s)

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
    float tileTop = hoveredCell->selected ? HEX_TILE_HEIGHT_SELECTED : HEX_TILE_HEIGHT_HOVER;
    reticle.position.y = tileTop + floatHeight + sinf(reticle.bobPhase) * bobAmplitude;
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
    impact.age += dt;
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
static void SpawnCard(void)
{
    // Plain brace initializers: raylib's GOLD/PINK/... macros are compound
    // literals, which MSVC rejects in a static initializer
    static const Color palette[4] = {
        {255, 203, 0, 255},   // GOLD
        {255, 109, 194, 255}, // PINK
        {0, 158, 47, 255},    // LIME
        {135, 60, 190, 255},  // VIOLET
    };
    static int dealt = 0;

    Entity *card = EntitySpawn(ENTITY_CARD);
    if (card == NULL) return;

    card->value = (dealt % 4) + 1;
    card->tint = palette[dealt % 4];
    card->cardMode = CARD_REC_FORM;
    // Starts below the screen edge and springs up into its fan slot
    card->position = (Vector3){(float)screenWidth / 2.0f, (float)screenHeight + CARD_HEIGHT, 0.0f};
    dealt++;
}

// Update and draw frame: a flat sequence of system steps, each its own pass
// over the entity list
static void UpdateDrawFrame(void)
{
    // Update
    //----------------------------------------------------------------------------------
    frameCounter++;

    // When ImGui wants the mouse (hovering/dragging a UI window), the board
    // must not see clicks; WantCaptureMouse lags one frame, which is fine
    uiWantsMouse = igGetIO_Nil()->WantCaptureMouse;

    // Pick the cell under the mouse: cast a ray through the cursor and
    // intersect it with the board plane (y = 0)
    mouseHexQ = HEX_CELL_OFFBOARD;
    mouseHexR = HEX_CELL_OFFBOARD;
    Ray mouseRay = GetScreenToWorldRay(GetMousePosition(), camera);
    if (fabsf(mouseRay.direction.y) > 0.0001f)
    {
        float t = -mouseRay.position.y / mouseRay.direction.y;
        if (t > 0.0f)
        {
            float hitX = mouseRay.position.x + mouseRay.direction.x * t;
            float hitZ = mouseRay.position.z + mouseRay.direction.z * t;
            HexWorldToAxial(hitX, hitZ, &mouseHexQ, &mouseHexR);
        }
    }

    float dt = GetFrameTime();
    if (dt > 0.05f) dt = 0.05f; // Clamp frame hitches so the springs cannot explode
    Vector2 mouse = GetMousePosition();

    //
    // hex cell hover + click selection; remember the hovered cell for the
    // card, reticle, and debug steps below
    //

    Entity *hoveredCell = NULL;
    for (int i = 0; i < entityCount; i++)
    {
        Entity *cell = &entities[i];
        if (cell->kind != ENTITY_HEX_CELL) continue;

        cell->hovered = ((cell->q == mouseHexQ) && (cell->r == mouseHexR) && !uiWantsMouse);
        if (cell->hovered) hoveredCell = cell;

        if (cell->hovered && IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
        {
            cell->selected = !cell->selected;
            LOG("INFO: HEX: clicked cell (q=%d, r=%d) selected=%d\n", cell->q, cell->r, cell->selected);
        }
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

            Rectangle rect = {card->position.x - CARD_WIDTH / 2.0f, card->position.y - CARD_HEIGHT / 2.0f, CARD_WIDTH, CARD_HEIGHT};
            if (CheckCollisionPointRec(mouse, rect))
            {
                card->selected = true;
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
            if ((hoveredCell != NULL) && (hoveredCell->value == 0))
            {
                // The ghost commits: the tile takes the card's value and color
                hoveredCell->value = card->value;
                hoveredCell->tint = card->tint;

                shakeTimeLeft = SHAKE_DURATION;
                impact = (ImpactFx){.position = hoveredCell->position, .tint = card->tint, .age = 0.0f, .active = true};

                LOG("INFO: CARD: placed value %d at (q=%d, r=%d)\n", card->value, hoveredCell->q, hoveredCell->r);
                EntityDespawn(i);
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
            (float)screenWidth / 2.0f + fanIndex * CARD_FAN_STEP,
            (float)screenHeight - CARD_FAN_BOTTOM + fabsf(fanIndex) * CARD_FAN_ARC,
        };

        Rectangle rect = {card->position.x - CARD_WIDTH / 2.0f, card->position.y - CARD_HEIGHT / 2.0f, CARD_WIDTH, CARD_HEIGHT};
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
    }

    //
    // effect state -- reticle chases the hovered tile, impact ages out
    //

    UpdateHoverReticle(hoveredCell);
    UpdateImpactEffect();
    //----------------------------------------------------------------------------------

    // Draw
    //----------------------------------------------------------------------------------
    // Render game screen to a texture,
    // it could be useful for scaling or further shader postprocessing
    BeginTextureMode(target);
    ClearBackground(RAYWHITE);

    // 2.5D: the world is 3D, viewed through a tilted orthographic camera
    BeginMode3D(camera);

    //
    // board tiles -- raised prisms, fill shows hover/selection
    //

    for (int i = 0; i < entityCount; i++)
    {
        const Entity *cell = &entities[i];
        if (cell->kind != ENTITY_HEX_CELL) continue;

        float drawSize = cell->radius * 0.92f; // Small gap between neighbour cells

        Color fill = cell->tint;
        if (cell->selected) fill = GOLD;
        if (cell->hovered) fill = cell->selected ? ORANGE : SKYBLUE;

        // Hover and selection read as raised tiles
        float height = HEX_TILE_HEIGHT;
        if (cell->selected)
            height = HEX_TILE_HEIGHT_SELECTED;
        else if (cell->hovered)
            height = HEX_TILE_HEIGHT_HOVER;

        // A 6-slice cylinder is a hexagonal prism; raylib places the first
        // vertex on +Z, which matches the pointy-top axial layout
        DrawCylinder(cell->position, drawSize, drawSize, height, 6, fill);
        DrawCylinderWires(cell->position, drawSize, drawSize, height, 6, DARKGRAY);
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

            Color ghost = (hoveredCell->value == 0) ? card->tint : RED; // Red = invalid target

            // Transparent hex fill + outline in the card's color
            float size = HEX_SIZE * 0.92f;
            DrawCylinder(reticle.position, size, size, 0.08f, 6, Fade(ghost, 0.35f));
            DrawCylinderWires(reticle.position, size, size, 0.08f, 6, Fade(ghost, 0.85f));
            break; // Only one card can be grabbed
        }
    }

    DrawHoverReticle(); // Last in 3D so its alpha blends over the tiles
    DrawImpactEffect();

    EndMode3D();

    //
    // card rectangles -- blank cards fanned along the bottom edge
    //

    for (int i = 0; i < entityCount; i++)
    {
        const Entity *card = &entities[i];
        if ((card->kind != ENTITY_CARD) || (card->alpha < 0.01f)) continue;

        // Colored border behind a plain body, art comes later; tilt follows
        // the eased position so cards straighten as they slide
        float rotation = (card->position.x - (float)screenWidth / 2.0f) * 0.02f;
        Vector2 center = {card->position.x, card->position.y};

        Rectangle border = {center.x, center.y, CARD_WIDTH + 6.0f, CARD_HEIGHT + 6.0f};
        Rectangle body = {center.x, center.y, CARD_WIDTH, CARD_HEIGHT};
        DrawRectanglePro(border, (Vector2){border.width / 2.0f, border.height / 2.0f}, rotation, Fade(card->tint, card->alpha));
        DrawRectanglePro(body, (Vector2){body.width / 2.0f, body.height / 2.0f}, rotation, Fade(card->hovered ? WHITE : RAYWHITE, card->alpha));

        DrawText(TextFormat("%d", card->value), (int)center.x - 5, (int)center.y - 10, 20, Fade(DARKGRAY, card->alpha));
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

    if (hoveredCell != NULL) DrawText(TextFormat("cell (q=%d, r=%d)", hoveredCell->q, hoveredCell->r), 24, screenHeight - 40, 20, DARKGRAY);

    if ((frameCounter / 20) % 2) DrawText("hex merge time!", screenWidth / 2 - MeasureText("hex merge time!", 30) / 2, 28, 30, MAROON);

    DrawRectangleLinesEx((Rectangle){0, 0, screenWidth, screenHeight}, 16, BLACK);

    EndTextureMode();

    // Render to screen (main framebuffer)
    BeginDrawing();
    ClearBackground(RAYWHITE);

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

    // ImGui UI, drawn on top of the scaled game texture
    rlImGuiBegin();

    igBegin("hex debug", NULL, 0);
    igText("entities: %d/%d", entityCount, MAX_ENTITIES);
    igText("mouse cell: (q=%d, r=%d)", mouseHexQ, mouseHexR);

    int selectedCount = 0;
    for (int i = 0; i < entityCount; i++)
    {
        if ((entities[i].kind == ENTITY_HEX_CELL) && entities[i].selected) selectedCount++;
    }
    igText("selected: %d", selectedCount);

    if (igButton("clear selection", (ImVec2_c){0, 0}))
    {
        for (int i = 0; i < entityCount; i++) entities[i].selected = false;
    }

    if (igButton("deal card", (ImVec2_c){0, 0})) SpawnCard();

    // Impact burst tweakers; the button replays the effect at the board center
    igSliderFloat("impact size", &impactLightSize, 0.5f, 4.0f, "%.2f", 0);
    igSliderFloat("impact height", &impactLightHeight, 0.0f, 2.0f, "%.2f", 0);
    if (igButton("test impact", (ImVec2_c){0, 0}))
    {
        impact = (ImpactFx){.position = HexAxialToWorld(0, 0), .tint = (Color){255, 203, 0, 255}, .age = 0.0f, .active = true};
        shakeTimeLeft = SHAKE_DURATION;
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

    rlImGuiEnd();

    EndDrawing();
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

    // Entity pool: one allocation for the whole game, entities live in a
    // packed array with swap-back removal
    entities = (Entity *)calloc(MAX_ENTITIES, sizeof(Entity));
    entityCount = 0;

    SpawnHexGrid();

    Entity *healthBar = EntitySpawn(ENTITY_HEALTH_BAR);
    if (healthBar != NULL)
    {
        healthBar->position = (Vector3){24.0f, 64.0f, 0.0f}; // Screen pixels for UI kinds
        healthBar->health = 3;
        healthBar->maxHealth = 5;
    }

    for (int i = 0; i < 4; i++) SpawnCard(); // Starting hand

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
    UnloadRenderTexture(target);
    free(entities);

    CloseWindow(); // Close window and OpenGL context
    //--------------------------------------------------------------------------------------

    return 0;
}
