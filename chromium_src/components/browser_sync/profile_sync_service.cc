#include "components/signin/core/browser/account_info.h"
#include "components/sync/engine/sync_credentials.h"
namespace browser_sync {
namespace {
syncer::SyncCredentials GetDummyCredentials();
AccountInfo GetDummyAccountInfo();
}
}
#include "../../../../components/browser_sync/profile_sync_service.cc"

#include "brave/components/brave_sync/brave_sync_prefs.h"
#include "brave/components/brave_sync/brave_sync_service_observer.h"
#include "brave/components/brave_sync/jslib_const.h"
#include "brave/components/brave_sync/jslib_messages.h"
#include "brave/components/brave_sync/settings.h"
#include "brave/components/brave_sync/sync_devices.h"
#include "brave/components/brave_sync/tools.h"
#include "brave/components/brave_sync/values_conv.h"
#include "content/public/browser/browser_thread.h"
#include "net/base/network_interfaces.h"

namespace browser_sync {

using brave_sync::jslib::Device;
using brave_sync::jslib::SyncRecord;
using brave_sync::jslib_const::SyncObjectData_DEVICE;
using brave_sync::jslib_const::SyncRecordType_PREFERENCES;
using brave_sync::RecordsList;
using brave_sync::RecordsListPtr;
using brave_sync::StrFromUint8Array;
using brave_sync::SyncDevice;
using brave_sync::SyncRecordPtr;
using brave_sync::tools::IsTimeEmpty;
using brave_sync::Uint8Array;
using brave_sync::Uint8ArrayFromString;

namespace {

void NotifyLogMessage(const std::string& message) {
  DLOG(INFO) << message;
}

std::string GetDeviceName() {
  std::string hostname = net::GetHostName();
  if (hostname.empty()) {
#if defined(OS_MACOSX)
    hostname = std::string("Mac Desktop");
#elif defined(OS_LINUX)
    hostname = std::string("Linux Desktop");
#elif defined(OS_WIN)
    hostname = std::string("Windows Desktop");
#endif
    }
  return hostname;
}

syncer::SyncCredentials GetDummyCredentials() {
  syncer::SyncCredentials credentials;
  credentials.account_id = "dummy_account_id";
  credentials.email = "dummy_email";
  credentials.sync_token = "dummy_access_token";
  return credentials;
}

AccountInfo GetDummyAccountInfo() {
  AccountInfo account_info;
  account_info.account_id = "dummy_account_id";
  return account_info;
}

RecordsListPtr CreateDeviceCreationRecordExtension(
  const std::string& deviceName,
  const std::string& objectId,
  const SyncRecord::Action &action,
  const std::string& deviceId) {
  RecordsListPtr records = std::make_unique<RecordsList>();

  SyncRecordPtr record = std::make_unique<SyncRecord>();

  record->action = action;
  record->deviceId = deviceId;
  record->objectId = objectId;
  record->objectData = SyncObjectData_DEVICE;  // "device"

  std::unique_ptr<Device> device = std::make_unique<Device>();
  device->name = deviceName;
  record->SetDevice(std::move(device));

  records->emplace_back(std::move(record));

  return records;
}

}

void ProfileSyncService::OnSetupSyncHaveCode(const std::string& sync_words,
    const std::string& device_name) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  if (sync_words.empty()) {
    OnSyncSetupError("ERR_SYNC_WRONG_WORDS");
    return;
  }

  if (brave_sync_initializing_) {
    NotifyLogMessage("currently initializing");
    return;
  }

  if (brave_sync_configured_) {
    NotifyLogMessage("already configured");
    return;
  }

  if (device_name.empty())
    brave_sync_prefs_->SetThisDeviceName(GetDeviceName());
  else
    brave_sync_prefs_->SetThisDeviceName(device_name);
  brave_sync_initializing_ = true;

  brave_sync_prefs_->SetSyncEnabled(true);
  brave_sync_words_ = sync_words;
}

