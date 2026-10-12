#include "online_service.h"

// The Windows SDK may have entered through the precompiled header. Its
// function-like min/max macros corrupt RapidJSON and numeric_limits<T>::max().
// Clear any inherited definitions now, then prevent libcurl from reintroducing
// them while it includes its Windows networking headers.
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif

#include <curl/curl.h>
#include <sodium.h>

// curl may include Windows headers after the first cleanup on some package
// variants. Do the final cleanup immediately before RapidJSON is parsed.
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

// The engine's allocator can return virtual addresses outside RapidJSON's
// optional 48-bit pointer-compression range. The control-plane DOM is wholly
// private to this translation unit, so favor full pointers over a small DOM
// saving and keep parsing correct on modern wide-address platforms.
#ifndef RAPIDJSON_48BITPOINTER_OPTIMIZATION
#define RAPIDJSON_48BITPOINTER_OPTIMIZATION 0
#endif
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <limits>
#include <mutex>
#include <sstream>

namespace BdOnline
{
namespace
{

std::once_flag CurlInitialization;
bool CurlReady = false;
constexpr size_t MaxControlResponseBytes = 1024 * 1024;
constexpr size_t MaxOnlinePasswordBytes = 255;

void InitializeCurl()
{
	CurlReady = curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK;
}

size_t WriteResponse(char* data, size_t size, size_t count, void* opaque)
{
	auto* response = static_cast<std::string*>(opaque);
	if (size != 0 && (count > std::numeric_limits<size_t>::max() / size ||
		response->size() > MaxControlResponseBytes - size * count))
		return 0;
	response->append(data, size * count);
	return size * count;
}

bool IsLocalControlHost(const char* host)
{
	return host != nullptr && (!std::strcmp(host, "localhost") || !std::strcmp(host, "127.0.0.1") || !std::strcmp(host, "::1"));
}

bool IsSafePathComponent(const std::string& value, size_t maximumLength)
{
	if (value.empty() || value.size() > maximumLength)
		return false;
	return std::all_of(value.begin(), value.end(), [](unsigned char character)
	{
		return std::isalnum(character) != 0 || character == '-' || character == '_';
	});
}

bool IsLowerHex(const std::string& value, size_t expectedLength)
{
	if (value.size() != expectedLength)
		return false;
	return std::all_of(value.begin(), value.end(), [](unsigned char character)
	{
		return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
	});
}

// JSON control responses never permit embedded NUL bytes. Reject those before
// parsing; every caller then validates the exact response schema before it
// accepts credentials or session data.
bool ParseJsonObject(const std::string& response, rapidjson::Document& document)
{
	if (response.empty() || response.find('\0') != std::string::npos)
		return false;
	document.Parse(response.c_str(), response.size());
	return !document.HasParseError() && document.IsObject();
}

// Passwords are never serialized into manifests, tickets, listings, or the
// relay. The directory compares this fixed-size, domain-separated proof over
// the authenticated TLS control channel before it releases protected metadata.
bool BuildPasswordProof(const std::string& password, std::string& proof, std::string& error)
{
	if (password.empty() || password.size() > MaxOnlinePasswordBytes)
	{
		error = "online session passwords must contain 1 to 255 bytes";
		return false;
	}
	if (sodium_init() < 0)
	{
		error = "libsodium could not initialize";
		return false;
	}
	static constexpr char Domain[] = "BiasedDoom online password proof v1";
	std::array<unsigned char, OnlineDigestBytes> digest = {};
	crypto_generichash_state state = {};
	if (crypto_generichash_init(&state, nullptr, 0, digest.size()) != 0 ||
		crypto_generichash_update(&state, reinterpret_cast<const unsigned char*>(Domain), sizeof(Domain) - 1) != 0 ||
		crypto_generichash_update(&state, reinterpret_cast<const unsigned char*>(password.data()), password.size()) != 0 ||
		crypto_generichash_final(&state, digest.data(), digest.size()) != 0)
	{
		error = "could not derive the online password proof";
		return false;
	}
	std::array<char, OnlineDigestBytes * 2 + 1> hexadecimal = {};
	sodium_bin2hex(hexadecimal.data(), hexadecimal.size(), digest.data(), digest.size());
	proof.assign(hexadecimal.data());
	return true;
}

bool IsSecureControlUrl(const std::string& base)
{
	CURLU* url = curl_url();
	if (url == nullptr)
		return false;
	if (curl_url_set(url, CURLUPART_URL, base.c_str(), 0) != CURLUE_OK)
	{
		curl_url_cleanup(url);
		return false;
	}
	char* scheme = nullptr;
	char* host = nullptr;
	char* user = nullptr;
	char* query = nullptr;
	char* fragment = nullptr;
	char* path = nullptr;
	const bool valid = curl_url_get(url, CURLUPART_SCHEME, &scheme, 0) == CURLUE_OK &&
		curl_url_get(url, CURLUPART_HOST, &host, 0) == CURLUE_OK &&
		curl_url_get(url, CURLUPART_USER, &user, 0) == CURLUE_NO_USER &&
		curl_url_get(url, CURLUPART_QUERY, &query, 0) == CURLUE_NO_QUERY &&
		curl_url_get(url, CURLUPART_FRAGMENT, &fragment, 0) == CURLUE_NO_FRAGMENT &&
		curl_url_get(url, CURLUPART_PATH, &path, 0) == CURLUE_OK && (path[0] == '\0' || !std::strcmp(path, "/")) &&
		(!std::strcmp(scheme, "https") || (!std::strcmp(scheme, "http") && IsLocalControlHost(host)));
	curl_free(scheme);
	curl_free(host);
	curl_free(user);
	curl_free(query);
	curl_free(fragment);
	curl_free(path);
	curl_url_cleanup(url);
	return valid;
}

std::string JoinUrl(const std::string& base, const std::string& path)
{
	if (base.empty())
		return {};
	const auto baseEnd = base.find_last_not_of('/');
	if (baseEnd == std::string::npos)
		return {};
	return base.substr(0, baseEnd + 1) + path;
}

std::string EncodeBase64(const uint8_t* bytes, size_t size)
{
	std::string output(sodium_base64_ENCODED_LEN(size, sodium_base64_VARIANT_ORIGINAL), '\0');
	sodium_bin2base64(output.data(), output.size(), bytes, size, sodium_base64_VARIANT_ORIGINAL);
	output.resize(std::strlen(output.c_str()));
	return output;
}

bool DecodeBase64(const std::string& encoded, uint8_t* output, size_t outputSize)
{
	size_t size = 0;
	return sodium_base642bin(output, outputSize, encoded.c_str(), encoded.size(), nullptr, &size, nullptr,
		sodium_base64_VARIANT_ORIGINAL) == 0 && size == outputSize;
}

std::string RequestTimestamp()
{
	return std::to_string(std::chrono::duration_cast<std::chrono::seconds>(
		std::chrono::system_clock::now().time_since_epoch()).count());
}

void WriteContentManifest(rapidjson::Writer<rapidjson::StringBuffer>& writer, const FContentManifest& manifest)
{
	writer.Key("version"); writer.Uint(manifest.Version);
	writer.Key("root_digest"); writer.String(manifest.RootDigest.c_str());
	writer.Key("total_bytes"); writer.Uint64(manifest.TotalBytes);
	writer.Key("packages"); writer.StartArray();
	for (const auto& package : manifest.Packages)
	{
		writer.StartObject();
		writer.Key("name"); writer.String(package.DisplayName.c_str());
		writer.Key("digest"); writer.String(package.Digest.c_str());
		writer.Key("extension"); writer.String(package.FileExtension.c_str());
		writer.Key("size"); writer.Uint64(package.Size);
		writer.Key("load_order"); writer.Uint(package.LoadOrder);
		writer.Key("directory_bundle"); writer.Bool(package.IsDirectoryBundle);
		writer.EndObject();
	}
	writer.EndArray();
}

void WriteSessionManifest(rapidjson::Writer<rapidjson::StringBuffer>& writer, const FSessionManifest& manifest)
{
	writer.Key("protocol_version"); writer.Uint(manifest.ProtocolVersion);
	writer.Key("engine_version"); writer.String(manifest.EngineVersion.c_str());
	writer.Key("iwad_digest"); writer.String(manifest.IWADDigest.c_str());
	writer.Key("title"); writer.String(manifest.Title.c_str());
	writer.Key("region"); writer.String(manifest.Region.c_str());
	writer.Key("mode"); writer.String(manifest.GameMode.c_str());
	writer.Key("visibility"); writer.String(manifest.Visibility == ESessionVisibility::Public ? "public" : "private");
	writer.Key("max_players"); writer.Uint(manifest.MaxPlayers);
	writer.Key("password_required"); writer.Bool(manifest.PasswordRequired);
	writer.Key("allow_join_in_progress"); writer.Bool(manifest.AllowJoinInProgress);
	writer.Key("content"); writer.StartObject();
	WriteContentManifest(writer, manifest.Content);
	writer.EndObject();
}

std::string SerializeCreateSession(const FSessionManifest& manifest, const std::string& passwordProof)
{
	rapidjson::StringBuffer buffer;
	rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
	writer.StartObject();
	WriteSessionManifest(writer, manifest);
	if (!passwordProof.empty())
	{
		writer.Key("password_proof"); writer.String(passwordProof.c_str());
	}
	writer.EndObject();
	return buffer.GetString();
}

bool ReadString(const rapidjson::Value& value, const char* name, std::string& output)
{
	const auto member = value.FindMember(name);
	if (member == value.MemberEnd() || !member->value.IsString())
		return false;
	output.assign(member->value.GetString(), member->value.GetStringLength());
	return true;
}

bool ReadOptionalString(const rapidjson::Value& value, const char* name, std::string& output)
{
	const auto member = value.FindMember(name);
	if (member == value.MemberEnd())
	{
		output.clear();
		return true;
	}
	if (!member->value.IsString())
		return false;
	output.assign(member->value.GetString(), member->value.GetStringLength());
	return true;
}

bool ReadUnsigned(const rapidjson::Value& value, const char* name, uint64_t& output)
{
	const auto member = value.FindMember(name);
	if (member == value.MemberEnd() || !member->value.IsUint64())
		return false;
	output = member->value.GetUint64();
	return true;
}

bool ReadBoolean(const rapidjson::Value& value, const char* name, bool& output)
{
	const auto member = value.FindMember(name);
	if (member == value.MemberEnd() || !member->value.IsBool())
		return false;
	output = member->value.GetBool();
	return true;
}

bool ParseContentManifest(const rapidjson::Value& value, FContentManifest& output, std::string& error)
{
	if (!value.IsObject())
	{
		error = "online service returned an invalid content manifest";
		return false;
	}
	FContentManifest manifest;
	uint64_t version = 0;
	if (!ReadUnsigned(value, "version", version) || version > std::numeric_limits<uint32_t>::max() ||
		!ReadString(value, "root_digest", manifest.RootDigest) || !ReadUnsigned(value, "total_bytes", manifest.TotalBytes))
	{
		error = "online service content manifest is incomplete";
		return false;
	}
	manifest.Version = static_cast<uint32_t>(version);
	const auto packages = value.FindMember("packages");
	if (packages == value.MemberEnd() || !packages->value.IsArray())
	{
		error = "online service content manifest package list is missing";
		return false;
	}
	for (const auto& item : packages->value.GetArray())
	{
		FContentPackage package;
		uint64_t order = 0;
		if (!item.IsObject() || !ReadString(item, "name", package.DisplayName) || !ReadString(item, "digest", package.Digest) ||
			!ReadString(item, "extension", package.FileExtension) || !ReadUnsigned(item, "size", package.Size) ||
			!ReadUnsigned(item, "load_order", order) || order > std::numeric_limits<uint32_t>::max() ||
			!ReadBoolean(item, "directory_bundle", package.IsDirectoryBundle))
		{
			error = "online service content package is invalid";
			return false;
		}
		package.LoadOrder = static_cast<uint32_t>(order);
		manifest.Packages.push_back(std::move(package));
	}
	if (!manifest.Validate(error))
		return false;
	output = std::move(manifest);
	return true;
}

bool ParseSessionManifest(const rapidjson::Value& value, FSessionManifest& output, std::string& error)
{
	if (!value.IsObject())
	{
		error = "online service returned an invalid session manifest";
		return false;
	}
	FSessionManifest manifest;
	uint64_t protocol = 0;
	uint64_t maxPlayers = 0;
	std::string visibility;
	if (!ReadUnsigned(value, "protocol_version", protocol) || protocol > std::numeric_limits<uint32_t>::max() ||
		!ReadString(value, "engine_version", manifest.EngineVersion) || !ReadString(value, "iwad_digest", manifest.IWADDigest) ||
		!ReadString(value, "title", manifest.Title) || !ReadString(value, "region", manifest.Region) || !ReadString(value, "mode", manifest.GameMode) ||
		!ReadString(value, "visibility", visibility) || !ReadUnsigned(value, "max_players", maxPlayers) || maxPlayers > 64 ||
		!ReadBoolean(value, "password_required", manifest.PasswordRequired) ||
		!ReadBoolean(value, "allow_join_in_progress", manifest.AllowJoinInProgress))
	{
		error = "online service session manifest is incomplete";
		return false;
	}
	manifest.ProtocolVersion = static_cast<uint32_t>(protocol);
	manifest.MaxPlayers = static_cast<uint32_t>(maxPlayers);
	if (visibility == "public") manifest.Visibility = ESessionVisibility::Public;
	else if (visibility != "private")
	{
		error = "online service session visibility is invalid";
		return false;
	}
	const auto content = value.FindMember("content");
	if (content == value.MemberEnd() || !ParseContentManifest(content->value, manifest.Content, error) || !manifest.Validate(error))
		return false;
	output = std::move(manifest);
	return true;
}

bool ParseError(const std::string& response, std::string& error)
{
	rapidjson::Document document;
	std::string serviceError;
	if (ParseJsonObject(response, document) && ReadString(document, "error", serviceError))
	{
		error = serviceError;
		return true;
	}
	return false;
}

} // namespace

bool FOnlineDeviceIdentity::Generate(std::string& error)
{
	if (sodium_init() < 0)
	{
		error = "libsodium could not initialize";
		return false;
	}
	std::array<uint8_t, crypto_sign_PUBLICKEYBYTES> publicKey = {};
	std::array<uint8_t, crypto_sign_SECRETKEYBYTES> privateKey = {};
	if (crypto_sign_keypair(publicKey.data(), privateKey.data()) != 0)
	{
		error = "could not generate online device identity";
		return false;
	}
	PublicKey = publicKey;
	PrivateKey = privateKey;
	Valid = true;
	return true;
}

FOnlineDeviceIdentity::~FOnlineDeviceIdentity()
{
	sodium_memzero(PublicKey.data(), PublicKey.size());
	sodium_memzero(PrivateKey.data(), PrivateKey.size());
	Valid = false;
}

bool FOnlineDeviceIdentity::ImportPrivateKeyBase64(const std::string& encoded, std::string& error)
{
	if (sodium_init() < 0)
	{
		error = "libsodium could not initialize";
		return false;
	}
	std::array<uint8_t, crypto_sign_SECRETKEYBYTES> privateKey = {};
	std::array<uint8_t, crypto_sign_PUBLICKEYBYTES> publicKey = {};
	if (!DecodeBase64(encoded, privateKey.data(), privateKey.size()) ||
		crypto_sign_ed25519_sk_to_pk(publicKey.data(), privateKey.data()) != 0)
	{
		error = "online device private key is invalid";
		return false;
	}
	PublicKey = publicKey;
	PrivateKey = privateKey;
	Valid = true;
	return true;
}

std::string FOnlineDeviceIdentity::ExportPrivateKeyBase64() const
{
	return Valid ? EncodeBase64(PrivateKey.data(), PrivateKey.size()) : std::string();
}

std::string FOnlineDeviceIdentity::PublicKeyBase64() const
{
	return Valid ? EncodeBase64(PublicKey.data(), PublicKey.size()) : std::string();
}

bool FOnlineDeviceIdentity::SignDetached(const uint8_t* message, size_t size, std::vector<uint8_t>& signature, std::string& error) const
{
	signature.clear();
	if (!Valid || (message == nullptr && size != 0))
	{
		error = "online device identity is unavailable";
		return false;
	}
	std::array<uint8_t, crypto_sign_BYTES> rawSignature = {};
	unsigned long long signatureSize = 0;
	if (crypto_sign_detached(rawSignature.data(), &signatureSize, message, size, PrivateKey.data()) != 0 ||
		signatureSize != rawSignature.size())
	{
		error = "could not sign online protocol record";
		return false;
	}
	signature.assign(rawSignature.begin(), rawSignature.end());
	return true;
}

bool FOnlineDeviceIdentity::VerifyDetached(const std::string& publicKeyBase64, const uint8_t* message, size_t size,
	const uint8_t* signature, size_t signatureSize, std::string& error)
{
	if (sodium_init() < 0 || (message == nullptr && size != 0) || signature == nullptr || signatureSize != crypto_sign_BYTES)
	{
		error = "online direct identity proof is invalid";
		return false;
	}
	std::array<uint8_t, crypto_sign_PUBLICKEYBYTES> publicKey = {};
	if (!DecodeBase64(publicKeyBase64, publicKey.data(), publicKey.size()) ||
		crypto_sign_verify_detached(signature, message, size, publicKey.data()) != 0)
	{
		error = "online direct identity proof did not verify";
		return false;
	}
	return true;
}

bool FOnlineDeviceIdentity::SignControlRequest(const std::string& method, const std::string& path, const std::string& timestamp,
	const std::string& nonce, const std::string& body, std::string& signature, std::string& error) const
{
	if (!Valid)
	{
		error = "online device identity is unavailable";
		return false;
	}
	std::array<uint8_t, crypto_hash_sha256_BYTES> bodyHash = {};
	crypto_hash_sha256(bodyHash.data(), reinterpret_cast<const unsigned char*>(body.data()), body.size());
	const auto payload = method + "\n" + path + "\n" + timestamp + "\n" + nonce + "\n" +
		EncodeBase64(bodyHash.data(), bodyHash.size());
	std::vector<uint8_t> rawSignature;
	if (!SignDetached(reinterpret_cast<const uint8_t*>(payload.data()), payload.size(), rawSignature, error))
		return false;
	signature = EncodeBase64(rawSignature.data(), rawSignature.size());
	return true;
}

FOnlineServiceClient::FOnlineServiceClient(FOnlineServiceConfig config) : Config(std::move(config))
{
}

bool FOnlineServiceClient::Request(const char* method, const std::string& path, const std::string& body, long& status, std::string& response, std::string& error) const
{
	std::call_once(CurlInitialization, InitializeCurl);
	if (!CurlReady)
	{
		error = "libcurl could not initialize";
		return false;
	}
	const auto url = JoinUrl(Config.BaseUrl, path);
	if (url.empty() || !IsSecureControlUrl(Config.BaseUrl))
	{
		error = "online service URL must use HTTPS (or loopback HTTP for local development)";
		return false;
	}
	if (body.size() > static_cast<size_t>(std::numeric_limits<long>::max()) || body.size() > 512 * 1024)
	{
		error = "online service request exceeds the control-plane size limit";
		return false;
	}

	CURL* handle = curl_easy_init();
	if (handle == nullptr)
	{
		error = "could not create online service request";
		return false;
	}
	curl_slist* headers = nullptr;
	headers = curl_slist_append(headers, "Content-Type: application/json");
	if (!Config.AccessToken.empty())
	{
		const auto authorization = "Authorization: Bearer " + Config.AccessToken;
		headers = curl_slist_append(headers, authorization.c_str());
	}
	// Device registration is also signed. That lets the service refresh an
	// expired bearer token for an existing local key without treating the
	// public key itself as an authentication secret.
	if (Config.Identity != nullptr && (!Config.DeviceId.empty() || path == "/v1/devices"))
	{
		std::array<uint8_t, 16> nonceBytes = {};
		randombytes_buf(nonceBytes.data(), nonceBytes.size());
		const auto timestamp = RequestTimestamp();
		const auto nonce = EncodeBase64(nonceBytes.data(), nonceBytes.size());
		std::string signature;
		if (!Config.Identity->SignControlRequest(method, path, timestamp, nonce, body, signature, error))
		{
			curl_slist_free_all(headers);
			curl_easy_cleanup(handle);
			return false;
		}
		headers = curl_slist_append(headers, ("X-BiasedDoom-Timestamp: " + timestamp).c_str());
		headers = curl_slist_append(headers, ("X-BiasedDoom-Nonce: " + nonce).c_str());
		headers = curl_slist_append(headers, ("X-BiasedDoom-Signature: " + signature).c_str());
	}

	response.clear();
	curl_easy_setopt(handle, CURLOPT_URL, url.c_str());
	curl_easy_setopt(handle, CURLOPT_CUSTOMREQUEST, method);
	curl_easy_setopt(handle, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, WriteResponse);
	curl_easy_setopt(handle, CURLOPT_WRITEDATA, &response);
	curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT, Config.ConnectTimeoutSeconds);
	curl_easy_setopt(handle, CURLOPT_TIMEOUT, Config.RequestTimeoutSeconds);
	curl_easy_setopt(handle, CURLOPT_SSL_VERIFYPEER, 1L);
	curl_easy_setopt(handle, CURLOPT_SSL_VERIFYHOST, 2L);
	if (!body.empty())
	{
		curl_easy_setopt(handle, CURLOPT_POSTFIELDS, body.data());
		curl_easy_setopt(handle, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
	}

	const auto result = curl_easy_perform(handle);
	if (result == CURLE_OK)
		curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &status);
	curl_slist_free_all(headers);
	curl_easy_cleanup(handle);
	if (result != CURLE_OK)
	{
		error = std::string("online service request failed: ") + curl_easy_strerror(result);
		return false;
	}
	if (status < 200 || status >= 300)
	{
		if (!ParseError(response, error))
			error = "online service returned HTTP " + std::to_string(status);
		return false;
	}
	return true;
}

