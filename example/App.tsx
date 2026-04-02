import React, { useRef, useState, useCallback, useMemo, memo } from 'react';
import {
  Modal,
  Pressable,
  ScrollView,
  StatusBar,
  StyleSheet,
  View,
  Text,
  TouchableOpacity,
} from 'react-native';
import {
  GestureHandlerRootView,
  Gesture,
  GestureDetector,
} from 'react-native-gesture-handler';
import Animated, {
  useSharedValue,
  useAnimatedStyle,
  useAnimatedReaction,
  withDecay,
  withSpring,
} from 'react-native-reanimated';
import { scheduleOnRN } from 'react-native-worklets';
import { callback } from 'react-native-nitro-modules';
import { CesiumView } from 'react-native-cesium';
import type { CesiumViewMethods, CesiumMetrics } from 'react-native-cesium';
import {
  SafeAreaProvider,
  useSafeAreaInsets,
} from 'react-native-safe-area-context';

const ION_ACCESS_TOKEN =
  'eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJqdGkiOiJhZGIzMGE2My1iODU3LTRkMzYtOTBmOS0wOGFjMWFkZmZiODIiLCJpZCI6MTcxMDczLCJpYXQiOjE3NzM4MjU0Mjd9.cckmnJRd3YpQYQGs7Y7_2YwcVco5elP3Gqhpj1tnoHs';

// ── Initial camera ────────────────────────────────────────────────────────────

const INIT_LAT = 46.02;
const INIT_LON = 7.6;
const INIT_ALT = 5800;
const INIT_HDG = 220;

// ── Camera gesture constants ──────────────────────────────────────────────────

// Degrees per pixel per metre of altitude — matches native GestureHandler scale.
const PAN_SCALE = 1.55e-8;
const MIN_ALT_ABOVE_TERRAIN = 3; // metres above terrain to clamp minimum altitude
const MIN_ALT_ABSOLUTE = 3;      // absolute fallback before first terrain estimate arrives
const MAX_ALT = 100_000_000;

// ── Layer selector data ───────────────────────────────────────────────────────

type LayerOption = { label: string; assetId: number };

const LAYER_OPTIONS: LayerOption[] = [
  { label: 'Terrain', assetId: 1 },
  { label: 'Bing', assetId: 2 },
  { label: 'Google', assetId: 3830182 },
  { label: 'Contour', assetId: 3830186 },
];

// ── Joystick constants ────────────────────────────────────────────────────────

const JOYSTICK_RADIUS = 52;
const THUMB_RADIUS = 20;
const DEAD_ZONE_PX = 8;
const CREDITS_BAR_HEIGHT = 26;
const JOYSTICK_MAX_RATE_DEG_S = 110;

// ── Joystick ──────────────────────────────────────────────────────────────────

type JoystickProps = { onRateChange: (pitch: number, roll: number) => void };

