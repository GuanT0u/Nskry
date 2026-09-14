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
// C++/WinRT base + projections
// ============================================================
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

// ============================================================
// WinRT ↔ native COM interop headers
// ============================================================
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

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
