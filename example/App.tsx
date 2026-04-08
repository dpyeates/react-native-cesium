import React, { useCallback, useRef, useState } from 'react';
import {
  StatusBar,
  StyleSheet,
  Text,
  TouchableOpacity,
  View,
} from 'react-native';
import { GestureHandlerRootView } from 'react-native-gesture-handler';
import type { FrameInfo } from 'react-native-reanimated';
import {
  useAnimatedReaction,
  useFrameCallback,
  useSharedValue,
} from 'react-native-reanimated';
import { scheduleOnRN } from 'react-native-worklets';
import { callback } from 'react-native-nitro-modules';
import type {
  CameraState,
  CesiumMetrics,
  CesiumViewMethods,
} from 'react-native-cesium';
import { CesiumView } from 'react-native-cesium';
import {
  SafeAreaProvider,
  useSafeAreaInsets,
} from 'react-native-safe-area-context';
import { Joystick } from './components/Joystick';
import { LayerPicker } from './components/LayerPicker';
import { CreditsDialog } from './components/CreditsDialog';
import { MapGestureHandler } from './components/MapGestureHandler';

const ION_ACCESS_TOKEN =
  'eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJqdGkiOiJhZGIzMGE2My1iODU3LTRkMzYtOTBmOS0wOGFjMWFkZmZiODIiLCJpZCI6MTcxMDczLCJpYXQiOjE3NzM4MjU0Mjd9.cckmnJRd3YpQYQGs7Y7_2YwcVco5elP3Gqhpj1tnoHs';

const INITIAL_CAMERA: CameraState = {
  latitude: 46.02,
  longitude: 7.6,
  altitude: 5800,
  heading: 220,
  pitch: -20,
  roll: 0,
  verticalFovDeg: 60,
};

function AppContent() {
  const insets = useSafeAreaInsets();
  const nativeRef = useRef<CesiumViewMethods | null>(null);
  const [imageryAssetId, setImageryAssetId] = useState(1);
  const [credits, setCredits] = useState('');
  const [creditsExpanded, setCreditsExpanded] = useState(false);

  // ── Camera shared demand (UI-thread, drives gestures + animations) ─────────
  const camera = useSharedValue<CameraState>(INITIAL_CAMERA);

  const joystickPitchRate = useSharedValue(0);
  const joystickRollRate = useSharedValue(0);

  const applyCameraToNative = useCallback((nextCamera: CameraState) => {
    nativeRef.current?.setCamera(nextCamera);
  }, []);

  // Joystick rates → pitch/roll. Both wrap ±180°; pitch loops like an aircraft.
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

  // Bridge UI-thread camera shared value to native imperative method.
  useAnimatedReaction(
    () => ({
      latitude: camera.value.latitude,
      longitude: camera.value.longitude,
      altitude: camera.value.altitude,
      heading: camera.value.heading,
      pitch: camera.value.pitch,
      roll: camera.value.roll,
      verticalFovDeg: camera.value.verticalFovDeg,
    }),
    (cur) => {
      scheduleOnRN(applyCameraToNative, cur);
    },
  );

  const handleJoystickRates = useCallback(
    (pitchRate: number, rollRate: number) => {
      joystickPitchRate.value = pitchRate;
      joystickRollRate.value = rollRate;
    },
    [joystickPitchRate, joystickRollRate],
  );

  const handleMetrics = useCallback(
    (m: CesiumMetrics) => {
      setCredits(m.creditsPlainText);
    },
    [],
  );

  return (
    <View style={styles.container}>
      <StatusBar barStyle="light-content" />

      <MapGestureHandler camera={camera}>
        <CesiumView
          hybridRef={callback((ref: CesiumViewMethods | null) => {
            nativeRef.current = ref;
          })}
          style={styles.map}
          ionAccessToken={ION_ACCESS_TOKEN}
          ionAssetId={1}
          initialCamera={INITIAL_CAMERA}
          pauseRendering={false}
          maximumScreenSpaceError={16}
          maximumSimultaneousTileLoads={8}
          loadingDescendantLimit={20}
          msaaSampleCount={1}
          ionImageryAssetId={imageryAssetId}
          onMetrics={callback(handleMetrics)}
        />
      </MapGestureHandler>

      {/* Joystick + layer picker */}
      <View
        style={[
          styles.bottomCenter,
          {
            bottom:
              insets.bottom +
              16 +
              (credits.length > 0 ? 24 : 0),
          },
        ]}
      >
        <Text style={styles.joystickLabel}>Pitch / Roll</Text>
        <Joystick onRateChange={handleJoystickRates} />
        <LayerPicker selected={imageryAssetId} onSelect={setImageryAssetId} />
      </View>

      {/* Attribution credits */}
      {credits.length > 0 && (
        <TouchableOpacity
          style={[styles.creditsBar, { bottom: insets.bottom + 4 }]}
          onPress={() => setCreditsExpanded(true)}
          activeOpacity={0.75}
        >
          <Text
            style={styles.creditsBarText}
            numberOfLines={1}
            ellipsizeMode="tail"
          >
            ⓘ {credits}
          </Text>
        </TouchableOpacity>
      )}

      <CreditsDialog
        visible={creditsExpanded}
        creditsText={credits}
        onClose={() => setCreditsExpanded(false)}
      />
    </View>
  );
}

function App() {
  return (
    <GestureHandlerRootView style={styles.rootFill}>
      <SafeAreaProvider>
        <AppContent />
      </SafeAreaProvider>
    </GestureHandlerRootView>
  );
}

const styles = StyleSheet.create({
  rootFill: { flex: 1 },
  container: { flex: 1, backgroundColor: '#000' },
  map: { ...StyleSheet.absoluteFillObject },
  bottomCenter: {
    position: 'absolute',
    alignSelf: 'center',
    alignItems: 'center',
    gap: 10,
  },
  joystickLabel: {
    color: 'rgba(255,255,255,0.55)',
    fontSize: 10,
    fontWeight: '500',
    letterSpacing: 0.5,
  },
  creditsBar: {
    position: 'absolute',
    left: 8,
    right: 8,
    backgroundColor: 'rgba(0,0,0,0.50)',
    borderRadius: 4,
    paddingHorizontal: 8,
    paddingVertical: 3,
  },
  creditsBarText: {
    textAlign: 'center',
    fontSize: 10,
    color: 'rgba(255,255,255,0.70)',
  },
});

export default App;
