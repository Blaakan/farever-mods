#include <windows.h>
#include "dashboard.h"
#include "atlas_ui.h"
#include "report.h"
#include <cstdint>

namespace fmk {
namespace {
volatile LONG g_saved_ready = 0;
volatile LONG g_dirty = 0;
volatile ULONGLONG g_saved_signature = 0;

void mix(ULONGLONG& h, const void* data, size_t size) {
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (size_t i = 0; i < size; ++i) {
        h ^= bytes[i];
        h *= 1099511628211ull;
    }
}
void mix_text(ULONGLONG& h, const std::string& value) {
    mix(h, value.data(), value.size());
    const unsigned char separator = 0xff;
    mix(h, &separator, 1);
}
template <typename T> void mix_number(ULONGLONG& h, T value) {
    mix(h, &value, sizeof(value));
}
void mix_texts(ULONGLONG& h, const std::vector<std::string>& values) {
    mix_number(h, values.size());
    for (const auto& value : values) mix_text(h, value);
}
void mix_items(ULONGLONG& h, const std::vector<Item>& values) {
    mix_number(h, values.size());
    for (const auto& item : values) {
        mix_text(h, item.kind); mix_number(h, item.level); mix_number(h, item.upgrade);
        mix_number(h, item.rarity); mix_number(h, item.count); mix_text(h, item.cls);
    }
}
ULONGLONG signature(const Collection& c, const Inventories& i,
                    const std::vector<JobState>& jobs, const RuneState& runes,
                    const CompletionState& completion,
                    const std::vector<WeaponMastery>& mastery) {
    ULONGLONG h = 1469598103934665603ull;
    mix_number(h, c.valid); mix_number(h, c.bank_slots);
    mix_texts(h, c.mounts); mix_texts(h, c.gliders); mix_texts(h, c.pets);
    mix_texts(h, c.gears); mix_texts(h, c.toys); mix_texts(h, c.emotes);
    mix_number(h, i.valid); mix_text(h, i.character); mix_text(h, i.hero_class);
    mix_text(h, i.steam_account_id);
    mix_text(h, i.active_weapon); mix_number(h, i.character_level); mix_number(h, i.experience);
    mix_number(h, i.bank_slots); mix_items(h, i.bank); mix_items(h, i.bank_equipment);
    mix_items(h, i.equipped); mix_items(h, i.bags);
    mix_number(h, jobs.size());
    for (const auto& job : jobs) {
        mix_text(h, job.job); mix_number(h, job.level); mix_number(h, job.knowledge);
        mix_texts(h, job.learned);
    }
    mix_texts(h, runes.learned); mix_texts(h, runes.slotted);
    mix_number(h, completion.valid); mix_texts(h, completion.done);
    mix_number(h, mastery.size());
    for (const auto& row : mastery) { mix_text(h, row.weapon); mix_number(h, row.kills); }
    return h;
}
} // namespace

bool dashboard_take_save_request() { return atlas_ui_take_save_request(); }

void dashboard_observe(const Collection& collection, const Inventories& inventories,
                       const std::vector<JobState>& jobs, const RuneState& runes,
                       const CompletionState& completion,
                       const std::vector<WeaponMastery>& mastery) {
    const ULONGLONG current = signature(collection, inventories, jobs, runes, completion, mastery);
    if (InterlockedCompareExchange(&g_saved_ready, 1, 0) == 0) {
        InterlockedExchange64(reinterpret_cast<volatile LONG64*>(&g_saved_signature), (LONG64)current);
        InterlockedExchange(&g_dirty, 0);
        return;
    }
    InterlockedExchange(&g_dirty,
        InterlockedCompareExchange64(reinterpret_cast<volatile LONG64*>(&g_saved_signature), 0, 0) !=
        static_cast<LONG64>(current));
}

bool dashboard_has_changes() {
    return InterlockedCompareExchange(&g_dirty, 0, 0) != 0;
}

void dashboard_save(const Collection& collection, const Inventories& inventories,
                    const std::vector<JobState>& jobs, const RuneState& runes,
                    const CompletionState& completion,
                    const std::vector<WeaponMastery>& mastery) {
    write_collection_json(collection);
    if (inventories.valid) write_inventory_json(inventories, inventories.character);
    report_refresh();
    const ULONGLONG current = signature(collection, inventories, jobs, runes, completion, mastery);
    InterlockedExchange64(reinterpret_cast<volatile LONG64*>(&g_saved_signature), (LONG64)current);
    InterlockedExchange(&g_dirty, 0);
}

void dashboard_mark_saved() { atlas_ui_mark_saved(); }

} // namespace fmk
