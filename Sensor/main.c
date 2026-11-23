void setup() {
  Serial.begin(9600);
  pinMode(2, INPUT);            // Salida OUT del HW-870 al pin D2
  pinMode(LED_BUILTIN, OUTPUT); // LED integrado de la placa
}

void loop() {
  int detectado = (digitalRead(2) == LOW); // El HW-870 entrega LOW al detectar
  digitalWrite(LED_BUILTIN, detectado);
  Serial.println(detectado);
  delay(50);
}