bool FOnlineServiceClient::RegisterDevice(const FOnlineDeviceIdentity& identity, std::string& error)
{
	const auto publicKey = identity.PublicKeyBase64();
	if (publicKey.empty())
	{
		error = "online device public key is empty";
		return false;
	}
	// Set this before Request so the initial registration is signed too. The
	// same signed endpoint doubles as a safe token refresh for an identity
	// restored from the local profile.
	Config.Identity = &identity;
	long status = 0;
	rapidjson::StringBuffer bodyBuffer;
	rapidjson::Writer<rapidjson::StringBuffer> writer(bodyBuffer);
	writer.StartObject();
	writer.Key("public_key"); writer.String(publicKey.c_str());
	writer.EndObject();
	std::string response;
	if (!Request("POST", "/v1/devices", bodyBuffer.GetString(), status, response, error))
		return false;
	rapidjson::Document document;
	if (!ParseJsonObject(response, document) || !ReadString(document, "access_token", Config.AccessToken) ||
		!ReadString(document, "device_id", Config.DeviceId) || !IsLowerHex(Config.DeviceId, 32) ||
		Config.AccessToken.empty() || Config.AccessToken.size() > 1024 ||
		Config.AccessToken.find_first_of("\r\n") != std::string::npos)
	{
		error = "online service returned an invalid device response";
		return false;
	}
	return true;
}

