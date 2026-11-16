#include "cover_art.h"
#include "runtime_policy.h"
#include "unique_handle.h"

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
#include <new>
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
constexpr UINT kMaximumSourceEdge = 4096;
constexpr std::uint64_t kMaximumSourcePixels = 16ULL * 1024 * 1024;
constexpr UINT kMaximumPreparedEdge = 512;

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
    const UniqueHandle file(CreateFileW(path.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE |
                                  FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                              nullptr));
    if (!file) {
        return std::nullopt;
    }

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file.get(), &size) || size.QuadPart <= 0 ||
        static_cast<std::uint64_t>(size.QuadPart) >
            kMaximumEncodedCoverBytes) {
        return std::nullopt;
    }

    std::vector<std::uint8_t> bytes(
        static_cast<size_t>(size.QuadPart));
    DWORD bytes_read = 0;
    const BOOL read = ReadFile(file.get(), bytes.data(),
                               static_cast<DWORD>(bytes.size()),
                               &bytes_read, nullptr);
    if (!read || bytes_read != bytes.size()) {
        return std::nullopt;
    }
    return bytes;
}

bool IsAllowedSourceSize(UINT width, UINT height) noexcept {
    return width != 0 && height != 0 && width <= kMaximumSourceEdge &&
           height <= kMaximumSourceEdge &&
           static_cast<std::uint64_t>(width) * height <= kMaximumSourcePixels;
}

bool CoverResultSucceeded(HRESULT result) {
    if (IsResourceAllocationFailure(result)) {
        throw std::bad_alloc();
    }
    return SUCCEEDED(result);
}

std::pair<UINT, UINT> PreparedSize(UINT width, UINT height) noexcept {
    const UINT longest = (std::max)(width, height);
    if (longest <= kMaximumPreparedEdge) {
        return {width, height};
    }
    return {(std::max)(1U, width * kMaximumPreparedEdge / longest),
            (std::max)(1U, height * kMaximumPreparedEdge / longest)};
}

using Microsoft::WRL::ComPtr;

struct WicImage final {
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICBitmapFrameDecode> frame;
    UINT width = 0;
    UINT height = 0;
};

bool OpenWicImage(const std::wstring& path, WicImage* image) {
    HRESULT result = CoCreateInstance(
        CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&image->factory));
    ComPtr<IWICBitmapDecoder> decoder;
    if (SUCCEEDED(result)) {
        result = image->factory->CreateDecoderFromFilename(
            path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand,
            &decoder);
    }
    if (SUCCEEDED(result)) {
        result = decoder->GetFrame(0, &image->frame);
    }
    if (SUCCEEDED(result)) {
        result = image->frame->GetSize(&image->width, &image->height);
    }
    return CoverResultSucceeded(result) &&
           IsAllowedSourceSize(image->width, image->height);
}

bool WritePng(IWICImagingFactory* factory, IWICBitmapSource* source,
              const std::wstring& destination) {
    UINT width = 0;
    UINT height = 0;
    HRESULT result = source->GetSize(&width, &height);
    if (FAILED(result) || width == 0 || height == 0 ||
        width > kMaximumPreparedEdge || height > kMaximumPreparedEdge) {
        return false;
    }
    ComPtr<IWICFormatConverter> converted;
    result = factory->CreateFormatConverter(&converted);
    if (SUCCEEDED(result)) {
        result = converted->Initialize(source, GUID_WICPixelFormat32bppBGRA,
            WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom);
    }
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
        result = frame->SetSize(width, height);
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
        result = frame->WriteSource(converted.Get(), nullptr);
    }
    if (SUCCEEDED(result)) {
        result = frame->Commit();
    }
    if (SUCCEEDED(result)) {
        result = encoder->Commit();
    }
    return CoverResultSucceeded(result);
}

bool ConvertWebPToPng(const std::wstring& source,
                      const std::wstring& destination) {
    const auto encoded = ReadFileBytes(source);
    if (!encoded) {
        return false;
    }
    WebPDecoderConfig config{};
    if (!WebPInitDecoderConfig(&config)) {
        return false;
    }
    struct BufferGuard final {
        WebPDecBuffer* buffer;
        ~BufferGuard() { WebPFreeDecBuffer(buffer); }
    } guard{&config.output};
    if (WebPGetFeatures(encoded->data(), encoded->size(), &config.input) !=
            VP8_STATUS_OK || config.input.width <= 0 || config.input.height <= 0 ||
        !IsAllowedSourceSize(static_cast<UINT>(config.input.width),
                             static_cast<UINT>(config.input.height))) {
        return false;
    }
    const auto [width, height] = PreparedSize(
        static_cast<UINT>(config.input.width),
        static_cast<UINT>(config.input.height));
    config.output.colorspace = MODE_BGRA;
    config.options.use_scaling = 1;
    config.options.scaled_width = static_cast<int>(width);
    config.options.scaled_height = static_cast<int>(height);
    const VP8StatusCode decode_result =
        WebPDecode(encoded->data(), encoded->size(), &config);
    if (decode_result == VP8_STATUS_OUT_OF_MEMORY) {
        throw std::bad_alloc();
    }
    if (decode_result != VP8_STATUS_OK) {
        return false;
    }
    ComPtr<IWICImagingFactory> factory;
    HRESULT result = CoCreateInstance(
        CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&factory));
    ComPtr<IWICBitmap> bitmap;
    if (SUCCEEDED(result)) {
        result = factory->CreateBitmapFromMemory(width, height,
            GUID_WICPixelFormat32bppBGRA,
            static_cast<UINT>(config.output.u.RGBA.stride),
            static_cast<UINT>(config.output.u.RGBA.size),
            config.output.u.RGBA.rgba, &bitmap);
    }
    return CoverResultSucceeded(result) &&
           WritePng(factory.Get(), bitmap.Get(), destination);
}

