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

    const String s = p.getString(MED_KEY, "");
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
    const bool ok = p.putString(MED_KEY, s_blob) > 0;
    p.end();
    return ok;
}

/* 解析一条 "名称|次数|时段|时间戳" */
static bool parseRec(const char *rec, size_t len, MedItem *it)
{
    /* 手动找三个分隔符, 避免构造临时 String */
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
            Serial.println("[MED] 记录超出 NVS 单值上限, 后面的被丢弃");
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
    if (!name || !*name) return -1;

    MedItem items[MED_MAX_ITEMS];
    const int n = medLoad(items, MED_MAX_ITEMS);
    for (int i = 0; i < n; i++) {
        if (nameMatch(items[i].name, name)) return i;
    }
    return -1;
}

bool medAdd(const char *name, uint8_t perDay, const char *slots)
{
    if (!name || !*name || strlen(name) >= MED_NAME_LEN) return false;

    MedItem items[MED_MAX_ITEMS];
    const int n = medLoad(items, MED_MAX_ITEMS);

    /* 已存在 → 只更新方案, 不动服用记录 */
    for (int i = 0; i < n; i++) {
        if (nameMatch(items[i].name, name)) {
            items[i].perDay = perDay;
            utf8SafeCopy(items[i].slots, MED_SLOT_LEN, slots);
            return saveItems(items, n);
        }
    }

    if (n >= MED_MAX_ITEMS) {
        Serial.printf("[MED] 药品表已满 (%d 条), 没记下新的\n", n);
        return false;
    }

    memset(&items[n], 0, sizeof(MedItem));
    utf8SafeCopy(items[n].name, MED_NAME_LEN, name);
    items[n].perDay = perDay;
    utf8SafeCopy(items[n].slots, MED_SLOT_LEN, slots);
    items[n].lastTaken = 0;

    return saveItems(items, n + 1);
}

bool medMarkTaken(const char *name)
{
    if (!name || !*name) return false;

    MedItem items[MED_MAX_ITEMS];
    const int n = medLoad(items, MED_MAX_ITEMS);

    for (int i = 0; i < n; i++) {
        if (nameMatch(items[i].name, name)) {
            items[i].lastTaken = medNow();     // 时间为 0 时也照样记, 只是个占位
            return saveItems(items, n);
        }
    }
    return false;
}