bool FOnlineServiceClient::CreateSession(const FSessionManifest& manifest, const std::string& password, FOnlineSession& session, std::string& error) const
{
	if (!manifest.Validate(error))
		return false;
	std::string passwordProof;
	if (manifest.PasswordRequired)
	{
		if (!BuildPasswordProof(password, passwordProof, error))
			return false;
	}
	else if (!password.empty())
	{
		error = "an online password was supplied for a session that does not require one";
		return false;
	}
	long status = 0;
	std::string response;
	if (!Request("POST", "/v1/sessions", SerializeCreateSession(manifest, passwordProof), status, response, error))
		return false;
	rapidjson::Document document;
	uint64_t epoch = 0;
	if (!ParseJsonObject(response, document) || !ReadString(document, "session_id", session.SessionId) ||
		!ReadString(document, "join_code", session.JoinCode) || !ReadString(document, "relay_token", session.RelayToken) ||
		!ReadString(document, "relay_auth", session.RelayAuth) || !ReadString(document, "session_key", session.SessionKey) || !ReadString(document, "local_peer_id", session.LocalPeerId) ||
		!ReadString(document, "host_peer_id", session.HostPeerId) || !ReadString(document, "endpoint", session.Endpoint) ||
		!ReadUnsigned(document, "epoch", epoch) || epoch > std::numeric_limits<uint32_t>::max())
	{
		error = "online service returned an invalid create-session response";
		return false;
	}
	if (!IsLowerHex(session.SessionKey, OnlineKeyBytes * 2) || !IsLowerHex(session.LocalPeerId, 32) ||
		!IsLowerHex(session.HostPeerId, 32) || !IsLowerHex(session.RelayToken, 32) || !IsLowerHex(session.RelayAuth, 32))
	{
		error = "online service returned invalid transport credentials";
		return false;
	}
	session.Manifest = manifest;
	session.Visibility = manifest.Visibility;
	session.Epoch = static_cast<uint32_t>(epoch);
	session.Phase = EOnlineSessionPhase::Advertising;
	return true;
}

