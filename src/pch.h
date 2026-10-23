#pragma once

// ============================================================
// Windows core
// ============================================================
#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <shellscalingapi.h>

// ============================================================
// Direct3D 11 / DXGI
// ============================================================
#include <d3d11_4.h>
#include <dxgi1_6.h>

// ============================================================
// C++/WinRT base. Heavy capture projections belong to capture modules, not
// every settings/JSON/update/annotation translation unit.
// ============================================================
#include <winrt/base.h>

// ============================================================
// STL
// ============================================================
#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <mutex>
#include <atomic>
#include <algorithm>
#include <stdexcept>
#include <cstdio>
