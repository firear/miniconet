/* Copyright Joyent, Inc. and other Node contributors. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */
#if defined(_MSC_VER) &&  _MSC_VER<1400
#include "winapi.h"
#include <assert.h>
#include <stdio.h>

/* Ntdll function pointers */
sRtlNtStatusToDosError pRtlNtStatusToDosError;
sNtDeviceIoControlFile pNtDeviceIoControlFile;
sNtQueryInformationFile pNtQueryInformationFile;
sNtSetInformationFile pNtSetInformationFile;
sNtQueryVolumeInformationFile pNtQueryVolumeInformationFile;
sNtQuerySystemInformation pNtQuerySystemInformation;

/* Kernel32 function pointers */
sGetQueuedCompletionStatusEx pGetQueuedCompletionStatusEx;
sSetFileCompletionNotificationModes pSetFileCompletionNotificationModes;
sCreateSymbolicLinkW pCreateSymbolicLinkW;
sCancelIoEx pCancelIoEx;
sInitializeSRWLock pInitializeSRWLock;
sAcquireSRWLockShared pAcquireSRWLockShared;
sAcquireSRWLockExclusive pAcquireSRWLockExclusive;
sTryAcquireSRWLockShared pTryAcquireSRWLockShared;
sTryAcquireSRWLockExclusive pTryAcquireSRWLockExclusive;
sReleaseSRWLockShared pReleaseSRWLockShared;
sReleaseSRWLockExclusive pReleaseSRWLockExclusive;
sInitializeConditionVariable pInitializeConditionVariable;
sSleepConditionVariableCS pSleepConditionVariableCS;
sSleepConditionVariableSRW pSleepConditionVariableSRW;
sWakeAllConditionVariable pWakeAllConditionVariable;
sWakeConditionVariable pWakeConditionVariable;
sUnregisterWait pUnregisterWait;
sUnregisterWaitEx pUnregisterWaitEx;
sRegisterWaitForSingleObject pRegisterWaitForSingleObject;
sGlobalMemoryStatusEx pGlobalMemoryStatusEx;
sQueueUserWorkItem pQueueUserWorkItem;
// sGetProcessMemoryInfo pGetProcessMemoryInfo;
sGetAdaptersAddresses pGetAdaptersAddresses;
sInterlockedCompareExchangePointer pInterlockedCompareExchangePointer = NULL;
sInterlockedCompareExchange32 pInterlockedCompareExchange32;
sInterlockedCompareExchange64 pInterlockedCompareExchange64;
sGetTickCount64 pGetTickCount64;
sGetNativeSystemInfo pGetNativeSystemInfo;
sGetCurrentProcessorNumber pGetCurrentProcessorNumber;
sGetThreadId pGetThreadId;
// ws2
sFreeAddrInfoW pFreeAddrInfoW;
sGetAddrInfoW pGetAddrInfoW;
sGetaddrinfo pgetaddrinfo;
sFreeaddrinfo pfreeaddrinfo;
sWSAPoll pWSAPoll;

#define h_fatal_error(code, str) printf("error:%d:%s\n", code, str);

