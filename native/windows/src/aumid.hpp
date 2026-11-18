#pragma once

#include <string>

// Set the process AppUserModelID (Task Manager / taskbar grouping). No-op if empty.
// Returns true on success or when id is empty.
bool set_app_user_model_id(const std::string& utf8_id);
