#include <MFRC522.h>
#include <SPI.h>
#include <LiquidCrystal_I2C.h>
#include <Keypad.h>
#include <ESP32Servo.h>
#include <WiFi.h>
#include <HTTPClient.h>

/* ================= WIFI ================= */

const char* ssid = "xxxxx"; // WiFi SSID
const char* password = "xxxxxx"; // WiFi Password

/* ================= SERVER API ================= */

const char* rfidURL = "http://xxx.xxx.xxx.xx:5001/api/check-rfid?uid="; // Put WiFi IPv4 Address on xxx.xxx.xxx.xx
const char* purchaseURL = "http://xxx.xxx.xxx.xx:5001/api/purchase"; // Put WiFi IPv4 Address on xxx.xxx.xxx.xx
const char* restockURL = "http://xxx.xxx.xxx.xx:5001/api/restock"; // Put WiFi IPv4 Address on xxx.xxx.xxx.xx
const char* productInfoURL = "http://xxx.xxx.xxx.xx:5001/api/product-info?product="; // Put WiFi IPv4 Address on xxx.xxx.xxx.xx

/* ================= RFID ================= */

#define SS_PIN   5
#define RST_PIN  2
MFRC522 rfid(SS_PIN, RST_PIN);

/* ================= LCD ================= */

LiquidCrystal_I2C lcd(0x27, 20, 4);

/* ================= KEYPAD ================= */

const byte ROWS = 4;
const byte COLS = 4;

char keys[ROWS][COLS] = {
  {'1','2','3','A'},
  {'4','5','6','B'},
  {'7','8','9','C'},
  {'*','0','#','D'}
};

byte rowPins[ROWS] = {32, 33, 16, 17};
byte colPins[COLS] = {12, 13, 15, 4};

Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);

/* ================= SERVOS ================= */

Servo servo[4];
int servoPins[4] = {14, 25, 26, 27};

/* MG995 360 continuous rotation servo values */
const int SERVO_STOP = 1500;
const int SERVO_FORWARD = 1000;
const int DISPENSE_TIME = 1500;

/* ================= IR RESTOCK SENSORS ================= */

#define IR_A 36
#define IR_B 39
#define IR_C 34
#define IR_D 35

bool restockLatched[4] = {false, false, false, false};

/* ================= CENTER LCD FUNCTION ================= */

void printCentered(int row, String text) {
  int col = (20 - text.length()) / 2;
  if (col < 0) col = 0;
  lcd.setCursor(col, row);
  lcd.print(text);
}

/* ================= RESET TO START SCREEN ================= */

void showScanPrompt() {
  lcd.clear();
  printCentered(1, "SaniTap VM");
  printCentered(2, "Scan your RFID");
}

/* ================= GET FIRST NAME ================= */

String getFirstName(String fullName) {
  fullName.trim();
  int spaceIndex = fullName.indexOf(' ');
  if (spaceIndex == -1) return fullName;
  return fullName.substring(0, spaceIndex);
}

/* ================= EXTRACT JSON STRING VALUE ================= */

String extractJsonString(String payload, String key) {
  String pattern = "\"" + key + "\":\"";
  int startIndex = payload.indexOf(pattern);

  if (startIndex == -1) return "";

  startIndex += pattern.length();
  int endIndex = payload.indexOf("\"", startIndex);

  if (endIndex == -1) return "";

  return payload.substring(startIndex, endIndex);
}

/* ================= EXTRACT JSON INT VALUE ================= */

int extractJsonInt(String payload, String key) {
  String pattern = "\"" + key + "\":";
  int startIndex = payload.indexOf(pattern);

  if (startIndex == -1) return -1;

  startIndex += pattern.length();

  while (startIndex < payload.length() && payload[startIndex] == ' ') {
    startIndex++;
  }

  int endIndex = payload.indexOf(",", startIndex);
  if (endIndex == -1) {
    endIndex = payload.indexOf("}", startIndex);
  }

  if (endIndex == -1) return -1;

  String value = payload.substring(startIndex, endIndex);
  value.trim();
  return value.toInt();
}

/* ================= GET PRODUCT INFO FROM DB ================= */

