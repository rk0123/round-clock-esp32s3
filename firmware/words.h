#pragma once
// ============================================================
//  背单词 v2.0  ·  百词斩式四选一 + 艾宾浩斯记忆曲线
//  8 档内置词库（小学/初中/四级/六级/考研/托福/GRE/雅思）各约 100 词，完全离线。
//  每个词独立记录复习阶段与下次复习时刻，只有到期的词才进出题队列；
//  答对推进阶段、答错回到第一阶段。复习状态存 NVS，断电不丢。
// ============================================================
#include "wordbank.h"
#include "wordbank_extra.h"

// main.cpp 里的前向声明（本头文件在颜色宏之后 include）
static void drawBtnText(int x, int boxY, int boxH, const char *s, uint16_t col,
                        int font, int scale, int align);
static bool hit(int x, int y, int cx, int cy, int r);
static void showToast(const char *s);
static void render();
#include "ui_kit.h"              // 二期统一 UI：uiPage / uiBtn / uiPill
#include "font_serif.h"          // v2.8：Georgia 30px 衬线英文（背单词题干用，4-bit AA）

// ---------------- 8 档词库 ----------------
#define WORD_BOOK_N 8
static const WordEntry *const WORD_TABLES[WORD_BOOK_N] = {
  WORDS_PRIMARY, WORDS_JUNIOR, WORDS_CET4, WORDS_CET6,
  WORDS_KAOYAN, WORDS_TOEFL, WORDS_GRE, WORDS_IELTS
};
static const int WORD_TABLE_N[WORD_BOOK_N] = {
  (int)WORDS_PRIMARY_N, (int)WORDS_JUNIOR_N, (int)WORDS_CET4_N, (int)WORDS_CET6_N,
  (int)WORDS_KAOYAN_N, (int)WORDS_TOEFL_N, (int)WORDS_GRE_N, (int)WORDS_IELTS_N
};
static const char *const WORD_BOOK_NAMES[WORD_BOOK_N] = {
  "小学", "初中", "四级", "六级", "考研", "托福", "GRE", "雅思"
};

// ---------------- 艾宾浩斯复习间隔 ----------------
// 5 分钟 / 30 分钟 / 12 小时 / 1 天 / 2 天 / 4 天 / 7 天 / 15 天 / 30 天
#define SRS_STAGES 9
static const uint32_t SRS_INTERVAL[SRS_STAGES] = {
  300UL, 1800UL, 43200UL, 86400UL, 172800UL,
  345600UL, 604800UL, 1296000UL, 2592000UL
};
#define WORD_SRS_MAX 1200                    // 预留上限（当前总词数 800）
struct WordSRS { uint32_t next; uint8_t stage; uint8_t pad[3]; };
static WordSRS gWordSrs[WORD_SRS_MAX];       // 9600B，NVS 一次性读写

static int  wordBook      = 0;
static int  wordBookPage  = 0;               // 0=前四档 1=后四档
static int  wordPos       = 0;
static int  wordRight     = 0;
static int  wordWrong     = 0;
static int  wordOpts[4]   = {0};
static int  wordAnsSlot   = 0;
static bool wordPicked    = false;
static bool wordWasRight  = false;
static int  wordPickSlot  = 0;
static unsigned long wordRng = 20261117UL;
static int  wordQueue[WORD_SRS_MAX];         // 本轮题目（词库内下标）
static int  wordQueueN    = 0;
static bool wordFreeMode  = false;           // 无到期词时自由复习
static bool wordPicking   = false;           // 正在选词书（8 宫格子页）
static int  wordLastStage = 0;               // 当前题作答前所处阶段

static const WordEntry *wordTable() { return WORD_TABLES[wordBook]; }
static int wordTableN() { return WORD_TABLE_N[wordBook]; }

// 全局 SRS 索引：各词库下标依次排开
static int wordGlobalIndex(int book, int pos) {
  int idx = 0;
  for (int b = 0; b < book && b < WORD_BOOK_N; b++) idx += WORD_TABLE_N[b];
  return idx + pos;
}
static uint32_t wordNow() { return (uint32_t)time(nullptr); }

// 确定性伪随机：同样的 seed 每轮题目顺序一致，便于验收截图比对
static unsigned wordRand(unsigned n) {
  wordRng = wordRng * 1103515245UL + 12345UL;
  return (unsigned)((wordRng >> 16) % n);
}

