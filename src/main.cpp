#include <Arduino.h>
#include <FreeRTOS.h>
#include <task.h>
#include <string.h>

#define CON Serial      /* programming port — always present via ATmega16U2 */

extern "C" {
    extern void vPortSVCHandler(void);
    extern void xPortPendSVHandler(void);
    extern void xPortSysTickHandler(void);
}

/* The Arduino SAM core defines non-weak SVC_Handler and PendSV_Handler that
   call svcHook()/pendSVHook() via a C wrapper. FreeRTOS's naked asm handlers
   cannot survive a C call frame, so we patch the live vector table to point
   SVC and PendSV directly to the FreeRTOS handlers before starting the
   scheduler. SAM3X8E has 45 peripheral IRQs → 61 entries total; 256-byte
   alignment satisfies VTOR requirements for a 244-byte table.              */
static uint32_t ramVT[64] __attribute__((aligned(256)));

static void installFreeRTOSVectors(void) {
    memcpy(ramVT, (const void *)SCB->VTOR, sizeof(ramVT));
    ramVT[11] = (uint32_t)vPortSVCHandler;    /* SVCall  */
    ramVT[14] = (uint32_t)xPortPendSVHandler; /* PendSV  */
    __DSB();
    SCB->VTOR = (uint32_t)ramVT;
    __DSB();
    __ISB();
}

/* Called from Arduino's SysTick_Handler. Returning non-zero skips Arduino's
   TimeTick_Increment so FreeRTOS owns the tick once the scheduler is up.   */
extern "C" int sysTickHook(void) {
    if (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED) {
        xPortSysTickHandler();
        return 1;
    }
    return 0;
}

extern "C" {
    void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName) {
        (void)xTask;
        CON.print("STACK OVERFLOW: ");
        CON.println(pcTaskName);
        for (;;);
    }

    void vApplicationMallocFailedHook(void) {
        CON.println("MALLOC FAILED");
        for (;;);
    }
}

static void taskBlink(void *pvParameters) {
    (void)pvParameters;
    pinMode(LED_BUILTIN, OUTPUT);
    for (;;) {
        digitalWrite(LED_BUILTIN, HIGH);
        vTaskDelay(pdMS_TO_TICKS(500));
        digitalWrite(LED_BUILTIN, LOW);
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

static void taskSerial(void *pvParameters) {
    (void)pvParameters;
    for (;;) {
        CON.println("FreeRTOS running on Arduino Due");
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void setup() {
    CON.begin(115200);

    installFreeRTOSVectors();

    xTaskCreate(taskBlink,  "Blink",  configMINIMAL_STACK_SIZE,     NULL, 1, NULL);
    xTaskCreate(taskSerial, "Serial", configMINIMAL_STACK_SIZE * 2, NULL, 1, NULL);

    vTaskStartScheduler();
}

void loop() {
    /* Never reached */
}
