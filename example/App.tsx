import React, { useRef, useState, useCallback, useMemo } from 'react';
import {
  StatusBar,
  StyleSheet,
  View,
  Text,
  TouchableOpacity,
} from 'react-native';
import { GestureHandlerRootView, Gesture, GestureDetector } from 'react-native-gesture-handler';
import Animated, {
  useSharedValue,
  useAnimatedStyle,
  runOnJS,
} from 'react-native-reanimated';
import { callback } from 'react-native-nitro-modules';
import { CesiumView } from 'react-native-cesium';
import type { CesiumViewMethods } from 'react-native-cesium';
import { SafeAreaProvider, useSafeAreaInsets } from 'react-native-safe-area-context';

const ION_ACCESS_TOKEN =
  'eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJqdGkiOiJhZGIzMGE2My1iODU3LTRkMzYtOTBmOS0wOGFjMWFkZmZiODIiLCJpZCI6MTcxMDczLCJpYXQiOjE3NzM4MjU0Mjd9.cckmnJRd3YpQYQGs7Y7_2YwcVco5elP3Gqhpj1tnoHs';

// ── Layer selector data ──────────────────────────────────────────────────────

type LayerOption = { label: string; assetId: number };

const LAYER_OPTIONS: LayerOption[] = [
  { label: 'Terrain',     assetId: 1       },
  { label: 'Bing Aerial', assetId: 2       },
  { label: 'Google Sat',  assetId: 3830182 },
  { label: 'Contour',     assetId: 3830186 },
];

// ── Joystick constants ───────────────────────────────────────────────────────

const JOYSTICK_RADIUS = 52;
const THUMB_RADIUS    = 20;
const DEAD_ZONE_PX    = 8;
/** Space above the home indicator for the native Cesium credits UILabel inside the map view. */
const NATIVE_CREDITS_FOOTER_RESERVE = 44;
/** Max commanded rate (deg/s) at full stick deflection — same for pitch and roll. */
const JOYSTICK_MAX_RATE_DEG_S = 110;

// ── Joystick ─────────────────────────────────────────────────────────────────

type JoystickProps = { onRateChange: (pitch: number, roll: number) => void };

