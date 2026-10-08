/**
 * @file firmware_update.h
 * @brief 外部 Flash A/B 双槽固件下载、校验和启动确认接口。
 * @details 与配套 firmware_update.c 一起使用，替换旧单区接口。
 *          本服务不安装内部 Flash、不跳转 App，也不配置看门狗；
 *          Bootloader 的安装、试运行、回滚阶段需要另外实现。
 * @note 所有接口由同一个升级任务串行调用，不能并发或在中断内调用。
 *       SPI 互斥只保护一次 Flash 操作，不保护完整升级会话。
 */
#ifndef FIRMWARE_UPDATE_H
#define FIRMWARE_UPDATE_H

#include <stdint.h>

/** @brief TCP 包标识，小端字节为 46 57 55 50，即 "FWUP"。 */
#define FIRMWARE_MAGIC             0x50555746UL
/** @brief 包格式版本，不是应用固件版本号。 */
#define FIRMWARE_PROTOCOL_VERSION  1UL
/** @brief TCP 包头的固定传输字节数，不使用 sizeof 确定线协议。 */
#define FIRMWARE_HEADER_SIZE       16U

/**
 * @brief TCP 测试包头：16 字节包头之后紧跟原始 .bin 正文。
 * @note 四个整数按小端逐字段解析，不能直接强制转换接收缓冲指针。
 *       TCP 可能拆分、合并数据，接收层需要累计和分离包头、正文。
 */
typedef struct
{
    uint32_t magic;            /**< 固定为 FIRMWARE_MAGIC。 */
    uint32_t protocol_version; /**< 固定为 FIRMWARE_PROTOCOL_VERSION。 */
    uint32_t firmware_size;    /**< 正文字节数，不含包头。 */
    uint32_t firmware_crc32;   /**< 正文的 CRC-32/ISO-HDLC。 */
} firmware_header_t;

/** @brief 管理副本 0，占用 0x0FE000～0x0FEFFF，一个 4KB 扇区。 */
#define FIRMWARE_META0_ADDR        0x000FE000UL
/** @brief 管理副本 1，占用 0x0FF000～0x0FFFFF，一个 4KB 扇区。 */
#define FIRMWARE_META1_ADDR        0x000FF000UL
/** @brief 一份管理记录独占的擦除扇区大小。 */
#define FIRMWARE_META_SECTOR_SIZE  4096U
/** @brief A 槽正文起始地址，区域 0x100000～0x1FFFFF。 */
#define FIRMWARE_SLOT_A_ADDR       0x00100000UL
/** @brief B 槽正文起始地址，区域 0x200000～0x2FFFFF。 */
#define FIRMWARE_SLOT_B_ADDR       0x00200000UL
/** @brief 每槽外部容量 1MiB；还需检查内部 App 分区容量。 */
#define FIRMWARE_SLOT_SIZE         0x00100000UL
/** @brief A/B 共两个槽。 */
#define FIRMWARE_SLOT_COUNT        2U

/** @brief 槽编号；A/B 的已确认、候选角色会交替，不能固定旧/新角色。 */
typedef enum
{
    FIRMWARE_SLOT_A    = 0,    /**< A 槽。 */
    FIRMWARE_SLOT_B    = 1,    /**< B 槽。 */
    FIRMWARE_SLOT_NONE = 0xFF  /**< 没有选定槽。 */
} firmware_slot_t;

/** @brief 管理标识，小端字节为 "FWMD"。 */
#define FIRMWARE_META_MAGIC       0x444D5746UL
/** @brief 管理记录格式版本。 */
#define FIRMWARE_META_VERSION     1UL
/** @brief 槽未保存完整且校验通过的固件。 */
#define FIRMWARE_IMAGE_INVALID    0UL
/** @brief 槽固件完整且校验通过，不代表启动已确认。 */
#define FIRMWARE_IMAGE_VALID      1UL
/** @brief 最后单独写入的提交标记；还必须检查记录 CRC。 */
#define FIRMWARE_META_COMMIT      0xA55AA55AUL
/** @brief 管理记录 CRC 覆盖序列化后的前 52 字节。 */
#define FIRMWARE_META_CRC_SIZE    52U
/** @brief 小端序列化后的完整记录长 60 字节，不能依赖 sizeof。 */
#define FIRMWARE_META_RECORD_SIZE 60U

/** @brief 持久化的安装/启动阶段，由 App 与 Bootloader 共同遵守。 */
typedef enum
{
    FIRMWARE_BOOT_NORMAL = 0, /**< 正常运行已确认固件，没有候选任务。 */
    FIRMWARE_BOOT_PENDING,    /**< 候选固件准备完成，等待安装。 */
    FIRMWARE_BOOT_INSTALLING, /**< 正在复制候选到内部 App 分区。 */
    FIRMWARE_BOOT_TRIAL,      /**< 候选正在试运行，尚未确认。 */
    FIRMWARE_BOOT_ROLLBACK    /**< 正在恢复已确认槽；候选编号为 NONE。 */
} firmware_boot_phase_t;