bool FOnlineServiceClient::RenewSession(const FOnlineSession& session, std::string& error) const
{
	if (!IsSafePathComponent(session.SessionId, 64))
	{
		error = "online session id is empty";
		return false;
	}
	long status = 0;
	std::string response;
	return Request("POST", "/v1/sessions/" + session.SessionId + "/renew", "{}", status, response, error);
}

bool FOnlineServiceClient::UpdateRoute(const FOnlineSession& session, const std::string& endpoint, std::string& error) const
{
	if (!IsSafePathComponent(session.SessionId, 64) || endpoint.empty() || endpoint.size() > 255)
	{
		error = "online session route is invalid";
		return false;
	}
	rapidjson::StringBuffer buffer;
	rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
	writer.StartObject();
	writer.Key("endpoint"); writer.String(endpoint.c_str());
	writer.EndObject();
	long status = 0;
	std::string response;
	return Request("POST", "/v1/sessions/" + session.SessionId + "/route",
		std::string(buffer.GetString(), buffer.GetSize()), status, response, error);
}

bool FOnlineServiceClient::MigrateSession(const FOnlineSession& session, const std::vector<std::string>& eligiblePeerIds,
	FOnlineHostMigration& migration, std::string& error) const
{
	if (!IsSafePathComponent(session.SessionId, 64) || eligiblePeerIds.empty() || eligiblePeerIds.size() >= 64)
	{
		error = "online migration roster is invalid";
		return false;
	}
	rapidjson::StringBuffer buffer;
	rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
	writer.StartObject();
	writer.Key("eligible_peer_ids");
	writer.StartArray();
	for (const auto& peerId : eligiblePeerIds)
	{
		if (!IsLowerHex(peerId, 32))
		{
			error = "online migration peer identity is invalid";
			return false;
		}
		writer.String(peerId.c_str());
	}
	writer.EndArray();
	writer.EndObject();
	long status = 0;
	std::string response;
	if (!Request("POST", "/v1/sessions/" + session.SessionId + "/migrate",
		std::string(buffer.GetString(), buffer.GetSize()), status, response, error))
	{
		return false;
	}
	rapidjson::Document document;
	uint64_t epoch = 0;
	if (!ParseJsonObject(response, document) || !ReadString(document, "host_peer_id", migration.HostPeerId) ||
		!ReadUnsigned(document, "epoch", epoch) || epoch == 0 || epoch > std::numeric_limits<uint32_t>::max() ||
		!IsLowerHex(migration.HostPeerId, 32))
	{
		error = "online service returned an invalid migration response";
		return false;
	}
	migration.Epoch = static_cast<uint32_t>(epoch);
	return true;
}

