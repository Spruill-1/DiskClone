#include "vss.h"

#include <winrt/base.h>

#include <vss.h>
#include <vswriter.h>
#include <vsbackup.h>

namespace DiskClone
{
    namespace
    {
        // Scoped MTA apartment via C++/WinRT.
        struct ApartmentGuard
        {
            ApartmentGuard()
            {
                try
                {
                    winrt::init_apartment(winrt::apartment_type::multi_threaded);
                }
                catch (const winrt::hresult_error& e)
                {
                    ThrowHr(ExitCode::VssFailure, L"init_apartment failed", e.code());
                }
            }
            ~ApartmentGuard() { winrt::uninit_apartment(); }

            ApartmentGuard(const ApartmentGuard&) = delete;
            ApartmentGuard& operator=(const ApartmentGuard&) = delete;
        };

        void CheckHr(HRESULT hr, const wchar_t* what)
        {
            if (FAILED(hr)) { ThrowHr(ExitCode::VssFailure, what, hr); }
        }

        void WaitAsync(IVssAsync* asyncRaw, const wchar_t* what)
        {
            winrt::com_ptr<IVssAsync> async;
            async.attach(asyncRaw);
            CheckHr(async->Wait(), what);
            HRESULT asyncStatus = S_OK;
            CheckHr(async->QueryStatus(&asyncStatus, nullptr), what);
            if (asyncStatus != VSS_S_ASYNC_FINISHED)
            {
                ThrowHr(ExitCode::VssFailure, what, asyncStatus);
            }
        }
    }

    struct SnapshotSet::Impl
    {
        // Member order matters for teardown: the components object must be
        // released (dropping the VSS_CTX_BACKUP snapshots) before the
        // apartment is uninitialized.
        ApartmentGuard apartment;
        winrt::com_ptr<IVssBackupComponents> backupComponents;
        std::vector<VSS_ID> snapshotIds;
        std::vector<std::wstring> volumes;   // parallel to snapshotIds
        bool completed{ false };
    };

    void SnapshotSet::Create(const std::vector<std::wstring>& volumeGuidPaths)
    {
        m_impl = std::make_unique<Impl>();
        auto& components = m_impl->backupComponents;

        // CoInitializeSecurity may already have been called process-wide; tolerate that.
        HRESULT hr = CoInitializeSecurity(nullptr, -1, nullptr, nullptr,
            RPC_C_AUTHN_LEVEL_PKT_PRIVACY, RPC_C_IMP_LEVEL_IMPERSONATE,
            nullptr, EOAC_NONE, nullptr);
        if (FAILED(hr) && hr != RPC_E_TOO_LATE)
        {
            ThrowHr(ExitCode::VssFailure, L"CoInitializeSecurity failed", hr);
        }

        CheckHr(CreateVssBackupComponents(components.put()), L"CreateVssBackupComponents failed");
        CheckHr(components->InitializeForBackup(), L"IVssBackupComponents::InitializeForBackup failed");
        CheckHr(components->SetContext(VSS_CTX_BACKUP), L"SetContext(VSS_CTX_BACKUP) failed");

        // Copy-type backup: do not disturb writers' incremental/differential state.
        CheckHr(components->SetBackupState(false, true, VSS_BT_COPY, false), L"SetBackupState failed");

        IVssAsync* async = nullptr;
        CheckHr(components->GatherWriterMetadata(&async), L"GatherWriterMetadata failed");
        WaitAsync(async, L"GatherWriterMetadata did not finish");
        CheckHr(components->FreeWriterMetadata(), L"FreeWriterMetadata failed");

        VSS_ID snapshotSetId{};
        CheckHr(components->StartSnapshotSet(&snapshotSetId), L"StartSnapshotSet failed");

        for (const auto& volumePath : volumeGuidPaths)
        {
            VSS_ID snapshotId{};
            std::wstring mutablePath = volumePath;   // AddToSnapshotSet takes non-const
            HRESULT addResult = components->AddToSnapshotSet(mutablePath.data(), GUID_NULL, &snapshotId);
            if (FAILED(addResult))
            {
                fwprintf(stderr, L"warning: VSS cannot snapshot %s (0x%08X); will copy via exclusive lock\n",
                    volumePath.c_str(), static_cast<unsigned>(addResult));
                m_skipped.push_back(volumePath);
                continue;
            }

            m_impl->snapshotIds.push_back(snapshotId);
            m_impl->volumes.push_back(volumePath);
        }

        if (m_impl->snapshotIds.empty())
        {
            if (!volumeGuidPaths.empty())
            {
                fwprintf(stderr, L"warning: no volumes could be added to the VSS snapshot set\n");
            }

            return;
        }

        CheckHr(components->PrepareForBackup(&async), L"PrepareForBackup failed");
        WaitAsync(async, L"PrepareForBackup did not finish");

        fwprintf(stderr, L"Creating VSS snapshot (this may take a moment)...\n");
        CheckHr(components->DoSnapshotSet(&async), L"DoSnapshotSet failed");
        WaitAsync(async, L"DoSnapshotSet did not finish");

        for (size_t i = 0; i < m_impl->snapshotIds.size(); ++i)
        {
            VSS_SNAPSHOT_PROP properties{};
            CheckHr(m_impl->backupComponents->GetSnapshotProperties(m_impl->snapshotIds[i], &properties),
                L"GetSnapshotProperties failed");
            auto propertiesCleanup = wil::scope_exit([&] { VssFreeSnapshotProperties(&properties); });

            m_shadowByVolume[m_impl->volumes[i]] = properties.m_pwszSnapshotDeviceObject;
        }
    }

    std::wstring SnapshotSet::ShadowDevice(const std::wstring& volumeGuidPath) const
    {
        auto found = m_shadowByVolume.find(volumeGuidPath);
        return found == m_shadowByVolume.end() ? L"" : found->second;
    }

    void SnapshotSet::Complete()
    {
        // Best-effort: by the time this runs the copy has succeeded and been
        // flushed. A writer hiccup during BackupComplete must not convert a
        // valid clone into a reported failure (this is a copy-type backup; no
        // writer state depends on it).
        if (!m_impl || !m_impl->backupComponents || m_impl->snapshotIds.empty()) { return; }
        try
        {
            IVssAsync* async = nullptr;
            HRESULT hr = m_impl->backupComponents->BackupComplete(&async);
            if (SUCCEEDED(hr))
            {
                WaitAsync(async, L"BackupComplete did not finish");
            }
        }
        catch (const Error& e)
        {
            fwprintf(stderr, L"warning: VSS BackupComplete failed (%s); the copy itself is intact\n",
                e.Message().c_str());
        }

        m_impl->completed = true;
    }

    // Defined here where Impl is complete; releasing the backup components
    // releases the VSS_CTX_BACKUP snapshots.
    SnapshotSet::SnapshotSet() = default;
    SnapshotSet::~SnapshotSet() = default;
}
