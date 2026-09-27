/*
** Sector-to-sector light bleed map generation.
**
** This builds a small world-space texture containing the base light color for
** each point on the map, blended toward the nearest neighboring sector. The
** hardware shader mixes this into the sector light before distance/fog shaping.
*/

#include <float.h>
#include <string.h>

#include "g_levellocals.h"
#include "g_mapinfo.h"
#include "hw_cvars.h"
#include "i_time.h"
#include "hw_drawinfo.h"
#include "r_sky.h"
#include "v_video.h"

static uint64_t SectorBleedHashMix(uint64_t hash, uint64_t value)
{
	hash ^= value;
	hash *= 1099511628211ULL;
	return hash;
}

static uint64_t SectorBleedHashFloat(uint64_t hash, float value)
{
	uint32_t bits;
	static_assert(sizeof(bits) == sizeof(value));
	memcpy(&bits, &value, sizeof(bits));
	return SectorBleedHashMix(hash, bits);
}

static double PointLineDistance(const DVector2 &point, const line_t *line)
{
	const DVector2 a = line->v1->fPos();
	const DVector2 b = line->v2->fPos();
	const DVector2 ab = b - a;
	const double lengthSquared = ab.LengthSquared();
	if (lengthSquared <= 0.0)
		return (point - a).Length();

	const double t = clamp((point - a).dot(ab) / lengthSquared, 0.0, 1.0);
	return (point - (a + ab * t)).Length();
}

static bool IsSkySector(const sector_t *sector)
{
	return sector->GetTexture(sector_t::floor) == skyflatnum ||
		sector->GetTexture(sector_t::ceiling) == skyflatnum;
}

static bool CanBleedBetween(FLevelLocals *Level, const sector_t *sector, const sector_t *other, ELightMode lightmode,
	const TArray<uint8_t> &controlSectors)
{
	if (other == nullptr || other == sector)
		return false;
	if (controlSectors[sector->sectornum] || controlSectors[other->sectornum])
		return false;
	if (other->PortalGroup != sector->PortalGroup)
		return false;
	if (IsSkySector(sector) || IsSkySector(other))
		return false;
	return !CheckFog(Level, const_cast<sector_t *>(sector), const_cast<sector_t *>(other), lightmode);
}

