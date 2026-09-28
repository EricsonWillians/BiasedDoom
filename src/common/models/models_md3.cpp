// 
//---------------------------------------------------------------------------
//
// Copyright(C) 2006-2016 Christoph Oelckers
// All rights reserved.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Lesser General Public License as published by
// the Free Software Foundation, either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Lesser General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with this program.  If not, see http://www.gnu.org/licenses/
//
//--------------------------------------------------------------------------
//

#include "filesystem.h"
#include "cmdlib.h"
#include "model_md3.h"
#include "texturemanager.h"
#include "modelrenderer.h"
#include "m_swap.h"
#include "printf.h"

#define MAX_QPATH 64

#ifdef _MSC_VER
#pragma warning(disable:4244) // warning C4244: conversion from 'double' to 'float', possible loss of data
#endif

//===========================================================================
//
// decode the lat/lng normal to a 3 float normal
//
//===========================================================================

static void UnpackVector(unsigned short packed, float & nx, float & ny, float & nz)
{
	double lat = ( packed >> 8 ) & 0xff;
	double lng = ( packed & 0xff );
	lat *= M_PI/128;
	lng *= M_PI/128;

	nx = cos(lat) * sin(lng);
	ny = sin(lat) * sin(lng);
	nz = cos(lng);
}

//===========================================================================
//
// MD3 File structure
//
//===========================================================================

#pragma pack(4)
struct md3_header_t
{
	uint32_t Magic;
	uint32_t Version;
	char Name[MAX_QPATH];
	uint32_t Flags;
	uint32_t Num_Frames;
	uint32_t Num_Tags;
	uint32_t Num_Surfaces;
	uint32_t Num_Skins;
	uint32_t Ofs_Frames;
	uint32_t Ofs_Tags;
	uint32_t Ofs_Surfaces;
	uint32_t Ofs_Eof;
};

struct md3_surface_t
{
	uint32_t Magic;
	char Name[MAX_QPATH];
	uint32_t Flags;
	uint32_t Num_Frames;
	uint32_t Num_Shaders;
	uint32_t Num_Verts;
	uint32_t Num_Triangles;
	uint32_t Ofs_Triangles;
	uint32_t Ofs_Shaders;
	uint32_t Ofs_Texcoord;
	uint32_t Ofs_XYZNormal;
	uint32_t Ofs_End;
};

struct md3_triangle_t
{
	uint32_t vt_index[3];
};

struct md3_shader_t
{
	char Name[MAX_QPATH];
	uint32_t index;
};

struct md3_texcoord_t
{
	float s, t;
};

struct md3_vertex_t
{
	short x, y, z, n;
};

struct md3_frame_t
{
	float min_Bounds[3];
	float max_Bounds[3];
	float localorigin[3];
	float radius;
	char Name[16];
};
#pragma pack()


//===========================================================================
//
//
//
//===========================================================================

