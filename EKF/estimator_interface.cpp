/****************************************************************************
 *
 *   Copyright (c) 2013 Estimation and Control Library (ECL). All rights reserved.
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
 * 3. Neither the name ECL nor the names of its contributors may be
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
 * @file estimator_interface.cpp
 * Definition of base class for attitude estimators
 *
 * @author Roman Bast <bapstroman@gmail.com>
 * @author Paul Riseborough <p_riseborough@live.com.au>
 * @author Siddharth B Purohit <siddharthbharatpurohit@gmail.com>
 */

#include "estimator_interface.h"

#include <ecl.h>
#include <mathlib/mathlib.h>

// Accumulate imu data and store to buffer at desired rate
void EstimatorInterface::setIMUData(const imuSample &imu_sample)
{
	if (!_initialised) {
		init(imu_sample.time_us);
		_initialised = true;
	}

	const float dt = math::constrain((imu_sample.time_us - _time_last_imu) / 1e6f, 1.0e-4f, 0.02f);

	_time_last_imu = imu_sample.time_us;

	if (_time_last_imu > 0) {
		_dt_imu_avg = 0.8f * _dt_imu_avg + 0.2f * dt;
	}

	// calculate a metric which indicates the amount of coning vibration
	Vector3f temp = imu_sample.delta_ang % _delta_ang_prev;
	_vibe_metrics[0] = 0.99f * _vibe_metrics[0] + 0.01f * temp.norm();

	// calculate a metric which indicates the amount of high frequency gyro vibration
	temp = imu_sample.delta_ang - _delta_ang_prev;
	_delta_ang_prev = imu_sample.delta_ang;
	_vibe_metrics[1] = 0.99f * _vibe_metrics[1] + 0.01f * temp.norm();

	// calculate a metric which indicates the amount of high frequency accelerometer vibration
	temp = imu_sample.delta_vel - _delta_vel_prev;
	_delta_vel_prev = imu_sample.delta_vel;
	_vibe_metrics[2] = 0.99f * _vibe_metrics[2] + 0.01f * temp.norm();

	// detect if the vehicle is not moving when on ground
	if (!_control_status.flags.in_air) {
		if ((_vibe_metrics[1] * 4.0E4f > _params.is_moving_scaler)
				|| (_vibe_metrics[2] * 2.1E2f > _params.is_moving_scaler)
				|| ((imu_sample.delta_ang.norm() / dt) > 0.05f * _params.is_moving_scaler)) {

			_time_last_move_detect_us = imu_sample.time_us;
		}

		_vehicle_at_rest = ((imu_sample.time_us - _time_last_move_detect_us) > (uint64_t)1E6);

	} else {
		_time_last_move_detect_us = imu_sample.time_us;
		_vehicle_at_rest = false;
	}

	// accumulate and down-sample imu data and push to the buffer when new downsampled data becomes available
	if (collect_imu(imu_sample)) {

		// down-sample the drag specific force data by accumulating and calculating the mean when
		// sufficient samples have been collected
		if ((_params.fusion_mode & MASK_USE_DRAG) && !_drag_buffer_fail) {

			// Allocate the required buffer size if not previously done
			// Do not retry if allocation has failed previously
			if (_drag_buffer.get_length() < _obs_buffer_length) {
				_drag_buffer_fail = !_drag_buffer.allocate(_obs_buffer_length);

				if (_drag_buffer_fail) {
					ECL_ERR_TIMESTAMPED("drag buffer allocation failed");
					return;
				}
			}

			_drag_sample_count ++;
			// note acceleration is accumulated as a delta velocity
			_drag_down_sampled.accelXY(0) += imu_sample.delta_vel(0);
			_drag_down_sampled.accelXY(1) += imu_sample.delta_vel(1);
			_drag_down_sampled.time_us += imu_sample.time_us;
			_drag_sample_time_dt += imu_sample.delta_vel_dt;

			// calculate the downsample ratio for drag specific force data
			uint8_t min_sample_ratio = (uint8_t) ceilf((float)_imu_buffer_length / _obs_buffer_length);

			if (min_sample_ratio < 5) {
				min_sample_ratio = 5;
			}

			// calculate and store means from accumulated values
			if (_drag_sample_count >= min_sample_ratio) {
				// note conversion from accumulated delta velocity to acceleration
				_drag_down_sampled.accelXY(0) /= _drag_sample_time_dt;
				_drag_down_sampled.accelXY(1) /= _drag_sample_time_dt;
				_drag_down_sampled.time_us /= _drag_sample_count;

				// write to buffer
				_drag_buffer.push(_drag_down_sampled);

				// reset accumulators
				_drag_sample_count = 0;
				_drag_down_sampled.accelXY.zero();
				_drag_down_sampled.time_us = 0;
				_drag_sample_time_dt = 0.0f;
			}
		}
	}
}

