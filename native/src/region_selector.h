#pragma once
#include "platform.h"
#include <optional>

namespace recdesk {
std::optional<RECT> selectRegion(HINSTANCE instance);
}
