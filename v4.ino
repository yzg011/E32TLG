  /* ============================================================================
   E32TLG 中文版 v2‑HTTPClient 长轮询版 + 标题栏时分显示
   ESP32 + SH1106 128x64 (I2C) + 旋转编码器 + Telegram 家庭留言机
   ✔ 完全移除 UniversalTelegramBot，使用 HTTPClient 实现 getUpdates 长轮询
   ✔ 无Webhook、不需要证书；超时保护，超长消息/图片不会卡死后台任务
   ✔ 保留全部原有UI、按键、WiFiManager配置、NVS存储、蜂鸣器
   ✔ 新增：标题栏 HH:MM 网络时间，放在标题文字和右上角图标中间
   依赖库
     U8g2 >= 2.35 / WiFiManager >= 2.0.17 / ArduinoJson >= 6
   分区表：Huge APP (3MB No OTA/1MB SPIFFS)
   字库：u8g2_font_wqy12_t_gb2312 或 u8g2_font_wqy12_t_gb2312a(更小111KB)
   ============================================================================ */
#include <Wire.h>
#include <U8g2lib.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <WiFiManager.h>
#include <time.h>

/* ---------------------------------------------------------------------------
   引脚定义
   --------------------------------------------------------------------------- */
#define ENC_A        18
#define ENC_B        19
#define ENC_BTN       5
#define BTN_BACK      2
#define BTN_CONFIRM   4
#define BUZZER       23

/* ---------------------------------------------------------------------------
   基本参数
   --------------------------------------------------------------------------- */
#define FONT_CN          u8g2_font_wqy12_t_gb2312
#define AP_ADI           "E32TLG-Setup"
#define KISI_SAYISI      4
#define TELEGRAM_ARALIK  2000      // 轮询间隔
#define CIFT_TIK_ARALIK  300       // 双击判定窗口
#define UZUN_BAS_SURE    2000      // 确认键长按
#define GERI_UZUN_SURE   3000      // 返回键长按
#define TUS_DEBOUNCE     15        // 按键去抖
#define BILDIRIM_SURE    5000      // 新消息提示时长
#define KAYMA_ARALIK     300       // 长文本滚动间隔
#define TUSBAS_TEKRAR    60        // 编码器键防连击
#define TZ_OFFSET        28800     // UTC+8 8*3600

WiFiClientSecure client;
int64_t tg_last_update_id = 0;
volatile bool tgReInit = false;

U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);
Preferences preferences;
WiFiManager wm;

bool encOnceki = false;
bool backOnceki = false;
bool confOnceki = false;

unsigned long sonSaatCizim = 0;

// 时间
struct tm timeinfo;
bool timeSynced = false;

/* ---------------------------------------------------------------------------
   机器人参数（NVS 命名空间 e32tlg）
   --------------------------------------------------------------------------- */
String botToken = "";
String chatIDleri[KISI_SAYISI]   = {"", "", "", ""};
String kisiIsimleri[KISI_SAYISI] = {"妈妈", "爸爸", "姐姐", "家庭群"};

WiFiManagerParameter *pToken = nullptr;
WiFiManagerParameter *pChat[KISI_SAYISI];
WiFiManagerParameter *pIsim[KISI_SAYISI];

const char *PARAM_ID_CHAT[KISI_SAYISI]  = {"c1", "c2", "c3", "c4"};
const char *PARAM_ID_ISIM[KISI_SAYISI]  = {"n1", "n2", "n3", "n4"};
const char *PARAM_LBL_CHAT[KISI_SAYISI] = {
  "联系人1 Chat ID", "联系人2 Chat ID", "联系人3 Chat ID", "家庭群 Chat ID"
};
const char *PARAM_LBL_ISIM[KISI_SAYISI] = {
  "联系人1 名称", "联系人2 名称", "联系人3 名称", "家庭群 名称"
};

/* ---------------------------------------------------------------------------
   输入字符 / 快捷消息
   --------------------------------------------------------------------------- */
const char *harfler[] = {
  "a", "b", "c", "d", "e", "f", "g", "h", "i", "j",
  "k", "l", "m", "n", "o", "p", "q", "r", "s", "t",
  "u", "v", "w", "x", "y", "z",
  "0", "1", "2", "3", "4", "5", "6", "7", "8", "9",
  " ", ".", "-", "!", "?"
};
int harfSayisi = sizeof(harfler) / sizeof(harfler[0]);

const char *hazirMesajlar[] = {
  "好", "可以", "不行", "我出门了马上到",
  "你在哪里", "我在等你", "等一下马上来"
};
int hazirMesajSayisi = sizeof(hazirMesajlar) / sizeof(hazirMesajlar[0]);