void EstimatorInterface::setIMUData(uint64_t time_usec, uint64_t delta_ang_dt, uint64_t delta_vel_dt,
				    float (&delta_ang)[3], float (&delta_vel)[3])
{
	imuSample imu_sample_new;
	imu_sample_new.delta_ang = Vector3f(delta_ang);
	imu_sample_new.delta_vel = Vector3f(delta_vel);

	// convert time from us to secs
	imu_sample_new.delta_ang_dt = delta_ang_dt / 1e6f;
	imu_sample_new.delta_vel_dt = delta_vel_dt / 1e6f;
	imu_sample_new.time_us = time_usec;

	setIMUData(imu_sample_new);
}

void EstimatorInterface::setMagData(uint64_t time_usec, float (&data)[3])
{
	if (!_initialised || _mag_buffer_fail) {
		return;
	}

	// Allocate the required buffer size if not previously done
	// Do not retry if allocation has failed previously
	if (_mag_buffer.get_length() < _obs_buffer_length) {
		_mag_buffer_fail = !_mag_buffer.allocate(_obs_buffer_length);

		if (_mag_buffer_fail) {
			ECL_ERR_TIMESTAMPED("mag buffer allocation failed");
			return;
		}
	}

	// limit data rate to prevent data being lost
	if ((time_usec - _time_last_mag) > _min_obs_interval_us) {

		magSample mag_sample_new;
		mag_sample_new.time_us = time_usec - _params.mag_delay_ms * 1000;

		mag_sample_new.time_us -= FILTER_UPDATE_PERIOD_MS * 1000 / 2;
		_time_last_mag = time_usec;

		mag_sample_new.mag = Vector3f(data);

		_mag_buffer.push(mag_sample_new);
	}
}

void EstimatorInterface::setGpsData(uint64_t time_usec, const gps_message &gps)
{
	if (!_initialised || _gps_buffer_fail) {
		return;
	}

	// Allocate the required buffer size if not previously done
	// Do not retry if allocation has failed previously
	if (_gps_buffer.get_length() < _obs_buffer_length) {
		_gps_buffer_fail = !_gps_buffer.allocate(_obs_buffer_length);

		if (_gps_buffer_fail) {
			ECL_ERR_TIMESTAMPED("GPS buffer allocation failed");
			return;
		}
	}

	// limit data rate to prevent data being lost
	bool need_gps = (_params.fusion_mode & MASK_USE_GPS) || (_params.vdist_sensor_type == VDIST_SENSOR_GPS);

	if (((time_usec - _time_last_gps) > _min_obs_interval_us) && need_gps && gps.fix_type > 2) {
		gpsSample gps_sample_new;
		gps_sample_new.time_us = gps.time_usec - _params.gps_delay_ms * 1000;

		gps_sample_new.time_us -= FILTER_UPDATE_PERIOD_MS * 1000 / 2;
		_time_last_gps = time_usec;

		gps_sample_new.time_us = math::max(gps_sample_new.time_us, _imu_sample_delayed.time_us);
		gps_sample_new.vel = Vector3f(gps.vel_ned);

		_gps_speed_valid = gps.vel_ned_valid;
		gps_sample_new.sacc = gps.sacc;
		gps_sample_new.hacc = gps.eph;
		gps_sample_new.vacc = gps.epv;

		gps_sample_new.hgt = (float)gps.alt * 1e-3f;

		gps_sample_new.yaw = gps.yaw;
		if (ISFINITE(gps.yaw_offset)) {
			_gps_yaw_offset = gps.yaw_offset;
		} else {
			_gps_yaw_offset = 0.0f;
		}

		// Only calculate the relative position if the WGS-84 location of the origin is set
		if (collect_gps(gps)) {
			float lpos_x = 0.0f;
			float lpos_y = 0.0f;
			map_projection_project(&_pos_ref, (gps.lat / 1.0e7), (gps.lon / 1.0e7), &lpos_x, &lpos_y);
			gps_sample_new.pos(0) = lpos_x;
			gps_sample_new.pos(1) = lpos_y;

		} else {
			gps_sample_new.pos(0) = 0.0f;
			gps_sample_new.pos(1) = 0.0f;
		}

		_gps_buffer.push(gps_sample_new);
	}
}

