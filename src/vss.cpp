#include "vss.h"

#include <winrt/base.h>

#include <vss.h>
#include <vswriter.h>
#include <vsbackup.h>

namespace dc {

// Scoped MTA apartment via C++/WinRT.
struct ApartmentGuard {
    ApartmentGuard() {
        try {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
        } catch (const winrt::hresult_error& e) {
            ThrowHr(ExitCode::VssFailure, L"init_apartment failed", e.code());
        }
    }
    ~ApartmentGuard() { winrt::uninit_apartment(); }
    ApartmentGuard(const ApartmentGuard&) = delete;
    ApartmentGuard& operator=(const ApartmentGuard&) = delete;
};

struct SnapshotSet::Impl {
    ApartmentGuard apartment;
    winrt::com_ptr<IVssBackupComponents> bc;
    std::vector<VSS_ID> snapshotIds;
    std::vector<std::wstring> volumes; // parallel to snapshotIds
    bool completed = false;
};

static void CheckHr(HRESULT hr, const wchar_t* what) {
    if (FAILED(hr)) ThrowHr(ExitCode::VssFailure, what, hr);
}

static void WaitAsync(IVssAsync* asyncRaw, const wchar_t* what) {
    winrt::com_ptr<IVssAsync> async;
    async.attach(asyncRaw);
    CheckHr(async->Wait(), what);
    HRESULT status = S_OK;
    CheckHr(async->QueryStatus(&status, nullptr), what);
    if (status != VSS_S_ASYNC_FINISHED)
        ThrowHr(ExitCode::VssFailure, what, status);
}

void SnapshotSet::Create(const std::vector<std::wstring>& volumeGuidPaths) {
    impl_ = new Impl();
    auto& bc = impl_->bc;

    // CoInitializeSecurity may already have been called process-wide; tolerate that.
    HRESULT hr = CoInitializeSecurity(nullptr, -1, nullptr, nullptr,
        RPC_C_AUTHN_LEVEL_PKT_PRIVACY, RPC_C_IMP_LEVEL_IMPERSONATE,
        nullptr, EOAC_NONE, nullptr);
    if (FAILED(hr) && hr != RPC_E_TOO_LATE)
        ThrowHr(ExitCode::VssFailure, L"CoInitializeSecurity failed", hr);

    CheckHr(CreateVssBackupComponents(bc.put()), L"CreateVssBackupComponents failed");
    CheckHr(bc->InitializeForBackup(), L"IVssBackupComponents::InitializeForBackup failed");
    CheckHr(bc->SetContext(VSS_CTX_BACKUP), L"SetContext(VSS_CTX_BACKUP) failed");
    // Copy-type backup: do not disturb writers' incremental/differential state.
    CheckHr(bc->SetBackupState(false, true, VSS_BT_COPY, false), L"SetBackupState failed");

    IVssAsync* async = nullptr;
    CheckHr(bc->GatherWriterMetadata(&async), L"GatherWriterMetadata failed");
    WaitAsync(async, L"GatherWriterMetadata did not finish");
    CheckHr(bc->FreeWriterMetadata(), L"FreeWriterMetadata failed");

    VSS_ID setId{};
    CheckHr(bc->StartSnapshotSet(&setId), L"StartSnapshotSet failed");

    for (const auto& vol : volumeGuidPaths) {
        VSS_ID snapId{};
        std::wstring v = vol;              // AddToSnapshotSet takes non-const
        HRESULT ahr = bc->AddToSnapshotSet(v.data(), GUID_NULL, &snapId);
        if (FAILED(ahr)) {
            fwprintf(stderr, L"warning: VSS cannot snapshot %s (0x%08X); will raw-copy live\n",
                vol.c_str(), static_cast<unsigned>(ahr));
            skipped_.push_back(vol);
            continue;
        }
        impl_->snapshotIds.push_back(snapId);
        impl_->volumes.push_back(vol);
    }
    if (impl_->snapshotIds.empty()) {
        if (!volumeGuidPaths.empty())
            fwprintf(stderr, L"warning: no volumes could be added to the VSS snapshot set\n");
        return;
    }

    CheckHr(bc->PrepareForBackup(&async), L"PrepareForBackup failed");
    WaitAsync(async, L"PrepareForBackup did not finish");

    fwprintf(stderr, L"Creating VSS snapshot (this may take a moment)...\n");
    CheckHr(bc->DoSnapshotSet(&async), L"DoSnapshotSet failed");
    WaitAsync(async, L"DoSnapshotSet did not finish");

    for (size_t i = 0; i < impl_->snapshotIds.size(); ++i) {
        VSS_SNAPSHOT_PROP prop{};
        CheckHr(impl_->bc->GetSnapshotProperties(impl_->snapshotIds[i], &prop),
            L"GetSnapshotProperties failed");
        struct PropGuard {
            VSS_SNAPSHOT_PROP* p;
            ~PropGuard() { VssFreeSnapshotProperties(p); }
        } guard{ &prop };
        shadowByVolume_[impl_->volumes[i]] = prop.m_pwszSnapshotDeviceObject;
    }
}

std::wstring SnapshotSet::ShadowDevice(const std::wstring& volumeGuidPath) const {
    auto it = shadowByVolume_.find(volumeGuidPath);
    return it == shadowByVolume_.end() ? L"" : it->second;
}

void SnapshotSet::Complete() {
    // Best-effort: by the time this runs the copy has succeeded and been
    // flushed. A writer hiccup during BackupComplete must not convert a valid
    // clone into a reported failure (this is a copy-type backup; no writer
    // state depends on it).
    if (!impl_ || !impl_->bc || impl_->snapshotIds.empty()) return;
    try {
        IVssAsync* async = nullptr;
        HRESULT hr = impl_->bc->BackupComplete(&async);
        if (SUCCEEDED(hr))
            WaitAsync(async, L"BackupComplete did not finish");
    } catch (const Error& e) {
        fwprintf(stderr, L"warning: VSS BackupComplete failed (%s); the copy itself is intact\n",
            e.message().c_str());
    }
    impl_->completed = true;
}

SnapshotSet::~SnapshotSet() {
    // Releasing the backup components releases VSS_CTX_BACKUP snapshots.
    delete impl_;
}

} // namespace dc
