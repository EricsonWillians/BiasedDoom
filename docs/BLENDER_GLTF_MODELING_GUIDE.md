# Blender → glTF 2.0 Authoring Guide for BiasedDoom

**Version 2.0 - Enhanced Skeletal Animation Support**

This guide describes, in depth, how to create 3D models and animations in Blender for use with BiasedDoom's glTF 2.0 pipeline. It covers scene setup, modeling, materials, textures, armatures/skinning, animations, export settings, project layout, and troubleshooting.

**NEW in v2.0**: BiasedDoom now includes comprehensive bone manipulation API that works seamlessly with Blender bone names and transform system. You can dynamically control bones via ZScript using Blender-friendly methods like `SetBoneRotation()`, `AddBoneLookAt()`, and more.

## Summary of Engine Capabilities and Constraints

- Geometry
  - Positions, vertex normals, tangents, UV0 are consumed.
  - Optional UV1, colors and tangents are allowed but not yet fully used.
  - Hard budget recommendation: ≤ 50k vertices per model; soft maximum ≤ 1M vertices per file; ≤ 2M triangles per file.
  - No negative object/armature scales. Apply transforms before export.

- Materials (PBR metallic‑roughness)
  - Principled BSDF is translated to glTF PBR.
  - Textures supported: BaseColor (sRGB), MetallicRoughness (non‑color), Normal (non‑color), Occlusion (non‑color), Emissive (sRGB).
  - Double‑Sided and Alpha Mask/Blend are supported; prefer Mask for opaque cutouts.

- Textures
  - Use external image files referenced by URI. Embedded images and data URIs are not supported at this time.
  - Prefer glTF Separate export (.gltf + .bin + textures); keep relative paths.
  - PNG (lossless) or JPEG (albedo only) recommended. Max 4096×4096 (power‑of‑two suggested).
  - Color space: sRGB for base color/emissive; Non‑Color for normal/MR/AO.

- Skinning / Armatures
  - Up to 4 bone weights per vertex (normalize weights).
  - Bone indices must be < 255 per vertex (practical limit: ≤ 255 bones per skin).
  - Use a single Armature modifier per skinned mesh.

- Animations
  - Transform channels (Translation, Rotation, Scale) are supported; Name your actions sensibly.
  - Keep to 30 FPS or 60 FPS baked keyframes for consistency.
  - Bake constraints; avoid drivers that won’t bake to TRS.

- Export format
  - Use “glTF Separate” (.gltf + .bin + textures folder). GLB and embedded images are currently not supported by the texture loader.
  - Do not enable Draco/KTX2 compression in the exporter.

- Project layout
  - Place exported assets in `models/<pack>/<model>/` so texture URIs remain relative, e.g.: `models/props/torch/torch.gltf` and `models/props/torch/textures/...`.

## Blender Project Setup

1) Units and Scale
- Set Scene Units to Metric; Unit Scale = 1.0.
- In BiasedDoom, 1 unit ≈ 1 Doom map unit. Common props: 1 m = 1 unit scale keeps proportions reasonable.
- Model to real‑world scale to avoid re‑scaling in the engine.

2) Axes and Orientation
- Blender uses Z‑Up; glTF uses Y‑Up. The Blender glTF exporter handles conversion automatically.
- Keep object rotations applied (see below) to avoid exporter adding corrective transforms.

3) Apply Transforms
- Before export: select each mesh and armature, then
  - Object → Apply → All Transforms (Location/Rotation/Scale).
  - Ensure scale becomes 1,1,1 and rotation 0,0,0.

4) Naming Conventions
- ASCII, no spaces, no special characters; use `snake_case` or `PascalCase`.
- Keep material names unique per file.
- Keep node/bone names stable; avoid renaming late in production.

## Modeling Best Practices

- Topology
  - Triangulate your mesh (the exporter can triangulate; manual triangulation gives more control).
  - Remove doubles; keep normals consistent.
  - Limit degenerate/zero‑area faces; avoid non‑manifold geometry for dynamic lighting compatibility.

- Normals and Tangents
  - Enable Auto Smooth (Object Data Properties → Normals → Auto Smooth ~ 60°).
  - In the exporter, enable “Tangents” to output MikkTSpace tangents.

- UV Mapping
  - Provide UV0 for all materials. Keep islands consistent; minimize stretching.
  - UV1 is optional; if used, ensure the exporter includes it.

## Materials and Textures (PBR)

- Use a Principled BSDF per material.
- Texture slots and color spaces:
  - Base Color: sRGB PNG/JPG.
  - Metallic‑Roughness: non‑color; often combined in a single ORM or MR texture. If using separate textures, wire Roughness to Roughness and Metallic to Metallic.
  - Normal: non‑color; through a Normal Map node.
  - Ambient Occlusion: non‑color; optional.
  - Emissive: sRGB; keep intensity reasonable.

- Texture resolution
  - Prefer 512, 1024, 2048; do not exceed 4096 unless strictly necessary.
  - Power‑of‑two textures help on older GPUs.

- File format and placement
  - Put textures in a `textures/` subfolder next to the `.gltf`. Keep relative paths.
  - Example: `models/props/torch/torch.gltf` and `models/props/torch/textures/torch_albedo.png`.

## Armatures (Skinning)

- One Armature per skinned mesh.
- Parenting: Mesh → Armature Deform (With Empty Groups or With Automatic Weights).
- Weight limits
  - Restrict to 4 weights per vertex. Use Weight Paint → Weights → Limit Total (limit = 4) + Normalize All.
