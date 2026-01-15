/*
** gltf_model.zs
**
** Minimal glTF 2.0 model support for ZScript 4.5
** Provides access to native glTF implementation
**
**---------------------------------------------------------------------------
**
** Copyright 2025 BiasedDoom Contributors
** All rights reserved.
**
*/

// ============================================================================
// GLTFModel - Actor mixin for glTF model support
// ============================================================================

mixin class GLTFModel
{
    // Runtime state
    private bool modelInitialized;
    private String currentModelPath;
    private String currentAnimationName;
    private double lastUpdateTime;

    // ========================================================================
    // Model Initialization
    // ========================================================================

    /// Initialize glTF model with given path
    bool InitGLTFModel(String modelPath)
    {
        if (modelInitialized)
        {
            Console.Printf("Warning: Model already initialized for %s", GetClassName());
            return true;
        }

        if (modelPath.Length() == 0)
        {
            Console.Printf("Error: Empty model path for %s", GetClassName());
            return false;
        }

        currentModelPath = modelPath;
        modelInitialized = true;
        lastUpdateTime = level.time;

        Console.Printf("InitGLTFModel: Loading %s", modelPath);
        return true;
    }

    /// Check if model is initialized
    bool HasGLTFModel()
    {
        return modelInitialized;
    }

    // ========================================================================
    // Transform Control
    // ========================================================================

    /// Set uniform model scale
    void SetModelScaleUniform(double s)
    {
        if (!HasGLTFModel()) return;
        // Scale will be applied via actor scale property
        A_SetScale(s, s);
    }

    /// Set model offset from actor origin
    void SetModelOffset(double x, double y, double z)
    {
        if (!HasGLTFModel()) return;
        // Offset handled by renderer
    }

    // ========================================================================
    // Animation Control
    // ========================================================================

    /// Play animation by name
    bool PlayAnimation(String animName, bool loop = true, double blendTime = 0.2)
    {
        if (!HasGLTFModel())
        {
            Console.Printf("Warning: PlayAnimation called but model not initialized");
            return false;
        }

        if (animName.Length() == 0)
        {
            Console.Printf("Error: Empty animation name");
            return false;
        }

        currentAnimationName = animName;

        Console.Printf("PlayAnimation: %s (loop=%s)", animName, loop ? "true" : "false");
        NativePlayAnimation(animName, loop, blendTime);
        return true;
    }

    /// Stop current animation
    void StopAnimation()
    {
        if (!HasGLTFModel()) return;
        NativeStopAnimation();
    }

    /// Pause animation
    void PauseAnimation()
    {
        if (!HasGLTFModel()) return;
        NativePauseAnimation();
    }

    /// Resume paused animation
    void ResumeAnimation()
    {
        if (!HasGLTFModel()) return;
        NativeResumeAnimation();
    }

    /// Set animation playback speed
    void SetAnimationSpeed(double speed)
    {
        if (!HasGLTFModel()) return;
        NativeSetAnimationSpeed(speed);
    }

    /// Get current animation name
    String GetCurrentAnimation()
    {
        return currentAnimationName;
    }

    // ========================================================================
    // Update and Rendering
    // ========================================================================

    /// Update model state (call every tic)
    void UpdateGLTFModel()
    {
        if (!HasGLTFModel()) return;

        double deltaTime = (level.time - lastUpdateTime) / 35.0; // Convert tics to seconds
        lastUpdateTime = level.time;

        // Notify native code to update
        NativeUpdateModel(deltaTime);
    }

    // ========================================================================
    // PBR Material Control
    // ========================================================================

    /// Enable/disable PBR rendering
    void SetPBREnabled(bool enable)
    {
        if (!HasGLTFModel()) return;
        NativeSetPBREnabled(enable);
    }

    /// Set metallic factor (0.0 = dielectric, 1.0 = metal)
    void SetMetallicFactor(double metallic)
    {
        if (!HasGLTFModel()) return;
        NativeSetMetallicFactor(metallic);
    }

    /// Set roughness factor (0.0 = smooth, 1.0 = rough)
    void SetRoughnessFactor(double roughness)
    {
        if (!HasGLTFModel()) return;
        NativeSetRoughnessFactor(roughness);
    }

    /// Set emissive color and strength
    void SetEmissive(Color color, double strength = 1.0)
    {
        if (!HasGLTFModel()) return;
        NativeSetEmissive(color, strength);
    }

    // ========================================================================
    // Native Interface (implemented in C++)
    // ========================================================================

    private native void NativePlayAnimation(String name, bool loop, double blendTime);
    private native void NativeStopAnimation();
    private native void NativePauseAnimation();
    private native void NativeResumeAnimation();
    private native void NativeSetAnimationSpeed(double speed);

    private native void NativeSetPBREnabled(bool enable);
    private native void NativeSetMetallicFactor(double metallic);
    private native void NativeSetRoughnessFactor(double roughness);
    private native void NativeSetEmissive(Color color, double strength);

    private native void NativeUpdateModel(double deltaTime);
}
