/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <linux/types.h>
#include <linux/netlink.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <android/log.h>
#include <cutils/properties.h>

#include "AutoMounter.h"
#include "nsVolumeService.h"
#include "AutoMounterSetting.h"
#include "base/message_loop.h"
#include "mozilla/AutoRestore.h"
#include "mozilla/FileUtils.h"
#include "mozilla/Hal.h"
#include "mozilla/StaticPtr.h"
#include "MozMtpServer.h"
#include "MozMtpStorage.h"
#include "nsAutoPtr.h"
#include "nsCharSeparatedTokenizer.h"
#include "nsMemory.h"
#include "nsString.h"
#include "nsThreadUtils.h"
#include "nsXULAppAPI.h"
#include "OpenFileFinder.h"
#include "Volume.h"
#include "VolumeManager.h"

using namespace mozilla::hal;
USING_MTP_NAMESPACE

/**************************************************************************
*
* The following "switch" files are available for monitoring usb
* connections:
*
*   /sys/devices/virtual/switch/usb_connected/state
*   /sys/devices/virtual/switch/usb_configuration/state
*
*   Under gingerbread, only the usb_configuration seems to be available.
*   Starting with honeycomb, usb_connected was also added.
*
*   When a cable insertion/removal occurs, then a uevent similar to the
*   following will be generted:
*
*    change@/devices/virtual/switch/usb_configuration
*      ACTION=change
*      DEVPATH=/devices/virtual/switch/usb_configuration
*      SUBSYSTEM=switch
*      SWITCH_NAME=usb_configuration
*      SWITCH_STATE=0
*      SEQNUM=5038
*
*    SWITCH_STATE will be 0 after a removal and 1 after an insertion
*
**************************************************************************/

#define USB_CONFIGURATION_SWITCH_NAME   NS_LITERAL_STRING("usb_configuration")

#define GB_SYS_UMS_ENABLE     "/sys/devices/virtual/usb_composite/usb_mass_storage/enable"
#define GB_SYS_USB_CONFIGURED "/sys/devices/virtual/switch/usb_configuration/state"

#define ICS_SYS_USB_FUNCTIONS "/sys/devices/virtual/android_usb/android0/functions"
#define ICS_SYS_UMS_DIRECTORY "/sys/devices/virtual/android_usb/android0/f_mass_storage"
#define ICS_SYS_MTP_DIRECTORY "/sys/devices/virtual/android_usb/android0/f_mtp"
#define ICS_SYS_USB_STATE     "/sys/devices/virtual/android_usb/android0/state"

#define USE_DEBUG 0

#undef LOG
#define LOG(args...)  __android_log_print(ANDROID_LOG_INFO,  "AutoMounter", ## args)
#define LOGW(args...) __android_log_print(ANDROID_LOG_WARN,  "AutoMounter", ## args)
#define ERR(args...)  __android_log_print(ANDROID_LOG_ERROR, "AutoMounter", ## args)

#if USE_DEBUG
#define DBG(args...)  __android_log_print(ANDROID_LOG_DEBUG, "AutoMounter" , ## args)
#else
#define DBG(args...)
#endif

namespace mozilla {
namespace system {

#define SYS_USB_CONFIG          "sys.usb.config"
#define PERSIST_SYS_USB_CONFIG  "persist.sys.usb.config"

#define USB_FUNC_ADB  "adb"
#define USB_FUNC_MTP  "mtp"
#define USB_FUNC_UMS  "mass_storage"

class AutoMounter;

static void SetAutoMounterStatus(int32_t aStatus);

/***************************************************************************/

inline const char* SwitchStateStr(const SwitchEvent& aEvent)
{
  return aEvent.status() == SWITCH_STATE_ON ? "plugged" : "unplugged";
}

/***************************************************************************/

static bool
IsUsbCablePluggedIn()
{
#if 0
  // Use this code when bug 745078 gets fixed (or use whatever the
  // appropriate method is)
  return GetCurrentSwitchEvent(SWITCH_USB) == SWITCH_STATE_ON;
#else
  // Until then, just go read the file directly
  if (access(ICS_SYS_USB_STATE, F_OK) == 0) {
    char usbState[20];
    if (ReadSysFile(ICS_SYS_USB_STATE, usbState, sizeof(usbState))) {
      DBG("IsUsbCablePluggedIn: state = '%s'", usbState);
      return strcmp(usbState, "CONFIGURED") == 0 ||
             strcmp(usbState, "CONNECTED") == 0;
    }
    ERR("Error reading file '%s': %s", ICS_SYS_USB_STATE, strerror(errno));
    return false;
  }
  bool configured;
  if (ReadSysFile(GB_SYS_USB_CONFIGURED, &configured)) {
    return configured;
  }
  ERR("Error reading file '%s': %s", GB_SYS_USB_CONFIGURED, strerror(errno));
  return false;
#endif
}

static bool
IsUsbConfigured()
{
  if (access(ICS_SYS_USB_STATE, F_OK) == 0) {
    char usbState[20];
    if (ReadSysFile(ICS_SYS_USB_STATE, usbState, sizeof(usbState))) {
      DBG("IsUsbConfigured: state = '%s'", usbState);
      return strcmp(usbState, "CONFIGURED") == 0;
    }
    ERR("Error reading file '%s': %s", ICS_SYS_USB_STATE, strerror(errno));
    return false;
  }
  bool configured;
  if (ReadSysFile(GB_SYS_USB_CONFIGURED, &configured)) {
    return configured;
  }
  ERR("Error reading file '%s': %s", GB_SYS_USB_CONFIGURED, strerror(errno));
  return false;
}

/***************************************************************************/

// The AutoVolumeManagerStateObserver allows the AutoMounter to know when
// the volume manager changes state (i.e. it has finished initialization)
class AutoVolumeManagerStateObserver : public VolumeManager::StateObserver
{
public:
  virtual void Notify(const VolumeManager::StateChangedEvent& aEvent);
};

// The AutoVolumeEventObserver allows the AutoMounter to know about card
// insertion and removal, as well as state changes in the volume.
class AutoVolumeEventObserver : public Volume::EventObserver
{
public:
  virtual void Notify(Volume* const& aEvent);
};

class AutoMounterResponseCallback : public VolumeResponseCallback
{
public:
  AutoMounterResponseCallback()
    : mErrorCount(0)
  {
  }

protected:
  virtual void ResponseReceived(const VolumeCommand* aCommand);

private:
    const static int kMaxErrorCount = 3; // Max number of errors before we give up

