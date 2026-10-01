/*
** procgen.zs
**
** ZScript API for procedural map generation
**
**---------------------------------------------------------------------------
**
** Copyright 2025 BiasedDoom Contributors
** All rights reserved.
**
*/

// ============================================================================
// ProceduralMapGenerator
//
// Configure and trigger procedural map generation from ZScript.
// The generated map is loaded via the "PROCMAP" map name.
// ============================================================================

class ProceduralMapGenerator
{
    // ------------------------------------------------------------------------
    // Native Interface (implemented in C++ in procgen_zscript.cpp)
    // ------------------------------------------------------------------------

    native static void SetSeed(int seed);
    native static void SetTheme(String theme);
    native static void SetDifficulty(int difficulty);
    native static void SetSize(int size);
    native static void SetLayout(int layout);
    native static void SetVerticality(int verticality);
    native static void SetDetail(int detail);
    native static void SetOutdoors(int outdoors);
    native static int Generate();        // returns 1 on success, 0 on failure
    native static String GetLastError();
    native static String GetRunProfile();
    native static String GetRunBriefing();
    native static int GenerateAndLoad(int seed, String theme, int difficulty, int size);
}
