#!/usr/bin/env node
/**
 * Ensure Cesium Native vendor output is present and built for the pinned ref.
 *
 * Fast-path (in order):
 *   1. Vendor dir already exists and was built for the current ref → nothing to do.
 *   2. Global cache (~/.cache/react-native-cesium/<ref>/<platform>/) has a matching
 *      build → restore from cache without recompiling.
 *   3. Otherwise → clone source and build, then populate the global cache.
 *
 * Invoked from CocoaPods (react_native_cesium_ensure_native in Podfile pre_install)
 * and Android Gradle (preBuild task).
 *
 * Opt out (CI without toolchains): REACT_NATIVE_CESIUM_SKIP_NATIVE_BUILD=1 —
 * fails fast if artifacts are still missing.
 */
import { spawnSync } from 'node:child_process'
import { cpSync, existsSync, mkdirSync, readFileSync, rmSync, writeFileSync } from 'node:fs'
import { homedir } from 'node:os'
import { dirname, join, resolve } from 'node:path'
import { fileURLToPath } from 'node:url'
import { CESIUM_NATIVE_REF } from './config.mjs'

const __dirname = dirname(fileURLToPath(import.meta.url))
const root = resolve(join(__dirname, '..', '..'))

// Marker files whose existence confirms a successful build.
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

// Stamp files that record which cesium-native ref the vendor dir was built from.
const REF_STAMP_IOS = join(root, 'vendor', 'ios', '.cesium-native-ref')
const REF_STAMP_ANDROID = join(root, 'vendor', 'android', '.cesium-native-ref')

// ─── Global cache ────────────────────────────────────────────────────────────
// Keyed by cesium-native ref so builds for different versions coexist.
// Survives node_modules being cleared between pod installs.

function globalCacheBase() {
  const xdg = process.env.XDG_CACHE_HOME?.trim()
  return xdg
    ? join(xdg, 'react-native-cesium')
    : join(homedir(), '.cache', 'react-native-cesium')
}

function cacheDir(platform, ref) {
  return join(globalCacheBase(), ref, platform)
}

// ─── Helpers ─────────────────────────────────────────────────────────────────

function readStamp(stampPath) {
  try {
    return readFileSync(stampPath, 'utf8').trim()
  } catch {
    return null
  }
}

function isStale(markerPath, stampPath, ref) {
  if (!existsSync(markerPath)) return true
  const built = readStamp(stampPath)
  if (built !== ref) {
    console.log(
      `react-native-cesium: vendor built for ${built ?? 'unknown ref'}, current ref is ${ref} — will refresh.`
    )
    return true
  }
  return false
}

/**
 * Try to restore a platform's vendor dir from the global cache.
 * Returns true if the cache hit and the restore succeeded.
 */
function tryRestoreFromCache(platform, ref, vendorDir, stampPath, markerPath) {
  const src = cacheDir(platform, ref)
  if (!existsSync(src)) return false

  console.log(
    `react-native-cesium: cache hit for ${platform} ${ref} — restoring from ${src} (skipping build)`
  )
  try {
    rmSync(vendorDir, { recursive: true, force: true })
    mkdirSync(vendorDir, { recursive: true })
    cpSync(src, vendorDir, { recursive: true })
    // Ensure stamp is present even if the cached dir predates stamp support.
    writeFileSync(stampPath, ref)
    if (!existsSync(markerPath)) {
      console.warn(
        `react-native-cesium: cache restore for ${platform} ${ref} succeeded but marker is missing: ${markerPath}\n` +
          'The cache entry may be corrupt; delete it and retry:\n' +
          `  rm -rf "${src}"`
      )
      return false
    }
    return true
  } catch (e) {
    console.warn(
      `react-native-cesium: cache restore failed (${e.message}) — will rebuild.`
    )
    return false
  }
}

/** Populate the global cache from a freshly built vendor dir. */
function saveToCache(platform, ref, vendorDir) {
  const dst = cacheDir(platform, ref)
  try {
    rmSync(dst, { recursive: true, force: true })
    mkdirSync(dst, { recursive: true })
    cpSync(vendorDir, dst, { recursive: true })
    console.log(
      `react-native-cesium: saved ${platform} ${ref} build to cache (${dst})`
    )
  } catch (e) {
    console.warn(
      `react-native-cesium: could not write to cache (${e.message}) — build succeeded but next run may rebuild.`
    )
  }
}