bool FMD3Model::Load(const char * path, int lumpnum, const char * buffer, int length)
{
	if (length < (int)sizeof(md3_header_t))
	{
		Printf("LoadModel: Model '%s' file too short\n", path);
		return false;
	}

	md3_header_t * hdr = (md3_header_t *)buffer;

	if ((int)hdr->Magic != LittleLong(MD3_MAGIC))
	{
		Printf("LoadModel: Model '%s' is not a valid MD3\n", path);
		return false;
	}

	// Header counts/offsets are uint32_t: values above INT_MAX are treated as
	// negative by the original signed code and must be rejected before any
	// arithmetic or allocations.
	uint32_t numFrames = LittleLong(hdr->Num_Frames);
	uint32_t numSurfaces = LittleLong(hdr->Num_Surfaces);
	uint32_t ofsFrames = LittleLong(hdr->Ofs_Frames);
	uint32_t ofsSurfaces = LittleLong(hdr->Ofs_Surfaces);
	if (numFrames == 0 || numFrames > 0x10000 || numSurfaces == 0 || numSurfaces > 0x1000 ||
		ofsFrames > 0x7fffffffu || ofsSurfaces > 0x7fffffffu)
	{
		Printf("LoadModel: Model '%s' has invalid counts\n", path);
		return false;
	}
	if ((int64_t)ofsFrames + (int64_t)numFrames * (int)sizeof(md3_frame_t) > length)
	{
		Printf("LoadModel: Model '%s' file too short\n", path);
		return false;
	}
	if ((int64_t)ofsSurfaces + (int)sizeof(md3_surface_t) > length)
	{
		Printf("LoadModel: Model '%s' file too short\n", path);
		return false;
	}

	hasSurfaces = numSurfaces > 1;

	numTags = LittleLong(hdr->Num_Tags);

	md3_frame_t * frm = (md3_frame_t*)(buffer + LittleLong(hdr->Ofs_Frames));

	Frames.Resize(numFrames);
	for (unsigned i = 0; i < numFrames; i++)
	{
		strncpy(Frames[i].Name, frm[i].Name, 15);
		for (int j = 0; j < 3; j++) Frames[i].origin[j] = frm[i].localorigin[j];
	}

	md3_surface_t * surf = (md3_surface_t*)(buffer + LittleLong(hdr->Ofs_Surfaces));

	Surfaces.Resize(numSurfaces);

	for (unsigned i = 0; i < numSurfaces; i++)
	{
		MD3Surface * s = &Surfaces[i];

		// Validate the surface before walking to the next one, so a corrupt
		// Ofs_End cannot send the walk out of the file or backwards.
		if ((int64_t)((const char*)surf - buffer) + (int)sizeof(md3_surface_t) > length)
		{
			Printf("LoadModel: Model '%s' has a corrupt surface\n", path);
			return false;
		}
		uint32_t surfEnd = LittleLong(surf->Ofs_End);
		if (surfEnd > 0x7fffffffu || surfEnd < sizeof(md3_surface_t) ||
			(int64_t)((const char*)surf - buffer) + surfEnd > length)
		{
			Printf("LoadModel: Model '%s' has a corrupt surface\n", path);
			return false;
		}

		md3_surface_t * ss = surf;

		surf = (md3_surface_t *)(((char*)surf) + surfEnd);

		uint32_t numSkins = LittleLong(ss->Num_Shaders);
		uint32_t numTriangles = LittleLong(ss->Num_Triangles);
		uint32_t numVertices = LittleLong(ss->Num_Verts);
		uint32_t ofsShaders = LittleLong(ss->Ofs_Shaders);
		uint32_t ofsTriangles = LittleLong(ss->Ofs_Triangles);
		uint32_t ofsTexcoord = LittleLong(ss->Ofs_Texcoord);
		uint32_t ofsXYZNormal = LittleLong(ss->Ofs_XYZNormal);

		// Values above INT_MAX were negative to the original signed code; the
		// caps also keep the offset products below int64_t overflow.
		if (numSkins > 0x10000 || numTriangles > 0x100000 || numVertices > 0x100000 ||
			ofsShaders > 0x7fffffffu || ofsTriangles > 0x7fffffffu ||
			ofsTexcoord > 0x7fffffffu || ofsXYZNormal > 0x7fffffffu)
		{
			Printf("LoadModel: Model '%s' has a corrupt surface\n", path);
			return false;
		}
		s->numSkins = numSkins;
		s->numTriangles = numTriangles;
		s->numVertices = numVertices;

		// All surface-local offsets must stay inside the surface.
		if ((int64_t)ofsShaders + (int64_t)numSkins * (int)sizeof(md3_shader_t) > surfEnd ||
			(int64_t)ofsTriangles + (int64_t)numTriangles * (int)sizeof(md3_triangle_t) > surfEnd ||
			(int64_t)ofsTexcoord + (int64_t)numVertices * (int)sizeof(md3_texcoord_t) > surfEnd ||
			(int64_t)ofsXYZNormal + (int64_t)numVertices * (int64_t)Frames.Size() * (int)sizeof(md3_vertex_t) > surfEnd)
		{
			Printf("LoadModel: Model '%s' has a corrupt surface\n", path);
			return false;
		}

		// copy shaders (skins)
		md3_shader_t * shader = (md3_shader_t*)(((char*)ss) + LittleLong(ss->Ofs_Shaders));
		s->Skins.Resize(s->numSkins);

		for (unsigned ii = 0; ii < s->numSkins; ii++)
		{
			// [BB] According to the MD3 spec, Name is supposed to include the full path.
			// ... and since some tools seem to output backslashes, these need to be replaced with forward slashes to work.
			FixPathSeperator(shader[ii].Name);
			s->Skins[ii] = LoadSkin("", shader[ii].Name);
			// [BB] Fall back and check if Name is relative.
			if (!s->Skins[ii].isValid())
				s->Skins[ii] = LoadSkin(path, shader[ii].Name);
		}
	}
	mLumpNum = lumpnum;
	return true;
}

