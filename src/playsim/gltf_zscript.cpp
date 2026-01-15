/*
** gltf_zscript.cpp
**
** Native C++ implementation of glTF ZScript interface
** Simplified stub implementation for BiasedDoom
**
**---------------------------------------------------------------------------
**
** Copyright 2025 BiasedDoom Contributors
** All rights reserved.
**
*/

#include "actor.h"
#include "g_levellocals.h"
#include "r_defs.h"
#include "vm.h"

// Helper native implementations (called from VM wrappers)
static void NativePlayAnimation(AActor *self, const char *name, bool loop,
                                double blendTime) {
  Printf("NativePlayAnimation: %s (loop=%d)\n", name ? name : "(null)", loop);
}

static void NativeStopAnimation(AActor *self) {
  Printf("NativeStopAnimation called\n");
}

static void NativePauseAnimation(AActor *self) {
  Printf("NativePauseAnimation called\n");
}

static void NativeResumeAnimation(AActor *self) {
  Printf("NativeResumeAnimation called\n");
}

static void NativeSetAnimationSpeed(AActor *self, double speed) {
  Printf("NativeSetAnimationSpeed: %f\n", speed);
}

static void NativeSetPBREnabled(AActor *self, bool enable) {
  Printf("NativeSetPBREnabled: %d\n", enable);
}

static void NativeSetMetallicFactor(AActor *self, double metallic) {
  Printf("NativeSetMetallicFactor: %f\n", metallic);
}

static void NativeSetRoughnessFactor(AActor *self, double roughness) {
  Printf("NativeSetRoughnessFactor: %f\n", roughness);
}

static void NativeSetEmissive(AActor *self, unsigned color, double strength) {
  Printf("NativeSetEmissive: color=%08x strength=%f\n", color, strength);
}

static void NativeUpdateModel(AActor *self, double deltaTime) {
  // Stub: would update animation state
}

//===========================================================================
//
// GLTFModel mixin - Native function implementations (STUBS)
//
// NOTE: These are simplified stub implementations to allow the game to
// compile and run. Full implementation requires deeper integration with
// the model rendering system.
//
//===========================================================================

//===========================================================================
//
// NativePlayAnimation
//
//===========================================================================

DEFINE_ACTION_FUNCTION_NATIVE(AActor, NativePlayAnimation,
                              NativePlayAnimation) {
  PARAM_SELF_PROLOGUE(AActor);
  PARAM_STRING(name);
  PARAM_BOOL(loop);
  PARAM_FLOAT(blendTime);

  // Stub: Log the call for debugging
  Printf("NativePlayAnimation: %s (loop=%d)\n", name.GetChars(), loop);

  return 0;
}

//===========================================================================
//
// NativeStopAnimation
//
//===========================================================================

DEFINE_ACTION_FUNCTION_NATIVE(AActor, NativeStopAnimation,
                              NativeStopAnimation) {
  PARAM_SELF_PROLOGUE(AActor);

  Printf("NativeStopAnimation called\n");

  return 0;
}

//===========================================================================
//
// NativePauseAnimation
//
//===========================================================================

DEFINE_ACTION_FUNCTION_NATIVE(AActor, NativePauseAnimation,
                              NativePauseAnimation) {
  PARAM_SELF_PROLOGUE(AActor);

  Printf("NativePauseAnimation called\n");

  return 0;
}

//===========================================================================
//
// NativeResumeAnimation
//
//===========================================================================

DEFINE_ACTION_FUNCTION_NATIVE(AActor, NativeResumeAnimation,
                              NativeResumeAnimation) {
  PARAM_SELF_PROLOGUE(AActor);

  Printf("NativeResumeAnimation called\n");

  return 0;
}

//===========================================================================
//
// NativeSetAnimationSpeed
//
//===========================================================================

DEFINE_ACTION_FUNCTION_NATIVE(AActor, NativeSetAnimationSpeed,
                              NativeSetAnimationSpeed) {
  PARAM_SELF_PROLOGUE(AActor);
  PARAM_FLOAT(speed);

  Printf("NativeSetAnimationSpeed: %f\n", speed);

  return 0;
}

//===========================================================================
//
// NativeSetPBREnabled
//
//===========================================================================

DEFINE_ACTION_FUNCTION_NATIVE(AActor, NativeSetPBREnabled,
                              NativeSetPBREnabled) {
  PARAM_SELF_PROLOGUE(AActor);
  PARAM_BOOL(enable);

  Printf("NativeSetPBREnabled: %d\n", enable);

  return 0;
}

//===========================================================================
//
// NativeSetMetallicFactor
//
//===========================================================================

DEFINE_ACTION_FUNCTION_NATIVE(AActor, NativeSetMetallicFactor,
                              NativeSetMetallicFactor) {
  PARAM_SELF_PROLOGUE(AActor);
  PARAM_FLOAT(metallic);

  Printf("NativeSetMetallicFactor: %f\n", metallic);

  return 0;
}

//===========================================================================
//
// NativeSetRoughnessFactor
//
//===========================================================================

DEFINE_ACTION_FUNCTION_NATIVE(AActor, NativeSetRoughnessFactor,
                              NativeSetRoughnessFactor) {
  PARAM_SELF_PROLOGUE(AActor);
  PARAM_FLOAT(roughness);

  Printf("NativeSetRoughnessFactor: %f\n", roughness);

  return 0;
}

//===========================================================================
//
// NativeSetEmissive
//
//===========================================================================

DEFINE_ACTION_FUNCTION_NATIVE(AActor, NativeSetEmissive, NativeSetEmissive) {
  PARAM_SELF_PROLOGUE(AActor);
  PARAM_COLOR(color);
  PARAM_FLOAT(strength);

  Printf("NativeSetEmissive: color=%08x strength=%f\n", color, strength);

  return 0;
}

//===========================================================================
//
// NativeUpdateModel
//
//===========================================================================

DEFINE_ACTION_FUNCTION_NATIVE(AActor, NativeUpdateModel, NativeUpdateModel) {
  PARAM_SELF_PROLOGUE(AActor);
  PARAM_FLOAT(deltaTime);

  // Stub: This would update animation state
  // Printf("NativeUpdateModel: dt=%f\n", deltaTime);

  return 0;
}
