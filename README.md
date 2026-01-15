# 🕹️ BiasedDoom

> [!IMPORTANT]
> **Disclaimer regarding AI-Generated Code**
> This project unashamedly leverages AI assistance for development. We prioritize results and functionality over the origin of the code. If you have a philosophical objection to AI-generated code, this project is not for you, and we kindly suggest you look elsewhere.

[![Build Status](https://github.com/YOURNAME/BiasedDoom/actions/workflows/ci.yml/badge.svg)](https://github.com/YOURNAME/BiasedDoom/actions/workflows/ci.yml)

## Next-Generation Modding for the Classic DOOM Engine

BiasedDoom is a modern fork of **GZDoom** that expands the engine with **native glTF 2.0 support**, enabling skeletal animations, PBR materials, and seamless workflows with **Blender** and other 3D tools.  
Our mission: preserve the soul of DOOM while empowering modders with next-gen asset pipelines.

---

## ✨ Features

- **glTF 2.0 Import**  
  Load `.gltf` and `.glb` files directly, no conversions required.  

- **Skeletal Animation**  
  Full support for armatures, multiple animations, bone weights, and blending.  

- **PBR Materials**  
  Metallic-roughness workflow for realistic rendering under OpenGL/Vulkan.  

- **Blender Workflow**  
  Export directly from Blender with the official glTF 2.0 exporter.  

- **Backward Compatibility**  
  Keep using MD2/MD3, voxels, and classic DECORATE/ZScript definitions.  

- **GPU Acceleration**  
  Hardware-skinned animation for smoother performance.  

---

## 🔧 Blender → BiasedDoom Workflow

1. **Create Your Model in Blender**  
   - Rig your mesh with armatures.  
   - Apply transforms (`Ctrl+A → Apply All Transforms`).  

2. **Export to glTF 2.0**  
   - `File → Export → glTF 2.0 (.glb)`  
   - Recommended settings:  
     - Format: Binary `.glb`  
     - ✓ Apply Modifiers  
     - ✓ Export Materials  
     - ✓ Export Animations  

3. **Use in BiasedDoom**  
   Define the model in your actor with ZScript/DECORATE:  

   ```cpp
   model MyCyberDemon
   {
       path = "models/cyberdemon.glb"
       animation = "Idle"
       scale = 1.0
   }
