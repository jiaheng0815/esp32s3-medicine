#include "logcfg.h"   /* 必须第一行: 日志配置要在 <Arduino.h> 之前生效 */
#include "strutil.h"  // std::string 桥接

static const char *TAG = "med";    // 日志标签

#include "med.h"

#include <Preferences.h>
#include <time.h>

#define MED_NS    "med"
#define MED_KEY   "tbl"

/* NVS putString 上限 4000 字节, 留点余量 */
#define MED_BLOB_MAX  3900

/* 24 小时计数窗口的长度。⚠️ 是"锚定"的: 从这一轮第一笔开始算满 24 小时,
   不是滚动的 24 小时 —— 见 med.h 里 MedItem 那段说明。 */
#define MED_WINDOW_SEC  (24UL * 60 * 60)

/* 整张表在这个静态缓冲里读写过一次。放静态区而不是栈上 ——
   3900 字节的栈数组会直接把 loopTask 的栈顶穿 */
static char s_blob[MED_BLOB_MAX];

/* ==================== 互斥锁 ====================
 * s_blob 是**所有函数共用**的一块静态缓冲: medLoad 从它解析, saveItems 往里拼。
 * 而两个 task 都会调进来 —— 语音 task(voiceTask, 优先级 3) 走 medHandleUtterance,
 * loopTask 走网页请求和拍照路径。一边在 saveItems 拼表、另一边 medLoad 把 s_blob
 * 覆盖掉, 存回 NVS 的就是半张表(读-改-写之间被人插队 = 丢失更新)。
 *
 * ⚠️ 必须是**递归**锁: medHandleUtterance 内部还要调 medAdvice / medMarkTaken,
 *    普通互斥量在这里直接自锁死。
 * ⚠️ 锁由 medBegin() 显式创建, 不在这里懒初始化 —— 两个 task 各自建一把 = 等于没锁。
 *    这和 main.cpp 那把 TFT 锁是同一个教训(见 CLAUDE.md 坑 #13)。
 *
 * 等锁用 portMAX_DELAY 而不是像 TFT 锁那样带超时: 这里超时会导致**带着未同步的
 * 数据继续跑**(写坏 NVS), 比等一会儿糟得多。临界区很短, 也不会阻塞在 I/O 上。 */
static SemaphoreHandle_t s_medMux = nullptr;

void medBegin()
{
    if (!s_medMux) s_medMux = xSemaphoreCreateRecursiveMutex();
}

/* RAII: 保证每条 return 路径(尤其早退)都会解锁, 不用手写一堆 unlock */
struct MedGuard {
    MedGuard()  { if (s_medMux) xSemaphoreTakeRecursive(s_medMux, portMAX_DELAY); }
    ~MedGuard() { if (s_medMux) xSemaphoreGiveRecursive(s_medMux); }
};

/* ==================== 底层读写 ==================== */
static size_t loadBlob()
{
    Preferences p;
    /* 只读模式打开一个还不存在的 namespace 会返回 false 并打一条 NOT_FOUND 错误。
       那不是故障 —— 只是还没存过任何药, 当成空表就是了。
       写路径(saveBlob)用的是读写模式, 会顺手把 namespace 建出来。 */
    if (!p.begin(MED_NS, true)) {
        s_blob[0] = '\0';
        return 0;
    }

    /* ⚠️ 先 isKey 再 getString。直接 getString 一个不存在的 key, Preferences
       会打一条 **ERROR 级别**的 "nvs_get_str len fail: tbl NOT_FOUND" ——
       串口上看着像出了故障, 其实只是"还没存过药"。这条噪音会把人带偏。 */
    if (!p.isKey(MED_KEY)) {
        p.end();
        s_blob[0] = '\0';
        return 0;
    }

    const std::string s = toStd(p.getString(MED_KEY, ""));   // NVS 返回 Arduino String, 过桥
    p.end();

    const size_t n = s.length();
    if (n == 0 || n >= sizeof(s_blob)) {
        s_blob[0] = '\0';
        return 0;
    }
    memcpy(s_blob, s.c_str(), n);
    s_blob[n] = '\0';
    return n;
}

static bool saveBlob()
{
    Preferences p;
    if (!p.begin(MED_NS, false)) return false;

    /* ⚠️ 空表要走 remove, 不能 putString(""): 空串的 putString 返回 0,
       会被下面那句 `> 0` 当成写失败 —— 删掉最后一条药时会表现为
       "明明删掉了, 却回你一句『没有这条记录』"。 */
    bool ok;
    if (s_blob[0] == '\0') {
        ok = p.remove(MED_KEY);
    } else {
        ok = p.putString(MED_KEY, s_blob) > 0;
    }
    p.end();
    return ok;
}

/* 解析一条 "名称|次数|时段|时间戳[|窗口起点|窗口内次数]"
 *
 * ⚠️ 最后两段(2026-10-07 加的 24 小时计数)**必须是可选的**:
 *    NVS 里存着的是老格式的 4 段记录, 少两段就解析失败 = 用户存的药全没了。
 *    这里 p4/p5 找不到就用 0(窗口没开), 新写的记录才带上它们。 */
static bool parseRec(const char *rec, size_t len, MedItem *it)
{
    /* 手动找三个分隔符, 避免构造临时 std::string */
    const char *p1 = (const char *)memchr(rec, '|', len);
    if (!p1) return false;
    const char *p2 = (const char *)memchr(p1 + 1, '|', len - (p1 + 1 - rec));
    if (!p2) return false;
    const char *p3 = (const char *)memchr(p2 + 1, '|', len - (p2 + 1 - rec));
    if (!p3) return false;

    const size_t nameLen = p1 - rec;
    if (nameLen == 0 || nameLen >= MED_NAME_LEN) return false;

    memset(it, 0, sizeof(*it));
    memcpy(it->name, rec, nameLen);
    it->name[nameLen] = '\0';

    it->perDay = (uint8_t)atoi(p1 + 1);

    const size_t slotLen = p3 - p2 - 1;
    if (slotLen < MED_SLOT_LEN) {
        memcpy(it->slots, p2 + 1, slotLen);
        it->slots[slotLen] = '\0';
    }

    it->lastTaken = (uint32_t)strtoul(p3 + 1, nullptr, 10);

    /* 可选的两段 */
    const char *p4 = (const char *)memchr(p3 + 1, '|', len - (p3 + 1 - rec));
    if (p4) {
        it->winStart = (uint32_t)strtoul(p4 + 1, nullptr, 10);
        const char *p5 = (const char *)memchr(p4 + 1, '|', len - (p4 + 1 - rec));
        if (p5) it->cnt24 = (uint8_t)atoi(p5 + 1);
    }
    return true;
}

