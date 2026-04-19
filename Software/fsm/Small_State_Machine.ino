#define FORCE_SENSOR_PIN 4
#define PWM_OUTPUT_PIN   2
#define DIGITAL_OUT_PIN  1  
#define BUTTON_OUT       18
#define BUTTON_IN        17
#define RELAY            16
#define TELLER           5          

#define FORCE_THRESHOLD  1000

bool break_flag = true;

void setup() {
  Serial.begin(9600);

  analogSetAttenuation(ADC_11db);

  pinMode(DIGITAL_OUT_PIN, OUTPUT);
  digitalWrite(DIGITAL_OUT_PIN, HIGH);

  pinMode(BUTTON_OUT, OUTPUT);
  digitalWrite(BUTTON_OUT, HIGH);

  pinMode(BUTTON_IN, INPUT);

  pinMode(RELAY, OUTPUT);
  digitalWrite(RELAY, HIGH);

  pinMode(BUTTON_OUT, OUTPUT);
  digitalWrite(BUTTON_OUT, LOW);

  analogWriteResolution(PWM_OUTPUT_PIN, 8);
  analogWriteFrequency(PWM_OUTPUT_PIN, 5000);
  analogWrite(PWM_OUTPUT_PIN, 0);

}

void loop() {
  int analogReading = analogRead(FORCE_SENSOR_PIN);

  int pwmValue = map(analogReading, 0, 4095, 0, 255);
  pwmValue = constrain(pwmValue, 0, 255);


  if (digitalRead(BUTTON_IN) == HIGH) {
    digitalWrite(RELAY, LOW);
    digitalWrite(TELLER, HIGH);
    break_flag = false;
  } 
  else {
    digitalWrite(RELAY, HIGH);
    digitalWrite(TELLER, LOW);
    break_flag = true;
  }

  if ((analogReading <= FORCE_THRESHOLD) && break_flag) {
    digitalWrite(DIGITAL_OUT_PIN, HIGH);
    analogWrite(PWM_OUTPUT_PIN, 0);   // important fix
  } else {
    digitalWrite(DIGITAL_OUT_PIN, LOW);
    analogWrite(PWM_OUTPUT_PIN, pwmValue);
  }

  Serial.print("Force = ");
  Serial.print(analogReading);
  Serial.print(" | PWM = ");
  Serial.print((analogReading <= FORCE_THRESHOLD && break_flag) ? 0 : pwmValue);
  Serial.print(" | OUT = ");
  Serial.print((analogReading <= FORCE_THRESHOLD && break_flag) ? "LOW" : "HIGH");
  Serial.print(" | break_flag = ");
  Serial.println(break_flag ? "true" : "false");

  delay(20);
}