# Arduino Due + FreeRTOS (PlatformIO)

Bare-metal FreeRTOS on the Arduino Due (Atmel SAM3X8E, ARM Cortex-M3 @ 84 MHz),
built with PlatformIO and the Arduino framework.

## Project structure

```
├── platformio.ini          # build, upload, and monitor configuration
├── include/
│   └── FreeRTOSConfig.h    # FreeRTOS tuning for SAM3X8E
└── src/
    └── main.cpp            # vector table patch, hooks, and demo tasks
```

## Non-obvious integration issues

Getting FreeRTOS running alongside the Arduino SAM core required solving three
distinct problems. They are documented here because none of them are obvious from
the FreeRTOS or Arduino documentation.

---

### 1. FreeRTOS library selection

`feilipu/FreeRTOS` (the most common search result) is **AVR-only** and will not
compile for the SAM3X8E. The correct library for ARM-based Arduino boards is
`bojit/PlatformIO-FreeRTOS`, which downloads the official FreeRTOS kernel at
build time and selects the right portable layer via a Python script.

The script detects the CPU by scanning preprocessor defines. For the SAM3X8E it
needs to see `SAM3X` to select the `GCC/ARM_CM3` port — this is **not** defined
by the Arduino framework, so it must be added manually:

```ini
build_flags = -DSAM3X
```

---

### 2. SysTick conflict

The Arduino SAM core defines a **strong** (non-weak) `SysTick_Handler` in
`cores/arduino/cortex_handlers.c`. FreeRTOS's ARM_CM3 port defines its own
`SysTick_Handler` as `__attribute__((weak))`, so the Arduino version wins and
FreeRTOS never receives a tick. The scheduler starts but no task ever runs.

The fix is `sysTickHook()`, an extension point that Arduino's `SysTick_Handler`
calls before its own tick logic:

```cpp
extern "C" int sysTickHook(void) {
    if (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED) {
        xPortSysTickHandler();  // drive the FreeRTOS tick
        return 1;               // non-zero: skip Arduino's TimeTick_Increment
    }
    return 0;  // let Arduino handle it before the scheduler is up
}
```

---

### 3. SVC / PendSV handler conflict — the core issue

The Arduino SAM core also defines **strong** `SVC_Handler` and `PendSV_Handler`
in `cortex_handlers.c`:

```c
void SVC_Handler (void) { svcHook();   }
void PendSV_Handler(void) { pendSVHook(); }
```

FreeRTOS uses SVCall to start the first task and PendSV for every context
switch. Its handlers (`vPortSVCHandler`, `xPortPendSVHandler`) are **naked
assembly functions** that assume they are entered directly as exception handlers
— the hardware exception frame is on the stack and `LR` holds an `EXC_RETURN`
token.

When invoked via a C function call (`svcHook()` / `pendSVHook()`), the stack
layout is completely different and `bx lr` inside the naked handler branches back
into C code instead of performing an exception return. The result is an
immediate hard fault; the board appears dead with no LED activity and no serial
output.

The fix is to **patch the live vector table** in RAM before starting the
scheduler, pointing entries 11 (SVCall) and 14 (PendSV) directly to the FreeRTOS
handlers, bypassing Arduino's wrappers entirely:

```cpp
static uint32_t ramVT[64] __attribute__((aligned(256)));

static void installFreeRTOSVectors(void) {
    memcpy(ramVT, (const void *)SCB->VTOR, sizeof(ramVT));
    ramVT[11] = (uint32_t)vPortSVCHandler;    // SVCall
    ramVT[14] = (uint32_t)xPortPendSVHandler; // PendSV
    __DSB();
    SCB->VTOR = (uint32_t)ramVT;
    __DSB();
    __ISB();
}
```

Call `installFreeRTOSVectors()` in `setup()` before `vTaskStartScheduler()`.

The 256-byte alignment satisfies the VTOR alignment requirement for a 61-entry
(244-byte) SAM3X8E vector table.

---

## Building and flashing

```sh
# build
pio run

# upload
pio run -t upload

# monitor (115200 baud)
pio device monitor
```

## FreeRTOS configuration highlights (`include/FreeRTOSConfig.h`)

| Setting | Value | Notes |
|---------|-------|-------|
| `configCPU_CLOCK_HZ` | 84 000 000 | SAM3X8E PLL output |
| `configTICK_RATE_HZ` | 1 000 | 1 ms tick via SysTick |
| `configTOTAL_HEAP_SIZE` | 48 KB | half of the 96 KB SRAM |
| `configMINIMAL_STACK_SIZE` | 128 words | 512 bytes per task minimum |
| `configPRIO_BITS` | 4 | SAM3X8E NVIC priority bits |
| `configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY` | 5 | ISRs at priorities 0–4 may not call FreeRTOS API |
| `configCHECK_FOR_STACK_OVERFLOW` | 2 | stack-paint method; calls `vApplicationStackOverflowHook` |