//===========================================================================
//
//
//
//===========================================================================

void FMD3Model::LoadGeometry()
{
	auto lumpdata = fileSystem.ReadFile(mLumpNum);
	auto buffer = lumpdata.string();
	md3_header_t * hdr = (md3_header_t *)buffer;
	md3_surface_t * surf = (md3_surface_t*)(buffer + LittleLong(hdr->Ofs_Surfaces));

	for (unsigned i = 0; i < Surfaces.Size(); i++)
	{
		MD3Surface * s = &Surfaces[i];
		md3_surface_t * ss = surf;

		surf = (md3_surface_t *)(((char*)surf) + LittleLong(surf->Ofs_End));

		// copy triangle indices
		md3_triangle_t * tris = (md3_triangle_t*)(((char*)ss) + LittleLong(ss->Ofs_Triangles));
		s->Tris.Resize(s->numTriangles);

		for (unsigned ii = 0; ii < s->numTriangles; ii++) for (int j = 0; j < 3; j++)
		{
			// Clamp corrupt vertex indices so the index buffer can never
			// reference past the surface's vertex data.
			unsigned int idx = LittleLong(tris[ii].vt_index[j]);
			s->Tris[ii].VertIndex[j] = (int)min<unsigned int>(idx, s->numVertices > 0 ? s->numVertices - 1 : 0);
		}

		// Load texture coordinates
		md3_texcoord_t * tc = (md3_texcoord_t*)(((char*)ss) + LittleLong(ss->Ofs_Texcoord));
		s->Texcoords.Resize(s->numVertices);

		for (unsigned ii = 0; ii < s->numVertices; ii++)
		{
			s->Texcoords[ii].s = tc[ii].s;
			s->Texcoords[ii].t = tc[ii].t;
		}

		// Load vertices and texture coordinates
		md3_vertex_t * vt = (md3_vertex_t*)(((char*)ss) + LittleLong(ss->Ofs_XYZNormal));
		s->Vertices.Resize(s->numVertices * Frames.Size());

		for (unsigned ii = 0; ii < s->numVertices * Frames.Size(); ii++)
		{
			s->Vertices[ii].x = LittleShort(vt[ii].x) / 64.f;
			s->Vertices[ii].y = LittleShort(vt[ii].y) / 64.f;
			s->Vertices[ii].z = LittleShort(vt[ii].z) / 64.f;
			UnpackVector(LittleShort(vt[ii].n), s->Vertices[ii].nx, s->Vertices[ii].ny, s->Vertices[ii].nz);
		}
	}
}

//===========================================================================
//
//
//
//===========================================================================

