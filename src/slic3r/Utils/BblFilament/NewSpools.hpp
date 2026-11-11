#pragma once

#include "Linker.hpp"

#include <map>
#include <string>
#include <vector>

namespace Slic3r::BblFilament {

// The AMS slots that hold a newly registered official spool, for the marker on the slot. Memory only; one thread.
class NewSpools
{
public:
    void add(const SlotKey& slot, const std::string& rfid);
    void dismiss(const SlotKey& slot);
    // Drops markers whose slot is empty or reports another spool, on printers that take part with fresh data.
    void reconcile(const std::vector<PrinterView>& printers);
    bool                 has(const SlotKey& slot) const;
    std::vector<SlotKey> of_printer(const std::string& dev_id) const;
    std::string          rfid(const SlotKey& slot) const; // empty when none
    void                 clear();

private:
    std::map<SlotKey, std::string> m_slots; // slot -> the RFID of the spool registered there
};

} // namespace Slic3r::BblFilament
