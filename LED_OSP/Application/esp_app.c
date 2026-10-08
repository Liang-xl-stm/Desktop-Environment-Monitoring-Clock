/**
 * @file    esp_app.c
 * @brief   ESP8266 联网、升级存储准备和 TCP 事件处理。
 *
 * @details 本文件的启动顺序：
 *          1. 初始化 USART1 日志输出。
 *          2. 检测 W25Q128，检查 A/B 管理记录，必要时首次备份当前 App。
 *          3. 初始化 LwESP，创建 USART3 接收任务。
 *          4. 复位 ESP8266，设置 STA 模式，连接 Wi-Fi 并查询 IP。
 *          5. Flash 可用时创建常驻下载任务，再启用开发板 TCP 服务器，按基线状态限制 FWUP。
 *
 *          硬件初始化和调度器启动在 USER/main.c；
 *          本任务由 Application/app.c 的 App_Init() 创建。
 *          Wi-Fi 名称、密码和服务器端口在 USER/app_config.h，电脑自动搜索开发板。
 *
 *          接收链路：
 *          USART3 中断 -> 串口环形缓存 -> ESP_Rx_Task -> LwESP 解析
 *          -> Tcp_Event -> 下载队列 -> FW_TCP 任务 -> W25Q128。
 *          download_stream.c 分辨图片与固件；原固件正文仍由 firmware_stream.c 处理。
 *
 * @note    LED 和 USART1 回显由独立任务处理，不等待本任务连接网络。
 *          普通网络错误只删除本任务；日志资源/接收任务创建失败仍触发断言。
 *          当前只下载并校验候选固件，不在此擦写片内 App、重启或执行回滚。
 */

#include "stm32f4xx.h"
#include "usart.h"
#include "w25q128.h"
#include "w25q128_test.h"
#include "firmware_tcp.h"
#include "firmware_startup.h"
#include "firmware_tcp_config.h"
#include "log.h"
#include "app_config.h"
#include "esp_app.h"
#include "elog.h"
#include "misc.h"
#include "FreeRTOS.h"
#include "task.h"
#include "lwesp/lwesp.h"
#include "lwesp/lwesp_input.h"
#include <stdio.h>

/**
 * @brief 打印启动准备的具体原因及只读管理记录快照。
 * @details 无参数，诊断来自最近一次 Firmware_StartupPrepare()。
 * @return 无，仅输出日志。
 * @pre Log_Init() 已成功，当前位于调度器运行中的任务。
 * @note 诊断 getter 读取 RAM 快照；本函数末尾可能另外读取 Flash SR1。
 *       不擦除管理区、不写固件、不输出 Wi-Fi 配置。
 */
