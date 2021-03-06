#include <Arduino.h>
#include <Wire.h>
#include "MultiLCD.h"
#include "TinyGPS.h"
#include "OBD.h"
#include "MPU6050.h"

#define INIT_CMD_COUNT 4
#define MAX_CMD_LEN 6

// GPS logging can only be enabled when there is additional hardware serial UART
#if defined(__AVR_ATmega2560__) || defined(__AVR_ATmega1280__)
#define GPSUART Serial3
#elif defined(__AVR_ATmega644P__)
#define GPSUART Serial1
#endif

#define GPS_BAUDRATE 38400 /* bps */

#ifdef GPSUART
TinyGPS gps;
#endif // GPSUART

int MPU6050_read(int start, uint8_t *buffer, int size);

const char initcmd[INIT_CMD_COUNT][MAX_CMD_LEN] = {"ATZ\r","ATE0\r","ATL1\r","0902\r"};

//SoftwareSerial softSerial(2, 3); // RX, TX

unsigned int  adc_key_val[5] ={30, 150, 360, 535, 760 };
int NUM_KEYS = 5;
int adc_key_in;
char key=-1;
char oldkey=-1;

byte index = 0;
uint16_t pid = 0x0111;

bool hasMPU6050 = false;

//create object to control an LCD.
//LCD_SSD1306 lcd;
//LCD_ZTOLED lcd;
LCD_PCD8544 lcd;
//LCD_1602 lcd;
//LCD_ILI9325D lcd;

#ifdef GPSUART
void ShowGPSData()
{
    // parsed GPS data is ready
    char buf[32];
    unsigned long fix_age;

    lcd.setFont(FONT_SIZE_SMALL);
    if (lcd.getLines() > 2) {
        unsigned long date, time;
        gps.get_datetime(&date, &time, &fix_age);
        sprintf(buf, "TIME: %08ld", time);
        lcd.setCursor(0, 6);
        lcd.print(buf);
    }

    if (lcd.getLines() > 3) {
        long lat, lon;
        gps.get_position(&lat, &lon, &fix_age);
        // display LAT/LON if screen is big enough
        lcd.setCursor(0, 7);
        if (((unsigned int)millis() / 1000) & 1)
            sprintf(buf, "LAT: %d.%5ld  ", (int)(lat / 100000), lat % 100000);
        else
            sprintf(buf, "LON: %d.%5ld  ", (int)(lon / 100000), lon % 100000);
        lcd.print(buf);
    }
}
#endif

