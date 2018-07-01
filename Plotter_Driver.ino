#include <LiquidCrystal_I2C.h>
#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <Wire.h>
#include <LiquidCrystal_I2C\LiquidCrystal_I2C.h>
#include <Servo.h>

#define toSteps(mm) (long)(mm * step_conv)
#define toMM(step_ps) (float)(step_ps / step_conv)


// GLOBALS

constexpr unsigned short
	step_pres = 16,
	step_delay_min = 110,
	step_delay_default = 6000,
	sd_cs = 10,
	pdata0 = 2,
	pdata1 = 3;
constexpr float
	step_conv = 1 / (0.04875 / step_pres);

unsigned long bedLength = toSteps(350),
	bedWidth = toSteps(250);
long headX = 0, 
	headY = 0,
	headZ = 0,
	xVal = 0, 
	yVal = 0,
	rVal = 0, 
	iVal = 0, 
	jVal = 0;
int step_delay = step_delay_min, //microsecond delay
	arcRes = 100; //point per 360° arc;
bool swingCW = true;
String gcodeData;


// STEPPER

struct stepper {
	const unsigned char nsw_p;
	const unsigned char dir_p;
	const unsigned char step_p;
	bool bstep_p;
	const bool bout; //pin state for moving head outwards
	void init();
	void step();
	void calibrate();
} stepm1 = { A3, 6, 7, 0, 0 },
  stepm2 = { A2, 4, 5 , 0, 1 };


// SERVO

struct pen_servo {
	const unsigned char data_p;
	pen_servo(const unsigned char _data_p) : data_p(_data_p) {}
	Servo serv;
	void init();
	void moveDown();
	void moveUp();
	void destruct();
} pen(9);


// LCD

LiquidCrystal_I2C lcd(0x27, 16, 2);
void lcdPrint(const char* msg, const int l);


// OTHER

char OPCIndex(const char* arr[], const int length, const char* find); 


// PLOTTER HEAD

void moveAbs();
void moveRel();
void arcCW();
void arcCCW();

void moveHead(const unsigned long x, const unsigned long y);
void arcHead(const unsigned long xv, const unsigned long yv, const unsigned long rv, const unsigned long iv, const unsigned long jv);
void arcHead(const unsigned long xv, const unsigned long yv, const unsigned long iv, const unsigned long jv);
void arcHead(const unsigned long xv, const unsigned long yv, const unsigned long rv);


// OPCODES

void evaluateGC(String);
void parseCommand(String);
void translateGC();

const char* opcodes[] = {
	"G00", //move absolute
	"G01", //move relative
	"G02", //trun CW
	"G03", //turn CCW
};
void(*opfuncs[])() = {
	&moveAbs,
	&moveRel,
	&arcCW,
	&arcCCW,
};

struct parampair {
	const char param;
	long* assoc;
} paramcodes[] = {
	{ 'X', &xVal },
	{ 'Y', &yVal },
	{ 'Z', &headZ },
	{ 'I', &iVal },
	{ 'J', &jVal },
	{ 'R', &rVal },
};


// ==== FUNCTION DEFINITIONS ==== //

// LCD

void lcdPrint(const char* msg, const int l)
{
	lcd.setCursor(0, l);
	for (unsigned char i = 0; i < strlen(msg); i++)
	{
		lcd.print(msg[i]);
	}
	for (unsigned char i = 0; i < 16 - strlen(msg); i++) {
		lcd.print(' ');
	}
}


// STEPPER

void stepper::init()
{
	pinMode(nsw_p, INPUT);
	pinMode(dir_p, OUTPUT);
	pinMode(step_p, OUTPUT);

	digitalWrite(nsw_p, HIGH);
	digitalWrite(dir_p, bout);
	digitalWrite(step_p, bstep_p);
}

void stepper::step()
{
	bstep_p = !bstep_p;
	digitalWrite(step_p, bstep_p);
	delayMicroseconds(step_delay);
}

