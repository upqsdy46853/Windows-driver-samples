#include <windows.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <functiondiscoverykeys_devpkey.h>
#include <propsys.h>

#include <atomic>
#include <algorithm>
#include <cstdlib>
#include <cwctype>
#include <cstring>
#include <iostream>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "ole32.lib")

namespace
{
    constexpr DWORD DBWIN_BUFFER_BYTES = 4096;

    struct DBWinBuffer
    {
        DWORD processId;
        char data[DBWIN_BUFFER_BYTES - sizeof(DWORD)];
    };

    struct SessionLogInfo
    {
        std::wstring endpointName;
        std::wstring sessionInstanceIdentifier;
        DWORD pid = 0;
        AudioSessionState state = AudioSessionStateInactive;
    };

    struct TextField
    {
        std::wstring key;
        std::wstring value;
    };

    struct ApoLineInfo
    {
        LONGLONG qpc = 0;
        std::wstring type;
        std::wstring eventName;
        std::wstring apoInstanceId;
        std::wstring streamId;
    };

    struct ApoStreamCandidate
    {
        LONGLONG qpc = 0;
        std::wstring type;
        std::wstring apoInstanceId;
        std::wstring streamId;
    };

    class SessionEvents;
    class SessionNotification;

    struct SessionItem
    {
        IAudioSessionControl* control = nullptr;
        SessionEvents* events = nullptr;
        SessionLogInfo info;
    };

    struct EndpointItem
    {
        IMMDevice* device = nullptr;
        IAudioSessionManager2* manager = nullptr;
        IAudioSessionEnumerator* sessionEnumerator = nullptr;
        SessionNotification* notification = nullptr;
        std::wstring endpointName;
    };

    std::atomic<bool> g_running = true;
    std::mutex g_stateLock;
    std::mutex g_printLock;
    std::vector<SessionItem> g_sessions;
    std::vector<EndpointItem> g_endpoints;
    std::set<std::wstring> g_matchedApoInstances;
    std::vector<ApoStreamCandidate> g_recentApoStreamCandidates;

    std::wstring StateToString(AudioSessionState state)
    {
        switch (state)
        {
        case AudioSessionStateInactive: return L"Inactive";
        case AudioSessionStateActive: return L"Active";
        case AudioSessionStateExpired: return L"Expired";
        default: return L"Unknown";
        }
    }

    LONGLONG GetQpc()
    {
        LARGE_INTEGER qpc = {};
        QueryPerformanceCounter(&qpc);
        return qpc.QuadPart;
    }

    std::wstring ToLowerInvariant(std::wstring value)
    {
        for (wchar_t& ch : value)
        {
            ch = static_cast<wchar_t>(std::towlower(ch));
        }

        return value;
    }

    bool EqualsInsensitive(const std::wstring& left, const std::wstring& right)
    {
        return ToLowerInvariant(left) == ToLowerInvariant(right);
    }

    std::wstring NormalizeGuidString(const std::wstring& value)
    {
        GUID guid = {};
        if (FAILED(CLSIDFromString(value.c_str(), &guid)))
        {
            return L"";
        }

        wchar_t buffer[64] = {};
        if (StringFromGUID2(guid, buffer, ARRAYSIZE(buffer)) == 0)
        {
            return L"";
        }

        return ToLowerInvariant(buffer);
    }

    std::wstring LastGuidInText(const std::wstring& text)
    {
        std::wstring result;

        size_t start = text.find(L'{');
        while (start != std::wstring::npos)
        {
            const size_t end = text.find(L'}', start + 1);
            if (end == std::wstring::npos)
            {
                break;
            }

            const size_t length = end - start + 1;
            if (length >= 38 && length <= 64)
            {
                const std::wstring normalized = NormalizeGuidString(text.substr(start, length));
                if (!normalized.empty())
                {
                    result = normalized;
                }
            }

            start = text.find(L'{', end + 1);
        }

        return result;
    }