void FMD3Model::BuildVertexBuffer(FModelRenderer *renderer)
{
	if (!GetVertexBuffer(renderer->GetType()))
	{
		LoadGeometry();

		unsigned int vbufsize = 0;
		unsigned int ibufsize = 0;

		for (unsigned i = 0; i < Surfaces.Size(); i++)
		{
			MD3Surface * surf = &Surfaces[i];
			vbufsize += Frames.Size() * surf->numVertices;
			ibufsize += 3 * surf->numTriangles;
		}

		auto vbuf = renderer->CreateVertexBuffer(true, Frames.Size() == 1);
		SetVertexBuffer(renderer->GetType(), vbuf);

		FModelVertex *vertptr = vbuf->LockVertexBuffer(vbufsize);
		unsigned int *indxptr = vbuf->LockIndexBuffer(ibufsize);

		assert(vertptr != nullptr && indxptr != nullptr);

		unsigned int vindex = 0, iindex = 0;

		for (unsigned i = 0; i < Surfaces.Size(); i++)
		{
			MD3Surface * surf = &Surfaces[i];

			surf->vindex = vindex;
			surf->iindex = iindex;
			for (unsigned j = 0; j < Frames.Size() * surf->numVertices; j++)
			{
				MD3Vertex* vert = &surf->Vertices[j];

				FModelVertex *bvert = &vertptr[vindex++];

				int tc = j % surf->numVertices;
				bvert->Set(vert->x, vert->z, vert->y, surf->Texcoords[tc].s, surf->Texcoords[tc].t);
				bvert->SetNormal(vert->nx, vert->nz, vert->ny);
			}

			for (unsigned k = 0; k < surf->numTriangles; k++)
			{
				for (int l = 0; l < 3; l++)
				{
					indxptr[iindex++] = surf->Tris[k].VertIndex[l];
				}
			}
			surf->UnloadGeometry();
		}
		vbuf->UnlockVertexBuffer();
		vbuf->UnlockIndexBuffer();
	}
}


//===========================================================================
//
// for skin precaching
//
//===========================================================================

void FMD3Model::AddSkins(uint8_t *hitlist, const FTextureID* surfaceskinids)
{
	// The caller provides only MD3_MAX_SURFACES skin slots per model, so
	// never index past that: a crafted model with more surfaces would read
	// garbage FTextureIDs and write the precache hitlist out of bounds.
	size_t maxSurfaces = min((size_t)Surfaces.Size(), (size_t)MD3_MAX_SURFACES);
	for (unsigned i = 0; i < Surfaces.Size(); i++)
	{
		if (surfaceskinids && i < maxSurfaces && surfaceskinids[i].isValid())
		{
			hitlist[surfaceskinids[i].GetIndex()] |= FTextureManager::HIT_Flat;
		}

		MD3Surface * surf = &Surfaces[i];
		for (unsigned j = 0; j < surf->numSkins; j++)
		{
			if (surf->Skins[j].isValid())
			{
				hitlist[surf->Skins[j].GetIndex()] |= FTextureManager::HIT_Flat;
			}
		}
	}
}

//===========================================================================
//
//
//
//===========================================================================

int FMD3Model::FindFrame(const char* name, bool nodefault)
{
	for (unsigned i = 0; i < Frames.Size(); i++)
	{
		if (!stricmp(name, Frames[i].Name)) return i;
	}
	return FErr_NotFound;
}

//===========================================================================
//
//
//
//===========================================================================

void FMD3Model::RenderFrame(FModelRenderer *renderer, FGameTexture * skin, int frameno, int frameno2, double inter, FTranslationID translation, const FTextureID* surfaceskinids, int boneStartPosition)
{
	if ((unsigned)frameno >= Frames.Size() || (unsigned)frameno2 >= Frames.Size()) return;

	renderer->SetInterpolation(inter);
	for (unsigned i = 0; i < Surfaces.Size(); i++)
	{
		MD3Surface * surf = &Surfaces[i];

		// [BB] In case no skin is specified via MODELDEF, check if the MD3 has a skin for the current surface.
		// Note: Each surface may have a different skin.
		FGameTexture *surfaceSkin = skin;
		if (!surfaceSkin)
		{
			// Same MD3_MAX_SURFACES slice limit as in AddSkins.
			if (surfaceskinids && i < MD3_MAX_SURFACES && surfaceskinids[i].isValid())
			{
				surfaceSkin = TexMan.GetGameTexture(surfaceskinids[i], true);
			}
			else if (surf->numSkins > 0 && surf->Skins[0].isValid())
			{
				surfaceSkin = TexMan.GetGameTexture(surf->Skins[0], true);
			}

			if (!surfaceSkin)
			{
				continue;
			}
		}

		renderer->SetMaterial(surfaceSkin, false, translation);
		renderer->SetupFrame(this, surf->vindex + frameno * surf->numVertices, surf->vindex + frameno2 * surf->numVertices, surf->numVertices, -1);
		renderer->DrawElements(surf->numTriangles * 3, surf->iindex * sizeof(unsigned int));
	}
	renderer->SetInterpolation(0.f);
}

