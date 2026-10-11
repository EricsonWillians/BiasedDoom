#include "online_session.h"

#include <sodium.h>
#include <miniz.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <fstream>
#include <limits>
#include <set>
#include <system_error>

namespace BdOnline
{
namespace
{

bool EnsureSodium(std::string& error)
{
	if (sodium_init() < 0)
	{
		error = "libsodium could not initialize";
		return false;
	}
	return true;
}

bool IsLowerHexDigest(const std::string& digest)
{
	if (digest.size() != OnlineDigestBytes * 2)
		return false;
	for (const auto character : digest)
	{
		if (!((character >= '0' && character <= '9') || (character >= 'a' && character <= 'f')))
			return false;
	}
	return true;
}

bool IsSafeExtension(const std::string& extension)
{
	if (extension.empty() || extension.size() > 16 || extension[0] != '.')
		return false;
	return std::all_of(extension.begin() + 1, extension.end(), [](unsigned char character)
	{
		return (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9');
	});
}

bool IsSafePackageName(const std::string& name)
{
	if (name.empty() || name.size() > 128 || name == "." || name == ".." ||
		name.find('/') != std::string::npos || name.find('\\') != std::string::npos)
		return false;
	return std::all_of(name.begin(), name.end(), [](unsigned char character)
	{
		return std::iscntrl(character) == 0;
	});
}

bool IsSafeMetadata(const std::string& value, size_t maximumLength, bool required)
{
	if ((required && value.empty()) || value.size() > maximumLength)
		return false;
	return std::all_of(value.begin(), value.end(), [](unsigned char character)
	{
		return character >= 32 && character != 127;
	});
}

bool IsArchiveExtension(const std::string& extension)
{
	return extension == ".pk3" || extension == ".zip";
}

bool IsSafeArchiveMember(const char* member)
{
	if (member == nullptr || member[0] == '\0' || member[0] == '/' || member[0] == '\\' ||
		std::strchr(member, '\\') != nullptr || std::strchr(member, ':') != nullptr)
		return false;
	std::string component;
	for (const char* cursor = member;; ++cursor)
	{
		const char character = *cursor;
		if (character == '/' || character == '\0')
		{
			if (component.empty() || component == "." || component == "..")
				return character == '\0' && component.empty() && cursor != member;
			component.clear();
			if (character == '\0')
				return true;
		}
		else if (static_cast<unsigned char>(character) < 32)
		{
			return false;
		}
		else
		{
			component.push_back(character);
		}
	}
}

struct FZipReadContext
{
	std::filesystem::path Path;
};

size_t ReadZipSource(void* opaque, mz_uint64 offset, void* buffer, size_t length)
{
	auto* context = static_cast<FZipReadContext*>(opaque);
	if (offset > static_cast<mz_uint64>(std::numeric_limits<std::streamoff>::max()) ||
		length > static_cast<size_t>(std::numeric_limits<std::streamsize>::max()))
		return 0;
	std::ifstream input(context->Path, std::ios::binary);
	if (!input)
		return 0;
	input.seekg(static_cast<std::streamoff>(offset));
	if (!input)
		return 0;
	input.read(static_cast<char*>(buffer), static_cast<std::streamsize>(length));
	return static_cast<size_t>(input.gcount());
}

bool ValidateArchive(const std::filesystem::path& path, std::string& error)
{
	std::error_code code;
	const auto archiveSize = std::filesystem::file_size(path, code);
	if (code || archiveSize > OnlineMaxArchiveContainerBytes)
	{
		error = "received content archive exceeds the allowed container limit";
		return false;
	}
	FZipReadContext context = { path };
	mz_zip_archive archive = {};
	archive.m_pRead = ReadZipSource;
	archive.m_pIO_opaque = &context;
	if (!mz_zip_reader_init(&archive, archiveSize, MZ_ZIP_FLAG_VALIDATE_HEADERS_ONLY))
	{
		error = "received content archive is malformed";
		return false;
	}
	const auto entryCount = mz_zip_reader_get_num_files(&archive);
	if (entryCount > OnlineMaxArchiveEntries)
	{
		mz_zip_reader_end(&archive);
		error = "received content archive has too many entries";
		return false;
	}
	uint64_t unpackedBytes = 0;
	for (mz_uint index = 0; index < entryCount; ++index)
	{
		mz_zip_archive_file_stat stat = {};
		if (!mz_zip_reader_file_stat(&archive, index, &stat) || stat.m_is_encrypted || !stat.m_is_supported ||
			std::strlen(stat.m_filename) >= MZ_ZIP_MAX_ARCHIVE_FILENAME_SIZE - 1 || !IsSafeArchiveMember(stat.m_filename))
		{
			mz_zip_reader_end(&archive);
			error = "received content archive contains an unsafe or unsupported entry";
			return false;
		}
		if (stat.m_uncomp_size > OnlineMaxArchiveUncompressedBytes ||
			unpackedBytes > OnlineMaxArchiveUncompressedBytes - stat.m_uncomp_size)
		{
			mz_zip_reader_end(&archive);
			error = "received content archive expands beyond the allowed limit";
			return false;
		}
		unpackedBytes += stat.m_uncomp_size;
	}
	mz_zip_reader_end(&archive);
	return true;
}

void HashBytes(crypto_generichash_state& state, const void* bytes, size_t size)
{
	crypto_generichash_update(&state, static_cast<const unsigned char*>(bytes), static_cast<unsigned long long>(size));
}

void HashString(crypto_generichash_state& state, const std::string& value)
{
	HashBytes(state, value.data(), value.size());
	const uint8_t terminator = 0;
	HashBytes(state, &terminator, sizeof(terminator));
}

void HashUint64(crypto_generichash_state& state, uint64_t value)
{
	uint8_t encoded[8] = {};
	for (size_t index = 0; index < sizeof(encoded); ++index)
		encoded[index] = static_cast<uint8_t>(value >> (index * 8));
	HashBytes(state, encoded, sizeof(encoded));
}

bool FinishHash(crypto_generichash_state& state, std::string& digest)
{
	std::array<unsigned char, OnlineDigestBytes> binary = {};
	if (crypto_generichash_final(&state, binary.data(), binary.size()) != 0)
		return false;

	std::array<char, OnlineDigestBytes * 2 + 1> hexadecimal = {};
	sodium_bin2hex(hexadecimal.data(), hexadecimal.size(), binary.data(), binary.size());
	digest = hexadecimal.data();
	return true;
}

bool HashRegularFile(const std::filesystem::path& path, crypto_generichash_state& state, uint64_t& size, std::string& error)
{
	std::ifstream input(path, std::ios::binary);
	if (!input)
	{
		error = "could not open content file: " + path.string();
		return false;
	}

	std::array<char, 64 * 1024> buffer = {};
	while (input)
	{
		input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
		const auto count = input.gcount();
		if (count <= 0)
			break;
		if (size > std::numeric_limits<uint64_t>::max() - static_cast<uint64_t>(count))
		{
			error = "content file is too large: " + path.string();
			return false;
		}
		HashBytes(state, buffer.data(), static_cast<size_t>(count));
		size += static_cast<uint64_t>(count);
	}
	if (!input.eof())
	{
		error = "could not read content file: " + path.string();
		return false;
	}
	return true;
}

bool HashFilePath(const std::filesystem::path& path, std::string& digest, uint64_t& size, std::string& error)
{
	crypto_generichash_state state = {};
	crypto_generichash_init(&state, nullptr, 0, OnlineDigestBytes);
	HashString(state, "biaseddoom-online-file-v1");
	size = 0;
	if (!HashRegularFile(path, state, size, error))
		return false;
	if (!FinishHash(state, digest))
	{
		error = "could not finalize content hash";
		return false;
	}
	return true;
}

bool HashDirectoryPath(const std::filesystem::path& root, std::string& digest, uint64_t& size, std::string& error)
{
	std::vector<std::filesystem::path> files;
	std::error_code code;
	for (std::filesystem::recursive_directory_iterator iterator(root, std::filesystem::directory_options::none, code), end;
		iterator != end; iterator.increment(code))
	{
		if (code)
		{
			error = "could not enumerate content directory: " + root.string();
			return false;
		}
		const auto status = iterator->symlink_status(code);
		if (code)
		{
			error = "could not inspect content directory entry";
			return false;
		}
		if (std::filesystem::is_symlink(status))
		{
			error = "directory-backed content cannot contain symlinks: " + iterator->path().string();
			return false;
		}
		if (std::filesystem::is_regular_file(status))
			files.push_back(iterator->path());
		else if (!std::filesystem::is_directory(status))
		{
			error = "directory-backed content contains an unsupported entry: " + iterator->path().string();
			return false;
		}
	}

	std::sort(files.begin(), files.end(), [&root](const auto& left, const auto& right)
	{
		return left.lexically_relative(root).generic_string() < right.lexically_relative(root).generic_string();
	});

	crypto_generichash_state state = {};
	crypto_generichash_init(&state, nullptr, 0, OnlineDigestBytes);
	HashString(state, "biaseddoom-online-directory-v1");
	size = 0;
	for (const auto& file : files)
	{
		const auto relative = file.lexically_relative(root).generic_string();
		if (relative.empty() || relative.compare(0, 3, "../") == 0)
		{
			error = "content directory escaped its root";
			return false;
		}
		HashString(state, relative);
		const auto entrySize = std::filesystem::file_size(file, code);
		if (code)
		{
			error = "could not stat content directory entry";
			return false;
		}
		HashUint64(state, entrySize);
		if (!HashRegularFile(file, state, size, error))
			return false;
	}
	if (!FinishHash(state, digest))
	{
		error = "could not finalize directory content hash";
		return false;
	}
	return true;
}

bool CollectBundleFiles(const std::filesystem::path& root, std::vector<std::filesystem::path>& files, std::string& error)
{
	std::error_code code;
	for (std::filesystem::recursive_directory_iterator iterator(root, std::filesystem::directory_options::none, code), end;
		iterator != end; iterator.increment(code))
	{
		if (code)
		{
			error = "could not enumerate content directory: " + root.string();
			return false;
		}
		const auto status = iterator->symlink_status(code);
		if (code || std::filesystem::is_symlink(status))
		{
			error = "directory-backed content cannot contain unreadable entries or symlinks";
			return false;
		}
		if (std::filesystem::is_regular_file(status))
			files.push_back(iterator->path());
		else if (!std::filesystem::is_directory(status))
		{
			error = "directory-backed content contains an unsupported entry";
			return false;
		}
	}
	std::sort(files.begin(), files.end(), [&root](const auto& left, const auto& right)
	{
		return left.lexically_relative(root).generic_string() < right.lexically_relative(root).generic_string();
	});
	return true;
}

void AppendDatagramNumber(std::array<uint8_t, crypto_aead_chacha20poly1305_ietf_NPUBBYTES>& nonce, uint32_t value, size_t offset)
{
	for (size_t index = 0; index < 4; ++index)
		nonce[offset + index] = static_cast<uint8_t>(value >> (index * 8));
}

void AppendDatagramNumber(std::array<uint8_t, crypto_aead_chacha20poly1305_ietf_NPUBBYTES>& nonce, uint64_t value, size_t offset)
{
	for (size_t index = 0; index < 8; ++index)
		nonce[offset + index] = static_cast<uint8_t>(value >> (index * 8));
}

std::array<uint8_t, crypto_aead_chacha20poly1305_ietf_NPUBBYTES> DatagramNonce(uint32_t epoch, uint64_t sequence)
{
	std::array<uint8_t, crypto_aead_chacha20poly1305_ietf_NPUBBYTES> nonce = {};
	AppendDatagramNumber(nonce, epoch, 0);
	AppendDatagramNumber(nonce, sequence, 4);
	return nonce;
}

std::array<uint8_t, 16> DatagramAssociatedData(uint32_t direction, uint32_t epoch, uint64_t sequence)
{
	std::array<uint8_t, 16> data = {};
	for (size_t index = 0; index < 4; ++index)
		data[index] = static_cast<uint8_t>(direction >> (index * 8));
	for (size_t index = 0; index < 4; ++index)
		data[4 + index] = static_cast<uint8_t>(epoch >> (index * 8));
	for (size_t index = 0; index < 8; ++index)
		data[8 + index] = static_cast<uint8_t>(sequence >> (index * 8));
	return data;
}

bool CalculateContentManifestDigest(const FContentManifest& manifest, std::string& digest, std::string& error)
{
	if (!EnsureSodium(error))
		return false;
	crypto_generichash_state state = {};
	crypto_generichash_init(&state, nullptr, 0, OnlineDigestBytes);
	HashString(state, "biaseddoom-online-manifest-v1");
	for (const auto& package : manifest.Packages)
	{
		HashString(state, package.DisplayName);
		HashString(state, package.Digest);
		HashString(state, package.FileExtension);
		HashUint64(state, package.Size);
		HashUint64(state, package.LoadOrder);
		const uint8_t directory = package.IsDirectoryBundle ? 1 : 0;
		HashBytes(state, &directory, sizeof(directory));
	}
	HashUint64(state, manifest.TotalBytes);
	if (!FinishHash(state, digest))
	{
		error = "could not finalize content manifest hash";
		return false;
	}
	if (digest.size() != OnlineDigestBytes * 2)
	{
		error = "content manifest hash calculation returned an invalid digest length";
		return false;
	}
	return true;
}

bool FinalizeContentManifest(std::vector<FContentPackage> packages, FContentManifest& output, std::string& error)
{
	FContentManifest manifest;
	manifest.Packages = std::move(packages);
	std::sort(manifest.Packages.begin(), manifest.Packages.end(), [](const auto& left, const auto& right)
	{
		return left.LoadOrder < right.LoadOrder;
	});
	for (const auto& package : manifest.Packages)
	{
		if (package.Size > OnlineMaxContentPackageBytes || manifest.TotalBytes > OnlineMaxContentBytes - package.Size)
		{
			error = "content manifest total size exceeds the online limit";
			return false;
		}
		manifest.TotalBytes += package.Size;
	}

	// Calculate first, then attach the exact digest that will be verified by
	// every guest before it accepts this manifest.
	std::string rootDigest;
	if (!CalculateContentManifestDigest(manifest, rootDigest, error))
		return false;
	manifest.RootDigest = rootDigest;
	if (!manifest.Validate(error))
		return false;
	output = std::move(manifest);
	return true;
}

} // namespace

void FOnlineSession::Fail(std::string error)
{
	LastError = std::move(error);
	Phase = EOnlineSessionPhase::Failed;
}

bool FOnlineSession::Transition(EOnlineSessionPhase expected, EOnlineSessionPhase next, std::string& error)
{
	if (Phase != expected)
	{
		error = "online session state transition is not valid";
		return false;
	}
	Phase = next;
	return true;
}

bool FContentManifest::Validate(std::string& error) const
{
	if (Version != OnlineProtocolVersion)
	{
		error = "unsupported content manifest version";
		return false;
	}
	if (!IsLowerHexDigest(RootDigest))
	{
		error = "content manifest root digest is invalid";
		return false;
	}
	uint64_t calculatedTotal = 0;
	uint32_t previousOrder = 0;
	bool first = true;
	if (Packages.size() > OnlineMaxContentPackages)
	{
		error = "content manifest has too many packages";
		return false;
	}
	for (const auto& package : Packages)
	{
		if (!IsSafePackageName(package.DisplayName) ||
			!IsLowerHexDigest(package.Digest) || !IsSafeExtension(package.FileExtension) ||
			package.LoadOrder == 0 ||
			package.Size > OnlineMaxContentPackageBytes ||
			(package.IsDirectoryBundle && package.FileExtension != ".pk3"))
		{
			error = "content manifest contains an invalid package";
			return false;
		}
		if (!first && package.LoadOrder <= previousOrder)
		{
			error = "content manifest load order is not strictly increasing";
			return false;
		}
		if (calculatedTotal > OnlineMaxContentBytes - package.Size)
		{
			error = "content manifest total size exceeds the online limit";
			return false;
		}
		calculatedTotal += package.Size;
		previousOrder = package.LoadOrder;
		first = false;
	}
	if (calculatedTotal != TotalBytes)
	{
		error = "content manifest total size does not match its packages";
		return false;
	}
	std::string calculatedRoot;
	if (!CalculateContentManifestDigest(*this, calculatedRoot, error))
		return false;
	if (calculatedRoot != RootDigest)
	{
		error = "content manifest root digest does not match its packages";
		return false;
	}
	return true;
}

bool FSessionManifest::Validate(std::string& error) const
{
	if (ProtocolVersion != OnlineProtocolVersion || EngineVersion.empty() || IWADDigest.empty() ||
		!IsSafeMetadata(EngineVersion, 128, true) || !IsSafeMetadata(Title, 96, true) ||
		!IsSafeMetadata(Region, 32, false) || !IsSafeMetadata(GameMode, 32, true) || MaxPlayers == 0 || MaxPlayers > 64)
	{
		error = "online session manifest has invalid metadata";
		return false;
	}
	if (!IsLowerHexDigest(IWADDigest))
	{
		error = "online session manifest has an invalid IWAD digest";
		return false;
	}
	return Content.Validate(error);
}

bool OnlineHashContentSource(const FPackageSource& source, FContentPackage& output, std::string& error)
{
	if (!EnsureSodium(error))
		return false;
	if (source.IsIWAD || source.IsEngineResource)
	{
		error = "IWAD and engine resources are not transferable session content";
		return false;
	}
	if (source.Path.empty())
	{
		error = "content source path is empty";
		return false;
	}

	std::error_code code;
	const auto status = std::filesystem::symlink_status(source.Path, code);
	if (code || std::filesystem::is_symlink(status))
	{
		error = "content source must be a real file or directory";
		return false;
	}

	FContentPackage package;
	package.DisplayName = source.DisplayName.empty() ? source.Path.filename().string() : source.DisplayName;
	package.LoadOrder = source.LoadOrder;
	package.FileExtension = std::filesystem::is_directory(status) ? ".pk3" : source.Path.extension().string();
	std::transform(package.FileExtension.begin(), package.FileExtension.end(), package.FileExtension.begin(), [](unsigned char character)
	{
		return static_cast<char>(std::tolower(character));
	});
	if (!IsSafeExtension(package.FileExtension))
	{
		error = "content source has no safe archive extension: " + source.Path.string();
		return false;
	}

	if (std::filesystem::is_regular_file(status))
	{
		const auto fileSize = std::filesystem::file_size(source.Path, code);
		if (code || fileSize > OnlineMaxContentPackageBytes ||
			(IsArchiveExtension(package.FileExtension) && fileSize > OnlineMaxArchiveContainerBytes))
		{
			error = "content source exceeds the allowed online size";
			return false;
		}
		if (!HashFilePath(source.Path, package.Digest, package.Size, error))
			return false;
		if (IsArchiveExtension(package.FileExtension) && !ValidateArchive(source.Path, error))
			return false;
	}
	else if (std::filesystem::is_directory(status))
	{
		package.IsDirectoryBundle = true;
		if (!HashDirectoryPath(source.Path, package.Digest, package.Size, error))
			return false;
	}
	else
	{
		error = "content source is neither a regular file nor a directory";
		return false;
	}

	output = std::move(package);
	return true;
}

bool OnlineBuildContentManifest(const std::vector<FPackageSource>& sources, FContentManifest& output, std::string& error)
{
	if (!EnsureSodium(error))
		return false;

	std::vector<FContentPackage> packages;
	packages.reserve(sources.size());
	for (const auto& source : sources)
	{
		// The game and engine packages remain local prerequisites. Every other
		// active source must be represented; silently omitting it would desync.
		if (source.IsIWAD || source.IsEngineResource)
			continue;
		std::error_code code;
		if (std::filesystem::is_directory(source.Path, code) && !code)
		{
			error = "directory-backed content must be canonicalized before it is advertised";
			return false;
		}
		FContentPackage package;
		if (!OnlineHashContentSource(source, package, error))
			return false;
		packages.push_back(std::move(package));
	}
	return FinalizeContentManifest(std::move(packages), output, error);
}

bool OnlineResolveContentPlan(const std::vector<FPackageSource>& sources, const std::filesystem::path& bundleDirectory,
	std::vector<FResolvedContentPackage>& resolved, FContentManifest& manifest, std::string& error)
{
	if (!EnsureSodium(error))
		return false;
	if (bundleDirectory.empty())
	{
		error = "online directory bundle location is empty";
		return false;
	}

	std::vector<FResolvedContentPackage> plan;
	std::vector<FContentPackage> packages;
	std::set<uint32_t> loadOrders;
	plan.reserve(sources.size());
	packages.reserve(sources.size());
	for (const auto& source : sources)
	{
		if (source.IsIWAD || source.IsEngineResource)
			continue;
		if (!loadOrders.insert(source.LoadOrder).second)
		{
			error = "online content sources contain duplicate load orders";
			return false;
		}
		std::error_code code;
		const bool isDirectory = std::filesystem::is_directory(source.Path, code) && !code;
		FResolvedContentPackage entry;
		if (isDirectory)
		{
			const auto bundle = bundleDirectory / ("mod-" + std::to_string(source.LoadOrder) + "-" + std::to_string(plan.size()) + ".pk3");
			if (!OnlineWriteDirectoryBundle(source, bundle, entry.Package, error))
				return false;
			entry.MountPath = bundle;
		}
		else
		{
			if (!OnlineHashContentSource(source, entry.Package, error))
				return false;
			entry.MountPath = source.Path;
		}
		packages.push_back(entry.Package);
		plan.push_back(std::move(entry));
	}
	if (!FinalizeContentManifest(std::move(packages), manifest, error))
		return false;
	std::sort(plan.begin(), plan.end(), [](const auto& left, const auto& right)
	{
		return left.Package.LoadOrder < right.Package.LoadOrder;
	});
	resolved = std::move(plan);
	return true;
}

bool OnlineWriteDirectoryBundle(const FPackageSource& source, const std::filesystem::path& outputPath, FContentPackage& output, std::string& error)
{
	if (!EnsureSodium(error))
		return false;
	std::error_code code;
	if (outputPath.empty() || outputPath.filename().empty() || outputPath.extension() != ".pk3")
	{
		error = "directory bundle destination must be a PK3 file";
		return false;
	}
	if (source.IsIWAD || source.IsEngineResource || !std::filesystem::is_directory(source.Path, code) || code ||
		std::filesystem::is_symlink(std::filesystem::symlink_status(source.Path, code)))
	{
		error = "directory bundle source must be a transferable directory";
		return false;
	}
	std::vector<std::filesystem::path> files;
	if (!CollectBundleFiles(source.Path, files, error))
		return false;
	uint64_t totalInputBytes = 0;
	for (const auto& file : files)
	{
		const auto size = std::filesystem::file_size(file, code);
		if (code || size > OnlineMaxContentPackageBytes || totalInputBytes > OnlineMaxArchiveContainerBytes - size)
		{
			error = "directory bundle source exceeds the online content limit";
			return false;
		}
		totalInputBytes += size;
	}
	const auto outputStatus = std::filesystem::symlink_status(outputPath, code);
	if (!code && std::filesystem::is_symlink(outputStatus))
	{
		error = "directory bundle destination cannot be a symlink";
		return false;
	}
	code.clear();

	mz_zip_archive archive = {};
	if (!mz_zip_writer_init_heap(&archive, 0, 0))
	{
		error = "could not initialize directory bundle archive";
		return false;
	}
	const MZ_TIME_T fixedTimestamp = 0;
	bool wroteAllFiles = true;
	for (const auto& file : files)
	{
		const auto relative = file.lexically_relative(source.Path).generic_string();
		const auto size = std::filesystem::file_size(file, code);
		if (code || relative.empty() || relative.compare(0, 3, "../") == 0 || !IsSafeArchiveMember(relative.c_str()) ||
			size > std::numeric_limits<mz_uint64>::max())
		{
			wroteAllFiles = false;
			break;
		}
		FZipReadContext context = { file };
		if (!mz_zip_writer_add_read_buf_callback(&archive, relative.c_str(), ReadZipSource, &context, static_cast<mz_uint64>(size),
			&fixedTimestamp, nullptr, 0, MZ_BEST_COMPRESSION, nullptr, 0, nullptr, 0))
		{
			wroteAllFiles = false;
			break;
		}
	}
	void* archiveBytes = nullptr;
	size_t archiveSize = 0;
	if (!wroteAllFiles || !mz_zip_writer_finalize_heap_archive(&archive, &archiveBytes, &archiveSize))
	{
		mz_zip_writer_end(&archive);
		error = "could not write directory bundle archive";
		return false;
	}
	mz_zip_writer_end(&archive);
	if (archiveSize > OnlineMaxArchiveContainerBytes)
	{
		mz_free(archiveBytes);
		error = "directory bundle archive exceeds the allowed container limit";
		return false;
	}
	std::filesystem::create_directories(outputPath.parent_path(), code);
	if (code)
	{
		mz_free(archiveBytes);
		error = "could not create directory bundle destination";
		return false;
	}
	const auto partial = outputPath.string() + ".part";
	const auto partialStatus = std::filesystem::symlink_status(partial, code);
	if (!code && std::filesystem::is_symlink(partialStatus))
	{
		mz_free(archiveBytes);
		error = "directory bundle partial destination cannot be a symlink";
		return false;
	}
	code.clear();
	{
		std::ofstream stream(partial, std::ios::binary | std::ios::trunc);
		if (!stream)
		{
			mz_free(archiveBytes);
			error = "could not open directory bundle destination";
			return false;
		}
		stream.write(static_cast<const char*>(archiveBytes), static_cast<std::streamsize>(archiveSize));
		if (!stream)
		{
			mz_free(archiveBytes);
			error = "could not write directory bundle destination";
			return false;
		}
	}
	mz_free(archiveBytes);
	std::filesystem::rename(partial, outputPath, code);
	if (code)
	{
		std::filesystem::remove(partial, code);
		error = "could not atomically commit directory bundle";
		return false;
	}
	FPackageSource bundleSource;
	bundleSource.Path = outputPath;
	bundleSource.DisplayName = source.DisplayName.empty() ? source.Path.filename().string() + ".pk3" : source.DisplayName + ".pk3";
	bundleSource.LoadOrder = source.LoadOrder;
	if (!OnlineHashContentSource(bundleSource, output, error))
		return false;
	output.IsDirectoryBundle = true;
	return true;
}

bool OnlineVerifyContentFile(const std::filesystem::path& path, const FContentPackage& expected, std::string& error)
{
	if (!EnsureSodium(error))
		return false;
	// A package is validated through a complete one-entry manifest so the
	// invariant that every manifest has a calculated root also applies to this
	// low-level file verifier. The caller supplies only a package, never a root.
	FContentManifest expectedManifest;
	if (!FinalizeContentManifest(std::vector<FContentPackage>{ expected }, expectedManifest, error))
		return false;
	std::error_code code;
	const auto status = std::filesystem::symlink_status(path, code);
	if (code || std::filesystem::is_symlink(status) || !std::filesystem::is_regular_file(status))
	{
		error = "received content is not a regular file";
		return false;
	}
	if (IsArchiveExtension(expected.FileExtension) && expected.Size > OnlineMaxArchiveContainerBytes)
	{
		error = "received content archive exceeds the allowed container limit";
		return false;
	}
	std::string digest;
	uint64_t size = 0;
	if (!HashFilePath(path, digest, size, error))
		return false;
	if (digest != expected.Digest || size != expected.Size)
	{
		error = "received content does not match the host manifest";
		return false;
	}
	if (IsArchiveExtension(expected.FileExtension) && !ValidateArchive(path, error))
		return false;
	return true;
}

bool OnlineIsSafeCacheKey(const std::string& key)
{
	return IsLowerHexDigest(key);
}

bool OnlineBuildContentApprovalToken(const std::string& contentRootDigest, const std::string& hostPeerId,
	std::string& token, std::string& error)
{
	token.clear();
	const auto isLowerHex = [](const std::string& value, size_t size)
	{
		return value.size() == size && std::all_of(value.begin(), value.end(), [](unsigned char character)
		{
			return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
		});
	};
	if (!IsLowerHexDigest(contentRootDigest) || !isLowerHex(hostPeerId, 32))
	{
		error = "online content approval identity is invalid";
		return false;
	}
	token = contentRootDigest + ":" + hostPeerId;
	return true;
}

FOnlineContentCache::FOnlineContentCache(std::filesystem::path root) : Root(std::move(root))
{
}

bool FOnlineContentCache::ValidateRoot(std::string& error) const
{
	if (Root.empty())
	{
		error = "online content cache root is invalid";
		return false;
	}
	std::error_code code;
	const auto status = std::filesystem::symlink_status(Root, code);
	if (code == std::errc::no_such_file_or_directory || status.type() == std::filesystem::file_type::not_found)
	{
		error.clear();
		return true;
	}
	if (code || std::filesystem::is_symlink(status) || !std::filesystem::is_directory(status))
	{
		error = "online content cache root is not a real directory";
		return false;
	}
	error.clear();
	return true;
}

std::filesystem::path FOnlineContentCache::FinalPath(const FContentPackage& package) const
{
	if (!OnlineIsSafeCacheKey(package.Digest) || !IsSafeExtension(package.FileExtension))
		return {};
	return Root / package.Digest.substr(0, 2) / (package.Digest + package.FileExtension);
}

std::filesystem::path FOnlineContentCache::PartialPath(const FContentPackage& package) const
{
	const auto final = FinalPath(package);
	return final.empty() ? std::filesystem::path() : std::filesystem::path(final.string() + ".part");
}

bool FOnlineContentCache::PreparePackageDirectory(const FContentPackage& package, std::string& error) const
{
	const auto final = FinalPath(package);
	if (final.empty() || !ValidateRoot(error))
		return false;
	const auto bucket = final.parent_path();
	std::error_code code;
	std::filesystem::create_directories(bucket, code);
	if (code)
	{
		error = "could not create the online content cache directory";
		return false;
	}
	if (!ValidateRoot(error))
		return false;
	const auto bucketStatus = std::filesystem::symlink_status(bucket, code);
	if (code || std::filesystem::is_symlink(bucketStatus) || !std::filesystem::is_directory(bucketStatus))
	{
		error = "online content cache bucket is not a real directory";
		return false;
	}
	error.clear();
	return true;
}

bool FOnlineContentCache::HasVerified(const FContentPackage& package, std::string& error) const
{
	if (!OnlineIsSafeCacheKey(package.Digest) || !IsSafeExtension(package.FileExtension))
	{
		error = "unsafe content cache key";
		return false;
	}
	if (!ValidateRoot(error))
		return false;
	std::error_code code;
	const auto final = FinalPath(package);
	const auto status = std::filesystem::symlink_status(final, code);
	if (final.empty())
	{
		error = "content cache final path is invalid";
		return false;
	}
	if (code == std::errc::no_such_file_or_directory || status.type() == std::filesystem::file_type::not_found)
	{
		error.clear();
		return false;
	}
	if (code || std::filesystem::is_symlink(status) || !std::filesystem::is_regular_file(status))
	{
		error = "content cache final is not a regular file";
		return false;
	}
	if (!OnlineVerifyContentFile(final, package, error))
	{
		// Cache entries are disposable. Purging a regular entry that no longer
		// matches its immutable digest lets a later resume obtain clean bytes and
		// keeps a bad cache leaf from consuming quota forever.
		std::error_code removeError;
		std::filesystem::remove(final, removeError);
		if (removeError)
			error += "; could not discard the corrupt content cache entry";
		else
			error.clear();
		return false;
	}
	// The filesystem timestamp is the LRU touch marker. Failure to update it
	// is non-fatal: the verified entry is still safe to use, it may simply be
	// evicted sooner than a recently touched entry on unusual filesystems.
	std::filesystem::last_write_time(final, std::filesystem::file_time_type::clock::now(), code);
	error.clear();
	return true;
}

bool FOnlineContentCache::PartialSize(const FContentPackage& package, uint64_t& size, std::string& error) const
{
	size = 0;
	if (!OnlineIsSafeCacheKey(package.Digest) || !IsSafeExtension(package.FileExtension))
	{
		error = "unsafe content cache key";
		return false;
	}
	if (!ValidateRoot(error))
		return false;
	const auto partial = PartialPath(package);
	if (partial.empty())
	{
		error = "content download partial path is invalid";
		return false;
	}
	std::error_code code;
	const auto status = std::filesystem::symlink_status(partial, code);
	if (code == std::errc::no_such_file_or_directory || status.type() == std::filesystem::file_type::not_found)
	{
		error.clear();
		return true;
	}
	if (code || std::filesystem::is_symlink(status) || !std::filesystem::is_regular_file(status))
	{
		error = "content download partial is not a regular file";
		return false;
	}
	size = std::filesystem::file_size(partial, code);
	if (code || size > package.Size)
	{
		error = "content download partial has an invalid size";
		return false;
	}
	error.clear();
	return true;
}

bool FOnlineContentCache::EnsureSpace(uint64_t incomingBytes, uint64_t quotaBytes, const std::vector<FContentPackage>& preserve, std::string& error) const
{
	if (quotaBytes == 0 || incomingBytes > quotaBytes)
	{
		error = "host-provided content exceeds the configured online cache quota";
		return false;
	}

	std::set<std::string> protectedDigests;
	for (const auto& package : preserve)
	{
		if (!OnlineIsSafeCacheKey(package.Digest) || !IsSafeExtension(package.FileExtension))
		{
			error = "online session contains an unsafe cache key";
			return false;
		}
		protectedDigests.insert(package.Digest);
	}
	if (!ValidateRoot(error))
		return false;

	struct FCacheEntry
	{
		std::filesystem::path Path;
		std::filesystem::file_time_type LastUse;
		uint64_t Size = 0;
		std::string Digest;
	};
	std::vector<FCacheEntry> entries;
	uint64_t usedBytes = 0;
	std::error_code code;
	if (!std::filesystem::exists(Root, code))
	{
		if (code && code != std::errc::no_such_file_or_directory)
		{
			error = "could not inspect the online content cache";
			return false;
		}
		return true;
	}
	if (code || !std::filesystem::is_directory(Root, code) || code)
	{
		error = "online content cache path is not a directory";
		return false;
	}

	for (std::filesystem::directory_iterator bucket(Root, std::filesystem::directory_options::none, code), end;
		bucket != end; bucket.increment(code))
	{
		if (code)
		{
			error = "could not enumerate the online content cache";
			return false;
		}
		const auto bucketName = bucket->path().filename().string();
		const auto bucketStatus = bucket->symlink_status(code);
		if (code)
		{
			error = "could not inspect an online content cache entry";
			return false;
		}
		if (bucketName.size() != 2 || !std::filesystem::is_directory(bucketStatus) || std::filesystem::is_symlink(bucketStatus) ||
			!std::all_of(bucketName.begin(), bucketName.end(), [](unsigned char value) { return std::isxdigit(value) && !std::isupper(value); }))
			continue;

		for (std::filesystem::directory_iterator file(bucket->path(), std::filesystem::directory_options::none, code), fileEnd;
			file != fileEnd; file.increment(code))
		{
			if (code)
			{
				error = "could not enumerate an online content cache bucket";
				return false;
			}
			const auto status = file->symlink_status(code);
			if (code || std::filesystem::is_symlink(status) || !std::filesystem::is_regular_file(status))
			{
				code.clear();
				continue;
			}
			std::string name = file->path().filename().string();
			if (name.size() > 5 && name.compare(name.size() - 5, 5, ".part") == 0)
				name.resize(name.size() - 5);
			if (name.size() <= OnlineDigestBytes * 2)
				continue;
			const auto digest = name.substr(0, OnlineDigestBytes * 2);
			const auto extension = name.substr(OnlineDigestBytes * 2);
			if (!OnlineIsSafeCacheKey(digest) || digest.compare(0, bucketName.size(), bucketName) != 0 || !IsSafeExtension(extension))
				continue;
			const auto size = std::filesystem::file_size(file->path(), code);
			const auto lastUse = std::filesystem::last_write_time(file->path(), code);
			if (code || size > std::numeric_limits<uint64_t>::max() - usedBytes)
			{
				error = "could not measure an online content cache entry";
				return false;
			}
			usedBytes += size;
			entries.push_back({ file->path(), lastUse, size, digest });
		}
	}

	if (usedBytes <= quotaBytes - incomingBytes)
		return true;
	std::sort(entries.begin(), entries.end(), [](const auto& left, const auto& right)
	{
		return left.LastUse < right.LastUse;
	});
	for (const auto& entry : entries)
	{
		if (protectedDigests.find(entry.Digest) != protectedDigests.end())
			continue;
		std::filesystem::remove(entry.Path, code);
		if (code)
		{
			error = "could not evict an old online content cache entry";
			return false;
		}
		usedBytes -= entry.Size;
		if (usedBytes <= quotaBytes - incomingBytes)
			return true;
	}

	error = "not enough room remains in the online content cache; increase -online-cache-mb or clear old cached mods";
	return false;
}

bool FOnlineContentCache::CommitPartial(const FContentPackage& package, std::string& error) const
{
	if (!OnlineIsSafeCacheKey(package.Digest) || !IsSafeExtension(package.FileExtension))
	{
		error = "unsafe content cache key";
		return false;
	}
	if (!PreparePackageDirectory(package, error))
		return false;
	const auto partial = PartialPath(package);
	const auto final = FinalPath(package);
	std::error_code code;
	const auto partialStatus = std::filesystem::symlink_status(partial, code);
	if (partial.empty() || final.empty() || code || std::filesystem::is_symlink(partialStatus) || !std::filesystem::is_regular_file(partialStatus))
	{
		error = "content download partial file is missing";
		return false;
	}
	if (!OnlineVerifyContentFile(partial, package, error))
	{
		// A complete-but-corrupt partial would otherwise cause every later
		// resume to skip straight to the same failed verification. Delete only
		// this checked cache leaf so the next explicit join starts cleanly.
		std::error_code removeError;
		std::filesystem::remove(partial, removeError);
		if (removeError)
			error += "; could not discard the corrupt download partial";
		return false;
	}
	const auto finalStatus = std::filesystem::symlink_status(final, code);
	// A missing destination is the normal first-download case. libstdc++
	// reports it through error_code for symlink_status(), whereas other
	// implementations return file_type::not_found without an error.
	if (code && code != std::errc::no_such_file_or_directory)
	{
		error = "could not inspect content cache destination";
		return false;
	}
	code.clear();
	if (std::filesystem::exists(finalStatus))
	{
		if (!std::filesystem::is_symlink(finalStatus) && OnlineVerifyContentFile(final, package, error))
		{
			std::filesystem::remove(partial, code);
			if (code)
			{
				error = "could not discard redundant content download partial";
				return false;
			}
			return true;
		}
		std::filesystem::remove(final, code);
		if (code)
		{
			error = "could not replace invalid content cache destination";
			return false;
		}
	}
	error.clear();
	std::filesystem::rename(partial, final, code);
	if (code)
	{
		error = "could not atomically commit content download";
		return false;
	}
	std::filesystem::last_write_time(final, std::filesystem::file_time_type::clock::now(), code);
	return true;
}

bool OnlineResolveCachedContent(const std::filesystem::path& cacheRoot, const FContentManifest& manifest,
	std::vector<std::filesystem::path>& paths, bool& complete, std::string& error)
{
	paths.clear();
	complete = false;
	if (!manifest.Validate(error))
		return false;
	FOnlineContentCache cache(cacheRoot);
	paths.reserve(manifest.Packages.size());
	for (const auto& package : manifest.Packages)
	{
		std::string cacheError;
		if (!cache.HasVerified(package, cacheError))
		{
			paths.clear();
			if (!cacheError.empty())
			{
				error = cacheError;
				return false;
			}
			error.clear();
			return true;
		}
		paths.push_back(cache.FinalPath(package));
	}
	complete = true;
	error.clear();
	return true;
}

bool FEncryptedDatagramCodec::Initialize(const std::array<uint8_t, OnlineKeyBytes>& key, uint32_t direction, std::string& error)
{
	if (!EnsureSodium(error))
		return false;
	// ChaCha20-Poly1305 IETF nonces are exactly 96 bits. Keeping the full
	// 32-bit epoch and 64-bit sequence consumes that space, so direction is
	// domain-separated into the key instead of overflowing the nonce. This
	// also gives independently routed peers separate AEAD key/nonce spaces.
	crypto_generichash_state keyDerivation = {};
	if (crypto_generichash_init(&keyDerivation, key.data(), key.size(), Key.size()) != 0)
	{
		error = "could not derive online datagram key";
		return false;
	}
	static constexpr char Domain[] = "biaseddoom-online-datagram-key-v1";
	if (crypto_generichash_update(&keyDerivation, reinterpret_cast<const unsigned char*>(Domain), sizeof(Domain) - 1) != 0)
	{
		error = "could not derive online datagram key";
		return false;
	}
	std::array<uint8_t, 4> encodedDirection = {};
	for (size_t index = 0; index < encodedDirection.size(); ++index)
		encodedDirection[index] = static_cast<uint8_t>(direction >> (index * 8));
	if (crypto_generichash_update(&keyDerivation, encodedDirection.data(), encodedDirection.size()) != 0 ||
		crypto_generichash_final(&keyDerivation, Key.data(), Key.size()) != 0)
	{
		error = "could not derive online datagram key";
		return false;
	}
	Direction = direction;
	NextSequence = 0;
	LargestReceivedSequence = 0;
	ReceivedWindow = 0;
	HasReceivedPacket = false;
	Initialized = true;
	return true;
}

bool FEncryptedDatagramCodec::Seal(uint32_t epoch, const uint8_t* bytes, size_t size, FEncryptedDatagram& output, std::string& error)
{
	if (!Initialized || (size > 0 && bytes == nullptr))
	{
		error = "encrypted datagram codec is not initialized";
		return false;
	}
	if (size > OnlineMaxDatagramBytes - crypto_aead_chacha20poly1305_ietf_ABYTES)
	{
		error = "online datagram exceeds the transport limit";
		return false;
	}
	if (NextSequence == std::numeric_limits<uint64_t>::max())
	{
		error = "encrypted datagram sequence exhausted";
		return false;
	}
	const auto sequence = NextSequence++;
	const auto nonce = DatagramNonce(epoch, sequence);
	const auto associatedData = DatagramAssociatedData(Direction, epoch, sequence);
	FEncryptedDatagram packet;
	packet.Epoch = epoch;
	packet.Sequence = sequence;
	packet.Ciphertext.resize(size + crypto_aead_chacha20poly1305_ietf_ABYTES);
	unsigned long long outputSize = 0;
	if (crypto_aead_chacha20poly1305_ietf_encrypt(packet.Ciphertext.data(), &outputSize, bytes,
		static_cast<unsigned long long>(size), associatedData.data(), associatedData.size(), nullptr,
		nonce.data(), Key.data()) != 0)
	{
		error = "could not encrypt online datagram";
		return false;
	}
	packet.Ciphertext.resize(static_cast<size_t>(outputSize));
	output = std::move(packet);
	return true;
}

bool FEncryptedDatagramCodec::Open(const FEncryptedDatagram& input, std::vector<uint8_t>& output, std::string& error)
{
	if (!Initialized || input.Ciphertext.size() < crypto_aead_chacha20poly1305_ietf_ABYTES || input.Ciphertext.size() > OnlineMaxDatagramBytes)
	{
		error = "encrypted datagram is invalid";
		return false;
	}
	if (HasReceivedPacket && input.Sequence <= LargestReceivedSequence)
	{
		const auto offset = LargestReceivedSequence - input.Sequence;
		if (offset >= 64 || (ReceivedWindow & (uint64_t(1) << offset)) != 0)
		{
			error = "replayed online datagram";
			return false;
		}
	}
	const auto nonce = DatagramNonce(input.Epoch, input.Sequence);
	const auto associatedData = DatagramAssociatedData(Direction, input.Epoch, input.Sequence);
	std::vector<uint8_t> plaintext(input.Ciphertext.size() - crypto_aead_chacha20poly1305_ietf_ABYTES);
	unsigned long long plaintextSize = 0;
	if (crypto_aead_chacha20poly1305_ietf_decrypt(plaintext.data(), &plaintextSize, nullptr,
		input.Ciphertext.data(), static_cast<unsigned long long>(input.Ciphertext.size()), associatedData.data(),
		associatedData.size(), nonce.data(), Key.data()) != 0)
	{
		error = "online datagram authentication failed";
		return false;
	}
	plaintext.resize(static_cast<size_t>(plaintextSize));
	if (!HasReceivedPacket)
	{
		LargestReceivedSequence = input.Sequence;
		ReceivedWindow = 1;
		HasReceivedPacket = true;
	}
	else if (input.Sequence > LargestReceivedSequence)
	{
		const auto delta = input.Sequence - LargestReceivedSequence;
		ReceivedWindow = delta >= 64 ? 1 : (ReceivedWindow << delta) | 1;
		LargestReceivedSequence = input.Sequence;
	}
	else
	{
		ReceivedWindow |= uint64_t(1) << (LargestReceivedSequence - input.Sequence);
	}
	output = std::move(plaintext);
	return true;
}

} // namespace BdOnline
