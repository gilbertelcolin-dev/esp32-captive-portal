# ESP32 Captive Portal (UI terpisah / LittleFS)

Captive portal untuk ESP32 (Arduino framework, PlatformIO). AP terbuka, portal muncul otomatis
di Android/iOS/Windows, kredensial form disimpan ke LittleFS, plus panel admin dan konsol serial.
**Tampilan (HTML/CSS) dipisah total dari firmware** — ada di `data/*.html`, bisa diganti tanpa
menyentuh kode C++.

> Proyek ini dibuat untuk **uji keamanan & edukasi** pada jaringan milik sendiri. Jangan dipakai di
> jaringan orang lain tanpa izin tertulis.

---

## Kenapa repot soal alamat IP?

Android 12 ke atas **menolak** menyebut sebuah Wi-Fi sebagai captive portal kalau nama host probe
(`connectivitycheck.gstatic.com`) di-resolve ke **IP privat**. Ini ada di AOSP:

```java
// packages/modules/NetworkStack/src/com/android/server/connectivity/NetworkMonitor.java
if (mPrivateIpNoInternetEnabled && probeType == ValidationProbeEvent.PROBE_HTTP
        && (proxy == null) && hasPrivateIpAddress(resolvedAddr)) {
    return CaptivePortalProbeResult.PRIVATE_IP;   // -> "Terhubung tanpa internet", TANPA tap "Masuk"
}

private static boolean hasPrivateIpAddress(InetAddress[] addresses) {
    // 10/8, 172.16/12, 192.168/16, 169.254/16, fc00::/7
}
```

Portal ESP32 pada umumnya memakai `192.168.4.1`, jadi di Android 12+ hasilnya:

```
Terhubung tanpa internet            <-- tidak ada tombol "Masuk", tidak ada sheet login
```

**Solusi di proyek ini:** AP memakai alamat **publik** `203.0.113.1/24` (TEST-NET-3, RFC 5737).
Probe resolve ke IP non-privat → lolos pengecekan → portal terdeteksi → tombol/sheet **Masuk** muncul.

```
Android 12-16   : OK (butuh trik IP publik di atas)
Android 7-11    : OK
Android 5-6     : OK
iOS 12+         : OK (/hotspot-detect.html -> 302 -> "Log In")
Windows 10/11   : OK (NCSI / connecttest.txt -> 302 -> "Sign in to network")
```

Catatan: di semua Android, opsi **"Switch to mobile data / Beralih ke data seluler"** harus OFF,
kalau tidak OS memilih seluler dan menahan sheet login.

---

## Fitur

| Fitur | Keterangan |
|---|---|
| AP terbuka | SSID `SMAN 2 MATARAM`, channel 6, maks 8 klien |
| DNS hijack sendiri | semua A -> `203.0.113.1`, AAAA -> NODATA (bukan timeout) |
| Probe captive portal | `/generate_204`, `/hotspot-detect.html`, `/connecttest.txt`, `/ncsi.txt`, `/wispr`, `/wpad.dat`, … -> `302 + Location` |
| DHCP sendiri | kirim opsi 1, 3, 6, 28, 51, 54 **+ 114 (Captive Portal URI, RFC 8910)** |
| CAPPORT API | `GET /captive-portal-api` -> `{"captive":true,"user-portal-url":…}` (RFC 8908) |
| Penyimpanan | `/creds.txt` di LittleFS (append), tahan reboot, ±5.000 entri (1.4 MB) |
| Panel admin | `/admin` (tanpa key), CSV, wipe |
| Alur login | gagal → gagal → **berhasil** di percobaan ke-3 (per-IP) |
| Blokir otomatis | setelah percobaan ke-3: IP+MAC diblokir, klien **di-deauth**, DHCP menolak memberi lease, semua HTTP-nya hanya dapat halaman "berhasil". Daftar di `/blocked.txt` |
| Anti duplikat | user+pass sama dari IP sama tidak disimpan ulang |
| Konsol serial | `count`, `csv`/`backup`, `dump`, `status`, `wipe`, `ui`, `blocked`, `unblock` |
| UI eksternal | semua halaman di `data/*.html`, di-serve dari LittleFS |

---

## Struktur

```
smanda-portal/
├── src/main.cpp          # firmware: HTTP, DNS, DHCP, storage, serial console
├── data/                 # UI (di-upload ke LittleFS, bukan dikompilasi)
│   ├── portal.html       # halaman login      (placeholder: <!--ERR-->)
│   ├── jump.html         # auto-redirect      (placeholder: %URL%)
│   ├── loading.html      # "menghubungkan"    (JS redirect ke /hasil)
│   ├── success.html      # percobaan ke-3 berhasil
│   └── admin.html        # panel log          (%META% %KEY% %ROWS%)
├── tools/
│   ├── pull_creds.py     # tarik CSV lewat USB (tanpa join Wi-Fi)
│   └── serial_log.py     # logger serial ber-timestamp
└── platformio.ini
```

