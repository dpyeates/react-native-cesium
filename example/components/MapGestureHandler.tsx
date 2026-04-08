import React, { useMemo, type ReactNode } from 'react';
import { Gesture, GestureDetector } from 'react-native-gesture-handler';
import {
  type SharedValue,
  useAnimatedReaction,
  useSharedValue,
  withDecay,
} from 'react-native-reanimated';
import type { CameraState } from 'react-native-cesium';

// Degrees per pixel per metre of altitude — matches native GestureHandler scale.
const PAN_SCALE = 1.55e-8;
const MIN_ALT_ABSOLUTE = 3;
const MAX_ALT = 100_000_000;

export type MapGestureHandlerProps = {
  children: ReactNode;
  camera: SharedValue<CameraState>;
  initialCamera: CameraState;
};

/** Per-gesture anchor values; each gesture seeds this in `onBegin` from `camera`. */
export function MapGestureHandler({
  children,
  camera,
  initialCamera,
}: MapGestureHandlerProps) {
  const snap = useSharedValue({
    lat: 0,
    lon: 0,
    alt: 0,
    hdg: 0,
  });

  const panDecayLat = useSharedValue(initialCamera.latitude);
  const panDecayLon = useSharedValue(initialCamera.longitude);
  useAnimatedReaction(
    () => ({ lat: panDecayLat.value, lon: panDecayLon.value }),
    cur => {
      'worklet';
      const prev = camera.value;
      camera.value = { ...prev, latitude: cur.lat, longitude: cur.lon };
    },
    [camera],
  );

  const panGesture = useMemo(
    () =>
      Gesture.Pan()
        .minPointers(1)
        .maxPointers(1)
        .onBegin(() => {
          'worklet';
          const prev = camera.value;
          snap.value = { ...snap.value, lat: prev.latitude, lon: prev.longitude };
        })
        .onUpdate(e => {
          'worklet';
          const cur = camera.value;
          const sens = cur.altitude * PAN_SCALE;
          const H = (cur.heading * Math.PI) / 180;
          const cosH = Math.cos(H);
          const sinH = Math.sin(H);
          const latRad = (snap.value.lat * Math.PI) / 180;
          const fwd = e.translationY * sens;
          const rgt = -e.translationX * sens;
          const lat = Math.max(
            -90,
            Math.min(90, snap.value.lat + cosH * fwd - sinH * rgt),
          );
          const lon =
            snap.value.lon +
            (sinH * fwd + cosH * rgt) / Math.max(Math.cos(latRad), 0.01);
          camera.value = { ...cur, latitude: lat, longitude: lon };
        })
        .onEnd(e => {
          'worklet';
          const cur = camera.value;
          const sens = cur.altitude * PAN_SCALE;
          const H = (cur.heading * Math.PI) / 180;
          const cosH = Math.cos(H);
          const sinH = Math.sin(H);
          const latRad = (cur.latitude * Math.PI) / 180;
          const latVel = sens * (cosH * e.velocityY + sinH * e.velocityX);
          const lonVel =
            (sens * (sinH * e.velocityY - cosH * e.velocityX)) /
            Math.max(Math.cos(latRad), 0.01);
          panDecayLat.value = cur.latitude;
          panDecayLon.value = cur.longitude;
          panDecayLat.value = withDecay({
            velocity: latVel,
            clamp: [-90, 90],
            deceleration: 0.99,
          });
          panDecayLon.value = withDecay({ velocity: lonVel, deceleration: 0.99 });
        }),
    [
      camera,
      panDecayLat,
      panDecayLon,
      snap,
    ],
  );

  const pinchGesture = useMemo(
    () =>
      Gesture.Pinch()
        .onBegin(() => {
          'worklet';
          const cur = camera.value;
          snap.value = { ...snap.value, alt: Math.max(cur.altitude, MIN_ALT_ABSOLUTE) };
        })
        .onUpdate(e => {
          'worklet';
          const cur = camera.value;
          const newAlt = snap.value.alt / Math.max(e.scale, 1e-6);
          camera.value = {
            ...cur,
            altitude: Math.max(MIN_ALT_ABSOLUTE, Math.min(MAX_ALT, newAlt)),
          };
        }),
    [camera, snap],
  );

  const rotationGesture = useMemo(
    () =>
      Gesture.Rotation()
        .onBegin(() => {
          'worklet';
          const cur = camera.value;
          snap.value = { ...snap.value, hdg: cur.heading };
        })
        .onUpdate(e => {
          'worklet';
          const cur = camera.value;
          const hdg = (((snap.value.hdg + (e.rotation * 180) / Math.PI) % 360) + 360) % 360;
          camera.value = { ...cur, heading: hdg };
        }),
    [camera, snap],
  );

  const pinchRotateGesture = useMemo(
    () => Gesture.Simultaneous(pinchGesture, rotationGesture),
    [pinchGesture, rotationGesture],
  );

  const mapGesture = useMemo(
    () => Gesture.Simultaneous(panGesture, pinchRotateGesture),
    [panGesture, pinchRotateGesture],
  );

  return <GestureDetector gesture={mapGesture}>{children}</GestureDetector>;
}