/* 把整表序列化回 s_blob 并落盘 */
static bool saveItems(const MedItem *items, int n)
{
    size_t len = 0;

    for (int i = 0; i < n; i++) {
        char line[96];
        const int m = snprintf(line, sizeof(line), "%s|%u|%s|%lu|%lu|%u;",
                               items[i].name,
                               (unsigned)items[i].perDay,
                               items[i].slots,
                               (unsigned long)items[i].lastTaken,
                               (unsigned long)items[i].winStart,
                               (unsigned)items[i].cnt24);
        if (m <= 0 || len + (size_t)m >= sizeof(s_blob)) {
            ESP_LOGI(TAG, "[MED] 记录超出 NVS 单值上限, 后面的被丢弃");
            break;
        }
        memcpy(s_blob + len, line, m);
        len += m;
    }
    s_blob[len] = '\0';
    return saveBlob();
}

/* 名字匹配: 互相包含即认为是同一种药。
 * 模型可能认出「阿莫西林胶囊」, 而记录里存的是「阿莫西林」。 */
static bool nameMatch(const char *a, const char *b)
{
    return a && b && *a && *b && (strstr(a, b) || strstr(b, a));
}

/* ==================== 常见药名(内置表) ==================== */
static const char *const kCommonDrugs[] = {
    "阿莫西林", "头孢", "阿奇霉素", "左氧氟沙星", "布洛芬", "对乙酰氨基酚",
    "阿司匹林", "二甲双胍", "格列美脲", "阿托伐他汀",
    "苯磺酸氨氯地平", "氨氯地平", "缬沙坦", "美托洛尔", "硝苯地平",
    "奥美拉唑", "蒙脱石散", "氯雷他定", "感冒灵", "维生素", "钙片",
};

const char *const *medCommonDrugs(int *count)
{
    if (count) *count = (int)(sizeof(kCommonDrugs) / sizeof(kCommonDrugs[0]));
    return kCommonDrugs;
}

/* 药名消毒: 存储格式用 '|' 和 ';' 分隔, 名字里混进这两个字符会把整张表解析乱掉
   —— 模型输出和网页输入都可能带进来(模型爱写标点)。顺带去掉换行/制表符。 */
static void sanitizeName(char *dst, size_t cap, const char *src)
{
    size_t o = 0;
    for (const char *p = src; p && *p && o + 1 < cap; p++) {
        if (*p == '|' || *p == ';' || *p == '\n' || *p == '\r' || *p == '\t') continue;
        dst[o++] = *p;
    }
    dst[o] = '\0';
}

/* 按 UTF-8 边界安全截断。
 * 直接 strncpy(dst, src, 23) 会把一个 3 字节的汉字切成两半 —— 网页上显示乱码,
 * 而且这半截字符写回 NVS 之后会雪球越滚越大。所以往回退到字符起始字节。 */
static void utf8SafeCopy(char *dst, size_t cap, const char *src)
{
    size_t n = src ? strlen(src) : 0;
    if (n >= cap) {
        n = cap - 1;
        /* 0x80~0xBF 是 UTF-8 的续字节, 退到不是续字节的位置就是字符边界 */
        while (n > 0 && ((unsigned char)src[n] & 0xC0) == 0x80) n--;
    }
    if (n) memcpy(dst, src, n);
    dst[n] = '\0';
}

/* ==================== 公开接口 ==================== */
int medLoad(MedItem *items, int max)
{
    MedGuard g;
    if (loadBlob() == 0) return 0;

    int n = 0;
    const char *p   = s_blob;
    const char *end = s_blob + strlen(s_blob);

    while (n < max && p < end) {
        const char *semi = (const char *)memchr(p, ';', end - p);
        if (!semi) break;
        if (parseRec(p, semi - p, &items[n])) n++;
        p = semi + 1;
    }
    return n;
}

int medFind(const char *name)
{
    MedGuard g;
    if (!name || !*name) return -1;

    MedItem items[MED_MAX_ITEMS];
    const int n = medLoad(items, MED_MAX_ITEMS);
    for (int i = 0; i < n; i++) {
        if (nameMatch(items[i].name, name)) return i;
    }
    return -1;
}

/* slots 传 nullptr = 保持原样。语音说「一天两次」时句子里根本没有时段信息,
   传 "" 会把之前存好的时段串抹掉(网页表单留空也是同一类事情)。 */
bool medAdd(const char *name, uint8_t perDay, const char *slots)
{
    MedGuard g;
    if (!name || !*name) return false;

    char clean[MED_NAME_LEN * 2];               // 先粗筛, 后面 utf8SafeCopy 再按字符边界截
    sanitizeName(clean, sizeof(clean), name);
    if (!clean[0] || strlen(clean) >= MED_NAME_LEN) return false;

    MedItem items[MED_MAX_ITEMS];
    const int n = medLoad(items, MED_MAX_ITEMS);

    /* 已存在 → 只更新方案, 不动服用记录 */
    for (int i = 0; i < n; i++) {
        if (nameMatch(items[i].name, clean)) {
            items[i].perDay = perDay;
            if (slots) utf8SafeCopy(items[i].slots, MED_SLOT_LEN, slots);
            return saveItems(items, n);
        }
    }

    if (n >= MED_MAX_ITEMS) {
        ESP_LOGI(TAG, "[MED] 药品表已满 (%d 条), 没记下新的", n);
        return false;
    }

    memset(&items[n], 0, sizeof(MedItem));
    utf8SafeCopy(items[n].name, MED_NAME_LEN, clean);
    items[n].perDay = perDay;
    if (slots) utf8SafeCopy(items[n].slots, MED_SLOT_LEN, slots);
    items[n].lastTaken = 0;

    ESP_LOGI(TAG, "[MED] 新增: %s (一天 %u 次)", items[n].name, (unsigned)perDay);
    return saveItems(items, n + 1);
}