static void App_PrintFirmwareStartupDiagnostic(void)
{
    /* 下标对应 firmware_startup_status_t；枚举顺序变化时要同步此表。 */
    static const char *const reasons[] =
    {
        "NOT_RUN", "READY_EXISTING", "READY_CREATED", "INVALID_ARGUMENT",
        "BIND_FAILED", "BOOT_PENDING", "CONFIRM_INVALID", "INIT_DISABLED",
        "META_READ_FAILED", "META_NOT_BLANK", "BASELINE_FAILED"
    };
    /* 对象由固件模块保存，返回的地址持续有效；这里只读，不修改其字段。 */
    const firmware_startup_diagnostic_t *diagnostic =
        Firmware_StartupGetDiagnostic();
    uint32_t status = (uint32_t)diagnostic->status;
    uint32_t copy, offset, byte_index; /* 管理副本编号、记录内偏移、行内字节编号。 */
    char row[61]; /* 每行 20 字节，每字节用“XX ”三字符表示，末尾再留一个 '\0'。 */
    uint8_t sr1;  /* Flash 状态寄存器 1：bit0=BUSY，bit1=WEL 等状态位。 */

    /* 先检查表的范围；未知状态打印 UNKNOWN，避免用非法下标读取字符串。 */
    elog_i("firmware", "startup reason=%s (%lu)",
           status < sizeof(reasons) / sizeof(reasons[0]) ? reasons[status] : "UNKNOWN",
           (unsigned long)status);

    /* 已解读到管理记录时，显示启动阶段与两种槽位：
     * confirmed：已确认的备份槽；candidate：新固件候选槽。
     * 槽位 0=A、1=B、255=NONE；启动阶段含 NORMAL/PENDING/INSTALLING/TRIAL/ROLLBACK。
     * 字段的含义定义在 firmware_update.h，不能把候选已保存理解为已安装。
     */
    if (status == FW_STARTUP_READY_EXISTING || status == FW_STARTUP_BOOT_PENDING ||
        status == FW_STARTUP_CONFIRM_INVALID)
    {
        elog_i("firmware", "metadata phase=%lu, confirmed=%lu, candidate=%lu",
               (unsigned long)diagnostic->boot_phase,
               (unsigned long)diagnostic->confirmed_slot,
               (unsigned long)diagnostic->candidate_slot);
    }

    /* 管理记录非空白或读取失败时，打印两份原始记录，便于定位损坏位置。 */
    if (status == FW_STARTUP_META_NOT_BLANK || status == FW_STARTUP_META_READ_FAILED)
    {
        if (Firmware_StartupGetCleanupDiagnostic()->status != FW_CLEANUP_NOT_RUN)
        {
            elog_w("firmware", "record snapshots below were taken before cleanup");
        }
        /* META0/META1 是管理记录的双副本，不是固件正文的 A/B 两个槽。 */
        for (copy = 0U; copy < 2U; ++copy)
        {
            elog_w("firmware", "META%lu addr=%06lX read=%s", (unsigned long)copy,
                   (unsigned long)(copy == 0U ? FIRMWARE_META0_ADDR : FIRMWARE_META1_ADDR),
                   diagnostic->raw_read_ok[copy] ? "OK" : "FAILED");
            /* 读失败时缓冲内容不能当作真实记录解释，直接跳过。 */
            if (!diagnostic->raw_read_ok[copy])
            {
                continue;
            }
            for (offset = 0U; offset < FIRMWARE_META_RECORD_SIZE; offset += 20U)
            {
                /* 当前记录长度为 60 字节，每行打印 20 字节，共三行。
                 * snprintf 参数：写入位置、最多占用 4 字节、格式、当前原始字节。
                 * 四字节包括“两位十六进制 + 空格 + 结束符”；下一组覆盖前一组结束符。
                 * 先拼整行再输出，日志互斥保证其他任务不能插入半行。
                 */
                for (byte_index = 0U; byte_index < 20U; ++byte_index)
                {
                    (void)snprintf(&row[byte_index * 3U], 4U, "%02X ",
                                   (unsigned int)diagnostic->raw[copy][offset + byte_index]);
                }
                elog_w("firmware", "META%lu +%02lu: %s", (unsigned long)copy,
                       (unsigned long)offset, row);
            }
        }
    }

    /* 对失败或未就绪状态补充采样当前 SR1；它不是之前失败时刻的历史 SR1。 */
    if (status >= FW_STARTUP_INVALID_ARGUMENT)
    {
        if (W25Q128_ReadStatus(&sr1))
        {
            elog_w("firmware", "Flash SR1=%02X", (unsigned int)sr1);
        }
        else
        {
            elog_e("firmware", "Flash SR1 read failed");
        }
    }
}

/**
 * @brief 打印本次旧管理区恢复的具体环节、异常地址及擦除/校验结果。
 * @details 无参数，结果来自最近一次 Firmware_StartupClearLegacyMetadata()。
 * @return 无，仅输出日志。
 * @pre 日志已初始化；管理区恢复已结束，读取诊断时不并发再次执行恢复。
 * @note getter 只读取 RAM 快照，本函数不会再次读写 Flash。
 *       erase_attempted：曾尝试擦除；erase_completed：擦除后完整读回为 FF。
 *       两个 mask 都是 bit0=META0、bit1=META1；例如 3 表示两个副本均置位。
 */