class COBDTester : public COBD
{
public:
    bool Init(bool passive = false)
    {
        unsigned long currentMillis;
        unsigned char n;
        char prompted;
        char buffer[128];

        for (unsigned char i = 0; i < INIT_CMD_COUNT; i++) {
            lcd.clear();
            lcd.print(initcmd[i]);
            lcd.setCursor(0, 1);
            write(initcmd[i]);
            n = 0;
            prompted = 0;
            currentMillis = millis();
            for (;;) {
                if (available()) {
                    char c = read();
                    if (c == '>') {
                        buffer[n] = 0;
                        prompted++;
                    } else if (n < sizeof(buffer) - 1) {
                        buffer[n++] = c;

                        if (c == '\n')
                            lcd.changeLine();
                        else if (c >= ' ') {
                            lcd.write(c);
                        }
                    }
                } else if (prompted) {
                    break;
                } else {
                    unsigned long elapsed = millis() - currentMillis;
                    if (elapsed > OBD_TIMEOUT_INIT) {
                        // init timeout
                        //WriteData("\r");
                        lcd.print("Timeout!");
                        if (i == 0) return false;
                        i--;
                        break;
                    }
                }
            }
            delay(500);
        }
        delay(1500);

        char* data;
        memset(pidmap, 0, sizeof(pidmap));
        for (byte i = 0; i < 4; i++) {
            lcd.clear();
            sprintf(buffer, "PIDs [%02x-%02x]", i * 0x20 + 1, i * 0x20 + 0x20);
            lcd.print(buffer);
            byte pid = i * 0x20;
            sendQuery(pid);
            data = getResponse(pid, buffer);
            if (!data) break;
            lcd.setCursor(0, 1);
            lcd.print(data);
            delay(500);
            data--;
            for (byte n = 0; n < 4; n++) {
                if (data[n * 3] != ' ')
                    break;
                pidmap[i * 4 + n] = hex2uint8(data + n * 3 + 1);
            }
        }
        // display pid map
        lcd.clear();
        for (byte i = 0; i < sizeof(pidmap); i++) {
            sprintf(buffer, "%02X ", pidmap[i]);
            lcd.print(buffer);
        }
        delay(2000);

        errors = 0;
        return true;
    }
    void SendQuery()
    {
        char buf[17];
        switch (index) {
        case 0:
            sprintf(buf, "[%04X]", pid);
            break;
        case 1:
            sprintf(buf, "%03X[%01X]", pid >> 4, pid & 0xf);
            break;
        case 2:
            sprintf(buf, "%02X[%01X]%01X", pid >> 8, (pid >> 4) & 0xf, pid & 0xf);
            break;
        case 3:
            sprintf(buf, "%01X[%01X]%02X", pid >> 12, (pid >> 8) & 0xf, pid & 0xff);
            break;
        case 4:
            sprintf(buf, "[%01X]%03X", pid >> 12, pid & 0xfff);
            break;
        }

        lcd.setCursor(0, 0);
        lcd.print(buf);

        dataMode = (byte)(pid >> 8);
        sendQuery((byte)pid);
    }
    void Recover()
    {
        write('\r');
    }
    void Loop()
    {
        int value;
        char buffer[OBD_RECV_BUF_SIZE];
        bool gpsReady = false;

        // issue a query for specified OBD-II pid
        SendQuery();

        do {
#ifdef GPSUART
            // while waiting for response, test GPS
            unsigned long start = millis();
            while (GPSUART.available() && millis() - start < 100) {
                if (gps.encode(GPSUART.read())) {
                    gpsReady = true;
                }
            }
#endif
        } while (!available());

#ifdef GPSUART
        if (gpsReady)
            ShowGPSData();
#endif

        // check OBD response
        buffer[0] = 0;
        byte curpid = (byte)pid;
        char* data = getResponse(curpid, buffer);
        lcd.setCursor(6, 0);
        if (!data) {
            lcd.setCursor(0, 2);
            lcd.print("Error");
            // try recover next time
            Recover();
        } else {
                char *p = buffer;
                while (*p && *p < ' ') p++;
                for (char *q = p; *q; q++) {
                    if (*q < ' ') *q = ' ';
            }
            lcd.setCursor(0, 2);
            lcd.print(p);
            lcd.setCursor(7 * 9, 0);
            sprintf(buffer, "%d", normalizeData(curpid, data));
            lcd.print(buffer);
        }
    }
};

COBDTester obd;

// Convert ADC value to key number
char get_key(unsigned int input)
{
	char k;
    for (k = 0; k < NUM_KEYS; k++) {
		if (input < adc_key_val[k])
			return k;
	}
	return -1;
}

void testMPU6050()
{
  int error;
  float dT;
  accel_t_gyro_union accel_t_gyro;

  // Read the raw values.
  // Read 14 bytes at once,
  // containing acceleration, temperature and gyro.
  // With the default settings of the MPU-6050,
  // there is no filter enabled, and the values
  // are not very stable.

  error = MPU6050_read (MPU6050_ACCEL_XOUT_H, (uint8_t *) &accel_t_gyro, sizeof(accel_t_gyro));
  if (error != 0) {
    lcd.setCursor(0, 4);
    lcd.print("MPU6050 N/A");
    return;
  }


  // Swap all high and low bytes.
  // After this, the registers values are swapped,
  // so the structure name like x_accel_l does no
  // longer contain the lower byte.
  uint8_t swap;
  #define SWAP(x,y) swap = x; x = y; y = swap

  SWAP (accel_t_gyro.reg.x_accel_h, accel_t_gyro.reg.x_accel_l);
  SWAP (accel_t_gyro.reg.y_accel_h, accel_t_gyro.reg.y_accel_l);
  SWAP (accel_t_gyro.reg.z_accel_h, accel_t_gyro.reg.z_accel_l);
  SWAP (accel_t_gyro.reg.t_h, accel_t_gyro.reg.t_l);
  SWAP (accel_t_gyro.reg.x_gyro_h, accel_t_gyro.reg.x_gyro_l);
  SWAP (accel_t_gyro.reg.y_gyro_h, accel_t_gyro.reg.y_gyro_l);
  SWAP (accel_t_gyro.reg.z_gyro_h, accel_t_gyro.reg.z_gyro_l);


  // Print the raw acceleration values

  char buf[20];
  dT = ( (float) accel_t_gyro.value.temperature + 12412.0) / 340.0;
  sprintf(buf, "%d %d %d %d    ",
    (int)dT,
    accel_t_gyro.value.x_accel / 128,
    accel_t_gyro.value.y_accel / 128,
    accel_t_gyro.value.z_accel / 128);
  //Serial.println(buf);
  lcd.setCursor(0, 4);
  lcd.print(buf);
}

