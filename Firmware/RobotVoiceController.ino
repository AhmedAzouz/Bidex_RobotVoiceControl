#include <Servo.h>

// NodeMCU ESP8266 wiring
const uint8_t WRIST_PIN   = D1;  // GPIO5
const uint8_t GRIPPER_PIN = D2;  // GPIO4
const uint8_t BUZZER_PIN  = D5;  // GPIO14; passive KC1206 through suitable driver

// Calibrate these two starting positions to match the physical robot.
const int WRIST_START_ANGLE   = 90;
const int GRIPPER_START_ANGLE = 90;

// Non-blocking movement tuning. One degree every 12 ms ~= 83 degrees/second.
const unsigned long MOVE_INTERVAL_MS = 12;
const unsigned long DETACH_DELAY_MS = 300;
const unsigned long MOVE_CHIRP_INTERVAL_MS = 95;
const unsigned long MOVE_CHIRP_DURATION_MS = 24;

Servo wristServo;
Servo gripperServo;

struct Axis {
  Servo* servo;
  uint8_t pin;
  int current;
  int target;
  bool attached;
  bool moving;
  unsigned long nextStepAt;
  unsigned long lastMotionAt;
  const char* name;
};

Axis wrist = { &wristServo, WRIST_PIN, WRIST_START_ANGLE, WRIST_START_ANGLE,
               false, false, 0, 0, "wrist" };
Axis gripper = { &gripperServo, GRIPPER_PIN, GRIPPER_START_ANGLE, GRIPPER_START_ANGLE,
                 false, false, 0, 0, "gripper" };

bool randomMode = false;
bool randomWristNext = true;
unsigned long nextRandomAt = 0;
bool detachSoundArmed = false;
unsigned long lastMovementChirpAt = 0;

// One piezo/electromagnetic buzzer output. All sound timing is non-blocking.
enum SoundPattern { SOUND_NONE, SOUND_ACK, SOUND_START, SOUND_FINISH, SOUND_STOP, SOUND_DETACH };
SoundPattern soundPattern = SOUND_NONE;
int soundIndex = 0;
unsigned long nextToneAt = 0;
unsigned long activeToneUntil = 0;
bool toneActive = false;

bool anyMoving() {
  return wrist.moving || gripper.moving;
}

bool anyAttached() {
  return wrist.attached || gripper.attached;
}

void startPattern(SoundPattern pattern) {
  // Do not continually restart an acknowledgement when a HOME command sends two lines.
  if (pattern == SOUND_ACK && soundPattern == SOUND_ACK) return;

  noTone(BUZZER_PIN);
  toneActive = false;
  soundPattern = pattern;
  soundIndex = 0;
  nextToneAt = millis();
}

bool getPatternNote(SoundPattern pattern, int index,
                    int &frequency, unsigned long &duration) {
  switch (pattern) {
    case SOUND_ACK:
      if (index == 0) { frequency = 950;  duration = 45; return true; }
      if (index == 1) { frequency = 1380; duration = 55; return true; }
      break;
    case SOUND_START:
      if (index == 0) { frequency = 500;  duration = 50; return true; }
      if (index == 1) { frequency = 780;  duration = 50; return true; }
      if (index == 2) { frequency = 1120; duration = 50; return true; }
      if (index == 3) { frequency = 1550; duration = 65; return true; }
      break;
    case SOUND_FINISH:
      if (index == 0) { frequency = 1200; duration = 45; return true; }
      if (index == 1) { frequency = 1580; duration = 45; return true; }
      if (index == 2) { frequency = 1880; duration = 65; return true; }
      break;
    case SOUND_STOP:
      if (index == 0) { frequency = 950; duration = 65; return true; }
      if (index == 1) { frequency = 620; duration = 90; return true; }
      break;
    case SOUND_DETACH:
      // A distinct descending two-note release sound when servo signals are detached.
      if (index == 0) { frequency = 1050; duration = 35; return true; }
      if (index == 1) { frequency = 520;  duration = 70; return true; }
      break;
    default:
      break;
  }
  return false;
}

void serviceSound(unsigned long now) {
  if (toneActive && (long)(now - activeToneUntil) >= 0) {
    noTone(BUZZER_PIN);
    toneActive = false;
  }

  if (soundPattern != SOUND_NONE) {
    if (!toneActive && (long)(now - nextToneAt) >= 0) {
      int frequency = 0;
      unsigned long duration = 0;
      if (getPatternNote(soundPattern, soundIndex, frequency, duration)) {
        tone(BUZZER_PIN, frequency);
        toneActive = true;
        activeToneUntil = now + duration;
        nextToneAt = activeToneUntil + 18;
        soundIndex++;
      } else {
        soundPattern = SOUND_NONE;
      }
    }
    return;
  }

  // Movement chirps are generated only in the gaps between the short sound patterns.
  if (anyMoving() && !toneActive && now - lastMovementChirpAt >= MOVE_CHIRP_INTERVAL_MS) {
    lastMovementChirpAt = now;
    int angle = wrist.moving ? wrist.current : gripper.current;
    int frequency = map(angle, 0, 180, 420, 1900);
    tone(BUZZER_PIN, frequency);
    toneActive = true;
    activeToneUntil = now + MOVE_CHIRP_DURATION_MS;
  }
}