bool FOnlineServiceClient::RecoverSession(const FOnlineSession& session, const std::vector<std::string>& eligiblePeerIds,
	FOnlineHostMigration& migration, std::string& error) const
{
	if (!IsSafePathComponent(session.SessionId, 64) || eligiblePeerIds.empty() || eligiblePeerIds.size() >= 64)
	{
		error = "online recovery roster is invalid";
		return false;
	}
	rapidjson::StringBuffer buffer;
	rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
	writer.StartObject();
	writer.Key("eligible_peer_ids");
	writer.StartArray();
	for (const auto& peerId : eligiblePeerIds)
	{
		if (!IsLowerHex(peerId, 32))
		{
			error = "online recovery peer identity is invalid";
			return false;
		}
		writer.String(peerId.c_str());
	}
	writer.EndArray();
	writer.EndObject();
	long status = 0;
	std::string response;
	if (!Request("POST", "/v1/sessions/" + session.SessionId + "/recover",
		std::string(buffer.GetString(), buffer.GetSize()), status, response, error))
	{
		return false;
	}
	rapidjson::Document document;
	uint64_t epoch = 0;
	if (!ParseJsonObject(response, document) || !ReadString(document, "host_peer_id", migration.HostPeerId) ||
		!ReadUnsigned(document, "epoch", epoch) || epoch == 0 || epoch > std::numeric_limits<uint32_t>::max() ||
		!IsLowerHex(migration.HostPeerId, 32))
	{
		error = "online service returned an invalid recovery response";
		return false;
	}
	const auto recovered = document.FindMember("recovered");
	if (recovered != document.MemberEnd())
	{
		if (!recovered->value.IsBool())
		{
			error = "online service returned an invalid recovery response";
			return false;
		}
		migration.RecoveredAfterHostLoss = recovered->value.GetBool();
	}
	migration.Epoch = static_cast<uint32_t>(epoch);
	return true;
}