void EstimatorInterface::setBaroData(uint64_t time_usec, float data)
{
	if (!_initialised || _baro_buffer_fail) {
		return;
	}

	// Allocate the required buffer size if not previously done
	// Do not retry if allocation has failed previously
	if (_baro_buffer.get_length() < _obs_buffer_length) {
		_baro_buffer_fail = !_baro_buffer.allocate(_obs_buffer_length);

		if (_baro_buffer_fail) {
			ECL_ERR_TIMESTAMPED("baro buffer allocation failed");
			return;
		}
	}

	// limit data rate to prevent data being lost
	if ((time_usec - _time_last_baro) > _min_obs_interval_us) {

		baroSample baro_sample_new;
		baro_sample_new.hgt = data;
		baro_sample_new.time_us = time_usec - _params.baro_delay_ms * 1000;

		baro_sample_new.time_us -= FILTER_UPDATE_PERIOD_MS * 1000 / 2;
		_time_last_baro = time_usec;

		baro_sample_new.time_us = math::max(baro_sample_new.time_us, _imu_sample_delayed.time_us);

		_baro_buffer.push(baro_sample_new);
	}
}

void EstimatorInterface::setAirspeedData(uint64_t time_usec, float true_airspeed, float eas2tas)
{
	if (!_initialised || _airspeed_buffer_fail) {
		return;
	}

	// Allocate the required buffer size if not previously done
	// Do not retry if allocation has failed previously
	if (_airspeed_buffer.get_length() < _obs_buffer_length) {
		_airspeed_buffer_fail = !_airspeed_buffer.allocate(_obs_buffer_length);

		if (_airspeed_buffer_fail) {
			ECL_ERR_TIMESTAMPED("airspeed buffer allocation failed");
			return;
		}
	}

	// limit data rate to prevent data being lost
	if ((time_usec - _time_last_airspeed) > _min_obs_interval_us) {
		airspeedSample airspeed_sample_new;
		airspeed_sample_new.true_airspeed = true_airspeed;
		airspeed_sample_new.eas2tas = eas2tas;
		airspeed_sample_new.time_us = time_usec - _params.airspeed_delay_ms * 1000;
		airspeed_sample_new.time_us -= FILTER_UPDATE_PERIOD_MS * 1000 / 2;
		_time_last_airspeed = time_usec;

		_airspeed_buffer.push(airspeed_sample_new);
	}
}

void EstimatorInterface::setRangeData(uint64_t time_usec, float data, int8_t quality)
{
	if (!_initialised || _range_buffer_fail) {
		return;
	}

	// Allocate the required buffer size if not previously done
	// Do not retry if allocation has failed previously
	if (_range_buffer.get_length() < _obs_buffer_length) {
		_range_buffer_fail = !_range_buffer.allocate(_obs_buffer_length);

		if (_range_buffer_fail) {
			ECL_ERR_TIMESTAMPED("range finder buffer allocation failed");
			return;
		}
	}

	// limit data rate to prevent data being lost
	if ((time_usec - _time_last_range) > _min_obs_interval_us) {
		rangeSample range_sample_new;
		range_sample_new.rng = data;
		range_sample_new.time_us = time_usec - _params.range_delay_ms * 1000;
		range_sample_new.quality = quality;
		_time_last_range = time_usec;

		_range_buffer.push(range_sample_new);
	}
}