static void wordLoad() {
  wordBook  = constrain(cfgGetInt("wd.book", 0), 0, WORD_BOOK_N - 1);
  wordRight = cfgGetInt("wd.right", 0);
  wordWrong = cfgGetInt("wd.wrong", 0);
  wordBookPage = wordBook / 4;
}
static void wordSave() {
  cfgSetInt("wd.book", wordBook);
  cfgSetInt("wd.right", wordRight);
  cfgSetInt("wd.wrong", wordWrong);
}
static void wordSrsLoad() {
  memset(gWordSrs, 0, sizeof(gWordSrs));
  gPrefs.begin("rcfg", true);
  size_t len = gPrefs.getBytesLength("wd.srs");
  if (len == sizeof(gWordSrs)) gPrefs.getBytes("wd.srs", gWordSrs, len);
  gPrefs.end();
}
static void wordSrsSave() {
  gPrefs.begin("rcfg", false);
  gPrefs.putBytes("wd.srs", gWordSrs, sizeof(gWordSrs));
  gPrefs.end();
}
static int wordDueCount() {
  uint32_t now = wordNow();
  int n = wordTableN(), due = 0;
  for (int i = 0; i < n; i++) {
    uint32_t nx = gWordSrs[wordGlobalIndex(wordBook, i)].next;
    if (nx == 0 || nx <= now) due++;
  }
  return due;
}
// 建出题队列：dueOnly=true 只取到期词；到期词不足时也照常出题（不会卡死）
static void wordNextQuestion();   // 前向声明：wordReset() 内部要用

static void wordBuildQueue(bool dueOnly) {
  uint32_t now = wordNow();
  int n = wordTableN();
  wordQueueN = 0;
  for (int i = 0; i < n; i++) {
    uint32_t nx = gWordSrs[wordGlobalIndex(wordBook, i)].next;
    if (!dueOnly || nx == 0 || nx <= now) wordQueue[wordQueueN++] = i;
  }
  wordFreeMode = !dueOnly;
  // 洗牌：Fisher-Yates（确定性 RNG）
  for (int i = wordQueueN - 1; i > 0; i--) {
    int j = (int)wordRand((unsigned)(i + 1));
    int t = wordQueue[i]; wordQueue[i] = wordQueue[j]; wordQueue[j] = t;
  }
}
static void wordReset(bool keepStats) {
  wordPos = 0; wordPicked = false; wordWasRight = false;
  if (!keepStats) { wordRight = 0; wordWrong = 0; }
  wordRng = 20261117UL + (unsigned long)wordBook * 7919UL;
  wordBuildQueue(true);
  if (wordQueueN == 0) wordBuildQueue(false);   // 今日已复习完 → 自由复习模式
  wordSave();
  if (wordQueueN) wordNextQuestion();
}
static void wordInit() {
  wordLoad();
  wordSrsLoad();
  wordReset(true);
}

// 出题：正确释义一个 + 三个来自同词库的干扰项，再打乱到四个槽位
static void wordNextQuestion() {
  if (wordPos >= wordQueueN) return;
  const WordEntry *tab = wordTable();
  int n = wordTableN();
  if (n < 4) return;
  int ans = wordQueue[wordPos];
  wordOpts[0] = ans;
  for (int i = 1; i < 4; i++) {
    int cand;
    do { cand = wordRand((unsigned)n); }
    while (cand == ans);
    bool dup = false;
    for (int k = 1; k < i; k++) if (wordOpts[k] == cand) { dup = true; break; }
    if (dup) { i--; continue; }
    wordOpts[i] = cand;
  }
  for (int i = 3; i > 0; i--) {
    int j = (int)wordRand((unsigned)(i + 1));
    int t = wordOpts[i]; wordOpts[i] = wordOpts[j]; wordOpts[j] = t;
  }
  for (int i = 0; i < 4; i++) if (wordOpts[i] == ans) { wordAnsSlot = i; break; }
  wordLastStage = gWordSrs[wordGlobalIndex(wordBook, ans)].stage;
  wordPicked = false;
  wordWasRight = false;
}

