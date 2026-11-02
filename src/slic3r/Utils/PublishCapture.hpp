#pragma once

// Dry runs for the printer MCP tools (fork). While a ScopedPublishCapture is alive on a thread,
// every printer message that thread would send (NetworkAgent::send_message[_to_printer] and
// BBLPrinterAgent's own send paths, which all typed BBL commands funnel through) is recorded here
// and NOT sent, and the print-start entry points refuse to run. Nothing changes when no capture
// is active, which is always the case outside an MCP dry run.

#include <string>
#include <utility>
#include <vector>

namespace Slic3r {

struct PublishCapture
{
    struct Message
    {
        std::string dev_id;
        std::string json;
        bool        lan; // send_message_to_printer (LAN MQTT) rather than send_message
    };
    std::vector<Message> messages;

    // The capture active on this thread, or null.
    static PublishCapture*& current()
    {
        static thread_local PublishCapture* active = nullptr;
        return active;
    }

    // Records the message if a capture is active; returns whether it did (the caller then returns 0
    // without sending).
    static bool intercept(const std::string& dev_id, const std::string& json, bool lan)
    {
        PublishCapture* capture = current();
        if (capture == nullptr)
            return false;
        capture->messages.push_back({dev_id, json, lan});
        return true;
    }
};

class ScopedPublishCapture
{
public:
    ScopedPublishCapture() : m_previous(PublishCapture::current()) { PublishCapture::current() = &m_capture; }
    ~ScopedPublishCapture() { PublishCapture::current() = m_previous; }
    ScopedPublishCapture(const ScopedPublishCapture&)            = delete;
    ScopedPublishCapture& operator=(const ScopedPublishCapture&) = delete;

    const std::vector<PublishCapture::Message>& messages() const { return m_capture.messages; }

private:
    PublishCapture  m_capture;
    PublishCapture* m_previous;
};

} // namespace Slic3r
