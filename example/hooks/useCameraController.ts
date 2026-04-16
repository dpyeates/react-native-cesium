import { useCallback, useState } from 'react';
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
  /** Shared camera value driven by gestures, joystick, and native sync. */
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
 * HUD updates, and native setCamera calls.
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

  // Push camera state to native on every change; throttle HUD React state
  // updates to avoid flooding the JS thread.
  useAnimatedReaction(
    () => camera.value,
    (cur) => {
      cesiumView?.setCamera(cur);
      const now = Date.now();
      if (now - lastHudDispatchMs.value < HUD_UPDATE_INTERVAL_MS) return;
      lastHudDispatchMs.value = now;
      scheduleOnRN(updateHudCamera, cur);
    },
    [cesiumView, lastHudDispatchMs, updateHudCamera],
  );

  const handleJoystickRates = useCallback(
    (pitchRate: number, rollRate: number) => {
      joystickPitchRate.value = pitchRate;
      joystickRollRate.value = rollRate;
    },
    [joystickPitchRate, joystickRollRate],
  );

  return { camera, hudCamera, setCesiumView, handleJoystickRates };
}
