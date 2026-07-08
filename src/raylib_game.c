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
} EntityKind;

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
    int frameCreated; // frameCounter value when this entity was spawned
} Entity;

#include "globals.h"

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

// Per-entity behavior, dispatched on kind
static void UpdateEntity(Entity *entity)
{
    switch (entity->kind)
    {
        case ENTITY_HEX_CELL:
        {
            entity->hovered = ((entity->q == mouseHexQ) && (entity->r == mouseHexR) && !uiWantsMouse);

            if (entity->hovered && IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
            {
                entity->selected = !entity->selected;
                LOG("INFO: HEX: clicked cell (q=%d, r=%d) selected=%d\n", entity->q, entity->r, entity->selected);
            }
        }
        break;
        default: break;
    }
}

// Per-entity drawing, dispatched on kind (called inside BeginMode3D)
static void DrawEntity(const Entity *entity)
{
    switch (entity->kind)
    {
        case ENTITY_HEX_CELL:
        {
            float drawSize = entity->radius * 0.92f; // Small gap between neighbour cells

            Color fill = entity->tint;
            if (entity->selected) fill = GOLD;
            if (entity->hovered) fill = entity->selected ? ORANGE : SKYBLUE;

            // Hover and selection read as raised tiles
            float height = HEX_TILE_HEIGHT;
            if (entity->selected)
                height = HEX_TILE_HEIGHT_SELECTED;
            else if (entity->hovered)
                height = HEX_TILE_HEIGHT_HOVER;

            // A 6-slice cylinder is a hexagonal prism; raylib places the first
            // vertex on +Z, which matches the pointy-top axial layout
            DrawCylinder(entity->position, drawSize, drawSize, height, 6, fill);
            DrawCylinderWires(entity->position, drawSize, drawSize, height, 6, DARKGRAY);
        }
        break;
        default: break;
    }
}

// Per-entity UI drawing, dispatched on kind (screen space, called after EndMode3D)
static void DrawEntityUI(const Entity *entity)
{
    switch (entity->kind)
    {
        case ENTITY_HEALTH_BAR:
        {
            float scale = 1.5f; // 32px pixel-art hearts drawn at 48px
            float step = (float)heartTexture.width * scale + 4.0f;

            for (int i = 0; i < entity->maxHealth; i++)
            {
                Vector2 pos = {entity->position.x + (float)i * step, entity->position.y};
                Color tint = (i < entity->health) ? WHITE : Fade(DARKGRAY, 0.4f); // Lost hearts are dimmed
                DrawTextureEx(heartTexture, pos, 0.0f, scale, tint);
            }
        }
        break;
        default: break;
    }
}

// Update and draw frame
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

    for (int i = 0; i < entityCount; i++) UpdateEntity(&entities[i]);
    //----------------------------------------------------------------------------------

    // Draw
    //----------------------------------------------------------------------------------
    // Render game screen to a texture,
    // it could be useful for scaling or further shader postprocessing
    BeginTextureMode(target);
    ClearBackground(RAYWHITE);

    // 2.5D: the world is 3D, viewed through a tilted perspective camera
    BeginMode3D(camera);

    for (int i = 0; i < entityCount; i++) DrawEntity(&entities[i]);

    EndMode3D();

    // 2D overlay on top of the 3D view
    for (int i = 0; i < entityCount; i++) DrawEntityUI(&entities[i]);

    for (int i = 0; i < entityCount; i++)
    {
        if ((entities[i].kind == ENTITY_HEX_CELL) && entities[i].hovered)
        {
            DrawText(TextFormat("cell (q=%d, r=%d)", entities[i].q, entities[i].r), 24, screenHeight - 40, 20, DARKGRAY);
            break;
        }
    }

    if ((frameCounter / 20) % 2) DrawText("hex merge time!", screenWidth / 2 - MeasureText("hex merge time!", 30) / 2, 28, 30, MAROON);

    DrawRectangleLinesEx((Rectangle){0, 0, screenWidth, screenHeight}, 16, BLACK);

    EndTextureMode();

    // Render to screen (main framebuffer)
    BeginDrawing();
    ClearBackground(RAYWHITE);

    // Draw render texture to screen, scaled if required
    DrawTexturePro(target.texture, (Rectangle){0, 0, (float)target.texture.width, -(float)target.texture.height},
                   (Rectangle){0, 0, (float)target.texture.width, (float)target.texture.height}, (Vector2){0, 0}, 0.0f, WHITE);

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
    UnloadRenderTexture(target);
    free(entities);

    CloseWindow(); // Close window and OpenGL context
    //--------------------------------------------------------------------------------------

    return 0;
}
