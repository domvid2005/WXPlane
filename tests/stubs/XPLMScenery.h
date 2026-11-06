#pragma once
#include "XPLMDefs.h"
float XPLMGetMagneticVariation(double latitude, double longitude);
float XPLMDegTrueToDegMagnetic(float headingDegreesTrue);
float XPLMDegMagneticToDegTrue(float headingDegreesMagnetic);