// ─── Script runner ───────────────────────────────────────────────────────────

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
  const args = ['run', name]
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

// ─── Main ────────────────────────────────────────────────────────────────────

function main() {
  const skip = process.env.REACT_NATIVE_CESIUM_SKIP_NATIVE_BUILD === '1'
  const want = parseArgs(process.argv.slice(2))
  const ref = CESIUM_NATIVE_REF

  const vendorIos = join(root, 'vendor', 'ios')
  const vendorAndroid = join(root, 'vendor', 'android')

  let needIos = want.ios && isStale(MARKER_IOS, REF_STAMP_IOS, ref)
  let needAndroid = want.android && isStale(MARKER_ANDROID, REF_STAMP_ANDROID, ref)

  if (!needIos && !needAndroid) {
    // Vendor is already up to date. Back-fill the global cache if it is missing so
    // that future runs (after node_modules is cleared) can restore without rebuilding.
    if (want.ios && !existsSync(cacheDir('ios', ref))) {
      saveToCache('ios', ref, vendorIos)
    }
    if (want.android && !existsSync(cacheDir('android', ref))) {
      saveToCache('android', ref, vendorAndroid)
    }
    return
  }

  if (skip) {
    const missing = [
      needIos && `  iOS:     ${MARKER_IOS}`,
      needAndroid && `  Android: ${MARKER_ANDROID}`,
    ]
      .filter(Boolean)
      .join('\n')
    console.error(
      'react-native-cesium: Cesium Native vendor output is missing or stale, but REACT_NATIVE_CESIUM_SKIP_NATIVE_BUILD=1.\n' +
        'Build the native libraries on a machine with the required toolchains, or unset the variable and retry.\n' +
        missing +
        '\nSee the package README (Build Cesium Native locally).'
    )
    process.exit(1)
  }

  if (needIos && process.platform !== 'darwin') {
    console.error(
      'react-native-cesium: iOS Cesium Native output is missing or stale; building it requires macOS with Xcode.\n' +
        `Expected: ${MARKER_IOS}`
    )
    process.exit(1)
  }

  // Fast path: restore from global cache if available.
  if (needIos && tryRestoreFromCache('ios', ref, vendorIos, REF_STAMP_IOS, MARKER_IOS)) {
    needIos = false
  }
  if (
    needAndroid &&
    tryRestoreFromCache('android', ref, vendorAndroid, REF_STAMP_ANDROID, MARKER_ANDROID)
  ) {
    needAndroid = false
  }

  if (!needIos && !needAndroid) return

  // Clone cesium-native source if not already present.
  if (!existsSync(join(root, 'vendor', 'cesium-native'))) {
    console.log('react-native-cesium: running package update (clone Cesium Native / vcpkg prep)…')
    runScript('update', {})
  }

  if (needIos) {
    console.log('react-native-cesium: building Cesium Native for iOS (this can take a long time)…')
    runScript('build', { CESIUM_BUILD_ONLY: 'ios' })
    writeFileSync(REF_STAMP_IOS, ref)
    saveToCache('ios', ref, vendorIos)
  }

  if (needAndroid) {
    console.log(
      'react-native-cesium: building Cesium Native for Android (this can take a long time)…'
    )
    runScript('build', { CESIUM_BUILD_ONLY: 'android' })
    writeFileSync(REF_STAMP_ANDROID, ref)
    saveToCache('android', ref, vendorAndroid)
  }

  if (needIos && !existsSync(MARKER_IOS)) {
    console.error(`react-native-cesium: iOS build finished but marker missing: ${MARKER_IOS}`)
    process.exit(1)
  }
  if (needAndroid && !existsSync(MARKER_ANDROID)) {
    console.error(
      `react-native-cesium: Android build finished but marker missing: ${MARKER_ANDROID}`
    )
    process.exit(1)
  }
}

main()
