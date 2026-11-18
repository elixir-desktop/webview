#include "aumid.hpp"
#include "win_util.hpp"

#include <cstdio>
#include <shobjidl.h>

bool set_app_user_model_id(const std::string& utf8_id) {
  if (utf8_id.empty()) return true;
  std::wstring wide = utf8_to_wide(utf8_id);
  HRESULT hr = SetCurrentProcessExplicitAppUserModelID(wide.c_str());
  if (FAILED(hr)) {
    fprintf(stderr, "edw: SetCurrentProcessExplicitAppUserModelID failed (0x%08lx)\n",
            static_cast<unsigned long>(hr));
    return false;
  }
  return true;
}
