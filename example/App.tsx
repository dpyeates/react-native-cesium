import React, { useState, useCallback, useRef } from 'react';
import {
  StatusBar,
  StyleSheet,
  View,
  Text,
  TouchableOpacity,
} from 'react-native';
import { GestureHandlerRootView } from 'react-native-gesture-handler';
import {
  useSharedValue,
  useAnimatedReaction,
  useFrameCallback,
} from 'react-native-reanimated';
import type { FrameInfo } from 'react-native-reanimated';
import { scheduleOnRN } from 'react-native-worklets';
import { callback } from 'react-native-nitro-modules';
import { CesiumView } from 'react-native-cesium';
import type { CesiumMetrics, CesiumViewMethods } from 'react-native-cesium';
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

// ── Initial camera ────────────────────────────────────────────────────────────

const INIT_LAT = 46.02;
const INIT_LON = 7.6;
const INIT_ALT = 5800;
const INIT_HDG = 220;
const INIT_PITCH = -20;
const INIT_ROLL = 0;

const CREDITS_BAR_HEIGHT = 26;

// ── Main App ──────────────────────────────────────────────────────────────────

function AppContent() {
  const insets = useSafeAreaInsets();
  const nativeRef = useRef<CesiumViewMethods | null>(null);
  const [imageryAssetId, setImageryAssetId] = useState(1);
  const [credits, setCredits] = useState('');
  const [creditsExpanded, setCreditsExpanded] = useState(false);

  // ── Terrain height shared value (updated from onMetrics ~3fps) ───────────
  const terrainHeight = useSharedValue(0);

  // ── Camera shared values (UI-thread, drives gestures + animations) ─────────
  const camLat   = useSharedValue(INIT_LAT);
  const camLon   = useSharedValue(INIT_LON);
  const camAlt   = useSharedValue(INIT_ALT);
  const camHdg   = useSharedValue(INIT_HDG);
  const camPitch = useSharedValue(INIT_PITCH);
  const camRoll  = useSharedValue(INIT_ROLL);

  const joystickPitchRate = useSharedValue(0);
  const joystickRollRate  = useSharedValue(0);

  // Snapshots taken at the start of each gesture to compute absolute deltas.
  const snapLat = useSharedValue(INIT_LAT);
  const snapLon = useSharedValue(INIT_LON);
  const snapAlt = useSharedValue(INIT_ALT);
  const snapHdg = useSharedValue(INIT_HDG);

  const [camState, setCamState] = useState({
    lat:   INIT_LAT,
    lon:   INIT_LON,
    alt:   INIT_ALT,
    hdg:   INIT_HDG,
    pitch: INIT_PITCH,
    roll:  INIT_ROLL,
  });

  const applyCamera = useCallback(
    (
      lat: number,
      lon: number,
      alt: number,
      hdg: number,
      pitch: number,
      roll: number,
    ) => {
      setCamState({ lat, lon, alt, hdg, pitch, roll });
    },
    [],
  );

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
      let p = camPitch.value + pr * dt;
      let r = camRoll.value + rr * dt;
      while (p > 180) p -= 360;
      while (p < -180) p += 360;
      while (r > 180) r -= 360;
      while (r < -180) r += 360;
      camPitch.value = p;
      camRoll.value = r;
    },
    [joystickPitchRate, joystickRollRate, camPitch, camRoll],
  );

  useFrameCallback(integrateJoystick);

  // All 6 DOF → React props → native demand target.
  useAnimatedReaction(
    () => ({
      lat:   camLat.value,
      lon:   camLon.value,
      alt:   camAlt.value,
      hdg:   camHdg.value,
      pitch: camPitch.value,
      roll:  camRoll.value,
    }),
    (cur, prev) => {
      if (
        !prev ||
        cur.lat   !== prev.lat   ||
        cur.lon   !== prev.lon   ||
        cur.alt   !== prev.alt   ||
        cur.hdg   !== prev.hdg   ||
        cur.pitch !== prev.pitch ||
        cur.roll  !== prev.roll
      ) {
        scheduleOnRN(
          applyCamera,
          cur.lat,
          cur.lon,
          cur.alt,
          cur.hdg,
          cur.pitch,
          cur.roll,
        );
      }
    },
  );

  const handleJoystickRates = useCallback(
    (pitchRate: number, rollRate: number) => {
      joystickPitchRate.value = pitchRate;
      joystickRollRate.value  = rollRate;
    },
    [joystickPitchRate, joystickRollRate],
  );

  const handleMetrics = useCallback(
    (m: CesiumMetrics) => {
      setCredits(m.creditsPlainText);
      terrainHeight.value = m.terrainHeightBelowCamera;
    },
    [terrainHeight],
  );

  return (
    <View style={styles.container}>
      <StatusBar barStyle="light-content" />

      <MapGestureHandler
        camLat={camLat}
        camLon={camLon}
        camAlt={camAlt}
        camHdg={camHdg}
        snapLat={snapLat}
        snapLon={snapLon}
        snapAlt={snapAlt}
        snapHdg={snapHdg}
        terrainHeight={terrainHeight}
      >
        <CesiumView
          hybridRef={callback((ref: CesiumViewMethods | null) => {
            nativeRef.current = ref;
          })}
          style={styles.map}
          ionAccessToken={ION_ACCESS_TOKEN}
          ionAssetId={1}
          cameraLatitude={camState.lat}
          cameraLongitude={camState.lon}
          cameraAltitude={camState.alt}
          cameraHeading={camState.hdg}
          cameraPitch={camState.pitch}
          cameraRoll={camState.roll}
          cameraVerticalFovDeg={60}
          debugOverlay={false}
          pauseRendering={false}
          maximumScreenSpaceError={16}
          maximumSimultaneousTileLoads={8}
          loadingDescendantLimit={20}
          msaaSampleCount={1}
          showCredits={true}
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
              (credits.length > 0 ? CREDITS_BAR_HEIGHT + 8 : 0),
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
