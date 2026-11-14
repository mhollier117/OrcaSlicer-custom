#include "AppUpdateDialog.hpp"

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "MainFrame.hpp"
#include "format.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/DialogButtons.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/ProgressBar.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <ios>
#include <istream>
#include <memory>
#include <optional>
#include <set>
#include <streambuf>
#include <string>
#include <utility>
#include <vector>

#include <boost/filesystem/operations.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>
#include <boost/system/error_code.hpp>

#include <wx/colour.h>
#include <wx/event.h>
#include <wx/sizer.h>
#include <wx/utils.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>

namespace fs = boost::filesystem;

namespace Slic3r { namespace GUI {

namespace {

constexpr long long one_mb = 1LL << 20;

// Reads a file through a Win32 handle, so the hash covers exactly the file that handle holds. A failed read throws,
// which the stream turns into its bad state.
class HandleReadBuf : public std::streambuf
{
public:
    explicit HandleReadBuf(HANDLE handle) : m_handle(handle), m_buffer(1 << 16) {}

protected:
    int_type underflow() override
    {
        if (gptr() < egptr())
            return traits_type::to_int_type(*gptr());
        DWORD got = 0;
        if (!::ReadFile(m_handle, m_buffer.data(), DWORD(m_buffer.size()), &got, nullptr))
            throw std::ios_base::failure("ReadFile failed");
        if (got == 0)
            return traits_type::eof();
        setg(m_buffer.data(), m_buffer.data(), m_buffer.data() + got);
        return traits_type::to_int_type(*gptr());
    }

private:
    HANDLE            m_handle;
    std::vector<char> m_buffer;
};

std::wstring process_image_path(HANDLE process)
{
    std::wstring path(32768, L'\0');
    DWORD        size = DWORD(path.size());
    if (!::QueryFullProcessImageNameW(process, 0, path.data(), &size))
        return {};
    path.resize(size);
    return path;
}

struct WindowSearch
{
    const std::set<DWORD>* candidates;
    std::set<DWORD>        with_window;
};

BOOL CALLBACK collect_visible_window(HWND hwnd, LPARAM param)
{
    auto* search = reinterpret_cast<WindowSearch*>(param);
    DWORD pid    = 0;
    ::GetWindowThreadProcessId(hwnd, &pid);
    if (::IsWindowVisible(hwnd) && search->candidates->count(pid) != 0)
        search->with_window.insert(pid);
    return TRUE;
}

wxString download_failed(const std::string& error, unsigned status)
{
    if (status != 0)
        return format_wxstr(_L("The download failed with HTTP status %1%."), status);
    return format_wxstr(_L("The download failed: %1%"), error);
}

wxString verdict_message(AppUpdate::Verdict verdict)
{
    switch (verdict) {
    case AppUpdate::Verdict::WrongSize: return _L("The download does not have the size published with the release and was deleted.");
    case AppUpdate::Verdict::WrongDigest: return _L("The download does not match its published checksum and was deleted.");
    case AppUpdate::Verdict::Unreadable: return _L("The download could not be read and was deleted.");
    case AppUpdate::Verdict::Ok: break;
    }
    return {};
}

wxString megabytes(long long bytes) { return wxString::Format("%.1f", double(bytes) / double(one_mb)); }

} // namespace

// ---- HeldFile ----

HeldFile::HeldFile(const fs::path& path) : m_path(path)
{
    // Shared for reading only: no other handle may write, rename or delete the file while this one is open.
    HANDLE handle = ::CreateFileW(path.wstring().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    m_handle = handle == INVALID_HANDLE_VALUE ? nullptr : handle;
}

HeldFile::~HeldFile()
{
    if (m_handle != nullptr)
        ::CloseHandle(static_cast<HANDLE>(m_handle));
}

long long HeldFile::size() const
{
    LARGE_INTEGER size{};
    if (m_handle == nullptr || !::GetFileSizeEx(static_cast<HANDLE>(m_handle), &size))
        return -1;
    return size.QuadPart;
}

std::optional<std::string> HeldFile::sha256_hex() const
{
    LARGE_INTEGER start{};
    if (m_handle == nullptr || !::SetFilePointerEx(static_cast<HANDLE>(m_handle), start, nullptr, FILE_BEGIN))
        return std::nullopt;
    HandleReadBuf buffer(static_cast<HANDLE>(m_handle));
    std::istream  in(&buffer);
    return AppUpdate::sha256_hex(in);
}

bool HeldFile::launch() const
{
    if (m_handle == nullptr)
        return false;
    const std::wstring file = m_path.wstring();
    SHELLEXECUTEINFOW  info{};
    info.cbSize = sizeof(info);
    info.fMask  = SEE_MASK_NOCLOSEPROCESS;
    info.lpVerb = L"open";
    info.lpFile = file.c_str();
    info.nShow  = SW_SHOWNORMAL;
    if (!::ShellExecuteExW(&info)) {
        BOOST_LOG_TRIVIAL(error) << "Update: the installer could not be started, error " << ::GetLastError();
        return false;
    }
    if (info.hProcess != nullptr)
        ::CloseHandle(info.hProcess);
    return true;
}

// ---- other processes of this executable ----

OtherInstances find_other_instances()
{
    OtherInstances result;
    const std::wstring own_path = process_image_path(::GetCurrentProcess());
    if (own_path.empty())
        return result;
    const DWORD own_pid = ::GetCurrentProcessId();

    HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return result;
    std::set<DWORD>  same_image;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    for (BOOL more = ::Process32FirstW(snapshot, &entry); more; more = ::Process32NextW(snapshot, &entry)) {
        if (entry.th32ProcessID == own_pid)
            continue;
        HANDLE process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
        if (process == nullptr)
            continue;
        const std::wstring path = process_image_path(process);
        ::CloseHandle(process);
        if (!path.empty() && ::_wcsicmp(path.c_str(), own_path.c_str()) == 0)
            same_image.insert(entry.th32ProcessID);
    }
    ::CloseHandle(snapshot);
    if (same_image.empty())
        return result;

    WindowSearch search{&same_image, {}};
    ::EnumWindows(collect_visible_window, reinterpret_cast<LPARAM>(&search));
    result.windows = search.with_window.size();
    for (DWORD pid : same_image)
        if (search.with_window.count(pid) == 0)
            result.helpers.push_back(pid);
    return result;
}

void terminate_update_helpers()
{
    for (unsigned long pid : find_other_instances().helpers) {
        HANDLE process = ::OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid);
        if (process == nullptr)
            continue;
        if (::TerminateProcess(process, 1))
            ::WaitForSingleObject(process, 3000);
        else
            BOOST_LOG_TRIVIAL(warning) << "Update: could not end helper process " << pid << ", error " << ::GetLastError();
        ::CloseHandle(process);
    }
}

// ---- AppUpdateDialog ----

AppUpdateDialog::AppUpdateDialog(wxWindow*                        parent,
                                 const AppUpdate::InstallerOffer& offer,
                                 const std::string&               version,
                                 const std::string&               release_url)
    : DPIDialog(parent ? parent : static_cast<wxWindow*>(wxGetApp().mainframe), wxID_ANY, _L("Update OrcaSlicer"), wxDefaultPosition,
                wxDefaultSize, wxCAPTION | wxCLOSE_BOX)
    , m_offer(offer)
    , m_version(version)
    , m_release_url(release_url)
{
    SetBackgroundColour(*wxWHITE);
    SetFont(Label::Body_14);

    const wxSize text_size(FromDIP(440), -1);
    m_message = new Label(this, Label::Body_14, wxEmptyString, LB_AUTO_WRAP, text_size);
    m_detail  = new Label(this, Label::Body_14, wxEmptyString, LB_AUTO_WRAP, text_size);
    m_progress = new ProgressBar(this, wxID_ANY, 100, wxDefaultPosition, wxSize(FromDIP(440), FromDIP(14)));

    m_download_buttons = new DialogButtons(this, {"Cancel"});
    m_failed_buttons   = new DialogButtons(this, {L("Open the release page"), L("Close")}, _L("Open the release page"));
    m_ready_buttons    = new DialogButtons(this, {L("Install now"), L("Later")}, _L("Install now"));

    // Every path that closes the dialog ends in the destructor, which cancels a running download.
    m_download_buttons->GetCANCEL()->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_CANCEL); });
    m_failed_buttons->GetButtonFromIndex(0)->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        wxLaunchDefaultBrowser(from_u8(m_release_url));
        EndModal(wxID_CANCEL);
    });
    m_failed_buttons->GetButtonFromIndex(1)->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_CANCEL); });
    m_ready_buttons->GetButtonFromIndex(0)->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        // Another OrcaSlicer window would keep its files in use: the check is repeated until it is closed.
        if (find_other_instances().windows > 0) {
            show_ready(true);
            return;
        }
        EndModal(wxID_OK);
    });
    m_ready_buttons->GetButtonFromIndex(1)->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_CANCEL); });

    auto* sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(m_message, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(20));
    sizer->Add(m_detail, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(20));
    sizer->Add(m_progress, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(20));
    sizer->AddSpacer(FromDIP(12));
    sizer->Add(m_download_buttons, 0, wxEXPAND);
    sizer->Add(m_failed_buttons, 0, wxEXPAND);
    sizer->Add(m_ready_buttons, 0, wxEXPAND);

    m_detail->Hide();
    show_stage(Stage::Downloading);
    on_progress(0, std::size_t(m_offer.size));

    SetSizerAndFit(sizer);
    CenterOnParent();
    wxGetApp().UpdateDlgDarkUI(this);

    start();
}