const Joystick = memo(function Joystick({ onRateChange }: JoystickProps) {
  const thumbX = useSharedValue(0);
  const thumbY = useSharedValue(0);

  const emitRates = useCallback(
    (tx: number, ty: number) => {
      const dist = Math.sqrt(tx * tx + ty * ty);
      if (dist < DEAD_ZONE_PX) {
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
          thumbX.value = withSpring(0, { damping: 20, stiffness: 300 });
          thumbY.value = withSpring(0, { damping: 20, stiffness: 300 });
          scheduleOnRN(stopRates);
        }),
    [emitRates, stopRates],
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
          joystickStyles.base,
          { width: size, height: size, borderRadius: JOYSTICK_RADIUS },
        ]}
      >
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
});

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
      {LAYER_OPTIONS.map(opt => {
        const active = opt.assetId === selected;
        return (
          <TouchableOpacity
            key={opt.assetId}
            style={[pickerStyles.pill, active && pickerStyles.pillActive]}
            onPress={() => onSelect(opt.assetId)}
            activeOpacity={0.75}
          >
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
  const [credits, setCredits] = useState('');
  const [creditsExpanded, setCreditsExpanded] = useState(false);

  // ── Terrain height shared value (updated from onMetrics ~3fps) ───────────
  const terrainHeight = useSharedValue(0);

  // ── Camera shared values (UI-thread, drives gestures + animations) ─────────
  const camLat = useSharedValue(INIT_LAT);
  const camLon = useSharedValue(INIT_LON);
  const camAlt = useSharedValue(INIT_ALT);
  const camHdg = useSharedValue(INIT_HDG);

  // Snapshots taken at the start of each gesture to compute absolute deltas.
  const snapLat = useSharedValue(INIT_LAT);
  const snapLon = useSharedValue(INIT_LON);
  const snapAlt = useSharedValue(INIT_ALT);
  const snapHdg = useSharedValue(INIT_HDG);

  // ── Camera React state — synced from shared values via useAnimatedReaction ─
  // CesiumView props are driven from here; the sync runs on the JS thread
  // one frame behind the UI-thread gesture, which is imperceptible at 60 fps.
  const [camState, setCamState] = useState({
    lat: INIT_LAT,
    lon: INIT_LON,
    alt: INIT_ALT,
    hdg: INIT_HDG,
  });

  const applyCamera = useCallback(
    (lat: number, lon: number, alt: number, hdg: number) => {
      setCamState({ lat, lon, alt, hdg });
    },
    [],
  );

  useAnimatedReaction(
    () => ({
      lat: camLat.value,
      lon: camLon.value,
      alt: camAlt.value,
      hdg: camHdg.value,
    }),
    (cur, prev) => {
      if (
        !prev ||
        cur.lat !== prev.lat ||
        cur.lon !== prev.lon ||
        cur.alt !== prev.alt ||
        cur.hdg !== prev.hdg
      ) {
        scheduleOnRN(applyCamera, cur.lat, cur.lon, cur.alt, cur.hdg);
      }
    },
  );

  // ── 1-finger pan: moves the camera laterally ──────────────────────────────
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
          // Convert pixel velocity → degrees/second and apply decay animation.
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
    [],
  );

  // ── 2-finger pinch: adjusts altitude (fingers apart = zoom in / lower alt) ─
  const pinchGesture = useMemo(
    () =>
      Gesture.Pinch()
        .onBegin(() => {
          'worklet';
          // Clamp snap to current minAlt so the gesture never starts from
          // a below-terrain baseline.
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
    [],
  );

  // ── 2-finger rotation: rotates the heading ───────────────────────────────
  // e.rotation is in radians; positive = clockwise finger rotation.
  // Clockwise fingers → map rotates CW under the camera → heading increases.
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
            ((snapHdg.value + (e.rotation * 180) / Math.PI) % 360 + 360) %
            360;
        }),
    [],
  );

  // Pinch and rotation fire simultaneously on two fingers.
  const pinchRotateGesture = useMemo(
    () => Gesture.Simultaneous(pinchGesture, rotationGesture),
    [pinchGesture, rotationGesture],
  );

  // Pan (1-finger) is independent; it cannot activate while 2+ fingers are down.
  const mapGesture = useMemo(
    () => Gesture.Simultaneous(panGesture, pinchRotateGesture),
    [panGesture, pinchRotateGesture],
  );

  // ── Joystick handler (pitch/roll via native rate mechanism) ───────────────
  const handleJoystickRates = useCallback(
    (pitchRate: number, rollRate: number) => {
      nativeMethods.current?.setJoystickRates(pitchRate, rollRate);
    },
    [],
  );

  const handleMetrics = useCallback((m: CesiumMetrics) => {
    setCredits(m.creditsPlainText);
    terrainHeight.value = m.terrainHeightBelowCamera;
  }, []);

  return (
    <View style={styles.container}>
      <StatusBar barStyle="light-content" />

      {/* Map with gesture overlay */}
      <GestureDetector gesture={mapGesture}>
        <CesiumView
          hybridRef={callback((ref: CesiumViewMethods | null) => {
            nativeMethods.current = ref;
          })}
          style={styles.map}
          ionAccessToken={ION_ACCESS_TOKEN}
          ionAssetId={1}
          cameraLatitude={camState.lat}
          cameraLongitude={camState.lon}
          cameraAltitude={camState.alt}
          cameraHeading={camState.hdg}
          cameraPitch={-20}
          cameraRoll={0}
          cameraVerticalFovDeg={60}
          debugOverlay={false}
          pauseRendering={false}
          maximumScreenSpaceError={32}
          maximumSimultaneousTileLoads={12}
          loadingDescendantLimit={20}
          msaaSampleCount={1}
          showCredits={true}
          ionImageryAssetId={imageryAssetId}
          onMetrics={callback(handleMetrics)}
        />
      </GestureDetector>

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

      {/* Full credits dialog */}
      <Modal
        visible={creditsExpanded}
        transparent
        animationType="fade"
        onRequestClose={() => setCreditsExpanded(false)}
      >
        <Pressable
          style={styles.creditsOverlay}
          onPress={() => setCreditsExpanded(false)}
        >
          <Pressable style={styles.creditsDialog} onPress={() => {}}>
            <Text style={styles.creditsDialogTitle}>Data Attribution</Text>
            <ScrollView
              style={styles.creditsDialogScroll}
              showsVerticalScrollIndicator={false}
            >
              <Text style={styles.creditsDialogBody} selectable>
                {credits}
              </Text>
            </ScrollView>
            <TouchableOpacity
              style={styles.creditsDialogClose}
              onPress={() => setCreditsExpanded(false)}
              activeOpacity={0.8}
            >
              <Text style={styles.creditsDialogCloseText}>Close</Text>
            </TouchableOpacity>
          </Pressable>
        </Pressable>
      </Modal>
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
  creditsOverlay: {
    flex: 1,
    backgroundColor: 'rgba(0,0,0,0.55)',
    justifyContent: 'center',
    alignItems: 'center',
    padding: 24,
  },
  creditsDialog: {
    width: '100%',
    maxWidth: 480,
    backgroundColor: '#1a1a1a',
    borderRadius: 12,
    padding: 20,
    shadowColor: '#000',
    shadowOffset: { width: 0, height: 8 },
    shadowOpacity: 0.6,
    shadowRadius: 20,
    elevation: 12,
  },
  creditsDialogTitle: {
    fontSize: 15,
    fontWeight: '600',
    color: '#ffffff',
    marginBottom: 12,
  },
  creditsDialogScroll: { maxHeight: 220 },
  creditsDialogBody: {
    fontSize: 13,
    lineHeight: 20,
    color: 'rgba(255,255,255,0.75)',
  },
  creditsDialogClose: {
    marginTop: 16,
    alignSelf: 'flex-end',
    backgroundColor: 'rgba(255,255,255,0.12)',
    paddingHorizontal: 18,
    paddingVertical: 8,
    borderRadius: 8,
  },
  creditsDialogCloseText: {
    fontSize: 14,
    fontWeight: '500',
    color: '#ffffff',
  },
});

export default App;