/** @brief 一个槽的持久化描述，INVALID 时长度和 CRC 都置零。 */
typedef struct
{
    uint32_t firmware_size;  /**< 正文实际字节数，VALID 时必须非零。 */
    uint32_t firmware_crc32; /**< 正文 CRC32，数值零也可以是合法 CRC。 */
    uint32_t valid;          /**< FIRMWARE_IMAGE_INVALID / VALID。 */
} firmware_slot_info_t;

/**
 * @brief 持久化管理记录，所有字段按小端逐个存储。
 * @details 前 52 字节内容、4 字节记录 CRC、4 字节最后提交标记。
 *          两副本轮换，保留上一份有效记录，序号按模 2^32 比较。
 * @note 持久化字段使用 uint32_t，不存储编译器决定大小的枚举。
 *       Bootloader 使用这份格式，并在启动前验证固件正文 CRC。
 */
typedef struct
{
    uint32_t magic;                       /**< FIRMWARE_META_MAGIC。 */
    uint32_t format_version;              /**< FIRMWARE_META_VERSION。 */
    uint32_t generation;                  /**< 提交序号，每次成功提交递增。 */
    firmware_slot_info_t slot[2];         /**< slot[0] 为 A，slot[1] 为 B。 */
    uint32_t confirmed_slot;              /**< 已确认槽，必须为有效 A 或 B。 */
    uint32_t candidate_slot;              /**< 候选槽或 FIRMWARE_SLOT_NONE。 */
    uint32_t boot_phase;                  /**< firmware_boot_phase_t 的值。 */
    uint32_t trial_boot_count;            /**< Bootloader 跳转候选前持久化递增。 */
    uint32_t record_crc32;                /**< 前 52 字节的 CRC32。 */
    uint32_t commit;                      /**< 最后写入 FIRMWARE_META_COMMIT。 */
} firmware_metadata_t;

/** @brief 下载会话的 RAM 状态，与持久化启动阶段分开管理。 */
typedef enum
{
    FIRMWARE_IDLE = 0,   /**< 尚未开始下载。 */
    FIRMWARE_PREPARING,  /**< 正在使目标槽无效并擦除正文。 */
    FIRMWARE_RECEIVING,  /**< 允许按顺序写入正文。 */
    FIRMWARE_VERIFYING,  /**< 正在读回校验并提交候选记录。 */
    FIRMWARE_READY,      /**< 候选已经提交，等待 Bootloader。 */
    FIRMWARE_ERROR       /**< 当前处理失败，需重试准备或清理会话。 */
} firmware_state_t;

/**
 * @brief 单次下载状态，由同一个升级任务持有并持续使用。
 * @note 初次使用必须清零，例如 static firmware_update_t update = {0};
 *       会话结束或取消前，对象地址必须保持有效，不能使用即将离开的局部对象。
 *       字段由服务维护，应用不能自行修改状态、地址或进度。
 */
typedef struct
{
    firmware_state_t state;       /**< 当前下载状态。 */
    firmware_slot_t target_slot;  /**< 本次下载到 A 或 B，自动选择未确认槽。 */
    uint32_t storage_addr;        /**< 目标正文起始地址，不由远端指定。 */
    uint32_t total_size;          /**< 本次正文总字节数。 */
    uint32_t received_size;       /**< 已整批写入成功的字节数。 */
    uint32_t expected_crc32;      /**< 从包头得到的正文预期 CRC32。 */
    uint32_t metadata_generation; /**< 准备成功时的管理序号，拒绝过期会话。 */
} firmware_update_t;

/**
 * @brief 注册当前实际运行 App 的内部 Flash 映像及分区容量。
 * @param[in] app_start 当前 App .bin 在内部 Flash 中的起始地址；
 *            从这里开始的 app_capacity 字节必须都可读取。
 * @param[in] app_capacity 内部 App 分区容量，必须非零且不超过外部单槽容量。
 * @retval 1 绑定成功，无活跃会话时允许重复绑定。
 * @retval 0 参数无效或存在尚未结束的下载会话。
 * @note 这是可读映像地址，不是函数指针、栈缓冲或外部候选槽地址。
 *       根据实际链接布局传入，本模块不猜测 Bootloader 占用大小。
 */
uint8_t Firmware_SetAppRegion(const uint8_t *app_start,
                              uint32_t app_capacity);

/**
 * @brief 手动建立首次回滚基线：将当前运行固件复制到外部 A 槽。
 * @param[in] firmware_size 当前运行 App 的准确 .bin 字节数，包含全部加载数据；
 *            不是内部 App 容量，也不能使用 sizeof 指针。
 * @retval 1 A 槽复制/读回校验成功，已建立 NORMAL、confirmed=A 的管理记录。
 * @retval 0 参数错误、未注册 App、已有有效记录、读写失败或 CRC 不匹配。
 * @pre 两个管理副本均可读取且没有任何有效管理记录。
 * @note 只供首次明确初始化，不要放进每次启动自动执行。
 *       会擦除 A 槽正文需要的扇区，不修改 B 正文；断电后可以重新执行。
 *       若存在有效管理记录，即使另一副本坏了，也拒绝覆盖基线。
 */
