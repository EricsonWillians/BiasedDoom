# glTF Compilation Fixes - Quick Reference

This document provides the exact code changes needed to fix all compilation errors and get glTF support working.

## Summary

- **Total Errors**: 18
- **Files to Modify**: 3
- **Estimated Time**: 30-60 minutes
- **Difficulty**: Medium (API migration + minor bugs)

---

## Fix Order (Apply in This Sequence)

### 1. Fix model_gltf_render.cpp (7 errors)

**File**: `/home/ericsonwillians/workspace/BiasedDoom/src/common/models/model_gltf_render.cpp`

#### Change 1: Add TICRATE Definition (Lines 1-10)

Add after the existing includes:

```cpp
// After #include statements, before first function
#ifndef TICRATE
#define TICRATE 35
#endif
```

#### Change 2: Fix Line 224

**Error**: `'TICRATE' was not declared in this scope`

```cpp
// BEFORE:
lastAnimationTime = (lastAnimationTime + deltaTime) * TICRATE;

// AFTER:
lastAnimationTime = (lastAnimationTime + deltaTime) * TICRATE;
// Now works because we defined TICRATE above
```

#### Change 3: Fix Line 292

**Error**: `TArray has no member named 'Empty'`

```cpp
// BEFORE:
if (!mesh.indices.Empty())

// AFTER:
if (mesh.indices.Size() > 0)
```

#### Change 4: Fix Line 320

**Error**: `TArray has no member named 'Empty'`

```cpp
// BEFORE:
if (scene.skins.Empty() || currentAnimationIndex < 0)

// AFTER:
if (scene.skins.Size() == 0 || currentAnimationIndex < 0)
```

#### Change 5: Fix Line 325

**Error**: `class TRS has no member named 'ToMatrix'`

This requires understanding GZDoom's matrix system. Temporary workaround:

```cpp
// BEFORE:
boneMatrices[i] = currentPose[i].ToMatrix();

// TEMPORARY WORKAROUND (models won't animate but will load):
boneMatrices[i].loadIdentity();

// PROPER FIX (TODO - need to check bonecomponents.h):
// VSMatrix mat;
// mat.loadIdentity();
// mat.translate(currentPose[i].translation.X, currentPose[i].translation.Y, currentPose[i].translation.Z);
// // Apply rotation quaternion to matrix
// // Apply scale
// boneMatrices[i] = mat;
```

#### Change 6: Fix Line 423

**Error**: `'TICRATE' was not declared in this scope`

```cpp
// BEFORE:
lastAnimationTime = 0.0 / TICRATE;

// AFTER:
lastAnimationTime = 0.0 / TICRATE;
// Now works because we defined TICRATE above
```

#### Change 7: Fix Line 431

**Error**: `TArray has no member named 'Empty'`

```cpp
// BEFORE:
if (scene.skins.Empty()) return;

// AFTER:
if (scene.skins.Size() == 0) return;
```

---

### 2. Fix model_gltf.cpp (11 errors)

**File**: `/home/ericsonwillians/workspace/BiasedDoom/src/common/models/model_gltf.cpp`

#### Change 1: Fix Line 290 - Missing mLumpNum

**Error**: `'mLumpNum' was not declared in this scope`

**Option A - If FModel base class has this field**:
```cpp
// Just use it - no changes needed
mLumpNum = lumpnum;
```

**Option B - If field doesn't exist in base, add to FGLTFModel**:

In `model_gltf.h`, add to FGLTFModel class private section:

```cpp
class FGLTFModel : public FModel
{
private:
    // ... existing members ...
    int mLumpNum = -1;  // ADD THIS LINE
```

Then in `model_gltf.cpp:290`:
```cpp
mLumpNum = lumpnum;  // Now works
```

#### Change 2: Fix Lines 405, 424 - fastgltf Options API

**Error**: `'GenerateMeshIndices' is not a member of 'fastgltf::Options'`

```cpp
// BEFORE (Line 405):
options |= fastgltf::Options::GenerateMeshIndices;

// AFTER:
options |= fastgltf::Options::GenerateIndices;

// BEFORE (Line 424):
options |= fastgltf::Options::GenerateMeshIndices;

// AFTER:
options |= fastgltf::Options::GenerateIndices;
```