/* ==================== 药名纠错 ==================== */

/* 药名纠错表。⚠️ 加条目必须从**实测**总结(先在 PC 上用实拍图复现确认, 见坑 #19),
 * 凭想象加映射会凭空造出新的错药名。
 *
 * 条目来源(都是实测):
 *   · 拍照: glm 在图像不佳时把「盐酸氯环利嗪片」读成「盐酸氟环利嗪片」(坑 #19)
 *   · 语音: ASR 对「嗪」这个字几乎必错 —— 转成「睛」(2026-10-11 实测)
 *     或「晴」(坑 #18 的「酸氯环利晴片」)
 *
 * ⚠️ 氯/氟、睛/晴 在别的药名里**都是真实存在的字**(氯雷他定 vs 氟康唑是两个
 *    不同的药), 所以**绝不能做全局单字替换**, 只能按实测错型整片段纠正。
 * ⚠️ 两个入口都会调它: 拍照(runCaptureAsk)和语音(剂量句剥名后)。 */
static const struct { const char *from; const char *to; } kNameFix[] = {
    { "氟环利嗪", "氯环利嗪" },
    { "氯环利睛", "氯环利嗪" },
    { "氯环利晴", "氯环利嗪" },
};

std::string medFixDrugName(const std::string &s)
{
    std::string out = s;
    for (size_t i = 0; i < sizeof(kNameFix) / sizeof(kNameFix[0]); i++) {
        const size_t fl = strlen(kNameFix[i].from);
        size_t p;
        while ((p = out.find(kNameFix[i].from)) != std::string::npos) {
            out.replace(p, fl, kNameFix[i].to);
        }
    }
    return out;
}

/* 24 小时窗口内的次数; 窗口没开、时间不可用、或窗口已过期都算 0 */
int medCount24(const MedItem &it)
{
    const uint32_t now = medNow();
    if (it.winStart == 0 || now == 0 || now < it.winStart) return 0;
    if (now - it.winStart >= MED_WINDOW_SEC) return 0;
    return it.cnt24;
}

/* 记一笔: 推进 24 小时窗口 + 更新 lastTaken。返回窗口内的新次数。 */
static int countDose(MedItem &it, uint32_t now)
{
    /* 窗口没开 / 已经过期 / 时钟被回拨 -> 开一个新窗口, 次数从 0 开始 */
    if (it.winStart == 0 || now < it.winStart ||
        (now - it.winStart) >= MED_WINDOW_SEC) {
        it.winStart = now;
        it.cnt24    = 0;
    }
    if (it.cnt24 < 255) it.cnt24++;      // uint8 封顶, 免得回绕成 0
    it.lastTaken = now;
    return it.cnt24;
}

bool medMarkTaken(const char *name, int *outCount)
{
    MedGuard g;
    if (!name || !*name) return false;

    MedItem items[MED_MAX_ITEMS];
    const int n = medLoad(items, MED_MAX_ITEMS);

    for (int i = 0; i < n; i++) {
        if (nameMatch(items[i].name, name)) {
            /* ⚠️ 时间没同步时 medNow() 是 0, 而 0 在存储里表示「从未服用」——
               照记会把上一次**有效**的时间戳直接冲掉, 之后所有间隔计算全错。
               宁可拒绝, 也不能把"记过一次"变成"从没记过"。 */
            const uint32_t now = medNow();
            if (now == 0) {
                ESP_LOGW(TAG, "[MED] 时间未同步, 拒绝记录服用: %s", name);
                return false;
            }
            const int c = countDose(items[i], now);
            if (outCount) *outCount = c;
            ESP_LOGI(TAG, "[MED] %s 记一次: 24 小时内第 %d 次 (方案一天 %u 次)",
                          items[i].name, c, (unsigned)items[i].perDay);
            return saveItems(items, n);
        }
    }
    return false;
}

bool medUsage(const char *name, int *perDay, int *count24)
{
    MedGuard g;
    if (!name || !*name) return false;

    MedItem items[MED_MAX_ITEMS];
    const int n = medLoad(items, MED_MAX_ITEMS);

    for (int i = 0; i < n; i++) {
        if (nameMatch(items[i].name, name)) {
            if (perDay)  *perDay  = items[i].perDay;
            if (count24) *count24 = medCount24(items[i]);
            return true;
        }
    }
    return false;
}

int medDemoBackdate(uint32_t hours)
{
    MedGuard g;
    if (hours == 0) return 0;

    const uint32_t sec = hours * 3600UL;

    MedItem items[MED_MAX_ITEMS];
    const int n = medLoad(items, MED_MAX_ITEMS);
    if (n == 0) return 0;

    int touched = 0;
    for (int i = 0; i < n; i++) {
        bool ch = false;
        /* 用 > 而不是 >= : 免得减成负数/下溢 */
        if (items[i].lastTaken > sec) { items[i].lastTaken -= sec; ch = true; }
        if (items[i].winStart  > sec) { items[i].winStart  -= sec; ch = true; }
        if (ch) touched++;
    }

    if (touched) saveItems(items, n);
    ESP_LOGW(TAG, "[MED] 演示快进 %u 小时: 影响 %d 条 (只挪时间戳, 不动系统时钟)",
                  (unsigned)hours, touched);
    return touched;
}

bool medResetCount(const char *name)
{
    MedGuard g;
    if (!name || !*name) return false;

    MedItem items[MED_MAX_ITEMS];
    const int n = medLoad(items, MED_MAX_ITEMS);

    for (int i = 0; i < n; i++) {
        if (nameMatch(items[i].name, name)) {
            /* 只清 cnt24。winStart 和 lastTaken 都**不动**:
               winStart 留着 -> "时间计时不变", 窗口还按原来的节奏走(到点自然归零);
               lastTaken 留着 -> 「该不该吃」的间隔判断完全不受影响 */
            items[i].cnt24 = 0;
            ESP_LOGI(TAG, "[MED] 次数清零: %s (窗口起点保持不变)", items[i].name);
            return saveItems(items, n);
        }
    }
    return false;
}

