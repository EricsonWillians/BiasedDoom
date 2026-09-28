/*
** file_zip.cpp
**
**---------------------------------------------------------------------------
** Copyright 1998-2009 Randy Heit
** Copyright 2005-2023 Christoph Oelckers
** All rights reserved.
**
** Redistribution and use in source and binary forms, with or without
** modification, are permitted provided that the following conditions
** are met:
**
** 1. Redistributions of source code must retain the above copyright
**    notice, this list of conditions and the following disclaimer.
** 2. Redistributions in binary form must reproduce the above copyright
**    notice, this list of conditions and the following disclaimer in the
**    documentation and/or other materials provided with the distribution.
** 3. The name of the author may not be used to endorse or promote products
**    derived from this software without specific prior written permission.
**
** THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
** IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
** OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
** IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
** INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
** NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
** DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
** THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
** (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
** THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
**---------------------------------------------------------------------------
**
**
*/

#include <time.h>
#include <stdexcept>
#include <cstdint>
#include "w_zip.h"
#include "ancientzip.h"
#include "resourcefile.h"
#include "fs_findfile.h"
#include "fs_swap.h"
#include "fs_stringpool.h"

namespace FileSys {
	using namespace byteswap;

#define BUFREADCOMMENT (0x400)

//-----------------------------------------------------------------------
//
// Finds the central directory end record in the end of the file.
// Taken from Quake3 source but the file in question is not GPL'ed. ;)
//
//-----------------------------------------------------------------------

static uint32_t Zip_FindCentralDir(FileReader &fin, bool* zip64)
{
	unsigned char buf[BUFREADCOMMENT + 4];
	uint32_t FileSize;
	uint32_t uBackRead;
	uint32_t uMaxBack; // maximum size of global comment
	uint32_t uPosFound=0;

	FileSize = (uint32_t)fin.GetLength();
	uMaxBack = std::min<uint32_t>(0xffff, FileSize);

	uBackRead = 4;
	while (uBackRead < uMaxBack)
	{
		uint32_t uReadSize, uReadPos;
		int i;
		if (uBackRead + BUFREADCOMMENT > uMaxBack) 
			uBackRead = uMaxBack;
		else
			uBackRead += BUFREADCOMMENT;
		uReadPos = FileSize - uBackRead;

		uReadSize = std::min<uint32_t>((BUFREADCOMMENT + 4), (FileSize - uReadPos));

		if (fin.Seek(uReadPos, FileReader::SeekSet) != 0) break;

		if (fin.Read(buf, (int32_t)uReadSize) != (int32_t)uReadSize) break;

		for (i = (int)uReadSize - 3; (i--) > 0;)
		{
			if (buf[i] == 'P' && buf[i+1] == 'K' && buf[i+2] == 5 && buf[i+3] == 6 && !*zip64 && uPosFound == 0)
			{
				*zip64 = false;
				uPosFound = uReadPos + i;
			}
			if (buf[i] == 'P' && buf[i+1] == 'K' && buf[i+2] == 6 && buf[i+3] == 6)
			{
				*zip64 = true;
				uPosFound = uReadPos + i;
				return uPosFound;
			}
		}
	}
	return uPosFound;
}

//==========================================================================
//
// Zip file
//
//==========================================================================

class FZipFile : public FResourceFile
{
	void SetEntryAddress(uint32_t entry) override;

public:
	FZipFile(const char* filename, FileReader& file, StringPool* sp);
	bool Open(LumpFilterInfo* filter, FileSystemMessageFunc Printf);
	FCompressedBuffer GetRawData(uint32_t entry) override;
};

//==========================================================================
//
// Zip file
//
//==========================================================================

FZipFile::FZipFile(const char * filename, FileReader &file, StringPool* sp)
: FResourceFile(filename, file, sp)
{
}

bool FZipFile::Open(LumpFilterInfo* filter, FileSystemMessageFunc Printf)
{
	bool zip64 = false;
	uint32_t centraldir = Zip_FindCentralDir(Reader, &zip64);
	int skipped = 0;

	if (centraldir == 0)
	{
		Printf(FSMessageLevel::Error, "%s: ZIP file corrupt!\n", FileName);
		return false;
	}

	uint64_t dirsize, DirectoryOffset;
	if (!zip64)
	{
		FZipEndOfCentralDirectory info;
		// Read the central directory info.
		Reader.Seek(centraldir, FileReader::SeekSet);
		Reader.Read(&info, sizeof(FZipEndOfCentralDirectory));

		// No multi-disk zips!
		if (info.NumEntries != info.NumEntriesOnAllDisks ||
			info.FirstDisk != 0 || info.DiskNumber != 0)
		{
			Printf(FSMessageLevel::Error, "%s: Multipart Zip files are not supported.\n", FileName);
			return false;
		}
		
		NumLumps = LittleShort(info.NumEntries);
		dirsize = LittleLong(info.DirectorySize);
		DirectoryOffset = LittleLong(info.DirectoryOffset);
	}
	else
	{
		FZipEndOfCentralDirectory64 info;
		// Read the central directory info.
		Reader.Seek(centraldir, FileReader::SeekSet);
		Reader.Read(&info, sizeof(FZipEndOfCentralDirectory64));

		// No multi-disk zips!
		if (info.NumEntries != info.NumEntriesOnAllDisks ||
			info.FirstDisk != 0 || info.DiskNumber != 0)
		{
			Printf(FSMessageLevel::Error, "%s: Multipart Zip files are not supported.\n", FileName);
			return false;
		}
		
		NumLumps = (uint32_t)info.NumEntries;
		dirsize = info.DirectorySize;
		DirectoryOffset = info.DirectoryOffset;
	}
	// Load the entire central directory. Too bad that this contains variable length entries...
	// Reject obviously bogus directory locations before reading anything.
	if (DirectoryOffset > (uint64_t)Reader.GetLength() || dirsize > (uint64_t)Reader.GetLength() - DirectoryOffset)
	{
		Printf(FSMessageLevel::Error, "%s: Central directory corrupted.", FileName);
		return false;
	}
	void *directory = malloc(dirsize);
	if (directory == nullptr)
	{
		Printf(FSMessageLevel::Error, "%s: Out of memory reading central directory.", FileName);
		return false;
	}
	Reader.Seek(DirectoryOffset, FileReader::SeekSet);
	if (Reader.Read(directory, dirsize) != (ptrdiff_t)dirsize)
	{
		free(directory);
		Printf(FSMessageLevel::Error, "%s: Central directory corrupted.", FileName);
		return false;
	}

	char *dirptr = (char*)directory;
	char *dirend = (char*)directory + dirsize;

	std::string name0, name1;
	bool foundspeciallump = false;
	bool foundprefix = false;

	// Check if all files have the same prefix so that this can be stripped out.
	// This will only be done if there is either a MAPINFO, ZMAPINFO or GAMEINFO lump in the subdirectory, denoting a ZDoom mod.
	if (NumLumps > 1) for (uint32_t i = 0; i < NumLumps; i++)
	{
		if (dirend - dirptr < (ptrdiff_t)sizeof(FZipCentralDirectoryInfo))
		{
			free(directory);
			Printf(FSMessageLevel::Error, "%s: Central directory corrupted.", FileName);
			return false;
		}
		FZipCentralDirectoryInfo *zip_fh = (FZipCentralDirectoryInfo *)dirptr;

		uint32_t nameLen = LittleShort(zip_fh->NameLength);
		uint32_t extraLen = LittleShort(zip_fh->ExtraLength);
		uint32_t commentLen = LittleShort(zip_fh->CommentLength);
		// The complete fixed header + name + extra + comment must fit in the
		// remaining directory bytes before the name is used.
		if ((uint64_t)sizeof(FZipCentralDirectoryInfo) + nameLen + extraLen + commentLen > (uint64_t)(dirend - dirptr))
		{
			free(directory);
			Printf(FSMessageLevel::Error, "%s: Central directory corrupted.", FileName);
			return false;
		}
		std::string name(dirptr + sizeof(FZipCentralDirectoryInfo), nameLen);

		dirptr += sizeof(FZipCentralDirectoryInfo) + nameLen + extraLen + commentLen;
	}

	dirptr = (char*)directory;
	AllocateEntries(NumLumps);
	auto Entry = Entries;
	for (uint32_t i = 0; i < NumLumps; i++)
	{
		if (dirend - dirptr < (ptrdiff_t)sizeof(FZipCentralDirectoryInfo))
		{
			free(directory);
			Printf(FSMessageLevel::Error, "%s: Central directory corrupted.", FileName);
			return false;
		}
		FZipCentralDirectoryInfo *zip_fh = (FZipCentralDirectoryInfo *)dirptr;

		uint32_t nameLen = LittleShort(zip_fh->NameLength);
		uint32_t extraLen = LittleShort(zip_fh->ExtraLength);
		uint32_t commentLen = LittleShort(zip_fh->CommentLength);
		// The complete fixed header + name + extra + comment must fit in the
		// remaining directory bytes before the name is used.
		if ((uint64_t)sizeof(FZipCentralDirectoryInfo) + nameLen + extraLen + commentLen > (uint64_t)(dirend - dirptr))
		{
			free(directory);
			Printf(FSMessageLevel::Error, "%s: Central directory corrupted.", FileName);
			return false;
		}
		std::string name(dirptr + sizeof(FZipCentralDirectoryInfo), nameLen);
		dirptr += sizeof(FZipCentralDirectoryInfo) + nameLen + extraLen + commentLen;

		// skip Directories
		if (name.empty() || (name.back() == '/' && LittleLong(zip_fh->UncompressedSize32) == 0))
		{
			skipped++;
			continue;
		}

		// Ignore unknown compression formats
		zip_fh->Method = LittleShort(zip_fh->Method);
		if (zip_fh->Method != METHOD_STORED &&
			zip_fh->Method != METHOD_DEFLATE &&
			zip_fh->Method != METHOD_LZMA &&
			zip_fh->Method != METHOD_BZIP2 &&
			zip_fh->Method != METHOD_IMPLODE &&
			zip_fh->Method != METHOD_SHRINK &&
			zip_fh->Method != METHOD_XZ)
		{
			Printf(FSMessageLevel::Error, "%s: '%s' uses an unsupported compression algorithm (#%d).\n", FileName, name.c_str(), zip_fh->Method);
			skipped++;
			continue;
		}
		// Also ignore encrypted entries
		zip_fh->Flags = LittleShort(zip_fh->Flags);
		if (zip_fh->Flags & ZF_ENCRYPTED)
		{
			Printf(FSMessageLevel::Error, "%s: '%s' is encrypted. Encryption is not supported.\n", FileName, name.c_str());
			skipped++;
			continue;
		}

		uint32_t UncompressedSize =LittleLong(zip_fh->UncompressedSize32);
		uint32_t CompressedSize = LittleLong(zip_fh->CompressedSize32);
		uint64_t LocalHeaderOffset = LittleLong(zip_fh->LocalHeaderOffset32);
		if (extraLen > 0)
		{
			uint8_t* rawext = (uint8_t*)zip_fh + sizeof(*zip_fh) + nameLen;
			uint32_t ExtraLength = extraLen;
			bool toolarge = false;

			// Walk the extra fields with a bounded cursor so a bogus block
			// length can never run past the end of the extra field.
			while (ExtraLength >= 4)
			{
				auto zip_64 = (FZipCentralDirectoryInfo64BitExt*)rawext;
				uint32_t BlockLength = LittleShort(zip_64->Length);
				if (BlockLength + 4 > ExtraLength) break;
				rawext += BlockLength + 4;
				ExtraLength -= BlockLength + 4;
				if (LittleShort(zip_64->Type) == 1 && BlockLength >= 0x18)
				{
					if (zip_64->CompressedSize > 0x7fffffff || zip_64->UncompressedSize > 0x7fffffff)
					{
						// The file system is limited to 32 bit file sizes;
						Printf(FSMessageLevel::Warning, "%s: '%s' is too large.\n", FileName, name.c_str());
						skipped++;
						toolarge = true;
						break;
					}
					UncompressedSize = (uint32_t)zip_64->UncompressedSize;
					CompressedSize = (uint32_t)zip_64->CompressedSize;
					LocalHeaderOffset = zip_64->LocalHeaderOffset;
				}
			}
			if (toolarge) continue;
		}

		Entry->FileName = NormalizeFileName(name.c_str());
		Entry->Length = UncompressedSize;
		// The start of the Reader will be determined the first time it is accessed.
		Entry->Flags = RESFF_FULLPATH | RESFF_NEEDFILESTART;
		Entry->Method = uint8_t(zip_fh->Method);
		if (Entry->Method != METHOD_STORED) Entry->Flags |= RESFF_COMPRESSED;
		if (Entry->Method == METHOD_IMPLODE)
		{
			// for Implode merge the flags into the compression method to make handling in the file system easier and save one variable.
			if ((zip_fh->Flags & 6) == 2) Entry->Method = METHOD_IMPLODE_2;
			else if ((zip_fh->Flags & 6) == 4) Entry->Method = METHOD_IMPLODE_4;
			else if ((zip_fh->Flags & 6) == 6) Entry->Method = METHOD_IMPLODE_6;
			else Entry->Method = METHOD_IMPLODE_0;
		}
		Entry->CRC32 = zip_fh->CRC32;
		Entry->CompressedSize = CompressedSize;
		Entry->Position = LocalHeaderOffset;

		Entry++;

	}
	// Resize the lump record array to its actual size
	NumLumps -= skipped;
	free(directory);

	GenerateHash();
	PostProcessArchive(filter);
	return true;
}

//==========================================================================
//
//
//
//==========================================================================

FCompressedBuffer FZipFile::GetRawData(uint32_t entry)
{
	FCompressedBuffer cbuf;

	if (entry >= NumLumps || Entries[entry].Length == 0)
	{
		cbuf = { 0, 0, METHOD_STORED, 0, 0, nullptr };
	}
	else
	{
		auto& e = Entries[entry];
		// For stored entries the reported size must never exceed the allocated buffer.
		size_t msize = (e.Method == METHOD_STORED && e.Length > e.CompressedSize) ? e.CompressedSize : e.Length;
		cbuf = { msize, (size_t)e.CompressedSize, e.Method, e.CRC32, new char[e.CompressedSize] };
		if (e.Flags & RESFF_NEEDFILESTART) SetEntryAddress(entry);
		Reader.Seek(e.Position, FileReader::SeekSet);
		Reader.Read(cbuf.mBuffer, e.CompressedSize);
	}
	
	return cbuf;
}

//==========================================================================
//
// SetLumpAddress
//
//==========================================================================

void FZipFile::SetEntryAddress(uint32_t entry)
{
	// This file is inside a zip and has not been opened before.
	// Position points to the start of the local file header, which we must
	// read and skip so that we can get to the actual file data.
	FZipLocalFileHeader localHeader;
	int skiplen;

	Reader.Seek(Entries[entry].Position, FileReader::SeekSet);
	Reader.Read(&localHeader, sizeof(localHeader));
	skiplen = LittleShort(localHeader.NameLength) + LittleShort(localHeader.ExtraLength);
	Entries[entry].Position += sizeof(localHeader) + skiplen;
	Entries[entry].Flags &= ~RESFF_NEEDFILESTART;
}

//==========================================================================
//
// File open
//
//==========================================================================

FResourceFile *CheckZip(const char *filename, FileReader &file, LumpFilterInfo* filter, FileSystemMessageFunc Printf, StringPool* sp)
{
	char head[4];

	if (file.GetLength() >= (ptrdiff_t)sizeof(FZipLocalFileHeader))
	{
		file.Seek(0, FileReader::SeekSet);
		file.Read(&head, 4);
		file.Seek(0, FileReader::SeekSet);
		if (!memcmp(head, "PK\x3\x4", 4))
		{
			auto rf = new FZipFile(filename, file, sp);
			if (rf->Open(filter, Printf)) return rf;
			file = rf->Destroy();
		}
	}
	return NULL;
}


}