void stepper::calibrate()
{
	digitalWrite(dir_p, !bout);
	while (digitalRead(nsw_p))
	{
		step();
	}
	//Serial.println("Reached zero!");
}


//SERVO

void pen_servo::init() 
{
	serv.attach(data_p);
}

void pen_servo::moveDown()
{
	serv.write(110);
}

void pen_servo::moveUp()
{
	serv.write(90);
}

void pen_servo::destruct() {
	serv.detach();
}


// HEAD MOVEMENT

void headOn()
{
	digitalWrite(pdata0, HIGH);
}
void headOff()
{
	digitalWrite(pdata0, LOW);
}

void moveHead(const unsigned long x, const unsigned long y) 
{
	//Serial.println("Move: " + String(x) + ", " + String(y));
	if (x > bedWidth || x < 0 || y > bedLength || y < 0) {
		Serial.println(x + String(",") + y + String(":") + bedLength + String(",") + bedWidth);
		lcdPrint(String("x:" + String(x) + ",y:" + String(y)).c_str(), 1);
		lcdPrint("Invalid X/Y!", 0);
		return;
	}

	long dx = x - headX,
		dy = y - headY,
		ystep_ps = 0;

	digitalWrite(stepm2.dir_p, (dy > 0) ? stepm2.bout : !stepm2.bout);
	digitalWrite(stepm1.dir_p, (dx > 0) ? stepm1.bout : !stepm1.bout);

	dx = abs(dx);
	dy = abs(dy);

	for (int i = 0; i < dx; i++)
	{
		stepm1.step();
		while (map(i, 0, dx, 0, dy) - ystep_ps >= 1) 
		{
			stepm2.step();
			ystep_ps++;
		}
	}
	while (ystep_ps < dy) 
	{
		stepm2.step();
		ystep_ps++;
	}

	headX = x;
	headY = y;
}

void arcHead(const unsigned long xv, const unsigned long yv, const unsigned long rv, const unsigned long iv, const unsigned long jv)
{
	long arcX = 0,
		arcY = 0,
		initX = headX,
		initY = headY;
	float headA = (-jv < 0 && -iv < 0 ? 180 : 0) + degrees(iv == 0 ? (float) 3/2 * PI : (float) atan((float) jv / iv)), //TODO add side of head point
		newA = ((jv + initY - yv) < 0 && (iv + initX - xv) < 0 ? 180 : 0) + degrees((iv + initX - xv) == 0 ? (float) 3/2 * PI : (float) atan((float) (jv + initY - yv) / (iv + initX - xv))); //this one too
	
	//Serial.println("Angs: " + String(headA) + ", " + String(newA));
	long dist = swingCW ? (headA - newA) : (newA - headA);
	Serial.println(dist + ((dist <= 0) ? 360 : 0));
	for (float i = 0; i <= dist + ((dist <= 0) ? 360 : 0); i += 360 / arcRes)
	{
		Serial.println(i);
		arcX = cos(radians((swingCW ? -1 : 1) * i + headA)) * rv + iv + initX;
		arcY = sin(radians((swingCW ? -1 : 1) * i + headA)) * rv + jv + initY;
		moveHead(arcX, arcY);
	}
	moveHead(xv, yv);
}
void arcHead(const unsigned long xv, const unsigned long yv, const unsigned long iv, const unsigned long jv)
{
	arcHead(xv, yv, sqrt(sq(headX - iVal) + sq(headY - jVal)), iv, jv);
}
void arcHead(const unsigned long xv, const unsigned long yv, const unsigned long rv)
{
	arcHead(xv, yv, rv, abs(xVal - headX) / 2, sqrt(sq(rVal) - sq(iVal)));
}

