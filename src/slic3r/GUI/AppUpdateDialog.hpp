#pragma once

// Windows only: the update prompt's Download downloads the installer, verifies it against the checksum published
// with the release and starts it when OrcaSlicer closes. See docs/HLSD/app-update.md.

#include <atomic>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <boost/filesystem/path.hpp>
#include <wx/gdicmn.h>
#include <wx/string.h>

#include "GUI_Utils.hpp"
#include "slic3r/Utils/AppUpdate.hpp"
#include "slic3r/Utils/Http.hpp"

class wxWindow;
class Label;
class ProgressBar;

namespace Slic3r { namespace GUI {

class DialogButtons;

// A file opened for reading and shared for reading only: while it is open, nobody can write, rename or delete it.
// The verified installer is hashed through this handle and kept open until its process has been started.
// Only a plain file is accepted: a symbolic link, junction or folder at the path leaves it closed.
class HeldFile
{
public:
    explicit HeldFile(const boost::filesystem::path& path);
    ~HeldFile();
    HeldFile(const HeldFile&)            = delete;
    HeldFile& operator=(const HeldFile&) = delete;

    bool                           is_open() const { return m_handle != nullptr; }
    const boost::filesystem::path& path() const { return m_path; }
    // The size in bytes, -1 when it cannot be read.
    long long size() const;
    // SHA-256 of the whole file read through this handle, nullopt when it cannot be read.
    std::optional<std::string> sha256_hex() const;
    // Starts the file with its normal user interface (Windows shows its administrator prompt), still holding it.
    bool launch() const;

private:
    void*                   m_handle{nullptr}; // a Win32 HANDLE
    boost::filesystem::path m_path;
};

// A process handle with its id, closed on destruction. While it is open, the id cannot be reused.
class ProcessHandle
{
public:
    ProcessHandle(void* handle, unsigned long pid) : m_handle(handle), m_pid(pid) {}
    ~ProcessHandle();
    ProcessHandle(ProcessHandle&& other) noexcept;
    ProcessHandle& operator=(ProcessHandle&& other) noexcept;
    ProcessHandle(const ProcessHandle&)            = delete;
    ProcessHandle& operator=(const ProcessHandle&) = delete;

    void*         get() const { return m_handle; }
    unsigned long pid() const { return m_pid; }

private:
    void*         m_handle{nullptr}; // a Win32 HANDLE
    unsigned long m_pid{0};
};

// Other running processes of this executable (same full image path):
// - helpers: the --mcp-stdio bridges of AI agent clients, which announce themselves with the named event of
//   AppUpdate::helper_event_name();
// - windows: not helpers, and own a visible top-level window;
// - others: neither, such as an instance still starting or a command-line job.
// Windows and others block the in-app install; only helpers are ever ended.
struct OtherInstances
{
    std::vector<ProcessHandle> helpers;
    std::size_t                windows{0};
    std::size_t                others{0};

    bool blocking() const { return windows + others > 0; }
};
OtherInstances find_other_instances();

// Ends the helper processes found now: sets each one's event and waits up to 3 s; one still running is terminated
// through the same handle, with up to 3 s more. Never touches a process that is not a helper.
void terminate_update_helpers();

class AppUpdateDialog : public DPIDialog
{
public:
    AppUpdateDialog(wxWindow* parent, const AppUpdate::InstallerOffer& offer, const std::string& version, const std::string& release_url);
    ~AppUpdateDialog() override;

    // After ShowModal() returned wxID_OK (Install now): the verified installer, still held.
    std::unique_ptr<HeldFile> take_installer() { return std::move(m_installer); }

protected:
    void on_dpi_changed(const wxRect& suggested_rect) override;

private:
    enum class Stage { Downloading, Failed, Ready };

    void start();
    void download_checksum();
    void download_installer();
    void on_progress(std::size_t now, std::size_t total);
    void on_installer_saved();
    // Opens and hashes the target file; on Ok it is held in m_installer, otherwise it is deleted.
    AppUpdate::Verdict verify();
    void show_failure(const wxString& message);
    void show_ready(bool windows_still_open);
    void show_stage(Stage stage);
    void relayout();

    AppUpdate::InstallerOffer m_offer;
    std::string               m_version;
    std::string               m_release_url;
    std::string               m_expected_hex; // from the .sha256 asset
    boost::filesystem::path   m_folder;
    boost::filesystem::path   m_target;

    std::shared_ptr<std::atomic<bool>> m_alive{std::make_shared<std::atomic<bool>>(true)};
    Http::Ptr                          m_request;
    std::unique_ptr<HeldFile>          m_installer;

    Label*         m_message{nullptr};
    Label*         m_detail{nullptr};
    ProgressBar*   m_progress{nullptr};
    DialogButtons* m_download_buttons{nullptr};
    DialogButtons* m_failed_buttons{nullptr};
    DialogButtons* m_ready_buttons{nullptr};
};

}} // namespace Slic3r::GUI
