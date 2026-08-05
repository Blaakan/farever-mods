#pragma once
#include <string>
namespace fmk {
const std::wstring& game_dir();
const std::wstring& data_dir();
std::wstring character_data_dir(const std::string& account_uuid, const std::string& character_id, const std::string& character_name);
}
