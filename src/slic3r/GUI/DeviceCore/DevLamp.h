#pragma once

#include <string>

namespace Slic3r
{

    class MachineObject;
    class DevLamp
    {
    public:
        DevLamp(MachineObject* obj) : m_owner(obj) {};

    public:
        enum LIGHT_EFFECT
        {
            LIGHT_EFFECT_ON,
            LIGHT_EFFECT_OFF,
            LIGHT_EFFECT_FLASHING,
            LIGHT_EFFECT_UNKOWN,
        };

    public:
        void SetChamberLight(const std::string& status);
        void SetChamberLight(LIGHT_EFFECT effect) { m_chamber_light = effect; }
        bool IsChamberLightOn() const { return m_chamber_light == LIGHT_EFFECT_ON || m_chamber_light == LIGHT_EFFECT_FLASHING; }

        void SetLampCloseRecheck(bool enable) { m_lamp_close_recheck = enable;};
        bool HasLampCloseRecheck() const { return m_lamp_close_recheck; }

        // Toolhead logo lamp (M960 S5). Printers do not report its state, so
        // this is what Orca last sent. A print's start G-code turns it back on,
        // so an "off" only holds for the job it was sent during.
        bool IsToolheadLogoOn() const;

    public:
        void CtrlSetChamberLight(LIGHT_EFFECT effect);
        int  CtrlSetToolheadLogo(bool on);

    private:
        int command_set_chamber_light(LIGHT_EFFECT effect, int on_time = 500, int off_time = 500, int loops = 1, int interval = 1000);
        int command_set_chamber_light2(LIGHT_EFFECT effect, int on_time = 500, int off_time = 500, int loops = 1, int interval = 1000);

    private:
        MachineObject* m_owner = nullptr;

        bool m_lamp_close_recheck = false;
        LIGHT_EFFECT m_chamber_light = LIGHT_EFFECT_UNKOWN;

        bool        m_toolhead_logo_off = false;
        std::string m_toolhead_logo_off_subtask; // subtask the "off" was sent during
    };
}