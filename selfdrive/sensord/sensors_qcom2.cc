#include <sys/resource.h>

#include <chrono>
#include <thread>
#include <vector>
#include <poll.h>
#include <linux/gpio.h>

// TODO: check if really needed
#include <poll.h>
#include <fcntl.h>

#include "cereal/messaging/messaging.h"
#include "common/i2c.h"
#include "common/swaglog.h"
#include "common/timing.h"
#include "common/util.h"
#include "selfdrive/sensord/sensors/bmx055_accel.h"
#include "selfdrive/sensord/sensors/bmx055_gyro.h"
#include "selfdrive/sensord/sensors/bmx055_magn.h"
#include "selfdrive/sensord/sensors/bmx055_temp.h"
#include "selfdrive/sensord/sensors/constants.h"
#include "selfdrive/sensord/sensors/light_sensor.h"
#include "selfdrive/sensord/sensors/lsm6ds3_accel.h"
#include "selfdrive/sensord/sensors/lsm6ds3_gyro.h"
#include "selfdrive/sensord/sensors/lsm6ds3_temp.h"
#include "selfdrive/sensord/sensors/mmc5603nj_magn.h"
#include "selfdrive/sensord/sensors/sensor.h"

#define I2C_BUS_IMU 1

ExitHandler do_exit;
std::mutex pm_mutex;

// filter first values (0.5sec) as those may contain inaccuracies
uint64_t init_ts = 0;
constexpr uint64_t init_delay = 500*1e6;

void interrupt_loop(int fd, std::vector<Sensor *>& sensors, PubMaster& pm) {
  struct pollfd fd_list[1] = {0};
  fd_list[0].fd = fd;
  fd_list[0].events = POLLIN | POLLPRI;

  uint64_t offset = nanos_since_epoch() - nanos_since_boot();

  while (!do_exit) {
    int err = poll(fd_list, 1, 100);
    if (err == -1) {
      if (errno == EINTR) {
        continue;
      }
      return;
    } else if (err == 0) {
      LOGE("poll timed out");
      continue;
    }

    if ((fd_list[0].revents & (POLLIN | POLLPRI)) == 0) {
      LOGE("no poll events set");
      continue;
    }

    // Read all events
    struct gpioevent_data evdata[16];
    err = read(fd, evdata, sizeof(evdata));
    if (err < 0 || err % sizeof(*evdata) != 0) {
      LOGE("error reading event data %d", err);
      continue;
    }

    int num_events = err / sizeof(*evdata);
    uint64_t ts = evdata[num_events - 1].timestamp - offset;

    MessageBuilder msg;
    auto orphanage = msg.getOrphanage();
    std::vector<capnp::Orphan<cereal::SensorEventData>> collected_events;
    collected_events.reserve(sensors.size());

    for (Sensor *sensor : sensors) {
      auto orphan = orphanage.newOrphan<cereal::SensorEventData>();
      auto event = orphan.get();
      if (!sensor->get_event(event)) {
        continue;
      }

      event.setTimestamp(ts);
      collected_events.push_back(kj::mv(orphan));
    }

    if (collected_events.size() == 0) {
      continue;
    }

    auto events = msg.initEvent().initSensorEvents(collected_events.size());
    for (int i = 0; i < collected_events.size(); ++i) {
      events.adoptWithCaveats(i, kj::mv(collected_events[i]));
    }

    if (ts - init_ts < init_delay) {
      continue;
    }

    std::lock_guard<std::mutex> lock(pm_mutex);
    pm.send("sensorEvents", msg);
  }

  // poweroff sensors, disable interrupts
  for (Sensor *sensor : sensors) {
    sensor->shutdown();
  }
}