void ShowECUCap()
{
    char buffer[24];
    byte pidlist[] = {PID_RPM, PID_SPEED, PID_THROTTLE, PID_ENGINE_LOAD, PID_ABS_ENGINE_LOAD, PID_MAF_FLOW, PID_INTAKE_MAP, PID_FUEL_LEVEL, PID_FUEL_PRESSURE, PID_COOLANT_TEMP, PID_INTAKE_TEMP, PID_AMBIENT_TEMP, PID_TIMING_ADVANCE, PID_BAROMETRIC};
    const char* namelist[] = {"RPM", "SPEED", "THROTTLE", "ENG.LOAD1", "ENG.LOAD2", "MAF", "MAP", "FUEL LV.", "FUEL PRE.", "COOLANT", "INTAKE","AMBIENT", "IGNITION", "BARO"};
    byte i = 0;
    lcd.clear();
    lcd.setFont(FONT_SIZE_SMALL);
    for (; i < sizeof(pidlist) / sizeof(pidlist[0]) / 2; i++) {
        lcd.setCursor(0, i);
        sprintf(buffer, "%s:%c", namelist[i], obd.isValidPID(pidlist[i]) ? 'Y' : 'N');
        lcd.print(buffer);
    }
    for (byte row = 0; i < sizeof(pidlist) / sizeof(pidlist[0]); i++, row++) {
        lcd.setCursor(64, row);
        sprintf(buffer, "%s:%c", namelist[i], obd.isValidPID(pidlist[i]) ? 'Y' : 'N');
        lcd.print(buffer);
    }
}

void setup()
{
    Wire.begin();
    lcd.begin();
    lcd.print("OBD TESTER 1.2");
    OBDUART.begin(OBD_SERIAL_BAUDRATE);
#ifdef GPSUART
    GPSUART.begin(GPS_BAUDRATE);
#endif

    do {
        lcd.clear();
        lcd.print("Init MPU6050...");
        lcd.setCursor(0, 1);

        hasMPU6050 = MPU6050_init() == 0;
        if (hasMPU6050) {
            unsigned long t = millis();
            do {
                testMPU6050();
                delay(100);
            } while (millis() - t <= 1000);
        }
        delay(1000);

#ifdef GPSUART
        if (GPSUART.available()) {
            lcd.clear();
            lcd.print("Init GPS...");
            delay(1000);
        }
#endif
        lcd.clear();
        lcd.print("Init OBD...");
        if (hasMPU6050) {
            testMPU6050();
        }
      delay(500);
    } while(!obd.Init());

    ShowECUCap();
    delay(3000);
    lcd.clear();
    //query();
}

void loop()
{
    obd.Loop();

    // test MPU6050
    if (hasMPU6050) {
        testMPU6050();
    }
#if 0
	adc_key_in = analogRead(0);    // read the value from the sensor
	key = get_key(adc_key_in);		        // convert into key press
	if (key != oldkey) {
		delay(50);		// wait for debounce time
		adc_key_in = analogRead(0);    // read the value from the sensor
		key = get_key(adc_key_in);		        // convert into key press
		if (key != oldkey)
		{
			oldkey = key;
			if (key >=0){
				switch (key) {
				case 2: // down key
					switch (index) {
					case 0:
					    pid--;
					    break;
					case 1:
					    pid = (pid & 0xfff0) | (((pid & 0xf) - 1) & 0xf);
					    break;
					case 2:
					    pid = (pid & 0xff0f) | (((pid & 0xf0) - 0x10) & 0xf0);
					    break;
					case 3:
					    pid = (pid & 0xf0ff) | (((pid & 0xf00) - 0x100) & 0xf00);
					    break;
					case 4:
					    pid = (pid & 0x0fff) | (((pid & 0xf000) - 0x1000) & 0xf000);
					    break;
					}
					break;
				case 1: // up key
					switch (index) {
					case 0:
					    pid++;
					    break;
					case 1:
					    pid = (pid & 0xfff0) | (((pid & 0xf) + 1) & 0xf);
					    break;
					case 2:
					    pid = (pid & 0xff0f) | (((pid & 0xf0) + 0x10) & 0xf0);
					    break;
					case 3:
					    pid = (pid & 0xf0ff) | (((pid & 0xf00) + 0x100) & 0xf00);
					    break;
					case 4:
					    pid = (pid & 0x0fff) | (((pid & 0xf000) + 0x1000) & 0xf000);
					}
					break;
				case 0: // right key
					if (index > 0) index--;
					break;
				case 3: // left key
					if (index < 4) index++;
					break;
				}
				lcd.clear();
				query();
			}
		}
	}
#endif
}
