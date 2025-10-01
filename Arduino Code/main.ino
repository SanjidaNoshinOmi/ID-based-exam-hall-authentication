Arduino code

#include <SPI.h>
#include <MFRC522.h>
#include <EEPROM.h>
#include <Servo.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>

// -------------------- USER CONFIG --------------------
// Put your admin card UID here (uppercase, zero-padded HEX, e.g., "04A1B203" or "A3F5127B99")
const char* PRESET_ADMIN_UID = "036AFC1A";   // <-- REPLACE WITH YOUR ADMIN UID
  
// --- RFID pins ---
#define SS_PIN   10
#define RST_PIN  9
MFRC522 mfrc522(SS_PIN, RST_PIN);

// --- Buttons ---
#define S1 A0  // Admin: authenticate, then delete next scanned student ID
#define S2 A1  // Toggle Enroll / Normal
#define S3 A2  // Show stored IDs

// --- LED + Servo ---
#define LED_PIN 7
Servo myServo;

// --- LCD ---
LiquidCrystal_I2C lcd(0x27, 16, 2);

// --- Modes / state ---
bool enrollMode = false;
int  idViewIndex = -1; // -1 = show count, >=0 = show specific ID

// --- Admin delete flow state ---
bool adminAuthPending   = false; // waiting for admin scan after S1
bool deleteAwaitTarget  = false; // after admin OK, waiting for target card to delete

// --- EEPROM layout (String-based IDs, fixed-width slots) ---
// Admin UID stored as zero-padded HEX string (up to 14 chars for 7-byte UIDs) + NUL
#define ADMIN_ADDR  0
#define ADMIN_SIZE  16         // enough for 14 hex chars + NUL + spare
#define DATA_START  16         // student slots start after admin record
#define SLOT_SIZE   16         // each student slot: up to 15 chars + NUL

// -------------------- Helpers: LCD --------------------
void lcdMsg(const char* l0, const char* l1 = nullptr) {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(l0);
  if (l1) {
    lcd.setCursor(0, 1);
    lcd.print(l1);
  }
}

// -------------------- Helpers: LED --------------------
void blinkLED(int times) {
  for (int i = 0; i < times; i++) {
    digitalWrite(LED_PIN, HIGH);
    delay(200);
    digitalWrite(LED_PIN, LOW);
    delay(200);
  }
}

// -------------------- UID as zero-padded HEX (consistent) --------------------
String getTagID() {
  // Build uppercase, zero-padded HEX string (e.g., 04A1B203...)
  char buf[3 * 10] = {0}; // supports up to 10 bytes if needed
  char* p = buf;
  for (byte i = 0; i < mfrc522.uid.size; i++) {
    p += sprintf(p, "%02X", mfrc522.uid.uidByte[i]);
  }
  return String(buf);
}

// -------------------- Admin helpers --------------------
bool isAdminSet() {
  uint8_t b = EEPROM.read(ADMIN_ADDR);
  return !(b == 0xFF || b == 0x00);
}

String readAdminTag() {
  String tag = "";
  for (int i = 0; i < ADMIN_SIZE; i++) {
    char c = EEPROM.read(ADMIN_ADDR + i);
    if (c == 0 || c == (char)255) break;
    tag += c;
  }
  return tag;
}

void writeAdminTag(const String &tag) {
  int n = min((int)tag.length(), ADMIN_SIZE - 1);
  for (int i = 0; i < n; i++) EEPROM.write(ADMIN_ADDR + i, tag[i]);
  EEPROM.write(ADMIN_ADDR + n, 0); // NUL
  for (int i = n + 1; i < ADMIN_SIZE; i++) EEPROM.write(ADMIN_ADDR + i, 0);
}

bool isAdminTag(const String &tag) {
  return tag == readAdminTag();
}

// -------------------- Storage: students --------------------
bool isCardEnrolled(String tag) {
  for (int addr = DATA_START; addr < EEPROM.length(); addr += SLOT_SIZE) {
    if (EEPROM.read(addr) == 255) break; // empty slot
    String storedTag = "";
    for (int j = 0; j < SLOT_SIZE; j++) {
      char c = EEPROM.read(addr + j);
      if (c == 0 || c == (char)255) break;
      storedTag += c;
    }
    if (storedTag.length() == 0) break;
    if (storedTag == tag) return true;
  }
  return false;
}