int sensor_loop() {
  I2CBus *i2c_bus_imu;

  try {
    i2c_bus_imu = new I2CBus(I2C_BUS_IMU);
  } catch (std::exception &e) {
    LOGE("I2CBus init failed");
    return -1;
  }

  BMX055_Accel bmx055_accel(i2c_bus_imu);
  BMX055_Gyro bmx055_gyro(i2c_bus_imu);
  BMX055_Magn bmx055_magn(i2c_bus_imu);
  BMX055_Temp bmx055_temp(i2c_bus_imu);

  LSM6DS3_Accel lsm6ds3_accel(i2c_bus_imu, GPIO_LSM_INT);
  LSM6DS3_Gyro lsm6ds3_gyro(i2c_bus_imu, GPIO_LSM_INT, true); // GPIO shared with accel
  LSM6DS3_Temp lsm6ds3_temp(i2c_bus_imu);

  MMC5603NJ_Magn mmc5603nj_magn(i2c_bus_imu);

  LightSensor light("/sys/class/i2c-adapter/i2c-2/2-0038/iio:device1/in_intensity_both_raw");

  // Sensor init
  std::vector<std::pair<Sensor *, bool>> sensors_init; // Sensor, required
  //sensors_init.push_back({&bmx055_accel, false});
  //sensors_init.push_back({&bmx055_gyro, false});
  sensors_init.push_back({&bmx055_magn, false});
  //sensors_init.push_back({&bmx055_temp, false});

  //sensors_init.push_back({&lsm6ds3_accel, true});
  //sensors_init.push_back({&lsm6ds3_gyro, true});
  //sensors_init.push_back({&lsm6ds3_temp, true});

  //sensors_init.push_back({&mmc5603nj_magn, false});

  //sensors_init.push_back({&light, true});

  bool has_magnetometer = false;

  // Initialize sensors
  std::vector<Sensor *> sensors;
  for (auto &sensor : sensors_init) {
    int err = sensor.first->init();
    if (err < 0) {
      // Fail on required sensors
      if (sensor.second) {
        LOGE("Error initializing sensors");
        delete i2c_bus_imu;
        return -1;
      }
    } else {
      if (sensor.first == &bmx055_magn || sensor.first == &mmc5603nj_magn) {
        has_magnetometer = true;
      }

      if (!sensor.first->has_interrupt_enabled()) {
        sensors.push_back(sensor.first);
      }
    }
  }

  if (!has_magnetometer) {
    LOGE("No magnetometer present");
    delete i2c_bus_imu;
    return -1;
  }

  PubMaster pm({"sensorEvents"});
  init_ts = nanos_since_boot();

  // thread for reading events via interrupts
  std::vector<Sensor *> lsm_interrupt_sensors = {&lsm6ds3_accel, &lsm6ds3_gyro};
  std::thread lsm_interrupt_thread(&interrupt_loop, lsm6ds3_accel.gpio_fd, std::ref(lsm_interrupt_sensors), std::ref(pm));

  // polling loop for non interrupt handled sensors
  while (!do_exit) {
    std::chrono::steady_clock::time_point begin = std::chrono::steady_clock::now();


  const int num_events = sensors.size();

  struct pollfd fdlist[3];

  // assumed to be configured as exported, defined as input and trigger on raising edge
  // TODO: cleanUp but works for now
  int fd_accel = open("/sys/class/gpio/gpio21/value", O_RDONLY);
  if (fd_accel < 0) { LOGE("FD ACCEL failed"); return 0; }
  int fd_gyro  = open("/sys/class/gpio/gpio23/value", O_RDONLY);
  if (fd_gyro < 0)  { LOGE("FD GYRO failed"); return 0; }
  int fd_magn  = open("/sys/class/gpio/gpio87/value", O_RDONLY);
  if (fd_magn < 0)  { LOGE("FD MAGN failed"); return 0; }

  fdlist[0].fd = fd_accel;
  fdlist[0].events = POLLPRI;

  fdlist[1].fd = fd_gyro;
  fdlist[1].events = POLLPRI;

  fdlist[2].fd = fd_magn;
  fdlist[2].events = POLLPRI;

  uint64_t start_time = nanos_since_boot();
  uint64_t f_avg = 0;
  uint64_t a_cnt = 0;

  while (1) {
    int err;

    /* events are received at 2kHz frequency, the bandwidth devider returns in the case of 125Hz
       7 times the same data, this needs to be filtered or handled smart */
    err = poll(fdlist, 3, -1);
    if (-1 == err) {
      return -1;
    }

    // TODO: check which interrupt triggered using revents

    // timing measurement
    uint64_t int_time = nanos_since_boot();
    f_avg += int_time - start_time;
    a_cnt++;
    LOGE("t: %lu, %lu", int_time - start_time, f_avg/a_cnt);
    start_time = int_time;

    // we dont read from the fd as its reset automatically after 50us
    // so we directly create the message and publish it
    MessageBuilder msg;
    auto sensor_events = msg.initEvent().initSensorEvents(num_events);

    for (int i = 0; i < num_events; i++) {
      auto event = sensor_events[i];
      sensors[i]->get_event(event);
    }

    if (nanos_since_boot() - init_ts < init_delay) {
      continue;
    }

    {
      std::lock_guard<std::mutex> lock(pm_mutex);
      pm.send("sensorEvents", msg);
    }

    std::chrono::steady_clock::time_point end = std::chrono::steady_clock::now();
    std::this_thread::sleep_for(std::chrono::milliseconds(10) - (end - begin));
  }

  for (Sensor *sensor :  sensors) {
    sensor->shutdown();
  }

  lsm_interrupt_thread.join();
  delete i2c_bus_imu;
  return 0;
}

int main(int argc, char *argv[]) {
  setpriority(PRIO_PROCESS, 0, -18);
  return sensor_loop();
}