void ProfileSyncService::OnSetupSyncNewToSync(
    const std::string& device_name) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);

  if (brave_sync_initializing_) {
    NotifyLogMessage("currently initializing");
    return;
  }

  if (brave_sync_configured_) {
    NotifyLogMessage("already configured");
    return;
  }

  brave_sync_words_.clear();  // If the previous attempt was connect to sync chain
                        // and failed to receive save-init-data
  if (device_name.empty())
    brave_sync_prefs_->SetThisDeviceName(GetDeviceName());
  else
    brave_sync_prefs_->SetThisDeviceName(device_name);

  brave_sync_initializing_ = true;

  brave_sync_prefs_->SetSyncEnabled(true);
}

void ProfileSyncService::OnDeleteDevice(const std::string& device_id) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  auto sync_devices = brave_sync_prefs_->GetSyncDevices();

  const SyncDevice *device = sync_devices->GetByDeviceId(device_id);
  if (device) {
    const std::string device_name = device->name_;
    const std::string object_id = device->object_id_;
    SendDeviceSyncRecord(
        SyncRecord::Action::A_DELETE, device_name, device_id, object_id);
  }
}

void ProfileSyncService::OnResetSync() {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  auto sync_devices = brave_sync_prefs_->GetSyncDevices();

  if (sync_devices->size() == 0) {
    // Fail safe option
    VLOG(2) << "[Sync] " << __func__ << " unexpected zero device size";
    ResetSyncInternal();
  } else {
    // We have to send delete record and wait for library deleted response then
    // we can reset it by ResetInternal()
    const std::string device_id = brave_sync_prefs_->GetThisDeviceId();
    OnDeleteDevice(device_id);
  }
}

void ProfileSyncService::GetSettingsAndDevices(
    const GetSettingsAndDevicesCallback& callback) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  auto settings = brave_sync_prefs_->GetBraveSyncSettings();
  auto devices = brave_sync_prefs_->GetSyncDevices();
  callback.Run(std::move(settings), std::move(devices));
}

void ProfileSyncService::GetSyncWords() {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  // Ask sync client
  std::string seed = brave_sync_prefs_->GetSeed();
  GetBraveSyncClient()->NeedSyncWords(seed);
}

std::string ProfileSyncService::GetSeed() {
  return brave_sync_prefs_->GetSeed();
}

void ProfileSyncService::OnSetSyncEnabled(const bool sync_this_device) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  brave_sync_prefs_->SetSyncEnabled(true);
}

void ProfileSyncService::OnSetSyncBookmarks(const bool sync_bookmarks) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  // POC so only do bookmarks 
  syncer::ModelTypeSet type_set = user_settings_->GetChosenDataTypes();
  if (sync_bookmarks)
    type_set.Put(syncer::BOOKMARKS);
  else
    type_set.Remove(syncer::BOOKMARKS);
  user_settings_->SetChosenDataTypes(false, type_set);
  brave_sync_prefs_->SetSyncBookmarksEnabled(sync_bookmarks);
}

void ProfileSyncService::OnSetSyncBrowsingHistory(
    const bool sync_browsing_history) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
#if 0
  brave_sync_prefs_->SetSyncHistoryEnabled(sync_browsing_history);
#endif
}

void ProfileSyncService::OnSetSyncSavedSiteSettings(
    const bool sync_saved_site_settings) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
#if 0
  brave_sync_prefs_->SetSyncSiteSettingsEnabled(sync_saved_site_settings);
#endif
}

void ProfileSyncService::BackgroundSyncStarted(bool startup) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
#if 0
  if (startup)
    bookmark_change_processor_->Start();

  StartLoop();
#endif
}

void ProfileSyncService::BackgroundSyncStopped(bool shutdown) {
#if 0
  if (shutdown)
    Shutdown();
  else
    StopLoop();
#endif
}

void ProfileSyncService::OnSyncDebug(const std::string& message) {
  NotifyLogMessage(message);
}

void ProfileSyncService::OnSyncSetupError(const std::string& error) {
  if (brave_sync_initializing_) {
    brave_sync_prefs_->Clear();
    brave_sync_initializing_ = false;
  }
  NotifySyncSetupError(error);
}