AppUpdateDialog::~AppUpdateDialog()
{
    *m_alive = false;
    if (m_request)
        m_request->cancel();
}

void AppUpdateDialog::on_dpi_changed(const wxRect&)
{
    const wxSize text_size(FromDIP(440), -1);
    m_message->SetMinSize(text_size);
    m_detail->SetMinSize(text_size);
    m_progress->SetMinSize(wxSize(FromDIP(440), FromDIP(14)));
    GetSizer()->SetSizeHints(this);
    Refresh();
}

void AppUpdateDialog::start()
{
    boost::system::error_code ec;
    m_folder = fs::temp_directory_path(ec) / "OrcaSlicer_update";
    m_target = m_folder / fs::path(from_u8(m_offer.name).ToStdWstring());
    if (!ec)
        fs::create_directories(m_folder, ec);
    if (ec || !fs::is_directory(m_folder, ec)) {
        show_failure(format_wxstr(_L("The download could not be saved in %1%."), from_path(m_folder)));
        return;
    }
    // Only the installer of this release stays in the folder; anything else is from earlier updates.
    for (fs::directory_iterator it(m_folder, ec), end; !ec && it != end; it.increment(ec)) {
        boost::system::error_code remove_ec;
        if (it->path().filename() != m_target.filename() && fs::is_regular_file(it->path(), remove_ec))
            fs::remove(it->path(), remove_ec);
    }
    download_checksum();
}