    std::vector<TextField> ParseBracketFields(const std::wstring& line)
    {
        std::vector<TextField> fields;
        size_t start = line.find(L'[');
        while (start != std::wstring::npos)
        {
            const size_t end = line.find(L']', start + 1);
            if (end == std::wstring::npos)
            {
                break;
            }

            const std::wstring field = line.substr(start + 1, end - start - 1);
            const size_t equals = field.find(L'=');
            if (equals != std::wstring::npos)
            {
                TextField parsed;
                parsed.key = field.substr(0, equals);
                parsed.value = field.substr(equals + 1);
                fields.push_back(parsed);
            }

            start = line.find(L'[', end + 1);
        }

        return fields;
    }

    std::wstring FindFieldValue(const std::vector<TextField>& fields, const wchar_t* key)
    {
        for (const TextField& field : fields)
        {
            if (EqualsInsensitive(field.key, key))
            {
                return field.value;
            }
        }

        return L"";
    }

    std::wstring FindFirstFieldValue(const std::vector<TextField>& fields, const wchar_t* const* keys, size_t keyCount)
    {
        for (size_t i = 0; i < keyCount; ++i)
        {
            const std::wstring value = FindFieldValue(fields, keys[i]);
            if (!value.empty())
            {
                return value;
            }
        }

        return L"";
    }

    LONGLONG ParseInt64Field(const std::wstring& value)
    {
        if (value.empty())
        {
            return 0;
        }

        wchar_t* end = nullptr;
        const long long parsed = std::wcstoll(value.c_str(), &end, 0);
        if (end == value.c_str())
        {
            return 0;
        }

        return static_cast<LONGLONG>(parsed);
    }

    double QpcDeltaMilliseconds(LONGLONG newerQpc, LONGLONG olderQpc)
    {
        LARGE_INTEGER frequency = {};
        QueryPerformanceFrequency(&frequency);
        if (frequency.QuadPart == 0)
        {
            return 0.0;
        }

        return (static_cast<double>(newerQpc - olderQpc) * 1000.0) /
            static_cast<double>(frequency.QuadPart);
    }

    std::wstring SanitizeValue(std::wstring value)
    {
        for (wchar_t& ch : value)
        {
            if (ch == L'\r' || ch == L'\n' || ch == L'\t')
            {
                ch = L' ';
            }
            else if (ch == L']')
            {
                ch = L'}';
            }
        }
        return value;
    }

    std::wstring LogPrefix(const wchar_t* type, const wchar_t* eventName)
    {
        std::wstringstream stream;
        stream << L"[QPC=" << GetQpc() << L"][Type=" << type << L"][Event=" << eventName << L"]";
        return stream.str();
    }

    std::wstring GetEndpointFriendlyName(IMMDevice* device)
    {
        if (!device)
        {
            return L"(Unknown Endpoint)";
        }

        IPropertyStore* properties = nullptr;
        HRESULT hr = device->OpenPropertyStore(STGM_READ, &properties);
        if (FAILED(hr) || !properties)
        {
            return L"(Unknown Endpoint)";
        }

        PROPVARIANT value;
        PropVariantInit(&value);
        std::wstring result = L"(Unknown Endpoint)";

        hr = properties->GetValue(PKEY_Device_FriendlyName, &value);
        if (SUCCEEDED(hr) && value.vt == VT_LPWSTR && value.pwszVal)
        {
            result = value.pwszVal;
        }

        PropVariantClear(&value);
        properties->Release();
        return result;
    }

    std::wstring TakeCoTaskMemString(LPWSTR value)
    {
        if (value == nullptr)
        {
            return L"";
        }

        std::wstring result(value);
        CoTaskMemFree(value);
        return result;
    }

    void PrintSessionLine(const wchar_t* eventName, const SessionLogInfo& info)
    {
        std::lock_guard<std::mutex> printLock(g_printLock);
        std::wcout
            << LogPrefix(L"Session", eventName)
            << L"[EndpointName=" << SanitizeValue(info.endpointName) << L"]"
            << L"[State=" << StateToString(info.state) << L"]"
            << L"[PID=" << info.pid << L"]"
            << L"[SessionInstanceIdentifier=" << SanitizeValue(info.sessionInstanceIdentifier) << L"]"
            << std::endl;
    }

