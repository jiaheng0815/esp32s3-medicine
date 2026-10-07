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

/* 解析一条 "名称|次数|时段|时间戳" */
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
    return true;
}

/* 把整表序列化回 s_blob 并落盘 */
static bool saveItems(const MedItem *items, int n)
{
    size_t len = 0;

    for (int i = 0; i < n; i++) {
        char line[96];
        const int m = snprintf(line, sizeof(line), "%s|%u|%s|%lu;",
                               items[i].name,
                               (unsigned)items[i].perDay,
                               items[i].slots,
                               (unsigned long)items[i].lastTaken);
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

bool medMarkTaken(const char *name)
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
            items[i].lastTaken = now;
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
        return std::string(it.name) + ": 还没录入用药方案 —— 按住 BOOT 说一句, 比如「一天两次」。";
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
        return std::string(it.name) + ": 距上次 " + medFmtTime(it.lastTaken) + " 已 " +
               elapsed + ", 按一天 " + std::to_string(it.perDay) + " 次该吃了。";
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

std::string medHandleUtterance(const char *text)
{
    MedGuard g;
    if (!text || !*text) return "没收到内容。";

    const std::string s(text);

    /* 先在已有记录里找药名 —— 找不到就什么都做不了 */
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

    /* 匹配不上时的提示 —— 要分清**三种**情况, 它们该说的话完全不同, 混成一句
       「没听出是哪种药」会把最该报警的那种盖掉(实测踩过, 见 commonDrugIn 的注释):
         (a) 表里一条药都没有          -> 不是"没听出来", 是根本没东西可比
         (b) 听清了是个药名、但不在记录里 -> **可能拿错药了**, 必须明说
         (c) 真没听清                  -> 让它再说一遍 */
    std::string noMatch;
    if (n == 0) {
        noMatch = "还没记录任何药 —— 先短按拍照识别药盒, 或者在网页上添加一条。";
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
        const size_t p = s2.find(items[idx].name);
        if (p != std::string::npos) s2.erase(p, strlen(items[idx].name));
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
        items[idx].lastTaken = now;
        if (!saveItems(items, n)) return "没存进去 (NVS 写入失败)。";
        return std::string(items[idx].name) + ": 已记下这次服用 (" +
               medFmtTime(now) + ")";
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