void beginMove(Axis &axis, int target) {
  target = constrain(target, 0, 180);

  if (target == axis.current) {
    Serial.print("Already at target: ");
    Serial.println(axis.name);
    return;
  }

  if (!axis.attached) {
    axis.servo->attach(axis.pin);
    axis.attached = true;
    axis.servo->write(axis.current);
  }

  axis.target = target;
  axis.moving = true;
  axis.nextStepAt = millis();
  axis.lastMotionAt = millis();
  detachSoundArmed = true;
}

void serviceAxis(Axis &axis, unsigned long now) {
  if (axis.moving && (long)(now - axis.nextStepAt) >= 0) {
    axis.nextStepAt = now + MOVE_INTERVAL_MS;

    if (axis.current < axis.target) axis.current++;
    else if (axis.current > axis.target) axis.current--;

    axis.servo->write(axis.current);
    axis.lastMotionAt = now;

    if (axis.current == axis.target) {
      axis.moving = false;
      axis.lastMotionAt = now;
      Serial.print("Move complete: ");
      Serial.print(axis.name);
      Serial.print(" = ");
      Serial.println(axis.current);
    }
  }

  if (!axis.moving && axis.attached && now - axis.lastMotionAt >= DETACH_DELAY_MS) {
    axis.servo->detach();
    axis.attached = false;
    Serial.print("Servo detached: ");
    Serial.println(axis.name);
    if (detachSoundArmed) {
      detachSoundArmed = false;
      startPattern(SOUND_DETACH);
    }
  }
}

void stopAxis(Axis &axis) {
  if (axis.moving) {
    axis.target = axis.current;
    axis.moving = false;
    axis.lastMotionAt = millis();
    detachSoundArmed = true;
  }
}

void printStatus() {
  Serial.print("Wrist=");
  Serial.print(wrist.current);
  Serial.print("; Gripper=");
  Serial.print(gripper.current);
  Serial.print("; Random=");
  Serial.print(randomMode ? "ON" : "OFF");
  Serial.print("; Moving=");
  Serial.println(anyMoving() ? "YES" : "NO");
}

bool parseAngle(const String &text, int &angle) {
  String value = text;
  value.trim();
  if (value.length() == 0 || value.length() > 3) return false;
  for (unsigned int i = 0; i < value.length(); i++) {
    if (!isDigit(value.charAt(i))) return false;
  }
  angle = value.toInt();
  return angle >= 0 && angle <= 180;
}

void handleCommand(String command) {
  command.trim();
  if (command.length() == 0) return;

  char cmd = command.charAt(0);

  if (cmd == 's' || cmd == 'S') {
    printStatus();
    return;
  }

  if (cmd == 'A' || cmd == 'a') {
    randomMode = true;
    randomWristNext = true;
    nextRandomAt = millis();
    startPattern(SOUND_START);
    Serial.println("Random mode ON. Send X to stop.");
    return;
  }

  if (cmd == 'X' || cmd == 'x') {
    randomMode = false;
    stopAxis(wrist);
    stopAxis(gripper);
    startPattern(SOUND_STOP);
    Serial.println("Random mode OFF; current motion stopped.");
    return;
  }

  if (cmd == 'w' || cmd == 'W' || cmd == 'g' || cmd == 'G') {
    int separator = command.indexOf(' ');
    if (separator < 0) {
      Serial.println("Invalid command. Use w 120 or g 80.");
      return;
    }

    int angle = 0;
    if (!parseAngle(command.substring(separator + 1), angle)) {
      Serial.println("Invalid angle. Allowed range: 0..180.");
      return;
    }

    randomMode = false;
    startPattern(SOUND_ACK);
    if (cmd == 'w' || cmd == 'W') beginMove(wrist, angle);
    else beginMove(gripper, angle);
    return;
  }

  Serial.println("Unknown command. Use w <angle>, g <angle>, s, A or X.");
}

String serialLine;

void readSerialNonBlocking() {
  while (Serial.available() > 0) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      if (serialLine.length() > 0) {
        handleCommand(serialLine);
        serialLine = "";
      }
    } else if (serialLine.length() < 60) {
      serialLine += c;
    } else {
      serialLine = "";
      Serial.println("Command too long; discarded.");
    }
  }
}

void setup() {
  Serial.begin(115200);
  serialLine.reserve(64);

  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  randomSeed(analogRead(A0));

  wristServo.attach(WRIST_PIN);
  gripperServo.attach(GRIPPER_PIN);
  wrist.attached = true;
  gripper.attached = true;
  wristServo.write(wrist.current);
  gripperServo.write(gripper.current);
  wrist.lastMotionAt = millis();
  gripper.lastMotionAt = millis();

  Serial.println();
  Serial.println("Robot voice firmware ready.");
  Serial.println("Commands: w 120 | g 80 | s | A | X");
}

void loop() {
  readSerialNonBlocking();

  unsigned long now = millis();
  bool wasMoving = anyMoving();

  serviceAxis(wrist, now);
  serviceAxis(gripper, now);

  // One finishing melody after the full move, not one per servo axis.
  if (wasMoving && !anyMoving() && !randomMode) {
    startPattern(SOUND_FINISH);
  }

  if (randomMode && !anyMoving() && (long)(now - nextRandomAt) >= 0) {
    if (randomWristNext) beginMove(wrist, random(20, 161));
    else beginMove(gripper, random(20, 161));
    randomWristNext = !randomWristNext;
    nextRandomAt = now + 120;
  }

  serviceSound(now);
}
