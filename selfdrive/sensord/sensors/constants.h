#pragma once


#define SENSOR_ACCELEROMETER 1
#define SENSOR_MAGNETOMETER 2
#define SENSOR_MAGNETOMETER_UNCALIBRATED 3
#define SENSOR_GYRO 4
#define SENSOR_GYRO_UNCALIBRATED 5
#define SENSOR_LIGHT 7

#define SENSOR_TYPE_ACCELEROMETER 1
#define SENSOR_TYPE_GEOMAGNETIC_FIELD 2
#define SENSOR_TYPE_GYROSCOPE 4
#define SENSOR_TYPE_LIGHT 5
#define SENSOR_TYPE_AMBIENT_TEMPERATURE 13
#define SENSOR_TYPE_MAGNETIC_FIELD_UNCALIBRATED 14
#define SENSOR_TYPE_MAGNETIC_FIELD  SENSOR_TYPE_GEOMAGNETIC_FIELD
#define SENSOR_TYPE_GYROSCOPE_UNCALIBRATED 16

constexpr const char* PM_GYRO =  "gyroscope";
constexpr const char* PM_ACCEL = "accelerometer";
constexpr const char* PM_GYRO_2 =  "gyroscope2";     // BMX
constexpr const char* PM_ACCEL_2 = "accelerometer2"; // BMX
constexpr const char* PM_MAGN =  "magnetometer";
constexpr const char* PM_LIGHT = "lightSensor";
constexpr const char* PM_TEMP =  "temperatureSensor";
