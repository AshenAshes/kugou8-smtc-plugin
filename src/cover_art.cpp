#include "cover_art.h"

#include <windows.h>
#include <shlobj.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <webp/decode.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cwchar>
#include <cwctype>
#include <optional>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace plugin {
namespace {

constexpr unsigned kMaximumTraversalDepth = 4;

enum class ImageFormat {
    unknown,
    jpeg,
    png,
    webp,
};

constexpr std::uint64_t kMaximumEncodedCoverBytes = 32ULL * 1024 * 1024;

class FindHandle final {
public:
    explicit FindHandle(HANDLE handle) noexcept : handle_(handle) {}
    ~FindHandle() {
        if (handle_ != INVALID_HANDLE_VALUE) {
            FindClose(handle_);
        }
    }

    FindHandle(const FindHandle&) = delete;
    FindHandle& operator=(const FindHandle&) = delete;

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
};

std::uint64_t FileTimeTicks(const FILETIME& time) noexcept {
    ULARGE_INTEGER value{};
    value.LowPart = time.dwLowDateTime;
    value.HighPart = time.dwHighDateTime;
    return value.QuadPart;
}

std::wstring RoamingDataRoot() {
    std::array<wchar_t, MAX_PATH> path{};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr,
                               SHGFP_TYPE_CURRENT, path.data()))) {
        return {};
    }
    return path.data();
}

std::wstring LocalDataRoot() {
    std::array<wchar_t, MAX_PATH> path{};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr,
                               SHGFP_TYPE_CURRENT, path.data()))) {
        return {};
    }
    return path.data();
}

bool HasImageExtension(const wchar_t* name) noexcept {
    const wchar_t* extension = wcsrchr(name, L'.');
    return extension != nullptr &&
           (_wcsicmp(extension, L".jpg") == 0 ||
            _wcsicmp(extension, L".jpeg") == 0 ||
            _wcsicmp(extension, L".png") == 0 ||
            _wcsicmp(extension, L".webp") == 0);
}

int ResolutionHint(const std::wstring& path) noexcept {
    const size_t file_separator = path.find_last_of(L"\\/");
    if (file_separator == std::wstring::npos || file_separator == 0) {
        return 0;
    }
    const size_t parent_separator =
        path.find_last_of(L"\\/", file_separator - 1);
    const size_t parent_start = parent_separator == std::wstring::npos
                                    ? 0
                                    : parent_separator + 1;
    return _wtoi(path.c_str() + parent_start);
}

bool PreferDetailedCandidate(const CoverArt& candidate,
                             const CoverArt& current) noexcept {
    const int candidate_resolution = ResolutionHint(candidate.path);
    const int current_resolution = ResolutionHint(current.path);
    if (candidate_resolution != current_resolution) {
        return candidate_resolution > current_resolution;
    }
    return candidate.last_access_ticks > current.last_access_ticks;
}

void SearchDirectory(const std::wstring& directory, unsigned depth,
                     std::optional<CoverArt>* best) {
    if (depth > kMaximumTraversalDepth) {
        return;
    }

    std::wstring pattern = directory;
    if (!pattern.empty() && pattern.back() != L'\\') {
        pattern.push_back(L'\\');
    }
    pattern.push_back(L'*');

    WIN32_FIND_DATAW data{};
    HANDLE raw_search = FindFirstFileW(pattern.c_str(), &data);
    if (raw_search == INVALID_HANDLE_VALUE) {
        return;
    }
    const FindHandle search(raw_search);

    do {
        if (wcscmp(data.cFileName, L".") == 0 ||
            wcscmp(data.cFileName, L"..") == 0) {
            continue;
        }

        std::wstring path = directory;
        if (!path.empty() && path.back() != L'\\') {
            path.push_back(L'\\');
        }
        path += data.cFileName;

        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            if ((data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0) {
                SearchDirectory(path, depth + 1, best);
            }
            continue;
        }
        if (!HasImageExtension(data.cFileName)) {
            continue;
        }

        CoverArt candidate{
            std::move(path), FileTimeTicks(data.ftLastAccessTime)};
        if (!best->has_value() ||
            PreferDetailedCandidate(candidate, **best)) {
            *best = std::move(candidate);
        }
    } while (FindNextFileW(raw_search, &data));
}

std::wstring DirectoryKey(std::wstring_view name) {
    const size_t separator = name.find_last_of(L'_');
    if (separator == std::wstring_view::npos || separator + 1 >= name.size()) {
        return std::wstring(name);
    }
    const bool numeric_suffix = std::all_of(
        name.begin() + separator + 1, name.end(),
        [](wchar_t value) { return iswdigit(value) != 0; });
    return std::wstring(name.substr(0, numeric_suffix ? separator
                                                      : name.size()));
}

