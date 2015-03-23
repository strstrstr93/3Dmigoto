#include "Direct3D11Device.h"

static bool gInitialized = false;
static bool gLogInput = false;
static bool gReloadConfigPending = false;

NvAPI_Status CheckStereo();
void FlagConfigReload(HackerDevice *device, void *private_data);
void LoadConfigFile();
void ReloadConfig(HackerDevice *device);