bool medRemove(const char *name)
{
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

String medFmtTime(uint32_t ts)
{
    if (ts == 0) return "从未";

    const time_t t = (time_t)ts;
    struct tm tm;
    localtime_r(&t, &tm);

    char buf[16];
    snprintf(buf, sizeof(buf), "%02d:%02d", tm.tm_hour, tm.tm_min);
    return String(buf);
}

/* ==================== 自然语言输入 ====================
 * 麦克风到货前由网页文本框调用; 到货后 ASR 转写结果也走这里。
 * 只做极简规则匹配, 不调模型 —— 输入本来就短, 而且这几类说法很固定。
 * (复杂说法比如「隔天一次」「饭前半小时」交给模型解析, 那是阶段 3 的事) */

/* 从 "一天两次" / "每天2次" / "一日三次" 里抠出次数 */
static int parsePerDay(const String &s)
{
    static const char *cn[] = { "一", "二", "两", "三", "四", "五", "六" };

    /* 必须用双引号! 「次」在 UTF-8 里是 3 个字节(E6 AC A1),
       写成单引号 '次' 会被当成 multi-character constant, 编译期截断成一个字节
       (0xA1), 于是这里实际上在找一个字节 0xA1 —— 永远找不到, 整个函数失效。 */
    const int ci = s.indexOf("次");
    if (ci <= 0) return 0;

    /* 从"次"字往前找最近的数字, 最多退 4 个字节(中文数字占 3 字节) */
    for (int back = 1; back <= 4 && ci - back >= 0; back++) {
        const char c = s[ci - back];
        if (c >= '1' && c <= '9') return c - '0';

        for (int k = 0; k < 6; k++) {
            if (s.substring(ci - back).startsWith(cn[k])) return k + 1;
        }
    }
    return 0;
}

String medHandleUtterance(const char *text)
{
    if (!text || !*text) return "没收到内容。";

    const String s(text);

    /* 先在已有记录里找药名 —— 找不到就什么都做不了 */
    MedItem items[MED_MAX_ITEMS];
    const int n = medLoad(items, MED_MAX_ITEMS);

    int idx = -1;
    for (int i = 0; i < n; i++) {
        if (s.indexOf(items[i].name) >= 0) { idx = i; break; }
    }

    const char *needName = "没听出是哪种药。先拍照识别, 或者在下面手动添加一条。";

    /* ① 记录服用 */
    if (s.indexOf("吃") >= 0 || s.indexOf("服") >= 0) {
        if (idx < 0) return needName;
        medMarkTaken(items[idx].name);
        return String(items[idx].name) + ": 已记下这次服用 (" +
               (medTimeReady() ? medFmtTime(medNow()) : String("时间未同步")) + ")";
    }

    /* ② 记录用药方案 */
    const int perDay = parsePerDay(s);
    if (perDay > 0) {
        if (idx < 0) return needName;
        medAdd(items[idx].name, (uint8_t)perDay, "");
        return String(items[idx].name) + ": 方案已记下 —— 一天 " +
               String(perDay) + " 次";
    }

    /* ③ 都不像, 当成查询 */
    if (idx < 0) return needName;
    return medAdvice(items[idx].name);
}

/* ==================== 建议 ====================
 * 全在本地算 —— 这条路径不调任何 API, 又快又免费。
 * 判断依据不用「早上/晚上」这种模糊时段, 而是按方案算出「该隔多久」再比实际间隔:
 * 时段串只作展示, 因为用户说「早晚各一次」时, 具体几点几分本来就没准数。 */
String medAdvice(const char *name)
{
    if (!name || !*name) return "没识别出药名";

    MedItem items[MED_MAX_ITEMS];
    const int n = medLoad(items, MED_MAX_ITEMS);

    int idx = -1;
    for (int i = 0; i < n; i++) {
        if (nameMatch(items[i].name, name)) { idx = i; break; }
    }

    if (idx < 0 || items[idx].perDay == 0) {
        return String(name) + ": 还没录入用药方案 —— 按住 BOOT 说一句, 比如「一天两次」。";
    }

    const MedItem &it = items[idx];

    if (it.lastTaken == 0) {
        return String(it.name) + ": 按方案一天 " + String(it.perDay) +
               " 次, 但还没有服用记录。";
    }

    const uint32_t now = medNow();
    if (now == 0) {
        return String(it.name) + ": 上次服用 " + medFmtTime(it.lastTaken) +
               "。(设备时间未同步, 这次只记录不判断)";
    }

    const uint32_t elapsedMin  = (now - it.lastTaken) / 60;
    const uint32_t intervalMin = 24UL * 60 / it.perDay;

    String elapsed = String(elapsedMin / 60) + " 小时";
    if (elapsedMin % 60) elapsed += String(elapsedMin % 60) + " 分";

    if ((uint64_t)elapsedMin * 10 < (uint64_t)intervalMin * 6) {     // 不足间隔的 60%
        return String(it.name) + ": " + medFmtTime(it.lastTaken) +
               " 刚吃过 (距今 " + elapsed + "), 按一天 " + String(it.perDay) +
               " 次不该现在吃, 别重复。";
    }
    if ((uint64_t)elapsedMin * 10 >= (uint64_t)intervalMin * 8) {    // 达到间隔的 80%
        return String(it.name) + ": 距上次 " + medFmtTime(it.lastTaken) + " 已 " +
               elapsed + ", 按一天 " + String(it.perDay) + " 次该吃了。";
    }
    return String(it.name) + ": 距上次 " + elapsed + ", 还不到时间, 再等等。";
}