std::wstring Normalize(std::wstring_view value) {
    std::wstring result;
    result.reserve(value.size());
    for (wchar_t character : value) {
        if (iswspace(character) || wcschr(L"\\/:*?\"<>|", character)) {
            continue;
        }
        result.push_back(static_cast<wchar_t>(towlower(character)));
    }
    return result;
}

std::wstring RemoveTrailingDescription(std::wstring_view value) {
    std::wstring result(value);
    while (!result.empty() && iswspace(result.back())) {
        result.pop_back();
    }
    if (result.empty() || (result.back() != L')' && result.back() != L'）')) {
        return result;
    }
    const wchar_t open = result.back() == L')' ? L'(' : L'（';
    const size_t opening = result.find_last_of(open);
    if (opening == std::wstring::npos) {
        return result;
    }
    result.erase(opening);
    while (!result.empty() && iswspace(result.back())) {
        result.pop_back();
    }
    return result;
}

std::vector<std::wstring> TitleKeys(std::wstring_view title) {
    std::vector<std::wstring> keys;
    const std::wstring normalized = Normalize(title);
    if (!normalized.empty()) {
        keys.push_back(normalized);
    }
    const std::wstring primary = Normalize(RemoveTrailingDescription(title));
    if (!primary.empty() &&
        std::find(keys.begin(), keys.end(), primary) == keys.end()) {
        keys.push_back(primary);
    }
    return keys;
}

std::vector<std::wstring> ArtistKeys(std::wstring_view artist) {
    std::vector<std::wstring> keys;
    size_t start = 0;
    for (size_t index = 0; index <= artist.size(); ++index) {
        const bool separator =
            index == artist.size() || artist[index] == L'、' ||
            artist[index] == L',' || artist[index] == L'，' ||
            artist[index] == L'&' || artist[index] == L';' ||
            artist[index] == L'；';
        if (!separator) {
            continue;
        }
        const std::wstring key = Normalize(artist.substr(start, index - start));
        if (!key.empty() &&
            std::find(keys.begin(), keys.end(), key) == keys.end()) {
            keys.push_back(key);
        }
        start = index + 1;
    }
    return keys;
}

std::optional<CoverArt> FindMatchingCacheEntry(
    const std::wstring& root, const std::vector<std::wstring>& keys) {
    if (root.empty() || keys.empty()) {
        return std::nullopt;
    }

    std::wstring pattern = root;
    if (!pattern.empty() && pattern.back() != L'\\') {
        pattern.push_back(L'\\');
    }
    pattern.push_back(L'*');

    WIN32_FIND_DATAW data{};
    HANDLE raw_search = FindFirstFileW(pattern.c_str(), &data);
    if (raw_search == INVALID_HANDLE_VALUE) {
        return std::nullopt;
    }
    const FindHandle search(raw_search);

    std::optional<CoverArt> best;
    do {
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
            (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
            wcscmp(data.cFileName, L".") == 0 ||
            wcscmp(data.cFileName, L"..") == 0) {
            continue;
        }
        const std::wstring key = Normalize(DirectoryKey(data.cFileName));
        if (std::find(keys.begin(), keys.end(), key) == keys.end()) {
            continue;
        }

        std::wstring directory = root;
        if (!directory.empty() && directory.back() != L'\\') {
            directory.push_back(L'\\');
        }
        directory += data.cFileName;
        std::optional<CoverArt> candidate;
        SearchDirectory(directory, 0, &candidate);
        if (candidate.has_value() &&
            (!best.has_value() ||
             PreferDetailedCandidate(*candidate, *best))) {
            best = std::move(candidate);
        }
    } while (FindNextFileW(raw_search, &data));
    return best;
}