void enrollCard(String tag) {
  for (int addr = DATA_START; addr < EEPROM.length(); addr += SLOT_SIZE) {
    if (EEPROM.read(addr) == 255) {
      int n = min((int)tag.length(), SLOT_SIZE - 1);
      for (int j = 0; j < n; j++) EEPROM.write(addr + j, tag[j]);
      EEPROM.write(addr + n, 0); // NUL
      // clear remainder
      for (int j = n + 1; j < SLOT_SIZE; j++) EEPROM.write(addr + j, 0);
      return;
    }
  }
}

void clearEEPROMAll() {
  for (int i = 0; i < EEPROM.length(); i++) EEPROM.write(i, 255);
}

int countStoredIDs() {
  int count = 0;
  for (int addr = DATA_START; addr < EEPROM.length(); addr += SLOT_SIZE) {
    if (EEPROM.read(addr) != 255) count++;
    else break;
  }
  return count;
}

void readSlotToBuf(int addr, char buf[SLOT_SIZE]) {
  for (int j = 0; j < SLOT_SIZE; j++) buf[j] = EEPROM.read(addr + j);
}

void writeBufToSlot(int addr, const char buf[SLOT_SIZE]) {
  for (int j = 0; j < SLOT_SIZE; j++) EEPROM.write(addr + j, buf[j]);
}

int findCardIndex(const String &tag) {
  int idx = 0;
  for (int addr = DATA_START; addr < EEPROM.length(); addr += SLOT_SIZE, idx++) {
    if (EEPROM.read(addr) == 255) break;
    String stored = "";
    for (int j = 0; j < SLOT_SIZE; j++) {
      char c = EEPROM.read(addr + j);
      if (c == 0 || c == (char)255) break;
      stored += c;
    }
    if (stored == tag) return idx;
  }
  return -1;
}

bool deleteCardByIndex(int index) {
  int total = countStoredIDs();
  if (index < 0 || index >= total) return false;

  // shift left from (index+1) .. (total-1)
  for (int i = index; i < total - 1; i++) {
    int fromAddr = DATA_START + (i + 1) * SLOT_SIZE;
    int toAddr   = DATA_START + i * SLOT_SIZE;
    char buf[SLOT_SIZE];
    readSlotToBuf(fromAddr, buf);
    writeBufToSlot(toAddr, buf);
  }

  // clear last slot
  int lastAddr = DATA_START + (total - 1) * SLOT_SIZE;
  for (int j = 0; j < SLOT_SIZE; j++) EEPROM.write(lastAddr + j, 255);
  return true;
}

bool deleteCardByTag(const String &tag) {
  int index = findCardIndex(tag);
  if (index < 0) return false;
  return deleteCardByIndex(index);
}

// -------------------- UI: showNextID --------------------
void showNextID() {
  int total = countStoredIDs();

  if (idViewIndex == -1) {
    lcd.clear();
    lcd.print("Total IDs: ");
    lcd.print(total);
    Serial.print("Total IDs: ");
    Serial.println(total);
    idViewIndex = 0; // next press shows first ID
  } else if (idViewIndex < total) {
    int addr = DATA_START + idViewIndex * SLOT_SIZE;
    String storedTag = "";
    for (int j = 0; j < SLOT_SIZE; j++) {
      char c = EEPROM.read(addr + j);
      if (c == 0 || c == (char)255) break;
      storedTag += c;
    }
    lcd.clear();
    lcd.print("ID ");
    lcd.print(idViewIndex + 1);
    lcd.setCursor(0, 1);
    lcd.print(storedTag);
    Serial.print("ID ");
    Serial.print(idViewIndex + 1);
    Serial.print(": ");
    Serial.println(storedTag);

    idViewIndex++;
    if (idViewIndex >= total) idViewIndex = -1; // reset after last
  } else {
    idViewIndex = -1; // safety reset
  }
}

// -------------------- Setup --------------------
void setup() {
  Serial.begin(9600);
  SPI.begin();
  mfrc522.PCD_Init();

  pinMode(S1, INPUT_PULLUP);
  pinMode(S2, INPUT_PULLUP);
  pinMode(S3, INPUT_PULLUP);
  pinMode(LED_PIN, OUTPUT);

  myServo.attach(8);
  myServo.write(0); // Locked

  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print("System Ready");
  delay(800);
  lcd.clear();
  lcd.print("Normal Mode");

  Serial.println("System Ready - Normal Mode");

  // --- Force the pre-set admin UID into EEPROM (keeps fixed admin)
  String current = readAdminTag();
  if (current != String(PRESET_ADMIN_UID)) {
    writeAdminTag(String(PRESET_ADMIN_UID));
    Serial.print("Admin UID set to: ");
    Serial.println(PRESET_ADMIN_UID);
  } else {
    Serial.print("Admin UID: ");
    Serial.println(current);
  }
}

