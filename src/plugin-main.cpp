#include <obs-module.h>
#include "vu-meter-source.hpp"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-vu-meter-pro", "en-US")

MODULE_EXPORT const char *obs_module_description(void)
{
    return "Professional stereo LED VU meter source for OBS Studio.";
}

bool obs_module_load(void)
{
    obs_register_source(&vu_meter_source_info);

    blog(LOG_INFO,
         "[OBS VU Meter PRO] loaded - v2.2 obs_volmeter");

    return true;
}

void obs_module_unload(void)
{
    blog(LOG_INFO,
         "[OBS VU Meter PRO] unloaded");
}