ImageFormat DetectFormat(const std::wstring& path) noexcept {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE |
                                  FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return ImageFormat::unknown;
    }
    std::array<unsigned char, 16> bytes{};
    DWORD bytes_read = 0;
    const BOOL read = ReadFile(file, bytes.data(),
                               static_cast<DWORD>(bytes.size()),
                               &bytes_read, nullptr);
    CloseHandle(file);
    if (!read || bytes_read < 12) {
        return ImageFormat::unknown;
    }
    if (bytes[0] == 0xFF && bytes[1] == 0xD8) {
        return ImageFormat::jpeg;
    }
    const std::array<unsigned char, 8> png = {
        0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    if (std::equal(png.begin(), png.end(), bytes.begin())) {
        return ImageFormat::png;
    }
    if (std::equal(bytes.begin(), bytes.begin() + 4,
                   std::array<unsigned char, 4>{'R', 'I', 'F', 'F'}.begin()) &&
        std::equal(bytes.begin() + 8, bytes.begin() + 12,
                   std::array<unsigned char, 4>{'W', 'E', 'B', 'P'}.begin())) {
        return ImageFormat::webp;
    }
    return ImageFormat::unknown;
}

const wchar_t* ExtensionForFormat(ImageFormat format) noexcept {
    switch (format) {
        case ImageFormat::jpeg:
            return L".jpg";
        case ImageFormat::png:
            return L".png";
        case ImageFormat::webp:
            return L".webp";
        case ImageFormat::unknown:
            return L"";
    }
    return L"";
}

bool ExtensionMatches(const std::wstring& path,
                      ImageFormat format) noexcept {
    const wchar_t* actual = wcsrchr(path.c_str(), L'.');
    const wchar_t* expected = ExtensionForFormat(format);
    if (actual == nullptr || *expected == L'\0') {
        return false;
    }
    if (format == ImageFormat::jpeg) {
        return _wcsicmp(actual, L".jpg") == 0 ||
               _wcsicmp(actual, L".jpeg") == 0;
    }
    return _wcsicmp(actual, expected) == 0;
}

std::optional<std::vector<std::uint8_t>> ReadFileBytes(
    const std::wstring& path) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE |
                                  FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return std::nullopt;
    }

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 ||
        static_cast<std::uint64_t>(size.QuadPart) >
            kMaximumEncodedCoverBytes) {
        CloseHandle(file);
        return std::nullopt;
    }

    std::vector<std::uint8_t> bytes(
        static_cast<size_t>(size.QuadPart));
    DWORD bytes_read = 0;
    const BOOL read = ReadFile(file, bytes.data(),
                               static_cast<DWORD>(bytes.size()),
                               &bytes_read, nullptr);
    CloseHandle(file);
    if (!read || bytes_read != bytes.size()) {
        return std::nullopt;
    }
    return bytes;
}

bool ConvertWebPToPng(const std::wstring& source,
                      const std::wstring& destination) {
    const auto encoded = ReadFileBytes(source);
    if (!encoded.has_value()) {
        return false;
    }

    int width = 0;
    int height = 0;
    std::uint8_t* decoded = WebPDecodeBGRA(
        encoded->data(), encoded->size(), &width, &height);
    if (decoded == nullptr || width <= 0 || height <= 0) {
        WebPFree(decoded);
        return false;
    }

    const std::uint64_t stride = static_cast<std::uint64_t>(width) * 4;
    const std::uint64_t decoded_size =
        stride * static_cast<std::uint64_t>(height);
    if (stride > std::numeric_limits<UINT>::max() ||
        decoded_size > std::numeric_limits<UINT>::max()) {
        WebPFree(decoded);
        return false;
    }

    using Microsoft::WRL::ComPtr;
    ComPtr<IWICImagingFactory> factory;
    HRESULT result = CoCreateInstance(
        CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&factory));
    ComPtr<IWICStream> stream;
    if (SUCCEEDED(result)) {
        result = factory->CreateStream(&stream);
    }
    if (SUCCEEDED(result)) {
        result = stream->InitializeFromFilename(destination.c_str(),
                                                GENERIC_WRITE);
    }

    ComPtr<IWICBitmapEncoder> encoder;
    if (SUCCEEDED(result)) {
        result = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr,
                                        &encoder);
    }
    if (SUCCEEDED(result)) {
        result = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    }

    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> properties;
    if (SUCCEEDED(result)) {
        result = encoder->CreateNewFrame(&frame, &properties);
    }
    if (SUCCEEDED(result)) {
        result = frame->Initialize(properties.Get());
    }
    if (SUCCEEDED(result)) {
        result = frame->SetSize(static_cast<UINT>(width),
                                static_cast<UINT>(height));
    }
    WICPixelFormatGUID pixel_format = GUID_WICPixelFormat32bppBGRA;
    if (SUCCEEDED(result)) {
        result = frame->SetPixelFormat(&pixel_format);
    }
    if (SUCCEEDED(result) &&
        !IsEqualGUID(pixel_format, GUID_WICPixelFormat32bppBGRA)) {
        result = WINCODEC_ERR_UNSUPPORTEDPIXELFORMAT;
    }
    if (SUCCEEDED(result)) {
        result = frame->WritePixels(
            static_cast<UINT>(height), static_cast<UINT>(stride),
            static_cast<UINT>(decoded_size), decoded);
    }
    if (SUCCEEDED(result)) {
        result = frame->Commit();
    }
    if (SUCCEEDED(result)) {
        result = encoder->Commit();
    }
    WebPFree(decoded);
    return SUCCEEDED(result);
}