    int   mErrorCount;
};

/***************************************************************************/

class AutoMounter
{
public:
  NS_INLINE_DECL_REFCOUNTING(AutoMounter)

  typedef nsTArray<RefPtr<Volume>> VolumeArray;

  AutoMounter()
    : mState(STATE_IDLE),
      mResponseCallback(new AutoMounterResponseCallback),
      mMode(AUTOMOUNTER_DISABLE)
  {
    VolumeManager::RegisterStateObserver(&mVolumeManagerStateObserver);
    Volume::RegisterObserver(&mVolumeEventObserver);

    // It's possible that the VolumeManager is already in the READY state,
    // so we call CheckVolumeSettings here to cover that case. Otherwise,
    // we'll pick it up when the VolumeManage state changes to VOLUMES_READY.
    CheckVolumeSettings();

    DBG("Calling UpdateState from constructor");
    UpdateState();
  }

  ~AutoMounter()
  {
    VolumeManager::VolumeArray::size_type numVolumes = VolumeManager::NumVolumes();
    VolumeManager::VolumeArray::index_type volIndex;
    for (volIndex = 0; volIndex < numVolumes; volIndex++) {
      RefPtr<Volume> vol = VolumeManager::GetVolume(volIndex);
      if (vol) {
        vol->UnregisterObserver(&mVolumeEventObserver);
      }
    }
    Volume::UnregisterObserver(&mVolumeEventObserver);
    VolumeManager::UnregisterStateObserver(&mVolumeManagerStateObserver);
  }

  void CheckVolumeSettings()
  {
    if (VolumeManager::State() != VolumeManager::VOLUMES_READY) {
      DBG("CheckVolumeSettings: VolumeManager is NOT READY yet");
      return;
    }
    DBG("CheckVolumeSettings: VolumeManager is READY");

    // The VolumeManager knows about all of the volumes from vold. We now
    // know the names of all of the volumes, so we can find out what the
    // initial sharing settings are set to.

    VolumeManager::VolumeArray::size_type numVolumes = VolumeManager::NumVolumes();
    VolumeManager::VolumeArray::index_type i;
    for (i = 0; i < numVolumes; i++) {
      RefPtr<Volume> vol = VolumeManager::GetVolume(i);
      if (vol) {
        vol->RegisterObserver(&mVolumeEventObserver);
        // We need to pick up the intial value of the
        // ums.volume.NAME.enabled setting.
        AutoMounterSetting::CheckVolumeSettings(vol->Name());

        // Note: eventually CheckVolumeSettings will call
        //       AutoMounter::SetSharingMode, which will in turn call
        //       UpdateState if needed.
      }
    }
  }

  void UpdateState();

  void ConfigureUsbFunction(const char* aUsbFunc);

  void StartMtpServer();
  void StopMtpServer();

  void StartUmsSharing();
  void StopUmsSharing();


  const char* ModeStr(int32_t aMode)
  {
    switch (aMode) {
      case AUTOMOUNTER_DISABLE:                 return "Disable";
      case AUTOMOUNTER_ENABLE_UMS:              return "Enable-UMS";
      case AUTOMOUNTER_DISABLE_WHEN_UNPLUGGED:  return "DisableWhenUnplugged";
      case AUTOMOUNTER_ENABLE_MTP:              return "Enable-MTP";
    }
    return "??? Unknown ???";
  }

  bool IsModeEnabled(int32_t aMode)
  {
    return aMode == AUTOMOUNTER_ENABLE_MTP ||
           aMode == AUTOMOUNTER_ENABLE_UMS;
  }

  void SetMode(int32_t aMode)
  {
    if ((aMode == AUTOMOUNTER_DISABLE_WHEN_UNPLUGGED) &&
        (mMode == AUTOMOUNTER_DISABLE)) {
      // If it's already disabled, then leave it as disabled.
      // AUTOMOUNTER_DISABLE_WHEN_UNPLUGGED implies "enabled until unplugged"
      aMode = AUTOMOUNTER_DISABLE;
    }

    if (aMode == AUTOMOUNTER_DISABLE &&
        mMode == AUTOMOUNTER_ENABLE_UMS && IsUsbCablePluggedIn()) {
      // On many devices (esp non-Samsung), we can't force the disable, so we
      // need to defer until the USB cable is actually unplugged.
      // See bug 777043.
      //
      // Otherwise our attempt to disable it will fail, and we'll wind up in a bad
      // state where the AutoMounter thinks that Sharing has been turned off, but
      // the files are actually still being Shared because the attempt to unshare
      // failed.
      LOG("Attempting to disable UMS. Deferring until USB cable is unplugged.");
      aMode = AUTOMOUNTER_DISABLE_WHEN_UNPLUGGED;
    }

    if (aMode != mMode) {
      LOG("Changing mode from '%s' to '%s'", ModeStr(mMode), ModeStr(aMode));
      mMode = aMode;
      DBG("Calling UpdateState due to mode set to %d", mMode);
      UpdateState();
    }
  }