    std::wstring AnsiToWide(const std::string& text)
    {
        if (text.empty())
        {
            return L"";
        }

        UINT codePage = CP_UTF8;
        DWORD flags = MB_ERR_INVALID_CHARS;
        int chars = MultiByteToWideChar(codePage, flags, text.data(), static_cast<int>(text.size()), nullptr, 0);
        if (chars == 0)
        {
            codePage = CP_ACP;
            flags = 0;
            chars = MultiByteToWideChar(codePage, flags, text.data(), static_cast<int>(text.size()), nullptr, 0);
        }

        if (chars == 0)
        {
            return L"";
        }

        std::wstring result(chars, L'\0');
        MultiByteToWideChar(codePage, flags, text.data(), static_cast<int>(text.size()), &result[0], chars);
        return result;
    }

    ApoLineInfo ParseApoLineInfo(const std::wstring& line)
    {
        const std::vector<TextField> fields = ParseBracketFields(line);

        ApoLineInfo info;
        info.qpc = ParseInt64Field(FindFieldValue(fields, L"QPC"));
        info.type = FindFieldValue(fields, L"Type");
        info.eventName = FindFieldValue(fields, L"Event");
        if (info.eventName.empty())
        {
            info.eventName = FindFieldValue(fields, L"Method");
        }

        const wchar_t* apoInstanceKeys[] = { L"ApoInstance", L"APOInstance", L"Instance", L"This", L"this", L"Object", L"Obj", L"APO", L"Apo" };

        info.apoInstanceId = FindFirstFieldValue(fields, apoInstanceKeys, ARRAYSIZE(apoInstanceKeys));
        info.streamId = FindFieldValue(fields, L"StreamId");

        return info;
    }

    void RememberRecentApoStreamCandidate(const ApoLineInfo& apoInfo)
    {
        if (!EqualsInsensitive(apoInfo.eventName, L"LockForProcessLeave") ||
            apoInfo.apoInstanceId.empty() ||
            apoInfo.streamId.empty())
        {
            return;
        }

        ApoStreamCandidate candidate;
        candidate.qpc = apoInfo.qpc != 0 ? apoInfo.qpc : GetQpc();
        candidate.type = apoInfo.type;
        candidate.apoInstanceId = apoInfo.apoInstanceId;
        candidate.streamId = apoInfo.streamId;

        const LONGLONG now = GetQpc();
        std::lock_guard<std::mutex> lock(g_stateLock);
        if (g_matchedApoInstances.find(ToLowerInvariant(candidate.apoInstanceId)) !=
            g_matchedApoInstances.end())
        {
            return;
        }

        g_recentApoStreamCandidates.erase(
            std::remove_if(
                g_recentApoStreamCandidates.begin(),
                g_recentApoStreamCandidates.end(),
                [&](const ApoStreamCandidate& existing)
                {
                    return QpcDeltaMilliseconds(now, existing.qpc) > 10000.0;
                }),
            g_recentApoStreamCandidates.end());
        g_recentApoStreamCandidates.push_back(candidate);
    }

    bool CorrelateRecentApoStreamToSession(const SessionLogInfo& session, LONGLONG sessionEventQpc)
    {
        if (session.sessionInstanceIdentifier.empty())
        {
            return false;
        }

        ApoStreamCandidate candidate;
        bool found = false;

        {
            std::lock_guard<std::mutex> lock(g_stateLock);

            auto best = g_recentApoStreamCandidates.end();
            for (auto it = g_recentApoStreamCandidates.begin(); it != g_recentApoStreamCandidates.end(); ++it)
            {
                if (it->qpc == 0 || it->qpc > sessionEventQpc)
                {
                    continue;
                }

                if (g_matchedApoInstances.find(ToLowerInvariant(it->apoInstanceId)) !=
                    g_matchedApoInstances.end())
                {
                    continue;
                }

                const double deltaMs = QpcDeltaMilliseconds(sessionEventQpc, it->qpc);
                if (deltaMs < 0.0 || deltaMs > 2000.0)
                {
                    continue;
                }

                found = true;
                best = it;
                break;
            }

            if (!found)
            {
                return false;
            }

            candidate = *best;
            g_recentApoStreamCandidates.erase(best);
            g_matchedApoInstances.insert(ToLowerInvariant(candidate.apoInstanceId));
        }

        std::lock_guard<std::mutex> printLock(g_printLock);
        std::wcout
            << LogPrefix(L"APOMatch", L"Matched")
            << L"[ApoType=" << SanitizeValue(candidate.type) << L"]"
            << L"[ApoInstance=" << SanitizeValue(candidate.apoInstanceId) << L"]"
            << L"[StreamId=" << SanitizeValue(candidate.streamId) << L"]"
            << L"[SessionGuid=" << SanitizeValue(LastGuidInText(session.sessionInstanceIdentifier)) << L"]"
            << L"[PID=" << session.pid << L"]"
            << std::endl;

        return true;
    }

