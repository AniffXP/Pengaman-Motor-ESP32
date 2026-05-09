// ================================================================
// ALAT PENGAMAN MOTOR MENGGUNAKAN ESP32
// DENGAN KONTROL JARAK JAUH MELALUI TELEGRAM
// ================================================================
// Hardware:
//   - ESP32 DevKit V1 (30 Pin)
//   - Modul GPS NEO-6M
//   - Modul GSM SIM800L V2
//   - Modul Relay 5V 1 Channel
//   - Regulator Tegangan LM2596
//   - Aki Motor 12V
// ================================================================
// Wiring:
//   SIM800L TX -> ESP32 GPIO26
//   SIM800L RX -> ESP32 GPIO27
//   GPS TX     -> ESP32 GPIO16
//   GPS RX     -> ESP32 GPIO17
//   RELAY IN   -> ESP32 GPIO15
//   LED        -> ESP32 GPIO2 (built-in)
// ================================================================

#include <HardwareSerial.h>
#include <TinyGPS++.h>

HardwareSerial modem(1);     // SIM800L (TX:26, RX:27)
HardwareSerial gpsSerial(2); // GPS Neo-6M (TX:17, RX:16)
TinyGPSPlus gps;

// Pin konfigurasi
constexpr int RELAY_PIN = 15;
constexpr int LED_PIN   = 2;

// Konfigurasi jaringan
constexpr char APN[]      = "internet";
constexpr char URL_GET[]  = "http://47.236.55.3/cmd.php?clear=1";
constexpr char URL_POST[] = "http://47.236.55.3/update_status.php";

// State fitur kehilangan
bool fiturKehilanganAktif     = false;
unsigned long timerKirimLokasi = 0;
const long intervalKirimLokasi = 300000; // 5 menit

// State koneksi
String lastStatus       = "";
bool gprsLost           = false;
unsigned long lastPoll     = 0;
unsigned long lastGpsDebug = 0;

// ======================== DEKLARASI FUNGSI ========================
String readAll(uint16_t timeout = 2000);
bool waitNetwork(uint32_t = 60000);
bool openBearer();
bool gprsAttached();
void sendAT(const String&, uint16_t = 600);
String httpGet(const char*);
bool httpPost(const char*, const String&);
void sendStatus(const String&);
void printSignal();
void printIP();
void halt(const char*);
void updateGPS();
void debugGPS();
void kirimDataLokasi();

// ======================== SETUP ========================
void setup() {
    Serial.begin(115200);
    modem.begin(9600, SERIAL_8N1, 26, 27);
    gpsSerial.begin(9600, SERIAL_8N1, 16, 17);

    pinMode(RELAY_PIN, OUTPUT);
    pinMode(LED_PIN, OUTPUT);

    // Relay HIGH = OFF (starter terputus) -> aman saat boot
    digitalWrite(RELAY_PIN, HIGH);
    digitalWrite(LED_PIN, LOW);

    delay(3000);

    // Inisialisasi modem SIM800L
    sendAT("AT");
    sendAT("ATE0");
    printSignal();

    if (!waitNetwork()) halt("Gagal registrasi jaringan");
    if (!openBearer())  halt("Gagal buka GPRS");

    printIP();
    sendStatus("ESP_READY");
    lastStatus = "ESP_READY";
    gprsLost = false;
    digitalWrite(LED_PIN, HIGH);
}

// ======================== LOOP UTAMA ========================
void loop() {
    // Baca data GPS secara kontinyu
    updateGPS();
    debugGPS();

    // Polling server setiap 10 detik
    if (millis() - lastPoll < 10000) return;
    lastPoll = millis();

    // Cek koneksi GPRS
    if (!gprsAttached()) {
        digitalWrite(LED_PIN, LOW);
        if (!gprsLost) {
            sendStatus("DISCONNECTED");
            gprsLost = true;
        }
        openBearer();
        return;
    }

    // Reconnect berhasil
    if (gprsLost) {
        sendStatus("RECONNECTED");
        gprsLost = false;
    }
    digitalWrite(LED_PIN, HIGH);

    // Ambil perintah dari server
    String cmd = httpGet(URL_GET);
    cmd.trim();
    cmd.toLowerCase();

    // ===== MODE KEHILANGAN AKTIF =====
    if (fiturKehilanganAktif) {
        if (cmd == "ditemukan") {
            // Nonaktifkan mode kehilangan, relay tetap OFF (aman)
            fiturKehilanganAktif = false;
            digitalWrite(RELAY_PIN, HIGH); // Pastikan starter tetap mati
            sendStatus("FITUR_HILANG_NONAKTIF");
        } else {
            sendStatus("FITUR_HILANG_AKTIF");
        }

        // Kirim lokasi berkala setiap 5 menit
        if (millis() - timerKirimLokasi >= intervalKirimLokasi) {
            kirimDataLokasi();
            timerKirimLokasi = millis();
        }
        return; // Abaikan perintah lain saat mode hilang
    }

    // ===== PROSES PERINTAH NORMAL =====
    if (cmd == "on") {
        // Nyalakan relay -> starter tersambung -> motor bisa nyala
        digitalWrite(RELAY_PIN, LOW);
        sendStatus("ON");
    }
    else if (cmd == "off") {
        // Matikan relay -> starter terputus -> motor tidak bisa nyala
        digitalWrite(RELAY_PIN, HIGH);
        sendStatus("OFF");
    }
    else if (cmd == "lokasi") {
        // Kirim lokasi GPS saat ini
        kirimDataLokasi();
    }
    else if (cmd == "hilang") {
        // Aktifkan mode kehilangan
        fiturKehilanganAktif = true;
        timerKirimLokasi = millis();

        // MATIKAN starter -> motor tidak bisa dinyalakan
        digitalWrite(RELAY_PIN, HIGH);

        sendStatus("FITUR_HILANG_AKTIF");
        kirimDataLokasi(); // Kirim lokasi pertama langsung
    }
}

