# Technical Specification: Automatic High-Resolution Mugshot Scaling

**System**: BiasedDoom Engine (Fork of GZDoom)
**Component**: Status Bar (SBARINFO)
**Status**: Draft

## 1. Executive Summary

This document proposes a modification to the BiasedDoom engine to enable **automatic downscaling** of high-resolution "mugshot" (status bar face) graphics. 

Currently, the `DrawMugShot` command renders graphics at their intrinsic display resolution. This works well for classic low-resolution assets but fails for high-definition replacements, which render oversized and obscure the screen. The proposed change extends the `SBARINFO` scripting language to accept explicit target dimensions, allowing the engine to mathematically scale high-fidelity assets to fit legacy UI constraints without manual texture definition tweaks.

## 2. Problem Description

### The "Intrinsic Size" Limitation
In the standard GZDoom engine, the Rendering Pipeline strictly respects a texture's defined `DisplayWidth` and `DisplayHeight`.
*   A classic Doom face is `24x29` pixels. It is rendered into a `24x29` box.
*   A high-resolution replacement might be `96x116` pixels (4x scale).
*   Without modification, the engine renders this 4x larger image pixel-for-pixel at the target coordinates, resulting in a giant face that breaks the UI bounds.

### The Inconsistency
The `DrawImage` command in `SBARINFO` already solves this via the `DrawInBox` or `ForceScale` flags, which constrain an image to a specific bounding box. However, the `DrawMugShot` command—specialized for the dynamic player face—**lacks this scaling capability**. It blindly renders the active sprite frame at its full resolution.

To fix this currently, modders must manually define `XScale` and `YScale` properties in global `TEXTURES` definitions for every single face sprite. This is error-prone, labor-intensive, and decouples the scaling logic from the UI layout where it belongs.

## 3. Proposed Solution

We will extend the `mugshot` command syntax in `SBARINFO` to support optional **Target Dimensions**. When provided, the engine will dynamically calculate the scale factors required to fit the high-resolution source texture into the specified UI box.

### 3.1 Syntax Extension (SBARINFO)

The command signature will be expanded as follows:

**Current Syntax:**
```c
// Standard usage (inherits texture size)
mugshot <FaceName>, <Accuracy>, <Flags>, <X>, <Y>;
```

**New Syntax:**
```c
// Constrained usage (forces strict target box)
mugshot <FaceName>, <Accuracy>, <Flags>, <X>, <Y>, [Width], [Height];
```

| Parameter | Type | Required | Description |
| :--- | :--- | :--- | :--- |
| `Width` | `Integer` | No | Examples: `24`. The maximum width of the drawn face. |
| `Height` | `Integer` | No | Examples: `29`. The maximum height of the drawn face. |

### 3.2 Expected Behavior
When `Width` and `Height` are specified:
1.  The engine retrieves the current face texture (e.g., `STFST00` at 96x116).
2.  It calculates the aspect-correct (or fill) scaling factor to reduce 96x116 down to the target 24x29.
3.  The renderer draws the texture using high-quality downsampling interpolation.

## 4. Implementation Guide

The modification requires updating the C++ parser and drawing logic in `src/g_statusbar/`.

### Phase 1: Parser Update
**File**: `src/g_statusbar/sbarinfo_commands.cpp`
**Class**: `CommandDrawMugShot`

We must update the parsing logic to detect integers appearing after the standard arguments.

```cpp
// In CommandDrawMugShot::Parse definition

void Parse(FScanner &sc, bool fullScreenOffsets)
{
    // ... [Existing parsing logic for Face, Accuracy, Flags] ...

    GetCoordinates(sc, fullScreenOffsets, x, y);

    // -- NEW LOGIC START --
    if (sc.CheckToken(','))
    {
        sc.MustGetToken(TK_IntConst);
        maxWidth = sc.Number;
        sc.MustGetToken(',');
        sc.MustGetToken(TK_IntConst);
        maxHeight = sc.Number;
    }
    // -- NEW LOGIC END --

    sc.MustGetToken(';');
}
```

### Phase 2: Render Logic Update
**File**: `src/g_statusbar/sbarinfo_commands.cpp`
**Class**: `CommandDrawMugShot`

The `Draw` method acts as the bridge. We calculate the forced size here. Fortunately, `DSBarInfo::DrawGraphic` already supports a `forceWidth`/`forceHeight` override, so we simply need to pass our new values.

```cpp
// In CommandDrawMugShot::Draw definition

void Draw(const SBarInfoMainBlock* block, const DSBarInfo* statusBar)
{
    FGameTexture* face = statusBar->wrapper->mugshot.GetFace(
        statusBar->CPlayer, 
        defaultFace.GetChars(), 
        accuracy, 
        stateFlags
    );

    if (face != NULL)
    {
        // Pass maxWidth and maxHeight (defaulting to -1 if unset)
        // logic will automatically be handled by DrawGraphic which interprets
        // positive values as a forced display size.
        
        statusBar->DrawGraphic(
            face, 
            x, y, 
            block->XOffset(), block->YOffset(), 
            block->Alpha(), 
            block->FullScreenOffsets(),
            false,  // translate
            false,  // dim
            0,      // offsetflags
            false,  // alphaMap
            maxWidth,   // <--- NEW: Passed to forceWidth
            maxHeight   // <--- NEW: Passed to forceHeight
        );
    }
}
```

## 5. Usage Example

Once compiled, you can use the new syntax directly in your `SBARINFO` lump:

```c
statusbar Normal
{
    // Render the status face. 
    // Even if "STF" graphics are 4K resolution, force them into 24x29.
    mugshot "STF", 5, "health", 143, 169, 24, 29;
}
```
