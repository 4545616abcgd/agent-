# ESP-Claw Long-term Memory fail-soft v3

This package is adapted to the uploaded source snapshot.

Original source SHA256 (`components\claw_modules\claw_memory\src\claw_memory.c`):
`42e2c63b63d3640b81a12609e686bfd34fee437ec74e992631c4c74981e426b4`

Patched SHA256:
`e29b6c73869fb1e542384a917c85e2bcd9f6d4984ec93b76f41e85eb8bae6e91`

## What changes

Only the automatic **Long-term Memory context provider** is changed. If `claw_memory_load_index()` fails (for example, corrupted `/sdcard/memory/memory_index.json`), the provider logs:

`Long-term Memory unavailable, continuing without it: <error>`

and returns `ESP_ERR_NOT_FOUND`, which the current `claw_core_context.c` already treats as “skip this provider and continue”.

Memory tool/storage functions keep their existing error behavior; this package does **not** auto-delete, auto-rebuild, or overwrite a corrupt memory index.

## Install

```powershell
$applyScript = Get-ChildItem "D:\esp-claw" -Recurse -File `
  -Filter "apply_esp_claw_memory_fail_soft_v3.ps1" |
  Select-Object -First 1

powershell.exe -ExecutionPolicy Bypass `
  -File "$($applyScript.FullName)" `
  -RepoRoot "D:\esp-claw" `
  -Mode Memory
```

Expected:

```text
Installed: Long-term Memory provider fail-soft
VERIFY OK
```

Then use ESP-IDF 5.5.4 to incremental Build -> Flash -> Monitor. Do not Full Clean or erase Flash/NVS for this test.