bool medRemove(const char *name)
{
    MedGuard g;
    if (!name || !*name) return false;

    MedItem items[MED_MAX_ITEMS];
    const int n = medLoad(items, MED_MAX_ITEMS);

    for (int i = 0; i < n; i++) {
        if (nameMatch(items[i].name, name)) {
            for (int j = i; j < n - 1; j++) items[j] = items[j + 1];   // 后面的往前挪
            return saveItems(items, n - 1);
        }
    }
    return false;
}

void medClear()
{
    MedGuard g;
    Preferences p;
    if (p.begin(MED_NS, false)) {
        p.remove(MED_KEY);
        p.end();
    }
}

/* ==================== 时间 ==================== */
uint32_t medNow()
{
    const time_t t = time(nullptr);
    /* 2023-11-14 之前的时间戳一律当作「还没同步」——
       ESP32 上电后默认从 1970 开始走 */
    return (t > 1700000000) ? (uint32_t)t : 0;
}

bool medTimeReady()
{
    return medNow() != 0;
}

std::string medFmtTime(uint32_t ts)
{
    if (ts == 0) return "从未";

    const time_t t = (time_t)ts;
    struct tm tm;
    localtime_r(&t, &tm);

    char buf[16];
    snprintf(buf, sizeof(buf), "%02d:%02d", tm.tm_hour, tm.tm_min);
    return std::string(buf);
}

/* ==================== 自然语言输入 ====================
 * 麦克风到货前由网页文本框调用; 到货后 ASR 转写结果也走这里。
 * 只做极简规则匹配, 不调模型 —— 输入本来就短, 而且这几类说法很固定。
 * (复杂说法比如「隔天一次」「饭前半小时」交给模型解析, 那是阶段 3 的事) */

/* 从 "一天两次" / "每天2次" / "一日三次" 里抠出次数 */
static int parsePerDay(const std::string &s)
{
    /* ⚠️ 中文数字和数值**不是** index+1 的关系 —— 「两」和「二」都表示 2。
     * 这里以前写成 {"一","二","两","三",...} 再 `return k + 1`, 于是:
     *   「两次」-> 3、「三次」-> 4、「四次」-> 5 ...整体错位,
     *   而「六」因为循环写的是 k < 6 根本匹配不到。
     * 更阴的是它**不报错**: 用药方案被静默记错, 后面所有"该不该吃"的判断
     * 全跟着错, 而热词表和网页占位符里最常用的恰好就是「一天两次」。
     * 所以数值必须显式写出来, 别再靠下标推。 */
    static const struct { const char *w; int v; } cn[] = {
        { "一", 1 }, { "二", 2 }, { "两", 2 },
        { "三", 3 }, { "四", 4 }, { "五", 5 }, { "六", 6 },
    };
    const int cnN = (int)(sizeof(cn) / sizeof(cn[0]));

    /* 必须用双引号! 「次」在 UTF-8 里是 3 个字节(E6 AC A1),
       写成单引号 '次' 会被当成 multi-character constant, 编译期截断成一个字节
       (0xA1), 于是这里实际上在找一个字节 0xA1 —— 永远找不到, 整个函数失效。 */
    const int ci = strIndex(s, "次");
    if (ci <= 0) return 0;

    /* 从"次"字往前找最近的数字, 最多退 4 个字节(中文数字占 3 字节) */
    for (int back = 1; back <= 4 && ci - back >= 0; back++) {
        const char c = s[ci - back];
        if (c >= '0' && c <= '9') {
            /* ⚠️ 可能是多位数: 「一天10次」只看最后一位会得到 0,
               再往前退一位又变成 1 —— 两位都读, 合起来才是 10。
               范围跟网页表单一致取 1~10, 超出的当没听懂。 */
            int v = c - '0';
            if (ci - back - 1 >= 0 && s[ci - back - 1] >= '1' && s[ci - back - 1] <= '9') {
                v = (s[ci - back - 1] - '0') * 10 + v;
            }
            return (v >= 1 && v <= 10) ? v : 0;
        }

        for (int k = 0; k < cnN; k++) {
            if (strStartsWith(strSub(s, ci - back), cn[k].w)) return cn[k].v;
        }
    }
    return 0;
}

/* 该不该吃 —— 纯计算, 只读一条记录。
 * 单独抽出来是因为**两个入口都要用**: medAdvice (按名字查) 和
 * medHandleUtterance (表已经在手上了, 不该再加载一遍 —— 每加载一次就是
 * 2240 字节的栈数组, 见下面那段). */