// ======================== FUNGSI GPS ========================
void kirimDataLokasi() {
    updateGPS();
    if (gps.location.isValid()) {
        // Format link dengan pin marker (bukan view mode)
        String lokasi = "LOKASI=https://www.google.com/maps?q="
                      + String(gps.location.lat(), 6) + ","
                      + String(gps.location.lng(), 6);
        sendStatus(lokasi);
    } else {
        sendStatus("LOKASI=GPS_NOT_READY");
    }
}

void updateGPS() {
    while (gpsSerial.available()) {
        gps.encode(gpsSerial.read());
    }
}

void debugGPS() {
    if (millis() - lastGpsDebug > 3000) {
        lastGpsDebug = millis();
        if (gps.location.isValid()) {
            Serial.print("GPS: ");
            Serial.print(gps.location.lat(), 6);
            Serial.print(", ");
            Serial.println(gps.location.lng(), 6);
        }
    }
}

// ======================== FUNGSI AT COMMAND ========================
void sendAT(const String& cmd, uint16_t dly) {
    modem.println(cmd);
    delay(dly);
    while (modem.available()) Serial.write(modem.read());
}

String readAll(uint16_t timeout) {
    String s;
    unsigned long t0 = millis();
    while (millis() - t0 < timeout) {
        while (modem.available()) s += char(modem.read());
    }
    return s;
}

bool waitNetwork(uint32_t tout) {
    unsigned long t0 = millis();
    while (millis() - t0 < tout) {
        modem.println("AT+CREG?");
        delay(1000);
        String res = readAll();
        if (res.indexOf("+CREG: 0,1") != -1 ||
            res.indexOf("+CREG: 0,5") != -1) return true;
        delay(2000);
    }
    return false;
}

bool gprsAttached() {
    modem.println("AT+CGATT?");
    delay(300);
    String r = readAll();
    return r.indexOf("+CGATT: 1") != -1;
}

bool openBearer() {
    sendAT("AT+SAPBR=3,1,\"CONTYPE\",\"GPRS\"");
    sendAT(String("AT+SAPBR=3,1,\"APN\",\"") + APN + "\"");
    sendAT("AT+SAPBR=1,1", 2000);
    modem.println("AT+SAPBR=2,1");
    delay(600);
    String r = readAll();
    return r.indexOf(".") != -1;
}

// ======================== FUNGSI HTTP ========================
String httpGet(const char* url) {
    modem.println("AT+HTTPTERM");
    delay(300);
    while (modem.available()) modem.read();

    sendAT("AT+HTTPINIT");
    sendAT("AT+HTTPPARA=\"CID\",1");
    sendAT(String("AT+HTTPPARA=\"URL\",\"") + url + "\"");

    modem.println("AT+HTTPACTION=0");
    delay(5000);
    String r = readAll();

    if (r.indexOf(",200,") == -1) {
        modem.println("AT+HTTPTERM");
        return "";
    }

    modem.println("AT+HTTPREAD");
    delay(1000);
    String response = readAll();
    modem.println("AT+HTTPTERM");

    int start = response.indexOf("+HTTPREAD:");
    if (start != -1) {
        start = response.indexOf("\r\n", start);
        if (start != -1) {
            start += 2;
            int end = response.indexOf("\r\n", start);
            if (end != -1) {
                String content = response.substring(start, end);
                content.trim();
                return content;
            }
        }
    }
    return "";
}

bool httpPost(const char* url, const String& data) {
    modem.println("AT+HTTPTERM");
    delay(300);
    while (modem.available()) modem.read();

    sendAT("AT+HTTPINIT");
    sendAT("AT+HTTPPARA=\"CID\",1");
    sendAT(String("AT+HTTPPARA=\"URL\",\"") + url + "\"");
    sendAT("AT+HTTPPARA=\"CONTENT\",\"application/x-www-form-urlencoded\"");

    modem.println("AT+HTTPDATA=" + String(data.length()) + ",5000");
    delay(500);
    modem.print(data);
    delay(1000);
    modem.write(26);
    delay(5000);

    modem.println("AT+HTTPACTION=1");
    delay(7000);
    String r = readAll(3000);
    modem.println("AT+HTTPTERM");

    return r.indexOf(",200,") != -1;
}

void sendStatus(const String& status) {
    httpPost(URL_POST, "status=" + status);
}

// ======================== FUNGSI UTILITY ========================
void printSignal() {
    modem.println("AT+CSQ");
    delay(500);
    Serial.println(readAll());
}

void printIP() {
    modem.println("AT+SAPBR=2,1");
    delay(500);
    Serial.println(readAll());
}

void halt(const char* msg) {
    Serial.println(msg);
    while (true) delay(1000);
}