    void HandleApoLine(std::wstring line)
    {
        while (!line.empty() && (line.back() == L'\r' || line.back() == L'\n'))
        {
            line.pop_back();
        }

        if (line.rfind(L"[Time=", 0) == 0)
        {
            const size_t close = line.find(L']');
            if (close != std::wstring::npos)
            {
                line.erase(0, close + 1);
            }
        }

        const ApoLineInfo apoInfo = ParseApoLineInfo(line);
        RememberRecentApoStreamCandidate(apoInfo);

        std::lock_guard<std::mutex> printLock(g_printLock);
        std::wcout << L"[APO] " << line << std::endl;
    }

    struct DbWinHandles
    {
        HANDLE mapping = nullptr;
        HANDLE bufferReady = nullptr;
        HANDLE dataReady = nullptr;
        DBWinBuffer* buffer = nullptr;
    };

    void CloseDbWinHandles(DbWinHandles& handles)
    {
        if (handles.buffer)
        {
            UnmapViewOfFile(handles.buffer);
            handles.buffer = nullptr;
        }

        if (handles.mapping)
        {
            CloseHandle(handles.mapping);
            handles.mapping = nullptr;
        }

        if (handles.bufferReady)
        {
            CloseHandle(handles.bufferReady);
            handles.bufferReady = nullptr;
        }

        if (handles.dataReady)
        {
            CloseHandle(handles.dataReady);
            handles.dataReady = nullptr;
        }
    }

    bool CreateDbWinHandles(const wchar_t* prefix, DbWinHandles& handles)
    {
        SECURITY_DESCRIPTOR securityDescriptor;
        InitializeSecurityDescriptor(&securityDescriptor, SECURITY_DESCRIPTOR_REVISION);
        SetSecurityDescriptorDacl(&securityDescriptor, TRUE, nullptr, FALSE);

        SECURITY_ATTRIBUTES securityAttributes = {};
        securityAttributes.nLength = sizeof(securityAttributes);
        securityAttributes.lpSecurityDescriptor = &securityDescriptor;
        securityAttributes.bInheritHandle = FALSE;

        std::wstring scope(prefix);
        const std::wstring bufferName = scope + L"DBWIN_BUFFER";
        const std::wstring bufferReadyName = scope + L"DBWIN_BUFFER_READY";
        const std::wstring dataReadyName = scope + L"DBWIN_DATA_READY";

        handles.mapping = CreateFileMappingW(
            INVALID_HANDLE_VALUE,
            &securityAttributes,
            PAGE_READWRITE,
            0,
            sizeof(DBWinBuffer),
            bufferName.c_str());
        if (!handles.mapping)
        {
            CloseDbWinHandles(handles);
            return false;
        }

        handles.buffer = static_cast<DBWinBuffer*>(
            MapViewOfFile(handles.mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(DBWinBuffer)));
        if (!handles.buffer)
        {
            CloseDbWinHandles(handles);
            return false;
        }

        handles.bufferReady = CreateEventW(&securityAttributes, FALSE, FALSE, bufferReadyName.c_str());
        handles.dataReady = CreateEventW(&securityAttributes, FALSE, FALSE, dataReadyName.c_str());
        if (!handles.bufferReady || !handles.dataReady)
        {
            CloseDbWinHandles(handles);
            return false;
        }

        return true;
    }