uint8_t Firmware_CreateBaseline(uint32_t firmware_size);

/**
 * @brief 只读取得两个管理副本中较新的有效记录。
 * @param[out] metadata 成功时写入完整记录，失败时保持原值。
 * @retval 1 两副本读取成功，且找到格式、CRC、提交标记均有效的记录。
 * @retval 0 无有效记录、参数无效、任一读取失败或序号关系有歧义。
 * @note 诊断接口不会擦除或写入 Flash，也不计算槽正文 CRC。
 */
uint8_t Firmware_ReadMetadata(firmware_metadata_t *metadata);

/**
 * @brief 检查已确认固件，自动选择另一槽并准备一次下载。
 * @param[in,out] update 已初始化且持续有效的会话对象，成功后进度清零。
 * @param[in] firmware_size 新固件正文总字节数，不含 16 字节 TCP 包头。
 * @param[in] firmware_crc32 新固件正文的预期 CRC-32/ISO-HDLC。
 * @retval 1 目标槽已准备完成，state 为 FIRMWARE_RECEIVING。
 * @retval 0 参数、阶段、当前映像不匹配或 Flash 操作失败。
 * @pre Flash 驱动和 App 区域已注册，首次回滚基线已经建立。
 * @note 要求 NORMAL 且无候选任务，外部已确认固件和当前 App 的 CRC 均匹配。
 *       先提交目标槽无效记录，再擦除目标正文，保留已确认槽。
 *       Flash 处理失败进入 ERROR；同一 ERROR 对象可以重新 Begin。
 *       参数/状态检查失败不改变当前会话。不要在 TCP 回调中执行长时间擦除。
 */
uint8_t Firmware_Begin(firmware_update_t *update,
                       uint32_t firmware_size,
                       uint32_t firmware_crc32);

/**
 * @brief 将一批正文按顺序写入目标槽，不自动擦除。
 * @param[in,out] update 当前活跃会话，整批成功后推进 received_size。
 * @param[in] data 本批正文的起始地址，至少有 length 字节；返回前不能被覆盖。
 * @param[in] length 本批实际字节数，必须非零且不超过剩余正文长度。
 * @retval 1 本批写入完成，已更新进度。
 * @retval 0 参数、状态、范围、管理序号错误或 Flash 写入失败。
 * @pre Begin 成功，state 为 RECEIVING。
 * @note 检查管理序号以拒绝过期会话。写入失败可能已改变部分 Flash，进入 ERROR。
 *       收齐数据仍需 Finish，不能直接将包视为有效。
 */
uint8_t Firmware_Write(firmware_update_t *update,
                       const uint8_t *data,
                       uint32_t length);

/**
 * @brief 读回校验完整正文，并事务提交候选安装记录。
 * @param[in,out] update 当前活跃且正文已收齐的会话。
 * @retval 1 CRC 匹配并提交成功，RAM 为 READY，持久化阶段为 PENDING。
 * @retval 0 长度不足、状态/序号错误、CRC 不匹配或 Flash 操作失败。
 * @note 始终保留原 confirmed_slot，不在下载完成时确认新固件。
 *       写提交标记后读回失败时，记录可能已经有效；必须以再次读取的记录为准，
 *       不能盲目继续写或取消候选。Bootloader 安装逻辑需要另外实现。
 */
uint8_t Firmware_Finish(firmware_update_t *update);

/**
 * @brief 清理尚未发布候选的 RECEIVING/ERROR 下载会话。
 * @param[in,out] update 当前活跃会话，成功后回到 IDLE、清零进度和目标。
 * @retval 1 管理记录仍为 NORMAL、无候选且目标槽无效，会话已清理。
 * @retval 0 参数/状态错误、读取失败或记录表明候选已经发布。
 * @note 不执行擦除，不修改已确认槽；PENDING/TRIAL 等阶段不能由此取消。
 *       若 Begin 在目标无效记录提交前失败导致拒绝 Abort，可对同一对象重试 Begin。
 */
uint8_t Firmware_Abort(firmware_update_t *update);

/**
 * @brief 当前新 App 完成关键初始化、自检后确认试运行成功。
 * @retval 1 候选转为已确认槽，阶段恢复 NORMAL，保留另一槽内容。
 * @retval 0 无 TRIAL 候选、实际 App/候选 CRC 不匹配、活跃下载或提交失败。
 * @pre Bootloader 已安装候选、更新 TRIAL 记录并启动候选 App。
 * @note 使用注册 App 可读区域校验运行映像；不能仅凭 TRIAL 状态就确认。
 *       不要刚进入 main 就确认，应在关键初始化和必要自检通过后调用。
 */
uint8_t Firmware_Confirm(void);

#endif /* FIRMWARE_UPDATE_H */
