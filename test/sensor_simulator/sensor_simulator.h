/****************************************************************************
 *
 *   Copyright (c) 2019 ECL Development Team. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name PX4 nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/

/**
 * This class is providing methods to feed the ECL EKF with measurement.
 * It takes a pointer to the Ekf object and will manipulate the object
 * by call set*Data functions.
 * It simulates the time to allow for sensor data being set at certain rate
 * and also calls the update method of the EKF
 * @author Kamil Ritz <ka.ritz@hotmail.com>
 */

#pragma once

#include <memory>

#include "imu.h"
#include "mag.h"
#include "baro.h"
#include "gps.h"
#include "EKF/ekf.h"

using namespace sensor_simulator::sensor;

class SensorSimulator
{
public:
	SensorSimulator(std::shared_ptr<Ekf> ekf);
	~SensorSimulator();

	void setImuRate(uint32_t rate){ _imu.setRate(rate); }
	void setMagRate(uint32_t rate){ _mag.setRate(rate); }
	void setBaroRate(uint32_t rate){ _baro.setRate(rate); }
	void setGpsRate(uint32_t rate){ _gps.setRate(rate); }

	void run_seconds(float duration_seconds);
	void run_microseconds(uint32_t duration);

	void startGps(){ _gps.start(); }
	void stopGps(){ _gps.stop(); }

	void setImuBias(Vector3f accel_bias, Vector3f gyro_bias);

private:
	std::shared_ptr<Ekf> _ekf;

	Imu _imu;
	Mag _mag;
	Baro _baro;
	Gps _gps;

	uint32_t _time {0};	// in microseconds

	gps_message getDefaultGpsData();

};