void AppUpdateDialog::download_checksum()
{
    auto alive = m_alive;
    auto http  = Http::get(m_offer.sha256_url);
    http.tls_verify(true)
        .size_limit(4096)
        .timeout_connect(10)
        .timeout_max(60)
        .on_complete([this, alive](std::string body, unsigned) {
            wxGetApp().CallAfter([this, alive, body = std::move(body)] {
                if (!*alive)
                    return;
                m_request.reset();
                const auto hex = AppUpdate::parse_sha256_file(body, m_offer.name);
                if (!hex) {
                    show_failure(_L("The checksum published with the release could not be read."));
                    return;
                }
                m_expected_hex = *hex;
                // An installer downloaded earlier is used when it passes the checks; verify() deletes it otherwise.
                boost::system::error_code ec;
                if (fs::exists(m_target, ec) && verify() == AppUpdate::Verdict::Ok) {
                    show_ready(false);
                    return;
                }
                download_installer();
            });
        })
        .on_error([this, alive](std::string, std::string error, unsigned status) {
            BOOST_LOG_TRIVIAL(error) << "Update: the checksum download failed: HTTP " << status << ", " << error;
            wxGetApp().CallAfter([this, alive, error, status] {
                if (*alive)
                    show_failure(download_failed(error, status));
            });
        });
    m_request = http.perform();
}

void AppUpdateDialog::download_installer()
{
    show_stage(Stage::Downloading);
    on_progress(0, std::size_t(m_offer.size));

    auto            alive        = m_alive;
    auto            last_percent = std::make_shared<std::atomic<int>>(-1);
    const fs::path  part         = m_folder / fs::path(m_target.filename().wstring() + L".part");
    const fs::path  target       = m_target;
    const long long total        = m_offer.size;

    auto http = Http::get(m_offer.url);
    http.tls_verify(true)
        .size_limit(std::size_t(m_offer.size + one_mb))
        .timeout_connect(10)
        .on_progress([this, alive, last_percent, total](Http::Progress progress, bool& cancel) {
            if (!*alive) {
                cancel = true;
                return;
            }
            const int percent = int(std::min<long long>(100, (long long)progress.dlnow * 100 / std::max(1LL, total)));
            if (last_percent->exchange(percent) == percent)
                return;
            wxGetApp().CallAfter([this, alive, now = progress.dlnow, total] {
                if (*alive)
                    on_progress(now, std::size_t(total));
            });
        })
        .on_complete([this, alive, part, target](std::string body, unsigned) {
            // Saved here, off the UI thread: <name>.part first, then renamed to <name>.
            bool saved = false;
            {
                boost::nowide::ofstream out(part.string().c_str(), std::ios::binary | std::ios::trunc);
                out.write(body.data(), std::streamsize(body.size()));
                out.close();
                saved = bool(out);
            }
            boost::system::error_code ec;
            if (saved) {
                fs::rename(part, target, ec);
                saved = !ec;
            }
            if (!saved) {
                BOOST_LOG_TRIVIAL(error) << "Update: the installer could not be saved: " << ec.message();
                fs::remove(part, ec);
            }
            wxGetApp().CallAfter([this, alive, saved] {
                if (!*alive)
                    return;
                m_request.reset();
                if (saved)
                    on_installer_saved();
                else
                    show_failure(format_wxstr(_L("The download could not be saved in %1%."), from_path(m_folder)));
            });
        })
        .on_error([this, alive](std::string, std::string error, unsigned status) {
            BOOST_LOG_TRIVIAL(error) << "Update: the installer download failed: HTTP " << status << ", " << error;
            wxGetApp().CallAfter([this, alive, error, status] {
                if (*alive)
                    show_failure(download_failed(error, status));
            });
        });
    m_request = http.perform();
}