void ProfileSyncService::OnGetInitData(const std::string& sync_version) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);

  Uint8Array seed;
  if (!brave_sync_words_.empty()) {
    VLOG(1) << "[Brave Sync] Init from sync words";
  } else if (!brave_sync_prefs_->GetSeed().empty()) {
    seed = Uint8ArrayFromString(brave_sync_prefs_->GetSeed());
    VLOG(1) << "[Brave Sync] Init from prefs";
  } else {
    VLOG(1) << "[Brave Sync] Init new chain";
  }

  Uint8Array device_id;
  if (!brave_sync_prefs_->GetThisDeviceId().empty()) {
    device_id = Uint8ArrayFromString(brave_sync_prefs_->GetThisDeviceId());
    VLOG(1) << "[Brave Sync] Init device id from prefs: " <<
        StrFromUint8Array(device_id);
  } else {
    VLOG(1) << "[Brave Sync] Init empty device id";
  }

  DCHECK(!sync_version.empty());
  // TODO(bridiver) - this seems broken because using the version we get back
  // from the server (currently v1.4.2) causes things to break. What is the
  // the point of having this value?
  brave_sync_prefs_->SetApiVersion("0");

  brave_sync::client_data::Config config;
  config.api_version = brave_sync_prefs_->GetApiVersion();
  config.server_url = "https://sync.brave.com";
  config.debug = true;
  GetBraveSyncClient()->SendGotInitData(seed, device_id, config, brave_sync_words_);
}

void ProfileSyncService::OnSaveInitData(const Uint8Array& seed,
                                        const Uint8Array& device_id) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  DCHECK(!brave_sync_initialized_);
  // If we are here and brave_sync_initializing_ is false, we have came
  // not from OnSetupSyncNewToSync or OnSetupSyncHaveCode.
  // One case is we put wrong code words and then restarted before cleared
  // kSyncEnabled pref. This should not happen.
  DCHECK(brave_sync_initializing_);

  std::string seed_str = StrFromUint8Array(seed);
  std::string device_id_str = StrFromUint8Array(device_id);

  std::string prev_seed_str = brave_sync_prefs_->GetPrevSeed();

  brave_sync_words_.clear();
  DCHECK(!seed_str.empty());

  if (prev_seed_str == seed_str) {  // reconnecting to previous sync chain
    brave_sync_prefs_->SetPrevSeed(std::string());
  } else if (!prev_seed_str.empty()) {  // connect/create to new sync chain
    // bookmark_change_processor_->Reset(true);
    brave_sync_prefs_->SetPrevSeed(std::string());
  } else {
    // This is not required, because when there is no previous seed, bookmarks
    // should not have a metadata. However, this is done by intention, to be
    // a remedy for cases when sync had been reset and prev_seed_str had been
    // cleared when it shouldn't (brave-browser#3188).
    // bookmark_change_processor_->Reset(true);
  }

  brave_sync_prefs_->SetSeed(seed_str);
  brave_sync_prefs_->SetThisDeviceId(device_id_str);

  brave_sync_configured_ = true;

  user_settings_->SetChosenDataTypes(false, syncer::ModelTypeSet());
  OnSetSyncBookmarks(true);

  brave_sync_initializing_ = false;
}

void ProfileSyncService::OnSyncReady() {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  const std::string bookmarks_base_order = brave_sync_prefs_->GetBookmarksBaseOrder();
  if (bookmarks_base_order.empty()) {
    std::string platform = brave_sync::tools::GetPlatformName();
    GetBraveSyncClient()->SendGetBookmarksBaseOrder(brave_sync_prefs_->GetThisDeviceId(),
                                            platform);
    // OnSyncReady will be called by OnSaveBookmarksBaseOrder
    return;
  }

  DCHECK(false == brave_sync_initialized_);
  brave_sync_initialized_ = true;

  user_settings_->SetSyncRequested(true);
  // fetch the records
  // RequestSyncData();
}

void ProfileSyncService::OnGetExistingObjects(
    const std::string& category_name,
    std::unique_ptr<RecordsList> records,
    const base::Time &last_record_time_stamp,
    const bool is_truncated) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
#if 0
  // TODO(bridiver) - what do we do with is_truncated ?
  // It appears to be ignored in b-l
  if (!tools::IsTimeEmpty(last_record_time_stamp)) {
    brave_sync_prefs_->SetLatestRecordTime(last_record_time_stamp);
  }

  if (category_name == jslib_const::kBookmarks) {
    auto records_and_existing_objects =
        std::make_unique<SyncRecordAndExistingList>();
    bookmark_change_processor_->GetAllSyncData(
        *records.get(), records_and_existing_objects.get());
    sync_client_->SendResolveSyncRecords(
        category_name, std::move(records_and_existing_objects));
  } else if (category_name == brave_sync::jslib_const::kPreferences) {
    auto existing_records = PrepareResolvedPreferences(*records.get());
    sync_client_->SendResolveSyncRecords(
        category_name, std::move(existing_records));
  }