  void SetSharingMode(const nsACString& aVolumeName, bool aAllowSharing)
  {
    RefPtr<Volume> vol = VolumeManager::FindVolumeByName(aVolumeName);
    if (!vol) {
      return;
    }
    if (vol->IsSharingEnabled() == aAllowSharing) {
      return;
    }
    vol->SetUnmountRequested(false);
    vol->SetMountRequested(false);
    vol->SetSharingEnabled(aAllowSharing);
    DBG("Calling UpdateState due to volume %s sharing set to %d",
        vol->NameStr(), (int)aAllowSharing);
    UpdateState();
  }

  void FormatVolume(const nsACString& aVolumeName)
  {
    RefPtr<Volume> vol = VolumeManager::FindVolumeByName(aVolumeName);
    if (!vol) {
      return;
    }
    if (vol->IsFormatRequested()) {
      return;
    }
    vol->SetUnmountRequested(false);
    vol->SetMountRequested(false);
    vol->SetFormatRequested(true);
    DBG("Calling UpdateState due to volume %s formatting set to %d",
        vol->NameStr(), (int)vol->IsFormatRequested());
    UpdateState();
  }

  void MountVolume(const nsACString& aVolumeName)
  {
    RefPtr<Volume> vol = VolumeManager::FindVolumeByName(aVolumeName);
    if (!vol) {
      return;
    }
    vol->SetUnmountRequested(false);
    if (vol->IsMountRequested() || vol->mState == nsIVolume::STATE_MOUNTED) {
      return;
    }
    vol->SetMountRequested(true);
    DBG("Calling UpdateState due to volume %s mounting set to %d",
        vol->NameStr(), (int)vol->IsMountRequested());
    UpdateState();
  }

  void UnmountVolume(const nsACString& aVolumeName)
  {
    RefPtr<Volume> vol = VolumeManager::FindVolumeByName(aVolumeName);
    if (!vol) {
      return;
    }
    if (vol->IsUnmountRequested()) {
      return;
    }
    vol->SetMountRequested(false);
    vol->SetUnmountRequested(true);
    DBG("Calling UpdateState due to volume %s unmounting set to %d",
        vol->NameStr(), (int)vol->IsUnmountRequested());
    UpdateState();
  }

private:

  enum STATE
  {
    // IDLE - Nothing is being shared
    STATE_IDLE,

    // We've detected that conditions are right to enable mtp. So we've
    // set sys.usb.config to include mtp, and we're waiting for the USB
    // subsystem to be "configured". Once mtp shows up in
    //  then we know
    // that its been configured and we can open /dev/mtp_usb
    STATE_MTP_CONFIGURING,

    // mtp has been configured (i.e. mtp now shows up in
    // /sys/devices/virtual/android_usb/android0/functions so we can start
    // the mtp server.
    STATE_MTP_STARTED,

    // The mtp server has reported sessionStarted. We'll leave this state
    // when we receive sessionEnded.
    STATE_MTP_CONNECTED,

    // We've added mass_storage (aka UMS) to sys.usb.config and we're waiting for
    // mass_storage to appear in /sys/devices/virtual/android_usb/android0/functions
    STATE_UMS_CONFIGURING,

    // mass_storage has been configured and we can start sharing once the user
    // enables it.
    STATE_UMS_CONFIGURED,
  };

  const char *StateStr(STATE aState)
  {
    switch (aState) {
      case STATE_IDLE:            return "IDLE";
      case STATE_MTP_CONFIGURING: return "MTP_CONFIGURING";
      case STATE_MTP_CONNECTED:   return "MTP_CONNECTED";
      case STATE_MTP_STARTED:     return "MTP_STARTED";
      case STATE_UMS_CONFIGURING: return "UMS_CONFIGURING";
      case STATE_UMS_CONFIGURED:  return "UMS_CONFIGURED";
    }
    return "STATE_???";
  }

  void SetState(STATE aState)
  {
    const char *oldStateStr = StateStr(mState);
    mState = aState;
    const char *newStateStr = StateStr(mState);
    LOG("AutoMounter state changed from %s to %s", oldStateStr, newStateStr);
  }

  STATE                           mState;

