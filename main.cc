#include <bitset>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <random>
#include <span>
#include <variant>

#include <Windows.h>
#include <lmcons.h>

namespace ch = std::chrono;
namespace fs = std::filesystem;
namespace rg = std::ranges;

// Types:
// Argument:    -q --quiet
// Parameter:   -n 5 --count 5
// Command:     --help
enum class OptionType
{
    Argument,
    Parameter,
    Command
};

struct Option
{
    enum class ParamType
    {
        Int,
        String,
        StringList
    };

    static Option NewParam(ParamType type, std::string_view name, std::string_view short_name = "")
    {
        Option opt;
        opt.type = OptionType::Parameter;
        opt.param_type = type;
        opt.name = name;
        opt.short_name = name;
        return opt;
    }

    static Option NewArg(std::string_view name, std::string_view short_name = "")
    {
        Option opt;
        opt.type = OptionType::Argument;
        opt.name = name;
        opt.short_name = name;
        return opt;
    }

    template<class T>
    const T& GetParam() const
    {
        return std::get<T>(param);
    }

    // General
    std::string short_name;
    std::string name;
    OptionType type;

    // Parameters
    ParamType param_type;
    std::variant<int, std::string, std::vector<std::string>> param;
};

std::vector<Option> g_options;
static bool g_quiet;
static int g_copied;

static std::vector<std::string> SplitString(const std::string& str, char delim = ' ')
{
    std::vector<std::string> res{};
    res.push_back(str.substr(0, str.find(delim)));
    for (size_t i = 0, pos = 0; i < str.length(); i++)
    {
        if (str[i] == delim)
        {
            pos = i + 1;
            res.push_back(str.substr(pos, str.find(delim, pos) - pos));
        }
    }
    return res;
}

static void CmdFillParam(Option* opt, char* arg)
{
    switch (opt->param_type)
    {
    case Option::ParamType::Int:    opt->param.emplace<int>(std::stoi(arg)); break;
    case Option::ParamType::String: opt->param.emplace<std::string>(arg);    break;
    case Option::ParamType::StringList:
        // not robust
        opt->param.emplace<std::vector<std::string>>(SplitString(arg, ','));
        break;
    }
}

static bool CmdLookup(int argc, char* argv[], Option* opt)
{
    size_t i = 0;
    for (std::string_view s : std::span<char*>(argv, ( size_t )argc))
    {
        for (size_t j = 0; j < g_options.size(); j++)
        {
            if ((!opt->short_name.empty() && opt->short_name == argv[i])
               || opt->name == argv[i])
            {
                if (opt->type == OptionType::Parameter)
                {
                    _ASSERT(argc >= i + 1);
                    CmdFillParam(opt, argv[i + 1]);
                }
                return true;
            }
        }
        i++;
    }
    return false;
}

static Option& CmdAddParam(std::string_view short_name, std::string_view name, Option::ParamType type)
{
    return g_options.emplace_back(Option::NewParam(type, short_name, name));
}

static Option& CmdAddArg(std::string_view short_name, std::string_view name)
{
    return g_options.emplace_back(Option::NewArg(short_name, name));
}

#define KB(x) (1000u * x)
#define MB(x) (1000u * KB(x))
#define GB(x) (1000u * MB(x))

struct FileFilter
{
    std::uintmax_t max_size = MB(5);
    std::vector<std::string> valid_extensions = {
        ".txt",
        ".docx",
        ".pptx",
        ".pdf",
        ".csv"
    };
};

static char GetPathDriveLetter(const fs::path& path)
{
    return path.string()[0];
}

static std::vector<std::wstring> CollectDrives(bool skip_current_drive)
{
    std::vector<std::wstring> drives{};

    // Get the current drive letter in case we want to skip it later.
    const auto cur_drive = GetPathDriveLetter(fs::current_path());

    // Collect all drive letters in a bitmask.
    std::bitset<32> bits = ::GetLogicalDrives();
    for (size_t i = 0; i < bits.size(); i++)
    {
        if (bits.test(i)) // In use?
        {
            const auto drive_letter = wchar_t(L'A' + i);
            if (skip_current_drive && drive_letter == cur_drive)
                continue;

            // Build a drive string and check the type.
            // We only care about hard disks and thumb drives.
            const auto drive = std::format(L"{}:\\", drive_letter);
            switch (::GetDriveType(drive.c_str()))
            {
            case DRIVE_FIXED:
            case DRIVE_REMOVABLE:
                if (!g_quiet) std::wcout << L"Found drive " << drive << '\n';
                drives.push_back(drive);
                break;
            default:
                break;
            }
        }
    }

    return drives;
}

static int RandomInt(int min, int max)
{
    static std::mt19937 random_engine{ std::random_device{}() };
    return std::uniform_int_distribution{ min, max }(random_engine);
}