    void DebugOutputThread()
    {
        DbWinHandles handles;
        if (!CreateDbWinHandles(L"Global\\", handles) &&
            !CreateDbWinHandles(L"", handles))
        {
            return;
        }

        while (g_running.load())
        {
            SetEvent(handles.bufferReady);

            DWORD wait = WaitForSingleObject(handles.dataReady, 250);
            if (wait != WAIT_OBJECT_0)
            {
                continue;
            }

            const size_t length = strnlen_s(handles.buffer->data, sizeof(handles.buffer->data));
            if (length == 0)
            {
                continue;
            }

            std::string text(handles.buffer->data, length);
            if (text.find("[Type=MFX]") == std::string::npos &&
                text.find("[Type=SFX]") == std::string::npos)
            {
                continue;
            }

            HandleApoLine(AnsiToWide(text));
        }

        CloseDbWinHandles(handles);
    }

    class SessionEvents : public IAudioSessionEvents
    {
    public:
        SessionEvents(std::wstring endpointName, std::wstring sessionInstanceIdentifier, DWORD pid)
            : m_endpointName(std::move(endpointName)),
            m_sessionInstanceIdentifier(std::move(sessionInstanceIdentifier)),
            m_pid(pid)
        {
        }

        ULONG STDMETHODCALLTYPE AddRef() override
        {
            return InterlockedIncrement(&m_refCount);
        }

        ULONG STDMETHODCALLTYPE Release() override
        {
            LONG count = InterlockedDecrement(&m_refCount);
            if (count == 0)
            {
                delete this;
            }

            return count;
        }

        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override
        {
            if (!object)
            {
                return E_POINTER;
            }

            if (riid == __uuidof(IUnknown) || riid == __uuidof(IAudioSessionEvents))
            {
                *object = static_cast<IAudioSessionEvents*>(this);
                AddRef();
                return S_OK;
            }

            *object = nullptr;
            return E_NOINTERFACE;
        }

        HRESULT STDMETHODCALLTYPE OnStateChanged(AudioSessionState newState) override
        {
            const LONGLONG eventQpc = GetQpc();
            SessionLogInfo logInfo;
            bool found = false;

            {
                std::lock_guard<std::mutex> lock(g_stateLock);
                for (SessionItem& session : g_sessions)
                {
                    if (session.info.sessionInstanceIdentifier == m_sessionInstanceIdentifier)
                    {
                        session.info.state = newState;
                        logInfo = session.info;
                        found = true;
                        break;
                    }
                }
            }

            if (!found)
            {
                logInfo.endpointName = m_endpointName;
                logInfo.sessionInstanceIdentifier = m_sessionInstanceIdentifier;
                logInfo.pid = m_pid;
                logInfo.state = newState;
            }

            PrintSessionLine(L"StateChanged", logInfo);
            if (newState == AudioSessionStateActive)
            {
                CorrelateRecentApoStreamToSession(logInfo, eventQpc);
            }
            return S_OK;
        }

        HRESULT STDMETHODCALLTYPE OnDisplayNameChanged(LPCWSTR, LPCGUID) override { return S_OK; }
        HRESULT STDMETHODCALLTYPE OnIconPathChanged(LPCWSTR, LPCGUID) override { return S_OK; }
        HRESULT STDMETHODCALLTYPE OnSimpleVolumeChanged(float, BOOL, LPCGUID) override { return S_OK; }
        HRESULT STDMETHODCALLTYPE OnChannelVolumeChanged(DWORD, float[], DWORD, LPCGUID) override { return S_OK; }
        HRESULT STDMETHODCALLTYPE OnGroupingParamChanged(LPCGUID, LPCGUID) override { return S_OK; }

        HRESULT STDMETHODCALLTYPE OnSessionDisconnected(AudioSessionDisconnectReason reason) override
        {
            std::lock_guard<std::mutex> printLock(g_printLock);
            std::wcout
                << LogPrefix(L"Session", L"Disconnected")
                << L"[EndpointName=" << SanitizeValue(m_endpointName) << L"]"
                << L"[PID=" << m_pid << L"]"
                << L"[Reason=" << reason << L"]"
                << L"[SessionInstanceIdentifier=" << SanitizeValue(m_sessionInstanceIdentifier) << L"]"
                << std::endl;
            return S_OK;
        }