static void BuildSectorBleedMap(FLevelLocals *Level, ELightMode lightmode, float distance)
{
	if (Level->vertexes.Size() == 0 || Level->sectors.Size() == 0)
		return;

	double minX = DBL_MAX;
	double minY = DBL_MAX;
	double maxX = -DBL_MAX;
	double maxY = -DBL_MAX;
	for (const auto &vertex : Level->vertexes)
	{
		const DVector2 pos = vertex.fPos();
		minX = min(minX, pos.X);
		minY = min(minY, pos.Y);
		maxX = max(maxX, pos.X);
		maxY = max(maxY, pos.Y);
	}

	minX -= distance;
	minY -= distance;
	maxX += distance;
	maxY += distance;

	const double extentX = max(maxX - minX, 1.0);
	const double extentY = max(maxY - minY, 1.0);
	const double cellSize = max(16.0, max(extentX, extentY) / 256.0);
	const int width = clamp(int(ceil(extentX / cellSize)), 1, 256);
	const int height = clamp(int(ceil(extentY / cellSize)), 1, 256);

	TArray<uint8_t> sectorColors(Level->sectors.Size() * 4, true);
	TArray<uint8_t> controlSectors(Level->sectors.Size(), true);
	for (auto &sector : Level->sectors)
	{
		if (sector.e == nullptr)
			continue;
		for (auto *floor : sector.e->XFloor.ffloors)
		{
			if (floor != nullptr && floor->model != nullptr)
				controlSectors[floor->model->sectornum] = 1;
		}
	}

	for (unsigned i = 0; i < Level->sectors.Size(); ++i)
	{
		sector_t *sector = &Level->sectors[i];
		// The texture stores the mode-independent sector light color. The shader
		// scales this target by the same local light/fog factor that was applied
		// to the owning sector, preserving software and hardware light modes.
		const int light = clamp(int(sector->lightlevel), 0, 255);
		sectorColors[i * 4 + 0] = uint8_t(light * sector->Colormap.LightColor.r / 255);
		sectorColors[i * 4 + 1] = uint8_t(light * sector->Colormap.LightColor.g / 255);
		sectorColors[i * 4 + 2] = uint8_t(light * sector->Colormap.LightColor.b / 255);
		sectorColors[i * 4 + 3] = 255;
	}

	Level->SectorBleedData.Resize(width * height * 4);
	uint8_t *pixels = Level->SectorBleedData.Data();

	for (int y = 0; y < height; ++y)
	{
		const double worldY = minY + (y + 0.5) * extentY / height;
		for (int x = 0; x < width; ++x)
		{
			const double worldX = minX + (x + 0.5) * extentX / width;
			sector_t *sector = Level->PointInSector(worldX, worldY);
			uint8_t *pixel = pixels + (y * width + x) * 4;

			const unsigned sectorIndex = unsigned(sector->sectornum) * 4;
			double red = sectorColors[sectorIndex + 0];
			double green = sectorColors[sectorIndex + 1];
			double blue = sectorColors[sectorIndex + 2];

			double bestDistance = distance;
			sector_t *bestSector = nullptr;
			const DVector2 point(worldX, worldY);
			for (unsigned i = 0; i < sector->Lines.Size(); ++i)
			{
				const line_t *line = sector->Lines[i];
				sector_t *other = line->frontsector == sector ? line->backsector : line->frontsector;
				if (!CanBleedBetween(Level, sector, other, lightmode, controlSectors))
					continue;

				const double lineDistance = PointLineDistance(point, line);
				if (lineDistance < bestDistance)
				{
					bestDistance = lineDistance;
					bestSector = other;
				}
			}

			if (bestSector != nullptr)
			{
				const unsigned otherIndex = unsigned(bestSector->sectornum) * 4;
				// At the boundary both sectors contribute equally; the influence
				// then falls linearly to zero at the configured bleed distance.
				const double blend = 0.5 * (1.0 - bestDistance / distance);
				red = red * (1.0 - blend) + sectorColors[otherIndex + 0] * blend;
				green = green * (1.0 - blend) + sectorColors[otherIndex + 1] * blend;
				blue = blue * (1.0 - blend) + sectorColors[otherIndex + 2] * blend;
			}

			pixel[0] = uint8_t(clamp(red + 0.5, 0.0, 255.0));
			pixel[1] = uint8_t(clamp(green + 0.5, 0.0, 255.0));
			pixel[2] = uint8_t(clamp(blue + 0.5, 0.0, 255.0));
			pixel[3] = 255;
		}
	}

	Level->SectorBleedWidth = width;
	Level->SectorBleedHeight = height;
	Level->SectorBleedMinX = float(minX);
	Level->SectorBleedMinY = float(minY);
	Level->SectorBleedInvWidth = float(1.0 / extentX);
	Level->SectorBleedInvHeight = float(1.0 / extentY);
	Level->SectorBleedDistance = distance;
}

void HW_UpdateSectorLightBleed(FLevelLocals *Level, ELightMode lightmode)
{
	if (Level == nullptr || screen == nullptr || !bd_sectorlight_bleed ||
		(Level->flags3 & LEVEL3_NOLIGHTFADE) || !screen->SupportsSectorBleed())
		return;

	const float distance = clamp(*bd_sectorlight_distance, 16.0f, 512.0f);
	uint64_t hash = 1469598103934665603ULL;
	hash = SectorBleedHashMix(hash, uint64_t(lightmode));
	hash = SectorBleedHashFloat(hash, distance);
	for (const auto &sector : Level->sectors)
	{
		hash = SectorBleedHashMix(hash, uint16_t(sector.lightlevel));
		hash = SectorBleedHashMix(hash, sector.Colormap.LightColor.d);
		hash = SectorBleedHashMix(hash, uint32_t(sector.Colormap.BlendFactor));
	}

	if (hash == Level->SectorBleedHash && Level->SectorBleedData.Size() != 0)
		return;

	// Rate-limit rebuilds: flickering sector light can change the hash every
	// frame, and rebuilding/uploading the map each time is not free. While a
	// valid map exists and the last rebuild is recent, keep serving it; the
	// stale hash keeps this check live so the refresh lands once the window
	// elapsed. First build (no timestamp) or empty data always builds now.
	const uint64_t now = I_msTimeFS();
	if (Level->SectorBleedData.Size() != 0 && Level->SectorBleedLastRebuild != 0 &&
		now - Level->SectorBleedLastRebuild < 100)
		return;

	BuildSectorBleedMap(Level, lightmode, distance);
	Level->SectorBleedHash = hash;
	Level->SectorBleedLastRebuild = now;
	screen->InitSectorBleed(Level->SectorBleedWidth, Level->SectorBleedHeight, Level->SectorBleedData);
}

void HW_UploadSectorLightBleed(FLevelLocals *Level)
{
	if (Level != nullptr && screen != nullptr && screen->SupportsSectorBleed() && Level->SectorBleedData.Size() != 0)
		screen->InitSectorBleed(Level->SectorBleedWidth, Level->SectorBleedHeight, Level->SectorBleedData);
}