static std::string adviceFor(const MedItem &it)
{
    if (it.perDay == 0) {
        /* ⚠️ 引导语必须**带上药名**: 拍照不再写表后, 不带药名的「一天两次」
           没有可挂靠的目标(老 pending 机制已移除), 说了也录不进去 */
        return std::string(it.name) + ": 还没录入用药方案。按住按钮说「" +
               it.name + " 一天两次」，就能录。";
    }
    if (it.lastTaken == 0) {
        return std::string(it.name) + ": 按方案一天 " + std::to_string(it.perDay) +
               " 次, 但还没有服用记录。";
    }

    const uint32_t now = medNow();
    if (now == 0) {
        return std::string(it.name) + ": 上次服用 " + medFmtTime(it.lastTaken) +
               "。(设备时间未同步, 这次只记录不判断)";
    }

    /* ---- 24 小时超量: 优先于"该不该吃" ----
     * ⚠️ 必须放在间隔判断**前面**: 一天里均匀地吃 5 次, 每一次的间隔都是够的 ——
     * 只看 lastTaken 拦不住, 只有总量能拦。这正是这个计数存在的理由。
     * (间隔是"两次之间别太近", 计数是"一天别太多", 两个问题不一样) */
    const int used = medCount24(it);
    if (it.perDay && used > it.perDay) {
        return std::string(it.name) + ": 24 小时内已经 " + std::to_string(used) +
               " 次, 超过方案的一天 " + std::to_string(it.perDay) +
               " 次了 —— 别再吃, 先确认一下。";
    }

    /* ⚠️ 时钟回拨(NTP 重新对时 / 手动改时间)时 now < lastTaken,
       无符号相减会下溢成约 40 亿分钟 —— 直接把"刚吃过"判成"早该吃了"。
       这里宁可按"刚吃过"处理(保守方向): 少提醒一次不要紧, 凭空催人吃药不行。 */
    const uint32_t elapsedMin  = (now >= it.lastTaken) ? (now - it.lastTaken) / 60 : 0;
    const uint32_t intervalMin = 24UL * 60 / it.perDay;

    std::string elapsed = std::to_string(elapsedMin / 60) + " 小时";
    if (elapsedMin % 60) elapsed += std::to_string(elapsedMin % 60) + " 分";

    if ((uint64_t)elapsedMin * 10 < (uint64_t)intervalMin * 6) {     // 不足间隔的 60%
        return std::string(it.name) + ": " + medFmtTime(it.lastTaken) +
               " 刚吃过 (距今 " + elapsed + "), 按一天 " + std::to_string(it.perDay) +
               " 次不该现在吃, 别重复。";
    }
    if ((uint64_t)elapsedMin * 10 >= (uint64_t)intervalMin * 8) {    // 达到间隔的 80%
        /* 已经用满方案量的话, 在这句后面附一个次数提醒 —— 这正是用户准备再吃
           一次的时刻, 次数最有参考价值 */
        std::string quota;
        if (it.perDay && used >= it.perDay) {
            quota = " (24 小时内第 " + std::to_string(used) + " 次, 方案一天 " +
                    std::to_string(it.perDay) + " 次)";
        }
        return std::string(it.name) + ": 距上次 " + medFmtTime(it.lastTaken) + " 已 " +
               elapsed + ", 按一天 " + std::to_string(it.perDay) + " 次该吃了。" + quota;
    }
    return std::string(it.name) + ": 距上次 " + elapsed + ", 还不到时间, 再等等。";
}

/* 转写里有没有"药名"的痕迹 —— 用来区分两种失败, 它们该说的话**完全不同**:
 *   (a) 真没听清                        -> "没听清是什么药"
 *   (b) 听清了是个药名, 但**不在记录里** -> 这可能是**拿错药了**, 必须明说
 *
 * 2026-10-07 实测踩过: 说「氯雷他定吃了」, 转写一字不差, 设备却回「没听出是哪种药」——
 * 把最该报警的那种情况, 说成了听力问题。
 */
static const char *commonDrugIn(const std::string &t)
{
    int n = 0;
    const char *const *cd = medCommonDrugs(&n);
    for (int i = 0; i < n; i++) {
        if (strIndex(t, cd[i]) >= 0) return cd[i];
    }
    return nullptr;
}

/* 内置表之外的药, 靠剂型后缀兜一下(「××片」「××胶囊」)。
 * ⚠️ 只看**短句子**: 「片」也是「照片」的片, 长句里的后缀不算证据。 */
static bool hasDrugSuffix(const std::string &t)
{
    if (t.length() > 30) return false;               // 超过 10 个汉字就不当药名了
    static const char *kSuf[] = { "片", "胶囊", "颗粒", "口服液", "溶液",
                                  "滴眼液", "眼药水", "软膏", "乳膏", "喷雾" };
    for (size_t i = 0; i < sizeof(kSuf) / sizeof(kSuf[0]); i++) {
        if (strIndex(t, kSuf[i]) >= 0) return true;
    }
    return false;
}

/* 从方案句里剥出药名 —— 语音**添加新药**靠它。
 *
 * 为什么需要: 拍照路径现在只识别不写表(见 main.cpp 的 runCaptureAsk),
 * 用户听到"这个药不在你的用药方案"的引导后, 说的那句
 * 「盐酸氯环利嗪片一天两次」就是把这味药加进方案的唯一方式。
 *
 * 剥法(以 "我想添加布洛芬一天两次吧" 为例):
 *   ① 剥频率短语: "一天/每天/一日/每日" 最早出现处 -> 一路剥到后面的第一个"次"
 *   ② 剥头部动词("我想添加/添加/新增/加入/录入"...) —— 用户听了引导很容易
 *      顺口说"添加布洛芬一天两次", 不剥的话药名就成了"添加布洛芬"
 *   ③ 剥尾部语气词("吧/哦/啊/呢"...) —— 同理, 名字里带"吧"会污染热词和匹配
 *
 * ⚠️ 剥完的合理性检查: 少于 2 个汉字(6 字节)当失败 —— "一天吃两次饭" 会剥出
 *    "饭", 那是误添加。宁可让用户重说, 也不能往表里塞垃圾名。
 * ⚠️ 只在 hasFreq(有"一天"这种锚) 的句子上调用 —— 没锚点的句子剥不干净。 */
