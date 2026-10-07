#!/usr/bin/env python3
"""Offline regression checks for Vulkan static descriptor reuse.

The hardware descriptor sets bind resource handles, not per-frame contents.
These checks protect the cache/invalidation contract that keeps the expensive
Vulkan allocation and descriptor-write path out of ordinary rendered frames.
"""

from __future__ import annotations

from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]


def function_body(source: str, signature: str, next_signature: str) -> str:
    start = source.index(signature)
    end = source.index(next_signature, start)
    return source[start:end]


def require(fragment: str, source: str, label: str) -> None:
    assert fragment in source, f"missing Vulkan descriptor contract in {label}: {fragment!r}"


def test_descriptor_sets_reuse_their_handles_until_resources_change() -> None:
    source = (
        REPO_ROOT
        / "src"
        / "common"
        / "rendering"
        / "vulkan"
        / "renderer"
        / "vk_descriptorset.cpp"
    ).read_text(encoding="utf-8")

    hardware = function_body(
        source,
        "void VkDescriptorSetManager::UpdateHWBufferSet()",
        "void VkDescriptorSetManager::UpdateFixedSet()",
    )
    fixed = function_body(
        source,
        "void VkDescriptorSetManager::UpdateFixedSet()",
        "void VkDescriptorSetManager::ResetHWTextureSets()",
    )

    for fragment in (
        "if (HWBufferSet &&",
        "HWViewpointBuffer == viewpointBuffer",
        "HWMatrixBuffer == matrixBuffer",
        "HWStreamBuffer == streamBuffer",
        "HWLightBuffer == lightBuffer",
        "HWBoneBuffer == boneBuffer",
        "if (!HWBufferSet)",
        "HWViewpointBuffer = viewpointBuffer;",
        "HWBoneBuffer = boneBuffer;",
    ):
        require(fragment, hardware, "UpdateHWBufferSet")
    assert "DrawDeleteList->Add(std::move(HWBufferSet))" not in hardware, (
        "the hardware descriptor set must not be retired and recreated every frame"
    )

    for fragment in (
        "if (FixedSet &&",
        "FixedShadowmapView == shadowmapView",
        "FixedLightmapView == lightmapView",
        "FixedSectorBleedView == sectorBleedView",
        "FixedAccelerationStructure == accelerationStructure",
        "if (!FixedSet)",
        "FixedShadowmapView = shadowmapView;",
        "FixedAccelerationStructure = accelerationStructure;",
    ):
        require(fragment, fixed, "UpdateFixedSet")
    assert "DrawDeleteList->Add(std::move(FixedSet))" not in fixed, (
        "the fixed descriptor set must not be retired and recreated every frame"
    )

    reset = function_body(
        source,
        "void VkDescriptorSetManager::ResetHWTextureSets()",
        "VulkanDescriptorSet* VkDescriptorSetManager::GetNullTextureDescriptorSet()",
    )
    for fragment in (
        "FixedShadowmapView = VK_NULL_HANDLE;",
        "FixedLightmapView = VK_NULL_HANDLE;",
        "FixedSectorBleedView = VK_NULL_HANDLE;",
        "FixedAccelerationStructure = VK_NULL_HANDLE;",
    ):
        require(fragment, reset, "ResetHWTextureSets")


def test_sector_bleed_reuses_same_size_storage_with_a_real_layout_transition() -> None:
    source = (
        REPO_ROOT
        / "src"
        / "common"
        / "rendering"
        / "vulkan"
        / "textures"
        / "vk_texture.cpp"
    ).read_text(encoding="utf-8")
    bleed = function_body(
        source,
        "void VkTextureManager::SetSectorBleed",
        "void VkTextureManager::CreateLightmap()",
    )

    require("const bool recreate = !SectorBleed.Image", bleed, "SetSectorBleed")
    require("SectorBleed.Image->width != width", bleed, "SetSectorBleed")
    require("SectorBleed.Image->height != height", bleed, "SetSectorBleed")
    require("if (recreate)", bleed, "SetSectorBleed")
    require(".AddImage(&SectorBleed, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, recreate)", bleed, "SetSectorBleed")

    reset = bleed.index("SectorBleed.Reset(fb);")
    recreate_guard = bleed.index("if (recreate)")
    assert reset > recreate_guard, "sector-bleed storage must only reset when dimensions change"


def test_sector_bleed_hashes_at_a_bounded_cadence_before_scanning_sectors() -> None:
    source = (
        REPO_ROOT
        / "src"
        / "rendering"
        / "hwrenderer"
        / "scene"
        / "hw_sectorbleed.cpp"
    ).read_text(encoding="utf-8")
    update = function_body(
        source,
        "void HW_UpdateSectorLightBleed",
        "void HW_UploadSectorLightBleed",
    )

    require("SectorBleedHashCheckIntervalMS = 16", source, "hw_sectorbleed.cpp")
    require("SectorBleedRebuildIntervalMS = 100", source, "hw_sectorbleed.cpp")
    require("const uint64_t now = I_msTimeFS();", update, "HW_UpdateSectorLightBleed")
    require("now - Level->SectorBleedLastCheck < SectorBleedHashCheckIntervalMS", update, "HW_UpdateSectorLightBleed")
    require("Level->SectorBleedLastCheck = now;", update, "HW_UpdateSectorLightBleed")
    assert update.index("now - Level->SectorBleedLastCheck") < update.index(
        "uint64_t hash = 1469598103934665603ULL"
    ), "sector-bleed cadence check must precede the O(sectors) input hash"
    assert update.index("now - Level->SectorBleedLastRebuild") > update.index(
        "uint64_t hash = 1469598103934665603ULL"
    ), "the heavy rebuild limiter must run after the bounded input hash"


def main() -> None:
    test_descriptor_sets_reuse_their_handles_until_resources_change()
    test_sector_bleed_reuses_same_size_storage_with_a_real_layout_transition()
    test_sector_bleed_hashes_at_a_bounded_cadence_before_scanning_sectors()
    print("PASS: Vulkan static descriptor and sector-bleed reuse contracts")


if __name__ == "__main__":
    main()