    private:
        volatile LONG m_refCount = 1;
        std::wstring m_endpointName;
        std::wstring m_sessionInstanceIdentifier;
        DWORD m_pid = 0;
    };

    void RegisterSession(IAudioSessionControl* control, const std::wstring& endpointName)
    {
        if (!control)
        {
            return;
        }

        IAudioSessionControl2* control2 = nullptr;
        DWORD pid = 0;
        AudioSessionState state = AudioSessionStateInactive;
        std::wstring sessionInstanceIdentifier;

        HRESULT hr = control->QueryInterface(__uuidof(IAudioSessionControl2), reinterpret_cast<void**>(&control2));
        if (SUCCEEDED(hr) && control2)
        {
            LPWSTR rawSessionInstanceIdentifier = nullptr;

            control2->GetProcessId(&pid);
            if (SUCCEEDED(control2->GetSessionInstanceIdentifier(&rawSessionInstanceIdentifier)))
            {
                sessionInstanceIdentifier = TakeCoTaskMemString(rawSessionInstanceIdentifier);
            }

            control2->Release();
        }

        control->GetState(&state);

        {
            std::lock_guard<std::mutex> lock(g_stateLock);
            for (const SessionItem& session : g_sessions)
            {
                if (!sessionInstanceIdentifier.empty() &&
                    session.info.sessionInstanceIdentifier == sessionInstanceIdentifier)
                {
                    return;
                }
            }
        }

        SessionLogInfo info;
        info.endpointName = endpointName;
        info.sessionInstanceIdentifier = sessionInstanceIdentifier;
        info.pid = pid;
        info.state = state;

        SessionEvents* events = new SessionEvents(endpointName, sessionInstanceIdentifier, pid);
        hr = control->RegisterAudioSessionNotification(events);
        if (FAILED(hr))
        {
            events->Release();
            return;
        }

        control->AddRef();

        {
            std::lock_guard<std::mutex> lock(g_stateLock);
            SessionItem item;
            item.control = control;
            item.events = events;
            item.info = info;
            g_sessions.push_back(item);
        }

        PrintSessionLine(L"Register", info);
    }

    class SessionNotification : public IAudioSessionNotification
    {
    public:
        explicit SessionNotification(std::wstring endpointName)
            : m_endpointName(std::move(endpointName))
        {
        }

        ULONG STDMETHODCALLTYPE AddRef() override
        {
            return InterlockedIncrement(&m_refCount);
        }

        ULONG STDMETHODCALLTYPE Release() override
        {
            LONG count = InterlockedDecrement(&m_refCount);
            if (count == 0)
            {
                delete this;
            }

            return count;
        }

        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override
        {
            if (!object)
            {
                return E_POINTER;
            }

            if (riid == __uuidof(IUnknown) || riid == __uuidof(IAudioSessionNotification))
            {
                *object = static_cast<IAudioSessionNotification*>(this);
                AddRef();
                return S_OK;
            }

            *object = nullptr;
            return E_NOINTERFACE;
        }

        HRESULT STDMETHODCALLTYPE OnSessionCreated(IAudioSessionControl* newSession) override
        {
            {
                std::lock_guard<std::mutex> printLock(g_printLock);
                std::wcout
                    << LogPrefix(L"Session", L"Created")
                    << L"[EndpointName=" << SanitizeValue(m_endpointName) << L"]"
                    << std::endl;
            }

            RegisterSession(newSession, m_endpointName);
            return S_OK;
        }

    private:
        volatile LONG m_refCount = 1;
        std::wstring m_endpointName;
    };