/* ---------------------------------------------------------------------------
   运行状态
   --------------------------------------------------------------------------- */
int seciliKisi = 0;
String aktifChatID = "";
bool kisiSecildi = false;
int mod = 0;              // 0 = 编写消息，1 = 快捷消息
int harfIndex = 0;
String mesaj = "";

volatile int enkoderDegisim = 0;
volatile int encA_son = HIGH;

String mesajGecmisi[KISI_SAYISI][4];
String mesajTipleri[KISI_SAYISI][4];

volatile bool telegramBagli = false;
volatile unsigned long sonBasariliIstek = 0;

int kaymaOffset0 = 0;
int kaymaOffset1 = 0;
unsigned long sonKaymaZamani = 0;
unsigned long sonCizim = 0;
int hazirMesajIndex = 0;

volatile bool bildirimVar = false;
volatile unsigned long bildirimZamani = 0;
unsigned long sonEncAksiyon = 0;
unsigned long sonConfirmZamani = 0;

/* ---------------------------------------------------------------------------
   线程间队列（UI <-> Telegram 任务）
   --------------------------------------------------------------------------- */
struct IsPaketi {
  int    kisi;
  String metin;
};
QueueHandle_t kuyrukGelen = nullptr;    // 后台收到的消息
QueueHandle_t kuyrukGiden = nullptr;    // 待发送的消息

volatile bool tgYeniden = false;
volatile bool tgHazir   = false;
volatile bool gonderimBitti = false;
volatile bool gonderimBasarili = false;

/* =========================================================================
   URL编码，用于发送消息POST payload
   ========================================================================= */
String urlEncode(String s) {
  String out;
  for (char c : s) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
        || c == '-' || c == '_' || c == '.' || c == '~') {
      out += c;
    } else {
      out += '%';
      char buf[3];
      sprintf(buf, "%02X", (uint8_t)c);
      out += buf;
    }
  }
  return out;
}

// 获取 HH:MM 时间字符串
String getTimeStr() {
  if (!timeSynced || !getLocalTime(&timeinfo)) {
    return "--:--";
  }
  char buf[16];
  strftime(buf, sizeof(buf), "%H:%M", &timeinfo);
  return String(buf);
}

/* =========================================================================
   一、机器人参数存取
   ========================================================================= */
void ayarlariYukle() {
  botToken = preferences.getString("token", "");
  for (int i = 0; i < KISI_SAYISI; i++) {
    chatIDleri[i]   = preferences.getString(PARAM_ID_CHAT[i], "");
    kisiIsimleri[i] = preferences.getString(PARAM_ID_ISIM[i], kisiIsimleri[i]);
    if (kisiIsimleri[i].length() == 0) kisiIsimleri[i] = "联系人" + String(i + 1);
  }
}

void kaydetParametreler() {
  if (pToken != nullptr) botToken = String(pToken->getValue());
  for (int i = 0; i < KISI_SAYISI; i++) {
    if (pChat[i] != nullptr) chatIDleri[i]   = String(pChat[i]->getValue());
    if (pIsim[i] != nullptr) kisiIsimleri[i] = String(pIsim[i]->getValue());
  }
  botToken.trim();
  for (int i = 0; i < KISI_SAYISI; i++) {
    chatIDleri[i].trim();
    kisiIsimleri[i].trim();
    if (kisiIsimleri[i].length() == 0) kisiIsimleri[i] = "联系人" + String(i + 1);
  }
  preferences.putString("token", botToken);
  for (int i = 0; i < KISI_SAYISI; i++) {
    preferences.putString(PARAM_ID_CHAT[i], chatIDleri[i]);
    preferences.putString(PARAM_ID_ISIM[i], kisiIsimleri[i]);
  }
}

/* =========================================================================
   二、Telegram 后台任务（运行在 core 0，HTTPClient getUpdates长轮询）
   ========================================================================= */
int chatIDdenKisiIndex(String chatID, String fromID) {
  for (int i = 0; i < KISI_SAYISI; i++) {
    if (chatIDleri[i].length() > 0 && chatID == chatIDleri[i]) return i;
  }
  if (chatIDleri[3].length() > 0 && chatID == chatIDleri[3]) return 3;
  for (int i = 0; i < 3; i++) {
    if (chatIDleri[i].length() > 0 && fromID == chatIDleri[i]) return 3;
  }
  return -1;
}