function Joystick({ onRateChange }: JoystickProps) {
  const thumbX = useSharedValue(0);
  const thumbY = useSharedValue(0);

  // Rates → JS thread only; thumb position runs entirely on the UI thread in the gesture worklet
  // so the white circle tracks the finger/mouse without JS-frame stutter.
  const emitRates = useCallback(
    (tx: number, ty: number) => {
      const dist = Math.sqrt(tx * tx + ty * ty);
      if (dist < DEAD_ZONE_PX) {
        onRateChange(0, 0);
        return;
      }
      const clamp  = Math.min(dist, JOYSTICK_RADIUS);
      const factor = clamp / JOYSTICK_RADIUS;
      const scale  = factor * JOYSTICK_MAX_RATE_DEG_S;
      const pitchRate = (ty / dist) * scale;
      const rollRate  = -(tx / dist) * scale;
      onRateChange(pitchRate, rollRate);
    },
    [onRateChange]
  );

  const stopRates = useCallback(() => {
    onRateChange(0, 0);
  }, [onRateChange]);

  const panGesture = useMemo(
    () =>
      Gesture.Pan()
        .minDistance(0)
        .onBegin(() => {
          'worklet';
          thumbX.value = 0;
          thumbY.value = 0;
        })
        .onUpdate((e) => {
          'worklet';
          const tx = e.translationX;
          const ty = e.translationY;
          const dist = Math.sqrt(tx * tx + ty * ty);
          const clamp = Math.min(dist, JOYSTICK_RADIUS);
          if (dist > 0) {
            const nx = (tx / dist) * clamp;
            const ny = (ty / dist) * clamp;
            thumbX.value = nx;
            thumbY.value = ny;
          } else {
            thumbX.value = 0;
            thumbY.value = 0;
          }
          runOnJS(emitRates)(tx, ty);
        })
        // Single cleanup path (runs on end, cancel, and interrupt).
        .onFinalize(() => {
          'worklet';
          thumbX.value = 0;
          thumbY.value = 0;
          runOnJS(stopRates)();
        }),
    [emitRates, stopRates]
  );

  const thumbStyle = useAnimatedStyle(() => ({
    transform: [{ translateX: thumbX.value }, { translateY: thumbY.value }],
  }));

  const size   = JOYSTICK_RADIUS * 2;
  const center = JOYSTICK_RADIUS - THUMB_RADIUS;

  return (
    <GestureDetector gesture={panGesture}>
      <View
        style={[joystickStyles.base, { width: size, height: size, borderRadius: JOYSTICK_RADIUS }]}>
        <View style={joystickStyles.crossH} />
        <View style={joystickStyles.crossV} />
        <Animated.View
          style={[
            joystickStyles.thumb,
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
}

const joystickStyles = StyleSheet.create({
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

// ── Layer picker ──────────────────────────────────────────────────────────────

type LayerPickerProps = { selected: number; onSelect: (id: number) => void };

function LayerPicker({ selected, onSelect }: LayerPickerProps) {
  return (
    <View style={pickerStyles.row}>
      {LAYER_OPTIONS.map((opt) => {
        const active = opt.assetId === selected;
        return (
          <TouchableOpacity
            key={opt.assetId}
            style={[pickerStyles.pill, active && pickerStyles.pillActive]}
            onPress={() => onSelect(opt.assetId)}
            activeOpacity={0.75}>
            <Text style={[pickerStyles.label, active && pickerStyles.labelActive]}>
              {opt.label}
            </Text>
          </TouchableOpacity>
        );
      })}
    </View>
  );
}

const pickerStyles = StyleSheet.create({
  row: {
    flexDirection: 'row',
    backgroundColor: 'rgba(0,0,0,0.55)',
    borderRadius: 24,
    padding: 4,
    gap: 4,
  },
  pill: { paddingHorizontal: 12, paddingVertical: 7, borderRadius: 20 },
  pillActive: { backgroundColor: 'rgba(255,255,255,0.92)' },
  label: { fontSize: 13, fontWeight: '500', color: 'rgba(255,255,255,0.85)' },
  labelActive: { color: '#1a1a1a' },
});

// ── Main App ──────────────────────────────────────────────────────────────────

function AppContent() {
  const insets = useSafeAreaInsets();

  const nativeMethods = useRef<CesiumViewMethods | null>(null);
  const [imageryAssetId, setImageryAssetId] = useState(1);

  // ── Joystick handler ──────────────────────────────────────────────────────
  const handleJoystickRates = useCallback(
    (pitchRate: number, rollRate: number) => {
      nativeMethods.current?.setJoystickRates(pitchRate, rollRate);
    },
    []
  );

  return (
    <View style={styles.container}>
      <StatusBar barStyle="light-content" />

      <CesiumView
        // Nitro: raw functions passed to native views become `true` unless wrapped
        // with `callback()` — otherwise hybridRef never runs and native methods stay null.
        hybridRef={callback((ref: CesiumViewMethods | null) => {
          nativeMethods.current = ref;
        })}
        style={styles.map}
        ionAccessToken={ION_ACCESS_TOKEN}
        ionAssetId={1}
        cameraLatitude={46.02}
        cameraLongitude={7.60}
        cameraAltitude={5800}
        cameraHeading={220}
        cameraPitch={-20}
        cameraRoll={0}
        cameraVerticalFovDeg={60}
        debugOverlay={false}
        pauseRendering={false}
        gesturePanEnabled
        gesturePinchZoomEnabled
        gesturePinchRotateEnabled
        gesturePanSensitivity={1}
        gesturePinchSensitivity={1}
        maximumScreenSpaceError={32}
        maximumSimultaneousTileLoads={12}
        loadingDescendantLimit={20}
        msaaSampleCount={1}
        showCreditsFooter
        ionImageryAssetId={imageryAssetId}
      />

      {/* Joystick + layer picker — above native attribution footer drawn on the map */}
      <View
        style={[
          styles.bottomCenter,
          { bottom: insets.bottom + 16 + NATIVE_CREDITS_FOOTER_RESERVE },
        ]}>
        <Text style={styles.joystickLabel}>Pitch / Roll</Text>
        <Joystick onRateChange={handleJoystickRates} />
        <LayerPicker
          selected={imageryAssetId}
          onSelect={setImageryAssetId}
        />
      </View>
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
  rootFill: {
    flex: 1,
  },
  container: {
    flex: 1,
    backgroundColor: '#000',
  },
  map: {
    ...StyleSheet.absoluteFillObject,
  },
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
});

export default App;