static std::string drugNameFromPlanPhrase(const std::string &s)
{
    /* ① 剥频率短语 */
    static const char *const kStart[] = { "一天", "每天", "一日", "每日" };
    size_t p1 = std::string::npos;
    for (size_t i = 0; i < sizeof(kStart) / sizeof(kStart[0]); i++) {
        const size_t p = s.find(kStart[i]);
        if (p != std::string::npos && (p1 == std::string::npos || p < p1)) p1 = p;
    }
    if (p1 == std::string::npos) return "";

    const size_t ci = s.find("次", p1);             // 频率短语的终点
    if (ci == std::string::npos) return "";

    std::string out = s.substr(0, p1) + s.substr(ci + strlen("次"));
    strTrim(out);

    /* ② 剥头部动词(长的先试) */
    static const char *const kVerb[] = {
        "我想添加", "我要添加", "帮我添加", "我想加", "我要加",
        "添加", "新增", "加入", "录入",
    };
    for (size_t i = 0; i < sizeof(kVerb) / sizeof(kVerb[0]); i++) {
        const std::string v = kVerb[i];
        if (out.size() > v.size() && out.compare(0, v.size(), v) == 0) {
            out.erase(0, v.size());
            strTrim(out);
            break;
        }
    }

    /* ③ 剥尾部语气词(剥一层可能露出来下一层, 循环到剥不动为止) */
    static const char *const kTail[] = { "谢谢", "吧", "哦", "啊", "呢", "嘛", "嗯" };
    for (bool again = true; again; ) {
        again = false;
        for (size_t i = 0; i < sizeof(kTail) / sizeof(kTail[0]); i++) {
            const std::string t = kTail[i];
            if (out.size() > t.size() &&
                out.compare(out.size() - t.size(), t.size(), t) == 0) {
                out.erase(out.size() - t.size());
                strTrim(out);
                again = true;
                break;
            }
        }
    }

    /* ④ 只保留药名会出现的字符(汉字/字母/数字), 标点全剔掉 ——
       ASR 转写会带标点("盐酸氯环利晴片，。"), 不去的话会连着标点原样存进表,
       之后热词表、所有匹配都跟着歪(2026-10-11 实测踩过: 表里真进了
       「盐酸氯环利晴片，。」这条记录)。 */
    std::string keep;
    keep.reserve(out.size());
    for (size_t i = 0; i < out.size(); ) {
        const unsigned char c = (unsigned char)out[i];
        if (c < 0x80) {                             // ASCII: 只留字母数字
            if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
                (c >= 'a' && c <= 'z'))
                keep += (char)c;
            i++;
            continue;
        }
        if (c >= 0xF0) { i += 4; continue; }        // 4 字节(emoji 等) -> 去
        if ((c & 0xE0) == 0xC0) {                   // 2 字节(拉丁扩展) -> 留
            keep.append(out, i, 2);
            i += 2;
            continue;
        }
        /* 3 字节: 按首/次字节粗筛标点(去), 其余当汉字(留)。
           CJK 标点在 U+3000~303F(E3 80 xx)、通用标点/引号在 U+2000~203F(E2 80 xx)、
           全角形在 U+FF00~FFEF(EF xx xx) —— 宁可多剔一点, 漏剔会写进 NVS */
        const unsigned char c2 = (i + 1 < out.size()) ? (unsigned char)out[i + 1] : 0;
        if (!((c == 0xE3 && c2 == 0x80) || (c == 0xE2 && c2 == 0x80) || c == 0xEF))
            keep.append(out, i, 3);
        i += 3;
    }
    out.swap(keep);

    if (out.size() < 6) return "";                  // 少于 2 个汉字当剥失败
    return out;
}

/* UTF-8 字符的字节长度(按首字节判; 非法字节当 1 前进, 不会死循环) */
static int utf8CharLen(const char *p)
{
    const unsigned char c = (unsigned char)*p;
    if (c < 0x80)           return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}

/* 模糊匹配: 在句子 s 里找"和 name 只差**一个字符**"的连续片段(替换型)。
 *
 * 动机: ASR 对药名里的生僻字几乎必错 —— 实测「嗪」稳定被转成「睛/晴」
 * (坑 #18 的「酸氯环利晴片」、2026-10-11 的「盐酸氯环利睛片」都是它)。
 * 用户说的一定就是表里那味药, 精确匹配却全军覆没, 只能回"没听清"。
 *
 * ⚠️ 只做**长度相同、恰一个字符不同**这一种: 表内药名彼此都差好几个字,
 *    误配率极低; 插入/删除型(长度不同)误配风险大得多, 不做。
 * ⚠️ 只对 **3 字符以上**的名字生效: "钙片"这种短名差一个字就面目全非。
 *
 * 命中返回起始**字节**偏移, *outLen 回填句子里命中段的字节长度(给 s2 擦除用 ——
 * 是错字那段实际长度, 和 name 的长度可能不同); 没命中返回 -1。 */
static long fuzzyNamePos(const std::string &s, const char *name, size_t *outLen)
{
    int nName = 0;                                  // 药名的字符数
    for (const char *p = name; *p; p += utf8CharLen(p)) nName++;
    if (nName < 3) return -1;

    /* 句子的字符起点(字节偏移)。⚠️ 用 uint16 不用 size_t —— 这里在 loopTask
       的 8KB 栈上执行(网页 /say 路径), size_t 版本要 768 字节, 它在坑 #21 的
       爆栈预算里很金贵。偏移只记录前 96 个字符, 最大 ≤ 96×4 字节, uint16 绰绰有余 */
    uint16_t offs[96];
    int    nOffs = 0;
    for (size_t i = 0; i < s.size() && nOffs < 96; ) {
        offs[nOffs++] = i;
        i += utf8CharLen(s.c_str() + i);
    }

    for (int i = 0; i + nName <= nOffs; i++) {
        const char *sp   = s.c_str() + offs[i];
        const char *np   = name;
        int         diff = 0;
        size_t      used = 0;
        for (int k = 0; k < nName; k++) {
            const int sl = utf8CharLen(sp + used);
            const int nl = utf8CharLen(np);
            if (sl != nl || memcmp(sp + used, np, sl) != 0) {
                if (++diff > 1) break;              // 差两个字符以上, 不是它
            }
            used += sl;
            np   += nl;
        }
        if (diff <= 1) {                            // 完整比完且只差一个字符
            if (outLen) *outLen = used;
            return (long)offs[i];
        }
    }
    return -1;
}