void arc() {
	if (!rVal) {
		if (iVal && jVal) {
			arcHead(xVal, yVal, iVal, jVal);
		}
	}
	else if (!iVal || !jVal) {
		arcHead(xVal, yVal, rVal);
	}
	else {
		arcHead(xVal, yVal, rVal, iVal, jVal);
	}
}
void arcCW() {
	swingCW = true;
	arc();
}
void arcCCW() {
	swingCW = false;
	arc();
}
void moveAbs() {
	moveHead(xVal, yVal);
}
void moveRel() {
	moveHead(xVal + headX, yVal + headY);
}


// OTHER

char OPCIndex(const char* arr[], const int length, const char* find) 
{
	for (int i = 0; i < length; i++) 
	{
		if (strcmp(arr[i], find) == 0) return i;
	}
	return -1;
}


// OPCODES

void translateGC() 
{
	int ni = 0;
	for (int i = 0; i <= gcodeData.length(); i++) 
	{
		if (gcodeData.charAt(i) == '\n' || i == gcodeData.length()) {
			String line = gcodeData.substring(ni, i);
			line.replace("\n", "");
			evaluateGC(line);
			//abort -? !TODO
			ni = i;
		}
	}
}

void evaluateGC(const String command) 
{
	char si = command.indexOf(' '),
		opind = -1;
	if (si == -1) { //no params
		si = command.length(); //normally impossible
	}

	Serial.println(command);
	opind = OPCIndex(opcodes, sizeof(opcodes)/sizeof(const char *), command.substring(0, si).c_str());
	if (opind == -1) {
		lcd.clear();
		lcdPrint("Unknown OPCODE", 0);
		return;
	}
	Serial.println(opcodes[opind]);

	if (si != command.length()) {
		parseCommand(command.substring(si, command.length()));
	}
	
	if (headZ != NULL) {
		step_delay = step_delay_default;
		headOn();
	} else {
		headOff();
		step_delay = step_delay_min;
	}

	if (opind < sizeof(opfuncs) / sizeof(void(*)())) {
		opfuncs[opind]();
	}
}

void parseCommand(const String params)
{
	xVal = headX;
	yVal = headY;
	rVal = 0;
	jVal = 0;
	iVal = 0;
	int si = 0;
	for (int i = 1; i <= params.length(); i++) { //start at 1 to avoid parsing first space
		if (params.charAt(i) == ' ' || i == params.length()) {
			String param = params.substring(si + 1, i);
			for (int j = 0; j < sizeof(paramcodes); j++) {
				if (paramcodes[j].param == param[0]) {
					Serial.println(param[0] + String(toSteps(param.substring(1, param.length()).toFloat())));
					*(paramcodes[j].assoc) = toSteps(param.substring(1, param.length()).toFloat());
					break;
				}
			}
			si = i;
		}
	}
}


// SETUP

void initPlotter()
{
	pinMode(pdata0, OUTPUT);
	pinMode(pdata1, OUTPUT);
	digitalWrite(pdata0, LOW);
	digitalWrite(pdata1, LOW);

	if (!SD.begin(sd_cs)) {
		lcdPrint("SDCARD not found", 0);
		return;
	}
	File gcFile = SD.open("gcode.txt");

	if (!gcFile) {
		lcdPrint("GCODE not found", 0);
		return;
	}

	lcdPrint("Found gcode.txt!", 0);
	delay(500);
	lcd.clear();
	lcdPrint("Initializing", 0);
	lcdPrint("Plotter...", 1);

	while (gcFile.available()) {
		gcodeData += (char) gcFile.read();
	}
	
	pen.init();
	pen.moveDown();
	delay(1000);
	pen.moveUp();
	delay(1000);

	stepm1.init();
	stepm2.init();
	calibrate();

	lcd.clear();
	lcdPrint("Plotting...", 0);
	translateGC();
}

void calibrate()
{
	step_delay = step_delay_min;
	stepm1.calibrate();
	stepm2.calibrate();
	headX = 0;
	headY = 0;
	headZ = 0;
}

// ==== END ==== //


// MAIN

void setup()
{
	Serial.begin(9600);
	lcd.init();
	lcd.backlight();

	initPlotter();

	lcdPrint("Done!", 0);
}

void loop() {}