// -------------------- Loop --------------------
void loop() {
  // --- Button Actions ---
  if (digitalRead(S1) == LOW) {
    // Always require admin authentication (admin is fixed from PRESET_ADMIN_UID)
    lcd.clear();
    lcd.print("Scan Admin Card");
    Serial.println("Authenticate Admin...");
    adminAuthPending = true;     // next scan must be admin
    deleteAwaitTarget = false;
    delay(300);
    idViewIndex = -1;
  }

  if (digitalRead(S2) == LOW) {
    enrollMode = !enrollMode;
    lcd.clear();
    if (enrollMode) {
      lcd.print("Enroll Mode");
      Serial.println("Switched to ENROLL MODE");
    } else {
      lcd.print("Normal Mode");
      Serial.println("Switched to NORMAL MODE");
    }
    delay(300);
    idViewIndex = -1;
  }

  if (digitalRead(S3) == LOW) {
    showNextID();
    delay(300);
  }

  // --- RFID Reading ---
  if (!mfrc522.PICC_IsNewCardPresent()) return;
  if (!mfrc522.PICC_ReadCardSerial())   return;

  // Build padded HEX tag once, then halt card immediately
  String tag = getTagID();
  mfrc522.PICC_HaltA();
  mfrc522.PCD_StopCrypto1();

  Serial.print("Scanned Tag: ");
  Serial.println(tag);

  // --- Admin flow handling ---
  if (adminAuthPending) {
    if (isAdminTag(tag)) {
      adminAuthPending = false;
      deleteAwaitTarget = true;
      lcd.clear(); lcd.print("Delete: scan ID");
      Serial.println("Admin OK. Waiting for target card...");
      delay(600);
      return;
    } else {
      adminAuthPending = false;
      deleteAwaitTarget = false;
      lcd.clear(); lcd.print("Admin Denied");
      Serial.println("Admin auth failed.");
      delay(800);
      lcd.clear(); lcd.print(enrollMode ? "Enroll Mode" : "Normal Mode");
      return;
    }
  }

  if (deleteAwaitTarget) {
    // Prevent deleting the admin card
    if (isAdminTag(tag)) {
      lcdMsg("Cannot delete", "Admin Card");
      Serial.println("Refused: tried to delete Admin card.");
      delay(1200);
    } else if (!isCardEnrolled(tag)) {
      lcdMsg("ID not found");
      Serial.println("Delete failed: not in list.");
      delay(800);
    } else {
      bool ok = deleteCardByTag(tag);
      lcd.clear();
      if (ok) {
        lcd.print("ID Deleted");
        Serial.print("Deleted: "); Serial.println(tag);
        blinkLED(1);
      } else {
        lcd.print("Delete Error");
        Serial.print("Delete error: "); Serial.println(tag);
        blinkLED(2);
      }
      delay(1000);
    }
    deleteAwaitTarget = false;
    lcd.clear(); lcd.print(enrollMode ? "Enroll Mode" : "Normal Mode");
    return;
  }

  // --- Normal operation ---
  if (enrollMode) {
    // Never enroll the admin card
    if (isAdminTag(tag)) {
      lcdMsg("Admin Card", "Not Enrolled");
      Serial.println("Refused to enroll Admin card.");
      delay(900);
      return;
    }
    if (isCardEnrolled(tag)) {
      lcdMsg("Already Enrolled");
      Serial.println("Already Enrolled");
      delay(900);
    } else {
      enrollCard(tag);
      lcdMsg("Card Enrolled");
      Serial.print("Enrolled new card: ");
      Serial.println(tag);
      delay(900);
    }
  } else {
    if (isCardEnrolled(tag)) {
      lcdMsg("Access Granted");
      Serial.println("Access Granted");
      blinkLED(1);          // Blink once for granted
      myServo.write(90);    // Unlock
      delay(3000);
      myServo.write(0);     // Lock again
      lcdMsg("Normal Mode");
    } else {
      // Admin card should not open the door
      if (isAdminTag(tag)) {
        lcdMsg("Admin Card");
        Serial.println("Admin card scanned (no unlock).");
        delay(900);
      } else {
        lcdMsg("Access Denied");
        Serial.println("Access Denied");
        blinkLED(2);        // Blink twice for denied
        delay(900);
        lcdMsg("Normal Mode");
      }
    }
  }
}

