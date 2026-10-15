#include "store.h"

#include <Preferences.h>

#include <mutex>

static Preferences s_prefs;
static std::mutex s_mu;

void store::begin() { s_prefs.begin("zhalt", false); }

std::string store::get(const char* key) {
  std::lock_guard<std::mutex> l(s_mu);
  if (!s_prefs.isKey(key)) return "";
  return s_prefs.getString(key, "").c_str();
}

void store::put(const char* key, const std::string& value) {
  std::lock_guard<std::mutex> l(s_mu);
  if (s_prefs.isKey(key) && s_prefs.getString(key, "") == value.c_str()) return;  // spare the flash
  s_prefs.putString(key, value.c_str());
}
