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
#include <string.h> // Required for:
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
#define HEX_SIZE 40.0f    // Hex circumradius (center to vertex) in pixels

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
    ENTITY_NONE = 0, // Free slot, skipped everywhere
    ENTITY_HEX_CELL, // One clickable cell of the hex board
} EntityKind;

// Fat struct: every possible member any entity could have, in one struct.
// An entity is initialized by tagging it with its kind; the update/draw
// switches on kind decide which members mean anything for it.
typedef struct Entity
{
    EntityKind kind; // Behavior tag, ENTITY_NONE = free slot

    // Spatial members
    Vector2 position; // Pixel position on screen (hex cell: precomputed center)
    Vector2 velocity; // Pixels per frame, for anything that moves
    float radius;     // Pixel radius (hex cell: circumradius)

    // Hex board members (axial coordinates, pointy-top)
    // https://www.redblobgames.com/grids/hexagons/
    int q; // Axial column, 0 at board center, range [-GRID_RADIUS, GRID_RADIUS]
    int r; // Axial row, 0 at board center, |q + r| <= GRID_RADIUS

    // Gameplay members
    int value;     // Merge value, 0 means empty
    bool hovered;  // True while the mouse is over this entity (refreshed every frame)
    bool selected; // True when toggled by a click

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

// Convert axial coordinates to a pixel center (pointy-top layout)
static Vector2 HexAxialToPixel(int q, int r)
{
    float sqrt3 = sqrtf(3.0f);
    Vector2 pixel = {0};
    pixel.x = screenWidth / 2.0f + HEX_SIZE * (sqrt3 * (float)q + sqrt3 / 2.0f * (float)r);
    pixel.y = screenHeight / 2.0f + HEX_SIZE * (3.0f / 2.0f) * (float)r;
    return pixel;
}

// Convert a pixel position to axial coordinates, rounded to the nearest cell
static void HexPixelToAxial(Vector2 pixel, int *q, int *r)
{
    float sqrt3 = sqrtf(3.0f);
    float px = (pixel.x - screenWidth / 2.0f) / HEX_SIZE;
    float py = (pixel.y - screenHeight / 2.0f) / HEX_SIZE;
    float qf = sqrt3 / 3.0f * px - 1.0f / 3.0f * py;
    float rf = 2.0f / 3.0f * py;

    // Cube rounding: round each cube coord, then fix the one that drifted most
    float x = qf;
    float z = rf;
    float y = -x - z;
    float rx = roundf(x);
    float ry = roundf(y);
    float rz = roundf(z);
    float dx = fabsf(rx - x);
    float dy = fabsf(ry - y);
    float dz = fabsf(rz - z);

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
            cell->position = HexAxialToPixel(q, r);
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

// Per-entity drawing, dispatched on kind
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

            DrawPoly(entity->position, 6, drawSize, 90.0f, fill);
            DrawPolyLinesEx(entity->position, 6, drawSize, 90.0f, 2.0f, DARKGRAY);

            if (entity->hovered) DrawText(TextFormat("cell (q=%d, r=%d)", entity->q, entity->r), 24, screenHeight - 40, 20, DARKGRAY);
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

    HexPixelToAxial(GetMousePosition(), &mouseHexQ, &mouseHexR);

    for (int i = 0; i < entityCount; i++) UpdateEntity(&entities[i]);
    //----------------------------------------------------------------------------------

    // Draw
    //----------------------------------------------------------------------------------
    // Render game screen to a texture,
    // it could be useful for scaling or further shader postprocessing
    BeginTextureMode(target);
    ClearBackground(RAYWHITE);

    for (int i = 0; i < entityCount; i++) DrawEntity(&entities[i]);

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

    // Entity pool: one allocation for the whole game, entities live in a
    // packed array with swap-back removal
    entities = (Entity *)calloc(MAX_ENTITIES, sizeof(Entity));
    entityCount = 0;

    SpawnHexGrid();

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
    UnloadRenderTexture(target);
    free(entities);

    CloseWindow(); // Close window and OpenGL context
    //--------------------------------------------------------------------------------------

    return 0;
}