void AppUpdateDialog::on_progress(std::size_t now, std::size_t total)
{
    const long long done = std::min<long long>((long long)now, (long long)total);
    m_progress->SetValue(total > 0 ? int(done * 100 / (long long)total) : 0);
    // TRN %1% is a version, %2% and %3% are sizes in megabytes, as in "12.3 of 98.7 MB".
    m_message->SetLabel(format_wxstr(_L("Downloading OrcaSlicer %1%… %2% of %3% MB"), m_version, megabytes(done), megabytes((long long)total)));
}

void AppUpdateDialog::on_installer_saved()
{
    const AppUpdate::Verdict verdict = verify();
    if (verdict == AppUpdate::Verdict::Ok)
        show_ready(false);
    else
        show_failure(verdict_message(verdict));
}

AppUpdate::Verdict AppUpdateDialog::verify()
{
    m_installer.reset();
    auto               file = std::make_unique<HeldFile>(m_target);
    AppUpdate::Verdict verdict;
    {
        wxBusyCursor busy;
        // Hashed through the held handle: the file that is checked is the file that will be started.
        const std::optional<std::string> hex = file->is_open() ? file->sha256_hex() : std::nullopt;
        verdict = AppUpdate::judge(file->size(), hex, m_offer.size, m_expected_hex, m_offer.digest_hex);
    }
    if (verdict == AppUpdate::Verdict::Ok) {
        m_installer = std::move(file);
        return verdict;
    }
    BOOST_LOG_TRIVIAL(warning) << "Update: the installer failed its check (" << int(verdict) << ") and is deleted";
    file.reset();
    boost::system::error_code ec;
    fs::remove(m_target, ec);
    return verdict;
}

void AppUpdateDialog::show_failure(const wxString& message)
{
    m_request.reset();
    m_installer.reset();
    m_message->SetLabel(message);
    m_detail->Hide();
    show_stage(Stage::Failed);
}

void AppUpdateDialog::show_ready(bool windows_still_open)
{
    m_message->SetLabel(format_wxstr(_L("OrcaSlicer %1% is downloaded and verified. OrcaSlicer will close and the installer will start."),
                                     m_version));
    const OtherInstances others = find_other_instances();
    wxString             detail;
    if (others.windows > 0)
        detail = windows_still_open ? _L("Other OrcaSlicer windows are still open. Close them first.") :
                                      _L("Close the other OrcaSlicer windows first.");
    else if (!others.helpers.empty()) {
        const unsigned int count = unsigned(others.helpers.size());
        detail = format_wxstr(_L_PLURAL("%1% connection of an AI agent to OrcaSlicer will be closed. Reconnect it afterwards.",
                                        "%1% connections of AI agents to OrcaSlicer will be closed. Reconnect them afterwards.", count),
                              count);
    }
    m_detail->SetLabel(detail);
    m_detail->Show(!detail.empty());
    show_stage(Stage::Ready);
}

void AppUpdateDialog::show_stage(Stage stage)
{
    m_progress->Show(stage == Stage::Downloading);
    m_download_buttons->Show(stage == Stage::Downloading);
    m_failed_buttons->Show(stage == Stage::Failed);
    m_ready_buttons->Show(stage == Stage::Ready);
    relayout();
}

void AppUpdateDialog::relayout()
{
    if (GetSizer() == nullptr)
        return; // still in the constructor
    GetSizer()->SetSizeHints(this);
    Layout();
    Refresh();
}

}} // namespace Slic3r::GUI