std::uint64_t PreparedNameHash(const CoverArt& source) noexcept {
    constexpr std::uint64_t kOffset = 14695981039346656037ULL;
    constexpr std::uint64_t kPrime = 1099511628211ULL;
    std::uint64_t hash = kOffset;
    for (wchar_t character : source.path) {
        hash ^= static_cast<std::uint16_t>(character);
        hash *= kPrime;
    }
    return hash;
}

std::optional<CoverArt> PrepareCover(
    const CoverArt& source, const std::wstring& prepared_root) {
    const ImageFormat format = DetectFormat(source.path);
    if (format == ImageFormat::unknown) {
        return std::nullopt;
    }
    if (format != ImageFormat::webp &&
        ExtensionMatches(source.path, format)) {
        return source;
    }
    if (prepared_root.empty()) {
        return std::nullopt;
    }
    const int directory_result =
        SHCreateDirectoryExW(nullptr, prepared_root.c_str(), nullptr);
    if (directory_result != ERROR_SUCCESS &&
        directory_result != ERROR_ALREADY_EXISTS &&
        directory_result != ERROR_FILE_EXISTS) {
        return std::nullopt;
    }

    wchar_t file_name[64]{};
    const wchar_t* prepared_extension =
        format == ImageFormat::webp ? L".png" : ExtensionForFormat(format);
    swprintf_s(file_name, L"%016llX%s",
               static_cast<unsigned long long>(PreparedNameHash(source)),
               prepared_extension);
    std::wstring prepared = prepared_root;
    if (!prepared.empty() && prepared.back() != L'\\') {
        prepared.push_back(L'\\');
    }
    prepared += file_name;
    if (GetFileAttributesW(prepared.c_str()) == INVALID_FILE_ATTRIBUTES) {
        if (format == ImageFormat::webp) {
            std::wstring temporary = prepared + L".tmp";
            DeleteFileW(temporary.c_str());
            if (!ConvertWebPToPng(source.path, temporary) ||
                !MoveFileExW(temporary.c_str(), prepared.c_str(),
                             MOVEFILE_REPLACE_EXISTING |
                                 MOVEFILE_WRITE_THROUGH)) {
                DeleteFileW(temporary.c_str());
                return std::nullopt;
            }
        } else if (!CopyFileW(source.path.c_str(), prepared.c_str(), TRUE) &&
                   GetLastError() != ERROR_FILE_EXISTS) {
            return std::nullopt;
        }
    }
    return CoverArt{std::move(prepared), source.last_access_ticks};
}

}  // namespace

CoverArtResolver::CoverArtResolver() {
    const std::wstring roaming = RoamingDataRoot();
    if (!roaming.empty()) {
        album_cache_root_ = roaming + L"\\KuGou8\\ImagesCache\\AlbumImg";
        singer_cache_root_ = roaming + L"\\KuGou8\\SingerRes";
    }
    const std::wstring local = LocalDataRoot();
    if (!local.empty()) {
        prepared_cache_root_ =
            local + L"\\KuGouSmtcPlugin\\CoverCache";
    }
}

CoverArtResolver::CoverArtResolver(std::wstring album_cache_root,
                                   std::wstring singer_cache_root,
                                   std::wstring prepared_cache_root)
    : album_cache_root_(std::move(album_cache_root)),
      singer_cache_root_(std::move(singer_cache_root)),
      prepared_cache_root_(std::move(prepared_cache_root)) {}

std::optional<CoverArt> CoverArtResolver::Resolve(
    std::wstring_view artist, std::wstring_view title) const noexcept {
    try {
        std::optional<CoverArt> source = FindMatchingCacheEntry(
            album_cache_root_, TitleKeys(title));
        if (!source.has_value()) {
            source = FindMatchingCacheEntry(singer_cache_root_,
                                            ArtistKeys(artist));
        }
        if (!source.has_value()) {
            return std::nullopt;
        }
        return PrepareCover(*source, prepared_cache_root_);
    } catch (...) {
        return std::nullopt;
    }
}

}  // namespace plugin
