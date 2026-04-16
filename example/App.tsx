import React, { useCallback, useMemo, useState } from 'react';
import {
  StatusBar,
  StyleSheet,
  Text,
  TouchableOpacity,
  View,
} from 'react-native';
import { GestureHandlerRootView } from 'react-native-gesture-handler';
import { callback } from 'react-native-nitro-modules';
import type { CameraState, CesiumMetrics } from 'react-native-cesium';
import { CesiumView } from 'react-native-cesium';
import {
  SafeAreaProvider,
  useSafeAreaInsets,
} from 'react-native-safe-area-context';
import { CESIUM_ION_ACCESS_TOKEN, ION_ACCESS_TOKEN } from '@env';
import { Joystick } from './components/Joystick';
import { LayerPicker } from './components/LayerPicker';
import { CreditsDialog } from './components/CreditsDialog';
import { MapGestureHandler } from './components/MapGestureHandler';
import { useCameraController } from './hooks/useCameraController';

const ionAccessToken = (CESIUM_ION_ACCESS_TOKEN ?? ION_ACCESS_TOKEN ?? '').trim();
const hasIonToken = ionAccessToken.length > 0;

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
  const [imageryAssetId, setImageryAssetId] = useState(1);
  const [credits, setCredits] = useState('');
  const [creditsExpanded, setCreditsExpanded] = useState(false);

  const { camera, hudCamera, setCesiumView, handleJoystickRates } =
    useCameraController(INITIAL_CAMERA);

  const hybridRef = useMemo(() => callback(setCesiumView), [setCesiumView]);

  const handleMetrics = useCallback((m: CesiumMetrics) => {
    setCredits((prev) => (prev === m.creditsPlainText ? prev : m.creditsPlainText));
  }, []);

  const metricsCallback = useMemo(
    () => callback(handleMetrics),
    [handleMetrics],
  );

  const handleCloseCredits = useCallback(() => setCreditsExpanded(false), []);
  const handleOpenCredits = useCallback(() => setCreditsExpanded(true), []);

  return (
    <View style={styles.container}>
      <StatusBar barStyle="light-content" />

      <MapGestureHandler camera={camera} initialCamera={INITIAL_CAMERA}>
        <CesiumView
          hybridRef={hybridRef}
          style={StyleSheet.absoluteFill}
          ionAccessToken={ionAccessToken}
          ionAssetId={1}
          initialCamera={INITIAL_CAMERA}
          pauseRendering={false}
          maximumScreenSpaceError={16}
          maximumSimultaneousTileLoads={8}
          loadingDescendantLimit={20}
          msaaSampleCount={1}
          ionImageryAssetId={imageryAssetId}
          onMetrics={metricsCallback}
        />
      </MapGestureHandler>

      {!hasIonToken && (
        <View style={[styles.missingTokenBanner, { top: insets.top + 12 }]}>
          <Text style={styles.missingTokenTitle}>Cesium Ion token missing</Text>
          <Text style={styles.missingTokenBody}>
            Create `example/.env` from `example/.env_example` and set
            `CESIUM_ION_ACCESS_TOKEN`.
          </Text>
        </View>
      )}

      {hasIonToken && (
        <View style={[styles.cameraHud, { top: insets.top + 12 }]}>
          <Text style={styles.cameraHudText}>
            Lat {hudCamera.latitude.toFixed(5)}°, Lon{' '}
            {hudCamera.longitude.toFixed(5)}°
          </Text>
          <Text style={styles.cameraHudText}>
            Alt {Math.round(hudCamera.altitude).toLocaleString()} m
          </Text>
          <Text style={styles.cameraHudText}>
            Heading {hudCamera.heading.toFixed(1)}°
          </Text>
        </View>
      )}

      <View
        style={[
          styles.bottomCenter,
          { bottom: insets.bottom + 16 + (credits.length > 0 ? 24 : 0) },
        ]}
      >
        <Text style={styles.joystickLabel}>Pitch / Roll</Text>
        <Joystick onRateChange={handleJoystickRates} />
        <LayerPicker selected={imageryAssetId} onSelect={setImageryAssetId} />
      </View>

      {credits.length > 0 && (
        <TouchableOpacity
          style={[styles.creditsBar, { bottom: insets.bottom + 4 }]}
          onPress={handleOpenCredits}
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
        onClose={handleCloseCredits}
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
  cameraHud: {
    position: 'absolute',
    left: 12,
    backgroundColor: 'rgba(0,0,0,0.32)',
    borderRadius: 8,
    paddingHorizontal: 10,
    paddingVertical: 8,
    gap: 2,
  },
  cameraHudText: {
    color: '#fff',
    fontSize: 12,
    fontVariant: ['tabular-nums'],
  },
  missingTokenBanner: {
    position: 'absolute',
    left: 12,
    right: 12,
    backgroundColor: 'rgba(120, 20, 20, 0.92)',
    borderRadius: 10,
    paddingHorizontal: 12,
    paddingVertical: 10,
    gap: 4,
  },
  missingTokenTitle: {
    color: '#fff',
    fontSize: 14,
    fontWeight: '700',
  },
  missingTokenBody: {
    color: 'rgba(255,255,255,0.9)',
    fontSize: 12,
    lineHeight: 18,
  },
});

export default App;