bool getProductInfo(String product, int &available, String &statusText, String &productName) {
  available = -1;
  statusText = "";
  productName = "";

  if (WiFi.status() != WL_CONNECTED) return false;

  HTTPClient http;
  http.begin(String(productInfoURL) + product);

  int httpCode = http.GET();

  if (httpCode <= 0) {
    Serial.println("PRODUCT INFO CHECK FAILED: " + product);
    http.end();
    return false;
  }

  String payload = http.getString();
  http.end();

  Serial.println("PRODUCT INFO RESPONSE [" + product + "]: " + payload);

  available = extractJsonInt(payload, "product_available");
  statusText = extractJsonString(payload, "status");
  productName = extractJsonString(payload, "name");

  return true;
}

/* ================= CHECK IF PRODUCT CAN DISPENSE ================= */

bool canDispenseProduct(String product, int &available, String &statusText, String &productName) {
  bool ok = getProductInfo(product, available, statusText, productName);

  if (!ok) return false;

  return available > 0;
}

/* ================= SEND RESTOCK ================= */
/* Default restock value is 8.
   Change this if you want the machine to send another value. */

bool sendRestock(String product, int quantity = 8) {
  if (WiFi.status() != WL_CONNECTED) return false;

  HTTPClient http;
  http.begin(restockURL);
  http.addHeader("Content-Type", "application/json");

  String json = "{\"product\":\"" + product + "\",\"product_available\":" + String(quantity) + "}";
  int code = http.POST(json);

  if (code <= 0) {
    Serial.println("RESTOCK FAILED: No HTTP response");
    http.end();
    return false;
  }

  String response = http.getString();

  Serial.println("RESTOCK HTTP CODE: " + String(code));
  Serial.println("RESTOCK RESPONSE [" + product + "]: " + response);

  http.end();

  if (code == 200 || code == 201) {
    return true;
  }

  return false;
}

/* ================= CHECK IR SENSORS ================= */

void checkRestock(int pin, String product, int productIndex) {
  int sensorState = digitalRead(pin);

  if (sensorState == LOW) {
    if (!restockLatched[productIndex]) {
      restockLatched[productIndex] = true;

      Serial.println("RESTOCK SENSOR DETECTED: " + product);

      int available = -1;
      String statusText = "";
      String productName = "";

      bool infoOk = getProductInfo(product, available, statusText, productName);

      if (infoOk && (statusText == "LOW STOCK" || statusText == "NO STOCK")) {
        lcd.clear();
        printCentered(1, "Restock Detected");
        printCentered(2, "Product " + product);

        bool success = sendRestock(product, 8);

        if (success) {
          lcd.clear();
          printCentered(1, "Restock Success");
          printCentered(2, "Product " + product);
          delay(1500);
        } else {
          lcd.clear();
          printCentered(1, "Restock Failed");
          printCentered(2, "Try Again");
          delay(1500);
        }
      } else {
        Serial.println("IGNORED: " + product + " is not LOW STOCK / NO STOCK in DB");
      }

      showScanPrompt();
    }
  } else {
    restockLatched[productIndex] = false;
  }
}

/* ================= DISPENSE ================= */

void dispense(Servo &s) {
  s.writeMicroseconds(SERVO_FORWARD);
  delay(DISPENSE_TIME);
  s.writeMicroseconds(SERVO_STOP);
  delay(300);
}

/* ================= WAIT FOR PRODUCT KEY ================= */

char waitForProductKey() {
  while (true) {
    char key = keypad.getKey();

    if (key >= 'A' && key <= 'D') {
      return key;
    }

    if (key == '*' || key == '#') {
      return key;
    }
  }
}

/* ================= HANDLE CANCEL ================= */

void cancelTransaction() {
  lcd.clear();
  printCentered(1, "Transaction");
  printCentered(2, "Cancelled");
  delay(1500);

  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();

  showScanPrompt();
}

/* ================= HANDLE RFID CLEANUP ================= */

void endRFIDSession() {
  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();
  showScanPrompt();
}

/* ================= SETUP ================= */

void setup() {
  Serial.begin(115200);

  lcd.init();
  lcd.backlight();

  printCentered(1, "Connecting WiFi");
  WiFi.begin(ssid, password);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  lcd.clear();
  printCentered(1, "WiFi Connected");
  delay(1500);

  SPI.begin(18, 19, 23, SS_PIN);
  rfid.PCD_Init();

  for (int i = 0; i < 4; i++) {
    servo[i].setPeriodHertz(50);
    servo[i].attach(servoPins[i], 500, 2400);
    servo[i].writeMicroseconds(SERVO_STOP);
    delay(300);
  }

  pinMode(IR_A, INPUT);
  pinMode(IR_B, INPUT);
  pinMode(IR_C, INPUT);
  pinMode(IR_D, INPUT);

  showScanPrompt();
}