template<class... Args>
static void LogMessage(std::ofstream& log_file, const char* fmt, const Args&&... args)
{
    if (g_quiet)
        return;

    const auto time = ch::system_clock::to_time_t(ch::system_clock::now());
    std::tm tm{};
    char tm_str[9]{};
    localtime_s(&tm, &time);
    std::strftime(tm_str, sizeof(tm_str), "%T", &tm);

    const auto msg = std::vformat(fmt, std::make_format_args(std::forward<decltype(args)>(args)...));
    log_file << tm_str << ' ' << msg << '\n';
#ifdef _DEBUG
    std::cout << tm_str << ' ' << msg << '\n';
#endif
}

static bool ShouldSkipDirectory(std::wstring_view path_string)
{
    // If the directory starts with a dot or dollar sign,
    // it's some system or config directory we don't care about.
    //
    // Windows and AppData are skipped just because they take long
    // to iterate through and contain irrelevant data anyways.

    auto last_dir = wcsrchr(path_string.data(), '\\');
    if (!last_dir)
        return true;

    return (wcsstr(last_dir, L"AppData")
        || wcsstr(last_dir, L"Windows")
        || last_dir[1] == L'.'
        || last_dir[1] == L'$');
}

static fs::path GenerateTargetPath(const fs::path& target_root, char drive_letter)
{
    DWORD length = UNLEN;
    char user_name[UNLEN]{};
    if (::GetUserNameA(user_name, &length))
        return target_root / std::format("User_{}\\{}", user_name, drive_letter);
    else
        return target_root / std::format("User_u{}\\{}", RandomInt(10000, 99999), drive_letter);
}

static void RecurseCopyFiles(std::wstring_view source_root, const fs::path& target_root, const FileFilter& filter)
{
    // Create a new directory for this user and drive. (Format: <target_root>\user_<USERNAME>\<DRIVE>\)
    const auto target = GenerateTargetPath(target_root, GetPathDriveLetter(source_root));
    fs::create_directories(target);

    // Log file goes here as well.
    std::ofstream log_file{ target / "results.txt", std::ios::out | std::ios::trunc };

    auto iter = fs::recursive_directory_iterator(source_root, fs::directory_options::skip_permission_denied);
    while (fs::begin(iter) != fs::end(iter))
    {
        auto entry = *(iter++);
        try
        {
            if (!entry.exists())
                continue;

            if (entry.is_directory())
            {
                if (ShouldSkipDirectory(entry.path().wstring()))
                {
                    iter.pop();
                    continue;
                }
            }
            else if (entry.is_regular_file())
            {
                // This is a potentially interesting file. Check if it satisfies the filter conditions.
                auto& file_path = entry.path();
                if (file_path.has_extension()
                    && rg::find(filter.valid_extensions, file_path.extension().string()) != filter.valid_extensions.end()
                    && entry.file_size() <= filter.max_size)
                {
                    LogMessage(log_file, "Copying: {}", file_path.string());
                    // Build the final path name:
                    // The source path without the drive letter and filename, appended to <target>.
                    // Example: C:\Users\admin\Documents\x.txt is transformed into
                    //          [D:\Data\user_admin\C\][Users\user\Documents]
                    //          where <target_root> == D:\Data.
                    const auto target_path = target / file_path.relative_path().remove_filename();
                    fs::create_directories(target_path);
                    fs::copy(file_path, target_path, fs::copy_options::update_existing);
                    g_copied++;
                }
            }
        }
        catch (const std::exception& ex)
        {
            LogMessage(log_file, "Exception: {}", ex.what());
            continue;
        }
    }
}

int main(int argc, char* argv[])
{
    FileFilter filter;

    auto size_option = CmdAddParam("-s", "--size", Option::ParamType::Int);
    if (CmdLookup(argc, argv, &size_option))
        filter.max_size = KB(size_option.GetParam<int>()); // assumed to be in KB!!!

    auto extensions_option = CmdAddParam("-e", "--extensions", Option::ParamType::StringList);
    if (CmdLookup(argc, argv, &extensions_option))
        filter.valid_extensions = extensions_option.GetParam<std::vector<std::string>>();

    std::string target = "D:\\Data";
    auto target_option = CmdAddParam("-t", "--target", Option::ParamType::String);
    if (CmdLookup(argc, argv, &target_option))
        target = target_option.GetParam<std::string>();

    auto quiet_option = CmdAddArg("-q", "--quiet");
    g_quiet = CmdLookup(argc, argv, &quiet_option);

    if (!g_quiet)
    {
        std::cout << "Max file size: " << (filter.max_size / 1000) << " kilobytes\n";
        std::cout << "Extensions:\n";
        for (const auto& ext : filter.valid_extensions)
            std::cout << ext << '\n';
        std::cout << "Target path: " << target << '\n';
        std::cout << "Confirm? (y/n)\n";
        char c; std::cin >> c;
        if (c != 'y')
            exit(0);
    }

    for (const auto& drive : CollectDrives(true))
        RecurseCopyFiles(drive, target, filter);

    if (!g_quiet)
    {
        std::cout << "Done. Copied " << g_copied << " files.\nPress a key to exit.\n";
        std::cin.get();
    }
}
