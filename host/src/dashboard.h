#pragma once
#include "hl_reader.h"

namespace fmk {

bool dashboard_take_save_request();
void dashboard_observe(const Collection& collection, const Inventories& inventories,
                       const std::vector<JobState>& jobs, const RuneState& runes,
                       const CompletionState& completion,
                       const std::vector<WeaponMastery>& mastery);
bool dashboard_has_changes();
void dashboard_save(const Collection& collection, const Inventories& inventories,
                    const std::vector<JobState>& jobs, const RuneState& runes,
                    const CompletionState& completion,
                    const std::vector<WeaponMastery>& mastery);
void dashboard_mark_saved();

} // namespace fmk