#endif
}

void ProfileSyncService::OnResolvedSyncRecords(
    const std::string& category_name,
    std::unique_ptr<RecordsList> records) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  if (category_name == brave_sync::jslib_const::kPreferences) {
    OnResolvedPreferences(*records.get());
  }
#if 0
  } else if (category_name == brave_sync::jslib_const::kBookmarks) {
    bookmark_change_processor_->ApplyChangesFromSyncModel(*records.get());
    bookmark_change_processor_->SendUnsynced(unsynced_send_interval_);
  } else if (category_name == brave_sync::jslib_const::kHistorySites) {
    NOTIMPLEMENTED();
  }
#endif
}

void ProfileSyncService::OnDeletedSyncUser() {
  NOTIMPLEMENTED();
}

void ProfileSyncService::OnDeleteSyncSiteSettings()  {
  NOTIMPLEMENTED();
}

void ProfileSyncService::OnSaveBookmarksBaseOrder(const std::string& order)  {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  DCHECK(!order.empty());
  brave_sync_prefs_->SetBookmarksBaseOrder(order);
  OnSyncReady();
}

void ProfileSyncService::OnSyncWordsPrepared(const std::string& words) {
  NotifyHaveSyncWords(words);
}

void ProfileSyncService::NotifySyncSetupError(const std::string& error) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  for (auto& observer : brave_sync::BraveSyncService::observers_)
    observer.OnSyncSetupError(this, error);
}

void ProfileSyncService::NotifySyncStateChanged() {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  for (auto& observer : brave_sync::BraveSyncService::observers_)
    observer.OnSyncStateChanged(this);
}

void ProfileSyncService::NotifyHaveSyncWords(
    const std::string& sync_words) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  for (auto& observer : brave_sync::BraveSyncService::observers_)
    observer.OnHaveSyncWords(this, sync_words);
}

void ProfileSyncService::ResetSyncInternal() {
  brave_sync_prefs_->SetPrevSeed(brave_sync_prefs_->GetSeed());

  brave_sync_prefs_->Clear();

  brave_sync_configured_ = false;
  brave_sync_initialized_ = false;

  brave_sync_prefs_->SetSyncEnabled(false);
}

void ProfileSyncService::SendCreateDevice() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  std::string device_name = brave_sync_prefs_->GetThisDeviceName();
  std::string object_id = brave_sync::tools::GenerateObjectId();
  std::string device_id = brave_sync_prefs_->GetThisDeviceId();
  CHECK(!device_id.empty());

  SendDeviceSyncRecord(
      SyncRecord::Action::A_CREATE,
      device_name,
      device_id,
      object_id);
}

void ProfileSyncService::SendDeviceSyncRecord(
    const int action,
    const std::string& device_name,
    const std::string& device_id,
    const std::string& object_id) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  RecordsListPtr records = CreateDeviceCreationRecordExtension(
      device_name,
      object_id,
      static_cast<SyncRecord::Action>(action),
      device_id);
  GetBraveSyncClient()->SendSyncRecords(
      SyncRecordType_PREFERENCES, *records);
}

brave_sync::BraveSyncClient* ProfileSyncService::GetBraveSyncClient() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  return sync_client_->GetBraveSyncClient();
}

void ProfileSyncService::OnResolvedPreferences(const RecordsList& records) {
  const std::string this_device_id = brave_sync_prefs_->GetThisDeviceId();
  bool this_device_deleted = false;
  bool contains_only_one_device = false;

  auto sync_devices = brave_sync_prefs_->GetSyncDevices();
  for (const auto &record : records) {
    DCHECK(record->has_device() || record->has_sitesetting());
    if (record->has_device()) {
      bool actually_merged = false;
      sync_devices->Merge(
          SyncDevice(record->GetDevice().name,
          record->objectId,
          record->deviceId,
          record->syncTimestamp.ToJsTime()),
          record->action,
          &actually_merged);
      this_device_deleted = this_device_deleted ||
        (record->deviceId == this_device_id &&
          record->action == SyncRecord::Action::A_DELETE &&
          actually_merged);
      contains_only_one_device = sync_devices->size() < 2 &&
        record->action == SyncRecord::Action::A_DELETE &&
          actually_merged;
    }
  }  // for each device

  brave_sync_prefs_->SetSyncDevices(*sync_devices);

  if (this_device_deleted) {
    ResetSyncInternal();
  } else if (contains_only_one_device) {
    // We see amount of devices had been decreased to 1 and it is not this
    // device had been deleted. So call OnResetSync which will send DELETE
    // record for this device
    OnResetSync();
  }
}