static void App_PrintFirmwareCleanupDiagnostic(void)
{
    /* 下标与 firmware_cleanup_status_t 对应，输出明确的拒绝或失败环节。 */
    static const char *const reasons[] =
    {
        "NOT_RUN", "READY", "VALID_RECORD", "META0_READ_FAILED",
        "META1_READ_FAILED", "META0_PROTECTED", "META1_PROTECTED",
        "PREFIX_MISMATCH", "META1_ERASE_FAILED", "META1_VERIFY_READ_FAILED",
        "META1_VERIFY_DIRTY", "META0_ERASE_FAILED", "META0_VERIFY_READ_FAILED",
        "META0_VERIFY_DIRTY"
    };
    const firmware_cleanup_diagnostic_t *diagnostic = Firmware_StartupGetCleanupDiagnostic();
    uint32_t status = (uint32_t)diagnostic->status;

    elog_i("firmware", "cleanup result=%s, erase_attempted=%u, erase_verified=%u",
           status < sizeof(reasons) / sizeof(reasons[0]) ? reasons[status] : "UNKNOWN",
           (unsigned int)diagnostic->erase_attempted_mask,
           (unsigned int)diagnostic->erase_completed_mask);
    /* NO_ADDRESS 是无相关地址的哨兵，不是实际 Flash 地址。 */
    if (diagnostic->address != FIRMWARE_CLEANUP_NO_ADDRESS)
    {
        /* 只有读取成功并标记 byte_valid 时，observed_byte 才可用于判断。 */
        if (diagnostic->byte_valid)
        {
            elog_w("firmware", "cleanup address=%06lX, byte=%02X",
                   (unsigned long)diagnostic->address, (unsigned int)diagnostic->observed_byte);
        }
        else
        {
            elog_w("firmware", "cleanup address=%06lX", (unsigned long)diagnostic->address);
        }
    }
    /* NOR 擦除态为 FF；记录 META1 首个非 FF 字节，便于发现扇区尾部遗留数据。 */
    if (diagnostic->meta1_first_non_ff != FIRMWARE_CLEANUP_NO_ADDRESS)
    {
        elog_i("firmware", "META1 first non-FF address=%06lX",
               (unsigned long)diagnostic->meta1_first_non_ff);
    }
    /* 恢复模块保存了结束后的 SR1 采样；读取失败时不能把默认值当真实状态。 */
    if (diagnostic->sr1_valid)
    {
        elog_i("firmware", "cleanup Flash SR1=%02X", (unsigned int)diagnostic->sr1);
    }
    else
    {
        elog_w("firmware", "cleanup Flash SR1 unavailable");
    }
}

/**
 * @brief ARMCC 链接器提供的完整加载映像地址与末地址。
 * @details Base 是内部 Flash 中加载映像的第一个字节，当前 App 从 0x08020000 开始。
 *          Limit 指向映像最后一个字节之后，因此 Limit - Base 得到完整字节数。
 * @note 包含代码、常量及 RW 初始化的加载数据，不包括 RAM 的 ZI 内容。
 *       RAM 的堆、任务栈等零初始化内容不会以同样大小存入 BIN。
 *       这些地址由链接器生成，不需要在 C 文件里定义数组或手填固件长度。
 */
extern const uint8_t Load$$LR$$LR_IROM1$$Base[];
extern const uint8_t Load$$LR$$LR_IROM1$$Limit[];

/**
 * @brief 从 USART3 缓存取数据，并交给 LwESP。
 * @param[in] argument 未使用，xTaskCreate 时传 NULL。
 * @return 正常运行不返回，持续处理模块的 AT 应答和网络数据。
 * @pre USART3 和 LwESP 已初始化；串口缓存仅由本任务读取。
 * @note 当前 LWESP_CFG_INPUT_USE_PROCESS=1，lwesp_input_process 同步解析本批字节。
 *       buffer 是任务栈上的临时缓冲，解析返回后可复用，不能把地址交给其他任务长期保存。
 */
