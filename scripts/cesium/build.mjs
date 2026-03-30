#!/usr/bin/env node
/**
 * Release build of Cesium Native for Android (arm64-android) and iOS (arm64-ios + arm64-ios-simulator),
 * then stage under vendor/android and vendor/ios (XCFramework).
 *
 * Prerequisites: CMake 3.15+ (Ninja recommended; otherwise Unix Makefiles on macOS/Linux), git (to auto-clone vcpkg), vcpkg (see resolveVcpkgRoot), Xcode (iOS). Android NDK: ANDROID_NDK_HOME or auto-detect from ANDROID_HOME / ANDROID_SDK_ROOT / ~/Library/Android/sdk/ndk.
 * See README "Maintainers: Cesium Native prebuilts".
 */
import { execFileSync, spawnSync } from 'node:child_process'
import {
  cpSync,
  existsSync,
  mkdirSync,
  mkdtempSync,
  readFileSync,
  readdirSync,
  rmSync,
  writeFileSync,
} from 'node:fs'
import os from 'node:os'
import { basename, dirname, join } from 'node:path'
import { fileURLToPath } from 'node:url'

const __dirname = dirname(fileURLToPath(import.meta.url))
const root = join(__dirname, '..', '..')
const cesiumSrc = join(root, 'vendor', 'cesium-native')
const vendorAndroid = join(root, 'vendor', 'android')
const vendorIos = join(root, 'vendor', 'ios')
const staging = join(root, 'vendor', '.cesium-staging')
const stagingDevice = join(staging, 'ios-device')
const stagingSim = join(staging, 'ios-sim')

const VENDOR_ANDROID_README = `# Android Cesium Native prebuilts

This directory is populated by maintainers when running \`yarn run build\` (after
\`yarn run update\`). It contains a CMake install prefix plus merged
\`vcpkg_installed/arm64-android\` content so \`find_package(cesium-native)\` works
without a local vcpkg checkout.

Publish this tree with the npm package so consumers do not compile Cesium
Native themselves.
`

const VENDOR_IOS_README = `# iOS Cesium Native prebuilts

This directory is populated by maintainers when running \`yarn run build\` on
macOS (after \`yarn run update\`). It contains \`CesiumNative.xcframework\` (device
\`arm64-ios\` and simulator \`arm64-ios-simulator\` slices) produced from merged
static libraries and shared headers.

Publish \`CesiumNative.xcframework\` with the npm package so CocoaPods can link
without building Cesium Native locally.
`

function run(cmd, args, opts = {}) {
  const r = spawnSync(cmd, args, { stdio: 'inherit', ...opts })
  if (r.status !== 0) {
    process.exit(r.status ?? 1)
  }
}

function hostTriplet() {
  return process.arch === 'arm64' ? 'arm64-osx' : 'x64-osx'
}

function which(bin) {
  try {
    execFileSync('which', [bin], { stdio: 'pipe' })
    return true
  } catch {
    return false
  }
}

/** Prefer Ninja; fall back to Makefiles so only CMake is required on PATH. */
let cmakeGenerator = 'Ninja'

function resolveCmakeGenerator() {
  if (which('ninja')) {
    cmakeGenerator = 'Ninja'
    return
  }
  if (process.platform === 'darwin' || process.platform === 'linux') {
    cmakeGenerator = 'Unix Makefiles'
    console.warn(
      'ninja not found on PATH; using Unix Makefiles. For faster builds install Ninja (e.g. brew install ninja, or apt install ninja-build).'
    )
    return
  }
  console.error(
    'ninja must be on PATH for CMake on this OS. Install Ninja: https://ninja-build.org/'
  )
  process.exit(1)
}

function isValidVcpkgRoot(dir) {
  return existsSync(join(dir, 'scripts', 'buildsystems', 'vcpkg.cmake'))
}