bool ResizeWicImage(const WicImage& image, const std::wstring& destination) {
    const auto [width, height] = PreparedSize(image.width, image.height);
    ComPtr<IWICBitmapScaler> scaled;
    HRESULT result = image.factory->CreateBitmapScaler(&scaled);
    if (SUCCEEDED(result)) {
        result = scaled->Initialize(image.frame.Get(), width, height,
                                     WICBitmapInterpolationModeFant);
    }
    return CoverResultSucceeded(result) &&
           WritePng(image.factory.Get(), scaled.Get(), destination);
}

std::uint64_t PreparedNameHash(const CoverArt& source,
                              const WIN32_FILE_ATTRIBUTE_DATA& attributes) noexcept {
    constexpr std::uint64_t kOffset = 14695981039346656037ULL;
    constexpr std::uint64_t kPrime = 1099511628211ULL;
    std::uint64_t hash = kOffset;
    for (wchar_t character : source.path) {
        hash ^= static_cast<std::uint16_t>(character);
        hash *= kPrime;
    }
    const std::array<std::uint64_t, 2> identity{
        FileTimeTicks(attributes.ftLastWriteTime),
        (static_cast<std::uint64_t>(attributes.nFileSizeHigh) << 32) |
            attributes.nFileSizeLow};
    for (const auto value : identity) {
        for (unsigned shift = 0; shift < 64; shift += 8) {
            hash ^= (value >> shift) & 0xFF;
            hash *= kPrime;
        }
    }
    return hash;
}

std::optional<CoverArt> PrepareCover(
    const CoverArt& source, const std::wstring& prepared_root) {
    WIN32_FILE_ATTRIBUTE_DATA attributes{};
    if (!GetFileAttributesExW(source.path.c_str(), GetFileExInfoStandard,
                              &attributes) || attributes.nFileSizeHigh != 0 ||
        attributes.nFileSizeLow > kMaximumEncodedCoverBytes) {
        return std::nullopt;
    }
    const ImageFormat format = DetectFormat(source.path);
    if (format == ImageFormat::unknown) {
        return std::nullopt;
    }
    WicImage image;
    if (format != ImageFormat::webp && !OpenWicImage(source.path, &image)) {
        return std::nullopt;
    }
    const bool needs_resize = format != ImageFormat::webp &&
        (image.width > kMaximumPreparedEdge || image.height > kMaximumPreparedEdge);
    if (format != ImageFormat::webp && !needs_resize &&
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
        format == ImageFormat::webp || needs_resize ? L".png" : ExtensionForFormat(format);
    swprintf_s(file_name, L"v2-%016llX%s",
               static_cast<unsigned long long>(PreparedNameHash(source, attributes)),
               prepared_extension);
    std::wstring prepared = prepared_root;
    if (!prepared.empty() && prepared.back() != L'\\') {
        prepared.push_back(L'\\');
    }
    prepared += file_name;
    if (GetFileAttributesW(prepared.c_str()) == INVALID_FILE_ATTRIBUTES) {
        if (format == ImageFormat::webp || needs_resize) {
            const std::wstring temporary = prepared + L"." +
                std::to_wstring(GetCurrentProcessId()) + L".tmp";
            struct TemporaryFile final {
                const std::wstring& path;
                ~TemporaryFile() { DeleteFileW(path.c_str()); }
            } cleanup{temporary};
            DeleteFileW(temporary.c_str());
            const bool converted = format == ImageFormat::webp
                ? ConvertWebPToPng(source.path, temporary)
                : ResizeWicImage(image, temporary);
            if (!converted ||
                !MoveFileExW(temporary.c_str(), prepared.c_str(),
                             MOVEFILE_REPLACE_EXISTING |
                                 MOVEFILE_WRITE_THROUGH)) {
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
    std::wstring_view artist, std::wstring_view title) const {
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
    } catch (const std::bad_alloc&) {
        throw;
    } catch (...) {
        return std::nullopt;
    }
}

}  // namespace plugin
