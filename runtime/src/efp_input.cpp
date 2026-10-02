// Throw measurement: do the Steam Frame's reported controller velocities match how the
// controller actually moves? (echoframe.ini VelocityLog = 1; it can be switched on while Echo runs.)
//
// Echo throws the disc with the hand's linear and angular velocity at the moment the grip is
// let go, as LibOVR reports them (OpenXR XrSpaceVelocity here). If the runtime's velocity is
// smoothed or late compared with the tracked positions, throws come out slow or late.
//
// Per hand, each time Echo asks for the tracking state (at most every 4 ms), the controller is
// also located at the current time, with its velocity: the runtime's latest estimate, not a
// prediction. A throw starts when that reported speed passes 2.5 m/s and ends below 1 m/s (or
// after 0.8 s). Its samples, from 150 ms before the start, are then compared:
//   - velocity from positions (central differences) against the reported velocity: their peaks,
//     the time shift that lines them up best, and the scale between them at that shift;
//   - the same for angular velocity, from orientations;
//   - what Echo itself was given (velocities at its own query time, which is predicted ahead);
//   - when the grip was let go, relative to the peak, and the speed at that moment.
// One runtime.log line per throw.
#include "efp.h"

#include <windows.h>
#include <math.h>
#include <algorithm>
#include <deque>
#include <mutex>
#include <vector>

XrTime AbsTimeToXrTime(XrInstance instance, double absTime);   // ReviveXR's Common.cpp