/* ================= LOOP ================= */

void loop() {
  /* RESTOCK SENSOR CHECK */
  checkRestock(IR_A, "A", 0);
  checkRestock(IR_B, "B", 1);
  checkRestock(IR_C, "C", 2);
  checkRestock(IR_D, "D", 3);

  /* WAIT FOR RFID */
  if (!rfid.PICC_IsNewCardPresent()) return;
  if (!rfid.PICC_ReadCardSerial()) return;

  String uidStr = "";

  for (byte i = 0; i < rfid.uid.size; i++) {
    if (rfid.uid.uidByte[i] < 0x10) uidStr += "0";
    uidStr += String(rfid.uid.uidByte[i], HEX);
  }

  uidStr.toUpperCase();
  Serial.println("Scanned RFID: " + uidStr);

  lcd.clear();
  printCentered(1, "Checking Card...");

  HTTPClient http;
  http.begin(String(rfidURL) + uidStr);

  int httpCode = http.GET();

  if (httpCode <= 0) {
    lcd.clear();
    printCentered(1, "Server Error");
    delay(3000);
    http.end();

    endRFIDSession();
    return;
  }

  String payload = http.getString();
  Serial.println(payload);
  http.end();

  if (payload.indexOf("\"status\":\"INVALID\"") != -1) {
    lcd.clear();
    printCentered(1, "Access Denied");
    printCentered(2, "Not Registered");
    delay(2500);

    endRFIDSession();
    return;
  }

  String userName = extractJsonString(payload, "studentName");
  String firstName = getFirstName(userName);

  lcd.clear();
  printCentered(1, "Access Granted");
  delay(1500);

  lcd.clear();
  printCentered(1, "Hello!");
  printCentered(2, firstName);
  delay(1500);

  lcd.clear();
  printCentered(0, "Select Product");
  printCentered(1, "A  B  C  D");
  printCentered(3, "* or # to Cancel");

  char key = waitForProductKey();

  if (key == '*' || key == '#') {
    cancelTransaction();
    return;
  }

  int index = key - 'A';

  /* CHECK STOCK FIRST BEFORE PURCHASE */
  int availableCount = -1;
  String productStatus = "";
  String productName = "";

  bool allowed = canDispenseProduct(String(key), availableCount, productStatus, productName);

  if (!allowed) {
    lcd.clear();
    printCentered(1, "No Stock");
    printCentered(2, "Product " + String(key));
    delay(2000);

    endRFIDSession();
    return;
  }

  /* SEND PURCHASE REQUEST */
  HTTPClient http2;
  http2.begin(purchaseURL);
  http2.addHeader("Content-Type", "application/json");

  String json =
    "{\"rfid\":\"" + uidStr +
    "\",\"product_code\":\"" + String(key) + "\"}";

  Serial.println("PURCHASE REQUEST: " + json);

  int httpResponse = http2.POST(json);

  if (httpResponse <= 0) {
    lcd.clear();
    printCentered(1, "Purchase Failed");
    delay(2000);
    http2.end();

    endRFIDSession();
    return;
  }

  String response = http2.getString();
  Serial.println("PURCHASE RESPONSE: " + response);
  http2.end();

  /* HANDLE NO STOCK RESPONSE */
  if (response.indexOf("\"status\":\"NO_STOCK\"") != -1) {
    lcd.clear();
    printCentered(1, "No Stock");
    printCentered(2, "Cannot Dispense");
    delay(2000);

    endRFIDSession();
    return;
  }

  /* HANDLE GENERIC ERROR */
  if (response.indexOf("\"status\":\"SUCCESS\"") == -1) {
    lcd.clear();
    printCentered(1, "Purchase Failed");
    delay(2000);

    endRFIDSession();
    return;
  }

  /* DISPENSE ONLY ON SUCCESS */
  lcd.clear();
  printCentered(1, "Dispensing...");

  dispense(servo[index]);

  lcd.clear();
  printCentered(1, "Item Dispensed");
  delay(2000);

  endRFIDSession();
}