// set optical flow data
void EstimatorInterface::setOpticalFlowData(uint64_t time_usec, flow_message *flow)
{
	if (!_initialised || _flow_buffer_fail) {
		return;
	}

	// Allocate the required buffer size if not previously done
	// Do not retry if allocation has failed previously
	if (_flow_buffer.get_length() < _imu_buffer_length) {
		_flow_buffer_fail = !_flow_buffer.allocate(_imu_buffer_length);

		if (_flow_buffer_fail) {
			ECL_ERR_TIMESTAMPED("optical flow buffer allocation failed");
			return;
		}
	}

	// limit data rate to prevent data being lost
	if ((time_usec - _time_last_optflow) > _min_obs_interval_us) {
		// check if enough integration time and fail if integration time is less than 50%
		// of min arrival interval because too much data is being lost
		float delta_time = 1e-6f * (float)flow->dt;
		float delta_time_min = 5e-7f * (float)_min_obs_interval_us;
		bool delta_time_good = delta_time >= delta_time_min;

		if (!delta_time_good) {
			// protect against overflow caused by division with very small delta_time
			delta_time = delta_time_min;
		}


		// check magnitude is within sensor limits
		// use this to prevent use of a saturated flow sensor when there are other aiding sources available
		float flow_rate_magnitude;
		bool flow_magnitude_good = true;
		if (delta_time_good) {
			flow_rate_magnitude = flow->flowdata.norm() / delta_time;
			flow_magnitude_good = (flow_rate_magnitude <= _flow_max_rate);
		}

		bool relying_on_flow = !_control_status.flags.gps && !_control_status.flags.ev_pos && !_control_status.flags.ev_vel;

		// check quality metric
		bool flow_quality_good = (flow->quality >= _params.flow_qual_min);

		// Check data validity and write to buffers
		// Invalid flow data is allowed when on ground and is handled as a special case in controlOpticalFlowFusion()
		bool use_flow_data_to_navigate = delta_time_good && flow_quality_good && (flow_magnitude_good || relying_on_flow);
		if (use_flow_data_to_navigate || (!_control_status.flags.in_air && relying_on_flow)) {
			flowSample optflow_sample_new;
			// calculate the system time-stamp for the trailing edge of the flow data integration period
			optflow_sample_new.time_us = time_usec - _params.flow_delay_ms * 1000;

			// copy the quality metric returned by the PX4Flow sensor
			optflow_sample_new.quality = flow->quality;

			// copy the optical and gyro measured delta angles
			// NOTE: the EKF uses the reverse sign convention to the flow sensor. EKF assumes positive LOS rate
			// is produced by a RH rotation of the image about the sensor axis.
			optflow_sample_new.gyroXYZ = - flow->gyrodata;
			optflow_sample_new.flowRadXY = -flow->flowdata;

			// convert integration interval to seconds
			optflow_sample_new.dt = delta_time;
			_time_last_optflow = time_usec;

			_flow_buffer.push(optflow_sample_new);
		}
	}
}

// set attitude and position data derived from an external vision system
void EstimatorInterface::setExtVisionData(uint64_t time_usec, ext_vision_message *evdata)
{
	if (!_initialised || _ev_buffer_fail) {
		return;
	}

	// Allocate the required buffer size if not previously done
	// Do not retry if allocation has failed previously
	if (_ext_vision_buffer.get_length() < _obs_buffer_length) {
		_ev_buffer_fail = !_ext_vision_buffer.allocate(_obs_buffer_length);

		if (_ev_buffer_fail) {
			ECL_ERR_TIMESTAMPED("external vision buffer allocation failed");
			return;
		}
	}

	// limit data rate to prevent data being lost
	if ((time_usec - _time_last_ext_vision) > _min_obs_interval_us) {
		extVisionSample ev_sample_new;
		// calculate the system time-stamp for the mid point of the integration period
		ev_sample_new.time_us = time_usec - _params.ev_delay_ms * 1000;

		// copy required data
		ev_sample_new.angErr = evdata->angErr;
		ev_sample_new.posErr = evdata->posErr;
		ev_sample_new.velErr = evdata->velErr;
		ev_sample_new.hgtErr = evdata->hgtErr;
		ev_sample_new.quat = evdata->quat;
		ev_sample_new.pos = evdata->pos;
		ev_sample_new.vel = evdata->vel;

		// record time for comparison next measurement
		_time_last_ext_vision = time_usec;

		_ext_vision_buffer.push(ev_sample_new);

	}
}

void EstimatorInterface::setAuxVelData(uint64_t time_usec, float (&data)[2], float (&variance)[2])
{
	if (!_initialised || _auxvel_buffer_fail) {
		return;
	}

	// Allocate the required buffer size if not previously done
	// Do not retry if allocation has failed previously
	if (_auxvel_buffer.get_length() < _obs_buffer_length) {
		_auxvel_buffer_fail = !_auxvel_buffer.allocate(_obs_buffer_length);

		if (_auxvel_buffer_fail) {
			ECL_ERR_TIMESTAMPED("aux vel buffer allocation failed");
			return;
		}
	}

	// limit data rate to prevent data being lost
	if ((time_usec - _time_last_auxvel) > _min_obs_interval_us) {

		auxVelSample auxvel_sample_new;
		auxvel_sample_new.time_us = time_usec - _params.auxvel_delay_ms * 1000;

		auxvel_sample_new.time_us -= FILTER_UPDATE_PERIOD_MS * 1000 / 2;
		_time_last_auxvel = time_usec;

		auxvel_sample_new.velNE = Vector2f(data);
		auxvel_sample_new.velVarNE = Vector2f(variance);

		_auxvel_buffer.push(auxvel_sample_new);
	}
}

