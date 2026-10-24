#include "esm/settings_store.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace esm::settings {
namespace {

constexpr std::uint32_t kStorageFormatVersion = 1U;
constexpr std::size_t kStorageEnvelopeMemberCount = 6U;
constexpr std::size_t kMaxJsonNodes = 4096U;
constexpr std::size_t kMaxJsonDepth = 32U;
// The values subtree is bounded independently; the SAX pass also sees the
// document root, all member keys, and the five non-values member values.
constexpr std::size_t kStoredEnvelopeAdditionalJsonNodes =
    1U + kStorageEnvelopeMemberCount + (kStorageEnvelopeMemberCount - 1U);
constexpr std::size_t kStoredEnvelopeAdditionalJsonDepth = 2U;
constexpr std::size_t kMaxStoredDocumentJsonNodes = kMaxJsonNodes + kStoredEnvelopeAdditionalJsonNodes;
constexpr std::size_t kMaxStoredDocumentJsonDepth = kMaxJsonDepth + kStoredEnvelopeAdditionalJsonDepth;
constexpr std::size_t kMaxErrorBytes = 512U;
std::atomic<std::uint64_t> g_temp_file_sequence{0U};

struct OperationError final {
    StoreStatus status = StoreStatus::io_error;
    std::string detail;
};

class UniqueHandle final {
public:
    UniqueHandle() = default;
    explicit UniqueHandle(HANDLE handle) noexcept : handle_(handle) {}
    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

    UniqueHandle(UniqueHandle&& other) noexcept : handle_(other.release()) {}
    UniqueHandle& operator=(UniqueHandle&& other) noexcept {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }

    ~UniqueHandle() { reset(); }

    HANDLE get() const noexcept { return handle_; }
    bool valid() const noexcept { return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE; }

    HANDLE release() noexcept {
        const HANDLE released = handle_;
        handle_ = nullptr;
        return released;
    }