std::string medHandleUtterance(const char *text)
{
    MedGuard g;
    if (!text || !*text) return "没收到内容。";

    const std::string s(text);

    /* 先在已有记录里找药名 —— 找不到就看下面能不能"添加新药" */
    MedItem items[MED_MAX_ITEMS];
    const int n = medLoad(items, MED_MAX_ITEMS);

    int idx = -1;
    for (int i = 0; i < n; i++) {
        if (strIndex(s, items[i].name) >= 0) { idx = i; break; }
    }

    /* 第二遍: 用户说的比记录里的名字**短**时, 上面那个方向是匹配不上的 ——
       记录存的是「苯磺酸氨氯地平片」, 用户只会说「氨氯地平」。用内置药名做中介再试一次。
       不补这一遍的话, 少说一个"苯磺酸"就会被当成"这不是你的药", 反而乱报警。 */
    if (idx < 0) {
        int cdN = 0;
        const char *const *cd = medCommonDrugs(&cdN);
        for (int i = 0; i < n && idx < 0; i++) {
            for (int k = 0; k < cdN; k++) {
                if (strIndex(s, cd[k]) >= 0 && strIndex(items[i].name, cd[k]) >= 0) {
                    idx = i;
                    break;
                }
            }
        }
    }

    /* 第三遍(模糊): 精确和"内置中介"都失败时, 试试"只差一个字符" ——
       ASR 对药名里的生僻字几乎必错(实测「嗪」稳定转成「睛/晴」), 而用户说的
       一定就是他表里那味药。见 fuzzyNamePos 的注释(为什么只做替换型、限 3 字以上)。 */
    long   fuzzyPos = -1;
    size_t fuzzyLen = 0;
    if (idx < 0) {
        for (int i = 0; i < n; i++) {
            const long p = fuzzyNamePos(s, items[i].name, &fuzzyLen);
            if (p >= 0) {
                idx      = i;
                fuzzyPos = p;
                ESP_LOGI(TAG, "[MED] 模糊匹配: 「%s」≈ 表内「%s」(差一个字)",
                              s.c_str(), items[i].name);
                break;
            }
        }
    }

    /* 匹配不上时的提示 —— 要分清**三种**情况, 它们该说的话完全不同, 混成一句
       「没听出是哪种药」会把最该报警的那种盖掉(实测踩过, 见 commonDrugIn 的注释):
         (a) 表里一条药都没有          -> 不是"没听出来", 是根本没东西可比
         (b) 听清了是个药名、但不在记录里 -> **可能拿错药了**, 必须明说
         (c) 真没听清                  -> 让它再说一遍 */
    std::string noMatch;
    if (n == 0) {
        noMatch = "还没记录任何药。按住按钮说「药名 一天两次」就能添加, 也可以在网页上添加。";
    } else {
        std::string have = "已记录的有: ";
        const int show = (n > 6) ? 6 : n;        // 要播报出来, 列太长听不完
        for (int i = 0; i < show; i++) {
            if (i) have += "、";
            have += items[i].name;
        }
        if (n > show) have += " 等 " + std::to_string(n) + " 种";
        have += "。";

        const char *cd = commonDrugIn(s);
        if (cd) {
            noMatch = "听出来是「" + std::string(cd) + "」，它不在你的用药记录里。" +
                      have + "注意别吃错药。";
        } else if (hasDrugSuffix(s)) {
            noMatch = "听出来是个药, 但它不在你的用药记录里。" + have + "注意别吃错药。";
        } else {
            noMatch = "没听清是什么药。" + have;
        }
    }

    /* ⚠️ 关键词判断要在**抠掉药名之后**的串上做。
       药名自己就可能带关键字: 「蓝芩口服液」里有个「服」——
       用户只是说个药名想查一下, 以前会被判成"服用了", 直接记一笔。
       所以先把匹配到的药名从句子里删掉, 再做下面所有判断。 */
    std::string s2 = s;
    if (idx >= 0) {
        /* 精确出现就精确删; 模糊命中的话删句子里**那段错字**
           (按名字精确 find 找不到错字版本 —— 不擦掉的话, 名字里若带
           "服/吃"这类关键词会干扰后面的意图判断) */
        const size_t p = s2.find(items[idx].name);
        if (p != std::string::npos) {
            s2.erase(p, strlen(items[idx].name));
        } else if (fuzzyPos >= 0) {
            s2.erase((size_t)fuzzyPos, fuzzyLen);
        }
    }

    /* ---- 意图判断 ----
     * ⚠️ 顺序**必须**是: 频率歧义 > 否定/疑问 > 方案 > 服用 > 查询。真实反例:
     *   「阿莫西林一天吃两次」里有"吃", 但它是**方案** —— 以前记成"已服用";
     *   「没吃」「忘了吃」     里也有"吃", 但它是**否定** —— 以前记成"已服用",
     *     接下来 medAdvice 还会回一句「刚吃过, 别重复」, 结果是**劝人漏服**;
     *   「蓝芩口服液」         药名自带"服" —— 以前**查询也被记成服用**。
     * 判断错不会报错, 只会安静地把记录写反, 所以这里宁可选保守的那支。 */
    const bool hasEat  = (strIndex(s2, "吃") >= 0 || strIndex(s2, "服") >= 0 ||
                          strIndex(s2, "喝") >= 0 || strIndex(s2, "滴") >= 0 ||
                          strIndex(s2, "喷") >= 0 || strIndex(s2, "贴") >= 0);
    const bool hasFreq = (strIndex(s2, "一天") >= 0 || strIndex(s2, "每天") >= 0 ||
                          strIndex(s2, "一日") >= 0 || strIndex(s2, "每日") >= 0);
    const bool hasNeg  = (strIndex(s2, "没") >= 0 || strIndex(s2, "未") >= 0 ||
                          strIndex(s2, "忘") >= 0);
    /* 疑问句不能当成"记录": 「能和酒一起喝吗」里有关键词, 但那是在问 */
    const bool isQuestion = (strIndex(s2, "吗") >= 0 || strIndex(s2, "呢") >= 0 ||
                             strIndex(s2, "？") >= 0 || strIndex(s2, "?") >= 0);
    const int  perDay  = parsePerDay(s2);

    /* ① 频率说法本地规则判不了 —— **宁可不记**, 也别静默记错。
       次数记错 → 间隔算错 → 之后每一次「该不该吃」都是错的, 比"没听懂"糟得多:
         「三天一次」「一周一次」「每隔 8 小时一次」都会算出 1 次/天;
         「早晚各一次」也会算成 1 次(其实是 2 次)。 */
    const bool trickyFreq = (strIndex(s2, "周") >= 0 || strIndex(s2, "星期") >= 0 ||
                             strIndex(s2, "隔") >= 0 || strIndex(s2, "小时") >= 0 ||
                             strIndex(s2, "早晚") >= 0 ||
                             (strIndex(s2, "天") >= 0 && !hasFreq));
    if (perDay > 0 && trickyFreq) {
        return "这个说法我还算不准 —— 请说「一天两次」这种, 明确到每天几次。";
    }

    /* 方案句 = 有次数, 且(带频率词 或 句子里根本没有服用动词)。
       后半句是给「两次」这种光说次数的短句留的;
       反过来「吃了两次」有服用动词、没有频率词, 就不算方案。 */
    const bool isPlan = (perDay > 0) && (hasFreq || !hasEat);

    /* ② 否定句 / 疑问句: 不记, 只回答"现在该不该吃" */
    if (hasEat && !isPlan && (hasNeg || isQuestion)) {
        if (idx < 0) return noMatch;
        return std::string(hasNeg ? "好, 这次不记入服用。" : "这是在问, 我不记入服用。")
               + adviceFor(items[idx]);
    }

    /* ③ 方案: 只改次数。时段串**保持原样** —— 以前这里传的是 "",
       经 medAdd 一存就把之前记好的时段抹掉了 */
    if (isPlan) {
        /* 药名不在表里 —— 看这是不是一句"添加新药"的话("盐酸氯环利嗪片一天两次")。
           拍照路径只识别不写表, 这句话就是唯一的自动添加入口: 拍照识别到方案外的
           药时, 播报里教用户说的正是它(见 main.cpp 的 runCaptureAsk)。
           ⚠️ 剥名字有失败面(剥不干净 / "一天吃两次饭"这种), 函数内做了合理性检查;
              剥不出来就退回 noMatch 让用户重说 —— 宁可多问一遍, 也不能往药品表里
              塞垃圾名(会污染 ASR 热词表和后续所有匹配)。
           ⚠️ 疑问/否定句**不添加**: 「这个药能一天两次吗」会剥出"这个药能吗",
              「布洛芬没吃一天两次」会剥出"布洛芬没吃" —— 这两类都回落 noMatch。
              (② 分支只在 hasEat 时拦疑问/否定, 这里必须再拦一次) */
        if (idx < 0 && hasFreq && !isQuestion && !hasNeg) {
            /* 剥完先过药名纠错(ASR 的错字型, 见 kNameFix)再查表/入库 ——
               不然「盐酸氯环利睛片一天两次」会新存一条错名记录 */
            const std::string nm = medFixDrugName(drugNameFromPlanPhrase(s2));
            if (!nm.empty()) {
                /* 剥出的名字可能命中表里已有的药(用户只说了简称) —— 命中就回到
                   "更新方案"的语义, 别按新药添加(那会多出一条重复记录) */
                for (int i = 0; i < n; i++) {
                    if (nameMatch(items[i].name, nm.c_str())) { idx = i; break; }
                }
                if (idx < 0) {
                    /* ⚠️ 这里**内联**添加, 不能调 medAdd() —— medAdd 会再 medLoad()
                       一份 items[MED_MAX_ITEMS](≈2.7KB), 叠在本函数自己那份上, 正好
                       把 loopTask 的 8KB 栈打爆(实测 2026-10-11: 网页 /say 加新药
                       两次全崩, Stack canary 触发; 语音 task 栈 12KB 侥幸活着)。
                       见 CLAUDE.md 坑 #21。
                       (nm 已经过 drugNameFromPlanPhrase 的字符过滤 + medFixDrugName,
                        不含 | ; 等分隔符, 安全) */
                    if (nm.size() >= MED_NAME_LEN) return "添加失败 —— 药名太长。";
                    if (n >= MED_MAX_ITEMS)        return "添加失败 —— 药品表已满。";
                    memset(&items[n], 0, sizeof(MedItem));
                    utf8SafeCopy(items[n].name, MED_NAME_LEN, nm.c_str());
                    items[n].perDay = (uint8_t)perDay;
                    if (!saveItems(items, n + 1)) return "添加失败 (NVS 写入失败)。";
                    ESP_LOGI(TAG, "[MED] 语音添加新药: %s (一天 %d 次)",
                                  items[n].name, perDay);
                    return std::string(items[n].name) + ": 已添加到用药方案 —— 一天 " +
                           std::to_string(perDay) + " 次";
                }
            }
        }
        if (idx < 0) return noMatch;

        items[idx].perDay = (uint8_t)perDay;
        if (!saveItems(items, n)) return "方案没存进去 (NVS 写入失败)。";
        return std::string(items[idx].name) + ": 方案已记下 —— 一天 " +
               std::to_string(perDay) + " 次";
    }

    /* ④ 服用 */
    if (hasEat) {
        if (idx < 0) return noMatch;
        const uint32_t now = medNow();
        if (now == 0) {
            return "设备时间还没同步(没连上 WiFi?), 这次先不记 —— 记下来也不准。";
        }
        const int used = countDose(items[idx], now);
        if (!saveItems(items, n)) return "没存进去 (NVS 写入失败)。";

        /* 每次都报一下 24 小时内的次数(用户要的就是这个反馈);
           超过方案量就明确提醒 —— 间隔够**不代表**总量够 */
        std::string msg = std::string(items[idx].name) + ": 已记下这次服用 (" +
                          medFmtTime(now) + "), 24 小时内第 " + std::to_string(used) + " 次";
        if (items[idx].perDay && used > items[idx].perDay) {
            msg += " —— 超过方案的一天 " + std::to_string(items[idx].perDay) +
                   " 次了, 注意别超量";
        }
        msg += "。";
        return msg;
    }

    /* ⑤ 都不像, 当成查询 */
    if (idx < 0) return noMatch;
    return adviceFor(items[idx]);
}

/* ==================== 建议 ====================
 * 全在本地算 —— 这条路径不调任何 API, 又快又免费。
 * 判断依据不用「早上/晚上」这种模糊时段, 而是按方案算出「该隔多久」再比实际间隔:
 * 时段串只作展示, 因为用户说「早晚各一次」时, 具体几点几分本来就没准数。 */
std::string medAdvice(const char *name)
{
    MedGuard g;
    if (!name || !*name) return "没识别出药名";

    MedItem items[MED_MAX_ITEMS];
    const int n = medLoad(items, MED_MAX_ITEMS);

    for (int i = 0; i < n; i++) {
        if (nameMatch(items[i].name, name)) return adviceFor(items[i]);
    }
    return std::string(name) + ": 还没录入用药方案 —— 按住 BOOT 说一句, 比如「一天两次」。";
}