bool EstimatorInterface::initialise_interface(uint64_t timestamp)
{
	// find the maximum time delay the buffers are required to handle
	uint16_t max_time_delay_ms = math::max(_params.mag_delay_ms,
					 math::max(_params.range_delay_ms,
					     math::max(_params.gps_delay_ms,
						 math::max(_params.flow_delay_ms,
						     math::max(_params.ev_delay_ms,
							 math::max(_params.auxvel_delay_ms,
							     math::max(_params.min_delay_ms,
								 math::max(_params.airspeed_delay_ms, _params.baro_delay_ms))))))));

	// calculate the IMU buffer length required to accomodate the maximum delay with some allowance for jitter
	_imu_buffer_length = (max_time_delay_ms / FILTER_UPDATE_PERIOD_MS) + 1;

	// set the observation buffer length to handle the minimum time of arrival between observations in combination
	// with the worst case delay from current time to ekf fusion time
	// allow for worst case 50% extension of the ekf fusion time horizon delay due to timing jitter
	uint16_t ekf_delay_ms = max_time_delay_ms + (int)(ceilf((float)max_time_delay_ms * 0.5f));
	_obs_buffer_length = (ekf_delay_ms / _params.sensor_interval_min_ms) + 1;

	// limit to be no longer than the IMU buffer (we can't process data faster than the EKF prediction rate)
	_obs_buffer_length = math::min(_obs_buffer_length, _imu_buffer_length);

	if (!(_imu_buffer.allocate(_imu_buffer_length) &&
	      _output_buffer.allocate(_imu_buffer_length) &&
	      _output_vert_buffer.allocate(_imu_buffer_length))) {

		ECL_ERR_TIMESTAMPED("buffer allocation failed!");
		unallocate_buffers();
		return false;
	}

	_dt_imu_avg = 0.0f;

	_imu_sample_delayed.delta_ang.setZero();
	_imu_sample_delayed.delta_vel.setZero();
	_imu_sample_delayed.delta_ang_dt = 0.0f;
	_imu_sample_delayed.delta_vel_dt = 0.0f;
	_imu_sample_delayed.time_us = timestamp;

	_initialised = false;

	_time_last_imu = 0;
	_time_last_gps = 0;
	_time_last_mag = 0;
	_time_last_baro = 0;
	_time_last_range = 0;
	_time_last_airspeed = 0;
	_time_last_optflow = 0;
	_fault_status.value = 0;
	_time_last_ext_vision = 0;
	return true;
}

void EstimatorInterface::unallocate_buffers()
{
	_imu_buffer.unallocate();
	_gps_buffer.unallocate();
	_mag_buffer.unallocate();
	_baro_buffer.unallocate();
	_range_buffer.unallocate();
	_airspeed_buffer.unallocate();
	_flow_buffer.unallocate();
	_ext_vision_buffer.unallocate();
	_output_buffer.unallocate();
	_output_vert_buffer.unallocate();
	_drag_buffer.unallocate();
	_auxvel_buffer.unallocate();

}

bool EstimatorInterface::local_position_is_valid()
{
	// return true if we are not doing unconstrained free inertial navigation
	return !_deadreckon_time_exceeded;
}

void EstimatorInterface::print_status()
{
	ECL_INFO("local position valid: %s", local_position_is_valid() ? "yes" : "no");
	ECL_INFO("global position valid: %s", global_position_is_valid() ? "yes" : "no");

	ECL_INFO("imu buffer: %d (%d Bytes)", _imu_buffer.get_length(), _imu_buffer.get_total_size());
	ECL_INFO("gps buffer: %d (%d Bytes)", _gps_buffer.get_length(), _gps_buffer.get_total_size());
	ECL_INFO("mag buffer: %d (%d Bytes)", _mag_buffer.get_length(), _mag_buffer.get_total_size());
	ECL_INFO("baro buffer: %d (%d Bytes)", _baro_buffer.get_length(), _baro_buffer.get_total_size());
	ECL_INFO("range buffer: %d (%d Bytes)", _range_buffer.get_length(), _range_buffer.get_total_size());
	ECL_INFO("airspeed buffer: %d (%d Bytes)", _airspeed_buffer.get_length(), _airspeed_buffer.get_total_size());
	ECL_INFO("flow buffer: %d (%d Bytes)", _flow_buffer.get_length(), _flow_buffer.get_total_size());
	ECL_INFO("ext vision buffer: %d (%d Bytes)", _ext_vision_buffer.get_length(), _ext_vision_buffer.get_total_size());
	ECL_INFO("output buffer: %d (%d Bytes)", _output_buffer.get_length(), _output_buffer.get_total_size());
	ECL_INFO("output vert buffer: %d (%d Bytes)", _output_vert_buffer.get_length(), _output_vert_buffer.get_total_size());
	ECL_INFO("drag buffer: %d (%d Bytes)", _drag_buffer.get_length(), _drag_buffer.get_total_size());
}