    void reset(HANDLE handle = nullptr) noexcept {
        if (valid()) {
            CloseHandle(handle_);
        }
        handle_ = handle;
    }

private:
    HANDLE handle_ = nullptr;
};

std::string bounded_error(std::string message) {
    if (message.size() > kMaxErrorBytes) {
        message.resize(kMaxErrorBytes);
    }
    return message;
}

std::string windows_error(std::string_view operation, DWORD error) {
    std::string message(operation);
    message += " failed with Win32 error ";
    message += std::to_string(error);
    return bounded_error(std::move(message));
}

ReadResult read_failure(StoreStatus status, std::string error = {}) {
    return ReadResult{status, std::nullopt, bounded_error(std::move(error))};
}

ReadResult read_success(StoredSettings value) {
    return ReadResult{StoreStatus::ok, std::move(value), {}};
}

StoreResult store_failure(StoreStatus status, std::string error = {}) {
    return StoreResult{status, bounded_error(std::move(error))};
}

bool is_missing_error(DWORD error) noexcept {
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
}

std::wstring win32_path(const std::filesystem::path& path) {
    const std::wstring value = path.native();
    if (value.rfind(L"\\\\?\\", 0U) == 0U) {
        return value;
    }
    if (value.rfind(L"\\\\", 0U) == 0U) {
        return L"\\\\?\\UNC\\" + value.substr(2U);
    }
    if (value.size() >= 3U && value[1U] == L':' && (value[2U] == L'\\' || value[2U] == L'/')) {
        return L"\\\\?\\" + value;
    }
    return value;
}

bool normalize_host_root(const std::filesystem::path& input, std::filesystem::path& output,
                         OperationError& error) {
    if (input.empty() || !input.has_root_name() || !input.has_root_directory()) {
        error = {StoreStatus::unsafe_path, "host settings root must be an absolute filesystem path"};
        return false;
    }

    const std::wstring input_text = input.native();
    const DWORD needed = GetFullPathNameW(input_text.c_str(), 0U, nullptr, nullptr);
    if (needed == 0U) {
        const DWORD last_error = GetLastError();
        error = {StoreStatus::io_error, windows_error("normalizing host settings root", last_error)};
        return false;
    }

    std::vector<wchar_t> buffer(static_cast<std::size_t>(needed) + 1U, L'\0');
    if (buffer.size() > static_cast<std::size_t>(std::numeric_limits<DWORD>::max())) {
        error = {StoreStatus::unsafe_path, "host settings root path is too long"};
        return false;
    }
    const DWORD length = GetFullPathNameW(input_text.c_str(), static_cast<DWORD>(buffer.size()), buffer.data(), nullptr);
    if (length == 0U || static_cast<std::size_t>(length) >= buffer.size()) {
        const DWORD last_error = GetLastError();
        error = {StoreStatus::io_error, windows_error("normalizing host settings root", last_error)};
        return false;
    }

    output = std::filesystem::path(std::wstring(buffer.data(), length)).lexically_normal();
    if (!output.is_absolute() || output.root_path().empty()) {
        error = {StoreStatus::unsafe_path, "host settings root is not an absolute Windows path"};
        return false;
    }
    return true;
}

bool open_directory_no_follow(const std::filesystem::path& path, UniqueHandle& handle,
                              OperationError& error) {
    const std::wstring native = win32_path(path);
    HANDLE raw = CreateFileW(native.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             nullptr, OPEN_EXISTING,
                             FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (raw == INVALID_HANDLE_VALUE) {
        const DWORD last_error = GetLastError();
        error = {is_missing_error(last_error) ? StoreStatus::not_found : StoreStatus::io_error,
                 windows_error("opening settings directory", last_error)};
        return false;
    }

    UniqueHandle opened(raw);
    BY_HANDLE_FILE_INFORMATION information{};
    if (GetFileInformationByHandle(raw, &information) == FALSE) {
        const DWORD last_error = GetLastError();
        error = {StoreStatus::io_error, windows_error("inspecting settings directory", last_error)};
        return false;
    }
    if ((information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U ||
        (information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0U) {
        error = {StoreStatus::unsafe_path, "settings directory is a reparse point or is not a directory"};
        return false;
    }

    handle.reset(opened.release());
    return true;
}

bool ensure_directory(const std::filesystem::path& path, bool create, OperationError& error,
                      UniqueHandle* keep_open = nullptr) {
    const std::wstring native = win32_path(path);
    DWORD attributes = GetFileAttributesW(native.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const DWORD query_error = GetLastError();
        if (!is_missing_error(query_error)) {
            error = {StoreStatus::io_error, windows_error("checking settings directory", query_error)};
            return false;
        }
        if (!create) {
            error = {StoreStatus::not_found, "settings directory does not exist"};
            return false;
        }
        if (CreateDirectoryW(native.c_str(), nullptr) == FALSE) {
            const DWORD create_error = GetLastError();
            if (create_error != ERROR_ALREADY_EXISTS) {
                error = {StoreStatus::io_error, windows_error("creating settings directory", create_error)};
                return false;
            }
        }
        attributes = GetFileAttributesW(native.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            const DWORD last_error = GetLastError();
            error = {StoreStatus::io_error, windows_error("checking created settings directory", last_error)};
            return false;
        }
    }
    if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U ||
        (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0U) {
        error = {StoreStatus::unsafe_path, "settings path component is a reparse point or is not a directory"};
        return false;
    }

    UniqueHandle verified;
    if (!open_directory_no_follow(path, verified, error)) {
        return false;
    }
    if (keep_open != nullptr) {
        *keep_open = std::move(verified);
    }
    return true;
}

bool prepare_host_root(const std::filesystem::path& requested, bool create,
                       std::filesystem::path& normalized, UniqueHandle& root_handle,
                       std::vector<UniqueHandle>& root_path_guards,
                       BY_HANDLE_FILE_INFORMATION& root_information, OperationError& error) {
    if (!normalize_host_root(requested, normalized, error)) {
        return false;
    }

    std::filesystem::path current = normalized.root_path();
    UniqueHandle root_component;
    if (!ensure_directory(current, false, error, &root_component)) {
        return false;
    }
    root_path_guards.push_back(std::move(root_component));
    const std::filesystem::path relative = normalized.relative_path();
    for (const auto& component : relative) {
        current /= component;
        UniqueHandle component_handle;
        if (!ensure_directory(current, create, error, &component_handle)) {
            return false;
        }
        root_path_guards.push_back(std::move(component_handle));
    }

    if (!open_directory_no_follow(normalized, root_handle, error)) {
        return false;
    }
    if (GetFileInformationByHandle(root_handle.get(), &root_information) == FALSE) {
        const DWORD last_error = GetLastError();
        error = {StoreStatus::io_error, windows_error("reading host settings root identity", last_error)};
        return false;
    }
    return true;
}

std::wstring hexadecimal(DWORD value) {
    constexpr wchar_t kDigits[] = L"0123456789abcdef";
    std::wstring result(8U, L'0');
    for (std::size_t index = 0U; index < result.size(); ++index) {
        const unsigned int shift = static_cast<unsigned int>((result.size() - index - 1U) * 4U);
        result[index] = kDigits[(value >> shift) & 0x0fU];
    }
    return result;
}

std::wstring root_mutex_name(const BY_HANDLE_FILE_INFORMATION& information) {
    return L"Global\\ESMSettingsStore_v1_" + hexadecimal(information.dwVolumeSerialNumber) +
           hexadecimal(information.nFileIndexHigh) + hexadecimal(information.nFileIndexLow);
}

class RootMutex final {
public:
    RootMutex() = default;
    RootMutex(const RootMutex&) = delete;
    RootMutex& operator=(const RootMutex&) = delete;

    ~RootMutex() {
        if (acquired_) {
            ReleaseMutex(handle_.get());
        }
    }

    bool acquire(const BY_HANDLE_FILE_INFORMATION& root_information, OperationError& error) {
        const std::wstring name = root_mutex_name(root_information);
        HANDLE raw = CreateMutexW(nullptr, FALSE, name.c_str());
        if (raw == nullptr) {
            const DWORD last_error = GetLastError();
            error = {StoreStatus::io_error, windows_error("creating settings root lock", last_error)};
            return false;
        }
        handle_.reset(raw);

        const DWORD wait = WaitForSingleObject(raw, INFINITE);
        if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED) {
            const DWORD last_error = wait == WAIT_FAILED ? GetLastError() : ERROR_GEN_FAILURE;
            error = {StoreStatus::io_error, windows_error("waiting for settings root lock", last_error)};
            return false;
        }
        acquired_ = true;
        return true;
    }

private:
    UniqueHandle handle_;
    bool acquired_ = false;
};

std::wstring encoded_id_component(std::string_view id, wchar_t prefix) {
    std::wstring result;
    result.reserve(id.size() + 2U);
    result.push_back(prefix);
    result.push_back(L'_');
    for (const unsigned char character : id) {
        // Canonical IDs are lowercase ASCII. Mapping '.' to '!' is reversible,
        // avoids Win32's trailing-dot normalization, and cannot collide because
        // '!' is outside the canonical grammar.
        result.push_back(character == static_cast<unsigned char>('.') ? L'!' : static_cast<wchar_t>(character));
    }
    return result;
}

bool prepare_record_path(const std::filesystem::path& root, const SettingsKey& key, bool create,
                         std::filesystem::path& record, UniqueHandle& storage_handle,
                         UniqueHandle& module_handle, OperationError& error) {
    const std::filesystem::path storage_directory = root / L".esm_settings";
    if (!ensure_directory(storage_directory, create, error, &storage_handle)) {
        return false;
    }

    const std::filesystem::path module_directory = storage_directory / encoded_id_component(key.module_id, L'm');
    if (!ensure_directory(module_directory, create, error, &module_handle)) {
        return false;
    }

    record = module_directory / (encoded_id_component(key.page_id, L'p') + L".json");
    return true;
}

bool inspect_regular_file(const std::filesystem::path& path, bool missing_allowed,
                          OperationError& error) {
    const std::wstring native = win32_path(path);
    const DWORD attributes = GetFileAttributesW(native.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const DWORD last_error = GetLastError();
        if (missing_allowed && is_missing_error(last_error)) {
            return true;
        }
        error = {is_missing_error(last_error) ? StoreStatus::not_found : StoreStatus::io_error,
                 windows_error("checking settings document", last_error)};
        return false;
    }
    if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U ||
        (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0U) {
        error = {StoreStatus::unsafe_path, "settings document is a reparse point or is not a regular file"};
        return false;
    }

    HANDLE raw = CreateFileW(native.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (raw == INVALID_HANDLE_VALUE) {
        const DWORD last_error = GetLastError();
        error = {is_missing_error(last_error) ? StoreStatus::not_found : StoreStatus::io_error,
                 windows_error("opening settings document metadata", last_error)};
        return false;
    }
    UniqueHandle handle(raw);
    BY_HANDLE_FILE_INFORMATION information{};
    if (GetFileInformationByHandle(raw, &information) == FALSE) {
        const DWORD last_error = GetLastError();
        error = {StoreStatus::io_error, windows_error("inspecting settings document", last_error)};
        return false;
    }
    if ((information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U ||
        (information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0U) {
        error = {StoreStatus::unsafe_path, "settings document is a reparse point or is not a regular file"};
        return false;
    }
    return true;
}

bool read_file_bytes(const std::filesystem::path& path, std::string& bytes,
                     OperationError& error) {
    if (!inspect_regular_file(path, false, error)) {
        return false;
    }
    const std::wstring native = win32_path(path);
    HANDLE raw = CreateFileW(native.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (raw == INVALID_HANDLE_VALUE) {
        const DWORD last_error = GetLastError();
        error = {is_missing_error(last_error) ? StoreStatus::not_found : StoreStatus::io_error,
                 windows_error("reading settings document", last_error)};
        return false;
    }
    UniqueHandle handle(raw);

    BY_HANDLE_FILE_INFORMATION information{};
    if (GetFileInformationByHandle(raw, &information) == FALSE) {
        const DWORD last_error = GetLastError();
        error = {StoreStatus::io_error, windows_error("checking settings document", last_error)};
        return false;
    }
    if ((information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U ||
        (information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0U) {
        error = {StoreStatus::unsafe_path, "settings document is a reparse point or is not a regular file"};
        return false;
    }

    LARGE_INTEGER file_size{};
    if (GetFileSizeEx(raw, &file_size) == FALSE) {
        const DWORD last_error = GetLastError();
        error = {StoreStatus::io_error, windows_error("sizing settings document", last_error)};
        return false;
    }
    if (file_size.QuadPart < 0 ||
        static_cast<unsigned long long>(file_size.QuadPart) > static_cast<unsigned long long>(kMaxStoredDocumentBytes)) {
        error = {StoreStatus::corrupt_data, "stored settings document exceeds the supported size bound"};
        return false;
    }

    bytes.assign(static_cast<std::size_t>(file_size.QuadPart), '\0');
    std::size_t offset = 0U;
    while (offset < bytes.size()) {
        const std::size_t remaining = bytes.size() - offset;
        const DWORD request = static_cast<DWORD>(std::min<std::size_t>(remaining, std::numeric_limits<DWORD>::max()));
        DWORD read = 0U;
        if (ReadFile(raw, bytes.data() + offset, request, &read, nullptr) == FALSE) {
            const DWORD last_error = GetLastError();
            error = {StoreStatus::io_error, windows_error("reading settings document", last_error)};
            return false;
        }
        if (read == 0U) {
            error = {StoreStatus::io_error, "settings document ended before its reported size"};
            return false;
        }
        offset += static_cast<std::size_t>(read);
    }
    return true;
}

class StrictJsonSax final : public nlohmann::json_sax<Json> {
public:
    using Base = nlohmann::json_sax<Json>;
    using string_t = typename Base::string_t;
    using binary_t = typename Base::binary_t;
    using number_integer_t = typename Base::number_integer_t;
    using number_unsigned_t = typename Base::number_unsigned_t;
    using number_float_t = typename Base::number_float_t;

    bool null() override { return count_node(); }
    bool boolean(bool) override { return count_node(); }
    bool number_integer(number_integer_t) override { return count_node(); }
    bool number_unsigned(number_unsigned_t) override { return count_node(); }
    bool number_float(number_float_t value, const string_t&) override {
        return std::isfinite(value) && count_node();
    }
    bool string(string_t&) override { return count_node(); }
    bool binary(binary_t&) override { malformed_ = true; return false; }

    bool start_object(std::size_t) override {
        if (!enter_container()) {
            return false;
        }
        containers_.push_back(Container{true, {}});
        return true;
    }

    bool key(string_t& value) override {
        if (!count_node() || containers_.empty() || !containers_.back().is_object) {
            malformed_ = true;
            return false;
        }
        if (!containers_.back().keys.insert(value).second) {
            malformed_ = true;
            return false;
        }
        return true;
    }

    bool end_object() override {
        if (containers_.empty() || !containers_.back().is_object) {
            malformed_ = true;
            return false;
        }
        containers_.pop_back();
        --depth_;
        return true;
    }

    bool start_array(std::size_t) override {
        if (!enter_container()) {
            return false;
        }
        containers_.push_back(Container{false, {}});
        return true;
    }

    bool end_array() override {
        if (containers_.empty() || containers_.back().is_object) {
            malformed_ = true;
            return false;
        }
        containers_.pop_back();
        --depth_;
        return true;
    }

    bool parse_error(std::size_t, const std::string&, const nlohmann::detail::exception&) override {
        malformed_ = true;
        return false;
    }

    bool malformed() const noexcept { return malformed_; }

private:
    struct Container final {
        bool is_object;
        std::unordered_set<std::string> keys;
    };

    bool count_node() noexcept {
        ++node_count_;
        if (depth_ > kMaxStoredDocumentJsonDepth || node_count_ > kMaxStoredDocumentJsonNodes) {
            malformed_ = true;
            return false;
        }
        return true;
    }

    bool enter_container() noexcept {
        if (!count_node() || depth_ >= kMaxStoredDocumentJsonDepth) {
            malformed_ = true;
            return false;
        }
        ++depth_;
        return true;
    }

    std::vector<Container> containers_;
    std::size_t node_count_ = 0U;
    std::size_t depth_ = 0U;
    bool malformed_ = false;
};

bool accumulate_string_budget(std::size_t length, std::size_t& budget) noexcept {
    if (length > kMaxStoredDocumentBytes || length > (kMaxStoredDocumentBytes - 2U) / 6U) {
        return false;
    }
    const std::size_t escaped_bound = length * 6U + 2U;
    if (escaped_bound > kMaxStoredDocumentBytes - budget) {
        return false;
    }
    budget += escaped_bound;
    return true;
}

bool validate_json_values(const Json& values, std::string& error) {
    if (!values.is_object()) {
        error = "settings field values must be a JSON object";
        return false;
    }

    struct PendingValue final {
        const Json* value;
        std::size_t depth;
    };
    std::vector<PendingValue> pending;
    pending.push_back(PendingValue{&values, 0U});
    std::size_t node_count = 0U;
    std::size_t string_budget = 0U;

    while (!pending.empty()) {
        const PendingValue current = pending.back();
        pending.pop_back();
        ++node_count;
        if (node_count > kMaxJsonNodes || current.depth > kMaxJsonDepth) {
            error = "settings values exceed the supported nesting or node bound";
            return false;
        }

        const Json& value = *current.value;
        if (value.is_discarded() || value.is_binary()) {
            error = "settings values must use standard declarative JSON types";
            return false;
        }
        if (value.is_number_float() && !std::isfinite(value.get<double>())) {
            error = "settings values cannot contain NaN or infinity";
            return false;
        }
        if (value.is_string()) {
            const auto& string_value = value.get_ref<const Json::string_t&>();
            if (!accumulate_string_budget(string_value.size(), string_budget)) {
                error = "settings strings exceed the supported encoded size bound";
                return false;
            }
        } else if (value.is_object()) {
            for (auto it = value.begin(); it != value.end(); ++it) {
                if (!accumulate_string_budget(it.key().size(), string_budget)) {
                    error = "settings keys exceed the supported encoded size bound";
                    return false;
                }
                ++node_count;
                if (node_count > kMaxJsonNodes) {
                    error = "settings values exceed the supported node bound";
                    return false;
                }
                pending.push_back(PendingValue{&it.value(), current.depth + 1U});
            }
        } else if (value.is_array()) {
            for (const auto& item : value) {
                pending.push_back(PendingValue{&item, current.depth + 1U});
            }
        }
    }

    return true;
}

bool read_unsigned(const Json& document, std::string_view key, std::uint64_t maximum,
                   std::uint64_t& value) {
    const auto iterator = document.find(std::string(key));
    if (iterator == document.end()) {
        return false;
    }
    if (iterator->is_number_unsigned()) {
        value = iterator->get<std::uint64_t>();
        return value <= maximum;
    }
    if (iterator->is_number_integer()) {
        const std::int64_t signed_value = iterator->get<std::int64_t>();
        if (signed_value < 0) {
            return false;
        }
        value = static_cast<std::uint64_t>(signed_value);
        return value <= maximum;
    }
    return false;
}

ReadResult parse_stored_document(std::string_view bytes, const SettingsKey& key) {
    StrictJsonSax validator;
    if (!Json::sax_parse(bytes.begin(), bytes.end(), &validator) || validator.malformed()) {
        return read_failure(StoreStatus::corrupt_data, "stored settings JSON is malformed, duplicated, or exceeds its parser bounds");
    }

    Json document;
    try {
        document = Json::parse(bytes.begin(), bytes.end());
    } catch (const Json::exception&) {
        return read_failure(StoreStatus::corrupt_data, "stored settings JSON could not be parsed");
    }

    if (!document.is_object()) {
        return read_failure(StoreStatus::corrupt_data, "stored settings document must be a JSON object");
    }

    std::uint64_t format_version = 0U;
    if (!read_unsigned(document, "format_version", std::numeric_limits<std::uint32_t>::max(), format_version)) {
        return read_failure(StoreStatus::corrupt_data, "stored settings format metadata is invalid");
    }
    if (format_version != kStorageFormatVersion) {
        return read_failure(StoreStatus::unsupported_format, "stored settings use an unsupported storage format version");
    }

    if (document.size() != kStorageEnvelopeMemberCount || !document.contains("module_id") || !document["module_id"].is_string() ||
        !document.contains("page_id") || !document["page_id"].is_string() ||
        !document.contains("schema_version") || !document.contains("revision") || !document.contains("values")) {
        return read_failure(StoreStatus::corrupt_data, "stored settings metadata is incomplete or contains unknown members");
    }

    const std::string module_id = document["module_id"].get<std::string>();
    const std::string page_id = document["page_id"].get<std::string>();
    if (!is_canonical_settings_id(module_id) || !is_canonical_settings_id(page_id) ||
        module_id != key.module_id || page_id != key.page_id) {
        return read_failure(StoreStatus::corrupt_data, "stored settings identity does not match the requested module and page");
    }

    std::uint64_t schema_version = 0U;
    std::uint64_t revision = 0U;
    if (!read_unsigned(document, "schema_version", std::numeric_limits<std::uint32_t>::max(), schema_version) ||
        schema_version == 0U || !read_unsigned(document, "revision", std::numeric_limits<std::uint64_t>::max(), revision) ||
        revision == 0U || !document["values"].is_object()) {
        return read_failure(StoreStatus::corrupt_data, "stored settings schema, revision, or value metadata is invalid");
    }

    std::string value_error;
    if (!validate_json_values(document["values"], value_error)) {
        return read_failure(StoreStatus::corrupt_data, "stored settings values are invalid: " + value_error);
    }

    StoredSettings stored;
    stored.schema_version = static_cast<std::uint32_t>(schema_version);
    stored.revision = revision;
    stored.values = document["values"];
    return read_success(std::move(stored));
}

ReadResult read_record(const std::filesystem::path& record, const SettingsKey& key) {
    std::string bytes;
    OperationError error;
    if (!read_file_bytes(record, bytes, error)) {
        return read_failure(error.status, std::move(error.detail));
    }
    return parse_stored_document(bytes, key);
}

bool serialize_settings(const SettingsKey& key, const StoredSettings& settings,
                        std::string& bytes, std::string& error) {
    if (settings.schema_version == 0U) {
        error = "settings schema version must be positive";
        return false;
    }
    if (!validate_json_values(settings.values, error)) {
        return false;
    }

    try {
        Json document = Json::object();
        document["format_version"] = kStorageFormatVersion;
        document["module_id"] = key.module_id;
        document["page_id"] = key.page_id;
        document["schema_version"] = settings.schema_version;
        document["revision"] = settings.revision;
        document["values"] = settings.values;
        bytes = document.dump(-1, ' ', false, Json::error_handler_t::strict);
    } catch (const Json::exception&) {
        error = "settings values must contain valid UTF-8 JSON strings";
        return false;
    }

    if (bytes.size() > kMaxStoredDocumentBytes) {
        error = "serialized settings document exceeds the supported size bound";
        return false;
    }
    return true;
}

bool write_all(HANDLE handle, std::string_view bytes, OperationError& error) {
    std::size_t offset = 0U;
    while (offset < bytes.size()) {
        const std::size_t remaining = bytes.size() - offset;
        const DWORD request = static_cast<DWORD>(std::min<std::size_t>(remaining, std::numeric_limits<DWORD>::max()));
        DWORD written = 0U;
        if (WriteFile(handle, bytes.data() + offset, request, &written, nullptr) == FALSE) {
            const DWORD last_error = GetLastError();
            error = {StoreStatus::io_error, windows_error("writing temporary settings document", last_error)};
            return false;
        }
        if (written == 0U) {
            error = {StoreStatus::io_error, "temporary settings document write made no progress"};
            return false;
        }
        offset += static_cast<std::size_t>(written);
    }
    return true;
}

StoreResult atomic_publish(const std::filesystem::path& record, std::string_view bytes,
                           bool replace_existing) {
    OperationError error;
    if (replace_existing) {
        if (!inspect_regular_file(record, true, error)) {
            return store_failure(error.status, std::move(error.detail));
        }
    } else {
        const std::wstring native_record = win32_path(record);
        const DWORD attributes = GetFileAttributesW(native_record.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES) {
            if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U ||
                (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0U) {
                return store_failure(StoreStatus::unsafe_path,
                    "settings document is a reparse point or is not a regular file");
            }
            return store_failure(StoreStatus::conflict,
                "settings document already exists and will not be replaced");
        }
        const DWORD last_error = GetLastError();
        if (!is_missing_error(last_error)) {
            return store_failure(StoreStatus::io_error,
                windows_error("checking settings document before creation", last_error));
        }
    }

    const std::wstring filename = record.filename().native();
    std::filesystem::path temporary;
    UniqueHandle temporary_file;
    bool created = false;
    for (std::size_t attempt = 0U; attempt < 8U; ++attempt) {
        const std::uint64_t sequence = g_temp_file_sequence.fetch_add(1U, std::memory_order_relaxed);
        const std::wstring temporary_name = filename + L".tmp-" + std::to_wstring(GetCurrentProcessId()) +
                                            L"-" + std::to_wstring(sequence);
        temporary = record.parent_path() / temporary_name;
        const std::wstring native_temporary = win32_path(temporary);
        HANDLE raw = CreateFileW(native_temporary.c_str(), GENERIC_WRITE, 0U, nullptr, CREATE_NEW,
                                 FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_WRITE_THROUGH | FILE_FLAG_OPEN_REPARSE_POINT,
                                 nullptr);
        if (raw != INVALID_HANDLE_VALUE) {
            temporary_file.reset(raw);
            created = true;
            break;
        }
        const DWORD last_error = GetLastError();
        if (last_error != ERROR_FILE_EXISTS && last_error != ERROR_ALREADY_EXISTS) {
            return store_failure(StoreStatus::io_error, windows_error("creating temporary settings document", last_error));
        }
    }
    if (!created) {
        return store_failure(StoreStatus::io_error, "could not allocate a unique temporary settings document name");
    }

    if (!write_all(temporary_file.get(), bytes, error)) {
        const std::wstring native_temporary = win32_path(temporary);
        temporary_file.reset();
        DeleteFileW(native_temporary.c_str());
        return store_failure(error.status, std::move(error.detail));
    }
    if (FlushFileBuffers(temporary_file.get()) == FALSE) {
        const DWORD last_error = GetLastError();
        const std::wstring native_temporary = win32_path(temporary);
        temporary_file.reset();
        DeleteFileW(native_temporary.c_str());
        return store_failure(StoreStatus::io_error, windows_error("flushing temporary settings document", last_error));
    }
    temporary_file.reset();

    if (!inspect_regular_file(record, true, error)) {
        const std::wstring native_temporary = win32_path(temporary);
        DeleteFileW(native_temporary.c_str());
        return store_failure(error.status, std::move(error.detail));
    }

    const std::wstring native_temporary = win32_path(temporary);
    const std::wstring native_record = win32_path(record);
    DWORD move_flags = MOVEFILE_WRITE_THROUGH;
    if (replace_existing) {
        move_flags |= MOVEFILE_REPLACE_EXISTING;
    }
    if (MoveFileExW(native_temporary.c_str(), native_record.c_str(), move_flags) == FALSE) {
        const DWORD last_error = GetLastError();
        DeleteFileW(native_temporary.c_str());
        if (!replace_existing &&
            (last_error == ERROR_ALREADY_EXISTS || last_error == ERROR_FILE_EXISTS)) {
            return store_failure(StoreStatus::conflict,
                "settings document was created concurrently and will not be replaced");
        }
        return store_failure(StoreStatus::io_error, windows_error("atomically replacing settings document", last_error));
    }
    return StoreResult{StoreStatus::ok, {}};
}

StoreResult atomic_replace(const std::filesystem::path& record, std::string_view bytes) {
    return atomic_publish(record, bytes, true);
}

StoreResult atomic_create(const std::filesystem::path& record, std::string_view bytes) {
    return atomic_publish(record, bytes, false);
}

StoreResult copy_read_failure(const ReadResult& read) {
    return store_failure(read.status, read.error);
}

}  // namespace

bool is_canonical_settings_id(std::string_view value) noexcept {
    if (value.empty() || value.size() > kMaxSettingsIdBytes) {
        return false;
    }
    const auto first = static_cast<unsigned char>(value.front());
    if (!((first >= 'a' && first <= 'z') || (first >= '0' && first <= '9'))) {
        return false;
    }
    return std::all_of(value.begin() + 1, value.end(), [](unsigned char character) {
        return (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9') ||
               character == '.' || character == '_' || character == '-';
    });
}

SettingsStore::SettingsStore(std::filesystem::path host_root)
    : host_root_(std::move(host_root)) {}

ReadResult SettingsStore::read(const SettingsKey& key) const {
    if (!is_canonical_settings_id(key.module_id) || !is_canonical_settings_id(key.page_id)) {
        return read_failure(StoreStatus::invalid_key, "module and page IDs must be canonical lowercase ASCII IDs");
    }

    try {
        std::filesystem::path root;
        UniqueHandle root_handle;
        std::vector<UniqueHandle> root_path_guards;
        BY_HANDLE_FILE_INFORMATION root_information{};
        OperationError error;
        if (!prepare_host_root(host_root_, false, root, root_handle, root_path_guards, root_information, error)) {
            return read_failure(error.status, std::move(error.detail));
        }

        RootMutex lock;
        if (!lock.acquire(root_information, error)) {
            return read_failure(error.status, std::move(error.detail));
        }

        UniqueHandle storage_handle;
        UniqueHandle module_handle;
        std::filesystem::path record;
        if (!prepare_record_path(root, key, false, record, storage_handle, module_handle, error)) {
            return read_failure(error.status, std::move(error.detail));
        }
        return read_record(record, key);
    } catch (const std::exception& exception) {
        return read_failure(StoreStatus::io_error, bounded_error(std::string("settings read failed: ") + exception.what()));
    } catch (...) {
        return read_failure(StoreStatus::io_error, "settings read failed with an unknown error");
    }
}

StoreResult SettingsStore::create_if_absent(const SettingsKey& key,
                                            const StoredSettings& initial) const {
    if (!is_canonical_settings_id(key.module_id) || !is_canonical_settings_id(key.page_id)) {
        return store_failure(StoreStatus::invalid_key,
            "module and page IDs must be canonical lowercase ASCII IDs");
    }
    if (initial.revision == 0U) {
        return store_failure(StoreStatus::invalid_value,
            "initial settings revision must be positive");
    }

    try {
        std::string initial_bytes;
        std::string validation_error;
        if (!serialize_settings(key, initial, initial_bytes, validation_error)) {
            return store_failure(StoreStatus::invalid_value, std::move(validation_error));
        }

        std::filesystem::path root;
        UniqueHandle root_handle;
        std::vector<UniqueHandle> root_path_guards;
        BY_HANDLE_FILE_INFORMATION root_information{};
        OperationError error;
        if (!prepare_host_root(host_root_, true, root, root_handle, root_path_guards,
                               root_information, error)) {
            return store_failure(error.status, std::move(error.detail));
        }

        RootMutex lock;
        if (!lock.acquire(root_information, error)) {
            return store_failure(error.status, std::move(error.detail));
        }

        UniqueHandle storage_handle;
        UniqueHandle module_handle;
        std::filesystem::path record;
        if (!prepare_record_path(root, key, true, record, storage_handle,
                                 module_handle, error)) {
            return store_failure(error.status, std::move(error.detail));
        }

        return atomic_create(record, initial_bytes);
    } catch (const std::exception& exception) {
        return store_failure(StoreStatus::io_error,
            bounded_error(std::string("settings create_if_absent failed: ") + exception.what()));
    } catch (...) {
        return store_failure(StoreStatus::io_error,
            "settings create_if_absent failed with an unknown error");
    }
}

StoreResult SettingsStore::compare_exchange(const SettingsKey& key, std::uint64_t expected_revision,
                                            const StoredSettings& next) const {
    if (!is_canonical_settings_id(key.module_id) || !is_canonical_settings_id(key.page_id)) {
        return store_failure(StoreStatus::invalid_key, "module and page IDs must be canonical lowercase ASCII IDs");
    }

    try {
        std::string next_bytes;
        std::string validation_error;
        if (!serialize_settings(key, next, next_bytes, validation_error)) {
            return store_failure(StoreStatus::invalid_value, std::move(validation_error));
        }

        std::filesystem::path root;
        UniqueHandle root_handle;
        std::vector<UniqueHandle> root_path_guards;
        BY_HANDLE_FILE_INFORMATION root_information{};
        OperationError error;
        if (!prepare_host_root(host_root_, true, root, root_handle, root_path_guards, root_information, error)) {
            return store_failure(error.status, std::move(error.detail));
        }

        RootMutex lock;
        if (!lock.acquire(root_information, error)) {
            return store_failure(error.status, std::move(error.detail));
        }

        UniqueHandle storage_handle;
        UniqueHandle module_handle;
        std::filesystem::path record;
        if (!prepare_record_path(root, key, true, record, storage_handle, module_handle, error)) {
            return store_failure(error.status, std::move(error.detail));
        }

        const ReadResult current = read_record(record, key);
        std::uint64_t current_revision = 0U;
        if (current.status == StoreStatus::ok && current.value.has_value()) {
            current_revision = current.value->revision;
        } else if (current.status != StoreStatus::not_found) {
            return copy_read_failure(current);
        }

        if (current_revision != expected_revision) {
            std::string detail = "settings revision conflict: expected ";
            detail += std::to_string(expected_revision);
            detail += ", current ";
            detail += std::to_string(current_revision);
            return store_failure(StoreStatus::conflict, std::move(detail));
        }
        if (current_revision == std::numeric_limits<std::uint64_t>::max()) {
            return store_failure(StoreStatus::revision_overflow, "settings revision cannot advance beyond UINT64_MAX");
        }
        if (next.revision != current_revision + 1U) {
            return store_failure(StoreStatus::invalid_value, "next settings revision must be exactly current revision plus one");
        }

        return atomic_replace(record, next_bytes);
    } catch (const std::exception& exception) {
        return store_failure(StoreStatus::io_error,
                             bounded_error(std::string("settings compare_exchange failed: ") + exception.what()));
    } catch (...) {
        return store_failure(StoreStatus::io_error, "settings compare_exchange failed with an unknown error");
    }
}

}  // namespace esm::settings
