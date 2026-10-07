#pragma once
#include <Arduino.h>
#include <string>

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
/* 药名(UTF-8)。⚠️ 原来写 24, 但**正好装不下这个用户自己那盒药**:
   「苯磺酸氨氯地平片」= 8 个汉字 = 24 字节, 而 utf8SafeCopy 要在末尾留 '\0',
   于是被按字符边界截成「苯磺酸氨氯地」。常见的长药名还有「琥珀酸美托洛尔缓释片」(10 字)
   和「盐酸二甲双胍缓释片」(9 字)。给到 32 = 10 个汉字, 够用。 */
#define MED_NAME_LEN   32
#define MED_SLOT_LEN   24           // 时段串

struct MedItem {
    char     name[MED_NAME_LEN];
    uint8_t  perDay;                // 每天几次
    char     slots[MED_SLOT_LEN];   // 时段, 如 "morning,evening" (仅作展示)
    uint32_t lastTaken;             // 上次服用 unix 时间戳; 0 = 还没记录过

    /* ---- 24 小时计数窗口 (2026-10-07 加) ----
     * 为什么需要它: 只看 lastTaken 的间隔, **一天里均匀地吃 5 次也全都通过**
     * (每次间隔都够) —— 管不住"总量超量"。所以另开一个 24 小时窗口单独计数。
     *
     * winStart = 窗口起点(第一笔的时间戳); 满 24 小时自动开新窗口、次数归零。
     * ⚠️ 是个"锚定的 24 小时"而不是"滚动的 24 小时": 窗口一开就走 24 小时,
     *    跨窗口的两笔不会互相计数。想改成滚动窗口得存每笔的时间戳(NVS 放不下),
     *    或者改成按自然日(那要处理时区)。 */
    uint32_t winStart;              // 计数窗口起点; 0 = 窗口还没开
    uint8_t  cnt24;                 // 窗口内已记录几次
};

/* ---------- 基础读写 ---------- */

/* setup() 里调一次, 建内部那把互斥锁。
 *
 * 语音 task 和 loopTask 都会读写 med 模块里共用的那块静态缓冲(s_blob) ——
 * 不加锁的话, 一边在拼表、另一边把缓冲覆盖掉, 写回 NVS 的就是半张表。
 * ⚠️ 锁必须在这里显式创建, 不能懒初始化(两个 task 各自建一把 = 等于没锁)。
 * 没调它也能跑, 只是退回"无锁"状态。 */
void medBegin();

/* 读出全部记录, 返回条数 (最多 max 条) */
int  medLoad(MedItem *items, int max);

/* 按名字查。用「互相包含」匹配而不是全等 ——
 * 模型可能认出「阿莫西林胶囊」而记录里存的是「阿莫西林」。
 * 返回下标, 没找到返回 -1。 */
int  medFind(const char *name);

/* 新增一条; 名字已存在则只更新规则, 不动服用记录 */
bool medAdd(const char *name, uint8_t perDay, const char *slots);

/* 记录一次服用 = 打上当前网络时间戳 + 24 小时窗口计数 +1。
 * outCount 非空时回填窗口内的次数(给网页/语音回话用: "24 小时内第 3 次")。
 * 返回 false = 没这条药, 或者设备时间未同步(那种情况**拒绝记录**, 见实现)。 */
bool medMarkTaken(const char *name, int *outCount = nullptr);

/* 24 小时窗口内的次数; 窗口还没开或已经过期都返回 0。 */
int  medCount24(const MedItem &it);

/* 一次问出某味药的方案次数和窗口内次数(网页上判断"超量"用)。
 * 找不到这条药返回 false。 */
bool medUsage(const char *name, int *perDay, int *count24);

/* ---------- 演示用 ----------
 * 把所有记录的时间戳**整体往前挪** hours 小时 = 让设备以为过了这么久。
 *
 * 现场演示用: 「该吃了」「24 小时窗口过期」这些本来要等几小时的状态, 点一下就出来。
 * 所有判断用的都是时间**差值**, 所以"把过去挪远"和"把现在推后"效果完全一样。
 *
 * ⚠️ 特意不碰系统时钟(那会影响 NTP 对时、TLS 证书校验), 也特意**往回挪**而不是
 *    往前——写进未来的时间戳会在演示结束后把药"锁"住(一直显示"刚吃过"), 而
 *    往回挪只留下一句无害的谎话, 下次真实记录就覆盖了。
 * ⚠️ 它会改 NVS 里的真实记录。演示完想交作品, 把 webui.h 的 DEMO_HELPERS 改 0。
 * 返回被影响的记录条数。 */
int medDemoBackdate(uint32_t hours);

/* 把计数清零 —— 网页上「清空次数」按钮走这里(记错了/重复点了)。
 * ⚠️ 只清 cnt24, **不动 winStart, 也不动 lastTaken**:
 *    winStart 留着 = "时间计时不变", 窗口还按原来的节奏走;
 *    lastTaken 留着 = 「该不该吃」的间隔判断完全不受影响。 */
bool medResetCount(const char *name);

/* 删掉一条 */
bool medRemove(const char *name);

/* 清空全部记录 (网页上「忘记所有药」用) */
void medClear();

/* ---------- 常见药名(内置表) ----------
 * **一处定义, 两处用**:
 *   1) ASR 热词表(voice.cpp) —— 提高药名的识别率
 *   2) 判断"听出来的这个是不是药名"(med.cpp 的提醒逻辑) —— 用来区分
 *      「真没听清」和「听清了、但那不是你的药」(后者要提醒吃错药)
 * 返回数组首地址, *count 填个数。 */
const char *const *medCommonDrugs(int *count);

/* ---------- 时间与建议 ---------- */

/* 系统时间是否已经同步过 (NTP 成功) */
bool medTimeReady();

/* 当前时间戳; 未同步时返回 0 */
uint32_t medNow();

/* 格式化时间戳为 "08:30"; 传 0 返回 "从未" */
std::string medFmtTime(uint32_t ts);

/* 核心: 现在该不该吃。
 * 返回一句可直接打印的中文说明 —— 这条路径全在本地算, 不调任何 API。 */
std::string medAdvice(const char *name);

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
std::string medHandleUtterance(const char *text);
