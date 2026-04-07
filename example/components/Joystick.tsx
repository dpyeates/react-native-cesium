import React, { memo, useCallback, useMemo } from 'react';
import { StyleSheet, View } from 'react-native';
import { Gesture, GestureDetector } from 'react-native-gesture-handler';
import Animated, {
  useSharedValue,
  useAnimatedStyle,
  withSpring,
} from 'react-native-reanimated';
import { scheduleOnRN } from 'react-native-worklets';

const JOYSTICK_RADIUS = 52;
const THUMB_RADIUS = 20;
/** Max angular rate (deg/s) at full stick deflection — lower = less sensitive. */
const JOYSTICK_MAX_RATE_DEG_S = 55;

export type JoystickProps = {
  onRateChange: (pitchRate: number, rollRate: number) => void;
};

export const Joystick = memo(function Joystick({ onRateChange }: JoystickProps) {
  const thumbX = useSharedValue(0);
  const thumbY = useSharedValue(0);

  const emitRates = useCallback(
    (tx: number, ty: number) => {
      const dist = Math.sqrt(tx * tx + ty * ty);
      // Only guard true zero (no translation yet) — no deadband; tiny moves give tiny rates.
      if (dist < 1e-6) {
        onRateChange(0, 0);
        return;
      }
      const clamp = Math.min(dist, JOYSTICK_RADIUS);
      const factor = clamp / JOYSTICK_RADIUS;
      const scale = factor * JOYSTICK_MAX_RATE_DEG_S;
      const pitchRate = (ty / dist) * scale;
      const rollRate = -(tx / dist) * scale;
      onRateChange(pitchRate, rollRate);
    },
    [onRateChange],
  );

  const stopRates = useCallback(() => {
    onRateChange(0, 0);
  }, [onRateChange]);

  const panGesture = useMemo(
    () =>
      Gesture.Pan()
        .minPointers(1)
        .maxPointers(1)
        .enableTrackpadTwoFingerGesture(false)
        .minDistance(0)
        .onBegin(() => {
          'worklet';
          thumbX.value = 0;
          thumbY.value = 0;
        })
        .onUpdate(e => {
          'worklet';
          const tx = e.translationX;
          const ty = e.translationY;
          const dist = Math.sqrt(tx * tx + ty * ty);
          const clamp = Math.min(dist, JOYSTICK_RADIUS);
          if (dist > 0) {
            thumbX.value = (tx / dist) * clamp;
            thumbY.value = (ty / dist) * clamp;
          } else {
            thumbX.value = 0;
            thumbY.value = 0;
          }
          scheduleOnRN(emitRates, tx, ty);
        })
        .onFinalize(() => {
          'worklet';
          thumbX.value = withSpring(0);
          thumbY.value = withSpring(0);
          scheduleOnRN(stopRates);
        }),
    [emitRates, stopRates, thumbX, thumbY],
  );

  const thumbStyle = useAnimatedStyle(() => ({
    transform: [{ translateX: thumbX.value }, { translateY: thumbY.value }],
  }));

  const size = JOYSTICK_RADIUS * 2;
  const center = JOYSTICK_RADIUS - THUMB_RADIUS;

  return (
    <GestureDetector gesture={panGesture}>
      <View
        style={[
          styles.base,
          { width: size, height: size, borderRadius: JOYSTICK_RADIUS },
        ]}
      >
        <View style={styles.crossH} />
        <View style={styles.crossV} />
        <Animated.View
          style={[
            styles.thumb,
            {
              width: THUMB_RADIUS * 2,
              height: THUMB_RADIUS * 2,
              borderRadius: THUMB_RADIUS,
              top: center,
              left: center,
            },
            thumbStyle,
          ]}
        />
      </View>
    </GestureDetector>
  );
});

const styles = StyleSheet.create({
  base: {
    backgroundColor: 'rgba(0,0,0,0.35)',
    borderWidth: 1.5,
    borderColor: 'rgba(255,255,255,0.35)',
    justifyContent: 'center',
    alignItems: 'center',
    overflow: 'visible',
  },
  crossH: {
    position: 'absolute',
    width: '80%',
    height: 1,
    backgroundColor: 'rgba(255,255,255,0.2)',
  },
  crossV: {
    position: 'absolute',
    width: 1,
    height: '80%',
    backgroundColor: 'rgba(255,255,255,0.2)',
  },
  thumb: {
    position: 'absolute',
    backgroundColor: 'rgba(255,255,255,0.85)',
    shadowColor: '#000',
    shadowOffset: { width: 0, height: 1 },
    shadowOpacity: 0.4,
    shadowRadius: 2,
    elevation: 3,
  },
});
