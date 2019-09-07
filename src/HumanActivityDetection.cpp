#if WIN32
#include "windows.h"
#endif
#include "Common.hpp"

namespace t2
{
  void HumanActivityDetectionInit()
  {
  }

  void HumanActivityDetectionDestroy()
  {
  }

#if WIN32
  void PumpOSMessageLoop()
  {
    MSG msg;
    while (PeekMessage(&msg, NULL, NULL, NULL, PM_REMOVE))
    {
      //Translate message
      TranslateMessage(&msg);

      //Dispatch message
      DispatchMessage(&msg);
    }
  }

  static DWORD first_observed_last_input = 0;

  double TimeSinceLastDetectedHumanActivityOnMachine()
  {
    LASTINPUTINFO lastinputInfo;
    lastinputInfo.cbSize = sizeof(LASTINPUTINFO);
    if (GetLastInputInfo(&lastinputInfo) == 0)
      return -1;

    if (first_observed_last_input == 0)
      first_observed_last_input = lastinputInfo.dwTime;

    if (first_observed_last_input == lastinputInfo.dwTime)
      return -1;

    uint64_t current_tick_count = GetTickCount64();
    uint64_t result_ticks = current_tick_count - lastinputInfo.dwTime;

    return  result_ticks / 1000.;
  }
#else
  //only implemented for windows for now
  void PumpOSMessageLoop()
  {
  }
  double TimeSinceLastDetectedHumanActivityOnMachine()
  {
    return -1;
  }
#endif

}
