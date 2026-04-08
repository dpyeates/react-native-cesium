#!/usr/bin/env node
/**
 * If Cesium Native vendor output is missing for the requested platform(s), run
 * `npm|yarn run update` then `CESIUM_BUILD_ONLY=… npm|yarn run build`.
 *
 * Invoked from CocoaPods (podspec prepare_command) and Android Gradle (preBuild).
 *
 * Opt out (CI without toolchains): REACT_NATIVE_CESIUM_SKIP_NATIVE_BUILD=1 — fails fast if artifacts are still missing.
 */
import { spawnSync } from 'node:child_process'
import { existsSync } from 'node:fs'
import { dirname, join, resolve } from 'node:path'
import { fileURLToPath } from 'node:url'

const __dirname = dirname(fileURLToPath(import.meta.url))
const root = resolve(join(__dirname, '..', '..'))

const MARKER_IOS = join(root, 'vendor', 'ios', 'CesiumNative.xcframework', 'Info.plist')
const MARKER_ANDROID = join(
  root,
  'vendor',
  'android',
  'share',
  'cesium-native',
  'cmake',
  'cesium-nativeConfig.cmake'
)

function parseArgs(argv) {
  const want = { ios: false, android: false }
  for (const a of argv) {
    if (a === '--ios') want.ios = true
    else if (a === '--android') want.android = true
  }
  if (!want.ios && !want.android) {
    want.ios = true
    want.android = true
  }
  return want
}

function useYarn() {
  return existsSync(join(root, 'yarn.lock'))
}

function runScript(name, extraEnv) {
  const yarn = useYarn()
  const cmd = yarn ? 'yarn' : 'npm'
  const args = yarn ? ['run', name] : ['run', name]
  const env = { ...process.env, ...extraEnv }
  const r = spawnSync(cmd, args, { cwd: root, env, stdio: 'inherit' })
  if (r.error) {
    console.error(r.error)
    process.exit(1)
  }
  const code = r.status
  if (code !== 0) {
    process.exit(code === null ? 1 : code)
  }
}

function main() {
  const skip = process.env.REACT_NATIVE_CESIUM_SKIP_NATIVE_BUILD === '1'
  const want = parseArgs(process.argv.slice(2))

  const needIos = want.ios && !existsSync(MARKER_IOS)
  const needAndroid = want.android && !existsSync(MARKER_ANDROID)

  if (!needIos && !needAndroid) {
    return
  }

  if (skip) {
    console.error(
      'react-native-cesium: Cesium Native vendor output is missing, but REACT_NATIVE_CESIUM_SKIP_NATIVE_BUILD=1.\n' +
        'Build the native libraries on a machine with the required toolchains, or unset the variable and retry.\n' +
        `Expected iOS: ${MARKER_IOS}\n` +
        `Expected Android: ${MARKER_ANDROID}\n` +
        'See the package README (Build Cesium Native locally).'
    )
    process.exit(1)
  }

  if (needIos && process.platform !== 'darwin') {
    console.error(
      'react-native-cesium: iOS Cesium Native output is missing; building it requires macOS with Xcode.\n' +
        `Expected: ${MARKER_IOS}`
    )
    process.exit(1)
  }

  if (!existsSync(join(root, 'vendor', 'cesium-native'))) {
    console.log('react-native-cesium: running package update (clone Cesium Native / vcpkg prep)…')
    runScript('update', {})
  }

  if (needIos && !existsSync(MARKER_IOS)) {
    console.log('react-native-cesium: building Cesium Native for iOS (this can take a long time)…')
    runScript('build', { CESIUM_BUILD_ONLY: 'ios' })
  }
  if (needAndroid && !existsSync(MARKER_ANDROID)) {
    console.log('react-native-cesium: building Cesium Native for Android (this can take a long time)…')
    runScript('build', { CESIUM_BUILD_ONLY: 'android' })
  }

  if (needIos && !existsSync(MARKER_IOS)) {
    console.error(`react-native-cesium: iOS build finished but marker missing: ${MARKER_IOS}`)
    process.exit(1)
  }
  if (needAndroid && !existsSync(MARKER_ANDROID)) {
    console.error(`react-native-cesium: Android build finished but marker missing: ${MARKER_ANDROID}`)
    process.exit(1)
  }
}

main()
