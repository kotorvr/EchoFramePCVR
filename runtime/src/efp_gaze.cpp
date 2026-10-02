// Eye gaze for foveated rendering (XR_EXT_eye_gaze_interaction; echoframe.ini GazeFoveation = 1).
//
// The Steam Frame tracks the eyes, and SteamVR offers the gaze to OpenXR apps as a pose action
// (/user/eyes_ext/input/gaze_ext/pose); wineopenxr passes it through. Our own action set holds
// one pose action bound to it, attached and synced next to Revive's. Each frame the gaze is
// located in view space at the frame's predicted display time, and its direction, as tangents,
// goes to efp_foveation.cpp, which centres the full-rate region on it. Without a valid gaze
// (eyes closed, tracking lost, extension missing) foveation stays fixed on the lens centre.
#include "efp.h"

#include <windows.h>
#include <math.h>
#include <string.h>

namespace {

XrActionSet g_set = XR_NULL_HANDLE;
XrAction g_action = XR_NULL_HANDLE;
XrSpace g_space = XR_NULL_HANDLE;
XrPath g_eyes;
unsigned g_valid, g_total;
ULONGLONG g_lastLog, g_start;

} // namespace

bool EFP_GazeInit(XrInstance instance)
{
	if (!EFP_GazeFoveation() || EFP_Foveation() == 0) return false;
	XrActionSetCreateInfo si = { XR_TYPE_ACTION_SET_CREATE_INFO };
	strcpy_s(si.actionSetName, "efp_gaze");
	strcpy_s(si.localizedActionSetName, "Eye gaze (foveated rendering)");
	si.priority = 0;
	XrActionCreateInfo ai = { XR_TYPE_ACTION_CREATE_INFO };
	ai.actionType = XR_ACTION_TYPE_POSE_INPUT;
	strcpy_s(ai.actionName, "gaze");
	strcpy_s(ai.localizedActionName, "Eye gaze");
	XrPath profile = XR_NULL_PATH, pose = XR_NULL_PATH;
	if (XR_FAILED(xrCreateActionSet(instance, &si, &g_set)) || XR_FAILED(xrCreateAction(g_set, &ai, &g_action))
	    || XR_FAILED(xrStringToPath(instance, "/interaction_profiles/ext/eye_gaze_interaction", &profile))
	    || XR_FAILED(xrStringToPath(instance, "/user/eyes_ext/input/gaze_ext/pose", &pose))) {
		EFP_Log("gaze: couldn't create the eye gaze action, foveation stays fixed");
		g_set = XR_NULL_HANDLE;
		return false;
	}
	XrActionSuggestedBinding binding = { g_action, pose };
	XrInteractionProfileSuggestedBinding suggested = { XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
	suggested.interactionProfile = profile;
	suggested.countSuggestedBindings = 1;
	suggested.suggestedBindings = &binding;
	XrResult rs = xrSuggestInteractionProfileBindings(instance, &suggested);
	if (XR_FAILED(rs)) {
		EFP_Log("gaze: eye gaze binding rejected (%d), foveation stays fixed", (int)rs);
		xrDestroyActionSet(g_set);
		g_set = XR_NULL_HANDLE;
		return false;
	}
	xrStringToPath(instance, "/user/eyes_ext", &g_eyes);
	EFP_Log("gaze: eye gaze action ready");
	return true;
}

XrActionSet EFP_GazeActionSet() { return g_set; }

void EFP_GazeAttach(XrSession session)
{
	if (g_space) { xrDestroySpace(g_space); g_space = XR_NULL_HANDLE; }
	if (!session || !g_set) return;
	XrActionSpaceCreateInfo ci = { XR_TYPE_ACTION_SPACE_CREATE_INFO };
	ci.action = g_action;
	ci.poseInActionSpace.orientation.w = 1.0f;
	if (XR_FAILED(xrCreateActionSpace(session, &ci, &g_space)))
		EFP_Log("gaze: couldn't create the gaze space");
}

void EFP_GazeUpdate(XrSession session, XrSpace viewSpace, XrTime time)
{
	if (!g_space) return;
	bool valid = false;
	float tx = 0, ty = 0;
	XrActionStatePose state = { XR_TYPE_ACTION_STATE_POSE };
	XrActionStateGetInfo gi = { XR_TYPE_ACTION_STATE_GET_INFO };
	gi.action = g_action;
	if (XR_SUCCEEDED(xrGetActionStatePose(session, &gi, &state)) && state.isActive) {
		XrSpaceLocation loc = { XR_TYPE_SPACE_LOCATION };
		if (XR_SUCCEEDED(xrLocateSpace(g_space, viewSpace, time, &loc))
		    && (loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)
		    && (loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT)) {
			// the gaze looks down the pose's -Z axis: rotate (0, 0, -1) by its orientation
			const XrQuaternionf& q = loc.pose.orientation;
			float dx = -2.0f * (q.x * q.z + q.w * q.y);
			float dy = -2.0f * (q.y * q.z - q.w * q.x);
			float dz = -(1.0f - 2.0f * (q.x * q.x + q.y * q.y));
			if (dz < -0.2f) {
				tx = dx / -dz;
				ty = dy / -dz;
				valid = fabsf(tx) < 1.5f && fabsf(ty) < 1.5f;
			}
		}
	}
	EFP_FoveationGaze(valid, tx, ty);
	g_total++;
	if (valid) g_valid++;
	ULONGLONG now = GetTickCount64();
	if (!g_start) g_start = g_lastLog = now;
	if (now - g_lastLog >= 30000 && now - g_start < 10 * 60 * 1000) {
		EFP_Log("gaze: valid in %.0f%% of the last %u frames (now %s, tangents %.2f %.2f)", g_total ? 100.0 * g_valid / g_total : 0.0,
		        g_total, valid ? "valid" : "invalid", tx, ty);
		g_lastLog = now;
		g_valid = g_total = 0;
	}
}
