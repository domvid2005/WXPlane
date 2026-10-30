// TEST STUB reconstructed from developer.x-plane.com/sdk/XPLMWeatherInfo_t (field order per docs). NOT shipped.
#pragma once
#include "XPLMDefs.h"
#define XPLM_NUM_WIND_LAYERS 13
#define XPLM_NUM_CLOUD_LAYERS 3
#define XPLM_NUM_TEMPERATURE_LAYERS 13
#define XPLM_WIND_UNDEFINED_LAYER -1
typedef struct { float alt_msl, speed, direction, gust_speed, shear, turbulence; } XPLMWeatherInfoWinds_t;
typedef struct { float cloud_type, coverage, alt_top, alt_base; } XPLMWeatherInfoClouds_t;
typedef struct {
  int structSize; float temperature_alt, dewpoint_alt, pressure_alt, precip_rate_alt, wind_dir_alt, wind_spd_alt, turbulence_alt,
  wave_height, wave_length; int wave_dir; float wave_speed, visibility, precip_rate, thermal_climb, pressure_sl;
  XPLMWeatherInfoWinds_t wind_layers[XPLM_NUM_WIND_LAYERS]; XPLMWeatherInfoClouds_t cloud_layers[XPLM_NUM_CLOUD_LAYERS];
  float temp_layers[XPLM_NUM_TEMPERATURE_LAYERS], dewp_layers[XPLM_NUM_TEMPERATURE_LAYERS];
  float troposphere_alt, troposphere_temp, age, radius_nm, max_altitude_msl_ft;
} XPLMWeatherInfo_t;
void XPLMBeginWeatherUpdate(void);
void XPLMEndWeatherUpdate(int, int);
void XPLMSetWeatherAtLocation(double, double, double, XPLMWeatherInfo_t*);
void XPLMSetWeatherAtAirport(const char*, XPLMWeatherInfo_t*);
int XPLMGetWeatherAtLocation(double, double, double, XPLMWeatherInfo_t*);
void XPLMEraseWeatherAtLocation(double, double);
void XPLMEraseWeatherAtAirport(const char*);
