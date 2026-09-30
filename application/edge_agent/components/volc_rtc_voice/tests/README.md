# RTC tool bridge host tests

Run from this directory with GCC and the same cJSON sources as ESP-IDF:

```powershell
gcc -std=c11 -Wall -Wextra -Werror -Istubs -I../../../../volc_rtc_agent/main -I"$env:IDF_PATH/components/json/cJSON" test_tool_bridge.c "$env:IDF_PATH/components/json/cJSON/cJSON.c" -o ../../../build/rtc_tool_bridge_test.exe
../../../build/rtc_tool_bridge_test.exe
gcc -std=c11 -Wall -Wextra -Werror -Istubs -I../../../../../components/common/weather_service/include test_weather_clock.c -o ../../../build/weather_clock_test.exe
../../../build/weather_clock_test.exe
```

The test includes the production parser and capability bridge. Stubs replace
FreeRTOS and device/cloud calls only. It checks request framing, argument
validation, permissions context, allowlisting, JSON escaping and oversized
reply handling. It does not prove cloud tool registration, Bot consumption,
real capability execution, task scheduling or audio quality. Keep the generated
executable in a build directory rather than committing it.

The weather test uses the production service with a fake clock and provider.
It checks that boot-time epochs never reach the provider/cache, synchronized
timestamps are recorded, and offline/config/provider failures preserve old
cached data. Real SNTP/HTTP and worker event timing still need device testing.
