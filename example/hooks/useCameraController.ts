import { useCallback, useEffect, useState } from 'react';
import type { FrameInfo } from 'react-native-reanimated';
import {
  useAnimatedReaction,
  useFrameCallback,
  useSharedValue,
} from 'react-native-reanimated';
import { scheduleOnRN } from 'react-native-worklets';
import type { CameraState, CesiumViewMethods } from 'react-native-cesium';

const HUD_UPDATE_INTERVAL_MS = 100;

export type CameraControllerResult = {
  /** Shared camera value driven by gestures, joystick, and the joystick frame loop. */
  camera: ReturnType<typeof useSharedValue<CameraState>>;
  /** Throttled React state copy of the camera for HUD rendering. */
  hudCamera: CameraState;
  /** Stable setter — pass to the CesiumView hybridRef callback. */
  setCesiumView: (view: CesiumViewMethods | null) => void;
  /** Wire to Joystick's onRateChange prop. */
  handleJoystickRates: (pitchRate: number, rollRate: number) => void;
};

/**
 * Manages the shared camera value, joystick pitch/roll integration, throttled
 * HUD updates, and the native per-DoF camera setters.
 *
 * The native side runs an α-β tracker per DoF, so each setter only needs the
 * value that actually changed. We split the writes per DoF here so a high-rate
 * IMU feed (50 Hz attitude) does not drag stale GPS lat/lon through every
 * call.
 *
 * @param initialCamera - Seed value for the camera shared value. Reactive
 * changes to this argument after mount are ignored.
 */
export function useCameraController(
  initialCamera: CameraState,
): CameraControllerResult {
  const camera = useSharedValue<CameraState>(initialCamera);
  const lastHudDispatchMs = useSharedValue(0);
  const joystickPitchRate = useSharedValue(0);
  const joystickRollRate = useSharedValue(0);

  const [cesiumView, setCesiumView] = useState<CesiumViewMethods | null>(null);
  const [hudCamera, setHudCamera] = useState<CameraState>(initialCamera);

  // Previous values pushed to native — kept on the UI thread (shared values)
  // so the per-DoF diffing logic stays inside the worklet reaction below.
  // -1 / NaN sentinels mark "not seeded yet" so the first reaction issues a
  // single teleport, after which we diff per DoF.
  const prevSeeded = useSharedValue(false);
  const prevLat   = useSharedValue(0);
  const prevLon   = useSharedValue(0);
  const prevAlt   = useSharedValue(0);
  const prevHdg   = useSharedValue(0);
  const prevPitch = useSharedValue(0);
  const prevRoll  = useSharedValue(0);
  const prevVfov  = useSharedValue(0);

  const updateHudCamera = useCallback((nextCamera: CameraState) => {
    setHudCamera(nextCamera);
  }, []);

  // Integrate joystick pitch/roll rates into the camera shared value each frame.
  const integrateJoystick = useCallback(
    (frameInfo: FrameInfo) => {
      'worklet';
      const dt =
        frameInfo.timeSincePreviousFrame != null
          ? frameInfo.timeSincePreviousFrame / 1000
          : 0;
      if (dt <= 0 || dt > 0.25) return;
      const pr = joystickPitchRate.value;
      const rr = joystickRollRate.value;
      if (pr === 0 && rr === 0) return;
      const cur = camera.value;
      let p = cur.pitch + pr * dt;
      let r = cur.roll + rr * dt;
      while (p > 180) p -= 360;
      while (p < -180) p += 360;
      while (r > 180) r -= 360;
      while (r < -180) r += 360;
      camera.value = { ...cur, pitch: p, roll: r };
    },
    [joystickPitchRate, joystickRollRate, camera],
  );

  useFrameCallback(integrateJoystick);

  // Push camera state to native via per-DoF setters; throttle HUD React
  // state updates so the JS thread isn't flooded.
  useAnimatedReaction(
    () => camera.value,
    (cur) => {
      'worklet';
      if (cesiumView == null) return;
      if (!prevSeeded.value) {
        cesiumView.teleport(cur);
        prevSeeded.value = true;
      } else {
        if (cur.latitude !== prevLat.value || cur.longitude !== prevLon.value) {
          cesiumView.setPosition(cur.latitude, cur.longitude);
        }
        if (cur.altitude !== prevAlt.value) cesiumView.setAltitude(cur.altitude);
        if (cur.heading !== prevHdg.value)  cesiumView.setHeading(cur.heading);
        if (cur.pitch !== prevPitch.value || cur.roll !== prevRoll.value) {
          cesiumView.setAttitude(cur.pitch, cur.roll);
        }
        if (cur.verticalFovDeg !== prevVfov.value) {
          cesiumView.setVerticalFov(cur.verticalFovDeg);
        }
      }
      prevLat.value   = cur.latitude;
      prevLon.value   = cur.longitude;
      prevAlt.value   = cur.altitude;
      prevHdg.value   = cur.heading;
      prevPitch.value = cur.pitch;
      prevRoll.value  = cur.roll;
      prevVfov.value  = cur.verticalFovDeg;

      const now = Date.now();
      if (now - lastHudDispatchMs.value < HUD_UPDATE_INTERVAL_MS) return;
      lastHudDispatchMs.value = now;
      scheduleOnRN(updateHudCamera, cur);
    },
    [
      cesiumView,
      lastHudDispatchMs,
      updateHudCamera,
      prevSeeded,
      prevLat,
      prevLon,
      prevAlt,
      prevHdg,
      prevPitch,
      prevRoll,
      prevVfov,
    ],
  );

  // The native integrator writes the clamped altitude back into its own
  // *actual* state via the terrain-floor hook, but the shared `camera` value
  // (and therefore the gesture pan/zoom anchors) is a JS-side construct. We
  // need a single nudge after mount so the anchors track reality. After that,
  // any subsequent pinch will overwrite altitude from the latest demand
  // anyway, so we don't need to poll continuously.
  useEffect(() => {
    if (!cesiumView) return;
    let cancelled = false;
    const id = setTimeout(async () => {
      if (cancelled || !cesiumView) return;
      try {
        const state = await cesiumView.getActualCamera();
        if (cancelled) return;
        if (state.altitude > camera.value.altitude + 0.5) {
          camera.value = { ...camera.value, altitude: state.altitude };
        }
      } catch (_) {
        // bridge not ready yet
      }
    }, 750);
    return () => {
      cancelled = true;
      clearTimeout(id);
    };
  }, [cesiumView, camera]);

  const handleJoystickRates = useCallback(
    (pitchRate: number, rollRate: number) => {
      joystickPitchRate.value = pitchRate;
      joystickRollRate.value = rollRate;
    },
    [joystickPitchRate, joystickRollRate],
  );

  return { camera, hudCamera, setCesiumView, handleJoystickRates };
}
