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

// Other running processes of this executable: those with a visible top-level window are other OrcaSlicer windows,
// the others are helpers (the --mcp-stdio bridges of AI agent clients, which have no window).
struct OtherInstances
{
    std::vector<unsigned long> helpers;
    std::size_t                windows{0};
};
OtherInstances find_other_instances();

// Terminates the helper processes found now, waiting up to 3 s for each. Never touches a process with a window.
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