// ---------------- 发音（v2.8）----------------
// 用有道词典的免密钥 dictvoice 接口（英音 type=1 / 美音 type=2，实测返回 audio/mpeg）。
// 必须走网络，没连 Wi-Fi 时静默跳过；开关写 NVS "wd.pron"，掉电保留。
static bool gWordPronOn = true;
static bool gWordPronLoaded = false;
static void wordPronLoad() {
  if (gWordPronLoaded) return;         // NVS 要等 setup 里 cfgLoad() 之后才读得到
  gWordPronLoaded = true;
  gWordPronOn = cfgGetInt("wd.pron", 1) != 0;
}
static void wordPronounce(bool force = false) {
  wordPronLoad();
  if ((!gWordPronOn && !force) || !gAudQ || wordPos >= wordQueueN) return;
  const WordEntry *tab = wordTable();
  const char *w = tab[wordOpts[wordAnsSlot]].en;
  char url[220];
  int k = snprintf(url, sizeof(url), "https://dict.youdao.com/dictvoice?audio=");
  for (const char *p = w; *p && k < (int)sizeof(url) - 16; ++p) {
    unsigned char ch = (unsigned char)*p;
    bool plain = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                 (ch >= '0' && ch <= '9') || ch == '-' || ch == '_';
    if (plain) url[k++] = (char)ch;
    else       k += snprintf(url + k, sizeof(url) - k, "%%%02X", ch);
  }
  snprintf(url + k, sizeof(url) - k, "&type=2");
  Serial.printf("[PRON] %s\n", url);
  audioPlayUrl(url);
}

// 右上角小喇叭：不要外圈。开=亮黄喇叭+声波，关=灰喇叭（点一下切换，热区仍保留 18px）
static void drawPronIcon(int cx, int cy) {
  wordPronLoad();
  uint16_t c = gWordPronOn ? C_YELLOW : C_GREY;
  fillRect(cx - 8, cy - 3, 4, 6, c);
  fillTri(cx - 4, cy - 3, cx + 2, cy - 8, cx + 2, cy + 8, c);
  if (gWordPronOn) {
    arcRing(cx + 3, cy, 8, 50, 130, 2, c);
    arcRing(cx + 3, cy, 12, 50, 130, 2, c);
  }
}

// 作答后更新这个词的艾宾浩斯阶段：对→下一间隔，错→回到第一阶段（5 分钟后）
static void wordGrade(bool right) {
  int gi = wordGlobalIndex(wordBook, wordOpts[wordAnsSlot]);
  uint8_t st = gWordSrs[gi].stage;
  if (right) st = (uint8_t)((st + 1 < SRS_STAGES) ? st + 1 : SRS_STAGES - 1);
  else       st = 0;
  gWordSrs[gi].stage = st;
  gWordSrs[gi].next  = wordNow() + SRS_INTERVAL[st];
  wordSrsSave();
}

// ---------------- 渲染 ----------------
// FONT16 真实墨迹右缘 + gap。
// charAdvance() 给 ASCII 只有 10px，但字形画布是 16px —— 末字墨迹比步进多出 6px，
// 直接按 textWidth 排下一段，实机上数字必然咬进后面的中文里。
static int wordInk(const char *s, int x, int gap) {
  int len = (int)strlen(s);
  int total = textWidth(s, 16, 1);
  bool asciiEnd = len > 0 && ((unsigned char)s[len - 1] < 0x80);
  return x + total - (asciiEnd ? 10 : 16) + 16 + gap;
}