#### Change 3: Fix Lines 411, 430 - fastgltf Error Messages

**Error**: `'getErrorMessage' is not a member of 'fastgltf'`

```cpp
// BEFORE (Line 411):
Printf("fastgltf error: %s\\n", fastgltf::getErrorMessage(result.error()));

// AFTER:
Printf("fastgltf error: %d\\n", static_cast<int>(result.error()));
// OR if getErrorCodeMessage exists in your fastgltf version:
// Printf("fastgltf error: %s\\n", fastgltf::getErrorCodeMessage(result.error()));

// BEFORE (Line 430):
Printf("fastgltf GLB error: %s\\n", fastgltf::getErrorMessage(result.error()));

// AFTER:
Printf("fastgltf GLB error: %d\\n", static_cast<int>(result.error()));
```

#### Change 4: Fix Lines 416, 435 - Missing GLTFLoadResult Parameter

**Error**: No matching function for call to 'ValidateAsset()'

```cpp
// BEFORE (Line 416):
if (!ValidateAsset()) { return false; }

// AFTER:
if (!ValidateAsset(lastError)) { return false; }

// BEFORE (Line 435):
if (!ValidateAsset()) { return false; }

// AFTER:
if (!ValidateAsset(lastError)) { return false; }
```

#### Change 5: Fix Lines 426-427 - std::span Not Available in C++17

**Error**: `'span' is not a member of 'std'`

This is the most complex fix. The code tries to use C++20's std::span but BiasedDoom uses C++17.

```cpp
// BEFORE (Lines 426-427):
std::span<const uint8_t> data(reinterpret_cast<const uint8_t*>(buffer), length);
if (!parser.loadGLTF(&gltf, data, directory).has_value())

// AFTER:
// Create a fastgltf GltfDataBuffer instead
fastgltf::GltfDataBuffer dataBuffer;
dataBuffer.loadFromMemory(reinterpret_cast<const uint8_t*>(buffer), static_cast<size_t>(length));
auto loadResult = parser.loadGLTF(&gltf, dataBuffer, directory);
if (!loadResult)
```

#### Change 6: Fix Line 491 - Missing Parameter

**Error**: No matching function for call to 'LoadTextureFromGLTF(size_t&)'

```cpp
// BEFORE:
FGameTexture* tex = LoadTextureFromGLTF(texIndex);

// AFTER:
FGameTexture* tex = LoadTextureFromGLTF(texIndex, lastError);
```

#### Change 7: Fix Line 517 - Missing Parameter

**Error**: No matching function for call to 'LoadMeshPrimitive(...)'

```cpp
// BEFORE:
if (!LoadMeshPrimitive(prim, mesh))

// AFTER:
if (!LoadMeshPrimitive(prim, mesh, lastError))
```

---

## Verification After Fixes

### 1. Compile Test

```bash
cd /home/ericsonwillians/workspace/BiasedDoom
cmake --build build --config Release -j16 2>&1 | tee build.log
```

**Expected**: No errors, only warnings acceptable

### 2. Symbol Check

```bash
nm build/neodoom | grep -i "gltf"
```

**Expected**: Should see FGLTFModel symbols

### 3. Test Launch

```bash
cd build
./neodoom -file ~/doom_blender/NeoPlayer_cube.pk3 +developer 2 +map e1m1 2>&1 | tee test.log
```

**Expected**:
- No "Mixin GLTFModel does not exist" error
- Should see "InitGLTFModel: Loading..." message
- Game starts without crashing

---

## Advanced Fix: TRS::ToMatrix() Proper Implementation

The temporary identity matrix fix will prevent crashes but models won't animate. For proper skeletal animation:

### Option 1: Find Existing Method

Check if TRS has a different method name:

```bash
grep -r "struct TRS" /home/ericsonwillians/workspace/BiasedDoom/src/common/models/
grep -r "ToMatrix\|GetMatrix\|BuildMatrix" /home/ericsonwillians/workspace/BiasedDoom/src/common/models/bonecomponents.h
```