void ProfileSyncService::OnBraveSyncPrefsChanged(const std::string& pref) {
  if (pref == brave_sync::prefs::kSyncEnabled) {
    GetBraveSyncClient()->OnSyncEnabledChanged();
    if (!brave_sync_prefs_->GetSyncEnabled())
      brave_sync_initialized_ = false;
  }
  NotifySyncStateChanged();
}

bool ProfileSyncService::IsBraveSyncEnabled() const{
  return brave_sync_prefs_->GetSyncEnabled();
}

void ProfileSyncService::BraveSyncSetup() {
  brave_sync_words_ = std::string();
  brave_sync_prefs_ =
    std::make_unique<brave_sync::prefs::Prefs>(sync_client_->GetPrefService());
  sync_client_->GetBraveSyncClient()->set_sync_message_handler(this);

  // Moniter syncs prefs required in GetSettingsAndDevices
  brave_pref_change_registrar_.Init(sync_client_->GetPrefService());
  brave_pref_change_registrar_.Add(
      brave_sync::prefs::kSyncEnabled,
      base::Bind(&ProfileSyncService::OnBraveSyncPrefsChanged,
                 base::Unretained(this)));
  brave_pref_change_registrar_.Add(
      brave_sync::prefs::kSyncDeviceName,
      base::Bind(&ProfileSyncService::OnBraveSyncPrefsChanged,
                 base::Unretained(this)));
  brave_pref_change_registrar_.Add(
      brave_sync::prefs::kSyncDeviceList,
      base::Bind(&ProfileSyncService::OnBraveSyncPrefsChanged,
                 base::Unretained(this)));
  brave_pref_change_registrar_.Add(
      brave_sync::prefs::kSyncBookmarksEnabled,
      base::Bind(&ProfileSyncService::OnBraveSyncPrefsChanged,
                 base::Unretained(this)));
  brave_pref_change_registrar_.Add(
      brave_sync::prefs::kSyncSiteSettingsEnabled,
      base::Bind(&ProfileSyncService::OnBraveSyncPrefsChanged,
                 base::Unretained(this)));
  brave_pref_change_registrar_.Add(
      brave_sync::prefs::kSyncHistoryEnabled,
      base::Bind(&ProfileSyncService::OnBraveSyncPrefsChanged,
                 base::Unretained(this)));

  if (!brave_sync_prefs_->GetSeed().empty() &&
      !brave_sync_prefs_->GetThisDeviceName().empty()) {
    brave_sync_configured_ = true;
  }
}

void ProfileSyncService::BraveEngineParamsInit(
    syncer::SyncEngine::InitParams* params) {
  DCHECK(params);
  params->nudge_sync_cycle_delegate_function =
      base::BindRepeating(&ProfileSyncService::OnNudgeSyncCycle,
                          sync_enabled_weak_factory_.GetWeakPtr());
  params->poll_sync_cycle_delegate_function =
      base::BindRepeating(&ProfileSyncService::OnPollSyncCycle,
                          sync_enabled_weak_factory_.GetWeakPtr());
}

void ProfileSyncService::OnNudgeSyncCycle(){
  LOG(ERROR) << __func__;
}

void ProfileSyncService::OnPollSyncCycle(){
  LOG(ERROR) << __func__;

  if (IsTimeEmpty(brave_sync_prefs_->GetLastFetchTime()))
    SendCreateDevice();
  GetBraveSyncClient()->SendFetchSyncDevices();
  // FetchSyncRecords(bookmarks, history, preferences, 1000);
  brave_sync_prefs_->SetLastFetchTime(base::Time::Now());
}

}   // namespace browser_sync
