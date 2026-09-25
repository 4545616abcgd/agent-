# Local legacy material

`legacy_local/` holds earlier patch bundles, duplicate source exports, and
backup material from the ESP-Claw voice experiments. It is intentionally
ignored by Git because some archives may contain local configuration or
credentials. These files are kept on this machine and are not needed to build
the current RTC validation application.

The active RTC validation application is at
[`application/volc_rtc_agent/`](../application/volc_rtc_agent/). The original
ESP-Claw application and its weather/Web capabilities remain at
[`application/edge_agent/`](../application/edge_agent/); that application is
still needed when the validated voice path is integrated back into ESP-Claw.