static void wordRender() {
  // 标题下方要放整条胶囊，分隔线（y=64，x=132..240）会正好压在胶囊上 → 关掉分隔线
  uiPage("背单词", 0, false);

  // 顶部：整颗「当前 xx」胶囊 = 换书入口（热区见 wordTap）。
  // 不能用圆键：r18 直径 36 一定穿过 y=64 那条位置并压到状态行，实机就是一团糊。
  char bkLab[24]; snprintf(bkLab, sizeof(bkLab), "当前 %s", WORD_BOOK_NAMES[wordBook]);
  uiPill(128, 66, 104, 30, bkLab, UI_S, 16, 10);

  int due = wordDueCount();
  if (wordPos >= wordQueueN) {
    // 成绩页
    drawText(180, 112, "本轮完成", C_YELLOW, 24, 1, 1);
    char s1[32]; snprintf(s1, sizeof(s1), "对 %d  错 %d", wordRight, wordWrong);
    drawText(180, 158, s1, C_WHITE, 16, 1, 1);
    char s2[12]; snprintf(s2, sizeof(s2), "%d", due);
    drawText(136, 186, "待复习", COL(140, 200, 170), 16, 1, 0);
    drawText(194, 186, s2, COL(140, 200, 170), 16, 1, 0);
    int total = wordRight + wordWrong;
    int pct = total ? (wordRight * 100 / total) : 0;
    char s3[24]; snprintf(s3, sizeof(s3), "%d%%", pct);
    drawText(180, 222, s3, pct >= 80 ? C_GREEN : C_ORANGE, 24, 1, 1);
    uiBtn(180, 284, 28, "再来", UI_A);
    return;
  }

  // 状态行：1/100 · 待复习 N · 第 S 阶
  // 原来这里还有一条 240px 进度条（y=92），实机上它被胶囊和状态行夹成一条缝，
  // 信息又与左边的 "n/N" 重复 —— 去掉，纵向空间全让给文字。
  char ip[16], id[12], is[12];
  snprintf(ip, sizeof(ip), "%d/%d", wordPos + 1, wordQueueN);
  snprintf(id, sizeof(id), "%d", due);
  snprintf(is, sizeof(is), "%d", wordLastStage + 1);
  int x2 = 72;
  drawText(x2, 104, ip,      C_GREY,   16, 1, 0); x2 = wordInk(ip,      x2, 10);
  drawText(x2, 104, "待复习", C_GREY,   16, 1, 0); x2 = wordInk("待复习", x2, 8);
  drawText(x2, 104, id,      C_ORANGE, 16, 1, 0); x2 = wordInk(id,      x2, 12);
  drawText(x2, 104, "第",     C_GREY,   16, 1, 0); x2 = wordInk("第",     x2, 6);
  drawText(x2, 104, is,      C_YELLOW, 16, 1, 0); x2 = wordInk(is,      x2, 6);
  drawText(x2, 104, "阶",     C_GREY,   16, 1, 0);

  const WordEntry *tab = wordTable();
  int ans = wordOpts[wordAnsSlot];
  // v2.8：衬线英文（Georgia 30px，4-bit AA）。baselineY=154，降部落到 159，
  // 下一行释义在 166，正好留 7px 空隙；点这行文字可手动重播发音。
  drawSerifText(180, 154, tab[ans].en, C_WHITE);
  drawPronIcon(254, 62);
  drawText(180, 166, tab[ans].ex, COL(150, 166, 188), 12, 1, 1);

  // 四个选项：2×2
  static const int OX[4] = {107, 253, 107, 253};
  static const int OY[4] = {204, 204, 258, 258};
  for (int i = 0; i < 4; i++) {
    int st = UI_N;
    if (wordPicked) {
      if (i == wordAnsSlot)       st = UI_G;
      else if (i == wordPickSlot) st = UI_D;
    }
    uiPill(OX[i] - 67, OY[i] - 21, 135, 42, tab[wordOpts[i]].cn, st);
  }
  // 三字中文 48px 宽：r22 时在字形上下缘圆内只剩 41px，会被圆边切掉。
  // 两态统一 r27（底部 310+27 距圆心 157 < 158 安全线），切换时按钮也不跳动。
  uiBtn(180, 310, 27, wordPicked ? "下一题" : "跳过", wordPicked ? UI_A : UI_N);
}

// 8 宫格选词书：4 列 × 2 行，圆键 r28，圆心距 57 不重叠
static void renderWordPicker() {
  uiPage("选词书");
  static const int PX[4] = {103, 160, 217, 274};
  static const int PY[2] = {150, 230};
  for (int b = 0; b < WORD_BOOK_N; b++) {
    int cx = PX[b % 4], cy = PY[b / 4];
    bool on = (b == wordBook);
    uiBtn(cx, cy, 28, WORD_BOOK_NAMES[b], on ? UI_S : UI_N,
          strcmp(WORD_BOOK_NAMES[b], "GRE") == 0 ? 12 : 16);
  }
  int total = 0;
  for (int b = 0; b < WORD_BOOK_N; b++) total += WORD_TABLE_N[b];
  char tot[24]; snprintf(tot, sizeof(tot), "共 %d 词", total);
  drawText(180, 296, tot, C_GREY, 16, 1, 1);
}

