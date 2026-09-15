#pragma once

#include <windows.h>
#include <cstddef>

// Shared-memory protocol between NativeControl and the injected shell bridge.
// The controller creates one mapping per target shell process named:
//   Local\OledTaskbar.Native.Status.<target PID>
// It fills the request fields before posting OledTaskbar.Native.Configure with
// wParam=RequestId and lParam=kProtocolVersion. The
// bridge publishes the response fields, sets Completion last with an interlocked
// operation, and only then signals Local\OledTaskbar.Native.Ready.<target PID>.
namespace OledTaskbarNative {

constexpr DWORD kProtocolMagic = 0x3242544f; // "OTB2" in little-endian form.
constexpr DWORD kProtocolVersion = 1;
constexpr DWORD kErrorCapacity = 512;
constexpr LONG kResponsePending = 0;
constexpr LONG kResponseComplete = 1;

enum Result : DWORD {
    ResultSuccess = 0,
    ResultInvalidRequest = 1,
    ResultOwnerUnavailable = 2,
    ResultTargetUnavailable = 3,
    ResultHookFailure = 4,
    ResultRefreshFailure = 5,
    ResultInternalFailure = 6,
};

#pragma pack(push, 8)
struct NativeStatus {
    // Request, immutable after the configure message is posted.
    DWORD Magic;
    DWORD Version;
    DWORD Size;
    DWORD TargetPid;
    DWORD OwnerPid;
    DWORD DesiredEnabled;   // Exactly 0 or 1.
    DWORD OriginalAutoHide; // Exactly 0 or ABS_AUTOHIDE.
    DWORD Reserved;         // Must be zero.
    ULONGLONG RequestId;    // Nonzero identifier chosen by the controller.

    // Response. ResponseRequestId is echoed and Completion is written last.
    ULONGLONG ResponseRequestId;
    DWORD Active;           // Exactly 0 or 1.
    DWORD ResultCode;       // One of Result.
    WCHAR Error[kErrorCapacity];
    volatile LONG Completion;
};
#pragma pack(pop)

static_assert(offsetof(NativeStatus, RequestId) == 32, "NativeStatus request layout changed");
static_assert(offsetof(NativeStatus, ResponseRequestId) == 40, "NativeStatus response layout changed");
static_assert(offsetof(NativeStatus, Completion) == 1080, "NativeStatus completion must remain last");
static_assert(sizeof(NativeStatus) == 1088, "NativeStatus wire layout changed");
static_assert(sizeof(WPARAM) == sizeof(ULONGLONG), "The native protocol requires a 64-bit shell");

} // namespace OledTaskbarNative
