#pragma once
namespace t2
{
void HumanActivityDetectionInit();
void HumanActivityDetectionDestroy();
void PumpOSMessageLoop();
double TimeSinceLastDetectedHumanActivityOnMachine();
} // namespace t2