namespace {

struct V3
{
	float x, y, z;
	V3 operator-(const V3& o) const { return { x - o.x, y - o.y, z - o.z }; }
	V3 operator+(const V3& o) const { return { x + o.x, y + o.y, z + o.z }; }
	V3 operator*(float s) const { return { x * s, y * s, z * s }; }
	float Dot(const V3& o) const { return x * o.x + y * o.y + z * o.z; }
	float Len() const { return sqrtf(Dot(*this)); }
};
V3 ToV3(const XrVector3f& v) { return { v.x, v.y, v.z }; }

struct Sample
{
	double T;          // seconds (QueryPerformanceCounter)
	V3 P, V, W;        // position, reported linear and angular velocity
	XrQuaternionf Q;
};

struct EchoSample { double Called, For; V3 V, W; };   // what Echo was given, when, for which time

struct Hand
{
	std::deque<Sample> Samples;      // the last 1.5 s
	std::deque<EchoSample> Echo;
	bool InThrow = false;
	double ThrowStart = 0, ThrowEnd = 0;   // ThrowEnd: analysed 0.3 s later, once the grip had time to open
	float Grip = 0;
	double Released = 0;             // last time the grip went below 0.4 from above 0.6
	bool Held = false;
};

std::mutex g_lock;
Hand g_hands[2];
double g_qpf;

double NowSeconds()
{
	LARGE_INTEGER t;
	QueryPerformanceCounter(&t);
	if (!g_qpf) {
		LARGE_INTEGER f;
		QueryPerformanceFrequency(&f);
		g_qpf = double(f.QuadPart);
	}
	return double(t.QuadPart) / g_qpf;
}

// Angular velocity (world frame, rad/s) taking orientation a to b over dt
V3 AngularFromQuats(const XrQuaternionf& a, const XrQuaternionf& b, double dt)
{
	// d = b * conj(a)
	float ax = -a.x, ay = -a.y, az = -a.z, aw = a.w;
	float dw = b.w * aw - b.x * ax - b.y * ay - b.z * az;
	float dx = b.w * ax + b.x * aw + b.y * az - b.z * ay;
	float dy = b.w * ay - b.x * az + b.y * aw + b.z * ax;
	float dz = b.w * az + b.x * ay - b.y * ax + b.z * aw;
	if (dw < 0) { dw = -dw; dx = -dx; dy = -dy; dz = -dz; }
	float s = sqrtf(dx * dx + dy * dy + dz * dz);
	if (s < 1e-7f || dt <= 0) return { 0, 0, 0 };
	float angle = 2.0f * atan2f(s, dw);
	return V3{ dx, dy, dz } * float(angle / s / dt);
}

// Linear interpolation of a series (times ascending) at time t; false outside it
bool At(const std::vector<double>& ts, const std::vector<V3>& vs, double t, V3& out)
{
	if (ts.empty() || t < ts.front() || t > ts.back()) return false;
	size_t i = std::upper_bound(ts.begin(), ts.end(), t) - ts.begin();
	if (i == 0) { out = vs[0]; return true; }
	if (i >= ts.size()) { out = vs.back(); return true; }
	double f = (t - ts[i - 1]) / std::max(1e-9, ts[i] - ts[i - 1]);
	out = vs[i - 1] + (vs[i] - vs[i - 1]) * float(f);
	return true;
}

// The shift (ms) of the reported series that best matches the one from positions, and the scale
// (from positions = scale x reported) there. Positive shift: the reported velocity lags.
void Align(const std::vector<double>& ts, const std::vector<V3>& truth, const std::vector<V3>& reported,
           double& bestShiftMs, float& bestScale)
{
	double best = 1e30;
	bestShiftMs = 0;
	bestScale = 1;
	for (int shift = -60; shift <= 60; shift += 2) {
		double vv = 0, tv = 0, tt = 0;
		int n = 0;
		for (size_t i = 0; i < ts.size(); i++) {
			V3 r;
			if (!At(ts, reported, ts[i] + shift / 1000.0, r)) continue;
			vv += r.Dot(r);
			tv += truth[i].Dot(r);
			tt += truth[i].Dot(truth[i]);
			n++;
		}
		if (n < 5 || vv <= 0) continue;
		double scale = tv / vv;
		double residual = (tt - 2 * scale * tv + scale * scale * vv) / std::max(1e-9, tt);
		if (residual < best) {
			best = residual;
			bestShiftMs = shift;
			bestScale = float(scale);
		}
	}
}

void Analyze(int handIndex, Hand& h, double start, double end)
{
	std::vector<double> ts;
	std::vector<V3> fdV, repV, fdW, repW;
	std::vector<Sample> s;
	for (const Sample& x : h.Samples)
		if (x.T >= start - 0.15 && x.T <= end + 0.02) s.push_back(x);
	if (s.size() < 8) return;
	for (size_t i = 1; i + 1 < s.size(); i++) {
		double dt = s[i + 1].T - s[i - 1].T;
		if (dt < 0.004) continue;
		ts.push_back(s[i].T);
		fdV.push_back((s[i + 1].P - s[i - 1].P) * float(1.0 / dt));
		fdW.push_back(AngularFromQuats(s[i - 1].Q, s[i + 1].Q, dt));
		repV.push_back(s[i].V);
		repW.push_back(s[i].W);
	}
	if (ts.size() < 6) return;

	auto peak = [&](const std::vector<V3>& v, double& when) {
		float m = 0;
		for (size_t i = 0; i < v.size(); i++)
			if (v[i].Len() > m) { m = v[i].Len(); when = ts[i]; }
		return m;
	};
	double tFd = 0, tRep = 0, tFdW = 0, tRepW = 0;
	float fdPeak = peak(fdV, tFd), repPeak = peak(repV, tRep);
	float fdPeakW = peak(fdW, tFdW), repPeakW = peak(repW, tRepW);
	double shift, shiftW;
	float scale, scaleW;
	Align(ts, fdV, repV, shift, scale);
	Align(ts, fdW, repW, shiftW, scaleW);

	// What Echo was given: the fastest it saw during the throw, and how far ahead it asks
	float echoPeak = 0, echoPeakW = 0;
	double lead = 0;
	int leads = 0;
	for (const EchoSample& e : h.Echo) {
		if (e.Called < start - 0.15 || e.Called > end + 0.02) continue;
		echoPeak = std::max(echoPeak, e.V.Len());
		echoPeakW = std::max(echoPeakW, e.W.Len());
		lead += e.For - e.Called;
		leads++;
	}

	char release[200] = "grip not let go during it";
	if (h.Released >= start - 0.15 && h.Released <= end + 0.3) {
		V3 atRelease = {};
		float fdAt = At(ts, fdV, h.Released, atRelease) ? atRelease.Len() : -1;
		float echoAt = -1;
		double nearest = 1;
		for (const EchoSample& e : h.Echo)
			if (fabs(e.Called - h.Released) < nearest) { nearest = fabs(e.Called - h.Released); echoAt = e.V.Len(); }
		snprintf(release, sizeof(release), "grip let go %+.0f ms from the peak, at %.1f m/s from positions, Echo was given %.1f",
		         (h.Released - tFd) * 1000.0, fdAt, echoAt);
	}
	EFP_Log("throw %s: %.1f m/s from positions, %.1f reported (Echo given %.1f, %.0f ms ahead); reported lags by %+.0f ms, "
	        "positions = %.2f x reported | spin %.1f rad/s from orientations, %.1f reported (Echo %.1f), lags %+.0f ms, x%.2f | %s",
	        handIndex ? "R" : "L", fdPeak, repPeak, echoPeak, leads ? lead / leads * 1000.0 : 0.0, shift, scale,
	        fdPeakW, repPeakW, echoPeakW, shiftW, scaleW, release);
}

} // namespace