bool FOnlineServiceClient::LeaveSession(const FOnlineSession& session, std::string& error) const
{
	if (!IsSafePathComponent(session.SessionId, 64))
	{
		error = "online session id is empty";
		return false;
	}
	long status = 0;
	std::string response;
	return Request("POST", "/v1/sessions/" + session.SessionId + "/leave", "{}", status, response, error);
}

bool FOnlineServiceClient::CloseSession(const FOnlineSession& session, std::string& error) const
{
	if (!IsSafePathComponent(session.SessionId, 64))
	{
		error = "online session id is empty";
		return false;
	}
	long status = 0;
	std::string response;
	return Request("POST", "/v1/sessions/" + session.SessionId + "/close", "{}", status, response, error);
}

bool FOnlineServiceClient::CloseLobby(const FOnlineSession& session, std::string& error) const
{
	if (!IsSafePathComponent(session.SessionId, 64))
	{
		error = "online session id is empty";
		return false;
	}
	long status = 0;
	std::string response;
	return Request("POST", "/v1/sessions/" + session.SessionId + "/state", "{\"lobby_open\":false}", status, response, error);
}

bool FOnlineServiceClient::ListSessions(std::vector<FOnlineSessionListing>& sessions, std::string& error) const
{
	long status = 0;
	std::string response;
	if (!Request("GET", "/v1/sessions", {}, status, response, error))
		return false;
	rapidjson::Document document;
	if (!ParseJsonObject(response, document))
	{
		error = "online service returned an invalid session list";
		return false;
	}
	const auto listed = document.FindMember("sessions");
	if (listed == document.MemberEnd() || !listed->value.IsArray())
	{
		error = "online service session list is missing";
		return false;
	}
	std::vector<FOnlineSessionListing> parsed;
	for (const auto& item : listed->value.GetArray())
	{
		FOnlineSessionListing listing;
		uint64_t players = 0;
		uint64_t reservedPlayers = 0;
		uint64_t maxPlayers = 0;
		if (!item.IsObject() || !ReadString(item, "session_id", listing.SessionId) || !ReadString(item, "join_code", listing.JoinCode) ||
		!ReadString(item, "title", listing.Title) || !ReadString(item, "region", listing.Region) || !ReadString(item, "mode", listing.GameMode) ||
			!ReadUnsigned(item, "players", players) || !ReadUnsigned(item, "max_players", maxPlayers) || players > 64 || maxPlayers == 0 || maxPlayers > 64 ||
			!ReadBoolean(item, "password_required", listing.PasswordRequired) || !ReadBoolean(item, "modded", listing.Modded) ||
			!ReadBoolean(item, "allow_join_in_progress", listing.AllowJoinInProgress))
		{
			error = "online service listed an invalid session";
			return false;
		}
		// A directory which predates explicit state does not know whether its
		// listed session has begun gameplay. Treat it as closed rather than
		// falsely sending a player toward an unsupported in-progress join.
		const auto lobbyOpen = item.FindMember("lobby_open");
		if (lobbyOpen != item.MemberEnd())
		{
			if (!lobbyOpen->value.IsBool())
			{
				error = "online service session listing has an invalid lobby state";
				return false;
			}
			listing.LobbyOpen = lobbyOpen->value.GetBool();
		}
		const auto reserved = item.FindMember("reserved_players");
		if (reserved != item.MemberEnd())
		{
			if (!reserved->value.IsUint64())
			{
				error = "online service session listing has an invalid reservation count";
				return false;
			}
			reservedPlayers = reserved->value.GetUint64();
		}
		if (reservedPlayers > 64 || players > maxPlayers || reservedPlayers > maxPlayers - players)
		{
			error = "online service session listing has invalid player counts";
			return false;
		}
		listing.Players = static_cast<uint32_t>(players);
		listing.ReservedPlayers = static_cast<uint32_t>(reservedPlayers);
		listing.MaxPlayers = static_cast<uint32_t>(maxPlayers);
		parsed.push_back(std::move(listing));
	}
	sessions = std::move(parsed);
	return true;
}