- Avoid negative scales on bones or armature objects.
- Freeze / apply transforms on the Armature object before export.

## Animations

- Organize animations as Actions in the Dope Sheet.
- Name actions descriptively (e.g., `Idle`, `Walk`, `Attack`). The exporter will preserve action names.
- Use keyframes on object/bone transforms (Location/Rotation/Scale). Avoid modifiers or drivers that won’t bake.
- Bake constraints if used: Select Armature → Object → Animation → Bake Action… (Only Selected, Visual Keying, Clear Constraints if necessary).
- Framerate: prefer 30 FPS or 60 FPS; keep consistent across actions.

Exporter tip: In glTF exporter panel, enable “Animations → NLA Strips” if you manage actions in NLA, or “All Actions” to include all actions in the file.

## Blender glTF 2.0 Export Settings

File → Export → glTF 2.0 (.glb/.gltf). Recommended profile:

- Format: glTF Separate (.gltf + .bin + textures) — REQUIRED for BiasedDoom (external images only).
- Include
  - Selected Objects: ON (optional; export only what you selected)
  - Apply Modifiers: ON
  - UVs: ON
  - Normals: ON
  - Tangents: ON
  - Vertex Colors: ON (optional)
  - Materials: ON
  - Images: Automatic (results in external images when using Separate format)
  - Loose Edges/Points: OFF

- Transform
  - +Y Up: Exporter handles axis conversion; leave defaults
  - Apply Unit: ON

- Geometry
  - Compression: OFF (no Draco)
  - Mesh Quantization: OFF (leave default)
  - Export Cameras/Lights: OFF (unless you need them)

- Animation
  - Animations: ON
  - Sampling Rate: 30 (or 60)
  - Limit to Playback Range: ON (optional)
  - NLA Strips: ON (if applicable)
  - All Actions: ON (if not using NLA)
  - Shape Keys: OFF (not currently used)

- Materials
  - Set Image Color Space correctly in Blender: sRGB for albedo/emissive; Non‑Color for MR/AO/Normal.
  - Double Sided: Enable per material as needed.

Do NOT enable:
- Draco mesh compression — unsupported.
- KTX2 texture compression — unsupported for now.
- “Embed Textures” (GLB) — BiasedDoom currently expects external textures via URI.

## Project Layout and Loading in BiasedDoom

Recommended layout under the game root:

```
models/
  props/
    torch/
      torch.gltf
      torch.bin
      textures/
        torch_albedo.png
        torch_mr.png
        torch_normal.png
        torch_emissive.png
```

- Keep the `.gltf`, `.bin`, and texture files together so relative URIs resolve.
- Use short, unique, lowercase paths.

## Quality Checklist (Pre‑Export)

- Geometry
  - [ ] All transforms applied (L/R/S).
  - [ ] No negative scales.
  - [ ] Triangulated or clean quads suitable for triangulation.
  - [ ] Auto Smooth enabled; custom splits where needed.
  - [ ] UV0 complete; no overlapping islands unless intended.

- Materials/Textures
  - [ ] Principled BSDF used; only one per material slot.
  - [ ] BaseColor (sRGB), MetallicRoughness/ORM (Non‑Color), Normal (Non‑Color), AO (Non‑Color), Emissive (sRGB) assigned correctly.
  - [ ] Texture sizes within 64–4096; power‑of‑two where possible.
  - [ ] Texture files placed in `textures/` folder next to model.

- Skinning
  - [ ] ≤ 4 bone weights per vertex; normalized.
  - [ ] Single Armature modifier; no constraints that aren’t baked.

- Animations
  - [ ] Actions named; 30/60 FPS; baked to TRS.
  - [ ] NLA/All Actions export option set appropriately.

## Troubleshooting

Symptoms → Causes → Fixes

- Model loads but no textures
  - Embedded/GLB or data URIs are not supported. Re‑export as glTF Separate with external images. Check relative paths and file placement under `models/...`.

- Shading looks wrong
  - Missing tangents or wrong normal map color space. Enable “Tangents” in export; set Normal maps to Non‑Color.

- Model scale/orientation incorrect
  - Unapplied transforms. Apply All Transforms (Ctrl‑A) before export; keep exporter defaults for axis.

- Animation doesn’t play or bones distort
  - More than 4 weights per vertex or non‑normalized weights. Limit Total (4) and Normalize All. Bake constraints to TRS.

- Exporter produces textures in unsupported formats
  - Convert exotic formats to PNG/JPG. Avoid KTX2, WebP for now.

## Performance Tips

- Keep triangle counts and texture sizes reasonable for the target GPU.
- Share materials/textures between meshes where possible.
- Prefer a single skinned mesh per character.
- Reduce keyframe density if motion is slow; keep 30 FPS if possible.

## Version Notes

- BiasedDoom uses fastgltf 0.5.x internally. Some glTF extensions are not yet supported.
- Draco, KTX2, embedded images, and data URIs are currently not supported by the loader; use external textures via URI.

## Example Export Recipe (Quick Reference)

1) Apply transforms to meshes and armature.
2) Auto Smooth normals; ensure UV0 exists.
3) Principled BSDF with BaseColor/Normal/MR/AO/Emissive textures (correct color spaces).
4) Weight limit to 4; normalize weights.
5) Ensure Actions are named and baked at 30 FPS.
6) Export → glTF 2.0 → Format: glTF Separate; Normals: ON; Tangents: ON; Animations: ON; NLA/All Actions as needed; Compression: OFF.
7) Place `.gltf`, `.bin`, and textures under `models/<pack>/<model>/` and keep relative paths intact.