  AutoVolumeEventObserver         mVolumeEventObserver;
  AutoVolumeManagerStateObserver  mVolumeManagerStateObserver;
  RefPtr<VolumeResponseCallback>  mResponseCallback;
  int32_t                         mMode;
  MozMtpStorage::Array            mMozMtpStorage;
};

static StaticRefPtr<AutoMounter> sAutoMounter;
static StaticRefPtr<MozMtpServer> sMozMtpServer;

/***************************************************************************/

void
AutoVolumeManagerStateObserver::Notify(const VolumeManager::StateChangedEvent &)
{
  LOG("VolumeManager state changed event: %s", VolumeManager::StateStr());

  if (!sAutoMounter) {
    return;
  }

  // In the event that the VolumeManager just entered the VOLUMES_READY state,
  // we call CheckVolumeSettings here (it's possible that this method never
  // gets called if the VolumeManager was already in the VOLUMES_READY state
  // by the time the AutoMounter was constructed).
  sAutoMounter->CheckVolumeSettings();

  DBG("Calling UpdateState due to VolumeManagerStateObserver");
  sAutoMounter->UpdateState();
}

void
AutoVolumeEventObserver::Notify(Volume * const &)
{
  if (!sAutoMounter) {
    return;
  }
  DBG("Calling UpdateState due to VolumeEventStateObserver");
  sAutoMounter->UpdateState();
}

void
AutoMounterResponseCallback::ResponseReceived(const VolumeCommand* aCommand)
{

  if (WasSuccessful()) {
    DBG("Calling UpdateState due to Volume::OnSuccess");
    mErrorCount = 0;
    sAutoMounter->UpdateState();
    return;
  }
  ERR("Command '%s' failed: %d '%s'",
      aCommand->CmdStr(), ResponseCode(), ResponseStr().get());

  if (++mErrorCount < kMaxErrorCount) {
    DBG("Calling UpdateState due to VolumeResponseCallback::OnError");
    sAutoMounter->UpdateState();
  }
}

static bool
IsUsbFunctionEnabled(const char* aConfig, const char* aUsbFunc)
{
  nsAutoCString config(aConfig);
  nsCCharSeparatedTokenizer tokenizer(config, ',');

  while (tokenizer.hasMoreTokens()) {
    nsAutoCString token(tokenizer.nextToken());
    if (token.Equals(aUsbFunc)) {
      DBG("IsUsbFunctionEnabled('%s', '%s'): returning true", aConfig, aUsbFunc);
      return true;
    }
  }
  DBG("IsUsbFunctionEnabled('%s', '%s'): returning false", aConfig, aUsbFunc);
  return false;
}

static void
SetUsbFunction(const char* aUsbFunc)
{
  char oldSysUsbConfig[PROPERTY_VALUE_MAX];
  property_get(SYS_USB_CONFIG, oldSysUsbConfig, "");

  if (IsUsbFunctionEnabled(oldSysUsbConfig, aUsbFunc)) {
    // The function is already configured. Nothing else to do.
    DBG("SetUsbFunction('%s') - already set - nothing to do", aUsbFunc);
    return;
  }

  char newSysUsbConfig[PROPERTY_VALUE_MAX];

  if (strcmp(aUsbFunc, USB_FUNC_MTP) == 0) {
    // We're enabling MTP. For this we'll wind up using mtp, or mtp,adb
    strlcpy(newSysUsbConfig, USB_FUNC_MTP, sizeof(newSysUsbConfig));
  } else if (strcmp(aUsbFunc, USB_FUNC_UMS) == 0) {
    // We're enabling UMS. For this we make the assumption that the persisted
    // property has mass_storage enabled.
    property_get(PERSIST_SYS_USB_CONFIG, newSysUsbConfig, "");
  } else {
    printf_stderr("AutoMounter::SetUsbFunction Unrecognized aUsbFunc '%s'\n", aUsbFunc);
    MOZ_ASSERT(0);
    return;
  }

  // Make sure the new value that we write into sys.usb.config keeps the adb
  // (or non-adb) of the current string.

  if (IsUsbFunctionEnabled(oldSysUsbConfig, USB_FUNC_ADB)) {
    // ADB was turned on - keep it on.
    if (!IsUsbFunctionEnabled(newSysUsbConfig, USB_FUNC_ADB)) {
      // Add adb to the new string
      strlcat(newSysUsbConfig, ",", sizeof(newSysUsbConfig));
      strlcat(newSysUsbConfig, USB_FUNC_ADB, sizeof(newSysUsbConfig));
    }
  } else {
    // ADB was turned off - keep it off
    if (IsUsbFunctionEnabled(newSysUsbConfig, USB_FUNC_ADB)) {
      // Remove ADB from the new string.
      if (strcmp(newSysUsbConfig, USB_FUNC_ADB) == 0) {
        newSysUsbConfig[0] = '\0';
      } else {
        nsAutoCString withoutAdb(newSysUsbConfig);
        withoutAdb.ReplaceSubstring( "," USB_FUNC_ADB, "");
        strlcpy(newSysUsbConfig, withoutAdb.get(), sizeof(newSysUsbConfig));
      }
    }
  }

  LOG("SetUsbFunction(%s) %s to '%s'", aUsbFunc, SYS_USB_CONFIG, newSysUsbConfig);
  property_set(SYS_USB_CONFIG, newSysUsbConfig);
}

void
AutoMounter::StartMtpServer()
{
  if (sMozMtpServer) {
    // Mtp Server is already running - nothing to do
    return;
  }
  LOG("Starting MtpServer");
  sMozMtpServer = new MozMtpServer();
  sMozMtpServer->Run();

  VolumeArray::index_type volIndex;
  VolumeArray::size_type  numVolumes = VolumeManager::NumVolumes();
  for (volIndex = 0; volIndex < numVolumes; volIndex++) {
    RefPtr<Volume> vol = VolumeManager::GetVolume(volIndex);
    nsRefPtr<MozMtpStorage> storage = new MozMtpStorage(vol, sMozMtpServer);
    mMozMtpStorage.AppendElement(storage);
  }
}

void
AutoMounter::StopMtpServer()
{
  LOG("Stopping MtpServer");

  mMozMtpStorage.Clear();
  sMozMtpServer = nullptr;
}

/***************************************************************************/

void
AutoMounter::UpdateState()
{
  static bool inUpdateState = false;
  if (inUpdateState) {
    // When UpdateState calls SetISharing, this causes a volume state
    // change to occur, which would normally cause UpdateState to be called
    // again. We want the volume state change to go out (so that device
    // storage will see the change in sharing state), but since we're
    // already in UpdateState we just want to prevent recursion from screwing
    // things up.
    return;
  }
  AutoRestore<bool> inUpdateStateDetector(inUpdateState);
  inUpdateState = true;

  MOZ_ASSERT(MessageLoop::current() == XRE_GetIOMessageLoop());

  // If the following preconditions are met:
  //    - UMS is available (i.e. compiled into the kernel)
  //    - UMS is enabled
  //    - AutoMounter is enabled
  //    - USB cable is plugged in
  //  then we will try to unmount and share
  //  otherwise we will try to unshare and mount.

  if (VolumeManager::State() != VolumeManager::VOLUMES_READY) {
    // The volume manager isn't in a ready state, so there
    // isn't anything else that we can do.
    LOG("UpdateState: VolumeManager not ready yet");
    return;
  }

  if (mResponseCallback->IsPending()) {
    // We only deal with one outstanding volume command at a time,
    // so we need to wait for it to finish.
    return;
  }

  // Calling setprop sys.usb.config mtp,adb (or adding mass_storage) will
  // cause /sys/devices/virtual/android_usb/android0/state to go:
  // CONFIGURED -> DISCONNECTED -> CONNECTED -> CONFIGURED
  //
  // Since IsUsbCablePluggedIn returns state == CONFIGURED, it will look
  // like a cable pull and replugin.

  bool umsAvail = false;
  bool umsConfigured = false;
  bool umsEnabled = false;
  bool mtpAvail = false;
  bool mtpConfigured = false;
  bool mtpEnabled = false;
  bool usbCablePluggedIn = IsUsbCablePluggedIn();

  if (access(ICS_SYS_USB_FUNCTIONS, F_OK) == 0) {
    char functionsStr[60];
    if (!ReadSysFile(ICS_SYS_USB_FUNCTIONS, functionsStr, sizeof(functionsStr))) {
      ERR("Error reading file '%s': %s", ICS_SYS_USB_FUNCTIONS, strerror(errno));
      functionsStr[0] = '\0';
    }
    DBG("UpdateState: USB functions: '%s'", functionsStr);

    bool  usbConfigured = IsUsbConfigured();
    umsAvail = (access(ICS_SYS_UMS_DIRECTORY, F_OK) == 0);
    if (umsAvail) {
      umsConfigured = usbConfigured && strstr(functionsStr, USB_FUNC_UMS) != nullptr;
      umsEnabled = (mMode == AUTOMOUNTER_ENABLE_UMS) ||
                   ((mMode == AUTOMOUNTER_DISABLE_WHEN_UNPLUGGED) && umsConfigured);
    } else {
      umsConfigured = false;
      umsEnabled = false;
    }

    mtpAvail = (access(ICS_SYS_MTP_DIRECTORY, F_OK) == 0);
    if (mtpAvail) {
      mtpConfigured = usbConfigured && strstr(functionsStr, USB_FUNC_MTP) != nullptr;
      mtpEnabled = (mMode == AUTOMOUNTER_ENABLE_MTP) ||
                   ((mMode == AUTOMOUNTER_DISABLE_WHEN_UNPLUGGED) && mtpConfigured);
    } else {
      mtpConfigured = false;
      mtpEnabled = false;
    }
  }

  bool enabled = mtpEnabled || umsEnabled;

  if (mMode == AUTOMOUNTER_DISABLE_WHEN_UNPLUGGED) {
    // DISABLE_WHEN_UNPLUGGED implies already enabled.
    enabled = usbCablePluggedIn;
    if (!usbCablePluggedIn) {
      mMode = AUTOMOUNTER_DISABLE;
      mtpEnabled = false;
      umsEnabled = false;
    }
  }

  DBG("UpdateState: ums:A%dC%dE%d mtp:A%dC%dE%d mode:%d usb:%d mState:%s",
      umsAvail, umsConfigured, umsEnabled,
      mtpAvail, mtpConfigured, mtpEnabled,
      mMode, usbCablePluggedIn, StateStr(mState));

  switch (mState) {

    case STATE_IDLE:
      if (!usbCablePluggedIn) {
        // Stay in the IDLE state. We'll get a CONNECTED or CONFIGURED
        // UEvent when the usb cable is plugged in.
        break;
      }
      if (mtpEnabled) {
        if (mtpConfigured) {
          // The USB layer has already been configured. Now we can go ahead
          // and start the MTP server. This particular codepath will not
          // normally be taken, but it could happen if you stop and restart
          // b2g while sys.usb.config is set to enable mtp.
          StartMtpServer();
          SetState(STATE_MTP_STARTED);
        } else {
          // The MTP USB layer is configuring. Wait for it to finish
          // before we start the MTP server.
          SetUsbFunction(USB_FUNC_MTP);
          SetState(STATE_MTP_CONFIGURING);
        }
      } else if (umsConfigured) {
        // UMS is already configured.
        SetState(STATE_UMS_CONFIGURED);
      } else if (umsAvail) {
        // We do this whether or not UMS is enabled. With UMS, it's the
        // sharing of the volume which is significant. What is important
        // is that we don't leave it in MTP mode when MTP isn't enabled.
        SetUsbFunction(USB_FUNC_UMS);
        SetState(STATE_UMS_CONFIGURING);
      }
      break;

    case STATE_MTP_CONFIGURING:
      // While configuring, the USB configuration state will change from
      // CONFIGURED -> CONNECTED -> DISCONNECTED -> CONNECTED -> CONFIGURED
      // so we don't check for cable unplugged here.
      if (mtpEnabled && mtpConfigured) {
        // The USB layer has been configured. Now we can go ahead and start
        // the MTP server.
        StartMtpServer();
        SetState(STATE_MTP_STARTED);
      }
      break;

    case STATE_MTP_STARTED:
      if (usbCablePluggedIn) {
        if (mtpConfigured && mtpEnabled) {
          // Everything is still good. Leave the MTP server running
          break;
        }
        DBG("STATE_MTP_STARTED: About to StopMtpServer "
            "mtpConfigured = %d mtpEnabled = %d usbCablePluggedIn: %d",
            mtpConfigured, mtpEnabled, usbCablePluggedIn);
        StopMtpServer();
        if (umsAvail) {
          // Switch back to UMS
          SetUsbFunction(USB_FUNC_UMS);
          SetState(STATE_UMS_CONFIGURING);
          break;
        }
      }
      SetState(STATE_IDLE);
      break;

    case STATE_UMS_CONFIGURING:
      // While configuring, the USB configuration state will change from
      // CONFIGURED -> CONNECTED -> DISCONNECTED -> CONNECTED -> CONFIGURED
      // so we don't check for cable unplugged here.
      if (umsConfigured) {
        SetState(STATE_UMS_CONFIGURED);
      }
      break;

    case STATE_UMS_CONFIGURED:
      if (usbCablePluggedIn) {
        if (mtpEnabled) {
          // MTP was enabled. Start reconfiguring.
          SetState(STATE_MTP_CONFIGURING);
          SetUsbFunction(USB_FUNC_MTP);
          break;
        }
        if (umsConfigured && umsEnabled) {
          // This is the normal state when UMS is enabled.
          break;
        }
      }
      SetState(STATE_IDLE);
      break;

    default:
      SetState(STATE_IDLE);
      break;
  }

  bool tryToShare = umsEnabled && usbCablePluggedIn;
  LOG("UpdateState: ums:A%dC%dE%d mtp:A%dC%dE%d mode:%d usb:%d tryToShare:%d state:%s",
      umsAvail, umsConfigured, umsEnabled,
      mtpAvail, mtpConfigured, mtpEnabled,
      mMode, usbCablePluggedIn, tryToShare, StateStr(mState));

  bool filesOpen = false;
  static unsigned filesOpenDelayCount = 0;
  VolumeArray::index_type volIndex;
  VolumeArray::size_type  numVolumes = VolumeManager::NumVolumes();
  for (volIndex = 0; volIndex < numVolumes; volIndex++) {
    RefPtr<Volume>  vol = VolumeManager::GetVolume(volIndex);
    Volume::STATE   volState = vol->State();

    if (vol->State() == nsIVolume::STATE_MOUNTED) {
      LOG("UpdateState: Volume %s is %s and %s @ %s gen %d locked %d sharing %s",
          vol->NameStr(), vol->StateStr(),
          vol->MediaPresent() ? "inserted" : "missing",
          vol->MountPoint().get(), vol->MountGeneration(),
          (int)vol->IsMountLocked(),
          vol->CanBeShared() ? (vol->IsSharingEnabled() ?
                                 (vol->IsSharing() ? "en-y" : "en-n") : "dis") : "x");
      if (vol->IsSharing() && !usbCablePluggedIn) {
        // We call SetIsSharing(true) below to indicate intent to share. This
        // causes a state change which notifys apps, and they'll close any
        // open files, which will initiate the change away from the mounted
        // state and into the sharing state. Normally, when the volume
        // transitions back to the mounted state, then vol->mIsSharing gets set
        // to false. However, if the user pulls the USB cable before we
        // actually start sharing, then the volume never actually leaves
        // the mounted state (and hence never transitions from
        // sharing -> mounted), and mIsSharing never gets set back to false.
        // So we clear mIsSharing here.
        vol->SetIsSharing(false);
      }
    } else {
      LOG("UpdateState: Volume %s is %s and %s", vol->NameStr(), vol->StateStr(),
          vol->MediaPresent() ? "inserted" : "missing");
    }
    if (!vol->MediaPresent()) {
      // No media - nothing we can do
      continue;
    }

    if ((tryToShare && vol->IsSharingEnabled()) ||
        vol->IsFormatRequested() ||
        vol->IsUnmountRequested()) {
      switch (volState) {
        // We're going to try to unmount the volume
        case nsIVolume::STATE_MOUNTED: {
          if (vol->IsMountLocked()) {
            // The volume is currently locked, so leave it in the mounted
            // state.
            LOGW("UpdateState: Mounted volume %s is locked, not sharing or formatting",
                 vol->NameStr());
            break;
          }

          // Mark the volume as if we've started sharing. This will cause
          // apps which watch device storage notifications to see the volume
          // go into the shared state, and prompt them to close any open files
          // that they might have.
          if (tryToShare && vol->IsSharingEnabled()) {
            vol->SetIsSharing(true);
          } else if (vol->IsFormatRequested()){
            vol->SetIsFormatting(true);
          }

          // Check to see if there are any open files on the volume and
          // don't initiate the unmount while there are open files.
          OpenFileFinder::Info fileInfo;
          OpenFileFinder fileFinder(vol->MountPoint());
          if (fileFinder.First(&fileInfo)) {
            LOGW("The following files are open under '%s'",
                 vol->MountPoint().get());
            do {
              LOGW("  PID: %d file: '%s' app: '%s' comm: '%s' exe: '%s'\n",
                   fileInfo.mPid,
                   fileInfo.mFileName.get(),
                   fileInfo.mAppName.get(),
                   fileInfo.mComm.get(),
                   fileInfo.mExe.get());
            } while (fileFinder.Next(&fileInfo));
            LOGW("UpdateState: Mounted volume %s has open files, not sharing or formatting",
                 vol->NameStr());

            // Check again in a few seconds to see if the files are closed.
            // Since we're trying to share the volume, this implies that we're
            // plugged into the PC via USB and this in turn implies that the
            // battery is charging, so we don't need to be too concerned about
            // wasting battery here.
            //
            // If we just detected that there were files open, then we use
            // a short timer. We will have told the apps that we're trying
            // trying to share, and they'll be closing their files. This makes
            // the sharing more responsive. If after a few seconds, the apps
            // haven't closed their files, then we back off.

            int delay = 1000;
            if (filesOpenDelayCount > 10) {
              delay = 5000;
            }
            MessageLoopForIO::current()->
              PostDelayedTask(FROM_HERE,
                              NewRunnableMethod(this, &AutoMounter::UpdateState),
                              delay);
            filesOpen = true;
            break;
          }

          // Volume is mounted, we need to unmount before
          // we can share.
          LOG("UpdateState: Unmounting %s", vol->NameStr());
          vol->StartUnmount(mResponseCallback);
          return; // UpdateState will be called again when the Unmount command completes
        }
        case nsIVolume::STATE_IDLE: {
          LOG("UpdateState: Volume %s is nsIVolume::STATE_IDLE", vol->NameStr());
          if (vol->IsFormatting() && !vol->IsFormatRequested()) {
            vol->SetFormatRequested(false);
            LOG("UpdateState: Mounting %s", vol->NameStr());
            vol->StartMount(mResponseCallback);
            break;
          }
          if (tryToShare && vol->IsSharingEnabled()) {
            // Volume is unmounted. We can go ahead and share.
            LOG("UpdateState: Sharing %s", vol->NameStr());
            vol->StartShare(mResponseCallback);
          } else if (vol->IsFormatRequested()){
            // Volume is unmounted. We can go ahead and format.
            LOG("UpdateState: Formatting %s", vol->NameStr());
            vol->StartFormat(mResponseCallback);
          }
          return; // UpdateState will be called again when the Share/Format command completes
        }
        default: {
          // Not in a state that we can do anything about.
          break;
        }
      }
    } else {
      // We're going to try and unshare and remount the volumes
      switch (volState) {
        case nsIVolume::STATE_SHARED: {
          // Volume is shared. We can go ahead and unshare.
          LOG("UpdateState: Unsharing %s", vol->NameStr());
          vol->StartUnshare(mResponseCallback);
          return; // UpdateState will be called again when the Unshare command completes
        }
        case nsIVolume::STATE_IDLE: {
          if (!vol->IsUnmountRequested()) {
            // Volume is unmounted and mount-requested, try to mount.

            LOG("UpdateState: Mounting %s", vol->NameStr());
            vol->StartMount(mResponseCallback);
          }
          return; // UpdateState will be called again when Mount command completes
        }
        default: {
          // Not in a state that we can do anything about.
          break;
        }
      }
    }
  }

  int32_t status = AUTOMOUNTER_STATUS_DISABLED;
  if (filesOpen) {
    filesOpenDelayCount++;
    status = AUTOMOUNTER_STATUS_FILES_OPEN;
  } else if (enabled) {
    filesOpenDelayCount = 0;
    status = AUTOMOUNTER_STATUS_ENABLED;
  }
  SetAutoMounterStatus(status);
}

/***************************************************************************/

static void
InitAutoMounterIOThread()
{
  MOZ_ASSERT(MessageLoop::current() == XRE_GetIOMessageLoop());
  MOZ_ASSERT(!sAutoMounter);

  sAutoMounter = new AutoMounter();
}

static void
ShutdownAutoMounterIOThread()
{
  MOZ_ASSERT(MessageLoop::current() == XRE_GetIOMessageLoop());

  sAutoMounter = nullptr;
  ShutdownVolumeManager();
}

static void
SetAutoMounterModeIOThread(const int32_t& aMode)
{
  MOZ_ASSERT(XRE_GetProcessType() == GeckoProcessType_Default);
  MOZ_ASSERT(MessageLoop::current() == XRE_GetIOMessageLoop());
  MOZ_ASSERT(sAutoMounter);

  sAutoMounter->SetMode(aMode);
}

static void
SetAutoMounterSharingModeIOThread(const nsCString& aVolumeName, const bool& aAllowSharing)
{
  MOZ_ASSERT(XRE_GetProcessType() == GeckoProcessType_Default);
  MOZ_ASSERT(MessageLoop::current() == XRE_GetIOMessageLoop());
  MOZ_ASSERT(sAutoMounter);

  sAutoMounter->SetSharingMode(aVolumeName, aAllowSharing);
}

static void
AutoMounterFormatVolumeIOThread(const nsCString& aVolumeName)
{
  MOZ_ASSERT(XRE_GetProcessType() == GeckoProcessType_Default);
  MOZ_ASSERT(MessageLoop::current() == XRE_GetIOMessageLoop());
  MOZ_ASSERT(sAutoMounter);

  sAutoMounter->FormatVolume(aVolumeName);
}

static void
AutoMounterMountVolumeIOThread(const nsCString& aVolumeName)
{
  MOZ_ASSERT(XRE_GetProcessType() == GeckoProcessType_Default);
  MOZ_ASSERT(MessageLoop::current() == XRE_GetIOMessageLoop());
  MOZ_ASSERT(sAutoMounter);

  sAutoMounter->MountVolume(aVolumeName);
}

static void
AutoMounterUnmountVolumeIOThread(const nsCString& aVolumeName)
{
  MOZ_ASSERT(XRE_GetProcessType() == GeckoProcessType_Default);
  MOZ_ASSERT(MessageLoop::current() == XRE_GetIOMessageLoop());
  MOZ_ASSERT(sAutoMounter);

  sAutoMounter->UnmountVolume(aVolumeName);
}

static void
UsbCableEventIOThread()
{
  MOZ_ASSERT(MessageLoop::current() == XRE_GetIOMessageLoop());

  if (!sAutoMounter) {
    return;
  }
  DBG("Calling UpdateState due to USBCableEvent");
  sAutoMounter->UpdateState();
}

/**************************************************************************
*
*   Public API
*
*   Since the AutoMounter runs in IO Thread context, we need to switch
*   to IOThread context before we can do anything.
*
**************************************************************************/

class UsbCableObserver MOZ_FINAL : public SwitchObserver
{
  ~UsbCableObserver()
  {
    UnregisterSwitchObserver(SWITCH_USB, this);
  }

public:
  NS_INLINE_DECL_REFCOUNTING(UsbCableObserver)