void tgGorevi(void *param) {
  unsigned long sonPoll = 0;
  int pollFailCnt = 0;
  HTTPClient http;
  WiFiClientSecure tgClient;
  tgClient.setInsecure();

  for (;;) {
    if(tgYeniden || tgReInit){
      tgYeniden = false;
      tgReInit = false;
      tgHazir = false;
      pollFailCnt = 0;
      tg_last_update_id = 0;
      telegramBagli = false;
      if(botToken.length() < 20){
        tgHazir = true;
        vTaskDelay(pdMS_TO_TICKS(500));
        continue;
      }
      tgHazir = true;
      sonPoll = millis();
    }

    if (!tgHazir || botToken.length()<20) {
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }

    if(pollFailCnt >=5){
      Serial.println(F("[TG]连续失败"));
      pollFailCnt = 0;
      vTaskDelay(pdMS_TO_TICKS(300));
      continue;
    }

    /* ---- 处理待发送消息队列 ---- */
    IsPaketi *s = nullptr;
    if (xQueueReceive(kuyrukGiden, &s, 0) == pdTRUE) {
      bool ok = false;
      if(chatIDleri[s->kisi].length()>0){
        String url = "https://tg.z2m.store/bot" + botToken + "/sendMessage";
        http.begin(tgClient, url);
        http.addHeader("Content-Type","application/x-www-form-urlencoded");
        String payload = "chat_id="+chatIDleri[s->kisi]+"&text="+urlEncode(s->metin);
        int httpCode = http.POST(payload);
        if(httpCode == 200){
          ok=true;
          sonBasariliIstek = millis();
          pollFailCnt=0;
        }else{
          pollFailCnt++;
        }
        http.end();
      }
      gonderimBasarili = ok;
      gonderimBitti = true;
      delete s;
      continue;
    }

    /* ---- getUpdates 长轮询接收消息 ---- */
    if (WiFi.status() == WL_CONNECTED && millis() - sonPoll >= TELEGRAM_ARALIK) {
      sonPoll = millis();
      bool pollOK = false;
      unsigned long pollStart = millis();

      String url = "https://tg.z2m.store/bot"+botToken+"/getUpdates?timeout=2&offset="+String(tg_last_update_id);
      http.begin(tgClient,url);
      http.setTimeout(4000);
      int httpCode = http.GET();

      if(millis()-pollStart >=4000){
        http.end();
        pollFailCnt++;
        continue;
      }

      if(httpCode == 200){
        pollOK = true;
        pollFailCnt = 0;
        String payload = http.getString();
        DynamicJsonDocument doc(4096);
        DeserializationError err = deserializeJson(doc,payload);
        if(!err && doc["ok"]==true){
          JsonArray res = doc["result"];
          for(JsonObject item : res){
            int64_t upd_id = item["update_id"];
            tg_last_update_id = upd_id + 1;

            if(!item.containsKey("message")) continue;
            JsonObject msg = item["message"];
            if(!msg.containsKey("text")) continue;

            String chatIdStr = msg["chat"]["id"].as<String>();
            String fromIdStr = msg["from"]["id"].as<String>();
            String textRaw = msg["text"].as<String>();

            if(textRaw.length()>180) textRaw = textRaw.substring(0,180);

            int idx = chatIDdenKisiIndex(chatIdStr, fromIdStr);
            if(idx < 0) continue;

            IsPaketi *g = new IsPaketi();
            g->kisi = idx;
            g->metin = textRaw;
            if(xQueueSend(kuyrukGelen,&g,0) != pdTRUE){
              delete g;
            }
          }
          telegramBagli = true;
          sonBasariliIstek = millis();
        }else{
          pollFailCnt++;
        }
      }else{
        pollFailCnt++;
      }
      http.end();
    }

    if(millis() - sonBasariliIstek >15000){
      telegramBagli = false;
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

/* =========================================================================
   三、显示辅助
   ========================================================================= */
int metinGenisligi(const String &s) {
  return u8g2.getUTF8Width(s.c_str());
}

void kayanYaz(int y, const String &s, int ofs) {
  if (s.length() == 0) return;
  int w = metinGenisligi(s);
  if (w <= 126) {
    u8g2.drawUTF8(0, y, s.c_str());
    return;
  }
  String tekrar = s + "     ";
  int wt = metinGenisligi(tekrar);
  int y0 = (y - 11 < 0) ? 0 : (y - 11);
  u8g2.setClipWindow(0, y0, 128, y + 2);
  u8g2.drawUTF8(-ofs, y, tekrar.c_str());
  u8g2.drawUTF8(-ofs + wt, y, tekrar.c_str());
  u8g2.setMaxClipWindow();
}

void girisYaz(int y, const String &s) {
  int w = metinGenisligi(s);
  int x = (w <= 126) ? 0 : (126 - w + 2);
  u8g2.setClipWindow(0, y - 11, 128, 64);
  u8g2.drawUTF8(x, y, s.c_str());
  u8g2.setMaxClipWindow();
}

void ekranMesaj(const String &satir1, const String &satir2) {
  u8g2.clearBuffer();
  u8g2.setFont(FONT_CN);
  u8g2.drawUTF8(0, 11, "E32TLG");
  u8g2.drawLine(0, 14, 127, 14);
  if (satir1.length()) kayanYaz(30, satir1, 0);
  if (satir2.length()) kayanYaz(46, satir2, 0);
  u8g2.sendBuffer();
  sonCizim = millis();
}

/* =========================================================================
   四、状态图标
   ========================================================================= */
void wifiIkonu(int x, bool bagli) {
  int yukseklikler[] = {3, 5, 7, 9};
  int rssi = WiFi.RSSI();
  int doluCubuk = 0;
  if (bagli) {
    if      (rssi >= -55) doluCubuk = 4;
    else if (rssi >= -65) doluCubuk = 3;
    else if (rssi >= -75) doluCubuk = 2;
    else                  doluCubuk = 1;
  }
  for (int i = 0; i < 4; i++) {
    int cx = x + i * 4;
    int cy = 11;
    int h  = yukseklikler[i];
    if (i < doluCubuk) u8g2.drawBox(cx, cy - h, 3, h);
    else               u8g2.drawFrame(cx, cy - h, 3, h);
  }
  if (!bagli) u8g2.drawStr(x + 18, 10, "!");
}

void telegramIkonu(int x, bool bagli) {
  if (bagli) {
    u8g2.drawLine(x,     6, x + 3, 10);
    u8g2.drawLine(x + 1, 6, x + 4, 10);
    u8g2.drawLine(x + 3, 10, x + 9, 2);
    u8g2.drawLine(x + 4, 10, x + 10, 2);
  } else {
    u8g2.drawLine(x,     2, x + 9, 11);
    u8g2.drawLine(x + 1, 2, x + 10, 11);
    u8g2.drawLine(x + 9, 2, x, 11);
    u8g2.drawLine(x + 10, 2, x + 1, 11);
  }
}

void mektupIkonu(int x) {
  u8g2.drawFrame(x, 3, 12, 8);
  u8g2.drawLine(x, 3, x + 6, 8);
  u8g2.drawLine(x + 12, 3, x + 6, 8);
}

/* =========================================================================
   五、界面
   ========================================================================= */
void kisiSecimEkrani() {
  u8g2.clearBuffer();
  u8g2.setFont(FONT_CN);
  u8g2.setDrawColor(1);
  u8g2.drawUTF8(0, 11, "E32TLG");
  // 选人界面也加上时间
  String t = getTimeStr();
  int tw = metinGenisligi(t);
  u8g2.drawUTF8(128 - tw - 2, 11, t.c_str());
  u8g2.drawLine(0, 14, 127, 14);
  u8g2.drawUTF8(0, 26, "发送给谁");
  if (seciliKisi > 0)
    u8g2.drawUTF8(3, 39, kisiIsimleri[seciliKisi - 1].c_str());
  u8g2.drawBox(0, 41, 128, 13);
  u8g2.setDrawColor(0);
  String seciliAd = kisiIsimleri[seciliKisi];
  if (seciliKisi == 3) seciliAd = "* " + seciliAd;
  u8g2.drawUTF8(3, 51, seciliAd.c_str());
  u8g2.setDrawColor(1);
  if (seciliKisi < KISI_SAYISI - 1)
    u8g2.drawUTF8(3, 63, kisiIsimleri[seciliKisi + 1].c_str());
  u8g2.sendBuffer();
  sonCizim = millis();
}

void apCallback(WiFiManager *m) {
  u8g2.clearBuffer();
  u8g2.setFont(FONT_CN);
  u8g2.drawUTF8(0, 11, "配置模式");
  u8g2.drawLine(0, 14, 127, 14);
  u8g2.drawUTF8(0, 28, "热点 " AP_ADI);
  u8g2.drawUTF8(0, 44, "打开 192.168.4.1");
  u8g2.drawUTF8(0, 58, "配置WiFi和机器人");
  u8g2.sendBuffer();
  sonCizim = millis();
}

String mesajSatiri(int idx) {
  String t = mesajGecmisi[seciliKisi][idx];
  if (t.length() == 0) return "";
  String p = (mesajTipleri[seciliKisi][idx] == "ben") ? "我> " : "对< ";
  return p + t;
}

void ekraniGuncelle() {
  u8g2.clearBuffer();
  u8g2.setFont(FONT_CN);
  u8g2.setDrawColor(1);
  String baslik = "> " + kisiIsimleri[seciliKisi];
  String timestr = getTimeStr();
  int titleW = metinGenisligi(baslik);
  int timeW  = metinGenisligi(timestr);

  // 先画标题文字
  u8g2.drawUTF8(0, 11, baslik.c_str());
  // 时间放在标题后面，留一点空隙
  int timeX = titleW + 4;
  // 防止时间跑到图标区域，超过就不画时间
  if(timeX + timeW < 84){
    u8g2.drawUTF8((128-timeW)/2, 11, timestr.c_str());
  }

  if (bildirimVar && millis() - bildirimZamani < BILDIRIM_SURE) {
    if ((millis() - bildirimZamani) % 400 < 200) mektupIkonu(114);
  } else {
    wifiIkonu(88, WiFi.status() == WL_CONNECTED);
    if (WiFi.status() == WL_CONNECTED) telegramIkonu(112, telegramBagli);
  }
  u8g2.drawLine(0, 14, 127, 14);
  if (mod == 0) {
    String s1 = mesajSatiri(1);
    if (s1.length()) kayanYaz(26, s1, kaymaOffset1);
    String s0 = mesajSatiri(0);
    if (s0.length()) kayanYaz(39, s0, kaymaOffset0);
    u8g2.drawLine(0, 50, 127, 50);
    girisYaz(62, ">" + mesaj + String(harfler[harfIndex]));
  } else {
    u8g2.drawUTF8(0, 26, "快捷消息");
    u8g2.drawLine(0, 29, 127, 29);
    if (hazirMesajIndex > 0)
      kayanYaz(39, String(hazirMesajlar[hazirMesajIndex - 1]), 0);
    u8g2.drawBox(0, 41, 128, 13);
    u8g2.setDrawColor(0);
    u8g2.drawUTF8(3, 51, hazirMesajlar[hazirMesajIndex]);
    u8g2.setDrawColor(1);
    if (hazirMesajIndex < hazirMesajSayisi - 1)
      kayanYaz(63, String(hazirMesajlar[hazirMesajIndex + 1]), 0);
  }
  u8g2.sendBuffer();
  sonCizim = millis();
  sonSaatCizim = millis();
}

/* =========================================================================
   六、非阻塞蜂鸣器
   ========================================================================= */
int bipTip = 0;
uint8_t bipAdim = 0;
unsigned long bipSonraki = 0;

void bipBaslat(uint8_t tip) {
  bipTip = tip;
  bipAdim = 1;
  if (tip == 1)      tone(BUZZER, 800, 90);
  else if (tip == 2) tone(BUZZER, 1000, 90);
  bipSonraki = millis() + 200;
}

void bipDongu() {
  if (bipAdim == 0) return;
  if ((long)(millis() - bipSonraki) < 0) return;
  if (bipTip == 1) {
    if (bipAdim == 1) {
      tone(BUZZER, 800, 90);
      bipAdim = 2;
      bipSonraki = millis() + 200;
    } else if (bipAdim == 2) {
      tone(BUZZER, 1200, 140);
      bipAdim = 0;
    } else {
      bipAdim = 0;
    }
  } else {
    if (bipAdim == 1) {
      tone(BUZZER, 1500, 90);
      bipAdim = 0;
    } else {
      bipAdim = 0;
    }
  }
}

/* =========================================================================
   七、消息处理
   ========================================================================= */
bool aktifKisidanMi(int kisiIndex) {
  return kisiIndex == seciliKisi;
}

void mesajEkle(String mesajMetni, String tip, int kisiIndex) {
  if (kisiIndex < 0 || kisiIndex >= KISI_SAYISI) return;
  mesajMetni.replace("\r", " ");
  mesajMetni.replace("\n", " ");
  mesajMetni.replace("\t", " ");
  if (mesajMetni.length() > 200) mesajMetni = mesajMetni.substring(0, 200);
  for (int i = 3; i > 0; i--) {
    mesajGecmisi[kisiIndex][i] = mesajGecmisi[kisiIndex][i - 1];
    mesajTipleri[kisiIndex][i] = mesajTipleri[kisiIndex][i - 1];
  }
  mesajGecmisi[kisiIndex][0] = mesajMetni;
  mesajTipleri[kisiIndex][0] = tip;
  if (kisiIndex == seciliKisi) {
    kaymaOffset0 = 0;
    kaymaOffset1 = 0;
  }
}

void mesajGonder(String m) {
  if (m.length() == 0) return;
  if (!tgHazir || botToken.length() < 20) {
    ekranMesaj("机器人未就绪", "长按返回键重新配置");
    delay(900);
    ekraniGuncelle();
    return;
  }
  if (aktifChatID.length() == 0) {
    ekranMesaj("该联系人无 Chat ID", "长按返回键配置");
    delay(900);
    ekraniGuncelle();
    return;
  }
  mesajEkle(m, "ben", seciliKisi);
  IsPaketi *p = new IsPaketi();
  p->kisi  = seciliKisi;
  p->metin = m;
  if (xQueueSend(kuyrukGiden, &p, 0) != pdTRUE) {
    delete p;
    ekranMesaj("发送失败", "队列已满稍后再试");
    delay(900);
  }
  bipBaslat(2);
  ekraniGuncelle();
}

void kuyruguIsle() {
  IsPaketi *g = nullptr;
  bool yeniVar = false;
  while (xQueueReceive(kuyrukGelen, &g, 0) == pdTRUE) {
    if (g->metin.length() > 0) {
      mesajEkle(g->metin, "o", g->kisi);
      if (g->kisi != seciliKisi) {
        bildirimVar = true;
        bildirimZamani = millis();
      }
      yeniVar = true;
    }
    delete g;
  }
  if (yeniVar) {
    bipBaslat(1);
    if (kisiSecildi) ekraniGuncelle();
  }
  if (gonderimBitti) {
    gonderimBitti = false;
    if (!gonderimBasarili) {
      ekranMesaj("发送失败", "请检查网络或 Chat ID");
      delay(900);
      ekraniGuncelle();
    }
  }
}

/* =========================================================================
   八、编码器中断
   ========================================================================= */
void IRAM_ATTR enkoderISR() {
  int a = digitalRead(ENC_A);
  int b = digitalRead(ENC_B);
  if (encA_son == HIGH && a == LOW) {
    if (b == HIGH) enkoderDegisim++;
    else           enkoderDegisim--;
  }
  encA_son = a;
}

/* =========================================================================
   九、非阻塞按键
   ========================================================================= */
#define TUS_ENC   0
#define TUS_BACK  1
#define TUS_CONF  2
uint8_t       tusPin[3]       = {ENC_BTN, BTN_BACK, BTN_CONFIRM};
bool          tusRaw[3]       = {false, false, false};
unsigned long tusRawZaman[3]  = {0, 0, 0};
bool          tusDurum[3]     = {false, false, false};
unsigned long tusBasZaman[3]  = {0, 0, 0};
bool          tusUzun[3]      = {false, false, false};

bool tusOku(int idx) {
  bool raw = (digitalRead(tusPin[idx]) == LOW);
  if (raw != tusRaw[idx]) {
    tusRaw[idx] = raw;
    tusRawZaman[idx] = millis();
  }
  if (tusDurum[idx] != tusRaw[idx] && millis() - tusRawZaman[idx] >= TUS_DEBOUNCE) {
    tusDurum[idx] = tusRaw[idx];
    if (tusDurum[idx]) {
      tusBasZaman[idx] = millis();
      tusUzun[idx] = false;
    }
  }
  return tusDurum[idx];
}

/* =========================================================================
   十、WiFi + 参数配置
   ========================================================================= */
void portalBaslat() {
  ekranMesaj("进入配置模式", "热点 " AP_ADI);
  delay(200);
  wm.setConfigPortalTimeout(300);
  wm.startConfigPortal(AP_ADI);
  ayarlariYukle();
  tgReInit = true;
  Serial.println("[WiFi] 配置门户结束");
}

void wifiBaslat() {
  ekranMesaj("正在连接网络", "请稍候");
  delay(300);
  wm.setDebugOutput(false);
  wm.setConnectTimeout(20);
  wm.setConfigPortalTimeout(300);
  wm.setAPCallback(apCallback);
  wm.setSaveParamsCallback(kaydetParametreler);
  wm.setBreakAfterConfig(true);
  wm.setTitle("E32TLG Setup");
  wm.setCustomHeadElement("<meta charset=\"utf-8\">");

  pToken = new WiFiManagerParameter("token", "机器人 Token (Bot Token)", botToken.c_str(), 100);
  wm.addParameter(pToken);
  for (int i = 0; i < KISI_SAYISI; i++) {
    pChat[i] = new WiFiManagerParameter(PARAM_ID_CHAT[i], PARAM_LBL_CHAT[i],
                                        chatIDleri[i].c_str(), 40);
    pIsim[i] = new WiFiManagerParameter(PARAM_ID_ISIM[i], PARAM_LBL_ISIM[i],
                                        kisiIsimleri[i].c_str(), 24);
    wm.addParameter(pChat[i]);
    wm.addParameter(pIsim[i]);
  }

  if (!wm.autoConnect(AP_ADI)) {
    ekranMesaj("WiFi连接失败", "即将重启");
    delay(3000);
    ESP.restart();
  }
  kaydetParametreler();
  String ip = WiFi.localIP().toString();
  ekranMesaj("已连接 " + WiFi.SSID(), ip);
  Serial.println("[WiFi] " + WiFi.SSID() + " -> " + ip);

  // NTP 同步时间
  configTime(TZ_OFFSET, 0, "ntp.aliyun.com", "time.windows.com");
  timeSynced = false;
  for(int i=0;i<10;i++){
    if(getLocalTime(&timeinfo)){
      timeSynced = true;
      break;
    }
    delay(500);
  }
  if(timeSynced) Serial.println("[NTP] 时间同步成功");
  else Serial.println("[NTP] 时间同步失败");

  tgReInit = true;
  delay(500);
}

/* =========================================================================
   十一、setup
   ========================================================================= */
void setup() {
  Serial.begin(115200);
  Wire.begin(21, 22);
  Wire.setClock(400000);
  u8g2.begin();
  u8g2.setFont(FONT_CN);

  pinMode(ENC_A,       INPUT_PULLUP);
  pinMode(ENC_B,       INPUT_PULLUP);
  pinMode(ENC_BTN,     INPUT_PULLUP);
  pinMode(BTN_BACK,    INPUT_PULLUP);
  pinMode(BTN_CONFIRM, INPUT_PULLUP);
  pinMode(BUZZER,      OUTPUT);

  tone(BUZZER, 1500, 60);
  attachInterrupt(digitalPinToInterrupt(ENC_A), enkoderISR, CHANGE);

  preferences.begin("e32tlg", false);
  ayarlariYukle();

  kuyrukGelen = xQueueCreate(6, sizeof(IsPaketi *));
  kuyrukGiden = xQueueCreate(4, sizeof(IsPaketi *));
  xTaskCreatePinnedToCore(tgGorevi, "telegram", 12288, nullptr, 1, nullptr, 0);

  ekranMesaj("正在启动", "读取参数");
  delay(300);
  wifiBaslat();

  if (botToken.length() < 20) {
    portalBaslat();
    ayarlariYukle();
  }

  if (botToken.length() < 20) {
    ekranMesaj("未配置机器人", "长按返回键进入配置");
    delay(1500);
  }

  seciliKisi   = 0;
  kisiSecildi  = false;
  enkoderDegisim = 0;
  encOnceki = backOnceki = confOnceki = false;
  kisiSecimEkrani();
}

/* =========================================================================
   十二、loop
   ========================================================================= */
void loop() {
  bool e = tusOku(TUS_ENC);
  bool b = tusOku(TUS_BACK);
  bool c = tusOku(TUS_CONF);

  kuyruguIsle();
  bipDongu();

  /* ---------------- 选联系人 ---------------- */
  if (!kisiSecildi) {
    if (enkoderDegisim != 0) {
      if (enkoderDegisim > 0) seciliKisi = (seciliKisi + 1) % KISI_SAYISI;
      else                    seciliKisi = (seciliKisi - 1 + KISI_SAYISI) % KISI_SAYISI;
      enkoderDegisim = 0;
      kisiSecimEkrani();
    }
    if ((e && !encOnceki) || (c && !confOnceki)) {
      aktifChatID = chatIDleri[seciliKisi];
      kisiSecildi = true;
      kaymaOffset0 = 0;
      kaymaOffset1 = 0;
      enkoderDegisim = 0;
      if (aktifChatID.length() == 0) {
        ekranMesaj("该联系人无 Chat ID", "长按返回键配置");
        delay(800);
      }
      ekraniGuncelle();
    }
    encOnceki = e; backOnceki = b; confOnceki = c;
    return;
  }

  /* ---------------- 编码器键：按下立即加字符 ---------------- */
  if (e && !encOnceki && millis() - sonEncAksiyon > TUSBAS_TEKRAR) {
    if (mod == 0) {
      mesaj += String(harfler[harfIndex]);
      ekraniGuncelle();
    }
    sonEncAksiyon = millis();
  }
  encOnceki = e;

  /* ---------------- 旋转 ---------------- */
  if (enkoderDegisim != 0) {
    if (mod == 0) {
      if (enkoderDegisim > 0) harfIndex = (harfIndex + 1) % harfSayisi;
      else                    harfIndex = (harfIndex - 1 + harfSayisi) % harfSayisi;
    } else {
      if (enkoderDegisim > 0) hazirMesajIndex = (hazirMesajIndex + 1) % hazirMesajSayisi;
      else                    hazirMesajIndex = (hazirMesajIndex - 1 + hazirMesajSayisi) % hazirMesajSayisi;
    }
    enkoderDegisim = 0;
    ekraniGuncelle();
  }

  /* ---------------- 返回键 ---------------- */
  if (b && !tusUzun[TUS_BACK] && millis() - tusBasZaman[TUS_BACK] >= GERI_UZUN_SURE) {
    tusUzun[TUS_BACK] = true;
    ekranMesaj("进入配置模式", "请稍候");
    delay(150);
    portalBaslat();
    mesaj = "";
    harfIndex = 0;
    enkoderDegisim = 0;
    ekraniGuncelle();
  }
  if (!b && backOnceki && !tusUzun[TUS_BACK]) {
    if (mod == 1) {
      mod = 0;
      ekraniGuncelle();
    } else if (mesaj.length() > 0) {
      int sonByte = mesaj.length() - 1;
      while (sonByte > 0 && (mesaj[sonByte] & 0xC0) == 0x80) sonByte--;
      mesaj = mesaj.substring(0, sonByte);
      ekraniGuncelle();
    }
  }
  backOnceki = b;

  /* ---------------- 确认键 ---------------- */
  if (c && !tusUzun[TUS_CONF] && millis() - tusBasZaman[TUS_CONF] >= UZUN_BAS_SURE) {
    tusUzun[TUS_CONF] = true;
    kisiSecildi = false;
    mod = 0;
    mesaj = "";
    enkoderDegisim = 0;
    kisiSecimEkrani();
  }
  if (!c && confOnceki && !tusUzun[TUS_CONF]) {
    if (mod == 1) {
      mesajGonder(String(hazirMesajlar[hazirMesajIndex]));
      mod = 0;
      hazirMesajIndex = 0;
      ekraniGuncelle();
    } else {
      unsigned long simdi = millis();
      if (sonConfirmZamani != 0 && simdi - sonConfirmZamani < CIFT_TIK_ARALIK) {
        sonConfirmZamani = 0;
        mod = 1;
        hazirMesajIndex = 0;
        ekraniGuncelle();
      } else {
        sonConfirmZamani = simdi;
      }
    }
  }
  confOnceki = c;

  if (sonConfirmZamani != 0 && millis() - sonConfirmZamani > CIFT_TIK_ARALIK) {
    if (mod == 0 && mesaj.length() > 0) {
      mesajGonder(mesaj);
      mesaj = "";
      harfIndex = 0;
    }
    sonConfirmZamani = 0;
  }

  /* ---------------- 新消息提示闪烁 ---------------- */
  if (bildirimVar) {
    if (millis() - bildirimZamani >= BILDIRIM_SURE) {
      bildirimVar = false;
      ekraniGuncelle();
    } else if (millis() - sonCizim >= 250) {
      ekraniGuncelle();
    }
  }


/* ---------------- 每分钟刷新一次时间显示 ---------------- */
if (millis() - sonSaatCizim >= 60000) {
  if (getLocalTime(&timeinfo)) {
    sonSaatCizim = millis();
    if(!kisiSecildi){
      kisiSecimEkrani();
    }else{
      ekraniGuncelle();
    }
  }
}



  /* ---------------- 长文本滚动 ---------------- */
  if (mod == 0 && millis() - sonKaymaZamani > KAYMA_ARALIK) {
    sonKaymaZamani = millis();
    bool guncelle = false;
    String s0 = mesajSatiri(0);
    String s1 = mesajSatiri(1);
    if (s0.length() && metinGenisligi(s0) > 124) {
      kaymaOffset0 += 2;
      if (kaymaOffset0 >= metinGenisligi(s0 + "     ")) kaymaOffset0 = 0;
      guncelle = true;
    }
    if (s1.length() && metinGenisligi(s1) > 124) {
      kaymaOffset1 += 2;
      if (kaymaOffset1 >= metinGenisligi(s1 + "     ")) kaymaOffset1 = 0;
      guncelle = true;
    }
    if (guncelle) ekraniGuncelle();
  }
}