static void ESP_Rx_Task(void *argument)
{
    (void)argument;
    uint8_t buffer[64]; /* 一次取出的串口字节，不代表一个完整 AT 应答或 TCP 包。 */
    uint16_t received;  /* 本轮实际取出的长度，可能从 0 到 64。 */

    for (;;)
    {
        /* 参数 1：输出缓冲地址；参数 2：最大可写容量。
         * 该函数只读取已发布的串口缓存，不等待新字节，返回实际复制长度。
         */
        received = USART3_Receive(buffer, sizeof(buffer));

        if (received > 0)
        {
            /* 参数 1：串口字节地址；参数 2：实际有效长度。
             * 同一输入既可能是 AT 文本，也可能包含二进制升级正文，不能用 strlen。
             * LwESP 根据自身解析状态区分 AT 应答和 TCP 接收事件。
             * 此处忽略返回值；TCP 接收、复制及写入错误由固件接入模块另行处理。
             */
            (void)lwesp_input_process(buffer, received);
        }
        else
        {
            /* 没有数据时阻塞本任务 1ms，让其他任务运行。
             * 有数据时连续取下一批，避免缓存长时间积压。
             */
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }
}

/**
 * @brief 处理 TCP 连接状态，将接收数据交给图片/固件下载任务。
 * @param[in] evt LwESP 传入的事件，内容仅在本次回调中使用。
 * @retval lwespOK 本次事件处理结束，不代表固件已完整接收或安装成功。
 * @note 回调由 LwESP 输入解析或内部任务调用，不是 USART3 硬件中断。
 *       不保存 evt 地址，不把接收 pbuf 当作跨任务长期有效的缓冲。
 *       不在接收回调里擦写 Flash、等待网络命令或执行阻塞的 TCP 发送。
 *       本回调是“连接事件回调”，不同于 lwesp_init 的全局事件回调。
 */
static lwespr_t Tcp_Event(lwesp_evt_t *evt)
{
    /* evt->type 的具体访问通过 getter 完成，根据事件选择相应处理。 */
    switch (lwesp_evt_get_type(evt))
    {
    case LWESP_EVT_CONN_ACTIVE:
        /* 先做应用层门禁并通知 worker，额外连接被非阻塞关闭。 */
        Firmware_TCP_OnActive(evt);
        elog_i("tcp", "connected");
        break;

    case LWESP_EVT_CONN_ERROR:
        /* 建连失败；error getter 只用于 CONN_ERROR 事件，打印库返回的错误枚举值。 */
        elog_e("tcp", "connect failed, result=%d",
               (int)lwesp_evt_conn_error_get_error(evt));
        break;

    case LWESP_EVT_CONN_CLOSE:
        /* 通知升级任务终止接收。
         * 未完成的数据不会成为有效候选；已完成提交的候选不会因断开而撤销。
         */
        Firmware_TCP_OnClosed(evt);
        elog_w("tcp", "closed");
        break;
    case LWESP_EVT_CONN_RECV:
        /* 此接口取出事件中的 pbuf，并把实际数据复制到固件模块自己的队列。
         * 它负责通知 LwESP 数据已消费；库在回调返回后释放接收缓冲。
         * FW_TCP 任务分辨 PIC1/FWUP，顺序写入独立图片槽或固件槽并校验 CRC。
         * 一次事件可能只有包头的一部分，也可能同时含包头和正文，不能按事件分包。
         */
        Firmware_TCP_OnReceive(evt);
        break;

    case LWESP_EVT_CONN_SEND:
        /* SEND 通知解除持久应答缓冲占用；回调不等待 worker。 */
        Firmware_TCP_OnSent(evt);
        break;

    default:
        /* 其他连接事件由库按自身流程处理；本应用没有额外动作。 */
        break;
    }

    return lwespOK;
}

/**
 * @brief 准备升级存储，初始化 ESP8266，并启用常驻 TCP 下载服务器。
 * @param[in] argument 未使用，创建任务时传 NULL。
 * @return 正常运行不返回；网络失败记录日志后删除本任务。
 * @pre main 已完成 USART1/USART3、SPI1/W25Q128 初始化，FreeRTOS 调度器已运行。
 * @details 普通联网失败时 vTaskDelete(NULL) 只删除当前任务，不关闭其他任务。
 *          已创建的 ESP_RX、LwESP 内部线程、FW_TCP 不在这些错误分支中销毁。
 *          当前不自动重连；网络恢复后可复位设备重新执行启动流程。
 * @note configASSERT 仍用于日志初始化或接收任务创建失败；这些资源错误会停机。
 */
void ESP8266_Task(void *argument)
{
    (void)argument;
    lwespr_t result;         /* LwESP API 返回码，lwespOK 表示成功。 */
    BaseType_t task_result;  /* FreeRTOS 任务创建结果，pdPASS 表示成功。 */
    lwesp_ip_t ip, gw, nm;   /* 模块 IP、网关、子网掩码；它们是结构体，不是字符串。 */
    uint32_t flash_id = 0;   /* 24 位 JEDEC ID，本板 W25Q128 通常为 EF4018。 */
    uint8_t firmware_ready = 0U; /* 仅表示 A/B 存储允许接收固件，不用于限制图片。 */
    uint8_t flash_ready = 0U;    /* 图片只依赖 Flash 可用，不依赖固件 A/B 基线。 */
    uint8_t download_ready = 0U; /* 图片/固件共享 TCP 接收队列创建结果。 */
    uint8_t log_initialized; /* Log_Init 的成功标志：1 成功、0 失败。 */
    /* 用 uintptr_t 进行地址数值运算，结果单位是字节，不是 uint32_t 字的个数。 */
    uint32_t current_image_size =
        (uint32_t)((uintptr_t)Load$$LR$$LR_IROM1$$Limit -
                   (uintptr_t)Load$$LR$$LR_IROM1$$Base);

    /* 第 1 步：先初始化日志，后面的每一个启动阶段才能输出诊断。
     * Log_Init 无参数，固定使用 USART1，内部创建格式化和整行输出的递归互斥。
     * 成功后 Console 任务才能经 Log_WriteRaw 原样回显，避免与日志交错。
     */
    log_initialized = Log_Init();
    configASSERT(log_initialized != 0U);
    elog_i("app", "FreeRTOS application started");

    /* 第 2 步：确认外部 Flash 可读。
     * &flash_id 是输出参数；API 返回 1 表示读成功，0 表示通信或参数错误。
     * 下面按厂商字节 EF 和容量字节 18 筛选；中间器件类型字节未参与比较。
     * 默认自检关闭；首次建立基线或启用的旧管理区清理可能擦写外部 Flash。
     */
    if (W25Q128_ReadJEDECID(&flash_id) &&
        (flash_id & 0xFF0000UL) == 0xEF0000UL && (flash_id & 0xFFU) == 0x18U)
    {
        elog_i("flash", "W25Q128 JEDEC ID: %06X", (unsigned int)flash_id);
        flash_ready = 1U;
        /* 自检行为由 USER/w25q128_config.h 的 W25Q128_TEST_MODE 决定：
         * 0 关闭；1 擦写专用测试区并校验；2 只读回检查，不在这里擦写。
         */
        w25q128_test();
        /* 注册当前片内 App，并检查外部 A/B 管理状态。
         * 参数 1：片内 App 加载映像起始地址。
         * 参数 2：片内 App 分区容量，当前为 896KiB，并非当前固件实际长度。
         * 参数 3：当前固件实际加载长度，用于首次备份当前 App。
         * 参数 4：1 允许仅在管理记录全空白时首次建立 A 槽基线；0 禁止首次建立。
         * 返回 1：准备成功；返回 0：失败或已有待安装/试运行等状态，不开放新下载。
         * 已有有效记录或损坏的非空白记录不会在此自动覆盖。
         */
        firmware_ready = Firmware_StartupPrepare(
            Load$$LR$$LR_IROM1$$Base,
            FIRMWARE_APP_CAPACITY,
            current_image_size,
            FIRMWARE_ALLOW_INITIAL_BASELINE);
#if FIRMWARE_CLEAR_LEGACY_METADATA
        /* 编译开关为 0 时不编译此分支。
         * 仅针对本次已授权且指纹匹配的旧管理数据执行恢复，不是每次启动格式化 Flash。
         */
        if (!firmware_ready && FIRMWARE_ALLOW_INITIAL_BASELINE &&
            Firmware_StartupGetDiagnostic()->status == FW_STARTUP_META_NOT_BLANK)
        {
            /* 用户已授权删除本次旧数据；接口仍检查完整扇区与旧数据指纹。
             * 升级任务尚未创建，此处不会与下载服务并发操作管理区。
             */
            elog_w("firmware", "checking legacy management cleanup at %06lX/%06lX",
                   (unsigned long)FIRMWARE_META0_ADDR, (unsigned long)FIRMWARE_META1_ADDR);
            /* 无参数；返回 1 表示两个管理扇区已经验证全 FF，0 表示拒绝或失败。
             * 恢复函数保留有效/疑似升级管理记录和 A/B 固件正文，避免破坏待升级状态。
             */
            if (Firmware_StartupClearLegacyMetadata())
            {
                App_PrintFirmwareCleanupDiagnostic();
                elog_i("firmware", "management blank verified; creating current App baseline in A");
                /* 清理成功只说明管理区可用，仍需重新创建当前 App 的 A 槽基线。 */
                firmware_ready = Firmware_StartupPrepare(
                    Load$$LR$$LR_IROM1$$Base,
                    FIRMWARE_APP_CAPACITY,
                    current_image_size,
                    FIRMWARE_ALLOW_INITIAL_BASELINE);
            }
            else
            {
                App_PrintFirmwareCleanupDiagnostic();
                elog_e("firmware", "management cleanup failed or protected; firmware download disabled");
            }
        }
#endif
        elog_i("firmware", "image bytes=%lu, storage=%s",
               (unsigned long)current_image_size,
               firmware_ready ? "READY" : "NOT READY");
        /* 输出固件准备原因；图片接收只要求 Flash 可用，不修改这份固件管理状态。 */
        App_PrintFirmwareStartupDiagnostic();
    }
    else
    {
        /* Flash 不可用时 firmware_ready 保持 0；仍继续联网，但不会开放固件下载。
         * LED 与控制台在各自任务中运行，不依赖 Flash 检测结果。
         */
        elog_e("flash", "W25Q128 unavailable, JEDEC ID: %06X", (unsigned int)flash_id);
    }

    /* 第 3 步：建立 LwESP 的线程、消息队列、信号量和底层串口适配。
     * 参数 1 NULL：不注册用户全局事件回调，采用库默认回调。
     * 参数 2 0：如果配置了“初始化时复位”，该复位使用非阻塞方式。
     * 当前 LWESP_CFG_RESET_ON_INIT=0，不在 init 中发起复位；稍后显式调用 reset。
     * 此处 lwespOK 只表示库初始化成功，还不代表模块已联网或 TCP 已连接。
     */
    result = lwesp_init(NULL, 0);
    if (result != lwespOK)
    {
        elog_e("wifi", "initialization failed, result=%d", (int)result);
        vTaskDelete(NULL);
        return;
    }

    /* 第 4 步：接收任务必须先于模块复位和网络命令建立。
     * xTaskCreate 参数依次是：
     * ESP_Rx_Task：任务入口；"ESP_RX"：调试时看到的任务名称；
     * 512U：栈深度（32 位字），实际 2048 字节；NULL：任务参数未使用；
     * 3U：任务优先级，高于本任务的 2；NULL：不保存创建后的任务句柄。
     * 返回 pdPASS 表示成功；失败可能是 FreeRTOS 堆空间不足。
     */
    task_result = xTaskCreate(ESP_Rx_Task, "ESP_RX",
                              512U, NULL, 3U, NULL);
    configASSERT(task_result == pdPASS);

    /* 第 5 步：模块复位并由库执行默认 AT 配置，接收任务负责解析模块应答。
     * 参数 1 NULL：不设置“命令完成”回调；参数 2 NULL：没有回调自定义参数；
     * 参数 3 1：阻塞当前任务，直到命令成功、失败或超时。
     * 当前硬件适配用 PF6 控制 ESP8266 的低电平复位。
     * 阻塞只等待本任务的命令结果，其他可运行的 FreeRTOS 任务继续调度。
     */
    result = lwesp_reset(NULL, NULL, 1);
    if (result != lwespOK)
    {
        elog_e("wifi", "initialization failed, result=%d", (int)result);
        vTaskDelete(NULL);
        return;
    }
    /* 第 6 步：设置为 STA，ESP8266 作为客户端连接热点。
     * 参数：模式 LWESP_MODE_STA、完成回调 NULL、回调参数 NULL、阻塞标志 1。
     * 这里只设置工作模式，不会自动执行下面的 Wi-Fi 加入步骤。
     */
    result = lwesp_set_wifi_mode(LWESP_MODE_STA, NULL, NULL, 1);
    if (result != lwespOK)
    {
        elog_e("wifi", "initialization failed, result=%d", (int)result);
        vTaskDelete(NULL);
        return;
    }
    /* 第 7 步：连接 app_config.h 中配置的 Wi-Fi。
     * 参数 1 WIFI_NAME：热点名称；参数 2 WIFI_PASSWORD：热点密码；
     * 参数 3 NULL：不指定 AP 的 BSSID/MAC，由库按名称选择；
     * 参数 4 NULL：不使用命令完成回调；参数 5 NULL：无回调自定义参数；
     * 参数 6 1：阻塞等待加入完成。返回 lwespOK 表示该命令成功。
     * ESP8266 使用 2.4GHz Wi-Fi；此处失败后不自动重试。
     */
    result = lwesp_sta_join(WIFI_NAME, WIFI_PASSWORD, NULL, NULL, NULL, 1);
    if (result != lwespOK)
    {
        elog_e("wifi", "initialization failed, result=%d", (int)result);
        vTaskDelete(NULL);
        return;
    }
    /* 第 8 步：读取模块当前地址信息，不修改地址。
     * 参数 1 &ip：模块地址输出；参数 2 &gw：网关输出；参数 3 &nm：子网掩码输出；
     * 参数 4/5 NULL：无完成回调及回调参数；参数 6 1：等待读取完成后返回。
     * 读取成功后再访问结构体；失败时不能把未填写的地址当真实网络状态。
     */
    result = lwesp_sta_getip(&ip, &gw, &nm, NULL, NULL, 1);
    if (result != lwespOK)
    {
        elog_e("wifi", "initialization failed, result=%d", (int)result);
        vTaskDelete(NULL);
        return;
    }
    /* IPv4 地址是四个数值字节，逐个打印；不能用 printf("%s", ip) 输出结构体。 */
    elog_i("wifi", "IP: %u.%u.%u.%u",
           (unsigned int)ip.addr.ip4.addr[0],
           (unsigned int)ip.addr.ip4.addr[1],
           (unsigned int)ip.addr.ip4.addr[2],
           (unsigned int)ip.addr.ip4.addr[3]);
    /* 第 9 步：Flash 可用即可创建下载任务；参数决定是否允许 FWUP。
     * 0 只接收 PIC1 图片；1 同时允许固件下载。A/B 管理记录异常不会开放固件擦写。
     */
    if (flash_ready) download_ready = Firmware_TCP_Init(firmware_ready);
    if (download_ready)
    {
        if (!firmware_ready) elog_w("firmware", "firmware download disabled; pictures allowed");
        /* 第 10 步：开发板常驻监听，电脑软件可随时搜索并连接，不需要先启动或再次复位。
         * 参数 1 1：启用服务器；参数 2：监听端口 8080；
         * 参数 3 1：请求最多一个客户端（应用层另有门禁兼容忽略此设置的 AT 固件）；
         * 参数 4 120：模块 TCP 空闲超时，单位秒；应用下载超时仍为 30 秒；
         * 参数 5 Tcp_Event：服务器每条连接事件；参数 6/7 NULL：无完成回调及其参数；
         * 参数 8 1：仅此启动命令阻塞等待完成，连接事件始终使用非阻塞接口。
         */
        result = lwesp_set_server(1U, TCP_SERVER_PORT, 1U, 120U,
                                  Tcp_Event, NULL, NULL, 1U);
        if (result != lwespOK)
        {
            elog_e("tcp", "server failed, result=%d; LED/screen remain active", (int)result);
            vTaskDelete(NULL);
            return;
        }
        elog_i("tcp", "picture/firmware server listening on %u", (unsigned int)TCP_SERVER_PORT);
    }
    else
    {
        elog_e("tcp", "download disabled: Flash unavailable or queue/task allocation failed");
    }

    for (;;)
    {
        /* 启动工作完成后本任务每秒醒来一次，当前没有额外周期业务。
         * vTaskDelay 的参数是节拍数；pdMS_TO_TICKS 将 1000ms 转为 RTOS 节拍。
         * 等待期间不占用 CPU；接收和固件写入继续由 ESP_RX / FW_TCP 等任务处理。
         * 此循环不检查升级标志、不擦除片内 App，也不会自动安装或重启。
         */
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

