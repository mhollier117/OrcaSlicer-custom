#ifndef slic3r_GUI_BblPresetSyncController_hpp_
#define slic3r_GUI_BblPresetSyncController_hpp_

#include "GUI_Utils.hpp"

#include "slic3r/Utils/BblPresetSync/Cloud.hpp"
#include "slic3r/Utils/BblPresetSync/LocalOps.hpp"
#include "slic3r/Utils/BblPresetSync/Mirror.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

class wxCheckListBox;
class wxTextCtrl;

namespace Slic3r {
class NetworkAgent;
class Preset;

namespace GUI {

class GUI_App;

// Shows what the first sync with the Bambu cloud would do and lets the user pick the filaments to convert.
class BblPresetSyncFirstRunDialog : public DPIDialog
{
public:
    BblPresetSyncFirstRunDialog(wxWindow* parent, const BblPresetSync::FirstRunSummary& summary);

    std::vector<std::string> checked() const;
    std::vector<std::string> unchecked() const;

protected:
    void on_dpi_changed(const wxRect& suggested_rect) override;

private:
    std::vector<std::string> m_candidates;
    wxCheckListBox*          m_list{nullptr};
};

// Connects the sync mirror to the app: host preconditions, UI-thread access to the presets, dialogs and notifications.
class BblPresetSyncController : public BblPresetSync::IHost, public BblPresetSync::ILocalStore
{
public:
    explicit BblPresetSyncController(GUI_App& app); // UI thread
    ~BblPresetSyncController() override;

    void tick(const std::weak_ptr<int>& sync_token); // sync thread, once per push pass
    void request_sync();                              // any thread: the next tick is a Manual pass
    BblPresetSync::Mirror& mirror();

    // Any thread: copy the K profiles of an old filament id to the named standalone filament, on the next pass.
    void request_k_copy(const std::string& filament_name, const std::string& from_filament_id_override);

    // UI thread:
    // The linked ledger entry's setting id, else the filament's own.
    std::string                    bbl_setting_id_for(const Preset& filament) const;
    BblPresetSync::ConvertResult   convert(const std::string& filament_name);
    nlohmann::json                 status_json() const;

    // IHost
    std::optional<BblPresetSync::Session> session() override;
    bool                                  cancelled() override;
    long long                             now() override;
    void                                  sleep_ms(int ms) override;
    std::string                           bundle_version() override;
    boost::filesystem::path               data_dir() override;
    boost::filesystem::path               resources_dir() override;
    void                                  on_first_run(const BblPresetSync::FirstRunSummary& summary) override;
    void on_candidates(const std::vector<BblPresetSync::Key>& candidates) override;
    void on_halt(const std::string& kind, const std::vector<BblPresetSync::Key>& local_deletes,
                 const std::vector<BblPresetSync::Key>& remote_deletes) override;
    void notify(const std::string& code, const std::string& detail) override;

    // ILocalStore
    std::optional<BblPresetSync::LocalSnapshot> snapshot(const BblPresetSync::Session& session) override;
    void apply_pull(BblPresetSync::PullOp op, std::function<void(BblPresetSync::OpOutcome)> done) override;
    void delete_local(BblPresetSync::DeleteOp op, std::function<void(BblPresetSync::OpOutcome)> done) override;

private:
    class CloudProxy;

    BblPresetSync::LocalOpsContext ops_context(const std::string& bambu_uid) const;
    void                           refresh_ui();
    void                           k_copy_step();
    void                           set_k_last(const std::string& key, const std::string& text);
    // Runs fn on the UI thread and waits for it; nullopt when cancelled first.
    template<class T> std::optional<T> run_on_ui(std::function<T()> fn);
    // Shows the conversion dialog; false when cancelled. Otherwise converts the checked filaments, declines the others and
    // reports the ones that were not converted.
    bool                           run_conversion_dialog(const BblPresetSync::FirstRunSummary& summary);

    GUI_App&                       m_app;
    std::unique_ptr<CloudProxy>    m_cloud;
    NetworkAgent*                  m_cloud_agent{nullptr};
    std::unique_ptr<BblPresetSync::Mirror> m_mirror;

    mutable std::mutex m_token_mutex;
    std::weak_ptr<int> m_token;
    std::atomic<bool>  m_manual_requested{false};

    struct KRequest
    {
        std::string name, from;
    };
    mutable std::mutex                 m_k_mutex;
    std::vector<KRequest>              m_k_requests;
    std::map<std::string, long long>   m_k_retry_after; // dev_id + "|" + key
    std::map<std::string, std::string> m_k_last;        // key -> last result text
};

} // namespace GUI
} // namespace Slic3r

#endif
