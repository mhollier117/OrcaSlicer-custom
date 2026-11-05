#include <nlohmann/json.hpp>
#include <string>
#include "DevLamp.h"

// TODO: remove this include
#include "slic3r/GUI/DeviceManager.hpp"

using namespace nlohmann;

namespace Slic3r
{


static std::string _light_effect_str(DevLamp::LIGHT_EFFECT effect)
{
    switch (effect)
    {
    case Slic3r::DevLamp::LIGHT_EFFECT_ON:
        return "on";
    case Slic3r::DevLamp::LIGHT_EFFECT_OFF:
        return "off";
    case Slic3r::DevLamp::LIGHT_EFFECT_FLASHING:
        return "flashing";
    default:
        return "unknown";
    }
    return "unknown";
}

void DevLamp::CtrlSetChamberLight(LIGHT_EFFECT effect)
{
    // copied from others, TODO CHECK
    command_set_chamber_light(effect);
    command_set_chamber_light2(effect);
}

bool DevLamp::IsToolheadLogoOn() const
{
    if (!m_toolhead_logo_off) return true;
    // A new print's start G-code (M960 S5 P1) has turned it back on.
    return m_owner && m_owner->subtask_id_ != m_toolhead_logo_off_subtask;
}

int DevLamp::CtrlSetToolheadLogo(bool on)
{
    if (!m_owner) return -1;
    // Sent as a G-code line, the same path the fan controls use, so it also
    // works mid-print.
    const int rtn = m_owner->publish_gcode(on ? "M960 S5 P1\n" : "M960 S5 P0\n");
    if (rtn == 0) {
        m_toolhead_logo_off         = !on;
        m_toolhead_logo_off_subtask = m_owner->subtask_id_;
    }
    return rtn;
}

int DevLamp::command_set_chamber_light(LIGHT_EFFECT effect, int on_time, int off_time, int loops, int interval)
{
    json j;
    j["system"]["command"] = "ledctrl";
    j["system"]["led_node"] = "chamber_light";
    j["system"]["sequence_id"] = std::to_string(MachineObject::m_sequence_id++);
    j["system"]["led_mode"] = _light_effect_str(effect);
    j["system"]["led_on_time"] = on_time;
    j["system"]["led_off_time"] = off_time;
    j["system"]["loop_times"] = loops;
    j["system"]["interval_time"] = interval;
    return m_owner->publish_json(j);
}

int DevLamp::command_set_chamber_light2(LIGHT_EFFECT effect, int on_time, int off_time, int loops, int interval)
{
    json j;
    j["system"]["command"] = "ledctrl";
    j["system"]["led_node"] = "chamber_light2";
    j["system"]["sequence_id"] = std::to_string(MachineObject::m_sequence_id++);
    j["system"]["led_mode"] = _light_effect_str(effect);
    j["system"]["led_on_time"] = on_time;
    j["system"]["led_off_time"] = off_time;
    j["system"]["loop_times"] = loops;
    j["system"]["interval_time"] = interval;
    return m_owner->publish_json(j);
}

}