import React, { useMemo, type ReactNode } from 'react';
import { Gesture, GestureDetector } from 'react-native-gesture-handler';
import {
  type SharedValue,
  withDecay,
} from 'react-native-reanimated';

// Degrees per pixel per metre of altitude — matches native GestureHandler scale.
const PAN_SCALE = 1.55e-8;
const MIN_ALT_ABOVE_TERRAIN = 3;
const MIN_ALT_ABSOLUTE = 3;
const MAX_ALT = 100_000_000;

export type MapGestureHandlerProps = {
  children: ReactNode;
  camLat: SharedValue<number>;
  camLon: SharedValue<number>;
  camAlt: SharedValue<number>;
  camHdg: SharedValue<number>;
  snapLat: SharedValue<number>;
  snapLon: SharedValue<number>;
  snapAlt: SharedValue<number>;
  snapHdg: SharedValue<number>;
  terrainHeight: SharedValue<number>;
};

export function MapGestureHandler({
  children,
  camLat,
  camLon,
  camAlt,
  camHdg,
  snapLat,
  snapLon,
  snapAlt,
  snapHdg,
  terrainHeight,
}: MapGestureHandlerProps) {
  const panGesture = useMemo(
    () =>
      Gesture.Pan()
        .minPointers(1)
        .maxPointers(1)
        .onBegin(() => {
          'worklet';
          snapLat.value = camLat.value;
          snapLon.value = camLon.value;
        })
        .onUpdate(e => {
          'worklet';
          const sens = camAlt.value * PAN_SCALE;
          const H = (camHdg.value * Math.PI) / 180;
          const cosH = Math.cos(H);
          const sinH = Math.sin(H);
          const latRad = (snapLat.value * Math.PI) / 180;
          const fwd = e.translationY * sens;
          const rgt = -e.translationX * sens;
          camLat.value = Math.max(
            -90,
            Math.min(90, snapLat.value + cosH * fwd - sinH * rgt),
          );
          camLon.value =
            snapLon.value +
            (sinH * fwd + cosH * rgt) / Math.max(Math.cos(latRad), 0.01);
        })
        .onEnd(e => {
          'worklet';
          const sens = camAlt.value * PAN_SCALE;
          const H = (camHdg.value * Math.PI) / 180;
          const cosH = Math.cos(H);
          const sinH = Math.sin(H);
          const latRad = (camLat.value * Math.PI) / 180;
          const latVel = sens * (cosH * e.velocityY + sinH * e.velocityX);
          const lonVel =
            (sens * (sinH * e.velocityY - cosH * e.velocityX)) /
            Math.max(Math.cos(latRad), 0.01);
          camLat.value = withDecay({
            velocity: latVel,
            clamp: [-90, 90],
            deceleration: 0.99,
          });
          camLon.value = withDecay({ velocity: lonVel, deceleration: 0.99 });
        }),
    [
      camLat,
      camLon,
      camAlt,
      camHdg,
      snapLat,
      snapLon,
    ],
  );

  const pinchGesture = useMemo(
    () =>
      Gesture.Pinch()
        .onBegin(() => {
          'worklet';
          const minAlt = Math.max(
            MIN_ALT_ABSOLUTE,
            terrainHeight.value + MIN_ALT_ABOVE_TERRAIN,
          );
          snapAlt.value = Math.max(camAlt.value, minAlt);
        })
        .onUpdate(e => {
          'worklet';
          const newAlt = snapAlt.value / Math.max(e.scale, 1e-6);
          const minAlt = Math.max(
            MIN_ALT_ABSOLUTE,
            terrainHeight.value + MIN_ALT_ABOVE_TERRAIN,
          );
          camAlt.value = Math.max(minAlt, Math.min(MAX_ALT, newAlt));
        }),
    [camAlt, snapAlt, terrainHeight],
  );

  const rotationGesture = useMemo(
    () =>
      Gesture.Rotation()
        .onBegin(() => {
          'worklet';
          snapHdg.value = camHdg.value;
        })
        .onUpdate(e => {
          'worklet';
          camHdg.value =
            (((snapHdg.value + (e.rotation * 180) / Math.PI) % 360) + 360) %
            360;
        }),
    [camHdg, snapHdg],
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