/** Normalize a path from .vcpkg-root: ~ expansion, or relative to repo root. */
function normalizeVcpkgCandidate(raw) {
  const v = raw.trim()
  if (!v || v.startsWith('#')) return null
  if (v.startsWith('~')) {
    return join(os.homedir(), v.slice(1).replace(/^\//, ''))
  }
  if (v.startsWith('/') || /^[A-Za-z]:[\\/]/.test(v)) {
    return v
  }
  return join(root, v)
}

/**
 * Sets process.env.VCPKG_ROOT. Order: env var, .vcpkg-root file, ~/vcpkg, vendor/vcpkg,
 * else clone+bootstrap into vendor/vcpkg (unless CESIUM_SKIP_AUTO_VCPKG=1).
 */
function resolveVcpkgRoot() {
  const env = process.env.VCPKG_ROOT?.trim()
  if (env) {
    if (isValidVcpkgRoot(env)) {
      process.env.VCPKG_ROOT = env
      return
    }
    console.error(
      `VCPKG_ROOT="${env}" does not contain scripts/buildsystems/vcpkg.cmake.\n` +
        'Clone and bootstrap vcpkg: https://github.com/microsoft/vcpkg'
    )
    process.exit(1)
  }

  const dotfile = join(root, '.vcpkg-root')
  if (existsSync(dotfile)) {
    const line = readFileSync(dotfile, 'utf8')
      .split(/\r?\n/)
      .map((l) => l.trim())
      .find((l) => l && !l.startsWith('#'))
    if (line) {
      const p = normalizeVcpkgCandidate(line)
      if (p && isValidVcpkgRoot(p)) {
        process.env.VCPKG_ROOT = p
        console.warn(`Using VCPKG_ROOT from .vcpkg-root: ${p}`)
        return
      }
    }
  }

  const homeVcpkg = join(os.homedir(), 'vcpkg')
  if (isValidVcpkgRoot(homeVcpkg)) {
    process.env.VCPKG_ROOT = homeVcpkg
    console.warn(
      `Using VCPKG_ROOT=${homeVcpkg} (auto-detected). Set VCPKG_ROOT or add .vcpkg-root to override.`
    )
    return
  }

  const vendorVcpkg = join(root, 'vendor', 'vcpkg')
  if (isValidVcpkgRoot(vendorVcpkg)) {
    process.env.VCPKG_ROOT = vendorVcpkg
    console.warn(`Using VCPKG_ROOT=${vendorVcpkg} (auto-detected).`)
    return
  }

  if (process.env.CESIUM_SKIP_AUTO_VCPKG === '1') {
    printVcpkgMissingHelp()
    process.exit(1)
  }

  cloneAndBootstrapVendorVcpkg(vendorVcpkg)
  process.env.VCPKG_ROOT = vendorVcpkg
}

function printVcpkgMissingHelp() {
  console.error(`No vcpkg installation found.

  Set one of:
    export VCPKG_ROOT=/path/to/vcpkg
  Or create a gitignored file in the repo root (first line = path):
    echo /path/to/vcpkg > .vcpkg-root

  Or remove CESIUM_SKIP_AUTO_VCPKG to let the build clone vcpkg into vendor/vcpkg.

  Manual install (from Cesium Native developer setup):
    git clone https://github.com/microsoft/vcpkg.git "$HOME/vcpkg"
    cd "$HOME/vcpkg" && ./bootstrap-vcpkg.sh

  https://github.com/CesiumGS/cesium-native/blob/main/doc/topics/developer-setup.md
`)
}

/**
 * Clone microsoft/vcpkg into vendor/vcpkg and run bootstrap (gitignored, not committed).
 */
function cloneAndBootstrapVendorVcpkg(vendorVcpkg) {
  if (!which('git')) {
    console.error('git is required on PATH to clone vcpkg, or set VCPKG_ROOT to an existing install.')
    printVcpkgMissingHelp()
    process.exit(1)
  }

  console.warn(
    'Cloning and bootstrapping vcpkg into vendor/vcpkg (first run; large download, several minutes)...'
  )
  mkdirSync(join(root, 'vendor'), { recursive: true })

  if (existsSync(vendorVcpkg)) {
    rmSync(vendorVcpkg, { recursive: true, force: true })
  }

  run('git', [
    'clone',
    '--depth',
    '1',
    'https://github.com/microsoft/vcpkg.git',
    vendorVcpkg,
  ])

  if (process.platform === 'win32') {
    run('cmd', ['/c', 'bootstrap-vcpkg.bat'], { cwd: vendorVcpkg, env: process.env })
  } else {
    run('sh', ['bootstrap-vcpkg.sh'], { cwd: vendorVcpkg, env: process.env })
  }

  if (!isValidVcpkgRoot(vendorVcpkg)) {
    console.error(
      'bootstrap-vcpkg did not produce scripts/buildsystems/vcpkg.cmake. See output above.'
    )
    process.exit(1)
  }

  console.warn(`VCPKG_ROOT=${vendorVcpkg} (vendor clone + bootstrap complete)`)
}

function mergeStaticLibs(libDir, outFile) {
  const libs = []
  function walk(d) {
    for (const name of readdirSync(d, { withFileTypes: true })) {
      const p = join(d, name.name)
      if (name.isDirectory()) walk(p)
      else if (name.name.endsWith('.a')) libs.push(p)
    }
  }
  walk(libDir)
  if (libs.length === 0) {
    throw new Error(`No .a files under ${libDir}`)
  }
  run('libtool', ['-static', '-o', outFile, ...libs])
}

function rsyncMerge(src, dst) {
  if (!existsSync(src)) {
    throw new Error(`Expected path missing: ${src}`)
  }
  cpSync(src, dst, { recursive: true })
}

function cmakeConfigure(buildDir, extraArgs) {
  const args = [
    '-S',
    cesiumSrc,
    '-B',
    buildDir,
    '-G',
    cmakeGenerator,
    '-DCMAKE_BUILD_TYPE=Release',
    `-DCMAKE_TOOLCHAIN_FILE=${join(process.env.VCPKG_ROOT, 'scripts', 'buildsystems', 'vcpkg.cmake')}`,
    '-DCESIUM_USE_EZVCPKG=OFF',
    '-DCESIUM_TESTS_ENABLED=OFF',
    '-DCESIUM_ENABLE_CLANG_TIDY=OFF',
    `-DVCPKG_HOST_TRIPLET=${hostTriplet()}`,
    ...extraArgs,
  ]
  run('cmake', args, { cwd: root, env: process.env })
}

function cmakeBuildInstall(buildDir, installPrefix) {
  const jobs =
    process.env.CESIUM_BUILD_JOBS && /^\d+$/.test(process.env.CESIUM_BUILD_JOBS)
      ? process.env.CESIUM_BUILD_JOBS
      : String(Math.max(1, os.cpus().length))
  run('cmake', ['--build', buildDir, '--parallel', jobs], { cwd: root, env: process.env })
  rmSync(installPrefix, { recursive: true, force: true })
  mkdirSync(installPrefix, { recursive: true })
  run('cmake', ['--install', buildDir, '--prefix', installPrefix], { cwd: root, env: process.env })
}

function mergeVcpkgInstalled(buildDir, triplet, installPrefix) {
  const vp = join(buildDir, 'vcpkg_installed', triplet)
  if (!existsSync(vp)) {
    throw new Error(`vcpkg_installed not found: ${vp}`)
  }
  rsyncMerge(vp, installPrefix)
}

function assertFile(p) {
  if (!existsSync(p)) {
    throw new Error(`Expected file missing: ${p}`)
  }
}

/**
 * Ensure Android static libs contain ELF objects (not Mach-O from a mistaken iOS/host build).
 */
function firstObjectFileUnder(dir) {
  const stack = [dir]
  while (stack.length) {
    const d = stack.pop()
    for (const e of readdirSync(d, { withFileTypes: true })) {
      const p = join(d, e.name)
      if (e.isDirectory()) stack.push(p)
      else if (e.name.endsWith('.o')) return p
    }
  }
  return null
}

function assertAndroidPrebuiltsAreElf() {
  const lib = join(vendorAndroid, 'lib', 'libCesiumUtility.a')
  if (!existsSync(lib)) return
  const arBin = resolveAndroidLlvmAr()
  if (!arBin) {
    throw new Error(
      'Cannot verify Android prebuilts: no llvm-ar under ANDROID_NDK_HOME (use NDK llvm-ar, not system ar).'
    )
  }
  const tmp = mkdtempSync(join(os.tmpdir(), 'rn-cesium-android-elf-'))
  try {
    const listOut = execFileSync(arBin, ['t', lib], { encoding: 'utf8' })
    const member = listOut
      .split(/\r?\n/)
      .map((l) => l.trim())
      .find((l) => l.endsWith('.o'))
    if (!member) return
    execFileSync(arBin, ['x', lib, member], { cwd: tmp })
    const target = firstObjectFileUnder(tmp)
    if (!target) return
    const out = execFileSync('file', [target], { encoding: 'utf8' })
    if (out.includes('Mach-O')) {
      throw new Error(
        'Android prebuilts in vendor/android contain Mach-O objects (Apple), not ELF. ' +
          'Remove vendor/android and run CESIUM_BUILD_ONLY=android yarn build with a proper Android NDK (arm64-android).'
      )
    }
    if (!out.includes('ELF')) {
      console.warn(`Unexpected object type from ${lib} (first member): ${out.trim()}`)
    }
  } finally {
    rmSync(tmp, { recursive: true, force: true })
  }
}

function isValidNdkRoot(dir) {
  return existsSync(join(dir, 'build', 'cmake', 'android.toolchain.cmake'))
}

/** Prefer newer NDK folders (e.g. 27.1.12297006). */
function compareNdkVersionDesc(a, b) {
  const pa = a.split('.').map((x) => parseInt(x, 10) || 0)
  const pb = b.split('.').map((x) => parseInt(x, 10) || 0)
  const n = Math.max(pa.length, pb.length)
  for (let i = 0; i < n; i++) {
    const da = pa[i] ?? 0
    const db = pb[i] ?? 0
    if (da !== db) return db - da
  }
  return 0
}

function pickLatestNdkUnder(ndkParentDir) {
  let dirs = []
  try {
    dirs = readdirSync(ndkParentDir, { withFileTypes: true })
      .filter((d) => d.isDirectory())
      .map((d) => join(ndkParentDir, d.name))
  } catch {
    return null
  }
  const valid = dirs.filter((d) => isValidNdkRoot(d))
  if (valid.length === 0) return null
  valid.sort((x, y) => compareNdkVersionDesc(basename(x), basename(y)))
  return valid[0]
}

/**
 * Sets process.env.ANDROID_NDK_HOME. Uses env var, or scans SDK ndk/ for versioned folders.
 */
function resolveAndroidNdkHome() {
  const env = process.env.ANDROID_NDK_HOME?.trim()
  if (env) {
    if (isValidNdkRoot(env)) {
      process.env.ANDROID_NDK_HOME = env
      return env
    }
    console.error(
      `ANDROID_NDK_HOME="${env}" is not a valid NDK root (missing build/cmake/android.toolchain.cmake).`
    )
    process.exit(1)
  }

  const sdkCandidates = [
    process.env.ANDROID_HOME?.trim(),
    process.env.ANDROID_SDK_ROOT?.trim(),
    join(os.homedir(), 'Library', 'Android', 'sdk'),
    join(os.homedir(), 'Android', 'Sdk'),
  ].filter(Boolean)

  for (const sdk of sdkCandidates) {
    if (!existsSync(sdk)) continue
    const ndkParent = join(sdk, 'ndk')
    const picked = pickLatestNdkUnder(ndkParent)
    if (picked) {
      process.env.ANDROID_NDK_HOME = picked
      console.warn(
        `Using ANDROID_NDK_HOME=${picked} (auto-detected). Override with ANDROID_NDK_HOME if needed.`
      )
      return picked
    }
  }

  console.error(`Could not find Android NDK.

  Install the NDK (Android Studio → Settings → SDK → SDK Tools → NDK),
  or set:
    export ANDROID_NDK_HOME=/path/to/ndk/27.x.x

  Or build only iOS (macOS):
    CESIUM_BUILD_ONLY=ios yarn build
`)
  process.exit(1)
}

/**
 * Path to NDK llvm-ar. macOS system `ar` is BSD and cannot list/extract GNU thin archives
 * produced by the Android toolchain (see `ar t` noise like `/`, `Assert.cpp.o/`).
 */
function resolveAndroidLlvmAr() {
  const ndk = process.env.ANDROID_NDK_HOME?.trim()
  if (!ndk) return null
  const prebuilt = join(ndk, 'toolchains', 'llvm', 'prebuilt')
  if (!existsSync(prebuilt)) return null
  for (const host of readdirSync(prebuilt)) {
    const base = join(prebuilt, host)
    for (const rel of ['bin/llvm-ar', 'llvm-ar']) {
      const p = join(base, ...rel.split('/'))
      if (existsSync(p)) return p
    }
  }
  return null
}

function buildAndroid() {
  const ndk = resolveAndroidNdkHome()
  const triplet = 'arm64-android'
  const buildDir = join(cesiumSrc, 'build-android-arm64')
  process.env.ANDROID_NDK_HOME = ndk

  const ndkToolchain = join(ndk, 'build', 'cmake', 'android.toolchain.cmake')
  if (!existsSync(ndkToolchain)) {
    throw new Error(`Missing NDK CMake toolchain: ${ndkToolchain}`)
  }
  // Required so vcpkg + dependencies compile with the NDK (ELF aarch64), not host Clang (Mach-O on macOS).
  cmakeConfigure(buildDir, [
    `-DCMAKE_INSTALL_PREFIX=${vendorAndroid}`,
    `-DVCPKG_TARGET_TRIPLET=${triplet}`,
    `-DVCPKG_CHAINLOAD_TOOLCHAIN_FILE=${ndkToolchain}`,
    `-DCMAKE_ANDROID_NDK=${ndk}`,
    '-DANDROID_ABI=arm64-v8a',
    '-DANDROID_PLATFORM=android-24',
  ])
  cmakeBuildInstall(buildDir, vendorAndroid)
  mergeVcpkgInstalled(buildDir, triplet, vendorAndroid)
  assertFile(join(vendorAndroid, 'share', 'cesium-native', 'cmake', 'cesium-nativeConfig.cmake'))
  assertAndroidPrebuiltsAreElf()
  writeFileSync(join(vendorAndroid, 'README.md'), VENDOR_ANDROID_README)
  console.log(`Android prebuilts staged under ${vendorAndroid}`)
}

function buildIosDevice() {
  const triplet = 'arm64-ios'
  const buildDir = join(cesiumSrc, 'build-ios-device')
  cmakeConfigure(buildDir, [
    `-DCMAKE_INSTALL_PREFIX=${stagingDevice}`,
    `-DVCPKG_TARGET_TRIPLET=${triplet}`,
    '-DCMAKE_SYSTEM_NAME=iOS',
    '-DCMAKE_OSX_SYSROOT=iphoneos',
    '-DCMAKE_OSX_ARCHITECTURES=arm64',
    '-DCMAKE_OSX_DEPLOYMENT_TARGET=15.1',
    // Xcode 16 may inject -ffp-model=precise while cesium-native sets -ffp-contract=off.
    // Keep the diagnostic visible, but avoid failing the whole prebuilt pipeline on this override.
    '-DCMAKE_CXX_FLAGS=-Wno-error=overriding-option',
  ])
  cmakeBuildInstall(buildDir, stagingDevice)
  mergeVcpkgInstalled(buildDir, triplet, stagingDevice)
  assertFile(join(stagingDevice, 'share', 'cesium-native', 'cmake', 'cesium-nativeConfig.cmake'))
  const merged = join(stagingDevice, 'libCesiumNativePrebuilt.a')
  mergeStaticLibs(join(stagingDevice, 'lib'), merged)
  console.log(`iOS device staging + merged static lib: ${merged}`)
}

function buildIosSimulator() {
  const triplet = 'arm64-ios-simulator'
  const buildDir = join(cesiumSrc, 'build-ios-simulator')
  cmakeConfigure(buildDir, [
    `-DCMAKE_INSTALL_PREFIX=${stagingSim}`,
    `-DVCPKG_TARGET_TRIPLET=${triplet}`,
    '-DCMAKE_SYSTEM_NAME=iOS',
    '-DCMAKE_OSX_SYSROOT=iphonesimulator',
    '-DCMAKE_OSX_ARCHITECTURES=arm64',
    '-DCMAKE_OSX_DEPLOYMENT_TARGET=15.1',
    // Same warning behavior as device build to keep simulator configuration consistent.
    '-DCMAKE_CXX_FLAGS=-Wno-error=overriding-option',
  ])
  cmakeBuildInstall(buildDir, stagingSim)
  mergeVcpkgInstalled(buildDir, triplet, stagingSim)
  assertFile(join(stagingSim, 'share', 'cesium-native', 'cmake', 'cesium-nativeConfig.cmake'))
  const merged = join(stagingSim, 'libCesiumNativePrebuilt.a')
  mergeStaticLibs(join(stagingSim, 'lib'), merged)
  console.log(`iOS simulator staging + merged static lib: ${merged}`)
}

function createXcframework() {
  rmSync(vendorIos, { recursive: true, force: true })
  mkdirSync(vendorIos, { recursive: true })
  const out = join(vendorIos, 'CesiumNative.xcframework')
  rmSync(out, { recursive: true, force: true })
  const devLib = join(stagingDevice, 'libCesiumNativePrebuilt.a')
  const simLib = join(stagingSim, 'libCesiumNativePrebuilt.a')
  const headers = join(stagingDevice, 'include')
  assertFile(devLib)
  assertFile(simLib)
  if (!existsSync(headers)) {
    throw new Error(`Expected headers at ${headers}`)
  }
  run('xcodebuild', [
    '-create-xcframework',
    '-library',
    devLib,
    '-headers',
    headers,
    '-library',
    simLib,
    '-headers',
    headers,
    '-output',
    out,
  ])
  assertFile(join(out, 'Info.plist'))
  const readme = join(vendorIos, 'README.txt')
  writeFileSync(
    readme,
    'Cesium Native iOS prebuilts: CesiumNative.xcframework (device + simulator).\n' +
      'Produced by `yarn run build` from vendor/cesium-native.\n'
  )
  writeFileSync(join(vendorIos, 'README.md'), VENDOR_IOS_README)
  console.log(`XCFramework written to ${out}`)
}

function main() {
  if (!existsSync(cesiumSrc)) {
    console.error('vendor/cesium-native not found. Run: yarn run update')
    process.exit(1)
  }
  if (!which('cmake')) {
    console.error(
      'cmake must be on PATH (CMake 3.15+). Install: https://cmake.org/download/ or brew install cmake'
    )
    process.exit(1)
  }
  resolveCmakeGenerator()
  resolveVcpkgRoot()

  const only = process.env.CESIUM_BUILD_ONLY
  if (!only || only === 'android') {
    buildAndroid()
  }
  if (!only || only === 'ios') {
    if (process.platform !== 'darwin') {
      console.warn('Skipping iOS build (macOS required).')
    } else {
      if (!which('xcodebuild')) {
        console.error('xcodebuild not found.')
        process.exit(1)
      }
      buildIosDevice()
      buildIosSimulator()
      createXcframework()
    }
  }
}

main()