### Option 2: Implement Manual Conversion

If no method exists, implement in model_gltf_render.cpp:

```cpp
// Add helper function at top of file
static VSMatrix TRSToMatrix(const TRS& transform)
{
    VSMatrix mat;
    mat.loadIdentity();

    // Apply translation
    mat.translate(transform.translation.X, transform.translation.Y, transform.translation.Z);

    // Apply rotation (quaternion to matrix)
    // This is the complex part - need quaternion math
    double qx = transform.rotation.X;
    double qy = transform.rotation.Y;
    double qz = transform.rotation.Z;
    double qw = transform.rotation.W;

    VSMatrix rotMat;
    rotMat.loadIdentity();

    // Quaternion to rotation matrix conversion
    rotMat.matrix[0] = 1.0 - 2.0*(qy*qy + qz*qz);
    rotMat.matrix[1] = 2.0*(qx*qy + qw*qz);
    rotMat.matrix[2] = 2.0*(qx*qz - qw*qy);

    rotMat.matrix[4] = 2.0*(qx*qy - qw*qz);
    rotMat.matrix[5] = 1.0 - 2.0*(qx*qx + qz*qz);
    rotMat.matrix[6] = 2.0*(qy*qz + qw*qx);

    rotMat.matrix[8] = 2.0*(qx*qz + qw*qy);
    rotMat.matrix[9] = 2.0*(qy*qz - qw*qx);
    rotMat.matrix[10] = 1.0 - 2.0*(qx*qx + qy*qy);

    mat = mat * rotMat;

    // Apply scale
    VSMatrix scaleMat;
    scaleMat.loadIdentity();
    scaleMat.scale(transform.scale.X, transform.scale.Y, transform.scale.Z);
    mat = mat * scaleMat;

    return mat;
}

// Then use it:
boneMatrices[i] = TRSToMatrix(currentPose[i]);
```

---

## Troubleshooting

### Build Still Fails

1. **Check fastgltf version**:
   ```bash
   ./vcpkg/vcpkg list | grep fastgltf
   ```
   Expected: 0.7.x or 0.8.x

2. **Clean build**:
   ```bash
   rm -rf build/
   cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=vcpkg/scripts/buildsystems/vcpkg.cmake -DNEODOOM_ENABLE_GLTF=ON -DNEODOOM_BUILD_GLTF=ON
   cmake --build build --config Release -j16
   ```

3. **Check compilation output**:
   ```bash
   cmake --build build --config Release 2>&1 | grep -A 5 "error:"
   ```

### Runtime Crashes

1. **Enable debug output**:
   ```bash
   ./neodoom -file test.pk3 +developer 2 +map e1m1 2>&1 | tee debug.log
   ```

2. **Check model loading**:
   Look for messages like:
   - "LoadModel: 'models/player.gltf' not found"
   - "fastgltf error: ..."
   - "glTF validation failed"

3. **Verify PK3 structure**:
   ```bash
   unzip -l NeoPlayer_cube.pk3
   ```
   Should contain:
   - ZSCRIPT.zs
   - zscript/NeoPlayer.zs
   - models/player/player.gltf
   - models/player/player.bin

---

## Quick Copy-Paste Fixes

For rapid iteration, here are search-replace patterns:

```bash
# In model_gltf_render.cpp:
sed -i 's/\.Empty()/\.Size() == 0/g' src/common/models/model_gltf_render.cpp
sed -i 's/lastAnimationTime = 0\.0 \/ TICRATE;/lastAnimationTime = 0.0;/g' src/common/models/model_gltf_render.cpp

# In model_gltf.cpp:
sed -i 's/fastgltf::Options::GenerateMeshIndices/fastgltf::Options::GenerateIndices/g' src/common/models/model_gltf.cpp
sed -i 's/fastgltf::getErrorMessage/fastgltf::getErrorCodeMessage/g' src/common/models/model_gltf.cpp
```

**WARNING**: Automated replacement may miss context - manual review recommended

---

**Last Updated**: October 26, 2025
**Status**: Ready to apply
**Next**: Apply fixes in order, then rebuild