void EFP_InputTrack(XrInstance instance, XrSpace handSpace, XrSpace base, int hand, double calledAt, double forTime,
                    const XrSpaceVelocity& given)
{
	if (!EFP_VelocityLog() || hand < 0 || hand > 1 || !handSpace) return;
	std::lock_guard<std::mutex> lk(g_lock);
	Hand& h = g_hands[hand];
	double now = NowSeconds();
	EchoSample e = { calledAt, forTime, ToV3(given.linearVelocity), ToV3(given.angularVelocity) };
	if (h.Echo.empty() || h.Echo.back().For != forTime) h.Echo.push_back(e);
	while (!h.Echo.empty() && h.Echo.front().Called < now - 1.5) h.Echo.pop_front();
	if (!h.Samples.empty() && now - h.Samples.back().T < 0.004) return;

	XrSpaceVelocity velocity = { XR_TYPE_SPACE_VELOCITY };
	XrSpaceLocation location = { XR_TYPE_SPACE_LOCATION, &velocity };
	if (XR_FAILED(xrLocateSpace(handSpace, base, AbsTimeToXrTime(instance, now), &location))) return;
	const XrSpaceLocationFlags need = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
	if ((location.locationFlags & need) != need || !(velocity.velocityFlags & XR_SPACE_VELOCITY_LINEAR_VALID_BIT)) return;

	Sample s = { now, ToV3(location.pose.position), ToV3(velocity.linearVelocity),
	             (velocity.velocityFlags & XR_SPACE_VELOCITY_ANGULAR_VALID_BIT) ? ToV3(velocity.angularVelocity) : V3{},
	             location.pose.orientation };
	h.Samples.push_back(s);
	while (!h.Samples.empty() && h.Samples.front().T < now - 1.5) h.Samples.pop_front();

	float speed = s.V.Len();
	if (h.ThrowEnd && now > h.ThrowEnd + 0.3) {
		Analyze(hand, h, h.ThrowStart, h.ThrowEnd);
		h.ThrowEnd = 0;
	}
	if (!h.InThrow && !h.ThrowEnd && speed > 2.5f) {
		h.InThrow = true;
		h.ThrowStart = now;
	}
	else if (h.InThrow && (speed < 1.0f || now - h.ThrowStart > 0.8)) {
		h.InThrow = false;
		h.ThrowEnd = now;
	}
}

void EFP_InputGrip(int hand, float value)
{
	if (!EFP_VelocityLog() || hand < 0 || hand > 1) return;
	std::lock_guard<std::mutex> lk(g_lock);
	Hand& h = g_hands[hand];
	if (value > 0.6f) h.Held = true;
	else if (h.Held && value < 0.4f) {
		h.Held = false;
		h.Released = NowSeconds();
	}
	h.Grip = value;
}