void uv_winapi_init()
{
    HMODULE ntdll_module;
    HMODULE kernel32_module;

    ntdll_module = GetModuleHandleA("ntdll.dll");
    if (ntdll_module == NULL) {
        h_fatal_error(GetLastError(), "GetModuleHandleA");
    }

    pRtlNtStatusToDosError = (sRtlNtStatusToDosError)GetProcAddress(ntdll_module, "RtlNtStatusToDosError");
    if (pRtlNtStatusToDosError == NULL) {
        h_fatal_error(GetLastError(), "GetProcAddress");
    }

    pNtDeviceIoControlFile = (sNtDeviceIoControlFile)GetProcAddress(ntdll_module, "NtDeviceIoControlFile");
    if (pNtDeviceIoControlFile == NULL) {
        h_fatal_error(GetLastError(), "GetProcAddress");
    }

    pNtQueryInformationFile = (sNtQueryInformationFile)GetProcAddress(ntdll_module, "NtQueryInformationFile");
    if (pNtQueryInformationFile == NULL) {
        h_fatal_error(GetLastError(), "GetProcAddress");
    }

    pNtSetInformationFile = (sNtSetInformationFile)GetProcAddress(ntdll_module, "NtSetInformationFile");
    if (pNtSetInformationFile == NULL) {
        h_fatal_error(GetLastError(), "GetProcAddress");
    }

    pNtQueryVolumeInformationFile = (sNtQueryVolumeInformationFile)GetProcAddress(ntdll_module, "NtQueryVolumeInformationFile");
    if (pNtQueryVolumeInformationFile == NULL) {
        h_fatal_error(GetLastError(), "GetProcAddress");
    }

    pNtQuerySystemInformation = (sNtQuerySystemInformation)GetProcAddress(ntdll_module, "NtQuerySystemInformation");
    if (pNtQuerySystemInformation == NULL) {
        h_fatal_error(GetLastError(), "GetProcAddress");
    }

    kernel32_module = GetModuleHandleA("kernel32.dll");
    if (kernel32_module == NULL) {
        h_fatal_error(GetLastError(), "GetModuleHandleA");
    }

    pGetQueuedCompletionStatusEx = (sGetQueuedCompletionStatusEx)GetProcAddress(kernel32_module, "GetQueuedCompletionStatusEx");
    pGetNativeSystemInfo = (sGetNativeSystemInfo)GetProcAddress(kernel32_module, "GetNativeSystemInfo");
    pGetCurrentProcessorNumber = (sGetCurrentProcessorNumber)GetProcAddress(kernel32_module, "GetCurrentProcessorNumber");
    pGetThreadId = (sGetThreadId)GetProcAddress(kernel32_module, "GetThreadId");
    pSetFileCompletionNotificationModes = (sSetFileCompletionNotificationModes)GetProcAddress(kernel32_module, "SetFileCompletionNotificationModes");
    pCreateSymbolicLinkW = (sCreateSymbolicLinkW)GetProcAddress(kernel32_module, "CreateSymbolicLinkW");
    pCancelIoEx = (sCancelIoEx)GetProcAddress(kernel32_module, "CancelIoEx");
    pInitializeSRWLock = (sInitializeSRWLock)GetProcAddress(kernel32_module, "InitializeSRWLock");
    pAcquireSRWLockShared = (sAcquireSRWLockShared)GetProcAddress(kernel32_module, "AcquireSRWLockShared");
    pAcquireSRWLockExclusive = (sAcquireSRWLockExclusive)GetProcAddress(kernel32_module, "AcquireSRWLockExclusive");
    pTryAcquireSRWLockShared = (sTryAcquireSRWLockShared)GetProcAddress(kernel32_module, "TryAcquireSRWLockShared");
    pTryAcquireSRWLockExclusive = (sTryAcquireSRWLockExclusive)GetProcAddress(kernel32_module, "TryAcquireSRWLockExclusive");
    pReleaseSRWLockShared = (sReleaseSRWLockShared)GetProcAddress(kernel32_module, "ReleaseSRWLockShared");
    pReleaseSRWLockExclusive = (sReleaseSRWLockExclusive)GetProcAddress(kernel32_module, "ReleaseSRWLockExclusive");
    pInitializeConditionVariable = (sInitializeConditionVariable)GetProcAddress(kernel32_module, "InitializeConditionVariable");
    pSleepConditionVariableCS = (sSleepConditionVariableCS)GetProcAddress(kernel32_module, "SleepConditionVariableCS");
    pSleepConditionVariableSRW = (sSleepConditionVariableSRW)GetProcAddress(kernel32_module, "SleepConditionVariableSRW");
    pWakeAllConditionVariable = (sWakeAllConditionVariable)GetProcAddress(kernel32_module, "WakeAllConditionVariable");
    pWakeConditionVariable = (sWakeConditionVariable)GetProcAddress(kernel32_module, "WakeConditionVariable");
    pUnregisterWait = (sUnregisterWait)GetProcAddress(kernel32_module, "UnregisterWait");
    pUnregisterWaitEx = (sUnregisterWaitEx)GetProcAddress(kernel32_module, "UnregisterWaitEx");
    pRegisterWaitForSingleObject = (sRegisterWaitForSingleObject)GetProcAddress(kernel32_module, "RegisterWaitForSingleObject");
    pGlobalMemoryStatusEx = (sGlobalMemoryStatusEx)GetProcAddress(kernel32_module, "GlobalMemoryStatusEx");
    pQueueUserWorkItem = (sQueueUserWorkItem)GetProcAddress(kernel32_module, "QueueUserWorkItem");

    // http://msdn.microsoft.com/en-us/library/windows/desktop/ms683219(v=vs.85).aspx
    //  pGetProcessMemoryInfo = (sGetProcessMemoryInfo)
    //    GetProcAddress(kernel32_module, "GetProcessMemoryInfo");
    //  if(pGetProcessMemoryInfo == NULL) {
    //    pGetProcessMemoryInfo = (sGetProcessMemoryInfo)
    //      GetProcAddress(GetModuleHandleA("Psapi.dll"), "GetProcessMemoryInfo");
    //  }
    {
        HANDLE h = GetModuleHandleA("Iphlpapi.dll");
        if (!h) {
            h = LoadLibrary("Iphlpapi.dll");
        }
        if (h) {
            pGetAdaptersAddresses = (sGetAdaptersAddresses)GetProcAddress(h, "GetAdaptersAddresses");
        }
    }
    // liigo 2013-8-28
    // InterlockedCompareExchangePointer: This function is implemented using a compiler intrinsic where possible.
    // http://msdn.microsoft.com/en-us/library/windows/apps/ms683568(v=vs.85).aspx
    pInterlockedCompareExchange32 = (sInterlockedCompareExchange32)GetProcAddress(kernel32_module, "InterlockedCompareExchange");
    pInterlockedCompareExchange64 = (sInterlockedCompareExchange64)GetProcAddress(kernel32_module, "InterlockedCompareExchange64");
#if _WIN64
    pInterlockedCompareExchangePointer = (sInterlockedCompareExchangePointer)pInterlockedCompareExchange64;
#else
    pInterlockedCompareExchangePointer = (sInterlockedCompareExchangePointer)pInterlockedCompareExchange32;
#endif
    pGetTickCount64 = (sGetTickCount64)GetProcAddress(kernel32_module, "GetTickCount64");
    if(!pGetTickCount64) pGetTickCount64 = (sGetTickCount64)GetTickCount;
    {
        HANDLE ws2_dll = GetModuleHandleA("Ws2_32.dll");
        pFreeAddrInfoW = (sFreeAddrInfoW)GetProcAddress(ws2_dll, "FreeAddrInfoW");
        pGetAddrInfoW = (sGetAddrInfoW)GetProcAddress(ws2_dll, "GetAddrInfoW");
        pgetaddrinfo = (sGetaddrinfo)GetProcAddress(ws2_dll, "getaddrinfo");
        pfreeaddrinfo = (sFreeaddrinfo)GetProcAddress(ws2_dll, "freeaddrinfo");
        pWSAPoll = (sWSAPoll)GetProcAddress(ws2_dll, "WSAPoll");
    }
}
#endif