Firmware tetap punya salinan HTML bawaan (PROGMEM) sebagai *fallback*: kalau `data/` belum
di-upload, portal tetap jalan.

---

## Build & flash

```bash
# butuh PlatformIO (pio) + ESP32 devkit
pio run -t upload       # firmware
pio run -t uploadfs     # UI ke LittleFS (WAJIB kalau habis ubah data/*.html)
pio device monitor      # 115200 baud
```

> **PENTING:** `pio run -t uploadfs` mem-format partisi LittleFS, jadi **kredensial yang sudah
> tersimpan ikut terhapus**. Selalu tarik dulu sebelum ganti UI:
> `python3 tools/pull_creds.py /dev/ttyUSB0 creds.csv`

Hasil boot yang benar:

```
=== SMANDA CAPTIVE PORTAL (ESP32) ===
[FS] 2 entri dimuat
[AP] UP SMAN 2 MATARAM | IP 203.0.113.1
[DHCP] DNS option 6 -> 203.0.113.1
[DNS] wildcard aktif :53 (logging on)
[DHCP] server sendiri :67 (option 114 ON)
[HTTP] server on :80
[ADMIN] http://203.0.113.1/admin  (key opsional: ?k=smanda2)
[CMD] ketik: help | count | csv | dump | status | wipe | ui
=== READY ===
```

Catu daya: cukup USB 5V (charger/powerbank). Tidak perlu komputer. Arus ±120–250 mA, pakai
charger minimal 1A supaya tidak brownout.

---

## Ganti tampilan (tanpa compile ulang)

1. Edit `data/portal.html` (atau file lain).
2. `pio run -t uploadfs`
3. Refresh di HP.

Placeholder yang dikenali firmware:

| File | Placeholder | Isi |
|---|---|---|
| `portal.html` | `<!--ERR-->` | kotak pesan error (kosong kalau bukan error) |
| `jump.html` | `%URL%` | `http://203.0.113.1/` |
| `admin.html` | `%META%`, `%KEY%`, `%ROWS%` | ringkasan, admin key, baris tabel kredensial |

Kalau hanya butuh ganti logo/teks CSS, tidak perlu sentuh `main.cpp` sama sekali.

---

## Panel admin

```
http://203.0.113.1/admin        # tabel kredensial (tanpa key)
http://203.0.113.1/admin.csv    # unduh CSV
http://203.0.113.1/admin/clear  # hapus semua
http://203.0.113.1/admin?k=smanda2   # tetap didukung kalau key diaktifkan lagi
```

`ADMIN_KEY` ada di `src/main.cpp`; `authOk()` menerima request tanpa `k` (biar gampang diketik
dari HP). Kalau dipakai rame-rame, balikkan ke wajib-key: satu baris di `authOk()`.

---

## Ambil data tanpa menyambung ke Wi-Fi

Lewat kabel USB:

```bash
python3 tools/pull_creds.py /dev/ttyUSB0 creds.csv
```

Atau perintah serial manual (115200):

```
count   # jumlah entri di LittleFS
csv     # seluruh isi sebagai CSV (diakhiri ###END###)
dump    # dibaca manusia
status  # uptime, klien, jumlah query DNS, besar heap
ui      # file UI mana yang aktif (LittleFS vs bawaan)
wipe    # hapus semua (kredensial + daftar blokir)
blocked # lihat IP yang diblokir
unblock all | unblock 203.0.113.12
```

Format baris: `waktu|username|password|ip|user-agent`
CSV: `waktu,username,password,ip,device`

---

## Konfigurasi cepat (`src/main.cpp`)

```cpp
#define AP_SSID     "SMAN 2 MATARAM"
#define AP_PASS     ""          // "" = terbuka
#define AP_CHANNEL  6
#define ADMIN_KEY   "smanda2"
IPAddress apIP(203, 0, 113, 1); // range publik, lihat bagian "Kenapa"
```

Byte DHCP dan jawaban DNS diturunkan otomatis dari `apIP` — cukup ubah satu baris itu.
Kalau mengganti ke IP publik lain, pastikan masih **bukan** RFC1918 / link-local.

---

## Kalau portal tidak muncul (checklist)

1. Log serial: apakah ada `[DHCP] ACK`? kalau tidak, DHCP tidak jalan.
2. Apakah ada `[PROBE] /generate_204`? kalau tidak, DNS tidak dipakai HP.
3. HP: lupakan jaringan lalu sambung ulang (Android menyimpan vonis "tanpa internet").
4. HP: "Switch to mobile data" OFF, Private DNS OFF, data seluler OFF saat tes.
5. Android 12+ wajib IP AP non-privat (lihat bagian pertama).

---

## Lisensi

MIT — lihat [LICENSE](LICENSE). Logo/nama instansi di halaman contoh hanya placeholder;
ganti sebelum dipakai.