static void wordTap(int x, int y) {
  // 选词书子页：点圆键切书并重置进度（退出子页靠滑动，main.cpp pollTouch 统一处理）
  if (wordPicking) {
    static const int PX[4] = {103, 160, 217, 274};
    static const int PY[2] = {150, 230};
    for (int b = 0; b < WORD_BOOK_N; b++) {
      if (hit(x, y, PX[b % 4], PY[b / 4], 34)) {
        if (b != wordBook) {
          wordBook = b; wordBookPage = b / 4;
          wordReset(false); wordSave(); showToast(WORD_BOOK_NAMES[b]);
          wordPronounce();                       // 换书后自动读一遍新词
        }
        wordPicking = false; render(); return;
      }
    }
    return;
  }
  // v2.8：右上角小喇叭 = 发音开关；开的时候顺手读一遍当前词
  if (hit(x, y, 254, 62, 18)) {
    wordPronLoad();
    gWordPronOn = !gWordPronOn;
    cfgSetInt("wd.pron", gWordPronOn ? 1 : 0);
    showToast(gWordPronOn ? "PRON ON" : "PRON OFF");
    if (gWordPronOn) wordPronounce(true);
    render(); return;
  }
  // 主页面：顶部「当前 xx」胶囊（(128,66) 104x30，热区外扩 6px，手指好按）
  if (x >= 122 && x <= 238 && y >= 60 && y <= 102) { wordPicking = true; render(); return; }
  if (wordPos >= wordQueueN) {
    if (hit(x, y, 180, 284, 34)) { wordReset(false); render(); }
    return;
  }
  // 点英文单词本身 = 重播（手动点不受开关限制）
  if (y >= 128 && y <= 162 && x >= 40 && x <= 320) { wordPronounce(true); return; }
  if (!wordPicked) {
    static const int OX[4] = {107, 253, 107, 253};
    static const int OY[4] = {204, 204, 258, 258};
    for (int i = 0; i < 4; i++) {
      if (x >= OX[i] - 70 && x <= OX[i] + 70 && y >= OY[i] - 25 && y <= OY[i] + 25) {
        wordPicked = true;
        wordWasRight = (i == wordAnsSlot);
        wordPickSlot = i;
        if (wordWasRight) wordRight++; else wordWrong++;
        wordGrade(wordWasRight);
        wordSave();
        render(); return;
      }
    }
  }
  // 底部按钮：答了是下一题，没答是跳过（跳过按答错处理，5 分钟后再来）
  if (hit(x, y, 180, 310, 30)) {
    if (!wordPicked) { wordWrong++; wordGrade(false); }
    wordPos++;
    wordSave();
    if (wordPos < wordQueueN) { wordNextQuestion(); wordPronounce(); }
    else { wordQueueN = wordPos; }
    render();
  }
}

static void wordCmd(const String &cmd) {
  if (cmd == "WORD") {
    int gi = wordPos < wordQueueN
           ? wordGlobalIndex(wordBook, wordOpts[wordAnsSlot]) : -1;
    Serial.printf("[WORD] book=%d(%s) pos=%d/%d due=%d right=%d wrong=%d stage=%d\n",
                  wordBook, WORD_BOOK_NAMES[wordBook], wordPos, wordQueueN,
                  wordDueCount(), wordRight, wordWrong,
                  gi < 0 ? -1 : gWordSrs[gi].stage);
    if (gi >= 0)
      Serial.printf("[WORD] now=%s\n", wordTable()[wordOpts[wordAnsSlot]].en);
  } else if (cmd == "WORDDUE") {
    Serial.printf("[WORD] due=%d total=%d free=%d\n",
                  wordDueCount(), wordTableN(), wordFreeMode ? 1 : 0);
  } else if (cmd == "WORDRESET") {
    // 清掉当前词库的复习进度，重新按到期队列出题（便于验收）
    int n = wordTableN();
    for (int i = 0; i < n; i++) {
      WordSRS &s = gWordSrs[wordGlobalIndex(wordBook, i)];
      s.stage = 0; s.next = 0;
    }
    wordSrsSave();
    wordReset(false); render();
    Serial.println("[WORD] SRS progress reset");
  } else if (cmd == "WORDSET") {
    // WORDSET <书号0-7> 直接切词库
    int b = cmd.substring(8).toInt();
    if (b >= 0 && b < WORD_BOOK_N) {
      wordBook = b; wordBookPage = b / 4; wordReset(false); render();
      Serial.printf("[WORD] book=%s\n", WORD_BOOK_NAMES[b]);
    }
  } else if (cmd == "PRON") {
    wordPronLoad();
    Serial.printf("[PRON] switch=%d\n", (int)gWordPronOn);
    wordPronounce(true);                 // 强制读一遍，方便无手指验收
  } else if (cmd.startsWith("PRON ")) {
    int v = cmd.substring(5).toInt();
    wordPronLoad(); gWordPronOn = v ? true : false;
    cfgSetInt("wd.pron", gWordPronOn ? 1 : 0);
    Serial.printf("[PRON] switch -> %d\n", (int)gWordPronOn);
    render();
  }
}