bool FOnlineServiceClient::InspectJoinCode(const std::string& joinCode, const std::string& password, FJoinTicket& ticket, FSessionManifest& manifest, std::string& error) const
{
	return ResolveJoinCodeInternal(joinCode, password, true, ticket, manifest, error);
}

bool FOnlineServiceClient::ResolveJoinCode(const std::string& joinCode, const std::string& password, FJoinTicket& ticket, FSessionManifest& manifest, std::string& error) const
{
	return ResolveJoinCodeInternal(joinCode, password, false, ticket, manifest, error);
}

bool FOnlineServiceClient::IsPeerAdmitted(const FOnlineSession& session, const std::string& peerId, FOnlinePeerAdmission& admission, std::string& error) const
{
	admission = {};
	if (!IsLowerHex(session.SessionId, 32) || !IsLowerHex(peerId, 32) || peerId == session.LocalPeerId)
	{
		error = "online peer admission query is invalid";
		return false;
	}
	rapidjson::StringBuffer bodyBuffer;
	rapidjson::Writer<rapidjson::StringBuffer> writer(bodyBuffer);
	writer.StartObject();
	writer.Key("peer_id"); writer.String(peerId.c_str());
	writer.EndObject();
	long status = 0;
	std::string response;
	if (!Request("POST", "/v1/sessions/" + session.SessionId + "/admission",
		std::string(bodyBuffer.GetString(), bodyBuffer.GetSize()), status, response, error))
	{
		return false;
	}
	rapidjson::Document document;
	if (!ParseJsonObject(response, document) || !ReadBoolean(document, "admitted", admission.Admitted) ||
		!ReadOptionalString(document, "public_key", admission.PublicKey) ||
		!ReadOptionalString(document, "direct_endpoint", admission.DirectEndpoint))
	{
		error = "online service returned an invalid peer admission response";
		return false;
	}
	if (admission.Admitted && admission.PublicKey.empty())
	{
		error = "online service omitted the admitted peer identity";
		return false;
	}
	return true;
}