    HRESULT RegisterEndpointSessions(IMMDevice* device)
    {
        EndpointItem endpoint;
        endpoint.device = device;
        endpoint.device->AddRef();
        endpoint.endpointName = GetEndpointFriendlyName(device);

        HRESULT hr = device->Activate(
            __uuidof(IAudioSessionManager2),
            CLSCTX_ALL,
            nullptr,
            reinterpret_cast<void**>(&endpoint.manager));
        if (FAILED(hr))
        {
            endpoint.device->Release();
            return hr;
        }

        endpoint.notification = new SessionNotification(endpoint.endpointName);
        hr = endpoint.manager->RegisterSessionNotification(endpoint.notification);
        if (FAILED(hr))
        {
            endpoint.notification->Release();
            endpoint.manager->Release();
            endpoint.device->Release();
            return hr;
        }

        hr = endpoint.manager->GetSessionEnumerator(&endpoint.sessionEnumerator);
        if (FAILED(hr))
        {
            endpoint.manager->UnregisterSessionNotification(endpoint.notification);
            endpoint.notification->Release();
            endpoint.manager->Release();
            endpoint.device->Release();
            return hr;
        }

        int count = 0;
        hr = endpoint.sessionEnumerator->GetCount(&count);
        if (SUCCEEDED(hr))
        {
            for (int i = 0; i < count; ++i)
            {
                IAudioSessionControl* control = nullptr;
                if (SUCCEEDED(endpoint.sessionEnumerator->GetSession(i, &control)) && control)
                {
                    RegisterSession(control, endpoint.endpointName);
                    control->Release();
                }
            }
        }

        g_endpoints.push_back(endpoint);
        return S_OK;
    }

    void Cleanup()
    {
        for (EndpointItem& endpoint : g_endpoints)
        {
            if (endpoint.manager && endpoint.notification)
            {
                endpoint.manager->UnregisterSessionNotification(endpoint.notification);
            }
        }

        for (SessionItem& session : g_sessions)
        {
            if (session.control && session.events)
            {
                session.control->UnregisterAudioSessionNotification(session.events);
            }
            if (session.events)
            {
                session.events->Release();
            }
            if (session.control)
            {
                session.control->Release();
            }
        }

        g_sessions.clear();
        g_matchedApoInstances.clear();

        for (EndpointItem& endpoint : g_endpoints)
        {
            if (endpoint.notification)
            {
                endpoint.notification->Release();
            }
            if (endpoint.sessionEnumerator)
            {
                endpoint.sessionEnumerator->Release();
            }
            if (endpoint.manager)
            {
                endpoint.manager->Release();
            }
            if (endpoint.device)
            {
                endpoint.device->Release();
            }
        }

        g_endpoints.clear();
    }
}

int wmain()
{
    SetConsoleOutputCP(CP_UTF8);

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr))
    {
        std::wcerr << L"CoInitializeEx failed: 0x" << std::hex << hr << std::endl;
        return 1;
    }

    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDeviceCollection* devices = nullptr;

    hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator),
        nullptr,
        CLSCTX_ALL,
        IID_PPV_ARGS(&enumerator));
    if (FAILED(hr))
    {
        std::wcerr << L"CoCreateInstance(MMDeviceEnumerator) failed: 0x" << std::hex << hr << std::endl;
        CoUninitialize();
        return 1;
    }

    hr = enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &devices);
    if (FAILED(hr))
    {
        std::wcerr << L"EnumAudioEndpoints failed: 0x" << std::hex << hr << std::endl;
        enumerator->Release();
        CoUninitialize();
        return 1;
    }

    UINT deviceCount = 0;
    devices->GetCount(&deviceCount);

    for (UINT i = 0; i < deviceCount; ++i)
    {
        IMMDevice* device = nullptr;
        if (SUCCEEDED(devices->Item(i, &device)) && device)
        {
            RegisterEndpointSessions(device);
            device->Release();
        }
    }

    std::thread debugThread(DebugOutputThread);

    {
        std::lock_guard<std::mutex> printLock(g_printLock);
        std::wcout << std::endl;
        std::wcout << L"Monitoring audio sessions and SwapAPO debug output." << std::endl;
        std::wcout << L"Run this as Administrator if APO debug output does not appear." << std::endl;
        std::wcout << L"Press Enter to exit." << std::endl;
    }

    std::wcin.get();
    g_running.store(false);

    if (debugThread.joinable())
    {
        debugThread.join();
    }

    Cleanup();

    devices->Release();
    enumerator->Release();
    CoUninitialize();
    return 0;
}
