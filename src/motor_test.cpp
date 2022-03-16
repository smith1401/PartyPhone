#include <Arduino.h>
#include <AFMotor.h>

#define MOTOR_NUM 4 // connect DC motor at terminal M4

AF_DCMotor motor(MOTOR_NUM);

unsigned int motorSpeed = 255; // set the speed to 100/255

void setup()
{
    Serial.begin(115200);
    motor.run(RELEASE);
    motor.setSpeed(motorSpeed);
    motor.run(FORWARD);   // turn motor on going forward
}

void loop()
{
    // motor.setSpeed(motorSpeed);
    // // Serial.println("Set motor speed to " + String(motorSpeed));

    // // Serial.println("  -> rotate forward");
    // motor.run(FORWARD);   // turn motor on going forward
    // delay(3000);
    // motor.run(RELEASE);   // stop motor
    // delay(1000);

    // Serial.println("  -> rotate backwards");
    // motor.run(BACKWARD);   // turn motor on going backwards
    // delay(3000);
    // motor.run(RELEASE);    // stop motor
    // delay(1000);

    // motorSpeed += 20;
    // if (motorSpeed > 255) {
    //     motorSpeed = 100;
    // }
}