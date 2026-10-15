#pragma once
#include "sdk/nskry_capture.h"
namespace nskry {
// Stateless service table. No threads, devices or capture before open.
const NskryCaptureServiceV1 &RegionCaptureService();
} // namespace nskry
