#pragma once
#include <Arduino.h>

/* 药品记忆 —— 存在 NVS 里, 断网也能查。
 *
 * NVS 硬约束: 分区只有 20KB, 且 putString 单值上限 4000 字节,
 * 而 WiFi 凭据和 PHY 校准数据也住在同一个分区。所以这里要省着用。
 *
 * 存储格式刻意不用 JSON —— 括号引号要吃掉近三成空间。用分隔符:
 *     阿莫西林|2|morning,evening|1758942600;
 *     布洛芬|3|morning,noon,evening|1758942700;
 * 每条约 40 字节, 4000 字节能放近百条, 家庭用药绰绰有余。 */

#define MED_MAX_ITEMS  40           // 主动限制条数, 给 WiFi 数据留余量
#define MED_NAME_LEN   24           // 药名(UTF-8): 中文药名一般 4~8 字 = 12~24 字节
#define MED_SLOT_LEN   24           // 时段串

struct MedItem {
    char     name[MED_NAME_LEN];
    uint8_t  perDay;                // 每天几次
    char     slots[MED_SLOT_LEN];   // 时段, 如 "morning,evening" (仅作展示)
    uint32_t lastTaken;             // 上次服用 unix 时间戳; 0 = 还没记录过
};

/* ---------- 基础读写 ---------- */

/* 读出全部记录, 返回条数 (最多 max 条) */
int  medLoad(MedItem *items, int max);

/* 按名字查。用「互相包含」匹配而不是全等 ——
 * 模型可能认出「阿莫西林胶囊」而记录里存的是「阿莫西林」。
 * 返回下标, 没找到返回 -1。 */
int  medFind(const char *name);

/* 新增一条; 名字已存在则只更新规则, 不动服用记录 */
bool medAdd(const char *name, uint8_t perDay, const char *slots);

/* 记录一次服用 = 把 lastTaken 置为当前时间。返回 false 表示没这条药 */
bool medMarkTaken(const char *name);

/* 删掉一条 */
bool medRemove(const char *name);

/* 清空全部记录 (网页上「忘记所有药」用) */
void medClear();

/* ---------- 时间与建议 ---------- */

/* 系统时间是否已经同步过 (NTP 成功) */
bool medTimeReady();

/* 当前时间戳; 未同步时返回 0 */
uint32_t medNow();

/* 格式化时间戳为 "08:30"; 传 0 返回 "从未" */
String medFmtTime(uint32_t ts);

/* 核心: 现在该不该吃。
 * 返回一句可直接打印的中文说明 —— 这条路径全在本地算, 不调任何 API。 */
String medAdvice(const char *name);

/* ---------- 自然语言输入 ---------- */

/* 处理一句人话, 返回可直接显示的中文反馈。
 *
 * 麦克风到货前由网页的文本框调用; 到货后 ASR 的转写结果也走这里 ——
 * 两条路共用同一条处理链路, 所以麦克风接上时不用改后面的逻辑。
 *
 * 能认三种说法:
 *   "阿莫西林吃了"     -> 记一次服用
 *   "阿莫西林一天两次" -> 记用药方案
 *   "阿莫西林"         -> 查该不该吃
 * 药名必须已经在记录里(拍照识别过或手动加过), 否则提示先去添加。 */
String medHandleUtterance(const char *text);
