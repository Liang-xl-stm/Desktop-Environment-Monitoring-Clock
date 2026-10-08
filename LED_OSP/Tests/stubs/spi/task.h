typedef void *TaskHandle_t;
#define taskSCHEDULER_RUNNING 2
int xTaskGetSchedulerState(void);
TaskHandle_t xTaskGetCurrentTaskHandle(void);
void vTaskDelay(TickType_t ticks);