  UsbCableObserver()
  {
    RegisterSwitchObserver(SWITCH_USB, this);
  }

  virtual void Notify(const SwitchEvent& aEvent)
  {
    DBG("UsbCable switch device: %d state: %s\n",
        aEvent.device(), SwitchStateStr(aEvent));
    XRE_GetIOMessageLoop()->PostTask(
        FROM_HERE,
        NewRunnableFunction(UsbCableEventIOThread));
  }
};

static StaticRefPtr<UsbCableObserver> sUsbCableObserver;
static StaticRefPtr<AutoMounterSetting> sAutoMounterSetting;

static void
InitVolumeConfig()
{
  // This function uses /system/etc/volume.cfg to add additional volumes
  // to the Volume Manager.
  //
  // This is useful on devices like the Nexus 4, which have no physical sd card
  // or dedicated partition.
  //
  // The format of the volume.cfg file is as follows:
  // create volume-name mount-point
  // Blank lines and lines starting with the hash character "#" will be ignored.

  nsCOMPtr<nsIVolumeService> vs = do_GetService(NS_VOLUMESERVICE_CONTRACTID);
  NS_ENSURE_TRUE_VOID(vs);

  ScopedCloseFile fp;
  int n = 0;
  char line[255];
  char *command, *vol_name_cstr, *mount_point_cstr, *save_ptr;
  const char *filename = "/system/etc/volume.cfg";
  if (!(fp = fopen(filename, "r"))) {
    LOG("Unable to open volume configuration file '%s' - ignoring", filename);
    return;
  }
  while(fgets(line, sizeof(line), fp)) {
    const char *delim = " \t\n";
    n++;

    if (line[0] == '#')
      continue;
    if (!(command = strtok_r(line, delim, &save_ptr))) {
      // Blank line - ignore
      continue;
    }
    if (!strcmp(command, "create")) {
      if (!(vol_name_cstr = strtok_r(nullptr, delim, &save_ptr))) {
        ERR("No vol_name in %s line %d",  filename, n);
        continue;
      }
      if (!(mount_point_cstr = strtok_r(nullptr, delim, &save_ptr))) {
        ERR("No mount point for volume '%s'. %s line %d", vol_name_cstr, filename, n);
        continue;
      }
      nsString  mount_point = NS_ConvertUTF8toUTF16(mount_point_cstr);
      nsString vol_name = NS_ConvertUTF8toUTF16(vol_name_cstr);
      nsresult rv;
      rv = vs->CreateFakeVolume(vol_name, mount_point);
      NS_ENSURE_SUCCESS_VOID(rv);
      rv = vs->SetFakeVolumeState(vol_name, nsIVolume::STATE_MOUNTED);
      NS_ENSURE_SUCCESS_VOID(rv);
    }
    else {
      ERR("Unrecognized command: '%s'", command);
    }
  }
}

void
InitAutoMounter()
{
  InitVolumeConfig();
  InitVolumeManager();
  sAutoMounterSetting = new AutoMounterSetting();

  XRE_GetIOMessageLoop()->PostTask(
      FROM_HERE,
      NewRunnableFunction(InitAutoMounterIOThread));

  // Switch Observers need to run on the main thread, so we need to
  // start it here and have it send events to the AutoMounter running
  // on the IO Thread.
  sUsbCableObserver = new UsbCableObserver();
}

int32_t
GetAutoMounterStatus()
{
  if (sAutoMounterSetting) {
    return sAutoMounterSetting->GetStatus();
  }
  return AUTOMOUNTER_STATUS_DISABLED;
}

//static
void
SetAutoMounterStatus(int32_t aStatus)
{
  if (sAutoMounterSetting) {
    sAutoMounterSetting->SetStatus(aStatus);
  }
}

void
SetAutoMounterMode(int32_t aMode)
{
  XRE_GetIOMessageLoop()->PostTask(
      FROM_HERE,
      NewRunnableFunction(SetAutoMounterModeIOThread, aMode));
}

void
SetAutoMounterSharingMode(const nsCString& aVolumeName, bool aAllowSharing)
{
  XRE_GetIOMessageLoop()->PostTask(
      FROM_HERE,
      NewRunnableFunction(SetAutoMounterSharingModeIOThread,
                          aVolumeName, aAllowSharing));
}

void
AutoMounterFormatVolume(const nsCString& aVolumeName)
{
  XRE_GetIOMessageLoop()->PostTask(
      FROM_HERE,
      NewRunnableFunction(AutoMounterFormatVolumeIOThread,
                          aVolumeName));
}

void
AutoMounterMountVolume(const nsCString& aVolumeName)
{
  XRE_GetIOMessageLoop()->PostTask(
      FROM_HERE,
      NewRunnableFunction(AutoMounterMountVolumeIOThread,
                          aVolumeName));
}

void
AutoMounterUnmountVolume(const nsCString& aVolumeName)
{
  XRE_GetIOMessageLoop()->PostTask(
      FROM_HERE,
      NewRunnableFunction(AutoMounterUnmountVolumeIOThread,
                          aVolumeName));
}

void
ShutdownAutoMounter()
{
  if (sAutoMounter) {
    DBG("ShutdownAutoMounter: About to StopMtpServer");
    sAutoMounter->StopMtpServer();
  }
  sAutoMounterSetting = nullptr;
  sUsbCableObserver = nullptr;

  XRE_GetIOMessageLoop()->PostTask(
      FROM_HERE,
      NewRunnableFunction(ShutdownAutoMounterIOThread));
}

} // system
} // mozilla