bool FOnlineServiceClient::ResolveJoinCodeInternal(const std::string& joinCode, const std::string& password, bool preflight, FJoinTicket& ticket,
	FSessionManifest& manifest, std::string& error) const
{
	if (!IsSafePathComponent(joinCode, 32))
	{
		error = "online join code is invalid";
		return false;
	}
	std::string passwordProof;
	if (!password.empty() && !BuildPasswordProof(password, passwordProof, error))
		return false;
	rapidjson::StringBuffer bodyBuffer;
	rapidjson::Writer<rapidjson::StringBuffer> writer(bodyBuffer);
	writer.StartObject();
	if (preflight)
	{
		writer.Key("preflight"); writer.Bool(true);
	}
	if (!passwordProof.empty())
	{
		writer.Key("password_proof"); writer.String(passwordProof.c_str());
	}
	writer.EndObject();
	long status = 0;
	std::string response;
	if (!Request("POST", "/v1/join/" + joinCode, std::string(bodyBuffer.GetString(), bodyBuffer.GetSize()), status, response, error))
		return false;
	rapidjson::Document document;
	uint64_t expiresAt = 0;
	uint64_t epoch = 0;
	if (!ParseJsonObject(response, document) || !ReadString(document, "session_id", ticket.SessionId) ||
		!ReadString(document, "ticket", ticket.Ticket) || !ReadUnsigned(document, "expires_at", expiresAt) ||
		!ReadUnsigned(document, "epoch", epoch) || epoch > std::numeric_limits<uint32_t>::max() ||
		!ReadBoolean(document, "lobby_open", ticket.LobbyOpen) ||
		!ReadBoolean(document, "uses_relay", ticket.UsesRelay) || !ReadString(document, "endpoint", ticket.Endpoint) ||
		!ReadString(document, "relay_token", ticket.RelayToken) || !ReadString(document, "relay_auth", ticket.RelayAuth) || !ReadString(document, "session_key", ticket.SessionKey) ||
		!ReadString(document, "local_peer_id", ticket.LocalPeerId) || !ReadString(document, "host_peer_id", ticket.HostPeerId) ||
		!ReadOptionalString(document, "host_public_key", ticket.HostPublicKey) ||
		!ReadOptionalString(document, "host_direct_endpoint", ticket.HostDirectEndpoint))
	{
		error = "online service returned an invalid join response";
		return false;
	}
	if (!IsLowerHex(ticket.SessionId, 32) || !IsLowerHex(ticket.RelayToken, 32) ||
		!IsLowerHex(ticket.SessionKey, OnlineKeyBytes * 2) || !IsLowerHex(ticket.LocalPeerId, 32) ||
		!IsLowerHex(ticket.HostPeerId, 32) || !IsLowerHex(ticket.RelayAuth, 32))
	{
		error = "online service returned invalid transport credentials";
		return false;
	}
	const auto serviceManifest = document.FindMember("manifest");
	if (serviceManifest == document.MemberEnd() || !ParseSessionManifest(serviceManifest->value, manifest, error))
		return false;
	ticket.ExpiresAtUnixSeconds = expiresAt;
	ticket.Epoch = static_cast<uint32_t>(epoch);
	return true;
}

bool FOnlineServiceClient::ReportSession(const std::string& sessionId, const std::string& reason, std::string& error) const
{
	if (!IsSafePathComponent(sessionId, 64) || reason.empty() || reason.size() > 256)
	{
		error = "online report is invalid";
		return false;
	}
	rapidjson::StringBuffer buffer;
	rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
	writer.StartObject();
	writer.Key("reason"); writer.String(reason.c_str());
	writer.EndObject();
	long status = 0;
	std::string response;
	const auto body = std::string(buffer.GetString(), buffer.GetSize());
	return Request("POST", "/v1/sessions/" + sessionId + "/report", body, status, response, error);
}

} // namespace BdOnline
